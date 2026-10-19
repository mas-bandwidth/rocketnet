# Structured Errors, FFI Logging, and Runtime Counters

This document describes structured failure reporting on the public lifecycle,
client and server creation entry points, the preformatted FFI-safe logging
callback, and the runtime counter and statistics queries in
[sdk/rocketnet.h](../sdk/rocketnet.h). Read this document beside
[docs/ABI-EVOLUTION.md](ABI-EVOLUTION.md), which governs symbol versioning and
the native ABI layout queries.

## 1. Public Lifecycle, Creation, and Validation Entry Points

A program initializes the library, fills a client or server configuration
struct, creates a client or server, and checks the return value. Every entry
point that can refuse returns a value the caller can test, logs one line at
`ROCKETNET_LOG_LEVEL_ERROR` when it refuses, and increments
`rocketnet_api_misuse_count` when the refusal is an API misuse (such as a
`NULL` pointer or an uninitialized library).

| Public Entry Point | Return on Refusal |
| :--- | :--- |
| `rocketnet_init( context )` | `false` |
| `rocketnet_term( void )` | refuses in place |
| `rocketnet_allocator( malloc_fn, free_fn )` | `false` |
| `rocketnet_client_default_config( config )` | a NULL config is a counted no-op |
| `rocketnet_client_validate_config( config )` | `0`, with a log line naming the rule |
| `rocketnet_client_create( context, config, memory, memory_bytes )` | `NULL`, with one error line |
| `rocketnet_client_connect( client, token )` | the status code: `CLIENT_NOT_DISCONNECTED`, `INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, or a transport's code |
| `rocketnet_client_create_sized(...)` | `NULL` for an unknown config size |
| `rocketnet_server_default_config( config )` / `rocketnet_server_dev_config( config )` | a NULL config is a counted no-op |
| `rocketnet_server_validate_config( config )` | `0`, with a log line naming the rule |
| `rocketnet_server_create( context, address, config, memory, memory_bytes )` | `NULL`, with one error line |
| `rocketnet_server_create_sized(...)` | `NULL` for an unknown config size |
| `rocketnet_server_start( server )` | the status code: `SERVER_NOT_STOPPED`, or the transport's code (`TRANSPORT_LADDER_EXHAUSTED`, `MISSING_KEYS`, ...) |
| `rocketnet_client_update( client, time )` / `rocketnet_server_update( server, time )` | `ROCKETNET_MESSAGE_OK` or `ROCKETNET_ERROR_MESSAGE_NOT_RELEASED` |

The client and server create paths validate the config, apply the environment
overrides (`ROCKETNET_CLIENT_*`, `ROCKETNET_SERVER_*`, with precedence
DEFAULT < CONFIG STRUCT < ENVIRONMENT), reserve the caller memory block or make
the one allocation, and start the selected transport. The detailed transport
create failure branches belong to the internal transport layer
(`sdk/rocketnet_layers.h`); a game sees only the public create result, the log
line and the misuse count.

The helper names used later in this document (`rocketnet_error`,
`rocketnet_assert`, `rocketnet_log_v`, `rocketnet_config_apply_env`,
`rocketnet_config_validate`, `rocketnet_driver_ladder_parse`,
`rocketnet_reassembly_memory_allowed`, `rocketnet_address_parse`,
`rocketnet_server_config_has_keys`, `rocketnet_client_protocol_connect`,
`rocketnet_platform_socket_create`, `rocketnet_platform_socket_create_with_pump`,
`rocketnet_platform_thread_create`, `rocketnet_platform_mutex_create`,
`rocketnet_free`, `rocketnet_frame_sync_create`, `rocketnet_frame_sync_create_ex`,
`rocketnet_num_counters`, `rocketnet_counter_name`, `rocketnet_counter_value`
and `rocketnet_last_error`) are internal to the SDK
(`sdk/rocketnet_layers.h`, `sdk/rocketnet_internal.h`); a game does not call
them.

## 2. STATUS CODES

### 2.1 Stable Status Code Enum (`rocketnet_status_t`)

1. `ROCKETNET_STATUS_OK = 0` is the sole success value.
2. Every failure code is a positive integer in `1 .. ROCKETNET_NUM_STATUS_CODES - 1`.
3. **Append-only stability**: Once a code is published in a release, its integer
   value and meaning are permanent. New codes append immediately before
   `ROCKETNET_NUM_STATUS_CODES`.
4. **Runtime introspection**:
   - `int rocketnet_num_status_codes( void )` returns `ROCKETNET_NUM_STATUS_CODES` (allocation-free and safe before init).
   - `const char * rocketnet_status_string( int status )` returns the static ASCII
     name without the `ROCKETNET_STATUS_` prefix (for example,
     `"INVALID_CONNECT_TOKEN"`) for `0 <= status < ROCKETNET_NUM_STATUS_CODES`,
     and `NULL` (without asserting) for out-of-range values.

```c
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,

    // lifecycle and allocator state (rocketnet_init_ex, rocketnet_term_ex, rocketnet_allocator_ex)
    ROCKETNET_STATUS_ALREADY_INITIALIZED                = 1,
    ROCKETNET_STATUS_NOT_INITIALIZED                    = 2,
    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = 3,
    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = 4,
    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = 5,

    // caller arguments and configuration (client, server, message, connect tokens)
    ROCKETNET_STATUS_INVALID_ARGUMENT                   = 6,
    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = 7,
    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = 8,
    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = 9,
    ROCKETNET_STATUS_MISSING_KEYS                       = 10,
    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = 11,
    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = 12,
    ROCKETNET_STATUS_INVALID_ADDRESS                    = 13,
    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = 14,

    // client connect token pre-send checks
    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = 15,
    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = 16,

    // transport candidate list and OS resources
    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = 17,
    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = 18,
    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,
    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,
    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,
    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,

    ROCKETNET_NUM_STATUS_CODES                          = 23
};
```

## 3. Logging

### 3.1 Levels and Filtering

Log levels run from `ROCKETNET_LOG_LEVEL_NONE` (`0`) through
`ROCKETNET_LOG_LEVEL_ERROR` (`1`), `ROCKETNET_LOG_LEVEL_WARN` (`2`),
`ROCKETNET_LOG_LEVEL_INFO` (`3`), `ROCKETNET_LOG_LEVEL_DEBUG` (`4`), and
`ROCKETNET_LOG_LEVEL_SPAM` (`5`).

- `rocketnet_log_level( level )` sets the process-wide maximum log level;
  `rocketnet_get_log_level()` reads it back, and `rocketnet_log_level_string( level )`
  returns `"error"`, `"warning"`, `"info"`, `"debug"`, `"spam"`, or `"???"`.
- `rocketnet_quiet( flag )` suppresses (`true`) or resumes (`false`) all log
  output without altering the configured log level.
- Both `quiet` and `log_level` are checked atomically before formatting, so
  filtered-out messages pay zero `vsnprintf` cost.

### 3.2 Preformatted Callback Signature and Contract

```c
typedef void (*rocketnet_log_callback_t)(
    void * context, int level, const char * message, size_t length );

ROCKETNET_EXPORT void rocketnet_set_log_callback(
    rocketnet_log_callback_t callback, void * context );
```

1. **Parameters**:
   - `context`: caller-supplied pointer passed to `rocketnet_set_log_callback` (may be `NULL`).
   - `level`: `ROCKETNET_LOG_LEVEL_NONE` (`0`) through `ROCKETNET_LOG_LEVEL_SPAM` (`5`).
   - `message`: non-NULL pointer to a formatted UTF-8 / ASCII stack buffer.
     `message[length]` is guaranteed to be `'\0'`, with no trailing newline.
   - `length`: exact byte length in `[0, 1023]`, excluding the terminating `'\0'`.
2. **Lifetime and threading**:
   - `message` borrows a stack buffer in the logging frame and is valid only
     until the callback returns. Copy `[message, message + length)` if the text
     is retained after return.
   - Callbacks may run on the main thread, on a transport worker thread, or on
     the client I/O thread; they must be thread-safe and must not call
     `rocketnet_set_log_callback` or `rocketnet_log_function` re-entrantly.
3. **Coexistence with `rocketnet_log_function`**:
   - `rocketnet_log_function( void (*function)( int level, const char * format, ... ) )`
     remains available for C callers that prefer a `printf`-style signature.
     The last registration across `rocketnet_set_log_callback` and
     `rocketnet_log_function` wins; passing `NULL` restores the default stdout
     sink.

## 4. Public Counters and Statistics

Applications read monotonic counters and trailing-second connection quality
metrics through the typed enum queries in [sdk/rocketnet.h](../sdk/rocketnet.h):

- Client: `rocketnet_client_counter( client, enum rocketnet_client_counter_t )`
  and `rocketnet_client_stat( client, enum rocketnet_client_stat_t )`.
- Server aggregate: `rocketnet_server_counter( server, enum rocketnet_server_counter_t )`
  and `rocketnet_server_stat( server, enum rocketnet_server_stat_t )`.
- Server per-client slot: `rocketnet_server_client_counter( server, client_index, enum rocketnet_server_client_counter_t )`
  and `rocketnet_server_client_stat( server, client_index, enum rocketnet_server_client_stat_t )`.

Every enum is append-only before its `*_COUNT` sentinel (see
[docs/ABI-EVOLUTION.md](ABI-EVOLUTION.md) section 8), and out-of-range enum
values return `0` without asserting.

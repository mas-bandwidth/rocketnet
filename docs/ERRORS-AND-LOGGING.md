# Structured Errors, FFI Logging, and Runtime Counters

This document describes structured failure reporting on the public lifecycle,
client and server creation entry points, the preformatted FFI-safe logging
callback, and the runtime counter inspection contract for C SDK consumers.
`sdk/rocketnet.h` is the authority for every public name, field, default and
environment variable in this document.

Read this document beside `docs/ABI-EVOLUTION.md`, which governs
shared-library symbol versioning, header installation boundaries, and the
native ABI queries and version contract (`sdk/rocketnet_abi.cpp`).

---

## 1. THE STATE

### 1.1 Public Lifecycle, Creation, and Validation Entry Points

The public header is `sdk/rocketnet.h`. A game initializes the library, fills a
client or server config, creates one client or server, and checks the result.
Every entry point that can refuse returns a value a caller can test, logs one
line at `ERROR` when it refuses, and counts the refusal in
`rocketnet_api_misuse_count`. A message send on a connection that is not
CONNECTED is no refusal of this kind: it is a normal drop that logs nothing,
returns `NULL` from start message and `0` from finish message, and counts in
`ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS` or
`ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS`, never in the misuse count.

| Public Entry Point | Return on Refusal |
| :--- | :--- |
| `rocketnet_init( context )` | the status code: `ALREADY_INITIALIZED`, `PLATFORM_INIT_FAILED` or `CRYPTO_INIT_FAILED` |
| `rocketnet_term( void )` | the status code: `NOT_INITIALIZED` or `LIVE_ALLOCATIONS`; nothing changes |
| `rocketnet_set_allocator( malloc_fn, free_fn )` | the status code: `INVALID_ARGUMENT`, `ALREADY_INITIALIZED` or `LIVE_ALLOCATIONS` |
| `rocketnet_client_default_config( config )` / `rocketnet_client_default_config_sized( config, config_bytes )` | `INVALID_ARGUMENT` for a NULL config, counted, or an unknown size |
| `rocketnet_client_validate_config( config )` / `rocketnet_client_validate_config_sized( config, config_bytes )` | `CONFIG_OUT_OF_RANGE`, with a log line naming the rule; `INVALID_ARGUMENT` for an unknown size |
| `rocketnet_client_create( context, config, memory, memory_bytes, status )` / `rocketnet_client_create_sized( context, config, config_bytes, memory, memory_bytes, status )` | `NULL` with one error line; `status` holds the refusal's code (`NOT_INITIALIZED`, `INVALID_ARGUMENT`, `CONFIG_ENV_INVALID`, `CONFIG_OUT_OF_RANGE`, `INVALID_TRANSPORT_LIST`, `MEMORY_BUDGET_EXCEEDED`, `OUT_OF_MEMORY`), or `OK` with the handle (`sdk/rocketnet.h:1081`, `sdk/rocketnet.h:1086`; `sdk/rocketnet_api.cpp:561`) |
| `rocketnet_client_connect( client, token )` | the status code: `CLIENT_NOT_DISCONNECTED`, `INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, or a transport's code |
| `rocketnet_server_default_config( config )` / `rocketnet_server_default_config_sized( config, config_bytes )`, `rocketnet_server_dev_config( config )` / `rocketnet_server_dev_config_sized( config, config_bytes )` | `INVALID_ARGUMENT` for a NULL config, counted, or an unknown size |
| `rocketnet_server_validate_config( config )` / `rocketnet_server_validate_config_sized( config, config_bytes )` | `CONFIG_OUT_OF_RANGE`, with a log line naming the rule; `INVALID_ARGUMENT` for an unknown size |
| `rocketnet_server_create( context, address, config, memory, memory_bytes, status )` / `rocketnet_server_create_sized( context, address, config, config_bytes, memory, memory_bytes, status )` | `NULL` with one error line; `status` holds the refusal's code (the client's codes, plus `INVALID_ADDRESS` for an over-long address), or `OK` with the handle (`sdk/rocketnet.h:1272`, `sdk/rocketnet.h:1277`; `sdk/rocketnet_api.cpp:1302`) |
| `rocketnet_server_start( server )` | the status code: `SERVER_NOT_STOPPED`, or the transport's code (`TRANSPORT_LADDER_EXHAUSTED`, `MISSING_KEYS`, ...) |
| `rocketnet_client_update( client, time )` / `rocketnet_server_update( server, time )` | `ROCKETNET_UPDATE_OK` or `ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED` |
| key and token writers, `*_set_send_kbps`, the sized config calls | the status code; see each declaration in `sdk/rocketnet.h` |

The client and server create paths validate the config, apply the environment
overrides (`ROCKETNET_CLIENT_*`, `ROCKETNET_SERVER_*`, with precedence
DEFAULT < CONFIG STRUCT < ENVIRONMENT), reserve the caller memory block or make
the one allocation, and start the selected transport. The detailed transport
create failure branches belong to the internal transport layer
(`sdk/rocketnet_layers.h`); a game sees only the public create result, the log
line and the misuse count.

The helper names used later in this document (`rocketnet_error`,
`rocketnet_assert`, `rocketnet_log_v`, `rocketnet_config_apply_env`,
`rocketnet_config_validate`, `rocketnet_transport_ladder_parse`,
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

Status codes follow the same numbering discipline as `ROCKETNET_MESSAGE_VIOLATION_*`
(`sdk/rocketnet_message.h:85-98`) and `ROCKETNET_COUNTER_*`
(`sdk/rocketnet_counters.h:37-350`):

1. `ROCKETNET_STATUS_OK = 0` is the sole success value.
2. Every failure code is a positive integer in `1 .. ROCKETNET_STATUS_COUNT - 1`.
3. **Append-only stability law**: Once a code is published in a release, its
   integer value and meaning are permanent. New codes append immediately before
`ROCKETNET_STATUS_COUNT`. Reserved codes keep their numeric positions in the enum
   and in `rocketnet_status_name` and are never renumbered or reassigned.
4. Runtime introspection mirrors counters (`sdk/rocketnet.h:1218-1221`):
   - `int rocketnet_num_status_codes(void)` returns `ROCKETNET_STATUS_COUNT`.
   - `const char * rocketnet_status_name(enum rocketnet_status_t status)` returns a static ASCII
     identifier (for example, `"INVALID_CONNECT_TOKEN"`) for `0 <= status < ROCKETNET_STATUS_COUNT`,
     and `"unknown"` (never `NULL`, without asserting) for any other value.

```c
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,

    // lifecycle and allocator state (rocketnet_init, rocketnet_term, rocketnet_set_allocator)
    ROCKETNET_STATUS_ALREADY_INITIALIZED                = 1,
    ROCKETNET_STATUS_NOT_INITIALIZED                    = 2,
    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = 3,
    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = 4,
    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = 5,

    // caller arguments and configuration (client, server, frame_sync, message)
    ROCKETNET_STATUS_INVALID_ARGUMENT                   = 6,
    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = 7,
    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = 8,
    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = 9,
    ROCKETNET_STATUS_MISSING_KEYS                       = 10,
    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = 11,
    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = 12,
    ROCKETNET_STATUS_INVALID_ADDRESS                    = 13,
    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST                = 14,

    // client connect token pre-send checks (rocketnet_client_protocol_connect)
    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = 15,
    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = 16,

    // transport ladder and OS resources
    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED       = 17,
    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED            = 18,
    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,
    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,
    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,
    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,

    ROCKETNET_STATUS_COUNT                              = 23
};
```

Every public call that can fail returns `enum rocketnet_status_t`:
`rocketnet_init` reports `ALREADY_INITIALIZED`, `PLATFORM_INIT_FAILED` or
`CRYPTO_INIT_FAILED`; `rocketnet_term` reports `NOT_INITIALIZED` or
`LIVE_ALLOCATIONS`; `rocketnet_set_allocator` reports `INVALID_ARGUMENT`,
`ALREADY_INITIALIZED` or `LIVE_ALLOCATIONS`. The client and server create
paths return a handle or `NULL` and log one `ERROR` line with the reason,
including `CONFIG_ENV_INVALID`, `CONFIG_OUT_OF_RANGE`,
`MEMORY_BUDGET_EXCEEDED`, `INVALID_TRANSPORT_LIST`, `OUT_OF_MEMORY`,
`INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, `TRANSPORT_LADDER_EXHAUSTED`, `TRANSPORT_FAMILIES_UNSUPPORTED`,
`MISSING_KEYS`, `INVALID_PROTOCOL_ID`, `INVALID_ADDRESS`,
`TRANSPORT_PORT_ZERO_UNSUPPORTED`, `MUTEX_CREATE_FAILED`,
`THREAD_CREATE_FAILED` and `TIMER_CREATE_FAILED`.
`ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED` is 26. Its string is
`TRANSPORT_FAMILIES_UNSUPPORTED` (`rocketnet_status_names` in `sdk/rocketnet.cpp`). It is appended
immediately before `ROCKETNET_STATUS_COUNT`. A transport returns it at create
when it cannot serve both named families, and start returns it when every
candidate that opened refused for that reason.

### 2.2 Reporting

Init, term and the allocator return the section-2.1 code directly (the
SDK's internal `_ex` forms write the same code to an `int *`). The client and server create
calls return a handle or `NULL` and write the refusal's code through the final
status out-parameter (sdk/rocketnet.h, the create declarations); the reason is
also one `ERROR` log line, and the refusal is counted in
`rocketnet_api_misuse_count`. `rocketnet_status_name`
names any code, and `rocketnet_num_status_codes` gives the count. The internal
transport layer keeps `_ex` create forms for the SDK's own tests.

## 3. LOGGING

### 3.1 One Log Sink

The library has one log sink, installed with `rocketnet_set_log_callback`.
Every log line is formatted once into a 1024-byte stack buffer and handed to
the installed callback, or to the default sink (stdout) when none is
installed. The level is `enum rocketnet_log_level_t`, set with
`rocketnet_set_log_level` and read with `rocketnet_get_log_level`;
`rocketnet_log_level_name` names a level by its enum (`"none"`, `"error"`,
`"warn"`, `"info"`, `"debug"`, `"spam"`), and `"unknown"` for a value outside
the enum. `rocketnet_set_quiet( true )` suppresses every line and
`rocketnet_set_quiet( false )` resumes them, without changing the level.
`log_quiet` and `log_level` are relaxed atomics read before any formatting.

### 3.2 Callback Signature and Contract

```c
typedef void (*rocketnet_log_callback_t)(
    void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

ROCKETNET_EXPORT void rocketnet_set_log_callback(
    rocketnet_log_callback_t callback, void * context );
```

1. **Parameters**:
   - `context`: opaque pointer passed to `rocketnet_set_log_callback` (may be `NULL`).
   - `level`: `ROCKETNET_LOG_LEVEL_ERROR` (`1`) .. `ROCKETNET_LOG_LEVEL_SPAM` (`5`),
     or `ROCKETNET_LOG_LEVEL_NONE` (`0`) for `rocketnet_printf`.
   - `message`: non-NULL pointer to a formatted UTF-8 / ASCII stack buffer
     (`char buffer[1024]`). `message[length]` is guaranteed to be `'\0'`, with
     no trailing newline added by the SDK.
   - `length`: exact byte length in `[0, 1023]`, excluding the terminating
     `'\0'`. When `vsnprintf` truncates an overlong message (`n >= 1024`),
     `length` is clamped to `1023` (`sizeof(buffer) - 1`).
2. **Lifetime and threading contract**:
   - `message` borrows a stack buffer in the logging frame and is valid **only
     for the synchronous duration of the callback call**. Callbacks that retain
     messages past return must copy `[message, message + length)`.
   - The callback may be invoked on the main thread, on any game fill thread
     between `update` and `send_packets`, or on a transport receive
     thread. Callbacks must be thread-safe, must not call
     `rocketnet_set_log_callback` re-entrantly, and must not unwind via C++
     exceptions or `longjmp`.
   - The `(callback, context)` pair is guarded by
     `rocketnet_platform_spin_lock_u64` so it is never observed torn.

### 3.3 Dispatch

`rocketnet_log_v( int level, const char * format, va_list args )` in
`sdk/rocketnet.cpp`:

1. **Filter before formatting**: If `log_quiet.load(std::memory_order_relaxed)`
   is true or (for `level != ROCKETNET_LOG_LEVEL_NONE`)
   `level > log_level.load(std::memory_order_relaxed)`, return before `vsnprintf`.
2. **Format once**: Run `vsnprintf( buffer, sizeof( buffer ), format, args )`
   once and compute clamped `size_t length`.
3. **Dispatch**: invoke the installed `callback(context, level, buffer, length)`,
   or the default sink, which writes the line to stdout with no second
   `vsnprintf`.
4. **Last registration wins**: a non-NULL callback replaces the active sink;
   passing `NULL` restores the default sink.

### 3.4 Refused Thread Priority Calls

The library's network-thread priority calls (`docs/CLIENT-IO.md`, Thread
priority; `TRANSPORTS.md`, Server network threads) check each result. A refused
call (`pthread_setschedparam` nonzero, `pthread_setaffinity_np` nonzero on the
XDP thread, `SetThreadPriority` FALSE, `timeBeginPeriod` or `timeEndPeriod`
not `TIMERR_NOERROR`) adds one to a process-wide count (`sdk/rocketnet_platform.cpp:118`), read with
`rocketnet_get_thread_priority_refusals()`, and never fails a create or
`rocketnet_init`. The first refusal of each call kind logs one warning, and
that kind never logs again in the process:

```
thread priority: pthread_setschedparam SCHED_RR 1 refused (Operation not permitted); the thread runs at its default priority
```

The other kinds read `thread priority: SetThreadPriority THREAD_PRIORITY_TIME_CRITICAL refused (error <n>); ...`, `thread affinity: pthread_setaffinity_np cpu <n> refused (...); the thread runs on any cpu`,
`timer period: timeBeginPeriod 1 refused (error <n>); the timer keeps its default period` and `timer period: timeEndPeriod 1 refused (...)`. The count
is updated with a relaxed atomic from whichever thread made the call, so a
worker thread never writes a transport's counters. An unprivileged Linux
process is refused `SCHED_RR`; the count and the one line are how a game sees
that its network threads run at the default priority.

---

## 4. COUNTERS

### 4.1 The Counter Surface a Game Uses

A game reads counters and stats through the client and server accessors in
`sdk/rocketnet.h`: `rocketnet_client_counter`, `rocketnet_client_stat`,
`rocketnet_server_counter`, `rocketnet_server_stat`,
`rocketnet_server_client_counter` and `rocketnet_server_client_stat`. The
generic counter introspection functions (`rocketnet_num_counters`,
`rocketnet_counter_value` and `rocketnet_counter_name`) are internal to the
SDK (`sdk/rocketnet_layers.h`).

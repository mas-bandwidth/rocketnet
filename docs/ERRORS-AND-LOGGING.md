# Structured Errors, FFI Logging, and Runtime Counters

This document describes structured failure reporting on the public lifecycle,
client and server creation entry points, the preformatted FFI-safe logging
callback, and the runtime stats contract for C SDK consumers.
`sdk/rocketnet.h` is the authority for every public name, field, default and
environment variable in this document.

Read this document beside `docs/ABI-EVOLUTION.md`, which governs
shared-library symbol versioning, the config structs' `config_bytes` field, and
the version contract.

---

## 1. THE STATE

### 1.1 Public Lifecycle and Creation Entry Points

The public header is `sdk/rocketnet.h`. A game initializes the library, fills a
client or server config, creates one client or server, and checks the result.
Refusals have the return behavior documented by each declaration in
`sdk/rocketnet.h`; only the specific caller errors listed under “API misuse”
there increment the process-wide `api_misuses` field. A refusal is not
automatically a misuse, and not every refusal logs an error. For example,
invalid send arguments return `NULL` or `0` and log, but do not increment
`api_misuses`. A message send on a connection that is not CONNECTED is a normal
drop that logs nothing, returns `NULL` from start message and `0` from finish
message, and increments `messages_dropped_not_connected`, never the misuse count.

| Public Entry Point | Return on Refusal |
| :--- | :--- |
| `rocketnet_init( context, malloc_fn, free_fn )` | the status code: `INVALID_ARGUMENT` for one allocator function without the other, `ALREADY_INITIALIZED`, `LIVE_ALLOCATIONS` for an allocator change while allocations are live, `PLATFORM_INIT_FAILED` or `CRYPTO_INIT_FAILED` |
| `rocketnet_term( void )` | the status code: `NOT_INITIALIZED` or `LIVE_ALLOCATIONS`; nothing changes |
| `rocketnet_client_default_config( config )` | `INVALID_ARGUMENT` for a NULL config, counted; `CONFIG_NOT_INITIALIZED` is never returned, because the inline call sets `config_bytes` first |
| `rocketnet_client_required_memory( config )` | `0` for a refused config, a `config_bytes` too small included |
| `rocketnet_client_create( context, config, status )` | `NULL` with one error line; `status` holds the refusal's code (`NOT_INITIALIZED`, `CONFIG_NOT_INITIALIZED`, `INVALID_ARGUMENT`, `CONFIG_ENV_INVALID`, `CONFIG_OUT_OF_RANGE`, `INVALID_TRANSPORT_LIST`, `MEMORY_BUDGET_EXCEEDED`, `OUT_OF_MEMORY`), or `OK` with the handle |
| `rocketnet_client_connect( client, token )` | the status code: `CLIENT_NOT_DISCONNECTED`, `INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, or a transport's code |
| `rocketnet_server_default_config( config )` | `INVALID_ARGUMENT` for a NULL config, counted |
| `rocketnet_server_required_memory( config )` | `0` for a refused config, a `config_bytes` too small included |
| `rocketnet_server_create( context, ipv4_address, ipv6_address, config, status )` | `NULL` with one error line; `status` holds the refusal's code (the client's codes, plus `INVALID_ADDRESS` for an address that does not parse or names the wrong family), or `OK` with the handle |
| `rocketnet_server_start( server )` | the status code: `SERVER_NOT_STOPPED`, or the transport's code (`TRANSPORT_LADDER_EXHAUSTED`, `MISSING_KEYS`, ...) |
| `rocketnet_client_update( client, time )` / `rocketnet_server_update( server, time )` | nothing (`void`); a refused time is counted as API misuse and logged |
| `rocketnet_create_dev_connect_token`, `*_set_send_kbps` | the status code; see each declaration in `sdk/rocketnet.h` |

The client and server create paths read `config_bytes`, validate the config,
apply the environment overrides (`ROCKETNET_CLIENT_*`, `ROCKETNET_SERVER_*`,
with precedence DEFAULT < CONFIG STRUCT < ENVIRONMENT), and make the one
allocation through the allocator; client connect and server start start the
selected transport. Create is the one check of a config: there is no separate
validate call. The detailed transport create failure branches belong to the
internal transport layer (`sdk/rocketnet_layers.h`); a game sees the public
create result, status and any documented log line. Only failures named in the
public API-misuse list increment the misuse count.

For client `send_kbps` and server `per_client_send_kbps`, zero selects the
documented default. `ROCKETNET_SEND_KBPS_UNLIMITED` means no send budget on
that connection. Negative values are refused as described by
the config fields and `*_set_send_kbps` declarations in `sdk/rocketnet.h`.

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

### 2.1 Status Code Enum (`rocketnet_status_t`)

Status codes follow these rules:

1. `ROCKETNET_STATUS_OK = 0` is the sole success value.
2. Every failure code is negative, dense from `-1` to `-27`. `status < 0`
   means the call failed.
3. There is no count value. `rocketnet_status_name` maps each code by value.
   It returns a static ASCII identifier, for example `"INVALID_CONNECT_TOKEN"`,
   and `"UNKNOWN"` for any other value, never `NULL` and without asserting.

```c
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,

    // lifecycle and allocator state (rocketnet_init, rocketnet_term)
    ROCKETNET_STATUS_ALREADY_INITIALIZED                = -1,
    ROCKETNET_STATUS_NOT_INITIALIZED                    = -2,
    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = -3,
    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = -4,
    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = -5,

    // caller arguments and configuration (client, server, message)
    ROCKETNET_STATUS_INVALID_ARGUMENT                   = -6,
    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = -7,
    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = -8,
    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = -9,
    ROCKETNET_STATUS_MISSING_KEYS                       = -10,
    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = -11,
    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = -12,
    ROCKETNET_STATUS_INVALID_ADDRESS                    = -13,
    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = -14,

    // client connect token pre-send checks
    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = -15,
    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = -16,

    // transport candidate list and OS resources
    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = -17,
    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = -18,
    ROCKETNET_STATUS_OUT_OF_MEMORY                      = -19,
    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = -20,
    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = -21,
    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = -22,

    // object lifecycle (rocketnet_client_connect, rocketnet_server_start) and destinations
    ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED            = -23,
    ROCKETNET_STATUS_SERVER_NOT_STOPPED                 = -24,
    ROCKETNET_STATUS_FLOW_ID_RESERVED                   = -25,
    ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED     = -26,
    ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED             = -27
};
```

Every public call that can fail returns `enum rocketnet_status_t`:
`rocketnet_init` reports `INVALID_ARGUMENT`, `ALREADY_INITIALIZED`,
`LIVE_ALLOCATIONS`, `PLATFORM_INIT_FAILED` or `CRYPTO_INIT_FAILED`.
`rocketnet_term` reports `NOT_INITIALIZED` or `LIVE_ALLOCATIONS`. The client and server create
calls return a handle or `NULL`, write the code through the status
out-parameter, and log one `ERROR` line with the reason: `NOT_INITIALIZED`,
`CONFIG_NOT_INITIALIZED`, `INVALID_ARGUMENT`, `CONFIG_ENV_INVALID`,
`CONFIG_OUT_OF_RANGE`, `MEMORY_BUDGET_EXCEEDED`, `INVALID_TRANSPORT_LIST`,
`INVALID_ADDRESS` (server) or `OUT_OF_MEMORY`. Client connect and server start
return the token and transport codes: `CLIENT_NOT_DISCONNECTED`,
`INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, `SERVER_NOT_STOPPED`,
`TRANSPORT_LADDER_EXHAUSTED`, `TRANSPORT_FAMILIES_UNSUPPORTED`, `MISSING_KEYS`,
`TRANSPORT_PORT_ZERO_UNSUPPORTED`, `MUTEX_CREATE_FAILED`,
`THREAD_CREATE_FAILED` and `TIMER_CREATE_FAILED`.
`ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED` is -26: a transport returns it
when it cannot serve both named families, and start returns it when every
candidate that opened refused for that reason.
`ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED` is -27, the last code: a config whose
`config_bytes` is 0 or smaller than the field itself, one that skipped its
default_config call, is refused with it before any other check
(`docs/ABI-EVOLUTION.md`). Its string is `CONFIG_NOT_INITIALIZED`
(`rocketnet_status_name` in `sdk/rocketnet.cpp`).

### 2.2 Reporting

Init and term return the section-2.1 code directly (the SDK's
internal `_ex` forms write the same code to an `int *`). The client and server
create calls return a handle or `NULL` and write the refusal's code through the
final status out-parameter; their refusals are logged. A create before init is
counted in the process-wide `api_misuses` stat; other create refusals are not
automatically counted as misuse. `rocketnet_status_name` names any code. The internal transport layer
keeps `_ex` create forms for the SDK's own tests.

## 3. LOGGING

### 3.1 One Log Sink

The library has one log sink, installed with `rocketnet_log_function`.
Every log line is formatted once into a 1024-byte stack buffer and handed to
the installed callback, or to the default sink (stdout) when none is
installed. The level is `enum rocketnet_log_level_t`, set with
`rocketnet_log_level` (the SDK reads it internally with `rocketnet_log_level_current`);
`rocketnet_log_level_name` names a level by its enum (`"NONE"`, `"ERROR"`,
`"WARN"`, `"INFO"`, `"DEBUG"`, `"SPAM"`), and `"UNKNOWN"` for a value outside
the enum. `rocketnet_quiet( true )` suppresses every line and
`rocketnet_quiet( false )` resumes them, without changing the level.
`log_quiet` and `log_level` are relaxed atomics read before any formatting.

### 3.2 Callback Signature and Contract

```c
typedef void (*rocketnet_log_function_t)(
    void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

ROCKETNET_EXPORT void rocketnet_log_function(
    rocketnet_log_function_t function, void * context );
```

1. **Parameters**:
   - `context`: opaque pointer passed to `rocketnet_log_function` (may be `NULL`).
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
     `rocketnet_log_function` re-entrantly, and must not unwind via C++
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

The library's network-thread priority calls (`TRANSPORTS.md`, Server network threads and their
priority) check each result. A refused
call (`pthread_setschedparam` nonzero, `pthread_setaffinity_np` nonzero on the
XDP thread, `SetThreadPriority` FALSE, `timeBeginPeriod` or `timeEndPeriod`
not `TIMERR_NOERROR`) adds one to a process-wide count, read from the
`thread_priority_refusals` field of the client and server stats structs, and never fails a create or
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

### 4.1 The Stats Surface a Game Uses

A game reads counts and measurements through three calls in `sdk/rocketnet.h`,
each filling a caller struct with the current stats:
`rocketnet_client_stats` (`struct rocketnet_client_stats_t`),
`rocketnet_server_stats` (`struct rocketnet_server_stats_t`) and
`rocketnet_server_client_stats` (`struct rocketnet_server_client_stats_t`, one
slot). The client and server structs also carry the process-wide `api_misuses`
and `thread_priority_refusals` counts. A NULL stats pointer is a counted no-op;
a NULL client or server fills only the process-wide fields and zeroes the rest.
The generic counter introspection functions (`rocketnet_num_counters`,
`rocketnet_counter_value` and `rocketnet_counter_name`) are internal to the
SDK (`sdk/rocketnet_layers.h`).

Client counts are lifetime totals across connections; server aggregate counts
are lifetime totals across starts and stops. Both begin at zero at object
creation. Process-wide `api_misuses` and `thread_priority_refusals` do not reset
with an object. Client measurements cover the current connection, and client
and server rates cover the trailing second; these read as zero without an
active connection or while the server is stopped. Per-slot statistics
describe the current connection and reset when a slot is reused.

`rocketnet_client_receipts` and `rocketnet_server_receipts` copy outcomes from
the most recent update. Re-reading does not consume them; the copied entries
remain stable until the next update. If the ring fills, it drops the oldest
outcomes and increments `receipts_dropped`. A smaller output capacity copies
only the first entries that fit. A zero-filled receipt is NONE; ACKED and LOSS
are reported outcomes. See the function declarations in `sdk/rocketnet.h` for
NULL, free-slot and stopped-server behavior.

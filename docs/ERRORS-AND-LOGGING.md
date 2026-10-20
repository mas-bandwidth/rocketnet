# Structured Errors, FFI Logging, and Runtime Counters

This document describes structured failure reporting on the public lifecycle,
client and server creation entry points, the preformatted FFI-safe logging
callback, and the runtime counter inspection contract for C SDK consumers.
`sdk/rocketnet.h` is the authority for every public name, field, default and
environment variable in this document.

Read this document beside `docs/ABI-EVOLUTION.md` (`issue-149`), which governs
shared-library symbol versioning, header installation boundaries, and the
native ABI queries and version contract (`sdk/rocketnet_abi.cpp`).

The former C# binding and its smoke/managed ABI gate have been removed;
the C API, its logging surface and native verification remain in scope.

---

## 1. THE STATE

### 1.1 Public Lifecycle, Creation, and Validation Entry Points Today

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
| `rocketnet_client_default_config( config )` | `INVALID_ARGUMENT` for a NULL config, counted |
| `rocketnet_client_validate_config( config )` | `CONFIG_OUT_OF_RANGE`, with a log line naming the rule |
| `rocketnet_client_create( context, config, memory, memory_bytes )` | `NULL`, with one error line |
| `rocketnet_client_connect( client, token )` | the status code: `CLIENT_NOT_DISCONNECTED`, `INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, or a transport's code |
| `rocketnet_client_create_sized(...)` | `NULL` for an unknown config size |
| `rocketnet_server_default_config( config )` / `rocketnet_server_dev_config( config )` | `INVALID_ARGUMENT` for a NULL config, counted |
| `rocketnet_server_validate_config( config )` | `CONFIG_OUT_OF_RANGE`, with a log line naming the rule |
| `rocketnet_server_create( context, address, config, memory, memory_bytes )` | `NULL`, with one error line |
| `rocketnet_server_create_sized(...)` | `NULL` for an unknown config size |
| `rocketnet_server_start( server )` | the status code: `SERVER_NOT_STOPPED`, or the transport's code (`TRANSPORT_LADDER_EXHAUSTED`, `MISSING_KEYS`, ...) |
| `rocketnet_client_update( client, time )` / `rocketnet_server_update( server, time )` | `ROCKETNET_UPDATE_OK` or `ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED` |
| key and token writers, `*_set_send_kbps`, the `_sized` config calls | the status code; see each declaration in `sdk/rocketnet.h` |

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

`enum rocketnet_status_t` is defined in `sdk/rocketnet.h`; this document names
it by reference and does not copy it. Its rules, as the header states:

1. `ROCKETNET_STATUS_OK = 0` is the sole success value.
2. Every failure code is a positive integer in `1 .. ROCKETNET_STATUS_COUNT - 1`.
3. **Append-only stability law**: Once a code is published in a release, its
   integer value and meaning are permanent. New codes append immediately before
   `ROCKETNET_STATUS_COUNT`. Deprecated codes remain reserved in the enum
   and in `rocketnet_status_name` and are never renumbered or reassigned.
4. Runtime introspection:
   - `int rocketnet_num_status_codes(void)` returns `ROCKETNET_STATUS_COUNT`.
   - `const char * rocketnet_status_name(enum rocketnet_status_t status)` returns a static ASCII
     identifier (for example, `"INVALID_CONNECT_TOKEN"`) for `0 <= status < ROCKETNET_STATUS_COUNT`,
     and `"unknown"` (never `NULL`, without asserting) for any other value.

A public call that can fail returns `enum rocketnet_status_t`, except a create,
which returns a handle or `NULL`, and finish message, which returns a
`uint32_t` receipt serial; both refuse without a status.
`rocketnet_init` reports `ALREADY_INITIALIZED`, `PLATFORM_INIT_FAILED` or
`CRYPTO_INIT_FAILED`; `rocketnet_term` reports `NOT_INITIALIZED` or
`LIVE_ALLOCATIONS`; `rocketnet_set_allocator` reports `INVALID_ARGUMENT`,
`ALREADY_INITIALIZED` or `LIVE_ALLOCATIONS`. The client and server create
paths log one `ERROR` line with the reason,
including `CONFIG_ENV_INVALID`, `CONFIG_OUT_OF_RANGE`,
`MEMORY_BUDGET_EXCEEDED`, `INVALID_TRANSPORT_LIST`, `OUT_OF_MEMORY`,
`INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, `TRANSPORT_LADDER_EXHAUSTED`,
`MISSING_KEYS`, `INVALID_PROTOCOL_ID`, `INVALID_ADDRESS`,
`TRANSPORT_PORT_ZERO_UNSUPPORTED`, `MUTEX_CREATE_FAILED`,
`THREAD_CREATE_FAILED` and `TIMER_CREATE_FAILED`.

### 2.2 The reporting today

Init, term and the allocator return the section-2.1 code directly. The client
and server create calls return a handle or `NULL`; the reason is one `ERROR`
log line, and the refusal is counted in `rocketnet_api_misuse_count`.
`rocketnet_status_name` names any code, and `rocketnet_num_status_codes`
gives the count.

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
priority; `DRIVERS.md`, Server network threads) check each result. A refused
call (`pthread_setschedparam` nonzero, `pthread_setaffinity_np` nonzero on the
XDP thread, `SetThreadPriority` FALSE, `timeBeginPeriod` or `timeEndPeriod`
not `TIMERR_NOERROR`) adds one to a process-wide count, read with
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

### 4.2 What Remains Open in the Internal Counter Layer

A game reads counters through the public client and server accessors; the
internal counter surface is not import API. The internal layer keeps a few
open items:

1. **Debug-mode abort on out-of-range `rocketnet_counter_name`**:
   - `rocketnet_counter_name` asserts and returns `"???"` when a caller probes
     `< 0` or `>= ROCKETNET_NUM_COUNTERS`.
2. **Non-counter state inside `struct rocketnet_counters_t`**:
   - `struct rocketnet_counters_t` contains `uint64_t values[ROCKETNET_NUM_COUNTERS]`
     plus `uint64_t handshakes_processed_this_update`, which is mutable per-update
     server budget state and not a counter value.
3. **No public C snapshot helper for opaque `rocketnet_counters_t`**:
   - External callers that only include `sdk/rocketnet.h` have no batch copy
     function to snapshot counters without looping `rocketnet_counter_value`.

### 4.3 Counter Design and Remediation

1. **Single-source X-macro table (`ROCKETNET_COUNTERS_TABLE`)**: Define the
   counter list once in `sdk/rocketnet_counters.h` to generate both the enum and
   `rocketnet_counter_name`, and resolve the `CONNECTION_RESPONSE_WRONG_SIZE`
   count in the native API.
2. **Keep `struct rocketnet_counters_t` pure**: Move
   `handshakes_processed_this_update` onto `struct rocketnet_server_protocol_t`,
   and pin `offsetof(rocketnet_counters_t, values) == 0` and
   `sizeof(rocketnet_counters_t) == ROCKETNET_NUM_COUNTERS * sizeof(uint64_t)`.
3. **Read counters through the internal accessors and fix their bounds**:
   a game reads the public client and server counter and stat accessors; the
   internal counter surface (`rocketnet_counter_value`, `rocketnet_num_counters`
   and `rocketnet_counter_name` in `sdk/rocketnet_layers.h`) returns a value or
   a name for an in-range index and refuses an out-of-range index without an
   assert.

### 4.4 Where This Design Meets `docs/ABI-EVOLUTION.md`

The `struct rocketnet_counters_t` body is internal. Gating it behind
`#ifdef ROCKETNET_IMPLEMENTATION` (leaving the `ROCKETNET_COUNTER_*` enum
public and using `rocketnet_counter_value` and `rocketnet_num_counters` across
the ABI) prevents new counters from changing any public struct size.

---

## 5. THE LAYERS

| Layer | Name | Files Owned | What It Guarantees | How It Is Checked |
| :--- | :--- | :--- | :--- | :--- |
| **Layer 1** | Counter Table & C API | `sdk/rocketnet_counters.h`, `sdk/rocketnet_counters.cpp`, `sdk/rocketnet_server_protocol.h`, `sdk/rocketnet_server_protocol.cpp`, `sdk/rocketnet.h`, `tools/exports.txt` | Single `ROCKETNET_COUNTERS_TABLE` generates the C enum and `rocketnet_counter_name`; `handshakes_processed_this_update` moves to `rocketnet_server_protocol_t`; `rocketnet_counter_name` never asserts on out-of-range indices; `rocketnet_counter_value` copies bounded prefixes. | `test_unit_counters.cpp` in `./bin/test` (bijective `0 .. rocketnet_num_counters() - 1` names, out-of-range bounds, counter value checks, `sizeof(rocketnet_counters_t)`). |
| **Layer 2** | Preformatted Log Callback | `sdk/rocketnet.h`, `sdk/rocketnet.cpp`, `tools/exports.txt` | `log_quiet` and `log_level` use atomic loads before `vsnprintf`; `rocketnet_set_log_callback(cb, ctx)` delivers `(ctx, level, message, length)` with `message[length] == '\0'` and `length <= 1023` in one `vsnprintf` pass; the default sink formats nothing a second time; it is the one sink. | `test_unit_logging.cpp` in `./bin/test` (level filtering before format, `log_quiet`, `context` forwarding, exact `length` and NUL termination at `0`, `1023`, `1024`, `2048`, sink switching, multi-thread logging). |
| **Layer 3** | Status Enum & Plumbing | `sdk/rocketnet.h`, `sdk/rocketnet.cpp`, `sdk/rocketnet_config.h`, `sdk/rocketnet_config.cpp`, `sdk/rocketnet_client_protocol.h`, `sdk/rocketnet_client_protocol.cpp`, `tools/exports.txt` | `enum rocketnet_status_t` (`0 .. ROCKETNET_STATUS_COUNT - 1`), `rocketnet_status_name`, and `rocketnet_num_status_codes` are defined; internal config, ladder, and token helpers accept `int * status` and log at `ROCKETNET_LOG_LEVEL_ERROR` on every refusal. | `test_unit_status.cpp` in `./bin/test` verifying every status string, out-of-range `rocketnet_status_name` queries, and exact status codes from config, transport ladder, and connect-token validation. |
| **Layer 4** | Public Lifecycle and Create Entry Points | `sdk/rocketnet.h`, `sdk/rocketnet.cpp`, `tools/exports.txt` | `rocketnet_init`, `rocketnet_term`, `rocketnet_set_allocator`, `rocketnet_client_create`, `rocketnet_server_create`, and `rocketnet_client_validate_config` report `rocketnet_status_t` or a handle or `NULL`, each with its log line and misuse count. | `test_lifecycle.cpp` and `test_unit_status.cpp` exercising every failure branch with a fault-injecting allocator failing at each sequential allocation site to prove `ROCKETNET_STATUS_OUT_OF_MEMORY` and zero leaks at `rocketnet_term`. |

---

## 6. QUESTIONS

1. **`Q1` (Out-of-range return value for `rocketnet_status_name`)**:
   When `rocketnet_status_name(status)` is called with an out-of-range integer,
   should it return `NULL` or the literal `"unknown"` it returns today?
2. **`Q2` (Timing of `rocketnet_set_log_callback` registration)**:
   Should `rocketnet_set_log_callback` allow live replacement at any time via the
   atomic spin lock in Section 3.2, or restrict registration to when no
   transports are active?

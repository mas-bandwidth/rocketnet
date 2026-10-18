# Structured Errors, FFI Logging, and Runtime Counters

This document specifies the design for structured failure reporting on lifecycle
and object creation entry points, a preformatted FFI-safe logging callback, and
the runtime counter inspection contract for C SDK consumers
(`issue-157`). Every statement of current behavior cites exact `file:line`
locations at commit `2507f11c383580f73241563fded80b35e2d2d4e6`.

Read this document beside `docs/ABI-EVOLUTION.md` (`issue-149`), which governs
shared-library symbol versioning, header installation boundaries, and the
native ABI queries and version contract (`sdk/rocketnet_abi.cpp`).

**Status: historical design for review before Layer 1 implementation begins.**
The former C# binding and its smoke/managed ABI gate have been removed;
C API/ABI design and native verification remain in scope.

---

## 1. THE STATE

### 1.1 Public Lifecycle, Creation, and Validation Entry Points Today

At commit `2507f11`, the shared-library C ABI (`tools/exports.txt:1-143`) and
public headers (`sdk/rocketnet.h`, `sdk/rocketnet_platform.h`) expose the
following initialization, creation, teardown, and configuration validation
entry points. None of the object-creating functions returns a structured error
code: every failure collapses to `NULL` or `false` / `0` / `void`, and the specific refusal reason is either written only
to `rocketnet_error` (a varargs-only C callback)
or omitted from `ERROR`-level logs altogether.

| Public Entry Point | Declaration & Export | Implementation | Return & Failure Signal | How Failure Reasons Surface Today |
| :--- | :--- | :--- | :--- | :--- |
| `rocketnet_init(context)` | `sdk/rocketnet.h:248`, `tools/exports.txt:52` | `sdk/rocketnet.cpp:446-489` | `bool` (`false` on failure) | Re-entry or concurrent transition returns `false` **silently** (`sdk/rocketnet.cpp:450-451`); platform or sodium init failure logs via `rocketnet_error` (`:469`) and returns `false`. |
| `rocketnet_term()` | `sdk/rocketnet.h:253`, `tools/exports.txt:134` | `sdk/rocketnet.cpp:491-512` | `void` | Call when uninitialized or transitioning returns **silently** (`:495-496`); call while `rocketnet_live_allocations != 0` logs via `rocketnet_error` (`:501`) without terminating. |
| `rocketnet_allocator(malloc_fn, free_fn)` | `sdk/rocketnet.h:319`, `tools/exports.txt:12` | `sdk/rocketnet.cpp:258-272` | `void` | Mismatched NULL/non-NULL pair (`:261`), call while initialized (`:264`), or call with live allocations (`:266`) logs via `rocketnet_error` (`:268`) and keeps prior callbacks. |
| `rocketnet_client_transport_create(ctx, token, cfg, time)` | `sdk/rocketnet.h:548`, `tools/exports.txt:20` | `sdk/rocketnet_client_transport.cpp:155-371` | `rocketnet_client_transport_t *` (`NULL` on failure) | 11 failure exits return `NULL` (Section 1.2). Nine log via `rocketnet_error`; reassembly budget refusal (`:186`) and connect-token refusals (`:254`, `DEBUG`/`INFO` only at `sdk/rocketnet_client_protocol.cpp:146,153`) emit **no `ERROR` log line**. |
| `rocketnet_server_transport_create(ctx, addr, cfg, time)` | `sdk/rocketnet.h:751`, `tools/exports.txt:109` | `sdk/rocketnet_server_transport.cpp:462-876` | `rocketnet_server_transport_t *` (`NULL` on failure) | 14 failure exits return `NULL` (Section 1.3). Eleven log via `rocketnet_error`; reassembly budget refusal (`:520`), outer struct malloc (`:622`), and sub-table mallocs (`:861`) emit **no outer `ERROR` log line**. |
| `rocketnet_frame_sync_create(ctx, rate)` | `sdk/rocketnet.h:1019`, `tools/exports.txt:45` | `sdk/rocketnet_frame_sync.cpp:274-320` | `rocketnet_frame_sync_t *` (`NULL` on failure) | 3 failure exits return `NULL` (Section 1.4): invalid `frame_rate` (`:278`) and Windows timer failure (`:311`) log via `rocketnet_error`; malloc failure (`:287`) returns `NULL` **silently**. |
| `rocketnet_message_validate_config(cfg)` | `sdk/rocketnet.h:1195`, `tools/exports.txt:61` | `sdk/rocketnet_config.cpp:98-167` | `int` (`0` on refusal, `1` on valid) | Logs the violated field or cross-field rule via `rocketnet_error` (`sdk/rocketnet_config.cpp:109,120,127,134,141,149,156,162`) and returns `0`. |
| `rocketnet_platform_socket_create(...)` | `sdk/rocketnet_platform.h:53`, `tools/exports.txt:82` | `sdk/rocketnet_platform.cpp:226,1414,2632` | `rocketnet_platform_socket_t *` (`NULL` on failure) | Logs socket/bind/fcntl/setsockopt failure via `rocketnet_error` and returns `NULL`. |
| `rocketnet_platform_socket_create_with_pump(..., pump)` | `sdk/rocketnet_platform.h:60`, `tools/exports.txt:83` | `sdk/rocketnet_platform.cpp:3096-3160` | `rocketnet_platform_socket_t *` (`NULL` on failure) | Logs unknown/unsupported pump name or socket failure via `rocketnet_error` and returns `NULL`. |
| `rocketnet_platform_thread_create(ctx, fn, arg)` | `sdk/rocketnet_platform.h:105`, `tools/exports.txt:91` | `sdk/rocketnet_platform.cpp:614,2191,2399` | `rocketnet_platform_thread_t *` (`NULL` on failure) | Returns `NULL` when `rocketnet_malloc` or `pthread_create` / `CreateThread` fails. |
| `rocketnet_platform_mutex_create(mutex)` | `sdk/rocketnet_platform.h:117`, `tools/exports.txt:75` | `sdk/rocketnet_platform.cpp:673,2250,2457` | `bool` (`false` on failure) | Returns `false` when `pthread_mutex_init` fails (`sdk/rocketnet_platform.cpp:681,2258`). |

### 1.2 Every Failure Exit Inside `rocketnet_client_transport_create`

`rocketnet_client_transport_create` (`sdk/rocketnet_client_transport.cpp:155-371`)
has 11 failure exits that all return `NULL`:

| # | Location (`2507f11`) | Condition | Log Output Today | Cleanup / Side Effects |
| :--- | :--- | :--- | :--- | :--- |
| C1 | `sdk/rocketnet_client_transport.cpp:157` | `connect_token == NULL` | `rocketnet_assert(connect_token)` (aborts in debug; dereferences `NULL` at `sdk/rocketnet_client_protocol.cpp:136` in release) | The C API has no release-mode check. |
| C2 | `sdk/rocketnet_client_transport.cpp:174-175` | `!rocketnet_config_apply_env("client", ...)` (`sdk/rocketnet_config.cpp:307-418`) | `rocketnet_error` naming the `ROCKETNET_*` variable (`sdk/rocketnet_config.cpp:320,353,364,380,386,397,406`) | None. |
| C3 | `sdk/rocketnet_client_transport.cpp:177-178` | `!rocketnet_config_validate("client", ...)` (`sdk/rocketnet_config.cpp:420-487`) | `rocketnet_error` naming the out-of-range int or unterminated string (`sdk/rocketnet_config.cpp:442,472,480`) | None. |
| C4 | `sdk/rocketnet_client_transport.cpp:186-187` | `!rocketnet_reassembly_memory_allowed(1, requested_slots, requested_payload, ROCKETNET_NUM_CLIENT_PACKETS / 2)` | None at call site | Refuses cross-field reassembly allocation above `ROCKETNET_REASSEMBLY_MEMORY_MIB` (`CMakeLists.txt:47`). |
| C5 | `sdk/rocketnet_client_transport.cpp:197-198` | `!rocketnet_driver_ladder_parse(config->driver, ...)` (`sdk/rocketnet_config.cpp:503-638`) | `rocketnet_error` (`sdk/rocketnet_config.cpp:554,560,567,585,599`) | None. |
| C6 | `sdk/rocketnet_client_transport.cpp:200-205` | `!client_transport` from `rocketnet_malloc` | `rocketnet_error("could not allocate client")` (`:203`) | None. |
| C7 | `sdk/rocketnet_client_transport.cpp:220-235` | `!client_transport->send_arena \|\| !client_transport->reassembly` | `rocketnet_error("client could not allocate fragmentation storage")` (`:232`) | Destroys `client_transport` (`:233`). |
| C8 | `sdk/rocketnet_client_transport.cpp:254-258` | `!rocketnet_client_protocol_connect(...)` (`sdk/rocketnet_client_protocol.cpp:141-149`): bad `address_flags`, `create_timestamp > expire_timestamp`, or `version != ROCKETNET_WIRE_VERSION` | `rocketnet_debug` (`:146`) and `rocketnet_info` (`:42`) — **silent at default `ROCKETNET_LOG_LEVEL_ERROR`** | Sets `STATE_INVALID_CONNECT_TOKEN`, then destroys `client_transport` (`:256`) so caller cannot read state. |
| C9 | `sdk/rocketnet_client_transport.cpp:254-258` | `!rocketnet_client_protocol_connect(...)` (`sdk/rocketnet_client_protocol.cpp:151-156`): `expire_timestamp <= current_timestamp` | `rocketnet_debug` (`:153`) and `rocketnet_info` (`:42`) — **silent at default `ROCKETNET_LOG_LEVEL_ERROR`** | Sets `STATE_CONNECT_TOKEN_EXPIRED`, then destroys `client_transport` (`:256`). |
| C10 | `sdk/rocketnet_client_transport.cpp:326-341` | `granted_driver < 0` after trying `ladder.candidates` (`:307-324`) | Per-rung `rocketnet_warn` (`:319`) plus `rocketnet_error("client driver ladder exhausted...")` (`:336`) | Destroys `client_transport` (`:339`). |
| C11 | `sdk/rocketnet_client_transport.cpp:355-368` | `!rocketnet_platform_mutex_create` (`:355`) or `!client_transport->thread` (`:362`) | `rocketnet_error("client could not create mutex")` (`:357`) or `"client could not create thread"` (`:365`) | Destroys `client_transport` (`:358,366`). |

### 1.3 Every Failure Exit Inside `rocketnet_server_transport_create`

`rocketnet_server_transport_create` (`sdk/rocketnet_server_transport.cpp:462-876`)
has 14 failure exits that all return `NULL` without consuming the one-shot
`ROCKETNET_PUBLIC_ADDRESS` environment override (`sdk/rocketnet_server_transport.cpp:539,869-873`):

| # | Location (`2507f11`) | Condition | Log Output Today | Cleanup / Side Effects |
| :--- | :--- | :--- | :--- | :--- |
| S1 | `sdk/rocketnet_server_transport.cpp:467` | `public_address_string == NULL` | `rocketnet_assert(public_address_string)` (debug assert only) | Dereferences `NULL` at `:560` or `:597` in release builds. |
| S2 | `sdk/rocketnet_server_transport.cpp:498-499` | `!rocketnet_config_apply_env("server", ...)` (`sdk/rocketnet_config.cpp:307-418`) | `rocketnet_error` naming the `ROCKETNET_*` variable (secrets redacted at `sdk/rocketnet_config.cpp:334,353`) | None. |
| S3 | `sdk/rocketnet_server_transport.cpp:501-508` | Deprecated alias parse/length failure (`ROCKETNET_SERVER_NUM_QUEUES`, `ROCKETNET_SERVER_NUM_OS_CPUS`, `ROCKETNET_SERVER_PUBLIC_ADDRESS`) | `rocketnet_error` (`sdk/rocketnet_server_transport.cpp:391,415`) | None. |
| S4 | `sdk/rocketnet_server_transport.cpp:510-511` | `!rocketnet_config_validate("server", ...)` (`sdk/rocketnet_config.cpp:420-487`) | `rocketnet_error` (`sdk/rocketnet_config.cpp:442,472`) | None. |
| S5 | `sdk/rocketnet_server_transport.cpp:520-521` | `!rocketnet_reassembly_memory_allowed(requested_clients, requested_slots, requested_payload, requested_clients * requested_slots)` | None at call site | Refuses cross-field reassembly allocation above `ROCKETNET_REASSEMBLY_MEMORY_MIB`. |
| S6 | `sdk/rocketnet_server_transport.cpp:571-575` | `!rocketnet_server_config_has_keys(config)` (`:361-368`: any of the three 32-byte keys is all-zero) | `rocketnet_error("server config has no keys...")` (`:573`) | None. |
| S7 | `sdk/rocketnet_server_transport.cpp:578-582` | `config->protocol_id == 0` | `rocketnet_error("protocol_id 0 is not permitted with production keys...")` (`:580`) | None. |
| S8 | `sdk/rocketnet_server_transport.cpp:591-592` | `!rocketnet_driver_ladder_parse(config->driver, ...)` (`sdk/rocketnet_config.cpp:503-638`) | `rocketnet_error` (`sdk/rocketnet_config.cpp:554,560,567,585,599`) | None. |
| S9 | `sdk/rocketnet_server_transport.cpp:597-601` | `!rocketnet_address_parse(&public_address, public_address_string)` | `rocketnet_error("could not parse public address")` (`:599`) | None. |
| S10 | `sdk/rocketnet_server_transport.cpp:621-623` | `!server_transport` from `rocketnet_malloc` | **None** (returns `NULL` silently) | None. |
| S11 | `sdk/rocketnet_server_transport.cpp:635-646` | `development_keys` in a `!ROCKETNET_DEVELOPMENT` release build | `rocketnet_error("development keys detected in release build; refusing to start")` (`:642`) | Calls `rocketnet_free(context, server_transport)` (`:643`). |
| S12 | `sdk/rocketnet_server_transport.cpp:697-723,775-787` | `public_address.port == 0` and all candidates refused because `!rocketnet_driver_discovers_bound_port(candidate)` | Per-rung `rocketnet_warn` (`:721`), ladder-exhausted `rocketnet_error` (`:764`), plus port-zero `rocketnet_error` (`:781`) | Destroys `server_transport` (`:785`). |
| S13 | `sdk/rocketnet_server_transport.cpp:754-787` | `granted_driver < 0` after trying `ladder.candidates` (non-port-zero driver refusals) | Per-rung `rocketnet_warn` (`:735`) plus `rocketnet_error("server driver ladder exhausted...")` (`:764`) | Destroys `server_transport` (`:785`). |
| S14 | `sdk/rocketnet_server_transport.cpp:839-865` | Any of `encryption_map`, `used_token_history`, `client_slots`, `acks`, `reassembly`, or `send_arena` is `NULL` | **None at outer branch** | Destroys `server_transport` (`:863`). |

### 1.4 Every Failure Exit Inside `rocketnet_frame_sync_create`

`rocketnet_frame_sync_create` (`sdk/rocketnet_frame_sync.cpp:274-320`) has 3
failure exits that return `NULL`:

| # | Location (`2507f11`) | Condition | Log Output Today | Cleanup / Side Effects |
| :--- | :--- | :--- | :--- | :--- |
| F1 | `sdk/rocketnet_frame_sync.cpp:276-280` | `!(frame_rate > 0.0) \|\| frame_rate > ROCKETNET_FRAME_SYNC_MAX_FRAME_RATE` (`1000.0`, `sdk/rocketnet.h:1007`) | `rocketnet_error("frame sync: invalid frame rate %.1f", frame_rate)` (`:278`) | Rejects `<= 0.0`, `> 1000.0`, and `NaN`. |
| F2 | `sdk/rocketnet_frame_sync.cpp:286-288` | `!frame_sync` from `rocketnet_malloc` | **None** (returns `NULL` silently) | None. |
| F3 | `sdk/rocketnet_frame_sync.cpp:302-315` | `!frame_sync->waitable_timer` on Windows after both high-res and fallback `CreateWaitableTimerExW` | `rocketnet_error("frame sync: could not create waitable timer (%lu)", ...)` (`:311`) | Calls `rocketnet_free(context, frame_sync)` (`:312`). |

---

## 2. STATUS CODES

### 2.1 Stable Status Code Enum (`rocketnet_status_t`)

Status codes follow the same numbering discipline as `ROCKETNET_MESSAGE_VIOLATION_*`
(`sdk/rocketnet_message.h:85-98`) and `ROCKETNET_COUNTER_*`
(`sdk/rocketnet_counters.h:37-350`):

1. `ROCKETNET_STATUS_OK = 0` is the sole success value.
2. Every failure code is a positive integer in `1 .. ROCKETNET_NUM_STATUS_CODES - 1`.
3. **Append-only stability law**: Once a code is published in a release, its
   integer value and meaning are permanent. New codes append immediately before
   `ROCKETNET_NUM_STATUS_CODES`. Deprecated codes remain reserved in the enum
   and in `rocketnet_status_string` and are never renumbered or reassigned.
4. Runtime introspection mirrors counters (`sdk/rocketnet.h:1218-1221`):
   - `int rocketnet_num_status_codes(void)` returns `ROCKETNET_NUM_STATUS_CODES`.
   - `const char * rocketnet_status_string(int status)` returns a static ASCII
     identifier (for example, `"INVALID_CONNECT_TOKEN"`) for `0 <= status < ROCKETNET_NUM_STATUS_CODES`,
     and `NULL` (without asserting) for out-of-range values.

```c
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,

    // lifecycle and allocator state (rocketnet_init, rocketnet_term, rocketnet_allocator)
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

    // driver ladder and OS resources
    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED       = 17,
    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED            = 18,
    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,
    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,
    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,
    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,

    ROCKETNET_NUM_STATUS_CODES                          = 23
};
```

Every failure exit in Section 1 maps deterministically to one code:
`rocketnet_init` $\to$ `ALREADY_INITIALIZED` (`:450`), `PLATFORM_INIT_FAILED` (`:458`),
`CRYPTO_INIT_FAILED` (`:459`); `rocketnet_term` $\to$ `NOT_INITIALIZED` (`:495`),
`LIVE_ALLOCATIONS` (`:499`); `rocketnet_allocator` $\to$ `INVALID_ARGUMENT` (`:261`),
`ALREADY_INITIALIZED` (`:264`), `LIVE_ALLOCATIONS` (`:266`);
`rocketnet_client_transport_create` $\to$ C1 `INVALID_ARGUMENT`, C2 `CONFIG_ENV_INVALID`,
C3 `CONFIG_OUT_OF_RANGE`, C4 `MEMORY_BUDGET_EXCEEDED`, C5 `INVALID_TRANSPORT_LIST`,
C6/C7 `OUT_OF_MEMORY`, C8 `INVALID_CONNECT_TOKEN`, C9 `CONNECT_TOKEN_EXPIRED`,
C10 `TRANSPORT_LADDER_EXHAUSTED`, C11 `MUTEX_CREATE_FAILED` / `THREAD_CREATE_FAILED`;
`rocketnet_server_transport_create` $\to$ S1 `INVALID_ARGUMENT`, S2/S3 `CONFIG_ENV_INVALID`,
S4 `CONFIG_OUT_OF_RANGE`, S5 `MEMORY_BUDGET_EXCEEDED`, S6 `MISSING_KEYS`,
S7 `INVALID_PROTOCOL_ID`, S8 `INVALID_TRANSPORT_LIST`, S9 `INVALID_ADDRESS`,
S10/S14 `OUT_OF_MEMORY`, S11 `DEV_KEYS_IN_RELEASE`, S12 `TRANSPORT_PORT_ZERO_UNSUPPORTED`,
S13 `TRANSPORT_LADDER_EXHAUSTED`; `rocketnet_frame_sync_create` $\to$ F1 `INVALID_ARGUMENT`,
F2 `OUT_OF_MEMORY`, F3 `TIMER_CREATE_FAILED`; `rocketnet_message_validate_config` $\to$
`CONFIG_OUT_OF_RANGE`.

### 2.2 Comparing the Three Reporting Mechanisms

- **Option A (`_create_ex` out-parameter)**: Add `rocketnet_*_create_ex(..., int * status)`
  and `rocketnet_init_ex(void * context, int * status)` with an optional
  (`NULL`-tolerant) out-parameter; existing `rocketnet_*_create` and
  `rocketnet_init` functions remain thin wrappers passing `status = NULL`.
- **Option B (Per-thread last-error)**: Keep only the existing signatures and
  record the most recent status code in thread-local storage via
  `int rocketnet_last_error(void)`.
- **Option C (Status-returning create)**: Change creation functions to return
  `int` (`rocketnet_status_t`) and write the created pointer to
  `struct rocketnet_*_t ** out_transport`.

| Dimension | Option A: `_create_ex(..., int * status)` Out-Param | Option B: Per-Thread `rocketnet_last_error()` | Option C: `int *_create(..., handle ** out)` |
| :--- | :--- | :--- | :--- |
| **C Callers** | **Zero migration cost** for existing callers; callers wanting the status pass `&status`. Pure data flow; cannot be clobbered by teardown callbacks. | Existing call sites unchanged, but implicit TLS can be overwritten if a custom allocator or log callback runs inside mid-create teardown (`rocketnet_*_transport_destroy` at `sdk/rocketnet_client_transport.cpp:256,339`). | **Breaks every existing C call site** (`client.c`, `server.c`, `soak.c`, `soak_sim.cpp`, `functional/`, `sdk/rocketnet_tests.cpp`, `tools/package_consumer/main.c`), or requires parallel `_ex` names anyway. |
| **ABI Stability (`tools/exports.txt`, `ROCKETNET_ABI_VERSION`)** | **Purely additive**: all 142 existing exports in `tools/exports.txt` preserve their exact signatures; `_ex` symbols append without forcing a `ROCKETNET_ABI_VERSION` break. | Additive in `tools/exports.txt`, but introduces cross-DLL `thread_local` runtime state on Linux, macOS, BSD, and Windows. | **Immediate ABI break** on `rocketnet_client_transport_create`, `rocketnet_server_transport_create`, and `rocketnet_frame_sync_create`, requiring a `ROCKETNET_ABI_VERSION` bump. |

### 2.3 Recommendation

**Recommend Option A (`_create_ex` with an optional `int * status` out-parameter)**,
paired with `rocketnet_status_string` and `rocketnet_num_status_codes`:

```c
ROCKETNET_EXPORT_FUNC int rocketnet_num_status_codes( void );
ROCKETNET_EXPORT_FUNC const char * rocketnet_status_string( int status );
ROCKETNET_EXPORT_FUNC bool rocketnet_init_ex( void * context, int * status );
ROCKETNET_EXPORT_FUNC int rocketnet_term_ex( void );
ROCKETNET_EXPORT_FUNC int rocketnet_allocator_ex(
    void * (*malloc_function)( void * context, size_t bytes ),
    void (*free_function)( void * context, void * pointer ) );
ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t *
rocketnet_client_transport_create_ex(
    void * context, const uint8_t * connect_token,
    const struct rocketnet_client_transport_config_t * config,
    double time, int * status );
ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t *
rocketnet_server_transport_create_ex(
    void * context, const char * public_address_string,
    const struct rocketnet_server_transport_config_t * config,
    double time, int * status );
ROCKETNET_EXPORT_FUNC struct rocketnet_frame_sync_t *
rocketnet_frame_sync_create_ex( void * context, double frame_rate, int * status );
ROCKETNET_EXPORT_FUNC int rocketnet_message_validate_config_ex(
    const struct rocketnet_message_config_t * config, int * status );
```

If `status != NULL`, `*status` is written on every return path (`ROCKETNET_STATUS_OK`
on success, non-zero `ROCKETNET_STATUS_*` on failure), and every failure path
also emits a `rocketnet_error` line (closing the silent refusal gaps at
`sdk/rocketnet_client_protocol.cpp:146,153`, `sdk/rocketnet_client_transport.cpp:186`,
`sdk/rocketnet_server_transport.cpp:520,622,861`, and `sdk/rocketnet_frame_sync.cpp:287`).

---

## 3. LOGGING

### 3.1 What the Current Logger Does (`sdk/rocketnet.cpp:79-222`)

1. **Varargs callback signature**: `sdk/rocketnet.h:299` exposes
   `void rocketnet_log_function( void (*function)( int level, const char * format, ... ) )`.
   C variadic function pointers are not a portable callback form across
   language boundaries; a preformatted C callback is the proposed addition.
2. **Redundant formatting (`sdk/rocketnet.cpp:114-222`)**: Every log wrapper
   (`rocketnet_printf` at `:152`, `rocketnet_info` at `:164`, `rocketnet_warn` at
   `:176`, `rocketnet_error` at `:188`, `rocketnet_debug` at `:200`, `rocketnet_spam`
   at `:212`) already formats its `va_list` into `char buffer[1024]` via
   `vsnprintf( buffer, sizeof( buffer ), format, args )` and calls
   `log_function( level, "%s", buffer )`. When `default_log_function`
   (`sdk/rocketnet.cpp:114-136`) is active, it runs `vsnprintf` a **second**
   time into another `char buffer[1024]` before calling `printf` and `fflush( stdout )`.
3. **Unfenced globals across threads (`sdk/rocketnet.cpp:79,86,138`)**:
   `log_quiet`, `log_level`, and `log_function` are plain non-atomic static
   variables, even though the client receive thread (`sdk/rocketnet_client_transport.cpp:150`),
   driver worker threads, and concurrent fill threads (`sdk/rocketnet_client_transport.cpp:643,663,684`)
   read them while the main thread may call `rocketnet_quiet` or `rocketnet_log_level`.

### 3.2 Preformatted Callback Signature and Contract

```c
typedef void (*rocketnet_log_callback_t)(
    void * context, int level, const char * message, size_t length );

ROCKETNET_EXPORT_FUNC void rocketnet_set_log_callback(
    rocketnet_log_callback_t callback, void * context );
```

1. **Parameters**:
   - `context`: opaque pointer passed to `rocketnet_set_log_callback` (may be `NULL`).
   - `level`: `ROCKETNET_LOG_LEVEL_ERROR` (`1`) .. `ROCKETNET_LOG_LEVEL_SPAM` (`5`),
     or `ROCKETNET_LOG_LEVEL_NONE` (`0`) for `rocketnet_printf` (`sdk/rocketnet.cpp:161`).
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
     between `update` and `send_packets`, or on a transport/driver receive
     thread. Callbacks must be thread-safe, must not call `rocketnet_set_log_callback`
     or `rocketnet_log_function` re-entrantly, and must not unwind via C++
     exceptions or `longjmp`.
   - `log_quiet` and `log_level` become `std::atomic<bool>` and `std::atomic<int>`
     (relaxed load before `vsnprintf`). The `(callback, context, legacy_log_function)`
     state is guarded by `rocketnet_platform_spin_lock_u64` (`sdk/rocketnet_platform.h:388`)
     so `callback` and `context` are never observed torn.

### 3.3 Coexistence with `rocketnet_log_function` During Migration

`rocketnet_log_function` (`sdk/rocketnet.h:299`, `tools/exports.txt:53`) remains
exported for backward compatibility. Both registration entry points feed a
single internal helper `rocketnet_log_v( int level, const char * format, va_list args )`
in `sdk/rocketnet.cpp`:

1. **Filter before formatting**: If `log_quiet.load(std::memory_order_relaxed)`
   is true or (for `level != ROCKETNET_LOG_LEVEL_NONE`)
   `level > log_level.load(std::memory_order_relaxed)`, return before `vsnprintf`.
2. **Format once**: Run `vsnprintf( buffer, sizeof( buffer ), format, args )`
   once and compute clamped `size_t length`.
3. **Dispatch**: If `callback` was set via `rocketnet_set_log_callback`, invoke
   `callback(context, level, buffer, length)`; else if a custom variadic
   `function` was set via `rocketnet_log_function`, invoke
   `function(level, "%s", buffer)`; else invoke `default_log_callback(NULL, level, buffer, length)`
   directly (`fwrite` + `fflush(stdout)` with no second `vsnprintf`).
4. **Last registration wins**: Setting a non-NULL callback on either entry point
   replaces the active sink; passing `NULL` restores `default_log_callback`.

---

## 4. COUNTERS

### 4.1 What Already Landed (`sdk/rocketnet.h:1214-1221`, `sdk/rocketnet_counters.cpp:9-225`)

Three runtime counter introspection functions are exported in
`tools/exports.txt:40,41,62` and declared in `sdk/rocketnet.h:1218-1221`:
`int rocketnet_num_counters()` (`sdk/rocketnet_counters.cpp:9`),
`uint64_t rocketnet_counter_value(const struct rocketnet_counters_t * counters, int counter)`
(`sdk/rocketnet_counters.cpp:11-15`), and `const char * rocketnet_counter_name(int counter)`
(`sdk/rocketnet_counters.cpp:17-225`).

### 4.2 What Remains Open at Commit `2507f11`

1. **Debug-mode abort on out-of-range `rocketnet_counter_name` (`sdk/rocketnet_counters.cpp:221-224`)**:
   - `rocketnet_counter_name` executes `rocketnet_assert( 0 ); return "???";`
     (`sdk/rocketnet_counters.cpp:222-223`), aborting debug builds when a caller
     probes `< 0` or `>= ROCKETNET_NUM_COUNTERS`.
2. **Non-counter state inside `struct rocketnet_counters_t` (`sdk/rocketnet_counters.h:352-362`)**:
   - `struct rocketnet_counters_t` contains `uint64_t values[ROCKETNET_NUM_COUNTERS]`
     (`:354`) **plus** `uint64_t handshakes_processed_this_update` (`:361`),
     which is mutable per-update server budget state and not a counter value.
3. **No public C snapshot helper for opaque `rocketnet_counters_t`**:
   - External callers that only include `sdk/rocketnet.h` have no batch copy
     function to snapshot counters without looping `rocketnet_counter_value` 200 times.

### 4.3 Counter Design and Remediation

1. **Single-source X-macro table (`ROCKETNET_COUNTERS_TABLE`)**: Define the
   counter list once in `sdk/rocketnet_counters.h` to generate both the enum and
   `rocketnet_counter_name`, and resolve the `CONNECTION_RESPONSE_WRONG_SIZE`
   count in the native API.
2. **Keep `struct rocketnet_counters_t` pure**: Move
   `handshakes_processed_this_update` (`sdk/rocketnet_counters.h:361`) onto
   `struct rocketnet_server_protocol_t` (`sdk/rocketnet_server_protocol.h`), and
   pin `offsetof(rocketnet_counters_t, values) == 0` and
   `sizeof(rocketnet_counters_t) == ROCKETNET_NUM_COUNTERS * sizeof(uint64_t)`.
3. **Add `rocketnet_counters_snapshot` and fix `rocketnet_counter_name` bounds**:
   ```c
   ROCKETNET_EXPORT_FUNC int rocketnet_counters_snapshot(
       const struct rocketnet_counters_t * counters,
       uint64_t * out_values, int max_counters );
   ```
   Copies `min(max_counters, ROCKETNET_NUM_COUNTERS)` values into `out_values`
   (or returns `0` if `counters == NULL || out_values == NULL || max_counters <= 0`),
   and removes `rocketnet_assert(0)` from `rocketnet_counter_name` (`sdk/rocketnet_counters.cpp:222`).

### 4.4 Where This Design Meets `docs/ABI-EVOLUTION.md` (`issue-149`)

1. **Installed headers and `sizeof(rocketnet_counters_t)` (`CMakeLists.txt:331-333`)**:
   `CMakeLists.txt:332` installs `sdk/rocketnet_counters.h`. Gating the
   `struct rocketnet_counters_t` body behind `#ifdef ROCKETNET_IMPLEMENTATION`
   (leaving the `ROCKETNET_COUNTER_*` enum public and using `rocketnet_counters_snapshot`
   / `rocketnet_counter_value` across the ABI) prevents new counters from
   changing any public struct size.

---

## 5. THE LAYERS

| Layer | Name | Files Owned | What It Guarantees | How It Is Checked |
| :--- | :--- | :--- | :--- | :--- |
| **Layer 1** | Counter Table & C API | `sdk/rocketnet_counters.h`, `sdk/rocketnet_counters.cpp`, `sdk/rocketnet_server_protocol.h`, `sdk/rocketnet_server_protocol.cpp`, `sdk/rocketnet.h`, `tools/exports.txt` | Single `ROCKETNET_COUNTERS_TABLE` generates the C enum and `rocketnet_counter_name`; `handshakes_processed_this_update` moves to `rocketnet_server_protocol_t`; `rocketnet_counter_name` never asserts on out-of-range indices; `rocketnet_counters_snapshot` copies bounded prefixes. | `test_unit_counters.cpp` in `./bin/test` (bijective `0 .. rocketnet_num_counters() - 1` names, out-of-range bounds, snapshot checks, `sizeof(rocketnet_counters_t)`). |
| **Layer 2** | Preformatted Log Callback | `sdk/rocketnet.h`, `sdk/rocketnet.cpp`, `tools/exports.txt` | `log_quiet` and `log_level` use atomic loads before `vsnprintf`; `rocketnet_set_log_callback(cb, ctx)` delivers `(ctx, level, message, length)` with `message[length] == '\0'` and `length <= 1023` in one `vsnprintf` pass; `default_log_function` double-formatting is removed; `rocketnet_log_function` coexists. | `test_unit_logging.cpp` in `./bin/test` (level filtering before format, `log_quiet`, `context` forwarding, exact `length` and NUL termination at `0`, `1023`, `1024`, `2048`, sink switching, multi-thread logging). |
| **Layer 3** | Status Enum & Plumbing | `sdk/rocketnet.h`, `sdk/rocketnet.cpp`, `sdk/rocketnet_config.h`, `sdk/rocketnet_config.cpp`, `sdk/rocketnet_client_protocol.h`, `sdk/rocketnet_client_protocol.cpp`, `tools/exports.txt` | `enum rocketnet_status_t` (`0 .. ROCKETNET_NUM_STATUS_CODES - 1`), `rocketnet_status_string`, and `rocketnet_num_status_codes` are defined; internal config, ladder, and token helpers accept `int * status` and log at `ROCKETNET_LOG_LEVEL_ERROR` on every refusal. | `test_unit_status.cpp` in `./bin/test` verifying every status string, out-of-range `rocketnet_status_string` queries, and exact status codes from config, driver ladder, and connect-token validation. |
| **Layer 4** | Public `*_create_ex` Entry Points | `sdk/rocketnet.h`, `sdk/rocketnet.cpp`, `sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`, `sdk/rocketnet_frame_sync.cpp`, `tools/exports.txt` | `rocketnet_init_ex`, `rocketnet_term_ex`, `rocketnet_allocator_ex`, `rocketnet_client_transport_create_ex`, `rocketnet_server_transport_create_ex`, `rocketnet_frame_sync_create_ex`, and `rocketnet_message_validate_config_ex` report exact `rocketnet_status_t` codes on all branches (C1–C11, S1–S14, F1–F3) while keeping non-`_ex` signatures unchanged. | `test_lifecycle.cpp` and `test_unit_status.cpp` exercising every failure branch, including a fault-injecting allocator (`rocketnet_allocator_ex`) failing at each sequential allocation site to prove `ROCKETNET_STATUS_OUT_OF_MEMORY` and zero leaks at `rocketnet_term_ex`. |

---

## 6. QUESTIONS

1. **`Q1` (`_create_ex` additive exports vs replacing `_create` under an ABI bump)**:
   Section 2.3 recommends adding `rocketnet_*_create_ex(..., int * status)` and
   `rocketnet_init_ex(void * context, int * status)` while keeping the existing
   `rocketnet_*_create` and `rocketnet_init` signatures intact as `status = NULL`
   wrappers. Should we keep both forms, or add `int * status` directly to
   `rocketnet_client_transport_create`, `rocketnet_server_transport_create`, and
   `rocketnet_frame_sync_create` as part of a `ROCKETNET_ABI_VERSION` bump?
2. **`Q2` (Null-pointer arguments at public `*_create` boundaries)**:
   Today `rocketnet_client_transport_create` (`sdk/rocketnet_client_transport.cpp:157`)
   and `rocketnet_server_transport_create` (`sdk/rocketnet_server_transport.cpp:467`)
   call `rocketnet_assert` on `connect_token` and `public_address_string`
   (aborting in debug builds and dereferencing `NULL` in release builds). Should
   these public entry points replace those top-level assertions with runtime
   checks that log `rocketnet_error`, set `ROCKETNET_STATUS_INVALID_ARGUMENT`,
   and return `NULL` in both debug and release builds?
3. **`Q3` (Status code granularity)**:
   Does the 23-code `rocketnet_status_t` enum in Section 2.1 match the desired
   granularity, or should OS primitive failures (`MUTEX_CREATE_FAILED`,
   `THREAD_CREATE_FAILED`, `TIMER_CREATE_FAILED`) collapse into a single
   `ROCKETNET_STATUS_PLATFORM_ERROR`?
4. **`Q4` (Out-of-range return value for `rocketnet_counter_name` and `rocketnet_status_string`)**:
   When `rocketnet_counter_name(counter)` or `rocketnet_status_string(status)`
   is called with an out-of-range integer (with `rocketnet_assert(0)` removed so
   debug builds do not abort), should it return `NULL` or the literal `"???"`
   currently returned in release builds (`sdk/rocketnet_counters.cpp:223`)?
5. **`Q5` (`struct rocketnet_counters_t` visibility in installed `sdk/rocketnet_counters.h`)**:
   `CMakeLists.txt:332` installs `sdk/rocketnet_counters.h`. Should
   `struct rocketnet_counters_t` (`sdk/rocketnet_counters.h:352-362`) be gated
   behind `#ifdef ROCKETNET_IMPLEMENTATION` so external C consumers rely on the
   opaque pointer plus `rocketnet_counter_value` / `rocketnet_counters_snapshot`
   (preventing new counters from changing a public struct size), or remain
   publicly defined?
6. **`Q6` (Timing of `rocketnet_set_log_callback` registration)**:
   Should `rocketnet_set_log_callback` allow live replacement at any time via the
   atomic spin lock in Section 3.2, or restrict registration to when no
   transports are active?

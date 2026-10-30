<a id="structured-errors-ffi-logging-and-runtime-counters"></a>

# Troubleshooting, errors, and logging

Start with the failed operation and its result. A synchronous status, a client
state observed during updates, a server disconnect reason, and a statistics
counter describe different events. Keep the status returned by the failing
call; a successful create status cannot explain a later failed connect.

| Symptom | Recovery guide |
| --- | --- |
| Create returns NULL, or connect/start refuses | [Create, connect, and start](#51-create-connect-and-start-refusals) |
| Connect accepted, but the handshake never completes | [Tokens, filters, and transport mismatches](#52-token-filter-transport-mismatch-and-handshake-failures) |
| Cannot queue a message | [Backpressure and send refusals](#53-refused-sends-can_send_message-vs-failed-start_message-full-queues-and-wire-budgets) |
| Connection ends with MESSAGE_NOT_RECEIVED | [Drain and buffer lifetime](#54-receive-drain-misuse-message_not_received-and-batch-lifetime) |
| Reconnect refuses or the client has a negative state | [Reconnect](#55-reconnect-negative-client-states-and-session-isolation) |
| Disconnect/stop has not finished | [Shutdown and context lifetime](#56-pending-disconnectstop-and-safe-destroy-allocator-and-context-lifetime) |

The [public header](../sdk/rocketnet.h) defines every result and callback
contract. After the recovery guide, this page explains the reporting, logging,
and counter interfaces. [ABI evolution](ABI-EVOLUTION.md) covers binary compatibility.

<a id="5-troubleshooting-guide"></a>

## Troubleshooting

This section maps public status codes (`rocketnet_status_t`), client states
(`rocketnet_client_state_t`), disconnect reasons
(`rocketnet_disconnect_reason_t`), and statistics counters
(`rocketnet_client_stats_t`, `rocketnet_server_stats_t`,
`rocketnet_server_client_stats_t`) to their root causes and bounded recovery
actions.

<a id="51-create-connect-and-start-refusals"></a>

### Create, Connect, and Start Refusals

When `rocketnet_client_create` or `rocketnet_server_create` returns `NULL`
(writing the reason to `*status`), or `rocketnet_client_connect` /
`rocketnet_server_start` returns a negative `rocketnet_status_t`, inspect the
status code and the accompanying `ERROR` log line:

| Status code (`rocketnet_status_name`) | Where returned | Root cause | Bounded recovery action |
| --- | --- | --- | --- |
| `NOT_INITIALIZED` | `create`, `term` | `rocketnet_init` was not called before `create` / `term`. A create before init increments `api_misuses`. | Call `rocketnet_init()` once at process startup before creating any endpoint, and `rocketnet_term()` once after all endpoints are destroyed. |
| `CONFIG_NOT_INITIALIZED` | `create`, `required_memory` | `config->config_bytes` is `0` or smaller than `sizeof(size_t)` (see [sized configs](ABI-EVOLUTION.md)). | Initialize with `rocketnet_client_default_config(&cfg)` / `rocketnet_server_default_config(&cfg)` (or the light function) before modifying fields. |
| `CONFIG_ENV_INVALID` | `create` (`required_memory` returns `0`) | A `ROCKETNET_*` environment variable failed strict syntax parsing (whitespace, `+` prefix, non-decimal characters, integer overflow, or wrong base64 key/filter length). | Read the `ERROR` line naming the variable, fix or unset it (`docs/ENVIRONMENT.md`), and recreate the endpoint. Empty variables are ignored and logged. |
| `CONFIG_OUT_OF_RANGE` | `create` (`required_memory` returns `0`) | A field is outside its public-header range or violates a cross-field invariant (`max_message_bytes > receive_message_buffer_bytes`, `packets_in_flight < reliable_messages_in_flight`, or simulator settings in `ROCKETNET_DEV=0`). | Read the field and rule from the `ERROR` line and adjust the configuration using [`docs/LOAD.md`](LOAD.md). Values are never silently clamped. |
| `MEMORY_BUDGET_EXCEEDED` | `client` / `server` create (`required_memory` returns `0`) | Transport fragment reassembly exceeds the build's `ROCKETNET_REASSEMBLY_MEMORY_MIB` cap. The operator `ROCKETNET_*_MEMORY_BUDGET_BYTES` values do not cap create-time endpoint/message memory. | Lower server `max_clients` or either endpoint's `max_fragmented_messages`, or rebuild with a deliberately qualified reassembly cap. Use `rocketnet_*_required_memory` to provision the caller allocator. |
| `INVALID_ADDRESS` | `rocketnet_server_create` | `ipv4_address` or `ipv6_address` (or its environment override) could not be parsed as its address family (`a.b.c.d:port` or `[v6]:port`). | Pass a valid address string with port for at least one family. |
| `TRANSPORT_PORT_ZERO_UNSUPPORTED` | `rocketnet_server_start` | The address specifies port `0` on a server transport that cannot discover the kernel-assigned port (such as `xdp`). | Bind an explicit UDP port (`1 .. 65535`) or include a transport that discovers its assigned port. |
| `CONNECT_TOKEN_PORT_ZERO` | `rocketnet_client_connect` | The token advertises a server address with port `0`. | Fix the token issuer so each advertised address carries the server's bound non-zero port. |
| `OUT_OF_MEMORY`, `MUTEX_CREATE_FAILED`, `THREAD_CREATE_FAILED`, `TIMER_CREATE_FAILED` | `create`, `connect`, `start` (`THREAD_CREATE_FAILED`: `connect`) | The caller allocator returned `NULL` or the OS refused a mutex, thread, or high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, required by Windows frame sync without coarse fallback). | Provision the full `rocketnet_*_required_memory` result and verify OS thread/descriptor/timer support (`ulimit -u`, `ulimit -n`). Do not retry in an unbounded loop. |

<a id="52-token-filter-transport-mismatch-and-handshake-failures"></a>

### Token, Filter, Transport Mismatch, and Handshake Failures

1. **Production build guards (`ROCKETNET_DEV=0`)**:
   - `ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION`: returned at
     `rocketnet_client_create` and `rocketnet_server_create` (and causes
     `rocketnet_*_required_memory` to return `0`) when `config.packet_filter`
     is set to the published four-byte test filter
     (`b7:59:01:00`, `ROCKETNET_TEST_FILTER_VALUE`) in a production library
     (`ROCKETNET_DEV=0`). That is the default value, so a config that is never
     given a title filter is refused; an all-zero filter is not the test
     filter and is not refused by this rule.
     **Action**: set a non-test four-byte `packet_filter` in `config.packet_filter`
     (or `ROCKETNET_CLIENT_PACKET_FILTER` / `ROCKETNET_SERVER_PACKET_FILTER`)
     identically on both client and server before calling
     `rocketnet_*_required_memory` or `rocketnet_*_create`.
   - `ROCKETNET_STATUS_MISSING_KEYS`: `config.backend_public_key` is all-zero
     when `rocketnet_server_start` validates the server's keys.
     **Action**: configure the backend's 32-byte Ed25519 public key in
     `config.backend_public_key` or `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`.
   - `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION` and
     `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION`: a production
     build (`ROCKETNET_DEV=0`) refused the published development backend key or
     a connect token sealed to the development server key.
     **Action**: generate production backend signing keys, read the server's
     runtime X25519 public key via `rocketnet_server_public_key(server, pub)`,
     and mint connect tokens with those production keys.
2. **Connect token refusals (`INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`)**:
   - If `rocketnet_client_connect` returns `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN`
     or `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED`, inspect
     `rocketnet_client_state(client)`. A decoded token refused by the transport
     sets the corresponding negative terminal state; a malformed base64 token
     string is rejected before an attempt starts and preserves the prior state.
   - **Action**: never re-submit the same invalid or expired token. Request a
     fresh connect token from the backend and verify that client and server
     wall clocks (or `ROCKETNET_CLIENT_TOKEN_EPOCH_UNIX_SECONDS` and
     `ROCKETNET_SERVER_TOKEN_EPOCH_UNIX_SECONDS`) are synchronized.
3. **Transport ladder and host capability failures (`INVALID_TRANSPORT_LIST`, `TRANSPORT_NOT_BUILT`, `TRANSPORT_FAMILIES_UNSUPPORTED`, `TRANSPORT_LADDER_EXHAUSTED`)**:
   - `INVALID_TRANSPORT_LIST`: `config.transport` (or
     `ROCKETNET_*_TRANSPORT`) contains an unknown transport name, an empty
     comma-separated token, or mixes `"auto"` with other names (`"auto"` must
     stand alone).
   - `TRANSPORT_NOT_BUILT`: a pinned transport (such as `"xdp"` or
     `"io_uring"`) was not compiled into the binary (`docs/LINUX.md`).
   - `TRANSPORT_FAMILIES_UNSUPPORTED`: the selected transport cannot
     serve the configured IPv4/IPv6 family combination.
   - `TRANSPORT_LADDER_EXHAUSTED`: every candidate in the resolved
     transport ladder failed host preflight, socket bind, or the operator
     transport memory budget (`ROCKETNET_*_MEMORY_BUDGET_BYTES`, which checks
     `allocator_bytes + umem_bytes` per candidate, not the create-time block).
   - **Action**: inspect the `WARN` / `ERROR` log lines emitted by each
     candidate during `start` or `connect`. When troubleshooting `xdp` on
     Linux, preserve the pinned geometry: 4,096-byte frames, a 4,096-entry send
     queue, 16,384-entry receive queue and fill set, a 32,768-entry fill ring,
     and 49,152 UMEM frames per queue. Use only the supported runtime controls
     (`num_xdp_queues`, `num_os_cpus`, and
     `ROCKETNET_GATEWAY_ETHERNET_ADDRESS`). The XDP data plane requires
     effective UID 0; Linux capabilities alone do not satisfy that preflight.
     Then verify provisioning (`CAP_NET_ADMIN`, `CAP_BPF`, `RLIMIT_MEMLOCK`),
     NIC queue count, and memory budget, or include
     `"gso,io_uring,portable"` / `"auto"` so the server can fall back cleanly.
4. **Handshake stage timeouts, denials, and protocol/config mismatches**:
   - Observe `rocketnet_client_state(client)` on each frame after
     `rocketnet_client_update(client, time)` and compare with
     `rocketnet_server_stats(server, &s)`:
     - `ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT` (`-2`): the client sent
       cookie requests (`REQUESTING_COOKIE`) and received no reply. Check:
       (a) server is running (`rocketnet_server_running(server) == true`) and
       reachable on the UDP port named in the token; (b) `packet_filter` bytes
       match on client and server (a mismatch causes the server prefilter to
       drop incoming packets silently); (c) `protocol_id` in the token matches
       `server_config.protocol_id`; (d) the server public key sealed into the
       token matches `rocketnet_server_public_key(server, ...)`; and (e) the
       server's `backend_public_key` matches the backend key that signed the
       token.
     - `ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT` (`-3`) or
       `ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT` (`-4`): check
       `s.handshakes_dropped_over_budget` in `rocketnet_server_stats_t` (which
       increments when a connection request waits past the server's
       `handshakes_per_update` budget; raise it if a mass join times out) and
       check path MTU / packet loss.
     - `ROCKETNET_CLIENT_STATE_CONNECTION_DENIED` (`-1`): the server explicitly
       rejected the connection. Check `s.connections_denied_full` in
       `rocketnet_server_stats_t` (all `max_clients` slots are occupied) and
       verify that the connect token was not already consumed by another
       connection or a duplicate `client_id`.
     - `ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH` (`-11`) /
       `ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH`: client and server
       configurations disagree on protocol geometry (such as `num_channels`).
       Align `rocketnet_client_config_t` and `rocketnet_server_config_t`.

<a id="53-refused-sends-can_send_message-vs-failed-start_message-full-queues-and-wire-budgets"></a>

### Refused Sends, `can_send_message` vs. Failed `start_message`, Full Queues, and Wire Budgets

Understanding the difference between `rocketnet_*_can_send_message` and
`rocketnet_*_start_message` is critical for avoiding self-inflicted
disconnects under load:

1. **Why calling `start_message` blindly on a full send region disconnects the session**:
   - Each connection operates inside a fixed, pre-allocated connection region
     (`connection_bytes`, [`docs/LOAD.md`](LOAD.md)).
   - Any send (reliable, or unreliable that finds no room in the send region)
     that finds no message memory ends the connection with
     `MESSAGE_MEMORY_EXHAUSTED` and returns `NULL`: if a connected caller calls
     `rocketnet_client_start_message` or `rocketnet_server_start_message` when
     the connection's send descriptor pool (`send_queue_messages`), send store
     (`send_message_buffer_bytes`), reliable sliding window
     (`reliable_messages_in_flight`), or sent-packet ring
     (`packets_in_flight`) cannot accommodate the message (for example, when
     messages accumulate faster than ACKs arrive or `send_kbps` /
     `per_client_send_kbps` paces them out), the message layer treats region
     exhaustion on an attempted `start_message` as fatal to that connection
     (`proofs/tla/RNConnectionMemory.tla` `Kick`, requirement `R5`): `start_message`
     returns `NULL` and **immediately disconnects that connection** with
     `ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED` (transitioning
     the client to `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED`, `-12`).
2. **How `can_send_message` prevents `MESSAGE_MEMORY_EXHAUSTED`**:
   - `rocketnet_client_can_send_message(client, channel, reliability, max_bytes)`
     and `rocketnet_server_can_send_message(server, client_index, channel, reliability, max_bytes)`
     execute the exact same room, window, per-update cap, and wire-budget check
     (`rn_message_object_send_room`) as `start_message`, but **purely (`O(1)`,
     zero state mutation, zero drops, no disconnect)**.
   - When `can_send_message` returns `true`, `start_message` with the same
     `(channel, reliability, max_bytes)` arguments is given room provided no
     intervening start or update changes that single-thread-owned object.
   - When `can_send_message` returns `false`, the connection remains healthy
     and connected.
3. **Other reasons `start_message` returns `NULL` (and how they are counted)**:
   - **Endpoint or slot not connected**: calling `start_message` when the
     client is not `ROCKETNET_CLIENT_STATE_CONNECTED`, when the server is
     stopped, or when `client_index` (`0 .. max_clients - 1`) has no connected
     client returns `NULL` quietly without logging or counting `api_misuses`,
     and increments `messages_dropped_not_connected` in
     `rocketnet_client_stats_t` / `rocketnet_server_stats_t`.
   - **Unreliable send over wire budget**: a fitting unreliable start may use
     the preallocated queue when it cannot spend budget immediately. If an
     unreliable packet cannot spend budget when sealed, it is dropped and
     `messages_dropped_over_budget` increments without disconnecting the
     session. Reliable messages wait for budget instead of being dropped.
   - **Refused send arguments**: an invalid channel, reliability, or size, a
     second start while one is open, or an invalid `bytes_written` is logged
     and refused but is not counted in `api_misuses`. A NULL object or a server
     destination outside `0 .. max_clients - 1` (including
     `ROCKETNET_FLOW_ID_BIT`) is API misuse. See the complete list in the public
     header's "API misuse" section.
4. **Safe action path for sends**:
   - Gate discretionary or bursty sends with `can_send_message` before calling
     `start_message`.
   - Always check the `uint8_t *` pointer returned by `start_message`. If it is
     `NULL`, do **not** write payload bytes and do **not** call
     `finish_message`: calling `finish_message` without a live reservation
     (including after `start_message` returned `NULL` or after a disconnect or
     slot reset cancelled the reservation) asserts in a debug build and returns
     `0` safely in a release build. When a reservation is open, a
     `finish_message` with `bytes_written < 1` or `bytes_written > max_bytes`
     aborts the open start, logs an `ERROR`, and returns `0`.
   - If `can_send_message` returns `false` or `start_message` returns `NULL`,
     **never retry in a tight same-frame loop**: queue space and send budgets
     only replenish when a subsequent frame runs `update` (advancing monotonic
     `time` and processing ACKs) and `send_packets`. Defer the message to a
     later tick, coalesce state updates, or adjust `send_queue_messages`,
     `send_message_buffer_bytes`, `reliable_messages_in_flight`, `packets_in_flight`,
     `packets_per_update`, `unreliable_messages_per_update`, and
     `send_kbps` / `per_client_send_kbps` ([`docs/LOAD.md`](LOAD.md)).

<a id="54-receive-drain-misuse-message_not_received-and-batch-lifetime"></a>

### Receive-Drain Misuse (`MESSAGE_NOT_RECEIVED`) and Batch Lifetime

1. **Root cause of `MESSAGE_NOT_RECEIVED`**:
   - During `rocketnet_client_update` / `rocketnet_server_update`, incoming
     messages are placed into the endpoint-wide delivery queue.
   - Each call to `rocketnet_client_receive_messages` or
     `rocketnet_server_receive_messages` releases the message views held by the
     previous batch and fills `rocketnet_message_batch_t` with up to
     `ROCKETNET_RECEIVE_BATCH_MESSAGES` messages, returning `NULL` when
     the queue is empty.
   - If the caller does not drain `receive_messages` until it returns `NULL`
     before the next `update` call (for example, calling `receive_messages`
     only once when more than one batch arrived, or skipping the
     receive step during a frame), the next `update` detects held undrained
     messages (`proofs/tla/RNSlots.tla`), logs an `ERROR`, increments `api_misuses`,
     and disconnects the offending connection with
     `ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED` (client state
     `ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED`, `-13`).
2. **Safe action path**:
   - Follow the five-step frame order on every tick:
     1. `update(endpoint, time)`
     2. `while ((batch = receive_messages(endpoint)) != NULL) { ... }`
     3. game simulation
     4. `can_send_message` -> `start_message` -> write -> `finish_message` (or `abort_message`)
     5. `send_packets(endpoint)`
   - Never retain `batch` or `batch->data[i]` pointers past the next
     `receive_messages`, `update`, `disconnect`, `stop`,
     or `destroy` call; copy any payload bytes that must outlive the current
     batch iteration.
   - Check `receipts_dropped` in `rocketnet_client_stats_t` /
     `rocketnet_server_stats_t`; if positive, increase `max_message_receipts`
     so receipt events between frames do not overflow the receipt ring.

<a id="55-reconnect-negative-client-states-and-session-isolation"></a>

### Reconnect, Negative Client States, and Session Isolation

1. **Observing negative terminal states (`rocketnet_client_state_t < 0`)**:
   - When a client handshake fails or an established connection terminates
     (`SERVER_DISCONNECTED`, `RELIABLE_TIMED_OUT`, `PROTOCOL_VIOLATION`,
     `CONFIG_MISMATCH`, `MESSAGE_MEMORY_EXHAUSTED`, `MESSAGE_NOT_RECEIVED`,
     `CONNECT_TOKEN_EXPIRED`, `INVALID_CONNECT_TOKEN`, `CONNECTION_TIMED_OUT`,
     `CONNECTION_RESPONSE_TIMED_OUT`, `CONNECTION_REQUEST_TIMED_OUT`,
     `COOKIE_REQUEST_TIMED_OUT`, `CONNECTION_DENIED`),
     `rocketnet_client_state(client)` remains in that negative state across
     subsequent `rocketnet_client_update` calls until the caller explicitly
     calls `rocketnet_client_disconnect(client)` (which resets the state to
     `ROCKETNET_CLIENT_STATE_DISCONNECTED`, `0`) or starts a new connection
     with `rocketnet_client_connect(client, new_token)`.
2. **Handling `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` during reconnect**:
   - Calling `rocketnet_client_connect` while `rocketnet_client_state(client) > ROCKETNET_CLIENT_STATE_DISCONNECTED`
     (an active handshake or `CONNECTED` session) is an API misuse: it logs an
     `ERROR`, increments `api_misuses`, and returns
     `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED`.
   - Calling `rocketnet_client_connect` from a **negative** terminal state (or
     after `rocketnet_client_disconnect`) is valid, but if the previous
     connection's background transport worker has not yet finished exiting
     (`rocketnet_client_disconnected(client) == false`),
     `rocketnet_client_connect` initiates retirement of the old worker, keeps
     the negative public state visible, wipes the temporary token buffer, and
     returns `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` without counting
     `api_misuses` (`proofs/tla/ClientSession.tla` `RetryPending`).
   - **Safe reconnect sequence**:
     1. Obtain a **fresh** connect token from the backend (a connect token is
        single-use once consumed by a server).
     2. Call `rocketnet_client_disconnect(client)` and on each normal game
        frame run `rocketnet_client_update(client, time)` until
        `rocketnet_client_disconnected(client)` returns `true`.
     3. Once `rocketnet_client_disconnected(client)` is `true`, call
        `rocketnet_client_connect(client, fresh_token)`. Never spin in a
        blocking same-frame retry loop on `CLIENT_NOT_DISCONNECTED`.
3. **Generational isolation and lazy server slot reset**:
   - **Client side**: when `rocketnet_client_disconnect(client)` runs and moves
     the old connection to retirement, `rocketnet_client_stats` immediately
     reads `0` for session counters
     (`packets_sent`, `packets_received`, `bytes_sent`, `bytes_received`,
     `payloads_received`, `messages_resent`, `messages_duplicated`,
     `receipts_acked`, `receipts_lost`, `receipts_dropped`, and
     `messages_dropped_over_budget`) as well as RTT, jitter, packet loss, and
     trailing rates, while `messages_dropped_not_connected` remains visible.
     When `rocketnet_client_disconnected(client)` becomes `true` and the next
     `rocketnet_client_connect` runs (`rn_client_end_connection`), only
     `messages_dropped_not_connected` is carried forward onto the client handle
     (`client->lifetime[RN_CLIENT_NOT_CONNECTED_DROPS]`); the retired
     connection's session counters are not folded into handle totals, the old
     message/transport objects are destroyed, and the new session is constructed
     fresh inside `client->block` with zeroed session counters and new keys,
     sequence numbers, and reliability windows.
   - **Server side**: when a client disconnects or is kicked via
     `rocketnet_server_disconnect_client(server, client_index)`,
     `rocketnet_server_client_connected(server, client_index)` immediately
     reads `false` and `rocketnet_server_disconnect_reason(server, client_index)`
     reports the terminal `rocketnet_disconnect_reason_t`
     (`ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED` for a kick,
     `ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED` (`-8`) for the client's
     own disconnect, `ROCKETNET_DISCONNECT_REASON_TIMED_OUT` (`-7`) for a
     client that went silent past its connection timeout, or a message-layer
     reason). At the start of the
     next `rocketnet_server_update`, before that slot index can be assigned to
     any incoming connection (`proofs/tla/RNSlots.tla` `ResetFree`,
     `proofs/tla/RNConnectionMemory.tla` `Reuse`), the server lazily resets the
     slot's connection region and per-slot statistics
     (`rocketnet_server_client_stats_t`), ensuring zero state or packet leakage
     between sessions.

<a id="56-pending-disconnectstop-and-safe-destroy-allocator-and-context-lifetime"></a>

### Pending Disconnect/Stop and Safe Destroy, Allocator, and Context Lifetime

1. **Asynchronous shutdown and completion polling (`rocketnet_client_disconnected`, `rocketnet_server_stopped`)**:
   - `rocketnet_client_disconnect(client)` and `rocketnet_server_stop(server)`
     request shutdown without blocking the game thread.
   - While shutdown is in progress:
     - `rocketnet_client_disconnected(client)` / `rocketnet_server_stopped(server)`
       returns `false`;
     - `rocketnet_server_start(server)` refuses to restart with
       `ROCKETNET_STATUS_SERVER_NOT_STOPPED`.
   - Continue advancing the frame loop (`rocketnet_client_update(client, time)`
     for retiring client transports) and poll
     `rocketnet_client_disconnected(client)` or `rocketnet_server_stopped(server)`
     until it returns `true` before calling `rocketnet_client_destroy(client)`,
     `rocketnet_server_destroy(server)`, or `rocketnet_server_start(server)`.
2. **Why early `destroy` is refused and retains memory if a worker is still live**:
   - Calling `rocketnet_client_destroy` when `!rocketnet_client_disconnected(client)`
     or `rocketnet_server_destroy` when `!rocketnet_server_stopped(server)`
     triggers a debug `rocketnet_assert`, increments `api_misuses`, logs an
     `ERROR`, and attempts at most `32` non-sleeping transport updates
     (`RN_DESTROY_STOP_UPDATES`).
   - If the transport worker thread has still not exited after those 32 bounded
     updates, **`destroy` intentionally leaves the allocation alive and returns
     without calling `rocketnet_free`** (`"the worker has not exited; the client/server is kept, not freed"`). Freeing an arena while a background
     worker thread still holds pointers into it would cause a use-after-free
     or memory corruption.
3. **Preserving endpoint, allocator, and callback context ownership through `destroy` and `rocketnet_term`**:
   - Keep the endpoint handle, the `context` pointer passed to `create` /
     `rocketnet_init`, the custom allocator backing store, the
     `state_change_callback` context, and the `rocketnet_log_function` context
     valid and in scope until:
     1. `rocketnet_client_disconnected(client)` / `rocketnet_server_stopped(server)`
        returns `true`;
     2. `rocketnet_client_destroy(client)` / `rocketnet_server_destroy(server)`
        returns; and
     3. `rocketnet_term()` returns `ROCKETNET_STATUS_OK`.
   - If `rocketnet_term()` returns `ROCKETNET_STATUS_LIVE_ALLOCATIONS`,
     one or more endpoints were not destroyed (or an early `destroy` retained a
     still-running worker). Do not tear down or unmap the custom allocator heap
     when `LIVE_ALLOCATIONS` is returned; always complete the documented
     `disconnect`/`stop` -> poll `disconnected`/`stopped` -> `destroy` ->
     `rocketnet_term` sequence first.

<a id="1-the-state"></a>

## Public failure reporting

<a id="11-public-lifecycle-and-creation-entry-points"></a>

### Lifecycle and creation entry points

The public header is `sdk/rocketnet.h`. A game initializes the library, fills a
client or server config, creates one client or server, and checks the result.
Refusals have the return behavior documented by each declaration in
`sdk/rocketnet.h`; only the specific caller errors listed under “API misuse”
there increment the process-wide `api_misuses` field. A refusal is not
automatically a misuse, and not every refusal logs an error. For example,
invalid send arguments return `NULL` or `0` and log, but do not increment
`api_misuses`. A message send on a connection that is not CONNECTED is a normal
drop: start returns `NULL`, logs nothing, and increments
`messages_dropped_not_connected`, never the misuse count. Do not call finish
after that refused start; finishing without a live reservation is a separate
application error.

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

Internal helper names in this reference describe SDK implementation paths;
applications call only declarations in [rocketnet.h](../sdk/rocketnet.h).

<a id="2-status-codes"></a>

## Status code reference

<a id="21-status-code-enum-rocketnet_status_t"></a>

### Status codes and return forms

Status codes follow these rules:

1. `ROCKETNET_STATUS_OK = 0` is the sole success value.
2. Every failure code is negative, dense from `-1` to `-34`. `status < 0`
   means the call failed.
3. There is no count value. `rocketnet_status_name` maps each code by value.
   It returns a static ASCII identifier, for example `"INVALID_CONNECT_TOKEN"`,
   and `"UNKNOWN"` for any other value, never `NULL` and without asserting.

The complete enum and numeric values live in [rocketnet.h](../sdk/rocketnet.h).
Use symbolic constants in code and `rocketnet_status_name` in diagnostics.
The tables here explain recovery; they do not duplicate the enum declaration.

Lifecycle calls use `enum rocketnet_status_t`. Other operations use their
documented handle, boolean, count, or output result:
`rocketnet_init` reports `INVALID_ARGUMENT`, `ALREADY_INITIALIZED`,
`LIVE_ALLOCATIONS`, `PLATFORM_INIT_FAILED` or `CRYPTO_INIT_FAILED`.
`rocketnet_term` reports `NOT_INITIALIZED` or `LIVE_ALLOCATIONS`. The client and server create
calls return a handle or `NULL`, write the code through the status
out-parameter, and log one `ERROR` line with the reason: `NOT_INITIALIZED`,
`CONFIG_NOT_INITIALIZED`, `INVALID_ARGUMENT`, `CONFIG_ENV_INVALID`,
`CONFIG_OUT_OF_RANGE`, `MEMORY_BUDGET_EXCEEDED`, `INVALID_TRANSPORT_LIST`,
`LOOPBACK_HUB_MISSING`, `LOOPBACK_HUB_MISMATCH`, `INVALID_ADDRESS` (server) or
`OUT_OF_MEMORY`. Client connect and server start
return the token and transport codes: `CLIENT_NOT_DISCONNECTED`,
`INVALID_CONNECT_TOKEN`, `CONNECT_TOKEN_EXPIRED`, `CONNECT_TOKEN_PORT_ZERO`, `SERVER_NOT_STOPPED`,
`TRANSPORT_LADDER_EXHAUSTED`, `TRANSPORT_FAMILIES_UNSUPPORTED`, `TRANSPORT_NOT_BUILT`, `MISSING_KEYS`,
`TRANSPORT_PORT_ZERO_UNSUPPORTED`, `MUTEX_CREATE_FAILED`,
`THREAD_CREATE_FAILED` and `TIMER_CREATE_FAILED`. Server start reports a
transport whose thread the platform refused as `TRANSPORT_LADDER_EXHAUSTED`;
`THREAD_CREATE_FAILED` comes from client connect.
`ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED` is -26: a transport returns it
when it cannot serve both named families, and start returns it when every
candidate that opened refused for that reason.
`ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED` is -27: a config whose
`config_bytes` is 0 or smaller than the field itself, one that skipped its
default_config call, is refused with it before any other check
(`docs/ABI-EVOLUTION.md`). Its string is `CONFIG_NOT_INITIALIZED`
(`rocketnet_status_name` in `sdk/rocketnet.cpp`).
`ROCKETNET_STATUS_TRANSPORT_NOT_BUILT` is -28: start returns it
when every candidate that opened refused because it is compiled out of this
build, the `xdp` server transport in a Linux build configured with
`ROCKETNET_XDP=OFF` or without a clang that has the BPF target, libbpf or
libxdp (`docs/LINUX.md`). Its string is
`TRANSPORT_NOT_BUILT`.
`ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION` is -29: a production build
(`ROCKETNET_DEV=0`) refuses the published test packet filter at client and
server create. `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` is -30: a
production client refuses, at connect, a token sealed to the published
development server key. `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO` is -31:
every client refuses, at connect, a token that names a server address with
port 0, on every platform. No server can be reached at port 0: macOS refuses
the socket connect, and Linux and Windows accept it and the packets reach no
server. The client stays `DISCONNECTED`, and connect logs one `ERROR` line.
`ROCKETNET_STATUS_LOOPBACK_HUB_MISSING` is -32: client and server create
refuse a transport list that names `loopback` when the config's
`loopback_hub` is `NULL`. `ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH` is -33:
create and `rocketnet_loopback_hub_destroy` refuse a hub whose magic, version
or layout id differ from this library's, one made by another build.
`ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE` is -34, the last code:
`rocketnet_loopback_hub_destroy` refuses while a server or client endpoint is
registered with the hub, keeps the hub, logs one `ERROR` line and counts API
misuse.

<a id="22-reporting"></a>

### Reporting

Init and term return a status code directly (the SDK's
internal `_ex` forms write the same code to an `int *`). The client and server
create calls return a handle or `NULL` and write the refusal's code through the
final status out-parameter; their refusals are logged. A create before init is
counted in the process-wide `api_misuses` stat; other create refusals are not
automatically counted as misuse. `rocketnet_status_name` names any code. The internal transport layer
keeps `_ex` create forms for the SDK's own tests.

<a id="3-logging"></a>

## Logging

<a id="31-one-log-sink"></a>

### One log sink

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

<a id="32-callback-signature-and-contract"></a>

### Callback signature and contract

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

<a id="33-dispatch"></a>

### Dispatch

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

<a id="34-refused-thread-priority-calls"></a>

### Refused thread priority calls

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

<a id="4-counters"></a>

## Counters

<a id="41-the-stats-surface-a-game-uses"></a>

### The stats surface a game uses

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

Client packet, byte, payload, resend, duplicate, receipt and budget counts
describe only the connection the client currently holds. They start at zero
for each connection and read as zero without one, including after disconnect
while the ended connection is still retiring. Rates, RTT, jitter and loss have
the same session boundary. `messages_dropped_not_connected` is the exception:
it is a client-handle diagnostic and remains monotonic across connections.
Server aggregate counts are lifetime totals across starts and stops and begin
at zero at object creation. Process-wide `api_misuses` and
`thread_priority_refusals` do not reset with an object. Client and server rates
cover the trailing second and read as zero without an active connection or
while the server is stopped. Per-slot statistics describe the current
connection and reset when a slot is reused.

`rocketnet_client_receipts` and `rocketnet_server_receipts` copy outcomes from
the most recent update. Re-reading does not consume them; the copied entries
remain stable until the next update. If the ring fills, it drops the oldest
outcomes and increments `receipts_dropped`. A smaller output capacity copies
only the first entries that fit. A zero-filled receipt is NONE; ACKED and LOSS
are reported outcomes. See the function declarations in `sdk/rocketnet.h` for
NULL, free-slot and stopped-server behavior.

---

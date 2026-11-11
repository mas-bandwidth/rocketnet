# RocketNet v1.1.0

Changes compared with the `v1.0.0` tag. v1.0.1 was prepared but never tagged;
its fixes ship here, under "Fixes since v1.0.0".

- Tag: `v1.1.0`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.0).

## A bad network no longer ends a connection

- A send that finds no room is refused and never ends the connection: `rocketnet_client_start_message` and `rocketnet_server_start_message` return `NULL`, the refusal is counted for that connection, and the connection stays up. Unreliable sends are included. In v1.0.0 the same send ended the connection with `MESSAGE_MEMORY_EXHAUSTED` ([MESSAGES.md m1.7](../standard/MESSAGES.md#m1.7)).
- A refused start changes nothing a later start depends on: a start succeeds again once the receiver acknowledges and room returns. `rocketnet_client_can_send_message` and `rocketnet_server_can_send_message` answer as before, and what to do about refusals is the game's decision.
- `reliable_message_timeout_ms` is now 0 (off) by default, for the client and the server. In v1.0.0 it was 10000 and ended a connection whose oldest reliable message had waited 10 s since it was first sent, even during a short outage.
- To opt in, set `reliable_message_timeout_ms` (or `ROCKETNET_CLIENT_RELIABLE_MESSAGE_TIMEOUT_MS` / `ROCKETNET_SERVER_RELIABLE_MESSAGE_TIMEOUT_MS`) to a nonzero value: it is validated as before (at least 1000 and at least four initial resend delays) and behaves exactly as v1.0.0 did, outage or not. Choose it well above the connection timeout; a value equal to it can end a connection before the timeout.
- With the default, a connected client survives a total outage in either or both directions shorter than the connection timeout T (the connect token's `timeout_seconds`, default 10 s, at most the server's `max_timeout_seconds`), and afterwards every reliable message is delivered exactly once and in order.
- That guarantee needs both ends at v1.1.0 or later, `reliable_message_timeout_ms` left 0, and a round trip of at most 900 ms. T of 10, 30 and 60 s were tested. An outage that starts just as the server accepts the client, before the client knows, costs one extra round trip of margin.
- With the default, the wait for the other side's CONFIG record has no clock of its own; only silence for T ends it. Opted in, it keeps v1.0.0's reliable timeout.
- Probe pacing: with the default, once 1 s passes with no reliable message newly acknowledged, the sender resends only the oldest unacknowledged message, once per resend delay, instead of every due message. The first new acknowledgement ends the hold, and the held messages are then sent at once within the send budget. Opted in, there is no probe pacing ([MESSAGES.md m9.3](../standard/MESSAGES.md#m9.3)).
- With the default, a packet sent before a silence of 1 s or more in the acknowledgements (less one smoothed round trip) gives no round-trip sample, so an outage does not inflate the round-trip estimate ([MESSAGES.md m9.2](../standard/MESSAGES.md#m9.2)).
- The client's 30 s connect timeout (the connect token's lifetime) now applies only until the client sends its connection response. From then the server may already hold the client connected, so only silence for T ends the attempt (`CONNECTION_RESPONSE_TIMED_OUT`). When T is longer than 30 s, a failed attempt can now take up to T to report instead of 30 s ([TRANSPORT.md t11](../standard/TRANSPORT.md#t11)).
- New: `struct rocketnet_conditions_t`, read with `rocketnet_client_conditions` and `rocketnet_server_client_conditions` (per client). It reports seconds since the other side was last heard, seconds since reliable progress, the reliable messages in flight and the starts refused for no room. Pass `sizeof` your struct: fields are only ever appended. Both return false and write zeroes when not connected.
- `MESSAGE_MEMORY_EXHAUSTED` (and client state `MESSAGE_MEMORY_EXHAUSTED`) now means only that the receive side broke its own bound, a delivery table with no room; no send reaches it. `RELIABLE_TIMED_OUT` now happens only when the game set `reliable_message_timeout_ms`.
- A v1.0.x end still ends a connection by its reliable timeout and on a full send side, so mixed versions get the old behaviour from the older end. The wire is unchanged.

## Compatibility

- v1.1.0 adds two functions (`rocketnet_client_conditions`, `rocketnet_server_client_conditions`), their struct, and no other export, configuration field, status value or environment variable; code built against v1.0.0 compiles against it unchanged, except on 32-bit x86 Windows.
- Behaviour changes, listed above: a full send side refuses instead of disconnecting, `reliable_message_timeout_ms` defaults to 0, and the client's connect clock stops at the connection response.
- Packet layouts, connect tokens, message records and the protocol version `0x4E7C1A93` are unchanged, so v1.0.0 and v1.1.0 endpoints interoperate in both directions.

## Fixes since v1.0.0

- A late fragment of a reliable-sequenced block that a newer message superseded is now dropped and counted as `MESSAGE_SEQUENCED_STALE` instead of later ending the connection ([MESSAGES.md m7.3](../standard/MESSAGES.md#m7.3)); a v1.0.0 receiver keeps the old behaviour.
- On Linux and Windows, an IPv6 dual-stack socket now also sets Don't-Fragment on its IPv4 (v4-mapped) traffic, as macOS already did through the IPv6 option, so an IPv4 client whose path MTU is below 1480 bytes can connect to it but loses full-size packets.
- A server answers connection requests that need a cookie with at most one cookie to each source address and port per update; a further request from that source in the same update is dropped unanswered, counted only by its cookie counter, and the client sends again at its send rate ([TRANSPORT.md t12](../standard/TRANSPORT.md#t12)).
- The per-source limit is a keyed-hash bitmap sized at creation, so under a flood that fills an update's receive bound an honest source can be refused for one update, with a chance of at most about 1 in 16.
- A connection request whose signature verifies but whose sealed block does not open, whose sealed fields disagree with the signed fields, or whose client key is a small-order point now counts against its source's failed-signature budget, as a failed signature does ([TRANSPORT.md t17](../standard/TRANSPORT.md#t17)); a client id already connected or a full server still charges nothing.
- `rocketnet_server_create` now refuses an IPv4 and an IPv6 address on different ports with `ROCKETNET_STATUS_INVALID_ADDRESS`; v1.0.0 refused the same configuration later, at start.
- A client call that would change the client, made from inside `state_change_callback`, is now refused, leaves the client as it was and counts one `api_misuses`.
- From that callback, `rocketnet_client_connect` and `rocketnet_client_set_send_kbps` return `ROCKETNET_STATUS_INVALID_ARGUMENT`, `rocketnet_client_start_message` and `rocketnet_client_receive_messages` return `NULL`, `rocketnet_client_finish_message` and the receipt calls return 0, and the other calls, destroy included, do nothing.
- The log callback must not call any rocketnet function, and the server now writes its disconnect, kick and in-place abort log lines after the state change.
- `rocketnet_server_create` returns `NULL` with `ROCKETNET_STATUS_CRYPTO_INIT_FAILED` when the server keypair cannot be generated or derived from `server_secret_key`.
- With a nonzero `token_epoch_unix_seconds`, `rocketnet_client_connect` checks token expiry against the epoch plus the client's last update time and returns `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED` at once, so the clock passed to update must read zero at that epoch.
- Connect-token time now stops at the largest timestamp instead of wrapping when the epoch plus the caller's time overflows.
- A debug build no longer asserts in `rocketnet_client_index` when the server-assigned index is at or past the client build's `ROCKETNET_MAX_CLIENTS`.
- `send_message_buffer_bytes` no longer has to be about three times `max_message_bytes`, so a configuration v1.0.0 refused for that reason is now created.
- A reliable message whose first send the transport refused starts its reliable timeout (when one is set) when it first goes on the wire.
- Reliable resends go retransmits first: due records already on the wire, single-packet and chunk alike, are written before message numbers never sent, so a large block's first pass no longer holds back a due resend; the packets on the wire are unchanged.
- A reliable resend that the transport refuses to send is retried in the next write as the first retransmit of its class, keeping its timeout clock when one is set; a corrupted resend queue ends the connection as `PROTOCOL_VIOLATION` instead of reading out of bounds.
- The server ends a client with `PROTOCOL_VIOLATION` when its own acknowledgement state for that client is inconsistent.
- When a slot is reused within one update, a packet flushed for the previous client counts in the server's totals only, not in the new client's send totals.
- Under send pressure on a non-staged transport, the server holds back keep-alive and disconnect packets sooner.
- `rocketnet_server_required_memory` for the light server at 1000 clients (development build, Release) reports 3,111,668,715 bytes on Linux x86-64 with io_uring and XDP, 2,316,958 more than v1.0.0's 3,109,351,757 on the same host, and 1,469,493,151 bytes on macOS arm64, 481,070 more than v1.0.0's 1,469,012,081; on Windows x64 it is computed, not measured, at 1,469,492,735 bytes, 481,070 more than v1.0.0's 1,469,011,665, and the default server at 1000 clients 1,057,070 more. Each connection's server plan grows by 448 bytes in the light profile and 1,024 in the default profile: the queued counts' 24 bytes per channel, 8 bytes of resend marks, 8 bytes of development resend gap counts, 192 bytes of used-token insertion order and 8 of encryption map index (measured between 600 and 1000 clients on Linux and macOS), and the 40 bytes of the probe and round-trip filter clocks and the refused-send count. On Linux the 2,316,958 is: the clocks and refused-send count 40,000; queued counts 192,000; resend marks 8,000; development resend gap counts 8,000; the used-token tables' insertion-order ring and expiry floor 192,190; the encryption map index at twice its capacity 8,008; the cookie limit's bitmap 32,768; the XDP worker's receive frame arrays moved from its stack into each of the 7 default queues 1,835,064; the XDP queues' new counters and flags with their alignment 840; the XDP socket's gateway and statistics fields 40; the development replay scan counter 8; and the development link simulator's receive scan bound 40. On macOS the 481,070 is the same parts without the XDP ones (40,000; 208,000; 192,190; 8,008; 40) and the cookie limit's 32,832 (its bitmap and 64 bytes of the transport object). `rocketnet_client_required_memory` for the light client grows by 296 bytes (15,186,082 on Linux, 15,036,426 on macOS). Query it instead of reusing a v1.0.0 figure ([MEMORY.md](MEMORY.md)).
- `PREFILTER_TOO_LARGE` counts only a datagram whose reported size is above the receive limit; a custom transport's negative size, `NULL` receive view or out-of-range count is dropped without counting there.
- `SEND_RING_FULL` and `SEND_NO_LINK_ADDRESS` on the server's receive path count only a response that was refused.
- A disconnect burst dropped at the server's disconnect ring cap is counted in the new `DISCONNECT_BURST_DROPPED` counter and logged at debug level.
- The sockets of every client, of the portable, gso and io_uring servers and of the Windows IOCP and RIO servers, and the library's wake handles, are non-inheritable, so a child process you start no longer holds them.
- Server stop finishes when a disconnect burst cannot be sealed, and that client then times out instead of receiving a disconnect.
- Server stop no longer hangs when disconnect packets are waiting and the transport can never free send room (for example an xdp queue in hard send error): those bursts are dropped, counted as `SEND_RING_FULL`, and the stop completes.
- Connect token generation holds the random source for all three of its draws, including the sealed block's ephemeral keypair.
- A create that fails after its I/O worker started, other than on the Windows IOCP server, and a destroy of a message layer that was not stopped, wait at most about one second for the worker, then log an error and keep its memory.
- `rocketnet_init` returns `ROCKETNET_STATUS_LIVE_ALLOCATIONS` only when another thread calls into the library while init installs a different allocator.
- `rocketnet_create_connect_token` with both addresses `NULL` counts as one API misuse.
- The header now states that gso and io_uring refuse `num_xdp_queues` above 32, that an overlong transport environment value returns `ROCKETNET_STATUS_CONFIG_ENV_INVALID`, that a loopback hub's memory is outside both memory plans, and that `rocketnet_generate_backend_keypair` and `rocketnet_generate_server_keypair` return `ROCKETNET_STATUS_CRYPTO_INIT_FAILED` when the crypto library did not start (they did in v1.0.0 too).
- 32-bit x86 Windows builds no longer compile; the supported platforms are unchanged.
- A client connect attempt that ends before any cookie arrives closes its second address family's socket.
- The client's I/O thread retries a send the kernel refused for the moment within 2 ms, instead of at its next idle wake.
- The server's encryption map keeps lookups and removals short when it is full.
- A full connect-token bindings table takes its oldest expired binding directly.
- The server checks a staged transport's receive view before reading it, and the write pass respects the payload capacity a transport reports for a frame.
- The loopback transport refuses a bind to port 0 when every ephemeral port is held.
- The standard now permits a server's transport to drop a datagram shorter than the smallest packet a server receives, or longer than the receive ceiling, before the shared checks, counted as `PREFILTER_TOO_SHORT` or `PREFILTER_TOO_LARGE` ([TRANSPORT.md t10](../standard/TRANSPORT.md#t10)).
- On Linux gso, one destination that refuses UDP segmentation no longer turns segmentation off for the others: the refused run is resent as plain datagrams, segmentation turns off only after 8 refusals in a row, and the first refusal is logged once per transport.
- On Linux gso, a send interrupted by a signal is retried at once; while a refused segmented run is resent as plain datagrams, it is retried at the next send retry, within 1 ms.
- On Linux gso, a socket that stays full while the server stops no longer keeps the transport from reaching stopped.
- On Linux gso, the packets such a socket never took at stop are dropped after at most 100 retries and counted in `SEND_DROPPED` once each.
- On Linux gso, the receive worker sleeps 1 ms instead of spinning when its buffer is full or a read keeps failing.
- On Linux gso, the receive walk rotates which queue it reads first, so a busy early queue cannot starve the later ones.
- On Linux gso, the client drops a truncated receive completion.
- On Linux io_uring, sends are spread across the socket pool, so a burst larger than one socket's send buffer uses its siblings.
- On Linux io_uring, a kernel that refuses multishot receive is reported with one error, and submit and re-arm failures are logged once per transport.
- On Linux xdp, `SEND_FAILED` counts each datagram the kernel dropped at a transmit wakeup, and a hard wakeup error once per run of errors, which is logged once.
- On Linux xdp, stop no longer waits forever on a queue whose transmit wakeup keeps failing, nor on a transmit ring that has stopped draining.
- On Linux xdp, frames the kernel dropped on a queue's socket are counted in `XDP_RECEIVE_FRAME_DROPPED`.
- On Linux xdp, destroy logs an error naming the interface when it cannot detach the XDP program, error messages show the errno text on musl as well as glibc, and the pre-flight probe sockets are close-on-exec.
- On Linux xdp, the gateway refresh asks for the default gateway's neighbour entry instead of reading the whole neighbour table; it still reads the default route every period, so it follows a change of the gateway's IP address as v1.0.0 did. As in v1.0.0, a new gateway IP with no neighbour entry yet keeps the old gateway's MAC address until the entry appears.
- On Linux xdp, the gateway refresh no longer makes a blocking netlink receive inside the server update: its sockets are non-blocking, and a reply not yet queued fails that refresh, which keeps the gateway in use and retries next period.
- On Linux, the netlink reply parser rejects a reply whose error code is `INT_MIN`.
- On the portable transport, the send worker waits for a full socket to become writable and retries within 1 ms, and on Linux it no longer loops without pausing on `ENOBUFS`.
- A failed client connect logs the error that connect itself returned.
- On the Windows IOCP server, a send release or receive retry refused while its queue is briefly busy is retried, so stop completes once every operation has finished.
- On the Windows IOCP server, a send length out of range is refused in release builds and counted as `SEND_FAILED`.
- On the Windows RIO server, stop drains every completion batch in one pass, and a create that fails part way waits only for its posted completions.
- On the Windows RIO server, a create that fails part way and stops seeing its posted receives complete logs an error and keeps the transport's memory.
- On the Windows IOCP client, posted I/O is cancelled before its socket is closed.
- The spin lock guarding the sent-packet history pauses while it spins and yields after a bounded number of attempts.
- In development builds, each client's network simulator has its own seed, server stop drains the simulator before the transport stops, and taking a pending simulator setting never blocks the send path.
- The standard's m7.3 now says a reliable-sequenced record any distance ahead of its epoch is held, never refused, as the library and the vectors already did.
- The standard's t2.4 now says what the library has done since v1.0.0: a single transport name that is not applicable on the platform is not a pin, so the endpoint falls to its `auto` order with one warning instead of failing the create ([TRANSPORT.md t2.4](../standard/TRANSPORT.md#t2.4)).
- The standard's t13.3 now matches the library's disconnect burst rules.
- PLATFORMS.md states what runs where, REQUIREMENTS.md states the 1500-byte path MTU the library assumes, DEPLOYING.md's xdp crash recovery now unloads the program before the sweep, and LINUX.md says xdp needs the gateway's neighbour entry.
- The standard's t8 counts the nine assigned packet types, the send rate row moved from t3 to t17 with the same value, and the t18 registry gained `DISCONNECT_BURST_DROPPED`, so its registry vector is 7 bytes longer.
- The rationale now says a retransmitted connection response does not refresh a slot's idle clock, matching the code and t12.
- The rationale now says the frozen connect token is reproducible byte-for-byte from its published minting inputs.
- The rationale now places the two-live-generations rule under the pending keyed epoch extension, and states constant-time token identifier comparison as a property of the reference implementation.
- Three counts no longer wrap: a channel's queued messages at one priority (`rn_channel_send_t::queued`, which could pass 65535 with a large `send_queue_messages`, with no visible effect) and, in development builds, the resend gap record's resend counts (`rn_resend_gaps_t::count`) and the replay walk's scan counter (`rocketnet_server_replay_marks_t::comparisons`); each connection's message region grows by 24 bytes per channel (768 at the default 32 channels, 192 in the light profile), and a development build adds 8 bytes per connection. LOAD.md's `connection_bytes` formula now starts `88C` (64 bytes of send state and 24 of receive state per channel; it was `64C`).

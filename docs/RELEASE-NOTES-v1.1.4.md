# RocketNet v1.1.4

Changes compared with the `v1.1.3` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1, v1.1.2, v1.1.3 and v1.1.4 endpoints interoperate in
both directions.

- Tag: `v1.1.4`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.4).

## Fixes since v1.1.3

- When a connection's send budget is short, the sender writes its packet before it takes a frame. If the transport then refused the frame (an XDP server with no link-layer address for the client yet, a client whose send ring is full, or a full lease table), v1.1.3 dropped every unreliable message the packet had taken, counted each as a send failure, and went on to drop the rest of that connection's unreliable queue the same way. Now a refused frame leaves the unreliable queue as it was: nothing is sent, dropped or counted, and the messages go in a later update. This is the behaviour of v1.1.0 and earlier.
- The round-trip estimate no longer takes a sample from the first acknowledgement after an outage on a connection that had no acknowledgement before it: the silence is measured from the first send of the lowest unacknowledged message, as [MESSAGES.md m9.2](../standard/MESSAGES.md#m9.2) states. A packet whose reliable messages were all acknowledged already, such as a late copy, no longer gives a sample either ([m9.1](../standard/MESSAGES.md#m9.1)). Before, either could raise the estimate, and with it the resend delay, after a long stall.
- On the Linux XDP server transport, a frame larger than the interface MTU is refused before it reaches the transmit ring and counted in `SEND_FAILED`, with one warning per queue; the server warns at create when the interface MTU is below 1500. The kernel's AF_XDP transmit does not check the MTU, so before, the device dropped such a frame and not every drop was counted. LINUX.md describes it.
- On the Linux XDP server transport, forgetting a disconnected client's replay key searches only the forgets the last commit kept, so a mass timeout of 1,000 clients no longer reads about 500,000 pending forgets on the update thread. A replay-map batch the kernel stops at one key goes on past that key, rather than one call for each remaining entry.
- On the Linux io_uring server transport, a coalesced receive of more than 64 datagrams stages the first 64 and counts each one past them in `RECEIVE_DROPPED_OVERFLOW`; before, the whole receive was dropped and counted as one fault. Receives held for the receive bank's room take turns by queue, so a flooding source on one queue no longer delays the other queues' receives by updates.
- On Linux, `rocketnet_server_required_memory` grows by 8 bytes a server, for the XDP replay-key bookkeeping above; clients are unchanged.
- `scripts/rocketnet-provision` check R8 no longer passes without checking on a 6.12 or later kernel when `/boot/config-*` is absent. It reads `/proc/config.gz` when present; when neither is readable and the throttle sysctl is not already off, `verify` fails with "could not verify R8" instead of passing. The script's selftest settings are now ignored outside selftest mode.
- The server's log line for `protocol_id` 0 now says to set a non-zero protocol_id; the refusal applies in every build and with any keys, not only with production keys.
- New tests cover the refused frame on each path above, the server send cursor at a later-round capacity cut, and the per-destination segmentation refusal table with more refusing destinations than it holds. The formal models of the round-trip filter and the server's send fairness were brought up to the v1.1.3 code. Test, model, tool and CI changes only.

## Documentation corrections

- [TRANSPORT.md](../standard/TRANSPORT.md) and TRANSPORT-INTERNALS.md now say that a kernel path such as XDP verifies no IP header or UDP checksum: the packet's contents need none (a changed byte fails authentication), and a corrupted source address is handled as a spoofed one.
- USAGE.md no longer says one update can end a slot and admit another into it; a freed slot reads not connected after the freeing update and is not retaken before the next.
- The docs now state that the server's `max_timeout_seconds` caps the connection timeout on the server only; the client keeps the connect token's `timeout_seconds`, so mint it at or below every server's cap.
- USAGE.md states the handshake's two clocks (silence for T, and the token's lifetime from connect), and that a client resuming after a stall longer than T can read CONNECTED for up to another T.
- ENVIRONMENT.md and ERRORS-AND-LOGGING.md state that a production library ignores the `ROCKETNET_DEV_` simulator variables and refuses a nonzero `ROCKETNET_*_SIMULATED_*` variable with `CONFIG_OUT_OF_RANGE`.
- A reused connect token or an already-connected `client_id` is documented as a silent refusal that times out, not a denial.
- BUILDING.md: every build tree writes the repository's `bin/`; the SDK install commands now keep the production tree's output in `build-sdk/bin/`. The header needs C99 or later, or C++11 or later.
- ERRORS-AND-LOGGING.md names `INVALID_PROTOCOL_ID`, `PLATFORM_INIT_FAILED`, `FLOW_ID_RESERVED` and client states -6 and -7; USAGE.md names the abort calls; a server address with no port is documented as port 0.
- The `connections_denied_full` counter's documentation now states that since v1.1.2 it also counts a connection response ignored because the only free slots were freed in the same update.

## Corrections to earlier notes

- **v1.1.2, required memory.** The v1.1.2 notes did not say that the per-destination segmentation refusal table grew the server's required memory. Measured against v1.1.1 (the planned server memory), a gso server needs 1,296 more bytes and an io_uring server 1,344 more, the same at 1 and at 1,000 clients; the portable transport is unchanged.
- **v1.1.3, refused frames.** The v1.1.3 notes did not say that a refused frame after a budget-short write dropped unreliable messages, described in the first fix above. It was introduced in v1.1.3 and is fixed in v1.1.4. Reliable messages were not affected: they stayed queued and were sent again.

## Known issue

- Unreliable messages sent with receipts in volume can evict reliable messages' entries from the sender's sent map before their acks arrive. Those reliable messages are sent again; with a reliable timeout configured, a connection can end with `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT`. Not changed in this release.

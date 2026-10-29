# Silent discard sweep, 2026-10-24

A sweep of `mas-bandwidth/rocketnet` at `c7dff96bef1631b7938de8f15207f357f261ca3f`
(`main`), the one base every `file:line` below is cited at, for one bug class: data dropped, truncated, clamped or overwritten,
or state reset, where nothing names the loss. Nothing names it when no counter
from `sdk/rocketnet_counters.h` is bumped, no log line is written, no comment
says why dropping is correct, and no return value carries the refusal to a
caller that counts or surfaces it. Without one of those, neither an operator
nor a test can tell the loss happened.

Scope read:
- every `.cpp` and `.h` under `sdk/`;
- `sdk/rocketnet_tests.cpp` only where it defines helpers that production code
  calls (none exist: its exported functions are called only from
  `tests/test.cpp`);
- every file under `tests/functional/`;
- the `tests/*.h` helpers that the functional and DST tiers share.

Method: each file was read whole. Then every hit of the patterns below was
judged by reading its function and following the value to its use and to
every caller reachable from `sdk/rocketnet.h`, the wire or a config value.
The findings of every other document under `docs/audits/` at this base are
excluded: an instance one of them already files is listed once under
`## Already filed`, not as a finding.

The data-path set (P1, P1b, P4) is the 85 `sdk/` files whose names contain
receive, fragment, message, channel, window, transport, tracker, ring, queue,
replay, ack, history, handoff, batch, lease, coalesce, pending, endpoint,
protocol, socket, gso, iocp, rio, xdp, uring, driver, slots, simulator,
frame_sync, encryption_map or token_tables.

Patterns and hit counts:
- **P1** `return 0;|return;|continue;|break;` over the data-path set: 857 hits.
- **P1b** `return false;|return -1;|return NULL;|return nullptr;` over the same
  set: 646 hits.
- **P2** calls to `min(` or `max(` across `sdk/`: 24 hits.
- **P3** the explicit clamp `if ( x OP limit ) x = …` on the same or the next
  line, across `sdk/`: 58 hits.
- **P4** resets and overwrites (`_reset(`, `memset(`, `clear(`, `oldest`,
  `evict`, `overwrite`, `= 0xFFFFFFFF`) over the data-path set: 364 hits.
- **P5** P1 together with a call to `min(` or `max(` (the regex
  `return 0;|return;|continue;|break;|(^|[^A-Za-z0-9_])(min|max)\(`) over
  `tests/functional/` and `tests/*.h`: 483 hits.
- **P6** casts to `int`, `uint8_t` or `uint16_t` of a value named len, size,
  bytes or count, across `sdk/`: 78 hits.

Reading showed instances these patterns miss, so P1 and P1b were widened to
files outside the data-path set:
- `sdk/rocketnet_client_io.h`: 23 hits;
- `sdk/rocketnet_api.cpp`: 144 hits;
- `sdk/rocketnet_platform.cpp`: 216 hits;
- `sdk/rocketnet_connect_token.cpp`: 4 hits;
- `sdk/rocketnet_connection_reset.cpp`: 7 hits;
- `sdk/rocketnet_send_budget.h`: 12 hits.

Reading also added shapes no regex here expresses:
- `goto` re-reads after a refused source (5 hits in `sdk/rocketnet_platform.cpp`);
- ternary clamps of the form `a < b ? x : y` and `? ( a - b ) : 0`;
- `if ( frame )` with no `else` after a start or reserve call;
- a call whose `int` or `bool` result is dropped by a `void` wrapper;
- an `assert` followed by a silent return or a raw store;
- a reset that follows a delivery loop in the same function;
- a tally field written but never read outside tests.

Findings are ordered most severe first. Severities:
- **bug**: wrong behaviour reachable today through the API, the wire or a
  supported configuration.
- **risk**: wrong under a condition the code permits but nothing exercises yet.
- **quality**: costs a reader or a maintainer, with no wrong behaviour.

Each finding gives `file:line` at c7dff96b, the defect, the failure or cost,
and the smallest fix; every `file:line` in the tables is at c7dff96b too. No
code was changed.

## Findings

### 1. bug — `sdk/rocketnet_client_transport.cpp:2476`

- Defect: `rocketnet_client_transport_update` calls `rocketnet_client_connection_reset` after the receive loop when the connection ended inside this update. That reset sets `received_packets.num_packets = 0` (line 410), throwing away the payloads this same update already put in the batch and counted as `CLIENT_PAYLOAD_DELIVERED`, with no counter, log or comment.
- Failure or cost: The server sends a last message and then disconnects within one update. The client receives both in one batch, the reset empties the batch, and the message layer never sees the payloads. Without the reset, they would reach admission, which is written to discard and count an ended connection's entries as `MESSAGE_STALE_CONNECTION_ENTRY` (comment at `sdk/rocketnet_message_server.cpp:516`). The application loses the server's final messages, and the counters say they were delivered. The caller's own `rocketnet_client_transport_disconnect` (line 3062) wipes an undelivered batch the same way.
- Smallest fix: Leave `received_packets` alone in the reset, since update clears it at line 2436 next frame and admission counts the stale entries. Otherwise count the non-empty batch the reset discards.

### 2. bug — `sdk/rocketnet_message_server.cpp:1735`

- Defect: `rocketnet_message_server_receipts` returns 0 for any connection not in `RN_MESSAGE_CONNECTION_CONNECTED`, so a connection kicked in this update has its receipts withheld. Their outcomes stay in the ack ring with nothing counting them. The client side (`sdk/rocketnet_message_client.cpp:372`) has no such check.
- Failure or cost: `rn_endpoint_tick` makes this update's ACKED and LOSS outcomes visible (line 704). A reliable timeout, exhaustion or violation then kicks the connection later in the same update (line 740), and `rocketnet_server_receipts` returns 0. `sdk/rocketnet.h:1678` says receipts "remain until the next update", but the application never learns the outcomes of the sends it was tracking.
- Smallest fix: Accept `RN_MESSAGE_CONNECTION_KICKED` as well as `CONNECTED` in the check, so outcomes stay readable until the next update resets the slot.

### 3. bug — `sdk/rocketnet_server_socket_linux.cpp:3198`

- Defect: After `quit` is set, each XDP worker makes one final pass and then `break`s. That pass claims at most one ready buffer and copies at most `ROCKETNET_XDP_SEND_BATCH_SIZE` (32) packets onto the TX ring. Everything else is dropped with no counter or log:
  - the rest of the claimed buffer;
  - further ready buffers;
  - the producer buffer;
  - a batch whose TX reservation returned 0.
- Failure or cost: `rocketnet_server_stop` (`sdk/rocketnet_api.cpp:1776`) queues 10 disconnect packets per connected client and then stops the transport. On an XDP server, more than 32 shutdown packets on one queue (four clients) means some clients never receive a disconnect and time out instead. The comment at line 2899 names the final pass, not the loss. The GSO transport drains every ready bank at stop and loses nothing.
- Smallest fix: While `stopping`, keep passing (bounded) until the ready queue, the producer and the TX batch are empty. Otherwise add the leftover count to a send-dropped counter and log it once.

### 4. risk — `sdk/rocketnet_client_transport.cpp:2070`

- Defect: When the client's receive batch already holds `ROCKETNET_NUM_CLIENT_PACKETS` entries, `rocketnet_client_process_packet` opens the payload into a stack `overflow` buffer. The protocol counts it `CLIENT_PAYLOAD_DELIVERED` and advances replay, and then the `payload_bytes > 0 && payload` test discards it with no counter. The fragment branch a few lines below counts `CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL` for the same condition.
- Failure or cost: If one update ever presents more records than batch entries (today each update processes at most 1024 records), a payload that was counted delivered, and possibly acked, vanishes uncounted.
- Smallest fix: Bump a batch-full counter in an `else` branch, as the fragment branch does, or refuse before the protocol commits the payload.

### 5. risk — `sdk/rocketnet_server_socket_linux.cpp:2785`

- Defect: `rocketnet_server_socket_xdp_counters` zeroes the output (line 2770) and then returns silently on two failures:
  - a failed `bpf_map_lookup_elem` (line 2785);
  - a negative `libbpf_num_possible_cpus()` (line 2779).
- Failure or cost: A failed stats read publishes every `XDP_KERNEL_*` counter as 0. An operator reads "the kernel dropped nothing" exactly when the real counts are unknown.
- Smallest fix: Log the failure (rate-limited, with errno) and leave the previous values, or report the read as failed to the caller.

### 6. risk — `sdk/rocketnet_server_transport.cpp:751`

- Defect: `rocketnet_server_link_drain` takes a due packet out of the link simulator, and on a staged transport discards it when `rocketnet_server_transport_io_start_packet` returns NULL, through an `if ( frame )` with no `else`. Every other caller of that start counts `SEND_RING_FULL`.
- Failure or cost: In a development build with simulator latency on an XDP, GSO, io_uring or IOCP server, a full staging area drops held packets that no counter shows. Simulated-loss experiments then report less loss than the server caused.
- Smallest fix: In an `else`, bump `SEND_RING_FULL` unless the id is `ROCKETNET_TRANSPORT_PACKET_UNREACHABLE`.

### 7. risk — `sdk/rocketnet_server_rio_windows.cpp:730`

- Defect: `rocketnet_server_rio_finish_packet` returns silently when `packet_bytes` is 0, negative or past `ROCKETNET_RECEIVE_CEILING`. The only guard is the assert at line 727, and the slot keeps zero bytes, so the post phase skips it as aborted.
- Failure or cost: In a release build, a finish with an out-of-range length loses the staged packet with no `ROCKETNET_RIO_COUNTER_SEND_FAILED` count and no log. (The `index < 0` half of the test is named by the comment at lines 322–324.)
- Smallest fix: Count `SEND_FAILED` before the return.

### 8. risk — `sdk/rocketnet_server_iocp_windows.cpp:804`

- Defect: `rocketnet_server_iocp_finish_packet` stores `packet_bytes` guarded only by the assert at line 802. (The `packet_id` bound, asserted at line 800, is already filed; see `## Already filed`.)
- Failure or cost: In a release build, `packet_bytes == 0` makes the slot look aborted, and `send_bank` (line 949) returns it to the pool with no `SEND_FAILED` count.
- Smallest fix: Check the length in release, count `SEND_FAILED`, and leave the slot aborted.

### 9. risk — `sdk/rocketnet_driver_loopback.cpp:319`

- Defect: `loopback_send` returns, and `loopback_server_send_batch_impl` `continue`s (line 363), on a byte count of 0 or less, or past `ROCKETNET_RECEIVE_CEILING`, without calling `loopback_send_failed`. Both functions call it on their other two drop branches, and the portable transport counts the same condition into `SEND_FAILED` (`sdk/rocketnet_driver_builtin.cpp:458`).
- Failure or cost: A loopback datagram with an out-of-range length vanishes with no `SEND_FAILED` count. Today's callers never produce one.
- Smallest fix: Call `loopback_send_failed( sender, 1 )` before the `return` and the `continue`.

### 10. risk — `sdk/rocketnet_network_simulator.cpp:487`

- Defect: `rocketnet_network_simulator_patterns` frees the reverse window and zeroes `window_count` without flushing the packets it holds, although the header (`sdk/rocketnet_network_simulator.h:54`) calls the patterns "adjustable at any time".
- Failure or cost: Calling it again with fewer than W packets buffered drops up to W−1 packets, with no counter, log or comment. Today it is called only at create, when the window is empty.
- Smallest fix: Call `rocketnet_network_simulator_flush_window( simulator )` before freeing the window.

### 11. risk — `sdk/rocketnet_api.cpp:1309`

- Defect: `rocketnet_server_served_address` copies the served address with `rocketnet_copy_string` into the caller's buffer, ignores truncation, and returns true. The sibling `rn_server_family_address` (lines 1934–1938) refuses the same case with `rocketnet_error`.
- Failure or cost: A caller passing a buffer shorter than the address receives a cut address reported as success.
- Smallest fix: Refuse with `rocketnet_error` and return false when `strlen( served[i] ) + 1 > address_bytes`.

### 12. risk — `sdk/rocketnet_frame_sync.cpp:222`

- Defect: `rocketnet_frame_sync_elevate_priority` narrows `period_mach`, a 64-bit tick count, into the `uint32_t` policy fields (`period`, `computation` and `constraint`). Nothing names the truncation, and `rocketnet_frame_sync_create` accepts any frame rate above 0.
- Failure or cost: On a 1/1 timebase, a frame rate below about 0.233 Hz wraps all three fields. The thread gets a real-time constraint unrelated to the requested period, possibly with `computation` greater than `period`.
- Smallest fix: Refuse a frame rate below a stated minimum (for example 1 Hz) next to `ROCKETNET_FRAME_SYNC_MAX_FRAME_RATE`, or clamp `period_mach` to `UINT32_MAX` with a log line.

### 13. risk — `sdk/rocketnet_frame_sync.cpp:337`

- Defect: `rocketnet_frame_sync_advance` computes `drops = (int) ( error / period_ns )` from 64-bit values with no bound.
- Failure or cost: A wake more than `INT_MAX` periods late (about 24.9 days stopped at 1000 Hz) wraps `drops` negative. The caller's `if ( drops > 0 )` (line 499) then skips the `dropped_frames` bump and the debug line, so the whole stall goes uncounted.
- Smallest fix: Saturate the quotient to `INT_MAX`, or add the 64-bit quotient to `dropped_frames` directly.

### 14. risk — `tests/dst_checks.h:1028`

- Defect: `rocketnet_dst_check_issued` records every endpoint's packet ids in one 4096-entry bitmap keyed by `id % 4096`, with no endpoint tag, and never clears it. Each endpoint numbers its packets from 1.
- Failure or cost: A client ack for an id only the server issued passes `acked_means_opened`. Once 4096 ids are issued, every slot is set and the check can never fail, so a link ack for a packet that was never issued passes DST silently.
- Smallest fix: Key the record on (endpoint, id), or keep one bitmap per endpoint with the base it covers, and fail on a lap.

### 15. risk — `tests/dst_checks.h:578`

- Defect: `rocketnet_dst_check_ordered_runtime` scans "earlier sent sequence still undelivered" from 0 up to the absolute bound `ROCKETNET_DST_CHECK_SEQ_BITS` (4096), not from `reliable_base`.
- Failure or cost: Once an ordered stream passes sequence 4096, a skipped ordered message is not seen at delivery. Only the end-of-run liveness check can catch it, and only if the session is still live. Long `--steps` runs can reach this.
- Smallest fix: Scan from `reliable_base` to `seq`.

### 16. risk — `tests/dst_checks.h:743`

- Defect: `rocketnet_dst_check_admit` appends to `admitted_client_id` only while `admitted_count < ROCKETNET_DST_CHECK_MAX_PACKETS`, and drops later admissions silently.
- Failure or cost: Past 4096 admissions, `token_admits_once` can no longer fail, so a token admitted twice late in a long run passes.
- Smallest fix: Call `rocketnet_dst_check_fail` when the list is full.

### 17. risk — `tests/dst_harness.h:501`

- Defect: `rocketnet_dst_endpoint_send_packets` drops a frame that was already sealed and recorded as issued (line 473) when the receiver index is out of range. It leaves no trace record and calls no `dst_fail`. The same happens to a written keep-alive or ack with no address (line 359) and a disconnect burst to an unresolved target (line 554).
- Failure or cost: A harness mapping error turns into silent loss that DST attributes to the library, or masks, instead of failing at the cause.
- Smallest fix: Call `rocketnet_dst_fail( w, "unroutable_frame", … )` in each branch.

### 18. risk — `tests/dst_checks.h:192`

- Defect: `rocketnet_dst_check_stream_index` maps an out-of-range session, channel or type to stream 0 and returns no failure.
- Failure or cost: A caller bug feeds another stream's sequences into stream 0's state, corrupting its checks with nothing reporting the bad index.
- Smallest fix: Call `rocketnet_dst_check_fail( st, "dst_stream_index", … )` before returning.

### 19. risk — `tests/functional/channel_harness.h:1028`

- Defect: The harness pump pushes a server delivery onto `to_server` only when `slot == h->client_slot`, and releases any other slot's message without bumping `overflow_count`, which every channel case asserts on.
- Failure or cost: A server that reported a message on the wrong slot would pass every channel case, because the message vanishes before any assertion sees it.
- Smallest fix: Bump `overflow_count` (or fail) in an `else` branch.

### 20. risk — `tests/functional/channel_ordered.cpp:220`

- Defect: The collect loop `break`s when a delivery arrives after all 64 ordered messages are counted. The delivery has already been popped, and nothing drains or checks the ring after the loop.
- Failure or cost: A duplicate or extra ordered delivery at the end of the run is discarded unchecked, and the case passes.
- Smallest fix: Fail the case on a delivery past `rocketnet_ordered_message_count`.

### 21. risk — `tests/functional/channel_resend_timing.cpp:488`

- Defect: `block_cost_run` counts every small delivery into `small_delivered` with no seen-set, unlike `resend_timing_run`.
- Failure or cost: One duplicate delivery and one lost message together satisfy `small_delivered == small_sent` (line 547), so the case passes with a message lost.
- Smallest fix: Keep a `seen[]` array and fail on a repeat.

### 22. risk — `tests/functional/posix_hard_killed_client.cpp:382`

- Defect: The live-client receive loop skips any payload with the wrong size or tag (`continue`), and does not count it (the same at lines 399, 513 and 525).
- Failure or cost: A corrupted payload delivered to or from a live client is ignored, and the case asserts only progress (lines 738–739), so corruption passes.
- Smallest fix: Fail when a malformed payload arrives on a live client's slot.

### 23. risk — `tests/functional/harness.inc:178`

- Defect: `rocketnet_test_capture_log` appends each log line into the 16 KiB `rocketnet_test_captured_log` with `snprintf`, which silently truncates once the buffer is full.
- Failure or cost: An absence check such as `strstr( …, "port-zero public address" ) == NULL` (`tests/functional/rocketnet_functional_tests.cpp:4868`) passes when the line it looks for was cut off.
- Smallest fix: Set an overflow flag on truncation, and have every absence check assert that it is clear.

### 24. risk — `tests/functional/rocketnet_functional_tests.cpp:6815`

- Defect: The acked-unfragmented case drops several things with no count or failure:
  - payloads of the wrong size;
  - sequences outside 1..total (line 6819);
  - acks for ids it never recorded;
  - acks for another client index (line 6858).
  It also tallies deliveries in `uint8_t` arrays that wrap at 256.
- Failure or cost: A corrupted payload or a stray ack is filtered out before the per-sequence checks, so the case can pass while the transport delivered wrong data.
- Smallest fix: Fail in each filter branch and make the tallies `int`.

### 25. quality — `sdk/rocketnet_server_transport.cpp:3142`

- Defect: `rocketnet_server_transport_send_packet` returns `void` and silently drops the payload in three cases:
  - an out-of-range size (line 3142);
  - a NULL start (line 3147);
  - a failed finish, whose result it ignores (line 3152).
  `rocketnet_client_transport_send_packet` does the same (`sdk/rocketnet_client_transport.cpp:2522`).
- Failure or cost: The example programs and functional cases that call these internal helpers cannot tell a dropped send from a sent one.
- Smallest fix: Return the result (or spam-log the refusal, as start does at line 2551).

### 26. quality — `sdk/rocketnet_channel.cpp:1278`

- Defect: When `rn_ordered_item` refuses a due number (a NONE or out-of-range descriptor, a slot not in use, or no data), `rn_ordered_walk` skips it with no `else`. The new-number loop at line 1389 does the same.
- Failure or cost: A corrupted send descriptor leaves its number unsent until the reliable timeout. The connection then ends as RELIABLE_TIMED_OUT, which names the wrong cause.
- Smallest fix: In an `else`, assert and end the connection through `rn_region_exhaust` (or a protocol-violation count).

### 27. quality — `sdk/rocketnet_server_protocol.cpp:2239`

- Defect: `rocketnet_server_protocol_disconnect_client` checks the sealed disconnect length only with `rocketnet_assert`, as does `rocketnet_server_protocol_write_fragments` (line 2190) for each fragment.
- Failure or cost: If sealing ever fails in a release build, the burst reports 10 packets of 0 bytes, which send nothing, and no counter moves. The deferred burst path (`sdk/rocketnet_server_transport.cpp:3216`) instead stalls on the same failure every update.
- Smallest fix: On a zero length, count `CONNECTION_RESPONSE_INTERNAL_ERROR` (or a send-error counter) and report a count of 0.

### 28. quality — `sdk/rocketnet_window_ack.cpp:580`

- Defect: `rn_window_ack_on_ack` clamps a negative RTT sample (`now` earlier than `send_time`) to 0.0 and feeds it into `srtt` and `rttvar`, with no comment. `sdk/rocketnet_ack.cpp:1173` skips the same case instead, with a comment.
- Failure or cost: The clamp is unreachable while update time is clamped monotone upstream. A reader must find that upstream rule to know a zero sample cannot shorten the resend delay.
- Smallest fix: Skip the sample as `rocketnet_ack.cpp` does, or comment the upstream monotone clamp.

### 29. quality — `sdk/rocketnet_client_protocol.cpp:130`

- Defect: `rocketnet_client_rtt_sample` discards a negative handshake RTT sample after disarming the stamp (line 128), with no comment saying why.
- Failure or cost: A reader cannot tell whether the discard guards a backwards clock or hides a stamping bug.
- Smallest fix: Add a one-line comment naming the backwards-clock case it guards.

### 30. quality — `tests/functional/harness.inc:96`

- Defect: `rocketnet_test_log_capture` drops a whole log line of `ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES` or more, with no count or comment.
- Failure or cost: A failing case's log ring silently lacks its longest lines, which are often the error lines being looked for.
- Smallest fix: Store the truncated line with a marker, or count dropped lines and print the count with the ring.

### 31. quality — `tests/functional/channel_harness.h:155`

- Defect: `rocketnet_channel_capture_flush` returns silently when the capture file cannot be opened. `past_budget` (line 184) is counted but never reported.
- Failure or cost: A requested wire capture can be missing or partial with nothing in the output saying so.
- Smallest fix: Print the path and errno on an open failure, and print `past_budget` when it is non-zero.

### 32. quality — `tests/pcapng.h:368`

- Defect: The pcapng reader keeps only the last `opt_comment` of a packet (each one overwrites the previous) and skips unknown option codes, while the header promises to refuse everything outside the subset.
- Failure or cost: A capture with two comments, or an unsupported option, reads back as valid with data missing.
- Smallest fix: Refuse a repeated comment and unknown option codes with `ROCKETNET_PCAPNG_BAD_OPTION`.

### 33. quality — `tests/suite_runner.h:364`

- Defect: The `ROCKETNET_TEST_JOBS` value is clamped to 8 with no comment or message, though the cap is load-bearing for the `handles[8]` array (line 1109).
- Failure or cost: A user asking for more jobs silently gets 8, and a maintainer raising the cap cannot see what depends on it.
- Smallest fix: Name the cap as a constant shared with `handles[]` and print a line when the request is clamped.

### 34. quality — `tests/functional/channel_resend_timing.cpp:251`

- Defect: The case drains every client channel into an empty loop body that discards the deliveries, with no comment saying why they are not checked. The same pattern appears at lines 499–502.
- Failure or cost: A reader cannot tell whether ignoring server-to-client deliveries is intended or a missing check.
- Smallest fix: Add a comment such as the one at `tests/functional/rocketnet_functional_tests.cpp:3665`, or check the drained count.

## Counts

- bug: 3 (findings 1–3)
- risk: 21 (findings 4–24)
- quality: 10 (findings 25–34)
- total: 34

## Judged not a defect

Each row covers the hits in one function, or one tight group sharing one
reason, so that every hit of every pattern above is accounted for.

### `sdk/`, data path

| File | Lines | Why it is safe |
|---|---|---|
| `sdk/rocketnet_server_transport.cpp` | 570, 629, 666, 709, 867, 2444, 2784, 2798, 3068 | success paths or production stubs |
| `sdk/rocketnet_server_transport.cpp` | 671, 2744, 2758, 2578, 2594, 2607, 2621, 2649 | counted SEND_RING_FULL, SEND_NO_LINK_ADDRESS, SEND_ARENA_FULL or SEND_LEASE_FULL |
| `sdk/rocketnet_server_transport.cpp` | 721, 723, 736, 746 | simulator inactive or empty; queued packets kept for the next flush (comment) |
| `sdk/rocketnet_server_transport.cpp` | 879, 2660, 2666, 2793, 2943 | nothing reserved; the flush reclaims the arena (comment) |
| `sdk/rocketnet_server_transport.cpp` | 897, 1839, 1844, 1893, 1921, 1945, 2046, 2053, 2454–2489, 2702–2706, 3002–3006, 3031, 3034, 3136, 3138, 3255, 3318–3344, 3391–3457 | counted null guards, not-running checks, destroy before stop asserted and retained (`sdk/rocketnet_layers.h:443`) |
| `sdk/rocketnet_server_transport.cpp` | 983, 1007–1052 | host stack estimate returns false to its caller |
| `sdk/rocketnet_server_transport.cpp` | 925–960, 1139–1807 (create refusals), 1480 | create and plan refusals set a status and log |
| `sdk/rocketnet_server_transport.cpp` | 1999 | bucket outside the one-second window (comment at 1997) |
| `sdk/rocketnet_server_transport.cpp` | 2229, 2234, 2246 | transport fault named by comment (2225–2226) and the `transport_faults` tally read by the unit tests |
| `sdk/rocketnet_server_transport.cpp` | 2156 | batch-full payload drop named by comment (2111–2112); unreachable |
| `sdk/rocketnet_server_transport.cpp` | 2260 | PACKETS_RECEIVED and PREFILTER_TOO_LARGE counted |
| `sdk/rocketnet_server_transport.cpp` | 2370, 2374, 2382, 3259 | slot not connected, maintenance not due, or SLOT_WITHOUT_MAPPING counted |
| `sdk/rocketnet_server_transport.cpp` | 2388, 3208, 3276, 3292 | staging full is logged at 819 and resumes next update (comments 289, 3192) |
| `sdk/rocketnet_server_transport.cpp` | 2516, 2547, 2552, 2562, 2694, 1931–1975, 2501–2540, 2685–2691, 3362 | NULL or 0 returned to the caller, which counts it; spam logged at 2551 |
| `sdk/rocketnet_server_transport.cpp` | 2826–3012 (finish and abort) | refusal returns 0 to the finish caller; burn and withdraw commented (2804–2817, 2916–2920, 2936) |
| `sdk/rocketnet_server_transport.cpp` | 3143, 3148 | named under finding 25 |
| `sdk/rocketnet_server_transport.cpp` | 3219 | named under finding 27 |
| `sdk/rocketnet_server_transport.cpp` | 3466, 3476, 3504–3512, 3554–3589 | zero answers for an empty slot (comment 3468–3473); DEV capture; test fixture |
| `sdk/rocketnet_server_transport.cpp` | P3 3171, 3200 | constant ack-block cap; ring cursor wrap |
| `sdk/rocketnet_server_transport.cpp` | P4 376, 410, 535, 910–3461 (init), 3067, 3109, 3185 | test seams; totals cleared at slot assignment (comment 182–185); initialisation; arena reset at flush (comment 2664); reset after SERVER_DISCONNECTED_CLIENT counted |
| `sdk/rocketnet_window_ack.cpp` | 61, 137, 204, 253, 277, 284, 292, 383, 399, 423, 699, 710, 729–842 | unlink no-op, insert success, reclaim stop, nothing unacked, DEV gap measure, serial 0 already resolved (comment 453), sweep stop (comment 686–692), nothing due |
| `sdk/rocketnet_window_ack.cpp` | 464 | `packet_id` is a lease or transport id, never 0 |
| `sdk/rocketnet_window_ack.cpp` | 570, 612 | unknown id ignored (header 211–213); stale reference (comment 598) |
| `sdk/rocketnet_window_ack.cpp` | 840 | timeout sets the disconnect reason |
| `sdk/rocketnet_window_ack.cpp` | P1b 47, 172, 180, 188; P2 593 | predicates; comment |
| `sdk/rocketnet_window_ack.cpp` | P3 669, 693, 712 | previous list expires (comment 667–668); sweep past entries already resolved as LOSS; RING_FULL eviction counted at 432 |
| `sdk/rocketnet_window_ack.cpp` | P4 15–781 | comments, initialisation, oldest-number tracking; 506–517 eviction counts SENT_MAP_EVICTED and resolves receipts as LOSS |
| `sdk/rocketnet_window_ack.cpp` | P6 536, 537 | bounded by the assert `count + receipt_count <= 16` |
| `sdk/rocketnet_window_ack.h` | P4 48–266 | declarations and comments |
| `sdk/rocketnet_window_send.cpp` | 52, 143, 164, 167, 173; P4 171–183 | MESSAGE_WINDOW_STALL counted; stale ack returns 0 (header 47–48); loop end; oldest-number reads |
| `sdk/rocketnet_window_receive.cpp` | 97 | loop end; slot collision counted in `sdk/rocketnet_channel.cpp:1501` |
| `sdk/rocketnet_packet_lease.h` | 41, 44, 52, 55, 63, 69–93 | caller counts SEND_LEASE_FULL; probe continues; refusal returns NULL or 0 |
| `sdk/rocketnet_out_of_order_tracker.h`, `sdk/rocketnet_jitter_tracker.h`, `sdk/rocketnet_packet_loss_tracker.h` | 56; P4 37, 43, 46, 57 | counted in `num_out_of_order_packets`; initialisation |
| `sdk/rocketnet_replay.cpp` | 36, 63 | predicate; full-window skip commented |
| `sdk/rocketnet_server_replay_marks.cpp` | 41–183 | walk skips; create OOM logged; test seam; marks cleared after `forget` |
| `sdk/rocketnet_xdp_replay_marks.h` | 29, 63, 78, 93 | `refused` counted, unreachable (`ROCKETNET_XDP_REPLAY_MAX_ENTRIES >= ROCKETNET_MAX_CLIENTS`); reset after a counted commit (the reset of the unread `refused` tally at 67 is already filed) |
| `sdk/rocketnet_stats_window.h`, `sdk/rocketnet_buffer_handoff.h`, `sdk/rocketnet_pending_frames.h`, `sdk/rocketnet_send_handoff.h`, `sdk/rocketnet_xdp_free_list.h` | all hits | `owed` flag (comment `sdk/rocketnet_buffer_handoff.h:42–44`); none ready; caller logs and quits; retain semantics commented; caller logs `rocketnet_error` |
| `sdk/rocketnet_client_transport.cpp` | 260, 292–359, 433, 438, 443, 469, 541, 1030–1060, 2211, 3307, 3312, 3373 | create zeroing, test seams, stubs, predicates |
| `sdk/rocketnet_client_transport.cpp` | 397, 404, 409, 412, 413 | connection reset of counts and trackers (comment 405–406); the batch wipe is finding 1 |
| `sdk/rocketnet_client_transport.cpp` | 591, 612, 617 | losing race family dropped (comments 588, 604) |
| `sdk/rocketnet_client_transport.cpp` | 710, 715, 728, 747, 2147, 2156 | sent directly, no simulator, or ring handed to the simulator |
| `sdk/rocketnet_client_transport.cpp` | P3 790, 1869, 2124, 2874 | clamp of overshoot from failed acquires, each counted SEND_RING_FULL |
| `sdk/rocketnet_client_transport.cpp` | 933–985, 1080–1816, 1195, 1272, 1273, 1379, 1428, 1472–1477, 1576 | create and memory-plan refusals with status or log; initialisation |
| `sdk/rocketnet_client_transport.cpp` | 1858, 1889, 1891, 1900, 1955, 1958 | repeat stop, predicates; premature destroy asserted and retained (`sdk/rocketnet_layers.h:443`) |
| `sdk/rocketnet_client_transport.cpp` | 2019 | CLIENT_RECEIVE_FOREIGN_SOURCE counted; uncounted after reset (comment 1998–2003) |
| `sdk/rocketnet_client_transport.cpp` | 2192, 2587, 2608, 2626, 2708, 2748 | counted SEND_RING_FULL, SEND_ARENA_FULL or SEND_LEASE_FULL |
| `sdk/rocketnet_client_transport.cpp` | 2227–2270, 2317, 2407 | busy mutex or budget leaves buffers Ready (commented); stats walk end; comment text |
| `sdk/rocketnet_client_transport.cpp` | 2365, 2377, 2515, 2517, 2523, 2528, 2551, 2553, 2557, 2560–2570, 2637–2674, 2722–2842 | counted null guards; NULL or 0 returned to the message layer, which counts; finding 25 covers 2523 and 2528 |
| `sdk/rocketnet_client_transport.cpp` | 2853–2945 | stop handed the final ring; arena reset after copy (comment); too-small payload counted RECEIVE_CURSOR_TOO_SMALL |
| `sdk/rocketnet_client_transport.cpp` | 2951–3168 | getter guards and predicates |
| `sdk/rocketnet_client_transport.cpp` | 3041, 3045, 3051, 3079, 3095, 3172 | disconnect guards (repeat commented at 3048); burst handed off or queued |
| `sdk/rocketnet_client_transport.cpp` | 3261, 3262, 3268, 3275 | a full buffer leaves datagrams in the socket (comment 3221); fault tally as at `sdk/rocketnet_server_transport.cpp:2229` |
| `sdk/rocketnet_client_io.h` | 209–326, 376–438, 459, 480, 583–637; P6 320 | keys and verdicts; deferred or busy data waits; aborted slot commented; transport faults named by comment and `transport_faults`; `sizeof` cast |
| `sdk/rocketnet_client_protocol.cpp` | 51, 65, 82, 114, 124, 154, 189, 208–263, 311, 340, 359, 380–403, 746, 999, 1002, 1377, 1710, 1717 | same-state no-op; resets at terminal, connect or disconnect (commented); debug log and terminal state; switch breaks; unreachable defaults |
| `sdk/rocketnet_client_protocol.cpp` | 425–676, 774–1354 | per-type counter plus spam, or the callee counts |
| `sdk/rocketnet_client_protocol.cpp` | 1430–1646 | 0 returned to the transport, which counts it; documented 0 returns of direct-drive helpers |
| `sdk/rocketnet_client_protocol.cpp` | P3 297, 1461, 1502, 1592, 364; P4 515, 1456 | ack block truncated to budget (comments 292, 1456); stamp arm; comment text |
| `sdk/rocketnet_client_slots.cpp` | 54–336 | sizing; create refusal; find miss; full checked by caller (`sdk/rocketnet_server_protocol.cpp:874`); found-break; test seam |
| `sdk/rocketnet_client_gso_linux.cpp` | 127–479 | scratch zeroing; send failure counted SEND_FAILED; rest stays in the socket (comment); zero-length or unspeakable source (comment 337) |
| `sdk/rocketnet_client_gso_linux.cpp` | 369 | GRO segments past the caller's room named by `sdk/rocketnet_client_gso.h:25–26` and `gro_packets_dropped` |
| `sdk/rocketnet_server_socket_linux.cpp` | 568–792, 984–1007, 1749, 1926 | parse loop ends; size refusals logged at 824; warned; destroy before stop asserted |
| `sdk/rocketnet_server_socket_linux.cpp` | 2175–2292 | inactive flow or stale pointer (comments 2182–2185, 2472–2475); try-lock retried next flush |
| `sdk/rocketnet_server_socket_linux.cpp` | 2650, 2664, 2685, 2818–2838, 2886, 2954, 2972, 3016, 3086, 3100 | unreachable (asserted 2844, checked 2837); logged; tail kept via the read index; false positive `pending_return`; descriptors cancelled back; counted `receive_drops`; batch full keeps the start index |
| `sdk/rocketnet_server_socket_linux.cpp` | P1b 149–2748 | exchange full or empty keeps the buffer; create failures logged; caller counts SEND_RING_FULL or SEND_NO_LINK_ADDRESS; test-only |
| `sdk/rocketnet_server_socket_linux.cpp` | P2 2865; P4 369–3023; P6 606, 1018, 1462 | scheduler priority call; initialisation; marks reset after a counted commit; comment; `sizeof` casts |
| `sdk/rocketnet_server_uring_linux.cpp` | all P1, P1b, P3, P4 and P6 hits | test seams and stubs; SEND_DROPPED, NO_BUFFER, REARM_FAILED or SEND_DEFERRED counted; stale pointer commented (1506–1507); completion queue empty; logged create refusals; bounds defer the rest to the next run |
| `sdk/rocketnet_uring_codec.h` | 92–261 | `gro_faults` counted; no GRO cmsg means one datagram (documented); caller counts DROPPED_SOURCE or NO_BUFFER; `packet_bytes` within the ceiling |
| `sdk/rocketnet_server_gso_linux.cpp` | all P1, P1b, P3, P4 and P6 hits | idle waits; EINTR retry; `receive_errors`, `dropped_size`, `too_large`, `dropped_source`, `dropped_overflow` and `send_dropped` counted; stale pointer fails closed (comment 1200–1202); try-lock keeps the tail; P3 370 never fires (compare-exchange at 1171) |
| `sdk/rocketnet_coalesce.h` | 38–92 | empty input or run boundaries; −1 counted by the caller |
| `sdk/rocketnet_xdp_frame.h` | 118–635 | counted `send_refused_build`, `malformed`, `refused` or `descriptor`; invalid MAC ignored (comment 569); oldest-entry eviction documented (488–490); UDP length trims padding (comment 286–287) |
| `sdk/rocketnet_xdp_compat.h`, `sdk/rocketnet_xdp_socket_config.h` | all hits | caller logs at `sdk/rocketnet_server_socket_linux.cpp:1548`; config initialisation |
| `sdk/rocketnet_server_protocol.cpp` | 25, 32, 75, 97, 112, 179–240, 943, 980, 1070–1075, 1526, 1529, 1591 | documented range guard; budget table full (comments 64–70, 101–105); predicates; RECEIVE_CURSOR_TOO_SMALL counted; switch breaks |
| `sdk/rocketnet_server_protocol.cpp` | 1619, 1639, 1687–1883 | not connected or not due; missing mapping counted SLOT_WITHOUT_MAPPING |
| `sdk/rocketnet_server_protocol.cpp` | 1913–2180, 2222, 2261, 2265 | send refusals return 0 and the transport aborts the frame; unconnected disconnect counts zero; sequence 0 is a fragment (comment 1362); idempotent re-accept |
| `sdk/rocketnet_server_protocol.cpp` | P2 910; P3 397, 1673, 1954, 2001, 2122; P4 48, 189, 949, 1376, 1600, 1949, 2254 | comment; clamp named in `sdk/rocketnet_server_protocol.h:58`; ack truncation commented; rollover commented (36–39); resets after counted disconnects |
| `sdk/rocketnet_ack.cpp` | 101–143, 164–228, 403–412, 485–736, 872–879, 1015–1125, 1233, 1319–1452, 1506; P1b 281–292, 1518–1540 | block build walk (truncation commented 88–92); malformed block returns 0 and the caller counts *_BAD_ACK_BLOCK; `unknown_sequence` counted; below-window commented (1076); reconcile commented (1118–1120) |
| `sdk/rocketnet_ack.cpp`, `sdk/rocketnet_ack.h` | P3 85, 1085, 1248; P4 90–1565, ack.h 48–335; P6 222; P2 ack.h 243, 244 | commented truncation and floors; endpoint reset on create or slot reuse (comment 296–300); comments and declarations |
| `sdk/rocketnet_protocol.cpp` | 77–492; P4 155; P6 223, 342 | default lands on a named counter; seal and open refusals counted *_DID_NOT_OPEN by callers; bad family counted INTERNAL_ERROR; sizes within the ceiling |
| `sdk/rocketnet_encryption_map.cpp`, `.h` | all hits | invalid capacity; chain unlink; iterator end; create refusal; full counted MAPPING_TABLE_FULL by caller; "never evicted" comment |
| `sdk/rocketnet_token_tables.cpp`, `.h` | all hits | invalid capacity; live binding no-op (comment 347); full claim counted COULD_NOT_ADD_MAPPING; oldest-binding eviction documented as capacity-bounded (214–220, `sdk/rocketnet_token_tables.h:49–50`) |
| `sdk/rocketnet_slots.cpp` | 18, 23 | null guard; initialisation (the clamp at 19–21 is already filed) |
| `sdk/rocketnet_frame_sync.cpp` | 53, 149, 180–276, 366–427, 441–513; P4 185, 293, 393 | deadline passed; priority refusals logged `rocketnet_info`; create refusals logged; null guards record misuse |
| `sdk/rocketnet_connect_token.cpp` | 184, 348, 479, 485; P6 337, 347, 352, 420 | test-only NULL key; sealed block counted COULD_NOT_OPEN_SEALED_BLOCK; refusals logged; `sizeof` casts; log-only cast |
| `sdk/rocketnet_connection_reset.cpp` | 77, 167, 172–211 | out-of-range slot has no state; inventory checker writes `failure` |
| `sdk/rocketnet_send_budget.h` | 146–342; P3 163, 223, 258, 346 | size refusals and refusals counted on `refusals`; backward-time refill keeps `last_time` (comment 211–213); token cap is the `0 <= tokens <= depth` invariant |
| `sdk/rocketnet_message_server.cpp` | 81–314, 345–498 | layout and create refusals logged; DEV capture; kick of a bad or non-connected slot is a no-op |
| `sdk/rocketnet_message_server.cpp` | 528, 536, 800, 808, 920, 1080 | counted STALE_CONNECTION_ENTRY, or by the endpoint |
| `sdk/rocketnet_message_server.cpp` | 574–727, 939–1114, 1151–1369 | nothing waiting; per-update view reset; 0 to the API caller (comment "refused finish aborts its start", 1058; that the refusal is not logged is already filed); queue scan; release not found recorded as misuse (1303) |
| `sdk/rocketnet_message_server.cpp` | 1573–1612, 1624–1773 | logged with status; `rocketnet_api_null_guard` records misuse (1736 is finding 2) |
| `sdk/rocketnet_message_server.cpp` | P2 775, 813, 944; P4 155–1483 | function name `rn_message_send_transport_max`; initialisation and view resets |
| `sdk/rocketnet_message_client.cpp` | all hits | null guards record misuse; logged with status; negative capacity documented in `sdk/rocketnet.h` |
| `sdk/rocketnet_message.cpp` | all hits | validity predicates; 0 makes the builder end the packet and the endpoint count; empty plan commented; reader done |
| `sdk/rocketnet_channel.cpp` | 40–1500 (all hits but 1278 and 1389) | probe loops; caller validates the channel; exhaust counts MEMORY_EXHAUSTED; BAD_BLOCK or SLOT_COLLISION counted; refusals return 0 so the object cancels; P2 1326 comment; P6 bounded by W and `max_message_bytes` |
| `sdk/rocketnet_channel_sequenced.cpp` | 30, 48, 88, 97, 143, 185, 186, 363–458 (but 434, already filed) | SLOT_COLLISION counted by caller; no hold; counted or exhausted; future epoch (stale case counted at 415); counts bounded by W |
| `sdk/rocketnet_channel_receipts.cpp`, `sdk/rocketnet_channel_reliable.cpp`, `sdk/rocketnet_channel.h` | all hits | predicate; test-hook memset; comments |
| `sdk/rocketnet_endpoint.cpp`, `.h` | all hits | 0 to the object, which cancels; counted; loop or switch breaks; descriptors stay queued; pure queries; P2 `rn_endpoint_transport_max` is a name; reset for a new connection |
| `sdk/rocketnet_fragment.cpp`, `.h` | all hits but those already filed | each refusal counted with `rocketnet_spam`; sizing 0 logged at 127; allocation failure returns NULL; count at least 2 explained (52–55); per-flush reset documented |
| `sdk/rocketnet_packet_builder.cpp` | all hits | switch or loop end; stray class explained (163–164); unreliable counted at 461; reliable waits (comment) |
| `sdk/rocketnet_driver.cpp`, `sdk/rocketnet_driver.h` | all hits | registry loops; buffers sized from the registry (comment 95); open refusals with status; comment |
| `sdk/rocketnet_driver_builtin.cpp` | all hits | producer kept on a failed publish, caller counts `dropped` into SEND_FAILED; rx acquire kept (comment 239–243); copy clamp keeps the true size (P3 492, 493); bank reset at 512 counted at 511; create refusals logged; test hooks |
| `sdk/rocketnet_driver_loopback.cpp` | all hits but 320 and 364 | predicates; callers count SEND_FAILED via `loopback_send_failed`; queue-full drop counted `dropped`; 410 and 435 named by `dropped` and `sdk/rocketnet_driver_loopback.h:42–43` |
| `sdk/rocketnet_network_simulator.cpp`, `.h` | all hits but 487, 489 | memory refusal returns 0; scan and selection predicates; full queue spam-logged and documented; simulated loss by design (header); production stubs |
| `sdk/rocketnet_server_iocp_windows.cpp`, `sdk/rocketnet_server_iocp.h` | all hits but 804 | RECEIVE_DROPPED, RECEIVE_REPOST_FAILED, SEND_FAILED or USO_FALLBACKS counted; reset statuses counted; staged frames kept on a failed try (comments 973, 1060); P3 545, 979 overshoot already refused; P6 sizes within the URO buffer or ceiling |
| `sdk/rocketnet_server_rio_windows.cpp`, `sdk/rocketnet_server_rio.h`, `sdk/rocketnet_rio_layout.h` | all hits but 730, 731 | −1 named "a lost packet" (322–324); refused commit logged and retried; SEND_FULL, SEND_FAILED, SEND_WAITS, RECEIVE_FAILED or REPOST_FAILED counted; close deferred (434–436); 475 and 482 stall stop on a corrupt queue (not a discard); plan refusals documented (`sdk/rocketnet_rio_layout.h:50–52`) |

### `sdk/`, outside the data path (P2, P3, P6 and the widened P1/P1b)

| File | Lines | Why it is safe |
|---|---|---|
| `sdk/rocketnet_api.cpp` | 115–905, 1386–1873 (61 hits) | config, environment, create, connect, start, stop and destroy refusals: each sets a status or logs `rocketnet_error` |
| `sdk/rocketnet_api.cpp` | 928–1214, 1302–1356, 1896–2365 (83 hits, 1309 excepted) | null guards record misuse; `NOT_CONNECTED_DROPS` counted; finish or abort with nothing pending (comment "counted nowhere"); stopped server reads empty (comment); stats zeroed then returned; simulator refusals logged |
| `sdk/rocketnet_platform.cpp` | 1015–1220, 1445–1661, 2944–3195, 3354–3645, 4800–4989, 5306, 5310 (60 data-path hits) | ICMP and buffer weather commented; drain ends; zero-length skips commented or counted `receive_empty_count`; oversize keeps its true size (comments 1192–1194, 4876–4877); send tails counted and logged; compaction drops commented (1630–1635, 3606–3609); invalid destination logged; `goto receive_again` refusal documented in `sdk/rocketnet_address.h:103–108` |
| `sdk/rocketnet_platform.cpp` | remaining 156 hits | socket, wake, thread, mutex, route and adapter setup: refusals logged or returned as NULL, false or UNKNOWN; the buffer-size walk-down is commented (816–822) and warns |
| `sdk/rocketnet_platform.cpp`, `sdk/rocketnet_platform.h`, `sdk/rocketnet_driver.h` | P2 249, 323, 817, 1821, 1828, 3776, 3783; platform.h 1212, 1282; driver.h 9 | `sched_get_priority_min`, or comments |
| `sdk/rocketnet_platform.cpp` | P3 98, 100, 105, 1591, 1787, 3548, 3757, 3916; P6 4806–4951 | sleep-plan clamps documented (23–30); batch chunking loops to completion; wait clamped to a day (commented); nanosecond rounding; `sizeof` casts and bounded DWORD |
| `sdk/rocketnet.h` | P2 1668 | API comment; the returned count names every receipt beyond capacity |
| `sdk/rocketnet.cpp` | P6 257, 261 | length bounded by the 1024-byte log buffer |
| `sdk/rocketnet_allocator.cpp` | P3 1405, 1418; P6 1444 | diagnostic text only; `sizeof` |
| `sdk/rocketnet_config.cpp` | P6 79–865 (14 hits) | descriptor `offsetof`/`sizeof`; short names checked against the buffer; snprintf truncation refused (371); refusals logged |
| `sdk/rocketnet_keys.cpp` | P6 32 | `sizeof` in an assert |
| `sdk/rocketnet_server_gso_linux.cpp` | 222, 1485 | `counter_gro_cmsg_faults` is never read, but the resulting oversize drop is still counted `dropped_size` |
| `sdk/rocketnet_platform.cpp` | 1109, 1444, 3040, 3353, 4871 | refused destinations are logged with `rocketnet_error`; they do not bump `send_failures`, although the SEND_FAILED description in `sdk/rocketnet_counters.h` lists them |

### `tests/functional/` and `tests/*.h` (P5, 483 hits)

| File | Lines | Why it is safe |
|---|---|---|
| `tests/functional/channel_config.cpp`, `tests/functional/channel_control_packets.cpp`, `tests/functional/channel_receipts.cpp`, `tests/functional/channel_receipts_reliable.cpp`, `tests/functional/channel_reliable.cpp`, `tests/functional/channel_reliable_sequenced.cpp`, `tests/functional/channel_smoke.cpp`, `tests/functional/channel_unreliable_*.cpp` | all hits | failure path after `check(0)`; refused send retried with exact counts checked; check helpers; loop ends with outcomes checked after |
| `tests/functional/channel_ordered.cpp` | 164, 172, 201, 206, 242 | failure path; send gate retried; loop ends when complete (220 is finding 20) |
| `tests/functional/channel_harness.h` | 152, 165–250, 397–988 | capture off or active (commented); over budget counted `past_budget`; failures returned to callers; `overflow_count` and `held_overflow` counted; scripted WIRE_DROP; held, not dropped |
| `tests/functional/channel_resend_timing.cpp` | 31, 122, 202, 211, 215, 400, 403 | comment; oracle min/max; no send this update; predicate |
| `tests/functional/cpp_transports.cpp`, `tests/functional/lifecycle_driver.cpp`, `tests/functional/main.cpp`, `tests/functional/platform_primitives.cpp` | all hits | wait or success loop exits followed by asserts; group return; thread sets `ok = false`, checked |
| `tests/functional/harness.inc` | 56, 102, 108, 187–189, 271 | setenv without overwrite; NONE level printed; prefix width commented (78–81); env guard; free port found |
| `tests/functional/live_matrix.cpp` | all hits | `live_matrix_fail`, asserted counters, or comments (386, 513, 601) |
| `tests/functional/posix_hard_killed_client.cpp` | all hits but 382, 399, 513, 525 | the dead child's slot by design; scans and run ends; port retry |
| `tests/functional/posix_lifecycle_stress.cpp` | all hits | directory entries; cell not selected, including the per-commit tier of destroy-under-traffic (comment 1136–1139); UMEM bytes 0 on a build without the xdp transport (comment 571–572); reservation failures checked at 1041 |
| `tests/functional/posix_socket_errors.cpp` | all hits | check helper; drained; empty datagram commented (437); duplicates printed by the summary line (1320) |
| `tests/functional/server_receive_backlog.cpp`, `tests/functional/windows_connreset.cpp`, `tests/functional/windows_socket_errors.cpp` | all hits | check helpers; warm-up commented; killed child's slot; empty datagram commented (`tests/functional/windows_socket_errors.cpp:468`) |
| `tests/functional/rocketnet_functional_tests.cpp` | 90 hits but 6815–6859 | wait-loop exits followed by checks; switch breaks; SEND_RING_FULL counted (627, 629); retry polls; commented skips; callers assert the counter (5415, 5430); the receive loops at 2054–2132 test the source address, not arrival |
| `tests/counters_expect.h`, `tests/live_verify.h`, `tests/native_shutdown.h`, `tests/unit_shutdown.h`, `tests/portable_crt.h`, `tests/rocketnet_value_tracker.h`, `tests/test_unit_standard.h`, `tests/wycheproof.h` | all hits | bad index reported; switch or comment; caller fails "refused"; NULL guards; documented (20–22); no samples; nibble parsing; all-or-nothing loader |
| `tests/dst_checks.h` | all hits but 192, 194, 196 | first failure kept; laps reported loudly; below base commented; predicates; self or unconnected; explained refusal list (985) |
| `tests/dst_harness.h` | all hits but 501, 554 | `dst_fail`; first failure kept; not connected; verify failures call `dst_fail`; create failure zeroes the result so `passed = 0` |
| `tests/fake_client_driver.h` | 91 | drained |
| `tests/pcapng.h` | 362, 412–428 | end of options; refusal returned as 0 |
| `tests/serialize.h` | all hits | documented; vector switches; surrogate decode; stand-down printed; a failed 320 MB allocation is printed under verbose |
| `tests/suite_runner.h` | all hits | commented; drain or line end; declared bound found (219); a nightly-tier case left out of a per-commit run (comment 564–565); spawn failure recorded as failed; still running |
| `tests/plan_rss.h` | 167 | `free` of NULL is a no-op |
| `tests/test_unit_registry.h` | 168, 179 | return codes |

## Already filed

| Instance | Filed as |
|---|---|
| `sdk/rocketnet_stats_window.h:13`, and the client copy at `sdk/rocketnet_client_transport.cpp:2281` | `docs/audits/2026-10-24-code-quality-a.md` finding 1 |
| `sdk/rocketnet_packet_loss_tracker.h:91` | `docs/audits/2026-10-24-code-quality-a.md` finding 2 |
| `sdk/rocketnet_window_send.cpp:19` | `docs/audits/2026-10-24-code-quality-a.md` finding 3 |
| `sdk/rocketnet_fragment.cpp:312` (and 265) | `docs/audits/2026-10-24-code-quality-a.md` finding 4 |
| `sdk/rocketnet_packet_lease.h:40` | `docs/audits/2026-10-24-code-quality-a.md` finding 6 |
| `sdk/rocketnet_window_send.cpp:193` | `docs/audits/2026-10-24-code-quality-a.md` finding 12 |
| `sdk/rocketnet_out_of_order_tracker.h:49` | `docs/audits/2026-10-24-code-quality-a.md` finding 13 |
| `sdk/rocketnet_window_send.cpp:32` | `docs/audits/2026-10-24-code-quality-b.md` finding 2 |
| `sdk/rocketnet_client_slots.cpp:294` | `docs/audits/2026-10-24-code-quality-b.md` finding 3 |
| `sdk/rocketnet_slots.cpp:19` | `docs/audits/2026-10-24-code-quality-b.md` finding 9 |
| `tests/test_unit_registry.h:194` (the run result dropped by an uncalled runner) | `docs/audits/2026-10-24-code-quality-b.md` finding 10 |
| `sdk/rocketnet_channel_sequenced.cpp:434` (a held sequenced message released uncounted when `rn_channels_deliver_sequenced` refuses it) | `docs/audits/2026-10-24-area-protocol-channels-messages.md` finding 1 |
| `sdk/rocketnet_server_protocol.cpp:2262` (payload accept passes an unchecked mapping index, −1 included, so the deferred ack is lost uncounted) | `docs/audits/2026-10-24-sweep-assert-only-guards.md` finding 1 |
| `sdk/rocketnet_server_iocp_windows.cpp:800` (the `packet_id` half of finding 8) | `docs/audits/2026-10-24-sweep-assert-only-guards.md` finding 3 |
| `sdk/rocketnet_server_rio_windows.cpp:464` (handed send phases cleared at stop with no counter) | `docs/audits/2026-10-24-area-transports-windows.md` finding 2 |
| `sdk/rocketnet_message_server.cpp:1058` (a refused finish aborts its start with no log line) | `docs/audits/2026-10-24-area-config-api.md` finding 2 |
| `sdk/rocketnet_packet_loss_tracker.h:71` (a late packet overwrites a newer sequence's ring slot) | `docs/audits/2026-10-24-area-protocol-acks-fragments.md` finding 2 |
| `sdk/rocketnet_xdp_replay_marks.h:67` (the `refused` tally zeroed at every commit and never read) | `docs/audits/2026-10-24-area-transports-linux-af-xdp.md` finding 18 |

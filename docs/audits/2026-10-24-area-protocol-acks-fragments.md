# Protocol acks, sequences and fragments audit, 2026-10-24

Cold reading audit of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd`
(`main`). Scope read completely, top to bottom: `sdk/rocketnet_protocol.cpp` and `.h`,
`sdk/rocketnet_ack.cpp` and `.h`, `sdk/rocketnet_window_ack.cpp` and `.h`,
`sdk/rocketnet_window_receive.cpp` and `.h`, `sdk/rocketnet_window_send.cpp` and `.h`,
`sdk/rocketnet_packet_loss_tracker.h`, `sdk/rocketnet_jitter_tracker.h`,
`sdk/rocketnet_out_of_order_tracker.h`, `sdk/rocketnet_history.h`, `sdk/rocketnet_fragment.cpp` and
`.h`, and the tests that pin them (`tests/test_unit_ack_*.cpp`, `tests/test_unit_standard_acks.cpp`,
`tests/test_unit_window_sequences.cpp`, `tests/test_unit_sequence_wrap_session.cpp`,
`tests/test_unit_standard_fragmentation.cpp`, `tests/test_unit_packet_loss_tracker.cpp`), with
`standard/TRANSPORT.md` (t4, t14, t15, t16) and the call sites in the client and server protocol
and transport files open beside them. Channels, reliability and the message layer above the
window tables are out of scope. Method: for each candidate, the input or call sequence that
reaches it was written down first; a candidate with no reachable path is graded risk or quality,
never bug. One candidate was settled by a throwaway unit test (not committed), as its finding
says. No code was changed. Findings are ordered most severe first; each gives severity (`bug` /
`risk` / `quality`), `file:line` at 9775266a, a one-sentence defect, a one-sentence concrete
failure or cost, and the smallest fix. Instances already filed in `docs/audits/` are listed once
under Already filed and are not repeated as findings.

## Findings

### 1. risk — `sdk/rocketnet_server_protocol.cpp:1253`

- Defect: A type 9 fragment packet is marked ACK-eligible (`rocketnet_replay_advance` on `accepted_session`, line 1254; the client does the same at `sdk/rocketnet_client_protocol.cpp:902`) as soon as its opened size passes, before reassembly runs the t16.2 checks, so a fragment that reassembly then refuses (stale, count mismatch, size mismatch, payload too large, duplicate index) and every fragment of a payload later abandoned or dropped on batch full is still acknowledged, while t15.3 makes a packet eligible only after its type data passes validation and holds a payload's acknowledgement until it is processed.
- Failure or cost: A reordered late fragment of payload X that arrives after payload X + num_slots has displaced X's slot is refused stale yet acknowledged, so the sender's aggregate (`sdk/rocketnet_ack.cpp:1190`) completes and surfaces X's packet id as acknowledged for a payload the receiver never delivered; today the only producer of fragmented payloads (large unreliable-sequenced state, `sdk/rocketnet_endpoint.cpp:835`) commits no sent-map entry for that id, so nothing acts on the false acknowledgement yet, but any receipt or reliable use of a fragmented packet id would report delivery that did not happen.
- Smallest fix: For type 9, advance `accepted_session` (and add the ack debt) only when `rocketnet_reassembly_process_fragment` returns 1, on both the client and the server path, and state in a comment that a completed-then-dropped payload (batch full) still acknowledges.

### 2. risk — `sdk/rocketnet_packet_loss_tracker.h:71`

- Defect: `rocketnet_packet_loss_tracker_packet_received` stamps the ring slot for any sequence, including one at or below `last_packet_processed` that has already been judged, so a packet arriving more than `ROCKETNET_PACKET_LOSS_TRACKER_HISTORY` (1024) sequences late overwrites the slot of the sequence 1024 above it.
- Failure or cost: When that newer sequence is received but not yet judged (within the 30-sequence safety margin), the next update counts it lost although it arrived, so one late packet is counted lost twice (itself, already judged, and the newer packet it clobbered); checked with a throwaway unit test that feeds 0..1100 without 50, updates every 10, then delivers 50 late and 1101..1200: the second phase reports 1 lost where 0 is right. The replay window (4096) admits such a packet, and the client feeds every accepted session packet to the tracker (`sdk/rocketnet_client_transport.cpp:2063`), though the tracker's result is not published today (finding 4).
- Smallest fix: Return without writing when `sequence <= tracker->last_packet_processed` (after the `sequence++`), since a judged sequence can only clobber.

### 3. quality — `sdk/rocketnet_window_ack.cpp:834`

- Defect: `rn_window_ack_resend` and its `rn_window_ack_resend_fifo` walk (lines 794–849) are a second implementation of the resend walk, marked test-only in `sdk/rocketnet_window_ack.h:240`, while the product walks the same FIFOs in `rn_ordered_write` (`sdk/rocketnet_channel.cpp:1251`); no production code calls them, yet they ship in every library.
- Failure or cost: The resend timing tests and the standard's resend vectors (`tests/test_unit_resend_timing.cpp:84`, `tests/test_unit_standard_resend_and_budget.cpp:71`) exercise this copy, so the product's walk can drift from the documented due-order and no-back-off rules while those tests stay green.
- Smallest fix: Point the tests at the product walk (or a thin wrapper over `rn_ordered_write`) and delete the copy, or compile it only into the unit-test library.

### 4. quality — `sdk/rocketnet_client_transport.cpp:2063`

- Defect: The client feeds every accepted session packet to the packet loss tracker and the jitter tracker, and sweeps the loss tracker every update, but the stats getter publishes `protocol.ack.packet_loss` and `protocol.ack.jitter` (lines 3185–3186); `client_stats.packet_loss`, which the tracker drives (line 2335), and `jitter_tracker.jitter` are read only by test seams.
- Failure or cost: Two per-packet trackers and their sweep run on the hot receive path and are maintained and audited as if they fed published statistics, when the published loss and jitter come from the ack endpoint, so a reader cannot tell which number a user sees without tracing the getter.
- Smallest fix: Delete both trackers, their feeds and the `client_stats.packet_loss` computation (keeping the getter's ack-endpoint sources), or publish them under their own names.

### 5. quality — `sdk/rocketnet_ack.cpp:751`

- Defect: The running jitter is `( stats_rtt_sum - count * min ) / count` in double over a sum that is only ever added to and subtracted from, while the reference recompute (lines 1332–1346) sums `sample - min_rtt` in float, yet the comments at `sdk/rocketnet_ack.h:211` and `sdk/rocketnet_ack.cpp:319` say the running aggregates hold exactly the numbers `update_stats` recomputes.
- Failure or cost: The two jitter values differ in rounding (the oracle at `tests/test_unit_server_stats_current.cpp:103` compares jitter with a relative tolerance while every other field is compared exactly), so the comment's exactness claim is false for the one field a maintainer would most likely test with `==`.
- Smallest fix: Say in both comments that jitter matches within floating-point rounding and every other field exactly, or compute the reference jitter in the same double form.

### 6. quality — `sdk/rocketnet_fragment.h:64`

- Defect: The reassembly comment says "Reassembly is one set per connection slot", but the server creates one set for all connections (`sdk/rocketnet_server_transport.cpp:1787`, `num_connections = max_clients`) and indexes connections inside it, as the struct and the comment at line 123 describe.
- Failure or cost: A reader sizing memory or reasoning about the delivered arena from this sentence multiplies the set (and its one delivered arena) by the client count, which is the wrong model of the per-update batch that BATCH_FULL guards.
- Smallest fix: Reword to "one set per transport, with num_slots reassembly slots per connection".

## Counts

- bug: 0
- risk: 2 (findings 1, 2)
- quality: 4 (findings 3–6)
- total: 6

## Already filed

- `sdk/rocketnet_packet_loss_tracker.h:91`, a gap of 1024 or more discarded as zero lost: `docs/audits/2026-10-24-code-quality-a.md` finding 2 (the number it feeds is not published; see finding 4 here).
- `sdk/rocketnet_window_send.cpp:19`, `ring_bytes` non-zero checked only by an assert: `docs/audits/2026-10-24-code-quality-a.md` finding 3.
- `sdk/rocketnet_fragment.cpp:312`, `connection_index` bounded only by an assert: `docs/audits/2026-10-24-code-quality-a.md` finding 4.
- `sdk/rocketnet_window_send.cpp:193`, `rn_window_send_can_write_unreliable` always true and uncalled: `docs/audits/2026-10-24-code-quality-a.md` finding 12.
- `sdk/rocketnet_out_of_order_tracker.h:49`, the out-of-order tracker has no production caller: `docs/audits/2026-10-24-code-quality-a.md` finding 13.
- `sdk/rocketnet_protocol.cpp:421`, the dead `best_distance` store: `docs/audits/2026-10-24-code-quality-a.md` finding 15.
- `sdk/rocketnet_window_send.cpp:32`, `count <= W` asserted only before `W - count`: `docs/audits/2026-10-24-code-quality-b.md` finding 2.
- `sdk/rocketnet_protocol.cpp:35`, the refusal-code comment above the wrong function: `docs/audits/2026-10-24-code-quality-b.md` finding 7.

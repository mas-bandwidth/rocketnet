# Count-before-success sweep, 2026-10-24

A sweep of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd` (`main`) for one bug class: a counter, budget, quota, reference count, sequence or index that is charged (incremented, reserved, advanced, acquired) before the operation it pays for succeeds and is never refunded on the failure return, so a failure permanently consumes capacity or inflates a published counter. Scope read: every `.cpp` and `.h` under `sdk/` except `sdk/rocketnet_tests.cpp` (it builds only into the `rocketnet_test_support` target, and no production source calls into it), plus `functional/` (`.cpp`, `.h`, `.inc`) and the `tests/*.h` helpers the functional and DST tiers share. Method: grep, then for every hit the enclosing function was read, the charged value was followed to its use, every return after the charge was checked for a refund, and a candidate defect's callers were traced back to the public API (`sdk/rocketnet.h`), the wire or a config value. Patterns and hit counts: the brief's raw pattern `(\+\+|\+= |fetch_add|--|-= |fetch_sub)` over `sdk/` gives 2124 lines; 725 comment lines and 476 `for (` loop headers were set aside, and the rest were split into (A) a member or field update, `(->|\.)name([...])? (++|+=|-=|--)`, a prefix `++`/`--` on a member, or `fetch_add`/`fetch_sub`: 504 hits; (B) an array element or `g_`/`s_` global update: 137 hits; (H) a bare identifier update (locals, parameters, cursors), widened in after the first pass to catch a local later stored into state: 281 hits. Reservations, `(reserve|claim|acquire|admit|lease_insert|alloc_slot|take)[A-Za-z_0-9]*\s*\(` over `sdk/` (C): 184 hits. The shared test tiers: the A and B patterns over `functional/` and `tests/*.h` (D): 137 hits; the reservation pattern there (E): 19 hits. Reading showed charges the first patterns missed (an atomic add helper charging sent statistics, a counting helper charging a published counter), so two patterns were widened in: atomic add/sub helper calls, `atomic_(add|sub|increment|decrement)[A-Za-z_0-9]*\s*\(`, 54 hits, and counting-helper calls, `\b(rn|rocketnet)_[A-Za-z_0-9]*_count\s*\(`, 68 hits; 122 of these were new (F: 46 judged safe, 8 findings; G: 67 safe, 1 finding). In all 1384 distinct hits were judged: 18 belong to the six findings below, 3 to findings already filed, and 1363 are judged not a defect, each with its row in the table, plus one instance found by reading. No code was changed. Findings are ordered most severe first; each gives severity (`bug` / `risk` / `quality`), `file:line` at 9775266a, a one-sentence defect, a one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_endpoint.cpp:1098`

- Defect: `rn_endpoint_write_packet` charges a large unreliable-sequenced packet's planned wire bytes (`large_admitted`) to the send budget's `tokens` and `bytes_spent` (lines 1098–1099) before the transport's `finish_packet` runs, and the failure path, `rn_endpoint_packet_failed`'s `RN_ENDPOINT_PACKET_SEQUENCED_LARGE` case (lines 1219–1223, reached from `sdk/rocketnet_message_server.cpp:1121`), counts the send failed but never settles the charge back.
- Failure or cost: A capped connection (the default: 1000 kbps client, 10000 kbps per server slot) whose message is larger than one packet comes from the send arena, so its wire frames are reserved only at finish; when the server's send frames are all in use (`sdk/rocketnet_server_transport.cpp:2873`, the condition `ROCKETNET_COUNTER_SEND_RING_FULL` counts, reached with every slot connected and sending) or the client's payload ring is full (`sdk/rocketnet_client_transport.cpp:2741`), the message is dropped and the connection still loses every fragment's maximum wire bytes from its bucket, so it sends below its configured rate until the refill restores them, exactly when the server is busiest.
- Smallest fix: In the `RN_ENDPOINT_PACKET_SEQUENCED_LARGE` case of `rn_endpoint_packet_failed`, call `rocketnet_send_budget_settle( &endpoint->budget, endpoint->large_admitted, 0 )` when `send_budget_enabled`, as `rn_endpoint_packet_sent` does with the real size.

### 2. risk — `sdk/rocketnet_endpoint.cpp:585`

- Defect: `rn_endpoint_in_place_finish` admits an in-place packet's maximum wire bytes to the send budget before the transport's `finish_packet`, and `rn_endpoint_in_place_failed` (lines 615–624, called from `sdk/rocketnet_message_server.cpp:1051` when the finish returns 0) zeroes `in_place_admitted` at line 623 without settling it.
- Failure or cost: Any in-place send whose finish fails after the admit (a refused seal, a lease that no longer resolves, a slot that lost its mapping between start and finish) keeps up to one maximum packet of tokens charged for bytes that never left, throttling that connection's next sends; at a low cap, where the bucket holds about one maximum packet, one such failure stalls the connection until the next refill.
- Smallest fix: In `rn_endpoint_in_place_failed`, call `rocketnet_send_budget_settle( &endpoint->budget, endpoint->in_place_admitted, 0 )` when `send_budget_enabled` and `in_place_admitted > 0`, before zeroing it.

### 3. risk — `sdk/rocketnet_packet_builder.cpp:439`

- Defect: `rn_packet_builder_write` admits each ordered or packed-unreliable packet to the send budget and records the charge in `admitted_wire_bytes` (line 476), but only `rn_endpoint_packet_sent` settles it; the `RN_ENDPOINT_PACKET_ORDERED` and `RN_ENDPOINT_PACKET_UNRELIABLE` cases of `rn_endpoint_packet_failed` (`sdk/rocketnet_endpoint.cpp:1206–1217`) call neither `rn_ordered_settle` nor `rn_packet_builder_settle`.
- Failure or cost: A write-pass packet whose transport finish fails keeps its maximum wire bytes charged; for an ordered packet `rn_ordered_packet_failed` makes the same records due again, so they are charged a second time when resent, and the connection pays twice for one delivery.
- Smallest fix: In those two cases of `rn_endpoint_packet_failed`, call `rn_ordered_settle( &endpoint->ordered, 0, 0 )` and `rn_packet_builder_settle( &endpoint->unreliable_builder, 0, 0 )` respectively, which refund the whole admitted charge.

### 4. risk — `sdk/rocketnet_message_server.cpp:354`

- Defect: `rn_message_flush_unreliable` admits the sealed unreliable build to the send budget before `finish_packet` (line 365) and settles only when the finish returns wire bytes (lines 366–373); the failure branch (lines 374–379) counts the records failed and emits receipt losses but leaves `admitted` charged.
- Failure or cost: Each flush whose finish fails on a capped connection removes up to one maximum packet from the bucket with nothing sent, so later unreliable sends on that connection are dropped as over budget (`ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET`) although the budget was never used.
- Smallest fix: In the `else if ( records > 0 )` branch, call `rocketnet_send_budget_settle( &connection->budget, admitted, 0 )` when `admitted` is non-zero.

### 5. risk — `sdk/rocketnet_server_transport.cpp:852`

- Defect: The server charges a datagram's sent statistics (`packets_sent`, `bytes_sent` and the per-client wire totals) before or regardless of `rocketnet_server_link_send`, which drops the datagram when the link simulator's mutex is busy (lines 668–671, counted only as `ROCKETNET_COUNTER_SEND_RING_FULL`): at lines 852–855 in `rocketnet_server_commit_send_frame`, at 3087–3089 before `rocketnet_server_link_send_batch` in the flush, and at 2781–2783 and 2983–2986 (through `rocketnet_server_wire_totals_add_sent_atomic`, lines 453–459) after a link send whose result is not known; the arena finish also withdraws frames it already committed and charged when a later fragment's seal fails on a transport without `ROCKETNET_TRANSPORT_FINISH_FINAL` (lines 2922–2926), with no refund.
- Failure or cost: In a development build with the link simulator active, two game threads finishing packets at once on a staged transport (or any other holder of the simulator lock during a send) drop a datagram that `rocketnet_server_stats` and the per-client totals still report as sent, so "UDP datagrams sent" exceeds what reached the simulator by every lock miss.
- Smallest fix: Make `rocketnet_server_link_send` return whether it accepted the datagram, and charge the sent statistics only when it did (the batch path charges per accepted send).

### 6. risk — `sdk/rocketnet_channel.cpp:1409`

- Defect: `rn_ordered_write` counts `ROCKETNET_COUNTER_MESSAGE_RESENT` for every resend item the builder placed in a packet, before the transport's `finish_packet`; when the finish fails, `rn_endpoint_packet_failed` calls `rn_ordered_packet_failed` (line 1421), which makes the same records due again, and the next write counts them again.
- Failure or cost: Each failed finish of an ordered packet that carried resends inflates the published resend counter by those records, so `MESSAGE_RESENT` (class path_loss) reports wire loss for records that never reached the wire even once more.
- Smallest fix: Count the resends in `rn_ordered_commit` (the transport-sent commit) instead of at write, from the committed packet's resend items.
- Status (2026-10-24): fixed. `MESSAGE_RESENT` is counted at `rn_packet_builder_commit`, when the packet carrying each number is sent; a refused resend whose original is acked before its retry is never counted. Covered by `test_ordered_refused_resend_acked_before_retry_not_counted`, `test_ordered_refused_resends_counted_only_when_sent` and `test_ordered_failed_resend_counted_once` in `tests/test_unit_channel_ordered.cpp`.

## Counts

- bug: 1 (finding 1)
- risk: 5 (findings 2–6)
- quality: 0
- total: 6

## Judged not a defect

One row per grep hit judged safe (1363), plus one instance found by reading (`read`). Pattern: A member or field update, B array element or global update, C reservation call in `sdk/`, D counter update in the shared test tiers, E reservation call in the shared test tiers, F atomic add/sub helper call, G counting-helper call, H bare identifier update. The 18 hits that belong to findings 1–6 are `sdk/rocketnet_endpoint.cpp:585, 1098, 1099`, `sdk/rocketnet_packet_builder.cpp:439`, `sdk/rocketnet_message_server.cpp:354`, `sdk/rocketnet_server_transport.cpp:453, 454, 458, 459, 852, 853, 2781, 2782, 2983, 2984, 3087, 3088` and `sdk/rocketnet_channel.cpp:1409`; the 3 already filed are listed under Already filed.

| Hit | Pattern | Why it is safe |
|---|---|---|
| `functional/channel_harness.h:184` | D | counts the failure on its own past-budget refusal path |
| `functional/channel_harness.h:189` | D | charged after the packet block was written |
| `functional/channel_harness.h:414` | D | counts the failure on its own overflow refusal path |
| `functional/channel_harness.h:429` | D | charged after slot linked in; no later failure |
| `functional/channel_harness.h:457` | D | decrement on release, after unlink succeeded; matching add in ring_push |
| `functional/channel_harness.h:480` | D | counts the failure on its own overflow refusal path |
| `functional/channel_harness.h:483` | D | charged after the bounds checks; no failure after it |
| `functional/channel_harness.h:499` | D | counts bytes offered to the link; WIRE_DROP is simulated loss, not failure |
| `functional/channel_harness.h:501` | D | counts bytes offered to the link; WIRE_DROP is simulated loss, not failure |
| `functional/channel_harness.h:639` | D | after start_payload succeeded; ids need uniqueness only, gaps harmless |
| `functional/channel_harness.h:663` | D | charged only when seal returned wire_bytes > 0 |
| `functional/channel_harness.h:686` | D | counts bytes the endpoint transmitted; drop/hold are simulated link behaviour |
| `functional/channel_harness.h:688` | D | counts bytes the endpoint transmitted; drop/hold are simulated link behaviour |
| `functional/channel_harness.h:696` | D | counts the failure on its own held-capacity refusal path |
| `functional/channel_harness.h:702` | D | charged after the held copy was stored |
| `functional/channel_harness.h:1014` | D | test clock advance, not a capacity counter |
| `functional/channel_ordered.cpp:120` | D | local histogram bin, does not outlive the call |
| `functional/channel_ordered.cpp:183` | D | precomputed planned payload total for the lower bound, not a success count |
| `functional/channel_ordered.cpp:238` | D | charged after the exact-match check (mismatch aborts via check) |
| `functional/channel_resend_timing.cpp:225` | D | bound checked fatally before; records an observed send |
| `functional/channel_smoke.cpp:156` | D | per-index seen count; duplicate detection on its own result |
| `functional/channel_smoke.cpp:168` | D | per-index seen count; duplicate detection on its own result |
| `functional/channel_smoke.cpp:284` | E | function definition matched on name, no acquire |
| `functional/channel_unreliable_duplicates.cpp:100` | D | send failure is fatal via dup_check (exit), nothing continues |
| `functional/channel_unreliable_duplicates.cpp:110` | D | send failure is fatal via dup_check (exit), nothing continues |
| `functional/lifecycle_driver.cpp:45` | D | counts allocation calls including injected failures, by design (fail_at index) |
| `functional/lifecycle_driver.cpp:55` | D | charged after malloc succeeded |
| `functional/lifecycle_driver.cpp:64` | D | decrement on release; matching add in allocate |
| `functional/live_matrix.cpp:225` | D | counts the failure itself |
| `functional/live_matrix.cpp:262` | E | read-only room query, reserves nothing |
| `functional/live_matrix.cpp:294` | E | drains receipts, reserves nothing |
| `functional/live_matrix.cpp:319` | D | charged after the entry was found and marked receipted; bad status fails separately |
| `functional/live_matrix.cpp:322` | D | charged on ACKED status only |
| `functional/live_matrix.cpp:326` | D | counts LOSS status itself |
| `functional/live_matrix.cpp:383` | D | charged after rocketnet_live_check verified the arrival |
| `functional/live_matrix.cpp:385` | D | charged after rocketnet_live_check verified the arrival |
| `functional/live_matrix.cpp:412` | D | charged after every refusal check returned |
| `functional/live_matrix.cpp:416` | D | charged after every refusal check returned |
| `functional/live_matrix.cpp:418` | D | charged after every refusal check returned |
| `functional/live_matrix.cpp:420` | D | charged after every refusal check returned |
| `functional/live_matrix.cpp:487` | D | token refill, clamped to rate; not paying for an operation |
| `functional/live_matrix.cpp:490` | D | token refill, clamped to rate; not paying for an operation |
| `functional/live_matrix.cpp:511` | E | read-only room query; refusal counted as paced, nothing reserved |
| `functional/live_matrix.cpp:514` | D | counts the held-back send itself |
| `functional/live_matrix.cpp:534` | D | charged after start_finish returned a nonzero serial |
| `functional/live_matrix.cpp:545` | D | charged after serial and receipt slot (fatal check) both succeeded |
| `functional/live_matrix.cpp:548` | D | charged after start_finish returned a nonzero serial |
| `functional/live_matrix.cpp:549` | D | charged after start_finish returned a nonzero serial |
| `functional/live_matrix.cpp:551` | D | tokens spent only after the send succeeded |
| `functional/live_matrix.cpp:553` | D | tokens spent only after the send succeeded |
| `functional/live_matrix.cpp:726` | D | counts refusal probes made; wrong outcome already recorded as failure |
| `functional/live_matrix.cpp:768` | D | charged after a nonzero serial and receipt slot claimed |
| `functional/live_matrix.cpp:794` | E | drains receipts, reserves nothing |
| `functional/live_matrix.cpp:795` | E | drains receipts, reserves nothing |
| `functional/live_matrix.cpp:841` | D | reclassifies the just-charged failure as an edge failure |
| `functional/live_matrix.cpp:842` | D | counts the never-arrived failure itself |
| `functional/live_matrix.cpp:859` | E | drains receipts, reserves nothing |
| `functional/live_matrix.cpp:860` | E | drains receipts, reserves nothing |
| `functional/live_matrix.cpp:895` | D | local search for the closing sequence, reset each window |
| `functional/live_matrix.cpp:933` | E | drains receipts, reserves nothing |
| `functional/live_matrix.cpp:934` | E | drains receipts, reserves nothing |
| `functional/live_matrix.cpp:979` | D | iteration counter, not paying for an operation |
| `functional/live_matrix.cpp:1011` | D | counts the undelivered shortfall itself |
| `functional/live_matrix.cpp:1041` | D | counts the counter failure itself |
| `functional/platform_primitives.cpp:159` | D | charged after every check of the minted token passed |
| `functional/platform_primitives.cpp:235` | D | wrong count sets ok=false, which the test asserts |
| `functional/posix_hard_killed_client.cpp:383` | D | charged after payload validated |
| `functional/posix_hard_killed_client.cpp:404` | D | charged after payload validated and slot matched |
| `functional/posix_hard_killed_client.cpp:424` | D | send_packet is void; counts calls to a connected slot, which is what line 733 asserts |
| `functional/posix_hard_killed_client.cpp:427` | D | send_packet is void; a silent drop makes received != sent fail the test |
| `functional/posix_hard_killed_client.cpp:447` | D | send_packet is void; a silent drop makes received != sent fail the test |
| `functional/posix_hard_killed_client.cpp:514` | D | charged after payload validated |
| `functional/posix_hard_killed_client.cpp:528` | D | charged after payload validated and slot matched |
| `functional/posix_lifecycle_stress.cpp:609` | D | clock skip counter, not paying for an operation |
| `functional/posix_lifecycle_stress.cpp:928` | E | computes a port; the file it opens is closed on every path |
| `functional/posix_lifecycle_stress.cpp:1014` | E | call to a pure port computation, nothing held |
| `functional/posix_socket_errors.cpp:418` | D | called after sendto checked fatally for full send |
| `functional/posix_socket_errors.cpp:420` | D | called after sendto checked fatally for full send |
| `functional/posix_socket_errors.cpp:422` | D | called after sendto checked fatally for full send |
| `functional/posix_socket_errors.cpp:424` | D | called after sendto checked fatally for full send |
| `functional/posix_socket_errors.cpp:446` | D | charged after sendto checked fatally for full send |
| `functional/posix_socket_errors.cpp:448` | D | charged after sendto checked fatally for full send |
| `functional/posix_socket_errors.cpp:450` | D | charged after sendto checked fatally for full send |
| `functional/posix_socket_errors.cpp:1164` | E | consumes received packets, reserves nothing |
| `functional/posix_socket_errors.cpp:1180` | D | counts the duplicate itself |
| `functional/posix_socket_errors.cpp:1184` | D | charged after the packet validated and was new |
| `functional/posix_socket_errors.cpp:1203` | E | consumes received packets, reserves nothing |
| `functional/posix_socket_errors.cpp:1315` | E | consumes received packets, reserves nothing |
| `functional/posix_socket_errors.cpp:1969` | D | caller sends first; posix_send_datagram fails fatally on short send |
| `functional/posix_socket_errors.cpp:1971` | D | caller sends first; posix_send_datagram fails fatally on short send |
| `functional/posix_socket_errors.cpp:1973` | D | caller sends first; posix_send_datagram fails fatally on short send |
| `functional/posix_socket_errors.cpp:1975` | D | caller sends first; posix_send_datagram fails fatally on short send |
| `functional/posix_socket_errors.cpp:2187` | D | charged after posix_send_datagram, which fails fatally on short send |
| `functional/posix_socket_errors.cpp:2188` | D | charged after posix_send_datagram, which fails fatally on short send |
| `functional/posix_socket_errors.cpp:2200` | D | charged after posix_send_datagram, which fails fatally on short send |
| `functional/posix_socket_errors.cpp:2204` | D | charged after posix_send_datagram, which fails fatally on short send |
| `functional/posix_socket_errors.cpp:2206` | D | charged after posix_send_datagram, which fails fatally on short send |
| `functional/posix_socket_errors.cpp:2215` | D | charged after posix_send_datagram, which fails fatally on short send |
| `functional/rocketnet_functional_tests.cpp:289` | D | counts every callback by design; array write bounded at 16 |
| `functional/rocketnet_functional_tests.cpp:1114` | D | local copy mutated to forge a context, does not outlive the loop |
| `functional/rocketnet_functional_tests.cpp:1115` | D | local copy mutated to forge a context, does not outlive the loop |
| `functional/rocketnet_functional_tests.cpp:1116` | D | local copy mutated to forge a context, does not outlive the loop |
| `functional/rocketnet_functional_tests.cpp:1900` | G | test reads the misuse count into a local; no charge |
| `functional/rocketnet_functional_tests.cpp:1912` | G | test compares the misuse count; no charge |
| `functional/rocketnet_functional_tests.cpp:6767` | E | std::unordered_map capacity reserve, freed with the local map |
| `functional/rocketnet_functional_tests.cpp:6768` | E | std::unordered_map capacity reserve, freed with the local map |
| `functional/rocketnet_functional_tests.cpp:6820` | D | charged after the payload validated and sequence in range |
| `functional/rocketnet_functional_tests.cpp:6834` | D | charged after the payload validated, slot matched, sequence in range |
| `functional/rocketnet_functional_tests.cpp:8940` | E | extern declaration matched on name, no acquire |
| `functional/rocketnet_functional_tests.cpp:9969` | G | returns a list length; not a counter |
| `functional/rocketnet_functional_tests.cpp:9974` | G | list length used as a bound; not a counter |
| `functional/rocketnet_functional_tests.cpp:9975` | G | list length used as an index offset; not a counter |
| `functional/rocketnet_functional_tests.cpp:9996` | G | local loop bound |
| `functional/server_receive_backlog.cpp:163` | E | std::vector capacity reserve, freed with the local vector |
| `functional/windows_connreset.cpp:406` | D | tick sequence stamp, not paying for an operation |
| `functional/windows_connreset.cpp:428` | D | charged after payload accepted by record_payload |
| `functional/windows_connreset.cpp:452` | D | send_packet is void; a silent drop makes received < sent fail the test |
| `functional/windows_connreset.cpp:641` | D | charged only when accepted |
| `functional/windows_socket_errors.cpp:201` | D | tick sequence stamp, not paying for an operation |
| `functional/windows_socket_errors.cpp:206` | D | charged after receive_packet returned > 0 |
| `functional/windows_socket_errors.cpp:222` | D | charged per payload the server returned |
| `functional/windows_socket_errors.cpp:233` | D | tick schedule advance, not a counter |
| `functional/windows_socket_errors.cpp:444` | D | send is void; an unsent datagram makes the counter compare fail, not pass |
| `functional/windows_socket_errors.cpp:446` | D | send is void; an unsent datagram makes the counter compare fail, not pass |
| `functional/windows_socket_errors.cpp:448` | D | send is void; an unsent datagram makes the counter compare fail, not pass |
| `functional/windows_socket_errors.cpp:450` | D | send is void; an unsent datagram makes the counter compare fail, not pass |
| `sdk/rocketnet.cpp:110` | H | local string cursor; no stored state, does not outlive the call |
| `sdk/rocketnet.cpp:401` | H | counts the refusal itself (failure counter on its own refusal path) |
| `sdk/rocketnet.cpp:422` | A | counts the refused misuse itself; called only on refusal paths |
| `sdk/rocketnet.cpp:492` | H | refusal counter charged on the refusal path; names the failure |
| `sdk/rocketnet.cpp:501` | H | live-allocation count; the NULL-return path at 508 decrements it under the lock |
| `sdk/rocketnet.cpp:506` | H | refusal counter charged on the refusal path; names the failure |
| `sdk/rocketnet.cpp:508` | H | refund of the 501 charge on allocator failure |
| `sdk/rocketnet.cpp:528` | H | release decrement in rocketnet_free; matching increment in rocketnet_malloc |
| `sdk/rocketnet.h:830` | H | pragma text matched '--'; not code |
| `sdk/rocketnet_ack.cpp:99` | H | local walk counters/cursor; block written only after each run is complete |
| `sdk/rocketnet_ack.cpp:102` | H | local walk counters/cursor; block written only after each run is complete |
| `sdk/rocketnet_ack.cpp:114` | H | local walk counters/cursor; block written only after each run is complete |
| `sdk/rocketnet_ack.cpp:117` | H | local walk counters/cursor; block written only after each run is complete |
| `sdk/rocketnet_ack.cpp:126` | H | local walk counters/cursor; block written only after each run is complete |
| `sdk/rocketnet_ack.cpp:132` | H | local walk counters/cursor; block written only after each run is complete |
| `sdk/rocketnet_ack.cpp:140` | A | local block struct, does not outlive the call; written once at the end |
| `sdk/rocketnet_ack.cpp:223` | H | local sum checked against largest before block->largest is set |
| `sdk/rocketnet_ack.cpp:224` | H | local sum checked against largest before block->largest is set |
| `sdk/rocketnet_ack.cpp:402` | H | local search cursor; only returned as a found sequence |
| `sdk/rocketnet_ack.cpp:405` | H | local search cursor; only returned as a found sequence |
| `sdk/rocketnet_ack.cpp:410` | H | local search cursor; only returned as a found sequence |
| `sdk/rocketnet_ack.cpp:418` | A | void helper, no failure path; mirrors a record already marked acked |
| `sdk/rocketnet_ack.cpp:419` | A | void helper, no failure path; mirrors a record already marked acked |
| `sdk/rocketnet_ack.cpp:437` | A | window recount of a record already written; matching decrement in sent_advance |
| `sdk/rocketnet_ack.cpp:438` | A | window recount of a record already written; matching decrement in sent_advance |
| `sdk/rocketnet_ack.cpp:453` | A | window recount of a present record; decremented when it leaves the window |
| `sdk/rocketnet_ack.cpp:455` | A | window recount of a present record; decremented on ack or window exit |
| `sdk/rocketnet_ack.cpp:506` | A | release: record leaving the window, matching add in stats_sent_add |
| `sdk/rocketnet_ack.cpp:507` | A | release: record leaving the window, matching add in stats_sent_add |
| `sdk/rocketnet_ack.cpp:510` | A | release: acked record leaving the window, matching add in add_acked |
| `sdk/rocketnet_ack.cpp:511` | A | release: acked record leaving the window, matching add in add_acked |
| `sdk/rocketnet_ack.cpp:515` | A | release: record leaving eligible part, matching add on crossing |
| `sdk/rocketnet_ack.cpp:517` | A | release: unacked eligible record leaving, matching add on crossing |
| `sdk/rocketnet_ack.cpp:534` | H | local search cursor before stats_sent_low is assigned the found sequence |
| `sdk/rocketnet_ack.cpp:562` | A | recount of present survivors crossing into eligible; no failure path follows |
| `sdk/rocketnet_ack.cpp:564` | A | recount of present unacked survivors; decremented on ack or exit |
| `sdk/rocketnet_ack.cpp:600` | A | void link of a record already written; matching subtract in received_unlink |
| `sdk/rocketnet_ack.cpp:601` | A | void link, every branch completes the link; matching decrement in unlink |
| `sdk/rocketnet_ack.cpp:643` | A | release: decrement on unlink, matching increment in received_link |
| `sdk/rocketnet_ack.cpp:648` | A | release: subtract on unlink, matching add in received_link |
| `sdk/rocketnet_ack.cpp:720` | A | release of the overwritten sample's share; no failure path |
| `sdk/rocketnet_ack.cpp:721` | A | release of the overwritten sample's share; no failure path |
| `sdk/rocketnet_ack.cpp:726` | A | charged after the sample is stored; no failure path follows |
| `sdk/rocketnet_ack.cpp:727` | A | charged after the sample is stored; no failure path follows |
| `sdk/rocketnet_ack.cpp:874` | F | counts the failure: EVICTED_LOST result only |
| `sdk/rocketnet_ack.cpp:878` | F | counts the failure: evicted-unknown or refused-stale result only |
| `sdk/rocketnet_ack.cpp:906` | F | id generator under lock; an unfinished aggregate never surfaces an ack and its ring slot is overwritten |
| `sdk/rocketnet_ack.cpp:1013` | H | local popcount accumulator |
| `sdk/rocketnet_ack.cpp:1114` | B | test-hook visit counter; compiled only under ROCKETNET_ACK_VISIT_TEST_HOOKS |
| `sdk/rocketnet_ack.cpp:1137` | H | batch_full counter counts the deferral itself; record left unacked |
| `sdk/rocketnet_ack.cpp:1147` | A | release on ack after record->acked set; matching add in stats_sent_add |
| `sdk/rocketnet_ack.cpp:1149` | H | local count of records actually marked acked, after the mark |
| `sdk/rocketnet_ack.cpp:1181` | H | append after the capacity check at 1135; at most one append per visit |
| `sdk/rocketnet_ack.cpp:1194` | A | decrement on fragment ack; zero pushes id into room checked by the deferral guard |
| `sdk/rocketnet_ack.cpp:1196` | H | append after the capacity check at 1135; at most one append per visit |
| `sdk/rocketnet_ack.cpp:1272` | H | local sum of acked records returned by apply_run |
| `sdk/rocketnet_ack.cpp:1281` | H | local sum of acked records returned by apply_run |
| `sdk/rocketnet_ack.cpp:1341` | H | local stats accumulator recomputed each call |
| `sdk/rocketnet_ack.cpp:1342` | H | local stats accumulator recomputed each call |
| `sdk/rocketnet_ack.cpp:1382` | H | local stats accumulator recomputed each call |
| `sdk/rocketnet_ack.cpp:1383` | H | local stats accumulator recomputed each call |
| `sdk/rocketnet_ack.cpp:1400` | H | local stats accumulator recomputed each call |
| `sdk/rocketnet_ack.cpp:1418` | H | local stats accumulator recomputed each call |
| `sdk/rocketnet_ack.cpp:1454` | H | local stats accumulator recomputed each call |
| `sdk/rocketnet_ack.h:178` | H | epoch bump invalidates tags; wraps via initialize, no failure path |
| `sdk/rocketnet_address.cpp:52` | H | local length scan bounded by buffer size; refused before use |
| `sdk/rocketnet_address.cpp:62` | H | local string cursor; no stored state, does not outlive the call |
| `sdk/rocketnet_allocator.cpp:38` | C | compile-time size check matched on the word reserved; no reservation |
| `sdk/rocketnet_allocator.cpp:51` | F | helper definition; callers count refusals on their own refusal path |
| `sdk/rocketnet_allocator.cpp:129` | H | local size sum; failure returns false before anything is stored |
| `sdk/rocketnet_allocator.cpp:165` | A | local layout sum over parts; no failure return after the loop |
| `sdk/rocketnet_allocator.cpp:258` | H | local layout offset; no failure path in init |
| `sdk/rocketnet_allocator.cpp:302` | A | fresh advanced only after rn_send_store_fits guaranteed all chunks; no later failure |
| `sdk/rocketnet_allocator.cpp:317` | A | charged after every chunk was linked; function cannot fail after fits check |
| `sdk/rocketnet_allocator.cpp:351` | A | release: chunk pushed on free list, matching subtract in rn_send_store_alloc |
| `sdk/rocketnet_allocator.cpp:541` | A | diagnostic carve count after the carve completes; no failure path |
| `sdk/rocketnet_allocator.cpp:595` | C | definition; both callers check descriptors_free first and keep the descriptor |
| `sdk/rocketnet_allocator.cpp:608` | A | callers checked descriptors_free first; take cannot fail and caller keeps the descriptor |
| `sdk/rocketnet_allocator.cpp:611` | A | callers checked descriptors_free first; matching increment in rn_region_send_free |
| `sdk/rocketnet_allocator.cpp:684` | C | take after fit guarantees descriptor and chunks; no failure return follows |
| `sdk/rocketnet_allocator.cpp:845` | C | dispatch to alloc or alloc_span; each refuses before taking anything |
| `sdk/rocketnet_allocator.cpp:869` | C | take after fit_span succeeded; span link cannot fail, no later return |
| `sdk/rocketnet_allocator.cpp:953` | A | release: descriptor pushed on free list, matching take |
| `sdk/rocketnet_allocator.cpp:1006` | A | release: one acked fragment's chunk returned to the class free list |
| `sdk/rocketnet_allocator.cpp:1011` | A | counts an ack event that happened; frees the descriptor at the total |
| `sdk/rocketnet_allocator.cpp:1022` | A | release: descriptor returned when its last fragment is acked |
| `sdk/rocketnet_allocator.cpp:1059` | A | output struct zeroed per call; read-only report |
| `sdk/rocketnet_allocator.cpp:1060` | A | output struct zeroed per call; read-only report |
| `sdk/rocketnet_allocator.cpp:1398` | A | snprintf contract counts full length by design; local text, does not outlive the call |
| `sdk/rocketnet_allocator.cpp:1423` | C | sizing text literal matched on the word reserve; no reservation |
| `sdk/rocketnet_allocator.h:533` | C | declaration only |
| `sdk/rocketnet_api.cpp:783` | A | folds a finished connection's counts into lifetime; nothing after can fail |
| `sdk/rocketnet_api.cpp:1085` | A | counts the not-connected drop on its own refusal path |
| `sdk/rocketnet_api.cpp:1211` | G | reads the misuse count into stats; no charge |
| `sdk/rocketnet_api.cpp:1224` | B | local counts array summed for a read; does not outlive the call |
| `sdk/rocketnet_api.cpp:1823` | A | folds a stopped server's counts into lifetime; nothing after can fail |
| `sdk/rocketnet_api.cpp:2061` | A | counts the not-connected drop on its own refusal path |
| `sdk/rocketnet_api.cpp:2198` | G | reads the misuse count into stats; no charge |
| `sdk/rocketnet_api.cpp:2206` | B | local counts array summed for a read; does not outlive the call |
| `sdk/rocketnet_base64.cpp:42` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_base64.cpp:110` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_base64.cpp:111` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_base64.cpp:119` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_base64.cpp:121` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_base64.cpp:136` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_base64.cpp:139` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_base64.cpp:140` | H | local codec cursor/length; does not outlive the call |
| `sdk/rocketnet_buffer_handoff.h:60` | A | init fills the free list; no failure path follows |
| `sdk/rocketnet_buffer_handoff.h:85` | A | charged after the free/max_ready refusal check; nothing can fail afterwards |
| `sdk/rocketnet_buffer_handoff.h:88` | A | free slot taken after the refusal check and handed back as the producer's next buffer |
| `sdk/rocketnet_buffer_handoff.h:96` | C | returns -1 without charging when empty; take completes otherwise |
| `sdk/rocketnet_buffer_handoff.h:102` | A | decrement after the empty check; index returned to the consumer |
| `sdk/rocketnet_buffer_handoff.h:114` | A | release side; matching decrement in publish |
| `sdk/rocketnet_channel.cpp:19` | F | helper definition; each call site judged separately |
| `sdk/rocketnet_channel.cpp:42` | A | caller checked room first; append is the final success step |
| `sdk/rocketnet_channel.cpp:243` | G | counts the failure on its own type-not-enabled refusal |
| `sdk/rocketnet_channel.cpp:370` | A | counts the violation itself |
| `sdk/rocketnet_channel.cpp:372` | G | counts the violation itself |
| `sdk/rocketnet_channel.cpp:379` | G | counts the violation once per connection |
| `sdk/rocketnet_channel.cpp:419` | G | counts the failure: unreliable delivery budget full drop |
| `sdk/rocketnet_channel.cpp:438` | A | charged after the delivery entry is written; returns DELIVERED immediately |
| `sdk/rocketnet_channel.cpp:667` | A | last step after every refusal check; later BAD_BLOCK violation ends the connection |
| `sdk/rocketnet_channel.cpp:935` | C | take is the last fallible step; a refused finish leaves it pending for cancel |
| `sdk/rocketnet_channel.cpp:1007` | A | charged after the descriptor is linked into the FIFO; no later failure |
| `sdk/rocketnet_channel.cpp:1080` | A | charged after the descriptor is linked at the head; returns true |
| `sdk/rocketnet_channel.cpp:1091` | C | reads and compares CONFIG; acquires nothing |
| `sdk/rocketnet_channel.cpp:1259` | A | test-seam visit count of work actually done, reset per pass |
| `sdk/rocketnet_channel.cpp:1281` | A | plan is local to rn_ordered_write; charged only after rn_ordered_item succeeds |
| `sdk/rocketnet_channel.cpp:1282` | A | plan count local to rn_ordered_write; only after the item is built |
| `sdk/rocketnet_channel.cpp:1371` | H | queued count decremented on dequeue, after numbering succeeded |
| `sdk/rocketnet_channel.cpp:1392` | A | local plan; charged only when rn_ordered_item succeeded |
| `sdk/rocketnet_channel.cpp:1393` | A | local plan count; does not outlive the call |
| `sdk/rocketnet_channel.cpp:1455` | H | local reservation size computed, not charged |
| `sdk/rocketnet_channel.cpp:1462` | H | local reservation size computed, not charged |
| `sdk/rocketnet_channel.cpp:1515` | C | CONFIG compare call; acquires nothing |
| `sdk/rocketnet_channel.h:427` | C | declaration; compares CONFIG, acquires nothing |
| `sdk/rocketnet_channel_reliable.cpp:15` | A | unit-library-only cost counter of work performed, no production state |
| `sdk/rocketnet_channel_reliable.cpp:104` | A | room checked first; append is the final success step |
| `sdk/rocketnet_channel_reliable.cpp:153` | A | room checked first; append is the final success step |
| `sdk/rocketnet_channel_sequenced.cpp:75` | F | helper definition; each call site judged separately |
| `sdk/rocketnet_channel_sequenced.cpp:87` | G | counts the failure (delivery full) on its own refusal path |
| `sdk/rocketnet_channel_sequenced.cpp:96` | A | callers check rn_sequenced_room first; append is the final step |
| `sdk/rocketnet_channel_sequenced.cpp:104` | A | charged with the appended delivery it counts; no later failure |
| `sdk/rocketnet_channel_sequenced.cpp:165` | A | later failure is a BAD_BLOCK violation that ends the connection |
| `sdk/rocketnet_channel_sequenced.cpp:227` | G | counts the superseded drop on its own stale return; reliable number released first |
| `sdk/rocketnet_channel_sequenced.cpp:231` | G | counts the in-turn block actually released; that release stands whatever the new record does |
| `sdk/rocketnet_channel_sequenced.cpp:241` | G | counts the stale drop on its own stale return after releasing number and block |
| `sdk/rocketnet_channel_sequenced.cpp:293` | G | counts the stale drop on its own stale return after releasing the number |
| `sdk/rocketnet_channel_sequenced.cpp:311` | G | counts the failure (oversize unreliable state not held) on its own drop return |
| `sdk/rocketnet_channel_sequenced.cpp:330` | G | counts the superseded drop on its own stale return after releasing the number |
| `sdk/rocketnet_channel_sequenced.cpp:336` | G | counts the held state actually released; later failure of the new record does not restore it |
| `sdk/rocketnet_channel_sequenced.cpp:367` | G | counts the failure (no free hold slot) on its own drop return |
| `sdk/rocketnet_channel_sequenced.cpp:397` | G | counts the stale in-turn block after its release completed |
| `sdk/rocketnet_channel_sequenced.cpp:415` | G | counts the stale held state after its release completed |
| `sdk/rocketnet_client_gso_linux.cpp:151` | H | local build cursor; messages rebuilt per chunk |
| `sdk/rocketnet_client_gso_linux.cpp:152` | H | local build cursor; messages rebuilt per chunk |
| `sdk/rocketnet_client_gso_linux.cpp:175` | A | counts syscalls attempted, true whatever the result |
| `sdk/rocketnet_client_gso_linux.cpp:181` | A | charged only for messages sendmmsg reported accepted (result > 0) |
| `sdk/rocketnet_client_gso_linux.cpp:184` | A | charged only for accepted segmented messages, after sendmmsg returned success |
| `sdk/rocketnet_client_gso_linux.cpp:186` | A | test-hook mirror of segment_messages_sent, same success-only position |
| `sdk/rocketnet_client_gso_linux.cpp:190` | H | local cursor advanced by messages the kernel accepted |
| `sdk/rocketnet_client_gso_linux.cpp:205` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_client_gso_linux.cpp:217` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_client_gso_linux.cpp:226` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_client_gso_linux.cpp:228` | H | skips the refused message; its packets counted as send failures |
| `sdk/rocketnet_client_gso_linux.cpp:243` | H | local chunk cursor |
| `sdk/rocketnet_client_gso_linux.cpp:324` | A | counts syscalls attempted, true whatever the result |
| `sdk/rocketnet_client_gso_linux.cpp:328` | H | local weather retry bound |
| `sdk/rocketnet_client_gso_linux.cpp:353` | A | counts the failure on its own fault path |
| `sdk/rocketnet_client_gso_linux.cpp:358` | A | charged after recvmmsg returned a coalesced message; batch did arrive |
| `sdk/rocketnet_client_gso_linux.cpp:360` | A | test-hook mirror of gro_batches, same post-receive position |
| `sdk/rocketnet_client_gso_linux.cpp:368` | A | counts the failure on its own drop path |
| `sdk/rocketnet_client_gso_linux.cpp:376` | H | local count of segments actually copied out |
| `sdk/rocketnet_client_gso_linux.cpp:377` | A | charged after the segment was copied out; no failure follows |
| `sdk/rocketnet_client_io.h:248` | C | epoch copy, no acquire to release and no failure path |
| `sdk/rocketnet_client_io.h:384` | C | every return after the acquire releases it; LATE leaves ring Filling for retry |
| `sdk/rocketnet_client_io.h:404` | C | every return after the acquire releases it |
| `sdk/rocketnet_client_io.h:408` | C | held buffer released on the next exchange; one held at a time |
| `sdk/rocketnet_client_io.h:463` | H | local batch count; flushed and reset |
| `sdk/rocketnet_client_io.h:525` | C | released on the next line; no return between |
| `sdk/rocketnet_client_io.h:528` | C | taken ring released at the top of the next pass |
| `sdk/rocketnet_client_io.h:565` | C | epoch load, no reservation |
| `sdk/rocketnet_client_io.h:583` | A | counts the failure on its own fault path |
| `sdk/rocketnet_client_io.h:591` | A | counts the failure on its own fault path |
| `sdk/rocketnet_client_io.h:599` | A | counts the failure on its own fault path |
| `sdk/rocketnet_client_io.h:601` | H | prefilter_too_large counts the refusal on its own refusal path |
| `sdk/rocketnet_client_io.h:615` | A | charged after the record is written and decrypted; nothing after can fail |
| `sdk/rocketnet_client_io.h:616` | H | local accepted count after the datagram is stored |
| `sdk/rocketnet_client_io.h:620` | A | advances by records actually accepted, after they are stored |
| `sdk/rocketnet_client_io.h:634` | A | counts the failure on its own fault path |
| `sdk/rocketnet_client_io.h:636` | H | prefilter_too_large counts the refusal on its own refusal path |
| `sdk/rocketnet_client_io.h:646` | A | charged after the record is written; no failure return follows |
| `sdk/rocketnet_client_io.h:647` | A | charged after the record is written; no failure return follows |
| `sdk/rocketnet_client_io.h:663` | C | released on the second line after; no return between |
| `sdk/rocketnet_client_protocol.cpp:79` | A | counts a packet that did not open; the failure is what it counts |
| `sdk/rocketnet_client_protocol.cpp:85` | A | counts the limit being reached, on that path, followed by a void state change |
| `sdk/rocketnet_client_protocol.cpp:228` | A | crypto tally counts keypair calls; crypto_box_keypair has no failure return here |
| `sdk/rocketnet_client_protocol.cpp:315` | A | advanced after rocketnet_write_sealed_packet returned nonzero bytes |
| `sdk/rocketnet_client_protocol.cpp:418` | B | received counts arrivals, not successes |
| `sdk/rocketnet_client_protocol.cpp:424` | B | counts the wrong-state refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:431` | B | counts the wrong-size refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:442` | B | counts the salt-mismatch refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:459` | B | charged after the cookie is stored; only a void state change follows |
| `sdk/rocketnet_client_protocol.cpp:483` | B | received counts arrivals, not successes |
| `sdk/rocketnet_client_protocol.cpp:489` | B | counts the wrong-size refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:500` | B | counts the wrong-state refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:509` | B | counts the zero-sequence refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:538` | B | counts the did-not-open refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:556` | B | counts the small-order-key refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:591` | A | advanced after the response write with fixed arguments; nothing after it fails |
| `sdk/rocketnet_client_protocol.cpp:593` | B | charged after the commit and response build; returns 1 |
| `sdk/rocketnet_client_protocol.cpp:610` | B | received counts arrivals, not successes |
| `sdk/rocketnet_client_protocol.cpp:616` | B | counts the wrong-size refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:624` | B | counts the wrong-state refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:633` | B | counts the zero-sequence refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:674` | B | counts the did-not-open refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:679` | B | charged after the denial opened; only a void state change follows |
| `sdk/rocketnet_client_protocol.cpp:765` | B | received counts arrivals, not successes |
| `sdk/rocketnet_client_protocol.cpp:773` | B | counts the wrong-size refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:783` | B | counts the wrong-state refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:792` | B | counts the zero-sequence refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:799` | B | counts the already-received refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:810` | B | counts the did-not-open refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:858` | B | counts the bad-ack-block refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:869` | B | counts the wrong-opened-size refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:914` | A | ack debt added after every refusal rung passed; last step before success return |
| `sdk/rocketnet_client_protocol.cpp:945` | B | received counts arrivals, not successes |
| `sdk/rocketnet_client_protocol.cpp:950` | B | counts the wrong-size refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:957` | B | counts the wrong-state refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:972` | B | counts the zero-sequence refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:978` | B | counts the already-received refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:987` | B | counts the I/O zero-sequence verdict on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:991` | B | counts the I/O already-received verdict on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:995` | B | counts the I/O did-not-open verdict on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1010` | B | counts the sequence-disagreement refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1017` | B | counts the already-received refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1076` | A | counts the refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1106` | A | charged after session entry accepted; remaining state change is void |
| `sdk/rocketnet_client_protocol.cpp:1155` | A | charged after payload copied out; accept_payload is void, function returns 1 |
| `sdk/rocketnet_client_protocol.cpp:1186` | A | charged after session entry accepted; returns 1 immediately |
| `sdk/rocketnet_client_protocol.cpp:1215` | A | received counts the arrival on the wrong-state refusal path, paired with wrong_state |
| `sdk/rocketnet_client_protocol.cpp:1216` | A | counts the wrong-state refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1237` | A | asserted-unreachable null-reassembly branch returns 1; no later step can fail |
| `sdk/rocketnet_client_protocol.cpp:1282` | A | charged after session entry accepted; set_state is void |
| `sdk/rocketnet_client_protocol.cpp:1314` | B | counts the capacity refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1326` | B | counts arrivals past the capacity gate, not successes |
| `sdk/rocketnet_client_protocol.cpp:1332` | B | counts the prefilter refusal reason on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1342` | B | counts the reserved-type refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1353` | B | counts the wrong-direction refusal on its own refusal path |
| `sdk/rocketnet_client_protocol.cpp:1517` | A | advanced after the sealed write returned nonzero bytes |
| `sdk/rocketnet_client_protocol.cpp:1543` | F | wire payload id generator; a skipped id costs nothing and wraps past zero |
| `sdk/rocketnet_client_protocol.cpp:1719` | A | debt added after replay advance as the last step; void function |
| `sdk/rocketnet_client_slots.cpp:199` | A | pop after the empty check; no failure return follows the pop |
| `sdk/rocketnet_client_slots.cpp:211` | A | last step of assign; no failure return after it |
| `sdk/rocketnet_client_transport.cpp:294` | C | test seam; connection_reset and the final arena reset reclaim the slot |
| `sdk/rocketnet_client_transport.cpp:324` | C | test seam; arena reset on the next line |
| `sdk/rocketnet_client_transport.cpp:425` | C | declaration; the definition acquires and releases on every path |
| `sdk/rocketnet_client_transport.cpp:430` | C | production stub, no reservation |
| `sdk/rocketnet_client_transport.cpp:470` | C | take_setting applies a pending setting under a released mutex |
| `sdk/rocketnet_client_transport.cpp:1213` | H | local ladder budget tallies; create refused before any allocation |
| `sdk/rocketnet_client_transport.cpp:1216` | H | local ladder budget tallies; create refused before any allocation |
| `sdk/rocketnet_client_transport.cpp:2018` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_client_transport.cpp:2067` | A | process_record zeroes received_session_sequence each call; nonzero only when accepted |
| `sdk/rocketnet_client_transport.cpp:2076` | A | charged after the payload was opened into its slot; no failure follows |
| `sdk/rocketnet_client_transport.cpp:2092` | A | charged after the bounds check and the store; no failure follows |
| `sdk/rocketnet_client_transport.cpp:2097` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_client_transport.cpp:2138` | A | counts committed slots once (stats_counted); deferred slots stay committed and leave later |
| `sdk/rocketnet_client_transport.cpp:2139` | A | counts committed slots once (stats_counted); deferred slots stay committed and leave later |
| `sdk/rocketnet_client_transport.cpp:2140` | A | counts committed slots once (stats_counted); deferred slots stay committed and leave later |
| `sdk/rocketnet_client_transport.cpp:2141` | A | counts committed slots once (stats_counted); deferred slots stay committed and leave later |
| `sdk/rocketnet_client_transport.cpp:2170` | A | counts the deferral on its own LATE path |
| `sdk/rocketnet_client_transport.cpp:2187` | F | nothing fails after the slot; overshoot clamped by every reader and reset at hand-off |
| `sdk/rocketnet_client_transport.cpp:2191` | F | counts the failure on its own ring-full refusal path |
| `sdk/rocketnet_client_transport.cpp:2207` | C | wrapper; callers release on every path after success |
| `sdk/rocketnet_client_transport.cpp:2215` | C | wrapper; callers release on every path after success |
| `sdk/rocketnet_client_transport.cpp:2236` | C | every return after the acquire releases it |
| `sdk/rocketnet_client_transport.cpp:2247` | C | taken buffers returned by update_return_legacy_receive each update |
| `sdk/rocketnet_client_transport.cpp:2248` | H | local packet budget sum for the exposed prefix |
| `sdk/rocketnet_client_transport.cpp:2267` | A | deferred-return list, published on this or the next expose under the mutex |
| `sdk/rocketnet_client_transport.cpp:2269` | C | busy mutex keeps the return list for the next expose; released on success |
| `sdk/rocketnet_client_transport.cpp:2319` | H | local stats totals recomputed every update |
| `sdk/rocketnet_client_transport.cpp:2320` | H | local stats totals recomputed every update |
| `sdk/rocketnet_client_transport.cpp:2321` | H | local stats totals recomputed every update |
| `sdk/rocketnet_client_transport.cpp:2322` | H | local stats totals recomputed every update |
| `sdk/rocketnet_client_transport.cpp:2323` | H | local stats totals recomputed every update |
| `sdk/rocketnet_client_transport.cpp:2324` | H | local stats totals recomputed every update |
| `sdk/rocketnet_client_transport.cpp:2341` | A | per-update frame number, no operation to fail |
| `sdk/rocketnet_client_transport.cpp:2350` | A | adds losses the tracker already judged; no failure path |
| `sdk/rocketnet_client_transport.cpp:2451` | A | counts datagrams already received into the exposed buffer |
| `sdk/rocketnet_client_transport.cpp:2452` | A | counts bytes of datagrams already received |
| `sdk/rocketnet_client_transport.cpp:2453` | A | counts datagrams already received into the exposed buffer |
| `sdk/rocketnet_client_transport.cpp:2454` | A | counts bytes of datagrams already received |
| `sdk/rocketnet_client_transport.cpp:2496` | A | client link_send has no refusal; socket failures counted separately as SEND_FAILED |
| `sdk/rocketnet_client_transport.cpp:2497` | A | client link_send has no refusal; socket failures counted separately as SEND_FAILED |
| `sdk/rocketnet_client_transport.cpp:2498` | A | client link_send has no refusal; socket failures counted separately as SEND_FAILED |
| `sdk/rocketnet_client_transport.cpp:2499` | A | client link_send has no refusal; socket failures counted separately as SEND_FAILED |
| `sdk/rocketnet_client_transport.cpp:2569` | read | sequence taken before the ring, arena and lease checks; a refusal leaves it unsent, which tla/FragmentsAndAcks.tla BurnOne models; no counter or capacity is consumed |
| `sdk/rocketnet_client_transport.cpp:2582` | F | lease-full abort zeroes wire_bytes; flush skips empty slot and resets the ring |
| `sdk/rocketnet_client_transport.cpp:2586` | F | counts the failure on its own ring-full refusal path |
| `sdk/rocketnet_client_transport.cpp:2603` | C | arena reclaimed wholesale at flush; an aborted slot lives no longer than a finished one |
| `sdk/rocketnet_client_transport.cpp:2607` | F | counts the failure on its own arena-full refusal path |
| `sdk/rocketnet_client_transport.cpp:2624` | F | counts the failure on its own lease-full refusal path |
| `sdk/rocketnet_client_transport.cpp:2677` | C | take consumes the lease the abort is for; slot zeroed |
| `sdk/rocketnet_client_transport.cpp:2702` | F | seal failure leaves wire_bytes 0; flush skips empty slot and resets the ring |
| `sdk/rocketnet_client_transport.cpp:2706` | F | counts the failure on its own ring-full refusal path |
| `sdk/rocketnet_client_transport.cpp:2740` | F | reserved slots zeroed empty; flush skips them and resets num_packets |
| `sdk/rocketnet_client_transport.cpp:2746` | F | counts the failure on its own ring-full refusal path |
| `sdk/rocketnet_client_transport.cpp:2785` | H | local wire total; seal failure returns 0 and zeroes every slot |
| `sdk/rocketnet_client_transport.cpp:2839` | C | take consumes the lease; every finish failure path aborts the frame |
| `sdk/rocketnet_client_transport.cpp:2886` | A | only sealed slots (wire_bytes > 0); client link_send cannot refuse |
| `sdk/rocketnet_client_transport.cpp:2887` | A | only sealed slots (wire_bytes > 0); client link_send cannot refuse |
| `sdk/rocketnet_client_transport.cpp:2888` | A | only sealed slots (wire_bytes > 0); client link_send cannot refuse |
| `sdk/rocketnet_client_transport.cpp:2889` | A | only sealed slots (wire_bytes > 0); client link_send cannot refuse |
| `sdk/rocketnet_client_transport.cpp:2931` | A | cursor skips a refused payload by design, refusal counted |
| `sdk/rocketnet_client_transport.cpp:2936` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_client_transport.cpp:3083` | A | one burst per attempt; staged burst always sent by link or stop flush |
| `sdk/rocketnet_client_transport.cpp:3084` | A | one burst per attempt; staged burst always sent by link or stop flush |
| `sdk/rocketnet_client_transport.cpp:3085` | A | one burst per attempt; staged burst always sent by link or stop flush |
| `sdk/rocketnet_client_transport.cpp:3086` | A | one burst per attempt; staged burst always sent by link or stop flush |
| `sdk/rocketnet_client_transport.cpp:3174` | A | counts getter calls (test observation), no operation to fail |
| `sdk/rocketnet_client_transport.cpp:3232` | C | released on the second line after; no return between |
| `sdk/rocketnet_client_transport.cpp:3268` | A | counts the failure on its own fault path |
| `sdk/rocketnet_client_transport.cpp:3296` | A | charged after the datagram is stored; no failure follows |
| `sdk/rocketnet_coalesce.h:53` | H | local segment count after the max_segments check |
| `sdk/rocketnet_coalesce.h:88` | H | local run length; returned, nothing reserved |
| `sdk/rocketnet_coalesce.h:89` | H | local run length; returned, nothing reserved |
| `sdk/rocketnet_config.cpp:400` | H | local string cursor; no stored state, does not outlive the call |
| `sdk/rocketnet_config.cpp:889` | H | local string cursor; no stored state, does not outlive the call |
| `sdk/rocketnet_config.cpp:892` | H | local string cursor; no stored state, does not outlive the call |
| `sdk/rocketnet_config.cpp:902` | A | ladder memset at entry; caller discards it on a false return |
| `sdk/rocketnet_config.cpp:915` | H | local string cursor; no stored state, does not outlive the call |
| `sdk/rocketnet_config.cpp:918` | H | local string cursor; no stored state, does not outlive the call |
| `sdk/rocketnet_config.cpp:958` | A | ladder memset at entry; caller discards it on a false return |
| `sdk/rocketnet_config.cpp:972` | A | ladder memset at entry; caller discards it on a false return |
| `sdk/rocketnet_config.cpp:1007` | A | fallback fill on the success path; bound by num_available |
| `sdk/rocketnet_connect_token.cpp:147` | H | local write cursor into the caller's token |
| `sdk/rocketnet_connect_token.cpp:314` | A | tallies the verify work attempted, which happens either way |
| `sdk/rocketnet_connect_token.cpp:338` | A | tallies the open work attempted, which happens either way |
| `sdk/rocketnet_connect_token.cpp:514` | H | local address list count for the token write |
| `sdk/rocketnet_connect_token.cpp:516` | H | local address list count for the token write |
| `sdk/rocketnet_connection_reset.cpp:168` | H | local match counter in an inventory check |
| `sdk/rocketnet_connection_reset.cpp:187` | H | local match counter in an inventory check |
| `sdk/rocketnet_counters.cpp:132` | H | local outcome sum for the conservation check |
| `sdk/rocketnet_driver.cpp:108` | H | static list insertion sized exactly from the registry; no failure path |
| `sdk/rocketnet_driver.cpp:115` | H | static list insertion sized exactly from the registry; no failure path |
| `sdk/rocketnet_driver_builtin.cpp:114` | H | local carve pointer after the allocation succeeded |
| `sdk/rocketnet_driver_builtin.cpp:119` | H | local carve pointer after the allocation succeeded |
| `sdk/rocketnet_driver_builtin.cpp:124` | H | local carve pointer after the allocation succeeded |
| `sdk/rocketnet_driver_builtin.cpp:139` | C | try-lock released on the free_count refusal and on success |
| `sdk/rocketnet_driver_builtin.cpp:148` | A | charged after try-lock and free_count checks; swap then completes unconditionally |
| `sdk/rocketnet_driver_builtin.cpp:150` | A | ready push after all refusal checks; no later failure |
| `sdk/rocketnet_driver_builtin.cpp:163` | C | mutex released at line 174 on every path |
| `sdk/rocketnet_driver_builtin.cpp:170` | A | take after ready_count check; bank always returned free at line 200 |
| `sdk/rocketnet_driver_builtin.cpp:189` | A | test hook counts a bank skipped by design; no failure follows |
| `sdk/rocketnet_driver_builtin.cpp:198` | C | mutex released at line 201 straight after the free push |
| `sdk/rocketnet_driver_builtin.cpp:200` | A | release side; matching take at line 170 |
| `sdk/rocketnet_driver_builtin.cpp:222` | C | every return after the acquire releases rx_mutex |
| `sdk/rocketnet_driver_builtin.cpp:231` | A | charged after the rx_free_count refusal; swap completes unconditionally |
| `sdk/rocketnet_driver_builtin.cpp:233` | A | ready push after all refusal checks; no later failure |
| `sdk/rocketnet_driver_builtin.cpp:244` | C | try-lock released before every return; consumer retained by design |
| `sdk/rocketnet_driver_builtin.cpp:251` | C | rx_mutex released at line 265 on the only path after it |
| `sdk/rocketnet_driver_builtin.cpp:256` | A | returns the fully copied consumer bank; matching take at line 263 |
| `sdk/rocketnet_driver_builtin.cpp:263` | A | take after ready_count check; consumer returned on a later acquire |
| `sdk/rocketnet_driver_builtin.cpp:289` | A | test counter of full-wait events on its own refusal path |
| `sdk/rocketnet_driver_builtin.cpp:447` | A | dropped counts the failure on the stopping path |
| `sdk/rocketnet_driver_builtin.cpp:453` | A | dropped counts the failure when publish is refused |
| `sdk/rocketnet_driver_builtin.cpp:460` | A | dropped counts the invalid packet on its own refusal path |
| `sdk/rocketnet_driver_builtin.cpp:467` | A | charged after all checks and the copy; unpublished bank retained and retried |
| `sdk/rocketnet_driver_builtin.cpp:480` | C | held consumer is the design; returned when fully copied on a later acquire |
| `sdk/rocketnet_driver_builtin.cpp:496` | A | cursor advanced after the copies complete |
| `sdk/rocketnet_driver_builtin.cpp:497` | H | local received count of datagrams copied |
| `sdk/rocketnet_driver_builtin.cpp:511` | A | dropped counts the unpublishable final bank, which is then zeroed |
| `sdk/rocketnet_driver_builtin.cpp:546` | C | mutex released on the next line; no return between |
| `sdk/rocketnet_driver_loopback.cpp:90` | C | returns the try result; caller releases at line 383 |
| `sdk/rocketnet_driver_loopback.cpp:96` | A | send_failures counts the failure itself |
| `sdk/rocketnet_driver_loopback.cpp:186` | A | dropped counts the full-queue refusal |
| `sdk/rocketnet_driver_loopback.cpp:326` | C | every return after the acquire releases the mutex |
| `sdk/rocketnet_driver_loopback.cpp:344` | A | tail advanced after destination and slot found and copy done |
| `sdk/rocketnet_driver_loopback.cpp:345` | A | count charged after the copy; nothing fails afterwards |
| `sdk/rocketnet_driver_loopback.cpp:359` | C | loop only continues; single release at line 383 |
| `sdk/rocketnet_driver_loopback.cpp:380` | A | tail advanced after slot found and copy done |
| `sdk/rocketnet_driver_loopback.cpp:381` | A | count charged after the copy; nothing fails afterwards |
| `sdk/rocketnet_driver_loopback.cpp:407` | A | dropped counts the oversize packet on its own discard path |
| `sdk/rocketnet_driver_loopback.cpp:408` | A | head advance discards the counted bad packet |
| `sdk/rocketnet_driver_loopback.cpp:409` | A | count decrement matches the discarded packet |
| `sdk/rocketnet_driver_loopback.cpp:415` | A | head advanced after the copy succeeded |
| `sdk/rocketnet_driver_loopback.cpp:416` | A | count decremented after the copy succeeded |
| `sdk/rocketnet_driver_loopback.cpp:417` | H | local count after the packet was copied out |
| `sdk/rocketnet_driver_loopback.cpp:432` | A | dropped counts the oversize packet on its own discard path |
| `sdk/rocketnet_driver_loopback.cpp:433` | A | head advance discards the counted bad packet |
| `sdk/rocketnet_driver_loopback.cpp:434` | A | count decrement matches the discarded packet |
| `sdk/rocketnet_driver_loopback.cpp:440` | A | head advanced after the copy succeeded |
| `sdk/rocketnet_driver_loopback.cpp:441` | A | count decremented after the copy succeeded |
| `sdk/rocketnet_driver_loopback.cpp:475` | H | local count of live endpoints |
| `sdk/rocketnet_encryption_map.cpp:172` | A | test-seam hash count of work performed |
| `sdk/rocketnet_encryption_map.cpp:235` | A | charged as the last step after the slot is linked |
| `sdk/rocketnet_encryption_map.cpp:300` | A | release-side decrement; matching add at line 235; chain miss only under prior corruption |
| `sdk/rocketnet_endpoint.cpp:34` | F | helper definition; each call site judged separately |
| `sdk/rocketnet_endpoint.cpp:221` | G | counts the refusal on its own NULL return; nothing reserved yet |
| `sdk/rocketnet_endpoint.cpp:230` | G | counts the refusal on its own NULL return; nothing reserved yet |
| `sdk/rocketnet_endpoint.cpp:242` | G | counts the over-max failure on its own NULL return; nothing reserved yet |
| `sdk/rocketnet_endpoint.cpp:257` | G | counts the refusal after rn_channels_start returned NULL; nothing reserved |
| `sdk/rocketnet_endpoint.cpp:269` | C | no failure return after the take; abort or finish owns the descriptor via pending_descriptor |
| `sdk/rocketnet_endpoint.cpp:362` | G | promotion counted after rn_channels_finish succeeded; no failure return follows |
| `sdk/rocketnet_endpoint.cpp:398` | A | charged after the descriptor is linked on the queue; returns serial |
| `sdk/rocketnet_endpoint.cpp:587` | G | counts the over-budget drop on its own drop path; receipt gets LOSS |
| `sdk/rocketnet_endpoint.cpp:610` | A | packet allowance charged after the transport sent the frame |
| `sdk/rocketnet_endpoint.cpp:619` | G | counts the transport refusal on the failure handler itself |
| `sdk/rocketnet_endpoint.cpp:624` | A | allowance charged on failure by design, bounding the write pass |
| `sdk/rocketnet_endpoint.cpp:636` | G | counts the refusal on its own zero return before any start |
| `sdk/rocketnet_endpoint.cpp:676` | H | local copy count bounded by out_capacity |
| `sdk/rocketnet_endpoint.cpp:780` | A | release: decrement on unlink, matching increment in rn_endpoint_finish |
| `sdk/rocketnet_endpoint.cpp:795` | G | counts drops after every descriptor was unlinked and freed; already_counted avoids double count |
| `sdk/rocketnet_endpoint.cpp:851` | G | counts the unplannable-fragment failure on its own drop; descriptor freed |
| `sdk/rocketnet_endpoint.cpp:868` | H | local admitted sum; -1 return stores nothing, budget not charged here |
| `sdk/rocketnet_endpoint.cpp:876` | A | counts the failure on its own over-budget refusal path |
| `sdk/rocketnet_endpoint.cpp:877` | A | counts the failure on its own over-budget refusal path |
| `sdk/rocketnet_endpoint.cpp:878` | G | counts the over-budget drop on its own drop; nothing charged to the bucket |
| `sdk/rocketnet_endpoint.cpp:945` | A | allowance charged for a consumed head (counted drop); bounds the loop |
| `sdk/rocketnet_endpoint.cpp:983` | H | local item count returned to the planner |
| `sdk/rocketnet_endpoint.cpp:1046` | A | per-packet serial list reset on sent and failed; failed emits LOSS for each |
| `sdk/rocketnet_endpoint.cpp:1086` | G | counts the writer failure on its own drop; budget charge happens only after success |
| `sdk/rocketnet_endpoint.cpp:1088` | A | allowance charged on the counted writer failure; bounds the write pass |
| `sdk/rocketnet_endpoint.cpp:1180` | A | allowance charged after the transport sent the packet |
| `sdk/rocketnet_endpoint.cpp:1208` | A | allowance charged on failure by design; records requeued, bounds retry loop |
| `sdk/rocketnet_endpoint.cpp:1213` | G | counts the transport failure per record on the failure handler (budget leak noted as WIDEN) |
| `sdk/rocketnet_endpoint.cpp:1216` | A | allowance charged on failure by design; bounds the write pass |
| `sdk/rocketnet_endpoint.cpp:1220` | G | counts the transport failure on the failure handler (budget leak noted as WIDEN) |
| `sdk/rocketnet_endpoint.cpp:1222` | A | allowance charged on failure by design; bounds the write pass |
| `sdk/rocketnet_endpoint.cpp:1238` | C | read-only copy of this update's receipts; takes no capacity |
| `sdk/rocketnet_endpoint.h:255` | C | declaration only |
| `sdk/rocketnet_fragment.cpp:295` | A | called only after every refusal check passed; no failure follows |
| `sdk/rocketnet_fragment.cpp:324` | B | counts the failure on its own refusal path (size_mismatch) |
| `sdk/rocketnet_fragment.cpp:346` | B | counts the failure on its own refusal path (zero_payload_id) |
| `sdk/rocketnet_fragment.cpp:390` | B | counts the failure on its own refusal path (stale) |
| `sdk/rocketnet_fragment.cpp:398` | B | counts the failure on its own refusal path (stale) |
| `sdk/rocketnet_fragment.cpp:408` | B | counts the failure on its own refusal path (count_invalid) |
| `sdk/rocketnet_fragment.cpp:415` | B | counts the failure on its own refusal path (index_out_of_range) |
| `sdk/rocketnet_fragment.cpp:430` | B | counts the failure on its own refusal path (count_mismatch) |
| `sdk/rocketnet_fragment.cpp:447` | B | counts the failure on its own refusal path (size_mismatch) |
| `sdk/rocketnet_fragment.cpp:453` | B | counts the failure on its own refusal path (size_mismatch) |
| `sdk/rocketnet_fragment.cpp:464` | B | counts the failure on its own refusal path; slot cleared |
| `sdk/rocketnet_fragment.cpp:490` | B | counts the failure on its own refusal path; slot cleared |
| `sdk/rocketnet_fragment.cpp:504` | B | counts the failure on its own refusal path (duplicate_index) |
| `sdk/rocketnet_fragment.cpp:518` | B | counts the eviction it performs (abandoned); eviction cannot fail |
| `sdk/rocketnet_fragment.cpp:564` | B | charged after every check passed and the bytes were placed |
| `sdk/rocketnet_fragment.cpp:590` | A | charged after the capacity check and the copy succeeded |
| `sdk/rocketnet_fragment.cpp:599` | B | counts the failure on its own drop path (batch_full) |
| `sdk/rocketnet_fragment.cpp:602` | B | names completed reassembly, which happened; a delivery drop is counted separately as batch_full |
| `sdk/rocketnet_fragment.cpp:659` | C | bump arena: failed acquire overshoots only past full; per-update reset restores |
| `sdk/rocketnet_fragment.cpp:663` | F | overshoot returns NULL; whole arena reclaimed by rocketnet_send_arena_reset at flush |
| `sdk/rocketnet_fragment.h:255` | C | declaration only |
| `sdk/rocketnet_frame_sync.cpp:501` | A | counts the dropped frames themselves |
| `sdk/rocketnet_frame_sync.cpp:505` | A | frame index advanced after the wait completed; no failure path |
| `sdk/rocketnet_gateway_netlink_linux.cpp:58` | H | local parse offset; malformed input returns before use |
| `sdk/rocketnet_gateway_netlink_linux.cpp:157` | H | local parse offset; malformed input returns before use |
| `sdk/rocketnet_jitter_tracker.h:69` | A | an EWMA estimate, not a charge; no failure path |
| `sdk/rocketnet_keys.cpp:76` | A | tallies the scalarmult work attempted, which happens either way |
| `sdk/rocketnet_memory.h:29` | H | overflow checked before the add |
| `sdk/rocketnet_message.cpp:59` | H | local header size computation |
| `sdk/rocketnet_message.cpp:61` | H | local header size computation |
| `sdk/rocketnet_message.cpp:63` | H | local header size computation |
| `sdk/rocketnet_message.cpp:65` | H | local header size computation |
| `sdk/rocketnet_message.cpp:67` | H | local header size computation |
| `sdk/rocketnet_message.cpp:193` | F | adds zero: an atomic read, no charge |
| `sdk/rocketnet_message.cpp:238` | F | counted after every refusal check; the copy that follows cannot fail |
| `sdk/rocketnet_message.cpp:243` | A | charged after the record is fully written; returns 1 |
| `sdk/rocketnet_message.cpp:244` | A | charged after the record is fully written; returns 1 |
| `sdk/rocketnet_message.cpp:466` | H | local decode offset; violation returns, nothing stored |
| `sdk/rocketnet_message.cpp:518` | A | advanced only after the record decoded cleanly |
| `sdk/rocketnet_message_client.cpp:372` | C | const read of this update's receipts; acquires nothing |
| `sdk/rocketnet_message_server.cpp:95` | H | local layout offset; nothing allocated in the layout step |
| `sdk/rocketnet_message_server.cpp:97` | H | local layout offset; nothing allocated in the layout step |
| `sdk/rocketnet_message_server.cpp:100` | H | local layout offset; nothing allocated in the layout step |
| `sdk/rocketnet_message_server.cpp:103` | H | local layout offset; nothing allocated in the layout step |
| `sdk/rocketnet_message_server.cpp:106` | H | local layout offset; nothing allocated in the layout step |
| `sdk/rocketnet_message_server.cpp:132` | F | helper definition; each call site judged separately |
| `sdk/rocketnet_message_server.cpp:376` | G | counts the failure/over-budget drop per record after finish failed or discard |
| `sdk/rocketnet_message_server.cpp:380` | A | per-update packet allowance, charged either way by design and reset at BeginUpdate |
| `sdk/rocketnet_message_server.cpp:439` | G | counts the refused misuse, then cancels the pending start |
| `sdk/rocketnet_message_server.cpp:456` | G | counts the refused start, then cancels it and releases its room |
| `sdk/rocketnet_message_server.cpp:522` | C | pure binding check (rn_slots_admit is const); acquires nothing |
| `sdk/rocketnet_message_server.cpp:527` | G | counts the stale entry on its own NULL return |
| `sdk/rocketnet_message_server.cpp:533` | C | rn_slots_admit is a const comparison; refusal counts the stale entry |
| `sdk/rocketnet_message_server.cpp:535` | G | counts the stale entry on its own NULL return |
| `sdk/rocketnet_message_server.cpp:555` | H | local tally of held/waiting views for counting misuse |
| `sdk/rocketnet_message_server.cpp:558` | G | counts the misuse (views not released) itself; recomputed each update |
| `sdk/rocketnet_message_server.cpp:579` | H | local tally of held/waiting views for counting misuse |
| `sdk/rocketnet_message_server.cpp:582` | G | counts the misuse (deliveries not received) itself; connection then kicked |
| `sdk/rocketnet_message_server.cpp:693` | C | pure binding check; acquires nothing |
| `sdk/rocketnet_message_server.cpp:715` | C | pure binding check; acquires nothing |
| `sdk/rocketnet_message_server.cpp:799` | G | counts the not-connected refusal on its own NULL return |
| `sdk/rocketnet_message_server.cpp:807` | G | counts the second-start refusal on its own NULL return |
| `sdk/rocketnet_message_server.cpp:904` | G | counts the fallback decision, which happened whether or not the queued start then succeeds |
| `sdk/rocketnet_message_server.cpp:1011` | A | serial assigned after the record is in the build; a failed flush emits its LOSS |
| `sdk/rocketnet_message_server.cpp:1012` | A | zero-skip of the same serial; a message id, not capacity |
| `sdk/rocketnet_message_server.cpp:1014` | A | receipt recorded with the build; flush failure emits LOSS for each serial |
| `sdk/rocketnet_message_server.cpp:1197` | A | held charged with the view marked HELD; release_view and release_batch decrement |
| `sdk/rocketnet_message_server.cpp:1255` | A | held charged with the view marked HELD; released by release_view |
| `sdk/rocketnet_message_server.cpp:1276` | A | release-side decrement; matching add at 1197/1255 |
| `sdk/rocketnet_message_server.cpp:1317` | A | release-side decrement guarded by the HELD state |
| `sdk/rocketnet_message_server.cpp:1339` | A | charged after a delivery was found; batch bookkeeping follows unconditionally |
| `sdk/rocketnet_message_server.cpp:1695` | H | local count of sends that returned nonzero (succeeded) |
| `sdk/rocketnet_message_server.cpp:1737` | C | const read of this update's receipts; acquires nothing |
| `sdk/rocketnet_network_simulator.cpp:32` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:41` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:49` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:52` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:63` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:73` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:81` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:82` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:90` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:93` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:429` | C | straight-line body, released at line 437 |
| `sdk/rocketnet_network_simulator.cpp:445` | C | lock taken only on the pending path and released before return |
| `sdk/rocketnet_network_simulator.cpp:453` | C | straight-line body, released at line 465 |
| `sdk/rocketnet_network_simulator.cpp:551` | F | decrement on slot release; matching add in rocketnet_simulator_insert |
| `sdk/rocketnet_network_simulator.cpp:611` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:620` | H | comment text matched '--'; not code |
| `sdk/rocketnet_network_simulator.cpp:793` | H | string literal '--' in an audit message; not code |
| `sdk/rocketnet_network_simulator.cpp:805` | H | string literal '--' in an audit message; not code |
| `sdk/rocketnet_network_simulator.cpp:813` | H | string literal '--' in an audit message; not code |
| `sdk/rocketnet_network_simulator.cpp:829` | H | string literal '--' in an audit message; not code |
| `sdk/rocketnet_network_simulator.cpp:835` | H | string literal '--' in an audit message; not code |
| `sdk/rocketnet_network_simulator.cpp:886` | F | charged after the slot is filled and linked; full queue returns before it |
| `sdk/rocketnet_network_simulator.cpp:958` | A | send-attempt clock for drop_every_nth; counting every attempt is its definition, never published |
| `sdk/rocketnet_network_simulator.cpp:996` | H | local count after insert returned success |
| `sdk/rocketnet_network_simulator.cpp:1021` | A | snprintf format string in the audit message, not a counter |
| `sdk/rocketnet_network_simulator.cpp:1037` | F | decrement on slot release; matching add in rocketnet_simulator_insert |
| `sdk/rocketnet_out_of_order_tracker.h:55` | A | counts the out-of-order arrival itself |
| `sdk/rocketnet_packet_builder.cpp:21` | F | helper definition; each call site judged separately |
| `sdk/rocketnet_packet_builder.cpp:140` | A | test-seam visit count, reset at each write call |
| `sdk/rocketnet_packet_builder.cpp:297` | C | unlinks from call-local class lists; refused packet resets taken and breaks |
| `sdk/rocketnet_packet_builder.cpp:316` | C | clock step; refused packet restores saved clocks and round robin |
| `sdk/rocketnet_packet_builder.cpp:407` | C | take_head on call-local lists; refusal undoes taken flags |
| `sdk/rocketnet_packet_builder.cpp:409` | H | local per-packet record/number count; known budget charge is at 439 |
| `sdk/rocketnet_packet_builder.cpp:416` | H | local per-packet record/number count; known budget charge is at 439 |
| `sdk/rocketnet_packet_builder.cpp:419` | C | take_level; refusal restores saved_level_clock and clock_base |
| `sdk/rocketnet_packet_builder.cpp:460` | A | counts the over-budget drop on its own refusal path |
| `sdk/rocketnet_packet_builder.cpp:461` | G | counts the over-budget drop per record on the refused packet; budget not charged |
| `sdk/rocketnet_packet_builder.cpp:478` | A | per-call output count, reset at entry; charged after the slot is filled |
| `sdk/rocketnet_packet_lease.h:96` | C | single-use consume of a lease; reset() frees the epoch |
| `sdk/rocketnet_packet_loss_tracker.h:67` | H | parameter shifted by one so sequence 0 is not the empty sentinel |
| `sdk/rocketnet_packet_loss_tracker.h:102` | H | local lost count |
| `sdk/rocketnet_pending_frames.h:20` | H | count added after the capacity check; all-or-zero |
| `sdk/rocketnet_pending_frames.h:29` | C | reserve is all-or-zero by contract; count kept on refusal |
| `sdk/rocketnet_platform.cpp:51` | A | timeval carry normalisation on a local value, not a counter |
| `sdk/rocketnet_platform.cpp:52` | A | timeval carry normalisation on a local value, not a counter |
| `sdk/rocketnet_platform.cpp:71` | A | local, does not outlive the call; time carry normalisation |
| `sdk/rocketnet_platform.cpp:72` | A | local, does not outlive the call; time carry normalisation |
| `sdk/rocketnet_platform.cpp:233` | A | counts the refusal on its own refusal path |
| `sdk/rocketnet_platform.cpp:343` | F | counts the failure: a send that returned -1 |
| `sdk/rocketnet_platform.cpp:415` | C | spin lock; every caller releases on every return |
| `sdk/rocketnet_platform.cpp:429` | H | local armed recount |
| `sdk/rocketnet_platform.cpp:435` | C | released on the single return; full table just skips registration |
| `sdk/rocketnet_platform.cpp:438` | H | test registry slot taken after the capacity check; no failure after |
| `sdk/rocketnet_platform.cpp:448` | C | released on the single return |
| `sdk/rocketnet_platform.cpp:455` | H | release on unregister; matching increment at register |
| `sdk/rocketnet_platform.cpp:464` | C | released on the single return |
| `sdk/rocketnet_platform.cpp:467` | H | local copy count bounded by max_sockets |
| `sdk/rocketnet_platform.cpp:474` | C | released on the single return |
| `sdk/rocketnet_platform.cpp:504` | C | break exits the loop; released before the return |
| `sdk/rocketnet_platform.cpp:510` | A | test seam: consumes one injected failure exactly when it injects it |
| `sdk/rocketnet_platform.cpp:1280` | H | local poll descriptor count |
| `sdk/rocketnet_platform.cpp:1286` | H | local pollfd count, does not outlive the call |
| `sdk/rocketnet_platform.cpp:1524` | H | local batch fill index, counted only when the sockaddr filled |
| `sdk/rocketnet_platform.cpp:1526` | H | local scan cursor over the caller's sends |
| `sdk/rocketnet_platform.cpp:1536` | H | local; advanced by the count sendmmsg returned as sent |
| `sdk/rocketnet_platform.cpp:1569` | H | local; skips the refused entry after counting the send failure |
| `sdk/rocketnet_platform.cpp:1572` | H | local chunk base cursor |
| `sdk/rocketnet_platform.cpp:1655` | H | local; counted after the datagram is parsed and copied |
| `sdk/rocketnet_platform.cpp:1658` | H | local return total of kept datagrams |
| `sdk/rocketnet_platform.cpp:1793` | A | local deadline arithmetic, does not outlive the call |
| `sdk/rocketnet_platform.cpp:1794` | A | local deadline arithmetic, does not outlive the call |
| `sdk/rocketnet_platform.cpp:1797` | A | local deadline carry, does not outlive the call |
| `sdk/rocketnet_platform.cpp:1798` | A | local deadline carry, does not outlive the call |
| `sdk/rocketnet_platform.cpp:1840` | C | primitive definition; release is the caller's |
| `sdk/rocketnet_platform.cpp:1847` | C | primitive definition; returns whether it holds the lock |
| `sdk/rocketnet_platform.cpp:1888` | C | reserve failure leaves length and reserved 0; nothing charged |
| `sdk/rocketnet_platform.cpp:1922` | C | reserved updated only after the allocation succeeded |
| `sdk/rocketnet_platform.cpp:1937` | H | local capacity; reserved stored only after malloc succeeds |
| `sdk/rocketnet_platform.cpp:1955` | C | length set only after reserve succeeded |
| `sdk/rocketnet_platform.cpp:1969` | H | decrement after the element is removed; no failure path |
| `sdk/rocketnet_platform.cpp:1977` | H | decrement after the memmove; no failure path |
| `sdk/rocketnet_platform.cpp:1999` | C | length incremented only after reserve succeeded |
| `sdk/rocketnet_platform.cpp:2001` | H | length advanced only after reserve succeeded |
| `sdk/rocketnet_platform.cpp:3078` | C | test seam exchange; the injected errno is consumed by the failure it causes |
| `sdk/rocketnet_platform.cpp:3088` | C | test seam; injected errno returned as the failure |
| `sdk/rocketnet_platform.cpp:3436` | H | local batch fill index, counted only when the sockaddr filled |
| `sdk/rocketnet_platform.cpp:3438` | H | local scan cursor |
| `sdk/rocketnet_platform.cpp:3462` | H | local; advanced by the count sendmsg_x returned |
| `sdk/rocketnet_platform.cpp:3513` | H | local chunk base cursor |
| `sdk/rocketnet_platform.cpp:3572` | C | test seam; injected errno becomes the call's result |
| `sdk/rocketnet_platform.cpp:3630` | H | local; counted after the datagram is parsed and copied |
| `sdk/rocketnet_platform.cpp:3647` | H | local; counted after a received datagram |
| `sdk/rocketnet_platform.cpp:3795` | C | primitive definition; release is the caller's |
| `sdk/rocketnet_platform.cpp:3802` | C | primitive definition; returns whether it holds the lock |
| `sdk/rocketnet_platform.cpp:3866` | H | local kevent change count; failure frees kq and wake |
| `sdk/rocketnet_platform.cpp:3869` | H | local kevent change count; failure frees kq and wake |
| `sdk/rocketnet_platform.cpp:4258` | C | primitive definition; release is the caller's |
| `sdk/rocketnet_platform.cpp:4265` | C | primitive definition; returns whether it holds the lock |
| `sdk/rocketnet_platform.cpp:5313` | H | local; counted after a received datagram |
| `sdk/rocketnet_platform.h:895` | C | declaration only |
| `sdk/rocketnet_platform.h:898` | C | declaration only |
| `sdk/rocketnet_platform.h:955` | C | RAII; destructor releases on every exit |
| `sdk/rocketnet_platform.h:1154` | F | primitive definition (Windows), no charge of its own |
| `sdk/rocketnet_platform.h:1242` | F | primitive definition (posix), no charge of its own |
| `sdk/rocketnet_platform.h:1244` | A | primitive wrapper; no operation follows that can fail |
| `sdk/rocketnet_platform.h:1395` | H | not code: a comment on an #endif ("C++") |
| `sdk/rocketnet_protocol.cpp:215` | A | tallies the seal work attempted, which happens either way |
| `sdk/rocketnet_protocol.cpp:271` | A | tallies the seal work attempted, which happens either way |
| `sdk/rocketnet_protocol.cpp:278` | H | local write cursor advanced after the seal succeeded |
| `sdk/rocketnet_protocol.cpp:314` | H | local header size, does not outlive the call |
| `sdk/rocketnet_protocol.cpp:333` | A | tallies the open work attempted, which happens either way |
| `sdk/rocketnet_read_write.h:23` | H | caller's write cursor; write cannot fail |
| `sdk/rocketnet_read_write.h:70` | H | caller's read cursor; read cannot fail |
| `sdk/rocketnet_send_budget.h:251` | A | counts the refusal itself |
| `sdk/rocketnet_send_budget.h:280` | A | spends only after can_send passed; the missing refund is in callers (finding 1) |
| `sdk/rocketnet_send_budget.h:281` | A | charged with the spend; settle is the refund; callers skip it on failure (finding 1) |
| `sdk/rocketnet_send_budget.h:295` | C | spends only on SEND; every other return spends nothing |
| `sdk/rocketnet_send_budget.h:301` | A | counts the bad-argument refusal itself; nothing spent |
| `sdk/rocketnet_send_budget.h:307` | A | counts the order refusal itself; nothing spent |
| `sdk/rocketnet_send_budget.h:324` | A | counts the unreliable drop itself; spend refused so nothing charged |
| `sdk/rocketnet_send_budget.h:325` | A | counts the dropped bytes themselves; nothing charged |
| `sdk/rocketnet_send_budget.h:341` | A | counts the settle refusal itself; tokens untouched |
| `sdk/rocketnet_send_budget.h:349` | A | the refund itself, guarded by admitted <= bytes_spent |
| `sdk/rocketnet_send_handoff.h:26` | C | filling generation retained by design until publish; used by tests only |
| `sdk/rocketnet_send_handoff.h:39` | H | full-queue refusal returns before the increment |
| `sdk/rocketnet_send_handoff.h:54` | H | decrement after the count<2 refusal; hands the generation back as filling |
| `sdk/rocketnet_send_handoff.h:65` | H | release: frees the front and decrements together; no failure path |
| `sdk/rocketnet_server_gso_linux.cpp:273` | C | released on the next line; no return between |
| `sdk/rocketnet_server_gso_linux.cpp:284` | C | try-lock released before the only later return |
| `sdk/rocketnet_server_gso_linux.cpp:299` | C | try-lock released on the free_count refusal and on success |
| `sdk/rocketnet_server_gso_linux.cpp:308` | A | charged after try-lock and free_count checks; swap completes unconditionally |
| `sdk/rocketnet_server_gso_linux.cpp:310` | A | ready push after all refusal checks; no later failure |
| `sdk/rocketnet_server_gso_linux.cpp:322` | C | mutex released before the single return |
| `sdk/rocketnet_server_gso_linux.cpp:326` | C | mutex released at line 339 on every path |
| `sdk/rocketnet_server_gso_linux.cpp:331` | A | take after ready check; worker always returns the bank via return_send |
| `sdk/rocketnet_server_gso_linux.cpp:346` | C | straight-line body, released at line 349 |
| `sdk/rocketnet_server_gso_linux.cpp:348` | A | release side; matching take in take_send |
| `sdk/rocketnet_server_gso_linux.cpp:361` | C | taken bank returned via return_send, or the final producer ends the worker |
| `sdk/rocketnet_server_gso_linux.cpp:706` | A | documented to count every recvmmsg call, failed or empty included |
| `sdk/rocketnet_server_gso_linux.cpp:722` | A | receive_errors counts the failure itself |
| `sdk/rocketnet_server_gso_linux.cpp:742` | A | dropped_size counts the truncated message on its drop path |
| `sdk/rocketnet_server_gso_linux.cpp:743` | A | dropped_too_large counts the truncated message on its drop path |
| `sdk/rocketnet_server_gso_linux.cpp:752` | A | dropped_size counts the zero-length datagram on its drop path |
| `sdk/rocketnet_server_gso_linux.cpp:761` | A | dropped_source counts the bad source on its drop path |
| `sdk/rocketnet_server_gso_linux.cpp:771` | A | cmsg fault counter counts the fault itself |
| `sdk/rocketnet_server_gso_linux.cpp:776` | A | defined as buffers that arrived coalesced, true regardless of per-segment drops |
| `sdk/rocketnet_server_gso_linux.cpp:789` | A | dropped_size counts the bad segment on its drop path |
| `sdk/rocketnet_server_gso_linux.cpp:791` | A | dropped_too_large counts the oversize segment on its drop path |
| `sdk/rocketnet_server_gso_linux.cpp:797` | A | dropped_overflow counts the full-buffer refusal |
| `sdk/rocketnet_server_gso_linux.cpp:805` | A | charged after the bound check and the copy |
| `sdk/rocketnet_server_gso_linux.cpp:819` | C | straight-line swap, released at line 826 |
| `sdk/rocketnet_server_gso_linux.cpp:954` | H | local; counted only when setsockopt succeeded |
| `sdk/rocketnet_server_gso_linux.cpp:969` | H | local; counted only when setsockopt succeeded, partial enable rolled back |
| `sdk/rocketnet_server_gso_linux.cpp:1273` | H | local cursor skipping an aborted slot |
| `sdk/rocketnet_server_gso_linux.cpp:1281` | H | local cursor; unroutable run is counted send_dropped |
| `sdk/rocketnet_server_gso_linux.cpp:1306` | H | local run length |
| `sdk/rocketnet_server_gso_linux.cpp:1307` | H | local run byte total |
| `sdk/rocketnet_server_gso_linux.cpp:1308` | H | local batch cursor |
| `sdk/rocketnet_server_gso_linux.cpp:1325` | A | counter_send_dropped counts the unroutable run on its drop path (see out-of-class note) |
| `sdk/rocketnet_server_gso_linux.cpp:1348` | H | local iovec count after the iovecs are filled |
| `sdk/rocketnet_server_gso_linux.cpp:1366` | H | local message count after the message is built |
| `sdk/rocketnet_server_gso_linux.cpp:1381` | A | round-robin socket rotation index; no capacity is consumed |
| `sdk/rocketnet_server_gso_linux.cpp:1400` | A | syscall counter, charged after the call returned |
| `sdk/rocketnet_server_gso_linux.cpp:1403` | A | charged only for messages sendmmsg reported sent |
| `sdk/rocketnet_server_gso_linux.cpp:1405` | A | charged only for segmented messages sendmmsg reported sent |
| `sdk/rocketnet_server_gso_linux.cpp:1407` | H | local; advanced by sendmmsg's success count, sent counters after success |
| `sdk/rocketnet_server_gso_linux.cpp:1411` | A | syscall counter for the failed call; counts the call, not a success |
| `sdk/rocketnet_server_gso_linux.cpp:1432` | A | counter_send_dropped counts the refused messages on the failure path |
| `sdk/rocketnet_server_gso_linux.cpp:1466` | C | try-lock released on the empty-ready return and after the swap |
| `sdk/rocketnet_server_gso_linux.cpp:1478` | A | folds a claimed buffer's totals once, after the swap succeeded |
| `sdk/rocketnet_server_gso_linux.cpp:1479` | A | folds a claimed buffer's drop count once, then zeroes it |
| `sdk/rocketnet_server_gso_linux.cpp:1480` | A | folds a claimed buffer's drop count once, then zeroes it |
| `sdk/rocketnet_server_gso_linux.cpp:1481` | A | folds a claimed buffer's drop count once, then zeroes it |
| `sdk/rocketnet_server_gso_linux.cpp:1482` | A | folds a claimed buffer's drop count once, then zeroes it |
| `sdk/rocketnet_server_gso_linux.cpp:1483` | A | folds a claimed buffer's error count once, then zeroes it |
| `sdk/rocketnet_server_gso_linux.cpp:1484` | A | folds a claimed buffer's batch count once, then zeroes it |
| `sdk/rocketnet_server_gso_linux.cpp:1485` | A | folds a claimed buffer's fault count once, then zeroes it |
| `sdk/rocketnet_server_gso_linux.cpp:1495` | H | local budget parameter, does not outlive the call |
| `sdk/rocketnet_server_gso_linux.cpp:1498` | A | cursor advanced inside the bounded loop with an unconditional copy |
| `sdk/rocketnet_server_gso_linux.cpp:1499` | A | index bounded by the loop guard; entry filled unconditionally |
| `sdk/rocketnet_server_gso_linux.cpp:1514` | A | exchange-to-zero fold of the worker's syscall counter; nothing fails |
| `sdk/rocketnet_server_iocp_windows.cpp:269` | A | injected failure path refunds pending with fetch_sub at line 300 |
| `sdk/rocketnet_server_iocp_windows.cpp:285` | A | refused WSARecvMsg refunds pending at line 300 before retry list |
| `sdk/rocketnet_server_iocp_windows.cpp:292` | A | refused WSARecvFrom refunds pending at line 300 before retry list |
| `sdk/rocketnet_server_iocp_windows.cpp:300` | A | the refund of the pending charge on a refused post |
| `sdk/rocketnet_server_iocp_windows.cpp:306` | A | counts the refused repost on its own failure path |
| `sdk/rocketnet_server_iocp_windows.cpp:355` | A | counts the drop on its own drop path |
| `sdk/rocketnet_server_iocp_windows.cpp:374` | H | local segment count for the URO batch counter |
| `sdk/rocketnet_server_iocp_windows.cpp:378` | A | bounds checked first; slot filled unconditionally after the increment |
| `sdk/rocketnet_server_iocp_windows.cpp:382` | A | charged after the datagram is copied into the fill bank |
| `sdk/rocketnet_server_iocp_windows.cpp:385` | A | counts the drop on its own drop path |
| `sdk/rocketnet_server_iocp_windows.cpp:387` | H | local window cursor |
| `sdk/rocketnet_server_iocp_windows.cpp:390` | A | counted after the split completed; nothing fails after it |
| `sdk/rocketnet_server_iocp_windows.cpp:401` | C | released at 411 on the only path |
| `sdk/rocketnet_server_iocp_windows.cpp:407` | A | charged only inside free_count > 0 branch; swap cannot fail under the mutex |
| `sdk/rocketnet_server_iocp_windows.cpp:408` | A | pop guarded by free_count > 0; bank returned in receive_packets |
| `sdk/rocketnet_server_iocp_windows.cpp:450` | A | counts a dequeued batch only when count > 0 |
| `sdk/rocketnet_server_iocp_windows.cpp:457` | A | test gate; fetch_sub at 460 or 533 on every path |
| `sdk/rocketnet_server_iocp_windows.cpp:460` | A | test gate refund matching the add at 457 |
| `sdk/rocketnet_server_iocp_windows.cpp:478` | A | charged at send completion with the counter chosen by its status |
| `sdk/rocketnet_server_iocp_windows.cpp:479` | A | fetch_sub on completion; matching add in post_send/post_send_run |
| `sdk/rocketnet_server_iocp_windows.cpp:507` | A | room and address checked in the guard before the increment |
| `sdk/rocketnet_server_iocp_windows.cpp:511` | A | charged after the datagram is copied into the fill bank |
| `sdk/rocketnet_server_iocp_windows.cpp:514` | A | counts the drop on its own drop path |
| `sdk/rocketnet_server_iocp_windows.cpp:516` | A | fetch_sub on receive completion; matching add in post_receive |
| `sdk/rocketnet_server_iocp_windows.cpp:533` | A | test gate release matching the add at 457 |
| `sdk/rocketnet_server_iocp_windows.cpp:536` | A | fetch_sub on worker exit; matching add at 737 |
| `sdk/rocketnet_server_iocp_windows.cpp:737` | A | thread-create failure refunds at 741; later failures stop a live worker |
| `sdk/rocketnet_server_iocp_windows.cpp:741` | A | the refund of 737 when the worker thread was not created |
| `sdk/rocketnet_server_iocp_windows.cpp:785` | A | overshoot only on refusal; clamped to queue size and reset to 0 on publish |
| `sdk/rocketnet_server_iocp_windows.cpp:837` | A | counts the failure on its own refusal path; frame returned |
| `sdk/rocketnet_server_iocp_windows.cpp:844` | A | refused WSASendTo refunds pending at line 858 |
| `sdk/rocketnet_server_iocp_windows.cpp:858` | A | the refund of the pending charge on a refused send |
| `sdk/rocketnet_server_iocp_windows.cpp:859` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_server_iocp_windows.cpp:883` | H | local byte total of the run |
| `sdk/rocketnet_server_iocp_windows.cpp:897` | A | refused WSASendMsg refunds pending at line 915 |
| `sdk/rocketnet_server_iocp_windows.cpp:904` | A | charged after WSASendMsg returned success |
| `sdk/rocketnet_server_iocp_windows.cpp:911` | A | charged after WSASendMsg returned IO_PENDING (accepted) |
| `sdk/rocketnet_server_iocp_windows.cpp:915` | A | the refund of the pending charge on a refused run |
| `sdk/rocketnet_server_iocp_windows.cpp:926` | A | counts the USO fallback on its own fallback path |
| `sdk/rocketnet_server_iocp_windows.cpp:930` | A | counts the failed run on its own failure path; frames retired |
| `sdk/rocketnet_server_iocp_windows.cpp:952` | A | counts the unsent frame on the stopping path; frame returned |
| `sdk/rocketnet_server_iocp_windows.cpp:962` | H | local loop index skipping a run posted successfully |
| `sdk/rocketnet_server_iocp_windows.cpp:980` | C | both returns after the acquire release the mutex |
| `sdk/rocketnet_server_iocp_windows.cpp:988` | A | pop guarded by send_free_count check; nothing fails after it |
| `sdk/rocketnet_server_iocp_windows.cpp:990` | A | publish cannot fail after the guard; decremented by send worker |
| `sdk/rocketnet_server_iocp_windows.cpp:992` | A | test counter; fetch_sub at 1033 when the bank is serviced |
| `sdk/rocketnet_server_iocp_windows.cpp:1010` | A | test gate; fetch_sub at 1013 or 1044 on every path |
| `sdk/rocketnet_server_iocp_windows.cpp:1013` | A | test gate refund matching the add at 1010 |
| `sdk/rocketnet_server_iocp_windows.cpp:1016` | C | released before send_bank on the only path |
| `sdk/rocketnet_server_iocp_windows.cpp:1022` | A | dequeue guarded by send_ready_count under the mutex |
| `sdk/rocketnet_server_iocp_windows.cpp:1029` | C | released on the next line |
| `sdk/rocketnet_server_iocp_windows.cpp:1030` | A | bank returned to the free list after send_bank finished |
| `sdk/rocketnet_server_iocp_windows.cpp:1033` | A | test counter release matching the add at 992 |
| `sdk/rocketnet_server_iocp_windows.cpp:1044` | A | test gate release matching the add at 1010 |
| `sdk/rocketnet_server_iocp_windows.cpp:1061` | C | failed try continues; success releases before the copy loop |
| `sdk/rocketnet_server_iocp_windows.cpp:1066` | A | returns last call's exposed banks; assert-bounded, no failure follows |
| `sdk/rocketnet_server_iocp_windows.cpp:1077` | A | room checked before the dequeue; break leaves the bank ready |
| `sdk/rocketnet_server_iocp_windows.cpp:1078` | A | exposed bank freed on the next call at 1066 |
| `sdk/rocketnet_server_iocp_windows.cpp:1079` | A | reset to 0 on every call; fill loop cannot fail |
| `sdk/rocketnet_server_iocp_windows.cpp:1093` | H | local write index into capacity reserved under the lock; no failure |
| `sdk/rocketnet_server_iocp_windows.cpp:1134` | C | test helper; paired with the release call |
| `sdk/rocketnet_server_iocp_windows.cpp:1148` | C | released before the return |
| `sdk/rocketnet_server_iocp_windows.cpp:1169` | C | test helper; paired with the release call |
| `sdk/rocketnet_server_iocp_windows.cpp:1178` | C | released before the return |
| `sdk/rocketnet_server_iocp_windows.cpp:1191` | C | released after each worker scan |
| `sdk/rocketnet_server_protocol.cpp:33` | A | issued after client_slots_assign succeeded; slot kept on every later path |
| `sdk/rocketnet_server_protocol.cpp:89` | A | counts a signature failure on the failure path; reset by window decay |
| `sdk/rocketnet_server_protocol.cpp:96` | A | per-source failure count on the failure path; reset by window decay |
| `sdk/rocketnet_server_protocol.cpp:140` | H | test tally of mac calls made, not successes |
| `sdk/rocketnet_server_protocol.cpp:202` | H | local read cursor |
| `sdk/rocketnet_server_protocol.cpp:284` | B | received counts arrivals, not successes |
| `sdk/rocketnet_server_protocol.cpp:293` | B | counts the wrong-size refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:313` | B | counts the already-connected refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:323` | B | charged after the stored challenge was copied out; returns immediately |
| `sdk/rocketnet_server_protocol.cpp:343` | B | counts the wrong-version refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:353` | B | counts the wrong-protocol-id refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:371` | B | counts the token-expired refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:382` | B | counts the created-in-future refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:394` | B | counts the invalid-timeout refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:405` | B | counts the bad-address-flags refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:415` | B | counts the not-advertised refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:422` | B | counts the wrong-server-key refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:454` | B | counts the no-cookie refusal; a cookie answer follows on that path |
| `sdk/rocketnet_server_protocol.cpp:478` | B | counts the cookie-did-not-verify refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:483` | B | counts the cookie-expired refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:499` | B | counts the internal-error refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:516` | B | counts the budget-exceeded refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:519` | A | per-update work cap counts every post-cookie attempt by design; zeroed each update at 1564 |
| `sdk/rocketnet_server_protocol.cpp:530` | B | counts the token-already-used refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:541` | B | counts the verify-limit refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:553` | C | claim_check is read-only; nothing acquired |
| `sdk/rocketnet_server_protocol.cpp:556` | B | counts the claimed-elsewhere refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:563` | B | counts the map-full refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:564` | B | counts the map-full refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:573` | C | claim_can_record is read-only; nothing acquired |
| `sdk/rocketnet_server_protocol.cpp:576` | B | counts the claim-capacity refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:584` | B | counts the signature failure on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:591` | C | every later non-challenge return releases: 603 directly, 623 for all opened-request outcomes |
| `sdk/rocketnet_server_protocol.cpp:596` | B | counts the claim-record refusal on its own refusal path; record wrote nothing |
| `sdk/rocketnet_server_protocol.cpp:603` | C | release on the sealed-block failure; matching acquire at 591 |
| `sdk/rocketnet_server_protocol.cpp:605` | B | counts the sealed-block refusal on its own refusal path, after releasing the claim |
| `sdk/rocketnet_server_protocol.cpp:623` | C | release on every opened-request refusal or denial; matching acquire at 591 |
| `sdk/rocketnet_server_protocol.cpp:649` | B | counts the sealed-mismatch refusal; caller releases the claim at 623 |
| `sdk/rocketnet_server_protocol.cpp:656` | B | counts the client-id refusal; caller releases the claim at 623 |
| `sdk/rocketnet_server_protocol.cpp:698` | B | counts the internal-error refusal; caller releases the claim at 623 |
| `sdk/rocketnet_server_protocol.cpp:703` | B | counts the no-slots denial after the denial packet was written |
| `sdk/rocketnet_server_protocol.cpp:709` | A | crypto tally counts keypair calls; the keypair call itself cannot fail |
| `sdk/rocketnet_server_protocol.cpp:725` | B | counts the small-order-key refusal; caller releases the claim at 623 |
| `sdk/rocketnet_server_protocol.cpp:735` | C | release on map-add failure; 623 releases again, harmlessly, as release is idempotent |
| `sdk/rocketnet_server_protocol.cpp:737` | B | counts the map-add refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:738` | B | counts the map-full refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:775` | B | counts the internal-error refusal after removing the mapping it added |
| `sdk/rocketnet_server_protocol.cpp:778` | A | advanced after the challenge write returned the exact wire size |
| `sdk/rocketnet_server_protocol.cpp:784` | B | charged after mapping added and challenge built; returns success |
| `sdk/rocketnet_server_protocol.cpp:785` | B | charged after the mapping add and challenge write both succeeded |
| `sdk/rocketnet_server_protocol.cpp:827` | B | counts the already-connected refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:842` | B | counts the client-id refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:852` | B | counts the no-slots denial on the denial path |
| `sdk/rocketnet_server_protocol.cpp:862` | B | counts the internal-error refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:865` | A | advanced after the denied write returned the exact wire size |
| `sdk/rocketnet_server_protocol.cpp:883` | B | charged after slot assign; the keep-alive failure path keeps the slot connected |
| `sdk/rocketnet_server_protocol.cpp:884` | B | charged after slot assign; the keep-alive failure path keeps the slot connected |
| `sdk/rocketnet_server_protocol.cpp:893` | B | counts the internal-error keep-alive failure on its own path |
| `sdk/rocketnet_server_protocol.cpp:912` | C | release helper definition; callers are removal paths of slotless mappings |
| `sdk/rocketnet_server_protocol.cpp:917` | C | release by the mapping's stored token and address; matching acquire at 591 |
| `sdk/rocketnet_server_protocol.cpp:940` | A | counts a packet that did not open; the failure is what it counts |
| `sdk/rocketnet_server_protocol.cpp:950` | B | charged after connection_reset released the slot |
| `sdk/rocketnet_server_protocol.cpp:956` | C | release before the slotless mapping is removed at the integrity limit |
| `sdk/rocketnet_server_protocol.cpp:959` | B | counts the limit being reached, after the teardown |
| `sdk/rocketnet_server_protocol.cpp:979` | B | counts the capacity refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:985` | B | charged after the memcpy; returns 1 |
| `sdk/rocketnet_server_protocol.cpp:1060` | B | received counts arrivals, not successes |
| `sdk/rocketnet_server_protocol.cpp:1081` | B | counts the wrong-size refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1088` | B | counts the response wrong-size refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1097` | B | counts the no-mapping refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1117` | B | counts the zero-sequence refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1124` | B | counts the already-received refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1146` | B | counts the not-connected refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1160` | A | test seam counts plaintext writes; the open did write plaintext into opened |
| `sdk/rocketnet_server_protocol.cpp:1165` | B | counts the did-not-open refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1179` | B | counts the response-size refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1188` | B | counts the nonzero-padding refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1225` | B | counts the bad-ack-block refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1241` | B | counts the wrong-opened-size refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1284` | A | debt added after every refusal rung passed; later branches cannot undo acceptance |
| `sdk/rocketnet_server_protocol.cpp:1290` | B | charged after all rungs passed; returns accepted at once |
| `sdk/rocketnet_server_protocol.cpp:1297` | B | charged after all rungs passed; returns accepted at once |
| `sdk/rocketnet_server_protocol.cpp:1307` | B | counts the capacity refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1312` | B | charged after the capacity check and payload handed out; returns accepted |
| `sdk/rocketnet_server_protocol.cpp:1337` | B | asserted-unreachable null-reassembly branch returns accepted; nothing after can fail |
| `sdk/rocketnet_server_protocol.cpp:1374` | B | charged before connection_reset, which is void and cannot fail |
| `sdk/rocketnet_server_protocol.cpp:1375` | B | charged before connection_reset, which is void and cannot fail |
| `sdk/rocketnet_server_protocol.cpp:1408` | B | counts arrivals, not successes |
| `sdk/rocketnet_server_protocol.cpp:1426` | B | counts the prefilter refusal reason on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1436` | B | counts the reserved-type refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1453` | B | counts the wrong-direction refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:1492` | A | test seam counted after the memcpy it counts |
| `sdk/rocketnet_server_protocol.cpp:1537` | A | schedule advance with an unconditional key rotation; no failure path |
| `sdk/rocketnet_server_protocol.cpp:1580` | B | charged after the mapping removal it counts |
| `sdk/rocketnet_server_protocol.cpp:1587` | C | release before the half-open idle removal; deadline removal needs none, same deadline as claim |
| `sdk/rocketnet_server_protocol.cpp:1589` | B | charged after claim release and mapping removal |
| `sdk/rocketnet_server_protocol.cpp:1604` | B | charged after the reset or removal it counts |
| `sdk/rocketnet_server_protocol.cpp:1605` | B | charged after the reset or removal it counts |
| `sdk/rocketnet_server_protocol.cpp:1612` | A | counts the missing-mapping failure itself |
| `sdk/rocketnet_server_protocol.cpp:1691` | A | advanced after the sealed write returned nonzero bytes |
| `sdk/rocketnet_server_protocol.cpp:1918` | A | counts the missing-mapping refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:2016` | A | advanced after the sealed write returned nonzero bytes |
| `sdk/rocketnet_server_protocol.cpp:2046` | A | counts the missing-mapping refusal on its own refusal path |
| `sdk/rocketnet_server_protocol.cpp:2070` | F | wire payload id generator; drawn after frames and sequence block are reserved |
| `sdk/rocketnet_server_protocol.cpp:2251` | B | charged after the mapping was found; burst writes and reset have no failure return |
| `sdk/rocketnet_server_protocol.cpp:2252` | B | charged after the mapping was found; burst writes and reset have no failure return |
| `sdk/rocketnet_server_protocol.cpp:2268` | A | debt added after replay advance as the last step; void function |
| `sdk/rocketnet_server_replay_marks.cpp:152` | H | test seam witness counting forget callbacks |
| `sdk/rocketnet_server_rio_windows.cpp:224` | A | counter helper; each caller charges the outcome it observed |
| `sdk/rocketnet_server_rio_windows.cpp:232` | H | local sum of outstanding counts |
| `sdk/rocketnet_server_rio_windows.cpp:319` | G | charged after the commit call returned success |
| `sdk/rocketnet_server_rio_windows.cpp:381` | A | charged after RIOReceiveEx returned success |
| `sdk/rocketnet_server_rio_windows.cpp:404` | G | SENT or FAILED chosen from the completion status, after the fact |
| `sdk/rocketnet_server_rio_windows.cpp:406` | A | fetch_sub on completion; matching add at 822 counts only posted sends |
| `sdk/rocketnet_server_rio_windows.cpp:425` | H | local count of completions retired |
| `sdk/rocketnet_server_rio_windows.cpp:463` | C | released on the next statement |
| `sdk/rocketnet_server_rio_windows.cpp:483` | A | fetch_sub of dequeued receive completions; matching add at 381 |
| `sdk/rocketnet_server_rio_windows.cpp:615` | A | initial free list fill; create failure destroys the whole transport |
| `sdk/rocketnet_server_rio_windows.cpp:704` | A | overflow clamped in send_packets; zero-byte slot never posted; phase index reset on reuse |
| `sdk/rocketnet_server_rio_windows.cpp:709` | G | counts the failure (queue full); overshot index reset at phase switch |
| `sdk/rocketnet_server_rio_windows.cpp:716` | G | counts the failure; slot left at zero bytes, skipped at post, freed at phase switch |
| `sdk/rocketnet_server_rio_windows.cpp:763` | C | released before both later returns |
| `sdk/rocketnet_server_rio_windows.cpp:771` | A | inside the hand branch under the mutex; nothing fails after it |
| `sdk/rocketnet_server_rio_windows.cpp:777` | G | counts the wait on its own not-handed return |
| `sdk/rocketnet_server_rio_windows.cpp:814` | H | counted only when RIOSendEx succeeded; refused post frees the slot |
| `sdk/rocketnet_server_rio_windows.cpp:818` | G | counts the failed post; slot freed, not added to outstanding |
| `sdk/rocketnet_server_rio_windows.cpp:822` | A | adds only the successfully posted count |
| `sdk/rocketnet_server_rio_windows.cpp:841` | A | fetch_sub on completion; matching add at 381; refused repost re-added on retry |
| `sdk/rocketnet_server_rio_windows.cpp:852` | G | counts the oversize drop from the completion status itself |
| `sdk/rocketnet_server_rio_windows.cpp:854` | G | counts the failed receive from the completion status itself |
| `sdk/rocketnet_server_rio_windows.cpp:861` | G | counts the size drop on its own drop branch |
| `sdk/rocketnet_server_rio_windows.cpp:863` | G | counts the address-read failure on its own drop branch |
| `sdk/rocketnet_server_rio_windows.cpp:865` | G | counts the overflow drop on its own (unreachable) drop branch |
| `sdk/rocketnet_server_rio_windows.cpp:868` | A | room checked first; slot filled unconditionally after the increment |
| `sdk/rocketnet_server_rio_windows.cpp:872` | G | charged after the datagram was copied into the bank; no later failure |
| `sdk/rocketnet_server_rio_windows.cpp:880` | G | counts the repost failure; slice kept on the retry list |
| `sdk/rocketnet_server_rio_windows.cpp:903` | G | counts the repost failure; slice kept on the retry list |
| `sdk/rocketnet_server_rio_windows.cpp:904` | H | local; keeps only slices whose re-post was refused |
| `sdk/rocketnet_server_rio_windows.cpp:929` | H | local count of completions dequeued |
| `sdk/rocketnet_server_rio_windows.cpp:930` | H | local room left in the fill bank |
| `sdk/rocketnet_server_rio_windows.cpp:946` | C | released before posting |
| `sdk/rocketnet_server_rio_windows.cpp:951` | A | dequeue guarded by send_ready_count under the mutex |
| `sdk/rocketnet_server_rio_windows.cpp:978` | C | released before the return |
| `sdk/rocketnet_server_rio_windows.cpp:982` | A | charged only when free_count > 0; swap cannot fail |
| `sdk/rocketnet_server_rio_windows.cpp:983` | A | pop guarded by free_count > 0; bank returned in receive_packets |
| `sdk/rocketnet_server_rio_windows.cpp:1046` | C | released before the copy loop; failed try returns holding nothing |
| `sdk/rocketnet_server_rio_windows.cpp:1049` | A | returns last call's exposed banks; no failure follows |
| `sdk/rocketnet_server_rio_windows.cpp:1058` | A | room checked before the dequeue; break leaves the bank ready |
| `sdk/rocketnet_server_rio_windows.cpp:1059` | A | exposed bank freed on the next call at 1049 |
| `sdk/rocketnet_server_rio_windows.cpp:1060` | H | local; bank exposed and counted together under the lock |
| `sdk/rocketnet_server_rio_windows.cpp:1069` | A | reset to 0 on every call; fill loop cannot fail |
| `sdk/rocketnet_server_rio_windows.cpp:1101` | A | test seam call count, not capacity |
| `sdk/rocketnet_server_rio_windows.cpp:1117` | A | test seam call count, not capacity |
| `sdk/rocketnet_server_rio_windows.cpp:1145` | A | test fixture free list fill after every failure return |
| `sdk/rocketnet_server_rio_windows.cpp:1238` | A | test seam stack pop guarded by completed > 0 |
| `sdk/rocketnet_server_rio_windows.cpp:1239` | H | test fake dequeue local count |
| `sdk/rocketnet_server_rio_windows.cpp:1245` | A | test seam call count, not capacity |
| `sdk/rocketnet_server_rio_windows.cpp:1257` | A | test seam call count, not capacity |
| `sdk/rocketnet_server_rio_windows.cpp:1266` | A | test seam call count, not capacity |
| `sdk/rocketnet_server_rio_windows.cpp:1268` | A | test seam moves deferred to completed; guarded by deferred > 0 |
| `sdk/rocketnet_server_rio_windows.cpp:1271` | A | test seam push; no failure follows |
| `sdk/rocketnet_server_rio_windows.cpp:1430` | C | released before the return |
| `sdk/rocketnet_server_socket_linux.cpp:153` | H | empty-free refusal returns before the decrement |
| `sdk/rocketnet_server_socket_linux.cpp:155` | H | ready entry written before increment; no later failure |
| `sdk/rocketnet_server_socket_linux.cpp:159` | C | returns -1 without change when empty; claimer recycles after use |
| `sdk/rocketnet_server_socket_linux.cpp:164` | H | empty refusal returns before the decrement |
| `sdk/rocketnet_server_socket_linux.cpp:172` | H | release with matching claim; no failure path |
| `sdk/rocketnet_server_socket_linux.cpp:276` | H | not code: a struct field comment |
| `sdk/rocketnet_server_socket_linux.cpp:277` | H | not code: a struct field comment |
| `sdk/rocketnet_server_socket_linux.cpp:486` | A | guarded by num_free > 0; callers free the frame on every refusal (3127, 3131) |
| `sdk/rocketnet_server_socket_linux.cpp:508` | A | only caller is create; its failure aborts the whole socket |
| `sdk/rocketnet_server_socket_linux.cpp:654` | H | local parse cursor |
| `sdk/rocketnet_server_socket_linux.cpp:663` | H | local parse cursor |
| `sdk/rocketnet_server_socket_linux.cpp:684` | H | local parse cursor |
| `sdk/rocketnet_server_socket_linux.cpp:691` | H | local count of parsed cpus |
| `sdk/rocketnet_server_socket_linux.cpp:693` | H | local parse cursor |
| `sdk/rocketnet_server_socket_linux.cpp:901` | H | not code: a log string |
| `sdk/rocketnet_server_socket_linux.cpp:990` | H | local count of matched IRQ lines for a warning |
| `sdk/rocketnet_server_socket_linux.cpp:1030` | H | not code: a log string |
| `sdk/rocketnet_server_socket_linux.cpp:1046` | H | not code: a log string |
| `sdk/rocketnet_server_socket_linux.cpp:1054` | H | not code: a log string |
| `sdk/rocketnet_server_socket_linux.cpp:1464` | H | local boot id copy index |
| `sdk/rocketnet_server_socket_linux.cpp:1477` | H | local parse cursor over /proc/self/stat |
| `sdk/rocketnet_server_socket_linux.cpp:1495` | H | not code: a log string |
| `sdk/rocketnet_server_socket_linux.cpp:1835` | C | ring reserve failure aborts the whole socket create |
| `sdk/rocketnet_server_socket_linux.cpp:1895` | H | local count for the bound-mode log line |
| `sdk/rocketnet_server_socket_linux.cpp:2099` | A | overshoot only on refusal; worker clamps to queue size; reset to 0 on publish/recycle |
| `sdk/rocketnet_server_socket_linux.cpp:2249` | C | both paths after the try release the mutex |
| `sdk/rocketnet_server_socket_linux.cpp:2306` | A | counts the failure on its own failure path |
| `sdk/rocketnet_server_socket_linux.cpp:2392` | C | unit check of claim order on a local exchange |
| `sdk/rocketnet_server_socket_linux.cpp:2393` | C | unit check of claim order on a local exchange |
| `sdk/rocketnet_server_socket_linux.cpp:2396` | C | unit check of claim order on a local exchange |
| `sdk/rocketnet_server_socket_linux.cpp:2435` | C | released on the following line |
| `sdk/rocketnet_server_socket_linux.cpp:2436` | C | test claim of a local fixture freed at the end |
| `sdk/rocketnet_server_socket_linux.cpp:2445` | C | released after publish on the following lines |
| `sdk/rocketnet_server_socket_linux.cpp:2454` | C | deliberately held to test a failed try; released at 2458 |
| `sdk/rocketnet_server_socket_linux.cpp:2666` | A | bound checked first; slot filled unconditionally after the increment |
| `sdk/rocketnet_server_socket_linux.cpp:2731` | H | local sum of failure counters |
| `sdk/rocketnet_server_socket_linux.cpp:2792` | B | sums per-cpu values into an output zeroed at entry |
| `sdk/rocketnet_server_socket_linux.cpp:2817` | C | failed try continues holding the buffer; next call retries the recycle |
| `sdk/rocketnet_server_socket_linux.cpp:2820` | C | claimed buffer recycled on the call after it is read |
| `sdk/rocketnet_server_socket_linux.cpp:2826` | C | failed try continues; success releases after the claim |
| `sdk/rocketnet_server_socket_linux.cpp:2828` | C | claimed buffer recycled on the call after it is read |
| `sdk/rocketnet_server_socket_linux.cpp:2890` | C | all-or-zero reserve; a zero keeps the pending frames for retry |
| `sdk/rocketnet_server_socket_linux.cpp:2959` | H | consumed descriptor's frame returns to fill queue even when refused |
| `sdk/rocketnet_server_socket_linux.cpp:2999` | A | charged after the frame was parsed and copied |
| `sdk/rocketnet_server_socket_linux.cpp:3026` | C | released after publish; failed publish keeps the producer filling |
| `sdk/rocketnet_server_socket_linux.cpp:3054` | C | released on the following line |
| `sdk/rocketnet_server_socket_linux.cpp:3055` | C | claimed buffer recycled at 3153 once fully copied |
| `sdk/rocketnet_server_socket_linux.cpp:3122` | A | counts the refused length on its own refusal path; frame freed |
| `sdk/rocketnet_server_socket_linux.cpp:3132` | A | counts the refused build on its own refusal path; frame freed |
| `sdk/rocketnet_server_socket_linux.cpp:3138` | H | batch kept intact for retry when tx reserve is short |
| `sdk/rocketnet_server_socket_linux.cpp:3142` | A | advanced after the packet is batched or refused; frame-exhaustion break precedes it |
| `sdk/rocketnet_server_socket_linux.cpp:3152` | C | released after the recycle on the following lines |
| `sdk/rocketnet_server_socket_linux.cpp:3167` | C | all-or-zero reserve; zero keeps the batch and its frames for retry |
| `sdk/rocketnet_server_transport.cpp:432` | A | helper; charge position judged at callers (see 852, 3087) |
| `sdk/rocketnet_server_transport.cpp:433` | A | helper; charge position judged at callers (see 852, 3087) |
| `sdk/rocketnet_server_transport.cpp:437` | A | helper; charge position judged at callers (see 852, 3087) |
| `sdk/rocketnet_server_transport.cpp:438` | A | helper; charge position judged at callers (see 852, 3087) |
| `sdk/rocketnet_server_transport.cpp:468` | A | counts datagrams already received; callers charge after the read |
| `sdk/rocketnet_server_transport.cpp:469` | A | counts bytes already received; callers charge after the read |
| `sdk/rocketnet_server_transport.cpp:473` | A | counts datagrams already received; callers charge after the read |
| `sdk/rocketnet_server_transport.cpp:474` | A | counts bytes already received; callers charge after the read |
| `sdk/rocketnet_server_transport.cpp:586` | F | counts the failure: unreachable link address refusal only |
| `sdk/rocketnet_server_transport.cpp:610` | C | declaration; the definition acquires and releases on every path |
| `sdk/rocketnet_server_transport.cpp:621` | C | production stub, no reservation |
| `sdk/rocketnet_server_transport.cpp:649` | C | take_setting applies a pending setting under a released mutex |
| `sdk/rocketnet_server_transport.cpp:668` | C | mutex released after the simulator send; the refusal is the drop judged at 852 |
| `sdk/rocketnet_server_transport.cpp:670` | F | counts the drop on its own busy-mutex refusal path |
| `sdk/rocketnet_server_transport.cpp:722` | C | every return after the acquire releases it |
| `sdk/rocketnet_server_transport.cpp:741` | C | released on the next statement but one; no return between |
| `sdk/rocketnet_server_transport.cpp:783` | H | local send count; buffer reset after the synchronous send |
| `sdk/rocketnet_server_transport.cpp:808` | C | every caller commits or aborts the frame it reserved |
| `sdk/rocketnet_server_transport.cpp:872` | A | commit of a reserved slot after its length is stamped; no failure follows |
| `sdk/rocketnet_server_transport.cpp:1058` | H | local exclusion list index, asserted bounded |
| `sdk/rocketnet_server_transport.cpp:1061` | H | local exclusion list index |
| `sdk/rocketnet_server_transport.cpp:1062` | H | local exclusion list index |
| `sdk/rocketnet_server_transport.cpp:1065` | H | local exclusion list index |
| `sdk/rocketnet_server_transport.cpp:1067` | H | local exclusion list index |
| `sdk/rocketnet_server_transport.cpp:1068` | H | local exclusion list index |
| `sdk/rocketnet_server_transport.cpp:1069` | H | local exclusion list index |
| `sdk/rocketnet_server_transport.cpp:1070` | H | local exclusion list index |
| `sdk/rocketnet_server_transport.cpp:1366` | H | counts the refusal on its own refusal path (local) |
| `sdk/rocketnet_server_transport.cpp:1369` | H | local count of plans within budget |
| `sdk/rocketnet_server_transport.cpp:1525` | H | not code: a log string |
| `sdk/rocketnet_server_transport.cpp:1553` | H | counts the open refusal on its own refusal path (local) |
| `sdk/rocketnet_server_transport.cpp:1555` | H | counts the refusal on its own refusal path (local) |
| `sdk/rocketnet_server_transport.cpp:1557` | H | counts the refusal on its own refusal path (local) |
| `sdk/rocketnet_server_transport.cpp:2001` | H | local sum recomputed each update from stats frames |
| `sdk/rocketnet_server_transport.cpp:2002` | H | local sum recomputed each update |
| `sdk/rocketnet_server_transport.cpp:2003` | H | local sum recomputed each update |
| `sdk/rocketnet_server_transport.cpp:2004` | H | local sum recomputed each update |
| `sdk/rocketnet_server_transport.cpp:2016` | A | per-update frame number, no operation to fail |
| `sdk/rocketnet_server_transport.cpp:2103` | A | counts datagrams the staged receive already returned |
| `sdk/rocketnet_server_transport.cpp:2104` | A | counts bytes the staged receive already returned |
| `sdk/rocketnet_server_transport.cpp:2123` | C | reserved frame is committed or aborted on every path |
| `sdk/rocketnet_server_transport.cpp:2128` | H | local pointer offset into a reserved frame |
| `sdk/rocketnet_server_transport.cpp:2166` | A | charged after the payload opened into its slot; no failure follows |
| `sdk/rocketnet_server_transport.cpp:2185` | A | charged after the bounds check and store; no failure follows |
| `sdk/rocketnet_server_transport.cpp:2190` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_server_transport.cpp:2229` | A | counts the failure on its own fault path |
| `sdk/rocketnet_server_transport.cpp:2234` | A | counts the failure on its own fault path |
| `sdk/rocketnet_server_transport.cpp:2245` | A | counts the failure on its own fault path |
| `sdk/rocketnet_server_transport.cpp:2249` | A | counts a datagram already received, after the fault check |
| `sdk/rocketnet_server_transport.cpp:2250` | A | counts bytes already received, after the fault check |
| `sdk/rocketnet_server_transport.cpp:2258` | A | received count for an oversize datagram that was received; paired with its refusal counter |
| `sdk/rocketnet_server_transport.cpp:2259` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_server_transport.cpp:2280` | C | reserved frame is committed or aborted on every path |
| `sdk/rocketnet_server_transport.cpp:2285` | H | local pointer offset into a reserved frame |
| `sdk/rocketnet_server_transport.cpp:2316` | A | charged after the payload opened into its slot; no failure follows |
| `sdk/rocketnet_server_transport.cpp:2335` | A | charged after the bounds check and store; no failure follows |
| `sdk/rocketnet_server_transport.cpp:2340` | A | counts the failure on its own refusal path |
| `sdk/rocketnet_server_transport.cpp:2344` | H | local loop bound of packets processed |
| `sdk/rocketnet_server_transport.cpp:2386` | C | committed or aborted; a full staging breaks with the cursor kept for retry |
| `sdk/rocketnet_server_transport.cpp:2408` | A | charges allowance by seals the drain actually committed (delta of disconnect_seals) |
| `sdk/rocketnet_server_transport.cpp:2411` | A | per-update generation count, no operation to fail |
| `sdk/rocketnet_server_transport.cpp:2435` | A | flush generation gate; an extra bump costs one empty flush, no capacity |
| `sdk/rocketnet_server_transport.cpp:2573` | C | arena reclaimed wholesale at flush; abort on lease-full needs no undo |
| `sdk/rocketnet_server_transport.cpp:2577` | F | counts the failure on its own arena-full refusal path |
| `sdk/rocketnet_server_transport.cpp:2592` | F | counts the failure on its own lease-full refusal path |
| `sdk/rocketnet_server_transport.cpp:2606` | F | counts the failure on its own staged-start refusal path |
| `sdk/rocketnet_server_transport.cpp:2609` | F | flush-dirty generation; an extra bump only causes one harmless extra flush |
| `sdk/rocketnet_server_transport.cpp:2616` | F | lease-full abort zeroes wire_bytes; flush skips empty slot and resets the ring |
| `sdk/rocketnet_server_transport.cpp:2620` | F | counts the failure on its own ring-full refusal path |
| `sdk/rocketnet_server_transport.cpp:2647` | F | counts the failure on its own lease-full refusal path |
| `sdk/rocketnet_server_transport.cpp:2709` | C | take consumes the lease the abort is for; frame aborted |
| `sdk/rocketnet_server_transport.cpp:2730` | C | callers abort every reserved frame on each failure; send_staged only gates a flush |
| `sdk/rocketnet_server_transport.cpp:2743` | F | counts the failure on its own staged-start refusal path |
| `sdk/rocketnet_server_transport.cpp:2746` | F | flush-dirty generation; an extra bump only causes one harmless extra flush |
| `sdk/rocketnet_server_transport.cpp:2752` | F | abort_fill_frame zeroes wire_bytes; flush skips empty slot and resets the ring |
| `sdk/rocketnet_server_transport.cpp:2756` | F | counts the failure on its own ring-full refusal path |
| `sdk/rocketnet_server_transport.cpp:2832` | C | start_payload and seal failures both abort the reserved frame |
| `sdk/rocketnet_server_transport.cpp:2873` | C | partial reservation, start_fragments and seal failures abort the frames |
| `sdk/rocketnet_server_transport.cpp:2929` | H | local; added after the seal succeeded and the frame committed |
| `sdk/rocketnet_server_transport.cpp:3009` | C | take consumes the lease; every finish failure path aborts the frame |
| `sdk/rocketnet_server_transport.cpp:3045` | A | folds the pending pair charged by commits; read-and-clear |
| `sdk/rocketnet_server_transport.cpp:3046` | A | folds the pending pair charged by commits; read-and-clear |
| `sdk/rocketnet_server_transport.cpp:3094` | H | local send count (stats charge here is the recorded 3087 finding) |
| `sdk/rocketnet_server_transport.cpp:3180` | A | burst written before the count; no failure follows |
| `sdk/rocketnet_server_transport.cpp:3183` | A | slot is disconnected unconditionally right after (reset follows) |
| `sdk/rocketnet_server_transport.cpp:3184` | A | slot is disconnected unconditionally right after (reset follows) |
| `sdk/rocketnet_server_transport.cpp:3206` | C | seal failure aborts the frame; success commits |
| `sdk/rocketnet_server_transport.cpp:3222` | H | local; counted after the frame committed |
| `sdk/rocketnet_server_transport.cpp:3223` | A | charged after the commit of the sealed frame |
| `sdk/rocketnet_server_transport.cpp:3224` | A | charged after the commit of the sealed frame |
| `sdk/rocketnet_server_transport.cpp:3225` | A | charged after the commit of the sealed frame |
| `sdk/rocketnet_server_transport.cpp:3234` | A | release when the head burst is fully sealed |
| `sdk/rocketnet_server_transport.cpp:3236` | A | cursor follows the removed head; release side |
| `sdk/rocketnet_server_transport.cpp:3240` | A | bumped only when at least one frame committed |
| `sdk/rocketnet_server_transport.cpp:3283` | A | counts seals the protocol already performed into the burst buffer |
| `sdk/rocketnet_server_transport.cpp:3284` | A | allowance charged for seals already done; over-charge only defers later bursts one update |
| `sdk/rocketnet_server_transport.cpp:3290` | C | reserved frame committed on the next lines; NULL breaks with nothing held |
| `sdk/rocketnet_server_transport.cpp:3296` | H | local; counted after the frame committed |
| `sdk/rocketnet_server_transport.cpp:3302` | A | bumped only when at least one frame committed |
| `sdk/rocketnet_server_transport.cpp:3380` | A | delta of the transport's cumulative drop count, baseline then advanced |
| `sdk/rocketnet_server_transport.cpp:3381` | A | delta of the transport's cumulative drop count, baseline then advanced |
| `sdk/rocketnet_server_transport.cpp:3548` | H | test seam counting flush callbacks |
| `sdk/rocketnet_server_uring_linux.cpp:254` | A | test fake: charged after the capacity check and the sqe is returned |
| `sdk/rocketnet_server_uring_linux.cpp:641` | A | counts the io_uring_enter call made, by definition |
| `sdk/rocketnet_server_uring_linux.cpp:657` | A | test hook budget consumed by the injected failure it implements |
| `sdk/rocketnet_server_uring_linux.cpp:696` | A | counts the failure itself; no buffer id to return |
| `sdk/rocketnet_server_uring_linux.cpp:729` | A | counts the truncated-name failure; buffer still returned at the end |
| `sdk/rocketnet_server_uring_linux.cpp:733` | A | counts the size failure; buffer still returned at the end |
| `sdk/rocketnet_server_uring_linux.cpp:735` | A | counts the payload-truncation failure itself |
| `sdk/rocketnet_server_uring_linux.cpp:739` | A | counts the bad-source failure itself |
| `sdk/rocketnet_server_uring_linux.cpp:746` | A | counts the cmsg fault itself |
| `sdk/rocketnet_server_uring_linux.cpp:747` | A | counts the size drop on its own failure path |
| `sdk/rocketnet_server_uring_linux.cpp:748` | A | counts the too-large drop on its own failure path |
| `sdk/rocketnet_server_uring_linux.cpp:752` | A | counts the zero-segment drop itself |
| `sdk/rocketnet_server_uring_linux.cpp:757` | A | defined as buffers holding more than one segment, true regardless of segment drops |
| `sdk/rocketnet_server_uring_linux.cpp:766` | A | counts the bad segment on its drop path |
| `sdk/rocketnet_server_uring_linux.cpp:768` | A | counts the oversize segment on its drop path |
| `sdk/rocketnet_server_uring_linux.cpp:772` | A | counts the full-bank refusal itself |
| `sdk/rocketnet_server_uring_linux.cpp:776` | A | index taken after the bound check; copy follows unconditionally |
| `sdk/rocketnet_server_uring_linux.cpp:780` | A | charged after the copy into the fill bank |
| `sdk/rocketnet_server_uring_linux.cpp:807` | A | charged only when arm returned true |
| `sdk/rocketnet_server_uring_linux.cpp:809` | A | counts the failed re-arm itself |
| `sdk/rocketnet_server_uring_linux.cpp:832` | A | release on the send CQE; matching increment at line 1674 |
| `sdk/rocketnet_server_uring_linux.cpp:846` | A | counts the fallback event itself |
| `sdk/rocketnet_server_uring_linux.cpp:849` | A | counts the failed CQE's packets on the failure path |
| `sdk/rocketnet_server_uring_linux.cpp:853` | A | charged only on a CQE with res >= 0 |
| `sdk/rocketnet_server_uring_linux.cpp:884` | A | counts the ENOBUFS termination itself |
| `sdk/rocketnet_server_uring_linux.cpp:919` | H | local count of CQEs reaped and advanced |
| `sdk/rocketnet_server_uring_linux.cpp:929` | A | counts the io_uring_enter call made, by definition |
| `sdk/rocketnet_server_uring_linux.cpp:939` | A | test counter of wait calls; no failure path |
| `sdk/rocketnet_server_uring_linux.cpp:947` | A | counts the io_uring_enter call made, by definition |
| `sdk/rocketnet_server_uring_linux.cpp:1253` | H | local; counted only when setsockopt succeeded |
| `sdk/rocketnet_server_uring_linux.cpp:1272` | H | local; counted only when setsockopt succeeded, partial enable rolled back |
| `sdk/rocketnet_server_uring_linux.cpp:1488` | A | overshoot on full is clamped in send_packets and reset to 0 at ResetIndex |
| `sdk/rocketnet_server_uring_linux.cpp:1585` | H | local reap budget |
| `sdk/rocketnet_server_uring_linux.cpp:1594` | H | local reap budget |
| `sdk/rocketnet_server_uring_linux.cpp:1604` | A | round-robin socket rotation index; no capacity is consumed |
| `sdk/rocketnet_server_uring_linux.cpp:1615` | H | local cursor skipping an aborted slot |
| `sdk/rocketnet_server_uring_linux.cpp:1623` | A | counts the unroutable slot on its drop path |
| `sdk/rocketnet_server_uring_linux.cpp:1624` | H | local cursor; unroutable slot counted send_dropped |
| `sdk/rocketnet_server_uring_linux.cpp:1660` | A | test hook budget consumed by the injected failure it implements |
| `sdk/rocketnet_server_uring_linux.cpp:1672` | A | documented as segmented SENDMSGs submitted; sqe is prepared and queued here |
| `sdk/rocketnet_server_uring_linux.cpp:1674` | A | sqe queued in the ring; its CQE always decrements at line 832 |
| `sdk/rocketnet_server_uring_linux.cpp:1675` | H | local; counted after the SQE was prepared, which cannot fail |
| `sdk/rocketnet_server_uring_linux.cpp:1676` | H | cursor stored to send_submitted; SQE stays in ring for a later submit |
| `sdk/rocketnet_server_uring_linux.cpp:1680` | H | local reap budget |
| `sdk/rocketnet_server_uring_linux.cpp:1690` | H | local reap budget |
| `sdk/rocketnet_server_uring_linux.cpp:1694` | A | counts the deferred-reap condition itself |
| `sdk/rocketnet_server_uring_linux.cpp:1713` | A | counts the deferral itself |
| `sdk/rocketnet_server_uring_linux.cpp:1733` | A | charged only when arm returned true |
| `sdk/rocketnet_server_uring_linux.cpp:1735` | A | counts the failed re-arm itself |
| `sdk/rocketnet_server_uring_linux.cpp:1762` | A | counts the deferred-reap condition itself |
| `sdk/rocketnet_slots.cpp:54` | C | const predicate; reserves nothing |
| `sdk/rocketnet_slots.h:51` | C | declaration of a const predicate; reserves nothing |
| `sdk/rocketnet_token_tables.cpp:185` | C | const lookup; acquires nothing |
| `sdk/rocketnet_token_tables.cpp:271` | A | monotonic age stamp for eviction order, not capacity |
| `sdk/rocketnet_token_tables.cpp:356` | A | monotonic age stamp on rebinding an expired entry |
| `sdk/rocketnet_token_tables.cpp:369` | C | const lookup; acquires nothing |
| `sdk/rocketnet_token_tables.cpp:375` | C | const lookup; acquires nothing |
| `sdk/rocketnet_token_tables.cpp:385` | C | every refusal after the claim in server_protocol releases it (603, 623, 735) |
| `sdk/rocketnet_token_tables.cpp:400` | A | monotonic age stamp on reusing an expired claim |
| `sdk/rocketnet_token_tables.cpp:418` | C | const capacity check; acquires nothing |
| `sdk/rocketnet_token_tables.cpp:439` | C | the release itself |
| `sdk/rocketnet_token_tables.h:65` | C | declaration; const lookup |
| `sdk/rocketnet_token_tables.h:75` | C | declaration; const check |
| `sdk/rocketnet_token_tables.h:84` | C | declaration; caller releases on every refusal |
| `sdk/rocketnet_token_tables.h:94` | C | declaration of the release |
| `sdk/rocketnet_uring_codec.h:133` | H | local cmsg walk offset |
| `sdk/rocketnet_window_ack.cpp:29` | F | helper definition; each call site judged separately |
| `sdk/rocketnet_window_ack.cpp:84` | A | release: decrement on unlink, matching increment on push |
| `sdk/rocketnet_window_ack.cpp:106` | A | list-size count after the link completes; no failure path |
| `sdk/rocketnet_window_ack.cpp:128` | A | list-size count after the link completes; no failure path |
| `sdk/rocketnet_window_ack.cpp:155` | A | list-size count after the link completes; no failure path |
| `sdk/rocketnet_window_ack.cpp:245` | C | definition of a release; reclaims retired head entries |
| `sdk/rocketnet_window_ack.cpp:254` | A | release: head advanced past retired entries |
| `sdk/rocketnet_window_ack.cpp:258` | A | release: head advanced past retired entries |
| `sdk/rocketnet_window_ack.cpp:404` | A | dev-build diagnostic counting a resend that is happening; no failure path |
| `sdk/rocketnet_window_ack.cpp:426` | G | the event is always stored after the count; a full ring drops only the oldest |
| `sdk/rocketnet_window_ack.cpp:431` | A | ring-full discard of oldest, counted as RECEIPT_RING_FULL |
| `sdk/rocketnet_window_ack.cpp:432` | G | counts the ring-full discard on the discard itself |
| `sdk/rocketnet_window_ack.cpp:437` | A | charged after the event is written; no failure path |
| `sdk/rocketnet_window_ack.cpp:502` | C | release call before the write; no acquire to leak |
| `sdk/rocketnet_window_ack.cpp:512` | G | counts the eviction after the entry was removed |
| `sdk/rocketnet_window_ack.cpp:514` | A | release: evicted entry's references retired before the new write |
| `sdk/rocketnet_window_ack.cpp:518` | A | release: eviction advances the head; counted SENT_MAP_EVICTED |
| `sdk/rocketnet_window_ack.cpp:519` | C | release call inside eviction loop; no acquire to leak |
| `sdk/rocketnet_window_ack.cpp:530` | A | refs written before advancing; sent_index insert cannot fail with 2P slots |
| `sdk/rocketnet_window_ack.cpp:540` | A | entry written before advancing; insert cannot fail with 2P slots for P entries |
| `sdk/rocketnet_window_ack.cpp:615` | A | test-hook cost counter under ROCKETNET_ACK_COST_TEST_HOOKS |
| `sdk/rocketnet_window_ack.cpp:631` | A | test-hook cost counter under ROCKETNET_ACK_COST_TEST_HOOKS |
| `sdk/rocketnet_window_ack.cpp:636` | H | local chain rank count |
| `sdk/rocketnet_window_ack.cpp:655` | C | release call after the acked entry is removed |
| `sdk/rocketnet_window_ack.cpp:702` | A | cursor advanced after the entry is resolved or skipped |
| `sdk/rocketnet_window_ack.cpp:735` | H | local; counted after on_ack matched |
| `sdk/rocketnet_window_ack.cpp:810` | A | diagnostic visit counter; counts visits, not capacity |
| `sdk/rocketnet_window_ack.cpp:811` | H | local walk bound |
| `sdk/rocketnet_window_ack.cpp:824` | H | test-only walk; void commit follows, no failing step after the id advance |
| `sdk/rocketnet_window_ack.cpp:827` | G | counted after the commit; only unit tests call rn_window_ack_resend |
| `sdk/rocketnet_window_ack.cpp:829` | H | local output index after commit |
| `sdk/rocketnet_window_ack.cpp:847` | H | local sum of the two FIFO walks |
| `sdk/rocketnet_window_ack.cpp:860` | H | local count of live entries |
| `sdk/rocketnet_window_receive.cpp:14` | F | counts the failure: duplicate record refusal |
| `sdk/rocketnet_window_receive.cpp:99` | A | advances over received bits after the accept is final |
| `sdk/rocketnet_window_send.cpp:27` | C | a check; refusal charges only the WINDOW_STALL counter once |
| `sdk/rocketnet_window_send.cpp:48` | F | counts the failure: window stall on the refusal path, once per stall |
| `sdk/rocketnet_window_send.cpp:109` | C | next advanced only after admit returned success |
| `sdk/rocketnet_window_send.cpp:113` | A | advanced only after admit succeeded; a failed packet resends the number |
| `sdk/rocketnet_window_send.cpp:142` | C | next advanced only after admit returned success |
| `sdk/rocketnet_window_send.cpp:148` | A | advanced only after admit succeeded; numbers persist for resend by design |
| `sdk/rocketnet_window_send.cpp:174` | A | advances U over acked entries only; no failure path |
| `sdk/rocketnet_xdp_frame.h:50` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:51` | H | local data cursor |
| `sdk/rocketnet_xdp_frame.h:52` | H | local remaining byte count |
| `sdk/rocketnet_xdp_frame.h:55` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:72` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:74` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:75` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:76` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:77` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:80` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:83` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:85` | H | local checksum accumulator |
| `sdk/rocketnet_xdp_frame.h:353` | A | not_ip counts the short frame on its own refusal path |
| `sdk/rocketnet_xdp_frame.h:369` | A | ipv6_extension counts the refusal itself |
| `sdk/rocketnet_xdp_frame.h:375` | A | not_ip counts the refusal itself |
| `sdk/rocketnet_xdp_frame.h:382` | A | other_family counts the refusal itself |
| `sdk/rocketnet_xdp_frame.h:391` | A | malformed counts the refusal itself |
| `sdk/rocketnet_xdp_frame.h:422` | H | counts the failure on its own refusal path |
| `sdk/rocketnet_xdp_frame.h:446` | A | descriptor counts the refusal itself |
| `sdk/rocketnet_xdp_frame.h:579` | A | logical LRU clock charged after the input checks; learn always completes |
| `sdk/rocketnet_xdp_frame.h:634` | H | counts the failure on its own refusal path |
| `sdk/rocketnet_xdp_free_list.h:21` | H | every refusal returns before the push; push is the last step |
| `sdk/rocketnet_xdp_replay_marks.h:77` | A | refused counts the full-list refusal itself |
| `sdk/rocketnet_xdp_replay_marks.h:81` | A | charged after the capacity check and the key write |
| `sdk/rocketnet_xdp_replay_marks.h:92` | A | refused counts the full-list refusal itself |
| `sdk/rocketnet_xdp_replay_marks.h:98` | A | charged after the capacity check and the key/value write |
| `tests/counters_expect.h:94` | D | local expected-delta table; bad index refused before it |
| `tests/dst_checks.h:254` | D | advances only past a delivered sequence |
| `tests/dst_checks.h:291` | D | only called after message send returned a serial (dst_harness.h:784) |
| `tests/dst_checks.h:304` | D | only called after send success; drained in check_delivered line 640 |
| `tests/dst_checks.h:334` | D | charged after the in-order check passed |
| `tests/dst_checks.h:362` | D | charged after the once and was-sent checks passed |
| `tests/dst_checks.h:556` | D | charged after the expected-sequence check passed |
| `tests/dst_checks.h:640` | D | release of outstanding bytes, clamped at zero; matching add in check_sent |
| `tests/dst_checks.h:744` | D | charged after the admitted-twice check passed, bounded by capacity |
| `tests/dst_harness.h:98` | D | PRNG state step, not a counter |
| `tests/dst_harness.h:276` | D | charged after bounds checks; no failure after it |
| `tests/dst_harness.h:443` | D | after start_payload succeeded; issued_set marks only sealed ids, gaps harmless |
| `tests/dst_harness.h:472` | D | charged only when seal returned wire_bytes > 0 |
| `tests/dst_harness.h:664` | D | type rotation cursor; a skipped pick changes no check |
| `tests/dst_harness.h:700` | D | generation per connect attempt; failures end the run via dst_fail |
| `tests/dst_harness.h:768` | D | refunded on refused send; the two early returns after it cannot fire (size in [14,16384]) |
| `tests/dst_harness.h:795` | D | the refund of line 768 on a refused send |
| `tests/dst_harness.h:993` | D | charged after malloc succeeded; destroy frees and decrements |
| `tests/dst_harness.h:1021` | D | charged after malloc succeeded; destroy frees and decrements |
| `tests/dst_harness.h:1059` | D | decrement on free; matching add in world_create |
| `tests/dst_harness.h:1067` | D | decrement on free; matching add in world_create |
| `tests/fake_client_driver.h:69` | D | counts calls, no failure path |
| `tests/fake_client_driver.h:76` | D | counts calls by design; overflow asserts |
| `tests/fake_client_driver.h:78` | D | charged after the capacity assert; no failure after |
| `tests/fake_client_driver.h:89` | D | counts calls by design, including empty polls |
| `tests/fake_client_driver.h:94` | D | pop after the empty check; matching add in push |
| `tests/fake_client_driver.h:105` | D | counts calls by design |
| `tests/fake_client_driver.h:109` | D | charged after the capacity assert; no failure after |
| `tests/fake_client_driver.h:121` | D | counts calls by design |
| `tests/fake_client_driver.h:127` | D | pop guarded by inbound_count > 0; matching add in push |
| `tests/fake_client_driver.h:143` | D | counts calls by design |
| `tests/fake_client_driver.h:155` | D | counts calls by design |
| `tests/fake_client_driver.h:165` | D | counts connect calls including refused ones, by design |
| `tests/fake_client_driver.h:210` | D | charged after the copy; capacity asserted first |
| `tests/rocketnet_value_tracker.h:80` | D | sample count after the value was stored; no failure path |
| `tests/serialize.h:482` | D | arithmetic on a local result, not a counter |
| `tests/test_unit_registry.h:193` | G | local loop bound |

## Already filed

- `sdk/rocketnet_packet_lease.h:40` — `used` incremented before the insert can fail: `docs/audits/2026-10-24-code-quality-aurora.md`, finding 6.
- `sdk/rocketnet_client_slots.cpp:308` and `:310` — the slot is pushed to the free stack and `num_connected` decremented even when the chain search failed: `docs/audits/2026-10-24-code-quality-johnny.md`, finding 3.

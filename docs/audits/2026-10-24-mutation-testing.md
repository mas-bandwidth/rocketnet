# Mutation testing of the core protocol, 2026-10-24

Mutation campaign over `mas-bandwidth/rocketnet` at `7b955800f1770b67030bb4f898c08f1d80df334d`
(`main`). Scope: `sdk/rocketnet_ack.cpp`, `sdk/rocketnet_fragment.cpp`, `sdk/rocketnet_protocol.cpp`,
`sdk/rocketnet_window_ack.cpp`, `sdk/rocketnet_window_receive.cpp`, `sdk/rocketnet_window_send.cpp`,
`sdk/rocketnet_packet_loss_tracker.h` and `sdk/rocketnet_jitter_tracker.h`, each survivor read
against its source and the unit tests under `tests/`. Method: `tools/mutation/run_mutants.py` with
`--max-mutants 500 --seed 20261024 --workers 8 --time-budget-minutes 75 --build-timeout 3600`
on a 16-core Linux machine, Release build (`-DCMAKE_BUILD_TYPE=Release`), `./bin/test` as the oracle.
The tool generated 3290 mutants and sampled 500; all 500 ran (wall 2681.9 s, unmutated
`./bin/test` 123.8 s, timeout 371.3 s). Re-run with the same command at the same sha to repeat
the sample. No code was changed. Findings are ordered most severe first; each gives severity
(`bug` / `risk` / `quality`), the mutated `file:line` at 7b955800, the behaviour the unit tests
do not pin, the regression that would ship unseen, and the unit test that would catch it.

| File | Mutants | Detected | Survived | Timed out | Did not compile | Score |
|---|---|---|---|---|---|---|
| `sdk/rocketnet_ack.cpp` | 211 | 158 | 42 | 0 | 11 | 0.790 |
| `sdk/rocketnet_fragment.cpp` | 79 | 47 | 29 | 0 | 3 | 0.618 |
| `sdk/rocketnet_protocol.cpp` | 47 | 37 | 8 | 0 | 2 | 0.822 |
| `sdk/rocketnet_window_ack.cpp` | 110 | 74 | 34 | 0 | 2 | 0.685 |
| `sdk/rocketnet_window_receive.cpp` | 16 | 10 | 6 | 0 | 0 | 0.625 |
| `sdk/rocketnet_window_send.cpp` | 32 | 15 | 16 | 0 | 1 | 0.484 |
| `sdk/rocketnet_packet_loss_tracker.h` | 3 | 3 | 0 | 0 | 0 | 1.000 |
| `sdk/rocketnet_jitter_tracker.h` | 2 | 1 | 1 | 0 | 0 | 0.500 |
| total | 500 | 345 | 136 | 0 | 19 | 0.717 |

Score is (detected + timed out) / (detected + timed out + survived). Of the 136 survivors, 46
mutate a `rocketnet_assert` or a static assertion, 39 are equivalent, 7 sit in code already
filed, and 44 are real survivors filed below as 39 findings (survivors on one line are one
finding).

## Findings

### 1. risk — `sdk/rocketnet_fragment.cpp:352`

- Defect: The suite passes with the staging buffer placed `max_payload_bytes` before the slot's payload instead of after it, so no unit test checks a final fragment that arrives before the stride is known.
- Failure or cost: A change that misplaces the staging buffer would write a final-first fragment into the neighbouring slot, or before the arena, and corrupt another payload with no test going red.
- Smallest fix: `tests/test_unit_standard_fragmentation.cpp`, new case `final_fragment_first_then_rest`: two connections, eight slots; send fragment `count-1` (the final) of payload id 5 on connection 1 first, then fragments `0..count-2`; expect one completed payload equal byte for byte to the original, and connection 0's slot for the same index untouched (a guard pattern written before is still there).

### 2. quality — `sdk/rocketnet_ack.cpp:410`

- Defect: Deleting `sequence++` in `rocketnet_ack_stats_lowest_acked` survives, so no unit test moves the acked low bound past a record that is present but not yet acked.
- Failure or cost: A regression in the scan step (a stuck or skipping cursor) would hang or mis-set `stats_acked_low` the first time the window drops its lowest acked record over an unacked one.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `acked_low_skips_unacked`: send sequences 1..N past the window base, ack only every second one, advance until the lowest acked leaves; expect `stats_acked_low` equal to the next acked sequence and `bandwidth_acked_kbps` matching the reference recompute.

### 3. quality — `sdk/rocketnet_ack.cpp:582`

- Defect: Deleting the `stats_received_bytes = 0` reset survives, so no unit test reads received bandwidth after the received window empties and refills.
- Failure or cost: The last record's bytes would stay in the running sum (unlink of the final record does not subtract them), inflating `bandwidth_received_kbps` after any idle gap.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `received_bytes_reset_when_window_empties`: receive one packet at time 1000.0, advance `highest_received` a full window, then receive two 100-byte packets 1.0 s apart; expect `bandwidth_received_kbps` equal to the reference recompute (`rocketnet_ack_endpoint_update_stats`).

### 4. quality — `sdk/rocketnet_ack.cpp:611`

- Defect: `after >= 0` mutated to `after > 0` survives, so no unit test links an out-of-order receive record when the tail of the receive-time list is ring slot 0.
- Failure or cost: A regression here would leave the receive-time list unsorted, so `stats_received_low_time` and the received bandwidth go wrong after reordering.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `received_link_older_than_slot_zero`: receive the sequence that maps to slot 0 at time 1002.0, then an older-stamped record entering at the top edge at 1001.0; expect the low time 1001.0 and received bandwidth equal to the reference recompute.

### 5. quality — `sdk/rocketnet_ack.cpp:616`

- Defect: Deleting the `stats_next = stats_received_head` link when a record becomes the new head survives, so no unit test inserts a record older than every counted one.
- Failure or cost: The new head would point at a stale successor and later unlinks would walk a broken list, so received statistics go wrong after reordering.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `received_link_new_head`: receive sequences at 1002.0 and 1003.0, then a record entering at the top edge stamped 1001.0; advance the window past all three one at a time and expect `stats_received_count` 2, 1, 0 and bandwidth equal to the reference at each step.

### 6. quality — `sdk/rocketnet_ack.cpp:690`

- Defect: The unlink loop bound `stats_received_count > 0` mutated to `> 1` survives, so no unit test advances the received window past its last counted record.
- Failure or cost: A regression would keep one record below the base counted forever, skewing received bandwidth.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `received_window_drops_last_record`: receive one packet, advance `highest_received` by half the ring plus one; expect `stats_received_count == 0` and `bandwidth_received_kbps == 0`.

### 7. quality — `sdk/rocketnet_ack.cpp:724`

- Defect: `sample > 0.0f` mutated to `>= 0.0f` survives, so no unit test writes a zero-millisecond RTT sample and checks it is not counted.
- Failure or cost: A regression would count zero samples in `stats_rtt_sum` and `stats_rtt_count`, pulling the published RTT and jitter down.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `zero_rtt_sample_not_counted`: send at 1000.0 and ack at 1000.0, then send at 1001.0 and ack at 1001.1; expect `rtt` 100 ms and `stats_rtt_count == 1`.

### 8. quality — `sdk/rocketnet_ack.cpp:1146`

- Defect: `sequence <= eligible_top` mutated to `<` survives, so no unit test acks the sequence exactly at the loss-eligible top.
- Failure or cost: An off-by-one here would leave `stats_eligible_unacked` one high, so the published packet loss reads a lost packet that was acked.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `ack_at_eligible_top`: send `ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 + 1` packets, ack only sequence 1 (the eligible top); expect `packet_loss` 0 and the reference recompute equal.

### 9. quality — `sdk/rocketnet_ack.cpp:1173`

- Defect: `sample >= 0.0` mutated to `> 0.0` survives, so no unit test acks a packet in the same update it was sent.
- Failure or cost: A regression would drop zero-elapsed acks from `has_rtt_sample`, so an endpoint on a zero-latency path (loopback, a stepped test clock) never reports an RTT sample.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `same_time_ack_sets_has_rtt_sample`: send sequence 1 at 1000.0, process a block acking it at 1000.0; expect `has_rtt_sample == 1`.

### 10. quality — `sdk/rocketnet_ack.cpp:1276`

- Defect: The range loop's `i++` mutated to `i--` survives, so no unit test checks the outcome of `rocketnet_ack_endpoint_process_block` applying a block's second and later ranges.
- Failure or cost: A regression in the range walk (wrong order, wrong bounds) would lose or invent acks for every packet below the first gap, and the suite would stay green.
- Smallest fix: `tests/test_unit_standard_acks.cpp`, new case `process_block_applies_every_range`: send 1..20, build a block with largest 20, first run 5, then (gap 2, run 3) and (gap 1, run 4); process it at 1001.0 and expect exactly sequences 16–20, 11–13 and 6–9 acked.

### 11. quality — `sdk/rocketnet_ack.cpp:1458`

- Defect: Deleting `finish = record->receive_time` for the first counted record in the reference recompute survives, so no unit test runs the reference over a window whose first record carries the latest receive time.
- Failure or cost: The reference recompute is the oracle other stats tests compare against; a regression here would silently change what "correct" means for received bandwidth after reordering.
- Smallest fix: `tests/test_unit_ack_stats.cpp`, new case `reference_received_span_first_record_latest`: receive sequence 1 at 1003.0 and sequence 2 at 1001.0 (100 bytes each); expect `rocketnet_ack_stats_history_compute` to report received kbps over a 2.0 s span.

### 12. quality — `sdk/rocketnet_fragment.cpp:176`

- Defect: `max_payload_bytes < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` mutated to `<=` survives, so no unit test sizes the server delivered arena at exactly the packet payload limit.
- Failure or cost: An off-by-one would return 0 bytes for the smallest allowed configuration and the server would size no delivered arena.
- Smallest fix: `tests/test_unit_standard_fragmentation.cpp`, new case `delivered_bytes_at_packet_payload_limit`: expect `rocketnet_server_reassembly_delivered_bytes( 1, 8, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) > 0`.

### 13. quality — `sdk/rocketnet_fragment.cpp:317`

- Defect: Deleting the `completed_payload = NULL` reset survives, so no unit test reads `completed_payload` after a refused fragment that follows a completion.
- Failure or cost: A regression would leave a pointer to the previous payload visible on a refusal, which a caller that checks the pointer instead of the return would deliver twice.
- Smallest fix: `tests/test_unit_standard_fragmentation.cpp`, new case `refusal_clears_completed_payload`: complete payload id 1, then process a one-byte fragment; expect return 0, `completed_payload == NULL` and `completed_bytes == 0`.

### 14. quality — `sdk/rocketnet_fragment.cpp:387`

- Defect: The stale test's `payload_id - reference > ROCKETNET_REASSEMBLY_REACH` mutated to `>=` survives, so no unit test sends a payload id exactly `ROCKETNET_REASSEMBLY_REACH` from the newest completed id.
- Failure or cost: An off-by-one at the reach would refuse (or admit) payloads one id early, unseen.
- Smallest fix: `tests/test_unit_reassembly_stale.cpp`, new case `stale_at_exact_reach`: complete id 1, then send ids `1 + ROCKETNET_REASSEMBLY_REACH` and `1 - ROCKETNET_REASSEMBLY_REACH` (wrapping); expect both admitted, and `1 + ROCKETNET_REASSEMBLY_REACH + 1` refused with `stale` counted only if the reverse distance also exceeds the reach.

### 15. quality — `sdk/rocketnet_fragment.cpp:481`

- Defect: The final fragment's assumed one-byte contribution mutated to two survives, so no unit test checks the size cap when the final is still unknown and the payload is exactly at the configured maximum.
- Failure or cost: An off-by-one would refuse a maximum-sized payload whose non-final fragments arrive first.
- Smallest fix: `tests/test_unit_standard_fragmentation.cpp`, new case `max_payload_nonfinal_first`: configure `max_payload_bytes` to a payload whose final fragment is one byte, send all non-final fragments first, then the final; expect one completed payload of the full size.

### 16. quality — `sdk/rocketnet_fragment.cpp:586`

- Defect: `delivered_offset + payload_bytes <= delivered_bytes` mutated to `<` survives, so no unit test fills the delivered arena to its last byte.
- Failure or cost: An off-by-one would drop a payload that exactly fits the delivered arena.
- Smallest fix: `tests/test_unit_standard_fragmentation.cpp`, new case `delivered_arena_exact_fit`: size the set so `delivered_bytes` equals one aligned payload, complete one payload of that size; expect `num_delivered == 1`.

### 17. quality — `sdk/rocketnet_fragment.cpp:608`

- Defect: Both the `advance != 0` and the `advance < 0x80000000u` bounds survive mutation (to `!= 1` and `< 0x80000001u`), so no unit test checks that the stale reference advances by exactly one id, or holds at a half-range jump.
- Failure or cost: A regression would freeze `newest_completed` on consecutive ids or let it jump backwards across the half range, so the stale rule refuses live payloads or admits stale ones.
- Smallest fix: `tests/test_unit_reassembly_stale.cpp`, new case `newest_completed_advances_by_one`: complete ids 1 then 2 and expect `newest_completed == 2`; then complete id `2 + 0x80000000u` and expect it unchanged at 2.

### 18. quality — `sdk/rocketnet_fragment.cpp:623`

- Defect: All three mutants of `if (slots < 1 || bytes < 1) return 0;` survive, so no unit test calls `rocketnet_send_arena_allocation_bytes` with one slot or with zero slots or bytes.
- Failure or cost: A regression would size a one-slot send arena as zero, or report a non-zero size for an empty one.
- Smallest fix: `tests/test_unit_standard_fragmentation.cpp`, new case `send_arena_allocation_bounds`: expect `( 0, 16 )` and `( 1, 0 )` to return 0, and `( 1, 16 )` to return more than 16.

### 19. quality — `sdk/rocketnet_fragment.cpp:673`

- Defect: `p < data + size` mutated to `<=` survives, so no unit test asks `rocketnet_send_arena_owns` about the one-past-end pointer.
- Failure or cost: A regression would claim the byte past the arena as owned and let a release path free a foreign pointer into the arena's bookkeeping.
- Smallest fix: `tests/test_unit_standard_fragmentation.cpp`, new case `send_arena_owns_bounds`: expect `owns( data )` 1, `owns( data + size - 1 )` 1, `owns( data + size )` 0.

### 20. quality — `sdk/rocketnet_protocol.cpp:215`

- Defect: `rocketnet_crypto_tally.aead_seal++` mutated to `--` survives, so no unit test reads the seal tally.
- Failure or cost: The tally that budgets AEAD work per packet could count backwards and no test would notice.
- Smallest fix: `tests/test_unit_standard_packet_protection.cpp`, new case `seal_tally_counts_one_per_packet`: read the tally, write one sealed payload packet, expect `aead_seal` up by exactly 1.

### 21. quality — `sdk/rocketnet_protocol.cpp:253`

- Defect: The `return 0` for an ephemeral key passed (or missing) against the packet type mutated to `return 1` survives, so no unit test calls `rocketnet_write_sealed_packet` with a mismatched ephemeral argument.
- Failure or cost: A regression would report a one-byte packet written for a mis-called seal, and the caller would send it.
- Smallest fix: `tests/test_unit_standard_packet_protection.cpp`, new case `seal_refuses_mismatched_ephemeral`: call with a type that carries no ephemeral key and a non-NULL key, and the reverse; expect 0 both times.

### 22. quality — `sdk/rocketnet_window_ack.cpp:171`

- Defect: `packet_id == 0 || sent_index_entries == 0` mutated to `&&` survives, so no unit test looks up a packet in a region with no sent index.
- Failure or cost: A regression would take `% 0` in `rn_sent_index_hash` for an index-less region.
- Smallest fix: `tests/test_unit_standard_reliable_window.cpp`, new case `sent_index_lookup_empty_index`: a region with `sent_index_entries == 0`; expect the ack of any packet id to resolve nothing and count it unknown.

### 23. quality — `sdk/rocketnet_window_ack.cpp:176`

- Defect: The probe loop starting at `step = 1` survives, so no unit test finds a packet that sits on the last probe position of a full sent index.
- Failure or cost: A regression would miss the last entry of a full index, and that packet's ack would read as unknown and the message resend.
- Smallest fix: `tests/test_unit_standard_reliable_window.cpp`, new case `sent_index_full_table_last_probe`: fill a four-entry index with ids that hash to the same slot, ack the last inserted; expect it found and released.

### 24. quality — `sdk/rocketnet_window_ack.cpp:254`

- Defect: `ack->ref_head += ...` in the head reclaim mutated to `-=` survives, so no unit test reclaims an acked head entry that holds references and then checks the reference ring.
- Failure or cost: A regression would corrupt the reference ring's free space and stall or overwrite live references after the first reclaim.
- Smallest fix: `tests/test_unit_standard_reliable_window.cpp`, new case `reclaim_head_returns_references`: send one packet with two messages, ack it, send again; expect `ref_tail - ref_head` equal to the new packet's two references.

### 25. quality — `sdk/rocketnet_window_ack.cpp:368`

- Defect: Deleting the `gap_min[c] = 1.0e30` initialisation survives, so no unit test reads the resend-gap diagnostics.
- Failure or cost: `gap_min` would stay 0 and the development diagnostics would report a zero minimum resend gap.
- Smallest fix: `tests/test_unit_standard_reliable_window.cpp`, new case `resend_gaps_min_max`: in a development build, resend one message across three updates at 1000.0, 1000.1, 1000.3; expect `gap_min[0] == 0.1` and `gap_max[0] == 0.2`.

### 26. quality — `sdk/rocketnet_window_ack.cpp:382`

- Defect: Negating `now == gaps->update_now` survives, so no unit test calls `rn_window_ack_gaps_update` twice for the same time.
- Failure or cost: A regression would shift `prev_now` on a repeated update and the next gap measure would compare against the wrong delay.
- Smallest fix: covered by the case in finding 25, with a repeated update at 1000.1 that must leave `gap_min[0]` at 0.1.

### 27. quality — `sdk/rocketnet_window_ack.cpp:398`

- Defect: `gaps->update_now != now` mutated to `==` survives, so no unit test measures a resend gap.
- Failure or cost: A regression would measure every resend outside an update, or none inside one, and the diagnostics would be empty.
- Smallest fix: covered by the case in finding 25 (`count[0] == 2` after the two resends).

### 28. quality — `sdk/rocketnet_window_ack.cpp:400`

- Defect: The chunk class `? 1 : 0` mutated to `? 0 : 0` survives, so no unit test measures a resend gap for a chunk-backed slot.
- Failure or cost: A regression would file chunk resends under the message class.
- Smallest fix: extend the case in finding 25 with a fragmented message; expect `count[1] > 0`.

### 29. quality — `sdk/rocketnet_window_ack.cpp:402`

- Defect: Both mutants of `c == 1` (to `c == 2` and `c == 0`) survive, so no unit test checks the early-resend measure against the class delay.
- Failure or cost: A regression would measure early resends against the other class's delay.
- Smallest fix: extend the case in finding 25 with an expected `early_min[0]` equal to the gap minus the message class delay.

### 30. quality — `sdk/rocketnet_window_ack.cpp:408`

- Defect: `late > late_max` mutated to `<` survives, so no unit test reads `late_max`.
- Failure or cost: A regression would record the least late resend instead of the latest.
- Smallest fix: extend the case in finding 25 with an expected `late_max[0]`.

### 31. quality — `sdk/rocketnet_window_ack.cpp:511`

- Defect: Deleting `oldest->packet_id = 0` when the sent map evicts its oldest entry survives, so no unit test evicts from a full sent map and then sweeps receipts.
- Failure or cost: A regression would leave the evicted entry live to `rn_window_ack_receipt_sweep`, which would resolve its receipts as LOSS a second time.
- Smallest fix: `tests/test_unit_channel_receipts.cpp`, new case `evicted_entry_reports_loss_once`: a region of four packets, send five unreliable messages with receipts, sweep past the receipt deadline; expect exactly one LOSS receipt per evicted message.

### 32. quality — `sdk/rocketnet_window_ack.cpp:514`

- Defect: `ack->ref_head += ...` in the eviction loop mutated to `-=` survives, so no unit test checks the reference ring after an eviction.
- Failure or cost: A regression would corrupt the reference ring on the first eviction.
- Smallest fix: extend the case in finding 31: after the eviction, expect `ref_tail - ref_head` equal to the references of the four live packets.

### 33. quality — `sdk/rocketnet_window_ack.cpp:518`

- Defect: Deleting `ack->pkt_head++` in the eviction loop survives, which means the loop body never runs in the unit suite (the loop would not end).
- Failure or cost: Every eviction path (`rn_window_ack_on_send` with a full map) ships untested.
- Smallest fix: the case in finding 31 runs this loop; expect `pkt_tail - pkt_head == P` after the fifth send.

### 34. quality — `sdk/rocketnet_window_ack.cpp:538`

- Defect: `receipt_first = ( ref_first + count ) % max_refs` mutated to `ref_first - count` survives, so no unit test sends a packet carrying both reliable references and unreliable receipts and then resolves its receipts.
- Failure or cost: A regression would resolve the wrong serials (reliable message numbers read as receipt serials) for mixed packets.
- Smallest fix: `tests/test_unit_channel_receipts.cpp`, new case `mixed_packet_receipt_serials`: one packet with two reliable messages and one unreliable message with a receipt, ack it; expect one ACKED receipt with the unreliable message's serial.

### 35. quality — `sdk/rocketnet_window_ack.cpp:539`

- Defect: Deleting `entry->flags = 0` when a sent entry is written survives, so no unit test reuses a sent-map entry whose previous occupant set a flag.
- Failure or cost: A regression would carry a stale flag into the next packet in that entry.
- Smallest fix: `tests/test_unit_standard_reliable_window.cpp`, new case `sent_entry_flags_reset_on_reuse`: wrap the sent map once with a flagged packet in entry 0; expect the reused entry's flags 0.

### 36. quality — `sdk/rocketnet_window_ack.cpp:693`

- Defect: `receipt_sweep < pkt_head` mutated to `>` survives, so no unit test sweeps after the head has passed the sweep cursor.
- Failure or cost: A regression would re-sweep resolved entries or skip the clamp, giving duplicate or missing LOSS receipts after a reclaim.
- Smallest fix: `tests/test_unit_channel_receipts.cpp`, new case `sweep_after_head_reclaim`: send three receipted messages, ack the first two, sweep past the deadline; expect exactly one LOSS receipt.

### 37. quality — `sdk/rocketnet_window_receive.cpp:82`

- Defect: `descriptor->block_have = 0` mutated to `1` survives, so no unit test reads a receive descriptor's block state after it is reused.
- Failure or cost: A reused descriptor would start with a block it never received, unless `sdk/rocketnet_channel.cpp:169` happens to reset it first; nothing pins which store matters.
- Smallest fix: `tests/test_unit_standard_reliable_window.cpp`, new case `receive_descriptor_reset_on_reuse`: wrap the receive window once with a fragmented message, then accept a plain message into the same descriptor; expect `block_have == 0` and `block_count == 0`.

### 38. quality — `sdk/rocketnet_window_receive.cpp:83`

- Defect: Deleting `descriptor->block_count = 0` survives, for the same reason as finding 37.
- Failure or cost: A reused descriptor could keep the previous message's block count.
- Smallest fix: covered by the case in finding 37.

### 39. quality — `sdk/rocketnet_window_send.cpp:185`

- Defect: Both `send->U_prime != old_prime` mutants (to `==` and negated) survive, so no unit test checks that a stalled send window un-stalls when its oldest number is released.
- Failure or cost: A regression would leave `stalled` set after the window advances, and a reliable channel would stop sending.
- Smallest fix: `tests/test_unit_window_sequences.cpp`, new case `release_oldest_clears_stall`: fill the window to stall, release the oldest number; expect `stalled == 0`; release a non-oldest number from a stalled window and expect `stalled == 1`.

## Judged not a defect

| Survivors | Mutants | Reason |
|---|---|---|
| `rocketnet_assert` lines: `rocketnet_ack.cpp:32` (3), `:33`, `:68`, `:642`, `:924`, `:1564`; `rocketnet_fragment.cpp:25`, `:69`, `:70` (3), `:71` (3), `:195` (3), `:629`; `rocketnet_protocol.cpp:370`, `:389`; `rocketnet_window_ack.cpp:83` (2), `:193` (2), `:472`, `:482`; `rocketnet_window_receive.cpp:25` (2), `:36`, `:109`; `rocketnet_window_send.cpp:32` (2), `:65` (4), `:83`, `:84`, `:85`, `:105`, `:136` (2), `:182` | 45 | The oracle is a Release build: `NDEBUG` sets `ROCKETNET_ASSERTS` to 0 (`sdk/rocketnet_internal.h:285`) and `rocketnet_assert` expands to nothing. |
| `rocketnet_fragment.cpp:17` | 1 | Static assertion; the mutated condition still holds at compile time. |
| `rocketnet_ack.cpp:81`, `:349`, `:530`, `:557`, `:796`, `:1067`, `:1394`, `:1463`; `rocketnet_fragment.cpp:161`; `rocketnet_protocol.cpp:202`, `:407`, `:408`; `rocketnet_window_ack.cpp:405`, `:406`, `:714`, `:715` | 16 | Comparison widened or narrowed at a point where both branches give the same value (equal operands give the same result, a zero shift, or a no-op move). |
| `rocketnet_ack.cpp:1405`; `rocketnet_fragment.cpp:558` | 2 | A truth flag set to 2 instead of 1; read only as true or false. |
| `rocketnet_ack.cpp:466`, `:470`, `:543`, `:583` | 4 | A cleared bound or time is rewritten before it is next read: sequences only grow, so the next ack or link sets it anew, and the readers are guarded by a zero count. |
| `rocketnet_ack.cpp:142` | 1 | `next_high` is 1 only after a run stopped on a clear bit (the 0xFFFF run cap is above the window), so the next gap reaches the floor and breaks anyway. |
| `rocketnet_ack.cpp:702` | 1 | Prefetch hint; the index stays inside the ring and the hint has no effect on results. |
| `rocketnet_ack.cpp:765` | 1 | With one acked record the span is zero, so the rate is 0 either way. |
| `rocketnet_ack.cpp:959` (2) | 2 | MSVC-only branch; not compiled by the Linux oracle. |
| `rocketnet_ack.cpp:1247` | 1 | Re-checks one already reconciled sequence; the reconcile is idempotent. |
| `rocketnet_fragment.cpp:64` | 1 | Dead guard: `ROCKETNET_MAX_FRAGMENT_BYTES` (1413) is below `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` (1416), so any payload that reaches the line needs at least two fragments. |
| `rocketnet_fragment.cpp:479` | 1 | With an unknown stride (0) the check computes a total no larger than the final's minimum, which always passes; no stride is 1. |
| `rocketnet_protocol.cpp:334` | 1 | `opened_bytes` is written by the decrypt call before it is read. |
| `rocketnet_window_ack.cpp:165` | 1 | A different hash mix; linear probing finds the same entries. |
| `rocketnet_window_ack.cpp:176` (`<=`), `:196` | 2 | One extra probe wraps to the start slot already checked. |
| `rocketnet_window_ack.cpp:227`, `:228` | 2 | The emptied index slot has `used = 0`, which every reader checks first. |
| `rocketnet_window_ack.cpp:626` | 1 | A one-fragment descriptor's chain loop runs zero times, so the chain index is 0 either way. |
| `rocketnet_window_ack.cpp:672` | 1 | After the clamp above it, `receipt_head >= receipt_visible_tail`, so the visible range is empty either way. |
| `rocketnet_jitter_tracker.h:67` | 1 | Differs only when the jitter change is exactly 0.00001 ms; the two branches then differ by less than that. |

## Counts

- bug: 0
- risk: 1 (finding 1)
- quality: 38 (findings 2–39)
- total: 39

## Already filed

- `sdk/rocketnet_ack.cpp:265`, `:272`, `:273`, `:274`, `:284` (5 survivors in the record equality helpers): `docs/audits/2026-10-24-coverage-production-paths.md` finding 4.
- `sdk/rocketnet_window_ack.cpp:838` (1 survivor in the test-only resend walk): `docs/audits/2026-10-24-area-protocol-acks-fragments.md` finding 3.
- `sdk/rocketnet_window_send.cpp:197` (1 survivor in `rn_window_send_can_write_unreliable`): `docs/audits/2026-10-24-code-quality-aurora.md` finding 12.

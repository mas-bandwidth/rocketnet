# Mutation testing of the core protocol, 2026-10-24

Mutation campaign over `mas-bandwidth/rocketnet` at
`44ab971773c8a05dd00f63e77b7159d6b489a76c` (`main`). Scope:
`sdk/rocketnet_ack.cpp`, `sdk/rocketnet_fragment.cpp`, `sdk/rocketnet_protocol.cpp`,
`sdk/rocketnet_window_ack.cpp`, `sdk/rocketnet_window_receive.cpp`,
`sdk/rocketnet_window_send.cpp`, `sdk/rocketnet_packet_loss_tracker.h` and
`sdk/rocketnet_jitter_tracker.h`, with every survivor read against the source and
the unit tests under `tests/`.

Method: `tools/mutation/run_mutants.py`, Release build
(`-DCMAKE_BUILD_TYPE=Release`), `./bin/test` as the oracle, seed `20261024`, four
workers per partition, a 75-minute time budget and a 3600-second build timeout.
The changed files ran as two disjoint partitions: ack plus jitter sampled 210 of
1379 generated mutants (wall 2431.4 s, baseline 88.2 s, mutant timeout 264.7 s),
and fragment, protocol, window ack and window send sampled 268 of 1753 (wall
3187.4 s, baseline 83.6 s, mutant timeout 250.8 s). The source-identical window
receive and packet-loss files retain 19 results from the same fixed-seed Release
campaign; their Git blobs are unchanged. The current sources generate 3276
mutants in all. All 497 selected mutants ran and none timed out. No code was
changed.

| File | Mutants | Detected | Survived | Timed out | Did not compile | Score |
|---|---|---|---|---|---|---|
| `sdk/rocketnet_ack.cpp` | 204 | 149 | 44 | 0 | 11 | 0.772 |
| `sdk/rocketnet_fragment.cpp` | 93 | 63 | 26 | 0 | 4 | 0.708 |
| `sdk/rocketnet_protocol.cpp` | 46 | 36 | 8 | 0 | 2 | 0.818 |
| `sdk/rocketnet_window_ack.cpp` | 98 | 60 | 32 | 0 | 6 | 0.652 |
| `sdk/rocketnet_window_receive.cpp` | 16 | 10 | 6 | 0 | 0 | 0.625 |
| `sdk/rocketnet_window_send.cpp` | 31 | 18 | 10 | 0 | 3 | 0.643 |
| `sdk/rocketnet_packet_loss_tracker.h` | 3 | 3 | 0 | 0 | 0 | 1.000 |
| `sdk/rocketnet_jitter_tracker.h` | 6 | 6 | 0 | 0 | 0 | 1.000 |
| total | 497 | 345 | 126 | 0 | 26 | 0.732 |

Score is (detected + timed out) / (detected + timed out + survived). Of the 126
survivors, 38 mutate a Release-disabled `rocketnet_assert`, one mutates a
still-true static assertion, 50 are equivalent for the reasons below, one sits
in code whose missing coverage is already filed, and 36 are real survivors
grouped into the 25 findings below. Findings are ordered by severity.

## Findings

### 1. risk — `sdk/rocketnet_ack.cpp:817`

- Defect: The RFC 9002 loss threshold comparison survives changing from `<=` to `<`.
- Failure or cost: A packet exactly `ROCKETNET_LOSS_PACKET_THRESHOLD` below the largest acknowledged sequence would be reported as an unknown eviction instead of known loss.
- Smallest fix: Record a packet, advance `largest_acked_sequence` by exactly the threshold, evict it, and expect `ROCKETNET_ACK_SENT_EVICTED_LOST`; add the one-below case expecting unknown.

### 2. risk — `sdk/rocketnet_fragment.cpp:626`, `sdk/rocketnet_fragment.cpp:627`

- Defect: Computing serial advance with addition, and admitting the exact half-range jump, both survive.
- Failure or cost: The stale reference could jump backwards across the serial half range, causing live payloads to be refused as stale or old payloads to be admitted.
- Smallest fix: Complete ids 1 then 2 and expect `newest_completed == 2`; complete ids at `2 + 0x7fffffff`, `2 + 0x80000000` and `2 + 0x80000001`, checking the reference advances only on the serially newer side.

### 3. risk — `sdk/rocketnet_window_ack.cpp:48`

- Defect: The descriptor upper bound survives changing from `>= region->queue` to `> region->queue`.
- Failure or cost: A slot with descriptor exactly equal to the queue length would index one element past `send_descriptors` during resend FIFO maintenance.
- Smallest fix: Exercise unlink with descriptors `queue - 1`, `queue` and `RN_DESCRIPTOR_NONE`; only the first may be dereferenced.

### 4. risk — `sdk/rocketnet_window_ack.cpp:297`

- Defect: Clearing `has_oldest` when a live oldest send is found survives.
- Failure or cost: The reliable timeout in `rn_window_ack_check_timeout` would remain disabled for an outstanding oldest message, contrary to the timeout rule documented above the function.
- Smallest fix: Refresh an already-sent oldest number, advance past `reliable_timeout`, and expect the reliable-timeout disconnect reason.

### 5. risk — `sdk/rocketnet_window_ack.cpp:629`, `sdk/rocketnet_window_ack.cpp:631`, `sdk/rocketnet_window_ack.cpp:638`

- Defect: Disabling the predecessor walk, walking one entry too far, and counting a different descriptor all survive in the legacy fragmented-message acknowledgement path.
- Failure or cost: `chain_index` could name the wrong fragment, so a distinct transport ACK could release the wrong chunk or fail to complete a fully acknowledged block.
- Smallest fix: Ack a three-fragment descriptor out of order with an unrelated descriptor interleaved in the ring; after each ACK, check the exact chunk released and complete only on the third distinct ACK.

### 6. quality — `sdk/rocketnet_protocol.cpp:57`

- Defect: Shrinking the SipHash output array from eight bytes to seven survives even though `crypto_shorthash_siphash24` writes eight bytes.
- Failure or cost: A future non-test caller, or the current helper under a memory checker, could write one byte past the stack object without the ordinary suite reporting it.
- Smallest fix: Call `rocketnet_keyed_filter_value` in an AddressSanitizer job and compare all four filter bytes with a direct eight-byte SipHash reference.

### 7. quality — `sdk/rocketnet_ack.cpp:661`

- Defect: Omitting the update of `stats_received_high_time` after unlinking the receive-time tail survives.
- Failure or cost: Received bandwidth would retain the removed newest timestamp and use an overstated time span after window eviction.
- Smallest fix: Receive three records with distinct times, advance out the newest-time tail, and compare the running received rate and high time with the reference recompute.

### 8. quality — `sdk/rocketnet_ack.cpp:722`

- Defect: Treating an old zero RTT sample as counted by changing `> 0.0f` to `>= 0.0f` survives.
- Failure or cost: Overwriting a stored zero sample would decrement `stats_rtt_count` even though zero was never added, corrupting the published RTT and jitter aggregates.
- Smallest fix: Write a zero sample into one history slot, overwrite that slot with a positive sample, and expect count 1 and the positive sample as the minimum.

### 9. quality — `sdk/rocketnet_ack.cpp:1150`

- Defect: The loss-eligible upper bound survives changing from `<=` to `<`.
- Failure or cost: Acknowledging the packet exactly at the eligible top would leave `stats_eligible_unacked` one high and overstate packet loss.
- Smallest fix: Send half a retained window plus one packet, ACK only the eligible-top sequence, and compare running packet loss with the reference recompute.

### 10. quality — `sdk/rocketnet_ack.cpp:1255`

- Defect: Reconciliation survives skipping `highest_sequence` by changing the loop bound from `<=` to `<`.
- Failure or cost: The newest slot's held and unacknowledged bits could remain inconsistent with its ring record until another sequence arrives.
- Smallest fix: Corrupt only the newest sequence's bitmap bits, process an empty ACK block, and expect both bitmaps restored to the ring state.

### 11. quality — `sdk/rocketnet_ack.cpp:1462`

- Defect: The reference recompute survives omitting `finish = record->receive_time` for its first received record.
- Failure or cost: The reference used by other statistics tests could silently define a wrong received-bandwidth span after reordered receive times.
- Smallest fix: Recompute a history whose first visited record has the latest receive time and compare with a direct min/max-time calculation.

### 12. quality — `sdk/rocketnet_ack.cpp:1510`

- Defect: The allocation-size helper survives accepting zero clients.
- Failure or cost: It would return a nonzero allocation size for a configuration that `rocketnet_server_acks_create` rejects, breaking the size/create contract.
- Smallest fix: Expect allocation bytes 0 for clients 0 and a nonzero value for clients 1.

### 13. quality — `sdk/rocketnet_fragment.cpp:142`

- Defect: The server delivered-capacity helper survives accepting zero slots.
- Failure or cost: It would report positive delivery capacity for a server that cannot retain any fragmented payload.
- Smallest fix: Expect capacity 0 for `(max_clients, 0)` and a positive capacity for `(max_clients, 1)`.

### 14. quality — `sdk/rocketnet_fragment.cpp:542`

- Defect: Replacing the unseen-final sentinel `-1` with zero survives.
- Failure or cost: Size validation for subsequent non-final fragments could treat an absent final fragment as zero bytes rather than the required minimum, admitting an over-limit layout one byte early.
- Smallest fix: Start a maximum-sized payload with non-final fragments while the final is absent, then test the one-byte boundary before and after the final arrives.

### 15. quality — `sdk/rocketnet_fragment.cpp:642`

- Defect: `rocketnet_send_arena_allocation_bytes` survives accepting zero bytes per slot.
- Failure or cost: The sizing helper would return a header allocation for a configuration that cannot store a payload.
- Smallest fix: Expect `(1, 0)` and `(0, 1)` to return 0, and `(1, 1)` to return a nonzero allocation.

### 16. quality — `sdk/rocketnet_fragment.cpp:692`

- Defect: The arena ownership upper bound survives changing from `< end` to `<= end`.
- Failure or cost: The one-past-end pointer would be claimed as owned and could be passed into arena release bookkeeping as a foreign payload.
- Smallest fix: Expect ownership for `data` and `data + size - 1`, and no ownership for `data + size`.

### 17. quality — `sdk/rocketnet_protocol.cpp:310`

- Defect: The ephemeral-key mismatch path survives returning zero instead of a negative error.
- Failure or cost: A caller could treat a mismatched packet-type/key argument as a successfully opened empty payload.
- Smallest fix: Open a type that excludes an ephemeral key with a non-null key, and a type that requires one with null; require a negative result both times.

### 18. quality — `sdk/rocketnet_window_ack.cpp:178`

- Defect: Starting the open-addressed lookup at probe 1 survives.
- Failure or cost: A packet in its home hash slot would be missed and its ACK treated as unknown.
- Smallest fix: Insert one packet into an otherwise empty sent index and ACK it; expect it found and released.

### 19. quality — `sdk/rocketnet_window_ack.cpp:256`, `sdk/rocketnet_window_ack.cpp:517`

- Defect: Omitting reference-head advancement during normal reclaim, and subtracting it during forced eviction, both survive.
- Failure or cost: The fixed reference ring would retain dead entries or move backwards, eventually stalling sends or overwriting live message references.
- Smallest fix: Cover both an acknowledged head reclaim and a full-map eviction, checking `ref_tail - ref_head` equals the references held by live packets after each step.

### 20. quality — `sdk/rocketnet_window_ack.cpp:356`

- Defect: Initializing `pkt_tail` to 1 survives.
- Failure or cost: A reset ACK map would begin with a phantom sent-packet entry and could reclaim or sweep state that was never sent.
- Smallest fix: After reset, require an empty packet/reference map and then send and ACK one packet, checking head and tail move from zero to one together.

### 21. quality — `sdk/rocketnet_window_ack.cpp:368`, `sdk/rocketnet_window_ack.cpp:370`, `sdk/rocketnet_window_ack.cpp:384`, `sdk/rocketnet_window_ack.cpp:386`, `sdk/rocketnet_window_ack.cpp:387`, `sdk/rocketnet_window_ack.cpp:404`

- Defect: Six mutants that skip one diagnostic class, erase the minimum sentinel, invert repeated-update handling, omit the previous/current times, or use the other class's delay all survive.
- Failure or cost: Development resend-gap diagnostics could report missing, zero, or misclassified early/late measurements while tests remain green.
- Smallest fix: In a development build, resend one message and one chunk across three updates including a repeated timestamp; assert each class's count, minimum gap, early minimum and late maximum.

### 22. quality — `sdk/rocketnet_window_ack.cpp:731`

- Defect: The batch-ACK guard survives changing `count == 0 || !packet_ids` to `&&`.
- Failure or cost: A null packet-id array with a positive count would be dereferenced.
- Smallest fix: Call with `(NULL, 1)` and expect zero matches without a crash; retain the empty non-null case.

### 23. quality — `sdk/rocketnet_window_receive.cpp:82`, `sdk/rocketnet_window_receive.cpp:83`

- Defect: A reused receive descriptor survives starting with `block_have = 1` or retaining its previous `block_count`.
- Failure or cost: A plain message could inherit fragment-block state from the prior occupant unless a separate caller happens to reset it first.
- Smallest fix: Wrap the receive window once with a fragmented message, then accept a plain message into the same descriptor and require both fields to be zero.

### 24. quality — `sdk/rocketnet_window_send.cpp:171`

- Defect: An invalid or duplicate ACK survives returning success instead of failure from `rn_window_send_mark_acked`.
- Failure or cost: Callers would count a packet outside the live window, or an already-acked packet, as a newly matched ACK.
- Smallest fix: ACK one live number twice and one number outside the window; require results 1, 0 and 0.

### 25. quality — `sdk/rocketnet_window_send.cpp:189`

- Defect: Negating the test that clears `stalled` when `U_prime` advances survives.
- Failure or cost: A reliable send window could remain stalled after its oldest number is released, or clear the stall when no capacity was freed.
- Smallest fix: Fill the window to stall and release the oldest number, expecting `stalled == 0`; release a non-oldest number and expect it to remain set.

## Judged not a defect

| Survivors | Mutants | Reason |
|---|---|---|
| `rocketnet_assert` lines: `rocketnet_ack.cpp:32` (2), `:33`, `:68`, `:104`, `:279`, `:905`, `:928`, `:1484`, `:1568`; `rocketnet_fragment.cpp:24`, `:25`, `:26`, `:69`, `:70` (2), `:197`, `:265`, `:319` (2), `:595` (3), `:648`; `rocketnet_window_ack.cpp:142`, `:427`; `rocketnet_window_receive.cpp:25` (2), `:36`, `:109`; `rocketnet_window_send.cpp:32`, `:69` (3), `:89`, `:109`, `:140`, `:199` | 38 | The oracle is a Release build: `NDEBUG` sets `ROCKETNET_ASSERTS` to 0 (`sdk/rocketnet_internal.h:285`) and `rocketnet_assert` expands to nothing. |
| `sdk/rocketnet_ack.cpp:305` | 1 | The mutated field-order comparison remains true inside the compile-time static assertion. |
| `sdk/rocketnet_ack.cpp:100`, `:115`, `:124` (3) | 5 | The floor checks make the two explicit breaks equivalent, and the replay window is far smaller than the mutated 16-bit run caps. |
| `sdk/rocketnet_ack.cpp:429`, `:615`, `:700`, `:765`, `:1071`, `:1083`, `:1084`, `:1325`, `:1392`, `:1413`; `sdk/rocketnet_fragment.cpp:161`, `:183`; `sdk/rocketnet_protocol.cpp:408`; `sdk/rocketnet_window_ack.cpp:409`, `:715`, `:718` | 16 | Equal operands select or assign the same value; truth value 2 is read only as true; the one-record bandwidth span is zero; and the initial sent-window floor is 1 under both boundary forms. |
| `sdk/rocketnet_protocol.cpp:407`, `:411` | 2 | The subtraction `below - expected` at :407 is the true arm of `below > expected`, and that arm is dead: `below` is `(base - 2^32) | wire`, so `below < base` and `base <= expected`. Deleting `best_distance = distance` at :411 does not change the returned value: once below is selected, expected is more than half a 32-bit range below the in-epoch candidate, so the above-wrap candidate is more than one full range away, or wraps to a tiny value at the top epoch, and loses to either stored distance. `docs/audits/2026-10-24-code-quality-a.md` finding 15 already calls this store dead. |
| `sdk/rocketnet_ack.cpp:468`, `:471`, `:587`, `:589`, `:591`; `sdk/rocketnet_window_ack.cpp:281`, `:341`, `:353`, `:356`, `:357`, `:364`, `:518`, `:682` | 13 | The deleted values are already zero, rewritten before a guarded read, or belong to an entry readers ignore after its live flag is cleared. |
| `sdk/rocketnet_ack.cpp:581`, `:705`, `:706` (2) | 4 | Prefetch targets do not affect program results. |
| `sdk/rocketnet_fragment.cpp:88`, `:89` | 2 | The enclosing public sizing function rejects these invalid arguments even if the private helper returns a different intermediate value. |
| `sdk/rocketnet_fragment.cpp:556`, `:577` | 2 | `final_staged` is a truth flag; after the staged bytes are copied its stale true value is used only by a Release-disabled assertion before the slot clears. |
| `sdk/rocketnet_protocol.cpp:216`, `:276`, `:301` | 3 | The crypto call overwrites `sealed_bytes`; the validated seal call cannot take its error return; and both `-1` and `-2` remain negative errors to callers. |
| `sdk/rocketnet_window_ack.cpp:173`, `:230` | 2 | The sent index has at least two entries, and an index entry with `used == 0` never exposes its stale `entry` field. |
| `sdk/rocketnet_ack.cpp:910` | 1 | Aggregate IDs are opaque nonzero tokens; starting at 2 instead of 1 preserves uniqueness, ordering and the ring's cycling capacity. |

## Counts

- bug: 0
- risk: 5 (findings 1–5)
- quality: 20 (findings 6–25)
- total: 25

## Already filed

- `sdk/rocketnet_ack.cpp:272` (record equality helper): `docs/audits/2026-10-24-coverage-production-paths.md` finding 4.

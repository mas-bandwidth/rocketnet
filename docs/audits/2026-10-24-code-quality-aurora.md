# Code-quality audit — Aurora Muse, 2026-10-24

Independent cold audit of `mas-bandwidth/rocketnet` at `3550a6f33f03126b38f42211cd768f6a42b16e90`
(`main` as named in the card). Scope read: the whole of `sdk/` plus `tests/`; the card's
`include/rocketnet.h` does not exist at this sha, so the public header read was `sdk/rocketnet.h`.
Code quality only: no findings about licensing, RakNet API parity, scope, features, or
performance targets. No code was changed. Findings are ordered most severe first; each gives
severity (`bug` / `risk` / `quality`), `file:line` at 3550a6f3, a one-sentence defect, a
one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_stats_window.h:13`

- Defect: The advance condition treats `time < buckets[index].time` (a backwards clock) as elapsed time, advancing to and zeroing the next bucket, despite the header comment (lines 6–8) saying buckets advance with elapsed time.
- Failure or cost: A non-monotonic time source (a rewound game clock, a step-driven test clock, a corrected wall clock) wipes one stats bucket per update and can cycle through all of them, silently destroying the published per-second rates.
- Smallest fix: Drop the `time < buckets[index].time` disjunct so a backwards step keeps the current bucket (the duplicated client copy at `sdk/rocketnet_client_transport.cpp:2281` needs the same change).

### 2. bug — `sdk/rocketnet_packet_loss_tracker.h:91`

- Defect: When the unprocessed sequence gap reaches `ROCKETNET_PACKET_LOSS_TRACKER_HISTORY` (1024), `rocketnet_packet_loss_tracker_update` jumps `last_packet_processed` to the newest sequence and returns 0 lost with no comment or counter naming the discard.
- Failure or cost: After a stall or burst larger than 1024 sequences between updates, the client publishes 0 lost packets for that interval (`sdk/rocketnet_client_transport.cpp:2348` adds the return straight into `packets_lost`), so packet-loss statistics read clean exactly when loss was worst.
- Smallest fix: Return the count of unjudged sequences as lost (or clamp-count them), and name the behaviour in a comment beside the branch.

### 3. risk — `sdk/rocketnet_window_send.cpp:19`

- Defect: `rn_window_send_position` computes `send->ring_next % B` with `B = region->ring_bytes`, and the only guarantee that `ring_bytes` is non-zero and a multiple of 16 is a `rocketnet_assert` in `rn_window_send_init` (line 85), which compiles out under `NDEBUG`.
- Failure or cost: A release-build region carved without the validated config path (a future or test caller) reaches this line with `ring_bytes == 0` and takes a hardware divide-by-zero (SIGFPE) instead of a stall/refusal.
- Smallest fix: Return a stall from `rn_window_send_admit` when `ring_bytes == 0`, or make `rn_window_send_init` refuse (return a status) on a zero/non-multiple-of-16 ring.

### 4. risk — `sdk/rocketnet_fragment.cpp:312`

- Defect: `rocketnet_reassembly_process_fragment` bounds-checks `connection_index` only with `rocketnet_assert` (also line 265 in the reset entry point), then indexes `newest_completed[connection_index]`, the slot array (line 350), and the arena with it unchecked.
- Failure or cost: In a release build, any caller that passes an out-of-range connection index gets an out-of-bounds read/write in heap arena memory rather than a counted refusal.
- Smallest fix: Replace the assert with a real range check that returns 0 (and fires `size_mismatch` or a dedicated counter) at both entry points.

### 5. risk — `sdk/rocketnet_base64.cpp:63`

- Defect: `rocketnet_base64_encode_data` returns `int( output_length )` with no check that the encoded length fits an `int`, while the only overflow guard (lines 21–26) covers the `size_t` computation, not the narrowing return.
- Failure or cost: Encoding more than about 1.6 GB of input into a sufficiently large caller buffer succeeds in memory but returns a negative or truncated length that callers read as the `-1` failure answer, corrupting any length-based follow-on use.
- Smallest fix: Refuse (return -1) when `output_length > INT_MAX` next to the existing overflow check.

### 6. risk — `sdk/rocketnet_packet_lease.h:40`

- Defect: `insert` increments `used` with `used.fetch_add(1, …)` before knowing the insert will succeed, and nothing except `reset()` ever decrements it, so failed inserts (ticket exhaustion at line 43, or a full probe at line 63) permanently consume the epoch's half-capacity budget.
- Failure or cost: A burst of failed lease inserts early in an epoch makes every later insert in that epoch fail the `>= Capacity / 2` test even while most lease slots are physically free, dropping packets the table could have carried.
- Smallest fix: Increment `used` only after a slot is claimed (or decrement it on each failure return).

### 7. quality — `sdk/rocketnet_network_simulator.cpp:201`

- Defect: The shard-hash comment claims `rocketnet_address_equal` "considers two addresses of a type that is neither IPv4 nor IPv6 equal on the type alone", but `sdk/rocketnet_address.cpp:199` returns `false` for every non-IPv4/IPv6 type, degenerate addresses included.
- Failure or cost: The comment states the index's load-bearing equivalence backwards, so a maintainer trusting it can "fix" the hash or the equality and silently change which simulator packets are deliverable.
- Smallest fix: Correct the comment to say degenerate types are never equal, and that the hash folding only the type is therefore merely conservative (equal ⇒ same shard still holds).

### 8. quality — `sdk/rocketnet_network_simulator.cpp:371`

- Defect: `rocketnet_network_simulator_config` sets `created_active = 1` on any call with a non-zero setting and never clears it, while the struct comment (lines 149–152) and `rocketnet_network_simulator_created_active` (lines 469–472) both say the value is decided once at create and never changes.
- Failure or cost: A simulator configured live (as the sdk test at `sdk/rocketnet_tests.cpp:6046` does) permanently flips the path-selection answer its transports read, so the documented "a connection's path never moves" rule rests on callers happening to call `config` only at create.
- Smallest fix: Set `created_active` only on the first `config` call (track "configured once"), or rewrite both comments to say the flag is sticky-on-first-active.

### 9. quality — `sdk/rocketnet_client_transport.cpp:2281`

- Defect: The client re-implements the stats-window advance inline instead of using the shared `rocketnet_stats_window_advance` template (`sdk/rocketnet_stats_window.h:10`) that the server uses, duplicating the bucket, frame, and time bookkeeping across two array layouts.
- Failure or cost: Every fix to the advance rule (including finding 1 above) must be made twice and can drift, leaving client and server publishing rates under different rules.
- Smallest fix: Store the client's frame/time beside its frames in one bucket struct and call the shared template, deleting the copy.

### 10. quality — `sdk/rocketnet_address.cpp:131`

- Defect: `rocketnet_address_to_string` checks the `snprintf` result and logs truncation on the IPv6 path (lines 119–123) but ignores the return entirely on the IPv4 paths (lines 131–148), and `rocketnet_address_to_string_without_port` checks it on neither path.
- Failure or cost: The same function family alternately reports and silently swallows a truncated address string depending on family and entry point, so a caller cannot tell from behaviour whether the buffer it received is complete.
- Smallest fix: Check the `snprintf` result the same way in all four branches (log, or return a distinguishable truncation answer).

### 11. quality — `sdk/rocketnet_address.cpp:236`

- Defect: `rocketnet_address_anonymize` sends every non-IPv4 address down the IPv6 branch, so an address of type NONE (or any future type) has its `data.ipv6[4..7]` words zeroed as if it were a native IPv6 address.
- Failure or cost: Anonymizing an unset address mutates bytes that do not belong to the field its type names, which misleads anyone later inspecting or logging the anonymized value's raw storage.
- Smallest fix: Branch explicitly on IPv6 and leave other types untouched apart from the port.

### 12. quality — `sdk/rocketnet_window_send.cpp:193`

- Defect: `rn_window_send_can_write_unreliable` ignores its argument (line 195 casts it away) and unconditionally returns 1, and no production caller calls it — only its declaration in `sdk/rocketnet_window_send.h:52` and this definition exist.
- Failure or cost: A reader meets a function whose name promises a window check that never checks anything, and cannot tell whether the always-yes answer is a load-bearing rule or unfinished work.
- Smallest fix: Delete the function and its declaration, or call it from the unreliable write path with a comment naming why the answer is always yes.

### 13. quality — `sdk/rocketnet_out_of_order_tracker.h:49`

- Defect: The whole out-of-order tracker (reset, packet_received, and its sentinel machinery) has no production caller; it is included by `sdk/rocketnet.cpp:14` and exercised only by the sdk test suite, unlike the loss and jitter trackers beside it which the client transport actually feeds.
- Failure or cost: Dead tracking machinery with sentinel boilerplate is maintained, reviewed, and read as if it fed published statistics, and can rot against the sequence conventions it claims to judge.
- Smallest fix: Delete the header and its include, keeping the test's expectations against a local fixture, or wire it into the client's published stats.

### 14. quality — `sdk/rocketnet_config.cpp:330`

- Defect: `rocketnet_config_env_field_name` is an identity function (`return field_name;`) called at lines 340 and 367 as if a name mapping existed.
- Failure or cost: The indirection tells a reader a public-to-env name translation exists and sends them looking for a table or rule that is not there.
- Smallest fix: Delete the helper and use `field_name` directly at both call sites.

### 15. quality — `sdk/rocketnet_protocol.cpp:421`

- Defect: The final `best_distance = distance;` assignment in `rocketnet_reconstruct_near` (also the pattern at line 411) stores a value that is never read afterwards, since the function returns `best` immediately after the candidate loop.
- Failure or cost: The dead store makes the tie-breaking logic look stateful past its end and costs a reader a check that no later candidate consumes it.
- Smallest fix: Remove the final `best_distance` assignment (keep the one at line 411 only if a later candidate is ever added, with a comment saying so).

## Counts

- bug: 2 (findings 1, 2)
- risk: 4 (findings 3–6)
- quality: 9 (findings 7–15)
- total: 15

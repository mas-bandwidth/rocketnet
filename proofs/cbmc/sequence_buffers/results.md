# Sequence-buffer ESBMC results

ESBMC 8.5.0, one bench core at nice 19. `make -C proofs/cbmc proofs-sequence_buffers` runs the nightly list, and `make -C proofs/cbmc proofs-sequence_buffers-owed` runs the owed list, which the nightly does not run. Flags for every harness: `--std c++17 --overflow-check --ub-shift-check -DROCKETNET_ESBMC -DNDEBUG -I../../sdk --unwindsetname __memset_impl:0:513 --multi-property --memlimit 12g`, 1800 s per harness, plus the per-harness unwind and solver below. Unwinding assertions are on (the ESBMC default), and so are ESBMC's default pointer, bounds, alignment and division-by-zero checks. ESBMC 8.5 has no memset-limit option. The only memset these readers reach is the far replay advance's 512-byte window clear (sdk/rocketnet_replay.cpp:62), so `__memset_impl` gets its own explicit unwind of 513.

Each harness is one translation unit. `proof_sequence_buffers.c` includes the real `sdk/rocketnet_replay.cpp`, `sdk/rocketnet_ack.cpp` and `sdk/rocketnet_window_receive.cpp` through `proofs/cbmc/esbmc_shim.h`, so the ESBMC 8.5 merge of header statics across units cannot apply. Only the platform lock and atomic primitives are modelled, as sequential operations. Each claim is per call: one arbitrary sequence against one arbitrary state.

CBMC is not a fallback here. CBMC 6.6.0 with `--cpp11 --external-sat-solver cadical` cannot parse `rocketnet_replay.cpp` (the `__builtin_offsetof` static_asserts at sdk/rocketnet_wire.h:150-151) or `rocketnet_ack.cpp` (`<initializer_list>`, and sdk/rocketnet_memory.h:24). Without `--cpp11` it cannot parse `rocketnet.h:71` either.

## Nightly: proved

| harness | properties | bound proved | unwind | solver | time |
| --- | ---: | --- | ---: | --- | ---: |
| `harness_replay_already_received` | 21/21 | every nonzero `uint64_t` sequence, every high-water mark, every 4096-bit window | 65 | Z3 | 4 s |
| `harness_replay_advance_step` | 42/42 | every sequence at or behind the high-water mark, every near skip up to 2, every far skip; with the induction below, every near skip | 65 | Z3 | 23 s |
| `harness_replay_advance_near_bounded` | 42/42 | every sequence at or behind the high-water mark, every near skip up to 256, every far skip | 257 | Bitwuzla | 140 s |
| `harness_replay_advance_far` | 42/42 | every far skip (at least 4096): the whole-window clear | 65 | Z3 | 12 s |
| `harness_ack_record_received` | 21/21 | every nonzero `uint64_t` sequence against the 2048-entry received ring, with the written record pinned to its ring slot | 2 | Bitwuzla | 15 s |
| `harness_ack_mark_keep_alive` | 23/23 | every nonzero `uint64_t` sequence against the 4096-entry sent ring: object-level bounds, alignment and the lock model (ring-level containment is owed, below) | 2 | Z3 | 6 s |
| `harness_window_receive_init` | 39/39 | every first number, every power-of-two W from 64 to 1 << 20 | 2 | Z3 | 7 s |
| `harness_window_receive_accept_step` | 167/167 | every wire u24 number, every R, every power-of-two W from 64 to 1 << 20, every received bit except R + 1 and R + 2; with the induction below, every run of the advance loop | 4 | Bitwuzla | 10 s |
| `harness_window_receive_accept_bounded` | 161/161 | every wire u24 number and arbitrary receiver state at W = 64, the whole advance loop unrolled | 65 | Bitwuzla | 105 s |
| `harness_window_receive_release` | 50/50 | every `uint64_t` number, every power-of-two W from 64 to 1 << 20 | 2 | Z3 | 4 s |

All 10 harnesses verify, 608 of 608 properties, in 326 s from one run of the target at the head. So the nightly is green while the sdk is.

## The two loops: step harness plus induction

The near clear loop of the replay advance (sdk/rocketnet_replay.cpp:66-67) runs up to 4095 times. The window-accept advance loop (sdk/rocketnet_window_receive.cpp:92-101) runs up to W times, and W goes up to 1 << 20. Neither can be unwound inside the limits: the full harnesses below run out of 12 GB within a minute. ESBMC 8.5's k-induction is disabled for loops that write an array through a pointer. So each loop is proved in two parts.

- **Step harness, proved by ESBMC.** `harness_replay_advance_step` takes every sequence and every window state, and limits a near skip to at most 2, so the loop body runs at most once. `harness_window_receive_accept_step` takes every W from 64 to 1 << 20, every R, every wire number and every received bit, except that the bits for R + 1 and R + 2 start clear. The accepted number sets at most one of those two bits, so the loop inspects at most R, R + 1 and R + 2. Both harnesses are proved whole, with unwinding assertions on.
- **Induction, by hand.** The checked operations in each loop body depend only on the loop counter (`s` in the replay loop; `R` and `W` in the accept loop) and on the fixed object pointers. They do not depend on the bits. These operations are: the bounds of the word read and write, the shift amount, and the increment. In the first iteration the counter is arbitrary (`s = highest_opened + 1` for an arbitrary `highest_opened`; `R` as the call receives it), so the step harness covers the body for every counter value. Iteration k of a longer run is the same body at counter value s + k or R + k, which the step harness has already covered. Neither increment can wrap: `s < sequence` holds in the replay loop, and in the accept loop `R` is a `uint64_t` whose increment is defined. Both loops terminate. The replay loop's trip count is `sequence - highest_opened - 1`, which is less than 4096. Each accept iteration clears the bit at `R & (W - 1)` and moves R on, and nothing sets a bit again during the loop, so after at most W iterations it reaches a clear bit.

The bounded companions are kept as direct checks of longer runs: `harness_replay_advance_near_bounded` covers skips up to 256, and `harness_window_receive_accept_bounded` covers W = 64.

## Owed: gated off the nightly

These run only from `proofs-sequence_buffers-owed`, so they do not turn the nightly red. Their follow-up is the proposed card `rocketnet-cbmc-sequence-buffers-owed-bounds`.

- `harness_replay_advance` (the full unroll, unwind 4097): out of 12 GB in about 30 s. Its bounds and UB claims are proved by the step harness and the induction above; this harness checks the same claims by unrolling.
- `harness_window_receive_accept` (the full unroll, unwind 1048577): out of 12 GB in about 55 s. Covered the same way.
- `harness_ack_record_sent_classified`: unproved. Bitwuzla proves 31 of 57 claims and then runs out of 12 GB (24 GB with a single combined claim). Z3 got no answer in 11 minutes, and nothing within 30 minutes on a single combined claim or with a heap endpoint. Boolector aborts. Yices, MathSAT and CVC5 are not built into this ESBMC. Proved claims: bounds and alignment of the selected record's reads at sdk/rocketnet_ack.cpp:245, 261 and 263; alignment of the writes at 272-274 and of the bitmap words at 282-283; the lock and atomic model's asserts; and the libm overflow claims. Not checked: the field writes at 267-271, the bounds of the writes at 272-274, the bitmap updates at 282-283, the outcome-range assert, and the ring pin below.
- `harness_ack_mark_keep_alive_pinned`: the ring pin for the keep-alive tag (below). The single pin claim gives "SMT solver failed" under Z3 after 16 s and runs out of 12 GB under Bitwuzla. The other 26 claims of the harness pass.

## The ring pin, and what the planted-bug check found

ESBMC checks a dereference only against the whole object. An index past the `sent` or `received` ring inside `rocketnet_ack_endpoint_t` still lands inside the endpoint, so ESBMC does not report it as a bounds failure. The sdk takes the address of the selected record (`&endpoint->sent[sequence % N]`), and that address carries no claim of its own. The planted-bug check showed this gap. With `sequence % ( ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1 )` planted at sdk/rocketnet_ack.cpp:331, the original `harness_ack_mark_keep_alive` still reports VERIFICATION SUCCESSFUL. That harness's own `slot < SIZE` assert was computed by the harness, not by the sdk.

The fix is to pin the record the sdk selects. That record is a function of the sequence alone (sdk/rocketnet_ack.cpp:244, 331 and 363). So if the record the sdk writes is `ring[sequence % N]` for every state that reaches the write, the sdk's access is inside the ring for every state. `harness_ack_record_received` asserts that the received record at `sequence % N` holds the sequence after the call, and that pin is proved. `harness_ack_mark_keep_alive` stays in the nightly for its object-level bounds, alignment and lock claims. Its ring-level containment is owed to `harness_ack_mark_keep_alive_pinned`.

The replay window and the window-receive tables do not have this gap. `window` is the last member of `rocketnet_replay_protection_t` (sdk/rocketnet_encryption_map.h:37-41), so any index past it is past the object. The receive bit and descriptor tables are their own heap objects (sdk/rocketnet_allocator.h:339-340).

## Planted-bug check

Each plant sits in a scratch copy of sdk/ on the bench, is never committed, and is run through the same harness and flags:

| planted at | change | harness | result |
| --- | --- | --- | --- |
| sdk/rocketnet_replay.cpp:14 and :26 | `bit / 64` becomes `bit / 63` | `harness_replay_advance_step` | violated: object out of bounds at :14 and at :26 (the loop body) |
| sdk/rocketnet_window_receive.cpp:96 | `floor_index >> 6` becomes `>> 5` | `harness_window_receive_accept_step` | VERIFICATION FAILED: heap array bounds at :97 and :99 (the loop body) |
| sdk/rocketnet_window_receive.cpp:111 | `number & ( window - 1 )` becomes `number & window` | `harness_window_receive_release` | VERIFICATION FAILED: heap array bounds at :113 and :114 |
| sdk/rocketnet_ack.cpp:363 | `% N` becomes `% ( N + 1 )` | `harness_ack_record_received` | VERIFICATION FAILED: the ring pin at proof_sequence_buffers.c:143 |
| sdk/rocketnet_ack.cpp:331 | `% N` becomes `% ( N + 1 )` | `harness_ack_mark_keep_alive` | VERIFICATION SUCCESSFUL: the gap above; the pin harness is owed |

No property was refuted on the real sdk, so there is no counterexample to check against the sdk.

## Known defect

The brief names the far-advance defect (rocketnet-replay-far-advance-clears-every-window-word). At this base the far branch is a memset of the whole window (sdk/rocketnet_replay.cpp:62), and `harness_replay_advance_far` proves it against the real sdk. The per-commit `replay-far` target in `all` still reports its expected counterexample, because `proof_replay_advance.c` mirrors the old four-word clear instead of including the sdk.

The window harnesses set `region->counters = NULL`, so the duplicate counter's atomic add (sdk/rocketnet_window_receive.cpp:15) is not reached. Its index is the fixed `ROCKETNET_COUNTER_MESSAGE_DUPLICATE`, not one derived from a sequence.

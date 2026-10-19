# Sequence-buffer ESBMC results

ESBMC 8.5.0, one bench core at nice 19, from `make -C proofs/cbmc proofs-sequence_buffers`. Flags for every harness: `--std c++17 --overflow-check --ub-shift-check -DROCKETNET_ESBMC -DNDEBUG -I../../sdk --unwindsetname __memset_impl:0:513 --multi-property --memlimit 12g`, 1800 s per harness, plus the per-harness unwind and solver below. ESBMC's default pointer, bounds, alignment and division-by-zero checks stay on. ESBMC 8.5 has no memset-limit option; the only memset these readers reach is the far replay advance's 512-byte window clear (sdk/rocketnet_replay.cpp:62), and `__memset_impl` gets its own explicit unwind of 513.

The harnesses include the real `sdk/rocketnet_replay.cpp`, `sdk/rocketnet_ack.cpp` and `sdk/rocketnet_window_receive.cpp` through `proofs/cbmc/esbmc_shim.h`. Only the platform lock and atomic primitives are modelled, as sequential operations. Each claim is per call: one arbitrary sequence against one arbitrary state. The ack harnesses leave every endpoint field arbitrary except the lock word, which is a wider state than the ring invariants allow.

Z3 is the default solver. Bitwuzla is used where it finishes and Z3 does not: `harness_ack_record_received` (Z3 spent more than 12 minutes on its read-back claim; Bitwuzla takes 24 s), `harness_ack_record_sent_classified` (Z3 spent 11 minutes on one claim at sdk/rocketnet_ack.cpp:263 without an answer; Bitwuzla reports 31 claims before running out of memory), and the two bounded loop harnesses (thousands of claims, one per unwound iteration).

| harness | result | properties | bound proved | unwind | solver | time |
| --- | --- | ---: | --- | ---: | --- | ---: |
| `harness_replay_already_received` | proved | 21/21 | every nonzero `uint64_t` sequence, every high-water mark, every 4096-bit window | 65 | Z3 | 7 s |
| `harness_replay_advance` | out of memory, unproved | 19 passed, rest not checked | none (full claim: every skip) | 4097 | Z3 | 30 s |
| `harness_replay_advance_near_bounded` | proved | 42/42 | every sequence at or behind the high-water mark, every near skip up to 256, every far skip | 257 | Bitwuzla | 737 s |
| `harness_replay_advance_far` | proved | 42/42 | every far skip (at least 4096): the whole-window clear | 65 | Z3 | 204 s |
| `harness_ack_record_received` | proved | 22/22 | every nonzero `uint64_t` sequence against the 2048-entry received ring | 2 | Bitwuzla | 24 s |
| `harness_ack_record_sent_classified` | out of memory, partly proved | 31 of 57 passed, 26 not checked | the 31 claims named below | 2 | Bitwuzla | 73 s |
| `harness_ack_mark_keep_alive` | proved | 23/23 | every nonzero `uint64_t` sequence against the 4096-entry sent ring | 2 | Z3 | 7 s |
| `harness_window_receive_init` | proved | 39/39 | every first number, every power-of-two W from 64 to 1 << 20 | 2 | Z3 | 8 s |
| `harness_window_receive_accept` | out of memory, unproved | 71 passed, rest not checked | none (full claim: every W to 1 << 20) | 1048577 | Z3 | 55 s |
| `harness_window_receive_accept_bounded` | proved | 161/161 | every wire u24 number and arbitrary receiver state at W = 64 | 65 | Bitwuzla | 548 s |
| `harness_window_receive_release` | proved | 50/50 | every `uint64_t` number, every power-of-two W from 64 to 1 << 20 | 2 | Z3 | 12 s |

Every row comes from one run of the make target at the merged head, except `harness_window_receive_accept_bounded`, which was rerun through the same target (`SEQUENCE_BUFFERS_HARNESSES=harness_window_receive_accept_bounded:65:--bitwuzla`) after its bound moved from 128 (timed out at 1800 s) to 64. That macro is read by no other harness. The target exits nonzero while the three resource-limited harnesses stay unproved, so the nightly is red by design until those bounds finish.

The window harnesses set `region->counters = NULL`, so the duplicate counter's atomic add (sdk/rocketnet_window_receive.cpp:15) is not reached. Its index is the fixed `ROCKETNET_COUNTER_MESSAGE_DUPLICATE`, not derived from a sequence.

No property was refuted, so there is no counterexample to check against the sdk. Every unproved property below is a tool or harness limit, not a defect.

## Unproved, with the sdk line and the reason

- `harness_replay_advance` (full claim). The near clear loop at sdk/rocketnet_replay.cpp:66-67 runs up to 4095 times. Unwinding 4097 runs out of 12 GB in 30 s (14 GB in 31 s in an earlier run). k-induction cannot help: ESBMC 8.5 disables the inductive step for loops that write an array through a pointer. CBMC 6.6 cannot parse the sdk (`__builtin_offsetof` static_asserts in sdk/rocketnet_wire.h:150-182), so the CBMC fallback does not apply. Proved instead: near skips up to 256 and every far skip. Bounds that did not finish inside 1800 s on one core: near skip 1024 timed out at 1800 s with 6405 claims passed and none failed; near skip 512 was stopped after about 14 minutes with 3178 claims passed and none failed. Near skips 257 to 4095 are not proved.
- `harness_ack_record_sent_classified`. Bitwuzla runs out of 12 GB on the record's field writes after the eviction check. Z3 had no answer after 11 minutes on the alignment claim at sdk/rocketnet_ack.cpp:263. Proved (31 claims): bounds and alignment of the selected record's reads at sdk/rocketnet_ack.cpp:245, 261 and 263; alignment of the writes at 272-274 and of the bitmap words at 282-283; the lock and atomic model's asserts; and the libm overflow claims. Not checked (26 claims): bounds and alignment of the field writes at sdk/rocketnet_ack.cpp:267-271; bounds of the writes at 272-274; the two bitmap updates at 282-283 (array bounds and shift); and the harness's three asserts at proof_sequence_buffers.c:155-162 (outcome range, ring index, bitmap index). With the harness's earlier, narrower state (the selected record and bitmaps written by the harness), both solvers stalled earlier, at 263. Turning off alignment checks did not help: the bounds claim at 263 then ran out of memory. `harness_ack_mark_keep_alive` proves the same `sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE` index into the same ring.
- `harness_window_receive_accept` (full claim). The advance loop at sdk/rocketnet_window_receive.cpp:92-101 runs up to W times. Unwinding 1048577 runs out of 12 GB in 55 s. k-induction is disabled for the same reason as above. Proved instead: W = 64, the smallest configurable window. W up to 128 timed out at 1800 s; W up to 256 was stopped after about 14 minutes with 1814 claims passed and none failed. W from 128 to 1 << 20, including the configured default of 1024, is not proved.

## Known defect

The brief names the far-advance defect (rocketnet-replay-far-advance-clears-every-window-word). At this base the far branch is a memset of the whole window (sdk/rocketnet_replay.cpp:62), and `harness_replay_advance_far` proves it against the real sdk. The per-commit `replay-far` target in `all` still reports its expected counterexample, because `proof_replay_advance.c` mirrors the old four-word clear instead of including the sdk.

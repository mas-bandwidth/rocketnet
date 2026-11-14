# CBMC and ESBMC proof inventory

Inventory recovery base: RocketNet `d5dd954f09d618d48aa5d897fca18f71785571db`
(earlier attempts ran at `ef0229e87240cadf6e3b2376d2f72782d5fd7c97`,
`b8058dc3a7bf84eb9d7899315aed1b9d62ff7f53`, and
`0928d743587212f7ad1d5592280ba991d70a65fb`).
This file distinguishes a harness that compiles the current SDK body from a
copy of a body, and a retained historical result from an execution at this
exact source. A timeout, resource exhaustion, `UNKNOWN`, failed unwinding
assertion, skipped property, or smaller diagnostic bound is not a pass.

A bound tied to an SDK constant is written with the constant and its value,
`ROCKETNET_RECEIVE_CEILING` (1452); `tools/check_cbmc_bounds.py` checks every
such statement here, in the results files and in the harness comments (the
`.c` harnesses and the `.cpp` ones: `ack_run_exhaustive.cpp` and
`connect_token/proof_connect_token.cpp`) against the SDK, and fails the CBMC
workflow when one drifts. A bare number in a harness comment is not checked.

## Exact-head execution status

The first full-bound completion run is recorded in `full-bounds-results.md`.
The reviewed recovery decomposition and its second timeout are recorded in
`full-bounds-recovery-results.md`. The materially different built-in path
exploration run and its third incomplete result are recorded in
`full-bounds-paths-results.md`. The disjoint/exhaustive clear-loop-shape and
selected-word relation partition run—which machine-checks partition
completeness (`0 of 429 failed`), proves all five empty-clear and
single-partial-word full-domain partitions (`0 of 431 failed` each) and their
five reachability negative controls (`1 of 431 failed` each), reruns all six
default `make -C proofs/cbmc all` targets at exact head, and stops fail-closed
at `nw_multi_first_word`—is recorded in `full-bounds-partition-results.md`.
Historical `results.md` files remain useful design evidence, but are not
exact-head execution receipts when the included SDK translation unit changed
after the recorded run.

## Harnesses and claims

| Target / harness | Checker and source binding | Property and assumptions | Bound and unwind | Reachability / negative control | Exact-head status |
| --- | --- | --- | --- | --- | --- |
| `replay-near-4096` / `harness_replay_near_partition_completeness`, `harness_replay_near_nw_*`, `harness_replay_near_wrap_*` | CBMC 6.6.0 + CaDiCaL 2.1.3; includes current `sdk/rocketnet_replay.cpp` | For symbolic `sequence > highest_opened`, delta below the deployed 4096-bit history, and an arbitrary selected word, the production advance clears exactly the skipped ring bits, sets the received bit, and advances the high mark. Partitioned across 13 disjoint/exhaustive clear-loop-shape and selected-word relation cases (`0..12`). `highest_opened < UINT64_MAX` prevents only arithmetic wrap outside this operation's contract. | Full delta 1..4095 across the 13 disjoint/exhaustive partitions; 64 window words; global unwind 65 covers 64-word initialization and every production range-clear path. | `harness_replay_near_partition_completeness` proves pairwise disjointness, joint exhaustiveness, and witness membership (`0 of 429 failed`). All 13 concrete intended-failure controls fail solely on `replay path reachability negative control` (`1 of 431 failed`). | **PASS** nightly: run 43162404799 (2026-11-12, at ee94e6fbf), completeness and all 13 partitions green, each job's control failing as it must. Earlier, `full-bounds-partition-results.md` recorded partition 5 (`nw_multi_first_word`) timing out at CBMC_REPLAY_TIMEOUT=600 (`HOLD`, exit 124). |
| `replay-delta-1-3` / `harness_replay_near` | CBMC 6.6.0; includes current `sdk/rocketnet_replay.cpp` | Quick per-commit reduced-bound instance of `harness_replay_near` admitting deltas 1..3. | Reduced delta 1..3 (`REPLAY_DELTA_BOUND=4`); 64 words; unwind 65. | Covered by `nw_empty_clear` and `nw_single_partial_*` concrete reachability controls. | **PASS** at `d5dd954f` (`0 of 431 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`). |
| `replay-far` / `harness_replay_far` | CBMC 6.6.0; includes current `sdk/rocketnet_replay.cpp` | For delta at least 4096, the production advance clears every word, sets only the received bit, and advances the high mark. The high mark is restricted only enough to make the subtraction defined. | Full far range; 64 words; unwind 65. | All 64 result words are asserted. | **PASS** at `d5dd954f` (`0 of 431 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`). |
| `reconstruct` | CBMC 6.6.0; includes current `sdk/rocketnet_protocol.cpp` and `sdk/rocketnet_message.h` | `rocketnet_reconstruct_near` and `rocketnet_reconstruct_sequence` (32-bit) and `rocketnet_message_u24_delta` (the 24-bit message number `rn_window_receive_accept` reconstructs) keep the wire low bits, and round-trip every non-negative in-window value; the 32-bit result is the nearest non-negative candidate (the larger on a tie), the 24-bit result is `reference + delta` within 2^23 of the reference where the behind value is non-negative (the lower on a tie). The first lap is proved from zero, with no floor assumed (since `4f69c6089`, 2026-11-09): there the 32-bit code does not wrap below zero and does not clamp, it takes the nearest candidate at or above zero, so `\|result - expected\| <= 2^31` is asserted only from `expected >= 2^31` on (below that the nearest candidate can be farther); `rocketnet_message_u24_delta` is plain modular arithmetic and returns a negative delta pointing below zero, and such a behind value that would sit below zero, which `rn_window_receive_accept` refuses as behind without subtracting, is proved to have no non-negative candidate in the window (its nearest lies at least 2^23 ahead). Assumed, not proved: the ceilings `expected < 2^64 - 2^32` (32-bit; `highest_opened < 2^64 - 2^32 - 1` for `reconstruct_sequence`) and `reference <= 2^64 - 2^23` (24-bit); the uint64 range above them is not covered. | One arithmetic step; unwind 1. | `reconstruct-reachability` must fail only its named control. A tie rule turned around in `rocketnet_reconstruct_near` or `rocketnet_message_u24_delta` fails the round-trip assertions; the two first-lap plants in `reconstruct-results.md` fail this harness and passed the floored one. | **PASS** at `4f2960e5f` (v1.1.54 integration) on aws-mut-3, pinned CBMC 6.6.0 (binary sha256 `ceefc9de…a6cc2`, the same as hulk's), 2026-11-13: `0 of 636 failed`, VERIFICATION SUCCESSFUL, 7.2 s; reachability `1 of 637 failed` only on `reconstruct reachability control`, EXPECTED FAILURE, 6.6 s. This row is the receipt for that run. Earlier: `0 of 617` at `4f69c6089` on vision (2026-11-09); the floored harness `0 of 608` at `59752f6` on hulk; both with the planted bugs in [reconstruct-results.md](reconstruct-results.md). |
| `ack-run-bound-8`, `ack-run-full-4096` / `harness_ack_apply_run` | ESBMC 8.5.0 C++17; includes current `sdk/rocketnet_ack.cpp` and calls its static production leaf | A symbolic in-window run has matching live records and exact `sent_written`, `sent_held`, and `sent_unacked` identities; each record's prior ack state is symbolic. Records carry no packet id and use a negative RTT sample. The production bitmap walk must count every and only newly acked record, visit it once, retain live identity, clear unacked identity, produce no unknown/batch-full result, and update `largest_acked_sequence`. | Required run 1..8, then declared retained-window run 1..4096. Run-8 global unwind 65 plus named init 9, popcount 3, bitmap inner 9 / outer 3 and unreachable RTT 1; full unwind 4097 with named init 4097. | `ack-run-negative-control` requires a newly unacked record and negates the observed production visit count after the call. It must fail on the named assertion. | **NO ESBMC VERDICT AT ANY BOUND (2026-11-12)**: ESBMC 8.5 cannot encode this leaf; see "ACK run measurements" below. Never a proof; on no list and in no workflow since 2026-11-12. The bound-8 claim is checked instead by `ack-run-exhaustive-8` (next row), which is exhaustive native testing, not a symbolic proof. `ack-run-full-4096` has never run and is not established by anything. |
| `ack-run-exhaustive-8` / `ack_run_exhaustive.cpp` | Native C++17 (g++/clang `-O2 -Wall -Wextra -Wpedantic -Werror`); includes current `sdk/rocketnet_ack.cpp` and calls its static production leaf | Every assertion of `harness_ack_apply_run`, checked for every sequence of the run (not one chosen), plus: in the filled neighbourhood the neighbours' unacked bits, records and share of `stats_eligible_unacked` are unchanged, and the whole endpoint is all-zero again after the run and its neighbours are cleared (no write outside them). | Every case of the harness's finite state at run length 1..8: first ring slot 0 to `ROCKETNET_SENT_PACKET_BUFFER_SIZE - 1` (4095), length 1..8, every prior-ack pattern, every `highest - low` in [len - 1, 4095], in two neighbourhoods (the rest of the ring empty, as the ESBMC harness has it; and every other slot of the bitmap words the run touches holding a live unacked record of the sequence the window gives it, as production has it), in sequence blocks 0 (start-up: the stats window clamped at 1), 1 and the last below 2^64; 51,258,513,416 cases. Blocks 2 to last - 1 are not run: for `highest >= 4096` the leaf reads sequences only through `% 4096`, differences and comparisons with `highest - 4095` and `highest - 2048`, which a shift by a multiple of 4096 preserves (an argument, not a check). Run lengths 9 to `ROCKETNET_SENT_PACKET_BUFFER_SIZE` (4096) are not covered. | `ack-run-exhaustive-negative-control` (visit check negated, must fail); `ack-run-exhaustive-vacuity-plant` (no case reaches the leaf, must report VACUOUS: the coverage counters for cases, wrapped runs, runs straddling the loss-eligibility edge, the start-up regime, filled neighbourhoods and newly acked records must all be non-zero); `ack-run-exhaustive-mutants` (four one-line bugs planted by sed in a copy of the leaf: masked top bit, masked low bits, unacked bit left set, wrapped half of the walk skipped; each must fail, and a sed that no longer applies is HOLD; the masked-low-bits mutant survived the first driver and is caught by this one). Every build stops at its first failure. Planted-violations row `nightly-ack-proof/exhaustive/top-bit` plants the masked-top-bit bug in `sdk/rocketnet_ack.cpp` itself and requires the nightly step to go red. | **PASS** at 76edea48e plus this driver (2026-11-12, MacBook M5 Max, 8 threads): `failures=0`, every coverage counter non-zero. Negative control, vacuity plant and all four mutants as expected. The whole set (seven builds, the full check, the negative control, the plant and four mutants) took 880 s wall, 6076 s CPU, 121 MB peak RSS (the compiler; the check itself holds about 5 MB). The empty-neighbourhood half alone passed earlier on space3 in 67.5 s on 22 threads. Nightly: `.github/workflows/nightly-ack-proof.yml` on the Linux long runners. |
| `read-write` | CBMC 6.6.0; current inline bodies from `sdk/rocketnet_read_write.h`; `rocketnet_copy_string` is the sdk's own text: the Makefile cuts its definition out of `sdk/rocketnet.cpp` verbatim into `copy_string.gen.c` (HOLD when no complete definition is found) and the harness includes it (since `121b59249`, 2026-11-12). `rocketnet_api_null_guard` is modelled as a release build has it (NULL returns false); the misuse count and the debug assert are not proved. | Integer and byte round trips (little-endian, cursor advance); `rocketnet_copy_string`: size 0, NULL dest and NULL source return 0 and write nothing; a symbolic `dest_size` 1..8 returns a length below it, terminates the copy, keeps the source prefix, stops at the source terminator, clears the rest of `dest_size` and writes nothing past it. | Eight-byte buffers; unwind 9. | `read-write-reachability` must fail only its named control. A terminator overrun planted in a copy of `sdk/rocketnet.cpp` (`i < dest_size` for `i < dest_size - 1`) fails the proof (`3 of 517 failed`: length fits, `dest` upper bound, copy terminated). | **PASS** at `4f2960e5f` (v1.1.54 integration) on aws-mut-3, pinned CBMC 6.6.0 (binary sha256 `ceefc9de…a6cc2`), 2026-11-13: `0 of 518 failed`, VERIFICATION SUCCESSFUL; reachability `1 of 519 failed` only on `read-write reachability control`, EXPECTED FAILURE; the planted overrun above VERIFICATION FAILED (HOLD, exit 10). This row is the receipt for that run. The `d5dd954f` result (`0 of 1031 failed`, `full-bounds-partition-results.md`) proved the earlier hand copy and is history. |
| `message-read` | CBMC 6.6.0; includes current `sdk/rocketnet_message.cpp` | Reader begin/next never reads beyond a 24-byte symbolic payload; accepted record spans fit the receive buffer; malformed input and a short fixed config body refuse. Limits are symbolic over every value the configuration accepts and below: W and B 1..`1 << 20`, `max_message_bytes` 1..`ROCKETNET_MAX_MESSAGE_BYTES` (1048576). | Payload 24; unwind 8. | `message-read-reachability`; with check 8's block span test removed from `rocketnet_message.cpp` the proof fails `block span fits the ring`. | **PASS** at a26abaf8 on vision (`0 of 1440 failed`, `VERIFICATION SUCCESSFUL`). |
| `xdp-free-list` | CBMC 6.6.0; includes current `sdk/rocketnet_xdp_free_list.h` | Push/pop preserves exactly the admitted frames; full, out-of-region, and non-4096-aligned pushes refuse. | Capacity 3, four operations; unwind 5. | Refusal branches and list equality asserted. | **PASS** at `d5dd954f` (`0 of 92 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`); reduced capacity 3 is not the deployed frame count. |
| `proofs-message_read_max` | ESBMC C++17; includes current `sdk/rocketnet_message.cpp` | Multi-record reader, full-size single record, and fixed config body parsing, with the limits symbolic over every value the configuration accepts (as `message-read`). | Multi-record payload only 0..24 with unwind 7, run as two W partitions (1..4096 and 4097..`1 << 20`); single/config payload 0..`ROCKETNET_RECEIVE_PAYLOAD_BYTES` (1416) with unwind 2/1. | Planted out-of-bounds failures are recorded in `esbmc_r_ok/results.md`; with check 8's block span test removed, partition 1 fails (`1 of 211 failed`). | **PASS** at `59752f6` on vision (`0 of 216`, `0 of 217`, `0 of 205`, `0 of 71` failed; current and historical receipts in `message_read_max/results.md`). The multi-record harness is reduced-bound in payload length. |
| `proofs-packet_open` | CBMC; includes current `sdk/rocketnet_protocol.cpp` | Full packet prefilter/open bounds, associated-data writer, and sequence nonce. Cryptographic primitive correctness is not claimed; nondeterministic crypto return values exercise caller memory and refusal paths. | Packet bytes 0..`ROCKETNET_RECEIVE_CEILING + 1` (1453) for the prefilter and 0..`ROCKETNET_RECEIVE_PACKET_BYTES` (1452) for the opener, output 0..`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` (1416); unwind 64; object bits 12. | A planted undersized allocation is retained in `esbmc_r_ok/results.md`. | **PASS** at 49c36e8f, SDK body identical at main 212f6da6 (`0 of 765`, `0 of 928`, `0 of 924`, `0 of 765` failed; `packet_open/results.md`). |
| `proofs-connect_token` | ESBMC C++17; includes current `sdk/rocketnet_connect_token.cpp` | Token, signed-data and private-data readers plus caller-side verify/open consistency and identifier bounds. Cryptographic primitive correctness is not claimed. | Full objects: `ROCKETNET_CONNECT_TOKEN_BYTES` (768), `ROCKETNET_SIGNED_CONNECT_DATA_BYTES` (576), `ROCKETNET_SEALED_BLOCK_BYTES` (432), `ROCKETNET_PRIVATE_BLOCK_BYTES` (384); per-harness unwind 1 to `ROCKETNET_SIGNED_CONNECT_DATA_BYTES + 1` (577); 30-minute and 8 GiB limits. | Retained planted read/write failures are in `esbmc_r_ok/results.md`. | Retained result predates current SDK translation-unit changes. |
| `proofs-esbmc_r_ok` | ESBMC 8.5.0, C and C++17; tests `esbmc_shim.h`, not product behavior | Dynamic allocation extent and pointer-offset checks accept in-bounds slices and reject a one-byte overrun. | Small standalone objects. | `planted_oob.c` must fail exactly one property, the planted overrun; with the shim's extent check widened by one byte it passes and the target goes red. | Nightly. **PASS** at `59752f6` on Studio: valid slices `0 of 9 failed`, planted overrun `1 of 5 failed` (EXPECTED FAILURE) in both modes. Earlier Linux/Darwin controls and exact tool provenance remain in `esbmc_r_ok/results.md`. |

## Coverage gaps at inventory time

- The retained arbitrary multi-record message-reader result is an
  explicitly reduced-bound diagnostic, not a full production-bound
  proof.
- `read-write` proves the sdk's own `rocketnet_copy_string` text, cut out
  of `sdk/rocketnet.cpp` by the Makefile; the null guard it calls is a
  release-build model, so the debug assert and the misuse count are not
  covered.
- No existing harness proves every production stale-generation transition or
  every live-slot overwrite refusal named by the completion brief. A result
  may not infer those properties from a different caller's memory-safety pass.
- Tool versions are pinned in `TOOL-VERSIONS` (CBMC 6.6.0, CaDiCaL 2.1.3,
  ESBMC 8.5.0); both workflows compare the installed `--version` output
  against it and fail with TOOL-MISSING on a mismatch. No container digest
  is pinned, so a run receipt still hashes the actual host binaries.
- `replay-near-4096` is proved at the full bound, nightly: Nightly proofs run
  43162404799 (2026-11-12, at ee94e6fbf) passed all 13 partitions and the
  completeness check, 14 jobs, each partition with its reachability control
  failing as it must. The earlier HOLD (partition `nw_multi_first_word` timed
  out at CBMC_REPLAY_TIMEOUT=600; `full-bounds-partition-results.md`) is
  history. Each partition is its own target (`replay-near-4096-<partition>`)
  and its own job in nightly-proofs.yml, with its unwind derived from
  ROCKETNET_HISTORY_PACKETS, so a partition that fails is red alone; it is
  nightly, not a per-commit gate, for its run time.
- The nightly's reader and partition lists must equal `NIGHTLY-TARGETS`
  (`check_nightly_targets.sh`, run by the nightly's plan job and by ci.yml's
  lint job): a reader that drops out of the `*/proof.mk` wildcard is red.
- `harness_replay_near_nonwrapping` (proof_replay_advance.c) is run only by the
  hand script `run_replay_paths_lifo.sh`, for its `--paths lifo` measurement;
  the 13 partitions supersede it as evidence.

## Soundness rules

Every successful claim requires all of: the named source binding, explicit
object and loop bounds, unwinding assertions, satisfiable assumptions or a
reaching negative control, a successful checker verdict, and a retained raw
receipt. Cryptographic caller proofs cover memory/state behavior only. Native
tests, fuzzing, sanitizers, TLA correspondence, and production qualification
remain separate evidence.

## ACK run measurements

2026-11-12, space3 (Ubuntu 24.04, x86_64, 755 GB), ESBMC 8.5.0 (the v8.5
Linux release, and the same version built by `tools/build_pinned_esbmc.sh`
with Z3 4.13.3). Raw logs are kept with the review record of that day.

- The nightly's failure. Run 43172277595 (studio, pinned ESBMC 8.5.0,
  Z3 4.13.3): symex 3820 s, 6012 VCCs, Z3 416 s, then `ERROR: SMT solver
  failed`; the negative control the same. ESBMC's Z3 back end returns that
  for any answer other than sat or unsat, so Z3 answered unknown. It was not
  memory (384 GiB limit, no kill) and not a timeout (none set). Every night
  from 11-01 to 11-12 ended this way or was cancelled; neither target ever
  printed a verdict.
- The cause, isolated with `diag_ack_micro.c` (one static endpoint, one
  symbolic slot): writing and reading `endpoint.sent[slot]` by index, or
  through a pointer to the endpoint, solves in under a second with
  Bitwuzla. A single write through `record = &endpoint.sent[slot]` does not
  finish in 300 s with Z3 (4.13.3 pinned and the release's), and takes
  0.04 s with Bitwuzla. Two or more writes through such pointers (MICRO 5
  and 8) are killed in "Encoding" at 52 to 90 GB with Bitwuzla too. So ESBMC
  turns each write through a pointer with a symbolic offset into the 260 KB
  endpoint into a byte-level update of the whole object, and the formula
  grows by tens of GB per write whatever the solver. The production visit
  makes one such write (`record->acked = 1`) per newly acked record, plus
  reads through the same pointer.
- `diag_ack_micro.c` is a diagnostic, not a proof: it measures how ESBMC
  encodes writes into the ACK endpoint, and its asserts read back what it
  wrote. No Makefile target or workflow runs it (CBMC cannot parse it: it
  includes the C++ leaf). Run one variant by hand from proofs/cbmc with the
  pinned ESBMC, through a C++ file that includes it as the Makefile's ACK
  targets do: `printf '#include "%s"\n' "$PWD/diag_ack_micro.c" > /tmp/micro.cpp`,
  then `esbmc /tmp/micro.cpp --function harness_micro --std c++17 --z3
  -DROCKETNET_ESBMC -DNDEBUG -DROCKETNET_DEV=1 -I../../sdk --include-file
  $PWD/esbmc_shim.h -DMICRO=<1..8> -DMICRO_N=<writes>` (add
  `-DMICRO_LOCAL=1` for a local endpoint; MICRO=1 prints VERIFICATION
  SUCCESSFUL in seconds with ESBMC 8.5.0). It was named `proof_ack_micro.c`
  until 2026-11-12; a `proof_*.c` file is one a target runs.
- The harness made it worse. It wrote 7 record fields and 2 or 3 bitmap
  words per run element through pointers. It now indexes the endpoint
  directly; with that, the bound-1 symex takes 400 s.
- No bound fits. Bound 8 (original harness), Z3 and Bitwuzla: still in
  symex after 40 min at 61 GB. Bound 1 (direct-index harness, every unwind
  limited to one visit, Bitwuzla, positive and negative control): symex
  397 s / 400 s, 1399 VCCs, then `ERROR: Out of memory` at a 150 GB
  address-space limit after about 15 minutes of encoding. Earlier: 16 GB
  and 48 GB limits exhausted at a26abaf8 on vision.
- CBMC 6.6.0 cannot parse the leaf's C++17 (`PARSING ERROR` in
  `<initializer_list>` and rocketnet.h), so it is not an alternative.
- What a symbolic proof of this leaf would need: a verifier that keeps a
  pointer made from `&a[i]` as an index into `a` rather than a byte offset
  into the enclosing object. CBMC has that, but it needs a C++17 front end
  it lacks. Or the visit would have to write
  `endpoint->sent[slot].acked` by index; micro cases 1, 3 and 6 show
  ESBMC handles that form. That is an SDK change, and none is made here.

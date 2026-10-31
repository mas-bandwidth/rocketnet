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
such statement here, in the results files and in the harness comments
against the SDK, and fails the CBMC workflow when one drifts.

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
| `replay-near-4096` / `harness_replay_near_partition_completeness`, `harness_replay_near_nw_*`, `harness_replay_near_wrap_*` | CBMC 6.6.0 + CaDiCaL 2.1.3; includes current `sdk/rocketnet_replay.cpp` | For symbolic `sequence > highest_opened`, delta below the deployed 4096-bit history, and an arbitrary selected word, the production advance clears exactly the skipped ring bits, sets the received bit, and advances the high mark. Partitioned across 13 disjoint/exhaustive clear-loop-shape and selected-word relation cases (`0..12`). `highest_opened < UINT64_MAX` prevents only arithmetic wrap outside this operation's contract. | Full delta 1..4095 across the 13 disjoint/exhaustive partitions; 64 window words; global unwind 65 covers 64-word initialization and every production range-clear path. | `harness_replay_near_partition_completeness` proves pairwise disjointness, joint exhaustiveness, and witness membership (`0 of 429 failed`). All 13 concrete intended-failure controls fail solely on `replay path reachability negative control` (`1 of 431 failed`). | **HOLD** in `full-bounds-partition-results.md`: completeness (`0 of 429 failed`) and partitions 0..4 (`nw_empty_clear` and all four `nw_single_partial_*`, `0 of 431 failed` each) plus their 5 negative controls passed at full `REPLAY_DELTA_BOUND=4096, --unwind 65`; partition 5 (`nw_multi_first_word`) timed out (`HOLD`, exit 124). |
| `replay-delta-1-3` / `harness_replay_near` | CBMC 6.6.0; includes current `sdk/rocketnet_replay.cpp` | Quick per-commit reduced-bound instance of `harness_replay_near` admitting deltas 1..3. | Reduced delta 1..3 (`REPLAY_DELTA_BOUND=4`); 64 words; unwind 65. | Covered by `nw_empty_clear` and `nw_single_partial_*` concrete reachability controls. | **PASS** at `d5dd954f` (`0 of 431 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`). |
| `replay-far` / `harness_replay_far` | CBMC 6.6.0; includes current `sdk/rocketnet_replay.cpp` | For delta at least 4096, the production advance clears every word, sets only the received bit, and advances the high mark. The high mark is restricted only enough to make the subtraction defined. | Full far range; 64 words; unwind 65. | All 64 result words are asserted. | **PASS** at `d5dd954f` (`0 of 431 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`). |
| `reconstruct` | CBMC 6.6.0; copied arithmetic leaves from `sdk/rocketnet_protocol.cpp`, not the current SDK body | 32-bit and 24-bit reconstruction keeps the wire low bits, selects the documented nearest candidate, and round-trips an in-window value. | One arithmetic step; unwind 1. | Direct assertions, no loop assumptions. | **PASS** on copied harness at `d5dd954f` (`0 of 36 failed`, `VERIFICATION SUCCESSFUL`); not a source-specific proof because current SDK changed after the harness was written. |
| `ack-run-bound-8`, `ack-run-full-4096` / `harness_ack_apply_run` | ESBMC 8.5.0 C++17; includes current `sdk/rocketnet_ack.cpp` and calls its static production leaf | A symbolic in-window run has matching live records and exact `sent_written`, `sent_held`, and `sent_unacked` identities; each record's prior ack state is symbolic. Records carry no packet or aggregate id and use a negative RTT sample. The production bitmap walk must count every and only newly acked record, visit it once, retain live identity, clear unacked identity, produce no unknown/batch-full result, and update `largest_acked_sequence`. | Required run 1..8, then declared retained-window run 1..4096. Run-8 unwind 65; full unwind 4097. | `ack-run-negative-control` requires a newly unacked record and negates the observed production visit count after the call. It must fail on the named assertion. | Source-binding C++17 syntax passed at `d5dd954f`; checker executions remain behind the replay stop condition (and hit ESBMC symbolic-index struct-array copy blowup on `endpoint.sent[4096]`). |
| `read-write` | CBMC 6.6.0; current inline bodies from `sdk/rocketnet_read_write.h`; copied `rocketnet_copy_string` body from `sdk/rocketnet.cpp` | Integer and byte round trips and bounded string termination. | Eight-byte buffers; unwind 9. | Direct accepted/refused assertions. No retained planted failure. | **PASS** at `d5dd954f` (`0 of 1031 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`); copied string body is not source-specific. |
| `message-read` | CBMC 6.6.0; includes current `sdk/rocketnet_message.cpp` | Reader begin/next never reads beyond a 24-byte symbolic payload; accepted record spans fit the receive buffer; malformed input and a short fixed config body refuse. | Payload 24; unwind 8. | Accepted/refused branches are asserted. | **PASS** at `d5dd954f` (`0 of 1851 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`). |
| `xdp-free-list` | CBMC 6.6.0; includes current `sdk/rocketnet_xdp_free_list.h` | Push/pop preserves exactly the admitted frames; full, out-of-region, and non-4096-aligned pushes refuse. | Capacity 3, four operations; unwind 5. | Refusal branches and list equality asserted. | **PASS** at `d5dd954f` (`0 of 92 failed`, `VERIFICATION SUCCESSFUL` in `full-bounds-partition-results.md`); reduced capacity 3 is not the deployed frame count. |
| `fragment-bounds` | CBMC; copied former reassembly body | Small malformed-state and placement checks. | One connection/slot, fragment bytes at most 8, payload maximum at most 16; unwind 64. | Direct refusal and arena assertions. | Superseded, excluded from `all`, and not source-specific. |
| `proofs-fragment_reassembly` | ESBMC C++17; includes current `sdk/rocketnet_fragment.cpp` | Header/plan, process-fragment memory and state refusals, and overflow-safe memory sizing. | Full fragment size, but process `max_payload_bytes <= 40`; one connection/slot; unwind 16 or 33 plus library `memset` 64. | Historical planted write failures are recorded in `fragment_reassembly/results.md`. | The retained result is reduced-bound and predates current SDK. `proofs-fragment_reassembly-full` is the full target and its historical result is resource exhaustion, not pass. |
| `proofs-message_read_max` | ESBMC C++17; includes current `sdk/rocketnet_message.cpp` | Multi-record reader, full-size single record, and fixed config body parsing. | Multi-record payload only 0..24 with unwind 7; single/config payload 0..`ROCKETNET_RECEIVE_PAYLOAD_BYTES` (1416) with unwind 2/1. | Planted out-of-bounds failures are recorded in `esbmc_r_ok/results.md`. | Retained multi-record result is reduced-bound and all retained results predate current SDK. |
| `proofs-packet_open` | CBMC; includes current `sdk/rocketnet_protocol.cpp` | Full packet prefilter/open bounds, associated-data writer, and sequence nonce. Cryptographic primitive correctness is not claimed; nondeterministic crypto return values exercise caller memory and refusal paths. | Packet bytes 0..`ROCKETNET_RECEIVE_CEILING + 1` (1453) for the prefilter and 0..`ROCKETNET_RECEIVE_PACKET_BYTES` (1452) for the opener, output 0..`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` (1416); unwind 64; object bits 12. | A planted undersized allocation is retained in `esbmc_r_ok/results.md`. | Retained result predates current SDK translation-unit changes. |
| `proofs-connect_token` | ESBMC C++17; includes current `sdk/rocketnet_connect_token.cpp` | Token, signed-data and private-data readers plus caller-side verify/open consistency and identifier bounds. Cryptographic primitive correctness is not claimed. | Full 768/576/432/384-byte objects; per-harness unwind 1..577; 30-minute and 8 GiB limits. | Retained planted read/write failures are in `esbmc_r_ok/results.md`. | Retained result predates current SDK translation-unit changes. |
| `esbmc_r_ok` controls | ESBMC C/C++17; tests `esbmc_shim.h`, not product behavior | Dynamic allocation extent and pointer-offset checks reject one-byte out-of-bounds reads/writes. | Small standalone objects. | `planted_oob.c` must fail for the intended object-bound reason. | Historical checker-compatibility evidence; rerun is required after a shim or checker change. |

## Coverage gaps at inventory time

- The retained fragment-process and arbitrary multi-record message-reader
  results are explicitly reduced-bound diagnostics, not full production-bound
  proofs.
- The copied reconstruct, string-copy, and old fragment harnesses do not
  establish source correspondence. The old copied ACK leaf was removed by
  this run and replaced with the production-source harness above.
- No existing harness proves every production stale-generation transition or
  every live-slot overwrite refusal named by the completion brief. A result
  may not infer those properties from a different caller's memory-safety pass.
- Tool versions are pinned in `TOOL-VERSIONS` (CBMC 6.6.0, CaDiCaL 2.1.3,
  ESBMC 8.5.0); both workflows compare the installed `--version` output
  against it and fail with TOOL-MISSING on a mismatch. No container digest
  is pinned, so a run receipt still hashes the actual host binaries.
- HOLD: `replay-near-4096` remains HOLD at the full bound (partition
  `nw_multi_first_word` timed out; see `full-bounds-partition-results.md`).
  It is scheduled nightly via `nightly` in nightly-proofs.yml, with its
  unwind derived from ROCKETNET_HISTORY_PACKETS, and is not a per-commit
  gate until that HOLD clears.

## Soundness rules

Every successful claim requires all of: the named source binding, explicit
object and loop bounds, unwinding assertions, satisfiable assumptions or a
reaching negative control, a successful checker verdict, and a retained raw
receipt. Cryptographic caller proofs cover memory/state behavior only. Native
tests, fuzzing, sanitizers, TLA correspondence, and production qualification
remain separate evidence.

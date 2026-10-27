# Full-bound clear-loop-shape and selected-word partition result at `d5dd954f`

Verdict: **HOLD**. Implementing the disjoint and exhaustive clear-loop-shape and
selected-word relation decomposition in `proofs/cbmc/proof_replay_advance.c`
discharged the machine-checked partition completeness proof (`0 of 429 failed`,
`VERIFICATION SUCCESSFUL`), the full-domain empty-clear partition
`nw_empty_clear` (`0 of 431 failed`, `VERIFICATION SUCCESSFUL`), all four
full-domain single-partial-word partitions
(`nw_single_partial_cleared_set`, `nw_single_partial_cleared_only`,
`nw_single_partial_set_only`, and `nw_single_partial_untouched`, each `0 of 431 failed`, `VERIFICATION SUCCESSFUL`), and all five corresponding concrete
reachability negative controls (`EXPECTED FAILURE`, `1 of 431 failed` on
`replay path reachability negative control`) at the full `REPLAY_DELTA_BOUND=4096`
and `--unwind 65` bound. Execution stopped fail-closed at the sixth partition,
`nw_multi_first_word` (the first multi-word / full-word range-clear loop shape
where `while (end - begin >= 64)` can execute 1..63 symbolic-index iterations),
which timed out (exit `124`, Make exit `2`) without a solver verdict. In
 addition, the default per-commit CBMC suite (`make -C proofs/cbmc all`) was
rerun at this exact source head and passed all six targets (`replay-delta-1-3`,
`replay-far`, `reconstruct`, `read-write`, `message-read`, and `xdp-free-list`).

## Exact source and execution identity

- RocketNet `origin/main` base: `d5dd954f09d618d48aa5d897fca18f71785571db`
- Attempt 3 recovery parent merged: `d41227c7df7bd9db82dcb705f957c4a27670e2be`
- Production replay source (`sdk/rocketnet_replay.cpp`) SHA-256:
  `b107e46e6be958998f91ab7c998d0887092fa9eb1810619f28841d1f64cde4fa`
- Production ACK source (`sdk/rocketnet_ack.cpp`) SHA-256:
  `3431208e1ff9e85249a771e7d699e8621a01c6f2e43162fb0cfc8d79f34f37a9`
- Invoked replay harness (`proofs/cbmc/proof_replay_advance.c`) SHA-256:
  `f183f9886f1e704c384d5ee71aa801791f756b7ece7a9f7c3ef17b31b56149e6`
- Invoked ACK harness (`proofs/cbmc/proof_ack_apply_run.c`) SHA-256:
  `eba04db66d4a1c777e950fd75c3ffe97adc85607bf3f977df603bc99cfff503e`
- Invoked Makefile (`proofs/cbmc/Makefile`) SHA-256:
  `1bc047e0b6157f0e4bc517cd780cf4deffe0bfe6ab04cc77eba7ea4825e08cd0`
- CBMC: `6.6.0 (cbmc-6.6.0)`, `/usr/bin/cbmc`, SHA-256
  `bf215bba2dbf514c69bb32f353a5f94aea150ab87735a9e1b4d5b3bcd745058a`
- CaDiCaL: `2.1.3`, `/usr/bin/cadical`, SHA-256
  `4acf586001aa286dff6e1a390a273023c3160f0175aba2693e3e6a0065089e70`
- ESBMC: `8.5.0 64-bit x86_64 linux`, `/home/glenn/sdk/bin/esbmc`, SHA-256
  `d314db72d213859d6cd1cf9045c227489a817cb133dc14d390a5e2663c31ca0a`
- Host: `vision`, Linux x86-64 `7.0.0-38-generic #38-Ubuntu SMP PREEMPT_DYNAMIC`
- Remote scratch directory: `/tmp/emma-cbmc-test` (cleaned up after log hash
  collection; `NO_RESIDUAL_CHECKER_PROCESSES` verified).

## Partition design and machine-checked completeness

`proofs/cbmc/proof_replay_advance.c` partitions the unconstrained near-window
domain (`replay.highest_opened < UINT64_MAX`, `sequence > replay.highest_opened`,
`sequence - replay.highest_opened < 4096`, `chosen_word < 64`) into 13 cases
(`0..12`) defined by `replay_near_partition_matches(part, begin, end, set_word, chosen)` using production-typed `begin = (int) ((old_high + 1) % 4096)`,
`end = begin + (int) delta - 1`, `first_word = begin / 64`,
`last_word = (end - 1) / 64`, `wrap_last_word = (end - 4096 - 1) / 64`, and
`set_word = (int) ((sequence % 4096) / 64ULL)`:

| ID | Partition name | Clear-loop shape and selected-word relation predicate | Concrete reachability witness `(highest_opened, sequence, chosen_word)` |
| --- | --- | --- | --- |
| 0 | `nw_empty_clear` | `end <= 4096 && begin == end` (`delta == 1`; `rocketnet_replay_clear_range` returns at `begin >= end`) | `(1, 2, 0)` |
| 1 | `nw_single_partial_cleared_set` | `end <= 4096 && end > begin && first_word == last_word && (end - begin) < 64 && chosen == first_word && chosen == set_word` | `(0, 2, 0)` |
| 2 | `nw_single_partial_cleared_only` | `end <= 4096 && end > begin && first_word == last_word && (end - begin) < 64 && chosen == first_word && chosen != set_word` | `(61, 64, 0)` |
| 3 | `nw_single_partial_set_only` | `end <= 4096 && end > begin && first_word == last_word && (end - begin) < 64 && chosen != first_word && chosen == set_word` | `(61, 64, 1)` |
| 4 | `nw_single_partial_untouched` | `end <= 4096 && end > begin && first_word == last_word && (end - begin) < 64 && chosen != first_word && chosen != set_word` | `(0, 2, 1)` |
| 5 | `nw_multi_first_word` | `end <= 4096 && end > begin && !(first_word == last_word && (end - begin) < 64) && chosen == first_word` | `(0, 66, 0)` |
| 6 | `nw_multi_interior_word` | `end <= 4096 && end > begin && !(first_word == last_word && (end - begin) < 64) && chosen > first_word && chosen < last_word` | `(0, 130, 1)` |
| 7 | `nw_multi_last_word` | `end <= 4096 && end > begin && !(first_word == last_word && (end - begin) < 64) && chosen > first_word && chosen == last_word` | `(0, 66, 1)` |
| 8 | `nw_multi_outside_word` | `end <= 4096 && end > begin && !(first_word == last_word && (end - begin) < 64) && (chosen < first_word \|\| chosen > last_word)` | `(0, 66, 2)` |
| 9 | `wrap_coincident_word` | `end > 4096 && chosen >= first_word && chosen <= wrap_last_word` (both wrapped range-clear calls touch word 0 when `first_word == 0`) | `(62, 4157, 0)` |
| 10 | `wrap_high_span_only` | `end > 4096 && chosen >= first_word && chosen > wrap_last_word` | `(4094, 4097, 63)` |
| 11 | `wrap_low_span_only` | `end > 4096 && chosen < first_word && chosen <= wrap_last_word` | `(4094, 4097, 0)` |
| 12 | `wrap_outside_spans` | `end > 4096 && chosen < first_word && chosen > wrap_last_word` | `(4094, 4097, 1)` |

Before running the per-partition proofs, `replay-near-4096` executes
`harness_replay_near_partition_completeness` (`--unwind 1`,
`-DREPLAY_DELTA_BOUND=4096`), which proves in CBMC (`** 0 of 429 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) that:
1. The sum of `replay_near_partition_matches(0..12, ...)` over every symbolic
   `(highest_opened, sequence, chosen_word)` in the full `1..4095` near-window
   domain satisfies `matches >= 1` (jointly exhaustive) and `matches <= 1`
   (pairwise disjoint).
2. Every partition's concrete reachability witness `0..12` lies inside the full
   near-window domain and satisfies its own partition predicate. All 13 concrete
   reachability negative controls (`-DREPLAY_REACHABILITY_WITNESS=1`) were also
   verified on `vision` to fail solely on the intended `replay path reachability negative control` assertion (`VERIFICATION FAILED`).

## Full-bound execution receipt (`replay-near-4096`)

Command executed on `vision`:

```sh
/usr/bin/time -v make -C proofs/cbmc CBMC_REPLAY_TIMEOUT=600 replay-near-4096
```

- Start: 2026-10-26 19:52:49 UTC
- End: 2026-10-26 20:24:21 UTC
- Wall time: `31:31.23` (`1,891.23` seconds)
- User / system CPU: `1259.28` s / `35.16` s
- Maximum resident set: `2,609,368` KiB
- Swaps: `0`
- Make exit: `2` (inner timeout `124` on partition 5 `nw_multi_first_word`)
- Raw log: `replay-near-4096-partitions.log`, `673,755` bytes, SHA-256
  `c5ba2bda39fa60b248cf64c756fedb4bbeaa818a3cfd1322f8ef32f2f4c3e7dd`

Per-step results inside `replay-near-4096`:

| Step | Function | Bound / unwind | Result |
| --- | --- | --- | --- |
| Completeness & disjointness | `harness_replay_near_partition_completeness` | `delta 1..4095`, `--unwind 1` | **PASS** (`** 0 of 429 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| Partition 0 (`nw_empty_clear`) | `harness_replay_near_nw_empty_clear` | `delta 1..4095`, `--unwind 65` | **PASS** (`** 0 of 431 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| Partition 0 reachability control | `harness_replay_near_nw_empty_clear` (`-DREPLAY_REACHABILITY_WITNESS=1`) | `delta 1..4095`, `--unwind 65` | **EXPECTED FAILURE** (`** 1 of 431 failed (2 iterations)`, `VERIFICATION FAILED` on `replay path reachability negative control`) |
| Partition 1 (`nw_single_partial_cleared_set`) | `harness_replay_near_nw_single_partial_cleared_set` | `delta 1..4095`, `--unwind 65` | **PASS** (`** 0 of 431 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| Partition 1 reachability control | `harness_replay_near_nw_single_partial_cleared_set` (`-DREPLAY_REACHABILITY_WITNESS=1`) | `delta 1..4095`, `--unwind 65` | **EXPECTED FAILURE** (`** 1 of 431 failed (2 iterations)`, `VERIFICATION FAILED` on `replay path reachability negative control`) |
| Partition 2 (`nw_single_partial_cleared_only`) | `harness_replay_near_nw_single_partial_cleared_only` | `delta 1..4095`, `--unwind 65` | **PASS** (`** 0 of 431 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| Partition 2 reachability control | `harness_replay_near_nw_single_partial_cleared_only` (`-DREPLAY_REACHABILITY_WITNESS=1`) | `delta 1..4095`, `--unwind 65` | **EXPECTED FAILURE** (`** 1 of 431 failed (2 iterations)`, `VERIFICATION FAILED` on `replay path reachability negative control`) |
| Partition 3 (`nw_single_partial_set_only`) | `harness_replay_near_nw_single_partial_set_only` | `delta 1..4095`, `--unwind 65` | **PASS** (`** 0 of 431 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| Partition 3 reachability control | `harness_replay_near_nw_single_partial_set_only` (`-DREPLAY_REACHABILITY_WITNESS=1`) | `delta 1..4095`, `--unwind 65` | **EXPECTED FAILURE** (`** 1 of 431 failed (2 iterations)`, `VERIFICATION FAILED` on `replay path reachability negative control`) |
| Partition 4 (`nw_single_partial_untouched`) | `harness_replay_near_nw_single_partial_untouched` | `delta 1..4095`, `--unwind 65` | **PASS** (`** 0 of 431 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| Partition 4 reachability control | `harness_replay_near_nw_single_partial_untouched` (`-DREPLAY_REACHABILITY_WITNESS=1`) | `delta 1..4095`, `--unwind 65` | **EXPECTED FAILURE** (`** 1 of 431 failed (2 iterations)`, `VERIFICATION FAILED` on `replay path reachability negative control`) |
| Partition 5 (`nw_multi_first_word`) | `harness_replay_near_nw_multi_first_word` | `delta 1..4095`, `--unwind 65` | **HOLD** (`replay-near-4096-nw_multi_first_word HOLD timeout after 600s`, exit `124`) |
| Partitions 6..12 | `nw_multi_interior_word` .. `wrap_outside_spans` | `delta 1..4095`, `--unwind 65` | Not started after the Partition 5 blocker (concrete reachability controls verified separately). |

## Exact-head default CBMC suite receipt (`make -C proofs/cbmc all`)

Command executed on `vision` at the same exact input hashes:

```sh
/usr/bin/time -v make -C proofs/cbmc all
```

- Wall time: `11:25.59` (`685.59` seconds)
- User / system CPU: `681.54` s / `3.71` s
- Maximum resident set: `1,322,864` KiB
- Swaps: `0`
- Exit status: `0`

| Target | Bound / unwind | Result |
| --- | --- | --- |
| `replay-delta-1-3` | `delta 1..3` (`REPLAY_DELTA_BOUND=4`), `--unwind 65` | **PASS** (`** 0 of 431 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| `replay-far` | `delta >= 4096`, `--unwind 65` | **PASS** (`** 0 of 431 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| `reconstruct` | 32-bit and 24-bit wire half-ranges, `--unwind 1` | **PASS** (`** 0 of 36 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| `read-write` | 8-byte symbolic buffers, `--unwind 9` | **PASS** (`** 0 of 1031 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| `message-read` | 24-byte symbolic payload, `--unwind 8` | **PASS** (`** 0 of 1851 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |
| `xdp-free-list` | capacity 3 frames, 4 operations, `--unwind 5` | **PASS** (`** 0 of 92 failed (1 iterations)`, `VERIFICATION SUCCESSFUL`) |

## Root cause of the multi-word symbolic-index blocker

In `nw_empty_clear` (`begin == end`) and the four `nw_single_partial_*`
partitions (`first_word == last_word && (end - begin) < 64`), the production
`while (end - begin >= 64)` loop in `rocketnet_replay_clear_range` executes zero
times along every feasible execution path, so CDCL unit propagation and bounded
conflict learning in CaDiCaL discharge all 64 unrolled loop iterations and prove
all 431 verification conditions at `--unwind 65`.

Beginning at `nw_multi_first_word` (`!(first_word == last_word && (end - begin) < 64)`), the production `while (end - begin >= 64)` loop executes a symbolic
number of iterations (`1..63`) writing `replay->window[begin / 64] = 0` at a
symbolic word index `begin / 64` that depends on `(old_high + 1) % 4096` plus
the head partial-word adjustment `begin += count`. Because CBMC bit-blasts each
of the 64 unrolled loop iterations as a 64-way symbolic-index array multiplexer
over `replay.window[0..63]`, the resulting 2.14M-variable CNF formula encodes a
64-stage barrel-shifter-like network of 64-word array updates that exceeds the
per-partition CDCL solver budget without inductive loop invariants or function
contracts (`--dfcc`).

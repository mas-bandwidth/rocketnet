# Full-bound completion result at `ef0229e8`

Verdict: **HOLD**. The first required full-bound execution timed out. It is not
a proof pass, a property refutation, or an unwinding failure. Per the card's
stop rule, no smaller diagnostic was substituted and the ACK run-8, ACK
full-4096, and ACK negative-control executions were not started afterward.

## Exact source and tool identity

- RocketNet base: `ef0229e87240cadf6e3b2376d2f72782d5fd7c97`
- Production source under proof: `sdk/rocketnet_replay.cpp`, SHA-256
  `b107e46e6be958998f91ab7c998d0887092fa9eb1810619f28841d1f64cde4fa`
- Invoked harness: `proofs/cbmc/proof_replay_advance.c`, preserved at the
  remote source root below, SHA-256
  `8c5f40819e546e598aa934f4e2550f06b50113e07d2631a08c5f3cfe34f4027a`
- Invoked Makefile bytes were not preserved: the remote file was replaced by
  post-run fail-closed classification edits at 2026-10-26 11:55:48 UTC. Its
  hash is therefore unavailable. The raw log preserves the exact expanded
  CBMC command and bound used; no Makefile hash is inferred from another
  revision.
- CBMC: `5.95.1 (cbmc-5.95.1)`, `/usr/bin/cbmc`, SHA-256
  `b63e5ae6399bdde233d93f2ca4902f70bba2823a93a56cc5899425001ac91e83`
- Host kernel: Linux x86-64 `6.8.0-139-generic`, 32 logical CPUs,
  130981968 KiB physical memory. Execution held one `~/bin/benchslot`; the
  proof process itself used one CPU. No proof ran on Studio.
- There was no container. The checker was the host-installed binary above;
  this receipt therefore does not invent a container digest.

Post-run Makefile classification and prose edits did not alter the replay
command shown in the raw log. The final harness differs from the preserved
executed harness only in comments describing the quick versus full target;
the included production body and executable harness statements are unchanged.

## Command and bound

Remote source root: `/tmp/stella-cbmc-full-bounds-ef0229e8-g1`.
The retained command wrapper was:

```sh
cd /tmp/stella-cbmc-full-bounds-ef0229e8-g1
~/bin/benchslot -- bash -lc \
  'ulimit -v 16000000; /usr/bin/time -v timeout 1800 make -C proofs/cbmc replay-near-4096'
```

The target invoked CBMC with bounds, pointer, signed-overflow,
unsigned-overflow, division-by-zero and unwinding checks, global unwind 65,
`REPLAY_DELTA_BOUND=4096`, and `harness_replay_near`. Its assumptions admit
every near-window delta 1..4095 and an arbitrary one of the 64 production
history words. The harness includes `sdk/rocketnet_replay.cpp`; no model or
copy replaces the production advance or range-clear implementation.

## Result

- Start: 2026-10-26 11:24:28 UTC
- End: 2026-10-26 11:54:29 UTC
- Exit: 124 from the 1,800-second wall timeout
- Wall time: 30:00.35
- User/system CPU: 1798.26 s / 1.69 s
- Peak resident memory: 1,560,116 KiB
- Simplified verification conditions: 536 from 2,105 generated
- SAT problem at timeout: 2,139,328 variables, 7,512,035 clauses
- Solver: MiniSAT 2.2.1 with simplifier
- Swaps: 0
- Checker verdict: none before timeout

Raw log: `replay-near-4096.ef0229e8.log`, 40,348 bytes, SHA-256
`f13c2ec7df971267908d2c9c4162e73c9eaa44701b168a2994f49b705f6236cf`.
The log retains the full unwind trace, property-generation counts, solver
entry, timeout exit and `/usr/bin/time -v` resource receipt.

## Disposition against the completion contract

| Contract item | Disposition |
| --- | --- |
| Exact-head harness inventory, source binding, assumptions, bounds and retained-result audit | Partial audit recorded in `INVENTORY.md`; historical grouped results lack complete exact-head checker/property-count receipts, and the remaining reachability/full-property gaps keep this item under **HOLD**. |
| Current production replay source at full near-window boundary 1..4095 | **HOLD**: timeout 124, no checker verdict. |
| Replay wrap and near-4096 identities | Included in the symbolic full-bound input, but not proved because the solver did not return. |
| Current production ACK source, run 1..8 and full retained window 1..4096 | Harness repaired to include `sdk/rocketnet_ack.cpp`; executions not started after the first full-bound blocker. |
| Nonvacuity / bounded negative control | Source control added as `ack-run-negative-control`; not executed after the stop condition. |
| Stale-generation, live-slot overwrite, memory sizing, malformed-state/input full claims | Existing coverage and gaps inventoried; no new pass claimed. |
| CBMC/ESBMC tool-specific claims, explicit unwind/object bounds | Recorded. No ESBMC execution was claimed in this run. |
| Changed harness compilation and workflow validation | Production-source ACK wrapper passed remote Linux C++17 syntax compilation; all changed proof recipes passed GNU make dry expansion; `git diff --check` passed. Workflows are unchanged. The compiler emitted only three expected ignored-ESBMC-annotation warnings from the forced checker shim. |
| Native suites and unrelated gates | Reused/not rerun as directed; they cannot replace the missing formal verdict. |
| Product source/API/capacity/security changes | None. Cryptographic primitive correctness is not claimed. |

The source-binding compilation receipt is
`source-binding-compile.ef0229e8.log`, 907 bytes, SHA-256
`a1a97ae6123891aa71a6ab8432c36ff53448b8414b1534f3bbeee66d4ae82acd`.
It contains the complete compiler output and explicit
`ACK_SOURCE_BINDING_SYNTAX_OK` and `MAKE_DRY_OK` completion markers; GNU make
dry expansion was intentionally redirected because it includes no checker
result.

## Smallest grounded next step

A follow-up must remain in the existing `proofs/cbmc` framework and keep the
same full 1..4095 domain. The smallest source-faithful decomposition is to
split `harness_replay_near` into the production range-clear's non-wrapping and
wrapping paths while retaining the arbitrary selected word, then run both
full-domain subproperties with CBMC and its already-installed CaDiCaL external
SAT solver. That changes solver decomposition only: no bound, assumption,
production source, assertion, or property may be weakened. It is proposed
work, not a completed proof or authorization to ignore this HOLD.

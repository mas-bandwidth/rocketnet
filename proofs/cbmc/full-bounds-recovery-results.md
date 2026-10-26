# Full-bound recovery result at `b8058dc3`

Verdict: **HOLD**. The independently reviewed source-backed decomposition did
not make the first full-bound replay partition feasible within the unchanged
1,800-second wall. CBMC reached CaDiCaL on the nonwrapping partition, but the
solver returned no verdict before timeout. The wrapping partition and both
path reachability controls did not start. No smaller bound was run or reported
as success.

## Exact source and tool identity

- RocketNet production base: `b8058dc3a7bf84eb9d7899315aed1b9d62ff7f53`
- Production source: `sdk/rocketnet_replay.cpp`, SHA-256
  `b107e46e6be958998f91ab7c998d0887092fa9eb1810619f28841d1f64cde4fa`
- Invoked harness: `proofs/cbmc/proof_replay_advance.c`, SHA-256
  `f2edee6ffc63a87a1ed3232da710cc9821aea81a1e87acfd2bcb3baa6b711431`
- Invoked Makefile: SHA-256
  `1f92d781064c48aa8ee33b7951410e7f3aa8bdc0f759f1e440ac75a58b9c26bd`
- CBMC: `5.95.1 (cbmc-5.95.1)`, `/usr/bin/cbmc`, SHA-256
  `b63e5ae6399bdde233d93f2ca4902f70bba2823a93a56cc5899425001ac91e83`
- CaDiCaL: `1.7.3`, `/usr/bin/cadical`, SHA-256
  `7b73df0a6d9cf3c751a1948300e5baff8e82c4d39bcd88f0c063b5f5cfb8b33e`
- Host: Linux x86-64 `6.8.0-139-generic`, 32 logical CPUs, 130981968 KiB
  physical memory. The run held one `~/bin/benchslot`; CaDiCaL used one CPU.
  Nothing ran on Studio and there was no container.

The remote input root is
`/tmp/stella-cbmc-full-bounds-b8058dc3-w2-g1`. The three input hashes above
were checked both before upload and at that root after upload.

## Reviewed decomposition and command

The full delta 1..4095 domain is partitioned only by the exact production
branch condition. With production-typed arithmetic,
`begin = (old_high + 1) % 4096` and `end = begin + delta - 1`:

- nonwrapping: `end <= 4096`, including the edge `end == 4096`;
- wrapping: `end > 4096`.

Those predicates are disjoint and exhaustive. Both wrappers call one common
body with the original full-bound assumptions, arbitrary selected word,
current production source call and assertions. Unwind 65 remains sufficient.
Each partition also has a concrete intended-failure reachability control after
the production call and common assertions. Independent pre-edit and post-edit
reviews found no soundness blocker before execution.

The retained top-level command was:

```sh
cd /tmp/stella-cbmc-full-bounds-b8058dc3-w2-g1
~/bin/benchslot -- bash -lc \
  '/usr/bin/time -v make -C proofs/cbmc replay-near-4096'
```

The first Make invocation expanded to CBMC with all existing bounds, pointer,
signed-overflow, unsigned-overflow, division-by-zero and unwinding checks,
global unwind 65, `REPLAY_DELTA_BOUND=4096`, function
`harness_replay_near_nonwrapping`, and
`--external-sat-solver cadical`. The recipe applied a 16,000,000 KiB virtual
memory limit and the unchanged 1,800-second timeout.

## Result

- Start: 2026-10-26 12:06:06 UTC
- End: 2026-10-26 12:36:06 UTC
- Inner checker exit: 124 from the 1,800-second timeout
- Overall Make exit: 2 after classifying the inner timeout as HOLD
- Wall time: 30:00.33
- Verification conditions: 2,109 generated, 540 after simplification
- Solver: external CaDiCaL 1.7.3; no checker verdict before timeout
- Swaps: 0
- `/usr/bin/time` reported maximum resident set: 688,668 KiB. This is not
  claimed as the external solver's exact peak: a read-only
  process checkpoint at 462 seconds observed CaDiCaL itself at 1,749,812 KiB
  resident, and the timeout/reap path did not return a solver peak receipt.
- The external solver and all owned wrapper processes were absent immediately
  after the command returned.

Raw log: `replay-near-4096-split.b8058dc3.log`, 40,860 bytes, SHA-256
`901c5f2cad76503e8c4585c2788fc183e161e9ea0ce8c57909a9117a1c5ce627`.
It retains the complete unwind trace, VCC counts, external-solver entry,
timeout classification, Make exit and `/usr/bin/time -v` receipt.

## Bounded source and solver diagnosis

This run produced no counterexample, property failure, unwinding failure or
checker verdict. It therefore yielded no evidence of a source bug, but it also
did not prove the property: the blocker is an unproved resource bound, not
evidence of correctness.

The path split did not materially shrink the monolithic SAT problem. The
timed-out external-solver input was a 167,242,740-byte DIMACS file with
2,140,778 variables and 7,572,034 clauses, SHA-256
`de467f3db658d0e5119ad3e0bff0bb67f541b1c5abe36b1cdf8e2e130ed2b09d`.
The first unsplit run entered MiniSAT with 2,139,328 variables and 7,512,035
clauses. The nonwrapping predicate removed the wrapping branch, but the proof
still bit-blasts a symbolic 64-word initial ring, an arbitrary selected-word
index, modulo/index arithmetic and up to 64 iterations of the production
range-clear loop. The recovery formula was slightly larger, not smaller. This
is consistent with why changing only the SAT solver and top-level wrap branch
did not establish feasibility within the same wall. The owned temporary CNF was
hashed and then deleted from the proof host after the timeout; it is not a
committed artifact and is not recoverable from the host path.

## Contract disposition after recovery

| Contract item | Disposition |
| --- | --- |
| Replay full boundary 1..4095 | **HOLD**: the first exhaustive partition timed out with no verdict. |
| Replay nonwrapping path | **HOLD**: timeout 124 in external CaDiCaL. |
| Replay wrapping path | Not started after the first blocker. |
| Replay path nonvacuity controls | Implemented and independently reviewed; not executed after the first blocker. |
| ACK run 1..8, full 1..4096 and negative control | Not started; STOP remained at replay. |
| Reduced-bound diagnostics | None run and none claimed. |
| Product source, API, capacity, security or proof assumptions | Unchanged. |

## Smallest grounded next step

The approved production-path split has now been tried and remains infeasible
at the declared wall. No further 30-minute retry is authorized by this result.
Two existing-framework follow-ups are grounded enough for independent review,
but neither has been run:

1. use CBMC's built-in path exploration on the same two source-bound wrappers,
   retaining the same checks, unwind, full-domain union and fail-closed wall,
   so the checker does not hand one monolithic formula to a SAT solver; or
2. partition each production path by the range-clear loop shape and the
   arbitrary selected word's relation to the first partial, interior full,
   final partial, outside-cleared and `set_at` words, including coincident
   boundary cases. The reviewed predicates must be disjoint and exhaustive
   and every case must retain a reachability control.

A standalone proof of `rocketnet_replay_clear_range` is useful evidence but is
not by itself a sound compositional proof of `rocketnet_replay_advance` unless
the existing checker can consume its established contract without replacing
or assuming away the production body. This is proposed follow-up work, not a
completed proof or permission to weaken the contract.

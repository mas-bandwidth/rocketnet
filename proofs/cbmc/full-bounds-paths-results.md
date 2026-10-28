# Full-bound path-exploration result at `0928d743`

Verdict: **HOLD**. CBMC's built-in `--paths lifo` mode made substantial
progress on the same source-backed, full-domain nonwrapping replay wrapper,
but did not exhaust the path set inside the unchanged 1,800-second wall. The
checker began 3,175 bounded paths and returned UNSAT for 3,173 of them before
the timeout stopped the active path. It emitted no global verification
success, verification failure, or unwinding-failure verdict. This result is
neither a proof nor a counterexample.

## Exact source and execution identity

- Git head: `0928d743587212f7ad1d5592280ba991d70a65fb`
- Git tree: `4cd543677503bd2297dac11865ed9df3278a8dd0`
- Production replay source SHA-256:
  `b107e46e6be958998f91ab7c998d0887092fa9eb1810619f28841d1f64cde4fa`
- Harness SHA-256:
  `f2edee6ffc63a87a1ed3232da710cc9821aea81a1e87acfd2bcb3baa6b711431`
- Runner SHA-256:
  `26233e4e025653415b5d0ce1e813e119de7e7f8354c7dedfa91fbb7d4c62de07`
- CBMC: `5.95.1 (cbmc-5.95.1)`, `/usr/bin/cbmc`, SHA-256
  `b63e5ae6399bdde233d93f2ca4902f70bba2823a93a56cc5899425001ac91e83`
- Host: `hetzner2`, Linux x86-64 `6.8.0-139-generic`, 32 logical CPUs,
  130981968 KiB physical memory. The run held one `benchslot` and used one
  checker CPU.
- Remote input root:
  `/tmp/stella-cbmc-full-bounds-0928d743-w3-g2-paths`

The archived exact-head source was checked locally and on the proof host. The
production replay and harness blobs are byte-identical to the independently
reviewed recovery inputs at `a1a7be9c`; the runner is the sole proof-mechanism
addition.

## Bound and command

The wrapper calls the production `rocketnet_replay_advance` body with an
arbitrary selected history word and the complete nonwrapping delta partition
from 1 through 4095. The reviewed nonwrapping and wrapping predicates remain
disjoint and exhaustive over the full delta domain. No smaller delta, window,
word selection, or unwind was used.

```sh
/usr/bin/cbmc \
  --bounds-check --pointer-check \
  --signed-overflow-check --unsigned-overflow-check \
  --div-by-zero-check --unwinding-assertions \
  --unwind 65 -I../../sdk --paths lifo \
  -DREPLAY_DELTA_BOUND=4096 \
  --function harness_replay_near_nonwrapping \
  proof_replay_advance.c
```

The task-local runner applied the unchanged 16,000,000 KiB virtual-memory
limit and 1,800-second timeout. It recorded the head, tree, source hashes,
tool identity, command, direct checker exit and outer pipeline exit in the raw
log.

## Result

- Start: 2026-10-26 17:03:58 UTC
- End: 2026-10-26 17:33:58 UTC
- Direct checker exit: 124
- Outer runner exit: 124
- Wall: 30:00.02
- User/system CPU: 1795.79 / 4.01 seconds, 99% aggregate CPU
- Maximum resident set: 77,764 KiB
- Swaps: 0
- Paths begun: 3,175
- Paths reporting UNSAT: 3,173
- Global `VERIFICATION SUCCESSFUL`: absent
- Global `VERIFICATION FAILED`: absent
- Unwinding-assertion text: absent
- Owned checker/wrapper process after return: absent

Raw log: `replay-near-4096-paths-lifo.0928d743.log`, 1,775,332 bytes,
SHA-256
`50734b9febdf87a6e12f03d1b42bc85c57fd7f8d60e909823c7c918a04c0ea72`.

## Contract disposition and next step

The full replay boundary remains **HOLD** at the first exhaustive partition.
The wrapping partition, both replay reachability controls, and the ACK proofs
remain unstarted under the brief's stop-on-first-blocker rule. Partial per-path
UNSAT results cannot be promoted to a global proof because the checker did
not report exhaustion of the path set.

This materially different strategy used far less memory than the monolithic
formula but still exceeded the fixed wall. Repeating it unchanged would add
no evidence. The smallest grounded next design step is the already identified
disjoint/exhaustive partition by clear-loop shape and the arbitrary selected
word's relation to the first partial, interior full, final partial,
outside-cleared, and `set_at` words, including coincident-boundary cases. Each
case must retain the production call, full union of values, unwind 65, and a
reaching negative control. That design requires independent predicate review
before another expensive execution; it has not been implemented or run here.

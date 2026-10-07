# Channels TLC evidence — 2026-10-07

The final checked sources are pinned by `model-sha256.txt` (paths relative to
the repository root). `bench-manifest.txt` records the matching Linux snapshot,
Java runtime and TLC artifact. The final hashes include the bookkeeping VIEW delta. The earlier snapshot is
preserved separately in `model-sha256.pre-view.txt`; its successful full safety
log and suite summaries are explicitly marked pre-view.

Every gate uses this command from the Linux snapshot's `tla/` directory, replacing
`CONFIG` with the corresponding `.cfg` filename:

```sh
flock /tmp/stella-tlc-bench.lock timeout 1800s \
  java -XX:+UseParallelGC -Xmx6g -cp /opt/tla/tla2tools.jar tlc2.TLC \
  -workers 4 -lncheck final -cleanup -config CONFIG MCChannels
```

The positive checks must exit 0 with the complete graph explored and explicit
successful temporal completion where configured. Mutation controls must report
the expected property violation (exit 12 for an invariant, exit 13 for temporal
failure). Exit 124 denotes the per-run timeout, not success.

The two timeout logs preserve earlier two-ticket experiments. At the time,
`MCChannels.cfg` held the contents now retained in `MCChannelsPerMessage.cfg`
(six temporal branches) and `MCChannelsTwoTickets.cfg` (joint completion).
Those earlier sources fixed the two-ticket names before the final configurable
name quotient was added. Their completed graph counts agree with the final
wider safety run, but the timeout logs are historical diagnostic evidence,
not passes or an exact-final-source temporal proof.

The original exploratory tableaux and interrupted development runs are retained
in the worker job, not treated as gates. The checked reduction, finite-workload
scope, fairness, and excluded races are explained in `../../README-Channels.md`.

`main-no-view-timeout.log` is the final one-ticket model without the counter
projection, retained as `MCChannelsNoView.cfg`: its graph completed at 125,317
states, but temporal analysis timed out. `historical-no-*.log` preserve the
initial mutation witnesses replayed in the model README. Final controls have
separate logs: concurrent exploration can select different valid witnesses.

## Final snapshot results

| Config | Generated | Distinct | Exit | Log |
|---|---:|---:|---:|---|
| `MCChannels.cfg` | 387,432 | 62,007 | 0 | [main.log](main.log) |
| `MCChannelsWindow2.cfg` | 156,766 | 26,097 | 0 | [window2.log](window2.log) |
| `MCChannelsSafety.cfg` | 2,954,518 | 471,553 | 0 | [safety-two-tickets.log](safety-two-tickets.log) |
| `MCChannelsNoRetry.cfg` | 108,252 | 20,367 | 13 | [no-retry.log](no-retry.log) |
| `MCChannelsNoDedup.cfg` | 3,653 | 1,489 | 12 | [no-dedup.log](no-dedup.log) |
| `MCChannelsNoOrder.cfg` | 78 | 49 | 12 | [no-order.log](no-order.log) |

Main and window-2 include completed temporal analysis. Safety uses no fairness
or VIEW. The three nonzero exits are required injected-defect witnesses.
`final-suite.log` records process exit statuses.

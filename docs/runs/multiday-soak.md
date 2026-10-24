# Multi-day soak past 2^32 packets on one connection

Roadmap row: docs/roadmap.sexp line 222, "Multi-day soaks past 2^32 packets on one connection".

## Verdict: NOT MADE

The 72-hour run on amd-3 was not made. Holding amd-3 from the fleet for three days is an
owner decision that is owed, and it has not been given. The roadmap cell stays `n`.

A second reason the long run could not be made from this branch: `scripts/soak-multiday`
(card rn-multiday-soak-harness) has not landed on main. This branch is cut from main at
8eb03874, which has no `scripts/soak-multiday`. The harness exists only on
`sprint/rn-multiday-soak-harness.w2.g1.e1` at e23e3e7757f3c4531a7255fe341580d01673c89a.

## Runs

| run | machine | head | command | result |
| --- | --- | --- | --- | --- |
| 72 h unbounded, both directions past 0x100000000 | amd-3, held | - | `bash scripts/soak-multiday` | not made: owner decision owed (hold amd-3 72 h); harness not on main |
| harness invocation check, 90 s bounded | b1 (amd-1), not held | e23e3e77 (harness branch) | `bash scripts/soak-multiday --seconds 90 --summary-every 30 --log-dir ~/rowan-card/rn-multiday-soak-run/short1` | pass: soak exited 0, asserts 1, asserts_failed 0 |

The 90-second check proves only that the harness starts, paces at the default rate,
summarises and stops cleanly. It is not evidence for the roadmap row: it ran on an
unheld, loaded bench, at a head that is not on main, and never came near the sequence wrap.

## The 90-second check

- `date -u`: Sat Oct 24 04:24:42 AM UTC 2026
- `uptime`: up 6:14, load average 16.11, 12.75, 12.45 (shared bench, not held)
- `hostname`: b1; `uname -r`: 7.0.0-38-generic; gcc 15.2.0; loopback, no NIC
- build: `make -j4`, last line `[100%] Built target test`, zero warnings
- start line: `soak --one-connection --rate 25000 --assert-every 60 --seconds 90`
- 60 s summary: client_sent 1494872, server_sent 1494872 (about 24,900 packets per second
  each way, the expected default rate), client_highest_sent_sequence 0x176217,
  server_highest_sent_sequence 0x17587d (wrapped 0 both), asserts 1, asserts_failed 0,
  peak_resident 201916416
- soak's final summary: client_sent 2244798, server_sent 2244798, client_verified 2244762,
  server_verified 2244730, client_highest_sent_sequence 2300654,
  server_highest_sent_sequence 2296904; exit 0
- harness note: the 90 s summary repeats the 60 s packet counts because the summary reads
  soak's last `--assert-every` line; with the defaults (summary 3600 s, assert 60 s) a
  summary lags by at most one assert interval.

## Owed

- owner decision: hold amd-3 from the fleet for 72 h for this run
- rn-multiday-soak-harness landed on main, then this card restaged from that main
- then: the unbounded run, hourly summaries checked, SIGINT stop, this file rewritten with the
  run's head, command, summaries and verdict

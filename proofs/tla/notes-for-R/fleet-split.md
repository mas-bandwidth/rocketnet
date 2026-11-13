# Notes for package R from M15 (fleet-split)

Branch models/v1.1.x/fleet-split. The records of this package's own modules (COVERAGE.tsv rows
live-fleet-run and split-start-barrier, the CASES.tsv and RUNS.tsv rows of groups rnlivefleetrun and
splitstart) are already on the branch. What follows belongs to R.

## docs/roadmap.sexp: one entry (group: pick the one R uses for missing bindings)

```
  (item "live-fleet-run-binding" :group "trace-check" :area "proofs"
   :title "Bind scripts/live-xdp-fleet to RNLiveFleetRun"
   :text "The live-fleet-run row of proofs/tla/COVERAGE.tsv is owed: scripts/live-xdp-fleet and scripts/live-xdp-fleet-remote name the model's actions in shell comments, which tools/check_tla_coverage.py does not read (it reads only C-style comments), and no trace check runs the scripts. Binding them would take a shell comment reader in the coverage check and a trace of the orchestrator's steps (arm, start, ssh check, listen, client starts, teardown steps with their statuses) checked against RNLiveFleetRun, run against a local stand-in for the hosts. No defect is hidden here: the 2026-11-13 models review found two (MM205, MM206), fixed on models/v1.1.x/fleet-split.")
```

(The owed_by column of the COVERAGE row already names `live-fleet-run-binding`.)

## proofs/tla/README.md

Line 166, group table: `| rnlivefleetrun | Live fleet run lifecycle (docs/LIVE-XDP.md). |` cites a document
that no longer holds the design. New text:
`| rnlivefleetrun | Live fleet run lifecycle (scripts/live-xdp-fleet, scripts/live-xdp-fleet-remote). |`

## docs/TLA-INVENTORY.md

Line 73 stays as it is (it names no section).

## Package C (comment only, tests/load/load_split.c)

load_split.c:1705-1709, the comment above the server's population loop, cites SplitStart Connect and
ServerFail for the population window only. The model now has the lease too. New text for its last
sentence: "proofs/tla/SplitStart.tla Connect: each client connecting, from the first connect time;
ServerExpired and ServerFail: the readiness lease or the population window expiring fails the start,
and cleanup ends every connection."

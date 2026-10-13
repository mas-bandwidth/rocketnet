# TLA+ models and measured records

The model documents describe their abstractions, fairness assumptions and
counterexamples. A completed bounded model check is not a proof of the full
implementation. The model modules are unchanged by the record migration.

## Files and evidence

- `CASES.tsv` names each root `MC*.cfg` exactly once, its module, expected
  outcome/property, deadlock policy, group, gate and any declared debt.
- Each variant has its own committed configuration. The Link and
  ClientConnection documents map all 32 historical rows to their case files.
- `RUNS.tsv` contains measurements written by `tlacheck run` and joined by
  `tlacheck merge`, in the runner's 19-column layout. Never fill or edit a
  measured row by hand.
- `RUNS-HISTORY.tsv` preserves the original 32 seven-column rows unchanged
  beneath an explanatory header. Those records lack the information needed
  to reconstruct current fingerprints or timings; they are historical
  evidence, not current measurements.

The three historical ClientConnection runs that did not finish remain
explicit `bench` debt cases with their original parameters. A timed-out run
is a failed measurement, even when declared debt. No incomplete exploration
is labeled a pass. Freshness (`groups --stale` returns `[]`) means the records
match the current inputs; it does not turn a debt failure into a proof.

## Pinned runner and checker

Build `tools/tlacheck` from public `mas-bandwidth/nova-tools` commit
`0c97d5958bf2b3cb1ad24e6e1b58f5b04e70347a`, using Go 1.26.6 on Linux.
Keep that pin aligned with the CI workflow and the runner that measured the
records. Its result-producing source files are part of every fingerprint.
Use the TLC JAR with SHA-256
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.
No credential or added secret is needed to read the runner's public source.

Run Go builds and TLC on a Linux bench. The examples assume `runner` is a
checkout of that exact nova-tools revision, `job` is a private scratch
directory, `jar` is the verified JAR path and `java` is the Java executable.
From the Rocketnet checkout:

```sh
(cd "$runner" && go build -o "$job/tlacheck" ./tools/tlacheck)
"$job/tlacheck" help run
"$job/tlacheck" help groups
"$job/tlacheck" help merge
"$job/tlacheck" groups --root .
"$job/tlacheck" groups --root . --stale
"$job/tlacheck" inputs --root . --case MCLink.cfg
```

## Refreshing records after a model edit

A model change commits fresh records in the same change. A configuration or
case-row edit stales that case; a module edit stales every case that reads it,
including transitive imports. A change to the pinned runner's result code
stales every case. Do not replace fingerprints to silence a stale result.

Use `groups --stale` to identify affected groups. Run each group's cases as
individual shards so each receives its own 110-second budget. For example,
the 11 Link cases use shards 0 through 10:

```sh
for i in $(seq 0 10); do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group link --shards 11 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/link-$i"
done
"$job/tlacheck" merge --root . --keep tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/link-*/RUNS.tsv
```

Inspect each actual outcome before accepting the merge. Invariant witnesses
must exit 12 and name the declared invariant; temporal/action witnesses must
exit 13 and match their declared property. A different violation, parse error
or unexpected timeout is a finding, not an acceptable reversed witness.
Keep the raw logs and report disagreements with historical rows.

The merger checks completeness, fingerprints and one JAR across the records;
`MERGE OK` does not assert that every measurement passed. Only after reviewing
outcomes, copy the merged file to `tla/RUNS.tsv` and confirm
`groups --stale` prints `[]`. On the first migration, merge all measured
shards without `--keep`, because the old seven-column file is not compatible.
Use fresh output directories for every attempt and preserve earlier evidence.

CI builds the same pinned runner on the existing self-hosted Linux runner
labels and refuses a nonempty stale list. It checks records without running
TLC or downloading the JAR. A missing, mismatched or incomplete case plan is
also a failure.

## Case inventory

| Group | Cases | Scope |
|---|---:|---|
| `link` | 11 | All historical Link configurations |
| `clientconnection` | 21 | All historical connection configurations, including three debt cases |
| `replaywindow` | 2 | Existing control and long-skip `WindowIsTruth` witness |
| `rnsendstore` | 1 | Existing committed control |
| `rnring` | 5 | Existing committed control and four invariant witnesses |
| `rnwindow` | 7 | Existing committed safety/liveness controls and five invariant witnesses |

The 47-case plan covers every root configuration. Other experiments described
in the model READMEs remain historical; this migration does not claim fresh
measurements for uncommitted variants or change their model semantics.

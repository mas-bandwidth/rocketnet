# TLA+ models and measured records

The model documents describe their abstractions, fairness assumptions and
counterexamples. A completed bounded model check is not a proof of the full
implementation.

## Files and evidence

- `CASES.tsv` names each root `MC*.cfg` exactly once, its module, expected
  outcome/property, deadlock policy, group, gate and any declared debt.
- Each variant has its own committed configuration. The Link and
  ClientConnection documents map all 32 historical rows to their case files.
- `RUNS.tsv` contains measurements written by `tlacheck run` and joined by
  `tlacheck merge`, in the runner's 19-column layout. Never fill or edit a
  measured row by hand.
- `RUNS-HISTORY.tsv` keeps 32 seven-column rows beneath an explanatory
  header. Those records lack the information needed to reconstruct current
  fingerprints or timings; they are evidence of completed longer runs, not
  current measurements.

The ClientConnection runs that do not finish inside the budget are explicit
`bench` debt cases with their full parameters. A timed-out run is a failed
measurement, even when declared debt. No incomplete exploration
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
Keep the raw logs and report disagreements with the rows in `RUNS-HISTORY.tsv`.

The merger checks completeness, fingerprints and one JAR across the records;
`MERGE OK` does not assert that every measurement passed. Only after reviewing
outcomes, copy the merged file to `tla/RUNS.tsv` and confirm
`groups --stale` prints `[]`. Use fresh output directories for every attempt
and preserve earlier evidence.

CI builds the same pinned runner on the existing self-hosted Linux runner
labels and refuses a nonempty stale list. It checks records without running
TLC or downloading the JAR. A missing, mismatched or incomplete case plan is
also a failure. CI also checks the pinned JAR and bounded mode, and rejects
failed records unless they are explicit `bench` debt with actual exit 124.

## Case inventory

Each group is one model's `MC*.tla` instance and its committed configurations, as
`CASES.tsv` declares them. A witness is a case whose expected outcome is a violation.

| Group | Cases | Witnesses | Declared debt | Model |
|---|---:|---:|---:|---|
| `clientconnection` | 22 | 7 | 9 | `ClientConnection`: the client's connection state machine |
| `clientsession` | 3 | 2 | 0 | `ClientSession`: the client object across connections and its per-connection reset |
| `fragmentsandacks` | 24 | 13 | 7 | `FragmentsAndAcks`: fragment reassembly and the ack window |
| `link` | 11 | 2 | 0 | `Link`: the shared lossy path every model sends through |
| `receiveorder` | 4 | 3 | 0 | `ReceiveOrder`: cheap refusals before crypto on the request path |
| `replaywindow` | 2 | 1 | 0 | `ReplayWindow`: the replay window of one encryption mapping |
| `rnblocks` | 2 | 1 | 0 | `RNBlocks`: a block split, numbered, placed and delivered whole |
| `rnchannel` | 8 | 6 | 0 | `RNChannel`: the message layer of standard/MESSAGES.md |
| `rnchanneltypes` | 11 | 4 | 0 | `RNChannelTypes`: sequenced and unordered reliable message semantics |
| `rnclientio` | 8 | 6 | 0 | `RNClientIo`: the client transport with socket I/O on one I/O thread |
| `rnclientiodecrypt` | 5 | 4 | 0 | `RNClientIoDecrypt`: decryption on the client I/O thread |
| `rnclientreceivequit` | 5 | 2 | 0 | `RNClientReceiveQuit`: the client receive thread's quit-and-wake handshake with destroy, on Linux, macOS and Windows |
| `rnconnectionmemory` | 3 | 2 | 0 | `RNConnectionMemory`: per-connection message memory and the kick on exhaustion |
| `rndriversendstaging` | 2 | 1 | 0 | `RNDriverSendStaging`: the server transport's staged send path |
| `rnendpoint` | 7 | 3 | 2 | `RNEndpoint`: two message endpoints end to end, with CONFIG |
| `rnfilterepochs` | 3 | 2 | 0 | `RNFilterEpochs`: the receiver's filter epoch window |
| `rnflows` | 2 | 1 | 0 | `RNFlows`: flows on the message server |
| `rnlivefleetrun` | 3 | 2 | 0 | `RNLiveFleetRun`: the lifecycle of a live multi-machine run |
| `rnoutgoingbuild` | 4 | 3 | 0 | `RNOutgoingBuild`: the outgoing build of one connection |
| `rnreceipts` | 4 | 3 | 0 | `RNReceipts`: receipt generation |
| `rnresendtiming` | 3 | 2 | 0 | `RNResendTiming`: the resend timing of reliable records |
| `rnring` | 5 | 4 | 0 | `RNRing`: the receive ring's placement rule |
| `rnriodriver` | 3 | 2 | 0 | `RNRioDriver`: the Windows Registered I/O server transport |
| `rnscheduler` | 3 | 2 | 0 | `RNScheduler`: the order items are first written in |
| `rnsecuritytables` | 11 | 10 | 0 | `RNSecurityTables`: the security tables at the table grain |
| `rnsendbudget` | 4 | 3 | 0 | `RNSendBudget`: the per-connection send budget |
| `rnsendspans` | 3 | 2 | 0 | `RNSendSpans`: the sender's span ring in the send store |
| `rnsendstore` | 1 | 0 | 0 | `RNSendStore`: one class of the message layer's send store |
| `rnsenthistory` | 3 | 2 | 0 | `RNSentHistory`: the sent history under loss, reordering and ack delay |
| `rnsimfaults` | 3 | 2 | 0 | `RNSimFaults`: the simulation fault injector for crash, restart and clock skew |
| `rnslots` | 9 | 5 | 0 | `RNSlots`: connection tags and message views |
| `rnuringdriver` | 3 | 2 | 0 | `RNUringDriver`: the io_uring server transport |
| `rnwindow` | 7 | 5 | 0 | `RNWindow`: the reliable window |
| `rnxdpreplay` | 3 | 2 | 0 | `RNXdpReplay`: kernel replay protection with a read-only high-water map |
| `serverconnection` | 4 | 2 | 1 | `ServerConnection`: the server's slots, encryption mapping and admission |
| `serversession` | 4 | 3 | 0 | `ServerSession`: the server object, its slots across clients and the per-slot reset |

The 202-case plan covers every root configuration. Other experiments described in the
model READMEs are edits to a committed configuration, run by hand; they have no row in
`RUNS.tsv`.

## Budget debts

Every case is measured on Linux with the pinned runner and JAR above, using two workers
for controls, one for witnesses, and 110 seconds per case. A case that cannot finish
inside that budget at its model constants is declared `bench` debt in `CASES.tsv`; its
record is an actual `FAIL` with exit 124, never a pass, and the constants and checked
properties are not reduced to make it fit. Nineteen cases are declared debt; seventeen
of them exhaust the budget in `RUNS.tsv`, and `MCServerConnection.cfg` and
`MCFragmentsAndAcksPatAlternate.cfg` finish inside it.

Six of the ClientConnection debts completed as passes in the longer runs that
`RUNS-HISTORY.tsv` records. A timeout here does not refute those completed runs, and it
does not complete a new proof:

| Case | Historical outcome | Current outcome | Disposition |
|---|---|---|---|
| `MCClientConnectionLCrash.cfg` | PASS | exit 124 at the budget | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSFirst.cfg` | PASS | exit 124 at the budget | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSAlternate.cfg` | PASS | exit 124 at the budget | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSBurst.cfg` | PASS | exit 124 at the budget | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSFirstkind.cfg` | PASS | exit 124 at the budget | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSDelay.cfg` | PASS | exit 124 at the budget | Explicit timeout debt; not a completed proof |

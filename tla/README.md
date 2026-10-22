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
also a failure. CI also checks the pinned JAR and bounded mode, and rejects
failed records unless they are explicit `bench` debt with actual exit 124.

## Case inventory

| Group | Cases | Scope |
|---|---:|---|
| `link` | 11 | All historical Link configurations |
| `clientconnection` | 21 | All historical connection configurations, including nine debt cases |
| `replaywindow` | 4 | Control, mid-scale bench control, the long-skip `WindowIsTruth` witness and the open-before-gate `NoOpenBeforeConnect` witness |
| `rnsendstore` | 1 | Existing committed control |
| `rnring` | 5 | Existing committed control and four invariant witnesses |
| `rnsendspans` | 3 | Send-span ring control and two invariant witnesses |
| `rnwindow` | 7 | Existing committed safety/liveness controls and five invariant witnesses |
| `rnuringdriver` | 3 | io_uring transport control (invariants and liveness) and two invariant witnesses |
| `rnclientio` | 6 | Client I/O thread design control (invariants and liveness) and five invariant witnesses |
| `rnclientiodecrypt` | 5 | Decrypt on the client I/O thread: control (invariants and liveness) and four invariant witnesses |
| `fragmentsandacks` | 24 | Fragments and the ack window: controls, findings and witnesses, and the sent-ring bitmaps control with its two invariant witnesses, including seven debt cases |
| `rnxdpreplay` | 3 | Kernel replay high-water map control and two invariant witnesses |
| `rnriodriver` | 3 | Windows RIO server transport design: control with two liveness properties and two invariant witnesses |
| `rnlivefleetrun` | 3 | Live fleet run lifecycle (docs/LIVE-XDP.md): control with one liveness property and two invariant witnesses |
| `rnoutgoingbuild` | 4 | The outgoing build (docs/OUTGOING-BUILD.md): control with one action property and three witnesses: the copy step (`NoCopy`), an allowance of its own for in-place packets (`PacketQuota`), and no packet kept for due reliable work (`ReliableSentWithinOneUpdate`) |
| `rnsimfaults` | 3 | The DST fault injector for crash, restart and clock skew (docs/DST.md section 7): control with one liveness and one action property, and two invariant witnesses |
| `rnclientreceivequit` | 5 | The client receive thread's quit-and-wake handshake with destroy: controls for linux, macOS and Windows (two invariants, one action property, one liveness property) and two witnesses: the wake sent before quit (`WokenSeesQuit`) and a wait with no timeout and no wake (`DestroyCompletes`) |
| `rnserversendfair` | 4 | The server send pass under a payload-frame cap: controls at N=3/K=1 and N=5/K=2 (bounded service within ceil(N/K) updates) and two reversed witnesses: the pass starts at index 0 every update (`MCRNServerSendFairBrokenFromZero.cfg`) and the first client consumes the cap (`MCRNServerSendFairBrokenInPlace.cfg`), both violating `FairService` |
| `sendhandoff` | 3 | Send handoff generations: the control and reversed witnesses for reopening the front and accepting a stale pointer below the published base |

The 50-case plan covers every root configuration. Other experiments described
in the model READMEs remain historical; this migration does not claim fresh
measurements for uncommitted variants or change their model semantics.

## Initial migration measurements

All 50 cases were measured on Linux with the pinned runner and JAR above,
using two workers for controls, one for witnesses, and 110 seconds per case.
Forty-one matched their declared outcomes; nine exhausted the budget.
The three historically unfinished cases remain debt. Six formerly required
cases also timed out and are now explicit debt under the same budget rule.
Their original historical PASS rows remain unchanged in `RUNS-HISTORY.tsv`;
a timeout here does not refute those older completed runs or complete a new
proof. Model constants and checked properties were not reduced.

| Case | Historical outcome | Initial current outcome | Disposition |
|---|---|---|---|
| `MCClientConnectionLCrash.cfg` | PASS | exit 124 after 110.127 s | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSFirst.cfg` | PASS | exit 124 after 110.206 s | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSAlternate.cfg` | PASS | exit 124 after 110.237 s | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSBurst.cfg` | PASS | exit 124 after 110.235 s | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSFirstkind.cfg` | PASS | exit 124 after 110.288 s | Explicit timeout debt; not a completed proof |
| `MCClientConnectionSDelay.cfg` | PASS | exit 124 after 110.253 s | Explicit timeout debt; not a completed proof |

The following original required-case failure receipts are preserved verbatim.
Changing a debt declaration changes that case's fingerprint; `RUNS.tsv` uses
fresh runner measurements for the changed declarations, not edited hashes.
Unknown final state counts remain `-`; progress counters are not substituted.

<details>
<summary>Original required-case timeout receipts before debt declarations</summary>

```tsv
config	module	input_sha256	input_files	jar_sha256	java_version	host	cpus	started_utc	workers	generated	distinct	seconds	exit	result	expected	property	budget	mode
MCClientConnectionLCrash.cfg	MCClientConnection.tla	3f73910b8f2e61e4dc509ee3747d64f6036b0d5fa03a2aa6011794dfcf1a6d3c	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	21.0.12.1	linux-amd64	16	2026-10-13T21:28:53.193269+00:00	2	-	-	110.127	124	FAIL	pass	-	110	bounded
MCClientConnectionSFirst.cfg	MCClientConnection.tla	b99e6002be76aa61951153a8df1ed31258ca5e692b94f0e5675537e7dddea44b	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	21.0.12.1	linux-amd64	16	2026-10-13T21:33:40.281831+00:00	2	-	-	110.206	124	FAIL	pass	-	110	bounded
MCClientConnectionSAlternate.cfg	MCClientConnection.tla	fca5e159ed92563adeb7f448a6798f62949aa120861c726a1090077ea0eb8b51	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	21.0.12.1	linux-amd64	16	2026-10-13T21:35:59.966910+00:00	2	-	-	110.237	124	FAIL	pass	-	110	bounded
MCClientConnectionSBurst.cfg	MCClientConnection.tla	3105f3824721bca5c3eab18c341a7962560ece8b9016b6fc48a44828c3119b93	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	21.0.12.1	linux-amd64	16	2026-10-13T21:37:50.283718+00:00	2	-	-	110.235	124	FAIL	pass	-	110	bounded
MCClientConnectionSFirstkind.cfg	MCClientConnection.tla	ba54ef9a3896e661e05c812a7a2f05813182acefe2bb1edddada8c3b77e5e260	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	21.0.12.1	linux-amd64	16	2026-10-13T21:41:30.904207+00:00	2	-	-	110.288	124	FAIL	pass	-	110	bounded
MCClientConnectionSDelay.cfg	MCClientConnection.tla	3d964104a0b397d969f314471a8866101a2ced7fbe19be39e0462b47fe330c2c	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	21.0.12.1	linux-amd64	16	2026-10-13T21:43:21.275232+00:00	2	-	-	110.253	124	FAIL	pass	-	110	bounded
```

</details>

Final refresh: the six changed debt declarations were remeasured without
changing their models or configurations. The runner joined those fresh rows
with the 41 unchanged measurements (`MERGE OK runs=6 records=47`). All nine
budget debts remain actual `FAIL`/exit 124 records. The final freshness check
returned `[]`; adding a comment to `RNSendStore.tla` in a scratch copy returned
`["rnsendstore"]`. The pinned CI outcome check also passed on this ledger.

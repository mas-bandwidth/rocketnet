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

## Per-action citations

A coverage row proves its code file cites its model at the module grain
(`tla/<Module>.tla`). Each modelled transition must also cite its module and
action.

- A module's actions are the operators its `Next` definition reaches, followed
  through operators that are only disjunctions or existentials of other
  operators (e.g. `Next == RefuseStep \/ AdvanceStep \/ AcceptStep` reaches the
  three steps), stopping at an operator whose body primes a variable or uses
  `UNCHANGED`. Parameters and quantifiers are dropped: `Connect(s)` is
  `Connect`.
- The rule applies to every module with at least one `current` coverage row; a
  module with only `owed`, `dst`, `none` or `covered` rows is out of scope,
  and Link is excluded as the module-grain checker excludes it.
- An action is cited when one of that module's `current`-row code files has a
  comment that names `<Module>.tla` (with or without the `tla/` prefix) and
  the action name in the same comment; the mention may run over consecutive
  comment lines.
- An action that models an outside event with no code (loss, forging, a clock,
  an environment step) is exempt only when a current row's note for the module
  contains `environment actions A, B and C`; naming an action the module does
  not have is a failure.

`tools/check_tla_coverage.py --actions` prints each uncited action as
`<module> <action>: <code files>` and a closing `ACTIONS UNCITED <n>` line;
`--actions-strict` adds exit status 1 while any remain, and `--actions` is in
report mode (same exit status as without the flag).

## Pinned runner and checker

Build `tools/tlacheck` from public `mas-bandwidth/nova-tools` commit
`0c97d5958bf2b3cb1ad24e6e1b58f5b04e70347a`, using Go 1.26.6 or a later 1.26
patch on Linux (its go.mod minimum; tlacheck records no Go version). Keep the
commit pin aligned with the CI workflow and the runner that measured the
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
| `rnupdateclock` | 2 | The update clock of the client transport, the server transport and the message object (README-RNUpdateClock.md): the passing control and the reversed stall witness |
| `rnring` | 5 | Existing committed control and four invariant witnesses |
| `rnsendspans` | 3 | Send-span ring control and two invariant witnesses |
| `rnsentmap` | 3 | The sent-packet map against the window (README-RNSentMap.md): control at P = W, RTO = RTT and two invariant witnesses, a map one entry short (`NoLiveEviction`) and a resend delay below the round trip (`NoLiveEviction`) |
| `rnwindow` | 7 | Existing committed safety/liveness controls and five invariant witnesses |
| `rnuringdriver` | 3 | io_uring transport control (invariants and liveness) and two invariant witnesses |
| `rnclientio` | 24 | Client I/O thread design: the full threaded instance split by obligation (one safety case and 13 liveness cases, each one property or one conjunct of one), the step-driven mode at one slot (invariants and liveness) and its nine invariants at the full instance, six invariant witnesses, one temporal witness and the step-mode witness |
| `rnclientiodecrypt` | 5 | Decrypt on the client I/O thread: control (invariants and liveness) and four invariant witnesses |
| `fragmentsandacks` | 24 | Fragments and the ack window: controls, findings and witnesses, and the sent-ring bitmaps control with its two invariant witnesses, including seven debt cases |
| `rnxdpreplay` | 4 | Kernel replay high-water map control and three invariant witnesses |
| `rnriodriver` | 3 | Windows RIO server transport design: control with two liveness properties and two invariant witnesses |
| `rnlivefleetrun` | 3 | Live fleet run lifecycle (docs/LIVE-XDP.md): control with one liveness property and two invariant witnesses |
| `rnoutgoingbuild` | 4 | The outgoing build (docs/OUTGOING-BUILD.md): control with one action property and three witnesses: the copy step (`NoCopy`), an allowance of its own for in-place packets (`PacketQuota`), and no packet kept for due reliable work (`ReliableSentWithinOneUpdate`) |
| `rnsimfaults` | 3 | The DST fault injector for crash, restart and clock skew (docs/DST.md section 7): control with one liveness and one action property, and two invariant witnesses |
| `serversession` | 12 | Server lifecycle: the passing control (invariants and HandshakeResolves) and eleven witnesses, including NoReceiveUnlessRunning and NoAdmitUnlessRunning and the two stopping witnesses |
| `rnclientreceivequit` | 5 | The client receive thread's quit-and-wake handshake with destroy: controls for linux, macOS and Windows (two invariants, one action property, one liveness property) and two witnesses: the wake sent before quit (`WokenSeesQuit`) and a wait with no timeout and no wake (`DestroyCompletes`) |
| `rnserversendfair` | 4 | The server send pass under a payload-frame cap: controls at N=3/K=1 and N=5/K=2 (bounded service within ceil(N/K) updates) and two reversed witnesses: the pass starts at index 0 every update (`MCRNServerSendFairBrokenFromZero.cfg`) and the first client consumes the cap (`MCRNServerSendFairBrokenInPlace.cfg`), both violating `FairService` |
| `sendhandoff` | 3 | Send handoff generations: the control and reversed witnesses for reopening the front and accepting a stale pointer below the published base |
| `splitstart` | 4 | The load_split start barrier (docs/LOAD_SPLIT.md): the one-deadline shard rule as control, and reversed witnesses for the per-phase rule it replaced and the deadline without the reliable timeout term (`ServerIsTheAuthority`) and for a completed start (`NotAllRunning`) |
| `rnmsgxpause` | 2 | Shared macOS msgx pause expiry (README-RNMsgxPause.md): two workers, bounded time, fresh-pause safety and expiry progress; unconditional-clear reversed witness |
| `rnportablerx` | 6 | The portable/msgx server receive worker (README-RNPortableRx.md): controls at three and two banks (invariants and two liveness properties, no wait timeout) and four reversed witnesses: a full producer overwritten (`ExactDelivery`), a consumer returned before it is read (`ExactDelivery`), the stop wake before quit (`StopCompletes`) and no wake on a returned bank (`EveryDatagramDelivered`) |
| `rnrioworker` | 8 | The RIO transport's I/O worker (README-RNRioWorker.md): a safety control for both directions, receive and send liveness controls, a two-bank backpressure control, and four reversed witnesses: a phase reopened before it is idle (`FillPhaseOwnedByMain`), an unbounded dequeue (`ExactDelivery`), exit before the drain (`FreedOnlyWhenDrained`) and no wake on a returned bank (`EveryDatagramConsumed`) |
| `ackblockwrap` | 2 | The ack block reader across the wire sequence wrap (README-AckBlockWrap.md): the control, the empty block recognised on its wire fields at any highest sent (every invariant), and the reversed witness that makes the empty test on the reconstructed largest (`EmptyRecognised`) |

The case plan in `CASES.tsv` covers every root configuration: 409 cases
in 56 groups, 390 required and 19 bench cases whose `debt` column names
an explicit over-budget reason. Other experiments described in the model
READMEs remain historical; this plan does not claim fresh measurements
for uncommitted variants or change their model semantics.

## Migration measurements

The initial migration measured its then-current cases on Linux with the
pinned runner and JAR above, using two workers for controls, one for
witnesses, and 110 seconds per case. The cases that exhausted the budget
in that measurement are explicit bench debt in `CASES.tsv` under the
same budget rule; they are not required cases and not completed proofs.
Their original historical PASS rows remain unchanged in
`RUNS-HISTORY.tsv`; a timeout in that measurement does not refute those
older completed runs or complete a new proof. Model constants and
checked properties were not reduced. The present inventory is
`CASES.tsv` itself: `docs/testing.sexp` names each group with the
required and bench counts `CASES.tsv` gives it.

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

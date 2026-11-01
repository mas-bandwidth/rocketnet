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
(`proofs/tla/<Module>.tla`). Each modelled transition must also cite its module and
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
  comment that names `<Module>.tla` (with or without the `proofs/tla/` prefix) and
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
"$job/tlacheck" groups --root proofs
"$job/tlacheck" groups --root proofs --stale
"$job/tlacheck" inputs --root proofs --case MCLink.cfg
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
  "$job/tlacheck" run --root proofs --jar "$jar" --java "$java" \
    --group link --shards 11 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/link-$i"
done
"$job/tlacheck" merge --root proofs --keep proofs/tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/link-*/RUNS.tsv
```

Inspect each actual outcome before accepting the merge. Invariant witnesses
must exit 12 and name the declared invariant; temporal/action witnesses must
exit 13 and match their declared property. A different violation, parse error
or unexpected timeout is a finding, not an acceptable reversed witness.
Keep the raw logs and report disagreements with historical rows.

The merger checks completeness, fingerprints and one JAR across the records;
`MERGE OK` does not assert that every measurement passed. Only after reviewing
outcomes, copy the merged file to `proofs/tla/RUNS.tsv` and confirm
`groups --stale` prints `[]`. On the first migration, merge all measured
shards without `--keep`, because the old seven-column file is not compatible.
Use fresh output directories for every attempt and preserve earlier evidence.

The model checks run nightly, not per commit
([tlc.yml](../../.github/workflows/tlc.yml), daily and on workflow_dispatch).
The nightly job builds the same pinned runner on a self-hosted Linux runner,
runs every case of every stale group with the pinned JAR from
`/opt/tla/tla2tools.jar` (one shard per case, two workers, the runner's
110-second per-case limit), and fails on any case that does not reach its
declared result. A missing, mismatched or incomplete case plan is also a
failure. It then checks the committed records (`tools/check_tla_records.py`): exact
`(config, module)` bijection with `CASES.tsv`, matching `expected` and
`property`, the pinned JAR and bounded mode, the declared TLC exit code on
every `PASS` record (`0` for `pass`, `12` for `invariant`, `13` for `action`
and `temporal`) with positive state counts, and no failed record unless it is
explicit `bench` debt with actual exit 124. A night that passes with stale groups still leaves the records to
be committed from a bench run; the nightly run does not commit them.

## Case inventory

[`CASES.tsv`](CASES.tsv) is the authoritative declaration of cases, groups,
expected outcomes, gates and named debt. `tlacheck groups --root proofs`
lists the current groups; `tlacheck groups --root proofs --stale` checks
which inputs need fresh records. [`RUNS.tsv`](RUNS.tsv) holds the measured
outcomes. A declared expected pass is not a measured pass.

The following selected, non-exhaustive descriptions help locate models;
they are not a second case/count inventory. Consult `CASES.tsv` and the
model's README for its current controls, witnesses and configured bounds.

| Group | Selected scope |
|---|---|
| `link` | Shared lossy-link abstraction and loss patterns. |
| `clientconnection` | Client connection state machine, safety/liveness cases and explicit timeout debt. |
| `replaywindow` | Control, mid-scale bench control, the long-skip `WindowIsTruth` witness, the open-before-gate `NoOpenBeforeConnect` witness and the advance-before-gate `NoAdvanceBeforeConnect` witness |
| `rnsendstore` | One store class, abstractly (README-RNSendStore.md). |
| `rnsendstorelayout` | The send store and the descriptor free stack at the code's layout, three chunks and four descriptors (README-RNSendStoreLayout.md). |
| `rnupdateclock` | The update clock of the client transport, the server transport and the message object (README-RNUpdateClock.md). |
| `rnclientladder` | The client ladder at create with the family fallback (README-RNClientLadder.md). |
| `rnring` | Existing committed control and four invariant witnesses |
| `rnsendspans` | Send-span ring storage and ownership. |
| `rnsentmap` | The sent-packet map against the window (README-RNSentMap.md). |
| `rnwindow` | Abstract reliable sender/receiver window. |
| `rnuringdriver` | io_uring transport (README-RNUringDriver.md). |
| `rnclientio` | Client I/O ownership and progress in threaded and step-driven modes. |
| `rnclientiodecrypt` | Decrypt on the client I/O thread. |
| `sessionacks` | Session ACK window and retained sent-ring bitmaps. |
| `rnxdpreplay` | Kernel replay high-water map, reconnects, refused operations and held replies (README-RNXdpReplay.md). |
| `rnxdpgatewayrefresh` | The XDP server's gateway refresh and the handoff of the gateway MAC to the fill threads, a MAC in two halves (RNXdpGatewayRefresh.tla). |
| `rnriodriver` | Historical caller-driven RIO transport design (README-RNRioDriver.md); current worker behavior is RNRioWorker. |
| `rnlivefleetrun` | Live fleet run lifecycle (docs/LIVE-XDP.md). |
| `rnoutgoingbuild` | The outgoing build (docs/OUTGOING-BUILD.md). |
| `rnsimfaults` | The DST fault injector for crash, restart and clock skew (docs/DST.md section 7). |
| `serversession` | Server lifecycle, slot reuse, stopping and public connection identity. |
| `rnclientreceivequit` | The client receive thread's quit-and-wake handshake with destroy. |
| `rnserversendfair` | The server send pass under a payload-frame cap. |
| `rnservermaintenancesweep` | The server maintenance sweep at the cursor's layout (README-RNServerMaintenanceSweep.md), four slots with two unreachable. |
| `splitstart` | The load_split start barrier (docs/LOAD_SPLIT.md). |
| `rnmsgxpause` | Shared macOS msgx pause expiry (README-RNMsgxPause.md). |
| `rnportablerx` | The portable/msgx server receive worker (README-RNPortableRx.md). |
| `rnrioworker` | RIO worker receive/send bank ownership, shutdown and progress (README-RNRioWorker.md). |
| `sessionsendsequence` | The session send counter shared by fills and control sends (README-SessionSendSequence.md). |
| `ackblockwrap` | The ack block reader across the wire sequence wrap (README-AckBlockWrap.md). |
| `loopbackhub` | The loopback hub's registration lifecycle (LoopbackHub.tla). |
| `loopbackhublayout` | The loopback hub's queue layout and ring arithmetic at capacity 3 (LoopbackHubLayout.tla). |
| `rnunreliableringlayout` | The endpoint's unreliable queue at the code's layout with tickets that wrap (README-RNUnreliableRingLayout.md). |
| `rnresendfifolayout` | The resend FIFO at the code's layout, two intrusive sequences threaded through one array of W = 4 slots with packed flag bits (README-RNResendFifoLayout.md). |
| `rnresendgenerations` | The names a transport ack follows into the resend FIFO, at the slot layout with W = 4, six numbers and two descriptors, the links carrying no generation (README-RNResendGenerations.md). |
| `rnsentringlayout` | The sent-packet ring, the reference ring, the receipt ring and the receipt serial at the code's layout (README-RNSentRingLayout.md). |
| `rnsendrotation` | The server send loop over the endpoint write pass under the transport's per-flush capacity (README-RNSendRotation.md). |
| `rniocprefqueue` | The IOCP server's MPMC reference queue as laid out (RNIocpRefQueue.tla). |
| `rniocpsendlease` | The IOCP send operations' generation tags (RNIocpSendLease.tla). |
| `rniocprunring` | The IOCP USO run-ticket ring (RNIocpRunRing.tla). |
| `rniocpretrystop` | The IOCP receive retry queue and its stop-ticketed pass (RNIocpRetryStop.tla). |
| `rnportabletx` | The portable/msgx server transmit handoff at the code's own fields (README-RNPortableTx.md). |
| `rnpacketlease` | The packet lease table (README-RNPacketLease.md). |
| `rnwindowlayout` | The receive window's bitmap words and descriptor slots at the code's layout, W = 4 and a wire modulus of 16 (README-RNWindowLayout.md). |
| `rnsequencedholdpool` | The code-1 hold pool at the code's layout, two holds and two channels, the free stack and the delivered list as one array (README-RNSequencedHoldPool.md). |
| `rnmessageviews` | The message object's receive views at the code's layout, a table of three and a batch of two (README-RNMessageViews.md). |
| `rnchannelqueueslayout` | The channel FIFOs and the ready list at the code's links, three descriptors and two channels (README-RNChannelQueuesLayout.md). |
| `rnorderingwrap` | The 24-bit ordering against the 32-bit read index and the sequenced hold's reservation term, W = 2, wire modulo 8, counter modulo 32, a table of three and Du = 1 (README-RNOrderingWrap.md). |
| `rnreplaylayout` | The replay window's words at the code's layout, three words of four bits and sequences to 30 (README-RNReplayLayout.md). |
| `rnackbuildlayout` | The ack block builder's descending word pass, three words of three bits, every window (README-RNAckBuildLayout.md). |
| `rnackwalklayout` | The acknowledged-run walk over the sent ring's words, six slots in three words (README-RNAckWalkLayout.md). |
| `rnreceivedblockslayout` | Receive statistics blocks and partial-edge queries (README-RNReceivedBlocksLayout.md); bounds vary by configuration. |
| `rnrtttreelayout` | The rtt minimum tree, a history of 8 (README-RNRttTreeLayout.md). |
| `rnfreestacklayout` | The protocol tables' free stacks, capacity 3 (README-RNFreeStackLayout.md). |
| `rnreconstructlayout` | The wire sequence reconstruction, modulus 8 in a space of 32 (README-RNReconstructLayout.md). |
| `rnbankfifolayout` | The hand-written bank queues of the portable, gso, xdp, iocp and rio server transports, the buffer handoff and the iocp client at their layout (README-RNBankFifoLayout.md). |
| `rnriosendslotlayout` | The RIO send slots, reservation counter and send_ready FIFO of phases at their layout (README-RNRioSendSlotLayout.md). |
| `rnclientiocompactlayout` | The client I/O batch receive's in-place compaction at its layout (README-RNClientIoCompactLayout.md). |
| `rnxdpreplaymarkslayout` | The userspace pending buffer for the kernel replay map at the code's layout, three entries, two keys and two values (README-RNXdpReplayMarksLayout.md). |
| `rnxdpreplymacslayout` | The XDP reply MAC table's open addressing, four entries, a window of two, three addresses over every home map (README-RNXdpReplyMacsLayout.md). |
| `rndisconnectburstring` | The server transport's ring of deferred disconnect bursts at the code's layout (README-RNDisconnectBurstRing.md). |
| `rnserverreplayslots` | The per-slot records of the kernel replay walk across slot reuse, two slots and two addresses (README-RNServerReplaySlots.md). |
| `rnloopbackbindlayout` | The loopback hub's bound table and ephemeral port search at the code's layout, three records and ports 0..5 with the range 2..5 (README-RNLoopbackBindLayout.md). |
| `rnclientslotslayout` | The client slots' LIFO free stack and owner generations at the code's layout, three slots, two ids and five generations (README-RNClientSlotsLayout.md). |
| `rnsimulatorstorelayout` | The development link simulator's slot store and destination index at the code's layout, three slots, two shards and three destinations (README-RNSimulatorStoreLayout.md). |
| `rnserverpayloadringlayout` | The portable server transport's payload ring at the code's layout (README-RNServerPayloadRingLayout.md). |
| `clientgrocarry` | The gso client's GRO receive at the code's layout, rooms 1 to 4 and messages of up to three segments (README-RNClientGroCarry.md). |
| `serverstopspread` | The disconnect ring's layout and the stop spread across steps within the send room (README-ServerStopSpread.md). |
| `rnuringreaplayout` | The io_uring reap at the code's layout, a completion ring of four, three buffers and a hold ring of three (README-RNUringReapLayout.md). |
| `rnuringsendphases` | The io_uring send staging's two phases at the code's layout, phases of two slots over four updates (README-RNUringSendPhases.md). |

Experiments described outside the declared cases remain historical evidence;
this listing does not make them current measurements or alter model semantics.
Existing timeout debt remains explicit in `CASES.tsv` and `RUNS.tsv`.

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
[`CASES.tsv`](CASES.tsv) itself; [`RUNS.tsv`](RUNS.tsv) records each current
result. [Testing](../../docs/TESTING.md) explains the evidence boundaries.

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

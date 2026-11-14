# RNClientIoDecrypt

Decrypt on the client I/O thread, as `docs/CLIENT-IO-DECRYPT.md`, If decrypt
moves, designs it. The I/O thread opens each session datagram in the receive
buffer it fills, before `IoPublish`, with its own copy of the open key and its
own replay window; `update` consumes the records in arrival order and keeps
every counter, the integrity limit, the session state and `replay_session`.
The code the design changes is in `sdk/rocketnet_client_protocol.cpp`:
`rocketnet_client_open_session` (the state check, the sequence
reconstruction, `rocketnet_replay_already_received` on `replay_session`, the
open with `rocketnet_open_sealed_packet`, `rocketnet_replay_advance` right
after it), `rocketnet_client_integrity_failure` (the limit, the state change
and the zeroing of `client->keys`) and `rocketnet_client_process_challenge`
(where the session keys are derived). rocketnet-client-io-decrypt implements
the design, and `proofs/tla/COVERAGE.tsv` marks the client-io-decrypt row current.

## What is modelled

- **The receive half of RNClientIo, restated.** The two buffers and their
  Fill, Ready, Exposed and Free states, `IoReceive`, `IoPublish`, `IoOwe`,
  `IoWait`, `IoWake`, `IoClearWake`, `Expose` and `Return` are
  `proofs/tla/RNClientIo.tla`'s, restated rather than extended so the send rings and
  the stop and join are left out; each restated action's comment names the
  RNClientIo action. A buffer holds records (the datagram, its sequence and its
  verdict) instead of a count, `Return` waits until `update` has consumed every
  record of the buffer it exposed, and `snap` holds the exposed records for
  `ExposedStable`.
- **The datagrams** (`socket`, `Arrive`). The challenge, once; the server's
  genuine session sequences, in increasing order, each a keep-alive (kind
  session) or a payload (kind payload); one replayed copy of a sent sequence,
  of either kind, which is a duplicate or, once the window has passed it, too old; one forged
  session datagram with any kind, sequence and size. Reordering is the
  replayed copy. A genuine session datagram arrives only after the I/O thread
  has read the live epoch: the server sends session traffic only after the
  connection response, which `update` commits after it publishes the keys and
  the I/O thread sends after the epoch read at the top of its drain. A payload
  can therefore arrive before the keep-alive that connects the client.
- **The keys** (`epoch`, `ioKeys`, `ioSeen`). `PublishKeys` is `update`
  consuming the challenge: it stores the epoch live (release) and sets the
  session to sending the connection response, and sets the wake, which stands for the
  hand-off of the connection response that `update` commits next (RNClientIo's
  `HandOff`). The first accepted keep-alive moves the session to connected,
  and in that step `OpenPayloads` stores `payloadsOpen`: the payloads word
  the transport stores (relaxed, `rocketnet_client_io_open_payloads`) in
  `rocketnet_client_process_packet` right after the record that connected
  the client. The I/O thread does not read `payloadsOpen` itself: it reads
  its own view `ioPayloads`, which `IoSeePayloads` sets once the store is
  visible to it. The relaxed load may read 0 for a while after the store,
  since nothing orders the two, so `ioPayloads` may lag `payloadsOpen` by
  any number of steps; weak fairness on `IoSeePayloads` is the eventual
  visibility of the store (`PayloadsEventuallySeen`). A payload checked
  while the view lags passes raw after the store (`lateRaw`, the reach
  witness below).
  `RetireKeys` is disconnect, destroy or a timeout, from sending the
  connection response or connected (`rocketnet_client_protocol_disconnect`
  and every terminal `rocketnet_client_set_state` run the connection reset,
  whose `retire_keys` hook retires the copy), and the integrity limit inside
  `UpdateConsume` does the same: the epoch retired (seq_cst) before
  `update`'s own keys are zeroed, the session ended, and the wake set so a
  blocked I/O thread reads the epoch. The same step zeroes `mainWin` and
  `accWin`, as the connection reset zeroes `replay_session` and
  `accepted_session` (`sdk/rocketnet_connection_reset.h`). The I/O thread reads the epoch once at
  the top of each drain (`fresh`, set by `IoWake` and `IoClearWake`):
  `IoTakeKeys` takes the copy on live, `IoDropKeys` zeroes it and `ioWin`
  on retired (`drop_keys` zeroes `io->open`, the replay window with it,
  `sdk/rocketnet_client_io.h`), `IoKeepKeys` is a read that sees no change. It never receives or blocks
  before that read.
- **The I/O thread's rungs.** `IoCheck` runs the cheap rungs in the reading
  order, the replay check on `ioWin` last: a handshake datagram, any
  datagram while the thread holds no live copy, or a payload while its view
  `ioPayloads` is false (TRANSPORT.md t11.3), passes through with the
  verdict raw; a short datagram gets wrong size; sequence zero gets zero
  sequence; a sequence `ioWin` refuses gets the verdict replay; anything else
  is checked. `IoOpen` is the AEAD on a checked record: a genuine datagram
  opens (opened) and advances `ioWin`, a forged one does not (forged). One
  datagram is worked at a time: the next `IoReceive`, `IoPublish` and epoch
  read wait until the last record has a final verdict.
- **update's consumption** (`UpdateConsume`), one record in arrival order,
  every verdict counted with today's counter (`nCounted`): an opened record
  passes the state check, `update`'s own replay check on `mainWin` and
  `rocketnet_replay_advance` on `mainWin`, and its sequence is accepted and
  advanced into `accWin`, the `accepted_session` window the ack block is
  built from (a keep-alive on open, a payload at `accept_payload`, which runs
  in the same call while the payload acks are not deferred; the deferred
  accept and the ack block read and size check that can refuse a packet
  after the open are not modelled, so here `accWin` follows `mainWin`). The
  state check accepts a keep-alive from sending the response on and a payload
  only once connected; a payload before connected reaches `update` raw and
  is refused by the state check before any open, touching neither window
  (`earlyRefused`); a replay record is counted as already received; a
  zero-sequence or wrong-size record is refused and counted; a forged record
  whose type the state accepts gets `update`'s own replay check on `mainWin`
  first (`rocketnet_client_open_session` checks a record the I/O thread did
  not open for zero sequence and already received after the state check and
  before the verdict switch): a
  sequence `mainWin` holds is counted as already received and nothing more,
  and any other runs `rocketnet_client_integrity_failure` in its position,
  which at the limit retires the keys and ends the session; a
  raw record is processed as at BASE (a handshake record outside the handshake
  is wrong state; a session record gets the state check, the size and
  zero-sequence rungs, the replay check on `mainWin` and the AEAD on the main
  thread). A genuine payload `update` accepts from a raw record is in
  `rawAccepted`.
- **The fill threads** (`FillAckBuild`) read `accWin`, as
  `rocketnet_session_ack_block_build` reads `accepted_session` under the
  phase contract; `ackMark` is the mark read.
- **Who writes what.** `Next` tags each step with its thread, and
  `wMain`, `wCount`, `wSession` and `wIo` record the thread of the last step
  that changed `mainWin` or `accWin`, the counters, the session and `ioWin`.
- **The too-old refusal** (`tooOld`, a ghost): set when `IoCheck` gives the
  verdict replay to a sequence at or below `ioWin.mark - W`, so the reach
  witness below can name that branch.
- **The end of a behaviour** (`Done`): every datagram consumed, the socket
  empty and the I/O thread blocked. Any other state without a step is a
  deadlock, and TLC checks for it.

## Abstractions

- Each window is a mark and a set, as `proofs/tla/ReplayWindow.tla` abstracts the
  ring: a sequence at or below `mark - W` is too old, a sequence in the set has
  opened. `ReplayWindow` checks the ring against that abstraction.
- The AEAD is a predicate on the datagram: genuine opens, forged does not.
- The key copy is the boolean `ioKeys`; the open block's other inputs
  (`protocol_id`, `connection_salt`, `packet_filter`) travel with it.
- Sequence reconstruction is exact: a record holds the 64-bit sequence, so
  `update`'s second reconstruction always agrees with the I/O thread's.
- The wire floor is one short flag and the zero sequence is the sequence
  field; the ack block read and everything after the open are not modelled.
- The counters are one count of verdicts counted; integrity failures are
  `integrity`. Handshake integrity failures are not modelled (the challenge is
  genuine).
- `update` holds no key variable, so the design's order inside a retire
  (the epoch stored retired, seq_cst, before `update` zeroes its own keys) is
  not checked; the retire is one atomic step.

## Invariants

- **`CheapRefusalsFirst`**: no datagram refused by a rung before the AEAD,
  the replay check included, reached the AEAD (`cheapRefused` and `crypto`
  are disjoint), the rule of `proofs/tla/ReceiveOrder.tla`. `update`'s refusal of an
  opened record after the session has ended is not a rung before the AEAD:
  the open ran while the session was live.
- **`NoOpenWithoutKeys`**: `IoOpen` never runs without a live copy, and the
  I/O thread holds no copy once it has read retired. The first half holds by
  construction (a record is checked only while the copy is held, and the copy
  is dropped only between datagrams), so the NoRetire witness fails on the
  second half.
- **`IoWindowWithinMain`**: while the session has not ended, `mainWin` advanced by
  the sequences the I/O thread opened and `update` has not consumed yet, in
  order, contains `ioWin`: every sequence `ioWin` refuses as already
  received, that window refuses too, so the I/O thread never marks a
  sequence `update` will not accept. Before the payload gate the I/O thread
  marked a payload `update` then refused by the state check, and a later copy
  of it was dropped as already received once connected (the PayloadMark
  witness). The other direction does not hold: a payload that passed raw
  while the I/O thread's view `ioPayloads` still read 0 is accepted by
  `update` alone (`rawAccepted`), so `ioWin` never marks it; a later copy of it opens
  on the I/O thread and `update` refuses it as already received.
- **`PayloadOpenedOnlyConnected`**: the I/O thread opens a payload only once
  the session is connected (or ended), TRANSPORT.md t11.3.
- **`AcceptedOnce`**: `update` accepts a sequence at most once; while the
  session has not ended, `mainWin` and `accWin` refuse every accepted
  sequence, and `ioWin` every one not accepted from a raw record
  (`ReceivedIsRefused` for each). Once the session has ended the windows are
  zeroed and refuse nothing; `acceptedTwice` still holds the at-most-once.
- **`RetiredZeroed`**: once the session has ended `mainWin` and `accWin` are
  empty, and once the I/O thread has read retired `ioWin` is empty.
- **`SingleWriter`**: `mainWin`, `accWin`, the counters and the session
  change only in `update`'s steps, `ioWin` only in the I/O thread's.
- **`ExposedStable`** and **`NoReceiveStall`**, from RNClientIo. A stall now
  also counts the new steps that move records toward `update`: `IoCheck`,
  `IoOpen`, `UpdateConsume` and `PublishKeys`.
- **`TypeOK`**.
- **`ReplayedForgeryNotCounted`** (an action property, checked by the full
  instance and the reduced control): a forged record whose sequence `mainWin`
  already holds changes neither the integrity count nor the session. Such a
  record reaches `update`: a payload `update` accepted raw is in `mainWin` and
  not in `ioWin`, so a forgery with its sequence passes the I/O replay check.

## Liveness

Checked under weak fairness (`SpecFair`) of the I/O thread's `IoReceive`,
`IoPublish`, `IoOwe`, `IoWake`, `IoClearWake`, `IoTakeKeys`, `IoDropKeys`,
`IoKeepKeys`, `IoSeePayloads`, `IoCheck` and `IoOpen`, and of `update`'s `Expose`, `Return`,
`UpdateConsume` and `PublishKeys`: RNClientIo's fairness on the receive half,
and the same for the steps this model adds. `IoClearWake` carries no fairness in
RNClientIo; here it does, which makes this fairness stronger than RNClientIo's. `Arrive`, `RetireKeys` and
`FillAckBuild` carry none. The fairness subscript leaves out the writer tags,
which `Next` sets after the step.

- **`GenuineEventuallyAccepted`**: every genuine session sequence the server
  sends is accepted by `update`, unless `update` refused it because it arrived
  as a payload before the client connected (`earlyRefused`), or the session
  ends first.
- **`RetiredEventuallyZeroed`**: after the retire the I/O thread eventually
  reads retired and holds no copy.
- **`PayloadsEventuallySeen`**: once `update` has stored the payloads word,
  the I/O thread's view eventually reads it. This is the only bound the model
  proves on the raw transient: it ends, after finitely many steps, but the
  model sets no number on the payloads that pass raw before it does.

## How to run it

On a Linux bench, from `proofs/tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNClientIoDecryptReduced.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNClientIoDecrypt.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenSharedWindow.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenNoRetire.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenCountOnIo.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenPayloadGate.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenPayloadMark.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptReachLateView.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptReachTooOld.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptReachCountedFailure.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenNoRetireLive.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenSharedWindowLive.cfg MCRNClientIoDecrypt
```

The records come from `tlacheck run` as `proofs/tla/README.md` describes (group
`rnclientiodecrypt`, thirteen cases, one shard each). The small instance (the
reach and liveness witnesses set their own smaller constants, in each file):

| Constant | Value |
|---|---|
| `BufferCap` | 2 |
| `MaxDatagrams` | 4 (3 in `MCRNClientIoDecryptReduced.cfg`) |
| `MaxSeq` | 3 |
| `W` | 1 |
| `MaxReplays` | 1 |
| `MaxForged` | 1 |
| `IntegrityLimit` | 1 |
| `OpenBeforeReplay` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg`) |
| `SharedWindow` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenSharedWindow.cfg`) |
| `NoRetire` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenNoRetire.cfg`) |
| `CountOnIo` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenCountOnIo.cfg`) |
| `NoPayloadGate` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenPayloadGate.cfg` and `MCRNClientIoDecryptBrokenPayloadMark.cfg`) |

A window of width 1 makes both kinds of replay reachable in four datagrams:
the copy of sequence 1 is a duplicate after sequence 1 has opened and too old
after sequence 2 has. An integrity limit of 1 makes the forged datagram end
the session.

The full instance (`MCRNClientIoDecrypt.cfg`) generates 5,818,687 states and
finds 2,146,235 distinct states, depth 43, with no error: the ten invariants
hold, and `GenuineEventuallyAccepted`, `RetiredEventuallyZeroed` and
`PayloadsEventuallySeen` hold under
the fairness above (a run with no time limit, two workers, 579 s on a Linux bench).
That is over the 110-second case budget at two workers, so it is declared bench
debt in `CASES.tsv` (`full-instance-over-budget`), its record is the timeout
(exit 124), and `MCRNClientIoDecryptReduced.cfg` is the required control: the
same rules and properties with three datagrams in a behaviour, 219,480 states
generated, 90,632 distinct, depth 35, no error. In the reduced instance every
path of the list below is reachable except two (scratch runs of the reduced
constants, one negated predicate each: the duplicate, the integrity limit,
sequences 1 and 2 both accepted, the copy dropped after retired,
`earlyRefused`, zero sequence, wrong size and the retire before connected are
violated). The two it cannot reach are the too-old replay, which needs
sequences 1 and 2 and a copy of 1, and the integrity failure counted short of
the limit, which needs a limit above one. Each has its own committed reach
witness, `MCRNClientIoDecryptReachTooOld.cfg` and
`MCRNClientIoDecryptReachCountedFailure.cfg`, with every invariant of the
control checked on the way.

Reachability, checked by scratch runs (not committed) of the committed
constants under `Spec` with one negated predicate each, every one violated,
so each is a path the committed run explores: a too-old replay refused by
`IoCheck`, a duplicate refused by `IoCheck`, the integrity limit reached, a
raw forged session record opened on the main thread, sequences 1 and 2 both
accepted, the copy dropped after retired, `mainWin` behind `ioWin` with an
opened record unconsumed, an open on the I/O thread after `RetireKeys` and
before the I/O thread reads retired, a fill thread reading a nonzero mark, a
zero-sequence record refused by `IoCheck`, a wrong-size record refused by
`IoCheck`, and a `RetireKeys` while the session is still sending the
connection response, with nothing accepted and the integrity limit not
reached. Those runs predate the payload gate, which changes only the path of
a payload. For the gate, scratch runs of the committed constants under `Spec`
(module `MCReach`, not committed: `RNClientIoDecrypt` and one predicate)
give: a payload update accepted from a raw record (`rawAccepted`
nonempty), a genuine payload refused by the state check before connected
(`earlyRefused` nonempty), a payload opened on the I/O thread, and a later
copy of a raw-accepted payload opened on the I/O thread are each violated,
so reachable; a genuine keep-alive passed raw while connected is not (that
negation passes): the arrival rule above keeps genuine keep-alives off the
main thread's AEAD.

Two committed liveness witnesses show the properties are not vacuous:
`MCRNClientIoDecryptBrokenNoRetireLive.cfg` exits 13 on
`RetiredEventuallyZeroed` and `MCRNClientIoDecryptBrokenSharedWindowLive.cfg`
exits 13 on `GenuineEventuallyAccepted` (traces below). Two scratch runs, not
committed, break the properties another way: with the retire not setting the
wake, TLC exits 13 on `RetiredEventuallyZeroed` (the I/O thread blocks
holding its copy with the epoch retired, and stutters); with no fairness on
`UpdateConsume`, TLC exits 13 on `GenuineEventuallyAccepted` (a genuine
record waits in the exposed buffer forever).

## The reversed witnesses

Each fails on its declared invariant at the first state that violates any
invariant, so on no earlier one in the configuration's list.

### `MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg`

`OpenBeforeReplay = TRUE`: `IoCheck` skips the replay check and `IoOpen`
runs it after the AEAD. TLC exits 12: **`Invariant CheapRefusalsFirst is violated`** (127,246 states generated, 54,835 distinct).

- **The trace:**
  1. The challenge arrives, passes the I/O thread raw and is published;
     `update` exposes it and `PublishKeys` stores the epoch live and sets the
     wake.
  2. The I/O thread wakes and takes its copy at the top of the drain.
  3. Genuine sequence 1 arrives; the I/O thread checks it, opens it and
     advances `ioWin` to mark 1.
  4. A copy of sequence 1 arrives; the I/O thread checks it without the
     replay check, opens it (it is genuine bytes, so the AEAD succeeds), and
     only then refuses it as already received: datagram 3 is in both
     `crypto` and `cheapRefused`.
- **Against the design, by hand:** the design's table row for
  `replay_session` says the I/O thread's own window refuses "a duplicate or
  too-old sequence ... before the AEAD", and The replay window and the
  receive order says an I/O thread that opened first would spend an AEAD on
  every replayed datagram. Today's order is the same:
  `rocketnet_client_open_session` calls
  `rocketnet_replay_already_received` before `rocketnet_open_sealed_packet`.
  With the rule, step 4's `IoCheck` gives the verdict replay and the copy
  never reaches `IoOpen`.

### `MCRNClientIoDecryptBrokenSharedWindow.cfg`

`SharedWindow = TRUE`: the I/O thread's open also advances `replay_session`
(`mainWin`). TLC exits 12: **`Invariant SingleWriter is violated`** (10,413
states generated, 6,900 distinct).

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy, as
     above.
  2. Genuine sequence 1 arrives; the I/O thread checks and opens it and
     advances both `ioWin` and `mainWin` to mark 1: the last writer of
     `mainWin` is the I/O thread.
- **Against the design, by hand:** the design's table row for
  `replay_session` says "update is its only writer"; the I/O thread's write
  races `update`'s own replay check on the main thread, and that check then
  refuses sequence 1 as already received, so it is never accepted (the
  liveness witness below shows exactly that). In the design the I/O thread
  advances only its own window of the same struct; `update` advances
  `replay_session` when it consumes the record.

### `MCRNClientIoDecryptBrokenNoRetire.cfg`

`NoRetire = TRUE`: `IoDropKeys` reads retired and keeps the copy. TLC exits
12: **`Invariant NoOpenWithoutKeys is violated`** (4,034 states generated,
2,429 distinct).

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy; the
     session is still sending the connection response.
  2. `update` disconnects before the first keep-alive (`RetireKeys`): the
     epoch retired, the session ended, the wake set.
  3. The I/O thread wakes, reads retired at the top of the drain, and keeps
     its copy.
- **Against the design, by hand:** the design's table row for the open key
  says "the I/O thread zeroes its copy when it reads retired and opens
  nothing after", and `rocketnet_client_integrity_failure` zeroes
  `client->keys` at the limit today; a copy that outlives the retire keeps
  the key in memory after the attempt has ended and lets the I/O thread keep
  opening with it. With the rule, step 3 zeroes the copy and every later
  session datagram passes raw to `update`, which refuses it by the state
  check.

### `MCRNClientIoDecryptBrokenCountOnIo.cfg`

`CountOnIo = TRUE`: the I/O thread counts its own replay refusal. TLC exits
12: **`Invariant SingleWriter is violated`** (4,781 states generated, 2,975
distinct).

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy.
  2. Genuine sequence 1 arrives and opens on the I/O thread.
  3. A copy of sequence 1 arrives; `IoCheck` refuses it as already received
     before the AEAD and counts it: the last writer of the counters is the
     I/O thread.
- **Against the design, by hand:** the design's row for counters and the
  integrity limit says "the I/O thread writes no counter": the record
  carries the verdict and `update` counts it with today's counter in its
  arrival position. A counter written by both threads is a data race on the
  counter block, and a count that runs ahead of `update`'s processing order.

### `MCRNClientIoDecryptBrokenForgedReplay.cfg`

`ForgedReplayFails = TRUE`: a forged record whose sequence `mainWin` already
holds is counted as an integrity failure, the rule this model had before it
followed the code. TLC exits 13: **`Action property ReplayedForgeryNotCounted
is violated`** (1,456,929 states generated, 559,245 distinct; 30 states).

- **The trace:**
  1. The challenge, `PublishKeys`, the I/O thread takes its copy; genuine
     keep-alive 1 opens on the I/O thread (`ioWin` = {1}) and `update`
     accepts it: connected, `mainWin` = {1}.
  2. `update` stores the payloads word; the I/O thread has not read it yet
     (`ioPayloads` FALSE), so genuine payload 2 is passed raw, and a forged
     keep-alive with sequence 2 follows it into the same buffer: the I/O
     replay check on `ioWin` = {1} lets it through, the AEAD fails, verdict
     forged.
  3. `update` opens payload 2 raw and accepts it: `mainWin` = {2}, `ioWin`
     still {1}.
  4. `update` consumes the forged record: the state accepts a keep-alive and
     the old rule counts an integrity failure; at the limit of one the keys
     retire and the session ends (`integrity` = 1, `session` = "ended").
- **Against the code, by hand:** `rocketnet_client_open_session`
  (`sdk/rocketnet_client_protocol.cpp`), for a verdict other than opened,
  checks the sequence for zero and then `rocketnet_replay_already_received`
  on `replay_session` (`mainWin`) before the verdict switch; sequence 2 is
  there since step 3, so the record is counted `already_received` and the
  function returns before `rocketnet_client_integrity_failure`. The code keeps
  the session the old rule ended. With the rule, step 4 changes neither the
  integrity count nor the session.

### `MCRNClientIoDecryptBrokenPayloadGate.cfg`

`NoPayloadGate = TRUE`: `IoCheck` does not read `payloadsOpen`, so a payload
is checked and opened as soon as the I/O thread holds its copy. TLC exits 12:
**`Invariant PayloadOpenedOnlyConnected is violated`** (11,072 states
generated, 7,081 distinct).

- **The trace:**
  1. The challenge arrives, passes the I/O thread raw and is published;
     `update` exposes it and `PublishKeys` stores the epoch live, sets the
     session to sending the connection response and sets the wake.
  2. The I/O thread wakes and takes its copy at the top of the drain.
  3. Genuine sequence 1 arrives as a payload; the I/O thread checks it and
     opens it while the session is still sending the connection response,
     and advances `ioWin` to mark 1.
- **Against the design, by hand:** TRANSPORT.md t11.3 counts a payload
  received in sending connection response as wrong state, before it is
  opened. With the gate, step 3's `IoCheck` reads `payloadsOpen` false and
  gives the verdict raw; `update` refuses the raw record by the state check
  before any open, as `rocketnet_client_open_session` does at BASE. This is
  the first red unit test,
  `test_client_io_decrypt_payload_before_connected_not_opened`.

### `MCRNClientIoDecryptBrokenPayloadMark.cfg`

`NoPayloadGate = TRUE` with `PayloadOpenedOnlyConnected` left out of the
list, so the effect of the early open on the windows is what is caught. TLC
exits 12: **`Invariant IoWindowWithinMain is violated`** (111,436 states generated,
49,155 distinct).

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy, as
     above.
  2. Genuine sequence 1 arrives as a payload; the I/O thread opens it and
     advances `ioWin` to mark 1, and publishes the buffer.
  3. `update` exposes the buffer and consumes the opened payload; the
     session is still sending the connection response, so the state check
     refuses it (`earlyRefused` = {1}) and `mainWin` stays empty: `ioWin`
     refuses sequence 1 and `update`'s window does not.
- **Against the design, by hand:** once the keep-alive connects the client,
  a later copy of payload 1 would be refused by `IoCheck` as already
  received and never delivered, where BASE delivers it (its window never
  marked 1). This is the second red unit test,
  `test_client_io_decrypt_payload_duplicate_after_connect_delivered`.

### `MCRNClientIoDecryptReachLateView.cfg` (a reach witness)

Every rule on, with `NoLateRawPayload` added last to the list: it shows that
the lagging view is a behaviour the model explores, not a defect. TLC exits
12: **`Invariant NoLateRawPayload is violated`** (358,461 states generated,
141,014 distinct), with every other invariant holding on the way.

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy.
  2. Genuine keep-alive 1 arrives, opens on the I/O thread and is
     published; `update` consumes it, the session moves to connected and
     `OpenPayloads` stores `payloadsOpen`.
  3. Genuine payload 2 arrives; the I/O thread checks it while its view
     `ioPayloads` still reads false, and gives it the verdict raw after the
     store.
- **Against the code, by hand:** the store in
  `rocketnet_client_process_packet` is relaxed and the load in
  `rocketnet_client_io_decrypt_datagram` is relaxed, with nothing between
  them that orders the store first, so the load may read 0 here. The record
  reaches `update` raw, and `update`, now connected, runs the state check,
  the size and zero-sequence rungs, its replay check and the AEAD, and
  accepts it (`rawAccepted`), as BASE does with every payload.

### `MCRNClientIoDecryptReachTooOld.cfg` (a reach witness)

Every rule on, four datagrams, sequences 1 and 2, one replayed copy and no
forged datagram, with `NoTooOldRefusal` added last. TLC exits 12:
**`Invariant NoTooOldRefusal is violated`** (24,594 states generated, 9,817
distinct), with every other invariant holding on the way.

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy.
  2. Genuine payload 1 arrives; the I/O thread's view of the payloads word
     reads false, so it passes raw.
  3. Genuine keep-alive 2 arrives; the I/O thread opens it and advances
     `ioWin` to mark 2.
  4. A copy of sequence 1 arrives; 1 is at or below `2 - W`, so `IoCheck`
     gives it the verdict replay as too old before the AEAD.
- **Against the code, by hand:** `rocketnet_replay_already_received` in
  `sdk/rocketnet_replay.cpp` returns true for a sequence at or below
  `highest_opened - W` before it looks at the ring, so sequence 1 is refused
  although `io->open.replay` never marked it; `rocketnet_client_io_decrypt_datagram`
  runs that check before `rocketnet_open_sealed_packet`.

### `MCRNClientIoDecryptReachCountedFailure.cfg` (a reach witness)

Every rule on, three datagrams, one forged and an integrity limit of two, with
`NoCountedFailure` added last. TLC exits 12: **`Invariant NoCountedFailure is
violated`** (1,628 states generated, 932 distinct), with every other
invariant holding on the way.

- **The trace:**
  1. The challenge arrives; the I/O thread holds no copy yet, so the forged
     session datagram after it passes raw too, and both are published.
  2. `update` consumes the challenge (`PublishKeys`), then the raw forged
     record: the state accepts a keep-alive while sending the connection
     response, the main thread's AEAD fails, and the integrity failure is
     counted, 1 of 2; the session goes on.
- **Against the code, by hand:** `rocketnet_client_integrity_failure` in
  `sdk/rocketnet_client_protocol.cpp` counts the failure and retires the keys
  only when the count reaches the limit; below it the client keeps its
  state.

### `MCRNClientIoDecryptBrokenNoRetireLive.cfg` (a liveness witness)

`NoRetire = TRUE` under `SpecFair`, two datagrams. TLC exits 13:
**`Temporal properties were violated`** on `RetiredEventuallyZeroed` (8,847
states generated, 4,286 distinct).

- **The trace:** the challenge, `PublishKeys`, and the I/O thread takes its
  copy; `RetireKeys` ends the session and sets the wake; the I/O thread
  wakes, reads retired and keeps its copy (`ioKeys` stays true); every
  datagram is consumed and the behaviour stutters with the copy held.
- **Against the code, by hand:** `drop_keys` in `sdk/rocketnet_client_io.h`
  wipes the copy and zeroes `io->open.replay` when the drain reads retired;
  without it nothing later zeroes them.

### `MCRNClientIoDecryptBrokenSharedWindowLive.cfg` (a liveness witness)

`SharedWindow = TRUE` under `SpecFair`, two datagrams, no forged one. TLC exits
13: **`Temporal properties were violated`** on `GenuineEventuallyAccepted`
(724 states generated, 365 distinct).

- **The trace:** the challenge, `PublishKeys`, and the I/O thread takes its
  copy; genuine keep-alive 1 opens on the I/O thread, which advances both
  `ioWin` and `mainWin` to mark 1; `update` consumes the opened record and its
  own replay check on `mainWin` refuses it as already received; the session
  stays sending the connection response with nothing accepted, and the
  behaviour stutters.
- **Against the code, by hand:** `update` runs `rocketnet_replay_already_received`
  on `replay_session` for every opened record before it accepts it, so a
  window the I/O thread had already advanced would refuse every genuine
  sequence; the code keeps the I/O thread's window in `io->open.replay`.

## Findings for the implementation

- **The retire must reach a blocked I/O thread.** The design reads the epoch
  at the top of each drain, and the wait has no timeout while running except
  a guard; without a wake at the retire a blocked I/O thread keeps its copy
  until traffic arrives (the scratch run above). The model sets the wake in
  the retire; the implementation wakes the I/O thread after storing retired,
  or relies on the guard timeout, which bounds the delay only by that
  timeout.
- **Opens after the retire, within one drain.** Because the epoch is read once
  per drain, the I/O thread can open up to a buffer of datagrams with its copy
  after `update` has stored retired and before its next drain (the reachable
  state above). `update` refuses every such record by the state check, so
  nothing is accepted and `NoOpenWithoutKeys` holds as the design states it,
  but those opens are AEAD work on the key past the integrity limit. One
  acquire load of the epoch before each open removes them, at the cost of one
  load per datagram.
- **Genuine keep-alives never reach the main thread's AEAD while
  connected**, given that the keys are published before the connection
  response is handed off and the I/O thread reads the epoch at the top of
  the drain that sends it. The implementation keeps that order: publish,
  then commit the response. A payload can: one the I/O thread passed raw
  while its relaxed load still read 0 is opened by `update` once connected
  (`rawAccepted`), and each costs the AEAD on the main thread that BASE spent
  on every payload. It happens only in the window after the client connects,
  once per connection, but the code bounds that window by no count of
  payloads: the store is relaxed, and what orders it before the I/O thread's
  load is only a later release by `update` after the store that the I/O
  thread then acquires (an end-of-update handoff can skip or lose its
  try-lock, so the next one is not certain to be it). Past that, only the
  eventual visibility of a store bounds it, which is what the model proves
  (`PayloadsEventuallySeen`). The raw fallback is sound however long it
  lasts. A later copy of such a payload opens on
  the I/O thread, since its window never marked it, and `update` refuses it
  as already received, the counter BASE gives it.

## Runs

The fourteen rows are in `proofs/tla/RUNS.tsv` from `tlacheck run` on a Linux bench
(TLC 2.19, the pinned JAR): the reduced control passes with exit 0, the full
instance is the declared bench-debt timeout (exit 124), each reversed safety
witness and each reach witness exits 12 naming the declared invariant, each
liveness witness exits 13, and the forged-replay witness exits 13 naming its
action property.

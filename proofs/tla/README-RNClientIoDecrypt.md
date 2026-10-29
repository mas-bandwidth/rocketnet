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
  genuine session sequences, in increasing order; one replayed copy of a sent
  sequence, which is a duplicate or, once the window has passed it, too old;
  one forged session datagram with any sequence. Reordering is the replayed
  copy. A genuine session datagram arrives only after the I/O thread has read
  the live epoch: the server sends session traffic only after the connection
  response, which `update` commits after it publishes the keys and the I/O
  thread sends after the epoch read at the top of its drain.
- **The keys** (`epoch`, `ioKeys`, `ioSeen`). `PublishKeys` is `update`
  consuming the challenge: it stores the epoch live (release) and the session
  connected, and sets the wake, which stands for the hand-off of the
  connection response that `update` commits next (RNClientIo's `HandOff`).
  `RetireKeys` is disconnect or destroy, and the integrity limit inside
  `UpdateConsume` does the same: the epoch retired (seq_cst) before
  `update`'s own keys are zeroed, the session ended, and the wake set so a
  blocked I/O thread reads the epoch. The I/O thread reads the epoch once at
  the top of each drain (`fresh`, set by `IoWake` and `IoClearWake`):
  `IoTakeKeys` takes the copy on live, `IoDropKeys` zeroes it on retired,
  `IoKeepKeys` is a read that sees no change. It never receives or blocks
  before that read.
- **The I/O thread's rungs.** `IoCheck` runs the cheap rungs with the replay
  check on `ioWin` last: a handshake datagram, or any datagram while the
  thread holds no live copy, passes through with the verdict raw; a sequence
  `ioWin` refuses gets the verdict replay; anything else is checked.
  `IoOpen` is the AEAD on a checked record: a genuine datagram opens
  (opened) and advances `ioWin`, a forged one does not (forged). One datagram
  is worked at a time: the next `IoReceive`, `IoPublish` and epoch read wait
  until the last record has a final verdict.
- **update's consumption** (`UpdateConsume`), one record in arrival order,
  every verdict counted with today's counter (`nCounted`): an opened record
  passes the state check, `update`'s own replay check on `mainWin` and
  `rocketnet_replay_advance` on `mainWin`, and its sequence is accepted; a
  replay record is counted as already received; a forged record runs
  `rocketnet_client_integrity_failure` in its position while connected, and
  at the limit retires the keys and ends the session; a raw record is
  processed as at BASE (a handshake record outside the handshake is wrong
  state; a session record gets the state check, the replay check on
  `mainWin` and the AEAD on the main thread).
- **The fill threads** (`FillAckBuild`) read `mainWin`, as
  `rocketnet_ack_block_build` does under the phase contract; `ackMark` is the
  mark read.
- **Who writes what.** `Next` tags each step with its thread, and
  `wMain`, `wCount`, `wSession` and `wIo` record the thread of the last step
  that changed `mainWin`, the counters, the session and `ioWin`.
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
- The cheap rungs other than the replay check (wire floor, zero sequence) and
  the ack block read and everything after the open are not modelled.
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
- **`MirrorAgrees`**: while the session has not ended, `mainWin` advanced by
  the sequences the I/O thread opened and `update` has not consumed yet, in
  order, equals `ioWin`. The two windows are one function of one sequence
  stream, `update`'s lagging by the unconsumed records; so `update`'s replay
  check never refuses an opened record while connected.
- **`AcceptedOnce`**: `update` accepts a sequence at most once, and both
  windows refuse every accepted sequence (`ReceivedIsRefused` for each).
- **`SingleWriter`**: `mainWin`, the counters and the session change only in
  `update`'s steps, `ioWin` only in the I/O thread's.
- **`ExposedStable`** and **`NoReceiveStall`**, from RNClientIo. A stall now
  also counts the new steps that move records toward `update`: `IoCheck`,
  `IoOpen`, `UpdateConsume` and `PublishKeys`.
- **`TypeOK`**.

## Liveness

Checked under weak fairness (`SpecFair`) of the I/O thread's `IoReceive`,
`IoPublish`, `IoOwe`, `IoWake`, `IoClearWake`, `IoTakeKeys`, `IoDropKeys`,
`IoKeepKeys`, `IoCheck` and `IoOpen`, and of `update`'s `Expose`, `Return`,
`UpdateConsume` and `PublishKeys`: RNClientIo's fairness on the receive half,
and the same for the steps this model adds. `IoClearWake` carries no fairness in
RNClientIo; here it does, which makes this fairness stronger than RNClientIo's. `Arrive`, `RetireKeys` and
`FillAckBuild` carry none. The fairness subscript leaves out the writer tags,
which `Next` sets after the step.

- **`GenuineEventuallyAccepted`**: every genuine session sequence the server
  sends is accepted by `update`, unless the session ends first.
- **`RetiredEventuallyZeroed`**: after the retire the I/O thread eventually
  reads retired and holds no copy.

## How to run it

On a Linux bench, from `proofs/tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNClientIoDecrypt.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenSharedWindow.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenNoRetire.cfg MCRNClientIoDecrypt
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoDecryptBrokenCountOnIo.cfg MCRNClientIoDecrypt
```

The records come from `tlacheck run` as `proofs/tla/README.md` describes (group
`rnclientiodecrypt`, five cases, one shard each). The small instance:

| Constant | Value |
|---|---|
| `BufferCap` | 2 |
| `MaxDatagrams` | 4 |
| `MaxSeq` | 3 |
| `W` | 1 |
| `MaxReplays` | 1 |
| `MaxForged` | 1 |
| `IntegrityLimit` | 1 |
| `OpenBeforeReplay` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg`) |
| `SharedWindow` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenSharedWindow.cfg`) |
| `NoRetire` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenNoRetire.cfg`) |
| `CountOnIo` | `FALSE` (`TRUE` in `MCRNClientIoDecryptBrokenCountOnIo.cfg`) |

A window of width 1 makes both kinds of replay reachable in four datagrams:
the copy of sequence 1 is a duplicate after sequence 1 has opened and too old
after sequence 2 has. An integrity limit of 1 makes the forged datagram end
the session.

The committed run generates 268,589 states and finds 113,876 distinct states,
depth 42, with no error: the eight invariants hold, and
`GenuineEventuallyAccepted` and `RetiredEventuallyZeroed` hold under the
fairness above (30.6 s under `tlacheck run`, two workers).

Reachability, checked by scratch runs (not committed) of the committed
constants under `Spec` with one negated predicate each, every one violated,
so each is a path the committed run explores: a too-old replay refused by
`IoCheck` (depth 23), a duplicate refused by `IoCheck` (17), the integrity
limit reached (12), a raw forged session record opened on the main thread
(12), sequences 1 and 2 both accepted (23), the copy dropped after retired
(11), `mainWin` behind `ioWin` with an opened record unconsumed (14) and with
two (18), an open on the I/O thread after `RetireKeys` and before the I/O
thread reads retired (15), and a fill thread reading a nonzero mark (19). A
raw genuine session record while connected is not reachable (the scratch run
of that negation passes): the arrival rule above is what keeps genuine
traffic off the main thread's AEAD.

Two scratch runs, not committed, show the liveness properties are not
vacuous: with the retire not setting the wake, TLC exits 13 on
`RetiredEventuallyZeroed` (the I/O thread blocks holding its copy with the
epoch retired, and stutters); with no fairness on `UpdateConsume`, TLC exits
13 on `GenuineEventuallyAccepted` (a genuine record waits in the exposed
buffer forever).

## The reversed witnesses

Each fails on its declared invariant at the first state that violates any
invariant, so on no earlier one in the configuration's list.

### `MCRNClientIoDecryptBrokenOpenBeforeReplay.cfg`

`OpenBeforeReplay = TRUE`: `IoCheck` skips the replay check and `IoOpen`
runs it after the AEAD. TLC exits 12: **`Invariant CheapRefusalsFirst is violated`**, at depth 18 (9,772 states generated, 4,484 distinct).

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
(`mainWin`). TLC exits 12: **`Invariant SingleWriter is violated`**, at depth
14 (1,518 states generated, 908 distinct).

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy, as
     above.
  2. Genuine sequence 1 arrives; the I/O thread checks and opens it and
     advances both `ioWin` and `mainWin` to mark 1: the last writer of
     `mainWin` is the I/O thread.
- **Against the design, by hand:** the design's table row for
  `replay_session` says "update is its only writer", read by the fill
  threads through `rocketnet_ack_block_build` under the phase contract; the
  I/O thread is outside that contract, so its write races the fill threads'
  reads, and the ack block would name sequence 1 before `update` has
  processed it. In the design the I/O thread advances only its own window of
  the same struct; `update` advances `replay_session` when it consumes the
  record.

### `MCRNClientIoDecryptBrokenNoRetire.cfg`

`NoRetire = TRUE`: `IoDropKeys` reads retired and keeps the copy. TLC exits
12: **`Invariant NoOpenWithoutKeys is violated`**, at depth 13 (805 states
generated, 480 distinct).

- **The trace:**
  1. The challenge, `PublishKeys`, and the I/O thread takes its copy.
  2. `update` disconnects (`RetireKeys`): the epoch retired, the session
     ended, the wake set.
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
12: **`Invariant SingleWriter is violated`**, at depth 17 (6,665 states
generated, 3,090 distinct).

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
- **Genuine traffic never reaches the main thread's AEAD while connected**,
  given that the keys are published before the connection response is handed
  off and the I/O thread reads the epoch at the top of the drain that sends
  it. The implementation keeps that order: publish, then commit the response.

## Runs

The five rows are in `proofs/tla/RUNS.tsv` from `tlacheck run` on a Linux bench
(TLC 2.19, the pinned JAR): the control passes with exit 0, and each witness
exits 12 naming its declared invariant.

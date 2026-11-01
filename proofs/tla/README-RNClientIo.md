# RNClientIo

**Shutdown scope:** This model retains its earlier synchronous shutdown
abstraction and exact historical evidence. The approved request/completion API
is separately modeled in [RNClientIoShutdown](README-RNClientIoShutdown.md).
That additive ownership model is not a proved refinement of this one; existing
data-path/session checks do not establish the new shutdown implementation.

The client I/O core is implemented in `sdk/rocketnet_client_io.h`:
`rocketnet_client_io_init`, `rocketnet_client_io_hand_off`,
`rocketnet_client_io_expose`, `rocketnet_client_io_return` and
`rocketnet_client_io_step`. `proofs/tla/COVERAGE.tsv` maps the retained
actions to that source. This model preserves the earlier two-send-ring and
two-receive-buffer design; the current N-buffer ownership exchange is
modeled separately by [RNBufferHandoff](README-RNBufferHandoff.md).
[Client I/O](../../docs/CLIENT-IO.md) describes the current implementation.
The historical traces below that refer to "the code at the base" describe
the implementation before send offload, not today's source.

- **The two send rings.** Game threads write slots of the ring that is
  Filling (`Write`, then `Commit`). A hand-off (`send_packets`, `send_packet`
  through it, or the end of `update`), with the fills joined and at least
  one slot committed, either hands the ring to the I/O thread, sets the wake
  and switches to the other ring when that ring is open (`HandOff`), or,
  when the other ring is still HandedOff or Sending, defers: the committed
  slots stay in the Filling ring and `CLIENT_SEND_DEFERRED` moves (`Defer`,
  the flag `deferred`). A hand-off never waits. The I/O thread sends each
  slot (`IoSend`, the ring Sending) and then releases the ring
  (`IoFinishRing`, Sent). Only a Sent ring is reopened (`Reopen`). `WaitDone`
  and `mainWait` exist only for the defect `HandOffWaits`.
- **The receive handoff.** The I/O thread reads a datagram into its Fill
  buffer while it has room (`IoReceive`, at most `BufferCap`) and, when a
  Free buffer is there, publishes the Fill buffer as Ready and takes the Free
  one to fill (`IoPublish`). With no Free buffer it records the publish as
  owed (`IoOwe`) before it may block. `update` is two steps: at its start it
  exposes the Ready buffer when there is one (`Expose`), and at its end,
  every update, it returns the buffer it holds as Free and wakes the I/O
  thread when the owed flag is set (`Return`). Each action is one atomic
  step, which is the seq_cst store-then-load pairing of `docs/CLIENT-IO.md`.
- **The wake.** `IoWait` blocks only when nothing is visible to do: never
  while a publish is possible, with an unpublished batch only once it is
  owed, and with datagrams in the socket only when the buffer is full. It
  records the wait's `readable` argument as `waitReadable`: true while the
  buffer has room, false while it is full. `IoWake` returns on the wake, or
  on a readable socket only when `waitReadable`, and consumes the wake;
  `IoClearWake` consumes a wake a running thread does not need. The wake is
  level-triggered, so the check and the wait are one step in the model and
  sound in code: a wake set between them makes the wait return at once.
- **Stop.** `Stop` (`rocketnet_client_transport_stop`, through
  `rocketnet_client_io_stop`) runs with the fills joined and never waits: the
  Filling ring's committed slots pass to the I/O thread as the final ring
  (`final`, `send_final` in the code), bound to their destination, then the
  phase is Stopping and the wake is set. The I/O thread sends every
  handed-off ring, then the final ring, and exits (`IoExit`); destroy joins
  (`Join`) and frees (`Free`). `ExitDropsFinal` is the defect that exits
  without sending the final ring.

## Invariants

- **`NoReuseBeforeSent`**: a slot a game thread is writing belongs to a ring
  that is Filling and that the I/O thread is not sending.
- **`ExposedStable`**: the exposed receive buffer's contents equal what
  `update` exposed at the last `Expose`, until `Return`.
- **`StopSendsAll`**: once the I/O thread has exited, every slot it was
  handed has been sent, the final ring's included: stop drops nothing.
- **`FreedOnlyAfterJoin`**: once freed, the I/O thread has exited and holds
  no ring.
- **`HandOffNeverWaits`**: the main thread is never blocked on the I/O
  thread (`mainWait` stays false).
- **`FullWaitsOnWake`**: a blocked I/O thread whose buffer is full waits on
  the wake alone (`waitReadable` is false).
- **`NoReceiveStall`**: while the transport runs, datagrams in the I/O
  thread's Fill buffer always have a next step toward `update`: `IoPublish`,
  `Expose`, `Return` or `IoWake` is enabled.
- **`TypeOK`**: every variable has its declared type.

`HandOffNeverWaits` and `FullWaitsOnWake` state the two rules directly; what
their witnesses show is that the defect each rule forbids is reached on the
common path in seven and eight steps, not only under overload. A spin is not
a liveness violation (the thread keeps returning, and `Return` eventually
frees a buffer), which is why the full-buffer rule is an invariant.

## Liveness

Checked under weak fairness of the I/O thread's `IoSend`, `IoFinishRing`,
`IoReceive`, `IoPublish`, `IoOwe`, `IoWake` and `IoExit`, of `Join`, of a
game thread's `Commit` of a packet it started, of `Reopen` and `HandOff`,
and of `update`'s `Expose` and `Return` (the game calls update every frame,
and update ends with a hand-off) (`SpecFair`):

- **`HandedOffEventuallySent`**: a handed-off ring is eventually Sent.
- **`CommittedEventuallyHandedOff`**: a committed slot, deferred or not, is
  eventually handed to the I/O thread, by a hand-off or by stop.
- **`StopEventuallyJoins`**: after `Stop` the thread is eventually joined.
- **`FilledEventuallyExposed`**: a Fill buffer holding datagrams is
  eventually Exposed, unless the transport stops.
- **`ArrivedEventuallyDelivered`**: every datagram that arrives is
  eventually exposed to `update`, unless the transport stops.

## What the model leaves out

- Slots, datagrams and buffers are states and counts; packet bytes, the
  wire, the clock callback and the decrypt are not modelled.
- The deferred counter is the flag `deferred`.
- A release and acquire implementation of the owed flag (weaker than
  seq_cst) is outside the model: its atomic steps are the seq_cst pairing.
- `update`'s protocol packet and the disconnect burst are slots committed and
  handed off like payloads, so they are `Write`, `Commit` and `HandOff` or
  `Defer`.

## How to run it

On a Linux bench, from `proofs/tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNClientIoSafety.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNClientIoLiveFilled0.cfg MCRNClientIoLiveness
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenReuseBeforeSent.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenSwapWhileReading.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenReceiveStall.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenHandOffWaits.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenFullWaitReadable.cfg MCRNClientIo
```

The records come from `tlacheck run` as `proofs/tla/README.md` describes (group
`rnclientio`). The small instance:

| Constant | Value |
|---|---|
| `NumSlots` | 2 |
| `BufferCap` | 1 (2 in `MCRNClientIoBrokenSwapWhileReading.cfg`) |
| `MaxDatagrams` | 3 |
| `ReopenBeforeSent` | `FALSE` (`TRUE` in `MCRNClientIoBrokenReuseBeforeSent.cfg`) |
| `ReceiveIntoExposed` | `FALSE` (`TRUE` in `MCRNClientIoBrokenSwapWhileReading.cfg`) |
| `SwapNeedsReady` | `FALSE` (`TRUE` in `MCRNClientIoBrokenReceiveStall.cfg`) |
| `HandOffWaits` | `FALSE` (`TRUE` in `MCRNClientIoBrokenHandOffWaits.cfg`) |
| `FullWaitReadable` | `FALSE` (`TRUE` in `MCRNClientIoBrokenFullWaitReadable.cfg`) |

A buffer of one datagram with three arrivals reaches the full buffer with a
datagram left in the socket and no Free buffer in a few steps. The
`SwapWhileReading` witness uses buffers of two because an exposed buffer
always holds at least one datagram, so a buffer of one leaves no room for
the defect's write.

The historical run before destination and destroy-deadline modeling generates
337,029 states and finds 76,264 distinct states
with no error: the seven invariants hold, and `HandedOffEventuallySent`,
`CommittedEventuallyHandedOff`, `StopEventuallyJoins`,
`FilledEventuallyExposed` and `ArrivedEventuallyDelivered` hold under the
fairness above (35.5 s, two workers, on a loaded bench).

Reachability, checked by scratch runs (not committed) of the committed
constants under `Spec` with one negated invariant each: a deferred hand-off
(`~deferred`, violated at depth 7), a blocked wait on the wake alone with a
datagram in the socket (violated at depth 10), and such a wait with the wake
set by `Return` (violated at depth 10). So the deferral and the full-buffer
wait are paths the committed run explores, not dead branches.

## The reversed witnesses

### `MCRNClientIoBrokenReuseBeforeSent.cfg`

`ReopenBeforeSent = TRUE`: the main thread may reopen a ring that is still
Sending. TLC exits 12: **`Invariant NoReuseBeforeSent is violated`**, at
depth 10 (3,697 states generated, 1,388 distinct).

- **The trace:**
  1. A game thread writes and commits slot 1 of ring A; the hand-off gives A
     to the I/O thread and switches to B.
  2. A game thread writes and commits slot 1 of B.
  3. The I/O thread sends slot 1 of A (A Sending, the I/O thread owns A).
  4. The main thread reopens A while it is Sending, hands B off and switches
     to A; a game thread writes slot 1 of A while the I/O thread still owns A.
- **Against the design, by hand:** this is the reuse the design forbids. In
  `docs/CLIENT-IO.md` the main thread reopens a ring only after an acquire
  load sees Sent, which the I/O thread stores after its last send of that
  ring; in the trace the reopen reads Sending. With the rule, step 4's
  `Reopen` is disabled until `IoFinishRing`, the hand-off of B defers, and no
  game thread writes A while the I/O thread reads it. In the code at the
  base this cannot happen yet, because the main thread sends the ring itself
  and resets it after the walk (line 1017); the hazard is the design's, and
  the rule is what the transport card must keep.

### `MCRNClientIoBrokenSwapWhileReading.cfg`

`ReceiveIntoExposed = TRUE`, `BufferCap = 2`: the I/O thread writes a
datagram into the buffer `update` has exposed. TLC exits 12: **`Invariant ExposedStable is violated`**, at depth 7 (808 states generated, 385
distinct).

- **The trace:**
  1. A datagram arrives; the I/O thread reads it into its Fill buffer 0.
  2. The I/O thread publishes buffer 0 as Ready and takes Free buffer 1 to
     fill.
  3. `update` exposes buffer 0 (one datagram, `Expose`) and processes it.
  4. A second datagram arrives and the I/O thread writes it into buffer 0,
     the exposed one: it now holds two while `update` read one.
- **Against the design, by hand:** in the design the I/O thread writes only
  its own Fill buffer, and a buffer becomes Fill only when `update` has
  returned it as Free. A swap made by `update` alone, without the I/O
  thread's publish, has exactly this effect: the I/O thread can have chosen
  its target before the swap and write it after, into the buffer `update`
  now reads. The code at the base avoids it with the mutex held across the
  whole drain (lines 1238 and 1272), which is the lock wait
  `docs/CLIENT-IO.md` measures; the handshake removes the lock without
  reopening the race.

### `MCRNClientIoBrokenReceiveStall.cfg`

`SwapNeedsReady = TRUE`: the one-step swap of the first draft of the design,
where `update` returns the buffer it exposed only in the step that exposes a
Ready one, and `Return` does not exist. TLC exits 12: **`Invariant NoReceiveStall is violated`**, at depth 7 (781 states generated, 362
distinct).

- **The trace:**
  1. A datagram arrives; the I/O thread reads it into Fill buffer 0.
  2. The I/O thread publishes buffer 0 as Ready and takes Free buffer 1.
  3. `update` exposes buffer 0 (one datagram).
  4. A second datagram arrives and the I/O thread reads it into buffer 1.
     Now buffer 0 is Exposed and buffer 1 is Fill with one datagram:
     `IoPublish` needs a Free buffer, the swap needs a Ready one, the I/O
     thread is not blocked, and no other action changes a buffer's state.
- **Against the design, by hand:** this is the deadlock found in the first
  draft: with two buffers, nothing after the first exposed batch ever
  reaches `update`. In the design as written, `update` returns buffer 0 at
  its end whatever it exposed (`Return`), so the I/O thread publishes buffer
  1 at its next batch end, or, if it blocked first, after the return's wake
  (it stored owed before blocking).

A scratch run, not committed, removes only the wake from `Return`
(`wake' = wake`) under the committed constants: TLC exits 12 with
`NoReceiveStall` violated, where the I/O thread stores owed, blocks on the
wake alone with a full buffer, and `update` returns buffer 0 without waking
it. That is the lost wake the seq_cst pairing and the owed flag exist to
prevent.

### `MCRNClientIoBrokenHandOffWaits.cfg`

`HandOffWaits = TRUE`: the hand-off of the second draft, which waits for the
other ring to be Sent. TLC exits 12: **`Invariant HandOffNeverWaits is violated`**, at depth 7 (415 states generated, 216 distinct).

- **The trace:**
  1. A game thread writes and commits slot 1 of ring A; the hand-off (here
     `send_packet`, which calls `send_packets` after its packet) gives A to
     the I/O thread and switches to B.
  2. A game thread writes and commits slot 1 of B (a second `send_packet`,
     or the protocol packet `update` commits).
  3. The next hand-off (the second `send_packet`, or the end of `update`)
     finds A still HandedOff, because the I/O thread has not run yet, and
     the main thread blocks on the I/O thread.
- **Against the design, by hand:** nothing in the trace is overload: two
  hand-offs in one frame before the I/O thread is scheduled. In the design as
  written the third step is `Defer`: slot 1 of B stays in the Filling ring,
  `CLIENT_SEND_DEFERRED` moves, the call returns, and the slot leaves with
  the next hand-off that finds A Sent. `CommittedEventuallyHandedOff` holds
  in the committed run because `update` retries every frame. Two scratch
  runs, not committed, show the property is not vacuous: with no fairness on
  `HandOff` TLC exits 13 (the committed slot is never handed off, the trace
  stutters).

### `MCRNClientIoBrokenFullWaitReadable.cfg`

`FullWaitReadable = TRUE`: the wait of the second draft, which returns on
socket readable or the wake whatever the buffer holds. TLC exits 12:
**`Invariant FullWaitsOnWake is violated`**, at depth 8 (1,576 states
generated, 672 distinct).

- **The trace:**
  1. A datagram arrives; the I/O thread reads it into buffer 0, which is now
     full (`BufferCap = 1`), and publishes it as Ready, taking buffer 1.
  2. A second datagram arrives; the I/O thread reads it into buffer 1, now
     full.
  3. No buffer is Free (buffer 0 is Ready, not yet exposed): the I/O thread
     stores owed.
  4. The I/O thread blocks in a wait that also returns on readable, with its
     buffer full.
- **Against the design, by hand:** the next datagram to arrive makes that
  wait return at once; the thread cannot read it (no room), cannot publish
  (no Free buffer), and waits again, which returns at once again: a spin
  until `update` exposes buffer 0 and returns it, about a frame of a busy
  core for any hitch of more than `ROCKETNET_NUM_CLIENT_PACKETS` replies. In
  the design as written the fourth step is `wait( transport, false, timeout )`:
  the full buffer waits on the wake alone, and the `Return` at the end of the
  next `update` wakes it because owed is set.

## Runs

The full threaded instance has 772,561 distinct states. The combined safety
and liveness control `MCRNClientIo.cfg` timed out at the 110-second budget
(exit 124) and is retired: its obligations are exactly the union of the
required split cases below, each of which completes within the budget.

The required threaded controls split the proof obligations, never the state
space. All retain two send slots, one-datagram receive buffers and three
arrivals, and every one reports the same 772,561 distinct states.
`MCRNClientIoSafety.cfg` checks all nine original invariants under `Spec`
(fairness is unnecessary for safety). The temporal configurations retain
`SpecFair` and all nine invariants, and each checks one property, or one
conjunct of one named in `MCRNClientIoLiveness.tla`:

| Configuration | Obligation | Seconds |
|---|---|---|
| `MCRNClientIoSafety.cfg` | the invariants (`Spec`) | 8.610 |
| `MCRNClientIoLiveHandedOffA.cfg` | `HandedOffEventuallySent`, ring A | 47.036 |
| `MCRNClientIoLiveHandedOffB.cfg` | `HandedOffEventuallySent`, ring B | 47.066 |
| `MCRNClientIoLiveCommittedA1.cfg` | `CommittedEventuallyHandedOff`, ring A, slot 1 | 57.845 |
| `MCRNClientIoLiveCommittedA2.cfg` | `CommittedEventuallyHandedOff`, ring A, slot 2 | 46.719 |
| `MCRNClientIoLiveCommittedB1.cfg` | `CommittedEventuallyHandedOff`, ring B, slot 1 | 47.515 |
| `MCRNClientIoLiveCommittedB2.cfg` | `CommittedEventuallyHandedOff`, ring B, slot 2 | 46.874 |
| `MCRNClientIoLiveStop.cfg` | `StopEventuallyJoins` | 47.903 |
| `MCRNClientIoLiveFilled0.cfg` | `FilledEventuallyExposed`, buffer 0 | 48.914 |
| `MCRNClientIoLiveFilled1.cfg` | `FilledEventuallyExposed`, buffer 1 | 48.848 |
| `MCRNClientIoLiveArrivedFirst.cfg` | `ArrivedEventuallyDelivered`, n = 1 | 49.826 |
| `MCRNClientIoLiveArrivedSecond.cfg` | `ArrivedEventuallyDelivered`, n = 2 | 52.698 |
| `MCRNClientIoLiveArrivedLast.cfg` | `ArrivedEventuallyDelivered`, n = 3 | 49.921 |
| `MCRNClientIoLiveDestroy.cfg` | `DestroyCompletes` | 57.940 |

Each split is a finite conjunction checked conjunct by conjunct on the
complete state graph, and an `ASSUME` in `MCRNClientIoLiveness.tla` makes each
partition complete, so a constant change that adds an element no conjunct
names fails at parse time instead of silently checking less:

- `HandedOffEventuallySent` is `\A r \in Rings`; `HandedOffA` and
  `HandedOffB` are its two conjuncts, with `ASSUME Rings = {"A", "B"}`.
- `CommittedEventuallyHandedOff` is `\A r \in Rings, s \in Slots`;
  `CommittedA1`, `CommittedA2`, `CommittedB1` and `CommittedB2` are its four
  conjuncts, with the ring assumption and `ASSUME Slots = {1, 2}`.
- `FilledEventuallyExposed` is `\A b \in Buffers`; `Filled0` and `Filled1`
  are its two conjuncts, with `ASSUME Buffers = {0, 1}`.
- `ArrivedEventuallyDelivered` is `\A n \in 1..MaxDatagrams`;
  `ArrivedFirst`, `ArrivedSecond` and `ArrivedLast` are its three conjuncts,
  with `ASSUME MaxDatagrams = 3`.

Checking every row above under the same `SpecFair` is exactly checking the
original six properties: no action, constant, state constraint, fairness
condition or property is removed or weakened.

`RUNS.tsv` is generated by the pinned `tlacheck` runner, each case as its own
shard with two TLC workers and an individual 110-second budget (seconds above
are from amd-3, a 16-core bench). The bounded controls must all finish with exit 0;
invariant witnesses must name their declared invariant. No `rnclientio`
case carries bench debt.

### `MCRNClientIoBrokenExitDropsFinal.cfg`

`ExitDropsFinal <- ExitDropsFinalOn`: the I/O thread exits without sending the
final ring. TLC exits 12 on `StopSendsAll`: a game thread commits a slot of the
Filling ring, `Stop` hands it to the I/O thread as the final ring, and `IoExit`
leaves it unsent. In the code, `rocketnet_client_io_step` sends
`io->send_final` after the last handed-off ring and only then stores
`stopped` (sdk/rocketnet_client_io.h), so the committed slots stop passed over
are sent.

## Step-driven mode

`StepDriven = TRUE` is the step-driven mode. Create starts no I/O thread
(`ThreadStarted` is false), and the I/O core actions run only as
`CallerStep`, inside a call: `StepBegin` is `update` or `send_packets` (or
after `Stop`, the exit loop)
entering `rocketnet_client_io_step` on the caller's thread with `block` false.
The step is one pass that never blocks: `StepEnd` returns once the ring it
took is finished, the exit is taken when stopped, and its drain has ended in
a publish or an owe; it may return with datagrams still in the socket, and
`IoWait` is never taken. While `inCall` holds, the main thread's own
actions (`HandOff`, `Defer`, `Reopen`, `Expose`, `Return`, `Stop`, `Join`,
`Free`) are disabled; game threads still write and commit. `lastExec`
records which executor took the last core action.

`NoIoThreadActionInStepMode` requires, in step mode, that no I/O-thread
action is enabled and that no core action has been taken by a thread
(`lastExec # "thread"`); the existing invariants hold in the mode too.
`MCRNClientIoStepDriven.cfg` (one slot, two datagrams) checks every invariant
and the five liveness properties under `SpecStepFair`: `SpecFair` with the
core actions fair on their executor, `StepBegin` and `StepEnd` weakly fair
(the game calls `update` every frame), and the main thread's reopen,
hand-off, expose, return and join and the receive strongly fair, since a
caller step disables them intermittently. That control is one slot and two
datagrams, which fits the runner's 110 s budget.

At the full instance (two slots, three datagrams) the step-driven state graph
has 449,119 distinct states. `MCRNClientIoStepFullSafety.cfg` checks the nine
invariants of `MCRNClientIoStepDriven.cfg` there under `Spec` (exit 0,
4.987 s), as a required case. Its liveness does not fit the budget:
checked one property per configuration under `SpecStepFair`, each of
`HandedOffEventuallySent`, `CommittedEventuallyHandedOff`,
`StopEventuallyJoins`, `FilledEventuallyExposed`,
`ArrivedEventuallyDelivered` and `DestroyCompletes` completes the 449,119-state
graph and then times out in the temporal check (exit 124 at 110.17 to
110.25 s, two workers, amd-3), so none is a required case. The combined
full-instance check passes when run by hand with TLC 2.19 (exit 0, about six
minutes on a 16-core bench), outside the budget, so it has no row in
`RUNS.tsv`.

The reversed witness, `MCRNClientIoBrokenStepStartsThread.cfg`, sets
`StepModeStartsThread = TRUE` and `MaxDatagrams = 0`: create starts the
thread in step mode too, but `Arrive` is disabled throughout this instance.
`NoIoThreadActionInStepMode` forbids both an enabled I/O-thread action and
a core action previously taken by that thread. From `Init`, `Stop` sets
the stopping phase and wake, enabling `IoExit` and `IoClearWake`; the
enabled-action conjunct can therefore fail without receiving any datagram.
The separately reported experiment checking only the `lastExec` conjunct
(exit 12) is historical, not the committed configuration's oracle.
The historical planted defects below exercise the step-mode liveness
checks: a receive that is only weakly fair, a `StepEnd` that
may return with the ring it took unsent, and an unfair `StepBegin` (no update
calls) each fail the temporal properties (exit 13).

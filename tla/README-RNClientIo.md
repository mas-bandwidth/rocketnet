# RNClientIo

The client transport with its socket I/O on one I/O thread, as
`docs/CLIENT-IO.md` designs it. The design replaces code in
`sdk/rocketnet_client_transport.cpp` at the base of the design: the payload
ring filled by `rocketnet_client_transport_start_packet` (line 697), the
per-packet send walk of `rocketnet_client_transport_send_packets` (line 987,
the walk at lines 1001-1017), the receive thread (created at line 407, its
loop at line 1275, the 1 ms sleep at line 1283), and the receive buffers
swapped under a mutex by `rocketnet_client_update_swap_receive_buffers`
(line 515) and filled by `rocketnet_client_receive_packets` (line 1234). No
code implements the design yet; `tla/COVERAGE.tsv` marks the machine owed.

- **The two send rings.** Game threads write slots of the ring that is
  Filling (`Write`, then `Commit`). `send_packets` (`HandOff`), with the fills
  joined and at least one slot committed, hands the ring to the I/O thread,
  sets the wake and switches to the other ring; if the other ring is not open
  yet it waits (the action is not enabled). The I/O thread sends each slot
  (`IoSend`, the ring Sending) and then releases the ring (`IoFinishRing`,
  Sent). Only a Sent ring is reopened (`Reopen`).
- **The receive handoff.** The I/O thread reads a datagram into its Fill
  buffer (`IoReceive`) and, when a Free buffer is there, publishes the Fill
  buffer as Ready and takes the Free one to fill (`IoPublish`). With no Free
  buffer it records the publish as owed (`IoOwe`) before it may block.
  `update` is two steps: at its start it exposes the Ready buffer when there
  is one (`Expose`), and at its end, every update, it returns the buffer it
  holds as Free and wakes the I/O thread when the owed flag is set
  (`Return`). Each action is one atomic step, which is the seq_cst
  store-then-load pairing of `docs/CLIENT-IO.md`.
- **The wake.** `IoWait` blocks only when nothing is visible to do, never
  while a publish is possible, and with an unpublished batch only once it is
  owed; `IoWake`
  returns on the wake or a readable socket and consumes the wake;
  `IoClearWake` consumes a wake a running thread does not need. The wake is
  level-triggered, so the check and the wait are one step in the model and
  sound in code: a wake set between them makes the wait return at once.
- **Stop.** `Stop` sets the phase Stopping and the wake; the I/O thread
  finishes the ring in flight and exits (`IoExit`); the main thread joins
  (`Join`) and frees (`Free`).

## Invariants

- **`NoReuseBeforeSent`**: a slot a game thread is writing belongs to a ring
  that is Filling and that the I/O thread is not sending.
- **`ExposedStable`**: the exposed receive buffer's contents equal what
  `update` exposed at the last `Expose`, until `Return`.
- **`FreedOnlyAfterJoin`**: once freed, the I/O thread has exited and holds
  no ring.
- **`NoReceiveStall`**: while the transport runs, datagrams in the I/O
  thread's Fill buffer always have a next step toward `update`: `IoPublish`,
  `Expose`, `Return` or `IoWake` is enabled.
- **`TypeOK`**: every variable has its declared type.

## Liveness

Checked under weak fairness of the I/O thread's `IoSend`, `IoFinishRing`,
`IoReceive`, `IoPublish`, `IoOwe`, `IoWake` and `IoExit`, of `Join`, and of
`update`'s `Expose` and `Return` (the game calls update every frame)
(`SpecFair`):

- **`HandedOffEventuallySent`**: a handed-off ring is eventually Sent.
- **`StopEventuallyJoins`**: after `Stop` the thread is eventually joined.
- **`FilledEventuallyExposed`**: a Fill buffer holding datagrams is
  eventually Exposed, unless the transport stops.
- **`ArrivedEventuallyDelivered`**: every datagram that arrives is
  eventually exposed to `update`, unless the transport stops.

## What the model leaves out

- Slots, datagrams and buffers are states and counts; packet bytes, the
  wire, the clock callback and the decrypt are not modelled.
- The receive buffer's capacity is not bounded: a full buffer with no Free
  one is the owed path the model checks (`IoOwe`, then `Return` wakes). The
  late counter's spin is not modelled; the hand-off waiting is the disabled
  `HandOff`.
- A release and acquire implementation of the owed flag (weaker than
  seq_cst) is outside the model: its atomic steps are the seq_cst pairing.
- `update`'s protocol packet and the disconnect burst are slots committed and
  handed off like payloads, so they are `Write`, `Commit` and `HandOff`.

## How to run it

On a Linux bench, from `tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNClientIo.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenReuseBeforeSent.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenSwapWhileReading.cfg MCRNClientIo
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 1 -cleanup -config MCRNClientIoBrokenReceiveStall.cfg MCRNClientIo
```

The records come from `tlacheck run` as `tla/README.md` describes (group
`rnclientio`). The small instance:

| Constant | Value |
|---|---|
| `NumSlots` | 2 |
| `MaxHandOffs` | 3 |
| `MaxDatagrams` | 2 |
| `ReopenBeforeSent` | `FALSE` (`TRUE` in `MCRNClientIoBrokenReuseBeforeSent.cfg`) |
| `ReceiveIntoExposed` | `FALSE` (`TRUE` in `MCRNClientIoBrokenSwapWhileReading.cfg`) |
| `SwapNeedsReady` | `FALSE` (`TRUE` in `MCRNClientIoBrokenReceiveStall.cfg`) |

The committed run generates 203,864 states and finds 55,764 distinct states
with no error: the five invariants hold, and `HandedOffEventuallySent`,
`StopEventuallyJoins`, `FilledEventuallyExposed` and
`ArrivedEventuallyDelivered` hold under the fairness above (10.4 s, two
workers).

## The reversed witnesses

### `MCRNClientIoBrokenReuseBeforeSent.cfg`

`ReopenBeforeSent = TRUE`: the main thread may reopen a ring that is still
Sending. TLC exits 12: **`Invariant NoReuseBeforeSent is violated`**, at
depth 10 (4,955 states generated, 1,934 distinct, one worker).

- **The trace:**
  1. A game thread writes and commits slot 1 of ring A; `send_packets` hands
     A off and switches to B.
  2. A game thread writes and commits slot 1 of B.
  3. The I/O thread sends slot 1 of A (A Sending, the I/O thread owns A).
  4. The main thread reopens A while it is Sending, hands B off and switches
     to A; a game thread writes slot 1 of A while the I/O thread still owns A.
- **Against the design, by hand:** this is the reuse the design forbids. In
  `docs/CLIENT-IO.md` the main thread reopens a ring only after an acquire
  load sees Sent, which the I/O thread stores after its last send of that
  ring; in the trace the reopen reads Sending. With the rule, step 4's
  `Reopen` is disabled until `IoFinishRing`, `HandOff` of B waits, and no
  game thread writes A while the I/O thread reads it. In the code at the
  base this cannot happen yet, because the main thread sends the ring itself
  and resets it after the walk (line 1017); the hazard is the design's, and
  the rule is what the transport card must keep.

### `MCRNClientIoBrokenSwapWhileReading.cfg`

`ReceiveIntoExposed = TRUE`: the I/O thread writes a datagram into the buffer
`update` has exposed. TLC exits 12: **`Invariant ExposedStable is violated`**,
at depth 7 (1,140 states generated, 542 distinct).

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
Ready one, and `Return` does not exist. TLC exits 12: **`Invariant
NoReceiveStall is violated`**, at depth 7 (1,142 states generated, 536
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
  (it stored owed before blocking). The same switch under `SpecFair` (a
  scratch run, not committed) violates the temporal properties (exit 13).

A second scratch run, not committed, removes only the wake from `Return`
(`wake' = wake`): TLC exits 13 with the temporal properties violated at
depth 13, where the I/O thread stores owed, blocks with one datagram in
buffer 1, and `update` returns buffer 0 without waking it; the datagram never
reaches `update`. That is the lost wake the seq_cst pairing and the owed flag
exist to prevent.

## Runs

The four rows are in `tla/RUNS.tsv` from `tlacheck run` on a Linux bench
(TLC 2.19, the pinned JAR): the control passes with exit 0, and each witness
exits 12 naming its declared invariant.

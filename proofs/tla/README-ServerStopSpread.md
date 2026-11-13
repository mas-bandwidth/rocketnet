# Server disconnect bursts across updates and across the stop

`ServerStopSpread.tla` models the server transport's disconnect ring with its
own index arithmetic and the transport's send staging
(`sdk/rocketnet_server_transport.cpp`: `disconnect_bursts`,
`rocketnet_server_transport_defer_disconnect_burst`,
`rocketnet_server_transport_drain_disconnect_bursts`, the stop step and
`rocketnet_server_transport_send_capacity_available`; `sdk/rocketnet_api.cpp`
`rocketnet_server_stop`). `ServerSession.tla` coarsens the stop's seals into
its Stop step; this model is the stop as a state machine across steps.

## What is modelled

| Variable | Meaning | Code |
|---|---|---|
| `ring`, `head`, `count`, `cursor` | the ring of `RingSize` entries, each a session and its sealed count | `disconnect_bursts`, `disconnect_burst_head`, `disconnect_burst_count`, `disconnect_burst_cursor` |
| `staged`, `inflight` | frames started since the last flush, and frames flushed that the transport still holds | the staged bank (the io_uring array, the gso producer bank, the IOCP send operations, the RIO fill phase, an XDP queue) |
| `phase` | `RUNNING`, `STOPPING`, `IOSTOP` (the I/O told to stop), `STOPPED` | `state`, `io_stop_requested`, the I/O's stopped |
| `held` | copies the development link simulator holds (always 0 with `Simulator = FALSE`) | the simulator's queue; `rocketnet_server_commit_staged` hands it the datagram and aborts the staging slot, so a held copy is neither `staged` nor `inflight` |
| `jammed` | environment: the transport has lost its send room for good | an XDP queue whose TX wakeup failed hard (`send_hard_error`) with its producer full |
| `sent`, `owed`, `abandoned`, `refused`, `overflow`, `cut` | ghosts | |
| `dropped`, `ringFull`, `early` | ghosts: bursts abandoned at a stop step for want of room, the `SEND_RING_FULL` counts those abandons added, and an abandon while room could still return | |

`Room` is `BankSize - staged - inflight`, or 0 while `jammed`: the
transport's `send_room` operation as `send_capacity_available` returns it.
On a buffered (unstaged) transport `rocketnet_server_transport_io_send_room`
subtracts the frames committed to the send buffer and not yet flushed
(`staged`); before that change the code reported room that `staged` had
already spent. `Idle` is `inflight = 0 \/ jammed`: the transport's
`send_idle`, which gives up on a queue in hard send error.

| Action | Code |
|---|---|
| `Kick(i)` | `rocketnet_server_transport_disconnect_client` on a running server: the burst appended at `( head + count ) % RingSize` while fewer than `SteadyCap` wait, else its copies dropped (the immediate seal is this append and its seals in the same update) |
| `Stop` | `rocketnet_server_stop`: every connected slot's burst appended (`rocketnet_server_transport_disconnect_client_shutdown`), then STOPPING |
| `SealOne` | one position of the drain's pass: a complete entry passed over, an unreachable start abandoning its burst, or one packet sealed while `Room > 0`; a completed head leaves with the cursor |
| `Flush` | the stop step's or the update's flush; with the simulator, also one copy of its shutdown drain (`rocketnet_server_link_drain` with `shutdown`), which re-stages a held copy on a queue and stops once `rocketnet_server_transport_io_send_room` (the fewest free slots over the queues, `FewestRoom`) is 0 |
| `RequestIoStop` | the stop step that finds the ring empty, its seals flushed, the transport's `send_idle` true (every flushed send finished) and, with the simulator, `link_empty` (it holds nothing, `held = 0`) calls `rocketnet_server_transport_io_stop` |
| `AbandonNoRoom` | the stop step after its flush (`rocketnet_server_transport_stop_step`) that finds bursts waiting, `send_room` 0, `send_idle` true and `send_room` still 0: every waiting burst is abandoned (key wiped, popped from the head, cursor reset), each one not yet complete counted once as `SEND_RING_FULL`. The reads are not one atomic look: the transport may finish some of its flushed frames before `send_idle` reads idle (the portable transport's `send_idle` publishes its producer and its worker may send it first), which frees room only the second read sees |
| `IoStopped` | the workers exit; `rocketnet_server_stopped` is true |
| `Connect(i)` | environment: a client takes a free slot |
| `Complete` | environment: the kernel or worker finishes a flushed frame; a jammed transport finishes nothing |
| `Jam` | environment: the transport loses its send room for good (an XDP queue in hard send error with its producer full). The hard error can clear on a later wakeup; the stop takes `send_idle`'s giving up as final, as the code does, so the model keeps the jam |
| `LoseRoute(s)`, `FindRoute(s)` | environment: a destination loses or regains its link-layer address (the XDP reply MAC), or the transport refuses it with room left (an address its socket cannot carry); either refusal abandons the burst. A sealed write that fails after its start (the code abandons that burst and keeps draining since the `rn-server-stop-sim-burst` merge) is the same step: `LoseRoute(s)` then the abandon branch of `SealOne`, which needs the same room and stages nothing |

## Properties

- `RingHolds`: an append always finds a free entry (`RingSize = 2 * SteadyCap`).
- `SealedWithinBurst`: no entry is sealed past its burst, so no sequence of
  the burst's reserved range is ever reused.
- `NoRefusedStart`: no staged start is called that the transport refuses for room.
- `WithinBank`: the staging never holds more than the transport has.
- `RingIsOwed`: the live window holds exactly the owed bursts not yet complete
  or abandoned, each once, each entry's count the packets its session was sent,
  every other entry clear (the layout refines the abstract owed set).
- `HeadNeverDone`: a completed head leaves at once.
- `FullBurstBeforeStopped`: before the I/O is told to stop, every burst kept
  (every client connected at the stop, every burst still waiting from a
  kick) has been handed whole to the transport, but for a destination the
  transport cannot reach or a transport with no send room left.
- `NoSendCutAtStop`: the I/O is told to stop only once every send it was
  handed has finished, so a stopping I/O cuts off no flushed disconnect packet
  (what a jammed transport holds can never leave, so it is not counted cut).
- `NoDropWhileRoomCanReturn`: no burst is abandoned for want of room while the
  transport has room, or while a send staged or in flight could still finish
  and free some.
- `EachBurstSettledOnce`: each burst abandoned for want of room was not yet
  complete and not already abandoned for its destination, and is counted as
  `SEND_RING_FULL` exactly once; once the I/O is told to stop, every burst kept
  is settled exactly one way: sent whole, abandoned for its destination, or
  abandoned for want of room.
- `StopFinishes` (liveness): STOPPING leads to STOPPED, under weak fairness of
  the drain, the flush, the transport's completions, `AbandonNoRoom` and the
  I/O stop.
- `StopFinishesWithoutRoom` (liveness): STOPPING on a jammed transport leads to
  STOPPED.
- `NoCopyHeldAtStop`: the I/O is told to stop only once the link simulator
  holds nothing.
- `StopFinishesWhileUnjammed` (liveness, simulator configs): STOPPING with no
  queue jammed leads to STOPPED or to a jam.

## Instance

Two slots, four connections (five for the off-by-one witness), bursts of two
packets (three in the control, so an abandoned entry can be revisited away from
the head), a staging of three frames and a ring of four entries, twice the two
a running server keeps waiting. A stop owes up to eight packets (twelve in the
control) against a staging of three, so it spreads across steps.

| Config | Expected |
|---|---|
| `MCServerStopSpread.cfg` | pass, every invariant (bursts of three) |
| `MCServerStopSpreadLive.cfg` | pass, `StopFinishes` under `FairSpec` |
| `MCServerStopSpreadLiveNoRoom.cfg` | pass, `StopFinishesWithoutRoom` under `FairSpec` (one property a run keeps each within the record budget) |
| `MCServerStopSpreadBrokenSingleShot.cfg` | `FullBurstBeforeStopped` violated |
| `MCServerStopSpreadBrokenRingSmall.cfg` | `RingHolds` violated |
| `MCServerStopSpreadBrokenSteadyCapOffByOne.cfg` | `RingHolds` violated |
| `MCServerStopSpreadBrokenNoSkip.cfg` | `SealedWithinBurst` violated |
| `MCServerStopSpreadBrokenUnreachableStalls.cfg` | `StopFinishes` violated (deadlock check off: the stall is a stuck state) |
| `MCServerStopSpreadBrokenStopInFlight.cfg` | `NoSendCutAtStop` violated |
| `MCServerStopSpreadBrokenNoAbandon.cfg` | `StopFinishesWithoutRoom` violated (deadlock check off, as for the unreachable stall) |
| `MCServerStopSpreadBrokenNoReread.cfg` | `NoDropWhileRoomCanReturn` violated |
| `MCServerStopSpreadQueues.cfg` | pass, every invariant with two send queues (three connections, bursts of two, a bank of two a queue) |
| `MCServerStopSpreadQueuesLive.cfg` | pass, `StopFinishes` and `StopFinishesWithoutRoom` under `FairSpec`, two send queues |
| `MCServerStopSpreadBrokenPooledRoom.cfg` | `StopFinishesWithoutRoom` violated (deadlock check off) |
| `MCServerStopSpreadSimulator.cfg` | pass, every invariant and `NoCopyHeldAtStop`, two queues, the link simulator on |
| `MCServerStopSpreadSimulatorLive.cfg` | pass, `StopFinishesWhileUnjammed` under `FairSpec` |
| `MCServerStopSpreadSimulatorJam.cfg` | `StopFinishesWithoutRoom` violated (deadlock check off): the open code defect below |
| `MCServerStopSpreadBrokenStopWithHeld.cfg` | `NoCopyHeldAtStop` violated |
| `MCServerStopSpreadBrokenNoLinkDrain.cfg` | `StopFinishesWhileUnjammed` violated (deadlock check off) |

Measured on aws-mut-3 (TLC 2.19, 8 workers, 2026-11-13): the control 3,506,156
distinct states in 59.0 s; `Live` and `LiveNoRoom` 804,126 in 67.9 s and
64.6 s; `Queues` 303,256 in 8.9 s and `QueuesLive` in 56.8 s; `NoAbandon`
fails at 713,340 in 72.5 s; `PooledRoom` at 162,994 in 18.6 s; the simulator
configs reach 32,150 distinct states (2.2 s, 4.4 s, and 6.3 s for the jam);
`StopWithHeld` fails at 187 and `NoLinkDrain` at 2,336. `RUNS.tsv` holds the
records.

The configs above `QueueCount = 1` are the transport with one send queue. With
`QueueCount` queues, a connection's bursts go on queue `s % QueueCount`
(`QueueOf`), the staging and in-flight sends are per queue, and `Complete` and
`Jam` act on one queue. `SealOne` passes a burst whose own queue is full while
another has room (the drain's full-queue pass in
`rocketnet_server_transport_drain_disconnect_bursts`), and `AbandonNoRoom`,
once every queue is idle, drops only the waiting bursts of queues with no
room, counting each in SEND_RING_FULL (`rocketnet_server_transport_stop_step`).
`OnlyJammedQueuesDrop` says no burst of a queue with room is dropped.

`Defect = "pooledroom"` judges room over all queues pooled, as the code did
before: the drain stops at a burst on a full queue while another queue has
room, and the stop step abandons only when the room summed over the queues is
0. With one queue jammed for good and the other with room, neither ever
happens: the stop never finishes. This is the stop hang the unit test
`test_server_stop_finishes_past_a_jammed_queue`
(`tests/test_unit_server_send_queues.cpp`) shows red on the code before.

## The link simulator, and an open code defect

With `Simulator = TRUE` a sealed packet goes to the development link
simulator instead of the staging: `rocketnet_server_commit_staged` hands the
datagram to the simulator and aborts its staging slot, so `SealOne` raises
`held` and leaves `staged` alone. The stop step's flush drains the simulator
with `shutdown` (`rocketnet_server_link_drain`), re-staging held copies one
by one while `rocketnet_server_transport_io_send_room` is above 0; that room
is the fewest free slots over the queues, so one full queue stops the drain.
The stop step calls `rocketnet_server_transport_io_stop` only when
`link_empty` holds (`rocketnet_server_transport_stop_step`), and nothing in
the code discards a held copy.

**Open code defect (MM080), owned by the send-queue-capacity follow-up.**
The stop's abandon branch frees the ring when no queue has room and every
queue is idle, but it does not touch the simulator. Once a queue is jammed
for good, `FewestRoom` stays 0, the drain never re-stages a held copy, and
`link_empty` never holds: the stop never reaches the I/O stop.
`MCServerStopSpreadSimulatorJam.cfg` is that trace and fails
`StopFinishesWithoutRoom`, as the code does. TLC's trace: a client connects,
queue 0 jams, the server stops, both packets of the burst are sealed into the
simulator (`held = 2`), queue 1 jams, and the run stutters in STOPPING with
the ring empty, `held = 2` and no room. Against the code by hand: the stop
step at the merged head reaches the `link_empty` test with
`disconnect_burst_count == 0`, `link_drain` breaks at once on
`io_send_room <= 0`, and nothing else empties the simulator. The model takes
no step the code cannot take; the committed case is expected to fail until
the code fix lands, and its expected column then turns to pass.

`Defect = "stopwithheld"` drops the `link_empty` test: TLC's trace seals a
stopping client's burst into the simulator, the ring empties, and the I/O
stop is requested with `held = 2` (`NoCopyHeldAtStop`). `Defect =
"nolinkdrain"` removes the drain: with no queue jammed, a held copy never
leaves and the stop stutters (`StopFinishesWhileUnjammed`).

## The reversed witnesses

`Defect = "singleshot"` is the stop before the spread: every burst staged in
the stop call whatever the room, a refused start dropping its packet, and the
I/O told to stop at once. TLC's trace: a client connects, the server stops,
and the I/O stop follows with the client's burst unsent. Against the code
before this model, `rocketnet_server_stop` sealed every connected client's
ten packets through `rocketnet_server_reserve_send_frame` and then called
`rocketnet_server_transport_stop`; a staged start past the bank returned NULL,
the rest of that burst was dropped, and at 1000 clients io_uring's 2048-slot
array took about 204 clients' bursts and gso's 4096-slot bank about 409.

`MCServerStopSpreadBrokenRingSmall.cfg` is the ring of `SteadyCap` entries
the code had: two kicked bursts wait, the stop appends the two connected
clients, and the fourth append lands on a live entry.

`Defect = "steadycapoffbyone"` defers while `count <= SteadyCap`: three
bursts wait, and the stop's two appends overrun the ring of four.

`Defect = "noskip"` seals the entry at the cursor even when it is complete.
TLC's trace at bursts of three: two clients stop, client 1's first packet is
sealed, client 2's destination is lost and its burst abandoned (marked
complete), client 1's second packet is sealed, the destination is found
again, and the pass seals client 2's entry to four. In the code the skip is
the `burst->sealed == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS` test at the top
of the drain's loop.

`Defect = "stopinflight"` tells the I/O to stop once the ring is empty and
its seals flushed, without waiting for the transport to finish them: TLC's
trace stops one client, seals and flushes its burst, and requests the I/O
stop with those frames in flight. On IOCP the send turn ends once the
transport is stopping and no bank is ready (`finish = index < 0 && stopping`
in `rocketnet_iocp_send_lane_turn`, `sdk/rocketnet_server_iocp_windows.cpp`).
On RIO, once stopping, `rocketnet_rio_lane_send_room` reports 0 and
`rocketnet_rio_lane_start_packet` refuses, and `rocketnet_rio_worker` turns to
`rocketnet_rio_stop_pass` (`sdk/rocketnet_server_rio_windows.cpp`), which
posts what is ready and closes the socket; closing a UDP socket cancels the
RIO requests still pending. A send not finished by then is cut. The stop step
now waits for `send_idle` (`rocketnet_server_transport_stop_step`).

`Defect = "unreachablestalls"` ends the drain at an unreachable start as at a
full staging: the abandoned burst waits forever and STOPPING never reaches
STOPPED.

`Defect = "noabandon"` is the stop step before `AbandonNoRoom`. TLC's trace:
a client connects, the transport jams, a second connects and is kicked (its
burst waits), a third connects, and the stop appends the other two: three
bursts wait with no room, nothing staged or in flight, and the run stutters
in STOPPING (`StopFinishesWithoutRoom`). In the code before the fix each stop
step drained with `max_seals` equal to a room of 0, flushed nothing and never
reached the I/O stop while `disconnect_burst_count > 0`:
`test_server_stop_abandons_bursts_without_send_room` (room 0, nothing in
flight) never stopped in 100000 polls.

`Defect = "noreread"` abandons without the second room read. TLC's trace: two
clients connect, the stop appends both bursts of two, three seals (client 1,
client 2, client 1, whose complete burst leaves the head) fill the staging of
three, the flush hands them over, and the stop step reads no room; the
transport finishes all three during the idle check, so it is idle with its
room back, and client 2's half-sent burst is abandoned with room to send it
(`NoDropWhileRoomCanReturn`). In the code that is a portable producer that
`send_idle` publishes and its worker sends before the check reads idle; the
second `send_room` read keeps the burst, and the next step seals its last
packet (`test_server_stop_without_room_waits_for_sends_in_flight` holds the
waiting half).

Each trace was replayed by hand against `rocketnet_server_transport_drain_disconnect_bursts`
and `rocketnet_server_transport_stop_step`.

## Worst case at the defaults

At `ROCKETNET_MAX_CLIENTS` 1000, a stop owes at most 1000 waiting bursts and
1000 connected clients' bursts of ten: 20000 packets. The smallest staged
bank is 2048 frames (io_uring's array, the IOCP send operations and the RIO
fill phase; gso and XDP take 4096). When each flush's frames are finished by
the next step, the stop needs `ceil( 20000 / 2048 ) = 10` steps (one more when
frames staged before the stop still fill the bank), 5 with only the 1000
connected clients' 10000 packets, plus the polls until the workers exit. A
transport that holds its frames longer only spreads the same steps further;
each step's work stays bounded by the room.

## Scope

The model abstracts the seal itself, the key wipe and the allowance of a
running update (any interleaving of `SealOne` covers every allowance). Room
is per queue (`Room(q)`), and `Jam` acts on one queue; the configs with one
queue are the transport with one send queue. The link simulator is one pool
of held copies, re-staged on any queue with room. The I/O's own stop (the transport's workers and its last flush) is its
transport's model's subject.

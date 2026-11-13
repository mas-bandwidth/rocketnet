# RNSendReserve and RNSendReserveCalls

The protocol send reserve of one send queue of a staged server transport
(`sdk/rocketnet_server_transport.cpp`). The game's packets
(`rocketnet_server_transport_start_packet`) and the protocol's own frames
(keep-alives, acks, handshake replies, disconnects, sent by the next update)
share each queue's staging. The reserve is what the protocol needs there:
`handshakes_per_update + disconnect_packets_per_update`, one frame for every
slot not connected, and one for every connected slot whose address hashes to
this queue (`rocketnet_server_transport_send_reserves`), and never more than
half the largest room the queue has reported when a budget opened (its
staging), so a config whose allowances outgrow the staging still leaves the
game half of it; the own frames handed out are never more than the reserve.
In the models `Reserve` is that figure, whatever it is. The game's packets may
take the room above it, and frames the transport retires before the budget is
opened again (a completion port, a ring the kernel drains) come back to them.

Two modules:

- `RNSendReserve.tla` is the design. Every read and write of a shared word is
  a step of its own, so several threads may start game packets at once.
- `RNSendReserveCalls.tla` is the same rules one whole call at a time, as one
  thread sees them. It is what the trace check records, so it is the module
  the code is bound to (`tests/tla_trace_send_reserve.cpp`,
  `TraceRNSendReserveCalls.tla`, layer `sendreserve`).

## State

| Model | Code |
|---|---|
| `free` | the queue's free frames: `send_room_queue`, or `send_room` for one queue |
| `started` | `fill_started[queue]`: game frames staged and counted since the budget opened |
| `ownUsed` | `fill_own_used[queue]`: own frames staged and counted since then |
| `ownTaken`, `ownLeft` (`own` in the calls module) | `fill_own_frame[client]` cleared and set; `fill_own_frames_left` |
| `claims` | `fill_claims[queue]` |
| `budget` | `fill_budget[queue]`: the grant cap, raised by an atomic max, never lowered until it is opened again |
| `seenS`, `seenO`, `seenR` | the three reads of `queue_budget_now` |
| `refused` (calls module) | the counter `SEND_RESERVE_REFUSED` |
| `starved` | ghost: a claim was refused while the room it read had a frame for it |

## RNSendReserve: actions and the code they mirror

- **`OpenFill`**: main, with no game packet being started (`send_packets`
  after the flush, and `update` after its body:
  `rocketnet_server_transport_open_fill_budget`). The update's protocol frames
  have taken up to the reserve; the budget opens at the room above the
  reserve, the counts and claims are zeroed, and each connected client whose
  keep-alive is acknowledged gets its own frame.
- **`OwnClaim`, `OwnIo`, `OwnCount`**: a client's first frame since the budget
  opened, when it owns one, takes its reserved frame: `fill_claim` clears
  `fill_own_frame`, `io_start_packet` stages it (it leaves the room), and
  `fill_started` counts it in `fill_own_used`, which lowers the reserve the
  budget keeps back. A transport that refuses it (`fill_refused`) leaves it in
  the reserve: its client's keep-alive is still due.
- **`Claim`, `Check`**: any other frame adds one to the claims (`claim_frames`);
  an index below the budget is granted.
- **`Read1`, `Read2`, `Read3`**: at or past the budget,
  `rocketnet_server_transport_refresh_fill_budget` recomputes it from
  `queue_budget_now`: the two counts, then the room, then the two counts
  again, starting over if either moved. The budget is raised to `started +
  (room - (reserve - own_used))`, and the claims outstanding (this one among
  them) are tested against it.
- **`TakeBack`, `Retest`**: a claim past the raised budget is taken back, and
  then the claims and the budget are read again. A frame now under the budget
  is claimed again; else the start is refused and counted as
  `SEND_RESERVE_REFUSED`.
- **`Stage`, `Count`**: `io_start_packet` stages a granted frame, then
  `fill_started` counts it. A transport that refuses it takes the claim back
  (`fill_refused`).
- **`Retire`**: the transport retires a staged frame at any time.

## RNSendReserveCalls: one action per call

| Action | Call | RNSendReserve steps it stands for |
|---|---|---|
| `Open(used, acked)` | `send_packets` after an update whose protocol frames took `used[q]` frames of each queue `q`; each queue's reserve is the formula cut to half the largest room seen on it | `OpenFill` |
| `StartStaged(c)` | `start_packet` gave a frame | `OwnClaim`, `OwnIo`, `OwnCount`; or `Claim`, `Check`, `Read1-3`, `Stage`, `Count` |
| `StartIoRefused(c)` | `start_packet` gave nothing: the transport refused the frame | `OwnClaim`, `OwnIo` refused; or `Claim`, `Check`, `Read1-3`, `Stage` refused |
| `StartRefused(c)` | `start_packet` gave nothing: past the budget, counted | `Claim`, `Check`, `Read1-3`, `TakeBack`, `Retest` |
| `Finish`, `Abort` | `finish_packet`, `abort_packet` | none: the reserve's words stay; an aborted frame's claim is not given back |
| `Capacity(n)` | `send_capacity_available` returned `n` | none: a read of the room, the budget as it would be raised, the claims and the own frames left |
| `CapacityTo(c, n)` | `send_capacity_to(c)` returned `n`: the client's own queue's room and budget left, with its own frame | none: a read |
| `CapacityAll(n)` | `send_capacity_to(-1)` returned `n`: the room summed over the queues, within the whole budget left | none: a read |
| `Retire(q, n)` | the transport frees `n` frames of queue `q` | `Retire`, `n` times |

Every word but the own frames is per send queue (`NQ` queues, client `c` on
queue `(c % NQ) + 1`); with `NQ = 1` the module is the one-queue model it was.

The refinement is stated in the module's header and argued, not checked by
TLC: each action is a run of RNSendReserve steps by one thread with no other
thread's step between them. The calls module's own invariants are checked,
and are RNSendReserve's two properties at the points between calls, plus
`ClaimsAreStarted` (with one thread every claim outstanding is a frame staged)
and `OwnLeftCounts`.

## Properties

- **`ReserveKept`**: once a game frame is granted, the room left after every
  frame in flight has staged still covers the reserve, less the own frames
  taken: `free + ownTaken >= Reserve + OwnUnstaged + Pending`. The next
  update's protocol frames are never refused for the game packets' sake.
- **`NeverStarved`**: no claim is refused while the room its recompute read
  had a frame for it past the reserve and for every other thread in flight
  when it is refused, nor refused for good while the claims left after its
  take back leave a frame under the budget. Room that comes back after the
  read, or that a thread claiming after the read takes, is not starvation.
- **`NoHiddenRoom`** (RNSendReserveCalls): a client's capacity is never
  answered 0 while a start to it would stage, so one full queue never hides
  another's room from the send pass and the sweep.
- **`ReserveRestored`** (RNSendReserveCalls): each queue's reserve is the
  formula cut to half the largest room seen on it, so a reserve cut while a
  queue's room was small grows back once larger room is seen.

## Configs

RNSendReserve: two threads, a reserve of 2 in a queue of 4, one own frame, two
frames per thread, two retires and two openings.

| Config | Expected | What it shows |
|---|---|---|
| `MCRNSendReserve.cfg` | pass | the code: `TypeOK`, `ReserveKept`, `NeverStarved` |
| `MCRNSendReserveBrokenRoomOnly.cfg` | `ReserveKept` | the budget is the whole room, with no reserve |
| `MCRNSendReserveBrokenSplitClaim.cfg` | `ReserveKept` | the claim is a load then a store, so two threads take one index |
| `MCRNSendReserveBrokenRoomFirst.cfg` | `ReserveKept` | the room is read before the counts, so a frame staged between is counted twice |
| `MCRNSendReserveBrokenOwnCountFirst.cfg` | `ReserveKept` | an own frame lowers the reserve before it leaves the room |
| `MCRNSendReserveBrokenNoRefresh.cfg` | `NeverStarved` | the budget stays as main opened it, so frames retired afterwards never reach the game's packets |
| `MCRNSendReserveBrokenNoReread.cfg` | `NeverStarved` | the counts are read once, so a frame counted between them and the room read is lost |
| `MCRNSendReserveBrokenIndexTest.cfg` | `NeverStarved` | the recompute tests the claim's index, which a refused claim ahead of it has raised |
| `MCRNSendReserveBrokenNoRetry.cfg` | `NeverStarved` | a claim taken back is refused with no retest, so two threads refused by each other's claims are both refused with a frame left |

RNSendReserveCalls: two clients, a reserve of 3 in a queue of 5, one frame a
claim, bounded at three refusals and seven frames staged per opening; and
the chunked claims in a queue of 8 with a reserve of 2, chunks of three and
a guard of three.

| Config | Expected | What it shows |
|---|---|---|
| `MCRNSendReserveCalls.cfg` | pass | the code: `TypeOK`, `ReserveKept`, `NeverStarved`, `ClaimsAreStarted`, `OwnLeftCounts` |
| `MCRNSendReserveCallsBrokenRoomOnly.cfg` | `ReserveKept` | the budget is the whole room |
| `MCRNSendReserveCallsBrokenNoRefresh.cfg` | `NeverStarved` | a start is refused while retired frames stand above the reserve |
| `MCRNSendReserveCallsBrokenAbortReturns.cfg` | `ReserveKept` | an aborted packet gives its claim back though its frame left the room |
| `MCRNSendReserveCallsChunk.cfg` | pass | the code with chunked claims: a chunk while the guard holds, its tail taken a frame a start, given back on a refused frame and at the next opening |
| `MCRNSendReserveCallsBrokenTailLeak.cfg` | `NeverStarved` | a refused frame gives back its own claim and not the tail held with it |
| `MCRNSendReserveCallsQueues.cfg` | pass | two queues, two clients, a reserve of 1 in queues of 3, a first room seen of 1: every invariant with `NoHiddenRoom` and `ReserveRestored` |
| `MCRNSendReserveCallsBrokenMinRoom.cfg` | `NoHiddenRoom` | the fewest room of any queue answers for every client, as `send_capacity_available` did for the send pass and the sweep |
| `MCRNSendReserveCallsBrokenStickyReserve.cfg` | `ReserveRestored` | a reserve cut by a small room seen never grows back |

## The trace check

`tests/tla_trace_send_reserve.cpp` drives a server transport with three seated
clients and a fake staged transport of one queue (seeds 1 and 2 modulo 4), or
four clients on two queues (seeds 3 and 4), one call per step, and writes the
reserve's words after each, every queue's as a sequence. The two-queue walks
reach QueueStall: one queue's room run out while the other has room. TLC checks each record is the
RNSendReserveCalls action its call's result names. An odd seed's queue is the
reserve and six frames, so every claim is one frame; an even seed's is the
reserve, the chunk guard (a chunk of 8 for each of 64 lines) and 200 frames,
so its walk claims chunks near the top of the room and single frames below
(reach predicates ChunkTail and SingleInBigRoom). Its plant
(`ROCKETNET_SEND_RESERVE_PLANT`) keeps the claim of a frame the transport
refused, and the tail held with it; the planted trace must be rejected at
the first record where the claims differ from the frames staged and the
tail held.

The walk is one thread, whole calls. It does not see two threads inside a
call: that is RNSendReserve's ground, and its witnesses SplitClaim, RoomFirst,
OwnCountFirst, NoReread, IndexTest and NoRetry have no call-level form.

## What it does not cover

The size of the reserve, which is the formula and the half above, checked by the unit tests
in `tests/test_unit_server_send_reserve.cpp` (the calls model carries only the
cut to half the room seen); several queues in RNSendReserve, whose steps are
per queue and independent (each its own fields), checked by the same tests
and by RNSendReserveCalls at the call level; and the C++
memory order, beyond what the atomics the code uses
(`rocketnet_platform_atomic_add_u64`, `_max_u64`, `_load_u64`) give.

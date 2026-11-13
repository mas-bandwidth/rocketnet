# RNSendReserve

The protocol send reserve of one send queue of a staged server transport
(`sdk/rocketnet_server_transport.cpp`). The game's fill (`start_packet` from
the fill threads) and the protocol's frames (keep-alives, acks, handshake
replies, disconnects, sent by the next update) share each queue's staging.
The reserve is what the protocol needs there: `handshakes_per_update +
disconnect_packets_per_update`, one frame for every slot not connected, and
one for every connected slot whose address hashes to this queue
(`rocketnet_server_transport_send_reserves`). The fill may take the room
above it, and frames the transport retires during the fill phase (a
completion port, a ring the kernel drains) come back to the fill.

## State

| Model | Code |
|---|---|
| `free` | the queue's free frames: `send_room_queue`, or `send_room` for one queue |
| `started` | `fill_started[queue]`: fill frames staged and counted this phase |
| `ownUsed` | `fill_own_used[queue]`: own frames staged and counted this phase |
| `ownTaken`, `ownLeft` | `fill_own_frame[client]` cleared and set; `fill_own_frames_left` |
| `claims` | `fill_claims[queue]` |
| `budget` | `fill_budget[queue]`: the grant cap, raised by an atomic max, never lowered in a phase |
| `seenS`, `seenO`, `seenR` | the three reads of `queue_budget_now` |
| `starved` | ghost: a claim was refused while the room it read had a frame for it |

## Actions and the code they mirror

- **`OpenFill`**: main, with no fill running (`send_packets` after the flush,
  and `update` after its body: `rocketnet_server_transport_open_fill_budget`).
  The update's protocol frames have taken up to the reserve; the budget opens
  at the room above the reserve, the counts and claims are zeroed, and each
  connected client whose keep-alive is acknowledged gets its own frame.
- **`OwnClaim`, `OwnIo`, `OwnCount`**: a client's first frame this phase,
  when it owns one, takes its reserved frame: `fill_claim` clears
  `fill_own_frame`, `io_start_packet` stages it (it leaves the room), and
  `fill_started` counts it in `fill_own_used`, which lowers the reserve the
  budget keeps back. A transport that refuses it (`fill_refused`) leaves it in
  the reserve: its client's keep-alive is still due.
- **`Claim`, `Check`**: any other frame adds one to the claims; an index
  below the budget is granted.
- **`Read1`, `Read2`, `Read3`**: at or past the budget,
  `rocketnet_server_transport_refresh_fill_budget` recomputes it from
  `queue_budget_now`: the two counts, then the room, then the two counts
  again, starting over if either moved. The budget is raised to `started +
  (room - (reserve - own_used))`, and the claims outstanding (this one among
  them) are tested against it. Refused, the claim is taken back and counted as
  `SEND_RESERVE_REFUSED`.
- **`Stage`, `Count`**: `io_start_packet` stages a granted frame, then
  `fill_started` counts it. A transport that refuses it takes the claim back
  (`fill_refused`).
- **`Retire`**: the transport retires a staged frame at any time.

## Properties

- **`ReserveKept`**: once the fill holds a frame, the room left after every
  frame in flight has staged still covers the reserve, less the own frames
  taken: `free + ownTaken >= Reserve + OwnUnstaged + Pending`. The next
  update's protocol frames are never refused for the fill's sake.
- **`NeverStarved`**: no claim is refused while the room its recompute read
  had a frame for it past the reserve and for every other filler in flight
  when it is refused. Room that comes back after the read, or that a filler
  claiming after the read takes, is not starvation: the fill as a whole still
  gets the frame.

## Configs

All run two fillers, a reserve of 2 in a queue of 4, one own frame, two frames
per filler, two retires and two phases.

| Config | Expected | What it shows |
|---|---|---|
| `MCRNSendReserve.cfg` | pass | the code: `TypeOK`, `ReserveKept`, `NeverStarved` |
| `MCRNSendReserveBrokenRoomOnly.cfg` | `ReserveKept` | the budget is the whole room, with no reserve |
| `MCRNSendReserveBrokenSplitClaim.cfg` | `ReserveKept` | the claim is a load then a store, so two fillers take one index |
| `MCRNSendReserveBrokenRoomFirst.cfg` | `ReserveKept` | the room is read before the counts, so a frame staged between is counted twice |
| `MCRNSendReserveBrokenOwnCountFirst.cfg` | `ReserveKept` | an own frame lowers the reserve before it leaves the room (the code before this model counted `fill_own_used` at the claim) |
| `MCRNSendReserveBrokenNoRefresh.cfg` | `NeverStarved` | the budget stays as main opened it, so frames retired during the phase never reach the fill (the first form of the reserve; the IOCP measurement of 2 frames an update at 1000 clients) |
| `MCRNSendReserveBrokenNoReread.cfg` | `NeverStarved` | the counts are read once, so a frame counted between them and the room read is lost to the fill |
| `MCRNSendReserveBrokenIndexTest.cfg` | `NeverStarved` | the recompute tests the claim's index, which a refused claim ahead of it has raised |

Each counterexample was read against the code by hand. `NoReread`: an own
frame is staged, its count lands between the count read and the room read,
and the budget comes out one frame low. `IndexTest`: a claim refused at a full
queue raised the next claim's index; a retire then gives room, and the index
test refuses a frame the claims outstanding would grant. `OwnCountFirst`: the
own count lands before its frame leaves the room, and a filler reads the
lowered reserve against the room that still holds it.

## What it does not cover

The protocol's own frames during a fill phase (the protocol sends from main,
outside the fill phase: `RNFillPhase`); the size of the reserve, which is the
formula above, checked by the unit tests in
`tests/test_unit_server_send_reserve.cpp`; several queues, which are
independent (each its own fields); and the C++ memory order, beyond what the
atomics the code uses (`rocketnet_platform_atomic_add_u64`, `_max_u64`,
`_load_u64`) give.

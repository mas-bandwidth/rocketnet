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
| `size`, `tail` | a claim's frames (one, or a chunk of `Chunk`); the granted frames a run has not yet staged |
| `budget` | `fill_budget[queue]`: the grant cap, raised by an atomic max, never lowered in a phase |
| `seenS`, `seenO`, `seenR` | the three reads of `queue_budget_now` |
| `starved` | ghost: a claim was refused while the room it read had a frame for it |
| `unnamed` | ghost: such a refusal came while no other filler held a chunk |

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
- **`Size`, `Claim`, `Check`**: any other frame belongs to a claim. The
  guard (two loads) sizes it: a chunk of `Chunk` frames while the budget less
  the claims leaves `Chunk x GuardWidth`, else one frame. One add puts the
  whole claim on the claims; a claim whose last frame is within the budget is
  granted.
- **`Read1`, `Read2`, `Read3`**: at or past the budget,
  `rocketnet_server_transport_refresh_fill_budget` recomputes it from
  `queue_budget_now`: the two counts, then the room, then the two counts
  again, starting over if either moved. The budget is raised to `started +
  (room - (reserve - own_used))`, and the claims outstanding (this one among
  them) are tested against it. Refused, the claim is taken back whole. A
  refused chunk is claimed again as one frame. A refused single claim is
  counted as `SEND_RESERVE_REFUSED`. The code claims one frame at a time
  (`Chunk` = 1), so no filler ever holds a chunk; a code with chunks would
  name a refusal while another filler holds one (its claim added and not
  yet taken back, or its tail not yet staged) apart from a true full, under a
  counter that exists only once that code does.
- **`Stage`, `Count`, `Continue`, `End`**: `io_start_packet` stages a
  granted frame from the run's tail, then `fill_started` counts it, one
  atomic add per frame on the queue's count. The run stages its next frame
  from the tail or ends at any frame; at its end what is left of the tail goes
  back with one add. A transport that refuses a frame ends the run, and the
  tail goes back the same way.
- **`Retire`**: the transport retires a staged frame at any time.

## Properties

- **`ReserveKept`**: once the fill holds a frame, the room left after every
  frame in flight has staged still covers the reserve, less the own frames
  taken: `free + ownTaken >= Reserve + OwnUnstaged + Pending`. The next
  update's protocol frames are never refused for the fill's sake.
- **`NeverStarved`**: no single claim is refused while the room its
  recompute read had a frame for it past the reserve and past every other
  filler in flight when it is refused, a filler counting as the frames it
  holds: its chunk (from the add until it is granted or taken back), the
  tail of it not yet staged, and a frame staged and not yet counted; at
  least one frame. Room that comes back after the read, or that a filler
  claiming after the read takes, is not starvation: the fill as a whole still
  gets the frame. With one-frame claims this is the property as it stood
  before chunks: one frame per filler in flight.
- **`TailsNamed`**: a single claim refused while the room it read had a frame
  for it past every filler in flight (one frame each) comes only while
  another filler holds a chunk, so a code with chunks can count it under its
  own name, apart from a true full (the model's comment names it
  SEND_RESERVE_REFUSED_TAILS_HELD; no such counter exists while the code's
  `Chunk` is 1, since then the refusal cannot happen). The slack chunks
  hold is at most fillers x (`Chunk` - 1) frames past the one frame each
  filler in flight is allowed anyway.

## Configs

All run two fillers, one own frame, two claims per filler, two retires and
two phases; a reserve of 2 in a queue of 4 except `Chunk`, a reserve of 1 in
a queue of 6 so that the guard lets chunks through.

| Config | Expected | What it shows |
|---|---|---|
| `MCRNSendReserve.cfg` | pass | the code with one-frame claims: `TypeOK`, `ReserveKept`, `NeverStarved`, `TailsNamed` |
| `MCRNSendReserveChunk.cfg` | pass | the code with chunks of 2 and the guard at 2 fillers |
| `MCRNSendReserveChunkNoGuard.cfg` | pass | chunks of 2 with no guard: the guard bounds the slack held, and no property rests on it |
| `MCRNSendReserveBrokenRoomOnly.cfg` | `ReserveKept` | the budget is the whole room, with no reserve |
| `MCRNSendReserveBrokenSplitClaim.cfg` | `ReserveKept` | the claim is a load then a store, so two fillers take one index |
| `MCRNSendReserveBrokenRoomFirst.cfg` | `ReserveKept` | the room is read before the counts, so a frame staged between is counted twice |
| `MCRNSendReserveBrokenOwnCountFirst.cfg` | `ReserveKept` | an own frame lowers the reserve before it leaves the room (the code before this model counted `fill_own_used` at the claim) |
| `MCRNSendReserveBrokenNoRefresh.cfg` | `NeverStarved` | the budget stays as main opened it, so frames retired during the phase never reach the fill (the first form of the reserve; the IOCP measurement of 2 frames an update at 1000 clients) |
| `MCRNSendReserveBrokenNoReread.cfg` | `NeverStarved` | the counts are read once, so a frame counted between them and the room read is lost to the fill |
| `MCRNSendReserveBrokenIndexTest.cfg` | `NeverStarved` | the recompute tests the claim's index, which a refused claim ahead of it has raised |
| `MCRNSendReserveBrokenNoFallback.cfg` | `NeverStarved` | a refused chunk is not claimed again as one frame, so a frame the room had for one claim is refused |
| `MCRNSendReserveBrokenNoTailReturn.cfg` | `TailsNamed` | a run's unused tail is never given back, so a later claim is refused with room while no chunk is held |

Each counterexample was read against the code by hand. `NoReread`: an own
frame is staged, its count lands between the count read and the room read,
and the budget comes out one frame low. `IndexTest`: a claim refused at a full
queue raised the next claim's index; a retire then gives room, and the index
test refuses a frame the claims outstanding would grant. `OwnCountFirst`: the
own count lands before its frame leaves the room, and a filler reads the
lowered reserve against the room that still holds it. `NoFallback`: one
filler claims a chunk of 2 with one frame above the reserve; the recompute
refuses the chunk and the filler stops. `NoTailReturn`: a run takes a chunk
of 2, stages one frame and ends; the other frame stays on the claims, and the
next single claim is refused at a budget that had room for it.

The two counts stay one atomic each per queue, added per frame (the shape
the F8 change kept). A form that counts per cache line and reads the lines
one at a time was tried and is not modelled: its re-read decides on the
counts as they stood at the room read, which the property, measured at the
decision, cannot yet state.

## What it does not cover

The protocol's own frames during a fill phase (the protocol sends from main,
outside the fill phase: `RNFillPhase`); the size of the reserve, which is the
formula above, checked by the unit tests in
`tests/test_unit_server_send_reserve.cpp`; several queues, which are
independent (each its own fields); and the C++ memory order, beyond what the
atomics the code uses (`rocketnet_platform_atomic_add_u64`, `_max_u64`,
`_load_u64`) give.

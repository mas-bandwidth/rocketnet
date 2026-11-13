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
game half of it; the own frames handed out are never more than the smallest
queue's reserve. In the models `Reserve` is that figure, whatever it is. The
game's packets may take the room above it, and frames the transport retires
before the budget is opened again (a completion port, a ring the kernel
drains) come back to them.

Two modules:

- `RNSendReserve.tla` is the design, one queue, several game threads at
  once. Every load, store, add, max and compare-and-swap of a shared word is
  a step of its own, at the code's layout (the fill lines below), but for the
  five steps "Steps and the code" lists, which each join two of the code's
  atomic operations.
- `RNSendReserveCalls.tla` is the same rules one whole call at a time, as one
  thread sees them, on `NQ` queues. It is what the trace check records, so it
  is the module the code is bound to (`tests/tla_trace_send_reserve.cpp`,
  `TraceRNSendReserveCalls.tla`, layer `sendreserve`).

## State

| Model | Code |
|---|---|
| `free` | the queue's free frames: `send_room_queue`, or `send_room` for one queue |
| `Lines`, `LineOf(f)` | `fill_lines[64]`: the line a game thread's id hashes to (`rocketnet_fill_phase_line_of`); `SharedLine` puts every filler on one line, else each on its own |
| `started[l]` | `fill_lines[l].started[queue]`: game frames staged and counted on line `l` since the budget opened; the budget's reads sum them one line at a time (`started_sum`) |
| `ownUsed` | `fill_own_used[queue]`: own frames staged and counted since then |
| `ownTaken`, `ownLeft` (`own` in the calls module) | `fill_own_frame[client]` taken and not refused; `fill_own_frames_left` |
| `claims` | `fill_claims[queue]` |
| `budget` | `fill_budget[queue]`: the grant cap, raised by an atomic max, never lowered until it is opened again |
| `tail[l]` | the low half of `fill_lines[l].tails[queue]`: frames granted to the line and not yet taken |
| `ChunkHold(g)` (derived from `pc`, `size`) | the high half of that word: a chunk claim from the guard's add until it is granted or taken back |
| `size`, `claim` | a start's claim (`ROCKETNET_SERVER_FILL_CHUNK`, 8, or 1) and the index its add returned |
| `ret` | the line's tail a refused frame took with its compare-and-swap, plus its own claim, until the add that gives them back (`fill_refused`'s `rest + 1`) |
| `ld` | a filler's last load: the budget in the guard and after the max, the claims in the retest |
| `rpos`, `racc` | a sum read one line at a time: `started_sum`, or `tails_held` |
| `seenS`, `seenO` | `queue_budget_now`'s first reads of the started sum and the own frames used |
| `seenR` | its room read, and with it a ghost record: own frames taken, other fillers in flight, their holds, the lines' tails, and the frames claimed since (`a`) |
| `roomy` | ghost: the decision that refused a single claim read room for it past every filler in flight |
| `refused` (calls module) | the counter `SEND_RESERVE_REFUSED` |
| `starved`, `unnamed` | ghosts: a refusal `NeverStarved` or `TailsNamed` forbids |

## RNSendReserve: actions and the code they mirror

- **`OpenFill`**: main, with no game packet being started (`send_packets`
  after the flush, and `update` after its body:
  `rocketnet_server_transport_open_fill_budget`). The update's protocol frames
  have taken up to the reserve; every line's tail goes back and its words and
  started counts are zeroed (`return_fill_tails`); the budget opens at the
  room above the reserve, the claims and own frames used are zeroed, and each
  connected client whose keep-alive is acknowledged gets its own frame.
- **`OwnClaim`, `OwnIo`, `OwnCount`**: a client's first frame since the budget
  opened, when it owns one, takes its reserved frame first, whatever its
  thread's line holds: `fill_claim` takes `fill_own_frame` and lowers
  `fill_own_frames_left`, `io_start_packet` stages it (it leaves the room), and
  `fill_started` counts it in `fill_own_used`, which lowers the reserve the
  budget keeps back. A transport that refuses it (`fill_refused`) leaves it in
  the reserve: its client's keep-alive is still due.
- **`Continue`**: a frame of the line's tail, taken with one
  compare-and-swap on the line's word (the loop at the top of `fill_claim`);
  no write to the claims.
- **`Size`**: on a line with no tail, the guard: the budget loaded, then the
  claims; a chunk while the budget less the claims leaves a chunk for every
  line (`Chunk * GuardWidth`; the code: 8 for each of 64 lines), its claim
  added to the line's high half; else one frame.
- **`Claim`, `Check`**: the claim added to the claims in one add
  (`claim_frames`); granted when the whole claim fits under the budget loaded
  then. A granted chunk's first frame is this start's, and the rest moves
  from the high half to the tail in one add.
- **`Read1`, `Read2`, `Read3`, `Decide`**: past the budget,
  `refresh_fill_budget` recomputes it from `queue_budget_now`: the lines'
  started counts summed one line at a time and the own frames used, then the
  room, then those two again, starting over if either moved. `Decide` is the
  atomic max to `started + (room - (reserve - own_used))` (the raise), then
  the test of the claims outstanding, this one among them, against the budget
  loaded after the max.
- **`TakeBack`, `Retest`**: a refused claim is taken back whole. A chunk
  drops its high half and is claimed again as one frame. A single claim is
  retested: the claims loaded, then the budget; a frame now under the budget
  is claimed again, else the start is refused and the counter is picked by
  reading every line's word, one line at a time (`tails_held`):
  `SEND_RESERVE_REFUSED_TAILS_HELD` when any tail or chunk claim is read,
  else `SEND_RESERVE_REFUSED`.
- **`Stage`, `Count`, `End`**: `io_start_packet` stages a granted frame, then
  `fill_started` counts it on the line. The transport may refuse it at any
  time, room or not (no link-layer address, a full lease table, a full
  queue): `fill_refused` takes the line's whole tail with one
  compare-and-swap (`Stage`'s other branch), then gives it and the frame's
  claim back to the claims in one add (`End`).
- **`Retire`**: the transport retires a staged frame at any time.

## Steps and the code

Every other step is one atomic operation of the code, or a load with the
filler's own arithmetic on it. Five steps join two of the code's operations:

| Step | The code's two operations | Why the join keeps what the model checks |
|---|---|---|
| `OwnClaim` | the add on `fill_own_frame[client]`, then the add on `fill_own_frames_left` | the frames left are read only by the capacity calls, which this module has none of; the model has no clients, so a start that finds the flag taken and puts its one back is not a step here (the calls module and the unit tests cover the flag) |
| `Size` (from `sizing`) | the guard's load of the claims, then the add of the chunk claim to the line's high half | only `tails_held` reads the high half, on the refusal path; the join lets another filler's counter read see the chunk claim one step early, which can only name its refusal `SEND_RESERVE_REFUSED_TAILS_HELD`, so a `TailsNamed` that passes does not cover a refusal counted in that one-step window |
| `Check`, `Decide` granting a chunk (`Grant`) | the test's load, then the add that moves the chunk's rest from the high half to the tail | the line's sum read by `tails_held` is the same before and after the add; a filler on the same line may take a tail frame one step early here, where the code would send it to the guard |
| `Decide` (the test) | the budget loaded after the max, then the claims loaded | the budget only rises within a phase, so the budget the code loaded is one of the values from the max's result (`ld`) to the budget now: the step grants when the claims now fit the budget now and refuses when they do not fit `ld`, every outcome the two loads allow |
| `TakeBack` of a chunk | the claims' add of `-size` in `claim_frames`, then the add that drops the high half in `fill_claim` | between them the claims no longer hold the chunk while the line still shows it: a counter read there names `SEND_RESERVE_REFUSED_TAILS_HELD`, as with `Size`, and nothing else reads the high half |

`fill_refused`'s compare-and-swap and its add are two steps (`Stage`, `End`),
and the retest's two loads are two steps, because properties turn on what
another filler reads between them.

## RNSendReserveCalls: one action per call

| Action | Call | RNSendReserve steps it stands for |
|---|---|---|
| `Open(used, acked)` | `send_packets` after an update whose protocol frames took `used[q]` frames of each queue `q`; each queue's reserve is the formula cut to half the largest room seen on it, and the own frames handed out are at most the smallest queue's reserve | `OpenFill` |
| `StartStaged(c)` | `start_packet` gave a frame | `OwnClaim`, `OwnIo`, `OwnCount`; or `Continue`, `Stage`, `Count`; or `Size`, `Claim`, `Check`, `Read1-3`, `Decide`, `Stage`, `Count` |
| `StartIoRefused(c)` | `start_packet` gave nothing: the transport refused the frame | `OwnClaim`, `OwnIo` refused; or `Continue` or `Size`, `Claim`, `Check`, `Read1-3`, `Decide`, then `Stage` refused, `End` |
| `StartRefused(c)` | `start_packet` gave nothing: past the budget, counted | `Size`, `Claim`, `Check`, `Read1-3`, `Decide`, `TakeBack`, `Retest` |
| `Finish`, `Abort` | `finish_packet`, `abort_packet` | none: the reserve's words stay; an aborted frame's claim is not given back |
| `Capacity(n)` | `send_capacity_available` returned `n` | none: a read of the room, the budget as it would be raised, the claims and the own frames left |
| `CapacityTo(c, n)` | `send_capacity_to(c)` returned `n`: the client's own queue's room and budget left, with its own frame | none: a read |
| `CapacityAll(n)` | `send_capacity_to(-1)` returned `n`: the room summed over the queues, within the whole budget left (0 with one queue) | none: a read |
| `Retire(q, n)` | the transport frees `n` frames of queue `q` | `Retire`, `n` times |

Every word but the own frames is per send queue (`NQ` queues, client `c` on
queue `(c % NQ) + 1`); with `NQ = 1` the module is the one-queue model it was.
With one thread there is one fill line, so `started` and `tail` are that
line's.

The refinement is stated in the module's header and argued, not checked by
TLC: each action is a run of RNSendReserve steps by one thread with no other
thread's step between them. The calls module's own invariants are checked,
and are RNSendReserve's two properties at the points between calls, plus
`ClaimsAreStarted` (with one thread every claim outstanding is a frame
staged or a tail held), `OwnLeftCounts`, and `OwnWithinReserve`, which is
RNSendReserve's assumption `MaxOwn <= Reserve` made a property of the open.

## Properties

- **`ReserveKept`**: once a game frame is granted, the room left after every
  frame in flight has staged still covers the reserve, less the own frames
  taken: `free + ownTaken >= Reserve + OwnUnstaged + Pending`, where
  `Pending` is the lines' tails and the frames granted and not yet staged.
  The next update's protocol frames are never refused for the game packets'
  sake.
- **`NeverStarved`**: no single claim is refused while the room its
  recompute read had a frame for it past the reserve, past every other
  filler in flight (at least one frame each, its chunk claim and the frames
  it is giving back when more), past every line's tail, and past the frames
  the other fillers claimed after that read; nor refused for good while the
  claims its retest loaded leave a frame under the budget. Room that comes
  back after the read is not starvation.
- **`TailsNamed`**: a refusal the code counts as `SEND_RESERVE_REFUSED` (its
  read of the lines found no tail and no chunk claim) had no room, at the
  decision that refused it, for its frame past the reserve and every filler
  in flight. That is the counter's documented meaning
  (`sdk/rocketnet_counters.h`): the plain counter is a true full, and the
  refusals that frames other threads held caused are counted apart. The
  code keeps it while every frame held is on a line word (`MCRNSendReserve`,
  `MCRNSendReserveChunk`), and breaks it when the held frames are on no line
  word: a tail a refused frame took and has not given back, or a tail frame
  taken and not yet staged (`MCRNSendReserveCodeTailsNamed`, a code defect
  handed up under MM076). It has no reversed witness of its own until the
  code keeps it on an instance where one could fire.
- **`OwnWithinReserve`** (RNSendReserveCalls): the own frames left and used
  on a queue are never more than the smallest queue's reserve.
- **`NoHiddenRoom`** (RNSendReserveCalls): a client's capacity is never
  answered 0 while a start to it would stage, so one full queue never hides
  another's room from the send pass and the sweep.
- **`ReserveRestored`** (RNSendReserveCalls): each queue's reserve is the
  formula cut to half the largest room seen on it, so a reserve cut while a
  queue's room was small grows back once larger room is seen.

## Configs

RNSendReserve: two fillers, two starts each, one own frame, two retires and
two openings; a reserve of 2 in a queue of 4 unless the row says otherwise.
Each filler is on its own line unless the row says one line.

| Config | Expected | What it shows |
|---|---|---|
| `MCRNSendReserve.cfg` | pass | the code with single claims: `TypeOK`, `ReserveKept`, `NeverStarved`, `TailsNamed` |
| `MCRNSendReserveChunk.cfg` | pass | chunks of two with the guard at two lines, a reserve of 1 in a queue of 6: all four |
| `MCRNSendReserveSharedLine.cfg` | pass | the Chunk instance on one line: both fillers take from one tail, and a refused frame gives back the tail the other filled; `TypeOK`, `ReserveKept`, `NeverStarved` |
| `MCRNSendReserveChunkNoGuard.cfg` | pass | chunks of two with no guard, no own frame and one retire: no property rests on the guard; `TypeOK`, `ReserveKept`, `NeverStarved` |
| `MCRNSendReserveCodeTailsNamed.cfg` | `TailsNamed` | the code with chunks of two and no guard: a refusal another filler's held frames caused is counted `SEND_RESERVE_REFUSED` while those frames are on no line word (MM076, handed up) |
| `MCRNSendReserveBrokenRoomOnly.cfg` | `ReserveKept` | the budget is the whole room, with no reserve |
| `MCRNSendReserveBrokenSplitClaim.cfg` | `ReserveKept` | the claim is a load then a store, so two threads take one index |
| `MCRNSendReserveBrokenRoomFirst.cfg` | `ReserveKept` | the room is read before the counts, so a frame staged between is counted twice |
| `MCRNSendReserveBrokenOwnCountFirst.cfg` | `ReserveKept` | an own frame lowers the reserve before it leaves the room |
| `MCRNSendReserveBrokenNoRefresh.cfg` | `NeverStarved` | the budget stays as main opened it, so frames retired afterwards never reach the game's packets |
| `MCRNSendReserveBrokenNoReread.cfg` | `NeverStarved` | the counts are read once, so a frame counted between them and the room read is lost |
| `MCRNSendReserveBrokenIndexTest.cfg` | `NeverStarved` | the recompute tests the claim's index, which a refused claim ahead of it has raised |
| `MCRNSendReserveBrokenNoRetry.cfg` | `NeverStarved` | a claim taken back is refused with no retest, so two threads refused by each other's claims are both refused with a frame left |
| `MCRNSendReserveBrokenNoFallback.cfg` | `NeverStarved` | chunks of two, no guard: a refused chunk is not claimed again as one frame |
| `MCRNSendReserveBrokenBatchCount.cfg` | `NeverStarved` | chunks of two, no guard: staged frames are counted at the filler's next start, so the recompute reads too few |
| `MCRNSendReserveBrokenNoTailReturn.cfg` | `NeverStarved` | chunks of two, no guard: a refused frame takes its line's tail and gives back its own claim only |

RNSendReserveCalls: two clients, a reserve of 3 in a queue of 5, one frame a
claim, bounded at three refusals and seven frames staged per opening; and
the chunked claims in a queue of 8 with a reserve of 2, chunks of three and
a guard of three.

| Config | Expected | What it shows |
|---|---|---|
| `MCRNSendReserveCalls.cfg` | pass | the code: `TypeOK`, `ReserveKept`, `NeverStarved`, `ClaimsAreStarted`, `OwnLeftCounts`, `OwnWithinReserve` |
| `MCRNSendReserveCallsBrokenRoomOnly.cfg` | `ReserveKept` | the budget is the whole room |
| `MCRNSendReserveCallsBrokenNoRefresh.cfg` | `NeverStarved` | a start is refused while retired frames stand above the reserve |
| `MCRNSendReserveCallsBrokenAbortReturns.cfg` | `ReserveKept` | an aborted packet gives its claim back though its frame left the room |
| `MCRNSendReserveCallsChunk.cfg` | pass | the code with chunked claims: a chunk while the guard holds, its tail taken a frame a start, given back on a refused frame and at the next opening |
| `MCRNSendReserveCallsBrokenTailLeak.cfg` | `NeverStarved` | a refused frame gives back its own claim and not the tail held with it |
| `MCRNSendReserveCallsQueues.cfg` | pass | two queues, two clients, a reserve of 1 in queues of 3, a first room seen of 1: every invariant with `NoHiddenRoom` and `ReserveRestored` |
| `MCRNSendReserveCallsBrokenMinRoom.cfg` | `NoHiddenRoom` | the fewest room of any queue answers for every client, as `send_capacity_available` did for the send pass and the sweep |
| `MCRNSendReserveCallsBrokenStickyReserve.cfg` | `ReserveRestored` | a reserve cut by a small room seen never grows back |
| `MCRNSendReserveCallsBrokenOwnUnbounded.cfg` | `OwnWithinReserve` | the Queues instance with an open that hands every acknowledged client its own frame, past the smallest queue's reserve |

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
OwnCountFirst, NoReread, IndexTest and NoRetry have no call-level form, nor
does `TailsNamed`.

## What it does not cover

The size of the reserve, which is the formula and the half above, checked by the unit tests
in `tests/test_unit_server_send_reserve.cpp` (the calls model carries only the
cut to half the room seen); several queues in RNSendReserve, whose steps are
per queue and independent (each its own fields), checked by the same tests
and by RNSendReserveCalls at the call level; more than one filler on a line
taking a client's own frame (the flag's contest); the 64 lines and the chunk
of 8, which the model runs at two lines and a chunk of two; and the C++
memory order, beyond what the atomics the code uses
(`rocketnet_platform_atomic_add_u64`, `_max_u64`, `_cas_u64`, `_load_u64`)
give.

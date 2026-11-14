# RNRioLaneLayout

The RIO server transport's lanes at their layout. With the server's
`num_rio_lanes` above one, `rocketnet_server_rio_create`
(`sdk/rocketnet_server_rio_windows.cpp`) opens that many lanes, each a whole
single-socket transport with its own worker, request queue, completion queues
and registered region, all bound to the one port. A frame's lane is its
destination's hash; the packet id the server hands back to finish or abort is
the only thing that carries the lane from start to finish. This model checks
that arithmetic (`sdk/rocketnet_rio_layout.h`), and the order a lane posts its
slots in, at a small capacity.

## State

| Model | Code |
|---|---|
| `L` | `rocketnet_server_rio_t.num_lanes` (`rocketnet_server_rio_lane_count` of `options.lanes`, the server's `num_rio_lanes`) |
| `F` | `rocketnet_server_rio_t.lane_slots`: the send phases times a lane's send queue size, the span of a lane's ids; the slots `Start` takes are one phase's (see below) |
| `Hashes` | the destinations, each named by its `rocketnet_rio_lane_hash` (of its family, port and address) |
| `slot[l][i]` | send slot `i` of lane `l`: `<<>>` when free, else `<<dest, start, state>>`: reserved (`"res"`), finished with its length (`"live"`, `slot_bytes > 0`) or aborted (`"zero"`) |
| `next[l]` | lane `l`'s reservation counter (`rio->next[fill]`) |
| `issued` | ids `start_packet` returned that are not yet finished or aborted |
| `started`, `posted` | ghosts: each destination's start numbers, and those posted in post order |
| `starts` | ghost: `start_packet` calls |
| `strayed` | ghost: a decode named a lane outside `0 .. L - 1` |
| `misrouted` | ghost: a finish or abort reached a slot it did not reserve, or a foreign id reached a lane |

## Actions and the code they mirror

- **`Start(d)`**: `rocketnet_server_rio_start_packet`: lane
  `rocketnet_rio_lane_of( hash, L )` (the high half folded in, then `% L`),
  that lane's next slot `i = next[lane]` (`rocketnet_rio_lane_start_packet`,
  `next[fill].fetch_add( 1 )`), id `rocketnet_rio_lane_packet_id( lane, i, F )`
  = `lane * F + i`. Here a lane refuses once its `F` slots are used; the code
  refuses (`SEND_FULL`) once the open phase's slot reaches the send queue size
  `Q`, which is `F / 3` (the abstraction below), and its counter still
  advances, which changes nothing here.
- **`Finish(id, keep)`**: `rocketnet_server_rio_finish_packet` (keep: the
  slot's length) or `rocketnet_server_rio_abort_packet` (the slot stays zero):
  `rocketnet_rio_lane_packet_decode` refuses an id at or past `L * F`,
  otherwise lane `id / F`, slot `id % F`; the lane finds the slot from the
  frame pointer (`rocketnet_rio_frame_index`) and refuses, counting
  `SEND_FAILED` on a finish, a frame that is not the slot the id names. The
  slot stays reserved either way.
- **`Foreign(id)`**: a finish with an id start never issued: the largest id
  and ids just past the last lane.
- **`Send(lane)`**: the lane's worker posts its reserved slots in slot order,
  which is their reservation order (`rocketnet_rio_post_phase` walks slot 0 up
  to the count), skips the zero ones, and the slots are free again. It waits
  until no slot of the lane is reserved and unfinished: the main thread hands a
  phase with the fill threads joined, every start finished or aborted.

The code reserves within the open fill phase (index `phase * Q + slot`); here
one counter runs over the lane's `F` ids, which is the code's order within a
phase, and `Send` posts and frees them all at once. How the three phases take
turns and the slot's hold until its completion are
[RNRioSendSlotLayout](README-RNRioSendSlotLayout.md)'s.

## Invariants

- **`LaneInRange`**: every decoded lane is in `0 .. L - 1`.
- **`FinishOwn`**: a finish or abort of an outstanding id reaches exactly the
  slot its start reserved; a foreign id past the lanes reaches none. An id
  below `L * F` that is not outstanding (already finished, or never issued)
  decodes to its own slot, and with that slot's frame pointer the code acts on
  it: no generation is carried, so finishing or aborting each start once is the
  caller's contract, and this model does not hand finish such an id.
- **`NoReissue`**: every issued id names its own reserved slot, so no slot is
  handed to two frames at once. It holds by `Start` taking the lane's next
  unused slot; its witness is `NoSlotBound` below.
- **`OneLanePerDest`**: every frame reserved and not yet sent for one
  destination is in one lane.
- **`InOrder`**: each destination's frames are posted in the order they were
  started: the start numbers posted to it strictly increase. It does not say
  that every finished frame is posted or that no aborted one is; `Send` skips
  the zero slots, and no invariant here checks that.

## The instances

`L = 3` lanes, `F = 2` slots, destinations `d1`, `d2`, `d3` with hashes 0, 1
and 4 (lanes 0, 1 and 1), at most five starts. Terminal states are the
harness's bound (`ignore-terminal`). The model's hashes are below 2^16, where
the fold is the identity; `test_rio_transport_lane_hash_and_lane`
(`tests/test_unit_rio_layout.cpp`) pins the fold and the hash on full-width
values.

- **`MCRNRioLaneLayout.cfg`**: the code. Passes all five invariants.
- **`MCRNRioLaneLayoutBrokenNoBound.cfg`**: the decode without its bound
  check. Fails `LaneInRange` in one step: `Foreign(6)` decodes lane
  `6 / 2 = 3`, past the lanes array. The code refuses it at
  `packet_id >= lanes * lane_slots` in `rocketnet_rio_lane_packet_decode`.
- **`MCRNRioLaneLayoutBrokenModLane.cfg`**: the lane decoded as `id % L`.
  Fails `FinishOwn`: a start for `d2` takes lane 1 slot 0, id 2; its finish
  decodes lane `2 % 3 = 2`, slot 0, which is empty. The code decodes
  `2 / 2 = 1`. This is also the trace check's plant
  (`ROCKETNET_RIO_LANE_PLANT`).
- **`MCRNRioLaneLayoutBrokenLeastFull.cfg`**: staging in the lane with the
  fewest slots used instead of the destination's. Fails `OneLanePerDest`: two
  starts for `d1` take lane 0 slot 0 and lane 1 slot 0, and `d1` has frames in
  two lanes, which two workers may post in either order. The code always
  stages in `rocketnet_rio_lane_of` of the destination's hash.
- **`MCRNRioLaneLayoutBrokenReverse.cfg`**: the worker posts a lane's slots
  from the last down. Fails `InOrder`: two starts for `d1` take lane 0 slots 0
  and 1, both finish, and the send posts start 1 before start 0. The code's
  `rocketnet_rio_post_phase` walks `slot = 0 .. count - 1`.
- **`MCRNRioLaneLayoutBrokenNoSlotBound.cfg`**: start without its refusal
  once the slots are used (`L = 2`, `F = 1`, one destination, two starts).
  Fails `NoReissue`: the first start takes lane 0 slot 0, id 0; the second
  takes slot 1, past the lane, and hands id 1, which decodes to lane 1 slot 0,
  a slot start never reserved. In the code, where a lane's ids span its
  phases, the slot past a phase is the next phase's slot 0, or past the last
  phase the next lane's: either way a slot this start did not reserve. The
  code refuses at
  `slot >= send_queue_size` in `rocketnet_rio_lane_start_packet`
  (`ROCKETNET_COUNTER_RIO_SEND_FULL`).

Each counterexample was checked against the code by hand.

## What the trace binds

The trace layer `rio-lane` (`tests/tla_trace_rio_lane.cpp`) calls only the
header arithmetic: `rocketnet_rio_lane_hash`, `rocketnet_rio_lane_of`,
`rocketnet_rio_lane_packet_id` and `rocketnet_rio_lane_packet_decode`. The
slot table, each lane's counter, the outstanding ids, the post order and the
started and posted orders are the walk's own, written from the code by hand;
it calls none of the transport's start, finish, abort, send or post. So
`NoReissue`, `OneLanePerDest` and `InOrder` are checked against state the walk
wrote. The Windows callers (the address byte selection in
`rocketnet_server_rio_lane_of`, the index cast in
`rocketnet_server_rio_finish_packet`, the lane's frame-pointer check) are not
bound either. Driving the transport's real start, finish and send path is a
roadmap entry.

## Not modelled here

The phases and the slot's hold until its completion:
`RNRioSendSlotLayout.tla`'s `NoReissue`.

The lanes' sockets, workers, receive banks and the prefilter: each lane is the
single-lane transport `RNRioWorker.tla`, `RNRioDriver.tla` and
`RNRioSendSlotLayout.tla` check, and the prefilter in the worker is
`RNRioWorker.tla`'s `NoGarbageBanked`. The port shared by the lanes: with more
than one lane every lane socket is given its processor by `SIO_CPU_AFFINITY`
before bind (no `SO_REUSEADDR` or `SO_EXCLUSIVEADDRUSE`, which cannot be
combined with it), after an exclusive probe bind of an explicit port
(`rocketnet_rio_exclusive_probe`); a single lane binds with
`SO_EXCLUSIVEADDRUSE`. That is tested on real sockets in `tests/test_iocp.cpp`
(`test_rio_transport_lanes_refuse_a_held_port`,
`test_rio_transport_lanes_later_binder_receives_nothing`,
`test_rio_transport_lanes_port_zero_never_held`).

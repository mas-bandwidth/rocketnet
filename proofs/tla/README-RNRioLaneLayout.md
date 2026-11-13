# RNRioLaneLayout

The RIO server transport's lanes at their layout. With `num_xdp_queues` above
one, `rocketnet_server_rio_create` (`sdk/rocketnet_server_rio_windows.cpp`)
opens that many lanes, each a whole single-socket transport with its own
worker, request queue, completion queues and registered region, all bound to
the one port. A frame's lane is its destination's hash; the packet id the
server hands back to finish or abort is the only thing that carries the lane
from start to finish. This model checks that arithmetic
(`sdk/rocketnet_rio_layout.h`) at a small capacity.

## State

| Model | Code |
|---|---|
| `L` | `rocketnet_server_rio_t.num_lanes` (`options.lanes`, from `num_xdp_queues`) |
| `F` | `rocketnet_server_rio_t.lane_slots`: the send phases times a lane's send queue size |
| `Hashes` | the destinations, each named by its `rocketnet_rio_lane_hash` (of its family, port and address) |
| `slot[l][i]` | send slot `i` of lane `l`: the destination (its hash) staged there, or `-1` |
| `issued` | ids `start_packet` returned that are not yet finished or aborted |
| `staged` | finished frames not yet posted, with their lane, in finish order |
| `starts` | ghost: `start_packet` calls |
| `strayed` | ghost: a decode named a lane outside `0 .. L - 1` |
| `misrouted` | ghost: a finish or abort reached a slot it did not reserve, or a foreign id reached a lane |

## Actions and the code they mirror

- **`Start(d)`**: `rocketnet_server_rio_start_packet`: lane
  `rocketnet_rio_lane_of( hash, L )` (the high half folded in, then `% L`),
  a free slot `i` of that lane, id `rocketnet_rio_lane_packet_id( lane, i, F )`
  = `lane * F + i`. A full lane refuses.
- **`Finish(id, keep)`**: `rocketnet_server_rio_finish_packet` (keep) or
  `rocketnet_server_rio_abort_packet`: `rocketnet_rio_lane_packet_decode`
  refuses an id at or past `L * F`, otherwise lane `id / F`, slot `id % F`;
  the lane finds the slot from the frame pointer (`rocketnet_rio_frame_index`)
  and refuses, counting `SEND_FAILED` on a finish, a frame that is not the
  slot the id names.
- **`Foreign(id)`**: a finish with an id start never issued: the largest id
  and ids just past the last lane.
- **`Send(lane)`**: a lane's worker posts the oldest frame staged in it.

## Invariants

- **`LaneInRange`**: every decoded lane and every staged frame's lane is in
  `0 .. L - 1`.
- **`FinishOwn`**: a finish or abort reaches exactly the slot its start
  reserved; a foreign id reaches none.
- **`NoReissue`**: every issued id names its own slot, so no slot is handed
  to two frames at once.
- **`OneLanePerDest`**: every staged frame for one destination is in one
  lane, so the lane's FIFO is the destination's order.

## The instances

`L = 3` lanes, `F = 2` slots, destinations `d1`, `d2`, `d3` with hashes 0, 1
and 4 (lanes 0, 1 and 1), at most five starts. Terminal states are the
harness's bound (`ignore-terminal`). The model's hashes are below 2^16, where
the fold is the identity; `test_rio_transport_lane_hash_and_lane`
(`tests/test_unit_rio_layout.cpp`) pins the fold and the hash on full-width
values.

- **`MCRNRioLaneLayout.cfg`**: the code. Passes all four invariants.
- **`MCRNRioLaneLayoutBrokenNoBound.cfg`**: the decode without its bound
  check. Fails `LaneInRange` in one step: `Foreign(6)` decodes lane
  `6 / 2 = 3`, past the lanes array. The code refuses it at
  `packet_id >= lanes * lane_slots` in `rocketnet_rio_lane_packet_decode`.
- **`MCRNRioLaneLayoutBrokenModLane.cfg`**: the lane decoded as `id % L`.
  Fails `FinishOwn` in two steps: `Start(d1)` takes lane 0 slot 1, id 1; its
  finish decodes lane `1 % 3 = 1`, slot 1, which is empty. The code decodes
  `1 / 2 = 0`. This is also the trace check's plant
  (`ROCKETNET_RIO_LANE_PLANT`).
- **`MCRNRioLaneLayoutBrokenLeastFull.cfg`**: staging in the lane with the
  most free slots instead of the destination's. Fails `OneLanePerDest` in
  four steps: two starts for `d1` take lane 0 slot 0 (id 0) and lane 1 slot 0
  (id 2); both finish, and `d1` has frames staged in lanes 0 and 1, which two
  workers may post in either order. The code always stages in
  `rocketnet_rio_lane_of` of the destination's hash.

Each counterexample was checked against the code by hand.

## Not modelled here

The slot's reuse: the model frees a slot at finish, where the code holds the
reservation until its phase is handed off and retired; that is
`RNRioSendSlotLayout.tla`'s `NoReissue`.

The lanes' sockets, workers, receive banks and the prefilter: each lane is the
single-lane transport `RNRioWorker.tla` and `RNRioSendSlotLayout.tla`
check, and the prefilter in the worker is
`RNRioWorker.tla`'s `NoGarbageBanked`. The port shared by the lanes (an
exclusive probe bind first, then `SO_REUSEADDR`) is tested on real sockets in
`tests/test_iocp.cpp`.

# RNIocpSendLanes

The IOCP server's send lanes (`sdk/rocketnet_server_iocp_windows.cpp`). The
server keeps `rocketnet_server_iocp_send_lanes( max_clients )` lanes. Each lane
has `rocketnet_server_iocp_send_queue_size( max_clients )` send operations (at
most 4096), its own free queue, its own three staging banks and its own TX
worker. A destination's packets all stage in one lane, the one its address hash
names, so that lane's worker can group them and post them in order.

## State

| Model | Code |
|---|---|
| `free[l]` | lane `l`'s free queue (`rocketnet_iocp_lane_t::free_sends`) |
| `bank[l]` | lane `l`'s staged packets in staged order, its three banks seen as one sequence |
| `inflight` | the operations posted and awaiting their completion |
| `started`, `posted` | ghosts: each destination's start and post order |

Operation `i` belongs to lane `i \div Size`, since the operations are carved
lane by lane (`rocketnet_iocp_lane_of_index`). The low `IndexBits` bits of a
packet handle name the operation (`rocketnet_iocp_packet_id`, 16 in the code).
A static assert there holds `ROCKETNET_IOCP_MAX_LANES * ROCKETNET_IOCP_LANE_PACKETS`
to 65,536.

## Actions

| Action | Code |
|---|---|
| `Start(d, l, i)` | `rocketnet_server_iocp_start_packet`: `l = DestLane(d)` (`rocketnet_iocp_lane_index`, FNV-1a over the fields `rocketnet_address_equal` compares, given as the set `DestLaneAt` of `d * Lanes + l`), then that lane's free operation `i`, the one the pop returns |
| `StartRefused(d, l)` | `rocketnet_server_iocp_start_packet` when lane `l` has no free operation: nothing changes |
| `Post(l)` | `rocketnet_iocp_send_worker` for lane `l` over `rocketnet_iocp_send_bank` |
| `Complete(i)` | `rocketnet_iocp_retire_send`, then `rocketnet_iocp_release_send` to the lane its index names |

## Invariants

- `Conserved`: every operation is in exactly one place: a free queue, a bank, or in flight.
- `OwnLane`: a lane's free queue and bank hold only its own operations.
- `InOrder`: each destination's packets are posted in the order they were started.
- `HandleUnique`: no two live operations share a handle's index bits.

The three-bank hand-off inside a lane is checked by
[RNBankFifoLayout](README-RNBankFifoLayout.md), the free queue's cells by
RNIocpRefQueue, the lease generations by RNIocpSendLease, and the run ring by
RNIocpRunRing. Grouping inside a bank is the send gather's (RNSendGatherLayout).

## Instances

| Config | Expected |
|---|---|
| `MCRNIocpSendLanes.cfg` | two lanes of two, three destinations (two share lane 0), six starts: pass |
| `MCRNIocpSendLanesThree.cfg` | three lanes of two, one destination each: pass |
| `MCRNIocpSendLanesBrokenAnyLane.cfg` | a packet stages in any lane: `InOrder` fails |
| `MCRNIocpSendLanesBrokenReleaseMod.cfg` | release to lane `i % Lanes`, the carve read the wrong way: `OwnLane` fails |
| `MCRNIocpSendLanesBrokenIndexBits.cfg` | one index bit for four operations: `HandleUnique` fails |

The unit tests `tests/test_unit_iocp_send_lanes.cpp` run the code's lane hash:
equal addresses share a lane, every lane is in range, and a thousand players
spread within a quarter of the mean over 4, 8 and 16 lanes.

# RNIocpSendLanes

The IOCP server's send lanes (`sdk/rocketnet_server_iocp_windows.cpp`). The
server keeps `rocketnet_server_iocp_send_lanes( max_clients )` lanes. Each lane
has `rocketnet_server_iocp_send_queue_size( max_clients )` send operations (4096,
or the smallest power of two that holds every client slot and the headroom
when that is more, above 3072 clients), its own free queue and its own three
staging banks, and is posted by the send thread that owns it. A destination's
packets all stage in one lane, the one its address hash names, so that lane's
send thread can group them and post them in order.

## State

| Model | Code |
|---|---|
| `free[l]` | lane `l`'s free queue (`rocketnet_iocp_lane_t::free_sends`) |
| `bank[l]` | lane `l`'s staged packets in staged order, its three banks seen as one sequence |
| `inflight` | the operations posted and awaiting their completion |
| `started`, `posted` | ghosts: each destination's start and post order |
| `postedBy[l]` | ghost: the send threads that ever posted lane `l` |
| `zeroed` | the `<<destination, start>>` of every packet whose slot was left at zero bytes (`op->packet_bytes == 0`): while it is in a bank this is the slot's length; after the post it is history |

Operation `i` belongs to lane `i \div Size`, since the operations are carved
lane by lane (`rocketnet_iocp_lane_of_index`). The low `IndexBits` bits of a
packet handle name the operation (`rocketnet_iocp_packet_id`, 16 in the code).
A static assert there holds `ROCKETNET_IOCP_MAX_LANES * ROCKETNET_IOCP_LANE_PACKETS`
to 65,536, and a lane is of `ROCKETNET_IOCP_LANE_PACKETS` unless every client slot
plus the headroom needs more (above 3072 clients), within the same 65,536.

## Actions

| Action | Code |
|---|---|
| `Start(d, l, i)` | `rocketnet_server_iocp_start_packet`: `l = DestLane(d)` (`rocketnet_iocp_lane_index`, FNV-1a over the fields `rocketnet_address_equal` compares, given as the set `DestLaneAt` of `d * Lanes + l`), then that lane's free operation `i`, the one the pop returns |
| `StartRefused(d, l)` | `rocketnet_server_iocp_start_packet` when lane `l` has no free operation: nothing changes |
| `Abort(i)` | `rocketnet_server_iocp_abort_packet` on staged operation `i`, or a `rocketnet_server_iocp_finish_packet` that refuses its length (counted `SEND_FAILED`): the slot keeps zero bytes |
| `Post(t, l)` | `rocketnet_iocp_send_worker` of send thread `t` for lane `l` over `rocketnet_iocp_send_bank`; `l` is one of `ThreadLanes(t)`. Each zero-byte slot's operation goes back to the free queue of the lane its index names (`rocketnet_iocp_release_send`), unposted; the rest is posted in staged order |
| `Complete(i)` | `rocketnet_iocp_retire_send`, then `rocketnet_iocp_release_send` to the lane its index names |

`start_packet` reserves the slot with `packet_bytes = 0` and `finish_packet`
writes the length; the model starts a packet finished and `Abort` leaves it
zero, which covers the code because the caller finishes or aborts every start
before `send_packets` publishes the bank. Not modelled: `start_packet`'s
refusal after its pop (an address the socket cannot carry), which releases the
operation to its lane's tail and starts no packet; and a refused post (no
completion), which releases the operation as `Complete` does.

## Invariants

- `Conserved`: every operation is in exactly one place: a free queue, a bank, or in flight.
- `OwnLane`: a lane's free queue and bank hold only its own operations.
- `InOrder`: what was posted to each destination is exactly its started
  packets that have left their bank and were not left zero, in start order:
  an aborted packet is never posted, a finished one is posted once, and none
  overtakes an earlier one.
- `HandleUnique`: no two live operations share a handle's index bits.
- `OnlyOwnerPosts`: a lane is only ever posted by thread `l % Threads`.
- `OneOwner`: every lane is in exactly one thread's lanes, so none is starved
  (no thread takes it) and none is double-posted (two take it). It is a
  predicate over the constants, so it holds or fails at the initial state
  (TLC reports a failure as an error). It checks the guard's partition, not
  behaviour.
- `OnlyOwnerPosts` is the invariant with force over the behaviours: `postedBy`
  is written by `Post`, and the invariant checks it against the true
  arithmetic `l % Threads`, never against the guard `Post` uses, so a defect
  put in the guard (the shift, the inversion) is caught by what was posted.
  Both reversed witnesses fall it at the code's two threads.

## The send threads

`Threads` send threads (`rocketnet_server_iocp_send_threads( lanes )`, at most
`ROCKETNET_IOCP_SEND_THREADS`, 2, and at most the lanes) post the lanes: lane
`l` belongs to thread `l % Threads` (`rocketnet_server_iocp_send_thread_of_lane`)
and thread `t` takes lanes `t`, `t + Threads`, ...
(`rocketnet_iocp_send_thread_lanes`, the walk the transport's send
threads, the model's `ThreadLanes` and the trace harness share, in
`sdk/rocketnet_server_iocp_windows.cpp`, with `rocketnet_server_iocp_send_threads` and `rocketnet_server_iocp_send_thread_of_lane` in `sdk/rocketnet_server_iocp.h`). The arithmetic `l % Threads` is the model's too,
not abstracted. The trace layer `iocp-lanes` records the thread and the lane
each post names, and the code's walk gives both.

## What the trace binds

The trace layer `iocp-lanes` (`tests/tla_trace_iocp_lanes.cpp`) calls the
code's lane free queues (the transport's own ref queue, through
`rocketnet_server_iocp_test_lanes_*`), its destination hash, its release lane
and its send threads' lane walk, and reads every free queue back after each
step. The banks, the in-flight set, the started and posted orders, `zeroed`
and `postedBy` are the walk's own, written from the model's rules: it calls
no `start_packet`, `finish_packet`, `abort_packet`, `rocketnet_iocp_send_bank`,
gather or post. So `InOrder` and `Conserved` are checked against state the
walk wrote; the trace shows the free queues and the arithmetic are the
model's, not that the transport's banks and posts are. Driving the real
staging, bank and post path is a roadmap entry.

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
| `MCRNIocpSendLanesFour.cfg` | four lanes, two send threads, four destinations: pass |
| `MCRNIocpSendLanesBrokenOwnerShift.cfg` | thread `t` posts the lanes with `(l + 1) % Threads = t`, one past its own: `OnlyOwnerPosts` fails |
| `MCRNIocpSendLanesBrokenOwnerInvert.cfg` | the ownership test inverted (`l % Threads # t`), two threads: `OnlyOwnerPosts` fails |
| `MCRNIocpSendLanesBrokenAbortPosted.cfg` | the send posts a zero-byte slot as if finished: an aborted packet is posted, `InOrder` fails |
| `MCRNIocpSendLanesBrokenAbortStranded.cfg` | the send neither posts nor releases a zero-byte slot: its operation is nowhere, `Conserved` fails |

The unit tests `tests/test_unit_iocp_send_lanes.cpp` run the code's lane hash:
equal addresses share a lane, every lane is in range, and a thousand players
spread within a quarter of the mean over 4, 8 and 16 lanes.

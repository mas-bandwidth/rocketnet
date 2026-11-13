# RNXdpReceiveRing

The AF_XDP receive ring and the ownership of its frames: the XDP worker's
receive pass in `rocketnet_server_socket_xdp_thread_function`
(`sdk/rocketnet_server_socket_linux.cpp`), `rocketnet_xdp_receive_ring_finish`
in the same file, the pending return (`rocketnet_pending_frames_t`,
`sdk/rocketnet_pending_frames.h`) and the fill ring the kernel reads through a
cached batch. The module header carries the full description; this file holds
the code each action mirrors, the witnesses and the runs.

## Actions and the code they mirror

The worker's steps are the functions of `sdk/rocketnet_xdp_receive_pass.h`,
which the worker calls in this order.

- **`Peek`**: `rocketnet_xdp_receive_peek`, `xsk_ring_cons__peek( &socket->receive_queue, receive_limit, ... )`,
  where `receive_limit` is the lesser of `receive_capacity`
  (`ROCKETNET_XDP_RECV_QUEUE_SIZE - receive_buffer->num_packets`, the staging
  room, `StageCap - staged`) and `pending_return.available()` (`PendCap -
  Cardinality(pending)`). The peek returns what libxdp's cached view of the
  ring holds up to that, and reads the producer again (`cprod`, the whole
  ring) only when that view is empty (`xsk_cons_nb_avail`).
- **`Take`**: `rocketnet_xdp_receive_take`, one iteration of the loop over
  `num_packets`: the descriptor is read, its frame goes to the pending return
  when `rocketnet_xdp_fill_return` places it in the receive region, and a
  packet is staged. The loop has no room test; the `rocketnet_assert` on the
  staging room is compiled out of a release build. What keeps it in bounds is
  the peek's cap.
- **`Finish`**: the loop ends with every peeked descriptor read
  (`num_consumed == num_packets`); `rocketnet_xdp_receive_end` calls
  `rocketnet_xdp_receive_ring_finish`, which cancels `num_packets -
  num_consumed`, which is zero, and releases `num_consumed`, and appends the
  pass's frames to the pending return; then `rocketnet_xdp_fill_flush` flushes
  it, all or none.
- **`Refill`**: `rocketnet_xdp_fill_flush` at the top of the next pass.
- **`Drain`**, **`KernelReceive`**, **`KernelRefresh`**,
  **`KernelMisdeliver`**: as the module header says.

A frame is its number in the UMEM (the cases use frames 1 to 3; frame 0, the
send region's first, is `SendFrame`). The first fill writes the frames in
frame order, and `Init` does too (`FrameOrder`), so the trace check's record 1
is the model's one initial state. A flush writes them in the pending return's
order, which the model leaves free (`Orders`): no rule depends on it, and the
trace pins it.

## The peek's two room terms

The staging term binds. The staging buffer holds copies of packets, and it
fills across passes while the frames go back to the fill ring every pass; a
peek without it reads more descriptors than the buffer has slots and the loop
writes past `receive_buffer`. `StagingInBounds` states it and
`MCRNXdpReceiveRingBrokenPeekPastStaging.cfg` drops it.

The pending term does not bind in the code. The pending return holds
`ROCKETNET_XDP_RECV_QUEUE_SIZE` frames, which equals
`ROCKETNET_XDP_FILL_FRAMES`, and it only ever holds receive frames (a refused
descriptor is not appended), so its room is never less than the receive frames
the RX ring can name. The design cfg carries the term at the code's ratio
(`PendCap` is the frames), where `PendingInBounds` cannot fail.
`MCRNXdpReceiveRingPendBinds.cfg` sets `PendCap` to two, below the three frames
and the RX ring of three, so the term binds and must keep `PendingInBounds`;
`MCRNXdpReceiveRingBrokenPeekPastPending.cfg` drops the term on that instance
and fails it. The pair checks the term as written, not a ratio the code reaches.

A defective descriptor naming a receive-region frame the worker does not hold
(one on the fill ring, or one already in the RX ring) is left out:
`rocketnet_xdp_fill_return` tests the region only, so the code hands such a
frame back and it sits on the fill ring twice. `KernelMisdeliver` names only a
send-region frame, the trace never posts the other kind, and
`FrameConservation` says nothing about it.

## The cancel

`rocketnet_xdp_receive_ring_finish` cancels the peeked descriptors a pass did
not read before it releases the ones it read. Because the peek is capped by the
room and the loop reads every descriptor, v1.1.x never cancels a nonzero count.
`Finish` keeps the code's arithmetic (`back == peeked - taken`) and is enabled
only at `taken = peeked`. The cancel is a defence for a pass the code cannot
make; `tests/test_unit_xdp_receive_ring.cpp` drives it over a fake ring. The
earlier witness `MCRNXdpReceiveRingBrokenNoCancel.cfg` removed the cancel on a
path where a pass stopped with descriptors unread, which the code cannot take;
it is gone with that path.

## The instance

`MCRNXdpReceiveRing.cfg`: three frames, an RX ring of two, a staging buffer
and a pending return of three each (the code's ratio), a fill ring of six
(twice the frames, `ROCKETNET_XDP_FILL_RING_SIZE`). With two packets staged
and two descriptors on the ring, the peek is cut to one by the staging room.

`MCRNXdpReceiveRingPendBinds.cfg`: the same with an RX ring of three and a
pending return of two, so a peek of three is cut to two by the pending room.
It runs every invariant and `ReceiveNeverStalls`.

## Reversed witnesses

Each counterexample was checked against the C by hand.

1. **`MCRNXdpReceiveRingBrokenPeekPastStaging.cfg`**
   (`PeekStagingRoom <- PeekStagingRoomBroken`): `receive_limit` without
   `receive_capacity`. Violates `StagingInBounds` (14 states): the kernel
   posts frame 1 and frame 2; a pass peeks two (staging room 3, pending room 3), reads
   both, stages 2, releases 2 and flushes both frames back. The application
   has not taken the buffer. The kernel posts frame 3 and, after a refresh, frame 1;
   the next pass has staging room 1 but asks for the pending room, 3, and the
   peek returns 2; the loop writes `receive_buffer` index 2, then index 3,
   one past `ROCKETNET_XDP_RECV_QUEUE_SIZE` (3 here). With the term the peek
   asks for 1 and the second descriptor waits for the next pass.
2. **`MCRNXdpReceiveRingBrokenFillOfTheFrames.cfg`** (`FillSize = 3`): a fill
   ring of exactly the frames. Violates `FillNeverEmptyWhileFramesFree`
   (8 states): the kernel's batch holds all three frames; a pass takes frame 1 and
   its flush finds no slot (`xsk_ring_prod__reserve` gives zero: the batch's
   taken entries keep their slots), so frame 1 stays in the pending return; the
   kernel takes frame 3, the last entry, and the fill ring is empty while the
   worker holds frame 1.
3. **`MCRNXdpReceiveRingBrokenRefusedRefilled.cfg`**
   (`RefusedRefilled <- RefusedRefilledBroken`): a refused descriptor's frame
   handed back like any dropped frame. Violates `FillHoldsOnlyReceiveFrames`
   (5 states): a defect posts a descriptor naming a send frame; the pass reads
   it, `rocketnet_xdp_receive_descriptor` refuses it, and without
   `rocketnet_xdp_fill_return`'s region test the flush puts the send frame on
   the fill ring.
4. **`MCRNXdpReceiveRingBrokenPeekPastPending.cfg`**
   (`PeekPendingRoom <- PeekPendingRoomBroken`, on the `PendBinds` instance):
   `receive_limit` without `pending_return.available()`. Violates
   `PendingInBounds` (93 states): the kernel posts frames 1, 2 and 3; the pass
   asks for the staging room, 3, peeks three and holds all three frames in a
   pending return of two. With the term the peek asks for 2 and the third
   descriptor waits for the next pass.

## Runs

Bench runs of this version (aws-mut-3, 2026-11-13):

| Case | Result | Distinct | Seconds |
|---|---|---|---|
| `MCRNXdpReceiveRing.cfg` | pass | 6,551 | 1.3 |
| `MCRNXdpReceiveRingPendBinds.cfg` | pass | 12,798 | 1.6 |
| `MCRNXdpReceiveRingBrokenPeekPastStaging.cfg` | `StagingInBounds`, exit 12 | 303 | 0.7 |
| `MCRNXdpReceiveRingBrokenFillOfTheFrames.cfg` | `FillNeverEmptyWhileFramesFree`, exit 12 | 39 | 0.7 |
| `MCRNXdpReceiveRingBrokenRefusedRefilled.cfg` | `FillHoldsOnlyReceiveFrames`, exit 12 | 21 | 0.7 |
| `MCRNXdpReceiveRingBrokenPeekPastPending.cfg` | `PendingInBounds`, exit 12 | 93 | 0.7 |

The records are in `RUNS.tsv` (group `rnxdpreceivering`), made with the pinned
runner as `proofs/tla/README.md` describes.

## The trace check

Layer `xdp-receive-ring` of `tools/tla_trace_check.py` (`README-TraceCheck.md`)
binds the model to the code. `tests/tla_trace_xdp_receive_ring.cpp` lays out
an RX ring, a fill ring, a UMEM of six frames (two send frames, four receive
frames) and a staging buffer, and drives them through libxdp's own ring views
and the functions of `sdk/rocketnet_xdp_receive_pass.h`, playing the kernel
itself. Each step is one record; `TraceRNXdpReceiveRing.tla` checks the
records are a behaviour of this module. Odd seeds use a fill ring of eight
slots, even seeds one of four, the frames' count, so a flush that finds no
room (`Refill`) is reached.

The walk names each receive frame by its number in the UMEM (2 to 5) and
any descriptor outside the receive region as `SendFrame`, 0; the cfg it
writes is plain values (`Frames = {2, 3, 4, 5}`).

The plant (`ROCKETNET_XDP_RECEIVE_RING_PLANT` in
`sdk/rocketnet_xdp_receive_pass.h`, built only into
`tla_trace_plant_xdp_receive_ring`, which includes
`sdk/rocketnet_server_socket_linux.cpp` with it defined) is the reversed witness
`PeekPastStaging` in the code: the peek asks for the pending return's room
alone. The walk reports the peek record where more was peeked than the
staging room, and TLC rejects that record.

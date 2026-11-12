# RNXdpReceiveRing

The AF_XDP receive ring and the ownership of its frames: the XDP worker's
receive pass in `rocketnet_server_socket_xdp_thread_function`
(`sdk/rocketnet_server_socket_linux.cpp`), `rocketnet_xdp_receive_ring_finish`
in the same file, the pending return (`rocketnet_pending_frames_t`,
`sdk/rocketnet_pending_frames.h`) and the fill ring the kernel reads through a
cached batch. The module header carries the full description; this file holds
the code each action mirrors, the witnesses and the runs.

## Actions and the code they mirror

- **`Peek`**: `xsk_ring_cons__peek( &socket->receive_queue, receive_limit, ... )`,
  where `receive_limit` is the lesser of `receive_capacity`
  (`ROCKETNET_XDP_RECV_QUEUE_SIZE - receive_buffer->num_packets`, the staging
  room, `StageCap - staged`) and `pending_return.available()` (`PendCap -
  Cardinality(pending)`). The peek returns what the ring holds up to that.
- **`Take`**: one iteration of the loop over `num_packets`: the descriptor is
  read, its frame goes to the pending return when `rocketnet_xdp_fill_return`
  places it in the receive region, and a packet is staged. The loop has no
  room test; the `rocketnet_assert` on the staging room is compiled out of a
  release build. What keeps it in bounds is the peek's cap.
- **`Finish`**: the loop ends with every peeked descriptor read
  (`num_consumed == num_packets`); `rocketnet_xdp_receive_ring_finish` cancels
  `num_packets - num_consumed`, which is zero, and releases `num_consumed`;
  then the pending return is flushed, all or none.
- **`Refill`**, **`Drain`**, **`KernelReceive`**, **`KernelRefresh`**,
  **`KernelMisdeliver`**: as the module header says.

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
the RX ring can name. The model carries the term at the code's ratio (`PendCap`
is the frames) and checks `PendingInBounds`; a witness dropping it would pass,
so there is none.

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

## Reversed witnesses

Each counterexample was checked against the C by hand.

1. **`MCRNXdpReceiveRingBrokenPeekPastStaging.cfg`**
   (`PeekStagingRoom <- PeekStagingRoomBroken`): `receive_limit` without
   `receive_capacity`. Violates `StagingInBounds` (14 states): the kernel
   posts F1 and F2; a pass peeks two (staging room 3, pending room 3), reads
   both, stages 2, releases 2 and flushes both frames back. The application
   has not taken the buffer. The kernel posts F3 and, after a refresh, F1;
   the next pass has staging room 1 but asks for the pending room, 3, and the
   peek returns 2; the loop writes `receive_buffer` index 2, then index 3,
   one past `ROCKETNET_XDP_RECV_QUEUE_SIZE` (3 here). With the term the peek
   asks for 1 and the second descriptor waits for the next pass.
2. **`MCRNXdpReceiveRingBrokenFillOfTheFrames.cfg`** (`FillSize = 3`): a fill
   ring of exactly the frames. Violates `FillNeverEmptyWhileFramesFree`
   (8 states): the kernel's batch holds all three frames; a pass takes F1 and
   its flush finds no slot (`xsk_ring_prod__reserve` gives zero: the batch's
   taken entries keep their slots), so F1 stays in the pending return; the
   kernel takes F3, the last entry, and the fill ring is empty while the
   worker holds F1.
3. **`MCRNXdpReceiveRingBrokenRefusedRefilled.cfg`**
   (`RefusedRefilled <- RefusedRefilledBroken`): a refused descriptor's frame
   handed back like any dropped frame. Violates `FillHoldsOnlyReceiveFrames`
   (5 states): a defect posts a descriptor naming a send frame; the pass reads
   it, `rocketnet_xdp_receive_descriptor` refuses it, and without
   `rocketnet_xdp_fill_return`'s region test the flush puts the send frame on
   the fill ring.

## Runs

Bench runs of this version (two workers):

| Case | Result | Distinct | Seconds |
|---|---|---|---|
| `MCRNXdpReceiveRing.cfg` | pass | 5,006 | 1.1 |
| `MCRNXdpReceiveRingBrokenPeekPastStaging.cfg` | `StagingInBounds`, exit 12 | 222 | 0.8 |
| `MCRNXdpReceiveRingBrokenFillOfTheFrames.cfg` | `FillNeverEmptyWhileFramesFree`, exit 12 | 39 | 0.7 |
| `MCRNXdpReceiveRingBrokenRefusedRefilled.cfg` | `FillHoldsOnlyReceiveFrames`, exit 12 | 21 | 0.7 |

The records are in `RUNS.tsv` (group `rnxdpreceivering`), made with the pinned
runner as `proofs/tla/README.md` describes.

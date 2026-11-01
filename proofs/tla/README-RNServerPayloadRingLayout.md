# RNServerPayloadRingLayout

The portable server transport's payload ring, at the layout of
`sdk/rocketnet_server_transport.cpp` and with the code's own arithmetic. Game
threads fill it between updates through the acquire/commit surface, and the
flush sends what they finished. The staged transports (io_uring, RIO, IOCP,
XDP, msgx) and the arena path for writes above one packet do not use it.

## State

| Model | Code |
|---|---|
| `count` | `payload_ring.num_packets`, the fetch-add counter |
| `info[i]`, `[wb, who]` | `payload_ring.info[i]`: `wire_bytes`, and `to` / `client_index` (here the thread) |
| a thread's `frame` | `context->frame - payload_ring.data`, or the fill frame's `data` |
| a thread's `slot` | the slot the fetch-add returned (ghost for the frame path; `frame->slot` for the fill path) |
| `Cap` | `send_capacity`: `max_clients`, never below `ROCKETNET_SERVER_MIN_SEND_PACKETS` |
| `RingSize` | `ROCKETNET_SERVER_PAYLOAD_RING_SIZE` (`ROCKETNET_MAX_CLIENTS`) |
| `Stride` | `ROCKETNET_FRAME_STRIDE` |

`send_capacity` is at most the ring: the floor
`ROCKETNET_SERVER_MIN_SEND_PACKETS` (256) is below `ROCKETNET_MAX_CLIENTS`, which
`CMakeLists.txt` holds to 512..32768, the configured client count's own bound, and the test seam's
`packets <= ROCKETNET_SERVER_MAX_SEND_PACKETS`. The counter has no bound of its
own between flushes: every refused acquire still moves it. The model bounds
acquires per flush by `MaxCount`, above `Cap`, so refused acquires are reached.
A frame is an offset into `data`; each packet carries `wire_bytes` 1, so a
packet's identity is its slot.

The flush runs with no packet between start and finish: the caller's contract
that an update's sends from game threads end before its flush. A lease left
open across a flush is RNDriverSendStaging's.

## Actions and the code they mirror

- **`Acquire(t)`**: the portable branch of
  `rocketnet_server_transport_start_packet` and
  `rocketnet_server_reserve_fill_frame`: `slot =
  rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 )` (fetch-add, the
  old value), refused at `slot >= send_capacity`, else `info[slot]` written
  with `wire_bytes = 0` and the frame at `ring->data + slot *
  ROCKETNET_FRAME_STRIDE`.
- **`Finish(t)`**: the portable branch of
  `rocketnet_server_transport_finish_packet_unchecked`, which recovers the
  slot as `( context->frame - payload_ring.data ) / ROCKETNET_FRAME_STRIDE`,
  and `rocketnet_server_commit_fill_frame`, which keeps `frame->slot`:
  `wire_bytes` set.
- **`Abort(t)`**: the portable branch of
  `rocketnet_server_transport_abort_packet_unchecked` (the slot from the frame
  pointer) and `rocketnet_server_abort_fill_frame`: `wire_bytes` cleared.
- **`Flush`**: the portable flush of
  `rocketnet_server_transport_flush`: `num_packets` clamped to
  `send_capacity`, every slot below it with `wire_bytes > 0` handed to the
  link, `num_packets = 0`.

## Invariants

- **`InBounds`**: no slot at or past `send_capacity`, and no `info` record past
  the ring, is handed out, written or read by the flush.
- **`ExactSend`**: each flush sends exactly the slots finished since the last
  one: no finished packet lost, no aborted or refused one sent.
- **`HeldExclusive`**: a held slot is below `Cap`, its frame is the slot's
  root, its `info` names its thread, and no two threads hold one slot.

## The instance

Frame stride 2 (the broken recovery divides by 1). The controls run three
threads at a capacity of three: `MCRNServerPayloadRingLayout.cfg` with the ring
exactly the capacity (the code's sizing at `ROCKETNET_MAX_CLIENTS` clients) and
four acquires per flush, `MCRNServerPayloadRingLayoutSpare.cfg` with a ring one
larger (fewer clients) and five. The witnesses run two threads at a capacity
of two.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNServerPayloadRingLayoutBrokenNoClamp.cfg`** (`Clamp = FALSE`, ring of
   three): the flush without `if ( num_packets > send_capacity ) num_packets =
   send_capacity`. Violates `InBounds`: two acquires take slots 0 and 1 and
   finish; a third acquire gets 2, which is the capacity, and is refused, but
   `num_packets` is now 3; the flush walks `i < 3` and reads `info[2]`, a
   record no acquire of this update wrote. At the code's sizing (ring equal to
   the capacity) that read is past the array.
2. **`MCRNServerPayloadRingLayoutBrokenFullOffByOne.cfg`** (`FullTest = FALSE`,
   ring of two): the refusal test `slot > send_capacity`. Violates `InBounds`:
   slots 0 and 1 are handed out, the third acquire gets 2, equal to the
   capacity and the ring size, and is not refused, so it writes `info[2]` and
   is handed `data + 2 * ROCKETNET_FRAME_STRIDE`, both past the ring.
3. **`MCRNServerPayloadRingLayoutBrokenWrongStride.cfg`** (`RecoverStride =
   FALSE`): the slot recovered as `( frame - data ) / ROCKETNET_FRAME_SIZE`
   (a size below the stride). Violates `ExactSend`: slot 0 is finished
   (offset 0 recovers 0); slot 1's frame is at offset 2 and recovers slot 2,
   so its `wire_bytes` lands in `info[2]` and `info[1]` stays 0; the flush
   walks `i < 2` and sends slot 0 alone, losing the finished packet in slot 1.

## Runs

The records are in `RUNS.tsv` (group `rnserverpayloadringlayout`), made with
the pinned runner as `proofs/tla/README.md` describes.

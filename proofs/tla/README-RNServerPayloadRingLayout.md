# RNServerPayloadRingLayout

The portable server transport's payload ring, at the layout of
`sdk/rocketnet_server_transport.cpp` and with the code's own arithmetic. Game
threads fill it between updates through the acquire/commit surface, and the
flush sends what they finished. The staged transports (io_uring, RIO, IOCP,
XDP, msgx) do not use it. Transport payloads fit within one packet.

## State

| Model | Code |
|---|---|
| `count` | `payload_ring.num_packets`, the fetch-add counter |
| `line[l]`, `[next, end]` | `fill_lines[l].ring`, `[end:32][next:32]`: the line's chunk, `next` the slot it gives next |
| `info[i]`, `[wb, who]` | `payload_ring.info[i]`: `wire_bytes`, and `to` / `client_index` (here the thread) |
| a thread's `pc` | where the thread is in `rocketnet_server_payload_ring_take` (`look`, `guard`, `claim`, `install`, `one`), or `held` between start and finish |
| a thread's `word` | the line word its last look loaded, the install's expected value |
| a thread's `base`, `end` | the chunk its claim got: the fetch-add's old value, and `base + ROCKETNET_SERVER_FILL_CHUNK` clipped at `send_capacity` |
| a thread's `frame` | `context->frame - payload_ring.data` |
| a thread's `slot` | the slot the take returned (ghost state) |
| `LineOf[t]` | `rocketnet_server_fill_line`: line 0 until the game names a client, then `rocketnet_fill_phase_line_of( thread id )`; threads may share a line |
| `Cap` | `send_capacity`: `max_clients`, never below `ROCKETNET_SERVER_MIN_SEND_PACKETS` |
| `RingSize` | `payload_ring.size`, allocated by `rocketnet_server_send_batch_packets(max_clients)` |
| `Stride` | `ROCKETNET_FRAME_STRIDE` |
| `Chunk` | `ROCKETNET_SERVER_FILL_CHUNK` (8 in the code) |
| `GuardLines` | `ROCKETNET_FILL_PHASE_FILL_LINES` (64), the guard's multiplier |

`send_capacity` is at most the ring: the capacity is the configured client
count raised to `ROCKETNET_SERVER_MIN_SEND_PACKETS` (256), while the ring is
that count raised to `ROCKETNET_SERVER_MIN_SIZED_CLIENTS` (512). Both are
bounded by the build ceiling. A test override is accepted only when it fits
`payload_ring.size`. The counter has no bound of its own between flushes:
every refused claim or single take still moves it. A take starts only while
the counter is below `MaxCount`, above `Cap`, so refused takes are reached. A
frame is an offset into `data`; each packet carries `wire_bytes` 1, so a
packet's identity is its slot.

The info records are not cleared by the flush. A slot below the counter that
no take of this update returned (an unused chunk slot, or a chunk whose
install lost) still holds the `wire_bytes` an earlier update left in it, and
only the two empty marks below keep it out of the send.

The flush runs with no take in progress and no packet between start and
finish: the caller's contract that an update's sends from game threads end
before its flush. A lease left open across a flush is RNDriverSendStaging's.
The DEV hook `steal_ring_chunk_for_test` stands for a second thread on the
line between a look and an install, which the model has as a real thread.

## Actions and the code they mirror

- **`Acquire(t)`**: one step of the portable branch of
  `rocketnet_server_transport_start_packet`, which is
  `rocketnet_server_payload_ring_take` on the calling thread's line and then
  `info[slot]` written with `wire_bytes = 0` and the frame at `ring->data +
  slot * ROCKETNET_FRAME_STRIDE`. The take's steps, each atomic in the code:
  - *look*: the line's word is loaded; with `next < end` one
    `rocketnet_platform_atomic_cas_u64( chunk, word, word + 1 )` takes `next`
    (a lost swap reloads, so the model takes it in one step). Otherwise the
    word is kept for the install.
  - *guard*: `used = num_packets` is read; with `used > capacity ||
    capacity - used < size * ROCKETNET_FILL_PHASE_FILL_LINES` the take goes to
    the single fetch-add, else to the chunk claim.
  - *claim*: `base = rocketnet_platform_atomic_add_u64( &ring->num_packets,
    size )`; `base >= capacity` is refused; `end` is `base + size` clipped at
    `capacity`; a chunk of one slot (`end == base + 1`) returns `base` with no
    install.
  - *install*: `rocketnet_platform_atomic_cas_u64( chunk, word, ( end << 32 )
    | ( base + 1 ) )` installs the chunk's tail and returns `base`. A line
    another thread on it refilled since the look keeps its chunk: the loop
    `for ( i = base; i < end; ++i ) ring->info[i].wire_bytes = 0;` marks this
    chunk empty and the take looks again.
  - *one*: `one = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 )`,
    refused at `one >= capacity`.
- **`Finish(t)`**: the portable branch of
  `rocketnet_server_transport_finish_packet_unchecked`, which recovers the
  slot as `( context->frame - payload_ring.data ) / ROCKETNET_FRAME_STRIDE`,
  and sets `wire_bytes`.
- **`Abort(t)`**: the portable branch of
  `rocketnet_server_transport_abort_packet_unchecked` (the slot from the frame
  pointer): `wire_bytes` cleared.
- **`Flush`**: the portable flush of `rocketnet_server_transport_flush`:
  `rocketnet_server_payload_ring_return_chunks` marks every line's unused
  slots, `[next, end)`, empty and clears the lines; then `num_packets` is
  clamped to `send_capacity`, every slot below it with `wire_bytes > 0` is
  handed to the link, and `num_packets = 0`.

## Invariants

- **`InBounds`**: no slot at or past `send_capacity`, and no `info` record past
  the ring, is handed out, written or read by the flush.
- **`ExactSend`**: each flush sends exactly the slots finished since the last
  one: no finished packet lost, no aborted or refused one sent, and no slot
  sent on the `wire_bytes` of an earlier update.
- **`HeldExclusive`**: a held slot is below `Cap`, its frame is the slot's
  root, its `info` names its thread, and no two threads hold one slot.
- **`ChunksBelowCap`**: a line's unused chunk lies below `send_capacity` and
  below the counter, and holds no slot a thread holds.

## The instance

Frame stride 2 (the broken recovery divides by 1), a chunk of two slots.
`MCRNServerPayloadRingLayout.cfg` runs three threads on one line at a capacity
of four, the ring exactly the capacity (as configured client counts at or
above 512 produce), the guard for one line and five as the take bound: the
chunk branch, its clip at the capacity, a lost install, the single fetch-add
and refused takes of both kinds are reached. `MCRNServerPayloadRingLayoutSpare.cfg`
runs three threads on two lines, two sharing one, with the guard for two
lines and a ring one larger than the capacity of four (the relation below the
ring's allocation floor). The witnesses run one or two threads on one line.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNServerPayloadRingLayoutBrokenNoClamp.cfg`** (`Clamp = FALSE`, capacity
   two, ring of three): the flush without `if ( num_packets > send_capacity )
   num_packets = send_capacity`. Violates `InBounds`: `t1` claims the chunk
   0..1; `t2` looks at the still empty line, finds the room below the guard
   and goes to the single fetch-add; `t1` installs, takes slot 0 and finishes
   it; `t2`'s fetch-add gets 2, the capacity, and is refused, but
   `num_packets` is now 3; the flush walks `i < 3` and reads `info[2]`, a
   record no take of this update wrote. At the code's sizing (ring equal to
   the capacity) that read is past the array.
2. **`MCRNServerPayloadRingLayoutBrokenFullOffByOne.cfg`** (`FullTest = FALSE`,
   ring of two): the refusal tests `base > send_capacity` and `one >
   send_capacity`. Violates `InBounds`: `t1` claims the chunk 0..1 and has
   not installed it yet; `t2` looks at the empty line, finds the room below
   the guard, and its single fetch-add gets 2, equal to the capacity and the
   ring size, which is not refused, so it writes `info[2]` and is handed
   `data + 2 * ROCKETNET_FRAME_STRIDE`, both past the ring.
3. **`MCRNServerPayloadRingLayoutBrokenWrongStride.cfg`** (`RecoverStride =
   FALSE`): the slot recovered as `( frame - data ) / ROCKETNET_FRAME_SIZE`
   (a size below the stride). Violates `ExactSend`: slot 0 is finished
   (offset 0 recovers 0); slot 1's frame is at offset 2 and recovers slot 2,
   so its `wire_bytes` lands in `info[2]` and `info[1]` stays 0; the flush
   walks `i < 2` and sends slot 0 alone, losing the finished packet in slot 1.
4. **`MCRNServerPayloadRingLayoutBrokenNoReturnClear.cfg`** (`ReturnClear =
   FALSE`, one thread, capacity two): the flush without
   `rocketnet_server_payload_ring_return_chunks`' empty marks. Violates
   `ExactSend`: the first update's claim takes the chunk 0..1, both slots are
   finished and sent; the next update's claim takes the chunk again, slot 0 is
   finished and slot 1 stays in the line unused. `num_packets` is 2, and slot
   1 still holds the first update's `wire_bytes`, so the flush sends it a
   second time.
5. **`MCRNServerPayloadRingLayoutBrokenNoLostClear.cfg`** (`LostClear = FALSE`,
   two threads on one line, capacity four): a lost install without the loop
   that marks its chunk empty. Violates `ExactSend`: in the first update `t1`
   claims the chunk 0..1, finishes slot 0 and the flush sends it; in the next
   both threads look at the empty line and pass the guard, `t1` claims 0..1
   and `t2` claims 2..3, `t2` installs its tail and takes 2, and `t1`'s install
   finds the line changed, so it looks again and takes slot 3 from `t2`'s
   chunk. Slots 2 and 3 are finished. Slot 0, of the lost chunk, is below
   `num_packets` (4) and still holds the first update's `wire_bytes`, so the
   flush sends it again.

## Runs

The records are in `RUNS.tsv` (group `rnserverpayloadringlayout`), made with
the pinned runner as `proofs/tla/README.md` describes.

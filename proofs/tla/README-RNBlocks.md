# RNBlocks: a block split, numbered, placed by index and delivered whole

`proofs/tla/RNBlocks.tla` is layer 5 of docs/CHANNELS.md (section 8, blocks) and section
15's `RNBlocks` row: the split at the fixed stride, the consecutive numbering, the
one contiguous ring span, and the receiver's placement by index with whole
delivery at one ordering slot, the sender's padding at the ring's end, and the
receiver's refusal of a chunk that disagrees with its block's anchor.
`proofs/tla/MCRNBlocks.tla` is the small instance; its seven cases (below) are the
`rnblocks` group in `proofs/tla/CASES.tsv`, measured by
`tlacheck` into `proofs/tla/RUNS.tsv` (proofs/tla/README.md, "Refreshing records after a model
edit").

## What the module covers

One sender and one receiver over one instance of `proofs/tla/Link.tla` (any copy lost,
duplicated or reordered). Sizes are in units of 16 bytes, as the record's ring
field is.

State the sender owns:

- `nextMsg`: the channel FIFO's head, messages `1..M` in order.
- `next`: the next reliable number.
- `ringNext`: `ring_next`, absolute units, starting at `RingStart`.
- `low`: the absolute ring start of number 0, the floor while nothing is acked.
- `rec[n]`: the record numbered `n`: its ordering index, fragment index, count
  `k`, ring offset (modulo `B`) and a tag `<<m, i>>` standing for fragment `i`
  of message `m`'s bytes.

State the receiver owns:

- `rcvd`: the numbers layer 3 has accepted (a second arrival is a duplicate and
  writes nothing, 6.5).
- `anchor[o]`: the block at ordering index `o`, the code's held key
  `(channel, ordering)`: chunks in (`have`), `k`, the first number and the
  span's base, opened by whichever fragment arrives first (section 8, "No start
  packet").
- `violated`: the receiver refused a chunk and the connection ended.
- `forged` (a ghost): the one forged record is on the wire.
- `ring`: the receive ring, unit to tag.
- `ordRead`: the channel's ordered read index.
- `log`: the deliveries in order (a ghost): the ordering index and the bytes read
  from the ring.

Actions and the code each one stands for (`sdk/rocketnet_channel.cpp`; each
function cites its action):

| Action | Who | What | Code |
|---|---|---|---|
| `NumberMessage` | the sender | the FIFO head is numbered: a message of at most `Small` units is one record; a larger one is `k = ceil(Units / F)` fragments taking `next .. next+k-1` at once and one span `[p, p + (k-1)F + last)`, fragment `i` at `p + iF`, both bounds of 6.2 checked once; every record carries ordering index `m - 1` | `rn_channels_finish` (the count `k`), `rn_ordered_write` calling `rn_window_send_first_write_block` (`sdk/rocketnet_window_send.cpp`) |
| `SendRecord(n)` | the sender | a numbered record is written into a packet: a first send or a resend, each fragment on its own; the resend delay is abstract | `rn_ordered_item` (index, `k - 1`, ring offset `ring_start % B`), `rn_ordered_walk` |
| `Receive(r)` | the receiver | a duplicate number writes nothing; a chunk (count 2 or more) is refused, ending the connection, when its index is not below its count, its base `ring - index·F` is outside the ring, or (`AnchorCheck`) its count, first number `n - index` or base disagrees with the anchor's (`Refused`); otherwise the anchor is found or opened by the ordering index, the fragment is copied to its own offset `ring + index·F` and `have` counts it | `rn_channels_dispatch_chunk` (`rn_block_shape_valid`, check 8's span test, the copy to `record->ring * 16`), `rn_channels_block_collect` (the agreement test, `MESSAGE_BAD_BLOCK`; `anchor->block_have++`); a single record is `rn_channels_dispatch_ordered` |
| `ForgeRecord(n, c)` | the outside world | with `Forge`, once: a copy of record `n` claiming count `c` goes on the wire | a peer whose chunk belongs to something else |
| `Deliver(o)` | the receiver | the message at the read index is complete: one view of its span read at stride `F`, and the read index moves by one | `rn_channels_dispatch_chunk` (`have == count`, `delta == 0`), `rn_channels_deliver_ordered`, `rn_channels_drain` |
| `Fwd!Environment` | the path | drop, duplicate, reorder | `proofs/tla/Link.tla` |

Invariants:

- `DeliveredEqualsSent`: a delivered message's bytes are the bytes that were sent,
  fragment by fragment in index order.
- `NoPartialDelivery`: a delivery is the whole message (`k` fragments) and every
  one of its numbers was received before it.
- `OneOrderingSlotPerBlock`: the deliveries take ordering indices `0, 1, 2, ...`,
  one each, and the read index equals the number of deliveries: a block moves it
  by one, never by its fragment count, and nothing is delivered twice.
- `SplitConsecutive`: the sender's split: consecutive numbers, one ordering index
  per message, indices `0..k-1`, fragment `i` at the block's offset plus `iF`.
- `SpanInsideRing`: every message's span lies inside one lap of the ring from
  its first record's offset (the padding's job).
- `NeverRefused`: a correct sender is never refused, padded or wrapped.
- `TypeOK`.

## What the module abstracts away

- **Acks and the window base.** Nothing is acked, so `U'` stays at the first number
  and the floor at the first span's start. The instance starts with the window
  empty and `ring_next` at `RingStart`, as earlier acked traffic left it; at
  `RingStart = 5` the padding branch and the wrap of offsets past `B` are
  reached. The instance is one window wide (four numbers, `W = 4`) and one ring
  long (at most 8 units from the floor), so no slot or ring unit is reused. Slot reuse under `U'` is `RNWindow`'s (`NoSlotCollision`) and ring reuse
  `RNRing`'s (`LiveSpansDisjoint`); a block mixed with another after numbers wrap
  is not reachable here.
- **The resend delay.** Any numbered record may be resent at any time, so every order of
  arrival the link and the sender can produce is explored.
- **Dispatch inside the receive.** The code delivers inside the dispatch call that
  completes the message and then drains; here `Deliver` is its own step, which
  only adds interleavings.
- **The receiver's checks** are modelled as one refusal (revision 1.1): the
  index below the count, the span inside the ring (every chunk to within one
  unit of the end, as the code checks a chunk not the last; the code checks
  the last chunk exactly, so the model is weaker there), and the count, first
  number and
  base against the anchor. The code's other refusals of a chunk (the slot
  collision, the completed block's first and last descriptors, the total
  against `max_message_bytes`) need a sender that lies in other ways and are
  not modelled. The held index's own refusal, an insert past `W` entries, is
  `RNHeldOwnership`'s `Hold`; nothing here fills the index. A forged record is
  a copy with another count; at most one is forged, because a peer that lies
  the same way about every chunk is only sending a different message.
- **Liveness** is not checked: the sender resends without bound and no fairness
  is stated. `RNWindow` checks that every reliable number is delivered.

## The instance

`W = 4`, `F = 2` units, `B = 8` units, `Small = 2` units, two messages: a single
record of 2 units (number 0, offset 0) and a block of 5 units, three fragments of
2, 2 and 1 units (numbers 1..3, offsets 2, 4, 6, ordering index 1). The link is
`"arbitrary"` with duplication and reordering, capacity 2, no delay.
`MCRNBlocksWrap.cfg` starts `ring_next` at 5 (`RingStart <- MCRingStartLate`):
the single takes units 5..6, the block's span of 5 would cross 8 from 7, so it
is padded to 8 and its offsets wrap to 0, 2, 4. Its state graph is the base
case's shifted, hence the same counts.

Measured by `tlacheck run` on aws-mut-3, 2026-11-13 (revision 1.1):

| Case | Result | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|
| `MCRNBlocks.cfg` (now with `SpanInsideRing`, `NeverRefused`) | every invariant holds | 11478 | 2421 | 3.4 |
| `MCRNBlocksWrap.cfg` | every invariant holds | 11478 | 2421 | 3.4 |
| `MCRNBlocksForged.cfg` | every invariant but `NeverRefused` holds | 66823 | 14703 | 10.2 |
| `MCRNBlocksBrokenPlaceByArrival.cfg` | `DeliveredEqualsSent` violated, exit 12 | 11447 | 2997 | 4.7 |
| `MCRNBlocksBrokenNoPad.cfg` | `SpanInsideRing` violated, exit 12 | 3 | 3 | 0.9 |
| `MCRNBlocksBrokenNoAnchorCheck.cfg` | `NoPartialDelivery` violated, exit 12 | 19344 | 5922 | 6.3 |
| `MCRNBlocksForgedRefused.cfg` | `NeverRefused` violated, exit 12 | 750 | 431 | 1.5 |

## Revision 1.1: padding, the wrap, and the anchor refusal

Before this revision every case started the ring at 0, so `Pos`'s padding
branch and the wrap of offsets past `B` were never reached, and the receiver
took every chunk with no check against its anchor. The traces of the new
witnesses, read by hand against the code:

- `MCRNBlocksBrokenNoPad.cfg`, 3 states: the single is numbered at 5..6, then
  the block at 7 with no padding, its span 7 + 5 = 12 past `B = 8`. In the
  code `rn_window_send_position` returns `ring_next + remaining` when the span
  exceeds the room left, so the block starts at 8; and a block placed at 7
  would fail the receiver's span check (`span_end > ring_bytes`, MESSAGE_BAD_RING).
- `MCRNBlocksForgedRefused.cfg`, 7 states: both messages numbered, number 1
  (fragment 0, count 3) sent and a copy of another fragment claiming another
  count forged; number 1 opens the anchor (count 3, base 2), and the forged
  chunk's count disagrees: refused, the connection ends. In the code
  `rn_channels_block_collect` finds the held entry and `anchor->block_count
  != count` counts MESSAGE_BAD_BLOCK.
- `MCRNBlocksBrokenNoAnchorCheck.cfg`, 11 states: number 0 is received; number
  1 (fragment 0, count 3) opens the anchor at count 3; a forged copy of number
  2 (fragment 1) claiming count 2 arrives next, the chunks in reach its count,
  and the block completes with count 2; both messages are delivered, the
  block with two of its three fragments. In the code without the agreement
  test the same order passes `anchor->block_have < count` (2 < 2 is false),
  the final chunk `first_number + count - 1` is live, and
  `rn_channels_block_settle` delivers two chunks. (The other order, the forged
  chunk first, is held in the code, since the correct chunk's count is 3;
  the model completes on the arriving chunk's count, as the code does.)

Two scratch invariants (not committed) confirmed the passing run is not vacuous:
`Len(log) < 2` is violated (both messages are delivered) and "fragment 2 received
while fragment 1 is not" is violated (out-of-order arrival is reached).

## The reversed witness: placement by arrival

`PlaceByIndex = FALSE` copies a fragment to `base + have·F`, the next free place
in arrival order: a reassembly that appends fragments as they arrive and
concatenates them in that order, never sorting them by index. TLC's trace, 13 states:

1. `NumberMessage` twice: number 0 is the single record at offset 0; numbers 1, 2,
   3 are the block's fragments 0, 1, 2 at offsets 2, 4, 6, ordering index 1.
2. `SendRecord(0)`, `SendRecord(1)`; `Receive` of number 0 (the single, at 0).
3. `SendRecord(3)` (fragment 2 written before fragment 1, as a resend of a lost
   fragment 1 would be); `Receive` of number 1 (fragment 0, `have = 0`, at 2).
4. `SendRecord(2)`; `Receive` of number 3 (fragment 2, `have = 1`, placed at
   `2 + 1·2 = 4`); `Receive` of number 2 (fragment 1, `have = 2`, placed at 6).
5. `Deliver` the single; `Deliver` the block reads offsets 2, 4, 6:
   `<<2,0>>, <<2,2>>, <<2,1>>`, not the sent `<<2,0>>, <<2,1>>, <<2,2>>`.

Replayed by hand against the code: `rn_ordered_item` gives every chunk its own
offset (`item->ring = (ring_start % ring_bytes) / 16`, `ring_start = p + i·F`
from `rn_window_send_first_write_block`), and `rn_channels_dispatch_chunk` copies
each chunk to `region->receive_ring + record->ring * 16`, its own offset, whatever
`block_have` is. In the same order of arrival the code writes fragment 2 at
offset 6 and fragment 1 at offset 4, and the delivered view
`receive_ring + first->ring * 16` holds the bytes in index order. The witness's
corruption is that arrival-order path's and is not reachable in the code.

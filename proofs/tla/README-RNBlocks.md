# RNBlocks: a block split, numbered, placed by index and delivered whole

`proofs/tla/RNBlocks.tla` is layer 5 of docs/CHANNELS.md (section 8, blocks) and section
15's `RNBlocks` row: the split at the fixed stride, the consecutive numbering, the
one contiguous ring span, and the receiver's placement by index with whole
delivery at one ordering slot. `proofs/tla/MCRNBlocks.tla` and `proofs/tla/MCRNBlocks.cfg` are
the small instance; `proofs/tla/MCRNBlocksBrokenPlaceByArrival.cfg` is the reversed
witness. Both are cases of the `rnblocks` group in `proofs/tla/CASES.tsv`, measured by
`tlacheck` into `proofs/tla/RUNS.tsv` (proofs/tla/README.md, "Refreshing records after a model
edit").

## What the module covers

One sender and one receiver over one instance of `proofs/tla/Link.tla` (any copy lost,
duplicated or reordered). Sizes are in units of 16 bytes, as the record's ring
field is.

State the sender owns:

- `nextMsg`: the channel FIFO's head, messages `1..M` in order.
- `next`: the next reliable number.
- `ringNext`: `ring_next`, absolute units.
- `rec[n]`: the record numbered `n`: its ordering index, fragment index, count
  `k`, ring offset (modulo `B`) and a tag `<<m, i>>` standing for fragment `i`
  of message `m`'s bytes.

State the receiver owns:

- `rcvd`: the numbers layer 3 has accepted (a second arrival is a duplicate and
  writes nothing, 6.5).
- `anchor[b]`: the block whose first number is `b`: chunks in (`have`), `k`, the
  ordering index and the span's base, opened by whichever fragment arrives first
  (section 8, "No start packet").
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
| `Receive(r)` | the receiver | a duplicate number writes nothing; otherwise the anchor is found or opened by `b = n - index`, the fragment is copied to its own offset `ring + index·F` and `have` counts it | `rn_channels_dispatch_chunk` (the copy to `record->ring * 16`, `anchor->block_have++`); a single record is `rn_channels_dispatch_ordered` |
| `Deliver(b)` | the receiver | the message at the read index is complete: one view of its span read at stride `F`, and the read index moves by one | `rn_channels_dispatch_chunk` (`have == count`, `delta == 0`), `rn_channels_deliver_ordered`, `rn_channels_drain` |
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
- `TypeOK`.

## What the module abstracts away

- **Acks and the window base.** Nothing is acked, so `U'` stays at the first number
  and the floor at the first span's start. The instance is one window wide (four
  numbers, `W = 4`) and one ring long (7 of 8 units), so no slot or ring unit is
  reused. Slot reuse under `U'` is `RNWindow`'s (`NoSlotCollision`) and ring reuse
  `RNRing`'s (`LiveSpansDisjoint`); a block mixed with another after numbers wrap
  is not reachable here.
- **The resend delay.** Any numbered record may be resent at any time, so every order of
  arrival the link and the sender can produce is explored.
- **Dispatch inside the receive.** The code delivers inside the dispatch call that
  completes the message and then drains; here `Deliver` is its own step, which
  only adds interleavings.
- **The receiver's agreement checks** (count, first number, base against the
  anchor; `MESSAGE_BAD_BLOCK`, `MESSAGE_BAD_RING`) never fire for a correct sender
  and are not modelled.
- **Liveness** is not checked: the sender resends without bound and no fairness
  is stated. `RNWindow` checks that every reliable number is delivered.

## The instance

`W = 4`, `F = 2` units, `B = 8` units, `Small = 2` units, two messages: a single
record of 2 units (number 0, offset 0) and a block of 5 units, three fragments of
2, 2 and 1 units (numbers 1..3, offsets 2, 4, 6, ordering index 1). The link is
`"arbitrary"` with duplication and reordering, capacity 2, no delay.

| Case | Result | States (distinct) |
|---|---|---|
| `MCRNBlocks.cfg` | every invariant holds | 11478 generated, 2421 distinct, depth 17 |
| `MCRNBlocksBrokenPlaceByArrival.cfg` | `DeliveredEqualsSent` violated, exit 12 | 11447 generated, 2997 distinct |

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

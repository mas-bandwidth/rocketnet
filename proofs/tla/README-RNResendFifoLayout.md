# RNResendFifoLayout

The resend FIFO of `sdk/rocketnet_window_ack.cpp` at its layout, with the
code's own fields. [RNResendTiming](README-RNResendTiming.md) and
`RNChannelQueues.tla` check one resend sequence per class as a sequence; this
model checks how the two sequences are laid into one array, which they cannot
see.

## State

| Model | Code |
|---|---|
| `pf[s]`, `[lk, ix]` | `region->resend_fifo[s].prev`: bit 31 `RN_RESEND_LINKED`, low 31 bits the previous slot |
| `nf[s]`, `[ck, ix]` | `region->resend_fifo[s].next`: bit 31 `RN_RESEND_CHUNK`, low 31 bits the next slot |
| index `W` | `RN_RESEND_END` (`0x7FFFFFFF`) in a field and `RN_RESEND_NONE` outside one (`rn_resend_index`, `rn_resend_field`, lines 48-62) |
| `hd["s"]`, `tl["s"]` | `ack->resend_head`, `ack->resend_tail` (small records) |
| `hd["c"]`, `tl["c"]` | `ack->chunk_head`, `ack->chunk_tail` (block chunks) |
| `cnt` | `ack->resend_count` |
| `isChunk[s]` | `rn_window_ack_slot_is_chunk( ack, s )`, read off the slot's descriptor now |
| `seq[c]` | ghost: the abstract sequence of class `c` |

A number's slot is `n & (W - 1)`. Both sequences share the `W` entries; a slot
is in at most one. The slot's class at link time is read from its descriptor,
but the descriptor can be freed, or another attached, while the slot is
linked, so `SetClass` flips `isChunk[s]` at any time. The class bit stored in
`next` at link time is what the unlink follows. The carve clears every link
(`Init`: every field 0).

Generations are not modelled. `rn_resend_fifo_unlink_named` (188) either
unlinks or changes nothing, so `Unlink` of any slot at any time covers it; the
generation rule is RNChannelQueues.tla's.

## Actions and the code they mirror

Line numbers are `sdk/rocketnet_window_ack.cpp` at this change.

- **`Unlink(s)`**: `rn_resend_fifo_unlink` (132): an unlinked slot is left
  alone; otherwise the class from bit 31 of `next`, the neighbours' fields or
  the class's head and tail, then `prev = 0`, `next = 0` (160), and the count.
  Called from `rn_window_ack_on_ack` (783, and through
  `rn_resend_fifo_unlink_named` at 785).
- **`PushTail(s)`**: `rn_resend_fifo_push_tail` (248) from
  `rn_window_ack_commit` (632): the tail of the slot's class, stepped back to
  the slot's prev when the slot is itself that tail (253), then
  `rn_resend_fifo_insert_after` (212): unlink first, the next of `after` (or the
  head), `next = chunk ? RN_RESEND_CHUNK : 0` (229), the links, the tail when
  the new entry is last (241), the count.
- **`PushHead(s)`**: `rn_resend_fifo_push_head` (258) from
  `rn_window_ack_packet_failed` (945): `insert_after( NONE, s )`.
- **`Park(after, s)`**: `rn_window_ack_park` (264) from `rn_ordered_write`
  (`sdk/rocketnet_channel.cpp`), with the caller's contract that its asserts
  state: `after` is `NONE` or linked in `s`'s class, and is not `s`.
- **`SetClass(s)`**: the environment, a descriptor freed or attached.

## Invariants

- **`ListsMatch`**: for each class, the head and tail are the first and last
  of its sequence (`NONE` when empty), and each member is linked, carries the
  class's bit, and points back and forward to its neighbours in the sequence
  (`NONE` at the ends); a slot in no sequence is not linked; the count is the
  two lengths together. This is the whole mapping: every walk the code makes
  (`rn_window_ack_resend_first`, `rn_window_ack_resend_next`, `rn_ordered_walk`)
  reads the abstract sequence.
- **`Disjoint`**: a slot is in at most one sequence, once.

## The instance

`MCRNResendFifoLayout.cfg`: `W = 4`, every slot's class arbitrary at the start
and free to change. Every action is enabled in every state, so the deadlock
check is on.

## Reversed witnesses

Each turns a rule off and must fail with `ListsMatch`. Every counterexample
was checked against the C by hand.

1. **`MCRNResendFifoLayoutBrokenNoSelfTailFix.cfg`** (`SelfTailFix = FALSE`):
   `rn_resend_fifo_push_tail` without `if ( tail == slot ) tail = rn_resend_index(
   ...prev )` (252-253). TLC: `PushTail(0)` links slot 0 as the only small
   record; a second `PushTail(0)` (a resend of the same number) calls
   `insert_after( 0, 0 )`. Against the C: the unlink empties the class
   (`resend_head = resend_tail = NONE`) and zeroes both fields; `next` is then
   read from `fifo[0].next`, which is now index 0, not the end marker; the
   entry is linked after itself and before itself, and the head and tail stay
   `NONE` with `resend_count = 1`. The number has left its sequence: no walk
   reaches it again, so it is never resent.
2. **`MCRNResendFifoLayoutBrokenNoTailOnEnd.cfg`** (`TailOnEnd = FALSE`):
   `rn_resend_fifo_insert_after` without `else *tail = slot;` (239-241). TLC:
   the first `PushTail(0)` into an empty class leaves `resend_tail = NONE`.
   Against the C: the next `push_tail` reads the tail as `NONE` and inserts at
   the head, so the sequence is no longer in send order and the walk's stop at
   the first number not yet due skips due numbers behind it.
3. **`MCRNResendFifoLayoutBrokenStaleClassBit.cfg`** (`UnlinkClearsClass = FALSE`,
   `InsertSetsClass = FALSE`): the unlink keeps bit 31 of `next` and the
   insert ORs the class in instead of assigning it. TLC: slot 3 is pushed as a
   chunk; its descriptor is freed (`SetClass`), so it is now a small record;
   `PushTail(3)` unlinks it from the chunk sequence and links it into the small
   sequence with the chunk bit still set. Against the C:
   `rn_window_ack_resend_next( ack, false, 3 )` returns `NONE` because the bit
   says chunk (111), so the small walk stops at slot 3, and its next unlink
   rewrites `chunk_head`/`chunk_tail` instead of the small ones.

Either half of the third witness alone passes (4,176 and 8,576 distinct
states, measured on hulk): the code clears the bit twice, once at the unlink
(160) and again by assigning the whole field at the insert (229), so each
clear masks the loss of the other. The witness removes both.

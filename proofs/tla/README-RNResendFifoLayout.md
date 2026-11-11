# RNResendFifoLayout

The resend FIFO of `sdk/rocketnet_window_ack.cpp` at its layout, with the
code's own fields. [RNResendTiming](README-RNResendTiming.md) and
[RNResendGenerations](README-RNResendGenerations.md) check one resend sequence per class as a sequence and the names an ack follows; this
model checks how the two sequences are laid into one array, which they cannot
see.

## State

| Model | Code |
|---|---|
| `pf[s]`, `[lk, ix]` | `region->resend_fifo[s].prev`: bit 31 `RN_RESEND_LINKED`, low 31 bits the previous slot |
| `nf[s]`, `[ck, ix]` | `region->resend_fifo[s].next`: bit 31 `RN_RESEND_CHUNK`, low 31 bits the next slot |
| index `W` | `RN_RESEND_END` (`0x7FFFFFFF`) in a field and `RN_RESEND_NONE` outside one (`rn_resend_index`, `rn_resend_field`) |
| `hd["s"]`, `tl["s"]` | `ack->resend_head`, `ack->resend_tail` (small records) |
| `hd["c"]`, `tl["c"]` | `ack->chunk_head`, `ack->chunk_tail` (block chunks) |
| `cnt` | `ack->resend_count` |
| `mk["s"]`, `mk["c"]` | `ack->resend_sent`, `ack->chunk_sent`: the first slot of the class on the wire, or `RN_RESEND_NONE` |
| `sent[s]` | ghost: the slot's number is on the wire: `last_send` is not `RN_SEND_NUMBER_NEVER_SENT` (a send time, or a refused resend's due-now sentinel) |
| `order` | ghost: the slots on the wire in order of last send, a refused resend's sentinel below every send time |
| `isChunk[s]` | `rn_window_ack_slot_is_chunk( ack, s )`, read off the slot's descriptor now |
| `seq[c]` | ghost: the abstract sequence of class `c` |

A number's slot is `n & (W - 1)`. Both sequences share the `W` entries; a slot
is in at most one. The slot's class at link time is read from its descriptor,
but the descriptor can be freed, or another attached, while the slot is
linked, so `SetClass` flips `isChunk[s]` at any time. The class bit stored in
`next` at link time is what the unlink follows. The carve clears every link
(`Init`: every field 0).

An entry is `prev` and `next` only (`rn_resend_link_t`, 8 bytes). Which number
an ack may unlink is [RNResendGenerations](README-RNResendGenerations.md)'s;
`Unlink` of any slot at any time covers it.

## Actions and the code they mirror

Functions in `sdk/rocketnet_window_ack.cpp`.

- **`Unlink(s)`**: `rn_resend_fifo_unlink`: an unlinked slot is left
  alone; otherwise the class from bit 31 of `next`, the neighbors' fields or
  the class's head and tail, the class's mark passed to the slot's next when
  it is the slot, then `prev = 0`, `next = 0`, and the count. Called from
  `rn_window_ack_on_ack`, and first in every insert.
- **`PushTail(s)`**: `rn_resend_fifo_push_tail` from
  `rn_window_ack_commit`: the tail of the slot's class, stepped back to
  the slot's prev when the slot is itself that tail, then
  `rn_resend_fifo_insert_after`: unlink first, the next of `after` (or the
  head), `next = chunk ? RN_RESEND_CHUNK : 0`, the links, the tail when
  the new entry is last, the count; then the class's mark set to the slot
  when the class had none. The number is on the wire.
- **`PushHead(s)`**: `rn_resend_fifo_push_head` from
  `rn_window_ack_packet_failed` for a refused first send:
  `insert_after( NONE, s )`. The number stays never sent (guard `~sent[s]`).
- **`PushMark(s)`**: `rn_resend_fifo_push_mark` from
  `rn_window_ack_packet_failed` for a refused resend: unlink, then
  `insert_after` the mark's prev (the class's tail when it has no mark), then
  the mark is the slot. The number stays on the wire, first in `order`: its
  due-now sentinel is below every send time, and of two refused in one packet
  the code's reverse loop leaves the first first.
- **`Park(after, s)`**: `rn_window_ack_park` from `rn_ordered_write`
  (`sdk/rocketnet_channel.cpp`), with the caller's contract: `after` is
  `NONE` or linked in `s`'s class, and is not `s` (both asserted there), and
  is not on the wire (the walk's last never-sent number; stated in the header,
  not asserted, as `test_resend_fifo_corrupt_link_is_bounded` parks outside
  it; `ParkContract = FALSE` drops it, witness 9). `s` is never sent. The
  mark is untouched.
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
- **`Partitioned`**: each sequence is its numbers not on the wire, then its
  numbers on the wire.
- **`MarkMatch`**: each class's mark is its first number on the wire (`NONE`
  when it has none), so `rn_ordered_walk` from the mark sees exactly the
  numbers on the wire, and from the head to the mark exactly the others.
- **`Ordered`**: each class's numbers on the wire are in order of last send
  (`order`), so the walk from the mark may stop at the first one not yet due.
  Checked in `MCRNResendFifoLayout.cfg`.

## The instance

`MCRNResendFifoLayout.cfg`: `W = 4`, every slot's class arbitrary at the start
and free to change. Every action is enabled in every state, so the deadlock
check is on.

## Reversed witnesses

Each turns a rule off and must fail, the first three with `ListsMatch` and
the last three with `MarkMatch`. Every counterexample was checked against
the C by hand.

1. **`MCRNResendFifoLayoutBrokenNoSelfTailFix.cfg`** (`SelfTailFix = FALSE`):
   `rn_resend_fifo_push_tail` without `if ( tail == slot ) tail = rn_resend_index(
   ...prev )`. TLC: `PushTail(0)` links slot 0 as the only small
   record; a second `PushTail(0)` (a resend of the same number) calls
   `insert_after( 0, 0 )`. Against the C: the unlink empties the class
   (`resend_head = resend_tail = NONE`) and zeroes both fields; `next` is then
   read from `fifo[0].next`, which is now index 0, not the end marker; the
   entry is linked after itself and before itself, and the head and tail stay
   `NONE` with `resend_count = 1`. The number has left its sequence: no walk
   reaches it again, so it is never resent.
2. **`MCRNResendFifoLayoutBrokenNoTailOnEnd.cfg`** (`TailOnEnd = FALSE`):
   `rn_resend_fifo_insert_after` without `else *tail = slot;`. TLC:
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
   says chunk, so the small walk stops at slot 3, and its next unlink
   rewrites `chunk_head`/`chunk_tail` instead of the small ones.

Either half of the third witness alone passes (4,176 and 8,576 distinct
states, measured on hulk): the code clears the bit twice, once at the unlink
(`rn_resend_fifo_unlink`) and again by assigning the whole field at the insert
(`rn_resend_fifo_insert_after`), so each
clear masks the loss of the other. The witness removes both.
4. **`MCRNResendFifoLayoutBrokenMarkOnUnlink.cfg`** (`MarkOnUnlink = FALSE`):
   `rn_resend_fifo_unlink` without `if ( *sent == slot ) *sent = next;`.
   TLC: `PushTail(0)` sends slot 0, the class's mark becomes 0; `PushHead(0)`
   (the packet carrying it failed) puts it back at the head, not on the wire,
   and the mark stays 0. Against the C: `rn_window_ack_packet_failed` calls
   `push_head`, whose insert unlinks slot 0 first; with the mark left on it,
   the walk takes a number not on the wire as a retransmit, and after an ack
   unlinks the mark's slot the walk starts at an unlinked entry whose zeroed
   `next` reads as slot 0.
5. **`MCRNResendFifoLayoutBrokenMarkOnPushTail.cfg`** (`MarkOnPushTail = FALSE`):
   `rn_resend_fifo_push_tail` without setting the mark of a class with none.
   TLC: `PushTail(0)` sends slot 0 into an empty class and the mark stays
   `NONE`. Against the C: `rn_window_ack_commit` links the number on the wire,
   and the walk from the mark sees nothing on the wire while the walk from the
   head runs to the end, so the number is walked as never sent, behind the
   small class's never-sent numbers, and a due retransmit waits again.
6. **`MCRNResendFifoLayoutBrokenMarkInit.cfg`** (`MarkInitNone = FALSE`):
   `rn_window_ack_init` without `resend_sent = chunk_sent = RN_RESEND_NONE`.
   TLC: the initial state, both marks 0 with both classes empty. Against the
   C: a zeroed `rn_window_ack_t` reads both marks as slot 0, so the first commit into a
   class finds a mark already set and leaves it on slot 0, a slot in no
   sequence.

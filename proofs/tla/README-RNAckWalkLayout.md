# RNAckWalkLayout: the acknowledged-run walk at the code's layout

`RNAckWalkLayout.tla` models the sender's walk of the sent ring's shadow
bitmaps in `sdk/rocketnet_ack.cpp`. Sequence `s` lives at slot `s % N`, word
`slot >> 6`, bit `slot & 63`; `sent_held` has a slot's bit while it holds its
in-window sequence, `sent_unacked` while that sequence is also
unacknowledged. `FragmentsAndAcks.tla` checks the reconcile that keeps them
so, slot by slot; this model carries the word arithmetic that reads them.
Every input is checked where it stands (`Next` is a stutter).

`Part = "run"` is `rocketnet_ack_apply_run` over any run `[low, high]`: the
part above the highest sent counted unknown, the clamp to the retained
window, the slot range split where it wraps, the held count by
`rn_bitmap_range_popcount` (masked end words), and the visits by
`rn_bitmap_walk_bits_desc`, newest first, each bit mapped back by
`seq_at_a + (slot - a)` (the wrapped low piece from `high - slot_high`). The
reference reads the run sequence by sequence.

`Part = "lowest"` is `rocketnet_ack_stats_lowest_acked` over `[from, to]`
inside one window: an empty word of held and not unacknowledged bits skips to
the next word boundary, else the cursor steps by ctz and the candidate is
checked against its record (a stale slot's bits may still be set). The
reference is the lowest present acknowledged sequence.

## The instances

Six slots in three words of two bits. `MCRNAckWalkLayout.cfg`: every
reconciled pair of bitmaps, highest sent to 13, every run up to two past it.
`MCRNAckWalkLayoutLowest.cfg`: every slot state (empty, acked, unacked,
stale), highest to 9, every in-window range.

## Invariants

| Invariant | Meaning |
|---|---|
| `VisitsExact` | the unknown count and the visit order are the run read sequence by sequence |
| `LowestExact` | the lowest acknowledged record found is the lowest there is |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNAckWalkLayoutBrokenWrapSeq.cfg` | the wrapped low-slot piece mapped from `low` | `VisitsExact` |
| `MCRNAckWalkLayoutBrokenHiMask.cfg` | the last word's mask `(1 << hi_bit) - 1` | `VisitsExact` |
| `MCRNAckWalkLayoutBrokenSkipWord.cfg` | an empty word skips a whole word | `LowestExact` |

Each counterexample was checked against the C by hand: wrap-seq, highest 6
and run 1-6 with slot 0 unacknowledged, visits 1 where the code visits 6;
hi-mask, run 1-1 with slot 1 held, counts it unknown; skip-word, range 1-2
with slot 2 acknowledged, steps from 1 to 3 and finds nothing where the code
steps to 2.

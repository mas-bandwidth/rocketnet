# RNOrderingWrap: the 24-bit ordering against the 32-bit read index

`RNOrderingWrap.tla` models one reliable ordered channel's receive side with
the code's two widths. The read index `ordered_read` is a 32-bit counter
(`rn_channel_receive_t`, `sdk/rocketnet_allocator.h`); a record's ordering is
the 24-bit wire value. Every comparison between them must be made modulo
2^24. 2^32 is a multiple of 2^24, so the counter's low 24 bits stay aligned
with the sender's wire ordering across the counter's own wrap; the model
keeps that relation with `CounterMod` a multiple of `WireMod`.

`RNChannel.tla` and `RNChannelTypes.tla` check ordering, holds and the
delivery table with orderings as plain integers. They cannot see a
comparison made at the wrong width.

## The instance

`MCRNOrderingWrap.tla`: `W = 2`, a wire ordering modulo 8, a read counter
modulo 32 (so it passes the wire wrap three times and wraps itself) and a
delivery table of two.

## Actions and the code they mirror

Functions in `sdk/rocketnet_channel.cpp`.

| Action | Code |
|---|---|
| `Receive(o)` | one record of `rn_ordered_receive`: `rn_receive_delivery_reservation` (one delivery, plus each complete held successor when the record is in turn by its in-turn compare), the deferral when the table lacks that room, then `rn_channels_dispatch_ordered`: `delta = rn_u24(ordering - ordered_read)`, in turn delivered through `rn_channels_reliable_delivery` (which exhausts the region on a full table) and `rn_channels_deliver_ordered`, then `rn_channels_drain` (key `rn_held_key(channel, ordered_read)`, a full table exhausts the region); ahead, held |
| `EndUpdate` | `rn_channels_end_update`: `deliveries = 0` |
| `Ended` | environment: an exhausted region takes nothing more (`rn_ordered_receive`); a terminal stutter |

Records are honest: layer 3 drops duplicates, and a sender has fewer than `W`
records numbered at once, so a record reaching the dispatch is fresh and in
`ordered_read .. ordered_read + W - 1`. Blocks (`used` 2 entries) and the
sequenced hold's extra reserved delivery are left to `RNChannel` and
`RNChannelTypes`; this model is the width of each comparison.

The other comparisons of the ordering with the read index were read against
this rule and all mask both sides: `rn_channels_deliver_sequenced`
(`sdk/rocketnet_channel.cpp`), the descriptor check of `rn_channels_drain`,
`rn_channels_dispatch_sequenced` (`sdk/rocketnet_channel_sequenced.cpp`),
and both compares of `rn_channels_drain_sequenced`.

## Invariants

| Invariant | Meaning |
|---|---|
| `TypeOK` | types |
| `NoExhaust` | a full delivery table defers the packet; the drain never exhausts the region |
| `CompareAgrees` | the reservation's in-turn test agrees with the comparison modulo the wire |
| `HeldAhead` | every held ordering is 1 .. `W - 1` ahead of the read index modulo the wire |

## Reversed witness

| Case | Rule off | Declared invariant |
|---|---|---|
| `MCRNOrderingWrapBrokenUnmasked.cfg` | `Masked = FALSE`: `rn_u24(record->ordering) != rx->ordered_read`, an in-turn compare of the 24-bit ordering against the unmasked 32-bit read index | `NoExhaust` |

### Hand check against the code

TLC's trace delivers the records at orderings 0 to 7 in order over five
updates, the last of them in the fifth, so `ordered_read` = 8, the wire
ordering has wrapped to 0, and the table has one delivery taken. A record at
wire ordering 1 (ordering 9) arrives ahead and is held. Then the record at
wire ordering 0 (ordering 8) arrives. The broken reservation compares 0 with 8, finds it not in turn and
reserves one delivery; one is free, so the record is accepted. The dispatch
computes `delta = rn_u24(0 - 8) = 0`, delivers it (the table is now full),
sets `ordered_read` = 9 and drains: the key `(channel, 9 mod 2^24)` finds the
held record, `rn_channels_delivery_room` finds the table full and exhausts the
region. With the masked compare the reservation counts two deliveries, finds
one free and defers the packet; the next update delivers both. This is the
sequence `test_reservation_counts_the_held_run_past_2_to_the_24` in
`tests/test_unit_channel_ordered.cpp` runs at the real widths.

## Records

See `RUNS.tsv` (group `rnorderingwrap`).

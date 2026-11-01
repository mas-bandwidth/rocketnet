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
modulo 32 (so it passes the wire wrap three times and wraps itself), a
delivery table of three and an unreliable allowance `Du = 1` (the table is
`Du + W` entries, as `delivery_entries` is).

## Actions and the code they mirror

Functions in `sdk/rocketnet_channel.cpp`.

| Action | Code |
|---|---|
| `Receive(o)` | one record of `rn_ordered_receive`: `rn_receive_delivery_reservation` (one delivery, plus each complete held successor when the record is in turn by its in-turn compare), the deferral when the table lacks that room, then `rn_channels_dispatch_ordered`: `delta = rn_u24(ordering - ordered_read)`, in turn delivered through `rn_channels_reliable_delivery` (which exhausts the region on a full table) and `rn_channels_deliver_ordered`, then `rn_channels_drain` (key `rn_held_key(channel, ordered_read)`, a full table exhausts the region); ahead, held |
| `ReceiveUnreliable` | a code-0 record through `rn_channels_dispatch`: delivered while the update's unreliable deliveries are below `Du` and the table has room |
| `HoldSequenced` | a future sequenced record taken into the channel's empty hold (`rn_channels_dispatch_sequenced`): code 1 (`"u"`) or code 4 (`"r"`, `RN_SEQUENCED_RELIABLE_HOLD` set) |
| `EndUpdate` | `rn_channels_end_update`: `deliveries = 0`, `unreliable_delivered = 0` |
| `Ended` | environment: an exhausted region takes nothing more (`rn_ordered_receive`); a terminal stutter |

Records are honest: layer 3 drops duplicates, and a sender has fewer than `W`
records numbered at once, so a record reaching the dispatch is fresh and in
`ordered_read .. ordered_read + W - 1`. Blocks (`used` 2 entries) are left to
`RNChannel` and `RNChannelTypes`.

The channel's one sequenced hold is modelled as the delivery the drain may
release with the in-turn record's run. `Receive` chooses whether the drain
reaches the hold's epoch. A code-4 hold takes a slot, or exhausts the region on
a full table; a code-1 hold takes one only while `uDelivered < Du`, and is
otherwise refused and counted (`rn_sequenced_room`). The reservation's
sequenced term (`SeqTerm`) counts the hold exactly when its release can take a
slot; `Actual(o)` is the deliveries the dispatch can make, and a deferral with
`Actual(o)` within the free room sets the ghost `wasted`.

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
| `NeverDeferWithRoom` | a packet is deferred only when the deliveries its record can make do not fit |

## Reversed witness

| Case | Rule off | Declared invariant |
|---|---|---|
| `MCRNOrderingWrapBrokenUnmasked.cfg` | `Masked = FALSE`: `rn_u24(record->ordering) != rx->ordered_read`, an in-turn compare of the 24-bit ordering against the unmasked 32-bit read index | `NoExhaust` |
| `MCRNOrderingWrapBrokenSeqHold.cfg` | `ExactSeq = FALSE`: `if ( rx->held_sequenced != RN_DESCRIPTOR_NONE ) needed++;`, a code-1 hold counted whatever `Du` has left | `NeverDeferWithRoom` |

### Hand check against the code

`MCRNOrderingWrapBrokenUnmasked.cfg`: TLC's trace delivers the records at
orderings 0 to 5 in order over two updates, takes a code-4 hold, and in the
third update delivers orderings 6 and 7, so `ordered_read` = 8, the wire
ordering has wrapped to 0, and the table has one slot free. The
record at wire ordering 0 (ordering 8) arrives. The broken reservation
compares 0 with 8, finds it not in turn and reserves one delivery; one is
free, so the record is accepted. The dispatch computes
`delta = rn_u24(0 - 8) = 0` and delivers it (the table is now full); the
drain reaches the code-4 hold, and `rn_sequenced_room` finds the table full
and exhausts the region. With the masked compare the reservation counts the
record and the hold, finds one free and defers the packet. The run-release
form of the same defect (a held ordered successor in place of the hold) is
`test_reservation_counts_the_held_run_past_2_to_the_24` in
`tests/test_unit_channel_ordered.cpp`, at the real widths.

`MCRNOrderingWrapBrokenSeqHold.cfg`: the record at ordering 0 is delivered,
then a code-0 record spends `Du`, so the table holds two of three. A code-1
state is held. The record at ordering 1 arrives in turn. The broken
reservation counts it and the hold, two, against one free, and defers the
packet; but the hold's release would be refused by `rn_sequenced_room`
(`unreliable_delivered >= delivery_entries - window`), so the dispatch makes
one delivery, which fits. Checked against the code by hand:
`rn_receive_delivery_reservation` added one for any `held_sequenced`, and
`rn_ordered_receive` returned `ROCKETNET_CHANNEL_RECEIVE_DEFERRED`. The
reservation now counts a code-1 hold only while `unreliable_delivered` is
below `Du`. `test_reservation_skips_a_code_1_hold_past_du` in
`tests/test_unit_channel_reliable_sequenced.cpp` runs it on the code.

## Records

See `RUNS.tsv` (group `rnorderingwrap`).

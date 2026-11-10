# RNSequencingWrap: the 24-bit sequencing compares at the half range

`RNSequencingWrap.tla` models one sequenced channel's receive side at the
code's widths. Ordering and sequencing indices travel as 24-bit values and
every compare in `sdk/rocketnet_channel_sequenced.cpp` is a masked difference
tested against `ROCKETNET_MESSAGE_HALF_RANGE`. The read index `ordered_read`
is a 32-bit counter; `sequenced_watermark` is 24-bit. Each record in the model
carries its true ordering and sequencing (naturals), the code sees only their
low bits, and ghosts compare every verdict of the code with the true order.

`RNChannelTypes.tla` and `RNChannel.tla` check the same rules with ordering
and sequencing as plain integers, and `RNOrderingWrap.tla` checks the ordered
path's compares in `sdk/rocketnet_channel.cpp`. None of them can see a
sequenced compare made at the wrong width or with the wrong boundary.

## The compares

| Site | Code | Model |
|---|---|---|
| `rn_sequenced_in_turn`, `sdk/rocketnet_channel_sequenced.cpp:337`, `:340` | `held_gap = ( sequencing - held->sequencing ) & mask`; superseded at 0 or at or past the half range | `InTurn`, `hg` |
| `rn_sequenced_in_turn`, `sdk/rocketnet_channel_sequenced.cpp:356`, `:357` | `sequence_gap = ( sequencing - rx->sequenced_watermark ) & mask`; stale at or past the half range | `InTurn`, `wg` |
| `rn_channels_dispatch_sequenced`, `sdk/rocketnet_channel_sequenced.cpp:418`, `:420` | `delta = ( ordering - rx->ordered_read ) & mask`; stale at or past the half range, in turn at 0 | `Arrive`, `delta` |
| `rn_channels_dispatch_sequenced`, `sdk/rocketnet_channel_sequenced.cpp:463`, `:464`, `:467` | the future hold: `gap` and `sequence_gap`, the newer by ordering then sequencing kept | `Future`, `g`, `sg` |
| `rn_channels_drain_sequenced`, `sdk/rocketnet_channel_sequenced.cpp:576`, `:579` | the hold's `gap = ( ordering - rx->ordered_read ) & mask` after an ordered delivery | `OpenEpoch`, `g` |
| `rn_channels_deliver_sequenced`, `sdk/rocketnet_channel.cpp:432`, `:438` | the delivery's own check, then `sequenced_watermark = ( sequencing + 1 ) & mask` | `DeliverOK`, `wm'` |

An unmasked difference is the uint32 subtraction: modulo `CounterMod` in the
model (2^32 in the code), a multiple of `Mod`.

## The instance

`MCRNSequencingWrap.tla`: the wire modulus scaled from 2^24 to `Mod = 8`
(half range 4), uint32 arithmetic to `CounterMod = 32`, two records in flight,
five sequenced and four ordered sends per behaviour, from a quiescent channel
at read index and sequencing index 0 or 6 (`Starts = {0, 6}`). From 6 both
the ordering and the sequencing pass the wire wrap within the sends; from 0
the half-range boundary is reached without a wrap. A start anywhere on the
ring is a state the code reaches: every earlier record delivered.

## Actions and the code they mirror

| Action | Code |
|---|---|
| `Arrive(r)` | one record through `rn_channels_dispatch_sequenced`: `delta`; stale; in turn through `rn_sequenced_in_turn` (`held_gap` against `rx->block_in_turn`, `sequence_gap` against the watermark, `rn_sequenced_block` for a fragment, `rn_channels_deliver_sequenced`); ahead, the future hold (`gap`, `sequence_gap`, continuation, supersede) |
| `OpenEpoch` | an ordered delivery (`rn_channels_deliver_ordered`: read + 1, watermark 0), then `rn_channels_drain_sequenced`: the closed epoch's in-turn block released stale, the hold's `gap`, an incomplete block moved to `block_in_turn`, a hold in turn delivered |
| `SendOne`, `SendBlock`, `SendOrdered` | environment: the sender (`rn_channels_send_sequenced`, `rn_channels_send_ordered`, standard m6.1 rule 10); a block is two fragments of one ordering and sequencing |
| `Lose(r)` | environment: a record lost |
| `Done` | environment: nothing in flight, every ordered record delivered, the sender spent or paced out; a terminal stutter |

The delivery table always has room here; `rn_sequenced_room` and the
reservation are `RNOrderingWrap`'s. The hold's slots and their ownership are
`RNSequencedHoldPool`'s and `RNHeldOwnership`'s.

## The honest network

A modular compare at the half range is exact only while its two operands are
within half the modulus of each other. The model states the protocol's
assumption as the sender and network behave, never as the receiver's verdict:

- an ordered send only while `ordered_write + 1 - ordered_read <= Half - 1`;
- a sequenced send only while its index is within `Half - 1` of its epoch's
  watermark (0 for an epoch the receiver has not opened, standard m6.1 rule 8);
- a record in flight is never `Half` or more behind the sender's next index:
  a send drops from the network what falls out (`Fresh`, `SendOrdered`).

Under it the dispatch's `delta` and the watermark's `sequence_gap` range over
the whole of `-Half .. Half - 1`, so a record exactly `Half` behind is reached
(the boundary witnesses below), and the control proves the code's verdicts
equal the true order there.

## Invariants

| Invariant | Meaning |
|---|---|
| `TypeOK` | types |
| `NewerNeverDiscarded` | no stale or superseded verdict discards a state truly newer than the one kept |
| `SequencedInOrder` | every sequenced delivery is in its epoch and at or after the true watermark |
| `NothingSilent` | every record reaching the dispatch is delivered, held or counted (standard m7.4) |
| `HoldAhead` | the hold is truly ahead of the read index |
| `InTurnBlockFresh` | the in-turn block is never behind the watermark |
| `WatermarkAligned` | the 24-bit watermark is the true watermark's low bits |

## Reversed witnesses

Each writes one compare of the code wrong, as a one-line defect would.

| Case | Defect | Declared invariant |
|---|---|---|
| `MCRNSequencingWrapBrokenDeltaUnmasked.cfg` | `:418` `delta = ordering - rx->ordered_read`, the mask left out | `NewerNeverDiscarded` |
| `MCRNSequencingWrapBrokenDeltaBoundary.cfg` | `:420` `delta > ROCKETNET_MESSAGE_HALF_RANGE` | `HoldAhead` |
| `MCRNSequencingWrapBrokenBlockGapUnmasked.cfg` | `:337` `held_gap = sequencing - held->sequencing`, the mask left out | `NewerNeverDiscarded` |
| `MCRNSequencingWrapBrokenWatermarkUnmasked.cfg` | `:356` `sequence_gap = sequencing - rx->sequenced_watermark`, the mask left out | `NewerNeverDiscarded` |
| `MCRNSequencingWrapBrokenWatermarkBoundary.cfg` | `:357` `sequence_gap > ROCKETNET_MESSAGE_HALF_RANGE` | `NothingSilent` |
| `MCRNSequencingWrapBrokenHoldGapUnmasked.cfg` | `:463` `gap = ordering - held_ordering`, the mask left out | `NewerNeverDiscarded` |
| `MCRNSequencingWrapBrokenDrainGapUnmasked.cfg` | `:576` `gap = ordering - rx->ordered_read`, the mask left out | `NewerNeverDiscarded` |

A signed compare of the unmasked difference (`(int32_t)( a - b ) < 0`) reads a
wrapped newer value as older exactly as the unsigned unmasked one does, so the
unmasked witnesses stand for both.

Three compares have no witness, because no defect at them can fail under the
honest network:

- `:340` `held_gap > HALF_RANGE`: the in-turn block passed the watermark and
  no newer record has been delivered since, so a record in turn is within
  `Half - 1` of it either way; `held_gap` never equals the half range.
- `:464` `sequence_gap` unmasked, or `:467` `sequence_gap > HALF_RANGE`: two
  records of one future epoch both lie in `0 .. Half - 1` (its watermark is 0
  until it opens), so their difference never wraps and never reaches the half
  range. A sender more than 2^23 sequenced sends into an epoch the receiver
  has not opened breaks the assumption; the drain would then refuse its hold
  against watermark 0 whatever this compare says.

### Hand check against the code

`MCRNSequencingWrapBrokenDeltaUnmasked.cfg`: from read 6 the sender makes two
ordered sends and a sequenced send at ordering 8 (wire 0). It arrives while the
read index is 6: `0 - 6` in uint32 is `0xFFFFFFFA`, at or past the half range,
so `rn_channels_dispatch_sequenced` counts it stale; masked, `delta` is 2 and
it is held.

`MCRNSequencingWrapBrokenDeltaBoundary.cfg`: a record at ordering 0 stays in
flight while four ordered records are delivered (read 4, ordered_write 4).
`delta = (0 - 4) & mask` is exactly the half range; `>` lets it through, it
is not 0, so the future path holds a record of a closed epoch.

`MCRNSequencingWrapBrokenBlockGapUnmasked.cfg`: from sequencing 6, record 6
is delivered (watermark 7), a block at 7 starts collecting in
`rx->block_in_turn`, and record 8 (wire 0) arrives. `0 - 7` unmasked is past
the half range, so `rn_sequenced_in_turn` counts the newer record superseded
and keeps the older block; masked, `held_gap` is 1 and the block is released.

`MCRNSequencingWrapBrokenWatermarkUnmasked.cfg`: from sequencing 6, record 6
is delivered (watermark 7), record 7 is still in flight and record 8 (wire 0)
arrives: `0 - 7` unmasked is past the half range and the newer record is
counted stale; masked, `sequence_gap` is 1 and it is delivered.

`MCRNSequencingWrapBrokenWatermarkBoundary.cfg`: from sequencing 6 records 7, 8
and 9 are delivered (watermark 10, wire 2) while record 6 is still in flight,
exactly the half range behind. `(6 - 2) & mask` is the half range; `>` lets it
through, and `rn_channels_deliver_sequenced` (`sdk/rocketnet_channel.cpp:434`)
refuses it, so `rn_sequenced_in_turn` returns `ROCKETNET_CHANNEL_DISPATCH_NONE`
with nothing counted (a code-4 record's number is not released either).

`MCRNSequencingWrapBrokenHoldGapUnmasked.cfg`: from read 6 a record at ordering
7 is held, the sender makes another ordered send, and a record at ordering 8
(wire 0) arrives: `0 - 7` unmasked is past the half range, so the newer future
record is counted superseded and the older hold kept.

`MCRNSequencingWrapBrokenDrainGapUnmasked.cfg`: from read 6 a record at
ordering 8 (wire 0) is held; the ordered record at 6 is delivered (read 7) and
`rn_channels_drain_sequenced` computes `0 - 7` unmasked, past the half range,
and releases the hold stale although it is one epoch ahead.

## Records

See `RUNS.tsv` (group `rnsequencingwrap`).

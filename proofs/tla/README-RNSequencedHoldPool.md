# RNSequencedHoldPool: the code-1 hold pool at the code's layout

`RNSequencedHoldPool.tla` models the unreliable sequenced (code 1) hold pool
of one connection region as `sdk/rocketnet_allocator.cpp` lays it out and
`sdk/rocketnet_channel_sequenced.cpp` uses it: `holds` slots, each with a
`used` state (0 free, 1 held, 2 delivered) and a payload, the free stack
(`sequenced_hold_free_stack`, top at `sequenced_hold_free_count - 1`), the
delivered list (`sequenced_holds_delivered`, `sequenced_holds_delivered_count`)
and each channel's `held_sequenced`.

The two index arrays are one allocation: `rn_region_layout` gives each hold
`2 * sizeof(uint32_t)` of indices and `rn_region_carve` sets
`sequenced_holds_delivered = sequenced_hold_free_stack + holds`
(`sdk/rocketnet_allocator.cpp:484-485`). The model keeps one array of
`2 * Holds` cells, so a push past the stack's `Holds` entries lands on the
delivered list as it would in memory.

`RNChannel`'s `BuffersBounded` and `RNChannelTypes`'s `HoldCapacity` bound
the number of holds; they cannot see a slot returned twice or a delivered
payload rewritten under its view.

## The instance

`MCRNSequencedHoldPool.tla`: two holds and two channels, so the free stack
empties, both holds can be delivered in one update, and a push past the
stack reaches the delivered list.

## Actions and the code they mirror

Line numbers at this commit.

| Action | Code |
|---|---|
| `Hold(c)` | `rn_channels_dispatch_sequenced` (`sdk/rocketnet_channel_sequenced.cpp:442-521`), a code-1 record ahead of its epoch and newer than the channel's hold: the old hold returned through `rn_sequenced_release_hold` and `rn_sequenced_release_slot` (97-138), then `HOLD_FULL` when the stack is empty or the top taken (502-508), `used = 1` and the payload copied (517-519) |
| `DrainDeliver(c)` | `rn_channels_drain_sequenced` (528-631), the hold in turn and delivered: `held_sequenced = NONE` (595), `used = 2` and the slot appended to the delivered list (626-627); the delivery's view points at the slot's payload |
| `DrainRelease(c)` | `rn_channels_drain_sequenced`: a stale hold (571), no delivery room (597) or a refused delivery (604), each returned to the free stack |
| `EndUpdate` | `rn_channels_end_update_sequenced` (634-652): each delivered slot returned in list order, the list emptied; the update's views expire |
| `Carve` | `rn_region_carve` (`sdk/rocketnet_allocator.cpp:583-599`): the stack full, lowest index on top, nothing used; `rn_channels_bind` leaves every `held_sequenced` NONE |

A return is `rn_sequenced_hold_return` (46-50): `used = 0` and
`stack[free_count++] = slot`. Orderings and sequencing are left to
`RNChannelTypes`: whether a newer, a stale or an in-turn record arrives is a
free choice here, and only the newer one ahead touches the pool. An in-turn
code-1 record is delivered from the batch and never holds.

## Invariants

| Invariant | Meaning |
|---|---|
| `TypeOK` | types |
| `StackInBounds` | no return pushes at `free_count >= holds`, onto the delivered list |
| `FreeStackExact` | `StackInBounds`, no return of a slot that is free or still a channel's hold, and the stack holds distinct free slots |
| `PoolPartition` | every slot is in exactly one place (the stack, one channel's hold, the delivered list), each in its `used` state, and the delivered list is the live views |
| `NoPayloadOverwrite` | no payload is written while its delivered view is live |

## Reversed witnesses

| Case | Rule off | Declared invariant |
|---|---|---|
| `MCRNSequencedHoldPoolBrokenDoubleReturn.cfg` | `DeliverClearsHold = FALSE`: line 595 removed, the delivery leaves `held_sequenced` naming the delivered slot | `StackInBounds` |
| `MCRNSequencedHoldPoolBrokenEarlyReturn.cfg` | `DeferReturn = FALSE`: the delivered slot returned at once instead of at the end of the update | `NoPayloadOverwrite` |

### Hand checks against the code

**DoubleReturn.** TLC's trace: channel 0 takes slot 0 (the stack holds slot
1). The drain delivers it: `used = 2`, delivered list `[0]`, but
`held_sequenced` still names slot 0. A second drain pass finds the same hold
and appends it again (delivered `[0, 0]`); in the C that second pass is
refused by the watermark (`rn_channels_deliver_sequenced`, the sequence gap)
and takes the refusal path at lines 604-612, which returns slot 0 to the
stack: the same double return by another road, and the model's `DrainRelease`
covers it. At the end of the update the loop at lines 643-648 returns slot 0
for each list entry: the stack count goes 1, 2, 3, and the third push writes
`sequenced_hold_free_stack[2]`, which is `sequenced_holds_delivered[0]`, the
entry the same loop is reading. Slot 0 is then free on the stack twice and
still channel 0's hold: two later holds take one payload.

**EarlyReturn.** TLC's trace: channel 0 takes slot 0, the drain delivers it
and returns it at once. The next newer record on channel 0 takes the top of
the stack, slot 0, and copies its payload into
`sequenced_message_payloads[0]` (line 519) while the application's view of
the delivered message still points there: the delivered message changes
under the reader. The code's `used = 2` and the delivered list keep the slot
out of the stack until `rn_channels_end_update_sequenced`, after the views
expire.

## Records

See `RUNS.tsv` (group `rnsequencedholdpool`).

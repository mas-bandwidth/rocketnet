# RNAckBuildLayout: the ack block builder at the code's layout

`RNAckBuildLayout.tla` models `rocketnet_ack_block_build`
(`sdk/rocketnet_ack.cpp`): one descending pass over the ack-eligible replay
window's words, from the largest received sequence to the floor
`max(1, largest - W + 1)`. Each word is loaded once: `bit_index = cursor % W`,
`offset = bit_index % 64`, `chunk = min(offset + 1, remaining)`, the mask of
bits `low_bit .. offset` with `low_bit = offset - chunk + 1`, and the shift by
`63 - offset` that puts the cursor's bit on top. Three phases count leading
ones (the first run, then a run) or zeros (a gap) with clz, carrying counts
across words; a run that ends emits `(gap, run)`, the range count stops at
`max_ranges`, and a run that reaches the floor is emitted after the loop.

The inputs are any window: any largest, any word values with the largest's
bit set (an advance always sets it), any `max_ranges`. Bits for sequences
outside `floor .. largest` are arbitrary, so a read past the floor shows. The
reference is the bit-by-bit walk the word pass replaced. `ReplayWindow.tla`
and `SessionAcks.tla` take the block as that set.

## The instance

`MCRNAckBuildLayout.tla`: three words of three bits (a window of 9), every
largest to 19, every window value, `max_ranges` 0 to 2.

## Actions and the code they mirror

| Action | Code |
|---|---|
| `Step` | one iteration of the `while ( remaining > 0 && !done )` loop: the word load when `word_bits` is 0, then one phase step |
| `Finish` | after the loop: the run that reached the floor, emitted while `num_ranges < max_ranges` |

## Invariants

| Invariant | Meaning |
|---|---|
| `RangesFit` | never more ranges than `max_ranges` |
| `BlockIsReference` | the finished block is the bit-by-bit walk's, truncated to `max_ranges` |
| `FirstRunNonEmpty` | the first run is at least one (the code's assert) |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNAckBuildLayoutBrokenNoClamp.cfg` | the chunk is not clamped to `remaining` | `BlockIsReference` |
| `MCRNAckBuildLayoutBrokenCap.cfg` | the range count stops at `num_ranges > max_ranges` | `RangesFit` |
| `MCRNAckBuildLayoutBrokenNoFlush.cfg` | the run at the floor is not emitted | `BlockIsReference` |

Each counterexample was checked against the C by hand: no-clamp reads bits 0-1
for largest 1 and counts a first run of 2; cap, at largest 8 with 8, 6 and 4
received and `max_ranges` 1, emits a second range; no-flush, at largest 3 with
3 and 1 received, drops the run at the floor that the code emits.

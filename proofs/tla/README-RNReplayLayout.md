# RNReplayLayout: the replay window at the code's layout

`RNReplayLayout.tla` models the replay window of one encryption mapping as
`sdk/rocketnet_replay.cpp` lays it out: `highest_opened` and the ring of
`ROCKETNET_REPLAY_WINDOW_BITS` bits stored as 64-bit words. It carries the
code's arithmetic: a sequence at bit `s % W`, word `bit / 64`, position
`bit % 64`; on an advance past the high-water mark, a skip of `W` or more
clears every word, and a shorter skip clears the ring positions of
`highest + 1 .. s - 1` as `[begin, end)` with `begin = (highest + 1) % W` and
`end = begin + skipped - 1`, split into `[begin, W)` and `[0, end - W)` when it
crosses the ring end. `rocketnet_replay_clear_range` clears a range as a
partial first word (mask `((1 << count) - 1) << offset`), whole words, and a
partial last word (mask `(1 << (end - begin)) - 1`).

`ReplayWindow.tla` checks the protocol around this with the skipped bits
cleared one by one. The CBMC harness `replay-near-4096` checks the real code
at full width but is HOLD past its fifth partition. This model carries the
word arithmetic at a small width, every path.

## The instance

`MCRNReplayLayout.tla`: three words of four bits (a ring of 12), sequences
to 30, so the ring turns over twice and a skipped range can be a partial
first word, a whole word and a partial last word, or wrap at the ring end.

## Actions and the code they mirror

| Action | Code (`sdk/rocketnet_replay.cpp`) |
|---|---|
| `Advance(s)` | `rocketnet_replay_advance` after `rocketnet_replay_already_received` refused nothing: past the high-water mark the clear (`rocketnet_replay_clear_range`) and the new mark, then `rocketnet_replay_set_bit`; at or below it only the bit |
| `Done` | a terminal stutter once every sequence is refused |

## Invariants

| Invariant | Meaning |
|---|---|
| `WindowIsTruth` | each ring bit is set exactly when a received sequence of `(highest - W, highest]` lives there |
| `RefusesExactly` | `already_received` refuses exactly the received sequences and those `W` or more below the mark |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNReplayLayoutBrokenNoWrap.cfg` | a skip across the ring end clears only `[begin, W)` | `WindowIsTruth` |
| `MCRNReplayLayoutBrokenBeginEarly.cfg` | `begin = highest % W`: the bit of `highest` is cleared, the bit of `s - 1` kept | `WindowIsTruth` |
| `MCRNReplayLayoutBrokenNoShift.cfg` | the partial first word's mask is not shifted by its offset | `WindowIsTruth` |

Each counterexample was checked against the C by hand: the begin-early trace
opens 1 then 3 and clears bit 1; the no-shift trace opens 1 then 4, and the
unshifted mask clears bits 0-1 in place of 2-3; the no-wrap trace opens 1, 3,
then 14, and bit 1 keeps sequence 1 where the code's `[0, end - W)` clears it.

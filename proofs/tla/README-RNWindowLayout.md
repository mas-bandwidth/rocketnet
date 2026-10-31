# RNWindowLayout: the receive window at the code's layout

`RNWindowLayout.tla` models the receive window of one connection as
`sdk/rocketnet_window_receive.cpp` lays it out: the received bitmap as words
of 64 bits (`region->received_bits`), the descriptor slot array
(`region->receive_descriptors`), `R`, and the sticky `violation`. It carries
the code's arithmetic: the 24-bit half-range reconstruction of the wire
number against `R`, `index = number & (W - 1)`, word `index >> 6` and bit
`index & 63`, the duplicate test on the bit, the `SLOT_COLLISION` test on a
live descriptor, the claim, and the advance loop that clears each floor bit
as `R` passes it.

`RNWindow.tla` checks the protocol around this (the sender's window, P6,
U') with the received bits as a set and the descriptors indexed abstractly.
It cannot see a bit left set after `R` passes it or a sign lost in the
reconstruction: those live in the mapping to memory, which is what this
model carries.

## The instance

`MCRNWindowLayout.tla`: `W = 4` (one bit word), `WireMod = 16` (the model's
2^24; half range 8), `R` bounded at 20 by the state constraint `Bound`, so
`R` crosses the wire wrap and the slot array turns over five times.

## Actions and the code they mirror

Functions in `sdk/rocketnet_window_receive.cpp` unless stated.

| Action | Code |
|---|---|
| `Accept(w, n)` | `rn_window_receive_accept`: the sticky violation, the reconstruction, behind is a duplicate, at or past `W` is `NUMBER_OUTSIDE_WINDOW`, index, word and bit, a set bit is a duplicate, a live descriptor of another number is `SLOT_COLLISION`, the claim, the advance clearing each floor bit |
| `Release(s)` | `descriptor->live = 0`: `rn_window_receive_release`, and the library's releases `rn_channels_release` (`sdk/rocketnet_channel.cpp`), `rn_sequenced_release_slot` and `rn_sequenced_release_number` (`sdk/rocketnet_channel_sequenced.cpp`) |

`Accept` takes two kinds of arrival. An honest one carries the number `n` the
sender meant: `n` lies in `R - 8 .. R + W - 1` (a duplicate as old as the half
range, or a number in the window) and never names a slot whose descriptor is
live with another number. That last condition is `RNWindow`'s
`NoSlotCollision`, the protocol's guarantee, taken here as an assumption. A
forged arrival is any wire value at all.

The `UINT64_MAX` guard in the window test of `rn_window_receive_accept` is
unreachable for a 64-bit `R` and is not modelled. The advance is a bounded
recursion (`W + 1` steps); with the floor bits cleared at most `W` bits are
ever set, so the bound never cuts the code's loop short.

## Invariants

| Invariant | Meaning |
|---|---|
| `TypeOK` | types |
| `AcceptedOnce` | no number is accepted twice |
| `NeverPastWindow` | no number at or past `R + W` is accepted |
| `NoSilentLoss` | `R` never passes a number that was not accepted, and an honest fresh number is never refused as a duplicate |
| `HonestReadExactly` | an honest number is reconstructed as itself and never refused as a violation |
| `BitsAreReceived` | the bit array is exactly the received numbers `R` has not passed, each at its slot's word and bit, and no bit outside the window's slots is set |
| `NoLiveOverwrite` | a claim never writes over a live descriptor of another number |
| `LiveNamesWindow` | a live descriptor names a received number below `R + W` at its own slot |

## Reversed witnesses

Each witness turns off one rule of the code and must fail with its declared
invariant (exit 12).

| Case | Rule off | Bug class | Declared invariant |
|---|---|---|---|
| `MCRNWindowLayoutBrokenNoClearFloor.cfg` | `ClearFloor = FALSE`: the advance does not clear the floor bit | a stale received bit after `R` passes: silent loss | `NoSilentLoss` |
| `MCRNWindowLayoutBrokenNoHalfRange.cfg` | `HalfRange = FALSE`: the unsigned difference, no sign | a duplicate behind `R` read as far ahead | `HonestReadExactly` |
| `MCRNWindowLayoutBrokenNoCollisionTest.cfg` | `CollisionTest = FALSE`: the `SLOT_COLLISION` test removed | a live descriptor overwritten | `NoLiveOverwrite` |

### Hand checks against the code

**NoClearFloor.** TLC's trace: number 0 arrives, sets bit 0 and `R` advances
to 1, but bit 0 stays set. Number 0's delivery ends and its descriptor is
released. The sender's number 4 (`R + W - 1`, slot 0) arrives: `delta = 3`,
index 0, and the received-bit test finds bit 0 still set, so
`rn_window_receive_accept` refuses it as a duplicate. Number 4 was never
received and is never handed up, while the transport acks the packet that
carried it. With the floor bit's clear (`*floor_word &= ~floor_bit`) the
advance clears bit 0 when `R` passes 0. In the C without that clear the stale
bits also make the advance loop run `R` past numbers never received, and once
every bit is set it never ends; the model's advance is bounded at `W + 1`
steps and reports that as the same loss.

**NoHalfRange.** TLC's trace: number 0 arrives (`R` = 1), then the sender's
resend of 0 arrives. With the signed difference, the reconstruction gives
`delta = -1` and the `delta < 0` test drops it as a duplicate. Without the
sign `delta = 15`, which is at least `W`, so the window test sets
`NUMBER_OUTSIDE_WINDOW` and the connection ends on an ordinary resend.

**NoCollisionTest.** TLC's trace: number 0 is accepted and its descriptor is
live (`R` = 1, slot 0 live with number 0, its delivery not yet released). A
forged record with wire number 4 arrives: `delta = 3 < W`, so the number is 4
at index 0; the bit for index 0 was cleared when `R` passed 0, so the
received-bit test lets it through, and without the `SLOT_COLLISION` test the
claim overwrites number 0's descriptor while number 0's delivery still points
at it. With the test the forged record ends the connection as
`SLOT_COLLISION`.

## Records

See `RUNS.tsv` (group `rnwindowlayout`).

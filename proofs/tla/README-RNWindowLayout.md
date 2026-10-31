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

Line numbers are at this commit.

| Action | Code |
|---|---|
| `Accept(w, n)` | `rn_window_receive_accept`, `sdk/rocketnet_window_receive.cpp:37-112`: the sticky violation (41-42), the reconstruction (47-50), behind is a duplicate (55-56), at or past `W` is `NUMBER_OUTSIDE_WINDOW` (58-62), index, word and bit (64-67), a set bit is a duplicate (68-69), a live descriptor of another number is `SLOT_COLLISION` (71-76), the claim (82-93), the advance clearing each floor bit (99-109) |
| `Release(s)` | `descriptor->live = 0`: `rn_window_receive_release` (`sdk/rocketnet_window_receive.cpp:118-127`), and the library's releases `rn_channels_release` (`sdk/rocketnet_channel.cpp`), `rn_sequenced_release_slot` and `rn_sequenced_release_number` (`sdk/rocketnet_channel_sequenced.cpp`) |

`Accept` takes two kinds of arrival. An honest one carries the number `n` the
sender meant: `n` lies in `R - 8 .. R + W - 1` (a duplicate as old as the half
range, or a number in the window) and never names a slot whose descriptor is
live with another number. That last condition is `RNWindow`'s
`NoSlotCollision`, the protocol's guarantee, taken here as an assumption. A
forged arrival is any wire value at all.

The `UINT64_MAX` guard on line 58 is unreachable for a 64-bit `R` and is not
modelled. The advance is a bounded recursion (`W + 1` steps); with the floor
bits cleared at most `W` bits are ever set, so the bound never cuts the
code's loop short.

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
| `MCRNWindowLayoutBrokenNoCollisionTest.cfg` | `CollisionTest = FALSE`: lines 71-76 removed | a live descriptor overwritten | `NoLiveOverwrite` |

### Hand checks against the code

**NoClearFloor.** TLC's trace: number 0 arrives, sets bit 0 and `R`
advances to 1, but bit 0 stays set. Number 0's delivery ends and its
descriptor is released. The sender's number 4 (`R + W - 1`, slot 0) arrives:
`delta = 3`, index 0, and line 68 finds bit 0 still set, so line 69 refuses
it as a duplicate. Number 4 was never received and is never handed up, while
the transport acks the packet that carried it. With line 107 in place the
advance cleared bit 0 when `R` passed 0. In the C with line 107 removed the
stale bits also make the loop at lines 99-109 run `R` past numbers never
received, and once every bit is set it never ends; the model's advance is
bounded at `W + 1` steps and reports that as the same loss.

**NoHalfRange.** TLC's trace: number 0 arrives (`R` = 1), then the sender's
resend of 0 arrives. With the signed difference, line 50 gives `delta = -1`
and line 55 drops it as a duplicate. Without the sign `delta = 15`, which is
at least `W`, so lines 58-62 set `NUMBER_OUTSIDE_WINDOW` and the connection
ends on an ordinary resend.

**NoCollisionTest.** TLC's trace: number 0 is accepted and its descriptor is
live (`R` = 1, slot 0 live with number 0, its delivery not yet released). A
forged record with wire number 4 arrives: `delta = 3 < W`, so the number is 4
at index 0; the bit for index 0 was cleared when `R` passed 0, so line 68 lets
it through, and without lines 71-76 the claim at lines 82-91 overwrites
number 0's descriptor while number 0's delivery still points at it. With the
test the forged record ends the connection as `SLOT_COLLISION`.

## Records

See `RUNS.tsv` (group `rnwindowlayout`).

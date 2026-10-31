# RNReconstructLayout: the wire sequence reconstruction at the code's arithmetic

`RNReconstructLayout.tla` models `rocketnet_reconstruct_near`
(`sdk/rocketnet_protocol.cpp`), which `rocketnet_reconstruct_sequence` and
`rocketnet_reconstruct_ack_largest` call: `base` is expected with its low 32
bits cleared; the candidates are `base | wire`, the one below only when
`base >= 2^32`, and the one above (`base + 2^32`, wrapping at `2^64`),
compared by unsigned distance to expected, the larger winning a tie. Here the
wire modulus is `M` and the space `M * K`. `AckBlockWrap.tla` takes the
reconstruction as the nearest of every candidate; this model checks that the
three-candidate arithmetic is that nearest for every expected and wire value.
Every input is checked where it stands (`Next` is a stutter).

## The instance

`MCRNReconstructLayout.tla`: `M = 8`, a space of 32, so the first lap (no
candidate below) and the last (the candidate above wraps) are both checked.

## Invariants

| Invariant | Meaning |
|---|---|
| `CodeIsNearest` | the code's choice is the nearest candidate, the larger on a tie |
| `RoundTrip` | a value within half the modulus of expected reads back as itself |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNReconstructLayoutBrokenTieSmaller.cfg` | a tie goes to the smaller candidate | `CodeIsNearest` |
| `MCRNReconstructLayoutBrokenBelowGate.cfg` | the candidate below only when `base > M` | `CodeIsNearest` |
| `MCRNReconstructLayoutBrokenNoAbove.cfg` | no candidate above | `CodeIsNearest` |

Each counterexample was checked against the C by hand: tie-smaller and
no-above at expected 4, wire 0 return 0 where the code returns 8; below-gate
at expected 8, wire 5 returns 13 where the code returns 5.

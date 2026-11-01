# RNFilterEpochs: the receiver's filter epoch window

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

`proofs/tla/RNFilterEpochs.tla` is the receiver side of the keyed filter value's
generations (`STANDARD.md`, "The keyed filter value"): the table of live
magics a keyed receiver holds, the backend handing it the next generation,
the oldest retired when the table is full, and the verdict on a packet's
epoch byte. The cookie key rotation in `proofs/tla/RNSecurityTables.tla` is the
worked example of a rotating-key machine; this module is the filter's own.

Rocketnet 1.0 is unkeyed: `rocketnet_prefilter_check`
(`sdk/rocketnet_protocol.cpp`) refuses every epoch but 0 on a comparison.
This is a historical keyed-filter design, with no v1.0 implementation.
The `owed` status of `rn-filter-epochs` in `proofs/tla/COVERAGE.tsv` records
that absence; it is not a promise of an active implementation card.

`proofs/tla/MCRNFilterEpochs.tla` and `proofs/tla/MCRNFilterEpochs.cfg` are the small
instance. `proofs/tla/MCRNFilterEpochsBrokenAcceptZero.cfg` and
`proofs/tla/MCRNFilterEpochsBrokenWrapToZero.cfg` are the reversed witnesses. The
cases are rows of `proofs/tla/CASES.tsv` (group `rnfilterepochs`) with measured
records in `proofs/tla/RUNS.tsv`.

## The rule

- **Generations.** They advance 1, 2, 3 and wrap from 255 back to 1. Zero is
  never a generation, so a wrap cannot turn a keyed deployment unkeyed.
- **The window.** A receiver accepts at least two live generations, so a
  rotation does not refuse packets already in flight, and drops any epoch
  outside its window. How many beyond two is local policy.
- **Epoch 0.** A receiver never treats epoch 0 as keyed, and a keyed
  deployment refuses it rather than falling back to a constant.
- **Cost.** An epoch naming a generation the receiver does not hold is
  dropped on a comparison, before any MAC. A live epoch costs one MAC, under
  the magic that epoch names.

## What the module covers

Constants: `N`, the number of generations (`1..N`, standing for `1..255`
with the same wrap rule); `Live`, the table's capacity; `MaxInstalls`, the
bound on installs. A key is named by its install index `k`; the magic
itself is not modelled.

Variables: `live`, the table oldest first, each entry a generation `g` and
its key `k`; `installs`, the number of keys installed (the newest key's
index); `genOf`, the generation handed with each key; `last`, the verdict
of the most recent check, its epoch `e`, the sender's key `k` and `macs`,
a ghost count of the MACs that check computed. An `Install` or a `Retire`
clears `last`, so every invariant over it reads the table the check saw.

Two rule switches are definitions with the rule's values, overridden in a
configuration: `AcceptZero` (`FALSE`; the broken shape falls back to the
constant at epoch 0) and `WrapTo` (`1`, the generation after `N`).

Actions:

- `Install(g)`: the backend hands generation `g`, the next of the newest
  (`1` first). Enabled while the table has room.
- `Retire`: the oldest generation leaves the table. Enabled only when the
  table is full.
- `Check(e, k)`: a packet carrying epoch byte `e`, its filter value computed
  under key `k` (`k = 0`: under no key, a forger). Epoch 0 is refused with
  no MAC. An epoch naming no live generation is refused with no MAC. A live
  epoch computes one MAC under the key held for it and is accepted exactly
  when that key is `k`.

Invariants:

- `TypeOK`.
- `NeverKeyedAtZero`: a check of epoch 0 is never accepted and computes no
  MAC.
- `WrapSkipsZero`: every live generation is in `1..N`, and each install is
  the next of the one before under the rule (`N` wraps to `1`).
- `AtLeastTwoLiveAfterRotation`: once a second generation is installed, at
  least two are live in every state, between the steps of a rotation too.
- `DeadEpochRefusedWithoutMac`: an epoch in `1..N` naming no live
  generation is refused with no MAC.
- `OneMacPerLiveEpoch`: a live epoch costs exactly one MAC.
- `LegitimateNeverRefused`: a sender under the newest key, or the key
  before it, carrying that key's epoch, is accepted.
- `OldKeyRefused`: an accepted key is in the table and among the last
  `Live` installed: never once retired, and never because a later key
  wrapped onto the same epoch.
- `LiveGenerationsDistinct`: each live generation names one key, so the
  epoch byte is unambiguous.

## Abstractions

- **The MAC.** A filter value matches exactly when it was computed under
  the key the receiver holds for its epoch. A forged 32-bit match (one in
  4.3 billion) is outside the model; the sealed layer beneath carries the
  guarantees.
- **Senders.** A legitimate sender holds the key it was most recently given
  or, for a packet in flight across one rotation, the key before it. An
  attacker chooses any epoch byte with any key, or none.
- **Retire before Install.** The table drops its oldest only when full and
  takes a new generation only with room, so a capacity of three is what
  keeps two live across a rotation; the module assumes `Live >= 3` and
  `Live <= N` (a window wider than the generation space would alias an
  epoch byte).
- **The schedule.** When the backend rotates, and when the table retires,
  are free: every interleaving with checks is explored.

## The small instance

`MCRNFilterEpochs.cfg`: `N = 4`, `Live = 3`, `MaxInstalls = 9` (two wraps).
TLC explores every install, retire and check interleaving in 537 distinct
states (19,907 generated, depth 18) and every invariant holds.

## Reversed witnesses

| Configuration | Rule removed | Expected failure |
|---|---|---|
| `MCRNFilterEpochsBrokenAcceptZero.cfg` | epoch 0 refused when keyed: `AcceptZero <- AcceptZeroBroken`, a fallback to the unkeyed constant | `NeverKeyedAtZero`, exit 12: from the initial state a check of epoch 0 with no key is accepted. `STANDARD.md` requires a keyed deployment to reject epoch 0, since a fallback an attacker can select is not a filter. |
| `MCRNFilterEpochsBrokenWrapToZero.cfg` | the wrap to 1: `WrapTo <- WrapToZero`, the generation after `N` is 0 | `WrapSkipsZero`, exit 12: installs 1, 2, 3, a retire, 4, a retire, then the fifth install hands generation 0 into the table beside 3 and 4. `STANDARD.md` wraps 255 to 1 and never makes zero a generation. |

Both traces were checked by hand against `STANDARD.md`. Neither has code to
replay against yet: `rocketnet_prefilter_check` holds no generation table
and refuses every nonzero epoch, so the keyed rule and its unit test
replaying these witnesses are the code card's.

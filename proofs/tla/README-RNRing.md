# RNRing: the receive ring's placement rule

`proofs/tla/RNRing.tla` is the sender's placement rule for the receive ring and the
receiver's live spans: docs/CHANNELS.md section 6.2 (the byte bound), 6.3 step 3
(why the receiver never runs out of room), section 8 (a block's span) and section
15's `RNRing` row. It was written before any of the rule's code exists: the only
ring code on main is the receiver's local check, `rn_region_ring_span_valid` in
`sdk/rocketnet_allocator.cpp` (a placement is 16-aligned and inside `B`), which is
what the module's `SpanInsideRing` and `Aligned` say a correct sender never trips.

`proofs/tla/MCRNRing.tla` and `proofs/tla/MCRNRing.cfg` are section 15's small instance. The four
`proofs/tla/MCRNRingBroken*.cfg` files are the reversed witnesses. Every TLC run is a row
for `proofs/tla/RUNS.tsv` (listed below; this change does not edit that file).

## What the module covers

State the sender owns (6.1):

- `next`: the next message number.
- `ringNext`: `ring_next`, an absolute byte position, never reduced modulo `B`.
- `span[n] = [s, e)`: the absolute ring span of every numbered record: the
  per-number table's `ring_start`, and the record's size rounded up to 16
  bytes past it.
- `first[n]`: the first number of `n`'s block, `n` itself for a single record. `U'`
  is `first[U]`.
- `acked`: the numbers the sender has seen acked. `U` is the lowest unacked number,
  `next` when nothing is outstanding.
- `pending`: the one record waiting to be numbered, so that liveness can be stated.
- `blocks`: how many blocks have been numbered (a size knob).

State the receiver owns (6.3 step 1):

- `opened`: the numbers whose records the receiver has opened, which is when their
  bytes are written into the ring. A second arrival is a duplicate (6.5) and writes
  nothing.
- `live`: the numbers whose spans are live: opened, and not yet released.

Actions, one per operation and per outside event:

| Action | Who | What |
|---|---|---|
| `WantSingle(len)`, `WantBlock` | the game | a record arrives and waits for its number; one at a time, and only one the numbering budget can still give |
| `PlaceSingle` | the sender | 6.2: `len16` is the length rounded up to 16; `p = ring_next` unless `[ring_next, ring_next + len16)` crosses a multiple of `B`, then the next multiple of `B` with the skipped bytes as padding; admitted only if `p + len16 - floor <= B` |
| `PlaceBlock` | the sender | section 8: `k` consecutive numbers, one contiguous span of `k * F`, fragment `i` at `p + i * F`, both bounds checked once for the whole block |
| `Open(n)` | the receiver | record `n` arrives and its bytes are written at its span; `n` is live from here |
| `Release(n)` | the receiver | `n` was delivered and the update ended; a block is released whole, once every fragment has been opened |
| `Ack(n)` | the receiver's ack | the sender learns `n` was acked (see the assumption below) |

The floor is `ring_start(U')` while something is outstanding (`U' < next`) and `p`
when nothing is. `U' = U` except when `U` lies inside a block that is not fully
acked, when `U'` is that block's first number.

Invariants, from section 15 and section 3's layer 3 guarantee ("Its slot and ring
writes never touch a live message"):

- `LiveSpansDisjoint`: no two live spans share a ring byte (every span taken modulo
  `B`).
- `SpanInsideRing`: every span, taken modulo `B`, lies inside `[0, B)` without
  wrapping. This is `rn_region_ring_span_valid`'s test, as a property of the
  sender.
- `Aligned`: every placement is a multiple of 16.
- `IdleAdmitsAnyMessage`: 6.2's last paragraph as a state predicate: with nothing
  outstanding the ring admits any single record and a block, wherever `ring_next`
  is.
- `TypeOK`.

Liveness, under weak fairness on place, open, release and ack (`Fairness` in the
module; no safety property depends on it):

- `EveryAdmissibleMessagePlaced`: when nothing is outstanding and a message of at
  most `max_message_bytes <= B` waits, it is placed (6.2).
- `NoRecordWaitsForever`: a waiting message is placed, whatever is outstanding
  when it arrives ("No record waits forever", 6.2).

## What the module abstracts away, and the one assumption

- **Acks are abstracted, and `U'` is assumed to move only past released
  records.** 6.3 step 2 argues from P2, P6 and the scheduler's first-write order
  that when the ack that moves `U'` past `n` was built, `n` had been released.
  That argument is `RNWindow`'s to check (the sibling card
  `rnwindow-model-checks-every-reliable-number-delivered-once`). Here it is a
  precondition of `Ack(n)`: `n` must be opened (P2: acked means opened), and the
  ack that completes a block, or acks a single record, needs the record released.
  A fragment of an incomplete block may be acked while it is still held; that is
  the case `U'` exists for, and the first witness needs it.
- **No link.** Placement is decided by the sender alone and the receiver's check
  is local, so loss, duplication and reordering appear only as the freedom to
  open and ack numbers in any order, and a duplicate arrival is a no-op.
- **The count bound** `n < U' + W` and the numbering of non-holdable records
  (`RNWindow`). Every numbered record here is holdable and takes ring bytes.
  `MaxNumbers` bounds the behaviour instead, like `Link`'s `Capacity`: a size knob,
  never a hidden rule.
- **Resends.** A resend carries the same ring fields and is a duplicate at the
  receiver, so it changes nothing here.
- **Channels, ordering holds and dispatch** (layer 4). A hold is just a longer
  time between `Open` and `Release`, which the model already allows.
- **The send store, the sent-packet map, the resend delay.**

One thing the design leaves open, found while writing `Floor`: a code 2 record
that is not a block "takes a number but no ring bytes" (6.2), so if `U'` lands on
one, `ring_start(U')` must still be defined. The natural choice, and the one the
code should make explicit, is the empty span `[ring_next, ring_next)` at the
moment it was numbered. This module does not number such records.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19 or later), from
`proofs/tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -config MCRNRing.cfg MCRNRing
```

`MCRNRing.cfg` is the committed run, every rule on. The small instance is section
15's: `B = 64` (4 units of 16), `Lengths = {1, 32, 48}` (1 to 3 units once rounded;
the 1 is there so that rounding has something to do), one block (`MaxBlocks = 1`)
of two 16-byte fragments (`BlockFragments = 2`, `FragBytes = 16`), and six numbers
(`MaxNumbers = 6`).

The witnesses are the same file with one `CONSTANTS` line changed (two lines for
the third, which also leaves out the invariant that would stop TLC before the
temporal check):

| Run | Rule removed | Expected |
|---|---|---|
| `MCRNRing.cfg` | none | passes |
| `MCRNRingBrokenFloorAtU.cfg` | `FloorAtUPrime = FALSE`: the floor is `ring_start(U)` | **fails** `LiveSpansDisjoint` |
| `MCRNRingBrokenNoPadding.cfg` | `PadAtBoundary = FALSE`: no padding at the boundary | **fails** `SpanInsideRing` |
| `MCRNRingBrokenIdleFloor.cfg` | `IdleFloorAtP = FALSE`: the idle floor is the last record's start | **fails** `EveryAdmissibleMessagePlaced` (and `IdleAdmitsAnyMessage` when it is checked) |
| `MCRNRingBrokenNoRounding.cfg` | `Round16 = FALSE`: `len16` is the raw length | **fails** `Aligned` |

## Runs

TLC 2.19 (rev 5a47802), bench `hetzner2`, `-workers 4` for the committed run and 1
for each witness, 2026-10-13. The rows for `proofs/tla/RUNS.tsv`, in its layout:

```
MCRNRing	MCRNRing.cfg (every rule on; TypeOK, LiveSpansDisjoint, SpanInsideRing, Aligned, IdleAdmitsAnyMessage; EveryAdmissibleMessagePlaced, NoRecordWaitsForever)	222691	79377	pass	hetzner2	2026-10-13
MCRNRing	MCRNRingBrokenFloorAtU.cfg (FloorAtUPrime FALSE; reversed witness)	3544	1544	fail as expected: Invariant LiveSpansDisjoint is violated at depth 10, a held fragment overwritten, see README-RNRing.md	hetzner2	2026-10-13
MCRNRing	MCRNRingBrokenNoPadding.cfg (PadAtBoundary FALSE; reversed witness)	809	456	fail as expected: Invariant SpanInsideRing is violated at depth 8, a span wraps	hetzner2	2026-10-13
MCRNRing	MCRNRingBrokenIdleFloor.cfg (IdleFloorAtP FALSE; reversed witness)	149	115	fail as expected: Invariant IdleAdmitsAnyMessage is violated at depth 6, an idle ring refuses a record that fits	hetzner2	2026-10-13
MCRNRing	MCRNRingBrokenIdleFloor.cfg without IdleAdmitsAnyMessage, -deadlock (reversed witness)	64946	20769	fail as expected: Temporal properties were violated, EveryAdmissibleMessagePlaced, stuttering lasso at depth 8, the block waits forever	hetzner2	2026-10-13
MCRNRing	MCRNRingBrokenNoRounding.cfg (Round16 FALSE; reversed witness)	30	30	fail as expected: Invariant Aligned is violated at depth 5	hetzner2	2026-10-13
```

The committed run finished in 6 s at depth 31. Each witness finished in under a
second except the temporal one, 3 s. Every witness was also run on the first
draft of the module (before the `Done` self-loop, below) with the same result.

## The counterexamples, replayed by hand

Each trace was replayed through the module's actions and through the rule as
section 6.2 states it. None is a defect in rocketnet code: no placement code exists
yet, and the receiver's check on main (`rn_region_ring_span_valid`) would refuse
the spans the second witness produces, as 6.3 says ("a violation, never memory
corruption").

1. **`MCRNRingBrokenFloorAtU.cfg`: `Invariant LiveSpansDisjoint is violated`, at
   depth 10** (3,544 states generated, 1,544 distinct).
   - **The trace:** the block is numbered 0 and 1 at `[0,16)` and `[16,32)`,
     `ring_next = 32`. A 32-byte record is numbered 2 at `[32,64)`, `ring_next =
     64`. The receiver opens fragment 0 (live: `{0}`). The sender sees 0 acked:
     the block is incomplete, so fragment 0 stays held. Now `U = 1` and `U' = 0`.
     A 1-byte record (`len16 = 16`) asks for `p = 64`; with the floor at
     `ring_start(U) = 16` the test is `64 + 16 - 16 = 64 <= 64`, admitted, number 3
     at `[64,80)`. The receiver opens 3: `[64,80)` modulo 64 is `[0,16)`, fragment
     0's bytes, still held.
   - **Against the rule, by hand:** with the floor at `ring_start(U') = 0` the test
     is `64 + 16 - 0 = 80 > 64` and the record waits. 6.2's "`U' = U`, except when
     `U` falls inside a block that is not fully acked" is exactly the rule the
     witness removes, and section 8's "That slot cannot be reused while the block
     is incomplete, because `U'` stops at `b`" is what it buys.
2. **`MCRNRingBrokenNoPadding.cfg`: `Invariant SpanInsideRing is violated`, at
   depth 8** (809 states generated, 456 distinct).
   - **The trace:** a 32-byte record is numbered 0 at `[0,32)`, opened, released
     and acked, so nothing is outstanding. A 48-byte record asks for a span from
     `ring_next = 32`: `[32,80)` crosses 64. Without padding `p = 32`; the floor
     is `p` (idle), `32 + 48 - 32 = 48 <= 64`, admitted. `(32 mod 64) + 48 = 80 >
     64`: the span wraps.
   - **Against the rule, by hand:** with padding `p = 64`, `64 + 48 - 64 = 48 <=
     64`, admitted at `[64,112)`, which is `[0,48)` modulo 64, inside the ring.
     The receiver's check on main, `rn_region_ring_span_valid(region, 32, 48)`
     with `ring_bytes = 64`, returns false (`48 <= 64 - 32` fails), so a sender
     without padding is refused as a violation, never a write past the ring.
     Before the ack the floor is 0 and `32 + 48 - 0 = 80 > 64` refuses the record
     either way: the byte bound alone hides the missing padding until the ring is
     idle, which is why the trace acks first.
3. **`MCRNRingBrokenIdleFloor.cfg`: `Invariant IdleAdmitsAnyMessage is violated`,
   at depth 6** (149 states generated, 115 distinct).
   - **The trace:** a 32-byte record is numbered 0 at `[0,32)`, opened, released
     and acked, so nothing is outstanding and `ring_next = 32`. The predicate asks
     whether a 48-byte record would be admitted: `[32,80)` crosses 64 so `p = 64`;
     with the idle floor at the last record's start, `64 + 48 - 0 = 112 > 64`. An
     idle ring refuses a record that fits in it.
   - **The same run with `IdleAdmitsAnyMessage` left out and TLC's `-deadlock`
     flag: `Temporal properties were violated`** (64,946 states generated, 20,769
     distinct), a stuttering lasso at depth 8. A 48-byte record is numbered 0 at
     `[0,48)`, opened, released and acked; the block then asks for 32 bytes from
     `ring_next = 48`: `[48,80)` crosses 64 so `p = 64`. With the idle floor at the
     last record's start, `64 + 32 - 0 = 96 > 64`, refused, and since nothing is
     outstanding no step can ever change that: the block waits forever. Without
     the flag TLC reports the same state as `Deadlock reached`, which it is: a
     waiting record with no step enabled.
   - **Against the rule, by hand:** the floor is `p` when nothing is outstanding
     (6.2), so `64 + 32 - 64 = 32 <= 64` and the block is placed at `[64,96)`.
     This is 6.2's last paragraph: with nothing outstanding any message of at most
     `max_message_bytes <= B` fits.
4. **`MCRNRingBrokenNoRounding.cfg`: `Invariant Aligned is violated`, at depth 5**
   (30 states generated, 30 distinct).
   - **The trace:** a 1-byte record is numbered 0 at `[0,1)`, `ring_next = 1`; the
     next 1-byte record is placed at `[1,2)`, and 1 is not a multiple of 16.
   - **Against the rule, by hand:** `len16` rounds 1 up to 16, so `ring_next` is
     16 and the second record starts at 16. Section 4 relies on this: "every ring
     placement, being a multiple of 16, is 16-aligned", and
     `rn_region_ring_span_valid` refuses `offset % 16 != 0`.

A fifth run, not a witness, found a model error on the first pass: the committed
run reported `Deadlock reached` at the end of the numbering budget (all six
numbers given and acked, nothing waiting). The `Done` self-loop in `Next` is the
fix, so that TLC's deadlock check is kept for real stalls, as in witness 3.

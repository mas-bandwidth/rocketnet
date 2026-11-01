# RNSimulatorStoreLayout

The development link simulator's slot store and its destination index, at the
layout of `sdk/rocketnet_network_simulator.cpp` and with the code's own
arithmetic. [Link](README-Link.md) is the wire's contract: what the simulator
loses, duplicates, reorders and delays. This model is the store that holds
the packets in flight and the index that picks the next one, whose whole
observable order is the selection rule at the top of the file: `receive`
delivers the lowest occupied slot that is due and addressed to the caller,
and `send` places a packet in the lowest free slot.

## State

| Model | Code |
|---|---|
| `used[s]`, `dt[s]`, `to[s]` | `used`, `delivery_time`, `to` |
| `gen[s]` | `generation`, the per-slot reuse tag, 0 before first use |
| `sis[s]` | `slot_index_shard`, the shard or `ROCKETNET_SIMULATOR_NO_SLOT` |
| `occ[sh][w]` | `shard_occupancy[sh * occupancy_words + w]` |
| `sum[sh][v]` | `shard_summary[sh * summary_words + v]` |
| `minDT[sh]` | `shard_min_delivery_time[sh]`, `DBL_MAX` is `T + 1` |
| `hint` | `free_hint`: every slot below it is occupied |
| `now` | the caller's `current_time` |

A word is the set of its set bit positions: a word is zero when its set is
empty and `ctz` is the set's minimum. Slot `s` is bit `s % W` of occupancy
word `s \div W` (`slot & 63`, `slot >> 6`); occupancy word `w` is bit
`w % SB` of summary word `w \div SB`. The code's `W` and `SB` are 64 and its
shard count is 64; the model takes them as constants so the word boundaries
are reached at three slots. The shard is the address hash masked by the shard
count; the model's hash is the identity, so shard is `d % NShards` and
destinations 0 and 2 share shard 0, which is why the walk's address predicate
matters.

Generations run modulo `GenMod` (2 in the instance); the code's are 64-bit
and never wrap. The model keeps one stale handle per slot, the last one
retired (`oldGen`, `oldValid`), and a reused slot's generation differs from
it after one bump at any modulus of 2 or more, so the comparison is exact for
that handle.

## Actions and the code they mirror

- **`Send(d, t)`**: `rocketnet_simulator_insert` with
  `rocketnet_simulator_index_add`, reached from
  `rocketnet_network_simulator_send` (through `rocketnet_simulator_emit` and
  `rocketnet_network_simulator_flush_window`) and
  `rocketnet_network_simulator_deliver_packets`. The scan from `free_hint`
  for the first free slot, `generation[i]++`, the slot filled, its
  occupancy bit, the summary bit when the word was empty, the shard bound
  lowered, `free_hint = i + 1`; with no free slot `free_hint = capacity` and
  the packet is dropped.
- **`Receive(d)`**: `rocketnet_network_simulator_receive`:
  `rocketnet_simulator_select_fast` (return at once when the shard bound is
  past `now`; else summary words low to high, their set bits low to high,
  each named occupancy word's set bits low to high, the first due slot
  addressed to `d`; a walk that finds none sets the bound to the minimum it
  saw), then `rocketnet_simulator_retire` of the chosen slot's handle:
  `rocketnet_simulator_index_remove` (refuses a free slot or a changed
  generation; clears the bit and, when the word became empty, its summary
  bit; leaves the bound), `used` cleared, `free_hint` lowered to the slot.
  The ghost `mismatch` records a fast choice that differs from
  `rocketnet_simulator_select_reference`, which is the audit's comparison.
- **`ReceiveAny`**: `rocketnet_network_simulator_receive_any`: the lowest
  used slot due now, any destination, retired through its handle.
- **`StaleRetire(s)`**: a caller presents the handle last retired from slot
  `s` (`rocketnet_network_simulator_test_remove_handle`); `index_remove`
  refuses it unless the slot holds that generation again.
- **`Tick`**: the caller's clock, an environment action.

## Invariants

- **`SelectMatchesReference`**: every receive chooses the slot the selection
  rule names (the audit's comparison, at every step).
- **`SendTakesLowest`**: send places a packet in the lowest free slot and
  drops it only when no slot is free.
- **`NoStaleRemoval`**: a stale handle never retires a live packet.
- **`HintBelowFree`**: every slot below `free_hint` is occupied
  (`rocketnet_simulator_audit_index`'s check).
- **`OccupancyExact`**: a slot's bit is set exactly in its destination's
  shard and only while the slot is used.
- **`MembershipExact`**: `slot_index_shard` names the shard of every used
  slot and is `NO_SLOT` otherwise.
- **`SummaryExact`**: a summary bit is set exactly when its occupancy word is
  nonzero.
- **`BoundBelowShard`**: a shard's bound is at or below every delivery time in
  the shard.

`SummaryExact`, `OccupancyExact` and `BoundBelowShard` are why the fast walk
finds what the full scan finds: it visits every occupied slot of the shard in
ascending order, and it skips the walk only when nothing in the shard is due.

## The instance

Three slots, two shards, three destinations, times 0..1, `GenMod = 2`. Two
passing controls split the word arithmetic so each loop of the walk iterates:

- `MCRNSimulatorStoreLayout.cfg`: `W = 2`, `SB = 1`: two occupancy words
  (the second half full) and two summary words.
- `MCRNSimulatorStoreLayoutSummaryBits.cfg`: `W = 1`, `SB = 2`: three
  occupancy words and two summary words (the second names one word).

Both use `VIEW View`: a free slot's delivery time and destination are dead
(insert writes both before anything reads them). Larger instances do not fit
the record budget on the shared bench (five slots ran past ten minutes; four
had passed 466,000 distinct states at 90 s); three slots reach every word
boundary. The witnesses run at the first control's constants without the
view.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNSimulatorStoreLayoutBrokenNoLowerHint.cfg`** (`LowerHint = FALSE`):
   `rocketnet_simulator_retire` without `if ( handle->slot <
   simulator->free_hint ) simulator->free_hint = handle->slot;`. Violates
   `SendTakesLowest`: a send takes slot 0 (`free_hint = 1`), a receive retires
   it and leaves the hint at 1, and the next send's scan starts at 1 and takes
   slot 1 while slot 0 is free.
2. **`MCRNSimulatorStoreLayoutBrokenSummaryCleared.cfg`**
   (`SummaryOnEmpty = FALSE`): `rocketnet_simulator_index_remove` clearing
   the summary bit without the `if ( *occupancy == 0 )` test. Violates
   `SelectMatchesReference`: two sends to destination 0, both due at 0, take
   slots 0 and 1, which share occupancy word 0 of shard 0; receiving slot 0
   clears bit 0 of that word and also summary bit 0 while bit 1 is still set;
   the next receive's walk sees summary word 0 as zero and returns
   `NO_SLOT`, while the reference scan returns slot 1.
3. **`MCRNSimulatorStoreLayoutBrokenBoundRaised.cfg`** (`KeepBoundLow =
   FALSE`): a remove that raises the shard bound to `DBL_MAX` when the removed
   packet was at it, the "earliest left" bug. Violates
   `SelectMatchesReference`: slots 0 and 1 hold packets for destination 0 due
   at 0, the bound is 0; receiving slot 0 sets the bound to `DBL_MAX`; the
   next receive returns at `shard_min_delivery_time[shard] > current_time`
   without a walk, while slot 1 is due. The code leaves the bound low, which
   costs a walk and hides nothing.
4. **`MCRNSimulatorStoreLayoutBrokenNoGenerationBump.cfg`**
   (`BumpGeneration = FALSE`): `rocketnet_simulator_insert` without
   `simulator->generation[i]++`. Violates `NoStaleRemoval`: a packet in slot
   0 under generation 0 is received (handle `{0, 0}` retired), the next send
   reuses slot 0 still at generation 0, and `rocketnet_simulator_index_remove`
   accepts the old handle `{0, 0}` and retires the new packet.

## Runs

The records are in `RUNS.tsv` (group `rnsimulatorstorelayout`), made with the
pinned runner as `proofs/tla/README.md` describes.

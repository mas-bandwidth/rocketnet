# RNConnectionMemory

Per-connection message memory, and the kick on exhaustion: docs/CHANNELS.md
section 4 ("Who owns each region, and exhaustion kicks"), implemented in
`sdk/rocketnet_allocator.cpp` by `rn_region_send_alloc`, `rn_region_exhaust`,
`rn_connection_memory_decide` and `rn_region_reset`, and the arena's create
paths (`rn_arena_create`, `rn_arena_create_in_block`). The unit tests are in
`test_unit_connection_memory.cpp`.

A server owns one region per client slot, all carved from one block at create.
A client owns one region; it is the same machine with one slot.

## The machine

- **State.** Each slot is `Free`, `Connected` or `Disconnected`. Its region has
  a `used` count against a fixed `Capacity`, a sticky `exhausted` flag, the
  `reason` the connection ended with, and `records`: the clients whose records
  the region still holds.
- **Actions.**
  - `Connect(s, c)`: a client takes a free slot.
  - `Use(s)`: a send, a receive placement or a hold takes room
    (`rn_region_send_alloc` finding room).
  - `UseSpan(s, n)`: a large send takes `n` records at once, a descriptor and
    a span, under the same `~exhausted` guard as `Use`
    (`rn_region_send_alloc_span` finding room).
  - `Release(s)`: an ack or a released delivery gives room back
    (`rn_region_send_free`, `rn_region_send_free_fragment`), also while
    exhausted; that room is not taken again before `Reuse`.
  - `Exhaust(s)`: an operation finds no room. The region takes its sticky
    exhausted state with the reason `MESSAGE_MEMORY_EXHAUSTED`
    (`rn_region_exhaust`, called from the refusals of `rn_region_send_alloc`
    and `rn_region_send_alloc_span`).
  - `Kick(s)`: the owner, reading `rn_connection_memory_decide`, disconnects
    the connection with that reason. A server kicks the slot; a client
    disconnects itself.
  - `Leave(s)`: an ordinary disconnect, not from exhaustion.
  - `Reuse(s)`: the slot is freed and its region reset (`rn_region_reset`,
    which is `rn_region_carve`). The server performs the two halves of this
    step at two times (the lazy reset): the update its connection ends in
    frees the slot in O(1) (`rn_message_connection_release`,
    sdk/rocketnet_message_server.cpp), and the Observe that binds the slot
    to its next connection carves the region before anything else touches it.
    Between the two the slot is Free, and no action of a Free slot reads or
    writes its region (every path checks CONNECTED, and `rn_slots_admit`
    discards for an unbound slot), so the split is a stutter of this step
    and the model keeps it one step. proofs/tla/RNSlots.tla models the split itself
    (ResetFree keeps the region, Observe carves it).
- **Fairness.** Weak fairness on `Kick(s)` and `Reuse(s)` for every slot.

## Properties

- **`ExhaustionOnlyLeadsToDisconnect`** (action property): from an exhausted,
  connected slot, the steps that change that slot are the kick to
  `Disconnected` with the reason kept, and a release that gives one record
  back; no further use, span or delivery takes room.
- **`ReusedSlotStartsClean`** (invariant): a free slot has `used = 0`, no
  exhaustion, no reason and no records, so the `Connect` that takes it sees a
  fresh region. A connected slot holds records of its own client only.
- **`NoCrossSlotEffect`** (action property): every step changes at most one
  slot. One slot's use or exhaustion never changes another slot's state or
  region.
- **`ExhaustedEventuallyFree`** (liveness): an exhausted slot is eventually
  `Free`, kicked and then reset.
- **`ExhaustedReasonNamed`** (invariant): a slot is exhausted exactly when its
  reason is `MESSAGE_MEMORY_EXHAUSTED`.
- **`UsedBounded`** and **`TypeOK`**.

## Reversed witnesses

| Case | Change | Must fail |
|---|---|---|
| `MCRNConnectionMemoryBrokenNoReset.cfg` | `Reset = FALSE`: `Reuse` frees the slot without the carve | `ReusedSlotStartsClean` |
| `MCRNConnectionMemoryBrokenCarryOn.cfg` | `CarryOn = TRUE`: an exhausted connection keeps taking room (`Use`, `UseSpan`) | `ExhaustionOnlyLeadsToDisconnect` |

The traces TLC printed, checked against the code by hand:

- **BrokenNoReset**, depth 4: `Connect(s1, c1)`, `Leave(s1)`, `Reuse(s1)`.
  The slot is `Free` with the reason `LEFT` still set. In the code,
  `rn_region_carve` clears the exhausted state and its reason, every table and
  every free list; a reuse without it leaves the previous connection's
  bookkeeping in place. `test_reused_slot_starts_clean` checks the code's side: a
  region used to exhaustion and reset equals a fresh region byte for byte in
  its bookkeeping, and with the reset removed the test fails.
- **BrokenCarryOn**, depth 5: `Connect(s1, c1)`, `Use(s1)`, `Exhaust(s1)`
  (an operation asks for more room than the region has left), then `Use(s1)`
  on the exhausted slot. This is the step the code took: a send still found
  room after exhaustion. `rn_region_send_alloc` already refused, but
  `rn_region_send_alloc_span` took a descriptor and a span whenever room
  happened to be free. Both allocators now refuse while exhausted (`Use`,
  `UseSpan`), and the guarantee remains the owner's: it reads
  `rn_connection_memory_decide` before any other work of the update and
  kicks. The witness shows that a caller who skips the decision, or an
  allocator that skips the guard, breaks the property.

## Records

Measured on Linux with the pinned runner and JAR (proofs/tla/README.md), 110 seconds
per case, one shard per case:

| Case | Workers | Generated | Distinct | Seconds | Exit | Outcome |
|---|---:|---:|---:|---:|---:|---|
| `MCRNConnectionMemory.cfg` | 2 | 985,441 | 151,849 | 16.628 | 0 | pass |
| `MCRNConnectionMemoryBrokenNoReset.cfg` | 1 | 167 | 123 | 0.632 | 12 | `ReusedSlotStartsClean` violated, as declared |
| `MCRNConnectionMemoryBrokenCarryOn.cfg` | 1 | 1,275 | 500 | 0.740 | 13 | `ExhaustionOnlyLeadsToDisconnect` violated, as declared |

The instance: three slots, four clients, a region of three records.

## What the model leaves to the code

- `Capacity` stands for every part of the region at once. The code has
  several pools (the three store classes, the descriptor pool, the receive
  ring, the holds). Any one of them refusing is `Exhaust`.
- The receive side's no-room sites are not wired to `rn_region_exhaust` yet;
  the objects layer wires them with the kick.
- `Connect`, `Leave` and the disconnect itself are the objects' (layer 8). The
  region provides the state, the reason, the decision and the reset.

## How to run it

On a Linux bench with TLC, from `proofs/tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNConnectionMemory.cfg MCRNConnectionMemory
```

Use `tlacheck run --group rnconnectionmemory` to measure the records, as
proofs/tla/README.md describes.

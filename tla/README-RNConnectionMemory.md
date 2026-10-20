# RNConnectionMemory

Per-connection message memory, and the kick on exhaustion that ends a
connection with `MESSAGE_MEMORY_EXHAUSTED` (a counter of
[t18 Error handling and counters](../standard/TRANSPORT.md#t18)), implemented in
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
  - `Release(s)`: an ack or a released delivery gives room back
    (`rn_region_send_free`, `rn_region_send_free_fragment`).
  - `Exhaust(s)`: an operation finds no room. The region takes its sticky
    exhausted state with the reason `MESSAGE_MEMORY_EXHAUSTED`
    (`rn_region_exhaust`, called from both refusals of `rn_region_send_alloc`).
  - `Kick(s)`: the owner, reading `rn_connection_memory_decide`, disconnects
    the connection with that reason. A server kicks the slot; a client
    disconnects itself.
  - `Leave(s)`: an ordinary disconnect, not from exhaustion.
  - `Reuse(s)`: the slot is freed and its region reset (`rn_region_reset`,
    which is `rn_region_carve`).
- **Fairness.** Weak fairness on `Kick(s)` and `Reuse(s)` for every slot.

## Properties

- **`ExhaustionOnlyLeadsToDisconnect`** (action property): from an exhausted,
  connected slot, the only step that changes that slot is the kick to
  `Disconnected` with the reason kept. No further use, release or delivery.
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
| `MCRNConnectionMemoryBrokenCarryOn.cfg` | `CarryOn = TRUE`: an exhausted connection keeps using and releasing | `ExhaustionOnlyLeadsToDisconnect` |

The traces TLC printed, checked against the code by hand:

- **BrokenNoReset**, depth 4: `Connect(s1, c1)`, `Leave(s1)`, `Reuse(s1)`.
  The slot is `Free` with the reason `LEFT` still set. In the code,
  `rn_region_carve` clears the exhausted state and its reason, every table and
  every free list; a reuse without it leaves the previous connection's
  bookkeeping in place. `reused_slot_starts_clean` checks the code's side: a
  region used to exhaustion and reset equals a fresh region byte for byte in
  its bookkeeping, and with the reset removed the test fails.
- **BrokenCarryOn**, depth 7: `Connect(s1, c1)`, three `Use(s1)`,
  `Exhaust(s1)`, then `Release(s1)` on the exhausted slot. This is the step
  the code's region still allows: `rn_region_send_free` works after
  exhaustion, because the allocator's unit tests free and refill after a
  refusal. The guarantee is the owner's: it reads `rn_connection_memory_decide` before any
  other work of the update and kicks. The witness shows that a caller who
  skips the decision breaks the property.

## Records

Measured on Linux with the pinned runner and JAR (tla/README.md), 110 seconds
per case, one shard per case:

| Case | Workers | Generated | Distinct | Seconds | Exit | Outcome |
|---|---:|---:|---:|---:|---:|---|
| `MCRNConnectionMemory.cfg` | 2 | 236,125 | 46,801 | 3.694 | 0 | pass |
| `MCRNConnectionMemoryBrokenNoReset.cfg` | 1 | 119 | 83 | 0.643 | 12 | `ReusedSlotStartsClean` violated, as declared |
| `MCRNConnectionMemoryBrokenCarryOn.cfg` | 1 | 6,782 | 1,994 | 0.852 | 13 | `ExhaustionOnlyLeadsToDisconnect` violated, as declared |

The instance: three slots, four clients, a region of three records.

## What the model leaves to the code

- `Capacity` stands for every part of the region at once. The code has
  several pools (the three store classes, the descriptor pool, the receive
  ring, the holds). Any one of them refusing is `Exhaust`.
- The receive side's no-room sites (`rn_channels_dispatch_ordered` and
  `rn_channels_dispatch_chunk` in `sdk/rocketnet_channel.cpp`) also call
  `rn_region_exhaust`, and the owner's next `rn_connection_memory_decide`
  kicks the connection.
- `Connect`, `Leave` and the disconnect itself belong to the message objects.
  The region provides the state, the reason, the decision and the reset.

## How to run it

On a Linux bench with TLC, from `tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNConnectionMemory.cfg MCRNConnectionMemory
```

Use `tlacheck run --group rnconnectionmemory` to measure the records, as
tla/README.md describes.

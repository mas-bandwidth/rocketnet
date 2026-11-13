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
  - `Connect(s, c)`: a client takes a free slot. The binding carves the
    slot's region first (`rn_endpoint_reset`, which runs `rn_region_reset`,
    that is `rn_region_carve`, and zeroes the connection's `send_refused`; on
    the server the Observe that binds the slot calls it, RNSlots).
  - `Use(s)`: a send, a receive placement or a hold takes room
    (`rn_region_send_alloc` finding room).
  - `UseSpan(s, n)`: a large send takes `n` records at once, a descriptor and
    a span, under the same `~exhausted` guard as `Use`
    (`rn_region_send_alloc_span` finding room).
  - `Release(s)`: an ack or a released delivery gives room back
    (`rn_region_send_free`, `rn_region_send_free_fragment`), also while
    exhausted; that room is not taken again before `Reuse`.
  - `Refuse(s, n)`: a send start of `n` records that the fit refuses (no
    room, or a region the receive side exhausted): `rn_region_send_refuse`
    counts it and the region and the connection are as they were
    (standard/MESSAGES.md m1.7). A refusal for no room moves the region's
    `room_refusals`, and `rn_endpoint_start` counts the connection's
    `send_refused` only when it sees `room_refusals` move across the start; a
    refusal of an exhausted region moves neither (the endpoint records
    `send_error` instead). `refused[s]` is `send_refused`. `room_refusals`
    is a counter the endpoint only compares across one start, never reset by
    the carve, so the model has no variable for it.
  - `Exhaust(s)`: a receive placement finds no room. The region takes its
    sticky exhausted state with the reason `MESSAGE_MEMORY_EXHAUSTED`
    (`rn_region_exhaust`). A send never exhausts the region: the send
    allocators' refusals call `rn_region_send_refuse`, which counts and
    changes nothing else.
  - `Kick(s)`: the owner, reading `rn_connection_memory_decide`, disconnects
    the connection with that reason. A server kicks the slot; a client
    disconnects itself.
  - `Leave(s)`: an ordinary disconnect, not from exhaustion.
  - `Reuse(s)`: the slot is freed in O(1), its region left as the ended
    connection left it (`rn_message_connection_release`,
    sdk/rocketnet_message_server.cpp): the lazy reset. The carve is the next
    `Connect`'s. Between the two the slot is Free with the old room,
    exhaustion, reason and records still in its region, and no action of a
    Free slot reads or writes its region (every path checks CONNECTED, and
    `rn_slots_admit` discards for an unbound slot). proofs/tla/RNSlots.tla
    models the same split (ResetFree keeps the region, Observe carves it).
- **Fairness.** Weak fairness on `Kick(s)` and `Reuse(s)` for every slot.

## Properties

- **`ExhaustionOnlyLeadsToDisconnect`** (action property): from an exhausted,
  connected slot, the steps that change that slot are the kick to
  `Disconnected` with the reason kept, and a release that gives one record
  back; no further use, span or delivery takes room.
- **`ReusedSlotStartsClean`** (invariant): a connected slot holds records of
  its own client only, and its reason is its own (none, or the receive side's
  exhaustion), never the previous connection's end. A free slot's region may
  still hold the ended connection's state (the lazy reset); the `Connect`
  that takes it carves it first.
- **`RefusalLeavesStateUnchanged`** (action property): a start the fit
  refuses leaves its slot as it was; `refused` moves by one for a refusal for
  no room and not at all for a refusal of an exhausted region.
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
| `MCRNConnectionMemoryBrokenNoReset.cfg` | `Reset = FALSE`: `Connect` binds a reused slot without the carve | `ReusedSlotStartsClean` |
| `MCRNConnectionMemoryBrokenCarryOn.cfg` | `CarryOn = TRUE`: an exhausted connection keeps taking room (`Use`, `UseSpan`) | `ExhaustionOnlyLeadsToDisconnect` |
| `MCRNConnectionMemoryBrokenCountExhausted.cfg` | `CountExhausted = TRUE`: a refusal of an exhausted region is counted in `send_refused` | `RefusalLeavesStateUnchanged` |

The traces TLC printed, checked against the code by hand:

- **BrokenNoReset**: `Connect(s1, c1)`, `Leave(s1)`, `Reuse(s1)`,
  `Connect(s1, c1)` (5 states). The connected slot carries the reason `LEFT`
  of the connection before. In the code, `rn_region_carve` (run by
  `rn_endpoint_reset` when the slot is bound) clears the exhausted state and
  its reason, every table and every free list; a bind without it leaves the
  previous connection's bookkeeping in place. `test_reused_slot_starts_clean` checks the code's side: a
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
- **BrokenCountExhausted** (5 states): `Connect(s1, c1)`, `Use(s1)`,
  `Exhaust(s1)`, then a start on the exhausted slot is refused and `refused`
  goes 0 to 1. In the code `rn_endpoint_start` (sdk/rocketnet_endpoint.cpp)
  tests `rn_region_exhausted` first and records `send_error`, on the
  channels route before it compares `room_refusals` (461-470) and on the
  plain route before any take (480-484); `send_refused` moves only for a
  refusal for no room.

## Records

Measured on Linux with the pinned runner and JAR (proofs/tla/README.md), 110 seconds
per case, one shard per case:

| Case | Workers | Generated | Distinct | Seconds | Exit | Outcome |
|---|---:|---:|---:|---:|---:|---|
| `MCRNConnectionMemory.cfg` | 2 | 985,441 | 151,849 | 16.628 | 0 | pass |
| `MCRNConnectionMemoryBrokenNoReset.cfg` | 1 | 167 | 123 | 0.632 | 12 | `ReusedSlotStartsClean` violated, as declared |
| `MCRNConnectionMemoryBrokenCarryOn.cfg` | 1 | 1,275 | 500 | 0.740 | 13 | `ExhaustionOnlyLeadsToDisconnect` violated, as declared |

The instance: three slots, three clients (four before the lazy reset was
modelled, which keeps a free slot's region as state; with three a client
still takes a slot another client freed), a region of three records.

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

## Revision 1.1: a send with no room is refused, not an end

Standard rule: MESSAGES.md m1.7. A send that finds no room in the region is
refused and counted; the region and the connection are as they were. Only the
receive side's exhaustion ends a connection with MESSAGE_MEMORY_EXHAUSTED.
The new action `Refuse(s)` is the send refusal (`rn_region_send_refuse`,
`sdk/rocketnet_allocator.cpp`); `Exhaust(s)` is now the receive placement
only. The ghost `cause` records the side that exhausted a region.
`SendExhausts = TRUE` is the reversed witness, v1.0's send-side kick.

| Property | Kind | Claim |
|---|---|---|
| `SendNeverEnds` | invariant | no slot is disconnected with cause `"send"` |
| `RefusalLeavesStateUnchanged` | action | a refused send leaves its slot's state, owner, room, exhaustion, reason and records unchanged |
| `ExhaustionOnlyLeadsToDisconnect` | action | narrowed to the receive side's exhaustion, the only one there is |

| Case | Expected | Exit | Distinct |
|---|---|---:|---:|
| `MCRNConnectionMemory.cfg` | pass | 0 | 151849 |
| `MCRNConnectionMemoryBrokenExhaustOnFull.cfg` (`SendExhausts`) | `SendNeverEnds` | 12 | 538 |
| `MCRNConnectionMemoryBrokenRefuseChanges.cfg` (`SendExhausts`) | `RefusalLeavesStateUnchanged` | 13 | 99 |

Witness traces, read by hand: a client connects, its sends fill the region's
three records, the next send is refused and, with `SendExhausts`, sets
`exhausted` with cause `"send"`; `Kick` then disconnects the slot with
MESSAGE_MEMORY_EXHAUSTED (ExhaustOnFull), or the refusing step itself
changes the slot (RefuseChanges). The code before revision 1.1 did this:
`rn_region_send_alloc` and `rn_region_send_alloc_span` called
`rn_region_exhaust` on a failed fit, and the owner kicked the exhausted
connection at its next decision. The code now calls `rn_region_send_refuse`,
which counts the refusal and leaves the region as it was.

## Revision 1.1.2: the refused start in the code's order

Revision 1.1's two properties read the model's own definitions back: `Refuse(s)`
was `UNCHANGED vars`, and the ghost `cause` behind the invariant
`SendNeverEnds` was never written to `"send"` (a probe of both passed on
`MCRNConnectionMemory.cfg`, 151,849 distinct). The ghost `cause` is removed.

- `Refuse(s, n)` is a start of `n` records (one, or a span) that the fit
  refuses (`FitOK(s, n) == ~exhausted[s] /\ used[s] + n <= Capacity`), with
  the code's steps in order: the fit only reads (`rn_region_send_fit_store`,
  `rn_region_send_fit_span`), then `rn_region_send_refuse` counts
  (`sdk/rocketnet_allocator.cpp`). The per-slot counter `refused`
  (mod `CountMod`) is the one variable a refusal may change, and it is
  excluded from the slot's state by name.
- `StartStep(s, n)` is every start (`Use`, `UseSpan`, `Refuse`). A refused
  start is named by the fit's decision on the state before the step, not by
  the action, so a start that takes room before it fits is caught.
- `SendNeverEnds` is an action property read on the resulting state: no start,
  refused or not, changes its slot's state, exhausted flag or reason.
- `RefusalLeavesStateUnchanged`: a start the fit refuses leaves `Slot(s)`
  (state, owner, room, exhaustion, reason, records) as it was and moves
  `refused[s]` by one.
- `TakeBeforeFit = TRUE` (reversed witness): a span start takes a record
  before its room is known and returns with it still taken.

| Property | Fails when | Witness |
|---|---|---|
| `SendNeverEnds` | a refused start exhausts the region (`SendExhausts`) | `MCRNConnectionMemoryBrokenExhaustOnFull.cfg` |
| `RefusalLeavesStateUnchanged` | a span start takes a record before the fit (`TakeBeforeFit`), or exhausts the region (`SendExhausts`) | `MCRNConnectionMemoryBrokenTakeBeforeFit.cfg`, `MCRNConnectionMemoryBrokenRefuseChanges.cfg` |

| Case | Expected | Exit | Distinct | Seconds (2 workers, vision) |
|---|---|---:|---:|---:|
| `MCRNConnectionMemory.cfg` (`CountMod = 1`) | pass | 0 | 151849 | 43.5 |
| `MCRNConnectionMemoryRefuse.cfg` (two slots, three clients, `CountMod = 3`) | pass | 0 | 15301 | 4.0 |
| `MCRNConnectionMemoryBrokenExhaustOnFull.cfg` | `SendNeverEnds` | 13 | 99 | 1.0 |
| `MCRNConnectionMemoryBrokenRefuseChanges.cfg` | `RefusalLeavesStateUnchanged` | 13 | 99 | 1.0 |
| `MCRNConnectionMemoryBrokenTakeBeforeFit.cfg` | `RefusalLeavesStateUnchanged` | 13 | 97 | 1.0 |

TakeBeforeFit's trace (5 states), checked by hand against the code: `Connect(s1, c1)`,
two `Use(s1)` (`used` 2 of 3), then a two-record span start that does not fit
takes one record (`used` 2 to 3) and is refused.
The code cannot do this: `rn_region_send_alloc_span` fits at
`sdk/rocketnet_allocator.cpp` on a const region and refuses at 1142,
and takes its descriptor only at 1148, after the fit; `rn_region_send_alloc`
fits at 901, refuses at 904, and takes at 908 and 910.

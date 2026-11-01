# RNClientSlotsLayout

The client slots' free stack and owner generations, at the layout of
`sdk/rocketnet_client_slots.cpp` and with the code's own arithmetic.
[RNOpenAddress](RNOpenAddress.tla) checks the full-id index the slots are
found through; [ServerConnection](README-ServerConnection.md) and
[ServerSession](README-ServerSession.md) take a free slot abstractly. This
model checks the free stack the assign pops and the release pushes, and the
generation tests that keep a stale owner from a reused slot.

## State

| Model | Code |
|---|---|
| `connected[i]`, `cid[i]` | `slots->connected[i]`, `slots->client_id[i]` (0 when free) |
| `gen[i]` | `slots->slot_generation[i]`: the owner generation the slot last held, 0 before its first assign |
| `stack[k]`, `top` | `slots->free_stack[k]`, `slots->free_stack_count`; the top entry is `free_stack[count - 1]` |
| `numConnected` | `slots->num_connected` |
| `nextGen` | the caller's next fresh owner generation (the encryption mapping's generation) |
| `handles` | ghost: every tenure's `rocketnet_client_slot_handle_t` (client id, index, generation), kept for release at any later time |
| `tenure[i]` | ghost: the tenure the slot holds |

The full-id index (open addressing with backward-shift deletion) is
abstracted to a lookup by client id over the connected slots;
`RNOpenAddress.tla` checks it. The header's contract for `owner_generation`:
it names one ownership instance and differs from the generation the reused
slot last held. The environment offers each assign a fresh generation or the
top slot's last one, which the code must refuse.

## Actions and the code they mirror

Functions in `sdk/rocketnet_client_slots.cpp`.

- **`Init`**: `rocketnet_client_slots_create` fills `free_stack` highest
  index first, so the first assigns pop 0, 1, 2.
- **`AssignFresh(id)`**, **`AssignStale(id)`**:
  `rocketnet_client_slots_assign` with a fresh generation or the top slot's
  last one. The caller has checked the id is not connected. The top entry
  `free_stack[count - 1]` is read; a slot whose `slot_generation` equals the
  owner generation is refused before any state changes; else
  `--free_stack_count`, the slot marked connected under the id and the
  generation.
- **`ReleaseHandle(h)`**: `rocketnet_client_slots_release_exact` through
  `rocketnet_client_slots_release_handle`: released only when the slot is
  connected under the handle's client id and generation; the slot cleared
  and its index pushed at `free_stack[free_stack_count++]`.
- **`Release(i)`**: `rocketnet_client_slots_release`: a slot not connected is
  refused before anything is touched, then `release_exact` of the slot's
  current identity.

## Invariants

- **`StackExact`**: the stack holds each free slot exactly once and nothing
  else, no push runs past the array's end, and `free_stack_count` plus
  `num_connected` is `max_clients`.
- **`OneClientPerSlot`**: no assign takes a slot a client holds.
- **`HandleReleasesOwnTenure`**: a handle releases only the tenure it was
  taken from, never the slot's next one.
- **`TenureGenerationsDistinct`**: a slot never holds the same owner
  generation twice.
- **`ConnectedGenerationPositive`**: a connected slot's generation is not 0.
- **`ConnectedCount`**: `num_connected` is the number of connected slots.

## The instance

`MCRNClientSlotsLayout.cfg`: three slots, two client ids (so an id can return
to the slot it left, the LIFO pop handing it straight back), five fresh
generations, at most ten tenures. The configuration runs without deadlock
checking (`ignore-terminal`): once the fresh generations are spent and every
slot is free, the only assign left is the stale one the code refuses.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNClientSlotsLayoutBrokenDoublePush.cfg`** (`ReleaseGuard = FALSE`):
   `rocketnet_client_slots_release` without its `!slots->connected[...]`
   refusal and `release_exact` without its exactness test. Violates
   `OneClientPerSlot`: slot 0 is assigned (the stack is `[2, 1]`), slot 1,
   never assigned, is released and pushed a second time (`[2, 1, 1]`), slot 0
   is released with the stack full (in the C a write one past
   `free_stack`), then two assigns both pop index 1, the second while the
   first client holds it.
2. **`MCRNClientSlotsLayoutBrokenPopKeepsCount.cfg`** (`PopDecrements =
   FALSE`): `rocketnet_client_slots_assign` without
   `--slots->free_stack_count`. Violates `OneClientPerSlot`: the first assign
   takes slot 0 and leaves it on the top; the second reads
   `free_stack[2] = 0` again and takes the slot client 1 holds.
3. **`MCRNClientSlotsLayoutBrokenNoReleaseGenCheck.cfg`** (`ReleaseGenCheck =
   FALSE`): `rocketnet_client_slots_release_exact` without
   `slots->slot_generation[client_index] != generation`. Violates
   `HandleReleasesOwnTenure`: client 1 takes slot 0 under generation 1 and a
   handle is kept; the slot is released; client 1 reconnects under generation
   2 and the LIFO pop gives it slot 0 again; the old handle (client 1, slot 0,
   generation 1) passes the connected and client id tests and releases the
   new tenure.
4. **`MCRNClientSlotsLayoutBrokenNoAssignGenCheck.cfg`** (`AssignGenCheck =
   FALSE`): `rocketnet_client_slots_assign` without the
   `slot_generation[client_index] == owner_generation` refusal. Violates
   `TenureGenerationsDistinct`: slot 0 is taken under generation 1 and
   released, and an assign offering generation 1 again (stale ownership)
   takes it, so a handle of the first tenure would also name the second.

## Runs

The records are in `RUNS.tsv` (group `rnclientslotslayout`), made with the
pinned runner as `proofs/tla/README.md` describes.

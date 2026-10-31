# RNSendStoreLayout

The send store and the send descriptor pool of one connection region, at the
layout of `sdk/rocketnet_allocator.cpp` and with the code's own arithmetic.
[RNSendStore](README-RNSendStore.md) is the store's abstract contract and
[RNLazyPool](README-RNLazyPool.md) the descriptor pool's; this model carries
the fields those abstractions cannot see: the meta words, the watermarks, the
scan cursor and a free stack stored inside the descriptors it lists.

One store class is the whole store: the three classes share no state, and
every store allocation is one chunk (a message above one fragment takes a span,
which RNSendSpans.tla models).

## State

| Model | Code |
|---|---|
| `meta[i]`, `[live, gen]` | `cls->meta[i]`: `RN_META_LIVE` and `RN_META_GENERATION` |
| `fresh` | `cls->fresh`: chunks at or past it were never taken |
| `freeHead` | `cls->free_head`, the scan cursor |
| `freeCount` | `cls->free_count` |
| `desc[d]`, `[inUse, gen, slot, chunk]` | `send_descriptors[d]`: `in_use`, `generation`, `free_slot`, `chunk` |
| `dTop`, `dFree`, `dFresh` | `descriptors_free_top`, `descriptors_free`, `descriptors_fresh` |
| `exhausted` | `region->exhausted` |

The free stack lives in the descriptors' own `free_slot` fields: the entry at
stack depth `k` is `desc[k].slot`. A descriptor below the stack's top therefore
holds a live stack entry in its `free_slot` while it is itself taken or free.

The region's memory is not cleared. `Init` gives every meta word an arbitrary
live bit and every descriptor an adversarial value (in use, generation 0, slot
0, chunk 0), and `Carve` leaves both as the previous connection left them. The
ghost `touchedPast` records any read or write of a meta word at or past
`fresh`, or of a descriptor at or past `descriptors_fresh`; the code must do
neither. Generations run modulo `GenMod` (`2^31` for a chunk's tag, `2^32` for
a descriptor's), and the instance uses 3, so a wrap is two retakes away.

## Actions and the code they mirror

Line numbers are `sdk/rocketnet_allocator.cpp` at this change.

- **`Alloc`**: `rn_region_send_alloc` (794) with room. Its guard is
  `rn_region_send_fit_store` (732): not exhausted, a free descriptor, and
  `rn_send_store_find` (289), which refuses at `free_count == 0` (297) and
  otherwise scans from `free_head` up (300) with `rn_store_chunk_is_free`
  (280-282), `i >= fresh` first. The take is `rn_send_store_take` (316): the
  next generation, read only below `fresh` and skipping 0 (331), `fresh` moved
  past a first take (336) and the cursor to `index + 1` (341). Then
  `rn_region_descriptor_take` (674): the pop of `free_slot` at depth
  `top - 1` or the fresh descriptor, the generation (703), `keep_free_slot`
  (704-705, 709).
- **`AllocSpan`**: the descriptor take of `rn_region_send_alloc_span` (1007).
- **`Refuse`**: `rn_region_send_fit_store` failing (no descriptor, or the
  store's find) and `rn_region_send_refuse` (772) exhausting the region.
- **`Free(d)`**: `rn_region_send_free` (1100): `rn_send_store_free_checked`
  (352) clears the live bit and lowers the cursor (374), then
  `rn_region_descriptor_release` (720) pushes `d` into
  `send_descriptors[top].free_slot` (727). `rn_region_send_free_fragment`
  (1127) ends in the same free on a send's last fragment.
- **`Carve`**: `rn_region_carve` (502), whose store part is
  `rn_send_store_init_counts` (259): both pools empty and both watermarks at
  zero, nothing else written.

## Invariants

- **`NeverTouchPastFresh`**: no meta word at or past `fresh`, and no
  descriptor at or past `descriptors_fresh`, is read or written.
- **`NoTakeWhileLive`**: no chunk and no descriptor is handed out while live.
- **`RefuseOnlyWhenNoneFree`**: the store refuses only when no chunk is free.
- **`FreeCountExact`**: `free_count` is the number of free chunks.
- **`CursorBelowFree`**: `free_head` is at or below `fresh` and at or below
  every free chunk, so the scan from it finds the lowest free chunk and never
  needs to wrap.
- **`LiveIsHeld`**: a chunk below `fresh` is live exactly when one live
  descriptor names it.
- **`StackExact`**: the stack holds exactly the released descriptors below
  the watermark, each once.
- **`DescCountExact`**: `descriptors_free` is the stack plus the never-taken
  descriptors, and `Q` less the live ones.
- **`HeldGenerationPositive`**: a live chunk's or descriptor's generation is
  never 0.

`CursorBelowFree` is why `rn_send_store_find` has no wrapping second pass: a
take moves the cursor just past the lowest free chunk and a free lowers it to
the freed chunk, so no free chunk is ever below it.

## The instance

`MCRNSendStoreLayout.cfg`: three chunks, four descriptors (one more than the
chunks, so the store's refusal with a free descriptor and the pool's refusal
are both reachable; spans take descriptors without chunks), `GenMod = 3`.

The control uses `VIEW View`. A meta word at or past `fresh`, a descriptor at
or past `descriptors_fresh`, and a `free_slot` at or above the stack's top are
dead: every action writes them before it reads them. The view leaves them out
of the fingerprint, so states that differ only in dead values are one state.
A read of a dead value before its write is exactly what `touchedPast`
records, and the ghosts stay in the view. The same configuration without the
view also passes with every invariant (67,485,152 states generated,
17,565,560 distinct, depth 41, 106 s at 8 workers on hulk); with the view it
fits the 110-second record budget at two workers. The witnesses run without
the view.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNSendStoreLayoutBrokenNoFreshGuard.cfg`** (`FreshGuard = FALSE`):
   `rn_store_chunk_is_free` without `i >= cls->fresh ||` (282). Violates
   `NeverTouchPastFresh` at the first `Alloc`: with `fresh = 0`, the scan
   from `free_head = 0` reads `meta[0]`, which nothing has written since the
   carve (an arena's fresh memory, or the previous connection's meta). With a
   stale live bit set, the class would also refuse while empty.
2. **`MCRNSendStoreLayoutBrokenNoLowerCursor.cfg`** (`LowerCursor = FALSE`):
   `rn_send_store_free_checked` without `if ( i < cls->free_head )
   cls->free_head = i;` (373-374). Violates `RefuseOnlyWhenNoneFree`: three
   allocs take chunks 0, 1, 2 and leave `free_head = 3`; freeing chunk 0
   leaves it at 3; the next alloc has `free_count = 1` but the scan from 3
   (300) meets nothing, so `rn_region_send_fit_store` returns
   `RN_SEND_FIT_STORE_FULL` and the region is exhausted with chunk 0 free.
   This is the bug class a cursor scan without a wrap has; the code needs no
   wrap because the free lowers the cursor.
3. **`MCRNSendStoreLayoutBrokenChunkGenZero.cfg`** (`ChunkSkipZero = FALSE`):
   `rn_send_store_take` without `if ( generation == 0 ) generation = 1;`
   (331-332). Violates `HeldGenerationPositive`: chunk 0 is taken (1), freed,
   taken (2), freed and taken again, and `( meta + 1 ) & RN_META_GENERATION`
   is 0 at the wrap.
4. **`MCRNSendStoreLayoutBrokenDescGenZero.cfg`** (`DescSkipZero = FALSE`):
   the descriptor generation as `descriptor->generation + 1` without the
   `== UINT32_MAX ? 1` test (703), the bug commit 5b214176 fixed. Violates
   `HeldGenerationPositive`: descriptor 0 is taken three times off the stack
   and its generation goes 1, 2, 0. A resend link stamped 0 is never unlinked
   by the ack while its descriptor is in use.
5. **`MCRNSendStoreLayoutBrokenNoKeepFreeSlot.cfg`** (`KeepFreeSlot = FALSE`):
   `rn_region_descriptor_take` without the `keep_free_slot` restore (709-710).
   Violates `NoTakeWhileLive`: descriptors 0 and 1 are taken; freeing 1 writes
   `send_descriptors[0].free_slot = 1` (depth 0), freeing 0 writes
   `send_descriptors[1].free_slot = 0` (depth 1). A take pops depth 1 and gets
   descriptor 0; the new top is 1 and 0 < 1, so descriptor 0's `free_slot` is
   the live entry at depth 0. The `memset` clears it to 0; the next take pops
   depth 0 and gets descriptor 0 again, while it is in use.

## Runs

The records are in `RUNS.tsv` (group `rnsendstorelayout`), made with the
pinned runner as `proofs/tla/README.md` describes.

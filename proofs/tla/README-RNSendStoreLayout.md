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
0, chunk 0), and `Carve` leaves these modeled fields as the previous
connection left them. The implementation separately clears `enqueue_ticket`
and `schedule_flags` below the old descriptor watermark; these scheduling
fields are outside this abstraction. The ghost `touchedPast` records any
read or write of a meta word at or past
`fresh`, or of a descriptor at or past `descriptors_fresh`; the code must do
neither. Generations run modulo `GenMod` (`2^31` for a chunk's tag, `2^32` for
a descriptor's), and the instance uses 3, so a wrap is two retakes away.

## Actions and the code they mirror

Functions in `sdk/rocketnet_allocator.cpp`.

- **`Alloc`**: `rn_region_send_alloc` with room. Its guard is
  `rn_region_send_fit_store`: not exhausted, a free descriptor, and
  `rn_send_store_find`, which refuses at `free_count == 0` and otherwise
  scans from `free_head` up with `rn_store_chunk_is_free`, `i >= fresh`
  first. The take is `rn_send_store_take`: the next generation, read only
  below `fresh` and skipping 0, `fresh` moved past a first take and the
  cursor to `index + 1`. Then `rn_region_descriptor_take`: the pop of
  `free_slot` at depth `top - 1` or the fresh descriptor, the generation,
  `keep_free_slot`.
- **`AllocSpan`**: the descriptor take of `rn_region_send_alloc_span`.
- **`Refuse`**: `rn_region_send_fit_store` failing (no descriptor, or the
  store's find) and `rn_region_send_refuse`, which counts the refusal and
  leaves the region as it was (revision 1.1 below).
- **`Free(d)`**: `rn_region_send_free`: `rn_send_store_free_checked`
  clears the live bit and lowers the cursor, then
  `rn_region_descriptor_release` pushes `d` into
  `send_descriptors[top].free_slot`. `rn_region_send_free_fragment`
  ends in the same free on a send's last fragment.
- **`Carve`**: `rn_region_carve`, whose store part is
  `rn_send_store_init_counts`: both pools empty and both watermarks at
  zero, with the modeled meta/descriptor fields unchanged. Scheduling-field
  cleanup below the old watermark occurs before it is lowered in the SDK.

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
   `rn_store_chunk_is_free` without `i >= cls->fresh ||`. Violates
   `NeverTouchPastFresh` at the first `Alloc`: with `fresh = 0`, the scan
   from `free_head = 0` reads `meta[0]`, which nothing has written since the
   carve (an arena's fresh memory, or the previous connection's meta). With a
   stale live bit set, the class would also refuse while empty.
2. **`MCRNSendStoreLayoutBrokenNoLowerCursor.cfg`** (`LowerCursor = FALSE`):
   `rn_send_store_free_checked` without `if ( i < cls->free_head )
   cls->free_head = i;`. Violates `RefuseOnlyWhenNoneFree`: three
   allocs take chunks 0, 1, 2 and leave `free_head = 3`; freeing chunk 0
   leaves it at 3; the next alloc has `free_count = 1` but the scan from 3
   meets nothing, so `rn_region_send_fit_store` returns
   `RN_SEND_FIT_STORE_FULL` and the send is refused with chunk 0 free.
   This is the bug class a cursor scan without a wrap has; the code needs no
   wrap because the free lowers the cursor.
3. **`MCRNSendStoreLayoutBrokenChunkGenZero.cfg`** (`ChunkSkipZero = FALSE`):
   `rn_send_store_take` without `if ( generation == 0 ) generation = 1;`.
   Violates `HeldGenerationPositive`: chunk 0 is taken (1), freed,
   taken (2), freed and taken again, and `( meta + 1 ) & RN_META_GENERATION`
   is 0 at the wrap.
4. **`MCRNSendStoreLayoutBrokenDescGenZero.cfg`** (`DescSkipZero = FALSE`):
   the descriptor generation as `descriptor->generation + 1` without the
   `== UINT32_MAX ? 1` test, the descriptor generation's skip-zero. Violates
   `HeldGenerationPositive`: descriptor 0 is taken three times off the stack
   and its generation goes 1, 2, 0. A resend link stamped 0 is never unlinked
   by the ack while its descriptor is in use.
5. **`MCRNSendStoreLayoutBrokenNoKeepFreeSlot.cfg`** (`KeepFreeSlot = FALSE`):
   `rn_region_descriptor_take` without the `keep_free_slot` restore.
   Violates `NoTakeWhileLive`: descriptors 0 and 1 are taken; freeing 1 writes
   `send_descriptors[0].free_slot = 1` (depth 0), freeing 0 writes
   `send_descriptors[1].free_slot = 0` (depth 1). A take pops depth 1 and gets
   descriptor 0; the new top is 1 and 0 < 1, so descriptor 0's `free_slot` is
   the live entry at depth 0. The `memset` clears it to 0; the next take pops
   depth 0 and gets descriptor 0 again, while it is in use.

## Runs

The records are in `RUNS.tsv` (group `rnsendstorelayout`), made with the
pinned runner as `proofs/tla/README.md` describes.

## Revision 1.1: a refused fit leaves the store and the region as they were

Standard rule: MESSAGES.md m1.7. `Refuse` (no free descriptor, or no chunk
run that fits) no longer sets the sticky `exhausted`: the send is refused and
counted, and the cursors, the free list, the descriptors and the region are as
before the refused fit. `RefuseExhausts = TRUE` is the reversed witness,
v1.0's `rn_region_exhaust` on a failed fit. The same change is made in
`RNSendSpans.tla`, the span ring this store's large sends use: `Exhaust` is
renamed `Refuse(l)` and leaves the ring unchanged.

| Module | Property | Kind | Claim |
|---|---|---|---|
| RNSendStoreLayout | `StoreNeverExhausts` | invariant | the store never exhausts its region (only the receive side does) |
| RNSendStoreLayout | `RefuseLeavesLayout` | action | a refusal changes no cursor, free list, descriptor or `exhausted` |
| RNSendSpans | `SpansNeverExhaust` | invariant | the span ring never exhausts its region |
| RNSendSpans | `RefuseLeavesRing` | action | a refusal leaves the ring, its links and the region unchanged |

| Case | Expected | Exit | Distinct |
|---|---|---:|---:|
| `MCRNSendStoreLayout.cfg` | pass | 0 | 118621 |
| `MCRNSendStoreLayoutBrokenRefuseExhausts.cfg` | `RefuseLeavesLayout` | 13 | 31 |
| `MCRNSendStoreLayoutBrokenRefuseSticky.cfg` | `StoreNeverExhausts` | 12 | 31 |
| `MCRNSendSpans.cfg` | pass | 0 | 1327995 |
| `MCRNSendSpansRefuse.cfg` (refusals reachable, both properties) | pass | 0 | 735 |
| `MCRNSendSpansBrokenRefuseExhausts.cfg` | `SpansNeverExhaust` | 12 | 27 |
| `MCRNSendSpansBrokenRefuseChanges.cfg` | `RefuseLeavesRing` | 13 | 27 |

Witness traces, read by hand. RNSendStoreLayout (both, 5 states): three
`Alloc` steps take the three free chunks (`freeCount` 3 to 0, `dFree` 4 to
1), and the fourth send's `Refuse` sets `exhausted = TRUE`. RNSendSpans (both,
3 states): one `Alloc` takes the whole 16-byte ring and the next send's
`Refuse` sets `exhausted = TRUE`. The code before revision 1.1 did both:
`rn_send_store` and the span allocator in `sdk/rocketnet_allocator.cpp`
called `rn_region_exhaust` on a failed fit. The code now calls
`rn_region_send_refuse`, which counts the refusal and leaves the region.

## Revision 1.1.2: the refused start in the code's order, in both modules

Revision 1.1's properties read the definitions back: `Refuse` was `UNCHANGED`
the layout (or the ring) and `exhausted`, so `RefuseLeavesLayout`,
`StoreNeverExhausts`, `RefuseLeavesRing` and `SpansNeverExhaust` held of any
code (probes `[][Refuse => UNCHANGED vars]_vars` passed on
`MCRNSendStoreLayout.cfg`, 118,621 distinct, and `MCRNSendSpansRefuse.cfg`,
735 distinct).

- **RNSendStoreLayout.** `Refuse(kind, spanRoom)` is a store or span start that
  the fit refuses (`FitOK`: not exhausted, a free descriptor, and the store's
  find or the span's room), written as the code's steps in order:
  `rn_region_send_fit_store` (`sdk/rocketnet_allocator.cpp`, a const
  region: exhausted, then the descriptors, then `rn_send_store_find` at 345)
  and `rn_region_send_refuse` (871), which counts. The counter `refused`
  (mod `CountMod`) and the ghost `calls` (every application start, so a
  refused start is never a stutter) are the variables a refusal may change.
  `Exhaust` is the receive side's `rn_region_exhaust` (709), so `exhausted`
  is reachable and the properties read it.
- **RNSendSpans.** `Refuse(l)` is a span start of `l` bytes that
  `rn_region_send_fit_span` (1075) refuses, then `rn_region_send_refuse`;
  `refused` and `calls` as above. `Ring` is the ring with its links, the live
  spans' starts and lengths, and `exhausted`.
- A refused start is named by the fit's decision on the state before the
  step (`StartStep /\ ~FitOK`), never by the action, so a start that takes
  before it fits is caught. `RefuseLeavesLayout` and `RefuseLeavesRing`: such
  a start leaves `Layout` (`Ring`) as it was and moves `refused` by one.
  `StoreNeverExhausts` is now an action property: no start changes
  `exhausted`. `SpansNeverExhaust` stays the invariant `~exhausted` (the span
  module has no receive side).
- Reversed witnesses, each a start order the code could have had:
  `StartOrder = "descfirst"` takes a descriptor (`rn_region_descriptor_take`,
  760) and, refused, releases it (809); `"chunkfirst"` takes the chunk
  (`rn_send_store_take`, 376) and frees it (`rn_send_store_free_checked`,
  414); `CountQueueFull = FALSE` leaves a full descriptor table uncounted;
  RNSendSpans' `LinkFirst = TRUE` links the span (`rn_span_link`, 999) and
  unlinks it (`rn_span_unlink`, 1025).

| Property | Fails when | Witness |
|---|---|---|
| `RefuseLeavesLayout` | a refused start takes a descriptor first and releases it; takes a chunk first and frees it; leaves a full descriptor table uncounted; or exhausts the region | `...BrokenDescFirst`, `...BrokenChunkFirst`, `...BrokenUncountedQueueFull`, `...BrokenRefuseExhausts` |
| `StoreNeverExhausts` | a refused start sets `exhausted` | `MCRNSendStoreLayoutBrokenRefuseSticky.cfg` |
| `RefuseLeavesRing` | a refused span start links and unlinks its span (spending `span_tail_undo`), or changes the ring | `MCRNSendSpansBrokenLinkFirst.cfg`, `MCRNSendSpansBrokenRefuseChanges.cfg` |
| `SpansNeverExhaust` | a refused span start sets `exhausted` | `MCRNSendSpansBrokenRefuseExhausts.cfg` |

| Case | Expected | Exit | Distinct | Seconds (2 workers, vision) |
|---|---|---:|---:|---:|
| `MCRNSendStoreLayout.cfg` (`CountMod = 1`; now with `Exhaust`) | pass | 0 | 237242 | 36.7 |
| `MCRNSendStoreLayoutRefuse.cfg` (N = 2, Q = 3, `CountMod = 3`) | pass | 0 | 58410 | 9.2 |
| `MCRNSendStoreLayoutBrokenRefuseExhausts.cfg` | `RefuseLeavesLayout` | 13 | 4 | 0.9 |
| `MCRNSendStoreLayoutBrokenRefuseSticky.cfg` | `StoreNeverExhausts` | 13 | 4 | 0.9 |
| `MCRNSendStoreLayoutBrokenDescFirst.cfg` | `RefuseLeavesLayout` | 13 | 4 | 0.9 |
| `MCRNSendStoreLayoutBrokenChunkFirst.cfg` | `RefuseLeavesLayout` | 13 | 151 | 1.0 |
| `MCRNSendStoreLayoutBrokenUncountedQueueFull.cfg` | `RefuseLeavesLayout` | 13 | 60 | 1.0 |
| `MCRNSendSpans.cfg` (`CountMod = 1`) | pass | 0 | 1327995 | 59.8 |
| `MCRNSendSpansRefuse.cfg` (`CountMod = 3`) | pass | 0 | 6615 | 1.8 |
| `MCRNSendSpansBrokenRefuseExhausts.cfg` | `SpansNeverExhaust` | 12 | 27 | 0.9 |
| `MCRNSendSpansBrokenRefuseChanges.cfg` | `RefuseLeavesRing` | 13 | 27 | 0.9 |
| `MCRNSendSpansBrokenLinkFirst.cfg` | `RefuseLeavesRing` | 13 | 10 | 0.9 |

Witness traces, checked by hand against the code:

- DescFirst (2 states): a span start at the initial state with no span room
  takes descriptor 0 fresh and releases it: `dFresh` 0 to 1, `dTop` 0 to 1,
  descriptor 0 left at generation 1, not in use. In the code the fit
  (`rn_region_send_alloc_span`, 1139) reads a const region and the refusal
  returns at 1142; the descriptor is taken only at 1148.
- ChunkFirst: two `Alloc` and an `AllocSpan` use the descriptors; a store
  start with no descriptor free takes chunk 2 and frees it, which bumps the
  chunk's generation and raises the fresh cursor. In the code
  `rn_region_send_alloc` fits at 901, refuses at 904, and takes the chunk only
  at 908.
- UncountedQueueFull: a start refused for a full descriptor table leaves
  `refused` where it was. In the code `rn_region_send_refuse` counts
  `RN_SEND_FIT_QUEUE_FULL` (871 to 886).
- LinkFirst (3 states; rewritten in the models fix round of 2026-11-13, the
  unlink now the code's with its tail undo, `UndoTail = TRUE` as in the code,
  so `LinkFirst` is the one rule changed): `Alloc(32)` leaves the tail at 32
  and `span_tail_undo` at 0; `Refuse(96)` links at 0 (the link keeps 32 in
  `span_tail_undo` and moves the tail), finds no room and unlinks the newest
  span, which gives the tail back from `span_tail_undo` and spends it. The
  tail is 32 again, but `span_tail_undo` is spent where it was 0, so an
  abort of the first span could no longer give its bytes back. In the code
  `rn_span_link` writes `span_tail_undo = span_tail` (1033) and
  `rn_span_unlink` of the newest span restores the tail and stores the
  sentinel (1083 to 1090); the span is linked only after the fit and the
  descriptor take in `rn_region_send_alloc_span`, so no refused start
  touches either.

## RNSendSpans: the carve keeps dead descriptor bytes (models fix round, 2026-11-13)

- **`Reset`** is `rn_region_carve`'s span part (`sdk/rocketnet_allocator.cpp`
  594 to 703, the span roots at 666 to 669): the roots and `exhausted`
  only. A descriptor keeps its `span_start`, length and links as dead
  bytes; the carve's `descriptors_fresh = 0` (650) forgets every descriptor,
  and a later take (`rn_region_descriptor_take`) and link write every span
  field before anything reads it. The old `Reset` zeroed every descriptor,
  which the code never does, and `ListInAllocationOrder` checked that a dead
  descriptor's links were NONE; that conjunct is gone, since nothing reads
  them.
- **`LinkClearsNext`** (`TRUE` in the code): `rn_span_link` writes
  `span_next = RN_DESCRIPTOR_NONE` (1019). With the dead links kept, that
  write is what keeps the list right after a carve.
  `MCRNSendSpansBrokenLinkKeepsNext.cfg` turns it off and fails
  `ListInAllocationOrder` (5 states): spans 1 and 2 are linked
  (`span_next` of 1 is 2), the carve leaves that link, and span 1 linked
  again is the newest span with a next.
- **`View`**: `MCRNSendSpans.cfg` fingerprints states without the dead
  fields (no action and no property reads them while `LinkClearsNext`
  holds); the witnesses and the other passing configurations run without it.

| Case | Expected | Exit | Distinct | Seconds (2 workers, aws-mut-3) |
|---|---|---:|---:|---:|
| `MCRNSendSpans.cfg` (`VIEW View`) | pass | 0 | 25771 | 2.1 |
| `MCRNSendSpansRefuse.cfg` | pass | 0 | 81901 | 4.8 |
| `MCRNSendSpansAbort.cfg` | pass | 0 | 9101 | 1.2 |
| `MCRNSendSpansBrokenLinkFirst.cfg` | `RefuseLeavesRing` | 13 | 10 | 0.7 |
| `MCRNSendSpansBrokenLinkKeepsNext.cfg` | `ListInAllocationOrder` | 12 | 84 | 0.7 |

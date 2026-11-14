# RNSendStoreLayout

The send store and the send descriptor pool of one connection region, at the
layout of `sdk/rocketnet_allocator.cpp` and with the code's own arithmetic.
[RNSendStore](README-RNSendStore.md) is the store's abstract contract and
[RNLazyPool](README-RNLazyPool.md) the descriptor pool's; this model carries
the fields those abstractions cannot see: the meta words, the watermarks, the
store's free stack of chunk indices and a free stack stored inside the
descriptors it lists.

One store class is the whole store: the three classes share no state, and
every store allocation is one chunk (a message above one fragment takes a span,
which RNSendSpans.tla models).

## State

| Model | Code |
|---|---|
| `meta[i]`, `[live, gen]` | `cls->meta[i]`: `RN_META_LIVE` and `RN_META_GENERATION` |
| `fresh` | `cls->fresh`: chunks at or past it were never taken |
| `cStack`, `cTop` | `cls->free_stack` and `cls->free_top`: entries `[0, free_top)` are the free chunks below `fresh` |
| `freeHead` | under `ScanFind` only (the reversed witness): the scan cursor `free_head` the code had before finding W2-86 |
| `visits` | ghost: the entries the last choice of a chunk examined |
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
  returns the top of the free stack, or `fresh` when the stack is empty.
  The take is `rn_send_store_take`: the pop (or `fresh` moved past a first
  take) and the next generation, read only below `fresh` and skipping 0.
  Then `rn_region_descriptor_take`: the pop of
  `free_slot` at depth `top - 1` or the fresh descriptor, the generation,
  `keep_free_slot`.
- **`AllocSpan`**: the descriptor take of `rn_region_send_alloc_span`.
- **`Refuse`**: `rn_region_send_fit_store` failing (no descriptor, or the
  store's find) and `rn_region_send_refuse` exhausting the region.
- **`Free(d)`**: `rn_region_send_free`: `rn_send_store_free_checked`
  clears the live bit and pushes the chunk on the free stack, then
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
- **`FreeStackExact`**: the free stack holds exactly the free chunks below
  `fresh`, each once, so the find needs nothing but its top and `fresh`.
- **`ChoiceBounded`**: each choice of a chunk (an allocation, or the room
  query `rn_region_send_fit_store` behind `rocketnet_*_can_send_message`)
  examines at most one entry, however many chunks are held. The receiver
  chooses which sends it acknowledges, so it chooses which chunks are freed;
  a choice that walked held chunks was its to steer (finding W2-86).
- **`LiveIsHeld`**: a chunk below `fresh` is live exactly when one live
  descriptor names it.
- **`StackExact`**: the stack holds exactly the released descriptors below
  the watermark, each once.
- **`DescCountExact`**: `descriptors_free` is the stack plus the never-taken
  descriptors, and `Q` less the live ones.
- **`HeldGenerationPositive`**: a live chunk's or descriptor's generation is
  never 0.

`CursorBelowFree` (still defined, for `ScanFind`) was the old scan's reason
to need no wrapping second pass; the stack needs no scan at all.

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
   `rn_send_store_take` without its `if ( index < cls->fresh )` around the
   generation read. Violates `NeverTouchPastFresh` at the first `Alloc`: with
   `fresh = 0` the take of chunk 0 reads `meta[0]`, which nothing has written
   since the carve (an arena's fresh memory, or the previous connection's
   meta), and would tag the chunk with that garbage plus one.
2. **`MCRNSendStoreLayoutBrokenNoPush.cfg`** (`PushOnFree = FALSE`):
   `rn_send_store_free_checked` without `cls->free_stack[cls->free_top++] =
   i;`. Violates `RefuseOnlyWhenNoneFree`: three allocs take chunks 0, 1, 2
   (`fresh = 3`); freeing chunk 0 leaves the stack empty, so the next find has
   `free_count = 1`, no stack entry and no fresh chunk, and
   `rn_region_send_fit_store` refuses with chunk 0 free.
2a. **`MCRNSendStoreLayoutBrokenScan.cfg`** (`ScanFind = TRUE`): the scan
   from the cursor the code had before finding W2-86 (`rn_send_store_find`
   looping from `free_head` with `rn_store_chunk_is_free`, the take setting
   `free_head = index + 1`, the free lowering it). Violates `ChoiceBounded`
   in six states: chunks 0 and 1 are taken, chunk 0 is freed (the cursor
   lowered to 0), chunk 0 is taken again (one visit, the cursor at 1), and
   the next allocation examines held chunk 1 and then fresh chunk 2: two
   visits. At the default config the same walk is the whole held run. The
   unit test tests/test_unit_connection_memory.cpp
   test_store_choice_never_walks_a_held_run holds the whole small class
   without descriptors and measured 30,122 entries before the fix; through
   an endpoint the run is bounded by the descriptors, 20,554 (Q less the two
   starts).
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
| `RefuseLeavesRing` | a refused span start links and unlinks its span, or changes the ring | `MCRNSendSpansBrokenLinkFirst.cfg`, `MCRNSendSpansBrokenRefuseChanges.cfg` |
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
- LinkFirst (3 states): `Alloc(32)` leaves the tail at 32; `Refuse(96)` links
  at 0 and unlinks, and the tail is left at 0, so a non-empty ring has
  `tail = head` and reads as full. `rn_span_unlink` restores the floor only
  when the ring empties (1062 to 1066); unlinking the newest span leaves the
  tail where the link put it. In the code the span is linked only at 1155,
  after the fit at 1139 and the descriptor at 1148.

## Revision 1.1.x: the free stack (finding W2-86)

The chunk choice was a scan from the lowest free index (`freeHead`), and the
receiver steers which chunks are free through its selective acks, so one
choice could walk a held run of up to Q chunks (20,554 at the default
configuration: about 20 us an update for one hostile receiver, and the room query
paid the same scan again). The code now keeps a LIFO stack of freed indices
per class (`free_stack`, `free_top`) beside the meta words; a take pops the
top or takes the fresh chunk, a free pushes. The model carries the stack
(`cStack`, `cTop`) and a ghost `visits`, the entries the last choice examined.
`FreeStackExact` says the stack holds exactly the free chunks below `fresh`,
once each; `ChoiceBounded` says a choice examines at most one entry.
`ScanFind = TRUE` restores the old scan and is the reversed witness for
`ChoiceBounded`; `PushOnFree = FALSE` (renamed from `BrokenNoLowerCursor`)
drops the push.

Runs with the pinned checker (tla2tools v1.7.4), 2 workers, bench-xdp-9:

| Case | Expected | Exit | Distinct | Seconds |
|---|---|---:|---:|---:|
| `MCRNSendStoreLayout.cfg` | pass | 0 | 681900 | 48.1 |
| `MCRNSendStoreLayoutRefuse.cfg` | pass | 0 | 118188 | 8.7 |
| `MCRNSendStoreLayoutBrokenScan.cfg` | `ChoiceBounded` | 12 | 891 | 0.8 |
| `MCRNSendStoreLayoutBrokenNoPush.cfg` | `RefuseOnlyWhenNoneFree` | 12 | 707 | 0.7 |
| `MCRNSendStoreLayoutBrokenNoFreshGuard.cfg` | `NeverTouchPastFresh` | 12 | 9 | 0.7 |
| `MCRNSendStoreLayoutBrokenNoKeepFreeSlot.cfg` | as before | 12 | 2503 | 0.8 |
| `MCRNSendStoreLayoutBrokenChunkGenZero.cfg` | as before | 12 | 939 | 0.8 |
| `MCRNSendStoreLayoutBrokenDescGenZero.cfg` | as before | 12 | 939 | 0.7 |
| `MCRNSendStoreLayoutBrokenDescFirst.cfg` | `RefuseLeavesLayout` | 13 | 4 | 0.7 |
| `MCRNSendStoreLayoutBrokenChunkFirst.cfg` | `RefuseLeavesLayout` | 13 | 154 | 0.7 |
| `MCRNSendStoreLayoutBrokenRefuseExhausts.cfg` | `RefuseLeavesLayout` | 13 | 4 | 0.7 |
| `MCRNSendStoreLayoutBrokenRefuseSticky.cfg` | `StoreNeverExhausts` | 13 | 4 | 0.7 |
| `MCRNSendStoreLayoutBrokenUncountedQueueFull.cfg` | `RefuseLeavesLayout` | 13 | 60 | 0.7 |

Witness traces, checked by hand against the code:

- Scan (6 states): take 0, take 1, free 0, take 0 (one visit); the next
  choice starts at the lower cursor and walks the held chunk 1 before it
  reaches fresh chunk 2: two visits. In the code before the fix,
  `rn_send_store_find` looped from `free_head` over live meta words; with the
  stack the same choice takes `fresh` and examines one entry.
- NoPush: three allocs take chunks 0, 1, 2 (`fresh = 3`); freeing chunk 0
  leaves `cTop = 0`, so `rn_send_store_find` sees `free_count = 1`, an empty
  stack and no fresh chunk, and the fit refuses with chunk 0 free.

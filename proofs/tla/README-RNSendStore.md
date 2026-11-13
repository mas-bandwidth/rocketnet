# RNSendStore

One class of the message layer's send store, abstractly: docs/CHANNELS.md
sections 4 and 15, implemented by `rn_send_store_find`, `rn_send_store_take`
and `rn_send_store_free_checked` in `sdk/rocketnet_allocator.cpp`. The three
classes share no state, so one class is the whole model.
[RNSendStoreLayout](README-RNSendStoreLayout.md) is the same store with the
code's arithmetic (the meta array, the lazy watermark, the scan cursor) and the
descriptor free stack beside it.

- **A send** of up to one fragment holds one chunk; a larger send takes a span
  and no chunk.
- **Alloc** takes a free chunk and tags it with its next generation:
  `rn_send_store_take` writes `( meta + 1 ) & RN_META_GENERATION`, and 1 in
  place of 0 (a chunk never taken gets 1). The model runs the tag modulo
  `GenMod` (2^31 in the code) so a wrap is reached.
- **Free** returns the chunk. An allocated chunk's meta carries `LIVE`, and
  free refuses a handle whose chunk does not.

## Invariants

- **`NoDoubleAllocate`**: no chunk is both free and held, or held by two
  senders.
- **`NoLeak`**: `free_count` equals the free chunks, and free plus held is the
  class.
- **`HeldGenerationPositive`**: a held chunk's generation is never 0. The
  handle `rn_send_store_take` returns is `( class << 30 ) | index` and carries
  no generation, so nothing in this layer compares a tag: the free checks the
  live bit alone. The invariant is the tag's own rule (0 is never a live
  name), checked here across a wrap and in
  [RNSendStoreLayout](README-RNSendStoreLayout.md) with the code's watermark.
- **`TypeOK`**: every variable has its declared type.

## What the model leaves to the caller

A second free is modelled only while the chunk has not been reallocated. Once
it has, a free through the old handle looks exactly like the new owner's own
free, and no check inside the allocator can tell them apart. The layers above
own that case:

- At its free, the descriptor drops its handle (layer 1).
- The per-number table never names a freed descriptor (layer 3).

## The instance

| Constant | Value |
|---|---|
| `Chunks` | 4 |
| `Senders` | `{s1, s2, s3}` |
| `GuardDoubleFree` | `TRUE` in the control |
| `GenMod` | 3: a chunk's third take wraps |
| `SkipZero` | `TRUE` in the control |

## The reversed witnesses

**`MCRNSendStoreBrokenNoGuard.cfg`** (`GuardDoubleFree = FALSE`) violates
`NoLeak`. A sender takes a chunk, frees it, and frees it again. Against the
code: without the `( cls->meta[i] & RN_META_LIVE ) == 0` refusal in
`rn_send_store_free_checked`, the second free clears an already clear live
bit and runs `cls->free_count++` again, so `free_count` counts one chunk more
than the class has free and no longer matches the live bits. With the guard the
second free returns false and changes nothing;
`test_channels_allocator_send_store` pins that (a refused double free leaves
the class's free count unchanged).

**`MCRNSendStoreBrokenGenZero.cfg`** (`SkipZero = FALSE`) violates
`HeldGenerationPositive`. One chunk is taken (generation 1), freed, taken (2),
freed and taken again: `( 2 + 1 ) mod 3` is 0, and the chunk is held under
generation 0. Against the code: without `if ( generation == 0 ) generation =
1;` in `rn_send_store_take`, `( cls->meta[index] + 1 ) & RN_META_GENERATION`
is 0 when the tag wraps, and the chunk is live under the name 0;
`test_store_chunk_generation_never_zero` pins the skip.

## Runs

The records are in `RUNS.tsv` (group `rnsendstore`), made with the pinned
runner as `proofs/tla/README.md` describes.

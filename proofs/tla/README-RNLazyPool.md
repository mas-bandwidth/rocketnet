# RNLazyPool

The lazily initialized pools of the message layer's region, in
`sdk/rocketnet_allocator.cpp`: the send descriptor pool
(`rn_region_descriptor_take`, `rn_region_send_free`,
`rn_region_send_free_fragment`, `rn_region_carve`). Each class of the send
store (`rn_send_store_find`, `rn_send_store_take`,
`rn_send_store_free_checked`, `rn_send_store_init_counts`) keeps the same
watermark discipline with a cursor scan instead of a stack; both pools are
modelled at the code's layout in [RNSendStoreLayout](README-RNSendStoreLayout.md).

An eager pool threads its free list through every entry when it is reset, so
a reset writes every entry and touches every page of the pool. At the
example soak's configuration that was 3.1 MB of descriptors per connection
region, written at create and again at every connect and disconnect, most of
it never used. The lazy pool writes an entry only when it is taken.

- **The watermark**, `fresh`: entries `1..fresh` have been taken since the
  reset and are free-listed or held; entries above it were never taken, and
  nothing writes or reads them.
- **Take** pops the free list or, when it is empty, takes entry `fresh + 1`,
  and the taker writes the entry whole.
- **Release** marks the entry free and pushes it on the list.
- **Reset** empties the list and zeroes the watermark. It writes no entry: an
  entry keeps whatever bytes its last use left (`mem`), and those bytes are
  not state.

The model runs the eager pool beside the lazy one on the same actions, as the
reference (`eager`, the list an eager reset threads, `<<1, ..., N>>`).

## Invariants

- **`Refines`**: the lazy list followed by the untaken entries in order is the
  eager list, so both pools take the same entry at every step. The lazy pool
  is observably the eager one.
- **`Conservation`**: every entry taken since the reset is free-listed or
  held, exactly once, and no two owners hold one entry.
- **`InUseExact`**: an entry is in use exactly when it is held. The in-use
  query reads an entry's mark only below the watermark; above it the mark is a
  previous use's stale byte.
- **`UntouchedAboveFresh`**: no entry above the watermark is written since the
  reset.
- **`TakeOnlyRefusedWhenFull`**: a take is refused only when every entry is
  held.
- **`TypeOK`**.

## Cases

| config | constants | expected |
|---|---|---|
| `MCRNLazyPool.cfg` | N = 3, three owners, both rules on | pass |
| `MCRNLazyPoolBrokenNoGuard.cfg` | `GuardFresh = FALSE` | `InUseExact` violated |
| `MCRNLazyPoolBrokenKeepFresh.cfg` | `ResetFresh = FALSE` | `Refines` violated |

Both witnesses were checked by hand against the code:

- **No guard** (Take, Reset, then the query reads entry 1's stale mark): a
  connection takes descriptor 0, the slot is reset, and a check of `d` against
  `queue` instead of `descriptors_fresh` reads the old `in_use = 1`. The unit
  test `test_channels_allocator_no_live_allocation` scanned descriptors up to
  `queue` and failed in exactly this way until it scanned to
  `descriptors_fresh`.
- **Keep fresh** (Take, Reset keeping the watermark): the next take after the
  reset gets descriptor 1 where the eager pool gives 0, and descriptor 0 is
  unreachable: `descriptors_free` counts it, but no take can return it.

`test_reused_slot_starts_clean` compares a reused region with a fresh one by
the pools' state (free list, free count, watermark) and the bytes below the
watermark, and then checks the reused region takes descriptors in a fresh
region's order.

# RNReceivedBlocksLayout: the receive statistics blocks at the code's layout

`RNReceivedBlocksLayout.tla` models the receive side of the running statistics
in `sdk/rocketnet_ack.cpp`. The received ring has `N = B * Blocks` records
(2048 = 32 x 64); sequence `s` lives at slot `s % N`. Each fixed block of `B`
slots keeps a summary of its records whose sequence lies in
`[base, highest]`, `base = max(1, highest - N + 1)`.
`rocketnet_ack_endpoint_record_received` refuses a sequence a ring or more
behind, then `rocketnet_ack_stats_received_advance` clears the slots of the
sequences leaving the window, `[old_base % N, + count)` split at the ring end
(a move of `N` or more empties every block);
`rocketnet_ack_stats_received_clear_physical_range` empties each block wholly
inside and rebuilds the partial ones (`rocketnet_ack_stats_received_rebuild_block`).
The record is written and its block rebuilt. The published cohort is
`rocketnet_ack_stats_received_query` over the older half `[base, top]`: pieces
that end at a block boundary, a whole aligned block read from its summary, a
partial one record by record against its exact sequence.

A summary is the set of sequences it counts: the code's bytes, count and time
extrema are functions of those records and its merge is their union.
`RNServerStats.tla` checks the aggregates with the reduction taken whole.

## The instance

`MCRNReceivedBlocksLayout.tla`: four blocks of two slots (a ring of 8, an
older half of 4), sequences to 26 in any order.

## Invariants

| Invariant | Meaning |
|---|---|
| `SummariesExact` | each block's summary counts exactly its records in `[base, highest]` |
| `QueryIsReference` | the older-half query is the records of `[base, top]` read one by one |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNReceivedBlocksLayoutBrokenNoWrap.cfg` | the leaving range is not split at the ring end | `QueryIsReference` |
| `MCRNReceivedBlocksLayoutBrokenFirstBlock.cfg` | the first block of a clear rounded up, missing a partial first block | `QueryIsReference` |
| `MCRNReceivedBlocksLayoutBrokenEdgeBlock.cfg` | a block summary read for a piece that starts on a boundary but is short | `QueryIsReference` |

A witness with the last block of a clear one short cannot fail: the leaving
range always ends at the slot of the new highest, whose block the write
rebuilds next. Each counterexample above was checked against the C by hand:
no-wrap receives 8, 14, 21 and the summary of block 0 keeps 8 where the
code's second clear empties it; first-block receives 1 then 13 and block 0
keeps 1; edge-block receives 5 and the query of `[1, 4]` reads block 2's
summary, which holds 5.

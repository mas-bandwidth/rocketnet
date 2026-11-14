# RNReceivedBlocksLayout: the receive statistics at the code's layout

`RNReceivedBlocksLayout.tla` models the receive side of the running statistics
in `sdk/rocketnet_ack.cpp`. The received ring has `N = B * Blocks` records
(2048 = 32 x 64); sequence `s` lives at slot `s % N`. A generation is the `B`
sequences `[g * B, g * B + B - 1]`, all in physical block `g % Blocks`. Each
block keeps one generation's summary and its tag `g + 1` (0 after a reset).

`rocketnet_ack_endpoint_record_received` refuses a sequence a ring or more
behind, writes the record, then `rocketnet_ack_stats_received_summarize`: a
block tagged with a later generation is left alone (that generation's start
has left the window, so it is never read whole again), an earlier tag is
replaced by this generation holding only this record, a duplicate rebuilds
the generation from its records, and any other record joins the summary.

`rocketnet_ack_stats_received_refresh` publishes the older half `[base, top]`.
Inside one generation it reads the records one by one and caches nothing.
Otherwise the older half is three cached pieces: the left piece from suffixes
built over base's generation, the middle (whole generations between, read
through `rocketnet_ack_stats_received_generation`) and the right piece (top's
generation from its start to top). The pieces are reused only while the write
lands above the cached top. Top leaving its generation with base in place
moves the finished right piece and any passed generations into the middle;
base leaving its generation rebuilds the suffixes and the middle.

A summary or piece is the bag (multiset) of `<<sequence, bytes>>` records it
counts. The code's merge (`rocketnet_ack_stats_received_merge`) adds counts
and bytes, so the model's merge is bag addition: a record merged into a piece
twice is counted twice in the model as in the code, and `QueryIsReference`
sees it (a set union would hide it; `MCRNReceivedBlocksLayoutBrokenOverlap.cfg`
passes the set model of v1.1.54 and fails this one). The time extrema are
functions of the bag's records. A duplicate that is merged instead of rebuilt
shows as a second copy of its sequence. The merge order (the first record's
NaN) is checked by the unit tests against the scan oracle
(`tests/test_unit_ack_stats.cpp`). `RNServerStats.tla` checks the aggregates
with the reduction taken whole.

`Init` is `rocketnet_ack_endpoint_reset`: the block tags, the cached base and
top and the middle and right aggregates are zero, which is the empty
aggregate (the reset zeroes them, so the refresh's first load of a cached
piece reads a word a write has set; through v1.1.54 only the tags and the base
were zeroed, and the other loaded values were discarded unused, so that had no
behavioural effect). `Init` states the reset the code now does; it does not
check it. With the cached base 0 the model's refresh, like the code's, finds
the cache invalid and rebuilds top, middle and right without reading them, so
the model would pass the same way with those three `Unset`: a read of a word no
write has set is checked here only for the block summaries and suffixes, below,
and the unit test `test_ack_received_stats_reset_sets_every_cached_piece`
checks the reset by value. The block summaries
and the left suffixes are left as they were: the model starts them as `Unset`,
the bag of a record no receive writes, so a summary read before its tag is set
or a suffix read before it is built would reach the published cohort and fail
`QueryIsReference`.

## The instance

`MCRNReceivedBlocksLayout.tla`: four blocks of two slots (a ring of 8, an
older half of 4), sequences in any order. `MCRNReceivedBlocksLayout.cfg`
checks sequences to 22 with one payload; `MCRNReceivedBlocksLayoutPayloads.cfg`
checks sequences to 11 with payloads 1 and 2, so a duplicate replaces a record
with different bytes. The witnesses use sequences to 20 and both payloads.

## Invariants

| Invariant | Meaning |
|---|---|
| `SummariesExact` | a tagged summary counts exactly its generation's records while none of them has left the retained window |
| `GenerationsExact` | every generation wholly inside the older half reads as its records |
| `PiecesExact` | the cached left suffixes, middle and right piece are their ranges' records |
| `QueryIsReference` | the published cohort is the records of `[base, top]` read one by one |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNReceivedBlocksLayoutBrokenInside.cfg` | the pieces are reused when the write lands inside the cached `[base, top]` | `QueryIsReference` |
| `MCRNReceivedBlocksLayoutBrokenLater.cfg` | a write into a block tagged with a later generation retags it | `QueryIsReference` |
| `MCRNReceivedBlocksLayoutBrokenDuplicate.cfg` | a duplicate joins its summary instead of rebuilding it | `QueryIsReference` |
| `MCRNReceivedBlocksLayoutBrokenBaseMove.cfg` | the left piece and middle are reused after base leaves its generation | `QueryIsReference` |
| `MCRNReceivedBlocksLayoutBrokenOverlap.cfg` | the right piece is appended from the cached top, not the sequence after it (`next = cache_top`), so that record is counted twice | `QueryIsReference` (receive 2 then 3: the right piece holds 2 twice) |

The first four witnesses are also code mutants: the same rule turned off in
`sdk/rocketnet_ack.cpp` fails `test_ack_received_stats_random_receives_match_scan`,
and the inside and base-move mutants fail older received statistics cases too.
The overlap witness is the multiplicity check: it needs one payload only, since
the second copy changes the count and bytes, not the set of records.

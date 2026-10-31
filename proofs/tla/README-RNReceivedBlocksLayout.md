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

A summary or piece is the set of `<<sequence, bytes>>` records it counts: the
code's bytes, count and time extrema are functions of those records and its
merge is their union. Two payloads make a duplicate that is merged instead of
rebuilt visible as two records of one sequence. The merge order (the first
record's NaN) is checked by the unit tests against the scan oracle
(`tests/test_unit_ack_stats.cpp`). `RNServerStats.tla` checks the aggregates
with the reduction taken whole.

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

Each witness is also a code mutant: the same rule turned off in
`sdk/rocketnet_ack.cpp` fails `test_ack_received_stats_random_receives_match_scan`,
and the inside and base-move mutants fail older received statistics cases too.

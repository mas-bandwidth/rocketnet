# RNSourceSnapshotLayout

The connected-source snapshot (`sdk/rocketnet_source_snapshot.h`). Main
publishes the set of connected client addresses once per update. Each IOCP
receive worker looks every datagram's source up in it, so a known source may
use the whole receive bank and an unknown one only a capped share. The share,
the admission and the counting belong to the transport; this layer is the
snapshot alone.

There are two slots, each an open-addressing hash set with a linear probe that
wraps at the slot's end. There is one writer (main) and up to 32 readers. The
writer fills the slot that is not published and publishes it with one store
of the word `( generation << 1 ) | slot`. A reader loads the word, announces
its slot in the reader's own word, and re-checks that the word is still
published before it reads the slot. The writer's update scans every reader's
announcement first, and skips its publish when one is on its target.

## State

| Model | Code |
|---|---|
| `slots[s][i]` | `snapshot->cells[s * capacity + i]`: an address, or 0 for a cell of type `ROCKETNET_ADDRESS_NONE` |
| `pub` | `published->word`: `slot` is bit 0, `gen` the rest |
| `ann[r]` | `reader[r - 1].slot`: 0, 1 or `ROCKETNET_SOURCE_SNAPSHOT_NONE` (-1) |
| `wslot` | `writer->target`, the slot not published |
| `wpc`, `wi` | where the writer is: `"idle"`, `"scan"` with `wi` readers scanned (the loop of `rocketnet_source_snapshot_writer_begin`), `"add"` (`writer->filling`) |
| `skipped`, `refused` | the counts the caller keeps: begin returned NONE, add returned false |
| `rpc[r]`, `rl[r]` | where reader r is in `rocketnet_source_snapshot_reader_acquire`, and the word it loaded |
| `wset`, `pset`, `held[r]` | ghosts: the addresses added this update, the set the published word stands for, the set published at reader r's load |
| `look[r]` | reader r's last lookup and its answer |
| `Cap`, `MaxEntries`, `Home(a)` | `rocketnet_source_snapshot_capacity( max_entries )`, `max_entries`, `rocketnet_source_snapshot_home` |

## Actions

| Action | Code |
|---|---|
| `Scan` | `rocketnet_source_snapshot_writer_scan`, one reader per step; on the target the update ends (`writer_begin` returns NONE) |
| `Fill` | `rocketnet_source_snapshot_writer_fill`: the target cleared |
| `Add(a)`, `Refuse(a)` | `rocketnet_source_snapshot_writer_add`: the first empty cell on the probe path, or false when the target is full |
| `Publish` | `rocketnet_source_snapshot_writer_publish`: one store of the next word; the other slot becomes the target |
| `Load(r)`, `Announce(r)`, `Recheck(r)` | `rocketnet_source_snapshot_reader_load`, `_reader_announce`, `_reader_recheck`, the three steps of `_reader_acquire`; a failed re-check withdraws the announcement |
| `Lookup(r, a)` | `rocketnet_source_snapshot_contains` |
| `Release(r)` | `rocketnet_source_snapshot_reader_release` |

Each step is atomic in the model. The cells a step writes are read by no one
until a later step, which `NoTornRead` checks. The ordering the atomic steps
assume is argued in the header: announce, re-check, publish and scan are
sequentially consistent, so a scan that misses an announcement on the target
is followed by a re-check that sees a newer word. TLC does not check the C++
memory model.

## Invariants

- `TargetUnpublished`: the writer's target is never the published slot.
- `NoTornRead`: a reader never holds the slot the writer is writing.
- `WriterNeverWritesAnnounced`: while the writer writes a slot, every
  announcement on it is one whose re-check will refuse it (the slot is not
  published and the reader has not yet re-checked). A reader may announce
  the target after the writer's scan of it; that announcement never reads.
- `LookupMatchesPublished`: while a reader holds a slot, every lookup answers
  membership in the set that was published at the reader's load.
- `PublishedExact`: the published slot holds exactly the published set.
- `ChainIntact`: every stored address is on its probe path from home with no
  empty cell before it, and is stored once.
- `AtMostHalf`: a slot holds at most `MaxEntries` addresses.

## Instances

| Config | Expected |
|---|---|
| `MCRNSourceSnapshotLayout.cfg` | two readers, three addresses on four cells, three updates: pass |
| `MCRNSourceSnapshotLayoutOneReader.cfg` | one reader, four updates: pass |
| `MCRNSourceSnapshotLayoutBrokenHalfPublished.cfg` | the publish before the last add: `NoTornRead` fails |
| `MCRNSourceSnapshotLayoutBrokenNoRecheck.cfg` | announce with no re-check: `WriterNeverWritesAnnounced` fails |
| `MCRNSourceSnapshotLayoutBrokenLookupNoWrap.cfg` | the lookup stops at the slot's end: `LookupMatchesPublished` fails |
| `MCTraceRNSourceSnapshotLayoutSelf.cfg` | the trace check's hand-written good record: pass |
| `MCTraceRNSourceSnapshotLayoutSelfBrokenAddPublishes.cfg` | the same record with the add changing the published word: `TraceConsistent` fails |

`MCHome` homes addresses 1 and 2 on cell 3 and address 3 on cell 0. The
second add on cell 3 wraps to cell 0, and address 3 then probes past it.

## Trace check

Layer `source-snapshot`: `tests/tla_trace_source_snapshot.cpp` drives the
header's own step functions in a seeded random walk, with one writer and two
readers on a snapshot of two entries, and records every model variable after
each call. It picks three real addresses whose hashed homes are the same as
`MCHome`'s. The plant build defines `ROCKETNET_SOURCE_SNAPSHOT_PLANT`, which
makes `writer_add` store the next word before it inserts, so the target is
published half built. That trace fails `TraceConsistent` at the first add.

Unit tests: `tests/test_unit_source_snapshot.cpp`.

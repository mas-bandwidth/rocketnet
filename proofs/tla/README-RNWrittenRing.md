# RNWrittenRing

The ack endpoint's record rings in `sdk/rocketnet_ack.cpp`: the sent records
and the received records. Each ring's emptiness is a written bit per record
(`sent_written`, `received_written`),
cleared by `rocketnet_ack_endpoint_reset`, instead of zeroed records.

A reset used to zero the whole endpoint, 271 KB, at every client create,
connect and disconnect and at every server slot's create and reuse. It now
clears the bits, counters and rtt history's empty marks. It also initializes
64 generation tags and a cached base in reclaimed received-record summary
words (520 bytes), so reset does touch those parts of the ring's pages.
The written-bit abstraction below concerns the sequence/time/byte fields,
which reset leaves unchanged; it does not model those summary words.

- **Write** (`record_sent`, `record_received`) writes the
  record and sets its bit.
- **Read** (every reader: the stale and eviction checks in `record_sent`,
  `mark_keep_alive`, the reconcile, the stats windows)
  takes a record's bytes only when its bit is set; a clear bit reads as the
  empty record (sequence 0) without reading the bytes.
- **Reset** clears the bits and leaves the modeled record fields as the
  previous connection left them; summary initialization is separate.

Sequences restart at every reset, so a stale record can hold exactly the
sequence the next connection's reader asks for: only the bit tells it
apart. The model runs the reference ring (zeroed at reset) beside the
implementation.

## Invariants

- **`ReadsAsReference`**: every read returns what the zeroing ring returns.
- **`NoUnwrittenRead`**: no read takes bytes from a record not written since
  the reset.
- **`BitsMatchWrites`**: the bits name exactly the records written since the
  reset.
- **`TypeOK`**.

## Cases

| config | constants | expected |
|---|---|---|
| `MCRNWrittenRing.cfg` | N = 3, MaxSeq = 5, both rules on | pass |
| `MCRNWrittenRingBrokenNoGuard.cfg` | `GuardRead = FALSE` | `NoUnwrittenRead` violated |
| `MCRNWrittenRingBrokenKeepBits.cfg` | `ClearOnReset = FALSE` | `BitsMatchWrites` violated |

Both witnesses were checked by hand against the code:

- **No guard** (Init, then a read of slot 0): a reader that reads a record's
  bytes without its bit reads memory no write initialized. Before this change
  the reconcile in `rocketnet_ack_endpoint_process_block` read
  `record->sequence` for every sequence in the window, written or not, which
  was safe only because the reset zeroed the rings.
- **Keep bits** (Write sequence 1 to slot 1, then Reset): the bit survives,
  and the next connection's first sequence 1 finds the old connection's
  record marked written. `rocketnet_ack_endpoint_reset` zeroes every field
  before `sent`, and the three bitmaps are declared there.

The unit tests compare endpoints with `rocketnet_ack_endpoint_state_equal`
(fields, rtt history, and records where written), and the projection oracle
in `test_unit_ack_stats.cpp` and the bitmap oracle in
`test_unit_ack_block_walk.cpp` read a record only where its bit is set.

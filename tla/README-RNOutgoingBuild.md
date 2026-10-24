# RNOutgoingBuild

The outgoing build of one connection: how a message the game starts, writes
and finishes reaches the transport (docs/OUTGOING-BUILD.md). Fitting
unreliable messages are packed: the connection keeps one open transport
frame, its build, holding a committed prefix of records back to back. Start
reserves the declared maximum at the committed end (the offset); finish
commits the bytes actually written, so a short finish returns the rest of the
reservation, and an abort returns all of it; no committed record moves. Later
records append until the frame is full (the next declared maximum does not
fit) or a flush seals it: send_packets, update, or a start that falls back to
the queued path, which must not overtake the frame's records. Every other
message, and a fitting one that fell back, is written into its region record
and copied into a frame by the write pass.

Code: `sdk/rocketnet_message_server.cpp` (`rn_message_object_start`,
`rn_message_object_finish`, `rn_message_connection_cancel_pending`,
`rn_message_flush_unreliable`, `rn_message_connection_write`) and
`sdk/rocketnet_endpoint.cpp` (`rn_endpoint_in_place_header`,
`rn_endpoint_in_place_affordable`, `rn_endpoint_in_place_finish`,
`rn_endpoint_in_place_sent`). The whole-frame path the code keeps
(`in_place == 1`, `rn_endpoint_in_place_finish`: a plain message that fits
SINGLE but not with a record header, or a transport whose frames are smaller)
is the model's case of a record that fills its frame: it opens a frame, fills
it and is sealed.

- Variables: the open build (`open`, its committed end `offset`, its
  committed records `frameRecs` as `[msg, off, len]`, the frame contents
  `bytes`), the pending reservation (`resAt`, the units written `wrote`), the
  region records (`regionData`), buffer identities (frames `1..NumFrames`,
  region record `NumFrames + m`) and the buffer start handed out (`handed`),
  the frames the transport can hand out, the queued path, the staged and sent
  packets, the acks due and made, the finish order, the shared packet counter
  since `BeginUpdate`, whether a send_packets flush runs (`flushing`), and the
  oldest reliable message the update owes a packet (`promised`). A frame's
  payload is `Cap` units; message `m` declares `Decl[m]` and the game writes
  `1..Decl[m]` of them.
- Actions: `StartInPlace` (only while `InPlaceRoom`, with no unreliable
  message queued: a reservation at the committed end of the open build when
  the declared maximum fits the rest, or a frame opened for it), `StartQueued`
  (a reliable or oversize message, or a fitting one that falls back: no frame,
  unreliable queued ahead, no room in the allowance, a refusal; an unreliable
  one only with no build open), `Write`, `FinishInPlace` (commit at the
  reservation, the committed end advanced by the units written),
  `FinishQueued`, `Abort` (the reservation returned; a frame with no committed
  record back to the transport), `SealBuild` (the start of the next message
  seals the build first: the frame is full, the declared maximum does not
  fit, or the message falls back), `BeginFlush` (seals the open build, then
  the write pass), `SendReliable` (the write pass's reliable phase, first),
  `SendUnreliable` (after it; a packet holding fewer than the whole queue is
  the packet full), `TransportSend` (the flush hands every staged packet to
  the wire), `EndFlush`, and `BeginUpdate` (seals the open build; the sole
  packet-quota reset; the transport's update, where acks become due). Every
  seal (`Seal`, `rn_message_flush_unreliable`) either stages the frame's
  records as one packet carrying the acks due, or, when the send budget
  cannot pay it, drops them (`MESSAGE_UNRELIABLE_OVER_BUDGET`, a fault) and
  returns the frame; the packet counts against the allowance either way.
- Invariants: `TypeOK`, `PacketQuota` (queued and in-place packets share one
  per-update allowance), `BytesUnchangedInPlace` (every record the transport
  has carries the units the game wrote, read at its place when sealed),
  `NoCopy` (buffer identity for every fitting unreliable message that did not
  fall back), `AcksNeverLost`, `ModeOrder` (per kind, what the transport has
  is a prefix of the finish order less drops; every finished message is
  queued, committed in the open build or with the transport; nothing
  duplicated).
- Properties (action): `ReliableSentWithinOneUpdate`: when a send_packets
  flush ends, the reliable message the update owes a packet (queued while the
  allowance had room, the open build's packet counted as spent, or still
  queued when the update began) is with the transport. It is the bounded form
  of "due reliable work is sent": its deadline is the flush, so TLC checks it
  as a safety (action) property; an unbounded `~>` would hold in a finite run
  even without the reservation, because the game's in-place sends run out.
  "Due" in the model is a reliable message waiting for its first send; the
  code's due test (`rn_endpoint_ordered_due`) also counts a resend or block
  chunk past its delay, which the model does not represent.
  `PriorRecordsUnmoved`: until its frame is sealed, every committed record
  keeps its place (offset and length, in offset order) and later records only
  append behind it; its bytes are `BytesUnchangedInPlace`'s.
  `FrameClosedOnlyByFullOrFlush`: a frame holding committed records closes
  only when it is full for the next start (`NextStartSeals`: its records fill
  it, the next declared maximum does not fit, or the next unreliable start
  falls back to the queued path, the flush that keeps the order) or at
  send_packets or update; never at a finish, an abort, an append that fits or
  a reliable start.
- Not modelled: the 16-receipt cap per packet (a seal like a full frame), the
  send budget's admission at start (`rn_endpoint_in_place_affordable`; a
  start the budget cannot pay falls back like one with no room), the fair
  rotation's frame reserve across connections, and receipts and sequencing
  numbers (taken at finish, as on the queued path).
- Bounds: `MaxFaults` bounds the faults of a run together (a frame refused at
  start, an abort, an over-budget drop), so the control stays inside the
  runner's budget.

| Case | Expected | Instance | Distinct states | Seconds |
|---|---|---|---|---|
| `MCRNOutgoingBuild.cfg` | pass | six messages (U, S, R, U, S, R) declaring 2, 2, 1, 2, 1, 1 units, two frames of 3 units, one packet per update, one ack, one fault | DISTINCT_CONTROL | SECONDS_CONTROL |
| `MCRNOutgoingBuildReversed.cfg` | `NoCopy` violated | the same with `CopyStep` TRUE: the build before the in-place change | DISTINCT_REVERSED | SECONDS_REVERSED |
| `MCRNOutgoingBuildOwnQuota.cfg` | `PacketQuota` violated | the same with `SharedQuota` FALSE: in-place packets on an allowance of their own | DISTINCT_OWNQUOTA | SECONDS_OWNQUOTA |
| `MCRNOutgoingBuildNoReserve.cfg` | `ReliableSentWithinOneUpdate` violated | the same with `Reserve` FALSE: no packet kept for due reliable work | DISTINCT_NORESERVE | SECONDS_NORESERVE |
| `MCRNOutgoingBuildBrokenMovesPriorRecord.cfg` | `PriorRecordsUnmoved` violated | the same with `MovePrior` TRUE: a finish moves the committed records | DISTINCT_MOVES | SECONDS_MOVES |
| `MCRNOutgoingBuildBrokenReuseCommitted.cfg` | `BytesUnchangedInPlace` violated | the same with `ReuseCommitted` TRUE: a finish leaves the committed end in place | DISTINCT_REUSE | SECONDS_REUSE |

States and seconds are the pinned runner's measurements in `tla/RUNS.tsv`
(two workers for the control, one for the witnesses, 110-second budget).

The reversed witness's counterexample: message 1 (UNRELIABLE, fitting) is
started on the queued path with no fallback reason, written into its region
record, finished into the queue, and copied by `SendUnreliable` into frame 1;
the staged packet's buffer is frame 1 while start handed out the region
record. That is the code before the in-place change: `rn_endpoint_start` returned
`rn_region_send_write_pointer` for a fitting UNRELIABLE message and
`rn_endpoint_write_packet` copied the record into the frame at send_packets
(`rocketnet_message_writer_add`).

The own-quota witness's counterexample: message 1 (UNRELIABLE) opens a frame
and commits both its units; message 2 (UNRELIABLE_SEQUENCED, two units) does
not fit the one unit left, so `SealBuild` seals the frame (one packet) and
message 2 opens a second frame on the in-place path's own allowance; the
flush seals it and the allowance of one packet is exceeded
(`packetsThisUpdate = 2`). That is
the first attempt's code, in which the in-place path kept a cap of its own
reset at each queued write pass; `rn_endpoint_in_place_header` now reads the
endpoint's one `written` counter, which only `rn_endpoint_begin_update`
resets (unit cases `test_mixed_in_place_and_queued_respect_packet_cap` and
`test_repeated_fill_flush_respects_packet_cap`).

The no-reserve witness's counterexample: message 1 goes in place and its
open build holds the allowance; message 2 is aborted; message 3 (reliable) is queued with no
room; the next update begins (sealing message 1's frame) and owes message 3 a
packet; message 4 (UNRELIABLE) opens a frame in place, and the flush seals it
with the update's only packet; the flush's reliable phase has no room, and the flush ends with
message 3 still queued. In the code without the reservation,
`rn_endpoint_in_place_header` compared `written` alone with
`packets_per_update`, so the in-place packet was admitted while
`rn_endpoint_ordered_due` held, and the write pass's reliable phase
(`written < max_packets`) sent nothing. The unit case
`test_reliable_not_starved_by_in_place` is the same run at a cap of two.

The moves-prior-record witness's counterexample: message 1 (UNRELIABLE)
opens a frame and commits one unit at offset 0; message 2
(UNRELIABLE_SEQUENCED) reserves at offset 1, writes and finishes, and the
broken finish puts its record at offset 0 and moves record 1 to offset 1
(its bytes moved with it, so `BytesUnchangedInPlace` still holds). The code
cannot do this: `rocketnet_message_writer_add_in_place` writes the new
record's header at `writer.bytes`, the committed end, refuses a record whose
data does not start where that header ends, and only advances
`writer.bytes`; nothing in `rn_message_object_finish` or
`rn_message_flush_unreliable` moves bytes below it.

The reuse-committed witness's counterexample: message 1 commits one unit at
offset 0, but the broken finish leaves the committed end at 0; message 2
reserves at offset 0 over record 1, and its abort returns everything past the
stale committed end, record 1's unit included; the flush seals the frame and
the packet carries record 1 with bytes the game did not write. (Without the
abort, message 2's write over record 1 breaks it the same way.) The code
cannot do this: the finish's `rocketnet_message_writer_add_in_place`
advances `writer.bytes` by the header and the bytes written, the next start
hands out `writer.payload + writer.bytes`, and an abort with committed
records (`rn_message_connection_cancel_pending`, `in_place == 2`) leaves the
writer untouched.

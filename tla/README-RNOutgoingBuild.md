# RNOutgoingBuild

The outgoing build of one connection: how a message the game starts, writes
and finishes reaches the transport (docs/OUTGOING-BUILD.md). A fitting
unreliable message is written in place in the transport frame start took;
every other message, and a fitting one that fell back, is written into its
region record and copied into a frame by the write pass.

Code: `sdk/rocketnet_message_server.cpp` (`rn_message_object_start`,
`rn_message_object_finish`, `rn_message_connection_cancel_pending`,
`rn_message_connection_write`) and `sdk/rocketnet_endpoint.cpp`
(`rn_endpoint_in_place_header`, `rn_endpoint_in_place_affordable`,
`rn_endpoint_in_place_finish`, `rn_endpoint_in_place_sent`).

- Variables: the game's written bytes per message (`frameRecs`,
  `regionData`), buffer identities (frames `1..NumFrames`, region record
  `NumFrames + m`) and the buffer start handed out (`handed`), the frames
  the transport can hand out, the queued path, the staged and sent packets,
  the acks due and made, the finish order.
- Actions: `StartInPlace`, `StartQueued` (a reliable or oversize message, or
  a fitting one that falls back: no frame, unreliable queued ahead, a
  refusal), `Write`, `FinishInPlace`, `FinishInPlaceOverBudget`,
  `FinishQueued`, `Abort`, `SendPackets` (one packet of the write pass; a
  packet holding fewer than the whole queue is the packet full),
  `TransportSend`, `AckDue`.
- Invariants: `TypeOK`, `BytesUnchangedInPlace`, `NoCopy` (buffer identity
  for every fitting unreliable message that did not fall back),
  `AcksNeverLost`, `ModeOrder` (per kind, what the transport has is a prefix
  of the finish order less drops; nothing lost or duplicated).
- Bounds: `MaxFaults` bounds the faults of a run together (a frame refused at
  start, an abort, an over-budget drop), so the control stays inside the
  runner's budget.

| Case | Expected | Instance |
|---|---|---|
| `MCRNOutgoingBuild.cfg` | pass | six messages (U, S, R, U, S, R), two frames, one ack, one fault |
| `MCRNOutgoingBuildReversed.cfg` | `NoCopy` violated | the same with `CopyStep` TRUE: the build before this change |

The reversed witness's counterexample: message 1 (UNRELIABLE, fitting) is
started on the queued path with no fallback reason, written into its region
record, finished into the queue, and copied by `SendPackets` into frame 1;
the staged packet's buffer is frame 1 while start handed out the region
record. That is the code before this change: `rn_endpoint_start` returned
`rn_region_send_write_pointer` for a fitting UNRELIABLE message and
`rn_endpoint_write_packet` copied the record into the frame at send_packets
(`rocketnet_message_writer_add`).

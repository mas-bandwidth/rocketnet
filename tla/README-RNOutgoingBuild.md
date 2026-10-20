# RNOutgoingBuild

The outgoing build of one connection: how a message the game starts, writes
and finishes reaches the transport ([docs/OUTGOING-BUILD.md](../docs/OUTGOING-BUILD.md)).
A fitting unreliable message is written in place, in the transport frame that
start takes;
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
  the acks due and made, the finish order, the shared packet-attempt
  counter since `BeginUpdate`, whether a send_packets flush runs
  (`flushing`), and the oldest reliable message the update owes a packet
  (`promised`).
- Actions: `StartInPlace` (only while `InPlaceRoom`: the shared allowance,
  less one packet while a reliable message is queued), `StartQueued` (a
  reliable or oversize message, or a fitting one that falls back: no frame,
  unreliable queued ahead, no room in the allowance, a refusal), `Write`,
  `FinishInPlace`, `FinishInPlaceOverBudget`, `FinishQueued`, `Abort`,
  `BeginFlush`, `SendReliable` (the write pass's reliable phase, first),
  `SendUnreliable` (after it; a packet holding fewer than the whole queue is
  the packet full), `TransportSend` (the flush hands every staged packet to
  the wire), `EndFlush`, and `BeginUpdate` (the sole packet-quota reset; the
  transport's update, where acks become due).
- Invariants: `TypeOK`, `PacketQuota` (queued and in-place attempts share
  one per-update allowance), `BytesUnchangedInPlace`, `NoCopy` (buffer
  identity for every fitting unreliable message that did not fall back),
  `AcksNeverLost`, `ModeOrder` (per kind, what the transport has is a prefix
  of the finish order less drops; nothing lost or duplicated).
- Property: `ReliableSentWithinOneUpdate`, an action property: when a
  send_packets flush ends, the reliable message the update owes a packet
  (queued while the allowance had room, or still queued when the update
  began) is with the transport. It is the bounded form of "due reliable
  work is sent": its deadline is the flush, so TLC checks it as a safety
  (action) property; an unbounded `~>` would hold in a finite run even
  without the reservation, because the game's in-place sends run out.
  "Due" in the model is a reliable message waiting for its first send; the
  code's due test (`rn_endpoint_ordered_due`) also counts a resend or block
  chunk past its delay, which the model does not represent.
- Bounds: `MaxFaults` bounds the faults of a run together (a frame refused at
  start, an abort, an over-budget drop), so the control stays inside the
  runner's budget.

| Case | Expected | Instance |
|---|---|---|
| `MCRNOutgoingBuild.cfg` | pass | six messages (U, S, R, U, S, R), two frames, one packet per update, one ack, one fault |
| `MCRNOutgoingBuildReversed.cfg` | `NoCopy` violated | the same with `CopyStep` TRUE: a fitting unreliable message is built in its region record and copied |
| `MCRNOutgoingBuildOwnQuota.cfg` | `PacketQuota` violated | the same with `SharedQuota` FALSE: in-place packets on an allowance of their own |
| `MCRNOutgoingBuildNoReserve.cfg` | `ReliableSentWithinOneUpdate` violated | the same with `Reserve` FALSE: no packet kept for due reliable work |

The reversed witness's counterexample: message 1 (UNRELIABLE, fitting) is
started on the queued path with no fallback reason, written into its region
record, finished into the queue, and copied by `SendUnreliable` into frame 1;
the staged packet's buffer is frame 1 while start handed out the region
record. That is a build in which `rn_endpoint_start` returns
`rn_region_send_write_pointer` for a fitting UNRELIABLE message and
`rn_endpoint_write_packet` copies the record into the frame at send_packets
(`rocketnet_message_writer_add`). The code instead hands out the frame itself
for a fitting UNRELIABLE message that does not fall back.

The own-quota witness's counterexample: messages 1 (UNRELIABLE) and 2
(UNRELIABLE_SEQUENCED) both start and finish in place in one update, and
the allowance of one packet is exceeded (`packetsThisUpdate = 2`). That is
an in-place path that keeps a cap of its own, reset at each queued write pass.
In the code, `rn_endpoint_in_place_header` reads the endpoint's one `written`
counter, which only `rn_endpoint_begin_update` resets (unit cases `mixed_in_place_and_queued_respect_packet_cap` and
`repeated_fill_flush_respects_packet_cap`).

The no-reserve witness's counterexample: message 1 goes in place and spends
the allowance; message 2 is aborted; message 3 (reliable) is queued with no
room; the next update begins and owes message 3 a packet; message 4
(UNRELIABLE) starts and finishes in place and takes the update's only
packet; the flush's reliable phase has no room, and the flush ends with
message 3 still queued. Without the reservation,
`rn_endpoint_in_place_header` would compare `written` alone with
`max_packets_per_update` and admit the in-place packet while
`rn_endpoint_ordered_due` holds, and the write pass's reliable phase
(`written < max_packets`) would send nothing. The code keeps one packet back
for due reliable work; the unit case `reliable_not_starved_by_in_place` is
the same run at a cap of two.

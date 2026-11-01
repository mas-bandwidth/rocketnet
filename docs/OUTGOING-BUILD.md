# Outgoing messages: direct-write unreliable aggregation

Small UNRELIABLE, UNRELIABLE_SEQUENCED and UNRELIABLE_WITH_ACK_RECEIPT
messages share one open transport frame per connection. The application writes
straight into that frame. Reliable and reliable-ordered messages keep their
existing aggregation: they retain storage for retransmission and copy each
record into each outgoing packet. That copy is intentional.

## Reserve, commit and seal

`start(bytes)` reserves the declared maximum after the committed records,
leaving room for the record header and the packet's trailing format byte.
`finish(bytes_written)` writes the header and advances the committed end by
the actual length. The next start uses that end. A short finish leaves no hole,
and an abort discards only the pending reservation. No earlier payload moves.
One application reservation may be pending per object; different server
connections may each hold a frame containing completed records.

Plain unreliable records use one flags byte, a two-byte length, then the
payload. Sequenced records include sequencing, ordering and channel metadata.
Records are back to back, with one RECORDS format byte at the packet's end.
Binary zeros have no special meaning.

Seal the frame when the next declared maximum will not fit, when adding another
receipt would exceed the 16 receipt references per packet, or at `send_packets`.
`update` also seals completed records before advancing the transport. An
unfinished application reservation at update/send_packets is still API misuse:
it is aborted, while completed records remain eligible to send. Disconnect,
reset and destruction discard open builds and release their frame leases.

The payload cap includes headers and the final format byte. Plain and sequenced
records have no artificial 16-record limit on this direct-write path; available
payload space bounds the count. The queued reliable writer retains its existing
16-reference bound. A plain message that fits SINGLE but cannot fit with a
record header keeps the whole-frame path.

## Budget, ordering and fallback

Each sealed packet consumes one per-update packet allowance and one wire-budget
charge, irrespective of its message count. Existing reliable-work reservations
and server send-capacity fairness checks remain. If budget becomes unavailable
before sealing, drop the unreliable records and emit LOSS for their receipt
serials. ACKED receipts are associated with the packet id only after sealing.

An open build is sealed before falling back to queued unreliable work, so newer
messages do not overtake older queued ones. Sequencing indices and receipt
serials are assigned at finish. Reliable work and its retransmissions continue
through the existing queued writer.

When a transport cannot supply a frame, ordering forbids bypassing queued work,
or packet/budget limits require it, use the existing region fallback and count
`MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK`. The direct-write guarantee concerns the
message layer: it does not change copies internal to individual transports.

## Evidence and scope

`test_unreliable_small_messages_pack_in_place` checks both directions, exact
message delivery, final-frame pointers, and MTU bounds. For 32 messages of
1, 4 and 64 bytes, it expects 1, 1 and 2 packets. This distinguishes aggregation
from the earlier one-message-per-packet sender, which sends 32 packets.
Unit tests additionally count message payload
copies, exercise short finish/abort, receipt boundaries, packet budgets,
interleaved server destinations and stopping with open builds.

`proofs/tla/RNOutgoingBuild.tla` describes the previous one-message-per-frame design.
Its existing results do not qualify aggregation; a revised model has not yet
been checked. The executable tests above are the evidence for this change.

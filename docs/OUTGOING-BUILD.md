# The outgoing build: a fitting unreliable message is the packet

This note describes how `rocketnet_message_client_start` and
`rocketnet_message_server_start` hand out a pointer inside the transport
packet buffer for a fitting unreliable message, so that no copy happens
between the message layer and the transport. The model is
`tla/RNOutgoingBuild.tla` (README: `tla/README-RNOutgoingBuild.md`); the
normative sentence is in standard/MESSAGES.md (RN-2), m2 _Payload forms and
sender size rules_.

## The requirement

A message is UNRELIABLE, UNRELIABLE_SEQUENCED or UNRELIABLE_WITH_ACK_RECEIPT,
and it _fits_ when its whole payload, every message header and the form byte
included, is at most one wire frame (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES`, 1416):

| type | form | header in front of the data | fits when `bytes` is at most |
|------|------|-----------------------------|------------------------------|
| UNRELIABLE | SINGLE | 0 (the form byte is last) | the unreliable limit, 1411 |
| UNRELIABLE_WITH_ACK_RECEIPT | RECORDS, one code-0 record | 3 | the unreliable limit, 1411 |
| UNRELIABLE_SEQUENCED | RECORDS, one code-1 record | 10, or 11 on a channel of 256 or more | 1416 - header - 1 |

For a fitting message the pointer start returns lies inside the frame the
transport later seals and sends. The copies inside a transport stay: the
arena path for a request over one frame, the loopback transport's queue, the
XDP UMEM copy. The reliable path keeps its record copy into the frame,
because a reliable record outlives the frame for its resends.

The declared maximum is start's `bytes`. It decides the buffer: start asks
the transport for `header + bytes + 1` payload bytes, so a message the game
declares at most one frame gets a real wire frame and never the arena.
finish may write fewer bytes than declared; the header and the form byte
are written at finish, for the bytes written.

## Option (a): the message gets its own packet

start asks the transport for one frame (`start_packet`) with headroom for
the message header in front of the data and one byte after it for the form
byte; the transport's own headroom (prefilter header, sequence, ack block)
is already in front of the payload root. finish writes the header and the
form byte around the bytes in place and calls `finish_packet`, which seals
and stages the packet; `send_packets` flushes it with everything else.

- **Acks and other messages riding along.** Acks are the transport's: every
  sealed session packet opens with the ack block, written at the seal
  (standard/TRANSPORT.md, t15 _Acknowledgement_). An in-place packet carries the acks due at its seal,
  as every packet does; nothing in the message layer has to put them there.
  Reliable records, other unreliable messages and the CONFIG record do not
  ride in it; they go in the packets of the write pass in send_packets, as
  today. The cost is one packet per fitting unreliable message: on the wire
  one packet overhead (26 bytes of prefilter header, sequence and tag) plus
  the ack block, against the 3 bytes a code-0 record costs packed beside
  others. Against the send budget each in-place packet is charged at its own
  sealed size, the same rule as any packet. A game that wants several small
  unreliable messages in one packet writes them as one message: that is the
  zero-copy pattern, and the game's serializer already owns its layout.
- **Packet full, or the declared maximum does not fit.** There is no packet
  being assembled, so there is no "full": each message has its own frame,
  sized from the declared maximum. A message whose declared maximum does not
  fit one frame is not a fitting message and keeps its current path (the
  region record; for UNRELIABLE_SEQUENCED the large single-record payload the
  transport fragments; for a large receipt-bearing message the promotion to
  the reliable code-2 block).
- **Ordering against reliable messages.** No unreliable type is ordered
  against a reliable one on the wire today: the write pass sends reliable
  records first and unreliable after. An in-place packet is sealed at finish,
  before the next pass, so it can leave before reliable records queued
  earlier. That is the reordering the network itself may do: an
  UNRELIABLE_SEQUENCED record carries its channel's ordering index, and a
  receiver holds one that is ahead of its channel's read index until the
  ordered records before it arrive (standard/MESSAGES.md), so nothing is lost.
- **Ordering within each unreliable mode.** Packets are sealed in finish
  order, so in-place messages leave in send order. The sequencing index of
  an UNRELIABLE_SEQUENCED message is taken at finish, exactly as on the queued
  path, so the sequenced drop rule (a stale or superseded state is dropped at
  the receiver) is unchanged. Receipt serials are taken at finish in send
  order; the packet's receipt entry is committed under its transport packet
  id when finish_packet returns, so each serial ends ACKED or LOSS exactly
  once. One rule keeps a mode's order when the two paths mix: a message takes
  the in-place path only while the connection's unreliable queue is empty, so
  a message can never overtake one queued before it.
- **Abort.** abort calls `abort_packet` on the frame and clears the pending
  start; nothing was numbered, so nothing else changes.
- **One start pending at a time.** Unchanged: one start is pending per
  object. The transport forbids an update or a flush while a fill is open,
  so an in-place start still pending at update or send_packets is aborted
  there and counted as `MESSAGE_SEND_REFUSED` (an API misuse); its later
  finish returns 0.
- **The send budget.** start takes the in-place path only when the bucket,
  refilled to the update's time, can pay the declared maximum now, no
  reliable item of the last pass waits, and, while the reliable path has
  work due, one maximum packet stays in the bucket for it. That keeps the
  class order for reliable work already due at start; a reliable message sent
  later in the same update, or due work of more than one packet, can find the
  bucket spent by in-place packets and wait for the next update, where on the
  queued path those unreliable messages would have been dropped instead. A message the bucket cannot pay
  now takes the queued path (counted as a fallback), and the write pass in
  send_packets sends it or drops it and counts it
  `MESSAGE_UNRELIABLE_OVER_BUDGET`, by the rule it always had: the drop
  happens in send_packets. finish admits the packet at the unreliable class;
  should the bucket have moved between start and finish it drops the message
  there, counted the same (a receipt-bearing one takes its LOSS), and still
  returns its serial. The settle refunds the difference between the admitted
  maximum and the sealed size.
- **The per-update packet cap.** `packets_per_update` is one allowance
  shared by queued and in-place packet attempts. `begin_update` resets it;
  another `send_packets` flush in the same update does not. Once the allowance
  is spent, a fitting in-place message takes the queued path (counted as a
  fallback), and queued packets wait for the next update. Failed transport
  attempts consume a slot, matching the queued write path. An uncapped
  connection is still bounded.
- **Reliable work is never starved of the cap.** While reliable work is due
  at start (a reliable record waits for its first send, or a resend or block
  chunk is past its delay: the condition the write pass's reliable phase
  sends on), an in-place packet never takes the last slot of the allowance:
  start takes the in-place path only when `written + 1 < packets_per_update`.
  The write pass runs its reliable phase first, so that reserved slot sends
  the due work in the same update. Without the reservation, `cap` in-place
  sends after a reliable send spend the allowance and the reliable record
  waits a whole update (the model's `ReliableSentWithinOneUpdate`, which its
  reversed witness without the reservation violates; the unit case
  `test_reliable_not_starved_by_in_place`). A reliable message sent after the
  in-place packets that spent the allowance waits for the next update, the
  same as one sent after a full write pass. The reservation keys on work
  being due, not on its being writable: when a full window or a waiting
  budget holds the reliable work, the kept packet goes to the unreliable
  phase, and nothing is lost. At `packets_per_update` 1 with reliable
  work due every update, every fitting unreliable message takes the queued
  path; the write pass's reliable-first rule decides it from there, as it
  always did.

## Option (b): written into the connection's current outgoing packet

start would place the message in a frame the connection keeps open across
calls, at the offset after the messages already in it, with room reserved
for headers written later; send_packets would add the reliable records and
seal it.

- **Acks and other messages.** Acks ride as in (a). Several unreliable
  messages share one packet, so the wire cost is a record header each rather
  than a packet each; reliable records could be appended at send_packets
  into the same frame, which needs the ordered writer to start at an offset
  inside a frame it does not own and to share the frame's budget charge.
- **Packet full, or the declared maximum does not fit what is left.** start
  must seal the open frame early and open a new one, so the declared maximum
  decides when a packet closes, and a game that over-declares wastes the
  tail of every packet; a finish that writes less than declared leaves a
  hole the records after it must close, so each record would be moved down,
which is avoided by the current path, or the space is padding on the wire.
- **Ordering.** The open frame holds messages of several modes; its packet
  id is shared, so the receipt entry carries several serials, and a
  sequenced record and the reliable records added later share one packet.
  That is the ordering of today's pass, kept, at the price of holding the
  frame open across the game's calls.
- **Abort.** The aborted record's room in the open frame has to be given
  back; with a later record already written behind it, that is a move or a
  hole again.
- **One start pending, and the transport's fill contract.** The frame stays
  open from one start to the next send_packets, across many calls, and the
  transport forbids an update while a fill is open; every update would have
  to seal it first, and a second thread filling for the same connection would
  need a lock on the open frame.
- **The declared maximum** would decide whether the message fits what is
  left of the open frame, not which buffer it gets.

## The choice: (a)

A fitting unreliable message is its own packet. It is the only option in
which the pointer start returns is the transport's frame with no move, no
hole and no copy whatever finish writes, abort is one call, and the frame is
open only between one start and its finish, as the transport's fill contract
asks. Acks cost nothing extra, because the transport writes them into every
packet. The price is a packet per message where today several small
unreliable records share one, which a game that cares about it avoids by
writing them as one message; that is the zero-copy pattern the layer exists
to offer, and it is the design the original zero-copy send named
(standard/MESSAGES.md: SINGLE form at the payload root).

What (b) would have needed: an open per-connection frame kept across game
calls and sealed before every update; an ordered writer and a budget charge
that start at an offset inside it; a rule for the hole a short finish or an
abort leaves (a move, which is a copy, or padding on the wire); several
receipt serials and modes under one packet id; and a lock for fills from
several threads.

## What changes on the wire and in order

- UNRELIABLE goes in the SINGLE form (body is the message, form byte 0x00)
  in its own packet, where the queued path packed it as a code-0 record in a
  RECORDS payload. A receiver of this standard already reads both.
- UNRELIABLE_WITH_ACK_RECEIPT and UNRELIABLE_SEQUENCED go as one record in a
  RECORDS payload of their own: the same record bytes as today, one per
  packet.
- An in-place packet can leave before reliable records queued earlier in the
  same update (above, ordering against reliable messages).
- A message that does not take the in-place path, because the transport
  gave no frame (a full send ring), because
  unreliable messages already wait in the queue, because the update's packet
  allowance is spent (one slot kept while reliable work is due), or because
  the budget cannot pay it now, takes the queued path as today and is counted
  `MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK`.

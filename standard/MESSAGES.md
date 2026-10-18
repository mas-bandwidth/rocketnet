# RN-2: RocketNet message layer

Version: 1.0 (draft)<br>
Status: normative<br>
Companion documents: RN-1 (`TRANSPORT.md`), the connection layer; RN-R
(`RATIONALE.md`), informative rationale.

Abstract. RN-2 specifies the message layer carried inside RN-1 payloads:
payload forms, the record format, the receiver's checks, reliable numbering,
the eight reliability types, blocks for large reliable messages, channels and
priorities, the CONFIG control record, receipts and the send budget.

<a id="m1"></a>
## m1. Scope, terminology and relation to RN-1

<a id="m1.1"></a>
### m1.1. Scope and conventions

RN-2 specifies the bytes of an RN-1 payload (a type 5 packet, or a whole
fragmented payload of type 9 packets) before the connection layer seals it,
and the behaviour of the two message layer endpoints that exchange them. RN-2
changes no byte and no rule of RN-1. An RN-1 implementation that carries
opaque payloads conforms to RN-1 without RN-2; an implementation of the
message layer MUST conform to both.

The requirements language, conventions and terminology of
[RN-1 t1 Introduction, scope, terminology and conventions](TRANSPORT.md#t1)
apply to RN-2 unchanged. Constants of RN-1 are named by their rows in
[RN-1 t3 Constants and registries](TRANSPORT.md#t3).

<a id="m1.2"></a>
### m1.2. Terminology

| Term | Meaning |
|---|---|
| message | the bytes one application send hands to the message layer, delivered whole or not at all. |
| reliability | one of the eight reliability types of [m1.3 Reliability types](#m1.3), chosen per send. |
| record | the encoding of one message, or one fragment of a block, inside a payload ([m3 Record format](#m3)). |
| code | the 3-bit wire form of a record's reliability, 0 to 4. |
| number | the reliable message counter, one per connection and direction ([m5 Message numbering and the reliable window](#m5)). |
| window | the count of reliable numbers a sender may have unacknowledged at once. |
| block | a reliable message longer than one record can carry, sent as consecutive fragments ([m8 Blocks](#m8)). |
| channel | an independent ordering and sequencing stream within a connection ([m10 Channels and priorities](#m10)). |
| receipt | an outcome reported to the sender for one message ([m12 Receipts](#m12)). |
| promotion | sending a message under its reliable twin instead of the reliability the application chose ([m2 Payload forms and sender size rules](#m2)). |
| send budget | the per-connection limit, in kilobits per second of RN-1 wire bytes, on what one endpoint sends ([m9 Acknowledgement mapping, resend and send budget](#m9)). |
| violation | a payload that breaks a rule of RN-2; it ends the connection ([m4 Receiver checks](#m4)). |
| update | one call by the application that advances an endpoint to a time it passes in. |

<a id="m1.3"></a>
### m1.3. Reliability types

The application chooses one of these per send. The values are the values an
application passes; the wire code each travels as is in
[m3 Record format](#m3).

| Name | Value | Reliable twin |
|---|---|---|
| unreliable | `0` | reliable |
| unreliable sequenced | `1` | reliable sequenced |
| reliable | `2` | |
| reliable ordered | `3` | |
| reliable sequenced | `4` | |
| unreliable with ack receipt | `5` | reliable with ack receipt |
| reliable with ack receipt | `6` | |
| reliable ordered with ack receipt | `7` | |

<a id="m1.4"></a>
### m1.4. What RN-2 requires of RN-1

The message layer is correct only over a connection layer that keeps the
rules below. An implementation of RN-2 MUST run over an RN-1 implementation
that keeps every one of them. Each rule is a requirement on that RN-1
implementation, stated here because RN-2 is the party that relies on it.

1. **One packet.** The connection layer MUST send a payload of at most the
   row `payload capacity` of [RN-1 t3 Constants and registries](TRANSPORT.md#t3)
   as exactly one type 5 packet, never fragmented.
2. **An acknowledged packet was delivered once.** When the connection layer
   reports a type 5 packet acknowledged, the peer's connection layer MUST have
   handed its payload to the peer's message layer exactly once. A connection
   layer MUST NOT acknowledge a type 5 packet whose payload it then discards.
3. **A fragmented payload carries no delivery fact.** An acknowledgement of
   the fragments of a type 9 payload does not prove the payload was
   delivered. A message layer sender MUST NOT place a reliable record or a
   CONTROL record in a payload longer than the row `payload capacity`.
4. **Silence is not loss.** The connection layer MAY leave a sequence
   unacknowledged indefinitely. A message layer sender MUST NOT infer loss
   from a missing acknowledgement; it resends after its timeout
   ([m9 Acknowledgement mapping, resend and send budget](#m9)).
5. **Acknowledgements are reported once, and attributed.** The connection
   layer MUST report each acknowledged packet to the message layer at most
   once, with the connection it was sent on. A server's connection layer MUST
   identify that connection by a value that differs for every connection a
   slot has held, not only by the slot index, and MUST attach the same
   identification to every payload it delivers.
6. **Replies open later.** When the peer sends a packet in reply to a packet
   this endpoint's connection layer opened in one update, the connection
   layer MUST open the reply in a later update, never in the same one.
7. **Replay window depth.** The connection layer's replay window MUST refuse
   every packet as many sequences behind the highest opened sequence as its
   depth, or more, and every packet it opened before. The connection layer
   MUST configure that depth no smaller than the row
   `replay window depth minimum` and no larger than the row
   `replay window depth maximum` of [RN-1 t3 Constants and registries](TRANSPORT.md#t3).
   Within that depth RN-2's 24-bit numbers do not alias
   ([m5 Message numbering and the reliable window](#m5)).
8. **Sequences rise in write order.** Within one direction of one
   connection's session phase, the connection layer MUST assign sequences
   that rise in the order the message layer writes the packets' payloads. An
   implementation that fills one connection's packets from several threads
   MUST keep the fills in the order their sequences were taken.

<a id="m2"></a>
## m2. Payload forms and sender size rules

Status: pending

Scope: the form byte (last byte of the payload), SINGLE and RECORDS, the size
rules a sender keeps, and promotion. Decided requirements this section
states: a message sent as unreliable, unreliable sequenced or unreliable with
ack receipt that is larger than one packet's records can carry MUST be sent
under its reliable twin in [m1.3 Reliability types](#m1.3), as a block where it needs one; the
promotion is visible to the application as a named outcome or counter, never
silent.

<a id="m3"></a>
## m3. Record format

Status: pending

Scope: the flags byte (code, BLOCK, WIDE, CONTROL), the length, and the
conditional fields in order; one encoding per record; the mapping from the
eight reliabilities to codes 0 to 4; vectors.

<a id="m4"></a>
## m4. Receiver checks

Status: pending

Scope: the ordered checks over a whole payload before any record is handed
up, each naming one violation; duplicates.

<a id="m5"></a>
## m5. Message numbering and the reliable window

Status: pending

Scope: 24-bit numbers, sequencing and ordering indexes; reconstruction by
signed half range; the window; the no-alias bound from the replay window
depth that [m1.4 What RN-2 requires of RN-1](#m1.4) requires.

<a id="m6"></a>
## m6. Delivery: unreliable codes 0 and 1

Status: pending

Scope: at-most-once delivery for code 0; sequenced delivery for code 1 that
never goes backwards on its channel.

<a id="m7"></a>
## m7. Delivery: reliable codes 2, 3 and 4

Status: pending

Scope: exactly-once delivery; ordered delivery on a channel; reliable
sequenced delivery; the receive ring.

<a id="m8"></a>
## m8. Blocks

Status: pending

Scope: the block stride, fragment numbering, ring placement, completion and
single delivery; vectors.

<a id="m9"></a>
## m9. Acknowledgement mapping, resend and send budget

Status: pending

Scope: mapping RN-1 packet acknowledgements to records; resend timing and
the reliable timeout; the send budget. Decided requirements this section
states, with their pins: the send budget default is 1000 kilobits per second
for a client (client to server) and 10000 kilobits per second for a server,
per client (server to each client); both are configuration fields under
[RN-1 t2 Architecture and key inventory](TRANSPORT.md#t2); a value of 0 or
below disables the budget, and disabling it is NOT RECOMMENDED; a reliable
record that does not fit the budget waits and is never dropped; an
unreliable record that does not fit is dropped and counted.

<a id="m10"></a>
## m10. Channels and priorities

A connection has a runtime number of channels, each an independent ordering and
sequencing stream. This section fixes what of channels is wire-visible: the
numbering, the runtime count bound, and which records carry a channel and how
it is encoded. How the streams are scheduled is not wire-visible and binds no
implementation.

1. **Channel numbering.** A message layer connection has `num_channels`
   channels, numbered 0 through `num_channels - 1`. An application chooses a
   channel by this number for every send that uses one. `num_channels` is a
   per-connection value from configuration and MUST NOT exceed the compile-time
   maximum row `num channels maximum`.
2. **Which records carry a channel.** A record of code 1, code 3 or code 4,
   and the receipt twin of each ([m1.3 Reliability types](#m1.3)), carries the
   channel field of [m3 Record format](#m3). A record of code 0 or code 2
   carries no channel.
3. **Channel encoding.** A channel below 256 is encoded as a u8. A channel of
   256 or more is encoded as a u16. A sender MUST set the WIDE flag when and
   only when the channel is 256 or more, so a record has exactly one encoding.

| Name | Value | Meaning |
|---|---|---|
| num channels maximum | `32` | the compile-time maximum runtime channel count |

```vector m10.narrow-channel
record: RELIABLE_ORDERED, channel 3, number 5, ring 0, ordering 0, data "hi"
03 02 00 05 00 00 00 00 00 00 00 03 68 69 01
```

```vector m10.wide-channel
record: RELIABLE_ORDERED, channel 300 (WIDE), number 5, ring 0, ordering 0, data "hi"
13 02 00 05 00 00 00 00 00 00 00 2c 01 68 69 01
```

<a id="m11"></a>
## m11. CONFIG control record

Status: pending

Scope: the one CONFIG record each endpoint sends, its 28-byte body, the
fields compared, mismatch handling; the defaults of the wire fields; vectors.

<a id="m12"></a>
## m12. Receipts

Types 5, 6 and 7 request a receipt. They are application-level types: the wire
carries only their base codes, and nothing else about a receipt is wire-visible.
The report of an outcome to the application is an API action.

1. **Wire code.** A sender MUST NOT put a receipt type on the wire. Type 5
   travels as code 0, type 6 as code 2 and type 7 as code 3
   ([m1.3 Reliability types](#m1.3)).
2. **Promotion.** A type-5 send whose data is larger than one packet's record
   can carry, above the 1411 bytes of the code-0 form, MUST be sent under its
   reliable twin ([m2 Payload forms and sender size rules](#m2)), as code 2, as
   a record or a block as its size needs, and MUST keep its receipt. The
   receiver sees code 2 on the wire. An implementation MUST count every
   promotion once, per send, in a counter for the source mode.
3. **Outcomes.** A send reports exactly one outcome, `ACKED` or `LOSS`, named
   in the API. A reliable send, and a promoted type-5 block, reports `ACKED`
   when its numbers are acknowledged; an unreliable receipt-bearing send reports
   `ACKED` when the carrying packet is acknowledged and `LOSS` when it is lost
   or dropped. These outcomes are API semantics; none of them is on the wire.

```vector m12.promotion
record: code 2 block, fragment 1 of 2, number 0, ring 72, data "A"
0a 01 00 00 00 00 48 00 01 00 01 00 41 01
```

<a id="m13"></a>
## m13. Security considerations

The transport seals every payload; this section bounds what a peer can do
within the message layer of a sealed connection.

1. **Violations end the connection.** A payload that breaks a rule of RN-2 is
   a violation. A receiver MUST refuse the whole payload, MUST NOT hand up any
   record of it, and MUST end the connection. A correct sender never sends a
   violation, so one is evidence of a faulty or hostile peer. Each violation has
   its own counter.
2. **No live growth.** Every table a peer can influence is fixed at connection
   creation from configuration. A receiver MUST NOT allocate memory after
   connection creation, so a peer MUST NOT be able to make a receiver hold more
   than the configured region. The checks of [m4 Receiver checks](#m4) run
   before a record is held or delivered.
3. **Bounds.** A record whose channel, number, span or size falls outside the
   configured values, the window or the ring is refused by
   [m4 Receiver checks](#m4), [m5 Message numbering and the reliable window](#m5)
   and [m8 Blocks](#m8), never stored. A receiver MUST NOT read outside the byte
   range the transport delivers.
4. **Amplification.** The checks run over a payload once. A receiver MUST
   complete them in time bounded by the payload's own length, with no
   allocation, so a peer MUST NOT be able to grow a receiver's work or memory
   with a small payload.

<a id="ma"></a>
## ma. Appendix A: test vectors

This appendix indexes every vector of RN-2 by the section that defines it.
It is generated; the bytes are in the sections.

<!-- vector-index:begin -->
| Section | Vector |
|---|---|
| [m10 Channels and priorities](#m10) | `m10.narrow-channel` |
| [m10 Channels and priorities](#m10) | `m10.wide-channel` |
| [m12 Receipts](#m12) | `m12.promotion` |
<!-- vector-index:end -->

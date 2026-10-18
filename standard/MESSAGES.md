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

<a id="m6.1"></a>
### m6.1. Rule

A sender chooses unreliable or unreliable sequenced in [m1.3 Reliability types](#m1.3); the wire code and the record's fields are in [m3 Record format](#m3). The rules below state what the receiver delivers, holds, drops and refuses.

1. A sender MUST send an unreliable message as code 0 and an unreliable sequenced message as code 1. A code 0 send larger than one record can carry MUST be sent under code 2, its reliable twin, and a code 1 send larger than one record can carry MUST be sent under code 4, its reliable twin. A receiver MUST deliver a promoted message under the code it arrives as.
2. A code 0 record and a code 2 record carry no channel field. A receiver MUST deliver a code 0 record, a code 2 record, and the message of application type 5 or 6 that travels as code 0 or 2, on channel 0. A code 1 record MUST be delivered on the channel its record names.
3. A receiver MUST deliver a code 0 record at most once by appending the record's bytes to the update's delivery list. It MUST NOT take a reliable number, keep a received state, retransmit the record, or acknowledge it at the message layer.
4. The per-update unreliable delivery allowance is the connection's `unreliable_deliveries` configuration field. When the update has already delivered that many code 0 records, a receiver MUST drop each further code 0 record, MUST count the drop as `MESSAGE_UNRELIABLE_DELIVERY_FULL`, and MUST NOT treat the drop as a violation.
5. A receiver MUST keep, for each channel, an ordered read index and a sequencing watermark. A code 1 record is in its epoch when its ordering index equals the read index. A record in its epoch whose sequencing index is at or after the watermark MUST be delivered, and the receiver MUST then set the watermark to one past that sequencing index modulo the counter modulus.
6. A code 1 record in its epoch whose sequencing index is before the watermark MUST be dropped and counted `MESSAGE_SEQUENCED_STALE`. A code 1 record whose ordering index is behind the read index, when the difference lies in the upper half of the counter modulus, MUST be dropped and counted `MESSAGE_SEQUENCED_STALE`.
7. A code 1 record ahead of its epoch by less than the window MUST be held as the channel's newest held message. A receiver MUST keep at most one held code 1 message per channel, in one slot of the connection's `sequenced_holds` configuration field. A newer held message, ordered by ordering index and then by sequencing index, MUST replace the held message, and the replaced message MUST be dropped and counted `MESSAGE_SEQUENCED_SUPERSEDED`. A candidate that is not newer than the held message MUST be dropped and counted `MESSAGE_SEQUENCED_SUPERSEDED`. A record larger than one hold slot, or a record with no free hold slot, MUST be dropped and counted `MESSAGE_SEQUENCED_HOLD_FULL`.
8. When an ordered message is delivered on a channel, the receiver MUST set the channel's read index to one past that ordering index and MUST reset the channel's watermark to zero. The receiver MUST then deliver the channel's held message when its ordering index is the read index and its sequencing index is at or after the watermark, and MUST clear the hold.
9. A code 1 record ahead of the read index by at least the window MUST be refused as a violation and counted `MESSAGE_ORDERING_AHEAD`. A violation ends the connection.
10. A sender MUST take a code 1 record's ordering index from the channel's ordered write index and its sequencing index from the channel's sequencing write index, and MUST increment the sequencing write index. An ordered send MUST take the ordered write index as its ordering index, MUST increment the ordered write index, and MUST reset the sequencing write index to zero.
11. A sender that promotes a send under rule 1 MUST make the promotion visible to the application as a named outcome or counter. The promotion is never silent.

<a id="m6.2"></a>
### m6.2. Constants

| Name | Value | Unit | Meaning |
|---|---|---|---|
| sequenced hold data bytes | `1416` | bytes | the data capacity of one code 1 hold slot |
| default sequenced holds | `16` | slots | the default number of hold slots in a connection |
| default unreliable deliveries | `256` | messages | the default per-update code 0 delivery allowance |
| counter modulus | `16777216` | values | the modulus of the ordering and sequencing indexes |

<a id="m6.3"></a>
### m6.3. Delivery states

The rows are the code 1 record's outcome on one channel. An event a row does not name leaves the state unchanged.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| current | code 1 record | ordering equals read index and sequencing at or after watermark | deliver, watermark becomes one past sequencing | current |
| current | code 1 record | ordering equals read index and sequencing before watermark | drop, count `MESSAGE_SEQUENCED_STALE` | current |
| current | code 1 record | ordering ahead by less than the window | hold as newest | held |
| held | code 1 record | ordering ahead of the read index and newer than the held message | supersede the held message, count `MESSAGE_SEQUENCED_SUPERSEDED` | held |
| held | code 1 record | ordering ahead of the read index and not newer than the held message | drop, count `MESSAGE_SEQUENCED_SUPERSEDED` | held |
| held | code 1 record | ordering behind the read index | drop, count `MESSAGE_SEQUENCED_STALE` | held |
| held | code 1 record | ordering equals read index | deliver if sequencing at or after watermark, else drop as stale | current |
| held | ordered delivery | read index reaches the held ordering index | deliver the held message, reset watermark, clear the hold | current |
| any | code 1 record | ordering ahead by at least the window | violation, count `MESSAGE_ORDERING_AHEAD` | ended |
| any | code 1 record | no free hold slot or larger than one hold slot | drop, count `MESSAGE_SEQUENCED_HOLD_FULL` | unchanged |

<a id="m6.4"></a>
### m6.4. Vectors

Each vector is one delivery trace. Its input lines name the configuration and one record per step, in order. The bytes are the step outcomes in that same order: one outcome byte, then a count byte, then that many delivered entries. An entry is the delivery's channel (u8), code (u8), length (u16, little-endian) and data bytes. The outcome bytes are `0x00` delivered, `0x01` held, `0x02` stale, `0x03` dropped by the unreliable delivery allowance, `0x04` dropped by the sequenced hold pool, and `0x05` violation.

```vector m6.code0-delivery-trace
config: unreliable_deliveries 2
step 1: code 0 channel 0 data 6f 6e 65
step 2: code 0 channel 0 data 74 77 6f
step 3: code 0 channel 0 data 74 68 72 65 65
step 4: code 0 channel 0 data 66 6f 75 72
00 01 00 00 03 00 6f 6e 65 00 01 00 00 03 00 74
77 6f 03 00 03 00
```

```vector m6.code1-sequenced-trace
config: defaults
step 1: code 1 channel 0 ordering 0 sequencing 0 data 61
step 2: code 1 channel 0 ordering 0 sequencing 1 data 62
step 3: code 1 channel 0 ordering 0 sequencing 0 data 63
step 4: code 1 channel 0 ordering 1 sequencing 0 data 64
step 5: code 1 channel 0 ordering 1 sequencing 1 data 65
step 6: code 1 channel 0 ordering 1 sequencing 0 data 66
step 7: code 3 channel 0 ordering 0 data 6f
step 8: code 1 channel 0 ordering 1 sequencing 0 data 67
step 9: code 1 channel 0 ordering 1 sequencing 2 data 68
step 10: code 1 channel 0 ordering 2 sequencing 0 data 69
step 11: code 3 channel 0 ordering 1 data 70
step 12: code 1 channel 0 ordering 2 sequencing 0 data 6a
step 13: code 1 channel 0 ordering 2 sequencing 1 data 6b
step 14: code 1 channel 0 ordering 1026 sequencing 0 data 78
00 01 00 01 01 00 61 00 01 00 01 01 00 62 02 00
01 00 01 00 02 00 00 02 00 03 01 00 6f 00 01 01
00 65 02 00 00 01 00 01 01 00 68 01 00 00 02 00
03 01 00 70 00 01 01 00 69 02 00 00 01 00 01 01
00 6b 05 00
```

```vector m6.code1-hold-full-trace
config: sequenced_holds 1
step 1: code 1 channel 0 ordering 1 sequencing 0 data 78
step 2: code 1 channel 1 ordering 1 sequencing 0 data 79
step 3: code 1 channel 0 ordering 1 sequencing 1 data 7a
step 4: code 3 channel 0 ordering 0 data 6f
01 00 04 00 01 00 00 02 00 03 01 00 6f 00 01 01
00 7a
```

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

Status: pending

Scope: per-channel state, the four priority levels, round robin across
channels, FIFO within a channel, and the packet writer's order.

<a id="m11"></a>
## m11. CONFIG control record

Status: pending

Scope: the one CONFIG record each endpoint sends, its 28-byte body, the
fields compared, mismatch handling; the defaults of the wire fields; vectors.

<a id="m12"></a>
## m12. Receipts

Status: pending

Scope: the receipt outcomes, when each is reported, and the receipt of a
promoted message.

<a id="m13"></a>
## m13. Security considerations

Status: pending

Scope: violations end the connection; memory bounds from configuration; what
a hostile peer can and cannot make a receiver hold.

<a id="ma"></a>
## ma. Appendix A: test vectors

This appendix indexes every vector of RN-2 by the section that defines it.
It is generated; the bytes are in the sections.

<!-- vector-index:begin -->
| Section | Vector |
|---|---|
| [m6 Delivery: unreliable codes 0 and 1](#m6) | `m6.code0-delivery-trace` |
| [m6 Delivery: unreliable codes 0 and 1](#m6) | `m6.code1-hold-full-trace` |
| [m6 Delivery: unreliable codes 0 and 1](#m6) | `m6.code1-sequenced-trace` |
<!-- vector-index:end -->

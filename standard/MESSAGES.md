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

A message that fits one record of its reliability MUST be sent as that
record, not as a block. A message that exceeds the capacity of a single
record of its reliability MAY be sent as a block ([m2 Payload forms and sender size rules](#m2)). A block is a reliable message carried as consecutive
fragments that share one sequence of message numbers and one ring span,
reassembled on the receiver and delivered whole.

**The stride.** The block stride F is the constant `1152` bytes (the table
below). Every fragment except the last carries exactly F bytes; the last
carries from one to F bytes. A block of k fragments occupies F × (k − 1)
bytes plus the last fragment's bytes.

**Numbering.** A sender numbers a block's fragments zero to k − 1 and gives
them consecutive message numbers, taken in ascending fragment order when the
fragments enter packets.

**Ring placement.** A block's fragments occupy one contiguous span of the
receive ring. The span starts at the base its fragments' ring fields name, in
16-byte units, and extends through the last fragment's end, rounded up to a
multiple of 16. When an assigned span would cross a multiple of the ring size
B, the sender pads at the boundary so the span wraps.

**Completion and delivery.** A block completes when every fragment has
arrived. A completed block is delivered exactly once, whole, at its single
ordering slot; an incomplete block is never delivered.

| Name | Value | Units |
|---|---|---|
| block stride | `1152` | bytes |

**Vectors.** The vectors below are RECORDS payloads, before sealing, each one
fragment of a reliable ordered block. The first is a non-final fragment
carrying exactly the stride; the second is a final fragment shorter than the
stride.

```vector m8.split-nonfinal
stride: 1152
number: 9
ring: 72
ordering: 7
channel: 3
fragment: 1
fragments: 3
data: 0, 1, ..., 15 repeated
0b 80 04 09 00 00 48 00 07 00 00 03 01 00 02 00
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
01
```

```vector m8.final-short
stride: 1152
number: 10
ring: 72
ordering: 8
channel: 3
fragment: 3
fragments: 4
data: 0, 1, ..., 99
0b 64 00 0a 00 00 48 00 08 00 00 03 03 00 03 00
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 01
```
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
| [m8 Blocks](#m8) | `m8.final-short` |
| [m8 Blocks](#m8) | `m8.split-nonfinal` |
<!-- vector-index:end -->

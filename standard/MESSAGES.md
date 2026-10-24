# RN-2: RocketNet message layer

Version: 1.0<br>
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
   reports a type 5 packet acknowledged, the receiver's connection layer MUST have
   handed its payload to the receiver's message layer exactly once. A connection
   layer MUST NOT acknowledge a type 5 packet whose payload it then discards.
   The receiver MUST finish successful message-layer processing before ACK publication.
   If a reliable record cannot reserve receive storage, the receiver MUST leave
   that record's number, ordering state and received bit uncommitted, withhold
   the carrying packet's ACK, and keep the connection alive for retry. Records
   already accepted from the packet remain retained or delivered; a retry MUST
   deduplicate that prefix. Once capacity returns, a fresh-sequence retransmission
   MUST be accepted normally. Existing reliable-message timeouts still apply.
   Admission MUST reserve space for every held delivery an ordered record can
   release, not only the record itself. Malformed records remain protocol errors.
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
6. **Replies open later.** When the remote endpoint sends a packet in reply to a packet
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

<a id="m1.5"></a>
### m1.5. Sending on a connection that is not connected

An endpoint MUST NOT send an application message on a connection that is not connected. A send the application makes on such a connection MUST be dropped, MUST NOT be held for a later send, and MUST be counted once in `MESSAGE_SEND_NOT_CONNECTED`. The rule is the same for a client and for a server.

A client's connection is connected while the client is in connected (4) of [RN-1 t11 Handshake: client](TRANSPORT.md#t11); a server's connection to a slot is connected from the session [RN-1 t12 Handshake: server](TRANSPORT.md#t12) assigns that slot until the session ends. The CONFIG control record is not an application message and keeps the timing of [m11 CONFIG control record](#m11). The drop is visible to the application as a refused send and in the counter.

<a id="m1.6"></a>
### m1.6. Receiving every delivered message

A receiver hands each update's deliveries to the application through one
receive queue. RN-1 acknowledges a packet when it accepts it, before the
application has seen the messages it carried, and the sender's window then
moves past their numbers ([m5.1 Sender numbering](#m5.1)). The receiver
therefore cannot keep a delivered message past the update that delivered it,
and it cannot ask for the message again.

1. The application MUST take every message delivered in an update from the
   receive queue before the next update of that endpoint.
2. When an update finds a delivered message that the application did not
   take, the receiver MUST treat it as application misuse. It MUST count each
   such message once in `MESSAGE_NOT_RECEIVED`, and MUST end that connection
   with the reason `MESSAGE_NOT_RECEIVED`. A server ends only the connections
   that have a message left; the others are unaffected. An implementation
   SHOULD also assert in a debug build.
3. A receiver MUST NOT drop a delivered message without the count and the
   end of rule 2, and MUST NOT deliver it at a later update.
4. A message the receiver did not offer to the application because its
   connection already ended is not misuse and is not counted.

<a id="m2"></a>
## m2. Payload forms and sender size rules

A payload ends with one form byte. A sender MUST write exactly one form byte, and a receiver MUST refuse a payload shorter than 2 bytes or one whose form byte is neither `0x00` nor `0x01`.

| Form | Value | Body |
|---|---|---|
| SINGLE | `0x00` | exactly one UNRELIABLE message |
| RECORDS | `0x01` | one or more records |

A sender MAY use SINGLE only for a message whose reliability is UNRELIABLE and whose length is at most the row `SINGLE message limit`. The message starts at payload offset 0 and the form byte follows it. A receiver MUST deliver a SINGLE payload as one UNRELIABLE message on channel 0.

A fitting unreliable message is written in place in the packet buffer, with no copy between the message layer and the connection layer. A message is fitting when its reliability is UNRELIABLE, UNRELIABLE_SEQUENCED or UNRELIABLE_WITH_ACK_RECEIPT and its data, record header and form byte fit in one payload. A sender MUST aggregate fitting messages into back-to-back records of a shared RECORDS payload when room remains. Start reserves the declared maximum after the committed prefix; finish commits only the actual length, and abort leaves the prefix unchanged. Neither operation moves committed payloads. Seal the payload when the next reservation cannot fit, a receipt-reference bound is reached, or send_packets or update closes the build. A sender MAY use SINGLE for an otherwise fitting plain UNRELIABLE message that cannot fit with a record header. A sender MUST write a fitting message in place, except that it MAY copy one into a shared RECORDS payload when the connection layer gives no packet buffer large enough for it, when messages queued before it would otherwise be overtaken by it, when the send budget cannot pay it, or when the update's packet allowance is spent; a sender counts each such message once in `MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK`. A receiver reads both forms alike.

A sender MUST encode every other message as one or more records in a RECORDS payload. The records MUST be written back to back, with no padding, and MUST fill the body exactly. An unreliable message that exceeds its one-record limit MUST be sent under its reliable twin as the promotion rule states; it MUST NOT be fragmented and MUST NOT be split across payloads.

<a id="m2.1"></a>
### m2.1. One-record limits and promotion

The one-record limit of a reliability is the largest message that reliability sends as exactly one record. The limits are rows of [m2.2 Constants](#m2.2).

| Reliability | Wire code | One record at most | Above the limit |
|---|---|---|---|
| UNRELIABLE | 0 | the row `UNRELIABLE record limit` | sent as RELIABLE (code 2) |
| UNRELIABLE_SEQUENCED | 1 | the row `UNRELIABLE_SEQUENCED record limit`, or the WIDE row on a channel of 256 or more | sent as RELIABLE_SEQUENCED (code 4) |
| RELIABLE | 2 | the row `RELIABLE record limit` | sent as a block |
| RELIABLE_ORDERED | 3 | the row `RELIABLE_ORDERED record limit` | sent as a block |
| RELIABLE_SEQUENCED | 4 | the row `RELIABLE_SEQUENCED record limit` | sent as a block |
| UNRELIABLE_WITH_ACK_RECEIPT | 5 | the row `UNRELIABLE record limit`, in wire code 0 | sent as RELIABLE (code 2) and keeps its ACKED receipt |

When the connection layer's payload capacity is below the row `maximum payload size`, the UNRELIABLE_SEQUENCED one-record limit is that capacity less the record's header and the form byte. A sender MUST send a message at or below its one-record limit as exactly one record. A sender MUST send a reliable message above that limit as a block of consecutive fragments of the row `block stride` bytes; only the last fragment MAY be shorter, and every fragment MUST carry at least 1 byte.

A sender MUST promote an UNRELIABLE, UNRELIABLE_SEQUENCED or UNRELIABLE_WITH_ACK_RECEIPT message above its one-record limit to its reliable twin: code 0 to code 2, code 1 to code 4, and code 5 to its reliable receipt twin (reliability 6, wire code 2) with the ACKED receipt kept. The receiver observes the twin's wire code. The sender MUST count each promotion once per send in `MESSAGE_PROMOTED_UNRELIABLE`, `MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED` or `MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT`, according to the reliability the application chose.

A sender MUST refuse a message above the configured maximum message size. For UNRELIABLE, UNRELIABLE_SEQUENCED and UNRELIABLE_WITH_ACK_RECEIPT that is the only refusal by size: a message at or below the configured maximum and above its one-record limit MUST be promoted, never refused by size, and a refused message MUST NOT be promoted. The row `record maximum message` bounds the data length of one record, not of a message: a message above its one-record limit travels as a block, and no record carries more than the row `record maximum message`. A receiver MUST accept a reliable or CONTROL record only in a payload no longer than the row `maximum payload size`.

<a id="m2.2"></a>
### m2.2. Constants

| Name | Value | Units | Meaning |
|---|---|---|---|
| form byte SINGLE | `0x00` | | the form byte of a SINGLE payload |
| form byte RECORDS | `0x01` | | the form byte of a RECORDS payload |
| maximum payload size | `1416` | bytes | the largest payload a message layer sender writes |
| SINGLE message limit | `1415` | bytes | the largest message the SINGLE form carries |
| record fixed bytes | `3` | bytes | the flags and length of every record |
| record maximum header | `20` | bytes | the largest record header |
| record maximum message | `65535` | bytes | the largest record data length |
| block stride | `1152` | bytes | the data bytes of every fragment but the last |
| UNRELIABLE record limit | `1411` | bytes | one-record limit of UNRELIABLE and UNRELIABLE_WITH_ACK_RECEIPT |
| UNRELIABLE_SEQUENCED record limit | `1405` | bytes | one-record limit on a channel below 256 |
| UNRELIABLE_SEQUENCED record limit, WIDE | `1404` | bytes | one-record limit on a channel of 256 or more |
| RELIABLE record limit | `1408` | bytes | one-record limit of RELIABLE |
| RELIABLE_ORDERED record limit | `1401` | bytes | one-record limit of RELIABLE_ORDERED |
| RELIABLE_SEQUENCED record limit | `1399` | bytes | one-record limit of RELIABLE_SEQUENCED |
| code UNRELIABLE | `0` | | wire code of UNRELIABLE |
| code UNRELIABLE_SEQUENCED | `1` | | wire code of UNRELIABLE_SEQUENCED |
| code RELIABLE | `2` | | wire code of RELIABLE |
| code RELIABLE_ORDERED | `3` | | wire code of RELIABLE_ORDERED |
| code RELIABLE_SEQUENCED | `4` | | wire code of RELIABLE_SEQUENCED |

### Vectors

```vector m2.single-limit
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad 00
```

```vector m2.single-over
0a 80 04 09 00 00 00 00 00 00 01 00 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 01
```

```vector m2.code0-at-limit
00 83 05 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 01
```

```vector m2.code0-over
0a 80 04 05 00 00 00 00 00 00 01 00 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 01
```

```vector m2.code1-1404
01 7c 05 04 00 00 07 00 00 03 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 01
```

```vector m2.code1-1405
01 7d 05 04 00 00 07 00 00 03 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98 9f a6
ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08 0f 16
1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78 7f 86
8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8 ef f6
fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58 5f 66
6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8 cf d6
dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38 3f 46
4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8 af b6
bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26
2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88 8f 96
9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06
0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68 6f 76
7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8 df e6
ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48 4f 56
5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8 bf c6
cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36
3d 44 4b 52 59 60 67 01
```

```vector m2.code1-1406
0c 80 04 05 00 00 00 00 04 00 00 07 00 00 03 00
00 01 00 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 01
```

```vector m2.reliable-1407
02 7f 05 05 00 00 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 01
```

```vector m2.reliable-1408
02 80 05 05 00 00 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2
c9 d0 d7 de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32
39 40 47 4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2
a9 b0 b7 be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12
19 20 27 2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82
89 90 97 9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2
f9 00 07 0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62
69 70 77 7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2
d9 e0 e7 ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42
49 50 57 5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2
b9 c0 c7 ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22
29 30 37 3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92
99 a0 a7 ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02
09 10 17 1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72
79 80 87 8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2
e9 f0 f7 fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52
59 60 67 6e 75 7c 01
```

```vector m2.reliable-1409
0a 80 04 05 00 00 00 00 00 00 01 00 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 01
```

```vector m2.reliable-ordered-1400
03 78 05 05 00 00 00 00 07 00 00 03 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 01
```

```vector m2.reliable-ordered-1401
03 79 05 05 00 00 00 00 07 00 00 03 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83 8a 91 98
9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3 fa 01 08
0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a 71 78
7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3 da e1 e8
ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43 4a 51 58
5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3 ba c1 c8
cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23 2a 31 38
3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93 9a a1 a8
af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03 0a 11 18
1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73 7a 81 88
8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3 ea f1 f8
ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53 5a 61 68
6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3 ca d1 d8
df e6 ed f4 fb 02 09 10 17 1e 25 2c 33 3a 41 48
4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3 aa b1 b8
bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13 1a 21 28
2f 36 3d 44 4b 01
```

```vector m2.reliable-ordered-1402
0b 80 04 05 00 00 00 00 07 00 00 03 00 00 01 00
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec
f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c
63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc
d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c
43 4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac
b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c
23 2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c
93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc
03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c
53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc
c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c
33 3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c
a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c
13 1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c
01
```

```vector m2.reliable-sequenced-1398
04 76 05 05 00 00 00 00 04 00 00 07 00 00 03 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 01
```

```vector m2.reliable-sequenced-1399
04 77 05 05 00 00 00 00 04 00 00 07 00 00 03 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 44 4b 52 59 60 67 6e 75 7c 83
8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7 de e5 ec f3
fa 01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63
6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7 be c5 cc d3
da e1 e8 ef f6 fd 04 0b 12 19 20 27 2e 35 3c 43
4a 51 58 5f 66 6d 74 7b 82 89 90 97 9e a5 ac b3
ba c1 c8 cf d6 dd e4 eb f2 f9 00 07 0e 15 1c 23
2a 31 38 3f 46 4d 54 5b 62 69 70 77 7e 85 8c 93
9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7 ee f5 fc 03
0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c 73
7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc e3
ea f1 f8 ff 06 0d 14 1b 22 29 30 37 3e 45 4c 53
5a 61 68 6f 76 7d 84 8b 92 99 a0 a7 ae b5 bc c3
ca d1 d8 df e6 ed f4 fb 02 09 10 17 1e 25 2c 33
3a 41 48 4f 56 5d 64 6b 72 79 80 87 8e 95 9c a3
aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7 fe 05 0c 13
1a 21 28 2f 36 3d 01
```

```vector m2.reliable-sequenced-1400
0c 80 04 05 00 00 00 00 04 00 00 07 00 00 03 00
00 01 00 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 83 8a 91 98 9f a6 ad b4 bb c2 c9 d0 d7
de e5 ec f3 fa 01 08 0f 16 1d 24 2b 32 39 40 47
4e 55 5c 63 6a 71 78 7f 86 8d 94 9b a2 a9 b0 b7
be c5 cc d3 da e1 e8 ef f6 fd 04 0b 12 19 20 27
2e 35 3c 43 4a 51 58 5f 66 6d 74 7b 82 89 90 97
9e a5 ac b3 ba c1 c8 cf d6 dd e4 eb f2 f9 00 07
0e 15 1c 23 2a 31 38 3f 46 4d 54 5b 62 69 70 77
7e 85 8c 93 9a a1 a8 af b6 bd c4 cb d2 d9 e0 e7
ee f5 fc 03 0a 11 18 1f 26 2d 34 3b 42 49 50 57
5e 65 6c 73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7
ce d5 dc e3 ea f1 f8 ff 06 0d 14 1b 22 29 30 37
3e 45 4c 53 5a 61 68 6f 76 7d 84 8b 92 99 a0 a7
ae b5 bc c3 ca d1 d8 df e6 ed f4 fb 02 09 10 17
1e 25 2c 33 3a 41 48 4f 56 5d 64 6b 72 79 80 87
8e 95 9c a3 aa b1 b8 bf c6 cd d4 db e2 e9 f0 f7
fe 05 0c 13 1a 21 28 2f 36 3d 44 4b 52 59 60 67
6e 75 7c 01
```

<a id="m3"></a>
## m3. Record format

A record is a flags byte, a 16-bit length, zero or more conditional fields in the order below, and the data. A sender MUST write the flags byte and the length first and the conditional fields in the order given. A sender MUST write a conditional field exactly when its condition holds and MUST NOT write it otherwise. A field a record does not carry is absent, not zero-filled.

The flags byte carries the code in bits 0 to 2, BLOCK in bit 3, WIDE in bit 4, CONTROL in bit 5, and zero in bits 6 and 7.

      0
      0 1 2 3 4 5 6 7
     +-+-+-+-+-+-+-+-+
     | code  |B|W|C|Z|Z|
     +-+-+-+-+-+-+-+-+

| Field | Offset | Size | Type | Present when | Valid values |
|---|---|---|---|---|---|
| flags | 0 | 1 | u8 | always | code 0 to 4; bits 6 and 7 zero |
| length | 1 | 2 | u16 | always | 1 to the row `record maximum message` |
| number | 3 | 3 | u24 | code 2, 3 or 4 | 0 to 16777215 |
| ring | after the present fields before it | 2 | u16 | code 3 or 4, or BLOCK set | 0 to 65535 |
| sequencing | after the present fields before it | 3 | u24 | code 1 or 4 | 0 to 16777215 |
| ordering | after the present fields before it | 3 | u24 | code 1, 3 or 4 | 0 to 16777215 |
| channel | after the present fields before it | 1 or 2 | u8 or u16 | code 1, 3 or 4 | 0 to 255, or 0 to 65535 when WIDE |
| fragment | after the present fields before it | 2 | u16 | BLOCK set | 0 to fragments |
| fragments | after the present fields before it | 2 | u16 | BLOCK set | 1 to 65535 |
| data | after the present fields before it | length | bytes[length] | always | any bytes |

The ring field is the absolute position of the record's first data byte in the receive ring, measured in 16-byte units and reduced modulo the ring size in those units:

    ring = ( absolute byte position / 16 ) mod ( B / 16 )

A sender MUST write that value, and a receiver MUST multiply the field by 16 to recover the position in the ring. For a block, the ring field is the position of that fragment's first data byte, so the block's base position is the ring field less the fragment index times the row `block stride` divided by 16.

The application chooses one of eight reliabilities. The wire code each travels as is:

| Reliability | Chosen as | Wire code |
|---|---|---|
| unreliable | 0 | 0 |
| unreliable sequenced | 1 | 1 |
| reliable | 2 | 2 |
| reliable ordered | 3 | 3 |
| reliable sequenced | 4 | 4 |
| unreliable with ack receipt | 5 | 0 |
| reliable with ack receipt | 6 | 2 |
| reliable ordered with ack receipt | 7 | 3 |

A sender MUST set BLOCK exactly on a record that is one fragment of a block, and BLOCK MUST NOT be set on code 0 or code 1. A sender MUST set WIDE exactly when the record carries a channel and the channel is 256 or greater, and WIDE MUST NOT be set on a record that carries no channel. A sender MUST set CONTROL only on a code 2 record that is not a block. A sender MUST set bits 6 and 7 of the flags byte to zero. A wire code of 5 to 7 MUST NOT appear.

Every record has exactly one encoding. A field a record does not carry is absent, not zero-filled, and every field a record carries is written with its full width. A receiver MUST parse the fields from the flags byte alone, including a flags byte it refuses, and MUST measure the header before it judges the flags.

### Vectors

```vector m3.code0
00 02 00 68 69 01
```

```vector m3.code1
01 02 00 04 00 00 07 00 00 03 68 69 01
```

```vector m3.code2
02 02 00 05 00 00 68 69 01
```

```vector m3.code3
03 02 00 05 00 00 00 00 07 00 00 03 68 69 01
```

```vector m3.code4
04 02 00 05 00 00 00 00 04 00 00 07 00 00 03 68
69 01
```

```vector m3.block-wide-last
1b 01 00 09 00 00 48 00 07 00 00 2c 01 01 00 01
00 5a 01
```
<a id="m4"></a>
## m4. Receiver checks


A receiver MUST apply the checks below, in this order, to one payload. A
payload that fails a check is a violation: the receiver MUST refuse the whole
payload, MUST hand no record of it to the application, and MUST increment the
counter that the check names. Checks 1 to 9 run over the whole payload before
any record of it is handed up, except check 7's block-descriptor agreement,
which runs when the record is applied. Checks 10 to 12 run as each record is
applied.
A duplicate is not a violation: the receiver MUST count it as
`MESSAGE_DUPLICATE` and MUST NOT refuse the payload for it.

A record is measured before its flags are judged, so a code of 5 to 7 still
has a defined header. The receiver's runtime values are its channel count C,
its enabled wire-code mask, its window W, its maximum message size, and its
receive ring size B.

The constants this section names are:

| Name | Value | Unit | Meaning |
|---|---|---|---|
| message code count | `5` | codes | wire codes 0 to 4 are defined |
| wide channel threshold | `256` | channels | the lowest channel a WIDE record carries |
| message fragment bytes | `1152` | bytes | the block stride F |
| message number modulus | `16777216` | numbers | 2^24, the number, sequencing and ordering space |
| config body bytes | `28` | bytes | the CONFIG control body |
| payload capacity | `1416` | bytes | the largest payload a sender emits in one packet |

1. The payload MUST be at least 2 bytes, and its last byte MUST be 0 for
   SINGLE or 1 for RECORDS. Counter: `MESSAGE_BAD_FORM`.
2. In RECORDS form, at least the row `record fixed bytes` MUST remain for a record,
   and every field the flags name MUST lie inside the body. Counter:
   `MESSAGE_TRUNCATED`.
3. A record MUST NOT carry a code at or above `message code count`, a set bit
   in the two high flag bits, BLOCK on code 0 or 1, WIDE on a code without a
   channel field, or CONTROL on a code other than 2 or together with BLOCK.
   Counter: `MESSAGE_BAD_FLAGS`.
4. A record's length MUST be at least 1, and its data MUST lie inside the
   body. Counter: `MESSAGE_BAD_LENGTH`.
 5. A record with a channel field MUST carry a channel below C, and a WIDE
    record MUST carry a channel at or above `wide channel threshold`. Counter:
    `MESSAGE_BAD_CHANNEL`.
 6. An application record MUST carry a code present in the receiver's enabled
    mask. A CONTROL record is exempt. Counter: `MESSAGE_TYPE_NOT_ENABLED`.
 7. In the whole-payload scan, a block record MUST satisfy all of: its
    fragment count is at least 1; its fragment index is at most its fragment
    count; its fragment count plus one is at most W; its fragment count plus
    one, times `message fragment bytes`, is at most the maximum message size
    plus `message fragment bytes` minus 1; every fragment but the last is
    exactly `message fragment bytes` long; the last is at most `message fragment bytes` long. Counter: `MESSAGE_BAD_BLOCK`. When the
    record is applied, a block record's ring base, the ring less the fragment
    index times `message fragment bytes` divided by 16, MUST agree with a live
    block descriptor for the same block. Counter: `MESSAGE_BAD_BLOCK`.
8. A holdable record MUST have a span inside B. The span of a non-block record
   is its length rounded up to 16 bytes, added to 16 times its ring value. The
   span of a block runs from its base, the ring value less the fragment index
   times `message fragment bytes` divided by 16, to the end of its last
    fragment; a fragment that is not the last is measured as the shortest block
    it could belong to. Counter: `MESSAGE_BAD_RING`.
9. A reliable or CONTROL record MUST NOT lie in a payload longer than
   `payload capacity`. A record's length, and a SINGLE payload's message
   length, MUST be at most the receiver's maximum message size; check 7 holds a
   block to the same limit. A SINGLE payload takes checks 1, 6 and this one.
   Counter: `MESSAGE_BAD_SIZE`.
10. A reliable record MUST have a number less than W ahead of the receiver's
     next number. Counter: `MESSAGE_NUMBER_OUTSIDE_WINDOW`. A record whose
     receive slot is live for a different number MUST be refused. Counter:
     `MESSAGE_SLOT_COLLISION`.
11. A code 3 record MUST NOT carry an ordering index behind its channel's read
    index, modulo `message number modulus`, and MUST NOT carry one W or more
    ahead of it. Counters: `MESSAGE_ORDERING_BEHIND` and
    `MESSAGE_ORDERING_AHEAD`.
12. A CONTROL record MUST carry a recognised kind, a body of exactly
     `config body bytes` bytes for kind 1, and zero reserved bytes for a known
     version. Counter: `MESSAGE_BAD_CONTROL`.

The vectors below freeze one malformed payload for each check and the counter
it produces.

```vector m4.check-1-bad-form
input: one payload byte whose form value is 2
counter: `MESSAGE_BAD_FORM`
02
```

```vector m4.check-2-truncated
input: a RECORDS payload whose body is one byte
counter: `MESSAGE_TRUNCATED`
00 01
```

```vector m4.check-3-bad-flags
input: a record whose code is 5
counter: `MESSAGE_BAD_FLAGS`
05 01 00 aa 01
```

```vector m4.check-4-bad-length
input: a code 0 record whose length is zero
counter: `MESSAGE_BAD_LENGTH`
00 00 00 01
```

```vector m4.check-5-bad-channel
input: a code 1 record on channel 4, the receiver channel count
counter: `MESSAGE_BAD_CHANNEL`
01 01 00 00 00 00 00 00 00 04 aa 01
```

```vector m4.check-6-type-not-enabled
input: a code 2 record whose code is absent from the enabled mask
counter: `MESSAGE_TYPE_NOT_ENABLED`
02 01 00 00 00 00 aa 01
```

```vector m4.check-7-bad-block
input: a block fragment whose fragment count is zero
counter: `MESSAGE_BAD_BLOCK`
0a 01 00 00 00 00 00 00 00 00 00 00 aa 01
```

```vector m4.check-8-bad-ring
input: a code 3 record at ring 65535 in a 65536-byte ring
counter: `MESSAGE_BAD_RING`
03 01 00 00 00 00 ff ff 00 00 00 00 aa 01
```

```vector m4.check-9-bad-size
input: a code 2 record that makes the payload 1417 bytes
counter: `MESSAGE_BAD_SIZE`
02 82 05 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 01
```

```vector m4.check-9-over-maximum
input: a SINGLE message of 1416 bytes at a receiver whose maximum message size is 1415
counter: `MESSAGE_BAD_SIZE`
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00
```

```vector m4.check-10-number-outside-window
input: a code 2 record numbered one window above the next number
counter: MESSAGE_NUMBER_OUTSIDE_WINDOW
02 01 00 00 04 00 aa 01
```

```vector m4.check-11-ordering-behind
input: a code 3 record one behind the channel read index
counter: MESSAGE_ORDERING_BEHIND
03 01 00 00 00 00 00 00 63 00 00 00 aa 01
```

```vector m4.check-12-bad-control
input: a CONTROL record whose body is 27 bytes
counter: MESSAGE_BAD_CONTROL
22 1b 00 00 00 00 01 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 01
```


<a id="m5"></a>
## m5. Message numbering and the reliable window

The message layer gives a number to each reliable record it sends and to no
other record. A number is a 24-bit counter that wraps. The receiver recovers
the 64-bit value of the counter from the wire value against a reference. This
section states the sender's numbering, the two bounds on it, the receiver's
reconstruction and window check, and the connection layer preconditions that
make the reconstruction unambiguous.

### m5.1. Sender numbering

1. A sender MUST assign the next number to a reliable record only when it
   first writes that record into a packet, and MUST NOT assign a number when
   the application queues the message. A record of code 0 or 1 MUST NOT take
   a number.
2. A sender MUST assign consecutive numbers, one per record, in the order it
   writes the records. A block MUST take one consecutive number for each of
   its fragments at the moment its first fragment is written.
3. Let `next` be the next number the sender assigns, `U` the lowest number it
   has not yet seen acknowledged, and `U'` the window base: `U` when no
   unacknowledged block contains `U`, and the first number of the lowest such
   block otherwise. A sender MUST number a record with number `n` only while
   `n < U' + W`, where `W` is the connection's `reliable_messages_in_flight` field and
   the row `window messages default` below gives its default.
4. A holdable record is a record with code 3, code 4, or a block. For a
   holdable record a sender MUST place the record in the receive ring of `B`
   bytes, where `B` is the connection's `receive_message_buffer_bytes` field, at the
   ring span `[p, p + len16)`. `len16` is the record's length rounded up to a
   multiple of the row `ring unit`. `p` is `ring_next` when the span does not
   cross a multiple of `B`; otherwise `p` is the next multiple of `B`, and the
   bytes between `ring_next` and `p` are padding. The sender MUST admit the
   span only when `p + len16 - floor <= B`, where `floor` is the ring start of
   `U'` when `U' < next` and `p` otherwise. A block MUST reserve the span of
   all its fragments at once. A code 2 record that is not a block MUST take a
   number and no ring bytes.
5. When a record cannot be numbered under the count bound or the byte bound,
   a sender MUST NOT number new reliable records until an acknowledgement
   moves `U'`. It MAY keep resending unacknowledged records and MAY keep
   writing unreliable records. A sender MUST count the first refusal of a
   stall in `MESSAGE_WINDOW_STALL` and MUST NOT count further refusals until
   numbering succeeds or `U'` moves.
6. A sender MUST keep every number it sends inside `[U', U' + W)` and every
   holdable span inside `B`. A receiver MUST police both claims as the checks
   of [m4 Receiver checks](#m4) describe.

### m5.2. Receiver reconstruction and window check

For each reliable record, in wire order, the receiver reconstructs the 64-bit
number `n` from the wire value `w` and its reference `R`, the lowest number it
has not yet received:

```
d     = ( w - ( R mod 2^24 ) ) mod 2^24
delta = d          when d < 2^23
delta = d - 2^24   otherwise
n     = R + delta
```

7. A receiver MUST use the half range exactly as stated: a difference at or
   above the row `half range` is negative. A receiver MUST reconstruct against
   the current `R` for each record.
8. A receiver MUST apply these outcomes in this order:
   1. When `delta < 0`, the record is a duplicate: the receiver MUST drop it,
      MUST count `MESSAGE_DUPLICATE`, and MUST NOT treat it as a violation.
   2. When `delta >= W` or `R + delta` exceeds the largest `u64`, the record is
      a violation and the receiver MUST refuse the connection with
      `MESSAGE_NUMBER_OUTSIDE_WINDOW`.
   3. When the descriptor at `n mod W` is live for a number other than `n`, the
      record is a violation and the receiver MUST refuse the connection with
      `MESSAGE_SLOT_COLLISION`.
   4. Otherwise the record is accepted: the receiver MUST mark `n` received,
      and while the number at `R` is marked received, MUST clear that mark and
      MUST increment `R`.
9. A duplicate is a normal outcome of a resend and carries no reliable
   delivery. The receiver MUST NOT report the message to the application a
   second time.

### m5.3. Precondition on the connection layer

10. The reconstruction is unambiguous only while both of these hold. The
    connection layer's replay window depth MUST be no larger than the row
    `replay window depth maximum`. Transport sequences MUST rise in the order
    in which the message layer writes the packets' payloads.
11. A stale copy that the replay window still opens is then at most the row
    `stale copy bound` numbers behind `R`, and that value is below the row
    `half range`. A receiver MUST refuse a stale copy as a duplicate under
    rule 8.1 rather than read it as a fresh number.

### m5.4. Constants and configuration

| Name | Value | Unit | Meaning |
|---|---|---|---|
| first number | `0` | number | the first reliable number of each direction |
| number bits | `24` | bits | the width of the wire number |
| number range | `16777216` | numbers | one more than the largest wire number |
| number mask | `0xFFFFFF` | mask | the 24-bit wire value |
| half range | `8388608` | numbers | the signed half range of the 24-bit counter |
| ring unit | `16` | bytes | the unit of the ring offset and of `len16` |
| fragment bytes | `1152` | bytes | the block stride |
| window messages default | `1024` | records | the default of `reliable_messages_in_flight` |
| receive ring bytes default | `1048576` | bytes | the default of `receive_message_buffer_bytes` |
| window messages limit | `1048576` | records | the largest valid `reliable_messages_in_flight` |
| replay window depth minimum | `4096` | sequences | the smallest valid connection layer replay depth |
| replay window depth maximum | `32768` | sequences | the largest valid connection layer replay depth |
| minimum numbered record | `7` | bytes | a code 2 header and one data byte |
| numbered records per packet maximum | `202` | records | a payload's capacity divided by the minimum record |
| block fragments maximum | `911` | records | the largest block that the largest message makes |
| stale copy bound | `7668421` | numbers | the most a stale copy can lie behind `R` |

### m5.5. Vectors

The bytes of a reconstruction vector are one outcome byte followed, for an
accepted record only, by the reconstructed number as a `u64` little-endian:
`0x00` accepted, `0x01` duplicate, `0x02` violation. A conforming
implementation produces the same outcome and the same number from the same
inputs.

```vector m5.reconstruct-at-wrap
reference: 16777215
wire number: 0
outcome: accepted
00 00 00 00 01 00 00 00 00
```

```vector m5.reconstruct-at-half-range
reference: 8388608
wire number: 0
outcome: duplicate
01
```

```vector m5.reconstruct-duplicate
reference: 100
wire number: 99
outcome: duplicate
01
```

```vector m5.reconstruct-stale-copy-at-depth
reference: 7668421
wire number: 0
replay window depth maximum: 32768
window messages limit: 1048576
numbered records per packet maximum: 202
block fragments maximum: 911
outcome: duplicate
01
```

<a id="m6"></a>
## m6. Delivery: unreliable codes 0 and 1

<a id="m6.1"></a>
### m6.1. Rule

A sender chooses unreliable or unreliable sequenced in [m1.3 Reliability types](#m1.3); the wire code and the record's fields are in [m3 Record format](#m3). The rules below state what the receiver delivers, holds, drops and refuses.

1. A sender MUST send an unreliable message as code 0 and an unreliable sequenced message as code 1. A code 0 send larger than one record can carry MUST be sent under code 2, its reliable twin, and a code 1 send larger than one record can carry MUST be sent under code 4, its reliable twin. A receiver MUST deliver a promoted message under the code it arrives as.
2. A code 0 record and a code 2 record carry no channel field. A receiver MUST deliver a code 0 record, a code 2 record, and the message of application type 5 or 6 that travels as code 0 or 2, on channel 0. A code 1 record MUST be delivered on the channel its record names.
3. A receiver MUST deliver a code 0 record at most once by appending the record's bytes to the update's delivery list. It MUST NOT take a reliable number, keep a received state, retransmit the record, or acknowledge it at the message layer.
4. The per-update unreliable delivery allowance is the connection's `unreliable_messages_per_update` configuration field. A code 0 delivery and a code 1 delivery each count against it. When the update has already made that many code 0 and code 1 deliveries, a receiver MUST drop each further code 0 record and each further code 1 record, MUST count the drop as `MESSAGE_UNRELIABLE_DELIVERY_FULL`, and MUST NOT treat the drop as a violation.
5. A receiver MUST keep, for each channel, an ordered read index and a sequencing watermark. A code 1 record is in its epoch when its ordering index equals the read index. A record in its epoch whose sequencing index is at or after the watermark MUST be delivered, and the receiver MUST then set the watermark to one past that sequencing index modulo the counter modulus.
6. A code 1 record in its epoch whose sequencing index is before the watermark MUST be dropped and counted `MESSAGE_SEQUENCED_STALE`. A code 1 record whose ordering index is behind the read index, when the difference lies in the upper half of the counter modulus, MUST be dropped and counted `MESSAGE_SEQUENCED_STALE`.
7. A code 1 record ahead of its epoch by less than the window MUST be held as the channel's newest held message. A receiver MUST keep at most one held code 1 message per channel, in one slot of the connection's `sequenced_messages_held` configuration field. A newer held message, ordered by ordering index and then by sequencing index, MUST replace the held message, and the replaced message MUST be dropped and counted `MESSAGE_SEQUENCED_SUPERSEDED`. A candidate that is not newer than the held message MUST be dropped and counted `MESSAGE_SEQUENCED_SUPERSEDED`. A record larger than one hold slot, or a record with no free hold slot, MUST be dropped and counted `MESSAGE_SEQUENCED_HOLD_FULL`.
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
| default unreliable deliveries | `1024` | messages | the default per-update code 0 and code 1 delivery allowance |
| counter modulus | `16777216` | values | the modulus of the ordering and sequencing indexes |

<a id="m6.3"></a>
### m6.3. Delivery states

The rows are the code 1 record's outcome on one channel. An event a row does not name leaves the state unchanged.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| current | code 1 record | ordering equals read index and sequencing at or after watermark | deliver, watermark becomes one past sequencing | current |
| current | code 1 record | ordering equals read index and sequencing before watermark | drop, count `MESSAGE_SEQUENCED_STALE` | current |
| current | code 1 record | ordering equals read index and the update's unreliable delivery allowance is full | drop, count `MESSAGE_UNRELIABLE_DELIVERY_FULL` | current |
| current | code 1 record | ordering ahead by less than the window | hold as newest | held |
| held | code 1 record | ordering ahead of the read index and newer than the held message | supersede the held message, count `MESSAGE_SEQUENCED_SUPERSEDED` | held |
| held | code 1 record | ordering ahead of the read index and not newer than the held message | drop, count `MESSAGE_SEQUENCED_SUPERSEDED` | held |
| held | code 1 record | ordering behind the read index | drop, count `MESSAGE_SEQUENCED_STALE` | held |
| held | code 1 record | ordering equals read index | deliver if sequencing at or after watermark and the allowance is not full; otherwise drop as stale or by the allowance; the hold stays | held |
| held | ordered delivery | read index reaches the held ordering index and the allowance is not full | deliver the held message, reset watermark, clear the hold | current |
| held | ordered delivery | read index reaches the held ordering index and the allowance is full | drop the held message, count `MESSAGE_UNRELIABLE_DELIVERY_FULL`, reset watermark, clear the hold | current |
| any | code 1 record | ordering ahead by at least the window | violation, count `MESSAGE_ORDERING_AHEAD` | ended |
| any | code 1 record | no free hold slot or larger than one hold slot | drop, count `MESSAGE_SEQUENCED_HOLD_FULL` | unchanged |

<a id="m6.4"></a>
### m6.4. Vectors

Each vector is one delivery trace. Its input lines name the configuration and one record per step, in order. The bytes are the step outcomes in that same order: one outcome byte, then a count byte, then that many delivered entries. An entry is the delivery's channel (u8), code (u8), length (u16, little-endian) and data bytes. The outcome bytes are `0x00` delivered, `0x01` held, `0x02` stale, `0x03` dropped by the unreliable delivery allowance, `0x04` dropped by the sequenced hold pool, and `0x05` violation.

```vector m6.code0-delivery-trace
config: unreliable_messages_per_update 2
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
config: sequenced_messages_held 1
step 1: code 1 channel 0 ordering 1 sequencing 0 data 78
step 2: code 1 channel 1 ordering 1 sequencing 0 data 79
step 3: code 1 channel 0 ordering 1 sequencing 1 data 7a
step 4: code 3 channel 0 ordering 0 data 6f
01 00 04 00 01 00 00 02 00 03 01 00 6f 00 01 01
00 7a
```

<a id="m7"></a>
## m7. Delivery: reliable codes 2, 3 and 4

This section starts after the receive window of
[m5 Message numbering and the reliable window](#m5) has accepted a record's
number. A duplicate number is dropped by the window and counted as a
duplicate; it is never delivered and is not a violation.

### m7.1. Code 2, RELIABLE

1. A receiver MUST deliver a code 2 record exactly once, when its number is
   accepted, and MUST NOT consult an ordering index, a sequencing index or a
   channel.
2. Code 2 records MAY be delivered in an order other than their number order.
3. A code 2 record whose number is outside the window, or whose slot is held
   by another live number, is a violation.

### m7.2. Code 3, RELIABLE_ORDERED

1. A receiver MUST deliver a code 3 record on a channel only when the
   record's ordering index equals the channel's next ordering index.
2. When the ordering index is ahead of the next index by fewer than the
   window count, the receiver MUST hold the record whole, copy it once into
   the receive ring at the ring offset the sender chose, and keep its number
   live until it is delivered. The receiver MUST key the held record by the
   channel and the ordering index. A record whose channel and ordering index
   are already held by another record is a violation and MUST be refused with
   `MESSAGE_DUPLICATE_ORDERING`.
3. An ordering index ahead of the next index by the window count or more is a
   violation, because a correct sender numbers fewer than the window count of
   records at once.
4. An ordering index behind the next index in 24-bit half-range order is a
   violation; a duplicate number was already dropped by the window.
5. Delivering a code 3 record MUST set the channel's next ordering index to
   one past the record's ordering index.
6. After every delivery on a channel, the receiver MUST drain: it MUST deliver
   the held record whose ordering index is now the next index, and repeat,
   until no held record carries the next index or the record carrying it is a
   block whose fragments are not all present ([m8 Blocks](#m8)).
7. A held record MUST be delivered exactly once, in ascending ordering order
   on its channel, and only by the drain or by being in turn.

### m7.3. Code 4, RELIABLE_SEQUENCED

1. A code 4 record carries an ordering index and a sequencing index. The
   receiver MUST deliver it only in its ordering epoch: the record's ordering
   index MUST equal the channel's next ordering index, and its sequencing
   index MUST not be behind the channel's sequencing watermark in 24-bit
   half-range order.
2. Delivering a code 4 record MUST set the sequencing watermark to one past
   the record's sequencing index, and MUST NOT advance the next ordering
   index. Only an ordered delivery advances the next ordering index.
3. A code 4 record in its epoch whose sequencing index is behind the
   watermark MUST be dropped and counted as stale; it is not a violation.
4. A code 4 record ahead of its epoch by fewer than the window count MUST be
   held in the receive ring at the ring offset the sender chose. The receiver
   MUST keep at most one held code 4 record per channel, the newest by
   ordering index and then sequencing index; a newer record replaces the held
   one, and an older or equal one MUST be dropped and counted as superseded.
5. A code 4 record behind its epoch in 24-bit half-range order MUST be
   dropped and counted as stale; it is not a violation.
6. An ordering index ahead of the next index by the window count or more is a
   violation.
7. When an ordered delivery opens an epoch, the drain MUST first release the
   held code 4 record for that epoch if its sequencing index is not behind the
   watermark, then continue with the held ordered records. An ordered
   delivery sets the sequencing watermark to zero.

### m7.4. Outcomes

Every change this section makes to a record is visible to the application or
to a counter: a delivery, a hold, a stale drop, a superseded drop, a duplicate
drop or a violation. Nothing is silent.

The tables below define the trace vectors of this section. They are not wire
fields; a trace is the record of what the delivery rules above did with a
sequence of records.

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |  dispatch     |  violation    |  deliveries   |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |  code         |  channel      |             bytes             |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                            data ...                           |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| dispatch | 0 | 1 | u8 | what the dispatch did with the input record | a row of the table below |
| violation | 1 | 1 | u8 | nonzero when the connection became violated on this record | `0`, `1` |
| deliveries | 2 | 1 | u8 | delivery entries that follow, in delivery order | `0` to `255` |
| code | 3 | 1 | u8 | the delivered record's application code | `2`, `3`, `4` |
| channel | 4 | 1 | u8 | the delivered record's channel | `0` to `255` |
| bytes | 5 | 2 | u16 | the delivered message's length | `1` to `65535` |
| data | 7 | `bytes` | bytes[`bytes`] | the delivered message | the input record's data |

Each delivery entry is 7 bytes plus its data. A step is 3 bytes plus its
delivery entries. The trace is the steps of the input records in order.

| Name | Value | Meaning |
|---|---|---|
| `none` | `0` | no dispatch: the window dropped the record as a duplicate, or refused it |
| `delivered` | `1` | the record produced at least one delivery entry |
| `held` | `2` | the record is held for a later ordering index or epoch |
| `stale` | `3` | the record is dropped as stale or superseded |

### m7.5. Vectors

The traces below fix the receiver's configuration at one channel, window
count 64, receive ring 4096 bytes, maximum message 4096 bytes and 16
sequenced holds. Every record carries one data byte. `ring` and `number` are
the record's fields as [m3 Record format](#m3) defines them.

```vector m7.reliable-loss-reorder
config: 1 channel, window 64, ring 4096, max message 4096, sequenced holds 16
input: code 2 number 2 data 12
input: code 2 number 0 data 10 (reordered behind number 2)
input: code 2 number 3 data 13 (number 1 is delayed)
input: code 2 number 1 data 11 (the delayed record arrives last)
input: code 2 number 2 data 12 (a duplicate)
input: code 2 number 68 data 44 (64 ahead of the next unread number)
01 00 01 02 00 01 00 12 01 00 01 02 00 01 00 10
01 00 01 02 00 01 00 13 01 00 01 02 00 01 00 11
00 00 00 00 01 00
```

```vector m7.reliable-ordered-hold-drain
config: 1 channel, window 64, ring 4096, max message 4096, sequenced holds 16
input: code 3 number 0 ring 1 ordering 1 channel 0 data 21
input: code 3 number 1 ring 3 ordering 3 channel 0 data 23
input: code 3 number 2 ring 0 ordering 0 channel 0 data 20
input: code 3 number 3 ring 2 ordering 2 channel 0 data 22
input: code 3 number 2 ring 0 ordering 0 channel 0 data 20 (a duplicate)
input: code 3 number 4 ring 4 ordering 0 channel 0 data 24 (behind)
02 00 00 02 00 00 01 00 02 03 00 01 00 20 03 00
01 00 21 01 00 02 03 00 01 00 22 03 00 01 00 23
00 00 00 00 01 00
```

```vector m7.reliable-sequenced-hold-drain
config: 1 channel, window 64, ring 4096, max message 4096, sequenced holds 16
input: code 3 number 0 ring 0 ordering 0 channel 0 data 30 (opens epoch 0)
input: code 4 number 1 ring 1 ordering 1 sequencing 0 channel 0 data 31
input: code 4 number 2 ring 1 ordering 1 sequencing 1 channel 0 data 32
input: code 4 number 3 ring 1 ordering 1 sequencing 0 channel 0 data 33 (behind the watermark)
input: code 4 number 4 ring 2 ordering 2 sequencing 0 channel 0 data 34 (held)
input: code 4 number 5 ring 2 ordering 2 sequencing 1 channel 0 data 35 (replaces the held record)
input: code 4 number 6 ring 2 ordering 2 sequencing 0 channel 0 data 36 (superseded)
input: code 3 number 7 ring 1 ordering 1 channel 0 data 37 (opens epoch 1 and drains)
input: code 4 number 8 ring 2 ordering 2 sequencing 3 channel 0 data 38
input: code 4 number 9 ring 3 ordering 66 sequencing 0 channel 0 data 39 (64 ahead of the next ordering index)
01 00 01 03 00 01 00 30 01 00 01 04 00 01 00 31
01 00 01 04 00 01 00 32 03 00 00 02 00 00 02 00
00 03 00 00 01 00 02 03 00 01 00 37 04 00 01 00
35 01 00 01 04 00 01 00 38 00 01 00
```

<a id="m8"></a>
## m8. Blocks

A block is a reliable message carried as consecutive fragments that share one
number sequence and one receive-ring span, reassembled by the receiver and
delivered whole. Each fragment is one record ([m3 Record format](#m3)).

1. **Use.** A sender MUST send a reliable message that fits one record of its
   reliability as that record. A sender MUST send a reliable message that does
   not fit one record of its reliability as a block. A sender MUST send an
   unreliable, an unreliable sequenced or an unreliable with ack receipt
   message larger than one packet's records can carry under its reliable twin
   as a block where that twin needs one
   ([m2 Payload forms and sender size rules](#m2)).
2. **Split.** The block stride F is the row `block stride` of
   [m2 Payload forms and sender size rules](#m2). A block of L bytes MUST have
   k = ceil(L / F) fragments, with k at least 2. Fragments 0 to k − 2 MUST
   carry exactly F bytes each; fragment k − 1 MUST carry from 1 to F bytes.
3. **Numbering.** A sender MUST assign the block's fragments consecutive
   numbers in ascending fragment order, all before the first fragment enters a
   payload. If the first fragment's number is b, fragment i MUST carry number
   b + i. Every fragment MUST carry the block's channel and ordering index;
   the block MUST take one ordering slot.
4. **Ring placement.** The receive ring is B bytes. A sender MUST place a
   block in one contiguous span that starts at ring position p and does not
   cross a multiple of B. When the span does not fit between the position the
   previous placement left and the next multiple of B, the sender MUST pad
   forward to that multiple and place the block there. Fragment i occupies
   bytes from p + i × F for that fragment's length. The span is (k − 1) × F
   plus the last fragment's length rounded up to a multiple of 16, and MUST NOT
   exceed B. A fragment's ring field is (p + i × F) modulo B divided by 16. A
   receiver MUST compute the block's base as ring − fragment × (F / 16) and
   MUST refuse the payload when the base is negative or when the base in bytes
   plus the span exceeds B ([m4 Receiver checks](#m4)). A receiver that has
   not seen the last fragment MUST use 16 bytes as the last fragment's rounded
   length for that check, the shortest block the fragment could belong to.
5. **Receive, completion and delivery.** A receiver MUST copy each fragment's
   data to ring + fragment × F, by index and not by arrival. When all k
   fragments have arrived, a receiver MUST deliver the block exactly once and
   whole, at its one ordering slot, as fragments 0 to k − 1 in order. A
   receiver MUST NOT deliver an incomplete block. A receiver MUST count a
   duplicate fragment once. A receiver MUST refuse a fragment whose fragment
   count, first number or base disagrees with the block's first fragment.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| collecting | a fragment arrives | no block state exists, or its count, first number and base agree | copy the fragment to ring + fragment × F and count it | collecting |
| collecting | a fragment arrives | a block state exists and the count, first number or base disagrees | refuse the payload as a violation | connection ends |
| collecting | a duplicate fragment arrives | its number was already counted | count it once and change nothing | collecting |
| collecting | the last missing fragment arrives | every fragment of the block has arrived | deliver the block whole at its ordering slot | complete |
| complete | any | | ignore the event | complete |

An event a state does not list is ignored in that state.

**Vectors.** Each vector is a RECORDS payload before sealing. The split
vectors are fragments 1 and 2 of a three-fragment block of 2404 bytes:
fragments 0 and 1 carry 1152 bytes and fragment 2 carries 100. The ring-wrap
vector is two RECORDS payloads back to back, fragment 0 and then fragment 1 of
a two-fragment block of 1153 bytes placed after the ring was filled to 1152
bytes of its 1168-byte ring, so the span is padded to the boundary and the
fragments' ring fields are 0 and 72.

```vector m8.split-nonfinal
block bytes: 2404
fragment bytes: 1152
fragment index: 1
fragment count: 3
number: 10
ring: 72
ordering: 7
channel: 3
0b 80 04 0a 00 00 48 00 07 00 00 03 01 00 02 00
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
01
```

```vector m8.final-short
block bytes: 2404
fragment bytes: 100
fragment index: 2
fragment count: 3
number: 11
ring: 144
ordering: 7
channel: 3
0b 64 00 0b 00 00 90 00 07 00 00 03 02 00 02 00
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 01
```

```vector m8.ring-wrap
ring bytes: 1168
prior bytes: 1152
block bytes: 1153
payloads: 2
first number: 1
fragment 0 ring: 0
fragment 1 ring: 72
0b 80 04 01 00 00 00 00 01 00 00 00 00 00 01 00
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f
90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf
c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf
d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df
e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef
f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f
30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f
40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f
50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f
60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f
70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f
01 0b 01 00 02 00 00 48 00 01 00 00 00 01 00 01
00 00 01
```

<a id="m9"></a>
## m9. Acknowledgement mapping, resend and send budget

### m9.1. Acknowledgement mapping

The connection layer reports which of a sender's packets the receiver
acknowledged. The message layer keeps its own reliable numbering, so it MUST
translate each report into the reliable numbers and receipt serials that
packet carried.

A sender MUST keep a sent-packet map from the connection layer's sequence of
each send to the reliable numbers and receipt serials that send carries. Every
send, a resend included, MUST take a fresh sequence, so an acknowledgement
names exactly one send. A sender MUST ignore an acknowledgement whose sequence
the map does not hold.

The map MUST hold at least `W` entries, where `W` is the connection's
`reliable_messages_in_flight`. Every entry names a send carrying at least one
reliable number, and a window of `W` numbers may be in flight one to a send,
all written inside one round trip; a smaller map would drop an entry whose
acknowledgement is still on its way, and the numbers it carried would be
resent. A sender whose map is full MUST drop its oldest entry to record a new
send; the numbers the dropped entry carried stay unacknowledged and in their
resend queues. With at least `W` entries and a resend delay above the round
trip, the dropped entry is one whose acknowledgement can no longer arrive.

On an acknowledgement, for each number the packet carried that is still
unacknowledged, the sender MUST mark the number acknowledged, remove it from
its resend queue, advance the lowest unacknowledged number, and take one
round-trip sample equal to the time between the send and the acknowledgement.
A number that was already acknowledged MUST NOT be acknowledged or sampled a
second time.

### m9.2. Round-trip estimate and resend delay

The sender MUST estimate the round-trip time as follows. On the first
sample, it MUST set both the smoothed round-trip time and deviation to the
sample. On every later sample, the deviation MUST move 5 percent toward
the absolute difference between the prior smoothed RTT and the new sample;
the smoothed RTT MUST then move 5 percent toward the new sample. The resend delay is

    clamp( 2 x srtt + 4 x deviation + 30 ms, message_resend_delay_min_ms, message_resend_delay_max_ms )

where srtt and deviation are in seconds. The timeout MUST be 2 seconds before
the first sample by default. The minimum defaults to 0, which applies no floor;
a caller may configure a nonzero minimum. The maximum
defaults to 2 seconds. The timeout MUST
NOT back off after a resend: every pass uses the same function of the
estimates, so a record that is never acknowledged is resent at that delay
until the reliable timeout ends the connection.

| Name | Value | Unit | Meaning |
|---|---|---|---|
| message_resend_delay_initial_ms | 2000 | ms | the timeout before the first round-trip sample |
| message_resend_delay_min_ms | 0 | ms | optional floor for the sampled resend delay; zero disables it |
| message_resend_delay_max_ms | 2000 | ms | the configured largest timeout |
| reliable_message_timeout_ms | 10000 | ms | how long the oldest unacknowledged number may stand |
| min_resend_attempts | 4 | count | the sends that fit before the reliable timeout |

### m9.3. Resend classes and the reliable timeout

A sender MUST keep two resend queues. A single-packet reliable record is in the
small-record queue; a fragment of a block is in the chunk queue. A number MUST
be in at most one queue.

The resend delay of a single-packet record MUST be resend_delay. The resend delay of
a chunk MUST be min( resend_delay, reliable_message_timeout_ms / min_resend_attempts ). A sender MUST resend a due number
at the first update whose time is at least the number's last send plus its
resend delay, and MUST NOT resend it before then. A resend MUST use a fresh
connection layer sequence and MUST move the number to the tail of its queue.

The sender MUST end the connection with ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT
when the oldest unacknowledged reliable number has stood unacknowledged for
reliable_message_timeout_ms or more, measured from its first send. A connection that
ends with ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT MUST stop resending.

### m9.4. Send budget

Every connection MUST send at most its send budget, a rate in kilobits per
second where 1 kbps is 1000 bit/s. The bytes counted MUST be the wire bytes of
each sealed packet: the prefilter header, sequence, acknowledgement block,
payload and tag, excluding the IP and UDP headers. A budget of K kbps is
K x 125 bytes per second.

The sender MUST hold the budget in a token bucket. The bucket starts full. It
refills at the budget rate multiplied by the elapsed time since its last
refill, and its contents MUST NOT exceed its depth. The depth MUST be the
larger of 100 ms of the budget and one maximum packet, so that one maximum
packet always fits an idle bucket and a stalled update releases at most 100 ms
of budget at once. An update at or before the last refill time MUST NOT add
tokens and MUST NOT move the refill time backwards.

Within one update the sender MUST offer its items in this order: retransmits
and ordered progress first, then new reliable records, then unreliable
records. A reliable item that does not fit MUST wait for a later update and
MUST NOT be dropped. Once a reliable item waits, every later item in that
update MUST NOT spend the bucket. An unreliable item that does not fit MUST be
dropped and counted as MESSAGE_UNRELIABLE_OVER_BUDGET. An item offered after a
lower-priority class in the same update MUST be counted as an order refusal; a
reliable item among them waits rather than spending bytes promised to the
earlier class.

| Name | Value | Unit | Meaning |
|---|---|---|---|
| `client_default_kbps` | `1000` | kbps | the send budget of a new client connection |
| `server_default_kbps` | `10000` | kbps | the send budget of a new server connection to one client |
| `send_kbps_min` | `1` | kbps | the smallest enabled budget |
| `send_kbps_max` | `2147483646` | kbps | the largest budget |
| `depth_seconds` | `0.1` | s | the bucket's time depth |
| `maximum_packet_wire_bytes` | `1452` | bytes | one maximum packet, the smallest bucket depth |

The client's budget to the server defaults to client_default_kbps and the
server's budget to each client defaults to server_default_kbps. The value
2147483647, one above send_kbps_max, means no send budget. A negative value
MUST be refused and MUST leave the budget unchanged. A budget of 0 sets that direction's default
budget. A budget of send_kbps_max is the ceiling and is NOT RECOMMENDED.

A setter MUST refill at the old rate up to its time argument, then apply the
new rate and depth from that time, keeping the bucket's bytes capped at the new
depth. The budget change is visible when the setter returns success.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Open | a reliable item is offered | it fits and no reliable item waits | spend its bytes | Open |
| Open | a reliable item is offered | it does not fit, or a reliable item waits | keep it for a later update | Waiting |
| Open | an unreliable item is offered | it fits and no reliable item waits | spend its bytes | Open |
| Open | an unreliable item is offered | it does not fit, or a reliable item waits | drop it and count it | Open |
| Waiting | any item is offered | | an unreliable item is dropped and counted; a reliable item waits | Waiting |
| Waiting | the update ends | | | Open |
| Any | an item is offered after a lower-priority class | | count the order refusal; a reliable item waits | Waiting |

### m9.5. Vectors

```vector m9.resend-trace
clock: 1000.0
trace: short path: a 20 ms sample, resend passes at 0.040, 0.109, 0.111 and 0.221 seconds, reliable timeout checks at 9.999 and 10.000 seconds; long path: a 400 ms sample, resend passes at 0.400, 1.629, 1.631 and 2.001 seconds
00 00 00 01 00 01 04 00 00 00 01
```

The first seven bytes are the short path's resend counts at its four passes,
then the timeout checks at 9.999 and 10.000 seconds, then the error value. The
last three are the long path's resend counts.

```vector m9.budget-trace
clock: 1000.0
rate: the client default
trace: a reliable 1452-byte item, seven unreliable 1452-byte items, then one more unreliable item; no time passes, a reliable 1452-byte item waits and an unreliable 100-byte item is dropped; 100 ms of refill, a reliable 1452-byte item; the rate is set to 2000 kbps; 100 ms of refill at the new rate, a reliable 1452-byte item; the budget is set to 0, then one 13000-byte unreliable sequenced item is offered, promoted to reliable sequenced and its first packet written
00 00 00 00 00 00 00 00 02 01 02 00 01 00 01 01
00 01 01 01
```

The first fourteen bytes are the decision for each offer in order: SEND,
WAIT, DROP and REFUSED are 0, 1, 2 and 3; the setter's success is 1. They are
the reliable item, the seven unreliable items that fit, the unreliable item
that is dropped, the reliable item that waits, the unreliable item behind it
that is dropped, the refill's reliable item, the setter and the new-rate
reliable item. The last six bytes are, in order, the setter to 0 returning
success, a packet request of more than zero, the first packet written larger
than one maximum packet (0: the promoted item is a reliable block, sent one
maximum packet at a time), the unreliable queue empty, no bytes charged to the
budget and no unreliable item dropped.

<a id="m10"></a>
## m10. Channels and priorities

Channels are per-connection streams. Their scheduling does not affect the
wire representation.

1. An implementation MUST set its build-specific maximum channel count to a
   value from 1 through 65,536, inclusive. The default build-specific maximum
   MUST be 32. A connection MUST set num_channels from 1 through its
   implementation build-specific maximum. The channel of a send is its
   ordering channel. For a send of reliability 1, 3, 4 or 7, a sender MUST
   select a channel from zero through one less than that connection's
   num_channels, and a message endpoint MUST refuse such a send with a channel
   outside that range. A send of reliability 0, 2, 5 or 6 is neither ordered
   nor sequenced: a message endpoint MUST ignore its channel, MUST NOT refuse
   it for its channel, and sends it with no channel field (rule 2): the channel
   orders only the ordered and sequenced reliabilities
   (`rn_message_object_start` in `sdk/rocketnet_message_server.cpp`).
2. A record with code 1, 3 or 4 MUST carry the channel field in
   [m3 Record format](#m3). Receipt types use their base codes from
   [m1.3 Reliability types](#m1.3): type 7 uses code 3 and carries a channel;
   types 5 and 6 use codes 0 and 2 and carry none. A promoted type-5 send uses
   code 2 and carries no channel.
3. A sender MUST encode a channel that fits in u8 as one byte. A sender MUST
   encode a larger channel as a little-endian u16 and MUST set WIDE if and only
   if it uses that u16 encoding. A receiver MUST reject WIDE when the channel
   fits in u8.
4. A receiver MUST hand the application its connection's deliveries as one
   queue in the update's delivery order, every channel's together, and MUST
   NOT filter them by channel. A server MUST hand them as one queue across all
   its connections, each message naming the connection it arrived on. Order
   holds within a channel of a connection as rules
   [m6 Delivery: unreliable codes 0 and 1](#m6) and
   [m7 Delivery: reliable codes 2, 3 and 4](#m7) state it; the queue makes no order between channels or between
   connections. The SDK's queue is each connection's deliveries in turn, in
   connection index order.

The diagrams show the channel field alone. Its position in a record is the
position defined by its code in [m3 Record format](#m3).

```
Offset 0
        0 1 2 3 4 5 6 7
       +-+-+-+-+-+-+-+-+
NARROW |    channel    |
       +-+-+-+-+-+-+-+-+

Offset 0               1
        0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
WIDE   |      channel, little-endian   |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---:|---:|---|---|---|
| channel, narrow | 0 | 1 | u8 | channel number | any u8 channel number |
| channel, wide | 0 | 2 | u16 | channel number | any u16 channel number |

| Name | Value | Units | Meaning |
|---|---:|---|---|
| channel wide boundary | `256` | channels | first channel encoded as WIDE |
| default maximum channel count | `32` | channels | SDK default for the build-specific maximum |
| maximum allowed build-specific channel count | `65536` | channels | largest build-specific maximum permitted |

```vector m10.narrow-channel
record: RELIABLE_ORDERED, channel 3, number 5, ring 0, ordering 0, data "hi"
03 02 00 05 00 00 00 00 00 00 00 03 68 69 01
```

```vector m10.wide-channel
record: RELIABLE_ORDERED, channel 300 (WIDE), number 5, ring 0, ordering 0, data "hi"
configuration: build-specific maximum at least 301; num_channels 301
13 02 00 05 00 00 00 00 00 00 00 2c 01 68 69 01
```

<a id="m11"></a>
## m11. CONFIG control record

1. Each endpoint MUST send exactly one CONFIG record for each connection and
   MUST NOT send another.
2. An endpoint MUST send the CONFIG record before any other reliable record of
   that connection, and MUST resend it until the connection layer acknowledges
   it ([m7 Delivery: reliable codes 2, 3 and 4](#m7)).
3. An endpoint MUST start its CONFIG wait at the first update at which its
   connection layer reports the connection established.
4. An endpoint MUST end the connection with `RELIABLE_TIMEOUT` when an update
   is at or after the start of the CONFIG wait plus the reliable timeout
   ([m9 Acknowledgement mapping, resend and send budget](#m9)) and it has not
   accepted the remote endpoint's CONFIG.
5. An endpoint MUST accept a CONFIG record only when its body length is the
   body size row of [m11.2 Constants and defaults](#m11.2), its kind is its kind row, and, when its
   version is its version row, both reserved fields are zero.
6. An endpoint MUST refuse every other CONFIG record as `MESSAGE_BAD_CONTROL`,
   and MUST end the connection with `PROTOCOL_VIOLATION` when it does.
7. An endpoint MUST read a body whose version it does not know and MUST
   compare it; such a body is a CONFIG mismatch and never
   `MESSAGE_BAD_CONTROL`.
8. An endpoint MUST compare the `version`, `enabled`, `num_channels`,
   `reliable_messages_in_flight`, `receive_message_buffer_bytes`, `max_message_bytes` and
   `fragment_bytes` fields of the body with its own values.
9. An endpoint MUST end the connection with `CONFIG_MISMATCH` when any
   compared field differs.
10. An endpoint MAY receive message records before the remote endpoint's CONFIG, and
    when it does it MUST process them against its own configuration.

<a id="m11.1"></a>
### m11.1. Wire layout

The CONFIG record is a code 2 record with the CONTROL flag set, inside a
payload of the RECORDS form. Its record header carries the connection's first
reliable number. The payload is 35 bytes:

```
       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |     flags     |            length             |    number     |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |            number             |     kind      |    version    |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |    enabled    |     zero      |         num_channels          |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |         num_channels          |        reliable_messages_in_flight        |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |        reliable_messages_in_flight        |      receive_message_buffer_bytes       |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |      receive_message_buffer_bytes       |       max_message_bytes       |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |       max_message_bytes       |        fragment_bytes         |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |        fragment_bytes         |             zero              |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |             zero              |     form      |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| flags | 0 | 1 | u8 | the record's code and flags | `0x22` |
| length | 1 | 2 | u16 | the body length | `28` |
| number | 3 | 3 | u24 | the connection's first reliable number | the first reliable number |
| kind | 6 | 1 | u8 | the record kind | `1` |
| version | 7 | 1 | u8 | the body version | `1` |
| enabled | 8 | 1 | u8 | bit r set: reliability r may be sent by the application | a mask of enabled reliabilities |
| reserved byte | 9 | 1 | zero[1] | reserved | `0` |
| num_channels | 10 | 4 | u32 | the count of channels | the sender's configured count |
| reliable_messages_in_flight | 14 | 4 | u32 | the reliable window W | the sender's configured window |
| receive_message_buffer_bytes | 18 | 4 | u32 | the receive ring B | the sender's configured ring size |
| max_message_bytes | 22 | 4 | u32 | the largest message | the sender's configured maximum |
| fragment_bytes | 26 | 4 | u32 | the block stride F | the fragment bytes row of [m5 Message numbering and the reliable window](#m5) |
| reserved tail | 30 | 4 | zero[4] | reserved | `0` |
| form | 34 | 1 | u8 | the payload form | `1` |

<a id="m11.2"></a>
### m11.2. Constants and defaults

The library's default configuration is the one below, with the window and
ring defaults and the fragment stride in the [m5 Message numbering and the reliable window](#m5)
rows window messages default, receive ring bytes default and fragment bytes. Every field of the
body except the reserved fields is compared, so two ends whose configurations
differ in any compared field end the connection with `CONFIG_MISMATCH`.

| Name | Value | Unit |
|---|---|---|
| flags | `0x22` | none |
| body size | `28` | bytes |
| kind | `1` | none |
| version | `1` | none |
| enabled default | `0xFF` | mask |
| num channels default | `32` | channels |
| max message bytes default | `1048576` | bytes |
| form | `1` | none |

`enabled` with bit r set permits the application to send reliability r; the
default mask sets every one of the eight reliabilities
([m1.3 Reliability types](#m1.3)).

<a id="m11.3"></a>
### m11.3. States

An endpoint has one CONFIG state per connection.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Waiting | CONFIG body accepted | every compared field is equal | the CONFIG wait ends | Verified |
| Waiting | CONFIG body accepted | a compared field differs | end the connection with `CONFIG_MISMATCH` | none |
| Waiting | CONFIG record refused | the body is malformed | refuse it as `MESSAGE_BAD_CONTROL` and end the connection with `PROTOCOL_VIOLATION` | none |
| Waiting | update | at or after the start of the CONFIG wait plus the reliable timeout | end the connection with `RELIABLE_TIMEOUT` | none |
| Verified | CONFIG body accepted | every compared field is equal | no change | Verified |
| Verified | CONFIG body accepted | a compared field differs | end the connection with `CONFIG_MISMATCH` | none |
| Verified | CONFIG record refused | the body is malformed | refuse it as `MESSAGE_BAD_CONTROL` and end the connection with `PROTOCOL_VIOLATION` | none |

An event that a state does not list is ignored in that state.

<a id="m11.4"></a>
### m11.4. Vectors

Each vector is the full payload, before the connection layer seals it. An
implementation MUST reproduce each one from the CONFIG record and MUST parse
each one back to the same record and body. The first vector is the CONFIG
record at the library's defaults, with number 0. Each later vector is the
defaults with exactly one field changed; a receiver at the defaults MUST
accept the first vector and MUST end the connection with `CONFIG_MISMATCH`
for each of the others.

```vector m11.config-defaults
config: library defaults
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04
00 00 00 00 10 00 00 00 10 00 80 04 00 00 00 00
00 00 01
```

```vector m11.version-mismatch
field: version
value: 2
22 1c 00 00 00 00 01 02 ff 00 20 00 00 00 00 04
00 00 00 00 10 00 00 00 10 00 80 04 00 00 00 00
00 00 01
```

```vector m11.enabled-mismatch
field: enabled
value: 0x01
22 1c 00 00 00 00 01 01 01 00 20 00 00 00 00 04
00 00 00 00 10 00 00 00 10 00 80 04 00 00 00 00
00 00 01
```

```vector m11.num-channels-mismatch
field: num_channels
value: 16
22 1c 00 00 00 00 01 01 ff 00 10 00 00 00 00 04
00 00 00 00 10 00 00 00 10 00 80 04 00 00 00 00
00 00 01
```

```vector m11.window-messages-mismatch
field: reliable_messages_in_flight
value: 512
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 02
00 00 00 00 10 00 00 00 10 00 80 04 00 00 00 00
00 00 01
```

```vector m11.receive-ring-bytes-mismatch
field: receive_message_buffer_bytes
value: 131072
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04
00 00 00 00 02 00 00 00 10 00 80 04 00 00 00 00
00 00 01
```

```vector m11.max-message-bytes-mismatch
field: max_message_bytes
value: 131072
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04
00 00 00 00 10 00 00 00 02 00 80 04 00 00 00 00
00 00 01
```

```vector m11.fragment-bytes-mismatch
field: fragment_bytes
value: 1024
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04
00 00 00 00 10 00 00 00 10 00 00 04 00 00 00 00
00 00 01
```

<a id="m12"></a>
## m12. Receipts

Types 5, 6 and 7 request an outcome for a send. The type is an API value; the
wire carries its base code, except that a large type-5 send uses the reliable
promotion defined here and in [m2 Payload forms and sender size rules](#m2).

1. A sender MUST NOT put receipt types 5, 6 or 7 on the wire. Types 5, 6 and 7
   use codes 0, 2 and 3, respectively, as [m1.3 Reliability types](#m1.3)
   defines.
2. After applying limits in [m2 Payload forms and sender size rules](#m2),
   a sender MUST use the reliable code-2 path for a permitted
   type-5 send whose data exceeds the row `UNRELIABLE record limit` of
   [m2.2 Constants](#m2.2). The sender MUST preserve the type-5 receipt for
   that send. The receiver sees code 2; the sender reports ACKED only after
   every block fragment is acknowledged.
3. A type-6 or type-7 receipt reports ACKED when all reliable numbers for its
   send are acknowledged. A type-5 send that remains unreliable reports ACKED
   when its carrying packet is acknowledged. It reports LOSS when it is
   refused by the send budget or by the transport, at once, since it was
   never sent; and otherwise only when no acknowledgement arrived by its
   receipt deadline. The receipt deadline is the packet's send time plus the
   sender's maximum resend delay (`message_resend_delay_max_ms`), a fixed
   bound that does not move with the adaptive resend delay. A sender MUST NOT
   report LOSS for a sent type-5 message before its receipt deadline, with
   one exception: when the sender's sent-packet map evicts the entry that
   carries the message, the message can no longer be acknowledged and the
   sender reports LOSS at the eviction. These outcomes are API semantics and
   are not carried on the wire.

```vector m12.promotion
public type: 5 (unreliable with ack receipt)
data length: one byte above the UNRELIABLE record limit
data: 0x41 in every byte
packet: second payload of the promoted code-2 block
0a 04 01 01 00 00 48 00 01 00 01 00 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
41 41 41 41 41 41 41 41 41 41 41 41 41 41 41 41
01
```

<a id="m13"></a>
## m13. Security considerations

The transport seals every payload; this section bounds what the remote endpoint can do
within the message layer of a sealed connection.

1. **Violations end the connection.** A payload that breaks a rule of RN-2 is
   a violation. A receiver MUST refuse the whole payload, MUST NOT hand up any
   record of it, and MUST end the connection. A correct sender never sends a
   violation, so one is evidence of a faulty or hostile sender. Each violation has
   its own counter.
2. **No live growth.** Every table the remote endpoint can influence is fixed at connection
   creation from configuration. A receiver MUST NOT allocate memory after
   connection creation, so a sender MUST NOT be able to make a receiver hold more
   than the configured region. The checks of [m4 Receiver checks](#m4) run
   before a record is held or delivered.
3. **Bounds.** A record whose channel, number, span or size falls outside the
   configured values, the window or the ring is refused by
   [m4 Receiver checks](#m4), [m5 Message numbering and the reliable window](#m5)
   and [m8 Blocks](#m8), never stored. A receiver MUST NOT read outside the byte
   range the transport delivers.
4. **Amplification.** The checks run over a payload once. A receiver MUST
   complete them in time bounded by the payload's own length, with no
   allocation, so a sender MUST NOT be able to grow a receiver's work or memory.

<a id="ma"></a>
## ma. Appendix A: test vectors

The index of the vectors of RN-2 by section is printed by
`python3 tools/conformance/check_rn_standard.py --print-index`; the bytes are
in the sections.

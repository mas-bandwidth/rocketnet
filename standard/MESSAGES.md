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
   channel and the ordering index.
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
2. **Split.** The block stride F is the row `block stride` of the table below.
   A block of L bytes MUST have k = ceil(L / F) fragments, with k at least 2.
   Fragments 0 to k − 2 MUST carry exactly F bytes each; fragment k − 1 MUST
   carry from 1 to F bytes.
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

| Name | Value | Units |
|---|---|---|
| block stride | `1152` | bytes |

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
| [m7 Delivery: reliable codes 2, 3 and 4](#m7) | `m7.reliable-loss-reorder` |
| [m7 Delivery: reliable codes 2, 3 and 4](#m7) | `m7.reliable-ordered-hold-drain` |
| [m7 Delivery: reliable codes 2, 3 and 4](#m7) | `m7.reliable-sequenced-hold-drain` |
| [m8 Blocks](#m8) | `m8.final-short` |
| [m8 Blocks](#m8) | `m8.ring-wrap` |
| [m8 Blocks](#m8) | `m8.split-nonfinal` |
<!-- vector-index:end -->

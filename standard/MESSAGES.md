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

Status: pending

Scope: per-channel state, the four priority levels, round robin across
channels, FIFO within a channel, and the packet writer's order.

<a id="m11"></a>
## m11. CONFIG control record

Each endpoint MUST send exactly one CONFIG record for each connection and MUST
NOT send another. It MUST send the CONFIG record before any other reliable
record of that connection, and it resends the CONFIG record until the
connection layer acknowledges it ([m7 Delivery: reliable codes 2, 3 and 4](#m7)).
An endpoint MUST start its CONFIG wait at the first update at which its
connection layer reports the connection established.

An endpoint MUST end the connection with `RELIABLE_TIMEOUT` when an update is
at or after the start of the CONFIG wait plus the reliable timeout
([m9 Acknowledgement mapping, resend and send budget](#m9)) and it has not
accepted the peer's CONFIG.

An endpoint MUST accept a CONFIG record only when its body is 28 bytes, its
kind is 1, and, when its version is 1, both reserved fields are zero. An
endpoint MUST refuse every other CONFIG record as `MESSAGE_BAD_CONTROL`. An
endpoint MUST read a body whose version it does not know and MUST compare it,
such a body is a CONFIG mismatch and never `MESSAGE_BAD_CONTROL`.

An endpoint MUST compare the `version`, `enabled`, `num_channels`,
`window_messages`, `receive_ring_bytes`, `max_message_bytes` and
`fragment_bytes` fields of the body with its own values. An endpoint MUST end
the connection with `CONFIG_MISMATCH` when any compared field differs. An
endpoint MAY receive message records before the peer's CONFIG and MUST
process them against its own configuration.

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
|    number     |     kind      |   version     |    enabled    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     zero      |                  num_channels                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  window_messages                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  receive_ring_bytes                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  max_message_bytes                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  fragment_bytes                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  zero                         |     form      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
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
| window_messages | 14 | 4 | u32 | the reliable window W | the sender's configured window |
| receive_ring_bytes | 18 | 4 | u32 | the receive ring B | the sender's configured ring size |
| max_message_bytes | 22 | 4 | u32 | the largest message | the sender's configured maximum |
| fragment_bytes | 26 | 4 | u32 | the block stride F | `1152` |
| reserved tail | 30 | 4 | zero[4] | reserved | `0` |
| form | 34 | 1 | u8 | the payload form | `1` |

### m11.2. Constants and defaults

The library's default configuration is the one below. Every field of the body
except the reserved fields is compared, so an implementation MUST use the
same defaults or the two ends end the connection with `CONFIG_MISMATCH`.

| Name | Value | Unit |
|---|---|---|
| flags | `0x22` | none |
| body size | `28` | bytes |
| kind | `1` | none |
| version | `1` | none |
| enabled default | `0xFF` | mask |
| num channels default | `32` | channels |
| window messages default | `1024` | messages |
| receive ring bytes default | `262144` | bytes |
| max message bytes default | `262144` | bytes |
| fragment bytes | `1152` | bytes |
| form | `1` | none |

`enabled` with bit r set permits the application to send reliability r; the
default mask sets every one of the eight reliabilities
([m1.3 Reliability types](#m1.3)).

### m11.3. States

An endpoint has one CONFIG state per connection.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Waiting | CONFIG body accepted | every compared field is equal | the CONFIG wait ends | Verified |
| Waiting | CONFIG body accepted | a compared field differs | end the connection with `CONFIG_MISMATCH` | none |
| Waiting | CONFIG record refused | the body is malformed | refuse it as `MESSAGE_BAD_CONTROL`; the violation ends the connection | none |
| Waiting | update | at or after the start of the CONFIG wait plus the reliable timeout | end the connection with `RELIABLE_TIMEOUT` | none |
| Verified | CONFIG body accepted | every compared field is equal | no change | Verified |
| Verified | CONFIG body accepted | a compared field differs | end the connection with `CONFIG_MISMATCH` | none |
| Verified | CONFIG record refused | the body is malformed | refuse it as `MESSAGE_BAD_CONTROL`; the violation ends the connection | none |

An event that a state does not list is ignored in that state.

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
00 00 00 00 04 00 00 00 04 00 80 04 00 00 00 00
00 00 01
```

```vector m11.version-mismatch
field: version
value: 2
22 1c 00 00 00 00 01 02 ff 00 20 00 00 00 00 04
00 00 00 00 04 00 00 00 04 00 80 04 00 00 00 00
00 00 01
```

```vector m11.enabled-mismatch
field: enabled
value: 0x01
22 1c 00 00 00 00 01 01 01 00 20 00 00 00 00 04
00 00 00 00 04 00 00 00 04 00 80 04 00 00 00 00
00 00 01
```

```vector m11.num-channels-mismatch
field: num_channels
value: 16
22 1c 00 00 00 00 01 01 ff 00 10 00 00 00 00 04
00 00 00 00 04 00 00 00 04 00 80 04 00 00 00 00
00 00 01
```

```vector m11.window-messages-mismatch
field: window_messages
value: 512
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 02
00 00 00 00 04 00 00 00 04 00 80 04 00 00 00 00
00 00 01
```

```vector m11.receive-ring-bytes-mismatch
field: receive_ring_bytes
value: 131072
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04
00 00 00 00 02 00 00 00 04 00 80 04 00 00 00 00
00 00 01
```

```vector m11.max-message-bytes-mismatch
field: max_message_bytes
value: 131072
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04
00 00 00 00 04 00 00 00 02 00 80 04 00 00 00 00
00 00 01
```

```vector m11.fragment-bytes-mismatch
field: fragment_bytes
value: 1024
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04
00 00 00 00 04 00 00 00 04 00 00 04 00 00 00 00
00 00 01
```

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
| [m11 CONFIG control record](#m11) | `m11.config-defaults` |
| [m11 CONFIG control record](#m11) | `m11.enabled-mismatch` |
| [m11 CONFIG control record](#m11) | `m11.fragment-bytes-mismatch` |
| [m11 CONFIG control record](#m11) | `m11.max-message-bytes-mismatch` |
| [m11 CONFIG control record](#m11) | `m11.num-channels-mismatch` |
| [m11 CONFIG control record](#m11) | `m11.receive-ring-bytes-mismatch` |
| [m11 CONFIG control record](#m11) | `m11.version-mismatch` |
| [m11 CONFIG control record](#m11) | `m11.window-messages-mismatch` |
<!-- vector-index:end -->

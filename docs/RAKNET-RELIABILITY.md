# RakNet's reliability layer, read for RocketNet

A study of how RakNet delivers its eight reliabilities, written so that the
engineer who starts RocketNet's message layer tomorrow can build UNRELIABLE
and RELIABLE_ORDERED first without cornering the other six, and so that the
customer's RakNet-shaped C# shim has a surface to land on.

Every fact below comes from the code, cited as `file:line` against the
facebookarchive/RakNet tree at commit `1a16989` (the only public tree; it has
not moved since 2014). Paths are under `Source/`. Where RakNet's comments and
its code disagree, the disagreement is called out in a **Code vs comment**
note; the code is what ships. RocketNet facts cite `USAGE.md` and
`sdk/rocketnet.h` in this repository.

Vocabulary, fixed for this document:

- **message**: one user `Send()`; RakNet's `InternalPacket`.
- **datagram**: one UDP payload; carries one datagram header and one or more
  messages, or an ack/NAK range list instead.
- **channel**: RakNet's "ordering channel" / "ordered stream", 0..31.

---

## 1. The eight reliabilities

`PacketPriority.h:46-85` declares the enum. Two more (sequenced with receipt)
are commented out at `:71` and `:81` with the reason at `:70`: "You can't have
sequenced and ack receipts, because you don't know if the other system
discarded the message." The eight that exist, in enum order (the order is
load-bearing; `:45` "Do not reorder, I check on >= UNRELIABLE_WITH_ACK_RECEIPT"):

| # | Reliability | Resent | Order kept | Older dropped | Receipt | Wire type sent as |
|---|---|---|---|---|---|---|
| 0 | UNRELIABLE | no | no | no | no | 0 |
| 1 | UNRELIABLE_SEQUENCED | no | per channel, newest wins | yes | no | 1 |
| 2 | RELIABLE | yes | no | no | no | 2 |
| 3 | RELIABLE_ORDERED | yes | per channel, all delivered | no | no | 3 |
| 4 | RELIABLE_SEQUENCED | yes | per channel, newest wins | yes | no | 4 |
| 5 | UNRELIABLE_WITH_ACK_RECEIPT | no | no | no | ACKED or LOSS | 0 |
| 6 | RELIABLE_WITH_ACK_RECEIPT | yes | no | no | ACKED only | 2 |
| 7 | RELIABLE_ORDERED_WITH_ACK_RECEIPT | yes | per channel | no | ACKED only | 3 |

The last column is the first fact that shapes a design: **the receiver never
learns that a receipt was requested.** `WriteToBitStreamFromInternalPacket`
maps the three receipt types onto their plain twins before writing the 3-bit
reliability field (`ReliabilityLayer.cpp:2619-2628`), and the reader's
reliable-number branch lists only RELIABLE, RELIABLE_SEQUENCED and
RELIABLE_ORDERED with the comment "I don't write ACK_RECEIPT to the remote
system" (`:2709-2718`). Receipts are a sender-side bookkeeping feature over
the acks the receiver already sends.

**Code vs comment.** `PacketPriority.h:48` says UNRELIABLE "will also discard
duplicate datagrams" and costs "16 [bits] ... to detect duplicate packets".
The code assigns a message number only to reliable messages
(`ReliabilityLayer.cpp:2046-2061`), writes it only for reliable types
(`:2634-2640`), and runs the duplicate check only when the reliability is
RELIABLE, RELIABLE_SEQUENCED or RELIABLE_ORDERED (`:941`). A duplicated
UNRELIABLE message is delivered twice. (Datagram numbers are checked by the
congestion manager at `CCRakNetSlidingWindow.cpp:137-158`, but an
out-of-order or repeated datagram number returns `true` with
`skippedMessageCount=0` at `:155-158` and the datagram is still parsed.)

**Code vs comment.** `PacketPriority.h:48` says the overhead of UNRELIABLE is
"(6 to 17) + 21 bits". The actual header is byte-aligned: 1 byte flags
(3 bits reliability, 1 bit has-split, padded) + 2 bytes bit-length, so 3
bytes for UNRELIABLE (`:2618-2633`; `GetMessageHeaderLengthBits` at
`:2562-2567` agrees: `8*1 + 8*2`).

### 1.1 Common per-connection state

One `ReliabilityLayer` per remote system (`RakPeer.cpp:5460`, `:5871`), reset
on connect by `Reset` (`ReliabilityLayer.cpp:345-363`). `InitializeVariables`
(`:384-457`) zeroes:

| Field | Type | Side | Purpose | Lines |
|---|---|---|---|---|
| `orderedWriteIndex[32]` | uint24 | sender | next ordering index per channel | `.h:442` |
| `sequencedWriteIndex[32]` | uint24 | sender | next sequencing index per channel | `.h:444` |
| `orderedReadIndex[32]` | uint24 | receiver | expected ordering index per channel | `.h:446` |
| `highestSequencedReadIndex[32]` | uint24 | receiver | highest sequence seen at current ordering index | `.h:448` |
| `orderingHeaps[32]` | min-heap of `InternalPacket*` | receiver | buffered out-of-order messages per channel | `.h:449` |
| `heapIndexOffsets[32]` | uint24 | receiver | base for heap weights per channel | `.h:450` |
| `sendReliableMessageNumberIndex` | uint24 | sender | next reliable message number | `.h:411` |
| `internalOrderIndex` | uint24 | sender | local send order, never on the wire | `.h:412`, `.cpp:1596` |
| `splitPacketId` | uint16 | sender | next split id | `.h:416` |
| `receivedPacketsBaseIndex` | uint24 | receiver | lowest reliable message number not yet seen | `.h:472` |
| `hasReceivedPacketQueue` | queue of bool | receiver | holes above the base index | `.h:471` |
| `resendBuffer[512]` | `InternalPacket*` | sender | reliable messages in flight, by message number & 511 | `.h:376`, `RakNetDefines.h:105-106` |
| `resendLinkedListHead` | circular list | sender | the same messages in retransmit order | `.h:377` |
| `unreliableLinkedListHead` | circular list | sender | unreliable messages waiting, for the unreliable timeout | `.h:378` |
| `outgoingPacketBuffer` | heap by priority weight | sender | messages not yet sent the first time | `.h:401` |
| `datagramHistory` | queue of up to 512 nodes | sender | datagram number to message numbers | `.h:352`, `RakNetDefines.h:98` |
| `unreliableWithAckReceiptHistory` | list | sender | (datagram number, receipt serial, deadline) | `.h:365` |
| `acknowlegements` | `RangeList<uint24>` | receiver | datagram numbers to ack | `.h:565` |
| `NAKs` | `RangeList<uint24>` | receiver | datagram numbers to NAK | `.h:566` |
| `splitPacketChannelList` | ordered list by split id | receiver | partial large messages | `.h:409` |
| `outputQueue` | queue of `InternalPacket*` | receiver | delivered messages awaiting `Receive()` | `.h:330` |
| `unacknowledgedBytes` | uint32 | sender | bytes in the resend list | `.h:528` |
| `congestionManager` | `CCRakNetSlidingWindow` | both | datagram numbers, cwnd, RTO | `.h:522` |

All indices are `uint24_t` (`InternalPacket.h:37-41`;
`CCRakNetSlidingWindow.h:73`), wrapping at 2^24 by masking in the increment
operators (`RakNetTypes.h:473-474`).

### 1.2 Per-message state (`InternalPacket.h:45-125`)

On the wire (`InternalPacketFixedSizeTransmissionHeader`, `:45-68`):
`reliableMessageNumber`, `orderingIndex`, `sequencingIndex`,
`orderingChannel`, `splitPacketId` (uint16), `splitPacketIndex` (uint32),
`splitPacketCount` (uint32), `dataBitLength`, `reliability`. Local only
(`:79-125`): `messageInternalOrder`, `messageNumberAssigned`, `creationTime`,
`nextActionTime`, `retransmissionTime`, `headerLength`, `data` plus an
allocation scheme (NORMAL / REF_COUNTED / STACK with a 128-byte `stackData`
at `:124`), `timesSent`, `priority`, `sendReceiptSerial`, and two pairs of
intrusive list links.

**Message numbers are assigned at first transmission, not at `Send()`.**
`Send` leaves `messageNumberAssigned=false` (`:1675`); `Update` sets the
number when the message is first pulled from `outgoingPacketBuffer`
(`:2060-2061`, `:2090`). The comment at `InternalPacket.h:83-85` gives the
reason: pre-assigning and then sending on a different priority "creates a huge
gap" that the receiver's hole queue has to walk.

### 1.3 Message header on the wire (`:2610-2677`, read at `:2682-2801`)

Byte-aligned fields, in order:

1. 1 byte: reliability in the top 3 bits (receipt types mapped to their plain
   twin), then 1 bit `hasSplitPacket`, then padding (`:2618-2631`).
2. 2 bytes: `dataBitLength` as uint16 (`:2633`). The assert at `:2632` bounds
   a message to under 65535 bits, 8191 bytes, but splitting keeps every
   message under the MTU anyway.
3. 3 bytes: `reliableMessageNumber`, only for wire types 2, 3, 4 (`:2634-2640`).
4. 3 bytes: `sequencingIndex`, only for wire types 1 and 4 (`:2643-2648`).
5. 3 bytes `orderingIndex` + 1 byte `orderingChannel`, for wire types 1, 3, 4
   (`:2650-2658`). The comment at `:2657` says "5 bits needed, write one
   byte".
6. If split: 4 bytes `splitPacketCount`, 2 bytes `splitPacketId`, 4 bytes
   `splitPacketIndex` (`:2660-2667`).
7. The data, byte aligned (`:2674`).

So the per-message header is 3 bytes (UNRELIABLE), 6 (RELIABLE), 10
(RELIABLE_ORDERED), 10 (UNRELIABLE_SEQUENCED), 13 (RELIABLE_SEQUENCED), plus
10 when split. The reader rejects a message when `dataBitLength==0`,
`reliability>=8`, `orderingChannel>=32` or `splitPacketIndex>=splitPacketCount`
(`:2760-2770`).

### 1.4 Datagram header (`DatagramHeaderFormat`, `:110-227`)

Three shapes, chosen by the first bits (`:143-183`): bit `isValid` (always
true), bit `isACK`; if ACK: bit `hasBAndAS`, align, optional float `AS`. Else
bit `isNAK`; if NAK nothing more. Else (data): bits `isPacketPair`,
`isContinuousSend`, `needsBAndAs`, align, 3-byte `datagramNumber`. With the
default sliding-window build the timestamp is compiled out
(`ReliabilityLayer.h:47-48`, `RakNetDefines.h:122`), so a data datagram
header is 4 bytes.

**Code vs comment.** `GetDataHeaderByteLength` (`:133-141`) reserves `2+3+4`
= 9 bytes for the header when computing the room left for messages
(`:3909-3919`), five more than the data header actually occupies. Harmless,
but a budget derived from this function over-counts.

---

## 2. The send path

### 2.1 `RakPeer::Send` (`RakPeer.cpp:1308-1352`)

Validates, picks a receipt serial (`:1326-1330`; `IncrementNextSendReceipt`
at `:1279-1287` skips zero so that zero can mean "assign one"), and calls
`SendBuffered` (`:1349`), which **copies the user data once** into a
`BufferedCommandStruct` (`:4194`, `:4206`) and queues it for the update
thread (`:4216`). IMMEDIATE_PRIORITY wakes that thread (`:4218-4222`). The
update thread drains the queue in `RunUpdateCycle` (`:5619-5641`) through
`SendImmediate` (`:4286-4384`), which calls `ReliabilityLayer::Send` for each
target with `makeDataCopy=false` for the last target so the buffered copy is
adopted rather than copied again (`:4362-4365`). Every reliable send also
stamps `lastReliableSend` (`:4367-4375`).

### 2.2 `ReliabilityLayer::Send` (`ReliabilityLayer.cpp:1539-1683`)

1. Bad parameters are **silently repaired**: a reliability above 7 becomes
   RELIABLE, a bad priority HIGH, a channel at or above 32 becomes channel 0
   (`:1557-1564`). Debug builds assert first (`:1541-1546`).
2. An `InternalPacket` comes from a per-connection pool (`:1571`;
   `AllocateFromInternalPacketPool`, pool page size 8 at
   `RakNetDefines.h:176`). Data up to 128 bytes lives in the packet's own
   `stackData` when copying (`:3860-3866`); otherwise `rakMalloc`.
3. The split decision: `numberOfBytesToSend > maxDataSizeBytes`, where
   `maxDataSizeBytes` = MTU minus UDP header minus datagram header minus the
   **largest possible** message header (RELIABLE_SEQUENCED with split, from
   `GetMaxMessageHeaderLengthBits` at `:2547-2553`), computed at `:1604-1606`.
4. **A split message is upgraded to reliable** (`:1609-1621`): UNRELIABLE to
   RELIABLE, UNRELIABLE_WITH_ACK_RECEIPT to RELIABLE_WITH_ACK_RECEIPT,
   UNRELIABLE_SEQUENCED to RELIABLE_SEQUENCED. The comment at `:1611` gives
   the reason: a missing part can never be reassembled.
5. Index assignment (`:1625-1657`):
   - sequenced (types 1, 4): `orderingIndex = orderedWriteIndex[ch]` (not
     incremented), `sequencingIndex = sequencedWriteIndex[ch]++`.
   - ordered (types 3, 7): `orderingIndex = orderedWriteIndex[ch]++`,
     `sequencedWriteIndex[ch] = 0`.
   - everything else: no channel fields written.
   The algorithm this implements is spelled out at `ReliabilityLayer.h:423-439`.
6. If split, `SplitPacket` (section 5) does the rest and returns (`:1659-1669`).
7. Otherwise the message joins the unreliable list if it is an unreliable type
   (`:1672`; `AddToUnreliableLinkedList` at `:3715-3736` only acts for types
   0, 1, 5) and is pushed on `outgoingPacketBuffer` with a priority weight
   (`:1676`).

**Code vs comment.** The commented-out block at `:1637-1649` records that
RakNet once deleted older queued sequenced messages on the same channel at
send time and stopped: "we don't have enough info to consistently do this".
Nothing is coalesced on the sender; "newest wins" is enforced only on the
receiver.

### 2.3 Priorities (`PacketPriority.h:21-40`, `:3926-3949`)

Four levels. `GetNextWeight` hands each message a monotonically increasing
weight whose stride grows with the level (`(1<<p)*p+p`, `:3929`), so the heap
interleaves roughly 2:1 between adjacent levels. **Code vs comment.**
`PacketPriority.h:23` says IMMEDIATE_PRIORITY messages "trigger sends
immediately" and are "generally not buffered or aggregated into a single
datagram". The reliability layer does not special-case IMMEDIATE anywhere;
the only effect is `RakPeer::SendBuffered` waking the update thread
(`RakPeer.cpp:4218-4222`). IMMEDIATE messages are aggregated like any other.

### 2.4 `Update`: filling datagrams (`:1687-2242`)

Once per update per connection (`RakPeer.cpp:5871`):

1. Clamp the tick to 100 ms (`:1735-1743`).
2. **Unreliable timeout** (`:1745-1793`): if `SetUnreliableTimeout` was
   called (default 0, never; `RakPeer.cpp:2714-2724`), walk the unreliable
   list from the oldest and free the data of any message older than the
   timeout, leaving the `InternalPacket` in the heap with `data==0` to be
   skipped and released later (`:2007-2016`).
3. **Dead connection**: if anything is in the resend buffer and no datagram
   has arrived for `timeoutTime` (10 s release, 30 s debug, `:310-312`), set
   `deadConnection` and return (`:1799-1808`; `AckTimeout` at `:3428-3433`).
   RakPeer turns this into ID_CONNECTION_LOST (`RakPeer.cpp:5874`, `:5901`).
4. Send acks if due (`:1810-1813`, section 4.2) and any pending NAKs
   (`:1815-1825`).
5. Fire LOSS receipts for unreliable-with-receipt sends whose deadline passed
   (`:1860-1881`, section 4.4).
6. **Retransmission pass** (`:1894-1970`): the retransmission budget is
   `unacknowledgedBytes` (`CCRakNetSlidingWindow.cpp:66-73`, i.e. everything
   outstanding is allowed). Walk the resend list from the head; each message
   whose `nextActionTime` has passed is popped, pushed into the current
   datagram, `timesSent++`, given a new RTO (`:1934-1938`), and re-appended at
   the tail (`:1953`; `InsertPacketIntoResendList` at `:3347-3356` is
   `AddToListTail`). The first message whose time has not come ends the pass,
   because the list is in deadline order (`:1958-1964`). `OnResend` halves
   `ssThresh` and drops `cwnd` to one MTU, once per block
   (`CCRakNetSlidingWindow.cpp:163-184`).
7. **First-transmission pass** (`:1976-2137`): while `cwnd - unacknowledgedBytes`
   allows (`CCRakNetSlidingWindow.cpp:75-86`) and `ResendBufferOverflow()` is
   false (`:3758-3765`: the slot `sendReliableMessageNumberIndex & 511` is
   occupied), peek the highest-weight message. If it does not fit the current
   datagram, close the datagram (`:2020-2026`); messages are not reordered to
   fill gaps. Reliable messages get their number, an RTO, a slot in
   `resendBuffer`, and a tail position in the resend list (`:2046-2091`);
   UNRELIABLE_WITH_ACK_RECEIPT records (predicted datagram number, serial,
   deadline) (`:2092-2099`). Every message is pushed with `PushPacket`, which
   also reports the bytes to the congestion manager (`:3462-3483`).
8. **Serialize and send** (`:2140-2227`): per datagram, take the next datagram
   number (`:2145`), write the header, and for each message that is not
   UNRELIABLE or UNRELIABLE_SEQUENCED append its message number to this
   datagram's history node (`:2174-2186`); unreliable-only datagrams get an
   empty history node (`:2202-2206`) so the ack still reaches the congestion
   manager. Only the first datagram of an update carries
   `isContinuousSend=false` (`:2142-2143`).
9. Unreliable messages are freed after the send (`:2229`; `ClearPacketsAndDatagrams`
   at `:3519-3534`).

**Code vs comment.** `ReliabilityLayer.h:560` and `:1961`/`:2134` describe
packet pairs "every 16 datagrams". `PushDatagram` has the pairing code
commented out under "Disable packet pairs" (`:3495-3504`) and
`InitializeVariables` says the same at `:407-408`. No pairs are ever marked.

**Code vs comment.** `PacketPriority.h:27` says lower priorities are
"buffered to be sent in groups at 10 millisecond intervals". There is no
10 ms timer in the reliability layer; the send cadence is whatever calls
`Update`.

---

## 3. The receive path

### 3.1 Dispatch (`RakPeer.cpp:5444-5464`, `ReliabilityLayer.cpp:635-1502`)

Datagrams from a connected address go to `HandleSocketReceiveFromConnectedPlayer`.
It reads the datagram header and takes one of three branches.

**ACK** (`:696-806`): deserialize the range list; for each datagram number
in each range, first match it against `unreliableWithAckReceiptHistory` and
emit ID_SND_RECEIPT_ACKED for each hit (`:750-770`), then look up the
datagram's history node (`:772`); if found, feed RTT to the congestion
manager (`:779-784`) and remove every message number in the node from the
resend buffer (`:786-793`), then drop the node (`:795`). A range whose
`maxIndex` is `0xFFFFFFFF` or inverted rejects the whole datagram (`:738-745`).

**NAK** (`:807-857`): for each datagram number, `OnNAK` (which only halves
`ssThresh`, `CCRakNetSlidingWindow.cpp:186-199`) and set every message in
that datagram's history node to `nextActionTime=now` so the next `Update`
resends it (`:839-854`).

**Data** (`:858-1496`): `OnGotPacket` records the datagram number; a gap
above the expected number yields `skippedMessageCount`, capped at 1000, and a
gap above 50000 drops the datagram (`CCRakNetSlidingWindow.cpp:128-161`).
Each skipped number is queued as a NAK (`:871-877`). The datagram number is
queued for ack **whether or not any message in it was reliable** (`:880-885`,
comment at `:881`: "Ack even unreliable messages for congestion control").
Then messages are parsed one at a time (`:888`, `:1493`) and each goes
through the steps below.

### 3.2 Per message (`:897-1487`)

1. Plugins see it (`:899-906`).
2. A channel at or above 32 on a channel-bearing type drops the message
   (`:919-937`).
3. **Reliable duplicate and hole tracking** (`:941-1044`), for wire types 2,
   3, 4 only. `holeCount = reliableMessageNumber - receivedPacketsBaseIndex`
   (unsigned, `:946`).
   - `holeCount == 0`: expected; pop the queue head, advance the base (`:952-958`).
   - `holeCount > 2^23`: older than the base; duplicate; drop (`:959-971`).
   - `holeCount < queue.Size()`: inside the window; if the slot says hole,
     fill it, else duplicate, drop (`:972-1000`).
   - else: above the window; refuse if `holeCount > 1000000` (`:1003-1017`),
     else push `true` (hole) until the queue reaches `holeCount`, then push
     `false` (`:1030-1032`).
   - Finally pop every leading `false` and advance the base (`:1039-1043`).
   The queue is a dense bool per message number; the 1,000,000 cap is the
   only bound on its growth (`:1003`).
4. **Split reassembly** (`:1221-1242`, section 5).
5. **Ordering and sequencing** (`:1248-1479`), for wire types 1, 3, 4, with
   `ch = orderingChannel`:
   - `orderingIndex == orderedReadIndex[ch]`:
     - sequenced: if `sequencingIndex` is not older than
       `highestSequencedReadIndex[ch]`, set that to `sequencingIndex+1` and
       deliver (`:1290-1320`); else drop (`:1321-1341`).
     - ordered: deliver, `orderedReadIndex[ch]++`,
       `highestSequencedReadIndex[ch]=0`, then pop from the heap while the
       top has the new `orderedReadIndex` (`:1343-1427`): an ordered pop
       advances the read index again; a sequenced pop sets
       `highestSequencedReadIndex` to its sequencing index (`:1415-1422`).
   - `orderingIndex` newer than `orderedReadIndex[ch]`: buffer in the heap
     (`:1429-1463`). Weight = `(orderingIndex - heapIndexOffsets[ch]) * 2^20
     + sequencingIndex` for sequenced, `+ (2^20 - 1)` for ordered, so within
     one ordering index sequenced messages come out before the ordered
     message that closes it (`:1439-1445`). `heapIndexOffsets[ch]` is reset
     to the read index whenever the heap is empty (`:1436-1437`).
   - older: drop (`:1464-1477`). For RELIABLE_ORDERED this only happens for a
     late retransmission, since step 3 already dropped duplicates.
   `IsOlderOrderedPacket` (`:2901-2922`) is a half-range comparison on
   uint24.
6. Everything else is delivered: pushed on `outputQueue` (`:1484`).

**Code vs comment.** The comment at `:1316` says the `+1` at `:1317` fixes a
bug where "a duplicated RELIABLE_SEQUENCED or UNRELIABLE_SEQUENCED packet
would be returned to the user". But `:1421`, the heap-pop path, stores
`sequencingIndex` without the `+1`, so a sequenced message popped off the
heap and then received again (a late UNRELIABLE_SEQUENCED duplicate, which
step 3 does not catch) passes the `>=` test at `:1290` and is delivered
twice.

**Observation.** A message with `orderingIndex > orderedReadIndex[ch]` is
buffered even when it is UNRELIABLE_SEQUENCED and the ordered message it
waits on is never coming, because the ordered one was a RELIABLE_ORDERED that
has not arrived yet. That is the design: sequenced and ordered share a
channel's ordering index, so an unreliable sequenced message after an ordered
one waits for the ordered one (`ReliabilityLayer.h:432`). Sequenced messages
on a channel that never sends ordered messages have `orderingIndex` fixed at
0 and are never buffered.

### 3.3 Delivery to the user

`ReliabilityLayer::Receive` pops `outputQueue` and hands over the data
pointer (`:1507-1530`). `RakPeer::RunUpdateCycle` wraps it in a `Packet`
(`RakPeer.cpp:5939-6248`); user messages, receipts and ID_DOWNLOAD_PROGRESS
pass the filter at `:6226` because their ids are at or above ID_TIMESTAMP
(`MessageIdentifiers.h:139`, `:151`, `:430`) or are a receipt id (`:96`,
`:103`). The user thread calls `RakPeer::Receive` (`:1479-1607`) and
`DeallocatePacket` (`:1613-1630`).

---

## 4. Acks, NAKs, datagram history, receipts

### 4.1 What is acked

Datagram numbers, never message numbers. The receiver inserts each arriving
data datagram's number into `acknowlegements` (`:2528-2541`;
`RangeList::Insert` at `DS_RangeList.h:159-192` merges adjacent numbers into
ranges). The sender maps a datagram number back to message numbers through
`datagramHistory` (`:3767-3836`): a queue indexed by
`datagramNumber - datagramHistoryPopCount`, each entry a linked list of
message numbers, capped at 512 entries (`RakNetDefines.h:98`; `:3800-3804`).
An ack for a datagram older than the window is ignored and the message is
resent on timeout (`RakNetDefines.h:95-96`). A retransmitted message gets a
new datagram number and a new history node, so the same message number can
appear in several nodes; `RemovePacketFromResendListAndDeleteOlderReliableSequenced`
tolerates the second removal by checking the slot still holds that number
(`:2471-2473`).

**Code vs comment.** The function name promises to "delete older reliable
sequenced"; the body (`:2437-2523`) removes one message and fires its
receipt. The sequenced deletion is in the commented-out
`DeleteSequencedPacketsInList` (`:2839-2900`).

### 4.2 When acks go out

`SendACKs` (`:3616-3656`) runs when `ShouldSendACKs` says so
(`CCRakNetSlidingWindow.cpp:88-101`): immediately until the first RTT sample
exists, then once the oldest unsent ack is `SYN`=10 ms old (`:20`). A
completed split message forces an immediate ack (`:3244-3245`). Acks fill as
many datagrams as the range list needs; each ack datagram is a header plus a
`RangeList` serialization (`DS_RangeList.h:65-113`): uint16 count, then per
range 1 byte "min equals max", 3-byte min, optional 3-byte max.

### 4.3 NAKs

Generated from datagram-number gaps on receive (`:871-877`), sent in the next
`Update` (`:1815-1825`), and on the sender they only pull the resend deadline
forward (`:839-854`). NAKs are an acceleration; the RTO is the correctness
path.

### 4.4 Receipts

Serial comes from `RakPeer::Send`'s return value (`RakPeer.cpp:1326-1351`).

- Reliable receipt types: when the message is removed from the resend buffer
  by an ack, and it is not split or is the **last** split part
  (`:2486-2496`), a 5-byte ID_SND_RECEIPT_ACKED + serial is pushed on
  `outputQueue`. There is no LOSS for reliable types; `PacketPriority.h:73`
  says so and the code has no path for it. On a dead connection the messages
  are freed in `FreeThreadSafeMemory` without receipts (`:468-560`).
- UNRELIABLE_WITH_ACK_RECEIPT: `Update` predicts the datagram number the
  message will ride in (`congestionManager.GetNextDatagramSequenceNumber() +
  datagrams already built this update`, `:2095`) and records it with the
  serial and an RTO deadline. An ack naming that datagram number yields
  ACKED (`:750-770`); the deadline passing yields ID_SND_RECEIPT_LOSS
  (`:1860-1881`). A split UNRELIABLE_WITH_ACK_RECEIPT was upgraded to
  RELIABLE_WITH_ACK_RECEIPT at send and follows the reliable rule.

**Code vs comment.** `PacketPriority.h:73` and `:76` say a reliable receipt
fires "after the message is delivered". It fires when the **ack arrives**
(`:2486-2496`), which for RELIABLE_ORDERED can be before the receiver has
delivered the message to its user (it may still be in the ordering heap).

**Observation.** The receipt for a split reliable message fires when the
part with `splitPacketIndex+1==splitPacketCount` is acked (`:2487`), not when
all parts are acked. Parts are acked independently, so the last part can be
acked while an earlier part is still in flight.

---

## 5. Large messages: split and reassembly

### 5.1 Split (`SplitPacket`, `:2928-3033`)

`splitPacketCount = (dataByteLength-1)/maximumSendBlockBytes + 1` (`:2942`),
where `maximumSendBlockBytes` is the same MTU-minus-worst-header figure used
for the split decision (`:2939`). Each part is a pooled `InternalPacket`
whose `data` points **into the original buffer** through one reference count
(`:2986`; `AllocInternalPacketData` REF_COUNTED at `:3838-3852`), so the
split copies nothing. Parts share `splitPacketId` (per-connection uint16,
incremented after each split, wrapping, `:3001`), carry their index and the
count, and inherit the original's channel, ordering index and sequencing
index because the fields were set before splitting (`:1625-1657` runs before
`:1666`). All parts are pushed as a series at the same priority (`:3007-3025`)
and each is a separate reliable message with its own message number, RTO and
ack. The last part's `dataBitLength` carries the remainder (`:2990-2993`).

**Observation.** Because parts are separate reliable messages, a 1 MB split
sends about 700 reliable messages through a 512-slot resend buffer; the
first-transmission pass stalls on `ResendBufferOverflow` until acks free
slots (`:1983`, `:2122-2123`). The comment at `RakNetDefines.h:101-103`
describes exactly this symptom.

### 5.2 Reassembly (`InsertIntoSplitPacketList` `:3038-3177`,
`BuildPacketFromSplitPacketList` `:3184-3254`)

Default build (`PREALLOCATE_LARGE_MESSAGES 0`, `RakNetDefines.h:128`). A
`SplitPacketChannel` is created on the first part seen for a split id, with a
list preallocated to `splitPacketCount` (`:3046-3059`), and lives in
`splitPacketChannelList` keyed by split id (`:3043`). Each part is appended
(`:3141`). When `list.Size() == splitPacketCount` of the first-listed part
(`:3241`), acks are flushed immediately (`:3245`), a new `InternalPacket` is
built by `rakMalloc` of the total size and `memcpy` of each part in list
order (`:3197-3212`), the parts are freed, and the channel is deleted
(`:3214-3219`).

Three facts matter for a port:

- **Parts are concatenated in arrival order, not index order.** The list is
  `Insert`ed in arrival order (`:3141`) and copied by list position
  (`:3207-3212`). `SortSplitPacketList` is declared (`.h:542`) and never
  called. Correct reassembly relies on parts of one split arriving in order,
  which holds because parts are consecutive reliable messages and... it does
  not hold under reordering or retransmission. **This is a RakNet bug, not a
  design to copy.**
- **Completion is by count, not by index set.** A duplicated part (a late
  retransmission whose message number the hole queue did not catch, which
  cannot happen for reliable parts, but see the next item) would count
  twice.
- **Reliable duplicates are filtered before reassembly** (`:941-1044` runs
  before `:1221`), so for the reliable parts RakNet actually sends, the
  count check is sound.
- **There is no timeout on a partial reassembly.** `DeleteOldUnreliableSplitPackets`
  is commented out (`:3255-3283`, call site `:2240-2241`), and since every
  split is reliable, the sender keeps resending until the parts arrive or
  the connection dies. The channel list is only freed on reset (`:475-491`).
  Memory per connection is bounded only by what the peer sends.
- Reassembled messages then go through ordering like any other (`:1248`);
  the orderingChannel of a non-channel type is set to 255 at `:1224-1225` so
  the channel arrays are not indexed with it.
- `ID_DOWNLOAD_PROGRESS` is emitted every N parts when
  `SetSplitMessageProgressInterval(N)` is set and part 0 has arrived
  (`:3149-3174`): id, parts so far (uint32), part count (uint32), byte length
  of part 0 (uint32), part 0's data.

The alternate build (`PREALLOCATE_LARGE_MESSAGES 1`, `:3047-3053`,
`:3063-3138`) allocates `dataBitLength * splitPacketCount` on the first part
and `memcpy`s each part at `index * stride`, index-correct but, as the
comment at `RakNetDefines.h:125-126` says, "vulnerable to attackers causing
the host to run out of memory". Its stride is taken from part 0
(`:3072`), and parts arriving before part 0 are buffered unsorted
(`:3092-3096`) then copied by **their own index** (`:3074-3079`), which is
correct.

---

## 6. The congestion window (`CCRakNetSlidingWindow.cpp`)

Compiled in by default (`RakNetDefines.h:122`); the UDT variant is the
alternative. State: `cwnd` (bytes on the wire allowed), `ssThresh`,
`nextDatagramSequenceNumber`, `expectedNextSequenceNumber`,
`nextCongestionControlBlock`, `backoffThisBlock`, `oldestUnsentAck`,
`lastRtt`, `estimatedRTT`, `deviationRtt` (`.h:188-213`).

- Init: `cwnd = MTU`, `ssThresh = 0` (`:43-58`); slow start while
  `cwnd <= ssThresh || ssThresh == 0` (`:367-370`).
- Transmission budget per update: `cwnd - unacknowledgedBytes`, else 0
  (`:75-86`). Retransmission budget: `unacknowledgedBytes` (`:66-73`), so
  retransmits are never throttled by cwnd.
- On ack (`:201-256`): RTT EWMA with gain 0.05 (`:218-222`). Only when the
  sender was continuously sending (`:226-227`): in slow start `cwnd += MTU`
  (`:241`); in avoidance `cwnd += MTU*MTU/cwnd` once per block (`:249-252`),
  a block being the datagram numbers issued since the last block boundary
  (`:229-237`).
- On resend (`:163-184`): if continuous and not already backed off this
  block and `cwnd > 2*MTU`: `ssThresh = cwnd/2` (floor one MTU),
  `cwnd = MTU`. The comment at `:170` says the spec's 1/2 cwnd "never
  recovers" and so cwnd is reset to one MTU with ssThresh at half.
- On NAK (`:186-199`): `ssThresh = cwnd/2`, nothing else.
- RTO (`:288-314`): `2*estimatedRTT + 4*deviationRtt + 30 ms`, capped at 2 s;
  2 s before the first sample. `timesSent` is accepted and ignored (`:290`),
  so there is **no exponential backoff** per message. **Code vs comment.**
  `CCRakNetSlidingWindow.h:149-155` describes RTO as "(RTT + 4 * RTTVar) +
  SYN", "SND*=2" on silence, and a 100 ms minimum; none of the three is in
  the code (the minimum is commented out at `:294`, `:298`).
- Ack pacing: `ShouldSendACKs` is true immediately until an RTT exists, then
  when the oldest unsent ack is 10 ms old (`:88-101`).
- `isContinuousSend` is "there was still data queued after the last send"
  (`ReliabilityLayer.cpp:1829`, `:1834`, `:2215`), carried to the receiver in
  the datagram header (`:175`) and echoed back implicitly by the ack timing.

The window is per connection and applies to the whole connection, not per
channel or per reliability. Unreliable messages consume cwnd exactly like
reliable ones at send time (`PushPacket` reports every message's bytes,
`:3482`) but never count in `unacknowledgedBytes` (`AddToListTail` is called
with `modifyUnacknowledgedBytes=isReliable`, `:2086`, `:3583-3587`), so a
burst of unreliable sends is limited by cwnd only within one update.

---

## 7. Per-reliability summary

What each type touches, sender then receiver. "common" means section 1.1's
shared machinery: datagram numbers, datagram history, acks for congestion
control.

**UNRELIABLE.** Sender: `outgoingPacketBuffer`, `unreliableLinkedList` (only
if an unreliable timeout is set), common. Header 3 bytes. Freed after the
send. Receiver: nothing beyond delivery; no duplicate detection.

**UNRELIABLE_SEQUENCED.** Sender adds `orderingChannel`,
`orderingIndex = orderedWriteIndex[ch]` (read, not advanced),
`sequencingIndex = sequencedWriteIndex[ch]++`. Header 10 bytes. Receiver
keeps `orderedReadIndex[ch]`, `highestSequencedReadIndex[ch]`, and may
buffer in `orderingHeaps[ch]` if an ordered message on the channel is
pending. Older than highest at the current ordering index: dropped.

**RELIABLE.** Sender adds `reliableMessageNumber` (at first send), a
`resendBuffer` slot, a resend-list node, RTO, `unacknowledgedBytes`. Header
6 bytes. Receiver adds the hole queue and base index. No channel state.

**RELIABLE_ORDERED.** RELIABLE plus `orderingChannel`,
`orderingIndex = orderedWriteIndex[ch]++`, `sequencedWriteIndex[ch] = 0`.
Header 10 bytes. Receiver: hole queue, then
`orderedReadIndex[ch]`/`orderingHeaps[ch]`; buffered until its index is
next; never dropped except as a duplicate.

**RELIABLE_SEQUENCED.** RELIABLE plus the sequenced fields. Header 13 bytes.
Receiver: hole queue (so resent duplicates are dropped), then the sequenced
rule (older than highest: dropped, after having been acked).

**UNRELIABLE_WITH_ACK_RECEIPT.** UNRELIABLE on the wire. Sender adds an
`unreliableWithAckReceiptHistory` entry keyed by predicted datagram number;
ACKED on that datagram's ack, LOSS on RTO. Receiver: as UNRELIABLE.

**RELIABLE_WITH_ACK_RECEIPT.** RELIABLE on the wire; ACKED when the ack
removes the message from the resend buffer. No LOSS.

**RELIABLE_ORDERED_WITH_ACK_RECEIPT.** RELIABLE_ORDERED on the wire; ACKED
as above, which can precede delivery on the far side.

Splits apply to all eight (the unreliable three are upgraded), produce
`splitPacketCount` reliable messages sharing a uint16 split id, and are
reassembled by count in arrival order on the receiver.

---

## 8. What RocketNet gives the message layer

From `USAGE.md` and `sdk/rocketnet.h`:

- **A datagram-sized frame, zero copy.** `start_packet` returns a 16-aligned
  frame with `payload_capacity` up to `ROCKETNET_MAX_PAYLOAD_BYTES` = 1192
  (`rocketnet.h:420`, `:815-839`, `:880-903`); `finish_packet` seals in place
  and stages; `send_packets` flushes. Fill may run on any thread between
  `update` and `send_packets` (`:841-855`).
- **A per-packet id and an acked batch.** Every `finish_packet` leaves a
  `packet_id` in the context (`:885`); `acked_packets` returns the ids newly
  acknowledged by the last update (`:646`, `:976`; `USAGE.md` "Delivery
  notification is the primitive"). The transport never resends.
- **A received batch per update.** `num_payloads`/`payload`/`payloads` on
  both ends (`:576-580`, `:801-809`), valid until the next update; the client
  also has a copying cursor `receive_packet` (`:563`).
- **Transport fragmentation, unreliable.** A request above 1192 bytes routes
  through the arena and splits into type 9 fragments, up to the configured
  maximum (default 16384, hard ceiling 305,408 bytes), reassembled per
  connection; a lost fragment loses the whole payload (`:423-433`;
  `USAGE.md` "Sending"). The payload's single id surfaces in the acked batch
  only when every fragment is acked.
- **Sequence burn semantics.** A failed `finish_packet` or an `abort_packet`
  burns the session sequence(s) it took, which reads as loss in the client's
  receive-side loss figure (`USAGE.md` "Sending").
- **Connection quality.** rtt (a minimum over history, not a current
  reading), jitter (different measurements on the two ends), loss
  (receive-side on the client, send-side estimate on the server)
  (`USAGE.md` "Connection quality").
- **Allocation.** One allocator hook, 64-byte aligned (`:315-329`); the
  layer above is expected to allocate per client and per server slot at
  create, like yojimbo, and never on the send or receive path (card rules).
- **Threading.** `update` and `send_packets` main-thread; fills on game
  threads strictly between them (`:841-847`).

Compared with RakNet's transport assumptions: RakNet owned the datagram
number, the ack range list, the RTT estimate and the cwnd. RocketNet owns
the first three (sequence, ack block, rtt) and gives back acked packet ids;
it does not provide a congestion window, a NAK, or a retransmit. RakNet
could coalesce messages across priorities into a datagram it built itself;
RocketNet gives a frame of fixed capacity per `start_packet`, so coalescing
means filling one frame with several messages before `finish_packet`.

---

## 9. What RocketNet's design must keep open

Invariants a per-send reliability over `n` channels needs so UNRELIABLE and
RELIABLE_ORDERED can ship first and the other six can be added without
reworking either. Each is stated as something layer 1 must not foreclose, with
the RakNet fact that motivates it.

**Message header.**

1. The per-message header carries a reliability field with room for all eight
   values (RakNet uses 3 bits and maps the receipt types down, `:2619-2628`).
   Decide now whether receipt types are visible on the wire; RakNet's choice
   (invisible) is the right one, since receipts are sender bookkeeping over
   acks the receiver sends anyway.
2. The header is byte-aligned with optional fields gated by the reliability
   (`:2634-2667`), so UNRELIABLE costs no channel or number bytes, and adding
   `sequencingIndex` later adds a field to two types without touching the
   others. Bit-packing is above the line in this repository (`CLAUDE.md`),
   so a byte header with a flags byte is the shape.
3. A `has_split` flag (RakNet: 1 bit) and three split fields reserved even if
   the first version never sets it, so a split message can be parsed by a
   receiver that does not yet implement reassembly (and dropped, counted).
4. The channel field is a byte on the wire (RakNet: 1 byte for 32 channels,
   `:2657`) and `ROCKETNET_MAX_CHANNELS` is a compile-time maximum with a
   per-connection runtime count at or under it (card rule). Stress at 1024
   and 10k channels means the byte is wrong from day one: **size the wire
   channel field for the compile-time maximum** (uint16 covers 10k) and
   reject a channel at or above the connection's runtime count on both ends
   (RakNet drops at `:926-936` and repairs to channel 0 at `:1563-1564`; do
   not copy the silent repair).

**Per-channel state.**

5. Every per-channel array is sized by the connection's runtime channel count
   from the connection's allocator at connect, not by the maximum, so 10k
   channels on a connection that asked for 2 costs nothing. RakNet's six
   fixed arrays of 32 (`ReliabilityLayer.h:442-450`) are the shape; the
   sizing is not.
6. Two sender indices per channel (`orderedWriteIndex`, `sequencedWriteIndex`)
   and two receiver indices (`orderedReadIndex`, `highestSequencedReadIndex`)
   even though the first version uses only the ordered pair. The sequenced
   types piggyback on the ordering index (`.h:423-439`): a sequenced message
   carries the channel's current ordering index and resets the sequencing
   counter when an ordered message is sent (`:1634-1635`, `:1656`). If layer
   1 defines "ordering index" as "count of ordered messages on this channel"
   and nothing else, RELIABLE_SEQUENCED and UNRELIABLE_SEQUENCED drop in
   later with no change to the ordered path. If it defines it as "count of
   all messages on the channel", they cannot.
7. The per-channel receive buffer for out-of-order RELIABLE_ORDERED messages
   is keyed by `(orderingIndex, sequencingIndex)` with ordered sorting after
   sequenced at the same ordering index (RakNet's heap weight `:1439-1445`),
   or at least is a structure that can take that key later. A plain ring
   indexed by ordering index works for ordered-only and can be extended if
   each slot can hold a short list; a ring that assumes one message per
   ordering index cannot hold the sequenced messages that share one.
8. Ordering indices wrap. RakNet uses 24 bits with half-range comparisons
   (`:2901-2922`); the receive window must be bounded well under half the
   range. Choose the width (16 or 32 bits) and the comparison once; both
   sides of every type use it.

**Message numbers, acks, resend.**

9. Reliable message numbers are **per connection, not per channel**, assigned
   at first transmission (`:2060-2061`), and the receiver's duplicate filter
   is a per-connection window over them (`:941-1044`). This is what lets
   RELIABLE, RELIABLE_SEQUENCED and the receipt types share one resend
   buffer with RELIABLE_ORDERED. Keep one number space. A receiver window
   that is a fixed bitfield (RocketNet already has one for session sequences)
   rather than RakNet's growable bool queue with a 1,000,000 cap (`:1003`).
10. The sender maps **transport packet id to message numbers**, one list per
    packet (RakNet's `datagramHistory`, `:3767-3836`), because RocketNet acks
    packet ids and a packet carries several messages. The map is a fixed ring
    from the allocator; an ack for a packet that fell out of the ring is
    ignored and the messages resend on timeout (RakNet's stated behaviour,
    `RakNetDefines.h:95-96`). A resent message gets a new packet id and a
    second entry; removal must tolerate the second ack (`:2471-2473`).
11. Unreliable messages are not in the resend buffer and do not count toward
    unacknowledged bytes (`:2086`, `:3583-3587`); but a packet carrying only
    unreliable messages still has an id and still gets acked by the transport,
    which UNRELIABLE_WITH_ACK_RECEIPT later needs (`:2092-2099`, `:750-770`).
    Keep the "message to packet id" association available for unreliable
    messages at finish time even if the first version discards it.
12. The resend buffer is a fixed array indexed by message number modulo its
    size (RakNet 512, `RakNetDefines.h:105`), and the first-transmission
    pass stops when the next slot is occupied (`:3758-3765`, `:1983`). This
    is the back-pressure that keeps allocation bounded. Size it per
    connection from the allocator, and make its size independent of the
    channel count: ordering is per channel, reliability is per connection.
13. Retransmission is by per-message deadline (RTO) off the transport's rtt,
    not by NAK; RakNet's NAKs only pull deadlines forward (`:839-854`).
    RocketNet gives acked ids and rtt but no NAK, so the design is RTO plus,
    optionally, "acked ids above this one imply loss" as the NAK substitute.
    Keep the resend list in deadline order (RakNet's circular list with
    re-append at the tail, `:3347-3356`, `:1908-1964`) so a pass stops at the
    first unexpired message.
14. Receipts fire from two places: the ack path for reliable messages
    (`:2486-2496`) and a deadline sweep for unreliable ones (`:1860-1881`).
    Both produce a message into the receive queue (RakNet: a 5-byte
    ID_SND_RECEIPT_* message). Reserve the delivery mechanism, a
    "locally generated message" that the application reads like a received
    one, or an acked-list API like the transport's, before the first type
    ships, so adding receipts does not change the receive API.

**Large messages.**

15. A message larger than one frame's payload capacity splits into parts
    that are each a reliable message with its own number and ack (RakNet
    `:2928-3033`), **not** transport fragments: the transport's type 9
    fragmentation is unreliable and loses the whole payload on one lost
    fragment (`rocketnet.h:423-433`), which is correct for a 16 KB
    unreliable snapshot and wrong for a reliable block. Both must remain
    available: large UNRELIABLE through the transport's arena with zero
    message-layer copies; large RELIABLE_* through message-layer splitting,
    queued like yojimbo block messages (card). So the split decision is per
    reliability, not per size.
16. Reassembly is **by index set, with parts placed at `index * stride`**,
    never by arrival-order count (RakNet's default concatenates in arrival
    order, `:3141`, `:3207-3212`, a bug; its preallocating variant places by
    index, `:3089`). The reassembly buffer comes from the connection's
    allocator at connect, sized by the configured maximum reliable message,
    with a fixed number of concurrent reassemblies (RocketNet's own
    `ROCKETNET_DEFAULT_REASSEMBLY_SLOTS` = 8 is the precedent,
    `rocketnet.h:433`), and a partial reassembly is **timed out** (RakNet has
    no timeout, `:3255-3283`).
17. A split message inherits its channel and ordering index once, for the
    whole message (`:1625-1657` before `:1666`), and is ordered after
    reassembly (`:1221-1248`). So ordering indices count messages, not
    parts. Any design that gives each part its own ordering index cannot
    later be made to match RakNet's receipts or RakNet's ordering.
18. A split UNRELIABLE or UNRELIABLE_SEQUENCED message is upgraded to the
    reliable twin (`:1609-1621`). Whether RocketNet keeps that rule or routes
    large unreliable messages through the transport's fragments (15) is a
    decision for layer 1; either way the C# shim's caller sees RakNet's
    enum, so the mapping is documented at the shim.

**Congestion, priorities, aggregation.**

19. The window, if any, is per connection and shared by all channels and all
    reliabilities (RakNet's `cwnd`, section 6). RocketNet has no congestion
    control; layer 1 must not need one for correctness, and if one is added
    it gates first transmission, not retransmission (RakNet's retransmission
    budget is unbounded, `CCRakNetSlidingWindow.cpp:66-73`).
20. Priorities are a sender-side ordering of the first-transmission queue
    only (`:3926-3949`); they are not on the wire and the receiver has no
    state for them. The C# surface takes a `PacketPriority`; the first
    version can accept it and treat all four alike without closing anything.
21. Small unreliable messages aggregate into one frame (card); RakNet
    aggregates across all types and priorities into one datagram and closes
    the datagram when the next message does not fit, without reordering to
    fill the gap (`:2017-2026`). Decide now whether reliable and unreliable
    messages may share a frame. If they share, one lost frame costs the
    reliable messages a resend and the unreliable ones nothing, which is
    fine; but the "message to packet id" map (10) must then hold both kinds.
22. Message numbers are assigned at the moment the message is written into
    a frame, not at `Send()` (`:2060`, reason at `InternalPacket.h:83-85`),
    so a low-priority message queued for a long time does not open a hole in
    the receiver's window.

**Threading and allocation.**

23. RakNet copies every send once into a command queue for its update thread
    (`RakPeer.cpp:4194`, `:4206`). RocketNet's fill may run on game threads
    with the frame written in place (`rocketnet.h:841-847`). A
    packet-sized UNRELIABLE message can therefore be written straight into a
    frame with zero copies **only if the message header is written into the
    frame by the message layer at finish time**, in front of the payload the
    game wrote at `frame + payload_index + header_bytes`. Reserve that
    header room in the API from the first version: `start_message` returns
    a pointer past the header, like `start_packet` returns a pointer past
    the transport's.
24. Every per-connection structure above (channel arrays, resend buffer,
    history ring, receive window, reassembly slots, ordering buffers, the
    receipt queue) is allocated once at connect from the connection's
    allocator and never on a send or receive path (card rule). RakNet's
    pools (`:330-332`) and its `rakMalloc` at `:3870` and `:3203` are what
    this rule forbids.

**Not to carry over from RakNet.**

- Silent parameter repair (`:1557-1564`). Refuse and count.
- Arrival-order reassembly (`:3141`, `:3207-3212`).
- Unbounded reassembly and hole-queue memory (`:1003`, `:3255-3283`).
- Delivering a duplicated UNRELIABLE message (no number, `:941`).
- The `+1` asymmetry in sequenced duplicate handling (`:1317` vs `:1421`).
- The receipt-on-last-part rule for splits (`:2487`); fire when all parts
  are acked.

---

## 10. The C# shim surface (provisional)

Provisional until the customer's shim arrives as data. This is what RakNet's
own SWIG binding exposes and what a RakNet-shaped C# layer will call. Sources:
the generated C# in
`DependentExtensions/Swig/InternalSwigItems/InternalSwigWindowsCSharpSample/InternalSwigTestApp/SwigFiles/`
(cited as `cs:`), and the C++ `RakPeerInterface.h` it wraps.

**Enums**, byte-for-byte RakNet's (`cs:PacketPriority.cs`,
`cs:PacketReliability.cs`; C++ `PacketPriority.h:21-85`):
`PacketPriority { IMMEDIATE_PRIORITY, HIGH_PRIORITY, MEDIUM_PRIORITY,
LOW_PRIORITY }`, `PacketReliability { UNRELIABLE, UNRELIABLE_SEQUENCED,
RELIABLE, RELIABLE_ORDERED, RELIABLE_SEQUENCED, UNRELIABLE_WITH_ACK_RECEIPT,
RELIABLE_WITH_ACK_RECEIPT, RELIABLE_ORDERED_WITH_ACK_RECEIPT }`. The numeric
values matter if the shim marshals them as ints.

**Send** (`cs:RakPeerInterface.cs:190-216`, `:548`; C++
`RakPeerInterface.h:198`, `:217`, `:239`):

```csharp
uint Send(byte[] data, int length, PacketPriority priority, PacketReliability reliability,
          char orderingChannel, AddressOrGUID systemIdentifier, bool broadcast);
uint Send(byte[] data, int length, PacketPriority priority, PacketReliability reliability,
          char orderingChannel, AddressOrGUID systemIdentifier, bool broadcast, uint forceReceiptNumber);
uint Send(BitStream bitStream, PacketPriority priority, PacketReliability reliability,
          char orderingChannel, AddressOrGUID systemIdentifier, bool broadcast[, uint forceReceiptNumber]);
uint SendList(...)   // C++ only in this binding; concatenates blocks
uint GetNextSendReceipt();        // cs:180
uint IncrementNextSendReceipt();  // cs:185
```

Return value: the receipt serial that ID_SND_RECEIPT_ACKED/LOSS will carry,
0 on bad input (`RakPeer.cpp:1306`, `:1326-1351`). `orderingChannel` is a
`char` in the C++ signature and 0..31 in practice (`ReliabilityLayer.h:162`).
`broadcast=true` means "everyone except `systemIdentifier`"
(`RakPeer.cpp:1431`). RocketNet's client has one peer and its server
addresses clients by index, so `AddressOrGUID` maps to a client index on the
server and is ignored on the client; `broadcast` is a loop over connected
clients.

**Receive** (`cs:RakPeerInterface.cs:218-224`; C++ `RakPeerInterface.h:247`,
`:251`):

```csharp
Packet Receive();                 // null when nothing is waiting
void DeallocatePacket(Packet p);
```

`Packet` (`cs:Packet.cs:57-144`; C++ `RakNetTypes.h:428-454`): `systemAddress`,
`guid`, `length` (bytes), `bitSize`, `data` (`byte[]`), `deleteData`,
`wasGeneratedLocally`. `data[0]` is the message id. The shim's caller
switches on it, so the shim must synthesize these ids (`MessageIdentifiers.h`;
`cs:DefaultMessageIDTypes.cs`):

| Id | When | Payload after the id |
|---|---|---|
| ID_CONNECTION_REQUEST_ACCEPTED (`:111`) | client connected | RakNet's handshake payload (not read in this study; the shim decides what to fill) |
| ID_NEW_INCOMING_CONNECTION (`:117`) | server: a client connected | none needed |
| ID_DISCONNECTION_NOTIFICATION (`:122`) | peer disconnected cleanly | none |
| ID_CONNECTION_LOST (`:125`) | peer timed out (`RakPeer.cpp:5901`) | none |
| ID_CONNECTION_ATTEMPT_FAILED (`:113`), ID_NO_FREE_INCOMING_CONNECTIONS (`:119`), ID_INVALID_PASSWORD (`:129`), ID_INCOMPATIBLE_PROTOCOL_VERSION (`:133`) | connect failed, by reason | none |
| ID_SND_RECEIPT_ACKED (`:96`), ID_SND_RECEIPT_LOSS (`:103`) | receipt | uint32 serial, native order, 5 bytes total (`ReliabilityLayer.cpp:2490-2495`) |
| ID_DOWNLOAD_PROGRESS (`:151`) | split progress when enabled | uint32 parts, uint32 count, uint32 len, part 0 (`:3159-3172`) |
| ID_TIMESTAMP (`:139`) | prefix the application put first | 8-byte time the receiver adjusts (`RakPeer.cpp:1549-1554`) |
| ID_USER_PACKET_ENUM (`:430`) and up | user messages | application's |

**Connection management the shim will also expect** (`cs:RakPeerInterface.cs`):
`GetInstance`/`DestroyInstance` (`:68`, `:74`), `Startup(maxConnections,
SocketDescriptor, count)` (`:78-83`), `SetMaximumIncomingConnections`
(`:115`), `Connect(host, port, password, passwordLength[, PublicKey, ...])`
(`:133-158`), `Shutdown(blockDuration[, orderingChannel, priority])`
(`:163-171`), `IsActive` (`:175`), `CloseConnection(target,
sendNotification[, orderingChannel, priority])` (`:233-243`),
`GetSystemAddressFromIndex` (`:265`), `GetGuidFromSystemAddress` (`:389`),
`GetAveragePing`/`GetLastPing` (`:320`, `:326`), `SetOccasionalPing`
(`:338`), `SetTimeoutTime(ms, address)` (`:407`), `GetMTUSize` (`:418`),
`SetSplitMessageProgressInterval` (`:453`), `SetUnreliableTimeout` (`:462`).
RocketNet's connect token replaces host/port/password; the shim mints or
receives a token and maps `Connect` onto `rocketnet_client_transport_create`.

**Semantics the shim's caller will assume, from the table in section 1:**
per-send reliability and channel; ordered and sequenced messages on the same
channel are ordered relative to each other (`PacketPriority.h:52`, `:59`,
`:63`); split messages are reliable; receipts come back through `Receive`;
a lost connection surfaces as ID_CONNECTION_LOST after the timeout; nothing
is delivered twice for reliable types. The two documented-but-absent
behaviours (duplicate detection for UNRELIABLE, immediate sends for
IMMEDIATE_PRIORITY) are not something the caller can have been relying on,
since RakNet never did them.

---

## 11. Not done, and open questions for the layering

- This is a reading, not a run: RakNet was not built or executed, and none of
  the code-vs-comment findings were confirmed by a test. The three that bear
  on design (arrival-order reassembly `:3141`/`:3207`; the `+1` asymmetry
  `:1317`/`:1421`; receipt on the last part `:2487`) are worth a small
  harness before they are relied on as facts about RakNet's behaviour.
- The UDT congestion controller (`CCRakNetUDT.*`) was not read; the default
  build does not compile it.
- `RakPeer.cpp` was read for `Send`, the buffered command path, `Receive`,
  `DeallocatePacket`, dispatch into the reliability layer and the
  reliability-layer drain; the connection handshake, pings and plugins were
  not.
- RakNet's SWIG `.i` files were not read beyond listing; the generated C#
  was taken as the surface.
- Open for Glenn before layer 1: the wire width of the channel field (4),
  whether reliable and unreliable messages share a frame (21), and whether
  large unreliable messages go through the transport's fragments or are
  upgraded to reliable as RakNet does (15, 18).

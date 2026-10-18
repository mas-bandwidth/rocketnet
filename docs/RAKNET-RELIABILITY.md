# RakNet's reliability layer, read from the code

A study for the engineer who starts RocketNet's message layer: what RakNet
actually does for each of its eight reliabilities, what state it keeps, how its
counters and acks work, how it splits and reassembles, what its congestion window
does, and then what RocketNet's design must keep open so that UNRELIABLE and
RELIABLE_ORDERED can ship first without cornering the other six.

Every fact below comes from the code, cited as `file:line`. Where the code and
its comments disagree, the disagreement is stated and the code wins. Nothing is
from memory of RakNet.

**Source.** `facebookarchive/RakNet` at commit `1a16989` (a `--depth 1` clone
made for this study on 2026-10-06). Paths are relative to its `Source/`
directory unless they start with `DependentExtensions/`. Abbreviations: `RL.h`
= `ReliabilityLayer.h`, `RL.cpp` = `ReliabilityLayer.cpp`, `PP.h` =
`PacketPriority.h`, `IP.h` = `InternalPacket.h`, `RP.cpp` = `RakPeer.cpp`,
`RPI.h` = `RakPeerInterface.h`, `CC.h` / `CC.cpp` = `CCRakNetSlidingWindow.h`
/ `.cpp`.

**Build configuration assumed**, because the code forks on it:
`USE_SLIDING_WINDOW_CONGESTION_CONTROL=1` (`RakNetDefines.h:121-123`), so the
congestion controller is `CCRakNetSlidingWindow` (`RL.h:521-522`) and
`INCLUDE_TIMESTAMP_WITH_DATAGRAMS=0` (`RL.h:46-48`);
`PREALLOCATE_LARGE_MESSAGES=0` (`RakNetDefines.h:127-129`);
`CC_TIME_TYPE_BYTES=8`, microsecond times (`CC.h:65-68`); `LIBCAT_SECURITY`
off. RocketNet-side facts cite `sdk/rocketnet.h` and `USAGE.md` in this
repository at the commit this document lands on.

---

## 1. The two things on the wire: datagrams and messages

RakNet has two numbered things, and keeping them apart is the whole trick.

- A **datagram** is one UDP payload. It carries a 24-bit `datagramNumber`
  (`CC.h:73`, `RL.cpp:115`) drawn from a per-connection counter in the
  congestion controller (`CC.cpp:108-113`). **Acks and NAKs name datagrams,
  never messages** (`RL.cpp:2534-2535`, `RL.cpp:736-746`).
- A **message** is one user send, or one fragment of one. It carries a message
  header (section 4.2) and, if reliable, a 24-bit `reliableMessageNumber`
  (`IP.h:37, 48`). **Resend, duplicate detection and ordering work on message
  numbers and ordering indices, never on datagram numbers.**

One datagram holds many messages of mixed reliabilities (`RL.cpp:1998-2136`).
The sender remembers, per datagram, which reliable message numbers it carried
(`RL.h:339-352`, `RL.cpp:2174-2186`), so a datagram ack turns into message acks
(`RL.cpp:772-795`).

## 2. The eight reliabilities and the four priorities

`PP.h:46-85`, enum `PacketReliability`:

| value | name | wire code sent | reliable | ordered/sequenced | receipt |
|---|---|---|---|---|---|
| 0 | `UNRELIABLE` | 0 | no | no | no |
| 1 | `UNRELIABLE_SEQUENCED` | 1 | no | sequenced | no |
| 2 | `RELIABLE` | 2 | yes | no | no |
| 3 | `RELIABLE_ORDERED` | 3 | yes | ordered | no |
| 4 | `RELIABLE_SEQUENCED` | 4 | yes | sequenced | no |
| 5 | `UNRELIABLE_WITH_ACK_RECEIPT` | 0 | no | no | ACKED or LOSS |
| 6 | `RELIABLE_WITH_ACK_RECEIPT` | 2 | yes | no | ACKED only |
| 7 | `RELIABLE_ORDERED_WITH_ACK_RECEIPT` | 3 | yes | ordered | ACKED only |

`NUMBER_OF_RELIABILITIES` is 8 (`PP.h:84`). Two more names are commented out
and never took a value: `UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT` (`PP.h:71`) and
`RELIABLE_SEQUENCED_WITH_ACK_RECEIPT` (`PP.h:81`), with the reason at `PP.h:70, 80`: a sequenced message can be discarded by the receiver, so an ack does not
mean it was processed. The longer reason, with the bug it caused, is in a
comment at `RL.cpp:2047-2053`.

**The receipt variants never reach the wire.** The writer maps 5 to 0, 6 to 2,
7 to 3 before writing the 3-bit code (`RL.cpp:2619-2628`); the reader says so
(`RL.cpp:2712`). The receiver therefore sees only values 0 to 4. The code is
written in 3 bits (`PP.h:43`, `RL.cpp:2628`); `PP.h:45` says "Do not reorder, I
check on >= UNRELIABLE_WITH_ACK_RECEIPT", and that check is at `RL.cpp:2486`
and `RP.cpp:1336`.

`PP.h:21-40`, enum `PacketPriority`: `IMMEDIATE_PRIORITY`, `HIGH_PRIORITY`,
`MEDIUM_PRIORITY`, `LOW_PRIORITY`, `NUMBER_OF_PRIORITIES` = 4. Priority affects
only the order in which queued messages are pulled into datagrams (section
5.3); it never changes a header.

## 3. State

### 3.1 Per connection

RakPeer holds one `ReliabilityLayer` per remote system (`RakPeer.h:669`), with
that system's `MTUSize` (`RakPeer.h:679`) and `lastReliableSend`
(`RakPeer.h:675`). Inside the layer, read off `RL.h` and initialised at
`RL.cpp:384-457`:

**Sender side**

| state | type / size | where | what |
|---|---|---|---|
| `sendReliableMessageNumberIndex` | uint24 | `RL.h:411`, `RL.cpp:397` | next reliable message number; assigned late, at datagram build (`RL.cpp:2060-2061, 2090`) |
| `internalOrderIndex` | uint24 | `RL.h:412` | local-only send order stamp (`RL.cpp:1596`); not on the wire |
| `splitPacketId` | uint16 | `RL.h:416`, `RL.cpp:394` | next split id (`RL.cpp:2996, 3001`) |
| `orderedWriteIndex[32]` | uint24 each | `RL.h:442` | per channel, section 3.2 |
| `sequencedWriteIndex[32]` | uint24 each | `RL.h:444` | per channel, section 3.2 |
| `outgoingPacketBuffer` | heap of `InternalPacket*` by uint64 weight | `RL.h:401` | messages not yet sent once; unbounded |
| `outgoingPacketBufferNextWeights[4]` | uint64 | `RL.h:402` | priority weights (`RL.cpp:3926-3949`) |
| `unreliableLinkedListHead` | circular list through `InternalPacket` | `RL.h:378`, `RL.cpp:3695-3736` | every queued unreliable message, for the optional cull |
| `resendBuffer[512]` | `InternalPacket*` | `RL.h:376`, `RakNetDefines.h:104-106` | reliable messages in flight, indexed by `messageNumber & 511` |
| `resendLinkedListHead` | circular list | `RL.h:377`, `RL.cpp:3536-3614` | the same messages, FIFO by last send |
| `unacknowledgedBytes` | uint32 | `RL.h:528` | header+data bytes of reliable messages in flight (`RL.cpp:3585, 3573`) |
| `datagramHistory` | queue of up to 513 `{head, timeSent}` | `RL.h:339-352`, `RL.cpp:3797-3828` | per sent datagram, the list of reliable message numbers it carried |
| `datagramHistoryPopCount` | uint24 | `RL.h:372` | datagram number of `datagramHistory[0]` |
| `unreliableWithAckReceiptHistory` | list of `{datagramNumber, serial, deadline}` | `RL.h:355-365` | receipts pending for reliability 5 |
| `packetsToSendThisUpdate` and friends | lists | `RL.h:545-552` | the datagrams being built in this `Update` |
| `congestionManager` | `CCRakNetSlidingWindow` | `RL.h:522` | section 9 |
| `timeoutTime` | ms | `RL.h:417`, `RL.cpp:308-313` | 10 000 release, 30 000 debug |
| `unreliableTimeout` | us | `RL.h:332`, `RL.cpp:404` | 0 = never cull |
| `deadConnection` | bool | `RL.h:415` | set at `RL.cpp:1806` |
| `internalPacketPool` | pool, page 8 | `RL.h:374`, `RL.cpp:331`, `RakNetDefines.h:176` | every `InternalPacket` comes from here |

**Receiver side**

| state | type / size | where | what |
|---|---|---|---|
| `receivedPacketsBaseIndex` | uint24 | `RL.h:472` | lowest reliable message number not yet received |
| `hasReceivedPacketQueue` | queue of bool, initial 512 | `RL.h:471`, `RL.cpp:49, 914` | one entry per message number above the base: `true` = still missing, `false` = got it |
| `orderedReadIndex[32]` | uint24 each | `RL.h:446` | per channel, section 3.2 |
| `highestSequencedReadIndex[32]` | uint24 each | `RL.h:448` | per channel, section 3.2 |
| `orderingHeaps[32]` | heap of `InternalPacket*` by uint64 weight | `RL.h:449` | per channel reorder buffer; unbounded |
| `heapIndexOffsets[32]` | uint24 each | `RL.h:450` | the `orderedReadIndex` when the channel's heap last became non-empty (`RL.cpp:1436-1437`) |
| `splitPacketChannelList` | ordered list keyed by `splitPacketId` | `RL.h:409`, `RL.h:64-80` | incomplete reassemblies; never expired (section 8) |
| `acknowlegements` | range list of uint24 | `RL.h:565` | datagram numbers to ack |
| `NAKs` | range list of uint24 | `RL.h:566` | datagram numbers to NAK |
| `incomingAcks` | range list | `RL.h:558` | scratch for parsing |
| `outputQueue` | queue of `InternalPacket*` | `RL.h:330` | what `Receive` hands up; unbounded |
| `timeLastDatagramArrived` | ms | `RL.h:391`, `RL.cpp:660` | any datagram, data or ack |
| `remoteSystemNeedsBAndAS` | bool | `RL.h:567`, `RL.cpp:878` | whether acks should carry a rate estimate (they never do, `CC.cpp:264-271`) |

### 3.2 Per ordering channel

`NUMBER_OF_ORDERED_STREAMS` is 32, "2^5" (`RL.h:51-52`). Each channel owns four
24-bit counters, two per side (`RL.h:441-450`), zeroed at `RL.cpp:386-389`. The
algorithm that couples ordered and sequenced on one channel is written out as a
comment at `RL.h:423-439` and matches the code:

- **Sender, ordered send** (`RL.cpp:1651-1657`): `orderingIndex = orderedWriteIndex[ch]++`, and `sequencedWriteIndex[ch] = 0`.
- **Sender, sequenced send** (`RL.cpp:1625-1636`): `orderingIndex = orderedWriteIndex[ch]` (not incremented), `sequencingIndex = sequencedWriteIndex[ch]++`.
- **Receiver** keeps `orderedReadIndex[ch]`, the next ordered index expected,
  and `highestSequencedReadIndex[ch]`, the sequenced watermark for the current
  ordered index. On delivering an ordered message: `orderedReadIndex++` and
  `highestSequencedReadIndex = 0` (`RL.cpp:1372-1373`). On delivering a
  sequenced message at the current ordered index directly: `highestSequencedReadIndex = sequencingIndex + 1` (`RL.cpp:1317`). On releasing a sequenced message from
  the channel heap: `highestSequencedReadIndex = sequencingIndex`, without the
  `+ 1` (`RL.cpp:1421`), so a duplicate of that exact message arriving next is
  not "older" and is delivered again; section 11 item 18. On releasing an
  **ordered** message from the heap: `orderedReadIndex++` only (`RL.cpp:1417`);
  the watermark is not reset to 0 as it is on the direct path (`RL.cpp:1373`),
  so sequenced messages of the new epoch are tested against the previous
  epoch's watermark until one of them is delivered; section 11 item 19.

So a sequenced message is "newer than" everything sent on the channel before
the last ordered message, and the ordered index acts as an epoch for the
sequenced index. The two reliabilities are not independent on a shared channel.

### 3.3 Per message: `InternalPacket`

`IP.h:45-68` is the part that is (partly) serialised; `IP.h:79-125` the rest.

| field | type | on the wire | notes |
|---|---|---|---|
| `reliableMessageNumber` | uint24 | reliable only | 0xFFFFFF until assigned (`RL.cpp:3678`), and on receive of an unreliable message (`RL.cpp:2720`) |
| `orderingIndex` | uint24 | ordered and sequenced | |
| `sequencingIndex` | uint24 | sequenced only | |
| `orderingChannel` | uint8 | ordered and sequenced | 0 on receive otherwise (`RL.cpp:2740`); 255 marks "not ordered" on split reassembly (`RL.cpp:1224-1225`) |
| `splitPacketId` | uint16 | if split | |
| `splitPacketIndex` | uint32 | if split | |
| `splitPacketCount` | uint32 | if split | 0 = not split |
| `dataBitLength` | uint32 in memory, **uint16 on the wire** | always | bits, not bytes (`RL.cpp:2632-2633`) |
| `reliability` | enum | 3 bits | receipt variants mapped away on write |
| `messageInternalOrder` | uint24 | no | `RL.cpp:1596` |
| `messageNumberAssigned` | bool | no | the "late assignment" fix, explained at `IP.h:83-85` and `RL.cpp:1024-1027` |
| `creationTime`, `nextActionTime`, `retransmissionTime` | us | no | resend timing |
| `headerLength` | bits | no | cached at `RL.cpp:2018` |
| `data`, `allocationScheme`, `refCountedData`, `stackData[128]` | | no | NORMAL (malloc), REF_COUNTED (fragments share one block), STACK (128 bytes or fewer inline) (`IP.h:100-112, 124`, `RL.cpp:3838-3907`) |
| `timesSent` | uint8 | no | |
| `priority` | enum | no | |
| `sendReceiptSerial` | uint32 | no | the number `Send()` returned (`RL.cpp:1599`) |
| `resendPrev/Next`, `unreliablePrev/Next` | pointers | no | intrusive lists |

### 3.4 The counters and their widths

| counter | width | wraps | compared by |
|---|---|---|---|
| `datagramNumber` | 24 | yes (`RakNetTypes.h:473-480`) | half-range (`CC.cpp:341-353`) |
| `reliableMessageNumber` | 24 | yes | unsigned subtraction against the base; duplicate if the gap exceeds half the range (`RL.cpp:946-971`) |
| `orderingIndex`, `sequencingIndex` | 24 | yes | `IsOlderOrderedPacket`, half-range (`RL.cpp:2901-2922`) |
| `splitPacketId` | 16 | yes, "It's ok if this wraps to 0" (`RL.cpp:3001`) | equality only (`RL.cpp:247-261`) |
| `splitPacketIndex`, `splitPacketCount` | 32 | no | |
| `sendReceiptSerial` | 32 | yes, skipping 0 (`RP.cpp:1283-1284`) | equality |

## 4. Wire format

### 4.1 Datagram header

`RL.cpp:110-227`. Bits, in order, written by `Serialize`:

- `isValid` (1 bit, always 1).
- `isACK` (1 bit). If set: `hasBAndAS` (1 bit), align to byte, then `float AS`
  only if `hasBAndAS` (never, `CC.cpp:264-271`). Then the ack range list
  (section 7.1).
- Else `isNAK` (1 bit). If set: the NAK range list.
- Else `isPacketPair`, `isContinuousSend`, `needsBAndAs` (3 bits), align, then
  `datagramNumber` (uint24). Then messages until the datagram ends.

So a data datagram header is 4 bytes and an ack header is 1 byte, while
`GetDataHeaderByteLength` budgets 2 + 3 + 4 = 9 bytes (`RL.cpp:133-141`); the
9 is the reservation used in MTU arithmetic (`RL.cpp:3909-3912`), not what is
written. Datagrams of length 2 or less are ignored (`RL.cpp:653-658`).

### 4.2 Message header

`RL.cpp:2610-2677` (write), `RL.cpp:2682-2801` (read), lengths at
`RL.cpp:2555-2605`. Byte aligned at the start (`RL.cpp:2618`):

| field | size | present when |
|---|---|---|
| reliability | 3 bits | always |
| hasSplitPacket | 1 bit, then pad to byte | always |
| `dataBitLength` | uint16, **bits** | always |
| `reliableMessageNumber` | uint24 | wire code 2, 3, 4 |
| `sequencingIndex` | uint24 | wire code 1, 4 |
| `orderingIndex` | uint24 | wire code 1, 3, 4 |
| `orderingChannel` | uint8 | wire code 1, 3, 4 |
| `splitPacketCount` | uint32 | hasSplitPacket |
| `splitPacketId` | uint16 | hasSplitPacket |
| `splitPacketIndex` | uint32 | hasSplitPacket |
| data | `ceil(dataBitLength/8)` bytes | always |

Per-message overhead by type: UNRELIABLE 3 bytes; RELIABLE 6;
UNRELIABLE_SEQUENCED 10; RELIABLE_ORDERED 10; RELIABLE_SEQUENCED 13; add 10
for a fragment. The maximum header, 23 bytes, is computed from a split
RELIABLE_SEQUENCED (`RL.cpp:2547-2553`) and is what the split threshold
subtracts (`RL.cpp:1604`).

The reader drops the message, and with it the rest of the datagram (the loop at
`RL.cpp:1491-1494` ends on a null), when: a read fails, `dataBitLength == 0`,
`reliability >= 8`, `orderingChannel >= 32` (hard-coded, not the macro), or a
split index is not below its count (`RL.cpp:2760-2770`). It then allocates a
fresh buffer for the data (`RL.cpp:2773`).

**Code vs comment.** The comment at `RL.cpp:2647` says `sequencingIndex` is
"Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED, RELIABLE_ORDERED"; the code
writes it only for the two sequenced types (`RL.cpp:2643-2648`). The reader
tests for `RELIABLE_ORDERED_WITH_ACK_RECEIPT` at `RL.cpp:2733`, which can never
be on the wire (`RL.cpp:2619-2626`, and the reader's own comment at
`RL.cpp:2712`). `PP.h:48` claims "(6 to 17) + 21 bits" of overhead for
UNRELIABLE, `PP.h:55` claims RELIABLE has "Same overhead as UNRELIABLE", and
`PP.h:62` claims RELIABLE_SEQUENCED has "Same overhead as
UNRELIABLE_SEQUENCED"; the code writes 3, 6, 13 and 10 whole bytes.

## 5. The send path

### 5.1 RakPeer: the user thread

`RakPeerInterface::Send` (`RPI.h:198`) takes `data, length, priority, reliability, char orderingChannel, AddressOrGUID, broadcast, forceReceiptNumber`
and returns a `uint32_t` receipt. In `RP.cpp:1308-1352`:

1. Bad input returns 0 (`RP.cpp:1317-1324`).
2. The receipt serial is `forceReceiptNumber` if non-zero, else
   `IncrementNextSendReceipt()` (`RP.cpp:1326-1330`), a mutexed counter that
   starts at 1 and skips 0 on wrap (`RP.cpp:1279-1287`, `RP.cpp:4386-4391`).
   Every send consumes a serial whether or not its reliability returns receipts.
3. A loopback target is handled inline (`RP.cpp:1332-1347`); see section 7.4.
4. Otherwise `SendBuffered` copies the data with `rakMalloc` into a
   `BufferedCommandStruct` and pushes it on a lock-free queue to the network
   thread (`RP.cpp:4189-4216`). `IMMEDIATE_PRIORITY` additionally signals the
   network thread to wake now (`RP.cpp:4218-4222`); that is the only thing
   "immediate" does.

`SendList` concatenates several blocks into one message the same way
(`RP.cpp:1434-1464`, `RP.cpp:4225-4284`).

The network thread drains the queue in `RunUpdateCycle` (`RP.cpp:5619-5632`)
and calls `SendImmediate` (`RP.cpp:4286-4384`), which resolves the target list
(one system, or all but one for broadcast, skipping systems that are
disconnecting, `RP.cpp:4297-4348`) and calls `reliabilityLayer.Send` once per
target (`RP.cpp:4363`). The malloc'd copy is handed to the last target without
copying and copied for every other target (`RP.cpp:4361-4365`). Any reliable
reliability stamps `lastReliableSend` (`RP.cpp:4367-4375`), which drives a
keep-alive reliable ping after `timeoutTime/2` of reliable silence when nothing
is in the resend buffer (`RP.cpp:5857-5869`).

### 5.2 ReliabilityLayer::Send

`RL.cpp:1539-1683`.

1. **Parameter repair, silent**: reliability outside 0..7 becomes `RELIABLE`,
   priority outside range becomes `HIGH_PRIORITY`, `orderingChannel >= 32`
   becomes 0 (`RL.cpp:1557-1564`). Note `RPI.h:198` declares the channel as a
   signed `char` and the assert at `RP.cpp:1315` cannot fire for a negative
   value; the layer takes `unsigned char` (`RL.h:168`), so channel -1 arrives as
   255 and is mapped to 0. The priority test is `> NUMBER_OF_PRIORITIES`
   (`RL.cpp:1560`), so the value 4 passes through unrepaired and later indexes
   the four-entry weight array (`RL.h:402`) one past its end at `RL.cpp:3934`;
   section 11 item 20.
2. An `InternalPacket` is taken from the pool (`RL.cpp:1571`). With
   `makeDataCopy`, data of 128 bytes or fewer is copied into `stackData`, else
   malloc'd and copied (`RL.cpp:1582-1587`, `RL.cpp:3860-3872`); without, the
   pointer is adopted (`RL.cpp:1588-1593`).
3. `messageInternalOrder`, priority, reliability and the receipt serial are
   stamped (`RL.cpp:1596-1599`).
4. **Split decision**: the message splits if its byte length exceeds
   `(MTU - 28 - 9) - 23` (`RL.cpp:1604-1606`, `RL.cpp:3909-3919`, the 28 from
   `RL.cpp:361` and `CC.h:51`). At the default MTU 1492 (`MTUSize.h:33`) that is
   1432 bytes. A split upgrades `UNRELIABLE` to `RELIABLE`,
   `UNRELIABLE_WITH_ACK_RECEIPT` to `RELIABLE_WITH_ACK_RECEIPT`,
   `UNRELIABLE_SEQUENCED` to `RELIABLE_SEQUENCED` (`RL.cpp:1609-1621`), with the
   reason at `RL.cpp:1611-1612`: a lost part cannot be reassembled.
5. **Index assignment** per section 3.2 (`RL.cpp:1625-1657`), *before* the
   split, so every fragment carries the same ordering and sequencing indices
   (the header is copied whole at `RL.cpp:2963`). The "newer sequenced send
   supersedes queued older ones" idea is present but disabled, with the reason
   at `RL.cpp:1637-1649`.
6. A split message goes to `SplitPacket` (section 8.1) and returns
   (`RL.cpp:1659-1669`). Otherwise the message joins the unreliable list if
   unreliable (`RL.cpp:1672`, `RL.cpp:3715-3736`) and the outgoing heap with a
   priority weight (`RL.cpp:1676`).

**The message number is not assigned here.** `messageNumberAssigned` stays
false until the message is actually placed in a datagram (`RL.cpp:1675, 2060`). The reason is at `IP.h:83-85` and `RL.cpp:1024-1027`: assigning at
`Send` time and then sending higher-priority messages first created huge gaps
in the receiver's hole queue.

### 5.3 Priority: the heap weights

Messages wait in one heap keyed by a 64-bit weight (`RL.h:401`). Each priority
level has a running next-weight (`RL.h:402`), seeded as `(1 << p) * p + p`
(`RL.cpp:3926-3930`) and advanced by `(1 << p) * (p + 1) + p` per push, pulled
forward if it has fallen behind the heap's head (`RL.cpp:3932-3949`). The
strides are 1, 5, 14 and 35 for IMMEDIATE, HIGH, MEDIUM and LOW, so under a
backlog adjacent levels interleave at about 5:1, 2.8:1 and 2.5:1. The heap is a
min-heap (`RL.h:401`, the `false` template argument), so the lowest weight is
pulled first. The weights advance only while the heap is non-empty; when it is
empty every level is reseeded (`RL.cpp:3935-3947`), so the ratios describe a
backlog and nothing else. This is the whole of priority; there is no
per-priority queue and no timer.

**Code vs comment.** `PP.h:23-36` says IMMEDIATE messages "trigger sends
immediately, and are generally not buffered or aggregated", that lower
priorities "are buffered to be sent in groups at 10 millisecond intervals", and
"For every 2 IMMEDIATE_PRIORITY messages, 1 HIGH_PRIORITY will be sent". The
reliability layer has no 10 ms interval and aggregates IMMEDIATE messages like
any other (`RL.cpp:1998-2136`); the only immediate behaviour is the thread wake
at `RP.cpp:4218-4222`, and the ratio the weights produce between IMMEDIATE and
HIGH is about 5:1, not 2:1.

### 5.4 Update: building and sending datagrams

`RL.cpp:1687-2242`, once per connection per network-thread cycle
(`RP.cpp:5871`). In order:

1. Time step, capped at 100 ms (`RL.cpp:1728-1743`).
2. **Unreliable cull**, only if `unreliableTimeout > 0`: unsent unreliable
   messages older than the timeout have their data freed and are marked by
   `data == 0` (`RL.cpp:1745-1793`); the heap skips and releases such entries
   when it reaches them (`RL.cpp:2007-2016`). Default is no cull
   (`RL.cpp:404`).
3. **Dead connection**: if anything is in the resend buffer and no datagram of
   any kind has arrived for `timeoutTime` (`RL.cpp:1799-1808`, `AckTimeout` at
   `RL.cpp:3428-3433`), `deadConnection` is set and `Update` returns. RakPeer
   then drops the system (`RP.cpp:5874`). The `timeLastDatagramArrived - curTime > 10000` term at `RL.cpp:3432` guards a clock race, per its comment.
4. **Acks** go out when the congestion controller says so (`RL.cpp:1810-1813`,
   section 7.2). **NAKs** go out every update that has any
   (`RL.cpp:1815-1825`).
5. The controller's `isContinuousSend` input is "was anything still queued
   after the last update" (`RL.cpp:1829, 1834, 2215, 2232`), and
   `needsBAndAs` is "am I in slow start" (`RL.cpp:1828`).
6. **Pending unreliable receipts** past their deadline become
   `ID_SND_RECEIPT_LOSS` (`RL.cpp:1860-1881`, section 7.3).
7. If there is anything to send or resend (`RL.cpp:1836, 1883`):
   - `transmissionBandwidth = cwnd - unacknowledgedBytes` and
     `retransmissionBandwidth = unacknowledgedBytes` (`RL.cpp:1894-1895`,
     `CC.cpp:66-86`).
   - **Resends first** (`RL.cpp:1903-1969`): walk the resend list from the head
     while the head's `nextActionTime` has passed, fill a datagram, pop the
     message, push it, `timesSent++`, tell the controller (`OnResend`,
     `CC.cpp:163-184`), compute a new RTO (`CC.cpp:288-314`), and re-append
     the message at the list tail (`RL.cpp:1951-1953`, `RL.cpp:3347-3356`).
     A message whose time has not come stops the datagram, and the resend walk
     ends when a whole pass pushed nothing (`RL.cpp:1958-1968`). Resends are
     limited only by `unacknowledgedBytes`, so the window never blocks them.
   - **New sends** (`RL.cpp:1976-2137`) while under `transmissionBandwidth`
     and the resend buffer is not full: pop the heap lowest weight first, stop the
     datagram at the MTU (`RL.cpp:2018-2026`), and for each message:
     - reliable (wire codes 2, 3, 4 and the two reliable receipt types,
       `RL.cpp:2028-2038`): assign the message number now, compute RTO and
       `nextActionTime`, place it in `resendBuffer[number & 511]` (asserting
       the slot is empty, `RL.cpp:2075-2079`), append to the resend list
       adding its bytes to `unacknowledgedBytes`, and increment the number
       (`RL.cpp:2060-2090`);
     - `UNRELIABLE_WITH_ACK_RECEIPT`: record `{datagram number it will get, serial, RTO deadline}` in `unreliableWithAckReceiptHistory`
       (`RL.cpp:2092-2099`);
     - push it into the datagram (`RL.cpp:2109`), `timesSent++`.
     `ResendBufferOverflow` is "the next message number's slot is still
     occupied" (`RL.cpp:3758-3765`), so **at most 512 reliable messages are in
     flight per connection**; when that trips, new sends stop until an ack
     frees the slot (`RL.cpp:1983, 2122-2123`). `RakNetDefines.h:101-103`
     says a too-small value underutilises high-ping links.
   - **Emit** (`RL.cpp:2140-2227`): per datagram, take the next datagram number
     (`RL.cpp:2145`), write the header, record in `datagramHistory` the message
     number of every message whose reliability is not `UNRELIABLE` or
     `UNRELIABLE_SEQUENCED` (`RL.cpp:2171-2186`; an unreliable-with-receipt
     message is recorded with its unassigned number 0xFFFFFF, which matches
     nothing at ack time because `RL.cpp:2473` compares the stored number), or
     a dummy node if the datagram was all unreliable (`RL.cpp:2202-2206`), write
     each message, and send (`RL.cpp:2213`). Unreliable messages are freed after
     the send (`RL.cpp:2229`, `RL.cpp:3519-3534`); reliable ones live on in
     the resend buffer.

`datagramHistory` keeps up to 513 datagrams (`RL.cpp:3800-3805`, the pop
happens only when the size already exceeds 512; the comment at `RL.h:349` says
512); an ack for an older datagram finds nothing (`RL.cpp:3767-3781`) and the
message is resent unnecessarily, as `RakNetDefines.h:94-96` says.

The outgoing bandwidth cap passed from RakPeer only stops new sends, measured
against the last second's user bytes (`RL.cpp:1995-1999`).

## 6. The receive path

### 6.1 RakPeer

The network thread receives a UDP datagram, resolves the remote system, and
calls `HandleSocketReceiveFromConnectedPlayer` on its reliability layer
(`RP.cpp:5429-5470`). Later in the same cycle it drains the layer's
`Receive()` (`RP.cpp:5939-6248`): each returned buffer becomes a `Packet`
(`RakNetTypes.h:428-454`) without copying (`RP.cpp:6230-6236`, `AllocPacket`
at `RP.cpp:169-185`) and goes to the user-facing queue. System messages below
`ID_TIMESTAMP` are consumed by RakPeer except the two receipts, which pass
through (`RP.cpp:6226`). The user thread's `Receive()` pops that queue
(`RP.cpp:1479-1607`) and `DeallocatePacket` frees the buffer and returns the
`Packet` to a pool (`RP.cpp:1613-1630`).

### 6.2 ReliabilityLayer::HandleSocketReceiveFromConnectedPlayer

`RL.cpp:635-1502`. Any datagram, including a bare ack, refreshes
`timeLastDatagramArrived` (`RL.cpp:660`). Then by header:

**ACK** (`RL.cpp:696-806`): parse the range list (`RL.cpp:728-735`); a range
with `min > max` or `max == 0xFFFFFF` rejects the whole datagram
(`RL.cpp:738-745`). For every datagram number in every range:

1. any `unreliableWithAckReceiptHistory` entry with that datagram number yields
   `ID_SND_RECEIPT_ACKED` and is removed (`RL.cpp:750-770`);
2. the datagram's message-number list is fetched from `datagramHistory`
   (`RL.cpp:772`); if present, the controller gets `OnAck` with `ping = now - timeSent` (`RL.cpp:779-784`), each message number goes through
   `RemovePacketFromResendListAndDeleteOlderReliableSequenced`
   (`RL.cpp:786-793`), and the history entry is cleared (`RL.cpp:795`).

`RemovePacketFromResendListAndDeleteOlderReliableSequenced`
(`RL.cpp:2437-2523`): look up `resendBuffer[number & 511]`, require the stored
number to match (`RL.cpp:2471-2473`, "May ask to remove twice"), clear the slot,
fire `ID_SND_RECEIPT_ACKED` if `reliability >= RELIABLE_WITH_ACK_RECEIPT` and
the message is unsplit or is the **last-index fragment** (`RL.cpp:2486-2496`),
unlink from the resend list subtracting its bytes (`RL.cpp:2510`), free. The
"DeleteOlderReliableSequenced" in the name is not implemented; the
superseding logic is commented out (`RL.cpp:2834-2896`).

**NAK** (`RL.cpp:807-857`): for every datagram number, `OnNAK`
(`CC.cpp:186-199`) and, for every message that datagram carried and that is
still in the resend buffer, `nextActionTime = now` (`RL.cpp:839-854`), so the
next `Update` resends it. A NAK is a hint; a message whose slot was reused is
left alone.

**Data** (`RL.cpp:858-1496`):

1. `OnGotPacket` (`CC.cpp:128-161`) compares the datagram number with the
   expected next: equal advances; greater yields `skippedMessageCount`, capped
   at 1000 and rejecting the datagram above 50 000; **less is accepted with no
   skip and no duplicate check** (`CC.cpp:155-158`). The receiver queues a NAK
   for every skipped number (`RL.cpp:871-877`).
2. The datagram number is queued for ack, "Ack even unreliable messages for
   congestion control" (`RL.cpp:880-886`, `RL.cpp:2528-2541`).
3. Messages are parsed one after another (`RL.cpp:888, 1491-1494`) and each is
   handled:
   - plugin hook (`RL.cpp:899-906`);
   - channel check for wire codes 1, 3, 4: `>= 32` drops it
     (`RL.cpp:920-937`);
   - **reliable duplicate and hole tracking** for wire codes 2, 3, 4
     (`RL.cpp:941-1044`), with the reason it is restricted to reliable at
     `RL.cpp:939-940`: `holeCount = number - base` (unsigned, 24-bit). Zero:
     expected, pop the queue head, `base++`. More than half the range: an old
     or duplicate message, dropped. Less than the queue size: either fills a
     hole (`true` to `false`) or is a duplicate, dropped. Otherwise push `true`
     for every missing number up to it and `false` for it, refusing beyond
     1 000 000 holes (`RL.cpp:1003-1017`). Then pop every leading `false`,
     advancing `base` (`RL.cpp:1039-1043`). The queue is shrunk when it is
     three times larger than used (`RL.cpp:1046-1048`).
   - **split** (`RL.cpp:1221-1242`): fragments of anything but the three
     ordered or sequenced types get `orderingChannel = 255`; the fragment is
     inserted and reassembly attempted; an incomplete message ends the
     fragment's handling here (section 8.2).
   - **ordered or sequenced** (`RL.cpp:1248-1479`), on the message's channel:
     - `orderingIndex == orderedReadIndex` (`RL.cpp:1283`): a sequenced message
       is delivered if its `sequencingIndex` is not older than
       `highestSequencedReadIndex`, which is then set to `sequencingIndex + 1`
       (`RL.cpp:1290-1320`; the `+ 1` was a 2012 fix for duplicates,
       `RL.cpp:1315-1316`), else dropped (`RL.cpp:1321-1341`). An ordered
       message is delivered, `orderedReadIndex++`, `highestSequencedReadIndex = 0`, and the channel's heap is drained while its head's
       `orderingIndex == orderedReadIndex` (`RL.cpp:1343-1427`): a popped
       ordered message does `orderedReadIndex++`, a popped sequenced one sets
       `highestSequencedReadIndex = sequencingIndex` (no `+ 1`,
       `RL.cpp:1415-1422`).
     - newer `orderingIndex` (`RL.cpp:1429-1463`): buffered in the channel heap
       with weight `(orderingIndex - heapIndexOffset) * 1 048 576 + sequencingIndex`, or `+ 1 048 575` for ordered, so that for one ordering
       index every sequenced message pops before the ordered one, in sequence
       order (`RL.cpp:1439-1446`). `heapIndexOffset` is captured when the heap
       is empty (`RL.cpp:1436-1437`).
     - older `orderingIndex` (`RL.cpp:1464-1477`): dropped.
   - anything else is delivered (`RL.cpp:1481-1486`).

"Delivered" means pushed on `outputQueue`; `Receive` pops it and hands the data
pointer up (`RL.cpp:1507-1530`).

**Code vs comment.** `PP.h:48` says UNRELIABLE is "Same as regular UDP, except
that it will also discard duplicate datagrams". It does not: the datagram
sequence check accepts a repeated number (`CC.cpp:155-158`), and the
duplicate test runs only for reliable messages (`RL.cpp:941`). A duplicated
datagram delivers its unreliable messages twice. The comment at
`RL.h:462-469` describes a queue of give-up times; the code stores booleans
(`RL.h:471`, `RL.cpp:1031-1032`).

## 7. Acks, NAKs and receipts

### 7.1 Range list encoding

`DS_RangeList.h:65-113` (write), `115-144` (read): `uint16 count`, then per
range `uint8 minEqualsMax`, `uint24 min`, and `uint24 max` only when they
differ. Ranges are merged on insert (`DS_RangeList.h:159-218`). The writer
stops at the bit budget it is given and, with `clearSerialized`, removes what
it wrote (`DS_RangeList.h:76, 102-110`), so `SendACKs` loops until the list is
empty, one datagram per pass (`RL.cpp:3616-3656`). The reader rejects `max < min` (`DS_RangeList.h:134-135`).

### 7.2 When acks go out

`ShouldSendACKs` (`CC.cpp:88-101`): immediately while no RTT sample exists,
else once `now >= oldestUnsentAck + SYN`, SYN = 10 ms (`CC.cpp:17-21`).
`oldestUnsentAck` is set by the first data datagram after an ack went out
(`CC.cpp:134-135, 273-279`). A completed split message flushes acks at once
"because for large files this can take a long time" (`RL.cpp:3244-3245`).
Acks are not counted against the window (`RL.cpp:3653-3654`).

### 7.3 How receipts fire

A receipt is a 5-byte message: `ID_SND_RECEIPT_ACKED` or `ID_SND_RECEIPT_LOSS`
(`MessageIdentifiers.h:96, 103`) then the `uint32` serial in native byte order
(`RL.cpp:757-762`, `RL.cpp:2490-2495`, `RL.cpp:1868-1873`). It is pushed on
the same `outputQueue` as data, so it arrives through `Receive()` interleaved
with messages.

| reliability | ACKED when | LOSS when |
|---|---|---|
| `UNRELIABLE_WITH_ACK_RECEIPT` | the datagram number it was sent in is acked (`RL.cpp:750-770`) | its RTO passes without that ack (`RL.cpp:1860-1881`); the message is never resent |
| `RELIABLE_WITH_ACK_RECEIPT` | the message's slot is cleared by an ack of any datagram that carried it (`RL.cpp:2486-2496`) | never (`PP.h:73`); the connection dies instead (`RL.cpp:1806`) |
| `RELIABLE_ORDERED_WITH_ACK_RECEIPT` | same | never (`PP.h:76`) |

For a split message the receipt fires when the fragment with the highest index
is acked (`RL.cpp:2487`), not when every fragment is. The other fragments are
still resent until acked, but the receipt can precede their acks. `PP.h:73`
says "after the message is delivered".

The receipt tells the sender the data reached the peer's reliability layer. It
does not say the peer's application read it; `PP.h:66, 73` are careful about
"arrives".

### 7.4 The serial on loopback

`RP.cpp:1332-1347`: a send to self delivers the data and, for receipt types,
an immediate ACKED receipt whose serial is copied from the global
`sendReceiptSerial` counter (`RP.cpp:1341`), not from the number `Send`
returns. With `forceReceiptNumber == 0` the counter was advanced by
`IncrementNextSendReceipt` at `RP.cpp:1330`, so the receipt carries one more
than the returned serial. With a non-zero `forceReceiptNumber` the counter is
not touched (`RP.cpp:1327-1328`), so the receipt carries whatever the counter
holds, unrelated to the forced number. Both go against `PP.h:66` ("Bytes 1-4
will contain the number returned from the Send() function"). Network sends
carry the right serial (`RL.cpp:1599`).

## 8. Large messages: split and reassemble

### 8.1 Split

`SplitPacket` (`RL.cpp:2928-3033`): `splitPacketCount = ceil(bytes / maximumSendBlockBytes)` with the block being `(MTU - 28 - 9) - 23`
(`RL.cpp:2939-2942`). One `InternalPacket` per fragment is taken from the pool
and the original's header copied into it (`RL.cpp:2957-2968`). Every fragment
points into the original data block through one ref-counted record
(`RL.cpp:2973-2986`, `RL.cpp:3838-3852`), freed when the last fragment is
(`RL.cpp:3879-3893`). Each fragment gets `splitPacketIndex`, the connection's
current `splitPacketId`, and the count (`RL.cpp:2995-2997`); the id then
increments (`RL.cpp:3001`). All fragments are pushed to the heap as a series at
the message's priority (`RL.cpp:3007-3025`). Each fragment is then an ordinary
message: numbered, acked and resent on its own.

A quirk: `RL.cpp:2966-2967` increments `internalOrderIndex` into the original,
not the copy, so fragments share one `messageInternalOrder`; it is unused on
the wire.

### 8.2 Reassemble

`InsertIntoSplitPacketList` (`RL.cpp:3038-3177`, `PREALLOCATE_LARGE_MESSAGES == 0` branch): find or create the `SplitPacketChannel` for `splitPacketId`
(`RL.cpp:3043-3061`), preallocate its list to `splitPacketCount`, **append**
the fragment (`RL.cpp:3141`, `DS_List.h:294-330`), record `lastUpdateTime`,
remember the index-0 fragment as `firstPacket`, and every
`splitMessageProgressInterval` fragments emit `ID_DOWNLOAD_PROGRESS` with
`{partCount, partTotal, partLength, first part data}` (`RL.cpp:3149-3174`,
`MessageIdentifiers.h:147-151`; default interval 0, `RP.cpp:2697`).

`BuildPacketFromSplitPacketList` (`RL.cpp:3225-3254`): complete when the list
size equals element 0's `splitPacketCount` (`RL.cpp:3241`). Then acks are
flushed, the message rebuilt (`RL.cpp:3184-3223`): a header copy of element 0
(`CreateInternalPacketCopy`, `RL.cpp:3289-3322`, carries ordering index,
sequencing index, channel, message number, priority, reliability), a malloc of
the total (`RL.cpp:3203`), and **a concatenation of the fragments in list order**
(`RL.cpp:3206-3212`). The channel is removed. The rebuilt message then goes
through the ordered or sequenced dispatch like any other (`RL.cpp:1221-1250`).

Three things to carry forward from this:

- **Reassembly order is arrival order, not `splitPacketIndex` order.** `RL.h:542`
  declares `SortSplitPacketList`; nothing defines or calls it (checked across
  `Source/`). The commented-out `splitPacketPartLength` at `RL.cpp:3201` is the
  remnant of index-based placement. A fragment that arrives out of order (a
  resent one after its successors) is concatenated in the wrong place. The
  duplicate check at `RL.cpp:941-1044` keeps duplicate fragments out but does
  not order them. With a depth-1 clone the history that would say when this
  changed is not available; this document states what the code does.
- **Incomplete reassemblies never expire.** `DeleteOldUnreliableSplitPackets`
  is commented out (`RL.cpp:2240-2241`, `RL.cpp:3255-3283`); `lastUpdateTime`
  is recorded and never read. Since every fragment is reliable after the
  upgrade (`RL.cpp:1609-1621`), completion is expected, but a stale channel
  with a reused 16-bit id would absorb the new message's fragments
  (`RL.cpp:3043-3061`; `IP.h:55` calls the id "the maximum number of split
  messages we can send simultaneously per connection").
- **Memory per fragment is a malloc on receive** (`RL.cpp:2773`), plus a malloc
  of the whole on completion (`RL.cpp:3203`), plus the copy. The preallocating
  alternative is off because it "is vulnerable to attackers causing the host
  to run out of memory" (`RakNetDefines.h:125-126`).
- **The `PREALLOCATE_LARGE_MESSAGES == 1` branch is not a model either.** It
  sizes the whole buffer from whichever fragment arrives first,
  `dataBitLength * splitPacketCount` (`RL.cpp:3052`); if that is the short
  last fragment the buffer is too small and the later `index * stride` copy
  at `RL.cpp:3089` writes past it, with the stride taken from fragment 0
  (`RL.cpp:3072`). Fragments that arrive before fragment 0 are pushed on a
  list (`RL.cpp:3094`); when fragment 0 arrives the loop at `RL.cpp:3074-3079`
  copies *fragment 0's* bytes to offset 0 once per listed fragment and frees
  each listed fragment without copying it, yet they were counted at
  `RL.cpp:3065`, so the message completes (`RL.cpp:3239`) with regions never
  written. Only the after-fragment-0 branch (`RL.cpp:3089`) places by index.

## 9. The congestion window

`CCRakNetSlidingWindow` (`CC.h:81-214`, `CC.cpp`). State: `cwnd`, `ssThresh`,
`oldestUnsentAck`, `nextDatagramSequenceNumber`, `nextCongestionControlBlock`,
`backoffThisBlock`, `speedUpThisBlock`, `expectedNextSequenceNumber`,
`_isContinuousSend`, `lastRtt`, `estimatedRTT`, `deviationRtt`
(`CC.h:189-212`).

- **Init** (`CC.cpp:43-58`): `cwnd = MTU`, `ssThresh = 0`, RTT unknown. The
  MTU passed is the connection's `MTUSize - 28` (`RL.cpp:361`, `CC.h:51`).
- **Slow start** is `cwnd <= ssThresh || ssThresh == 0` (`CC.cpp:367-370`).
- **OnAck** (`CC.cpp:201-256`), once per acked datagram that is in history:
  RTT estimate and deviation move 5 % toward each sample (`CC.cpp:210-222`).
  If the sender was not continuously sending, nothing else happens
  (`CC.cpp:226-227`). A new "period" starts when the acked number passes
  `nextCongestionControlBlock`, resetting the per-period flags
  (`CC.cpp:229-237`). In slow start `cwnd += MTU`, capped at `ssThresh + MTU^2/cwnd` when it crosses `ssThresh` (`CC.cpp:239-248`); in avoidance
  `cwnd += MTU^2/cwnd` once per period (`CC.cpp:249-255`).
- **OnResend** (`CC.cpp:163-184`): if continuously sending, not already backed
  off this period, and `cwnd > 2 * MTU`: `ssThresh = cwnd / 2` (at least MTU),
  `cwnd = MTU`, period marked backed off. The spec's half-cwnd restart is
  rejected in the comment at `CC.cpp:170-171`.
- **OnNAK** (`CC.cpp:186-199`): under the same guard as `OnResend`
  (continuously sending and not yet backed off this period, `CC.cpp:191`),
  `ssThresh = cwnd / 2`; `cwnd` is not changed and the period is not marked
  backed off.
- **Bandwidth** (`CC.cpp:66-86`): new data may send `cwnd - unacknowledgedBytes` bytes, resends may send `unacknowledgedBytes`.
  `unacknowledgedBytes` counts reliable message bytes in the resend list
  (`RL.cpp:3585, 3573`), not datagram bytes; unreliable traffic is never
  charged, so **unreliable sends are never throttled by the window**, only by
  the optional bits-per-second cap (`RL.cpp:1995`).
- **RTO** (`CC.cpp:288-314`): 2 s while RTT is unknown; else `2 * RTT + 4 * deviation + 30 ms`, capped at 2 s. The per-message `timesSent` parameter is
  ignored (`CC.cpp:290`), so there is no exponential back-off.
- **Datagram numbers** (`CC.cpp:103-113`) and the expected-next check
  (`CC.cpp:128-161`) live here.
- No-ops: `Update`, `OnSendBytes`, `OnGotPacketPair`, `OnDuplicateAck`,
  `OnSendNACK`; `OnSendAckGetBAndAS` always says no (`CC.cpp:60-64, 115-126, 258-286`). Packet pairs are disabled (`RL.cpp:407-408`, `RL.cpp:3495-3504`).

**Code vs comment.** `CC.h:151-154` says `RTO = (RTT + 4 * RTTVar) + SYN`,
"If we have been continuously sending for the last RTO, and no ACK or NAK at
all, SND *= 2", and "Minimum value is 100 milliseconds"; the code is `2 * RTT + 4 * dev + 30 ms`, no doubling, and the minimum is commented out
(`CC.cpp:294, 298, 306-313`). `CC.h:109` says every 15th and 16th packet is
a pair; pairs are off. `CC.h:176, 184` return 0 for the window limit and
bytes-per-second limit.

## 10. Each reliability, end to end

What each one costs and does, assembled from the sections above.

**UNRELIABLE (0).** Header 3 bytes. No message number, no indices. Sent once
in the next datagram the window lets out (unreliable traffic is not charged to
the window); freed after the send (`RL.cpp:3519-3534`). Receiver: no duplicate
check, delivered on arrival. Optional sender-side cull of unsent messages
(`RL.cpp:1745-1793`). A message above the split threshold becomes RELIABLE
(`RL.cpp:1613-1614`).

**UNRELIABLE_SEQUENCED (1).** Header 10 bytes: ordering index, sequencing
index, channel. No message number; never enters the receiver's reliable window
(`RL.cpp:941`), which is what keeps that window hole-free. Receiver on the
channel: delivered only if `orderingIndex == orderedReadIndex` and
`sequencingIndex >= highestSequencedReadIndex`, or buffered if the ordering
index is newer (waits for the ordered message that opens that epoch); older
dropped. Split becomes RELIABLE_SEQUENCED (`RL.cpp:1617-1618`).

**RELIABLE (2).** Header 6 bytes: message number. Assigned at datagram build;
in `resendBuffer` and the resend list until a datagram carrying it is acked;
resent at RTO or on NAK; at most 512 in flight. Receiver: duplicate and hole
tracked on the message number, delivered on first arrival in any order.
Connection dies after `timeoutTime` of total silence while it waits.

**RELIABLE_ORDERED (3).** Header 10 bytes: message number, ordering index,
channel. Everything RELIABLE does, then the channel dispatch: delivered when
`orderingIndex == orderedReadIndex`, which also releases buffered successors
from the heap in (ordering, sequencing) order; buffered if newer; dropped if
older (a late resend). Delivering it resets the channel's sequenced watermark
to 0.

**RELIABLE_SEQUENCED (4).** Header 13 bytes: message number, sequencing index,
ordering index, channel. Resent until the datagram carrying it is acked, and
the ack is sent on datagram arrival (`RL.cpp:880-886`) before the sequencing
decision, so **a stale RELIABLE_SEQUENCED message is acked and then discarded**
(`RL.cpp:1321-1341`). The sender cannot tell. Receiver dispatch as
UNRELIABLE_SEQUENCED. The idea of dropping queued older sequenced sends when a
newer one is queued is disabled (`RL.cpp:1637-1649`).

**UNRELIABLE_WITH_ACK_RECEIPT (5).** Wire code 0, header 3 bytes. Sent once;
the layer keeps a `{datagram number, serial, deadline}` node, not the message
(`RL.cpp:2092-2099`). ACKED if that datagram number is acked; LOSS at the RTO
otherwise (`RL.cpp:1860-1881`); the deadline is one RTO, 2 s when the RTT is
unknown. Note that the ack is per datagram, so a datagram that carried this
message and other things acks it as a whole. Split becomes
RELIABLE_WITH_ACK_RECEIPT (`RL.cpp:1615-1616`).

**RELIABLE_WITH_ACK_RECEIPT (6).** Wire code 2, header 6 bytes. As RELIABLE;
ACKED when its resend slot is cleared; never LOSS.

**RELIABLE_ORDERED_WITH_ACK_RECEIPT (7).** Wire code 3, header 10 bytes. As
RELIABLE_ORDERED; ACKED when its slot is cleared, which is before the receiver
necessarily delivers it in order; never LOSS.

RakPeer itself uses `UNRELIABLE` for pings (`RP.cpp:2075, 5927`), `RELIABLE`
for the keep-alive ping (`RP.cpp:5864`), and `RELIABLE_ORDERED` on channel 0
for the connection handshake (`RP.cpp:3553, 6201`); the disconnect notice is
`RELIABLE_ORDERED` on the channel the caller passes to `CloseConnection`,
default 0 (`RP.cpp:3560-3563`, `RPI.h:262`). So channel 0's ordered index is
shared with the protocol.

## 11. Where the code and its comments disagree

Collected here from the sections above, so the reader does not inherit the
comments.

1. `PP.h:48`: UNRELIABLE "will also discard duplicate datagrams". It does not
   (`CC.cpp:155-158`, `RL.cpp:941`).
2. `PP.h:48, 55, 62`: stated overheads. The code writes whole bytes: 3, 6, 10,
   13 (`RL.cpp:2618-2658`); RELIABLE_SEQUENCED is not the same size as
   UNRELIABLE_SEQUENCED.
3. `PP.h:23-36`: "10 millisecond intervals", "sent immediately", 2:1 ratios.
   No timer; heap weights only (`RL.cpp:3926-3949`); immediate is a thread wake
   (`RP.cpp:4218-4222`).
4. `PP.h:66, 73`: receipt "Bytes 1-4 will contain the number returned from
   the Send() function". True on the network; on loopback the receipt carries
   the global counter (`RP.cpp:1341`), one more than the returned serial, or
   an unrelated number when `forceReceiptNumber` is used (`RP.cpp:1327-1330`).
5. `PP.h:73`: ACKED "after the message is delivered". For a split message it
   fires on the ack of the last-index fragment (`RL.cpp:2487`).
6. `RL.cpp:2647`: sequencing index "Used for ... RELIABLE_ORDERED". Not written
   for it (`RL.cpp:2643-2648`).
7. `RL.cpp:2733` tests a reliability that is never on the wire
   (`RL.cpp:2619-2626, 2712`).
8. `RL.cpp:2437`, the function name promises deleting older reliable sequenced
   messages; the body does not, and the superseding code is commented out
   (`RL.cpp:1644-1649, 2834-2896`).
9. `RL.h:542` declares `SortSplitPacketList`; undefined and uncalled.
   Reassembly is in arrival order (`RL.cpp:3206-3212`).
10. `RL.h:462-469`: the received-packet queue "stores the time to stop
    waiting"; it stores booleans (`RL.h:471`).
11. `RL.h:349`: history "restricted to DATAGRAM_MESSAGE_ID_ARRAY_LENGTH";
    the pop happens at `> 512`, so 513 (`RL.cpp:3800`).
12. `RL.cpp:133-141`: data header budgeted at 9 bytes, written as 4
    (`RL.cpp:170-182`).
13. `RL.cpp:2763` hard-codes 32 where `RL.h:52` defines the constant; changing
    the macro would not change the receiver's bound.
14. `CC.h:151-154`: RTO formula, doubling and 100 ms minimum. Code:
    `2 * RTT + 4 * dev + 30 ms`, cap 2 s, no minimum (`CC.cpp:288-314`).
15. `CC.h:109`: packet pairs every 15th and 16th; disabled (`RL.cpp:407-408`).
16. `RPI.h:198`: `char orderingChannel`, signed, with an assert that cannot
    catch negatives (`RP.cpp:1315`); the layer silently maps 32 and above to 0
    (`RL.cpp:1563-1564`).
17. `IP.h:55`: the split id is "the maximum number of split messages we can
    send simultaneously"; nothing enforces it and the receiver keys
    reassembly by the id alone (`RL.cpp:3043-3061`).
18. `RL.cpp:1315-1317` records that the sequenced watermark must be
    `sequencingIndex + 1` so "a duplicated ... packet would [not] be returned
    to the user"; the heap-drain path at `RL.cpp:1421` sets it to
    `sequencingIndex` without the `+ 1`, so a duplicate of a sequenced message
    released from the heap passes the check at `RL.cpp:1290` and is delivered
    twice. For `RELIABLE_SEQUENCED` the message-number check at
    `RL.cpp:941-1044` catches a resent duplicate first; for
    `UNRELIABLE_SEQUENCED` nothing does.
19. `RL.h:423-439` (point 2) has the sender reset the sequenced index every
    time the ordered index advances. The receiver mirrors that on the direct
    path (`RL.cpp:1372-1373`) but the heap-drain path advances
    `orderedReadIndex` without resetting the watermark (`RL.cpp:1415-1418`).
    Sequenced messages of the new epoch arriving after an ordered message was
    released from the heap are tested against the old epoch's watermark and
    may be dropped as "older".
20. `RL.cpp:1557-1564` is headed "Fix any bad parameters"; the priority test
    uses `>` (`RL.cpp:1560`) where the reliability test uses the last valid
    value, so priority 4 is not repaired and indexes
    `outgoingPacketBufferNextWeights[4]` (`RL.h:402`, `RL.cpp:3934`), one past
    the end. The assert at `RP.cpp:1314` has the same `>`.
21. `RakNetDefines.h:124-125` presents `PREALLOCATE_LARGE_MESSAGES` as a trade
    of memory for memcpy time. The branch also loses the bytes of fragments
    that arrive before fragment 0 (`RL.cpp:3074-3079`) and sizes the buffer
    from the first fragment seen (`RL.cpp:3052`); section 8.2.

## 12. What RocketNet's design must keep open

The task: a client and server message layer on the transports, reliability
chosen per send as RakNet does, `n` ordering channels with a compile-time
maximum (`ROCKETNET_MAX_CHANNELS`, 32 by default; the name is the card's, not
yet in this tree) and a per-connection runtime count at or under it,
UNRELIABLE and RELIABLE_ORDERED first, then the other six. No malloc or free
on a send or receive path; every buffer from the connection's allocator or the
transport's frame.

What the transport gives, from `sdk/rocketnet.h` and `USAGE.md`: a per-packet
`packet_id` handed out at `start_packet` (`rocketnet.h:880-888`) and a
delivery notification batch of acked `packet_id`s per update
(`rocketnet.h:646, 966-976`), "The transport tells; it never resends"
(`rocketnet.h:970-971`); a zero-copy frame of up to `ROCKETNET_MAX_PAYLOAD_BYTES`
= 1416 (`rocketnet.h`) and a copying fragment path above it up to the
configured maximum, default 16384, hard ceiling 256 fragments
(`rocketnet.h:423-433`, `rocketnet_fragment.h:29`), with unreliable semantics,
"A lost fragment loses the whole payload" (`rocketnet.h:833-834`); a delivered
batch that is replaced every update, with reassembled payloads in arrival
position (`rocketnet.h:785-791`, `USAGE.md:68-81`); `start_packet` /
`finish_packet` / `abort_packet` on game threads between `update` and
`send_packets` (`rocketnet.h:841-846`); a replay window on the session
sequence (`USAGE.md:171, 283`), so the transport already drops duplicated
packets; per-connection acks encoded as up to 255 ranges
(`rocketnet_ack.h:49`); and a global allocator hook (`rocketnet.h:319-329`)
with per-create allocation.

The invariants below are what RakNet's code shows a per-send reliability
needs. Each is stated so that the first two types can be built against it
without having to be rebuilt for the rest.

**I1. The message header carries the reliability code from the first
version, and the receiver dispatches on it.** RakNet's receiver is a switch
on the 3-bit code (`RL.cpp:920-1486`); every later type is a new case, not a
new header. Reserve the eight values now. An unknown or disallowed code is
dropped and counted (`RL.cpp:2760-2770`), never read as one of the two shipped
types.

**I2. The header is variable by reliability, not fixed.** UNRELIABLE is 3
bytes with no indices; RELIABLE_ORDERED adds message number, ordering index
and channel; RELIABLE has a number but no channel; the sequenced types have
indices but UNRELIABLE_SEQUENCED has no number (`RL.cpp:2634-2658`). A fixed
header sized for RELIABLE_ORDERED would either waste 7 bytes on every
unreliable message or leave no room for the sequencing index. Write the
layout as "flags, length, then fields the code implies".

**I3. The reliable message number and the ordering indices are two
independent counter families.** The message number is consumed only by
reliable sends and only at datagram build (`RL.cpp:2060-2061, 2090`); the
ordering and sequencing indices are consumed per channel at `Send`
(`RL.cpp:1625-1657`). The receiver's duplicate and hole window is advanced only
by reliable messages (`RL.cpp:941`). `RL.cpp:2050` records the bug that came
from letting an unreliable type take a message number: a permanent hole that
eventually stopped all delivery. So: an unreliable send of any kind never takes
a number from the reliable sequence, and nothing but a reliable message moves
the receive window.

**I4. The reliable message number is not the transport sequence.** A resend is
a new transport packet with a new sequence and a new `packet_id`, carrying the
same message number. The transport's replay window dedupes packets; the message
layer dedupes resent messages on the message number (`RL.cpp:946-1000`). Keep
both; neither replaces the other.

**I5. Per channel, both an ordered index and a sequenced index, coupled as
RakNet couples them, from the first version.** Even with only RELIABLE_ORDERED
shipped, the ordered send must reset the channel's sequenced write index to 0
(`RL.cpp:1656`) and the ordered delivery must reset the sequenced read
watermark to 0 (`RL.cpp:1373`), so that a later UNRELIABLE_SEQUENCED on the
same channel is "newer than everything before the last ordered message" from
day one. The per-channel struct holds four counters (`RL.h:441-450`); allocate
it that way now. If the sequenced reset is left out of the first version,
adding sequenced later changes the ordered path's behaviour. And apply one
watermark rule on both delivery paths (section 11 items 18 and 19), so the
sequenced types inherit neither RakNet's duplicate nor its stale watermark
after a heap-released ordered message.

**I6. The reorder buffer is keyed by (ordering index, sequencing index), not
by ordering index alone.** RakNet's heap weight is `ordering_gap * 2^20 + sequencing`, with ordered messages at the top of their epoch
(`RL.cpp:1439-1446`), and the drain releases sequenced messages of the next
epoch before its ordered message (`RL.cpp:1376-1423`). A buffer that holds one
message per ordering index cannot hold the sequenced messages that arrive ahead
of their epoch's ordered message, and those are the normal case for a state
stream sharing a channel with an ordered one.

**I7. The reorder buffer and the resend state are bounded at create, per
connection and per channel.** RakNet bounds the in-flight reliable messages at
512 by a fixed array (`RL.cpp:3758-3765`) and the datagram history at 513
(`RL.cpp:3800`), and stalls new sends when the bound is hit
(`RL.cpp:1983, 2122-2123`): that is the shape "no live allocation" takes. It
does not bound its heaps, its hole queue (up to 1 000 000, `RL.cpp:1003`), its
reassembly list or its output queue, and `RakNetDefines.h:125-126` names the
attack that costs. With 1024 or 10 000 channels the per-channel buffers cannot
each be large; size them per connection from the runtime channel count, with
a stated policy when full (stall the sender, as RakNet does for reliable; drop
and count, as RocketNet does for frames). What is in flight must be countable
by the sender from its own state, as `unacknowledgedBytes` is
(`RL.h:528`).

**I8. Acks are per packet, and the sender keeps a packet-to-messages map.**
RakNet acks datagrams only (`RL.cpp:2534-2535`) and turns an ack into message
acks through `datagramHistory` (`RL.cpp:2174-2186, 772-795`). RocketNet's
`acked_packets` is the same primitive, keyed by `packet_id`. A packet mixes
reliabilities (`RL.cpp:1998-2136`), so the map is `packet_id` to a list of
message records, not `packet_id` to one message, even if the first version
puts one reliable message per packet. Aggregated small unreliable messages and
a reliable message can then share a packet later without changing the map.

**I9. Every queued message record carries a receipt serial, and the serial is
issued at send time by the API, not by the layer.** RakNet stamps
`sendReceiptSerial` on every `InternalPacket` (`RL.cpp:1599`) and issues it in
`RakPeer::Send` before the message is queued (`RP.cpp:1326-1330`), so the
caller has the number the receipt will name. The receipt variants are the
same code paths as their base types plus a check at ack time
(`RL.cpp:2486-2496`). If the first version's message record has no serial
field and `Send` returns nothing, adding receipts means a second bookkeeping
path. Reserve the field and the return value.

**I10. Unreliable-with-receipt needs a record that outlives the frame.**
RakNet keeps a small `{packet, serial, deadline}` node, not the message
(`RL.h:355-365`, `RL.cpp:2092-2099`), and fires LOSS from a timer
(`RL.cpp:1860-1881`). In RocketNet an unreliable send is a transport frame that
is gone at `finish_packet`; the record must live in the connection's allocator.
The ack path (I8) and a per-update deadline sweep are the two things it needs.
This is the one receipt that needs a timer; the reliable receipts need only the
ack path.

**I11. A reliable message larger than a frame is split and resent somewhere,
and the design must say where.** RakNet splits in the layer
(`RL.cpp:2928-3033`), numbers and resends each fragment on its own, and
upgrades unreliable splits to reliable (`RL.cpp:1609-1621`). RocketNet's
transport offers a second shape: its fragmenter carries a payload up to the
configured maximum as type 9 fragments under one `packet_id`, that id surfaces
in the acked batch only when every fragment is acked, and a lost fragment loses
the whole payload (`rocketnet.h:830-834`, `USAGE.md:120-124`). So a reliable
message could ride the transport's fragments and be resent whole when its id
does not come back. The trade is whole-payload resend (one record per message,
the transport reassembles, a lost fragment costs the whole payload again and
the arena route copies) against per-fragment resend (RakNet's: a record per
fragment, only the lost part is resent, the layer owns reassembly). Whichever
is chosen, three things hold. If the layer ever splits, the split fields (id,
index, count) are independent of reliability and sit behind one flag
(`RL.cpp:2630, 2660-2671`); all fragments of one message share its ordering
and sequencing indices (`RL.cpp:1625-1657` then `2963`), so a fragmented
ordered message takes one ordering slot; and reassembly is by index, never by
arrival (section 8.2's bug), in slots bounded per connection as the
transport's already are (`rocketnet.h:433`), with a reused split id never
matching a stale incomplete reassembly. Whether an UNRELIABLE message above a
frame uses the transport's fragmenter or is upgraded as RakNet does is the
same kind of choice. Make both explicitly, before RELIABLE ships.

**I12. Sequenced types are acked on arrival and may still be discarded.** The
ack is queued before dispatch (`RL.cpp:880-886` precedes `RL.cpp:1248`), and
RakNet refused sequenced receipts for that reason (`PP.h:70, 80`,
`RL.cpp:2047-2053`). The sender's resend state must therefore be cleared by the
ack regardless of what the receiver did with the message. If RocketNet ever
wants a receipt that means "delivered to the application", that is a new
receiver-side report, not an ack.

**I13. Unreliable traffic is not charged to the reliable window.** RakNet
counts only reliable bytes in flight (`RL.cpp:3585, 3573`) and never throttles
unreliable sends by the window (`RL.cpp:1894-1895`, `CC.cpp:66-86`). Whatever
RocketNet does for flow control on the reliable types, UNRELIABLE must keep
the transport's rate, or the game's state stream stalls behind a reliable
backlog. Decide where the congestion signal lives (the transport has RTT and
loss per connection, `rocketnet.h:954-964`) before RELIABLE needs an RTO.

**I14. The RTO and the dead-connection decision are per connection, not per
type.** One RTT estimate (`CC.cpp:210-222`), one RTO (`CC.cpp:288-314`), one
`timeoutTime` (`RL.cpp:1799-1808`). RocketNet's connection already has the
timeout; the message layer needs only the RTO, and only once RELIABLE exists.

**I15. The channel number is a per-connection runtime bound checked on both
ends, and the field width follows the compile-time maximum.** RakNet writes a
byte, needs 5 bits (`RL.cpp:2657`), checks against a hard-coded 32 on receive
(`RL.cpp:2763, 926`) and silently maps out-of-range to channel 0 on send
(`RL.cpp:1563-1564`). RocketNet: refuse on send, drop and count on receive, and
compare with the connection's runtime count, not the compile-time maximum. At
`ROCKETNET_MAX_CHANNELS` above 256 the field is two bytes; derive it from the
constant so the stress runs at 1024 and 10 000 change one number.

**I16. The protocol does not share a user channel.** RakNet sends its own
handshake and disconnect on `RELIABLE_ORDERED` channel 0 (`RP.cpp:3553, 3563, 6201`), so a user's channel 0 ordered index is advanced by the protocol.
RocketNet's connection protocol is below the message layer; keep it there.

**I17. The receive side hands up messages, not packets, and in one stream.**
RakNet's `Receive()` returns one message at a time with system notices and
receipts interleaved (`RL.cpp:1507-1530`, `RP.cpp:6226`). RocketNet's transport
delivers a batch of payloads per update (`rocketnet.h:785-809`); the message
layer's drain is a loop over those payloads producing messages, with the batch
lifetime the same ("valid until the next update"). An ordered message released
from the reorder buffer in a later update is delivered then; copy-out semantics
for what a caller keeps are the transport's already (`USAGE.md:73-76`).

## 13. Language bindings

RakNet's C# binding is SWIG-generated from its public headers. RocketNet
offers C and C++ only: `sdk/rocketnet.h` is the public C surface, and the C++
headers sit beside it. There is no C# or managed binding in this tree.

## 14. Not done, and open for a harness

- RakNet was read, not built or run. Five findings bear on design and would be
  cheap to confirm with a two-peer harness and a lossy link: arrival-order
  reassembly (section 8.2), the sequenced `+ 1` asymmetry and the heap-drain
  watermark (section 11 items 18 and 19), the receipt on the last-index
  fragment (section 7.3), and the unthrottled unreliable path (section 9).
- `CCRakNetUDT`, the non-default congestion controller, was not read.
- `RakPeer.cpp` was read for `Send`, `SendList`, the buffered command path,
  `SendImmediate`, `Receive`, `DeallocatePacket`, the dispatch into and drain
  of the reliability layer and the receipt serial; the handshake, pings beyond
  the keep-alive, and plugins were not.
- The SWIG `.i` files were read for the include list, the ignores and the
  `byte[]` typemap. RakNet's C# binding has no counterpart in RocketNet, which
  offers C and C++ only.

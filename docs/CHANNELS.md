# Channels: the message layer above the transports

The design every message-layer card implements. It puts a client and a server
on top of `client_transport` and `server_transport` that send **messages**
across `n` **ordering channels**, with the **reliability chosen per send** from
RakNet's eight types. Only `UNRELIABLE` and `RELIABLE_ORDERED` ship first; then
the client and server objects, so the game can integrate and prove it; then the
other six. This document shows that the other six slot in without changing the
wire or the per-channel state, layer by layer, with the study beside it.

Read [docs/RAKNET-RELIABILITY.md](RAKNET-RELIABILITY.md) (the study) with this.
Its invariants I1 to I17 (study section 12) are cited here as **I1** .. **I17**,
and every one is answered in the table in section 13. Transport facts are cited
as `file:line` in this repository at the commit this document lands on.

Status: a design. Nothing in it is built. The layer list (section 2) goes to
Glenn for correction before layer 1 starts, per the standing practice.

---

## 1. What the layer stands on (layer 0, given)

The transports are finished and are not changed by this design except for the
one accessor named in section 11.4. What the message layer uses:

- **A zero-copy frame per packet.** `start_packet` hands back a 16-aligned frame;
  the payload is written at `frame + payload_index` and sealed in place at
  `finish_packet` (`sdk/rocketnet.h:815-821`). A request at or under
  `ROCKETNET_MAX_PAYLOAD_BYTES` = 1192 (`sdk/rocketnet.h:420`) is a real wire
  frame with zero copies; a larger request, up to the configured maximum payload
  (default 16384, `sdk/rocketnet.h:432`), routes through the transport's arena
  and is split into type 9 fragments at finish, one copy
  (`sdk/rocketnet.h:822-834`, `USAGE.md:118-124`).
- **A packet id at start.** The context's `packet_id` is assigned when
  `start_packet` succeeds: the private lease issues a process-unique ticket
  (`sdk/rocketnet_packet_lease.h:39-55`, from the global counter at
  `sdk/rocketnet.cpp:521-529`). Ids on one connection are unique but **not
  contiguous**: every transport in the process draws from the same counter.
- **Delivery notification, never resend.** Each update publishes the packet ids
  newly acknowledged (`sdk/rocketnet.h:646`, `sdk/rocketnet.h:966-976`). "The
  transport tells; it never resends" (`sdk/rocketnet.h:970`). A fragmented
  payload's id surfaces only when every fragment is acknowledged
  (`sdk/rocketnet.h:833-834`). There is **no negative notification**: missing a
  notification does not prove loss (`USAGE.md:283-291`), and history overflow
  silently ages records out (`sdk/rocketnet_ack.h:76-79`). The message layer
  therefore resends on a timer, never on a NAK.
- **Every opened session packet is acknowledged.** The ack block is built from
  every opened session sequence (STANDARD.md _Elicitation_, lines 1793-1809).
  The transport acks a packet the moment it opens it, before any layer above has
  looked inside. **This is the fact the whole receive side is built around**: a
  reliable message whose packet was opened is, to its sender, delivered. The
  receiver can never drop it afterwards for lack of space (section 5.4).
- **A delivered batch per update.** Payloads from the last update, by index or as
  parallel arrays, valid until the next update and not one instruction longer
  (`sdk/rocketnet.h:785-809`, `USAGE.md:68-94`). Replay protection already drops
  duplicated packets (`USAGE.md:171`, STANDARD.md _Replay Protection_).
- **Bounded per-update receive.** At most
  `ROCKETNET_MAX_PAYLOADS_PER_UPDATE` = 16 x `ROCKETNET_MAX_CLIENTS` payloads in
  one server update (`sdk/rocketnet_constants.h:16, 53`), and
  `ROCKETNET_NUM_CLIENT_PACKETS` = 1024 on the client
  (`sdk/rocketnet_constants.h:78`).
- **The fill-phase thread contract.** `update` and `send_packets` are
  main-thread; `start_packet`, `finish_packet` and `abort_packet` run on any
  number of game threads strictly between them, joined before the flush
  (`sdk/rocketnet.h:841-846`, `USAGE.md:39-55`). Lease metadata bounds one fill:
  512 acquisitions on the client, 65,536 on the server (`USAGE.md:298-301`,
  `sdk/rocketnet_client_transport.cpp:102`,
  `sdk/rocketnet_server_transport.cpp:134`).
- **Time is a value.** "The library never reads a clock of its own on these
  paths" (`sdk/rocketnet.h:542-544`).
- **The allocator hook.** `rocketnet_malloc` / `rocketnet_free`, 64-byte aligned
  (`sdk/rocketnet.h:319-329`). The message layer calls it at create and destroy
  and nowhere else.

## 2. The layers, bottom up

| # | layer | owns | in one line |
|---|---|---|---|
| 0 | transports | sockets, keys, acks, fragments | given; section 1 |
| 1 | the connection allocator | every byte and record a connection will ever use | one block per connection, carved at create into fixed pools |
| 2 | message records in a packet | the wire layout | records end to end, or one message zero-copy with its header as a trailer |
| 3 | per-send reliability and ordering channels | message numbers, the packet map, resend, dedupe, the per-channel counters and reorder lists | the RakNet machinery, bounded, with the sender keeping the receiver's capacity |
| 4 | the eight types | the dispatch tables | two enabled first; six are table rows, not code paths |
| 5 | blocks | reliable messages larger than a packet | fixed 1024-byte stride, resent per fragment, reassembled by index |
| 6 | scheduling | what goes in each packet | resends, then four priorities by RakNet's weights |
| 7 | the endpoint machine, and the client and server objects | binding the machine to the transports | a pure machine per connection; the objects are the only place with sockets and time |
| 8 | the C API | `sdk/rocketnet.h` | section 11 |

Each layer below says what it owns, what it guarantees, and how it is checked.
The TLA+ modules are in section 12; the reachability of every type is in
section 13.

### The rule every layer keeps: no transition on one unreliable packet

**No state transition between peers rides a single unreliable packet.** Every
packet that announces something the peer must act on (a channel's first
message, a block's start, a configuration, a reset, a disconnect) is carried on
the reliable path, or re-sent on a timer until the peer's next-state packet
confirms it, as netcode does for its handshake. A first-packet loss never hangs
either side. How each layer keeps it:

- **Channels are never opened.** Every channel below the connection's count
  exists, zeroed, from the first update of the connection (section 5.3). There
  is no channel-start packet to lose.
- **A block has no start packet.** Every fragment is a reliable record carrying
  the block id, its index and the count; whichever fragment arrives first opens
  the slot (section 7).
- **The configuration exchange is a reliable control record**, resent on the
  timer until its packet is acknowledged (section 5.6).
- **Reset is local.** A connection's message state is created fresh when the
  transport connects and destroyed when it disconnects. The transport's
  handshake already retransmits at the send rate until the peer's next-state
  packet arrives (STANDARD.md _Timing_, lines 1981-2023), and its disconnect is
  ten redundant packets backed by the connection timeout. The message layer adds
  no reset message of its own.
- **Unreliable messages carry no state the peer keeps** (sequenced ones carry a
  watermark that only moves forward; losing one changes nothing).

### The rule every state machine keeps: a pure module

Every state machine in this design (the allocator's pools, the endpoint, the
block reassembler, the scheduler) is a **pure module**: its state is in one
struct, it is driven by events (a received payload's bytes, an acknowledged
packet id, an application call, the time passed in as a `double`), and it
answers with actions the caller performs (bytes to put in this frame, messages
to deliver, receipts to report, the earliest time it next needs an event). It
holds no socket, reads no clock and allocates nothing. A unit test drives it end
to end with two instances and a byte array between them. **The client and
server objects (layer 7) are the only place a machine is bound to sockets and
time** (Glenn, 2026-10-05 ~10:30 PM).

## 3. Layer 1: the connection allocator

**Owns.** One block of memory per connection, from `rocketnet_malloc` at
create, carved into fixed pools: channel states, send records, send byte chunks,
the in-flight number table, the receive number window, the packet map and its
reference ring, reorder nodes and slots, block slots, the unreliable staging
buffer, receipt records. The client owns one connection; the server owns
`max_clients`, one per slot, all carved at server create like the transport's
own per-slot tables (`sdk/rocketnet.h:671-680`). Nothing is allocated on a send
or receive path, ever. Server-wide, one more pool: the delivered-message list
(section 9.3).

**Guarantees.**

- Every acquire and release is O(1): free lists of 32-bit indices, never
  pointers on the wire, never a search.
- Exhaustion is a refusal with a counter, never a wait and never a corruption.
  Which refusals are possible is fixed by the layer above: a send can be refused
  (`MESSAGE_SEND_QUEUE_FULL`, `MESSAGE_SEND_BUFFER_FULL`); a receive-side pool
  **cannot** run out for a reliable message, because the sender keeps the
  receiver's capacity (section 5.4); it can run out for an unreliable sequenced
  message, which is then dropped and counted.
- Every element is in exactly one owner at a time (free, queued, in flight,
  acknowledged-and-held, buffered, delivered-pending-release). Resetting a
  connection returns every element to free in one pass, with no leak.
- Pools are sized from the configuration alone, so the required memory is
  known before create (`rocketnet_message_required_memory`, section 11), the
  same shape as `rocketnet_server_transport_required_memory`
  (`sdk/rocketnet.h:722-730`).

**The send bytes are chunked; the receive bytes are slotted.** A copying send
stores its bytes in a chain of 64-byte chunks, because only the packet writer
reads them and it can gather a chain into the frame with one copy per chunk;
chunking wastes at most 63 bytes per message and cannot fragment. A buffered
receive needs one contiguous pointer to hand the application, so the reorder
pool is fixed slots of `max_message_bytes` and the block pool is fixed slots of
`max_block_bytes`. (Size classes for the reorder slots are a later refinement
that changes only the sender's accounting and the configuration digest, never
the wire.)

### 3.1 The sizing formula

From the per-connection configuration (section 11.2). Symbols:

| symbol | field | default | bound |
|---|---|---|---|
| C | `num_channels` | 32 | 1 .. `ROCKETNET_MAX_CHANNELS` |
| M | `max_message_bytes` | 1024 | 1 .. `ROCKETNET_MAX_MESSAGE_BYTES` (1174) |
| S | `send_queue_messages` | 1024 | 64 .. 65536 |
| Bs | `send_buffer_bytes` | 131072 | at least X + M |
| W | `number_window` | 1024 | power of two, 64 .. 65536 |
| R | `reorder_slots` | 64 | 8 .. 65535 |
| B | `block_slots` | 1 | 0 .. 64 |
| X | `max_block_bytes` | 65536 | M + 1 .. `ROCKETNET_MAX_BLOCK_BYTES` (4 MB) |
| U | `unreliable_buffer_bytes` | 8192 | 1192 .. 1 MB |
| P | `packet_map_entries` | 256 | 16 .. 65536 |
| Q | `receipt_records` | 256 | 0 .. 65536 |
| F | `max_packets_per_flush` | 8 | 1 .. 64 |

The record sizes below are this design's working figures; the layer 1 card pins
them as `sizeof` and the required-memory function uses the real values.

    per connection bytes =
          C x 48                          channel states (section 5.3)
        + S x 48                          send records
        + Bs + (Bs / 64) x 4              send chunks and their links
        + W x 4                           in-flight number table (sender)
        + W / 8                           received-number bitfield (receiver)
        + P x 32                          packet map entries
        + 2S x 8                          packet map reference ring
        + (R + Ru + B) x 16               reorder nodes, Ru = max(2, R / 4)
        + (R + Ru) x (M + 32)             reorder slots: R reliable, Ru unreliable sequenced
        + B x (X + ceil(X/1024)/8 + 64)   block slots and their fragment bitfields
        + 2U                              unreliable staging bytes and descriptors
        + Q x 24                          receipt records

At the defaults that is **392,664 bytes, about 383 KiB per connection**: 1,536 +
49,152 + 139,264 + 4,096 + 128 + 8,192 + 16,384 + 1,296 + 84,480 + 65,608 +
16,384 + 6,144. A server at 1000 clients carves about 375 MiB at create. A game
that sends no blocks sets B = 0 and saves 64 KiB per slot, and a lean profile
(S = 256, Bs = 32 KiB, W = 256, R = 32, M = 512, B = 0, the rest default) is
106,912 bytes, about 104 KiB per slot. Server-wide, add the delivered list of
section 9.3:

    server bytes += ( ROCKETNET_MAX_PAYLOADS_PER_UPDATE x 64
                      + max_clients x (R + Ru + B) ) x 24

26,520,000 bytes, about 25 MiB, at the shipped `ROCKETNET_MAX_CLIENTS` of 1000
and 1000 clients. Client-side the same term is
`( ROCKETNET_NUM_CLIENT_PACKETS x 64 + R + Ru + B ) x 24`, about 1.5 MiB.

At 1024 channels the channel term is 48 KiB per connection; at 10,000 it is
about 469 KiB per connection, 458 MiB at 1000 clients. That is the honest cost of the
stress target and the reason the per-channel state is kept to 48 bytes with
every queue intrusive (section 5.3).

**Checked by:** unit tests on a real carved block (acquire to exhaustion, the
refusal and its counter, release, reset, the formula against the sum of the
carved pools); `RNMessagePool` (section 12) for the single-owner and no-leak
properties.

## 4. Layer 2: message records in a packet

**Owns.** The bytes of a message-layer packet, inside the transport payload.
**Messages are opaque byte buffers**; nothing here bit-packs, and every field is
byte aligned and little-endian, as the transport's own layouts are
(`CLAUDE.md`, "protocol packets are byte aligned"; STANDARD.md _General
Conventions_, lines 216-232).

**Guarantees.** Every packet parses to exactly one of two forms or is refused
whole; a refusal names its reason on a counter; a record's header says
everything the receiver needs to dispatch it, and nothing about priority or
receipts, which never reach the wire (study section 2: receipt variants map
away on write, `RL.cpp:2619-2628`).

### 4.1 The packet: a form byte at the end

The **last byte** of every message-layer transport payload is the **form**:

| form | value | layout of the payload's first N - 1 bytes |
|---|---|---|
| (invalid) | 0x00 | refused, `MESSAGE_FORM_INVALID`; zero is what an uninitialised buffer holds |
| RECORDS | 0x01 | one to 64 message records end to end, exactly filling the N - 1 bytes |
| SINGLE | 0x02 | one message's data from offset 0, then that message's record header, then one byte H = the header's length |
| (reserved) | 0x03 .. 0xFF | refused, `MESSAGE_FORM_INVALID` |

**Why the form is a trailer.** The transport's payload pointer is 16-aligned and
its capacity a multiple of eight, so "any word-based serializer roots at the
payload pointer directly" (`sdk/rocketnet.h:411-415`, `sdk/rocketnet.h:816-818`).
A header in front would move the message off that alignment. With the form and
header at the end, **a SINGLE message starts at payload offset 0**: the game
writes it straight into the transport frame through `start_message`, aligned,
and the receiver hands back a pointer to payload offset 0 in the transport's
delivered batch. Zero copies each way. This is the zero-copy single-message
frame form.

### 4.2 The record header

Byte aligned, fields in this fixed order, each present only when its condition
holds. All integers little-endian.

    offset 0   flags            u8
    offset 1   length           u16   data bytes of this record, 1 .. 65535
               message number   u32   when code is 2, 3 or 4            (reliable)
               channel          u8    when code is 1, 3 or 4 and WIDE is clear
                                u16   when code is 1, 3 or 4 and WIDE is set
               ordering index   u32   when code is 1, 3 or 4            (ordered or sequenced)
               sequencing index u32   when code is 1 or 4               (sequenced)
               block id         u32   when BLOCK is set
               fragment index   u16   when BLOCK is set
               fragment count   u16   when BLOCK is set
               data             length bytes                            (RECORDS form only)

The flags byte, bit 0 the least significant:

| bits | name | meaning |
|---|---|---|
| 0-2 | code | the wire reliability: 0 UNRELIABLE, 1 UNRELIABLE_SEQUENCED, 2 RELIABLE, 3 RELIABLE_ORDERED, 4 RELIABLE_SEQUENCED; 5, 6, 7 are refused |
| 3 | BLOCK | a block fragment; the three block fields are present; legal only with codes 2, 3, 4 |
| 4 | WIDE | the channel is a u16; legal only when a channel is present, and only for a channel at or above 256 |
| 5 | CONTROL | the layer's own record, never delivered to the application; legal only with code 2 and BLOCK clear; the first data byte is the control kind |
| 6-7 | | zero; anything else is refused |

The wire codes are RakNet's numbering for its first five types, and the three
receipt types map onto their base code on write, as RakNet does
(`RL.cpp:2619-2628`, study section 2): 5 to 0, 6 to 2, 7 to 3. **All eight API
values exist from the first version; all five wire codes are parsed by the first
version** (I1). The receiver dispatches on the code; a code the build does not
yet enable is dropped and counted (`MESSAGE_CODE_NOT_ENABLED`), never read as one
of the enabled two.

**Header sizes by type** (narrow channel):

| type | wire code | header bytes | RakNet (study section 4.2) |
|---|---|---|---|
| UNRELIABLE, UNRELIABLE_WITH_ACK_RECEIPT | 0 | 3 | 3 |
| RELIABLE, RELIABLE_WITH_ACK_RECEIPT | 2 | 7 | 6 |
| RELIABLE_ORDERED, RELIABLE_ORDERED_WITH_ACK_RECEIPT | 3 | 12 | 10 |
| UNRELIABLE_SEQUENCED | 1 | 12 | 10 |
| RELIABLE_SEQUENCED | 4 | 16 | 13 |
| any block fragment | 2, 3, 4 | + 8 | + 10 |
| channel 256 or above | 1, 3, 4 | + 1 | (not reachable) |

The largest header is 25 bytes (RELIABLE_SEQUENCED, wide, block). The header is
variable by type, never one fixed size (I2), so UNRELIABLE pays 3 bytes and not
16.

**Why every counter is 32 bits.** A counter compared by serial arithmetic is
safe when it advances fewer than 2^31 between a record's send and the last
moment a copy of it can still be accepted. The transport bounds that moment: a
packet older than the replay history is refused, and the history is at most
32,768 packets (`ROCKETNET_HISTORY_PACKETS`, `sdk/rocketnet_history.h:7-13`,
`USAGE.md:283-284`, STANDARD.md _Replay window sizing_, line 1550). This layer
bounds a packet to 64 records (section 4.4). So any counter advances at most
64 x 32,768 = 2^21 within the acceptance window, far below 2^31. A 16-bit counter fails that bound at about one record per packet on
the same channel (32,768 packets x 1 = 2^15), and RakNet's 24-bit one fails it
at 256 records per packet. The cost is two or three bytes on the reliable and
sequenced types against RakNet; bandwidth is traded for an argument that holds
(CLAUDE.md, "at 1000 players, trading a little bandwidth for simpler code").

**Lengths are bytes**, not bits as RakNet's are (`RL.cpp:2632-2633`).

### 4.3 The SINGLE form

    payload offset 0       message data, L bytes
    offset L               the record header, H bytes, with length = L
    offset L + H           H, u8
    offset L + H + 1       0x02, the form

Total N = L + H + 2. For UNRELIABLE, H = 3 and the trailer is 5 bytes, so the
largest zero-copy message in one wire frame is 1192 - 5 = **1187 bytes**. A
SINGLE payload larger than one frame rides the transport's fragments (the arena
route, one copy, a lost fragment loses the whole message: exactly UNRELIABLE's
semantics, `sdk/rocketnet.h:822-834`); the largest is the configured transport
maximum payload less the trailer, and at most 65,535 bytes.

Validity (refuse the whole payload, `MESSAGE_SINGLE_INVALID`): N < 6; H not
between 3 and 17; H + 2 > N; the trailer header's flags fail section 4.2's
rules; BLOCK or CONTROL set; the header's length differs from N - H - 2. Any of
codes 0 to 4 is legal in SINGLE; the first version's sender uses it only for the
unreliable codes. The length field must equal L, so a SINGLE message is at most
65,535 bytes even where the transport's configured maximum payload is larger.

### 4.4 The RECORDS form

    payload offset 0       record 1: header, then length bytes of data
                           record 2 ...
                           ... at most 64 records
    offset N - 1           0x01, the form

The receiver validates the **whole geometry before dispatching any record**:
every header's flags legal, every field inside the payload, every length
nonzero, the records ending exactly at N - 1, and no more than
`ROCKETNET_MAX_RECORDS_PER_PACKET` = 64 of them. Any failure refuses the whole
payload (`MESSAGE_RECORDS_INVALID`) and nothing in it is dispatched. The payload
was sealed by the peer, so a geometric fault is a peer defect, never line
noise; refusing whole keeps "rejected" meaning "rejected here" (CLAUDE.md,
verification ladder, rung 1).

A record that is geometrically sound but semantically refused (a channel at or
above the connection's count, a code not enabled, a block count over the
budget) is dropped alone, counted on its own reason, and the rest of the packet
is dispatched.

The 64-record cap bounds three things at once: the counter argument above, the
packet map's reference ring (section 5.2), and the server's delivered list
(section 9.3). The smallest record is 4 bytes (UNRELIABLE, one data byte), so
without it a packet could carry 297.

`ROCKETNET_MAX_MESSAGE_BYTES` = 1192 - 1 - 17 = **1174**: the largest message
that fits one RECORDS packet as a single record with the largest non-block
header (RELIABLE_SEQUENCED on a wide channel). A reliable message above
`max_message_bytes` is a block (section 7).

### 4.5 Control records

A control record is code 2 (RELIABLE), CONTROL set, BLOCK clear. It takes a
message number and is resent, deduplicated and acknowledged exactly like a user
RELIABLE record, so the control path **is** the reliable path; it is never on a
user channel (I16; RakNet put its handshake on user channel 0,
`RP.cpp:3553, 6201`). The first data byte is the kind; an unknown kind is
dropped and counted (`MESSAGE_CONTROL_UNKNOWN`). One kind exists:

    CONFIG (kind 1), 16 bytes of data:
        kind               u8    1
        version            u8    1, this layer's wire version
        num_channels       u16   C
        max_message_bytes  u16   M
        reorder_slots      u16   R
        number_window      u16   W / 64, so 1 .. 1024
        block_slots        u16   B
        max_block_bytes    u32   X

### 4.6 Worked bytes

A RELIABLE_ORDERED record, message number 1, channel 0, ordering index 0, data
`68 69` ("hi"), alone in a RECORDS packet:

    03 02 00 01 00 00 00 00 00 00 00 00 68 69 01
    |  |     |           |  |           |     form RECORDS
    |  |     |           |  |           data
    |  |     |           |  ordering index 0
    |  |     |           channel 0
    |  |     message number 1
    |  length 2
    flags: code 3

An UNRELIABLE message `61 62 63` ("abc") in the SINGLE form:

    61 62 63 00 03 00 03 02
    |        |  |     |  form SINGLE
    |        |  |     H = 3
    |        |  length 3
    |        flags: code 0
    data, at payload offset 0

These two, and one of each other code, a block fragment and a CONFIG record, go
into STANDARD.md as known-answer vectors with the layer 2 card.

### 4.7 For STANDARD.md

Sections 4.1 to 4.6 are written to drop into STANDARD.md as a new section,
"Messages". STANDARD.md today says reliability and messages "belong to a
high-level tier above the transport ... and nothing in them is specified here"
(STANDARD.md lines 7-10, 66-70). Moving that line is Glenn's ruling, so this
card does not edit STANDARD.md (section 15).

**Checked by:** the codec is not a state machine, so no TLA+ module; it gets
known-answer vectors compared with memcmp, a write-then-read round trip of every
header combination, one refusal test per validity rule asserting the specific
counter and that nothing was dispatched, and a fuzz target on the reader.

## 5. Layer 3: per-send reliability and ordering channels

**Owns.** Per connection, both directions: the reliable message numbers, the
packet map, the resend timer, the receive dedupe window, the per-channel
counters, the reorder lists, the sender's account of the receiver's capacity,
and the configuration exchange. **Reliability is chosen per send**, as RakNet
does; the ordering channel is a number on the send; the channel count is the
compile-time maximum `ROCKETNET_MAX_CHANNELS` (32 by default) and a
per-connection runtime count at or under it.

**Guarantees.**

1. A reliable record is delivered to the receiving application **exactly once**,
   for as long as the connection lives.
2. Records on one channel that are ordered are delivered in send order with no
   gap.
3. **A reliable record whose packet the transport acknowledged is held or
   delivered by the receiver, never dropped** (AckedImpliesHeld).
4. No unreliable send is ever delayed by reliable state (I13).
5. Nothing on the wire or in a channel's state depends on which of the eight
   types are enabled.

### 5.1 Message numbers (I3, I4)

One u32 counter per connection per direction, starting at 1. **A number is
assigned when a reliable record is first written into a packet, never at
send** (RakNet's late assignment, `IP.h:83-85`, `RL.cpp:1024-1027`): assigning at
send and then writing a higher-priority record first opened holes in the
receiver's window. Unreliable records never take a number; nothing but a
reliable record moves the receive window (`RL.cpp:2050` records the permanent
hole RakNet got from breaking that). The message number is not the transport
sequence: a resend is a new packet with a new sequence and a new packet id
carrying the same number (I4). The transport's replay window removes duplicated
packets; this window removes resent messages. Both stay.

**Sender:** an in-flight table of W entries indexed by `number & (W - 1)`, each
the index of the send record holding that number, or empty; and
`oldest_unacked`, the lowest number still in the table. A new number may be
assigned only while `next_number - oldest_unacked < W_eff` (section 5.6). When
the span is full, new reliable records wait in their queues; resends and
unreliable records continue (RakNet's `ResendBufferOverflow` stall,
`RL.cpp:3758-3765`, with the same shape).

**Receiver:** `base`, the lowest number not yet received, and a W-bit field over
`[base, base + W)`. A record with number n:

- `n - base` (unsigned 32-bit) at or above 2^31: older than the window, a
  duplicate; drop, `MESSAGE_DUPLICATE`.
- below W: if its bit is set, a duplicate, drop; else set the bit and dispatch;
  then advance `base` over every leading set bit, clearing as it goes.
- otherwise (at or beyond the window, below 2^31): the sender broke its span
  rule; drop, `MESSAGE_NUMBER_OUT_OF_WINDOW`. This cannot happen between
  conforming ends: the sender's oldest unacknowledged number is at or below the
  receiver's base, because every lower number was acknowledged and so received,
  and the sender never assigns past its oldest plus W.

### 5.2 The packet map, the acks and the resend timer (I8, I14)

Every packet the endpoint writes gets a **packet map entry**: `{packet_id,
send_time, first_ref, num_refs}`, where the refs are `{record index, message
number}` pairs in a ring of 2S entries, one per reliable record in the packet,
plus one per unreliable-receipt record (section 6). The map is a ring of P
entries in write order with an open-addressed index on `packet_id` (ids are
unique but not contiguous, section 1). A packet mixes reliabilities, so the map
is packet to **list** of records from the first version, even when only one
reliable record is in a packet (I8).

**On an acknowledged packet id** (the transport's batch, per update): look the
entry up; for each ref, if the in-flight table still holds that record **with
that number**, the record is acknowledged: remove it from the table, advance
`oldest_unacked`, release its bytes. A ref whose number no longer matches (the
record was acknowledged through another packet first, and its slot reused) is
ignored, as RakNet's stored-number check does (`RL.cpp:2471-2473`). An id not in
the map (aged out, or another connection's) is ignored.

**RTT and RTO.** The entry's `send_time` is the time passed to the last update
before the fill that wrote it. An acknowledgment processed at update time `t`
yields one RTT sample `t - send_time`. Because every resend is a new packet with
its own id, **every sample is unambiguous**: Karn's problem does not arise.
Smoothing is RFC 6298's (srtt gain 1/8, rttvar gain 1/4). RTO is
`clamp(srtt + 4 x rttvar, resend_min, resend_max)`, 0.25 s before the first
sample, with no exponential back-off (RakNet has none either,
`CC.cpp:288-314`); a dead connection is the transport's connection timeout, not
this layer's (I14). The sample includes the peer's ack aggregation delay, which
is the delay a resend timer must cover. The transport's own rtt figure is a
floor and cannot track latency upward, so it is not used for resend
(`USAGE.md:216-236`).

**Resend.** A reliable record not acknowledged by `last_send_time + RTO` is due.
The scheduler writes due records first (section 8). Nothing else triggers a
resend: there is no NAK (section 1).

An entry evicted from the ring before its acknowledgment arrived simply cannot
acknowledge anything; its records are resent by the timer, as RakNet's 513-entry
history does (`RL.cpp:3800-3805`). `MESSAGE_PACKET_MAP_EVICTED` counts it.

### 5.3 Ordering channels (I5, I6, I15)

Each connection has C channel states, zeroed when the connection starts, never
opened or closed. One channel state, 48 bytes, the same in every version:

| field | side | what |
|---|---|---|
| `ordered_write` u32 | sender | next ordering index; an ordered send takes it and increments it |
| `sequenced_write` u32 | sender | next sequencing index within the current ordering index; reset to 0 by every ordered send |
| `ordered_read` u32 | receiver | the next ordering index to deliver |
| `sequenced_read` u32 | receiver | the lowest sequencing index still deliverable at `ordered_read`; reset to 0 by every ordered delivery |
| `fifo_head`, `fifo_tail` u32 | sender | this channel's reliable indexed records from send until passed, in send order (section 5.4) |
| `reorder_head` u32 | receiver | this channel's buffered records, sorted by key |
| `held` u32 | sender | how many of this channel's FIFO records have been written at least once |
| reserved | | to 48 bytes |

**The four counters exist and move from the first version** (I5): the first
version's ordered send resets `sequenced_write` to 0, and its ordered delivery
resets `sequenced_read` to 0, although nothing reads them until a sequenced type
is enabled. So a later sequenced send is "newer than everything before the last
ordered message" from day one, and enabling it changes nothing on the ordered
path.

**Ordering indices are assigned at send**, in the API call, as RakNet does
(`RL.cpp:1625-1657`), so a channel delivers in **send order** whatever the
priorities of its messages. (Message numbers are assigned at write; the two are
independent families, I3.)

**Receive dispatch on a channel**, the one rule for both paths. A record with
ordering index `o` and, if sequenced, sequencing index `s`:

- `o == ordered_read`:
  - ordered: deliver; `ordered_read++`; `sequenced_read = 0`; then **drain**.
  - sequenced: deliver if `s >= sequenced_read` and set `sequenced_read = s + 1`;
    else drop (stale, `MESSAGE_SEQUENCED_STALE`).
- `o` newer than `ordered_read` (serial arithmetic): insert into the channel's
  reorder list.
- `o` older: drop. For a reliable record this cannot happen after the dedupe
  window has passed it, and is counted (`MESSAGE_ORDERED_STALE`) because it
  would be a defect.

**Drain**: while the reorder list's head has `o == ordered_read`, pop it and
apply **the same rule as above**, through the same function. RakNet's two paths
differ, and both of its sequenced defects live in the difference (study section
11 items 18 and 19: the heap path sets the watermark without the `+ 1`, and does
not reset it on an ordered release). Here there is one function, so there is
one rule.

**The reorder key is (ordering index, sequencing index), with the ordered record
last in its epoch** (I6): within one ordering index, sequenced records sort by
sequencing index and the ordered record that opens the next epoch sorts after
them all, as RakNet's heap weight does (`RL.cpp:1439-1446`). The list is sorted
on insert; insertion walks at most the held count, which section 5.4 bounds. The
first version inserts only ordered records, so the key's second half is always
the "ordered" sentinel, but the comparator is the full one.

**Channel bounds** (I15): the API refuses a channel at or above the
connection's `num_channels` (`MESSAGE_CHANNEL_OUT_OF_RANGE` and serial 0); the
receiver drops a record whose channel is at or above its own count and counts
the same reason. The runtime count is compared, never the compile-time maximum.
The wire width does not depend on the build: channels below 256 are one byte,
256 and above set WIDE and take two, so a build at 32 and a build at 10,000
interoperate, and the stress runs change one number.

### 5.4 The sender keeps the receiver's capacity

The transport acknowledges a packet when it opens it (section 1). If the
receiver then had nowhere to buffer an out-of-order reliable record, the record
would be lost while its sender believed it delivered. So **the sender never
writes a record the receiver might have to hold unless it knows the receiver has
room**, and the receiver's pools are sized so that it always does. Three
windows:

- **Numbers:** span below W (section 5.1).
- **Reorder:** every reliable record that takes an ordering index (ordered or
  reliable-sequenced, block-carried or not) joins its channel's FIFO **at send**,
  when the index is assigned, so the FIFO is the channel's indices in order. It
  leaves the FIFO only when it is at the head **and** has been acknowledged; a
  record acknowledged while an older one on its channel is not stays in the FIFO
  with its bytes already released. The channel's `held` counts the FIFO records
  that have been written at least once, and the connection's `held_total` is
  their sum over channels. A record not yet written may be written for the first
  time only while `held_total < R_eff`, **except the FIFO's head, which may
  always be written**. Block-carried records count against `B_eff` instead
  (section 7).
- **Blocks:** at most `B_eff` blocks are open, and blocks open in send order
  (section 7).

**Why the head is exempt.** Without it the window deadlocks: a LOW message at
index 0 still queued while HIGH messages 1 to R on the same channel were written
fills `held_total`, and index 0 could never be written, so never acknowledged,
so never popped. The head is safe to write: every index below it was received,
so `ordered_read` equals its index and the receiver delivers it on arrival
without a slot.

**Why this is enough.** The receiver buffers a record with ordering index `o` on
channel `c` only while `o > ordered_read[c]`. Every index on `c` below the
FIFO's head was acknowledged, so received; a received run with no gap from the
start of the channel is delivered in the update that completes it; so
`ordered_read[c]` is at or above the head's index. A record the receiver holds
was received, so written, and its index is at or above the head, so it is a
written record in the FIFO. There are at most `held_total < R_eff` of them.

**Unreliable sequenced records are never in that count.** They may also be
buffered ahead of their epoch (section 5.3), so the receiver gives them their
own pool of `R / 4` slots (at least 2), separate from the R reliable slots, and
drops one that finds it full (`MESSAGE_SEQUENCED_NO_SLOT`). An unreliable flood
can therefore never take a slot a reliable record was promised.

**The one-update lag.** A record delivered from the reorder pool is handed to
the application as a pointer into its slot, valid until the next update, so the
slot is released at the **start** of the next update, before any packet is
processed. A record whose FIFO pop lets the sender write a new one was
acknowledged; that acknowledgment left the receiver in a packet built after the
update that received it; so the new record arrives in a later update, after the
release. R slots are enough; no doubling is needed. `RNOrderedChannel` checks
exactly this lag (section 12).

### 5.5 The send records

A send record is 48 bytes: the reliability, the channel, the ordering and
sequencing indices, the message number once assigned, `last_send_time`, the
receipt serial, the length, the head of its byte chunk chain, and the intrusive
links for its priority queue, its channel FIFO and the in-flight list. The
record lives from the send until it is acknowledged and has left its FIFO (S of
them). Its bytes live in send chunks until it is acknowledged (Bs of them).

### 5.6 The configuration exchange

The windows above are the receiver's capacities, so the sender needs them.
Each endpoint sends one CONFIG control record (section 4.5) as the first record
of its first packet. It is reliable: resent on the timer until acknowledged
(the transition rule). On receiving the peer's CONFIG:

- `version`, `num_channels` and `max_message_bytes` must equal this end's own.
  A mismatch is a deployment error: `MESSAGE_CONFIG_MISMATCH`, an error log
  naming the fields, and the endpoint enters the **error** state: it refuses
  further sends and the object reports the state (section 11). Why the game
  disconnects is the game's decision.
- The sender's effective windows become `W_eff = min(W, peer W)`,
  `R_eff = min(R, peer R)`, `B_eff = min(B, peer B)`,
  `X_eff = min(X, peer X)`.

**Before the peer's CONFIG arrives** the sender uses the protocol floors:
`W_eff` = 64, `R_eff` = 8, `B_eff` = 0. Every valid configuration is at or above
the floors (section 3.1), so reliable messages flow from the first packet with
no round trip of delay; only blocks wait for the peer's CONFIG. The floors are
protocol constants and go into STANDARD.md with the layout.

**Checked by:** unit tests driving two endpoints through a byte array under
scripted loss, duplication, reordering and acknowledgment delay, each asserting
delivery once and in order, the counters, and that no pool refused; the TLA+
modules `RNReliableWindow`, `RNOrderedChannel` and `RNMessageConfig`
(section 12).

## 6. Layer 4: the eight types

**Owns.** Two tables: the API reliability to its wire code, its storage, its
receipt behaviour and whether it is enabled; and the wire code to its receive
dispatch. Every type is a row. **Enabling a type is flipping its row's enable
bit and adding its tests**; nothing else moves.

| API value | name | wire code | number | channel FIFO | receipt | larger than M | first version |
|---|---|---|---|---|---|---|---|
| 0 | UNRELIABLE | 0 | no | no | none | SINGLE over the transport's fragments | **enabled** |
| 1 | UNRELIABLE_SEQUENCED | 1 | no | no | none | SINGLE over the transport's fragments | parsed, refused |
| 2 | RELIABLE | 2 | yes | no | none | block, unordered | parsed, refused |
| 3 | RELIABLE_ORDERED | 3 | yes | yes | none | block, ordered | **enabled** |
| 4 | RELIABLE_SEQUENCED | 4 | yes | yes | none | block, sequenced | parsed, refused |
| 5 | UNRELIABLE_WITH_ACK_RECEIPT | 0 | no | no | ACKED or LOSS | SINGLE over the transport's fragments | refused at send |
| 6 | RELIABLE_WITH_ACK_RECEIPT | 2 | yes | no | ACKED | block, unordered | refused at send |
| 7 | RELIABLE_ORDERED_WITH_ACK_RECEIPT | 3 | yes | yes | ACKED | block, ordered | refused at send |

The API values are RakNet's, in RakNet's order (`PP.h:46-85`), so the C# shim
passes them through. A disabled type is refused at send with serial 0 and
`MESSAGE_RELIABILITY_NOT_ENABLED`; a disabled wire code is dropped at receive
with `MESSAGE_CODE_NOT_ENABLED`.

**Unreliable messages above a packet do not become reliable.** RakNet upgrades
an unreliable split to reliable because it has no other way to carry it
(`RL.cpp:1609-1621`). This layer has one: the transport's fragmentation already
carries a payload whole or not at all (`sdk/rocketnet.h:422-430`), which is
exactly unreliable semantics, so a large unreliable message is a SINGLE payload
over the transport's arena route, one copy, and keeps its type (I11's
unreliable half, decided).

**Receipts** (I9, I10, I12). Every send consumes a **serial**, a u32 issued by
the API before the message is queued, starting at 1 and skipping 0 on wrap, one
counter per client or server object, atomic because sends run on game threads.
The call returns it whatever the type (RakNet issues one per send too,
`RP.cpp:1326-1330`). Every send record carries it from the first version, so a
receipt type is the base type plus a check at acknowledgment time, never a
second bookkeeping path:

- a reliable receipt type reports **ACKED when every record of the message has
  been acknowledged**: for a block, all its fragments, not the last-index one
  RakNet chose (`RL.cpp:2486-2496`, study section 7.3). ACKED means the peer's
  transport opened it; it does not mean the application read it, as RakNet's
  does not (study section 7.3). There is no LOSS for a reliable type; a dead
  connection is the transport's timeout.
- UNRELIABLE_WITH_ACK_RECEIPT keeps a **receipt record** `{packet_id, serial,
  deadline}` from the Q pool (it outlives the frame, I10), referenced from the
  packet map: ACKED when that packet id is acknowledged; LOSS when the per-update
  sweep finds `time > deadline`, with `deadline = send_time + RTO`. It is the one
  receipt with a timer. An unreliable message dropped unsent at the end of a
  flush (section 8) reports LOSS at once.
- Sequenced types have no receipt types (RakNet commented them out for this
  reason, `PP.h:70, 80`): a sequenced record is acknowledged on arrival and may
  still be discarded as stale (I12). The ack clears the sender's state
  regardless.

Receipts reach the application as a per-update batch (section 11), each
`{serial, outcome}` with the client index on the server.

**Checked by:** a table test asserting every row against the API and the
dispatch; the type reachability argument in section 13; `RNReceipts`
(section 12).

## 7. Layer 5: blocks

**Owns.** Reliable messages larger than `max_message_bytes`, up to
`max_block_bytes`: the split on the sender, the reassembly slots on the
receiver, and the block ids.

**Guarantees.** A block is delivered once, whole, with the type's ordering, or
not at all while the connection lives; reassembly is by index, never by arrival;
a stale or reused block id never merges into another block; a block's receipt
fires when all its fragments are acknowledged.

**The decision (I11).** Two shapes were open: ride the transport's fragments and
resend the whole payload when its id does not come back, or split in this layer
and resend each fragment. Whole-payload resend at 1% loss loses a 14-fragment
payload 13% of the time and a 64-fragment one 47% of the time, and every loss
resends all of it, through the arena copy; under sustained loss a large block may
never complete. **This layer splits, and each fragment is an ordinary reliable
record**, numbered, mapped, acknowledged and resent on its own, as RakNet's are
(`RL.cpp:2928-3033`) and as yojimbo's block messages are. Only the lost fragment
is resent.

**Split.** The stride is a protocol constant, `ROCKETNET_BLOCK_FRAGMENT_BYTES` =
**1024**: every fragment but the last carries exactly 1024 bytes and the last 1
to 1024. A fixed stride means the receiver places every fragment at
`index x 1024` on arrival with no staging buffer and no "first non-final fixes
the stride" rule, and 1024 leaves room in a 1192-byte packet for the 25-byte
header and other records. The count is `ceil(bytes / 1024)`, at least 2 (a
message that fits one record is never a block), at most 4096 (4 MB). All
fragments carry the block id (a u32 per connection per direction, from 1), their
index, the count, and the **same** ordering and sequencing indices, assigned once
at send, so a block takes one ordering slot (`RL.cpp:1625-1657` then `2963`).

**Receive.** After the dedupe window accepts the fragment's number (so a resent
fragment of a completed block is a duplicate and never reaches here): find the
slot holding this block id, or take a free one. Validity, each with its counter:
count below 2 or above `ceil(X / 1024)`; index at or above the count; a count
differing from the one the slot pinned; a non-final fragment that is not exactly
1024 bytes; a final over 1024. The fragment is copied to `index x 1024` and its
bit set. On the last bit, the block is a complete message, and goes through the
type's dispatch: delivered now (unordered), or delivered or linked into its
channel's reorder list (ordered, sequenced), its bytes staying in the block slot.
The slot is released at the start of the update after its delivery.

**Capacity.** A block is **open** at the sender from its first fragment's first
write until every fragment is acknowledged and, if ordered, it has left its
channel FIFO. At most `B_eff` blocks are open, and a block may open only if it
is the oldest unopened block of the connection by send order; a further block
waits in its queue. The order rule prevents the deadlock of section 5.4 in its
block form: an open block never waits on a block that has not opened, so it
never holds the slot that block needs. The receiver holds a block slot only
while that block is open at the sender, by the argument of section 5.4, so B
slots suffice and a fragment always finds its slot. Before the peer's CONFIG,
`B_eff` = 0. Block ids are never reused while open, and the receiver keys by the full u32, so RakNet's stale
16-bit id merge (`RL.cpp:3043-3061`, study section 8.2) cannot happen.

**Send bytes.** The block's bytes are copied once at send into the send chunks,
so `send_buffer_bytes` must be at least X + M (validated at create).

**Checked by:** unit tests with fragments delivered in every order, duplicated,
and with the last fragment first; refusal tests per rule; `RNBlocks`
(section 12), whose reversed witness is arrival-order reassembly.

## 8. Layer 6: scheduling with four priorities

**Owns.** What goes into each packet a connection writes in a flush.

**Guarantees.** Due resends go before new data; among new data, the four
priorities interleave in RakNet's measured ratios and no non-empty priority
starves; an unreliable record is never held back by a reliable window; a
channel delivers in send order whatever the priorities of its messages.

**Priorities** (`PP.h:21-40`, study section 5.3): IMMEDIATE, HIGH, MEDIUM, LOW,
values 0 to 3. They affect **only the order in which queued records are pulled
into packets**; never a header, never a timer. RakNet's IMMEDIATE wakes its
network thread (`RP.cpp:4218-4222`) and does nothing else; here there is no
network thread, so IMMEDIATE is simply the heaviest weight. RakNet's comment
promises 10 ms batching and a 2:1 ratio; its code does neither (study section
11 item 3), and the code is what is matched.

**The queues.** Per connection, per priority, two FIFOs: one of reliable send
records and one of unreliable records (their bytes in the per-flush unreliable
staging buffer). Eight FIFOs per connection, intrusive through the records, so
the cost does not grow with the channel count.

**One flush, per connection.** Up to F packets, each requested at 1192 bytes
(the zero-copy wire route; this layer never asks the transport to fragment a
RECORDS packet). Into each packet, in order:

1. the CONFIG record, if not yet acknowledged and due;
2. due resends, oldest `last_send_time` first;
3. new records: pick the priority with the lowest **virtual time** among the
   priorities with a head that can be written; within it, write the older of
   its two heads that can be written; advance that priority's virtual time by
   its stride: **1, 5, 14, 35** for IMMEDIATE to LOW, RakNet's strides
   (`RL.cpp:3926-3949`), which interleave adjacent levels about 5:1, 2.8:1 and
   2.5:1 under a backlog. When every queue is empty the virtual times reset, as
   RakNet's weights do.

A head "can be written" if it fits the packet's remaining room and, for a
reliable record, the windows of section 5.4 allow it. A reliable head blocked by
a window waits, and the reliable records behind it in the same FIFO wait with
it (head-of-line within one priority's reliable FIFO, accepted so that a
priority's reliable records go out in send order); every other head is still
considered, and **the unreliable FIFOs never wait on a reliable window** (I13).

When a channel's messages span priorities, a higher-priority message may be
written before an older one on the same channel. The ordering index was
assigned at send, so the receiver buffers the newer one and delivers both in
send order, as RakNet's does; the record counts against `R_eff` while it waits
(section 5.4).

A packet closes when the next pick does not fit; the flush ends at F packets or
empty queues. **Unreliable records still staged when the flush ends are
dropped**, counted (`MESSAGE_UNRELIABLE_DROPPED`), and their receipts report
LOSS; the staging buffer resets for the next fill. Unreliable data is the
newest state; carrying it across flushes would only send it late. Reliable
records stay queued.

**Small unreliable messages aggregate**: any number of them, up to 64 records,
share one RECORDS packet with reliable records and resends. Two unreliable
sends bypass the queues and are their own SINGLE packet, so priority does not
apply to them: a message the game writes through `start_message`, and a copying
`send_message` of an unreliable message larger than `max_message_bytes`, which
the call writes at once through `start_packet`, copies and finishes. Both leave
in the transport flush that follows.

**Checked by:** unit tests on fixed backlogs asserting the pulled order and the
ratios; `RNScheduler` (section 12), whose reversed witness is strict priority
(LOW starves).

## 9. Layer 7: the endpoint, and the client and server objects

### 9.1 The endpoint machine

`rocketnet_message_endpoint_t`, one per connection, a pure module per section 2.
Its state is one struct whose pools point into the connection's carved block.
Its events and actions:

| event | from | action |
|---|---|---|
| `receive_payload( bytes, n, time )` | the transport's delivered batch | append delivered messages (pointer, length, channel, type) to the caller's list; process CONFIG |
| `acked( packet_id, time )` | the transport's acked batch | acknowledge records, take an RTT sample, append ACKED receipts |
| `begin_update( time )` | the object, first thing each update | release last update's delivered slots; sweep receipt deadlines (LOSS) |
| `send( bytes, n, reliability, channel, priority, serial )` | the API | queue a send record, or refuse with a reason |
| `write_packet( buffer, capacity, packet_id, time )` | the object, in the fill phase | write one packet's records into the given frame, bind them to `packet_id`; return the length, or 0 when nothing is left |
| `end_flush()` | the object | drop staged unreliable records left over |
| `next_deadline()` | the object | the earliest resend or receipt deadline, so a caller can sleep |

`write_packet` is given the packet id the transport already issued, so the
machine never sees a transport. A unit test makes up packet ids.

### 9.2 The client object

`rocketnet_client_t` owns one `client_transport` and one endpoint. Its update:
transport update; if the transport state just became CONNECTED, reset the
endpoint (fresh state, CONFIG queued); if it just left CONNECTED, report it;
`begin_update`; feed the acked batch; feed every delivered payload. Its flush
(`write_packets`): `write_packet` into `start_packet` frames until it returns 0
or F packets, `finish_packet` each; `end_flush`. Its `send_packets`: the flush if
not already done this fill, then the transport's `send_packets`.

### 9.3 The server object

`rocketnet_server_t` owns one `server_transport` and `max_clients` endpoints, one
per slot. Its update: transport update; for every slot whose connection changed
(section 11.4), reset or close that endpoint and append a slot event
`{client_index, CONNECTED | DISCONNECTED}`; `begin_update` on every connected
endpoint; route the acked batch by client index; route every delivered payload
by client index.

The server's **delivered list** is one array for the whole server, sized at
create to the most one update can deliver: `ROCKETNET_MAX_PAYLOADS_PER_UPDATE`
payloads (one client may use all of them, `sdk/rocketnet_constants.h:48-53`)
times 64 records, plus every slot's reorder and block slots (section 3.1). It
cannot overflow, so a reliable message the transport acknowledged is never dropped for
want of a descriptor. A descriptor is 24 bytes across the parallel arrays the
API returns: the data pointer, the length (u32, blocks reach 4 MB), the client
index, the channel and the type.

Flushing a slot (`write_packets_for_client`) touches only that slot's endpoint
and the transport's fill-phase calls, so a game spreads its clients across fill
threads and flushes them in parallel. `send_packets` flushes any slot not yet
flushed this fill, then calls the transport's. The server's per-fill lease
budget (65,536, section 1) must cover `max_clients x F`; create refuses a
configuration that does not.

**Checked by:** unit tests of the endpoint pair (section 5); functional tests of
the objects on the bench over real transports with the network simulator
(`sdk/rocketnet_network_simulator.h`), including a slot that disconnects and
reconnects; `RNMessageConnection` (section 12). Then the space game integrates
the objects, which is the proof Glenn named.

## 10. Thread rules, inherited from the transport

The transports' contract (section 1) stands unchanged, and the objects add only
this:

- `update`, `send_packets`, `disconnect`, `destroy` and every receive accessor
  are **main-thread**. The delivered messages, receipts and slot events are
  valid from an update until the next update, as the transport's batches are
  (`USAGE.md:68-94`): **copy anything you keep, and drain every update.**
- `send_message`, `start_message`, `finish_message`, `abort_message` and
  `write_packets` run on any thread **strictly between an update and the
  following `send_packets`**, joined before it, as `start_packet` does.
- **One writer per connection at a time.** All of a connection's sends and its
  flush run on one thread at a time (any thread; a game typically assigns each
  client to one fill thread). Different connections are fully independent. The
  serial counter is the one shared word, and it is atomic.
- After `write_packets` for a connection, further sends to it in the same fill
  are queued for the next flush.
- No call takes a lock on a send or receive path except the transport's own.

## 11. The C API, as it will appear in `sdk/rocketnet.h`

A new region after the transports' sections, written in the header's own style:
a prose contract above each surface, plain `extern "C"` symbols, no C++ type on
the surface (REQUIREMENTS.md: "No C++ types cross that surface").

### 11.1 Constants

```c
// -----------------------------------------
//                                messages
// -----------------------------------------

// The message layer: messages across n ordering channels, reliability
// chosen per send, on top of the transports. The channel count is a
// per-connection runtime value at or under this compile-time maximum;
// builds at 1024 or 10000 interoperate with a build at 32 (the wire
// carries channels below 256 in one byte and the rest in two).

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                            32
#endif

#if ROCKETNET_MAX_CHANNELS < 1 || ROCKETNET_MAX_CHANNELS > 65535
#error ROCKETNET_MAX_CHANNELS must be between 1 and 65535, the wire's u16 channel
#endif

#define ROCKETNET_MAX_MESSAGE_BYTES                     1174        // the largest message in one packet, any type
#define ROCKETNET_MAX_SINGLE_MESSAGE_BYTES              1187        // the largest zero-copy unreliable message in one wire frame
#define ROCKETNET_MAX_BLOCK_BYTES                 ( 4 * 1024 * 1024 )
#define ROCKETNET_BLOCK_FRAGMENT_BYTES                  1024
#define ROCKETNET_MAX_RECORDS_PER_PACKET                  64

// reliability, chosen per send: RakNet's eight, with RakNet's values

#define ROCKETNET_UNRELIABLE                               0
#define ROCKETNET_UNRELIABLE_SEQUENCED                     1
#define ROCKETNET_RELIABLE                                 2
#define ROCKETNET_RELIABLE_ORDERED                         3
#define ROCKETNET_RELIABLE_SEQUENCED                       4
#define ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT              5
#define ROCKETNET_RELIABLE_WITH_ACK_RECEIPT                6
#define ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT        7

// priority: the order queued messages are pulled into packets, nothing else

#define ROCKETNET_PRIORITY_IMMEDIATE                       0
#define ROCKETNET_PRIORITY_HIGH                            1
#define ROCKETNET_PRIORITY_MEDIUM                          2
#define ROCKETNET_PRIORITY_LOW                             3

#define ROCKETNET_RECEIPT_ACKED                            1
#define ROCKETNET_RECEIPT_LOSS                             2

#define ROCKETNET_SLOT_CONNECTED                           1
#define ROCKETNET_SLOT_DISCONNECTED                        2

#define ROCKETNET_MESSAGE_STATE_OK                         0
#define ROCKETNET_MESSAGE_STATE_CONFIG_MISMATCH           -1
```

### 11.2 Configuration

One X-macro table, the transports' config system (`sdk/rocketnet.h:447-466`):
struct, defaults, `ROCKETNET_` environment names, parse and validation from one
declaration; precedence default < struct < env; refuse, never clamp.

```c
// The message config table: one per connection, the same on the client and
// on every server slot. num_channels, max_message_bytes and the layer
// version must match the peer's -- the endpoints exchange them reliably on
// connect and a mismatch is a loud error state; the capacities may differ
// and each sender uses the smaller of its own and the peer's. All storage
// is spent at create; rocketnet_message_required_memory states it first.

#define ROCKETNET_MESSAGE_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                     \
    INT_FIELD( num_channels,             32, 1,    ROCKETNET_MAX_CHANNELS,       0 )                             \
    INT_FIELD( max_message_bytes,      1024, 1,    ROCKETNET_MAX_MESSAGE_BYTES,  0 )                             \
    INT_FIELD( send_queue_messages,    1024, 64,   65536,                        0 )                             \
    INT_FIELD( send_buffer_bytes,    131072, 4096, 64 * 1024 * 1024,             0 )                             \
    INT_FIELD( number_window,          1024, 64,   65536,                        0 )                             \
    INT_FIELD( reorder_slots,            64, 8,    65535,                        0 )                             \
    INT_FIELD( block_slots,               1, 0,    64,                           0 )                             \
    INT_FIELD( max_block_bytes,       65536, 1175, ROCKETNET_MAX_BLOCK_BYTES,    0 )                             \
    INT_FIELD( unreliable_buffer_bytes, 8192, 1192, 1024 * 1024,                 0 )                             \
    INT_FIELD( packet_map_entries,      256, 16,   65536,                        0 )                             \
    INT_FIELD( receipt_records,         256, 0,    65536,                        0 )                             \
    INT_FIELD( max_packets_per_flush,     8, 1,    64,                           0 )                             \
    INT_FIELD( resend_min_ms,            20, 1,    1000,                         0 )                             \
    INT_FIELD( resend_max_ms,          1000, 10,   10000,                        0 )

struct rocketnet_message_config_t
{
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

ROCKETNET_EXPORT_FUNC void rocketnet_message_default_config( struct rocketnet_message_config_t * config );

// bytes one connection carves at create, and the server-wide delivered
// list for max_clients connections; zero on an invalid config
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_required_memory( const struct rocketnet_message_config_t * config, int max_clients );
```

Cross-field validation, refused at create: `number_window` a power of two;
`send_buffer_bytes >= max_block_bytes + max_message_bytes` when
`block_slots > 0`; `resend_min_ms <= resend_max_ms`; on the server,
`max_clients x max_packets_per_flush <= 65536`; on the client,
`max_packets_per_flush <= 256` (the client payload ring,
`sdk/rocketnet_client_transport.cpp:47`).

### 11.3 The client

```c
struct rocketnet_client_t;

// The client: a client transport and its message endpoint. NULL configs are
// the defaults (the transport's NULL is its development posture).
ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * transport_config, const struct rocketnet_message_config_t * message_config, double time );
ROCKETNET_EXPORT_FUNC void rocketnet_client_destroy( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void rocketnet_client_update( struct rocketnet_client_t * client, double time );
ROCKETNET_EXPORT_FUNC void rocketnet_client_disconnect( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC int rocketnet_client_state( struct rocketnet_client_t * client );               // the transport's state
ROCKETNET_EXPORT_FUNC int rocketnet_client_message_state( struct rocketnet_client_t * client );       // ROCKETNET_MESSAGE_STATE_*
ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t * rocketnet_client_transport( struct rocketnet_client_t * client );

// Sending. send_message copies: any reliability, any size up to
// max_block_bytes for the reliable types, and for the unreliable ones up to
// the transport's maximum payload less the SINGLE trailer (5 bytes for
// UNRELIABLE) and at most 65535. It returns the send's receipt
// serial, never zero, or zero when refused -- the reason is on a counter.
// Every send consumes a serial; only the receipt types report on it.
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_client_send_message( struct rocketnet_client_t * client, const uint8_t * data, int bytes, int reliability, int channel, int priority );

// The zero-copy form, for the unreliable types: start_message hands back a
// 16-aligned pointer into the transport's own frame and the capacity in
// context->capacity; write the message there; finish_message writes the
// trailer, seals and stages it in place, and returns the serial (zero on
// refusal). Up to ROCKETNET_MAX_SINGLE_MESSAGE_BYTES it is one wire frame,
// zero copies; above, the transport's arena and fragments, one copy. It is
// its own packet: priority does not apply, and it leaves with the next
// send_packets. Same thread rules as the transport's start_packet.
struct rocketnet_message_context_t
{
    uint8_t * data;
    int capacity;
    int reliability;
    int channel;
    struct rocketnet_packet_context_t packet;
};
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int request_bytes, int reliability, int channel, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context, int bytes );
ROCKETNET_EXPORT_FUNC void rocketnet_client_abort_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context );

// write_packets builds this connection's packets now, in the fill phase;
// send_packets builds them if not yet built this fill and flushes.
ROCKETNET_EXPORT_FUNC void rocketnet_client_write_packets( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void rocketnet_client_send_packets( struct rocketnet_client_t * client );

// Receiving: the messages delivered by the last update, valid until the
// next update, by index, as parallel arrays, or through the copying cursor,
// whose result outlives the update (it refuses a message larger than the
// buffer whole and counts it, as the transport's cursor does).
ROCKETNET_EXPORT_FUNC int rocketnet_client_num_messages( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC const uint8_t * rocketnet_client_message( struct rocketnet_client_t * client, int index, int * bytes, int * channel, int * reliability );
ROCKETNET_EXPORT_FUNC int rocketnet_client_messages( struct rocketnet_client_t * client, const uint8_t * const ** data, const int ** bytes, const int ** channel );
ROCKETNET_EXPORT_FUNC int rocketnet_client_receive_message( struct rocketnet_client_t * client, uint8_t * buffer, int capacity, int * channel );

// Receipts from the last update: serial and outcome, valid until the next.
ROCKETNET_EXPORT_FUNC int rocketnet_client_receipts( struct rocketnet_client_t * client, const uint32_t ** serials, const uint8_t ** outcomes );

// Counters: the transport's, with the message layer's appended to the same
// enum, read by rocketnet_counter_value as today.
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_client_counters( struct rocketnet_client_t * client );

struct rocketnet_message_stats_t
{
    float rtt;                      // smoothed, from this layer's own samples (milliseconds)
    float rto;                      // the current resend timeout (milliseconds)
    int queued_messages;            // send records waiting to be written
    int in_flight_messages;         // reliable records written and not acknowledged
    int held_messages;              // the receiver capacity in use (section 5.4)
    int send_buffer_bytes_used;
};
ROCKETNET_EXPORT_FUNC void rocketnet_client_message_stats( struct rocketnet_client_t * client, struct rocketnet_message_stats_t * stats );
```

### 11.4 The server

```c
struct rocketnet_server_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context, const char * public_address, const struct rocketnet_server_transport_config_t * transport_config, const struct rocketnet_message_config_t * message_config, double time );
ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC void rocketnet_server_stop( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC int rocketnet_server_state( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server, double time );
ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );
ROCKETNET_EXPORT_FUNC int rocketnet_server_message_state( struct rocketnet_server_t * server, int client_index );
ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t * rocketnet_server_transport( struct rocketnet_server_t * server );

// Slot events from the last update: a slot that connected (its message
// state is fresh) or disconnected (its undelivered state is gone).
ROCKETNET_EXPORT_FUNC int rocketnet_server_slot_events( struct rocketnet_server_t * server, const int ** client_index, const uint8_t ** events );

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_server_send_message( struct rocketnet_server_t * server, int client_index, const uint8_t * data, int bytes, int reliability, int channel, int priority );
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, int client_index, int request_bytes, int reliability, int channel, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, struct rocketnet_message_context_t * context, int bytes );
ROCKETNET_EXPORT_FUNC void rocketnet_server_abort_message( struct rocketnet_server_t * server, struct rocketnet_message_context_t * context );

// One connection's packets, on any fill thread that owns that connection;
// send_packets builds every connection not yet built this fill, then flushes.
ROCKETNET_EXPORT_FUNC void rocketnet_server_write_packets_for_client( struct rocketnet_server_t * server, int client_index );
ROCKETNET_EXPORT_FUNC void rocketnet_server_send_packets( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_messages( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC const uint8_t * rocketnet_server_message( struct rocketnet_server_t * server, int index, int * client_index, int * bytes, int * channel, int * reliability );
ROCKETNET_EXPORT_FUNC int rocketnet_server_messages( struct rocketnet_server_t * server, const int ** client_index, const uint8_t * const ** data, const int ** bytes, const int ** channel );

ROCKETNET_EXPORT_FUNC int rocketnet_server_receipts( struct rocketnet_server_t * server, const int ** client_index, const uint32_t ** serials, const uint8_t ** outcomes );

ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_server_counters( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC void rocketnet_server_message_stats( struct rocketnet_server_t * server, int client_index, struct rocketnet_message_stats_t * stats );
```

**One transport addition is needed.** The server object must see every
connection change on a slot, including a disconnect and a new admission on the
same slot between two of its updates, which `is_client_connected` and
`client_id` (`sdk/rocketnet_server_transport.cpp:1788-1797`) cannot show if the
same client reconnects. The layer 7 card adds
`uint64_t rocketnet_server_transport_client_generation( server_transport, client_index )`,
a per-slot count incremented on every admission, to the transport. Packets of
the old connection cannot open under the new connection's keys, so nothing old
is ever delivered into the new endpoint.

### 11.5 Counters

Appended to the counter enum (`sdk/rocketnet_counters.h:301`), each
`MESSAGE_` reason named where it fires above: `FORM_INVALID`, `SINGLE_INVALID`,
`RECORDS_INVALID`, `CODE_NOT_ENABLED`, `CHANNEL_OUT_OF_RANGE`, `DUPLICATE`,
`NUMBER_OUT_OF_WINDOW`, `ORDERED_STALE`, `SEQUENCED_STALE`,
`SEQUENCED_NO_SLOT`, `CONTROL_UNKNOWN`, `CONFIG_MISMATCH`, the block refusals
(`BLOCK_COUNT_INVALID`, `BLOCK_INDEX_OUT_OF_RANGE`, `BLOCK_COUNT_MISMATCH`,
`BLOCK_FRAGMENT_SIZE`), `RELIABILITY_NOT_ENABLED`, `SEND_QUEUE_FULL`,
`SEND_BUFFER_FULL`, `UNRELIABLE_BUFFER_FULL`, `RECEIPT_RECORDS_FULL`,
`UNRELIABLE_DROPPED`, `PACKET_MAP_EVICTED`, and the arrival and outcome
counters a soak sums (`RECORDS_RECEIVED`, `MESSAGES_DELIVERED`, `RESENT`,
`ACKED`). Must-stay-zero on a conforming pair: every refusal above except the
send-side `_FULL` ones and `UNRELIABLE_DROPPED`.

### 11.6 What the C# shim gets

Against the study's provisional surface (study section 13): a per-send call
taking `(payload, reliability, channel, target)` and returning a non-zero u32
serial; the eight values with RakNet's numbering; the four priorities, honoured;
a channel number refused when out of range (the shim decides what to do with
RakNet's silent map to 0); one message at a time with its sender through the
index form; receipts as `(serial, ACKED | LOSS)`, LOSS only for the unreliable
receipt type; connection notices from the client state and the server's slot
events; `DeallocatePacket` as a no-op over the update-lifetime batch, or a copy
through the cursor. Broadcast is a loop over clients above this API (flows are
REQUIREMENTS.md's separate item). The first payload byte is not reserved; the
shim adds RakNet's message id byte if its callers expect one. All of this is
rechecked when the shim's target surface arrives.

## 12. The TLA+ modules

Each in `tla/<Module>.tla` with `tla/MC<Module>.tla` and `.cfg` beside it, each
TLC run a row in `tla/RUNS.tsv`, run on vision (else hetzner), each with a
**reversed witness** that TLC must reject, each driving its transitions through
`tla/Link.tla` (loss, duplication, reordering, delay) where a peer is involved,
and each cited from the code it models. The conventions are
`docs/TLA-INVENTORY.md`'s (lines 849-874). Bottom up:

| # | module | layer | what it checks | reversed witness |
|---|---|---|---|---|
| 1 | `RNMessagePool` | 1 | every element in exactly one owner; acquire fails only when the pool is empty; reset returns every element; no element reachable from two owners after any interleaving of send, write, ack, deliver, release | release on ack before the FIFO pop: an element is both free and held |
| 2 | `RNReliableWindow` | 3 | over Link: `DeliveredOnce`; `AckedImpliesReceived`; `SpanBelowWindow` (next - oldest < W); `StaleAckHarmless` (an ack through a second packet changes nothing); liveness `EveryReliableDelivered` under fairness on some copy of each resend | numbers assigned at send with priorities, or the sender ignores the span bound: a duplicate is delivered past the window |
| 3 | `RNOrderedChannel` | 3, 4 | two channels sharing one connection's R, records at two priorities, update boundaries modelled: `InSendOrder`; `NoGap`; `AckedImpliesHeld` (the receiver never refuses a reliable record for want of a slot, including the one-update release lag); liveness `EveryOrderedDelivered`; with the sequenced actions enabled by a constant, `SequencedNeverOlder` and `SameRuleBothPaths` | three, one per property class: the drain path skips the watermark reset (RakNet item 19), `SequencedNeverOlder` fails; slots released at delivery instead of next update, `AckedImpliesHeld` fails; no head exemption, `EveryOrderedDelivered` fails (section 5.4's deadlock) |
| 4 | `RNMessageConfig` | 3 | the CONFIG exchange over a lossy Link: each side eventually knows the peer's capacities; the sender never exceeds the floors before, nor the minimum after; a mismatch reaches the error state on both ends | CONFIG sent once, unreliably: a first-packet loss leaves the windows at the floors forever (the transition rule) |
| 5 | `RNBlocks` | 5 | `ReassembledEqualsSent` (content by index), `BlockDeliveredOnce`, `BlockSlotsSuffice` at `B_eff`, `NoStaleMerge` across id reuse, `ReceiptAfterAllFragments` | arrival-order reassembly (RakNet section 8.2), or the receipt on the last-index fragment |
| 6 | `RNReceipts` | 4 | every serial of a receipt type gets exactly one outcome; LOSS only for type 5; ACKED for a reliable type only after every record is acknowledged; no outcome for a serial never issued | LOSS reported for a reliable receipt type on a resend |
| 7 | `RNScheduler` | 6 | resends before new data; no non-empty priority starves (liveness); the stride ratios within a bound over a window; an unreliable record is never blocked by a full reliable window | strict priority: LOW starves |
| 8 | `RNMessageConnection` | 7 | two endpoints over Link in both directions with a server slot that disconnects and readmits: nothing of an old connection reaches a new one; a reset leaves every pool free; the object's update order (release, receipts, acks, payloads) preserves modules 2 to 6 | a slot reused without the generation check: the new endpoint keeps old state |

The codec (layer 2) and the C API (layer 8) are not state machines and get
vectors, round trips, refusal tests and fuzzing instead.

## 13. Every type reachable: the reviewer's table

The test this document must pass: a type that cannot be added later without
changing the wire or the per-channel state means the design is broken. For each
of the six types that do not ship first, what it needs from each layer, and
whether the first version already has it.

| type | wire (section 4) | per-channel state (5.3) | connection state | code to add later |
|---|---|---|---|---|
| UNRELIABLE_SEQUENCED (1) | code 1; channel, ordering, sequencing fields: in the v1 layout, parsed by v1 | `sequenced_write`, `sequenced_read`, reset by v1's ordered paths; reorder key already (o, s) | its own Ru reorder slots, carved by v1 (drop when full; it is unreliable) | enable the row; the sequenced branch of the one dispatch function |
| RELIABLE (2) | code 2; number: in v1 (RELIABLE_ORDERED uses it) | none | in-flight table, packet map: v1's | enable the row; the deliver-on-arrival branch |
| RELIABLE_SEQUENCED (4) | code 4; all fields in v1 | as type 1; joins the channel FIFO as an ordered record does | as type 2 | enable the row |
| UNRELIABLE_WITH_ACK_RECEIPT (5) | code 0 on the wire, unchanged | none | receipt records (Q pool, carved by v1); packet map refs | the receipt record and the deadline sweep |
| RELIABLE_WITH_ACK_RECEIPT (6) | code 2 | none | the serial on every send record, from v1 | the ACKED check at acknowledgment |
| RELIABLE_ORDERED_WITH_ACK_RECEIPT (7) | code 3 | none | as type 6 | as type 6 |

**The wire never changes**: all five codes and every field are in the first
layout and parsed by the first reader. **The per-channel state never changes**:
the four counters, the FIFO and the reorder list are the first version's 48
bytes. **The allocator layout never changes**: every pool, including receipts,
is carved by the first version. The study's invariants, answered:

| invariant | where |
|---|---|
| I1 code in the header, dispatch on it, unknown dropped | 4.2, 6 |
| I2 header variable by type | 4.2 |
| I3 numbers and indices independent; unreliable never takes a number | 5.1, 5.3 |
| I4 message number is not the transport sequence | 5.1 |
| I5 four counters per channel, coupled, one watermark rule on both paths | 5.3 |
| I6 reorder keyed by (ordering, sequencing) | 5.3 |
| I7 everything bounded at create, stated policy when full | 3, 5.4 |
| I8 per-packet acks, packet to list of records | 5.2 |
| I9 serial on every record, issued by the API | 6 |
| I10 unreliable receipt record outlives the frame | 6 |
| I11 where large messages split, and how unreliable ones go | 6, 7 |
| I12 sequenced acked on arrival, may be discarded | 6 |
| I13 unreliable not charged to the reliable window | 5.4, 8 |
| I14 RTO and death per connection | 5.2 |
| I15 channel bound per connection, both ends; width from the value | 4.2, 5.3 |
| I16 the protocol does not share a user channel | 4.5 |
| I17 messages, not packets, in one update-lifetime stream | 9.3, 11 |

At 1024 and 10,000 channels nothing changes but `ROCKETNET_MAX_CHANNELS` and the
channel term of section 3.1: the queues are per connection and per priority,
not per channel; the FIFO and reorder lists are intrusive; the wire widens the
channel field by one byte for channels from 256.

## 14. The cards, in order

Each secured before the next starts, per the standing practice; the first
version is cards 1 to 7, then the game integrates.

1. Layer 1, the allocator, with `RNMessagePool`.
2. Layer 2, the codec, with its vectors and fuzz target.
3. Layer 3, the reliable path and the configuration exchange, RELIABLE_ORDERED
   enabled, with `RNReliableWindow` and `RNMessageConfig`.
4. Layer 3, ordering channels, with `RNOrderedChannel`.
5. Layer 6, the scheduler, UNRELIABLE enabled, aggregation and the SINGLE form,
   with `RNScheduler`.
6. Layer 7, the endpoint, the client and server objects and the transport's slot
   generation, with `RNMessageConnection`.
7. Layer 8, the C API and its C# smoke.
8. Then the space game integrates. Then blocks (layer 5, `RNBlocks`), then the
   six other types (section 13, `RNReceipts`), then the stress runs at 1024 and
   10,000 channels.

## 15. Not decided here

- **STANDARD.md's scope.** It says messages are not specified there
  (STANDARD.md lines 7-10). Sections 4.1 to 4.6 and the floors of section 5.6
  are written to become its "Messages" section; whether the standard grows that
  section is Glenn's ruling, so STANDARD.md is not edited by this card.
- **The configuration exchange.** This design exchanges capacities and requires
  three fields to match. The simpler alternative, identical configurations
  required and unchecked (yojimbo's), was rejected because a mismatch would lose
  reliable messages the transport had acknowledged. Glenn may prefer the simpler
  rule.
- **The fragment stride.** 1024 is chosen for placement without staging. A
  stride equal to the packet capacity would save about 12% of block fragments
  and bring back the staging rule.
- **The C# shim's surface** replaces section 11.6 when it arrives.
- **Congestion control.** The reliable path is bounded by its windows and the
  per-flush packet count, not by a congestion window. RakNet's sliding window
  (study section 9) is the reference if measurement asks for one; it would
  charge only reliable bytes (I13).

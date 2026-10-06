# Channels: the message layer above the transports

The design for RocketNet's message layer: a client and a server that send
messages over `n` ordering channels with RakNet's eight reliabilities, chosen
per send, built on `client_transport` and `server_transport`. Every layer card
that implements it starts from this document. It is written to be read beside
[RAKNET-RELIABILITY.md](RAKNET-RELIABILITY.md) (cited as _the study_, section
numbers `§n`, invariants `I1` to `I17`).

**Status.** Design, not code. Nothing here exists in `sdk/` yet. The layer list
in section 3 goes to Glenn for correction before layer 1 starts. Struct sizes in
section 14 are working figures, which the layer 1 card fixes with `sizeof`.

**What ships first.** UNRELIABLE and RELIABLE_ORDERED, then the client and
server objects, so the space game can integrate and prove the layer. The other
six reliabilities follow. Section 17 shows, one type at a time, that each of
the six slots in with no change to the wire and no change to the per-channel
state. Anyone who finds a type that cannot be added that way has found this
design broken, and should say so.

**Citations.** `file:line` is relative to the repository root at the commit
this document lands on. `S:n` is STANDARD.md line `n`, `U:n` is USAGE.md line
`n`. RakNet facts come from the study, and are cited by its section.

---

## 1. The rules every layer keeps

Six rules. The first three come from the card. The other three follow from them,
or from the transport.

**R1. No allocation on a send or receive path.** Every buffer comes from the
connection's pools (layer 1) or from the transport's frame. Memory is spent once,
at create. A pool that runs out produces a refusal the caller can see, or a
counted drop. It never grows.

**R2. Reliability is chosen per send, as RakNet does it.** The ordering channel
is a number passed with each send. `ROCKETNET_MAX_CHANNELS` (32 by default) is
the compile-time maximum. `num_channels` is a per-connection runtime count at or
under it.

**R3. No state transition between peers rides a single unreliable packet.** A
packet that announces something the peer must act on (a channel start, a block
start, a reset, a disconnect) goes one of two ways. Either it is carried on a
reliable path, or it is re-sent on a timer until the peer's next-state packet
confirms it, which is what netcode does for its handshake. A lost first packet
must never hang either side. Section 16 lists every transition in this design
and how it is carried. The short version: this layer has **no** channel-open,
block-start or reset packets at all. The one thing it announces, its
configuration, goes out as a reliable record.

**R4. Every state machine is a pure module.** Its state is one struct. It is
driven by events: a payload's parsed contents, an application call, or time,
passed in as a value. It answers with actions the caller performs: write these
bytes into this frame, deliver this message, report this receipt, disconnect
this connection. Inside a machine there is no socket, no clock read and no
allocation, so a unit test can drive it end to end without a socket. The client
and server objects (layer 8) are the only code that binds a machine to a
transport and a clock. (Glenn, 2026-10-05: the security and reliability machines,
the client and server logic, and the transport connection logic all come out
as state machines so they can be tested without sockets. This card covers the
message machines. Extracting the transport's own machines is a separate card.)

**R5. The receiver never drops a reliable record once its packet has opened.**
The transport acknowledges every session packet it opens
(`sdk/rocketnet_server_protocol.cpp:791`, `sdk/rocketnet_client_protocol.cpp:711`),
before this layer has seen the payload. An acknowledged record the receiver then
dropped would never be resent, and its channel would hang. So the receiver must
always have room for every reliable record it can legally be sent. The sender
guarantees that room by counting the receiver's capacity (section 6.3). This rule
shapes everything below layer 4.

**R6. Constant shape.** Nothing iterates over all channels on a send, a receive
or an update. Channel count changes memory, never per-operation cost. This is how
the 1024-channel and 10,000-channel stress runs change one number and nothing
else.

---

## 2. Layer 0: what the transport gives, and what it does not

This layer is not built by any card here, but every layer above it relies on it.
Each item is a precondition, and each says how it is checked. Two items are
findings from this reading that the design depends on.

| # | Precondition | Source | Status |
|---|---|---|---|
| P1 | An ack block names only sequences its sender opened. | `S:1733` | **Broken today by F1** (`docs/TLA-INVENTORY.md:30`, `sdk/rocketnet_replay.cpp:54-60`). A forward jump of 4096 or more sequences leaves stale bits in the replay window, and the next block claims thousands of sequences that were never opened. The peer marks them acked (`docs/TLA-INVENTORY.md`, "F1, traced"). A falsely acked reliable record is never resent, and its channel hangs for good. **F1 must be fixed before RELIABLE_ORDERED ships.** It is card 1 in section 19. |
| P2 | An acked **type 5** payload was placed in the receiver's delivered batch in the same update that opened it. | Server: the batch holds `ROCKETNET_MAX_PAYLOADS_PER_UPDATE` (`sdk/rocketnet_constants.h:53`), which is the drain bound, so the stack overflow buffer at `sdk/rocketnet_server_transport.cpp:982-985` is never chosen for a drained packet. Client: the batch is `ROCKETNET_NUM_CLIENT_PACKETS` (`sdk/rocketnet_client_transport.cpp:407-408`). | Holds by sizing. Owed: a test that drains a full receive batch and asserts every opened payload is in the batch. |
| P3 | An acked **fragmented** payload id does **not** prove the payload was delivered. | The fragment packet is marked received (`sdk/rocketnet_server_protocol.cpp:791`) before reassembly runs (`:924`), and a fragment refused as stale or abandoned is still acknowledged. `rocketnet_ack.h:94-99` surfaces the payload's id once every fragment's wire packet is acked. | **Finding.** So this layer never puts a reliable record, or a record whose receipt depends on delivery, on the transport's fragment path (section 8). |
| P4 | `packet_id` is opaque and process-unique. It is not dense and not per connection. | `sdk/rocketnet.h:858-860`, `sdk/rocketnet_packet_lease.h:7-8`, `sdk/rocketnet.cpp:521-531` | The layer maps `packet_id` to its own records through a hash (section 6.4). It never relies on ids being monotonic. |
| P5 | The client delivers payloads from _sending connection response_ (state 3) on, but sends them only once _connected_. | `S:2205-2210`, `sdk/rocketnet_client_protocol.cpp:670-676`, `:1056-1060` | So the client endpoint is live from create and is reset only at create (section 11.3). |
| P6 | Within one update, no payload for a slot comes from a connection admitted during that update. A new connection's first payload needs a keep-alive round trip, and that round trip ends after the batch was captured. | `sdk/rocketnet_client_protocol.cpp:803-807`, `:1056-1060`, and the batch is captured once at the top of update (`sdk/rocketnet_server_transport.cpp:973`, `:1054`) | Argued, not tested. Owed: a test that reuses a slot inside one update. |
| P7 | The transport needs a way to tell a slot changed hands. | none exists | **Transport addition, named and not built:** `uint64_t rocketnet_server_transport_client_generation( server_transport, client_index )`, which increments each time the slot admits a connection. It is card 2 in section 19. |
| P8 | Causality across one update. No packet in an update's batch was sent by the peer after the peer saw anything this end sends once that update has begun. | The batch is captured before update's own sends: `sdk/rocketnet_server_transport.cpp:973`, `:1054`, `:1140`; `sdk/rocketnet_client_transport.cpp:563-591` | Physical. Section 6.3 uses it. |
| P9 | A wire frame is 16-aligned, its capacity is 1192 bytes, and it is zero copy. A request above 1192 bytes routes through the arena and fragments, with one copy. | `sdk/rocketnet.h:420`, `:815-839`, `sdk/rocketnet_constants.h:44-46` | Holds. |
| P10 | Fill-phase threading: `start_packet`, `finish_packet` and `abort_packet` run on any game threads, strictly between `update` and `send_packets`. | `sdk/rocketnet.h:841-860`, `U:46-50` | Holds. Inherited by section 16.1. |
| P11 | A spent sequence is never given back. `abort_packet` burns one. | `U:126-137` | The endpoint answers "do you have output?" before the object starts a packet (sections 9.3 and 11.1), so the object never aborts in the ordinary path. |
| P12 | Ack history is a packet-count budget, and an evicted record is never acked (`SEND_HISTORY_OVERFLOW`). | `U:283-291` | No ack is not proof of loss. This layer resends on its own timer and never waits for a loss report (section 6.5). |

What the transport gives, then: a zero-copy frame per packet, a `packet_id` per
packet, a batch of acked `packet_id`s per update that is truthful once F1 is
fixed, a batch of payloads per update that is valid until the next update, and
unreliable fragmentation up to the configured maximum payload. What it does not
give: resends, loss reports, ordering, message boundaries inside a payload, a
per-slot generation, or a delivery guarantee for fragmented payloads.

---

## 3. The layers, bottom up

Each layer owns some state, guarantees some things to the layer above, and is
checked by named instruments. A layer is secured (Glenn's bar: modelled with
reversed witnesses, tested on a real store including every refusal, probed green
by a second reader, used for real work, usable cold from its own help,
documented, locked) before anything above it starts.

| Layer | Name | Owns | Pure? | Section |
|---|---|---|---|---|
| 0 | transport preconditions | nothing new | (transport) | 2 |
| 1 | pools | per-connection buffers and descriptors, carved at create | yes | 4 |
| 2 | records | the wire codec: write and parse records, and the SINGLE frame form | yes (functions) | 5, 13 |
| 3 | reliable window | message numbers, the send ring, the receive bitmap, sent-packet records, the RTO | yes | 6 |
| 4 | channels and per-send reliability | per-channel ordered and sequenced state, holds, dispatch for all eight types | yes | 7 |
| 5 | blocks | reliable messages larger than a packet: split, number, reassemble | yes | 8 |
| 6 | scheduler | four priorities, channel heads, packet assembly, the packet budget | yes | 9 |
| 7 | endpoint and slots | the per-connection machine composed of layers 1-6, and the server's slot lifecycle machine | yes | 10 |
| 8 | client and server objects | binding machines to transports and time | no: the only binder | 11 |
| 9 | the C API | `sdk/rocketnet.h` surface, config table, counters | n/a | 12 |

Data flow, send side: `send_message` copies into a pool buffer and enqueues a
descriptor (layers 1, 4, 6). `write_packets` asks the scheduler to fill
transport frames with records (layers 6, 3, 2), and records which window
numbers each packet carries (layer 3). `start_message`/`finish_message` writes
one unreliable message straight into a transport frame and appends its trailer
(layer 2). That is the SINGLE form, and it makes no copy.

Data flow, receive side: each payload in the transport batch is parsed whole
(layer 2) and validated before any record takes effect. Reliable records pass the
window (layer 3). Each record is then dispatched by its code (layer 4). In-order
records are delivered **pointing into the transport's batch**, so in-order
receive is zero copy. Out-of-order reliable records are copied once into a hold
buffer (layer 1), and so are block fragments, which go into a block buffer
(layer 5). Each acked `packet_id` advances the window (layer 3).

---

## 4. Layer 1: pools

**Owns.** One contiguous allocation per connection, made at create through
`rocketnet_malloc` (`sdk/rocketnet.h:319-329`), and carved into fixed-size
class pools. Each pool is an array of buffers with an index free list.

| Pool | Buffer bytes | Holds | Count (config) |
|---|---|---|---|
| send SMALL | 64 | queued or in-flight messages of 1..64 bytes | `send_small` |
| send MEDIUM | 256 | 65..256 bytes | `send_medium` |
| send PACKET | 1184 | 257..1176 bytes (`ROCKETNET_MESSAGE_PACKET_BYTES`) | `send_packet` |
| send BLOCK | `max_block_bytes` rounded up to 64 | anything larger: reliable blocks, and large unreliable messages waiting for the transport's arena | `send_blocks` |
| hold SMALL / MEDIUM / PACKET | 64 / 256 / 1184 | received reliable records waiting for an earlier one | `hold_small`, `hold_medium`, `hold_packet` |
| hold BLOCK | `max_block_bytes` rounded up to 64 | block reassembly | `hold_blocks` |
| held-sequenced | 1184 | UNRELIABLE_SEQUENCED records ahead of their epoch (section 7.4) | `held_sequenced` |
| descriptors | 48 each | one per send buffer | the sum of the send counts |
| hold records | 32 each | one per held record | `window_size + held_sequenced` |

**Why classes, not a general allocator.** R5 needs a stronger promise than "it
probably fits". The receiver must be able to store everything the sender's
accounting allows. With a general allocator (TLSF, buddy) live bytes under a
budget can still fail to fit, because of fragmentation. With fixed classes,
a class with a free buffer always succeeds, and the sender can count exactly
how many buffers of each class the receiver may be asked to hold (section 6.3).
Class sizes are constants of the design. The counts are configuration. The
class of a message is a function of its byte length, which both ends compute
the same way (`rocketnet_message_class( bytes )`).

**Guarantees.**
- `acquire(class)` and `release(handle)` are O(1) and never allocate.
- `acquire` fails only when that class is empty, and reports it.
- A buffer handle is `{class u8, index u32}`, so it is never a raw pointer that
  outlives a reset.
- `reset` returns every buffer and leaves the free count equal to the configured
  count.
- Buffers are 16-aligned, and the PACKET and BLOCK classes are 64-aligned.

**Checked by.**
- Unit tests: exhaust every class and assert the refusal; release out of order;
  double release (asserted in debug, counted in release); reset with buffers
  live; `free + live == count` after every operation of a random sequence.
- TLA+ `RNPools` (section 15).
- Soak counter: `MESSAGE_POOL_EMPTY` by class, must stay zero on the receive
  side in an honest soak (receive-side emptiness is a violation, section 6.3).

---

## 5. Layer 2: records in a packet

A transport payload carries one or more **records**, end to end, and each
record carries one message or one fragment of a block. Every record has its
header **after** its data, as a trailer. The receiver parses a payload backward
from its last byte.

**Why trailers.** The zero-copy unreliable send writes the message at the
frame's payload pointer, which is 16-aligned and is where a word-based
serializer roots (`sdk/rocketnet.h:815-820`). With the header at the end, the
message is the first thing in the payload, at offset 0, and nothing moves
before the seal. A header in front would push the message off alignment, or
cost a memmove. One record format serves both the SINGLE form (one record, data
at offset 0) and the RECORDS form (many records aggregated). No form byte is
needed, and the parser has no second path.

**Record, forward byte order** (full bit layout in section 13):

```
data[L] | number u24? | ordering u24? | sequencing u24? | channel u8/u16? | frag index u16? | frag count u16? | L u8/u16 | flags u8
```

- `flags`: bits 0-2 hold the **wire code**, bit 3 is BLOCK, bit 4 is LONG (L is
  u16), bit 5 is WIDE (channel is u16), and bits 6-7 are reserved and zero.
- The fields present follow from the wire code. Wire codes are RakNet's 0-4
  (`§2`: the receipt variants never reach the wire), plus code 5,
  RELIABLE_CONTROL, which is this layer's own. Codes 6 and 7 are reserved.

Header bytes with short length and a narrow channel. These are RakNet's sizes
(`§4.2`) less one byte, because the length is a byte count with a short form:

| Code | Type on the wire | Fields | Header bytes |
|---|---|---|---|
| 0 | UNRELIABLE (also UNRELIABLE_WITH_ACK_RECEIPT) | L, flags | 2 |
| 1 | UNRELIABLE_SEQUENCED | ordering, sequencing, channel, L, flags | 9 |
| 2 | RELIABLE (also RELIABLE_WITH_ACK_RECEIPT) | number, L, flags | 5 |
| 3 | RELIABLE_ORDERED (also RELIABLE_ORDERED_WITH_ACK_RECEIPT) | number, ordering, channel, L, flags | 9 |
| 4 | RELIABLE_SEQUENCED | number, ordering, sequencing, channel, L, flags | 12 |
| 5 | RELIABLE_CONTROL (this layer's own, never delivered to the application) | number, L, flags | 5 |

LONG adds 1 byte, WIDE adds 1 byte, and BLOCK adds 4. The largest header is 18
bytes (code 4 with all three flags).

**The SINGLE frame form.** This is one record of code 0 (later also 1 and the
unreliable receipt type) whose data starts at payload offset 0. `start_message`
hands the caller `frame + payload_index` with capacity
`ROCKETNET_MESSAGE_PACKET_BYTES = 1176`. That is 1192 less 16 bytes reserved for
the trailer, a multiple of 8 so the transport's capacity rule carries over
(`sdk/rocketnet.h:411-418`). `finish_message( bytes )` appends the trailer at
`payload + bytes` and calls the transport's `finish_packet( bytes + trailer )`.
The message is sealed where the caller wrote it. Zero copies. The receiver sees
an ordinary one-record payload.

**The RECORDS form.** This is the same format, with up to
`ROCKETNET_MESSAGE_MAX_RECORDS_PER_PACKET = 64` records laid end to end,
written by the scheduler (layer 6). Small unreliable messages aggregate here,
together with reliable records and resends.

**Alignment promise: on send only.** The SINGLE form gives the writer a
16-aligned pointer, which is the transport's promise (`sdk/rocketnet.h:815-820`).
On receive there is no alignment promise, from the transport or from this layer.
The server's batch stores payloads at a 1393-byte stride
(`sdk/rocketnet_server_transport.cpp:982-983`), so a received payload is not
aligned. After the first record, records sit end to end. Messages are byte
buffers (CLAUDE.md, "Messages are opaque byte buffers"). A reader that needs
alignment copies the message.

**Parse rule.** Validation runs over the whole payload first, then the records
are applied, so a malformed payload has no partial effect:

1. Read `flags` at `end - 1`. Reserved bits set: `VIOLATION_RESERVED`. Code 6
   or 7: `VIOLATION_CODE`. Code not enabled in this build: `VIOLATION_TYPE`.
2. Read L (u8, or u16 when LONG). If L is 0, or LONG with L < 256:
   `VIOLATION_LENGTH`.
3. Read the code's fields backward. BLOCK on a code other than 2, 3 or 4, or WIDE
   with channel < 256, or a channel at or above the connection's
   `num_channels`: `VIOLATION_FIELD`.
4. The data begins at `end - trailer - L`. If that is below the record's start
   bound: `VIOLATION_TRUNCATED`. The next record ends just before the data.
5. Stop at exactly offset 0. More than 64 records: `VIOLATION_RECORDS`.

Then the records are applied in **forward** order. The parse stored the record
offsets in a 64-entry array, so delivery order inside a packet is send order.

Any violation stops processing that payload and raises the endpoint's
DISCONNECT action with the reason (section 10). An honest peer never violates.
A peer that does may have been acked for records this end will not process, so
continuing would break R5.

**Guarantees.**
- `write_record` and `parse_payload` are pure functions over byte spans.
- Every byte of a payload belongs to exactly one record.
- A payload that fails format validation has no effect. (The window, hold and
  block checks run as records apply, and a failure there disconnects; 13.3.)
- The encoding is canonical (one byte string per record), so the known-answer
  vectors in section 13 are a straight memcmp, as the standard's are (CLAUDE.md,
  "the assembled wire vectors ... diff against the implementation with a
  straight memcmp").

**Checked by.**
- Known-answer vectors (section 13.6), written and parsed.
- A round trip of random record lists through write and parse.
- One negative test per violation, each asserting its counter and that nothing
  was applied.
- A fuzz target `fuzz/rocketnet_fuzz_message_records.cpp` beside the existing
  ones (`fuzz/`), with a seed corpus from the vectors.
- No TLA+ module: the codec has no state.

---

## 6. Layer 3: the reliable window

### 6.1 What it owns

**Sender.**
- `next_number` (N): the next 24-bit message number to assign.
- `window_base` (U'): the lowest number still accounted.
- A ring of `window_size` (W) **window entries** indexed by `number mod W`, each
  holding: number, descriptor, fragment index, receive class, acked flag,
  last-sent time, and resend-list links.
- The **class counts in window**: how many entries of each receive class lie in
  `[U', N)`, with a block counted once at its first fragment.
- The **resend list**: unacked entries in order of last send, so the head is the
  next one due. It is RakNet's resend list (`§5.4`).
- Sent-packet records, with a hash from `packet_id` to record (section 6.4).
- The RTT estimate and the RTO.

**Receiver.**
- `receive_base`: the lowest number not yet received.
- A `W`-bit bitmap over `[receive_base, receive_base + W)`.

The numbers are a connection-wide counter family, separate from the per-channel
ordering indices, as RakNet keeps them (I3). Only reliable records (codes 2, 3,
4 and 5) take a number. Nothing else moves the receive window. A resent record
keeps its number in a new transport packet with a new `packet_id` and sequence
(I4).

### 6.2 Numbering is late, as in RakNet

A message takes its number when it is first written into a packet, not at
`send_message` (`§5.2`, `IP.h:83-85`). A block takes all its fragments' numbers
at once, consecutively, when its first fragment is first written (section 8).

**Within one channel, reliable messages are first written in ordering-index
order.** The scheduler enforces this (section 9.2). Section 6.3 needs it, and it
costs nothing a game can see: an ordered message written ahead of an earlier one
on its channel would only be held at the receiver.

### 6.3 The receiver's capacity, kept by the sender

R5 says the receiver must never be unable to store a reliable record it has
opened. The sender guarantees this by numbering a new message only when, after
numbering it:

```
N - U'                           <= window_size
count_in_window[c]               <= hold_c          for c in SMALL, MEDIUM, PACKET
blocks_in_window                 <= hold_blocks
```

Here `hold_*` and `window_size` are the **receiver's** capacities, and the
configuration contract makes them equal to the sender's own (section 10.4). U'
advances past an entry only when the entry is acked **and**, if it is a block
fragment, its whole block is acked. So U' is the first number of the block
containing the lowest unacked number.

**Why this bounds the receiver.** Take any moment while the receiver is
processing update `u`.

1. *Held ordered records.* A record x is held because an earlier message m on
   its channel is missing. First-write order within a channel (6.2) gives
   `number(m) < number(x)`. m was not received, so it is not acked (P1), so
   `U' <= number(m)` at every moment the sender could have sent x. Hence x is in
   `[U', N)`.
2. *Held block fragments.* A partial block is missing some fragment f, so
   `U' <= first(block)` (the block rule above), and every received fragment of
   the block is in `[U', N)` and counted once as a block.
3. *Records processed this update.* The transport marks a packet received
   when it opens it, inside this update, so the first ack naming record n rides
   a packet this end seals after the batch was captured. By P8, no packet in the
   batch was sent after the sender saw that ack. So at the send time of every
   packet in the batch, `U' <= n`.
4. *Records released from hold this update* are delivered from their hold
   buffers, which stay allocated until the next update's `begin_update`. Each
   was released because a predecessor arrived in this update, and by (3) that
   predecessor still held U' at or below itself at every send time in the batch.
   So the released records are in the window too.

Every receive-side buffer live during update `u` therefore holds a number in
`[U'_T, N_T)`, where T is the latest send time among the packets processed so
far, and the sender kept that interval within the receiver's class counts. The
receiver's hold pools cannot run out against an honest sender. Running out is
a violation (`VIOLATION_CAPACITY`), and the endpoint disconnects.

This argument is model-checked by `RNReliableWindow` (section 15), whose reversed
witness drops rule 6.2 and finds the overflow.

**Delivered-list bound, from the same argument.** Each update delivers at most
`window_size` reliable messages per connection (all live reliable numbers lie in
one window). The per-connection delivered list therefore reserves
`window_size` entries for reliable messages, plus `max_unreliable_delivered` for
unreliable ones. Unreliable records past that cap are dropped and counted
(`UNRELIABLE_DELIVERED_FULL`), which is legal because they are unreliable.

### 6.4 Sent-packet records and the `packet_id` map

Each packet the endpoint writes gets a **sent-packet record**: `packet_id`,
send time, and a span of **packet entries**. An entry is either a window number
(a reliable record this packet carried) or an unreliable-receipt serial with its
deadline (section 7.5). This is RakNet's datagram history (`§5.4`, I8) keyed by
the transport's `packet_id`. A packet mixes reliabilities, so the map is from
packet to a list of records from the first version, even when a packet carries
one record.

- Records live in a ring of `sent_packets` (P) in send order. Entries live in a
  ring of `packet_entries` (E).
- A hash with `2P` open-addressed slots maps `packet_id` to record. Lookup is
  O(1) and does not depend on ids being monotonic (P4).
- When the ring wraps, the oldest record is evicted. Its reliable entries are
  still resent by their own timers (6.5), and its pending unreliable receipts
  report LOSS. Eviction is counted (`SENT_RECORD_EVICTED`). It costs efficiency,
  never correctness.

**On an acked `packet_id`.** Look up the record (a miss is ignored and counted,
covering ids from a previous slot generation and evicted records). Take one RTT
sample from it (6.5). For each window entry: if the ring slot still holds that
number and it is unacked, mark it acked, unlink it from the resend list, and
decrement the message's unacked-fragment count. At zero, the message is
complete: its send buffer is released, and its receipt fires if its type has
one. Then advance U' over acked entries, subject to the block rule, and
decrement the class counts as entries leave. Retire the record.

### 6.5 Resend, RTT and giving up

- **RTT.** Each sent-packet record yields one sample, the first time it is acked:
  `ack update time - send time`. Every resend is a new packet, so samples are
  never ambiguous. Karn's problem does not arise, which is RakNet's situation too
  (`§9`). The layer keeps its own estimate rather than reading the transport's
  rtt, which is a floor and lags upward (`U:216-236`): `srtt` and `rttvar` use
  RFC 6298 gains (1/8, 1/4), and
  `RTO = clamp( srtt + 4 * rttvar, rto_min, rto_max )`, with `rto_initial` before
  the first sample. The defaults are 50 ms, 1000 ms and 250 ms. The 1000 ms cap
  follows RakNet's 2 s cap halved, because a game's tick-paced acks arrive faster
  than RakNet's 10 ms SYN-paced ones do (`§7.2`); it is a default, not a rule.
- **Resend.** An unacked entry is due when `now - last_sent >= RTO`. The scheduler
  writes due entries first, oldest first, from the head of the resend list (section
  9.3). There is no exponential backoff, as in RakNet (`§9`, "the per-message
  `timesSent` parameter is ignored"). The packet budget is what limits resend
  bursts.
- **A written packet that the transport dropped.** `finish_packet` can return
  zero (`sdk/rocketnet.h:862-865`). The object then calls
  `endpoint_packet_dropped( packet_id )`, which makes that packet's reliable
  entries due at once and reports LOSS for its unreliable receipts.
- **Giving up.** An entry unacked for `reliable_timeout` (default 10 s, RakNet's
  `timeoutTime`, `§5.4`) raises DISCONNECT with reason `RELIABLE_TIMEOUT`. The
  transport's own idle timeout usually fires first. This is the backstop for a
  peer that stays connected but never acknowledges the record.

### 6.6 The receiver's window check

For a record numbered `n`, with `d = (n - receive_base) mod 2^24`:

- If `d >= 2^23`, n is behind the base. It is a duplicate: drop it and count
  `DUPLICATE`.
- If `d >= window_size`, the sender broke the window: `VIOLATION_WINDOW`.
- If bit `d` is set, it is a duplicate: drop it and count it.
- Otherwise, set bit `d` and pass the record to layer 4. Then advance
  `receive_base` over the leading set bits, clearing each one.

**24 bits is enough, and here is the bound.** A late duplicate can only arrive in
a packet the transport still accepts, which is one within `ROCKETNET_HISTORY_PACKETS`
(at most 32,768) sequences of the newest opened (`sdk/rocketnet_replay.cpp:32-39`).
Those packets can carry at most `32768 * 64` new numbers between them. Adding
`window_size` (at most 32,768) gives 2,129,920, which is below the half range of
2^23 = 8,388,608. The same bound covers the 24-bit ordering and sequencing indices.

### 6.7 Checked by

- TLA+ `RNReliableWindow`: two windows over `Link.tla` with loss, duplication and
  reordering, a truthful or a lying ack channel, and class counts. It has three
  reversed witnesses: no first-write order (receiver overflow), F1-style lying acks
  (a liveness failure), and U' advancing past a partial block (overflow).
- Unit tests that drive the pure window with scripted acks and payloads, without
  sockets. They cover wrap at 2^24, a full window with refusal visible as "no new
  numbers", eviction, `packet_dropped`, and reliable timeout.
- Soak counters that must stay zero on an honest link: `VIOLATION_WINDOW`,
  `VIOLATION_CAPACITY`, `RELIABLE_TIMEOUT`.

---

## 7. Layer 4: channels and per-send reliability

### 7.1 Per-channel state, all eight types from the first version

Each channel holds the same 32 bytes for every type, from the first version:

| Side | Field | Width | What it is | Used by |
|---|---|---|---|---|
| send | `ordered_write` | u32 (24 used) | the next ordering index | 3, 4, 7 (and read by 1) |
| send | `sequenced_write` | u32 (24 used) | the next sequencing index within the current epoch | 1, 4 |
| send | `pending_head`, `pending_tail` | u32 each | the channel's FIFO of reliable descriptors not yet numbered | 3, 4, 7 |
| send | `priority` | u8 | the priority the channel's head is scheduled at (section 9.2) | 3, 4, 7 |
| receive | `ordered_read` | u32 (24 used) | the next ordering index to deliver | 1, 3, 4, 7 |
| receive | `sequenced_read` | u32 (24 used) | the sequenced watermark within the current epoch | 1, 4 |
| receive | `held_head` | u32 | held records sorted by (ordering, sequencing) | 1, 3, 4, 7 |

Per-channel state is 29 bytes, padded to 32. At 10,000 channels this is 320,000
bytes per connection, and nothing else changes (R6).

### 7.2 The epoch coupling, both sides, from the first version (I5)

These are RakNet's rules (`§3.2`), with the two receiver fixes the study names
(`§11` items 18 and 19):

- **Ordered send** (codes 3 and 7, and 4's epoch): `ordering = ordered_write++`,
  then `sequenced_write = 0`. The first version does the reset even though no
  sequenced type ships yet, so adding sequenced later changes nothing on the
  ordered path.
- **Sequenced send** (codes 1 and 4): `ordering = ordered_write` (the current
  epoch, not incremented), `sequencing = sequenced_write++`.
- **Ordered delivery**: `ordered_read++` and `sequenced_read = 0`, on **every**
  path. That includes a record delivered on arrival and a record released from
  hold. RakNet skips the reset on the heap path (item 19). This design does not.
- **Sequenced delivery**: `sequenced_read = sequencing + 1`, on every path.
  RakNet writes `sequencing` without the `+ 1` on the heap path (item 18). This
  design does not.

**Indices are assigned at `send_message`, as RakNet does it** (`§5.2` step 5).
Every fragment of a block carries the block's indices, so a block takes one
ordering slot (I11).

### 7.3 Dispatch per record, by wire code

The window (layer 3) passes reliable records through. Then:

| Code | Receive rule |
|---|---|
| 0 | Deliver now. Zero copy: the pointer is into the transport batch. |
| 2, 5 | Deliver now (code 5 goes to the control handler, not the application). Zero copy. |
| 3 | If `ordering == ordered_read`: deliver, then release held successors in (ordering, sequencing) order while they match. If newer: hold (copy into a hold buffer of its class). If older: drop and count `STALE_ORDERED`. That cannot happen with an honest sender, because the window drops duplicates first. |
| 1, 4 | If `ordering == ordered_read` and `sequencing >= sequenced_read`: deliver and set the watermark. If `ordering == ordered_read` but older: drop and count `STALE_SEQUENCED`. If `ordering` is newer: hold (code 4 in the window's hold pools; code 1 in the held-sequenced pool, section 7.4). If `ordering` is older: drop and count it. |

"Newer" and "older" compare 24-bit values with serial arithmetic (half range), as
RakNet's `IsOlderOrderedPacket` does (`§3.4`).

**The hold list.** Each channel keeps one list sorted by (ordering, sequencing),
with sequenced records before the ordered record of the same epoch. This is
RakNet's heap order (`§6.2`, I6), so sequenced records that arrive ahead of their
epoch's ordered message are held and released before it. Insertion walks the
channel's list. Across the connection, the list length is bounded by
`window_size + held_sequenced`. Release walks from the head while records match.
Held block messages stay in their block buffer and are released the same way.

### 7.4 UNRELIABLE_SEQUENCED records ahead of their epoch

These records carry no number, so the window does not account for them. They
use their own pool, `held_sequenced` buffers per connection:

- A newer record for the same channel and epoch replaces the held one, and the
  older one is dropped. Sequenced semantics allow this.
- A full pool drops the new record and counts `HELD_SEQUENCED_FULL`.

Both drops are legal: the record is unreliable, and no ack depends on it. The
pool is in the allocator layout from the first version, even though code 1 is
not enabled until later, so enabling it changes no layout.

### 7.5 The receipt variants: send side only

Codes 5, 6 and 7 at the API become wire codes 0, 2 and 3 (I1, `§2`). Every send
descriptor carries a `serial` (u32) from the first version, and every send returns
one (I9):

- **RELIABLE_WITH_ACK_RECEIPT and RELIABLE_ORDERED_WITH_ACK_RECEIPT.** ACKED when
  **every** fragment is acked. RakNet fires on the last-index fragment
  (`§7.3`, `§11` item 5), and this design fixes that. There is no LOSS. The
  connection gives up instead (6.5).
- **UNRELIABLE_WITH_ACK_RECEIPT.** The packet that carries it gets a receipt entry
  `{serial, deadline = send time + RTO}` (I10). ACKED when that `packet_id` is
  acked. LOSS when the deadline passes, swept in `end_update`, or when the packet's
  record is evicted or reported dropped. Above `ROCKETNET_MESSAGE_PACKET_BYTES`
  the transport's fragment path cannot prove delivery (P3), so the send is
  upgraded to RELIABLE_WITH_ACK_RECEIPT, as RakNet upgrades splits (`§5.2` step 4).
- A receipt means the peer's message layer received the record. It does not mean
  the application read it (`§7.3`).

### 7.6 The first two types, and what "enabled" means

The first version enables wire codes **0, 3 and 5** in the receiver's dispatch,
and API reliabilities **0 and 3** at send. Code 5 carries CONFIG (section 10.4).
It also means the unordered reliable path runs on every connection from day
one, which is CLAUDE.md's "no rare codepaths". A send with any other reliability
returns 0 and counts `SEND_TYPE_NOT_ENABLED`. A record with codes 1, 2 or 4
raises `VIOLATION_TYPE`.

The enabled set is one compile-time bitmask, `ROCKETNET_MESSAGE_ENABLED_TYPES`.
Section 17 shows that turning on each bit adds a dispatch case and nothing else.

### 7.7 Checked by

- TLA+ `RNOrderedChannel`: one channel carrying ordered, unreliable-sequenced and
  reliable-sequenced records over a reordering, duplicating link. Invariants:
  ordered records are delivered in index order with no gaps; no sequenced record
  is delivered at or below the current watermark; and the watermark rule holds
  on both paths. Its reversed witnesses are RakNet's item 18 (a duplicate is
  delivered) and item 19 (a stale watermark drops a fresh record).
- Unit tests per row of the 7.3 table, including holds released across an
  update boundary, and 10,000 channels with a record on the last one.
- Soak counters that must stay zero on an honest link: `STALE_ORDERED`,
  `VIOLATION_FIELD`.

---

## 8. Layer 5: blocks

A **block** is a reliable message larger than `ROCKETNET_MESSAGE_PACKET_BYTES`
(1176). This is yojimbo's block message, and RakNet's split (`§8`) with its two
defects fixed.

**Where the split happens (I11, decided).** In this layer, per fragment, and
never on the transport's fragment path. The transport acknowledges a fragmented
payload's id even when the receiver refused or abandoned the payload (P3), so a
reliable message resent whole on a missing id would be delivered falsely on a
false id. And per-fragment resend costs only the lost part, which the study
weighs in I11.

**Split.**
- The stride is fixed: `ROCKETNET_MESSAGE_BLOCK_STRIDE = 1152` bytes. That is
  `1192 - 18` (the largest header, section 5) = 1174, rounded down to a multiple
  of 128. A fragment record is therefore always at most 1170 bytes, and the
  remaining 22 or more bytes of its packet can carry a small record.
- `count = ceil( bytes / 1152 )`, which is at least 2. Fragment `i` holds bytes
  `[i*1152, min((i+1)*1152, bytes))`.
- Every fragment carries the block's code, channel and indices, plus
  `frag index` and `frag count` (u16 each).
- When the block's first fragment is first written, the block takes `count`
  **consecutive** numbers. That needs `count` free numbers in the window and one
  free block in `blocks_in_window`. All `count` window entries are placed at the
  head of the resend list, marked due now, so the following packets carry them
  "resends first" (section 9.3). Each fragment is then acked and resent on its
  own.

**Reassemble.**
- The block's identity on the wire is its first number, `number - frag index`
  (mod 2^24). No block id field is needed, and a reused id cannot match a stale
  reassembly, because numbers do not repeat inside the window (RakNet `§8.2`,
  `§11` item 17).
- The first fragment to arrive, of any index, takes a hold BLOCK buffer and a
  block slot `{first, count, code, channel, ordering, sequencing, received
  bitmap, received count, last fragment bytes}`. No start packet is needed, and
  none exists (R3).
- Each fragment is placed at `index * 1152`. Placement is by index, never by
  arrival (RakNet's `§8.2` bug, I11).
- A fragment is refused with `VIOLATION_BLOCK` when:
  - `count < 2`;
  - `count > ceil( max_block_bytes / 1152 )`;
  - `index >= count`;
  - a non-final fragment's data is not exactly 1152 bytes;
  - a final fragment's data is not between 1 and 1152 bytes;
  - any field disagrees with the slot.
- A duplicate fragment never gets this far, because the window dropped it.
- When the received count reaches `count`, the block is one message of
  `(count-1)*1152 + last` bytes in its block buffer. It goes through the 7.3
  dispatch like any record. If it is in order, it is delivered with its pointer
  into the block buffer, valid until the next update. Otherwise it is held in
  place.

**Limits.**
- `max_block_bytes` is configuration: default 16,384, at most 16 MiB.
  Validation requires `ceil( max_block_bytes / 1152 ) <= window_size / 2`, so a
  block can always be numbered once the window drains.
- A send larger than `max_block_bytes` returns 0 and counts
  `SEND_TOO_LARGE`.

**Large unreliable messages.**
- **UNRELIABLE (and later UNRELIABLE_SEQUENCED) above 1176 bytes.** These go the
  transport's fragment path, as a one-record payload of up to the transport's
  configured `max_payload_bytes`. They keep unreliable semantics: a lost fragment
  loses the message (`sdk/rocketnet.h:423-430`). This departs from RakNet, which
  upgrades the split to reliable (`§5.2` step 4). An unreliable message that
  silently becomes reliable would take window and hold capacity the game never
  asked for.
- The length field is u16, so this path carries at most 65,535 bytes of
  message, and less if the transport's `max_payload_bytes` is smaller (less the
  3-byte trailer). A larger unreliable send returns 0 and counts
  `SEND_TOO_LARGE`. The transport could carry up to 305,408 bytes
  (`sdk/rocketnet_fragment.h:40-43`), but an unreliable message that large dies
  to loss almost surely (`S:1892-1898`), and a game that needs one should send it
  reliable.
- `start_message` with a request above 1176 takes the arena frame (one copy at
  finish, inside the transport).
- `send_message` queues the message in a send BLOCK buffer, and `write_packets`
  writes it to an arena frame. Arena slots are 16 per transport per fill phase
  (`sdk/rocketnet_constants.h:76`), so on `SEND_ARENA_FULL` the message stays
  queued until the next write or its expiry.

**Checked by.**
- TLA+ `RNBlocks`: one block of 3 fragments over the lossy link. Invariants:
  delivered bytes equal sent bytes; delivery happens once; the block is counted
  once in the window. Its reversed witnesses are arrival-order concatenation
  (RakNet `§8.2`) and receipt-on-last-fragment (`§7.3`).
- Unit tests: every refusal; fragments arriving in reverse; a block exactly 1152
  and 1153 bytes long; a block at `max_block_bytes`; the wrap of the first number
  across 2^24.

---

## 9. Layer 6: the scheduler

### 9.1 What it owns

- Four priority queues: IMMEDIATE, HIGH, MEDIUM and LOW, with RakNet's numbering
  (`§2`). Each queue is a FIFO of **candidates**. A candidate is either an
  unordered message (codes 0, 2, 5, and the unreliable sequenced and receipt
  variants later) or a **channel head** (the oldest unnumbered reliable message of
  a channel, codes 3, 4 and 7).
- A virtual time per priority, with RakNet's strides 1, 5, 14 and 35
  (`§5.3`).
- The packet budget per write (`packet_budget`, default 8 packets per connection
  per `write_packets`).
- The expiry clock for queued unreliable messages (`unreliable_timeout`, default
  250 ms; RakNet's cull, `§5.4` step 2, which is off by default there).

### 9.2 Priority and channels

A reliable ordered or sequenced message goes to the tail of its channel's pending
FIFO. Only the channel's head is a candidate. That is how first-write order within
a channel (6.2) holds without any check at write time.

**Priority is per send, and a channel inherits the highest priority waiting in
it.** When a message is queued behind a head of lower priority, the channel
moves to the higher priority's queue, carrying its head. The channel keeps that
priority until its pending FIFO empties. On a strictly ordered channel, writing
a later message first gains nothing (it would be held at the receiver). Lifting
the head is the only way a priority can matter there, and it is what a caller
who sent HIGH means.

### 9.3 Writing one packet

`write_packet( endpoint, buffer, capacity, packet_id, time )`, called by the object
for each packet it decides to send (section 11):

1. **Due resends first** (RakNet `§5.4`): from the head of the resend list, write
   each entry whose time has come, oldest first, while it fits. That includes the
   unsent fragments of newly numbered blocks.
2. **New messages by priority.** Pick the non-empty priority with the smallest
   virtual time (ties go to the higher priority), take its head candidate, and
   advance that virtual time by the priority's stride. A priority that becomes
   non-empty is pulled forward to the current minimum. When every queue is empty,
   all four are reseeded (`§5.3`).
   - An unreliable candidate is written and its send buffer released.
   - A reliable candidate is numbered (6.2) if the window allows it (6.3), then
     written. If the window does not allow it, **reliable candidates are skipped
     for the rest of this write** and unreliable ones still go (I13). The window
     never blocks unreliable traffic.
3. Stop when the next candidate does not fit the remaining space, or 64 records
   are written. The candidate stays first in line.
4. Record the packet (6.4).

`has_output( time )` tells the object whether `write_packet` would write anything.
The object calls it before `start_packet`, so it never spends a sequence for
nothing (P11).

`write_packets` calls `write_packet` while `has_output` is true and the budget is
not spent. Queued unreliable messages older than `unreliable_timeout` are dropped
in `end_update` and counted (`UNRELIABLE_EXPIRED`).

**IMMEDIATE.** In RakNet, IMMEDIATE only wakes the network thread (`§5.3`). Here
nothing waits for a timer, because every `write_packets` writes, so IMMEDIATE is
simply the heaviest weight. That matches the code RakNet ships, not its comments.

### 9.4 Checked by

- TLA+ `RNScheduler`, with three properties:
  - no starvation: every queued message is eventually written while the budget is
    at least 1 and acks keep arriving;
  - first-write order within a channel;
  - unreliable messages are never blocked by a full window.
  Its reversed witnesses: priority without channel inheritance (an earlier LOW
  message starves behind later HIGH ones on the same channel), and a single queue
  where a blocked reliable head stalls the unreliable messages behind it.
- Unit tests:
  - stride ratios under backlog, measured over 1000 picks;
  - the packet budget;
  - expiry;
  - `has_output` exactly mirrors `write_packet`.

---

## 10. Layer 7: the endpoint and the slots, as pure machines

### 10.1 The endpoint

`struct rocketnet_message_endpoint_t` is one connection's whole message state.
It composes layers 1 to 6. All its memory is the block handed to
`endpoint_init( endpoint, memory, bytes, config )`. The size of that block comes
from `endpoint_memory_required( config )` (section 14). The endpoint never
allocates, never reads a clock, and never touches a socket.

**Events in:**

| Event | Function |
|---|---|
| a new update begins | `endpoint_begin_update( e, time )`: releases the previous update's delivered hold and block buffers, clears the delivered, receipt and action lists |
| a payload arrived | `endpoint_process_payload( e, data, bytes )`: the data must stay valid until the next `begin_update`, which the transport batch guarantees (`U:68-81`) |
| a packet was acked | `endpoint_process_acked( e, packet_id )` |
| the application sends | `endpoint_send( e, data, bytes, reliability, priority, channel, serial )` (copying), `endpoint_single_trailer( e, ... )` (the SINGLE form) |
| a written packet was dropped | `endpoint_packet_dropped( e, packet_id )` |
| the update ends | `endpoint_end_update( e, time )`: receipt deadlines, unreliable expiry, reliable timeout |
| a frame is ready | `endpoint_has_output( e, time )`, `endpoint_write_packet( e, buffer, capacity, packet_id, time )` |
| the connection starts | `endpoint_reset( e )` |

**Actions out** (lists inside the struct, read by the binder after each call):

- **delivered**: `{data pointer, bytes, channel, reliability}`. This is valid
  until the next `begin_update`.
- **receipts**: `{serial, ACKED | LOSS}`.
- **disconnect**: a flag and a reason. The binder disconnects the connection
  through the transport, on the main thread.
- **bytes written**: the return value of `write_packet`. The binder seals them.

There is no timer action. The binder calls `end_update` every update with the
time it was given, and every deadline is compared there. That is the transport's
own model: "the library never reads a clock of its own" (`sdk/rocketnet.h:542-544`).

### 10.2 The endpoint's update order

The binder calls these in order. A test calls them the same way.

```
begin_update( time )
process_payload( ... )        for each payload in the transport batch for this connection, in batch order
process_acked( packet_id )    for each acked id for this connection
end_update( time )
-- the application reads delivered and receipts, and sends --
has_output / write_packet     during the fill phase
```

### 10.3 Reset

`endpoint_reset` returns every pool buffer and zeroes the counters of layers 3
to 6. It clears all channels (an O(`num_channels`) memset, once per connection,
which R6 allows because it happens at connect, not per operation). After reset,
numbering and indices start at 0. A reset discards queued and in-flight messages.
It fires no receipts, because the connection those receipts belonged to is gone,
and the binder reports that with a DISCONNECTED event.

### 10.4 CONFIG: the one thing the layer announces

Both ends must be built and configured with the same **wire configuration**:
`num_channels`, `window_size`, `hold_small`, `hold_medium`, `hold_packet`,
`hold_blocks`, `max_block_bytes`. The sender uses its own `hold_*` and
`window_size` as the receiver's capacities (6.3). This is yojimbo's rule.

**Mismatch is detected, never undefined.** Reset queues a code 5 CONFIG record
(section 13.4) at the head of the IMMEDIATE queue, so it is the first reliable
record each endpoint writes, and takes number 0.
The receiver compares it with its own wire configuration and, on any difference,
raises DISCONNECT with `CONFIG_MISMATCH`. Before CONFIG arrives, the receiver's
own checks (6.6, 7.3, 8) still bound everything, so a mismatched peer meets a
violation or a mismatch. It never meets a hang or an overflow.

CONFIG is reliable, so it is resent until acked (R3). Neither side waits for it.
A design that negotiated capacities would have to wait for the peer's numbers
before sending reliable records, which is exactly the transition R3 is wary of.
Equality, verified, needs no waiting. The alternative stays open (section 19.3).

### 10.5 The slots machine (server)

`struct rocketnet_message_slots_t`, pure, one entry per slot:
`{connected u8, generation u64}`.

- **Event:** `slots_observe( s, client_index, connected, generation )`, once per slot
  per update, after the endpoint has processed that update's batch.
- **Actions:**
  - `CONNECTED(i)` when the slot goes from not connected to connected, or the
    generation changes while connected (after `DISCONNECTED(i)` for the old one);
  - `DISCONNECTED(i)` when the slot goes from connected to not connected;
  - `RESET(i)` with each of those, which the binder applies to endpoint `i`.

The batch is processed **before** the slot machine runs, so an update's payloads
for slot `i` reach the endpoint as it was before any reset. By P6 those payloads
all belong to the old generation. An update's delivered messages for slot `i`
therefore precede its events for slot `i`. The API states this (section 12).

### 10.6 Checked by

- TLA+ `RNMessageEndpoint`: two endpoints over two `Link.tla` directions, with the
  transport's ack semantics modelled (ack iff opened, and a toggle for the F1 lie).
  Properties:
  - **Safety**: every delivered reliable message was sent, exactly once, in
    channel order. No receive pool overflows.
  - **Liveness**: under weak fairness on delivery, every sent reliable message is
    delivered and every receipt fires.

  Its reversed witnesses are the F1 lie (liveness fails) and a receiver that
  drops on a full pool instead of the sender counting (safety fails).
- TLA+ `RNSlots`: slot reuse within one update. The invariant: a message is never
  attributed to a generation it did not come from. Its reversed witness resets the
  endpoint before processing the batch.
- A **two-endpoint harness with no sockets**, in `test.cpp`. Two endpoints and a
  `fake_transport` that applies a seeded loss, duplicate and reorder schedule, and
  acks exactly what it delivered. It runs every layer at once, at 32, 1024 and
  10,000 channels. This is the test the card asks for: "a unit test drives it end
  to end without a socket".

---

## 11. Layer 8: the client and server objects

These are the only code that binds a machine to a transport and a clock.

### 11.1 The server

`struct rocketnet_server_t`:
- one `rocketnet_server_transport_t`;
- `max_clients` endpoints, carved from one allocation at create;
- one slots machine;
- per slot, a `written` flag for the current fill phase;
- a receipt serial counter (`std::atomic<uint32_t>`, skipping 0, as RakNet does,
  `§5.1`).

**`rocketnet_server_update( server, time )`, main thread:**

1. `rocketnet_server_transport_update( transport, time )`.
2. `begin_update` on every endpoint whose previous update left delivered buffers
   or lists. A per-update dirty list keeps this O(active), not O(max_clients).
3. For each payload in the transport batch: `process_payload` on its slot's
   endpoint. If the slot is not connected, skip the payload and count it. P6
   makes this impossible, and the count proves it.
4. For each acked id: `process_acked` on its slot's endpoint.
5. `end_update` on every endpoint that was touched this update or is on the
   **timed list**: an endpoint joins it while it has queued unreliable messages,
   pending receipt deadlines or unacked reliable entries, and leaves it when it
   has none. An idle connection has no deadline to check, so the cost is
   O(active connections), not O(max_clients).
6. `slots_observe` for each slot (O(max_clients) reads, constant shape). Apply
   RESET, and append CONNECTED and DISCONNECTED events.
7. For each endpoint whose action list says DISCONNECT:
   `rocketnet_server_transport_disconnect_client`. This is main thread, outside a
   fill phase, as required (`sdk/rocketnet.h:905-912`).
8. Build the server-wide views: messages, receipts and events, as arrays across
   the endpoints touched this update, in slot order.

**`rocketnet_server_write_packets( server, client_index )`, fill phase, any
thread, one thread per slot at a time:**

```
while endpoint_has_output( e, time ) and budget left:
    frame = rocketnet_server_transport_start_packet( transport, i, ROCKETNET_MAX_PAYLOAD_BYTES, &ctx )
    if !frame: break                                  // counted by the transport
    n = endpoint_write_packet( e, frame + ctx.payload_index, ctx.payload_capacity, ctx.packet_id, time )
    if rocketnet_server_transport_finish_packet( transport, &ctx, n ) == 0:
        endpoint_packet_dropped( e, ctx.packet_id )
```

Large unreliable messages take one more step: the endpoint reports one as its
next output, the binder requests the arena size, and the endpoint writes the
record into the arena frame.

Sealing happens here, on the calling thread, so a server spreads the crypto cost
across its game threads the way the transport intends.

**`rocketnet_server_send_packets( server )`, main thread:** calls
`write_packets` for every connected slot not yet written in this fill phase
(a convenience for single-threaded games), then
`rocketnet_server_transport_send_packets`.

### 11.2 The zero-copy send, on the server

```
uint8_t * rocketnet_server_start_message( server, i, bytes, reliability, channel, &mctx )
    // reliability must be UNRELIABLE in the first version (later 1 and 5)
    frame = rocketnet_server_transport_start_packet( transport, i, bytes + 16, &mctx.packet )
    mctx.capacity = mctx.packet.payload_capacity - 16      // 1176 on the wire route
    return frame + mctx.packet.payload_index
uint32_t rocketnet_server_finish_message( server, &mctx, bytes )
    t = endpoint_single_trailer( e, payload + bytes, reliability, channel, bytes, mctx.packet.packet_id, &serial )
    rocketnet_server_transport_finish_packet( transport, &mctx.packet, bytes + t )
    return serial
void rocketnet_server_abort_message( server, &mctx )
```

`endpoint_single_trailer` touches only the channel's sequencing counter (later
types) and the sent-packet records (receipt types). It is subject to the same
one-writer-per-slot rule as `write_packets`.

### 11.3 The client

`struct rocketnet_client_t`: one `rocketnet_client_transport_t` and one endpoint.
The same update order, without slots:

- The endpoint is reset at create and never again. The client transport delivers
  and acks payloads from state 3 (P5), so the endpoint must already be live.
- A CONNECTED event fires when the transport reaches _connected_. A DISCONNECTED
  event fires when it reaches 0 or a negative state.
- Messages may be sent from create. Packets are written only once the transport is
  _connected_, because `start_packet` refuses before then
  (`sdk/rocketnet_client_protocol.cpp:1056-1060`). The client binder calls
  `has_output` only in that state, so it never spends a sequence on a refusal.
- A reconnect is a new client object, because a client transport is created with
  its token. This keeps the client machine to one connection lifetime and no
  restart path.

### 11.4 What the objects own and do not own

The objects own their transport. Raw `start_packet` and `send_packet` on that
transport would put bytes the peer parses as records, and the peer would
disconnect with a violation. So the objects expose the transport read-only (stats,
counters, driver) through `rocketnet_server_transport_of` and
`rocketnet_client_transport_of`, and the header says not to send on it.

**Checked by.**
- Functional tests on a real loopback pair (Linux bench, per BUILDING.md), with
  the transport's network simulator for loss: CONNECTED, then messages both ways
  at 32 channels, then DISCONNECTED. Slot reuse, reliable timeout, CONFIG
  mismatch.
- The swarm soak (CLAUDE.md "Verification ladder") gains message traffic, with
  must-stay-zero counters: every `VIOLATION_*`, `STALE_ORDERED`,
  `RELIABLE_TIMEOUT`, `MESSAGE_POOL_EMPTY` on the receive side, and the transport's
  `FRAGMENT_PAYLOAD_BATCH_FULL`.

---

## 12. Layer 9: the C API, as it will appear in `sdk/rocketnet.h`

This follows the header's own conventions: prose blocks that cover a surface
(`U:15-28`), one X-macro config table, `extern "C"` symbols, no C++ types
(REQUIREMENTS.md "The C API is for C to call"). It is placed after the transport
sections.

```c
// -----------------------------------------
//                                messages
// -----------------------------------------

// The message layer: a client and a server over the transports, sending
// messages -- opaque byte buffers, as payloads are -- with a reliability
// chosen per send, over num_channels ordering channels. Reliabilities and
// priorities are RakNet's, with RakNet's numbering, so a binding written
// against RakNet passes them through. This version enables UNRELIABLE and
// RELIABLE_ORDERED; a send with any other value returns 0 and counts
// SEND_TYPE_NOT_ENABLED. docs/CHANNELS.md is the design.

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                                       32
#endif // #ifndef ROCKETNET_MAX_CHANNELS

#if ROCKETNET_MAX_CHANNELS < 1 || ROCKETNET_MAX_CHANNELS > 65536
#error ROCKETNET_MAX_CHANNELS must be between 1 and 65536
#endif

#define ROCKETNET_UNRELIABLE                                          0
#define ROCKETNET_UNRELIABLE_SEQUENCED                                1
#define ROCKETNET_RELIABLE                                            2
#define ROCKETNET_RELIABLE_ORDERED                                    3
#define ROCKETNET_RELIABLE_SEQUENCED                                  4
#define ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT                         5
#define ROCKETNET_RELIABLE_WITH_ACK_RECEIPT                           6
#define ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT                   7

#define ROCKETNET_PRIORITY_IMMEDIATE                                  0
#define ROCKETNET_PRIORITY_HIGH                                       1
#define ROCKETNET_PRIORITY_MEDIUM                                     2
#define ROCKETNET_PRIORITY_LOW                                        3

#define ROCKETNET_RECEIPT_ACKED                                       1
#define ROCKETNET_RECEIPT_LOSS                                        2

#define ROCKETNET_MESSAGE_EVENT_CONNECTED                             1
#define ROCKETNET_MESSAGE_EVENT_DISCONNECTED                          2

// a packet-sized message: the most one record carries in one packet, for
// every reliability. start_message's capacity on the wire route, and the
// size above which a reliable message becomes a block and an unreliable
// one rides the transport's fragments.

#define ROCKETNET_MESSAGE_PACKET_BYTES                             1176

// The message config table, per connection: the same on both ends for
// the WIRE fields (marked W below) -- verified by the CONFIG record each
// end sends first, a mismatch disconnecting with CONFIG_MISMATCH -- and
// free per end for the rest. Same config system as the transports:
// default < struct < env, refuse and never clamp, env name ROCKETNET_
// plus the field uppercased. Times are milliseconds.

#define ROCKETNET_MESSAGE_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                  \
    INT_FIELD( num_channels,              32,     1, ROCKETNET_MAX_CHANNELS,  0 )      /* W */                        \
    INT_FIELD( window_size,              512,    64, 32768,                   0 )      /* W, power of two */          \
    INT_FIELD( hold_small,               256,     1, 65536,                   0 )      /* W, 64-byte holds */         \
    INT_FIELD( hold_medium,              128,     1, 65536,                   0 )      /* W, 256-byte holds */        \
    INT_FIELD( hold_packet,               32,     1, 65536,                   0 )      /* W, packet-sized holds */    \
    INT_FIELD( hold_blocks,                2,     1, 1024,                    0 )      /* W, block reassemblies */    \
    INT_FIELD( max_block_bytes,        16384,  1177, 16777216,                0 )      /* W */                        \
    INT_FIELD( send_small,               256,     1, 65536,                   0 )                                     \
    INT_FIELD( send_medium,              128,     1, 65536,                   0 )                                     \
    INT_FIELD( send_packet,               64,     1, 65536,                   0 )                                     \
    INT_FIELD( send_blocks,                2,     1, 1024,                    0 )                                     \
    INT_FIELD( held_sequenced,             8,     0, 4096,                    0 )                                     \
    INT_FIELD( sent_packets,             256,    16, 65536,                   0 )                                     \
    INT_FIELD( packet_entries,          4096,    64, 1048576,                 0 )                                     \
    INT_FIELD( packet_budget,              8,     1, 1024,                    0 )                                     \
    INT_FIELD( max_unreliable_delivered, 1024,    1, 1048576,                 0 )                                     \
    INT_FIELD( max_unreliable_receipts,  256,     0, 65536,                   0 )                                     \
    INT_FIELD( unreliable_timeout_ms,    250,     1, 60000,                   0 )                                     \
    INT_FIELD( reliable_timeout_ms,    10000,   100, 600000,                  0 )                                     \
    INT_FIELD( rto_min_ms,                50,     1, 10000,                   0 )                                     \
    INT_FIELD( rto_max_ms,              1000,     1, 60000,                   0 )                                     \
    INT_FIELD( rto_initial_ms,           250,     1, 60000,                   0 )

struct rocketnet_message_config_t
{
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

ROCKETNET_EXPORT_FUNC void rocketnet_message_default_config( struct rocketnet_message_config_t * config );

// the bytes one connection's message state takes at this config: what
// create spends per client slot, once. section 14 of docs/CHANNELS.md is
// the formula. zero means the config is invalid.

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_connection_memory( const struct rocketnet_message_config_t * config );

// The zero-copy send's context: the transport's packet context plus what
// finish needs. Read-only to the caller, consumed once, as the packet
// context is.

struct rocketnet_message_context_t
{
    struct rocketnet_packet_context_t packet;
    int capacity;                                   // write up to this many bytes
    int reliability;
    int channel;
};

// -----------------------------------------
//                          message server
// -----------------------------------------

struct rocketnet_server_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context, const char * public_address_string,
                                                                     const struct rocketnet_server_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     double time );

ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server );        // stop and wait for stopped first

ROCKETNET_EXPORT_FUNC void rocketnet_server_stop( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC int rocketnet_server_state( struct rocketnet_server_t * server );

// the transport underneath, for its stats, counters, port and driver. do
// not send on it: every payload on a message server's connections is
// parsed as message records, and anything else disconnects the peer.

ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t * rocketnet_server_transport_of( struct rocketnet_server_t * server );

// The cycle is the transport's: update, drain, fill, send_packets -- the
// same threads and the same phases. update runs the transport's update,
// then hands every payload and every acked packet to its connection's
// message state, then publishes what arrived: messages, receipts and
// connection events, all valid until the next update. Drain every update;
// nothing is kept for a later one.

ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server, double time );

// Messages delivered by the last update, by index or as parallel arrays.
// data points into storage the library owns until the next update: the
// transport's batch for a message that arrived in order, a hold buffer
// for one released from waiting, a block buffer for a block -- copy what
// you keep. Order: by slot, then as delivered; a slot's messages in an
// update come from the connection that held the slot BEFORE that update's
// events for it. channel is the send's channel for ordered and sequenced
// reliabilities and 0 otherwise, which is RakNet's wire.

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_messages( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC const uint8_t * rocketnet_server_message( struct rocketnet_server_t * server, int message_index,
                                                          int * client_index, int * message_bytes, int * channel, int * reliability );

ROCKETNET_EXPORT_FUNC int rocketnet_server_messages( struct rocketnet_server_t * server, const int ** client_index,
                                               const uint8_t * const ** message_data, const int ** message_bytes,
                                               const int ** channel, const int ** reliability );

// Receipts fired by the last update: the serial a send returned and
// ROCKETNET_RECEIPT_ACKED or ROCKETNET_RECEIPT_LOSS. Only the receipt
// reliabilities fire them; LOSS only for UNRELIABLE_WITH_ACK_RECEIPT.
// ACKED means the peer's message layer received every byte of the message,
// not that its application read it. Empty in this version.

ROCKETNET_EXPORT_FUNC int rocketnet_server_receipts( struct rocketnet_server_t * server, const int ** client_index,
                                               const uint32_t ** serial, const int ** result );

// Connection events from the last update: CONNECTED when a slot admits a
// connection, DISCONNECTED when it loses one, both in one update when a
// slot changes hands. A slot's events follow its messages in the same
// update.

ROCKETNET_EXPORT_FUNC int rocketnet_server_events( struct rocketnet_server_t * server, const int ** client_index,
                                             const int ** event, const uint64_t ** client_id );

// Sending. send_message copies the message into the connection's pools and
// queues it; it returns the send's serial (non-zero), or 0 when refused --
// reliability not enabled, channel at or above num_channels for an ordered
// or sequenced reliability, size above max_block_bytes, a pool or the
// connection's descriptors full, slot not connected -- each refusal on its
// counter. Nothing queued is dropped silently: an unreliable message not
// written within unreliable_timeout is dropped and counted
// UNRELIABLE_EXPIRED. Priority orders the queue; a channel takes the
// highest priority waiting in it.
//
// start_message / finish_message is the zero-copy send for one unreliable
// message: the frame is the transport's, the message is written where it
// will be sealed, and finish appends the record trailer and seals it as its
// own packet. Up to ROCKETNET_MESSAGE_PACKET_BYTES rides one wire packet
// with no copy; larger, up to the transport's max_payload_bytes, rides the
// transport's fragments with its one copy and unreliable semantics.
//
// write_packets builds and seals this slot's packets -- resends first, then
// queued messages by priority, small messages aggregated -- up to
// packet_budget packets. send_packets writes every connected slot not
// written in this fill phase, then flushes. Threading, the transport's:
// update, send_packets, stop and destroy on the main thread; send_message,
// start_message, finish_message, abort_message and write_packets on any
// game thread strictly between update and send_packets, AT MOST ONE THREAD
// PER CLIENT SLOT AT A TIME, any number of slots in parallel. send_message
// is also valid on the main thread outside a fill phase.

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_server_send_message( struct rocketnet_server_t * server, int client_index,
                                                        const uint8_t * message_data, int message_bytes,
                                                        int reliability, int priority, int channel );

ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, int client_index,
                                                          int request_bytes, int reliability, int channel,
                                                          struct rocketnet_message_context_t * context );

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server,
                                                          struct rocketnet_message_context_t * context, int message_bytes );

ROCKETNET_EXPORT_FUNC void rocketnet_server_abort_message( struct rocketnet_server_t * server,
                                                     struct rocketnet_message_context_t * context );

ROCKETNET_EXPORT_FUNC void rocketnet_server_write_packets( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC void rocketnet_server_send_packets( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

// Per-connection message state, for a game's flow control: what is
// queued, what is in flight, the layer's own rtt estimate and rto, and the
// window's room.

struct rocketnet_message_stats_t
{
    int queued_reliable;                            // sent, not yet numbered
    int queued_unreliable;                          // sent, not yet written
    int in_flight;                                  // numbered, not yet acked
    int window_free;                                // numbers the window can still assign
    float rtt;                                      // this layer's smoothed rtt (milliseconds)
    float rto;                                      // the resend timeout in force (milliseconds)
};

ROCKETNET_EXPORT_FUNC void rocketnet_server_message_stats( struct rocketnet_server_t * server, int client_index,
                                                     struct rocketnet_message_stats_t * stats );

// The message counters, server-wide (summed across slots) and per slot;
// introspected like the transport's, by index and name.

struct rocketnet_message_counters_t;

ROCKETNET_EXPORT_FUNC const struct rocketnet_message_counters_t * rocketnet_server_message_counters( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC const struct rocketnet_message_counters_t * rocketnet_server_client_message_counters( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC int rocketnet_message_num_counters( void );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_counter_value( const struct rocketnet_message_counters_t * counters, int counter );

ROCKETNET_EXPORT_FUNC const char * rocketnet_message_counter_name( int counter );

// -----------------------------------------
//                          message client
// -----------------------------------------

struct rocketnet_client_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token,
                                                                     const struct rocketnet_client_transport_config_t * transport_config,
                                                                     const struct rocketnet_message_config_t * message_config,
                                                                     double time );

ROCKETNET_EXPORT_FUNC void rocketnet_client_destroy( struct rocketnet_client_t * client );        // disconnect and pump a few updates first

ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t * rocketnet_client_transport_of( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC void rocketnet_client_update( struct rocketnet_client_t * client, double time );

ROCKETNET_EXPORT_FUNC int rocketnet_client_state( struct rocketnet_client_t * client );           // the transport's state

ROCKETNET_EXPORT_FUNC int rocketnet_client_num_messages( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC const uint8_t * rocketnet_client_message( struct rocketnet_client_t * client, int message_index,
                                                          int * message_bytes, int * channel, int * reliability );

ROCKETNET_EXPORT_FUNC int rocketnet_client_messages( struct rocketnet_client_t * client, const uint8_t * const ** message_data,
                                               const int ** message_bytes, const int ** channel, const int ** reliability );

// the copying cursor, as the transport's: pops the next message into a
// buffer you own -- the one form that outlives the next update -- and
// refuses a message larger than the buffer whole, counting
// RECEIVE_CURSOR_TOO_SMALL. size by max_block_bytes, or by the transport's
// max_payload_bytes if larger, to never refuse.

ROCKETNET_EXPORT_FUNC int rocketnet_client_receive_message( struct rocketnet_client_t * client, uint8_t * message_data, int message_capacity,
                                                      int * channel, int * reliability );

ROCKETNET_EXPORT_FUNC int rocketnet_client_receipts( struct rocketnet_client_t * client, const uint32_t ** serial, const int ** result );

ROCKETNET_EXPORT_FUNC int rocketnet_client_events( struct rocketnet_client_t * client, const int ** event );

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_client_send_message( struct rocketnet_client_t * client, const uint8_t * message_data, int message_bytes,
                                                        int reliability, int priority, int channel );

ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int request_bytes, int reliability, int channel,
                                                          struct rocketnet_message_context_t * context );

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context, int message_bytes );

ROCKETNET_EXPORT_FUNC void rocketnet_client_abort_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context );

ROCKETNET_EXPORT_FUNC void rocketnet_client_send_packets( struct rocketnet_client_t * client );   // writes, then flushes

ROCKETNET_EXPORT_FUNC void rocketnet_client_disconnect( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC void rocketnet_client_message_stats( struct rocketnet_client_t * client, struct rocketnet_message_stats_t * stats );

ROCKETNET_EXPORT_FUNC const struct rocketnet_message_counters_t * rocketnet_client_message_counters( struct rocketnet_client_t * client );
```

Notes on the surface:

- **The C# shim** (`§13`, provisional). `send_message` is RakNet's `Send`: it
  returns a non-zero serial, reliability and priority are passed through by
  value, and the channel is an int that is refused at or above `num_channels`.
  RakNet silently maps out-of-range channels to 0. The shim decides what to do
  with a refusal. RakNet's `Receive` returns one packet at a time, receipts and
  notices included, with a message id in the first byte. The shim builds that
  from the three arrays: messages, receipts and events. This layer does not
  reserve the first byte. `DeallocatePacket` is a no-op over the per-update
  lifetime, or the shim copies out. `broadcast` is a loop in the shim. Flows and
  broadcast proper (REQUIREMENTS.md) are a later layer, and nothing here blocks
  them.
- **The ABI.** The two new public structs (`rocketnet_message_config_t` and
  `rocketnet_message_context_t`, plus `rocketnet_message_stats_t`) join the
  ABI introspection (`sdk/rocketnet.h:233-253`). `ROCKETNET_ABI_VERSION` does
  not change, because no existing layout changes.
- **Counters**, by layer: `SEND_*` refusals, `UNRELIABLE_EXPIRED`, `RESENDS`,
  `DUPLICATE`, `STALE_ORDERED`, `STALE_SEQUENCED`, `HELD_SEQUENCED_FULL`,
  `UNRELIABLE_DELIVERED_FULL`, `SENT_RECORD_EVICTED`, `ACKED_UNKNOWN`,
  `MESSAGE_POOL_EMPTY` by class and side, one `VIOLATION_*` per check (sections
  5, 6, 7 and 8), `CONFIG_MISMATCH`, `RELIABLE_TIMEOUT`, `PACKETS_WRITTEN`,
  `PACKETS_DROPPED`, `RECEIPTS_ACKED`, `RECEIPTS_LOST`, and `MESSAGES_SENT` and
  `MESSAGES_DELIVERED` by reliability.

---

## 13. The wire, to the bit, ready for STANDARD.md

Written as a standard section would be, with MUSTs, so it can be moved into
STANDARD.md, or into a companion standard for the tier above the transport,
without a rewrite. STANDARD.md is not edited by this card. Its scope line says
messages are not specified there (`S:7-10`, `S:69-71`), and moving that line is
Glenn's ruling (section 19.3).

### 13.1 Where records live

A **message payload** is the application data of a type 5 payload packet, or a
payload reassembled from type 9 fragments, on a connection whose both ends run
the message layer. It is a sequence of 1 to 64 **records** laid end to end with
no padding. Its length is the payload length the transport reports. A message
payload MUST NOT be empty.

### 13.2 The record

All multi-byte integers are little-endian, as everywhere in the standard
(`S:216-268`). A `u24` is the low three bytes of the value, low byte first.

Fields in forward byte order. A field appears only where its condition holds:

| # | Field | Size | Present when |
|---|---|---|---|
| 1 | data | L bytes | always; L >= 1 |
| 2 | message number | u24 | code in {2, 3, 4, 5} |
| 3 | ordering index | u24 | code in {1, 3, 4} |
| 4 | sequencing index | u24 | code in {1, 4} |
| 5 | channel | u8, or u16 when WIDE | code in {1, 3, 4} |
| 6 | fragment index | u16 | BLOCK |
| 7 | fragment count | u16 | BLOCK |
| 8 | length L | u8, or u16 when LONG | always |
| 9 | flags | u8 | always; the record's last byte |

The flags byte:

```
bit   7   6   5     4     3      2   1   0
    +---+---+-----+-----+------+-----------+
    | 0 | 0 | WIDE| LONG| BLOCK|   code    |
    +---+---+-----+-----+------+-----------+
```

| Code | Meaning |
|---|---|
| 0 | unreliable |
| 1 | unreliable sequenced |
| 2 | reliable |
| 3 | reliable ordered |
| 4 | reliable sequenced |
| 5 | reliable control: the layer's own, never delivered to the application |
| 6, 7 | reserved |

Codes 0 to 4 equal RakNet's wire codes (`§2`). The three receipt reliabilities
are sent as 0, 2 and 3. They never appear on the wire.

**Writer rules.**
- A writer MUST set LONG if and only if L >= 256.
- A writer MUST set WIDE if and only if the channel is at least 256.
- A writer MUST set BLOCK only with codes 2, 3 or 4.
- A writer MUST NOT set bits 6 and 7.
- These rules make the encoding canonical.

**Header sizes**, in bytes (fields 2 to 9), before LONG (+1), WIDE (+1) and
BLOCK (+4):

| Code | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| Header bytes | 2 | 9 | 5 | 9 | 12 | 5 |

The largest header is 18 bytes, which is why a packet-sized message is
`1192 - 16 = 1176` bytes for codes 0 to 3 and 5. Code 4 with LONG and WIDE
needs 14 bytes, so it too fits in the 16 reserved. The block stride is 1152
(`<= 1192 - 18`).

### 13.3 Parsing, and the receiver's checks in order

The receiver parses backward from the payload's last byte. At each record end
`e` (initially the payload length) it applies the checks below, in order. A check
that fails refuses the **whole** payload with that check's counter, applies none
of its records, and disconnects the connection.

1. `flags` = byte `e-1`. Bits 6 or 7 set: `VIOLATION_RESERVED`. Code 6 or 7:
   `VIOLATION_CODE`. A code this receiver does not enable: `VIOLATION_TYPE`.
2. BLOCK with a code other than 2, 3 or 4: `VIOLATION_FIELD`.
3. L is read before `flags`, as u16 if LONG or u8 if not. L = 0, or LONG with
   L < 256: `VIOLATION_LENGTH`.
4. The present fields of 13.2 are read backward, in the reverse of their forward
   order. Running past the start of the payload: `VIOLATION_TRUNCATED`.
5. The channel (if present) must be below the connection's `num_channels`, and
   WIDE requires channel >= 256: otherwise `VIOLATION_FIELD`.
6. For BLOCK, all must hold, otherwise `VIOLATION_BLOCK`:
   - count >= 2;
   - count <= `ceil(max_block_bytes / 1152)`;
   - index < count;
   - L = 1152 when index < count - 1;
   - 1 <= L <= 1152 when index = count - 1.
7. The data occupies the L bytes immediately before the first present field. If
   fewer than L bytes remain: `VIOLATION_TRUNCATED`. The next record ends at the
   data's first byte.
8. Parsing ends exactly at offset 0. More than 64 records: `VIOLATION_RECORDS`.

The records then take effect in forward order: the reliable window check
(section 6.6) for codes 2 to 5, then dispatch (section 7.3), then block placement
(section 8). The window, hold and block checks may also refuse:
- `VIOLATION_WINDOW`: number at or above `receive_base + window_size`;
- `VIOLATION_CAPACITY`: a hold or block pool exhausted;
- `VIOLATION_BLOCK`: a fragment that disagrees with its block's slot.

Such a refusal disconnects. The records before it in the payload have already
taken effect, which is harmless, because the connection ends. Duplicates and
stale records are dropped and counted. They are not violations.

### 13.4 The CONFIG control record

The data of a code 5 record begins with a control type byte. Type 1 is CONFIG,
20 bytes:

| Offset | Field | Size |
|---|---|---|
| 0 | control type = 1 | u8 |
| 1 | layer version = 1 | u8 |
| 2 | num_channels | u32 |
| 6 | window_size | u16 |
| 8 | hold_small | u16 |
| 10 | hold_medium | u16 |
| 12 | hold_packet | u16 |
| 14 | hold_blocks | u16 |
| 16 | max_block_bytes | u32 |

Rules:
- After reset, each end MUST send CONFIG as its first reliable record, with
  message number 0.
- A receiver MUST compare CONFIG with its own values, and disconnect with
  `CONFIG_MISMATCH` on any difference or an unknown layer version.
- An unknown control type is `VIOLATION_CONTROL`.
- Control types 2 to 255 are reserved for later layer messages. Each would be a
  reliable record, so the transition rule holds for them too.

### 13.5 Numbers and comparison

Message numbers, ordering indices and sequencing indices are 24-bit and wrap.
"Newer" means `(a - b) mod 2^24` lies in `[1, 2^23)`. Section 6.6 bounds how late
a duplicate can arrive, which is why 24 bits is enough.

### 13.6 Known-answer vectors

**V1. Two records aggregated.** UNRELIABLE `"hi"`, then RELIABLE_ORDERED `"ok"`
with number 7, ordering 2, channel 3. 15 bytes:

```
68 69 02 00  6f 6b 07 00 00 02 00 00 03 02 03
|data|L |fl| |data|number |ordering|ch|L |fl
```

Parse from the end:
- `03`: code 3, no flags.
- L = `02`.
- channel = `03`.
- ordering = `02 00 00` = 2.
- number = `07 00 00` = 7.
- data = `6f 6b`.
- The next record ends at offset 4. `00`: code 0. L = `02`. data = `68 69`.
  Offset 0 is reached.

**V2. SINGLE form, 1000-byte UNRELIABLE.** `data[1000] | e8 03 | 10`, 1003 bytes.
The flags byte `0x10` is LONG with code 0. L = 0x03e8 = 1000.

**V3. Block fragment.** The middle fragment of a 2500-byte RELIABLE_ORDERED block:
channel 0, ordering 5, numbers 20 to 22, index 1. 1166 bytes:

```
data[1152] | 15 00 00 | 05 00 00 | 00 | 01 00 | 03 00 | 80 04 | 1b
           | number 21| ordering 5|ch 0|index 1|count 3| L=1152| code 3 | BLOCK | LONG
```

The block's first number is 21 - 1 = 20. The final fragment (index 2, number 22)
carries `2500 - 2*1152 = 196` bytes, with L = `c4` and flags `0x0b` (not LONG,
since 196 < 256).

**V4. CONFIG at the defaults.** Number 0, 20 bytes of data, 25 bytes in all:

```
01 01 20 00 00 00 00 02 00 01 80 00 20 00 02 00 00 40 00 00 | 00 00 00 | 14 | 05
```

**V5. WIDE.** RELIABLE_ORDERED `"x"` on channel 300, number 1, ordering 0:

```
78 | 01 00 00 | 00 00 00 | 2c 01 | 01 | 23
```

The flags byte `0x23` is WIDE with code 3.

The layer 2 card turns these into a test vector header beside
`sdk/rocketnet_test_vectors.h`. Each vector is written by the writer and compared
with memcmp, and parsed back to its fields.

---

## 14. Sizing: memory from the connection configuration

Memory per connection, spent once at create (`rocketnet_message_connection_memory`).
The struct sizes are working figures. The layer 1 card replaces them with
`sizeof` values and asserts this formula against the allocation.

```
classbytes( s, m, p, b ) = 64*s + 256*m + 1184*p + B*b          B = roundup( max_block_bytes, 64 )
F = ceil( max_block_bytes / 1152 )                               fragments per block, at most
D = send_small + send_medium + send_packet + send_blocks          send descriptors

per connection =
      classbytes( send_small, send_medium, send_packet, send_blocks )        send pools
    + 48 * D                                                                 send descriptors
    + 32 * window_size                                                       window entries
    + 32 * sent_packets + 8 * packet_entries + 16 * 2 * sent_packets         sent-packet records, entries, packet_id hash
    + classbytes( hold_small, hold_medium, hold_packet, hold_blocks )        hold pools
    + ( 32 + roundup( ceil( F / 8 ), 32 ) ) * hold_blocks                    block slots
    + 1184 * held_sequenced                                                  held-sequenced pool
    + 32 * ( window_size + held_sequenced )                                  hold records
    + window_size / 8                                                        receive bitmap
    + 24 * ( window_size + max_unreliable_delivered )                        delivered list
    + 8 * ( window_size + max_unreliable_receipts )                          receipt list
    + 32 * num_channels                                                      channels
    + 1024                                                                   the endpoint's fixed fields
```

**At the defaults** (section 12's table):

| Item | Bytes |
|---|---|
| send pools: 256x64 + 128x256 + 64x1184 + 2x16384 | 157,696 |
| send descriptors: 450 x 48 | 21,600 |
| window entries: 512 x 32 | 16,384 |
| sent-packet records, entries, hash: 8,192 + 32,768 + 8,192 | 49,152 |
| hold pools: 256x64 + 128x256 + 32x1184 + 2x16384 | 119,808 |
| block slots: 2 x 64 | 128 |
| held-sequenced pool: 8 x 1184 | 9,472 |
| hold records: 520 x 32 | 16,640 |
| receive bitmap | 64 |
| delivered list: 1536 x 24 | 36,864 |
| receipt list: 768 x 8 | 6,144 |
| channels: 32 x 32 | 1,024 |
| fixed | 1,024 |
| **per connection** | **436,000** |

So the defaults cost 436,000 bytes per connection: about 426 KiB on the client,
and about 416 MiB for a 1000-slot server, spent at create. Nothing else is
server-wide. The slots machine is 16 bytes per slot, and the server's
messages, receipts and events views are pointers into the per-connection lists.

**How it scales.**
- **Channels.** 32 bytes each. 1024 channels add 31,744 bytes per connection.
  10,000 channels add 318,976. Nothing else changes (R6).
- **Window.** `window_size` costs about 96 bytes per number: window entry (32),
  hold record (32), delivered entry (24), receipt entry (8) and a bitmap bit. The hold pools are what the
  window actually admits.
- **Big blocks.** `max_block_bytes` x (`send_blocks` + `hold_blocks`). At 1 MiB
  with the default 2 + 2, that is 4 MiB per connection, which a 1000-slot server
  should not run with. A game that sends large blocks to few clients sizes the
  pools on the client side, and gives the server 1 + 1.
- **What the counts trade.** Each `hold_*` count is a receiver memory cost, and,
  through 6.3, the sender's in-flight ceiling for that class. With
  `hold_packet = 32`, at most 32 packet-sized reliable messages (about 37 KB) are
  outstanding per connection. At a 100 ms RTT that is about 370 KB/s of
  packet-sized reliable throughput per connection. Small and medium messages have
  their own budgets.

---

## 15. The TLA+ modules

Per the standing practice, each module is written beside the layer it models,
before or with the code. TLC runs on a bench machine on a small instance, and
every run is a row in `tla/RUNS.tsv`. Each module drives its peers over
`tla/Link.tla` (loss, duplication, reorder, delay), which is already in the tree.
Each has a **reversed witness**: the same module with one rule removed, which TLC
must answer with a counterexample. The counterexample is checked against the code
by hand before it is written down.

| Module | Layer | Checks | Small instance | Reversed witness |
|---|---|---|---|---|
| `RNPools` | 1 | `free + live = count` per class; no buffer handed out twice; reset returns all | 2 classes x 3 buffers, 8 operations | release without returning to the free list: a double-handout counterexample |
| `RNReliableWindow` | 3 | **receiver never needs more than its class counts (R5)**; every reliable message delivered exactly once; `receive_base >= U'`; liveness: every number is eventually acked under fair delivery | W = 4, 2 classes x 2, 2 channels, 6 messages, link capacity 3 | (a) drop first-write order within a channel: overflow; (b) acks that name unopened packets (F1): liveness fails; (c) advance U' past a partial block: overflow |
| `RNOrderedChannel` | 4 | ordered delivery in index order with no gaps; sequenced never at or below the watermark; the epoch reset on both delivery paths; held records released in (ordering, sequencing) order | 1 channel, 3 ordered + 3 sequenced, duplicating link | (a) RakNet's heap-path watermark without `+1` (`§11` item 18): duplicate delivery; (b) no watermark reset on the heap path (item 19): a fresh sequenced record is dropped |
| `RNBlocks` | 5 | delivered bytes equal sent bytes; one delivery per block; the block counted once in the window; consecutive numbering | 1 block x 3 fragments, lossy reordering link | (a) concatenate by arrival (`§8.2`): wrong bytes; (b) receipt on the last-index fragment (`§7.3`): ACKED before every fragment is acked |
| `RNReceipts` | 3, 7 | each serial gets at most one receipt; ACKED only after every fragment is acked; LOSS only for the unreliable receipt type; a pending unreliable receipt resolves by its deadline | 2 reliable + 2 unreliable receipt sends | fire LOSS for a reliable type on eviction: a LOSS for a delivered message |
| `RNScheduler` | 6 | no starvation with budget >= 1; first-write order within a channel; unreliable never blocked by a full window; stride ratios under backlog (as a bounded-count invariant) | 4 priorities, 2 channels, 6 messages, window 2 | (a) no channel inheritance: an earlier LOW starves behind later HIGH on one channel; (b) one queue for reliable and unreliable: a blocked head stalls unreliable |
| `RNSlots` | 7 | a message is never attributed to a generation it did not come from; CONNECTED and DISCONNECTED alternate per slot; reset follows processing | 1 slot, 2 generations, reuse within one update | reset before processing the batch: an old message lands in the new generation |
| `RNMessageEndpoint` | 7 | the composition: two endpoints, both directions, transport acks modelled as "ack iff opened", CONFIG first; safety and liveness from 6.3 and 10.6 | 2 channels, W = 4, 4 messages each way, link capacity 2 | the F1 lying ack (liveness); a receiver that drops on a full pool instead of the sender counting (safety) |

`RNReliableWindow` and `RNMessageEndpoint` are the two that matter most. The first
is the R5 argument of 6.3 made checkable. The second is the whole layer. Both are
owed before layer 3's code is written.

---

## 16. Thread rules, and the transition rule

### 16.1 Threads, inherited from the transport

The transport's phases are the layer's phases (`U:37-62`, `sdk/rocketnet.h:841-860`).

| Call | Thread | When |
|---|---|---|
| `create`, `destroy`, `stop`, `update`, `send_packets`, `disconnect_client`, `disconnect` | main | never while a fill phase is open |
| `start_message`, `finish_message`, `abort_message`, `write_packets` | any game thread | strictly between `update` and `send_packets`; **at most one thread per connection at a time** |
| `send_message` | any game thread during the fill phase, under the same one-per-connection rule; or the main thread outside a fill phase | queues only; writes nothing |
| message, receipt and event accessors, stats, counters | any thread | after `update` and before the next `update`, read-only, concurrent reads allowed |

**One writer per connection.**
- Connections share nothing mutable: the pools, the endpoint and the channel state
  are per connection.
- The receipt serial counter is one atomic per object.
- Counters are per connection and are summed on read.
- So a game partitions its slots across threads and needs no lock.
- Debug builds assert the rule with a per-endpoint owner atomic, and release
  builds do not check it, as the transport's leases are checked and its phase rule
  is not.
- The handoff from fill to flush is the transport's: a join or a barrier, never a
  wall-clock wait.

### 16.2 The transition rule

Every peer-visible transition in the design, and what carries it (R3):

| Transition | Carried by | Why a lost first packet cannot hang |
|---|---|---|
| connection start | the transport's handshake (redundant, timer-retried; `S:2024-2121`) | the transport's own rule; the layer resets locally on the CONNECTED edge (server) or at create (client) |
| connection end | the transport's redundant disconnect burst, or its timeout | same; the layer resets locally on the DISCONNECTED edge |
| configuration agreement | the CONFIG record: reliable, resent until acked | resend; and nothing waits on it (10.4) |
| channel start | **none exists**: channels exist from reset on both ends | there is no packet to lose |
| ordering epoch (sequenced records ahead of their ordered message) | the ordered message, which is reliable | sequenced records wait in hold until it arrives; it is resent until acked |
| block start | **none exists**: every fragment is a reliable record that names its block; any fragment opens the slot | each fragment is resent until acked |
| window advance | the transport's acks of packets this end sent | the sender resends on its own RTO; nothing waits for a report of loss (P12) |
| receipt ACKED / LOSS | sender-local: acks and a deadline | no packet announces it |
| disconnect for a violation or timeout | the transport's disconnect | the transport's rule |

The design has no reset packet, no channel-open packet and no block-start packet.
Every transition is either local to one side, or carried by a reliable record, or
carried by the transport's handshake and disconnect, which are themselves retried.
A future layer message (a flow subscribe, say) is a reliable control record (13.4),
so it inherits this.

### 16.3 The pure-module rule, per machine

| Machine | State struct | Events | Actions | Clock | Sockets | Allocation |
|---|---|---|---|---|---|---|
| pools | `rocketnet_message_pools_t` | acquire, release, reset | a handle, or refusal | none | none | none (memory given at init) |
| records | none (functions) | bytes | parsed records, or a violation | none | none | none |
| window | inside the endpoint | number, ack, packet dropped, time value | resend due, receipt, disconnect | value only | none | none |
| channels, blocks, scheduler | inside the endpoint | a record, a send, time value | deliver, hold, write | value only | none | none |
| endpoint | `rocketnet_message_endpoint_t` | section 10.1 | section 10.1 | value only | none | none |
| slots | `rocketnet_message_slots_t` | observe | CONNECTED, DISCONNECTED, RESET | none | none | none |
| client, server objects | the binders | the transport | the transport | `time` from the caller, as the transport | through the transport | once, at create |

---

## 17. For the reviewer: all eight types reachable, two shipped

### 17.1 The six later types, one by one

The claim: each type is added by enabling one bit in
`ROCKETNET_MESSAGE_ENABLED_TYPES`, plus a dispatch case or a send-side check.
It needs no wire change and no per-channel state change. "Wire" means section 13.
"Per-channel state" means the table in 7.1. "Connection state" means everything
else in the endpoint.

| Type (API value) | Wire | Per-channel state | Connection state | What the enabling card adds |
|---|---|---|---|---|
| UNRELIABLE_SEQUENCED (1) | code 1 and its fields (ordering, sequencing, channel) are defined and parsed by layer 2 from v1 | uses `sequenced_write` and `sequenced_read`, which v1 maintains (the ordered path resets both, 7.2) | held-sequenced pool, present in v1's layout (7.4) | the send-side index assignment (7.2); the 7.3 row for code 1; the SINGLE form accepts it |
| RELIABLE (2) | code 2: number only, defined in v1 | none: no channel | the window; v1 already runs code 5 through the same unordered reliable path | the send-side enqueue as an unordered candidate (9.1); deliver on arrival (7.3) |
| RELIABLE_SEQUENCED (4) | code 4: all fields, defined in v1 | the channel FIFO and both sequenced fields, from v1 | the window and hold pools; stale records are acked and dropped (I12), and counting them against the window is already conservative | the 7.3 row for code 4; it joins the channel FIFO like code 3 |
| UNRELIABLE_WITH_ACK_RECEIPT (5) | sent as code 0, which is unchanged | none | receipt entries in sent-packet records (6.4), the deadline sweep, the receipt list: all in v1's layout; v1's `end_update` already runs the sweep over an empty set | enable the send value; above 1176 bytes, upgrade to 6 (7.5) |
| RELIABLE_WITH_ACK_RECEIPT (6) | sent as code 2 | none | the descriptor's serial and the unacked-fragment count, both in v1; the receipt list | enable the send value; fire ACKED at the zero count, which v1 already reaches when it releases the buffer |
| RELIABLE_ORDERED_WITH_ACK_RECEIPT (7) | sent as code 3, which ships in v1 | as code 3 | as 6 | enable the send value |

Nothing in this table changes:
- the flags byte;
- a field's width or position;
- the per-channel struct;
- the allocator's classes;
- the window's accounting.

Each later type adds a case where v1 already has the switch.

The test for a reviewer: pick a type and find the first place its enabling card
would have to change a v1 struct or a v1 wire byte. If there is one, this design
is broken there.

### 17.2 The study's invariants, answered

| Inv. | The study asks | This design |
|---|---|---|
| I1 | the reliability code on the wire from v1; dispatch on it; unknown codes dropped and counted | 3-bit code in every record (13.2); dispatch per code (7.3); unknown or disabled codes are a counted violation |
| I2 | header variable by reliability | fields follow the code (13.2) |
| I3 | numbers and ordering indices independent; only reliable records move the window | 6.1 and 7.2; code 1 takes no number |
| I4 | message number is not the transport sequence | resends are new packets with the same number (6.1) |
| I5 | both indices per channel, coupled, from v1; one watermark rule on both paths | 7.1 and 7.2 |
| I6 | reorder buffer keyed by (ordering, sequencing) | the hold list order (7.3) |
| I7 | bounded at create, per connection; a stated policy when full; in-flight countable by the sender | section 4 pools; 6.3 accounting; send refuses, receive cannot run out, unreliable drops are counted |
| I8 | per-packet acks; packet to list of records | sent-packet records with entry spans (6.4) |
| I9 | a serial on every record, issued at send, returned | descriptor `serial`; `send_message` returns it (7.5, 12) |
| I10 | unreliable receipts outlive the frame | receipt entries in sent-packet records; deadline sweep (7.5) |
| I11 | say where reliable splitting happens; index placement; bounded slots; no stale id match | layer 5 splits; place by index; `hold_blocks` slots; identity is the first number (8); unreliable large stays unreliable on the transport path |
| I12 | sequenced acked on arrival, may be discarded | yes; the window frees on ack regardless (7.3) |
| I13 | unreliable not charged to the reliable window | 9.3 step 2: a full window skips reliable only |
| I14 | RTO and dead-connection per connection | one RTO per endpoint (6.5); the transport's timeout plus `reliable_timeout` |
| I15 | channel checked at runtime count on both ends; field width from the constant | refuse on send (12), violation on receive (13.3); WIDE above 255; `ROCKETNET_MAX_CHANNELS` up to 65,536 |
| I16 | the protocol does not share a user channel | the layer's own records are code 5, which has no channel |
| I17 | messages handed up, one stream, update lifetime | the messages view, valid until the next update; the client's cursor copies |

### 17.3 Departures from RakNet, each on purpose

1. Two ordering-buffer bugs fixed: the `+1` and the epoch reset on the hold path
   (`§11` items 18 and 19).
2. Reassembly by index, not by arrival (`§8.2`).
3. A block receipt fires when every fragment is acked, not on the last index
   (`§7.3`).
4. A block's identity is its first message number. There is no 16-bit split id
   that can collide (`§11` item 17).
5. Unreliable messages above a packet stay unreliable, on the transport's
   fragments, instead of being upgraded. The exception is the unreliable receipt
   type, which is upgraded for the reason in P3.
6. Out-of-range channels and reliabilities are refused, not repaired
   (`§5.2` step 1, `§11` items 16 and 20).
7. All receive buffering is bounded, and the sender keeps the receiver's bound
   (6.3). RakNet bounds only the resend buffer (`§3.1`, I7).
8. A channel inherits the highest waiting priority. RakNet lets a later
   high-priority ordered message overtake, and then holds it at the receiver.
9. The length is in bytes with a one-byte short form. RakNet uses 16 bits of bit
   length.
10. RTO from this layer's own unambiguous samples. Defaults suit tick-paced acks.
11. UNRELIABLE duplicates. The transport's replay window already refuses replayed
    packets (`sdk/rocketnet_replay.cpp:27-40`, I4), so the duplicated-datagram delivery RakNet allows
    (`§11` item 1) cannot happen here.

---

## 18. What a layer card inherits from this document

Each layer card gets:
- its section here;
- the rules R1 to R6;
- the preconditions of section 2 that it touches;
- its TLA+ module from section 15, written first;
- its "checked by" list, which is its definition of done.

A layer is locked only when Glenn agrees the lock.

## 19. Order of work, open choices, and what is not done

### 19.1 Cards, in order

1. **Fix F1** in `sdk/rocketnet_replay.cpp:54-60`: clear the whole window on a skip
   of the window size or more. The `ReplayWindow` model in
   `docs/TLA-INVENTORY.md` reproduces it first. This blocks RELIABLE_ORDERED.
2. **Add `rocketnet_server_transport_client_generation`** (P7), with the
   slot-reuse-in-one-update test (P6).
3. Layer 1, pools: `RNPools`.
4. Layer 2, records: vectors and fuzz.
5. Layer 3, window: `RNReliableWindow`, and `RNReceipts` (the receipt path for
   the first version is empty, but the model covers the sweep).
6. Layer 4, channels with codes 0, 3 and 5: `RNOrderedChannel`.
7. Layer 5, blocks: `RNBlocks`.
8. Layer 6, scheduler: `RNScheduler`.
9. Layer 7, endpoint and slots: `RNMessageEndpoint`, `RNSlots`, and the
   two-endpoint harness.
10. Layer 8, client and server objects: loopback functional tests, and swarm
    traffic.
11. Layer 9, C API: header, ABI introspection, a C# smoke.
12. **The space game integrates.**
13. The six later types, in any order, each one card (section 17.1).
14. Stress at 1024 and 10,000 channels: `-DROCKETNET_MAX_CHANNELS`, the
    two-endpoint harness first, then the swarm.

### 19.2 What blocks this design

Nothing blocks the design. Two things block its shipping: F1 (card 1) and the
missing per-slot generation (card 2).

### 19.3 Choices for Glenn

- **STANDARD.md scope.** Section 13 is written to drop in. It could become a
  "Messages" section once the scope line at `S:7-10` moves, or a second standard
  for the tier above. This card does not edit STANDARD.md.
- **Identical configuration, verified, versus negotiated capacities** (10.4). This
  design picks identical-and-verified, because it never waits. Negotiation would
  let a client and server differ, at the cost of a wait before reliable sends.
- **The block stride.** 1152 (this design) leaves at least 22 bytes per fragment
  packet for small records. A full-packet stride (1174) saves about 2% on large
  blocks and leaves nothing.
- **No congestion control yet.** `packet_budget` and the window cap the send rate.
  The transport carries RTT and loss, so a congestion signal can live there
  later (I13), and nothing here forecloses it.
- **The defaults** in section 12's table, and the 436,000-byte per-connection cost
  they imply.
- **The C# shim surface**, provisional until the shim arrives as data (`§13`).
- **The layer list in section 3**, for correction before layer 1 starts.

### 19.4 Not done by this card

- No code, no TLA+ module and no test was written or run. This is the design.
- The struct sizes in section 14 are working figures, to be fixed by `sizeof` in
  layer 1.
- P2, P6 and P8 are argued from the code and still need the tests named beside
  them.
- The ack-delay effect on RTO under a peer that sends nothing (acks then ride
  10 Hz keep-alives, `S:1981-2023`) is reasoned, not measured. The functional
  tests should record the RTO they settle at.

# CHANNELS: the message layer, bottom up

The design every layer card of RocketNet's message layer implements: a client and
a server on top of `client_transport` and `server_transport`, sending messages to
each other over `n` ordering channels with reliability chosen per send, as RakNet
does it. UNRELIABLE and RELIABLE_ORDERED ship first, then the client and server
objects, so the space game can integrate and prove it; the other six RakNet types
come after, and this document shows that each of them slots in without a change to
the wire or to the per-channel state.

It is written to be read beside [RAKNET-RELIABILITY.md](RAKNET-RELIABILITY.md)
(cited as "the study", with its section numbers and its invariants I1 to I17).
RocketNet facts are cited as `file:line` at the commit this document lands on.
`S:n` is STANDARD.md line n and `U:n` is USAGE.md line n.

Nothing here is built. Every structure, size and name below is a proposal for the
layer cards. The layer list goes to Glenn for correction before layer 1 starts
(section 17).

---

## 1. The shape, in one page

**What a message is.** An opaque byte buffer (CLAUDE.md, "Messages are opaque
byte buffers"), sent to one connection with three numbers on the send: a
reliability (RakNet's eight values, with RakNet's numbering), an ordering channel
(`0 .. num_channels-1`), and a priority (RakNet's four). The message layer never
looks inside the bytes.

**The decisions that shape everything else**, each argued in the section named:

| # | Decision | Where |
|---|---|---|
| D1 | Every payload the layer sends is either a **RECORDS** payload (many message records, aggregated) or a **SINGLE** payload (one unreliable message at payload offset 0, written in place, with a one-byte footer). The form is read from the payload's last byte. | 5, 12 |
| D2 | A record's header is variable: a flags byte, a u16 length, then only the fields its reliability code implies. Sizes match RakNet's byte for byte: 3, 6, 10, 10 and 13 bytes. | 5, 12 |
| D3 | Reliable records carry a **connection-wide reliable message number** (24 bits on the wire), as RakNet does. Ordered and sequenced records carry a per-channel ordering index and sequencing index (24 bits each). The two families are independent (study I3). | 6 |
| D4 | **The sender keeps the receiver's capacity.** A reliable record is numbered only inside a window `[U, U + N)` where `U` is the sender's oldest unacknowledged number, and the window is also counted per size class. That bound proves the receiver never runs out of hold storage, so it never has to drop a record whose packet the transport already acknowledged. | 6.3 |
| D5 | **Within one ordering channel, records are first sent in ordering-index order.** Priority chooses between channels and between unchanneled messages, never within a channel. This is what makes D4's bound hold. RakNet allows the reorder; nothing is delivered differently because of the change. | 6.2, 16.3 |
| D6 | Per channel: four counters (ordered write, sequenced write, ordered read, sequenced read) and three list heads, 24 bytes, from the first version. The sequenced counters are reset by ordered traffic from day one, so the sequenced types arrive later without changing how the ordered path behaves (study I5). | 6.2 |
| D7 | Reliable messages larger than one record are **blocks**: fragments of a fixed 1024-byte stride, each its own reliable record, numbered consecutively, resent one by one, reassembled by index into bounded slots. Unreliable messages larger than a packet stay unreliable and ride the transport's own fragmentation in SINGLE form. | 8 |
| D8 | Scheduling is RakNet's: a 64-bit weight per queued item from per-priority strides 1, 5, 14 and 35, in one min-heap. Resends go before new sends. Unreliable traffic is never charged to the reliable window (study I13). | 9 |
| D9 | Every state machine is a **pure module**: one struct, events in, actions out, no socket, no clock read, no allocation. The client and server objects are the only code that binds a machine to a transport, and the transport is the only code that binds to sockets. | 3.2, 10 |
| D10 | **No peer-to-peer state transition rides one unreliable packet.** The layer has no channel-open, block-start or reset packet at all. Its one control record (CONFIG) is reliable. Connection start and end are the transport's handshake and redundant disconnect. | 3.1 |
| D11 | Nothing on a send or receive path allocates. Every buffer is carved at create, per connection, by layer 1, and the sizes come from one formula (section 13). Full queues refuse the send and count it; the receiver never needs to refuse. | 4, 13 |
| D12 | A malformed or out-of-bounds record from an authenticated peer **disconnects that peer**, with a counter for the reason. Continuing would mean dropping a reliable record that was already acknowledged, which hangs its channel for good. | 3.4 |

The layers, bottom up. Each one is secured before anything above it starts:

| Layer | What | Ships with the first two types |
|---|---|---|
| 0 | What the transport must guarantee (preconditions, not built here) | P1 needs a fix first (F1) |
| 1 | The connection allocator | yes |
| 2 | Message records in a packet: the codec for both forms | yes |
| 3 | Per-send reliability (the reliable window) and ordering channels | yes |
| 4 | The types: dispatch for all eight, two enabled | yes |
| 5 | Blocks | yes (RELIABLE_ORDERED above one record) |
| 6 | Scheduling with four priorities | yes |
| 7 | The endpoint machine, then the client and server objects | yes |
| 8 | The C API in `sdk/rocketnet.h` | yes |

---

## 2. Layer 0: what the transport gives, and what this layer requires of it

The facts this layer builds on, from the transport as it stands:

- **A packet id at `start_packet`.** `rocketnet_packet_context_t` carries
  `packet_id` and `sequence` (`sdk/rocketnet.h:880-888`). The id is a
  process-wide, non-repeating lease ticket (`sdk/rocketnet_packet_lease.h:7-8`),
  "unique and monotonic, never a ring slot"
  (`sdk/rocketnet_server_transport.cpp:1334-1336`). It is **not** consecutive per
  connection, so the layer looks it up in a table (section 6.1), never by
  arithmetic.
- **A per-update batch of acknowledged packet ids** (`sdk/rocketnet.h:966-976`
  server, `:639-646` client). "The transport tells; it never resends"
  (`sdk/rocketnet.h:970`). An id is acknowledged when the peer **opened** the
  packet: blocks are built from every opened session sequence (S:1693-1698,
  S:1795-1800). A fragmented payload's id surfaces only when every fragment is
  acknowledged (`sdk/rocketnet.h:830-834`, U:121-123). Overflow past the batch is
  deferred, not lost (`sdk/rocketnet.h:972-975`).
- **A zero-copy frame** of up to `ROCKETNET_MAX_PAYLOAD_BYTES` = 1192
  (`sdk/rocketnet.h:420`). The payload pointer is 16-aligned and its capacity a
  multiple of eight (`sdk/rocketnet.h:816-818`, `sdk/rocketnet_constants.h:32-45`).
  A larger request, up to the configured maximum (default 16384,
  `sdk/rocketnet.h:432`), goes through the transport's arena with one copy and
  unreliable semantics: "A lost fragment loses the whole payload"
  (`sdk/rocketnet.h:423-430`, U:118-124).
- **A per-update delivered batch**, valid until the next update and replaced, never
  appended (`sdk/rocketnet.h:785-803`, U:68-81). Every update resets it first
  (`sdk/rocketnet_server_transport.cpp:956`).
- **The fill-phase threading contract** (`sdk/rocketnet.h:841-856`, U:39-55):
  `update` and `send_packets` on the main thread; `start_packet`, `finish_packet`
  and `abort_packet` on any number of game threads strictly between them, joined
  before the flush.
- **The game owns time.** "The library never reads a clock of its own on these
  paths" (`sdk/rocketnet.h:542-544`).
- **Packet-level duplicate suppression.** The replay window refuses a packet
  opened twice (U:171, U:283-291). So, unlike RakNet (study section 6.2, "Code vs
  comment"), a duplicated datagram never delivers an UNRELIABLE message twice.
- **The transport's rtt is a floor, not a reading** (U:216-236). It is no input to
  a resend timer. This layer measures its own round trips (section 6.1).
- **A global allocator hook** used only at create (`sdk/rocketnet.h:319-325`).

The preconditions. Each one is a property this layer's correctness rests on, with
the place it is checked. A layer card does not start until its preconditions are
green.

| # | Precondition | Status | Checked by |
|---|---|---|---|
| P1 | **Acks are truthful**: a surfaced packet id names a packet the peer opened. | **Broken today** by F1 (`docs/TLA-INVENTORY.md:30`): after a forward jump of 4096 or more sequences, `sdk/rocketnet_replay.cpp:54-60` clears 4 of 64 window words, and the next ack block "claim[s] thousands of sequences it never opened". A reliable record whose packet is falsely acknowledged is never resent, and its channel stops for good. **F1 must be fixed before RELIABLE_ORDERED ships.** | `RNReplayWindow` and `RNAckDelivery` (`docs/TLA-INVENTORY.md:578-579`, `INV SurfacedOnlyIfOpened`) |
| P2 | **Every opened payload reaches the delivered batch.** The batch is sized to the drain bound "so a burst is never silently discarded after its sequence has already been consumed" (`sdk/rocketnet_constants.h:47-53`); the drain refuses excess packets *before* opening them (`sdk/rocketnet_constants.h:55-69`), so they are never acknowledged. | holds by construction; `FRAGMENT_PAYLOAD_BATCH_FULL` stays zero (`sdk/rocketnet_server_transport.cpp:1028-1040`) | a transport test that asserts the counter at the drain bound |
| P3 | **An update processes one batch fetched once.** A packet opened in update `k` is acknowledged no earlier than update `k`, and anything the peer sends in reaction is opened in update `k+1` or later. Layers 3 and 5 free delivered storage at the start of the next update and rely on this. | holds: one `receive` or `receive_batch` call per update (`sdk/rocketnet_server_transport.cpp:973, 1054`) | `RNMessageEndpoint` (section 14) models update boundaries |
| P4 | **A slot's connection identity is observable.** The server object must reset a slot's machine whenever a new connection takes the slot, including a disconnect and a reconnect between two of its updates. (The client needs nothing: one client transport is one connection.) The slot assignment resets the transport's own per-slot state (`sdk/rocketnet_server_protocol.cpp:546-558`) but exposes no count. | **missing**: one transport addition, `rocketnet_server_transport_client_generation( transport, client_index )`, a u64 incremented at every assignment | a transport test: disconnect and reconnect in one update, the generation moves |
| P5 | **The packet id is known before the payload is written.** It is: `start_packet` fills `context.packet_id` (`sdk/rocketnet.h:880-890`). | holds | none needed |
| P6 | **A failed `finish_packet` is visible.** It returns zero (`sdk/rocketnet.h:862-865`). The layer treats the records in that packet as never sent. | holds | layer 7 unit test |
| P7 | **A slot's first payload of a new connection comes in a later update than its assignment.** A client sends payloads only once CONNECTED, which it reaches on the server's first keep-alive for the assignment (S:2205-2206), so the first payload of a new connection is at least a round trip after the assignment, in a later server update. The server object's update order (10.3) relies on it. | holds by the handshake; unchecked | a transport test: reassign a slot inside one update with traffic on both connections, assert every payload of that update came from the old one |

---

## 3. The rules every layer keeps

### 3.1 No state transition between peers rides a single unreliable packet

Every packet that announces a transition the peer must act on is either carried on
the reliable path or resent on a timer until the peer's next-state packet confirms
it, as netcode does for its handshake, so a first-packet loss never hangs either
side. This layer meets the rule mostly by having **no transitions to announce**:

| Transition | How it travels | Why a lost packet cannot hang it |
|---|---|---|
| Connection start (both ends start their machines from zero) | The transport's handshake, confirmed by its own retransmission (S:1981-2000, "Send rate ... handshake retransmission"). The server resets a slot's machine at slot assignment (P4). The client's machine is fresh from create, because a client transport carries one connection for its lifetime (its surface has no reconnect call, `sdk/rocketnet.h:546-646`). | The server sends payloads only to an assigned slot, so its machine is reset before its first record. The client transport delivers payloads from _sending connection response_ on, before it is CONNECTED (S:2208-2214), so the client machine must accept records from create and **must not** reset at CONNECTED: a reset there would discard records the transport had already acknowledged. The client sends only once CONNECTED, after the server's reset. |
| CONFIG (each end tells the other its capacities) | A reliable CONTROL record, message number 0 in each direction, resent until acknowledged (section 12.6). | It is reliable. Nothing waits for it: a sender sends from its own configuration, and a peer whose capacities are too small finds out from the CONFIG or from the first out-of-range record, and disconnects with a named counter. |
| Channel start | None. Every channel exists from reset with its counters at zero. | There is no open packet to lose. |
| Block start | None. Every fragment is a reliable record carrying its index and count, and any fragment that arrives first opens the reassembly slot (section 8). | Each fragment is resent until acknowledged. |
| Sequenced epoch (an ordered message moves the channel's epoch) | The ordered message itself, which is reliable (study section 3.2). | It is reliable. Sequenced records that arrive ahead of their epoch wait in a bounded hold (section 7). |
| Receipts | Local events at the sender, from acks and deadlines. | Nothing crosses the wire. |
| Reset or disconnect | The transport's: a deliberate disconnect sends the redundant burst, 10 packets (S:1995, `sdk/rocketnet.h:905-907`), and either end times out on silence. A reconnect is a new connection with a new generation (P4). | Redundancy and the timeout, both transport mechanisms. The message layer adds no disconnect packet and has no state that outlives a connection. |

A layer card that wants to add a transition writes it into this table first, with
its reliable carrier, and its TLA+ module runs it under the `"first"` and
`"alternate"` loss patterns of `tla/Link.tla` (`tla/README-Link.md`, "Loss
patterns").

### 3.2 Every state machine is a pure module

Glenn, 2026-10-05: the security and reliability machines, the client and server
logic and the transport connection logic are all pulled out to state machines so
they can be tested without sockets. For this layer that means:

- **Its state lives in one struct.** The endpoint (one per connection) is a
  `rocketnet_message_endpoint_t`. Its arrays are carved by layer 1 at create and
  reached by index, never by pointer to heap memory it owns.
- **It is driven by events**: a payload's bytes, an acknowledged packet id, an
  application call, a buffer to write into, and **time passed in as a value**.
- **It answers with actions the caller performs**: bytes written into the buffer
  the caller supplied ("send these bytes"), message descriptors appended to an
  output list ("deliver this message"), receipt events appended to a list, a
  violation code ("disconnect this peer"), and a `next_deadline` the caller reads
  ("call `advance_time` by then"; this is the timer, owned by the caller).
- **It opens no socket, reads no clock and allocates nothing.** A unit test drives
  two endpoints into each other's `receive` through a scripted lossy link, end to
  end, with no transport at all.

The client and server objects (section 10) are thin: they own a transport and one
host machine, also pure, that turns transport outputs (payload batch, acked batch,
slot generations) into endpoint events. The transport stays the only code that
touches sockets.

### 3.3 No allocation on a send or receive path

`rocketnet_malloc` is called at create and nowhere else. `send_message` copies
into a pool slot or refuses. `receive` hands out pointers into the transport's
batch, or into a pool slot it already owns. When a pool is full the send is
refused and counted; that is the only failure, it happens on the sender, and it is
reported to the caller who made the send.

### 3.4 Strictness: a violation disconnects

The peer is authenticated (the payload was opened under the session's keys), so a
record that breaks a rule here comes from a defective or hostile peer. The
receiver does not try to skip it. It returns a violation code, the object counts
`MESSAGE_VIOLATION_<reason>` and disconnects that peer. Skipping is unsafe: the
packet was already acknowledged by the transport, so any reliable record in it
that the receiver discards is a record the sender will never resend. The reasons
are listed with the wire rules (section 12.7).

---

## 4. Layer 1: the connection allocator

**Owns.** One contiguous region per connection (a server slot or the client),
taken from `rocketnet_malloc` at object create and never resized. Inside it, the
fixed arrays every higher layer uses, at offsets computed once from the
configuration:

| Region | Elements | Element |
|---|---|---|
| send pools, three classes | `send_small`, `send_medium`, `send_large` slots | 64, 256, 1184 bytes |
| send block slots | `send_blocks` | `max_message_bytes`, 16-aligned |
| hold pools, three classes | `window_small`, `window_medium`, `window_large` slots | 64, 256, 1184 bytes |
| unreliable hold pool | `unreliable_hold` slots | 1184 bytes |
| receive block slots | `block_slots` | `max_message_bytes` plus a fragment bitmap |
| record descriptors | `send_small + send_medium + send_large + send_blocks + window_fragments` (sender), `window_small + window_medium + window_large + unreliable_hold` (receiver) | 40 bytes (section 13) |
| channels | `num_channels` | 24 bytes (section 6.2) |
| sender window ring | the power of two at or above `N` | 1 byte |
| receiver window bitmap | the same power of two | 1 bit |
| scheduling heaps, two | the sender descriptor count, each | 16 bytes |
| sent-packet table | `sent_packets` entries, `sent_refs` refs | 24 bytes, 8 bytes |
| unreliable-receipt entries | `pending_receipts` | 24 bytes |

The class thresholds are protocol constants, the same on both ends, because the
receiver must pick the same class the sender counted: data of 1 to 64 bytes is
small, 65 to 256 is medium, 257 to 1184 is large. 1184 is the largest record data
a packet carries, rounded up to 16 (section 12.4).

**Each pool is a free list of fixed-size slots**, indexed by u16. `alloc` pops,
`free` pushes, `reset` rebuilds the list. There is no general-purpose allocator
and so no fragmentation argument to make: a pool either has a slot or it does not,
and layer 3's window rule decides in advance which pools can never be empty when
a receiver needs them.

**Guarantees.**
- No call on a send or receive path reaches `rocketnet_malloc`.
- `alloc` on an empty pool returns NONE and never touches memory outside the
  pool. `free` of a slot not allocated, or freed twice, asserts in debug and is
  refused and counted in release.
- `reset` returns every slot in O(slots) and is called only at connection start.
- A slot is 16-aligned, so a held message handed to the application is as aligned
  as the transport's own frames.

**Checked by.** `RNMessagePools` (section 14) for the free-list states, unit tests
that exhaust every pool and assert the refusal and the counter, and a test that
drives a whole connection's lifetime and checks every pool full again at reset.
The sizing formula is a function with a unit test that compares it with
`sizeof` and the offsets the carve actually produced.

---

## 5. Layer 2: message records in a packet

**Owns.** The codec, as pure functions: write a record into a buffer, read one
from a buffer, write and read the SINGLE footer, and validate. It owns no state.
The full normative layout, to the bit, is section 12. In outline:

**RECORDS payload**: `record record ... record footer`. Records are parsed front to
back. The last byte is the footer, `0x00`.

```
record = flags(u8) length(u16) [number(u24)] [sequencing(u24)] [ordering(u24)]
         [channel(u8 | u16)] [fragment index(u16) fragment count-1(u16)] data(length)
```

**SINGLE payload**: `data fields footer`. One message, written by the application
at payload offset 0 in the transport's frame. The footer is the last byte:
`0x80 | flags`. The fields the flags imply sit between the data and the footer,
and the data length is whatever is left. An UNRELIABLE message in SINGLE form
costs exactly one byte, the footer.

Why the footer goes at the end: the application writes at the transport's payload
pointer, which is 16-aligned (`sdk/rocketnet.h:816-818`), and a word-based
serializer must be able to root there (`sdk/rocketnet.h:411-414`). A header in front would push the message off that alignment, or
force a copy to put it back. With the header behind the data, `start_message`
returns the transport's own pointer and `finish_message` writes one byte after
the last byte the application wrote. Nothing is copied.

**Guarantees.**
- `write` then `read` is the identity on every legal record, in both forms.
- `read` of any byte string either returns a legal record or a violation reason.
  It never reads past the payload and never returns a partly read record.
- The record sizes match RakNet's per-type overhead (study section 4.2): 3 bytes
  for UNRELIABLE, 6 for RELIABLE, 10 for RELIABLE_ORDERED and UNRELIABLE_SEQUENCED,
  13 for RELIABLE_SEQUENCED, plus 4 for a fragment and 1 for a wide channel.
- At most `ROCKETNET_MESSAGE_MAX_RECORDS_PER_PAYLOAD` = 64 records per RECORDS
  payload. The cap bounds the receiver's per-update delivered list (section 13).

**Checked by.** Known-answer vectors (section 12.8) compared with `memcmp`, as
the transport's Appendix A vectors are (CLAUDE.md, "assembled wire vectors ...
diff against the implementation with a straight memcmp"); a round-trip test over
every flag combination; a test per violation reason; and a fuzz target on `read`
with an assert that the parse never leaves the buffer. The codec has no state, so
it has no TLA+ module.

---

## 6. Layer 3: per-send reliability and ordering channels

Reliability is chosen per send. The ordering channel is a number on the send. Both
are fields of the record, and the receiver dispatches on the record's code
(study I1). This layer has two parts: the **reliable window**, shared by every
reliable record on the connection whatever its channel, and the **channels**.

### 6.1 The reliable window

**State, sender side.**

| Field | What |
|---|---|
| `next_number` (u32) | the next reliable message number. Assigned when a record is first written into a packet, never at send (study section 5.2: assigning at send "created huge gaps in the receiver's hole queue"). |
| `oldest_unacked` (`U`, u32) | the lowest number not yet acknowledged; everything below it is acknowledged. |
| window ring | one byte per number in `[U, next_number)`: the record's size class (small, medium, large, fragment) and an acknowledged bit. |
| `window_count[4]` | how many numbers in `[U, next_number)` are in each class. |
| resend list | the numbered, unacknowledged records in order of last send, each with `resend_time`. |
| sent-packet table | per written packet that carried at least one reliable or receipt record: `packet_id`, `write_time`, and refs to those records (number and descriptor index for a reliable record; serial for an unreliable-with-receipt record). Open addressing on `packet_id`, because ids are not consecutive (section 2). A packet of plain unreliable records gets no entry, and its acknowledgment is ignored. |
| RTT estimate | `srtt`, `rttvar`, `has_sample`; `rto` derived. |

**State, receiver side.**

| Field | What |
|---|---|
| `receive_base` (`B`, u32) | the lowest number not yet received. |
| window bitmap | one bit per number in `[B, B + N)`. |

**Events and what they do.**

- *Write.* A reliable record may be numbered only if the window has room:
  `next_number - U < N` and `window_count[class] < limit[class]`, where the limits
  are `window_small`, `window_medium`, `window_large` and `window_fragments`, and
  `N` is their sum. Numbering appends to the ring, bumps the count, records the
  write time and puts the record on the resend list with
  `resend_time = time + rto`. The packet's table entry gets a ref.
- *Resend.* At write time, before any new record, every record on the resend list
  whose `resend_time <= time` is written again with the same number and fields,
  into whatever packet is being filled, and moves to the tail with a new
  `resend_time`. Each send of a record gets its own ref, so an ack of any packet
  that carried it acknowledges it (study section 6.2, ACK).
- *Acked packet id.* Look the id up. For each ref: if the record still holds that
  number and is unacknowledged, mark the ring entry acknowledged, free the
  record's pool slot (for a block fragment, its descriptor; the block's send slot
  is freed when its last unacknowledged fragment is), take it off the resend list,
  and fire its receipt if it has one (section 7.3). Then advance `U` over acknowledged ring entries, decrementing
  `window_count` for each. Take one RTT sample from the entry's `write_time`
  (`time - write_time`, the time passed to the update that surfaced the ack), and
  retire the entry.
- *Receive (reliable record, number reconstructed to `m`).* If `m < B` or its bit
  is set: a duplicate. Drop it, count `MESSAGE_DUPLICATE`, and do nothing else
  (this is how a resend whose first copy arrived is absorbed). If
  `m >= B + N`: violation `WINDOW`. Otherwise set the bit, hand the record to the
  channel layer, and advance `B` over set bits.
- *Reset.* Everything to zero, pools reset.

**The timer.** `rto = clamp( srtt + 4 * rttvar, resend_min, resend_max )`, with
`srtt` and `rttvar` updated by the usual 1/8 and 1/4 gains, and `rto =
resend_initial` until the first sample. The defaults are 0.1 s, 1.0 s and 0.25 s
(section 11.2). Each sample is unambiguous, because every resend is a new transport
packet with a new id and the sample is taken per packet, never per record (study
I4). There is no exponential back-off and no congestion window in the first
version; RakNet has no back-off either (study section 9, `CC.cpp:290`). Both are
open (section 17).

**A table entry that is never acknowledged** (the packet was lost) is retired when
the table needs its slot and the entry is older than `resend_max`. Its reliable
records are on the resend list already, so retiring loses nothing. If the table is
full of younger entries the oldest is evicted anyway and counted
(`MESSAGE_SENT_TABLE_EVICTED`); that costs at most a resend that was not needed,
the same trade RakNet makes at 513 datagrams (study section 5.4).

**Wire widths.** Numbers are 32-bit inside and 24-bit on the wire, reconstructed by
the receiver against `B` as the transport reconstructs its sequence: the value
within half the 24-bit range, 2^23, of the expected one. That is safe because no
legitimate record is further than 2^23 numbers from `B`: the window bounds what is
in flight (`N`, at most a few thousand), and a stale copy delayed in the network
is bounded by the transport's replay window, at most `ROCKETNET_HISTORY_PACKETS` =
32768 packets (`sdk/rocketnet_history.h:7-14`) of at most 64 records each, which is
2^21. Sixteen bits would not be safe by the same arithmetic; that is why the width
is RakNet's 24 (study section 3.4).

### 6.2 Ordering channels

**State, per channel, both directions, 24 bytes:**

```c
struct rocketnet_message_channel_t
{
    uint32_t ordered_write;         // sender: the next ordering index an ordered send takes
    uint32_t sequenced_write;       // sender: the next sequencing index; set to 0 by every ordered send
    uint32_t ordered_read;          // receiver: the ordering index delivered next
    uint32_t sequenced_read;        // receiver: the lowest sequencing index still deliverable in the
                                    // current epoch; set to 0 by every ordered delivery
    uint16_t queue_head;            // sender: first record waiting in this channel's FIFO, or NONE
    uint16_t queue_tail;
    uint16_t hold_head;             // receiver: first held record, sorted (section 6.2), or NONE
    uint16_t hold_count;
};
```

The four counters are RakNet's (`RL.h:441-450`, study section 3.2). All four exist
and all four are maintained from the first version, including the two sequenced
ones that only the later types read: the ordered send sets `sequenced_write = 0`
(`RL.cpp:1656`) and the ordered delivery sets `sequenced_read = 0`
(`RL.cpp:1373`). Shipping without them would change the ordered path when the
sequenced types arrive (study I5).

**Send (application call).** A send on channel `c` is validated (`c <
num_channels`, refused and counted otherwise, never mapped to 0 as RakNet does,
study I15), copied into a send pool slot by size class, and stamped: an ordered
send takes `ordering = ordered_write++` and sets `sequenced_write = 0`; a sequenced
send takes `ordering = ordered_write` and `sequencing = sequenced_write++`. Then it
joins the channel's FIFO at the tail. Indices are assigned at send, as RakNet does
(`RL.cpp:1625-1657`), so they follow the application's order.

**The first-send rule (D5).** Only the record at the head of a channel's FIFO is
eligible for scheduling. When the head has been written once (for a block, when
its last fragment has been written once), it leaves the FIFO and the next record
becomes eligible. Resends are not affected; they come from the resend list.

The reason is the hold bound in 6.3. If a later record on a channel could be
numbered before an earlier one, the receiver would hold it while the earlier one
is not even numbered yet, the sender's window could slide past the held record,
and nothing would bound what the receiver holds. RakNet lets priority reorder
within a channel (its heap mixes all records, `RL.cpp:1676`), and its holds are
unbounded heaps (study I7). Nothing is delivered differently under the rule: the
receiver could never have delivered the later record before the earlier one
anyway. Section 16.3 states what this means for priority.

**Receive (a record that names a channel).** Reconstruct the ordering index `o`
against `ordered_read`, then:

- **Ordered record.** `o == ordered_read`: deliver; `ordered_read++`;
  `sequenced_read = 0`; then release from the hold list (below). `o > ordered_read`:
  hold. `o < ordered_read`: violation `ORDER`, because the window already rejected
  every duplicate by number, so an older ordering index on a new number can only
  come from a sender that broke the rules.
- **Sequenced record** (later types, section 7). `o == ordered_read`: deliver if
  `sequencing >= sequenced_read` and set `sequenced_read = sequencing + 1`, else
  drop and count `MESSAGE_SEQUENCED_STALE`. `o > ordered_read`: hold (it belongs to
  an epoch whose ordered message has not arrived). `o < ordered_read`: drop and
  count stale (a sequenced record of an epoch already closed is legitimately old).

**The hold list** is per channel, intrusive through the record descriptors, kept
sorted by the key `(ordering, kind, sequencing)` where `kind` puts a sequenced
record before the ordered record of the same index. That is RakNet's heap weight,
`(orderingIndex - offset) * 2^20 + sequencingIndex` with ordered at `+ 2^20 - 1`
(`RL.cpp:1439-1446`, study I6), as a list. Insertion walks the list (bounded by
the hold capacity); release pops from the front. **Release** runs after every
delivery on the channel: while the head's `ordering == ordered_read`, pop it and
apply exactly the rule a record arriving now would get. That one rule on both
paths is the repair of study section 11 items 18 and 19: RakNet's heap path sets
the watermark without the `+ 1` and skips the epoch reset.

A held record's bytes are copied into a hold pool slot of its class. A held
record is the one copy on the receive path, and only out-of-order arrival pays it.
A record delivered on arrival is handed to the application as a pointer into the
transport's batch: zero copies.

### 6.3 Why the receiver never runs out of hold storage

This is the property the whole layer stands on, so it is stated as a claim with
its argument. `RNReliableWindow` and `RNOrderedChannel` check it (section 14).

**Claim.** At every point, for each class, the number of held reliable records of
that class at the receiver is at most the sender's `window_count` limit for that
class.

**Argument.**
1. Every held reliable record `X` on channel `c` is waiting for an earlier
   ordering index on `c` that has not been received; call the earliest such record
   `Y`.
2. By the first-send rule, `Y` was written before `X` was first written, so `Y` is
   numbered and `number(Y) < number(X)`.
3. `Y` has not been received. A received ordered record at `ordered_read` is
   delivered at once, so an unreceived `Y` is unacknowledged, and so
   `U <= number(Y)` at the sender (P1: an acknowledgment means it was opened). If
   `Y` is a block, read `number(Y)` as the number of one of its fragments not yet
   received; every fragment of `Y` was first written before `X`, so step 2 holds
   for it too. If `X` is sequenced (a later type), `Y` is the ordered record at
   `ordered_read`, which was sent before `X` because `X` took its ordering index
   from `ordered_write` after `Y` advanced it.
4. So `U < number(X) < next_number`: every held record's number lies in the
   sender's current `[U, next_number)`.
5. The sender counts every number in `[U, next_number)` in `window_count` by class,
   acknowledged or not, and never lets a count exceed its limit. Held records are
   a subset of those numbers, so each class's held count is at most its limit.

The hold pool for each class has exactly the limit's number of slots, so a slot is
always free. The receiver therefore never refuses a reliable record, and never
needs to (3.4).

One timing detail completes it. A record released in update `k` is handed to the
application as a pointer into its hold slot, valid until the next update, so the
slot is freed at the start of update `k+1`, not at release. The sender can only
reuse that capacity after `U` passes the record, which needs the ack of `Y`; the
receiver opened `Y` in update `k`, so anything the sender writes after seeing that
ack is opened in update `k+1` or later (P3), after the slot was freed. The same
argument covers block slots (section 8). `RNMessageEndpoint` models the update
boundary so that this step is checked, not assumed.

**Liveness.** The window never deadlocks. `U` is an unacknowledged record that has
been sent; it is on the resend list and is resent every `rto` until acknowledged,
and on a link that delivers some packets it is eventually acknowledged, which
moves `U`. Held records never hold `U` back, because they are acknowledged when
they arrive.

**Reversed witnesses** (each must fail its module): count only unacknowledged
numbers in `window_count` (step 5 breaks: acknowledged held records escape the
count); let priority reorder first sends within a channel (step 2 breaks); free a
released hold slot at release instead of at the next update (the timing detail
breaks).

---

## 7. Layer 4: the types

### 7.1 The dispatch, and what each type uses

The reliability code is three bits on the wire (study I1). The receipt variants
map to their base codes before writing (`RL.cpp:2619-2628`), so only codes 0 to 4
travel; 5 to 7 are reserved and are violation `CODE` on receipt. The receiver is a
switch on the code, and every type is a case in that switch from the first
version. The first version **enables** two of them at the API; the others are
refused at send with `MESSAGE_SEND_REFUSED_TYPE` until their card lands.

| Value | Type | Wire code | Header | Window number | Channel fields | Hold | Receipt | First version |
|---|---|---|---|---|---|---|---|---|
| 0 | UNRELIABLE | 0 | 3 (1 in SINGLE) | no | no | no | no | **enabled** |
| 1 | UNRELIABLE_SEQUENCED | 1 | 10 (8 in SINGLE) | no | ordering, sequencing, channel | unreliable hold, drop when full | no | refused |
| 2 | RELIABLE | 2 | 6 | yes | no | no | no | refused (but the path runs for CONFIG) |
| 3 | RELIABLE_ORDERED | 3 | 10 | yes | ordering, channel | yes | no | **enabled** |
| 4 | RELIABLE_SEQUENCED | 4 | 13 | yes | ordering, sequencing, channel | yes | no | refused |
| 5 | UNRELIABLE_WITH_ACK_RECEIPT | 0 | 3 (1 in SINGLE) | no | no | no | ACKED or LOSS | refused |
| 6 | RELIABLE_WITH_ACK_RECEIPT | 2 | 6 | yes | no | no | ACKED | refused |
| 7 | RELIABLE_ORDERED_WITH_ACK_RECEIPT | 3 | 10 | yes | ordering, channel | yes | ACKED | refused |

Headers grow by one byte when the channel is 256 or above (the WIDE flag) and by
four for a fragment. RakNet's two commented-out names
(`UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT`, `RELIABLE_SEQUENCED_WITH_ACK_RECEIPT`,
`PP.h:70-81`) never had values and are not reserved here either: a sequenced
record can be discarded after its ack, so a receipt would lie (study I12).

### 7.2 The two that ship first

**UNRELIABLE.** Send: zero copy through `start_message`/`finish_message` (a SINGLE
payload, its own transport packet, sent at the next flush), or copying through
`send_message` (a pool slot, then a record in the next RECORDS packet, aggregated
with every other small queued message on the connection). No number, no channel,
no state after the write: the pool slot is freed when the record is written.
Receive: delivered on arrival, a pointer into the transport's batch. Duplicates
cannot occur (the transport's replay window). Above the record capacity
(1177 bytes of data, section 12.4) a copied UNRELIABLE message is written in
SINGLE form into a transport arena packet and fragments at the transport, keeping
its unreliable semantics (section 8.3).

**RELIABLE_ORDERED.** Send: `send_message` only (the bytes must outlive the first
write, for resends). A pool slot by class, an ordering index at send, the channel
FIFO, a number at first write, the resend list until acknowledged. Above the
record capacity it is a block (section 8). Receive: the window, then the ordered
rule of 6.2.

### 7.3 How the other six slot in

For each type, what its card adds, and the check that nothing below it changes:

- **RELIABLE (2).** Adds: the API accepts it. The record path, the window, the
  resend list and the unordered delivery already run in the first version, because
  the CONFIG record is code 2 (section 12.6); this card exposes a path that is
  already exercised every connection ("no rare codepaths", CLAUDE.md). Sender:
  queued outside any channel, in the reliable heap directly. Receiver: delivered
  when the window accepts it. Wire: none. Per-channel state: none.
- **UNRELIABLE_SEQUENCED (1).** Adds: the send stamping already written in 6.2
  (`ordering = ordered_write`, `sequencing = sequenced_write++`), the receive rule
  already written in 6.2, and the use of the unreliable hold pool, which is
  allocated from the first version (section 13). A held unreliable sequenced
  record that finds the unreliable pool full is dropped and counted
  (`MESSAGE_UNRELIABLE_HOLD_FULL`), which its unreliable contract allows; this pool
  is outside the window bound because these records carry no number. In the
  channel FIFO like any channeled record, so it waits behind an unsent ordered
  record on the same channel, as its delivery would anyway. SINGLE form allowed
  (section 12.3). Wire: code 1 and its fields, reserved from the first version.
  Per-channel state: the two sequenced counters and the hold list, present from
  the first version.
- **RELIABLE_SEQUENCED (4).** Adds: sender stamping as UNRELIABLE_SEQUENCED,
  reliable path as RELIABLE_ORDERED, receive rule as UNRELIABLE_SEQUENCED. A
  stale one is acknowledged and then discarded, which the sender cannot see, as in
  RakNet (study section 10, I12). Its holds are inside the window bound (it has a
  number, and the first-send rule covers it). Wire: code 4. Per-channel state: as
  above.
- **UNRELIABLE_WITH_ACK_RECEIPT (5).** Adds: a pending-receipt entry
  `{packet_id, serial, deadline = write_time + rto}` per written record, ACKED when
  that packet id is acknowledged, LOSS when `advance_time` passes the deadline,
  exactly one of the two (the entry is removed by whichever fires first), as
  RakNet (`RL.cpp:750-770, 1860-1881`). The table and the deadline sweep already
  exist for the reliable path. In SINGLE form, `start_message` knows the packet id
  at once (P5). Wire: code 0, nothing new.
- **RELIABLE_WITH_ACK_RECEIPT (6)** and **RELIABLE_ORDERED_WITH_ACK_RECEIPT (7).**
  Add: ACKED when the record is acknowledged, from the ack handler that already
  frees it. For a block, ACKED when **every** fragment is acknowledged, not when
  the last-index fragment is (RakNet's rule, `RL.cpp:2486-2496`, study section 7.3,
  is a divergence repaired here). Never LOSS; a connection that dies first ends
  every pending receipt with the connection. Wire: codes 2 and 3, nothing new.

**What the first version must already have, so that this list is true,** and so
that a reviewer can check it against the code of the first version: the eight
values in the API enum; the 3-bit code and the dispatch with all five wire cases
(three refused at the API, none refused on the wire); the four channel counters
with both resets; the sorted hold list keyed `(ordering, kind, sequencing)`; the
unreliable hold pool in the allocator; a serial on every record and returned by
every send (study I9); the sent-packet table mapping one packet to many records
(study I8); the deadline sweep in `advance_time`; the receipt list in the API.

**The reviewer's test**, from the card: a type that cannot be added later without
changing the wire or the per-channel state means the design is broken. Section 16
runs that test row by row.

---

## 8. Layer 5: blocks

### 8.1 Reliable messages larger than one record

A reliable message whose data exceeds the record capacity (1177 bytes, 12.4) is a
**block**: split into `count = ceil( bytes / 1024 )` fragments, every fragment but
the last exactly `ROCKETNET_MESSAGE_FRAGMENT_BYTES` = 1024 bytes, the last 1 to
1024. Each fragment is a reliable record of the message's own code (2, 3 or 4) with
the FRAGMENT flag, its index and its count, numbered, resent and acknowledged on its
own, as RakNet does (`RL.cpp:2928-3033`, study section 8.1). All fragments carry the
message's ordering and sequencing indices, so an ordered block takes one ordering
slot (study I11).

**Why per-fragment resend and not the transport's fragments.** The transport would
carry a 16 KB reliable message as 14 fragments under one packet id, and a lost
fragment would cost all 14 again (`sdk/rocketnet.h:830-834`; the survival
arithmetic is S:1892-1896). Per-fragment resend costs only the lost fragment. It
also keeps one accounting for everything reliable: fragments are numbers in the
window like any record.

**Why a fixed 1024-byte stride.** The receiver places fragment `i` at `i * 1024`
the moment it arrives, so it needs no staging buffer and no wait for a stride to
become known (contrast the transport's final-fragment staging,
`sdk/rocketnet_fragment.h:64-71`, and RakNet's arrival-order concatenation, study
section 8.2). 1024 leaves room for the largest fragment header (18 bytes) and the
footer inside 1192, with space over for small records to share the packet.

### 8.2 Numbering, slots and the bounds

- **Consecutive numbers.** When a block's first fragment is first written, the
  sender reserves `count` consecutive numbers for it, and only if the window has
  room for all of them: `next_number + count - U <= N` and
  `window_count[fragment] + count <= window_fragments`. Fragment `i` has number
  `start + i`. So the receiver knows a fragment's block from its own record:
  `start = number - index`. No block id is needed on the wire. The configuration
  must satisfy `ceil( max_message_bytes / 1024 ) <= window_fragments`, checked at
  create.
- **Receive slots.** The receiver has `block_slots` reassembly slots, each
  `max_message_bytes` with a bitmap. A fragment finds the slot holding its `start`,
  or takes a free one. It is checked (count, index, length, total against
  `max_message_bytes`; section 12.7), copied to `index * 1024`, and its bit set. When
  every bit is set the block is complete and is handed to the channel layer as one
  record: delivered at once (unordered, or ordered at `ordered_read`) or held in
  place in its slot (ordered and early). The slot is freed at the start of the
  update after the block is delivered.
- **The sender keeps the slots.** The sender tracks up to `block_slots` blocks as
  "occupying a receiver slot" from the first write of their first fragment until
  `U` passes their last number. A new block starts only when one of those entries
  is free. By the argument of 6.3, once `U` passes a block's last number every
  fragment was received and every earlier record on its channel was received, so
  the block was delivered, and by P3 its slot was freed before anything the sender
  writes next is opened. So a free slot always exists at the receiver for a
  legitimate block, and a fragment that finds none is violation `BLOCK`.
- **Send storage.** A queued block holds a send block slot (`send_blocks` of them,
  `max_message_bytes` each) from `send_message` until all its fragments are
  acknowledged. `send_message` of a block when none is free is refused and counted.
- **Duplicates.** A duplicate fragment is absorbed by the window before it reaches
  the slot, so a slot never sees the same index twice. A slot's `start` is
  compared exactly, so a stale slot can never absorb a newer block's fragments
  (the hazard RakNet has with its reused 16-bit split id, study section 8.2).

Within a channel the block is the FIFO head until its last fragment is first
written, so a large block delays later messages on its own channel only, which they
would be delayed by on delivery anyway. Other channels interleave with it, because
after each fragment the block's heap entry is re-keyed with a fresh weight of its
priority (section 9).

### 8.3 Unreliable messages larger than a packet

An UNRELIABLE (or, later, UNRELIABLE_SEQUENCED or UNRELIABLE_WITH_ACK_RECEIPT)
message too large for a record is never upgraded to reliable, unlike RakNet
(`RL.cpp:1609-1621`). It is written in SINGLE form into a transport packet requested
above `ROCKETNET_MAX_PAYLOAD_BYTES`, so the transport fragments it, and "a lost
fragment loses the whole payload" (`sdk/rocketnet.h:423-430`). The type keeps its
contract: it may be lost, it is never resent, its receipt (type 5) fires ACKED only
when every fragment is acknowledged, because the transport surfaces the id only
then (`sdk/rocketnet.h:830-834`). Through `start_message` it is written straight
into the transport's arena (one copy, the transport's, at split). Through
`send_message` it waits in a send block slot. The limit is the transport's
configured maximum payload less the footer and fields.

**Checked by.** `RNBlocks` (section 14), unit tests for every refusal in 12.7 under
the FRAGMENT flag, a test that loses every fragment once in every order, and a test
that sends `block_slots + 1` ordered blocks on one channel while the first block's
predecessor is lost, and asserts the sender holds the last block back.

---

## 9. Layer 6: scheduling with four priorities

**Owns.** The order in which queued work goes into packets, the packet budget, and
the aggregation of records into RECORDS packets.

**Weights.** RakNet's (`RL.cpp:3926-3949`, study section 5.3), so a RakNet shim
gets the same interleaving under backlog: four priorities, IMMEDIATE, HIGH, MEDIUM
and LOW; a running next-weight per priority seeded `(1 << p) * p + p` and advanced
by `(1 << p) * (p + 1) + p` per push (strides 1, 5, 14, 35), pulled forward to the
heap's minimum when it has fallen behind, and reseeded when the heap is empty.
Priorities outside `0..3` are refused at send (RakNet's `>` test lets 4 through to
index past its array, study section 11 item 20).

**What is in the heaps.** Two min-heaps keyed by weight: a **reliable heap** and an
**unreliable heap**. Each eligible item sits in the one its record's code chooses:
every queued record that names no channel (UNRELIABLE, RELIABLE and their receipt
variants), and the **head** of every non-empty channel FIFO. A channel's head joins
the heap of its own code when a send makes the FIFO non-empty, with its own weight;
when the head leaves the FIFO the next record joins with its own weight. A block
re-enters the reliable heap with a fresh weight of its priority after each
fragment. Each heap is a fixed array of the sender descriptor count.

**Writing a connection's packets** (`write_packets`, one connection, a fill-phase
call). "Writable" means: a resend is due, or the unreliable heap is non-empty, or
the reliable heap is non-empty and its minimum fits the window (6.1; for a block,
all of its numbers, 8.2).

```
for packets in 1 .. max_packets_per_write:
    if nothing is writable: stop
    start_packet( transport, ROCKETNET_MAX_PAYLOAD_BYTES ) -> buffer, packet_id
    if start_packet returned NULL: stop                  (ring or lease pressure, counted by the transport)
    room = capacity - 1                                  (the footer)
    1. resends due, oldest first, while each fits
    2. the lower-weight of the two heap minima (the reliable one only while it
       fits the window), while it fits the room:
           write it; pop; re-key it or advance its channel
    write the footer; finish_packet
    if finish_packet returned 0: endpoint.packet_failed( packet_id )
```

A record that does not fit the room left stops the packet: the packet is finished
and the record goes first in the next one, so weights decide order and nothing
skips ahead to fill a gap. When the reliable minimum does not fit the window, only
the unreliable heap is drawn from, so the reliable window never throttles
unreliable traffic (study I13). An unreliable record queued behind a reliable head
on the same channel waits with it; its delivery would wait for that head anyway.
Small records aggregate naturally: one packet holds as many as fit, up to 64.

**The packet budget** `max_packets_per_write` (default 8) bounds what one connection
puts on the wire per fill phase. There is no congestion window yet: RakNet's
(study section 9) is listed as open, and nothing in the wire forecloses adding one,
because a window lives entirely in the sender.

**IMMEDIATE.** RakNet's IMMEDIATE only wakes its network thread
(`RP.cpp:4218-4222`, study section 5.3). Here the caller owns the cadence, so
IMMEDIATE is a weight and nothing else, and the shim needs nothing more.

**Guarantees.**
- Under a constant backlog every priority makes progress, in the stride ratios
  (no starvation of LOW).
- Within one channel records are first written in ordering-index order.
- Resends due at a write are written before any new record.
- No record is written twice in one packet.

**Checked by.** `RNScheduler` (section 14), unit tests that replay RakNet's weight
sequence for a scripted send pattern and compare, and a backlog test that measures
the interleaving ratios.

---

## 10. Layer 7: the endpoint machine, and the client and server objects

### 10.1 The endpoint machine

One per connection, the composition of layers 2 to 6 plus CONFIG, receipts and
violations. Internal, in `sdk/rocketnet_message.h`; the C API never sees it.

```c
struct rocketnet_message_endpoint_t;    // layer 1 carves it and every array it reaches

struct rocketnet_message_actions_t      // output lists the caller provides and drains
{
    struct rocketnet_message_t * delivered;   int num_delivered;  int delivered_capacity;
    struct rocketnet_receipt_t * receipts;    int num_receipts;   int receipts_capacity;
    int violation;                            // 0, or ROCKETNET_MESSAGE_VIOLATION_*
};

// events
void     rocketnet_message_endpoint_reset( e, double time );
int      rocketnet_message_endpoint_send( e, const uint8_t * data, int bytes, int reliability, int channel, int priority, uint32_t serial );
void     rocketnet_message_endpoint_single_sent( e, int reliability, uint64_t packet_id, uint32_t serial, double time );
void     rocketnet_message_endpoint_begin_update( e );
void     rocketnet_message_endpoint_acked( e, uint64_t packet_id, double time, struct rocketnet_message_actions_t * out );
void     rocketnet_message_endpoint_receive( e, const uint8_t * payload, int bytes, double time, struct rocketnet_message_actions_t * out );
void     rocketnet_message_endpoint_advance_time( e, double time, struct rocketnet_message_actions_t * out );
int      rocketnet_message_endpoint_write_packet( e, uint8_t * buffer, int capacity, uint64_t packet_id, double time );
void     rocketnet_message_endpoint_packet_failed( e, uint64_t packet_id );

// queries
int      rocketnet_message_endpoint_wants_write( const e, double time );
double   rocketnet_message_endpoint_next_deadline( const e );     // the timer, as a value
int      rocketnet_message_endpoint_pending( const e );           // queued plus unacknowledged
```

`send` returns 1 when admitted and 0 when refused (the reason is counted); the
object issues the serial (15). `reset` also queues the CONFIG record. `receive` parses one payload, applies the
window, channel and block rules, and appends what became deliverable. Descriptors
point into the payload (delivered at once) or into a hold or block slot (released);
both are valid until the next `begin_update`. A violation sets `out->violation`
and stops parsing that payload; the endpoint takes no further events but `reset`.

A unit test needs no transport: two endpoints, a scripted link between their
`write_packet` and `receive`, acks fed back by packet id, time held still or
stepped. Every TLA+ counterexample in section 14 becomes one such test.

### 10.2 The host machine

The objects' per-update logic is one more pure machine, so that it too is tested
without sockets. It owns the endpoints' slot table (generation per slot, connected
flag), the delivered list, the receipt list and the connection event list. Its
events are what the transport reports after `update`: a slot's generation and
connected state, each acked `(client_index, packet_id)`, each payload
`(client_index, bytes)`. Its actions: reset this endpoint, disconnect this client
(on a violation), and the lists. On the client there is one slot, reset once at create and
never again (3.1): the client transport delivers payloads before it reports
CONNECTED (S:2208-2214), so CONNECTED is not a reset point.

### 10.3 The objects

`rocketnet_server_t` owns a `rocketnet_server_transport_t`, a host machine and
`max_clients` endpoints (one region per slot, carved by layer 1, so a slot is
yojimbo's per-client allocator). `rocketnet_client_t` owns a
`rocketnet_client_transport_t`, a host machine and one endpoint. Their `update`
is:

```
transport update( time )
host.begin_update()                     every endpoint's begin_update; lists cleared
for each acked (client, id)        -> endpoint.acked                  (the endpoint as it was)
for each payload (client, bytes)   -> endpoint.receive                (the endpoint as it was)
                                      a violation -> disconnect_client, only if the slot's
                                      generation is still the endpoint's
for each slot: generation and connected -> host: on a new generation, DISCONNECTED for the
                                      old connection, reset, CONNECTED for the new one;
                                      on a disconnect alone, DISCONNECTED
for each connected endpoint        -> endpoint.advance_time   (O(1) unless next_deadline <= time)
```

The order matters. One transport update can free a slot and assign it again (a
disconnect or timeout, then a new client's connection response, in one drain). Every
payload and ack id that update reports for the slot belongs to the connection that
held it when the update began, because the new client sends its first payload only
after it has received the server's keep-alive for the new assignment, a round trip
later (P7). So the batch goes to the endpoint as it was, then the generation change
resets it, and a violation found in the old connection's bytes never disconnects
the new client.

Then the application drains messages, receipts and connection events, stages
sends, writes packets and flushes, in the transport's own phase order (U:39-55).
`write_packets` for one connection calls `start_packet`/`finish_packet` on the
transport, so it inherits the fill-phase contract.

The cost per update is a pass over the slots and a pass over the transport's two
batches. Each step is O(1) for an idle slot, so the cost profile is the same with
one client or a thousand (CLAUDE.md, "ALWAYS BE LOADED").

---

## 11. Layer 8: the C API

As it will appear in `sdk/rocketnet.h`, below the transports. `extern "C"`, no C++
type crosses it (REQUIREMENTS.md, "The C API is for C to call"). Names follow the
transport's. The transport stays reachable from the object for its stats, counters
and slot queries.

### 11.1 Constants

```c
// -----------------------------------------
//                                 messages
// -----------------------------------------

// The compile-time channel maximum. A connection's runtime num_channels is at or
// under it. Raise it with -DROCKETNET_MAX_CHANNELS=... (up to 65535); channels at
// 256 and above use the two-byte channel field on the wire, so a stress build at
// 1024 or 10000 changes this number and nothing else.

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                              32
#endif

// RakNet's eight reliabilities, with RakNet's numbering, so a shim passes them
// through. The first version accepts UNRELIABLE and RELIABLE_ORDERED; the others
// are refused at send, counted on MESSAGE_SEND_REFUSED_TYPE, until their layer
// card lands. The values never change.

#define ROCKETNET_UNRELIABLE                                0
#define ROCKETNET_UNRELIABLE_SEQUENCED                      1
#define ROCKETNET_RELIABLE                                  2
#define ROCKETNET_RELIABLE_ORDERED                          3
#define ROCKETNET_RELIABLE_SEQUENCED                        4
#define ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT               5
#define ROCKETNET_RELIABLE_WITH_ACK_RECEIPT                 6
#define ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT         7

// RakNet's four priorities: queue weights under backlog, nothing else.

#define ROCKETNET_PRIORITY_IMMEDIATE                        0
#define ROCKETNET_PRIORITY_HIGH                             1
#define ROCKETNET_PRIORITY_MEDIUM                           2
#define ROCKETNET_PRIORITY_LOW                              3

// The largest message start_message writes as one zero-copy packet: the
// transport's payload capacity less the one-byte footer.

#define ROCKETNET_MAX_SINGLE_MESSAGE_BYTES                  ( ROCKETNET_MAX_PAYLOAD_BYTES - 1 )     // 1191

#define ROCKETNET_RECEIPT_ACKED                             1
#define ROCKETNET_RECEIPT_LOSS                              2

#define ROCKETNET_CONNECTION_EVENT_CONNECTED                1
#define ROCKETNET_CONNECTION_EVENT_DISCONNECTED             2
```

### 11.2 Configuration

One X-macro table, as the transports have (`sdk/rocketnet.h:698-711`): it derives
the struct, the defaults, `ROCKETNET_` environment names, parsing and validation,
with precedence default < struct < env, and validation refuses, never clamps
(U:330-336). Both ends must be configured so that each end's values are at or
under the other's; the CONFIG record checks it (12.6).

```c
#define ROCKETNET_MESSAGE_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                \
    INT_FIELD( num_channels,            32,    1, ROCKETNET_MAX_CHANNELS,   0 )                          \
    INT_FIELD( window_small,           256,    1, 16384,                    0 )                          \
    INT_FIELD( window_medium,           64,    1, 16384,                    0 )                          \
    INT_FIELD( window_large,            32,    1, 16384,                    0 )                          \
    INT_FIELD( window_fragments,        64,    2, 16384,                    0 )                          \
    INT_FIELD( unreliable_hold,         16,    0, 16384,                    0 )                          \
    INT_FIELD( send_small,             512,    1, 65535,                    0 )                          \
    INT_FIELD( send_medium,            128,    1, 65535,                    0 )                          \
    INT_FIELD( send_large,              64,    1, 65535,                    0 )                          \
    INT_FIELD( send_blocks,              4,    1, 255,                      0 )                          \
    INT_FIELD( block_slots,              2,    1, 255,                      0 )                          \
    INT_FIELD( max_message_bytes,    16384, 1177, 16777216,                 0 )                          \
    INT_FIELD( sent_packets,           256,   16, 65535,                    0 )                          \
    INT_FIELD( sent_refs,             1024,   64, 1048576,                  0 )                          \
    INT_FIELD( pending_receipts,        64,    1, 65535,                    0 )                          \
    INT_FIELD( max_packets_per_write,    8,    1, 1024,                     0 )                          \
    INT_FIELD( resend_initial_ms,      250,   10, 10000,                    0 )                          \
    INT_FIELD( resend_min_ms,          100,    1, 10000,                    0 )                          \
    INT_FIELD( resend_max_ms,         1000,    1, 60000,                    0 )

struct rocketnet_message_config_t
{
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

ROCKETNET_EXPORT_FUNC void rocketnet_message_default_config( struct rocketnet_message_config_t * config );

// cross-field rules, refused at create: window_small + window_medium + window_large
// + window_fragments <= 32768; ceil( max_message_bytes / 1024 ) <= window_fragments;
// resend_min_ms <= resend_max_ms. A reliable message is limited by max_message_bytes
// (a block); an unreliable message above 1177 bytes by the transport's configured
// max_payload_bytes less its trailer (8.3).

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_required_memory( const struct rocketnet_message_config_t * config, int max_clients );
```

### 11.3 The objects and their calls

```c
struct rocketnet_server_t;
struct rocketnet_client_t;

// One delivered message. data is valid until the next update -- copy anything you
// keep, as for the transport's batch (USAGE.md, "Consuming what arrived").
// client_index is -1 on the client.

struct rocketnet_message_t
{
    const uint8_t * data;
    int bytes;
    int client_index;
    int channel;                    // -1 for a message that names no channel
    int reliability;                // the wire code it arrived with: 0..4
};

// One receipt: the serial a send returned, and whether it was acknowledged or lost.

struct rocketnet_receipt_t
{
    int client_index;
    uint32_t serial;
    int result;                     // ROCKETNET_RECEIPT_ACKED or ROCKETNET_RECEIPT_LOSS
};

struct rocketnet_connection_event_t
{
    int client_index;
    int type;                       // ROCKETNET_CONNECTION_EVENT_*
    uint64_t client_id;
};

// The zero-copy send's context: the transport's packet context plus what the
// message layer needs at finish. Read-only to the caller.

struct rocketnet_message_context_t
{
    struct rocketnet_packet_context_t packet;
    int reliability;
    int channel;
    uint32_t serial;
};

// --- server ---

ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context, const char * public_address_string,
                                                                           const struct rocketnet_server_transport_config_t * transport_config,
                                                                           const struct rocketnet_message_config_t * message_config, double time );
ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server );        // stop and wait for stopped first
ROCKETNET_EXPORT_FUNC void rocketnet_server_stop( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC int  rocketnet_server_state( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t * rocketnet_server_transport( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server, double time );

// The copying send. Returns the receipt serial (never zero), or zero when the send
// is refused: a bad reliability, channel, priority or size, a client not connected,
// or a full pool -- each counted on its own MESSAGE_SEND_REFUSED_* counter.
// Every accepted send consumes a serial, whatever its reliability.

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_server_send_message( struct rocketnet_server_t * server, int client_index,
                                                               const uint8_t * data, int bytes,
                                                               int reliability, int channel, int priority );

// The zero-copy send, for UNRELIABLE (and later UNRELIABLE_SEQUENCED and
// UNRELIABLE_WITH_ACK_RECEIPT): start_message hands back the transport's own
// 16-aligned frame, write the message at frame + context->packet.payload_index,
// finish_message writes the footer after it and seals it in place. One message,
// one packet, sent at the next flush. Up to ROCKETNET_MAX_SINGLE_MESSAGE_BYTES it
// is a real wire frame with zero copies; above, up to the transport's configured
// maximum less the trailer, it fragments at the transport with one copy and stays
// unreliable. finish_message returns the serial, or zero when the transport
// dropped the packet.

ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, int client_index, int bytes,
                                                                int reliability, int channel, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC uint32_t  rocketnet_server_finish_message( struct rocketnet_server_t * server, struct rocketnet_message_context_t * context, int bytes );
ROCKETNET_EXPORT_FUNC void      rocketnet_server_abort_message( struct rocketnet_server_t * server, struct rocketnet_message_context_t * context );

// Writes one client's queued messages and due resends into packets, up to
// max_packets_per_write, and stages them. A fill-phase call: any game thread,
// one thread per client_index at a time. write_all_packets is the main-thread
// convenience over every connected client.

ROCKETNET_EXPORT_FUNC int  rocketnet_server_write_packets( struct rocketnet_server_t * server, int client_index );
ROCKETNET_EXPORT_FUNC void rocketnet_server_write_all_packets( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC void rocketnet_server_send_packets( struct rocketnet_server_t * server );

// What arrived, valid until the next update: by count and index, or as an array.
// Messages appear in the order they became deliverable; each names its client.

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_messages( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC const struct rocketnet_message_t * rocketnet_server_message( struct rocketnet_server_t * server, int index );
ROCKETNET_EXPORT_FUNC int rocketnet_server_messages( struct rocketnet_server_t * server, const struct rocketnet_message_t ** messages );

ROCKETNET_EXPORT_FUNC int rocketnet_server_receipts( struct rocketnet_server_t * server, const struct rocketnet_receipt_t ** receipts );
ROCKETNET_EXPORT_FUNC int rocketnet_server_connection_events( struct rocketnet_server_t * server, const struct rocketnet_connection_event_t ** events );

// Queued plus unacknowledged messages for a client, so a caller can flush before
// disconnect_client: messages still pending at disconnect are dropped.

ROCKETNET_EXPORT_FUNC int  rocketnet_server_pending_messages( struct rocketnet_server_t * server, int client_index );
ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

// Message-layer counters join the transport's counter enum and its introspection
// (rocketnet_counter_value, rocketnet_counter_name); per-client figures below.

ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_server_counters( struct rocketnet_server_t * server );

struct rocketnet_message_stats_t
{
    float rtt;                      // this layer's smoothed round trip, milliseconds
    float rto;                      // the resend timeout in force, milliseconds
    int queued;                     // waiting for a first write
    int in_flight;                  // numbered, unacknowledged
    int window_used;                // next_number - U
    int held;                       // receive side, waiting for order
};

ROCKETNET_EXPORT_FUNC void rocketnet_server_message_stats( struct rocketnet_server_t * server, int client_index, struct rocketnet_message_stats_t * stats );

// --- client: the same calls minus client_index, plus a copying cursor ---

ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token,
                                                                           const struct rocketnet_client_transport_config_t * transport_config,
                                                                           const struct rocketnet_message_config_t * message_config, double time );
ROCKETNET_EXPORT_FUNC void      rocketnet_client_destroy( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC int       rocketnet_client_state( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t * rocketnet_client_transport( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void      rocketnet_client_update( struct rocketnet_client_t * client, double time );
ROCKETNET_EXPORT_FUNC uint32_t  rocketnet_client_send_message( struct rocketnet_client_t * client, const uint8_t * data, int bytes, int reliability, int channel, int priority );
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int bytes, int reliability, int channel, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC uint32_t  rocketnet_client_finish_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context, int bytes );
ROCKETNET_EXPORT_FUNC void      rocketnet_client_abort_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC int       rocketnet_client_write_packets( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void      rocketnet_client_send_packets( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC int       rocketnet_client_num_messages( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC const struct rocketnet_message_t * rocketnet_client_message( struct rocketnet_client_t * client, int index );
ROCKETNET_EXPORT_FUNC int       rocketnet_client_messages( struct rocketnet_client_t * client, const struct rocketnet_message_t ** messages );

// the copying cursor: pops the next delivered message into a buffer the caller
// owns, returning its length and filling *message (its data pointing into the
// caller's buffer), or zero when drained. a message larger than capacity is
// refused whole and counted, never truncated, as the transport's cursor does.

ROCKETNET_EXPORT_FUNC int       rocketnet_client_receive_message( struct rocketnet_client_t * client, uint8_t * buffer, int capacity, struct rocketnet_message_t * message );
ROCKETNET_EXPORT_FUNC int       rocketnet_client_receipts( struct rocketnet_client_t * client, const struct rocketnet_receipt_t ** receipts );
ROCKETNET_EXPORT_FUNC int       rocketnet_client_pending_messages( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void      rocketnet_client_disconnect( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_client_counters( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void      rocketnet_client_message_stats( struct rocketnet_client_t * client, struct rocketnet_message_stats_t * stats );
```

### 11.4 Counters

Appended to the counter enum by the card that first counts them
(`sdk/rocketnet_counters.h:18-20`: "entries land here in the same change as their
first user"), one per reason:

- send side: `MESSAGE_SEND_REFUSED_TYPE`, `_CHANNEL`, `_PRIORITY`, `_SIZE`,
  `_NOT_CONNECTED`, `_POOL_FULL`, `_BLOCKS_FULL`; `MESSAGE_RECORDS_WRITTEN`,
  `MESSAGE_RESENT`, `MESSAGE_SINGLE_SENT`, `MESSAGE_PACKET_FAILED`,
  `MESSAGE_SENT_TABLE_EVICTED`;
- receive side: `MESSAGE_DELIVERED`, `MESSAGE_DUPLICATE`, `MESSAGE_HELD`,
  `MESSAGE_RELEASED`, `MESSAGE_SEQUENCED_STALE`, `MESSAGE_UNRELIABLE_HOLD_FULL`,
  `MESSAGE_BLOCK_COMPLETED`;
- receipts: `MESSAGE_RECEIPT_ACKED`, `MESSAGE_RECEIPT_LOSS`;
- violations, one per reason in 12.7: `MESSAGE_VIOLATION_FORM`, `_RECORD`, `_CODE`,
  `_CHANNEL`, `_WINDOW`, `_ORDER`, `_BLOCK`, `_CONFIG`, `_RECORD_LIMIT`.

The soak's must-stay-zero set (CLAUDE.md, "Soak at load"): every violation counter
and `MESSAGE_SENT_TABLE_EVICTED` under polite traffic, and the accounting identity
`records accepted = delivered + duplicate + stale + hold_full` per update.

### 11.5 The C# shim, provisionally

From the study's section 13, until the shim's own surface arrives as data:
`Send( data, length, priority, reliability, channel, target, broadcast )` maps to
`send_message` (broadcast is a loop over connected slots, one serial per target, a
shim decision); the serial is RakNet's `uint32` receipt. `Receive()` maps to a walk
of `messages`, `receipts` and `connection_events`, which the shim interleaves into
one stream and prefixes with the RakNet message id byte if the shim reserves it
(open, study section 13). `DeallocatePacket` is a no-op over the per-update batch.
A RakNet channel above `num_channels` is refused here, not mapped to 0; the shim
decides what to do with the refusal.

---

## 12. The wire, to the bit

Written to drop into STANDARD.md as a section of its own, in its voice. All
multi-byte fields are little-endian, byte aligned, written with plain read/write
helpers and no bit packing (CLAUDE.md, "protocol packets are byte aligned").
Section 17.2 says why STANDARD.md is not edited by this card.

### 12.1 Where it lives

A message payload is the plaintext payload of a transport payload packet (type 5)
or of a reassembled fragmented payload (type 9). Its form is read from its **last
byte**, the footer:

| Footer | Form |
|---|---|
| `0x00` | RECORDS |
| `0x80 \| flags`, with flags bits 3 and 5 zero | SINGLE |
| anything else | violation `FORM` |

A payload of zero bytes is violation `FORM`.

### 12.2 The flags byte

| Bits | Name | Meaning |
|---|---|---|
| 0-2 | code | the reliability wire code: 0 UNRELIABLE, 1 UNRELIABLE_SEQUENCED, 2 RELIABLE, 3 RELIABLE_ORDERED, 4 RELIABLE_SEQUENCED; 5, 6, 7 reserved, violation `CODE` |
| 3 | FRAGMENT | the record is one fragment of a block; codes 2, 3 and 4 only |
| 4 | WIDE | the channel field is a u16 rather than a u8; codes 1, 3 and 4 only |
| 5 | CONTROL | the record is a message-layer control record, not application data; code 2 only, never with FRAGMENT |
| 6 | reserved | zero; set is violation `RECORD` |
| 7 | form | zero inside a RECORDS payload; set only in a SINGLE footer |

**Fields the code implies**, in this order, after the length:

| Field | Size | Present for codes |
|---|---|---|
| number | u24 | 2, 3, 4 |
| sequencing | u24 | 1, 4 |
| ordering | u24 | 1, 3, 4 |
| channel | u8, or u16 when WIDE | 1, 3, 4 |
| fragment index | u16 | FRAGMENT |
| fragment count - 1 | u16 | FRAGMENT |

The order is RakNet's (`RL.cpp:2634-2671`, study section 4.2): number, sequencing,
ordering, channel, then the split fields. `F(flags)` below is the total size of the
fields present: 0, 7, 3, 7 or 10 for codes 0 to 4, plus 1 for WIDE, plus 4 for
FRAGMENT.

### 12.3 The two forms

**RECORDS** (footer `0x00`):

```
offset 0                                                             payload_bytes - 1
| record 1 | record 2 | ... | record k |                                    | 0x00 |

record:
| flags u8 | length u16 | fields F(flags) | data (length bytes) |
```

- `k >= 1` and `k <= 64`; zero records is violation `FORM`, more than 64 is
  `RECORD_LIMIT`.
- Records fill the payload exactly up to the footer; a record that would run past
  the footer, or bytes left over that are fewer than a record header, are
  violation `RECORD`.
- A RECORDS payload is at most `ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES` = 1393 bytes
  (`sdk/rocketnet.h:421`); the sender writes at most 1192.

**SINGLE** (footer `0x80 | flags`):

```
offset 0                                                       payload_bytes - 1
| data (payload_bytes - 1 - F(flags) bytes) | fields F(flags) | 0x80 | flags |
```

- Codes 0 and 1 only. Any other code, FRAGMENT or CONTROL is violation `FORM`.
- The data is at least one byte; less is violation `FORM`.
- No length field: the data is everything before the fields.
- The data starts at payload offset 0, where the sender's frame is 16-aligned
  (`sdk/rocketnet.h:816-818`). That is the reason for the footer.
- A SINGLE payload may be any size the transport delivers, including a reassembled
  fragmented payload up to its configured maximum.

### 12.4 Lengths and limits

- `length` is the data length in bytes, at least 1; zero is violation `RECORD`.
- The largest record header without FRAGMENT is 14 bytes (code 4, WIDE: 1 + 2 + 3 +
  3 + 3 + 2). So the largest unfragmented record data the sender writes is
  `1192 - 1 - 14 = 1177` bytes, whatever the code, so that one size rule holds for
  every type. The size classes round that to 1184. A message above 1177 bytes is a
  block (reliable codes) or a SINGLE payload over the transport's fragmentation
  (unreliable codes).
- A fragment's data is exactly `ROCKETNET_MESSAGE_FRAGMENT_BYTES` = 1024 bytes,
  except the last fragment (index = count - 1), which is 1 to 1024. The largest
  fragment header is 18 bytes, so a fragment fits with 149 bytes to spare.

### 12.5 Counters on the wire, and their reconstruction

`number`, `ordering` and `sequencing` are the low 24 bits of 32-bit counters. The
receiver reconstructs each against its expected value `e` (for `number`, the
receive base `B`; for `ordering`, the channel's `ordered_read`; for `sequencing`,
the channel's `sequenced_read`):

```
d = ( wire - e ) mod 2^24
value = d < 2^23 ? e + d : e + d - 2^24          (in 32-bit unsigned arithmetic)
```

so the reconstructed value is the one within half the 24-bit range of `e`. The
sender never puts a value on the wire further than 2^23 from what the receiver
expects (6.1, "Wire widths").

### 12.6 The CONFIG control record

The first record each endpoint writes after reset: the endpoint queues it at reset
and writes it before consulting the heaps in its first `write_packet`, so its number
is 0. Flags `0x22` (code 2, CONTROL). Data, 19 bytes:

| Offset | Field | Size |
|---|---|---|
| 0 | control type, 1 = CONFIG | u8 |
| 1 | layer version, 1 | u8 |
| 2 | num_channels | u16 |
| 4 | window_small | u16 |
| 6 | window_medium | u16 |
| 8 | window_large | u16 |
| 10 | window_fragments | u16 |
| 12 | unreliable_hold | u16 |
| 14 | block_slots | u8 |
| 15 | max_message_bytes | u32 |

The receiver compares every field with its own configuration. A peer value above
this end's, a different `num_channels`, an unknown layer version or an unknown
control type is violation `CONFIG`. The record is delivered to no application. A
CONTROL record with a non-zero number, or a second CONFIG, is violation `CONFIG`.
Sending it on every connection keeps the reliable-unordered path in use from the
first version (7.3, RELIABLE).

### 12.7 The receiver's checks, in order

For each payload: the footer (12.1). For each record, in this order, each with its
violation reason; the first that fails ends the payload and disconnects the peer
(3.4):

1. Header bytes present for the flags; length present; data present. `RECORD`.
2. Reserved bit 6 zero; form bit 7 zero inside RECORDS. `RECORD`.
3. Code at most 4. `CODE`.
4. FRAGMENT only with codes 2-4; WIDE only with 1, 3, 4; CONTROL only with code 2
   and without FRAGMENT. `RECORD`.
5. `length >= 1`. `RECORD`.
6. Channel below this end's `num_channels`. `CHANNEL`.
7. For reliable codes: the reconstructed number below `B + N`. `WINDOW`. A number
   below `B`, or already received, is a duplicate: dropped and counted, not a
   violation.
8. For FRAGMENT: count - 1 at least 1; index at most count - 1; length 1024 unless
   the last index, and 1 to 1024 for the last; `count * 1024` and the implied total
   within this end's `max_message_bytes`; a free or matching slot exists; a
   matching slot's count equal to this fragment's. `BLOCK`.
9. For an ordered record, or a block when it completes: ordering not below
   `ordered_read`. `ORDER`. (A fragment's ordering is reconstructed when it
   arrives and kept in its slot; the check runs once, on the completed block.)
10. For CONTROL: 12.6. `CONFIG`.
11. More than 64 records. `RECORD_LIMIT`.

### 12.8 Worked bytes

A SINGLE UNRELIABLE message of four bytes `DE AD BE EF`:

```
DE AD BE EF 80
```

A RECORDS payload with an UNRELIABLE message `01 02`, then a RELIABLE_ORDERED
message `AA` on channel 5 with number 258 (0x000102) and ordering index 7:

```
00 02 00 01 02                         flags 0x00, length 2, data
03 01 00 02 01 00 07 00 00 05 AA       flags 0x03, length 1, number 0x000102, ordering 7, channel 5, data
00                                     footer
```

Seventeen bytes. The second fragment (index 1) of a 16-fragment RELIABLE_ORDERED
block on channel 300, number 0x000A01, ordering 2:

```
1B 00 04 01 0A 00 02 00 00 2C 01 01 00 0F 00 <1024 data bytes> ...
flags 0x1B = code 3 | FRAGMENT 0x08 | WIDE 0x10; length 0x0400; number; ordering; channel 0x012C;
index 1; count - 1 = 15
```

The CONFIG record at the defaults, alone in a payload:

```
22 13 00 00 00 00                      flags 0x22, length 19, number 0
01 01 20 00 00 01 40 00 20 00 40 00 10 00 02 00 40 00 00
                                       CONFIG v1, 32 channels, windows 256/64/32/64, hold 16,
                                       2 block slots, max message 16384
00                                     footer
```

Twenty-six bytes. The layer-2 card turns these into known-answer vectors.

---

## 13. Sizing: the allocator formula

Per connection, in bytes, from the configuration (`C` is `num_channels`; `M` is
`max_message_bytes` rounded up to 16; `N = window_small + window_medium +
window_large + window_fragments`; `R2(N)` is the power of two at or above `N`):

```
send pools       = 64 * send_small + 256 * send_medium + 1184 * send_large + M * send_blocks
hold pools       = 64 * window_small + 256 * window_medium + 1184 * window_large + 1184 * unreliable_hold
block slots      = block_slots * ( M + 16 + ceil( M / 1024 / 8 ) )
descriptors      = 40 * ( send_small + send_medium + send_large + send_blocks + window_fragments
                        + window_small + window_medium + window_large + unreliable_hold )
channels         = 24 * C
window           = R2(N) + R2(N) / 8
heaps            = 32 * ( send_small + send_medium + send_large + send_blocks + window_fragments )
sent table       = 24 * sent_packets + 8 * sent_refs
pending receipts = 24 * pending_receipts
endpoint struct  = a few hundred bytes, pinned by sizeof

per connection   = the sum
```

Per object, beside the connections:

```
server delivered list = 24 * ( ROCKETNET_MAX_PAYLOADS_PER_UPDATE * 64
                              + max_clients * ( window_small + window_medium + window_large + unreliable_hold + block_slots ) )
server receipt list   = 12 * max_clients * ( send_small + send_medium + send_large + send_blocks + window_fragments + pending_receipts )
client delivered list = 24 * ( ROCKETNET_NUM_CLIENT_PACKETS * 64 + window_small + window_medium + window_large + unreliable_hold + block_slots )
client receipt list   = 12 * ( the same sum, once )
```

The delivered list's bound: each opened payload yields at most 64 records
(12.3), the transport delivers at most `ROCKETNET_MAX_PAYLOADS_PER_UPDATE` payloads
per server update (`sdk/rocketnet_constants.h:53`) and at most
`ROCKETNET_NUM_CLIENT_PACKETS` = 1024 per client update
(`sdk/rocketnet_constants.h:78`, `sdk/rocketnet_client_transport.cpp:222-226`), and
releases from holds and completed blocks add at most the hold and block capacity
per connection. At that size the list cannot fill, so it is never a refusal.

**At the defaults** (32 channels, windows 256/64/32/64, `unreliable_hold` 16, send
512/128/64/4, 2 block slots, 16384-byte messages, 256 table entries, 1024 refs, 64
pending receipts):

| Part | Bytes |
|---|---|
| send pools | 32,768 + 32,768 + 75,776 + 65,536 = 206,848 |
| hold pools | 16,384 + 16,384 + 37,888 + 18,944 = 89,600 |
| block slots | 2 * ( 16,384 + 16 + 2 ) = 32,804 |
| descriptors | 40 * ( 772 + 368 ) = 45,600 |
| channels | 768 |
| window | 512 + 64 = 576 |
| heaps | 32 * 772 = 24,704 |
| sent table | 6,144 + 8,192 = 14,336 |
| pending receipts | 1,536 |
| **per connection** | **416,772 (about 407 KiB)** |

At 1000 clients that is about 397 MiB of connection regions. The server's delivered
list adds `24 * ( 16,000 * 64 + 1000 * 370 )` = 33,456,000 bytes (about 31.9 MiB) and
its receipt list `12 * 1000 * 836` = 10,032,000 (about 9.6 MiB). The client spends
about 407 KiB plus `24 * ( 65,536 + 370 )` = 1,581,744 for its list. For comparison,
the transport's own reassembly arena alone is `max_clients * slots * (max payload +
1387)`, about 136 MiB at 1000 clients and the defaults (`sdk/rocketnet.h:671-680`).

`rocketnet_message_required_memory` computes this before create, as the transport's
`required_memory` does (`sdk/rocketnet.h:722-730`).

**At 1024 and at 10,000 channels** only `channels` changes: 24,576 or 240,000 bytes
per connection, so at 1000 clients 10,000 channels cost about 229 MiB more. Nothing
else scales with the channel count: holds, windows and the heap are per connection,
and a channel with nothing queued and nothing held costs only its 24 bytes. The
stress runs at 1024 and 10,000 change `ROCKETNET_MAX_CHANNELS` and `num_channels`
and nothing in the wire (WIDE, 12.2).

---

## 14. The TLA+ modules

Each beside the code it checks, in `tla/`, driven through `tla/Link.tla` for the
lossy path (loss, duplication, reorder, delay, and the `"first"`, `"alternate"`
and `"burst"` patterns, `tla/README-Link.md`), run with TLC on a small instance on a
bench machine, each with a reversed witness that must fail, rows in `tla/RUNS.tsv`,
and cited from the code that implements it. Every counterexample is checked by hand
against the code before it is written down.

| Module | Layer | Variables | Checks | Reversed witness |
|---|---|---|---|---|
| `RNMessagePools` | 1 | per pool: free list, allocated set | `INV NoDoubleAllocation`, `INV NoFreeOfFree`, `INV CountConserved` (free + allocated = capacity), `INV ResetRestoresAll` | `free` without the allocated check: `NoFreeOfFree` fails |
| `RNReliableWindow` | 3 | sender: `next`, `U`, ring, counts, resend set, sent table; receiver: `B`, bitmap, holds per class; the two Links | `INV ReceiverInWindow` (every arriving new number is below `B + N`), `INV HoldBound` (6.3, per class), `INV DeliveredOnce`, `INV AckMeansReceived` (assumes P1), `LIVE EveryNumberedAcked` | count only unacknowledged numbers: `HoldBound` fails |
| `RNOrderedChannel` | 3, 4 | one or two channels' four counters, FIFOs, hold lists, over `RNReliableWindow` | `INV InOrderExactlyOnce`, `INV HoldBound` with the first-send rule, `INV SequencedResetByOrdered` (both resets), `LIVE EverySendDelivered` | priority may reorder first sends within a channel: `HoldBound` fails |
| `RNSequenced` | 4 | one channel with ordered and sequenced sends, both delivery paths | `INV NoStaleSequenced` (never below the watermark of its epoch), `INV NoDuplicateSequenced`, `INV EpochOrder` | RakNet's heap path (`sequencing` without `+ 1`, no reset on release, study section 11 items 18-19): `NoDuplicateSequenced` or `NoStaleSequenced` fails |
| `RNBlocks` | 5 | sender block entries, fragment numbers, receiver slots and bitmaps | `INV SlotAvailable` (a legitimate fragment always finds a slot), `INV PlacedByIndex`, `INV NoCrossBlockAlias`, `INV BlockDeliveredOnce`, `LIVE EveryBlockDelivered` | free the sender's slot entry when all fragments are acknowledged rather than when `U` passes: `SlotAvailable` fails for an ordered block held behind a lost predecessor |
| `RNReceipts` | 4, 7 | serials, pending receipts, ack and deadline events | `INV AckedOnce` (reliable receipts fire ACKED exactly once, never LOSS), `INV AllFragmentsBeforeAcked`, `INV ExactlyOneOutcome` (type 5: ACKED or LOSS, one) | RakNet's last-index fragment rule: `AllFragmentsBeforeAcked` fails |
| `RNScheduler` | 6 | per-priority weights, the two heaps, channel heads, resend list, the window as a guard | `INV ChannelFirstSendInOrder`, `INV ResendsFirst`, `LIVE NoStarvation` (each priority under constant backlog is written infinitely often) | strict priority without weights: `NoStarvation` fails for LOW |
| `RNMessageEndpoint` | 7 | two endpoints, two Links, update boundaries, slot generation | `INV FreedOnlyAfterUpdate` (a delivered view stays valid until the next update), `INV NoCrossGenerationDelivery`, `INV ViolationDisconnects`, `INV ConfigFirst` (number 0 is CONFIG), `LIVE ConnectionsMakeProgress` under the `"first"` and `"alternate"` patterns | free released storage at release: `FreedOnlyAfterUpdate` fails; reset only on disconnect (no generation), or reset before the update's batch is processed: `NoCrossGenerationDelivery` fails for a reconnect inside one update |

Small instances: two or three messages per direction, `N` of 2 to 4 with two size
classes, one or two channels, one block of two fragments, `MaxDelay` 2, the link's
`Capacity` 3. Each module's card states its instance and its run time.

They stand on `RNReplayWindow` and `RNAckDelivery` from the inventory
(`docs/TLA-INVENTORY.md`, modules 1 and 4), whose `SurfacedOnlyIfOpened` is P1.

---

## 15. Thread rules

Inherited from the transport's fill-phase contract (`sdk/rocketnet.h:841-856`,
U:39-55), with one rule added for the per-connection state:

- **Main thread:** `create`, `destroy`, `stop`, `update`, `send_packets`,
  `write_all_packets`, `disconnect_client` / `disconnect`. Never while a fill is open.
- **Between `update` and the next `update`, any thread, read-only:** the message,
  receipt and connection-event accessors, `pending_messages`, `message_stats`,
  counters. They read lists that only `update` writes.
- **The fill phase** (strictly between `update` and `send_packets`, joined before
  the flush by a synchronizing operation, never a wall-clock wait): `send_message`,
  `start_message`, `finish_message`, `abort_message`, `write_packets`.
- **One writer per connection.** Calls in the fill phase that name the same
  connection (the same `client_index`, or the client) are serialized by the caller.
  Calls naming different connections run concurrently, because each connection's
  state is its own region (layer 1) and the only shared object below them is the
  transport, which is already safe for concurrent fills. The one exception:
  `start_message`/`finish_message` of plain UNRELIABLE touch no connection state
  and may run concurrently with anything, as `start_packet` may.
- **Serials** are per object, from an atomic counter starting at 1 and skipping
  0 on wrap (RakNet's rule, `RP.cpp:1279-1287`), so concurrent sends to different
  connections get distinct serials.
- **The C# smoke's lesson applies unchanged** (U:83-94): replies are staged after
  the drain, in the loop that will drain the answer. A message delivered into an
  update nobody drained is gone at the API surface.

---

## 16. Against the study

### 16.1 The invariants I1 to I17

| Study | Where this design keeps it |
|---|---|
| I1 code in the header, receiver dispatches | 12.2 bits 0-2; 7.1 the switch; 5-7 reserved and refused |
| I2 variable header | 12.2; sizes equal RakNet's (5) |
| I3 two counter families | 6.1 numbers (connection-wide, at first write), 6.2 indices (per channel, at send); unreliable records never take a number |
| I4 message number is not the transport sequence | 6.1: resends are new packets with new ids, same number; the transport dedupes packets, the window dedupes records |
| I5 four counters, coupled, from the first version | 6.2, with both resets, and one rule on both delivery paths |
| I6 hold keyed by (ordering, sequencing) | 6.2 hold list key `(ordering, kind, sequencing)` |
| I7 everything bounded at create; stated full policy | 4, 13; full send pools refuse the send (counted), the receiver provably never fills (6.3), the unreliable hold drops and counts |
| I8 per-packet acks, packet-to-records map | 6.1 sent-packet table, many refs per packet from the first version |
| I9 a serial on every record, returned by the send | 11.3 `send_message` returns it; 15 serial rules |
| I10 unreliable receipt record outlives the frame | 7.3 pending-receipt entries in layer 1, deadline sweep in `advance_time` |
| I11 where large reliable messages are split; by index; bounded; no stale match | 8: in this layer, per-fragment, fixed stride, placed by index, bounded slots, exact `start` match. Unreliable over the transport's fragments (8.3) |
| I12 sequenced acked then possibly discarded | 7.1: no sequenced receipt types; acks clear sender state regardless |
| I13 unreliable not charged to the window | 6.1 window counts numbered records only; 9 only the unreliable heap is drawn when the window is full |
| I14 one RTO per connection | 6.1; the dead-connection decision stays the transport's timeout |
| I15 channel bound per connection, both ends, width from the constant | 6.2 refuse on send; 12.7 step 6 on receive; WIDE in 12.2 |
| I16 the protocol does not share a user channel | CONFIG is a CONTROL record with no channel (12.6); connection start and end are the transport's |
| I17 messages in one stream, batch lifetime | 11.3 `messages` in deliverable order, valid until the next update |

### 16.2 The reviewer's table

The card's test: find a type that cannot be added later without changing the wire
or the per-channel state.

| Type | Wire it needs | Present in the first version's wire? | Per-channel state it needs | Present in the first version? | Other state it needs |
|---|---|---|---|---|---|
| UNRELIABLE_SEQUENCED | code 1; sequencing, ordering, channel; SINGLE with fields | yes: code 1 is a defined case, its fields defined (12.2, 12.3) | `sequenced_write` reset by ordered sends; `sequenced_read` reset by ordered deliveries; hold list with sequenced before ordered | yes (6.2) | unreliable hold pool: allocated from the first version (13) |
| RELIABLE | code 2; number | yes, and in use by CONFIG | none | n/a | none new |
| RELIABLE_SEQUENCED | code 4; number, sequencing, ordering, channel | yes | as UNRELIABLE_SEQUENCED | yes | none new; its holds are inside the window bound |
| UNRELIABLE_WITH_ACK_RECEIPT | code 0 | yes | none | n/a | pending-receipt entries (allocated, 13); the sent table (in use); the deadline sweep (in use for resends) |
| RELIABLE_WITH_ACK_RECEIPT | code 2 | yes | none | n/a | the record's serial (present); the ack handler (in use) |
| RELIABLE_ORDERED_WITH_ACK_RECEIPT | code 3 | yes | as RELIABLE_ORDERED | yes | as above |

At 10,000 channels: the channel field is WIDE (defined from the first version), the
per-channel struct is the same 24 bytes, and nothing per type is per channel except
the counters and list heads above. Nothing is cornered.

### 16.3 Where this design departs from RakNet, deliberately

| # | RakNet | Here | Why |
|---|---|---|---|
| R1 | Priority can reorder first sends within a channel (one heap of all records, `RL.cpp:1676`) | First sends within a channel follow ordering index; priority orders channels and unchanneled messages (6.2) | It is what bounds the receiver's holds (6.3). Delivery is unchanged. The cost: a HIGH message queued behind a LOW one on the same channel waits for the LOW one's turn under backlog; a caller who wants it sooner sends it on another channel. Priority inheritance (the head takes the best weight queued behind it) is a later option inside the sender, with no wire change. |
| R2 | Unbounded heaps, hole queue and output queue (study I7) | Everything bounded; the send refuses when full | No live allocation; the attack RakNet names (`RakNetDefines.h:125-126`) |
| R3 | Unreliable above the MTU becomes reliable (`RL.cpp:1609-1621`) | Stays unreliable, over the transport's fragmentation (8.3) | Keeps the type's contract and needs no layer state; the transport already offers it |
| R4 | Split reassembly by arrival order (study 8.2) | By index, fixed stride (8.1) | RakNet's order is a bug for out-of-order fragments |
| R5 | Split receipt on the last-index fragment (`RL.cpp:2486-2496`) | After every fragment (7.3) | The receipt should mean the message arrived |
| R6 | Heap-path watermark without `+ 1`, no reset on release (study 11.18-19) | One rule on both paths (6.2) | Duplicate and stale delivery |
| R7 | A bad record drops the rest of the datagram silently (`RL.cpp:1491-1494`) | Disconnects the peer, counted (3.4) | Silently dropping an acknowledged reliable record hangs its channel |
| R8 | UNRELIABLE can be delivered twice (study 6.2) | Cannot: the transport's replay window | Free here |
| R9 | Channel out of range silently mapped to 0 (`RL.cpp:1563-1564`) | Refused on send, violation on receive | Study I15 |
| R10 | RTO `2 * RTT + 4 * dev + 30 ms`, cap 2 s; a congestion window | `srtt + 4 * rttvar`, clamped to configured bounds; no window yet | The window is open (17); the RTO is the standard form with explicit bounds |
| R11 | Protocol messages on user channel 0 (study I16) | None: the connection protocol is the transport's | Study I16 |

---

## 17. Build order, open choices, and what is not done

### 17.1 The cards, bottom up

Each card secures its layer before the next starts: its TLA+ module with reversed
witnesses, unit tests on real buffers including every refusal, a second reader's
probes, documented, then locked with Glenn.

1. **Transport preconditions**: the F1 fix (P1) with `RNReplayWindow` and
   `RNAckDelivery`; `rocketnet_server_transport_client_generation` (P4).
2. **Layer 1**, the allocator, with `RNMessagePools` and the sizing function.
3. **Layer 2**, the codec, with the known-answer vectors of 12.8 and the fuzz target.
4. **Layer 3**, the window and channels, with `RNReliableWindow` and
   `RNOrderedChannel`.
5. **Layer 4**, the dispatch with all eight cases and two enabled; the RELIABLE path
   behind CONFIG.
6. **Layer 5**, blocks, with `RNBlocks`.
7. **Layer 6**, scheduling, with `RNScheduler`.
8. **Layer 7**, the endpoint machine, the host machine and the objects, with
   `RNMessageEndpoint` and `RNReceipts` (the receipt list is empty in this version,
   the machinery is checked).
9. **Layer 8**, the C API in `sdk/rocketnet.h`, the C# binding of it, and USAGE.md.
10. **The space game integration**: UNRELIABLE snapshots through `start_message`,
    RELIABLE_ORDERED events through `send_message`, under the swarm (CLAUDE.md,
    "Verification ladder").
11. Then one card per later type, in the order RELIABLE, UNRELIABLE_SEQUENCED,
    RELIABLE_SEQUENCED, then the three receipt types; then the stress runs at 1024 and
    10,000 channels; then the C# RakNet shim against its surface when it arrives.

### 17.2 Open choices for Glenn

- **CONFIG versus identical configuration.** This design has each end send its
  capacities in a reliable CONFIG record and refuse a peer whose values exceed its
  own. yojimbo instead requires both ends to be configured identically and checks
  nothing. CONFIG costs 26 bytes per connection and turns a mismatch into a named
  counter.
- **The 1024-byte fragment stride** versus the largest that fits (1173). 1024 is
  simpler to place and leaves room for small records in a fragment's packet; 1173
  is 13 % fewer fragments.
- **Congestion control.** None in the first version; RakNet's sliding window (study
  section 9) or a rate cap would live entirely in the sender, layer 6.
- **Priority within a channel** (R1): first-send order as designed, or priority
  inheritance later.
- **The size classes** (64, 256, 1184) and the default window counts. They set the
  memory figure of section 13; a lean profile for 1000 players could halve it.
- **The C# shim surface**: provisional until it arrives (study section 13). Whether
  the first payload byte is reserved as RakNet's message id is the shim's question.
- **STANDARD.md.** Its scope line says messages are "carried entirely in payload
  packets, and nothing in them is specified here" (S:8-10, and again at S:69-71).
  Section 12 is written to drop in as its own section, or into a second standard
  for the message tier. Moving that line is Glenn's ruling, so this card does not
  edit STANDARD.md.

### 17.3 Not done

- No code, no TLA+ and no test was written or run. This card is the design.
- The record descriptor (40 bytes) and the endpoint struct sizes are working
  figures for the formula; the layer-1 card pins them with `sizeof` and
  `static_assert`.
- The client transport's per-update drain bound is taken from
  `ROCKETNET_NUM_CLIENT_PACKETS` as its own comment states
  (`sdk/rocketnet_client_transport.cpp:222-226`); a layer-7 card confirms it with a
  test before relying on it for the delivered list.
- The server's per-client drain bound (`ROCKETNET_RECEIVE_PACKETS_PER_CLIENT`, 16,
  `sdk/rocketnet_server_transport.cpp:803-808`) is not relied on; the server-wide
  list is sized by the whole-update bound instead.
- Section 11.5 is provisional until the shim's surface arrives.

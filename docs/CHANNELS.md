# Channels: the message layer above the transports

The design that every message-layer card implements. It covers a client and a
server that send messages over `n` ordering channels with RakNet's eight
reliabilities, chosen per send. Only `UNRELIABLE` and `RELIABLE_ORDERED` ship
first. After them come the client and server objects the space game integrates,
and only then the other six types. This document also shows that adding the
other six later changes neither the wire nor the per-channel state.

Read it beside [RAKNET-RELIABILITY.md](RAKNET-RELIABILITY.md) (cited "the
study", with its invariants written I1 to I17). The transport facts are cited
as `file:line` at origin/main `7314a3d`. `S:n` is STANDARD.md line n and `U:n`
is USAGE.md line n. Layer 1 is built (`sdk/rocketnet_allocator.h`, section 4)
and layer 2 is built (`sdk/rocketnet_message.h`, section 5); nothing above
layer 2 is yet. `ROCKETNET_MAX_CHANNELS` is in `sdk/rocketnet.h`. Names that
are not yet in the tree (`rocketnet_server_t`, the public object surface) are
introduced here.

**Status: layers 1 and 2 are built. The layer list (section 3) is still for
Glenn's correction before layer 3 starts.**

---

## 1. Rules every layer keeps

Each layer section says how it keeps these rules, and section 15 checks them.

- **R1. No live allocation.** Nothing calls malloc or free on a send or receive
  path. Every byte belongs to one of two places: the connection's region, which
  layer 1 carves once at object create from one `rocketnet_malloc`
  (`sdk/rocketnet.h:319-329`), or a transport frame or batch. When a bounded
  thing is full, the rule depends on what it holds. Reliable data stalls the
  sender, and `send_message` refuses (returns 0, counted). Unreliable data is
  dropped and counted. The design never grows anything.
- **R2. Reliability is per send.** Every send names a reliability (RakNet's
  eight values and numbering, study §2), a priority (RakNet's four) and an
  ordering channel. The channel is a number from 0 to `num_channels - 1`.
  `num_channels` is a per-connection runtime count, at most the compile-time
  `ROCKETNET_MAX_CHANNELS` (32 by default; any value up to 65,536 is legal).
- **R3. The transition rule.** No state transition between peers rides a single
  unreliable packet. Any packet that announces a transition the peer must act
  on, such as a channel start, a block start, a reset or a disconnect, goes on
  a reliable path, or it is re-sent on a timer until the peer's next-state
  packet confirms it, as netcode does for its handshake. A lost first packet
  therefore never hangs either side. Section 16.2 shows how this layer meets
  the rule: it has almost no transitions to announce, and the one it has rides
  the reliable path.
- **R4. The pure-module rule.** Every state machine in this layer is a pure
  module. Its state lives in one struct, plus the region that struct points
  into. Events drive it: a parsed payload, an acked packet id, an application
  call, or the current time passed in as a value. It answers with actions the
  caller performs: write these bytes into this frame, deliver this message,
  report this receipt, call me again at time t, disconnect this connection.
  Inside a machine there is no socket, no clock read and no allocation, so a
  unit test can drive it end to end without a socket. Only the objects (layer
  8) bind a machine to a transport and to time (Glenn, 2026-10-05 ~10:30 PM).
- **R5. Copies are few and counted.** On the send side, a zero-copy message
  (`start_message`) costs nothing. `send_message` copies into the
  connection's send store, because no frame exists yet at send time. Writing
  the packet then copies from the store into the transport frame, and each
  resend repeats that write. On the receive side, a message that can be
  delivered when it arrives is handed up as a pointer into the transport's
  delivered batch, with zero copies. A reliable message that has to wait for an
  ordering predecessor, or a block fragment, is copied once into the receive
  ring.
- **R6. A peer that breaks the wire is disconnected, loudly.** A correct peer
  never sends a malformed record. The transport has already acknowledged any
  packet this layer parses (P2), so skipping a bad record could silently lose
  reliable data. Any violation therefore stops parsing that payload, increments
  its named counter, and puts the endpoint into the error state. The object then
  disconnects the connection. This mirrors the transport's own "refuses, never
  clamps" posture (U:333-335).
- **R7. Constant shape.** Every table is sized at create from the configuration.
  Per-update work is O(events), never O(table): there are no full-window scans
  on any hot path. The cost does not depend on load, as CLAUDE.md's "always be
  loaded" rule asks.
- **R8. Every counter is attributable.** Each refusal and drop has its own
  counter. Received records equal the sum of the outcomes for each type, which
  is what the soak harness asserts.

## 2. Layer 0: what the transport gives

These facts come from the code. The design rests on them, and each is
paired with the test or card that keeps it true.

| # | Precondition | Evidence | Kept by |
|---|---|---|---|
| P1 | A payload up to `ROCKETNET_MAX_PAYLOAD_BYTES` = 1192 is one wire packet. `start_packet` hands back a 16-aligned frame, and the payload sits at `frame + payload_index`. | `sdk/rocketnet.h:420`, `:815-821` | existing transport tests |
| P2 | **For an unfragmented payload (type 5), an acked packet id means the payload was in exactly one delivered batch at the peer.** The sequence is marked received at open (`sdk/rocketnet_server_protocol.cpp:791`, `sdk/rocketnet_client_protocol.cpp:711`). The payload is then copied into the batch, which is sized to the receive drain bound, so it cannot overflow (`sdk/rocketnet_server_transport.cpp:982-1018`, `sdk/rocketnet_constants.h:30,53`). The client's receive thread drops before opening when it is full (`sdk/rocketnet_client_transport.cpp:1111`), and its batch has the same capacity (`:407`). The only "acked but not delivered" exits are a malformed ack block or a wrong opened size (`sdk/rocketnet_server_protocol.cpp:806-829`), and a correct peer never sends those. | as cited | `test_transport_acked_unfragmented_was_in_exactly_one_batch` in `sdk/rocketnet_tests.cpp` and `functional/rocketnet_functional_tests.cpp` (card C0b); **F1 must be fixed first** (below) |
| P3 | **For a fragmented payload (type 9), P2 does not hold.** A fragment's sequence is marked received before reassembly can refuse it as stale or displace its slot as abandoned (`sdk/rocketnet_fragment.cpp:250-256, 375-376`). The payload id still surfaces once every fragment is acked (`sdk/rocketnet_ack.cpp:403-415`). So **no reliable record ever rides a fragmented payload.** Large reliable messages are split by this layer (layer 5). | as cited | rule L2.4 below |
| P4 | Acked packet ids arrive once per update, with the client index on the server. They are valid until the next update, and overflow is deferred to a later update, never lost (`sdk/rocketnet.h:966-976`, `:640-646`). An id is a process-wide ticket that never repeats (`sdk/rocketnet_packet_lease.h:7-8`). | as cited | existing |
| P5 | A missing ack does not prove loss: history overflow (`U:283-291`) and burned sequences (`U:126-160`) both leave gaps. This layer treats silence as "resend after the RTO", never as a delivery fact. | as cited | design |
| P6 | **Causality across updates.** If the peer sends anything in reply to a packet this end opened in update *j*, this end opens the reply in update *j+1* or later. Update *j*'s batch is fixed before any packet that update builds can leave. The server takes one `receive()` batch and then processes it (`sdk/rocketnet_server_transport.cpp:968-1045`). The client swaps its receive buffers at the start of `update` (`sdk/rocketnet_client_transport.cpp:466, 563`). | as cited | `test_transport_reply_opens_a_later_update` in `sdk/rocketnet_tests.cpp` and `functional/rocketnet_functional_tests.cpp` (card C0b) |
| P7 | The thread contract: `update` and `send_packets` run on the main thread. `start_packet`, `finish_packet` and `abort_packet` run on any thread strictly between the two, with a join or barrier as the handoff (`sdk/rocketnet.h:841-855`, `U:39-50`). | as cited | inherited, section 16.1 |
| P8 | The client delivers payloads from state 3 (SENDING_CONNECTION_RESPONSE) on (`sdk/rocketnet_client_protocol.cpp:666-675, 826-831`). A client transport makes exactly one connection. | as cited | the client endpoint lives from create (layer 8) |
| P9 | **The server needs one addition, in two parts.** A slot can be released and reassigned inside one update: a disconnect packet releases it (`sdk/rocketnet_server_protocol.cpp:934-944`) and a later response in the same batch can claim it. The batch entries tag only the slot, at receive time (`sdk/rocketnet_server_transport.cpp:1014, 1031, 1103, 1120`; acked ids `sdk/rocketnet_server_protocol.cpp:860`), and acked ids may be deferred to a later update (P4). So no reading of the slot's state after the update, by any accessor, can say which connection an entry belongs to: `is_client_connected`, `client_id` and a connection id read at update end all name the slot's *last* owner. **(a)** `uint64_t rocketnet_server_transport_client_connection_id( server, client_index )`: nonzero, unique for each connection over the transport's lifetime, and 0 while the slot is free. **(b)** Each batch entry carries the connection id of the connection it was received for, written beside `client_index` when the entry is appended: `rocketnet_server_transport_payload_connection_ids( server, const uint64_t ** connection_id )` and `rocketnet_server_transport_acked_connection_ids( server, const uint64_t ** connection_id )`, parallel to the existing arrays. (a) tells the object who owns the slot now; (b) tells it whose an entry is. The object compares them when it applies an entry (10.2). | as cited | **transport card C0a** |

**F1 blocks RELIABLE_ORDERED.** `sdk/rocketnet_replay.cpp:54-60` still clears
only 4 of the window's 64 words on a forward jump of 4096 or more
(`docs/TLA-INVENTORY.md:30-73`). The next ack block then names sequences that
were never opened. To this layer, a false ack is a reliable record marked
delivered that the peer never received, so a channel hangs for good. The fix
(a `memset` of the window) is card C0. Nothing reliable ships before it.

## 3. The layers, bottom up

Each row is one layer, and each layer is secured before the one above it
starts.

| L | Layer | Owns | Guarantees | Checked by |
|---|---|---|---|---|
| 1 | **Region** (the allocator) | One block per connection: fixed tables, the send store (three slab classes) and the receive ring | No allocation after create. Every pointer it hands out stays inside the region. A slab allocates and frees in O(1). It refuses when full. | unit tests, including every refusal |
| 2 | **Records** (the wire) | The packet forms SINGLE and RECORDS, the record codec, and the receiver's checks in order | Encoding then decoding is the identity. Every malformed input maps to exactly one violation counter. SINGLE costs one byte. | known-answer vectors (section 13.6); a fuzz target |
| 3 | **Reliable window** | Connection-wide message numbers, the window `W`, the ring floor, the sent-packet map, RTO and resend, and the receiver's bitset and descriptors | Every reliable record is delivered once (given P2 and P6). The receiver never needs room it does not have. Its slot and ring writes never touch a live message. | `RNWindow`, `RNRing` |
| 4 | **Channels and dispatch** | Four counters per channel on each side, the dispatch of all eight reliabilities, held ordered messages, the newest held sequenced message | Ordered delivery is exactly-once and in order. Sequenced delivery never goes backwards. The receipt types are their base types plus bookkeeping. | `RNChannel`, `RNReceipts` |
| 5 | **Blocks** | Splitting large reliable messages at a fixed stride: consecutive numbers, a contiguous ring span, and completion | A block is delivered whole, once, at its single ordering slot, with no reassembly copy. | `RNBlocks` |
| 6 | **Scheduler** | Four priority levels with RakNet's strides, round robin of channels within a level, FIFO within a channel, and the packet builder that aggregates | Within a channel, the first send of each record follows ordering order. No level starves. A stalled window never blocks unreliable traffic. | `RNScheduler` |
| 7 | **Endpoint machine** and **slots machine** | One connection's composition of layers 3 to 6, its error state and deadline; the server's slot-to-endpoint map | The pure-module rule. End to end, over a lossy link, delivery is exactly-once for reliable, at-most-once for unreliable, and no state hangs. | `RNEndpoint`, `RNSlots`; socketless unit tests |
| 8 | **Objects** | `rocketnet_server_t` and `rocketnet_client_t`: transport plus endpoints plus regions plus time | The update order, the thread rules, the delivered list's lifetime | functional tests over loopback; the swarm soak |
| 9 | **C API** | `sdk/rocketnet.h`, `extern "C"`, no C++ types | The surface section 12 lists, usable from C#. | binding smoke; header compile test |

The order of work is in section 18.

## 4. Layer 1: the region

**Owns.** Each connection (each server slot, and the client) gets one
contiguous region, carved at object create from a single `rocketnet_malloc`
call per object. The server makes one allocation of `max_clients × region`, so
no slot ever allocates again. A region holds:

1. **Fixed tables**, each an array sized from the configuration: the
   per-channel state (`C` entries), the sender's per-number table (`W`), the
   receiver's descriptors (`W`) and received bitset (`W` bits), the
   held-ordered index (`2W`), the send descriptors (`Q`), the sent-packet ring
   (`P`) and its reference ring (`16P`), the delivered entries (`W + Du`), the
   sequenced holds (`Ru`), and the receipt ring (`Rq`). Section 14 gives each
   size.
2. **The send store.** A slab of `S` bytes divided equally by bytes into three
   classes of fixed chunks: 128, 512 and 1152 bytes, where 1152 is the block
   stride `F`. Each class keeps an intrusive free list, so allocate and free are
   both O(1). There is no fragmentation and no coalescing. A message of up to
   1152 bytes takes one chunk of the smallest class that fits. A larger message
   is a block (layer 5) and takes `ceil(bytes / F)` chunks of the 1152 class,
   chained, one chunk per fragment. A send the store cannot satisfy is refused
   whole.
3. **The receive ring.** `B` bytes with no allocator logic at all. Placement is
   decided by the *sender*: each holdable record carries its ring offset (layer
   3). The receiver only checks bounds.

**Guarantees.** After create the region never changes size. Every pointer the
region hands out lies inside it. Allocation and free are O(1). Refusal is
explicit and counted: `MESSAGE_SEND_STORE_FULL` and
`MESSAGE_SEND_QUEUE_FULL`. Region memory is carved with 64-byte alignment
(`ROCKETNET_ALLOCATION_ALIGNMENT`), so the ring base is 16-aligned and every
ring placement, being a multiple of 16, is 16-aligned too.

**Checked by.** Unit tests that fill every class to refusal, free in random
order and refill, and assert the free-list counts; a carve test that asserts
every table is in bounds for the minimum, default and maximum configurations;
and assert that a class never double-allocates or loses a chunk.

**As built** (`sdk/rocketnet_allocator.h` and `.cpp`, card C1). The code taught
the design these points:

- **Allocated at create, carved at connect.** An *arena* is the object's one
  `rocketnet_malloc`: its header, its region structs, then `max_clients`
  regions (one for a client) back to back, each 64-aligned. `rn_region_carve`
  runs at every connect in the region's own memory and allocates nothing. It
  resets every fixed table, rethreads the store's and the descriptor pool's
  free lists, and leaves the ring's and the chunks' bytes alone, because every
  reader of them writes first. The carve is O(tables), once per connection,
  never on a send or receive path.
- **The send descriptors are layer 1's.** The `64·Q` term is a pool with an
  intrusive LIFO free list. `rn_region_send_alloc` takes a descriptor and its
  store chunks together, or neither. A full pool counts
  `MESSAGE_SEND_QUEUE_FULL`; a full store counts `MESSAGE_SEND_STORE_FULL`.
  Each counts once per refused send, on the owner's counters, atomically,
  because an object's regions share them. A size outside 1 to
  `max_message_bytes` is the caller's refusal to count (`MESSAGE_SEND_REFUSED`,
  layer 8) and takes nothing here.
- **Each class's links live inside its third.** The thirds are 64-aligned
  (`floor(S/3)` rounded down to 64). Each holds its chunks, then one u32 link
  per chunk, so a class has `floor(third / (chunk + 4))` chunks and nothing
  lies outside `S`.
  - At the defaults the classes are 330 × 128, 84 × 512 and 37 × 1152.
  - A free chunk's link names the next free chunk.
  - An allocated chunk's link carries `IN_USE`, `HEAD` on a chain's first
    chunk, and the next chunk of its block. A block's chain therefore costs no
    bytes, and a double free is an exact refusal in every build, not only an
    assert. The send-store tests exercise this refusal.
- **No fallback to a larger class.** A message takes the smallest class that
  fits, or it is refused. Fallback would make one class's exhaustion drain the
  others and blur which counter means what.
- **At the defaults, `MESSAGE_SEND_QUEUE_FULL` cannot fire.** The store's 451
  chunks run out before the 1024 descriptors can. The queue bound only binds
  when `Q` is below the store's chunk count (for example a client with
  `send_queue_messages = 64`, which is how the soak reaches it). Whether the
  default `Q` should drop to the store's chunk count is choice G7 (18.2).
- **The lean profile is refused by the store rule.** At `S = 65536` the large
  class holds 18 chunks. The default `max_message_bytes` of 32768 needs 29.
  The lean profile therefore also needs `max_message_bytes <= 20,736`.
- **One validation rule more:** `rto_min_ms <= rto_initial_ms <= rto_max_ms`.
  The clamp in 6.4 needs `min <= max`, and a first timeout outside the clamp
  has no meaning.
- **The exact region** is the formula plus at most 63 bytes of alignment per
  part. At the defaults the padding is 0, and the region is 449,024 bytes,
  which is section 14's figure to the byte.
- **Every entry type is pinned** to its section 14 size by a static assert.
  `rn_delivery_t` holds its pointer in a union with a u64, so it is 24 bytes on
  a 32-bit target too.
- **The sizing is callable before connect.**
  - `rocketnet_message_connection_bytes` returns the region.
  - `rocketnet_message_connection_sizing` writes the formula, each term at the
    config, the store's classes, the padding, the region and the total for `n`
    connections. It writes into a caller's buffer, with snprintf's contract,
    and allocates nothing.
  - `rocketnet_message_print_connection_sizing` logs the same lines at info
    level.
- **The gate:** `test_channels_allocator_no_live_allocation`, in
  `test_channels.cpp`, run by `./bin/test`.
  - **What it traps.** It fails at the first call, while armed, to any of the
    rocketnet allocator hooks, the global operator new or delete, or, on glibc
    without a sanitizer, `malloc`, `free`, `calloc`, `realloc` or the aligned
    forms.
  - **What it soaks.** A 16-slot server arena and 16 one-region client arenas
    at full configured load. Every carve-out is driven to capacity each round,
    with a reconnect every 20th tick.
  - **The stand-in.** The soak drives layer 1's carve-outs the way layers 3 to
    8 will. Layer 2 holds no state and allocates nothing (section 5). Each
    later layer must run its own send and receive paths under the same traps
    when it lands. The two size macros (`ROCKETNET_MESSAGE_SINGLE_BYTES`,
    `ROCKETNET_MESSAGE_FRAGMENT_BYTES`) are defined once in `sdk/rocketnet.h`;
    the codec header uses those.

## 5. Layer 2: records in a packet

The transport payload this layer writes has one of two forms. Its **last byte**
(the *form byte*) says which. Putting the form byte last is what makes the
zero-copy form possible: the application's message starts at payload offset 0,
which is the transport's 16-aligned root, and nothing comes before it.

- **SINGLE** (`0x00`): bytes `[0, L-1)` are exactly one UNRELIABLE message.
  The cost is one byte. This is the zero-copy form. The application writes
  directly into the transport frame, and `finish_message` writes the form byte
  after the message and calls `finish_packet`. It is also the form for a large
  unreliable message sent zero-copy: the transport's arena route fragments it
  (P1, P3).
- **RECORDS** (`0x01`): bytes `[0, L-1)` are one or more records back to back,
  each a header then data. This is the aggregation form. Many small unreliable
  messages, reliable records, resends and the CONFIG record share one packet.

The record header is a flags byte, a u16 length, then only the fields the
record's code needs, in the order RakNet uses (study §4.2, I2). The layout to
the bit is section 13.

| Code | Reliability on the wire | Fields after flags and length | Header bytes |
|---|---|---|---|
| 0 | UNRELIABLE (and 5, as sent) | none | 3 |
| 1 | UNRELIABLE_SEQUENCED | sequencing, ordering, channel | 10 |
| 2 | RELIABLE (and 6) | number | 6 |
| 3 | RELIABLE_ORDERED (and 7) | number, ring, ordering, channel | 12 |
| 4 | RELIABLE_SEQUENCED | number, ring, sequencing, ordering, channel | 15 |

The BLOCK flag adds `ring` (on code 2), then fragment index and fragment count,
for +4 or +6 bytes. The WIDE flag makes the channel a u16, for +1 byte.

Rules of this layer:

- **L2.1** As in RakNet, the receipt variants never reach the wire
  (study §2): 5 is sent as 0, 6 as 2 and 7 as 3. Codes 5 to 7 on the wire are
  violations.
- **L2.2** Every packet that carries a reliable record is a RECORDS packet of
  at most 1192 bytes, so the transport sends it as an unfragmented type 5
  (P2). A reliable message longer than one record can hold is a block.
- **L2.3** An unreliable message (code 0 or 1) that is longer than one packet
  can hold travels alone, in a payload above 1192 bytes that the transport
  fragments. With `send_message` that payload is a RECORDS packet holding one
  record, so the u16 length caps it at 65,535 bytes. With `start_message` it
  is a SINGLE packet, capped by the transport's `max_payload_bytes`. Losing one
  fragment loses the message, which is what unreliable means. RakNet upgrades
  such a message to reliable (study §5.2 step 4). This layer does not, except
  for the receipt type (section 7.4).
- **L2.4** No reliable record and no CONTROL record ever rides a fragmented
  payload (P3).

**Checked by.** Known-answer vectors (section 13.6), a round-trip property test
over every flag combination, and a fuzz target on the decoder. Each malformed
input must hit exactly one violation counter and never read past `L-1`.

**Built.** `sdk/rocketnet_message.{h,cpp}` is this layer: the record codec, a
writer that builds a RECORDS payload in place in a frame, the SINGLE finish,
and a reader that runs checks 1 to 9 of 13.3 over the whole payload before it
hands out any record, plus the CONFIG body codec (check 12). The layer holds no
state, so no TLA+ module covers it. Its tests are `test_channels.cpp` in
`./bin/test`: the 13.6 vectors both ways, one input per violation, SINGLE in
place at a 16-aligned payload root, and a pack/parse property test over every
prefix and one-byte mutation of 4,000 generated payloads
(`ChannelsMessageRecordRoundTripAndNeverOverreads`). The fuzz target is
`fuzz/rocketnet_fuzz_message.cpp`, seeded from `fuzz/corpus_message`. The code
taught the design four things, now in 13.2 and 13.3:

- The header's fields are defined for every flags byte, valid or not, so
  check 2 (the header fits) can run before check 3 (the flags are allowed)
  without guessing what a refused code would carry.
- WIDE below channel 256 is refused (check 5). Without that a record had two
  encodings, and a parsed payload would not always pack back to its own bytes.
- Check 6 admits a wire code when an enabled reliability travels as it (7
  admits 3), and a SINGLE payload is code 0 for it.
- Check 8 for a block measures the whole block from its base: a fragment that
  is not the last knows the block's end only to within the last fragment's
  length, so it checks the shortest block it could be part of, and a base
  below 0 is a span outside the ring.

## 6. Layer 3: the reliable window

### 6.1 What it owns

**The sender** owns:

- `next`, the next message number, reconstructed as u64 and written as u24.
- `U`, the oldest unacked number, and `U'`, the window base (section 6.2).
- `ring_next`, the absolute byte position, u64.
- The per-number table of `W` entries, indexed `number % W`:
  `{ number, descriptor, fragment, ring_start, ring_end, last_send, acked }`.
- The **resend FIFO**: a list of unacked numbers ordered by last send time.
- The **sent-packet ring** of `P` entries,
  `{ packet_id, send_time, ref_first, ref_count, unreliable_receipts }`, plus a
  `16P` reference ring of numbers and a `2P` open-addressed index from
  `packet_id` to entry.
- The RTT estimator `{ srtt, rttvar, rto, backoff }`.

**The receiver** owns:

- `R`, the lowest number not yet received.
- The received bitset of `W` bits.
- The descriptors of `W` entries, indexed `number % W`:
  `{ number, live, code, channel, ordering, sequencing, ring, bytes, block_start, block_have }`.
- The held-ordered index: `2W` open-addressed entries keyed by
  `(channel, ordering)` that point to a number.

### 6.2 Numbering is late, and bounded twice

As in RakNet, a record takes its number when it is **first written into a
packet**, never at `send_message` (study §5.2, I3, the reason at
`RL.cpp:1024-1027`). Unreliable records never take a number (I3).

The sender may give the next number `n` to a record only when both of these
hold:

- **Count bound:** `n < U' + W`.
- **Byte bound** (only for a *holdable* record, meaning code 3, code 4, or any
  BLOCK): the record gets the ring span `[p, p + len16)`, where `len16` is its
  length rounded up to 16. `p = ring_next` if that span does not cross a
  multiple of `B`; otherwise `p` is the next multiple of `B`, and the skipped
  bytes are padding. The span is admitted only if `p + len16 - floor <= B`.
  `floor` is `ring_start(U')` when `U' < next`, and `p` when nothing is
  outstanding. A block reserves the whole span of all its fragments at once
  (layer 5).

Here `U` is the lowest unacked number. `U' = U`, except when `U` falls inside a
block that is not fully acked; then `U'` is that block's first number. A code 2
record that is not a block is never held by the receiver, so it takes a number
but no ring bytes.

If the next record fails either bound, the sender stops numbering new reliable
records until acks move `U'`. Resends and unreliable records keep flowing
(I13). No record waits forever: when nothing is outstanding, any message of at
most `max_message_bytes <= B` and at most `W` fragments fits (liveness,
checked by `RNRing`).

### 6.3 Why the receiver never runs out of room

The transport acks a packet when it opens it, before this layer sees the
payload (P2). So a receiver that has to drop an acked reliable record has lost
it, because the sender has already been told it was delivered. This design
guarantees the receiver always has room. The argument, which `RNWindow` and
`RNRing` model-check:

1. **Live numbers.** At the receiver, a number is *live* while it is held
   (waiting on an ordering predecessor, or part of an incomplete block) or
   while it is delivered in the current update. Claim: no two live numbers
   share a slot `n % W`.
2. Suppose `n` is live and `m = n + kW` (k ≥ 1) arrives. The sender numbered
   `m` only while `m < U'_s + W`, so `U'_s > n`. Every number at or below `n`,
   and every block containing one, was then fully acked, so by P2 the receiver
   had opened all of them. Because the scheduler first-writes records in
   ordering order within each channel (layer 6), every ordering predecessor of
   `n` has a number below `n`. So when the ack that moved `U'_s` was built, `n`
   was already releasable, and it was released in that update (layer 4 drains
   releasable holds to completion). By P6, `m` arrives in a later update, after
   that update's delivered list expired. So `n` is not live. Contradiction.
3. **Ring.** The same argument applies with byte positions. The sender places
   `m` only if `ring_end(m) - ring_start(U'_s) <= B`. Any message whose ring
   span `m`'s span overlaps (mod `B`) therefore lies below `U'_s`, and by step
   2 it is no longer live.
4. **Delivered list.** Each update delivers only live numbers, and they occupy
   distinct slots (step 1). So reliable deliveries per update are at most `W`,
   and the delivered list reserves exactly `W` entries for them. Unreliable
   deliveries get a separate budget `Du`. When `Du` is full the rest are
   dropped and counted (`MESSAGE_UNRELIABLE_DELIVERY_FULL`), which unreliable
   semantics allow.

The receiver still checks every claim it can check: the number is inside the
window, the slot is not held by a different live number, and the ring span is
inside `B`. A sender that breaks the bounds is a violation (R6), never memory
corruption.

### 6.4 The sent-packet map, acks and resend

- **Commit.** After each RECORDS packet is written, the sender records
  `{packet_id, now, refs}`: the numbers the packet carries and any
  unreliable-receipt serials. Like RakNet's `datagramHistory`, this maps a
  packet to its records (I8), and a packet holds mixed reliabilities from the
  first version.
- **Ack.** For each acked `packet_id` the sender finds the entry through the
  index. It takes one RTT sample from `now - send_time`; every packet id is
  unique, resends included, so samples are unambiguous (I4). Then, for each
  reference `r`: if the per-number entry at `r % W` still names `r` and is
  unacked, it marks it acked, frees that fragment's chunk, and fires any
  receipt (section 7.4). After that it advances `U` past acked numbers and
  recomputes `U'`. An id that is not in the map is ignored at O(1): a SINGLE
  packet, an evicted entry, or a packet carrying only unreliable records.
- **Eviction.** When the sent ring or the reference ring is full, the oldest
  entry is evicted. Its unreliable receipts fire LOSS, and its numbers keep
  their per-number state, so they are resent on the RTO. Eviction costs
  bandwidth, never correctness, and is counted
  (`MESSAGE_SENT_MAP_EVICTED`).
- **RTO.** `srtt` and `rttvar` follow RFC 6298 over this layer's own samples,
  not the transport's rtt floor (`U:216-235` explains why that floor cannot
  drive a timeout). `rto = clamp( srtt + 4·rttvar, rto_min, rto_max )`; before
  the first sample it is `rto_initial`. The connection-wide `backoff` doubles
  each time an RTO fires with no ack in between, up to 8, and resets on any
  ack. RakNet has no back-off (study §9); this departure is deliberate.
- **Resend.** The resend FIFO is ordered by last send. In `write_packets`, the
  head is due when `now >= last_send + rto·backoff`. Due records are rewritten
  first, each in a new packet with a new id, carrying the same number, ring and
  ordering fields. Each is then moved to the tail. The walk stops at the first
  record that is not due, so the work is O(due).
- **Failed send.** If `finish_packet` returns 0, the numbered records in that
  packet are marked due now and the packet's unreliable records are dropped and
  counted.
- **Giving up.** The transport's idle timeout is the only dead-connection
  decision (I14). On top of it, if the oldest unacked record has gone
  `reliable_timeout` without an ack while the connection stays up, the endpoint
  enters the error state with reason `RELIABLE_TIMEOUT` and the object
  disconnects. That catches a live peer whose message layer is broken. Without
  it the window would stall silently.

### 6.5 The receiver's window check

The 24-bit number is reconstructed against `R` by half range: `n = R + δ`,
with δ the signed 24-bit difference. Then:

- `n < R`, or the bit for `n` is already set: a duplicate. It is dropped and
  counted (`MESSAGE_DUPLICATE`). This is normal after a resend.
- `n >= R + W`: violation `MESSAGE_NUMBER_OUTSIDE_WINDOW`.
- The descriptor at `n % W` is live and names a different number: violation
  `MESSAGE_SLOT_COLLISION`.
- Otherwise the bit is set. While the bit at `R` is set, the receiver clears
  it and increments `R`. Clearing frees the bit for the number `W` above.
  The cost is O(advance).

Congestion control is not in this design (section 18.2, choice G3). The
sender's numbering is bounded by `W` and `B`, and each update by
`max_packets_per_update`.

## 7. Layer 4: channels and per-send reliability

### 7.1 Per-channel state, all of it from the first version

Every channel has RakNet's four counters (study §3.2, I5), sized for every type
from day one. In the first version two of them go unused, and they are never
removed or resized:

```c
struct rn_channel_send_t            // 32 bytes
{
    uint32_t ordered_write;         // next ordering index for an ordered send
    uint32_t sequenced_write;       // next sequencing index; zeroed by every ordered send
    uint32_t queue_head, queue_tail;// FIFO of queued send descriptors (layer 6)
    uint16_t queued[4];             // queued count per priority (layer 6)
    uint32_t ready_prev, ready_next;// this channel's link in its level's ready list (layer 6)
};

struct rn_channel_receive_t         // 16 bytes
{
    uint32_t ordered_read;          // next ordering index to deliver
    uint32_t sequenced_watermark;   // lowest sequencing index still deliverable in this epoch
    uint32_t held_sequenced;        // descriptor of the newest held sequenced message, or none
    uint32_t pad;
};
```

The coupling is RakNet's (`RL.h:423-439`), applied on **one** path:

- **Send ordered** (code 3): `ordering = ordered_write++`, then `sequenced_write = 0`.
- **Send sequenced** (code 1 or 4): `ordering = ordered_write`, `sequencing = sequenced_write++`.
- **Deliver ordered** `(o)`: `ordered_read = o + 1; sequenced_watermark = 0`.
- **Deliver sequenced** `(o, s)`: only if `o == ordered_read` and
  `s >= sequenced_watermark`; then `sequenced_watermark = s + 1`.

A single function performs both "deliver" steps, and it runs whether a message
is delivered as it arrives or released from a hold. So the study's items 18 and
19 cannot happen: there is no second path to drift. The first version runs the
`sequenced_write = 0` and `sequenced_watermark = 0` resets even though no
sequenced type is enabled yet. Enabling sequenced later therefore changes no
ordered trace, and `RNChannel` checks that.

### 7.2 Dispatch per record

The decoder (layer 2) gives a record with a code. Layer 3 has already accepted
it, if it is reliable and not a duplicate. Then:

| Code | Action |
|---|---|
| 0 | Deliver now: a pointer into the batch, from the unreliable budget. |
| 1 | If `o == ordered_read` and `s >= watermark`, deliver now. If `o > ordered_read`, hold it as the channel's newest sequenced message (7.3). Otherwise drop it as stale (`MESSAGE_SEQUENCED_STALE`). |
| 2 | Deliver now (unordered). |
| 3 | If `o == ordered_read`, deliver now, then **drain** (below). If `o > ordered_read`, copy into the ring at its offset, insert `(channel, o)` into the held index, and keep the descriptor live. If `o < ordered_read`, it is a violation (`MESSAGE_ORDERING_BEHIND`), because a reliable ordered record can only be behind if it is a duplicate, and layer 3 already caught those. |
| 4 | Same as code 1, except the held copy lives in the ring at its offset. |
| CONTROL | Go to the endpoint (section 10.3). Never delivered. |

**Drain** runs after each ordered delivery on channel `c`:

1. If `held_sequenced[c]` has ordering `ordered_read`, deliver it if
   `s >= watermark`. If its ordering is below `ordered_read`, release it as
   stale.
2. Look up `(c, ordered_read)` in the held index. If it is present, deliver it,
   unlink it, and go back to step 1.

Each step is O(1), and the drain is bounded by the number of held messages.

The receipt variants (5, 6, 7) arrive as 0, 2 and 3 and are dispatched exactly
as those codes. A receipt is the sender's affair (study §2, I9).

### 7.3 Sequenced messages ahead of their epoch

RakNet buffers every sequenced message that arrives ahead of its epoch, then
releases them in sequence order (study §6.2, I6). This design keeps only the
**newest** one per channel, ordered by `(o, s)`, and drops the others, counted
as `MESSAGE_SEQUENCED_SUPERSEDED`. Sequenced semantics allow this: "never older
than what was delivered", with drops permitted. A state stream gets its newest
state as soon as the epoch opens. The storage depends on the code:

- Code 4 (reliable) is held in the ring at its own offset, which the sender
  already counted against `B`.
- Code 1 (unreliable) is held in one of `Ru` hold slots of 1192 bytes each.
  When the slots are all in use, it is dropped and counted
  (`MESSAGE_SEQUENCED_HOLD_FULL`).

This is the study's I6 point. A buffer keyed only by ordering index could not
hold a sequenced message ahead of its epoch. Here the per-channel
`held_sequenced` field and the `Ru` pool exist from the first version, so
enabling codes 1 and 4 adds no state.

### 7.4 Receipts

Every `send_message` returns a nonzero u32 serial, whatever the reliability.
The object issues serials (I9) from one per-object counter that skips 0. The
serial is stored in the send descriptor. The receipt types differ from their
base types only by a flag on that descriptor:

| Type | ACKED when | LOSS when |
|---|---|---|
| 6, 7 | every number the message took is acked. A block waits for all of its fragments, not for the last index as RakNet does (study §7.3, item 5). | never. The connection dies instead (study §7.3). |
| 5, single packet | the `packet_id` of the RECORDS packet that carried it is acked | its sent-map entry is evicted, or reaches `send_time + rto` without an ack. A sweep cursor walks the sent ring in send order, fires LOSS for each expired entry's unreliable receipts and clears them. It costs O(expired). The entry itself stays, so its reliable references still match a late ack. |
| 5, larger than a packet | sent as a block of type 6, as RakNet does (`RL.cpp:1615-1616`), so ACKED only | never |

Receipts go into the connection's receipt ring of `Rq` entries and then into
the object's per-update receipt list. When the ring is full, the oldest
receipt is dropped and counted (`MESSAGE_RECEIPT_RING_FULL`). An ACKED receipt
means the peer's transport opened the packet, which is RakNet's meaning too.
It does not mean the application read the message (I12). Sequenced types have
no receipt variants (study §2).

### 7.5 What "enabled" means in the first version

The object holds a compile-time mask of the types an application may send. In
the first version it is `{0, 3}`.

- A send with any other reliability is refused (0, counted as
  `MESSAGE_SEND_TYPE_NOT_ENABLED`).
- The mask goes into CONFIG (section 13.4). Peers with different masks
  disconnect at CONFIG, so a first-version receiver never meets codes 1, 2 or 4
  from a correct peer. Code 2 is still live internally, because CONFIG uses it.
  The receiver treats an application record with code 1, 2 or 4 as violation
  `MESSAGE_TYPE_NOT_ENABLED`.
- Enabling a type means three things: one bit in the mask, removing one
  refusal, and the dispatch case. The dispatch code is written and unit-tested
  from the first version, behind the mask.

## 8. Layer 5: blocks

A reliable message is a **block** when its single record would not fit in
`1191` bytes, which is the RECORDS area of a 1192-byte payload.

- **Split.** The block is cut at the fixed stride `F = 1152`, a wire constant
  and a multiple of 16. Fragments `0 .. k-2` hold exactly `F` bytes and
  fragment `k-1` holds 1 to `F` bytes, so `k = ceil(len / F)`, with `2 <= k`.
  A block must satisfy `k <= W` and `len <= max_message_bytes <= B`, and
  `send_message` refuses anything larger. The largest fragment record is
  `F + 20` bytes (code 4, BLOCK, WIDE), so it always fits.
- **Numbering.** When the block's first fragment is first written, all `k`
  fragments take **consecutive numbers** `b .. b+k-1`, and the block takes one
  contiguous ring span starting at `p`. Fragment `i` lives at `p + i·F`. Both
  bounds of 6.2 are checked once, for the whole block. While the block cannot
  be admitted, numbering stops (layer 6). Smaller reliable records do not
  overtake it, so a large block cannot starve.
- **Shared fields.** Every fragment carries the block's code, channel and
  ordering and sequencing indices. A block takes one ordering slot
  (study I11). Each fragment also carries its own ring offset, its index and
  `k - 1`.
- **First sends and resends.** The block's first fragment goes in the packet
  being written. Its other fragments are numbered at the same moment, never
  sent yet, so they go to the head of the resend FIFO as due now. From there
  the following packets write them, one per packet, ahead of new items. Each
  fragment is acked and resent on its own, like RakNet (study §8.1). Each
  write copies the fragment straight from its chunk in the send store into the
  frame.
- **Receive.** A fragment passes the window check like any reliable record.
  The receiver computes `b = n - index` and checks that `ring - index·F/16`
  agrees with the block-start descriptor, if one exists. It copies the
  fragment's bytes to `ring + index·F` and counts it in the descriptor at
  `b % W`. That slot cannot be reused while the block is incomplete, because
  `U'` stops at `b` (6.2). When `have == k`, the block is one contiguous
  message in the ring, placed by index and not by arrival (the study's §8.2
  bug), and it is dispatched under its code (section 7.2) as one message.
  There is no reassembly buffer and no second copy.
- **No start packet.** Any fragment opens the block's state on its own,
  because every fragment carries everything needed (R3).

`RNBlocks` checks these properties: a delivered block equals the sent bytes; no
part of a block is ever delivered; a block is never mixed with another block
after numbers wrap; and the `U'` rule prevents slot reuse.

## 9. Layer 6: the scheduler and packet builder

### 9.1 What it owns

- The per-channel FIFOs and queued counts (fields in 7.1).
- Four **ready lists**, one per priority level, each a doubly linked list of
  channels. A channel sits in the list of the best priority among its queued
  messages.
- An **unchanneled queue** per level for codes 0 and 2, which have no channel
  on the wire. RakNet's API takes a channel for them and ignores it, and so
  does this one.
- A per-level virtual clock, advanced by RakNet's strides `1, 5, 14, 35` (study
  §5.3).

### 9.2 Order rules

- **Within a channel, FIFO.** A channel's ordered and sequenced records are
  first written in their ordering order, which is what section 6.3 step 2
  needs. RakNet does not keep this rule; its heaps are unbounded, so it does
  not need it.
- **Per-send priority boosts the whole channel.** A high-priority send on a
  channel moves that channel into the high-priority ready list, and the
  channel's queued messages go out in FIFO order at that priority until the
  high-priority message has been written. An ordered message cannot be
  delivered before its predecessors anyway. Boosting is what actually reduces
  its latency, without breaking the FIFO.
- **Between levels**, the builder takes from the level with the smallest
  virtual clock and advances that clock by its stride. Under backlog this
  gives RakNet's interleave of about 5:1, 2.8:1 and 2.5:1, and no level
  starves. When every queue is empty, the clocks reset, as in RakNet.
- **Within a level**, channels take turns in round robin, one message each.
  RakNet uses one global FIFO here. Round robin is the deliberate departure
  that keeps one chatty channel from starving the other 9,999.

### 9.3 Writing packets

`write_packets` runs per connection, at most `max_packets_per_update` packets:

1. **Plan:** if the next item is an unreliable message longer than one packet,
   it gets its own large payload (L2.3).
2. **Fill** a RECORDS packet in this order:
   - CONFIG, if it has not been written yet;
   - due resends (6.4);
   - new items in scheduler order. A reliable item that fails the 6.2 bounds
     stops new reliable numbering for this call, and unreliable items keep
     filling.
   
   Small unreliable messages pack until the packet is full. A record that does
   not fit ends the packet; it is never split across packets, except as a
   block.
3. **Commit** the packet (6.4). Unreliable data chunks are freed as soon as
   they are written.

An empty plan writes nothing. The transport's keep-alives and standalone acks
carry acks when this layer has nothing to send (S:1793-1828).

**Checked by** `RNScheduler`, with these properties:

- *FirstWriteInOrder*: within a channel, first writes are in ordering order.
- *NoStarvation* (liveness, under weak fairness of `write`): every queued
  message is eventually written, provided the window keeps reopening.
- *UnreliableNotBlocked*: unreliable records still go out while the reliable
  window is closed.

## 10. Layer 7: the endpoint and slots machines

### 10.1 The endpoint (one connection, pure)

```c
struct rn_endpoint_t;     // all of layers 3-6 for one connection; points into its region

void     rn_endpoint_init( rn_endpoint_t *, void * region, const rn_endpoint_config_t * );
void     rn_endpoint_reset( rn_endpoint_t *, double now );          // a new connection on this endpoint

// events in
uint32_t rn_endpoint_send( rn_endpoint_t *, const uint8_t * data, int bytes,
                           int reliability, int priority, int channel, uint32_t serial );  // 0 = refused
void     rn_endpoint_on_acked( rn_endpoint_t *, uint64_t packet_id, double now );
void     rn_endpoint_on_payload( rn_endpoint_t *, const uint8_t * payload, int bytes,
                                 rn_delivery_t * out, int out_capacity, int * out_count, double now );
void     rn_endpoint_tick( rn_endpoint_t *, double now );          // RTO due marks, LOSS sweep, reliable timeout

// actions out
int      rn_endpoint_next_packet_bytes( rn_endpoint_t *, double now ); // 0 = nothing to send; else request size
int      rn_endpoint_write_packet( rn_endpoint_t *, uint8_t * payload, int capacity,
                                   uint64_t packet_id, double now );  // bytes written
void     rn_endpoint_packet_failed( rn_endpoint_t *, uint64_t packet_id, double now );
int      rn_endpoint_take_receipts( rn_endpoint_t *, rn_receipt_t * out, int capacity );
int      rn_endpoint_error( const rn_endpoint_t * );               // 0, or a reason (section 12)
double   rn_endpoint_deadline( const rn_endpoint_t * );            // the earliest time tick has work
```

The "send these bytes to that peer" action is the payload the endpoint writes
into a frame the caller supplies. "Deliver this message" is the `rn_delivery_t`
entries it appends: `{ data, bytes, channel, code }`. Here `data` points into
the payload the caller passed (zero copy) or into the ring. "Start a timer" is
`rn_endpoint_deadline`. "Disconnect" is a nonzero `rn_endpoint_error`. The
endpoint never reads a clock, never calls the transport and never allocates.
A unit test can run two endpoints back to back through a lossy array that
stands in for the transport, with time as a loop variable.

### 10.2 The slots machine (server, pure)

`rn_slots_t` holds `connection_id[max_clients]`: the connection each slot's
endpoint is bound to. It has two events.

`observe( slot, connection_id )`, fed once per slot per update from P9(a).
Its actions:

- `RESET(slot)` when the id changes to a nonzero value. This also emits the
  CONNECTED event.
- `DROP(slot)` when the id goes to 0. This emits DISCONNECTED and discards the
  slot's queues.

`admit( slot, entry_connection_id )`, asked for every batch entry (payload or
acked id) at the moment the object would apply it, with the id P9(b) tagged
the entry with. Its answer is `APPLY` when the tag equals
`connection_id[slot]`, and otherwise `DISCARD`, counted as
`stale_connection_entries`. **An entry is applied only to the connection it was
received for.** The slot index alone never decides it, and no state read after
the transport's update decides it either: the decision is made at apply time,
from the tag written at receive time.

What this covers:

- **Same-batch churn.** A sends, A disconnects, B claims the slot, all in one
  batch. `observe` resets the slot to B; A's entries carry A's id and are
  discarded, never applied to B. B's own entries, later in the same batch,
  carry B's id and are applied.
- **Deferred acks** (P4). A's acked ids deferred past the update in which B
  took the slot still carry A's id and are discarded.
- **A connection that lived and died inside one batch** (A, then B, then C in
  one update). `observe` sees only C. B's entries are discarded, and B gets no
  CONNECTED or DISCONNECTED event: no endpoint was ever bound to it.
- **A free slot.** `connection_id[slot]` is 0 and no tag is 0, so every entry
  for it is discarded.

`observe` is O(slots that changed); `admit` is one u64 compare per entry. To
keep the per-update sweep constant-shape, the accessor is read for every slot,
one u64 load each. `RNSlots` checks that no message, ack or receipt of
connection A is ever applied to connection B in the same slot, and that no
entry is discarded from the connection it belongs to.

### 10.3 CONFIG: the one announcement

Each endpoint queues one CONFIG control record at reset, at the head of the
IMMEDIATE level. It takes reliable number 0, so it is resent until acked (R3).
Its body (section 13.4) states the wire-relevant configuration and the enabled
mask.

- On receipt, the endpoint compares the body field by field with its own
  configuration. On a mismatch it enters the error state with reason
  `CONFIG_MISMATCH`, which the object turns into a loud log line naming both
  values, then a disconnect.
- Data records may arrive before CONFIG. They are processed against the
  receiver's own bounds, which is safe either way (R6).
- A peer that never sends CONFIG within `reliable_timeout` is a
  `RELIABLE_TIMEOUT`.

This is the "identical and verified" model: both ends must be configured the
same, and the first reliable record proves it. Section 18.2, G1, gives the
alternative.

## 11. Layer 8: the client and server objects

### 11.1 The server: `rocketnet_server_t`

It owns:

- a `rocketnet_server_transport_t`;
- `max_clients` endpoints and their regions, from one allocation;
- the slots machine;
- the object's per-update lists: deliveries
  (`max_clients × (W + Du)` entries), receipts and events;
- the serial counter.

`rocketnet_server_update( server, time )` runs on the main thread:

1. `rocketnet_server_transport_update( transport, time )`.
2. The delivered, receipt and event lists of the previous update are reset.
3. **Slots:** observe every slot's connection id (P9a), then apply RESET and
   DROP.
4. **Acks:** for each `(client_index, connection_id, packet_id)` in the
   transport's acked batch, ask `admit( client_index, connection_id )` (P9b);
   on `APPLY`, call `on_acked`; on `DISCARD`, count it and go on.
5. **Payloads:** for each `(client_index, connection_id, payload)` in the
   transport's batch, ask `admit` the same way; on `APPLY`, call `on_payload`
   into that client's share of the delivered list. The check is here, at apply
   time, after step 3 has bound each slot to its current connection; it is
   never made at receive time against a slot whose owner may still change
   within the batch.
6. **Ticks:** call `tick( time )` for each slot whose deadline has passed. The
   deadlines sit in a 4-ary min-heap keyed by slot, so this is O(due · log n).
7. **Errors:** for each endpoint in error, call
   `rocketnet_server_transport_disconnect_client` (P7 allows this on the main
   thread, outside a fill phase) and record a DISCONNECTED event with its
   reason.

After update returns, the application reads deliveries, receipts and events.
They stay valid until the next update, the same lifetime as the transport's
batch (U:68-81). It then sends with `send_message` or `start_message`. In the
fill phase it may call `rocketnet_server_write_packets( server, client_index )`
from any thread, one thread per client at a time. Then, on the main thread,
`rocketnet_server_send_packets( server )` writes every client not yet written
this phase and calls the transport's `send_packets`. A simple caller never
calls `write_packets` at all.

### 11.2 The zero-copy send

```c
struct rocketnet_message_context_t { struct rocketnet_packet_context_t packet; uint8_t * message; int capacity; };

uint8_t * rocketnet_server_start_message( server, client_index, request_bytes, &ctx );
// write up to ctx.capacity bytes at ctx.message (16-aligned, the transport's payload root)
int       rocketnet_server_finish_message( server, &ctx, message_bytes );   // writes the form byte, finish_packet
void      rocketnet_server_abort_message( server, &ctx );
```

- `start_message` calls `start_packet( request_bytes + 1 )`. When
  `request_bytes <= 1191` that is a real wire frame, with no copy at all.
  Larger requests go through the transport's arena (L2.3).
- The message is always UNRELIABLE and is always SINGLE form. Endpoint state
  is not touched; only atomic counters are. So any number of threads may call
  it for any clients during the fill phase, exactly like `start_packet`.
- A zero-copy message gets no receipt and no ordering. UNRELIABLE needs
  neither. Every other type goes through `send_message`.

### 11.3 The client: `rocketnet_client_t`

The client is the same shape with one endpoint. That endpoint is reset at
create, because the client receives payloads from state 3 on (P8), and it is
never reset again. The client transport accepts sends only in state 4
(`sdk/rocketnet_client_protocol.cpp:1056-1057`). So `write_packets` writes
nothing before CONNECTED, and CONFIG and any early sends wait in the queue
without being numbered. One client object makes one connection, like the
transport. The client adds a copying cursor, `rocketnet_client_receive_message`,
modelled on `rocketnet_client_transport_receive_packet`. CONNECTED is emitted
when the transport first reports state 4. DISCONNECTED is emitted when the
transport leaves state 4, or when the endpoint errors, in which case the
object calls `rocketnet_client_transport_disconnect`.

### 11.4 What the objects do not own

The objects do not own serialization (CLAUDE.md), broadcast, or flows.

- **Broadcast** is a loop over `send_message`, the copy RakNet makes per target
  (study §5.1).
- **Flows** (REQUIREMENTS.md) belong to a later layer above this API: a flow
  send is a loop over subscribers. Nothing here forecloses that.
- **The game's own clock:** time is always passed in (`sdk/rocketnet.h:542-544`).

## 12. Layer 9: the C API, as it will appear in `sdk/rocketnet.h`

```c
// -----------------------------------------
//                                messages
// -----------------------------------------

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                                         32
#endif // #ifndef ROCKETNET_MAX_CHANNELS

#if ROCKETNET_MAX_CHANNELS < 1 || ROCKETNET_MAX_CHANNELS > 65536
#error ROCKETNET_MAX_CHANNELS must be between 1 and 65536
#endif

// reliability, per send -- RakNet's values and numbering (PacketPriority.h:46-85)

#define ROCKETNET_UNRELIABLE                                            0
#define ROCKETNET_UNRELIABLE_SEQUENCED                                  1
#define ROCKETNET_RELIABLE                                              2
#define ROCKETNET_RELIABLE_ORDERED                                      3
#define ROCKETNET_RELIABLE_SEQUENCED                                    4
#define ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT                           5
#define ROCKETNET_RELIABLE_WITH_ACK_RECEIPT                             6
#define ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT                     7

#define ROCKETNET_PRIORITY_IMMEDIATE                                    0
#define ROCKETNET_PRIORITY_HIGH                                         1
#define ROCKETNET_PRIORITY_MEDIUM                                       2
#define ROCKETNET_PRIORITY_LOW                                          3

#define ROCKETNET_RECEIPT_ACKED                                         1
#define ROCKETNET_RECEIPT_LOSS                                          2

#define ROCKETNET_MESSAGE_EVENT_CONNECTED                               1
#define ROCKETNET_MESSAGE_EVENT_DISCONNECTED                            2

#define ROCKETNET_MESSAGE_ERROR_NONE                                    0
#define ROCKETNET_MESSAGE_ERROR_TRANSPORT                               1   // the transport ended the connection
#define ROCKETNET_MESSAGE_ERROR_VIOLATION                               2   // the peer broke the wire; see counters
#define ROCKETNET_MESSAGE_ERROR_CONFIG_MISMATCH                         3
#define ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT                        4

// the largest message start_message writes into a real wire frame, zero copy:
// the payload capacity less the one form byte

#define ROCKETNET_MESSAGE_SINGLE_BYTES          ( ROCKETNET_MAX_PAYLOAD_BYTES - 1 )      // 1191

// the block stride, a wire constant

#define ROCKETNET_MESSAGE_FRAGMENT_BYTES                             1152

// The message config table, one declaration deriving struct, defaults, env
// names (ROCKETNET_ + field), parse and validation, as the transports' tables.
// Fields marked WIRE must be identical on both ends; CONFIG verifies them.

#define ROCKETNET_MESSAGE_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                       \
    INT_FIELD( num_channels,               32,      1, ROCKETNET_MAX_CHANNELS, 0 )   /* WIRE */                            \
    INT_FIELD( window_messages,          1024,     64, 1 << 20,                0 )   /* WIRE, power of two */              \
    INT_FIELD( receive_ring_bytes,      65536,   4096, 1 << 20,                0 )   /* WIRE, multiple of 16 */            \
    INT_FIELD( max_message_bytes,       32768,   1191, 1 << 20,                0 )   /* WIRE, <= receive_ring_bytes */     \
    INT_FIELD( send_buffer_bytes,      131072,  12288, 1 << 28,                0 )                                         \
    INT_FIELD( send_queue_messages,      1024,     16, 1 << 20,                0 )                                         \
    INT_FIELD( packets_in_flight,         256,     16, 1 << 16,                0 )                                         \
    INT_FIELD( max_packets_per_update,      4,      1, 256,                    0 )                                         \
    INT_FIELD( unreliable_deliveries,     256,      1, 1 << 16,                0 )   /* per connection per update */       \
    INT_FIELD( sequenced_holds,            16,      0, 4096,                   0 )                                         \
    INT_FIELD( receipt_ring,              256,     16, 1 << 16,                0 )                                         \
    INT_FIELD( rto_initial_ms,           1000,     10, 60000,                  0 )                                         \
    INT_FIELD( rto_min_ms,                 50,      1, 60000,                  0 )                                         \
    INT_FIELD( rto_max_ms,               2000,     10, 60000,                  0 )                                         \
    INT_FIELD( reliable_timeout_ms,     10000,   1000, 600000,                 0 )

struct rocketnet_message_config_t
{
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

ROCKETNET_EXPORT_FUNC void rocketnet_message_default_config( struct rocketnet_message_config_t * config );

// 1 accepted, 0 refused with a log line naming the rule; NULL is the defaults (landed, C1)
ROCKETNET_EXPORT_FUNC int rocketnet_message_validate_config( const struct rocketnet_message_config_t * config );

// Allocation-free: the bytes one connection's region needs (section 14 of docs/CHANNELS.md).
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_connection_bytes( const struct rocketnet_message_config_t * config );

// Allocation-free: the formula, each term, the padding, the region and the total for n
// connections, as text, with snprintf's contract; and the same lines logged at info (landed, C1)
ROCKETNET_EXPORT_FUNC int rocketnet_message_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections, char * buffer, int buffer_size );
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_print_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections );

struct rocketnet_message_context_t
{
    struct rocketnet_packet_context_t packet;
    uint8_t * message;                      // 16-aligned; write the message here
    int capacity;                           // bytes writable at message
};

// ---- server ----

struct rocketnet_server_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context, const char * public_address_string,
                                                                          const struct rocketnet_server_transport_config_t * transport_config,
                                                                          const struct rocketnet_message_config_t * message_config, double time );
ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server );        // stop and wait for stopped first
ROCKETNET_EXPORT_FUNC void rocketnet_server_stop( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC int rocketnet_server_state( struct rocketnet_server_t * server );           // ROCKETNET_SERVER_TRANSPORT_*
ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t * rocketnet_server_transport( struct rocketnet_server_t * server ); // stats, port, driver; never its batches
ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server, double time );

// sending: copying, any reliability. returns the receipt serial, nonzero, or 0 when refused
// (counted: not connected, bad channel or reliability, type not enabled, too large, store or queue full)
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_server_send_message( struct rocketnet_server_t * server, int client_index,
                                                              const uint8_t * data, int bytes,
                                                              int reliability, int priority, int channel );

// sending: zero copy, UNRELIABLE only, any thread in the fill phase
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, int client_index, int request_bytes, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC int rocketnet_server_finish_message( struct rocketnet_server_t * server, struct rocketnet_message_context_t * context, int message_bytes );
ROCKETNET_EXPORT_FUNC void rocketnet_server_abort_message( struct rocketnet_server_t * server, struct rocketnet_message_context_t * context );

ROCKETNET_EXPORT_FUNC void rocketnet_server_write_packets( struct rocketnet_server_t * server, int client_index );  // fill phase, one thread per client
ROCKETNET_EXPORT_FUNC void rocketnet_server_send_packets( struct rocketnet_server_t * server );                    // main thread

// receiving: valid until the next update
ROCKETNET_EXPORT_FUNC int rocketnet_server_num_messages( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC const uint8_t * rocketnet_server_message( struct rocketnet_server_t * server, int message_index,
                                                                int * client_index, int * channel, int * reliability, int * message_bytes );
ROCKETNET_EXPORT_FUNC int rocketnet_server_messages( struct rocketnet_server_t * server, const int ** client_index,
                                                     const uint8_t * const ** message_data, const int ** message_bytes,
                                                     const uint16_t ** channel, const uint8_t ** reliability );

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_receipts( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_server_receipt( struct rocketnet_server_t * server, int receipt_index, int * client_index, int * status );

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_events( struct rocketnet_server_t * server );
ROCKETNET_EXPORT_FUNC int rocketnet_server_event( struct rocketnet_server_t * server, int event_index, int * client_index, int * error );

ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );  // main thread
ROCKETNET_EXPORT_FUNC int rocketnet_server_is_client_connected( struct rocketnet_server_t * server, int client_index );

struct rocketnet_message_stats_t
{
    int queued_messages;                    // waiting for their first write
    int reliable_in_flight;                 // numbered, unacked
    int receive_held;                       // held at this end for ordering
    float rtt;                              // this layer's srtt (milliseconds)
    float rto;                              // current rto including backoff (milliseconds)
};

ROCKETNET_EXPORT_FUNC void rocketnet_server_message_stats( struct rocketnet_server_t * server, int client_index, struct rocketnet_message_stats_t * stats );
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_server_counters( struct rocketnet_server_t * server );

// ---- client ----

struct rocketnet_client_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token,
                                                                          const struct rocketnet_client_transport_config_t * transport_config,
                                                                          const struct rocketnet_message_config_t * message_config, double time );
ROCKETNET_EXPORT_FUNC void rocketnet_client_destroy( struct rocketnet_client_t * client );        // disconnect and pump a few updates first
ROCKETNET_EXPORT_FUNC void rocketnet_client_disconnect( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC int rocketnet_client_state( struct rocketnet_client_t * client );           // ROCKETNET_CLIENT_TRANSPORT_STATE_*
ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t * rocketnet_client_transport( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void rocketnet_client_update( struct rocketnet_client_t * client, double time );

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_client_send_message( struct rocketnet_client_t * client, const uint8_t * data, int bytes,
                                                              int reliability, int priority, int channel );
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int request_bytes, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC int rocketnet_client_finish_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context, int message_bytes );
ROCKETNET_EXPORT_FUNC void rocketnet_client_abort_message( struct rocketnet_client_t * client, struct rocketnet_message_context_t * context );
ROCKETNET_EXPORT_FUNC void rocketnet_client_write_packets( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC void rocketnet_client_send_packets( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC int rocketnet_client_num_messages( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC const uint8_t * rocketnet_client_message( struct rocketnet_client_t * client, int message_index,
                                                                int * channel, int * reliability, int * message_bytes );
ROCKETNET_EXPORT_FUNC int rocketnet_client_messages( struct rocketnet_client_t * client, const uint8_t * const ** message_data,
                                                     const int ** message_bytes, const uint16_t ** channel, const uint8_t ** reliability );
// the copying cursor: pops the next message into data, returns its length or 0 when drained;
// a message larger than capacity is refused whole and counted, never truncated
ROCKETNET_EXPORT_FUNC int rocketnet_client_receive_message( struct rocketnet_client_t * client, uint8_t * data, int capacity,
                                                            int * channel, int * reliability );

ROCKETNET_EXPORT_FUNC int rocketnet_client_num_receipts( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_client_receipt( struct rocketnet_client_t * client, int receipt_index, int * status );
ROCKETNET_EXPORT_FUNC int rocketnet_client_num_events( struct rocketnet_client_t * client );
ROCKETNET_EXPORT_FUNC int rocketnet_client_event( struct rocketnet_client_t * client, int event_index, int * error );

ROCKETNET_EXPORT_FUNC void rocketnet_client_message_stats( struct rocketnet_client_t * client, struct rocketnet_message_stats_t * stats );
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_client_counters( struct rocketnet_client_t * client );
```

The objects' counters are the transport's counters with the message counters
appended to the same enum (`sdk/rocketnet_counters.h`). The client's message
counters carry the `CLIENT_` prefix, following the existing convention. The
counters this document names are `MESSAGE_SEND_STORE_FULL`,
`MESSAGE_SEND_QUEUE_FULL`, `MESSAGE_SEND_TYPE_NOT_ENABLED`,
`MESSAGE_SEND_REFUSED` (bad argument), `MESSAGE_DUPLICATE`,
`MESSAGE_SEQUENCED_STALE`, `MESSAGE_SEQUENCED_SUPERSEDED`,
`MESSAGE_SEQUENCED_HOLD_FULL`, `MESSAGE_UNRELIABLE_DELIVERY_FULL`,
`MESSAGE_SENT_MAP_EVICTED`, `MESSAGE_RECEIPT_RING_FULL`,
`MESSAGE_RESENT`, `MESSAGE_DELIVERED`, `MESSAGE_WINDOW_STALL`, plus one
counter for each violation in section 13.3.

**The C# shim.** The study's §13 is provisional until the shim arrives. This
API covers each provisional need:

- RakNet's `Send` maps to `send_message`, which returns a nonzero serial.
- `Receive` and `DeallocatePacket` become iteration over the update's list, or
  the client's copying cursor.
- Receipts and connection notices are lists. A shim can merge them into
  RakNet's single stream, so the interleaving I17 describes is reproduced above
  this layer.
- This layer does not reserve the first payload byte for a message id. It
  stays the shim's (study §13).

## 13. The wire, to the bit, ready for STANDARD.md

This section is written to be pasted under a new STANDARD.md heading,
_Messages_, as is. All integers are little endian and byte aligned; no field is
narrower than a byte (CLAUDE.md, 2026-08-26 ruling). The subject is the
transport *payload*, the application data of a type 5 or type 9 packet (S:638).

### 13.1 Payload forms

```
payload := body form            L = len(payload) >= 2
form    := u8                   the LAST byte
           0x00  SINGLE    body is one UNRELIABLE message, 1 <= len(body)
           0x01  RECORDS   body is one or more records, exactly filling it
           else  violation MESSAGE_BAD_FORM
```

### 13.2 Record

```
offset  size  field
0       1     flags
                bits 0-2  code: 0 UNRELIABLE, 1 UNRELIABLE_SEQUENCED, 2 RELIABLE,
                          3 RELIABLE_ORDERED, 4 RELIABLE_SEQUENCED; 5-7 violation
                bit  3    BLOCK    this record is one fragment of a block
                bit  4    WIDE     channel is u16, not u8
                bit  5    CONTROL  a layer control record (13.4)
                bits 6-7  zero
1       2     length               data bytes, >= 1
3       ...   the fields below, each present only when its condition holds, in this order:

  number      u24   code in {2,3,4}                     reliable message number, mod 2^24
  ring        u16   code in {3,4} or BLOCK              receive-ring offset in 16-byte units
  sequencing  u24   code in {1,4}                       sequencing index, mod 2^24
  ordering    u24   code in {1,3,4}                     ordering index, mod 2^24
  channel     u8    code in {1,3,4} and not WIDE
              u16   code in {1,3,4} and WIDE
  fragment    u16   BLOCK                               fragment index
  fragments   u16   BLOCK                               fragment count - 1

  data        length bytes
```

Header bytes by type are 3, 10, 6, 12 and 15 for codes 0 to 4. BLOCK adds 4
(6 on code 2, which gains the ring field). WIDE adds 1. A sender sets WIDE
exactly when `channel >= 256`, and a receiver refuses WIDE below 256, so every
record has one encoding. The field conditions hold for every flags byte, so a
code of 5 to 7 carries no code-dependent fields and BLOCK adds its fields on any
code: a receiver measures the header before it judges the flags.

### 13.3 The receiver's checks, in order

Checks 1 to 9 run over the whole payload before any record of it is handed
up, so a refused payload delivers nothing and a record is whole or absent.
Checks 10 to 12 need the receiver's state and run as each record is applied.
Each failed check stops parsing the payload, increments the named counter
(prefixed `CLIENT_` on the client), and makes the connection a violation
(R6).

1. `L < 2`, or the form byte is not 0 or 1: `MESSAGE_BAD_FORM`.
2. In RECORDS form, fewer than 3 bytes remain, or the header's fields run past
   the body: `MESSAGE_TRUNCATED`.
3. The code is 5 to 7, bits 6 and 7 are not zero, BLOCK is set on code 0 or 1,
   WIDE is set on code 0 or 2, or CONTROL is set with a code other than 2 or
   together with BLOCK: `MESSAGE_BAD_FLAGS`.
4. `length == 0`, or `length` runs past the body: `MESSAGE_BAD_LENGTH`.
5. `channel >= num_channels`, or WIDE with `channel < 256`:
   `MESSAGE_BAD_CHANNEL`. The check is against this end's runtime count, not
   the compile-time maximum (I15).
6. The type is not in the enabled mask (CONTROL is exempt):
   `MESSAGE_TYPE_NOT_ENABLED`. A wire code is admitted when an enabled
   reliability travels as it (7 admits 3). A SINGLE payload is code 0 here.
7. BLOCK checks:
   - `fragments == 0`, `fragment > fragments`, `(fragments+1) > W`, or
     `(fragments+1) × F > max_message_bytes + F - 1`;
   - a fragment below the last whose length is not `F`, or a last fragment
     longer than `F`;
   - a ring base `ring - fragment × F/16` that disagrees with a live block
     descriptor (this one needs layer 5's state, and runs with check 10).

   Any of these: `MESSAGE_BAD_BLOCK`.
8. A holdable record with `ring × 16 + span > B`, where span is `length`
   rounded up to 16, or for a block the whole block's span:
   `MESSAGE_BAD_RING`. A block's span runs from its base
   `ring - fragment × F/16` (never below 0) to
   `base × 16 + fragments × F + last`, where `last` is the last fragment's
   length rounded up to 16; a fragment that is not the last takes `last = 16`,
   the shortest block it could be part of.
9. A reliable or CONTROL record inside a payload longer than
   `ROCKETNET_MAX_PAYLOAD_BYTES` (1192). Such a payload either rode the
   transport's fragments or came from a sender that broke L2.2 (L2.4):
   `MESSAGE_BAD_SIZE`.
10. Window (6.5): `MESSAGE_NUMBER_OUTSIDE_WINDOW`, `MESSAGE_SLOT_COLLISION`.
11. A code 3 record whose ordering is behind `ordered_read`:
    `MESSAGE_ORDERING_BEHIND`.
12. A CONTROL record that is malformed or of an unknown kind:
    `MESSAGE_BAD_CONTROL`.

A duplicate (6.5) is not a violation; it is counted as `MESSAGE_DUPLICATE`.

### 13.4 The CONFIG control record

The record is code 2 with CONTROL set. It takes the sender's first reliable
number. Its data is 28 bytes:

```
offset  size  field
0       1     kind                 1 = CONFIG
1       1     version              1
2       1     enabled              bit r set = reliability r may be sent by the application
3       1     zero
4       4     num_channels         u32
8       4     window_messages      u32
12      4     receive_ring_bytes   u32
16      4     max_message_bytes    u32
20      4     fragment_bytes       u32, 1152
24      4     zero
```

The receiver compares every field with its own values. Any difference is
`ERROR_CONFIG_MISMATCH`. A version the receiver does not know is a mismatch
too. Kinds other than 1 are reserved; they are `MESSAGE_BAD_CONTROL` until a
version defines them.

### 13.5 Numbers

Number, sequencing and ordering are 24-bit counters that wrap. Each end keeps
64-bit values and reconstructs a wire value against its reference by signed
half range (2^23). For numbers the reference is `R`. For ordering it is the
channel's `ordered_read`. For sequencing it is the channel's
`sequenced_watermark`. Every live value lies within `W` (at most 2^20) of its
reference, so the half range never aliases.

### 13.6 Known-answer vectors

The bytes below are the payload, before the transport seals it.

```
SINGLE, message "hi":                        68 69 00
RECORDS, one UNRELIABLE "hi":                00 02 00 68 69 01
RECORDS, RELIABLE_ORDERED "hi", number 5, ring 0, ordering 0, channel 3:
                                             03 02 00 05 00 00 00 00 00 00 00 03 68 69 01
RECORDS, the two records above in one packet:
                                             00 02 00 68 69 03 02 00 05 00 00 00 00 00 00 00 03 68 69 01
RECORDS, fragment 1 of 3 of a RELIABLE_ORDERED block, number 9, ring 72, ordering 7, channel 300 (WIDE):
                                             1b 80 04 09 00 00 48 00 07 00 00 2c 01 01 00 02 00 <1152 data bytes> 01
CONFIG, defaults, number 0:
                                             22 1c 00 00 00 00 01 01 09 00 20 00 00 00 00 04 00 00
                                             00 00 01 00 00 80 00 00 80 04 00 00 00 00 00 00 01
```

Field by field, for the block vector:

- flags `0x1b`: code 3, BLOCK, WIDE.
- length `0x0480`: 1152.
- number 9.
- ring 72: its data starts at `72 × 16 = 1152`, fragment 1 of a block placed
  at 0.
- ordering 7.
- channel 300, as `2c 01`.
- fragment 1.
- fragments−1: 2.

The CONFIG vector:

- flags `0x22`: code 2 with CONTROL.
- length 28, then number 0.
- kind 1, version 1.
- enabled `0x09`: types 0 and 3.
- 32 channels, `W` 1024, `B` 65536, max 32768, F 1152.

Card C2 regenerates every vector from the codec and diffs it byte for byte.

## 14. Sizing: memory from the configuration

Symbols:

- `C` = `num_channels`, `W` = `window_messages`, `B` = `receive_ring_bytes`
- `S` = `send_buffer_bytes`, `Q` = `send_queue_messages`, `P` = `packets_in_flight`
- `Du` = `unreliable_deliveries`, `Ru` = `sequenced_holds`, `Rq` = `receipt_ring`

The per-entry sizes are working figures. Layer 1 pins each one with `sizeof`
and a static assert, and `rocketnet_message_connection_bytes` returns the
exact figure.

| Part | Bytes |
|---|---|
| channel state, send and receive | `48·C` |
| sender per-number table | `32·W` |
| receiver descriptors | `32·W` |
| received bitset | `W/8` |
| held-ordered index | `16·2W` |
| receive ring | `B` |
| send descriptors | `64·Q` |
| send store (slab) | `S` |
| sent-packet ring, its index and its references | `32·P + 16·2P + 4·16P` = `128·P` |
| delivered entries (on the server, the object's list) | `24·(W + Du)` |
| sequenced holds | `1208·Ru` |
| receipt ring | `16·Rq` |

```
connection_bytes = 48C + 96.125W + B + 64Q + S + 128P + 24(W + Du) + 1208Ru + 16Rq
```

At the defaults, `C=32, W=1024, B=65536, S=131072, Q=1024, P=256, Du=256,
Ru=16, Rq=256`:

| term | value |
|---|---|
| 48·32 | 1,536 |
| 96.125·1024 | 98,432 |
| B | 65,536 |
| 64·1024 | 65,536 |
| S | 131,072 |
| 128·256 | 32,768 |
| 24·1280 | 30,720 |
| 1208·16 | 19,328 |
| 16·256 | 4,096 |
| **total** | **449,024** bytes, about 438 KiB |

That is about 428 MiB for a 1000-client server, on top of the transport's own
memory (`rocketnet_server_transport_required_memory`).

- **Lean profile:** `W=256, B=32768, S=65536, Q=256, P=64, Du=128` comes to
  about 177 KiB per connection, or 173 MiB at 1000. As built, its store holds 18 large chunks, so it also needs
  `max_message_bytes <= 20,736` (section 4).
- **Channel cost:** 48 bytes per channel. 1024 channels cost 48 KiB per
  connection, and 10,000 channels cost 469 KiB (458 MiB at 1000 clients). No
  other term moves, and nothing scales as channels × window. That is what
  makes the 10k stress run a one-number change (I15).
- **Validation at create** refuses any of these:
  - `max_message_bytes > B`;
  - `ceil(max_message_bytes / F) > W`;
  - `W` not a power of two;
  - `B` not a multiple of 16;
  - a 1152 class (a third of `S`) too small to hold one maximal block, that
    is, fewer than `ceil(max_message_bytes/F)` chunks (as built, the class
    holds `floor(third / 1156)` chunks with their links, section 4);
  - `rto_initial_ms` outside `rto_min_ms` to `rto_max_ms` (added by card C1).

  Validation refuses and never clamps.

## 15. The TLA+ modules

Each module drives its transitions through `tla/Link.tla` (loss, duplication,
reordering, delay) where it talks to a peer. Each has a small instance in an
`MC*.tla` and `.cfg`, and a **reversed witness**: the same module with its
guarding rule removed, which TLC must fail with the counterexample named here.
Every run is a row in `tla/RUNS.tsv`. A counterexample is checked against the
code by hand before it is written down.

| Module | Models | Invariants and properties | Reversed witness (rule removed, expected failure) |
|---|---|---|---|
| `RNWindow` | sender numbering with `U'`, the count bound, sent map and acks; the receiver's `R`, bitset and descriptors; update boundaries with P6 | `ReceiverNeverOutsideWindow` (a correct sender never trips 6.5), `NoSlotCollision`, `AckedImpliesReceived`, LIVE `EveryReliableEventuallyAcked` (weak fairness on Deliver) | `U'` ignoring partial blocks: slot collision. Dropping P6 (same-update reply): slot collision |
| `RNRing` | ring placement, the padding rule and the floor; receiver live spans | `LiveSpansDisjoint`, `SpanInsideRing`, LIVE `EveryAdmissibleMessagePlaced` | a floor of `ring_start(U)` instead of `U'`: overlap. No padding at the boundary: span outside the ring |
| `RNChannel` | the four counters and coupling, holds, the drain, newest-sequenced | `OrderedExactlyOnceInOrder`, `SequencedNeverBackwards`, `NoSequencedDuplicate` (study items 18, 19), `OrderedTraceUnchangedBySequenced` (I5) | the heap-drain path without the watermark reset: item 19's stale drop |
| `RNBlocks` | split, consecutive numbering, the contiguous span, receive by index | `DeliveredEqualsSent`, `NoPartialDelivery`, `OneOrderingSlotPerBlock` | placement by arrival instead of index: the study's §8.2 corruption |
| `RNScheduler` | priority levels, ready lists, channel FIFO, window stall | `FirstWriteInOrderPerChannel`, `UnreliableNotBlockedByWindow`, LIVE `NoStarvation` | per-message priority without channel FIFO: `FirstWriteInOrderPerChannel` fails |
| `RNReceipts` | serials, ACKED and LOSS for 5, 6 and 7, eviction, block receipts | `ExactlyOneOutcomePerReceiptSerial`, `LossOnlyForUnreliableReceipt`, `BlockAckedOnlyWhenAllFragmentsAcked` | firing on the last-index fragment (RakNet): early ACKED |
| `RNEndpoint` | two endpoints over Link, with transport ack semantics (acked ⇒ opened, burned ids, history overflow ⇒ silence), CONFIG, errors | end to end: reliable exactly-once, ordered in order, unreliable at-most-once, CONFIG mismatch ⇒ both sides error; LIVE no hang: with CONFIG lost on first send, both still verify | CONFIG sent once, unreliable: a lost first packet hangs verification |
| `RNSlots` | one slot under the transport's loop: connect, disconnect and reassign any number of times between object updates; payload and acked-id entries tagged with the connection they were received for; acked ids deferred across updates (P4); the object update as observe, then admit at apply time | `NoCrossConnectionApply` (no payload or ack of connection A is applied to B), `NoOwnEntryDropped` (admit never discards an entry from the connection it belongs to) | applying by slot after observing the slot's connection id (the old rule, which P9a alone gives): A sends, A disconnects, B claims the slot, all in one update; observe resets to B and A's payload is applied to B. By hand, the same trace also fails with `is_client_connected` or `client_id` in place of the connection id: neither changes, so there is not even a RESET |

The instances are small: 2 to 3 channels, `W = 4`, `B = 64` (4 units), 3 to 4
messages, and Link capacity 3. `RNEndpoint` is the largest, and it runs on a
bench machine, never on the working machine.

### 15.1 `RNSlots`, drafted and run

`RNSlots` is the one module drafted ahead of its card, because the slot rule
was found broken in review and its fix must be shown, not argued. Card C7
moves it into `tla/` with its `RUNS.tsv` rows. `CheckAtApply = TRUE` is the
rule of 10.2; `FALSE` is the old rule, applying by slot after `observe`.

```tla
---------------------------- MODULE RNSlots ----------------------------
\* One server slot over the transport's update loop (CHANNELS.md 10.2, 11.1).
\* The transport may release and reassign the slot many times between two
\* object updates; every batch entry (payload or acked id) is tagged with the
\* connection id it was received for. Acked ids may be deferred to a later
\* update (P4). The object update observes the slot's id (RESET/DROP), then
\* applies entries. CheckAtApply = FALSE is the old rule (apply by slot).
EXTENDS Naturals, Sequences

CONSTANTS MaxConn, MaxEntries, CheckAtApply

VARIABLES owner, nextId, batch, deferred, bound, applied, dropped, made

vars == << owner, nextId, batch, deferred, bound, applied, dropped, made >>

Entry == [kind : {"payload", "ack"}, conn : 1..MaxConn]

Init == /\ owner = 0 /\ nextId = 1
        /\ batch = << >> /\ deferred = << >>
        /\ bound = 0
        /\ applied = {} /\ dropped = {} /\ made = 0

\* ---- transport, inside one update ----
Connect == /\ owner = 0 /\ nextId <= MaxConn
           /\ owner' = nextId /\ nextId' = nextId + 1
           /\ UNCHANGED << batch, deferred, bound, applied, dropped, made >>

Disconnect == /\ owner # 0 /\ owner' = 0
              /\ UNCHANGED << nextId, batch, deferred, bound, applied, dropped, made >>

Receive(k) == /\ owner # 0 /\ made < MaxEntries
              /\ batch' = Append(batch, [kind |-> k, conn |-> owner])
              /\ made' = made + 1
              /\ UNCHANGED << owner, nextId, deferred, bound, applied, dropped >>

AckDeferred == /\ owner # 0 /\ made < MaxEntries
               /\ deferred' = Append(deferred, [kind |-> "ack", conn |-> owner])
               /\ made' = made + 1
               /\ UNCHANGED << owner, nextId, batch, bound, applied, dropped >>

\* ---- the object update: observe (RESET/DROP), then apply ----
Entries(s) == { s[i] : i \in 1..Len(s) }

Applies(e, now) == IF CheckAtApply THEN e.conn = now ELSE now # 0

ObjectUpdate ==
    LET now == owner
        es  == Entries(batch) \cup Entries(deferred)
    IN /\ bound' = now                                  \* RESET or DROP
       /\ applied' = applied \cup { [from |-> e.conn, to |-> now] : e \in { x \in es : Applies(x, now) } }
       /\ dropped' = dropped \cup { [from |-> e.conn, to |-> now] : e \in { x \in es : ~Applies(x, now) } }
       /\ batch' = << >> /\ deferred' = << >>
       /\ UNCHANGED << owner, nextId, made >>

Next == Connect \/ Disconnect \/ Receive("payload") \/ Receive("ack")
        \/ AckDeferred \/ ObjectUpdate

Spec == Init /\ [][Next]_vars

\* No payload or ack of connection A is ever applied to connection B.
NoCrossConnectionApply == \A a \in applied : a.from = a.to
\* The check never drops what belongs to the connection it is applied to.
NoOwnEntryDropped == \A d \in dropped : d.from # d.to
TypeOK == /\ owner \in 0..MaxConn /\ bound \in 0..MaxConn
          /\ batch \in Seq(Entry) /\ deferred \in Seq(Entry)
==========================================================================
```

```
SPECIFICATION Spec
CONSTANTS MaxConn = 3
          MaxEntries = 3
          CheckAtApply = TRUE
INVARIANTS TypeOK NoCrossConnectionApply NoOwnEntryDropped
```

Run on the bench `vision`, TLC 2.19 (rev 5a47802), `-workers 2`, 2026-10-06:

| Instance | Result |
|---|---|
| `CheckAtApply = TRUE` (10.2) | `Model checking completed. No error has been found.` 7427 states generated, 3762 distinct states found, 0 states left on queue. |
| `CheckAtApply = FALSE` (old rule) | `Error: Invariant NoCrossConnectionApply is violated.` at depth 6: Connect (A = 1), Receive payload tagged 1, Disconnect, Connect (B = 2), ObjectUpdate, ending in `applied = {[from \|-> 1, to \|-> 2]}`. |

The counterexample matches the code by hand: the payload entry is appended
with only its slot index (`sdk/rocketnet_server_transport.cpp:1014`), the
disconnect releases the slot in the same receive loop
(`sdk/rocketnet_server_protocol.cpp:941`), and a later connection response in
that loop can claim it, so after the transport's update the slot names B while
A's payload is still in the batch.

## 16. Threads, transitions, purity

### 16.1 Thread rules, inherited from the transport (P7)

- **Main thread:**
  - `create`, `destroy`, `stop` and `disconnect`
  - `update`
  - `send_packets`
  - `disconnect_client`

  None of these runs while a fill phase is open (`sdk/rocketnet.h:841-855`).
- **Reads, between `update` and the next `update`:** messages, receipts,
  events, stats and counters, from any number of threads, read-only.
- **`send_message` for connection *i*:** from one thread at a time per
  connection, between `update` returning and `send_packets`. This is the
  *one writer per connection* rule. Different connections may be sent from
  different threads at the same time. The serial counter is atomic.
- **`write_packets` for connection *i*:** fill phase, under the same one
  writer per connection rule. It calls the transport's `start_packet` and
  `finish_packet`, which are fill-phase safe.
- **`start_message`, `finish_message`, `abort_message`:** fill phase, any
  thread, any connection, concurrently. They touch only the transport and
  atomic counters.
- **Handoff:** the fill-to-flush handoff is a join or a barrier, never a
  wall-clock wait, exactly as for the transport.

### 16.2 The transition rule, kept

| Transition | How this design keeps R3 |
|---|---|
| Connection start | The transport owns it, through its handshake, which is retried on timers (S:2024-2121). The endpoint resets locally on an observed connection id (server) or at create (client). Nothing goes on the wire. |
| Channel start | Does not exist. Every channel is open from reset, and its four counters start at 0 on both ends. |
| Block start | Does not exist. Every fragment carries everything needed to open the block's state (section 8). |
| Configuration | CONFIG, a reliable record: resent until acked, and the peer acts on it once. |
| Reset | Local only. It is tied to the transport's connection boundary, where new keys mean an old packet cannot open. |
| Disconnect | The transport's redundant burst and idle timeout (`sdk/rocketnet.h:905-912`). The message layer only reacts. |
| Window stall and reopen | Driven by acks, which the transport repeats in every block (S:1700-1792), and by RTO resends. No single packet carries the reopen. |

### 16.3 The pure-module rule, per machine

| Machine | State struct | Events | Actions | Time |
|---|---|---|---|---|
| send store | `rn_send_store_t` | alloc, free | chunk or refusal | none |
| window (send) | inside `rn_endpoint_t` | number, ack, write, failed | refs, resend marks | `now` argument |
| window (receive) | inside `rn_endpoint_t` | record | accept, duplicate, violation | none |
| channel | `rn_channel_*_t` arrays | send, record | deliver, hold, drop | none |
| scheduler | inside `rn_endpoint_t` | enqueue, take | next item | none |
| endpoint | `rn_endpoint_t` | send, payload, acked, tick | bytes into a frame, deliveries, receipts, error, deadline | `now` argument |
| slots | `rn_slots_t` | observe(slot, id), admit(slot, entry id) | RESET, DROP; APPLY or DISCARD | none |

## 17. For the reviewer: eight types reachable, two shipped

### 17.1 The six later types, one by one

No row changes the wire, the per-channel state or the region layout.

| Type | Wire (already specified) | Sender state used (already allocated) | Receiver state used (already allocated) | What enabling it adds |
|---|---|---|---|---|
| 1 UNRELIABLE_SEQUENCED | code 1: sequencing, ordering, channel | `sequenced_write`, already zeroed by every ordered send | `sequenced_watermark`, already reset on ordered delivery; `held_sequenced`; `Ru` pool | mask bit; dispatch case 1 (already written and unit-tested behind the mask) |
| 2 RELIABLE | code 2: number | the per-number table (in use for CONFIG from day one) | bitset and descriptors (in use for CONFIG) | mask bit |
| 4 RELIABLE_SEQUENCED | code 4: number, ring, sequencing, ordering, channel | number, ring bytes, `sequenced_write` | ring hold via `held_sequenced` | mask bit; dispatch case 4 |
| 5 UNRELIABLE_WITH_ACK_RECEIPT | as code 0; a block of code 2 above a packet | the descriptor's receipt flag; the sent-map entry's receipt list; the LOSS sweep | none | mask bit; the receipt hook on commit, ack and evict |
| 6 RELIABLE_WITH_ACK_RECEIPT | as code 2 | the receipt flag | none | mask bit; ACKED on the last number acked |
| 7 RELIABLE_ORDERED_WITH_ACK_RECEIPT | as code 3 | the receipt flag | none | mask bit; as 6 |

The CONFIG `enabled` mask changes when a type is enabled, so a new build and an
old build refuse each other loudly instead of misbehaving. That is the one
visible effect, and it is the intended one.

### 17.2 The study's invariants, answered

| I | Answer here |
|---|---|
| I1 | A 3-bit code, dispatched in 7.2. All eight values are reserved, and unknown or disabled codes are counted violations, never read as a shipped type (13.3). |
| I2 | Flags, then length, then the fields the code needs (13.2). |
| I3 | Numbers are connection-wide and late (6.2). Ordering and sequencing are per channel, taken at send (7.1). Unreliable records never take a number. |
| I4 | Every resend is a new `packet_id` that carries the same number. The transport dedupes packets; this layer dedupes numbers (6.5). |
| I5 | Four counters per channel and the coupling from the first version, applied on one path (7.1). |
| I6 | Holds are keyed by `(channel, ordering)`, plus the newest sequenced message per channel (7.2, 7.3). |
| I7 | Everything is bounded at create (section 14). Reliable traffic stalls; unreliable traffic drops and is counted. The sender counts in-flight from its own state (`next - U`, ring use). |
| I8 | The sent map from `packet_id` to a list of numbers, mixed-reliability packets from day one (6.4). |
| I9 | `send_message` returns a serial for every type, and the descriptor stores it (7.4). |
| I10 | Receipts for type 5 live in the sent-map entry in the region. LOSS comes from the deadline sweep (7.4). |
| I11 | Large reliable messages are split by this layer at a fixed stride and resent per fragment, never through the transport's fragments (P3). They take consecutive numbers, one ordering slot, and are placed by index. Large unreliable messages use the transport's fragments (L2.3). Both choices are explicit. |
| I12 | The sender clears resend state on the transport ack, whatever the receiver did with the record. ACKED means "opened", nothing more (7.4). |
| I13 | Unreliable records are never subject to the window bounds (6.2, 9.3). |
| I14 | One RTT estimator and one RTO per connection. Dead-connection detection is the transport's, plus `RELIABLE_TIMEOUT` (6.4). |
| I15 | Checked against the runtime `num_channels` on both ends. The channel field is u8 or u16 by the WIDE flag, so 1024 and 10,000 channels change one number (13.2, section 14). |
| I16 | Connection protocol packets never enter this layer. CONFIG is a control record outside every channel. |
| I17 | Messages, not packets, in one list per update with the same lifetime as the transport's batch. Receipts and events are adjacent lists a shim can merge (11.1, 12). |

### 17.3 Departures from RakNet, each on purpose

1. **The receive capacity is kept by the sender.** The window and ring bounds
   of 6.2 replace RakNet's unbounded heaps and hole queue, because the
   transport acks before this layer looks (P2).
2. **Holdable reliable records carry a ring offset.** That is +2 bytes on codes
   3 and 4, so RELIABLE_ORDERED is 12 bytes against RakNet's 10. In return
   there is no receive allocator, no fragmentation, and blocks reassemble with
   no copy.
3. **Records are first-written in FIFO order within each channel.** Priority
   boosts the whole channel.
4. **Round robin across channels within a priority level.**
5. **Only the newest sequenced message ahead of its epoch is held** (7.3).
6. **One delivery path for the watermark**, which avoids the study's items 18
   and 19.
7. **Blocks are placed by index and acked when every fragment is acked.** This
   fixes the study's §8.2 and §7.3 behaviour.
8. **Large unreliable messages stay unreliable** and ride the transport's
   fragments. The receipt type is the one exception.
9. **An RTO with exponential back-off** from this layer's own samples.
10. **Out-of-range arguments are refused, never repaired** (study §5.2 step 1,
    item 20).
11. **A wire violation disconnects.** RakNet drops the rest of the datagram
    (study §4.2), but here the transport has already acked the packet.
12. **The application's messages carry no message-id byte.** The shim decides
    that.

## 18. Order of work, open choices, not done

### 18.1 Cards, in order

Each card is secured before the next starts, as Glenn's standing practice
requires.

- **C0.** Fix F1 (`memset` of the window). TLA module 1 (the inventory's
  replay module) shows the trace first.
- **C0a.** Add `rocketnet_server_transport_client_connection_id` and the
  per-entry connection ids of the payload and acked batches (P9a, P9b), with a
  test that releases and reassigns a slot inside one update and checks that
  the released connection's payloads and acked ids carry its id, not the new
  one's.
- **C0b.** Transport tests that pin P2 and P6: an acked unfragmented id means
  the payload was in exactly one batch; a reply is never seen in the update
  that elicited it. Both on every driver.
- **C1.** Layer 1: region carve, send store, and
  `rocketnet_message_connection_bytes`.
- **C2.** Layer 2: codec, checks, vectors and fuzz target. The STANDARD.md
  _Messages_ section lands with it (G4).
- **C3.** Layer 3: window, ring, sent map, RTO; `RNWindow`, `RNRing`.
- **C4.** Layer 4: channels and dispatch, codes 0 and 3 enabled; `RNChannel`.
- **C5.** Layer 5: blocks; `RNBlocks`.
- **C6.** Layer 6: scheduler and builder; `RNScheduler`.
- **C7.** Layer 7: endpoint and slots machines, with socketless tests;
  `RNEndpoint`, `RNSlots`.
- **C8.** Layer 8: objects, loopback functional tests, the swarm soak with
  messages.
- **C9.** Layer 9: the C API and the C# binding smoke. **The space game
  integrates here.**
- **C10 onward**, one card per type, in this order: 2, 6, 7, 1, 4, 5. Each
  card adds `RNReceipts` and the dispatch tests where needed.
- **C-stress.** `ROCKETNET_MAX_CHANNELS` at 1024 and 10,000 under the swarm.

### 18.2 Choices for Glenn

- **G1. Identical-and-verified configuration** (CONFIG) or negotiated
  capacities. Negotiation would mean each side announcing its receive
  capacities reliably and the sender waiting for them before numbering past a
  safe default: one more transition, and slower first sends. This document
  recommends identical-and-verified.
- **G2. The defaults and their cost** (section 14), about 438 KiB per
  connection, and whether the game wants the lean profile.
- **G3. No congestion control in the first version.** The sender is bounded
  by `W`, `B` and `max_packets_per_update`, and RTO back-off limits resend
  storms. RakNet's sliding window (study §9) could sit at layer 6 later
  without a wire change.
- **G4. STANDARD.md's scope.** It specifies the transport today, and S:7-10
  says messages are specified nowhere in it. Section 13
  here is written to drop in as a _Messages_ section. Whether the standard
  grows to cover messages is Glenn's ruling. The C2 card (layer 2) added
  STANDARD.md _Messages_ with the wire only, behaviour staying here, as its
  card asked; it is one section to revert if the ruling goes the other way.
- **G5. The fragment stride** of 1152. 1168 is the largest stride that fits,
  and 1152 leaves 16 bytes of header headroom.
- **G6. A wire violation disconnects** (R6). The alternative, counting and
  dropping the rest of the payload, would let a malformed packet lose reliable
  data silently.
- **G7. The default send queue** (card C1). At the defaults the store holds 451
  chunks and `Q = 1024`, so a send is always refused by the store first and
  `MESSAGE_SEND_QUEUE_FULL` cannot fire: 573 descriptors (36 KiB per
  connection) can never be used. A default `Q` at or below the store's chunk
  count, such as 448, would make both bounds reachable and save 36 KiB per
  connection. Keeping 1024 leaves headroom for an application that raises `S`.
  This card keeps the design's 1024 and records the fact.

### 18.3 Not done by this card

- Layers 1 and 2 are in the tree (sections 4 and 5). The modules in section 15
  are the plan, and `RNSlots` exists only as
  the draft in 15.1, run on a bench.
- The per-entry sizes in section 14 are pinned by layer 1's static asserts.
- P2 and P6 are kept by
  `test_transport_acked_unfragmented_was_in_exactly_one_batch` and
  `test_transport_reply_opens_a_later_update`, each in `sdk/rocketnet_tests.cpp`
  (in-memory simulator) and `functional/rocketnet_functional_tests.cpp` (real
  loopback), card C0b, the commit that adds them.
- The RTO behaviour when acks are paced only by keep-alives was reasoned, not
  measured.
- The C# shim surface is still the study's provisional §13.
- The layer list (section 3) has not yet gone to Glenn for correction.

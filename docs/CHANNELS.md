# Messages, channels and receipts

RocketNet carries opaque application messages over an authenticated client and
server connection. `sdk/rocketnet.h` is the public API authority and
[standard/MESSAGES.md](../standard/MESSAGES.md) is the wire authority.

## Frame order

Each endpoint uses the same bounded frame order:

1. call `rocketnet_client_update` or `rocketnet_server_update` with the
   caller's monotonic time;
2. drain the endpoint-wide queue with `rocketnet_client_receive_messages` or
   `rocketnet_server_receive_messages` until it returns NULL;
3. run the game simulation;
4. reserve, write and finish outgoing messages;
5. call `rocketnet_client_send_packets` or `rocketnet_server_send_packets`
   last.

One `rocketnet_message_batch_t` contains parallel arrays for client index,
channel, reliability, bytes and data. Server entries identify the sending
client; client entries use client index 0. An entry's reliability is the mode
the message arrived as: a receipt mode arrives without its receipt, and an
unreliable message promoted for its size arrives as its reliable type. Data
pointers remain valid until the
next receive call or update on the same endpoint, whichever comes first.
There is no release call and no per-channel receive call.

## Sending

`start_message` reserves up to the declared byte count. The caller checks that
the returned pointer is non-NULL, writes into the returned storage, and calls
`finish_message` with the number written, or `abort_message`. A client keeps
one reservation open at a time. A server keeps at most one open per client
index, and a server used from one thread keeps one open at a time. Finishing
returns a nonzero
receipt serial. A refused start returns NULL; calling `finish_message` without
a live reservation (including after `start_message` returned NULL or after a
disconnect or slot reset cancelled the reservation) is an application error
that asserts in a debug build and returns zero in a release build. A refused
finish with a live reservation (a byte count outside 1 to the bytes started)
returns zero and aborts the pending reservation, so the next start is never
blocked.

The server start call takes a `uint64_t` destination id; the server finish and
abort calls take none, acting on the one start pending on the whole server.
Game threads that each hold a reservation on their own clients finish and abort
by client index with `rocketnet_server_finish_client_message` and
`rocketnet_server_abort_client_message` ([threads](USAGE.md#threads)). Values below
`max_clients` name a connected client slot. Values with `ROCKETNET_FLOW_ID_BIT` are reserved for the
flow layer. Broadcast is an application loop over connected slots.

## Reliability

The eight values in `enum rocketnet_reliability_t` have these delivery rules.
Names in the table omit the `ROCKETNET_RELIABILITY_` prefix.

| Mode | Delivery while the connection survives | Channel behavior |
| --- | --- | --- |
| `UNRELIABLE` | At most once; may be lost | No ordering channel |
| `UNRELIABLE_SEQUENCED` | Newer state replaces older state; may be lost | Sequenced per channel |
| `RELIABLE` | Retransmitted until acknowledged | No ordering channel; no send-order guarantee |
| `RELIABLE_ORDERED` | Retransmitted until acknowledged; delivered in send order | Ordered per channel |
| `RELIABLE_SEQUENCED` | Retransmitted, but newer state may supersede older state | Sequenced per channel |
| `UNRELIABLE_WITH_ACK_RECEIPT` | Unreliable delivery plus ACKED or LOSS outcome | No ordering channel |
| `RELIABLE_WITH_ACK_RECEIPT` | Reliable delivery plus ACKED outcome | No ordering channel |
| `RELIABLE_ORDERED_WITH_ACK_RECEIPT` | Reliable ordered delivery plus ACKED outcome | Ordered per channel |

Reliability does not persist across a disconnect. If an application needs a
transaction to survive reconnect, it must track completion and make retries
safe at the application level.

All modes accept up to the configured `max_message_bytes`. Messages larger
than one packet are fragmented and reassembled by the reliable path:
`UNRELIABLE` promotes to `RELIABLE`, `UNRELIABLE_SEQUENCED` to
`RELIABLE_SEQUENCED`, and `UNRELIABLE_WITH_ACK_RECEIPT` to
`RELIABLE_WITH_ACK_RECEIPT`. For state that must never retransmit, keep its size
at or below `rocketnet_max_unfragmented_message_bytes( reliability, channel )`.
Plain unreliable messages have a compile-time limit of
`ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES` (1,411 bytes) before promotion.

Ordered and sequenced modes share a channel's ordering state. A sequenced
message in its current ordered turn is delivered as soon as it is complete
and newer than that channel's sequenced watermark. A newer message can
supersede an incomplete reliable-sequenced message. A sequenced message ahead
of its ordered turn waits for that turn; at most one such future state is held
per channel, newest wins. Put independent traffic on different channels to
avoid ordering dependencies, while remembering that channels still share the
connection's memory, reliable window, and send budget.

The resend delay starts at `message_resend_delay_initial_ms`, follows the RTT
estimator, and stays within `message_resend_delay_min_ms` and
`message_resend_delay_max_ms`. The estimator's first sample is the transport's
measured round trip as soon as the transport has one, so a record lost on its
first send is resent after a delay derived from the path rather than after the
initial delay. `reliable_message_timeout_ms` is 0 (off) by default; set
nonzero, it ends the connection
when its oldest unacknowledged reliable number has waited that long since its
first send, outage or not, so choose it well above the connection timeout
([surviving a network outage](USAGE.md#surviving-a-network-outage)). It is not a deadline from `finish_message`: work waiting for its
first send has no reliable-send timeout, and each fragment has its own first
send time. `packets_per_update` bounds packet work per connection per update.

## Receipts and stats

Only the three `*_WITH_ACK_RECEIPT` modes generate outcomes, although every
successful finish returns a nonzero serial. Read outcomes with
`rocketnet_client_receipts` or `rocketnet_server_receipts`. Match serials within
their endpoint and connection; a reconnect starts fresh message state.

ACKED means the receiver acknowledged the containing packet or all fragments; it
does not confirm that the application committed a transaction. LOSS is only
reported for unpromoted unreliable receipt sends. It can reflect a missing
acknowledgement even if the receiver received the data. A promoted or explicitly
reliable receipt send produces ACKED only; no in-flight send receives a
terminal outcome when its connection ends.

An unreliable receipt's deadline is its packet send time plus
`message_resend_delay_max_ms`, not the shorter adaptive resend delay. A send
refused by the send budget or transport reports LOSS immediately; evicting an
entry from the sent-packet map can also resolve it as LOSS before that deadline.

The sent-packet map (`packets_in_flight` entries, at least
`reliable_messages_in_flight`) is shared by every packet that carries reliable
messages and every packet that carries only unreliable receipt sends. An
entry leaves only when it is acknowledged or when the map records the
`packets_in_flight`-th tracked packet sent after it; acknowledged entries behind an
older unacknowledged one keep their places until it leaves. So no entry leaves
before its acknowledgement lands while the map holds every tracked packet, of
both kinds and resends included, sent from any packet's send, that packet
included, until the processing of its acknowledgement: fewer than
`packets_in_flight` later tracked packets.
Past that, the oldest entry can be evicted while its acknowledgement is on its
way: the late acknowledgement is ignored, a reliable message in it is resent
and can hold back the reliable window until a later send is acknowledged, and
an unreliable receipt in it reports LOSS early. A steady excess
can stop reliable progress until `reliable_message_timeout_ms`, when set, ends
the connection. Size `packets_in_flight` for the most tracked packets the
connection sends over its longest delay from a send to the processing of its
acknowledgement, counting update and queue delay and bursts, not an average
rate. For example, at 60 Hz with up to 4 tracked packets an update and each
acknowledgement processed at the start of the update 1 s after its send,
before that update's sends, a packet and the packets after it before its
acknowledgement fill at most 60 updates of 4: 240 entries. If an update's sends
can come before the acknowledgement is processed, count that update too.

Receipt reads return the number available and copy only what fits. Iterate
up to `min(returned_count, capacity)`; rereading returns the same list rather
than consuming it. Read or copy outcomes before the next update. A full ring
drops its oldest outcome and increments `receipts_dropped`; choose
`max_message_receipts` for the application's peak outcomes between updates.
See [Usage](USAGE.md#receipts) for the complete read pattern.

The public stats distinguish ordinary disconnected sends
(`messages_dropped_not_connected`), budget drops
(`messages_dropped_over_budget`), and API misuse (`api_misuses`). Their fields
and units are defined in [rocketnet.h](../sdk/rocketnet.h).

## Memory and failure

Message stores, queues, delivery batches, receipt rings and replay windows
have bounded capacity derived from the public config. Their
storage lifecycle uses the caller allocator and a refused allocation is
reported. Message frame calls do not wait on the network. A start that finds
the connection's send memory full is refused: `start_message` returns `NULL`,
the refusal is counted in that connection's `send_refused`
(`struct rocketnet_conditions_t`), and the connection stays up, for unreliable
sends too; a later start succeeds once acknowledgements return room. A send
never ends a connection. A reliable message the receiver has no delivery room
for in this update does not end it either: the packet is left unacknowledged,
the sender resends it, and it is delivered once room returns
([receive pressure](RECEIVE-PRESSURE.md)). Only a broken internal bound on the
receive side ends a connection with `ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED`
(client state `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED`). Unreliable work may be
dropped at its documented bounded queues; queued reliable work is retained
while the connection survives. Use `can_send_message` to defer discretionary
sends before they are refused, and see
[refused sends](ERRORS-AND-LOGGING.md#a-game-policy-for-a-client-it-cannot-send-to)
for a game policy built on the refusal count.

## Queue scheduling

The message layer uses bounded storage with explicit identity and lifetime.
The current container choices are recorded in the
[owned storage manifest](SEQUENCE-BUFFERS.md#owned-storage-manifest):

| Queue | Representation | Why |
| --- | --- | --- |
| Per-channel send FIFO | Descriptor indices linked inside the fixed descriptor array | Constant-time tail append, front insertion for CONFIG, and head removal |
| Ready channels | Previous/next channel indices inside the fixed channel array | Constant-time insertion and removal of a named channel |
| Reliable resends | Slot indices linked inside the fixed reliable window | Constant-time insertion after a slot and interior removal |
| Unreliable sends | Bounded insertion-order ring with tickets and tombstones | Amortized constant-time push/pop; bounded compaction when full |
| Live send spans | Descriptor indices linked inside the fixed descriptor array | Reclaim the oldest span while acknowledgements can free spans in any order |

These index links are deliberate exceptions to the general preference for
flat arrays and rings. They do not allocate nodes or point outside their owning
array. Descriptor generations and connection epochs distinguish current entries
from stale handles. The [approved-link policy](SEQUENCE-BUFFERS.md#approved-index-links)
also states the limits of the textual audit; a clean audit is not a proof that
all code is link-free or that every operation meets a wall-clock target.

Use [memory planning](MEMORY.md) and [the storage model](LOAD.md) for capacity.
Do not copy a historical byte formula into a new allocator without checking
its config and build inputs.

## Verification

Ordering, sequencing, block fragmentation and reassembly, and receipts are
checked by the TLA+ models listed in the [model inventory](TLA-INVENTORY.md),
and the public calls, every reliability value, reset, capacity and
cross-endpoint behavior by the unit and functional suites.

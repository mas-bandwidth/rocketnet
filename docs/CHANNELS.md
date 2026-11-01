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
client; client entries use client index 0. Data pointers remain valid until the
next receive call or update on the same endpoint, whichever comes first.
There is no release call and no per-channel receive call.

## Sending

`start_message` reserves up to the declared byte count. The caller checks that
the returned pointer is non-NULL, writes into the returned storage, and calls
`finish_message` with the number written, or `abort_message`. Only one
reservation may be open on an endpoint object. Finishing returns a nonzero
receipt serial. A refused start returns NULL; calling `finish_message` without
a live reservation (including after `start_message` returned NULL or after a
disconnect or slot reset cancelled the reservation) is an application error
that asserts in a debug build and returns zero in a release build. A refused
finish with a live reservation (a byte count outside 1 to the bytes started)
returns zero and aborts the pending reservation, so the next start is never
blocked.

The server start call takes a `uint64_t` destination id; the server finish and
abort calls take none, acting on the one pending start. Values below
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
initial delay. `reliable_message_timeout_ms` ends the connection
when its oldest unacknowledged reliable number has waited that long since its
first send. It is not a deadline from `finish_message`: work waiting for its
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
reported. Message frame calls do not wait on the network. Client update can
briefly wait for a lock shared with the I/O thread. Exhausting connection message memory ends
that connection: the server's slot reads
`ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED` and the client's state
reads `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED`. Unreliable work may be
dropped at its documented bounded queues; queued reliable work is retained
while the connection survives. Use `can_send_message` to defer discretionary
sends before exhausting message memory.

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

The executable message invariants are split by concern:

- `proofs/tla/RNChannel.tla` and `proofs/tla/RNChannelTypes.tla`: ordering, sequencing and
  exactly-once delivery;
- `proofs/tla/RNBlocks.tla`: bounded block fragmentation and reassembly;
- `proofs/tla/RNReceipts.tla` and `proofs/tla/RNSentHistory.tla`: receipt and sent-history
  behavior;
- unit and functional suites: public calls, all reliability values, reset,
  capacity and cross-endpoint behavior.

The model configurations and `proofs/tla/RUNS.tsv` are the maintained execution
inventory.

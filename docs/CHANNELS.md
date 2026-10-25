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

`start_message` reserves up to the declared byte count. The caller writes into
the returned storage and calls `finish_message` with the number written, or
`abort_message`. Only one reservation may be open on an endpoint object.
Finishing returns a nonzero receipt serial. A refused start returns NULL; a
refused finish (a byte count outside 1 to the bytes started) returns zero and
aborts the pending reservation, so the next start is never blocked.

The server start call takes a `uint64_t` destination id; the server finish and
abort calls take none, acting on the one pending start. Values below
`max_clients` name a connected client slot. Values with `ROCKETNET_FLOW_ID_BIT` are reserved for the
flow layer. Broadcast is an application loop over connected slots.

## Reliability

The eight values in `enum rocketnet_reliability_t` provide unreliable,
sequenced, reliable, ordered and receipt-bearing combinations. Ordered and
sequenced state is per channel. Unreliable records are at most once. Reliable
records are retransmitted until acknowledged or the reliable timeout ends the
connection. Large messages use bounded fragmentation and reassembly sized at
create.

A sequenced message in its ordered turn follows one rule whether it is one
record or a reliable-sequenced block of fragments: it is delivered as soon as
it is complete and newer than the channel's sequenced watermark; a newer
in-turn message supersedes an in-turn block still collecting fragments, and
one no newer is discarded; a held future state never touches it. Only a
message ahead of its turn is held, at most one per channel, newest wins
(`tla/RNChannelTypes.tla`, `InTurnOneRule`).

The resend delay starts at `message_resend_delay_initial_ms`, follows the shared RTT
estimator, and remains within `message_resend_delay_min_ms` and
`message_resend_delay_max_ms`. `reliable_message_timeout_ms` bounds the complete reliable
operation. `packets_per_update` bounds packet work per connection per update.

## Receipts and stats

Receipt-bearing sends report `ROCKETNET_RECEIPT_STATUS_ACKED` or
`ROCKETNET_RECEIPT_STATUS_LOSS` through `rocketnet_client_receipts` or
`rocketnet_server_receipts`. Receipt serials are scoped to their endpoint.
A type-5 receipt reports LOSS only after its receipt deadline, the packet's
send time plus `message_resend_delay_max_ms`; an acknowledgement that arrives
after the adaptive resend delay but inside that deadline reports ACKED. A send
refused by the send budget or the transport reports LOSS at once, and an
entry evicted from the sent-packet map reports LOSS at the eviction, the one
exception (standard/MESSAGES.md m12).
The stats structs filled by `rocketnet_client_stats`, `rocketnet_server_stats`
and `rocketnet_server_client_stats` distinguish ordinary connection-state drops
(`messages_dropped_not_connected`), budget drops (`messages_dropped_over_budget`) and API misuse
(`api_misuses`). The field and enum comments in `sdk/rocketnet.h` define each
public stat and status exactly.

## Memory and failure

Message stores, queues, delivery batches, receipt rings, replay windows and
reassembly slots have bounded capacity derived from the public config. Their
storage lifecycle uses the caller allocator and a refused allocation is
reported. Message frame calls do not wait on the network. Client update can
briefly wait for a lock shared with the I/O thread. Exhausting connection message memory ends
that connection: the server's slot reads
`ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED` and the client's state
reads `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED`. Unreliable work may be
dropped at its documented bounded queues; reliable work is retained until it
is delivered or the connection reaches its timeout.

## Verification

The executable message invariants are split by concern:

- `tla/RNChannel.tla` and `tla/RNChannelTypes.tla`: ordering, sequencing and
  exactly-once delivery;
- `tla/RNBlocks.tla` and `tla/FragmentsAndAcks.tla`: bounded fragmentation,
  reassembly and acknowledgement coupling;
- `tla/RNReceipts.tla` and `tla/RNSentHistory.tla`: receipt and sent-history
  behavior;
- unit and functional suites: public calls, all reliability values, reset,
  capacity and cross-endpoint behavior.

The model configurations and `tla/RUNS.tsv` are the maintained execution
inventory.

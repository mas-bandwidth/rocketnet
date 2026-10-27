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

## Queue scheduling

Reliable resend, the per-channel descriptor FIFO, ready-channel membership, and
the unreliable endpoint FIFO name an entry by descriptor generation and the
connection's schedule epoch (`tla/RNChannelQueues.tla`). A carve restarts that
epoch at 1 and clears tickets, flags, and resend links, so a reused slot matches
a slot carved once. Inside the connection, freeing a descriptor bumps its
generation, and ACK, cancel, and expiry remove only the named generation. A resend slot is in at most one FIFO;
capacity is W, unlink is O(1), and an acked resend hole stays linked so a walk
still counts it. A channel or unreliable cancel of an interior entry marks a
hole in O(1). Holes compact in one pass when more than 64 are outstanding, or
once at the start of a write, never once per emitted record. That pass is
O(C + queued), not O(C times Q). Channel enqueue tickets are distinct because
reliable numbers are assigned later. CONFIG is spliced at the front of channel
0 and that one channel's tickets are rebased so CONFIG holds the smallest
ticket. Ready membership is one ticket per channel.

The sized terms are 16W for the resend link, 40C for channel send state, and
64C (40 bytes of channel send state plus 24 bytes of channel receive state).
A send descriptor is 80 bytes (`rn_send_descriptor_t`). `docs/LOAD.md` prints
`64C + 120.125W + B + 80Q + S + 128P + 24(W + Du) + 1440Ru + 16Rq + SR`.

`rn_channels_cancel_queued`, `rn_endpoint_cancel_unreliable`,
`rn_endpoint_expire_unreliable`, and `rn_window_ack_cancel_resend` are test
seams. The unit cases call them. Nothing under `sdk/` does, so a production
connection leaves `queue_holes` at 0 and does not run the hole compact from
the product path. The generation check those seams perform is the cancel and
expiry behaviour the model names.

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

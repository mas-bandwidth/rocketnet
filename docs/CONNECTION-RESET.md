# Connection reset inventory

A connection owns every item marked **RESET**. Ending a connection runs one
internal reset for that side before a slot or client object can be reused. A
**KEEP** row is object-lifetime state and includes the reason it survives.
`tla/ClientSession.tla` models the client `End` transition and
`tla/ServerSession.tla` models `EndSlot` and the per-slot `Reset(i)`.

The same decisions are data in `sdk/rocketnet_connection_reset.h`, one X-macro
per struct. Each of these structs is declared from a manifest list beside it
(`rocketnet_client_protocol_t`, `rocketnet_client_transport_t`,
`rocketnet_encryption_mapping_t`, `rocketnet_client_slots_t`, the replay mark
`rocketnet_server_replay_slot_mark_t`, `rn_endpoint_t` and
`rn_message_connection_t`), and `test_connection_reset_inventory_covers_every_field`
walks every one: a member with no RESET or KEEP decision fails the suite
naming `<struct>.<member>`.

## Client per-connection state

| Struct | Field | What it is | Decision | Reason |
|---|---|---|---|---|
| `rocketnet_client_protocol_t` | `packet_filter`, `send_rate`, `counters`, `defer_payload_acks` | configuration and this connection's counters | KEEP | the object is destroyed with the connection; public session counters are read from the connection that is held |
| | `state` | terminal state / disconnect reason | KEEP | observable until the next connection attempt |
| | `token`, `signed_fields`, `server_address` | parsed attempt and server | RESET | belongs to one attempt |
| | `connection_salt`, ephemeral keypair, `return_cookie`, `has_cookie` | handshake material | RESET | secret and attempt material must not cross attempts |
| | `session_nonce`, `keys`, `stored_response` | session key state | RESET | one session only; key copies retire before erasure |
| | `client_index`, `max_clients` | assigned server slot | RESET | learned by this session |
| | handshake/session sequences, fragment payload id | wire sequence spaces | RESET | fresh sequence spaces start at one |
| | replay windows, `integrity_failures` | receive security state | RESET | old traffic must not affect a new session |
| | attempt, send, receive and total deadlines | attempt clocks | RESET | token and session clocks |
| | RTT sample state and received sequence | quality state | RESET | per-session inputs |
| | `ack`, `num_acked`, `acked_packet_ids` | ack bitfields, packet rings and receipt batch | RESET | packet ids and measurements belong to one session |
| | `reassembly` | allocation owner | KEEP | allocation lives with the client; connection-0 slots reset |
| | key publication hooks and context | transport hooks | KEEP | transport wiring |
| | current record verdict and sequence | input record | RESET | no record is live after reset |
| `rocketnet_reassembly_set_t` | `slots[0]`, `newest_completed[0]` | fragment slots and stale watermark | RESET | partial payloads never cross sessions |
| | geometry, arenas and delivered capacity | fixed allocation | KEEP | client object storage |
| `rocketnet_ack_endpoint_t` | sequence heads, ack debt, sent/received records, bitmaps and aggregates | delivery and ack history | RESET | a new connection starts with no packet history |
| | RTT, jitter, loss and bandwidth estimates | connection quality | RESET | statistics describe one session |
| `rocketnet_client_transport_t` | payload rings, received batch and send arena contents | staged connection packets | RESET | invalid after the terminal transition |
| | packet-loss and jitter trackers | transport estimators | RESET | no samples from the ended session |
| | config, transport, I/O objects, lifetime counters and statistics rings | client-lifetime transport state | KEEP | construction state of this transport object; public session counters are the connection that is held |
| | `time`, `raw_time`, `time_offset` | the update clock, the caller's raw time and the backwards offset | KEEP | the game's clock is client-lifetime state; a connection reset does not rewind it |
| | `link_simulator`, `link_simulator_from` | the simulated link and its source address | KEEP | a property of the link, not of a connection; packets already on it are on the wire, sealed under the ended session |
| `rn_message_connection_t` / `rn_endpoint_t` | endpoint region; ordered/reliable queues; descriptors; receive ring; send store; packet, delivery, hold, receipt and resend rings | channel and allocator state | RESET | `rn_endpoint_reset` carves the region fresh |
| | packet/message serials, ordering/sequencing indices, unreliable queue, pending start, write pass and send-budget accumulator | message send state | RESET | no work may cross connections |
| | `views`, `cursors` | allocation pointers | KEEP | storage is fixed; pointed-to contents reset |
| | `connection_id`, `state`, `held` | message slot binding | RESET | slot becomes free |
| | `disconnect_reason` | last message-layer reason | KEEP | the public client state carries it until disconnect or the next connect |

The disconnect burst is sealed before reset. The client transport copies the
destination address locally, sends only that burst after reset, and retains no
other session state.

## Server per-slot state

| Struct | Field | What it is | Decision | Reason |
|---|---|---|---|---|
| `rocketnet_encryption_mapping_t` | keys, salt, nonce and ephemeral public keys | session cryptography | RESET | mapping removal wipes secrets and unlinks the entry; assignment initializes the next mapping |
| | challenge packet and token record | challenge/cookie/token state | RESET | one handshake |
| | client id, user data | slot identity | RESET | identity cannot leak to reuse |
| | control pointer | parallel timeout-control allocation | KEEP | map owner retains the indexed allocation |
| | send times | clocks and pacing | RESET | one mapping |
| | handshake/session sequences and fragment id | packet sequence spaces | RESET | new mapping starts fresh |
| | replay windows and integrity failures | receive security history | RESET | old ciphertext cannot affect a new mapping |
| `rocketnet_encryption_mapping_control_t` | client index, established | slot identity | RESET | identity cannot leak to reuse |
| | timeout, handshake deadline and receive time | expiry clocks | RESET | one mapping |
| `rocketnet_client_slots_t` | per-index identity, mapping, confirmation, user data and hash link | slot record | RESET | slot release clears the index |
| | hash key, buckets and free-stack geometry | table owner | KEEP | server-lifetime allocation |
| `rocketnet_server_acks_t` | `endpoints[client_index]` | ack rings, bitfields and quality estimates | RESET | packet history is per connection |
| | acked batch rows for this index | pending receipts | RESET | compacted out before reuse |
| | other endpoints and rows | other clients | KEEP | reset touches only its slot |
| `rocketnet_reassembly_set_t` | slot slice and newest watermark for this index | partial fragments | RESET | partial payloads never cross clients |
| | other slices and geometry | other clients / fixed storage | KEEP | reset touches only its slot |
| `rocketnet_server_protocol_t` | `connection_ids[client_index]` | slot incarnation tag | RESET | free slots have id zero |
| | `next_connection_id` | lifetime id source | KEEP | ids never repeat in one server life |
| | component pointers | allocation owners | KEEP | their named per-slot records reset |
| | update output tags | receipt attribution | KEEP | produced rows retain attribution for the update |
| server replay marks | marks for this client index | staged replay ownership | RESET | transport cleanup removes this slot's marks |
| `rocketnet_server_transport_t` | `client_wire_totals[client_index]` | the slot's wire packet and byte totals | KEEP through the reset | not touched by `rocketnet_server_connection_reset`; cleared when the slot is next assigned (`rocketnet_server_wire_totals_assign`, which zeroes them when the connecting response assigns the slot), and deferred disconnect frames sealed after reset carry `client_index = -1` so they never charge a reused slot. `rocketnet_server_transport_client_stats` reads zeros for an empty slot, so the ended connection's totals are never observable |
| | other slots, server totals, transport/config and counters | server-wide state | KEEP | lifetime state or another connection |
| message connection / endpoint | the client table's message and channel fields | per-slot message state | RESET | endpoint reset makes the slot fresh |
| | `disconnect_reason` | last connection's reason | KEEP | cleared only when a new id is observed |

The server reset wipes the encryption mapping's secrets and removes it while the slot still
names it, releases identity and user data, clears the connection id, compacts
the slot's pending ack rows, resets its ack endpoint, and finally resets only
that slot's fragment slice. Message storage resets when the message object
observes the transport connection-id change.

A deferred server disconnect burst has no per-client wire-total owner after
the reset. Its datagrams increment the server-lifetime totals exactly once,
but never the totals of a later connection that reuses the ended connection's
slot. An immediate disconnect burst is committed before reset and therefore
still increments the ending connection's totals. Assignment continues to
clear the slot totals, and an empty-slot stats query continues to return zero.

## Server-wide state

| Struct | Field | Decision | Reason |
|---|---|---|---|
| server protocol | protocol/filter, server keys, advertised addresses, cookie keys and rotation state | KEEP | server identity and configuration |
| | used-token history | KEEP | the standard's rationale (`standard/RATIONALE.md`) says, “Entries expire with the tokens they name”; token lifetime is independent of stop/start |
| | signature verification budget and counters | KEEP | shared abuse budget and lifetime accounting |
| server transport | transport, I/O owner, config, whole-server wire totals and stats | KEEP | server-wide or lifetime state |
| message object | config, arena owner, aggregate counters, clock and transport link | KEEP | object lifetime |

The client and encryption mapping reset `accepted_session` independently of
`replay_session`. ACK eligibility never survives connection reuse. The client
keeps `defer_payload_acks`, because message-layer ACK admission is a transport
binding policy, not state learned from the previous remote endpoint.

## Secret erasure versus generation invalidation

An epoch or generation increment invalidates stale control, packet, ACK, and
message records, but it is never treated as secret erasure. Actual secret
material is overwritten in place with `sodium_memzero`:

- Client protocol reset (`rocketnet_client_protocol_wipe_secrets`):
  `keys` (`session_client_to_server`, `session_server_to_client`, `cookie_key`),
  `token`, `client_ephemeral_secret_key` (also erased immediately after
  challenge processing), and `session_nonce`.
- Client I/O open-key copy (`rocketnet_client_io_drop_keys`):
  `session_server_to_client`, `connection_salt`, and `packet_filter`. When a
  background I/O thread owns `client_io.open`, `retire_keys` publishes
  `ROCKETNET_CLIENT_IO_EPOCH_RETIRED` and the worker calls
  `rocketnet_client_io_take_keys` / `rocketnet_client_io_drop_keys` on its next
  wakeup. When no worker thread exists (`!client_transport->thread`, including
  `step_driven` and `step_in_update` modes), `rocketnet_client_transport_retire_keys`
  calls `rocketnet_client_io_take_keys` immediately on the caller thread so
  `client_io.open.session_server_to_client` is wiped with `sodium_memzero` at
  the terminal transition without waiting for a later `update` or `stop`.
- Server encryption mapping and slot release:
  `rocketnet_encryption_mapping_wipe_secrets` zeroes `keys`, `session_nonce`,
  and `user_data` before unlinking the mapping; `rocketnet_client_slots_release`
  zeroes the slot's `user_data` copy; and deferred server disconnect bursts copy
  only `session_server_to_client`, `connection_salt`, and `sequence` into the
  bounded burst entry and erase that entry with `sodium_memzero` as soon as the
  burst drains or is replaced. The key is stored at the ring index where the
  burst is appended, and the completing drain wipes `disconnect_burst_keys` at
  `disconnect_burst_head`, not at the ended connection's client slot.

## Statistics lifecycle and public counter contract

- **Session-scoped quality and rate metrics (RESET)**: RTT (`rtt_ms`), jitter
  (`jitter_ms`), packet loss (`packet_loss_percent`), bandwidth estimates
  (`send_kbps`, `receive_kbps`, `ack_kbps`), and the underlying ACK history
  rings, bitmaps, and loss/jitter trackers reset on every client connection reset
  and every server slot reset (`rocketnet_ack_endpoint_reset`,
  `rocketnet_packet_loss_tracker_reset`, `rocketnet_jitter_tracker_reset`).
- **Server per-client totals (`rocketnet_server_client_stats`)**: Scoped strictly
  to the currently connected occupant of `client_index`. Queries on an empty
  slot return all zeros; `rocketnet_server_wire_totals_assign` zeroes the slot's
  wire totals when a new connection claims the slot; and deferred disconnect
  bursts drained after slot release carry `client_index = -1` so they charge
  server-lifetime wire totals without charging a reused slot.
- **Public client session counters (`rocketnet_client_stats`)**: `packets_sent`,
  `packets_received`, `bytes_sent`, `bytes_received`, `payloads_received`,
  `messages_resent`, `messages_duplicated`, `receipts_acked`, `receipts_lost`,
  `receipts_dropped`, and `messages_dropped_over_budget` are the connection
  that is held. A new connect, including a reconnect to the same or a different
  server, does not carry the ended connection's totals. With no connection
  held they read 0. `messages_dropped_not_connected` stays on the client handle
  and remains monotonic across connections. Rate and quality gauges stay
  session-scoped and read 0 without a connection. The public struct is
  unchanged. `tla/ClientSession.tla` `End` leaves `kept` unchanged, and
  `kept` includes `lifetime`. `HandshakeStep` into `CONNECTED` increments
  `kept.lifetime`. That model text does not describe this getter.

## Reset state without clearing bulk storage

RESET means a fresh logical state, not zeroing every byte of an allocation.
Reset counters, validity bits, queue heads, generation tags, ownership and
per-connection configuration state before the next client can use a slot.
Payload buffers, unused descriptors and ACK history records may retain bytes;
readers must consult initialized counts or validity bits before reading them,
and a new record must initialize every field its readers use.

Destroy does not wipe entire client/server blocks, message arenas, packet
buffers or statistics storage. It wipes keys, tokens and other secret-bearing
fields. Slot assignment writes the complete connect-token user-data record;
release wipes that record, and destroy wipes only records still connected.
Unused slots are never cleared just to free them. A server's next start builds
fresh state, so destroy does not reset each message slot before freeing it.

`test_connection_reset_dirty_slot_discards_open_aggregate` reuses a server slot
after an unfinished unreliable aggregate and pending reservation. It keeps
canaries in the receive ring and send store across reset, checks that no old
receipts or aggregate survive, and verifies exact single delivery in both
directions for all eight delivery modes. The existing reset inventory and
slot-reuse tests cover field decisions, ACK/replay state, partial fragments,
timeout exits and isolation from other connected slots. The secret-wipe tests
(`test_unit_secrets_wiped.cpp`, including
`test_secrets_wiped_on_disconnect_and_reconnect_without_arena_zeroing`) use a
nonzero allocator and inspect memory across disconnect, reconnect, and destroy;
they verify both immediate `sodium_memzero` secret erasure and the absence of
whole-arena writes.

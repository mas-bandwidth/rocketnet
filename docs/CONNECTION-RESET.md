# Connection reset inventory

A connection owns every item marked **RESET**. Ending a connection runs one
internal reset for that side before a slot or client object can be reused. A
**KEEP** row is object-lifetime state and includes the reason it survives.
`tla/ClientSession.tla` models the client `End` transition and
`tla/ServerSession.tla` models `EndClient` and the per-slot `Reset(i)`.

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
| `rocketnet_client_protocol_t` | `packet_filter`, `send_rate`, `counters` | configuration and lifetime counter owner | KEEP | public counters are monotonic over the client life |
| | `state` | terminal state / disconnect reason | KEEP | observable until the next connection attempt |
| | `token`, `signed_fields`, `server_address` | parsed attempt and peer | RESET | belongs to one attempt |
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
| `rocketnet_ack_endpoint_t` | sequence heads, ack debt, sent/received records, bitmaps and aggregates | delivery and ack history | RESET | a new peer starts with no packet history |
| | RTT, jitter, loss and bandwidth estimates | connection quality | RESET | statistics describe one session |
| `rocketnet_client_transport_t` | payload rings, received batch and send arena contents | staged connection packets | RESET | invalid after the terminal transition |
| | packet-loss and jitter trackers | transport estimators | RESET | no samples from the ended session |
| | config, transport, I/O objects, lifetime counters and statistics rings | client-lifetime transport state | KEEP | construction and public monotonic statistics |
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
| `rocketnet_encryption_mapping_t` | keys, salt, nonce and ephemeral public keys | session cryptography | RESET | mapping removal zeroes the mapping |
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
| `rocketnet_server_transport_t` | `client_wire_totals[client_index]` | the slot's wire packet and byte totals | KEEP through the reset | not touched by `rocketnet_server_connection_reset`; cleared when the slot is next assigned (`rocketnet_server_wire_totals_assign`), which also folds in the new client's pre-slot handshake totals. `rocketnet_server_transport_client_stats` reads zeros for an empty slot, so the ended connection's totals are never observable |
| | other slots, server totals, transport/config and counters | server-wide state | KEEP | lifetime state or another connection |
| message connection / endpoint | the client table's message and channel fields | per-slot message state | RESET | endpoint reset makes the slot fresh |
| | `disconnect_reason` | last connection's reason | KEEP | cleared only when a new id is observed |

The server reset removes and zeroes the encryption mapping while the slot still
names it, releases identity and user data, clears the connection id, compacts
the slot's pending ack rows, resets its ack endpoint, and finally resets only
that slot's fragment slice. Message storage resets when the message object
observes the transport connection-id change.

## Server-wide state

| Struct | Field | Decision | Reason |
|---|---|---|---|
| server protocol | protocol/filter, server keys, advertised addresses, cookie keys and rotation state | KEEP | server identity and configuration |
| | used-token history | KEEP | STANDARD says, “Entries expire with the tokens they name”; token lifetime is independent of stop/start |
| | signature verification budget and counters | KEEP | shared abuse budget and lifetime accounting |
| server transport | transport, I/O owner, config, whole-server wire totals and stats | KEEP | server-wide or lifetime state |
| message object | config, arena owner, aggregate counters, clock and transport link | KEEP | object lifetime |

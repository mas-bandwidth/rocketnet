# TLA+ inventory: every state machine in rocketnet

Written 2026-10-05 from a cold read of the code at `1ecc4d2` (main), for the
standing practice: a TLA+ module beside every state machine, variables the
state it owns, actions the operations and the outside events, invariants and
liveness from the rules the design must keep, checked with TLC on a small
instance on a bench, every counterexample verified against the code by hand,
the model cited from the code.

This file is the inventory the model cards start from. It writes no module
and runs nothing. For each machine: the states, the transitions with the
`file:line` of the code that makes them, the variables it owns, the rules it
must keep (each traced to a sentence in `STANDARD.md`, `USAGE.md` or a
comment), and a verdict. The ordered list of modules to write, with the small
instance each should check and its `MC<Module>.tla` / `.cfg`, is at the end,
followed by what the read found that is not a model.

Line numbers are against `1ecc4d2`. `S:` is a `STANDARD.md` line, `U:` a
`USAGE.md` line, `C:` a `CLAUDE.md` line.

Reading order, as the card asked: `STANDARD.md` and `USAGE.md` whole, then
`sdk/rocketnet_client_transport.cpp`, `rocketnet_client_protocol.cpp`,
`rocketnet_server_transport.cpp`, `rocketnet_server_protocol.cpp`,
`rocketnet_client_slots.cpp`, `rocketnet_token_tables.cpp`,
`rocketnet_replay.cpp`, `rocketnet_fragment.cpp`, `rocketnet_ack.cpp`,
`rocketnet_frame_sync.cpp`, `rocketnet_server_socket_linux.cpp`,
`rocketnet_server_gso_linux.cpp`, `rocketnet_server_iocp_windows.cpp`,
`server_xdp/server_xdp.c`, with `rocketnet_encryption_map.cpp`,
`rocketnet_driver.cpp`, `rocketnet_driver_builtin.cpp`,
`rocketnet_packet_lease.h`, `rocketnet_pending_frames.h`,
`rocketnet_send_handoff.h` and the headers of each read alongside.

---

## The machines found

| # | Machine | Lives in | Verdict |
|---|---|---|---|
| 1 | Client connection | `rocketnet_client_protocol.cpp` | model |
| 2 | Server mapping and slot lifecycle | `rocketnet_server_protocol.cpp`, `rocketnet_encryption_map.cpp`, `rocketnet_client_slots.cpp` | model |
| 3 | The handshake, both ends over a lossy wire | 1 + 2 + `rocketnet_protocol.cpp` | model (the headline module) |
| 4 | Keep-alive generation and the acknowledgment latch | `rocketnet_server_protocol.cpp`, `rocketnet_ack.cpp` | model |
| 5 | Replay window | `rocketnet_replay.cpp` | model (defect found by reading, below) |
| 6 | Ack block: build, validate, apply; sent-record ring; aggregates | `rocketnet_ack.cpp` | model |
| 7 | Used-token history and admitted-token set | `rocketnet_token_tables.cpp` | model |
| 8 | Cookie gate and cookie key rotation | `rocketnet_server_protocol.cpp` | model the gate inside 3; rotation is not implemented (gap, below) |
| 9 | Fragment reassembly slot | `rocketnet_fragment.cpp` | model |
| 10 | Fragmented send: sequence block, arena, all-or-nothing frames | `rocketnet_*_transport.cpp` finish paths | fold into 6; no separate module |
| 11 | Server transport lifecycle RUNNING / STOPPING / STOPPED and driver stop | `rocketnet_server_transport.cpp`, `rocketnet_driver_builtin.cpp`, XDP, IOCP | model (one small module) |
| 12 | XDP send handoff generations | `rocketnet_send_handoff.h`, `rocketnet_server_socket_linux.cpp` | model |
| 13 | Receive double buffer flip (XDP, GSO, IOCP, client) | the four drivers and `rocketnet_client_transport.cpp` | model (one module, four instances) |
| 14 | XDP umem frame conservation and the pending-return spill | `rocketnet_pending_frames.h`, XDP thread | model |
| 15 | Packet leases | `rocketnet_packet_lease.h` | model, later |
| 16 | IOCP overlapped operation pool | `rocketnet_server_iocp_windows.cpp` | fold into 11 and 13 |
| 17 | GSO feature tiers and live degradation | `rocketnet_server_gso_linux.cpp` | not now |
| 18 | Frame sync deadline loop | `rocketnet_frame_sync.cpp` | no |
| 19 | Driver ladder | `rocketnet_*_transport_create`, `rocketnet_driver.cpp` | no |
| 20 | Prefilter and the kernel filter | `rocketnet_protocol.cpp`, `server_xdp.c` | no (a pure predicate; a conformance equivalence, not a machine) |
| 21 | Statistics windows, loss and jitter trackers, rtt history | transports, `rocketnet_ack.cpp` | no |
| 22 | Reassembly delivered window and the receive batch | `rocketnet_fragment.cpp`, transports | fold into 9 |

Global constants every model shares (`rocketnet_constants.h`, `rocketnet_wire.h`,
`rocketnet_history.h`): send budget 1232, receive ceiling 1426, overhead 26,
ack block min 7, max ranges 255, history 4096 (4096..32768 by build), sent
buffer = history, received buffer 2048, rtt history 2048, integrity limit
2^36, ack debt threshold 64, max fragments 256, max fragment 1193 send /
1387 receive, send arena slots 16, default reassembly slots 8, client packet
batch 1024, server receive 16 per client per update, disconnect burst 10,
send rate 10 Hz, handshake deadline 5 s, cookie lifetime 3 s.

---

## 1. Client connection

The one machine the standard draws (S:2025-2121). The code's states are the
standard's twelve integers (`rocketnet.h:398-409`).

### States

`-7 connect token expired`, `-6 invalid connect token`, `-5 connection timed
out`, `-4 connection response timed out`, `-3 connection request timed out`,
`-2 cookie request timed out`, `-1 connection denied`, `0 disconnected`,
`1 requesting cookie`, `2 sending connection request`, `3 sending connection
response`, `4 connected`.

### Transitions

Operation or event → code:

- `connect(token, time, timestamp)`: wipes the attempt, fresh salt and ephemeral, sequences at 1 (`rocketnet_client_protocol.cpp:123-134,181-184`); bad flags / create after expire / wrong version → `-6` (`141-149`); expired on arrival → `-7` (`151-156`); else → `1` (`192`). Only entry into the positive range.
- `update(time)` (the application calls, every frame): `check_deadlines` first (`285`): in `1..3`, `time - attempt_start >= expire - create` → `-7` (`206-211`); in `1..4`, `timeout_seconds > 0` and `time - last_receive >= timeout_seconds` → `-(state+1)` (`214-220`). Then the one sending rule: nothing before `1/send_rate` has elapsed since `last_send_time` (`297`), except a standalone ack in `4` when `ack_debt >= 64` (`299-310`); then the state's packet: request in `1` and `2` (with the cookie once held, `321-333`), the stored response byte-identical in `3` (`335-342`), a keep-alive in `4` (`344-356`).
- cookie arrives: processed in `1` and `2` only (`372-378`); wrong size refused (`380-385`); salt mismatch refused (`391-396`); stored, replacing any held (`402-404`), `last_receive` refreshed (`404`); `1 → 2` (`409-413`). Never sends (the next scheduled request carries it).
- challenge arrives: processed in `2` only (`431-436`); size / zero sequence / open failure refused (`438-483`), open failures count toward the integrity limit (`442,481`); small-order server ephemeral refused (`489-500`); commit nonce, keys, erase ephemeral secret (`508-509`); build and store the response once at handshake sequence 1 (`518-527`); `2 → 3` (`531`).
- denied arrives: processed in `2` and `3` (`547-553`); in `3` opened with the held keys, in `2` with temporaries from the carried nonce (`581-602`); on open `→ -1` (`614`).
- session packet arrives (keep-alive 4, payload 5, disconnect 6, ack 8, fragment 9): the shared opener `rocketnet_client_open_session_packet` (`642-769`): wire floor (`660-665`), state gate `3` or `4` (`670-676`), sequence reconstruction and zero refusal (`679-685`), replay check (`687-692`), open (`694-704`, failure → integrity count), replay advance (`711`), ack block read (`719-725`), opened size (`729-735`); only then `last_receive` refresh (`743`), `record_received` and `process_block` (`756-761`), ack debt for every type but 8 (`763-764`). Then per type: keep-alive in `3` stores index and max clients, `3 → 4` (`803-808`); payload delivered in `3` or `4` (`843-846`); ack nothing more (`877`); fragment to reassembly (`934-936`); disconnect `→ 0` (`963`).
- integrity limit reached (2^36 opens failed this attempt, any key): `→ 0`, keys and ephemeral zeroed (`55-67`).
- `disconnect()` (the application calls): in `3` or `4` a burst of 10 sealed disconnects at consecutive session sequences (`1309-1330`), then `→ 0` from any state (`1332-1333`), keys zeroed (`1335-1336`).
- `start_payload` / `seal_payload_frame` / `seal_fragment` (fill threads): refused unless `4` (`1056-1057,1074-1075,1203-1204`); a sequence is spent at `start_payload` (`1059`) whatever follows (U:126-137); the seal resets ack debt and stamps `last_send_time` (`1103-1105`).
- The transport above: `rocketnet_client_transport_update` drains the receive buffer, runs `protocol_update`, sends the one packet, and fires the state-change callback once per crossing (`rocketnet_client_transport.cpp:546-605`); packets not from the dialed server are dropped before the protocol sees them (`402-403`); the receive thread stages into the off buffer until `quit` (`1099-1150`), joined at destroy (`377-382`).

### Variables (the state the code owns)

`state`; attempt: `connection_salt`, `client_ephemeral_{public,secret}_key`, `return_cookie`, `has_cookie`; `session_nonce`, `keys` (four), `stored_response`; `client_index`, `max_clients`; `send_sequence_handshake`, `send_sequence_session`, `next_fragment_payload_id`; `replay_handshake`, `replay_session`; `integrity_failures`; clocks `attempt_start_time`, `last_send_time`, `last_receive_time`, `total_deadline_seconds`, `timeout_seconds`; the ack endpoint (`ack`, `num_acked`, `acked_packet_ids`); `reassembly` (machine 9). (`rocketnet_client_protocol.h:32-112`.)

### Rules it must keep

Safety:

- **Monotone climb, no sideways move**: within one attempt `state` only ever goes `0 → 1 → 2 → 3 → 4` or from a positive state to a negative one; no negative state leaves except by `connect` (S:2043-2053 "The machine steps forward or fails; it never moves sideways ... entering each exactly once per attempt").
- **The pairing**: a timeout from state `N > 0` lands in `-(N+1)` and nowhere else; `-1`, `-6`, `-7` are reached only by their named causes (S:2075-2084, "An implementation SHOULD assert the pairing"). The code computes `-(state+1)` (`217`): the invariant is that the enum stays dense.
- **Handshake is the range `0 < state < 4`** (S:2086-2089): the total deadline fires only there (`206`); the token's timeout fires in `1..4`.
- **Keys exist iff `state >= 3`**; the ephemeral secret is zero from the challenge on (S:1090-1093, S:2176-2177); after any exit to `<= 0` the keys are zero (`65-66,1335-1336`).
- **Sequences never reset within an attempt; both start at 1** (S:2111-2115). `send_sequence_handshake` is spent exactly once (the stored response, `527`).
- **Nothing is sent because a packet arrived** (S:1261-1263, S:2179-2183): a send happens only in `update`, and only when the rate allows, except the standalone ack (S:1819-1828). Model this as: the number of sends in any interval of length `1/send_rate` is at most 1 plus standalone acks.
- **A cookie whose salt is not this attempt's is ignored** (S:1269-1278; `391-396`).
- **A challenge is acted on in `2` only; a denied in `2` and `3` only; session types in `3` and `4` only** (S:2145, 2162, 2187, 2200-2202).
- **Replay marked before the ack block and size checks, effects after** (S:1481-1497; `711` then `719-743`).
- **The cookie is replaced, not kept** (S:2150-2154).
- `-7` on arrival for an expired token, before anything is sent (S:2079-2082).

Liveness (with fairness on `update` and on the wire delivering some copy of each retransmitted packet):

- From `1`, with a responsive server, eventually `4` (the loss walkthrough, S:2224-2258: "every wait state retransmits ... There is no packet whose loss leaves either end waiting").
- From any positive state with a silent server, eventually a negative state (the deadlines, S:2091-2101, S:2914-2917).
- From `4`, a disconnect packet or `disconnect()` eventually reaches `0`.

### Verdict: model.

What it would catch: a state added without its timeout pairing; a send on an arrival path (amplification); an event accepted in a state the standard excludes; a key or sequence surviving into a new attempt; a deadline that cannot fire from some state. On its own it is cheap (the server is an oracle that answers from a small menu). It is also half of module 3.

---

## 2. Server mapping and slot lifecycle

Per client address: an encryption mapping (`rocketnet_encryption_map.h:37-62`), created only after the cookie and the signature, bound by two deadlines, and a client slot assigned only on a connection response (S:2492-2500). Scattered across the request ladder, the response path, the session path, the sweep, the integrity counter and the deliberate disconnect.

### States (per address)

`None` → `HalfOpen` (mapping, `client_index = -1`, `established = 0`) → `Established` (slot assigned, `established = 1`, `confirmed = 0`) → `Confirmed` (`confirmed = 1`) → `None` again. `Confirmed` is a flag on the slot, not a gate for anything but observability (S:2651-2666).

### Transitions

- request, no mapping, passes the ladder (`rocketnet_server_protocol.cpp:78-305,338-357`): fresh nonce and ephemeral (`365-370`), keys derived (`372-387`, small-order refused `383-387`), ephemeral secret erased (`389`); **no free slot → denied, no mapping** (`395-413`); `encryption_map_add` with `handshake_deadline = now + 5 s` (`415-423`, table full → ignore); mapping filled, challenge built once at handshake sequence 1 and stored (`425-461`). `None → HalfOpen`.
- request, mapping exists: `client_index != -1` → ignored as already connected; else the stored challenge resent byte-identical (`91-108`). No state change. (S:2371-2384.)
- request from a connected client id or an admitted token → ignored (`345-357`).
- response opens under the mapping's handshake key (`716-718,743-752`), 24 zero bytes checked (`758-776`), `last_receive` refreshed (`781`): already a slot → ignored (`491-496`); client id already in a slot → ignored (`498-503`); no free slot → denied, mapping kept (`514-519,528-544`); admitted set full → denied (`521-526`); else admitted set takes the token (`521`), slot assigned unconfirmed (`546-550`), `established = 1`, acks and reassembly reset for the slot (`554-558`), first keep-alive built (`567-576`). `HalfOpen → Established`.
- session packet for a mapping (types 4,5,6,8,9): floor (`695-700`), mapping (`702-708`), sequence (`720-728`), replay check (`730-735`), open (`743-752`), **replay advance before the slot check** (`791`), no slot → ignored (`796-801`), ack block (`811-817`), size (`824-833`), `last_receive` (`843`), confirm (`845-846`), ack bookkeeping (`852-864`); disconnect releases the slot and removes the mapping (`938-944`). `Established → Confirmed`; disconnect → `None`.
- sweep, every update (`1033-1059`): unestablished and `now >= handshake_deadline` → removed (`1038-1046`); established and `timeout_seconds > 0` and `now >= last_receive + timeout` → slot released, mapping removed (`1049-1058`). → `None`.
- integrity limit on a mapping (2^36 failed opens, either key): slot released if held, mapping removed, nothing sent (`597-617`). → `None`.
- `disconnect_client(i)` (the application calls): burst of 10 sealed disconnects at consecutive session sequences, then release and remove (`1627-1680`). → `None`.
- Slot assignment picks the lowest free index by a linear scan (`rocketnet_client_slots.cpp:148-156`); release unlinks the id chain and zeroes (`226-263`); the map add zero-fills except sequences at 1 and `client_index = -1` (`rocketnet_encryption_map.cpp:139-145`); remove zeroes the keys (`199`).
- Send paths read the slot's mapping through `rocketnet_server_connected_mapping` (`1062-1090`), which counts `SLOT_WITHOUT_MAPPING` for a connected slot whose `mapping_index < 0`.

### Variables

Per mapping (`rocketnet_encryption_map.h:37-62`): keys, salt, nonce, both ephemeral publics, the stored challenge, `client_id`, `user_data`, `token_identifier`, `token_expire_timestamp`, `timeout_seconds`, `established`, `client_index`, `handshake_deadline`, `last_receive_time`, `last_send_time`, `last_keep_alive_send_time`, `send_sequence_handshake`, `send_sequence_session`, `send_fragment_payload_id`, `replay_handshake`, `replay_session`, `integrity_failures`. The map: address → index, `live[]`, free list, `num_mappings`. Slots (`rocketnet_client_slots.cpp:13-32`): `connected[]`, `client_id[]`, `mapping_index[]`, `confirmed[]`, `user_data`, `num_connected`, the id chain. Plus the token tables (machine 7) and the per-slot ack endpoint (machine 4/6).

### Rules it must keep

Safety:

- **A half-open handshake occupies a mapping, never a slot** (S:2497-2500): `client_index = -1` until a response is accepted.
- **Slot ↔ mapping bijection**: every connected slot's `mapping_index` names a live mapping whose `client_index` is that slot and whose `established = 1`; every established mapping's slot is connected and points back. `SLOT_WITHOUT_MAPPING` stays 0 (`1068-1086`, a comment that names the silent failure).
- **One mapping per address, one slot per client id** (S:1583-1589 table; `rocketnet_encryption_map.cpp:128`, `rocketnet_client_slots.cpp:144`).
- **A mapping is created once, answered identically, replaced never** (S:2373-2384): the stored challenge bytes and the nonce never change while the mapping lives.
- **The handshake deadline is fixed at creation and never refreshed** (S:2508-2517); traffic refreshes only `last_receive_time`.
- **The handshake deadline is not the token's timeout** (S:2519-2527): a half-open mapping dies by `handshake_deadline` alone; an established one by `timeout_seconds` alone (S:2542-2543).
- **A denied never creates a mapping at the request stage, and never destroys one at the response stage** (S:2459-2464, S:2603-2608).
- **The admitted-token set takes its entry only on the response, after the slot check** (S:2603-2613; `514-526`).
- **No free slot ⇒ denied, never a dropped request** (S:2478-2490 "Every valid request is answered every time").
- **Release zeroes keys and sequence state** (S:2926-2931; `rocketnet_encryption_map.cpp:199`).
- Counters: `CLIENT_CONNECTED = connected + CLIENT_DISCONNECTED + CLIENT_TIMED_OUT` (the comment at `584-595`); `MAPPING_CREATED = live + HANDSHAKE_DEADLINE_EXPIRED + IDLE_TIMEOUT + removed-by-disconnect + removed-by-integrity`.
- The mapping table is sized above the slot count (S:2545-2549; `rocketnet_server_transport.cpp:798`: `2 × max_clients`).

Liveness:

- Every half-open mapping is eventually removed or established (the handshake deadline, S:2512-2517).
- Every established mapping whose peer goes silent is eventually released (S:2914-2917).
- A request from an address whose mapping just expired is answered with a new challenge (fresh nonce) on its next arrival.

### Verdict: model.

What it would catch: a path that releases a slot without its mapping or the reverse; a deadline refreshed by the wrong packet; a denied that leaks a mapping or a token admission; a disconnect, timeout or integrity teardown that leaves the other half; the ordering of the slot check against the admitted set. This is the server half of module 3 and the home of the integrity-limit event.

---

## 3. The handshake: both ends over a lossy wire (the headline)

Machines 1 and 2 composed with the wire (a packet arrives, is lost, is duplicated, is reordered, is replayed from another address) and the security checks of the request ladder in order (S:2368-2439). This is the module the card was asked for first: "the state machines for client and server connection and security".

### What is abstracted

Crypto is modelled as predicates on tags: a packet carries `(type, from, nonce, keyset, sequence)` and `opens` iff the receiver holds the same keyset; a cookie is `(address, salt, expire)` and verifies iff it was issued by this server for that address and salt and is unexpired; a token is `(id, client_id, expire)` and verifies iff it is in the backend's issued set; the session nonce is fresh by construction (a counter); the ephemeral exchange is `{client_eph, server_eph}` with a small-order flag.

### Variables

Client (machine 1), server (machine 2 for each address, plus the token tables of machine 7 abstracted to sets), the cookie key, and the wire: a bag of in-flight packets with a bound.

### Actions

Client: `Connect`, `ClientUpdate` (time passes; sends per the rate), `ClientReceive(p)` for each type, `ClientDisconnect`. Server: `ServerReceive(p)` (the full ladder: size, already connected, resend challenge, version, protocol, expire, timeout, flags, advertised, server key, cookie, used-token, signature, seal, consistency, client id, admitted, record, derive, no slots, add mapping, challenge), `ServerUpdate` (the sweep, keep-alives), `ServerDisconnect(i)`. Wire: `Lose(p)`, `Duplicate(p)`, `Deliver(p)` in any order, `Replay(p, from')` (an attacker re-sends a captured request from another address), `Inject(p)` (an off-path attacker sends any packet with keys it does not hold), `Tick`.

### Rules it must keep

Safety:

- **No asymmetric work for a spoofed source**: the signature step is reached only after a cookie that verifies for this `from` (S:2323-2327, S:2397-2411). In the model: `SignatureChecked(p) ⇒ CookieValid(p.from, p.salt)`.
- **No slot for a client that is not there**: a slot is assigned only on a response that opened under keys derived from a challenge sent to that address (S:2309, S:2496-2500, S:3216).
- **Every valid request is answered every time** (S:2478-2490): any accepted request produces exactly one of cookie / challenge / denied; silence only for an invalid one.
- **No response larger than what provoked it** (S:3272-3294; the sizes are constants: 710→38, 710→66, 710→34, 66→50, 50→41 or 34, 38→nothing).
- **A key or nonce never outlives one connection** (S:1005-1026, S:3220): two live mappings never share a nonce; a mapping that was removed is never recreated with the same nonce; a client's new attempt has a new salt.
- **A token admits one connection** (S:2432-2435, S:2573-2586, S:3376-3383): at most one established mapping ever carries a given token id while it is unexpired; a replay of a captured request from another address is refused by the used-token history (S:2412-2423, S:3260-3270).
- **A client id is connected at most once on this server** (S:974-978).
- **Phase separation**: a type 8/9 under handshake keys never opens; a response under session keys never opens (S:1295-1297).
- **An injected or replayed packet changes no state except its counter and (for session types that opened) its replay mark** (S:1486-1492).
- **The stranding invariant**: no reachable state has an end waiting for a packet that will never be sent again (S:2127-2130, S:2255-2258). As an invariant on the model: for every client in `1..3` whose server-side counterpart exists, the server's next `ServerReceive` of the client's retransmitted packet produces the answer the client is waiting for; and for a client in `3` whose slot exists, a keep-alive is eventually sent (module 4).

Liveness (weak fairness on both updates and on `Deliver` for some copy of every packet that is retransmitted; strong fairness not needed because every wait state retransmits):

- A client with a valid token and a server with a free slot eventually reach `client.state = 4 ∧ slot confirmed` (S:2224-2258).
- A client whose token the server refuses eventually reaches `-3` (the server issued a cookie and then went silent; S:2061-2070).
- A client against a full server eventually reaches `-1`.
- A half-open mapping whose client stopped eventually goes to `None`.

### Small instance

One server, two client addresses `{a, b}`, one slot, mapping capacity 2, token set of three ids, time as a counter with `send interval = 1`, `handshake deadline = 3`, `timeout = 4`, `cookie lifetime = 2`, `token lifetime = 6`, wire bound 3, and one attacker address `x` that can replay any captured request and inject any packet without keys. Expect on the order of 10^5 to 10^6 states; the replay bitmap is abstracted to "highest opened" plus a set of the last 3.

### Verdict: model; sixth in the list, after its parts (modules 1 to 5 settle the abstractions it composes).

What it would catch: an ordering change in the ladder that lets a spoofed source reach the signature; a response path that assigns a slot before the admitted set or the slot check; a replayed request that creates a second mapping; a nonce reused after a removal; a lost packet that strands either end; an injected packet that moves state; a denied that is not re-provoked.

---

## 4. Keep-alive generation and the acknowledgment latch (#90)

The subtle one (S:2679-2888, the longest argument in the standard). Per connected slot, which clock paces the keep-alive turns on whether the client has acknowledged a keep-alive, and the latch is set from the client's own unverifiable claim.

### States (per slot)

`Unacknowledged` (`keep_alive_acked = 0`: keep-alives paced off `last_keep_alive_send_time`, never suppressed) → `Acknowledged` (`keep_alive_acked = 1`: paced off `last_send_time`, suppressed by any send). One-way until the slot is reset (`rocketnet_ack.cpp:220-226`).

### Transitions

- keep-alive built for slot `i` (`rocketnet_server_protocol.cpp:1161-1211`): `write_session_packet` spends a session sequence and stamps `last_send_time` (`1124-1157`); then `last_keep_alive_send_time = now` (`1198`); then the record is tagged `was_keep_alive` (`1206` → `rocketnet_ack.cpp:267-278`; a recycled record is silently not tagged).
- due check each update (`1240-1319`): `acknowledged` read from the endpoint (`1309-1311`); `last = acknowledged ? last_send_time : last_keep_alive_send_time` (`1313`); send iff `now - last >= 1/send_rate` (`1315`).
- any other send to the slot (payload, fragment, standalone ack, disconnect burst) stamps `last_send_time` only (`1157,1402,1577`).
- a session packet from the client opens and its block is applied (`852-864`): `process_block` marks records acked; a newly-acked record with `was_keep_alive` sets `keep_alive_acked = 1` (`rocketnet_ack.cpp:384-385`). `Unacknowledged → Acknowledged`.
- standalone ack sent when `ack_debt >= 64` (`1213-1238`), only when no keep-alive was due this update (`rocketnet_server_transport.cpp:1164-1166`).
- the maintenance loop reserves a send frame per connected slot and stops at the first refusal, resuming next update from `maintenance_next_client` (`rocketnet_server_transport.cpp:1151-1172`).
- slot reset on assignment clears the latch (`554-555` → `rocketnet_ack.cpp:685-690`).
- The client side: a keep-alive moves the client `3 → 4` (machine 1); the client's own keep-alives from `4` are suppressed by any of its sends without a split (`rocketnet_client_protocol.cpp:297`, header comment `157-161`).

### Variables

Per slot: `keep_alive_acked`, `last_send_time`, `last_keep_alive_send_time`, the sent-record ring's `was_keep_alive` bits, `ack_debt`; the client's state (3 or 4) and its receive bitmap (what it will truthfully claim).

### Rules it must keep

Safety:

- **Until acknowledged, ordinary traffic MUST NOT suppress the keep-alive** (S:2682-2686): in `Unacknowledged`, `now - last_keep_alive_send_time` never exceeds `interval + one update tick` while the slot is connected and a frame is available. (The discrete-check band, S:2782-2799, is the `+ tick`.)
- **Once acknowledged, a keep-alive is emitted only when nothing at all was sent for an interval** (S:2687-2691, S:2772-2780): in `Acknowledged`, a keep-alive at time `t` implies `t - last_send_time >= interval`.
- **A conformant client never trips the latch before it holds its index** (S:2722-2728): with a truthful client (its blocks name only sequences it opened), `keep_alive_acked = 1 ⇒ client.state = 4`. This is the invariant that the confirmed flag broke and this test keeps.
- **The flag the latch does not read**: `confirmed` is set by any sealed packet (`845-846`) and is never consulted by pacing (S:2659-2666).
- **An authentically opened keep-alive always reaches its transition** (S:2821-2846): for a keep-alive the server built, the client's ack-block validity and opened-size checks pass; so the client's claim about a keep-alive is equivalent to its having acted on it. In the model this is the assumption that lets the previous invariant be stated; the model should also check it against the client opener (machine 1, `719-735`) with the server's builder (8 bytes of type data, a block the server built).
- **An over-claiming client strands only itself** (S:2748-2751): the latch of slot `i` is set only by packets that opened under slot `i`'s keys.
- **The standalone ack also stamps the suppression clock** (S:2813-2819).

Liveness:

- With a truthful client in `3` that keeps retransmitting its response, and fair delivery, the server sends keep-alives unboundedly often until one is acknowledged, and the client eventually reaches `4` (S:2848-2865 "another one keeps coming, and that no traffic pattern can stop it").
- The bound on the window is the peer's behaviour, not a server deadline (S:2867-2879): a client that never acknowledges keeps receiving keep-alives until its own total deadline.

### Small instance

One slot, `interval = 2` ticks, update tick 1, a server application that may send a payload every tick (the dense stream of S:2737-2741), a client in `3` that is either truthful or over-claiming (a constant), loss on the server→client direction as a nondeterministic choice bounded to 3 consecutive, the sent-record ring of size 4 (so recycling of a keep-alive's record before its ack is reachable).

### Verdict: model.

What it would catch: the #90 regression itself (reading `last_send_time` before the latch) and its mirror (reading the keep-alive clock after it, which would make an idle connected client's keep-alives never suppressible, a cost not a strand); a maintenance-loop change that skips a slot forever under send-pool pressure (the `maintenance_next_client` resume); the latch set by a non-keep-alive record; the latch not cleared on slot reuse.

---

## 5. Replay window

`rocketnet_replay.cpp`: a high-water mark and a bitmap of `ROCKETNET_HISTORY_PACKETS` (4096 by default, up to 32768) bits indexed by `sequence mod window` (`9-25`). Per key phase per direction (S:712-716): `replay_handshake` and `replay_session` on both ends. The ack block builder reads the same bitmap (machine 6; `rocketnet_replay.h:22-24` "the one home").

### States

`highest_opened` (0 = nothing opened), `window[]`. A sequence is `Above` (`> highest`), `InWindow` (within `window` of highest, marked or not), or `Below` (older than the window).

### Transitions

- `already_received(s)` (`27-40`): `s > highest` → 0; `highest >= W ∧ s <= highest - W` → 1 (too old, refused); else the bit.
- `advance(s)` after a successful open (`42-70`): if `s > highest`: `skipped = s - highest`; `skipped >= W` → **zero `window[0..3]`** (`54-60`); else clear bits `highest+1 .. s-1` (`63-64`); `highest = s`. Then set bit `s` (`69`). Precondition asserted: not already received (`46`).
- Callers: client opener checks then advances (`rocketnet_client_protocol.cpp:687,711`); server checks then advances for session types (`rocketnet_server_protocol.cpp:730,791`); the handshake phase on the server only raises `highest_opened` for the response (`778-779`); the client raises it for the challenge (`511-512`) and not for a denied.

### Variables

`highest_opened`, `window[W/64]`.

### Rules it must keep

- **Within the window a sequence is accepted exactly once; below it, refused; above the mark, always accepted** (S:1522-1524; `rocketnet_replay.h:15-17`).
- **An undersized window discards old legitimate packets; it never accepts a replay** (S:1562): `already_received(s) = 0 ⇒ s was never advanced` (soundness).
- **A sequence far ahead does not inherit a stale bit** (`rocketnet_replay.h:35-37` and `rocketnet_replay.cpp:50-51`, the comments on `advance`): after `advance(s)`, for every `t` in `(highest_old, s)`, the bit of `t` is clear. Completeness: `already_received(t) = 1 ⇒ t was advanced ∨ t <= highest - W`.
- **The bitmap is the ack block's truth** (S:1797-1799, machine 6): every bit set inside the window was opened.
- Marked only after the open, never on receipt (`rocketnet_replay.h:18-20`).

### Defect found by reading (verified by hand against the code; no code change on this card)

`rocketnet_replay_advance` zeroes `window[0]..window[3]` when the jump is at least the window (`rocketnet_replay.cpp:54-60`), 256 bits, the floor the window had when the file was written (`6cfd014`, 2026-08-27: `uint64_t window[4]`). On 2026-09-07 `ea9b2c0` widened the struct to `window[ROCKETNET_HISTORY_PACKETS / 64]`, 64 words at the default, and did not touch `rocketnet_replay.cpp` (its last change is `9465b99`, 2026-08-28). The `static_assert` in `rocketnet_replay.h:28-30` checks the macro against the struct, not the clear against the struct.

Trace, replayed through the functions by hand at `W = 4096`:

1. Fresh window. `advance(300)`: `skipped = 300 >= 4096`? no; clears bits 1..299; `highest = 300`; sets bit 300, which is `window[300/64] = window[4]`, bit 44.
2. The sender burns or the link loses 4096 or more sequences (U:139-157: one failed fragmented finish burns up to 14; U:283-291: history overflow is an operating condition; S:1566 "Finite history cannot promise ..."). The next opened packet is `s = 4396` (= 300 + 4096). `advance(4396)`: `skipped = 4096 >= 4096` → zero `window[0..3]` only; `highest = 4396`; set bit `4396 mod 4096 = 300` → `window[4]` bit 44, already set from step 1 (harmless here, same bit).
3. Now the real inheritance: in step 1 instead advance `300` and also `301`; in step 2 open `4396` as above. Then packet `4397` arrives, never seen: `already_received(4397)`: `4397 > 4396`? no. `4396 >= 4096 ∧ 4397 <= 300`? no. Return bit `4397 mod 4096 = 301` → `window[4]` bit 45, still set from the old lap. **A genuine, never-before-opened packet is refused as ALREADY_RECEIVED.** Every later sequence whose ring bit lies in words 4..63 and was set on the old lap is refused on first arrival, until the bit-by-bit clears of later advances reach it.
4. The second consequence, through machine 6: `rocketnet_ack_block_build` walks the bitmap down from `highest = 4396` (`rocketnet_ack.cpp:81-131`) and reads bit 301 as received; the block names sequence 4397, which this end never opened. That violates S:1734-1744 ("A block MUST NOT name a sequence its sender has not opened"), and if 4397 was the server's keep-alive the client's block latches `keep_alive_acked` (machine 4) for a packet the client never acted on: the exact stranding S:2821-2846 says the design depends on never being reachable.

The existing test (`rocketnet_tests.cpp:2790-2797`) advances 1 then `W+1`: bit 1 lives in `window[0]`, which is cleared, so the test passes over the defect. A TLC model with `W = 8` bits as 4 words of 2 bits and a clear that zeroes only the first two words reproduces step 3 in a handful of states; the model card should check the model of the *code* first (to see the counterexample), then the model of the *rule* (clear everything skipped).

Reachability in practice: a jump of `W` or more between two opened packets on one connection. Not an attack vector (it refuses, it does not accept), but a conformant peer under burn or burst loss is refused, and the ack block then lies. The fix is one line (`memset( replay->window, 0, sizeof(replay->window) )`); it belongs to a code card, not this one.

### Verdict: model, first in the list. Small, and it already has a counterexample to confirm.

---

## 6. Ack block: build, validate, apply; the sent-record ring and aggregates

`rocketnet_ack.cpp`. The receiver's bitmap becomes a block (`51-135`), the block is validated on the reconstructed 64-bit largest (`145-214`), and applied to the sender's ring of sent records (`318-467`), surfacing packet ids once, with fragments aggregated.

### States

Per sent record (`rocketnet_ack.h:81-93`): `Empty` (`sequence = 0`) → `Sent` (`acked = 0`) → `Acked`; recycled when a sequence `W` later takes the slot. Per aggregate (`104-110`): `Open(remaining = n)` → counts down → `Surfaced` when 0; lapped when the id is overwritten. The latch `keep_alive_acked` (machine 4). `highest_sequence`, `highest_received_sequence`, `ack_debt`.

### Transitions

- `record_sent(seq, ...)` (`228-265`): refused (returns false, counted `SEND_HISTORY_OVERFLOW`) if the slot holds a newer or equal sequence or `seq` is below the retained window; else the slot is overwritten (an unacked older record is evicted: the "retained without loss" return) and `highest_sequence` folds by atomic max. Under the spin lock.
- `mark_keep_alive(seq)` (`267-278`): tags only if the slot still holds `seq`.
- `open_aggregate(packet_id, n)` (`280-298`): next id, slot `id mod A`, `remaining = n`.
- `record_received(seq, bytes, t)` (`300-313`): the received ring and `highest_received_sequence`.
- `block_build(capacity, replay)` (`51-135`): from `highest_opened` downward: first run, then (gap, run) pairs, newest first, stopping at the enumerable floor (`highest - W + 1`), at `max_ranges` (capacity), or at a gap that reaches the floor (dropped: "unspoken, not lost"). The 7-byte minimum always fits.
- `block_read(data, bytes, highest_sent)` (`145-214`): geometry in the opened size; `largest` reconstructed near `highest_sent`; `largest = 0 ⇒ first = 0 ∧ ranges = 0`; `first >= 1`; `first <= largest`; no zero gap or length; `first + Σ(gap+len) <= largest`.
- `process_block(block)` (`427-467`) → `apply_run(low, high)` per run (`318-425`): named sequences above `highest_sequence` are counted `ACK_UNKNOWN_SEQUENCE` arithmetically and not visited; below the retained window skipped; within, a slot not holding `seq` counts unknown; an unacked record with a packet id or aggregate is deferred (counted `ACKED_BATCH_FULL`, left unacked) when the batch is full; else marked acked, rtt sampled once, the latch set if `was_keep_alive`, the packet id appended, or the aggregate decremented and its packet id appended at zero.
- Senders: every session packet built by `write_session_packet` / `seal_payload_frame` / `seal_fragment` records and zeroes `ack_debt` (`rocketnet_client_protocol.cpp:265-269,1099-1103,1237-1241`; server `1150-1154,1395-1399,1570-1574`); every opened session packet of a type other than 8 adds to `ack_debt` (`rocketnet_client_protocol.cpp:763-764`; server `862-863`).
- The fragmented send (machine 10): `start_packet` spends one sequence (U:126-130); `start_fragments(n-1)` reserves a block (U:139-147); every frame is reserved before any seal; a seal failure withdraws all reserved frames on portable and GSO and only the remainder on XDP (`rocketnet_server_transport.cpp:1584-1597`, U:149-157); the aggregate is opened before the seals (`1571-1572`).

### Variables

`highest_sequence`, `highest_received_sequence`, `ack_debt`, `keep_alive_acked`, `num_aggregates`, `sent[W]`, `received[2048]`, `aggregates[W/2]`, the per-update batch (`acked_packet_ids`, `num_acked`), and the counters.

### Rules it must keep

- **A block names only sequences its sender opened** (S:1734-1744): with the bitmap sound (machine 5), every sequence a built block describes as received has its bit set and is within the window.
- **A built block is valid under `block_read` at the peer** (S:1754-1778): ranges strictly descending, non-empty, non-touching, confined to `[1, largest]`; `first = 0 ⇔ largest = 0`.
- **Validity is evaluated on the reconstructed 64-bit largest** (S:1774-1778).
- **The walk is bounded by the retained window, never by the block's claims** (S:1836-1846): `apply_run` visits at most `W` records per block.
- **A sequence this end never sent is ignored and counted, never an error** (S:1842-1846).
- **A packet id surfaces exactly once, and only after its packet (or every fragment of its payload) was acknowledged**; a lapped aggregate never attributes an ack to a newer payload (`rocketnet_ack.h:94-100`); a deferred record is surfaced by a later block, never dropped (`rocketnet_ack.h:207-210`; the acked-batch comment above `rocketnet_client_transport_acked_packets` in `rocketnet.h`).
- **Keep-alive, disconnect and standalone-ack sequences carry no packet id and never surface** (U:277-281).
- **Elicitation**: type 8 never adds to `ack_debt`; sending any block zeroes it (S:1796-1810).
- **Overflow preserves transmission** (U:283-291, S:1566-1570): `record_sent` returning false never prevents the send.
- **The rtt sample is taken from the first block that acknowledges a sequence** (S:1853-1858).

Liveness: every sent record that the peer opened is eventually acked, given that the peer keeps sending blocks and the record is not recycled first (the window bound, U:289-291).

### Small instance

`W = 8` (sent ring), received ring 4, aggregate ring 4, `max_ranges = 2`, capacity for 2 ranges, a peer bitmap of 8, sequences up to 24 so recycling and the floor are reached, batch capacity 2 so deferral is reached, wire sequences modelled as 3-bit with reconstruction near `highest_sent` (so a "wrap" is reachable at 8). Loss and reordering of session packets as wire actions.

### Verdict: model, second in the list (machines 4 and 3 build on its abstractions).

What it would catch: a block that over-claims after a window advance (the machine 5 defect surfaces here as an invariant violation); a truncation that drops the newest instead of the oldest range; a validity rule weakened so a block reaches the walk out of bounds; a packet id surfaced twice (deferral then re-ack) or never (eviction before ack with no overflow count); a fragment aggregate attributing a lapped slot; the latch set by an untagged record.

---

## 7. Used-token history and admitted-token set

`rocketnet_token_tables.cpp`: one table shape (`24-40`), two policies.

### States (per entry)

`Free` → `Bound(id, address?, expire, order)` → `Expired` (same entry, `expire <= now`, treated as absent by `find`, `130`) → `Free` or overwritten. The table: free list, chains, `next_insert_order`.

### Transitions

- `find(id, now)` (`122-133`): chain walk; a hit that is expired returns absent.
- `select_slot(now, evict_oldest)` (`160-188`): free list first; else the first expired entry (unlinked); else the oldest by insert order if `evict_oldest`, else `-1`.
- history `check(id, from, now)` (`221-231`): unseen or expired → 1; bound to `from` → 1; bound elsewhere → 0.
- history `record(id, from, expire, now)` (`233-248`): no-op if found unexpired; else `select_slot(evict_oldest = 1)` and insert. Called on every request past the seal open (`rocketnet_server_protocol.cpp:359`), before the mapping exists.
- set `contains(id, now)` (`262-268`); set `add(id, expire, now)` (`270-290`): found → 1 (idempotent); else `select_slot(evict_oldest = 0)`; `-1` → 0 (full of unexpired: the caller denies, `ADMITTED_SET_FULL`). Called only on the response (`rocketnet_server_protocol.cpp:521`).
- Sizes: history `8 × max_clients`, set `2 × max_clients` (`rocketnet_server_transport.cpp:799-800`; S:2566-2567, S:2581-2582).

### Variables

`identifier[]`, `address[]`, `expire_timestamp[]`, `insert_order[]`, `bucket_head[]`, `next[]`, `free_head`, `next_insert_order`.

### Rules it must keep

- **Every live entry is findable from its own bucket** (the header comment `19-22`): the chain of `bucket(id)` contains the index iff the entry is live.
- **The history binds an identifier to the first address that presented it; a different address is refused while the entry lives** (S:2563-2571, S:3219).
- **An unexpired entry of the admitted set is NEVER evicted** (S:2576; `283-285`): `add` never unlinks an entry with `expire > now`.
- **A token admits one connection** (S:2573-2576): once `add(id)` succeeded, `contains(id)` is true until expiry; so a second request with the same token is refused at `rocketnet_server_protocol.cpp:352-357`.
- **The set fails closed** (S:2583-2586): full of unexpired ⇒ `add` fails ⇒ denied, never an eviction.
- **Expiry is the only exit from the set** (S:2574-2578).
- **The history's eviction is oldest-first among unexpired** (S:2566-2567) and expired-first (`174-178`).
- **The free list and the chains partition the slots**; `num live + free = capacity`.

### Small instance

Capacity 2 (so 4 buckets), identifiers `{1,2,3}`, addresses `{a,b}`, time `0..5`, expiries from `{2,4}`. Both policies as one module with a constant `EvictOldest`.

### Verdict: model, third. Cheap, and it carries the "a token admits one connection" guarantee that the rest of the security argument rests on (S:3376-3383).

What it would catch: an eviction of an unexpired admitted entry; a `find` that misses an entry after a relink; a record that refreshes a binding to a second address; an expired entry that still refuses.

---

## 8. Cookie gate and cookie key rotation

The gate (`rocketnet_server_protocol.cpp:213-275`): zero field → `NO_COOKIE`; MAC under the current key, else under the previous key when `has_previous_cookie_key` (`223-237`); mismatch → `COOKIE_DID_NOT_VERIFY`; `expire <= now` → `COOKIE_EXPIRED`; any of the three → a fresh cookie issued at `now + lifetime` under the current key and stop (`260-274`). The order is deliberate: zero, MAC, expiry (`219-221`). The client stores the last 24 bytes and echoes them (`rocketnet_client_protocol.cpp:402-404`).

### Rules

- **Address validation before any asymmetric work** (S:2323-2327): the gate sits after the fixed-offset checks and before the used-token hash and the signature (S:2403-2411).
- **A request that fails the gate is answered with a cookie, every time** (S:2482-2484).
- **A cookie verifies only for the address and salt it was issued to, within its lifetime** (S:1234-1255).
- **Rotation**: the key rotates every 60 s and the previous key is accepted for one period (S:1176-1179, S:1995 "Cookie key rotation | 60 s | server: previous key accepted for one period"; C:180-181 "cookie key rotation runs always at its 60 s period so the previous-key acceptance window is exercised every minute of every session").
- The race with the send rate (S:2018-2023): a cookie that expires before the next scheduled request costs one round trip and nothing more.

### Gap found by reading

**Cookie key rotation is not implemented.** `cookie_key` is generated once at create (`rocketnet_server_transport.cpp:792`); `previous_cookie_key` and `has_previous_cookie_key` exist in the struct (`rocketnet_server_protocol.h:49-50`) and are consulted by the gate (`231-236`), but nothing in `sdk/` ever writes them; only the test does (`rocketnet_tests.cpp:2728-2730`). No timer, no rotation action, no 60 s period. The standard's `Timing` table and the working notes both describe rotation as running; the code runs one key for the process's life. The machine to model (`Current(key), Previous(key?), rotation clock`) has today only its initial state.

### Verdict

Model the gate as part of module 3 (it is the first rung that matters). Model rotation when it exists: the module should state the invariant "a cookie issued at `t` under key `k` verifies at any `t' < t + lifetime` provided `t' < rotation(k) + period`", and the liveness "a client that received a cookie and retransmits within the lifetime passes the gate". The defect line for the report: rotation absent; `previous_cookie_key` dead in production.

---

## 9. Fragment reassembly slot

`rocketnet_fragment.cpp:199-460`, one slot per `(connection, payload_id mod num_slots)`.

### States (per slot)

`Empty` (`payload_id = 0`) → `Collecting` with sub-state `{stride unknown, final staged?}` × `{stride known, final placed?}` → `Complete` (delivered and cleared in the same call) or `Abandoned` (evicted by a newer id) or `Refused-and-cleared` (too large, or a staged final over the stride).

### Transitions (one fragment arrives, `type_data = header + bytes`)

In order, each refusal before any mutation (`225-228`): zero id (`232-237`); stale: `slot.id != 0 ∧ id - slot.id > 2^31` in unsigned arithmetic (`253-258`); count 1 (`263-268`); index ≥ count (`270-275`); same payload with a different count (`285-290`); non-final size ≠ stride, or final > stride, once the stride is known (`300-314`); a staged final larger than the stride the first non-final fixes → refuse and **clear the slot** (`319-325`); reconstructed size over `max_payload_bytes` once stride and count are known (final counted as its real size when known, else 1) → refuse and clear if same payload (`337-353`); duplicate index (`359-364`). Then: a different (newer) id evicts the occupant (`abandoned` if non-empty) and takes the slot (`371-382`); a non-final fixes the stride if unknown and places a staged final (`384-400`); a final places or stages (`401-418`); the bit is set (`420`); when `num_received = count`, the payload is copied into the delivered mirror (or `batch_full`), counted `reassembled`, and the slot cleared (`424-457`).

`reset_connection` zeroes a connection's slots on slot assignment (server `557-558`) and on client connect (`rocketnet_client_protocol.cpp:128-129`); `begin_update` resets the delivered window each update (`173-179`).

### Variables

Per slot: `payload_id`, `fragment_count`, `fragment_stride`, `final_bytes`, `final_staged`, `num_received`, `received[256/8]`, the arena bytes and the staging buffer. Per set: `num_delivered`, `delivered_capacity`, `completed_payload`, `completed_bytes`.

### Rules it must keep

- **Bytes, once, or nothing** (S:1867-1873, `rocketnet_fragment.h:21-22`): a delivered payload equals the sender's payload exactly; a payload is delivered at most once; a lost fragment loses the payload (counted `abandoned` when evicted).
- **The first fragment pins the count; a later fragment with a different count refuses and keeps the slot** (S:1920-1924).
- **Every non-final fragment is the stride; the first non-final fixes it; the final never exceeds it; only the final may arrive before the stride is known and it waits in staging** (S:1925-1935).
- **Stale ids refuse under serial-number arithmetic; a later id always displaces** (S:1906-1916): no id can poison a slot.
- **The too-large check runs the moment stride and count are known, never deferred** (S:1936-1946).
- **A refusal mutates nothing** except the two "clear the slot" cases, which are stated (S:1944-1946, `319-325`).
- **The slot clears on completion** (S:1956-1959), so a late duplicate starts a ghost that is abandoned (accepted behaviour, S:1959-1963).
- **Storage is fixed at create and never grown** (S:1969-1980): the arena index never exceeds `num_connections × num_slots`.
- **`BATCH_FULL` stays zero at the stated delivered capacity** (`rocketnet_fragment.h:108-115`: a payload takes at least two fragments, so at most half the drained packets complete). The transports size it so (`rocketnet_client_transport.cpp:222-229`, `rocketnet_server_transport.cpp:803-811`).
- Sender side (`rocketnet_fragment_plan`, `40-70`): count ≥ 2, the last fragment between 1 and stride, count ≤ 256.

Liveness: if every fragment of a payload is delivered (in any order) before a newer payload claims the slot, the payload is delivered.

### Small instance

2 slots, ids drawn from `1..6` with the stale comparison modelled at modulus 8 (so wrap and the 2^31 half are reachable), counts `{2,3}`, stride `{1,2}`, `max_payload = 5`, a wire that delivers a payload's fragments in any order, loses any, and may interleave two payloads. The fragment bytes are abstract tokens `(id, index)`; the delivered payload is the sequence of tokens.

### Verdict: model.

What it would catch: aliasing of bytes across payload ids on a slot; a staged final placed at the wrong offset or placed twice; a count or stride re-pinned after the fact; a too-large payload occupying the arena before refusal; delivery with a fragment missing; a refusal that mutates.

---

## 10. Fragmented send (sequence block, arena, all-or-nothing frames)

`rocketnet_client_transport.cpp:760-854` and `rocketnet_server_transport.cpp:1499-1604`. One sequence spent at `start_packet` (U:126-130), `fragment_count - 1` more reserved at finish (U:139-147), every wire frame reserved before any seal (`1543-1555`), the aggregate opened (`1571-1572`), each fragment sealed; a seal failure withdraws all frames on portable and GSO (`FINISH_FINAL` unset) and only the remainder on XDP (`1584-1597`; U:149-157). The arena is reset by the flush (`rocketnet_fragment.cpp:521-525`).

Rules: sequences are monotone and a fragmented payload occupies `count` consecutive sequences (U:143-147); a withdrawn payload burns its sequences and counts nothing but ring pressure (U:187-204); on XDP the committed prefix is sent and the rest withdrawn (a partial payload the receiver abandons).

### Verdict: fold into module 6 as the sender's actions (`StartPacket`, `StartFragments`, `SealFragment` with a nondeterministic failure, `Withdraw(FinishFinal)`), with the invariant that the aggregate's `remaining` equals the number of fragments actually transmitted. No separate module.

---

## 11. Server transport lifecycle and driver stop

`rocketnet_server_transport.cpp`: `RUNNING` set at create (`774`); `stop()` → `STOPPING` (`866-870`); `update()` in `STOPPING` polls `driver.stop` and moves to `STOPPED` when it returns true, returning early either way (`945-951`); `start_packet` refuses unless `RUNNING` (`1255-1256`). The driver contract: "Main-thread stop/poll is idempotent. False requires another poll" (`rocketnet_driver.h:45-48`).

Drivers:

- portable, msgx, GSO: `synchronous_stop` returns true (`rocketnet_driver_builtin.cpp:17,28,72`); GSO workers are stopped at destroy by `quit` + join (`rocketnet_server_gso_linux.cpp:730-745`).
- XDP: `xdp_stop` calls `socket_stop` (→ `STOPPING`, `rocketnet_server_socket_linux.cpp:1682-1686`) then `socket_update` (→ `STOPPED`, `1672-1680`) and reports `state == STOPPED` (`rocketnet_driver_builtin.cpp:85-94`); the XDP threads keep running until destroy sets `quit` and joins (`1637-1642`); destroy asserts `STOPPED` (`1592`). `xdp_destroy` stops first (`95-99`).
- IOCP: `stop` sets `stopping` once, takes and releases every worker mutex (so no repost slips past), cancels I/O, posts a wakeup per worker (`rocketnet_server_iocp_windows.cpp:237-258`); `stopped` ⇔ `stopping ∧ pending = 0 ∧ active_workers = 0` (`260-265`); workers exit when `stopping ∧ pending = 0` (`166-167`), decrement `active_workers` (`234`); `post_receive` refuses when stopping (`142-143`), `pending` is incremented before the post and decremented on failure or completion (`149-154,198,218`); `send_packets` frees staged ops instead of posting when stopping (`446-450`); `start_packet` refuses when stopping (`396`). Destroy stops, joins, asserts `pending = 0` (`267-283`).
- Client transport: `quit` + join at destroy (`rocketnet_client_transport.cpp:377-382`); the thread loops on `quit` (`1144-1149`).

### Rules

- **No send after stop**: `start_packet` returns NULL once not `RUNNING`; staged ops are not posted after `stopping` (U:46-50 "`stop` ... main-thread ... never while a fill is open").
- **Stop completes** (liveness): with workers scheduled fairly, `STOPPING` eventually becomes `STOPPED`. For IOCP this is the interesting one: every `pending` increment is matched by a completion or a failure path, and no repost occurs after `stopping` (the mutex hand-off in `stop` is what makes the race unreachable).
- **Destroy only from STOPPED** (XDP assert `1592`; IOCP assert `283`).
- **Idempotent stop/poll** (`rocketnet_driver.h:45-46`).

### Small instance

Transport state × driver `{XDP-like synchronous two-step, IOCP-like counting}`; 2 workers, 2 receive ops, 1 send op; actions `Stop`, `Update`, `WorkerLoop`, `Complete(op)`, `PostFails`, `Destroy`. Check `stopped ⇒ pending = 0`, no post after stopping, and `<>STOPPED`.

### Verdict: model, one small module (`DriverStop`). The IOCP counting is the kind of thing TLC is for.

---

## 12. XDP send handoff generations

`rocketnet_send_handoff.h` (two buffers: `free[]`, `queued[]` ring, `filling`) driven by `rocketnet_server_socket_linux.cpp`: producers fill the generation named by `send_flow_index` (`1762-1795`, slow path acquires under the mutex when `-1`, `1766-1776`); `send_packets` publishes the filling generation if non-empty and acquires the next (`1883-1898`); the worker copies the front generation into umem frames batch by batch and retires it only when `packet_start_index == num_packets` (`2251-2325`, "UMEM now owns every copied packet"); `finish`/`abort` resolve their slot against the *current* filling buffer and fail closed on an out-of-range pointer (`1821-1836,1853-1863`); frame pressure leaves `packet_start_index` untouched for retry (`2287-2298`).

### Rules

- A buffer is never both filling and queued; `acquire` returns the one already filling if any (`22-28`); `publish` requires a filling buffer and room (`29-35`); `retire` requires a queued one (`37-43`).
- **A finished frame on XDP cannot be withdrawn** (`ROCKETNET_DRIVER_FINISH_FINAL`, `rocketnet_driver.h:26-27`): after `publish`, producers never touch that generation again; the worker drains it to completion across flushes.
- `start_packet` refuses when both generations are queued (the test `1903-1968` pins exactly this: "Both generations belong to the worker").
- Every packet with `packet_bytes > 0` in a published generation is copied exactly once before the generation is reused.
- The `finish` after a flip: a pointer into the retired generation resolves out of range and is dropped, never written (`1830-1836`).

### Small instance

2 generations, queue size 3, batch size 2, a frame pool of 3 so pressure is reachable; actions `Start`, `Finish`, `Abort`, `Flush`, `WorkerBatch`, `Completion`. Invariant: the set of packets sent equals the set finished in published generations; no packet sent twice; no write to a queued generation.

### Verdict: model. It is the dataplane's one lock-free handoff with a known untested edge (C:104-110, the pending-return spill is machine 14, but the retire-only-when-drained rule here is the same class).

---

## 13. Receive double buffer flip

One pattern in four places: a worker fills `buffer[counter % 2]` under a mutex held across the whole fill pass; the main thread bumps `counter` under the mutex and then drains the other buffer with no lock. XDP (`rocketnet_server_socket_linux.cpp:2057-2097` and `2151-2215`), GSO (`rocketnet_server_gso_linux.cpp:427-554` and `1092-1148`), IOCP (`rocketnet_server_iocp_windows.cpp:185-232` and `469-493`), client (`rocketnet_client_transport.cpp:466-478`, `1099-1138`). C:91-93 describes it: "the XDP thread holds `receive_mutex` across its whole fill loop, so the game's counter flip lands only between batches and the game then drains the *other* buffer lock-free".

XDP drains *every* buffer that is not the on-buffer (`2075-2095`) because `ROCKETNET_XDP_NUM_BUFFERS` may exceed 2; the others drain the one off buffer.

### Rules

- The worker and the main thread never hold the same buffer at once (mutual exclusion by parity under the mutex).
- A packet staged is drained exactly once or counted dropped (overflow at the staging bound, at the process-array bound `ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS`, `1989-1990`; GSO `1133-1137`; IOCP `481-485`).
- `num_packets` of the off buffer is zeroed by the drain before the next flip hands it back.
- Overflow is bounded by the array, never by the queue depth (the comment at `1978-1988`, a past overrun).

### Small instance

2 buffers of capacity 2, process array 3, a worker that stages 0..3 packets per pass, the main thread that flips and drains; check exclusion and the conservation count (`staged = drained + dropped`).

### Verdict: model once, instantiate four times (constants for buffer count and bounds).

---

## 14. XDP umem frame conservation and the pending-return spill

`rocketnet_pending_frames.h` and the XDP thread (`2121-2230`): consumed rx descriptors are appended to `pending_return` (all-or-zero against capacity, `16-22`), and `flush` reserves exactly `count` fill-ring slots or none, writes, submits, zeroes (`25-34`). The peek is bounded by `pending_return.available()` (`2147`) so the append cannot fail (`2219-2228`, and if it did the thread quits). Send frames come from `send_frames[]` and return via the completion ring (`2234-2247`). C:104-110 names this the top open item: the spill "has not been driven on hardware".

### Rules

- **Every umem frame is in exactly one place**: the fill ring, the rx ring, `pending_return`, the receive free pool, the tx ring, the completion ring, or the send free pool. Receive frames never cross into the send half and vice versa (`1487-1505`).
- Repeated zero reservations retain every frame; full success submits each exactly once (C:108-110, the "tested all-or-zero pending frame helper").
- The rx peek never exceeds pending storage.

### Small instance

4 receive frames, fill ring capacity 2, pending capacity 3, a kernel that moves fill → rx nondeterministically and a reserve that returns `count` or 0. Invariant: the multiset of frames across all places is constant.

### Verdict: model. Tiny, and it is the standing "unverified edge" of the dataplane.

---

## 15. Packet leases

`rocketnet_packet_lease.h`: per entry `state = (epoch << 2) | phase` with phases `0 free`, `1 writing`, `2 active`, `3 consumed` (`16-21,42,57,84`); `insert` claims a slot by CAS from any phase of an older epoch (`44-58`), capacity `Capacity/2` per fill (`37`); `resolve` walks the probe chain, spins on `writing`, matches frame and ticket, checks the other fields, and consumes by CAS `active → consumed` (`63-91`); `reset` bumps the epoch on the main thread (`98-102`, wraps to 0 = exhausted).

### Rules

- A copied context is consumed at most once (U:292-296 "A copied context can be consumed once; changed fields, stale phases and wrong-owner contexts are refused").
- A context from an earlier epoch never resolves after `reset`.
- `insert` of two contexts with the same frame in one epoch: the second finds the first's entry occupied with the same epoch and probes on (so a frame reused in one fill phase is two leases); `resolve` matches on `(frame, packet_id)` so each is distinct.

### Verdict: model, later in the list. Its properties are the lease table's own, not the protocol's, and the ownership tests already cover the single-thread cases; the value is the concurrent `insert`/`resolve` interleavings.

---

## 16. IOCP overlapped operation pool

Sends: `free_sends` SList → staged (`394-419`) → posted (`438-467`, `pending++`) → completion returns the op to `free_sends` (`194-200`). Receives: posted → completed → reposted, or to `retry_receives` on a failed repost and retried next wakeup (`137-158,224-231`). Invariant: every op is in exactly one of {free, staged, posted, retry}; `pending` equals the number posted. Fold into module 11 (stop) and 13 (the receive flip).

---

## 17. GSO feature tiers

Probed at create: `SEGMENT` and `GRO` all-or-nothing across the group with rollback (`640-686`); a live `EINVAL/EIO/EOPNOTSUPP` on a GSO send flips `SEGMENT` off and resumes from the message's first slot as plain packets, lossless (`1038-1048`, `1079-1083`). States: `{SEGMENT on/off} × {GRO on/off}`, one-way off at runtime. Rule: degradation never drops a staged packet (the comment at `1041-1043`); `EAGAIN` drops and counts (`1050-1059`). Verdict: not now. Two bits and one irreversible transition; the resume-from-slot property is a loop invariant better pinned by a test than a model.

---

## 18. Frame sync

`rocketnet_frame_sync.cpp:338-386`: first wait anchors the deadline and takes priority; spin guard only while priority is held; `advance` moves the deadline one period or resynchronizes past a late wake, counting drops (`250-268`). Pure functions, unit-tested, single-threaded. Verdict: no model.

## 19. Driver ladder

A pure function of the candidate list: first grant wins, pins refuse, skipped-for-platform names fall to auto, port-zero refuses non-discovering rungs (`rocketnet_server_transport.cpp:635-747`, `rocketnet_client_transport.cpp:307-341`; S:73-172). No state across time. Verdict: no model.

## 20. Prefilter and the kernel filter

`rocketnet_prefilter_check` and `server_xdp.c:80-181` must reject exactly the same packets (S:1675-1683 "A transport that rejects early MUST reject exactly what the full sequence would have rejected"). A predicate equivalence over packet shapes, shared via `rocketnet_wire.h`; it is a property for a test with vectors, not a state machine. Verdict: no model.

## 21. Statistics, loss and jitter trackers, rtt history

Windows over time and sequence; no rule in the standard beyond "SHOULD expose" (S:3083-3091). Verdict: no model.

## 22. The delivered window and the receive batch

"The delivered batch belongs to the update that published it ... Drain every update" (U:68-94). A one-update ownership rule; folded into module 9's delivered-capacity invariant and the transports' `begin_update`. Verdict: no separate model.

---

## The modules to write, in order

Each sibling card writes `tla/<Module>.tla`, `tla/MC<Module>.tla`, `tla/MC<Module>.cfg`, runs TLC on the bench, records the run in `tla/RUNS.tsv` (module, config, states, distinct, result, bench, date), verifies every counterexample against the code by hand, and cites the module from the code it models (a comment at the machine's home naming the module and the invariant). Shared conventions: time is a natural counter (`Tick` advances it by one); the wire is a bag with a bound; crypto is the tag predicates of machine 3; `Nat` ranges are bounded by the constants below. Invariants are named `Inv<Rule>` and liveness `Live<Rule>` so `RUNS.tsv` can cite them.

### 1. `RNReplay` (machine 5)

- Constants: `W = 8`, `Words = 4` (so `BitsPerWord = 2`), `ClearWords = 2` (the code's `4` scaled; set `= Words` for the rule), `MaxSeq = 24`.
- Variables: `highest`, `window` (a function `0..W-1 → BOOLEAN`), `opened` (the ghost set of sequences actually advanced).
- Actions: `Open(s)` for `s ∈ 1..MaxSeq` with `¬AlreadyReceived(s)` → `Advance(s)`; `Replay(s)` for any `s`: must be refused if `s ∈ opened`.
- Invariants: `InvSound`: `¬AlreadyReceived(s) ⇒ s ∉ opened`; `InvComplete`: `AlreadyReceived(s) ∧ s > highest - W ⇒ s ∈ opened`; `InvBlockTruth`: every window bit set for `s ∈ (highest - W, highest]` has `s ∈ opened`.
- `MCRNReplay.tla`: `EXTENDS RNReplay`, defines the constants above and `StateConstraint == highest <= MaxSeq`.
- `MCRNReplay.cfg`: `SPECIFICATION Spec`, `CONSTANTS W = 8, Words = 4, ClearWords = 2, MaxSeq = 24`, `INVARIANTS InvSound InvComplete InvBlockTruth`, `CONSTRAINT StateConstraint`.
- Expected: `ClearWords = 2` violates `InvComplete` and `InvBlockTruth` (the trace of machine 5 at small scale); `ClearWords = Words` passes.

### 2. `RNAck` (machine 6, with machine 10's sender actions)

- Constants: `W = 8` (sent ring), `R = 4` (received ring), `A = 4` (aggregates), `MaxRanges = 2`, `Batch = 2`, `MaxSeq = 24`, `WireBits = 3`.
- Variables: sender `sent`, `highestSent`, `aggregates`, `numAggregates`, `batch`, `latch`, `counters`; receiver `replay` (an instance of `RNReplay` with `ClearWords = Words`), `ackDebt`; wire `blocks` (bag of built blocks in flight).
- Actions: `SendPacket(kind ∈ {KeepAlive, Payload, Ack, Disconnect})`, `StartFragments(n)`, `SealFragment(i)` / `WithdrawFragments(finishFinal)`, `DeliverPacket` (receiver opens; `replay` advances), `LoseInWire`, `ReorderInWire`, `BuildBlock(capacity)`, `DeliverBlock`, `DrainBatch`.
- Invariants: `InvBlockValid` (a built block passes `BlockRead`), `InvBlockTruth` (every named sequence is in the receiver's `opened`), `InvSurfaceOnce` (a packet id appears in drained batches at most once), `InvSurfaceOnlyIfOpened` (every surfaced packet id's sequence, or all its fragments' sequences, are in `opened`), `InvNoLap` (an aggregate decrement only when `aggregate.id` matches), `InvWalkBounded` (records visited per block ≤ W), `InvLatchOnlyKeepAlive`.
- Liveness: `LiveEventuallyAcked` under weak fairness on `BuildBlock` and `DeliverBlock`, for records not recycled.
- `MCRNAck.cfg`: the constants above, `INVARIANTS` the seven, `PROPERTY LiveEventuallyAcked`, `CONSTRAINT highestSent <= MaxSeq`.

### 3. `RNTokenTables` (machine 7)

- Constants: `Capacity = 2`, `Ids = {1,2,3}`, `Addrs = {a,b}`, `MaxTime = 5`, `Expiries = {2,4}`, `EvictOldest ∈ BOOLEAN`.
- Variables: `entries` (index → record or free), `chains`, `freeList`, `order`, `now`.
- Actions: `Check`, `Record`, `Contains`, `Add`, `Tick`.
- Invariants: `InvFindable`, `InvNeverEvictUnexpired` (when `¬EvictOldest`), `InvOneAdmission` (`Add(id)` succeeded at `t` ⇒ `Contains(id)` at all `t' < expire`), `InvBindsFirstAddress`, `InvPartition`.
- Two cfgs: `MCRNTokenHistory.cfg` (`EvictOldest = TRUE`) and `MCRNAdmittedSet.cfg` (`FALSE`).

### 4. `RNServer` (machine 2, with the integrity event and the sweep)

- Constants: `Addrs = {a,b}`, `Slots = 1`, `Mappings = 2`, `ClientIds = {1,2}`, `HandshakeDeadline = 3`, `Timeout = 4`, `MaxTime = 12`, `IntegrityLimit = 2`.
- Variables: `mapping` (addr → record or none), `slot` (index → record or free), `admitted` (an instance of `RNTokenTables` with `EvictOldest = FALSE`), `now`, `counters`.
- Actions: `RequestNoMapping(addr, tokenId, clientId, slotsFree)`, `RequestWithMapping(addr)`, `Response(addr)`, `SessionPacket(addr, type)`, `Disconnect(addr)`, `IntegrityFailure(addr)`, `DisconnectClient(i)`, `Sweep`, `Tick`.
- Invariants: `InvHalfOpenHoldsNoSlot`, `InvBijection`, `InvOneMappingPerAddr`, `InvOneSlotPerClientId`, `InvDeadlineFixed` (the `handshake_deadline` of a mapping never changes), `InvAdmittedIffEstablished`, `InvCounters`.
- Liveness: `LiveHalfOpenDies`, `LiveSilentDies`.

### 5. `RNClient` (machine 1)

- Constants: `SendInterval = 1`, `Timeout = 3`, `TokenLife = 6`, `MaxTime = 10`, `IntegrityLimit = 2`.
- Variables: `state`, `hasCookie`, `hasKeys`, `seqHandshake`, `seqSession`, `lastSend`, `lastReceive`, `attemptStart`, `ackDebt`, `integrity`, `now`, `sent` (the ghost log of sends with times).
- Actions: `Connect(valid, expired)`, `Update`, `Cookie(saltOk, state)`, `Challenge(opens, smallOrder)`, `Denied(opens)`, `Session(type, opens)`, `Disconnect`, `Tick`.
- Invariants: `InvMonotone` (the only positive-to-positive moves are `+1`), `InvPairing`, `InvKeysIffAtLeast3`, `InvSeqNeverReset`, `InvOneSendPerInterval` (plus standalone acks), `InvNoSendOnArrival` (the ghost log gains an entry only in `Update`), `InvGate` (events refused outside their states leave the state unchanged).
- Liveness: `LiveConnects` (with an oracle server answering every retransmission), `LiveTimesOut` (with a silent one).

### 6. `RNHandshake` (machine 3; composes 4 and 5 with the wire and the cookie gate of machine 8)

- Constants: the instance of machine 3 above (`Addrs = {a,b}`, attacker `x`, `Slots = 1`, `Mappings = 2`, `Tokens = 3`, `WireBound = 3`, the four timers).
- Variables: `client[addr]` (instances of `RNClient`), `server` (an instance of `RNServer`), `cookieKey`, `issued` (the backend's token set), `wire`, `now`.
- Actions: every action of the two instances, lifted; `Deliver`, `Lose`, `Duplicate`, `Replay(p, x)`, `Inject`, `Tick`.
- Invariants: `InvCookieBeforeSignature`, `InvNoSlotWithoutResponse`, `InvAnsweredEveryTime`, `InvAmplification`, `InvNonceUnique`, `InvTokenAdmitsOnce`, `InvClientIdOnce`, `InvPhaseSeparation`, `InvInjectMovesNothing`, `InvNoStrand`.
- Liveness: `LiveConnect`, `LiveRefusedTimesOut`, `LiveFullDenied`, `LiveHalfOpenDies`.
- This is the module to run largest; start with one client and the attacker, add the second client once the first instance is green.

### 7. `RNKeepAlive` (machine 4)

- Constants: `Interval = 2`, `Tick = 1`, `Ring = 4`, `MaxLoss = 3`, `Truthful ∈ BOOLEAN`, `MaxTime = 16`.
- Variables: `latch`, `lastSend`, `lastKeepAliveSend`, `ring` (sequence → tagged record), `clientState ∈ {3,4}`, `clientOpened`, `wire`, `now`.
- Actions: `ServerUpdate` (the due predicate; may also send a payload if the app chooses; standalone ack when debt ≥ threshold), `AppSend`, `Deliver`/`Lose` (server→client), `ClientUpdate` (sends a response or keep-alive carrying a block built from `clientOpened`, or an over-claiming block when `¬Truthful`), `DeliverBlock`, `Tick`.
- Invariants: `InvUnsuppressedUntilAcked` (`¬latch ⇒ now - lastKeepAliveSend ≤ Interval + Tick`), `InvSuppressedAfter` (a keep-alive sent at `t` with `latch` ⇒ `t - lastSend ≥ Interval`), `InvTruthfulLatchMeansConnected` (`Truthful ∧ latch ⇒ clientState = 4`), `InvLatchOnlyFromOwnKeys`.
- Liveness: `LiveEstablishes` (`Truthful ⇒ <>clientState = 4` under fairness on delivery of some keep-alive).
- Two cfgs: `Truthful = TRUE` must pass everything; `Truthful = FALSE` must violate only `LiveEstablishes` (the stranding the standard accepts, S:2736-2746), and nothing else.

### 8. `RNReassembly` (machine 9)

- Constants: `Slots = 2`, `IdMod = 8`, `Counts = {2,3}`, `Strides = {1,2}`, `MaxPayload = 5`, `Delivered = 2`.
- Variables: `slot[i]` records, `delivered` (sequence of token sequences), `numDelivered`, `counters`, `inFlight` (the wire bag of fragments).
- Actions: `SendPayload(id, count, stride, finalBytes)` (puts all fragments in the bag), `Deliver(fragment)`, `Lose(fragment)`, `BeginUpdate`.
- Invariants: `InvBytesOnceOrNothing` (each delivered payload equals its sent token sequence; each id delivered at most once), `InvCountPinned`, `InvStridePinned`, `InvStagedFinalPlacedOnce`, `InvRefusalMutatesNothing` (except the two named clears), `InvTooLargeNeverOccupies`, `InvStaleRefused`, `InvArenaBound`, `InvBatchFullZero` at `Delivered = Slots`.
- Liveness: `LiveCompletes` when every fragment of an id is delivered before a newer id claims its slot.

### 9. `RNDriverStop` (machine 11, with machine 16's op pool)

- Constants: `Workers = 2`, `ReceiveOps = 2`, `SendOps = 1`.
- Variables: `transportState`, `stopping`, `pending`, `activeWorkers`, `ops` (op → place), `xdpState`.
- Actions: `Stop`, `Update`, `WorkerLoop`, `Complete(op)`, `Post(op)` / `PostFails(op)`, `SendPackets`, `Destroy`.
- Invariants: `InvNoPostAfterStopping`, `InvPendingCountsPosted`, `InvStoppedMeansQuiet`, `InvDestroyOnlyStopped`, `InvOpInOnePlace`.
- Liveness: `LiveStops` (`stopping ⇒ <>transportState = STOPPED`).

### 10. `RNSendHandoff` (machine 12)

- Constants: `Generations = 2`, `QueueSize = 3`, `BatchSize = 2`, `Frames = 3`.
- Variables: `free`, `queued`, `filling`, `flowIndex`, `buffers` (generation → packets with bytes), `startIndex`, `umem`, `txRing`, `completions`.
- Actions: `Start`, `Finish`, `FinishStale` (a pointer into a retired generation), `Abort`, `Flush`, `WorkerBatch`, `Complete`.
- Invariants: `InvNotBothFillingAndQueued`, `InvRetireOnlyDrained`, `InvSentOnce`, `InvNoWriteToQueued`, `InvRefuseWhenBothQueued`, `InvFrameConservation` (with machine 14's places).

### 11. `RNReceiveFlip` (machine 13)

- Constants: `Buffers = 2` (XDP may be instantiated with more), `Capacity = 2`, `ProcessBound = 3`.
- Variables: `counter`, `buffers`, `holder` (who holds each buffer), `staged`, `drained`, `dropped`.
- Actions: `WorkerPass(n)`, `Flip`, `Drain`.
- Invariants: `InvExclusion`, `InvConservation` (`staged = drained + dropped`), `InvOffBufferEmptyAfterDrain`.
- Four cfgs by constants: XDP (`Buffers = 2`, bound by the process array), GSO, IOCP, client.

### 12. `RNPendingFrames` (machine 14)

- Constants: `RxFrames = 4`, `FillRing = 2`, `Pending = 3`.
- Variables: `place` (frame → {fill, rx, pending, free}), `fillReserved`.
- Actions: `KernelReceive`, `Peek`, `Append`, `FlushReserveOk`, `FlushReserveZero`.
- Invariants: `InvOnePlace`, `InvAllOrZero`, `InvPeekBounded`.

### 13. `RNPacketLeases` (machine 15)

- Constants: `Capacity = 4`, `Fills = 2`.
- Variables: `entries`, `used`, `epoch`, `tickets`.
- Actions: `Insert`, `Resolve(consume)`, `Reset`, two concurrent threads interleaved.
- Invariants: `InvConsumeOnce`, `InvStaleEpochRefused`, `InvCapacityHalf`.

### 14. `RNCookieRotation` (machine 8's rotation)

Blocked: the machine has no transitions in the code. Write it when rotation lands; the invariant and liveness are stated under machine 8.

---

## Defects and deviations found by reading (for the report; no code change on this card)

1. **Replay window clear is 256 bits wide on a 4096-bit window** (`rocketnet_replay.cpp:54-60`). Verified by hand, machine 5 above. Consequence: a genuine packet refused as `ALREADY_RECEIVED` after a jump of a window or more, and an ack block that names a sequence never opened, which can falsely latch `keep_alive_acked`. Introduced by `ea9b2c0` (2026-09-07) widening the struct without the clear; the existing test's far-ahead case sits in the word that is cleared.
2. **Cookie key rotation is not implemented** (`rocketnet_server_transport.cpp:792` generates the key once; nothing writes `previous_cookie_key`). S:1176-1179 and S:1995 say rotate at 60 s and accept the previous key for one period; C:180-181 says it runs always. The gate's previous-key branch (`rocketnet_server_protocol.cpp:231-236`) is reachable only from the test.
3. **Slot assignment is a linear scan, not a free list** (`rocketnet_client_slots.cpp:148-156`). S:1594-1595: "A free slot is found from a free list, not a search"; S:1589: the free-slot lookup "every completed connection" must be constant time. Correctness is unaffected (the lowest free index is deterministic, which the models can rely on); the complexity rule is not met at 1000 slots.
4. Observation, not a defect: on the server a session packet that opens for a half-open mapping is replay-marked before being refused as not connected (`rocketnet_server_protocol.cpp:791,796`), as S:1486-1492 allows; the model of machine 2 should encode that order so a later change does not move the mark after the gate (which would let an injected sequence be re-tried).
5. Observation: `rocketnet_server_connected_mapping` tolerates a connected slot without a mapping by counting `SLOT_WITHOUT_MAPPING` (`1062-1090`) because `client_slots_assign` accepts `-1`; machine 2's `InvBijection` is the invariant that keeps that counter at zero, and the model should include `assign(-1)` as an unreachable action to show it.

## What this inventory did not cover

- `rocketnet_platform.cpp` (sockets, threads, the msgx pump on Darwin): not in START; the Darwin pump's "live-refusal pause" (S:165-167) is a small machine worth a look on a later card.
- `rocketnet_network_simulator.cpp`: test infrastructure.
- The C# binding and `soak.c`: above the transport.
- Nothing ran: no TLC, no bench. The modules above are the sibling cards' work.

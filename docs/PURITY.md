# Which connection and security machines are pure

This document audits the connection and security machines M1 to M7 of
`docs/TLA-INVENTORY.md` against the pure-module rule of `docs/CHANNELS.md` 16.3. A pure
module has its state in one struct, takes events in and puts actions out, and takes time
as an argument. It does no I/O, reads no clock, reads no global and allocates nothing
inside a transition. For each machine the document names the state struct, the events,
the actions, the time source and every impurity with its `file:line`, and gives a verdict.
THE CARDS lists the ordered extraction cards for every machine that is not yet pure.

All line numbers are at `4ea64ce` (`sprint/rocketnet-proof`). Paths are relative to
`sdk/` unless they say otherwise. The inventory's own line numbers are at `5cb7613` and
have moved; this document re-reads every site at `4ea64ce`.

## What counts as an impurity

The rule is applied to transitions: the functions that run on an event once the
machine exists. Each finding is in one of these classes.

| Class | Meaning | Counts against the verdict |
|---|---|---|
| clock | the transition reads a clock instead of taking time as an argument | yes |
| entropy | the transition draws from the system random source (`randombytes_buf`, `crypto_box_keypair`). The draw is an input from outside the state, the same as a clock read | yes |
| behaviour global | the transition reads a value outside its state struct, a global or a counter, and that value decides what it does | yes |
| I/O | the transition calls the transport, a driver or a socket, or runs interleaved with those calls | yes |
| allocation | the transition allocates | yes |
| construction | allocation or a random draw in `create`, outside every transition | no: recorded, because the struct cannot yet be supplied by the caller |
| write-only instrumentation | a counter, tally or log that the transition writes and nothing in the machine reads back. Logs read the log level global (`rocketnet.cpp:88`) and print with a clock (`rocketnet.cpp:239`). They never change a result | no: recorded, see "Write-only instrumentation" |
| tangle | the machine calls another machine's code directly, or keeps part of its state in another machine's struct. Each part may be pure on its own, but the machine cannot be driven alone | no, but the verdict is pure-but-tangled |

`rocketnet_assert` is not counted: it states a precondition and is not part of a result.
The keyed hashes and AEAD calls (`crypto_shorthash`, seal and open) are deterministic
functions of their inputs and are not counted.

Verdicts: **pure** (no counted impurity, no tangle), **pure-but-tangled** (no counted
impurity, but at least one tangle), **impure** (at least one counted impurity).

## The table

| Machine | State struct | Events | Actions | Time source | Impurities (`file:line`, class) | Verdict |
|---|---|---|---|---|---|---|
| M1 client connection | `rocketnet_client_protocol_t` (`rocketnet_client_protocol.h:32-112`), with `rocketnet_ack_endpoint_t ack` embedded (`:100`) and pointers to the counters (`:37`) and to the reassembly set (`:111`) | connect (`rocketnet_client_protocol.cpp:122`), update (`:291`), process_packet (`:1044`), the fill-phase sends (`:1140-1377`), disconnect (`:1379`) | the packet written into `out_packet` (`:291-378`), the disconnect burst into `out_packets` (`:1379-1424`), `state`, the delivered payload in `out_payload` | `current_time` argument everywhere except disconnect; `current_timestamp` (unix) argument to connect, read by the caller at `rocketnet_client_transport.cpp:299` (`::time( NULL )`); disconnect stamps its burst with the stored `last_send_time` (`:1407`) | entropy: `:198` (salt), `:200` (ephemeral keypair). Tangle: `:146` and `:1010` (reassembly, M9); `:281-285`, `:788-802` (ack endpoint, M8); the fill-phase atomics `:1148`, `:1191`, `:1193`, `:1257`, `:1270`, `:1327`, `:1329`. Write-only: `:199` (TLS crypto tally), the counters writes, `:43` and the log calls | **impure** |
| M2 server per-address handshake | `rocketnet_server_protocol_t` (`rocketnet_server_protocol.h:36-70`) and the map `rocketnet_encryption_map_t` (`rocketnet_encryption_map.cpp:15-29`), one `rocketnet_encryption_mapping_t` per address (`rocketnet_encryption_map.h:37-61`) | process_connection_request (`rocketnet_server_protocol.cpp:159`), process_packet (`:1146`), update (`:1256`), disconnect_client (`:1861`) | the reply written into `response_packet` and the process code (`rocketnet_server_protocol.h:29-34`, and `ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED` at `rocketnet_counters.h:26`); the payload and its slot in `out_payload`; the disconnect burst into `out_packets` | `current_time` and `current_timestamp` arguments, read by the caller at `rocketnet_server_transport.cpp:1041-1042` (`::time( NULL )`); disconnect_client stamps its burst with the stored `last_send_time` (`:1894`) | entropy: `:546` (session nonce), `:584` (server ephemeral keypair). Behaviour global: `:439` reads and `:445` writes `counters->handshakes_processed_this_update`, reset at `:1267`; the field lives in `rocketnet_counters_t` (`rocketnet_counters.h:568`), not in the protocol state, and it decides whether a request is dropped. Tangle: M3 at `:525`, `:561`, `:688`, `:704`, `:736`, and the release paired with the mapping removal at four sites (`:802`/`:805`, `:1140`/`:1141`, `:1292`/`:1293`, `:1912`/`:1913`); M6 at `:453`, `:532`, `:539`, `:711`; M8 at `:745`, `:1049-1057`, `:1387-1392`; M9 at `:748`, `:1123`; fill-phase atomics `:1587`, `:1635`, `:1638`, `:1732`, `:1746`, `:1808`, `:1811`. Write-only: `:106`/`:122` (TLS cookie mac tally), `:583` (TLS crypto tally), counters, logs. Construction: `rocketnet_encryption_map.cpp:50-64` (allocation), `:58` (hash key) | **impure** |
| M3 server client slots | `rocketnet_client_slots_t` (`rocketnet_client_slots.cpp:14-41`) | assign (`:163`), confirm (`:242`), release (`:251`); queries find (`:151`), num_free (`:136`) | the slot index returned by assign, or -1 when full | none | none counted. Construction: `:63-80` (allocation), `:71` (hash key; it orders the bucket chains only, never which slot is chosen). Write-only: `:195`, `:247`, `:292` (debug logs) | **pure** |
| M4 keep-alive generation and the acknowledgment latch | split across three structs: per mapping `last_keep_alive_send_time` and `last_send_time` (`rocketnet_encryption_map.h:37-61`); per slot `keep_alive_acked` and the per-record `was_keep_alive` in `rocketnet_ack_endpoint_t` (`rocketnet_ack.h:119-164`, the latch at `:156`); the round-robin cursor `maintenance_next_client` in the server transport (`rocketnet_server_transport.cpp:199`) | the due check (`rocketnet_server_protocol.cpp:1477-1556`), the keep-alive write (`:1398-1448`), the tag (`rocketnet_ack.cpp:312-323`), the latch on an ack (`rocketnet_ack.cpp:438`, inside `rocketnet_ack_apply_run`), the per-slot walk (`rocketnet_server_transport.cpp:1251-1271`) | the keep-alive or standalone ack written into a send frame | `current_time` argument | I/O: the cursor transition runs inside the transport's frame loop, interleaved with `rocketnet_server_reserve_send_frame`, `rocketnet_server_commit_send_frame` and `rocketnet_server_abort_send_frame` (`rocketnet_server_transport.cpp:1251-1271`); the break on an exhausted frame pool at `:1261-1263` is what moves the cursor. Tangle: the latch is set inside M8's apply (`rocketnet_ack.cpp:438`) and read across the struct boundary at `rocketnet_server_protocol.cpp:1547`; the tag takes the sent-ring spin lock (`rocketnet_ack.cpp:318`, `:322`). Write-only: `rocketnet_ack.cpp:7`/`:397` (`g_ack_visit_count`, a global written in the same apply loop) | **impure** |
| M5 cookie gate and key rotation | fields of `rocketnet_server_protocol_t` (`rocketnet_server_protocol.h:49-55`), not a struct of their own; the client side is `has_cookie` and `return_cookie` in `rocketnet_client_protocol_t` | request past the fixed checks (`rocketnet_server_protocol.cpp:352-431`), rotation (`:1225-1254`, called at `:355` and `:1264`); client cookie receive (`rocketnet_client_protocol.cpp:380-433`) | the fresh cookie packet into `response_packet` (`:428-430`), or pass | `current_time` and `current_timestamp` arguments; the first rotation time is set from the transport clock at create (`rocketnet_server_transport.cpp:830`) | entropy: `:1252` (the fresh cookie key, drawn inside the rotation, which the request path runs at `:355`). Tangle: the gate state is not separable from the server struct. Construction: the first key at `rocketnet_server_transport.cpp:825`. Write-only: `:122` (TLS cookie mac tally), counters, logs | **impure** |
| M6 token tables | `rocketnet_token_table_t` (`rocketnet_token_tables.cpp:26-41`), seen as `rocketnet_used_token_history_t` (the admitted-token set view is removed, `docs/SESSION.md` 1.6) | history check, history record (the set's contains and add are removed) | the check and contains answers; add succeeds or refuses | `current_timestamp` argument (unix, read by the caller at `rocketnet_server_transport.cpp:1042`) | none counted. Construction: `:62-77` (allocation), `:70` (hash key; it orders the bucket chains only, never which slot `select_slot` picks, `:168-196`) | **pure** |
| M7 replay window and sequence reconstruction | `rocketnet_replay_protection_t` (`rocketnet_encryption_map.h:31-35`) | already_received (`rocketnet_replay.cpp:29`), advance (`:44`), reconstruct (`rocketnet_protocol.cpp:420`, through `rocketnet_reconstruct_near` at `:385`) | the answer, the advanced window; the ack block written by `rocketnet_ack_block_build` (`rocketnet_ack.cpp:54-137`) | none | none. The ack block build keeps its own copy of the bit read (`rocketnet_ack.cpp:48`, `rocketnet_ack_replay_bit`), a second reader of the representation, not an impurity | **pure** |

## Per machine

### M1, the client connection: impure

The protocol layer already takes time as an argument (`current_time` in every event,
`current_timestamp` at connect) and puts every packet into a caller buffer. What makes
it impure is the attempt's entropy: `rocketnet_client_protocol_connect` draws the
connection salt (`rocketnet_client_protocol.cpp:198`) and the ephemeral keypair (`:200`)
itself. Connect is an application call, not a receive, so nothing about cost order stops
the caller from drawing them: `ReceiveOrder` does not cover the client.

The rest is tangle, not impurity. The struct embeds M8's ack endpoint and drives it from
the session write (`:281-285`) and the session open (`:788-802`); it resets M9's
reassembly at connect (`:146`) and feeds it fragments (`:1010`). The fill-phase sends
spend sequences with atomics (`:1148`, `:1257`, `:1270`) and reset the ack debt and send
clock with atomic stores (`:1191`, `:1193`, `:1327`, `:1329`), as `docs/CHANNELS.md`
16.1 allows. The disconnect burst (`:1379-1424`) takes no time argument and records its
packets at the stored `last_send_time` (`:1407`), so the delivery records carry a stale
send time. That is a behaviour question, outside a card that only moves code, and is
recorded as a finding below.

After PUR-5, M1 is pure-but-tangled. Its tangles are the composition with M8 and M9,
which their own purity cards answer.

### M2, the server's per-address handshake: impure

Two counted impurities.

1. **Entropy.** `rocketnet_server_process_opened_request` draws the session nonce
   (`rocketnet_server_protocol.cpp:546`) and the server ephemeral keypair (`:584`).
   They cannot simply be hoisted into the caller. The keypair is the last rung of
   `ReceiveOrder` (`tla/ReceiveOrder.tla`, `CheapRefusalsFirst`, rung `keypair`), and
   drawing it before the request runs would pay a keypair for every refused request,
   which is the flood shape the ladder exists to make cheap. The ways out are a
   two-phase request (the machine returns "needs a keypair" and the caller supplies it
   in a second event) or a deterministic generator held in the state (a seed ratcheted
   and erased on every draw, so a captured state does not reveal earlier ephemerals).
   Both are design changes, and the second touches forward secrecy (STANDARD.md, the
   ephemeral erasure rule that `EphemeralErasedOutsideHandshake` pins). PUR-6 is that
   design, not a move.
2. **A counter that changes behaviour.** The per-update handshake budget is read at
   `:439` and incremented at `:445`. It is reset at `:1267`, and it lives in
   `rocketnet_counters_t` (`rocketnet_counters.h:568`), the published counters struct,
   not in the protocol state. A request is dropped on what that field holds. PUR-3
   moves the field into `rocketnet_server_protocol_t`.

Tangle: the request and response ladders call M3, M6, M8 and M9 directly through
pointers held in the server struct (the sites in the table). The pair "release the slot,
remove the mapping" is written out at four sites (`:802`/`:805`, `:1140`/`:1141`,
`:1292`/`:1293`, `:1912`/`:1913`), the binding `SlotMappingBijection` is about.
`tla/ServerConnection.tla` already names that pair as one action, `Release(a)`. PUR-4
moves it into one function.

The request's signature-verify budget (`:23-99`) is pure: state in
`rocketnet_signature_verify_budget_t`, time as `current_timestamp`.

After PUR-3, PUR-4 and PUR-6, M2 is pure-but-tangled.

### M3, the server's client slots: pure

Every transition reads and writes only `rocketnet_client_slots_t`. Assign pops the free
stack (`rocketnet_client_slots.cpp:179-181`), release pushes it, and neither takes time.
The random hash key (`:71`) orders the bucket chains only. The three debug logs
(`:195`, `:247`, `:292`) change nothing. No card.

### M4, keep-alive generation and the acknowledgment latch: impure

The pacing rule itself is pure: `rocketnet_server_protocol_write_keep_alive_if_due`
(`rocketnet_server_protocol.cpp:1477-1556`) reads its clocks and the latch and takes
`current_time` as an argument. What is impure is where the rest of the machine lives.

- The round-robin cursor (`rocketnet_server_transport.cpp:199`) is advanced inside the
  transport's frame loop (`:1251-1271`). The cursor step depends on whether
  `rocketnet_server_reserve_send_frame` returned a frame (`:1261-1263`). That is the
  I/O result `EverySlotServiced` is about, and it cannot be driven without a transport.
- The latch is set inside M8's apply loop (`rocketnet_ack.cpp:438`), and the tag takes
  M8's spin lock (`rocketnet_ack.cpp:318-322`). The latch belongs in M8's endpoint by
  design, because the ack is the signal. It is recorded as a tangle, and M8's own audit
  answers the lock.

`tla/ServerConnection.tla` models the pacing as `ServerKeepAlive(i)` with the latch as
`m.acked`, but no model has the cursor or frame exhaustion: inventory module 5,
`RNKeepAlive`, is not written. PUR-7 writes it, and PUR-8 moves the cursor walk out of
the frame loop and cites it.

After PUR-7 and PUR-8, M4 is pure-but-tangled (the latch stays in M8's struct).

### M5, the cookie gate and key rotation: impure

The gate (`rocketnet_server_protocol.cpp:352-431`) is pure apart from its state's home.
It takes both clocks as arguments and writes the fresh cookie into `response_packet`.
The rotation (`:1225-1254`) draws the next cookie key itself (`:1252`), and the request
path runs the rotation (`:355`), so a receive can reach the random source. The gate
state is seven fields of the server struct (`rocketnet_server_protocol.h:49-55`), not a
struct of its own. The client half, the cookie store (`rocketnet_client_protocol.cpp:380-433`),
is pure apart from the counters.

PUR-1 moves the gate state and the gate into a struct of their own. PUR-2 makes the
rotation consume a key that the caller drew before the update. A pre-drawn next key sits
in memory for at most one rotation period before it is used. A leaked cookie key forges
cookies, not sessions, and the previous key is already held for one period
(`PreviousBoundedToOnePeriod`).

After PUR-1 and PUR-2, M5 is pure.

### M6, the token tables: pure

Every operation takes `current_timestamp` and reads and writes only the table. Slot
choice is the free list, then an expired slot, then (history only) the oldest
(`rocketnet_token_tables.cpp:168-196`). The random hash key (`:70`) orders the chains
only. No card.

### M7, the replay window and sequence reconstruction: pure

`rocketnet_replay.cpp` has no call outside the window struct beyond `memset`
(`:60`). `rocketnet_reconstruct_sequence` (`rocketnet_protocol.cpp:420-423`) and
`rocketnet_reconstruct_near` (`:385-418`) are arithmetic. The ack block build
(`rocketnet_ack.cpp:54-137`) reads the window and writes a caller buffer. It keeps its
own bit read (`:48`) where it could call the replay module's. That is duplication, not
impurity, and `tla/ReplayWindow.tla` already models the block as that reader
(`AckReports`). No card.

## Write-only instrumentation

These are globals that the machines write and never read back. They do not change a
result, so they do not count against a verdict. They are listed because a pure module in
the 16.3 sense touches no global at all, and whether test tallies may stay as
thread-locals is a call for the maintainer.

| Global | Defined | Written from the machines |
|---|---|---|
| `rocketnet_crypto_tally` (thread_local) | `rocketnet_protocol.cpp:18`, declared `rocketnet_crypto_tally.h:24` | `rocketnet_client_protocol.cpp:199`, `rocketnet_server_protocol.cpp:583`; through seal and open at `rocketnet_protocol.cpp:207`, `:263`, `:325`; `rocketnet_keys.cpp:76`; `rocketnet_connect_token.cpp:189`, `:199` |
| `rocketnet_server_cookie_mac_calls` (thread_local) | `rocketnet_server_protocol.cpp:106` | `:122` |
| `g_ack_visit_count` | `rocketnet_ack.cpp:7` | `:397` |

The counters struct (`rocketnet_counters_t`) is not a global. It is reached through a
pointer held in each protocol struct. Apart from the handshake budget field (M2), its
writes are outputs.

## Findings outside the cards

- **F-P1, disconnect bursts carry a stale send time.** `rocketnet_client_protocol_disconnect`
  (`rocketnet_client_protocol.cpp:1407`) and `rocketnet_server_protocol_disconnect_client`
  (`rocketnet_server_protocol.cpp:1894`) take no time argument and record each burst
  packet at the stored `last_send_time`. The records feed RTT samples and the loss rule
  in M8. The fix adds a `current_time` argument. That changes a signature and its
  callers, so it is a behaviour card, not an extraction card, and it is proposed here
  only.
- **F-P2, the budget the model names.** `tla/RNSecurityTables.tla` counts `verifies` per
  `Request` and resets it in `Tick`: the budget check at `:108`, the increment `verifies' = verifies + 1` at `:110`, the reset `verifies' = 0` at `:163`. Its header says the
  failed-verify budget (`rocketnet_server.h`), but the code's failed-verify budget
  resets by window (`rocketnet_server_protocol.cpp:23-31`), not per update. A reset per
  update is the handshake budget's shape (`:439`, `:1267`). PUR-3 cites the model's
  `Request` and `Tick`, and its reader confirms which budget `verifies` is.

## THE CARDS

These come in the inventory's bottom-up order: M7 and M6 need nothing, then M5 (module 2),
then M2 and M1 (module 3), then M4 (module 5). Each extraction card moves code and never
rewrites it. Where a signature has many callers, the existing function stays as the
shell that draws the input and calls the moved pure function, so callers do not change.
Each card names its unit test (no real time, no sockets, a test clock starting at
1000.0) and the TLA+ module and action it cites from the moved function. Small
extraction cards are tier pro; model and design cards are tier heavy.

| # | Card | Machine | Tier | Files (at most 4) | What moves | Unit test | TLA+ cited |
|---|---|---|---|---|---|---|---|
| PUR-1 | `rn-purity-cookie-gate-struct` | M5 | pro | `sdk/rocketnet_server_protocol.h`, `sdk/rocketnet_server_protocol.cpp`, `sdk/rocketnet_server_transport.cpp`, `test_unit_cookie_key_rotation.cpp` | the seven cookie fields (`rocketnet_server_protocol.h:49-55`) into `rocketnet_cookie_gate_t`, embedded in the server struct; the gate block (`rocketnet_server_protocol.cpp:357-431`) into `rocketnet_cookie_gate_check( gate, from, salt, cookie, current_time, current_timestamp, out )`; the create-time setup (`rocketnet_server_transport.cpp:825-831`) to the struct | `test_cookie_gate_runs_without_a_server`: a gate alone, clock from 1000.0, current key, previous key in and out of window, expired cookie | `RNSecurityTables` `Request` (the `g \notin LiveGens` rung), `RotationWindow`; `ReceiveOrder` rung `cookie` |
| PUR-2 | `rn-purity-cookie-rotation-takes-the-key` | M5 | pro | `sdk/rocketnet_server_protocol.h`, `sdk/rocketnet_server_protocol.cpp`, `sdk/rocketnet_server_transport.cpp`, `test_unit_cookie_key_rotation.cpp` | the draw at `rocketnet_server_protocol.cpp:1252` out of the rotation into the transport update, before `rocketnet_server_protocol_update` (`rocketnet_server_transport.cpp:1240`), into a `next_cookie_key` the rotation consumes; `rocketnet_server_protocol_rotate_cookie_key_if_due` stays as the shell for its test callers | `test_cookie_rotation_consumes_the_drawn_key`: the key after rotation equals the key supplied, and a stall rotation consumes it the same way | `RNSecurityTables` `Tick` (the rotation branch), `PreviousBoundedToOnePeriod`, `InFlightCookieSurvivesRotation` |
| PUR-3 | `rn-purity-handshake-budget-in-protocol-state` | M2 | pro | `sdk/rocketnet_counters.h`, `sdk/rocketnet_server_protocol.h`, `sdk/rocketnet_server_protocol.cpp`, `test_unit_handshake_budget.cpp` (new) | `handshakes_processed_this_update` (`rocketnet_counters.h:568`) into `rocketnet_server_protocol_t`, with its read (`rocketnet_server_protocol.cpp:439`), increment (`:445`) and reset (`:1267`) | `TestHandshakeBudgetResetsEachUpdate`: a budget's worth of cookie-valid requests pass, the next is dropped, an update restores it; no transport. The existing `test_handshake_work_budget_per_update` (`sdk/rocketnet_tests.cpp:3615`) stays green unchanged | `RNSecurityTables` `Request` (`verifies >= VerifyBudget`) and `Tick` (`verifies' = 0`), `VerifyBudgetRespected` (see F-P2) |
| PUR-4 | `rn-purity-one-release-path` | M2/M3 | pro | `sdk/rocketnet_server_protocol.cpp`, `test_unit_handshake_witness.cpp` | the four release-and-remove pairs (`:802`/`:805`, `:1140`/`:1141`, `:1292`/`:1293`, `:1912`/`:1913`) into one static `rocketnet_server_release_connection( server, mapping_index )`, each site's counters left where they are | `TestEveryTeardownReleasesSlotAndMapping`: integrity limit, client disconnect, idle timeout and application disconnect each leave the slot free and the mapping gone, and the slot is reusable at once | `ServerConnection` `Release(a)`, `SlotMappingBijection` |
| PUR-5 | `rn-purity-client-connect-takes-entropy` | M1 | pro | `sdk/rocketnet_client_protocol.h`, `sdk/rocketnet_client_protocol.cpp`, `test_unit_handshake_witness.cpp` | the body of `rocketnet_client_protocol_connect` (`rocketnet_client_protocol.cpp:122-213`) into `rocketnet_client_protocol_connect_with( client, token, salt, public_key, secret_key, current_time, current_timestamp )`; the draws at `:198` and `:200` and the tally at `:199` stay in `rocketnet_client_protocol_connect`, now the shell, so its 50 callers are unchanged | `TestClientConnectUsesTheGivenSaltAndKeypair`: a fixed salt and keypair give a byte-identical first request at 1000.0 on two runs | `ClientConnection` `AppConnect` |
| PUR-6 | `rn-purity-server-handshake-entropy-design` | M2 | heavy | `docs/PURITY.md`, `tla/ReceiveOrder.tla`, `tla/README-ReceiveOrder.md`, `tla/CASES.tsv` | none yet: a design and model card. It chooses between a two-phase request (the machine returns a need-keypair action, the caller supplies the pair in a second event) and a ratcheted seed held in the state, erased per draw. It extends `ReceiveOrder` with the chosen boundary, with a reversed witness that draws the keypair before the cheap rungs and must fail `CheapRefusalsFirst`. The move card that follows it (nonce `:546`, keypair `:584`) is pro | the follow-up move card names `TestRequestRefusedBeforeEntropy` (a refused request draws nothing); this card runs `./bin/test` green and records the TLC runs with `tlacheck run` | `ReceiveOrder` `AdvanceStep`, `CheapRefusalsFirst`; `ServerConnection` `ServerRecvRequest`, `NonceFresh` |
| PUR-7 | `rn-tla-keep-alive` | M4 | heavy | `tla/RNKeepAlive.tla`, `tla/MCRNKeepAlive.tla`, `tla/MCRNKeepAlive.cfg`, `tla/README-RNKeepAlive.md` (plus `tla/CASES.tsv` and the `tlacheck` records) | none: inventory module 5. Per-slot `sinceKA`, `sinceSend`, `acked`, the cursor and frame exhaustion as a nondeterministic budget per update; `LatchImpliesConnected`, `EstablishmentUnstoppable`, `EverySlotServiced`; reversed witness: the due check reads `last_send_time` before the latch (#90) and must fail `EstablishmentUnstoppable` | none (a model card); `./bin/test` stays green | `RNKeepAlive` (new), its actions `Due(i)`, `Exhaust`, `Ack(i)` |
| PUR-8 | `rn-purity-keep-alive-cursor` | M4 | pro | `sdk/rocketnet_server_transport.cpp`, `sdk/rocketnet_server_protocol.h`, `sdk/rocketnet_server_protocol.cpp`, `test_unit_keep_alive_cursor.cpp` (new) | the cursor (`rocketnet_server_transport.cpp:199`) into `rocketnet_server_protocol_t`, and the index arithmetic of the walk (`:1251-1271`) into `rocketnet_server_protocol_maintenance_begin` / `_next` / `_served`, which the transport loop calls around its frame reserve and commit; the frame calls stay in the transport | `TestMaintenanceCursorResumesAfterExhaustion`: four slots, the frame budget fails at slot 2 on one update, the next update starts at slot 2, and every connected slot is served within two updates | `RNKeepAlive` `Exhaust`, `EverySlotServiced`; `ServerConnection` `ServerKeepAlive(i)` |

PUR-7 comes before PUR-8 because PUR-8 cites it. PUR-6 comes before its move card for
the same reason. PUR-1 to PUR-5 cite actions that exist at `4ea64ce`.

These optional cards answer the write-only instrumentation, if the maintainer wants no
global at all.

| # | Card | Tier | Files | What moves | Unit test | TLA+ cited |
|---|---|---|---|---|---|---|
| PUR-9 | `rn-purity-ack-visit-count-in-endpoint` | pro | `sdk/rocketnet_ack.h`, `sdk/rocketnet_ack.cpp`, `sdk/rocketnet_tests.cpp` | `g_ack_visit_count` (`rocketnet_ack.cpp:7`, `:397`) into a field of `rocketnet_ack_endpoint_t`; the three test reads (`rocketnet_tests.cpp:8474-8548`) read the field | the existing visit-count tests, unchanged in what they assert | `RNSentHistory` (the delivery walk it counts) |
| PUR-10 | `rn-purity-crypto-tally-decision` | heavy | `docs/PURITY.md`, `sdk/rocketnet_crypto_tally.h` | a decision, not a move: either keep the thread_local tallies as the one permitted test global, documented in `rocketnet_crypto_tally.h`, or pass a tally pointer through the protocol structs. The move, if chosen, is split per file into pro cards | none | `ReceiveOrder` `CheapRefusalsFirst` (the property the tallies witness) |

## What this audit did not do

- It changes no code. Every line above was read at `4ea64ce`, and no line was traced by
  running it.
- It does not audit M8 (delivery notification) or M9 (reassembly), which M1, M2 and M4
  are tangled with. Whether those tangles leave M1, M2 and M4 pure-but-tangled or pure
  depends on those audits.
- It does not audit the transports (`rocketnet_client_transport.cpp`,
  `rocketnet_server_transport.cpp`). They are the shell that reads the clocks and does
  the I/O, and that is where the impurities belong.
- No TLC was run for this document.

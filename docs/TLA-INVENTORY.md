# rocketnet state machines: the TLA+ inventory

Every state machine in rocketnet, read from the code at `5cb7613` (origin/main), with
the states it owns, the transitions that move it (each with the `file:line` that makes
it), the rules it must keep (each traced to a sentence in STANDARD.md, USAGE.md or a
comment), and a verdict: model it or not. The last section is the ordered list of
modules the sibling cards write, each with the small instance TLC should check.

Paths are relative to the repository root. `S:` is STANDARD.md, `U:` is USAGE.md,
`sdk/` files are cited by name. Line numbers are at `5cb7613`.

Nothing here is a model. No TLA+ was written or run on this card; every finding in
_Findings from the reading_ was traced by hand through the functions it names.

## How to read a section

- **States**: the values the machine can be in, with the code that names them.
- **Owns**: the variables a module would declare. Only what the code actually holds.
- **Transitions**: one row per operation or outside event. Outside events are written
  _Recv(type)_ (a packet arrives), _Lose_, _Dup_, _Reorder_, _Tick_ (time passes),
  _App(call)_ (the application calls the API).
- **Rules**: candidate `INV` (safety) and `LIVE` (liveness) properties, each traced to
  the sentence that demands it.
- **Verdict**: model it or not, and what the model would catch.

## Findings from the reading

These came out of reading the code against the rules below. Each was traced by hand;
none is a code change on this card. They are listed first because F1, F4, F5 and F6
are exactly what the modules below exist to catch, and the module cards should confirm
each of those with a TLC trace before anyone changes the code. F2 and F3 are gaps a
model cannot see (a missing timer, a lookup's cost) and stand on the reading alone.

| # | Kind | Where | What |
|---|---|---|---|
| F1 | defect | `sdk/rocketnet_replay.cpp:54-60` | A forward jump of 4096 or more sequences clears only `window[0..3]` (256 bits) of a 4096-bit window (64 words). Stale bits survive in words 4-63. |
| F2 | gap against a SHOULD and a standing practice | `sdk/rocketnet_server_protocol.h:49-50`, `sdk/rocketnet_server_transport.cpp:792` | The cookie key is generated once at create and never rotated; `has_previous_cookie_key` is set only by a test (`sdk/rocketnet_tests.cpp:2728-2730`). |
| F3 | gap against a MUST | `sdk/rocketnet_client_slots.cpp:146-156` | The free client slot is found by a linear scan, not a free list. |
| F4 | question: is STOPPED absorbing? | `sdk/rocketnet_server_transport.cpp:945-950` | `update` short-circuits only in STOPPING; called again in STOPPED it runs the full receive, request and keep-alive path. |
| F5 | question: should a replay refresh a slot? | `sdk/rocketnet_server_protocol.cpp:758-783` | A byte-identical connection response, which carries no replay protection, refreshes `last_receive_time` of an already-connected mapping. |
| F6 | question: are failure states terminal? | `sdk/rocketnet_client_protocol.cpp:1332-1333` | `disconnect()` rewrites a negative terminal state (-7..-1) to `disconnected` (0). |
| F7 | observation | `sdk/rocketnet_server_protocol.cpp:1038-1046` | An unestablished mapping is reclaimed only by its handshake deadline; the idle deadline applies only after establishment. |

**F1, traced.** With `ROCKETNET_REPLAY_WINDOW_BITS = ROCKETNET_HISTORY_PACKETS = 4096`
(`sdk/rocketnet_replay.h:26`, `sdk/rocketnet_history.h:8`) the window is 64 words
(`sdk/rocketnet_encryption_map.h:34`).
1. `advance(300)` from `highest_opened = 0`: skip 300 < 4096, so the loop at
   `rocketnet_replay.cpp:63-64` clears 1..299. Bit 300 (word 4) is set. `highest = 300`.
2. `advance(9396)`: skip 9096 ≥ 4096, so only words 0-3 are zeroed
   (`rocketnet_replay.cpp:56-59`). Bit 300 in word 4 is still set. `highest = 9396`.
3. `already_received(8492)`: 8492 > 9396 − 4096 = 5300, so it is inside the window, and
   its ring bit is 8492 mod 4096 = 300, which is still set. The packet is refused as
   `ALREADY_RECEIVED` although it was never received.
4. `rocketnet_ack_block_build` (`sdk/rocketnet_ack.cpp:51-131`) walks the same bits over
   (largest − 4096, largest], so the next block sent names 8492 as received. That
   breaks the sender obligation at S:1733-1743 ("A block MUST NOT name a sequence its
   sender has not opened"). The peer surfaces a false delivery notification, and if 8492
   was a keep-alive the peer's `keep_alive_acked` latch (`sdk/rocketnet_ack.cpp:384-385`)
   is set by a claim the receiver never made, which is the
   stranding direction S:2735-2745 describes.

How it got here: the four-word clear was exact when the window was 256 bits
(`6cfd014`). `ea9b2c0` widened the window to `ROCKETNET_HISTORY_PACKETS` and left the
clear alone. The unit test at `sdk/rocketnet_tests.cpp:2792-2799` laps with sequence 1,
whose bit is in word 0, so it still passes. Reachable whenever 4096 or more consecutive
session sequences go unopened: a loss burst, or sequences burnt by failed fragmented
sends (U:126-160). The refusal half is in the safe direction. The over-claim half is
not.

**F2.** S:1175-1178: "The cookie key is generated at server start and rotated
periodically; a deployment SHOULD accept cookies under the previous key for one rotation
period". S:1994 timing table: "Cookie key rotation | 60 s". CLAUDE.md, verification
ladder: "cookie key rotation runs always at its 60 s period so the previous-key
acceptance window is exercised every minute". The accept side exists
(`rocketnet_server_protocol.cpp:231-236`). Nothing in `sdk/` outside the tests writes
`previous_cookie_key` or `has_previous_cookie_key`.

**F3.** S:1577-1594: "Every lookup a server performs per packet or per connection MUST
be constant time in the number of client slots ... **A free slot is found from a free
list, not a search**". `rocketnet_client_slots_assign` scans `connected[]` from 0. This
runs once per established connection (`rocketnet_server_protocol.cpp:546`).

**F4.** `rocketnet_server_transport_update` returns early only when the state is
STOPPING (`rocketnet_server_transport.cpp:945-950`). If the application calls `update`
once more after observing STOPPED, the portable and GSO drivers (whose stop is
`synchronous_stop`, `sdk/rocketnet_driver_builtin.cpp:17`, leaving the socket open until
destroy) receive, answer requests, create mappings and send keep-alives.
`start_packet` alone checks RUNNING (`rocketnet_server_transport.cpp:1255`). The
canonical shutdown (`sdk/rocketnet.h:734`, "stop and wait for stopped first", then
destroy) never calls `update` after STOPPED, so this matters only if STOPPED is meant
to be absorbing. That is a question for the transport-lifecycle module.

**F5.** The connection response is a handshake type, exempt from replay protection
(S:1466-1470). The server opens it, raises `replay_handshake.highest_opened`, stamps
`mapping->last_receive_time` (`rocketnet_server_protocol.cpp:778-781`) and only then
finds the slot already connected (`:491-496`). S:2873-2876 states this refresh as the
intended effect of the client's own retransmissions. The client retransmits the stored
response byte for byte (`rocketnet_client_protocol.cpp:337-341`), so an on-path
attacker who has captured one copy and can spoof the client's source address can keep
that slot alive after the client is gone. Every other packet that refreshes a slot is
replay-protected. This is a design question. An off-path attacker cannot do it.

**F6.** S:2042-2047: "On any failure it does not detour into recovery: it fails to a
terminal state". `rocketnet_client_protocol_disconnect` moves any non-zero state,
including the negative terminals, to 0. An application that calls `disconnect` before
reading `rocketnet_client_transport_state` loses the reason, and
`state_change_callback` does not fire for that change, because it fires only inside
`update` (`rocketnet_client_transport.cpp:603-604`).

**F7.** S:2502-2509 names two bounds on a mapping, idle and handshake. The sweep checks
only the handshake deadline before establishment. That is the stronger bound at the
shipped values (5 s against a token timeout of tens of seconds,
`rocketnet_server_transport.cpp:794`), so this is recorded rather than raised.

---

## M1. The client connection

**Files.** `sdk/rocketnet_client_protocol.cpp`, `sdk/rocketnet_client_protocol.h`,
`sdk/rocketnet_client_transport.cpp`, states in `sdk/rocketnet.h:398-409`.

**States.** `state ∈ {-7 token expired, -6 invalid token, -5 connection timed out,
-4 response timed out, -3 request timed out, -2 cookie request timed out, -1 denied,
0 disconnected, 1 requesting cookie, 2 sending connection request, 3 sending connection
response, 4 connected}`.

**Owns** (`rocketnet_client_protocol.h:32-117`): `state`, `connection_salt`,
`has_cookie`, `return_cookie` (abstract to a cookie id), `keys_held` (handshake and
session keys committed or zero), `session_nonce`, `ephemeral_secret_live`,
`stored_response`, `client_index`, `send_sequence_handshake`, `send_sequence_session`,
`replay_handshake.highest_opened`, `replay_session` (shared with M7),
`integrity_failures`, `attempt_start_time`, `last_send_time`, `last_receive_time`,
`ack.ack_debt`.

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| any | _App(connect)_: bad flags, create > expire, wrong version | -6 | `rocketnet_client_protocol.cpp:141-149` |
| any | _App(connect)_: expire ≤ now | -7 | `:151-156` |
| any | _App(connect)_: fresh salt, ephemeral, sequences = 1 | 1 | `:123-134, 181-192` |
| 1, 2 | _Tick_: send interval elapsed → send request (with cookie if held) | same | `:297-333` |
| 3 | _Tick_: send interval elapsed → resend stored response byte-identical | 3 | `:335-342` |
| 4 | _Tick_: send interval since last send of anything → keep-alive | 4 | `:344-356`, payloads stamp `last_send_time` at `:1105, 1149, 1243` |
| 4 | _Tick_: `ack_debt ≥ 64` between scheduled sends → standalone ack | 4 | `:299-310` |
| 1..3 | _Tick_: `now − attempt_start ≥ expire − create` | -7 | `:206-212` |
| 1..4 | _Tick_: `timeout > 0` and `now − last_receive ≥ timeout` | -(N+1) | `:214-220` |
| 1, 2 | _Recv(cookie)_, salt matches → store, replace | 2 | `:372-413` |
| other | _Recv(cookie)_ | same, counted WRONG_STATE | `:372-378` |
| 2 | _Recv(challenge)_ opens under temp keys from carried nonce, non-small-order | 3 (keys committed, ephemeral zeroed, response built once) | `:431-531` |
| 2 | _Recv(challenge)_ fails open | 2, `integrity_failures++` | `:476-483` |
| 2, 3 | _Recv(denied)_ opens (held keys in 3, temp keys in 2) | -1 | `:547-614` |
| 3, 4 | _Recv(session type)_: zero seq / replay / open / ack block / size ladder | same | `:642-769` |
| 3 | _Recv(keep-alive)_ accepted | 4, `client_index` set | `:803-808` |
| 3, 4 | _Recv(disconnect)_ accepted | 0 | `:963` |
| any > 0 | `integrity_failures` reaches 2^36 | 0, keys zeroed | `:55-67` |
| 3, 4 | _App(disconnect)_ → 10 sealed disconnects | 0, keys zeroed | `:1309-1336` |
| 1, 2 | _App(disconnect)_ | 0 | `:1332-1336` |
| -7..-1 | _App(disconnect)_ | 0 (F6) | `:1332-1333` |
| 4 | _App(start_payload / start_fragments)_ → sequence spent | 4 | `:1051-1062, 1174-1186` |

**Rules.**
- `INV Forward`: within one attempt the positive states are entered in order 1, 2, 3, 4
  and none is entered twice (S:2042-2045).
- `INV TimeoutPairing`: a timeout from waiting state N lands in -(N+1)
  (S:2074-2083; asserted shape in `:217`).
- `INV TerminalAbsorbing`: no transition leaves -7..-1 except a new `connect` (S:2045-2047,
  under question by F6).
- `INV HandshakeRange`: the total connect deadline applies only while
  `0 < state < 4` (S:2085-2100; `:206`).
- `INV NoSendOnArrival`: no packet is sent by a receive action. Every send is in the
  _Tick_ action, and the standalone ack is the one debt-triggered send (S:1260-1266,
  S:2178-2182; `:290-296`).
- `INV CookieSalt`: a stored cookie always came from a cookie packet carrying this
  attempt's salt (S:1268-1277; `:391-396`).
- `INV ForgedNonceNeverCommits`: keys and `session_nonce` change only after a successful
  open (S:1460-1464; `:462-508`).
- `INV EphemeralErased`: in states 3 and 4 the ephemeral secret is zero
  (S:1089-1092, S:2175-2176; `:509`).
- `INV SequenceMonotone`: within an attempt neither send sequence decreases or repeats.
  Zero is never sent (S:705-709, S:2110-2114).
- `INV SessionOnlyFrom3`: no session type is accepted in states below 3 (S:2199-2201;
  `:670-676`).
- `LIVE Resolves`: under weak fairness on _Tick_, every attempt leaves 1..3 (S:2090-2100).
- `LIVE NoSingleLossStrands`: if the network delivers infinitely often (lossy but fair),
  and the server has a slot and a mapping can be made, the client reaches 4
  (S:2125-2129, S:2225-2257). This is the loss walkthrough as a property.

**Verdict: model it**, together with M2 as one handshake module (see _The module list_
below). It is the machine the request names first, and the standard's own argument for it is
that its invariants exist only because the machine never moves sideways. A model with
a lossy, duplicating, reordering channel and a sender that can replay captured packets
would check: the forward-only climb under arbitrary duplication of cookie, challenge,
denied and keep-alive; that a denied after a challenge opens with held keys and never
re-derives; the loss walkthrough's claim that no single loss strands either end; and
F6.

## M2. The server's per-address handshake: the encryption mapping

**Files.** `sdk/rocketnet_server_protocol.cpp`, `sdk/rocketnet_encryption_map.{h,cpp}`.

**States**, per source address: `None` (no mapping), `HalfOpen` (a mapping with
`established = 0`, `client_index = -1`), `Established` (a mapping with a slot), and,
derived from a request with no mapping, `CookieIssued`. That last one is stateless: it
is a fact about the cookie key and the clock, not stored.

**Owns** (`rocketnet_encryption_map.h:37-61`): per address `mapping ∈ {None, Half,
Est}`, `client_index`, `client_id`, `token_identifier`, `handshake_deadline`,
`last_receive_time`, `send_sequence_handshake`, `stored_challenge` (abstract id),
`session_nonce` (abstract fresh id), `replay_handshake.highest_opened`,
`integrity_failures`. Globally `num_mappings ≤ capacity` (capacity
`2 × max_clients`, `rocketnet_server_transport.cpp:798`).

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| Est | _Recv(request)_ | Est, ignored ALREADY_CONNECTED | `rocketnet_server_protocol.cpp:91-101` |
| Half | _Recv(request)_ | Half, stored challenge resent | `:102-106` |
| None | _Recv(request)_ fails a fixed-offset check (version, protocol id, expire, timeout, flags, advertised, server key) | None, silent | `:121-206` |
| None | _Recv(request)_ zero, bad or expired cookie | None, cookie sent | `:213-275` |
| None | cookie ok, used-token bound elsewhere | None, silent | `:283-288` |
| None | signature fails / seal does not open / sealed fields differ | None, silent | `:292-305, 338-343` |
| None | client id connected / token already admitted | None, silent | `:345-357` |
| None | used-token history records (token, address) | None | `:359` |
| None | small-order client ephemeral | None, silent | `:373-387` |
| None | no free slot | None, denied (sequence 1, fresh nonce) | `:395-413` |
| None | mapping table full | None, silent | `:415-423` |
| None | otherwise | Half, challenge built once and stored, deadline = now + 5 s | `:415-470` |
| Half | _Recv(response)_ opens, padding zero, client id free, slot free, admitted-set add ok | Est, slot assigned, first keep-alive | `:478-576` |
| Half | _Recv(response)_ no slot or admitted set full | Half, denied (handshake seq++) | `:512-544` |
| Est | _Recv(response)_ | Est, ignored, **`last_receive_time` refreshed** (F5) | `:778-783, 491-496` |
| Half | _Recv(session type)_ | Half, replay-marked, NOT_CONNECTED | `:791-801` |
| Est | _Recv(session type)_ accepted | Est, `last_receive_time` refreshed, slot confirmed | `:843-846` |
| Est | _Recv(disconnect)_ accepted | None, slot released | `:938-942` |
| Half, Est | a sealed packet fails to open, 2^36th time | None, slot released if held | `:597-617` |
| Half | _Tick_: `now ≥ handshake_deadline` | None | `:1038-1046` |
| Est | _Tick_: `timeout > 0`, `now ≥ last_receive + timeout` | None, slot released | `:1049-1058` |
| Est | _App(disconnect_client)_ → 10 sealed disconnects | None, slot released | `:1627-1680` |

**Rules.**
- `INV NoSlotBeforeResponse`: a slot is assigned only on an opened response, never on a
  request (S:1168-1173, S:2496-2499).
- `INV MappingCreatedOnce`: a mapping is never replaced. While it lives, a request from
  its address gets the same stored challenge (S:2372-2383; `:102-106`).
- `INV NonceFreshPerMapping`: every mapping created holds a session nonce no earlier
  mapping held. Model the nonce as a counter drawn at creation and check that no
  (salt, nonce) pair recurs (S:1004-1018, S:1113-1121).
- `INV NoMappingWithoutValidToken`: a mapping exists only for an address that
  presented a cookie valid at that time and a token whose signature verified
  (S:2322-2326, S:3409-3413).
- `INV DeadlineNeverRefreshed`: `handshake_deadline` is written once, at creation
  (S:2507-2509, S:2511-2516).
- `INV AlreadyConnectedSilent`: an address holding a slot is never made to emit
  anything by a request (S:2381-2383).
- `INV RequestSmallerThanResponse`: each response is no larger than the packet that
  provoked it (S:2275-2278, S:1160-1166). Sizes are constants, so this is one
  `ASSUME` per response kind.
- `INV Ledger`: `CLIENT_CONNECTED = CLIENT_DISCONNECTED + CLIENT_TIMED_OUT +
  num_connected` (`rocketnet_server_protocol.cpp:589-592`).
- `LIVE HalfOpenBounded`: every Half mapping leaves Half within the handshake deadline,
  whatever the peer sends (S:2511-2516).
- `LIVE AnsweredEveryTime`: every valid request is answered on every arrival
  (S:2477-2489).

**Verdict: model it**, joint with M1 and M3. This is the security half of the
connection: zombie slots, half-open floods, key recurrence across replays and
restarts. The model would catch any ordering of the ladder that lets an address reach a
slot without a round trip, or lets a replayed cookie or request recreate a key. A
`Replay` action for an on-path attacker would show F5 directly.

## M3. The server's client slots

**Files.** `sdk/rocketnet_client_slots.{h,cpp}`, uses in
`sdk/rocketnet_server_protocol.cpp` and `sdk/rocketnet_server_transport.cpp`.

**States**, per slot: `Free`, `Assigned(unconfirmed)`, `Assigned(confirmed)`.

**Owns** (`rocketnet_client_slots.cpp:13-28`): `connected[i]`, `confirmed[i]`,
`client_id[i]`, `mapping_index[i]`, `num_connected`, plus per slot the ack endpoint
latch `keep_alive_acked` (M4) and the reassembly slots (M9), both reset at assign.

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| Free | response accepted (M2) | Assigned(unconfirmed), acks and reassembly reset | `rocketnet_client_slots.cpp:146-168`; `rocketnet_server_protocol.cpp:546-558` |
| Assigned(unconfirmed) | any accepted session packet | Assigned(confirmed) | `rocketnet_server_protocol.cpp:845-846` |
| Assigned | disconnect / timeout / integrity / disconnect_client | Free, identity zeroed, mapping removed and zeroed | `rocketnet_client_slots.cpp:236-263`; `rocketnet_encryption_map.cpp:172-207` |

**Rules.**
- `INV SlotMappingBijection`: `connected[i] ⇒ mapping_index[i]` is live and that
  mapping's `client_index = i`; and every Established mapping is named by exactly one
  slot (S:2642-2648). Indices are reused by the map's free list
  (`rocketnet_encryption_map.cpp:203-204`) and by the slot scan, so the model should
  let a removed index be reused at once, to catch a dangling reference. The code
  already counts `SLOT_WITHOUT_MAPPING` for this (`rocketnet_server_protocol.cpp:1068-1087`).
- `INV OneSlotPerClientId`: no two connected slots hold one client id (S:973-977;
  checked at `:345, 498`).
- `INV ConnectedCount`: `num_connected = |{i : connected[i]}| ≤ max_clients`.
- `INV ReleaseZeroes`: a released slot's mapping keys are zero (S:2925-2930;
  `sodium_memzero` at `rocketnet_encryption_map.cpp:199`). In the model a released
  mapping leaves the domain.
- Constant-time free slot (S:1593-1594), which a TLA model cannot judge: F3.

**Verdict: model it, inside the handshake module** (M1+M2+M3). On its own it is a small
allocator. Its risk is the slot↔mapping binding under index reuse, which only shows
up in combination with M2's removal paths.

## M4. Keep-alive generation and the acknowledgment latch

**Files.** `sdk/rocketnet_server_protocol.cpp:1161-1319`, `sdk/rocketnet_ack.cpp`
(`:267-278` tags a sent keep-alive, `:384-385` sets the latch),
`sdk/rocketnet_server_transport.cpp:1150-1172`.

**States**, per slot: `Unacked` (no keep-alive acknowledged; paced off the keep-alive
clock) and `Acked` (paced off the last send of anything). On the client: whether it
holds its index (state 3 versus 4).

**Owns**: `last_keep_alive_send_time`, `last_send_time` (`rocketnet_encryption_map.h:50-51`),
`keep_alive_acked`, per sent record `was_keep_alive`, the client's `replay_session`
(which the ack blocks are built from), and on the client `state` and `client_index`.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| response accepted | first keep-alive sent, both clocks stamped | `rocketnet_server_protocol.cpp:552, 567` |
| _Tick_, Unacked, `now − last_keep_alive ≥ 1/rate` | keep-alive sent and tagged | `:1309-1318, 1198, 1206` |
| _Tick_, Acked, `now − last_send ≥ 1/rate` | keep-alive sent | `:1313-1318` |
| any session send to the slot (payload, fragment, ack, keep-alive, disconnect) | `last_send_time` stamped | `:1157, 1402`, fragments `:1486-1626` |
| _Tick_, `ack_debt ≥ 64` and no keep-alive due | standalone ack | `:1213-1238`; transport order `rocketnet_server_transport.cpp:1164-1166` |
| _Recv_ a block naming a sent keep-alive sequence | `keep_alive_acked := 1` (never cleared until slot reset) | `rocketnet_ack.cpp:384-385` |
| client _Recv(keep-alive)_ in 3 | client 3 → 4 | `rocketnet_client_protocol.cpp:803-808` |

**Rules.**
- `INV CorrectClientNeverLatchesEarly`: among clients in state 3, `keep_alive_acked`
  is set only if the client's block names a keep-alive sequence it opened, and an
  opened keep-alive always reaches the 3 → 4 transition (S:2820-2845). With a correct
  client this gives: `keep_alive_acked ⇒ client.state = 4`, modulo packets in flight.
  F1 breaks the premise, which is why M7 comes first.
- `LIVE EstablishmentUnstoppable`: under weak fairness on _Tick_ and a fair lossy
  channel, a client in state 3 whose slot is assigned reaches 4 whatever the server's
  application sends, including a continuous payload stream (S:2847-2864, #90).
- `INV OverClaimStrandsOnlyItself`: an over-claiming client changes only its own slot's
  pacing (S:2747-2750). This is structural: one latch per slot.

**Verdict: model it**, as its own module with time. The standard spends three pages on
why this rule is the right one, and the one regression it records (#90) was a
liveness bug: a payload stream starving establishment. That is the class TLC finds and
tests do not. Time can be abstracted to a send-interval tick plus an update tick.

## M5. The cookie gate and cookie key rotation

**Files.** `sdk/rocketnet_server_protocol.cpp:208-275`, `sdk/rocketnet_server_protocol.h:48-51`,
`sdk/rocketnet_client_protocol.cpp:363-416`.

**States.** Server: `cookie_key` generation `g`, `has_previous_cookie_key`. Client:
`has_cookie`, stored cookie `(gen, expire)`.

**Owns.** `key_gen`, `prev_valid`, the client's stored cookie, the cookie lifetime,
the clock.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| server create | `key_gen := 0`, no previous | `rocketnet_server_transport.cpp:792` |
| rotation every 60 s | `prev := key; key := fresh` | **absent** (F2) |
| _Recv(request)_ with zero / unverifiable / expired cookie | fresh cookie under the current key, lifetime 3 s | `rocketnet_server_protocol.cpp:239-274` |
| _Recv(request)_, MAC under current or previous key, unexpired | pass | `:223-257` |
| client _Recv(cookie)_ in 1 or 2 with matching salt | store, replacing any stored cookie | `rocketnet_client_protocol.cpp:402-413` |

**Rules.**
- `INV NoAsymmetricWithoutCookie`: signature verify, seal open and scalar mult happen
  only after a cookie passed (S:2322-2326, S:2402-2410).
- `LIVE CookieRaceResolves`: with lifetime ≥ 2 send intervals, a client that keeps
  retransmitting gets past the gate even when its stored cookie expires
  (S:2017-2022, S:2149-2153).
- `INV RotationWindow`: a cookie verifies only under the key generation that issued it
  or the one after (S:1175-1178).

**Verdict: model it, small**, as part of the security module with M6. Without rotation
the module has one key and is trivial. F2 is the reason to write it: the rotation it
specifies is the code the sibling card should add, and the model should be the spec it
is written against. On its own, without rotation, it is not worth a module.

## M6. The token tables: used-token history and admitted-token set

**Files.** `sdk/rocketnet_token_tables.{h,cpp}`, uses at
`sdk/rocketnet_server_protocol.cpp:283, 352, 359, 521`.

**States**, per token id: in the history `Absent | Bound(addr, expire)`; in the
admitted set `Absent | Admitted(expire)`.

**Owns.** `history: id ↦ (addr, expire, order)` with capacity `8 × max_clients`;
`admitted: id ↦ expire` with capacity `2 × max_clients`
(`rocketnet_server_transport.cpp:799-800`); the unix clock.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| request past the cookie: check | refuse if bound to another address and unexpired | `rocketnet_token_tables.cpp:221-231` |
| request past the seal: record | insert unless present; free slot, else expired slot, else **evict oldest** | `:233-248, 160-188` |
| response accepted: admit | insert unless present; free slot, else expired slot, else **refuse** (fail closed) | `:270-290` |
| request past the seal: contains | refuse if admitted and unexpired | `:262-268` |
| _Tick_ (clock passes expire) | entry reads as absent (`find` treats expired as absent) | `:122-133` |

**Rules.**
- `INV OneConnectionPerToken`: a token id admits at most one connection while
  unexpired (S:2431-2436, S:2572-2577).
- `INV AdmittedNeverEvictedUnexpired`: the admitted set never loses an unexpired entry
  (S:2575; `select_slot(..., 0)` at `:283`).
- `INV DeniedTokenStaysUsable`: a response denied for no slots does not consume the
  token (S:2602-2607; ordering at `rocketnet_server_protocol.cpp:512-526`).
- `INV BindingFirstAddress`: while a history entry lives, only its first address passes
  `check` (S:2562-2565).
- Eviction is documented as security-relevant (S:2566-2570). The model should show the
  flush-then-replay trace as reachable when capacity is small. That is the expected
  counterexample, documenting the bound rather than a defect.

**Verdict: model it, small**, with M5 as one security-tables module. The interesting
behaviour is the interplay between the two tables and the slot table across a
denied-then-retried response, an expired-then-reused token, and a full set. Three ids,
capacity 2 each, three clock values is enough.

## M7. Sequence reconstruction and the replay window

**Files.** `sdk/rocketnet_replay.{h,cpp}`, `rocketnet_reconstruct_sequence` /
`rocketnet_reconstruct_near` in `sdk/rocketnet_protocol.cpp`, the bit reader in
`sdk/rocketnet_ack.cpp:44-49`.

**States**, per (key phase, direction): `highest_opened`, the ring `window` of
`W = ROCKETNET_REPLAY_WINDOW_BITS` bits stored as `W/64` words.

**Owns.** `highest`, `bits: 0..W-1 → BOOLEAN` (the representation, not a set), and for
the specification a ghost `opened: SUBSET Nat`.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| reconstruct(wire) | nearest 64-bit candidate to `highest + 1`, ties to the larger | S:717-725 |
| `already_received(s)` | `s > highest → no`; `s ≤ highest − W → yes`; else bit | `rocketnet_replay.cpp:27-40` |
| `advance(s)`, `s > highest`, skip < W | clear bits `highest+1 .. s-1`, set `s` | `:48-69` |
| `advance(s)`, `s > highest`, skip ≥ W | clear words 0..3 only (F1), set `s` | `:54-60` |
| `advance(s)`, `s ≤ highest` | set bit `s` | `:69` |
| block build | walk bits over (highest − W, highest] | `rocketnet_ack.cpp:51-131` |

**Rules.**
- `INV NeverAcceptsReplay`: `already_received(s) = FALSE ⇒ s ∉ opened`
  (S:1521-1525, S:1561-1562).
- `INV InWindowExact`: for `highest − W < s ≤ highest`,
  `already_received(s) ⇔ s ∈ opened`. The window may refuse old packets but never a
  fresh in-window one (S:1559-1562). F1 violates this.
- `INV BlockTruthful`: every sequence a built block names is in `opened`
  (S:1733-1743). F1 violates this.
- `INV MarkAndAdvance`: after an accepted session packet, its bit is set and `highest`
  is at least it (S:1480-1483).
- `INV ZeroNeverValid`: sequence 0 is never accepted (S:705-709).

**Verdict: model it, first.** It is the lowest layer under every session packet, the
ack blocks and the keep-alive latch, and it holds the one confirmed defect. The model
must be written at the representation level: bits grouped in words, and the large-skip
branch clearing a constant `ClearWords` words, as the code does. An abstract set model
would prove the spec and miss F1. The small instance shrinks the word, not the shape:
W = 8 bits as 4 words of 2 bits. `ClearWords = 1` reproduces the code (it clears fewer
words than the window has: 4 of 64) and must fail `InWindowExact`. The reversed
witness `ClearWords = 4` clears the whole window and must pass.

## M8. Delivery notification: sender bookkeeping and the ack block

**Files.** `sdk/rocketnet_ack.{h,cpp}`, recording at every session send
(`rocketnet_client_protocol.cpp:265, 1099, 1145, 1237`;
`rocketnet_server_protocol.cpp:1150, 1395, 1447`), processing at
`rocketnet_client_protocol.cpp:756-764` and `rocketnet_server_protocol.cpp:848-864`.

**States**, per sent record (ring of `ROCKETNET_SENT_PACKET_BUFFER_SIZE = 4096`):
`Empty | Sent(seq, packet_id?, aggregate?) | Acked | Overwritten`. Per aggregate (ring
of 2048): `remaining`. Per endpoint: `ack_debt`, `keep_alive_acked`,
`highest_sequence`.

**Owns.** `sent[seq mod N]`, `aggregates[id mod N/2]`, `highest_sequence`, `ack_debt`,
`keep_alive_acked`, the per-update acked batch and its capacity.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| send session packet | record; overwriting an unacked record returns false → `SEND_HISTORY_OVERFLOW` | `rocketnet_ack.cpp:228-265` |
| fragmented send | aggregate opened with `remaining = count` | `rocketnet_ack.cpp:280-295`; `rocketnet_server_transport.cpp:1571` |
| _Recv_ block | each named, retained, unacked record → Acked; payload id surfaced; aggregate decremented, id surfaced at 0 | `rocketnet_ack.cpp:318-425, 427-465` |
| batch full | record stays unacked (deferred), `ACKED_BATCH_FULL` | `rocketnet_ack.cpp:318-425` |
| named but not sent | `ACK_UNKNOWN_SEQUENCE`, no effect | `rocketnet_ack.cpp:318-425` |
| any eliciting packet (4, 5, 6, 9) accepted | `ack_debt++`; type 8 does not | `rocketnet_client_protocol.cpp:763-764`; `rocketnet_server_protocol.cpp:862-863` |
| any block sent | `ack_debt := 0` | `rocketnet_client_protocol.cpp:269, 1103, 1150, 1241`; server `:1154, 1399, 1451` |

**Rules.**
- `INV SurfacedOnce`: each packet id surfaces at most once (U:276-282, S:1831-1834).
- `INV SurfacedOnlyIfDelivered`: a surfaced id was opened by the peer, given truthful
  blocks. Under F1 this fails, so the module should take the peer's received set from
  M7.
- `INV AggregateAllFragments`: a fragmented payload's id surfaces only when every
  fragment record is acked (U:117-120).
- `INV WalkBounded`: processing visits at most the retained window, whatever the block
  claims (S:1835-1845). This is a cost bound and stays a test, not a TLC property.
- `LIVE AckConversationTerminates`: two idle peers stop exchanging type 8
  (S:1800-1809). The model is two endpoints, each with `ack_debt`, where type 8 does not
  elicit.

**Verdict: model it**, after M7 and with M7's window as its input. It checks the
deferral path (a batch-full deferral must still surface later), aggregate counting
under reordered and partial acks, and the termination of the ack conversation. Small:
ring 4, aggregate ring 2, batch capacity 1.

## M9. Fragment reassembly

**Files.** `sdk/rocketnet_fragment.cpp:189-449`; the send split in
`rocketnet_client_protocol.cpp:1162-1291` and `rocketnet_server_protocol.cpp` (fragments).

**States**, per reassembly slot (`payload_id mod num_slots`): `Empty`,
`Collecting(id, count, stride ∈ {unknown, k}, final_staged, received bits)`, then
`Delivered`, which clears straight back to `Empty`.

**Owns.** `payload_id`, `fragment_count`, `fragment_stride`, `final_bytes`,
`final_staged`, `received` bits, `num_received`; counters `abandoned`, `reassembled`,
`stale`.

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| any | id = 0 | same, `ZERO_PAYLOAD_ID` | `rocketnet_fragment.cpp:223-228` |
| Collecting(o) | id older than o (serial arithmetic) | same, `STALE` | `:242-247` |
| any | count = 1 / index ≥ count | same, refused | `:251-264` |
| Collecting(id) | count differs | same, `COUNT_MISMATCH` | `:273-278` |
| Collecting(id) | non-final ≠ stride / final > stride | same, `SIZE_MISMATCH` | `:287-300` |
| Collecting(id), final staged | first non-final with stride < final | Empty, `SIZE_MISMATCH` | `:304-310` |
| any | reconstructed size > max | Empty if same id, else unchanged; `PAYLOAD_TOO_LARGE` | `:321-336` |
| Collecting(id) | index already held | same, `DUPLICATE_INDEX` | `:341-346` |
| Collecting(o), newer id | Collecting(id), `ABANDONED` | `:353-363` |
| Empty | first fragment | Collecting(id) | `:353-363` |
| Collecting | final first | staged in the slot's buffer | `:381-386` |
| Collecting | first non-final | stride fixed, staged final placed | `:365-376` |
| Collecting | last missing fragment | Delivered → Empty | `:393-422` |
| any | slot assigned / connect | all slots Empty | `rocketnet_server_protocol.cpp:557-558`; `rocketnet_client_protocol.cpp:128-129` |

**Rules.**
- `INV RefusalLeavesSlot`: every refusal except the two whole-payload refusals leaves
  the slot unchanged (S:1919-1923; comment at `rocketnet_fragment.cpp:216-221`).
- `INV DeliveredExact`: a delivered payload is the concatenation of exactly the
  fragments of one id, each once, at `index × stride` (S:1924-1934).
- `INV NoAliasing`: bytes of an older id never land in a newer id's payload
  (S:1906-1915).
- `INV CountPinned`: once a slot holds an id, its count never changes (S:1919-1923).
- `LIVE SlotRecovers`: after any id, including one near 2^32, later ids eventually
  occupy the slot (S:1910-1915).

**Verdict: model it**, small. The slot's state space is the classic place for
reassembly memory bugs: a final arriving first, a stride fixed late, a count widened.
The code is careful and its comments carry the argument. A model with ids modulo a
small wrap (4 bits), 2 slots, counts 2..3 and reordering would make the argument
checked. The arena bounds stay tests.

## M10. The fill phase, packet leases and sequence burns

**Files.** `sdk/rocketnet_packet_lease.h`, `sdk/rocketnet_server_transport.cpp:1248-1750`,
`sdk/rocketnet_client_transport.cpp:634-930`, USAGE _The transport cycle_ and _Sending_.

**States.** Transport phase: `Update | Fill | Flush` (a contract, not a variable). Per
lease entry: `state = (epoch << 2) | {0 free, 1 writing, 2 active, 3 consumed}`.
Per staged frame: `Reserved | Committed | Aborted`. Per send sequence: `Unallocated |
Spent | Sent`.

**Owns.** `epoch`, `entries[k].state`, `used`; `send_sequence_session`; per in-flight
packet `{seq, frame, consumed}`; the arena's slot use.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| `update` / `send_packets` (main thread, joined) | `epoch++`, `used := 0`; every outstanding context invalid | `rocketnet_packet_lease.h:96-100`; `rocketnet_server_transport.cpp:939, 1669` |
| `start_packet` (any fill thread) | sequence spent (atomic add), then frame acquired, then lease inserted by CAS on a stale-epoch entry | `rocketnet_server_protocol.cpp:1350`; `rocketnet_packet_lease.h:35-61` |
| `finish_packet` / `abort_packet` | `take`: CAS active → consumed, refused if fields differ or epoch is stale | `rocketnet_packet_lease.h:63-94` |
| arena finish, fragmented | reserve `count − 1` sequences; a seal failure withdraws the whole block (portable, GSO) or the rest (XDP) | `rocketnet_server_transport.cpp:1499-1650` (`:1562, 1584`); client `rocketnet_client_transport.cpp:760-890` |

**Rules.**
- `INV ConsumeOnce`: a context is consumed at most once; a copy, a changed field, or a
  context from an earlier phase is refused (U:293-296).
- `INV ResolveFindsOwn`: probe chains never skip a live entry of the current epoch
  (linear probing over entries claimed, never released, within an epoch).
- `INV SequenceNeverReused`: two staged packets on one connection never carry one
  sequence (S:698-703). Burns are allowed (U:126-160).
- `INV NoLeaseAfterReset`: after `reset`, no entry of an older epoch resolves.

**Verdict: model it, but after the protocol modules.** The lease table is a small
lock-free protocol (CAS, a spin on `writing`, epoch reuse of entries) run by any number
of fill threads. That is the shape where interleavings hide. Model 2 fill threads,
capacity 4, 2 epochs. The sequence-burn arithmetic is accounting rather than a
machine; keep it in tests.

## M11. Transport lifecycle: RUNNING, STOPPING, STOPPED

**Files.** `sdk/rocketnet.h:652-654`, `sdk/rocketnet_server_transport.cpp:774, 866-870,
936-950`, `sdk/rocketnet_driver.cpp:133-136`, `sdk/rocketnet_driver_builtin.cpp:17, 85-98,
117-121`, `sdk/rocketnet_server_socket_linux.cpp:1672-1691`.

**States.** Transport `RUNNING → STOPPING → STOPPED`; XDP socket
`RUNNING → STOPPING → STOPPED`; IOCP `stopping` flag with `pending` and
`active_workers`.

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| — | create succeeds | RUNNING | `rocketnet_server_transport.cpp:774` |
| any | _App(stop)_ | STOPPING | `:866-870` |
| STOPPING | _App(update)_, driver stop granted | STOPPED | `:945-950` |
| STOPPING | _App(update)_, driver stop not yet | STOPPING | `:947-948` |
| STOPPED | _App(update)_ | **full update runs** (F4) | `:936-1186` |
| XDP STOPPING | socket update | STOPPED (immediately, no drain) | `rocketnet_server_socket_linux.cpp:1676-1679` |
| any | destroy | threads quit, joined | `rocketnet_server_socket_linux.cpp:1635-1647`; `rocketnet_server_iocp_windows.cpp:268-283` |

**Rules.**
- `INV NoSendAfterStopped`: nothing is staged or sent after STOPPED (implied by
  `sdk/rocketnet.h:734`; not stated in the standard, hence F4 is a question).
- `INV StopIdempotent`: `stop` twice is `stop` once.
- `LIVE StopCompletes`: repeated `update` after `stop` reaches STOPPED under fair driver
  progress.

**Verdict: model it, small, together with M14 (IOCP).** On its own it is three states
and F4. Its value is as the frame that M12-M14 run inside: they each have a "stop
while a worker holds something" edge.

## M12. XDP and GSO receive flip, and the client receive thread

**Files.** `sdk/rocketnet_server_socket_linux.cpp:2057-2097` (game side),
`:2131-2230` (worker); `sdk/rocketnet_server_gso_linux.cpp:1092-1145` (game side),
`:427-560` (worker); `sdk/rocketnet_client_transport.cpp:466-478, 1099-1148`.
CLAUDE.md, _The scaling story_: "The buffer handoff is correct: the XDP thread holds
`receive_mutex` across its whole fill loop, so the game's counter flip lands only
between batches and the game then drains the other buffer lock-free".

**States.** `receive_counter` (its parity names the buffer the worker fills); per
buffer `num_packets`; mutex holder ∈ {none, worker, game}.

**Transitions.** Game: lock, `counter++`, unlock, drain buffer `(counter + 1) mod 2`
without the lock, set its count to 0. Worker: lock, read `counter mod 2`, append up to
capacity, unlock.

**Rules.**
- `INV Exclusive`: the game never reads a buffer the worker is writing, and the worker
  never writes the buffer the game is draining. Packet conservation: every packet the
  worker appends is drained exactly once or counted dropped.

**Verdict: model it, one small module for all three copies.** The pattern is
duplicated in three drivers (XDP, GSO, client) and the claim of correctness is
prose. A two-process model with 2 packets is cheap and makes the claim checked once
for the pattern. Note the client's variant swaps with `receive_buffer_index`
(`rocketnet_client_transport.cpp:466-478`) and drains the swapped-to index, the mirror
image; the module should be parameterised on which side it drains.

## M13. XDP UMEM frame ownership and the send handoff

**Files.** `sdk/rocketnet_server_socket_linux.cpp:361-397` (free stacks),
`:1488-1540` (setup), `:2099-2373` (worker), `:1870-1897` (send acquire and publish);
`sdk/rocketnet_pending_frames.h`; `sdk/rocketnet_send_handoff.h`. CLAUDE.md _Known
issues_ #1: "the pending-return spill ... has not been driven on hardware".

**States.** Each receive frame ∈ {Fill (kernel), Rx (kernel → user ring), Pending
(user, awaiting fill reservation)}. Each send frame ∈ {Free, Batched (user),
Tx (kernel), Completed}. Send generations (`rocketnet_send_handoff_t`) ∈ {free,
filling, queued}, with `head`, `count`, `filling`.

**Transitions.** Worker: `peek ≤ pending.available()`, consume ≤ staging room, release
consumed, append to pending, `flush` all-or-zero into the fill ring
(`rocketnet_pending_frames.h:19-33`); complete-ring frames → free stack; front
generation → batch (frames allocated; on exhaustion stop and retain); reserve all-or-zero
on the TX ring; retire a generation when every packet is copied. Main thread:
`send_packets` publishes the filling generation and acquires the next
(`rocketnet_server_socket_linux.cpp:1883-1897`).

**Rules.**
- `INV FrameConservation`: every UMEM frame is in exactly one owner at every step
  (CLAUDE.md _Known issues_ #1, comment at `rocketnet_server_socket_linux.cpp:2159-2167`).
- `INV ReleaseConsumedOnly`: the RX release covers exactly the descriptors consumed
  (same comment).
- `INV HandoffNoOverwrite`: a full queue refuses new storage without overwriting a
  queued generation (`rocketnet_send_handoff.h:5-8`).
- `INV RetireAfterCopy`: a generation is retired only after its last packet is copied
  into UMEM (`rocketnet_server_socket_linux.cpp:2318-2322`).
- `LIVE FramesReturn`: if the kernel drains the fill and TX rings fairly, every Pending
  frame returns to Fill and every Tx frame returns to Free.

**Verdict: model it.** This is the one item CLAUDE.md ranks first among known open
issues, it cannot be driven without hardware, and conservation across four rings is
exactly what a model checks without hardware. Small: 4 receive frames, fill ring 2, RX
ring 2, pending capacity 2, 3 send frames, 2 generations.

## M14. IOCP shutdown

**Files.** `sdk/rocketnet_server_iocp_windows.cpp:137-283`.

**States.** `stopping ∈ BOOLEAN`, `pending ∈ Nat` (posted operations not yet completed),
`active_workers`, per operation `Posted | Completing | Idle | Retry`, per worker mutex
holder.

**Transitions.** `post_receive`: if not stopping, `pending++`, post (`:137-158`).
Worker: at loop top exit if `stopping ∧ pending = 0` (`:166-167`); dequeue; under its
mutex, `pending--` then repost (`:185-220`); drain the retry list (`:224-232`).
`stop`: `stopping := TRUE`, pass through every worker mutex, `CancelIoEx`, post one wake
per worker (`:237-258`). `stopped`: `stopping ∧ pending = 0 ∧ active_workers = 0`
(`:260-265`).

**Rules.**
- `INV NoPostAfterCancel`: no operation is posted after `CancelIoEx` returns (comment
  `:139-141`).
- `INV DestroyQuiescent`: at destroy, `pending = 0` (assert `:283`).
- `LIVE StopDrains`: after `stop`, eventually `stopped`, provided each posted operation
  completes (a cancel guarantees completion).
- The window between a worker's `pending--` and its repost, where another worker can
  see `pending = 0` and leave, is the case to check. By hand it is safe: the reposting
  worker stays because it sees `pending ≥ 1` at its next loop top. A model makes that
  checked.

**Verdict: model it, small.** Two workers, two receive operations, one send. This is a
textbook shutdown protocol over an atomic counter and a mutex barrier, written in
comments rather than proven.

## M15. GSO tier degradation

**Files.** `sdk/rocketnet_server_gso_linux.cpp:30-40, 1036-1048`.

**States.** `gso_enabled ∈ {on, off}`, monotone on → off.

**Transitions.** A live GSO refusal (EINVAL, EIO, EOPNOTSUPP on a GSO message) flips
the tier off and resumes plain from the first unsent slot (`:1040-1047`). EAGAIN and
other errors drop the rest of that flush, counted (`:1051-1059`).

**Rules.** `INV LosslessDegrade`: a flip loses no packet (`:36-38`). Tier monotone.

**Verdict: do not model.** Two states, one edge, and the resume index is arithmetic. A
unit test that forces a refusal mid-batch and counts packets is the right instrument.

## M16. Frame sync

**Files.** `sdk/rocketnet_frame_sync.{h,cpp}`.

**States.** `first_wait_done`, `spinning` (fixed at the first wait), `deadline_ns`,
`guard_ewma_ns`.

**Transitions.** First wait anchors the deadline and takes priority
(`rocketnet_frame_sync.cpp:338-354`); each wait sleeps, maybe spins, then advances
(`:355-385`, `rocketnet_frame_sync_advance` at `:250-268`).

**Rules.** Deadline strictly increases; drops counted; guard ≤ cap.

**Verdict: do not model.** A two-state anchor and arithmetic on clocks, with no
concurrency and no protocol. Its pure parts are already factored out for direct tests
(`rocketnet_frame_sync.h`, "The pure parts, tested directly").

## M17. Items considered and set aside

| Item | Where | Why no module |
|---|---|---|
| Driver ladder selection | `sdk/rocketnet_driver.cpp`, S:82-160 | A pure function from a list to a grant, no state that persists past create. Table tests. |
| Config precedence | `sdk/rocketnet_config.cpp`, S:142-160 | Pure function, already walked by a completeness test. |
| Library init / term | `sdk/rocketnet.cpp`, U:1-8 | Three states (uninit, init, terminated) and two refusals. A unit test covers it. |
| Env `public_address` consumed once | `rocketnet_server_transport.cpp:827-832` | One process-wide flag set by CAS. |
| Keyed prefilter epochs | S:478-546 | Specified and not implemented: 1.0 is epoch 0 only (S:411-415). Model it when it is built. Generation window and wrap 255 → 1 are the properties. |
| Backend connected set | S:946-981 | A deployment property outside the library. |
| Darwin `msgx` live-refusal pause | not in this card's START list | Not read on this card. It should get its own line in the next inventory. |
| Stats windows, loss and jitter trackers | `rocketnet_stats_window.h`, `rocketnet_packet_loss_tracker.h`, `rocketnet_jitter_tracker.h` | Measurement, not control: nothing reads them to decide a transition. |

## The module list, in order

Ordered bottom-up, per the standing practice: each module stands on the ones above it,
so a sibling card can check its layer with the layer below already secured. Every
module goes in `tla/`, with its `MC<Module>.tla` and `MC<Module>.cfg` beside it, every
TLC run recorded in `tla/RUNS.tsv`, and each module cited from the code it models.

Common shape for the network: a channel as a bag of packets with actions `Send`, `Lose`
(remove any), `Dup` (copy any), `Deliver` (any, so order is free), and, where the
module concerns security, `Inject` (an attacker that can replay any packet it has
seen and forge only packets without a key). Time is a `Tick` action over small
integer clocks. Liveness is checked with weak fairness on `Tick` and on delivery of
retransmitted packets, never on a single packet.

| # | Module | Covers | Small instance | Must find first |
|---|---|---|---|---|
| 1 | `ReplayWindow` | M7 | `W = 8` bits as 4 words of 2 bits, `MaxSeq = 24`, sequences arrive in any order and with any skips; `ClearWords = 1` (the code's ratio) and the reversed witness `ClearWords = 4` | F1 under `InWindowExact` and `BlockTruthful`; the witness must pass |
| 2 | `SecurityTables` | M5 + M6 | 3 token ids, 2 addresses, history capacity 2, admitted capacity 2, clock 0..4, cookie lifetime 1, rotation period 2 | the flush-then-replay trace (expected, documents the bound); `OneConnectionPerToken`; rotation spec for F2 |
| 3 | `Handshake` | M1 + M2 + M3 | 2 clients (one token each, plus one shared token for the client-id rule), 1 server, `max_clients = 1`, mapping capacity 2, send interval 1, handshake deadline 3, timeout 4, token lifetime 6, lossy/dup/reorder channel with a bound of 3 in flight per direction, an on-path `Replay` | forward-only climb, no slot without response, nonce freshness across replays, denied paths, F5 (replayed response refresh), F6 if `TerminalAbsorbing` is asserted |
| 4 | `KeepAlive` | M4 (on top of 3) | 1 client, 1 slot, send interval 2 ticks, update tick 1, application stream that sends every tick, ack blocks built from M7's abstract `opened` | `EstablishmentUnstoppable` with a continuous payload stream (the #90 trace when the pacing reads `last_send_time` before the latch: the reversed witness) |
| 5 | `AckDelivery` | M8 | 2 endpoints, sent ring 4, aggregate ring 2, acked batch capacity 1, one fragmented payload of 2 fragments | `SurfacedOnce`, `AggregateAllFragments`, deferral surfaces later, `AckConversationTerminates` |
| 6 | `Reassembly` | M9 | payload ids mod 16, 2 slots, counts 2..3, stride 2, max payload 5, reordered and duplicated fragments, one defective sender | `RefusalLeavesSlot`, `NoAliasing`, `SlotRecovers` across the id wrap |
| 7 | `TransportLifecycle` | M11 + M14 | 2 IOCP workers, 2 receive operations, 1 send; transport states; `update` callable in any state | `NoPostAfterCancel`, `StopDrains`, and F4 if `NoSendAfterStopped` is asserted |
| 8 | `FillPhaseLeases` | M10 | 2 fill threads, lease capacity 4, 2 epochs, 3 contexts including a copied one | `ConsumeOnce`, `NoLeaseAfterReset` |
| 9 | `XdpFrames` | M13 | 4 receive frames, fill ring 2, RX ring 2, pending capacity 2, 3 send frames, TX ring 2, 2 send generations, kernel actions nondeterministic | `FrameConservation`, `HandoffNoOverwrite`, `FramesReturn` |
| 10 | `ReceiveFlip` | M12 | 2 buffers of capacity 2, 3 packets, game and worker processes, parameterised for the client's mirror | `Exclusive` and conservation |

Modules 1 to 4 are what the request names: client and server connection and security.
Module 1 goes first because modules 3 to 5 read its received state and F1 lives in it.
Module 3 is the largest state space; if TLC exceeds a bench budget at the instance
above, cut to one client before cutting the channel's freedom. Losing duplication or
reordering hides exactly the traces the standard argues about.

M15 and M16 get no module; the reasons are in their sections.

## What this inventory did not do

- No module was written and TLC was not run. `tla/RUNS.tsv` does not exist yet; the
  first sibling card creates it.
- The Darwin `msgx` driver was outside the START list and was not read.
- F1 to F7 are recorded here and in the report. None is fixed on this card. F1 is a
  defect. F2 and F3 are gaps against the standard. F4 to F6 are design questions for the maintainer.
  F7 is an observation.

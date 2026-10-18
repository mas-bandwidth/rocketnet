# rocketnet state machines: the TLA+ inventory

Every state machine in rocketnet, read from the code at `5cb7613` (origin/main). Each
section gives the states, the state the machine owns (the variables a module would
declare), the transitions that move it (each with the `file:line` that makes it), the
rules it must keep (each traced to a sentence in STANDARD.md, USAGE.md or a code
comment), and a verdict: model it or not, and why. The last part is the ordered list of
modules the sibling cards write, with the small instance each one checks.

Conventions. Paths are relative to the repository root. `S:n` is STANDARD.md line n,
`U:n` is USAGE.md line n. All line numbers are at `5cb7613`. Outside events are written
_Recv(type)_ (a packet arrives), _Lose_, _Dup_, _Reorder_, _Tick_ (time passes) and
_App(call)_ (the application calls the API). Properties are `INV` (safety) or `LIVE`
(liveness).

This document is an inventory. No TLA+ was written or run for it. Every finding below
was traced by hand through the functions it names; F1 was also replayed through a
line-for-line transcription of the two functions it involves (see F1).

---

## Findings from the reading

The modules below exist to catch these, so they come first. Each module card should
reproduce its finding as a TLC counterexample before anyone changes code. A finding is
not a code change on this card.

| # | Kind | Where | One line |
|---|---|---|---|
| F1 | **defect** | `sdk/rocketnet_replay.cpp:54-60` | A forward jump of ≥ 4096 sequences clears 4 of the window's 64 words. The stale bits make the receiver refuse genuine packets and make its next ack block claim thousands of sequences it never opened. |
| F2 | closed (gap, SHOULD, and CLAUDE.md:179) | `sdk/rocketnet_server_transport.cpp:832-838`, `sdk/rocketnet_server_protocol.cpp:377, 1198-1227` | Closed: the cookie key rotates on its 60 s period (`ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS`) via `rocketnet_server_protocol_rotate_cookie_key_if_due` in `rocketnet_server_protocol_update`, keeping the previous key for one period while `current_time < previous_cookie_key_expire_time`. |
| F3 | gap (MUST) | `sdk/rocketnet_client_slots.cpp:148-156` | The free slot is found by a linear scan. The standard requires a free list. |
| F4 | gap (MUST) | `sdk/rocketnet_client_protocol.cpp:201-221, 612-614` | When an attempt fails from state 1 or 2 (denied, cookie/request timeout, token expired), the client's ephemeral secret is not zeroed. It survives until the application calls `disconnect`, `connect` or `destroy`. |
| F5 | question | `sdk/rocketnet_server_protocol.cpp:514-576` | A server can deny a response and then, after a slot frees, accept the client's retransmission. If the denied packet reaches the client first, the client ends at -1 while the server holds a slot, an admitted token and an established mapping for it until the idle timeout. |
| F6 | question | `sdk/rocketnet_server_transport.cpp:866-870, 945-951` | STOPPED is not absorbing. `update` after STOPPED runs the full receive/answer/keep-alive path, and `stop` from STOPPED moves the state back to STOPPING. |
| F7 | question | `sdk/rocketnet_server_protocol.cpp:758-783` | A byte-identical replay of a captured connection response refreshes an established slot's `last_receive_time`. S:2873-2876 intends this for the client's own retransmissions; an on-path attacker who can spoof the client's address can use it to keep a departed client's slot alive. |
| F8 | question | `sdk/rocketnet_client_protocol.cpp:1332-1333` | `disconnect()` rewrites a terminal failure state (-7..-1) to 0, and no state callback fires for that change. |
| F9 | superseded 2026-10-18 (the set is removed, `docs/SESSION.md` 1.6) | `sdk/rocketnet_token_tables.cpp:277-278` at the time | `admitted_token_set_add` returns success when the token is already admitted. "A token admits one connection" therefore rests on the request-time `contains` check and the client-id check, not on the set's add. |
| F10 | observation | `sdk/rocketnet_fragment.cpp:181-184, 253, 456` | A slot cleared on delivery forgets its payload id. A late fragment of an older, already-abandoned id then re-occupies the empty slot and is counted abandoned a second time. Bytes never alias. |
| F11 | observation | `sdk/rocketnet_server_transport.cpp:1158-1159` | The maintenance loop reads the mapping address without the `mapping_index < 0` guard that `rocketnet_server_connected_mapping` (`sdk/rocketnet_server_protocol.cpp:1062-1090`) applies. It cannot be reached in the shipped protocol. The `SlotMappingBijection` invariant covers it. |

### F1, traced

The window is `ROCKETNET_REPLAY_WINDOW_BITS = ROCKETNET_HISTORY_PACKETS = 4096` bits
(`sdk/rocketnet_replay.h:26`, `sdk/rocketnet_history.h:8`). It is stored as
`window[ROCKETNET_HISTORY_PACKETS / 64]`, which is 64 words (`sdk/rocketnet_encryption_map.h:34`).
When `rocketnet_replay_advance` skips `≥ W`, it zeroes `window[0..3]` only
(`sdk/rocketnet_replay.cpp:54-60`). That was exact when the window was 256 bits; it has
not been exact since the window was widened to the history budget.

**The outage trace.** A receiver opens session sequences 1..10000 in order. A
consecutive advance clears nothing (`:63-64` loops over an empty range), so after
10000 every one of the 4096 ring bits is set. Then 4,199 sequences are lost. At 1000
packets per second that is a 4.2 s outage, well inside a typical timeout. Sequence 14200
arrives next:

1. `advance(14200)`: skipped = 4200 ≥ 4096, so words 0..3 (ring bits 0..255) are zeroed.
   Bit 14200 mod 4096 = 1912 is set, and `highest_opened = 14200`.
2. `already_received(12000)`: 12000 > 14200 − 4096, so the bit decides. Bit 12000 mod 4096
   = 3808 sits in word 59, still set from sequence 7904. **A genuine packet that was never
   received is refused as ALREADY_RECEIVED** (`:35-39`).
3. `rocketnet_ack_block_build` (`sdk/rocketnet_ack.cpp:51-131`) walks the same bits down
   to the floor 10105. It emits a first run 14200..12544, a gap 12543..12288 (the zeroed
   words), and a run 12287..10105. **The block names 3,839 sequences this end never
   opened.** That breaks S:1733 ("A block MUST NOT name a sequence its sender has not
   opened").

The counts were checked by a literal Python transcription of `rocketnet_replay.cpp:27-70`
and the block walk. The output was `named 3840 never opened 3839` and
`already_received(12000) = 1`.

**Consequences.**
- *False delivery notifications.* The peer still holds sent records for all of
  10105..14199, because its sent ring is also 4096 (`sdk/rocketnet_constants.h:82`). So it
  marks them acked and surfaces their packet ids in `acked_packets`
  (`sdk/rocketnet_ack.cpp:384-414`). The application is told that payloads lost in the
  outage were delivered (U:278-280).
- *Self-stranding at establishment.* Suppose this happens on a client in state 3: the
  server's establishment keep-alives are lost in the burst, and a later session packet
  opens. Then the client's block can name a keep-alive sequence it never opened, and the
  server latches `keep_alive_acked` (`:384-385`). This is the over-claim S:2735-2745
  describes: keep-alives become suppressible and the client can sit in state 3 until its
  total connect deadline.
- *Refused stragglers.* Reordered genuine packets from the outage range are refused.

**Reachability.** 4096 consecutive unopened sequences arise from a loss burst, or from
burnt sequences: a failed fragmented finish burns up to 14 (U:126-160). The unit test at
`sdk/rocketnet_tests.cpp:2792-2799` laps with a bit in word 0, which the four-word clear
covers, so it passes. **The fix shape:** clear every word (`memset` of the window) when
skipped ≥ W. It belongs to a code card, after module 1 shows the trace.

### F2 (closed)

S:1182-1184: "The cookie key is generated at server start and rotated periodically; a
deployment SHOULD accept cookies under the previous key for one rotation period". The
S:2226 timing table gives "Cookie key rotation | 60 s". CLAUDE.md:179 says "cookie key
rotation runs always at its 60 s period". Closed: `sdk/rocketnet_server_transport.cpp:832-838`
initialises `cookie_rotation_seconds` from `ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS` (60)
and `cookie_key_rotation_time = server_transport->time + cookie_rotation_seconds`;
`rocketnet_server_protocol_rotate_cookie_key_if_due` (`sdk/rocketnet_server_protocol.cpp:1198-1227`),
called first in `rocketnet_server_protocol_update`, rotates the key on its period and bounds
the previous key's acceptance window to one period (`sdk/rocketnet_server_protocol.cpp:377`,
`current_time < server->previous_cookie_key_expire_time`), dropping the previous key if an
update stalls past a whole window (`sdk/rocketnet_server_protocol.h:49-55`).

### F3

S:1593: "**A free slot is found from a free list, not a search**". The Lookup table
(S:1580-1588) puts "free client slot | every completed connection | constant time".
`rocketnet_client_slots_assign` scans `connected[]` from 0. A TLA+ model cannot judge
cost, so this finding stands on the reading.

### F4

S:1089-1092: both sides "MUST zero their own ephemeral secret once it is no longer
needed for a handshake in progress". The client zeroes it in three places: on accepting
a challenge (`:509`), at the integrity limit (`:66`) and in `disconnect` (`:1336`). The
failure exits do not zero it:
- the deadline checks set the state and return (`:201-221`);
- the denied path sets -1 and returns (`:612-614`).

In state 2 the secret has not yet been used, so a denied, a timeout to -2/-3, or a -7
from state 1 or 2 leaves it in memory. The only caller of
`rocketnet_client_protocol_disconnect` is `rocketnet_client_transport_disconnect`
(`sdk/rocketnet_client_transport.cpp:1045`). Until the application calls that, or
`connect` (which `memset`s the struct, `:123`), or destroy (`rocketnet_clear_and_free`,
`:395`), the secret stays. The exposure is small: no keys were ever derived from it.
This is still a MUST, and the invariant `EphemeralErasedOutsideHandshake` checks it
directly. The comment at `:49-52` assumes `disconnect` runs "at the end of every attempt",
which nothing enforces.

### F5, traced

S:2602-2607 requires a denied when there are no slots, with "The mapping is left in
place, so a retransmitted response is denied again". S:2251 says "A lost denied:
re-provoked by the next retransmitted ... response". Both assume the server stays full.
Here is a trace where it does not:

1. Client C is in state 3 and retransmits its stored response every 100 ms
   (`client_protocol.cpp:335-342`). The server is full.
2. Response r1 arrives. `num_free ≤ 0` (`server_protocol.cpp:514-519`), so the server
   sends a denied at handshake sequence k and leaves the mapping Half (`:528-544`).
3. Another client's slot is released in an update (timeout or disconnect).
4. Retransmission r2, sent before the denied reached C, arrives. It is a handshake type,
   so there is no replay check (`:716-735`). The slot is free and the admitted-token add
   succeeds, so the server assigns a slot, establishes, and sends keep-alive K1
   (`:546-576`).
5. The denied reaches C first, whether by RTT ordering or reordering. C is in state 3, so
   it opens with its held keys and goes to -1 (`client_protocol.cpp:581-614`). K1 then
   arrives and is ignored (`:670-676`).

*(Superseded in part 2026-10-18: there is no admitted-token set, so the last sentence
below no longer holds; `docs/SESSION.md` 1.6.)* The server now holds a slot, an
established mapping and an admitted token for a client that has given up, until the idle timeout (`server_protocol.cpp:1049-1058`, tens of
seconds). During that time a reconnect from the same address is refused before any
answer, as already connected (`:91-101`). Even after it, a retry with the same token is
refused as already admitted (`:352-357`).

Is this a defect? That is for the maintainer. The handshake module should state
`INV DeniedMeansNoSlot` (client at -1 ⇒ the server holds no slot for that attempt) and
expect this trace as its counterexample. Then decide.

### F6

`rocketnet_server_transport_update` short-circuits only in STOPPING (`:945-951`). In
STOPPED it runs everything: receive, request processing, mapping creation, keep-alives.
On the portable and GSO drivers the socket is still open until destroy (`synchronous_stop`,
`sdk/rocketnet_driver_builtin.cpp:17`). `start_packet` alone checks RUNNING (`:1255`).
Separately, `stop` assigns STOPPING unconditionally (`:866-870`), so STOPPED → STOPPING is
an edge. The documented shutdown is "stop and wait for stopped first" and then destroy
(`sdk/rocketnet.h:734`), and it never takes either edge. So these matter only if STOPPED
is meant to be absorbing, which nothing in S or U says.

### F7

S:2873-2876 says a retransmitted connection response "refreshes the slot's last-received
time even though the response itself is ignored as already connected". That is the code
at `server_protocol.cpp:778-781` (refresh) followed by `:491-496` (ignored). The client
retransmits the same bytes (`client_protocol.cpp:337-341`), and handshake types are
exempt from replay protection (S:1521-1525, `server_protocol.cpp:730`). So an on-path
attacker that captured one response and can spoof the client's address can keep that
slot from timing out after the client leaves. Every other packet that refreshes a slot
is replay-protected. An off-path attacker cannot do this.

### F8

S:2042-2047: "it fails to a terminal state". `rocketnet_client_protocol_disconnect`
moves any non-zero state, including the negative terminals, to 0 (`:1332-1333`). The
state callback fires only inside update
(`sdk/rocketnet_client_transport.cpp:603-604`), so an application that calls
`disconnect` before polling the state loses the reason (S:2054, "The state MUST be
readable").

### F9

Superseded 2026-10-18: the admitted-token set is removed (`docs/SESSION.md` 1.6). One
token is kept to one live connection by the used-token history, the connected-address check
and the client-id check, and a token whose connection has closed may connect again inside
its expiry; `tla/RNSecurityTables.tla` carries `OneTokenOneLiveConnection` and
`ChurnNeverDenied`. The finding below is the record of the set as it was.

`rocketnet_admitted_token_set_add` returns 1 when the identifier is already present
(`token_tables.cpp:277-278`). Two Half mappings carrying one token would therefore both
be admitted by the set. That needs two addresses passing the used-token history, which
is possible only after the history evicts the first binding (S:2566-2570, the documented
flush). The client-id check at response time (`server_protocol.cpp:498-503`) still
refuses the second while the first is connected. After the first disconnects, the second
is admitted under a token that "admits one connection" (S:2430-2433). The handshake
module should check `OneConnectionPerToken` with history eviction enabled and settle
whether that trace is reachable.

### F10

`rocketnet_reassembly_clear_slot` zeroes the whole slot, including `payload_id`
(`fragment.cpp:181-184`). It runs on delivery (`:456`) and on two refusals (`:323, :350`).
The stale test applies only when `slot->payload_id != 0` (`:253`). So a reordered late
fragment of an older id that was already abandoned in this slot re-occupies it after a
newer id completes. The next newer id displaces it and counts a second ABANDONED. S:1906
("older than the id currently occupying its slot") is met to the letter. There is no
aliasing, only counter noise.

---

## M1. The client connection

**Files.** `sdk/rocketnet_client_protocol.{h,cpp}`, `sdk/rocketnet_client_transport.cpp`;
the state constants are at `sdk/rocketnet.h:398-409`.

**States.** `state ∈ {-7 token expired, -6 invalid token, -5 connection timed out, -4
response timed out, -3 request timed out, -2 cookie request timed out, -1 denied, 0
disconnected, 1 requesting cookie, 2 sending connection request, 3 sending connection
response, 4 connected}` (S:2028-2040).

**Owns.** `state`, `salt` (fresh per attempt), `has_cookie` and the stored cookie
(abstract `(gen, expire)`), `eph_secret_live`, `keys` (none | handshake+session committed),
`session_nonce`, `stored_response` (built once), `client_index`,
`send_sequence_handshake`, `send_sequence_session`, `replay_handshake.highest_opened`,
`replay_session` (module 1), `integrity_failures`, `attempt_start_time`,
`last_send_time`, `last_receive_time`, `ack.ack_debt`.

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| any | _App(connect)_: struct reset, then a bad flags / create > expire / wrong version | -6 | `client_protocol.cpp:123-149` |
| any | _App(connect)_: expire ≤ now | -7 | `:151-156` |
| any | _App(connect)_: fresh salt and ephemeral, sequences := 1 | 1 | `:181-192` |
| 1..4 | _Tick_ with `0 < state < 4` and `now − attempt_start ≥ expire − create` | -7 | `:206-212` |
| 1..4 | _Tick_ with `timeout > 0` and `now − last_receive ≥ timeout` | `-(state+1)` | `:214-220` |
| 1, 2 | _Tick_, send interval elapsed: request (carrying the cookie if held) | same | `:297, 314, 321-333` |
| 3 | _Tick_, interval elapsed: stored response, byte-identical | 3 | `:335-342` |
| 4 | _Tick_, interval since the last send of anything: keep-alive | 4 | `:344-356`; payload sends stamp `last_send_time` at `:1105, 1149, 1243` |
| 4 | _Tick_ inside the interval with `ack_debt ≥ 64`: standalone ack | 4 | `:299-310` |
| 1, 2 | _Recv(cookie)_, right size, salt matches: store (replacing) | 2 | `:372-413` |
| other | _Recv(cookie)_ | same | `:372-378` |
| 2 | _Recv(challenge)_ opens under temporary keys from the carried nonce; server ephemeral is not small-order | 3: keys and nonce committed, ephemeral zeroed, response sealed once | `:431-531` |
| 2 | _Recv(challenge)_ fails to open, or is the wrong size | 2, `integrity_failures++` | `:438-444, 476-483` |
| 2 | _Recv(challenge)_ small-order key | 2 (the attempt dies at its deadline) | `:489-500` |
| 2, 3 | _Recv(denied)_ opens (temporary keys in 2, held keys in 3) | -1, **ephemeral not zeroed (F4)** | `:547-614` |
| 3, 4 | _Recv(session type)_: floor, zero seq, replay, open, **advance**, block, size | same; the advance precedes the block and size checks | `:660-735` |
| 3 | _Recv(keep-alive)_ accepted | 4, `client_index` set | `:803-808` |
| 3, 4 | _Recv(disconnect)_ accepted | 0 | `:963` |
| > 0 | 2^36th failed open | 0, keys and ephemeral zeroed | `:55-67` |
| 3, 4 | _App(disconnect)_: 10 sealed disconnects | 0, keys and ephemeral zeroed | `:1309-1336` |
| 1, 2 | _App(disconnect)_ | 0, zeroed | `:1332-1336` |
| -7..-1 | _App(disconnect)_ | **0 (F8)** | `:1332-1333` |
| 4 | _App(start_payload / start_fragments)_ | 4, sequences spent | `:1051-1062, 1174-1186` |

**Rules.**
- `INV ForwardOnly`: within one attempt the positive states are entered in the order
  1, 2, 3, 4, and none twice (S:2042-2045).
- `INV TimeoutPairing`: a timeout from waiting state N lands in -(N+1) (S:2074-2077;
  `:217`).
- `INV TerminalAbsorbing`: only `connect` leaves -7..-1 (S:2046; F8 breaks it through
  `disconnect`; assert it and expect that trace).
- `INV HandshakeRange`: the total deadline applies only while `0 < state < 4`
  (S:2085-2098).
- `INV NoSendOnArrival`: no receive action sends. All sends are in _Tick_, and the
  standalone ack is the one debt-triggered send (S:2178-2182; comment `:290-295`).
- `INV CookieSalt`: a stored cookie came from a cookie packet echoing this attempt's
  salt (S:2142-2148; `:391-396`).
- `INV ForgedNonceNeverCommits`: `keys`/`session_nonce` change only on a successful
  open (`:455-457, 504-509`).
- `INV EphemeralErasedOutsideHandshake`: `state ∉ {1, 2} ⇒ ¬eph_secret_live`
  (S:1089-1092, S:2175-2176). **Fails today (F4).**
- `INV SequenceMonotone`: neither send sequence repeats within an attempt, and 0 is
  never sent (S:2112-2116).
- `INV SessionOnlyFrom3`: no session type is accepted below 3 (S:2199-2201; `:670-676`).
- `LIVE AttemptResolves`: under weak fairness on _Tick_, every attempt leaves 1..3
  (S:2090-2098).
- `LIVE NoSingleLossStrands`: with a fair lossy channel, a server with a free slot and a
  valid token, the client reaches 4 (S:2126, S:2223-2257).

**Verdict: model it.** It is the first machine the request names. The standard's case
for it is that the invariants exist only because it never moves sideways. It is joined
with M2 and M3 as one module (`RNHandshake`), because the climb's properties are
properties of both ends and the wire.

## M2. The server's per-address handshake: encryption mappings

**Files.** `sdk/rocketnet_server_protocol.cpp`, `sdk/rocketnet_encryption_map.{h,cpp}`.

**States** per source address: `None`, `Half` (`established = 0`, `client_index = -1`),
`Est` (with a slot). "Cookie issued" is not stored: it is a fact about the key and the
clock.

**Owns** (`rocketnet_encryption_map.h:37-61`): per address `mapping ∈ {None, Half, Est}`,
`client_index`, `client_id`, `token_id`, `handshake_deadline` (written once),
`last_receive_time`, `send_sequence_handshake`, `stored_challenge` (abstract id),
`session_nonce` (fresh counter value), `replay_handshake.highest_opened`,
`replay_session`, `integrity_failures`. Globally, `num_mappings ≤ capacity` with capacity
`2 × max_clients` (`rocketnet_server_transport.cpp:798`) and a free list of indices
(`encryption_map.cpp:72-74, 133-134, 203-204`).

**Transitions** (all in `rocketnet_server_protocol.cpp` unless marked).

| From | Event | To | Code |
|---|---|---|---|
| Est | _Recv(request)_ | Est, silent | `:91-101` |
| Half | _Recv(request)_ | Half, stored challenge resent byte-identical | `:102-106` |
| None | _Recv(request)_ fails a fixed-offset check (size, version, protocol id, expire, timeout, flags, advertised address, server key) | None, silent | `:78-83, 121-206` |
| None | zero / unverifiable / expired cookie | None, a fresh cookie sent under the current key | `:213-275` |
| None | token bound to another address in the history | None, silent | `:283-288` |
| None | signature, seal or consistency fails | None, silent | `:292-305, 338-343` |
| None | client id connected / token admitted (the admitted check is removed 2026-10-18) | None, silent | `:345-357` |
| None | (past those) history records (token, address) | None | `:359` |
| None | client ephemeral small-order | None, silent | `:373-387` |
| None | no free slot | None, denied at handshake sequence 1 under a fresh nonce | `:395-413` |
| None | mapping table full | None, silent | `:415-423` |
| None | otherwise | Half: challenge built and stored, deadline = now + 5 s, `last_receive = now` | `:415-470`; `encryption_map.cpp:122-154` |
| Half | _Recv(response)_ opens, padding zero, client id free, slot free, admitted add ok | Est: slot assigned, acks and reassembly reset, first keep-alive sent | `:478-576` |
| Half | _Recv(response)_ with no slot (admitted set full until 2026-10-18) | Half, denied (handshake seq++) | `:512-544` |
| Est | _Recv(response)_ opens | Est, **`last_receive_time` refreshed**, ignored (F7) | `:778-783, 491-496` |
| Half | _Recv(session type)_ opens | Half: replay advanced, ignored as NOT_CONNECTED | `:791-801` |
| Est | _Recv(session type)_ accepted | Est: `last_receive_time` refreshed, slot confirmed | `:843-846` |
| Est | _Recv(disconnect)_ accepted | None, slot released | `:938-942` |
| Half, Est | the 2^36th failed open | None, slot released if held | `:597-617` |
| Half | _Tick_ with `now ≥ handshake_deadline` | None | `:1038-1046` |
| Est | _Tick_ with `timeout > 0` and `now ≥ last_receive + timeout` | None, slot released | `:1049-1058` |
| Est | _App(disconnect_client)_: 10 sealed disconnects | None, slot released | `:1627-1680` |

A Half mapping is bounded by its handshake deadline alone. The idle deadline is checked
only once the mapping is established (`:1038-1047`). S:2502-2509 names both bounds; at
the shipped values the handshake bound (5 s) is the tighter one, so this is noted rather
than raised.

**Rules.**
- `INV NoSlotBeforeResponse`: a slot is assigned only on an opened response (S:2496-2499,
  S:1168-1173).
- `INV MappingCreatedOnce`: a live mapping is never replaced. While it lives, its
  address's requests get the same stored challenge (S:2372-2380).
- `INV NonceFresh`: no two mappings ever hold the same session nonce for one salt
  (S:2440-2442). Model the nonce as a counter drawn at creation.
- `INV DeadlineFixed`: `handshake_deadline` is written only at creation (S:2507-2508).
- `INV AlreadyConnectedSilent`: no request sent from an Est address produces any output
  (S:2374-2381).
- `INV NoMappingWithoutCookieAndToken`: a mapping exists only for an address that
  presented an unexpired cookie under a live key and a verified token (S:2322-2326).
- `INV Ledger`: `CLIENT_CONNECTED = CLIENT_DISCONNECTED + CLIENT_TIMED_OUT +
  num_connected` (comment `:589-592`).
- `INV DeniedMeansNoSlot`: client at -1 ⇒ no Est mapping for that attempt. **Expected to
  fail (F5).**
- `LIVE HalfBounded`: every Half mapping leaves Half by its deadline, whatever the peer
  sends (S:2511-2516).

**Verdict: model it**, inside `RNHandshake`. This is the security half of connection
setup: zombie slots, half-open floods, and key recurrence across replays. A `Replay`
attacker action (re-inject any packet seen, from a spoofed source) shows F7 directly.

## M3. The server's client slots

**Files.** `sdk/rocketnet_client_slots.{h,cpp}`.

**States** per slot: `Free`, `Assigned(unconfirmed)`, `Assigned(confirmed)`.

**Owns** (`client_slots.cpp:13-32`): `connected[i]`, `confirmed[i]`, `client_id[i]`,
`mapping_index[i]`, `num_connected`, and the client-id hash chains.

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| Free | response accepted (M2) | Assigned(unconfirmed) | `client_slots.cpp:137-175` (lowest-index scan `:148-156`) |
| Assigned(unconfirmed) | any accepted session packet | Assigned(confirmed) | `server_protocol.cpp:845-846`; `client_slots.cpp:217-224` |
| Assigned | disconnect / timeout / integrity limit / `disconnect_client` | Free: identity cleared, mapping removed and zeroed | `client_slots.cpp:226-263`; `encryption_map.cpp:172-207` |

**Rules.**
- `INV SlotMappingBijection`: `connected[i] ⇒ mapping_index[i] ≥ 0`, that mapping is
  live, and its `client_index = i`; and every Est mapping is named by exactly one slot
  (S:2642-2648). Both index spaces reuse at once (the map's free list and the lowest-free
  scan), so the model must let a released index be reused immediately. That is what
  catches a dangling reference. F11 is the code reading through this invariant unguarded.
- `INV OneSlotPerClientId` (S:2430; checked at `server_protocol.cpp:345, 498`).
- `INV ConnectedCount`: `num_connected = |{i : connected[i]}| ≤ max_clients`.
- `INV ReleaseZeroes`: a released slot's mapping keys are zero (S:2925-2930;
  `encryption_map.cpp:199`). In the model a released mapping leaves the domain.

**Verdict: model it, inside `RNHandshake`.** Alone it is a small allocator. Its risk is
the slot↔mapping binding under index reuse, which shows only alongside M2's removal
paths. F3 (cost) is outside what a model can see.

## M4. Keep-alive generation and the acknowledgment latch

**Files.** `sdk/rocketnet_server_protocol.cpp:1161-1319`, `sdk/rocketnet_ack.cpp:267-278`
(tag), `:384-385` (latch), and the per-slot loop at
`sdk/rocketnet_server_transport.cpp:1151-1172`.

**States** per slot: `Unacked` (paced from `last_keep_alive_send_time`) and `Acked`
(paced from `last_send_time`). The latch is monotone: only a slot reset clears it
(`server_protocol.cpp:554-555`; `ack.cpp:220-225`).

**Owns.** `last_keep_alive_send_time`, `last_send_time`, `keep_alive_acked`, per sent
record `was_keep_alive`, the slot round-robin cursor `maintenance_next_client`, and the
client side's `state`, `client_index` and `replay_session` (the source of its blocks).

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| response accepted | first keep-alive sent; both clocks stamped | `server_protocol.cpp:552, 567`; `:1157, 1198` |
| _Tick_ in Unacked with `now − last_keep_alive ≥ 1/rate` | keep-alive sent and tagged | `:1309-1318, 1198, 1205-1206` |
| _Tick_ in Acked with `now − last_send ≥ 1/rate` | keep-alive sent | `:1313-1318` |
| any session send to the slot | `last_send_time` stamped | `:1157, 1402, 1456, 1577` |
| _Tick_ with no keep-alive due and `ack_debt ≥ 64` | standalone ack | `:1213-1238`; order at `server_transport.cpp:1164-1166` |
| send frames exhausted mid-loop | loop breaks; cursor resumes there next update | `server_transport.cpp:1161-1163, 1171` |
| _Recv_ a block naming a tagged sequence | `keep_alive_acked := 1` | `ack.cpp:384-385` |
| client _Recv(keep-alive)_ in 3 | client 3 → 4 | `client_protocol.cpp:803-808` |

**Rules.**
- `INV LatchImpliesConnected`: with a correct client and truthful blocks,
  `keep_alive_acked ⇒` the client has processed a keep-alive, so it is in state 4 or
  later (S:2820-2845, the "required property" at S:2840-2846). F1 breaks truthful
  blocks, so this module takes the client's received set from module 1's *code*
  representation, not the abstract set.
- `LIVE EstablishmentUnstoppable`: under weak fairness on _Tick_ and a fair lossy
  channel, a client in 3 whose slot is assigned reaches 4 whatever the server
  application sends, including a payload every tick (S:2255-2258, S:2862-2864; the #90
  regression described at `server_protocol.cpp:1257-1264`).
- `LIVE EverySlotServiced`: with frame exhaustion allowed on some updates but not on all,
  every connected slot is eventually checked (the cursor at
  `server_transport.cpp:1151-1172`).
- `INV OverClaimStrandsOnlyItself`: an over-claiming client changes only its own slot's
  latch (S:2747-2750). Structural: one latch per slot.

**Verdict: model it, as its own module with time.** The one regression the standard
records here (#90) was a liveness bug, a payload stream starving establishment. That is
the class TLC finds and tests miss. Time is a tick counter with the send interval as a
constant number of ticks.

## M5. The cookie gate and cookie key rotation

**Files.** `sdk/rocketnet_server_protocol.cpp:350-416, 1198-1236`, `.h:49-55`;
`sdk/rocketnet_server_transport.cpp:832-838`; `sdk/rocketnet_client_protocol.cpp:363-416`.

**States.** Server: key generation `g`, `has_previous`, `previous_cookie_key_expire_time`. Client: `has_cookie` and stored
`(gen, expire)`.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| server create | `g := 0`, no previous key, `rotation_time := time + 60` | `server_transport.cpp:832-838` |
| rotation every 60 s (or stall) | on time: `prev := key; prev_expire := rot + 60; key := fresh`; stall past window: `prev := 0; has_prev := 0; key := fresh` (F2 closed) | `server_protocol.cpp:1198-1236` (`rocketnet_server_protocol_rotate_cookie_key_if_due`) |
| _Recv(request)_ with zero / non-verifying / expired cookie | fresh cookie, current key, lifetime 3 s | `server_protocol.cpp:361-415`; lifetime `server_transport.cpp:835` |
| _Recv(request)_ with a MAC under the current key, or the previous key while `current_time < previous_cookie_key_expire_time`, unexpired | pass | `server_protocol.cpp:370-398` |
| client _Recv(cookie)_ in 1/2 with matching salt | store, replacing | `client_protocol.cpp:402-413` |

**Rules.**
- `INV NoAsymmetricWithoutCookie`: signature verify, seal open and scalar multiplication
  happen only after a cookie passed (S:2322-2326; ordering `:350-445`).
- `INV RotationWindow`, `PreviousBoundedToOnePeriod`, `InFlightCookieSurvivesRotation`: a
  cookie verifies only under the generation that issued it or the next within one rotation
  period, and a cookie issued within `CookieLife < RotatePeriod` survives one rotation
  (S:1182-1184).
- `LIVE CookieRaceResolves`: with lifetime ≥ 2 send intervals, a retransmitting client
  gets past the gate even when a stored cookie expires (S:2016-2022, S:2149-2153).

**Verdict: modelled, small, inside `RNSecurityTables`.** F2 is closed: `RNSecurityTables`
checks `RotationWindow`, `PreviousBoundedToOnePeriod` and `InFlightCookieSurvivesRotation`
with reversed witnesses `AcceptPrevious = FALSE` and `PreviousOutlivesWindow = TRUE`.

## M6. The token tables: used-token history and admitted-token set

*Superseded in part 2026-10-18: the admitted-token set is removed
(`docs/SESSION.md` 1.6). One token is kept to one live connection by the
history, the connected-address check and the client-id check, and
`tla/RNSecurityTables.tla` carries `OneTokenOneLiveConnection` and
`ChurnNeverDenied`. The set's rows below are the record of the code as it
was.*

**Files.** `sdk/rocketnet_token_tables.{h,cpp}`; uses at
`sdk/rocketnet_server_protocol.cpp:283, 352, 359, 521`.

**States** per token id: in the history `Absent | Bound(addr, expire, order)`; in the
admitted set `Absent | Admitted(expire)`. An expired entry reads as absent (`find`,
`:122-133`).

**Owns.** `history` with capacity `8 × max_clients` and `admitted` with capacity
`2 × max_clients` (`server_transport.cpp:799-800`), each with a free list, insert order
and hash chains; the unix clock.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| request past the cookie: check | refuse if bound to another address and unexpired | `token_tables.cpp:221-231` |
| request past consistency: record | no-op if present; else free slot, then any expired slot, then **evict oldest** | `:233-248, 160-188` |
| request: contains | refuse if admitted and unexpired | `:262-268` |
| response accepted: add | **success if present (F9)**; else free slot, then expired slot, else **refuse** (fail closed) | `:270-290` |
| _Tick_ (clock passes expire) | entry reads absent; its slot is reusable | `:130, 174-178` |

**Rules.**
- `INV OneConnectionPerToken`: a token id is never admitted for two connections while
  unexpired (S:2430-2433, S:2572-2577). Check with history eviction on (F9).
- `INV AdmittedNeverEvictedUnexpired` (S:2575; `select_slot(..., 0)` at `:283`).
- `INV DeniedTokenStaysUsable`: a response denied for no slots does not consume the
  token (S:2604-2607; ordering `server_protocol.cpp:512-526`).
- `INV BindingFirstAddress`: while a history entry lives, only its first address passes
  `check` (S:2561-2563).
- The flush-then-replay trace (S:2566-2570) is the *expected* counterexample to
  `BindingFirstAddress` once eviction runs at capacity. It documents the bound rather
  than a defect. State it as a separate property so the expected failure does not mask
  the others.

**Verdict: model it, small, with M5 as `RNSecurityTables`.** What is interesting is how
the two tables and the slot table interact: across a denied-then-retried response, an
expired-then-reused token, a full set, and an eviction.

## M7. The replay window and sequence reconstruction

**Files.** `sdk/rocketnet_replay.{h,cpp}`; `rocketnet_reconstruct_sequence` at
`sdk/rocketnet_protocol.cpp:377` (and `_near` at `:341`); the block walk at
`sdk/rocketnet_ack.cpp:45-131`.

**States** per (connection, key phase, direction): `highest_opened` and the ring
`window` of `W` bits stored as `W/64` words.

**Owns.** `highest`, `bits` (the representation, grouped in words), and for the
specification a ghost `opened`.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| reconstruct(wire) | 64-bit candidate nearest `highest + 1` | `protocol.cpp:377`; S:717-725 |
| `already_received(s)` | `s > highest → no`; `s ≤ highest − W → yes`; else the bit | `replay.cpp:27-40` |
| `advance(s)`, `s > highest`, skip < W | clear `highest+1 .. s−1`, set `s` | `:48-69` |
| `advance(s)`, `s > highest`, skip ≥ W | **clear words 0..3 only (F1)**, set `s` | `:54-60` |
| `advance(s)`, `s ≤ highest` | set `s` | `:69` |
| block build | enumerate bits over `[max(1, highest − W + 1), highest]` | `ack.cpp:51-131` |

Callers advance *before* the ack-block and size checks: client
`client_protocol.cpp:711`, server `server_protocol.cpp:791`. S:2829-2846 depends on
this order.

**Rules.**
- `INV NeverAcceptsReplay`: `¬already_received(s) ⇒ s ∉ opened` (S:1521-1525).
- `INV InWindowExact`: for `highest − W < s ≤ highest`, `already_received(s) ⇔ s ∈ opened`.
  An undersized window "discards old legitimate packets; it never accepts a replay"
  (S:1559-1561). It refuses only by age, never a fresh in-window sequence. **F1 violates
  this.**
- `INV BlockTruthful`: every sequence the block names is in `opened` (S:1733). **F1
  violates this.**
- `INV ZeroNeverValid` (S:2112-2116).

**Verdict: model it, first.** It is the lowest layer under every session packet, every
ack block and the keep-alive latch, and it holds the confirmed defect. It must be
modelled at the representation level: bits grouped in words, and the large-skip branch
clearing a constant `ClearWords`. An abstract-set model would prove the specification
and miss F1.

## M8. Delivery notification: sent records, aggregates, ack debt

**Files.** `sdk/rocketnet_ack.{h,cpp}`; record calls at
`client_protocol.cpp:265, 1099, 1145, 1237` and `server_protocol.cpp:1150, 1395, 1447,
1570`; processing at `client_protocol.cpp:756-764` and `server_protocol.cpp:848-864`.

**States** per sent record (a ring of `ROCKETNET_SENT_PACKET_BUFFER_SIZE` = 4096):
`Empty | Sent(seq, packet_id?, aggregate_id?, was_keep_alive) | Acked`, overwritten by a
lap. Per aggregate (a ring of 2048): `(id, packet_id, remaining)`. Per endpoint:
`highest_sequence`, `ack_debt`, `keep_alive_acked`, and the per-update acked batch.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| session send | record; overwriting an unacked record is classified: known lost (a later ack at least `ROCKETNET_LOSS_PACKET_THRESHOLD` above it) → `SEND_HISTORY_LOST`, unknown fate → `SEND_HISTORY_OVERFLOW`; a stale sequence is refused → `SEND_HISTORY_OVERFLOW`; the packet is always sent | `ack.cpp:228-265` |
| fragmented send | aggregate opened with `remaining = count` (it can lap) | `:280-298` |
| _Recv_ block | named, retained, unacked record → Acked; its id surfaces; aggregate decremented, id surfaces at 0 | `:318-425, 427-467` |
| batch full | a record with an id or aggregate stays unacked (deferred), counted | `:371-374` |
| named but never sent | `ACK_UNKNOWN_SEQUENCE`, no effect | `:327-334, 354-360` |
| eliciting type (4, 5, 6, 9) accepted | `ack_debt++`; type 8 does not | `client_protocol.cpp:763-764`; `server_protocol.cpp:862-863` |
| any block sent | `ack_debt := 0` | `client_protocol.cpp:269, 1103, 1150, 1241`; `server_protocol.cpp:1154, 1399, 1451, 1574` |

**Rules.**
- `INV SurfacedOnce`: each packet id surfaces at most once (U:278-280).
- `INV SurfacedOnlyIfOpened`: a surfaced id's packets were all opened by the peer.
  **F1 breaks this through the peer's blocks.**
- `INV AggregateAllFragments`: a fragmented payload's id surfaces only when every
  fragment is acked (U:121-123).
- `LIVE DeferredSurfaces`: a deferral surfaces in a later update, provided the record is
  not lapped first (S:1831-1834; comment `ack.cpp:363-369`).
- `LIVE AckConversationTerminates`: two idle peers stop exchanging type 8 (S:1800-1806).

**Verdict: model it**, after M7, with the peer's received state supplied by module 1's
representation. It checks deferral under a full batch, aggregate counting under
reordered and partial acks, and the termination argument.

**Modelled: the sent-record slice.** `tla/RNSentHistory.tla` (`tla/README-RNSentHistory.md`)
models the sent history under loss, reordering and ack delay: the ring, the eviction
classification (lost, overflow), `largestAcked`, and the never-refuse rule, with the
reversed witnesses `MCRNSentHistoryBrokenCountEveryUnacked.cfg` and
`MCRNSentHistoryBrokenRefuseWhenFull.cfg`. Aggregates, ack debt and the batch remain
the rest of this module's row.

**Modelled: aggregates and the batch.** `tla/FragmentsAndAcks.tla`
(`tla/README-FragmentsAndAcks.md`) models aggregates under reordered and partial acks,
the batch and its deferral, and burned sequences, with `SurfacedOnce`,
`SurfacedOnlyIfOpened`, `AckedMeansSurfaced`, `BurnedNeverNamed` and the liveness
`OpenedSurfacesOrLaps`. Ack debt and `AckConversationTerminates` remain unmodelled.

## M9. Fragment reassembly

**Files.** `sdk/rocketnet_fragment.cpp:173-460`; the send split in
`client_protocol.cpp:1162-1291` and `server_protocol.cpp:1486-1625`.

**States** per reassembly slot (`payload_id mod num_slots`, per connection): `Empty`, or
`Collecting(id, count, stride ∈ {unknown, k}, final_bytes, final_staged, received bits)`.
Completion delivers and clears straight back to `Empty`.

**Transitions** (all in `rocketnet_fragment.cpp`).

| From | Event | To | Code |
|---|---|---|---|
| any | id = 0 | same, ZERO_PAYLOAD_ID | `:232-237` |
| Collecting(o) | id older than o (serial arithmetic) | same, STALE | `:253-258` |
| any | count = 1 / index ≥ count | same, refused | `:263-275` |
| Collecting(id) | count differs | same, COUNT_MISMATCH | `:285-290` |
| Collecting(id), stride known | non-final ≠ stride / final > stride | same, SIZE_MISMATCH | `:300-314` |
| Collecting(id), stride unknown, final staged | a non-final whose stride < staged final | **Empty**, SIZE_MISMATCH | `:319-325` |
| any | reconstructed size > max | **Empty** if same id, else unchanged; PAYLOAD_TOO_LARGE | `:337-353` |
| Collecting(id) | index already held | same, DUPLICATE_INDEX | `:359-364` |
| Collecting(o) or Empty | a newer id passes every check | Collecting(id); ABANDONED if it displaced o | `:371-382` |
| Collecting | final first | staged in the slot's buffer | `:409-417` |
| Collecting | first non-final | stride fixed, staged final placed | `:389-396` |
| Collecting | last missing fragment | delivered → Empty (F10) | `:424-457` |
| any | slot assigned / connect | all slots of the connection Empty | `server_protocol.cpp:557-558`; `client_protocol.cpp:128-129` |

**Rules.**
- `INV RefusalLeavesSlot`: every refusal except the two whole-payload refusals leaves
  the slot unchanged (S:1919-1923; comment `:225-228`).
- `INV DeliveredExact`: a delivered payload is exactly the fragments of one id, each
  once, at `index × stride` (S:1924-1934).
- `INV NoAliasing`: no byte of one id lands in another id's payload (S:1906-1910).
- `INV CountPinned` (S:1919-1923).
- `LIVE SlotRecovers`: after any id, including one near 2^32, later ids occupy the
  slot (S:1909-1915).
- `INV NoResurrection`: an id abandoned in a slot never occupies it again. **Expected
  to fail (F10); recorded so the card can rule on it.**

**Verdict: model it, small.** This is the classic home of reassembly memory bugs: a
final fragment arriving first, a stride fixed late, a count widened. The comments carry
the argument; the model makes it checked. Arena bounds stay unit tests.

**Modelled.** `tla/FragmentsAndAcks.tla` (`tla/README-FragmentsAndAcks.md`) models one
slot set over a lossy link with `DeliveredExact`, `DeliveredOnce`, `CompleteDelivers`,
`ArenaBounded` and `CountPinned`. Its findings: D1, the stale test is an unsigned
bounded difference, not the serial comparison the standard names, so whole-payload loss
or one late fragment poisons a slot (`StaleOnlyIfOlder`); D2, the delivered batch
overflows under legitimate load (`BatchFullNeverFires`). `NoResurrection` (F10) and
`SlotRecovers` remain unmodelled.

## M10. The fill phase: packet leases and sequence burns

**Files.** `sdk/rocketnet_packet_lease.h:13-103`;
`sdk/rocketnet_server_transport.cpp:936-939, 1248-1750` (start/finish/abort);
`sdk/rocketnet_client_transport.cpp:549, 634-930`; U:37-62, U:110-160, U:290-300.

**States.** Phase `Update | Fill | Flush` is a contract, not a variable (U:46-50). Each
lease entry has `state = (epoch << 2) | {0 free, 1 writing, 2 active, 3 consumed}`. Each
send sequence is `Unallocated | Spent | Sent`.

**Transitions.**

| Event | Effect | Code |
|---|---|---|
| `update` / `send_packets` (main thread, joined) | `epoch++`, `used := 0`; every outstanding context goes stale | `packet_lease.h:98-102`; `server_transport.cpp:939, 1669`; `client_transport.cpp:549` |
| `start_packet` (any fill thread) | sequence spent (atomic add), frame acquired, lease inserted by CAS on a stale-epoch entry | `server_protocol.cpp:1350`; `packet_lease.h:35-61` |
| `finish_packet` / `abort_packet` | `take`: CAS active → consumed; refused if a field differs or the epoch is stale | `packet_lease.h:63-96` |
| arena finish (fragmented) | reserve `count − 1` more sequences; a seal failure withdraws the block | `server_transport.cpp:1499-1650` (`:1523, 1584`) |

**Rules.**
- `INV ConsumeOnce`: a context is consumed at most once. A copy, a changed field, or a
  context from an earlier phase is refused (U:294-296).
- `INV ResolveFindsOwn`: a resolve from the frame's hash never stops short of its own
  live entry in the current epoch. This holds because entries are claimed, never
  released, within an epoch, and `resolve` stops at the first entry of another epoch
  (`:72-73`).
- `INV SequenceNeverReused`: no two packets on one connection carry one sequence
  (S:2112-2116). Burns are allowed (U:126-160).
- `INV NoLeaseAfterReset`: no entry of an older epoch resolves.

**Verdict: model it, after the protocol modules.** It is a small lock-free protocol (CAS,
a spin on `writing`, entries reused across epochs) run by any number of threads. That is
where interleavings hide. The burn arithmetic is accounting, not a machine; it stays in
tests.

## M11. The server transport lifecycle

**Files.** `sdk/rocketnet.h:652-654, 734`; `sdk/rocketnet_server_transport.cpp:774, 866-870,
936-951`; `sdk/rocketnet_driver.cpp:133-136`; `sdk/rocketnet_driver_builtin.cpp:17, 85-99,
117-121`; XDP `sdk/rocketnet_server_socket_linux.cpp:1589-1592, 1672-1686`.

**States.** Transport `RUNNING → STOPPING → STOPPED` (with F6's STOPPED → STOPPING edge).
The XDP socket has its own `RUNNING → STOPPING → STOPPED`; its STOPPED does not mean the
worker threads are quiescent, because they are joined only in destroy (`:1635-1647`).

**Transitions.**

| From | Event | To | Code |
|---|---|---|---|
| — | create succeeds | RUNNING | `server_transport.cpp:774` |
| any | _App(stop)_ | STOPPING | `:866-870` |
| STOPPING | _App(update)_, driver stop granted | STOPPED | `:945-950` |
| STOPPING | _App(update)_, driver stop pending (IOCP draining) | STOPPING | `:947-948`; `driver_builtin.cpp:117-121` |
| STOPPED | _App(update)_ | **full update (F6)** | `:953-1190` |
| XDP STOPPING | socket update | STOPPED at once, no drain | `server_socket_linux.cpp:1676-1679` |
| any | destroy | threads quit and are joined | `:1635-1647`; IOCP `server_iocp_windows.cpp:267-283` |

**Rules.**
- `INV NoStageAfterStop`: `start_packet` returns NULL outside RUNNING (`:1255`).
- `INV NoProtocolAfterStopped` — assert it and expect F6's trace, unless the maintainer
  rules that STOPPED is not absorbing.
- `LIVE StopCompletes`: repeated `update` after `stop` reaches STOPPED, given fair
  driver progress.

**Verdict: model it, small, with M14 as one module.** Alone it is three states and F6.
Its value is as the frame M14 runs inside.

## M12. The receive flip (XDP, GSO, client)

**Files.** XDP game side `sdk/rocketnet_server_socket_linux.cpp:2057-2097` and worker
`:2147-2230`; GSO game side `sdk/rocketnet_server_gso_linux.cpp:1105-1111` and worker
`:443-552`; client `sdk/rocketnet_client_transport.cpp:466-478, 563-585` (game) and
`:1099-1150` (receive thread); IOCP worker `sdk/rocketnet_server_iocp_windows.cpp:185-232`.
CLAUDE.md:91 claims the handoff is correct: the worker "holds `receive_mutex`" across its
fill, and the game flips between batches.

**States.** A counter (or index) whose parity names the worker's buffer; per buffer
`num_packets`; the mutex holder.

**Transitions.**
- Game: lock, flip, unlock, then drain the other buffer without the lock and zero its
  count.
- Worker: lock, read the parity, append up to capacity, unlock.

The client is the mirror image. It flips `receive_buffer_index`, zeroes the *off* buffer
inside the lock, and drains the *on* index (`client_transport.cpp:466-478, 565`), while
its thread fills the off index (`:1105`). XDP and IOCP copy the packet bytes out of the
kernel frame before unlocking (`server_socket_linux.cpp:2207`;
`server_iocp_windows.cpp:212`), so no frame is shared across the flip.

**Rules.**
- `INV Exclusive`: the game never reads a buffer the worker is writing, and the worker
  never writes the buffer the game is draining.
- `INV Conserved`: every appended packet is drained exactly once or counted dropped.

**Verdict: model it, one small module parameterised for the client's mirror.** The
pattern exists four times (XDP, GSO, IOCP, client) and its correctness is prose. Two
processes and two packets make it checked once.

## M13. XDP frame ownership and the send handoff

**Files.** `sdk/rocketnet_server_socket_linux.cpp:2121-2229` (receive frames and pending
return), `:2232-2247` (completions), `:2249-2356` (send batch), `:1762-1795, 1870-1897`
(start, acquire, publish); `sdk/rocketnet_pending_frames.h:9-35`;
`sdk/rocketnet_send_handoff.h:9-44`. CLAUDE.md:100-106 ranks the pending-return spill
first among known issues: it "has not been driven on hardware".

**States.** Each receive frame is in one of `Fill` (kernel), `Rx` (kernel → user ring)
or `Pending` (user, awaiting a fill reservation). Each send frame is in one of `Free`,
`Batched`, `Tx` or `Completed`. Each send generation is `free | filling | queued`, with
`head`, `count`, `filling` and the published `send_flow_index`.

**Transitions.**
- Worker receive: peek ≤ `pending.available()`; consume ≤ the staging room; release
  exactly the consumed count; append to pending; flush all-or-zero into the fill ring
  (`:2141-2229`; `pending_frames.h:16-34`).
- Worker send: complete-ring frames → free (`:2236-2247`). The front generation's packets
  → frames; on exhaustion stop and retain `packet_start_index` (`:2287-2298`). Retire the
  generation when every packet is copied (`:2318-2321`). TX reserve is all-or-zero; zero
  keeps the batch (`:2334-2355`).
- Main thread: `send_packets` publishes a non-empty filling generation and acquires the
  next (`:1883-1897`). A fill thread may acquire on the slow path when both are queued
  (`:1766-1775`).

**Rules.**
- `INV FrameConservation`: every UMEM frame has exactly one owner at every step
  (comment `:2159-2170`; `pending_frames.h:6-7`).
- `INV ReleaseConsumedOnly`: the RX release covers exactly the consumed descriptors.
- `INV HandoffNoOverwrite`: a full queue refuses new storage without overwriting a
  queued generation (`send_handoff.h:8`).
- `INV RetireAfterCopy`: a generation is retired only after its last packet is in UMEM
  (`:2318-2319`).
- `LIVE FramesReturn`: if the kernel drains the fill and TX rings fairly, every Pending
  frame returns to Fill and every Tx frame to Free.

**Verdict: model it.** It is CLAUDE.md's top known issue, it cannot be driven without
hardware, and conservation across four rings is exactly what a model checks without
hardware.

## M14. IOCP shutdown

**Files.** `sdk/rocketnet_server_iocp_windows.cpp:137-285, 438-467`.

**States.** `stopping`, `pending` (operations posted and not yet completed),
`active_workers`, the per-worker mutex, the retry list, and per operation
`Posted | Completing | Idle | Retry`.

**Transitions.**
- `post_receive`: if stopping, return; else `pending++` and post; on an immediate failure
  `pending--` and push to the retry list (`:137-158`).
- Worker: exit at the loop top if `stopping ∧ pending = 0` (`:166-167`). Otherwise
  dequeue, then under its mutex `pending--` and repost for each completion (`:185-220`),
  then drain the retry list (`:224-231`).
- `stop`: `stopping := TRUE` (once); pass through every worker mutex; `CancelIoEx`; post
  one wake per worker (`:237-258`).
- `stopped`: `stopping ∧ pending = 0 ∧ active_workers = 0` (`:260-265`).
- Sends: the main thread checks `stopping`, then `pending++` and posts (`:446-455`). This
  is safe only because `stop` also runs on the main thread (U:49-50). The model takes
  that as an assumption and a reversed witness drops it.

**Rules.**
- `INV NoPostAfterCancel`: no operation is posted after `CancelIoEx` returns (comment
  `:139-141`).
- `INV DestroyQuiescent`: at destroy, `pending = 0` (assert `:283`).
- `LIVE StopDrains`: after `stop`, eventually `stopped`.
- The case to check is the window between one worker's `pending--` and its repost,
  where another worker sees `pending = 0`. By hand it is safe: the repost sees
  `stopping` and does not post, or it posted before the mutex barrier released.

**Verdict: model it, small, with M11.** A shutdown protocol over an atomic counter and a
mutex barrier, argued in comments and not yet checked.

## M15. GSO tier degradation

**Files.** `sdk/rocketnet_server_gso_linux.cpp:1036-1059`.

**States.** `gso_enabled`, monotone on → off.

**Transitions.** A live GSO refusal (EINVAL, EIO, EOPNOTSUPP on a GSO message) turns
the tier off and resends from that message's first slot as plain packets
(`:1038-1048`). EAGAIN and other errors drop the rest of that flush, counted
(`:1050-1059`).

**Verdict: do not model.** Two states, one edge, and an index computation. A unit test
that forces a refusal mid-batch and counts packets is the right instrument.

## M16. Frame sync

**Files.** `sdk/rocketnet_frame_sync.{h,cpp}`.

**States.** `first_wait_done`, `spinning` (fixed at the first wait), `deadline_ns`,
`guard_ewma_ns`.

**Transitions.** The first wait anchors the deadline (`:344-354`). Each wait sleeps, may
spin, then advances (`:356-386`; `rocketnet_frame_sync_advance` at `:250-268`).

**Verdict: do not model.** A two-state anchor and clock arithmetic, with no concurrency
and no protocol. Its pure parts are already factored out for direct tests
(`rocketnet_frame_sync.h`).

## M17. Considered and set aside

| Item | Where | Why no module |
|---|---|---|
| Driver ladder selection | `sdk/rocketnet_driver.cpp`; S:73-172 | A pure function from a list to a grant; no state survives create. Table tests. |
| Config and env precedence | `sdk/rocketnet_server_transport.cpp:377-397` and around | Pure function. |
| Library init / term | `sdk/rocketnet.cpp` | Three states and two refusals; a unit test covers it. |
| Client transport lifecycle (thread start/quit) | `client_transport.cpp:355-386, 1140-1150` | One quit flag, a join; the flip is covered by M12. |
| Keyed prefilter epochs | S:478-546 | Specified, not implemented in 1.0. Model it when built: the generation window and the wrap are the properties. |
| Backend connected set | S:889-981 | A deployment property outside the library. |
| Darwin `msgx` pump's live-refusal pause | `sdk/rocketnet_platform.cpp:1139-2000` | Outside this card's START list; not read. It deserves its own line in the next inventory. |
| Stats windows, loss and jitter trackers | `rocketnet_stats_window.h`, `rocketnet_packet_loss_tracker.h`, `rocketnet_jitter_tracker.h` | Measurement, not control: nothing reads them to decide a transition. |
| Integrity limit | `client_protocol.cpp:55-67`; `server_protocol.cpp:597-617` | A counter to 2^36 with one edge; folded into `RNHandshake` as an action guarded by a small `IntegrityLimit` constant. |

---

## The module list, in order

These are ordered bottom-up, per the standing practice: each module stands on the ones
before it. **The layer list goes to the maintainer for correction before module 1 is built**; this
inventory proposes the layers, it does not settle them.

Common shape for every module:
- Every module lives in `tla/<Module>.tla`, with `tla/MC<Module>.tla` and
  `tla/MC<Module>.cfg` beside it.
- Every TLC run is a row in `tla/RUNS.tsv` (module, config, states, distinct, result,
  bench, date), run on vision (else hetzner), never on the Studio.
- Each module is cited from the code it models, in a comment at the function named in
  its "cite from" line.

Common channel: a bag of packets, with actions `Send`, `Lose` (remove any), `Dup` (copy
any) and `Deliver` (any one, so order is free). Where the module is about security, add
`Inject`: an attacker that can re-send any packet it has seen from any source address,
and can forge nothing sealed.

Time is a `Tick` action over a small integer clock. Liveness uses weak fairness on
`Tick` and strong fairness on delivering *some* copy of a retransmitted packet, never on
one particular packet.

Every module also carries a **reversed witness**: one constant flip that reproduces a
known-bad version, which TLC must reject. A module whose witness passes is not checking
anything.

### 1. `RNReplayWindow` (M7)

- CONSTANTS `WordBits = 2`, `Words = 4` (W = 8), `ClearWords ∈ {1, 4}`, `MaxSeq = 24`.
- VARIABLES `highest`, `win ∈ [0..Words-1 → [0..WordBits-1 → BOOLEAN]]`, ghost `opened`.
- Actions: `Open(s)` for any `s ∈ 1..MaxSeq` with `¬AlreadyReceived(s)`. This is
  `advance` exactly as `replay.cpp:42-70` writes it: clear `ClearWords` words when the
  skip is ≥ W, else clear the skipped bits. `Probe(s)` is the check alone. Order and gaps
  are free, so loss, reorder and duplication are all inside `Open`'s choice.
- Properties: `NeverAcceptsReplay`, `InWindowExact`, `BlockTruthful` (the block as the
  set of `s ∈ [max(1, highest − W + 1), highest]` whose bit is set), `ZeroNeverValid`.
- MC: `ClearWords = 1` keeps the code's ratio of cleared to total words (4 of 64 is
  1 of 16; at 4 words, 1 is the nearest). It must fail `InWindowExact` and
  `BlockTruthful` with a trace of a skip ≥ W over stale words: F1. `ClearWords = Words`
  is the fix and must pass.
- Reversed witness: the fix passing *and* the code failing is the pair.
- Cite from: `rocketnet_replay_advance`.

### 2. `RNSecurityTables` (M5 + M6)

- CONSTANTS `Tokens = {t1, t2, t3}`, `Addrs = {a1, a2}`, `HistoryCap = 2`,
  `AdmittedCap = 2`, `MaxClock = 4`, `CookieLife = 1`, `RotatePeriod = 2`.
- VARIABLES `history`, `admitted`, `insertOrder`, `keyGen`, `hasPrev`, `clock`,
  `connections` (a ghost: which token admitted which connection).
- Actions:
  - `Request(t, a, cookieGen, cookieExp)`, running the ladder in order (gate, check,
    contains, record);
  - `Response(t, a)`, running add (with F9's present-means-success), or deny on
    `noSlot`;
  - `Rotate`, `Tick`;
  - `Disconnect(t)`, which ends a ghost connection.
- Properties: `OneConnectionPerToken`, `AdmittedNeverEvictedUnexpired`,
  `DeniedTokenStaysUsable`, `RotationWindow`, `NoAsymmetricWithoutCookie`.
  `BindingFirstAddress` goes in a second cfg: expected to fail by flush-then-replay,
  documenting S:2566-2570.
- Reversed witness: `admitted` evicts oldest. That must fail `OneConnectionPerToken`.
- Cite from: `rocketnet_token_table_select_slot`, and the gate at
  `rocketnet_server_protocol_process_connection_request`.

### 3. `RNHandshake` (M1 + M2 + M3, with the integrity limit and key erasure)

- CONSTANTS `Clients = {c1, c2}` (c2 shares c1's client id in one cfg), `MaxClients = 1`,
  `MapCap = 2`, `SendInterval = 1`, `HandshakeDeadline = 3`, `Timeout = 4`,
  `TokenLife = 6`, `IntegrityLimit = 2`, `InFlight = 3` per direction. The tables come
  from module 2 as an abstract admitted/bound predicate.
- VARIABLES: client `state`, `salt`, `ephLive`, `keys`, `cookie`, `seqH`, `seqS`;
  server `mapping[addr]` with `{status, clientIndex, nonce, deadline, lastRecv,
  challengeId}`, `slot[i]` with `{connected, confirmed, clientId, mappingIndex}`,
  `freeMapIdx`; the channels; `nonceCounter`; `clock`.
- Actions: `Connect`, `ClientTick` (send / deadlines), `ClientRecv(p)`, `ServerRecv(p)`,
  `ServerSweep`, `AppDisconnect`, `AppDisconnectClient(i)`, `SlotFreedElsewhere` (for F5),
  `Lose`, `Dup`, `Deliver`, `Inject`, `Tick`.
- Properties:
  - safety: `ForwardOnly`, `TimeoutPairing`, `NoSlotBeforeResponse`,
    `MappingCreatedOnce`, `NonceFresh`, `DeadlineFixed`, `AlreadyConnectedSilent`,
    `SlotMappingBijection`, `OneSlotPerClientId`, `EphemeralErasedOutsideHandshake`
    (expect F4);
  - liveness: `AttemptResolves`, `NoSingleLossStrands`, `HalfBounded`;
  - in a separate cfg: `TerminalAbsorbing` (expect F8), `DeniedMeansNoSlot` (expect F5),
    and `SlotReleasedAfterClientGone` under `Inject` (expect F7).
- Budget: this is the largest state space. If TLC exceeds a bench budget, cut to one
  client before cutting `Dup` or `Deliver`'s freedom. Those carry the traces the
  standard argues about.
- Reversed witness: assign the slot at the challenge (pre-step-3). That must fail
  `NoSlotBeforeResponse` under `Inject` (S:1168-1173).
- Cite from: `rocketnet_client_protocol_update`,
  `rocketnet_server_protocol_process_connection_request`,
  `rocketnet_client_slots_assign`.

### 4. `RNAckDelivery` (M8)

- CONSTANTS `SentRing = 4`, `AggRing = 2`, `BatchCap = 1`, `MaxSeq = 8`, one fragmented
  payload of 2 fragments.
- VARIABLES: per endpoint `sent`, `aggregates`, `highestSent`, `ackDebt`, `batch`; the
  peer's received set as module 1's `win`/`highest`.
- Actions: `SendPayload`, `SendFragmented`, `SendAck`, `RecvBlock`, `Update` (drains the
  batch), plus the channel actions.
- Properties: `SurfacedOnce`, `AggregateAllFragments`, `SurfacedOnlyIfOpened` (fails
  under module 1's `ClearWords = 1` and passes under the fix), `DeferredSurfaces`,
  `AckConversationTerminates`.
- Reversed witness: type 8 elicits. That must fail `AckConversationTerminates`.
- Cite from: `rocketnet_ack_apply_run`.

### 5. `RNKeepAlive` (M4)

- CONSTANTS `SendInterval = 2` ticks, `UpdateTick = 1`, one client and one slot,
  `AppSendsEveryTick ∈ {TRUE, FALSE}`, `Slots = 2` with `FrameBudget = 1` per update
  (for `EverySlotServiced`).
- VARIABLES `lastKA`, `lastSend`, `latch`, `tagged`, the client `state`, the cursor, the
  channels, `clock`.
- Properties: `LatchImpliesConnected`, `EstablishmentUnstoppable`, `EverySlotServiced`,
  `OverClaimStrandsOnlyItself`.
- Reversed witness: pacing reads `lastSend` before the latch (the #90 code). That must
  fail `EstablishmentUnstoppable` with `AppSendsEveryTick`.
- Cite from: `rocketnet_server_protocol_write_keep_alive_if_due`.

### 6. `RNReassembly` (M9)

- CONSTANTS `IdBits = 4` (ids modulo 16, serial arithmetic at 8), `Slots = 2`,
  `Counts = {2, 3}`, `Stride = 2`, `MaxPayload = 5`, and one defective sender allowed to
  vary count and size.
- VARIABLES: per slot `{id, count, stride, finalBytes, finalStaged, received}`, the
  ghost `delivered`, and `abandonedIds`.
- Properties: `RefusalLeavesSlot`, `DeliveredExact`, `NoAliasing`, `CountPinned`,
  `SlotRecovers`; `NoResurrection` in a separate cfg (expect F10).
- Reversed witness: a plain `<` instead of serial arithmetic. That must fail
  `SlotRecovers` across the wrap (S:1909-1915).
- Cite from: `rocketnet_reassembly_process_fragment`.

### 7. `RNPacketLeases` (M10)

- CONSTANTS `Threads = 2`, `Capacity = 4` (half usable), `Epochs = 2`, `Contexts = 3`
  including a copied one.
- VARIABLES: `entry[k].state`, `entry[k].ctx`, `used`, `epoch`, per thread `pc`.
- Actions: insert, resolve and take, broken into their atomic steps (load, CAS, publish)
  so interleavings are real; `Reset` only when every thread is idle (the join contract,
  U:46-48).
- Properties: `ConsumeOnce`, `ResolveFindsOwn`, `NoLeaseAfterReset`.
- Reversed witness: `Reset` allowed while a thread is mid-insert. That must fail
  `ResolveFindsOwn` or `ConsumeOnce`.
- Cite from: `rocketnet_packet_leases_t`.

### 8. `RNReceiveFlip` (M12)

- CONSTANTS `BufCap = 2`, `Packets = 3`, `Mirror ∈ {FALSE, TRUE}` (TRUE is the client's
  index variant).
- VARIABLES `counter`, `buf[0..1]`, `lockHolder`, game and worker `pc`, the ghosts
  `appended` and `drained`.
- Properties: `Exclusive`, `Conserved`.
- Reversed witness: the worker releases the mutex between appends. That must fail
  `Exclusive`.
- Cite from: `rocketnet_server_socket_receive_packets`, `rocketnet_client_receive_packets`.

### 9. `RNXdpFrames` (M13)

- CONSTANTS `RxFrames = 4`, `FillRing = 2`, `RxRing = 2`, `PendingCap = 2`,
  `StageCap = 1`, `TxFrames = 3`, `TxRing = 2`, `Generations = 2`; kernel actions are
  nondeterministic (fill → rx, tx → complete, reserve may return zero).
- VARIABLES `owner[frame]`, `pending`, the ring contents, `handoff {free, queued, head,
  count, filling}`, `flowIndex`, `startIndex`, `batch`.
- Properties: `FrameConservation`, `ReleaseConsumedOnly`, `HandoffNoOverwrite`,
  `RetireAfterCopy`, `FramesReturn`.
- Reversed witness: release the peeked count instead of the consumed count. That must
  fail `FrameConservation` (the bug the comment at `:2159-2166` describes).
- Cite from: `rocketnet_server_socket_xdp_thread_function`.

### 10. `RNTransportLifecycle` (M11 + M14)

- CONSTANTS `Workers = 2`, `RecvOps = 2`, `SendOps = 1`.
- VARIABLES: transport `state`, `stopping`, `pending`, `activeWorkers`, per op status,
  per worker `pc` and mutex, `cancelled`, `retry`.
- Properties: `NoPostAfterCancel`, `DestroyQuiescent`, `StopDrains`, `StopCompletes`,
  `NoStageAfterStop`; `NoProtocolAfterStopped` in a separate cfg (expect F6).
- Reversed witness: `stop` skips the mutex barrier. That must fail `NoPostAfterCancel`.
- Cite from: `rocketnet_server_iocp_stop`, `rocketnet_server_transport_update`.

### 11. `RNConnectionMemory` (the message layer's per-connection memory)

Added after the inventory, with the message layer (docs/CHANNELS.md 4). Written,
run and recorded: `tla/RNConnectionMemory.tla`, `tla/README-RNConnectionMemory.md`.

- CONSTANTS `Slots = {s1, s2, s3}`, `Clients = {c1, c2, c3, c4}`, `Capacity = 3`.
- VARIABLES: per slot `state` (Free, Connected, Disconnected), `owner`, `used`,
  `exhausted`, `reason`, `records`.
- Actions: `Connect`, `Use`, `Release`, `Exhaust`, `Kick`, `Leave`, `Reuse`.
- Properties: `ExhaustionOnlyLeadsToDisconnect`, `ReusedSlotStartsClean`,
  `NoCrossSlotEffect`, `ExhaustedReasonNamed`, `UsedBounded`; LIVE
  `ExhaustedEventuallyFree`.
- Reversed witnesses: `BrokenNoReset` must fail `ReusedSlotStartsClean`;
  `BrokenCarryOn` must fail `ExhaustionOnlyLeadsToDisconnect`.
- Cite from: `rn_region_send_alloc`, `rn_region_send_free`,
  `rn_region_send_free_fragment`, `rn_region_exhaust`,
  `rn_connection_memory_decide`, `rn_region_reset`.

### 12. `ClientSession` (the client object's life across connections)

Added after the inventory, ahead of the connect, disconnect and reconnect API. Written, run
and recorded: `tla/ClientSession.tla`, `tla/README-ClientSession.md`, group `clientsession`.

- CONSTANTS `MaxSessions = 3`, `MaxCreates = 2`; `Defect`, `UnresetField` select a witness.
- VARIABLES: `st` (NONE, DISCONNECTED, the three handshake stages, CONNECTED, seven error
  states), `cur`, `conn` (one field per row of the reset inventory, Init or a session tag),
  `reason`, `kept` (lifetime counters, config), `link`.
- Actions: `Create`, `Destroy`, `Connect`, `HandshakeStep`, `Deny`, `TokenExpires`, `Timer`,
  `ServerDisconnect`, `ClientDisconnect`, `DisconnectFromError`, `Send`, `ServerSend`,
  `Deliver`, `Lose`; every exit from a connection goes through one `Reset`.
- Properties: `ConnectOnlyFromDisconnected`, `NoSendUnlessConnected`, `ResetIsInit`,
  `RecordIsCurrent`, `ReconnectObservesNothing`, `ReasonKeptUntilNextConnection`; LIVE
  `ConnectReachesConnectedOrError`.
- Reversed witnesses: `MCClientSessionUnreset.cfg` (Reset skips `replay_window`) must fail
  `ReconnectObservesNothing`; `MCClientSessionConnectAnywhere.cfg` must fail
  `ConnectOnlyFromDisconnected`.
- Cite from: the client's single reset function and connect, written by
  rocketnet-client-connect-disconnect-reconnect (owed; `tla/COVERAGE.tsv`).

### 13. `ServerSession` (the server object's life and the per-slot reset)

Added with module 12. Written, run and recorded: `tla/ServerSession.tla`,
`tla/README-ServerSession.md`, group `serversession`.

- CONSTANTS `SlotCount = 2`, `MaxSessions = 2`, `MaxTokens = 2`, `MaxStarts = 2`,
  `MaxCreates = 1`; `Defect`, `UnresetField` select a witness.
- VARIABLES: `state` (NONE, STOPPED, RUNNING, STOPPING), per slot `st`, `sess`, `rec` (the
  same per-connection record), `reason`; `kept` (used-token history, keys, config, lifetime
  counters, kept across stop and start per STANDARD.md:2913); `draining`; `link`.
- Actions: `Create`, `Destroy`, `Start`, `Stop`, `DrainStep(i)`, `StopComplete`,
  `DisconnectAll`, `DrainDone`, `ClientConnects(i, t)`, `Establish(i)`, `Deny(i)`,
  `SlotTimeout(i)`, `ClientLeaves(i)`, `DisconnectClient(i)`, `ServerSend(i)`, `ClientSend`,
  `Deliver`, `Lose`; every exit from a slot's connection goes through one `Reset(i)`.
- Properties: `StartOnlyFromStopped`, `NoSendUnlessConnected`, `ResetIsInit`,
  `RecordIsCurrent`, `SlotReuseObservesNothing`, `NoClientWhileStopped`,
  `StopDisconnectsAll`, `ReasonKeptUntilNextConnection`, `ResetTouchesOnlyItsSlot`,
  `TokenAdmitsOnce`; LIVE `HandshakeResolves`, `StopReachesStopped`.
- Reversed witnesses: `MCServerSessionUnresetSlot.cfg` must fail `SlotReuseObservesNothing`;
  `MCServerSessionStopLeavesClient.cfg` must fail `NoClientWhileStopped`;
  `MCServerSessionTouchesOtherSlot.cfg` must fail `ResetTouchesOnlyItsSlot`.
- Cite from: the server's per-slot reset, start, stop and `disconnect_all_clients`, written by
  rocketnet-client-connect-disconnect-reconnect and rocketnet-per-connection-reset-is-complete
  (owed; `tla/COVERAGE.tsv`).

Modules 1 to 5 are what the request names: client and server connection and security,
with the ack layer under the keep-alive rule. Module 1 goes first because modules 3 to 5
read its received state and F1 lives in it. M15 and M16 get no module; their reasons are
in their sections.

---

## What this inventory did not do

- No module was written and TLC was not run. `tla/` and `tla/RUNS.tsv` do not exist yet;
  the first module card creates them.
- The Darwin `msgx` pump (`sdk/rocketnet_platform.cpp`) was outside START and was not
  read.
- F1 to F11 are recorded here and in the card's report. None is fixed here:
  - F1 is a defect.
  - F2, F3 and F4 are gaps against the standard.
  - F5 to F8 are questions for the maintainer.
  - F9 to F11 are observations.

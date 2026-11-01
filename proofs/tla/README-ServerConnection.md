# ServerConnection: the server's slot lifecycle and encryption mapping

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

> **Current model note (2026-10-18).** The following historical narrative and RUNS examples describe the earlier admitted-token abstraction. The current model removes `admitted`, `admits`, and `AdmittedCap`: admission is not a separate global token set. `bound[t]` represents used-token history by source address; a mapping is created at request time, while history is bound only when an opened response is accepted. Request-side connected-ID checks are followed by the authoritative response-side connected-ID check. The current properties are `OneTokenOneLiveConnection`, `ChurnNeverDenied`, and `ExpiredTokenNeverMapped`; `OneConnectionPerToken` remains a compatibility alias for the first property for the small `MCServerConnection.tla` wrapper.

> Two reversed witnesses exercise the checks at the point they matter. In `MCServerConnectionClientIdNoCheck.cfg`, two same-token requests from distinct clients create pending mappings before either response arrives. The first response binds the token and occupies a slot; with the response-side client-ID check disabled, the second response occupies another slot too, violating `OneTokenOneLiveConnection` (24,910 generated / 10,671 distinct states; expected invariant violation, exit 12). The production check at `sdk/rocketnet_server_protocol.cpp:734` looks up the client ID when processing the response, after the request race, and prevents that second seat.

> In `MCServerConnectionHoldUntilExpire.cfg`, the trace seats then times out token 1, seats then times out token 2 while history persists, and finally submits fresh token 3 while a slot is free. A global held-token cap would reject token 3 despite no live connection; the current behavior accepts it, so the old hold restriction is exposed by the expected `ChurnNeverDenied` violation (102,917 generated / 39,836 distinct, exit 12). The compact witness uses MapCap=1, Capacity=1, Timeout=1, and disables application disconnect actions to keep the timeout/history sequence small. `MCServerConnectionExpiredToken.cfg` passes `ExpiredTokenNeverMapped` (134 generated / 48 distinct, exit 0).

> The main `MCServerConnection.cfg` is a bounded representative instance: two clients, two selectable tokens (`TokenChoice = "any"`), one slot, disconnect-based slot reuse, and both live safety invariants. To keep full exploration within the runner's 110-second cap it uses MapCap=1, MaxNonces=1, Timeout=0, HandshakeDeadline=0, and link Capacity=1 with no loss, duplication, reordering, or delay. It passes all declared checks (554,563 generated / 116,118 distinct states, depth 38, 52.181 seconds). This bound excludes concurrent pending mappings and timer-driven release; the dedicated reversed client-ID witness retains two pending mappings and exercises the duplicate-identity response race, while the hold witness exercises timeout/history churn. The original larger timer-enabled main instance timed out at 110.481 seconds (192,963 distinct states) and is retained as a limitation in the run evidence.
>
> The four tool-generated serverconnection records are merged into the global `proofs/tla/RUNS.tsv` with `tlacheck merge --keep proofs/tla/RUNS.tsv`, and `tlacheck groups --root . --stale` prints `[]`. The four cases re-measured identically with the pinned runner and JAR: `MCServerConnection.cfg` passes (554,563 generated / 116,118 distinct, exit 0), `MCServerConnectionExpiredToken.cfg` passes (134 / 48, exit 0), `MCServerConnectionClientIdNoCheck.cfg` violates `OneTokenOneLiveConnection` (24,910 / 10,671, exit 12), and `MCServerConnectionHoldUntilExpire.cfg` violates `ChurnNeverDenied` (102,917 / 39,836, exit 12). The earlier sidecar `proofs/tla/RUNS-ServerConnection.tsv` is retained as the raw run evidence.


> **Both families.** `ServedFamilies` is the set of families the server serves (one, or both when `ConfiguredFamilies` names both). `ReachesServer(c)` is `PickFamily(c) \in ServedFamilies`. `TokenAddressAdvertised` checks the token address of the family the request arrived on and does not consult the other family. `ClientHost(c)` is that client's host families. `MCServerConnectionServesBothFamilies.cfg` passes `BothFamiliesConnect`: client 1 is IPv4-only, client 2 is IPv6-only, each token names both families, and the server's config names both. `MCServerConnectionBrokenOneFamilyServed.cfg` is the reversed witness (`ServeFirstFamilyOnly`): the config names both and the server serves only the first family, so `BothFamiliesConnect` fails.



`proofs/tla/ServerConnection.tla` is the server of `sdk/rocketnet_server_protocol.cpp` (the
request ladder, the response, the sealed session types, the sweep, the keep-alive and the
application disconnect) with its tables: the encryption map keyed by source address
(`sdk/rocketnet_encryption_map.cpp`), the client slots (`sdk/rocketnet_client_slots.cpp`)
and the two token tables as far as they decide admission (`sdk/rocketnet_token_tables.cpp`).
It is the server-side counterpart of `proofs/tla/ClientConnection.tla`: the same packet names and
the same phases, with the clients as the environment this time. Both are checked over two
instances of the shared lossy link `proofs/tla/Link.tla`: `C2S` carries every client's packets to
the server, `S2C` the server's packets to every address. `proofs/tla/MCServerConnection.tla` and
`proofs/tla/MCServerConnection.cfg` are the small instance; every TLC run is a row in
`proofs/tla/RUNS.tsv`. The inventory this card worked from is `docs/TLA-INVENTORY.md`, M2 and M3
(with M4's keep-alive pacing and M6's admission checks where they decide a slot). Line
numbers below are at `3ad4179` (origin/main at the time of writing).

## What the model covers

**The server's state** (`struct rocketnet_server_protocol_t` and its tables):

| Variable | Code | Meaning |
|---|---|---|
| `smap[a]` | `rocketnet_encryption_map_t`, one `rocketnet_encryption_mapping_t` per address | `live`; `est` (`established`, `client_index != -1`); the keys as `[salt, nonce]` (`connection_salt`, `session_nonce`); `tok` (`token_identifier`, `client_id`); `slot` (`client_index`); `age` (the fixed `handshake_deadline`); `sinceRecv` (`last_receive_time`, the idle clock in either state); `sinceKA` (`last_keep_alive_send_time`); `acked` (`keep_alive_acked`) |
| `slot[i]` | `rocketnet_client_slots_t` | `used` (`connected[i]`), `addr` (the mapping's address, which the code reaches through `mapping_index[i]`), `tok` (`client_id[i]`), `confirmed` |
| `bound[t]` | the used-token history | the first address that presented `t`, or none |
| `admitted` | the admitted-token set | tokens that admitted a connection |
| `admits[t]` | ghost | how many connections `t` admitted, saturating at 2 |
| `sDiscPending`, `sDiscPkt` | the disconnect burst | copies still to send after `disconnect_client` |

Capacities: `MaxSlots` is `max_clients`; `MapCap` and `AdmittedCap` are the code's
`max_clients * 2` (`sdk/rocketnet_server_transport.cpp:821-823`).

**The clients, as the server sees them**: one record per client with the ClientConnection
states (-1 denied, 0 disconnected, 1 requesting cookie, 2 sending request, 3 sending
response, 4 connected), the attempt number, the token this attempt presents (its own, or any token the
environment holds, chosen at connect), the address held now, the address the held cookie was
issued to (the cookie MAC covers the source address, so a cookie does not survive a move),
the committed nonce, the send clock and the disconnect burst. The client's deadlines are
folded into two outside events: the application disconnects (the burst from 3 or 4), or the
host vanishes with nothing sent.

**Actions**, one per operation and per outside event, each cited to the function it models:

| Action | Who | Code |
|---|---|---|
| `ServerRecvRequest` | a request arrives | `rocketnet_server_protocol_process_connection_request` (`:150`): a connected address is silent (`:200`); a half-open mapping resends its stored challenge whatever the request's salt (`:239`); no cookie, or one issued to another address, gets a cookie (`:364`); a token bound elsewhere in the history (`:439`), a connected client id (`:511`) or an admitted token (`:518`) is silent; the history records the binding (`:523`); no free slot is a denied under fresh keys with no mapping (`:574`); a full map is silent (`:583`); else a mapping at a fresh nonce and its challenge (`:579-631`) |
| `ServerRecvResponse` | a response opens under the mapping's handshake keys | the idle clock is refreshed before the ladder (`:952`); `rocketnet_server_process_connection_response` (`:642`): already connected (`:657`) or the client id connected (`:664`) is silent; no slot (`:680`) or the admitted set full of other tokens (`:687`) is a denied, mapping kept; else the token is admitted, the lowest free slot assigned (`:710`, `rocketnet_client_slots_assign` `sdk/rocketnet_client_slots.cpp:146-153`) and the first keep-alive sent (`:731`) |
| `ServerRecvSession` | a keep-alive or disconnect opens under the session keys | a mapping without a slot ignores it (`:969`); a keep-alive refreshes the idle clock (`:1014`), confirms the slot (`:1017`) and its block sets the keep-alive-acknowledged latch; a disconnect releases the slot and removes the mapping at once (`:1112-1113`) |
| `ServerKeepAlive(i)` | the server's timer | `rocketnet_server_protocol_write_keep_alive_if_due` (`:1414`): paced off the keep-alive's own clock (`:1487`, `:1372`) |
| `ServerAppDisconnect(i)`, `ServerDiscCopy` | the server application | `rocketnet_server_protocol_disconnect_client` (`:1801`): the burst (`:1830`), then the slot released and the mapping removed (`:1852-1853`) |
| `Tick` | time | `rocketnet_server_protocol_update` (`:1194`): a half-open mapping is removed at whichever of its idle and handshake deadlines comes first (`:1216-1230`); an established one idle past its timeout releases its slot and is removed with it (`:1230-1240`); the clients' send clocks; both links `Age` |
| `ClientConnect`, `ClientSend`, `ClientDiscCopy`, `ClientAppDisconnect` | a client | ClientConnection's `AppConnect`, `ClientSend`, `ClientDiscCopy`, `AppDisconnect`, with the token chosen at connect |
| `ClientVanish` | the outside | the host is gone; nothing is sent again |
| `ClientMove` | the outside | the client's address changes to one no client holds (NAT rebinding, a handover); the old address keeps its mapping; the cookie no longer verifies |
| `ClientRecv` | a packet arrives at an address | the client holding that address runs ClientConnection's receive (cookie 1→2, challenge 2→3, denied → -1, keep-alive 3→4, disconnect → 0); a packet to an address nobody holds is delivered to the void |
| `C2SEnvironment`, `S2CEnvironment` | the path | Link's `Drop`, `Duplicate`, `Reorder`, `Delay` |

Every packet that drives a transition crosses a Link instance; every packet has a receive
step whatever the state (what the code's ladder, state gate or key check refuses is
delivered and discarded), so the links' fairness is never vacuous (README-Link.md,
caution 1).

### Properties

Safety, checked under every loss pattern with no fairness:

| Property | The rule |
|---|---|
| `OneClientPerSlot` | no two live clients in one slot: at most one client holds the keys a used slot's mapping opens under, and no two used slots share an address (S:342-351, the address is the connection identity) |
| `OneSlotPerClientId` | a client id holds at most one slot (S:2647, S:2817) |
| `SlotMappingBijection` | no slot without a live, established mapping behind it naming that slot, and no established mapping without the used slot it names bound to its address (S:3141-3144, slot reset destroys the mapping; S:2852-2853; the leak the audits suspect at the 2x mapping table) |
| `NoSlotBeforeResponse` | a live mapping holds a slot only once established: no amount of requests consumes one (S:2712-2715) |
| `MappingsBounded` | the map stays inside its capacity (S:2760-2764, the 2x table; `sdk/rocketnet_server_transport.cpp:821`) |
| `OneConnectionPerToken` | the same token never admits two clients (S:2648, S:2788-2791) |
| `NonceFresh` | no two live mappings share keys (S:32-33, no key or nonce outlives one connection; S:2656-2658) |
| `StoredChallengeNeverReplaced` | every live mapping holds the one challenge fixed at its creation: the salt and nonce a request sees are the stored ones, never replaced (S:2691-2697) |
| `TimeoutHonoured`, `DeadlineHonoured`, `HalfOpenIdleHonoured` | the idle timeout and the handshake deadline fire when due, for an established mapping and for a half-open one |
| `PacketsReachOwnSlotOnly` | a client's packets never reach another client's slot: a client packet that touches a used slot (its record, or the mapping behind it) came from that slot's address and opened under that mapping's keys; after an address change the client's packets come from an address with no mapping, or another client's, and touch nothing |
| `SessionNeverAnswered` | a session packet never provokes a send (S:2852-2853, a disconnect is never answered); the keep-alive is timer driven (S:2894, keep-alive generation reads the clocks, never an arrival) |
| `TypeOK` | types, including both links' |

Liveness, under the fairness described below:

| Property | The rule |
|---|---|
| `Connects` | with a slot free and fair loss, every attempt reaches connected |
| `ServerNotStranded` | a half-open mapping is established or dies; an unacknowledged slot is acknowledged or released |
| `TimedOutSlotFreed` | a timed-out slot is freed: a slot whose client is gone (no client holds its keys in a session state) is eventually released |
| `AbandonedMappingDies` | a half-open mapping whose client is gone dies at its deadline |

### The hang the request named

The class: one side moves to a new state, sends the packet announcing it once, the packet is
lost, and nobody ever sends it again. On the server the packets that announce a state are
the cookie, the challenge, the denied and the establishing keep-alive. The first three are
re-provoked by the client's own retransmission (S:2693-2705: every valid request is answered
every time it arrives), so the server holds state to repeat them (the stored challenge) or
needs none (the cookie and the request-time denied are recomputed). The keep-alive is the
one the server must re-send on its own timer, because the client in state 3 can only repeat
its response, which a connected mapping ignores. `Connects` and `ServerNotStranded` are
checked with every deadline and timeout off, so that only the protocol's own re-sending can
get the client to 4; `RepeatKeepAlive = FALSE` is the reversed witness, the server that sends
the establishing keep-alive once at the response and never again. The code's behaviour is
`RepeatKeepAlive = TRUE` (`rocketnet_server_protocol_write_keep_alive_if_due`,
`sdk/rocketnet_server_protocol.cpp:1414-1492`).

## What the model abstracts away

- **Keys.** A key pair is `[salt, nonce]`: the attempt's salt (the pair `<<client, attempt>>`
  here; 64 random bits in the code) and the mapping's session nonce are the two fresh inputs
  every key derives from, so "opens under the mapping's keys" is equality on that pair. The
  ephemerals, the small-order check, the AEAD open and the integrity limit are not modelled: a
  packet under the wrong pair is delivered and discarded.
- **The cookie** is the address it was issued to, echoing the salt: the MAC covers the source
  address, so a cookie obtained at one address fails at another. Lifetime and key rotation
  are the security module's. A cookie never expires here.
- **The replay windows** are not modelled: a duplicated client keep-alive refreshes the slot
  again. That weakens nothing above (a copy on the wire is finite) and is `RNReplayWindow`'s
  business; F7 (a byte-identical response refreshes an established slot) is in the model as
  the code has it, the refresh before the ladder.
- **The tokens** do not expire inside a run, so the admitted set never forgets (the code's
  entries leave only by expiry); the used-token history binds without eviction, and
  `History = "none"` is the flush the standard documents (S:2782-2786), checked as a separate
  run so the admitted set is seen to carry the rule alone.
- **The client id** is a function of the token: `IdOf(t) = t`, or `1` for every token with
  `SameId = TRUE` (one player holding two tokens).
- **Payloads and the standalone ack** are not here: a payload confirms a slot and refreshes
  it as a keep-alive does, and neither moves a slot or a mapping. The #90 clock (a payload
  stream suppressing the keep-alive) is ClientConnection's witness.
- **Time** is a tick of one send interval on every clock, with saturating counters. A
  `Timeout` or `HandshakeDeadline` of 0 is off. `Timeout` bounds an established mapping's
  silence and, after a request refreshes it, a half-open mapping's; a half-open mapping
  dies at whichever of `Timeout` and `HandshakeDeadline` it reaches first.
- **The ack block** is one bit: a client session packet accepted by an established mapping
  sets the keep-alive-acknowledged latch.
- **The sweep and the receive loop** interleave freely; the code runs the sweep after the
  update's receives. No property here depends on that order.
- **Fairness** is restated at this model's size, as ClientConnection does; see the next
  section.

## Fairness

`Spec` is `Init /\ [][Next]_vars /\ Fairness` with weak fairness on `Tick`, each client's
`ClientConnect`, `ClientSend` and `ClientDiscCopy`, each slot's `ServerKeepAlive` and the
server's `ServerDiscCopy`; strong fairness on the through-sends (`ClientSendThrough`,
`ServerKeepAliveThrough`: a timer-driven send that is enabled with the pattern letting it
through, infinitely often, goes through infinitely often, which is Link's
`EventualDelivery` stated on the sender); and strong fairness on delivering the head of each
wire (`ClientRecvHead`, `ServerRecvHead`). `SpecTyped` adds Link's `EventualDelivery` per
packet type for `"arbitrary"`. README-ClientConnection.md records why Link's own `Fairness`
does not fit a model of this size and why the liveness runs use a FIFO link without
duplication (`Reordering = FALSE`, `MaxDelay = 0`, `Duplication = FALSE`); the same holds
here. The reversed witness `RepeatKeepAlive = FALSE` is the check that the fairness is not
vacuous.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19), from `proofs/tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -deadlock -lncheck final \
     -config MCServerConnection.cfg MCServerConnection
```

The committed cases are the rows of group `serverconnection` in `CASES.tsv`; `RUNS.tsv`
holds each one's measured result. Every case is on a FIFO link without duplication
(`C2SPattern = S2CPattern = "none"`), as the fairness section requires for liveness.

| Case | Instance | Checks | Expected |
|---|---|---|---|
| `MCServerConnection.cfg` | two clients, two addresses, any token, one slot, disconnects | `SafetyInvariants`, `ChurnNeverDenied`, `ExpiredTokenNeverMapped`, `PacketsReachOwnSlotOnly`, `SessionNeverAnswered` | pass |
| `MCServerConnectionTwoSlots.cfg` | the same with two slots | the same | pass |
| `MCServerConnectionLive.cfg` | one client, one slot, `KindLoss = "firstkind"`, every deadline off | `SafetyInvariants`, `Connects`, `ServerNotStranded` | pass |
| `MCServerConnectionBrokenOnceKeepAlive.cfg` | `Live` with `RepeatKeepAlive = FALSE` | `ServerNotStranded` | fails (temporal) |
| `MCServerConnectionTimers.cfg` | one client that may disconnect or vanish, `Timeout = 2`, `HandshakeDeadline = 2` | `SafetyInvariants` (the timer invariants live), `TimedOutSlotFreed`, `AbandonedMappingDies`, `ServerNotStranded` | pass |
| `MCServerConnectionBrokenNoTimeout.cfg` | `Timers` with `Timeout = 0` | `TimedOutSlotFreed` | fails (temporal) |
| `MCServerConnectionBrokenNoDeadline.cfg` | `Timers` with `Timeout = 0`, `HandshakeDeadline = 0` | `AbandonedMappingDies` | fails (temporal) |
| `MCServerConnectionBrokenSweepKeepsMapping.cfg` | `Timers` with `SweepRemovesMapping = FALSE` | `SlotMappingBijection` | fails (invariant) |
| `MCServerConnectionMove.cfg` | one client, any token, two attempts, one address change | `SafetyInvariants`, `PacketsReachOwnSlotOnly`, `SessionNeverAnswered` | pass |
| `MCServerConnectionBrokenAddressNotIdentity.cfg` | `Move` with `AddressIsIdentity = FALSE` | `PacketsReachOwnSlotOnly` | fails (action) |
| `MCServerConnectionSameId.cfg` | `SameId = TRUE`, two slots, two nonces, no disconnects | `SafetyInvariants` | pass |
| `MCServerConnectionBrokenClientIdNoCheck.cfg` | `SameId` with both client-id checks off | `OneSlotPerClientId` | fails (invariant) |
| `MCServerConnectionHistoryNone.cfg` | one token shared by two clients, two slots, `History = "none"` | `SafetyInvariants` | pass |
| `MCServerConnectionClientIdNoCheck.cfg` | one token, two clients, the response-time id check off | `OneTokenOneLiveConnection` | fails (invariant) |
| `MCServerConnectionHoldUntilExpire.cfg` | three clients, one slot, `HoldUntilExpire = TRUE` | `ChurnNeverDenied` | fails (invariant) |
| `MCServerConnectionExpiredToken.cfg` | one client with an expired token | `ExpiredTokenNeverMapped` | pass |
| `MCServerConnectionResponseReplay.cfg`, `...ResponseReplayBroken.cfg` | one client, timers on, duplication | `ResponseNeverRefreshesConnected` | pass; fails (action) |
| `MCServerConnectionDualFamily.cfg`, `...BrokenIpv6OnlyMatch.cfg`, `...ServesBothFamilies.cfg`, `...BrokenOneFamilyServed.cfg`, `...Race.cfg`, `...RaceIpv6.cfg`, `...BrokenCommitEarly.cfg` | the address-family runs | `OneFamilyHostConnects`, `BothFamiliesConnect`, `Connects`, `CommitsOnAnswer` | as `CASES.tsv` declares |

Every rule switch is off in some case: `RepeatKeepAlive`, `SweepRemovesMapping`,
`AddressIsIdentity`, both client-id checks and `HoldUntilExpire` in a reversed witness;
`SameId`, `MaxMoves`, `KindLoss` and `History` in a passing case. Other loss patterns
(`"first"`, `"alternate"`, `"burst"`, `"arbitrary"`) and duplication with reordering are not
committed cases; nothing here claims them.

## What TLC found

Every counterexample below was replayed by hand through the functions it names before it was
written down. Every reversed witness fails as it must, and the code's behaviour passes every
committed case. One model error was found and fixed on the way.

### The hang, reproduced (reversed witnesses)

1. **`RepeatKeepAlive = FALSE` under `"firstkind"`**
   (`MCServerConnectionBrokenOnceKeepAlive.cfg`): `ServerNotStranded` fails with a lasso.
   The first packet of each kind is lost on the way: a request, the first cookie, the
   first challenge. The retransmitted request draws the stored challenge, the response
   assigns the slot, and the establishing keep-alive, the first of its kind, is lost. From
   there the lasso is `Tick`, `ClientSend` (the stored response), `ServerRecvResponse`
   (already connected: the idle clock refreshed, nothing sent). The server never sends
   another keep-alive, and the slot is never acknowledged. Replayed:
   `rocketnet_server_process_connection_response` ignores the retransmission once
   connected; what the witness removes is `rocketnet_server_protocol_write_keep_alive_if_due`,
   which the code runs every update off the keep-alive clock until the client's block
   acknowledges one.
2. The code's behaviour passes (`MCServerConnectionLive.cfg`): the stored
   challenge answers every retransmitted request (`:211`), the cookie and the request-time
   denied are recomputed on every arrival (`:364`, `:574`), and the keep-alive is the one
   packet the server re-sends on its own timer.
3. **The timers** (`MCServerConnectionTimers.cfg` passes `TimedOutSlotFreed` and
   `AbandonedMappingDies`). `MCServerConnectionBrokenNoTimeout.cfg` turns the idle timeout
   off: the client connects and vanishes, the server keeps sending keep-alives into the
   void, and the slot is never freed. `MCServerConnectionBrokenNoDeadline.cfg` turns off the
   handshake deadline and the half-open idle timeout: the client disconnects with the
   mapping half open, and the mapping lives forever. Replayed: the code's
   `rocketnet_server_protocol_update` removes a half-open mapping at the earlier of its
   handshake deadline and `ROCKETNET_HALF_OPEN_IDLE_TIMEOUT_SECONDS`, and an established one
   at `timeout_seconds` with its slot (`rocketnet_server_connection_reset`); each witness
   removes those removals.

### The slot and mapping witnesses (reversed, each expected to fail)

4. **`SweepRemovesMapping = FALSE`** (`MCServerConnectionBrokenSweepKeepsMapping.cfg`,
   `SlotMappingBijection`, 14 states): one
   client connects (request, challenge, response, slot 1 assigned), then two ticks with
   nothing heard; at `Timeout = 2` the sweep releases the slot and leaves the mapping live,
   established and naming slot 1, which is free. That is the leak the audits suspected at
   the 2x table, and the stale reference F11 reads through. Replayed: the code's sweep does
   both at `rocketnet_server_protocol_update` (`:1228` releases, `:1229` removes); the
   witness drops `:1229`. With the code's behaviour the bijection holds in every state of
   every passing case, and `MappingsBounded` with it.
5. **Both client-id checks off with `SameId = TRUE`**
   (`MCServerConnectionBrokenClientIdNoCheck.cfg`, `OneSlotPerClientId`, two slots): two clients present two tokens carrying one client id; both get
   mappings and both responses are seated. Replayed: the code refuses the second at the
   request (`rocketnet_client_slots_find`, `sdk/rocketnet_server_protocol.cpp:511`) and
   again at the response (`:664`); the witness removes both.
6. **`AddressIsIdentity = FALSE`** (`MCServerConnectionBrokenAddressNotIdentity.cfg`,
   `PacketsReachOwnSlotOnly`, 16 states): a
   client gets its challenge at address 1, moves to address 2, and sends its response and
   then a disconnect from there. The witness server opens each under whichever live mapping
   its keys fit: the response seats the client with the mapping at address 1, and the
   disconnect from address 2 releases that slot, a packet from one address acting on a slot
   bound to another. Replayed: the code looks a sealed packet up by its source address only
   (`rocketnet_encryption_map_find(server->encryption_map, from)`,
   `sdk/rocketnet_server_protocol.cpp:873`); a response from address 2 finds no mapping and
   is `NO_MAPPING`, and the slot at address 1 is left to its idle timeout. With the code's
   behaviour the property holds (`MCServerConnectionMove.cfg`).

### One token, one live connection

7. **The used-token history flushed** (`MCServerConnectionHistoryNone.cfg`, pass). The code
   has no admitted-token set (`docs/SESSION.md` 1.6): the rule is one token, one live
   connection, and a token whose connection has closed may connect again inside its expiry.
   `OneConnectionPerToken` is `OneTokenOneLiveConnection`. With one token shared by two
   clients and no history binding, the client-id checks at the request and the response
   keep the token to one live slot. `MCServerConnectionClientIdNoCheck.cfg` turns the
   response-time check off and fails it.

### Model errors, fixed

8. **The salt keyed on the attempt number.** The first draft's key pair was
   `[att, nonce]` with `att` each client's own attempt counter, as ClientConnection has it
   for its one client. TLC's first full run (19 states, `OneClientPerSlot`) had two clients
   present the same token with attempt number 1 each: the first got a mapping and its
   challenge, which the link duplicated; the first client moved away and the second moved
   onto the vacated address; the duplicate challenge, addressed to that address and
   carrying attempt 1, was accepted by the second client, which committed the first
   client's keys and then completed the first client's handshake from the first client's
   address. Two clients held one slot's keys. Replayed against the code: the challenge is
   sealed under handshake keys derived from the token keys, the 64-bit random
   `connection_salt` and the session nonce (`rocketnet_derive_connection_keys`, called at
   `sdk/rocketnet_server_protocol.cpp:537`); two clients sharing a token share the token
   keys, and nothing else, so the second client cannot open the first's challenge. The
   model's salt was too small, not the code. The salt is now the pair
   `<<client, attempt>>`, which no two clients share, and the same instance passes.

## Two-slot instance

`MCServerConnectionTwoSlots.cfg` is the committed configuration with
`MaxSlots = 2` and `MapCap = 2`, two clients and two tokens, every deadline
and both application disconnects unchanged. It keeps `SafetyInvariants`,
`ChurnNeverDenied` and `ExpiredTokenNeverMapped` and the two action
properties. It passes: 470,755 generated / 100,278 distinct states, depth 38,
44.315 s, exit 0, and is a required case.

The deployed server has up to `ROCKETNET_MAX_CLIENTS` slots and a mapping
table of `max_clients * 2` entries. The safety invariants are local: each
names a slot, a mapping or a token binding and its statement does not count
slots, while `MappingsBounded` compares the live mappings with the configured
capacity. The two-slot instance exercises the lowest-free-slot assignment
across two live slots, two simultaneous mappings and slot reuse; a third or
later slot changes only which index is free, not the predicates. The
size-dependent part it does not cover is the sweep and handshake-deadline
interleaving at a full table, and the reverse witnesses use their own small
instances. Link patterns are the committed no-loss FIFO pattern; the larger
loss-pattern runs are recorded above as owed bound.

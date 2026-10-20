# ServerConnection: the server's slot lifecycle and encryption mapping

`tla/ServerConnection.tla` is the server of `sdk/rocketnet_server_protocol.cpp` (the
request ladder, the response, the sealed session types, the sweep, the keep-alive and the
application disconnect) with its tables: the encryption map keyed by source address
(`sdk/rocketnet_encryption_map.cpp`), the client slots (`sdk/rocketnet_client_slots.cpp`)
and the used-token history as far as it decides admission (`sdk/rocketnet_token_tables.cpp`).
It is the server-side counterpart of `tla/ClientConnection.tla`: the same packet names and
the same phases, with the clients as the environment this time. Both are checked over two
instances of the shared lossy link `tla/Link.tla`: `C2S` carries every client's packets to
the server, `S2C` the server's packets to every address. `tla/MCServerConnection.tla` and
`tla/MCServerConnection.cfg` are the small instance; every measured case is a row in
`tla/RUNS.tsv` (the raw records of the four cases are also in `tla/RUNS-ServerConnection.tsv`).
It covers the server's connection states and slots, with the keep-alive pacing and the
admission checks where they decide a slot.

Admission is one token, one live connection. There is no separate set of admitted tokens:
`bound[t]` is the used-token history, which binds a token to the first source address
whose response opens, and the client-id checks refuse a second seat while the first is
connected. A token whose connection has closed may connect again from its bound address.
The properties for this are `OneTokenOneLiveConnection`, `ChurnNeverDenied` and
`ExpiredTokenNeverMapped`; `OneConnectionPerToken` is another name for the first.

## What the model covers

**The server's state** (`struct rocketnet_server_protocol_t` and its tables):

| Variable | Code | Meaning |
|---|---|---|
| `smap[a]` | `rocketnet_encryption_map_t`, one `rocketnet_encryption_mapping_t` per address | `live`; `est` (`established`, `client_index != -1`); the keys as `[salt, nonce]` (`connection_salt`, `session_nonce`); `tok` (`token_identifier`, `client_id`); `slot` (`client_index`); `age` (the fixed `handshake_deadline`); `sinceRecv` (`last_receive_time`, the idle clock in either state); `sinceKA` (`last_keep_alive_send_time`); `acked` (`keep_alive_acked`) |
| `slot[i]` | `rocketnet_client_slots_t` | `used` (`connected[i]`), `addr` (the mapping's address, which the code reaches through `mapping_index[i]`), `tok` (`client_id[i]`), `confirmed` |
| `bound[t]` | the used-token history | the first address that presented `t`, or none |
| `churnDenied` | ghost | a fresh, valid token was refused while a slot was free |
| `sDiscPending`, `sDiscPkt` | the disconnect burst | copies still to send after `disconnect_client` |

Capacities: `MaxSlots` is `max_clients`; `MapCap` is the code's map capacity,
`max_clients * ROCKETNET_ENCRYPTION_MAP_RATIO` (2, in `sdk/rocketnet_server_transport.cpp`).
`HeldCapacity` is used only by the hold-until-expiry witness.

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
| `ServerRecvRequest` | a request arrives | `rocketnet_server_protocol_process_connection_request`: a connected address is silent; a half-open mapping resends its stored challenge whatever the request's salt; an expired token is silent, before the cookie gate and any decryption; no cookie, or one issued to another address, gets a cookie; a token bound to another address in the history or a connected client id is silent (and so, in the `HoldUntilExpire` witness, is any token already in the history, or any new one once `HeldCapacity` tokens are held); no free slot is a denied under fresh keys with no mapping; a full map is silent; else a mapping at a fresh nonce and its challenge |
| `ServerRecvResponse` | a response opens under the mapping's handshake keys | `rocketnet_server_process_connection_response`: the idle clock is refreshed and the history binds the token to this address (`rocketnet_used_token_history_record`); already connected, or the client id connected (`rocketnet_client_slots_find`), is silent; no slot is a denied, mapping kept; else the lowest free slot is assigned (`rocketnet_client_slots_assign`) and the first keep-alive sent |
| `ServerRecvSession` | a keep-alive or disconnect opens under the session keys | a mapping without a slot ignores it; a keep-alive refreshes the idle clock, confirms the slot and its block sets the keep-alive-acknowledged latch; a disconnect releases the slot and removes the mapping at once |
| `ServerKeepAlive(i)` | the server's timer | `rocketnet_server_protocol_write_keep_alive_if_due`: paced off the keep-alive's own clock, `last_keep_alive_send_time` |
| `ServerAppDisconnect(i)`, `ServerDiscCopy` | the server application | `rocketnet_server_protocol_disconnect_client`: the burst, then the slot released and the mapping removed |
| `Tick` | time | `rocketnet_server_protocol_update`: a half-open mapping is removed at whichever of its idle and handshake deadlines comes first; an established one idle past its timeout releases its slot and is removed with it; the clients' send clocks; both links `Age` |
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

Safety, checked with no fairness:

| Property | The rule |
|---|---|
| `OneClientPerSlot` | no two live clients in one slot: at most one client holds the keys a used slot's mapping opens under, and no two used slots share an address (the address is the connection identity) |
| `OneSlotPerClientId` | a client id holds at most one slot |
| `SlotMappingBijection` | no slot without a live, established mapping behind it naming that slot, and no established mapping without the used slot it names bound to its address (a slot reset destroys the mapping, so no stale mapping outlives its slot in the 2x map) |
| `NoSlotBeforeResponse` | a live mapping holds a slot only once established: no amount of requests consumes one |
| `MappingsBounded` | the map stays inside its capacity (the 2x map) |
| `OneTokenOneLiveConnection` (alias `OneConnectionPerToken`) | a token holds at most one used slot at a time; it may connect again after release |
| `ChurnNeverDenied` | a fresh, valid token whose client id is free is never refused while a slot is free |
| `ExpiredTokenNeverMapped` | an expired token never creates a mapping |
| `NonceFresh` | no two live mappings share keys (no key or nonce outlives one connection) |
| `StoredChallengeNeverReplaced` | every live mapping holds the one challenge fixed at its creation: the salt and nonce a request sees are the stored ones, never replaced |
| `TimeoutHonoured`, `DeadlineHonoured`, `HalfOpenIdleHonoured` | the idle timeout and the handshake deadline fire when due, for an established mapping and for a half-open one |
| `PacketsReachOwnSlotOnly` | a client's packets never reach another client's slot: a client packet that touches a used slot (its record, or the mapping behind it) came from that slot's address and opened under that mapping's keys; after an address change the client's packets come from an address with no mapping, or another client's, and touch nothing |
| `SessionNeverAnswered` | a session packet never provokes a send (a disconnect is never answered); the keep-alive is timer driven (keep-alive generation reads the clocks, never an arrival) |
| `TypeOK` | types, including both links' |

Liveness, under the fairness described below:

| Property | The rule |
|---|---|
| `Connects` | with a slot free and fair loss, every attempt reaches connected |
| `ServerNotStranded` | a half-open mapping is established or dies; an unacknowledged slot is acknowledged or released |
| `TimedOutSlotFreed` | a timed-out slot is freed: a slot whose client is gone (no client holds its keys in a session state) is eventually released |
| `AbandonedMappingDies` | a half-open mapping whose client is gone dies at its deadline |

### The hang: a packet sent once and lost

The class: a peer moves to a new state, sends the packet announcing it once, the packet is
lost, and nobody ever sends it again. On the server the packets that announce a state are
the cookie, the challenge, the denied and the establishing keep-alive. The first three are
re-provoked by the client's own retransmission (every valid request is answered
every time it arrives), so the server holds state to repeat them (the stored challenge) or
needs none (the cookie and the request-time denied are recomputed). The keep-alive is the
one the server must re-send on its own timer, because the client in state 3 can only repeat
its response, which a connected mapping ignores. `Connects` and `ServerNotStranded` are
checked with every deadline and timeout off, so that only the protocol's own re-sending can
get the client to 4; `RepeatKeepAlive = FALSE` is the reversed witness, the server that sends
the establishing keep-alive once at the response and never again. The code's behaviour is
`RepeatKeepAlive = TRUE` (`rocketnet_server_protocol_write_keep_alive_if_due`,
`sdk/rocketnet_server_protocol.cpp`).

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
 again. That weakens nothing above (a copy on the wire is finite) and is `tla/ReplayWindow.tla`'s
 business. A byte-identical response that refreshes an established slot is in the model as
 the code has it: the refresh comes before the ladder.
- **The tokens** do not expire during a run; `ExpiredTokens` names the tokens that are
 expired from the start. The used-token history binds without eviction under
 `History = "kept"`; `History = "none"` is the flush (an attacker filling the history
 pushes a binding out), so the client-id checks are seen to carry the one-live-connection
 rule alone.
- **The client id** is a function of the token: `IdOf(t) = t`, or `1` for every token with
 `SameId = TRUE` (one player holding two tokens).
- **Payloads and the standalone ack** are not here: a payload confirms a slot and refreshes
 it as a keep-alive does, and neither moves a slot or a mapping. The keep-alive suppression
 clock (a payload stream suppressing the keep-alive) is ClientConnection's witness.
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

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19), from `tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -deadlock -lncheck final \
   -config MCServerConnection.cfg MCServerConnection
```

`MCServerConnection.cfg` is the committed run, a bounded representative instance: two
clients, two tokens either may present (`TokenChoice = "any"`), one slot reused after a
disconnect from either side, and every safety property. To finish inside the runner's
110-second bound it uses `MapCap = 1`, `MaxNonces = 1`, `Timeout = 0`,
`HandshakeDeadline = 0` and a link with `Capacity = 1` and no loss, duplication,
reordering or delay. That bound excludes concurrent half-open mappings and timer-driven
release; the client-id witness keeps two half-open mappings, and the hold witness
exercises timeout and history churn. The four committed configurations, with their
`tla/RUNS.tsv` measurements:

| Configuration | What it checks | Generated / distinct states | Result |
|---|---|---|---|
| `MCServerConnection.cfg` | `SafetyInvariants`, `ChurnNeverDenied`, `ExpiredTokenNeverMapped`, `PacketsReachOwnSlotOnly`, `SessionNeverAnswered` | 554,563 / 116,118 | pass |
| `MCServerConnectionExpiredToken.cfg` | `ExpiredTokenNeverMapped`, one client whose token is expired | 134 / 48 | pass |
| `MCServerConnectionClientIdNoCheck.cfg` | `OneTokenOneLiveConnection`, with `ClientIdCheckAtResponse = FALSE` (reversed witness) | 24,910 / 10,671 | fail as expected |
| `MCServerConnectionHoldUntilExpire.cfg` | `ChurnNeverDenied`, with `HoldUntilExpire = TRUE` (reversed witness) | 159,998 / 60,741 | fail as expected |

The remaining knobs (`SweepRemovesMapping`, `AddressIsIdentity`, `ClientIdCheckAtRequest`,
`RepeatKeepAlive`, `KindLoss`, the link patterns) are set to the code's behaviour in every
committed configuration; the witnesses in "What TLC found" are runs of the committed file
with the knob changed, the instance constants each one names, and the named property
checked. They are run by hand and have no row in `tla/RUNS.tsv`.

### Loss patterns explored

The link supports nothing lost (`"none"`); the first send on each link lost (`"first"`);
every other send lost (`"alternate"`: the first packet dropped, the second through,
forever); sends 1 and 2 lost (`"burst"`); any copy dropped at any time (`"arbitrary"`);
and the first packet of every kind lost at each sender (`KindLoss = "firstkind"`, applied
before the link), with duplication and reordering as separate switches. The committed
configurations run the lossless FIFO link; the byte-identical retransmitted response is in
every run, because the client in state 3 re-sends it on its timer.

## What TLC found

Every counterexample below was replayed by hand through the functions it names before it was
written down. Every invariant the model names holds on the code's behaviour, and every
reversed witness fails as it must. One model error was found and fixed on the way.

### The hang, reproduced (reversed witnesses)

These run the liveness instance: one client, one address, one token, one nonce, every
deadline and timeout off, no disconnects, checking `Connects` and `ServerNotStranded`.

1. **`RepeatKeepAlive = FALSE` under `"firstkind"`** (`W-once-firstkind`): `Connects` and
  `ServerNotStranded` fail with a 28-state lasso. The client's first request is lost
  before the link; the second reaches the server, whose first cookie is lost; the third
  provokes the cookie that arrives (state 11, the client at 2). The request with the
  cookie makes the mapping, and the first challenge is lost; the retransmitted request is
  answered with the stored challenge, byte-identical, which arrives (state 18, the client
  at 3). The response assigns the slot (state 22) and the establishing keep-alive, the
  first of its kind, is lost. From there the lasso is `Tick`, `ClientSend` (the stored
  response), `ServerRecvResponse` (already connected: the idle clock refreshed, nothing
  sent). The server never sends another keep-alive; the client waits in 3 for a packet
  nobody will send again, and the slot is never acknowledged. Replayed:
  `rocketnet_server_process_connection_response` refreshes `last_receive_time` and
  ignores the retransmission as already connected; what the witness removes is
  `rocketnet_server_protocol_write_keep_alive_if_due`, which the code runs every update
  off `last_keep_alive_send_time` until the client's block acknowledges one.
2. **`RepeatKeepAlive = FALSE` under `"alternate"`** (`W-once-alternate`): the same hang
  with the loss coming from the link on both directions (38 states, lasso from 29). The
  client's odd sends are lost: requests 1, 3, 5, 7 and 9, so the cookie is provoked by
  send 2 and is the server's first send, lost; send 4 provokes the server's second, the
  cookie, which arrives; send 6 makes the mapping and the challenge is the server's third
  send, lost; send 8 draws the stored challenge, the fourth, which arrives; send 10 is the
  response, and the keep-alive is the server's fifth send, lost. The lasso is two ticks and
  two responses, every other one lost, each arriving one ignored as already connected.
3. The code's behaviour passes under every pattern: the stored challenge answers every
  retransmitted request, the cookie and the request-time denied are recomputed on every
  arrival (`rocketnet_server_protocol_process_connection_request`), and the keep-alive is
  the one packet the server re-sends on its own timer. Under `"arbitrary"` the one-client
  instance finishes under `SpecTyped` in two minutes, where ClientConnection's instance
  does not; the state space is a fifth of that one's.

### The slot, mapping and token witnesses (reversed, each expected to fail)

4. **`SweepRemovesMapping = FALSE`** (`SlotMappingBijection`, 14 states): one client
  connects (request, challenge, response, slot 1 assigned), then two ticks with nothing
  heard; at `Timeout = 2` the sweep releases the slot and leaves the mapping live,
  established and naming slot 1, which is free: a stale mapping in the 2x map. Replayed:
  the code's sweep in `rocketnet_server_protocol_update` releases the slot and removes the
  mapping together; the witness drops the removal. With the code's behaviour the
  bijection holds in every state of every run, and `MappingsBounded` with it.
5. **`HoldUntilExpire = TRUE`** (`MCServerConnectionHoldUntilExpire.cfg`,
  `ChurnNeverDenied`): the trace seats then times out token 1, seats then times out
  token 2 while the history keeps both, and finally presents a fresh token 3 while a slot
  is free. A cap on tokens held until expiry refuses token 3 though no connection is live;
  the code's history only binds a token to an address, so token 3 is accepted. The witness
  uses `MapCap = 1`, `Capacity = 1`, `Timeout = 1` and no application disconnects to keep
  the timeout and history sequence small.
6. **`ClientIdCheckAtResponse = FALSE`** (`MCServerConnectionClientIdNoCheck.cfg`,
  `OneTokenOneLiveConnection`): two clients at two addresses present one token, and both
  requests create half-open mappings before either response arrives. The first response
  binds the token and takes a slot; without the response-side client-id check the second
  response takes the other slot too. Replayed: `rocketnet_server_process_connection_response`
  looks the client id up with `rocketnet_client_slots_find` after the request race, and
  refuses the second seat as already connected.
7. **`AddressIsIdentity = FALSE`** (`PacketsReachOwnSlotOnly`, 16 states): a client gets
  its challenge at address 1, moves to address 2, and sends its response and then a
  disconnect from there. The witness server opens each under whichever live mapping its
  keys fit: the response seats the client with the mapping at address 1, and the
  disconnect from address 2 releases that slot, a packet from one address acting on a slot
  bound to another. Replayed: the code looks a sealed packet up by its source address only
  (`rocketnet_encryption_map_find(server->encryption_map, from)` in
  `sdk/rocketnet_server_protocol.cpp`); a response from address 2 finds no mapping and is
  `NO_MAPPING`, and the slot at address 1 is left to its idle timeout. With the code's
  behaviour the property holds in every safety run.

### Model errors, fixed

8. **The salt keyed on the attempt number.** The first draft's key pair was
  `[att, nonce]` with `att` each client's own attempt counter, as ClientConnection has it
  for its one client. TLC's first full run (19 states, `OneClientPerSlot`) had two clients
  present the same token with attempt number 1 each: the first got a mapping and its
  challenge, which the link duplicated; the first client moved away and the second moved
  onto the vacated address; the duplicate challenge, addressed to that address and
  carrying attempt number 1, was accepted by the second client, which committed the first
  client's keys and then completed the first client's handshake from the first client's
  address. Two clients held one slot's keys. Replayed against the code: the challenge is
  sealed under handshake keys derived from the token keys, the 64-bit random
  `connection_salt` and the session nonce (`rocketnet_derive_connection_keys`,
  `sdk/rocketnet_keys.cpp`); two clients sharing a token share the token keys, and
  nothing else, so the second client cannot open the first's challenge. The model's salt
  was too small, not the code. The salt is the pair `<<client, attempt>>`, which no two
  clients share, and the same instance passes.

## The RUNS.tsv rows

The model's measured rows are the four `MCServerConnection*.cfg` rows of `tla/RUNS.tsv`,
summarised in the table under "How to run it"; `tla/README.md` describes the runner that
produces them.

# ServerConnection: the server's slots and encryption mappings

`tla/ServerConnection.tla` models the server side of the connection. The server is
`sdk/rocketnet_server_protocol.cpp`, driven by `rocketnet_server_transport_update`
(`sdk/rocketnet_server_transport.cpp`). The model includes the tables the server keys
on:

- the encryption map (`sdk/rocketnet_encryption_map.cpp`): one mapping per source
  address, capacity `2 x max_clients`, and a LIFO free list of indices;
- the client slots (`sdk/rocketnet_client_slots.cpp`): the lowest free slot, and the
  client-id index;
- the used-token history and the admitted-token set (`sdk/rocketnet_token_tables.cpp`).

Several clients talk to this server over two instances of the shared lossy link
`tla/Link.tla`. `C2S` carries everything the clients send and `S2C` everything the
server sends. Every packet names an address: its source on `C2S`, its destination on
`S2C`. `tla/MCServerConnection.tla` and `tla/MCServerConnection.cfg` are the small
instance. Every TLC run is a row in `tla/RUNS.tsv`. The inventory this card worked from
is `docs/TLA-INVENTORY.md`, sections M2 and M3, with the token tables of M6 that the
connection path calls.

## What the model covers

**The server's state** (the variables the code owns):

| Variable | Code | Meaning |
|---|---|---|
| `map` | `rocketnet_encryption_map_t`, `rocketnet_encryption_mapping_t` | per index: `"none"`, `"half"` (`client_index = -1`) or `"est"`; the address, the keys, the token and its client id, the slot, the handshake age (`handshake_deadline`, fixed at creation), the idle age (`last_receive_time`) |
| `free` | `free_head` / `next` | the free list, head first; `remove` pushes, `add` pops |
| `slots` | `rocketnet_client_slots_t` | per slot: held, `mapping_index`, `client_id`, and the keep-alive-acknowledged latch (`keep_alive_acked`, reset with the slot at `rocketnet_server_acks_reset_slot`) |
| `kaDue` | `last_keep_alive_send_time`, `last_send_time` | the keep-alive interval has elapsed for this slot |
| `history` | the used-token history | `[tok, addr]` entries, oldest first, capacity `HistCap` |
| `admitted` | the admitted-token set | capacity `AdmCap`; nothing is ever evicted |
| `sburst` | `rocketnet_server_protocol_disconnect_client` | the redundant disconnect burst still to send |

**The clients**, as the server sees them. Each client has a state (0, 1, 2, 3, 4, -1
denied, -5 timed out, -9 vanished), an attempt number, its current address, a cookie
flag, its committed keys, the slot index learned from a keep-alive, and its own
disconnect burst. The client's own machine is `ClientConnection`'s; here it is only
the scheduled sends of each state and what each server packet does to it.

**Ghosts** (no code reads them): `admits` (connections admitted per token), `misrouted`
(a client's packet was accepted for a mapping that is not its own), the ledger counters
(`CLIENT_CONNECTED`, `CLIENT_DISCONNECTED`, `CLIENT_TIMED_OUT`), `lost` (loss-pattern
bookkeeping), `captured` (responses an attacker has seen), and `starved` (the model's
nonce pool ran dry).

**Actions.** There is one action per operation and per outside event. Each is cited to
its function in the module.

| Action | Who | Code |
|---|---|---|
| `Connect(c)` | client application | `rocketnet_client_protocol_connect`: a fresh attempt, salt and token |
| `ClientSend(c)` | client timer | request in 1 and 2 (with the cookie in 2), the stored response in 3, keep-alive in 4 |
| `ClientDisconnect(c)`, `ClientBurst(c)` | client application | `rocketnet_client_protocol_disconnect`: the burst from 3 or 4, then 0 |
| `Vanish(c)` | the outside | the client is gone without a word |
| `Move(c)` | the outside | NAT rebinding: the client's address changes once, to any address no present client holds, including one a departed client used |
| `ClientRecvHead` | a packet arrives | the client at the packet's address, if any: cookie, challenge, denied, keep-alive, disconnect, each with the gate and key check the client code applies |
| `ServerRecvHead` → `ServerRecvRequest` | a packet arrives | `rocketnet_server_protocol_process_connection_request`, in the code's order: an established address ignored (`:91-101`); a half-open mapping resends its stored challenge (`:102-106`); no cookie → cookie; the history check (`:283`); client id connected (`:345`); token admitted (`:352`); record (`:359`); no free slot → denied, no mapping (`:395-412`); table full → ignored (`:415-423`); otherwise a half-open mapping at `Head(free)` and its challenge |
| `ServerRecvHead` → `ServerRecvResponse` | a packet arrives | opens only under the mapping at its source address, then `last_receive_time` is refreshed (`:781`); `rocketnet_server_process_connection_response`: already connected → ignored (`:491`); client id connected → ignored (`:498`); no slot (`:514`) or admitted set refuses (`:521`) → denied, mapping kept; otherwise the lowest free slot, admitted, and the first keep-alive (`:546-576`) |
| `ServerRecvHead` → `ServerRecvSession` | a packet arrives | the sealed ladder: no mapping or does not open → ignored; half-open → ignored (`:791-801`); a keep-alive refreshes the slot (`:843`) and its ack block sets the latch; a disconnect releases the slot and removes the mapping (`:938-942`) |
| `ServerKeepAlive(i)` | server timer | `rocketnet_server_protocol_write_keep_alive_if_due`, from the maintenance loop (`rocketnet_server_transport.cpp:1151-1172`), to the mapping's address |
| `ServerDisconnect(i)`, `ServerBurst(a)` | server application | `rocketnet_server_protocol_disconnect_client`: the burst, then the slot and mapping are gone at once |
| `Replay` | an on-path attacker | re-injects a captured response from its source address (handshake types are exempt from replay protection) |
| `Tick` | time | one send interval on every end: the clients' idle timeout; `rocketnet_server_protocol_update` walks the map in index order, removing a half-open mapping at its handshake deadline and an established one at the idle timeout with its slot (`:1023-1060`), each removal pushing its index on the free list; both links `Age` |
| `C2SEnvironment`, `S2CEnvironment` | the path | Link's `Drop`, `Duplicate`, `Reorder`, `Delay` |

Line numbers are in `sdk/rocketnet_server_protocol.cpp` at `c4f9a8c` unless the row
names another file.

Every packet that drives a transition crosses a Link instance. Sends are `Link!Send`,
every receive is `Link!Deliver` of the wire's head, and `Tick` is `Link!Age`. Every
packet has a receive step in every state: a packet the gate or the keys refuse is
delivered and discarded, and a packet to an address with no client there is delivered
to no one. So the links' fairness is never vacuous (README-Link.md, caution 1).

### Properties

Safety, checked in every run with no fairness:

| Property | The rule |
|---|---|
| `SlotMappingBijection` | no slot without its mapping while live, no established mapping without its slot, a half-open mapping holds none (S:2829, S:2852). This is the leak the audits suspect at the 2x mapping table. |
| `ReleaseRemovesMapping` | a released slot takes its mapping with it, in the same step: no mapping is left without a slot after a disconnect, a timeout or a server disconnect |
| `FreeListExact` | the map's free list is exactly the free indices, each once, so a released index is reused immediately and never twice |
| `AddressIdentity` | one mapping per address (STANDARD.md, "The client address is the connection identity") |
| `NoTwoClientsInOneSlot` | one slot names one mapping, and one client id holds at most one slot (S:2647-2649); at most one connected client holds the keys of the mapping a slot names |
| `NoCrossDelivery` | a client's packets are never accepted for another client's mapping or slot, after an address change or otherwise |
| `OneAdmissionPerToken` | the same token never admits two connections (S:2647-2649 "a token admits one connection", S:3591 "a connect token buys exactly one connection") |
| `Ledger` | `CLIENT_CONNECTED = CLIENT_DISCONNECTED + CLIENT_TIMED_OUT + num_connected` (the comment at `rocketnet_server_protocol.cpp:591`) |
| `ModelBoundsOK` | the model's nonce pool never ran dry, so the model's bound never stood in for the code |
| `TypeOK` | types, including both links' |

`DeniedMeansNoSlot` (a client told it is denied holds no slot) is checked in a run of its
own. It is expected to fail (inventory F5).

Liveness, under the fairness below:

| Property | The rule |
|---|---|
| `Connects` | every attempt resolves: connected, or denied |
| `ServerNotStranded` | every held slot's establishing keep-alive is acknowledged, or the slot is released; every half-open mapping leaves half-open |
| `SlotFreed` | a timed-out slot is freed: a slot whose client has ended the attempt, vanished or moved away is released |
| `HalfBounded` | a half-open mapping leaves half-open (by its handshake deadline) |

### The hang the request named

The class is this: a peer moves to a new state, sends the packet that announces it
once, the packet is lost, and nobody ever sends it again. `Connects` and
`ServerNotStranded` together say that neither peer is ever left waiting for a packet
nobody will send again. They are checked with every timeout and deadline off, so only
the protocol's own re-sending can finish the handshake. A deadline would otherwise turn
a hang into a tidy timeout. The standard states the same rule at S:2470-2473: "every
wait state retransmits, and everything a server sends, it can send again."

On the server, every packet that announces a transition is re-sent until the client's
next packet shows it was seen:

| Server transition | Announced by | Re-sent until | Code |
|---|---|---|---|
| none → (cookie issued) | cookie | a request carrying it; every request without one provokes a fresh one | `:213-275` |
| none → half-open | challenge | a response opens; every retransmitted request gets the stored challenge byte-identical | `:93-106` |
| (full) | denied, at the request | every retransmitted request is denied again; no mapping is made | `:395-412` |
| half-open, denied at the response | denied | every retransmitted response is denied again; the mapping is kept | `:512-544` |
| half-open → established | keep-alive (the client's index) | the client acknowledges one, on the keep-alive's own clock | `:552-576`, `:1240-1319` |

Two constants reproduce the class as reversed witnesses:

- `RepeatKeepAlive = FALSE`: the establishing keep-alive is sent once, at the response,
  and not again until the client acknowledges one.
- `ResendChallenge = FALSE`: a half-open mapping stays silent to a retransmitted
  request.

The code's behaviour is `TRUE` for both.

## What the model abstracts away

- **Keys.** A key pair is `[att, nonce]`. The attempt's salt and the mapping's session
  nonce are the two fresh inputs every key is derived from, so "opens under the
  mapping's keys" is equality on that pair. A nonce is fresh: no live mapping, client,
  burst, captured packet or copy in flight holds it. The AEAD, the small-order check,
  the integrity limit (2^36 failed opens, `rocketnet_server_integrity_failure`) and the
  ephemeral keypairs are not modelled. A packet that does not open is discarded.
- **The cookie** is a flag on the request. The cookie MAC, its 3 s lifetime and key
  rotation are the security module's business (F2).
- **Tokens never expire** within a run. Every entry in the history and the admitted
  set outlives the run, so the history evicts only the oldest entry at capacity and
  the admitted set only refuses. This is the case the tables have to get right without
  expiry's help. A token is an attempt id: each attempt presents its own, except under
  `SharedToken`, where client 2's second attempt presents client 1's first token. A
  token's client id is the client it was issued to.
- **Sequence numbers and the replay window.** Client session packets carry no sequence,
  so a duplicated keep-alive copy can refresh a slot where the code's replay window
  would refuse it. This only over-approximates (more refreshes). The liveness runs have
  duplication off, so it cannot keep a slot alive there. The response is exempt from
  replay protection in the code as well, which is what `Replay` uses.
- **The ack block** is one bit: a client keep-alive accepted for a slot sets the latch.
  A client in 4 has processed a keep-alive, so its block acknowledges one. Payloads, the
  standalone ack and the #90 payload-stream starvation are `ClientConnection`'s
  (`KeepAliveClock`). Here, after the latch the keep-alive is still due every interval.
- **Time** is a tick of one send interval on every end, with saturating counters. A
  `Timeout`, `HsDeadline` or `ClientTimeout` of 0 is off. The code's handshake deadline
  (5 s, `rocketnet_server_transport.cpp:794`) and its idle timeout cannot be off (the
  token's timeout must be positive, `:157`).
- **The confirmed flag** (`rocketnet_client_slots_confirm`) moves nothing in the
  connection machine and is not modelled. The client-id hash chains are modelled as the
  set they implement.
- **The client** is reduced to what the server sees. Client timeouts all land in -5;
  their numbering is `ClientConnection`'s.

## Fairness

`Spec` is `Init /\ [][Next]_vars /\ Fairness`. This is the restatement
`ClientConnection` uses (README-ClientConnection.md, "Fairness"):

- weak fairness on `Tick`, each client's `Connect` and `ClientSend`, each slot's
  `ServerKeepAlive`, and both bursts;
- strong fairness on `ClientSendThrough(c)` and `ServerKeepAliveThrough(i)`: a
  timer-driven send that is enabled infinitely often with the pattern letting it
  through goes through infinitely often. This is Link's `EventualDelivery` ("a packet
  re-sent often enough eventually arrives"), stated on the sender;
- strong fairness on delivering the head of each wire, which is Link's `WireFair` on the
  head.

Nothing obliges an application to disconnect, a client to vanish or move, or the
attacker to act. The liveness runs use a FIFO link without latency or duplication, for
the reasons `ClientConnection` records. The safety runs turn all three on.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19), from `tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 8 -cleanup -deadlock \
     -config MCServerConnection.cfg MCServerConnection
```

`MCServerConnection.cfg` is the committed run, the hang check. It has two slots, two
clients, the code's re-sending, and `KindLoss = "firstkind"` (the first packet of every
kind of every connection, each way, is lost). Every timeout and deadline is off and
nobody leaves. It checks every safety property plus `Connects` and `ServerNotStranded`.
The other runs are the same file with the lines below changed; the TSV has one row per
run.

| Run | Lines changed from the committed cfg | Checks beyond the safety set | Distinct states | Result |
|---|---|---|---|---|
| `L-firstkind` | committed | OneAdmissionPerToken, ReleaseRemovesMapping, Connects, ServerNotStranded | 295448 | pass |
| `L-none` | KindLoss "none" | OneAdmissionPerToken, ReleaseRemovesMapping, Connects, ServerNotStranded | 82776 | pass |
| `L-first` | KindLoss "none", C2SPattern "first", S2CPattern "first" | OneAdmissionPerToken, ReleaseRemovesMapping, Connects, ServerNotStranded | 82519 | pass |
| `L-burst` | KindLoss "none", C2SPattern "burst", S2CPattern "burst" | OneAdmissionPerToken, ReleaseRemovesMapping, Connects, ServerNotStranded | 82491 | pass |
| `L-altconn` | KindLoss "alternate" | OneAdmissionPerToken, ReleaseRemovesMapping, Connects, ServerNotStranded | 894694 | stopped by hand at depth 63, no violation |
| `L-alternate` | KindLoss "none", C2SPattern "alternate", S2CPattern "alternate" | OneAdmissionPerToken, ReleaseRemovesMapping, Connects, ServerNotStranded | 22681 | fail, model error (8) |
| `W-once` | RepeatKeepAlive FALSE | Connects, ServerNotStranded | 42280 | fail as expected (1) |
| `W-nochallenge` | ResendChallenge FALSE | Connects | 8254 | fail as expected (2) |
| `W-once-altconn` | KindLoss "alternate", RepeatKeepAlive FALSE | Connects | 393619 | fail as expected (3) |
| `W-nochallenge-altconn` | KindLoss "alternate", ResendChallenge FALSE | Connects | 60999 | fail as expected (3) |
| `W-once-alternate` | KindLoss "none", RepeatKeepAlive FALSE, C2SPattern "alternate", S2CPattern "alternate" | Connects | 70254 | fail; not a witness (8) |
| `L-timeout` | NumAddrs 3, Timeout 2, HsDeadline 2, ClientTimeout 3, ClientDisconnects TRUE, ServerDisconnects TRUE, Vanishes TRUE, Moves TRUE, MaxNonces 3 | OneAdmissionPerToken, ReleaseRemovesMapping, SlotFreed, HalfBounded | 1064645 | pass |
| `W-attacker1` | NumClients 1, NumAddrs 1, Slots 1, MapCap 2, AdmCap 2, HistCap 8, Timeout 2, HsDeadline 2, ClientTimeout 3, Vanishes TRUE, Attacker TRUE, MaxNonces 6, KindLoss "none" | OneAdmissionPerToken, SlotFreed | 14306 | fail as expected (6, F7) |
| `F9-evict` | KindLoss "none", SharedToken TRUE, HistCap 1, ClientDisconnects TRUE, MaxAttempts 2, MaxNonces 3 | OneAdmissionPerToken | 3222064 | fail as expected (4, F9) |
| `F9-noevict` | KindLoss "none", SharedToken TRUE, HistCap 4, ClientDisconnects TRUE, MaxAttempts 2, MaxNonces 3 | OneAdmissionPerToken, ReleaseRemovesMapping | 6984515 | pass |
| `F5-denied` | KindLoss "none", Slots 1, MapCap 2, AdmCap 2, ClientDisconnects TRUE, MaxNonces 3 | DeniedMeansNoSlot | 150414 | fail as expected (5, F5) |
| `O-hsdeadline` | HsDeadline 2, MaxNonces 6 | Connects | 297279 | fail as expected (7) |
| `W-leak` | MaxNonces 3, Timeout 2, Vanishes TRUE, KindLoss "none", ReleaseMapping FALSE | safety set only | 2882 | fail as expected: SlotMappingBijection (reversed witness: the mapping left behind) |
| `W-nokey` | NumAddrs 3, MaxNonces 6, Timeout 2, HsDeadline 2, ClientTimeout 3, Vanishes TRUE, Moves TRUE, KindLoss "none", KeyCheck FALSE | safety set only | 13529866 | fail as expected: NoCrossDelivery (reversed witness: no key check) |
| `S-tiny` | Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxNonces 6, Timeout 1, HsDeadline 1, ClientTimeout 2, DisconnectCopies 1, ServerDisconnects TRUE, Vanishes TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication FALSE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 5280488 | pass |
| `S-arb-min` | Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxNonces 3, Timeout 1, HsDeadline 1, ClientTimeout 2, DisconnectCopies 1, ClientDisconnects TRUE, ServerDisconnects TRUE, Vanishes TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 1440278 | stopped on ModelBoundsOK (9) |
| `S-arb-min6` | Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxNonces 6, Timeout 1, HsDeadline 1, ClientTimeout 2, DisconnectCopies 1, ClientDisconnects TRUE, ServerDisconnects TRUE, Vanishes TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 83988201 | stopped by hand at depth 30, no violation |
| `S-moves-min` | NumAddrs 3, Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxNonces 3, Timeout 1, HsDeadline 1, ClientTimeout 2, DisconnectCopies 1, Vanishes TRUE, Moves TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 5849125 | stopped on ModelBoundsOK (9) |
| `S-moves-min6` | NumAddrs 3, Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxNonces 6, Timeout 1, HsDeadline 1, ClientTimeout 2, DisconnectCopies 1, Vanishes TRUE, Moves TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 36825893 | stopped by hand at depth 24, no violation |
| `S-moves-1` | NumAddrs 3, Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxNonces 4, Timeout 2, HsDeadline 2, ClientTimeout 3, DisconnectCopies 1, ClientDisconnects TRUE, ServerDisconnects TRUE, Vanishes TRUE, Moves TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 111118313 | stopped by hand at depth 23, no violation |
| `S-moves-2` | NumAddrs 3, MaxNonces 4, Timeout 1, HsDeadline 1, ClientTimeout 2, DisconnectCopies 1, ClientDisconnects TRUE, ServerDisconnects TRUE, Vanishes TRUE, Moves TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 53712036 | stopped by hand at depth 25, no violation |
| `S-reuse-1` | Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxAttempts 2, MaxNonces 4, Timeout 2, HsDeadline 2, ClientTimeout 3, DisconnectCopies 1, ClientDisconnects TRUE, ServerDisconnects TRUE, Vanishes TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 113423806 | stopped by hand at depth 24, no violation |

Every row is a run of this tip on vision (TLC 2.19, 2026-10-06), recorded in
`tla/RUNS.tsv`. The numbers in brackets point at "What TLC found". "Stopped by hand"
means a breadth-first run that was stopped (SIGTERM) because the card ran out of time.
No violation had been found by then: every behaviour shorter than the depth reached
was checked, but the instance was not exhausted.

`S-arb-min` and `S-moves-min` stopped on `ModelBoundsOK` with three nonces, which is
the model's bound and not the code (see 9). `S-arb-min6` and `S-moves-min6` are the
same runs with six nonces.

The small instance: two clients, two slots, `MapCap = 4` (`2 x max_clients`, as the
code), `AdmCap = 4` (`2 x max_clients`), `HistCap = 16` (`8 x max_clients`),
`DisconnectCopies = 2`, `Capacity = 2`, and a FIFO link without latency in the liveness
runs. The `S-*` safety runs turn on `"arbitrary"` loss on both links, with reordering
and (except `S-tiny`) duplication. They also turn on the ways a client can leave: its
own disconnect, the server's, vanishing, and (in `S-moves-*`) an address change onto an
address a departed client used. `S-reuse-1` gives each client a second attempt, so a
released slot and a released mapping index are reused by a new connection while copies
of the old one are still in flight. Most of them use one slot (`MapCap = 2`) so that
the instance can finish. Of these, `S-tiny` (no duplication, no client disconnect, no
moves) is the one that exhausted its state space. The two-slot and duplicating ones
are bounded (see the table).

### Loss patterns explored

Across the runs TLC explored:

- nothing lost (`"none"`);
- the first send on each link lost (`"first"`);
- sends 1 and 2 on each link lost (`"burst"`);
- any copy dropped at any time, with reordering and (except `S-tiny`) duplication
  (`"arbitrary"`, in the `S-*` runs; exhausted in `S-tiny`, bounded in the others);
- the first packet of every kind of every connection lost, each way
  (`KindLoss = "firstkind"`, the committed run);
- every other packet of every connection lost, the first included
  (`KindLoss = "alternate"`, "the first packet dropped, the second through", forever).

Reordering is on in every `S-*` run, so an old packet arriving late is in the safety
checks; a duplicated copy is in all but `S-tiny`. An attacker's re-injected response is
in `W-attacker1`. The liveness runs use a FIFO link (see "Fairness").

## What TLC found

Every counterexample below was replayed by hand through the functions it names before
it was written down. Line numbers are at `c4f9a8c`.

### The hang, reproduced (reversed witnesses)

1. **`RepeatKeepAlive = FALSE` under `"firstkind"`** (`W-once`): `Connects` fails with a
   42-state lasso. Both clients reach 3 and the server assigns both slots. Each
   establishing keep-alive is the first of its kind for its connection and is lost. From
   state 38 the lasso is `Tick`, both clients resend their stored responses, and the
   server ignores them as "client is already connected" (`:491-496`). No keep-alive is
   ever sent again, so both clients wait in 3 for a packet nobody will send again. With
   the code's behaviour the same instance passes: `L-firstkind`, the committed run.
2. **`ResendChallenge = FALSE` under `"firstkind"`** (`W-nochallenge`): a 33-state lasso
   from state 23. Both challenges are lost. Each client's retransmitted request reaches
   a half-open mapping that stays silent, and the clients sit in 2. In the code that
   request gets the stored challenge byte-identical (`:93-106`).
3. **The same two under "every other packet dropped"** (`W-once-altconn`, 57 states;
   `W-nochallenge-altconn`, 44 states): the same hangs, with the loss coming from
   per-connection alternation.

Every packet the server sends to announce a transition is re-sent until the client's
next packet shows it was seen (the table above). The code passes `Connects` and
`ServerNotStranded` exhaustively under "first packet of every kind dropped" (the
committed run), "nothing lost", "first" and "burst". Under "every other packet dropped"
(`L-altconn`) the run was stopped at depth 63 before it finished. TLC's periodic
liveness check over the 6.58 million product states it had reached at that point found
no violation, but the run is a bound, not a proof. Both witnesses under the same pattern
fail within 57 and 44 states. Reading the code, no server transition in this machine is
announced once and then left. The one packet sent without re-sending is the disconnect
burst (ten copies, then the slot and mapping are gone, `:1627-1680`). If every copy is
lost, the peer waits for its idle timeout. S:3135-3139 states that as the design
("losing all ten costs only a peer that waits for the timeout"), and `SlotFreed` checks
the server's side of it (`L-timeout`).

### Findings in the code (each a line in the report, none a code change here)

4. **F9 is reachable: one token admits two connections** (`F9-evict`,
   `OneAdmissionPerToken`, 41 states; `SharedToken`, `HistCap = 1`). The trace:
   - Client 1 requests with token T from address 1. T is recorded and a half-open
     mapping is made.
   - Client 2's first, ordinary attempt is recorded, and the history at capacity
     evicts T (`rocketnet_used_token_history_record`, `token_tables.cpp:244`,
     `select_slot` evicting the oldest at `:183-187`). Client 2 connects and then
     disconnects.
   - Client 2's second attempt presents T from address 2. The history check passes
     (`:283`; `rocketnet_used_token_history_check` returns 1 for an absent token,
     `token_tables.cpp:226-228`). Client id 1 is not connected (`:345`: client 1 is
     still half-open). T is not admitted (`:352`). So a second half-open mapping holds T.
   - Client 1's response is admitted (`:521`) and assigned. Then client 1 disconnects
     (`:938-942`, which leaves the admitted entry in place).
   - Client 2's response passes the client-id check (`:498`) and the slot check, and
     `rocketnet_admitted_token_set_add` returns success because T is already present
     (`token_tables.cpp:277-278`). T has now admitted its second connection.

   Replayed by hand through those functions: confirmed. This breaks S:3591 ("a connect
   token buys exactly one connection") and S:2647-2649. The client-id check at `:498` only
   stops the two connections from overlapping in time. In the code the precondition is
   `8 x max_clients` other tokens recorded (`rocketnet_server_transport.cpp:799`) while
   the first holder is still half-open (inside the 5 s handshake deadline). That is the
   flush S:2784 already names for the address binding, and here it defeats the admission
   rule as well. `F9-noevict` (the same instance, with a history that never evicts)
   passes, so eviction is the cause. The fix shape is to make an add that finds the
   token present refuse at the response (deny), or to re-run `contains` there.
5. **F5 is reachable: a denied client's slot is held until the idle timeout**
   (`F5-denied`, `DeniedMeansNoSlot`, 29 states, one slot, a FIFO link with no
   reordering). The trace:
   - Client 1 holds the slot. (It got there from a response still in flight after its
     application had disconnected from 3; its own disconnect copy, which follows, will
     free the slot.) Client 2 is in 3, and its response is denied for no slot
     (`:514-544`, the mapping kept).
   - Client 1's disconnect frees the slot (`:938-942`).
   - Client 2 sends its next scheduled response before the denied reaches it.
   - The denied arrives and client 2 goes to -1 (`client_protocol.cpp:614`).
   - Client 2's response still in flight is accepted and assigned the free slot
     (`:546-576`). The keep-alive that follows meets a client at -1 and is ignored.

   Replayed: confirmed. The server holds a slot, an established mapping and an admitted
   token for a client that has given up, until the idle timeout. `SlotFreed` in
   `L-timeout` shows the timeout does free it. The inventory classes this as a question
   for the maintainer; the trace stands as its answer to "is it reachable".
6. **F7 is reachable: a replayed response keeps a departed client's slot alive forever**
   (`W-attacker1`, `SlotFreed`, 26 states with the lasso from 17; one client, the idle
   timeout on, `Attacker`). The client reaches 3, sends its response, and vanishes. An
   on-path attacker re-injects the captured response from the client's address. That
   response completes the handshake for the departed client (state 14). From then on
   each replay opens under the mapping's handshake key, because handshake types are
   exempt from the replay window (`:713-730`; S:1475). The opened response refreshes
   `last_receive_time` (`:781`) and is then ignored as already connected (`:491`), so
   the idle timeout (`:1049-1058`) never fires. Replayed: confirmed. This is the
   inventory's F7. S:3089-3091 intends the refresh for the client's own retransmissions. It
   needs an attacker on the path who can spoof the client's address. Without one,
   `SlotFreed` holds (`L-timeout`).
### Observations (bounded waits, not the hang)

7. **The handshake deadline outruns a client in state 3** (`O-hsdeadline`, `Connects`,
   48 states with the lasso from 39; the server's handshake deadline on, client timeout
   off). Both clients accept their challenges. Their first responses are lost, and the
   half-open mappings reach the handshake deadline and are removed (`:1038-1046`). From
   then on each client's retransmitted response finds no mapping and is ignored
   (`:705`), so the client waits in 3 for a keep-alive the server will never send. The
   server cannot answer: it has no keys for that address any more. In the code the
   client always has its own response timeout (`timeout_seconds > 0`, enforced at
   `:157`) and total deadline (`client_protocol.cpp:201-221`), so this ends in -4. It is
   a bounded wait, not a wait without end. It does contradict the letter of
   S:2470-2473 ("There is no packet whose loss leaves either end waiting for something
   that will never be sent again"), which names the timeout as the backstop only for
   the disconnect. Under heavy loss a handshake that runs past the server's 5 s
   deadline fails this way, at the client's response timeout, with no packet saying
   so. Whether that needs a sentence in the standard or a change in the deadline is for
   the maintainer.

### Model errors, fixed

8. **Link's `"alternate"` on a shared link.** `L-alternate` (the code's behaviour) failed
   `Connects` and `ServerNotStranded` with a 49-state lasso (the loop from state 28).
   Two clients share each link, and the Link pattern counts every send on the link.
   In the loop client 1 is connected and client 2 is stuck in 1: its requests get
   through, but the server's keep-alives to client 1 take the even (delivered) indices
   of `S2C`, and every cookie reply to client 2 lands on an odd (lost) one. Replayed through
   Link's `NextCount` and `LostAtSend`: every step is as written. This is the
   correlation README-Link.md, caution 2, warns of: one sender's retries landing
   exactly on the pattern's losses. The real path does not do it. "Every other packet
   dropped" is therefore checked per connection, as `KindLoss = "alternate"` (each
   `<<side, attempt>>` stream loses its 1st, 3rd, 5th ... send). `W-once-alternate`,
   the witness on the shared link, fails for the same reason the code does and is not
   counted as a witness.
9. **The nonce pool.** `S-arb-min` and `S-moves-min` failed `ModelBoundsOK` with three
   nonces (19 states each). After a 1-tick handshake deadline, stale request copies
   still in flight re-create mappings for an attempt whose client has already left, each
   at a fresh nonce. That is the code's behaviour too (a request for an address with no
   mapping makes one, `:415-470`); it ends at the next handshake deadline. The pool was
   too small for it. `MaxNonces = 6` covers what can be in use at once in those
   instances (`S-arb-min6`, `S-moves-min6`). An earlier attempt at this card hit the same
   bound in `O-hsdeadline` and `W-attacker` and sized them the same way.
10. **Instance size.** With `"arbitrary"` loss, duplication and reordering on both links,
    two clients and every departure, the state space grows faster than this card's time
    allowed: `S-moves-1`, `S-reuse-1`, `S-arb-min6`, `S-moves-2` and `S-moves-min6`
    were still widening at 37 to 113 million distinct states. They were stopped by hand
    with no violation found (the table gives the depth reached). `S-tiny` drops
    duplication, client disconnects and moves, and it finished.

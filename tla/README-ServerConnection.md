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
the reasons `ClientConnection` records. The safety run `S-small` turns on arbitrary
loss, duplication and reordering. `MaxDelay` is 0 in every run. For safety that loses
nothing: no action forces a copy off the wire before a `Tick`, so any copy may already
stay in flight across any number of ticks and arrive after a timeout, a release or a
reuse. Link's `Delay` and `Age` only take deliveries away.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19), from `tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -deadlock \
     -config MCServerConnection.cfg MCServerConnection
```

`MCServerConnection.cfg` is the committed run, the hang check. It has two slots, two
clients, the code's re-sending, and `KindLoss = "firstkind"`: the first packet of every
kind of every connection is lost, in each direction. Every timeout and deadline is off
and nobody leaves. It checks every safety property plus `Connects` and
`ServerNotStranded`. Each other run is the same file with the constants below changed
and its own INVARIANTS and PROPERTIES. The TSV has one row per run.

`SafetyInvariants` (in `MCServerConnection.tla`) is `TypeOK`, `ModelBoundsOK`,
`FreeListExact`, `AddressIdentity`, `SlotMappingBijection`, `NoTwoClientsInOneSlot`,
`NoCrossDelivery` and `Ledger`. Every run checks it.

| Run | Constants changed from the committed cfg | Checks beyond `SafetyInvariants` | Distinct states | Result |
|---|---|---|---|---|
| `L-firstkind` | committed | OneAdmissionPerToken, ReleaseRemovesMapping, Connects, ServerNotStranded | 295448 | pass |
| `L-none` | KindLoss "none" | as committed | 82776 | pass |
| `L-first` | KindLoss "none", C2SPattern "first", S2CPattern "first" | as committed | 82519 | pass |
| `L-burst` | KindLoss "none", C2SPattern "burst", S2CPattern "burst" (sends 1-2 lost) | as committed | 82491 | pass |
| `L-altconn1` | NumClients 1, NumAddrs 1, Slots 1, MapCap 2, AdmCap 2, HistCap 8, KindLoss "alternate" | as committed | 3390 | pass |
| `L-altconn` | KindLoss "alternate" | as committed | 707117 | not finished |
| `L-alternate` | KindLoss "none", C2SPattern "alternate", S2CPattern "alternate" | as committed | 21729 | fail, model error (shared-link alternate, finding 10) |
| `L-timeout` | NumAddrs 3, MaxNonces 3, Timeout 2, HsDeadline 2, ClientTimeout 3, ClientDisconnects, ServerDisconnects, Vanishes, Moves TRUE | OneAdmissionPerToken, ReleaseRemovesMapping, SlotFreed, HalfBounded | 1064645 | pass |
| `S-small` | NumAddrs 3, Slots 1, MapCap 2, AdmCap 2, HistCap 8, MaxNonces 6, Timeout 2, HsDeadline 2, ClientTimeout 3, ClientDisconnects, ServerDisconnects, Vanishes, Moves TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication, Reordering TRUE | OneAdmissionPerToken, ReleaseRemovesMapping | 97717527 | not finished |
| `W-once` | RepeatKeepAlive FALSE | Connects, ServerNotStranded | 42280 | fail as expected (finding 1) |
| `W-nochallenge` | ResendChallenge FALSE | Connects | 8254 | fail as expected (finding 2) |
| `W-once-altconn1` | as `L-altconn1`, RepeatKeepAlive FALSE | Connects | 1819 | fail as expected (finding 3) |
| `W-nochallenge-altconn1` | as `L-altconn1`, ResendChallenge FALSE | Connects | 1318 | fail as expected (finding 3) |
| `W-leak` | MaxNonces 3, Timeout 2, Vanishes TRUE, KindLoss "none", ReleaseMapping FALSE | ReleaseRemovesMapping | 2857 | fail as expected (finding 4) |
| `W-nokey` | NumAddrs 3, MaxNonces 6, Timeout 2, HsDeadline 2, ClientTimeout 3, Vanishes, Moves TRUE, KindLoss "none", KeyCheck FALSE | (safety only) | 13543079 | fail as expected (finding 5) |
| `F9-evict` | HistCap 1, MaxAttempts 2, MaxNonces 3, ClientDisconnects, SharedToken TRUE, KindLoss "none" | OneAdmissionPerToken | 3310405 | fail as expected (F9, finding 6) |
| `F9-noevict` | as `F9-evict` with HistCap 4 | OneAdmissionPerToken, ReleaseRemovesMapping | 6984515 | pass |
| `F5-denied` | Slots 1, MapCap 2, AdmCap 2, MaxNonces 3, ClientDisconnects TRUE, KindLoss "none" | DeniedMeansNoSlot | 159297 | fail as expected (F5, finding 7) |
| `W-attacker1` | NumClients 1, NumAddrs 1, Slots 1, MapCap 2, HistCap 8, AdmCap 2, MaxNonces 6, Timeout 2, HsDeadline 2, ClientTimeout 3, Vanishes, Attacker TRUE, KindLoss "none" | OneAdmissionPerToken, SlotFreed | 14306 | fail as expected (F7, finding 8) |
| `O-hsdeadline` | MaxNonces 6, HsDeadline 2 | Connects | 246770 | fail as expected (observation, finding 9) |

`S-small` and `L-altconn` were stopped by hand at the card's deadline, so neither is a
pass. `S-small` explored 97,717,527 distinct states (512,246,972 generated, depth 23,
38,783,340 left on the queue) without a violation. `L-altconn` explored 707,117 distinct
states, and its periodic liveness checks on the partial graph (the last on 5,852,592
graph nodes) found nothing. The complete safety evidence under arbitrary loss is
therefore the explored prefix of `S-small`. The complete runs under deterministic loss
are `L-*`, and those include every departure (`L-timeout`).

The small instance has two clients and two slots, with `MapCap = 4` (`2 x max_clients`,
as in the code, `rocketnet_server_transport.cpp:798`), `AdmCap = 4` (`2 x max_clients`,
`:800`), `HistCap = 16` (`8 x max_clients`, `:799`) and `DisconnectCopies = 2`.
`Capacity = 2`, and the liveness runs use a FIFO link without latency. The one-slot
runs keep the code's ratios: `MapCap 2`, `AdmCap 2`, `HistCap 8`.

`S-small` is the safety run under the full path. Any copy can be dropped at any time,
and duplication and reordering are on. Every way a client can leave is enabled: its own
disconnect, the server's disconnect, vanishing, the idle and handshake timeouts, and an
address change onto an address a departed client used. With one slot and two clients,
a released slot and a released mapping index are reused by the other client's
connection while copies of the old one are still in flight.

### Loss patterns explored

Across the runs, TLC explored these loss patterns:

- nothing lost (`"none"`);
- the first send on each link lost (`"first"`);
- sends 1 and 2 on each link lost (`"burst"`);
- any copy dropped at any time, with duplication and reordering (`"arbitrary"`, in
  `S-small`, the explored prefix), which covers an old packet arriving late and a burst
  lost;
- the first packet of every kind of every connection lost, in each direction
  (`KindLoss = "firstkind"`, the committed run);
- every other packet of every connection lost, the first included
  (`KindLoss = "alternate"`, "the first packet dropped, the second through", forever);
- the link's own `"alternate"`, counted across all senders on the link (`L-alternate`;
  see model error 8);
- an attacker's re-injected response (`W-attacker1`).

The liveness runs use a FIFO link (see "Fairness").

## What TLC found

Every counterexample below was replayed by hand through the functions it names before
it was written down. Line numbers are in `sdk/rocketnet_server_protocol.cpp` at
`c4f9a8c` unless the line names another file.

### The hang, reproduced (reversed witnesses)

1. **`RepeatKeepAlive = FALSE` under `"firstkind"`** (`W-once`): `Connects` fails with a
   42-state lasso. Both clients reach state 3 and the server assigns both slots. Each
   establishing keep-alive is the first of its kind for its connection, so it is lost.
   The lasso runs from state 38: `Tick`, then both clients resend their stored
   responses, and the server ignores them as "client is already connected"
   (`:491-496`). No keep-alive is ever sent again, so both clients wait in 3 for a
   packet nobody will send again. With the code's behaviour the same instance passes
   (`L-firstkind`, the committed run). In the code, an unacknowledged slot's keep-alive
   is paced from `last_keep_alive_send_time` and is sent every interval until a block
   acknowledges one (`rocketnet_server_protocol_write_keep_alive_if_due`,
   `:1309-1318`).
2. **`ResendChallenge = FALSE` under `"firstkind"`** (`W-nochallenge`): a 30-state trace
   whose lasso returns to state 26. Both challenges are lost. Each client's
   retransmitted request reaches a half-open mapping that stays silent, so the clients
   sit in state 2. In the code, that request gets the stored challenge back
   byte-identical (`:91-106`).
3. **The same two under "every other packet dropped"** (`W-once-altconn1`,
   `W-nochallenge-altconn1`): the same hangs, with the loss coming from per-connection
   alternation. The client's response, or its request, gets through on every second
   send and is answered with nothing.

Every packet the server sends to announce a transition is re-sent until the client's
next packet shows it was seen (see the table above). The code therefore passes every
pattern: see the `L-*` rows. **No server transition in this machine is announced once
and then left.** The one packet the server never re-sends is the disconnect burst: ten
copies, then the slot and mapping are gone (`:1627-1680`). If every copy is lost, the
peer waits for its idle timeout. S:3134-3139 states that as the design ("losing all ten
costs only a peer that waits for the timeout instead"), and `SlotFreed` checks the
server's side of it (`L-timeout`).

The table rules the leak and the cross-delivery out as well:

4. **`ReleaseMapping = FALSE`** (`W-leak`, `SlotMappingBijection`, 14 states): a client
   connects and vanishes. At the idle timeout the slot is released and its mapping is
   left established without a slot. That is the leak the audits suspect at the 2x
   mapping table. The code removes both together in the same pass: the idle timeout at
   `:1049-1058`, the client's disconnect at `:938-942`, and the server's disconnect at
   `:1678-1679`. So `SlotMappingBijection` and `ReleaseRemovesMapping` hold in every
   code run.
5. **`KeyCheck = FALSE`** (`W-nokey`, `NoCrossDelivery`, 27 states): client 2 vanishes.
   Client 1 moves (NAT rebinding) onto client 2's old address while client 2's mapping
   still lives there, and client 1's keep-alive is accepted for client 2's slot. In the
   code, a session packet is looked up by source address (`:702`) and then has to open
   under that mapping's session keys. Client 1's packet does not, so it is ignored, and
   client 1 is left to time out. That is what STANDARD.md, "The client address is the
   connection identity", says happens. `NoCrossDelivery` holds in every code run,
   including `S-small` and `L-timeout`, which have moves.

### Findings in the code (each a line in the report, none a code change here)

6. **F9 is reachable: one token admits two connections** (`F9-evict`,
   `OneAdmissionPerToken`, 41 states; `SharedToken`, `HistCap = 1`). The trace:
   - Client 1 requests with token T from address 1. T is recorded (`:359`) and a
     half-open mapping is made.
   - Client 2's own request is recorded next. The history is at capacity, so it evicts T
     (`rocketnet_used_token_history_record`, `rocketnet_token_tables.cpp:244`;
     `rocketnet_token_table_select_slot`, `:183-187`, takes the oldest).
   - Client 2 connects, disconnects (`:938-942`), and presents T from address 2 on its
     second attempt. The history check passes (`:283`) because T is no longer bound.
     Client id 1 is not connected (`:345`) because client 1 is still half-open. T is not
     admitted (`:352`). So a second half-open mapping now holds T.
   - Client 1's response is admitted (`:521`) and assigned. Then client 1 disconnects
     (`:938-942`). Nothing removes the admitted entry: the set has no remove function.
   - Client 2's response passes the client-id check (`:498`) and the slot check, and
     `rocketnet_admitted_token_set_add` returns success because T is already present
     (`rocketnet_token_tables.cpp:277-278`). T has now admitted its second connection.

   I replayed this by hand through those functions and it is confirmed. It breaks
   S:3591 ("a connect token buys exactly one connection") and S:2648-2649 ("a token
   admits one connection"). The client-id check at `:498` only stops the two
   connections overlapping in time.

   In the code, the precondition is `8 x max_clients` other tokens recorded while the
   first holder is still half-open (inside the 5 s handshake deadline). That is the
   flush S:2784 already names for the address binding, and here it also defeats the
   admission rule. `F9-noevict` is the same instance with a history that never evicts,
   and it passes, so eviction is the cause. The fix shape: at the response, an add that
   finds the token already present should refuse (deny) instead of succeeding.
7. **F5 is reachable: a denied client's slot is held until the idle timeout**
   (`F5-denied`, `DeniedMeansNoSlot`, 29 states, one slot, a FIFO link with no
   reordering). The trace:
   - Client 1 holds the slot. Client 2 is in state 3, and its response is denied for no
     slot (`:514-544`; the mapping is kept).
   - Client 1's application disconnects, and the server releases the slot on the first
     copy (`:938-942`).
   - Client 2's timer sends its stored response again (`client_protocol.cpp:335-342`)
     before the denied reaches it.
   - The denied arrives, and client 2 goes to -1 (`client_protocol.cpp:614`).
   - The server then accepts the in-flight response and assigns the freed slot
     (`:546-576`). The keep-alive that follows meets a client at -1 and is ignored.

   I replayed this by hand and it is confirmed. The server holds a slot, an established
   mapping and an admitted token for a client that has given up, until the idle
   timeout. `SlotFreed` in `L-timeout` shows that the timeout does free it. The
   inventory classes this as a question for the maintainer. This trace answers "is it
   reachable": yes, on an in-order link with no loss.
8. **F7 is reachable: a replayed response keeps a departed client's slot alive forever**
   (`W-attacker1`, `SlotFreed`, a lasso from state 15 to 24; one client, the idle
   timeout on, `Attacker`).
   - The client reaches state 3, sends its response, and vanishes.
   - The response completes the handshake (`:546-576`).
   - From then on, an on-path attacker re-injects the captured response from the
     client's address. Each replay opens under the mapping's handshake key, because
     handshake types are exempt from the replay window (`:730`, `session_phase &&`;
     S:1473-1476).
   - The opened response refreshes `last_receive_time` (`:781`) and is then ignored as
     already connected (`:491`). So the idle timeout (`:1049-1058`) never fires.

   I replayed this and it is confirmed. This is the inventory's F7. S:3088-3091 intends
   the refresh for the client's own retransmissions. The attack needs an attacker on
   the path who can spoof the client's address. Without one, `SlotFreed` holds
   (`L-timeout`).

### Observations (bounded waits, not the hang)

9. **The handshake deadline outruns a client in state 3** (`O-hsdeadline`, `Connects`,
   49 states with the lasso from state 39; the server's handshake deadline on, the
   client's timeout off).
   - Both clients accept their challenges, and their first responses are lost.
   - The half-open mappings reach the handshake deadline and are removed
     (`:1038-1046`).
   - From then on, each client's retransmitted response finds no mapping and is ignored
     (`:702-708`), so the client waits in state 3 for a keep-alive the server will never
     send. The server cannot answer, because it no longer has keys for that address.

   In the code, the client always has its own timeout (`timeout_seconds > 0`,
   `:157`) and total deadline (`client_protocol.cpp:201-221`), so this ends in a client
   timeout. It is a bounded wait, not a wait without end. It does contradict the letter
   of S:2470-2473 ("There is no packet whose loss leaves either end waiting for something
   that will never be sent again"), which names the timeout as the backstop only for the
   disconnect. Under heavy loss, a handshake that runs past the server's 5 s deadline
   (`rocketnet_server_transport.cpp:794`) fails this way, at the client's timeout, with
   no packet saying so. Whether that needs a sentence in the standard or a change in the
   deadline is for the maintainer.

### Model errors, fixed

10. **Link's `"alternate"` on a shared link** (`L-alternate`, the code's behaviour). Two
    clients share `C2S`, and the Link pattern counts every send on the link. The
    schedule can line one client's re-sends up with the odd (lost) indices forever. This
    is the correlation README-Link.md, caution 2, warns of: one sender's retries landing
    exactly on the pattern's losses. The real path does not do this. "Every other
    packet dropped" is therefore checked per connection, as `KindLoss = "alternate"`:
    each `<<side, attempt>>` stream loses its 1st, 3rd, 5th ... send. `L-alternate` fails
    `Connects` with a 39-state lasso from state 28. The run is kept in the table as the
    record of why. Client 1's keep-alive always takes C2S's odd (lost) send index, and
    client 2's request takes the even one, which is delivered. The server's cookie
    answer to client 2 then always lands on S2C's odd index and is lost, and its
    keep-alive to client 1 lands on the even index. Client 2 sits in state 1 for ever.
    I replayed it through Link's `NextCount` and `LostAtSend`, and every step is as
    written. No real path loses one client's packets by another client's send count. Per
    connection, the code passes: `L-altconn1` with one client, and `L-altconn` with two (see its row).
11. **The nonce pool.** After a handshake deadline, stale request copies still in flight
    re-create mappings for the same attempt, each at a fresh nonce. With three nonces,
    `ModelBoundsOK` failed in the deadline and attacker instances while the model was
    being built. `MaxNonces = 6` covers what can be in use at once in those instances,
    and `ModelBoundsOK` holds in every run in the table.

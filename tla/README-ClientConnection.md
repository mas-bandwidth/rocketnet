# ClientConnection: the client's connection state machine

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

`tla/ClientConnection.tla` is the client of `sdk/rocketnet_client_protocol.cpp` (driven
by `sdk/rocketnet_client_transport.cpp`'s update cycle), checked over two instances of
the shared lossy link `tla/Link.tla`: `C2S` carries what the client sends, `S2C` what
the server sends. The server is modelled as the environment the client talks to, only
as far as it drives the client. `tla/MCClientConnection.tla` and
`tla/MCClientConnection.cfg` are the small instance; current measured cases are in
`tla/RUNS.tsv`, with earlier records retained in `tla/RUNS-HISTORY.tsv`. The inventory this card worked from is `docs/TLA-INVENTORY.md`, M1.

## What the model covers

**States** are the standard's numbers (STANDARD.md, _Client State Machine_;
`sdk/rocketnet.h:398-409`): -7 token expired, -5 connection timed out, -4 response
timed out, -3 request timed out, -2 cookie request timed out, -1 denied, 0 disconnected,
1 requesting cookie, 2 sending connection request, 3 sending connection response,
4 connected. -6 (invalid token) is decided before anything is sent and is not modelled.

**State the client owns** (`struct rocketnet_client_protocol_t`):

| Variable | Code | Meaning |
|---|---|---|
| `cstate` | `state` | the state above |
| `cAttempt` | `connection_salt`, ephemeral keypair | the attempt number; fresh per `connect` |
| `cHasCookie` | `has_cookie` | a cookie is held and carried in every request |
| `cKeys` | `keys`, `session_nonce` | the committed keys, `[att, nonce]`, or none |
| `cEph` | `client_ephemeral_secret_key` | the ephemeral secret is live (not zeroed) |
| `cIndex` | `client_index` | the index is known (from the first keep-alive) |
| `cSinceSend`, `cSinceRecv`, `cAge` | `last_send_time`, `last_receive_time`, `attempt_start_time` | ticks since, saturating |
| `cSeen` | `replay_session` | ids of accepted server session packets |
| `cDiscPending`, `cDiscKeys` | the disconnect burst | copies still to send after `disconnect`, and their keys |

**The server, as the client sees it**: one encryption mapping for the client's address
(`sMap`: live, keys, slot held, handshake age, idle age, the keep-alive clock, the last
send of anything, the keep-alive-acknowledged latch), the slots other clients hold
(`sOthers`), and its own disconnect burst.

**Actions**, one per operation and per outside event, each cited to the function it
models in the module:

| Action | Who | Code |
|---|---|---|
| `AppConnect` | application | `rocketnet_client_protocol_connect`: struct reset, fresh salt and ephemeral, state 1 |
| `AppDisconnect` | application | `rocketnet_client_protocol_disconnect`: the burst from 3 or 4, state 0, keys and ephemeral zeroed; from a terminal state, 0 alone |
| `ClientSend` | the scheduled send | `rocketnet_client_protocol_update`: request in 1 and 2, the stored response in 3, keep-alive in 4, at the send interval |
| `ClientDiscCopy` | the client | one copy of the disconnect burst |
| `ClientRecvCookie` | a packet arrives | `rocketnet_client_process_cookie`: 1 or 2, salt matches, store, 1 → 2 |
| `ClientRecvChallenge` | a packet arrives | `rocketnet_client_process_challenge`: 2 only, commit keys, erase ephemeral, 2 → 3 |
| `ClientRecvDenied` | a packet arrives | `rocketnet_client_process_denied`: 2 or 3, → -1, keys and ephemeral zeroed |
| `ClientRecvSession` | a packet arrives | `rocketnet_client_open_session_packet` then keep-alive (3 → 4, index set), payload (accepted from 3 on), disconnect (→ 0) |
| `ServerRecvRequest` | a packet arrives | `rocketnet_server_protocol_process_connection_request`: connected address ignored; half-open mapping answers its stored challenge; no cookie → cookie; no slot → denied, no mapping; else a mapping and its challenge |
| `ServerRecvResponse` | a packet arrives | `rocketnet_server_process_connection_response`: already connected → ignored (the mapping is refreshed); no slot → denied, mapping kept; else slot assigned and the first keep-alive |
| `ServerRecvSession` | a packet arrives | the sealed ladder for a connected mapping: a keep-alive refreshes and its block sets the latch; a disconnect releases slot and mapping |
| `ServerKeepAlive` | the server's timer | `rocketnet_server_protocol_write_keep_alive_if_due`: paced off the keep-alive's own clock until one is acknowledged, off the last send of anything after |
| `ServerPayload` | the server application | a payload, at most one per send interval |
| `ServerAppDisconnect`, `ServerDiscCopy` | the server application | `rocketnet_server_protocol_disconnect_client`: the burst, slot and mapping gone at once |
| `ServerCrash` | the outside | the mapping vanishes with nothing sent |
| `OtherJoins`, `OtherLeaves` | other clients | a slot taken or freed |
| `Tick` | time | one send interval passes: `rocketnet_client_check_deadlines` (total deadline in 1..3, then idle timeout to -(state+1)), `rocketnet_server_protocol_update` (handshake deadline for a half-open mapping, idle timeout for an established one), both links `Age` |
| `C2SEnvironment`, `S2CEnvironment` | the path | Link's `Drop`, `Duplicate`, `Reorder`, `Delay` |

Every packet that drives a transition crosses a Link instance: `ClientSend` and the
server's answers are `Link!Send`, every receive is `Link!Deliver`, `Tick` is `Link!Age`.
Every server packet has a receive step in every client state (what the code's gate or
key check refuses is delivered and discarded), and the same for client packets at the
server, so the links' fairness is never vacuous (README-Link.md, caution 1).

### Properties

Safety, checked under every loss pattern with no fairness:

| Property | The rule |
|---|---|
| `ConnectedMeansHandshake` | never connected without a completed handshake: state 4 holds a cookie, this attempt's committed keys, the index from a keep-alive, and no ephemeral |
| `StepsForwardOrFails` | the machine climbs one rung at a time or fails; 1 is entered only by a connect (S:2042-2045) |
| `DisconnectIsTerminal` | a disconnect is terminal until the application reconnects: the only way out of a non-positive state is a fresh attempt |
| `AcceptedUnderLiveKeys` | no packet is accepted outside the live session's keys: an accepted session packet opened under the held keys in state 3 or 4 |
| `AtMostOneLiveSession` | the client accepts under at most one key pair, which is this attempt's; a slot the server holds for this attempt is under those keys or the client has thrown its keys away |
| `TimeoutPairing` | a timeout from waiting state N lands in -(N+1) (S:2074-2077) |
| `TimeoutHonoured`, `DeadlineHonoured` | the idle timeout and the total deadline fire when due |
| `NoSendOnArrival` | no receive sends (S:2178-2182) |
| `OnlyAuthenticatedRefreshes` | the client trusts only the server: in states 1 and 2 of one attempt nothing restarts the receive clock; a cookie's salt is in the clear, so only the challenge and the session packets after it reset it (TRANSPORT.md t11 rule 8, t11.1) |
| `TypeOK` | types, including both links' |

Liveness, under the fairness described below:

| Property | The rule |
|---|---|
| `Connects` | with a responsive server and fair loss, an attempt reaches connected |
| `ServerNotStranded` | the server's half-open mapping is answered or dies; an unacknowledged slot is acknowledged or released |
| `SilentServerTimesOut` | with a silent server the client reaches -2 within the timeout (with `TimeoutHonoured` for the bound) |
| `CrashedServerTimesOut` | a connected client whose server has gone silent reaches -5 (or its own application disconnects) |
| `AttemptResolves` | every attempt leaves the handshake range |

`TerminalAbsorbing` and `EphemeralErasedOutsideHandshake` hold. `KeysErasedWhenDown`
is still an observation and its run is expected to fail (see below).

### The hang the request named

The class: a peer moves to a new state, sends the packet announcing it once, the packet
is lost, and nobody ever sends it again. `Connects` and `ServerNotStranded` together
say neither peer is left waiting for a packet nobody will send again. They are checked
with every deadline and timeout off, so that only the protocol's own re-sending can get
the client to 4; a deadline would otherwise turn a hang into a tidy -3 or -4.

Two constants reproduce the class, as reversed witnesses:

- `RepeatKeepAlive = FALSE`: the server sends the establishing keep-alive once, at the
  response, and no more until the client acknowledges one. This is the protocol
  without netcode's rule.
- `KeepAliveClock = "anysend"`: the keep-alive is paced off the last send of anything,
  so a payload stream suppresses it. This is the #90 regression the server's comment
  at `rocketnet_server_protocol_write_keep_alive_if_due` records.

The code's behaviour is `RepeatKeepAlive = TRUE`, `KeepAliveClock = "keepalive"`.

## What the model abstracts away

- **Keys.** A key pair is `[att, nonce]`: the attempt's salt and the mapping's session
  nonce are the two fresh inputs every key derives from, so "opens under the held keys"
  is equality on that pair. The ephemeral keypair is a boolean (live or erased). The
  small-order check, the AEAD open and the integrity limit (2^36 failed opens) are not
  modelled: a packet under the wrong pair is delivered and discarded.
- **Sequence numbers and the replay window.** Server session packets carry an id from a
  pool of `MaxIds` instead of a 64-bit sequence. A fresh send takes an id no copy on the
  wire carries and removes it from `cSeen`, so a fresh packet is never refused as a
  replay and no older copy with that id can exist; a copy whose id is in `cSeen` is the
  replay `rocketnet_replay_already_received` refuses. The window's 4096-sequence width
  is above any instance here. Client session packets carry no id: the server's replay
  window is the server card's business. The inventory's `SequenceMonotone` is therefore
  not checked here; `RNReplayWindow` owns sequences.
- **The cookie** is a flag echoing the attempt. The cookie gate (MAC, lifetime, key
  rotation) is the security module's. A cookie never expires here.
- **The ack block** is one bit: a client session packet accepted by a connected mapping
  sets the keep-alive-acknowledged latch, which is what the client's first keep-alive
  from state 4 does in the code. The standalone ack (debt threshold 64) and the client's
  payloads are not modelled; neither moves the connection machine.
- **Time** is a tick of one send interval on both ends, with saturating counters. A
  `Timeout`, `Deadline` or `ServerDeadline` of 0 is off. The code's idle timeout can be
  0; its total deadline cannot, so `Deadline = 0` is the model asking whether the
  protocol alone gets there.
- **The server** holds one mapping for this address and one slot for this client. Other
  clients are a count. Token tables, cookie keys, the client-id check and the confirmed
  flag are the server and security cards'.
- **Payload rate.** The server application sends at most one payload per send interval
  (a model-size bound; the code places no rate on payloads). The #90 witness still
  shows the suppression, because one payload per interval is enough to restamp the
  last-send clock before the keep-alive is due.
- **Fairness** is restated at this model's size; see the next section.

## Fairness

`Spec` is `Init /\ [][Next]_vars /\ Fairness` with

- weak fairness on `Tick`, `AppConnect`, `ClientSend`, `ServerKeepAlive` and the two
  disconnect bursts;
- strong fairness on `ClientSendThrough` and `ServerKeepAliveThrough`: a timer-driven
  send that is enabled with the pattern letting it through, infinitely often, goes
  through infinitely often. This is Link's `EventualDelivery` ("a packet re-sent
  without bound is delivered without bound") stated on the sender. It is what a
  deterministic pattern could otherwise defeat (every re-send landing on a lost index,
  README-Link.md caution 2) and what the real path does not do;
- strong fairness on delivering the head of each wire (`ClientRecvHead`,
  `ServerRecvHead`): a wire with something at its head infinitely often delivers
  infinitely often. This is Link's `WireFair` on the head rather than per value; on a
  FIFO link every copy reaches the head. `Spec` uses this and the send fairness above,
  and it is enough for the deterministic patterns, where loss happens at `Send` and
  nothing on the wire is lost.
- `SpecTyped` adds Link's `EventualDelivery` per packet **type**: a type that enters a
  wire infinitely often is delivered infinitely often. `"arbitrary"` needs it: there
  `Drop` removes copies in flight, and under `Spec` alone TLC produced a path that
  dropped every keep-alive, each one sent behind a payload and dropped before it reached
  the head, and connected nobody (see "What TLC found"). Minutes per run rather than
  seconds; the tableau grows with each implication.

Link's `Fairness` was tried first and does not fit this model: with forty packet values
over two links, the per-value `EventualDelivery` implication makes TLC's tableau
degenerate (`Temporal formula is a tautology`, from the first run, recorded in
RUNS.tsv), and the per-value strong fairness runs the liveness graph out of a 16 GB
heap. The restatement is weaker in one place: a copy of one value can be starved
behind other copies of its type by `Reorder`. No property here depends on a particular
value arriving. Liveness runs use a FIFO link without latency or duplication
(`Reordering = FALSE`, `MaxDelay = 0`, `Duplication = FALSE`); the safety runs keep all
three on. With reordering or latency on, `Reorder` or `Delay` can hold a copy from the
head forever, which only the per-value `EventualDelivery` excludes (README-Link.md,
"What TLC found" 2). With duplication on, TLC found a second artefact: the path keeps a
wire at `Capacity` with duplicated copies exactly when the peer must send, and Link
disables `Send` at `Capacity` rather than dropping, so the protocol's send is blocked
rather than lost, forever. That is the model-size knob acting as a hidden blocker, not
the protocol; see "What TLC found".

The reversed witnesses are the check that this fairness is not vacuous: under it the
code's behaviour passes and the two bad behaviours fail.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19), from `tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 8 -cleanup -deadlock -lncheck final \
     -config MCClientConnection.cfg MCClientConnection
```

**Fragments** are modelled at the receive step: `ClientFragmentReceive` creates
client reassembly state (`cReasm`) only in state 4 (connected), and
`ServerFragmentReceive` creates server reassembly state (`sReasm`) only for a
mapping holding its slot. The invariant `FragmentOnlyWhenConnected` states
that reassembly state changes only while connected, on both sides.
`MCClientConnectionBrokenEarlyFragment.cfg` is its reversed witness: it adds
`ClientFragmentReceiveEarly`, the pre-fix acceptance in sending-connection-response
(state 3), and must fail `FragmentOnlyWhenConnected`.

`MCClientConnection.cfg` is the committed run: the hang check under "the first packet
of every kind is dropped" (`KindLoss = "firstkind"`), the code's keep-alive behaviour,
deadlines and timeouts off, payloads on, no disconnects, checking every safety property
plus `Connects` and `ServerNotStranded`. The other runs are the same file with the lines
below changed; the TSV has one row per run.

| Run | Lines changed from the committed cfg | Spec | Checks beyond the safety set | Distinct states | Result |
|---|---|---|---|---|---|
| `L-firstkind` | committed | Spec | Connects, ServerNotStranded | 9748 | pass |
| `L-none` | KindLoss "none" | Spec | Connects, ServerNotStranded | 4089 | pass |
| `L-first` | KindLoss "none", C2SPattern "first", S2CPattern "first" | Spec | Connects, ServerNotStranded | 4057 | pass |
| `L-alternate` | KindLoss "none", C2SPattern "alternate", S2CPattern "alternate" | Spec | Connects, ServerNotStranded | 11237 | pass |
| `L-burst` | KindLoss "none", C2SPattern "burst", S2CPattern "burst" | Spec | Connects, ServerNotStranded | 4055 | pass |
| `L-arbitrary` | KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary" | SpecTyped | Connects, ServerNotStranded | 23032 | not finished (stopped at the card's deadline) |
| `W-once-firstkind` | RepeatKeepAlive FALSE | Spec | Connects | 1312 | fail as expected |
| `W-once-alternate` | KindLoss "none", RepeatKeepAlive FALSE, C2SPattern "alternate", S2CPattern "alternate" | Spec | Connects | 7413 | fail as expected |
| `W-anysend` | KeepAliveClock "anysend" | Spec | Connects | 9748 | fail as expected |
| `L-silent` | Timeout 2, ServerMute TRUE, KindLoss "none" | Spec | SilentServerTimesOut, AttemptResolves | 20 | pass |
| `L-crash` | Timeout 3, Deadline 6, ServerDeadline 4, MaxNonces 2, Disconnects TRUE, KindLoss "none" | Spec | CrashedServerTimesOut, AttemptResolves | 772894 | pass |
| `S-none` | Timeout 2, Deadline 4, ServerDeadline 3, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", MaxDelay 1, Duplication TRUE, Reordering TRUE | Spec | safety only | 28369060 | not finished (stopped at the card's deadline) |
| `S-first` | Timeout 2, Deadline 5, ServerDeadline 3, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "first", S2CPattern "first", Duplication TRUE, Reordering TRUE | Spec | safety only | 74297106 | pass |
| `S-alternate` | MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "alternate", S2CPattern "alternate", Duplication TRUE, Reordering TRUE | Spec | safety only | 18268052 | pass |
| `S-burst` | MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "burst", S2CPattern "burst", Duplication TRUE, Reordering TRUE | Spec | safety only | 24756464 | pass |
| `S-arbitrary` | Timeout 2, Deadline 5, ServerDeadline 3, MaxAttempts 2, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | Spec | safety only | 75959281 | not finished (stopped at the card's deadline) |
| `S-firstkind` | Timeout 2, Deadline 4, ServerDeadline 3, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, MaxDelay 1, Duplication TRUE, Reordering TRUE | Spec | safety only | 7054770 | pass |
| `S-delay` | Timeout 2, Deadline 4, ServerDeadline 3, MaxNonces 2, KindLoss "none", C2SPattern "first", S2CPattern "first", MaxDelay 1, Duplication TRUE, Reordering TRUE | Spec | safety only | 5051198 | pass |
| `F4-ephemeral` | Timeout 2, ServerMute TRUE, KindLoss "none" | Spec | EphemeralErasedOutsideHandshake | 20 | pass |
| `F8-terminal` | Timeout 2, MaxAttempts 2, Disconnects TRUE, ServerMute TRUE, KindLoss "none" | Spec | TerminalAbsorbing | 149 | pass |
| `OBS-keys` | Timeout 2, Deadline 4, ServerDeadline 3, MaxAttempts 2, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", MaxDelay 1, Duplication TRUE, Reordering TRUE | Spec | KeysErasedWhenDown | 17569 | fail as expected (observation) |
| `W-denied-keeps-keys` | Timeout 0, Deadline 0, OthersMayJoin TRUE, Disconnects FALSE, ServerPayloads FALSE, KindLoss "none" | SpecDeniedKeepsKeys | KeysErasedWhenDown | 250 | fail as expected |

The small instance: `SendInterval = 1`, `MaxIds = 2`, `MaxSlots = 1`,
`DisconnectCopies = 2`, `Capacity = 2`, `BurstStart = 1`, `BurstLen = 2`. Liveness runs
(`L-*`, `W-*`): a FIFO link, `MaxNonces = 1`, `MaxAttempts = 1`, every deadline off.
Safety runs (`S-*`): duplication and reordering on, `MaxNonces = 2`, the application
and the server may disconnect, other clients may take the slot, deadlines on where the
pattern lets the handshake finish inside them and off where it does not
(`"alternate"`, `"burst"`, `"firstkind"`); `S-firstkind` and `S-delay` add a tick of
latency; `S-arbitrary` adds a second attempt (a reconnect, so a late packet of an old
attempt meets a new one) and did not finish inside the card's deadline: 74.6 million
distinct states with no violation when it was stopped, and TLC reports a violation the
moment it finds one. `S-none` with latency and the full environment was stopped at 28
million states for the same reason; `S-firstkind` (7 million, complete) is the
latency-plus-full-environment run that finished. `L-arbitrary` under `SpecTyped` was
stopped 42 minutes into its temporal check (69 thousand graph nodes): the per-type
implications are what TLC's tableau is slow on. Under `Spec` the same instance fails
with the type-targeted drop of "What TLC found" 9, which is the fairness being too
weak, not the protocol; the arbitrary-loss liveness therefore stands on the hand
replay of that lasso and on the four deterministic patterns, not on a completed run.

### Loss patterns explored

Each run names its patterns in the TSV. Across the runs TLC explored: nothing lost
(`"none"`); the first send on each link lost (`"first"`); every other send lost
(`"alternate"`: "the first packet dropped, the second through", forever); sends 1 and 2
lost (`"burst"`); any copy dropped at any time (`"arbitrary"`); and the first packet of
every kind lost (`KindLoss = "firstkind"`, applied before the link). In every safety run
both links also duplicate and reorder, and in `S-firstkind`, `S-delay` and the three
findings runs delay by a tick, so an old packet arriving late and a replayed packet are
in every safety run. The liveness runs use a FIFO link (the fairness section says why).

## What TLC found

Every counterexample below was replayed by hand through the functions it names before
it was written down. Three are findings in the code (all three already in
`docs/TLA-INVENTORY.md`, now reproduced as traces); the rest are model errors fixed on
the way, or the reversed witnesses doing their job.

### The hang, reproduced (reversed witnesses)

1. **`RepeatKeepAlive = FALSE` under `"firstkind"`** (`W-once-firstkind`): `Connects`
   fails with a 30-state lasso. The client reaches 3 and sends its response; the server
   accepts it, assigns the slot and sends the keep-alive (state 21); it is the first of
   its kind and is lost. From there the lasso is `Tick`, `ClientSend` (the stored
   response, byte-identical), `ServerRecvResponse` ("client is already connected",
   ignored). The server never sends another keep-alive; the client waits in 3 for a
   packet nobody will send again. That is the hang the request described, and
   `ServerNotStranded` fails on the same lasso: the slot is never acknowledged.
2. **`RepeatKeepAlive = FALSE` under `"alternate"`** (`W-once-alternate`): the same
   hang, with the loss coming from the link: the keep-alive is the server's fifth send
   on an odd index and is lost at `Send`; 28 states, lasso from state 25.
3. **`KeepAliveClock = "anysend"` under `"firstkind"`** (`W-anysend`): the #90
   regression. The first keep-alive is lost; the server application streams payloads
   (`streaming = TRUE` in every `Tick`), so `sinceSend` is 0 at every keep-alive check
   and the repair keep-alive is never due; `sinceKA` sits at 1, due, unread. 28 states,
   lasso from state 23. With the code's clock (`"keepalive"`) the same instance passes
   (`L-firstkind`, the committed run).

The code's behaviour passes under every pattern: see `L-*` in the table. The
protocol's answer to the hang is the one the standard states: every wait state
re-sends, and the establishing keep-alive is re-sent on its own clock until the client
acknowledges one (`rocketnet_server_protocol_write_keep_alive_if_due`,
`sdk/rocketnet_server_protocol.cpp`).

### The code, and the keys observation

4. **The ephemeral is erased outside the handshake** (`F4-ephemeral`,
   `EphemeralErasedOutsideHandshake`, 20 distinct states, pass). A denial is
   `rocketnet_client_set_state` of `CONNECTION_DENIED`, and a deadline or a timeout
   is `rocketnet_client_check_deadlines` calling the same. A state at or below
   `DISCONNECTED` runs `rocketnet_client_connection_reset`, which zeroes the
   ephemeral. The model does the same. The old full environment does not finish
   once the property holds, so the run is the silent-server timeout, which is
   the old counterexample's trace.
5. **A terminal state leaves by connect or disconnect** (`F8-terminal`,
   `TerminalAbsorbing`, 149 distinct states, pass). `rocketnet_client_protocol_disconnect`
   sets `DISCONNECTED` from any state that is not already `DISCONNECTED`, and
   `sdk/rocketnet.h` documents that. The property allows that step, to 0, and a
   connect, to 1. `DisconnectIsTerminal` still says the only way up from a
   non-positive state is a fresh connect.
6. **The session keys survive a timeout or a received disconnect** (`OBS-keys`,
   `KeysErasedWhenDown`, 11 states): the challenge commits the keys; two ticks
   later the client is down and `cKeys` is still set. A denial zeroes the keys,
   matching `rocketnet_client_set_state`. A timeout (`rocketnet_client_check_deadlines`)
   and a received disconnect (`rocketnet_client_process_disconnect`) also go
   through `set_state`, which zeroes the keys, and this model does not. The run
   stays the observation. The state gate refuses every session packet below 3,
   so the keys cannot be used, only read from memory.

### Model errors, fixed

7. **Link's fairness at this size.** The first run with `Link!Fairness` on both links
   stopped at parse time: `Temporal formula is a tautology (its negation is
   unsatisfiable)`. The per-value `EventualDelivery` implication over forty packet
   values made the tableau degenerate. With it removed and `WireFair`'s per-value
   strong fairness kept, the liveness graph ran a 16 GB heap out in two minutes. The
   fairness section above is the result.
8. **The wire as a blocker.** Under `"alternate"` and `"arbitrary"` with duplication on,
   `Connects` failed with lassos in which the path kept one wire at `Capacity` with
   duplicated copies of an old request (or challenge) exactly when the client (or
   server) was due to send, interleaving `Duplicate` and a receive-and-discard so the
   wire never had room at the right moment. Link disables `Send` at `Capacity`, so the
   protocol's re-send was blocked, not lost, forever. Replayed through Link's
   `Duplicate` and `Send`: every step is as written. Not a protocol behaviour (a real
   send never blocks on the receiver's backlog). Liveness runs now use
   `Duplication = FALSE`.
9. **Type-correlated drops.** Under `"arbitrary"` with head-only delivery fairness,
   `Connects` failed with a lasso in which `Drop` removed every keep-alive while every
   payload was delivered. First attempt at a fix, strong fairness on delivering the
   head *per type*, failed the same way with a sharper lasso: the server sends the
   keep-alive behind a payload every time, the path drops it while it is second on the
   wire, and the keep-alive is never at the head, so a condition on the head has
   nothing to force. A path that drops one packet type forever is exactly what "a
   packet re-sent often enough eventually arrives" excludes, so `"arbitrary"` runs
   under `SpecTyped`, whose `DeliveryFairTyped` is Link's `EventualDelivery` per type.
10. **The payload race.** With payloads limited to one per interval and the witness
    clock `"anysend"`, the first `W-anysend` run passed: at tick granularity the payload
    and the keep-alive are due at the same instant and the keep-alive's own fairness
    wins the race, where the code's continuous stream restamps the clock before every
    check. The `streaming` choice in `Tick` is the fix: the stream's effect on the
    clock, with the packets themselves still `ServerPayload`.

### Not a hang: the disconnect burst

The one packet either end sends without re-sending is the disconnect: ten copies
(`DisconnectCopies` here), then the sender's state is gone
(`rocketnet_client_protocol_disconnect`, `sdk/rocketnet_client_protocol.cpp:1309-1336`;
`rocketnet_server_protocol_disconnect_client`, `sdk/rocketnet_server_protocol.cpp:1656-1676`).
If every copy is lost the peer waits for its idle timeout, which S:2918-2924 states as
the design ("losing all ten costs only a peer that waits for the timeout"). In this
model that is `CrashedServerTimesOut` (`L-crash`): the client reaches -5. It is a
bounded wait, not a packet nobody will send again, and it is not reported as a defect.

## Reproducible case files

The original seven-column records are preserved unchanged in
[`RUNS-HISTORY.tsv`](RUNS-HISTORY.tsv). Current measurements use the runner
and refresh procedure in [`README.md`](README.md). Each historical row now
has its own committed configuration; no edit to a shared file is needed.
The table follows the original ledger order. A declared debt retains its
original parameters and is not a completed proof.

| Historical row | Committed configuration | Declared outcome |
|---|---|---|
| 12 | `MCClientConnection.cfg` | pass |
| 13 | `MCClientConnectionLNone.cfg` | pass |
| 14 | `MCClientConnectionLFirst.cfg` | pass |
| 15 | `MCClientConnectionLAlternate.cfg` | pass |
| 16 | `MCClientConnectionLBurst.cfg` | pass |
| 17 | `MCClientConnectionLArbitrary.cfg` | pass; debt: arbitrary-loss-liveness-over-budget |
| 18 | `MCClientConnectionWOnceFirstkind.cfg` | temporal / Connects |
| 19 | `MCClientConnectionWOnceAlternate.cfg` | temporal / Connects |
| 20 | `MCClientConnectionWAnysend.cfg` | temporal / Connects |
| 21 | `MCClientConnectionLSilent.cfg` | pass |
| 22 | `MCClientConnectionLCrash.cfg` | pass; debt: crash-timeout-liveness-over-budget |
| 23 | `MCClientConnectionSNone.cfg` | pass; debt: full-environment-safety-over-budget |
| 24 | `MCClientConnectionSFirst.cfg` | pass; debt: first-loss-full-environment-safety-over-budget |
| 25 | `MCClientConnectionSAlternate.cfg` | pass; debt: alternate-loss-full-environment-safety-over-budget |
| 26 | `MCClientConnectionSBurst.cfg` | pass; debt: burst-loss-full-environment-safety-over-budget |
| 27 | `MCClientConnectionSArbitrary.cfg` | pass; debt: arbitrary-loss-safety-over-budget |
| 28 | `MCClientConnectionSFirstkind.cfg` | pass; debt: first-kind-latency-safety-over-budget |
| 29 | `MCClientConnectionSDelay.cfg` | pass; debt: delayed-first-loss-safety-over-budget |
| 30 | `MCClientConnectionF4Ephemeral.cfg` | pass |
| 31 | `MCClientConnectionF8Terminal.cfg` | pass |
| 32 | `MCClientConnectionObsKeys.cfg` | invariant / KeysErasedWhenDown |
| 33 | `MCClientConnectionBrokenDeniedKeepsKeys.cfg` | invariant / KeysErasedWhenDown |

### 2026-10-21: the cookie restarts nothing (OnlyAuthenticatedRefreshes)

`ClientRecvCookie` no longer sets `cSinceRecv' = 0`, matching
`rocketnet_client_process_cookie`, which no longer writes
`last_receive_time` (and no longer takes the round-trip sample). The action
property `OnlyAuthenticatedRefreshes` says that within one attempt, while
the client is in states 1 or 2, `cSinceRecv` never goes down.
`MCClientConnectionCookieTrust.cfg` checks it with a responsive server and
the idle timeout on (Timeout 3), with `SafetyInvariants`, `TimeoutPairing`
and `StepsForwardOrFails`: pass, 28,946 distinct states.
`MCClientConnectionBrokenCookieRefreshes.cfg` is the reversed witness: the
pre-fix cookie (`ClientRecvCookieRefreshes`, only in `SpecCookieRefreshes`).
TLC exits 13 with `OnlyAuthenticatedRefreshes` violated in 6 states: connect
(`cSinceRecv` 0), the request, the server's cookie, one tick (`cSinceRecv`
1), then the cookie delivered, `cstate` 1 -> 2 and `cSinceRecv` 0. Checked
against the code by hand: `rocketnet_client_protocol_connect` sets
`last_receive_time` to the connect time, and the old cookie path set it again
to the cookie's arrival time, which is what
`test_client_cookie_does_not_refresh_idle_timer` catches on the old code.

The whole `clientconnection` group was re-measured on captain (Linux, pinned
jar, `/opt/tla/tlacheck`, one shard per case, 110 s each). Every declared
outcome holds. Three bench debt cases that timed out before now finish
inside the budget, because a cookie no longer resets the clock and the state
space shrank: `SFirst` (2,234 distinct), `SFirstkind` (12,162) and `SDelay`
(2,196). The other six debt cases still exit 124. `tlacheck merge --keep
tla/RUNS.tsv` refuses to join these records while eight other groups are
already stale on the base branch (fragmentsandacks, replaywindow, rnblocks,
rnchannel, rnchanneltypes, rnreceipts, rnscheduler, rnslots), so
`tla/RUNS.tsv` is not edited by hand. The records the runner wrote,
verbatim, for the merge that refreshes those groups:

```
MCClientConnection.cfg	MCClientConnection.tla	ddf55aaccbc5790c87892c7607906cc80b71773ee3bc5ede2e42868812a4d646	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.570443+00:00	2	192207	33480	78.509	0	PASS	pass	-	110	bounded
MCClientConnectionLNone.cfg	MCClientConnection.tla	4ab9d74ebc804a598a76386b0dc28ceab82ff2f3a9b42da45e8408a21588d01f	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.563772+00:00	2	81974	14521	33.217	0	PASS	pass	-	110	bounded
MCClientConnectionLFirst.cfg	MCClientConnection.tla	028907294be906bac8e5815520ab6fa5718e3b311bd456cee256f9090ac91ce7	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.559050+00:00	2	81857	14489	33.492	0	PASS	pass	-	110	bounded
MCClientConnectionLAlternate.cfg	MCClientConnection.tla	949c9563bfb39374592463c45ef06de67ab1414ec81bfd46d9a873c5fea05db7	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.574947+00:00	2	211590	36965	83.695	0	PASS	pass	-	110	bounded
MCClientConnectionLBurst.cfg	MCClientConnection.tla	d8a75b1af0d30141854f2f89be7b33589830414b16c40aba29e04d1ce2a12361	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.565091+00:00	2	81851	14487	33.253	0	PASS	pass	-	110	bounded
MCClientConnectionLArbitrary.cfg	MCClientConnection.tla	596f380c50abf27eafaaec9f85061a32b61d3f439260616e0aedcddd8967f023	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.570376+00:00	2	-	-	110.187	124	FAIL	pass	-	110	bounded
MCClientConnectionWOnceFirstkind.cfg	MCClientConnection.tla	28d3f45f044955682ccdaf8e2df8a047af485768caefdbdf095d03f61147fe6c	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.562103+00:00	1	9983	2232	8.920	13	PASS	temporal	Connects	110	bounded
MCClientConnectionWOnceAlternate.cfg	MCClientConnection.tla	eeaf31351f0038b5fc5b95bdae92a654f40eb2fc36e0e43007a60713462bdda2	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:50.568377+00:00	1	123838	23789	27.353	13	PASS	temporal	Connects	110	bounded
MCClientConnectionWAnysend.cfg	MCClientConnection.tla	125baa624c3370cda6aa669a6405212ffea285e3d7f81e32a0b3dbe6047c4244	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:33:59.664124+00:00	1	183567	33480	37.500	13	PASS	temporal	Connects	110	bounded
MCClientConnectionLSilent.cfg	MCClientConnection.tla	905e0cc557a194d97ad6ccdc3d702c66d1f6a0be35a43bf14dd210588e14fb70	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:34:18.083888+00:00	2	36	20	1.929	0	PASS	pass	-	110	bounded
MCClientConnectionLCrash.cfg	MCClientConnection.tla	7e4afe9ee715240b818caecea40f2e1eb339f58b3d3b83cb65ad45f7dd6925e2	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:34:20.166660+00:00	2	-	-	110.160	124	FAIL	pass	-	110	bounded
MCClientConnectionSNone.cfg	MCClientConnection.tla	9d719bfa2e2ebbe02773068454c690a2ccc1b3c90571db94d243b1c6e1d0aceb	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:34:23.924894+00:00	2	-	-	110.366	124	FAIL	pass	-	110	bounded
MCClientConnectionSFirst.cfg	MCClientConnection.tla	821f38bea9f788912d056aa43c70510ed4531bbac4f27f2f6ba41a3a88eb5d65	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:34:23.955105+00:00	2	9355	2234	4.727	0	PASS	pass	-	110	bounded
MCClientConnectionSAlternate.cfg	MCClientConnection.tla	ef73ee059c5a8ca6cf97658bc6fbaeb2823d5d42e2075fd6a9f14ed4c79940de	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:34:24.199145+00:00	2	-	-	110.287	124	FAIL	pass	-	110	bounded
MCClientConnectionSBurst.cfg	MCClientConnection.tla	a2751e8a887bb07994eae0fd74626c853e7a26af96543e789a34c0fa4b239c53	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:34:28.852588+00:00	2	-	-	110.334	124	FAIL	pass	-	110	bounded
MCClientConnectionSArbitrary.cfg	MCClientConnection.tla	a0096a06302c3eb3c69054d2cdb608eada41ea30f7dc7a9deb2523382145fe2c	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:34:37.499331+00:00	2	-	-	110.264	124	FAIL	pass	-	110	bounded
MCClientConnectionSFirstkind.cfg	MCClientConnection.tla	42600b4fcbb6ac75c96ee2e824a19ee1005fbdb738ab24674febc69bfba2fad5	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:09.252298+00:00	2	101789	12162	6.607	0	PASS	pass	-	110	bounded
MCClientConnectionSDelay.cfg	MCClientConnection.tla	8a674bc41263151014d4bac30ebde134ec16939b6bded3b2f83d5fa999e0916c	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:14.433799+00:00	2	15104	2196	4.389	0	PASS	pass	-	110	bounded
MCClientConnectionF4Ephemeral.cfg	MCClientConnection.tla	ee5729eefd5699b5cf00710cd660cbffc3e90749abe2cc47751b6c1bef6e7b83	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:16.025146+00:00	2	36	20	2.139	0	PASS	pass	-	110	bounded
MCClientConnectionF8Terminal.cfg	MCClientConnection.tla	abfddd92e891b45f83bc9f5d1f8769973ee9d984168239fee43b7c5b6528d28c	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:18.392077+00:00	2	357	149	2.030	0	PASS	pass	-	110	bounded
MCClientConnectionObsKeys.cfg	MCClientConnection.tla	d7988610aba60a605552e02c0e31d42d589025c202f8d2ed487bafc3ad75c82e	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:18.988305+00:00	1	70215	13010	5.343	12	PASS	invariant	KeysErasedWhenDown	110	bounded
MCClientConnectionBrokenDeniedKeepsKeys.cfg	MCClientConnection.tla	2e8b7f001fd5174976d3c1c7be58c0156fc3fdadeabe09d7f3277c36be93440e	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:20.603031+00:00	1	640	250	2.127	12	PASS	invariant	KeysErasedWhenDown	110	bounded
MCClientConnectionBrokenEarlyFragment.cfg	MCClientConnection.tla	9d54bce581d6400a1e4ff7507b4395bda46ec0cdd1a6d7a0c15bae39ba2fefb1	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:22.901695+00:00	1	148	78	2.553	12	PASS	invariant	FragmentOnlyWhenConnected	110	bounded
MCClientConnectionCookieTrust.cfg	MCClientConnection.tla	c1cae5d943c86fcf45aa694fcf808f8030ae8bf4bf486a68fb558c06e7d1fef8	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:24.497942+00:00	2	158359	28946	7.094	0	PASS	pass	-	110	bounded
MCClientConnectionBrokenCookieRefreshes.cfg	MCClientConnection.tla	7dd153d222db343a3d957ba53eb60871c865eaf3272311ddfd1ce95a33c4e648	9	936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88	25.0.4.1	linux-amd64	64	2026-10-21T15:35:25.618085+00:00	1	30	22	2.288	13	PASS	action	OnlyAuthenticatedRefreshes	110	bounded
```

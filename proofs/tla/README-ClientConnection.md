# ClientConnection: the client's connection state machine

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

`proofs/tla/ClientConnection.tla` is the client of `sdk/rocketnet_client_protocol.cpp` (driven
by `sdk/rocketnet_client_transport.cpp`'s update cycle), checked over two instances of
the shared lossy link `proofs/tla/Link.tla`: `C2S` carries what the client sends, `S2C` what
the server sends. The server is modelled as the environment the client talks to, only
as far as it drives the client. `proofs/tla/MCClientConnection.tla` and
`proofs/tla/MCClientConnection.cfg` are the small instance; current measured cases are in
`proofs/tla/RUNS.tsv`, with earlier records retained in `proofs/tla/RUNS-HISTORY.tsv`. The inventory this card worked from is `docs/TLA-INVENTORY.md`, M1.

## What the model covers

**States** are the standard's numbers (STANDARD.md, _Client State Machine_;
`sdk/rocketnet.h`): -7 token expired, -5 connection timed out, -4 response
timed out, -3 request timed out, -2 cookie request timed out, -1 denied, 0 disconnected,
1 requesting cookie, 2 sending connection request, 3 sending connection response,
4 connected. -6 (invalid token) is decided before anything is sent and is not modelled.

**State the client owns** (`struct rocketnet_client_protocol_t`):

| Variable | Code | Meaning |
|---|---|---|
| `cstate` | `state` | the state above |
| `cAttempt` | `connection_salt`, ephemeral keypair | the attempt number; fresh per `connect` |
| `cKept` | `has_cookie`, `return_cookie` | the attempt's first cookie, kept |
| `cLatest` | `has_latest_cookie`, `latest_cookie` | a later cookie (`ClientRecvCookie`) |
| `cAltLatest` | `send_latest_cookie` | the next request carries the latest cookie |
| `cKeys` | `keys` (derived from the nonce; the nonce itself is not kept) | the committed keys, `[att, nonce]`, or none |
| `cEph` | `client_ephemeral_secret_key` | the ephemeral secret is live (not zeroed) |
| `cIndex` | `client_index` | the index is known (from the first keep-alive) |
| `cSinceSend`, `cSinceRecv`, `cAge` | `last_send_time`, `last_receive_time`, `attempt_start_time` | ticks since, saturating |
| `cSeen` | `replay_session` | ids of accepted server session packets |
| `cPayloadDelivered` | none (ghost) | the application has been handed a payload in connected this attempt; recorded only by the reachability witness's step `ClientRecvPayloadWitness`, so it stays FALSE under `Spec` |
| `cDiscPending`, `cDiscKeys` | the disconnect burst | copies still to send after `disconnect`, and their keys |
| `cEarly` | `handshake_early_send` | an advance (1 → 2 by the first cookie, 2 → 3 by the challenge) armed one send at the next update (TRANSPORT.md t11 rule 7) |
| `cEarlySends` | none (ghost) | the early sends of this attempt, bounded by `EarlySendPerAdvance` |

**The server, as the client sees it**: one encryption mapping for the client's address
(`sMap`: live, keys, slot held, handshake age, idle age, the keep-alive clock, the last
send of anything, the keep-alive-acknowledged latch), the slots other clients hold
(`sOthers`), and its own disconnect burst.

**Actions**, one per operation and per outside event, each cited to the function it
models in the module:

| Action | Who | Code |
|---|---|---|
| `AppConnect` | application | `rocketnet_client_protocol_connect`: struct reset, fresh salt and ephemeral, state 1 |
| `AppDisconnect` | application | `rocketnet_client_protocol_disconnect`: the burst from 3 or 4, state 0 through `rocketnet_client_set_state` (or `rocketnet_client_connection_reset` directly), keys, ephemeral, cookies, client index and replay window zeroed; from a terminal state, 0 and the zeroing alone |
| `ClientSend` | the scheduled send | `rocketnet_client_protocol_update`: request in 1 and 2, the stored response in 3, keep-alive in 4, at the send interval |
| `ClientEarlySend` | the advance | `rocketnet_client_protocol_update` with `handshake_early_send`: the new state's packet without waiting for the interval, once, in 2 or 3; any send clears the arm |
| `ClientDiscCopy` | the client | one copy of the disconnect burst |
| `ClientRecvCookie` | a packet arrives | `rocketnet_client_process_cookie`: 1 or 2, salt matches, store, 1 → 2 |
| `ClientRecvChallenge` | a packet arrives | `rocketnet_client_process_challenge`: 2 only, commit keys, erase ephemeral, 2 → 3; with `SmallOrderServer` a challenge carrying a small-order server key is ignored and nothing changes |
| `ClientRecvDenied` | a packet arrives | `rocketnet_client_process_denied`: 2 or 3, → -1 through `rocketnet_client_set_state`, whose `rocketnet_client_connection_reset` zeroes keys, ephemeral, both cookies, the client index and the replay window |
| `ClientRecvSession` | a packet arrives | `rocketnet_client_open_session` then keep-alive (3 → 4, index set), payload (accepted in 4 only, `rocketnet_client_session_state_accepts`), disconnect (→ 0 through `rocketnet_client_set_state`: keys, cookies, client index and replay window zeroed), and with `ServerAcks` the standalone ack (`rocketnet_client_process_ack`: the receive clock refreshed, no state change); with `AuthenticRefusals` a packet that opens and is replay-marked but then refused (bad ack block, wrong size, keep-alive index past the slot count: the checks after the replay advance in `rocketnet_client_open_session` and `rocketnet_client_validate_keep_alive`) marks its id and changes nothing else |
| `ServerRecvRequest` | a packet arrives | `rocketnet_server_protocol_process_connection_request`: connected address ignored; half-open mapping answers its stored challenge; no cookie → cookie; no slot → denied, no mapping; else a mapping and its challenge |
| `ServerRecvResponse` | a packet arrives | `rocketnet_server_process_connection_response`: already connected → ignored, the idle clock not refreshed (only a mapping with no slot refreshes it from a response, as in ServerConnection); no slot → the half-open mapping refreshed and denied, mapping kept; else slot assigned and the first keep-alive |
| `ServerRecvSession` | a packet arrives | the sealed check sequence for a connected mapping: a keep-alive refreshes and its block sets the latch; a disconnect releases slot and mapping |
| `ServerAck` | the server's ack timer | `rocketnet_server_protocol_write_ack_if_due`: with `ServerAcks`, a standalone ack to a connected client at the send interval while an ack is owed (`ack_debt`: set by an accepted client keep-alive, cleared by every server session send; an environment action) |
| `ServerKeepAlive` | the server's timer | `rocketnet_server_protocol_write_keep_alive_if_due`: paced off the keep-alive's own clock until one is acknowledged, off the last send of anything after |
| `ServerPayload` | the server application | a payload, at most one per send interval |
| `ServerAppDisconnect`, `ServerDiscCopy` | the server application | `rocketnet_server_protocol_disconnect_client`: the burst, slot and mapping gone at once |
| `ServerCrash` | the outside | the mapping vanishes with nothing sent |
| `OtherJoins`, `OtherLeaves` | other clients | a slot taken or freed |
| `Tick` | time | one send interval passes: `rocketnet_client_check_deadlines` (total deadline in 1..2, then idle timeout to -(state+1), both through `rocketnet_client_set_state`, which zeroes keys, cookies, client index and replay window), `rocketnet_server_protocol_update` (handshake deadline for a half-open mapping, idle timeout for an established one), both links `Age` |
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
| `StepsForwardOrFails` | the machine climbs one rung at a time or fails; 1 is entered only by a connect (STANDARD.md, ClientConnection StepsForwardOrFails) |
| `DisconnectIsTerminal` | a disconnect is terminal until the application reconnects: the only way out of a non-positive state is a fresh attempt |
| `AcceptedUnderLiveKeys` | no packet is accepted outside the live session's keys: an accepted session packet opened under the held keys in state 3 or 4 |
| `AtMostOneLiveSession` | the client accepts under at most one key pair, which is this attempt's; a slot the server holds for this attempt is under those keys or the client has thrown its keys away |
| `TimeoutPairing` | a timeout from waiting state N lands in -(N+1) (STANDARD.md, ClientConnection TimeoutPairing) |
| `TimeoutHonoured`, `DeadlineHonoured` | the idle timeout and the total deadline fire when due |
| `NoSendOnArrival` | no receive sends (STANDARD.md, ClientConnection NoSendOnArrival) |
| `EarlySendPerAdvance` | the early send is bounded by the advances, not by the arrivals: none in 1, at most one in 2, at most two in an attempt (TRANSPORT.md t11 rule 7) |
| `OnlyAuthenticatedRefreshes` | the client trusts only the server: in states 1 and 2 of one attempt nothing restarts the receive clock; a cookie's salt is in the clear, so only the challenge and the session packets after it reset it (TRANSPORT.md t11 rule 8, t11.1) |
| `FreshAttemptFromFailure` | a connect from a failure state (-7..-1) is a fresh attempt: a new attempt index (the fresh salt, ephemeral and keys) and nothing of the earlier attempt carried, no keys, cookie, client index or replay window, and fresh timers |
| `TypeOK` | types, including both links' |

Liveness, under the fairness described below:

| Property | The rule |
|---|---|
| `Connects` | with a responsive server and fair loss, an attempt reaches connected |
| `ServerNotStranded` | the server's half-open mapping is answered or dies; an unacknowledged slot is acknowledged or released |
| `SilentServerTimesOut` | with a silent server the client reaches -2 within the timeout (with `TimeoutHonoured` for the bound) |
| `CrashedServerTimesOut` | a connected client whose server has gone silent reaches -5 (or its own application disconnects) |
| `AttemptResolves` | every attempt leaves the handshake range |

`TerminalAbsorbing`, `EphemeralErasedOutsideHandshake` and `KeysErasedWhenDown` hold.

### The hang the request named

The class: one side moves to a new state, sends the packet announcing it once, the packet
is lost, and nobody ever sends it again. `Connects` and `ServerNotStranded` together
say neither side is left waiting for a packet nobody will send again. They are checked
with every deadline and timeout off, so that only the protocol's own re-sending can get
the client to 4; a deadline would otherwise turn a hang into a tidy -3 or -4.

Two constants reproduce the class, as reversed witnesses:

- `RepeatKeepAlive = FALSE`: the server sends the establishing keep-alive once, at the
  response, and no more until the client acknowledges one. This is the protocol
  without the repeated keep-alive of TRANSPORT.md t13.1.
- `KeepAliveClock = "anysend"`: the keep-alive is paced off the last send of anything,
  so a payload stream suppresses it. This is the #90 regression the server's comment
  at `rocketnet_server_protocol_write_keep_alive_if_due` records.

The code's behaviour is `RepeatKeepAlive = TRUE`, `KeepAliveClock = "keepalive"`.

## What the model abstracts away

- **Keys.** A key pair is `[att, nonce]`: the attempt's salt and the mapping's session
  nonce are the two fresh inputs every key derives from, so "opens under the held keys"
  is equality on that pair. The ephemeral keypair is a boolean (live or erased). The
  AEAD open and the integrity limit (2^36 failed opens) are not modelled: a packet under
  the wrong pair is delivered and discarded. So the teardown at the integrity limit, by
  which an observer sending junk under the session's addresses can end an attempt
  (`sdk/rocketnet_constants.h` `ROCKETNET_INTEGRITY_LIMIT`), is a code transition no
  module has; a roadmap entry for it is proposed (`integrity-limit-model`). The small-order check on
  the challenge is the `SmallOrderServer` branch of `ClientRecvChallenge`.
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
  `Timeout`, `Deadline` or `ServerDeadline` of 0 is off. In the code neither can be
  0 (connect refuses a token whose timeout is not positive, and the total deadline is
  always set), so `Deadline = 0` is the model asking whether the protocol alone gets
  there.
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
wire at `Capacity` with duplicated copies exactly when the sender must send, and Link
disables `Send` at `Capacity` rather than dropping, so the protocol's send is blocked
rather than lost, forever. That is the model-size knob acting as a hidden blocker, not
the protocol; see "What TLC found".

The reversed witnesses are the check that this fairness is not vacuous: under it the
code's behaviour passes and the two bad behaviours fail.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19), from `proofs/tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 8 -cleanup -deadlock -lncheck final \
     -config MCClientConnection.cfg MCClientConnection
```

**Payloads** are accepted only in state 4 (TRANSPORT.md t11.3): the server's
first payloads can reach the client ahead of the keep-alive that connects it
(the keep-alive lost, as `KindLoss = "firstkind"` does, or reordered behind
them), and a payload delivered in state 3 let the application reply on a
connection that could not yet send (the reply refused `SEND_NOT_CONNECTED`
and lost). The action property `PayloadOnlyWhenConnected` states that a
payload is accepted only while connected.
`MCClientConnectionBrokenEarlyPayload.cfg` is its reversed witness: it adds
`ClientRecvEarlyPayload`, the pre-fix acceptance in state 3, and must fail
`PayloadOnlyWhenConnected`.

`MCClientConnection.cfg` is the committed run: the hang check under "the first packet
of every kind is dropped" (`KindLoss = "firstkind"`), the code's keep-alive behaviour,
deadlines and timeouts off, payloads on, no disconnects, checking every safety property
plus `Connects` and `ServerNotStranded`. The other runs are the same file with the lines
below changed, except the seven `Reach-*` runs, whose constants are their S case's with
only the measured changes the table names; the TSV has one row per run.

| Run | Lines changed from the committed cfg | Spec | Checks beyond the safety set | Distinct states | Result |
|---|---|---|---|---|---|
| `L-firstkind` | committed | Spec | Connects, ServerNotStranded | 32712 | pass |
| `L-none` | KindLoss "none" | Spec | Connects, ServerNotStranded | 14521 | pass |
| `L-first` | KindLoss "none", C2SPattern "first", S2CPattern "first" | Spec | Connects, ServerNotStranded | 14489 | pass |
| `L-alternate` | KindLoss "none", C2SPattern "alternate", S2CPattern "alternate" | Spec | Connects, ServerNotStranded | 35385 | pass |
| `L-burst` | KindLoss "none", C2SPattern "burst", S2CPattern "burst" | Spec | Connects, ServerNotStranded | 14487 | pass |
| `L-arbitrary` | KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary" | SpecTyped | Connects, ServerNotStranded | 71874 at the 110 s stop | bench debt: arbitrary-loss-liveness-over-budget |
| `W-once-firstkind` | RepeatKeepAlive FALSE | Spec | Connects | 1752 | fail as expected |
| `W-once-alternate` | KindLoss "none", RepeatKeepAlive FALSE, C2SPattern "alternate", S2CPattern "alternate" | Spec | Connects | 22849 | fail as expected |
| `W-anysend` | KeepAliveClock "anysend" | Spec | Connects | 32712 | fail as expected |
| `L-silent` | Timeout 2, ServerMute TRUE, KindLoss "none" | Spec | SilentServerTimesOut, AttemptResolves | 20 | pass |
| `L-crash` | Timeout 3, Deadline 6, ServerDeadline 4, MaxNonces 2, Disconnects TRUE, KindLoss "none" | Spec | CrashedServerTimesOut, AttemptResolves | - at the 110 s stop | bench debt: crash-timeout-liveness-over-budget |
| `S-none` | Timeout 2, Deadline 4, ServerDeadline 3, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", MaxDelay 1, Duplication TRUE, Reordering TRUE | Spec | safety only | - at the 110 s stop | bench debt: no reduced safety check (`Reach-none` is a reachability witness only) |
| `S-first` | Timeout 2, Deadline 5, ServerDeadline 3, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "first", S2CPattern "first", Duplication TRUE, Reordering TRUE | Spec | safety only | - at the 110 s stop | bench debt since the early send: reaches connected; no reduced safety check (`Reach-first` is a reachability witness only) |
| `S-alternate` | MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "alternate", S2CPattern "alternate", Duplication TRUE, Reordering TRUE | Spec | safety only | - at the 110 s stop | bench debt: no reduced safety check (`Reach-alternate` is a reachability witness only) |
| `S-burst` | MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "burst", S2CPattern "burst", Duplication TRUE, Reordering TRUE | Spec | safety only | - at the 110 s stop | bench debt: no reduced safety check (`Reach-burst` is a reachability witness only) |
| `S-arbitrary` | Timeout 2, Deadline 5, ServerDeadline 3, MaxAttempts 2, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", Duplication TRUE, Reordering TRUE | Spec | safety only | - at the 110 s stop | bench debt: no reduced safety check (`Reach-arbitrary` is a reachability witness only) |
| `S-firstkind` | Timeout 2, Deadline 4, ServerDeadline 3, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, MaxDelay 1, Duplication TRUE, Reordering TRUE | Spec | safety only | - at the 110 s stop | bench debt since the early send: reaches connected; no reduced safety check (`Reach-firstkind` is a reachability witness only) |
| `S-delay` | Timeout 2, Deadline 4, ServerDeadline 3, MaxNonces 2, KindLoss "none", C2SPattern "first", S2CPattern "first", MaxDelay 1, Duplication TRUE, Reordering TRUE | Spec | safety only | - at the 110 s stop | bench debt since the early send: reaches connected; no reduced safety check (`Reach-delay` is a reachability witness only) |
| `Reach-none` | the S-none constants | SpecReach | NotReached | 86216 | fail as expected (NotReached violated) |
| `Reach-first` | the S-first constants, Timeout 3 | SpecReach | NotReached | 19163 | fail as expected (NotReached violated) |
| `Reach-alternate` | the S-alternate constants | SpecReach | NotReached | 118680 | fail as expected (NotReached violated) |
| `Reach-burst` | the S-burst constants | SpecReach | NotReached | 22592 | fail as expected (NotReached violated) |
| `Reach-arbitrary` | the S-arbitrary constants | SpecReach | NotReached | 69297 | fail as expected (NotReached violated) |
| `Reach-firstkind` | the S-firstkind constants, Timeout 5, ServerDeadline 4, Deadline 8, Disconnects FALSE, OthersMayJoin FALSE | SpecReach | NotReached | 1009269 | fail as expected (NotReached violated) |
| `Reach-delay` | the S-delay constants, Timeout 3 | SpecReach | NotReached | 16493 | fail as expected (NotReached violated) |
| `F4-ephemeral` | Timeout 2, ServerMute TRUE, KindLoss "none" | Spec | EphemeralErasedOutsideHandshake | 20 | pass |
| `F8-terminal` | Timeout 2, MaxAttempts 2, Disconnects TRUE, ServerMute TRUE, KindLoss "none" | Spec | TerminalAbsorbing | 149 | pass |
| `keys-erased` | Timeout 2, Deadline 4, ServerDeadline 3, MaxAttempts 1, MaxNonces 1, OthersMayJoin FALSE, Disconnects TRUE, KindLoss "none", FIFO links | Spec | SafetyInvariants, KeysErasedWhenDown | 21987 | pass |
| `W-down-keeps-keys` | the keys-erased constants | SpecDownKeepsKeys | KeysErasedWhenDown | 151 | fail as expected |
| `W-denied-keeps-keys` | Timeout 0, Deadline 0, OthersMayJoin TRUE, Disconnects FALSE, ServerPayloads FALSE, KindLoss "none" | SpecDeniedKeepsKeys | KeysErasedWhenDown | 250 | fail as expected |

The small instance: `SendInterval = 1`, `MaxIds = 2`, `MaxSlots = 1`,
`DisconnectCopies = 2`, `Capacity = 2`, `BurstStart = 1`, `BurstLen = 2`. Liveness runs
(`L-*`, `W-*`): a FIFO link, `MaxNonces = 1`, `MaxAttempts = 1`, every deadline off.
Safety runs (`S-*`): duplication and reordering on, `MaxNonces = 2`, the application and
the server may disconnect, other clients may take the slot; `S-firstkind` and `S-delay`
add a tick of latency; `S-arbitrary` adds a second attempt (a reconnect, so a late
packet of an old attempt meets a new one). `L-arbitrary` under `SpecTyped` and `L-crash`
do not finish inside the 110 s per-case budget, and `S-none`, `S-alternate`, `S-burst`
and `S-arbitrary` do not either: they are explicit bench debt. No reduced case checks
their safety properties. Each names its reachability witness (`Reach-none`,
`Reach-alternate`, `Reach-burst`, `Reach-arbitrary`), which shows only that CONNECTED with
a payload is reachable under the same constants; it is not a smaller safety check, and
the safety at those constants is checked only by the weekly debt run. Under `Spec` the arbitrary-loss instance fails with the
type-targeted drop of "What TLC found" 9, which is the fairness being too weak, not the
protocol; the arbitrary-loss liveness therefore stands on the hand replay of that lasso
and on the four deterministic patterns, not on a completed run. `S-first`, `S-firstkind`
and `S-delay` passed at their `Timeout 2` constants while the request waited a whole
send interval after the cookie: the idle timeout fired first and the run covered the
handshake up to it. With the early send (TRANSPORT.md t11 rule 7) the request leaves
in the update the cookie advanced the client, the attempt reaches connected under the
same constants, and the three cases explore the full environment like `S-none`: they
are bench debt at the 110 s stop, and their connected coverage is the reduced
reachability witnesses `Reach-first`, `Reach-firstkind` and `Reach-delay`.

### Loss patterns explored

Each run names its patterns in the TSV. Across the runs TLC explored: nothing lost
(`"none"`); the first send on each link lost (`"first"`); every other send lost
(`"alternate"`: "the first packet dropped, the second through", forever); sends 1 and 2
lost (`"burst"`); any copy dropped at any time (`"arbitrary"`); and the first packet of
every kind lost (`KindLoss = "firstkind"`, applied before the link). In every S-environment run
both links also duplicate and reorder, and in `S-firstkind`, `S-delay` and the three
findings runs delay by a tick, so an old packet arriving late and a replayed packet are
in every safety run. The liveness runs use a FIFO link (the fairness section says why).

## What TLC found

Every counterexample below was replayed by hand through the functions it names before
it was written down. Three are findings in the code (all three already in
`docs/TLA-INVENTORY.md`, now reproduced as traces); the rest are model errors fixed on
the way, or the reversed witnesses doing their job.

### Connected under the S environment (reversed reachability witnesses)

The S-environment cases are the only safety runs with `Duplication` and `Reordering`
on, so they are the only place a connected client sees duplicated and reordered
packets. Three of them set `Timeout 2`, and at that timeout the client never connects:
the idle timeout fires before the cookie-carrying request leaves. A scratch reachability
probe over `MCClientConnectionSFirst.cfg` that checks `~(cstate = 4)` completes with no
violation: 9,355 states generated, 2,234 distinct. Its only route runs out like this:

1. connect (`cstate` 1, `cSinceSend` 1);
2. `ClientSend` sends the first request, which `C2SPattern = "first"` loses;
3. a tick (`cSinceRecv` 1);
4. the second request enters the wire; the server answers twice, the first cookie lost
   to `S2CPattern = "first"`;
5. `ClientRecvCookie` takes `cstate` to 2 with `cSinceRecv` still 1 — a cookie restarts
   no clock (`OnlyAuthenticatedRefreshes`);
6. one tick takes `cSinceRecv` to 2 and `Timeout 2` fires: `cstate` 2 -> -3, before any
   request carrying the cookie goes out (`cReqFams` stays `{}`).

`SpecReach` adds one step, `ClientRecvPayloadWitness`: exactly the payload acceptance
`ClientRecvSession` makes in connected, with the delivery recorded in the ghost
`cPayloadDelivered`. `ReachConnected` is `cstate = 4 /\ cPayloadDelivered` and
`NotReached` is its negation. Each `MCClientConnectionReach*.cfg` runs `SpecReach` under
its S case's loss pattern and checks `NotReached`; TLC violates it in every one, so
connected with a payload delivered is reachable under every S environment. `ReachSNone`,
`ReachSAlternate`, `ReachSBurst` and `ReachSArbitrary` use their S case's constants
unchanged. `ReachSFirst`, `ReachSDelay` and `ReachSFirstkind` raise the timeout to the
measured values that let the handshake finish (3, 3 and 5); first-of-kind loss also
drops the first response and the first keep-alive, so `ReachSFirstkind` raises the
server handshake deadline to 4 and the client deadline to 8 and turns the application
branches off (no disconnects, no other clients) to stay inside the 110 s budget. The
three S cases at `Timeout 2` remain the controls: they pass and cover the handshake up
to that timeout, not connected.

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
6. **Every way down zeroes the session keys** (`keys-erased`, `KeysErasedWhenDown`,
   pass; `W-down-keeps-keys`, fail as expected). `rocketnet_client_set_state`
   zeroes the keys and the ephemeral (`rocketnet_client_connection_reset`) at every
   state at or below `DISCONNECTED`, and every automatic terminal transition goes
   through it: the deadline and the idle timeout (`rocketnet_client_check_deadlines`),
   a denial, and a received disconnect. The model's `Tick` (`TickRule` with `Erase`
   TRUE) and `ClientRecvSession` (`ClientRecvSessionRule` with `Erase` TRUE) zero
   `cKeys` as the code does. `SpecDownKeepsKeys` adds the pre-fix rules
   (`TickKeepsKeys`, `ClientRecvSessionKeepsKeys`) that leave the keys, and TLC
   finds a timeout from state 3 or 4 that reaches a state below 0 with the keys set.

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
(`rocketnet_client_protocol_disconnect`, `sdk/rocketnet_client_protocol.cpp`;
`rocketnet_server_protocol_disconnect_client`, `sdk/rocketnet_server_protocol.cpp`).
If every copy is lost the receiver waits for its idle timeout, which STANDARD.md states as
the design ("losing all ten costs only a receiver that waits for the timeout"). In this
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
| 23 | `MCClientConnectionSNone.cfg` | pass; debt: full-environment-safety-over-budget-no-reduced-safety-check-MCClientConnectionReachSNone.cfg-is-a-reachability-witness-only |
| 24 | `MCClientConnectionSFirst.cfg` | pass; debt: first-loss-early-send-reaches-connected-full-environment-safety-over-budget-reduced-safety-check-is-MCClientConnectionSFirstSmall.cfg |
| 25 | `MCClientConnectionSAlternate.cfg` | pass; debt: alternate-loss-full-environment-safety-over-budget-no-reduced-safety-check-MCClientConnectionReachSAlternate.cfg-is-a-reachability-witness-only |
| 26 | `MCClientConnectionSBurst.cfg` | pass; debt: burst-loss-full-environment-safety-over-budget-no-reduced-safety-check-MCClientConnectionReachSBurst.cfg-is-a-reachability-witness-only |
| 27 | `MCClientConnectionSArbitrary.cfg` | pass; debt: arbitrary-loss-safety-over-budget-no-reduced-safety-check-MCClientConnectionReachSArbitrary.cfg-is-a-reachability-witness-only |
| 28 | `MCClientConnectionSFirstkind.cfg` | pass; debt: firstkind-loss-early-send-reaches-connected-full-environment-safety-over-budget-no-reduced-safety-check-MCClientConnectionReachSFirstkind.cfg-is-a-reachability-witness-only |
| 29 | `MCClientConnectionSDelay.cfg` | pass; debt: delay-early-send-reaches-connected-full-environment-safety-over-budget-no-reduced-safety-check-MCClientConnectionReachSDelay.cfg-is-a-reachability-witness-only |
| 30 | `MCClientConnectionF4Ephemeral.cfg` | pass |
| 31 | `MCClientConnectionF8Terminal.cfg` | pass |
| 33 | `MCClientConnectionBrokenDeniedKeepsKeys.cfg` | invariant / KeysErasedWhenDown |

Seven reversed reachability witnesses have no historical row: `MCClientConnectionReachSNone.cfg`, `MCClientConnectionReachSFirst.cfg`, `MCClientConnectionReachSAlternate.cfg`, `MCClientConnectionReachSBurst.cfg`, `MCClientConnectionReachSArbitrary.cfg`, `MCClientConnectionReachSFirstkind.cfg` and `MCClientConnectionReachSDelay.cfg` (invariant / NotReached).
`MCClientConnectionKeysErased.cfg` (pass) and its reversed witness `MCClientConnectionBrokenDownKeepsKeys.cfg` (invariant / KeysErasedWhenDown) replace historical row 32's observation, which recorded the model keeping the keys where the code zeroes them.

### The cookie restarts nothing (OnlyAuthenticatedRefreshes)

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

Every declared outcome in the `clientconnection` group holds in the current
records in `proofs/tla/RUNS.tsv`, one bounded run per case under `tlacheck` and its
110 s cap: `CookieTrust` passes and `BrokenCookieRefreshes` reaches
`OnlyAuthenticatedRefreshes`. `SFirst` (2,234 distinct), `SFirstkind` (12,162)
and `SDelay` (2,196) pass at their `Timeout 2` constants, which covers the
handshake up to the idle timeout and never connected; the connected coverage
those constants owe is the `Reach-first`, `Reach-firstkind` and `Reach-delay`
witnesses above.

### A forged cookie never rewinds the handshake (HandshakeNeverRewinds)

A cookie is now `"none"`, `"good"` (the server's) or `"forged"`. `ForgeCookie`
is the observer of the request as an environment action, enabled by the
definition `Forgery` (`Forgery <- MCForgery` in the forgery cfgs, `FALSE`
everywhere else): in requesting cookie (1) or sending connection request (2)
it sends a cookie echoing the attempt's salt with a MAC the client cannot
check. The server answers a request whose cookie is not `"good"` with a
fresh cookie. `ClientRecvCookie` keeps the first cookie of the attempt
(`cKept`, `return_cookie`) and stores a later one only as the latest
(`cLatest`, `latest_cookie`); `ClientSend` alternates the two once a latest
is stored (`cAltLatest`, `send_latest_cookie`), starting with the kept one,
matching `rocketnet_client_process_cookie` and
`rocketnet_client_protocol_update`. A latest cookie of the kept cookie's
value is not recorded, because a request carries the same value either way;
every run without forgery therefore keeps its state space
(`CookieTrust` is still 28,946 distinct states).

The action property `HandshakeNeverRewinds` says that within one attempt in
the positive states the state never goes down and a kept `"good"` cookie is
never taken away. `MCClientConnectionForgedCookie.cfg` checks it under
forgery with a responsive server and Timeout 3, with `SafetyInvariants`,
`OnlyAuthenticatedRefreshes` and `StepsForwardOrFails`: pass, 162,727
distinct states. `MCClientConnectionBrokenCookieReplaces.cfg` is the
reversed witness: the pre-fix cookie that replaces the one stored
(`ClientRecvCookieReplaces`, only in `SpecCookieReplaces`). TLC exits 13
with `HandshakeNeverRewinds` violated in 7 states: connect, the request, the
server's cookie, the cookie kept (`cKept` `"good"`, `cstate` 2), a forged
cookie, then the forged cookie delivered and `cKept` `"forged"`. Checked
against the code by hand: before the fix `rocketnet_client_process_cookie`
copied every accepted cookie over `return_cookie` in sending connection
request, so the next request carried the forged cookie and drew a fresh
cookie instead of the challenge, which
`test_client_forged_cookie_never_displaces_kept_cookie` catches on the old
code. A forgery that beats the server's first cookie in requesting cookie
(1) is kept; that is a step forward on a packet the standard has always
accepted there, not a rewind, and the server's cookie then rides as the
latest.

### A connect from a failure state is a fresh attempt (FreshAttemptFromFailure)

`AppConnect` already ran from any `cstate <= 0` and reset the attempt by
construction (commit 9095b711 lets `rocketnet_client_connect` start over
from every negative state; `rocketnet_client_protocol_connect` runs
`rocketnet_client_connection_reset` and takes a fresh salt, a fresh
ephemeral keypair and sequences at one). No property said so. The action
property `FreshAttemptFromFailure` does: a step from `cstate < 0` to 1
takes `cAttempt + 1` and leaves no keys, no cookie, no client index, an
empty replay window (`cSeen`), the ephemeral live and both
timers at zero.

`MCClientConnectionRetryFromFailure.cfg` is the control: a responsive
server of one slot that another client may take (`OthersMayJoin`), the idle
timeout 3 and the total deadline 5, two attempts, with `SafetyInvariants`,
`FreshAttemptFromFailure`, `DisconnectIsTerminal`, `TerminalAbsorbing` and
`StepsForwardOrFails`: pass, 224,404 distinct states. The instance reaches
-1 (denied, a slot taken), -2, -3, -4, -5 (the four timeouts) and -7 (the
deadline) and connects again from each; each was checked on the bench with
a scratch action property `[][~(cstate = k /\ cstate' = 1)]_vars`, which
TLC violates for every k in {-1, -2, -3, -4, -5, -7}. The cookie restarts
no clock, so -4 needs the deadline past the timeout and -7 needs it inside
the timeout of a late challenge: Timeout 2 with Deadline 3 misses -4, with
Deadline 4 misses -7. -6 (invalid token) is decided before anything is sent
and is not modelled (the module header); `rocketnet_client_protocol_connect`
resets before its token checks, so a connect from -6 is the same path, and
`test_connection_reset_connect_from_each_failure_state` covers it.

`MCClientConnectionBrokenRetryKeepsAttempt.cfg` is the reversed witness.
In the code the failed attempt is reset twice: `rocketnet_client_process_denied`
and the deadline and timeout paths set the failure state through
`rocketnet_client_set_state`, which runs `rocketnet_client_connection_reset` at
any state at or below DISCONNECTED, and `rocketnet_client_protocol_connect` runs
it again before anything else. `ClientRecvDenied` and `Tick` model the first
reset (both cookies, the client index and the replay window go with the keys),
and `AppConnect` the second. The witness removes both:
`SpecRetryKeepsAttempt` adds the pre-fix denial `ClientRecvDeniedKeepsKeys`
(the failure state with nothing zeroed) and the pre-fix retry
`AppConnectKeepsAttempt` (a connect that keeps the cookies and `cSeen`). TLC
exits 13 with `FreshAttemptFromFailure` violated (113 generated / 58 distinct
states), a trace of 9 states: connect, the request, the server's cookie, the
cookie kept (`cKept` `"good"`, state 2), the early request with the cookie,
the server's denial (its one slot is held), `ClientRecvDeniedKeepsKeys` to -1
still holding the cookie, then `AppConnectKeepsAttempt` to state 1 at attempt
2 with `cKept` still `"good"`. Checked against the code by hand: either reset
alone zeroes `has_cookie` and `return_cookie`, so only a step that skips both,
as the witness does, carries them into the new attempt. The witness checks
`FreshAttemptFromFailure` alone: the pre-fix denial from state 2 also leaves
the ephemeral live at -1, which `EphemeralErasedOutsideHandshake` would
report first. Its constants (Timeout 0, Deadline 0, so the denial's -1 is
the only failure state) differ from `RetryFromFailure`'s, which predates
this revision.

The `clientconnection` group has 40 cases and `tlacheck groups --root . --stale`
prints `[]`. Every required case reaches its declared result: `RetryFromFailure`
passes and `BrokenRetryKeepsAttempt` exits 13 on `FreshAttemptFromFailure`, and the
seven `Reach-*` witnesses each exit 12 on `NotReached`. The six bench debt cases
(`LArbitrary`, `LCrash`, `SNone`, `SAlternate`, `SBurst`, `SArbitrary`) exit 124 at
the 110 s budget.

On 2026-11-01 the reconciled model was checked with the pinned native runner
and TLC jar on Linux: all 34 required cases reached their declared outcomes.
The six declared bench cases were remeasured at the unchanged 110-second
budget and timed out (exit 124); their current records remain debt, not passes.

### Revision 1.1: the connect clock stops at the connection response

Standard rule: TRANSPORT.md t11 rule 6. The total deadline (the connect
token's lifetime) bounds requesting cookie (1) and sending connection request
(2) only. Once the client sends its connection response (3) the server may
already report it connected, so in state 3 only the idle timeout ends the
wait. `DeadlineStates` is `1..2`; the definition `DeadlineInResponse`
(replaced with `<-` in a configuration) restores `1..3`, the rule before
revision 1.1. `DeadlineHonoured` and the `dead` predicate of `TickRule` read
`DeadlineStates`.

New action property `NoConnectClockAfterResponse == [][cstate = 3 => cstate'
# -7]_vars`: state 3 never ends with connect token expired.

| Case | Expected | Exit | Distinct |
|---|---|---:|---:|
| `MCClientConnectionConnectClock.cfg` (`Timeout = Deadline = 3`, `SafetyInvariants`, `KeysErasedWhenDown`, the property) | pass | 0 | 114188 |
| `MCClientConnectionBrokenDeadlineInResponse.cfg` (the same with `DeadlineInResponse`) | `NoConnectClockAfterResponse` | 13 | 174 |

The pass case sets the idle timeout equal to the total deadline so the client
can reach state 3 with one tick of the deadline left and stay there through
it; with `Timeout = 2` and `Deadline = 4` (`MCClientConnectionKeysErased.cfg`)
the idle timeout always fires first and the witness cannot fail. The witness
differs only in the deadline predicate, so the state it fails from is
reachable in the pass case too. The `clientconnection` group now has 41
cases.

The code: `rocketnet_client_check_deadlines` (`sdk/rocketnet_client_protocol.cpp`)
tests the total deadline only while `state <
ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE`; before
revision 1.1 it tested it in every state below connected (4), state 3 included.

Witness trace, read by hand (11 states): connect (state 1), the request
and the cookie (state 2), one tick (age 1), the request with the cookie and
the challenge (state 3, `cSinceRecv = 0`), two ticks: at age 3 = `Deadline`
the client has heard nothing for 2 ticks, under `Timeout = 3`, and the old
predicate takes it to −7. Against the C: before revision 1.1 state 3 is
below CONNECTED, so `current_time - attempt_start_time >=
total_deadline_seconds` ended it with connect token expired; the code now
skips the test in state 3 and only the silence timeout can end it there.

### The early send is bounded by the advances (EarlySendPerAdvance)

Standard rule: TRANSPORT.md t11 rule 7. The advance from requesting cookie
(1) to sending connection request (2), made by the first accepted cookie,
and the advance from 2 to sending connection response (3), made by the
challenge, each arm one send of the new state's packet at the next update
(`handshake_early_send`, set in `rocketnet_client_process_cookie` and
`rocketnet_client_process_challenge`, taken in
`rocketnet_client_protocol_update`). Before it the client waited out the send
interval after every reply: on a fast link a handshake took three send
intervals, about 200 ms at 10 Hz.

`cEarly` is the arm; `ClientEarlySend` sends `ClientPacket` in 2 or 3 when it
is set; every send (`ClientSendNow`, shared with `ClientSend`) clears it and
restarts the interval. The ghost `cEarlySends` counts early sends per attempt,
and `EarlySendPerAdvance` (in `SafetyInvariants`, so every run checks it)
holds them to the advances: none in 1, at most one in 2, at most two in the
attempt. `NoSendOnArrival` still holds: no receive step sends.

| Case | Expected | Exit |
|---|---|---:|
| `MCClientConnectionForgedCookie.cfg` (a forger echoing the salt; `EarlySendPerAdvance` in `SafetyInvariants`) | pass | 0 |
| `MCClientConnectionBrokenEarlyAnyCookie.cfg` (the same with `EarlyArmsOnAnyCookie`: every accepted cookie arms the send) | `EarlySendPerAdvance` | 12 |
| `MCClientConnectionSFirstSmall.cfg` (duplication and reordering on, first-packet loss on both links; one nonce, one id, no other clients, no disconnects) | pass | 0 |
| `MCClientConnectionReachSFirstSmall.cfg` (its reach witness: `NotEarlyConnected`) | `NotEarlyConnected` | 12 |

Witness trace, read by hand (8 states): connect (state 1); the forger sends
a cookie echoing the salt; the client accepts it (state 2, armed); the early
request goes out (`cEarlySends = 1`); the server answers that request with
its own cookie; the client accepts it in state 2 and, under the witness, is
armed again; a second early request goes out in state 2 (`cEarlySends = 2`).
Against the C: `rocketnet_client_process_cookie` sets
`handshake_early_send` only inside the branch that moves requesting cookie to
sending connection request; a cookie accepted in state 2 replaces the latest
cookie and arms nothing, so a stream of cookies, forged or genuine, brings no
request forward after the first.

Under the S environments the early send changes what three cases reach.
`S-first`, `S-firstkind` and `S-delay` timed the client out at `Timeout 2`
before the cookie-carrying request left, because that request waited a whole
interval after the cookie; it now leaves in the same update, the attempt
reaches connected, and the three are bench debt at the 110 s stop like
`S-none`. `MCClientConnectionReachSFirstkind.cfg` grew from 42 s to 76 s and
carries a declared budget. Duplicated and reordered datagrams stay in the
required set through `MCClientConnectionSFirstSmall.cfg`, sized to the budget,
and its reach witness shows that case reaches connected through both early
sends. The `clientconnection` group now has 44 cases.

### Authenticated refusals, the small-order challenge and the server's ack

Three code paths had no action: a session packet that opens under the
session keys and is replay-marked, then refused (a bad ack block, a size
wrong for its type, a keep-alive whose client index is past the slot
count); a challenge whose server key is a small-order point, which the
client ignores; and the server's standalone ack, which refreshes the
client's receive clock. Each is a definition replaced with `<-`
(`AuthenticRefusals`, `SmallOrderServer`, `ServerAcks`, all `FALSE` in the
module, so every other case keeps its state space) and folded into
`ClientRecvSession` and `ClientRecvChallenge`; the ack's send is the
environment action `ServerAck`. `MCClientConnectionAuthenticRefusals.cfg`
turns all three on (`AuthenticOn`) with a responsive server, Timeout 3,
Deadline 5, one attempt, `SafetyInvariants`, `KeysErasedWhenDown`,
`EphemeralErasedOutsideHandshake` and the action properties
`StepsForwardOrFails`, `TimeoutPairing`, `AcceptedUnderLiveKeys`,
`PayloadOnlyWhenConnected`, `NoSendOnArrival`, `OnlyAuthenticatedRefreshes`,
`HandshakeNeverRewinds` and `AttemptResolves`: pass, 710,786 generated /
136,081 distinct states, 39.0 s on the bench. `ServerAck` produces 7,419
distinct states in that run (TLC coverage).

The server environment no longer refreshes a connected mapping's idle
clock on a connection response (`ServerRecvResponse`): the code refreshes
only a half-open mapping and ignores a connected one's response, as
ServerConnection has it.

The `clientconnection` group has 45 cases. Nine are FAIL/124 at the 110 s
case limit and are declared debt, not proofs: `LArbitrary`, `LCrash`,
`SNone`, `SFirst`, `SAlternate`, `SBurst`, `SArbitrary`, `SFirstkind` and
`SDelay`. The safety they carry under those loss patterns has never
completed at their constants; the reduced checks are `SFirstSmall` and the
seven `Reach-*` witnesses, which show reachability only.
`CrashedServerTimesOut` is checked only by `LCrash` and has no completed
run.

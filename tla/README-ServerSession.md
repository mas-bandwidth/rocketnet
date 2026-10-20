# ServerSession: the server object's life, its slots across clients, and the per-slot reset

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

`tla/ServerSession.tla` models the server object over its life: create (STOPPED, no transport
bound), start, clients connecting into free slots, sending and receiving, leaving by their own
disconnect, a timeout, a denial during the handshake, `disconnect_client` or
`disconnect_all_clients`, stop (every client disconnected, then STOPPED), start again, and
destroy. The handshake packet by packet is `tla/ServerConnection.tla`, which this module does
not extend; this one models each slot's life across many clients and the one per-slot `Reset(i)`
between them. `tla/MCServerSession.tla` and its cfg files are the instances; every run is a row
in `tla/RUNS.tsv` (group `serversession`). Line numbers below are at `14993f6` (the base of this
change).

## What the model covers

**The server object.**

| Variable | Code | Meaning |
|---|---|---|
| `state` | `struct rocketnet_server_transport_t` (`sdk/rocketnet_server_transport.cpp:183`); `ROCKETNET_SERVER_TRANSPORT_RUNNING`, `_STOPPING`, `_STOPPED` (`sdk/rocketnet.h:1156-1160`) | `NONE` (no object), `STOPPED`, `RUNNING`, `STOPPING` |
| `slot[i].st`, `slot[i].sess` | `struct rocketnet_client_slots_t` (`sdk/rocketnet_client_slots.cpp:14`): `connected[i]`, assigned by `rocketnet_client_slots_assign` (`:163`) and freed by `rocketnet_client_slots_release` (`:251`) | `FREE`, `HANDSHAKE` (a mapping is open for the slot), `CONNECTED`; the session tag of the slot's client |
| `slot[i].rec[f]` | the slot's per-connection state: the encryption mapping (`sdk/rocketnet_encryption_map.cpp`), the slot record, and the message layer's connection (`rn_message_connection_reset`, `sdk/rocketnet_message_server.cpp:127`) | one field per row of the reset inventory, each 0 (Init) or the tag of the session that wrote it |
| `slot[i].reason` | `rocketnet_message_server_disconnect_reason` (`sdk/rocketnet.h:2400`) | kept through the reset, cleared to NONE when the slot's next client is established |
| `kept.used` | the used-token history, `rocketnet_used_token_history_check` and `_record` (`sdk/rocketnet_token_tables.cpp:229`, `:241`), held at `sdk/rocketnet_server_protocol.h:63` | server-wide, never per slot; per run: fresh at each start (a start builds a new server transport, which creates the history), kept across slot resets and the stop within the run (standard/TRANSPORT.md t13.4) |
| `kept.keys`, `kept.config`, `kept.lifetime` | the cookie key and server keys (rotation at `sdk/rocketnet_server_protocol.cpp:382`), the config, the lifetime counters (`sdk/rocketnet.h:1687-1690`) | kept across slots, stops and starts; fresh only at create |
| `draining` | `disconnect_all_clients` in progress | no client is accepted until every slot is free |
| `link` | the path from the clients | the session tags of client packets in flight (lossy: `Lose`) |

`stop` exists (`rocketnet_server_transport_stop`, `sdk/rocketnet_server_transport.cpp:1306`, sets
STOPPING), as do create (`:696`), destroy (`:1269`) and `disconnect_client` (`:2362`, over
`rocketnet_server_protocol_disconnect_client`, `sdk/rocketnet_server_protocol.cpp:1920`). A
server created STOPPED with no transport bound, `start`, start again after stop, and
`disconnect_all_clients` do not exist at `14993f6`; the card
rocketnet-client-connect-disconnect-reconnect writes them, and the card
rocketnet-per-connection-reset-is-complete writes the one per-slot reset function and the
inventory each field below stands for, citing this model.

**The per-slot record** (`Fields`), grouped by the step that writes it:

| Written at | Fields |
|---|---|
| `ClientConnects(i, t)` (the mapping is created) | `token_state` (the slot's challenge, cookie and token state), `client_id`, `user_data`, `timeouts`, `session_keys` |
| `ServerSend(i)` | `packet_sequence`, `message_sequence`, `sent_packets`, `reliable_send_queue`, `ordering_indices`, `send_budget`, `connection_counters`, `ack_receipt_ring` |
| `Deliver(s)` (a client packet opens) | `received_packets`, `reliable_receive_queue`, `ack_bits`, `replay_window`, `fragment_reassembly`, `rtt_estimator`, `loss_estimator`, `connection_counters` |
| `DisconnectClient(i)`, `DrainStep(i)` | `pending_disconnect` (the disconnect burst) |

**The used-token history is per run.** standard/TRANSPORT.md t13.4 (Slot reset): a slot
reset keeps the used-token history, and a server that starts again begins with a fresh one.
The code does the same: `rocketnet_server_start` builds a new message server and server
transport (`sdk/rocketnet_api.cpp:1149`, `sdk/rocketnet_message_server.cpp:1035`), the
transport creates an empty history (`sdk/rocketnet_server_transport.cpp:1439`), stop destroys
it with the transport (`sdk/rocketnet_api.cpp:1187`, `sdk/rocketnet_server_transport.cpp:1513`),
and the slot reset (`rocketnet_server_connection_reset`, `sdk/rocketnet_connection_reset.cpp:61`)
does not touch it (line numbers at `dc2ff1f9`). The model's `Start` empties `kept.used`; every
`Reset(i)` and the stop keep it. Every request in this model comes from its own address, the
replay case the history exists for, so a token in the history is refused (`TokenAdmitsOnce`:
one connection per token per run); the standard binds a token to its first source address,
and a re-presentation from that same address is not modelled here. A token used before a stop
is admitted again after the next start; `MCServerSessionReadmitAfterStart.cfg` proves that
state reachable. The
restart sentences under _Keys_ and _Per-connection keys_ (`STANDARD.md:189`, `:1058`) make key
freshness independent of the history: the session nonce is chosen when the mapping is created,
so a restart can never recreate a key. STANDARD.md:189 has the server generate its cookie key "at start"; the
model keeps the keys as one value fresh at create and unchanged by stop and start, as the card
lists them kept. A cookie key regenerated at each start instead would invalidate only
outstanding cookies, which no slot record holds, so no property here depends on the choice.

**Actions.**

| Action | API or event |
|---|---|
| `Create`, `Destroy` | create (STOPPED, fresh object); destroy from any state (client packets in flight outlive it) |
| `Start` | start: refused with a status code unless STOPPED |
| `Stop`, `DrainStep(i)`, `StopComplete` | stop: STOPPING, no client accepted; each occupied slot is sent its disconnect and `Reset(i)`; then STOPPED |
| `DisconnectAll`, `DrainStep(i)`, `DrainDone` | `disconnect_all_clients`: every occupied slot is disconnected and reset before another client is accepted |
| `ClientConnects(i, t)` | a request with token `t` opens a mapping into free slot `i`, a new session; a token in the used-token history is refused (each request comes from its own address, so a used token is a replay) |
| `Establish(i)` | the response opens: the token is recorded in the history, the slot is CONNECTED, its reason cleared |
| `Deny(i)`, `SlotTimeout(i)`, `ClientLeaves(i)`, `DisconnectClient(i)` | a denial during the handshake; the slot's timeout; the client's own disconnect; `disconnect_client(i)` with its burst |
| `ServerSend(i)` | message start/finish to a connected slot |
| `ClientSend(s)`, `Deliver(s)`, `Lose(s)` | the client of any session ever started sends (also after its connection ended); the packet arrives at that session's slot (the same address reconnecting into the same slot is the worst case) and opens only under the keys it was sealed with, into a connected slot |

Restart is `Stop`, `StopComplete`, `Start`. Every path out of a slot's connection goes through
`EndSlot`, which applies the one `Reset(i)`.

## Properties

| Property | Kind | The rule |
|---|---|---|
| `TypeOK`, `SlotOwnsItsSession` | invariant | types; an occupied slot holds its session, a free one none |
| `StartOnlyFromStopped` | invariant | start succeeds only from STOPPED |
| `NoSendUnlessConnected` | invariant | no message is queued to a slot that is not connected, and no send-side field is written outside CONNECTED |
| `ResetIsInit` | invariant | a free slot's per-connection fields are all Init; the history holds every token established in this run, the lifetime counter every connection of the object, and the keys and config stay |
| `RecordIsCurrent` | invariant | a slot's record holds only Init and its current session's writes |
| `SlotReuseObservesNothing` | invariant | per slot, no packet of session s opens in another session, and no step reads a field another session wrote (a stale ack, a stale fragment, a replay-window false reject) |
| `NoClientWhileStopped` | invariant | no slot is occupied while STOPPED (or with no object) |
| `StopDisconnectsAll` | invariant | after a stop every slot is free and Init, and every connected client the server ended was sent its disconnect (the second part is structural: every server-initiated end passes the burst, and no witness breaks it) |
| `ReasonKeptUntilNextConnection` | invariant | a slot's reason equals the one its ended connection set until its next client is established, where it is NONE |
| `ResetTouchesOnlyItsSlot` | invariant | `Reset(i)` leaves every slot `j # i` unchanged |
| `TokenAdmitsOnce` | invariant | a token admits one connection per run: the history is kept across slot resets and fresh at each start |
| `HandshakeResolves` | liveness | under weak fairness of the response, the denial and the timers, a slot in HANDSHAKE leaves it |
| `StopReachesStopped` | liveness | under weak fairness of the update that drains, STOPPING reaches STOPPED |

## Reversed witnesses

| cfg | Defect | Expected | Counterexample, last state |
|---|---|---|---|
| `MCServerSessionUnresetSlot.cfg` | `Defect = "unresetslot"`, `UnresetField = "replay_window"`: `Reset(i)` skips the replay window | `SlotReuseObservesNothing`, exit 12 | 12 states: create, start, session 1 connects into slot 1 and is established, its packet opens (`replay_window = 1`), the slot times out (the reset leaves `replay_window = 1`), session 2 connects into slot 1 and is established, its first packet opens: `slot[1].sess = 2`, `observed = {[slot \|-> 1, field \|-> "replay_window"]}`, the new client's first sequence checked against the old client's window |
| `MCServerSessionStopLeavesClient.cfg` | `Defect = "stopleavesclient"`: stop skips the last slot | `NoClientWhileStopped`, exit 12 | 6 states: create, start, session 1 opens a mapping into slot 2, stop, StopComplete: `state = "STOPPED"`, `slot[2].st = "HANDSHAKE"`, `slot[2].sess = 1` |
| `MCServerSessionTouchesOtherSlot.cfg` | `Defect = "touchesotherslot"`: `Reset(i)` also clears slot i+1 | `ResetTouchesOnlyItsSlot`, exit 12 | 6 states: create, start, sessions 1 and 2 open mappings into slots 1 and 2, slot 1 is denied: both slots `FREE`, slot 1's reason `DENIED`, `collateral = {2}`: session 2 vanished with no reason and no reset path of its own |
| `MCServerSessionResetClearsHistory.cfg` | `Defect = "resetclearshistory"`: `Reset(i)` empties the used-token history | `TokenAdmitsOnce`, exit 12 | 8 states: create, start, session 1 connects with token 1 and is established (`kept.used = {1}`), the slot times out and the reset empties the history, session 2 connects with token 1 and is established: `doubleTok = TRUE`, one token admitted twice in one run |
| `MCServerSessionReadmitAfterStart.cfg` | `Defect = "none"`, `MaxStarts = 2`; invariant `NoReadmitAfterStart` (a reachability witness) | `NoReadmitAfterStart`, exit 12 | 11 states: create, start, session 1 established with token 1, stop, its slot drained, STOPPED, start (`kept.used = {}`, `priorTokens = {1}`), session 2 connects with token 1 and is established: `readmitted = TRUE`, the fresh history at start reached |

Each trace was read step by step against the defect its constant names. With the UnresetSlot
instance and `INVARIANT ResetIsInit` instead, TLC fails `ResetIsInit` at the first reset (8
states: the timeout after session 1's packet opened leaves `replay_window = 1` in a free slot);
that run is a bench check, not a recorded case. With `SPECIFICATION Spec` (no fairness) and the
main constants, `HandshakeResolves` fails by stuttering with a slot in HANDSHAKE and
`StopReachesStopped` by stuttering in STOPPING, so neither property is vacuous under `LiveSpec`.

## Runs

Measured with the pinned runner and JAR of `tla/README.md` (Linux, two workers for the passing
case, one for witnesses, 110-second cap per case):

| cfg | Result | Generated | Distinct | Seconds | Exit |
|---|---|---:|---:|---:|---:|
| `MCServerSession.cfg` | pass | 444,363 | 65,987 | 13.077 | 0 |
| `MCServerSessionUnresetSlot.cfg` | `SlotReuseObservesNothing` violated | 87,120 | 19,643 | 3.384 | 12 |
| `MCServerSessionStopLeavesClient.cfg` | `NoClientWhileStopped` violated | 193 | 137 | 1.823 | 12 |
| `MCServerSessionTouchesOtherSlot.cfg` | `ResetTouchesOnlyItsSlot` violated | 91 | 79 | 1.701 | 12 |
| `MCServerSessionResetClearsHistory.cfg` | `TokenAdmitsOnce` violated | 1,820 | 947 | 0.683 | 12 |
| `MCServerSessionReadmitAfterStart.cfg` | `NoReadmitAfterStart` violated | 51,389 | 14,069 | 1.173 | 12 |

## Bounds

- `MCServerSession.cfg`: `SlotCount = 2`, `MaxSessions = 2` (two clients over the run, so a
  slot is reused or both slots are occupied), `MaxTokens = 2`, `MaxStarts = 2` (start, stop,
  start again), `MaxCreates = 1`, `Defect = "none"`, `SPECIFICATION LiveSpec`. The same
  constants with `MaxSessions = 3` pass every invariant under `Spec` (1,010,515 distinct states,
  81 seconds on the bench) but do not fit the cap with the liveness properties.
- The witnesses: `SlotCount = 2`, `MaxSessions = 2`, `MaxTokens = 2`, `MaxStarts = 1`,
  `MaxCreates = 1`, `SPECIFICATION Spec`.
- Deadlock is not checked (`ignore-terminal`): a run ends when sessions, starts and creates are
  spent.
- The link is a set of session tags (at most one packet per session in flight). The handshake
  is a mapping opening and one response; its packets, cookies, keys, the encryption map and the
  client-id check are `tla/ServerConnection.tla`'s. Server packets to clients are not modelled
  here (the client's receive side is `tla/ClientSession.tla`). Tokens do not expire inside a run.

# ServerSession: the server object's life, its slots across clients, and the per-slot reset

`tla/ServerSession.tla` models the server object over its life: create (STOPPED, no transport
bound), start, clients connecting into free slots, sending and receiving, leaving by their own
disconnect, a timeout, a denial during the handshake, `disconnect_client` or
`disconnect_all_clients`, stop (every client disconnected, then STOPPED), start again, and
destroy. The handshake packet by packet is `tla/ServerConnection.tla`, which this module does
not extend; this one models each slot's life across many clients and the one per-slot `Reset(i)`
between them. `tla/MCServerSession.tla` and its cfg files are the instances; every run is a row
in `tla/RUNS.tsv` (group `serversession`). The code is cited by file and symbol.

## What the model covers

**The server object.**

| Variable | Code | Meaning |
|---|---|---|
| `state` | `struct rocketnet_server_transport_t` (`sdk/rocketnet_server_transport.cpp`); `ROCKETNET_SERVER_TRANSPORT_RUNNING`, `_STOPPING`, `_STOPPED` (`sdk/rocketnet_layers.h`) | `NONE` (no object), `STOPPED`, `RUNNING`, `STOPPING` |
| `slot[i].st`, `slot[i].sess` | `struct rocketnet_client_slots_t` (`sdk/rocketnet_client_slots.cpp`): `connected[i]`, assigned by `rocketnet_client_slots_assign` and freed by `rocketnet_client_slots_release` | `FREE`, `HANDSHAKE` (a mapping is open for the slot), `CONNECTED`; the session tag of the slot's client |
| `slot[i].rec[f]` | the slot's per-connection state: the encryption mapping (`sdk/rocketnet_encryption_map.cpp`), the slot record, and the message layer's connection (`rocketnet_message_connection_reset`, `sdk/rocketnet_connection_reset.cpp`) | one field per row of the reset inventory, each 0 (Init) or the tag of the session that wrote it |
| `slot[i].reason` | `rocketnet_message_server_disconnect_reason` (`sdk/rocketnet_layers.h`), returned by `rocketnet_server_disconnect_reason` | kept through the reset, cleared to NONE when the slot's next client is established |
| `kept.used` | the used-token history, `rocketnet_used_token_history_check` and `rocketnet_used_token_history_record` (`sdk/rocketnet_token_tables.cpp`), held in `struct rocketnet_server_protocol_t` (`sdk/rocketnet_server_protocol.h`) | server-wide, never per slot, kept across slots, stops and starts |
| `kept.keys`, `kept.config`, `kept.lifetime` | the cookie key and server keys (rotation in `rocketnet_server_protocol_rotate_cookie_key_if_due`, `sdk/rocketnet_server_protocol.cpp`), the config, the lifetime counters | kept across slots, stops and starts; fresh only at create |
| `draining` | `disconnect_all_clients` in progress | no client is accepted until every slot is free |
| `link` | the path from the clients | the session tags of client packets in flight (lossy: `Lose`) |

In the transport (`sdk/rocketnet_server_transport.cpp`), `rocketnet_server_transport_stop` sets
STOPPING; create is `rocketnet_server_transport_create`, destroy
`rocketnet_server_transport_destroy`, and `rocketnet_server_transport_disconnect_client` runs
over `rocketnet_server_protocol_disconnect_client` (`sdk/rocketnet_server_protocol.cpp`). The
public server is this lifecycle object: `rocketnet_server_create` makes it stopped with no
transport bound, `rocketnet_server_start` and `rocketnet_server_stop` run it, it can start
again after a stop, and `rocketnet_server_disconnect_all_clients` drains every slot. The
code cites this model where the server is defined (`sdk/rocketnet_api.cpp`), and the one
per-slot reset function and the inventory each field below stands for are in
`sdk/rocketnet_connection_reset.h`.

**The per-slot record** (`Fields`), grouped by the step that writes it:

| Written at | Fields |
|---|---|
| `ClientConnects(i, t)` (the mapping is created) | `token_state` (the slot's challenge, cookie and token state), `client_id`, `user_data`, `timeouts`, `session_keys` |
| `ServerSend(i)` | `packet_sequence`, `message_sequence`, `sent_packets`, `reliable_send_queue`, `ordering_indices`, `send_budget`, `connection_counters`, `ack_receipt_ring` |
| `Deliver(s)` (a client packet opens) | `received_packets`, `reliable_receive_queue`, `ack_bits`, `replay_window`, `fragment_reassembly`, `rtt_estimator`, `loss_estimator`, `connection_counters` |
| `DisconnectClient(i)`, `DrainStep(i)` | `pending_disconnect` (the disconnect burst) |

**The used-token history across stop and start.** The model keeps the history in `kept`, so
it survives stop and start and only destroy discards it, with the object. The code and the
standard do not: [t13.4 Slot reset](../standard/TRANSPORT.md#t13.4) has a server that starts
again begin with a fresh used-token history and no key from the previous run, and
`rocketnet_server_start` (`sdk/rocketnet_api.cpp`) builds a fresh transport on every start,
and creating a transport creates a new history and cookie key.
`TokenAdmitsOnce` across a restart is therefore a property of the model's kept history, not
of the code; within one run the two agree. Every request in
this model comes from its own address, the replay case the history exists for, so a token in
the history is refused (`TokenAdmitsOnce`, which holds by that guard while the history is
kept); the standard binds a token to its first source address
([t12 Handshake: server](../standard/TRANSPORT.md#t12)), and a re-presentation from that
same address is not modelled here. Key freshness does not depend on the history: the session
nonce is chosen when the mapping is created, so a restart can never recreate a key. The
model keeps the cookie key and server keys as one value fresh at create and unchanged by stop
and start, as the reset inventory keeps them across slots; the cookie key the code generates
at each start ([t2.2 Key inventory](../standard/TRANSPORT.md#t2.2)) invalidates only
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
| `ResetIsInit` | invariant | a free slot's per-connection fields are all Init; the history holds every token this object established, the lifetime counter every connection, and the keys and config stay |
| `RecordIsCurrent` | invariant | a slot's record holds only Init and its current session's writes |
| `SlotReuseObservesNothing` | invariant | per slot, no packet of session s opens in another session, and no step reads a field another session wrote (a stale ack, a stale fragment, a replay-window false reject) |
| `NoClientWhileStopped` | invariant | no slot is occupied while STOPPED (or with no object) |
| `StopDisconnectsAll` | invariant | after a stop every slot is free and Init, and every connected client the server ended was sent its disconnect (the second part is structural: every server-initiated end passes the burst, and no witness breaks it) |
| `ReasonKeptUntilNextConnection` | invariant | a slot's reason equals the one its ended connection set until its next client is established, where it is NONE |
| `ResetTouchesOnlyItsSlot` | invariant | `Reset(i)` leaves every slot `j # i` unchanged |
| `TokenAdmitsOnce` | invariant | a token admits one connection over the object's life, across stop and start |
| `HandshakeResolves` | liveness | under weak fairness of the response, the denial and the timers, a slot in HANDSHAKE leaves it |
| `StopReachesStopped` | liveness | under weak fairness of the update that drains, STOPPING reaches STOPPED |

## Reversed witnesses

| cfg | Defect | Expected | Counterexample, last state |
|---|---|---|---|
| `MCServerSessionUnresetSlot.cfg` | `Defect = "unresetslot"`, `UnresetField = "replay_window"`: `Reset(i)` skips the replay window | `SlotReuseObservesNothing`, exit 12 | 12 states: create, start, session 1 connects into slot 1 and is established, its packet opens (`replay_window = 1`), the slot times out (the reset leaves `replay_window = 1`), session 2 connects into slot 1 and is established, its first packet opens: `slot[1].sess = 2`, `observed = {[slot \|-> 1, field \|-> "replay_window"]}`, the new client's first sequence checked against the old client's window |
| `MCServerSessionStopLeavesClient.cfg` | `Defect = "stopleavesclient"`: stop skips the last slot | `NoClientWhileStopped`, exit 12 | 6 states: create, start, session 1 opens a mapping into slot 2, stop, StopComplete: `state = "STOPPED"`, `slot[2].st = "HANDSHAKE"`, `slot[2].sess = 1` |
| `MCServerSessionTouchesOtherSlot.cfg` | `Defect = "touchesotherslot"`: `Reset(i)` also clears slot i+1 | `ResetTouchesOnlyItsSlot`, exit 12 | 6 states: create, start, sessions 1 and 2 open mappings into slots 1 and 2, slot 1 is denied: both slots `FREE`, slot 1's reason `DENIED`, `collateral = {2}`: session 2 vanished with no reason and no reset path of its own |

Each trace was read step by step against the defect its constant names. With the UnresetSlot
instance and `INVARIANT ResetIsInit` instead, TLC fails `ResetIsInit` at the first reset (8
states: the timeout after session 1's packet opened leaves `replay_window = 1` in a free slot);
that run is a manual check, not a recorded case. With `SPECIFICATION Spec` (no fairness) and the
main constants, `HandshakeResolves` fails by stuttering with a slot in HANDSHAKE and
`StopReachesStopped` by stuttering in STOPPING, so neither property is vacuous under `LiveSpec`.

## Runs

Measured with the pinned runner and JAR of `tla/README.md` (Linux, two workers for the passing
case, one for witnesses, 110-second cap per case):

| cfg | Result | Generated | Distinct | Seconds | Exit |
|---|---|---:|---:|---:|---:|
| `MCServerSession.cfg` | pass | 333,379 | 47,443 | 24.818 | 0 |
| `MCServerSessionUnresetSlot.cfg` | `SlotReuseObservesNothing` violated | 87,120 | 19,643 | 3.384 | 12 |
| `MCServerSessionStopLeavesClient.cfg` | `NoClientWhileStopped` violated | 193 | 137 | 1.823 | 12 |
| `MCServerSessionTouchesOtherSlot.cfg` | `ResetTouchesOnlyItsSlot` violated | 91 | 79 | 1.701 | 12 |

## Bounds

- `MCServerSession.cfg`: `SlotCount = 2`, `MaxSessions = 2` (two clients over the run, so a
  slot is reused or both slots are occupied), `MaxTokens = 2`, `MaxStarts = 2` (start, stop,
  start again), `MaxCreates = 1`, `Defect = "none"`, `SPECIFICATION LiveSpec`. The same
  constants with `MaxSessions = 3` pass every invariant under `Spec` (1,010,515 distinct states,
  81 seconds) but do not fit the cap with the liveness properties.
- The witnesses: `SlotCount = 2`, `MaxSessions = 2`, `MaxTokens = 2`, `MaxStarts = 1`,
  `MaxCreates = 1`, `SPECIFICATION Spec`.
- Deadlock is not checked (`ignore-terminal`): a run ends when sessions, starts and creates are
  spent.
- The link is a set of session tags (at most one packet per session in flight). The handshake
  is a mapping opening and one response; its packets, cookies, keys, the encryption map and the
  client-id check are `tla/ServerConnection.tla`'s. Server packets to clients are not modelled
  here (the client's receive side is `tla/ClientSession.tla`). Tokens do not expire inside a run.

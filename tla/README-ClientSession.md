# ClientSession: the client object's life across connections, and the per-connection reset

**Shutdown scope:** This model retains its earlier synchronous shutdown
abstraction and exact historical evidence. The approved request/completion API
is separately modeled in [RNClientIoShutdown](README-RNClientIoShutdown.md).
That additive ownership model is not a proved refinement of this one; existing
data-path/session checks do not establish the new shutdown implementation.

`tla/ClientSession.tla` models the client object over many connections: create (no token,
DISCONNECTED), connect(token), the handshake abstracted to its three stages, CONNECTED, every
path out of a connection, disconnect back to DISCONNECTED, connect again, and destroy. The
handshake packet by packet is `tla/ClientConnection.tla`, which this module does not extend;
this one models what that one does not: the per-connection state one connection writes and the
one `Reset` that clears it before the next connection. `tla/MCClientSession.tla` and its cfg
files are the instances; every run is a row in `tla/RUNS.tsv` (group `clientsession`). Line
numbers below are at `14993f6` (the base of this change).

## What the model covers

**The client object.**

| Variable | Code | Meaning |
|---|---|---|
| `st` | `struct rocketnet_client_transport_t` (`sdk/rocketnet_client_transport.cpp:120`), read by `rocketnet_client_transport_state` (`:1591`); the state codes are `ROCKETNET_CLIENT_TRANSPORT_STATE_*` (`sdk/rocketnet.h:705-716`) | `NONE` (no object), `DISCONNECTED`, the three handshake stages, `CONNECTED`, and the seven error states |
| `cur` | ghost | the session tag of the connection in progress, 0 outside one |
| `conn[f]` | the per-connection fields of `struct rocketnet_client_protocol_t` (`sdk/rocketnet_client_protocol.h:57`: the send sequences `:91-92`, the replay windows `:94-95`, the reassembly set `:136`) and the message layer above it | one field per row of the reset inventory, each 0 (Init) or the tag of the session that wrote it |
| `reason` | `rocketnet_message_client_disconnect_reason` (`sdk/rocketnet.h:2324`) | the disconnect reason, kept through the reset, cleared to NONE when the next connection reaches CONNECTED |
| `kept` | the lifetime counters and the config | kept across connections; fresh only at create |
| `link` | the path from the server | the session tags of server packets in flight (lossy: `Lose`) |

The connect-without-token API (`create`, then `connect(token)`, `disconnect`, connect again)
does not exist at `14993f6`: today the token is passed to `rocketnet_client_transport_create`
(`sdk/rocketnet_client_transport.cpp:293`) and `rocketnet_client_transport_disconnect`
(`:1618`) ends the object's only connection. The card rocketnet-client-connect-disconnect-reconnect
writes that API and its single reset function; the card rocketnet-per-connection-reset-is-complete
writes the reset inventory each field below stands for and cites this model.

**The per-connection record** (`Fields`), grouped by the step that writes it:

| Written at | Fields |
|---|---|
| `Connect` (the token) | `token_state` (challenge, cookie and token state), `client_id`, `user_data`, `timeouts` |
| the challenge (`SENDING_CONNECTION_RESPONSE`) | `session_keys` (keys and nonces) |
| `Send` (message start/finish) | `packet_sequence`, `message_sequence`, `sent_packets`, `reliable_send_queue`, `ordering_indices`, `send_budget`, `connection_counters`, `ack_receipt_ring` |
| `Deliver` (a packet opens) | `received_packets`, `reliable_receive_queue`, `ack_bits`, `replay_window`, `fragment_reassembly`, `rtt_estimator`, `loss_estimator`, `connection_counters` |
| `ClientDisconnect` | `pending_disconnect` (the disconnect burst) |

**Kept on purpose**: `kept.lifetime` (the lifetime counters, monotonic over the client's life),
`kept.config`, and `reason`. Everything else is reset.

**Actions.**

| Action | API or event |
|---|---|
| `Create`, `Destroy` | create (DISCONNECTED, a fresh object); destroy from any state (packets in flight outlive it) |
| `Connect(tok)` | connect(token): refused with a status code unless DISCONNECTED (a refused call changes nothing); a valid token starts a session; an invalid or expired token goes straight to its error state |
| `HandshakeStep` | the server's reply to the current stage arrives; the challenge writes the session keys; the establishing keep-alive reaches CONNECTED and clears the reason |
| `Deny`, `TokenExpires`, `Timer` | a denial during the handshake; the token expiring during the handshake; each stage's deadline and the connected timeout |
| `ServerDisconnect`, `ClientDisconnect` | the server's disconnect; the client's disconnect from the handshake or connected |

The model's reasons map to `enum rocketnet_disconnect_reason_t` (`sdk/rocketnet.h:339-347`):
`ServerDisconnect`'s `SERVER_DISCONNECTED` is `ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED`
(`6`), recorded by the message object's client terminal check
(`sdk/rocketnet_message_server.cpp:477-478`) when the client transport counted the server's
disconnect packet (`sdk/rocketnet_client_protocol.cpp:1202`, read by
`sdk/rocketnet_message_client.cpp:89`). The public API carries the reason in the client
state: `SERVER_DISCONNECTED` reads `ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED` from
`rocketnet_client_state`, a `Timer` end reads the timed-out state, and `ClientDisconnect`'s
`CLIENT_DISCONNECTED` returns the client to `ROCKETNET_CLIENT_STATE_DISCONNECTED`.
| `DisconnectFromError` | disconnect from an error state: back to DISCONNECTED, the reason kept |
| `Send` | message start/finish, only while CONNECTED (the `queuebeforeconnected` defect also enables it in a handshake state) |
| `DropSend` | message start outside CONNECTED (`rocketnet_client_start_message` returns NULL and counts the `messages_dropped_not_connected` stat; the message layer's `MESSAGE_SEND_NOT_CONNECTED`, `rn_message_object_start` in `sdk/rocketnet_message_server.cpp`): no per-connection field changes, nothing is held; the ghost `droppedIn` is `{st}` after a drop and `{}` after any other step (RN-2 m1.5) |
| `ServerSend(s)`, `Deliver(s)`, `Lose(s)` | the server's packets of any session ever started, delivered, or lost; a packet opens only under the session keys it was sealed with |

Every path out of a connection goes through `End`, which applies the one `Reset`. Reconnect is
`Connect` from DISCONNECTED after an earlier session.

**Reads.** Every field is read by some step of the next session (`HandshakeReads`, `SendReads`,
`RecvReads`, with an `ASSUME` that together they cover `Fields`), so a field a reset misses is
observed: a stale ack bit or sent entry (`Deliver` reads `ack_bits`, `sent_packets`,
`ack_receipt_ring`), a stale fragment slot (`fragment_reassembly`), a replay window that refuses
the new session's first sequences (`replay_window`).

## Properties

| Property | Kind | The rule |
|---|---|---|
| `TypeOK`, `CurrentOnlyWhileLive` | invariant | types; a session is in progress exactly in the live states |
| `ConnectOnlyFromDisconnected` | invariant | connect succeeds only from DISCONNECTED |
| `NoSendUnlessConnected` | invariant | no message is queued outside CONNECTED, and no send-side field is written outside it |
| `DropLeavesNothing` | invariant | a drop happens only outside CONNECTED (and with an object), and outside CONNECTED no send-side field holds anything, so nothing dropped is sent once the connection reaches CONNECTED |
| `ResetIsInit` | invariant | outside a connection every per-connection field is Init; the lifetime counter equals the connections this object made, and the config stays |
| `RecordIsCurrent` | invariant | the record holds only Init and the current session's writes |
| `ReconnectObservesNothing` | invariant | no packet of session s opens in session s' # s, and no step reads a field another session wrote |
| `ReasonKeptUntilNextConnection` | invariant | the reason equals the one the ended connection set until the next connection reaches CONNECTED, where it is NONE (the ghost `lastEnd` is written in the same step as the reason on every end path, so this catches a reason cleared or overwritten by a reset, a connect or a handshake step; no witness breaks it) |
| `ConnectReachesConnectedOrError` | liveness | under weak fairness of the handshake replies and the timers, a handshake stage reaches CONNECTED or an error (or the application disconnects or destroys first) |

## Reversed witnesses

| cfg | Defect | Expected | Counterexample, last state |
|---|---|---|---|
| `MCClientSessionUnreset.cfg` | `Defect = "unreset"`, `UnresetField = "replay_window"`: Reset skips the replay window | `ReconnectObservesNothing`, exit 12 | 13 states: create, connect, two handshake steps, a packet of session 1 opens (`replay_window = 1`), the client disconnects (the reset leaves `replay_window = 1`), connect again, two handshake steps, a packet of session 2 opens: `cur = 2`, `st = "SENDING_CONNECTION_RESPONSE"`, `observed = {"replay_window"}`. The new session's first packet is checked against session 1's window: the replay-window false reject. |
| `MCClientSessionConnectAnywhere.cfg` | `Defect = "connectanywhere"`: connect succeeds in every state | `ConnectOnlyFromDisconnected`, exit 12 | 4 states: create, connect, connect again from the handshake: `cur = 2`, `st = "REQUESTING_COOKIE"`, `connectFrom = {"REQUESTING_COOKIE", "DISCONNECTED"}` |
| `MCClientSessionQueueBeforeConnected.cfg` | `Defect = "queuebeforeconnected"`: `Send` also runs in a handshake state, a send queued before CONNECTED | `NoSendUnlessConnected`, exit 12 | 4 states: create, connect, a send in the first handshake stage: `st = "REQUESTING_COOKIE"`, `queuedIn = {"REQUESTING_COOKIE"}`, the send-side fields written with session 1's tag. In the code that is `rocketnet_client_start_message` after `rocketnet_client_connect` and before CONNECTED, which the message layer now drops (`DropSend`) instead of queueing |

Every trace was read step by step against the defect each constant names. With the Unreset
instance and `INVARIANT ResetIsInit` instead, TLC fails `ResetIsInit` at the first reset (8
states: a denial after a packet of session 1 opened leaves `replay_window = 1` in
`CONNECTION_DENIED`); that run is a bench check, not a recorded case. With `SPECIFICATION Spec` (no
fairness) and the main constants, `ConnectReachesConnectedOrError` fails by stuttering in a
handshake stage, so the property is not vacuous under `LiveSpec`.

## Runs

Measured with the pinned runner and JAR of `tla/README.md` (Linux, two workers for the passing
case, one for witnesses, 110-second cap per case):

| cfg | Result | Generated | Distinct | Seconds | Exit |
|---|---|---:|---:|---:|---:|
| `MCClientSession.cfg` | pass | 129,594 | 13,523 | 1.768 | 0 |
| `MCClientSessionUnreset.cfg` | `ReconnectObservesNothing` violated | 8,531 | 1,551 | 0.750 | 12 |
| `MCClientSessionConnectAnywhere.cfg` | `ConnectOnlyFromDisconnected` violated | 12 | 9 | 0.610 | 12 |
| `MCClientSessionQueueBeforeConnected.cfg` | `NoSendUnlessConnected` violated | 14 | 14 | 0.609 | 12 |

## Bounds

- `MCClientSession.cfg`: `MaxSessions = 3` (three connections over the run), `MaxCreates = 2`
  (destroy and create again once), `Defect = "none"`, `SPECIFICATION LiveSpec`.
- The witnesses: `MaxSessions = 2`, `MaxCreates = 1`, `SPECIFICATION Spec`.
- Deadlock is not checked (`ignore-terminal`): a run ends when the sessions and creates are
  spent.
- The link is a set of session tags (at most one packet per session in flight). The handshake
  is three abstract steps; its packets, cookies, keys and replay rules are
  `tla/ClientConnection.tla`'s. Client packets to the server are not modelled here (the server's
  receive side is `tla/ServerSession.tla`).

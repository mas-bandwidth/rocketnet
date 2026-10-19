# RNSecurityTables

The security tables at the table grain: the used-token history
(`sdk/rocketnet_token_tables.cpp`) and the one-live-connection rule (with an
admitted-token set as its reversed witness), the cookie gate
and its key generation, and the per-update verify budget, as
`sdk/rocketnet_server_protocol.cpp` drives them and
[t12 Handshake: server](../standard/TRANSPORT.md#t12) specifies them. The
module works at the grain of the token and cookie tables: the full handshake, the lossy link and the replay window are outside
it, and a request past the cookie gate, a response that opens under its
mapping, another client taking a slot, a disconnect and a tick are the only
events. The attacker is the quantification itself: any address
presents any token under any cookie generation it has seen. Only an
address in `Holds[t]` holds token `t`'s keys, so only it can open a
response under `t`; the instance has `tf` held at `A` alone, so `B`
replaying `tf` is an attacker that has captured a token and replays it from
another address, which t12 refuses by binding the token to the source address
whose connection response first opened under it.

- **The history.** Per token: absent, or bound to an address with an
  expiry, an insert order and a ghost `opened` (whether a response that
  opened made the record). A request bound to another address while
  the entry lives is refused. The record is made when a response opens
  under the mapping, from the responding address, and never for an
  expired token (`rocketnet_server_process_connection_response`).
  `BindAtRequest = TRUE` is the reversed witness that records at the
  request instead. Recording is a no-op for a live entry; otherwise it
  takes a free slot, then an expired slot, then (in the code) the oldest
  entry, live or not (`rocketnet_token_table_select_slot` with
  `evict_oldest = 1`, reached from `rocketnet_used_token_history_record`).
- **One live connection.** The code has no admitted-token set. A token is
  kept to one live connection by the history (another address is refused
  while the entry lives), by the connected-address check at the top of the request path
  (`CONNECTION_REQUEST_ALREADY_CONNECTED`, the `Request` guard on `slots`)
  and by the client-id check at the response (`CheckCidAtResponse`).
  `HoldUntilExpire = TRUE` is the reversed witness for `ChurnNeverDenied`:
  an admitted set of `AdmittedCap` entries, each held until its token
  expires, never evicted, checked at the request and the response
  (`RefuseAdmittedAtResponse`) and added to on admission.
- **The cookie gate and rotation.** A mapping is created only under a
  cookie of a live generation: the current key, or the previous one
  while `clock < prevUntil` (the cookie gate in
  `sdk/rocketnet_server_protocol.cpp`: `server->has_previous_cookie_key &&
  current_time < server->previous_cookie_key_expire_time`). The key is
  initialised at create (`sdk/rocketnet_server_transport.cpp`) and rotated on its period by `rocketnet_server_protocol_rotate_cookie_key_if_due`,
  called before the request cookie gate and from `rocketnet_server_protocol_update`.
  The receive path rotates before both verification and issuance because the
  transport drains receives before its sweep. An on-time rotation
  copies the outgoing key to `previous_cookie_key` and sets `prevUntil`
  (`previous_cookie_key_expire_time`) to the rotation instant plus
  `RotatePeriod`; a stall past a whole window zeroes the previous key
  and clears `has_previous_cookie_key`.
- **The verify budget.** Requests past the gate spend from a per-update
  budget (`verifies`, reset by the tick): the handshake work budget
  (`ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE`, checked in the request
  path before any asymmetric work) and the failed-verify budget with
  its 64 table slots (`ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS`).

## Invariants

- **`OneTokenOneLiveConnection`** (the headline rule, TokenUsedOnce as the
  server enforces it): no two client slots hold one token at once. A
  token whose connection has closed may connect again inside its short
  expiry; that is the accepted residual risk.
- **`ChurnNeverDenied`**: a fresh, unexpired, never-admitted token whose
  client id holds no slot, at a server with a free slot, is never refused
  or denied at the response (ghost `churnDenied`).
- **`BoundOnlyToOpener`**: every history entry was written by a response
  that opened under its token, from the address it binds; a request
  never binds.
- **`NoBindToNonHolder`**: no address outside `Holds[t]` is ever bound to
  `t`, so a replay from an address without the keys cannot lock the
  token's holder out.
- **`DeniedTokenStaysUsable`**: a step that sends a denied leaves the
  admitted set unchanged; the token is consumed only at the moment a
  connection is admitted.
- **`NoLiveHistoryEviction`**: recording never removes a history entry
  whose expiry is still ahead of the clock (the ghost `evictedLive`
  never fires).
- **`ExpiredNeverAdmitted`**: no slot is assigned under a token whose
  expiry has passed (the ghost `lateAdmit` never fires).
- **`RotationWindow`** (action property): a mapping is created only
  under a cookie generation that is live at that step.
- **`PreviousBoundedToOnePeriod`** (action property): a mapping is
  created under a non-current generation only while `clock < prevUntil`,
  and `prevUntil` never exceeds the instant the generation stopped
  being current plus `RotatePeriod`.
- **`InFlightCookieSurvivesRotation`** (action property): with
  `CookieLife < RotatePeriod`, a cookie issued under the current
  generation and presented within `CookieLife` is accepted across one
  rotation.
- **`VerifyBudgetRespected`**: verifies in one update never pass the
  budget, while the cap switch is in its code position.
- **`TablesBounded`**: neither table passes its capacity, and slots
  plus other clients never pass `MaxClients`.

## Switches: the code position and the other position

| Switch | Code position | Other position |
|---|---|---|
| `RefuseAdmittedAtResponse` | `TRUE`: an admitted token is refused at the response; it has an effect only with `HoldUntilExpire = TRUE`, since the code has no admitted set | `FALSE`: the add succeeds when the token is already present |
| `HoldUntilExpire` | `FALSE`: no admitted-token set | reversed witness `TRUE`: the set holds each token until expiry, denying churn |
| `CheckCidAtResponse` | `TRUE`: the response refuses a connected client id | reversed witness `FALSE`: two slots hold one token |
| `BindAtRequest` | `FALSE`: the history is recorded when a response opens | reversed witness `TRUE`: recorded at the request |
| `AdmitExpiredAtResponse` | `TRUE`: the response rung has no expiry check | `FALSE`: it refuses an expired token |
| `EvictLiveHistory` | `TRUE`: recording evicts the oldest, live or not | `FALSE`: it leaves a live entry in place |
| `DeniedConsumesToken` | `FALSE`: a denied adds nothing | mutant `TRUE` guards the ordering |
| `CapVerifyBudget` | `TRUE`: the per-update budget caps verifies | mutant `FALSE` guards the cap |
| `AcceptPrevious` | `TRUE`: previous generation accepted while `clock < prevUntil` | reversed witness `FALSE` drops the previous-key window |
| `PreviousOutlivesWindow` | `FALSE`: a stall past the window drops the previous generation | mutant `TRUE` keeps the previous generation across a stall |

## Findings

Each row is verified by hand against the code. A row whose code column
describes an admitted-token set or a binding at the request describes the
reversed witness's switch, not the shipped code; the last column says what the
code does instead. Cookie key rotation is guarded by its two reversed
witnesses.

| Invariant / Property | Trace | Code | Hand verification |
|---|---|---|---|
| `ChurnNeverDenied` | 9 states (`HoldUntilExpire = TRUE`) | an admitted-token set of `max_clients * 2` entries that refuses an add when full of unexpired entries, reported as `CONNECTION_RESPONSE_ADMITTED_SET_FULL` by `rocketnet_server_process_connection_response` | `A` connects `th` and disconnects, `B` connects `te`, then `A` requests the fresh `tf`: a slot is free but the set holds two unexpired entries at capacity two, so `A`'s response is denied. The code has no admitted set, and `CONNECTION_RESPONSE_ADMITTED_SET_FULL` always reads zero; `server_admits_sequential_fresh_tokens_past_twice_max_clients` connects five fresh tokens from five fresh addresses in turn at a one-slot server, each connecting and disconnecting, and checks that every one is admitted. |
| `OneTokenOneLiveConnection` | 5 states (`CheckCidAtResponse = FALSE`, with `RefuseAdmittedAtResponse = FALSE`) | the client-id check of `rocketnet_server_process_connection_response` | `A` and `B` each request the shared token `th` before either response binds it; `A`'s response admits; with the client-id check off, `B`'s response is admitted while `A` is connected, so two slots hold `th`. The code refuses it as `CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED`, pinned by `test_request_path_state_refusals` (one token, two addresses); `server_refuses_a_token_connected_now` pins the same token after a history flush at the request; `TestOneConnectionPerTokenAfterHistoryFlush` pins one client id, one live connection. |
| `BoundOnlyToOpener` | 2 states (`BindAtRequest = TRUE`) | `rocketnet_used_token_history_record` called from the request path, `rocketnet_server_process_opened_request` | `A`'s request for `th` writes the history entry before any response has opened: the request path records the source address before the challenge. The code records in `rocketnet_server_process_connection_response`, after the open. |
| `NoBindToNonHolder` | 2 states (`BindAtRequest = TRUE`, `BindAtRequestAttacker.cfg`) | the same record at the request; the history check at the top of the request path | `B`, which does not hold `tf`, replays `tf` and the request binds `tf` to `B`; `A`'s own request for `tf` is then refused as `TOKEN_ALREADY_USED` for the token's lifetime. `test_token_bound_at_response_open` runs this sequence against the code and checks `TOKEN_ALREADY_USED == 0`: with the binding at the response, `A` connects. |
| `ExpiredNeverAdmitted` | 5 states | request rung expiry check in `rocketnet_server_protocol_process_connection_request`; no expiry check in `rocketnet_server_process_connection_response` | A token presented before expiry whose response arrives after expiry is admitted: the request path refuses an expired token at its top, the response path checks only the client id and the slots. Bounded by the handshake deadline. |
| `NoLiveHistoryEviction` | 8 states | `rocketnet_used_token_history_record` and `rocketnet_token_table_select_slot(..., 1)` in `sdk/rocketnet_token_tables.cpp` | At capacity with no expired entry, recording a new token unlinks and reuses the oldest slot even while its entry is unexpired. This is the documented flush bound; it is what lets a second request for a flushed token pass the history check, which the client-id check then refuses while the first connection is live (`server_refuses_a_token_connected_now`). |
| `DeniedTokenStaysUsable` | 5 states (mutant) | the slots check in `rocketnet_server_process_connection_response`; a request-rung denied creates no mapping | The code holds this invariant: a denied for a full server consumes nothing, at both rungs (with `HoldUntilExpire`, it is sent before the set takes its entry). The reversed witness is a mutant that consumes the token on a denied; TLC rejects it, which guards the ordering against a future edit. |
| `TypeOK` verify bound | 7 states (mutant) | handshake budget check in the request path (`ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE` in `sdk/rocketnet_counters.h`); 64-slot failed-verify table in `sdk/rocketnet_server.h` | The code holds this invariant: past the cookie gate, verifies per update are counted and capped before asymmetric work runs. The reversed witness removes the cap; TLC rejects `SafetyInvariants` through the `TypeOK` bound when `verifies = 4` with `VerifyBudget = 2`. `VerifyBudgetRespected` itself is conditional on `CapVerifyBudget`, so it does not reject the disabled-cap mutant. |
| `InFlightCookieSurvivesRotation` | 3 states (`AcceptPrevious = FALSE`) | `sdk/rocketnet_server_protocol.cpp` (`rocketnet_server_protocol_rotate_cookie_key_if_due` and the previous-key branch of the cookie gate) | An on-time rotation moves the outgoing key to `previous_cookie_key` with `previous_cookie_key_expire_time = cookie_key_rotation_time + period`, so a cookie minted before the rotation is accepted across it while `current_time < previous_cookie_key_expire_time`. Turning `AcceptPrevious` off drops the outgoing generation at the rotation tick (`clock = 1 -> 2`) and fails `InFlightCookieSurvivesRotation`. |
| `PreviousBoundedToOnePeriod` | 2 states (mutant `PreviousOutlivesWindow = TRUE`) | `sdk/rocketnet_server_protocol.cpp` (`rocketnet_server_protocol_rotate_cookie_key_if_due`, stall branch when `current_time >= cookie_key_rotation_time + period`, and the gate check `current_time < server->previous_cookie_key_expire_time`) | When `update` stalls past an entire rotation window (`clock = 0 -> 4`), the outgoing key's acceptance window `[2, 4)` has already elapsed, so the code zeroes `previous_cookie_key` and clears `has_previous_cookie_key`. The mutant `PreviousOutlivesWindow = TRUE` keeps the outgoing key until `current_time + period` (`prevUntil = 6 > 4`), which TLC rejects on the stall step. |

## How to run it

On Linux with `tla2tools.jar` at `/opt/tla/tla2tools.jar`, from `tla/`:

```sh
java -XX:+UseParallelGC -cp /opt/tla/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNSecurityTables.cfg MCRNSecurityTables
```

Each reversed witness has a committed configuration registered in
`tla/CASES.tsv`, in group `rnsecuritytables`:

| Configuration suffix after `MCRNSecurityTables` | Reversed switch | Expected failure |
|---|---|---|
| `NoRefuseAdmitted.cfg` | `RefuseAdmittedAtResponse = FALSE`, `CheckCidAtResponse = FALSE` | `SafetyInvariants` (`OneTokenOneLiveConnection`) |
| `HoldUntilExpire.cfg` | `HoldUntilExpire = TRUE` | `SafetyInvariants` (`ChurnNeverDenied`) |
| `BindAtRequest.cfg` | `BindAtRequest = TRUE` | `SafetyInvariants` (`BoundOnlyToOpener`) |
| `BindAtRequestAttacker.cfg` | `BindAtRequest = TRUE` | `NoBindToNonHolder` |
| `AdmitExpired.cfg` | `AdmitExpiredAtResponse = TRUE` | `SafetyInvariants` (`ExpiredNeverAdmitted`) |
| `EvictLiveHistory.cfg` | `EvictLiveHistory = TRUE` | `SafetyInvariants` (`NoLiveHistoryEviction`) |
| `DeniedConsumesToken.cfg` | `DeniedConsumesToken = TRUE` (with `HoldUntilExpire = TRUE`: only a set entry can be consumed) | `DeniedTokenStaysUsable` |
| `NoVerifyCap.cfg` | `CapVerifyBudget = FALSE` | `SafetyInvariants` (`TypeOK` verify bound) |
| `NoPrevious.cfg` | `AcceptPrevious = FALSE` | `InFlightCookieSurvivesRotation` |
| `PreviousOutlivesWindow.cfg` | `PreviousOutlivesWindow = TRUE` | `PreviousBoundedToOnePeriod` |

Refresh all eleven cases through the pinned `tlacheck` and merge with
`--keep tla/RUNS.tsv` as `README.md` describes. The measured ledger contains
one row per configuration; `RUNS-HISTORY.tsv` holds the older seven-column
records.

The small instance:

| Constant | Value |
|---|---|
| `Addrs` | `{"A", "B"}` |
| `Tokens` | `{"th", "te", "tf"}` (`te` expires at tick 3) |
| `Holds` | `th`, `te` at `{"A", "B"}`; `tf` at `{"A"}` only |
| `MaxClock` | 4 |
| `CookieLife` / `RotatePeriod` | 1 / 2 |
| `HistoryCap` / `AdmittedCap` | 2 / 2 |
| `MaxClients` | 2 |
| `VerifyBudget` | 2 per update |
| `HandshakeDeadline` | 4 ticks |

The passing run (`MCRNSecurityTables.cfg`) generates 4,031,603 states and
finds 903,855 distinct states, with no error: the invariants hold and
`DeniedTokenStaysUsable`, `RotationWindow`,
`PreviousBoundedToOnePeriod` and `InFlightCookieSurvivesRotation` hold
as properties.

## What the model leaves out

- The passing run holds `EvictLiveHistory = FALSE`, so TLC does not check
  `OneTokenOneLiveConnection` under the code's live-entry eviction; the code
  holds it there by the client-id check, which the model applies at the
  response only (the code also applies it at the request after the open).
  `server_refuses_a_token_connected_now` pins the evicted case in the code.
- `DeniedTokenStaysUsable` constrains only the admitted set, so it is
  vacuous with `HoldUntilExpire = FALSE`; its witness runs with
  `HoldUntilExpire = TRUE`.
- `Tick` advances the clock and rotates atomically. It does not represent
  the transport receive-before-sweep ordering. Unit regressions exercise
  cookies issued before a delayed sweep, both near a rotation deadline and
  after a full-window stall, plus refusal of the outgoing key after a stall.
- Packets, the link, the replay window and the session keys are not
  modelled; a response here is a packet that has already opened under its
  mapping.
- The cookie is its generation only: the MAC is the attacker's
  inability to make one for a generation it has not seen, and a seen
  cookie for another address is refused by the address binding inside
  the history check's address, not modelled separately.
- Nonces and the ephemeral exchange are outside the tables; a mapping
  here is the token it was created for, its cookie generation and its
  birth tick, swept at the handshake deadline.

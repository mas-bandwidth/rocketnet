# RNSecurityTables

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

The security tables at the table grain: the used-token history
(`sdk/rocketnet_token_tables.cpp`) and the one-live-connection rule (the
admitted-token set it replaced survives as a reversed witness), the cookie gate
and its key generation, and the per-update verify budget, as
`sdk/rocketnet_server_protocol.cpp` drives them and STANDARD.md
specifies them. This module lands the held draft of `tla/Security.tla`
(branch `sprint/rn-tla-security.w3.g1.e15`, commit `2f8de9d`) at the
grain of inventory modules 5 and 6 in `docs/TLA-INVENTORY.md`: the full
handshake, the lossy link and the replay window stay in that draft;
here a request past the cookie gate, a response that opens under its
mapping, another client taking a slot, a disconnect and a tick are the
only events. The attacker is the quantification itself: any address
presents any token under any cookie generation it has seen. Only an
address in `Holds[t]` holds token `t`'s keys, so only it can open a
response under `t`; the instance has `tf` held at `A` alone, so `B`
replaying `tf` is the on-path attacker of STANDARD.md's replay paragraph.

- **The history.** Per token: absent, or bound to an address with an
  expiry, an insert order and a ghost `opened` (whether a response that
  opened made the record). A request bound to another address while
  the entry lives is refused. The record is made when a response opens
  under the mapping, from the responding address, and never for an
  expired token (`rocketnet_server_process_connection_response`);
  `BindAtRequest = TRUE` is the code before 2026-10-17, which recorded at
  the request. Recording is a no-op for a live entry,
  then a free slot, then an expired slot, then — in the code — the
  oldest entry, live or not (`rocketnet_token_table_select_slot`, reached from
  `rocketnet_used_token_history_record`).
- **One live connection.** The code has no admitted-token set (removed
  2026-10-18, `docs/SESSION.md` 1.6). A token is kept to one live
  connection by the history (another address is refused while the entry
  lives), by the connected-address check at the top of the request path
  (`CONNECTION_REQUEST_ALREADY_CONNECTED`, the `Request` guard on `slots`)
  and by the client-id check at the response (`CheckCidAtResponse`).
  `HoldUntilExpire = TRUE` is the code before: an admitted set of
  `AdmittedCap` entries, each held until its token expired, never evicted,
  checked at the request and the response (`RefuseAdmittedAtResponse`) and
  added to on admission; the model keeps it as the reversed witness for
  `ChurnNeverDenied`.
- **The cookie gate and rotation.** A mapping is created only under a
  cookie of a live generation: the current key, or the previous one
  while `clock < prevUntil` (`sdk/rocketnet_server_protocol.cpp:377`,
  `server->has_previous_cookie_key && current_time < server->previous_cookie_key_expire_time`).
  The key is initialised at create (`sdk/rocketnet_server_transport.cpp:832-838`)
  and rotated on its period by `rocketnet_server_protocol_rotate_cookie_key_if_due`,
  called before the request cookie gate and from `rocketnet_server_protocol_update`.
  The receive path rotates before both verification and issuance because the
  transport drains receives before its sweep. An on-time rotation
  copies the outgoing key to `previous_cookie_key` and sets `prevUntil`
  (`previous_cookie_key_expire_time`) to the rotation instant plus
  `RotatePeriod`; a stall past a whole window zeroes the previous key
  and clears `has_previous_cookie_key`.
- **The request-claim table.** `ClaimSlots` (two) slots, each holding
  the token, address and handshake deadline last written to it, a free
  list and an order list of claims, head first
  (`rocketnet_used_token_history_claim_can_record`, `_claim_record` and
  `_claim_release` in `sdk/rocketnet_token_tables.cpp`). A request is
  refused while another address holds a live claim on its token, and on
  capacity by the O(1) rule: the token is in the table, or the free list
  is not empty, or the order head's deadline has passed. Past the verify
  the claim is recorded: the token's own entry (an expired one is
  rewritten and moved to the tail), else the free list's head, else the
  expired order head. `RequestRefused` records then releases (a refusal
  past the verify; a same-address request after a disconnect finds its own
  live claim, records nothing and releases it from wherever it sits in the
  order list); a mapping swept by `Tick` at its handshake deadline
  releases nothing, as its claim expires at the same instant. The table's
  `ClaimRecord` then reclaims an expired order head directly, matching
  `rocketnet_used_token_history_claim_record`; no intermediate free-list
  push is needed and no slot leaks. Claim deadlines are held at
  `MaxClock + 1`, which changes no live-or-expired answer and keeps their
  order. The integrated control takes `ClaimTokens = {"th"}` so the full
  protocol state space remains within the runner's 110-second budget.
  `MCRNSecurityTablesClaims.cfg` runs `ClaimsSpec` with both `th` and `tf`
  and both slots, holding unrelated protocol variables fixed. It checks
  record, renewal, release, wrong-address/repeated release and expiry
  against all four claim invariants. The capacity invariant queries all
  three tokens, including an identifier absent from a full table. Both
  release witnesses retain the two-token integrated instance. The original
  two-token full control timed out; no protocol constant or invariant was
  weakened to obtain a passing run. A release unlinks the entry and
  pushes its slot on the free list. `ReleaseToFree = FALSE` is the code
  before the free list, without its scan: the release zeroed the deadline
  and left the slot in place. `ReleaseChecksLive = FALSE` pushes the slot
  without unlinking it, so a second release finds it again.
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
- **`FreeAndOrderPartitionSlots`**: every claim slot is on exactly one of
  the free list or the order list, and neither list holds a slot twice.
- **`OrderIsDeadlineOrder`**: the order list's deadlines never decrease
  from head to tail, so the head holds the earliest deadline. It rests on
  the clock never going backwards (the transport clamps a step backwards
  to the last good time) and one fixed handshake deadline.
- **`LiveClaimNeverReplaced`**: a claim record never lands on a slot
  holding another token's live claim (ghost `claimReplaced`).
- **`CanRecordMatchesScan`**: for every token, the O(1) capacity answer
  equals the answer of the scan over every slot that the code ran before.

## Switches: the code and the fixed code

| Switch | Code position | Fixed position |
|---|---|---|
| `RefuseAdmittedAtResponse` | no effect since 2026-10-18 unless `HoldUntilExpire` (the code has no admitted set); `TRUE` from 2026-10-17 refused an admitted token at the response | `FALSE`: the add succeeds when present, the code before 2026-10-17 |
| `HoldUntilExpire` | `FALSE` since 2026-10-18: no admitted-token set | reversed witness `TRUE`: the set held each token until expiry, denying churn |
| `CheckCidAtResponse` | `TRUE`: the response refuses a connected client id | reversed witness `FALSE`: two slots hold one token |
| `BindAtRequest` | `FALSE` since 2026-10-17: the history is recorded when a response opens | reversed witness `TRUE`: recorded at the request, the code before |
| `AdmitExpiredAtResponse` | `TRUE`: the response rung has no expiry check | `FALSE`: it refuses an expired token |
| `EvictLiveHistory` | `TRUE`: recording evicts the oldest, live or not | `FALSE`: it leaves a live entry in place |
| `DeniedConsumesToken` | `FALSE`: a denied adds nothing | mutant `TRUE` guards the ordering |
| `CapVerifyBudget` | `TRUE`: the per-update budget caps verifies | mutant `FALSE` guards the cap |
| `AcceptPrevious` | `TRUE`: previous generation accepted while `clock < prevUntil` | reversed witness `FALSE` drops the previous-key window |
| `PreviousOutlivesWindow` | `FALSE`: a stall past the window drops the previous generation | mutant `TRUE` keeps the previous generation across a stall |
| `ReleaseToFree` (definition) | `TRUE`: a released claim's slot goes back on the free list | reversed witness `FALSE`: the slot stays in place with a zeroed deadline |
| `ReleaseChecksLive` (definition) | `TRUE`: a release unlinks the entry, so a second release finds nothing | reversed witness `FALSE`: the slot is pushed without unlinking it |
| `RefuseUnremembered` (FloodSpec, a definition) | `FALSE` since 2026-10-20: only a source with a recorded failure is refused | reversed witness `TRUE`: a full table refused every source it could not remember |
| `BackwardsRollover` (FloodSpec, a definition) | `TRUE` since 2026-10-24: the window also rolls over when the clock steps back before its start | reversed witness `FALSE`: the one-sided rollover keeps a spent window open for the length of a backwards correction |

## The failed-signature budget under a flood (FloodSpec)

`FloodSpec` models `rocketnet_signature_verify_budget_allow`,
`rocketnet_signature_verify_budget_record_failure` and the window decay
they both run first (`sdk/rocketnet_server_protocol.cpp`): two forgers
against one table slot and a budget of two spend the budget and fill
the table while one honest client connects. The flood clock `fclock`
moves forward (`FloodClockForward`) and is stepped back to any earlier
time (`FloodClockBack`: an NTP step, a VM resume; the timestamp is the
unix wall clock whenever no game clock epoch is configured). The window
(`FailWindow = 2` ticks over `0..3`) rolls over, clearing the count and
the table, when the clock has passed it or has stepped back before its
start. `FloodWindowBounded`: whenever a source would be refused, the
clock is at or after the window's start and less than one window past
it, so a recorded failure refuses its source for at most the window
length of clock movement in either direction. Under the one-sided
rollover (`BackwardsRollover = FALSE`, the code before 2026-10-24) a
forger spends the budget in a window begun at clock 2 (6 states), the clock steps
back to 0, and the source is still refused: the window would stay open
for the correction plus its length. `test_signature_budget_window_restarts_on_backwards_clock`
pins the code: the budget spent at timestamp 1000 refuses the source,
and the same source is allowed at 900.

## Findings

Each finding is verified by hand against the code. The model lists the
token-table findings; cookie key rotation (F2) is implemented and
guarded by its two reversed witnesses.

| Invariant / Property | Trace | Code | Hand verification |
|---|---|---|---|
| `ChurnNeverDenied` | 9 states (`HoldUntilExpire = TRUE`) | the admitted-token set at the base before 2026-10-18: `rocketnet_admitted_token_set_create( context, max_clients * 2 )` and `rocketnet_admitted_token_set_add` refusing when full of unexpired entries, then `CONNECTION_RESPONSE_ADMITTED_SET_FULL` in `rocketnet_server_process_connection_response` | `A` connects `th` and disconnects, `B` connects `te`, then `A` requests the fresh `tf`: a slot is free but the set holds two unexpired entries at capacity two, so `A`'s response is denied. The code at the base does the same with one slot and a fresh address per cycle: `test_server_admits_sequential_fresh_tokens_past_twice_max_clients` printed `cycle 3 of 5 not admitted: client state -1, CONNECTION_RESPONSE_ADMITTED_SET_FULL 1`. Fixed by removing the set. |
| `OneTokenOneLiveConnection` | 5 states (`CheckCidAtResponse = FALSE`, with `RefuseAdmittedAtResponse = FALSE`) | the client-id check of `rocketnet_server_process_connection_response` | `A` and `B` each request the shared token `th` before either response binds it; `A`'s response admits; with the client-id check off, `B`'s response is admitted while `A` is connected, so two slots hold `th`. The code refuses it as `CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED`, pinned by `test_request_path_state_refusals` (one token, two addresses); `test_server_refuses_a_token_connected_now` pins the same token after a history flush at the request; `test_one_connection_per_token_after_history_flush` pins one client id, one live connection. |
| `OneConnectionPerToken` (retired 2026-10-18) | 6 states (`RefuseAdmittedAtResponse = FALSE`; 11 states before the binding moved) | `sdk/rocketnet_token_tables.cpp`, `rocketnet_admitted_token_set_add` (find returns success when present); response rung in `sdk/rocketnet_server_protocol.cpp` (`rocketnet_server_process_connection_response`) | `A` and `B` each request the shared token `th` (nothing binds at a request now, where before a history flush had to evict the first binding); `A`'s response admits, `A` disconnects, `B`'s response finds the client id free and the add succeeds because the entry is present, so the token admits a second connection. Closed in the code on 2026-10-17 by the admitted-set refusal on the response rung, pinned by `test_request_path_state_refusals`. |
| `BoundOnlyToOpener` | 2 states (`BindAtRequest = TRUE`) | `rocketnet_used_token_history_record` called from `rocketnet_server_process_opened_request` at the base before 2026-10-17 | `A`'s request for `th` writes the history entry before any response has opened: the request path recorded the source address after the admitted-set check and before the challenge. The code now records in `rocketnet_server_process_connection_response`, after the open. |
| `NoBindToNonHolder` | 2 states (`BindAtRequest = TRUE`, `BindAtRequestAttacker.cfg`) | the same record at the request; the history check at the top of the request path | `B`, which does not hold `tf`, replays `tf` and the request binds `tf` to `B`; `A`'s own request for `tf` is then refused as `TOKEN_ALREADY_USED` for the token's lifetime. `test_token_bound_at_response_open` reproduced exactly this at the base (its first check, `TOKEN_ALREADY_USED == 0`, failed) and passes with the binding at the response. |
| `ExpiredNeverAdmitted` | 5 states | request rung expiry check in `rocketnet_server_protocol_process_connection_request`; no expiry check in `rocketnet_server_process_connection_response` | A token presented before expiry whose response arrives after expiry is admitted: the request path refuses an expired token at its top, the response path checks only the client id and the slots. Bounded by the handshake deadline. |
| `NoLiveHistoryEviction` | 8 states (6 before the binding moved) | `rocketnet_used_token_history_record` and `rocketnet_token_table_select_slot(..., 1)` in `sdk/rocketnet_token_tables.cpp` | At capacity with no expired entry, recording a new token unlinks and reuses the oldest slot even while its entry is unexpired. This is the documented flush bound; it is what lets the first finding's second binding pass the history check. |
| `DeniedTokenStaysUsable` | 5 states (mutant) | slots check before the admitted add in `rocketnet_server_process_connection_response`; request-rung denied creates no mapping | The code holds this invariant: a denied for a full server consumes nothing, at both rungs (with `HoldUntilExpire`, it was sent before the set took its entry). The reversed witness is a mutant that consumes the token on a denied; TLC rejects it, which guards the ordering against a future edit. |
| `TypeOK` verify bound | 7 states (mutant) | handshake budget check in the request path (`ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE` in `sdk/rocketnet_counters.h`); 64-slot failed-verify table in `sdk/rocketnet_server.h` | The code holds this invariant: past the cookie gate, verifies per update are counted and capped before asymmetric work runs. The reversed witness removes the cap; TLC rejects `SafetyInvariants` through the `TypeOK` bound when `verifies = 4` with `VerifyBudget = 2`. `VerifyBudgetRespected` itself is conditional on `CapVerifyBudget`, so it does not reject the disabled-cap mutant. |
| `InFlightCookieSurvivesRotation` | 3 states (`AcceptPrevious = FALSE`) | `sdk/rocketnet_server_protocol.cpp` (`rocketnet_server_protocol_rotate_cookie_key_if_due` and the previous-key branch of the cookie gate at `:377`) | An on-time rotation moves the outgoing key to `previous_cookie_key` with `previous_cookie_key_expire_time = cookie_key_rotation_time + period`, so a cookie minted before the rotation is accepted across it while `current_time < previous_cookie_key_expire_time`. Turning `AcceptPrevious` off drops the outgoing generation at the rotation tick (`clock = 1 -> 2`) and fails `InFlightCookieSurvivesRotation`. |
| `PreviousBoundedToOnePeriod` | 2 states (mutant `PreviousOutlivesWindow = TRUE`) | `sdk/rocketnet_server_protocol.cpp` (`rocketnet_server_protocol_rotate_cookie_key_if_due`, stall branch when `current_time >= cookie_key_rotation_time + period`, and the gate check `current_time < server->previous_cookie_key_expire_time`) | When `update` stalls past an entire rotation window (`clock = 0 -> 4`), the outgoing key's acceptance window `[2, 4)` has already elapsed, so the code zeroes `previous_cookie_key` and clears `has_previous_cookie_key`. The mutant `PreviousOutlivesWindow = TRUE` keeps the outgoing key until `current_time + period` (`prevUntil = 6 > 4`), which TLC rejects on the stall step. |

## How to run it

On a Linux bench holding `/opt/tla/tla2tools.jar`, from `tla/`:

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
| `BrokenGlobalLockout.cfg` | `RefuseUnremembered <- MCRefuseUnremembered` (`TRUE`), FloodSpec | `HonestConnectsUnderFlood` |
| `BrokenBackwardsWindow.cfg` | `BackwardsRollover <- MCNoBackwardsRollover` (`FALSE`), FloodSpec | `FloodWindowBounded` |
| `BrokenReleaseKeepsSlot.cfg` | `ReleaseToFree <- MCReleaseToFree` (`FALSE`) | `CanRecordMatchesScan` |
| `BrokenReleaseTwice.cfg` | `ReleaseChecksLive <- MCReleaseChecksLive` (`FALSE`) | `FreeAndOrderPartitionSlots` |

`Flood.cfg` is FloodSpec's passing control: `FloodInvariants` (with
`FloodWindowBounded`) and `HonestConnectsUnderFlood` hold.

Refresh all sixteen cases through the pinned `tlacheck` runner and merge with
`--keep tla/RUNS.tsv` as `README.md` describes. The measured ledger contains
one row per configuration; earlier seven-column records remain in
`RUNS-HISTORY.tsv`.

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

The fixed-code run generates 4,031,603 states and finds 903,855
distinct states (2,609,659 and 606,181 while the admitted set held each
token to expiry: a token may now connect again after its connection
closes), with no error: the invariants hold and
`DeniedTokenStaysUsable`, `RotationWindow`,
`PreviousBoundedToOnePeriod` and `InFlightCookieSurvivesRotation` hold
as properties.

## What the model leaves out

- The passing run holds `EvictLiveHistory = FALSE`, so TLC does not check
  `OneTokenOneLiveConnection` under the code's live-entry eviction; the code
  holds it there by the client-id check, which the model applies at the
  response only (the code also applies it at the request after the open).
  `test_server_refuses_a_token_connected_now` pins the evicted case in the code.
- `DeniedTokenStaysUsable` constrains only the admitted set, so it is
  vacuous with `HoldUntilExpire = FALSE`; its witness tests the old code.

- `Tick` advances the clock and rotates atomically. It does not represent
  the transport receive-before-sweep ordering. Unit regressions exercise
  cookies issued before a delayed sweep, both near a rotation deadline and
  after a full-window stall, plus refusal of the outgoing key after a stall.

- A half-open mapping removed before its handshake deadline (the idle
  timeout or the integrity limit, `rocketnet_server_release_mapping_claim`)
  is not an action: it would release a live claim from anywhere in the
  order list, which `RequestRefused` already does, and its mapping states
  put the instance past the checking budget. `test_claim_released_with_half_open_idle_mapping`
  and `test_claim_released_with_integrity_limit_mapping` pin it in the code.
- Packets, the link, the replay window and the session keys stay in
  the held draft; a response here is a packet that has already opened
  under its mapping.
- The cookie is its generation only: the MAC is the attacker's
  inability to make one for a generation it has not seen, and a seen
  cookie for another address is refused by the address binding inside
  the history check's address, not modelled separately.
- Nonces and the ephemeral exchange are outside the tables; a mapping
  here is the token it was created for, its cookie generation and its
  birth tick, swept at the handshake deadline.

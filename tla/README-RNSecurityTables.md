# RNSecurityTables

The security tables at the table grain: the used-token history and the
admitted-token set (`sdk/rocketnet_token_tables.cpp`), the cookie gate
and its key generation, and the per-update verify budget, as
`sdk/rocketnet_server_protocol.cpp` drives them and STANDARD.md
specifies them. This module lands the held draft of `tla/Security.tla`
(branch `sprint/rn-tla-security.w3.g1.e15`, commit `2f8de9d`) at the
grain of inventory modules 5 and 6 in `docs/TLA-INVENTORY.md`: the full
handshake, the lossy link and the replay window stay in that draft;
here a request past the cookie gate, a response that opens under its
mapping, another client taking a slot, a disconnect and a tick are the
only events. The attacker is the quantification itself: any address
presents any token under any cookie generation it has seen.

- **The history.** Per token: absent, or bound to an address with an
  expiry and an insert order. A request bound to another address while
  the entry lives is refused. Recording is a no-op for a live entry,
  then a free slot, then an expired slot, then — in the code — the
  oldest entry, live or not (`rocketnet_token_table_select_slot` with
  `evict_oldest = 1`, reached from `rocketnet_used_token_history_record`).
- **The admitted set.** Per token: absent, or admitted with an expiry.
  The response rung checks the client id, then the slots, then adds;
  the add succeeds when the token is already present, and at capacity
  with nothing expired it refuses, never evicting a live entry
  (`rocketnet_admitted_token_set_add`, `select_slot(..., 0)`).
- **The cookie gate and rotation.** A mapping is created only under a
  cookie of a live generation: the current key, or the previous one
  while it is held. The key rotates on its period and the previous key
  is kept for one window. In the code the key is drawn once at create
  and never rotated (F2 in the inventory); the previous-key branch in
  the gate is reachable only from a test. The model states the window
  the rotation code is written against.
- **The verify budget.** Requests past the gate spend from a per-update
  budget (`verifies`, reset by the tick): the handshake work budget
  (`ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE`, checked in the request
  path before any asymmetric work) and the failed-verify budget with
  its 64 table slots (`ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS`).

## Invariants

- **`OneConnectionPerToken`**: a token admits at most one connection
  while unexpired (the ghost count of admissions never passes one).
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
- **`VerifyBudgetRespected`**: verifies in one update never pass the
  budget, while the cap switch is in its code position.
- **`TablesBounded`**: neither table passes its capacity, and slots
  plus other clients never pass `MaxClients`.

## Switches: the code and the fixed code

| Switch | Code position | Fixed position |
|---|---|---|
| `RefuseAdmittedAtResponse` | `FALSE`: the add succeeds when present | `TRUE`: the response rung refuses an admitted token |
| `AdmitExpiredAtResponse` | `TRUE`: the response rung has no expiry check | `FALSE`: it refuses an expired token |
| `EvictLiveHistory` | `TRUE`: recording evicts the oldest, live or not | `FALSE`: it leaves a live entry in place |
| `DeniedConsumesToken` | `FALSE`: a denied adds nothing | mutant `TRUE` guards the ordering |
| `CapVerifyBudget` | `TRUE`: the per-update budget caps verifies | mutant `FALSE` guards the cap |

## Findings

Each finding is verified by hand against the code at the base of this
change (`abd8bdc`). The model lists them; it does not fix them.

| Invariant | Trace | Code | Hand verification |
|---|---|---|---|
| `OneConnectionPerToken` | 13 states | `sdk/rocketnet_token_tables.cpp`, `rocketnet_admitted_token_set_add` (find returns success when present); response rung in `sdk/rocketnet_server_protocol.cpp` (`rocketnet_server_process_connection_response`) | Two mappings for one shared token exist after the history flush evicts the first binding; the first response admits, the slot is released, the second response finds the client id free and the add succeeds because the entry is present, so the token admits a second connection. |
| `ExpiredNeverAdmitted` | 7 states | request rung expiry check in `rocketnet_server_protocol_process_connection_request`; no expiry check in `rocketnet_server_process_connection_response` | A token presented before expiry whose response arrives after expiry is admitted: the request path refuses an expired token at its top, the response path checks only the client id, the slots and the admitted set. Bounded by the handshake deadline. |
| `NoLiveHistoryEviction` | 6 states | `rocketnet_used_token_history_record` and `rocketnet_token_table_select_slot(..., 1)` in `sdk/rocketnet_token_tables.cpp` | At capacity with no expired entry, recording a new token unlinks and reuses the oldest slot even while its entry is unexpired. This is the documented flush bound; it is what lets the first finding's second binding pass the history check. |
| `DeniedTokenStaysUsable` | 5 states (mutant) | slots check before the admitted add in `rocketnet_server_process_connection_response`; request-rung denied creates no mapping | The code holds this invariant: a denied for a full server is sent before the set takes its entry, at both rungs. The reversed witness is a mutant that consumes the token on a denied; TLC rejects it, which guards the ordering against a future edit. |
| `VerifyBudgetRespected` | 7 states (mutant) | handshake budget check in the request path (`ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE` in `sdk/rocketnet_counters.h`); 64-slot failed-verify table in `sdk/rocketnet_server.h` | The code holds this invariant: past the cookie gate, verifies per update are counted and capped before asymmetric work runs. The reversed witness removes the cap; TLC rejects it. |

Cookie rotation (F2) is a specification finding, not a TLC witness:
the gate's previous-key branch exists, the rotation that would make it
reachable does not. `RotationWindow` passes in the fixed run with
`Rotation = TRUE`; turning `AcceptPrevious` off removes the window and
is the reversed witness the rotation code is written against.

## How to run it

On a Linux bench holding `/opt/tla/tla2tools.jar`, from `tla/`:

```sh
java -XX:+UseParallelGC -cp /opt/tla/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNSecurityTables.cfg MCRNSecurityTables
```

Each reversed witness is the same command with one switch edited in a
copy of the config, as `tla/RUNS.tsv` records per row.

The small instance:

| Constant | Value |
|---|---|
| `Addrs` | `{"A", "B"}` |
| `Tokens` | `{"th", "te", "tf"}` (`te` expires at tick 3) |
| `MaxClock` | 4 |
| `HistoryCap` / `AdmittedCap` | 2 / 2 |
| `MaxClients` | 2 |
| `VerifyBudget` | 2 per update |
| `HandshakeDeadline` | 4 ticks |

The fixed-code run generates 1,025,308 states and finds 249,679
distinct states, with no error: the invariants hold and
`DeniedTokenStaysUsable` and `RotationWindow` hold as properties.

## What the model leaves out

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

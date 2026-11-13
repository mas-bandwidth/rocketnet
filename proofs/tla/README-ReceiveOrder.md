# ReceiveOrder: cheap refusals first

`ReceiveOrder.tla` models a receive path as ordered rungs, each cheap or
crypto. `RequestRungOrder` pins the request rungs to the code's order, and
`CheapRefusalsFirst` says no packet refused at a cheap rung has passed a crypto
rung. Keyed hashes (the cookie MAC, the token
identifier, key derivation) are the cheap tier; keypair generation, scalar
multiplication, AEAD seal or open, signature verification and sealed-box open
are crypto.

The request path runs in the ruled order. `Pos` puts the rungs in the code's
order: used, verifybudget, claim, capacity, claimfull, repeat (t12 rule 9
steps 1 to 6), then the per-update handshake work budget immediately before the
signature (t12 rule 8). Past the seal, clientid then slots refuse (t12 rules 10
and 11); each records its token for the repeat rung until the update ends
(`repeated`, emptied by MCReceiveOrder's `UpdateStart`), and
`RepeatSpendsNoVerify` says a request whose token was so recorded never
reaches the signature. `WorkBudgetOnlyForVerify` says a request refused at any rung above
the signature has not been charged that budget. Each rung maps to its code and
STANDARD step in _Processing Connection Requests_:

Every rung that refuses in the code refuses in the model, either on a fault
the request chose (`Faults`) or on the state it reads (a bound token, a claim
held elsewhere, a full table, a token already refused this update, a request
for the token its address's half-open mapping holds). `AcceptStep` runs only
once the last rung passes.

| Rung | Kind | Code | STANDARD step | Refuses on |
|---|---|---|---|---|
| restart | cheap | the size check, then the address lookup in rocketnet_server_protocol_process_connection_request_known: a connected address is ignored, and a request carrying its half-open mapping's salt or token gets the stored challenge | t12 rule 3: already connected; resend stored challenge | fault `restart` (wrong size, connected address); `maps[reqAddr] = reqToken` |
| version | cheap | sdk/rocketnet_server_protocol.cpp version, then protocol id compare | TRANSPORT.md t12 rule 4: version, then protocol id | fault `version` |
| timestamps | cheap | expire and creation skew compares, then the timeout compare | t12 rule 5: expiry, creation skew; t12 rule 6: timeout seconds | fault `expired` |
| compares | cheap | address flags, signed zero pad, advertised address, server key compares, in that order | t12 rule 6: address flags; advertised address; server public key | fault `compares` |
| cookie | cheap | cookie MAC (a missing, forged or expired cookie is answered with a fresh cookie, or withheld by the reply limit) | recompute the cookie MAC | fault `cookie` |
| used | cheap | used-token history lookup: rocketnet_used_token_history_check against the request's address, then, for a request that would replace a half-open mapping, against the zero address | token identifier in used-token history (t12 rule 9 step 1) | `UsedRefuses`: a token bound to another address, or any bound token on a replacing request |
| verifybudget | cheap | rocketnet_signature_verify_budget_allow in sdk/rocketnet_server_protocol.cpp | the failed-signature budget refuses a source with a recorded failure in the spent window (t12 rule 9 step 2) | fault `budget` |
| claim | cheap | ClaimCheck: rocketnet_used_token_history_claim_check in sdk/rocketnet_token_tables.cpp; ClaimWrite: rocketnet_used_token_history_claim_record, run only after the signature verifies | the first request whose signature verifies claims its token for its source address until the handshake deadline; a second address is refused (t12 rule 9 step 3) | fault `claimed`; `ClaimOther` |
| capacity | cheap | rocketnet_encryption_map_full in sdk/rocketnet_server_protocol.cpp | a full encryption mapping table refuses before any signature, seal or keypair work (t12 rule 9 step 4) | fault `mapfull`; a full table for a request that replaces nothing |
| claimfull | cheap | the read-only rocketnet_used_token_history_claim_can_record in sdk/rocketnet_token_tables.cpp, run after the mapping-capacity rung | a request-claim table with no free or expired slot refuses with its own counter, CONNECTION_REQUEST_CLAIM_TABLE_FULL, and not as a mapping failure (t12 rule 9 step 5) | fault `claimfull` |
| repeat | cheap | the repeat table scan in rocketnet_server_protocol_process_connection_request (sdk/rocketnet_server_protocol.cpp), written by rocketnet_server_record_repeat and emptied in rocketnet_server_protocol_update | a token verified earlier in the update and refused by the clientid or slots rung is refused again, unanswered, counted by that reason (t12 rule 9 step 6) | `reqToken \in repeated` |
| workbudget | cheap | the handshakes_processed_this_update check and charge in sdk/rocketnet_server_protocol.cpp, immediately before the verify | refuse, unanswered, when the per-update handshake work budget is spent; only a request that reaches it spends budget (t12 rule 8) | fault `workspent` |
| signature | crypto | crypto_sign_verify_detached in sdk/rocketnet_connect_token.cpp | verify the signature | fault `signature` |
| seal | crypto | crypto_box_seal_open in sdk/rocketnet_connect_token.cpp | open the sealed private block | fault `seal` |
| consistent | cheap | rocketnet_connect_token_private_consistent in rocketnet_server_process_opened_request, charged to the source | ignore a request whose sealed fields disagree with its signed fields (t12 rule 10) | fault `mismatch` |
| clientid | cheap | rocketnet_client_slots_find in rocketnet_server_process_opened_request | ignore a request whose client id already holds a slot (t12 rule 10) | fault `clientid` |
| slots | cheap | rocketnet_client_slots_num_free in sdk/rocketnet_server_protocol.cpp | no client slots available | fault `full` |
| keypair | crypto | crypto_box_keypair in sdk/rocketnet_server_protocol.cpp | obtain the server ephemeral keypair | nothing (a failed keypair is an internal error, not modelled) |
| derive | crypto | the scalar multiplication in rocketnet_derive_connection_keys, charged to the source | ignore a small-order client ephemeral key (t17 item 12) | fault `smallorder` |

Not modelled: the internal-error refusals past the keypair (a failed
`crypto_box_keypair`, a mapping add that fails after the capacity rung passed,
a challenge or denial writer that returns the wrong size), each asserted or an
operating-system failure; none of them leaves a mapping or a claim behind.

## Witnesses

Each witness is the same module under a Broken cfg, moving one cheap rung
behind a crypto rung or removing the token-to-address claim. TLC must reject
them with the invariant each names:

| Witness | Order | Code replay |
|---|---|---|
| SlotsAfterKeypair | slots at 11, keypair at 10 | BASE: crypto_box_keypair and the scalar multiplication inside rocketnet_derive_connection_keys run before rocketnet_client_slots_num_free, so a full server pays for a keypair on every denial |
| VersionNotFirst | only version moves from 2 to 12; restart remains 1, signature 8, slots 10 and keypair 11 | a version refusal decided only after signature work; restoring version to 2 restores the healthy order, without a slots-after-keypair defect |
| TokenManyAddresses | claim removed (ClaimAtRequest FALSE) | BASE: the token was not claimed at the request, so one token from many addresses bought many half-open mappings and full crypto until the map filled; OneLiveMappingPerToken fails |
| WorkBudgetAtCookie | workbudget at 51, just after the cookie gate (WorkBudgetAtVerify FALSE) | BASE before 2026-10-31: the budget was charged after the cookie validated, so a request the used, failed-signature-budget, claim or capacity rungs then refused had spent budget; one source past its failed-signature budget, refused at no cost to itself, kept the default 16-per-update budget full at about 1000 requests per second and honest requests were refused as HANDSHAKE_BUDGET_EXCEEDED. WorkBudgetOnlyForVerify fails |
| RepeatRungOff | the repeat rung refuses nothing (RepeatRungRefuses FALSE); repeated still records the tokens the clientid and slots rungs refuse | BASE before 2026-11-13: a request refused past the verify because its client id held a slot, or denied because the server was full, charged nothing and recorded nothing, so the same request from the same source was verified again at once; one valid token took every verify of every update (default 16, about 1.3 ms of main thread) and honest requests were refused as HANDSHAKE_BUDGET_EXCEEDED. RepeatSpendsNoVerify fails |
| ClaimBeforeSignature | claim written at its check rung (ClaimWriteRung "claim") | BASE before the split: rocketnet_used_token_history_claim_record ran at the claim rung, ahead of rocketnet_verify_signed_connect_data, and a failed signature released it after; every unauthenticated request past the cookie wrote (and then released) a claim-table entry. ClaimOnlyAfterSignature fails |

`VersionNotFirst` retains the healthy positions of every other rung and
uses the unused position 12, so all rung positions remain unique. The earlier
configuration also put slots after keypair and could fail independently of
version ordering; it is preserved in Git history, not used as the isolated
version witness. Its declared oracle remains `CheapRefusalsFirst`.

Replayed by hand: in ClaimBeforeSignature a request with fault `signature`
advances past the claim rung (position 62) with no other claim on its token,
so `claimWritten` holds while `verified` does not, and the signature rung
then refuses it; this is the BASE code writing the claim at the rung and
releasing it on the failed verify. With the split the write happens on
passing the signature rung, the same request is refused with
`claimWritten` FALSE, and test_claim_written_only_after_signature_verifies
pins it in the code (no claim-table entry after a forged signature).

Replayed by hand: in RepeatRungOff (30 states) a request from `A` for `t1`
with fault `clientid` passes every rung through the signature (position 80)
and the seal and is refused at the clientid rung (111), which records `t1` in
`repeated`; with no `UpdateStart` between, the next request from `A` for `t1`
begins with `wasRepeat`, passes the repeat rung at 78 (it refuses nothing)
and the work budget at 79, and reaches the signature, so
`RepeatSpendsNoVerify` fails. That is the BASE code: after a
CLIENT_ID_ALREADY_CONNECTED refusal nothing is recorded and the same request
is verified again in the same update.
test_handshake_client_id_connected_never_limits and
test_handshake_full_server_denial_never_limits pin it in the code: the first
request of an update is verified, its repeats are refused before the work
budget (each fails at its first repeat with the rung's loop taken out).

Replayed by hand: in WorkBudgetAtCookie a request with fault `budget` (a
source past its failed-signature budget) passes the workbudget rung at 51 and
is refused at the verifybudget rung at 61, so `WorkBudgetOnlyForVerify` fails;
this is the BASE code charging `handshakes_processed_this_update` before
`rocketnet_signature_verify_budget_allow` refuses.
test_handshake_refused_source_spends_no_work_budget pins it in the code: a
flood from such a source leaves the budget unspent and an honest request in
the same update is challenged.

Replayed by hand: in SlotsAfterKeypair a full-server packet (fault `full`)
passes the keypair rung, so `spent` holds when the slots rung refuses, and
`CheapRefusalsFirst` fails exactly as the tally test fails at BASE with
keypair 1. The fix moves the slots rung ahead of the keypair, and the same
packet is refused with the tally at zero.

## Response binding

`MCReceiveOrder.ResponseBind` models an opened response for an unexpired
mapped token: it records the address binding and releases that address's
request claim. `BoundHasNoOwnClaim` requires a bound token to keep no claim of
the address it is bound to once a request is done; the stricter
`BoundHasNoClaim` fails across the deadline window (below). Requests may
carry bound tokens, which the used rung refuses as the code does. Left out:
the reconnect (a free address presenting a token bound to itself after its
connection closed), which the code serves by writing a new claim beside the
binding, so `BoundHasNoOwnClaim` is stated for this instance without it;
token expiry and binding eviction are outside this instance too. Existing
mapping-removal and request-order witnesses remain.

`MCReceiveOrderBrokenClaimOutlivesBinding.cfg` overrides
`ResponseBindReleasesClaim` with FALSE. Its accepted request leaves a claim;
the following response writes the binding but retains that claim, violating
`BoundHasNoOwnClaim`. This is the missing claim release in
`rocketnet_server_process_connection_response`, pinned by
`test_claim_released_when_response_binds_token`: a second address passes the
claim check after the response but remains refused by the token binding.

## Half-open replacement

`MCReceiveOrder.NewRequest` starts a check sequence run for a free address, or for an
address whose half-open mapping holds another token (a connected mapping is
never replaced). The half-open mapping's own token may be bound: a response
that bound it and was then denied, for want of a slot or because its client
id is already connected, leaves the mapping half-open, and the code replaces
it like any other. Such a request passes the capacity
rung with a full table, since it reuses its address's mapping, and only
`AcceptStep`, past every rung including the signature, replaces the mapping:
it installs the new token and releases the replaced token's claim
(`ReplaceReleasesClaim`). This is the code's `replace_index` path in
`rocketnet_server_protocol_process_connection_request`: a request with another
salt and another token is not answered with the stored challenge, and the
replacement runs just before `rocketnet_encryption_map_add`, after
`rocketnet_server_release_mapping_claim` and
`rocketnet_encryption_map_remove_current` on the replaced mapping. A
replacing request whose own token is bound to any address, its own
included, is refused at the used rung (the second
`rocketnet_used_token_history_check`, against the zero address):
`ReplaceOnlyUnboundToken` says an accepted request that found a half-open
mapping at its address carried an unbound token.

`RefusalKeepsHalfOpen` holds the address's half-open mapping fixed across a
refused request, through the ghost `held` (the mapping the address held as
the request began). Three reversed witnesses:

| Witness | Change | Code replay |
|---|---|---|
| ReplaceIgnoresBinding | `ReplaceNeedsUnboundToken <- FALSE` | the used rung without its zero-address check (the rule ServerConnection's replace branch stated until 2026-11-13): A's response binds t1 and is denied, so A's mapping stays half-open; A replaces it with t2; A's next request, for t1, is checked only against A's own binding, passes, and replaces A's mapping for t2. ReplaceOnlyUnboundToken fails (61 states in the trace; replayed by hand) |
| ReplaceKeepsClaim | `ReplaceReleasesClaim <- FALSE` | the replacement without `rocketnet_server_release_mapping_claim`: address A replaces its mapping for t1 with t2, and t1's claim stays with no mapping holding t1. ClaimHasMapping fails |
| ReplaceBeforeVerify | `ReplaceAtRequest <- TRUE` | the replacement at the top of the request path, before any check: a request for t2 with a wrong version evicts A's mapping for t1 and is then refused, leaving A with no mapping. RefusalKeepsHalfOpen fails |

All three counterexamples were replayed against the code by hand as above.
test_new_token_replaces_stale_half_open_mapping pins the code: a forged
request and a bound token leave the stale mapping in place, and a new token
under a new salt gets a cookie, then a fresh challenge from the replacing
mapping, with the stale token's claim released.

## Handshake deadline window

A claim and its mapping carry the same deadline, `current_time +
handshake_deadline_seconds`, but they leave at different times. The claim
expires the moment the clock reaches it: `rocketnet_used_token_history_claim_check`
and `rocketnet_used_token_history_claim_record` treat `claim_deadline <=
current_time` as free. The mapping leaves only at the sweep in
`rocketnet_server_protocol_update`, which the server transport runs after the
receive batch of the same update. `MCReceiveOrder` models the two steps:
`ClaimExpire` drops the claim and marks its address's mapping `stale`, and
`Sweep` removes every stale mapping, releasing a claim only when the swept
address holds it (`rocketnet_server_release_mapping_claim`). Requests and
responses may run between them, as the rest of that receive batch does.

In that window `OneLiveMappingPerToken` does not hold, so the committed run
checks `OneUnexpiredMappingPerToken`: at most one mapping per token that is
not yet past its deadline. Two reversed witnesses keep the strict forms:

| Witness | Strict invariant | Code replay |
|---|---|---|
| DeadlineWindow | `OneLiveMappingPerToken` | A's mapping and claim for t1 reach their deadline T. In the receive batch at T, before the sweep, B's request for t1 passes the used rung (t1 unbound), passes the claim rung (A's claim expired), claims t1 and is accepted; A's and B's mappings both hold t1 until the sweep removes A's, whose claim release leaves B's claim |
| DeadlineWindowBound | `BoundHasNoClaim` | as above, then A's connection response opens on its stale mapping in the same batch (the response path does not check the handshake deadline): `rocketnet_used_token_history_record` binds t1 to A and `rocketnet_used_token_history_claim_release(t1, A)` leaves B's claim, which lasts until B's response releases it or B's deadline |

Both counterexamples were replayed against the code by hand as above. The
window gives a token one more half-open mapping per handshake deadline, not
more: the second address's claim refuses any third until its own deadline.

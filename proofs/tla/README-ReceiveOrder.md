# ReceiveOrder: cheap refusals first

`ReceiveOrder.tla` models a receive path as ordered rungs, each cheap or
crypto. `RequestRungOrder` pins the request rungs to the code's order, and
`CheapRefusalsFirst` says no packet refused at a cheap rung has passed a crypto
rung. Keyed hashes (the cookie MAC, the token
identifier, key derivation) are the cheap tier; keypair generation, scalar
multiplication, AEAD seal or open, signature verification and sealed-box open
are crypto.

The request path runs in the ruled order. `Pos` puts the rungs in the code's
order: used, verifybudget, claim, capacity, claimfull (t12 rule 9 steps 1 to
5), then the per-update handshake work budget immediately before the signature
(t12 rule 8). `WorkBudgetOnlyForVerify` says a request refused at any rung above
the signature has not been charged that budget. Each rung maps to its code and
STANDARD step in _Processing Connection Requests_:

| Rung | Kind | Code | STANDARD step |
|---|---|---|---|
| restart | cheap | already-connected and mapping lookups; the address lookup runs right after the size check (t12 rule 3), and the client-id check after the seal open | t12 rule 3: already connected; resend stored challenge |
| version | cheap | sdk/rocketnet_server_protocol.cpp version, then protocol id compare | TRANSPORT.md t12 rule 4: version, then protocol id |
| timestamps | cheap | expire and creation skew compares, then the timeout compare | t12 rule 5: expiry, creation skew; t12 rule 6: timeout seconds |
| compares | cheap | address flags, advertised address, server key compares, in that order | t12 rule 6: address flags; advertised address; server public key |
| cookie | cheap | cookie MAC | recompute the cookie MAC |
| used | cheap | used-token history lookup | token identifier in used-token history (t12 rule 9 step 1) |
| verifybudget | cheap | rocketnet_signature_verify_budget_allow in sdk/rocketnet_server_protocol.cpp | the failed-signature budget refuses a source with a recorded failure in the spent window (t12 rule 9 step 2) |
| claim | cheap | ClaimCheck: rocketnet_used_token_history_claim_check in sdk/rocketnet_token_tables.cpp; ClaimWrite: rocketnet_used_token_history_claim_record, run only after the signature verifies | the first request whose signature verifies claims its token for its source address until the handshake deadline; a second address is refused (t12 rule 9 step 3) |
| capacity | cheap | rocketnet_encryption_map_full in sdk/rocketnet_server_protocol.cpp | a full encryption mapping table refuses before any signature, seal or keypair work (t12 rule 9 step 4) |
| claimfull | cheap | the read-only rocketnet_used_token_history_claim_can_record in sdk/rocketnet_token_tables.cpp, run after the mapping-capacity rung | a request-claim table with no free or expired slot refuses with its own counter, CONNECTION_REQUEST_CLAIM_TABLE_FULL, and not as a mapping failure (t12 rule 9 step 5) |
| admitted | cheap | removed with the admitted-token set (`docs/SESSION.md` 1.6): the used-token history rung is now the cheap refusal of a token a connected client holds, from any other address; the model keeps the position | (none) |
| workbudget | cheap | the handshakes_processed_this_update check and charge in sdk/rocketnet_server_protocol.cpp, immediately before the verify | refuse, unanswered, when the per-update handshake work budget is spent; only a request that reaches it spends budget (t12 rule 8) |
| signature | crypto | crypto_sign_verify_detached in sdk/rocketnet_connect_token.cpp | verify the signature |
| seal | crypto | crypto_box_seal_open in sdk/rocketnet_connect_token.cpp | open the sealed private block |
| slots | cheap | rocketnet_client_slots_num_free in sdk/rocketnet_server_protocol.cpp | no client slots available |
| keypair | crypto | crypto_box_keypair in sdk/rocketnet_server_protocol.cpp | obtain the server ephemeral keypair |

## Witnesses

Each witness is the same module under a Broken cfg, moving one cheap rung
behind a crypto rung or removing the token-to-address claim. TLC must reject
them with the invariant each names:

| Witness | Order | Code replay |
|---|---|---|
| SlotsAfterKeypair | slots at 11, keypair at 10 | BASE: crypto_box_keypair and the scalar multiplication inside rocketnet_derive_connection_keys run before rocketnet_client_slots_num_free, so a full server pays for a keypair on every denial |
| AdmittedAfterVerify | admitted at 8, signature at 6 | BASE order with the admitted lookup behind the signature verify: a cheap refusal decided only after signature work |
| VersionNotFirst | version at 9, behind the signature at 8 | a version refusal decided only after signature work; the version byte is a fixed-offset compare and belongs first |
| TokenManyAddresses | claim removed (ClaimAtRequest FALSE) | BASE: the token was not claimed at the request, so one token from many addresses bought many half-open mappings and full crypto until the map filled; OneLiveMappingPerToken fails |
| WorkBudgetAtCookie | workbudget at 51, just after the cookie gate (WorkBudgetAtVerify FALSE) | BASE before 2026-10-31: the budget was charged after the cookie validated, so a request the used, failed-signature-budget, claim or capacity rungs then refused had spent budget; one source past its failed-signature budget, refused at no cost to itself, kept the default 16-per-update budget full at about 1000 requests per second and honest requests were refused as HANDSHAKE_BUDGET_EXCEEDED. WorkBudgetOnlyForVerify fails |
| ClaimBeforeSignature | claim written at its check rung (ClaimWriteRung "claim") | BASE before the split: rocketnet_used_token_history_claim_record ran at the claim rung, ahead of rocketnet_verify_signed_connect_data, and a failed signature released it after; every unauthenticated request past the cookie wrote (and then released) a claim-table entry. ClaimOnlyAfterSignature fails |

Replayed by hand: in ClaimBeforeSignature a request with fault `signature`
advances past the claim rung (position 62) with no other claim on its token,
so `claimWritten` holds while `verified` does not, and the signature rung
then refuses it; this is the BASE code writing the claim at the rung and
releasing it on the failed verify. With the split the write happens on
passing the signature rung, the same request is refused with
`claimWritten` FALSE, and test_claim_written_only_after_signature_verifies
pins it in the code (no claim-table entry after a forged signature).

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
request claim. `BoundHasNoClaim` requires every bound token to have no claim
once a request is done. The instance explores fresh unbound-token requests
between bindings; token expiry, binding eviction and reconnect are outside
this instance. Existing mapping-removal and request-order witnesses remain.

`MCReceiveOrderBrokenClaimOutlivesBinding.cfg` overrides
`ResponseBindReleasesClaim` with FALSE. Its accepted request leaves a claim;
the following response writes the binding but retains that claim, violating
`BoundHasNoClaim`. This is the missing claim release in
`rocketnet_server_process_connection_response`, pinned by
`test_claim_released_when_response_binds_token`: a second address passes the
claim check after the response but remains refused by the token binding.

## Half-open replacement

`MCReceiveOrder.NewRequest` starts a ladder run for a free address, or for an
address whose half-open mapping holds another token (a mapping whose token is
bound, a connected one, is never replaced). Such a request passes the capacity
rung with a full table, since it reuses its address's mapping, and only
`AcceptStep`, past every rung including the signature, replaces the mapping:
it installs the new token and releases the replaced token's claim
(`ReplaceReleasesClaim`). This is the code's `replace_index` path in
`rocketnet_server_protocol_process_connection_request`: a request with another
salt and another token is not answered with the stored challenge, and the
replacement runs just before `rocketnet_encryption_map_add`, after
`rocketnet_server_release_mapping_claim` and
`rocketnet_encryption_map_remove_current` on the replaced mapping. The
instance explores only unbound tokens; the code also refuses a replacing
request whose token is bound to any address.

`RefusalKeepsHalfOpen` holds the address's half-open mapping fixed across a
refused request, through the ghost `held` (the mapping the address held as
the request began). Two reversed witnesses:

| Witness | Change | Code replay |
|---|---|---|
| ReplaceKeepsClaim | `ReplaceReleasesClaim <- FALSE` | the replacement without `rocketnet_server_release_mapping_claim`: address A replaces its mapping for t1 with t2, and t1's claim stays with no mapping holding t1. ClaimHasMapping fails |
| ReplaceBeforeVerify | `ReplaceAtRequest <- TRUE` | the replacement at the top of the request path, before any check: a request for t2 with a wrong version evicts A's mapping for t1 and is then refused, leaving A with no mapping. RefusalKeepsHalfOpen fails |

Both counterexamples were replayed against the code by hand as above.
test_new_token_replaces_stale_half_open_mapping pins the code: a forged
request and a bound token leave the stale mapping in place, and a new token
under a new salt gets a cookie, then a fresh challenge from the replacing
mapping, with the stale token's claim released.

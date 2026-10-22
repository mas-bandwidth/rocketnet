# ReceiveOrder: cheap refusals first

`ReceiveOrder.tla` models a receive path as ordered rungs, each cheap or
crypto. The invariant is `CheapRefusalsFirst`: no packet refused at a cheap
rung has passed a crypto rung. Keyed hashes (the cookie MAC, the token
identifier, key derivation) are the cheap tier; keypair generation, scalar
multiplication, AEAD seal or open, signature verification and sealed-box open
are crypto.

The request path runs in the ruled order. Each rung maps to its code and
STANDARD step in _Processing Connection Requests_:

| Rung | Kind | Code | STANDARD step |
|---|---|---|---|
| restart | cheap | already-connected and mapping lookups; the address lookup runs right after the size check (t12 rule 3), and the client-id check after the seal open | t12 rule 3: already connected; resend stored challenge |
| version | cheap | sdk/rocketnet_server_protocol.cpp version, then protocol id compare | TRANSPORT.md t12 rule 4: version, then protocol id |
| timestamps | cheap | expire and creation skew compares, then the timeout compare | t12 rule 5: expiry, creation skew; t12 rule 6: timeout seconds |
| compares | cheap | address flags, advertised address, server key compares, in that order | t12 rule 6: address flags; advertised address; server public key |
| cookie | cheap | cookie MAC and budget gate | recompute the cookie MAC |
| used | cheap | used-token history lookup | token identifier in used-token history |
| capacity | cheap | rocketnet_encryption_map_full in sdk/rocketnet_server_protocol.cpp | a full encryption mapping table refuses before any signature, seal or keypair work |
| claim | cheap | ClaimCheck: rocketnet_used_token_history_claim_check, then (after the capacity rung) the read-only rocketnet_used_token_history_claim_can_record, in sdk/rocketnet_token_tables.cpp; ClaimWrite: rocketnet_used_token_history_claim_record, run only after the signature verifies | the first request whose signature verifies claims its token for its source address until the handshake deadline; a second address is refused |
| admitted | cheap | removed with the admitted-token set (`docs/SESSION.md` 1.6): the used-token history rung is now the cheap refusal of a token a connected client holds, from any other address; the model keeps the position | (none) |
| signature | crypto | crypto_sign_verify_detached in sdk/rocketnet_connect_token.cpp | verify the signature |
| seal | crypto | crypto_box_seal_open in sdk/rocketnet_connect_token.cpp | open the sealed private block |
| slots | cheap | rocketnet_client_slots_num_free in sdk/rocketnet_server_protocol.cpp | no client slots available |
| keypair | crypto | crypto_box_keypair in sdk/rocketnet_server_protocol.cpp | obtain the server ephemeral keypair |

## Witnesses

Each witness is the same module under a Broken cfg, moving one cheap rung
behind a crypto rung or removing the token-to-address claim. TLC must reject
them with `CheapRefusalsFirst` or `OneLiveMappingPerToken`:

| Witness | Order | Code replay |
|---|---|---|
| SlotsAfterKeypair | slots at 11, keypair at 10 | BASE: crypto_box_keypair and the scalar multiplication inside rocketnet_derive_connection_keys run before rocketnet_client_slots_num_free, so a full server pays for a keypair on every denial |
| AdmittedAfterVerify | admitted at 8, signature at 6 | BASE order with the admitted lookup behind the signature verify: a cheap refusal decided only after signature work |
| VersionNotFirst | version at 9, behind the signature at 8 | a version refusal decided only after signature work; the version byte is a fixed-offset compare and belongs first |
| TokenManyAddresses | claim removed (ClaimAtRequest FALSE) | BASE: the token was not claimed at the request, so one token from many addresses bought many half-open mappings and full crypto until the map filled; OneLiveMappingPerToken fails |
| ClaimBeforeSignature | claim written at its check rung (ClaimWriteRung "claim") | BASE before the split: rocketnet_used_token_history_claim_record ran at the claim rung, ahead of rocketnet_verify_signed_connect_data, and a failed signature released it after; every unauthenticated request past the cookie wrote (and then released) a claim-table entry. ClaimOnlyAfterSignature fails |

Replayed by hand: in ClaimBeforeSignature a request with fault `signature`
advances past the claim rung (position 65) with no other claim on its token,
so `claimWritten` holds while `verified` does not, and the signature rung
then refuses it; this is the BASE code writing the claim at the rung and
releasing it on the failed verify. With the split the write happens on
passing the signature rung, the same request is refused with
`claimWritten` FALSE, and test_claim_written_only_after_signature_verifies
pins it in the code (no claim-table entry after a forged signature).

Replayed by hand: in SlotsAfterKeypair a full-server packet (fault `full`)
passes the keypair rung, so `spent` holds when the slots rung refuses, and
`CheapRefusalsFirst` fails exactly as the tally test fails at BASE with
keypair 1. The fix moves the slots rung ahead of the keypair, and the same
packet is refused with the tally at zero.

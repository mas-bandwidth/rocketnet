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
| version | cheap | sdk/rocketnet_server_protocol.cpp version compare | version is not 0x4E7C1A93 |
| timestamps | cheap | expire and timeout compares | expire timestamp, timeout seconds |
| compares | cheap | address flags, advertised address, server key compares | address flags; advertised address; server public key |
| cookie | cheap | cookie MAC and budget gate | recompute the cookie MAC |
| used | cheap | used-token history lookup | token identifier in used-token history |
| admitted | cheap | removed with the admitted-token set (`docs/SESSION.md` 1.6): the used-token history rung is now the cheap refusal of a token a connected client holds; the model keeps the position | (none) |
| restart | cheap | already-connected and mapping lookups | already connected; resend stored challenge |
| signature | crypto | crypto_sign_verify_detached in sdk/rocketnet_connect_token.cpp | verify the signature |
| seal | crypto | crypto_box_seal_open in sdk/rocketnet_connect_token.cpp | open the sealed private block |
| slots | cheap | rocketnet_client_slots_num_free in sdk/rocketnet_server_protocol.cpp | no client slots available |
| keypair | crypto | crypto_box_keypair in sdk/rocketnet_server_protocol.cpp | obtain the server ephemeral keypair |

The code holds the slots and keypair rungs once this card lands: the slot
check seals the denial under the handshake keys, which need no ephemeral
pair, so it stands ahead of the keypair. The remaining rungs reach the model
order as the token-order cards land; until then the table states the target
order the model checks.

## Witnesses

Each witness is the same module under a Broken cfg, moving one cheap rung
behind a crypto rung. TLC must reject all three with `CheapRefusalsFirst`:

| Witness | Order | Code replay |
|---|---|---|
| SlotsAfterKeypair | slots at 11, keypair at 10 | BASE: crypto_box_keypair and the scalar multiplication inside rocketnet_derive_connection_keys run before rocketnet_client_slots_num_free, so a full server pays for a keypair on every denial |
| AdmittedAfterVerify | admitted at 8, signature at 6 | BASE order with the admitted lookup behind the signature verify: a cheap refusal decided only after signature work |
| VersionNotFirst | version at 9, behind the signature at 8 | a version refusal decided only after signature work; the version byte is a fixed-offset compare and belongs first |

Replayed by hand: in SlotsAfterKeypair a full-server packet (fault `full`)
passes the keypair rung, so `spent` holds when the slots rung refuses, and
`CheapRefusalsFirst` fails exactly as the tally test fails at BASE with
keypair 1. The fix moves the slots rung ahead of the keypair, and the same
packet is refused with the tally at zero.

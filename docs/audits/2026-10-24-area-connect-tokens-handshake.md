# Connect tokens and handshake audit, 2026-10-24

Cold audit of one area of `mas-bandwidth/rocketnet` at
`9775266a705148969cc00fcadc1463b06879cfbd` (`main`): connect tokens and the
handshake that admits them. Scope read completely: `sdk/rocketnet_connect_token.h`,
`sdk/rocketnet_connect_token.cpp`, `sdk/rocketnet_token_tables.h`,
`sdk/rocketnet_token_tables.cpp`; the connection request, challenge, response,
refusal, half-open removal and cookie rotation paths of
`sdk/rocketnet_server_protocol.cpp`; the connect, cookie, challenge, denial and
deadline paths of `sdk/rocketnet_client_protocol.cpp`; the token and handshake
sections of `standard/TRANSPORT.md` (t6, t10, t11, t12, t13.2, t17);
`SECURITY.md`; and the tests that pin them (`tests/test_unit_standard_connect_token.cpp`,
`tests/test_unit_connect_token_*.cpp`, `tests/test_unit_token_*.cpp`,
`tests/test_unit_public_connect_token.cpp`,
`tests/test_unit_standard_handshake_server.cpp`). Method: each candidate was
traced to the input or call sequence that reaches it and compared against the
standard, the public header and the code's own comments; two candidates were
settled with temporary unit tests (time as an argument, no sockets) that were
run once and deleted. Packet encryption, keys and replay protection are out of
scope. No code was changed. Findings are ordered most severe first; each gives
severity (`bug` / `risk` / `quality`), `file:line` at 9775266a, a one-sentence
defect, a one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_connect_token.cpp:482`

- Defect: `rn_dev_token_address` accepts any address that parses as the right family, and `rocketnet_address_parse` gives an address with no port (or `:0`) port 0, so `rocketnet_create_dev_connect_token` (and `rocketnet_write_production_connect_token`, line 259, by the same path) mints a token naming port 0, which every client refuses at `sdk/rocketnet_client_transport.cpp:1332`.
- Failure or cost: `rocketnet_create_dev_connect_token( "127.0.0.1", ... )` returns `ROCKETNET_STATUS_OK` and `rocketnet_client_connect` then refuses the token with `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO`, although `sdk/rocketnet.h:1471` promises `ROCKETNET_STATUS_INVALID_ADDRESS` for an invalid address (checked with a temporary unit test: both `"127.0.0.1"` and `"127.0.0.1:0"` return OK, and connect returns -31).
- Smallest fix: refuse `parsed.port == 0` with `ROCKETNET_STATUS_INVALID_ADDRESS` in `rn_dev_token_address` and in the address loop of `rocketnet_write_production_connect_token`, so no SDK token maker produces a token the SDK client refuses.

### 2. risk — `sdk/rocketnet_server_protocol.cpp:819`

- Defect: the response path records the response binding but never releases the token's request claim, although `standard/TRANSPORT.md` t12 rule 9 and t17 item 13, the header comment at `sdk/rocketnet_token_tables.h:61` and the comment at line 812 ("it replaces the request claim") all say the claim ends when a response binds the token.
- Failure or cost: the claim of every admitted connection stays live until the handshake deadline (checked with a temporary unit test: after the response connects, `rocketnet_used_token_history_claim_check` from a second address returns 0 until the deadline), so once the capacity-bounded binding table evicts that binding under connect churn, a request the standard admits is refused as `CONNECTION_REQUEST_TOKEN_ALREADY_USED`, and admitted connections hold claim-table capacity they no longer need.
- Smallest fix: call `rocketnet_used_token_history_claim_release` for the mapping's token and address beside the binding record, or change the standard, the header and the comment to say the claim lives to the handshake deadline.

### 3. risk — `sdk/rocketnet_client_protocol.cpp:189`

- Defect: `rocketnet_client_protocol_connect` resets and begins a fresh attempt from whatever state the client is in, while `standard/TRANSPORT.md` t11 rule 1 requires a token to be accepted only in disconnected (0) and ignored, with no state change, in every other state.
- Failure or cost: the rule holds today only because the one production caller (`sdk/rocketnet_client_transport.cpp:1300`) runs connect once straight after `rocketnet_client_protocol_init`; any later caller that connects a live client (a reconnect path) silently tears down a connected or handshaking attempt instead of ignoring the token.
- Smallest fix: return 0 without touching any state unless `client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED`, and pin it with a unit case.

### 4. quality — `sdk/rocketnet_server_protocol.cpp:538`

- Defect: the cheap rungs between the cookie and the signature run in three different orders: the code checks used token, failed-signature budget, claim, mapping capacity, claim capacity; `standard/TRANSPORT.md` t12 rule 9 states used token and claim, then mapping capacity, then the failed-signature budget; and `tla/ReceiveOrder.tla` (`Pos`, "capacity" at `10 * PosUsed + 2` before "claim" at `+ 5`) puts mapping capacity before the claim.
- Failure or cost: a request that fails two of these rungs is counted differently by the code, the standard's sentence order and the model (a source with a recorded signature failure presenting a token claimed elsewhere counts `CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT` here, not `CONNECTION_REQUEST_TOKEN_ALREADY_USED`), and t10 requires exactly one named counter per refusal.
- Smallest fix: choose one order, write it into t12 rule 9 as numbered steps, and make the code and the model's `Pos` follow it.

### 5. quality — `sdk/rocketnet_token_tables.cpp:220`

- Defect: the comment says the slot scans "run only on the record path, which is bounded by genuinely issued tokens", but `rocketnet_used_token_history_claim_can_record` (line 427) scans the claim table on every cookie-validated request before the signature, and `rocketnet_used_token_history_claim_release` (line 449) leaves a released entry linked, so once the claim table has filled once its free list stays empty and every can-record and claim-record walks the deadline array from slot 0.
- Failure or cost: a request on the unauthenticated side of the signature can cost a walk of up to 8 x max_clients deadlines, a cost the comment tells a maintainer does not exist.
- Smallest fix: correct the comment to name the can-record scan and its bound, or return released and expired claim slots to the free list so the scan only runs when the table is full of live claims.

### 6. quality — `sdk/rocketnet_server_protocol.cpp:576`

- Defect: a full request-claim table is counted as `CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING` (also at line 596), the mapping-table counter, without `MAPPING_TABLE_FULL`, while the mapping table itself has room.
- Failure or cost: an operator reading `COULD_NOT_ADD_MAPPING` above `MAPPING_TABLE_FULL` cannot tell claim-table exhaustion from a mapping failure, and the standard names no counter for this refusal.
- Smallest fix: give the claim-capacity refusal its own counter (and a row in t18), or name in the counter's description that it also counts a full claim table.

### 7. quality — `sdk/rocketnet_server_protocol.cpp:1189`

- Defect: a connection response that opened under the handshake key but carries nonzero padding is counted as `CONNECTION_RESPONSE_DID_NOT_OPEN` and charged to the integrity limit, while `standard/TRANSPORT.md` t10 rule 13 reserves `DID_NOT_OPEN` and the integrity count for a packet that does not open or whose handshake plaintext size is not exact.
- Failure or cost: an authentic packet, which only the token's key holder can produce, is reported as a forgery attempt, so the forgery counter and the integrity budget no longer mean what t10 and t14 say they mean.
- Smallest fix: count nonzero padding under its own reason (or the response's wrong-size counter) without calling `rocketnet_server_integrity_failure`, or add the padding check to t10 rule 13.

### 8. quality — `sdk/rocketnet_server_protocol.cpp:735`

- Defect: `rocketnet_server_process_opened_request` releases the claim when the mapping add fails, and its caller releases it again at line 623 for every result other than `ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE`.
- Failure or cost: the release rule is written twice, so a reader cannot tell which site owns it and a change to one can drift from the other.
- Smallest fix: delete the release at line 735 and leave the caller's single release for every refusal.

### 9. quality — `sdk/rocketnet_client_protocol.cpp:57`

- Defect: comments in this area cite sections and line numbers that no longer exist: `STANDARD.md` lines 1095-1098 and 2425-2426 (here), `STANDARD.md` _Processing Connection Requests_, _Processing Connection Response Packets_, _Reading packets_ and _Half-open state_ (`sdk/rocketnet_server_protocol.cpp:550`, `:664`, `:674`, `:1575`; `sdk/rocketnet_client_protocol.cpp:485`, `:612`, `:1206`), `docs/SESSION.md`, Token reuse (`sdk/rocketnet_connect_token.h:297`) and `docs/SESSION.md` 1.6 (`sdk/rocketnet_server_protocol.cpp:832`), while `STANDARD.md` is now a 14-line index and `docs/SESSION.md` a 26-line page with neither section.
- Failure or cost: a maintainer checking a security rule against its cited source finds nothing at the reference and has to search `standard/TRANSPORT.md` to learn which numbered rule the code implements.
- Smallest fix: replace each reference with the `standard/TRANSPORT.md` rule id it means (t10, t11, t12 rule 9, t12 rule 14, t13.4), as `sdk/rocketnet_client_protocol.cpp:195` already does.

### 10. quality — `sdk/rocketnet_connect_token.cpp:469`

- Defect: the comment describing `rocketnet_create_dev_connect_token` ("the public development token ... nothing is written on a failure") runs straight into the comment for `rn_dev_token_address` above that helper, 16 lines before the function it describes.
- Failure or cost: a reader takes the no-write-on-failure promise as a property of the address helper and finds the public function itself undocumented in the source.
- Smallest fix: move the first three comment lines to directly above `rocketnet_create_dev_connect_token`.

### 11. quality — `sdk/rocketnet_server_protocol.cpp:808`

- Defect: `rocketnet_server_process_connection_response` casts `current_time` to void as unused, then uses it at line 880 (`mapping->last_send_time = current_time`) and line 889.
- Failure or cost: the cast tells a reader the response path ignores the clock when it stamps the mapping's send time from it.
- Smallest fix: delete the `(void) current_time;` line.

## Counts

- bug: 1 (finding 1)
- risk: 2 (findings 2, 3)
- quality: 8 (findings 4–11)
- total: 11

## Already filed

- `sdk/rocketnet_server_protocol.cpp:33`, connection ids issued by a bare pre-increment with no skip of zero: `docs/audits/2026-10-24-code-quality-b.md`, finding 5.

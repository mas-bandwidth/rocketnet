# RocketNet backend examples

A client never connects to a RocketNet server by passing a raw IP address to the
client API. Every client connects with a 768-byte connect token (encoded as
exactly 1024 characters of base64 in a
`ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES` = 1025-byte buffer), and the game's
backend mints that token.

The Go and C# examples issue tokens and keep one seat per authenticated
account. Both expose the same HTTP API. To try a ban while a player is
connected, follow [Bans and whitelists](../../docs/BANS-AND-WHITELISTS.md#run-it).

| Method and route | Request and response |
| --- | --- |
| `PUT /servers/{serverID}` | Register the server's advertised UDP addresses and 32-byte X25519 public key. The example game server sends its bound port and public key after startup. |
| `GET /servers/{serverID}/whitelist` | Return the ids allowed on this server in ascending order, each as eight little-endian bytes. The response type is `application/octet-stream`. An empty list has a zero-length body. |
| `POST /mock/ticket/{userID}` | Issue a short-lived mock sign-in ticket for the requested user. This route exists for local testing. |
| `POST /match` | Send the ticket as the request body. The backend verifies it, derives the account's `client_id`, selects a server, checks the ban and records the seat before issuing a token. The response is base64 text with `Cache-Control: no-store`. Pass it directly to `rocketnet_client_connect`. |
| `PUT /bans/{provider}/{userID}` | Ban the account. Each subsequent whitelist omits the id, and the backend refuses to issue another token for it. |
| `DELETE /bans/{provider}/{userID}` | Lift the ban. The example retains the user's last seat, so that server's whitelist can include the id again. |

Both backends implement the 768-byte connect-token format specified in [STANDARD.md](../../docs/STANDARD.md) and implemented in
[`sdk/rocketnet_connect_token.cpp`](../../sdk/rocketnet_connect_token.cpp):

- The backend signs the 576-byte `signed_connect_data` prefix (`offsets 0..575`)
  with its 64-byte Ed25519 secret key (`signature` at `offsets 576..639`), so
  any server holding the matching 32-byte Ed25519 `backend_public_key` can
  verify that the backend issued the token.
- Inside `signed_connect_data`, the backend seals a 384-byte private block into
  the 432-byte `sealed_private_block` (`offsets 89..520`) using
  `crypto_box_seal` with the selected server's 32-byte X25519
  `server_public_key` (`offsets 57..88`), so only that server instance can open
  the client's session keys and `client_id`.
- The trailing client section (`offsets 640..703`) carries the client's copy of
  the 32-byte `client_to_server_key` and 32-byte `server_to_client_key`
  (followed by 64 zero bytes at `offsets 704..767`), allowing the client to
  complete the handshake without being able to forge the backend's signature or
  open the server's sealed block.

| Backend | Source | Dependencies |
| --- | --- | --- |
| Go | [go/main.go](go/main.go), [go/users.go](go/users.go), [go/users_http.go](go/users_http.go) | Go standard library, plus `golang.org/x/crypto` (`nacl/box` for `crypto_box_seal` and `blake2b`) |
| C# | [csharp/Backend/](csharp/Backend/) (users in [Users.cs](csharp/Backend/Users.cs)) | ASP.NET Core minimal API, plus `Sodium.Core` |

The C# project targets .NET 10 and uses `Sodium.Core` for token cryptography
(which wraps `libsodium`,
the same library the C SDK vendors under `sodium/`). **The C# project is a
backend token service only: RocketNet is a C library (`sdk/rocketnet.h`) and has
no C# client API.**

---

<a id="title-owned-integration-seams-development-vs-production"></a>

## Connecting a real game backend

- **Player sign-in.** Replace the ticket verifier constructed by
  `verifyTicketWith` in Go or `Users.VerifyWith` in C#. Have it return
  the provider and user id from a verified ticket. Remove the mock issuer,
  which issues a ticket for any requested account.
- **Server identity.** Authenticate the game server before accepting a
  registration or returning its whitelist. The example accepts registration
  JSON up to 4096 bytes on its local HTTP listener. In a deployment,
  authorize each server for its own `serverID` and protect the channel.
- **Match selection.** The example picks a registered server uniformly at
  random. A game can use its match assignment, region and available capacity.
  Matching the account to a new server moves its one seat.
- **Ban administration.** Authorize operators before accepting ban or unban
  requests. Persist bans and the user hash key across backend restarts.
  The example keeps bans, seats and registrations in memory. If you run
  multiple backend instances, give them the same user hash key and share
  the records used for matches and whitelists. See
  [Running multiple backend instances](../../docs/BANS-AND-WHITELISTS.md#running-multiple-backend-instances).

**Join pacing is title-owned too, and these examples do not do it.**
`POST /match` mints one token per request as it arrives and does not
pace the issue. When many clients join one server at once, the backend is
expected to spread the issue of their tokens over time: the server takes at
most `handshakes_per_update` connection requests (default 16) to the signature
check in one update, as its defence against handshake CPU floods, and a refused
request is sent again with a return cookie that lives about 3 seconds, inside
the token's 10-second default timeout. Keep the issue rate to one server below
`handshakes_per_update` times its update rate each second (16 x 60 = 960 a
second at 60 updates a second), spread evenly. How long a mass join takes
depends on your update rate, round trip and loss: measure yours. See
[Production integration](../../docs/PRODUCTION.md#spread-joins-over-time).

---

<a id="step-1-development-build-rocketnet_dev1-and-first-guarded-message-exchange"></a>
<a id="step-2-build-the-sdk-and-examples-for-production-rocketnet_dev0"></a>
<a id="step-3-provision-the-backend-signing-keypair-and-enforce-key-boundaries"></a>
<a id="step-4-start-the-production-server-and-backend-then-register-the-server"></a>
<a id="step-5-authenticate-the-player-mint-a-fresh-connect-token-and-connect-the-c-client"></a>
<a id="step-by-step-walkthrough-from-development-build-to-rocketnet_dev0-production"></a>

## Build and run

The complete [production integration walkthrough](../../docs/PRODUCTION.md)
shows key generation, a production SDK build, backend and server startup, and
a client exchange. Follow it for the manual path. A Release
compiler configuration alone does not set `ROCKETNET_DEV=0`.

For an automated exchange from the repository root:

```sh
cmake -S . -B build-production -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF
cmake --build build-production --target example_backend_server example_backend_client backend_check_token keygen
python3 tools/run_backend_examples.py --build build-production --production
```

The harness needs both Go and the .NET 10 SDK. The backend examples link the
selected `rocketnet` library; `backend_check_token` links `rocketnet_dev` because
it is a fixed-development-server-key test fixture. It cannot inspect a token
sealed to a live production server. The harness supplies its fixture token on
standard input, then tests a separate live server/token/client exchange.

`example_backend_server` registers itself with the backend at
`127.0.0.1:8080` (`--backend <ip:port>`) as `example-<port>`
(`--server-id <id>`), fetches its whitelist every 2 seconds on its own
thread ([whitelist.h](../whitelist.h)), and disconnects any client not on
it after a qualifying fetch. If a fetch fails, the server keeps the last
good list. Six seconds after connection it enforces that list even if it is
stale. See [server behavior during an outage](../../docs/BANS-AND-WHITELISTS.md#the-example-game-server).
It exits once a client has come and gone (`--sessions N`: N clients).
`example_backend_client --stay` stays connected after its echo until the
server disconnects it, and passes only if that happens within `--seconds`.

`example_backend_server` runs a 60 Hz frame. By default every server call is
made on the main thread. With `--game-threads N` it runs a game server's
frame: the main thread joins last frame's split-off half and reads the
messages the last update left, and the second half of the network update
(queue the echoes, send, update) is split off onto another thread to run in
parallel with the main thread until the join next frame. The
[game loop](../../docs/examples/game_loop.md) explains this frame.

Both services listen on `127.0.0.1:8080` by default. Setting
`ROCKETNET_BACKEND_LISTEN_ADDRESS` to a non-loopback address exposes the
unauthenticated routes and emits a warning. The local sample does not become
a production service merely by connecting it to a production SDK build.

Each backend also provides two command-line modes:

- `--keygen` prints fresh backend public and private keys as environment
  assignments, without starting the HTTP listener. Follow the protected-file
  instructions in [Keys](../../docs/KEYS.md).
- `--mint <clientID>` prints one token without starting the HTTP listener.
  Supply `ROCKETNET_BACKEND_SERVER_PUBLIC_KEY`, the server addresses, and the
  backend signing key through the variables below. It is an operator tool:
  it issues a token without checking a ticket, checking a ban or changing a
  seat. The game server still checks the id against its whitelist.

The long-running service retains its signing secret for successive tokens;
protect its process and environment. Key-generation mode clears its temporary
private-key buffer after printing, and minting clears temporary session-key
buffers. This does not erase the key output or token held by the caller.

## Configuration reference

All backend settings are read from environment variables so no key or deployment
address is hardcoded in source control.

| Variable | Meaning | Default |
| --- | --- | --- |
| `ROCKETNET_BACKEND_PRIVATE_KEY` | Backend's 64-byte Ed25519 secret key, base64, printed by `--keygen` (Go/C#) or as the labelled `backend private key base64` line by `./bin/keygen`. Give each server the matching 32-byte `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`. | Published development key (**development only**; refused by `ROCKETNET_DEV=0` servers) |
| `ROCKETNET_BACKEND_SERVER_PUBLIC_KEY` | Optional initial server's 32-byte X25519 public key, base64 (`rocketnet_server_public_key`). When set, the HTTP service registers it under server ID `environment`; required for `--mint`. | none |
| `ROCKETNET_BACKEND_SERVER_ADDRESSES` | Optional initial server's UDP address(es): `a.b.c.d:port`, `[ipv6]:port`, or one of each separated by a comma. | `127.0.0.1:40000` when `ROCKETNET_BACKEND_SERVER_PUBLIC_KEY` is set |
| `ROCKETNET_BACKEND_PROTOCOL_ID` | Game protocol ID (`0x` hex or decimal `uint64`); must be nonzero and match `server_config.protocol_id`. | `0x52434b544e455431` |
| `ROCKETNET_BACKEND_PORT` | Backend HTTP listen port (`1..65535`). | `8080` |
| `ROCKETNET_BACKEND_LISTEN_ADDRESS` | Literal IP for the HTTP listener; non-loopback addresses expose the unauthenticated example routes. | `127.0.0.1` |
| `ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS` | Token validity window in seconds (`expire_timestamp - create_timestamp`, `1..4294967295`). | `30` |
| `ROCKETNET_BACKEND_USER_HASH_KEY` | 32-byte secret, base64, that keys the user hash (`client_id`). Keep it fixed: a new key gives every user a new `client_id`, and bans keyed by the old ones stop matching. | A fresh random key at each start, with a warning |
| `ROCKETNET_BACKEND_MOCK_PROVIDER_KEY` | 32-byte secret, base64, that the mock auth provider signs tickets with. | A fresh random key at each start |
| `ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS` | Connection keepalive/handshake timeout in seconds (`1..2147483647`); the server also enforces its configured `max_timeout_seconds`. | `10` |

---

## Failure and refusal flows, status codes, and safe recovery actions

HTTP and synchronous client failures have explicit results. Many server-side
handshake checks silently discard a request; the client later observes a
timeout, which alone cannot identify the rejected field or distinguish it
from network loss.

### 1. Server startup refusals (`rocketnet_server_create` / `rocketnet_server_start`)

| Condition | Call that refuses | `rocketnet_status_t` | Safe action |
| --- | --- | --- | --- |
| `backend_public_key` is all zeros and `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` is unset | `rocketnet_server_start` | `ROCKETNET_STATUS_MISSING_KEYS` (`-10`) | Provision a keypair (`--keygen` or `./bin/keygen`) and pass `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` or `config.backend_public_key`. |
| Production build (`ROCKETNET_DEV=0`) given the published development backend or server key | `rocketnet_server_start` | `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION` (`-12`) | Replace the development public key with your deployment's `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`. |
| Production build (`ROCKETNET_DEV=0`) given the published test packet filter | `rocketnet_server_create` / `rocketnet_client_create` | `ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION` (`-29`) | Set a 4-byte title `packet_filter` (or `ROCKETNET_SERVER_PACKET_FILTER` / `ROCKETNET_CLIENT_PACKET_FILTER`). |
| `config.protocol_id` is `0` | `rocketnet_server_start` | `ROCKETNET_STATUS_INVALID_PROTOCOL_ID` (`-11`) | Set a nonzero 64-bit `protocol_id` matching `ROCKETNET_BACKEND_PROTOCOL_ID`. |

### 2. Backend HTTP refusals

| Route & condition | HTTP status | Safe action |
| --- | --- | --- |
| `POST /match` when no servers have registered | `503 Service Unavailable` | Start a game server (it registers itself via `PUT /servers/{serverID}`) before matching players. |
| `POST /match` with a ticket that is malformed, forged, edited or expired | `401 Unauthorized` | Sign in again with the auth provider and send the fresh ticket. |
| `POST /match` for a banned user | `403 Forbidden` | None for the client: the user is banned. An operator lifts the ban with `DELETE /bans/{provider}/{userID}`. |
| `POST /match` with a body three or more bytes longer than the longest valid ticket | `400 Bad Request` | Send only the ticket. |
| `POST /mock/ticket/{userID}`, `PUT` or `DELETE /bans/{provider}/{userID}` with a user id that is not 1 to 128 bytes of UTF-8, or a provider name that is empty or holds `:` | `400 Bad Request` | Fix the user id or provider name. |
| `GET /servers/{serverID}/whitelist` for a server that is not registered | `404 Not Found` | The game server registers again (`example_backend_server` does this on its own). |
| `PUT /servers/{serverID}` with empty `serverID` or one over the limit (Go: `> 128` bytes; C#: `> 128` characters), malformed JSON, `0` or `> 2` addresses, two addresses of the same IP family, port `0`, a hostname (`localhost:40000`) instead of a literal IP, an IPv6 zone (`%en0`), or a `public_key` that is not 32-byte base64 | `400 Bad Request` | Export the served literal IP addresses (`rocketnet_server_ipv4_address`, `rocketnet_server_ipv6_address`) with nonzero ports and the 32-byte `rocketnet_server_public_key` in base64. |
| `PUT /servers/{serverID}` contains an unknown JSON field | `400 Bad Request` (Go); accepted and ignored (C#) | Send only the documented `addresses` and `public_key` fields. Do not rely on the C# backend ignoring additional fields. |
| `PUT /servers/{serverID}` body exceeds 4096 bytes | `400 Bad Request` (Go) / `413 Payload Too Large` (C#) | Keep registration payload within the 4096-byte bound (1–2 literal address strings and one base64 public key). |

### 3. Client pre-send token refusals (`rocketnet_client_connect` fails synchronously)

When `rocketnet_client_connect(client, token)` fails before any UDP packet is
sent, it returns a negative `rocketnet_status_t` immediately:

| Condition | `rocketnet_client_connect` return | `rocketnet_client_state(client)` after refusal | Safe client action |
| --- | --- | --- | --- |
| `client` or `token` pointer is `NULL` | `ROCKETNET_STATUS_INVALID_ARGUMENT` (`-6`) | A non-NULL client keeps its previous state; do not query a NULL client | Fix the caller argument; do not enter the connected loop. |
| Token string is empty, not 1024 base64 characters, or invalid base64 (`./bin/example_backend_client --token "not-base64"`) | `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN` (`-15`) | Previous state (DISCONNECTED on a fresh client) | Request a fresh token from the backend and call `rocketnet_client_connect` again. |
| Token decodes to 768 bytes, but `wire_version != 0x4E7C1A93`, `address_flags` invalid, `timeout_seconds <= 0`, or `create_timestamp > expire_timestamp` | `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN` (`-15`) | `ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN` (`-6`) | Check backend token writer configuration; fetch a fresh valid token before retrying. |
| Token's `expire_timestamp <= current_timestamp` before the first packet is sent | `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED` (`-16`) | `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` (`-7`) | Never cache or reuse connect tokens; request a fresh token from the backend immediately before connecting. |
| Production client (`ROCKETNET_DEV=0`) given a token sealed to the published development server key | `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` (`-30`) | `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`), also on a client retrying from a terminal state | Register the live production server's X25519 public key with the backend and mint a production token. |
| Token names a flagged server address with port `0` | `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO` (`-31`) | `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`), also on a client retrying from a terminal state | Register the server's actual bound port (`rocketnet_server_ipv4_address` after `rocketnet_server_start`), never `:0`. |
| Previous connection attempt is still active or its worker thread is still retiring | `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` (`-23`) | Current active or retiring state | Call `rocketnet_client_disconnect(client)` and poll `rocketnet_client_disconnected(client)` until `true` before calling `rocketnet_client_connect`. |

### 4. Server-side handshake refusals (`rocketnet_client_connect` returns `OK`, then `rocketnet_client_state < 0`)

Once `rocketnet_client_connect` succeeds (`ROCKETNET_STATUS_OK`), the client
begins the UDP handshake (`ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE` = `1`, then
`ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST` = `2`). To avoid
amplification and oracle leaks, the server silently ignores unauthenticated or
mismatched requests and sends an explicit denial only when all slots are full:

| Server-side condition | When checked on server | Client state transition (`rocketnet_client_state_t`) | Safe client & operator action |
| --- | --- | --- | --- |
| **Wrong `protocol_id`** (`ROCKETNET_BACKEND_PROTOCOL_ID` != server `config.protocol_id`), **wrong or stale `server_public_key`** (server object was recreated with a new keypair without re-registering via `PUT /servers/{serverID}`), **unadvertised server address**, or **token expired / >30s future skew on arrival** | Before the cookie gate (dropped unanswered) | `REQUESTING_COOKIE` (`1`) -> `ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT` (`-2`) (or `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` `-7` if token expiry arrives first) | Ensure server re-registers its current `public_key` and served `addresses` on every start, align `protocol_id`, and mint a fresh token. |
| **Wrong backend signing key** (`ROCKETNET_BACKEND_PRIVATE_KEY` does not match server's `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`, failing Ed25519 signature verification), **corrupted sealed private block** (`crypto_box_seal_open` fails), **token already used/claimed by another client address**, or **`client_id` already connected** on the server | After the cookie gate (dropped unanswered) | `SENDING_CONNECTION_REQUEST` (`2`) -> `ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT` (`-3`) (or `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` `-7`) | Verify the server's `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` matches the backend's keypair; always mint a fresh, single-use token per connection attempt. |
| **Server is full** (all `config.max_clients` slots are occupied) | After signature and sealed-block verification (answered with a denial) | `SENDING_CONNECTION_REQUEST` (`2`) -> `ROCKETNET_CLIENT_STATE_CONNECTION_DENIED` (`-1`) | Select another server with free capacity in the backend matcher, mint a fresh token for that server, and reconnect. |

**Safe recovery pattern whenever `rocketnet_client_state(client) < 0`:**

1. Stop the failed attempt cleanly by calling `rocketnet_client_disconnect(client)`.
2. Poll `rocketnet_client_disconnected(client)` once per frame (calling
   `rocketnet_client_update` if the client is step-driven) until it returns
   `true`.
3. If reconnecting, request a **brand-new** connect token from the backend
   (`POST /match` with a fresh ticket) and pass it to `rocketnet_client_connect(client, new_token)`.
   If exiting, call `rocketnet_client_destroy(client)`.

---

## Unit and functional verification

The backend unit suites use no sockets and no wall-clock sleeps (`create_timestamp`
is passed explicitly). They reproduce every deterministic vector in
[`standard/vectors/connect_token_v1.json`](../../standard/vectors/connect_token_v1.json)
byte-for-byte, verify IPv4/IPv6 address encoding, open freshly minted tokens the
way a server does, and exercise configuration and registration logic at the
function level. The user tests (`go/users_test.go`, `csharp/Tests/UsersTests.cs`)
check tickets (forged, edited and expired ones get no token), user hashes,
bans, seats and whitelists, and run the ticket, match, ban and whitelist routes
with a fixed clock and no socket (Go through an in-process recorder, C# as
plain functions), asserting their status codes; both backends produce the same
ticket and user hash for the same keys. The registration handlers return
`204`, `400`, and `413`, but the unit suites do not assert those:

```sh
(cd examples/backend/go && go test -v ./...)
(cd examples/backend/csharp && dotnet run --project Tests)
```

The end-to-end functional harness builds and exercises both the Go and C#
backends against `example_backend_server`, `example_backend_client`, and
`backend_check_token`. Each backend's `--keygen` generates a fresh Ed25519
keypair in memory (never written to the test receipt). The harness first mints a
separate token sealed to the published development server key and passes it on
standard input to `backend_check_token`, with the expected client ID and
protocol ID as arguments. It then starts the backend with an empty registry to
verify HTTP `503`, starts the server (which registers itself), and runs the
ban, whitelist and sharing steps listed in
[Bans and whitelists](../../docs/BANS-AND-WHITELISTS.md#testing): a ban kicks
the client within one whitelist interval plus 0.5 seconds, a shared token or a
second sign-in as the same user gets no second seat, and with the backend gone
the server keeps its last good list and still kicks a client on no list. Passing `--production` requires both `example_backend_server` and
`example_backend_client` to report `mode=production transport=portable`:

```sh
python3 tools/run_backend_examples.py --build build-production --production
```

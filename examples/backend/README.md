# RocketNet backend examples

A client never connects to a RocketNet server by passing a raw IP address to the
client API. Every client connects with a 768-byte connect token
(`ROCKETNET_CONNECT_TOKEN_BYTES`, encoded as 1024 characters of base64 in a
`ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES` = 1025-byte buffer), and the game's
backend mints that token.

This directory contains two minimal reference backend services — one in Go and
one in C# — modeled after `yojimbo`'s matcher and Space Game's backend registry.
Each exposes two HTTP routes:

```
PUT /servers/{serverID}
GET /match/{clientID}
```

- `PUT /servers/{serverID}` registers or updates a live game server's bound UDP
  addresses (`a.b.c.d:port`, `[ipv6]:port`, or one of each) and 32-byte X25519
  public key (`rocketnet_server_public_key`).
- `GET /match/{clientID}` selects a registered server, mints a fresh connect
  token bound to that server and `clientID`, and returns it as base64 text
  (`Content-Type: text/plain; charset=utf-8`, `Cache-Control: no-store`). The C
  game client passes that opaque string directly to `rocketnet_client_connect`.

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
| Go | [go/main.go](go/main.go) | Go standard library, plus `golang.org/x/crypto` (`nacl/box` for `crypto_box_seal` and `blake2b`) |
| C# | [csharp/Backend/](csharp/Backend/) | ASP.NET Core minimal API, plus `Sodium.Core` |

The C# project targets .NET 10 and uses `Sodium.Core` for token cryptography
(which wraps `libsodium`,
the same library the C SDK vendors under `sodium/`). **The C# project is a
backend token service only: RocketNet is a C library (`sdk/rocketnet.h`) and has
no C# client API.**

---

## Title-owned integration seams (development vs. production)

These examples keep the HTTP surface minimal so the cryptographic token flow can
be read and tested end-to-end on `localhost`. Two routes are **title-owned
integration seams** whose authentication and policy belong to the game's
platform infrastructure in production:

1. **Server registration seam (`PUT /servers/{serverID}`) — unauthenticated in
   this example (development only):**
   In `go/main.go` and `csharp/Backend/Program.cs`, `PUT /servers/{serverID}`
   accepts any syntactically valid registration JSON up to 4096 bytes without
   caller authentication. **In production, server registration must never be
   exposed unauthenticated on a public network.** Authenticate fleet
   registrations (for example with mTLS, a fleet control-plane bearer/HMAC
   credential, or authenticated orchestrator registration) before calling
   `registry.put` / `registry.Put` so an untrusted caller cannot register a
   rogue server or overwrite a live server's address and public key.
2. **Player authentication and server selection seam (`GET /match/{clientID}`) —
   unauthenticated URL parameter and random server pick in this example
   (development only):**
   In `go/main.go` and `csharp/Backend/Program.cs`, `GET /match/{clientID}`
   trusts the decimal `clientID` in the URL path and calls `registry.random()` /
   `registry.Random()` to pick a registered server uniformly at random. **In
   production, a client must never choose its own `client_id`.** Your match or
   session endpoint must first verify the player's platform or account session
   credential, derive the authoritative `uint64_t` `client_id` on the backend,
   and select a target server by region, available capacity, or match assignment before calling
   `generateConnectToken` / `ConnectToken.Generate`.

---

<a id="step-1-development-build-rocketnet_dev1-and-first-guarded-message-exchange"></a>
<a id="step-2-build-the-sdk-and-examples-for-production-rocketnet_dev0"></a>
<a id="step-3-provision-the-backend-signing-keypair-and-enforce-key-boundaries"></a>
<a id="step-4-start-the-production-server-and-backend-then-register-the-server"></a>
<a id="step-5-authenticate-the-player-mint-a-fresh-connect-token-and-connect-the-c-client"></a>
<a id="step-by-step-walkthrough-from-development-build-to-rocketnet_dev0-production"></a>

## Build and run

The complete [production integration walkthrough](../../docs/PRODUCTION.md)
shows key generation, a production SDK build, server startup, registration, and
a client exchange in three terminals. Follow it for the manual path. A Release
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
  backend signing key through the variables below.

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

### 2. Backend HTTP refusals (`PUT /servers/{serverID}` and `GET /match/{clientID}`)

| Route & condition | HTTP status | Safe action |
| --- | --- | --- |
| `GET /match/{clientID}` when no servers have registered | `503 Service Unavailable` | Register at least one live server via `PUT /servers/{serverID}` (or wait for fleet registration) before matching players. |
| `GET /match/{clientID}` with a non-decimal or out-of-range `uint64` `clientID` | `400 Bad Request` | Pass a valid unsigned 64-bit integer derived by the backend's player authentication layer. |
| `PUT /servers/{serverID}` with empty `serverID` or one over the limit (Go: `> 128` bytes; C#: `> 128` characters), malformed JSON, `0` or `> 2` addresses, two addresses of the same IP family, port `0`, a hostname (`localhost:40000`) instead of a literal IP, an IPv6 zone (`%en0`), or a `public_key` that is not 32-byte base64 | `400 Bad Request` | Export literal bound IP addresses (`rocketnet_server_ipv4_address`, `rocketnet_server_ipv6_address`) with nonzero ports and the 32-byte `rocketnet_server_public_key` in base64. |
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
| Production client (`ROCKETNET_DEV=0`) given a token sealed to the published development server key | `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` (`-30`) | Previous state (DISCONNECTED on a fresh client) | Register the live production server's X25519 public key with the backend and mint a production token. |
| Token names a flagged server address with port `0` | `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO` (`-31`) | Previous state (DISCONNECTED on a fresh client) | Register the server's actual bound port (`rocketnet_server_ipv4_address` after `rocketnet_server_start`), never `:0`. |
| Previous connection attempt is still active or its worker thread is still retiring | `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` (`-23`) | Current active or retiring state | Call `rocketnet_client_disconnect(client)` and poll `rocketnet_client_disconnected(client)` until `true` before calling `rocketnet_client_connect`. |

### 4. Server-side handshake refusals (`rocketnet_client_connect` returns `OK`, then `rocketnet_client_state < 0`)

Once `rocketnet_client_connect` succeeds (`ROCKETNET_STATUS_OK`), the client
begins the UDP handshake (`ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE` = `1`, then
`ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST` = `2`). To avoid
amplification and oracle leaks, the server silently ignores unauthenticated or
mismatched requests and sends an explicit denial only when all slots are full:

| Server-side condition | When checked on server | Client state transition (`rocketnet_client_state_t`) | Safe client & operator action |
| --- | --- | --- | --- |
| **Wrong `protocol_id`** (`ROCKETNET_BACKEND_PROTOCOL_ID` != server `config.protocol_id`), **wrong or stale `server_public_key`** (server object was recreated with a new keypair without re-registering via `PUT /servers/{serverID}`), **unadvertised server address**, or **token expired / >30s future skew on arrival** | Before the cookie gate (`ROCKETNET_SERVER_PROCESS_IGNORED`) | `REQUESTING_COOKIE` (`1`) -> `ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT` (`-2`) (or `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` `-7` if token expiry arrives first) | Ensure server re-registers its current `public_key` and bound `addresses` on every start, align `protocol_id`, and mint a fresh token. |
| **Wrong backend signing key** (`ROCKETNET_BACKEND_PRIVATE_KEY` does not match server's `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`, failing Ed25519 signature verification), **corrupted sealed private block** (`crypto_box_seal_open` fails), **token already used/claimed by another client address**, or **`client_id` already connected** on the server | After the cookie gate (`ROCKETNET_SERVER_PROCESS_IGNORED`) | `SENDING_CONNECTION_REQUEST` (`2`) -> `ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT` (`-3`) (or `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` `-7`) | Verify the server's `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` matches the backend's keypair; always mint a fresh, single-use token per connection attempt. |
| **Server is full** (all `config.max_clients` slots are occupied) | After signature and sealed-block verification (`ROCKETNET_SERVER_PROCESS_SEND_DENIED`) | `SENDING_CONNECTION_REQUEST` (`2`) -> `ROCKETNET_CLIENT_STATE_CONNECTION_DENIED` (`-1`) | Select another server with free capacity in the backend matcher, mint a fresh token for that server, and reconnect. |

**Safe recovery pattern whenever `rocketnet_client_state(client) < 0`:**

1. Stop the failed attempt cleanly by calling `rocketnet_client_disconnect(client)`.
2. Poll `rocketnet_client_disconnected(client)` once per frame (calling
   `rocketnet_client_update` if the client is step-driven) until it returns
   `true`.
3. If reconnecting, request a **brand-new** connect token from the backend
   (`GET /match/{clientID}`) and pass it to `rocketnet_client_connect(client, new_token)`.
   If exiting, call `rocketnet_client_destroy(client)`.

---

## Unit and functional verification

The backend unit suites use no sockets and no wall-clock sleeps (`create_timestamp`
is passed explicitly). They reproduce every deterministic vector in
[`standard/vectors/connect_token_v1.json`](../../standard/vectors/connect_token_v1.json)
byte-for-byte, verify IPv4/IPv6 address encoding, open freshly minted tokens the
way a server does, and exercise configuration, registration, and match refusal
logic at the function level. The HTTP handlers return `204`, `400`, and `413`
(registration) and `200`, `400`, and `503` (match), but the unit suites do not
assert those HTTP status codes:

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
verify HTTP `503`, registers the live server over `PUT /servers/example` (`HTTP 204`), obtains a fresh token from the HTTP match route, and runs the production
client-server message exchange without passing that live-server token to the
checker. Passing `--production` requires both `example_backend_server` and
`example_backend_client` to report `mode=production transport=portable`:

```sh
python3 tools/run_backend_examples.py --build build-production --production
```

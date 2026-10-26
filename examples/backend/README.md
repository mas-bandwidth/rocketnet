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

The token written by both backends is byte-for-byte identical to the 768-byte
connect token specified in [STANDARD.md](../../STANDARD.md) and implemented in
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

.NET does not include Ed25519, X25519, XSalsa20-Poly1305, or BLAKE2b in the base
class library, so the C# backend uses `Sodium.Core` (which wraps `libsodium`,
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
   credential, or an internal orchestrator network) before calling
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
   and select a target server by region, capacity (such as the default
   1000-player `ROCKETNET_MAX_CLIENTS` server configuration under its stated
   workload and deployment conditions), or match assignment before calling
   `generateConnectToken` / `ConnectToken.Generate`.

---

## Step-by-step walkthrough: from development build to `ROCKETNET_DEV=0` production

### Step 1: Development build (`ROCKETNET_DEV=1`) and first guarded message exchange

A default CMake configuration builds the development library (`ROCKETNET_DEV=1`),
which compiles in `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and
`rocketnet_create_dev_connect_token` so a local client and server can exchange
messages before you stand up a backend:

```sh
cmake -S . -B build
cmake --build build --target example_client_server_server example_client_server_client example_backend_server example_backend_client
```

In `examples/client_server_server.c` and `examples/backend_server.c`, the send
path checks the pointer returned by `rocketnet_server_start_message` (and in
production code, every caller of `rocketnet_client_start_message` or
`rocketnet_server_start_message` must guard `p != NULL`) before writing payload
bytes and calling `finish_message`:

```c
uint8_t * p = rocketnet_server_start_message( server, (uint64_t) client_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] );
if ( !p )
    continue;
memcpy( p, batch->data[i], (size_t) batch->bytes[i] );
echoed |= rocketnet_server_finish_message( server, batch->bytes[i] ) != 0;
```

Run the development pair or the backend example runner to confirm the baseline
message exchange:

```sh
python3 tools/run_backend_examples.py --build build
```

In a `ROCKETNET_DEV=1` build, `example_backend_server` and
`example_backend_client` report `mode=development transport=portable`.

### Step 2: Build the SDK and examples for production (`ROCKETNET_DEV=0`)

Configure a separate build tree with `-DROCKETNET_DEV=OFF` (`ROCKETNET_DEV=0`):

```sh
cmake -S . -B build-production -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF
cmake --build build-production --target example_backend_server example_backend_client backend_check_token keygen
```

What `-DROCKETNET_DEV=OFF` enforces at compile time and runtime:

- `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and `rocketnet_create_dev_connect_token`
  are compiled out of `sdk/rocketnet.h` and the library.
- `rocketnet_server_start` refuses an all-zero `backend_public_key` with
  `ROCKETNET_STATUS_MISSING_KEYS` (`-10`) and refuses the published development
  backend or server key with `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION` (`-12`).
- `rocketnet_client_create` and `rocketnet_server_create` refuse the published
  development packet filter (`ROCKETNET_TEST_FILTER_VALUE`) with
  `ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION` (`-29`).
  `example_backend_server` and `example_backend_client` set the non-secret
  4-byte title packet filter `RNBE` (`{ 'R', 'N', 'B', 'E' }`); another filter
  can be supplied via matching base64 `ROCKETNET_SERVER_PACKET_FILTER` and
  `ROCKETNET_CLIENT_PACKET_FILTER`.
- `rocketnet_client_connect` refuses any token sealed to the published
  development server key with
  `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` (`-30`).

Verify that starting the production server without a backend public key fails
closed immediately with `MISSING_KEYS`:

```sh
unset ROCKETNET_SERVER_BACKEND_PUBLIC_KEY
./bin/example_backend_server
# FAIL: server start: MISSING_KEYS
```

### Step 3: Provision the backend signing keypair and enforce key boundaries

Generate a fresh Ed25519 backend signing keypair using the Go backend, the C#
backend, or the native `keygen` tool. From the repository root, choose Go:

```sh
umask 077
(cd examples/backend/go && go run . --keygen) > backend-keys.env
```

or C#:

```sh
(cd examples/backend/csharp && dotnet build Backend)
umask 077
(cd examples/backend/csharp && dotnet run --no-build --project Backend -- --keygen) > backend-keys.env
```

or the C SDK `keygen` executable. The Go and C# `--keygen` modes print the
two environment assignments below directly; the C `keygen` tool instead
prints labelled lines (`backend public key base64 = ...` and
`backend private key base64 = ...`, `keygen.cpp`), so convert its output:

```sh
umask 077
./bin/keygen > backend-keys.txt
printf 'ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=%s\n' "$(sed -n 's/^backend public key base64 = //p' backend-keys.txt)" > backend-keys.env
printf 'ROCKETNET_BACKEND_PRIVATE_KEY=%s\n' "$(sed -n 's/^backend private key base64 = //p' backend-keys.txt)" >> backend-keys.env
```

The Go and C# commands print the same two base64 environment variable assignments
without starting a network listener; the C tool prints the same key material
under the labels shown above:

- `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` (32-byte Ed25519 public verification
  key, `ROCKETNET_BACKEND_PUBLIC_KEY_BYTES` = 32)
- `ROCKETNET_BACKEND_PRIVATE_KEY` (64-byte Ed25519 secret signing key: 32-byte
  seed followed by the 32-byte public key)

**Key boundaries (see [KEYS.md](../../KEYS.md)):**

| Material | Size | Where it lives | Must never reach |
| --- | --- | --- | --- |
| `ROCKETNET_BACKEND_PRIVATE_KEY` | 64 bytes (base64) | Backend token service only | Game clients, game servers, source control, logs, or docs |
| `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` | 32 bytes (base64) | Every game server (`config.backend_public_key` or env) | Not secret (public verification key) |
| Server X25519 secret key | 32 bytes (in-memory) | Generated inside `rocketnet_server_create`; wiped by `rocketnet_server_destroy` | Never exported by any API |
| Server X25519 public key (`rocketnet_server_public_key`) | 32 bytes (base64) | Exported by each live server and registered with the backend | Not secret (public sealing key) |
| Per-token session keys (`client_to_server_key`, `server_to_client_key`) | 2 x 32 bytes | Minted per token, sealed to the server, included in the token for the client, then wiped in backend memory | Logs, persistent storage, or other clients |

The long-running Go and C# services retain the backend private signing key in
their configuration for the service lifetime so they can sign successive
tokens. Protect the service process and its environment accordingly, and
restart it when rotating the key. The Go and C# backend `--keygen` modes zero
their temporary generated private-key buffers after printing the environment
assignments, and each token mint zeroes its two temporary per-token session-key
buffers after the token has been written.

### Step 4: Start the production server and backend, then register the server

In **Terminal 1 (game server)**, load only the public verification key and start
`example_backend_server`:

```sh
export ROCKETNET_SERVER_BACKEND_PUBLIC_KEY="$(grep '^ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=' backend-keys.env | cut -d= -f2-)"
./bin/example_backend_server
```

Verify that the server reports `mode=production` and prints its bound UDP
address and freshly generated 32-byte X25519 public key (also written to
`backend.env`, which contains public registration fields only and no secrets):

```
backend_server: mode=production transport=portable
ROCKETNET_BACKEND_SERVER_ADDRESSES=127.0.0.1:40000
ROCKETNET_BACKEND_SERVER_PUBLIC_KEY=<32-byte-server-public-key-base64>
```

In **Terminal 2 (backend token service)**, load `ROCKETNET_BACKEND_PRIVATE_KEY`
and start either the Go or C# backend:

```sh
export ROCKETNET_BACKEND_PRIVATE_KEY="$(grep '^ROCKETNET_BACKEND_PRIVATE_KEY=' backend-keys.env | cut -d= -f2-)"
# Go backend:
(cd examples/backend/go && go run .)
# Or C# backend:
# (cd examples/backend/csharp && dotnet run --project Backend)
```

Both backends listen on `http://localhost:8080` by default
(`ROCKETNET_BACKEND_PORT`). Before any server is registered, confirm that
requesting a match token fails with HTTP `503 Service Unavailable`:

```sh
curl -i http://localhost:8080/match/42
# HTTP/1.1 503 Service Unavailable
```

Now register the live server from **Terminal 3** using the public address and
public key written to `backend.env` (*title-owned seam: in production, this call
is authenticated on your fleet control plane*):

```sh
. ./backend.env
curl -i -X PUT http://localhost:8080/servers/local \
  -H 'Content-Type: application/json' \
  --data "{\"addresses\":[\"${ROCKETNET_BACKEND_SERVER_ADDRESSES}\"],\"public_key\":\"${ROCKETNET_BACKEND_SERVER_PUBLIC_KEY}\"}"
```

Verify that the backend responds with `HTTP/1.1 204 No Content`. A repeated
`PUT /servers/local` atomically replaces that server's addresses and public key
(for example when a server restarts and generates a new X25519 keypair).

### Step 5: Authenticate the player, mint a fresh connect token, and connect the C client

At the **title-owned player authentication and server selection seam**, the
backend authenticates the player, selects a registered server, and mints a fresh
768-byte connect token bound to:

- `protocol_id`: `0x52434b544e455431` by default (`ROCKETNET_BACKEND_PROTOCOL_ID`),
  matching the server's `config.protocol_id`;
- `create_timestamp` and `expire_timestamp`: current Unix time in seconds and
  `create_timestamp + 30` (`ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS`);
- `timeout_seconds`: `10` (`ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS`);
- `addresses` and `address_flags`: the selected server's registered IPv4 and/or
  IPv6 UDP endpoints;
- `server_public_key`: the selected server's 32-byte X25519 public key, to which
  the private block (`client_id`, inner `protocol_id`, inner `expire_timestamp`,
  `client_to_server_key`, `server_to_client_key`, and 256-byte `user_data`) is
  sealed with `crypto_box_seal`.

`backend_check_token` is a development-server-key functional-test fixture, not
an inspector for a token sealed to a live production server. It opens the
sealed block with the published development server secret key and therefore
requires a separately minted token whose `server_public_key` is the published
development server public key. The harness described below supplies that
fixture token on standard input and passes the expected client ID, protocol ID,
and optional backend public key on the command line.

To exercise the supported production path, request a fresh token (tokens expire
after 30 seconds by default, so fetch immediately before connecting) and pass
the opaque base64 string to the production C client:

```sh
./bin/example_backend_client --token "$(curl -sSf http://localhost:8080/match/42)"
```

Expected output across the client and server terminals:

- Client prints:
  ```
  backend_client: mode=production transport=portable
  client state: DISCONNECTED -> REQUESTING_COOKIE
  client state: REQUESTING_COOKIE -> SENDING_CONNECTION_REQUEST
  client state: SENDING_CONNECTION_REQUEST -> SENDING_CONNECTION_RESPONSE
  client state: SENDING_CONNECTION_RESPONSE -> CONNECTED
  client received: hello from the backend's client
  client state: CONNECTED -> DISCONNECTED
  client state after disconnect: DISCONNECTED
  PASS
  ```
- Server prints:
  ```
  backend_server: mode=production transport=portable
  ...
  server received from client 2a: hello from the backend's client
  server client disconnected (reason SERVER_DISCONNECTED)
  PASS
  ```
  (`0x2a` = decimal `42`, the authenticated `client_id` recovered by opening the
  token's sealed private block on the server) and then exits cleanly.

To mint a single token on the command line without starting the HTTP listener,
pass `--mint <clientID>` with `ROCKETNET_BACKEND_SERVER_PUBLIC_KEY` and
`ROCKETNET_BACKEND_SERVER_ADDRESSES` set in the environment.

---

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
| `ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS` | Token validity window in seconds (`expire_timestamp - create_timestamp`, `> 0`). | `30` |
| `ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS` | Connection keepalive/handshake timeout in seconds (any positive value `> 0`; both backends refuse only `<= 0`, with no upper bound in source). | `10` |

---

## Failure and refusal flows, status codes, and safe recovery actions

Every refusal is explicit and deterministic across the HTTP backend, the client
pre-send check (`rocketnet_client_connect`), and the server handshake
(`rocketnet_client_update` / `rocketnet_client_state`).

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
| `client` or `token` pointer is `NULL` | `ROCKETNET_STATUS_INVALID_ARGUMENT` (`-6`) | `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`) | Fix the caller argument; do not enter the connected loop. |
| Token string is empty, not 1024 base64 characters, or invalid base64 (`./bin/example_backend_client --token "not-base64"`) | `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN` (`-15`) | `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`) | Request a fresh token from the backend and call `rocketnet_client_connect` again. |
| Token decodes to 768 bytes, but `wire_version != 0x4E7C1A93`, `address_flags` invalid, `timeout_seconds <= 0`, or `create_timestamp > expire_timestamp` | `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN` (`-15`) | `ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN` (`-6`) | Check backend token writer configuration; fetch a fresh valid token before retrying. |
| Token's `expire_timestamp <= current_timestamp` before the first packet is sent | `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED` (`-16`) | `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` (`-7`) | Never cache or reuse connect tokens; request a fresh token from the backend immediately before connecting. |
| Production client (`ROCKETNET_DEV=0`) given a token sealed to the published development server key | `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` (`-30`) | `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`) | Register the live production server's X25519 public key with the backend and mint a production token. |
| Token names a flagged server address with port `0` | `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO` (`-31`) | `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`) | Register the server's actual bound port (`rocketnet_server_ipv4_address` after `rocketnet_server_start`), never `:0`. |
| Previous connection attempt is still active or its worker thread is still retiring | `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` (`-23`) | Current active or retiring state | Call `rocketnet_client_disconnect(client)` and poll `rocketnet_client_disconnected(client)` until `true` before calling `rocketnet_client_connect`. |

### 4. Server-side handshake refusals (`rocketnet_client_connect` returns `OK`, then `rocketnet_client_state < 0`)

Once `rocketnet_client_connect` succeeds (`ROCKETNET_STATUS_OK`), the client
begins the UDP handshake (`ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE` = `1`, then
`ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST` = `2`). To avoid
amplification and oracle leaks, the server silently ignores unauthenticated or
mismatched requests and sends an explicit denial only when all slots are full:

| Server-side condition | When checked on server | Client state transition (`rocketnet_client_state_t`) | Safe client & operator action |
| --- | --- | --- | --- |
| **Wrong `protocol_id`** (`ROCKETNET_BACKEND_PROTOCOL_ID` != server `config.protocol_id`), **wrong or stale `server_public_key`** (server restarted and generated a new keypair without re-registering via `PUT /servers/{serverID}`), **unadvertised server address**, or **token expired / >30s future skew on arrival** | Before the cookie gate (`ROCKETNET_SERVER_PROCESS_IGNORED`) | `REQUESTING_COOKIE` (`1`) -> `ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT` (`-2`) (or `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` `-7` if token expiry arrives first) | Ensure server re-registers its current `public_key` and bound `addresses` on every start, align `protocol_id`, and mint a fresh token. |
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
[`tests/connect_token_vectors.json`](../../tests/connect_token_vectors.json)
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
verify HTTP `503`, registers the live server over `PUT /servers/example` (`HTTP
204`), obtains a fresh token from the HTTP match route, and runs the production
client-server message exchange without passing that live-server token to the
checker. Passing `--production` requires both `example_backend_server` and
`example_backend_client` to report `mode=production transport=portable`:

```sh
python3 tools/run_backend_examples.py --build build-production --production
```

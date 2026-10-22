# RocketNet backend examples

A client connects to a RocketNet server with a connect token, and the game's
backend makes it. These are two very simple backends that do it, the way
yojimbo's matcher does for yojimbo: one in Go, one in C#. Each serves one
endpoint:

```
GET /match/{clientID}
```

which returns a fresh connect token for that client as base64 text. The game
client passes that string to `rocketnet_client_connect`, and the token sends it
to the one server it names.

The token is the 768-byte connect token in [STANDARD.md](../../STANDARD.md),
byte for byte the one `sdk/rocketnet_connect_token.cpp` writes. The backend
signs the token's signed connect data with its Ed25519 key, so the server knows
the backend made it, and seals the private block inside it (`crypto_box_seal`)
to the server's X25519 public key, so only that server can read the client's
session keys.

A real backend authenticates the player before it mints them a token. These
mint for any client id they are asked for.

| Backend | Source | Dependencies |
| --- | --- | --- |
| Go | [go/main.go](go/main.go) | the standard library, and `golang.org/x/crypto` for `nacl/box` (`crypto_box_seal`) |
| C# | [csharp/Backend/](csharp/Backend/) | ASP.NET minimal API, and `Sodium.Core` |

C# has no Ed25519, X25519, XSalsa20-Poly1305 or BLAKE2b in .NET, so the C#
backend takes one package for them: `Sodium.Core`, which wraps libsodium, the
library the C SDK vendors in `sodium/`. The tokens are sealed and signed by the
same code that opens and verifies them. This is a backend only: RocketNet has no
C# client API.

## Running the server, a backend and a client

Generate a backend signing keypair with either backend. From the repository
root, choose Go:

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

Both commands print the same two environment assignments:
`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` (32-byte Ed25519 public key) and
`ROCKETNET_BACKEND_PRIVATE_KEY` (64-byte Ed25519 secret key), encoded as base64.
They need no server configuration and do not start the backend. Keep the
private key with the backend; give servers only the public key. The generated
file contains the private key and must stay out of source control.

For the local walkthrough, load the assignments in each terminal:

```sh
set -a
. ./backend-keys.env
set +a
```

Build the portable backend examples against the production library:

```sh
cmake -S . -B build-production -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF
cmake --build build-production --target example_backend_server example_backend_client backend_check_token
```

Then run three programs in three terminals: the server, a backend, the client.
The backend examples link the selected `rocketnet` build, so `ROCKETNET_DEV=OFF`
uses the production library. These two examples share the non-secret `RNBE`
title packet filter, distinct from the SDK development filter. To use another
filter, set matching `ROCKETNET_SERVER_PACKET_FILTER` and
`ROCKETNET_CLIENT_PACKET_FILTER` base64 values. Other examples that mint development tokens
continue to use the development library.

1. The server. It makes its own keypair when it starts, and prints the address
   and public key the backend needs, also writing them to `backend.env`:

```sh
./bin/example_backend_server
```

```
ROCKETNET_BACKEND_SERVER_ADDRESSES=127.0.0.1:40000
ROCKETNET_BACKEND_SERVER_PUBLIC_KEY=c3b7jgh0ehn5akuHjGQzU/pS2hUOnHsiTp6w2vTTfQ0=
```

2. A backend, with those two variables. Go:

```sh
cd examples/backend/go
env $(cat ../../../backend.env) go run .
```

or C#:

```sh
cd examples/backend/csharp
env $(cat ../../../backend.env) dotnet run --project Backend
```

Both listen on port 8080.

3. The client, with a token from the backend:

```sh
./bin/example_backend_client --token "$(curl -s http://localhost:8080/match/42)"
```

The client connects, sends one message, receives the echo and disconnects; the
server prints the message with the client id the token carried (`2a`), and
exits once its client has gone. A token lives 30 seconds, so fetch it just
before connecting.

To make a request to the backend:

```sh
CLIENT_ID=42 && curl http://localhost:8080/match/${CLIENT_ID}
```

`--mint <client id>` prints one token and exits, instead of serving.

## Configuration

Everything comes from the environment, so no key is ever in the code or the
repository.

| Variable | Meaning | Default |
| --- | --- | --- |
| `ROCKETNET_BACKEND_PRIVATE_KEY` | The backend's 64-byte Ed25519 secret key, base64, as either backend's `--keygen` command prints it. Give each server the matching public key (`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`). | the published development key, **development only** |
| `ROCKETNET_BACKEND_SERVER_PUBLIC_KEY` | The server's 32-byte public key, base64 (`rocketnet_server_public_key`). | none: required |
| `ROCKETNET_BACKEND_SERVER_ADDRESSES` | The server's address, `a.b.c.d:port`, `[ipv6]:port`, or one of each separated by a comma. | `127.0.0.1:40000` |
| `ROCKETNET_BACKEND_PROTOCOL_ID` | The game's protocol id, `0x` hex or decimal; the server's must match. | `0x52434b544e455431`, the examples' id |
| `ROCKETNET_BACKEND_PORT` | The HTTP port. | `8080` |
| `ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS` | How long a token lives after it is made. | `30` |
| `ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS` | How long a connection may hear nothing before it times out. | `10` |

With no `ROCKETNET_BACKEND_PRIVATE_KEY` the backend uses the published
development backend key and says so when it starts. Both halves of that key are
printed in the standard, so ANYONE can mint tokens a server using it accepts.
It is for local runs only, and a release library refuses it at server start.
To deploy, use either backend's `--keygen` command (or `./bin/keygen`), give
the backend the private key and every server the public key, and keep the
private key out of source control.

## Unit and functional tests

The unit tests use no sockets and no real time; the token's create time is an
argument. They mint every token in the C library's own known answers,
[tests/connect_token_vectors.json](../../tests/connect_token_vectors.json), from
the vectors' fixed inputs, and check each one byte for byte; they also check the
address encoding against the vectors, open a freshly minted token the way a
server does, and check every configuration refusal:

```sh
cd examples/backend/go && go test ./...
cd examples/backend/csharp && dotnet run --project Tests
```

The functional test builds both backends and runs them in turn. Each backend's
own `--keygen` command generates fresh keys. The harness passes the public key
to the server and the private key to that backend through the environment.
The C library first checks a minted token (`backend_check_token`: signature,
sealed block, inner fields and client pre-send checks). Then the backend
serves a token over HTTP and a portable client connects to the portable
server, sends a message, receives its echo and disconnects. `--production`
requires both endpoints to report production mode and portable transport;
a development build cannot pass that functional test. Private keys are not
written to the functional-test receipt.

```sh
python3 tools/run_backend_examples.py --build build-production --production
```

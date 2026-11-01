# Keys

A RocketNet deployment has two long-lived keys and one short-lived secret:

| Key | Algorithm | Made by | Held by |
| --- | --- | --- | --- |
| Backend keypair | Ed25519 | `keygen`, once per title | the secret on the backend only; the public key on every server |
| Server keypair | X25519 | Generated at create, or derived from the application's supplied `config.server_secret_key` | the server object; the public key is registered with the backend |
| Connect token | (a token, not a key) | the backend, once per client connection | the client |

The backend signs every connect token with its Ed25519 secret, and the server
checks the signature against the backend public key: that is how the server
knows the backend made the token. The backend also seals the token's private
block to the server's X25519 public key, so only that server can open it. The
wire layout of the token is in [STANDARD.md](STANDARD.md).

No key value appears in this file. Never commit one.

## Generating the backend keypair

`keygen` is built by default from a checkout, under `ROCKETNET_BUILD_TOOLS`
(on when RocketNet is the top-level project; `CMakeLists.txt`). From the
repository root:

```sh
make -j4
umask 077
./bin/keygen > backend-keys.txt
```

or with CMake alone:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_BUILD_TOOLS=ON
cmake --build build --target keygen
./bin/keygen
```

`keygen` calls `rocketnet_generate_backend_keypair` (public in
[sdk/rocketnet.h](../sdk/rocketnet.h); libsodium's `crypto_sign_keypair`
underneath) and prints a fresh keypair twice: as base64, and as C
byte arrays.

```
backend public key base64 = <32 bytes, base64>
backend private key base64 = <64 bytes, base64>
const uint8_t backend_public_key[] = { <32 bytes> };
const uint8_t backend_private_key[] = { <64 bytes> };
```

The private key is the 64-byte Ed25519 signing secret. Its output is a secret:
write it to a file only you can read, as above, and keep that file out of
source control. Both example backends print the same keypair as environment
assignments with `--keygen` ([examples/backend/README.md](../examples/backend/README.md)).

## Where each key lives

**The backend secret key lives on the backend and nowhere else.** No server
and no client needs it. The example backends read it from
`ROCKETNET_BACKEND_PRIVATE_KEY`, base64. Anyone holding it can mint tokens
every server of the title accepts.

**The backend public key lives in every server's config.** Set
`backend_public_key` in `struct rocketnet_server_config_t`, or set
`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` to the base64 of its 32 bytes; the
environment is read once, at create ([docs/ENVIRONMENT.md](ENVIRONMENT.md)).
`rocketnet_server_default_config` leaves the field empty, and
`rocketnet_server_start` refuses an all-zero key with
`ROCKETNET_STATUS_MISSING_KEYS`. The public key is not a secret, but it decides
which tokens the server accepts, so it comes from the deployment, not from the
client.

**Each server object owns an X25519 keypair.** `rocketnet_server_create`
generates it when the config's `server_secret_key` is all zeros, the default.
A backend that seals tokens to a fixed server public key, or separate processes
that must agree on one, supply the secret key instead in `server_secret_key`
(`ROCKETNET_SERVER_SECRET_KEY_BYTES`, 32): create copies it and derives the
public key. It has no environment variable. Read the public key with
`rocketnet_server_public_key` (`ROCKETNET_SERVER_PUBLIC_KEY_BYTES`, 32), while
running or stopped. Wipe the caller's copy of a supplied secret once create
returns; the server's copy never leaves the server object, and
`rocketnet_server_destroy` wipes it. A production build refuses the published
development server key at start with `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`.
After `rocketnet_server_start`, read the served addresses with
`rocketnet_server_ipv4_address` and `rocketnet_server_ipv6_address`, and
register the addresses and the public key with the backend over an
authenticated channel. The backend issues tokens for that key and those
addresses. RocketNet does not provide the registration channel: it is the
title's. The example backends take it as `PUT /servers/{serverID}`, with no
authentication, and say so.

**The client holds no long-lived signing or server secret key.** It holds a
connect token, an opaque base64 string
of at most `ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES`, which it passes to
`rocketnet_client_connect`. The token carries the two token keys for that one
connection, which the backend draws fresh for each token; they are not
long-lived and they identify nothing outside that connection. Fetch a token
just before connecting: a token expires. 30 seconds is the recommended
lifetime and the `expire_seconds` that `rocketnet_connect_token_default_config`
fills in; a development token lives for the `expire_seconds` passed to
`rocketnet_create_dev_connect_token`.

## Development keys and production keys

A default build is a development build (`ROCKETNET_DEV=1`). It provides
`ROCKETNET_DEV_BACKEND_PUBLIC_KEY`, the published development backend public
key, and `rocketnet_create_dev_connect_token`, which mints a token signed with
the matching published development secret. Together they let a server and a
client connect with no backend: initialize `backend_public_key` from
`ROCKETNET_DEV_BACKEND_PUBLIC_KEY`, start the server, then pass its addresses
and `rocketnet_server_public_key` to `rocketnet_create_dev_connect_token`. A
server started with a development key logs a warning on every start.

All four published development keys are public macros in a development
build, so a backend under test (in C, or in another language that copies the
bytes) needs no internal header:

| Macro | Bytes | What it is |
|---|---|---|
| `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` | `ROCKETNET_BACKEND_PUBLIC_KEY_BYTES` (32) | the server config's `backend_public_key` |
| `ROCKETNET_DEV_BACKEND_SECRET_KEY` | `ROCKETNET_BACKEND_SECRET_KEY_BYTES` (64) | the token config's `backend_secret_key`; its last 32 bytes are the public key |
| `ROCKETNET_DEV_SERVER_PUBLIC_KEY` | `ROCKETNET_SERVER_PUBLIC_KEY_BYTES` (32) | the token config's `server_public_key` for a server given the secret below |
| `ROCKETNET_DEV_SERVER_SECRET_KEY` | `ROCKETNET_SERVER_SECRET_KEY_BYTES` (32) | the server config's `server_secret_key` |

With the development server pair a backend can seal tokens before the server
starts, as with a production fixed server key.

The development backend key is published in the standard: its public half is
`ROCKETNET_DEV_BACKEND_PUBLIC_KEY` ([standard/TRANSPORT.md, t19.2](../standard/TRANSPORT.md#t19.2)),
so anyone can mint a token a server accepts. A server that accepts the
development backend key accepts tokens from anyone.

A production build (`ROCKETNET_DEV=0`, CMake `-DROCKETNET_DEV=OFF`;
[BUILDING.md, "Production builds"](BUILDING.md#production-builds)):

- compiles out the four `ROCKETNET_DEV_*_KEY` macros and
  `rocketnet_create_dev_connect_token`;
- refuses the published development backend or server key at start, with
  `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`;
- refuses at connect a token sealed to the published development server key,
  with `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION`.

Ship only a production build, with a backend keypair from `keygen`.

## Writing production tokens

The backend writes production tokens. The public header carries the SDK's
own production minter, legal in a production build (`ROCKETNET_DEV=0`):

- `rocketnet_generate_backend_keypair( public_key, secret_key )` makes an
  Ed25519 backend keypair (`ROCKETNET_BACKEND_PUBLIC_KEY_BYTES`, 32, and
  `ROCKETNET_BACKEND_SECRET_KEY_BYTES`, 64).
- `rocketnet_generate_server_keypair( public_key, secret_key )` makes an
  X25519 keypair of the kind a server object makes for itself
  (`ROCKETNET_SERVER_PUBLIC_KEY_BYTES` and `ROCKETNET_SERVER_SECRET_KEY_BYTES`,
  32 each).
- `rocketnet_create_connect_token( &config, connect_token )` writes the token
  string from a `struct rocketnet_connect_token_config_t`, initialized with
  `rocketnet_connect_token_default_config`: the backend secret key, the server
  addresses (IPv4 and/or IPv6 strings, as the development call takes them),
  the server public key, the client id, the protocol id, the user data,
  `expire_seconds`, `timeout_seconds` and `unix_timestamp`, the creation time.
  It reads no clock and allocates nothing; it refuses a bad argument with the
  existing statuses and leaves the output unchanged, and a production build
  refuses the published development keys with
  `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`.

Use them on a title's backend written in C or C++, in build/debug tools, or
for in-process minting with ephemeral keys in singleplayer and listen servers.
**Never compile a real backend secret key into a shipped client**: anyone
who extracts it mints tokens every server of the title accepts. A shipped
client receives its token from the backend.

Under the public call is the internal binary writer,
`rocketnet_write_production_connect_token` in
[sdk/rocketnet_connect_token.h](../sdk/rocketnet_connect_token.h), which the SDK's
tests and tools also use. It draws the two token keys, seals the private block
to the server key, signs the signed connect data with the backend key, and
wipes its copy of the private fields. `rocketnet_create_dev_connect_token` is
the same call over the published development backend key.

[examples/backend](../examples/backend/README.md) has two backends that implement the same
token format: Go ([go/main.go](../examples/backend/go/main.go),
`generateConnectToken`) and C#
([csharp/Backend/ConnectToken.cs](../examples/backend/csharp/Backend/ConnectToken.cs),
`ConnectToken.Generate`). Their fixed-input writers are checked byte for byte
against the known answers in
[standard/vectors/connect_token_v1.json](../standard/vectors/connect_token_v1.json). That file
is written only by `./bin/connect_token_vectors standard/vectors/connect_token_v1.json`
([tools/connect_token_vectors/main.cpp](../tools/connect_token_vectors/main.cpp)),
from the fixed test keys and inputs; the unit suite compiles the committed copy
into `./bin/test` and checks it byte for byte against the same generator
(`test_vectors_regenerate_byte_identical`). A real backend authenticates the
player before it writes them a token.

The backend must also rate-limit token issuance per account. A server bounds
half-open handshakes per token and per source address, not per client id: its
encryption mapping table holds `2 * max_clients` mappings, and each token buys
at most one half-open mapping. One account that is issued tokens freely and
presents each from its own source address (an IPv6 prefix supplies many) can
fill that table, and the server refuses everyone else's handshakes until those
mappings reach their handshake deadline. Issue an account only the tokens a
player needs to connect and retry.

## Rotation

What the code supports today:

- **The backend keypair.** A server holds one backend public key, resolved at
  create; start uses that value and never reads it again. To rotate, generate
  a new keypair with `keygen`, give the backend the new secret, and create
  each server again with the new public key (stop, poll
  `rocketnet_server_stopped`, destroy, create, start). Once a server has the
  new key, a token signed by the old key fails the signature check and the
  server ignores the connection request
  (`ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY`, in
  `sdk/rocketnet_counters.h`).
- **Server keys.** A server object keeps its keypair across stop and start,
  and wipes its copy at destroy. With the default all-zero `server_secret_key`,
  each new object generates a new key; register its public key with the backend
  before issuing tokens. Supplying the same secret at create retains that key
  across object replacements. To rotate a supplied key, create the replacement
  with a new secret (or use the generated-key default), then update registration.
  A token naming and sealed to the old key is refused at the server public-key
  check by a server using the new key, which ignores the request
  (`ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY`).
  Short token lifetimes bound how long a token for a replaced key is out.

What it does not support: a server cannot accept two backend public keys at
once, there is no grace window for tokens signed by the previous key, and the
backend key cannot be changed on a server object after create. A server's own
keypair is fixed at create: supply `server_secret_key` then, or leave it zero
to generate a keypair. Replacing that key requires creating a new server object.

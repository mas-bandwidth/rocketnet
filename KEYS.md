# Keys

A RocketNet deployment has two long-lived keys and one short-lived secret:

| Key | Algorithm | Made by | Held by |
| --- | --- | --- | --- |
| Backend keypair | Ed25519 | `keygen`, once per title | the secret on the backend only; the public key on every server |
| Server keypair | X25519 | `rocketnet_server_create`, once per server object | the server object; the public key is registered with the backend |
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

`keygen` calls `rocketnet_generate_backend_keypair` (libsodium's
`crypto_sign_keypair`) and prints a fresh keypair twice: as base64, and as C
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
assignments with `--keygen` ([examples/backend/README.md](examples/backend/README.md)).

## Where each key lives

**The backend secret key lives on the backend and nowhere else.** No server
and no client needs it. The example backends read it from
`ROCKETNET_BACKEND_PRIVATE_KEY`, base64. Anyone holding it can mint tokens
every server of the title accepts.

**The backend public key lives in every server's config.** Set
`backend_public_key` in `struct rocketnet_server_config_t`, or set
`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` to the base64 of its 32 bytes; the
environment is read once, at create ([docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)).
`rocketnet_server_default_config` leaves the field empty, and
`rocketnet_server_start` refuses an all-zero key with
`ROCKETNET_STATUS_MISSING_KEYS`. The public key is not a secret, but it decides
which tokens the server accepts, so it comes from the deployment, not from the
client.

**Each server object makes its own X25519 keypair.** `rocketnet_server_create`
generates it; there is no config field or environment variable that supplies
it. Read the public key with `rocketnet_server_public_key`
(`ROCKETNET_SERVER_PUBLIC_KEY_BYTES`, 32), while running or stopped. The secret
key never leaves the server object, and `rocketnet_server_destroy` wipes it.
After `rocketnet_server_start`, read the served addresses with
`rocketnet_server_ipv4_address` and `rocketnet_server_ipv6_address`, and
register the addresses and the public key with the backend over an
authenticated channel. The backend issues tokens for that key and those
addresses. RocketNet does not provide the registration channel: it is the
title's. The example backends take it as `PUT /servers/{serverID}`, with no
authentication, and say so.

**The client holds no key.** It holds a connect token, an opaque base64 string
of at most `ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES`, which it passes to
`rocketnet_client_connect`. The token carries the two token keys for that one
connection, which the backend draws fresh for each token; they are not
long-lived and they identify nothing outside that connection. Fetch a token
just before connecting: a token expires (30 seconds is the recommended
lifetime, `ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS`).

## Development keys and production keys

A default build is a development build (`ROCKETNET_DEV=1`). It provides
`ROCKETNET_DEV_BACKEND_PUBLIC_KEY`, the published development backend public
key, and `rocketnet_create_dev_connect_token`, which mints a token signed with
the matching published development secret. Together they let a server and a
client connect with no backend: initialize `backend_public_key` from
`ROCKETNET_DEV_BACKEND_PUBLIC_KEY`, start the server, then pass its addresses
and `rocketnet_server_public_key` to `rocketnet_create_dev_connect_token`. A
server started with a development key logs a warning on every start.

Both secret halves of the development keys are printed in the standard. A
server that accepts the development backend key accepts tokens from anyone.

A production build (`ROCKETNET_DEV=0`, CMake `-DROCKETNET_DEV=OFF`;
[BUILDING.md, "Production builds"](BUILDING.md#production-builds)):

- compiles out `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and
  `rocketnet_create_dev_connect_token`;
- refuses the published development backend key at start, with
  `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`;
- refuses at connect a token sealed to the published development server key,
  with `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION`.

Ship only a production build, with a backend keypair from `keygen`.

## Writing production tokens

The backend writes production tokens. The SDK's own writer is
`rocketnet_write_production_connect_token` in
[sdk/rocketnet_connect_token.h](sdk/rocketnet_connect_token.h). It is internal
and not exported: the SDK's unit tests use it, and a backend written in C or
C++ can take it as the reference. It takes the backend secret key, the
server public key, the protocol id, one IPv4 and one IPv6 server address
(either or both), the client id, the create and expire timestamps, the timeout
and the user data. It draws the two token keys, seals the private block to the
server key, signs the signed connect data with the backend key, and wipes its
copy of the private fields.

[examples/backend](examples/backend/README.md) has two backends that write the
same token byte for byte: Go ([go/main.go](examples/backend/go/main.go),
`generateConnectToken`) and C#
([csharp/Backend/ConnectToken.cs](examples/backend/csharp/Backend/ConnectToken.cs),
`ConnectToken.Generate`). Both are checked against the known answers in
[tests/connect_token_vectors.json](tests/connect_token_vectors.json). That file
is written only by `./bin/connect_token_vectors tests/connect_token_vectors.json`
([tools/connect_token_vectors/main.cpp](tools/connect_token_vectors/main.cpp)),
from the fixed test keys and inputs; the unit suite compiles the committed copy
into `./bin/test` and checks it byte for byte against the same generator
(`test_vectors_regenerate_byte_identical`). A real backend authenticates the
player before it writes them a token.

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
  and loses it at destroy. Each new server object has a new key, which is
  registered with the backend in place of the old one. A token sealed to the
  old key cannot be opened by the new server, which ignores the request
  (`ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK`).
  Short token lifetimes bound how long a token for a replaced key is out.

What it does not support: a server cannot accept two backend public keys at
once, there is no grace window for tokens signed by the previous key, and the
backend key cannot be changed on a server object after create. A server's own
keypair cannot be supplied or replaced except by creating a new server object.

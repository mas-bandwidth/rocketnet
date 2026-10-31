# Production integration

This guide replaces the development-token shortcut with a backend that issues
tokens for a running game server. Complete [Getting started](GETTING-STARTED.md)
first. For key ownership and rotation, use [Keys](KEYS.md); for operational
limits, use [qualification](TESTING.md) and [memory planning](MEMORY.md).

The Go and C# examples are token services. They are **not production services
as shipped**: their HTTP registration and token endpoints do not authenticate
callers. Put server registration and player matching behind your own
authentication and authorization, use a protected channel, and do not expose
the example service to untrusted clients. RocketNet does not supply that
service or a C# game-client API.

Before deployment:

1. Link a production library built with `ROCKETNET_DEV=0`.
2. Keep the backend signing secret on the backend; give game servers its public key.
3. Supply your own nonzero protocol id and matching title packet filter.
4. Register each live server's actual addresses and public key before issuing tokens.
5. Authenticate each player, and rate-limit token issuance per account
   ([Keys, "Writing production tokens"](KEYS.md#writing-production-tokens)).
6. Fetch fresh tokens for clients, handle both immediate refusals and asynchronous
   handshake outcomes, and wait for disconnect completion before reconnecting.
7. Qualify the chosen transport, client count, workload, and host resources.

The commands below run from the repository root in a POSIX shell. Windows
builds add `--config Release`; use `bin\Release\` for executables and the
equivalent environment-setting syntax in your shell.

## 1. Build the production library and backend examples (`ROCKETNET_DEV=0`)

Configure a production build with `-DROCKETNET_DEV=OFF` and build the portable
backend server, client, `keygen` utility, and the development-server-key token
checker used by the functional harness:

```sh
cmake -S . -B build-production -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF
cmake --build build-production --target example_backend_server example_backend_client backend_check_token keygen
```

In a `ROCKETNET_DEV=0` build:

- `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and `rocketnet_create_dev_connect_token`
  are compiled out of `sdk/rocketnet.h` and the library.
- `rocketnet_server_start` refuses an all-zero `backend_public_key` with
  `ROCKETNET_STATUS_MISSING_KEYS` (`-10`) and refuses the published development
  backend or server key with `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION` (`-12`).
- `rocketnet_client_create` and `rocketnet_server_create` refuse the published
  development packet filter with
  `ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION` (`-29`).
  `example_backend_server` and `example_backend_client` use the non-secret
  4-byte title packet filter `RNBE` (`EXAMPLE_BACKEND_PACKET_FILTER`, overridable
  via matching base64 `ROCKETNET_SERVER_PACKET_FILTER` and
  `ROCKETNET_CLIENT_PACKET_FILTER`).
- `rocketnet_client_connect` refuses any token sealed to the published
  development server key with
  `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` (`-30`).

## 2. Provision the backend signing keypair and keep key boundaries

Generate a fresh Ed25519 backend signing keypair with the Go backend, the C#
backend, or `./bin/keygen`:

```sh
umask 077
(cd examples/backend/go && go run . --keygen) > backend-keys.env
# Or with C#:
# (cd examples/backend/csharp && dotnet build Backend && dotnet run --no-build --project Backend -- --keygen) > backend-keys.env
# Or with the C SDK keygen tool, which prints labelled lines
# (`backend public key base64 = ...` / `backend private key base64 = ...`),
# not environment assignments, so convert its output:
# ./bin/keygen > backend-keys.txt
# printf 'ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=%s\n' "$(sed -n 's/^backend public key base64 = //p' backend-keys.txt)" > backend-keys.env
# printf 'ROCKETNET_BACKEND_PRIVATE_KEY=%s\n' "$(sed -n 's/^backend private key base64 = //p' backend-keys.txt)" >> backend-keys.env
```

The Go and C# commands each print two base64 environment assignments; the C
`keygen` tool prints the same material under the labels shown above:

- `ROCKETNET_BACKEND_PRIVATE_KEY` (64-byte Ed25519 secret signing key): stays
  **strictly on the backend token service**. Never ship it with game clients,
  never pass it to game servers, and never commit `backend-keys.env` or log its
  value. The long-running Go and C# services retain this key in their
  configuration so they can sign successive tokens; protect the service process
  and its environment, and restart it when rotating the key. The Go and C#
  backend `--keygen` modes zero their temporary generated private-key buffers
  after printing, and each token mint zeroes its temporary per-token session-key
  buffers.
- `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` (32-byte Ed25519 public verification
  key): given to every game server via `config.backend_public_key` or the
  `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` environment variable (read once at
  `rocketnet_server_create`).

## 3. Start and register a live production server

By default, `rocketnet_server_create` generates an X25519 keypair. This
example uses that default. A deployment that needs a stable server identity
can instead supply `config.server_secret_key` at create; it has no environment
variable. Wipe the caller's copy after create. The server keeps its key across
stop/start, and destroy wipes its copy. See [Keys](KEYS.md#where-each-key-lives). After `rocketnet_server_start` succeeds, the server
exports its bound UDP address and 32-byte X25519 public key. The complete
[backend server example](../examples/backend_server.c) checks create and start
separately, calls `rocketnet_server_ipv4_address` and
`rocketnet_server_public_key`, and writes the public registration fields to
`backend.env`. Its shutdown helper waits for completion before destroying the
server. Use reachable advertised addresses in a deployment; a loopback address
in this local example is reachable only from the same host.

The create/start failure branch below is an excerpt, with `config`, `address`,
and the example shutdown helper already defined. From
[examples/backend_server.c](../examples/backend_server.c), lines 101 to 120:

```c
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &status );
    if ( server && status == ROCKETNET_STATUS_OK )
    {
        status = rocketnet_server_start( server );
    }

    if ( !server || status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( status ) );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }
```

Start the production server in Terminal 1 with only the public verification key,
and start the Go or C# backend in Terminal 2 with the private signing key:

```sh
# Terminal 1: production server
export ROCKETNET_SERVER_BACKEND_PUBLIC_KEY="$(grep '^ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=' backend-keys.env | cut -d= -f2-)"
./bin/example_backend_server
```

```sh
# Terminal 2: Go or C# backend (port 8080)
export ROCKETNET_BACKEND_PRIVATE_KEY="$(grep '^ROCKETNET_BACKEND_PRIVATE_KEY=' backend-keys.env | cut -d= -f2-)"
(cd examples/backend/go && go run .)
# Or: (cd examples/backend/csharp && dotnet run --project Backend)
```

In Terminal 3, register the live server with `PUT /servers/{serverID}`:

```sh
. ./backend.env
curl -i -X PUT http://localhost:8080/servers/local \
  -H 'Content-Type: application/json' \
  --data "{\"addresses\":[\"${ROCKETNET_BACKEND_SERVER_ADDRESSES}\"],\"public_key\":\"${ROCKETNET_BACKEND_SERVER_PUBLIC_KEY}\"}"
# HTTP/1.1 204 No Content
```

**Title-owned seam — server registration authentication:** The example backend
accepts unauthenticated `PUT /servers/{serverID}` requests on `localhost` (up to
4096 bytes) for local testing. In production, authenticate server registration
on a trusted control plane (for example, mTLS or fleet credentials) so
untrusted callers cannot add or overwrite servers in the registry.

## 4. Authenticate a player, mint a token, and connect the C client

**Title-owned seam — player authentication and server selection:** In the
example backends, `GET /match/{clientID}` accepts an unauthenticated decimal
64-bit `clientID` in the URL path and selects a registered server uniformly at
random (`registry.random()` / `registry.Random()`). In production, your match
endpoint authenticates the player's platform or account session first, derives
the authoritative `uint64_t` `client_id` on the backend, and selects a server by
region, available capacity, or match assignment before minting the token.

The minted 768-byte connect token (`ROCKETNET_CONNECT_TOKEN_BYTES`, encoded as
1024 base64 characters) binds:

- `protocol_id`: 64-bit game protocol ID (`0x52434b544e455431` default via
  `ROCKETNET_BACKEND_PROTOCOL_ID`), which must be nonzero and match
  `server_config.protocol_id`;
- `create_timestamp` and `expire_timestamp`: 64-bit Unix timestamps in seconds
  (default 30-second lifetime via `ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS`);
- `timeout_seconds`: 32-bit handshake and keepalive timeout in seconds (default
  `10` via `ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS`);
- `addresses` and `address_flags`: 1 or 2 literal UDP server endpoints (`IPv4`,
  `IPv6`, or one of each);
- `server_public_key`: the selected server's 32-byte X25519 public key, to which
  the 432-byte `sealed_private_block` (`client_id`, inner `protocol_id` and
  `expire_timestamp`, `client_to_server_key`, `server_to_client_key`, and
  256-byte `user_data`) is sealed via `crypto_box_seal`, with the 576-byte
  `signed_connect_data` signed by `ROCKETNET_BACKEND_PRIVATE_KEY`.

Pass the opaque base64 token to `rocketnet_client_connect`. A return of
`ROCKETNET_STATUS_OK` starts the handshake; it does not mean connected. The
[backend client example](../examples/backend_client.c) sets the matching title
packet filter, checks create and connect separately, then drives updates until
`rocketnet_client_state` reports `ROCKETNET_CLIENT_STATE_CONNECTED` or a
terminal state. It drains received messages and waits for disconnect completion
before destroying the client.

Request a fresh token and connect the production C client (or run the automated
end-to-end verification across both Go and C# backends):

```sh
./bin/example_backend_client --token "$(curl -sSf http://localhost:8080/match/42)"
python3 tools/run_backend_examples.py --build build-production --production
```

## 5. Handle token and handshake refusals safely

Treat each stage separately:

- HTTP failures come from registration or matching. An empty registry returns
  503; malformed registration or client IDs are rejected. Correct the request
  or fleet state before retrying.
- A negative status from `rocketnet_client_connect` refuses the attempt
  immediately. Check the specific status: invalid or expired token, production
  development-key guard, port zero, pending shutdown, or transport failure.
- A successful connect can still end in a negative client state during updates.
  Timeouts alone do not identify the cause: address, filter, key, protocol,
  token age, load, and network loss can all prevent the handshake.

On a terminal state, request disconnect and poll
`rocketnet_client_disconnected` from the event loop (updating step-driven
clients) until it returns true. Then fetch a fresh token just before reconnect,
or destroy the client if exiting. Do not spin on an expired token or on a
worker that is still stopping.

The [backend reference](../examples/backend/README.md#failure-and-refusal-flows-status-codes-and-safe-recovery-actions)
lists exact HTTP refusals and handshake stages. For general configuration,
transport, messaging, and shutdown recovery, use
[Troubleshooting](ERRORS-AND-LOGGING.md).

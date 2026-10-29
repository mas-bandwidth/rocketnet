# Production integration

This guide replaces the development-token shortcut with a backend that issues
tokens for a running game server. Complete [Getting started](GETTING-STARTED.md)
first. For key ownership and rotation, use [Keys](../KEYS.md); for operational
limits, use [qualification](../TESTING.md) and [memory planning](MEMORY.md).

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
5. Fetch fresh tokens for clients, handle both immediate refusals and asynchronous
   handshake outcomes, and wait for disconnect completion before reconnecting.
6. Qualify the chosen transport, client count, workload, and host resources.

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
stop/start, and destroy wipes its copy. See [Keys](../KEYS.md#where-each-key-lives). After `rocketnet_server_start` succeeds, the server
exports its bound UDP address and 32-byte X25519 public key
(`rocketnet_server_public_key`) so the backend can seal tokens to it. From
[examples/backend_server.c](../examples/backend_server.c), lines 101 to 120 and
127 to 163:

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

```c
    char served_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char server_public_key_base64[4 * ( ( ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 2 ) / 3 ) + 1];
    if ( !rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    rocketnet_server_public_key( server, server_public_key );
    write_base64( server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES, server_public_key_base64 );

    FILE * env = fopen( ENV_FILE, "w" );
    if ( !env )
    {
        printf( "FAIL: could not write %s\n", ENV_FILE );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    fprintf( env, "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\nROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", served_address, server_public_key_base64 );
    fclose( env );
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
on a trusted control plane (mTLS, fleet credentials, or private network) so
untrusted callers cannot add or overwrite servers in the registry.

## 4. Authenticate a player, mint a token, and connect the C client

**Title-owned seam — player authentication and server selection:** In the
example backends, `GET /match/{clientID}` accepts an unauthenticated decimal
64-bit `clientID` in the URL path and selects a registered server uniformly at
random (`registry.random()` / `registry.Random()`). In production, your match
endpoint authenticates the player's platform or account session first, derives
the authoritative `uint64_t` `client_id` on the backend, and selects a server by
region, capacity (such as the demonstrated 1000-player `ROCKETNET_MAX_CLIENTS`
server configuration under its stated workload and deployment conditions), or
match assignment before minting the token.

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

Pass the opaque base64 token to `rocketnet_client_connect`. From
[examples/backend_client.c](../examples/backend_client.c), lines 50 to 103:

```c
    const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = EXAMPLE_BACKEND_PACKET_FILTER;
    struct rocketnet_client_config_t config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    config.transport = "portable";
    memcpy( config.packet_filter, packet_filter, sizeof( packet_filter ) );
    config.state_change_callback = state_changed;

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    if ( !client || create_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create: %s\n", rocketnet_status_name( create_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    // connect checks the token first: a string that does not parse, or one
    // already expired, is refused here

    const enum rocketnet_status_t status = rocketnet_client_connect( client, argv[2] );
    if ( status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect refused: %s\n", rocketnet_status_name( status ) );
        rn_cli_client_stop_and_destroy( client, 1000.0 );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    printf( "backend_client: mode=%s transport=%s\n", ROCKETNET_DEV ? "development" : "production", rocketnet_client_transport_name( client ) );
```

Request a fresh token and connect the production C client (or run the automated
end-to-end verification across both Go and C# backends):

```sh
./bin/example_backend_client --token "$(curl -sSf http://localhost:8080/match/42)"
python3 tools/run_backend_examples.py --build build-production --production
```

## 5. Handle token and handshake refusals safely

- **Backend HTTP refusals:**
  - `GET /match/{clientID}` returns `HTTP 503 Service Unavailable` when no
    servers are registered; register a live server via `PUT /servers/{serverID}`
    (`HTTP 204 No Content`) before matching.
  - `GET /match/{clientID}` returns `HTTP 400 Bad Request` if `clientID` is not
    an unsigned 64-bit decimal integer.
  - `PUT /servers/{serverID}` returns `HTTP 400 Bad Request` (or `HTTP 413` in
    C# when the body exceeds 4096 bytes) if `serverID` is empty or over the
    limit (Go: `> 128` bytes; C#: `> 128` characters), JSON is malformed, `addresses` is empty, has `> 2` entries, repeats
    an IP family, uses a hostname instead of a literal IP, uses port `0`, or
    `public_key` is not 32-byte base64.
  - For unknown JSON fields, the Go backend returns `HTTP 400 Bad Request`; the
    C# backend currently accepts and ignores them. Send only the documented
    `addresses` and `public_key` fields, and do not rely on that C# behavior.
- **Synchronous pre-send refusals at `rocketnet_client_connect(client, token)`:**
  - `NULL` pointer -> `ROCKETNET_STATUS_INVALID_ARGUMENT` (`-6`).
  - Empty string, length `!= 1024`, invalid base64, wrong `wire_version`, bad
    `address_flags`, `timeout_seconds <= 0`, or `create_timestamp > expire_timestamp`
    -> `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN` (`-15`) (setting
    `ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN` = `-6` when the 768-byte
    token decoded but failed field validation).
  - Token already expired (`expire_timestamp <= current_timestamp`) ->
    `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED` (`-16`) and state
    `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` (`-7`).
  - Token sealed to the published development server key in a `ROCKETNET_DEV=0`
    build -> `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` (`-30`).
  - Token naming port `0` -> `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO` (`-31`).
  - **Safe action:** Do not enter the connected loop; request a fresh token from
    the backend before calling `rocketnet_client_connect` again.
- **Asynchronous handshake refusals (`rocketnet_client_state(client) < 0` during `rocketnet_client_update`):**
  - Wrong `protocol_id`, wrong or stale `server_public_key` (e.g., server object was
    recreated with a new key without re-registering), unadvertised server address, or token
    expired on arrival -> server ignores the request before the cookie gate;
    client ends in `ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT` (`-2`) or
    `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` (`-7`).
  - Wrong backend signing key (`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` mismatch),
    corrupted sealed private block, token already used by another client
    address, or `client_id` already connected -> server ignores the post-cookie
    connection request; client ends in
    `ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT` (`-3`) or
    `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` (`-7`).
  - All `max_clients` server slots full -> server sends a sealed denial; client
    ends in `ROCKETNET_CLIENT_STATE_CONNECTION_DENIED` (`-1`).
  - **Safe action:** Call `rocketnet_client_disconnect(client)`, poll
    `rocketnet_client_disconnected(client)` until `true`, and request a
    **fresh** connect token from the backend before reconnecting (or call
    `rocketnet_client_destroy(client)`). Never reuse an expired or consumed
    connect token.

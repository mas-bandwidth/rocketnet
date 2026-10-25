# Getting started

This guide takes a game from a fresh checkout to a dedicated server and a
client exchanging messages. It is the path through the documentation, in the
order an integration needs it, and it links to the reference for depth:

- [BUILDING.md](../BUILDING.md) for building, installing and linking;
- [KEYS.md](../KEYS.md) for keys and connect tokens;
- [USAGE.md](../USAGE.md) for the frame loop, sending, receiving, receipts and
  the connection lifecycle;
- [sdk/rocketnet.h](../sdk/rocketnet.h), the authority for every name and
  contract.

Every code block below is copied from an example under
[examples/](../examples/README.md), with its file and lines named. The
examples are checked by `python3 tools/check_examples.py` and built and run by
`python3 tools/run_examples.py`, so the code here is code that runs.

## 1. Build and link

Build a checkout with `make` from the repository root, then run the unit suite
with `./bin/test`. [BUILDING.md](../BUILDING.md) lists the prerequisites for
Linux, Windows and macOS, the build targets and the test tiers.

A default build is a development build (`ROCKETNET_DEV=1`): it accepts the
published development keys and mints development connect tokens, which is
what this guide uses. A game ships a production build (`-DROCKETNET_DEV=OFF`),
which refuses all of that. To link the library into a game, install it and use
the CMake package, as
[BUILDING.md, "Installing and embedding the SDK"](../BUILDING.md#installing-and-embedding-the-sdk)
shows; [BUILDING.md, "Production builds"](../BUILDING.md#production-builds)
lists the production settings. Most examples link the development library,
`rocketnet_dev`, to mint development tokens locally.

## 2. Start a dedicated server

Initialize the library once, fill a server config from its defaults, give it
the protocol id and the backend public key, then create and start the server.
From [examples/client_server_server.c](../examples/client_server_server.c),
lines 38 to 56:

```c
rocketnet_init( NULL, NULL, NULL );

// a deployment's server takes its own backend's public key; this one takes
// the published development key, whose tokens the client mints
const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
struct rocketnet_server_config_t config;
rocketnet_server_default_config( &config );
config.protocol_id = EXAMPLE_PROTOCOL_ID;
memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
config.max_clients = 1;
config.transport = "portable";

struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, NULL );
if ( !server || rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
{
    // the example runner reads this line as a port already taken, and retries on another
    printf( "FAIL: server != NULL\n" );
    return 1;
}
```

Create binds nothing; start binds the transport and accepts clients. A game
checks the status of every call that returns one, as the
[README](../README.md#quick-start) quick start does. The config fields, their
defaults and their environment variables are in
[USAGE.md, "Configuration"](../USAGE.md#configuration).

## 3. Connect a client with a connect token

A client never connects to an address. It connects with a connect token: the
token names the server, carries the keys and is the credential. In a
development build a program can mint its own token from the server's address
and public key. From
[examples/client_server_client.c](../examples/client_server_client.c), lines
76 to 84:

```c
char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
rocketnet_create_dev_connect_token( address, NULL, server_public_key, 30, 5, 0x0123456789abcdefull, protocol_id, (uint64_t) time( NULL ), NULL, connect_token );

struct rocketnet_client_config_t config;
rocketnet_client_default_config( &config );
config.transport = "portable";
config.state_change_callback = state_changed;
struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL );
rocketnet_client_connect( client, connect_token );
```

The server reads its address and public key with
`rocketnet_server_ipv4_address` and `rocketnet_server_public_key`
([examples/client_server_server.c](../examples/client_server_server.c), lines
60 to 63); the example passes them to the client process in a file that holds
no secret.

Production tokens come from the game's backend, never from the client. The
backend authenticates the player, then writes a token signed with the backend
secret key and sealed to the chosen server's public key, and hands it to the
client. [KEYS.md](../KEYS.md) covers generating the backend keypair with
`keygen`, where each key lives, and
[writing production tokens](../KEYS.md#writing-production-tokens);
[examples/backend](../examples/backend/README.md) has a backend in Go and one
in C# that write them.

## 4. Run the frame loop

Each endpoint runs the same five steps once per frame, in this order:

1. update with the game's time;
2. receive until the call returns `NULL`;
3. simulate;
4. queue this frame's messages with start message and finish message;
5. send packets, last.

The server's frame, from
[examples/client_server_server.c](../examples/client_server_server.c), lines
79 to 96. It has one receive queue across every client, and each message names
the client that sent it:

```c
rocketnet_server_update( server, 1000.0 + rocketnet_time() - start );

// one queue across every client, each message naming its sender: echo each one back
const struct rocketnet_message_batch_t * batch;
while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
{
    for ( int i = 0; i < batch->num_messages; i++ )
    {
        printf( "server received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) batch->client_index[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] );
        if ( !p )
            continue;
        memcpy( p, batch->data[i], (size_t) batch->bytes[i] );
        echoed |= rocketnet_server_finish_message( server, batch->bytes[i] ) != 0;
    }
}

rocketnet_server_send_packets( server );
```

The client's frame, from
[examples/client_server_client.c](../examples/client_server_client.c), lines
90 to 107:

```c
rocketnet_client_update( client, 1000.0 + rocketnet_time() - start );

const struct rocketnet_message_batch_t * batch;
while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
{
    for ( int i = 0; i < batch->num_messages; i++, received++ )
        printf( "client received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
}

if ( !sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
{
    const char message[] = "hello from the client";
    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) );
    memcpy( p, message, strlen( message ) );
    sent = rocketnet_client_finish_message( client, (int) strlen( message ) ) != 0;
}

rocketnet_client_send_packets( client );
```

What each step requires:

- **Update** takes the game's time in seconds. Use one monotonic clock for
  every call that takes a time, finite and never decreasing; update reads no
  clock of its own (connect token expiry reads the wall clock). These examples pass elapsed time from
  `1000.0`; [examples/game_loop.c](../examples/game_loop.c) runs a 60 Hz fixed
  step and passes `1000.0 + frame * FRAME_DT` (line 143).
- **Receive until `NULL`.** One update can deliver more than one batch, and
  every message must be taken before the next update: a message left in the
  queue ends that connection with `MESSAGE_NOT_RECEIVED`, because its sender
  was already told it arrived. A `data` pointer is valid until the next receive
  or update on that endpoint; process or copy it first.
- **Start message, write in place, finish message.** Start returns a pointer to
  write the message into, or `NULL` when the send is refused or the connection
  is not CONNECTED; check it, as the server loop does. Finish returns a nonzero
  serial, or `0` when the send was refused. One start is open per object at a
  time.
- **Send packets last.** Sending before this frame's messages are queued
  delays them a frame. Never assume a send has left the machine or a message
  has arrived when the call returns: receipts report the outcome later, and
  extra frames under load are possible in both directions.

[examples/game_loop.c](../examples/game_loop.c) is the whole loop at scale: a
server and up to 1000 clients over `loopback`, the steps numbered (1) to (5)
on lines 152 to 170, and `--wrong-order` to show the frame of latency an early
send costs. [USAGE.md, "Frame loop"](../USAGE.md#frame-loop),
["Sending messages"](../USAGE.md#sending-messages),
["Receiving messages"](../USAGE.md#receiving-messages) and
["Receipts"](../USAGE.md#receipts) are the reference.

## 5. Choose reliability modes and channels

Each message names a reliability mode and a channel when it is started. A
common split:

- state the next message replaces, such as a position, goes
  `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`: it may be lost, and an older
  one never arrives after a newer one on its channel;
- events that must all arrive in order, such as chat or a game event, go
  `ROCKETNET_RELIABILITY_RELIABLE_ORDERED`;
- a send whose outcome the game wants reported uses a `_WITH_ACK_RECEIPT`
  mode and reads the receipts.

The channel is the ordering scope. Ordered and sequenced modes hold order only
within their channel, so a lost chat message holds back later chat on its
channel and never the positions on another. From
[examples/channels.c](../examples/channels.c), lines 103 to 115:

```c
if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
{
    position_sent++;
    uint8_t * p = rocketnet_client_start_message( client, POSITION_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, sizeof( position_sent ) );
    memcpy( p, &position_sent, sizeof( position_sent ) );
    rocketnet_client_finish_message( client, sizeof( position_sent ) );
    if ( chat_sent < NUM_CHAT )
    {
        p = rocketnet_client_start_message( client, CHAT_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
        p[0] = ++chat_sent;
        rocketnet_client_finish_message( client, 1 );
    }
}
```

`num_channels` must be the same on the client and the server. The eight modes
and their guarantees are in [docs/RELIABILITY.md](RELIABILITY.md) and
[USAGE.md, "Sending messages"](../USAGE.md#sending-messages); channels and
receipts in [docs/CHANNELS.md](CHANNELS.md). The examples
[reliability](../examples/reliability.c),
[ack_receipts](../examples/ack_receipts.c) and
[big_messages](../examples/big_messages.c) show each mode, receipts, and
messages larger than one packet.

## 6. Shut down

Shutdown is three steps: request it, poll for completion, then destroy.
Disconnect and stop never wait for the library's network threads, so a game
keeps its frame loop running and polls once per frame. A client that is still
being updated by the game keeps calling update while it polls. The examples
wait in a loop at exit, with a failure bound; from
[examples/shutdown.h](../examples/shutdown.h), which every example includes,
lines 12 to 27 and 43 to 53:

```c
static inline void rn_cli_client_wait_disconnected( struct rocketnet_client_t * client )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_client_disconnected( client ) )
    {
        if ( rocketnet_time() >= deadline )
        {
            fprintf( stderr, "client shutdown did not complete within 10 seconds\n" );
            abort();
        }
        // Needed only by an explicitly step-driven client. During shutdown,
        // update progresses cleanup without reading or changing game time.
        rocketnet_client_update( client, 1000.0 );
        rocketnet_sleep( 0.001 );
    }
}
```

```c
static inline void rn_cli_client_stop_and_destroy( struct rocketnet_client_t * client )
{
    if ( !client )
        return;
    if ( client )
    {
        rocketnet_client_disconnect( client );
        rn_cli_client_wait_disconnected( client );
    }
    rocketnet_client_destroy( client );
}
```

The server is the same with `rocketnet_server_stop`,
`rocketnet_server_stopped` and `rocketnet_server_destroy` (lines 29 to 41 and
55 to 65). Destroy every client and server, then call `rocketnet_term`. Keep
each object, its allocator and callback contexts alive until its destroy
returns. [USAGE.md, "Connection lifecycle"](../USAGE.md#connection-lifecycle)
has the per-frame form a game uses and the states a connection ends in.

## 7. Threads

The contract, stated in full in [USAGE.md, "Threads"](../USAGE.md#threads):

- **The caller owns each client and server object, one thread at a time.**
  Every call on one object is made from one thread at a time; the objects are
  not thread-safe and take no lock for the caller.
- **The caller may move an object between threads** with its own handoff, for
  example a mutex, a queue or a thread join, so that the last call on the old
  thread happens before the first call on the new one. The library adds no
  handoff of its own.
- **The library's own network threads never call into the game** except
  through the documented callbacks: the allocator functions passed to
  `rocketnet_init` and the log function, which can run on a network thread and
  must be thread-safe, and the assert function, which runs on the thread that
  fires the assert. `state_change_callback` runs on the caller's thread, during
  update, disconnect and destroy.

The library creates its network threads at client connect and server start.
They do the socket I/O and bounded background work, and hand their results to
the caller's update; the connection's protocol state advances in update, on
the caller's thread. Disconnect and stop request that those threads end, the
completion queries report when they have, and destroy joins them, which is
why destroy comes after completion.

## 8. Move from development tokens to a production backend (`ROCKETNET_DEV=0`)

Sections 2 through 4 started with the development build (`ROCKETNET_DEV=1`),
where `examples/client_server_server.c` uses `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`,
`examples/client_server_client.c` calls `rocketnet_create_dev_connect_token`,
and the server loop guards `rocketnet_server_start_message` (`if ( !p ) continue;`)
before writing payload bytes and calling `rocketnet_server_finish_message`.
(Every caller of `rocketnet_client_start_message` and
`rocketnet_server_start_message` in production code must check `p != NULL`
before writing to `p`.)

Before deploying a game, switch the C SDK build to `ROCKETNET_DEV=0`
(`-DROCKETNET_DEV=OFF`) and mint connect tokens on a backend service using
[examples/backend](../examples/backend/README.md) (available in Go at
`examples/backend/go` and C# at `examples/backend/csharp`; **the C# project is a
backend token service only — RocketNet has no C# client API**).

### Step 1: Build the production library and backend examples (`ROCKETNET_DEV=0`)

Configure a production build with `-DROCKETNET_DEV=OFF` and build the portable
backend server, client, token checker, and `keygen` utility:

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

### Step 2: Provision the backend signing keypair and keep key boundaries

Generate a fresh Ed25519 backend signing keypair with the Go backend, the C#
backend, or `./build-production/bin/keygen`:

```sh
umask 077
(cd examples/backend/go && go run . --keygen) > backend-keys.env
# Or with C#:
# (cd examples/backend/csharp && dotnet build Backend && dotnet run --no-build --project Backend -- --keygen) > backend-keys.env
# Or with the C SDK keygen tool:
# ./build-production/bin/keygen > backend-keys.env
```

Each command prints two base64 environment assignments:

- `ROCKETNET_BACKEND_PRIVATE_KEY` (64-byte Ed25519 secret signing key): stays
  **strictly on the backend token service**. Never ship it with game clients,
  never pass it to game servers, and never commit `backend-keys.env` or log its
  value. Both the Go and C# backends zero the secret key buffer and per-token
  session keys in memory after signing.
- `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` (32-byte Ed25519 public verification
  key): given to every game server via `config.backend_public_key` or the
  `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` environment variable (read once at
  `rocketnet_server_create`).

### Step 3: Start and register a live production server

Each server instance generates its own ephemeral X25519 keypair inside
`rocketnet_server_create` (`rocketnet_generate_server_keypair`); the 32-byte
secret key never leaves the server object and is wiped by
`rocketnet_server_destroy`. After `rocketnet_server_start` succeeds, the server
exports its bound UDP address and 32-byte X25519 public key
(`rocketnet_server_public_key`) so the backend can seal tokens to it. From
[examples/backend_server.c](../examples/backend_server.c), lines 75 to 84 and
88 to 96:

```c
enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &status );
if ( server )
    status = rocketnet_server_start( server );
if ( status != ROCKETNET_STATUS_OK )
{
    printf( "FAIL: server start: %s\n", rocketnet_status_name( status ) );
    return 1;
}
printf( "backend_server: mode=%s transport=%s\n", ROCKETNET_DEV ? "development" : "production", rocketnet_server_transport_name( server ) );
```

```c
char served_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
char server_public_key_base64[4 * ( ( ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 2 ) / 3 ) + 1];
rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) );
rocketnet_server_public_key( server, server_public_key );
write_base64( server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES, server_public_key_base64 );
FILE * env = fopen( ENV_FILE, "w" );
fprintf( env, "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\nROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", served_address, server_public_key_base64 );
fclose( env );
```

Start the production server in Terminal 1 with only the public verification key,
and start the Go or C# backend in Terminal 2 with the private signing key:

```sh
# Terminal 1: production server
export ROCKETNET_SERVER_BACKEND_PUBLIC_KEY="$(grep '^ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=' backend-keys.env | cut -d= -f2-)"
./build-production/bin/example_backend_server
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

### Step 4: Authenticate a player, mint a token, and connect the C client

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
[examples/backend_client.c](../examples/backend_client.c), lines 44 to 60:

```c
const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = EXAMPLE_BACKEND_PACKET_FILTER;
struct rocketnet_client_config_t config;
rocketnet_client_default_config( &config );
config.transport = "portable";
memcpy( config.packet_filter, packet_filter, sizeof( packet_filter ) );
config.state_change_callback = state_changed;
struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL );

// connect checks the token first: a string that does not parse, or one
// already expired, is refused here
const enum rocketnet_status_t status = rocketnet_client_connect( client, argv[2] );
if ( status != ROCKETNET_STATUS_OK )
{
    printf( "FAIL: connect refused: %s\n", rocketnet_status_name( status ) );
    return 1;
}
printf( "backend_client: mode=%s transport=%s\n", ROCKETNET_DEV ? "development" : "production", rocketnet_client_transport_name( client ) );
```

Request a fresh token and connect the production C client (or run the automated
end-to-end verification across both Go and C# backends):

```sh
./build-production/bin/example_backend_client --token "$(curl -sSf http://localhost:8080/match/42)"
python3 tools/run_backend_examples.py --build build-production --production
```

### Step 5: Handle token and handshake refusals safely

- **Backend HTTP refusals:**
  - `GET /match/{clientID}` returns `HTTP 503 Service Unavailable` when no
    servers are registered; register a live server via `PUT /servers/{serverID}`
    (`HTTP 204 No Content`) before matching.
  - `GET /match/{clientID}` returns `HTTP 400 Bad Request` if `clientID` is not
    an unsigned 64-bit decimal integer.
  - `PUT /servers/{serverID}` returns `HTTP 400 Bad Request` (or `HTTP 413` in
    C# when the body exceeds 4096 bytes) if `serverID` is empty or `> 128`
    bytes, JSON is malformed or has unknown fields, `addresses` is empty, has
    `> 2` entries, repeats an IP family, uses a hostname instead of a literal
    IP, uses port `0`, or `public_key` is not 32-byte base64.
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
  - Wrong `protocol_id`, wrong or stale `server_public_key` (e.g., server
    restarted without re-registering), unadvertised server address, or token
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

## Where to go next

- [examples/backend/README.md](../examples/backend/README.md): full reference
  for the Go and C# token services, failure tables, and test suites.
- [USAGE.md](../USAGE.md): configuration, stats, send budgets, and every state
  and refusal.
- [examples/README.md](../examples/README.md): one example per feature, and how
  to run them.
- [KEYS.md](../KEYS.md) and [AGENTS.md, "Before production"](../AGENTS.md#before-production):
  what changes before a server is deployed.
- [docs/LINUX.md](LINUX.md), [docs/MACOS.md](MACOS.md) and
  [TRANSPORTS.md](../TRANSPORTS.md): choosing and provisioning server
  transports.

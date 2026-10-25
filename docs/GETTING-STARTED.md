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
        if ( p )
        {
            memcpy( p, batch->data[i], (size_t) batch->bytes[i] );
            echoed |= rocketnet_server_finish_message( server, batch->bytes[i] ) != 0;
        }
    }
}

rocketnet_server_send_packets( server );
```

The client's frame, from
[examples/client_server_client.c](../examples/client_server_client.c), lines
90 to 107, with the canonical non-`NULL` `start_message` guard around
`finish_message`:

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
    if ( p )
    {
        memcpy( p, message, strlen( message ) );
        sent = rocketnet_client_finish_message( client, (int) strlen( message ) ) != 0;
    }
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
- **Start message, check `if ( p )`, write in place, finish message.** Start
  returns a pointer to write the message into, or `NULL` when the send is
  refused or the connection is not CONNECTED; always check `if ( p )` and call
  `rocketnet_client_finish_message` or `rocketnet_server_finish_message` (or
  `abort_message`) only inside that non-`NULL` branch. Calling `finish_message`
  without a successful outstanding `start_message` is an error and returns `0`.
  Finish returns a nonzero serial, or `0` when the send was refused. One start
  is open per object at a time. When sending under load where overflowing
  message memory (`MESSAGE_MEMORY_EXHAUSTED`) must be avoided, call the optional
  preflight `rocketnet_client_can_send_message` or
  `rocketnet_server_can_send_message` first; `start_message` remains the
  authoritative reservation result, and what the game does when
  `can_send_message` returns `false` (skip, defer, drop, or coalesce) is
  application policy rather than library behavior.
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
[examples/channels.c](../examples/channels.c), lines 103 to 115, with the
canonical non-`NULL` `start_message` guards:

```c
if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
{
    position_sent++;
    uint8_t * p = rocketnet_client_start_message( client, POSITION_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, sizeof( position_sent ) );
    if ( p )
    {
        memcpy( p, &position_sent, sizeof( position_sent ) );
        rocketnet_client_finish_message( client, sizeof( position_sent ) );
    }
    if ( chat_sent < NUM_CHAT )
    {
        p = rocketnet_client_start_message( client, CHAT_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
        if ( p )
        {
            p[0] = ++chat_sent;
            rocketnet_client_finish_message( client, 1 );
        }
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

## Where to go next

- [AGENTS.md](../AGENTS.md): complete technical reference for AI coding agents integrating RocketNet.
- [USAGE.md](../USAGE.md): configuration, stats, send budgets, and every state
  and refusal.
- [docs/SESSION.md](SESSION.md): connect tokens, session isolation, and reconnect lifecycle.
- [examples/README.md](../examples/README.md): one example per feature, and how
  to run them ([hello](examples/hello.md), [client_server](examples/client_server.md), [game_loop](examples/game_loop.md), [transports](examples/transports.md)).
- [KEYS.md](../KEYS.md) and [AGENTS.md, "Before production"](../AGENTS.md#before-production):
  what changes before a server is deployed.
- [docs/LINUX.md](LINUX.md), [docs/MACOS.md](MACOS.md) and
  [TRANSPORTS.md](../TRANSPORTS.md): choosing and provisioning server
  transports.

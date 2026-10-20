# Using rocketnet

RocketNet gives a game one client object and one server object that exchange
messages over UDP. A message is an opaque byte buffer: RocketNet moves it,
fragments it when it is large, and reports whether it arrived. It does not
serialize it.

The authority for every name, field, default and environment variable is
[sdk/rocketnet.h](sdk/rocketnet.h). This file is the narrative: how a frame is
ordered, how messages are sent and received, what the counters mean, and how
the configuration system decides what happens at create.

Build and platform instructions live in [BUILDING.md](BUILDING.md); the wire
format lives in [STANDARD.md](STANDARD.md).

## Start and stop

Call `rocketnet_init` once before creating a client or a server, and check its
result. Every call that can fail returns `enum rocketnet_status_t`, and
`ROCKETNET_STATUS_OK` (0) is success; `rocketnet_status_name` names a code. A
second init returns `ROCKETNET_STATUS_ALREADY_INITIALIZED` without disturbing
the live library. A create before init returns NULL, allocates nothing, and writes `ROCKETNET_STATUS_NOT_INITIALIZED` through its status out-parameter. Call
`rocketnet_term` after every client and server is destroyed; termination with
live allocations is refused with `ROCKETNET_STATUS_LIVE_ALLOCATIONS` and
logged, and a repeated termination returns `ROCKETNET_STATUS_NOT_INITIALIZED`. Startup,
shutdown and process-wide logging setup are serialized with application work.

Set custom allocator callbacks before initialization, with no live allocations.
A refused `rocketnet_set_allocator` replacement returns its status; an accepted
one returns `ROCKETNET_STATUS_OK`. Every refused API call logs one line and increments
`rocketnet_api_misuse_count`.

The public API is NULL safe at the boundary. Every entry point checks each
pointer argument it would dereference once, at the top of the call: a NULL is a
no-op that returns the function's documented value (`0`, `NULL` or `false`; a
void function returns nothing) and is counted as misuse. A NULL handle is
refused before any I/O, so no socket opens.

```c
#include "rocketnet.h"

int main( void )
{
    if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    /* create clients and servers here */

    rocketnet_term();
    return 0;
}
```

Server addresses are `"a.b.c.d:port"` or `"[v6]:port"`. A port of `0` asks the
kernel for an ephemeral port; read the granted port with
`rocketnet_server_port` before minting a connect token. `rocketnet_time` is a
monotonic clock in seconds; use the same clock for every call that takes a
time, and keep it finite and non-decreasing.

The connect-token create and expire checks read the wall clock unless the
config sets `game_clock_epoch`, the unix time at the game clock's zero. With an
epoch, the check reads epoch plus the caller's time, floored to whole seconds,
so a test can hold time still and a fixed token still connects. Zero, the
default, keeps the wall clock.

## Configuration

The client and the server each take one plain config struct. Fill it with
`rocketnet_client_default_config` or `rocketnet_server_default_config`, set
what the program means, and pass it to create. The server default has no keys,
and a server create refuses it until keys are supplied, because default keys
are how every deployment of a protocol ends up sharing one identity. For local
work, `rocketnet_server_dev_config` installs the published development keys and
test filter and says so loudly. In a development build,
`rocketnet_write_dev_connect_token` mints a matching local token.

The precedence at create is **DEFAULT < STRUCT < ENV**: the defaults come
first, the struct you pass is the middle layer, and the environment overlays it
field by field. Every override is logged with the variable and the value it
displaced. A field with no environment form is the two callbacks. Byte fields
are base64 of exactly the field's length; integers are decimal; strings are
taken as they are. An environment variable set to the empty string is ignored,
with a log line.

A value that does not parse, or is out of range, **refuses the create with one
error line; nothing is ever clamped**. A secret key logs redacted, on both the
old and the new value. The create, default and validate calls each take the struct size (`config_bytes`) so a program built against an older header keeps working; the same-named convenience macros pass `sizeof(*config)` for a current caller. An unknown size is refused before anything is read or written. Each create takes a final status out-parameter (NULL is accepted): it holds `ROCKETNET_STATUS_OK` with a non-NULL result, or the refusal's code with NULL (`sdk/rocketnet.h:1031`, `sdk/rocketnet.h:1220`; `sdk/rocketnet_api.cpp:529`, `sdk/rocketnet_api.cpp:1228`).

```c
#include "rocketnet.h"

void configure( struct rocketnet_client_config_t * client, struct rocketnet_server_config_t * server )
{
    rocketnet_client_default_config( client );
    client->transport = "portable";
    client->send_kbps = 2000;
    client->num_channels = 8;

    rocketnet_server_default_config( server );
    server->protocol_id = 0x1122334455667788ULL;
    server->max_clients = 64;
    server->transport = "portable";
    server->num_channels = 8;
}
```

`transport` is an ordered candidate list, comma separated, tried left to right
at create. The first candidate that starts wins, and a list that every
candidate refuses fails loudly. `"auto"` (the default, also NULL or the empty
string) is the platform's recommended order; a one-element list is a pin. A
known name this endpoint does not have is skipped with one log line; a name
that is no transport anywhere is a typo and fails loudly. The accessor on each
end reports the transport that won.

Every transport serves IPv4 and IPv6, the AF_XDP server transport (`xdp`)
included: a server serves the family of its public address, and `"auto"`
tries `xdp` first for either family. One interface takes one XDP program, so
on a dual-stack interface `xdp` serves one family and another transport the
other.

`public_address` on the server displaces the address passed to create, so the
address the transport binds and the address connect tokens are checked against
are always the same one. The environment form applies to the first server
create in the process only.

### Environment overrides

Every field except a callback has an environment variable named
`ROCKETNET_CLIENT_<FIELD>` or `ROCKETNET_SERVER_<FIELD>`. The table lists one
row per field of both public structs.

| Field | Variable | Type | Default | A bad value |
|---|---|---|---|---|
| filter_value | ROCKETNET_CLIENT_FILTER_VALUE | uint8_t[4], base64 | `b7 59 01 00` (test filter) | not exactly 4 bytes, or invalid base64: refuses the create |
| max_payload_bytes | ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES | int | `0` (16384) | refuses the create unless `0` or `1416..305408` |
| num_reassembly_slots | ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS | int | `0` (8) | refuses the create unless `0` or `1..256` |
| transport | ROCKETNET_CLIENT_TRANSPORT | const char * | `NULL` (`"auto"`) | an unknown name, or a list with no usable candidate: refuses the create |
| state_change_callback | none | `void (*)( void *, int, int )` | `NULL` | no environment form |
| time_callback | none | `double (*)( void * )` | `NULL` (platform clock) | no environment form |
| game_clock_epoch | ROCKETNET_CLIENT_GAME_CLOCK_EPOCH | uint64_t | `0` (wall clock) | a non-integer: refuses the create |
| send_kbps | ROCKETNET_CLIENT_SEND_KBPS | int | `1000` | a non-integer, or above `10000000`: refuses the create; `0` or less sets the budget to its ceiling, `ROCKETNET_SEND_BUDGET_MAX_KBPS` (10 Gbit/s), NOT RECOMMENDED; each connection's starting rate |
| num_channels | ROCKETNET_CLIENT_NUM_CHANNELS | int | `32` | refuses the create unless `1..32` |
| window_messages | ROCKETNET_CLIENT_WINDOW_MESSAGES | int | `1024` | refuses the create unless `64..1048576` and a power of two |
| receive_ring_bytes | ROCKETNET_CLIENT_RECEIVE_RING_BYTES | int | `262144` | refuses the create unless `4096..1048576` and a multiple of 16 |
| max_message_bytes | ROCKETNET_CLIENT_MAX_MESSAGE_BYTES | int | `262144` | refuses the create unless `1415..1048576` and no larger than `receive_ring_bytes` |
| send_buffer_bytes | ROCKETNET_CLIENT_SEND_BUFFER_BYTES | int | `800000` | refuses the create unless `12288..268435456` |
| send_queue_messages | ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES | int | `1024` | refuses the create unless `16..1048576` |
| packets_in_flight | ROCKETNET_CLIENT_PACKETS_IN_FLIGHT | int | `256` | refuses the create unless `16..65536` |
| max_packets_per_update | ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE | int | `256` | refuses the create unless `1..1024` |
| unreliable_deliveries | ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES | int | `256` | refuses the create unless `1..65536` |
| sequenced_holds | ROCKETNET_CLIENT_SEQUENCED_HOLDS | int | `16` | refuses the create unless `0..4096` |
| receipt_ring | ROCKETNET_CLIENT_RECEIPT_RING | int | `256` | refuses the create unless `16..65536` |
| rto_initial_ms | ROCKETNET_CLIENT_RTO_INITIAL_MS | int | `100` | refuses the create unless `10..60000` |
| rto_min_ms | ROCKETNET_CLIENT_RTO_MIN_MS | int | `10` | refuses the create unless `1..60000` |
| rto_max_ms | ROCKETNET_CLIENT_RTO_MAX_MS | int | `250` | refuses the create unless `10..60000` |
| reliable_timeout_ms | ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS | int | `10000` | refuses the create unless `1000..600000` |
| protocol_id | ROCKETNET_SERVER_PROTOCOL_ID | uint64_t | `0` | a non-decimal value, or one past the type: refuses the create |
| max_clients | ROCKETNET_SERVER_MAX_CLIENTS | int | `1000` | refuses the create unless `1..1000` |
| filter_value | ROCKETNET_SERVER_FILTER_VALUE | uint8_t[4], base64 | `b7 59 01 00` (test filter) | not exactly 4 bytes, or invalid base64: refuses the create |
| backend_public_key | ROCKETNET_SERVER_BACKEND_PUBLIC_KEY | uint8_t[32], base64 | all zero (no key) | not exactly 32 bytes, or invalid base64: refuses the create |
| server_public_key | ROCKETNET_SERVER_SERVER_PUBLIC_KEY | uint8_t[32], base64 | all zero (no key) | not exactly 32 bytes, or invalid base64: refuses the create |
| server_secret_key | ROCKETNET_SERVER_SERVER_SECRET_KEY | uint8_t[32], base64, secret | all zero (no key) | not exactly 32 bytes, or invalid base64: refuses the create; logs redacted |
| num_xdp_queues | ROCKETNET_SERVER_NUM_XDP_QUEUES | int | `0` (transport default) | refuses the create unless `0` or `1..1024` |
| num_os_cpus | ROCKETNET_SERVER_NUM_OS_CPUS | int | `0` (transport default) | refuses the create unless `0` or `1..256` |
| max_payload_bytes | ROCKETNET_SERVER_MAX_PAYLOAD_BYTES | int | `0` (16384) | refuses the create unless `0` or `1416..305408` |
| num_reassembly_slots | ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS | int | `0` (8) | refuses the create unless `0` or `1..256` |
| max_timeout_seconds | ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS | int | `60` | refuses the create unless `1..3600` |
| handshake_deadline_seconds | ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS | int | `5` | refuses the create unless `1..60` |
| game_clock_epoch | ROCKETNET_SERVER_GAME_CLOCK_EPOCH | uint64_t | `0` (wall clock) | a non-integer: refuses the create |
| transport | ROCKETNET_SERVER_TRANSPORT | const char * | `NULL` (`"auto"`) | an unknown name, or a list with no usable candidate: refuses the create |
| public_address | ROCKETNET_SERVER_PUBLIC_ADDRESS | const char * | `NULL` | an address that does not parse: refuses the create |
| per_client_send_kbps | ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS | int | `10000` | a non-integer, or above `10000000`: refuses the create; `0` or less sets the budget to its ceiling, `ROCKETNET_SEND_BUDGET_MAX_KBPS` (10 Gbit/s), NOT RECOMMENDED; each client connection's starting rate |
| num_channels | ROCKETNET_SERVER_NUM_CHANNELS | int | `32` | refuses the create unless `1..32` |
| window_messages | ROCKETNET_SERVER_WINDOW_MESSAGES | int | `1024` | refuses the create unless `64..1048576` and a power of two |
| receive_ring_bytes | ROCKETNET_SERVER_RECEIVE_RING_BYTES | int | `262144` | refuses the create unless `4096..1048576` and a multiple of 16 |
| max_message_bytes | ROCKETNET_SERVER_MAX_MESSAGE_BYTES | int | `262144` | refuses the create unless `1415..1048576` and no larger than `receive_ring_bytes` |
| send_buffer_bytes | ROCKETNET_SERVER_SEND_BUFFER_BYTES | int | `800000` (per client) | refuses the create unless `12288..268435456` |
| send_queue_messages | ROCKETNET_SERVER_SEND_QUEUE_MESSAGES | int | `1024` (per client) | refuses the create unless `16..1048576` |
| packets_in_flight | ROCKETNET_SERVER_PACKETS_IN_FLIGHT | int | `256` (per client) | refuses the create unless `16..65536` |
| max_packets_per_update | ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE | int | `256` (per client) | refuses the create unless `1..1024` |
| unreliable_deliveries | ROCKETNET_SERVER_UNRELIABLE_DELIVERIES | int | `256` (per client per update) | refuses the create unless `1..65536` |
| sequenced_holds | ROCKETNET_SERVER_SEQUENCED_HOLDS | int | `16` | refuses the create unless `0..4096` |
| receipt_ring | ROCKETNET_SERVER_RECEIPT_RING | int | `256` | refuses the create unless `16..65536` |
| rto_initial_ms | ROCKETNET_SERVER_RTO_INITIAL_MS | int | `100` | refuses the create unless `10..60000` |
| rto_min_ms | ROCKETNET_SERVER_RTO_MIN_MS | int | `10` | refuses the create unless `1..60000` |
| rto_max_ms | ROCKETNET_SERVER_RTO_MAX_MS | int | `250` | refuses the create unless `10..60000` |
| reliable_timeout_ms | ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS | int | `10000` | refuses the create unless `1000..600000` |

The message fields marked WIRE must be identical on both ends; the connection
verifies them. The cross rules are in [docs/CHANNELS.md](docs/CHANNELS.md)
section 14.

## Create, connect, start, stop and destroy

`rocketnet_client_create` takes the config and an optional caller memory
block, and makes a client in `DISCONNECTED`. `rocketnet_client_connect` starts
a connection with a connect token: a client connects with a token, never an
address, and the token names the server, carries the keys and is the
credential. The attempt's clock starts at the next update.
`rocketnet_client_disconnect` ends the connection from any state and returns
the client to `DISCONNECTED`, ready to connect again with a new token; connect
anywhere else is refused with `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED`. A
negative state says how the last attempt ended and stays until disconnect.

`rocketnet_server_create` takes the bind address and the config and binds
nothing: the server starts stopped. `rocketnet_server_start` binds the
transport and accepts clients; `rocketnet_server_stop` disconnects every
client, resets every slot and releases the transport, and the server can
start again. Clients are identified by index (`uint64_t`),
`0 .. max_clients - 1`, the slot each connection takes.
`rocketnet_client_required_memory` and `rocketnet_server_required_memory`
report the bytes a caller block needs. Pass NULL and zero to let RocketNet make
the allocation; a block of at least the reported size, aligned to
`ROCKETNET_ALLOCATION_ALIGNMENT`, makes none.

```c
#include "rocketnet.h"

#include <stdio.h>

struct rocketnet_client_t * start_client( const uint8_t * token, const struct rocketnet_client_config_t * config )
{
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, config, NULL, 0, &status );
    if ( !client )
        printf( "create refused: %s\n", rocketnet_status_name( status ) );
    if ( client && rocketnet_client_connect( client, token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        return NULL;
    }
    return client;
}

struct rocketnet_server_t * start_server( const struct rocketnet_server_config_t * config )
{
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "0.0.0.0:40000", config, NULL, 0, &status );
    if ( !server )
        printf( "create refused: %s\n", rocketnet_status_name( status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        return NULL;
    }
    return server;
}

void stop( struct rocketnet_client_t * client, struct rocketnet_server_t * server )
{
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
}
```

`rocketnet_client_validate_config` and `rocketnet_server_validate_config`
return `ROCKETNET_STATUS_OK` when create would accept the struct's own values,
else `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`; the environment is not read. Destroying a client ends its connection. Destroying a server
disconnects every client and frees the server.

## Frame loop

Run the same five steps once per frame, in this order, on the client and on
the server:

1. `rocketnet_client_update` or `rocketnet_server_update` pumps the transport
   and takes in what arrived.
2. Receive and release every received message with the receive and release
   calls.
3. Run the game simulation for the frame.
4. Queue this frame's outgoing messages with start message and finish message.
5. Call `rocketnet_client_send_packets` or `rocketnet_server_send_packets`
   **last**, after all of this frame's messages are queued.

The common mistake is calling `send_packets` early in the frame, or before
queuing (here that is `rocketnet_client_send_packets` or
`rocketnet_server_send_packets`): this frame's messages then wait for the next
frame, adding a frame of latency. Receiving before update does the same:
nothing has been pumped yet, so the messages that arrived wait a frame.
`send_packets` sends the packets this frame's messages are included in.

```c
#include "rocketnet.h"

void client_frame( struct rocketnet_client_t * client, double time )
{
    /* 1. pump the transport and take in what arrived */
    rocketnet_client_update( client, time );

    /* 2. receive and release every delivered message */
    const uint8_t * data;
    int bytes;
    while ( rocketnet_client_receive_message( client, 0, &data, &bytes ) )
        rocketnet_client_release_message( client, data );

    /* 3. run the game simulation for this frame */

    /* 4. queue this frame's outgoing messages */
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 64 );
    if ( out )
        rocketnet_client_finish_message( client, 64 );

    /* 5. last: send now, so this frame's messages leave this frame */
    rocketnet_client_send_packets( client );
}
```

The server runs the same block with `rocketnet_server_update`,
`rocketnet_server_receive_message`, `rocketnet_server_release_message`,
`rocketnet_server_start_message`, `rocketnet_server_finish_message` and
`rocketnet_server_send_packets`, looping the receive step over every client
index. `rocketnet_client_update` and `rocketnet_server_update` return
`ROCKETNET_UPDATE_OK`, or `ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED` when a
received view from the last update was not released.

## Sending messages

The send is start message, write the message once in place, then finish message:
`rocketnet_client_start_message` or `rocketnet_server_start_message` returns a pointer into the
connection's region, and finish message queues the bytes written and returns the
serial, nonzero, or `0` when the send was refused. Abort message gives the room back.
One start message is pending per object at a time. To send one message to every
connected client, loop over the slots with `rocketnet_server_client_connected`
and start message and finish message for each. A destination id is a client index today
(`uint64_t`); ids with `ROCKETNET_FLOW_ID_BIT` set are reserved for flows.

A message is sent only on a connection that is CONNECTED, on the client and on
the server alike: the client while `rocketnet_client_state` is
`ROCKETNET_CLIENT_STATE_CONNECTED`, the server to an index for which
`rocketnet_server_client_connected` is true. A send on any other connection (a
client from create until CONNECTED, or after it disconnects; a server index
below `max_clients` with no CONNECTED client, or a stopped server) is dropped:
start message returns `NULL`, logs nothing and adds 1 to
`ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS` or
`ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS`, and finish message returns `0`.
The drop is normal, never misuse, and nothing is held for a later send. A game
that sends every frame needs no state check: the same `if ( out )` that covers
the other `NULL` returns covers this one, and the first frame after CONNECTED
carries the current state. A server index at or past `max_clients` is misuse,
counted in `rocketnet_api_misuse_count`.

The reliability is one of `ROCKETNET_RELIABILITY_UNRELIABLE`,
`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_RELIABLE`,
`ROCKETNET_RELIABILITY_RELIABLE_ORDERED`, `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`,
`ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`, `ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT`
or `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT`. Plain UNRELIABLE is one
message in one packet, up to `ROCKETNET_MESSAGE_SINGLE_BYTES`; the other types
are bounded by the config's `max_message_bytes`. A larger payload fragments
transparently, and a lost fragment loses the whole payload.

A send the connection's region has no room for ends the connection with
`ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED`: the server disconnects that
client, and the client disconnects itself. The send budget governs the wire,
never the queue. A reliable message waits for budget and is never dropped; an
unreliable message over budget is dropped and counted. A start refused for a
full send ring, arena or lease table spends its session sequence and sends
nothing; this is harmless: no ack block names it, nothing resends, and the
receiver takes every packet after it. The one trace is the client's packet
loss stat, which counts a sequence the server spent this way as a gap.

```c
#include "rocketnet.h"

uint32_t queue_state( struct rocketnet_client_t * client, const uint8_t * state, int bytes )
{
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    if ( !out )
        return 0;

    for ( int i = 0; i < bytes; i++ )
        out[i] = state[i];

    return rocketnet_client_finish_message( client, bytes );
}
```

The config's `send_kbps` and `per_client_send_kbps` set a connection's starting
rate. `rocketnet_client_set_send_kbps` and `rocketnet_server_set_send_kbps`
change a CONNECTED connection's budget at runtime. A value of `0` or less sets
the budget to its ceiling, `ROCKETNET_SEND_BUDGET_MAX_KBPS` (10 Gbit/s), NOT
RECOMMENDED; a positive value up to `ROCKETNET_SEND_BUDGET_MAX_KBPS` caps it.
Each returns `ROCKETNET_STATUS_OK`, or `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`
above the maximum with the rate unchanged. On a connection that is not
CONNECTED (a client in a handshake state, a server slot with no CONNECTED
client, a stopped server) each returns `ROCKETNET_STATUS_INVALID_ARGUMENT` and
changes nothing; that is not misuse.

## Receiving messages

`rocketnet_client_receive_message` and `rocketnet_server_receive_message` hand out the next
message of the update on a channel as a const view and its length. The view is
valid until the next update, and every received message must be released with
`rocketnet_client_release_message` or `rocketnet_server_release_message` before the next
update. Copy anything kept past the update. Updating with a view still held
asserts in a debug build and returns `ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED` in
a release build.

Each call returns `1` with the next message, or `0` when there is none on that
channel for that client.

```c
#include "rocketnet.h"

void server_frame( struct rocketnet_server_t * server, int client_index, int channel, double time )
{
    /* 1. pump the transport and take in what arrived */
    rocketnet_server_update( server, time );

    /* 2. receive and release every delivered message */
    const uint8_t * data;
    int bytes;
    while ( rocketnet_server_receive_message( server, client_index, channel, &data, &bytes ) )
    {
        /* process data and bytes, then release the view */
        rocketnet_server_release_message( server, client_index, data );
    }

    /* 3. run the game simulation for this frame */

    /* 4. queue this frame's outgoing messages */
    const uint8_t payload[] = { 1, 2, 3, 4 };
    uint32_t serial = 0;
    uint8_t * out = rocketnet_server_start_message( server, client_index, channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) sizeof( payload ) );
    if ( out )
    {
        memcpy( out, payload, sizeof( payload ) );
        serial = rocketnet_server_finish_message( server, client_index, (int) sizeof( payload ) );
    }
    if ( serial == 0 )
    {
        /* the send was refused: check the connection */
    }

    /* 5. last: send now, so this frame's messages leave this frame */
    rocketnet_server_send_packets( server );

    struct rocketnet_receipt_t receipts[64];
    int count = rocketnet_server_receipts( server, client_index, receipts, 64 );
    for ( int i = 0; i < count; i++ )
    {
        if ( receipts[i].status == ROCKETNET_RECEIPT_STATUS_LOSS )
        {
            /* the send with this serial was lost */
        }
    }
}
```

## Receipts

Every send returns a serial. `rocketnet_client_receipts` and
`rocketnet_server_receipts` copy this update's outcomes into a caller array, up
to its capacity, and return the copied count. Each entry carries the serial and
`ROCKETNET_RECEIPT_STATUS_ACKED` or `ROCKETNET_RECEIPT_STATUS_LOSS`. Repeated calls see the
same list until the next update. A full receipt ring drops its oldest entry;
the `receipt_ring` config field sets its size.

## Counters and stats

Counters are monotonic over the object's life and start at zero at create.
`rocketnet_client_counter`, `rocketnet_server_counter` and
`rocketnet_server_client_counter` read them by enum. Client and server stats
come from `rocketnet_client_stat`, `rocketnet_server_stat` and
`rocketnet_server_client_stat`: round trip time and jitter in milliseconds,
packet loss as a percentage, and trailing rates. The per-client counters and
stats cover the slot's current connection and go back to zero when a new
connection takes the slot.

Watch jitter and loss when conditions worsen; the rtt figure is the minimum
over the retained history, so it cannot track latency upward until every lower
sample has been evicted. A server-side rtt of zero means no sample has arrived
yet, not a zero-millisecond link.

```c
#include "rocketnet.h"

void report_quality( const struct rocketnet_client_t * client, const struct rocketnet_server_t * server, int client_index )
{
    uint64_t packets_sent = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_PACKETS_SENT );
    uint64_t receipts_lost = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST );
    double rtt_ms = rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_RTT );
    double loss_percent = rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_PACKET_LOSS );

    uint64_t clients_connected = rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED );
    double client_rtt_ms = rocketnet_server_client_stat( server, client_index, ROCKETNET_SERVER_CLIENT_STAT_RTT );

    (void) packets_sent;
    (void) receipts_lost;
    (void) rtt_ms;
    (void) loss_percent;
    (void) clients_connected;
    (void) client_rtt_ms;
}
```

## Connection lifecycle

A client moves from `ROCKETNET_CLIENT_STATE_DISCONNECTED` through the handshake
states to `ROCKETNET_CLIENT_STATE_CONNECTED`. Every negative value is a way the
attempt or the connection ended, and the client stays in it until it is
destroyed. Read the state with `rocketnet_client_state`, its name with
`rocketnet_client_state_name`, and the end reason with
`rocketnet_client_disconnect_reason`. `rocketnet_client_index` reports the
client's slot on the server while connected.

`rocketnet_client_disconnect` ends the connection: the disconnect is sent and
the state becomes DISCONNECTED. On the server,
`rocketnet_server_client_connected` and `rocketnet_server_client_id` identify a
slot and its client, `rocketnet_server_num_connected_clients` counts them, and
`rocketnet_server_disconnect_client` ends one deliberately.
`rocketnet_server_disconnect_reason` reports how a slot's last connection
ended. The reason is an `enum rocketnet_disconnect_reason_t`, named by
`rocketnet_disconnect_reason_name`. A client whose server disconnected it,
stopped, or was destroyed reads `ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED`
once its update accepts the server's disconnect packet, until its next
connection reaches CONNECTED; a deliberate `rocketnet_client_disconnect` and a
connection timeout read `ROCKETNET_DISCONNECT_REASON_NONE`. A NULL client reads as
`ROCKETNET_CLIENT_STATE_INVALID`.

```c
#include "rocketnet.h"

void end_client( struct rocketnet_client_t * client )
{
    rocketnet_client_disconnect( client );
    rocketnet_client_destroy( client );
}

void end_server_client( struct rocketnet_server_t * server, int client_index )
{
    rocketnet_server_disconnect_client( server, client_index );
}
```

Destroy every client and server before `rocketnet_term`. A server destroy
disconnects every client and frees the server; a client destroy ends its
connection and frees the client.

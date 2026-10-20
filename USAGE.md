# Using rocketnet

RocketNet gives a game one client object and one server object that exchange
messages over UDP. A message is an opaque byte buffer: RocketNet moves it,
fragments it when it is large, and reports whether it arrived. It does not
serialize it.

The authority for every name, field, default and environment variable is
[sdk/rocketnet.h](sdk/rocketnet.h). This file is the narrative: how a frame is
ordered, how messages are sent and received, what the counters mean, and how
the configuration system decides what happens at create, connect and start.

Build and platform instructions live in [BUILDING.md](BUILDING.md); the wire
and message formats live in [standard/TRANSPORT.md](standard/TRANSPORT.md) and
[standard/MESSAGES.md](standard/MESSAGES.md).

## Start and stop

Call `rocketnet_init` once before creating a client or a server, and check its
result. Every call that can fail returns `enum rocketnet_status_t`, and
`ROCKETNET_STATUS_OK` (0) is success; `rocketnet_status_name` names a code. A
second init returns `ROCKETNET_STATUS_ALREADY_INITIALIZED` without disturbing
the live library. A create before init returns NULL and allocates nothing. Call
`rocketnet_term` after every client and server is destroyed; termination with
live allocations is refused with `ROCKETNET_STATUS_LIVE_ALLOCATIONS` and
logged, and a repeated termination returns `ROCKETNET_STATUS_NOT_INITIALIZED`.
Startup, shutdown and process-wide logging setup are serialized with
application work.

Set custom allocator callbacks before initialization, with no live allocations.
A refused `rocketnet_set_allocator` replacement returns its status; an accepted
one returns `ROCKETNET_STATUS_OK`. Every refused API call logs one line and
increments `rocketnet_api_misuse_count`.

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

On the client, `step_driven` (`ROCKETNET_CLIENT_STEP_DRIVEN`) controls whether
client socket I/O runs on a background thread (`0`, the default) or directly on
the caller's thread inside `rocketnet_client_update` and
`rocketnet_client_send_packets` (`1`).

## Configuration

The client and the server each take one plain config struct. Fill it with
`rocketnet_client_default_config` or `rocketnet_server_default_config`, set
what the program means, and pass it to create. The server default has no keys,
and `rocketnet_server_start` refuses it with `ROCKETNET_STATUS_MISSING_KEYS`
until keys are supplied, because default keys
are how every deployment of a protocol ends up sharing one identity. For local
work, `rocketnet_server_dev_config` installs the published development keys and
test filter and says so loudly. In a development build,
`rocketnet_write_dev_connect_token` mints a matching local token.

The precedence is **DEFAULT < STRUCT < ENV**: the defaults come
first, the struct you pass is the middle layer, and the environment overlays it
field by field. Every override is logged with the variable and the value it
displaced. A field with no environment form is the two callbacks. Byte fields
are base64 of exactly the field's length; integers are decimal; strings are
taken as they are. An environment variable set to the empty string is ignored,
with a log line.

A value that does not parse, or is out of range, **is refused with one error
line; nothing is ever clamped**. The message fields (`num_channels` through
`reliable_timeout_ms`), the send budget and `max_clients` take their
environment override and are checked at create, which returns NULL on a
refusal. Every other field is read and checked when the transport starts, in
`rocketnet_client_connect` or `rocketnet_server_start`, which return the status
code and leave the client `DISCONNECTED` or the server stopped. A secret key
logs redacted, on both the old and the new value. The `_sized` default,
validate, required-memory and create calls take the struct size the program
was compiled against, so a program built against an older header keeps
working; an unknown size is refused before anything is read or written. The
unsized calls read and write the revision 1 prefix of each struct, which ends
at `reliable_timeout_ms` ([docs/ABI-EVOLUTION.md](docs/ABI-EVOLUTION.md)).

Both `rocketnet_client_config_t` and `rocketnet_server_config_t` also carry four
development-only link-simulator fields (`simulated_loss_percent`,
`simulated_latency_ms`, `simulated_jitter_ms`, `simulated_duplicate_percent`).
They follow `reliable_timeout_ms`, so only the `_sized` calls read them: fill the struct with `rocketnet_client_default_config_sized`
or `rocketnet_server_default_config_sized` and create with
`rocketnet_client_create_sized` or `rocketnet_server_create_sized`, passing
`sizeof` the struct. These apply only in `ROCKETNET_DEVELOPMENT` builds; any
nonzero value in a production build is refused.

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
at client connect and server start. The first candidate that starts wins, and a list that every
candidate refuses fails loudly. `"auto"` (the default, also NULL or the empty
string) is the platform's recommended order; a one-element list is a pin. A
known name this endpoint does not have is skipped with one log line, and a
list whose every name is skipped falls back to the endpoint's `"auto"` order
with a warning; a name that is no transport anywhere is a typo and fails
loudly. `rocketnet_client_transport_name` and `rocketnet_server_transport_name`
report the transport that won.

Every transport serves IPv4 and IPv6, the AF_XDP server transport (`xdp`)
included: a server serves the family of its public address, and `"auto"`
tries `xdp` first for either family. One interface takes one XDP program, so
on a dual-stack interface `xdp` serves one family and another transport the
other.

`public_address` on the server displaces the address passed to create when the
server starts, so the address the transport binds and the address connect
tokens are checked against are always the same one. The environment form
applies to the first server start in the process only.

### Environment overrides

Every field except a callback has an environment variable named
`ROCKETNET_CLIENT_<FIELD>` or `ROCKETNET_SERVER_<FIELD>`. The table lists one
row per field of both public structs.

| Field | Variable | Type | Default | A bad value |
|---|---|---|---|---|
| filter_value | ROCKETNET_CLIENT_FILTER_VALUE | uint8_t[4], base64 | `b7 59 01 00` (test filter) | not exactly 4 bytes, or invalid base64: refused |
| max_payload_bytes | ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES | int | `0` (16384) | refused unless `0` or `1416..305408` |
| num_reassembly_slots | ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS | int | `0` (8) | refused unless `0` or `1..256` |
| transport | ROCKETNET_CLIENT_TRANSPORT | const char * | `NULL` (`"auto"`) | an unknown name: refused; a list whose every candidate fails to start: refused at connect or start |
| state_change_callback | none | `void (*)( void *, int, int )` | `NULL` | no environment form |
| time_callback | none | `double (*)( void * )` | `NULL` (platform clock) | no environment form |
| step_driven | ROCKETNET_CLIENT_STEP_DRIVEN | bool | `0` (I/O thread) | refused unless `0` or `1` and a transport with a non-blocking step |
| send_kbps | ROCKETNET_CLIENT_SEND_KBPS | int | `1000` | a non-integer, or above `10000000`: refused; `0` or less disables the budget in this direction (not recommended) |
| num_channels | ROCKETNET_CLIENT_NUM_CHANNELS | int | `32` | refused unless `1..32` |
| window_messages | ROCKETNET_CLIENT_WINDOW_MESSAGES | int | `1024` | refused unless `64..1048576` and a power of two |
| receive_ring_bytes | ROCKETNET_CLIENT_RECEIVE_RING_BYTES | int | `262144` | refused unless `4096..1048576` and a multiple of 16 |
| max_message_bytes | ROCKETNET_CLIENT_MAX_MESSAGE_BYTES | int | `262144` | refused unless `1415..1048576` and no larger than `receive_ring_bytes` |
| send_buffer_bytes | ROCKETNET_CLIENT_SEND_BUFFER_BYTES | int | `800000` | refused unless `12288..268435456` |
| send_queue_messages | ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES | int | `1024` | refused unless `16..1048576` |
| packets_in_flight | ROCKETNET_CLIENT_PACKETS_IN_FLIGHT | int | `256` | refused unless `16..65536` |
| max_packets_per_update | ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE | int | `256` | refused unless `1..1024` |
| unreliable_deliveries | ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES | int | `256` | refused unless `1..65536` |
| sequenced_holds | ROCKETNET_CLIENT_SEQUENCED_HOLDS | int | `16` | refused unless `0..4096` |
| receipt_ring | ROCKETNET_CLIENT_RECEIPT_RING | int | `256` | refused unless `16..65536` |
| rto_initial_ms | ROCKETNET_CLIENT_RTO_INITIAL_MS | int | `100` | refused unless `10..60000` |
| rto_min_ms | ROCKETNET_CLIENT_RTO_MIN_MS | int | `10` | refused unless `1..60000` |
| rto_max_ms | ROCKETNET_CLIENT_RTO_MAX_MS | int | `250` | refused unless `10..60000` |
| reliable_timeout_ms | ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS | int | `10000` | refused unless `1000..600000` |
| protocol_id | ROCKETNET_SERVER_PROTOCOL_ID | uint64_t | `0` | a non-decimal value, or one past the type: refused |
| max_clients | ROCKETNET_SERVER_MAX_CLIENTS | int | `1000` | refused unless `1..1000` |
| filter_value | ROCKETNET_SERVER_FILTER_VALUE | uint8_t[4], base64 | `b7 59 01 00` (test filter) | not exactly 4 bytes, or invalid base64: refused |
| backend_public_key | ROCKETNET_SERVER_BACKEND_PUBLIC_KEY | uint8_t[32], base64 | all zero (no key) | not exactly 32 bytes, or invalid base64: refused |
| server_public_key | ROCKETNET_SERVER_SERVER_PUBLIC_KEY | uint8_t[32], base64 | all zero (no key) | not exactly 32 bytes, or invalid base64: refused |
| server_secret_key | ROCKETNET_SERVER_SERVER_SECRET_KEY | uint8_t[32], base64, secret | all zero (no key) | not exactly 32 bytes, or invalid base64: refused; logs redacted |
| num_xdp_queues | ROCKETNET_SERVER_NUM_XDP_QUEUES | int | `0` (transport default) | refused unless `0` or `1..1024` |
| num_os_cpus | ROCKETNET_SERVER_NUM_OS_CPUS | int | `0` (transport default) | refused unless `0` or `1..256` |
| max_payload_bytes | ROCKETNET_SERVER_MAX_PAYLOAD_BYTES | int | `0` (16384) | refused unless `0` or `1416..305408` |
| num_reassembly_slots | ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS | int | `0` (8) | refused unless `0` or `1..256` |
| max_timeout_seconds | ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS | int | `60` | refused unless `1..3600` |
| handshake_deadline_seconds | ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS | int | `5` | refused unless `1..60` |
| transport | ROCKETNET_SERVER_TRANSPORT | const char * | `NULL` (`"auto"`) | an unknown name: refused; a list whose every candidate fails to start: refused at connect or start |
| public_address | ROCKETNET_SERVER_PUBLIC_ADDRESS | const char * | `NULL` | an address that does not parse: refused |
| per_client_send_kbps | ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS | int | `10000` | a non-integer, or above `10000000`: refused; `0` or less disables the budget in this direction (not recommended) |
| num_channels | ROCKETNET_SERVER_NUM_CHANNELS | int | `32` | refused unless `1..32` |
| window_messages | ROCKETNET_SERVER_WINDOW_MESSAGES | int | `1024` | refused unless `64..1048576` and a power of two |
| receive_ring_bytes | ROCKETNET_SERVER_RECEIVE_RING_BYTES | int | `262144` | refused unless `4096..1048576` and a multiple of 16 |
| max_message_bytes | ROCKETNET_SERVER_MAX_MESSAGE_BYTES | int | `262144` | refused unless `1415..1048576` and no larger than `receive_ring_bytes` |
| send_buffer_bytes | ROCKETNET_SERVER_SEND_BUFFER_BYTES | int | `800000` (per client) | refused unless `12288..268435456` |
| send_queue_messages | ROCKETNET_SERVER_SEND_QUEUE_MESSAGES | int | `1024` (per client) | refused unless `16..1048576` |
| packets_in_flight | ROCKETNET_SERVER_PACKETS_IN_FLIGHT | int | `256` (per client) | refused unless `16..65536` |
| max_packets_per_update | ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE | int | `256` (per client) | refused unless `1..1024` |
| unreliable_deliveries | ROCKETNET_SERVER_UNRELIABLE_DELIVERIES | int | `256` (per client per update) | refused unless `1..65536` |
| sequenced_holds | ROCKETNET_SERVER_SEQUENCED_HOLDS | int | `16` | refused unless `0..4096` |
| receipt_ring | ROCKETNET_SERVER_RECEIPT_RING | int | `256` | refused unless `16..65536` |
| rto_initial_ms | ROCKETNET_SERVER_RTO_INITIAL_MS | int | `100` | refused unless `10..60000` |
| rto_min_ms | ROCKETNET_SERVER_RTO_MIN_MS | int | `10` | refused unless `1..60000` |
| rto_max_ms | ROCKETNET_SERVER_RTO_MAX_MS | int | `250` | refused unless `10..60000` |
| reliable_timeout_ms | ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS | int | `10000` | refused unless `1000..600000` |

`num_channels`, `window_messages`, `receive_ring_bytes` and
`max_message_bytes` are WIRE fields: they must be identical on both ends, and
the connection verifies them. The ranges above are per field; validation and
create also apply these cross rules, each refused with one error line:

- `max_message_bytes` is no larger than `receive_ring_bytes`.
- `window_messages` is at least the number of 1152-byte fragments a
  `max_message_bytes` message takes.
- `send_buffer_bytes` holds every fragment of one `max_message_bytes` message.
- `rto_min_ms` is at most `rto_max_ms`, and `rto_initial_ms` lies between them.
- `reliable_timeout_ms` is at least four times the larger of 100 ms and
  `rto_initial_ms`.

The message layer's rules are in [standard/MESSAGES.md](standard/MESSAGES.md).

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

struct rocketnet_client_t * start_client( const uint8_t * token, const struct rocketnet_client_config_t * config )
{
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, config, NULL, 0 );
    if ( client && rocketnet_client_connect( client, token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        return NULL;
    }
    return client;
}

struct rocketnet_server_t * start_server( const struct rocketnet_server_config_t * config )
{
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "0.0.0.0:40000", config, NULL, 0 );
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
report whether the struct's own values are accepted; the environment is not
read. Destroying a client from a handshake state or `CONNECTED` sends the
disconnect packets first, as `rocketnet_client_disconnect` does, then frees it.
Destroying a server stops it first when it is running, which disconnects every
client. `rocketnet_server_running` returns `1` while the server is running and
`0` while it is stopped.

## Frame loop

Run the same five steps once per frame, in this order, on the client and on
the server:

1. `rocketnet_client_update` or `rocketnet_server_update` pumps the transport
   and takes in what arrived.
2. Receive and release every received message with the receive message and
   release message calls.
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

The send is start message, write the message once in place, then finish
message: `rocketnet_client_start_message` or `rocketnet_server_start_message`
returns a pointer into the connection's region for the declared bytes, and
`rocketnet_client_finish_message` or `rocketnet_server_finish_message` queues
the bytes written and returns the receipt serial, nonzero, or `0` when the send
was refused. `rocketnet_client_abort_message` or
`rocketnet_server_abort_message` gives the room back instead. One start message
is pending per object at a time. Messages flow only while the client is
`CONNECTED`; in any other state start message returns NULL and finish message
returns `0`, counted. To send one message to every connected client, loop over
the slots with `rocketnet_server_client_connected` and call start message and
finish message for each. A destination id is a client index (`uint64_t`); an
id with `ROCKETNET_FLOW_ID_BIT` set is reserved and refused.

The reliability is one of `ROCKETNET_RELIABILITY_UNRELIABLE`,
`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_RELIABLE`,
`ROCKETNET_RELIABILITY_RELIABLE_ORDERED`, `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`,
`ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`, `ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT`
or `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT`. Unreliable messages fit in a
single packet up to single-packet record capacity (`ROCKETNET_MESSAGE_SINGLE_BYTES`
is 1415 bytes at the wire record boundary: 1411 payload bytes for
`ROCKETNET_RELIABILITY_UNRELIABLE` and `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`, or 1405
bytes on channels `0..31` for `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`). An unreliable
send (`ROCKETNET_RELIABILITY_UNRELIABLE`, `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`, or
`ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`) whose record exceeds single-packet
capacity up to `max_message_bytes` is automatically promoted to its reliable
twin (`ROCKETNET_RELIABILITY_RELIABLE`, `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`, or the reliable
receipt block path with `ROCKETNET_RECEIPT_STATUS_ACKED` only) and fragmented across
blocks; only a send above `max_message_bytes` is refused.

A send the connection's region has no room for ends the connection with
`ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED`: the server disconnects that
client, and the client disconnects itself. The send budget governs the wire,
never the queue. A reliable message waits for budget and is never dropped; an
unreliable message over budget is dropped and counted.

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

`rocketnet_client_set_send_kbps` and `rocketnet_server_set_send_kbps` change a
connection's budget at runtime. A value of `0` or less disables the budget in
that direction, which is not recommended; a positive value up to
`ROCKETNET_SEND_BUDGET_MAX_KBPS` caps it.

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
#include <string.h>
#include "rocketnet.h"

void server_frame( struct rocketnet_server_t * server, uint64_t client_index, int channel, double time )
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

Counters are monotonic over the object's life, across connections and
across starts and stops, and start at zero at create.
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

void report_quality( const struct rocketnet_client_t * client, const struct rocketnet_server_t * server, uint64_t client_index )
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

## Logging

RocketNet logs process-wide across six levels: `ROCKETNET_LOG_LEVEL_NONE`,
`ROCKETNET_LOG_LEVEL_ERROR`, `ROCKETNET_LOG_LEVEL_WARN`,
`ROCKETNET_LOG_LEVEL_INFO`, `ROCKETNET_LOG_LEVEL_DEBUG`, and
`ROCKETNET_LOG_LEVEL_SPAM`. `rocketnet_set_log_level` sets the active threshold,
`rocketnet_get_log_level` reads it, and `rocketnet_log_level_name` returns the
display name for a level. `rocketnet_quiet` suppresses (`true`) or resumes
(`false`) all log output without changing the configured level.

To route logs into an application or FFI sink, register a preformatted callback
(`rocketnet_log_callback_t`) with `rocketnet_set_log_callback`; the most recent
registration wins, and passing `NULL` restores default standard output
logging. See [docs/ERRORS-AND-LOGGING.md](docs/ERRORS-AND-LOGGING.md) for
the callback threading and re-entrancy contract.

## Connection lifecycle

A client moves from `ROCKETNET_CLIENT_STATE_DISCONNECTED` through the handshake
states to `ROCKETNET_CLIENT_STATE_CONNECTED`. A negative value is how the last
attempt or connection ended, and the client stays in it until
`rocketnet_client_disconnect` returns it to `DISCONNECTED`. Read the state with `rocketnet_client_state`, its name with
`rocketnet_client_state_name`, and the end reason with
`rocketnet_client_disconnect_reason`. `rocketnet_client_index` reports the
client's slot on the server while connected.

`rocketnet_client_disconnect` ends the connection: from a handshake state or
`CONNECTED` the disconnect packets are sent first, and the state becomes
`DISCONNECTED`; the client can connect again with a new token. On the server,
`rocketnet_server_client_connected` and `rocketnet_server_client_id` identify a
slot and its client, `rocketnet_server_num_connected_clients` counts them, and
`rocketnet_server_disconnect_client` ends one deliberately, and
`rocketnet_server_disconnect_all_clients` ends every one while the server keeps
running.
`rocketnet_server_disconnect_reason` reports how a slot's last connection
ended. The reason is one of the `ROCKETNET_DISCONNECT_REASON_*` values.

```c
#include "rocketnet.h"

void end_client( struct rocketnet_client_t * client )
{
    rocketnet_client_disconnect( client );
    rocketnet_client_destroy( client );
}

void end_server_client( struct rocketnet_server_t * server, uint64_t client_index )
{
    rocketnet_server_disconnect_client( server, client_index );
}
```

Destroy every client and server before `rocketnet_term`. A server destroy
stops a running server, which disconnects every client, and frees it; a client
destroy sends the disconnect packets from a live state, as disconnect does, and
frees the client. Call `rocketnet_client_disconnect` alone to keep the client
and connect it again with a new token.

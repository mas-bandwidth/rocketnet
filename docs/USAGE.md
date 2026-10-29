<a id="using-rocketnet"></a>

# Integrating RocketNet

RocketNet moves opaque byte messages between a client and server. Your game
defines their serialization and drives the public frame loop. Start with
[Getting started](GETTING-STARTED.md) for a runnable connection.

| Integration task | Section |
| --- | --- |
| Initialize, create, connect, and stop | [Startup](#start-and-stop), [endpoint lifecycle](#create-connect-start-stop-and-destroy) |
| Drive each frame | [Frame loop](#frame-loop), [sending](#sending-messages), [receiving](#receiving-messages) |
| Handle pressure and delivery outcomes | [Backpressure](#asking-whether-there-is-room), [receipts](#receipts) |
| Reconnect or shut down | [Connection lifecycle](#connection-lifecycle) |
| Set options and inspect behavior | [Configuration](#configuration), [stats](#counters-and-stats) |
| Plan memory and capacity | [Memory guide](MEMORY.md) |

The [public header](../sdk/rocketnet.h) owns API names, defaults, and contracts.
[Building](BUILDING.md) owns build and linking instructions; [Keys](KEYS.md)
owns key lifetimes; [Production integration](PRODUCTION.md) connects them.
A production library uses `ROCKETNET_DEV=0`; a Release compiler configuration
alone does not remove development features.

Main frame calls target less than 100 microseconds, but that target is not
qualified at 1000 clients. Create and synchronous destruction have separate
lifecycle costs. Read [qualification](TESTING.md) before treating a design goal
or historical benchmark as a deployment guarantee.

## Threads

This section is the threading contract. The header states it in short:
lookup and time calls are thread-safe, and client and server objects are not.

- **The caller owns each client and server object, one thread at a time.**
  Every call on one object (update, receive, start, finish and abort message,
  send packets, receipts, stats, connect, disconnect, start, stop and destroy)
  is made from one thread at a time, never concurrently. The library takes no
  lock on the caller's behalf.
- **The caller may move an object between threads** with its own handoff, for
  example a mutex, a queue or a thread join, so that the last call on the old
  thread happens before the first call on the new one. The library adds no
  handoff of its own. A received message's `data` pointer stays valid only
  until the next receive or update on that object, whichever thread makes it.
- **The library's own network threads never call into the game except through
  the documented callbacks.** The allocator functions passed to
  `rocketnet_init` can run on a network thread and must be thread-safe. The log
  function installed with `rocketnet_log_function` runs on the thread that
  logs, which can be a network thread, so it must be thread-safe and must not
  call `rocketnet_log_function`. The assert function installed with
  `rocketnet_assert_function` runs on the thread that fires the assert, in
  debug builds only. A client's `state_change_callback` reports each change of
  `rocketnet_client_state` once; it runs on the caller's thread, during
  connect, update, disconnect, destroy and a start message that ends the
  connection, and must not call client functions.
- **Process-wide setup is serialized with application work.** Never race
  `rocketnet_init` with `rocketnet_term`, call `rocketnet_term` while no other
  library call runs, and install the assert function before concurrent
  library calls.

Client connect and server start create the library's network threads. They
do the socket I/O and bounded background work and hand their results to the
caller's update; protocol state advances in update, on the caller's thread.
Disconnect and stop request that those threads end, the completion queries
`rocketnet_client_disconnected` and `rocketnet_server_stopped` report when
they have, and destroy joins them.

## Start and stop

Call `rocketnet_init` once before creating a client or a server, and check its
result. Lifecycle calls such as init, connect, and start return
`enum rocketnet_status_t`; `ROCKETNET_STATUS_OK` (0) is success. Other calls
signal refusal through a handle, boolean, count, or their documented output; `rocketnet_status_name` names a code. A
second init returns `ROCKETNET_STATUS_ALREADY_INITIALIZED` without disturbing
the live library; one that names another allocator also counts as API misuse. A create before init returns NULL, allocates nothing, and writes `ROCKETNET_STATUS_NOT_INITIALIZED` through its status out-parameter. Call
`rocketnet_term` after every client and server is destroyed; termination with
live allocations is refused with `ROCKETNET_STATUS_LIVE_ALLOCATIONS` and
logged, and a repeated termination returns `ROCKETNET_STATUS_NOT_INITIALIZED`. Startup,
shutdown and process-wide logging setup are serialized with application work.

Pass custom allocator functions to `rocketnet_init`. NULL for both is the
system allocator. Passing only one returns `ROCKETNET_STATUS_INVALID_ARGUMENT`. Each API misuse logs one line and increments
the process-wide `api_misuses` count that the client and server stats structs
carry.

Follow each declaration's pointer contract. Optional pointers may be NULL
(for example, a create's status output); required NULL arguments return the
documented refusal. Only the caller errors explicitly listed as API misuse
increment `api_misuses`. Do not infer that every NULL or refused call is misuse.
See [Errors](ERRORS-AND-LOGGING.md).

```c
#include "rocketnet.h"

int main( void )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    /* create clients and servers here */

    return rocketnet_term() == ROCKETNET_STATUS_OK ? 0 : 1;
}
```

Server addresses are `"a.b.c.d:port"` or `"[v6]:port"`. A port of `0` asks the
kernel for an ephemeral port; read the granted port with
`rocketnet_server_port` before minting a connect token. `rocketnet_time` is a
monotonic clock in seconds; use the same clock for every call that takes a
time, and keep it finite and non-decreasing.

The frame and update loops (`rocketnet_client_update`, `rocketnet_server_update`)
take the caller's monotonic time and read no clock of their own on those paths,
so the game owns simulation time and a test can hold it still. Token time is
the caller's too: the token create and expire checks read
`token_epoch_unix_seconds` (config field, or
`ROCKETNET_CLIENT_TOKEN_EPOCH_UNIX_SECONDS` /
`ROCKETNET_SERVER_TOKEN_EPOCH_UNIX_SECONDS`) plus the time passed to update,
so a paused caller clock pauses token expiry. An epoch of 0 reads the host wall
clock once (the client at connect, the server at its first update after start)
as the epoch; see [Environment](ENVIRONMENT.md). Keep the token issuer and
endpoints in the same time domain.

## Frame loop

Run the same five steps once per frame, in this order, on the client and on
the server:

1. `rocketnet_client_update` or `rocketnet_server_update` pumps the transport
   and takes in what arrived.
2. Drain the endpoint's one receive queue with `rocketnet_client_receive_messages`
   or `rocketnet_server_receive_messages`, calling it until it returns `NULL`.
   This is required: see [Receiving messages](#receiving-messages).
3. Run the game simulation for the frame.
4. Queue this frame's outgoing messages with start message and finish message.
5. Call `rocketnet_client_send_packets` or `rocketnet_server_send_packets`
   **last**, after all of this frame's messages are queued.

The common mistake is calling `send_packets` early in the frame, or before
queuing (here that is `rocketnet_client_send_packets` or
`rocketnet_server_send_packets`): this frame's messages then wait for the next
frame, adding a frame of latency. Receiving before update does the same:
nothing has been pumped yet, so the messages that arrived wait a frame.
`send_packets` makes queued packet work available to the transport.

Buffered background I/O adds an ownership handoff around those packets. A send
worker may take a packet bank after the caller's `send_packets`, and a receive
worker may publish a packet bank after the caller's `update` has already taken
the available banks. Extra frames under load are an accepted cost of moving
bulk I/O off the frame thread; unnecessary delay of available data remains a
regression. Both directions have bounded work and no fixed public update-count
guarantee. Keep the five-step order unchanged; neither direction promises
same-update message delivery.

The following frame fragment assumes the application supplies `process_message`
and owns the connected client. It omits startup and shutdown; the
[complete example](INTEGRATION-EXAMPLE.md) includes both.

```c
#include "rocketnet.h"

void client_frame( struct rocketnet_client_t * client, double time )
{
    /* 1. pump the transport and take in what arrived */
    rocketnet_client_update( client, time );

    /* 2. drain the one receive queue */
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        for ( int i = 0; i < batch->num_messages; i++ )
            process_message( batch->channel[i], batch->reliability[i], batch->data[i], batch->bytes[i] );

    /* 3. run the game simulation for this frame */

    /* 4. queue this frame's outgoing messages */
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 64 );
    if ( out )
    {
        for ( int i = 0; i < 64; i++ )
            out[i] = 0; /* replace with this frame's serialized message */
        rocketnet_client_finish_message( client, 64 );
    }

    /* 5. make this frame's messages available to the transport */
    rocketnet_client_send_packets( client );
}
```

The server runs the same block with `rocketnet_server_update`,
`rocketnet_server_receive_messages`,
`rocketnet_server_start_message`, `rocketnet_server_finish_message` and
`rocketnet_server_send_packets`. Each server batch entry names its
`client_index`. Client batch entries use client index 0.

## Sending messages

The send is start message, check that the returned pointer is non-`NULL`, write
the message once in place inside that branch, and call finish message inside the
same branch: `rocketnet_client_start_message` or `rocketnet_server_start_message`
returns a pointer into the connection's region (or `NULL` when dropped or refused),
and finish message queues the bytes written and returns the serial, nonzero, or
`0` when the send was refused. Calling `rocketnet_client_finish_message` or
`rocketnet_server_finish_message` without a successful outstanding start is an
error and returns `0`. Abort message gives the room back.
One start message is pending per object at a time. To send one message to every
connected client, loop over the slots with `rocketnet_server_client_connected`
and start message and finish message for each. Every server per-client call
takes an `int` client index; only `rocketnet_server_start_message` takes a
`uint64_t` destination id, a client index today, with ids that have
`ROCKETNET_FLOW_ID_BIT` set reserved for flows. Server finish message and abort
message take no destination: they act on the one pending start. A finish whose
`bytes_written` is outside 1 to the bytes started returns `0` and aborts the
pending start, so the next start is never blocked.

Finish or abort every start before `rocketnet_server_update`,
`rocketnet_server_send_packets`, or `rocketnet_server_stop` (and the client
calls of the same three). Those calls retire a server packet lease that is
still open, and they are not legal while a fill is open. Finishing that
packet afterwards is caller misuse, not a lease the reset should have kept.
`rocketnet_server_transport_finish_packet` then returns 0 in every build,
adds 1 to the process-wide `api_misuses` count, and logs
`server finish packet refused: the lease was reset by update, send_packets or stop`.
It does not assert, including in a debug build, and it does not seal the
frame. The same context finished again is no longer that retired lease.
A finish that had no start, a second finish, a finish after abort, or a
modified context is a different error: it still asserts in a debug build
and returns 0 in a release build.

A message is sent only on a connection that is CONNECTED, on the client and on
the server alike: the client while `rocketnet_client_state` is
`ROCKETNET_CLIENT_STATE_CONNECTED`, the server to an index for which
`rocketnet_server_client_connected` is true. A send on any other connection (a
client from create until CONNECTED, or after it disconnects; a server index
below `max_clients` with no CONNECTED client, or a stopped server) is dropped:
start message returns `NULL`, logs nothing and adds 1 to the
`messages_dropped_not_connected` field of the client or server stats, and finish message
returns `0`.
The drop is normal, never misuse, and nothing is held for a later send. A game
that sends every frame needs no state check: the same `if ( out )` that covers
the other `NULL` returns covers this one, and the first frame after CONNECTED
carries the current state. A server index at or past `max_clients` is misuse,
counted in the stats' `api_misuses`.

The reliability is one of `ROCKETNET_RELIABILITY_UNRELIABLE`,
`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_RELIABLE`,
`ROCKETNET_RELIABILITY_RELIABLE_ORDERED`, `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`,
`ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`, `ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT`
or `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT`. Plain UNRELIABLE is one packet up to `ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES` (1,411 bytes);
above 1,411 bytes it promotes to its reliable twin (`ROCKETNET_RELIABILITY_RELIABLE`),
which fragments across packets transparently up to `max_message_bytes`.
`rocketnet_max_unfragmented_message_bytes( reliability, channel )` returns the
one-packet limit of every mode (the one-record limits of
[standard/MESSAGES.md, m2.1](../standard/MESSAGES.md#m2.1)); it is a pure function,
so size a per-packet game message against it, or against
`ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES` at compile time for UNRELIABLE.

The channel is the ordering channel. RELIABLE_ORDERED,
RELIABLE_ORDERED_WITH_ACK_RECEIPT and the two SEQUENCED types are ordered or
sequenced within it, independently of every other channel, and a channel at or
past the config's `num_channels` refuses them. UNRELIABLE, RELIABLE and the
other two receipt types ignore it, since they are neither ordered nor sequenced:
any channel is accepted for them, never refused, and they travel with no
channel (`rn_message_send_channel` in `sdk/rocketnet_message_server.cpp`,
which `rn_message_object_start` calls to map the channel before reserving
the message).

A send the connection's region has no room for ends the connection: the
server disconnects that client with
`ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED`, and the client ends its
own connection in `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED`. The send budget governs the wire,
never the queue. A reliable message waits for budget and is never dropped; an
unreliable message over budget is dropped and counted.

### Asking whether there is room

`rocketnet_client_can_send_message( client, channel, reliability, max_bytes )`
and `rocketnet_server_can_send_message( server, client_index, channel,
reliability, max_bytes )` provide an optional preflight check answering whether
a start message with the same arguments would be given room now: `true` or
`false`. They use the same decision start message uses, so a `true` answer is a
start that gets its pointer (`start_message` remains the authoritative
reservation result, so always check `if ( out )` and call `finish_message` only
inside that branch). A `false` answer means the start would not: the
connection's message memory is full, or the start would be dropped or refused
(not CONNECTED, a message already open, a bad argument). The query does not
soften the rule above: a start after a `false` still ends the connection with
`MESSAGE_MEMORY_EXHAUSTED`. What the application does when `can_send_message`
returns `false`—skipping a send, deferring it until room returns as the remote
endpoint acknowledges messages, dropping it, or coalescing state—is application
policy rather than library behavior.

The query changes no state, counts nothing, allocates nothing and runs in
constant time. Only start message and update take room, so a `true` answer
holds until the next of either. Each server client slot has its own message
memory; a server index outside `0 .. max_clients - 1` is misuse and answers
`false`. A `false` may be pessimistic for an unreliable message that fits in
one packet, which start can write straight into a free packet without taking
message memory.

```c
#include "rocketnet.h"

bool queue_state_if_room( struct rocketnet_client_t * client, const uint8_t * state, int bytes )
{
    if ( !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
        return false;                       /* application policy: hold/defer rather than exhaust message memory */

    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    if ( out )
    {
        for ( int i = 0; i < bytes; i++ )
            out[i] = state[i];
        return rocketnet_client_finish_message( client, bytes ) != 0;
    }
    return false;
}
```

### Not every refused start spends a sequence number

A client start refused for a full send ring, arena or lease table spends its
session sequence and sends nothing. An aborted client start and a refused client
finish also leave a sequence gap. The server assigns its session sequence only
when finish accepts the payload and reserves every wire frame. A refused server
start or finish and an aborted server start spend no sequence, including a full
lease table or a ring or arena that cannot reserve the output frames. For a
fragmented server payload, the sequence block is reserved after every fragment
frame is ready.

The client's packet loss stat, like the server's per-client packet loss stat,
estimates outbound packet loss from sent packets that remain unacknowledged in
the endpoint's ACK history. Server sequence allocation still keeps accepted
session sequence numbers contiguous; it does not define the client's loss
stat.

```c
#include "rocketnet.h"

uint32_t queue_state( struct rocketnet_client_t * client, const uint8_t * state, int bytes )
{
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    if ( out )
    {
        for ( int i = 0; i < bytes; i++ )
            out[i] = state[i];

        return rocketnet_client_finish_message( client, bytes );
    }
    return 0;
}
```

The config's `send_kbps` and `per_client_send_kbps` set a connection's starting
rate. `rocketnet_client_set_send_kbps` and `rocketnet_server_set_send_kbps`
change a CONNECTED connection's budget at runtime. In the config and in both
calls, `0` is the default rate (`ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS`, 1000, or
`ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS`, 10000), a positive value below
`ROCKETNET_SEND_KBPS_UNLIMITED` caps it, and `ROCKETNET_SEND_KBPS_UNLIMITED`
means no send budget. Each call returns `ROCKETNET_STATUS_OK`, or
`ROCKETNET_STATUS_INVALID_ARGUMENT` for a negative value. A refusal leaves the
rate unchanged. On a connection that is not CONNECTED (a client in a handshake
state, a server slot with no CONNECTED client, a stopped server) each returns
`ROCKETNET_STATUS_OK` and changes nothing. That is not misuse.

## Receiving messages

`rocketnet_client_receive_messages` and `rocketnet_server_receive_messages`
return the next `rocketnet_message_batch_t` from one endpoint-wide queue. A
batch contains up to `ROCKETNET_RECEIVE_BATCH_MESSAGES` (`65536`) entries. Each
entry names `client_index`, `channel`, `reliability`, `bytes`, and `data`; the
client's entries use client index 0. Keep calling until the function returns
`NULL`. A data pointer is valid until the next receive call or the next update
on that endpoint, whichever comes first. Process or copy every entry first;
there is no release call.

The application must take every message before the next update. The transport
has already acknowledged each delivered message to its sender, so a message
left in the queue cannot be kept or delivered again. An update that finds a
message still in the queue treats it as API misuse: it asserts in a debug
build, counts `MESSAGE_NOT_RECEIVED`, and ends that connection. The server
disconnects each client with a message left with
`ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED`, and the client ends its own
connection in `ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED`. Nothing is dropped
silently. Calling receive once per frame is not enough: one update can deliver
more than one batch.

```c
#include "rocketnet.h"

void server_frame( struct rocketnet_server_t * server, int client_index, int channel, double time )
{
    /* 1. pump the transport and take in what arrived */
    rocketnet_server_update( server, time );

    /* 2. drain the one receive queue */
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
            process_message( batch->client_index[i], batch->channel[i],
                             batch->reliability[i], batch->data[i], batch->bytes[i] );
    }

    /* 3. run the game simulation for this frame */

    /* 4. queue this frame's outgoing messages */
    uint32_t serial = 0;
    uint8_t * out = rocketnet_server_start_message( server, (uint64_t) client_index, channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    if ( out )
    {
        /* serialize straight into the started message, then finish with the bytes written */
        uint8_t * p = out;
        *p++ = 1; *p++ = 2; *p++ = 3; *p++ = 4;
        serial = rocketnet_server_finish_message( server, (int) ( p - out ) );
    }
    if ( serial == 0 )
    {
        /* the send was refused: check the connection */
    }

    /* 5. make this frame's messages available to the transport */
    rocketnet_server_send_packets( server );

    struct rocketnet_receipt_t receipts[64];
    int count = rocketnet_server_receipts( server, client_index, receipts, 64 );
    for ( int i = 0; i < count && i < 64; i++ )
    {
        if ( receipts[i].status == ROCKETNET_RECEIPT_STATUS_LOSS )
        {
            /* the send with this serial was lost */
        }
    }
}
```

## Receipts

Every successful finish returns a nonzero serial. `rocketnet_client_receipts` and
`rocketnet_server_receipts` copy this update's outcomes into a caller array, up
to its capacity, and return the available count. Iterate only over the smaller
of the returned count and the array capacity. Pass NULL and zero capacity to
query the count without copying. If the buffer is too small, read again into
a larger buffer before the next update. Each entry carries the serial and
a status. A zero-filled receipt reads `ROCKETNET_RECEIPT_STATUS_NONE`. The library
writes `ROCKETNET_RECEIPT_STATUS_ACKED` or `ROCKETNET_RECEIPT_STATUS_LOSS`. Repeated calls see the
same list until the next update. A full receipt ring drops its oldest entry;
the `max_message_receipts` config field sets its size. The outcomes recorded
for a connection remain readable after that connection ends, until its next
update. A receipt-bearing send, reliable or
`ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`, that is still in flight
when its connection ends gets no terminal outcome.

## Create, connect, start, stop and destroy

`rocketnet_client_create( context, config, &status )` takes the config and a
nullable status out-parameter, and makes a client in `DISCONNECTED`.
It returns a client with `ROCKETNET_STATUS_OK`, or NULL with the specific
refusal status. `rocketnet_client_connect` starts
a connection with a connect token: a client connects with a token, never an
address, and the token names the server, carries the keys and is the
credential. The attempt's clock starts at the next update.
`rocketnet_client_disconnect` ends the connection from any state and returns
the client state to `DISCONNECTED`. This state alone does not prove I/O has
stopped: wait for `rocketnet_client_disconnected` before destroy or reconnect. Connect
accepts `DISCONNECTED` and every negative terminal state. From a terminal state
whose old worker still owns the attempt, it returns
`ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` and retains that state; poll
`rocketnet_client_disconnected` and retry. Positive handshake states and
`CONNECTED` are refused with `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED`.
A connect from a negative state starts a fresh attempt that carries no key,
replay window, sequence, cookie or message state of the earlier one. A negative
state says how the last attempt or connection ended and stays until disconnect
or the next connect.

`rocketnet_server_create( context, ipv4_address, ipv6_address, config, &status )`
takes the IPv4 and IPv6 bind addresses, the config and a nullable status
out-parameter. Either address may be NULL, but not both. It returns a stopped server
with `ROCKETNET_STATUS_OK`, or NULL with the specific refusal status, and binds
nothing. `rocketnet_server_start` binds the
transport and accepts clients; `rocketnet_server_stop` disconnects every
client and requests transport shutdown. Stop is the whole shutdown flush: it
seals each connected client's disconnect packets and hands them to the
transport before telling the transport to stop, so a client reads
`ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED` rather than timing out; there is
no separate disconnect-all or send call to make first. `rocketnet_server_stopped` reports
completion without waiting. Until it is true, start is refused; afterward start
can finalize the old run and activate a new transport. Clients are identified by an `int` index,
`0 .. max_clients - 1`, the slot each connection takes.
Each client and server starts with one block that create allocates through the
allocator. `rocketnet_client_required_memory` and
`rocketnet_server_required_memory` report the peak allocator memory over the
whole lifecycle, including transport allocations at connect and start and the
development link simulator in development builds. For planned built-in
transports with unchanged configuration and environment overrides, a program
can pass `rocketnet_init` an allocator over a region of that size, aligned to
`ROCKETNET_ALLOCATION_ALIGNMENT`. Operating-system-owned memory and a custom
transport's own allocations are outside this bound. A normal client connect starts
one background network thread. A transport with wait/wake support uses that
thread for bounded batched I/O and hands buffers to the frame thread; the
fallback path uses it as a polling receive worker while sends stay on the frame
thread. The client transport memory plan includes the SDK allocations for that
thread, its socket and wake object, but excludes the operating system's thread
stack reserve and kernel socket queues.
Server stats are computed in update: `rocketnet_server_update_stats` runs on
the caller's thread inside the server update (`sdk/rocketnet_server_transport.cpp`)
and publishes per-second numbers from the stats frames. There is no stats
worker thread. After a successful start, stop requests transport shutdown;
`rocketnet_server_stopped` remains false until transport I/O has exited.
Destroy normally follows completion and synchronously joins the transport
worker before freeing its storage; called early, it logs and counts the
misuse, requests shutdown and updates a bounded number of times without
sleeping, then joins and frees once complete, or logs and keeps the object if
the worker has not exited.

## Connection lifecycle

A client moves from `ROCKETNET_CLIENT_STATE_DISCONNECTED` through the handshake
states to `ROCKETNET_CLIENT_STATE_CONNECTED`. Every negative value is a state that ended the attempt or the connection, one per way it can end,
and the client stays in it until `rocketnet_client_disconnect` returns it to
`ROCKETNET_CLIENT_STATE_DISCONNECTED`; `rocketnet_client_disconnected` must
also be true before reconnect or destroy. Read the state with `rocketnet_client_state`, its name with
`rocketnet_client_state_name`; the state itself is how the connection ended.
`rocketnet_client_index` reports the client's slot on the server, an `int`,
while CONNECTED, and `-1` in any other state.

`rocketnet_client_disconnect` ends the connection: the disconnect is sent and
the state becomes DISCONNECTED. Worker completion is separate and is reported
by `rocketnet_client_disconnected`. On the server,
`rocketnet_server_client_connected` and `rocketnet_server_client_id` identify a
slot and its client, `rocketnet_server_client_user_data` copies the
`ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES` of user data the backend put in that
client's connect token, `rocketnet_server_num_connected_clients` counts them, and
`rocketnet_server_disconnect_client` ends one deliberately. The user data is
readable from connect until the slot is released; release wipes it, and a slot
without a connected client reads zeros and returns false.
`rocketnet_server_client_connection_id` names the connection in a slot: a
nonzero id, unique for the server's lifetime across slots, stops and starts,
and 0 when the slot is free. One update can end a slot's connection and admit
another into it, so `rocketnet_server_client_connected` reads true before and
after; compare each slot's connection id after every update to see that change
and reset per-connection state.
`rocketnet_server_disconnect_reason` reports how a slot's last connection
ended. The reason is an `enum rocketnet_disconnect_reason_t`, named by
`rocketnet_disconnect_reason_name`: `ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED`
when the server ended it, `ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED` when
the client's disconnect packet arrived, `ROCKETNET_DISCONNECT_REASON_TIMED_OUT`
when no packet arrived within the connection timeout, and the message-layer
reasons for the ends a client shares with its own state.
A client whose server disconnected it, stopped, or was destroyed reads
`ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED` once its update accepts the
server's disconnect packet; a reliable timeout, a protocol violation, a config
mismatch, exhausted message memory and an update with a message not received read `ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT`,
`ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION`, `ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH`,
`ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED` and `ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED`. Each stays until
disconnect, or until connect ends it to start the next connection. A NULL
client reads as `ROCKETNET_CLIENT_STATE_DISCONNECTED`, counted as misuse.

```c
#include "rocketnet.h"

void end_client( struct rocketnet_client_t * client )
{
    rocketnet_client_disconnect( client );
}

/* Poll once per frame after end_client. */
bool finish_client( struct rocketnet_client_t * client )
{
    if ( !rocketnet_client_disconnected( client ) )
        return false;
    rocketnet_client_destroy( client );
    return true; /* The client pointer is now invalid. */
}

void end_server_client( struct rocketnet_server_t * server, int client_index )
{
    rocketnet_server_disconnect_client( server, client_index );
}
```

Destroy every client and server before `rocketnet_term`. Request shutdown first,
wait by polling completion from the event loop, then destroy. An early destroy
is API misuse: it logs one error naming the skipped completion function, records
the misuse, then requests shutdown, updates a bounded number of times without
sleeping and frees once complete; otherwise it logs and keeps the object. A stalled
worker keeps completion false, so its object and contexts must remain alive.
Destroy is synchronous after completion; allocator hooks and thread-handle
cleanup do not have a strict wall-time guarantee.

## Configuration

The client and the server each take one plain config struct. Fill it with
`rocketnet_client_default_config` or `rocketnet_server_default_config`, set
what the program means, and pass it to create. Always fill the struct with
its default_config call first, as two lines:

```c
struct rocketnet_client_config_t config;
rocketnet_client_default_config( &config );
```

The struct's first field, `config_bytes`, is the size of the struct the
program compiled against, and default_config sets it; a config that skipped it
(`config_bytes` 0, a zeroed struct) is refused with
`ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED` (docs/ABI-EVOLUTION.md).

The server default has no backend key, and a server start refuses it until
the key is supplied, because default keys are how every deployment of a
protocol ends up sharing one identity. The program sets `protocol_id` and
`backend_public_key` into the config after default_config. For a server, the
environment (`ROCKETNET_SERVER_PROTOCOL_ID`,
`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`) is the recommended place for them, and
setting them in code works too. Each server generates its own keypair at
create unless the config's `server_secret_key` supplies one, for a backend that
seals tokens to a fixed server public key (KEYS.md). Always start from
`rocketnet_server_default_config`: create takes any nonzero bytes in that field
as the key, including uninitialized ones. Wipe your own copy of a supplied
secret once create returns. A client
config holds no keys: the connect token carries them, whether the backend mints
it in code, a tool generates it, or the program reads it from the environment.
For local work a development build sets `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`
into the server config by hand, and `rocketnet_create_dev_connect_token` makes
a matching local token string from the server's public key and address.

The precedence at create is **DEFAULT < STRUCT < ENV**: the defaults come
first, the struct you pass is the middle layer, and the environment overlays it
field by field. Every override is logged with the variable and the value it
displaced. The fields with no environment form are callbacks (`state_change_callback`
and `time_callback`), `loopback_hub`, `server_secret_key`, and `config_bytes`. Byte fields
are base64 of exactly the field's length; integers are decimal; strings are
taken as they are. An environment variable set to the empty string is ignored,
with a log line.

A value that does not parse, or is out of range, **refuses the create with one
error line; nothing is ever clamped**. Create is the one check of a config: there is no
separate validate call. A `config_bytes` larger than the library knows, from a
newer header, is read as far as the library knows; a smaller one, from an older
header, is read as far as it goes and the rest takes the defaults. Each create
takes a final status out-parameter (NULL is accepted): it holds
`ROCKETNET_STATUS_OK` with a non-NULL result, or the refusal's code with NULL.

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

`transport` is an ordered candidate list, comma separated, validated at create
and walked left to right at client `connect` and server `start`. The first
candidate that starts wins, and a list that every candidate refuses fails
loudly. `"auto"` (the default, also NULL or the empty
string) is the platform's recommended order; a one-element list is a pin.
The server's `auto` order is `xdp`, `gso`, `io_uring`, `portable` on Linux;
`msgx`, `portable` on macOS; and `iocp`, `rio`, `portable` on Windows in
production builds (`portable`, `iocp`, `rio` in development builds).
The client's `auto` order is the native client, then `portable`: `gso`,
`portable` on Linux; `msgx`, `portable` on macOS; `iocp`, `portable` on Windows. `portable` is ranked last on every OS in production and is the
fallback when the native transport's create refuses. A
known name this endpoint does not have is skipped with one log line; a name
that is no transport anywhere is a typo and fails loudly. When every name in a
list is skipped for this platform or endpoint, the endpoint falls back to its
`auto` order with a warning. The accessor on each
end reports the transport that won. See [TRANSPORTS.md](TRANSPORTS.md) for the
full registry table and privilege requirements. `loopback` is pin-only and
also needs the config's `loopback_hub`: a hub from
`rocketnet_loopback_hub_create`, shared by the server and its clients and
destroyed after them; create refuses a list naming `loopback` with a `NULL`
hub (`LOOPBACK_HUB_MISSING`).

Every transport serves IPv4, IPv6, or both: `rocketnet_server_create` takes an
IPv4 address and an IPv6 address, either NULL but not both. With both, on the
same port (both 0, or equal), the server binds the IPv6 wildcard on
that port with `IPV6_V6ONLY` off and advertises both named addresses.
`"auto"` tries `xdp` first when both addresses are on one interface. `xdp` on
two interfaces, and `portable` where `IPV6_V6ONLY` cannot be cleared, refuse
with `TRANSPORT_FAMILIES_UNSUPPORTED` and the ladder continues. A connect
token is accepted when its address of the arrival family is one the server
advertises (`TokenAddressAdvertised`).

`ROCKETNET_SERVER_IPV4_ADDRESS` and `ROCKETNET_SERVER_IPV6_ADDRESS`, when set,
override the create arguments for every server created, with a log line.

### Environment overrides

Every field except `state_change_callback`, `time_callback`, `loopback_hub`,
`server_secret_key` and `config_bytes` has an
environment variable named `ROCKETNET_CLIENT_<FIELD>` or
`ROCKETNET_SERVER_<FIELD>`. The table lists one row per field of both public
structs, except `config_bytes`.

| Field | Variable | Type | Default | A bad value |
|---|---|---|---|---|
| packet_filter | ROCKETNET_CLIENT_PACKET_FILTER | uint8_t[4], base64 | `b7 59 01 00` (test filter) | not exactly 4 bytes, or invalid base64: refuses the create; the test filter in a production build (`ROCKETNET_DEV=0`): refuses the create with `TEST_PACKET_FILTER_IN_PRODUCTION` |
| max_fragmented_messages | ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES | int | `0` (8) | refuses the create unless `0` or `1..256` |
| transport | ROCKETNET_CLIENT_TRANSPORT | const char * | `NULL` (`"auto"`) | an unknown name, an empty element, or `auto` inside a list: refuses the create; a list whose every name is skipped for this endpoint on this platform falls back to `auto` with a warning |
| state_change_callback | none | `void (*)( void *, enum rocketnet_client_state_t, enum rocketnet_client_state_t )` | `NULL` | no environment form |
| send_kbps | ROCKETNET_CLIENT_SEND_KBPS | int | `1000` | a non-integer refuses the create; `0` is the default (`1000`); a negative value refuses the create with `ROCKETNET_STATUS_INVALID_ARGUMENT`; `ROCKETNET_SEND_KBPS_UNLIMITED` is no send budget; each connection's starting rate |
| num_channels | ROCKETNET_CLIENT_NUM_CHANNELS | int | `32` (`ROCKETNET_MAX_CHANNELS` when smaller) | refuses the create unless `1..ROCKETNET_MAX_CHANNELS` |
| reliable_messages_in_flight | ROCKETNET_CLIENT_RELIABLE_MESSAGES_IN_FLIGHT | int | `1024` | refuses the create unless `64..1048576`, a power of two, and at least the fragments of one `max_message_bytes` message (one per 1152 bytes: 911 at the default, so `1024` and up) |
| receive_message_buffer_bytes | ROCKETNET_CLIENT_RECEIVE_MESSAGE_BUFFER_BYTES | int | `1048576` | refuses the create unless `4096..1048576` and a multiple of 16 |
| max_message_bytes | ROCKETNET_CLIENT_MAX_MESSAGE_BYTES | int | `1048576` | refuses the create unless `1415..1048576` and no larger than `receive_message_buffer_bytes` |
| send_message_buffer_bytes | ROCKETNET_CLIENT_SEND_MESSAGE_BUFFER_BYTES | int | `4194304` | refuses the create unless `12288..268435456` and large enough to hold one `max_message_bytes` message whole (a third of it holds the 1152-byte fragments: a little over three times `max_message_bytes`) |
| send_queue_messages | ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES | int | `1024` | refuses the create unless `16..1048576` |
| packets_in_flight | ROCKETNET_CLIENT_PACKETS_IN_FLIGHT | int | `1024` | refuses the create unless `64..1048576` and at least `reliable_messages_in_flight` |
| packets_per_update | ROCKETNET_CLIENT_PACKETS_PER_UPDATE | int | `256` | refuses the create unless `1..1024` |
| unreliable_messages_per_update | ROCKETNET_CLIENT_UNRELIABLE_MESSAGES_PER_UPDATE | int | `1024` | refuses the create unless `1..65536` |
| sequenced_messages_held | ROCKETNET_CLIENT_SEQUENCED_MESSAGES_HELD | int | `16` | refuses the create unless `0..4096` |
| max_message_receipts | ROCKETNET_CLIENT_MAX_MESSAGE_RECEIPTS | int | `256` | refuses the create unless `16..65536` |
| message_resend_delay_initial_ms | ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS | int | `2000` | refuses the create unless `10..60000` and between `message_resend_delay_min_ms` and `message_resend_delay_max_ms` |
| message_resend_delay_min_ms | ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS | int | `0` (disabled) | refuses the create unless `0..60000` and no larger than `message_resend_delay_max_ms` |
| message_resend_delay_max_ms | ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS | int | `2000` | refuses the create unless `10..60000` and at least `message_resend_delay_initial_ms` |
| reliable_message_timeout_ms | ROCKETNET_CLIENT_RELIABLE_MESSAGE_TIMEOUT_MS | int | `10000` | refuses the create unless `1000..600000` and at least four times `message_resend_delay_initial_ms` (`8000` at the default) |
| time_callback | none | `double (*)( void * )` | `NULL` (platform clock) | development builds only: the receive stamp clock; a production build (`ROCKETNET_DEV=0`) refuses a non-NULL value at create with `CONFIG_OUT_OF_RANGE` |
| step_driven | ROCKETNET_CLIENT_STEP_DRIVEN (development only) | bool | `false` | development builds only: no client network thread, see "Step-driven client"; a production build refuses `true` at create with `CONFIG_OUT_OF_RANGE` |
| protocol_id | ROCKETNET_SERVER_PROTOCOL_ID | uint64_t | `0` | a non-decimal value, or one past the type: refuses the create |
| max_clients | ROCKETNET_SERVER_MAX_CLIENTS | int | `1000` | refuses the create unless `1..1000` |
| packet_filter | ROCKETNET_SERVER_PACKET_FILTER | uint8_t[4], base64 | `b7 59 01 00` (test filter) | not exactly 4 bytes, or invalid base64: refuses the create; the test filter in a production build (`ROCKETNET_DEV=0`): refuses the create with `TEST_PACKET_FILTER_IN_PRODUCTION` |
| backend_public_key | ROCKETNET_SERVER_BACKEND_PUBLIC_KEY | uint8_t[32], base64 | all zero (no key) | not exactly 32 bytes, or invalid base64: refuses the create |
| num_xdp_queues | ROCKETNET_SERVER_NUM_XDP_QUEUES | int | `0` (transport default) | refuses the create unless `0` or `1..1024`; the Windows iocp transport ignores it (always runs one completion worker plus one send worker) and the rio transport ignores it (runs one I/O worker) |
| num_os_cpus | ROCKETNET_SERVER_NUM_OS_CPUS | int | `0` (transport default) | refuses the create unless `0` or `1..256` |
| max_fragmented_messages | ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES | int | `0` (8) | refuses the create unless `0` or `1..256` |
| max_timeout_seconds | ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS | int | `60` | refuses the create unless `1..3600` |
| handshake_deadline_seconds | ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS | int | `5` | refuses the create unless `1..60` |
| transport | ROCKETNET_SERVER_TRANSPORT | const char * | `NULL` (`"auto"`) | an unknown name, an empty element, or `auto` inside a list: refuses the create; a list whose every name is skipped for this endpoint on this platform falls back to `auto` with a warning |
| per_client_send_kbps | ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS | int | `10000` | a non-integer refuses the create; `0` is the default (`10000`); a negative value refuses the create with `ROCKETNET_STATUS_INVALID_ARGUMENT`; `ROCKETNET_SEND_KBPS_UNLIMITED` is no send budget; each client connection's starting rate |
| num_channels | ROCKETNET_SERVER_NUM_CHANNELS | int | `32` (`ROCKETNET_MAX_CHANNELS` when smaller) | refuses the create unless `1..ROCKETNET_MAX_CHANNELS` |
| reliable_messages_in_flight | ROCKETNET_SERVER_RELIABLE_MESSAGES_IN_FLIGHT | int | `1024` | refuses the create unless `64..1048576`, a power of two, and at least the fragments of one `max_message_bytes` message (one per 1152 bytes: 911 at the default, so `1024` and up) |
| receive_message_buffer_bytes | ROCKETNET_SERVER_RECEIVE_MESSAGE_BUFFER_BYTES | int | `1048576` | refuses the create unless `4096..1048576` and a multiple of 16 |
| max_message_bytes | ROCKETNET_SERVER_MAX_MESSAGE_BYTES | int | `1048576` | refuses the create unless `1415..1048576` and no larger than `receive_message_buffer_bytes` |
| send_message_buffer_bytes | ROCKETNET_SERVER_SEND_MESSAGE_BUFFER_BYTES | int | `4194304` (per client) | refuses the create unless `12288..268435456` and large enough to hold one `max_message_bytes` message whole (a third of it holds the 1152-byte fragments: a little over three times `max_message_bytes`) |
| send_queue_messages | ROCKETNET_SERVER_SEND_QUEUE_MESSAGES | int | `1024` (per client) | refuses the create unless `16..1048576` |
| packets_in_flight | ROCKETNET_SERVER_PACKETS_IN_FLIGHT | int | `1024` (per client) | refuses the create unless `64..1048576` and at least `reliable_messages_in_flight` |
| packets_per_update | ROCKETNET_SERVER_PACKETS_PER_UPDATE | int | `256` (per client) | refuses the create unless `1..1024` |
| unreliable_messages_per_update | ROCKETNET_SERVER_UNRELIABLE_MESSAGES_PER_UPDATE | int | `1024` (per client per update) | refuses the create unless `1..65536` |
| sequenced_messages_held | ROCKETNET_SERVER_SEQUENCED_MESSAGES_HELD | int | `16` | refuses the create unless `0..4096` |
| max_message_receipts | ROCKETNET_SERVER_MAX_MESSAGE_RECEIPTS | int | `256` | refuses the create unless `16..65536` |
| message_resend_delay_initial_ms | ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS | int | `2000` | refuses the create unless `10..60000` and between `message_resend_delay_min_ms` and `message_resend_delay_max_ms` |
| message_resend_delay_min_ms | ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS | int | `0` (disabled) | refuses the create unless `0..60000` and no larger than `message_resend_delay_max_ms` |
| message_resend_delay_max_ms | ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS | int | `2000` | refuses the create unless `10..60000` and at least `message_resend_delay_initial_ms` |
| reliable_message_timeout_ms | ROCKETNET_SERVER_RELIABLE_MESSAGE_TIMEOUT_MS | int | `10000` | refuses the create unless `1000..600000` and at least four times `message_resend_delay_initial_ms` (`8000` at the default) |
| server_secret_key | none | uint8_t[32] | all zero (generate a fresh keypair) | the published development server key in a production build: refuses the start with `DEV_KEYS_IN_PRODUCTION` |

### Step-driven client

In a development build (`ROCKETNET_DEV=1`), `client_config.step_driven = true`
runs the client with no network thread: `rocketnet_client_update` and
`rocketnet_client_send_packets` do its receive and send work on the caller's
thread. Over the `"loopback"` transport the server needs no thread either, so a
message the client sends with `rocketnet_client_send_packets` arrives in the
server's next `rocketnet_server_update`, and a server message in the client's
next update, all on one thread. Use it for single-thread debugging and for unit
tests on the public API with no threads, sockets or real time; set
`time_callback` to a clock the test owns so receive stamps are deterministic
too. The granted transport must have a non-blocking I/O step (loopback has
one). A production build (`ROCKETNET_DEV=0`) refuses `step_driven` or a
`time_callback` at create with `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`.

The fields the header marks "must match on both ends" must be identical on
both ends. The connection verifies them. `max_message_bytes` must not exceed
`receive_message_buffer_bytes`, and create refuses a config that breaks it.
`max_message_bytes` is the one size limit for every reliability type: an
unreliable message too large for one packet is sent as its reliable twin.

### Operator-only environment variables

Several variables configure global subsystem behavior without corresponding
fields in the public configuration structs:

| Variable | Type | Default | Description |
|---|---|---|---|
| `ROCKETNET_CLIENT_TOKEN_EPOCH_UNIX_SECONDS` / `ROCKETNET_SERVER_TOKEN_EPOCH_UNIX_SECONDS` | uint64, unix seconds | `0` (wall clock, read once) | Sets `token_epoch_unix_seconds`, the unix time at the game clock's zero, for the connect-token create and expire checks, which read the epoch plus the caller's time; `0` reads the host wall clock once (client connect, server first update) as the epoch. |
| `ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH` / `ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH` | int, `2..1000000` | `0` (off) | Development only: drop every Nth outgoing datagram. |
| `ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW` / `ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW` | int, `2..64` | `0` (off) | Development only: reverse each window of W outgoing datagrams. |
| `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` | uint64 | `0` (unlimited) | Read at `rocketnet_server_create`. Enforces an upper bound on server transport memory plans; over-budget candidates are skipped, and `rocketnet_server_start` (which creates the transport) fails with `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED` only when no candidate fits. |
| `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` | uint64 | `0` (unlimited) | Read at `rocketnet_client_create`. Enforces an upper bound on client transport memory plans; over-budget candidates are skipped, and `rocketnet_client_connect` (which creates the transport) fails with `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED` only when no candidate fits. |
| `ROCKETNET_GATEWAY_ETHERNET_ADDRESS` | string (`xx:xx:xx:xx:xx:xx`) | auto-discovered | Sets the gateway MAC address for Linux AF_XDP server sockets when automatic gateway ARP discovery cannot resolve it. |
| `ROCKETNET_LOG_LEVEL` | int (`0..5`) | `info` (`3`) | Sets process-wide log verbosity: `none` (`0`), `error` (`1`), `warn` (`2`), `info` (`3`), `debug` (`4`), `spam` (`5`). Only a whole-string integer in `0..5` is accepted; the names are output names, not accepted inputs, and a word is ignored with a warning. |

### Server cost is flat by design

The design requirement is to provision for configured capacity rather than
grow storage as players join. It is not evidence that an idle and a busy server
take identical CPU time: packet work, active messages, and transport scheduling
still need measurement. Size and test at `max_clients` with every slot active.
The [capacity guide](MEMORY.md#qualification-and-scale) separates this
requirement from recorded workload results and the unqualified timing target.

### Server memory footprint

Use [Memory and capacity](MEMORY.md#server-memory-footprint) for required
allocator memory, configuration profiles, and host resource planning.

### Receive buffer sizing

Use [Receive buffer sizing](MEMORY.md#receive-buffer-sizing) before changing
message limits or the number of outstanding receive batches.

## Counters and stats

Three calls fill caller-owned structs:
`rocketnet_client_stats` fills `struct rocketnet_client_stats_t`,
`rocketnet_server_stats` fills `struct rocketnet_server_stats_t`, and
`rocketnet_server_client_stats` fills `struct rocketnet_server_client_stats_t`
for one slot. Public fields, units and meanings are unchanged:

- **Client stats (`rocketnet_client_stats_t`)**: packet, byte, payload, resend,
  duplicate, receipt and budget counts (`packets_sent`, `packets_received`,
  `bytes_sent`, `bytes_received`, `payloads_received`, `messages_resent`,
  `messages_duplicated`, `receipts_acked`, `receipts_lost`, `receipts_dropped`
  and `messages_dropped_over_budget`) are connection-lifetime counters for the
  connection the client currently holds. They start at `0` when each connection
  starts (including a reconnect to the same or a different server) and read `0`
  when no connection is held, including after `rocketnet_client_disconnect`
  while the old connection is still retiring. `messages_dropped_not_connected`
  is the handle-lifetime exception on `rocketnet_client_stats_t`: it stays on
  the client handle and accumulates monotonically across connections until
  `rocketnet_client_destroy`. Client quality and rate fields (`rtt_ms`,
  `jitter_ms`, `packet_loss_percent`, `packets_sent_per_second`,
  `packets_received_per_second`, `bandwidth_sent_kbps` and
  `bandwidth_received_kbps`) share the same current-connection boundary and
  read `0.0` when no connection is held.
- **Server aggregate stats (`rocketnet_server_stats_t`)**: server-wide counts
  (`packets_sent`, `packets_received`, `bytes_sent`, `bytes_received`,
  `connections_accepted`, `connections_denied_full`,
  `handshakes_dropped_over_budget`, `payloads_received`, `messages_resent`,
  `messages_duplicated`, `receipts_acked`, `receipts_lost`, `receipts_dropped`,
  `messages_dropped_over_budget` and `messages_dropped_not_connected`) are
  server-object lifetime totals that start at `0` at `rocketnet_server_create`
  and accumulate monotonically across `rocketnet_server_start` and
  `rocketnet_server_stop` cycles until `rocketnet_server_destroy`. Server
  trailing-second rates (`packets_sent_per_second`,
  `packets_received_per_second`, `bandwidth_sent_kbps` and
  `bandwidth_received_kbps`) cover the whole server while running and read `0.0`
  while the server is stopped.
- **Server per-slot stats (`rocketnet_server_client_stats_t`)**: per-client
  counts (`packets_sent`, `packets_received`, `bytes_sent`, `bytes_received`),
  quality measurements (`rtt_ms`, `jitter_ms`, `packet_loss_percent`) and
  trailing-second bandwidth rates (`bandwidth_sent_kbps`,
  `bandwidth_received_kbps`, `bandwidth_acked_kbps`) cover the slot's current
  connection, reset to `0` when a new connection takes the slot, and read `0`
  when the slot has no connected client or the server is stopped.
- **Process-wide counts**: `api_misuses` and `thread_priority_refusals` on both
  `rocketnet_client_stats_t` and `rocketnet_server_stats_t` are process-wide
  totals and do not reset with a connection or endpoint object.

Client quality stats are computed synchronously when `rocketnet_client_stats`
is called. Server-wide totals are copied from the server's current counters.
Server per-client stats combine current counters with ACK-derived quality
values that are always current: each slot keeps running aggregates that every
send, receive and ack updates in O(1) (the rtt minimum in O(log n)), so RTT,
jitter, loss and bandwidth equal a full recompute over the retained history at
every read, and an idle slot costs nothing. The per-client API call copies
those values; server update does no statistics pass and runs no statistics
thread. Reset clears that slot's quality values immediately
(sdk/rocketnet_ack.cpp, proofs/tla/RNServerStats.tla).

Watch jitter and loss when conditions worsen; the rtt figure is the minimum
over the retained history, so it cannot track latency upward until every lower
sample has been evicted. A server-side rtt of zero means no sample has arrived
yet, not a zero-millisecond link.

```c
#include "rocketnet.h"

void report_quality( const struct rocketnet_client_t * client, const struct rocketnet_server_t * server, int client_index )
{
    struct rocketnet_client_stats_t client_stats;
    rocketnet_client_stats( client, &client_stats );
    uint64_t packets_sent = client_stats.packets_sent;
    uint64_t receipts_lost = client_stats.receipts_lost;
    double rtt_ms = client_stats.rtt_ms;
    double loss_percent = client_stats.packet_loss_percent;

    struct rocketnet_server_stats_t server_stats;
    rocketnet_server_stats( server, &server_stats );
    uint64_t connections_accepted = server_stats.connections_accepted;

    struct rocketnet_server_client_stats_t slot_stats;
    rocketnet_server_client_stats( server, client_index, &slot_stats );
    double client_rtt_ms = slot_stats.rtt_ms;

    (void) packets_sent;
    (void) receipts_lost;
    (void) rtt_ms;
    (void) loss_percent;
    (void) connections_accepted;
    (void) client_rtt_ms;
}
```

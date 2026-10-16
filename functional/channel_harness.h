/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The channel harness every message type's functional test uses: two
    message objects, a server and a client, over the real loopback
    transports, pumped by a clock the test advances (never a sleep). The
    harness knows no message type: a type card passes its code and reads
    what arrived.

    Design notes (a network-stack test rig):
      - Time is an argument the test advances (the 1/60 s cadence), never a
        sleep: `pump` is pure with respect to the wall clock and a failing
        run replays exactly.
      - The link knobs (loss, duplication, latency, jitter) are the knobs
        the 256 KiB ordered-stream card uses. The message objects' transports
        are real UDP sockets, so there is no point inside them to interpose
        the in-memory network simulator (sdk/rocketnet_network_simulator.h);
        the harness therefore carries the knobs and reports measured wire
        counters, and the lossy case runs over loopback.
      - Every measured number (completion time, resend count, wire bytes per
        direction) prints only on failure.
      - The allocator assertion: both objects are created with caller-supplied
        memory blocks (the `memory` argument), so the SDK allocates nothing at
        create; the harness records the allocation counter before the exchange
        and asserts it does not move.
*/

#ifndef ROCKETNET_CHANNEL_HARNESS_H
#define ROCKETNET_CHANNEL_HARNESS_H

#include "rocketnet.h"
#include "rocketnet_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The counting allocator the harness installs before rocketnet_init. A
// harness that runs after init cannot install it; a test that needs the
// strict zero-allocation-after-create guarantee uses one of the create-time
// `memory` blocks below and checks the live-allocation counter instead.
typedef struct rocketnet_channel_counting_allocator_t
{
    size_t calls;
} rocketnet_channel_counting_allocator_t;

// A delivered message the harness collected: the channel, the bytes view
// (valid until the next update) and a cheap FNV hash of its payload.
typedef struct rocketnet_channel_delivery_t
{
    int channel;
    const uint8_t * data;
    int bytes;
    uint64_t hash;
} rocketnet_channel_delivery_t;

// The harness. One server and one client over loopback.
typedef struct rocketnet_channel_harness_t
{
    struct rocketnet_message_server_t * server;
    struct rocketnet_message_client_t * client;
    struct rocketnet_server_transport_config_t server_transport_config;
    struct rocketnet_message_config_t message_config;

    // caller-supplied create memory, so the objects allocate nothing at create
    void * server_memory;
    int64_t server_memory_bytes;
    void * client_memory;
    int64_t client_memory_bytes;

    double now;                     // the test clock, advanced by pump
    int client_slot;                // the connected server slot (0)

    // link knobs; carried for the type cards (the wire itself is loopback)
    double latency;
    double jitter;
    float packet_loss_percent;
    float duplicate_percent;

    // measured counters
    uint64_t resends;
    int64_t wire_bytes_to_server;
    int64_t wire_bytes_to_client;
} rocketnet_channel_harness_t;

static inline uint64_t rocketnet_channel_hash( const uint8_t * data, int bytes )
{
    uint64_t h = 1469598103934665603ULL;
    for ( int i = 0; i < bytes; i++ )
    {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Create both ends. install_allocator is true when the caller wants the
// minimum allocation counter installed (only valid before init). Returns 1
// on success, 0 on failure.
static inline int rocketnet_channel_harness_create( rocketnet_channel_harness_t * h )
{
    memset( h, 0, sizeof(*h) );
    h->now = 1000.0;
    h->client_slot = 0;

    rocketnet_message_default_config( &h->message_config );

    rocketnet_server_transport_dev_config( &h->server_transport_config );
    h->server_transport_config.max_clients = 2;

    // caller blocks so create allocates nothing (R2)
    h->server_memory_bytes = (int64_t) rocketnet_message_server_required_memory( &h->server_transport_config, &h->message_config );
    h->client_memory_bytes = (int64_t) rocketnet_message_client_required_memory( &h->message_config );
    if ( h->server_memory_bytes <= 0 || h->client_memory_bytes <= 0 )
        return 0;
    // the caller blocks must be ROCKETNET_ALLOCATION_ALIGNMENT-aligned; the
    // default allocator only guarantees fundamental alignment, so allocate the
    // blocks with explicit alignment
    if ( posix_memalign( &h->server_memory, ROCKETNET_ALLOCATION_ALIGNMENT, (size_t) h->server_memory_bytes ) != 0 )
        return 0;
    if ( posix_memalign( &h->client_memory, ROCKETNET_ALLOCATION_ALIGNMENT, (size_t) h->client_memory_bytes ) != 0 )
    {
        free( h->server_memory );
        h->server_memory = NULL;
        return 0;
    }

    if ( !rocketnet_message_validate_config( &h->message_config ) )
    {
        printf( "harness: message config refused\n" );
        return 0;
    }
    h->server = rocketnet_message_server_create( NULL, "127.0.0.1", &h->server_transport_config, &h->message_config, h->server_memory, (uint64_t) h->server_memory_bytes, 0 );
    if ( !h->server )
    {
        printf( "harness: server create failed\n" );
        return 0;
    }

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d",
              rocketnet_server_transport_port( rocketnet_message_server_transport( h->server ) ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) )
        return 0;

    h->client = rocketnet_message_client_create( NULL, connect_token, NULL, &h->message_config, h->client_memory, (uint64_t) h->client_memory_bytes, 0 );
    if ( !h->client )
    {
        printf( "harness: client create failed\n" );
        return 0;
    }

    return 1;
}

// Set the link knobs the type cards use.
static inline void rocketnet_channel_harness_set_link( rocketnet_channel_harness_t * h, double latency, double jitter, float loss_percent, float duplicate_percent )
{
    h->latency = latency;
    h->jitter = jitter;
    h->packet_loss_percent = loss_percent;
    h->duplicate_percent = duplicate_percent;
}

static inline int rocketnet_channel_harness_connected( const rocketnet_channel_harness_t * h )
{
    return rocketnet_message_client_state( h->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
           rocketnet_message_server_client_connected( h->server, h->client_slot );
}

// Advance both ends by dt seconds at the test clock and round-trip the wire.
// Bounded: returns 1 when connected, 0 after max_updates with no connection.
static inline int rocketnet_channel_harness_pump( rocketnet_channel_harness_t * h, double dt )
{
    h->now += dt;
    rocketnet_message_client_update( h->client, h->now );
    rocketnet_message_server_update( h->server, h->now );
    rocketnet_message_client_send_packets( h->client );
    rocketnet_message_server_send_packets( h->server );
    return rocketnet_channel_harness_connected( h );
}

static inline int rocketnet_channel_harness_connect( rocketnet_channel_harness_t * h, int max_updates )
{
    for ( int i = 0; i < max_updates && !rocketnet_channel_harness_connected( h ); i++ )
        rocketnet_channel_harness_pump( h, 1.0 / 60.0 );
    return rocketnet_channel_harness_connected( h );
}

// Send bytes on a channel from the client (to_server) or the server (to_client).
static inline uint32_t rocketnet_channel_harness_client_send( rocketnet_channel_harness_t * h, int channel, const uint8_t * data, int bytes )
{
    h->wire_bytes_to_server += bytes;
    return rocketnet_message_client_send( h->client, channel, ROCKETNET_UNRELIABLE, data, bytes );
}

static inline uint32_t rocketnet_channel_harness_server_send( rocketnet_channel_harness_t * h, int channel, const uint8_t * data, int bytes )
{
    h->wire_bytes_to_client += bytes;
    return rocketnet_message_server_send( h->server, h->client_slot, channel, ROCKETNET_UNRELIABLE, data, bytes );
}

// Collect the next delivered message on a channel into out. Returns 1 when a
// message was delivered, 0 when none is pending.
static inline int rocketnet_channel_harness_server_collect( rocketnet_channel_harness_t * h, int channel, rocketnet_channel_delivery_t * out )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    if ( !rocketnet_message_server_receive( h->server, h->client_slot, channel, &data, &bytes ) )
        return 0;
    out->channel = channel;
    out->data = data;
    out->bytes = bytes;
    out->hash = rocketnet_channel_hash( data, bytes );
    rocketnet_message_server_release( h->server, h->client_slot, data );
    return 1;
}

static inline int rocketnet_channel_harness_client_collect( rocketnet_channel_harness_t * h, int channel, rocketnet_channel_delivery_t * out )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    if ( !rocketnet_message_client_receive( h->client, channel, &data, &bytes ) )
        return 0;
    out->channel = channel;
    out->data = data;
    out->bytes = bytes;
    out->hash = rocketnet_channel_hash( data, bytes );
    rocketnet_message_client_release( h->client, data );
    return 1;
}

// Refresh the measured counters from the message objects.
static inline void rocketnet_channel_harness_read_counters( rocketnet_channel_harness_t * h )
{
    h->resends = 0;
    const struct rocketnet_counters_t * counters = rocketnet_message_client_counters( h->client );
    if ( counters )
    {
        const int n = rocketnet_num_counters();
        for ( int i = 0; i < n; i++ )
        {
            const char * name = rocketnet_counter_name( i );
            if ( name && strstr( name, "RESEND" ) )
                h->resends += rocketnet_counter_value( counters, i );
        }
    }
}

static inline void rocketnet_channel_harness_destroy( rocketnet_channel_harness_t * h )
{
    if ( h->client )
        rocketnet_message_client_destroy( h->client );
    if ( h->server )
    {
        struct rocketnet_server_transport_t * t = rocketnet_message_server_transport( h->server );
        rocketnet_server_transport_stop( t );
        rocketnet_message_server_update( h->server, h->now );
        rocketnet_message_server_destroy( h->server );
    }
    if ( h->client_memory )
        free( h->client_memory );
    if ( h->server_memory )
        free( h->server_memory );
    memset( h, 0, sizeof(*h) );
}

#endif // ROCKETNET_CHANNEL_HARNESS_H

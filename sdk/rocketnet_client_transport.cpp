/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_protocol.h"

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <atomic>

// the receive thread fills one buffer while update processes the other --
// flow's IO, carried over whole

struct rocketnet_client_receive_buffer_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_NUM_CLIENT_PACKETS];
    double receive_time[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    size_t packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_NUM_CLIENT_PACKETS];
};

// payloads are decrypted out of the receive frames into this storage, and
// handed to the caller as pointers into it, valid until the next update

// the client payload ring: the acquire/commit staging, filled from any
// number of game threads between updates and flushed by send_packets on
// the main thread. same discipline as the server's: one atomic add per
// slot, per-slot info alone on its cache line, roots 16-aligned.

#define ROCKETNET_CLIENT_PAYLOAD_RING_SIZE     256

struct alignas(64) rocketnet_client_payload_slot_info_t
{
    int wire_bytes;
};

struct rocketnet_client_payload_ring_t
{
    uint8_t padding_0[4096];

    volatile uint64_t num_packets;

    uint8_t padding_1[4096];

    struct rocketnet_client_payload_slot_info_t info[ROCKETNET_CLIENT_PAYLOAD_RING_SIZE];

    uint8_t padding_2[4096];

    alignas(16) uint8_t data[ROCKETNET_FRAME_STRIDE*ROCKETNET_CLIENT_PAYLOAD_RING_SIZE];

    uint8_t padding_3[4096];
};

struct rocketnet_client_received_packets_t
{
    int num_packets;
    int receive_index;
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    int packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_NUM_CLIENT_PACKETS];
};

struct rocketnet_client_t
{
    void * context;

    uint16_t bound_port;

    rocketnet_platform_socket_t * socket;

    rocketnet_platform_thread_t * thread;
    rocketnet_platform_mutex_t mutex;
    std::atomic<bool> quit;

    int receive_buffer_index;
    rocketnet_client_received_packets_t received_packets;
    rocketnet_client_receive_buffer_t receive_buffer[2];

    double time;                                        // the game's clock, last seen
    int last_state;
    struct rocketnet_client_config_t config;

    struct rocketnet_client_payload_ring_t payload_ring;

    struct rocketnet_counters_t counters;
    struct rocketnet_client_protocol_t protocol;
};

static void rocketnet_client_thread_function( void * data );

void rocketnet_client_default_config( struct rocketnet_client_config_t * config )
{
    rocketnet_assert( config );
    memset( config, 0, sizeof(struct rocketnet_client_config_t) );
    const uint8_t test_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( config->filter_value, test_filter, ROCKETNET_FILTER_VALUE_BYTES );
}

rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, double time )
{
    rocketnet_assert( connect_token );

    struct rocketnet_client_config_t default_config;
    if ( !config )
    {
        rocketnet_client_default_config( &default_config );
        config = &default_config;
    }

    rocketnet_client_t * client_transport = (rocketnet_client_t*) rocketnet_malloc( context, sizeof(rocketnet_client_t) );
    if ( !client_transport )
    {
        rocketnet_error( "could not allocate client" );
        return NULL;
    }

    memset( (char*) client_transport, 0, sizeof(rocketnet_client_t) );

    client_transport->context = context;

    rocketnet_address_t bind_address;
    memset( &bind_address, 0, sizeof(bind_address) );
    bind_address.type = ROCKETNET_ADDRESS_IPV4;

    // IMPORTANT: for many platforms it's best practice to bind to ipv6 and go dual stack on the client
    if ( rocketnet_platform_client_dual_stack() )
    {
        rocketnet_info( "client socket is dual stack ipv4 and ipv6" );
        bind_address.type = ROCKETNET_ADDRESS_IPV6;
    }

    // IMPORTANT: some platforms (GDK) have a preferred port that we must use to access packet tagging
    // If the bind address has set port of 0, substitute the preferred client port here
    if ( bind_address.port == 0 )
    {
        int preferred_client_port = rocketnet_platform_preferred_client_port();
        if ( preferred_client_port != 0 )
        {
            rocketnet_info( "client socket using preferred port %d", preferred_client_port );
            bind_address.port = preferred_client_port;
        }
    }

    client_transport->socket = rocketnet_platform_socket_create( client_transport->context, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    if ( client_transport->socket == NULL )
    {
        rocketnet_error( "client could not create socket" );
        rocketnet_client_destroy( client_transport );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client bound to %s", rocketnet_address_to_string( &bind_address, address_string ) );

    client_transport->bound_port = bind_address.port;

    // connect with the caller's token: the token names the server and is
    // the credential. the game owns the clock; unix time is read here once
    // for the token expiry check, which is wall-clock by definition.

    rocketnet_client_protocol_init( &client_transport->protocol, &client_transport->counters );

    memcpy( client_transport->protocol.filter_value, config->filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    client_transport->config = *config;
    client_transport->time = time;
    client_transport->last_state = client_transport->protocol.state;

    if ( !rocketnet_client_protocol_connect( &client_transport->protocol, connect_token, time, (uint64_t) ::time( NULL ) ) )
    {
        rocketnet_client_destroy( client_transport );
        return NULL;
    }

    char string_buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client connecting to %s", rocketnet_address_to_string( &client_transport->protocol.server_address, string_buffer ) );

    if ( !rocketnet_platform_mutex_create( &client_transport->mutex ) )
    {
        rocketnet_error( "client could not create mutex" );
        rocketnet_client_destroy( client_transport );
        return NULL;
    }

    client_transport->thread = rocketnet_platform_thread_create( NULL, rocketnet_client_thread_function, client_transport );
    if ( !client_transport->thread )
    {
        rocketnet_error( "client could not create thread" );
        rocketnet_client_destroy( client_transport );
        return NULL;
    }

    return client_transport;
}

void rocketnet_client_destroy( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );

    if ( client_transport->thread )
    {
        client_transport->quit = true;
        rocketnet_platform_thread_join( client_transport->thread );
        rocketnet_platform_thread_destroy( client_transport->thread );
    }

    if ( client_transport->socket )
    {
        rocketnet_platform_socket_destroy( client_transport->socket );
    }

    rocketnet_platform_mutex_destroy( &client_transport->mutex );

    rocketnet_clear_and_free( client_transport->context, client_transport, sizeof(rocketnet_client_t) );
}

static void rocketnet_client_process_packet( rocketnet_client_t * client_transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes, double current_time )
{
    // only the dialed server is listened to; everything else is junk

    if ( !rocketnet_address_equal( from, &client_transport->protocol.server_address ) )
        return;

    struct rocketnet_client_received_packets_t * received = &client_transport->received_packets;

    uint8_t * payload = received->num_packets < ROCKETNET_NUM_CLIENT_PACKETS ?
        received->data + received->num_packets * ROCKETNET_FRAME_SIZE : NULL;

    uint8_t overflow[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    if ( rocketnet_client_protocol_process_packet( &client_transport->protocol, packet_data, packet_bytes, current_time,
                                          payload ? payload : overflow, &payload_bytes ) &&
         payload_bytes > 0 && payload )
    {
        const int index = received->num_packets;
        received->packet_data[index] = payload;
        received->packet_bytes[index] = payload_bytes;
        received->num_packets++;
    }
}

static void rocketnet_client_update_swap_receive_buffers( rocketnet_client_t * client_transport )
{
    rocketnet_platform_mutex_acquire( &client_transport->mutex );

    client_transport->receive_buffer_index++;
    client_transport->receive_buffer_index &= 1;

    const int off_index = client_transport->receive_buffer_index ? 0 : 1;

    client_transport->receive_buffer[off_index].num_packets = 0;

    rocketnet_platform_mutex_release( &client_transport->mutex );
}

void rocketnet_client_update( rocketnet_client_t * client_transport, double time )
{
    rocketnet_assert( client_transport );

    const double current_time = time;
    client_transport->time = time;

    const int state_before = client_transport->protocol.state;

    rocketnet_client_update_swap_receive_buffers( client_transport );

    struct rocketnet_client_receive_buffer_t * receive_buffer = &client_transport->receive_buffer[client_transport->receive_buffer_index];

    client_transport->received_packets.num_packets = 0;
    client_transport->received_packets.receive_index = 0;

    for ( int i = 0; i < receive_buffer->num_packets; i++ )
    {
        rocketnet_client_process_packet( client_transport, &receive_buffer->from[i], receive_buffer->packet_data[i], (int) receive_buffer->packet_bytes[i], current_time );
    }

    receive_buffer->num_packets = 0;

    // the protocol's scheduled send and its deadlines

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client_transport->protocol, current_time, packet, &packet_bytes );
    if ( packet_bytes > 0 )
        rocketnet_platform_socket_send_packet( client_transport->socket, &client_transport->protocol.server_address, packet, packet_bytes );

    // one callback per update crossing: enough to observe every terminal,
    // and the poll of rocketnet_client_state stays the source of truth

    if ( client_transport->protocol.state != state_before && client_transport->config.state_change_callback )
        client_transport->config.state_change_callback( client_transport->context, state_before, client_transport->protocol.state );
}

void rocketnet_client_send_packet( rocketnet_client_t * client_transport, const uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    if ( packet_bytes < 1 || packet_bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_start_packet( client_transport, &context );
    if ( !frame )
        return;

    memcpy( frame + context.payload_index, packet_data, packet_bytes );

    rocketnet_client_finish_packet( client_transport, &context, packet_bytes );
    rocketnet_client_send_packets( client_transport );
}

// The client's acquire/commit family: the same shape, layout and phase
// contract as the server's, minus the client index. The convenience send
// above rides this path and flushes immediately, preserving its
// send-it-now meaning for simple callers.

uint8_t * rocketnet_client_start_packet( rocketnet_client_t * client_transport, struct rocketnet_packet_context_t * context )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    memset( context, 0, sizeof(struct rocketnet_packet_context_t) );

    uint64_t sequence;
    if ( !rocketnet_client_protocol_start_payload( &client_transport->protocol, &sequence ) )
        return NULL;

    struct rocketnet_client_payload_ring_t * ring = &client_transport->payload_ring;

    uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( slot >= ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
    {
        rocketnet_spam( "client payload ring full. packet dropped" );
        rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return NULL;
    }

    ring->info[slot].wire_bytes = 0;

    uint8_t * frame = ring->data + slot * ROCKETNET_FRAME_STRIDE;

    rocketnet_assert( ( (uintptr_t) frame % 16 ) == 0 );

    context->frame = frame;
    context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET;
    context->payload_capacity = ROCKETNET_MAX_PAYLOAD_BYTES;
    context->packet_id = slot;
    context->sequence = sequence;
    context->client_index = -1;

    return frame;
}

void rocketnet_client_abort_packet( rocketnet_client_t * client_transport, struct rocketnet_packet_context_t * context )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return;

    client_transport->payload_ring.info[context->packet_id].wire_bytes = 0;
    context->frame = NULL;
}

int rocketnet_client_finish_packet( rocketnet_client_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return 0;

    if ( payload_bytes < 1 || payload_bytes > context->payload_capacity )
    {
        rocketnet_client_abort_packet( client_transport, context );
        return 0;
    }

    int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &client_transport->protocol, context->sequence,
                                                                   context->frame + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                   payload_bytes, client_transport->time );
    if ( wire_bytes == 0 )
    {
        rocketnet_client_abort_packet( client_transport, context );
        return 0;
    }

    client_transport->payload_ring.info[context->packet_id].wire_bytes = wire_bytes;
    context->frame = NULL;

    return wire_bytes;
}

void rocketnet_client_send_packets( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );

    struct rocketnet_client_payload_ring_t * ring = &client_transport->payload_ring;

    uint64_t num_packets = ring->num_packets;
    if ( num_packets > ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
        num_packets = ROCKETNET_CLIENT_PAYLOAD_RING_SIZE;

    for ( uint64_t i = 0; i < num_packets; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            rocketnet_platform_socket_send_packet( client_transport->socket, &client_transport->protocol.server_address,
                                                   ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET,
                                                   ring->info[i].wire_bytes );
        }
    }

    ring->num_packets = 0;
}

int rocketnet_client_receive_packet( rocketnet_client_t * client_transport, uint8_t * packet_data )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( packet_data );

    struct rocketnet_client_received_packets_t * received = &client_transport->received_packets;

    if ( received->receive_index >= received->num_packets )
        return 0;

    const int index = received->receive_index++;
    memcpy( packet_data, received->packet_data[index], (size_t) received->packet_bytes[index] );

    return received->packet_bytes[index];
}

// the batch the copying cursor above walks, exposed whole: by count and
// index, and as parallel arrays in one call for bindings and tight loops.
// valid until the next update.

int rocketnet_client_num_payloads( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );
    return client_transport->received_packets.num_packets;
}

uint8_t * rocketnet_client_payload( rocketnet_client_t * client_transport, int payload_index, int * payload_bytes )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( payload_bytes );

    struct rocketnet_client_received_packets_t * received = &client_transport->received_packets;

    if ( payload_index < 0 || payload_index >= received->num_packets )
    {
        *payload_bytes = 0;
        return NULL;
    }

    *payload_bytes = received->packet_bytes[payload_index];
    return received->packet_data[payload_index];
}

int rocketnet_client_payloads( rocketnet_client_t * client_transport, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( payload_data );
    rocketnet_assert( payload_bytes );

    *payload_data = client_transport->received_packets.packet_data;
    *payload_bytes = client_transport->received_packets.packet_bytes;

    return client_transport->received_packets.num_packets;
}

int rocketnet_client_state( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );
    return client_transport->protocol.state;
}

int rocketnet_client_client_index( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );
    return client_transport->protocol.client_index;
}

void rocketnet_client_disconnect( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );

    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES];
    int burst_count = 0;
    rocketnet_client_protocol_disconnect( &client_transport->protocol, burst, &burst_count );

    for ( int i = 0; i < burst_count; i++ )
        rocketnet_platform_socket_send_packet( client_transport->socket, &client_transport->protocol.server_address,
                                               burst + i * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES,
                                               ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES );
}

const struct rocketnet_counters_t * rocketnet_client_counters( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );
    return &client_transport->counters;
}

static void rocketnet_client_receive_packets( rocketnet_client_t * client_transport )
{
    rocketnet_assert( client_transport );

    rocketnet_platform_mutex_acquire( &client_transport->mutex );

    const int off_index = client_transport->receive_buffer_index ? 0 : 1;

    rocketnet_client_receive_buffer_t * receive_buffer = &client_transport->receive_buffer[off_index];

    while ( 1 )
    {
        if ( receive_buffer->num_packets >= ROCKETNET_NUM_CLIENT_PACKETS )
            break;

        uint8_t * packet_data = receive_buffer->data + receive_buffer->num_packets * ROCKETNET_FRAME_SIZE;

        struct rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( client_transport->socket, &from, packet_data, ROCKETNET_MAX_PACKET_BYTES );
        if ( packet_bytes == 0 )
            break;

        // a cheap size sanity on the receive thread; the full checks and
        // their counters run on the update thread

        if ( packet_bytes < ROCKETNET_PREFILTER_BYTES || packet_bytes > ROCKETNET_RECEIVE_CEILING )
            continue;

        const int index = receive_buffer->num_packets;

        receive_buffer->from[index] = from;
        receive_buffer->receive_time[index] = rocketnet_platform_time();
        receive_buffer->packet_data[index] = packet_data;
        receive_buffer->packet_bytes[index] = packet_bytes;

        receive_buffer->num_packets++;
    }

    rocketnet_platform_mutex_release( &client_transport->mutex );
}

static void rocketnet_client_thread_function( void * data )
{
    rocketnet_client_t * client_transport = (rocketnet_client_t*) data;

    while ( !client_transport->quit )
    {
        rocketnet_client_receive_packets( client_transport );

        rocketnet_platform_sleep( 0.001 );
    }
}

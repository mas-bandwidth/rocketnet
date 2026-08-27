/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_client_transport.h"
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

struct rocketnet_client_transport_receive_buffer_t
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

struct rocketnet_client_transport_received_packets_t
{
    int num_packets;
    int receive_index;
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    int packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_NUM_CLIENT_PACKETS];
};

struct rocketnet_client_transport_t
{
    void * context;

    uint16_t bound_port;

    rocketnet_platform_socket_t * socket;

    rocketnet_platform_thread_t * thread;
    rocketnet_platform_mutex_t mutex;
    std::atomic<bool> quit;

    int receive_buffer_index;
    rocketnet_client_transport_received_packets_t received_packets;
    rocketnet_client_transport_receive_buffer_t receive_buffer[2];

    struct rocketnet_counters_t counters;
    struct rocketnet_client_protocol_t protocol;
};

static void rocketnet_client_transport_thread_function( void * data );

rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const char * connect_address_string )
{
    rocketnet_assert( connect_address_string );

    const char * connect_address_env = getenv( "ROCKETNET_CLIENT_CONNECT_ADDRESS" );
    if ( connect_address_env )
    {
        connect_address_string = connect_address_env;
    }

    rocketnet_address_t server_address;
    if ( !rocketnet_address_parse( &server_address, connect_address_string ) )
    {
        rocketnet_error( "client could not parse connect address" );
        return NULL;
    }

    char string_buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client connecting to %s (development token)", rocketnet_address_to_string( &server_address, string_buffer ) );

    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t*) rocketnet_malloc( context, sizeof(rocketnet_client_transport_t) );
    if ( !client_transport )
    {
        rocketnet_error( "could not allocate client" );
        return NULL;
    }

    memset( (char*) client_transport, 0, sizeof(rocketnet_client_transport_t) );

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
        rocketnet_client_transport_destroy( client_transport );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client bound to %s", rocketnet_address_to_string( &bind_address, address_string ) );

    client_transport->bound_port = bind_address.port;

    // the development connect: mint a token for the dialed address with the
    // published development keys and start the handshake. the transport owns
    // the real clocks; the protocol never reads one.

    rocketnet_client_protocol_init( &client_transport->protocol, &client_transport->counters );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    uint64_t client_id;
    rocketnet_random_bytes( (uint8_t*) &client_id, 8 );
    if ( !rocketnet_write_dev_connect_token( connect_address_string, client_id, (uint64_t) time( NULL ), connect_token ) )
    {
        rocketnet_error( "client could not mint dev connect token" );
        rocketnet_client_transport_destroy( client_transport );
        return NULL;
    }

    if ( !rocketnet_client_connect( &client_transport->protocol, connect_token, rocketnet_platform_time(), (uint64_t) time( NULL ) ) )
    {
        rocketnet_client_transport_destroy( client_transport );
        return NULL;
    }

    if ( !rocketnet_platform_mutex_create( &client_transport->mutex ) )
    {
        rocketnet_error( "client could not create mutex" );
        rocketnet_client_transport_destroy( client_transport );
        return NULL;
    }

    client_transport->thread = rocketnet_platform_thread_create( NULL, rocketnet_client_transport_thread_function, client_transport );
    if ( !client_transport->thread )
    {
        rocketnet_error( "client could not create thread" );
        rocketnet_client_transport_destroy( client_transport );
        return NULL;
    }

    return client_transport;
}

void rocketnet_client_transport_destroy( rocketnet_client_transport_t * client_transport )
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

    rocketnet_clear_and_free( client_transport->context, client_transport, sizeof(rocketnet_client_transport_t) );
}

static void rocketnet_client_transport_process_packet( rocketnet_client_transport_t * client_transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes, double current_time )
{
    // only the dialed server is listened to; everything else is junk

    if ( !rocketnet_address_equal( from, &client_transport->protocol.server_address ) )
        return;

    struct rocketnet_client_transport_received_packets_t * received = &client_transport->received_packets;

    uint8_t * payload = received->num_packets < ROCKETNET_NUM_CLIENT_PACKETS ?
        received->data + received->num_packets * ROCKETNET_FRAME_SIZE : NULL;

    uint8_t overflow[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    if ( rocketnet_client_process_packet( &client_transport->protocol, packet_data, packet_bytes, current_time,
                                          payload ? payload : overflow, &payload_bytes ) &&
         payload_bytes > 0 && payload )
    {
        const int index = received->num_packets;
        received->packet_data[index] = payload;
        received->packet_bytes[index] = payload_bytes;
        received->num_packets++;
    }
}

static void rocketnet_client_transport_update_swap_receive_buffers( rocketnet_client_transport_t * client_transport )
{
    rocketnet_platform_mutex_acquire( &client_transport->mutex );

    client_transport->receive_buffer_index++;
    client_transport->receive_buffer_index &= 1;

    const int off_index = client_transport->receive_buffer_index ? 0 : 1;

    client_transport->receive_buffer[off_index].num_packets = 0;

    rocketnet_platform_mutex_release( &client_transport->mutex );
}

void rocketnet_client_transport_update( rocketnet_client_transport_t * client_transport )
{
    rocketnet_assert( client_transport );

    const double current_time = rocketnet_platform_time();

    rocketnet_client_transport_update_swap_receive_buffers( client_transport );

    struct rocketnet_client_transport_receive_buffer_t * receive_buffer = &client_transport->receive_buffer[client_transport->receive_buffer_index];

    client_transport->received_packets.num_packets = 0;
    client_transport->received_packets.receive_index = 0;

    for ( int i = 0; i < receive_buffer->num_packets; i++ )
    {
        rocketnet_client_transport_process_packet( client_transport, &receive_buffer->from[i], receive_buffer->packet_data[i], (int) receive_buffer->packet_bytes[i], current_time );
    }

    receive_buffer->num_packets = 0;

    // the protocol's scheduled send and its deadlines

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_update( &client_transport->protocol, current_time, packet, &packet_bytes );
    if ( packet_bytes > 0 )
        rocketnet_platform_socket_send_packet( client_transport->socket, &client_transport->protocol.server_address, packet, packet_bytes );
}

void rocketnet_client_transport_send_packet( rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int wire_bytes = rocketnet_client_write_payload( &client_transport->protocol, packet_data, packet_bytes, rocketnet_platform_time(), packet );
    if ( wire_bytes == 0 )
        return;

    rocketnet_platform_socket_send_packet( client_transport->socket, &client_transport->protocol.server_address, packet, wire_bytes );
}

int rocketnet_client_transport_receive_packet( rocketnet_client_transport_t * client_transport, uint8_t * packet_data )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( packet_data );

    struct rocketnet_client_transport_received_packets_t * received = &client_transport->received_packets;

    if ( received->receive_index >= received->num_packets )
        return 0;

    const int index = received->receive_index++;
    memcpy( packet_data, received->packet_data[index], (size_t) received->packet_bytes[index] );

    return received->packet_bytes[index];
}

int rocketnet_client_transport_state( rocketnet_client_transport_t * client_transport )
{
    rocketnet_assert( client_transport );
    return client_transport->protocol.state;
}

int rocketnet_client_transport_client_index( rocketnet_client_transport_t * client_transport )
{
    rocketnet_assert( client_transport );
    return client_transport->protocol.client_index;
}

void rocketnet_client_transport_disconnect( rocketnet_client_transport_t * client_transport )
{
    rocketnet_assert( client_transport );

    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES];
    int burst_count = 0;
    rocketnet_client_disconnect( &client_transport->protocol, burst, &burst_count );

    for ( int i = 0; i < burst_count; i++ )
        rocketnet_platform_socket_send_packet( client_transport->socket, &client_transport->protocol.server_address,
                                               burst + i * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES,
                                               ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES );
}

const struct rocketnet_counters_t * rocketnet_client_transport_counters( rocketnet_client_transport_t * client_transport )
{
    rocketnet_assert( client_transport );
    return &client_transport->counters;
}

static void rocketnet_client_transport_receive_packets( rocketnet_client_transport_t * client_transport )
{
    rocketnet_assert( client_transport );

    rocketnet_platform_mutex_acquire( &client_transport->mutex );

    const int off_index = client_transport->receive_buffer_index ? 0 : 1;

    rocketnet_client_transport_receive_buffer_t * receive_buffer = &client_transport->receive_buffer[off_index];

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

static void rocketnet_client_transport_thread_function( void * data )
{
    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t*) data;

    while ( !client_transport->quit )
    {
        rocketnet_client_transport_receive_packets( client_transport );

        rocketnet_platform_sleep( 0.001 );
    }
}

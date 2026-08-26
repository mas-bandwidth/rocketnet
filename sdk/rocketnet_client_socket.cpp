/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_client_socket.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_packet_filter.h"
#include "rocketnet_packets.h"

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <atomic>

struct rocketnet_client_socket_receive_buffer_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_NUM_CLIENT_PACKETS];
    double receive_time[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    size_t packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_NUM_CLIENT_PACKETS];
};

struct rocketnet_client_socket_received_packets_t
{
    int num_packets;
    int receive_index;
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    int packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
};

struct rocketnet_client_socket_t
{
    void * context;

    int state;

    uint16_t bound_port;

    rocketnet_address_t server_address;

    rocketnet_platform_socket_t * socket;

    rocketnet_platform_thread_t * thread;
    rocketnet_platform_mutex_t mutex;
    std::atomic<bool> quit;

    int receive_buffer_index;
    double last_packet_receive_time;
    rocketnet_client_socket_received_packets_t received_packets;
    rocketnet_client_socket_receive_buffer_t receive_buffer[2];
};

void rocketnet_client_socket_connection_timed_out( rocketnet_client_socket_t * client_socket )
{
    rocketnet_warn( "client connection timed out" );
}

void rocketnet_client_socket_connected( rocketnet_client_socket_t * client_socket )
{
    rocketnet_info( "client connected" );
}

void rocketnet_client_socket_disconnected( rocketnet_client_socket_t * client_socket )
{
    rocketnet_info( "client disconnected" );
}

static void rocketnet_client_socket_thread_function( void * data );

rocketnet_client_socket_t * rocketnet_client_socket_create( void * context, const char * connect_address_string )
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
    rocketnet_info( "client connecting to %s", rocketnet_address_to_string( &server_address, string_buffer ) );

    rocketnet_client_socket_t * client_socket = (rocketnet_client_socket_t*) rocketnet_malloc( context, sizeof(rocketnet_client_socket_t) );
    if ( !client_socket )
    {
        rocketnet_error( "could not allocate client" );
        return NULL;
    }

    memset( (char*) client_socket, 0, sizeof(rocketnet_client_socket_t) );

    const uint64_t current_time = rocketnet_platform_time();

    client_socket->context = context;

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

    client_socket->socket = rocketnet_platform_socket_create( client_socket->context, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    if ( client_socket->socket == NULL )
    {
        rocketnet_error( "client could not create socket" );
        rocketnet_client_socket_destroy( client_socket );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client bound to %s", rocketnet_address_to_string( &bind_address, address_string ) );

    client_socket->bound_port = bind_address.port;

    client_socket->server_address = server_address;
    client_socket->state = ROCKETNET_CLIENT_SOCKET_CONNECTED;
    client_socket->last_packet_receive_time = current_time;
    rocketnet_client_socket_connected( client_socket );

    if ( !rocketnet_platform_mutex_create( &client_socket->mutex ) )
    {
        rocketnet_error( "client could not create mutex" );
        rocketnet_client_socket_destroy( client_socket );
        return NULL;
    }

    client_socket->thread = rocketnet_platform_thread_create( NULL, rocketnet_client_socket_thread_function, client_socket );
    if ( !client_socket->thread )
    {
        rocketnet_error( "client could not create thread" );
        rocketnet_client_socket_destroy( client_socket );
        return NULL;
    }

    return client_socket;
}

void rocketnet_client_socket_destroy( rocketnet_client_socket_t * client_socket )
{
    // IMPORTANT: Please call disconnect and wait for the client to disconnect before destroying the client
    rocketnet_assert( client_socket->state <= ROCKETNET_CLIENT_SOCKET_DISCONNECTED );

    if ( client_socket->thread )
    {
        client_socket->quit = true;
        rocketnet_platform_thread_join( client_socket->thread );
        rocketnet_platform_thread_destroy( client_socket->thread );
    }

    if ( client_socket->socket )
    {
        rocketnet_platform_socket_destroy( client_socket->socket );
    }

    rocketnet_platform_mutex_destroy( &client_socket->mutex );

    rocketnet_clear_and_free( client_socket->context, client_socket, sizeof(rocketnet_client_socket_t) );
}

void rocketnet_client_socket_send_packet_internal( rocketnet_client_socket_t * client_socket, rocketnet_address_t * to_address, uint8_t * packet_data, int packet_bytes )
{
    uint8_t to_address_data[32];
    rocketnet_address_data( to_address, to_address_data );

    rocketnet_address_t from_address;
    memset( &from_address, 0, sizeof(from_address) );
    from_address.type = ROCKETNET_ADDRESS_IPV4;

    uint8_t from_address_data[32];
    rocketnet_address_data( &from_address, from_address_data );

    uint8_t * a = packet_data + 1;
    uint8_t * b = packet_data + 3;

    uint8_t magic[8];
    memset( magic, 0, sizeof(magic) );

    rocketnet_generate_pittle( a, from_address_data, to_address_data, packet_bytes );
    rocketnet_generate_chonkle( b, magic, from_address_data, to_address_data, packet_bytes );

    rocketnet_platform_socket_send_packet( client_socket->socket, to_address, packet_data, packet_bytes );
}

void rocketnet_client_socket_update_timeout( rocketnet_client_socket_t * client_socket )
{
    if ( client_socket->state != ROCKETNET_CLIENT_SOCKET_CONNECTED )
        return;

    if ( client_socket->last_packet_receive_time + ROCKETNET_DIRECT_TIMEOUT < rocketnet_platform_time() )
    {
        client_socket->state = ROCKETNET_CLIENT_SOCKET_CONNECTION_TIMED_OUT;
        rocketnet_client_socket_connection_timed_out( client_socket );
    }
}

void rocketnet_client_socket_process_packet( rocketnet_client_socket_t * client_socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes )
{
    // we only support ipv4 at the moment

    if ( from->type != ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_warn( "ignored packet from non-ipv4 address" );
        return;
    }

    // basic packet filter

    if ( !rocketnet_basic_packet_filter( packet_data, packet_bytes ) )
    {
        rocketnet_warn( "basic packet filter dropped packet" );
        return;
    }

#if ROCKETNET_ADVANCED_PACKET_FILTER

    // todo: advanced packet filter

#endif // #if ROCKETNET_ADVANCED_PACKET_FILTER

    // process packet type

    const uint8_t packet_type = packet_data[0];

    if ( client_socket->state != ROCKETNET_CLIENT_SOCKET_CONNECTED )
        return;

    if ( packet_type == ROCKETNET_PACKET_DIRECT && rocketnet_address_equal( from, &client_socket->server_address ) && packet_bytes > ROCKETNET_HEADER_BYTES )
    {
        client_socket->last_packet_receive_time = rocketnet_platform_time();

        const int index = client_socket->received_packets.num_packets;

        if ( index < ROCKETNET_NUM_CLIENT_PACKETS )
        {
            client_socket->received_packets.packet_data[index] = packet_data + ROCKETNET_HEADER_BYTES;
            client_socket->received_packets.packet_bytes[index] = packet_bytes - ROCKETNET_HEADER_BYTES;
            client_socket->received_packets.num_packets++;
        }
    }
}

void rocketnet_client_socket_update_swap_receive_buffers( rocketnet_client_socket_t * client_socket )
{
    rocketnet_platform_mutex_acquire( &client_socket->mutex );

    client_socket->receive_buffer_index++;
    client_socket->receive_buffer_index &= 1;

    const int off_index = client_socket->receive_buffer_index ? 0 : 1;

    client_socket->receive_buffer[off_index].num_packets = 0;

    rocketnet_platform_mutex_release( &client_socket->mutex );
}

void rocketnet_client_socket_update_process_packets( rocketnet_client_socket_t * client_socket )
{
    rocketnet_client_socket_receive_buffer_t * receive_buffer = &client_socket->receive_buffer[client_socket->receive_buffer_index];

    client_socket->received_packets.num_packets = 0;
    client_socket->received_packets.receive_index = 0;

    const int num_packets = receive_buffer->num_packets;

    for ( int i = 0; i < num_packets; i++ )
    {
        rocketnet_client_socket_process_packet( client_socket, &receive_buffer->from[i], receive_buffer->packet_data[i], receive_buffer->packet_bytes[i] );
    }

    receive_buffer->num_packets = 0;
}

void rocketnet_client_socket_receive_packets( rocketnet_client_socket_t * client_socket );

void rocketnet_client_socket_update( rocketnet_client_socket_t * client_socket )
{
    rocketnet_assert( client_socket );

    rocketnet_client_socket_receive_packets( client_socket );

    rocketnet_client_socket_update_swap_receive_buffers( client_socket );

    rocketnet_client_socket_update_process_packets( client_socket );

    rocketnet_client_socket_update_timeout( client_socket );
}

void rocketnet_client_socket_send_packet( rocketnet_client_socket_t * client_socket, const uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( client_socket );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_MTU );

    if ( client_socket->state != ROCKETNET_CLIENT_SOCKET_CONNECTED )
        return;

    uint8_t direct_packet_data[ROCKETNET_MAX_PACKET_BYTES];
    direct_packet_data[0] = ROCKETNET_PACKET_DIRECT;
    memcpy( direct_packet_data + ROCKETNET_HEADER_BYTES, packet_data, packet_bytes );
    rocketnet_client_socket_send_packet_internal( client_socket, &client_socket->server_address, direct_packet_data, ROCKETNET_HEADER_BYTES + packet_bytes );
}

int rocketnet_client_socket_receive_packet( rocketnet_client_socket_t * client_socket, uint8_t * packet_data )
{
    rocketnet_assert( client_socket );
    rocketnet_assert( packet_data );

    const int receive_index = client_socket->received_packets.receive_index;

    const int num_packets = client_socket->received_packets.num_packets;

    if ( receive_index >= num_packets )
        return 0;

    const int packet_bytes = client_socket->received_packets.packet_bytes[receive_index];

    rocketnet_assert( packet_bytes >= 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_MTU );

    memcpy( packet_data, client_socket->received_packets.packet_data[receive_index], packet_bytes );

    client_socket->received_packets.receive_index++;

    return packet_bytes;
}

void rocketnet_client_socket_disconnect( rocketnet_client_socket_t * client_socket )
{
    rocketnet_assert( client_socket );

    client_socket->state = ROCKETNET_CLIENT_SOCKET_DISCONNECTED;

    rocketnet_client_socket_disconnected( client_socket );
}

int rocketnet_client_socket_state( rocketnet_client_socket_t * client_socket )
{
    rocketnet_assert( client_socket );
    return client_socket->state;
}

void rocketnet_client_socket_receive_packets( rocketnet_client_socket_t * client_socket )
{
    rocketnet_assert( client_socket );

    rocketnet_platform_mutex_acquire( &client_socket->mutex );

    const int off_index = client_socket->receive_buffer_index ? 0 : 1;

    rocketnet_client_socket_receive_buffer_t * receive_buffer = &client_socket->receive_buffer[off_index];

    while ( 1 )
    {
        if ( receive_buffer->num_packets >= ROCKETNET_NUM_CLIENT_PACKETS )
            break;

        uint8_t * packet_data = receive_buffer->data + receive_buffer->num_packets * ROCKETNET_FRAME_SIZE;

        struct rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( client_socket->socket, &from, packet_data, ROCKETNET_MAX_PACKET_BYTES );
        if ( packet_bytes == 0 )
            break;

        double receive_time = rocketnet_platform_time();

        const uint8_t packet_type = packet_data[0];

        const int index = receive_buffer->num_packets;

        if ( packet_type == ROCKETNET_PACKET_DIRECT && packet_bytes < ROCKETNET_HEADER_BYTES )
            continue;

        receive_buffer->from[index] = from;
        receive_buffer->receive_time[index] = receive_time;
        receive_buffer->packet_data[index] = packet_data;
        receive_buffer->packet_bytes[index] = packet_bytes;

        receive_buffer->num_packets++;
    }

    rocketnet_platform_mutex_release( &client_socket->mutex );
}

static void rocketnet_client_socket_thread_function( void * data )
{
    rocketnet_client_socket_t * client_socket = (rocketnet_client_socket_t*) data;

    while ( !client_socket->quit )
    {
        rocketnet_client_socket_receive_packets( client_socket );

        rocketnet_platform_sleep( 0.001 );
    }
}

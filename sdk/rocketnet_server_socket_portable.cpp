/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"

#if ROCKETNET_XDP == 0

#include "rocketnet_server_socket.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_packet_filter.h"
#include "rocketnet_hash.h"

#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>

struct rocketnet_server_socket_send_buffer_t
{
    std::atomic<int> num_packets;
    rocketnet_address_t to[ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS];
    size_t packet_bytes[ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS];
    uint8_t packet_type[ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS];
};

struct rocketnet_server_socket_receive_buffer_t
{
    int current_packet;
    rocketnet_address_t from[ROCKETNET_SERVER_SOCKET_MAX_RECEIVE_PACKETS];
    uint8_t * packet_data[ROCKETNET_SERVER_SOCKET_MAX_RECEIVE_PACKETS];
    size_t packet_bytes[ROCKETNET_SERVER_SOCKET_MAX_RECEIVE_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_SERVER_SOCKET_MAX_RECEIVE_PACKETS];
};

struct rocketnet_server_socket_t
{
    void * context;
    int state;
    rocketnet_address_t bind_address;
    rocketnet_address_t public_address;
    uint64_t server_id;
    uint64_t match_id;

    std::atomic<uint64_t> packet_id;

    rocketnet_platform_socket_t * socket;
    rocketnet_server_socket_send_buffer_t send_buffer;
    rocketnet_server_socket_receive_buffer_t receive_buffer;
    rocketnet_server_socket_process_packets_t process_packets;
};

void rocketnet_server_socket_destroy( rocketnet_server_socket_t * server_socket );

rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * public_address_string, int num_xdp_queues, int num_os_cpus )
{
    (void) num_xdp_queues;  // not used
    (void) num_os_cpus;     // not used

    rocketnet_assert( public_address_string );

    const char * public_address_env = getenv( "ROCKETNET_SERVER_PUBLIC_ADDRESS" );
    if ( public_address_env )
    {
        public_address_string = public_address_env;
    }

    rocketnet_info( "server socket public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        rocketnet_error( "could not parse public address" );
        return NULL;
    }

    if ( public_address.type != ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_error( "we only support ipv4 servers at the moment" );
        return NULL;
    }

    rocketnet_address_t bind_address = public_address;
    bind_address.data.ipv4[0] = 0;
    bind_address.data.ipv4[1] = 0;
    bind_address.data.ipv4[2] = 0;
    bind_address.data.ipv4[3] = 0;

    rocketnet_server_socket_t * server_socket = (rocketnet_server_socket_t*) rocketnet_malloc( context, sizeof(rocketnet_server_socket_t) );
    if ( !server_socket )
        return NULL;

    memset( server_socket, 0, sizeof( rocketnet_server_socket_t) );
    
    server_socket->context = context;

    server_socket->socket = rocketnet_platform_socket_create( server_socket->context, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    if ( server_socket->socket == NULL )
    {
        rocketnet_error( "could not create server socket" );
        rocketnet_server_socket_destroy( server_socket );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server socket started on %s", rocketnet_address_to_string( &bind_address, address_string ) );

    server_socket->bind_address = bind_address;
    server_socket->public_address = public_address;
    server_socket->state = ROCKETNET_SERVER_SOCKET_RUNNING;
    server_socket->server_id = rocketnet_hash_string( public_address_string );
    server_socket->match_id = rocketnet_random_uint64();

    rocketnet_info( "server id is %016" PRIx64, server_socket->server_id );
    rocketnet_info( "match id is %016" PRIx64, server_socket->match_id );

    return server_socket;    
}

void rocketnet_server_socket_destroy( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( server_socket->state == ROCKETNET_SERVER_SOCKET_STOPPED );        // IMPORTANT: Please stop the server and wait until state is ROCKETNET_SERVER_STOPPED before destroying it

    if ( server_socket->socket )
    {
        rocketnet_platform_socket_destroy( server_socket->socket );
    }

    rocketnet_clear_and_free( server_socket->context, server_socket, sizeof(rocketnet_server_socket_t) );
}

void rocketnet_server_socket_update( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    if ( server_socket->state == ROCKETNET_SERVER_SOCKET_STOPPING )
    {
        server_socket->state = ROCKETNET_SERVER_SOCKET_STOPPED;
    }
}

void rocketnet_server_socket_stop( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    server_socket->state = ROCKETNET_SERVER_SOCKET_STOPPING;
}

int rocketnet_server_socket_state( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return server_socket->state;
}

uint64_t rocketnet_server_socket_id( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return server_socket->server_id;
}

uint8_t * rocketnet_server_socket_start_packet_internal( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * to, uint8_t packet_type, uint64_t * packet_id )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( to );

    const int packet_index = server_socket->send_buffer.num_packets.fetch_add( 1 );

    if ( packet_index >= ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS )
        return NULL;

    uint8_t * packet_data = server_socket->send_buffer.data + packet_index * ROCKETNET_FRAME_SIZE;

    packet_data += ROCKETNET_HEADER_BYTES;

    server_socket->send_buffer.to[packet_index] = *to;
    server_socket->send_buffer.packet_type[packet_index] = packet_type;
    server_socket->send_buffer.packet_bytes[packet_index] = 0;

    *packet_id = server_socket->packet_id.fetch_add(1);

    return packet_data;
}

uint8_t * rocketnet_server_socket_start_packet( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( to );
    rocketnet_assert( to->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_id );

    // direct packet

    uint8_t * packet_data = rocketnet_server_socket_start_packet_internal( server_socket, to, ROCKETNET_PACKET_DIRECT, packet_id );
    if ( !packet_data )
        return NULL;

    return packet_data;
}

void rocketnet_server_socket_finish_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( packet_bytes >= 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_MTU );

    (void) packet_id;

    size_t offset = ( packet_data - server_socket->send_buffer.data );

    offset -= offset % ROCKETNET_FRAME_SIZE;

    const int packet_index = (int) ( offset / ROCKETNET_FRAME_SIZE );

    rocketnet_assert( packet_index >= 0 );  
    rocketnet_assert( packet_index < ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS );  

    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_MTU );

    server_socket->send_buffer.packet_bytes[packet_index] = packet_bytes + ROCKETNET_HEADER_BYTES;

    // write the packet header

    packet_data -= ROCKETNET_HEADER_BYTES;

    packet_data[0] = server_socket->send_buffer.packet_type[packet_index];

    uint8_t to_address_data[32];
    rocketnet_address_data( &server_socket->send_buffer.to[packet_index], to_address_data );

    uint8_t from_address_data[32];
    rocketnet_address_data( &server_socket->public_address, from_address_data );

    uint8_t * a = packet_data + 1;
    uint8_t * b = packet_data + 3;

    uint8_t magic[8];
    memset( magic, 0, sizeof(magic) );

    rocketnet_generate_pittle( a, from_address_data, to_address_data, packet_bytes );
    rocketnet_generate_chonkle( b, magic, from_address_data, to_address_data, packet_bytes );
}

void rocketnet_server_socket_abort_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( server_socket );

    (void) packet_id;

    size_t offset = ( packet_data - server_socket->send_buffer.data );

    offset -= offset % ROCKETNET_FRAME_SIZE;

    const int packet_index = (int) ( offset / ROCKETNET_FRAME_SIZE );

    rocketnet_assert( packet_index >= 0 );  
    rocketnet_assert( packet_index < ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS );  

    server_socket->send_buffer.packet_bytes[packet_index] = 0;
}

void rocketnet_server_socket_send_packets( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    int num_packets = server_socket->send_buffer.num_packets;
    if ( num_packets > ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS )
    {
        num_packets = ROCKETNET_SERVER_SOCKET_MAX_SEND_PACKETS;
    }

    for ( int i = 0; i < num_packets; i++ )
    {
        uint8_t * packet_data = server_socket->send_buffer.data + i * ROCKETNET_FRAME_SIZE;

        const int packet_bytes = (int) server_socket->send_buffer.packet_bytes[i];

        if ( packet_bytes > 0 )
        {
            rocketnet_assert( packet_data );
            rocketnet_assert( packet_bytes <= ROCKETNET_MAX_PACKET_BYTES );
            rocketnet_platform_socket_send_packet( server_socket->socket, &server_socket->send_buffer.to[i], packet_data, packet_bytes );
        }
    }

    server_socket->send_buffer.num_packets = 0;
}

void rocketnet_server_socket_process_packet_internal( rocketnet_server_socket_t * server_socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes )
{
    const uint8_t packet_type = packet_data[0];

    // ...

    (void) packet_type;
}

void rocketnet_server_socket_process_direct_packet( rocketnet_server_socket_t * server_socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes )
{
    if ( packet_bytes < ROCKETNET_HEADER_BYTES )
        return;

    if ( server_socket->process_packets.num_packets == ROCKETNET_SERVER_SOCKET_MAX_RECEIVE_PACKETS )
        return;

    const int index = server_socket->process_packets.num_packets++;

    packet_data += ROCKETNET_HEADER_BYTES;
    packet_bytes -= ROCKETNET_HEADER_BYTES;

    rocketnet_assert( packet_bytes >= 0 );

    server_socket->process_packets.from[index] = *from;
    server_socket->process_packets.packet_data[index] = packet_data;
    server_socket->process_packets.packet_bytes[index] = packet_bytes;
}

void rocketnet_server_socket_receive_packets( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    server_socket->process_packets.num_packets = 0;

    server_socket->receive_buffer.current_packet = 0;

    while ( 1 )
    {
        if ( server_socket->receive_buffer.current_packet >= ROCKETNET_SERVER_SOCKET_MAX_RECEIVE_PACKETS )
            break;

        uint8_t * packet_data = server_socket->receive_buffer.data + ROCKETNET_FRAME_SIZE * server_socket->receive_buffer.current_packet;

        struct rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( server_socket->socket, &from, packet_data, ROCKETNET_MAX_PACKET_BYTES );
        if ( packet_bytes == 0 )
            break;

        const uint8_t packet_type = packet_data[0];

        // basic packet filter

        if ( !rocketnet_basic_packet_filter( packet_data, packet_bytes ) )
        {
            rocketnet_debug( "basic packet filter dropped packet" );
            continue;
        }


        if ( packet_type == ROCKETNET_PACKET_DIRECT )
        {  
            rocketnet_server_socket_process_direct_packet( server_socket, &from, packet_data, packet_bytes );
        }
        else
        {
            rocketnet_server_socket_process_packet_internal( server_socket, &from, packet_data, packet_bytes );            
        }

        server_socket->receive_buffer.current_packet++;
    }
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    return &server_socket->process_packets;
}

int rocketnet_server_socket_num_queues( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return 0;
}

#else // #if ROCKETNET_XDP == 0

int rocketnet_server_socket_portable_cpp_dummy = 0;

#endif // #if ROCKETNET_XDP == 0

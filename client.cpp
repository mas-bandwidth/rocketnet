/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_client_socket.h"
#include "rocketnet_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile int quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

static inline int generate_packet( uint8_t * packet_data, int min_size, int max_size )
{
    const int packet_bytes = min_size + rand() % ( max_size - min_size );
    const int start = packet_bytes % 256;
    for ( int i = 0; i < packet_bytes; i++ )
    {
        packet_data[i] = (uint8_t) ( start + i ) % 256;
    }
    return packet_bytes;
}

static inline bool verify_packet( uint8_t * packet_data, int packet_bytes )
{
    const int start = packet_bytes % 256;
    for ( int i = 0; i < packet_bytes; i++ )
    {
        if ( packet_data[i] != (uint8_t) ( ( start + i ) % 256 ) )
            return false;
    }
    return true;
}

int main()
{
    signal( SIGINT, interrupt_handler ); signal( SIGTERM, interrupt_handler );

    if ( !rocketnet_init() )
    {
        rocketnet_error( "could not initialize rocketnet" );
        return 1;        
    }

    const char * connect = "69.67.149.151:40000";       // space1

    // const char * connect = "64.34.88.117:40000";        // space2

    // const char * connect = "127.0.0.1:40000";

    rocketnet_client_socket_t * client_socket = rocketnet_client_socket_create( NULL, connect );
    if ( !client_socket )
    {
        rocketnet_error( "could not create client socket" );
        return 1;
    }

    rocketnet_info( "connecting..." );

    bool previous_connected = false;

    int count = 0;

    while ( !quit )
    {
        if ( rocketnet_client_socket_state( client_socket ) <= ROCKETNET_CLIENT_SOCKET_DISCONNECTED )
            break;

        rocketnet_client_socket_update( client_socket );

        while ( true )
        {
            uint8_t packet_data[ROCKETNET_MTU];
            int packet_bytes = rocketnet_client_socket_receive_packet( client_socket, packet_data );
            if ( packet_bytes == 0 )
                break;

            rocketnet_info( "client received %d byte packet from server", packet_bytes );

            if ( !verify_packet( packet_data, packet_bytes ) )
            {
                rocketnet_warn( " ---> packet did not verify" );
            }
        }

        for ( int i = 0; i < 10; i++ )
        {
            uint8_t packet_data[256];
            const int packet_bytes = generate_packet( packet_data, 100, 256 );
            rocketnet_client_socket_send_packet( client_socket, packet_data, packet_bytes );
        }

        if ( !previous_connected )
        {
            if ( rocketnet_client_socket_state( client_socket ) == ROCKETNET_CLIENT_SOCKET_CONNECTED )
            {
                rocketnet_info( "connected" );

                previous_connected = true;
            }
        }

        rocketnet_platform_sleep( 1.0 / 100.0 );
    }

    rocketnet_info( "disconnecting" );

    rocketnet_client_socket_disconnect( client_socket );

    while ( rocketnet_client_socket_state( client_socket ) > ROCKETNET_CLIENT_SOCKET_DISCONNECTED )
    {
        rocketnet_client_socket_update( client_socket );
    }

    rocketnet_info( "disconnected" );

    rocketnet_client_socket_destroy( client_socket );

    rocketnet_term();

    return 0;
}

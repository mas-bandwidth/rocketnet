/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_client_socket.h"
#include "rocketnet_base64.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_hydrogen.h"
#include "rocketnet_platform.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

#define CLIENT_DIRECT 1

const uint8_t buyer_public_key[] = { 0x9d, 0x59, 0x40, 0xa4, 0xe2, 0x4a, 0xa3, 0x0a, 0xf2, 0x30, 0xb6, 0x1b, 0x49, 0x7d, 0x60, 0xe8, 0x6d, 0xf9, 0x03, 0x28, 0x5c, 0x96, 0x83, 0x06, 0x89, 0xf5, 0xdd, 0x62, 0x8a, 0x25, 0x95, 0x16 };

const uint8_t buyer_private_key[] = { 0x7b, 0xd7, 0x97, 0x88, 0xcb, 0x67, 0x03, 0xac, 0xdc, 0x75, 0x6d, 0x7a, 0xcc, 0x03, 0x88, 0x67, 0xd4, 0xc6, 0xe8, 0xb5, 0x99, 0x42, 0x77, 0xd9, 0x68, 0xb3, 0xf6, 0xbd, 0xa5, 0xd3, 0x55, 0xfa, 0x9d, 0x59, 0x40, 0xa4, 0xe2, 0x4a, 0xa3, 0x0a, 0xf2, 0x30, 0xb6, 0x1b, 0x49, 0x7d, 0x60, 0xe8, 0x6d, 0xf9, 0x03, 0x28, 0x5c, 0x96, 0x83, 0x06, 0x89, 0xf5, 0xdd, 0x62, 0x8a, 0x25, 0x95, 0x16 };

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

#if CLIENT_DIRECT

    const char * connect = "69.67.149.151:40000";       // space1

    // const char * connect = "64.34.88.117:40000";        // space2

    // const char * connect = "127.0.0.1:40000";

#else // #if CLIENT_DIRECT

    char connect[ROCKETNET_MAX_CONNECT_TOKEN_BYTES];
    memset( connect, 0, sizeof(connect) );
    {
        rocketnet_connect_token_t token;
        memset( &token, 0, sizeof(token) );
        token.expire_timestamp = rocketnet_random_uint64();
        token.buyer_id = 0x12345678;
        token.server_id = rocketnet_random_uint64();
        token.rocketnet_id = rocketnet_random_uint64();
        token.user_hash = rocketnet_random_uint64();
        token.client_public_address = 0xa89d4f2d;               // 45.79.157.168  (home ip address)
        token.backend_addresses[0] = 0xf3fdfa2d;                // 45.250.253.243 (latitude.newyork)
        token.backend_ports[0] = rocketnet_platform_htons( 40000 );
        token.pings_per_second = 10;
        token.pongs_before_select = 10;
        token.max_connect_seconds = 30;
        token.backend_token_refresh_seconds = 30;
        if ( !rocketnet_write_connect_token( &token, connect, buyer_private_key ) )
        {
            rocketnet_error( "failed to write connect token" );
            return 1;        
        }
    }

#endif // #if CLIENT_DIRECT

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

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The out-of-the-box server: runs with the published development keys, so
    a first server needs no backend and no configuration -- run it, point a
    client at it, connected. Echoes every payload back to its sender. Pass
    the public address clients dial: ./bin/server 192.0.2.10:40000
*/

#include "rocketnet.h"
#include <stdio.h>
#include <signal.h>


// the demo's own sleep; the library surface stays minimal

#ifdef _WIN32
#include <windows.h>
static void demo_sleep_ms( int ms ) { Sleep( ms ); }
#else
#include <time.h>
static void demo_sleep_ms( int ms ) { struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = ( ms % 1000 ) * 1000000L; nanosleep( &ts, NULL ); }
#endif

static volatile int quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    if ( !rocketnet_init( NULL ) )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    const char * public_address = argc > 1 ? argv[1] : "127.0.0.1:40000";

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, public_address, 256 );
    if ( !server )
    {
        rocketnet_term();
        return 1;
    }

    int last_num_connected = 0;

    while ( !quit )
    {
        rocketnet_server_transport_update( server );

        const int num_connected = rocketnet_server_transport_num_connected_clients( server );
        if ( num_connected != last_num_connected )
        {
            printf( "server has %d client%s connected\n", num_connected, num_connected == 1 ? "" : "s" );
            last_num_connected = num_connected;
        }

        // echo every payload back to its sender

        struct rocketnet_server_transport_payloads_t * payloads = rocketnet_server_transport_payloads( server );
        for ( int i = 0; i < payloads->num_payloads; i++ )
        {
            rocketnet_server_transport_send_packet( server, payloads->client_index[i],
                                                    payloads->payload_data[i], payloads->payload_bytes[i] );
        }

        demo_sleep_ms( 10 );
    }

    printf( "\nserver stopping\n" );
    rocketnet_server_transport_stop( server );
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED )
        rocketnet_server_transport_update( server );
    rocketnet_server_transport_destroy( server );

    rocketnet_term();

    return 0;
}

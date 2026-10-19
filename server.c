/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The out-of-the-box server: runs with the published development keys, so
    a first server needs no backend and no configuration -- run it, point a
    client at it, connected. Echoes every payload back to its sender. Pass
    the public address clients dial: ./bin/server 192.0.2.10:40000
*/

#include "rocketnet.h"
#include "rocketnet_layers.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

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

    // the public address rides the config table (#106): ROCKETNET_PUBLIC_ADDRESS
    // (or its deprecated alias) overrides inside create, logged loudly --
    // no second read here, so the displacement is never applied twice

    const char * public_address = argc > 1 ? argv[1] : "127.0.0.1:40000";

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = 256;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, public_address, &config);
    if ( !server )
    {
        rocketnet_term();
        return 1;
    }

    // pace the loop at 60 frames per second, pegged to real time

    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, 60.0 );
    if ( !frame_sync )
    {
        rocketnet_server_transport_destroy( server );
        rocketnet_term();
        return 1;
    }

    int last_num_connected = 0;

    while ( !quit )
    {
        rocketnet_server_transport_update( server, rocketnet_time() );

        const int num_connected = rocketnet_server_transport_num_connected_clients( server );
        if ( num_connected != last_num_connected )
        {
            printf( "server has %d client%s connected\n", num_connected, num_connected == 1 ? "" : "s" );
            last_num_connected = num_connected;
        }

        // echo every payload back to its sender

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &payload_bytes );
            rocketnet_server_transport_send_packet( server, client_index, payload, payload_bytes );
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_frame_sync_wait( frame_sync );
    }

    printf( "\nserver stopping\n" );
    rocketnet_frame_sync_destroy( frame_sync );
    rocketnet_server_transport_stop( server );
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED )
        rocketnet_server_transport_update( server, rocketnet_time() );

    // ROCKETNET_SERVER_PRINT_COUNTERS=1: every server counter at stop, one
    // line each, through the public counter API. unset, output is unchanged.

    const char * print_counters = getenv( "ROCKETNET_SERVER_PRINT_COUNTERS" );
    if ( print_counters && strcmp( print_counters, "1" ) == 0 )
    {
        const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
        const int num_counters = rocketnet_num_counters();
        for ( int i = 0; i < num_counters; i++ )
            printf( "rocketnet counter %s %llu\n", rocketnet_counter_name( i ), (unsigned long long) rocketnet_counter_value( counters, i ) );
        fflush( stdout );
    }

    rocketnet_server_transport_destroy( server );

    rocketnet_term();

    return 0;
}

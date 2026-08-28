/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Soak: a client and server on loopback exchanging pattern-verified
    payloads of every size, reconnecting on a cycle, forever -- duration
    converts probability into certainty, and this is what finds the
    difficult problems. Run ./bin/soak and leave it; pass a number of
    iterations to bound it: ./bin/soak 10000
*/

#include "rocketnet.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h>

static volatile int quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

static void generate_payload( uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
        payload_data[i] = (uint8_t) ( start + i );
}

static int verify_payload( const uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
    {
        if ( payload_data[i] != (uint8_t) ( start + i ) )
            return 0;
    }
    return 1;
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    if ( !rocketnet_init( NULL ) )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    const uint64_t max_iterations = argc > 1 ? strtoull( argv[1], NULL, 10 ) : 0;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:40100", &server_config, rocketnet_time() );
    if ( !server )
        return 1;

    struct rocketnet_client_transport_t * client = NULL;

    uint64_t iteration = 0;
    uint64_t connects = 0;
    uint64_t payloads_verified = 0;
    uint64_t failures = 0;
    int payload_size = 1;
    double last_report_time = rocketnet_time();
    double reconnect_time = rocketnet_time();

    while ( !quit )
    {
        iteration++;
        if ( max_iterations && iteration > max_iterations )
            break;

        // reconnect on a cycle: the connect path is never a rare path

        if ( !client || rocketnet_time() - reconnect_time > 30.0 )
        {
            if ( client )
            {
                rocketnet_client_transport_disconnect( client );
                rocketnet_sleep( 0.05 );
                rocketnet_server_transport_update( server, rocketnet_time() );
                rocketnet_client_transport_destroy( client );
            }
            uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
            if ( !rocketnet_write_dev_connect_token( "127.0.0.1:40100", rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) )
            {
                printf( "soak: failed to write dev connect token\n" );
                exit( 1 );
            }
            client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_time() );
            if ( !client )
                break;
            reconnect_time = rocketnet_time();
            connects++;
        }

        rocketnet_client_transport_update( client, rocketnet_time() );
        rocketnet_server_transport_update( server, rocketnet_time() );

        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            // every size cycles through, both directions, every byte checked

            uint8_t payload[ROCKETNET_MAX_PAYLOAD_BYTES];
            generate_payload( payload, payload_size );
            rocketnet_client_transport_send_packet( client, payload, payload_size );
            rocketnet_server_transport_send_packet( server, 0, payload, payload_size );
            rocketnet_server_transport_send_packets( server );
            payload_size = payload_size < ROCKETNET_MAX_PAYLOAD_BYTES ? payload_size + 1 : 1;

            const int num_payloads = rocketnet_server_transport_num_payloads( server );
            for ( int i = 0; i < num_payloads; i++ )
            {
                int payload_client_index, payload_bytes;
                uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
                if ( verify_payload( payload, payload_bytes ) )
                    payloads_verified++;
                else
                    { printf( "PAYLOAD VERIFY FAILED at server (%d bytes)\n", payload_bytes ); failures++; }
            }

            uint8_t received[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];
            int received_bytes;
            while ( ( received_bytes = rocketnet_client_transport_receive_packet( client, received, sizeof(received) ) ) > 0 )
            {
                if ( verify_payload( received, received_bytes ) )
                    payloads_verified++;
                else
                    { printf( "PAYLOAD VERIFY FAILED at client (%d bytes)\n", received_bytes ); failures++; }
            }
        }

        if ( rocketnet_time() - last_report_time > 10.0 )
        {
            last_report_time = rocketnet_time();
            printf( "soak: %llu iterations, %llu connects, %llu payloads verified, %llu failures\n",
                    (unsigned long long) iteration, (unsigned long long) connects,
                    (unsigned long long) payloads_verified, (unsigned long long) failures );
        }

        rocketnet_sleep( 0.001 );
    }

    printf( "\nsoak finished: %llu iterations, %llu connects, %llu payloads verified, %llu failures\n",
            (unsigned long long) iteration, (unsigned long long) connects,
            (unsigned long long) payloads_verified, (unsigned long long) failures );

    if ( client )
    {
        rocketnet_client_transport_disconnect( client );
        rocketnet_sleep( 0.05 );
        rocketnet_client_transport_destroy( client );
    }
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_time() );
    rocketnet_server_transport_destroy( server );

    rocketnet_term();

    return failures ? 1 : 0;
}

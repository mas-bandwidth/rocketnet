/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The happy_path client: the real client binary. It mints a development
    connect token for the server address the tool passes, connects over the
    portable transport, sends one message on channel 0 for each of the eight
    reliability types, prints the receipt for the three receipt types, then
    disconnects cleanly and exits 0.

    HAPPY_PATH_BREAK=client-skip-mode-<n> makes this program omit that one
    send. The tool reads it only through the child, and it exists only to
    prove the tool's failing path.

    This is a small functional test program, not an example. It is built
    beside client and server under ROCKETNET_BUILD_EXAMPLES.
*/

#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 1
#endif

#include "rocketnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char happy_pattern[] = "rocketnet-happy-path";

int main( int argc, char ** argv )
{
    if ( !rocketnet_init( NULL ) )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    const char * address = getenv( "ROCKETNET_CLIENT_CONNECT_ADDRESS" );
    if ( !address )
        address = argc > 1 ? argv[1] : "127.0.0.1:40000";

    int skip_mode = -1;
    const char * broken = getenv( "HAPPY_PATH_BREAK" );
    if ( broken && strncmp( broken, "client-skip-mode-", 17 ) == 0 )
    {
        skip_mode = atoi( broken + 17 );
        if ( skip_mode < 0 || skip_mode > 7 )
            skip_mode = -1;
    }

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_dev_connect_token( address, 1234, (uint64_t) time( NULL ), connect_token ) )
    {
        printf( "error: failed to write dev connect token\n" );
        rocketnet_term();
        return 1;
    }

    struct rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "portable";

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL, 0 );
    if ( client && rocketnet_client_connect( client, connect_token, rocketnet_time() ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    if ( !client )
    {
        printf( "error: failed to create client\n" );
        rocketnet_term();
        return 1;
    }

    const int pattern_bytes = (int) sizeof( happy_pattern ) - 1;
    const int payload_bytes = 1 + pattern_bytes;

    const double wall_start = rocketnet_time();
    int connected = 0;
    int next_mode = 0;
    int acked[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t serials[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    double all_sent_at = 0.0;
    int done = 0;
    int exit_code = 0;

    while ( !done )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        if ( rocketnet_time() - wall_start > 20.0 )
        {
            printf( "error: client timed out\n" );
            exit_code = 1;
            break;
        }

        // (1) update pumps the transport and takes in what arrived
        if ( rocketnet_client_update( client, now ) != ROCKETNET_MESSAGE_OK )
        {
            printf( "error: client update failed\n" );
            exit_code = 1;
            break;
        }

        // (2) receive and process every received message for this update
        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_client_receive_message( client, 0, &data, &bytes ) )
            rocketnet_client_release_message( client, data );

        const int state = rocketnet_client_state( client );
        if ( state < 0 )
        {
            printf( "error: client failed to connect: %s\n", rocketnet_client_state_name( state ) );
            exit_code = 1;
            break;
        }

        // (3) run the game simulation for the frame: nothing to simulate
        if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( !connected )
            {
                connected = 1;
                printf( "client connected\n" );
                fflush( stdout );
            }

            // (4) queue this frame's outgoing messages, one mode per frame
            if ( next_mode < 8 )
            {
                if ( next_mode != skip_mode )
                {
                    uint8_t * payload = rocketnet_client_start_message( client, 0, next_mode, payload_bytes );
                    if ( !payload )
                    {
                        printf( "error: client could not start mode %d\n", next_mode );
                        exit_code = 1;
                        break;
                    }
                    payload[0] = (uint8_t) next_mode;
                    memcpy( payload + 1, happy_pattern, pattern_bytes );
                    serials[next_mode] = rocketnet_client_finish_message( client, payload_bytes );
                    if ( serials[next_mode] == 0 )
                    {
                        printf( "error: client could not finish mode %d\n", next_mode );
                        exit_code = 1;
                        break;
                    }
                }
                else
                {
                    printf( "skipping mode %d\n", next_mode );
                    fflush( stdout );
                }
                next_mode++;
                if ( next_mode == 8 )
                    all_sent_at = now;
            }

            struct rocketnet_receipt_t receipts[64];
            const int num_receipts = rocketnet_client_receipts( client, receipts, 64 );
            for ( int i = 0; i < num_receipts; i++ )
            {
                for ( int mode = 5; mode <= 7; mode++ )
                {
                    if ( serials[mode] != 0 && receipts[i].serial == serials[mode] &&
                         receipts[i].status == ROCKETNET_RECEIPT_ACKED && !acked[mode] )
                    {
                        acked[mode] = 1;
                        printf( "receipt mode %d acked\n", mode );
                        fflush( stdout );
                    }
                }
            }

            int receipts_complete = 1;
            for ( int mode = 5; mode <= 7; mode++ )
                if ( serials[mode] != 0 && !acked[mode] )
                    receipts_complete = 0;

            if ( next_mode == 8 && receipts_complete && ( now - all_sent_at ) >= 0.25 )
                done = 1;
            else if ( next_mode == 8 && ( now - all_sent_at ) > 5.0 )
                done = 1;
        }

        // (5) send_packets (send_messages after the lifecycle rename) is last,
        // after this frame's messages are queued, so this frame's messages
        // leave this frame
        rocketnet_client_send_messages( client );

        if ( !done )
            rocketnet_sleep( 0.005 );
    }

    if ( exit_code == 0 && connected )
    {
        rocketnet_client_disconnect( client );
        printf( "disconnected\n" );
        fflush( stdout );
    }

    rocketnet_client_destroy( client );
    rocketnet_term();

    return exit_code;
}

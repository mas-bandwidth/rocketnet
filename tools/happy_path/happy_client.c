/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The happy_path client: the real client binary. It mints a development
    connect token for the server address and server public key (hex) the
    tool passes, connects over the
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
#include "../../examples/dev_keys.h"
#include "../../test/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char happy_pattern[] = "rocketnet-happy-path";

int main( int argc, char ** argv )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    char address_buffer[256];
    const char * address = address_buffer;
    if ( !rocketnet_test_getenv( "ROCKETNET_CLIENT_CONNECT_ADDRESS", address_buffer, sizeof( address_buffer ) ) )
        address = argc > 1 ? argv[1] : "127.0.0.1:40000";

    int skip_mode = -1;
    char broken[256];
    if ( rocketnet_test_getenv( "HAPPY_PATH_BREAK", broken, sizeof( broken ) ) && strncmp( broken, "client-skip-mode-", 17 ) == 0 )
    {
        skip_mode = atoi( broken + 17 );
        if ( skip_mode < 0 || skip_mode > 7 )
            skip_mode = -1;
    }

    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    const char * key_hex = argc > 2 ? argv[2] : "";
    if ( strlen( key_hex ) != 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES )
    {
        printf( "error: expected the server public key as 64 hex characters\n" );
        rocketnet_term();
        return 1;
    }
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        int value = 0;
        for ( int j = 0; j < 2 && value >= 0; j++ )
        {
            const char c = key_hex[2 * i + j];
            const int nibble = ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10 : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
            value = nibble < 0 ? -1 : value * 16 + nibble;
        }
        if ( value < 0 )
        {
            printf( "error: bad server public key\n" );
            rocketnet_term();
            return 1;
        }
        server_key[i] = (uint8_t) value;
    }

    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( rocketnet_create_dev_connect_token( address, NULL, server_key, 30, 10, 1234, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to create dev connect token\n" );
        rocketnet_term();
        return 1;
    }

    struct rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "portable";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    if ( !client )
    {
        printf( "error: failed to create client: %s\n", rocketnet_status_name( create_status ) );
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
        rocketnet_client_update( client, now );

        // (2) receive and process every received message for this update
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            // the server sends this client nothing, so there is nothing to read
        }

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
                         receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED && !acked[mode] )
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

        // (5) send_packets is last,
        // after this frame's messages are queued, so this frame's messages
        // leave this frame
        rocketnet_client_send_packets( client );

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

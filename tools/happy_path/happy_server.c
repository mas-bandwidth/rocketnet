/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The happy_path server: the real server binary, on the portable transport
    over loopback, started with the address the tool chose. It prints one
    line per received message when the payload is exactly what the client
    sent, stops once its only client has disconnected, and exits 0.

    This is a small functional test program, not an example. It is built
    beside client and server under ROCKETNET_BUILD_EXAMPLES.
*/

#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 1
#endif

#include "../../examples/shutdown.h"
#include "rocketnet.h"
#include "../../examples/dev_keys.h"
#include "../../test/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * reliability_names[8] =
{
    "ROCKETNET_RELIABILITY_UNRELIABLE",
    "ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED",
    "ROCKETNET_RELIABILITY_RELIABLE",
    "ROCKETNET_RELIABILITY_RELIABLE_ORDERED",
    "ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED",
    "ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT",
    "ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT",
    "ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT"
};

static const char happy_pattern[] = "rocketnet-happy-path";

int main( int argc, char ** argv )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    // ROCKETNET_SERVER_IPV4_ADDRESS, when set, overrides this address in create
    const char * address = argc > 1 ? argv[1] : "127.0.0.1:0";

    struct rocketnet_server_config_t config;
    // the published development backend key (rocketnet.h, development builds); a real server brings its own backend's key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = 1;
    config.transport = "portable";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &create_status );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    if ( !server )
    {
        printf( "error: failed to create server: %s\n", rocketnet_status_name( create_status ) );
        rocketnet_term();
        return 1;
    }

    // the server makes its own keypair: the client needs its public key to mint a token
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );
    printf( "server public key " );
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
        printf( "%02x", server_key[i] );
    printf( "\n" );
    printf( "server started\n" );
    printf( "server transport %s\n", rocketnet_server_transport_name( server ) );
    fflush( stdout );

    const int pattern_bytes = (int) sizeof( happy_pattern ) - 1;
    const int payload_bytes = 1 + pattern_bytes;

    const double wall_start = rocketnet_time();
    int had_client = 0;
    int bad = 0;

    while ( 1 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );

        // (1) update pumps the transport and takes in what arrived
        rocketnet_server_update( server, now );

        // (2) receive and process every received message for this update
        // from one queue across every client, each message naming its sender
        {
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            {
                for ( int i = 0; i < batch->num_messages; i++ )
                {
                    const uint8_t * data = batch->data[i];
                    const int bytes = batch->bytes[i];
                    const int mode = bytes > 0 ? data[0] : -1;
                    const int ok = mode >= 0 && mode < 8 &&
                                   bytes == payload_bytes &&
                                   memcmp( data + 1, happy_pattern, (size_t) pattern_bytes ) == 0;
                    if ( ok )
                        printf( "received mode %d %s ok\n", mode, reliability_names[mode] );
                    else
                    {
                        printf( "received mode %d BAD\n", mode );
                        bad = 1;
                    }
                    fflush( stdout );
                }
            }
        }

        // (3) run the game simulation for the frame: nothing to simulate
        // (4) queue this frame's outgoing messages: none

        const int num_connected = rocketnet_server_num_connected_clients( server );
        if ( num_connected > 0 && !had_client )
        {
            printf( "client connected\n" );
            fflush( stdout );
        }
        if ( num_connected > 0 )
            had_client = 1;
        if ( had_client && num_connected == 0 )
        {
            printf( "client disconnected\n" );
            fflush( stdout );
            break;
        }

        // (5) send_packets is last,
        // after this frame's messages are queued, so this frame's messages
        // leave this frame
        rocketnet_server_send_packets( server );

        if ( rocketnet_time() - wall_start > 30.0 )
        {
            printf( "error: server timed out\n" );
            rn_cli_server_stop_and_destroy( server );
            rocketnet_term();
            return 1;
        }

        rocketnet_sleep( 0.005 );
    }

    printf( "server stopped\n" );
    fflush( stdout );

    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();

    return bad ? 1 : 0;
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Backend client: connects with a connect token a backend minted. The
    backends in examples/backend/ (Go and C#) answer GET /match/<client id>
    with the token as base64; this program takes that string as its
    argument, so it needs no HTTP code of its own:

        example_backend_client --token "$(curl -s http://127.0.0.1:8080/match/1)"

    A game fetches the token from its backend with whatever HTTP client it
    already has. The token names the server and carries the session keys,
    so the client needs nothing else: it connects over real UDP, sends one
    reliable-ordered message, receives the echo, prints its state changes,
    disconnects, and prints the state that says how the connection ended.

    Run example_backend_server, then a backend, then this program. See
    examples/backend/README.md.
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// serialize text (without its terminator) straight into a started message
static int write_text( uint8_t * p, const char * text, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        p[i] = (uint8_t) text[i];
    return bytes;
}

static void state_changed( void * context, enum rocketnet_client_state_t previous, enum rocketnet_client_state_t current )
{
    (void) context;
    printf( "client state: %s -> %s\n", rocketnet_client_state_name( previous ), rocketnet_client_state_name( current ) );
}

int main( int argc, char ** argv )
{
    const char * connect_token = NULL;
    for ( int i = 1; i + 1 < argc; i++ )
        if ( strcmp( argv[i], "--token" ) == 0 )
            connect_token = argv[i + 1];
    if ( !connect_token )
    {
        printf( "usage: example_backend_client --token <base64 connect token from the backend>\n" );
        return 1;
    }

    struct rocketnet_client_config_t config;
    struct rocketnet_client_t * client = NULL;
    int result = 1;
    int sent = 0;
    int received = 0;

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    rocketnet_client_default_config( &config );
    config.transport = "portable";
    const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = EXAMPLE_BACKEND_PACKET_FILTER;
    memcpy( config.packet_filter, packet_filter, sizeof( packet_filter ) );
    config.state_change_callback = state_changed;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &config, &create_status );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    CHECK( client != NULL );
    printf( "backend_client: mode=%s transport=%s\n", ROCKETNET_DEV ? "development" : "production", rocketnet_client_transport_name( client ) );

    // connect checks the token before the first packet: a string that does
    // not parse, or one already expired, is refused here
    const enum rocketnet_status_t connect_status = rocketnet_client_connect( client, connect_token );
    if ( connect_status != ROCKETNET_STATUS_OK ) printf( "connect refused: %s\n", rocketnet_status_name( connect_status ) );
    CHECK( connect_status == ROCKETNET_STATUS_OK );

    const double start = rocketnet_time();

    while ( rocketnet_time() - start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );

        // (1) update pumps the transport and takes in what arrived
        rocketnet_client_update( client, now );

        // (2) receive and release every received message
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                printf( "client received: %.*s\n", batch->bytes[i], (const char*) batch->data[i] );
                received = 1;
            }
        }

        // (3) run the game simulation for the frame: nothing to simulate here

        // (4) queue this frame's outgoing message, once the client is connected
        const int state = rocketnet_client_state( client );
        if ( !sent && state == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const char message[] = "hello from the backend's client";
            const int message_bytes = (int) sizeof( message ) - 1;
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message_bytes );
            if ( out )
            {
                sent = rocketnet_client_finish_message( client, write_text( out, message, message_bytes ) ) != 0;
            }
        }

        // (5) send_packets is last,
        // after this frame's messages are queued, so they leave this frame

        rocketnet_client_send_packets( client );

        // a negative state is the end of a connection that failed: a refused
        // or expired token, or a server that never answered
        if ( received || state < 0 ) break;
        rocketnet_sleep( 0.001 );
    }

    CHECK( received );
    rocketnet_client_disconnect( client );
    // the state says how a connection ended: DISCONNECTED after our own
    // disconnect, a negative state when the server or the protocol ended it
    printf( "client state after disconnect: %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    result = 0;

cleanup:
    // destroy on NULL is a no-op, as free(NULL) is
    if ( client )
        rocketnet_client_destroy( client );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}

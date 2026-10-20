/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Hello: the least RocketNet program that works. A server and a client in
    one process over the in-memory loopback transport, so it needs no socket
    and no network. The client connects, sends one reliable-ordered message,
    the server receives and prints it, and the client disconnects. It then
    connects a second time with a second connect token and disconnects, which
    shows that the server keeps serving after a client leaves.

    Run it from the repository root: ./bin/example_hello
*/

#include "rocketnet.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// Run frames until the client is connected and, when message is set, the
// server has received it. Returns 1 when done, 0 on the five-second deadline,
// and -1 when an update fails.
static int run_frames( struct rocketnet_server_t * server, struct rocketnet_client_t * client,
                       const char * message, int * received, double * time_base )
{
    const int send = message != NULL;
    const double start = rocketnet_time();
    const int max_clients = rocketnet_server_max_clients( server );
    int sent = 0;

    if ( received )
        *received = 0;

    while ( rocketnet_time() - start < 5.0 )
    {
        const double now = *time_base + ( rocketnet_time() - start );

        // (1) update pumps the transport and takes in what arrived

        if ( rocketnet_server_update( server, now ) != ROCKETNET_UPDATE_OK )
            return -1;
        if ( rocketnet_client_update( client, now ) != ROCKETNET_UPDATE_OK )
            return -1;

        // (2) receive and release every received message

        const uint8_t * data = NULL;
        int bytes = 0;

        while ( rocketnet_client_receive_message( client, 0, &data, &bytes ) )
            rocketnet_client_release_message( client, data );

        for ( int i = 0; i < max_clients; i++ )
        {
            if ( !rocketnet_server_client_connected( server, i ) )
                continue;
            while ( rocketnet_server_receive_message( server, i, 0, &data, &bytes ) )
            {
                printf( "server received: %.*s\n", bytes, (const char*) data );
                rocketnet_server_release_message( server, i, data );
                if ( received )
                    ( *received )++;
            }
        }

        // (3) run the game simulation for the frame: nothing to simulate here

        // (4) queue this frame's outgoing message, once the client is connected

        if ( send && !sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const int message_bytes = (int) strlen( message );
            uint8_t * buffer = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message_bytes );
            if ( !buffer )
                return -1;
            memcpy( buffer, message, (size_t) message_bytes );
            if ( !rocketnet_client_finish_message( client, message_bytes ) )
                return -1;
            sent = 1;
        }

        // (5) send_packets is last,
        // after this frame's messages are queued, so this frame's messages
        // leave this frame

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             ( !send || ( received && *received > 0 ) ) )
        {
            *time_base += rocketnet_time() - start;
            return 1;
        }

        rocketnet_sleep( 0.001 );
    }

    return 0;
}

int main( void )
{
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_client_config_t client_config;
    struct rocketnet_server_config_t server_config;
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char server_address[64];
    int result = 1;
    double time_base = 1000.0;

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

    // the server takes the published development keys, the loopback pin, and
    // an ephemeral port that is read back below

    rocketnet_server_dev_config( &server_config );
    server_config.max_clients = 2;
    server_config.transport = "loopback";

    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );

    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";

    // connect, send one message, disconnect; then connect again with a second
    // token and disconnect

    for ( int round = 1; round <= 2; round++ )
    {
        // the token names the server and carries the keys; a production client
        // gets one from a backend. This development token expires thirty
        // seconds after its timestamp, so the example uses one far ahead
        // instead of reading the wall clock. A real backend stamps the current
        // time.

        CHECK( rocketnet_write_dev_connect_token( server_address, (uint64_t) round, 4102444800ull, connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

        client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
        if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
        {
            rocketnet_client_destroy( client );
            client = NULL;
        }
        CHECK( client != NULL );

        int received = 0;
        CHECK( run_frames( server, client, round == 1 ? "hello from the client" : NULL, &received, &time_base ) == 1 );

        rocketnet_client_disconnect( client );
        rocketnet_client_destroy( client );
        client = NULL;
    }

    result = 0;

cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (sdk/rocketnet_internal.h:314)
    if ( client )
        rocketnet_client_destroy( client );
    if ( server )
        rocketnet_server_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}

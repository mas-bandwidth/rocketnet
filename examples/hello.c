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

#include "shutdown.h"
#include "rocketnet.h"
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// serialize text (without its terminator) straight into a started message
static int write_text( uint8_t * p, const char * text, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        p[i] = (uint8_t) text[i];
    return bytes;
}

// Run frames until the client is connected and, when message is set, the
// server has received it. Returns 1 when done, 0 on the five-second deadline,
// and -1 when the message cannot be queued.
static int run_frames( struct rocketnet_server_t * server, struct rocketnet_client_t * client,
                       const char * message, int * received, double * time_base )
{
    const int send = message != NULL;
    const double start = rocketnet_time();
    int sent = 0;

    if ( received )
        *received = 0;

    while ( rocketnet_time() - start < 5.0 )
    {
        const double now = *time_base + ( rocketnet_time() - start );

        // (1) update pumps the transport and takes in what arrived

        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        // (2) receive every message in batches; the next receive or update releases them

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            // the server sends this client nothing, so there is nothing to read
        }

        {
            const struct rocketnet_message_batch_t * incoming = NULL;
            while ( ( incoming = rocketnet_server_receive_messages( server ) ) != NULL )
            {
                for ( int i = 0; i < incoming->num_messages; i++ )
                {
                    const uint8_t * data = incoming->data[i];
                    const int bytes = incoming->bytes[i];
                    printf( "server received: %.*s\n", bytes, (const char*) data );

                    if ( received )
                        ( *received )++;
                }
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
            if ( !rocketnet_client_finish_message( client, write_text( buffer, message, message_bytes ) ) )
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
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    int result = 1;
    double time_base = 1000.0;

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    // the server takes the published development keys, the loopback pin, and
    // an ephemeral port that is read back below

    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, dev_backend_public_key, sizeof( server_config.backend_public_key ) );
    server_config.max_clients = 2;
    server_config.transport = "loopback";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    CHECK( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );
    rocketnet_server_public_key( server, server_public_key );

    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";

    // connect, send one message, disconnect; then connect again with a second
    // token and disconnect

    for ( int round = 1; round <= 2; round++ )
    {
        // the token names the server and carries the keys; a production client
        // gets one from a backend. Stamp the current Unix time, matching the
        // default token-validation clock; this token expires thirty seconds later.

        const time_t token_time = time( NULL );
        CHECK( token_time != (time_t) -1 );
        CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, (uint64_t) round, EXAMPLE_PROTOCOL_ID, (uint64_t) token_time, NULL, connect_token ) == ROCKETNET_STATUS_OK );

        enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
        client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
        if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
        if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
        {
            rn_cli_client_stop_and_destroy( client );
            client = NULL;
        }
        CHECK( client != NULL );

        int received = 0;
        CHECK( run_frames( server, client, round == 1 ? "hello from the client" : NULL, &received, &time_base ) == 1 );

        rocketnet_client_disconnect( client );
        rn_cli_client_stop_and_destroy( client );
        client = NULL;
    }

    result = 0;

cleanup:
    // destroy on NULL is a no-op, as free(NULL) is
    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    // CLI cleanup helpers wait for shutdown completion before destroy.
    // PASS is printed afterward so it stays the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}

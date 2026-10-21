/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Transports: auto, a pin, and a list. Three servers are created in turn:
    one with the platform's automatic ladder, one pinned to a single
    transport, and one given a candidate list. After each create the example
    prints the transport that the public query reports, and the library's own
    log lines say why each candidate was granted or refused. A fourth create
    pins a name no transport has and shows the loud refusal. Each server that
    starts is proved over the loopback interface with a client exchange.

    Run it from the repository root: ./bin/example_transports
*/

#include "rocketnet.h"
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

#include <stdio.h>
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

// Connect a portable client over the loopback interface, send one reliable
// message, and wait until the server receives it. Returns 1 when it arrived.
static int exchange( struct rocketnet_server_t * server, const char * server_address )
{
    struct rocketnet_client_config_t config;
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    struct rocketnet_client_t * client = NULL;
    const double start = rocketnet_time();
    int sent = 0;
    int received = 0;
    int ok = 0;
    rocketnet_client_default_config( &config );
    config.transport = "portable";   // the client side is portable; the server may be any transport
    rocketnet_server_public_key( server, server_public_key );   // the key the server made at create; the token carries it
    if ( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, 4102444800ull, NULL, connect_token ) != ROCKETNET_STATUS_OK )
        return 0;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &config, &create_status );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    if ( !client )
        return 0;
    while ( rocketnet_time() - start < 5.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );
        // (1) update pumps the transport and takes in what arrived
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        // (2) receive and release every received message
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            // the server sends this client nothing, so there is nothing to read
        }
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_server_receive_messages( server ) ) != NULL )
            received += incoming->num_messages;
        // (3) run the game simulation for the frame: nothing to simulate here
        // (4) queue this frame's outgoing message, once the client is connected
        if ( !sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const char * message = "transports";
            const int message_bytes = (int) strlen( message );
            uint8_t * buffer = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message_bytes );
            if ( !buffer )
                break;
            if ( !rocketnet_client_finish_message( client, write_text( buffer, message, message_bytes ) ) )
                break;
            sent = 1;
        }
        // (5) send_packets is last, after this frame's messages are queued, so
        // this frame's messages leave this frame
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED && received > 0 )
        {
            ok = 1;
            break;
        }
        rocketnet_sleep( 0.001 );
    }
    rocketnet_client_disconnect( client );
    rocketnet_client_destroy( client );
    return ok;
}

// Create a server with the given transport setting, print what won, and prove
// it with one exchange over 127.0.0.1. Returns 1 when the server did its job.
static int create_and_prove( const char * label, const char * transport )
{
    struct rocketnet_server_config_t config;
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    struct rocketnet_server_t * server;
    int ok;
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = 2;
    config.transport = transport;
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &create_status_2 );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    if ( !server )
        return 0;
    if ( !rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) )
    {
        rocketnet_server_destroy( server );
        return 0;
    }
    printf( "%s chose %s\n", label, rocketnet_server_transport_name( server ) );
    ok = exchange( server, server_address );
    rocketnet_server_destroy( server );
    return ok;
}

int main( void )
{
    struct rocketnet_server_config_t config;
    int result = 1;
    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    // let the library's own log lines through: they are the reason a
    // transport was granted, skipped, or refused
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    // auto: the platform's ladder, and the first transport that starts wins
    CHECK( create_and_prove( "auto", NULL ) );
    // a pin: exactly this transport, or a loud failure
    CHECK( create_and_prove( "pin", "portable" ) );
    // a list: tried left to right; the first candidate granted wins. Here the
    // ladder starts with xdp, which the library refuses (a port-zero public
    // address is not a port xdp can bind), so gso is granted and the log
    // lines print both the refusal and the grant.
    CHECK( create_and_prove( "list", "xdp,gso,portable" ) );
    // an impossible pin: a name no transport has, refused loudly by the
    // library, with the valid names in its error line
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = 2;
    config.transport = "not-a-transport";
    {
        enum rocketnet_status_t create_status_3 = ROCKETNET_STATUS_OK;
        struct rocketnet_server_t * refused = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &create_status_3 );
        if ( !refused ) printf( "create refused: %s\n", rocketnet_status_name( create_status_3 ) );
        const int started = refused ? (int) rocketnet_server_start( refused ) : -1;
        if ( refused )
            rocketnet_server_destroy( refused );
        CHECK( started != ROCKETNET_STATUS_OK );
    }
    printf( "refused as promised: the pin named no transport\n" );
    printf( "PASS\n" );
    result = 0;
cleanup:
    rocketnet_term();
    return result;
}

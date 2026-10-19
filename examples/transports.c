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

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// The lifecycle card that lands after this one renames start, finish, receive,
// release and send_packets to *_start_message, *_finish_message,
// *_receive_message, *_release_message and *_send_messages. The header at this
// base still declares the old names, so this example uses them.

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// Connect a portable client over the loopback interface, send one reliable
// message, and wait until the server receives it. Returns 1 when it arrived.
static int exchange( struct rocketnet_server_t * server, const char * server_address )
{
    struct rocketnet_client_config_t config;
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    struct rocketnet_client_t * client = NULL;
    const int max_clients = rocketnet_server_max_clients( server );
    const double start = rocketnet_time();
    int sent = 0;
    int received = 0;
    int ok = 0;
    rocketnet_client_default_config( &config );
    config.transport = "portable";   // the client side is portable; the server may be any driver
    if ( !rocketnet_write_dev_connect_token( server_address, 1, 4102444800ull, connect_token ) )
        return 0;
    client = rocketnet_client_create( NULL, &config, NULL, 0 );
    if ( client && rocketnet_client_connect( client, connect_token, rocketnet_time() ) != ROCKETNET_STATUS_OK )
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
        if ( rocketnet_server_update( server, now ) != ROCKETNET_MESSAGE_OK )
            break;
        if ( rocketnet_client_update( client, now ) != ROCKETNET_MESSAGE_OK )
            break;
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
                received++;
                rocketnet_server_release_message( server, i, data );
            }
        }
        // (3) run the game simulation for the frame: nothing to simulate here
        // (4) queue this frame's outgoing message, once the client is connected
        if ( !sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const char * message = "transports";
            const int message_bytes = (int) strlen( message );
            uint8_t * buffer = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABLE_ORDERED, message_bytes );
            if ( !buffer )
                break;
            memcpy( buffer, message, (size_t) message_bytes );
            if ( !rocketnet_client_finish_message( client, message_bytes ) )
                break;
            sent = 1;
        }
        // (5) send_packets is last, after this frame's messages are queued, so
        // this frame's messages leave this frame (send_messages after the
        // lifecycle rename)
        rocketnet_client_send_messages( client );
        rocketnet_server_send_messages( server );
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
    char server_address[64];
    struct rocketnet_server_t * server;
    int ok;
    rocketnet_server_dev_config( &config );
    config.max_clients = 2;
    config.transport = transport;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &config, NULL, 0 );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    if ( !server )
        return 0;
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    printf( "%s chose %s\n", label, rocketnet_server_transport_name( server ) );
    ok = exchange( server, server_address );
    rocketnet_server_destroy( server );
    return ok;
}

int main( void )
{
    struct rocketnet_server_config_t config;
    int result = 1;
    CHECK( rocketnet_init( NULL ) );
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
    rocketnet_server_dev_config( &config );
    config.max_clients = 2;
    config.transport = "not-a-transport";
    {
        struct rocketnet_server_t * refused = rocketnet_server_create( NULL, "127.0.0.1:0", &config, NULL, 0 );
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

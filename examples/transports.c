/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Transports: auto, a pin, and a list. Three servers are created in turn:
    one on the platform's automatic ladder, one pinned to one transport, and
    one given a candidate list. Each prints the transport the public query
    reports, the library's log lines say why each candidate was granted or
    refused, and a client exchange over 127.0.0.1 proves it. A fourth pins a
    name no transport has and is refused loudly. Run: ./bin/example_transports
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// create and start a server on this transport setting, then prove it with
// one message from a portable client. Returns 1 when the message arrived
static int create_and_prove( const char * label, const char * transport )
{
    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &server_config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );
        return 0;
    }
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = transport;

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
        server_status = rocketnet_server_start( server );
    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        printf( "%s: server start refused: %s\n", label, rocketnet_status_name( server_status ) );
        rn_cli_server_stop_and_destroy( server );
        return 0;
    }
    printf( "%s chose %s\n", label, rocketnet_server_transport_name( server ) );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( !rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );
        return 0;
    }
    rocketnet_server_public_key( server, server_public_key );
    const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );
    if ( token_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect token: %s\n", rocketnet_status_name( token_status ) );
        rn_cli_server_stop_and_destroy( server );
        return 0;
    }

    // the client is portable; the server may be any transport
    struct rocketnet_client_config_t client_config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &client_config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );
        rn_cli_server_stop_and_destroy( server );
        return 0;
    }
    client_config.transport = "portable";
    enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &client_status );
    if ( !client || client_status != ROCKETNET_STATUS_OK || rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create/connect: %s\n", rocketnet_status_name( client_status ) );
        rn_cli_client_stop_and_destroy( client );
        rn_cli_server_stop_and_destroy( server );
        return 0;
    }

    int sent = 0, received = 0;
    const double start = rocketnet_time();
    while ( !received && rocketnet_time() - start < 5.0 )
    {
        const double now = 1000.0 + rocketnet_time() - start;
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL ) {}
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            received += batch->num_messages;

        if ( !sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            // reliable: defer to a later frame when there is no room
            if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 10 ) )
            {
                uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 10 );
                if ( p )
                {
                    memcpy( p, "transports", 10 );
                    sent = rocketnet_client_finish_message( client, 10 ) != 0;
                }
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    return received > 0;
}

int main( void )
{
    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the library's own log lines say why a transport was granted, skipped or refused
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

    // auto: the platform's ladder, and the first transport that starts wins
    int ok = create_and_prove( "auto", NULL );

    // a pin: exactly this transport, or a loud failure
    ok &= create_and_prove( "pin", "portable" );

    // a list, tried left to right: xdp is refused (port zero is not a port xdp
    // can bind), so gso is granted, and the log lines show both
    ok &= create_and_prove( "list", "xdp,gso,portable" );

    // a name no transport has: refused loudly, with the valid names in the error line
    if ( ok && !create_and_prove( "impossible", "not-a-transport" ) )
        printf( "refused as promised: the pin named no transport\n" );
    else
        ok = 0;

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }
    printf( ok ? "PASS\n" : "FAIL: auto, pin and list each start a server, and the impossible pin is refused\n" );
    return ok ? 0 : 1;
}

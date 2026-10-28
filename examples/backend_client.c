/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Backend client: connects with a connect token a backend minted. The
    backends in examples/backend/ (Go and C#) answer GET /match/<client id>
    with the token as base64; this program takes that string as its argument,
    so it needs no HTTP code of its own:

        example_backend_client --token "$(curl -s http://127.0.0.1:8080/match/1)"

    The token names the server and carries the session keys, so the client
    needs nothing else: it connects over real UDP, sends one reliable-ordered
    message, receives the echo, prints its state changes, disconnects, and
    prints the state that says how the connection ended.

    Run example_backend_server, then a backend, then this program. See
    examples/backend/README.md.
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>

static void state_changed( void * context, enum rocketnet_client_state_t previous, enum rocketnet_client_state_t current )
{
    (void) context;
    printf( "client state: %s -> %s\n", rocketnet_client_state_name( previous ), rocketnet_client_state_name( current ) );
}

int main( int argc, char ** argv )
{
    if ( argc != 3 || strcmp( argv[1], "--token" ) != 0 )
    {
        printf( "usage: example_backend_client --token <base64 connect token from the backend>\n" );
        return 1;
    }

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the packet filter is the title's, shared with its servers

    const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = EXAMPLE_BACKEND_PACKET_FILTER;
    struct rocketnet_client_config_t config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    config.transport = "portable";
    memcpy( config.packet_filter, packet_filter, sizeof( packet_filter ) );
    config.state_change_callback = state_changed;

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    if ( !client || create_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create: %s\n", rocketnet_status_name( create_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    // connect checks the token first: a string that does not parse, or one
    // already expired, is refused here

    const enum rocketnet_status_t status = rocketnet_client_connect( client, argv[2] );
    if ( status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect refused: %s\n", rocketnet_status_name( status ) );
        rn_cli_client_stop_and_destroy( client );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    printf( "backend_client: mode=%s transport=%s\n", ROCKETNET_DEV ? "development" : "production", rocketnet_client_transport_name( client ) );

    // a negative state ends a connection that failed: a refused or expired
    // token, or a server that never answered

    int sent = 0, received = 0;
    const double start = rocketnet_time();
    while ( !received && rocketnet_client_state( client ) >= 0 && rocketnet_time() - start < 10.0 )
    {
        rocketnet_client_update( client, 1000.0 + rocketnet_time() - start );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++, received++ )
            {
                printf( "client received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
            }
        }

        if ( !sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const char message[] = "hello from the backend's client";

            // reliable: defer to a later frame when there is no room

            if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) ) )
            {
                uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) );
                if ( p )
                {
                    memcpy( p, message, strlen( message ) );
                    sent = rocketnet_client_finish_message( client, (int) strlen( message ) ) != 0;
                }
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_sleep( 0.001 );
    }

    // the state says how a connection ended: DISCONNECTED after our own disconnect

    rocketnet_client_disconnect( client );
    printf( "client state after disconnect: %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    rn_cli_client_stop_and_destroy( client );

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( received ? "PASS\n" : "FAIL: the echo did not arrive\n" );
    return received ? 0 : 1;
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Backend client: connects with a connect token a backend minted. The
    backends in examples/backend/ (Go and C#) answer POST /match, whose body
    is the ticket the player's auth provider signed, with the token as
    base64; this program takes that string as its argument, so it needs no
    HTTP code of its own:

        ticket="$(curl -s -X POST http://127.0.0.1:8080/mock/ticket/alice)"
        example_backend_client --token "$(curl -s --data "$ticket" http://127.0.0.1:8080/match)"

    The token names the server and carries the session keys, so the client
    needs nothing else: it connects over real UDP, sends one reliable-ordered
    message, receives the echo, prints its state changes, disconnects, and
    prints the state that says how the connection ended.

    With --stay it does not disconnect after the echo: it stays connected
    until the server disconnects it (a ban, or another session of the same
    user taking its place on the backend's whitelist), and passes only if
    that happens within --seconds (default 10).

    Run example_backend_server, then a backend, then this program. See
    examples/backend/README.md.
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void state_changed( void * context, enum rocketnet_client_state_t previous, enum rocketnet_client_state_t current )
{
    (void) context;
    printf( "client state: %s -> %s\n", rocketnet_client_state_name( previous ), rocketnet_client_state_name( current ) );
    fflush( stdout );
}

int main( int argc, char ** argv )
{
    const char * token = NULL;
    int stay = 0;
    double seconds = 10.0;
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--token" ) == 0 && i + 1 < argc )
        {
            token = argv[++i];
        }
        else if ( strcmp( argv[i], "--stay" ) == 0 )
        {
            stay = 1;
        }
        else if ( strcmp( argv[i], "--seconds" ) == 0 && i + 1 < argc )
        {
            char * end = NULL;
            seconds = strtod( argv[++i], &end );
            if ( *end != '\0' || !( seconds > 0.0 && seconds <= 600.0 ) )
            {
                token = NULL;
                break;
            }
        }
        else
        {
            token = NULL;
            break;
        }
    }

    if ( !token )
    {
        printf( "usage: example_backend_client --token <base64 connect token from the backend> [--stay] [--seconds <1 .. 600>]\n" );
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

    const enum rocketnet_status_t status = rocketnet_client_connect( client, token );
    if ( status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect refused: %s\n", rocketnet_status_name( status ) );
        rn_cli_client_stop_and_destroy( client, 1000.0 );

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
    while ( ( stay || !received ) && rocketnet_client_state( client ) >= 0 && rocketnet_time() - start < seconds )
    {
        rocketnet_client_update( client, 1000.0 + rocketnet_time() - start );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++, received++ )
            {
                printf( "client received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
                fflush( stdout );
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

    // with --stay, the server ended it: SERVER_DISCONNECTED

    const enum rocketnet_client_state_t ended = rocketnet_client_state( client );
    const int kicked = ended == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED;
    if ( stay )
    {
        printf( "client stayed %.3f seconds, state %s\n", rocketnet_time() - start, rocketnet_client_state_name( ended ) );
    }

    // the state says how a connection ended: DISCONNECTED after our own disconnect

    rocketnet_client_disconnect( client );
    printf( "client state after disconnect: %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    rn_cli_client_stop_and_destroy( client, 1000.0 + rocketnet_time() - start );

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    if ( stay )
    {
        printf( received && kicked ? "PASS\n" : ( received ? "FAIL: the server did not disconnect the client\n" : "FAIL: the echo did not arrive\n" ) );
        return received && kicked ? 0 : 1;
    }

    printf( received ? "PASS\n" : "FAIL: the echo did not arrive\n" );
    return received ? 0 : 1;
}

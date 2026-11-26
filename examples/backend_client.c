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

    --link-mtus <list> sets the link MTU list (examples/link_mtus.h); the
    server must be given the same list. Once connected the client prints the
    packet size its connection chose and its largest unreliable message.

    Run example_backend_server, then a backend, then this program. See
    examples/backend/README.md.
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include "link_mtus.h"
#include <stdio.h>
#include <string.h>

static void state_changed( void * context, enum rocketnet_client_state_t previous, enum rocketnet_client_state_t current )
{
    (void) context;
    printf( "client state: %s -> %s\n", rocketnet_client_state_name( previous ), rocketnet_client_state_name( current ) );
}

int main( int argc, char ** argv )
{
    int link_mtus[ROCKETNET_MAX_LINK_MTUS];
    int num_link_mtus = 0;
    argc = rn_cli_take_link_mtus( argc, argv, link_mtus, &num_link_mtus );
    if ( argc < 0 )
    {
        return 1;
    }

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
    if ( num_link_mtus > 0 )
    {
        memcpy( config.link_mtus, link_mtus, sizeof( int ) * (size_t) num_link_mtus );
        config.num_link_mtus = num_link_mtus;

        // a list down to 576 carries the default 1 MiB message in 480-byte
        // fragments, more than the default window of 1024 holds, so create
        // would refuse it (LINK_MTUS_TOO_SMALL_FOR_MESSAGES): raise both
        // windows, as "Path MTU" in examples/backend/README.md says

        config.reliable_messages_in_flight = 4096;
        config.packets_in_flight = 4096;
    }

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

    // With a link MTU list of more than one entry the connection starts at the
    // list's smallest packet size and rises as the probe finds a larger one
    // (standard/TRANSPORT.md t11.4), so the client stays up to one more
    // second after its echo to show the rise. The default list never probes.

    const double linger_seconds = num_link_mtus > 1 ? 1.0 : 0.0;
    int sent = 0, received = 0, reported_packet_bytes = 0;
    double received_time = 0.0;
    const double start = rocketnet_time();
    while ( ( !received || rocketnet_time() - received_time < linger_seconds ) && rocketnet_client_state( client ) >= 0 && rocketnet_time() - start < 10.0 )
    {
        rocketnet_client_update( client, 1000.0 + rocketnet_time() - start );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++, received++ )
            {
                printf( "client received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
                received_time = received ? received_time : rocketnet_time();
            }
        }

        // the connection's packet size, the largest UNRELIABLE message that
        // still goes in one packet, and the link MTU list entry the size comes
        // from: at connect, and again each time the size rises (it never falls)

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED && rocketnet_client_max_packet_bytes( client ) > reported_packet_bytes )
        {
            reported_packet_bytes = rocketnet_client_max_packet_bytes( client );
            printf( "backend_client: packet_bytes=%d max_unreliable_message_bytes=%d link_mtu=%d\n",
                    reported_packet_bytes, rocketnet_client_max_unreliable_message_bytes( client ), rocketnet_client_link_mtu( client ) );
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
    rn_cli_client_stop_and_destroy( client, 1000.0 + rocketnet_time() - start );

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( received ? "PASS\n" : "FAIL: the echo did not arrive\n" );
    return received ? 0 : 1;
}

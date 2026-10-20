/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Budgets: the send budget shapes what leaves a connection every second.
    This example prints the client and per-client server send budget defaults,
    sets the client's lower in its config at create and again with the public
    set-send-kbps call, then goes over budget on purpose: a burst of unreliable
    messages above it is dropped and counted, while a burst of reliable
    messages above it waits and every one arrives. One server and one client
    in one process over the loopback transport. Run: ./bin/example_budgets.
    The environment variables ROCKETNET_CLIENT_SEND_KBPS and
    ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS override a budget at create.
*/

#include "rocketnet.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// This example uses the send names the header at this base declares (start,
// finish, receive, release, send_packets); a later lifecycle rename makes
// them *_start_message and friends.

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

#define CLIENT_SEND_KBPS        10
#define UNRELIABLE_COUNT        120
#define RELIABLE_COUNT          40
#define MESSAGE_BYTES           200

static int send_burst( struct rocketnet_client_t * client, int channel, int reliability, int count )
{
    for ( int i = 0; i < count; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( client, channel, reliability, MESSAGE_BYTES );
        if ( !out )
            return 0;
        memset( out, (int)( 'A' + ( i % 26 ) ), MESSAGE_BYTES );
        if ( !rocketnet_client_finish_message( client, MESSAGE_BYTES ) )
            return 0;
    }
    return 1;
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

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    printf( "client send budget default: %d kbps\n", ROCKETNET_DEFAULT_CLIENT_SEND_KBPS );
    printf( "server per-client send budget default: %d kbps\n", ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS );

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
    CHECK( rocketnet_write_dev_connect_token( server_address, 1, 4102444800ull, connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.send_kbps = CLIENT_SEND_KBPS;
    printf( "client send budget from config at create: %d kbps\n", client_config.send_kbps );

    client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );

    int reliable_received = 0;
    int unreliable_sent = 0;
    int reliable_sent = 0;
    int runtime_budget_set = 0;
    const double start = rocketnet_time();

    while ( rocketnet_time() - start < 60.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - start );

        // (1) update pumps the transport and takes in what arrived
        if ( rocketnet_server_update( server, now ) != ROCKETNET_UPDATE_OK )
            goto cleanup;
        if ( rocketnet_client_update( client, now ) != ROCKETNET_UPDATE_OK )
            goto cleanup;

        // (2) receive and release every message; count the reliable ones,
        // sent on channel 1, so the example checks none was lost
        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_client_receive_message( client, 0, &data, &bytes ) )
            rocketnet_client_release_message( client, data );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const int slot = rocketnet_client_index( client );
            while ( rocketnet_server_receive_message( server, slot, 0, &data, &bytes ) )
                rocketnet_server_release_message( server, slot, data );
            while ( rocketnet_server_receive_message( server, slot, 1, &data, &bytes ) )
            {
                reliable_received++;
                rocketnet_server_release_message( server, slot, data );
            }
        }

        // (3) run the game simulation for the frame: nothing to simulate here

        // (4) queue this frame's outgoing messages, once connected
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            // set-send-kbps needs a CONNECTED client, so the runtime call waits
            // for the handshake; the config's send_kbps set the starting rate
            if ( !runtime_budget_set )
            {
                const enum rocketnet_status_t status = rocketnet_client_set_send_kbps( client, CLIENT_SEND_KBPS );
                CHECK( status == ROCKETNET_STATUS_OK );
                printf( "runtime set-send-kbps returned %d\n", (int) status );
                runtime_budget_set = 1;
            }
            // unreliable messages over budget are dropped and counted at send
            if ( !unreliable_sent )
            {
                CHECK( send_burst( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, UNRELIABLE_COUNT ) );
                unreliable_sent = 1;
            }
            // reliable messages over budget wait and are never dropped
            if ( !reliable_sent )
            {
                CHECK( send_burst( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, RELIABLE_COUNT ) );
                reliable_sent = 1;
            }
        }

        // (5) send_packets is last, after this frame's messages are queued,
        // so this frame's messages leave this frame
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );

        if ( reliable_sent && reliable_received >= RELIABLE_COUNT )
            break;

        rocketnet_sleep( 0.001 );
    }

    const uint64_t drops = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS );
    printf( "unreliable over-budget drops: %llu\n", (unsigned long long) drops );
    printf( "reliable messages arrived: %d/%d\n", reliable_received, RELIABLE_COUNT );
    CHECK( drops > 0 );
    CHECK( reliable_received == RELIABLE_COUNT );
    result = 0;

cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (rocketnet_api_null_guard)
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

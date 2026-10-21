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

#include "shutdown.h"
#include "rocketnet.h"
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
#include <stdio.h>
#include <time.h>
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
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    int result = 1;

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    printf( "client send budget default: %d kbps\n", ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    printf( "server per-client send budget default: %d kbps\n", ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );

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
    const time_t token_time = time( NULL );
    CHECK( token_time != (time_t) -1 );
    CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) token_time, NULL, connect_token ) == ROCKETNET_STATUS_OK );

    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.send_kbps = CLIENT_SEND_KBPS;
    printf( "client send budget from config at create: %d kbps\n", client_config.send_kbps );

    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_client_stop_and_destroy( client );
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
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        // (2) receive and release every message; count the reliable ones,
        // sent on channel 1, so the example checks none was lost. The server
        // takes its one queue in batches, each entry naming its channel; the
        // next batch call or update releases a batch
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( client ) ) != NULL )
            ;
        const int slot = (int) rocketnet_client_index( client );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                if ( batch->client_index[i] == slot && batch->channel[i] == 1 )
                    reliable_received++;
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
                // the runtime budget is set once connected: set_send_kbps refuses before then
                CHECK( rocketnet_client_set_send_kbps( client, CLIENT_SEND_KBPS ) == ROCKETNET_STATUS_OK );
                printf( "runtime set-send-kbps returned 1\n" );
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

    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( client, &stats );
    const uint64_t drops = stats.messages_dropped_over_budget;
    printf( "unreliable over-budget drops: %llu\n", (unsigned long long) drops );
    printf( "reliable messages arrived: %d/%d\n", reliable_received, RELIABLE_COUNT );
    CHECK( drops > 0 );
    CHECK( reliable_received == RELIABLE_COUNT );
    result = 0;

cleanup:
    // destroy on NULL is a no-op, as free(NULL) is
    if ( client )
        rn_cli_client_stop_and_destroy( client );
    if ( server )
        rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}

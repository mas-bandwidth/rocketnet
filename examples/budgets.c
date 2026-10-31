/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Budgets: the send budget shapes what leaves a connection every second. The
    example prints the client and per-client server defaults, sets the
    client's budget lower in its config and again at runtime, then goes over
    it on purpose over loopback: a burst of unreliable messages is dropped and
    counted, while a burst of reliable messages waits and every one arrives.
    ROCKETNET_CLIENT_SEND_KBPS and ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS
    override a budget at create. Run: ./bin/example_budgets
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLIENT_SEND_KBPS    10
#define UNRELIABLE_COUNT    120
#define RELIABLE_COUNT      40
#define MESSAGE_BYTES       200

static void send_burst( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int count )
{
    for ( int i = 0; i < count; i++ )
    {
        // burst policy: skip a message when there is no room; the budget drop counters still show the over-budget burst

        if ( !rocketnet_client_can_send_message( client, channel, reliability, MESSAGE_BYTES ) )
            continue;

        uint8_t * p = rocketnet_client_start_message( client, channel, reliability, MESSAGE_BYTES );
        if ( p )
        {
            memset( p, 'A' + i % 26, MESSAGE_BYTES );

            const uint32_t serial = rocketnet_client_finish_message( client, MESSAGE_BYTES );
            (void) serial;
        }
    }
}

int main( void )
{
    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the in-memory loopback transport keeps its endpoints in a hub the caller
    // owns, sized here for one server and one client; it is destroyed after them

    enum rocketnet_status_t hub_status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 1, &hub_status );
    if ( !hub || hub_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: loopback hub create: %s\n", rocketnet_status_name( hub_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    printf( "client send budget default: %d kbps\n", ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    printf( "server per-client send budget default: %d kbps\n", ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &server_config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
    {
        server_status = rocketnet_server_start( server );
    }

    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( server_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( !rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    rocketnet_server_public_key( server, server_public_key );

    const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );
    if ( token_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect token: %s\n", rocketnet_status_name( token_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    struct rocketnet_client_config_t client_config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &client_config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    client_config.transport = "loopback";
    client_config.loopback_hub = hub;
    client_config.send_kbps = CLIENT_SEND_KBPS;
    printf( "client send budget from config at create: %d kbps\n", client_config.send_kbps );

    enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &client_status );
    if ( client && client_status == ROCKETNET_STATUS_OK )
    {
        client_status = rocketnet_client_connect( client, connect_token );
    }

    if ( !client || client_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create/connect: %s\n", rocketnet_status_name( client_status ) );
        rn_cli_client_stop_and_destroy( client, 1000.0 );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    int bursts_sent = 0, reliable_received = 0;
    const double start = rocketnet_time();

    while ( reliable_received < RELIABLE_COUNT && rocketnet_time() - start < 60.0 )
    {
        const double now = 1000.0 + rocketnet_time() - start;
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        // the server's one queue names each message's channel: the reliable burst is on channel 1

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
        }

        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                reliable_received += batch->channel[i] == 1;
            }

        if ( !bursts_sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            // the runtime call needs a connected client; the config set the starting rate

            const enum rocketnet_status_t status = rocketnet_client_set_send_kbps( client, CLIENT_SEND_KBPS );
            printf( "runtime set-send-kbps returned %s\n", rocketnet_status_name( status ) );

            // over budget, unreliable messages are dropped and counted; reliable messages wait

            send_burst( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, UNRELIABLE_COUNT );
            send_burst( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, RELIABLE_COUNT );
            bursts_sent = 1;
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( client, &stats );
    printf( "unreliable over-budget drops: %llu\n", (unsigned long long) stats.messages_dropped_over_budget );
    printf( "reliable messages arrived: %d/%d\n", reliable_received, RELIABLE_COUNT );

    const int result = stats.messages_dropped_over_budget > 0 && reliable_received == RELIABLE_COUNT ? 0 : 1;

    rn_cli_client_stop_and_destroy( client, 1000.0 + rocketnet_time() - start );
    rn_cli_server_stop_and_destroy( server );

    rn_cli_loopback_hub_destroy( hub );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( result == 0 ? "PASS\n" : "FAIL: unreliable messages over budget drop and reliable ones all arrive\n" );
    return result;
}

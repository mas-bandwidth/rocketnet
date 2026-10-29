/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Stats: how a game's debug overlay reads connection quality. A server and a
    client over loopback exchange a little traffic; then one stats call per
    endpoint copies RTT, jitter, loss, bandwidth and counters into a struct the
    caller owns, and the example prints them, then how the connection ended
    on both ends. Run: ./bin/example_stats
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// one frame: the server echoes every message back, the client sends one while connected
static void frame( struct rocketnet_server_t * server, struct rocketnet_client_t * client, double now, int send )
{
    rocketnet_server_update( server, now );
    rocketnet_client_update( client, now );

    const struct rocketnet_message_batch_t * batch;
    while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
    {
    }

    while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            // echo: skip this message when there is no room; the caller keeps draining later frames

            if ( rocketnet_server_can_send_message( server, batch->client_index[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] ) )
            {
                uint8_t * p = rocketnet_server_start_message( server, (uint64_t) batch->client_index[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, batch->bytes[i] );
                if ( p )
                {
                    memcpy( p, batch->data[i], (size_t) batch->bytes[i] );

                    const uint32_t serial = rocketnet_server_finish_message( server, batch->bytes[i] );
                    (void) serial;
                }
            }
        }
    }

    if ( send && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 32 ) )
        {
            uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 32 );
            if ( p )
            {
                memset( p, 0, 32 );

                const uint32_t serial = rocketnet_client_finish_message( client, 32 );
                (void) serial;
            }
        }
    }

    rocketnet_client_send_packets( client );
    rocketnet_server_send_packets( server );
    rocketnet_sleep( 0.001 );
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

    enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &client_status );
    if ( !client || client_status != ROCKETNET_STATUS_OK || rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
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

    // connect, then a second and a half of traffic

    const double start = rocketnet_time();
    double connected_at = 0.0;
    while ( rocketnet_time() - start < 5.0 && ( connected_at == 0.0 || rocketnet_time() - connected_at < 1.5 ) )
    {
        frame( server, client, 1000.0 + rocketnet_time() - start, 1 );
        if ( connected_at == 0.0 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            connected_at = rocketnet_time();
        }
    }

    const int client_index = rocketnet_client_index( client );      // the client's server slot; -1 until CONNECTED
    if ( client_index < 0 )
    {
        printf( "FAIL: the client did not connect\n" );
        rn_cli_client_stop_and_destroy( client, 1000.0 + rocketnet_time() - start );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    printf( "client state %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );

    // one call copies every stat into a struct; read the fields, not a call per field

    struct rocketnet_client_stats_t client_stats;
    rocketnet_client_stats( client, &client_stats );
    printf( "client rtt %.2f ms, jitter %.2f ms, loss %.2f%%, bandwidth %.1f kbps sent %.1f kbps received\n",
            client_stats.rtt_ms, client_stats.jitter_ms, client_stats.packet_loss_percent, client_stats.bandwidth_sent_kbps, client_stats.bandwidth_received_kbps );
    printf( "client bytes sent %llu received %llu, messages resent %llu, api misuse %llu\n",
            (unsigned long long) client_stats.bytes_sent, (unsigned long long) client_stats.bytes_received,
            (unsigned long long) client_stats.messages_resent, (unsigned long long) client_stats.api_misuses );

    // the server's view of one connection, and its totals, are the same idiom

    struct rocketnet_server_client_stats_t slot_stats;
    rocketnet_server_client_stats( server, client_index, &slot_stats );
    printf( "server client %d rtt %.2f ms, jitter %.2f ms, loss %.2f%%, bandwidth %.1f kbps sent %.1f kbps received\n",
            client_index, slot_stats.rtt_ms, slot_stats.jitter_ms, slot_stats.packet_loss_percent, slot_stats.bandwidth_sent_kbps, slot_stats.bandwidth_received_kbps );
    printf( "server client %d bytes sent %llu received %llu\n",
            client_index, (unsigned long long) slot_stats.bytes_sent, (unsigned long long) slot_stats.bytes_received );

    struct rocketnet_server_stats_t server_stats;
    rocketnet_server_stats( server, &server_stats );
    printf( "server clients connected %llu, packets sent %llu received %llu\n", (unsigned long long) server_stats.connections_accepted,
            (unsigned long long) server_stats.packets_sent, (unsigned long long) server_stats.packets_received );

    // disconnect, then read how the connection ended: the client's state, and
    // the server's disconnect reason for the slot

    rocketnet_client_disconnect( client );

    const double disconnect_start = rocketnet_time();
    while ( rocketnet_server_client_connected( server, client_index ) && rocketnet_time() - disconnect_start < 2.0 )
    {
        frame( server, client, 1000.0 + rocketnet_time() - start, 0 );
    }

    printf( "client after disconnect state %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    printf( "server client %d disconnect reason %s\n", client_index, rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, client_index ) ) );

    const int result = client_stats.bytes_sent > 0 && client_stats.rtt_ms >= 0.0 && slot_stats.bytes_sent > 0 && slot_stats.rtt_ms >= 0.0 &&
                               server_stats.connections_accepted >= 1 &&
                               rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED
                           ? 0
                           : 1;
    rn_cli_client_stop_and_destroy( client, 1000.0 + rocketnet_time() - start );
    rn_cli_server_stop_and_destroy( server );

    rn_cli_loopback_hub_destroy( hub );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( result == 0 ? "PASS\n" : "FAIL: traffic counted on both ends and a clean disconnect\n" );
    return result;
}

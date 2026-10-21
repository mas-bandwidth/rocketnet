/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Stats: how a game's debug overlay reads connection quality. A server and a
    client in one process over loopback exchange a little traffic, then print
    state, disconnect reason, RTT, jitter, loss, bandwidth and counters.

    Run it from the repository root: ./bin/example_stats
*/

#include "rocketnet.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

static int print_client_stats( struct rocketnet_client_t * client )
{
    const double rtt = rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_RTT );
    const double jitter = rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_JITTER );
    const double loss = rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_PACKET_LOSS );
    const double sent = rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_BANDWIDTH_SENT );
    const double received = rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_BANDWIDTH_RECEIVED );
    printf( "client rtt %.2f ms, jitter %.2f ms, loss %.2f%%, bandwidth %.1f kbps sent %.1f kbps received\n", rtt, jitter, loss, sent, received );
    printf( "client bytes sent %llu received %llu\n", (unsigned long long) rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_BYTES_SENT ), (unsigned long long) rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED ) );
    if ( !( rtt >= 0.0 && jitter >= 0.0 && loss >= 0.0 && sent >= 0.0 && received >= 0.0 ) )
        return 0;
    return rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_BYTES_SENT ) > 0;
}

static int run_connect_and_traffic( struct rocketnet_server_t * server, struct rocketnet_client_t * client, double * time_base )
{
    const double start = rocketnet_time();
    static uint8_t payload[32] = { 0 };
    int connected = 0;
    double connected_at = 0.0;

    while ( rocketnet_time() - start < 5.0 )
    {
        const double now = *time_base + ( rocketnet_time() - start );
        // (1) update pumps the transport; (2) receive, release, echo back
        if ( rocketnet_server_update( server, now ) != ROCKETNET_UPDATE_OK )
            return -1;
        if ( rocketnet_client_update( client, now ) != ROCKETNET_UPDATE_OK )
            return -1;
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            struct rocketnet_message_batch_t batch;
            while ( rocketnet_client_receive_messages( client, &batch ) )
            {
                for ( int i = 0; i < batch.num_messages; i++ )
                {
                    data = batch.data[i];
                    bytes = batch.bytes[i];
                }
            }
            {
                uint64_t i = 0;
                struct rocketnet_message_batch_t incoming;
                while ( rocketnet_server_receive_messages( server, &incoming ) )
                {
                    for ( int j = 0; j < incoming.num_messages; j++ )
                    {
                        data = incoming.data[j];
                        bytes = incoming.bytes[j];
                        i = incoming.client_index[j];
                        uint8_t * out = rocketnet_server_start_message( server, i, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
                        if ( out )
                        {
                            memcpy( out, data, (size_t) bytes );
                            rocketnet_server_finish_message( server, i, bytes );
                        }
                    }
                }
            }
        }
        // (3) simulate; (4) queue this frame's outgoing message once connected
        if ( connected )
        {
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) sizeof( payload ) );
            if ( out )
            {
                memcpy( out, payload, sizeof( payload ) );
                rocketnet_client_finish_message( client, (int) sizeof( payload ) );
            }
        }
        // (5) send_packets is last
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
        if ( !connected && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            connected = 1;
            connected_at = rocketnet_time();
        }
        if ( connected && rocketnet_time() - connected_at > 1.5 )
            break;
    }
    *time_base += rocketnet_time() - start;
    return connected ? 0 : -1;
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
    double time_base = 1000.0;
    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_server_dev_config( &server_config );
    server_config.max_clients = 2;
    server_config.transport = "loopback";
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
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
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, NULL, 0, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );
    CHECK( run_connect_and_traffic( server, client, &time_base ) == 0 );
    const int client_index = (int) rocketnet_client_index( client );
    printf( "client state %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    CHECK( print_client_stats( client ) );
    printf( "server client %d rtt %.2f ms, jitter %.2f ms, loss %.2f%%, bandwidth %.1f kbps sent %.1f kbps received\n",
            client_index, rocketnet_server_client_stat( server, client_index, ROCKETNET_SERVER_CLIENT_STAT_RTT ), rocketnet_server_client_stat( server, client_index, ROCKETNET_SERVER_CLIENT_STAT_JITTER ), rocketnet_server_client_stat( server, client_index, ROCKETNET_SERVER_CLIENT_STAT_PACKET_LOSS ), rocketnet_server_client_stat( server, client_index, ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_SENT ), rocketnet_server_client_stat( server, client_index, ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_RECEIVED ) );
    printf( "server client %d bytes sent %llu received %llu\n", client_index, (unsigned long long) rocketnet_server_client_counter( server, client_index, ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT ), (unsigned long long) rocketnet_server_client_counter( server, client_index, ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED ) );
    CHECK( rocketnet_server_client_stat( server, client_index, ROCKETNET_SERVER_CLIENT_STAT_RTT ) >= 0.0 );
    CHECK( rocketnet_server_client_counter( server, client_index, ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT ) > 0 );
    // disconnect, then read the reason on both ends
    rocketnet_client_disconnect( client );
    {
        const double start = rocketnet_time();
        while ( rocketnet_time() - start < 2.0 )
        {
            const double now = time_base + ( rocketnet_time() - start );
            rocketnet_client_update( client, now );
            rocketnet_server_update( server, now );
            rocketnet_client_send_packets( client );
            rocketnet_server_send_packets( server );
            rocketnet_sleep( 0.001 );
            if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED )
                break;
        }
        time_base += rocketnet_time() - start;
    }
    printf( "client after disconnect state %s reason %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ), rocketnet_disconnect_reason_name( rocketnet_client_disconnect_reason( client ) ) );
    printf( "server client %d disconnect reason %s\n", client_index, rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, client_index ) ) );
    result = 0;
cleanup:
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}

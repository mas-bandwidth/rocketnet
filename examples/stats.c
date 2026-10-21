/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Stats: how a game's debug overlay reads connection quality. A server and a
    client in one process over loopback exchange a little traffic, then print
    state, RTT, jitter, loss, bandwidth and counters from one stats call
    per call, then how the connection ended on both ends.

    Run it from the repository root: ./bin/example_stats
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

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// serialize a received message's bytes straight into a started message
static int write_bytes( uint8_t * p, const uint8_t * data, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        p[i] = data[i];
    return bytes;
}

// One call copies the current value of every client stat into a struct the caller
// owns; read the fields from the struct, not with a call per field.
static int print_client_stats( const struct rocketnet_client_t * client )
{
    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( client, &stats );

    printf( "client rtt %.2f ms, jitter %.2f ms, loss %.2f%%, bandwidth %.1f kbps sent %.1f kbps received\n",
            stats.rtt_ms, stats.jitter_ms, stats.packet_loss_percent, stats.bandwidth_sent_kbps, stats.bandwidth_received_kbps );
    printf( "client bytes sent %llu received %llu, messages resent %llu, api misuse %llu\n",
            (unsigned long long) stats.bytes_sent, (unsigned long long) stats.bytes_received,
            (unsigned long long) stats.messages_resent, (unsigned long long) stats.api_misuses );

    if ( !( stats.rtt_ms >= 0.0 && stats.jitter_ms >= 0.0 && stats.packet_loss_percent >= 0.0 &&
            stats.bandwidth_sent_kbps >= 0.0 && stats.bandwidth_received_kbps >= 0.0 ) )
        return 0;
    return stats.bytes_sent > 0;
}

// The server's view of one connection is the same idiom: one call, one struct.
static int print_server_client_stats( const struct rocketnet_server_t * server, int client_index )
{
    struct rocketnet_server_client_stats_t stats;
    rocketnet_server_client_stats( server, client_index, &stats );

    printf( "server client %d rtt %.2f ms, jitter %.2f ms, loss %.2f%%, bandwidth %.1f kbps sent %.1f kbps received\n",
            client_index, stats.rtt_ms, stats.jitter_ms, stats.packet_loss_percent, stats.bandwidth_sent_kbps, stats.bandwidth_received_kbps );
    printf( "server client %d bytes sent %llu received %llu\n",
            client_index, (unsigned long long) stats.bytes_sent, (unsigned long long) stats.bytes_received );

    return stats.rtt_ms >= 0.0 && stats.bytes_sent > 0;
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
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        {
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
            {
                // the echoes only need to arrive; the stats count them
            }
        }
        {
            const struct rocketnet_message_batch_t * incoming = NULL;
            while ( ( incoming = rocketnet_server_receive_messages( server ) ) != NULL )
            {
                for ( int j = 0; j < incoming->num_messages; j++ )
                {
                    const int bytes = incoming->bytes[j];
                    uint8_t * out = rocketnet_server_start_message( server, (uint64_t) incoming->client_index[j], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
                    if ( out )
                    {
                        rocketnet_server_finish_message( server, write_bytes( out, incoming->data[j], bytes ) );
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
                rocketnet_client_finish_message( client, write_bytes( out, payload, (int) sizeof( payload ) ) );
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
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    int result = 1;
    double time_base = 1000.0;
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
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
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_client_stop_and_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );
    CHECK( run_connect_and_traffic( server, client, &time_base ) == 0 );
    // the client's slot on the server; -1 until the client is CONNECTED
    const int client_index = rocketnet_client_index( client );
    CHECK( client_index >= 0 );
    printf( "client state %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    CHECK( print_client_stats( client ) );
    CHECK( print_server_client_stats( server, client_index ) );
    {
        // the server-wide totals are the same idiom again
        struct rocketnet_server_stats_t server_stats;
        rocketnet_server_stats( server, &server_stats );
        printf( "server clients connected %llu, packets sent %llu received %llu\n",
                (unsigned long long) server_stats.connections_accepted,
                (unsigned long long) server_stats.packets_sent, (unsigned long long) server_stats.packets_received );
        CHECK( server_stats.connections_accepted >= 1 );
    }
    // disconnect, then read how the connection ended on both ends: the client
    // reads its state (a negative state names how the server or the protocol
    // ended it), the server keeps a disconnect reason per client slot
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
            if ( !rocketnet_server_client_connected( server, client_index ) )
                break;
        }
        time_base += rocketnet_time() - start;
    }
    printf( "client after disconnect state %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
    printf( "server client %d disconnect reason %s\n", client_index, rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, client_index ) ) );
    CHECK( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    result = 0;
cleanup:
    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}

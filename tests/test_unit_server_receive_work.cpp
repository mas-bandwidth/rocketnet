/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's receive work per packet, counted through the ROCKETNET_TEST_SEAMS
// receive-work seam (rocketnet_server_transport_test_receive_work): the keyed
// hashes of the encryption map, and the writes of payload plaintext. One
// accepted session packet costs exactly one encryption-map lookup, however
// many layers read its mapping, and a payload's plaintext is written once,
// by the open, into the storage the application reads it from. The cases run
// a connected pair on the loopback transport, time passed as an argument from
// 1000.0, no socket.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_server_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE 16
#define SERVER_RECEIVE_WORK_UPDATES 8
#define SERVER_RECEIVE_WORK_PAYLOAD_BYTES 200

static double server_receive_work_clock = 1000.0;

static double server_receive_work_time_callback( void * )
{
    return server_receive_work_clock;
}

struct server_receive_work_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    double time;
};

static bool server_receive_work_pair_setup( struct server_receive_work_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    server_receive_work_clock = pair->time;

    const uint64_t protocol_id = 0x7265637677726b31ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );

    pair->server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    check( pair->server != NULL );
    if ( !pair->server )
        return false;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( pair->server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x5eed5eed5eed5eedULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.step_driven = 1;
    client_config.time_callback = server_receive_work_time_callback;

    pair->client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), pair->time );
    check( pair->client != NULL );
    if ( !pair->client )
        return false;

    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_is_client_connected( pair->server, 0 ) )
            return true;
        pair->time += 0.01;
        server_receive_work_clock = pair->time;
        rocketnet_server_transport_update( pair->server, pair->time );
        rocketnet_client_transport_update( pair->client, pair->time );
        rocketnet_server_transport_send_packets( pair->server );
        rocketnet_client_transport_send_packets( pair->client );
    }
    check( false );
    return false;
}

static void server_receive_work_pair_teardown( struct server_receive_work_pair_t * pair )
{
    rn_test_client_transport_stop_and_destroy( pair->client );
    rn_test_server_transport_stop_and_destroy( pair->server );
}

// One update's work on the server: the client sends its payloads, then the
// server's update receives them. The counts are read around that update alone.

struct server_receive_work_update_t
{
    uint64_t packets_received;
    uint64_t map_hashes;
    uint64_t payload_writes;
    int payloads_delivered;
    bool payloads_match;
};

static void server_receive_work_one_update( struct server_receive_work_pair_t * pair, int round, struct server_receive_work_update_t * out )
{
    for ( int i = 0; i < SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE; i++ )
    {
        uint8_t payload[SERVER_RECEIVE_WORK_PAYLOAD_BYTES];
        memset( payload, (uint8_t) ( round * SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE + i + 1 ), sizeof( payload ) );
        rocketnet_client_transport_send_packet( pair->client, payload, (int) sizeof( payload ) );
    }
    rocketnet_client_transport_send_packets( pair->client );

    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( pair->server );
    const uint64_t packets_before = counters->values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    uint64_t hashes_before = 0;
    uint64_t writes_before = 0;
    rocketnet_server_transport_test_receive_work( pair->server, &hashes_before, &writes_before );

    pair->time += 0.01;
    server_receive_work_clock = pair->time;
    rocketnet_server_transport_update( pair->server, pair->time );

    uint64_t hashes_after = 0;
    uint64_t writes_after = 0;
    rocketnet_server_transport_test_receive_work( pair->server, &hashes_after, &writes_after );

    out->packets_received = counters->values[ROCKETNET_COUNTER_PACKETS_RECEIVED] - packets_before;
    out->map_hashes = hashes_after - hashes_before;
    out->payload_writes = writes_after - writes_before;

    const int * client_index = NULL;
    uint8_t * const * payload_data = NULL;
    const int * payload_bytes = NULL;
    out->payloads_delivered = rocketnet_server_transport_payloads( pair->server, &client_index, &payload_data, &payload_bytes );
    out->payloads_match = true;
    for ( int i = 0; i < out->payloads_delivered; i++ )
    {
        const uint8_t expected = (uint8_t) ( round * SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE + i + 1 );
        if ( client_index[i] != 0 || payload_bytes[i] != SERVER_RECEIVE_WORK_PAYLOAD_BYTES )
        {
            out->payloads_match = false;
            continue;
        }
        for ( int j = 0; j < payload_bytes[i]; j++ )
        {
            if ( payload_data[i][j] != expected )
            {
                out->payloads_match = false;
                break;
            }
        }
    }

    rocketnet_server_transport_send_packets( pair->server );
    rocketnet_client_transport_update( pair->client, pair->time );
}

static void test_server_receive_one_lookup_per_packet()
{
    struct server_receive_work_pair_t pair;
    check( server_receive_work_pair_setup( &pair ) );

    uint64_t packets_total = 0;
    for ( int round = 0; round < SERVER_RECEIVE_WORK_UPDATES; round++ )
    {
        struct server_receive_work_update_t update;
        server_receive_work_one_update( &pair, round, &update );
        check( update.payloads_delivered == SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE );
        check( update.payloads_match );
        check( update.packets_received >= (uint64_t) SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE );
        // every packet in this update is an accepted session packet from a
        // connected client: one keyed hash each, and no other
        check( update.map_hashes == update.packets_received );
        packets_total += update.packets_received;
    }
    check( packets_total >= (uint64_t) ( SERVER_RECEIVE_WORK_UPDATES * SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE ) );

    server_receive_work_pair_teardown( &pair );

    printf( "test_server_receive_one_lookup_per_packet\n" );
}

REGISTER_UNIT_TEST( test_server_receive_one_lookup_per_packet );

static void test_server_receive_payload_one_copy()
{
    struct server_receive_work_pair_t pair;
    check( server_receive_work_pair_setup( &pair ) );

    for ( int round = 0; round < SERVER_RECEIVE_WORK_UPDATES; round++ )
    {
        struct server_receive_work_update_t update;
        server_receive_work_one_update( &pair, round, &update );
        check( update.payloads_delivered == SERVER_RECEIVE_WORK_PACKETS_PER_UPDATE );
        check( update.payloads_match );
        // the open writes each payload's plaintext into the storage the
        // application reads, and nothing copies it again
        check( update.payload_writes == (uint64_t) update.payloads_delivered );
    }

    server_receive_work_pair_teardown( &pair );

    printf( "test_server_receive_payload_one_copy\n" );
}

REGISTER_UNIT_TEST( test_server_receive_payload_one_copy );

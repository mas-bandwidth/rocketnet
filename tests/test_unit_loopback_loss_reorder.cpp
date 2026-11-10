/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Deterministic loss and reorder over the loopback transport: the client and
// server transport configs carry simulated_drop_every_nth (every Nth outgoing
// datagram is lost) and simulated_reverse_window (each window of W outgoing
// datagrams is delivered in reverse, the window flushed whole at the end of
// each send_packets). Both count from the endpoint's first send and use no
// random draw (proofs/tla/Link.tla "everynth" and ReverseWindow). The cases run on
// the loopback transport, with time passed as an argument from 1000.0, no socket.
//
// test_loopback_transport_patterns_off_same_trace compares a fixed run, with both fields zero
// and with the link simulator on and off, against counters recorded at the base
// (sprint/rocketnet-proof cac844d) before this change existed.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "portable_crt.h"

#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_client_io.h"
#include "rocketnet_counters.h"
#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"
#include "rocketnet_network_simulator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

#if ROCKETNET_DEV
static double loopback_loss_reorder_clock = 1000.0;

static double loopback_loss_reorder_time_callback( void * )
{
    return loopback_loss_reorder_clock;
}

struct loopback_transport_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    double time;
};

// the patterns of one endpoint, and the link simulator's latency
struct loopback_pattern_t
{
    int drop_every_nth;
    int reverse_window;
    int latency_ms;
};

static bool loopback_transport_pair_setup( struct loopback_transport_pair_t * pair,
                                           struct loopback_pattern_t client_pattern,
                                           struct loopback_pattern_t server_pattern )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    loopback_loss_reorder_clock = pair->time;

    const uint64_t protocol_id = 0x6c6f6f706261636bULL;
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
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    server_config.simulated_latency_ms = server_pattern.latency_ms;
    server_config.simulated_drop_every_nth = server_pattern.drop_every_nth;
    server_config.simulated_reverse_window = server_pattern.reverse_window;

    pair->server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    check( pair->server != NULL );
    if ( !pair->server )
        return false;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( pair->server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x9e3779b97f4a7c15ULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = loopback_loss_reorder_time_callback;
    client_config.simulated_latency_ms = client_pattern.latency_ms;
    client_config.simulated_drop_every_nth = client_pattern.drop_every_nth;
    client_config.simulated_reverse_window = client_pattern.reverse_window;

    pair->client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), pair->time );
    check( pair->client != NULL );
    return pair->client != NULL;
}

static void loopback_transport_pair_step( struct loopback_transport_pair_t * pair, double dt )
{
    pair->time += dt;
    loopback_loss_reorder_clock = pair->time;
    rocketnet_server_transport_update( pair->server, pair->time );
    rocketnet_client_transport_update( pair->client, pair->time );
    rocketnet_server_transport_send_packets( pair->server );
    rocketnet_client_transport_send_packets( pair->client );
}

static bool loopback_transport_pair_connect( struct loopback_transport_pair_t * pair )
{
    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_is_client_connected( pair->server, 0 ) )
            return true;
        loopback_transport_pair_step( pair, 0.01 );
    }

    return false;
}

static void loopback_transport_pair_teardown( struct loopback_transport_pair_t * pair )
{
    rn_test_client_transport_stop_and_destroy( pair->client );
    rn_test_server_transport_stop_and_destroy( pair->server );
}

// ---- the fixed run, recorded at the base ------------------------------------

struct loopback_trace_t
{
    uint64_t client_packets_sent;
    uint64_t client_packets_received;
    uint64_t client_bytes_sent;
    uint64_t client_bytes_received;
    uint64_t server_packets_sent;
    uint64_t server_packets_received;
    uint64_t server_bytes_sent;
    uint64_t server_bytes_received;
    uint64_t client_counters_hash;
    uint64_t server_counters_hash;
    int connected_step;
};

static uint64_t loopback_counters_hash( const struct rocketnet_counters_t * counters )
{
    uint64_t hash = UINT64_C( 0xcbf29ce484222325 );

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        for ( int b = 0; b < 8; b++ )
        {
            hash ^= ( counters->values[i] >> ( 8 * b ) ) & 0xff;
            hash *= UINT64_C( 0x100000001b3 );
        }
    }

    return hash;
}

// connect, then 32 rounds of one payload each way, stepped by 10 ms

static void loopback_run_fixed_trace( int latency_ms, struct loopback_trace_t * out )
{
    struct loopback_pattern_t pattern = { 0, 0, latency_ms };
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, pattern, pattern ) );

    int connected_step = -1;

    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_is_client_connected( pair.server, 0 ) )
        {
            connected_step = i;
            break;
        }

        loopback_transport_pair_step( &pair, 0.01 );
    }

    check( connected_step >= 0 );

    for ( int i = 0; i < 32; i++ )
    {
        uint8_t payload[8];
        memset( payload, (uint8_t) i, sizeof( payload ) );
        rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
        rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
        loopback_transport_pair_step( &pair, 0.01 );
    }

    loopback_transport_pair_step( &pair, 0.01 );

    struct rocketnet_client_transport_stats_t cs;
    struct rocketnet_server_transport_stats_t ss;
    rocketnet_client_transport_stats( pair.client, &cs );
    rocketnet_server_transport_stats( pair.server, &ss );
    out->client_packets_sent = cs.packets_sent;
    out->client_packets_received = cs.packets_received;
    out->client_bytes_sent = cs.bytes_sent;
    out->client_bytes_received = cs.bytes_received;
    out->server_packets_sent = ss.packets_sent;
    out->server_packets_received = ss.packets_received;
    out->server_bytes_sent = ss.bytes_sent;
    out->server_bytes_received = ss.bytes_received;
    out->client_counters_hash = loopback_counters_hash( rocketnet_client_transport_counters( pair.client ) );
    out->server_counters_hash = loopback_counters_hash( rocketnet_server_transport_counters( pair.server ) );
    out->connected_step = connected_step;

    loopback_transport_pair_teardown( &pair );
}

static void loopback_print_trace( const char * name, const struct loopback_trace_t * t )
{
    char print_trace[256];
    if ( !rocketnet_test_getenv( "ROCKETNET_LOOPBACK_PRINT_TRACE", print_trace, sizeof( print_trace ) ) )
        return;
    printf( "TRACE %s { %llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu, UINT64_C( 0x%016llx ), UINT64_C( 0x%016llx ), %d }\n", name,
            (unsigned long long) t->client_packets_sent, (unsigned long long) t->client_packets_received,
            (unsigned long long) t->client_bytes_sent, (unsigned long long) t->client_bytes_received,
            (unsigned long long) t->server_packets_sent, (unsigned long long) t->server_packets_received,
            (unsigned long long) t->server_bytes_sent, (unsigned long long) t->server_bytes_received,
            (unsigned long long) t->client_counters_hash, (unsigned long long) t->server_counters_hash, t->connected_step );
}

static void loopback_check_trace( const struct loopback_trace_t * got, const struct loopback_trace_t * want )
{
    check( got->client_packets_sent == want->client_packets_sent );
    check( got->client_packets_received == want->client_packets_received );
    check( got->client_bytes_sent == want->client_bytes_sent );
    check( got->client_bytes_received == want->client_bytes_received );
    check( got->server_packets_sent == want->server_packets_sent );
    check( got->server_packets_received == want->server_packets_received );
    check( got->server_bytes_sent == want->server_bytes_sent );
    check( got->server_bytes_received == want->server_bytes_received );
    check( got->client_counters_hash == want->client_counters_hash );
    check( got->server_counters_hash == want->server_counters_hash );
    check( got->connected_step == want->connected_step );
}

// The fixed traces: every eliciting packet gets a standalone ACK on the next
// update, and simulator latency adds two connected steps (24 without it, 26
// with it). The hashes cover every counter of the registry in index order.
static const struct loopback_trace_t loopback_base_trace_simulator_off = { 67, 66, 5954, 4320, 67, 66, 4389, 5889, UINT64_C( 0xb5e293d9f9886527 ), UINT64_C( 0x9f8c8641bf5a96c5 ), 24 };
static const struct loopback_trace_t loopback_base_trace_simulator_on = { 68, 66, 5775, 4328, 67, 66, 4389, 5653, UINT64_C( 0x413d3c77f2865567 ), UINT64_C( 0x9f8c8641bf5a96c5 ), 26 };

static void test_loopback_transport_patterns_off_same_trace()
{
    struct loopback_trace_t off;
    struct loopback_trace_t on;
    loopback_run_fixed_trace( 0, &off );
    loopback_run_fixed_trace( 5, &on );
    loopback_print_trace( "simulator_off", &off );
    loopback_print_trace( "simulator_on", &on );

    loopback_check_trace( &off, &loopback_base_trace_simulator_off );
    loopback_check_trace( &on, &loopback_base_trace_simulator_on );
}

REGISTER_UNIT_TEST( test_loopback_transport_patterns_off_same_trace );

// ---- the pattern cases (they need the config fields) ------------------------

static void test_loopback_transport_drop_every_nth()
{
    struct loopback_pattern_t client_pattern = { 7, 0, 0 };
    struct loopback_pattern_t server_pattern = { 5, 0, 0 };
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, client_pattern, server_pattern ) );
    check( loopback_transport_pair_connect( &pair ) );

    struct rocketnet_client_transport_stats_t cstats;
    struct rocketnet_server_transport_stats_t sstats;

    // every datagram the client sends, handshake included, is counted from
    // its first send: the 7th, 14th, ... are lost and the server counts the
    // others; the server's own sends, counted the same way, lose 5th, 10th, ...

    for ( int i = 0; i < 56; i++ )
    {
        rocketnet_client_transport_stats( pair.client, &cstats );
        rocketnet_server_transport_stats( pair.server, &sstats );

        const uint64_t client_sent_before = cstats.packets_sent;
        const uint64_t server_sent_before = sstats.packets_sent;

        uint8_t payload[4];
        const uint32_t seq = (uint32_t) ( i + 1 );
        memcpy( payload, &seq, sizeof( seq ) );
        rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
        rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
        rocketnet_client_transport_send_packets( pair.client );
        rocketnet_server_transport_send_packets( pair.server );

        rocketnet_client_transport_stats( pair.client, &cstats );
        rocketnet_server_transport_stats( pair.server, &sstats );

        const uint64_t client_number = cstats.packets_sent;
        const uint64_t server_number = sstats.packets_sent;
        check( client_number == client_sent_before + 1 );
        check( server_number == server_sent_before + 1 );

        pair.time += 0.001;
        loopback_loss_reorder_clock = pair.time;
        rocketnet_client_transport_update( pair.client, pair.time );
        rocketnet_server_transport_update( pair.server, pair.time );
        rocketnet_client_transport_update( pair.client, pair.time );
        rocketnet_server_transport_update( pair.server, pair.time );
        rocketnet_client_transport_stats( pair.client, &cstats );
        rocketnet_server_transport_stats( pair.server, &sstats );

        // Next-update ACKs may add datagrams during either update. Check the
        // exact cumulative simulator loss after both client and server have had that update.

        rocketnet_client_transport_stats( pair.client, &cstats );
        rocketnet_server_transport_stats( pair.server, &sstats );
    }

    // drain the standalone ACKs generated by the final data update before
    // checking the exact periodic-drop accounting for every transport send

    rocketnet_client_transport_update( pair.client, pair.time );
    rocketnet_server_transport_update( pair.server, pair.time );
    rocketnet_client_transport_stats( pair.client, &cstats );
    rocketnet_server_transport_stats( pair.server, &sstats );
    check( sstats.packets_received == cstats.packets_sent - cstats.packets_sent / 7 );
    check( cstats.packets_received == sstats.packets_sent - sstats.packets_sent / 5 );

    loopback_transport_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_loopback_transport_drop_every_nth );

static void loopback_send_window( struct loopback_transport_pair_t * pair, uint8_t first, uint8_t count )
{
    for ( uint8_t v = first; v < first + count; v++ )
    {
        struct rocketnet_packet_context_t ctx;
        check( rocketnet_client_transport_start_packet( pair->client, 1, &ctx ) != NULL );

        int cap = 0;
        uint8_t * buf = rocketnet_client_transport_packet_payload( pair->client, &ctx, &cap );
        check( buf != NULL && cap >= 1 );
        buf[0] = v;
        check( rocketnet_client_transport_finish_packet( pair->client, &ctx, 1 ) > 0 );
    }

    rocketnet_client_transport_send_packets( pair->client );

    pair->time += 0.001;
    loopback_loss_reorder_clock = pair->time;
    rocketnet_client_transport_update( pair->client, pair->time );
    rocketnet_server_transport_update( pair->server, pair->time );
}

static void test_loopback_transport_reverse_window()
{
    struct loopback_pattern_t client_pattern = { 0, 4, 0 };
    struct loopback_pattern_t server_pattern = { 0, 0, 0 };
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, client_pattern, server_pattern ) );
    check( loopback_transport_pair_connect( &pair ) );
    rocketnet_server_transport_update( pair.server, pair.time );

    const int * client_indices = NULL;
    uint8_t * const * payloads = NULL;
    const int * payload_bytes = NULL;

    // datagrams 1..4 arrive 4, 3, 2, 1

    loopback_send_window( &pair, 1, 4 );

    int count = rocketnet_server_transport_payloads( pair.server, &client_indices, &payloads, &payload_bytes );
    check( count == 4 );

    const uint8_t expected[4] = { 4, 3, 2, 1 };

    for ( int i = 0; i < count && i < 4; i++ )
    {
        check( payload_bytes[i] == 1 );
        check( payloads[i][0] == expected[i] );
    }

    // a window left short at the end of send_packets is flushed whole, and
    // nothing is held across updates: 5, 6 arrive 6, 5

    loopback_send_window( &pair, 5, 2 );
    count = rocketnet_server_transport_payloads( pair.server, &client_indices, &payloads, &payload_bytes );
    check( count == 2 );

    if ( count == 2 )
    {
        check( payloads[0][0] == 6 );
        check( payloads[1][0] == 5 );
    }

    // more than one window in a send_packets: 7..10 then 11, 12

    loopback_send_window( &pair, 7, 6 );
    count = rocketnet_server_transport_payloads( pair.server, &client_indices, &payloads, &payload_bytes );
    check( count == 6 );

    const uint8_t expected_six[6] = { 10, 9, 8, 7, 12, 11 };
    for ( int i = 0; i < count && i < 6; i++ )
    {
        check( payloads[i][0] == expected_six[i] );
    }

    loopback_transport_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_loopback_transport_reverse_window );

// A payload that opens when the server's payload batch is full is counted
// PAYLOAD_BATCH_FULL. The batch holds one entry per packet the update takes,
// so the unit seam lowers its capacity to two and three payloads arrive in
// one update: the first two are delivered in order and the third is counted.
// The client's slot always exists (ROCKETNET_CLIENT_UPDATE_PACKET_BUDGET), so
// the three payloads sent the other way in the same update all arrive.

extern void rocketnet_server_transport_set_payload_batch_capacity_for_test( int payloads );

static void test_loopback_transport_payload_batch_full_is_counted()
{
    struct loopback_pattern_t pattern = { 0, 0, 0 };
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, pattern, pattern ) );
    check( loopback_transport_pair_connect( &pair ) );
    loopback_transport_pair_step( &pair, 0.01 );

    const uint64_t server_full_before = rocketnet_server_transport_counters( pair.server )->values[ROCKETNET_COUNTER_PAYLOAD_BATCH_FULL];
    check( server_full_before == 0 );

    rocketnet_server_transport_set_payload_batch_capacity_for_test( 2 );

    for ( uint8_t i = 1; i <= 3; i++ )
    {
        rocketnet_client_transport_send_packet( pair.client, &i, 1 );
        rocketnet_server_transport_send_packet( pair.server, 0, &i, 1 );
    }
    rocketnet_client_transport_send_packets( pair.client );
    rocketnet_server_transport_send_packets( pair.server );

    pair.time += 0.01;
    loopback_loss_reorder_clock = pair.time;
    rocketnet_server_transport_update( pair.server, pair.time );
    rocketnet_client_transport_update( pair.client, pair.time );

    const int * client_indices = NULL;
    uint8_t * const * server_payloads = NULL;
    const int * server_payload_bytes = NULL;
    const int server_count = rocketnet_server_transport_payloads( pair.server, &client_indices, &server_payloads, &server_payload_bytes );

    uint8_t * const * client_payloads = NULL;
    const int * client_payload_bytes = NULL;
    const int client_count = rocketnet_client_transport_payloads( pair.client, &client_payloads, &client_payload_bytes );

    const uint64_t server_full = rocketnet_server_transport_counters( pair.server )->values[ROCKETNET_COUNTER_PAYLOAD_BATCH_FULL];

    rocketnet_server_transport_set_payload_batch_capacity_for_test( 0 );

    if ( server_count != 2 || client_count != 3 || server_full != 1 )
        printf( "test_loopback_transport_payload_batch_full_is_counted: server %d payloads, PAYLOAD_BATCH_FULL %llu; client %d payloads\n",
                server_count, (unsigned long long) server_full, client_count );

    check( server_count == 2 && client_count == 3 );
    check( server_full == 1 );

    for ( int i = 0; i < 2 && i < server_count; i++ )
        check( server_payload_bytes[i] == 1 && server_payloads[i][0] == (uint8_t) ( i + 1 ) );

    for ( int i = 0; i < 3 && i < client_count; i++ )
        check( client_payload_bytes[i] == 1 && client_payloads[i][0] == (uint8_t) ( i + 1 ) );

    loopback_transport_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( test_loopback_transport_payload_batch_full_is_counted );

static void test_loopback_transport_reliable_ordered_survives_loss_and_reorder()
{
    struct loopback_pattern_t pattern = { 7, 4, 0 };
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, pattern, pattern ) );

    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    message_config.num_channels = 4;
    message_config.reliable_messages_in_flight = 64;
    message_config.receive_message_buffer_bytes = 65536;
    message_config.max_message_bytes = 16384;
    message_config.send_message_buffer_bytes = 65536;
    message_config.send_queue_messages = 1024;
    message_config.packets_in_flight = 64;
    message_config.packets_per_update = 8;
    message_config.unreliable_messages_per_update = 64;
    message_config.message_resend_delay_initial_ms = 100;
    message_config.message_resend_delay_min_ms = 20;
    message_config.message_resend_delay_max_ms = 400;
    message_config.reliable_message_timeout_ms = 120000;

    struct rn_message_link_t server_link;
    rn_message_server_link( &server_link, pair.server );

    struct rn_message_object_t * server = rn_message_object_create(
        NULL, &server_link, 1, 1, &message_config, NULL, 0, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, pair.time );
    check( server != NULL );

    struct rn_message_link_t client_link;
    rn_message_client_link( &client_link, pair.client );
    client_link.clock_from_first_update = 0;

    struct rn_message_object_t * client = rn_message_object_create(
        NULL, &client_link, 0, 1, &message_config, NULL, 0, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, pair.time );
    check( client != NULL );

    const int total_messages = 1000;
    int sent = 0;
    int received = 0;
    for ( int step = 0; step < 8000 && received < total_messages; step++ )
    {
        pair.time += 0.005;
        loopback_loss_reorder_clock = pair.time;
        check( rn_message_object_update( server, pair.time ) == ROCKETNET_UPDATE_OK );
        check( rn_message_object_update( client, pair.time ) == ROCKETNET_UPDATE_OK );

        if ( rn_message_object_connected( client, 0 ) && rn_message_object_connected( server, 0 ) )
        {
            while ( sent < total_messages && sent - received < 32 )
            {
                uint8_t * out = rn_message_object_start( client, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
                if ( !out )
                    break;

                const uint32_t seq = (uint32_t) sent;
                memcpy( out, &seq, sizeof( seq ) );
                check( rn_message_object_finish( client, 0, 4 ) != 0 );
                sent++;
            }
        }

        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rn_message_object_receive( server, NULL, NULL, &data, &bytes ) )
        {
            check( bytes == 4 );

            uint32_t seq = 0;
            memcpy( &seq, data, sizeof( seq ) );
            check( (int) seq == received );
            received++;
            rn_message_object_release( server, data );
        }

        rn_message_object_send_packets( server );
        rn_message_object_send_packets( client );
    }

    check( sent == total_messages );
    check( received == total_messages );

    // the stream was lost from, and reordered: each endpoint's receiver saw
    // exactly the datagrams the other endpoint sent less every 7th

    struct rocketnet_client_transport_stats_t cstats;
    struct rocketnet_server_transport_stats_t sstats;
    rocketnet_client_transport_stats( pair.client, &cstats );
    rocketnet_server_transport_stats( pair.server, &sstats );

    // Message acceptance publishes ACK eligibility after transport update.
    // Drain the finite ACK exchange instead of assuming one update suffices.

    for ( int drain = 0; drain < 16; ++drain )
    {
        pair.time += 0.005;
        loopback_loss_reorder_clock = pair.time;
        check( rn_message_object_update( server, pair.time ) == ROCKETNET_UPDATE_OK );
        check( rn_message_object_update( client, pair.time ) == ROCKETNET_UPDATE_OK );
        rn_message_object_send_packets( server );
        rn_message_object_send_packets( client );
        rocketnet_client_transport_stats( pair.client, &cstats );
        rocketnet_server_transport_stats( pair.server, &sstats );
        if ( sstats.packets_received == cstats.packets_sent - cstats.packets_sent / 7 &&
             cstats.packets_received == sstats.packets_sent - sstats.packets_sent / 7 )
            break;
    }

    check( sstats.packets_received == cstats.packets_sent - cstats.packets_sent / 7 );
    check( cstats.packets_received == sstats.packets_sent - sstats.packets_sent / 7 );

    rocketnet_client_transport_stop( pair.client );
    rn_test_client_transport_wait_stopped( pair.client );
    rocketnet_server_transport_stop( pair.server );
    check( rocketnet_server_transport_stopped( pair.server ) );
    rn_message_object_destroy( client );
    rn_message_object_destroy( server );
}

REGISTER_UNIT_TEST( test_loopback_transport_reliable_ordered_survives_loss_and_reorder );
#endif // #if ROCKETNET_DEV

#if !ROCKETNET_DEV
// ---- the production refusal and the ranges ----------------------------------

static int loopback_error_lines = 0;
static int loopback_error_lines_naming = 0;
static const char * loopback_error_field = NULL;

static void loopback_error_capture( void *, enum rocketnet_log_level_t level, const char * message, size_t )
{
    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
        return;
    loopback_error_lines++;
    if ( loopback_error_field && strstr( message, loopback_error_field ) )
    {
        loopback_error_lines_naming++;
    }
}

static void loopback_error_begin( const char * field )
{
    loopback_error_lines = 0;
    loopback_error_lines_naming = 0;
    loopback_error_field = field;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_log_function( loopback_error_capture, NULL );
}

static void loopback_error_end()
{
    rocketnet_log_function( NULL, NULL );
}

static void loopback_signed_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = UINT64_C( 1 ) << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static void loopback_fields_present( const struct rocketnet_config_field_t * fields, int num_fields )
{
    int drop = 0;
    int reverse = 0;

    for ( int i = 0; i < num_fields; i++ )
    {
        if ( strcmp( fields[i].name, "simulated_drop_every_nth" ) == 0 )
        {
            drop++;
        }
        if ( strcmp( fields[i].name, "simulated_reverse_window" ) == 0 )
        {
            reverse++;
        }
    }

    check( drop == 1 );
    check( reverse == 1 );
}

static void test_loopback_transport_loss_reorder_production_refusal()
{
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    check( client_config.simulated_drop_every_nth == 0 );
    check( client_config.simulated_reverse_window == 0 );

    int client_fields = 0;
    const struct rocketnet_config_field_t * client_table = rocketnet_client_transport_config_fields( &client_fields );
    loopback_fields_present( client_table, client_fields );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    check( server_config.simulated_drop_every_nth == 0 );
    check( server_config.simulated_reverse_window == 0 );

    int server_fields = 0;
    const struct rocketnet_config_field_t * server_table = rocketnet_server_transport_config_fields( &server_fields );
    loopback_fields_present( server_table, server_fields );

    // zero is off in both builds

    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );

    // a nonzero value is refused by the production walk, with one log line
    // naming the field, and admitted by the development one

    loopback_error_begin( "simulated_drop_every_nth" );
    client_config.simulated_drop_every_nth = 7;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( loopback_error_lines == 1 );
    check( loopback_error_lines_naming == 1 );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );
    client_config.simulated_drop_every_nth = 0;

    loopback_error_begin( "simulated_reverse_window" );
    server_config.simulated_reverse_window = 4;
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    check( loopback_error_lines == 1 );
    check( loopback_error_lines_naming == 1 );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );
    server_config.simulated_reverse_window = 0;
    loopback_error_end();

    // the same through the create: a production library (the development
    // posture off) refuses the struct with the config-out-of-range status.
    // Environment overlay mechanics are development-hook tests and do not
    // compile into this production-linked unit suite.

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    loopback_signed_token( token );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.simulated_reverse_window = 4;
    loopback_error_begin( "simulated_reverse_window" );
    check( rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), 1000.0 ) == NULL );
    check( loopback_error_lines_naming == 1 );
    loopback_error_end();
    client_config.simulated_reverse_window = 0;

    // Real environment create refusals run in test_platform_primitives.
    // ranges: zero, or N >= 2 up to 1000000; zero, or W >= 2 up to 64. each
    // refusal logs one error line; the capture keeps those lines (and the
    // alias refusals an ambient environment adds) off stdout

    loopback_error_begin( NULL );

    const int bad_drop[] = { -1, 1, 1000001 };
    const int good_drop[] = { 0, 2, 7, 1000000 };
    const int bad_window[] = { -1, 1, 65 };
    const int good_window[] = { 0, 2, 4, 64 };

    for ( int i = 0; i < 3; i++ )
    {
        client_config.simulated_drop_every_nth = bad_drop[i];
        check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
        client_config.simulated_drop_every_nth = 0;
        server_config.simulated_drop_every_nth = bad_drop[i];
        check( !rocketnet_config_validate( "server", &server_config, server_table, server_fields ) );
        server_config.simulated_drop_every_nth = 0;
        client_config.simulated_reverse_window = bad_window[i];
        check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
        client_config.simulated_reverse_window = 0;
        server_config.simulated_reverse_window = bad_window[i];
        check( !rocketnet_config_validate( "server", &server_config, server_table, server_fields ) );
        server_config.simulated_reverse_window = 0;
    }

    for ( int i = 0; i < 4; i++ )
    {
        client_config.simulated_drop_every_nth = good_drop[i];
        check( rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
        client_config.simulated_drop_every_nth = 0;
        server_config.simulated_drop_every_nth = good_drop[i];
        check( rocketnet_config_validate( "server", &server_config, server_table, server_fields ) );
        server_config.simulated_drop_every_nth = 0;
        client_config.simulated_reverse_window = good_window[i];
        check( rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
        client_config.simulated_reverse_window = 0;
        server_config.simulated_reverse_window = good_window[i];
        check( rocketnet_config_validate( "server", &server_config, server_table, server_fields ) );
        server_config.simulated_reverse_window = 0;
    }

    // real environment overlay checks run in test_platform_primitives

    check( loopback_error_lines >= 12 );
    loopback_error_end();
}

REGISTER_UNIT_TEST( test_loopback_transport_loss_reorder_production_refusal );
#endif // !ROCKETNET_DEV: a development build accepts what this checks is refused

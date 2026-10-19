/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Deterministic periodic packet drop (every Nth outgoing datagram) and
// window-reversal reordering (each window of W outgoing datagrams emitted in
// reverse order, flushed whole at the end of send_packets) over the loopback
// transport, plus reliable-ordered convergence of 1000 messages under N = 7
// and W = 4, trace identity when both are zero, and production / range refusal.

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_client_io.h"
#include "rocketnet_counters.h"
#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"
#include "rocketnet_network_simulator.h"
#include "test/fake_client_driver.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static double loopback_loss_reorder_clock = 1000.0;

static double loopback_loss_reorder_time_callback( void * )
{
    return loopback_loss_reorder_clock;
}

static void loopback_loss_reorder_signed_token( uint8_t * token )
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

struct loopback_transport_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    double time;
};

static bool loopback_transport_pair_setup( struct loopback_transport_pair_t * pair, int drop_every_nth, int reverse_window )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    loopback_loss_reorder_clock = pair->time;

    rocketnet_public_link_simulator_test_development( 1 );
    rocketnet_loopback_set_patterns( drop_every_nth, reverse_window );

    const uint64_t protocol_id = 0x6c6f6f706261636bULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.driver, sizeof( server_config.driver ), "%s", "loopback" );

    int status = -1;
    pair->server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, pair->time, &status );
    check( status == ROCKETNET_STATUS_OK );
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
    snprintf( client_config.driver, sizeof( client_config.driver ), "%s", "loopback" );
    client_config.step_driven = 1;
    client_config.time_callback = loopback_loss_reorder_time_callback;

    status = -1;
    pair->client = rocketnet_client_transport_create_ex( NULL, token, &client_config, pair->time, &status );
    check( status == ROCKETNET_STATUS_OK );
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
    rocketnet_client_transport_destroy( pair->client );
    rocketnet_server_transport_destroy( pair->server );
    rocketnet_loopback_set_patterns( 0, 0 );
    rocketnet_public_link_simulator_test_development( 0 );
}

static void loopback_drop_every_nth()
{
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, 7, 0 ) );
    check( loopback_transport_pair_connect( &pair ) );

    struct rocketnet_client_transport_stats_t cstats;
    struct rocketnet_server_transport_stats_t sstats;

    for ( int i = 0; i < 28; i++ )
    {
        rocketnet_client_transport_stats( pair.client, &cstats );
        rocketnet_server_transport_stats( pair.server, &sstats );
        const uint64_t sent_before = cstats.packets_sent;
        const uint64_t recv_before = sstats.packets_received;

        uint8_t payload[4];
        const uint32_t seq = (uint32_t) ( i + 1 );
        memcpy( payload, &seq, sizeof( seq ) );
        rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
        rocketnet_client_transport_send_packets( pair.client );

        rocketnet_client_transport_stats( pair.client, &cstats );
        const uint64_t send_number = cstats.packets_sent;
        check( send_number == sent_before + 1 );

        pair.time += 0.001;
        loopback_loss_reorder_clock = pair.time;
        rocketnet_client_transport_update( pair.client, pair.time );
        rocketnet_server_transport_update( pair.server, pair.time );
        rocketnet_server_transport_stats( pair.server, &sstats );

        if ( ( send_number % 7 ) == 0 )
        {
            check( sstats.packets_received == recv_before );
        }
        else
        {
            check( sstats.packets_received == recv_before + 1 );
        }
        check( sstats.packets_received == send_number - ( send_number / 7 ) );
    }

    loopback_transport_pair_teardown( &pair );
    printf( "loopback_drop_every_nth\n" );
}

static void loopback_reverse_window()
{
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, 0, 4 ) );
    check( loopback_transport_pair_connect( &pair ) );

    rocketnet_server_transport_update( pair.server, pair.time );

    for ( uint8_t v = 1; v <= 4; v++ )
    {
        struct rocketnet_packet_context_t ctx;
        check( rocketnet_client_transport_start_packet( pair.client, 1, &ctx ) != NULL );
        int cap = 0;
        uint8_t * buf = rocketnet_client_transport_packet_payload( pair.client, &ctx, &cap );
        check( buf != NULL && cap >= 1 );
        buf[0] = v;
        check( rocketnet_client_transport_finish_packet( pair.client, &ctx, 1 ) > 0 );
    }
    rocketnet_client_transport_send_packets( pair.client );

    pair.time += 0.001;
    loopback_loss_reorder_clock = pair.time;
    rocketnet_client_transport_update( pair.client, pair.time );
    rocketnet_server_transport_update( pair.server, pair.time );

    const int * client_indices = NULL;
    uint8_t * const * payloads = NULL;
    const int * payload_bytes = NULL;
    const int count = rocketnet_server_transport_payloads( pair.server, &client_indices, &payloads, &payload_bytes );
    check( count == 4 );
    const uint8_t expected[4] = { 4, 3, 2, 1 };
    for ( int i = 0; i < 4; i++ )
    {
        check( payload_bytes[i] == 1 );
        check( payloads[i][0] == expected[i] );
    }

    for ( uint8_t v = 5; v <= 6; v++ )
    {
        struct rocketnet_packet_context_t ctx;
        check( rocketnet_client_transport_start_packet( pair.client, 1, &ctx ) != NULL );
        int cap = 0;
        uint8_t * buf = rocketnet_client_transport_packet_payload( pair.client, &ctx, &cap );
        check( buf != NULL && cap >= 1 );
        buf[0] = v;
        check( rocketnet_client_transport_finish_packet( pair.client, &ctx, 1 ) > 0 );
    }
    rocketnet_client_transport_send_packets( pair.client );

    pair.time += 0.001;
    loopback_loss_reorder_clock = pair.time;
    rocketnet_client_transport_update( pair.client, pair.time );
    rocketnet_server_transport_update( pair.server, pair.time );

    const int count2 = rocketnet_server_transport_payloads( pair.server, &client_indices, &payloads, &payload_bytes );
    check( count2 == 2 );
    check( payloads[0][0] == 6 );
    check( payloads[1][0] == 5 );

    loopback_transport_pair_teardown( &pair );
    printf( "loopback_reverse_window\n" );
}

static void loopback_reliable_ordered_survives_loss_and_reorder()
{
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, 7, 4 ) );

    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    message_config.num_channels = 4;
    message_config.window_messages = 64;
    message_config.receive_ring_bytes = 65536;
    message_config.max_message_bytes = 16384;
    message_config.send_buffer_bytes = 65536;
    message_config.send_queue_messages = 1024;
    message_config.packets_in_flight = 64;
    message_config.max_packets_per_update = 8;
    message_config.unreliable_deliveries = 64;
    message_config.rto_initial_ms = 100;
    message_config.rto_min_ms = 20;
    message_config.rto_max_ms = 400;
    message_config.reliable_timeout_ms = 120000;

    struct rn_message_link_t server_link;
    rn_message_server_link( &server_link, pair.server );
    struct rocketnet_server_t * server = (struct rocketnet_server_t *) rn_message_object_create(
        NULL, &server_link, 1, 1, &message_config, NULL, 0, ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS, pair.time );
    check( server != NULL );

    struct rn_message_link_t client_link;
    rn_message_client_link( &client_link, pair.client );
    client_link.clock_from_first_update = 0;
    struct rocketnet_client_t * client = (struct rocketnet_client_t *) rn_message_object_create(
        NULL, &client_link, 0, 1, &message_config, NULL, 0, ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, pair.time );
    check( client != NULL );

    const int total_messages = 1000;
    int sent = 0;
    int received = 0;

    for ( int step = 0; step < 8000 && received < total_messages; step++ )
    {
        pair.time += 0.005;
        loopback_loss_reorder_clock = pair.time;
        check( rocketnet_server_update( server, pair.time ) == ROCKETNET_MESSAGE_OK );
        check( rocketnet_client_update( client, pair.time ) == ROCKETNET_MESSAGE_OK );

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_client_connected( server, 0 ) )
        {
            while ( sent < total_messages && sent - received < 32 )
            {
                uint8_t * out = rocketnet_client_start( client, 0, ROCKETNET_RELIABLE_ORDERED, 4 );
                if ( !out )
                    break;
                const uint32_t seq = (uint32_t) sent;
                memcpy( out, &seq, sizeof( seq ) );
                check( rocketnet_client_finish( client, 4 ) != 0 );
                sent++;
            }
        }

        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_server_receive( server, 0, 0, &data, &bytes ) )
        {
            check( bytes == 4 );
            uint32_t seq = 0;
            memcpy( &seq, data, sizeof( seq ) );
            check( (int) seq == received );
            received++;
            rocketnet_server_release( server, 0, data );
        }

        rocketnet_server_send_packets( server );
        rocketnet_client_send_packets( client );
    }

    check( sent == total_messages );
    check( received == total_messages );

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_loopback_set_patterns( 0, 0 );
    rocketnet_public_link_simulator_test_development( 0 );

    printf( "loopback_reliable_ordered_survives_loss_and_reorder\n" );
}

static void loopback_run_fixed_trace( int drop_every_nth, int reverse_window,
                                      struct rocketnet_counters_t * out_client_counters,
                                      struct rocketnet_counters_t * out_server_counters,
                                      struct rocketnet_client_transport_stats_t * out_client_stats,
                                      struct rocketnet_server_transport_stats_t * out_server_stats )
{
    struct loopback_transport_pair_t pair;
    check( loopback_transport_pair_setup( &pair, drop_every_nth, reverse_window ) );
    check( loopback_transport_pair_connect( &pair ) );

    for ( int i = 0; i < 32; i++ )
    {
        uint8_t payload[8];
        memset( payload, (uint8_t) i, sizeof( payload ) );
        rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
        rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
        loopback_transport_pair_step( &pair, 0.01 );
    }

    *out_client_counters = *rocketnet_client_transport_counters( pair.client );
    *out_server_counters = *rocketnet_server_transport_counters( pair.server );
    rocketnet_client_transport_stats( pair.client, out_client_stats );
    rocketnet_server_transport_stats( pair.server, out_server_stats );

    loopback_transport_pair_teardown( &pair );
}

static void loopback_patterns_off_same_trace()
{
    struct rocketnet_counters_t cc_a, sc_a, cc_b, sc_b;
    struct rocketnet_client_transport_stats_t cs_a, cs_b;
    struct rocketnet_server_transport_stats_t ss_a, ss_b;

    loopback_run_fixed_trace( 0, 0, &cc_a, &sc_a, &cs_a, &ss_a );
    loopback_run_fixed_trace( 0, 0, &cc_b, &sc_b, &cs_b, &ss_b );

    check( memcmp( &cc_a, &cc_b, sizeof( cc_a ) ) == 0 );
    check( memcmp( &sc_a, &sc_b, sizeof( sc_a ) ) == 0 );
    check( cs_a.packets_sent == cs_b.packets_sent );
    check( cs_a.packets_received == cs_b.packets_received );
    check( ss_a.packets_sent == ss_b.packets_sent );
    check( ss_a.packets_received == ss_b.packets_received );

    printf( "loopback_patterns_off_same_trace\n" );
}

static void loopback_loss_reorder_production_refusal()
{
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.driver, sizeof( client_config.driver ), "%s", "loopback" );
    int client_fields = 0;
    const struct rocketnet_config_field_t * client_table = rocketnet_client_transport_config_fields( &client_fields );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    snprintf( server_config.driver, sizeof( server_config.driver ), "%s", "loopback" );
    int server_fields = 0;
    const struct rocketnet_config_field_t * server_table = rocketnet_server_transport_config_fields( &server_fields );

    rocketnet_loopback_set_patterns( 1, 0 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );
    rocketnet_loopback_set_patterns( -1, 0 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );
    rocketnet_loopback_set_patterns( 1000001, 0 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );
    rocketnet_loopback_set_patterns( 0, 1 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );
    rocketnet_loopback_set_patterns( 0, 65 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    rocketnet_loopback_set_patterns( 7, 0 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );

    rocketnet_loopback_set_patterns( 0, 4 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );

    snprintf( client_config.driver, sizeof( client_config.driver ), "%s", "udp" );
    rocketnet_loopback_set_patterns( 7, 0 );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    loopback_loss_reorder_signed_token( token );
    rocketnet_public_link_simulator_test_development( 1 );
    int status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_transport_create_ex( NULL, token, &client_config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    snprintf( client_config.driver, sizeof( client_config.driver ), "%s", "loopback" );
    rocketnet_public_link_simulator_test_development( 0 );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_transport_create_ex( NULL, token, &client_config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    rocketnet_loopback_set_patterns( 0, 0 );
    rocketnet_public_link_simulator_test_development( 0 );

    printf( "loopback_loss_reorder_production_refusal\n" );
}

REGISTER_UNIT_TEST( loopback_drop_every_nth );
REGISTER_UNIT_TEST( loopback_reverse_window );
REGISTER_UNIT_TEST( loopback_reliable_ordered_survives_loss_and_reorder );
REGISTER_UNIT_TEST( loopback_patterns_off_same_trace );
REGISTER_UNIT_TEST( loopback_loss_reorder_production_refusal );

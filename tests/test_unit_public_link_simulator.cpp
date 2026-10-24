/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public link simulator's production refusal: a nonzero simulated loss,
// latency, jitter or duplicate value is refused by the same config walk a
// production create uses, and admitted when the build is development. This
// exercises the walk over the real transport config tables, so the four
// fields' presence, their ranges and their refusal are pinned together.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_client_io.h"

static double live_link_clock = 1000.0;

static double live_link_time_callback( void * )
{
    return live_link_clock;
}

struct live_link_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    double time;
};

static bool live_link_pair_setup( struct live_link_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    live_link_clock = pair->time;
    rocketnet_public_link_simulator_test_development( 1 );

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
    client_config.step_driven = 1;
    client_config.time_callback = live_link_time_callback;
    pair->client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), pair->time );
    check( pair->client != NULL );
    return pair->client != NULL;
}

static void live_link_pair_step( struct live_link_pair_t * pair, double dt )
{
    pair->time += dt;
    live_link_clock = pair->time;
    rocketnet_server_transport_update( pair->server, pair->time );
    rocketnet_client_transport_update( pair->client, pair->time );
    rocketnet_server_transport_send_packets( pair->server );
    rocketnet_client_transport_send_packets( pair->client );
}

static bool live_link_pair_connect( struct live_link_pair_t * pair )
{
    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_is_client_connected( pair->server, 0 ) )
            return true;
        live_link_pair_step( pair, 0.01 );
    }
    return false;
}

static void live_link_pair_teardown( struct live_link_pair_t * pair )
{
    rn_test_client_transport_stop_and_destroy( pair->client );
    rn_test_server_transport_stop_and_destroy( pair->server );
    rocketnet_public_link_simulator_test_development( 0 );
}

static int live_link_server_payload_count( struct live_link_pair_t * pair )
{
    const int * indices = NULL;
    uint8_t * const * data = NULL;
    const int * bytes = NULL;
    return rocketnet_server_transport_payloads( pair->server, &indices, &data, &bytes );
}

static int live_link_client_receive( struct live_link_pair_t * pair )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    return rocketnet_client_transport_receive_packet( pair->client, packet, (int) sizeof( packet ) );
}

// The server's payload list and the client's delivery cursor both hold one
// update's worth: a payload delivered in an earlier update is gone by the
// next one. So the count is taken after every step and summed, and the
// client's cursor is drained at every step, which is also what the API asks
// of an application (take every message before the next update).

static int live_link_server_payloads_seen( struct live_link_pair_t * pair, int steps )
{
    int seen = 0;
    for ( int i = 0; i < steps; i++ )
    {
        live_link_pair_step( pair, 0.01 );
        seen += live_link_server_payload_count( pair );
    }
    return seen;
}

static int live_link_client_bytes_seen( struct live_link_pair_t * pair, int steps )
{
    int seen = 0;
    for ( int i = 0; i < steps; i++ )
    {
        live_link_pair_step( pair, 0.01 );
        int bytes = live_link_client_receive( pair );
        while ( bytes > 0 )
        {
            seen += bytes;
            bytes = live_link_client_receive( pair );
        }
    }
    return seen;
}

static void test_public_link_simulator_applies_to_live_connection()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct rocketnet_client_transport_stats_t before;
    struct rocketnet_client_transport_stats_t after;
    rocketnet_client_transport_stats( pair.client, &before );

    // the setting applies to the live connection at once: the packet the
    // client sends after the call leaves the client and never reaches the
    // server
    check( rocketnet_client_transport_network_simulator( pair.client, 0, 0, 100, 0 ) );
    uint8_t payload[4] = { 1, 2, 3, 4 };
    rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
    check( live_link_server_payloads_seen( &pair, 20 ) == 0 );
    rocketnet_client_transport_stats( pair.client, &after );
    check( after.packets_sent > before.packets_sent );

    // the connection is still up: the loss applies to the packets, not to the
    // connection
    check( rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_server_transport_is_client_connected( pair.server, 0 ) );

    // and the setting comes back off: the next packet is delivered
    check( rocketnet_client_transport_network_simulator( pair.client, 0, 0, 0, 0 ) );
    rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
    check( live_link_server_payloads_seen( &pair, 20 ) > 0 );

    live_link_pair_teardown( &pair );
}

static void test_public_link_simulator_server_applies_to_live_start()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct rocketnet_server_transport_stats_t before;
    struct rocketnet_server_transport_stats_t after;
    rocketnet_server_transport_stats( pair.server, &before );

    // the setting applies to the running server at once: the packet the
    // server sends after the call leaves the server and never reaches the
    // client
    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 100, 0 ) );
    uint8_t payload[4] = { 5, 6, 7, 8 };
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( live_link_client_bytes_seen( &pair, 20 ) == 0 );
    rocketnet_server_transport_stats( pair.server, &after );
    check( after.packets_sent > before.packets_sent );

    // and the setting comes back off: the next packet is delivered
    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 0, 0 ) );
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( live_link_client_bytes_seen( &pair, 20 ) > 0 );

    live_link_pair_teardown( &pair );
}

// A server switched from latency to all-zero while the simulator holds
// packets still delivers every one of them: the setting changes what the
// simulator does with the packets sent after it, never the ones it holds.
static void test_public_link_simulator_server_switch_off_delivers_held()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    check( rocketnet_server_transport_network_simulator( pair.server, 200, 0, 0, 0 ) );
    const int num_payloads = 8;
    uint8_t payload[4] = { 9, 10, 11, 12 };
    for ( int i = 0; i < num_payloads; i++ )
        rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( live_link_client_bytes_seen( &pair, 1 ) == 0 );

    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 0, 0 ) );
    check( live_link_client_bytes_seen( &pair, 40 ) == num_payloads * (int) sizeof( payload ) );
    check( rocketnet_server_transport_is_client_connected( pair.server, 0 ) );

    live_link_pair_teardown( &pair );
}

static void public_link_simulator_check_fields( const struct rocketnet_config_field_t * fields, int num_fields )
{
    int found_loss = 0;
    int found_latency = 0;
    int found_jitter = 0;
    int found_duplicate = 0;
    for ( int i = 0; i < num_fields; i++ )
    {
        if ( strcmp( fields[i].name, "simulated_loss_percent" ) == 0 )
            found_loss = 1;
        if ( strcmp( fields[i].name, "simulated_latency_ms" ) == 0 )
            found_latency = 1;
        if ( strcmp( fields[i].name, "simulated_jitter_ms" ) == 0 )
            found_jitter = 1;
        if ( strcmp( fields[i].name, "simulated_duplicate_percent" ) == 0 )
            found_duplicate = 1;
    }
    check( found_loss );
    check( found_latency );
    check( found_jitter );
    check( found_duplicate );
}

static void test_public_link_simulator_production_refusal()
{
    // the client transport, from its real table
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    int client_fields = 0;
    const struct rocketnet_config_field_t * client_table = rocketnet_client_transport_config_fields( &client_fields );
    public_link_simulator_check_fields( client_table, client_fields );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_latency_ms = 25;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_latency_ms = 0;
    client_config.simulated_duplicate_percent = 50;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_duplicate_percent = 0;
    client_config.simulated_loss_percent = 100;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    // the server transport, from its real table
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    int server_fields = 0;
    const struct rocketnet_config_field_t * server_table = rocketnet_server_transport_config_fields( &server_fields );
    public_link_simulator_check_fields( server_table, server_fields );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    server_config.simulated_jitter_ms = 10;
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );

    // ranges: percentages cap at 100, milliseconds at 60000
    client_config.simulated_loss_percent = 101;
    check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
    client_config.simulated_loss_percent = 0;
    client_config.simulated_latency_ms = 60001;
    check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
    client_config.simulated_latency_ms = 0;

    printf( "test_public_link_simulator_production_refusal\n" );
}

REGISTER_UNIT_TEST( test_public_link_simulator_production_refusal );

#include "rocketnet_network_simulator.h"
#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_connect_token.h"
#include "fake_client_driver.h"

struct public_link_wire_t
{
    struct rocketnet_network_simulator_t * simulator;
    struct rocketnet_address_t from;
    struct rocketnet_address_t to;
    double time;
    uint8_t received[ROCKETNET_RECEIVE_CEILING];
};

static void public_link_wire_begin( struct public_link_wire_t * wire,
                                    const struct rocketnet_client_transport_config_t * config,
                                    uint64_t seed )
{
    memset( wire, 0, sizeof(*wire) );
    wire->time = 1000.0;
    wire->simulator = rocketnet_network_simulator_create( NULL, 256, seed );
    check( wire->simulator != NULL );
    check( rocketnet_address_parse( &wire->from, "198.51.100.1:10000" ) );
    check( rocketnet_address_parse( &wire->to, "198.51.100.2:10000" ) );
    rocketnet_network_simulator_config( wire->simulator,
                                        config->simulated_latency_ms / 1000.0,
                                        config->simulated_jitter_ms / 1000.0,
                                        (float) config->simulated_loss_percent,
                                        (float) config->simulated_duplicate_percent );
}

static void public_link_wire_end( struct public_link_wire_t * wire )
{
    rocketnet_network_simulator_destroy( wire->simulator );
}

static void public_link_wire_send( struct public_link_wire_t * wire, uint8_t value )
{
    rocketnet_network_simulator_send( wire->simulator, &wire->from, &wire->to, &value, 1, wire->time );
}

static int public_link_wire_receive( struct public_link_wire_t * wire )
{
    struct rocketnet_address_t from;
    return rocketnet_network_simulator_receive( wire->simulator, &wire->to, wire->time, &from, wire->received );
}

static void public_link_simulator_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = UINT64_C(1) << 40;
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

static rocketnet_client_transport_t * public_link_simulator_client(
    rocketnet_fake_client_transport_io_t * transport, const rocketnet_client_transport_config_t * config )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_link_simulator_token( token );
    return rocketnet_client_transport_create_step_driven_current( transport, token, config, 1000.0,
                                                           &rocketnet_fake_client_operations_base, transport );
}

static void test_public_link_simulator_drops_at_loss()
{
    rocketnet_public_link_simulator_test_development( 1 );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_loss_percent = 100;
    rocketnet_fake_client_transport_io_t * dropped = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = public_link_simulator_client( dropped, &config );
    check( transport != NULL );
    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1001.0 );
    check( dropped->num_sent == 0 );
    rn_test_client_transport_stop_and_destroy( transport );
    delete dropped;

    config.simulated_loss_percent = 0;
    rocketnet_fake_client_transport_io_t * direct = new rocketnet_fake_client_transport_io_t();
    transport = public_link_simulator_client( direct, &config );
    check( transport != NULL );
    rocketnet_client_transport_update( transport, 1000.0 );
    check( direct->num_sent == 1 );
    rn_test_client_transport_stop_and_destroy( transport );
    delete direct;
}

static void test_public_link_simulator_latency()
{
    rocketnet_public_link_simulator_test_development( 1 );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_latency_ms = 25;
    rocketnet_fake_client_transport_io_t * transport_io = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = public_link_simulator_client( transport_io, &config );
    check( transport != NULL );
    rocketnet_client_transport_update( transport, 1000.0 );
    check( transport_io->num_sent == 0 );
    rocketnet_client_transport_update( transport, 1000.024 );
    check( transport_io->num_sent == 0 );
    rocketnet_client_transport_update( transport, 1000.025 );
    check( transport_io->num_sent == 1 );
    rn_test_client_transport_stop_and_destroy( transport );
    delete transport_io;
}

static void test_public_link_simulator_jitter()
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_latency_ms = 10;
    config.simulated_jitter_ms = 20;

    struct public_link_wire_t wire;
    public_link_wire_begin( &wire, &config, 13 );
    for ( int i = 0; i < 64; i++ )
        public_link_wire_send( &wire, (uint8_t) i );

    int delivered = 0;
    while ( wire.time < 1000.009 )
    {
        wire.time += 0.001;
        while ( public_link_wire_receive( &wire ) > 0 )
            delivered++;
    }
    check( delivered == 0 );
    while ( wire.time < 1000.031 )
    {
        wire.time += 0.001;
        while ( public_link_wire_receive( &wire ) > 0 )
            delivered++;
    }
    check( delivered == 64 );
    public_link_wire_end( &wire );
}

static void test_public_link_simulator_duplicate()
{
    rocketnet_public_link_simulator_test_development( 1 );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_duplicate_percent = 100;
    rocketnet_fake_client_transport_io_t * transport_io = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = public_link_simulator_client( transport_io, &config );
    check( transport != NULL );
    rocketnet_client_transport_update( transport, 1000.0 );
    check( transport_io->num_sent == 0 );
    rocketnet_client_transport_update( transport, 1000.001 );
    check( transport_io->num_sent == 2 );
    rn_test_client_transport_stop_and_destroy( transport );
    delete transport_io;
}

static void test_public_link_simulator_same_seed_same_schedule()
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_loss_percent = 15;
    config.simulated_latency_ms = 5;
    config.simulated_jitter_ms = 30;
    config.simulated_duplicate_percent = 25;

    struct public_link_wire_t a;
    struct public_link_wire_t b;
    public_link_wire_begin( &a, &config, 23 );
    public_link_wire_begin( &b, &config, 23 );
    for ( int i = 0; i < 64; i++ )
    {
        public_link_wire_send( &a, (uint8_t) i );
        public_link_wire_send( &b, (uint8_t) i );
    }

    for ( int step = 0; step < 40; step++ )
    {
        a.time += 0.001;
        b.time += 0.001;
        for (;;)
        {
            const int a_bytes = public_link_wire_receive( &a );
            const int b_bytes = public_link_wire_receive( &b );
            check( a_bytes == b_bytes );
            if ( a_bytes == 0 )
                break;
            check( memcmp( a.received, b.received, (size_t) a_bytes ) == 0 );
        }
    }
    public_link_wire_end( &a );
    public_link_wire_end( &b );
}

REGISTER_UNIT_TEST( test_public_link_simulator_drops_at_loss );
REGISTER_UNIT_TEST( test_public_link_simulator_latency );
REGISTER_UNIT_TEST( test_public_link_simulator_jitter );
REGISTER_UNIT_TEST( test_public_link_simulator_duplicate );
REGISTER_UNIT_TEST( test_public_link_simulator_same_seed_same_schedule );
REGISTER_UNIT_TEST( test_public_link_simulator_applies_to_live_connection );
REGISTER_UNIT_TEST( test_public_link_simulator_server_applies_to_live_start );
REGISTER_UNIT_TEST( test_public_link_simulator_server_switch_off_delivers_held );

static void test_public_link_simulator_revision_1_prefixes()
{
    struct alignas(16) client_revision_1_t
    {
        uint8_t bytes[ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR];
        uint8_t guard[16];
    } client;
    memset( &client, 0xa5, sizeof(client) );
    check( rocketnet_client_transport_default_config_sized(
        (rocketnet_client_transport_config_t *) client.bytes, sizeof(client.bytes) ) == 1 );
    for ( size_t i = 0; i < sizeof(client.guard); i++ )
        check( client.guard[i] == 0xa5 );
    rocketnet_client_transport_config_t client_current;
    rocketnet_client_transport_default_config_current( &client_current );
    snprintf( client_current.transport, sizeof(client_current.transport), "%s", "loopback" );
    memcpy( client.bytes, &client_current, sizeof(client.bytes) );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_link_simulator_token( token );
    rocketnet_client_transport_t * client_transport = rocketnet_client_transport_create_sized(
        NULL, token, (const rocketnet_client_transport_config_t *) client.bytes,
        sizeof(client.bytes), 1000.0 );
    check( client_transport != NULL );
    rn_test_client_transport_stop_and_destroy( client_transport );
    for ( size_t i = 0; i < sizeof(client.guard); i++ )
        check( client.guard[i] == 0xa5 );

    struct alignas(16) server_revision_1_t
    {
        uint8_t bytes[ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1];
        uint8_t guard[16];
    } server;
    memset( &server, 0x5a, sizeof(server) );
    check( rocketnet_server_transport_default_config_sized(
        (rocketnet_server_transport_config_t *) server.bytes, sizeof(server.bytes) ) == 1 );
    for ( size_t i = 0; i < sizeof(server.guard); i++ )
        check( server.guard[i] == 0x5a );
    rocketnet_server_transport_config_t server_current;
    rocketnet_server_transport_default_config_current( &server_current );
    server_current.max_clients = 1;
    server_current.protocol_id = 1;
    memset( server_current.backend_public_key, 0xAB, 32 );
    memset( server_current.server_public_key, 0xCD, 32 );
    memset( server_current.server_secret_key, 0xEF, 32 );
    snprintf( server_current.transport, sizeof(server_current.transport), "%s", "loopback" );
    memcpy( server.bytes, &server_current, sizeof(server.bytes) );
    rocketnet_server_transport_t * server_transport = rocketnet_server_transport_create_sized(
        NULL, "127.0.0.1:0", (const rocketnet_server_transport_config_t *) server.bytes,
        sizeof(server.bytes) );
    check( server_transport != NULL );
    rn_test_server_transport_stop_and_destroy( server_transport );
    for ( size_t i = 0; i < sizeof(server.guard); i++ )
        check( server.guard[i] == 0x5a );
}

REGISTER_UNIT_TEST( test_public_link_simulator_revision_1_prefixes );

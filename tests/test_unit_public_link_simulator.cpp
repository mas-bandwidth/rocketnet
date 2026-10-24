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

// The revision 1 config buffers keep the public config's 16-byte alignment.
// Their intentional struct padding triggers MSVC C4324.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4324)
#endif
static void test_public_link_simulator_revision_1_prefixes()
{
    struct alignas(alignof(rocketnet_client_transport_config_t)) client_revision_1_t
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

    struct alignas(alignof(rocketnet_server_transport_config_t)) server_revision_1_t
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
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

REGISTER_UNIT_TEST( test_public_link_simulator_revision_1_prefixes );

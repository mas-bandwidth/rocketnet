/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The transport config's public link simulator: simulated loss, latency,
// jitter and duplication on this endpoint's outgoing datagrams, through the
// existing sdk/rocketnet_network_simulator.h, in a development library only.
// Every case runs on the loopback driver, with no socket and no wall clock;
// time is an argument and a test clock starts at 1000.0.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_driver.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_client_io.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define check rocketnet_unit_check

extern const rocketnet_driver_backend_t rocketnet_loopback_driver;

struct link_fixture_t
{
    rocketnet_server_driver_t server;
    rocketnet_client_transport_t * client;
};

static void link_fixture_token( uint8_t * token, uint16_t port )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = (uint8_t) ( port & 0xff );
    signed_fields.ipv4_address[5] = (uint8_t) ( port >> 8 );
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static void link_fixture_open( link_fixture_t * fixture, const rocketnet_client_transport_config_t * config )
{
    memset( fixture, 0, sizeof(*fixture) );
    rocketnet_public_link_simulator_test_development( 1 );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    rocketnet_address_t server_address;
    check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    check( rocketnet_server_driver_open( rocketnet_loopback_driver.server, NULL, &server_setup, &server_address, &fixture->server ) );
    check( server_address.port != 0 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    link_fixture_token( token, server_address.port );

    rocketnet_client_transport_config_t effective = *config;
    strcpy( effective.driver, "loopback" );
    fixture->client = rocketnet_client_transport_create_step_driven( NULL, token, &effective, 1000.0,
                                                                     rocketnet_loopback_driver.client, NULL );
    check( fixture->client != NULL );
}

static void link_fixture_close( link_fixture_t * fixture )
{
    if ( fixture->client )
        rocketnet_client_transport_destroy( fixture->client );
    rocketnet_server_driver_close( &fixture->server );
    rocketnet_public_link_simulator_test_development( 0 );
    memset( fixture, 0, sizeof(*fixture) );
}

static int link_fixture_drain( link_fixture_t * fixture )
{
    int total = 0;
    for (;;)
    {
        rocketnet_address_t from;
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int bytes = 0;
        const int received = fixture->server.operations->receive_batch( fixture->server.instance, &from, packet, sizeof(packet), &bytes, 1 );
        if ( received <= 0 )
            break;
        total += received;
        if ( total > 100000 )
            break;
    }
    return total;
}

static void link_config_zero( rocketnet_client_transport_config_t * config )
{
    rocketnet_client_transport_default_config( config );
    config->simulated_loss_percent = 0;
    config->simulated_latency_ms = 0;
    config->simulated_jitter_ms = 0;
    config->simulated_duplicate_percent = 0;
}

// 100 percent loss delivers nothing; 0 delivers the update's datagram.
static void public_link_simulator_drops_at_loss()
{
    link_fixture_t fixture;
    rocketnet_client_transport_config_t config;
    link_config_zero( &config );
    config.simulated_loss_percent = 100;

    link_fixture_open( &fixture, &config );
    rocketnet_client_transport_update( fixture.client, 1000.0 );
    check( link_fixture_drain( &fixture ) == 0 );
    link_fixture_close( &fixture );

    link_config_zero( &config );
    link_fixture_open( &fixture, &config );
    rocketnet_client_transport_update( fixture.client, 1000.0 );
    check( link_fixture_drain( &fixture ) >= 1 );
    link_fixture_close( &fixture );

    printf( "public_link_simulator_drops_at_loss\n" );
}

// A datagram arrives no earlier than the latency.
static void public_link_simulator_latency()
{
    link_fixture_t fixture;
    rocketnet_client_transport_config_t config;
    link_config_zero( &config );
    config.simulated_latency_ms = 100;

    link_fixture_open( &fixture, &config );
    rocketnet_client_transport_update( fixture.client, 1000.0 );
    check( link_fixture_drain( &fixture ) == 0 );

    rocketnet_client_transport_update( fixture.client, 1000.099 );
    check( link_fixture_drain( &fixture ) == 0 );

    rocketnet_client_transport_update( fixture.client, 1000.100 );
    check( link_fixture_drain( &fixture ) >= 1 );

    link_fixture_close( &fixture );
    printf( "public_link_simulator_latency\n" );
}

// Arrival spread stays inside the jitter: two copies of one datagram, each
// due at latency + a draw in [0, jitter).
static void public_link_simulator_jitter()
{
    link_fixture_t fixture;
    rocketnet_client_transport_config_t config;
    link_config_zero( &config );
    config.simulated_jitter_ms = 5;
    config.simulated_duplicate_percent = 100;

    link_fixture_open( &fixture, &config );

    double first[2] = { -1.0, -1.0 };
    int found = 0;
    for ( int step = 0; step <= 5 && found < 2; step++ )
    {
        const double now = 1000.0 + (double) step / 1000.0;
        rocketnet_client_transport_update( fixture.client, now );
        const int arrived = link_fixture_drain( &fixture );
        for ( int i = 0; i < arrived && found < 2; i++ )
            first[found++] = now;
    }

    check( found == 2 );
    check( first[0] >= 1000.0 );
    check( first[0] <= 1000.005 );
    check( first[1] >= 1000.0 );
    check( first[1] <= 1000.005 );
    check( fabs( first[1] - first[0] ) <= 0.005 + 1e-9 );

    link_fixture_close( &fixture );
    printf( "public_link_simulator_jitter\n" );
}

// 100 percent duplication delivers two copies of the update's datagram.
static void public_link_simulator_duplicate()
{
    link_fixture_t fixture;
    rocketnet_client_transport_config_t config;
    link_config_zero( &config );
    config.simulated_duplicate_percent = 100;

    link_fixture_open( &fixture, &config );
    rocketnet_client_transport_update( fixture.client, 1000.0 );
    check( link_fixture_drain( &fixture ) == 2 );
    link_fixture_close( &fixture );

    printf( "public_link_simulator_duplicate\n" );
}

static void link_schedule( int * delivered, int steps )
{
    link_fixture_t fixture;
    rocketnet_client_transport_config_t config;
    link_config_zero( &config );
    config.simulated_latency_ms = 20;
    config.simulated_jitter_ms = 20;
    config.simulated_duplicate_percent = 50;

    link_fixture_open( &fixture, &config );
    for ( int i = 0; i < steps; i++ )
    {
        rocketnet_client_transport_update( fixture.client, 1000.0 + (double) i / 1000.0 );
        delivered[i] = link_fixture_drain( &fixture );
    }
    link_fixture_close( &fixture );
}

// The same config and seed give the same delivery schedule.
static void public_link_simulator_same_seed_same_schedule()
{
    enum { STEPS = 64 };
    int first[STEPS];
    int second[STEPS];
    link_schedule( first, STEPS );
    link_schedule( second, STEPS );
    check( memcmp( first, second, sizeof(first) ) == 0 );

    printf( "public_link_simulator_same_seed_same_schedule\n" );
}

// The unit suite links only the development library, so the production
// refusal is pinned through the config validation walk the production create
// uses: a nonzero simulator field is refused when development is false, and
// admitted when it is true.
static void public_link_simulator_production_refusal()
{
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.simulated_latency_ms = 25;

    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &num_fields );
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, fields, num_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, fields, num_fields, true ) );

    client_config.simulated_latency_ms = 0;
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, fields, num_fields, false ) );

    // The real create path refuses a nonzero value with CONFIG_OUT_OF_RANGE
    // before any driver opens, so no socket is involved.
    rocketnet_public_link_simulator_test_development( 0 );
    rocketnet_client_transport_config_t refused;
    rocketnet_client_transport_default_config( &refused );
    strcpy( refused.driver, "loopback" );
    refused.simulated_loss_percent = 10;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    link_fixture_token( token, 40000 );
    int status = 0;
    check( rocketnet_client_transport_create_ex( NULL, token, &refused, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.simulated_loss_percent = 10;
    fields = rocketnet_server_transport_config_fields( &num_fields );
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, fields, num_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, fields, num_fields, true ) );

    printf( "public_link_simulator_production_refusal\n" );
}

REGISTER_UNIT_TEST( public_link_simulator_drops_at_loss );
REGISTER_UNIT_TEST( public_link_simulator_latency );
REGISTER_UNIT_TEST( public_link_simulator_jitter );
REGISTER_UNIT_TEST( public_link_simulator_duplicate );
REGISTER_UNIT_TEST( public_link_simulator_same_seed_same_schedule );
REGISTER_UNIT_TEST( public_link_simulator_production_refusal );

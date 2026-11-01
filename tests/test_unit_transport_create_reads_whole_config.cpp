/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Unit tests: the transport creates (rocketnet_server_transport_create and
// _create_ex, rocketnet_client_transport_create and _create_ex) read the caller's whole config, the
// struct of this header, so a field appended after the first config revision
// (second_public_address, loopback_hub, the simulated_* fields) is honored.
// Every endpoint is on the loopback transport in the test's hub: no socket,
// no wall clock, and the client is caller-stepped.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "loopback_hub.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define WHOLE_CONFIG_PROTOCOL_ID 0x57484F4C45ULL
#define WHOLE_CONFIG_TOKEN_EPOCH 1000000ULL
#define WHOLE_CONFIG_TIME        1000.0

struct whole_config_keys_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
};

static void whole_config_keys( whole_config_keys_t * keys )
{
    check( rocketnet_generate_backend_keypair( keys->backend_public, keys->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( keys->server_public, keys->server_secret ) == ROCKETNET_STATUS_OK );
}

// a server config a production build accepts: generated keys, a packet
// filter of its own, the loopback transport in the test's hub
static void whole_config_server( struct rocketnet_server_transport_config_t * config, const whole_config_keys_t * keys )
{
    rocketnet_server_transport_default_config( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    config->protocol_id = WHOLE_CONFIG_PROTOCOL_ID;
    config->max_clients = 4;
    memcpy( config->backend_public_key, keys->backend_public, sizeof( config->backend_public_key ) );
    memcpy( config->server_public_key, keys->server_public, sizeof( config->server_public_key ) );
    memcpy( config->server_secret_key, keys->server_secret, sizeof( config->server_secret_key ) );
    snprintf( config->transport, sizeof( config->transport ), "%s", "loopback" );
    config->loopback_hub = rocketnet_test_loopback_hub();
}

// loopback_hub is past the first revision: create_ex reads it, so the
// loopback server is created in the hub rather than refused for a missing one
static void test_server_transport_create_ex_honors_loopback_hub()
{
    whole_config_keys_t keys;
    whole_config_keys( &keys );

    struct rocketnet_server_transport_config_t config;
    whole_config_server( &config, &keys );

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:40501", &config, WHOLE_CONFIG_TIME, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( strcmp( rocketnet_server_transport_io_name( server ), "loopback" ) == 0 );

    rn_test_server_transport_stop_and_destroy( server );
}

// second_public_address is past the first revision: create_ex and create
// read it, so the server advertises the IPv6 address beside the IPv4 one
static void whole_config_check_both_families( bool plain, int port )
{
    whole_config_keys_t keys;
    whole_config_keys( &keys );

    char ipv4[64];
    char ipv6[64];
    snprintf( ipv4, sizeof( ipv4 ), "127.0.0.1:%d", port );
    snprintf( ipv6, sizeof( ipv6 ), "[::1]:%d", port );

    struct rocketnet_server_transport_config_t config;
    whole_config_server( &config, &keys );
    snprintf( config.second_public_address, sizeof( config.second_public_address ), "%s", ipv6 );

    int status = -1;
    struct rocketnet_server_transport_t * server = plain ? rocketnet_server_transport_create( NULL, ipv4, &config )
                                                         : rocketnet_server_transport_create_ex( NULL, ipv4, &config, WHOLE_CONFIG_TIME, &status );
    check( server != NULL );
    check( plain || status == ROCKETNET_STATUS_OK );

    rocketnet_address_t expected_ipv4;
    rocketnet_address_t expected_ipv6;
    check( rocketnet_address_parse( &expected_ipv4, ipv4 ) );
    check( rocketnet_address_parse( &expected_ipv6, ipv6 ) );

    rocketnet_address_t advertised;
    check( rocketnet_server_transport_advertised_address( server, ROCKETNET_ADDRESS_IPV4, &advertised ) );
    check( rocketnet_address_equal( &advertised, &expected_ipv4 ) );
    check( rocketnet_server_transport_advertised_address( server, ROCKETNET_ADDRESS_IPV6, &advertised ) );
    check( rocketnet_address_equal( &advertised, &expected_ipv6 ) );

    rn_test_server_transport_stop_and_destroy( server );
}

static void test_server_transport_create_ex_honors_second_public_address()
{
    whole_config_check_both_families( false, 40502 );
}

static void test_server_transport_create_honors_second_public_address()
{
    whole_config_check_both_families( true, 40504 );
}

// second_public_address alone: a second address the resolve refuses (the
// public address's family, then a port other than its own) is refused with
// INVALID_ADDRESS before any transport opens, so the loopback_hub field plays
// no part. A create that read only the first revision would default the
// field to empty, resolve, and fail later at the open for some other reason.
static void test_server_transport_create_ex_refuses_a_bad_second_public_address()
{
    rocketnet_quiet( 1 );

    whole_config_keys_t keys;
    whole_config_keys( &keys );

    const char * refused[] = { "127.0.0.2:40505", "[::1]:40506" };
    for ( size_t i = 0; i < sizeof( refused ) / sizeof( refused[0] ); ++i )
    {
        struct rocketnet_server_transport_config_t config;
        whole_config_server( &config, &keys );
        snprintf( config.second_public_address, sizeof( config.second_public_address ), "%s", refused[i] );

        int status = -1;
        check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:40505", &config, WHOLE_CONFIG_TIME, &status ) == NULL );
        check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
    }

    rocketnet_quiet( 0 );
}

// simulated_reverse_window is past the first revision: create_ex reads it,
// so a window of 1 (the range is 2 to 64, 0 off) is refused before the
// token is read
static void test_client_transport_create_ex_reads_simulated_fields()
{
    rocketnet_quiet( 1 );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.simulated_reverse_window = 1;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );

    int status = -1;
    check( rocketnet_client_transport_create_ex( NULL, token, &config, WHOLE_CONFIG_TIME, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    rocketnet_quiet( 0 );
}

#if ROCKETNET_DEV

// loopback_hub is past the first revision: create_ex and create read it,
// so the loopback client is created in the hub. step_driven runs the client
// on the caller's thread (a development field)
static void whole_config_check_client_hub( bool plain )
{
    whole_config_keys_t keys;
    whole_config_keys( &keys );

    const char * addresses[] = { "127.0.0.1:40503" };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( keys.backend_secret, keys.server_public, WHOLE_CONFIG_PROTOCOL_ID,
                                                     addresses, 1, UINT64_C( 0x57484F4C45 ), WHOLE_CONFIG_TOKEN_EPOCH, WHOLE_CONFIG_TOKEN_EPOCH + 3600, 60, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.token_epoch_unix_seconds = WHOLE_CONFIG_TOKEN_EPOCH;
    config.step_driven = 1;

    int status = -1;
    struct rocketnet_client_transport_t * client = plain ? rocketnet_client_transport_create( NULL, token, &config, WHOLE_CONFIG_TIME )
                                                         : rocketnet_client_transport_create_ex( NULL, token, &config, WHOLE_CONFIG_TIME, &status );
    check( client != NULL );
    check( plain || status == ROCKETNET_STATUS_OK );

    rn_test_client_transport_stop_and_destroy( client );
}

static void test_client_transport_create_ex_honors_loopback_hub()
{
    whole_config_check_client_hub( false );
}

static void test_client_transport_create_honors_loopback_hub()
{
    whole_config_check_client_hub( true );
}

REGISTER_UNIT_TEST( test_client_transport_create_ex_honors_loopback_hub );
REGISTER_UNIT_TEST( test_client_transport_create_honors_loopback_hub );
#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_server_transport_create_ex_honors_loopback_hub );
REGISTER_UNIT_TEST( test_server_transport_create_ex_honors_second_public_address );
REGISTER_UNIT_TEST( test_server_transport_create_honors_second_public_address );
REGISTER_UNIT_TEST( test_server_transport_create_ex_refuses_a_bad_second_public_address );
REGISTER_UNIT_TEST( test_client_transport_create_ex_reads_simulated_fields );

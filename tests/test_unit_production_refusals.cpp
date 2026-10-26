/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A production build (ROCKETNET_DEV=0) refuses the development values: the
// server and the client refuse the published test packet filter at create
// with ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION, and the client
// refuses a connect token sealed
// to the published development server key with
// ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION. The unit suite's
// production-posture hook library
// switches the refusal on for these cases only
// (rocketnet_development_values_refused_test_enable), so its other cases keep
// the default test filter; a production library refuses always. A
// development build accepts all of it, so these cases are production only.
//
// Every refusal happens before a worker or socket exists. Successful public
// create counterparts are destroyed before connect/start.

#include "test_unit_registry.h"

#if !ROCKETNET_DEV

#include "unit_shutdown.h"

#include "rocketnet.h"
#include "rocketnet_base64.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static const uint8_t production_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x5A, 0x5A, 0x5A, 0x5A };

static void production_token( uint8_t * token, const uint8_t * server_key )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = 1;
    signed_fields.create_timestamp = 1000;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, server_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

// Public connect with a complete token string. Every caller here expects the
// production-value check to refuse before the transport opens.
static int production_client_connect( const uint8_t * server_key )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    production_token( token, server_key );
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, sizeof( token_string ) ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    struct rocketnet_client_config_t config;
    check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.transport = "loopback";
    memcpy( config.packet_filter, production_filter, sizeof( config.packet_filter ) );
    enum rocketnet_status_t create_status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    check( client != NULL && create_status == ROCKETNET_STATUS_OK );
    const int status = rocketnet_client_connect( client, token_string );
    check( status != ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );
    return status;
}

static void test_production_server_refuses_test_packet_filter()
{
    rocketnet_quiet( true );
    rocketnet_development_values_refused_test_enable( 1 );

    // the public create
    {
        rocketnet_server_config_t config;
        check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
        config.protocol_id = 1;
        memset( config.backend_public_key, 0x42, sizeof( config.backend_public_key ) );
        config.transport = "loopback";
        config.max_clients = 2; // the refusal, not capacity: stay under the suite's RSS bound
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );

        memcpy( config.packet_filter, production_filter, sizeof( config.packet_filter ) );
        rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
        check( server != NULL );
        check( status == ROCKETNET_STATUS_OK );
        if ( server )
            rn_test_server_stop_and_destroy( server );
    }

    // the transport create
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_default_config( &config );
        memset( config.backend_public_key, 0x42, 32 );
        memset( config.server_public_key, 0x43, 32 );
        memset( config.server_secret_key, 0x44, 32 );
        config.protocol_id = 1;
        snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
        config.max_clients = 2;
        int status = -1;
        check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );

        memcpy( config.packet_filter, production_filter, sizeof( config.packet_filter ) );
        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &status );
        check( server != NULL );
        check( status == ROCKETNET_STATUS_OK );
        if ( server )
            rn_test_server_transport_stop_and_destroy( server );
    }

    rocketnet_development_values_refused_test_enable( 0 );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_production_server_refuses_test_packet_filter );

static void test_production_client_refuses_test_packet_filter()
{
    rocketnet_quiet( true );
    rocketnet_development_values_refused_test_enable( 1 );

    // the public create
    {
        rocketnet_client_config_t config;
        check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
        config.transport = "loopback";
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        check( rocketnet_client_create( NULL, &config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );

        memcpy( config.packet_filter, production_filter, sizeof( config.packet_filter ) );
        rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &status );
        check( client != NULL );
        check( status == ROCKETNET_STATUS_OK );
        if ( client )
            rn_test_client_stop_and_destroy( client );
    }

    rocketnet_development_values_refused_test_enable( 0 );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_production_client_refuses_test_packet_filter );

static void test_production_client_refuses_dev_connect_token()
{
    rocketnet_quiet( true );
    rocketnet_development_values_refused_test_enable( 1 );

    uint8_t dev_key[ROCKETNET_KEY_BYTES];
    memcpy( dev_key, rocketnet_dev_server_public_key, sizeof( dev_key ) );
    check( production_client_connect( dev_key ) == ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION );

    rocketnet_development_values_refused_test_enable( 0 );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_production_client_refuses_dev_connect_token );

#endif // #if !ROCKETNET_DEV

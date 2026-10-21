/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The development keys are public and must never serve production: an
// implementation must warn on every start that uses them, at each end that
// can tell, and a production build (ROCKETNET_DEV=0) must refuse them on the server. This case
// captures the log through the preformatted callback (as test_unit_logging.cpp
// does), with no socket, and asserts the warning level, the line, and that a
// non-development key logs no such line.
//
// Every case runs in memory, with no clock, sockets or network.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "rocketnet.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static const char DEV_KEY_WARN_PHRASE[] = "public and not for production";

static char g_buf[4096];
static int g_level = -1;
static size_t g_len = 0;
static int g_total_lines = 0;
static int g_dev_warn_lines = 0;
static int g_dev_warn_level = -1;
static char g_dev_warn_buf[4096];

static void capture_callback( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    g_total_lines++;
    g_level = level;
    g_len = length < sizeof( g_buf ) ? length : sizeof( g_buf ) - 1;
    memcpy( g_buf, message, g_len );
    g_buf[g_len] = '\0';
    if ( level == ROCKETNET_LOG_LEVEL_WARN && strstr( g_buf, DEV_KEY_WARN_PHRASE ) != NULL )
    {
        g_dev_warn_lines++;
        g_dev_warn_level = level;
        memcpy( g_dev_warn_buf, g_buf, g_len + 1 );
    }
}

static void start_capture()
{
    g_total_lines = 0;
    g_dev_warn_lines = 0;
    g_dev_warn_level = -1;
    g_dev_warn_buf[0] = '\0';
    g_level = -1;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( capture_callback, NULL );
}

static void stop_capture()
{
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
}

// A connect token whose signed server key is the given 32 bytes. The client
// reads the signed fields at connect, so the signed data is enough.
static void token_with_server_key( uint8_t * token, const uint8_t * server_key )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    if ( server_key )
        memcpy( signed_fields.server_public_key, server_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static void client_create_and_expect_warn( const uint8_t * server_key, int expect_dev_warn )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    token_with_server_key( token, server_key );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );

    start_capture();
    rocketnet_client_transport_t * transport =
        rocketnet_client_transport_create_step_driven( fake, token, &config, 1000.0, &rocketnet_fake_client_operations_base, fake );
    check( transport != NULL );
    check( g_dev_warn_lines == ( expect_dev_warn ? 1 : 0 ) );
    if ( expect_dev_warn )
    {
        check( g_dev_warn_level == ROCKETNET_LOG_LEVEL_WARN );
        check( strstr( g_dev_warn_buf, DEV_KEY_WARN_PHRASE ) != NULL );
    }
    stop_capture();

    rocketnet_client_transport_destroy( transport );
    check( fake->destroy_calls == 1 );
    delete fake;
}

static int test_set_environment( const char * name, const char * value )
{
#if defined( _WIN32 )
    return _putenv_s( name, value );
#else
    return setenv( name, value, 1 );
#endif
}

static int test_unset_environment( const char * name )
{
#if defined( _WIN32 )
    return _putenv_s( name, "" );
#else
    return unsetenv( name );
#endif
}

static void test_dev_keys_warn_on_every_start()
{
    // the server, development keys: a warning on create, and in a production
    // build (the unit suite's build) the published keys are refused.
    // The create pins the loopback transport on port 0: the development-keys
    // warn is raised before the transport ladder
    // (sdk/rocketnet_server_transport.cpp), so the WARN line, level and
    // DEV_KEY_WARN_PHRASE checks are unaffected by the transport choice, and
    // the unit suite stays free of any real socket bind.
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
        int status = -1;
        start_capture();
        void * srv = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &status );
        check( g_dev_warn_lines == 1 );
        check( g_dev_warn_level == ROCKETNET_LOG_LEVEL_WARN );
        check( strstr( g_dev_warn_buf, DEV_KEY_WARN_PHRASE ) != NULL );
        stop_capture();
#if !ROCKETNET_DEV
        check( srv == NULL );
        check( status == ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
#else
        if ( srv )
            rocketnet_server_transport_destroy( (struct rocketnet_server_transport_t *) srv );
#endif
    }

    // the server, production keys: no warning line. The ladder is forced to
    // exhaust on the operator memory budget, so no socket opens.
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_default_config( &config );
        memset( config.backend_public_key, 0x42, 32 );
        memset( config.server_public_key, 0x43, 32 );
        memset( config.server_secret_key, 0x44, 32 );
        config.protocol_id = 1;
        check( test_set_environment( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "1" ) == 0 );
        int status = -1;
        start_capture();
        void * srv = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &status );
        stop_capture();
        check( srv == NULL );
        check( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
        check( g_dev_warn_lines == 0 );
        check( test_unset_environment( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" ) == 0 );
    }

    // the client can tell from its token's server key: a development key warns
    {
        uint8_t dev_key[ROCKETNET_KEY_BYTES];
        memcpy( dev_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
        client_create_and_expect_warn( dev_key, 1 );
    }

    // the client, a non-development server key: no warning line
    {
        uint8_t prod_key[ROCKETNET_KEY_BYTES];
        memset( prod_key, 0x52, ROCKETNET_KEY_BYTES );
        client_create_and_expect_warn( prod_key, 0 );
    }
}

REGISTER_UNIT_TEST( test_dev_keys_warn_on_every_start );

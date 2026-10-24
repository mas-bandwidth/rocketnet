/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Each endpoint resolves its configuration once, at create: the defaults, the
// caller's struct, then the environment, every field, before validation and
// before memory is sized. Start and connect use that one resolved config and
// never read the environment again.
//
//   - a variable changed between create and start or connect has no effect on
//     the started endpoint: a 1-client server started after
//     ROCKETNET_SERVER_MAX_CLIENTS=2 still admits one client, and a client
//     connected after ROCKETNET_CLIENT_PACKET_FILTER changed still uses the
//     filter it was created with
//   - a variable set before create is validated at create: an invalid
//     ROCKETNET_CLIENT_TRANSPORT is refused by create, not by connect
//   - in a production build (ROCKETNET_DEV=0), a private packet filter set
//     only through ROCKETNET_SERVER_PACKET_FILTER or
//     ROCKETNET_CLIENT_PACKET_FILTER is accepted, and the published test
//     filter set only through the environment is refused at create
//
// Every case runs in memory on the loopback transport with step-driven
// clients: no socket, no thread, and time from 1000.0 as an argument.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "portable_crt.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_connect_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define RESOLVE_ONCE_PROTOCOL 0x2E50C0FFEE0A11CEULL

#if defined( _WIN32 )
static void resolve_once_setenv( const char * name, const char * value ) { check( rocketnet_test_env_putenv_s( name, value ) == 0 ); }
static void resolve_once_unsetenv( const char * name ) { check( rocketnet_test_env_putenv_s( name, "" ) == 0 ); }
#else
static void resolve_once_setenv( const char * name, const char * value ) { check( rocketnet_test_env_setenv( name, value, 1 ) == 0 ); }
static void resolve_once_unsetenv( const char * name ) { check( rocketnet_test_env_unsetenv( name ) == 0 ); }
#endif

static const uint8_t resolve_once_filter_a[ROCKETNET_PACKET_FILTER_BYTES] = { 0x5A, 0x5A, 0x5A, 0x5A };
static const uint8_t resolve_once_filter_b[ROCKETNET_PACKET_FILTER_BYTES] = { 0x6B, 0x6B, 0x6B, 0x6B };

static void resolve_once_filter_text( const uint8_t * filter, char * text, size_t text_bytes )
{
    check( rocketnet_base64_encode_data( filter, ROCKETNET_PACKET_FILTER_BYTES, text, text_bytes ) > 0 );
}

struct resolve_once_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
};

static void resolve_once_identity( resolve_once_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
}

static void resolve_once_server_config( rocketnet_server_config_t * config, const resolve_once_identity_t * identity, int max_clients )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = RESOLVE_ONCE_PROTOCOL;
    config->max_clients = max_clients;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
}

// a token for client_id sealed to the running server's own key
static void resolve_once_token( const rocketnet_server_t * server, const resolve_once_identity_t * identity, uint64_t client_id, char * token_string )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( identity->backend_secret, server_public,
        RESOLVE_ONCE_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

static double resolve_once_now = 1000.0;
static double resolve_once_clock( void * ) { return resolve_once_now; }

static void resolve_once_client_config( rocketnet_client_test_config_t * config )
{
    check( rocketnet_client_default_config( config ) == ROCKETNET_STATUS_OK );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = resolve_once_clock;
}

static void resolve_once_pump( rocketnet_server_t * server, rocketnet_client_t * const * clients, int num_clients, int steps )
{
    for ( int step = 0; step < steps; step++ )
    {
        for ( int i = 0; i < num_clients; i++ )
        {
            rocketnet_client_update( clients[i], resolve_once_now );
            rocketnet_client_send_packets( clients[i] );
        }
        rocketnet_server_update( server, resolve_once_now );
        rocketnet_server_send_packets( server );
        resolve_once_now += 0.01;
    }
}

// A 1-client server is created; ROCKETNET_SERVER_MAX_CLIENTS=2 is set before
// start. The started server is the one create sized: one slot, so the second
// client is denied and never connects.
static void test_config_server_env_changed_after_create_has_no_effect()
{
    rocketnet_quiet( 1 );
    resolve_once_identity_t identity;
    resolve_once_identity( &identity );
    resolve_once_unsetenv( "ROCKETNET_SERVER_MAX_CLIENTS" );

    rocketnet_server_config_t server_config;
    resolve_once_server_config( &server_config, &identity, 1 );
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_max_clients( server ) == 1 );

    resolve_once_setenv( "ROCKETNET_SERVER_MAX_CLIENTS", "2" );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_max_clients( server ) == 1 );

    rocketnet_client_t * clients[2] = { NULL, NULL };
    for ( int i = 0; i < 2; i++ )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        resolve_once_token( server, &identity, 0x1000 + (uint64_t) i, token );
        rocketnet_client_test_config_t client_config;
        resolve_once_client_config( &client_config );
        clients[i] = rocketnet_client_create( NULL, &client_config, NULL );
        check( clients[i] != NULL );
        check( rocketnet_client_connect( clients[i], token ) == ROCKETNET_STATUS_OK );
        int connected_at = -1;
        for ( int step = 0; step < 400 && connected_at < 0; step++ )
        {
            resolve_once_pump( server, clients, i + 1, 1 );
            if ( rocketnet_client_state( clients[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED || rocketnet_client_state( clients[i] ) < 0 )
                connected_at = step;
        }
    }
    resolve_once_pump( server, clients, 2, 50 );

    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_state( clients[1] ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    struct rocketnet_server_stats_t stats;
    rocketnet_server_stats( server, &stats );
    check( stats.connections_accepted == 1 );
    check( stats.connections_denied_full >= 1 );

    resolve_once_unsetenv( "ROCKETNET_SERVER_MAX_CLIENTS" );
    for ( int i = 0; i < 2; i++ )
        rn_test_client_stop_and_destroy( clients[i] );
    rn_test_server_stop_and_destroy( server );
    rocketnet_quiet( 0 );
}
REGISTER_UNIT_TEST( test_config_server_env_changed_after_create_has_no_effect );

// Both endpoints take filter A from the environment at create; the client's
// variable then changes to filter B before connect. The client connects with
// the filter it was created with, A, and reaches the server.
static void test_config_client_env_changed_after_create_has_no_effect()
{
    rocketnet_quiet( 1 );
    resolve_once_identity_t identity;
    resolve_once_identity( &identity );
    char filter_a[16];
    char filter_b[16];
    resolve_once_filter_text( resolve_once_filter_a, filter_a, sizeof( filter_a ) );
    resolve_once_filter_text( resolve_once_filter_b, filter_b, sizeof( filter_b ) );
    resolve_once_setenv( "ROCKETNET_SERVER_PACKET_FILTER", filter_a );
    resolve_once_setenv( "ROCKETNET_CLIENT_PACKET_FILTER", filter_a );

    rocketnet_server_config_t server_config;
    resolve_once_server_config( &server_config, &identity, 1 );
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    resolve_once_client_config( &client_config );
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );

    resolve_once_setenv( "ROCKETNET_CLIENT_PACKET_FILTER", filter_b );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    resolve_once_token( server, &identity, 0x2000, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int step = 0; step < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; step++ )
        resolve_once_pump( server, &client, 1, 1 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server, 0 ) != 0 );

    resolve_once_unsetenv( "ROCKETNET_SERVER_PACKET_FILTER" );
    resolve_once_unsetenv( "ROCKETNET_CLIENT_PACKET_FILTER" );
    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    rocketnet_quiet( 0 );
}
REGISTER_UNIT_TEST( test_config_client_env_changed_after_create_has_no_effect );

// An invalid transport list set only through the environment is refused at
// create, as the same list in the struct is, never first at connect.
static void test_config_client_env_transport_refused_at_create()
{
    rocketnet_quiet( 1 );
    resolve_once_setenv( "ROCKETNET_CLIENT_TRANSPORT", "no-such-transport" );
    rocketnet_client_config_t config;
    check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.transport = "loopback";
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &status );
    check( client == NULL );
    check( status == ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );
    if ( client )
        rocketnet_client_destroy( client );
    resolve_once_unsetenv( "ROCKETNET_CLIENT_TRANSPORT" );
    rocketnet_quiet( 0 );
}
REGISTER_UNIT_TEST( test_config_client_env_transport_refused_at_create );

#if !ROCKETNET_DEV

// Production posture: the struct keeps the published test filter and the
// environment supplies a private one. Create resolves the environment before
// it validates, so both endpoints are accepted, start and connect.
static void test_production_env_only_packet_filter_accepted()
{
    rocketnet_quiet( 1 );
    rocketnet_development_values_refused_test_enable( 1 );
    resolve_once_identity_t identity;
    resolve_once_identity( &identity );
    char filter_a[16];
    resolve_once_filter_text( resolve_once_filter_a, filter_a, sizeof( filter_a ) );
    resolve_once_setenv( "ROCKETNET_SERVER_PACKET_FILTER", filter_a );
    resolve_once_setenv( "ROCKETNET_CLIENT_PACKET_FILTER", filter_a );

    rocketnet_server_config_t server_config;
    resolve_once_server_config( &server_config, &identity, 1 );
    enum rocketnet_status_t status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( server && rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    resolve_once_client_config( &client_config );
    status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    if ( server && client )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        resolve_once_token( server, &identity, 0x3000, token );
        check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
        for ( int step = 0; step < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; step++ )
            resolve_once_pump( server, &client, 1, 1 );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    }

    resolve_once_unsetenv( "ROCKETNET_SERVER_PACKET_FILTER" );
    resolve_once_unsetenv( "ROCKETNET_CLIENT_PACKET_FILTER" );
    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    rocketnet_development_values_refused_test_enable( 0 );
    rocketnet_quiet( 0 );
}
REGISTER_UNIT_TEST( test_production_env_only_packet_filter_accepted );

// Production posture: the struct holds a private filter and the environment
// supplies the published test filter. The resolved filter is the test filter,
// so both creates refuse it with TEST_PACKET_FILTER_IN_PRODUCTION.
static void test_production_env_only_test_packet_filter_refused()
{
    rocketnet_quiet( 1 );
    rocketnet_development_values_refused_test_enable( 1 );
    resolve_once_identity_t identity;
    resolve_once_identity( &identity );
    static const uint8_t test_filter[ROCKETNET_PACKET_FILTER_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    char test_filter_text[16];
    resolve_once_filter_text( test_filter, test_filter_text, sizeof( test_filter_text ) );
    resolve_once_setenv( "ROCKETNET_SERVER_PACKET_FILTER", test_filter_text );
    resolve_once_setenv( "ROCKETNET_CLIENT_PACKET_FILTER", test_filter_text );

    rocketnet_server_config_t server_config;
    resolve_once_server_config( &server_config, &identity, 1 );
    memcpy( server_config.packet_filter, resolve_once_filter_a, sizeof( server_config.packet_filter ) );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server == NULL );
    check( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );

    rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    memcpy( client_config.packet_filter, resolve_once_filter_a, sizeof( client_config.packet_filter ) );
    status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client == NULL );
    check( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );

    resolve_once_unsetenv( "ROCKETNET_SERVER_PACKET_FILTER" );
    resolve_once_unsetenv( "ROCKETNET_CLIENT_PACKET_FILTER" );
    if ( client )
        rn_test_client_stop_and_destroy( client );
    if ( server )
        rn_test_server_stop_and_destroy( server );
    rocketnet_development_values_refused_test_enable( 0 );
    rocketnet_quiet( 0 );
}
REGISTER_UNIT_TEST( test_production_env_only_test_packet_filter_refused );

#endif // #if !ROCKETNET_DEV

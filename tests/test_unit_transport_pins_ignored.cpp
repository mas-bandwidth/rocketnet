/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The unit suite runs over the loopback transport with no sockets, so a
// transport pin in the environment must not reach it: this case sets both
// pins itself, applies the suite's prepare step, builds a loopback pair
// and expects CONNECTED. The environment it changed is restored on exit.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "portable_crt.h"

#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV
void rocketnet_unit_prepare();

static void pins_set( const char * name, const char * value )
{
    rocketnet_test_env_setenv( name, value, 1 );
}

static void pins_clear( const char * name )
{
    rocketnet_test_env_unsetenv( name );
}

static double pins_ignored_clock = 1000.0;

static double pins_ignored_time_callback( void * )
{
    return pins_ignored_clock;
}

static void test_unit_transport_pins_ignored()
{
    char saved_server[64] = "";
    char saved_client[64] = "";
    const bool had_server = rocketnet_test_env_getenv( "ROCKETNET_SERVER_TRANSPORT", saved_server, sizeof( saved_server ) ) != 0;
    const bool had_client = rocketnet_test_env_getenv( "ROCKETNET_CLIENT_TRANSPORT", saved_client, sizeof( saved_client ) ) != 0;

    pins_set( "ROCKETNET_SERVER_TRANSPORT", "io_uring" );
    pins_set( "ROCKETNET_CLIENT_TRANSPORT", "io_uring" );

    // the suite's isolation step: clears the pins for the run

    rocketnet_unit_prepare();

    rocketnet_public_link_simulator_test_development( 1 );

    pins_ignored_clock = 1000.0;

    const uint64_t protocol_id = 0x70696e73ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );

    struct rocketnet_server_transport_t * server =
        rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    rocketnet_unit_check( server != NULL );
    rocketnet_unit_check( strcmp( rocketnet_server_transport_io_name( server ), "loopback" ) == 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                                    0x9e3779b97f4a7c15ULL, 0, 1ULL << 40, 60, NULL,
                                                                    token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.step_driven = 1;
    client_config.time_callback = pins_ignored_time_callback;

    struct rocketnet_client_transport_t * client =
        rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), pins_ignored_clock );
    rocketnet_unit_check( client != NULL );
    rocketnet_unit_check( strcmp( rocketnet_client_transport_io_name( client ), "loopback" ) == 0 );

    bool connected = false;
    for ( int i = 0; i < 400 && !connected; i++ )
    {
        pins_ignored_clock += 0.01;
        rocketnet_server_transport_update( server, pins_ignored_clock );
        rocketnet_client_transport_update( client, pins_ignored_clock );
        rocketnet_server_transport_send_packets( server );
        rocketnet_client_transport_send_packets( client );
        connected = rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                    rocketnet_server_transport_is_client_connected( server, 0 );
    }

    rocketnet_unit_check( connected );

    rn_test_client_transport_stop_and_destroy( client );
    rn_test_server_transport_stop_and_destroy( server );
    rocketnet_public_link_simulator_test_development( 0 );

    if ( had_server )
        pins_set( "ROCKETNET_SERVER_TRANSPORT", saved_server );
    else
        pins_clear( "ROCKETNET_SERVER_TRANSPORT" );
    if ( had_client )
        pins_set( "ROCKETNET_CLIENT_TRANSPORT", saved_client );
    else
        pins_clear( "ROCKETNET_CLIENT_TRANSPORT" );

    printf( "test_unit_transport_pins_ignored\n" );
}

REGISTER_UNIT_TEST( test_unit_transport_pins_ignored );

#endif // #if ROCKETNET_DEV

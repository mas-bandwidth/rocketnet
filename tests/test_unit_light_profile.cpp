/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public light-client profile, pinned. A 1000-slot server and a client
// size their create blocks within the figures USAGE.md records, and both
// endpoints create and destroy on the profile without opening a socket.

#include "test_unit_registry.h"
#include "rocketnet.h"

#include <stddef.h>

#define check rocketnet_unit_check

// USAGE.md "Server memory footprint" and docs/LOAD.md record these exact
// figures, measured with rocketnet_server_required_memory at max_clients
// 1000 and rocketnet_client_required_memory under the profile.
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 868798336 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 2976256 )

static void test_light_profile_server_1000_required_memory()
{
    struct rocketnet_server_config_t config;
    check( rocketnet_server_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );

    config.max_clients = 1000;
    const size_t bytes = rocketnet_server_required_memory( &config );
    check( bytes > 0 );
    check( bytes <= LIGHT_SERVER_1000_MAX_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_server_1000_required_memory );

static void test_light_profile_client_required_memory()
{
    struct rocketnet_client_config_t config;
    check( rocketnet_client_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );

    const size_t bytes = rocketnet_client_required_memory( &config );
    check( bytes > 0 );
    check( bytes <= LIGHT_CLIENT_MAX_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_client_required_memory );

static void test_light_profile_configs_create()
{
    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_light_config( &server_config ) == ROCKETNET_STATUS_OK );

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_stopped( server ) );
    rocketnet_server_destroy( server );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_light_config( &client_config ) == ROCKETNET_STATUS_OK );

    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_destroy( client );
}

REGISTER_UNIT_TEST( test_light_profile_configs_create );

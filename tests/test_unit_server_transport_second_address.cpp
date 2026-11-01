/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server transport's second public address refusals: a
// second_public_address that does not parse, that names the public
// address's own family, or that names another port is refused with
// ROCKETNET_STATUS_INVALID_ADDRESS while the config is resolved, before the
// transport is allocated and before any socket opens.

#include "test_unit_registry.h"

#if ROCKETNET_DEV

#include "rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static int second_address_create_status( const char * public_address, const char * second_address )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = 4;
    snprintf( config.transport, sizeof( config.transport ), "portable" );
    snprintf( config.second_public_address, sizeof( config.second_public_address ), "%s", second_address );

    int status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_ex( NULL, public_address, &config, 0.0, &status );
    check( server == NULL );

    return status;
}

static void test_server_transport_second_address_refusals()
{
    // a second address that does not parse

    check( second_address_create_status( "127.0.0.1:0", "not an address" ) == ROCKETNET_STATUS_INVALID_ADDRESS );

    // a second address of the public address's own family, on the same port

    check( second_address_create_status( "127.0.0.1:0", "127.0.0.2:0" ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( second_address_create_status( "[::1]:0", "[::2]:0" ) == ROCKETNET_STATUS_INVALID_ADDRESS );

    // a second address of the other family on another port

    check( second_address_create_status( "127.0.0.1:0", "[::1]:1" ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( second_address_create_status( "[::1]:40000", "127.0.0.1:40001" ) == ROCKETNET_STATUS_INVALID_ADDRESS );
}

REGISTER_UNIT_TEST( test_server_transport_second_address_refusals );

#endif // #if ROCKETNET_DEV

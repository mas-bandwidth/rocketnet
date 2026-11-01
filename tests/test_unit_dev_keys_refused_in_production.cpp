/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A production build (ROCKETNET_DEV=0) refuses a server whose config carries
// either published development public key, each on its own: a token forged
// with the published development backend key is otherwise a full
// authentication bypass, and the published server pair lets anyone read the
// traffic. The refusal comes before any socket opens.
//
// A development build accepts what this checks is refused, so the case is
// registered in the production unit suite only.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"

#include <string.h>

#if !ROCKETNET_DEV

#define check rocketnet_unit_check

// production keys, a protocol id and a packet filter of the caller's own, so
// the development-key check is the only refusal the config can reach
static void production_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config_current( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
}

static void test_dev_keys_refused_in_production()
{
    // the development backend key with a production server pair

    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        memcpy( config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
        int status = -1;
        check( rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
    }

    // the development server public key with a production backend key

    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        memcpy( config.server_public_key, rocketnet_dev_server_public_key, 32 );
        int status = -1;
        check( rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
    }
}

REGISTER_UNIT_TEST( test_dev_keys_refused_in_production );

#endif // #if !ROCKETNET_DEV

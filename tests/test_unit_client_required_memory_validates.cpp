/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// rocketnet_client_required_memory validates a config exactly as
// rocketnet_client_create does (the one check, rn_client_config_valid), as
// rocketnet_server_required_memory does for the server: each config create
// refuses gives 0, never a size for a block create would not use. No socket,
// no thread, no real time: client create opens nothing until connect.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_fragment.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

enum client_required_memory_case_t
{
    CLIENT_REQUIRED_MEMORY_NO_CHANNELS,
    CLIENT_REQUIRED_MEMORY_MESSAGE_NEGATIVE,
    CLIENT_REQUIRED_MEMORY_MESSAGE_OVER,
    CLIENT_REQUIRED_MEMORY_FRAGMENTED_NEGATIVE,
    CLIENT_REQUIRED_MEMORY_FRAGMENTED_OVER,
    CLIENT_REQUIRED_MEMORY_TRANSPORT_LIST,
    CLIENT_REQUIRED_MEMORY_SEND_KBPS_NEGATIVE,
    CLIENT_REQUIRED_MEMORY_NUM_CASES
};

static void client_required_memory_config( int which, struct rocketnet_client_config_t * config )
{
    check( rocketnet_client_default_config( config ) == ROCKETNET_STATUS_OK );
    switch ( which )
    {
        case CLIENT_REQUIRED_MEMORY_NO_CHANNELS:         config->num_channels = 0; break;
        case CLIENT_REQUIRED_MEMORY_MESSAGE_NEGATIVE:    config->max_message_bytes = -1; break;
        case CLIENT_REQUIRED_MEMORY_MESSAGE_OVER:        config->max_message_bytes = ROCKETNET_MAX_MESSAGE_BYTES + 1; break;
        case CLIENT_REQUIRED_MEMORY_FRAGMENTED_NEGATIVE: config->max_fragmented_messages = -1; break;
        case CLIENT_REQUIRED_MEMORY_FRAGMENTED_OVER:     config->max_fragmented_messages = ROCKETNET_MAX_FRAGMENTS + 1; break;
        case CLIENT_REQUIRED_MEMORY_TRANSPORT_LIST:      config->transport = "no-such-transport"; break;
        case CLIENT_REQUIRED_MEMORY_SEND_KBPS_NEGATIVE:  config->send_kbps = -1; break;
        default:                                         check( false ); break;
    }
}

static void test_client_required_memory_validates()
{
    // the defaults: create succeeds and required_memory gives a size
    struct rocketnet_client_config_t config;
    check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_required_memory( &config ) > 0 );

    // each config create refuses: required_memory gives 0
    for ( int i = 0; i < CLIENT_REQUIRED_MEMORY_NUM_CASES; i++ )
    {
        client_required_memory_config( i, &config );
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        check( rocketnet_client_create( NULL, &config, &status ) == NULL );
        check( status != ROCKETNET_STATUS_OK );
        check( rocketnet_client_required_memory( &config ) == 0 );
    }
}
REGISTER_UNIT_TEST( test_client_required_memory_validates );

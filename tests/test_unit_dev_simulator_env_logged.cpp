/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_dev_simulator_env_logged.cpp -- the development network
// simulator's environment form (ROCKETNET_DEV_SERVER_LATENCY_MS and the rest)
// is logged as every other override is (rocketnet.h, Config): an applied
// variable names the value it displaced, an empty one is ignored and said so.
//
// No socket, no clock: server create binds nothing, and the environment is
// the development library's in-memory overlay (tests/test_env_source.h).

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"

#if ROCKETNET_DEV

#include "test_env_source.h"

#include <string.h>

#define check rocketnet_unit_check

struct dev_env_log_t
{
    int applied;
    int ignored;
};

static void dev_env_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) level;
    dev_env_log_t * log = (dev_env_log_t *) context;
    char line[512];
    const size_t bytes = length < sizeof( line ) - 1 ? length : sizeof( line ) - 1;
    memcpy( line, message, bytes );
    line[bytes] = '\0';
    if ( strstr( line, "ROCKETNET_DEV_SERVER_LATENCY_MS=25" ) && strstr( line, "displaces 0" ) )
    {
        log->applied++;
    }
    if ( strstr( line, "ignores empty ROCKETNET_DEV_SERVER_JITTER_MS" ) )
    {
        log->ignored++;
    }
}

static void test_dev_simulator_env_overrides_are_logged()
{
    check( rocketnet_test_env_setenv( "ROCKETNET_DEV_SERVER_LATENCY_MS", "25", 1 ) == 0 );
    check( rocketnet_test_env_setenv( "ROCKETNET_DEV_SERVER_JITTER_MS", "", 1 ) == 0 );

    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = 0x44455645ULL;
    config.max_clients = 2;

    dev_env_log_t log = { 0, 0 };
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_quiet( 0 );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( dev_env_log_capture, &log );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:40000", NULL, &config, &status );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );
    rocketnet_quiet( 1 );
    rocketnet_test_env_restore();

    check( server != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );

    check( log.applied == 1 );
    check( log.ignored == 1 );
}

REGISTER_UNIT_TEST( test_dev_simulator_env_overrides_are_logged );

#endif // #if ROCKETNET_DEV

/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Round 2 survivors of the 2026-11-10 mutation hunt (run 2) over
// sdk/rocketnet_server_transport.cpp that only a production library reaches:
// a production library refuses a nonzero link simulator setting at create
// (a development library owns a simulator and accepts it). The case is
// registered in the production (ROCKETNET_DEV=0) unit suite only, and runs
// in the Release unit legs. No socket: the refusal comes before anything is
// allocated or opened.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#include <stdio.h>
#include <string.h>

#if !ROCKETNET_DEV

// a production config the create would otherwise go on with: its own
// packet filter and non-zero, non-development keys
static void r2_st_production_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
}

// A production create with a nonzero simulated link setting is refused, and
// the refusal is written through the caller's status: CONFIG_OUT_OF_RANGE.
// mutation gap: hunt 2/4926 (public link simulator refusal: if ( status ) negated)
// mutation gap: hunt 2/4927 (public link simulator refusal: CONFIG_OUT_OF_RANGE write deleted)
static void test_gaps_r2_st_production_create_refuses_a_simulated_link()
{
    struct rocketnet_server_transport_config_t config;
    r2_st_production_config( &config );
    config.simulated_loss_percent = 5;

    int status = -1;
    const bool refused = rocketnet_server_transport_create_internal( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL;
    if ( !refused || status != ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE )
    {
        printf( "fault: a production create with a simulated link setting was not refused with CONFIG_OUT_OF_RANGE (refused %d, status %d)\n", refused ? 1 : 0, status );
        fflush( stdout );
    }
    rocketnet_unit_check( refused );
    rocketnet_unit_check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
}

REGISTER_UNIT_TEST( test_gaps_r2_st_production_create_refuses_a_simulated_link );

#endif // #if !ROCKETNET_DEV

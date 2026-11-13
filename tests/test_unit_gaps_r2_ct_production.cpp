/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The production client transport (ROCKETNET_DEV=0) at create
// (sdk/rocketnet_client_transport.cpp): it refuses a link simulator setting
// with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, written only through a status
// pointer it was given. A development build accepts the simulator, so the
// case is production only: the Release DEV=OFF unit leg runs it. Every
// create here is refused before any worker or socket exists. Time is an
// argument starting at 1000. Each failing check prints the fault first.

#include "test_unit_registry.h"

#if !ROCKETNET_DEV

#include "unit_shutdown.h"
#include "loopback_hub.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// a token sealed to the published development server key, naming
// 127.0.0.1:40000: a production client refuses it after the config checks,
// before any socket or worker exists
static void r2_production_token( uint8_t * token )
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
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

// the default config with a packet filter of the caller's own: a production
// build refuses the published test filter the default config carries
static void r2_production_config( struct rocketnet_client_transport_config_t * config )
{
    rocketnet_client_transport_default_config( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    snprintf( config->transport, sizeof( config->transport ), "%s", "loopback" );
    config->loopback_hub = rocketnet_test_loopback_hub();
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6953, 2/6954 (a production create refuses a simulator setting with its status)
//
// simulated_loss_percent 1 is in range, so the config walk passes it; the
// production link simulator check refuses it. The status the caller passed
// reads CONFIG_OUT_OF_RANGE, and a create given no status pointer refuses the
// same way without writing anywhere.

static void test_gaps_r2_ct_production_client_refuses_a_link_simulator_setting()
{
    rocketnet_quiet( true );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_production_token( token );

    struct rocketnet_client_transport_config_t config;
    r2_production_config( &config );
    config.simulated_loss_percent = 1;

    int status = -1;
    struct rocketnet_client_transport_t * transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, &status );
    if ( transport != NULL || status != ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE )
    {
        printf( "mutation gap hunt 2/6953,6954: a production client create with simulated_loss_percent 1 returned %s with status %d, not NULL with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE (%d)\n",
                transport ? "a transport" : "NULL", status, (int) ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    }
    check( transport == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, NULL );
    if ( transport != NULL )
    {
        printf( "mutation gap hunt 2/6953: a production client create with simulated_loss_percent 1 and no status pointer returned a transport\n" );
    }
    check( transport == NULL );

    // the same config with simulated_loss_percent 0 passes the simulator
    // check: it is refused later, for the token's development key, so the
    // refusal above is the simulator's (the create stops before any worker)

    config.simulated_loss_percent = 0;
    status = -1;
    check( rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION );

    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_production_client_refuses_a_link_simulator_setting );

#endif // #if !ROCKETNET_DEV

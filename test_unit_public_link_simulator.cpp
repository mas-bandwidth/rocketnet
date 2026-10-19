/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public link simulator's production refusal: a nonzero simulated loss,
// latency, jitter or duplicate value is refused by the same config walk a
// production create uses, and admitted when the build is development. This
// exercises the walk over the real transport config tables, so the four
// fields' presence, their ranges and their refusal are pinned together.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static void public_link_simulator_check_fields( const struct rocketnet_config_field_t * fields, int num_fields )
{
    int found_loss = 0;
    int found_latency = 0;
    int found_jitter = 0;
    int found_duplicate = 0;
    for ( int i = 0; i < num_fields; i++ )
    {
        if ( strcmp( fields[i].name, "simulated_loss_percent" ) == 0 )
            found_loss = 1;
        if ( strcmp( fields[i].name, "simulated_latency_ms" ) == 0 )
            found_latency = 1;
        if ( strcmp( fields[i].name, "simulated_jitter_ms" ) == 0 )
            found_jitter = 1;
        if ( strcmp( fields[i].name, "simulated_duplicate_percent" ) == 0 )
            found_duplicate = 1;
    }
    check( found_loss );
    check( found_latency );
    check( found_jitter );
    check( found_duplicate );
}

static void public_link_simulator_production_refusal()
{
    // the client transport, from its real table
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    int client_fields = 0;
    const struct rocketnet_config_field_t * client_table = rocketnet_client_transport_config_fields( &client_fields );
    public_link_simulator_check_fields( client_table, client_fields );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_latency_ms = 25;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_latency_ms = 0;
    client_config.simulated_duplicate_percent = 50;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_duplicate_percent = 0;
    client_config.simulated_loss_percent = 100;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    // the server transport, from its real table
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config( &server_config );
    int server_fields = 0;
    const struct rocketnet_config_field_t * server_table = rocketnet_server_transport_config_fields( &server_fields );
    public_link_simulator_check_fields( server_table, server_fields );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    server_config.simulated_jitter_ms = 10;
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );

    // ranges: percentages cap at 100, milliseconds at 60000
    client_config.simulated_loss_percent = 101;
    check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
    client_config.simulated_loss_percent = 0;
    client_config.simulated_latency_ms = 60001;
    check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );
    client_config.simulated_latency_ms = 0;

    printf( "public_link_simulator_production_refusal\n" );
}

REGISTER_UNIT_TEST( public_link_simulator_production_refusal );

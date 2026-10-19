/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public link simulator's production refusal: a nonzero simulated loss,
// latency, jitter or duplicate value is refused by the same config walk a
// production create uses, and admitted when the build is development. The
// transport configs this card was meant to extend live in sdk/rocketnet_layers.h
// on this base, so this case exercises the walk with a local struct shaped
// like the four fields until the card's PATHS cover that header.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"

#include <stddef.h>
#include <stdio.h>

#define check rocketnet_unit_check

struct public_link_simulator_fields_t
{
    int simulated_loss_percent;
    int simulated_latency_ms;
    int simulated_jitter_ms;
    int simulated_duplicate_percent;
};

static const struct rocketnet_config_field_t public_link_simulator_field_table[] =
{
    { "simulated_loss_percent",      ROCKETNET_CONFIG_KIND_INT, (int) offsetof( struct public_link_simulator_fields_t, simulated_loss_percent ),      (int) sizeof(int), 0, 100,   0 },
    { "simulated_latency_ms",        ROCKETNET_CONFIG_KIND_INT, (int) offsetof( struct public_link_simulator_fields_t, simulated_latency_ms ),        (int) sizeof(int), 0, 60000, 0 },
    { "simulated_jitter_ms",         ROCKETNET_CONFIG_KIND_INT, (int) offsetof( struct public_link_simulator_fields_t, simulated_jitter_ms ),         (int) sizeof(int), 0, 60000, 0 },
    { "simulated_duplicate_percent", ROCKETNET_CONFIG_KIND_INT, (int) offsetof( struct public_link_simulator_fields_t, simulated_duplicate_percent ), (int) sizeof(int), 0, 100,   0 },
};

static const int public_link_simulator_field_count = (int) ( sizeof( public_link_simulator_field_table ) / sizeof( public_link_simulator_field_table[0] ) );

static void public_link_simulator_production_refusal()
{
    struct public_link_simulator_fields_t config;
    memset( &config, 0, sizeof( config ) );

    check( rocketnet_config_validate_public_link_simulator( "client", &config, public_link_simulator_field_table, public_link_simulator_field_count, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &config, public_link_simulator_field_table, public_link_simulator_field_count, true ) );

    config.simulated_latency_ms = 25;
    check( !rocketnet_config_validate_public_link_simulator( "client", &config, public_link_simulator_field_table, public_link_simulator_field_count, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &config, public_link_simulator_field_table, public_link_simulator_field_count, true ) );

    config.simulated_latency_ms = 0;
    config.simulated_duplicate_percent = 50;
    check( !rocketnet_config_validate_public_link_simulator( "server", &config, public_link_simulator_field_table, public_link_simulator_field_count, false ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &config, public_link_simulator_field_table, public_link_simulator_field_count, true ) );

    printf( "public_link_simulator_production_refusal\n" );
}

REGISTER_UNIT_TEST( public_link_simulator_production_refusal );

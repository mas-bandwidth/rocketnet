/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP driver backend reports its kernel prefilter counters through the
// transport counter surface (rocketnet_server_driver_operations_t
// prefilter_counters). Only a driver with a kernel prefilter fills it; the
// other backends' stay NULL. This checks the registry, not a live map: no
// driver is created, no XDP program is loaded or attached.

#include "test_unit_registry.h"

#include "rocketnet_driver.h"

#define check rocketnet_unit_check

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

static void prefilter_counters_xdp_driver_reports_them()
{
    const rocketnet_driver_definition_t * xdp = rocketnet_driver_definition( ROCKETNET_DRIVER_XDP );
    check( xdp->backend->server->prefilter_counters != NULL );

    const rocketnet_driver_definition_t * portable = rocketnet_driver_definition( ROCKETNET_DRIVER_PORTABLE );
    check( portable->backend->server->prefilter_counters == NULL );

    const rocketnet_driver_definition_t * gso = rocketnet_driver_definition( ROCKETNET_DRIVER_GSO );
    check( gso->backend->server->prefilter_counters == NULL );
}

#else

static void prefilter_counters_xdp_driver_reports_them()
{
    printf( "prefilter_counters_xdp_driver_reports_them [SKIPPED the XDP driver backend registers only on Linux]\n" );
    fflush( stdout );
}

#endif

REGISTER_UNIT_TEST( prefilter_counters_xdp_driver_reports_them );

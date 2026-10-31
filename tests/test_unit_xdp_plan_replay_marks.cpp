/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server memory plan counts every allocation create makes: a transport
// that takes replay marks (xdp) also gets the per-slot replay records
// (rocketnet_server_replay_marks_create), and its plan carries their bytes
// beside the protocol tables. The plan is arithmetic: no socket, no clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_server_replay_marks.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

#define check rocketnet_unit_check

static void test_xdp_memory_plan_counts_the_replay_marks()
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    rocketnet_server_memory_plan_t xdp;
    rocketnet_server_memory_plan_t gso;
    check( rocketnet_server_transport_memory_plan( &config, "xdp", xdp ) );
    check( rocketnet_server_transport_memory_plan( &config, "gso", gso ) );

    const uint64_t marks = rocketnet_server_replay_marks_allocation_bytes( config.max_clients );
    check( marks > 0 );
    check( xdp.protocol_bytes == gso.protocol_bytes + marks );
}

REGISTER_UNIT_TEST( test_xdp_memory_plan_counts_the_replay_marks );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

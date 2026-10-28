/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Windows iocp transport always runs exactly one completion worker plus its
// one send worker, whatever num_xdp_queues says (that field sizes the Linux
// AF_XDP and gso transports, not iocp). This pins the memory plan: worker
// threads stay at 2 and sockets at 1 for 0, 4 and 1024 queues. No socket is
// opened; the plan is a pure size computation.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_memory.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static void test_iocp_server_has_one_completion_worker()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    rocketnet_server_memory_plan_t plan;

    config.num_xdp_queues = 0;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 2 );
    rocketnet_unit_check( plan.sockets == 1 );

    const uint64_t transport_io_bytes = plan.transport_io_bytes;

    config.num_xdp_queues = 4;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 2 );
    rocketnet_unit_check( plan.sockets == 1 );
    rocketnet_unit_check( plan.transport_io_bytes == transport_io_bytes );

    config.num_xdp_queues = 1024;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 2 );
    rocketnet_unit_check( plan.sockets == 1 );
    rocketnet_unit_check( plan.transport_io_bytes == transport_io_bytes );
}

REGISTER_UNIT_TEST( test_iocp_server_has_one_completion_worker );

#endif // ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

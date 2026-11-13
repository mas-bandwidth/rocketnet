/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Windows iocp transport always runs exactly one completion worker plus its
// one send worker, whatever num_xdp_queues says (that field sizes the Linux
// AF_XDP and gso transports, not iocp). This pins the memory plan: worker
// threads stay at 2 and sockets at 1 for 0, 4 and 1024 queues. No socket is
// opened; the plan is a pure size computation. The second test pins the count
// the transport is created with, through the backend's own create.

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

#if ROCKETNET_DEV

// The iocp backend's create, through the driver's create seam: the worker
// count the transport is actually created with, not only the plan's. The
// seam records the arguments and refuses, so no socket is opened.

#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_server_iocp.h"

extern const rocketnet_transport_backend_t rocketnet_iocp_transport;
extern void rocketnet_server_iocp_set_create_for_test( rocketnet_server_iocp_t * ( *function )(void *, rocketnet_address_t *, int, int, int, int, int *) );

static int iocp_create_seen_workers = -1;
static int iocp_create_seen_max_clients = -1;
static int iocp_create_calls = 0;

static rocketnet_server_iocp_t * iocp_create_recorder( void *, rocketnet_address_t *, int num_workers, int max_clients, int, int, int * )
{
    iocp_create_calls++;
    iocp_create_seen_workers = num_workers;
    iocp_create_seen_max_clients = max_clients;
    return NULL;
}

static void test_iocp_server_creates_one_completion_worker()
{
    rocketnet_server_transport_config_t transport;
    rocketnet_server_transport_default_config( &transport );

    rocketnet_server_transport_io_config_t config = {};
    config.name = "iocp";
    config.transport = &transport;

    rocketnet_address_t bind = {};

    rocketnet_server_iocp_set_create_for_test( iocp_create_recorder );

    // the completion workers are num_xdp_queues when it asks for more than
    // one, otherwise one; past ROCKETNET_IOCP_MAX_WORKERS the driver refuses
    // before it calls create
    const int queues[] = { 0, 1, 4 };
    const int expected_workers[] = { 1, 1, 4 };
    for ( int i = 0; i < 3; ++i )
    {
        transport.num_xdp_queues = queues[i];
        iocp_create_seen_workers = -1;
        rocketnet_unit_check( rocketnet_iocp_transport.server->create( NULL, &config, &bind ) == NULL );
        rocketnet_unit_check( iocp_create_calls == i + 1 );
        rocketnet_unit_check( iocp_create_seen_workers == expected_workers[i] );
        rocketnet_unit_check( iocp_create_seen_max_clients == transport.max_clients );
    }

    transport.num_xdp_queues = ROCKETNET_IOCP_MAX_WORKERS + 1;
    rocketnet_unit_check( rocketnet_iocp_transport.server->create( NULL, &config, &bind ) == NULL );
    rocketnet_unit_check( iocp_create_calls == 3 );

    rocketnet_server_iocp_set_create_for_test( NULL );
}

REGISTER_UNIT_TEST( test_iocp_server_creates_one_completion_worker );

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

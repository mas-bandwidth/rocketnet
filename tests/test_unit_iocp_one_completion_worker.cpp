/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Windows iocp transport runs one completion worker for each of
// num_xdp_queues (more than one: that many, else 1; each with its own
// socket) plus ROCKETNET_IOCP_SEND_THREADS send threads. This pins the memory
// plan by value: the planned worker threads are the completion workers plus
// the send threads, the number the startup reports and the runtime starts
// (it was one short: the plan added one send worker, the runtime two). No
// socket is opened; the plan is a pure size computation. The second test pins
// the worker count the transport is created with, through the backend's own
// create.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_server_iocp.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static void test_iocp_memory_plan_counts_workers_and_send_threads()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    rocketnet_server_memory_plan_t plan;

    // read by value: 2 send threads, so 1 + 2 = 3 threads for one worker
    rocketnet_unit_check( ROCKETNET_IOCP_SEND_THREADS == 2 );

    config.num_xdp_queues = 0;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 3 );
    rocketnet_unit_check( plan.sockets == 1 );

    const uint64_t one_worker_bytes = plan.transport_io_bytes;

    config.num_xdp_queues = 1;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 3 );
    rocketnet_unit_check( plan.sockets == 1 );
    rocketnet_unit_check( plan.transport_io_bytes == one_worker_bytes );

    config.num_xdp_queues = 2;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 2 + ROCKETNET_IOCP_SEND_THREADS );
    rocketnet_unit_check( plan.worker_threads == 4 );
    rocketnet_unit_check( plan.sockets == 2 );
    rocketnet_unit_check( plan.transport_io_bytes > one_worker_bytes );

    // 1000 clients is 8 lanes: the 2 send threads; 512 clients is 4 lanes: still 2
    config.num_xdp_queues = 1;
    config.max_clients = 1000;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( rocketnet_server_iocp_send_lanes( 1000 ) == 8 && plan.worker_threads == 1 + 2 );

    // the plan refuses what create refuses: more workers than the transport has
    config.num_xdp_queues = ROCKETNET_IOCP_MAX_WORKERS + 1;
    rocketnet_unit_check( !rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    config.num_xdp_queues = 1024;
    rocketnet_unit_check( !rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    config.num_xdp_queues = 2;

    config.num_xdp_queues = ROCKETNET_IOCP_MAX_WORKERS;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == ROCKETNET_IOCP_MAX_WORKERS + ROCKETNET_IOCP_SEND_THREADS );
    rocketnet_unit_check( plan.sockets == ROCKETNET_IOCP_MAX_WORKERS );
}

REGISTER_UNIT_TEST( test_iocp_memory_plan_counts_workers_and_send_threads );

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

static void test_iocp_server_creates_a_worker_for_each_queue()
{
    rocketnet_server_transport_config_t transport;
    rocketnet_server_transport_default_config( &transport );

    rocketnet_server_transport_io_config_t config = {};
    config.name = "iocp";
    config.transport = &transport;

    rocketnet_address_t bind = {};

    rocketnet_server_iocp_set_create_for_test( iocp_create_recorder );

    // num_xdp_queues of 0 and 1 is one worker, more than one is that many
    const int queues[] = { 0, 1, 4 };
    const int workers[] = { 1, 1, 4 };
    for ( int i = 0; i < 3; ++i )
    {
        transport.num_xdp_queues = queues[i];
        iocp_create_seen_workers = -1;
        rocketnet_unit_check( rocketnet_iocp_transport.server->create( NULL, &config, &bind ) == NULL );
        rocketnet_unit_check( iocp_create_calls == i + 1 );
        rocketnet_unit_check( iocp_create_seen_workers == workers[i] );
        rocketnet_unit_check( iocp_create_seen_max_clients == transport.max_clients );
    }

    // past the transport's workers is refused before any socket is made
    transport.num_xdp_queues = 1024;
    rocketnet_unit_check( rocketnet_iocp_transport.server->create( NULL, &config, &bind ) == NULL );
    rocketnet_unit_check( iocp_create_calls == 3 );

    rocketnet_server_iocp_set_create_for_test( NULL );
}

REGISTER_UNIT_TEST( test_iocp_server_creates_a_worker_for_each_queue );

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

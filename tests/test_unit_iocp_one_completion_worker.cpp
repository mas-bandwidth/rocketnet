/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Windows iocp transport runs num_iocp_completion_workers completion
// workers (each with its own socket) plus num_iocp_send_threads send threads.
// With nothing asked, a bind address the workers can share gets the defaults
// (ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS, ROCKETNET_IOCP_DEFAULT_SEND_THREADS)
// and the wildcard gets one completion worker, said in a warning. This pins
// the memory plan by value: the planned worker threads are the completion
// workers plus the send threads, the number the runtime starts. No socket is
// opened; the plan is a pure size computation. The second test pins the
// counts the transport is created with, through the backend's own create.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_server_iocp.h"

#include <string.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static void test_iocp_memory_plan_counts_workers_and_send_threads()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    rocketnet_server_memory_plan_t plan;

    // read by value: the defaults are 2 completion workers and 2 send threads
    rocketnet_unit_check( ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS == 2 );
    rocketnet_unit_check( ROCKETNET_IOCP_SEND_THREADS == 2 );

    // nothing asked on the wildcard (no bind address): 1 worker + 2 send threads
    config.num_iocp_completion_workers = 0;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 3 );
    rocketnet_unit_check( plan.sockets == 1 );

    const uint64_t one_worker_bytes = plan.transport_io_bytes;

    config.num_iocp_completion_workers = 1;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 3 );
    rocketnet_unit_check( plan.sockets == 1 );
    rocketnet_unit_check( plan.transport_io_bytes == one_worker_bytes );

    // nothing asked on a specific bind address: the defaults, 2 + 2
    config.num_iocp_completion_workers = 0;
    snprintf( config.bind_ipv4_address, sizeof( config.bind_ipv4_address ), "10.0.0.5" );
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 4 );
    rocketnet_unit_check( plan.sockets == 2 );
    rocketnet_unit_check( plan.transport_io_bytes > one_worker_bytes );

    // the wildcard written out is still the wildcard
    snprintf( config.bind_ipv4_address, sizeof( config.bind_ipv4_address ), "0.0.0.0" );
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.sockets == 1 );
    config.bind_ipv4_address[0] = '\0';

    // 1000 clients is 8 lanes: the 2 send threads; one send thread asked is one
    config.num_iocp_completion_workers = 1;
    config.max_clients = 1000;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( rocketnet_server_iocp_send_lanes( 1000 ) == 8 && plan.worker_threads == 1 + 2 );
    config.num_iocp_send_threads = 1;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 1 + 1 );
    config.num_iocp_send_threads = 8;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 1 + 8 );

    // the plan refuses what create refuses: send threads past the lanes,
    // workers past the transport's
    config.num_iocp_send_threads = 9;
    rocketnet_unit_check( !rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    config.num_iocp_send_threads = 0;
    config.num_iocp_completion_workers = ROCKETNET_IOCP_MAX_WORKERS + 1;
    rocketnet_unit_check( !rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    config.num_iocp_completion_workers = 1024;
    rocketnet_unit_check( !rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );

    config.num_iocp_completion_workers = ROCKETNET_IOCP_MAX_WORKERS;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    rocketnet_unit_check( plan.worker_threads == ROCKETNET_IOCP_MAX_WORKERS + ROCKETNET_IOCP_SEND_THREADS );
    rocketnet_unit_check( plan.sockets == ROCKETNET_IOCP_MAX_WORKERS );
}

REGISTER_UNIT_TEST( test_iocp_memory_plan_counts_workers_and_send_threads );

#if ROCKETNET_DEV

// The iocp backend's create, through the driver's create seam: the counts
// the transport is actually created with, not only the plan's. The seam
// records the arguments and refuses, so no socket is opened.

#include "../sdk/rocketnet_driver.h"

extern const rocketnet_transport_backend_t rocketnet_iocp_transport;
extern void rocketnet_server_iocp_set_create_for_test( rocketnet_server_iocp_t * ( *function )(void *, rocketnet_address_t *, int, int, int, int, int, int *) );

static int iocp_create_seen_workers = -1;
static int iocp_create_seen_send_threads = -1;
static int iocp_create_seen_max_clients = -1;
static int iocp_create_calls = 0;

static rocketnet_server_iocp_t * iocp_create_recorder( void *, rocketnet_address_t *, int num_workers, int num_send_threads, int max_clients, int, int, int * )
{
    iocp_create_calls++;
    iocp_create_seen_workers = num_workers;
    iocp_create_seen_send_threads = num_send_threads;
    iocp_create_seen_max_clients = max_clients;
    return NULL;
}

static void test_iocp_server_creates_the_workers_asked()
{
    rocketnet_server_transport_config_t transport;
    rocketnet_server_transport_default_config( &transport );

    int status = ROCKETNET_STATUS_OK;
    rocketnet_server_transport_io_config_t config = {};
    config.name = "iocp";
    config.transport = &transport;
    config.status = &status;

    rocketnet_address_t wildcard = {};
    wildcard.type = ROCKETNET_ADDRESS_IPV4;
    rocketnet_address_t specific;
    rocketnet_unit_check( rocketnet_address_parse( &specific, "10.0.0.5:0" ) );
    rocketnet_address_t mapped;
    rocketnet_unit_check( rocketnet_address_parse( &mapped, "[::ffff:10.0.0.5]:0" ) );

    rocketnet_server_iocp_set_create_for_test( iocp_create_recorder );

    // which path was taken, by value: nothing asked is one worker on the
    // wildcard and on an IPv4-mapped address, the default on a specific one;
    // asked is that many on either (the transport refuses more than one on
    // the wildcard); the send threads asked pass through for create to check
    struct
    {
        rocketnet_address_t * bind;
        int asked;
        int send_threads;
        int workers;
    } cases[] = {
        { &wildcard, 0, 0, 1 },
        { &mapped, 0, 0, 1 },
        { &specific, 0, 0, ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS },
        { &specific, 1, 0, 1 },
        { &specific, 4, 3, 4 },
        { &wildcard, 4, 0, 4 },
    };
    const int num_cases = (int) ( sizeof( cases ) / sizeof( cases[0] ) );
    for ( int i = 0; i < num_cases; ++i )
    {
        transport.num_iocp_completion_workers = cases[i].asked;
        transport.num_iocp_send_threads = cases[i].send_threads;
        iocp_create_seen_workers = -1;
        rocketnet_unit_check( rocketnet_iocp_transport.server->create( NULL, &config, cases[i].bind ) == NULL );
        rocketnet_unit_check( iocp_create_calls == i + 1 );
        rocketnet_unit_check( iocp_create_seen_workers == cases[i].workers );
        rocketnet_unit_check( iocp_create_seen_send_threads == cases[i].send_threads );
        rocketnet_unit_check( iocp_create_seen_max_clients == transport.max_clients );
    }

    // past the transport's workers is refused before any socket is made
    transport.num_iocp_completion_workers = 1024;
    transport.num_iocp_send_threads = 0;
    status = ROCKETNET_STATUS_OK;
    rocketnet_unit_check( rocketnet_iocp_transport.server->create( NULL, &config, &specific ) == NULL );
    rocketnet_unit_check( iocp_create_calls == num_cases );
    rocketnet_unit_check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    rocketnet_server_iocp_set_create_for_test( NULL );
}

REGISTER_UNIT_TEST( test_iocp_server_creates_the_workers_asked );

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

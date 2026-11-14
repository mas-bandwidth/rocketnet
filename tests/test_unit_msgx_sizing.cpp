/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS batched server transport (msgx) is sized by the configured
// max_clients, never by the build's ROCKETNET_MAX_CLIENTS or a fixed table
// size (sdk/rocketnet_constants.h, "Per-server sizing"). Its three receive
// banks each hold one update's receive budget at the configured count
// (rocketnet_server_receive_packets_per_update, the budget the Linux and
// Windows servers size theirs by), and its socket table, the table's lists
// and index and every send worker's sort buckets hold the configured clients.
// No socket, no clock, no thread.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_constants.h"
#include "rocketnet_server_msgx.h"
#include "rocketnet_msgx_socket_table.h"
#include "rocketnet_memory.h"
#include "rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// A server for 8 clients plans at least the receive banks and the per-client
// table and buckets less than a server for 1000: three banks of the receive
// budget's difference in frames, and the difference in clients in socket
// slots and in each worker's sort buckets. A plan that keeps either at a
// build constant fails.
static void test_msgx_plan_follows_configured_clients()
{
    const int workers = ROCKETNET_MSGX_DEFAULT_WORKERS;
    const uint64_t plan_small = rocketnet_server_msgx_allocation_bytes( workers, 8 );
    const uint64_t plan_large = rocketnet_server_msgx_allocation_bytes( workers, 1000 );
    check( plan_small > 0 );
    check( plan_large > plan_small );

    const uint64_t budget_gap = (uint64_t) ( rocketnet_server_receive_packets_per_update( 1000 ) - rocketnet_server_receive_packets_per_update( 8 ) );
    const uint64_t client_gap = 1000 - 8;
    const uint64_t at_least = 3 * budget_gap * (uint64_t) ROCKETNET_FRAME_STRIDE + client_gap * sizeof( struct rocketnet_msgx_socket_slot_t ) +
                              (uint64_t) workers * client_gap * sizeof( int );
    if ( plan_large - plan_small < at_least )
    {
        printf( "msgx plan: %llu bytes at 8 clients, %llu at 1000: %llu apart, at least %llu expected\n", (unsigned long long) plan_small,
                (unsigned long long) plan_large, (unsigned long long) ( plan_large - plan_small ), (unsigned long long) at_least );
    }
    check( plan_large - plan_small >= at_least );
}

REGISTER_UNIT_TEST( test_msgx_plan_follows_configured_clients );

#if ROCKETNET_DEV

// A counting allocator: every byte asked of it (the library's per-allocation
// overhead included, as the plan counts it) and the allocations still live.
static uint64_t msgx_sizing_bytes = 0;
static int msgx_sizing_live = 0;

static void * msgx_sizing_malloc( void * context, size_t bytes )
{
    (void) context;
    msgx_sizing_bytes += bytes;
    msgx_sizing_live++;
    void * base = malloc( bytes + 128 );
    if ( !base )
    {
        return NULL;
    }
    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    return (void *) address;
}

static void msgx_sizing_free( void * context, void * pointer )
{
    (void) context;
    msgx_sizing_live--;
    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    free( base );
}

// the bytes create allocates before its sockets and threads, counted
static uint64_t msgx_sizing_allocated( int workers, int max_clients )
{
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( NULL, msgx_sizing_malloc, msgx_sizing_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );
    msgx_sizing_bytes = 0;
    msgx_sizing_live = 0;
    check( rocketnet_server_msgx_test_allocate( workers, max_clients ) );
    const uint64_t allocated = msgx_sizing_bytes;
    check( msgx_sizing_live == 0 );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    return allocated;
}

// What create allocates before its sockets and threads is the plan less the
// threads and wakes (which this case does not create: the plan counts them by
// the platform's own byte helpers, and this case subtracts the same), at 1, 8
// and 1000 clients: the transport, the process view, the receive banks, the
// socket table and the worker buckets come to exactly what the plan counts,
// and grow with the count (the table and buckets between 1 and 8 while the
// receive budget keeps its 512-client floor, everything up to 1000).
static void test_msgx_allocation_is_the_plan()
{
    const int workers = ROCKETNET_MSGX_DEFAULT_WORKERS;
    const int counts[] = { 1, 8, 1000 };
    uint64_t last = 0;
    for ( int c = 0; c < 3; c++ )
    {
        const uint64_t plan = rocketnet_server_msgx_allocation_bytes( workers, counts[c] );
        const uint64_t threads = (uint64_t) ( workers + 1 ) * rocketnet_platform_thread_allocation_bytes() +
                                 (uint64_t) workers * rocketnet_platform_wake_allocation_bytes();
        const uint64_t allocated = msgx_sizing_allocated( workers, counts[c] );
        if ( allocated + threads != plan )
        {
            printf( "msgx at %d clients: allocated %llu, threads and wakes %llu, plan %llu\n", counts[c], (unsigned long long) allocated,
                    (unsigned long long) threads, (unsigned long long) plan );
        }
        check( allocated + threads == plan );
        check( allocated > last );
        last = allocated;
    }
}

REGISTER_UNIT_TEST( test_msgx_allocation_is_the_plan );

#endif // #if ROCKETNET_DEV

#endif // #if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

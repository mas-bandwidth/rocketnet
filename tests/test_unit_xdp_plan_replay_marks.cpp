/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server memory plan counts every allocation create makes: a transport
// that takes replay marks (xdp) also gets the per-slot replay records
// (rocketnet_server_replay_marks_create), and its plan carries their bytes
// beside the protocol tables. The plan is arithmetic, and the records' term is
// checked against the bytes their create allocates: no socket, no clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_server_replay_marks.h"
#include "loopback_hub.h"

#include <stdlib.h>

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

// The term itself, measured: the bytes the replay records' create asks the
// installed allocator for (each request is the caller's bytes plus
// rocketnet_allocation_overhead, sdk/rocketnet.cpp rocketnet_malloc) are the
// bytes the plan adds for them, at one slot and at the default max_clients.
// The plan's term is read against what create allocates, not against itself.

struct xdp_plan_marks_allocator_t
{
    uint64_t bytes;
    int live;
};

static void * xdp_plan_marks_allocate( void * context, size_t bytes )
{
    xdp_plan_marks_allocator_t * state = (xdp_plan_marks_allocator_t *) context;
    void * pointer = malloc( bytes );
    if ( pointer )
    {
        state->bytes += bytes;
        state->live++;
    }
    return pointer;
}

static void xdp_plan_marks_release( void * context, void * pointer )
{
    xdp_plan_marks_allocator_t * state = (xdp_plan_marks_allocator_t *) context;
    state->live--;
    free( pointer );
}

static void test_xdp_memory_plan_replay_marks_term_is_what_create_allocates()
{
    xdp_plan_marks_allocator_t state = { 0, 0 };

    // the worker starts initialized: terminate, install the counting
    // allocator, and initialize with its state as the context
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( &state, xdp_plan_marks_allocate, xdp_plan_marks_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    const int counts[] = { 1, config.max_clients };
    for ( int i = 0; i < 2; ++i )
    {
        state.bytes = 0;
        struct rocketnet_server_replay_marks_t * marks = rocketnet_server_replay_marks_create( &state, counts[i] );
        check( marks != NULL );
        check( state.bytes > 0 );
        check( state.bytes == rocketnet_server_replay_marks_allocation_bytes( counts[i] ) );
        rocketnet_server_replay_marks_destroy( marks );
        check( state.live == 0 );
    }

    // a clean term, then the default allocator back for the worker's own term
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_xdp_memory_plan_replay_marks_term_is_what_create_allocates );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

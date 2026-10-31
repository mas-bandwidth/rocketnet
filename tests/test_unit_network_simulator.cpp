/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The network simulator's memory and refusals (sdk/rocketnet_network_simulator.cpp):
// create allocates exactly rocketnet_network_simulator_memory_bytes, refuses
// what that plan refuses before any allocation, and frees everything when an
// allocation fails; a reverse window the allocator refuses leaves the patterns
// off. No socket, no thread, no clock: time is an argument.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_wire.h"
#include "rocketnet_network_simulator.h"

#include <limits.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

// a counting allocator: live bytes and calls, and one call it refuses
// (refuse_call, -1 for none). A header in front of each block keeps its size.

enum
{
    SIMULATOR_COUNT_HEADER = 16
};

struct simulator_allocator_t
{
    uint64_t live_bytes;
    int calls;
    int refuse_call;
};

static simulator_allocator_t simulator_allocator;

static void * simulator_count_malloc( void * context, size_t bytes )
{
    const int call = simulator_allocator.calls++;
    if ( call == simulator_allocator.refuse_call )
        return NULL;

    uint8_t * block = (uint8_t *) rocketnet_default_malloc_function( context, bytes + SIMULATOR_COUNT_HEADER );
    if ( !block )
        return NULL;

    memcpy( block, &bytes, sizeof( bytes ) );
    simulator_allocator.live_bytes += bytes;

    return block + SIMULATOR_COUNT_HEADER;
}

static void simulator_count_free( void * context, void * memory )
{
    if ( !memory )
        return;

    uint8_t * block = (uint8_t *) memory - SIMULATOR_COUNT_HEADER;
    size_t bytes;
    memcpy( &bytes, block, sizeof( bytes ) );
    simulator_allocator.live_bytes -= bytes;
    rocketnet_default_free_function( context, block );
}

static void simulator_allocator_install()
{
    memset( &simulator_allocator, 0, sizeof( simulator_allocator ) );
    simulator_allocator.refuse_call = -1;

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, simulator_count_malloc, simulator_count_free ) == ROCKETNET_STATUS_OK );
}

static void simulator_allocator_remove()
{
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static void simulator_quiet_assert( const char *, const char *, const char *, int ) {}

// create takes exactly its plan, and a reverse window adds exactly the plan's
// window term; destroy returns every byte

static void test_network_simulator_create_allocates_its_plan()
{
    simulator_allocator_install();

    const int capacities[] = { 1, 63, 64, 65, 130, 4096, 4097 };
    for ( int i = 0; i < (int) ( sizeof( capacities ) / sizeof( capacities[0] ) ); i++ )
    {
        const int capacity = capacities[i];

        struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, capacity, 1 );
        check( simulator != NULL );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 0 ) );

        check( rocketnet_network_simulator_patterns( simulator, 0, 64 ) );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 64 ) );

        check( rocketnet_network_simulator_patterns( simulator, 0, 2 ) );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 2 ) );

        check( rocketnet_network_simulator_patterns( simulator, 0, 0 ) );
        check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( capacity, 0 ) );

        rocketnet_network_simulator_destroy( simulator );
        check( simulator_allocator.live_bytes == 0 );
    }

    // a window under 2 is no pattern and holds no memory

    check( rocketnet_network_simulator_memory_bytes( 64, 1 ) == rocketnet_network_simulator_memory_bytes( 64, 0 ) );

    simulator_allocator_remove();
}

// a capacity the plan refuses is refused by create before any allocation

static void test_network_simulator_create_refuses_what_its_plan_refuses()
{
    simulator_allocator_install();
    rocketnet_assert_function( simulator_quiet_assert );

    const int refused[] = { 0, -1, INT_MIN, ( 1 << 30 ) + 1, INT_MAX };
    for ( int i = 0; i < (int) ( sizeof( refused ) / sizeof( refused[0] ) ); i++ )
    {
        check( rocketnet_network_simulator_memory_bytes( refused[i], 0 ) == 0 );
        check( rocketnet_network_simulator_create( NULL, refused[i], 1 ) == NULL );
        check( simulator_allocator.calls == 0 );
    }

    rocketnet_assert_function( NULL );

    check( rocketnet_network_simulator_memory_bytes( 1 << 30, 0 ) > 0 );

    simulator_allocator_remove();
}

// each of create's allocations refused in turn: create returns NULL and frees
// what it took

static void test_network_simulator_create_frees_on_a_refused_allocation()
{
    simulator_allocator_install();

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 130, 1 );
    check( simulator != NULL );
    const int allocations = simulator_allocator.calls;
    rocketnet_network_simulator_destroy( simulator );
    check( allocations > 1 );

    for ( int refuse = 0; refuse < allocations; refuse++ )
    {
        simulator_allocator.calls = 0;
        simulator_allocator.refuse_call = refuse;
        check( rocketnet_network_simulator_create( NULL, 130, 1 ) == NULL );
        check( simulator_allocator.live_bytes == 0 );
    }

    simulator_allocator_remove();
}

static void simulator_addresses( struct rocketnet_address_t * from, struct rocketnet_address_t * to )
{
    check( rocketnet_address_parse( from, "198.51.100.10:10000" ) );
    check( rocketnet_address_parse( to, "198.51.100.20:20000" ) );
}

// a reverse window the allocator refuses leaves both patterns off and the
// wire passes packets through in send order

static void test_network_simulator_refused_window_leaves_patterns_off()
{
    simulator_allocator_install();

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 16, 1 );
    check( simulator != NULL );

    simulator_allocator.refuse_call = simulator_allocator.calls;
    check( !rocketnet_network_simulator_patterns( simulator, 7, 4 ) );
    check( !rocketnet_network_simulator_patterns_active( simulator ) );
    check( simulator_allocator.live_bytes == rocketnet_network_simulator_memory_bytes( 16, 0 ) );

    struct rocketnet_address_t from, to;
    simulator_addresses( &from, &to );

    // the seventh send is past the refused every-7th pattern

    for ( int i = 0; i < 8; i++ )
    {
        const uint8_t byte = (uint8_t) i;
        rocketnet_network_simulator_send( simulator, &from, &to, &byte, 1, 10.0 );
    }

    for ( int i = 0; i < 8; i++ )
    {
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        struct rocketnet_address_t received_from;
        check( rocketnet_network_simulator_receive( simulator, &to, 10.0, &received_from, packet ) == 1 );
        check( packet[0] == (uint8_t) i );
    }

    rocketnet_network_simulator_destroy( simulator );
    simulator_allocator_remove();
}

REGISTER_UNIT_TEST( test_network_simulator_create_allocates_its_plan );
REGISTER_UNIT_TEST( test_network_simulator_create_refuses_what_its_plan_refuses );
REGISTER_UNIT_TEST( test_network_simulator_create_frees_on_a_refused_allocation );
REGISTER_UNIT_TEST( test_network_simulator_refused_window_leaves_patterns_off );

#endif // #if ROCKETNET_DEV

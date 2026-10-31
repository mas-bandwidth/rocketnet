/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// rocketnet_malloc and rocketnet_free take no lock: neither takes the
// lifecycle lock, on success or on refusal, whatever allocator is installed
// (proofs/tla/RNAllocatorInstall.tla). Install and term still see every live
// allocation. No real time, no sockets, no threads.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"

#include <stdint.h>
#include <stddef.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

static bool g_refuse_next;

static void * no_lock_malloc( void * context, size_t bytes )
{
    if ( g_refuse_next )
    {
        g_refuse_next = false;
        return NULL;
    }

    return rocketnet_default_malloc_function( context, bytes );
}

static void no_lock_free( void * context, void * p )
{
    rocketnet_default_free_function( context, p );
}

static void test_allocator_malloc_and_free_take_no_lock()
{
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, no_lock_malloc, no_lock_free ) == ROCKETNET_STATUS_OK );

    const uint64_t locks_before = rocketnet_lifecycle_locks_for_test();

    void * blocks[16];
    for ( int i = 0; i < 16; i++ )
    {
        blocks[i] = rocketnet_malloc( NULL, (size_t) ( 1 + i * 997 ) );
        check( blocks[i] != NULL );
    }

    for ( int i = 0; i < 16; i++ )
        rocketnet_free( NULL, blocks[i] );

    // a refusal by the installed allocator, and a size past the header overhead

    g_refuse_next = true;
    check( rocketnet_malloc( NULL, 64 ) == NULL );
    check( rocketnet_malloc( NULL, SIZE_MAX ) == NULL );

    check( rocketnet_lifecycle_locks_for_test() == locks_before );

    // install and term still see a live allocation

    void * live = rocketnet_malloc( NULL, 64 );
    check( live != NULL );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_LIVE_ALLOCATIONS );
    rocketnet_free( NULL, live );

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_allocator_malloc_and_free_take_no_lock );

#endif // #if ROCKETNET_DEV

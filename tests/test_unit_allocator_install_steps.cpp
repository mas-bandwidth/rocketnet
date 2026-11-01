/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// An allocator install against one rocketnet_malloc call, step by step, on
// one thread (proofs/tla/RNAllocatorInstall.tla). Install runs from inside
// the call at a chosen step: once the call is counted and before it reads
// the pair (the call seam, CallCount then InstallBegin), and while the call
// is inside the allocator (CallAllocate then InstallBegin). Each time install
// refuses with LIVE_ALLOCATIONS, the pair stays installed, and the call
// allocates and its block releases through that pair, never the other:
// CountCoversCalls, InstallOnlyWhenIdle and ReleasedThroughItsPair. A call
// that meets the installing sentinel needs install's publish to come from
// another thread, so that step is driven in tests/test_lifecycle.cpp. No real
// time, no sockets, no threads.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

struct install_steps_pair_t
{
    uint64_t tag;
    int allocates;
    int releases;
    int foreign_releases;
    int live;
};

static install_steps_pair_t g_steps_pairs[2];

// what runs inside the call, once: an install of the other pair, and its status
static bool g_steps_install_when_counted;
static bool g_steps_install_when_allocating;
static int g_steps_install_status;

static int steps_install_b();

static void * steps_pair_allocate( install_steps_pair_t & pair, size_t bytes )
{
    ++pair.allocates;
    if ( g_steps_install_when_allocating )
    {
        g_steps_install_when_allocating = false;
        g_steps_install_status = steps_install_b();
    }

    uint8_t * base = (uint8_t *) rocketnet_default_malloc_function( NULL, bytes + 16 );
    if ( !base )
        return NULL;

    memcpy( base, &pair.tag, sizeof( pair.tag ) );
    ++pair.live;
    return base + 16;
}

static void steps_pair_release( install_steps_pair_t & pair, void * pointer )
{
    uint8_t * base = (uint8_t *) pointer - 16;
    uint64_t tag = 0;
    memcpy( &tag, base, sizeof( tag ) );
    if ( tag != pair.tag )
        ++pair.foreign_releases;
    ++pair.releases;
    --pair.live;
    rocketnet_default_free_function( NULL, base );
}

static void * steps_allocate_a( void *, size_t bytes ) { return steps_pair_allocate( g_steps_pairs[0], bytes ); }
static void steps_release_a( void *, void * pointer ) { steps_pair_release( g_steps_pairs[0], pointer ); }
static void * steps_allocate_b( void *, size_t bytes ) { return steps_pair_allocate( g_steps_pairs[1], bytes ); }
static void steps_release_b( void *, void * pointer ) { steps_pair_release( g_steps_pairs[1], pointer ); }

static int steps_install_b()
{
    return rocketnet_allocator_ex( steps_allocate_b, steps_release_b );
}

static void steps_call_hook( int step )
{
    if ( step == ROCKETNET_ALLOCATOR_STEP_COUNTED && g_steps_install_when_counted )
    {
        g_steps_install_when_counted = false;
        g_steps_install_status = steps_install_b();
    }
}

// uninitialized with pair a installed and nothing live
static void steps_begin()
{
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    rocketnet_quiet( true );

    memset( g_steps_pairs, 0, sizeof( g_steps_pairs ) );
    g_steps_pairs[0].tag = 0xA110CA7EA11CEAAAULL;
    g_steps_pairs[1].tag = 0xA110CA7EB0B0BBBBULL;
    g_steps_install_status = ROCKETNET_TEST_STATUS_UNSET;
    check( rocketnet_allocator_ex( steps_allocate_a, steps_release_a ) == ROCKETNET_STATUS_OK );
}

// the install refused, pair a kept: the call's block came from a and goes
// back through a; then, with nothing live, b installs and serves the next call
static void steps_check_refused_and_finish( void * block )
{
    check( g_steps_install_status == ROCKETNET_STATUS_LIVE_ALLOCATIONS );
    check( rocketnet_allocator_installed( steps_allocate_a, steps_release_a ) );
    check( block != NULL );
    check( g_steps_pairs[0].allocates == 1 && g_steps_pairs[0].live == 1 );
    check( g_steps_pairs[1].allocates == 0 );

    rocketnet_free( NULL, block );
    check( g_steps_pairs[0].releases == 1 && g_steps_pairs[0].live == 0 );
    check( g_steps_pairs[1].releases == 0 );

    check( steps_install_b() == ROCKETNET_STATUS_OK );
    void * next = rocketnet_malloc( NULL, 64 );
    check( next != NULL );
    check( g_steps_pairs[0].allocates == 1 && g_steps_pairs[1].allocates == 1 );
    rocketnet_free( NULL, next );
    check( g_steps_pairs[1].releases == 1 && g_steps_pairs[1].live == 0 );

    check( g_steps_pairs[0].foreign_releases == 0 && g_steps_pairs[1].foreign_releases == 0 );

    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( false );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// counted, pair not yet read: install sees the count and refuses
// (CountCoversCalls); the call then reads the pair install put back

static void test_allocator_install_refused_while_a_call_is_counted()
{
    steps_begin();

    rocketnet_allocator_set_call_hook_for_test( steps_call_hook );
    g_steps_install_when_counted = true;
    void * block = rocketnet_malloc( NULL, 64 );
    rocketnet_allocator_set_call_hook_for_test( NULL );
    check( !g_steps_install_when_counted );

    steps_check_refused_and_finish( block );
}

REGISTER_UNIT_TEST( test_allocator_install_refused_while_a_call_is_counted );

// inside the allocator: install sees the call in flight and refuses
// (InstallOnlyWhenIdle); the block's header keeps the pair that allocated it

static void test_allocator_install_refused_while_a_call_allocates()
{
    steps_begin();

    g_steps_install_when_allocating = true;
    void * block = rocketnet_malloc( NULL, 64 );
    check( !g_steps_install_when_allocating );

    steps_check_refused_and_finish( block );
}

REGISTER_UNIT_TEST( test_allocator_install_refused_while_a_call_allocates );

#endif // #if ROCKETNET_DEV

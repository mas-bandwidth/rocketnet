/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every block the SDK holds is counted: through the allocator rocketnet_init
// is given, or in the direct allocation count (sdk/rocketnet_memory.h) for the
// regions and rings a transport takes from the OS itself. A create/destroy
// cycle returns both to their start. The unit tier reaches the fixtures that
// build a transport without a socket or a kernel ring: the io_uring fake on
// Linux and the RIO overflow fixture on Windows; the real creates (AF_XDP UMEM
// and rings, io_uring rings, the RIO region) open sockets, so the soak's
// --assert-flat covers them. No real time, no sockets, no threads.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_server_uring.h"
#if defined( _WIN32 )
#include "../sdk/rocketnet_server_rio.h"
#endif // #if defined( _WIN32 )
#if defined( __linux__ )
#include "../sdk/rocketnet_uring_codec.h"
#endif // #if defined( __linux__ )

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )
#define DIRECT_COUNT_URING_CASE 1
#else
#define DIRECT_COUNT_URING_CASE 0
#endif // #if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )
#if defined( _WIN32 ) && ROCKETNET_DEV
#define DIRECT_COUNT_RIO_CASE 1
#else
#define DIRECT_COUNT_RIO_CASE 0
#endif // #if defined( _WIN32 ) && ROCKETNET_DEV

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#if DIRECT_COUNT_URING_CASE || DIRECT_COUNT_RIO_CASE

enum
{
    DIRECT_COUNT_HEADER = 16
};
static size_t g_direct_live_bytes;
static size_t g_direct_allocation_calls;

static void * direct_count_malloc( void * context, size_t bytes )
{
    ++g_direct_allocation_calls;

    uint8_t * block = (uint8_t *) rocketnet_default_malloc_function( context, bytes + DIRECT_COUNT_HEADER );
    if ( !block )
        return NULL;
    memcpy( block, &bytes, sizeof( bytes ) );
    g_direct_live_bytes += bytes;

    return block + DIRECT_COUNT_HEADER;
}

static void direct_count_free( void * context, void * memory )
{
    if ( !memory )
        return;

    uint8_t * block = (uint8_t *) memory - DIRECT_COUNT_HEADER;
    size_t bytes;
    memcpy( &bytes, block, sizeof( bytes ) );
    g_direct_live_bytes -= bytes;
    rocketnet_default_free_function( context, block );
}

static void direct_count_install()
{
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, direct_count_malloc, direct_count_free ) == ROCKETNET_STATUS_OK );
}

static void direct_count_uninstall()
{
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

#endif // #if DIRECT_COUNT_URING_CASE || DIRECT_COUNT_RIO_CASE

// the count itself: an add then its remove leaves it where it started

static void test_direct_allocation_count_returns_to_start()
{
    const uint64_t start = rocketnet_direct_allocation_bytes();
    rocketnet_direct_allocation_add( 4096 );
    rocketnet_direct_allocation_add( 61079552 );
    check( rocketnet_direct_allocation_bytes() == start + 4096 + 61079552 );
    rocketnet_direct_allocation_remove( 61079552 );
    rocketnet_direct_allocation_remove( 4096 );
    check( rocketnet_direct_allocation_bytes() == start );
}

REGISTER_UNIT_TEST( test_direct_allocation_count_returns_to_start );

#if DIRECT_COUNT_URING_CASE

// red first: the fake took its transport and queues with new, so the
// allocator saw none of it. Now both blocks are the allocator's, held while
// the fake lives and returned at its destroy; nothing goes to the direct count.

static void test_io_uring_fake_create_destroy_counted()
{
    direct_count_install();

    const size_t live_before = g_direct_live_bytes;
    const size_t calls_before = g_direct_allocation_calls;
    const uint64_t direct_before = rocketnet_direct_allocation_bytes();

    for ( int cycle = 0; cycle < 3; cycle++ )
    {
        struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create_fake( 2 );
        check( uring != NULL );
        if ( !uring )
            break;
        check( g_direct_live_bytes > live_before );
        check( g_direct_allocation_calls == calls_before + 2 * (size_t) ( cycle + 1 ) );
        rocketnet_server_uring_test_destroy_fake( uring );
        check( g_direct_live_bytes == live_before );
        check( rocketnet_direct_allocation_bytes() == direct_before );
    }

    direct_count_uninstall();
}

REGISTER_UNIT_TEST( test_io_uring_fake_create_destroy_counted );

#endif // #if DIRECT_COUNT_URING_CASE

#if DIRECT_COUNT_RIO_CASE

bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                 int * repost_calls, int * pending_receives, int * guard_hit );

// red first: the fixture took its transport, region, banks and slot bytes
// with calloc, so the allocator saw none of them. Now each is the
// allocator's, and the fixture's free returns every byte.

static void test_rio_fixture_create_destroy_counted()
{
    direct_count_install();

    const size_t live_before = g_direct_live_bytes;
    const size_t calls_before = g_direct_allocation_calls;
    const uint64_t direct_before = rocketnet_direct_allocation_bytes();

    uint64_t counters[2 * ROCKETNET_RIO_NUM_COUNTERS] = {};
    int packet_counts[2] = { 0, 0 };
    int repost_calls = 0;
    int pending_receives = 0;
    int guard_hit = 0;
    check( rocketnet_server_rio_test_receive_overflow( counters, packet_counts, &repost_calls, &pending_receives, &guard_hit ) );
    check( g_direct_allocation_calls >= calls_before + 4 );
    check( g_direct_live_bytes == live_before );
    check( rocketnet_direct_allocation_bytes() == direct_before );

    direct_count_uninstall();
}

REGISTER_UNIT_TEST( test_rio_fixture_create_destroy_counted );

#endif // #if DIRECT_COUNT_RIO_CASE

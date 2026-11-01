/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_server_socket.h"
#include "test_unit_registry.h"

#if ROCKETNET_DEV

// The fixture calls the actual transport send_packets twice with a fake
// staged backend carrying the given flags, after one commit: a transport that
// declares ROCKETNET_TRANSPORT_REFLUSHES flushes at both boundaries, any other
// staged transport only for the commit.
int rocketnet_server_transport_test_reflush_flags( unsigned int flags );
int rocketnet_server_transport_test_reflush( int transport_kind );

static void test_staged_transport_reflush_follows_the_flag()
{
    rocketnet_unit_check( rocketnet_server_transport_test_reflush_flags( ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_REFLUSHES ) == 2 );
    rocketnet_unit_check( rocketnet_server_transport_test_reflush_flags( ROCKETNET_TRANSPORT_STAGED ) == 1 );
}

REGISTER_UNIT_TEST( test_staged_transport_reflush_follows_the_flag );

// XDP may retain its producer generation, gso retains its producer on a
// contended or full handoff and io_uring keeps its index while sends wait for
// CQEs: each declares the flag, so the next send boundary retries that work
// with no new commit. A synchronous staged transport (iocp) does not.

#if defined( __linux__ )
static void test_linux_staged_transports_declare_reflush()
{
#if ROCKETNET_XDP
    rocketnet_unit_check( rocketnet_server_transport_test_reflush( ROCKETNET_TRANSPORT_XDP ) == 2 );
#endif // #if ROCKETNET_XDP
    rocketnet_unit_check( rocketnet_server_transport_test_reflush( ROCKETNET_TRANSPORT_GSO ) == 2 );
    rocketnet_unit_check( rocketnet_server_transport_test_reflush( ROCKETNET_TRANSPORT_IO_URING ) == 2 );
}

REGISTER_UNIT_TEST( test_linux_staged_transports_declare_reflush );
#endif // #if defined( __linux__ )

#if defined( _WIN32 )
static void test_windows_staged_transports_flush_once_per_commit()
{
    rocketnet_unit_check( rocketnet_server_transport_test_reflush( ROCKETNET_TRANSPORT_IOCP ) == 1 );
    rocketnet_unit_check( rocketnet_server_transport_test_reflush( ROCKETNET_TRANSPORT_RIO ) == 1 );
}

REGISTER_UNIT_TEST( test_windows_staged_transports_flush_once_per_commit );
#endif // #if defined( _WIN32 )

#endif // #if ROCKETNET_DEV

// the AF_XDP code is Linux only, so its tests are built there only
#if defined( __linux__ )

#if ROCKETNET_DEV

int rocketnet_server_transport_test_staged_simulator_flush();
static void test_xdp_transport_send_packets_flushes_simulator_window()
{
    rocketnet_unit_check( rocketnet_server_transport_test_staged_simulator_flush() == 1 );
}

REGISTER_UNIT_TEST( test_xdp_transport_send_packets_flushes_simulator_window );

#endif // #if ROCKETNET_DEV

// The helper returns each in-range packet index and refuses and counts every
// claim beyond the queue limit. Resetting the counters restores those results;
// this checks claim accounting only, without invoking send or flush code.
static void test_xdp_transport_send_claim_counts_each_refusal()
{
    const int limit = 1024;
    std::atomic<int> num_packets( 0 );
    std::atomic<uint64_t> refused( 0 );

    // the first limit claims take an index of their own, the next five are
    // refused and counted

    for ( int i = 0; i < limit + 5; ++i )
    {
        const int index = rocketnet_xdp_send_claim( num_packets, limit, refused );
        if ( i < limit )
            rocketnet_unit_check( index == i );
        else
            rocketnet_unit_check( index == -1 );
    }

    rocketnet_unit_check( refused.load( std::memory_order_relaxed ) == 5 );

    // a queue that starts over claims from zero again and counts again

    num_packets.store( 0, std::memory_order_relaxed );
    refused.store( 0, std::memory_order_relaxed );

    for ( int i = 0; i < limit + 5; ++i )
    {
        const int index = rocketnet_xdp_send_claim( num_packets, limit, refused );
        if ( i < limit )
            rocketnet_unit_check( index == i );
        else
            rocketnet_unit_check( index == -1 );
    }

    rocketnet_unit_check( refused.load( std::memory_order_relaxed ) == 5 );
}

REGISTER_UNIT_TEST( test_xdp_transport_send_claim_counts_each_refusal );

#endif // #if defined(__linux__)

/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_server_socket.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_config.h"
#include "loopback_hub.h"
#include "test_unit_registry.h"
#include "unit_shutdown.h"

#include <errno.h>
#include <string.h>

#if ROCKETNET_DEV

// The stop drain's view of one XDP queue. A queue whose worker reported a
// hard TX wakeup error (ENETDOWN) can never drain its ring, even though its
// producer buffer still holds frames; the drain must release it so
// rocketnet_server_stop reaches rocketnet_server_stopped. These cases run the
// real transport stop over a fake staged io whose send_idle is the XDP seam:
// no socket, thread, clock or kernel resource is involved.

static struct
{
    const rocketnet_server_transport_io_operations_t * real_operations;
    void * real_instance;
    int hard_error;
    bool buffer_empty;
    bool handoff_idle;
    int stops;
} xdp_stop_fixture;

static rocketnet_server_transport_io_operations_t xdp_stop_operations;

static void xdp_stop_fixture_destroy( void * )
{
    xdp_stop_fixture.real_operations->destroy( xdp_stop_fixture.real_instance );
}

static void xdp_stop_fixture_stop( void * )
{
    xdp_stop_fixture.stops++;
    xdp_stop_fixture.real_operations->stop( xdp_stop_fixture.real_instance );
}

static bool xdp_stop_fixture_stopped( void * )
{
    return xdp_stop_fixture.real_operations->stopped( xdp_stop_fixture.real_instance );
}

static void xdp_stop_fixture_flush( void * ) {}

static bool xdp_stop_fixture_send_idle( void * )
{
    return rocketnet_xdp_send_queue_released( xdp_stop_fixture.hard_error, xdp_stop_fixture.buffer_empty, xdp_stop_fixture.handoff_idle );
}

// Stop one server over the fake staged io with the given queue state. Returns
// false only when the case's own cleanup failed; stopped_after is the answer
// after the stop call and `polls` completion polls.
static bool xdp_stop_scenario( int hard_error, bool buffer_empty, bool handoff_idle, int polls, bool * stopped_after )
{
    xdp_stop_fixture.real_operations = NULL;
    xdp_stop_fixture.real_instance = NULL;
    xdp_stop_fixture.hard_error = hard_error;
    xdp_stop_fixture.buffer_empty = buffer_empty;
    xdp_stop_fixture.handoff_idle = handoff_idle;
    xdp_stop_fixture.stops = 0;

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config_current( &config );
    config.max_clients = 1;
    config.protocol_id = 1;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );

    int create_status = -1;
    rocketnet_server_transport_t * server =
        rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    rocketnet_unit_check( server != NULL );

    memset( &xdp_stop_operations, 0, sizeof( xdp_stop_operations ) );
    xdp_stop_operations.flags = ROCKETNET_TRANSPORT_STAGED;
    xdp_stop_operations.destroy = xdp_stop_fixture_destroy;
    xdp_stop_operations.stop = xdp_stop_fixture_stop;
    xdp_stop_operations.stopped = xdp_stop_fixture_stopped;
    xdp_stop_operations.flush = xdp_stop_fixture_flush;
    xdp_stop_operations.send_idle = xdp_stop_fixture_send_idle;

    const rocketnet_server_transport_io_operations_t * real_operations = &xdp_stop_operations;
    void * real_instance = &xdp_stop_fixture;
    rocketnet_server_transport_swap_io_for_test( server, &real_operations, &real_instance );
    xdp_stop_fixture.real_operations = real_operations;
    xdp_stop_fixture.real_instance = real_instance;

    rocketnet_server_transport_stop( server );

    bool stopped = rocketnet_server_transport_stopped( server );
    for ( int poll = 0; !stopped && poll < polls; ++poll )
        stopped = rocketnet_server_transport_stopped( server );

    *stopped_after = stopped;

    // A waiting queue still finishes when its work is done, so restore this
    // transport to a stoppable state before destroy even for the not-stopped
    // case.

    if ( !stopped )
    {
        xdp_stop_fixture.buffer_empty = true;
        xdp_stop_fixture.handoff_idle = true;
        stopped = rocketnet_server_transport_stopped( server );
    }

    const bool clean = stopped;

    const rocketnet_server_transport_io_operations_t * fake_operations = &xdp_stop_operations;
    void * fake_instance = &xdp_stop_fixture;
    rocketnet_server_transport_swap_io_for_test( server, &fake_operations, &fake_instance );

    rocketnet_server_transport_destroy( server );

    return clean;
}

static void test_xdp_transport_stop_gives_up_on_hard_send_error()
{
    // A hard TX wakeup error releases the queue though its ring is still full.
    bool stopped = false;
    rocketnet_unit_check( xdp_stop_scenario( ENETDOWN, false, false, 0, &stopped ) );
    rocketnet_unit_check( stopped );
    rocketnet_unit_check( xdp_stop_fixture.stops == 1 );

    // A healthy queue with the same non-empty ring still waits.
    rocketnet_unit_check( xdp_stop_scenario( 0, false, true, 4, &stopped ) );
    rocketnet_unit_check( !stopped );
}

REGISTER_UNIT_TEST( test_xdp_transport_stop_gives_up_on_hard_send_error );

static void test_xdp_send_wakeup_hard_errno_and_logging_run()
{
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_hard_error( 0 ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_hard_error( EAGAIN ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_hard_error( ENOBUFS ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_hard_error( ENETDOWN ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_hard_error( ENXIO ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_hard_error( EBADF ) );

    // one log per contiguous run of one hard errno

    int last_error = 0;
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_report( ENETDOWN, last_error ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_report( ENETDOWN, last_error ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_report( ENXIO, last_error ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_report( EAGAIN, last_error ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_report( ENXIO, last_error ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_report( 0, last_error ) );
}

REGISTER_UNIT_TEST( test_xdp_send_wakeup_hard_errno_and_logging_run );

// The worker's accounting of failed TX wakeups, which the public SEND_FAILED
// counter sums. Back-pressure leaves the descriptors queued and loses
// nothing, so it is no failed send; a hard error is one, and is kept for the
// stop drain.
static void test_xdp_send_wakeup_back_pressure_is_not_a_send_failure()
{
    std::atomic<uint64_t> failures( 0 );
    std::atomic<int> hard_error( 0 );

    rocketnet_xdp_send_wakeup_failed( EAGAIN, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( EBUSY, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( ENOBUFS, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 0 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == 0 );

    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 1 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == ENETDOWN );

    // back-pressure after a hard error neither counts nor forgets it

    rocketnet_xdp_send_wakeup_failed( EAGAIN, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 1 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == ENETDOWN );

    rocketnet_xdp_send_wakeup_failed( ENXIO, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 2 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == ENXIO );
}

REGISTER_UNIT_TEST( test_xdp_send_wakeup_back_pressure_is_not_a_send_failure );

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
#if defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING
    rocketnet_unit_check( rocketnet_server_transport_test_reflush( ROCKETNET_TRANSPORT_IO_URING ) == 2 );
#else
    // Optional liburing is absent in the s390x guest. The registry must
    // expose no io_uring backend in that build, rather than promise reflush.
    rocketnet_unit_check( rocketnet_server_transport_test_reflush( ROCKETNET_TRANSPORT_IO_URING ) == -2 );
#endif // #if defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING
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
    std::atomic<uint64_t> num_packets( 0 );
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

// the counter type rocketnet_xdp_send_claim reserves from
template <typename T> struct xdp_send_claim_counter;
template <typename T> struct xdp_send_claim_counter<int ( * )( std::atomic<T> &, int, std::atomic<uint64_t> & )>
{
    typedef T type;
};

// A send buffer that has taken 2^32 - 1 claims without a flush refuses the
// next two: the claim count does not wrap back onto a slot it handed out.
static void test_xdp_transport_send_claim_does_not_wrap()
{
    typedef xdp_send_claim_counter<decltype( &rocketnet_xdp_send_claim )>::type count_t;
    std::atomic<count_t> num_packets( (count_t) 0xFFFFFFFFull );
    std::atomic<uint64_t> refused( 0 );

    rocketnet_unit_check( rocketnet_xdp_send_claim( num_packets, 1024, refused ) == -1 );
    rocketnet_unit_check( rocketnet_xdp_send_claim( num_packets, 1024, refused ) == -1 );
    rocketnet_unit_check( refused.load( std::memory_order_relaxed ) == 2 );
}

REGISTER_UNIT_TEST( test_xdp_transport_send_claim_does_not_wrap );

#endif // #if defined(__linux__)

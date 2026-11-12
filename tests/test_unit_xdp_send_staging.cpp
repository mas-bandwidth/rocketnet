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

//
// The TX ring cases add a fake ring and the worker's stall watch: every stop
// step's send_idle first runs passes_per_poll worker passes, in each of which
// the ring's consumer takes one descriptor every drain_every passes (0:
// never, a ring that has stopped draining), each pass takes pass_seconds of
// the fake clock, and the watch runs on the result
// (rocketnet_xdp_send_stall_pass, with a small bound), as the worker loop does.

static struct
{
    const rocketnet_server_transport_io_operations_t * real_operations;
    void * real_instance;
    int hard_error;
    bool buffer_empty;
    bool handoff_idle;
    int stops;

    bool draining;                      // begin_shutdown was called
    uint32_t producer;                  // the fake TX ring's indices
    uint32_t consumer;
    int drain_every;
    int passes_per_poll;
    double bound_seconds;
    double pass_seconds;
    double now;                         // the fake clock, advanced pass_seconds each pass
    int pass;
    rocketnet_xdp_send_stall_t stall;
    std::atomic<bool> stalled;
    std::atomic<uint64_t> abandoned;    // what the worker counts in send_wakeup_failures
    bool released_while_draining;       // send_idle gave up on a ring that was still moving
} xdp_stop_fixture;

static rocketnet_server_transport_io_operations_t xdp_stop_operations;

static void xdp_stop_fixture_begin_shutdown( void * )
{
    xdp_stop_fixture.draining = true;
}

static bool xdp_stop_fixture_worker_passes()
{
    bool pending = xdp_stop_fixture.consumer != xdp_stop_fixture.producer;
    for ( int i = 0; i < xdp_stop_fixture.passes_per_poll; i++ )
    {
        xdp_stop_fixture.pass++;
        xdp_stop_fixture.now += xdp_stop_fixture.pass_seconds;
        if ( xdp_stop_fixture.drain_every > 0 && xdp_stop_fixture.pass % xdp_stop_fixture.drain_every == 0 && pending )
        {
            xdp_stop_fixture.consumer++;
        }
        pending = xdp_stop_fixture.consumer != xdp_stop_fixture.producer;
        if ( rocketnet_xdp_send_stall_pass( xdp_stop_fixture.stall, xdp_stop_fixture.consumer, pending, xdp_stop_fixture.draining,
                                            xdp_stop_fixture.now, xdp_stop_fixture.bound_seconds, xdp_stop_fixture.stalled ) )
        {
            xdp_stop_fixture.abandoned.fetch_add( xdp_stop_fixture.producer - xdp_stop_fixture.consumer );
        }
    }
    return pending;
}

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
    const bool pending = xdp_stop_fixture_worker_passes();
    const bool released = rocketnet_xdp_send_queue_released( xdp_stop_fixture.hard_error, xdp_stop_fixture.stalled.load(), xdp_stop_fixture.buffer_empty,
                                                             xdp_stop_fixture.handoff_idle && !pending );
    if ( released && pending && xdp_stop_fixture.drain_every > 0 )
    {
        xdp_stop_fixture.released_while_draining = true;
    }
    return released;
}

// The fake TX ring the next scenario starts with: frames descriptors queued,
// the rest as in xdp_stop_fixture. No call: an empty ring and no passes.
static struct
{
    uint32_t frames;
    int drain_every;
    int passes_per_poll;
    double bound_seconds;
    double pass_seconds;
} xdp_stop_tx_next;

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

    xdp_stop_fixture.draining = false;
    xdp_stop_fixture.producer = xdp_stop_tx_next.frames;
    xdp_stop_fixture.consumer = 0;
    xdp_stop_fixture.drain_every = xdp_stop_tx_next.drain_every;
    xdp_stop_fixture.passes_per_poll = xdp_stop_tx_next.passes_per_poll;
    xdp_stop_fixture.bound_seconds = xdp_stop_tx_next.bound_seconds;
    xdp_stop_fixture.pass_seconds = xdp_stop_tx_next.pass_seconds;
    xdp_stop_fixture.now = 1000.0;
    xdp_stop_fixture.pass = 0;
    xdp_stop_fixture.stall = rocketnet_xdp_send_stall_t{ 0, false, 0.0 };
    xdp_stop_fixture.stalled.store( false );
    xdp_stop_fixture.abandoned.store( 0 );
    xdp_stop_fixture.released_while_draining = false;
    xdp_stop_tx_next = {};

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
        rocketnet_server_transport_create_current_internal( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    rocketnet_unit_check( server != NULL );

    memset( &xdp_stop_operations, 0, sizeof( xdp_stop_operations ) );
    xdp_stop_operations.flags = ROCKETNET_TRANSPORT_STAGED;
    xdp_stop_operations.destroy = xdp_stop_fixture_destroy;
    xdp_stop_operations.stop = xdp_stop_fixture_stop;
    xdp_stop_operations.stopped = xdp_stop_fixture_stopped;
    xdp_stop_operations.flush = xdp_stop_fixture_flush;
    xdp_stop_operations.send_idle = xdp_stop_fixture_send_idle;
    xdp_stop_operations.begin_shutdown = xdp_stop_fixture_begin_shutdown;

    const rocketnet_server_transport_io_operations_t * real_operations = &xdp_stop_operations;
    void * real_instance = &xdp_stop_fixture;
    rocketnet_server_transport_swap_io_for_test( server, &real_operations, &real_instance );
    xdp_stop_fixture.real_operations = real_operations;
    xdp_stop_fixture.real_instance = real_instance;

    rocketnet_server_transport_stop( server );

    bool stopped = rocketnet_server_transport_stopped( server );
    for ( int poll = 0; !stopped && poll < polls; ++poll )
    {
        stopped = rocketnet_server_transport_stopped( server );
    }

    *stopped_after = stopped;

    // A waiting queue still finishes when its work is done, so restore this
    // transport to a stoppable state before destroy even for the not-stopped
    // case.

    if ( !stopped )
    {
        xdp_stop_fixture.buffer_empty = true;
        xdp_stop_fixture.handoff_idle = true;
        xdp_stop_fixture.consumer = xdp_stop_fixture.producer;
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

// A TX ring whose consumer never moves, with no errno to say so (carrier lost
// on a zero-copy queue while the interface stays up): the stop gives up on
// the queue once the bound has passed with frames pending, and counts the ten
// frames it abandons. Bound 8 s, one second a pass, three passes a stop step:
// the timing starts at the stop's first pass; the stop call and the first
// stopped poll run six passes and wait; one more poll reaches the bound at
// the ninth pass.
static void test_xdp_transport_stop_gives_up_on_a_ring_that_never_drains()
{
    bool stopped = false;
    xdp_stop_tx_next = { 10, 0, 3, 8.0, 1.0 };
    rocketnet_unit_check( xdp_stop_scenario( 0, true, true, 0, &stopped ) );
    rocketnet_unit_check( !stopped );
    rocketnet_unit_check( xdp_stop_fixture.abandoned.load() == 0 );

    xdp_stop_tx_next = { 10, 0, 3, 8.0, 1.0 };
    rocketnet_unit_check( xdp_stop_scenario( 0, true, true, 1, &stopped ) );
    rocketnet_unit_check( stopped );
    rocketnet_unit_check( xdp_stop_fixture.stops == 1 );
    rocketnet_unit_check( xdp_stop_fixture.abandoned.load() == 10 );
    rocketnet_unit_check( xdp_stop_fixture.stalled.load() );
}

REGISTER_UNIT_TEST( test_xdp_transport_stop_gives_up_on_a_ring_that_never_drains );

// The production bound, with worker passes of 10 us each (a pass that is not
// on an isolated core, or a copy-mode poll): a ring that never drains is
// given up on within one second of worker time after the stop begins, and
// the ten frames it held are counted. 1000 passes a stop step, at most 110
// steps: 1.1 s.
static void test_xdp_transport_stop_bound_is_one_second()
{
    bool stopped = false;
    xdp_stop_tx_next = { 10, 0, 1000, ROCKETNET_XDP_SEND_STALL_SECONDS, 10e-6 };
    rocketnet_unit_check( xdp_stop_scenario( 0, true, true, 110, &stopped ) );
    rocketnet_unit_check( stopped );
    rocketnet_unit_check( xdp_stop_fixture.pass * 10e-6 <= 1.1 );
    rocketnet_unit_check( xdp_stop_fixture.abandoned.load() == 10 );
}

REGISTER_UNIT_TEST( test_xdp_transport_stop_bound_is_one_second );

// A TX ring that drains slowly, one descriptor every four passes against a
// bound of eight, is never given up on: the stop waits until the consumer has
// taken all ten, then completes with nothing abandoned.
static void test_xdp_transport_stop_waits_for_a_ring_that_drains_slowly()
{
    bool stopped = false;
    xdp_stop_tx_next = { 10, 4, 3, 8.0, 1.0 };
    rocketnet_unit_check( xdp_stop_scenario( 0, true, true, 5, &stopped ) );
    rocketnet_unit_check( !stopped );
    rocketnet_unit_check( !xdp_stop_fixture.released_while_draining );

    xdp_stop_tx_next = { 10, 4, 3, 8.0, 1.0 };
    rocketnet_unit_check( xdp_stop_scenario( 0, true, true, 20, &stopped ) );
    rocketnet_unit_check( stopped );
    rocketnet_unit_check( xdp_stop_fixture.consumer == 10 );
    rocketnet_unit_check( xdp_stop_fixture.abandoned.load() == 0 );
    rocketnet_unit_check( !xdp_stop_fixture.stalled.load() );
    rocketnet_unit_check( !xdp_stop_fixture.released_while_draining );
}

REGISTER_UNIT_TEST( test_xdp_transport_stop_waits_for_a_ring_that_drains_slowly );

// The watch itself: no stall outside a stop, however long the ring has not
// moved; during a stop the timing starts at the first pass, and the stall is
// reported once the bound has passed, once; the consumer moving, or nothing
// pending, clears it, and a new stall is a new report. Time is the argument.
static void test_xdp_send_stall_watch()
{
    rocketnet_xdp_send_stall_t stall = { 0, false, 0.0 };
    std::atomic<bool> stalled( false );

    for ( int i = 0; i < 20; i++ )
    {
        rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 5, true, false, 1000.0 + i, 1.0, stalled ) );
    }
    rocketnet_unit_check( !stalled.load() );

    rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 5, true, true, 1020.0, 1.0, stalled ) );
    rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 5, true, true, 1020.999, 1.0, stalled ) );
    rocketnet_unit_check( !stalled.load() );
    rocketnet_unit_check( rocketnet_xdp_send_stall_pass( stall, 5, true, true, 1021.0, 1.0, stalled ) );
    rocketnet_unit_check( stalled.load() );
    rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 5, true, true, 1030.0, 1.0, stalled ) );
    rocketnet_unit_check( stalled.load() );

    rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 6, true, true, 1030.5, 1.0, stalled ) );
    rocketnet_unit_check( !stalled.load() );
    rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 6, true, true, 1030.75, 1.0, stalled ) );
    rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 6, true, true, 1031.5, 1.0, stalled ) );
    rocketnet_unit_check( rocketnet_xdp_send_stall_pass( stall, 6, true, true, 1031.75, 1.0, stalled ) );

    rocketnet_unit_check( !rocketnet_xdp_send_stall_pass( stall, 6, false, true, 1032.0, 1.0, stalled ) );
    rocketnet_unit_check( !stalled.load() );
}

REGISTER_UNIT_TEST( test_xdp_send_stall_watch );

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

    // an errno that is neither back-pressure, a lost datagram nor hard (a
    // driver's EINVAL or EOPNOTSUPP) is unexpected: reported, and counted by
    // the worker, once per run

    rocketnet_unit_check( rocketnet_xdp_send_wakeup_unexpected( EINVAL ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_unexpected( EOPNOTSUPP ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_unexpected( 0 ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_unexpected( EAGAIN ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_unexpected( ENOBUFS ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_unexpected( EBUSY ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_unexpected( ENETDOWN ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_report( EINVAL, last_error ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_report( EINVAL, last_error ) );
    rocketnet_unit_check( !rocketnet_xdp_send_wakeup_report( EBUSY, last_error ) );
    rocketnet_unit_check( rocketnet_xdp_send_wakeup_report( EINVAL, last_error ) );
}

REGISTER_UNIT_TEST( test_xdp_send_wakeup_hard_errno_and_logging_run );

// The worker's accounting of failed TX wakeups, which the public SEND_FAILED
// counter sums in datagrams. Back-pressure (EAGAIN, ENOBUFS) leaves the
// descriptors queued and loses nothing. EBUSY is the kernel's answer for a
// frame the device dropped after it left the TX ring: one lost datagram each
// time. A hard error loses nothing by itself and repeats on every wakeup
// while it lasts, so it counts once per run and is kept for the stop drain.
static void test_xdp_send_wakeup_counts_lost_datagrams_and_a_hard_error_once()
{
    std::atomic<uint64_t> failures( 0 );
    std::atomic<int> hard_error( 0 );

    rocketnet_xdp_send_wakeup_failed( EAGAIN, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( ENOBUFS, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 0 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == 0 );

    // every EBUSY is a dropped datagram, and no hard error

    rocketnet_xdp_send_wakeup_failed( EBUSY, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( EBUSY, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 2 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == 0 );

    // an interface that stays down fails every wakeup: one count for the run

    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 3 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == ENETDOWN );

    // back-pressure after a hard error neither counts nor forgets it

    rocketnet_xdp_send_wakeup_failed( EAGAIN, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 3 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == ENETDOWN );

    // a different hard error is a new run

    rocketnet_xdp_send_wakeup_failed( ENXIO, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( ENXIO, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 4 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == ENXIO );
}

REGISTER_UNIT_TEST( test_xdp_send_wakeup_counts_lost_datagrams_and_a_hard_error_once );

// An interface that went down and came back up: the hard error the worker
// kept releases the queue at stop only until a wakeup succeeds again. After
// that the stop drain waits for the queue's ring as for any healthy queue,
// so its disconnect packets are sent. The failure count is not taken back,
// and the interface going down again is a new run that counts again.
static void test_xdp_send_wakeup_success_clears_the_hard_error()
{
    std::atomic<uint64_t> failures( 0 );
    std::atomic<int> hard_error( 0 );

    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_unit_check( rocketnet_xdp_send_queue_released( hard_error.load( std::memory_order_relaxed ), false, false, false ) );

    // back-pressure is no sign the interface is back

    rocketnet_xdp_send_wakeup_failed( EAGAIN, failures, hard_error );
    rocketnet_unit_check( rocketnet_xdp_send_queue_released( hard_error.load( std::memory_order_relaxed ), false, false, false ) );

    rocketnet_xdp_send_wakeup_succeeded( hard_error );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == 0 );
    rocketnet_unit_check( !rocketnet_xdp_send_queue_released( hard_error.load( std::memory_order_relaxed ), false, false, false ) );
    rocketnet_unit_check( rocketnet_xdp_send_queue_released( hard_error.load( std::memory_order_relaxed ), false, true, true ) );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 1 );

    // a success with no error kept changes nothing

    rocketnet_xdp_send_wakeup_succeeded( hard_error );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == 0 );

    // down again after the success: a second run, a second count

    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_xdp_send_wakeup_failed( ENETDOWN, failures, hard_error );
    rocketnet_unit_check( failures.load( std::memory_order_relaxed ) == 2 );
    rocketnet_unit_check( hard_error.load( std::memory_order_relaxed ) == ENETDOWN );
}

REGISTER_UNIT_TEST( test_xdp_send_wakeup_success_clears_the_hard_error );

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
        {
            rocketnet_unit_check( index == i );
        }
        else
        {
            rocketnet_unit_check( index == -1 );
        }
    }

    rocketnet_unit_check( refused.load( std::memory_order_relaxed ) == 5 );

    // a queue that starts over claims from zero again and counts again

    num_packets.store( 0, std::memory_order_relaxed );
    refused.store( 0, std::memory_order_relaxed );

    for ( int i = 0; i < limit + 5; ++i )
    {
        const int index = rocketnet_xdp_send_claim( num_packets, limit, refused );
        if ( i < limit )
        {
            rocketnet_unit_check( index == i );
        }
        else
        {
            rocketnet_unit_check( index == -1 );
        }
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

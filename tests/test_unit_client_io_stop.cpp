/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The stop of the client I/O core (sdk/rocketnet_client_io.h) against the
// fake client transport (tests/fake_client_driver.h): RNClientIo Stop, IoExit,
// Join and the invariant FreedOnlyAfterJoin. Single-threaded: no sockets, no
// real time.

#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "client_io_rig.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_message_object.h"
#include "rocketnet_test_seams.h"

#include <stdint.h>
#include <string.h>

struct client_io_stop_test_clock_t
{
    double now;
};

static double client_io_stop_test_time( void * context )
{
    return ( (client_io_stop_test_clock_t *) context )->now;
}

static void client_io_stop_test_open( rocketnet_fake_client_transport_io_t * fake,
                                      const rocketnet_client_transport_io_operations_t * operations, rocketnet_client_transport_io_t * transport )
{
    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( operations, fake, &config, &bind, transport ) );
}

// Stop with a handed-off ring: the I/O thread sends the ring in flight (both
// packets), then exits, in the same step. wake_calls 2: the hand-off wakes
// once, stop wakes again. RNClientIo Stop, IoExit; RNBufferHandoff WorkerExit.

static void test_client_io_stop_sends_in_flight_ring_first()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 5;
    rig->counts[0][1] = 7;

    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_stop_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9000 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 2, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( fake.wake_calls == 1 );

    rocketnet_client_io_stop( &io, 0, &to );
    rocketnet_unit_check( fake.wake_calls == 2 );
    rocketnet_unit_check( io.send_final == -1 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( io.stopped.load( std::memory_order_acquire ) );
    rocketnet_unit_check( fake.send_calls == 2 );
    rocketnet_unit_check( fake.num_sent == 2 );
    rocketnet_unit_check( fake.sent[0].bytes == 5 );
    rocketnet_unit_check( fake.sent[1].bytes == 7 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_stop_sends_in_flight_ring_first );

// Stop with committed slots in the Filling ring and two rings queued: stop
// takes no mutex and hands the Filling ring over with quit; the I/O thread
// sends both queued rings in order, then that one, then exits; nothing
// committed is lost. RNBufferHandoff StopSendsAll, ExitSentAll,
// MCRNBufferHandoffBrokenDropFinal.cfg.

static void test_client_io_stop_hands_over_the_filling_ring()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();

    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        rig->counts[ring][0] = 3;
        rocketnet_client_io_rig_slot( rig, ring, 0 )[0] = (uint8_t) ( 0xB0 + ring );
    }

    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_stop_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9050 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( io.send_fill == 2 );

    // the mutex is busy at stop: stop does not need it

    rocketnet_platform_mutex_acquire( &rig->mutex );
    rocketnet_client_io_stop( &io, 1, &to );
    rocketnet_platform_mutex_release( &rig->mutex );
    rocketnet_unit_check( io.send_final == 2 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.num_sent == 3 );
    rocketnet_unit_check( fake.sent[0].data[0] == 0xB0 );
    rocketnet_unit_check( fake.sent[1].data[0] == 0xB1 );
    rocketnet_unit_check( fake.sent[2].data[0] == 0xB2 );
    rocketnet_unit_check( rocketnet_address_equal( &fake.sent[2].address, &to ) );

    // a step after the exit sends nothing again

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.num_sent == 3 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_stop_hands_over_the_filling_ring );

// Stop with nothing handed off and a datagram waiting: the step exits at once,
// sends nothing and does not wait; it does not receive the datagram into a
// buffer or publish one, but the exit's discard reads it with the single
// receive (this backend has no receive_batch) and counts it, the second
// receive finding the socket empty. This backend has no close. RNClientIo
// IoExit (no publish, no wait); RNClientIoShutdown WorkerExit.

static void test_client_io_stop_exits_with_nothing_new()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_stop_test_time, &clock );

    uint8_t datagram[16];
    for ( int i = 0; i < 16; i++ )
    {
        datagram[i] = (uint8_t) ( 0x40 + i );
    }

    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9100 );
    rocketnet_fake_client_push( &fake, &from, datagram, 16 );

    rocketnet_client_io_stop( &io, 0, &from );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.send_calls == 0 );
    rocketnet_unit_check( fake.receive_calls == 2 );
    rocketnet_unit_check( fake.inbound_count == 0 );
    rocketnet_unit_check( io.receive_discarded_at_stop.load( std::memory_order_acquire ) == 1 );
    rocketnet_unit_check( fake.wait_calls == 0 );
    rocketnet_unit_check( fake.close_calls == 0 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_stop_exits_with_nothing_new );

// Datagrams queued on the socket when the worker exits are read at the exit,
// each counted once as discarded at stop, and the transport's handles are
// closed before stopped is stored: no datagram delivered to the socket is
// left unread and uncounted. A batch backend is read by receive_batch, 70
// datagrams over two batches. A step after the exit reads and closes nothing
// again. RNClientIoShutdown WorkerExit.

static void test_client_io_stop_counts_and_closes_what_is_queued()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_batch;
    operations.close = rocketnet_fake_client_close;

    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &operations, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_stop_test_time, &clock );

    uint8_t datagram[16] = {};
    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9150 );

    const int queued = ROCKETNET_CLIENT_IO_BATCH + 6;
    for ( int i = 0; i < queued; i++ )
    {
        datagram[0] = (uint8_t) i;
        rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    }

    rocketnet_client_io_stop( &io, 0, &from );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.inbound_count == 0 );
    rocketnet_unit_check( fake.close_calls == 1 );
    rocketnet_unit_check( io.receive_discarded_at_stop.load( std::memory_order_acquire ) == (uint64_t) queued );
    rocketnet_unit_check( fake.send_calls == 0 );
    rocketnet_unit_check( fake.wait_calls == 0 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.close_calls == 1 );
    rocketnet_unit_check( io.receive_discarded_at_stop.load( std::memory_order_acquire ) == (uint64_t) queued );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_stop_counts_and_closes_what_is_queued );

// Stop stores quit before its wake call, so a worker thread can reach its exit
// while that call is still to return; the exit closes the wake handle only
// once stop_woken says it has (RNClientIoShutdown NoWakeAfterClose, found by
// ThreadSanitizer as a write to the closed eventfd). Here the wake call sees
// stop_woken still clear and the handle still open, stop then stores it, and
// the exit that follows closes once. This order alone does not show the exit
// waiting; the development case after it does.

static rocketnet_client_io_t * client_io_stop_wake_io = NULL;
static int client_io_stop_wake_saw_woken = -1;
static int client_io_stop_wake_saw_closes = -1;

static void client_io_stop_test_wake_order( void * transport )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    client_io_stop_wake_saw_woken = client_io_stop_wake_io->stop_woken.load( std::memory_order_acquire ) ? 1 : 0;
    client_io_stop_wake_saw_closes = fake->close_calls;
    fake->wake_calls++;
    fake->wake = true;
}

static void test_client_io_stop_wakes_before_the_exit_closes()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_batch;
    operations.close = rocketnet_fake_client_close;
    operations.wake = client_io_stop_test_wake_order;

    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &operations, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_stop_test_time, &clock );
    client_io_stop_wake_io = &io;

    rocketnet_unit_check( !io.stop_woken.load( std::memory_order_acquire ) );

    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9160 );
    rocketnet_client_io_stop( &io, 0, &from );

    rocketnet_unit_check( fake.wake_calls == 1 );
    rocketnet_unit_check( client_io_stop_wake_saw_woken == 0 );
    rocketnet_unit_check( client_io_stop_wake_saw_closes == 0 );
    rocketnet_unit_check( io.stop_woken.load( std::memory_order_acquire ) );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.close_calls == 1 );

    client_io_stop_wake_io = NULL;
    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_stop_wakes_before_the_exit_closes );

#if ROCKETNET_DEV
// The interleaving itself, run on one thread: the wake call itself runs the I/O
// core's step, so the exit runs while that call is held, exactly where a
// worker thread can be. The exit then waits on stop_woken; the DEV stop-wait
// hold (rocketnet_dev_set_client_io_stop_wait_hook) answers each poll instead
// of a sleep, records whether the handle was closed by then, and on its third
// poll stores stop_woken as stop's return would. So the case sees the exit
// wait (polls), close nothing while the wake is held, and close once after
// the store. Without the wait the exit closes inside the wake call and the
// hold is never polled.

#define CLIENT_IO_STOP_WAKE_RELEASE_POLL 3
#define CLIENT_IO_STOP_WAKE_POLL_CEILING 1000

struct client_io_stop_wake_hold_t
{
    rocketnet_client_io_t * io;
    rocketnet_fake_client_transport_io_t * fake;
    int polls;
    int closes_while_held;
    int wake_saw_woken;
    int step_result;
    int closes_after_step;
};

static client_io_stop_wake_hold_t * client_io_stop_wake_hold = NULL;

static void client_io_stop_test_wait_poll( void * context )
{
    client_io_stop_wake_hold_t * hold = (client_io_stop_wake_hold_t *) context;
    hold->polls++;
    hold->closes_while_held += hold->fake->close_calls;
    rocketnet_unit_check( hold->polls < CLIENT_IO_STOP_WAKE_POLL_CEILING );
    if ( hold->polls >= CLIENT_IO_STOP_WAKE_RELEASE_POLL )
    {
        hold->io->stop_woken.store( true, std::memory_order_release );
    }
}

static void client_io_stop_test_wake_runs_exit( void * transport )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    client_io_stop_wake_hold_t * hold = client_io_stop_wake_hold;
    fake->wake_calls++;
    fake->wake = true;
    hold->wake_saw_woken = hold->io->stop_woken.load( std::memory_order_acquire ) ? 1 : 0;
    hold->step_result = rocketnet_client_io_step( hold->io, false );
    hold->closes_after_step = fake->close_calls;
}

static void test_client_io_stop_exit_waits_for_the_held_wake()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_batch;
    operations.close = rocketnet_fake_client_close;
    operations.wake = client_io_stop_test_wake_runs_exit;

    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &operations, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_stop_test_time, &clock );

    client_io_stop_wake_hold_t hold = {};
    hold.io = &io;
    hold.fake = &fake;
    hold.wake_saw_woken = -1;
    hold.step_result = -1;
    hold.closes_after_step = -1;
    client_io_stop_wake_hold = &hold;
    rocketnet_dev_set_client_io_stop_wait_hook( client_io_stop_test_wait_poll, &hold );

    rocketnet_unit_check( !io.stop_woken.load( std::memory_order_acquire ) );

    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9160 );
    rocketnet_client_io_stop( &io, 0, &from );

    rocketnet_dev_set_client_io_stop_wait_hook( NULL, NULL );
    client_io_stop_wake_hold = NULL;

    // the exit ran inside the held wake call, waited, closed nothing while
    // held, and closed once when stop_woken was stored

    rocketnet_unit_check( fake.wake_calls == 1 );
    rocketnet_unit_check( hold.wake_saw_woken == 0 );
    rocketnet_unit_check( hold.step_result == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( hold.polls == CLIENT_IO_STOP_WAKE_RELEASE_POLL );
    rocketnet_unit_check( hold.closes_while_held == 0 );
    rocketnet_unit_check( hold.closes_after_step == 1 );
    rocketnet_unit_check( io.stopped.load( std::memory_order_acquire ) );
    rocketnet_unit_check( io.stop_woken.load( std::memory_order_acquire ) );

    // a step after the exit closes nothing again

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.close_calls == 1 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_stop_exit_waits_for_the_held_wake );
#endif // #if ROCKETNET_DEV

// A late hand-off after worker exit asserts in Debug and is refused even when
// the assertion hook returns or assertions are compiled out. It cannot bind
// a destination, mutate either ring, or wake the exited worker.
static int client_io_stop_hand_off_asserts = 0;

static void client_io_stop_hand_off_assert_hook( const char * condition, const char * function,
                                                 const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    client_io_stop_hand_off_asserts++;
}

static void test_client_io_hand_off_after_stop_asserts()
{
    void ( *saved_assert )( const char * condition, const char * function, const char * file, int line ) =
        rn_assert_function;
    rocketnet_assert_function( client_io_stop_hand_off_assert_hook );
    client_io_stop_hand_off_asserts = 0;

    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 5;

    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_stop_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9200 );

    rocketnet_client_io_stop( &io, 0, &to );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( io.stopped.load( std::memory_order_acquire ) );

    const int fill = io.send_fill;
    const int ready = io.send.ready_count;
    const int free_count = io.send.free_count;
    uint64_t committed[ROCKETNET_CLIENT_IO_NUM_RINGS];
    rocketnet_address_t destination[ROCKETNET_CLIENT_IO_NUM_RINGS];
    bool held[ROCKETNET_CLIENT_IO_NUM_RINGS];

    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        committed[i] = io.committed[i];
        destination[i] = io.to[i];
        held[i] = io.to_held[i];
    }

    const int wakes = fake.wake_calls;
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_EMPTY );

#if ROCKETNET_ASSERTS
    rocketnet_unit_check( client_io_stop_hand_off_asserts == 1 );
#else
    rocketnet_unit_check( client_io_stop_hand_off_asserts == 0 );
#endif // #if ROCKETNET_ASSERTS
    rocketnet_unit_check( io.send_fill == fill );
    rocketnet_unit_check( io.send.ready_count == ready && io.send.free_count == free_count );

    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        rocketnet_unit_check( io.committed[i] == committed[i] );
        rocketnet_unit_check( memcmp( &io.to[i], &destination[i], sizeof( destination[i] ) ) == 0 );
        rocketnet_unit_check( io.to_held[i] == held[i] );
    }

    rocketnet_unit_check( fake.wake_calls == wakes );

    rocketnet_assert_function( saved_assert );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_hand_off_after_stop_asserts );

#if ROCKETNET_DEV
// the bounded destroy-wait harness: a step-driven transport on a wrapper of
// the fake driver whose receive (called once per I/O step) counts steps,
// advances the test clock (so destroy's deadline fires) and fails the case
// when a step budget is exceeded. A destroy that never returns spins past the
// budget and goes red; one bounded by its deadline returns first. No real time,
// no socket, no thread.
struct destroy_wait_clock_t
{
    double now;
    long steps;     // I/O steps taken (receive calls)
    long budget;    // step budget; exceeded fails the case
    uint64_t dropped; // SEND_RING_FULL when the driver closes (destroy's drop count)
};

struct destroy_wait_harness_t
{
    rocketnet_fake_client_transport_io_t fake;
    destroy_wait_clock_t clock;
    rocketnet_client_transport_t * transport;   // read at close, before destroy frees it
};

static double destroy_wait_time( void * context )
{
    return ( (destroy_wait_clock_t *) context )->now;
}

static void * destroy_wait_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    return context;    // the driver instance is the harness
}

static void destroy_wait_destroy( void * driver )
{
    destroy_wait_harness_t * h = (destroy_wait_harness_t *) driver;

    // the transport is still alive when the driver closes: read the counter
    // destroy increments before it frees the transport

    if ( h->transport )
    {
        h->clock.dropped = rocketnet_client_transport_counters( h->transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    }
    rocketnet_fake_client_destroy( &h->fake );
}

static void destroy_wait_send( void * driver, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    destroy_wait_harness_t * h = (destroy_wait_harness_t *) driver;
    rocketnet_fake_client_send( &h->fake, to, data, bytes );
}

static int destroy_wait_receive( void * driver, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    destroy_wait_harness_t * h = (destroy_wait_harness_t *) driver;
    h->clock.steps++;
    h->clock.now += 0.001;
    if ( h->clock.steps > h->clock.budget )
    {
        rocketnet_unit_check( 0 );
    }

    return rocketnet_fake_client_receive( &h->fake, from, data, capacity );
}

static bool destroy_wait_wait( void * driver, bool readable, double timeout_seconds )
{
    destroy_wait_harness_t * h = (destroy_wait_harness_t *) driver;
    return rocketnet_fake_client_wait( &h->fake, readable, timeout_seconds );
}

static void destroy_wait_wake( void * driver )
{
    destroy_wait_harness_t * h = (destroy_wait_harness_t *) driver;
    rocketnet_fake_client_wake( &h->fake );
}

static const rocketnet_client_transport_io_operations_t destroy_wait_io_operations = {
    destroy_wait_create, destroy_wait_destroy, destroy_wait_send, destroy_wait_receive,
    NULL, NULL, destroy_wait_wait, destroy_wait_wake, NULL, NULL, NULL, NULL
};

static void destroy_wait_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static rocketnet_client_transport_t * destroy_wait_transport_create( destroy_wait_harness_t * h )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    destroy_wait_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = destroy_wait_time;
    h->transport = rocketnet_client_transport_create_step_driven( &h->clock, token, &config, h->clock.now, &destroy_wait_io_operations, h );
    return h->transport;
}

// A worker that does not run (stuck in a platform call) holds two queued
// rings. Stop returns at once, polling neither advances the clock nor frees
// worker-owned storage, and completion waits for the worker's own steps.

static void test_client_io_stop_retains_stuck_ring_without_waiting()
{
    destroy_wait_harness_t h = {};
    h.clock.now = 1000.0;
    h.clock.budget = 100;

    rocketnet_client_transport_t * transport = destroy_wait_transport_create( &h );
    rocketnet_unit_check( transport != NULL );

    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    rocketnet_unit_check( io != NULL );
    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1001.0 );
    rocketnet_unit_check( io->send_fill == 2 && io->send.ready_count == 2 );

    const double before = h.clock.now;
    const long steps = h.clock.steps;
    rocketnet_client_transport_stop( transport );

    const int wakes = h.fake.wake_calls;
    rocketnet_client_transport_stop( transport );
    rocketnet_unit_check( h.fake.wake_calls == wakes );
    for ( int i = 0; i < 100; ++i )
    {
        rocketnet_unit_check( !rocketnet_client_transport_stopped( transport ) );
    }
    rocketnet_unit_check( h.clock.now == before );
    rocketnet_unit_check( h.clock.steps == steps );
    rocketnet_unit_check( h.fake.destroy_calls == 0 );
    rocketnet_unit_check( io->send.ready_count == 2 );
    rocketnet_unit_check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( rocketnet_client_transport_stopped( transport ) );
    rocketnet_unit_check( h.fake.num_sent == 2 );
    rocketnet_client_transport_destroy( transport );
    rocketnet_unit_check( h.fake.destroy_calls == 1 );
    rocketnet_unit_check( h.clock.dropped == 0 );
}
REGISTER_UNIT_TEST( test_client_io_stop_retains_stuck_ring_without_waiting );

// A returning assertion hook and a release build must both retain the object
// after premature destroy, so a caller can finish stopping it safely.

static void test_client_io_destroy_requires_completed_stop()
{
    destroy_wait_harness_t h = {};
    h.clock.now = 1000.0;
    h.clock.budget = 100;

    rocketnet_client_transport_t * transport = destroy_wait_transport_create( &h );
    rocketnet_unit_check( transport != NULL );
    rocketnet_client_transport_stop( transport );

    auto saved_assert = rn_assert_function;
    client_io_stop_hand_off_asserts = 0;
    rocketnet_assert_function( client_io_stop_hand_off_assert_hook );
    rocketnet_client_transport_destroy( transport );
    rocketnet_assert_function( saved_assert );
#if ROCKETNET_ASSERTS
    rocketnet_unit_check( client_io_stop_hand_off_asserts == 1 );
#endif // #if ROCKETNET_ASSERTS
    rocketnet_unit_check( h.fake.destroy_calls == 0 );
    rocketnet_unit_check( !rocketnet_client_transport_stopped( transport ) );
    rocketnet_unit_check( h.clock.now == 1000.0 );
    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_unit_check( rocketnet_client_transport_stopped( transport ) );
    rocketnet_client_transport_destroy( transport );
    rocketnet_unit_check( h.fake.destroy_calls == 1 );
}
REGISTER_UNIT_TEST( test_client_io_destroy_requires_completed_stop );

// Create fails after the transport worker is up. The hold seam keeps that
// worker not-stopped and counts polls; the wait is the build's own loop, run
// on a counter clock its sleep advances. Its 1 s deadline at 1 ms steps ends
// it after two polls a step (one before the transport's update, one after):
// past the ceiling the case fails, so a wait with no deadline does not hang
// and does not sleep. On the deadline the wait logs once and the transport,
// still owned by its worker, is kept: destroy is not called on it. No socket.
struct link_stop_wait_hold_t
{
    int polls;
    int ceiling;
    int destroyed;
    int logged;
};

static int link_stop_wait_hold( void * context )
{
    link_stop_wait_hold_t * hold = (link_stop_wait_hold_t *) context;
    hold->polls++;
    if ( hold->polls > hold->ceiling )
    {
        rocketnet_unit_check( 0 );
    }
    return 1;
}

static void link_stop_wait_destroy( void * transport )
{
    ( (link_stop_wait_hold_t *) transport )->destroyed++;
}

static void link_stop_wait_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t )
{
    if ( level == ROCKETNET_LOG_LEVEL_ERROR && strstr( message, "the transport is kept, not freed" ) != NULL )
    {
        ( (link_stop_wait_hold_t *) context )->logged++;
    }
}

static void test_message_link_stop_and_wait_bounds_held_worker()
{
    link_stop_wait_hold_t hold = {};
    hold.ceiling = 2 * 1000 + 4;

    rocketnet_dev_set_link_stop_wait_hold_hook( link_stop_wait_hold, &hold );
    rocketnet_log_function( link_stop_wait_log, &hold );

    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = &hold;
    link.destroy = link_stop_wait_destroy;

    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 0, NULL, NULL, 0, 1, 1000.0, false );

    rocketnet_log_function( NULL, NULL );
    rocketnet_dev_set_link_stop_wait_hold_hook( NULL, NULL );

    rocketnet_unit_check( object == NULL );
    rocketnet_unit_check( hold.destroyed == 0 );
    rocketnet_unit_check( hold.logged == 1 );
    rocketnet_unit_check( hold.polls >= 2 * 1000 );
}
REGISTER_UNIT_TEST( test_message_link_stop_and_wait_bounds_held_worker );
#endif // #if ROCKETNET_DEV

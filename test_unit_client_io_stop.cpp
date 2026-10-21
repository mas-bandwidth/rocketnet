/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The stop of the client I/O core (sdk/rocketnet_client_io.h) against the
// fake client transport (test/fake_client_driver.h): RNClientIo Stop, IoExit,
// Join and the invariant FreedOnlyAfterJoin. Single-threaded: no sockets, no
// real time.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"

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

static void client_io_stop_test_setup( rocketnet_client_io_t * io, rocketnet_client_transport_io_t * transport,
    rocketnet_client_io_ring_view_t * view0, rocketnet_client_io_ring_view_t * view1, uint8_t * ring0,
    uint8_t * ring1, int * counts0, int * counts1, int slots,
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] )
{
    for ( int i = 0; i < slots; i++ )
        ring0[ROCKETNET_FRAME_WIRE_OFFSET + i] = (uint8_t) ( i + 1 );

    view0->data = ring0;
    view0->frame_stride = ROCKETNET_FRAME_STRIDE;
    view0->wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0->wire_bytes = (const uint8_t *) counts0;
    view0->wire_bytes_stride = sizeof( int );

    view1->data = ring1;
    view1->frame_stride = ROCKETNET_FRAME_STRIDE;
    view1->wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1->wire_bytes = (const uint8_t *) counts1;
    view1->wire_bytes_stride = sizeof( int );

    client_io_stop_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_init( io, transport, view0, view1, buffers, client_io_stop_test_time, &clock );
}

// Stop with a handed-off ring: the I/O thread sends the ring in flight (both
// packets) and then exits on the same step. wake_calls 2: the hand-off wakes
// once, stop wakes again. RNClientIo Stop, IoExit.
static void test_client_io_stop_sends_in_flight_ring_first()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[2 * ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[2 * ROCKETNET_FRAME_STRIDE] = {};
    int counts0[2] = { 5, 7 };
    int counts1[2] = { 0, 0 };

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_ring_view_t view0 = {};
    rocketnet_client_io_ring_view_t view1 = {};
    client_io_stop_test_setup( &io, &transport, &view0, &view1, ring0, ring1, counts0, counts1, 2, buffers );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9000 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 2, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( fake.wake_calls == 1 );

    rocketnet_client_io_stop( &io );
    rocketnet_unit_check( fake.wake_calls == 2 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.send_calls == 2 );
    rocketnet_unit_check( fake.num_sent == 2 );
    rocketnet_unit_check( fake.sent[0].bytes == 5 );
    rocketnet_unit_check( fake.sent[1].bytes == 7 );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_stop_sends_in_flight_ring_first );

// Stop with nothing handed off and a datagram waiting: the step exits at once,
// sends nothing, does not receive, does not wait. RNClientIo IoExit (no
// receive, no publish, no wait).
static void test_client_io_stop_exits_with_nothing_new()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[ROCKETNET_FRAME_STRIDE] = {};
    int counts0[1] = { 0 };
    int counts1[1] = { 0 };

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_ring_view_t view0 = {};
    rocketnet_client_io_ring_view_t view1 = {};
    client_io_stop_test_setup( &io, &transport, &view0, &view1, ring0, ring1, counts0, counts1, 0, buffers );

    uint8_t datagram[16];
    for ( int i = 0; i < 16; i++ )
        datagram[i] = (uint8_t) ( 0x40 + i );
    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9100 );
    rocketnet_fake_client_push( &fake, &from, datagram, 16 );

    rocketnet_client_io_stop( &io );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.send_calls == 0 );
    rocketnet_unit_check( fake.receive_calls == 0 );
    rocketnet_unit_check( fake.wait_calls == 0 );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_stop_exits_with_nothing_new );

// Handing off after stop asserts once through the installed non-aborting hook.
// RNClientIo Stop (nothing new after stop); the assert compiles out in a
// release build (ROCKETNET_ASSERTS 0), so the hook stays quiet there.
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
    void (*saved_assert)( const char * condition, const char * function, const char * file, int line ) =
        rn_assert_function;
    rocketnet_assert_function( client_io_stop_hand_off_assert_hook );
    client_io_stop_hand_off_asserts = 0;

    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[ROCKETNET_FRAME_STRIDE] = {};
    int counts0[1] = { 5 };
    int counts1[1] = { 0 };

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_ring_view_t view0 = {};
    rocketnet_client_io_ring_view_t view1 = {};
    client_io_stop_test_setup( &io, &transport, &view0, &view1, ring0, ring1, counts0, counts1, 1, buffers );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9200 );

    rocketnet_client_io_stop( &io );
    rocketnet_client_io_hand_off( &io, 1, &to );

#if ROCKETNET_ASSERTS
    rocketnet_unit_check( client_io_stop_hand_off_asserts == 1 );
#else
    rocketnet_unit_check( client_io_stop_hand_off_asserts == 0 );
#endif

    rocketnet_assert_function( saved_assert );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_hand_off_after_stop_asserts );

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
        h->clock.dropped = rocketnet_client_transport_counters( h->transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
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
        rocketnet_unit_check( 0 );
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

static const rocketnet_client_transport_io_operations_t destroy_wait_io_operations =
{
    destroy_wait_create, destroy_wait_destroy, destroy_wait_send, destroy_wait_receive,
    NULL, NULL, destroy_wait_wait, destroy_wait_wake, NULL, NULL
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

// The last hand-off's wait, bounded: the other ring is stuck in SENDING (a
// send that never completes), so it never reaches Sent and the wait would spin
// forever without the deadline. Destroy gives up at the 2 s deadline (the
// clock advances 1 ms per I/O step), counts the committed slot it drops under
// SEND_RING_FULL, and continues to Stop; the post-stop step loop is bounded by
// its own 2 s deadline (a SENDING ring is in flight), so the whole destroy
// takes at most 4 s of test time. Nothing is sent (the committed slot is
// dropped, not handed off).
static void test_client_io_destroy_handoff_wait_is_bounded()
{
    destroy_wait_harness_t h = {};
    h.clock.now = 1000.0;
    h.clock.budget = 10000;   // 10 s of test time: past both 2 s bounds, far short of forever
    rocketnet_client_transport_t * transport = destroy_wait_transport_create( &h );
    rocketnet_unit_check( transport != NULL );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    rocketnet_unit_check( io != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1001.0 );
    rocketnet_unit_check( io->rings.fill == 1 );
    rocketnet_unit_check( io->rings.state[0].load() == ROCKETNET_CLIENT_RING_HANDED_OFF );
    io->rings.state[0].store( ROCKETNET_CLIENT_RING_SENDING, std::memory_order_relaxed );

    const double now_before = h.clock.now;
    const long steps_before = h.clock.steps;
    rocketnet_client_transport_destroy( transport );
    const double elapsed = h.clock.now - now_before;
    rocketnet_unit_check( h.clock.steps - steps_before < h.clock.budget );
    rocketnet_unit_check( elapsed >= 3.999 );   // both waits reach their 2 s deadlines
    rocketnet_unit_check( elapsed <= 4.002 );   // and neither runs past them
    rocketnet_unit_check( h.clock.dropped == 1 ); // the committed slot destroy drops, counted
    rocketnet_unit_check( h.fake.destroy_calls == 1 );
    rocketnet_unit_check( h.fake.num_sent == 0 );
}
REGISTER_UNIT_TEST( test_client_io_destroy_handoff_wait_is_bounded );

// The post-stop step loop, bounded: no committed fill slots, but a ring stuck
// in SENDING keeps the I/O core in flight, so the step loop never reaches
// EXITED without the deadline. Destroy gives up at the 2 s deadline (1 ms per
// step) and returns; nothing is dropped because destroy handed off nothing.
static void test_client_io_destroy_step_loop_is_bounded()
{
    destroy_wait_harness_t h = {};
    h.clock.now = 1000.0;
    h.clock.budget = 10000;   // 10 s of test time: past the 2 s bound, far short of forever
    rocketnet_client_transport_t * transport = destroy_wait_transport_create( &h );
    rocketnet_unit_check( transport != NULL );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    rocketnet_unit_check( io != NULL );

    io->rings.state[0].store( ROCKETNET_CLIENT_RING_SENDING, std::memory_order_relaxed );

    const double now_before = h.clock.now;
    const long steps_before = h.clock.steps;
    rocketnet_client_transport_destroy( transport );
    const double elapsed = h.clock.now - now_before;
    rocketnet_unit_check( h.clock.steps - steps_before < h.clock.budget );
    rocketnet_unit_check( elapsed >= 1.999 );   // the step loop reaches its 2 s deadline
    rocketnet_unit_check( elapsed <= 2.002 );   // and does not run past it
    rocketnet_unit_check( h.clock.dropped == 0 );
    rocketnet_unit_check( h.fake.destroy_calls == 1 );
}
REGISTER_UNIT_TEST( test_client_io_destroy_step_loop_is_bounded );

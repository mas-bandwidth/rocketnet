/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The stop of the client I/O core (sdk/rocketnet_client_io.h) against the
// fake client driver (test/fake_client_driver.h): RNClientIo Stop, IoExit,
// Join and the invariant FreedOnlyAfterJoin. Single-threaded: no sockets, no
// real time.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"

struct client_io_stop_test_clock_t
{
    double now;
};

static double client_io_stop_test_time( void * context )
{
    return ( (client_io_stop_test_clock_t *) context )->now;
}

static void client_io_stop_test_open( rocketnet_fake_client_driver_t * fake,
    const rocketnet_client_driver_operations_t * operations, rocketnet_client_driver_t * driver )
{
    rocketnet_client_driver_config_t config = { "fake", NULL };
    rocketnet_address_t bind = {};
    rocketnet_unit_check( rocketnet_client_driver_open( operations, fake, &config, &bind, driver ) );
}

static void client_io_stop_test_setup( rocketnet_client_io_t * io, rocketnet_client_driver_t * driver,
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
    rocketnet_client_io_init( io, driver, view0, view1, buffers, client_io_stop_test_time, &clock );
}

// Stop with a handed-off ring: the I/O thread sends the ring in flight (both
// packets) and then exits on the same step. wake_calls 2: the hand-off wakes
// once, stop wakes again. RNClientIo Stop, IoExit.
static void client_io_stop_sends_in_flight_ring_first()
{
    rocketnet_fake_client_driver_t fake = {};
    rocketnet_client_driver_t driver = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &driver );

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
    client_io_stop_test_setup( &io, &driver, &view0, &view1, ring0, ring1, counts0, counts1, 2, buffers );

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

    rocketnet_client_driver_close( &driver );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( client_io_stop_sends_in_flight_ring_first );

// Stop with nothing handed off and a datagram waiting: the step exits at once,
// sends nothing, does not receive, does not wait. RNClientIo IoExit (no
// receive, no publish, no wait).
static void client_io_stop_exits_with_nothing_new()
{
    rocketnet_fake_client_driver_t fake = {};
    rocketnet_client_driver_t driver = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &driver );

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
    client_io_stop_test_setup( &io, &driver, &view0, &view1, ring0, ring1, counts0, counts1, 0, buffers );

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

    rocketnet_client_driver_close( &driver );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( client_io_stop_exits_with_nothing_new );

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

static void client_io_hand_off_after_stop_asserts()
{
    void (*saved_assert)( const char * condition, const char * function, const char * file, int line ) =
        rocketnet_assert_function_pointer;
    rocketnet_set_assert_callback( client_io_stop_hand_off_assert_hook );
    client_io_stop_hand_off_asserts = 0;

    rocketnet_fake_client_driver_t fake = {};
    rocketnet_client_driver_t driver = {};
    client_io_stop_test_open( &fake, &rocketnet_fake_client_operations_io, &driver );

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
    client_io_stop_test_setup( &io, &driver, &view0, &view1, ring0, ring1, counts0, counts1, 1, buffers );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9200 );

    rocketnet_client_io_stop( &io );
    rocketnet_client_io_hand_off( &io, 1, &to );

#if ROCKETNET_ASSERTS
    rocketnet_unit_check( client_io_stop_hand_off_asserts == 1 );
#else
    rocketnet_unit_check( client_io_stop_hand_off_asserts == 0 );
#endif

    rocketnet_set_assert_callback( saved_assert );

    rocketnet_client_driver_close( &driver );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( client_io_hand_off_after_stop_asserts );

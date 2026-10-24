/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The I/O step (sdk/rocketnet_client_io.h) against the fake client transport
// (tests/fake_client_driver.h) and the RNClientIo actions of
// tla/RNClientIo.tla. Single-threaded: no sockets, no real time.

#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"

struct client_io_step_test_clock_t
{
    double now;
};

static double client_io_step_test_time( void * context )
{
    return ( (client_io_step_test_clock_t *) context )->now;
}

static void client_io_step_test_open( rocketnet_fake_client_transport_io_t * fake,
    const rocketnet_client_transport_io_operations_t * operations, rocketnet_client_transport_io_t * transport )
{
    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( operations, fake, &config, &bind, transport ) );
}

// Three slots, the middle aborted: one step sends the two live slots byte
// exact to the handed-off destination, per packet with the io table, and the
// next step is idle. RNClientIo HandOff, IoSend, IoFinishRing.
static void test_client_io_step_sends_handed_off_ring()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[3 * ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[3 * ROCKETNET_FRAME_STRIDE] = {};
    int counts0[3] = { 11, 0, 7 };
    int counts1[3] = { 0, 0, 0 };
    for ( int i = 0; i < 11; i++ )
        ring0[ROCKETNET_FRAME_WIRE_OFFSET + i] = (uint8_t) ( i + 1 );
    for ( int i = 0; i < 7; i++ )
        ring0[2 * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET + i] = (uint8_t) ( 0x80 + i );

    rocketnet_client_io_ring_view_t view0 = {};
    view0.data = ring0;
    view0.frame_stride = ROCKETNET_FRAME_STRIDE;
    view0.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0.wire_bytes = (const uint8_t *) counts0;
    view0.wire_bytes_stride = sizeof( int );

    rocketnet_client_io_ring_view_t view1 = {};
    view1.data = ring1;
    view1.frame_stride = ROCKETNET_FRAME_STRIDE;
    view1.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1.wire_bytes = (const uint8_t *) counts1;
    view1.wire_bytes_stride = sizeof( int );

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_init( &io, &transport, &view0, &view1, buffers, client_io_step_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9000 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 3, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( fake.wake_calls == 1 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.send_calls == 2 );
    rocketnet_unit_check( fake.send_batch_calls == 0 );
    rocketnet_unit_check( fake.num_sent == 2 );
    rocketnet_unit_check( fake.sent[0].bytes == 11 );
    rocketnet_unit_check( memcmp( fake.sent[0].data, ring0 + ROCKETNET_FRAME_WIRE_OFFSET, 11 ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &fake.sent[0].address, &to ) );
    rocketnet_unit_check( fake.sent[1].bytes == 7 );
    rocketnet_unit_check( memcmp( fake.sent[1].data,
        ring0 + 2 * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET, 7 ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &fake.sent[1].address, &to ) );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_IDLE );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_step_sends_handed_off_ring );

// The batch table takes the whole ring in one send_batch call and no send
// calls. RNClientIo IoSend with the transport's batch callback.
static void test_client_io_step_uses_send_batch_when_set()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_batch, &transport );

    uint8_t ring0[2 * ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[2 * ROCKETNET_FRAME_STRIDE] = {};
    int counts0[2] = { 9, 13 };
    int counts1[2] = { 0, 0 };
    for ( int i = 0; i < 9; i++ )
        ring0[ROCKETNET_FRAME_WIRE_OFFSET + i] = (uint8_t) ( 0x10 + i );
    for ( int i = 0; i < 13; i++ )
        ring0[ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET + i] = (uint8_t) ( 0x20 + i );

    rocketnet_client_io_ring_view_t view0 = {};
    view0.data = ring0;
    view0.frame_stride = ROCKETNET_FRAME_STRIDE;
    view0.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0.wire_bytes = (const uint8_t *) counts0;
    view0.wire_bytes_stride = sizeof( int );

    rocketnet_client_io_ring_view_t view1 = {};
    view1.data = ring1;
    view1.frame_stride = ROCKETNET_FRAME_STRIDE;
    view1.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1.wire_bytes = (const uint8_t *) counts1;
    view1.wire_bytes_stride = sizeof( int );

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_init( &io, &transport, &view0, &view1, buffers, client_io_step_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9001 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 2, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.send_batch_calls == 1 );
    rocketnet_unit_check( fake.send_calls == 0 );
    rocketnet_unit_check( fake.num_sent == 2 );
    rocketnet_unit_check( fake.sent[0].bytes == 9 );
    rocketnet_unit_check( fake.sent[1].bytes == 13 );
    rocketnet_unit_check( memcmp( fake.sent[1].data,
        ring0 + ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET, 13 ) == 0 );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_step_uses_send_batch_when_set );

// The clock callback stamps receive_time: one datagram read with now = 1000.25
// is exposed with receive_time 1000.25. docs/CLIENT-IO.md _The clock_.
static void test_client_io_receive_time_from_callback()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[ROCKETNET_FRAME_STRIDE] = {};
    int counts0[1] = { 0 };
    int counts1[1] = { 0 };

    rocketnet_client_io_ring_view_t view0 = {};
    view0.data = ring0;
    view0.frame_stride = ROCKETNET_FRAME_STRIDE;
    view0.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0.wire_bytes = (const uint8_t *) counts0;
    view0.wire_bytes_stride = sizeof( int );

    rocketnet_client_io_ring_view_t view1 = {};
    view1.data = ring1;
    view1.frame_stride = ROCKETNET_FRAME_STRIDE;
    view1.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1.wire_bytes = (const uint8_t *) counts1;
    view1.wire_bytes_stride = sizeof( int );

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_init( &io, &transport, &view0, &view1, buffers, client_io_step_test_time, &clock );

    uint8_t datagram[32];
    for ( int i = 0; i < 32; i++ )
        datagram[i] = (uint8_t) ( 0x40 + i );
    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9100 );
    rocketnet_fake_client_push( &fake, &from, datagram, 32 );

    clock.now = 1000.25;
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );

    rocketnet_client_receive_buffer_t * received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 1 );
    rocketnet_unit_check( received->receive_time[0] == 1000.25 );
    rocketnet_unit_check( received->packet_bytes[0] == 32 );
    rocketnet_unit_check( memcmp( received->packet_data[0], datagram, 32 ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &received->from[0], &from ) );
    rocketnet_client_io_return( &io );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_receive_time_from_callback );

// Two datagrams, one under the size prefilter: the expose delivers both, the
// runt as a record for the main thread's prefilter to refuse and count; after
// its return a later datagram reaches the next expose. RNClientIo
// IoReceive, IoPublish, Expose, Return.
static void test_client_io_step_publishes_received()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[ROCKETNET_FRAME_STRIDE] = {};
    int counts0[1] = { 0 };
    int counts1[1] = { 0 };

    rocketnet_client_io_ring_view_t view0 = {};
    view0.data = ring0;
    view0.frame_stride = ROCKETNET_FRAME_STRIDE;
    view0.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0.wire_bytes = (const uint8_t *) counts0;
    view0.wire_bytes_stride = sizeof( int );

    rocketnet_client_io_ring_view_t view1 = {};
    view1.data = ring1;
    view1.frame_stride = ROCKETNET_FRAME_STRIDE;
    view1.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1.wire_bytes = (const uint8_t *) counts1;
    view1.wire_bytes_stride = sizeof( int );

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_init( &io, &transport, &view0, &view1, buffers, client_io_step_test_time, &clock );

    uint8_t datagram[16];
    for ( int i = 0; i < 16; i++ )
        datagram[i] = (uint8_t) ( 0x60 + i );
    uint8_t short_datagram[4] = { 1, 2, 3, 4 };
    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9200 );

    rocketnet_fake_client_push( &fake, &from, short_datagram, 4 );
    rocketnet_fake_client_push( &fake, &from, datagram, 16 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );

    rocketnet_client_receive_buffer_t * received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 2 );
    rocketnet_unit_check( received->packet_bytes[0] == 4 );
    rocketnet_unit_check( memcmp( received->packet_data[0], short_datagram, 4 ) == 0 );
    rocketnet_unit_check( received->packet_bytes[1] == 16 );
    rocketnet_unit_check( memcmp( received->packet_data[1], datagram, 16 ) == 0 );
    rocketnet_client_io_return( &io );

    rocketnet_fake_client_push( &fake, &from, datagram, 8 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );

    received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 1 );
    rocketnet_unit_check( received->packet_bytes[0] == 8 );
    rocketnet_unit_check( memcmp( received->packet_data[0], datagram, 8 ) == 0 );
    rocketnet_client_io_return( &io );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_step_publishes_received );

// A blocked idle step waits once with the guard timeout and readable true; a
// step with a handed-off ring is worked and does not wait. RNClientIo IoWait.
static void test_client_io_step_waits_only_when_idle()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[ROCKETNET_FRAME_STRIDE] = {};
    int counts0[1] = { 5 };
    int counts1[1] = { 0 };
    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9300 );

    rocketnet_client_io_ring_view_t view0 = {};
    view0.data = ring0;
    view0.frame_stride = ROCKETNET_FRAME_STRIDE;
    view0.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0.wire_bytes = (const uint8_t *) counts0;
    view0.wire_bytes_stride = sizeof( int );

    rocketnet_client_io_ring_view_t view1 = {};
    view1.data = ring1;
    view1.frame_stride = ROCKETNET_FRAME_STRIDE;
    view1.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1.wire_bytes = (const uint8_t *) counts1;
    view1.wire_bytes_stride = sizeof( int );

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_init( &io, &transport, &view0, &view1, buffers, client_io_step_test_time, &clock );

    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( fake.wait_calls == 1 );
    rocketnet_unit_check( fake.last_wait_timeout == ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS );
    rocketnet_unit_check( fake.last_wait_readable );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.wait_calls == 1 );
    rocketnet_unit_check( fake.num_sent == 1 );

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_step_waits_only_when_idle );

// An idle I/O thread is woken by its socket or its wake, never by a clock: the
// wait's timeout guards a lost wake and is not a polling cadence. Each idle
// blocked step may wake the thread at most 20 times a second. At a 1 ms guard,
// 1000 same-host clients woke 340,000 times a second, held 70% of a 64-thread
// host with up to 570 threads ready, and starved client I/O threads and the
// server's IOCP completion worker for up to 12.7 s, so clients timed out.
static void test_client_io_step_idle_wait_is_a_guard_not_a_cadence()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[ROCKETNET_FRAME_STRIDE] = {};
    int counts0[1] = { 0 };
    int counts1[1] = { 0 };

    rocketnet_client_io_ring_view_t view0 = {};
    view0.data = ring0;
    view0.frame_stride = ROCKETNET_FRAME_STRIDE;
    view0.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0.wire_bytes = (const uint8_t *) counts0;
    view0.wire_bytes_stride = sizeof( int );

    rocketnet_client_io_ring_view_t view1 = {};
    view1.data = ring1;
    view1.frame_stride = ROCKETNET_FRAME_STRIDE;
    view1.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1.wire_bytes = (const uint8_t *) counts1;
    view1.wire_bytes_stride = sizeof( int );

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_init( &io, &transport, &view0, &view1, buffers, client_io_step_test_time, &clock );

    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
        rocketnet_unit_check( fake.wait_calls == i + 1 );
        rocketnet_unit_check( fake.last_wait_readable );
        rocketnet_unit_check( fake.last_wait_timeout > 0.0 );
        rocketnet_unit_check( 1.0 / fake.last_wait_timeout <= 20.0 );
        clock.now += fake.last_wait_timeout;
    }

    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_step_idle_wait_is_a_guard_not_a_cadence );

// A full I/O buffer with update holding the other: the next step leaves the
// datagram in the fake, waits with readable false, and does not wake; the
// return at the end of the next update wakes the I/O thread exactly once, and
// the step after that publishes. MCRNClientIoBrokenFullWaitReadable.cfg,
// FullWaitsOnWake.
static void test_client_io_step_full_buffer_waits_on_wake_only()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    uint8_t ring0[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t ring1[ROCKETNET_FRAME_STRIDE] = {};
    int counts0[1] = { 0 };
    int counts1[1] = { 0 };

    rocketnet_client_io_ring_view_t view0 = {};
    view0.data = ring0;
    view0.frame_stride = ROCKETNET_FRAME_STRIDE;
    view0.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view0.wire_bytes = (const uint8_t *) counts0;
    view0.wire_bytes_stride = sizeof( int );

    rocketnet_client_io_ring_view_t view1 = {};
    view1.data = ring1;
    view1.frame_stride = ROCKETNET_FRAME_STRIDE;
    view1.wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
    view1.wire_bytes = (const uint8_t *) counts1;
    view1.wire_bytes_stride = sizeof( int );

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] =
    {
        new rocketnet_client_receive_buffer_t, new rocketnet_client_receive_buffer_t
    };
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_init( &io, &transport, &view0, &view1, buffers, client_io_step_test_time, &clock );

    uint8_t datagram[16];
    for ( int i = 0; i < 16; i++ )
        datagram[i] = (uint8_t) ( 0x70 + i );
    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9400 );

    rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );

    rocketnet_client_receive_buffer_t * held = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( held != NULL );
    rocketnet_unit_check( held->num_packets == 1 );

    for ( int batch = 0; batch < ROCKETNET_NUM_CLIENT_PACKETS / ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS; batch++ )
    {
        for ( int i = 0; i < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS; i++ )
            rocketnet_fake_client_push( &fake, &from, datagram, 16 );
        rocketnet_client_io_step( &io, true );
    }

    rocketnet_unit_check( io.fill_slots == ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_unit_check( io.buffer[io.receive.fill]->num_packets == ROCKETNET_NUM_CLIENT_PACKETS );

    const int wait_calls = fake.wait_calls;
    const int wake_calls = fake.wake_calls;

    rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( fake.inbound_count == 1 );
    rocketnet_unit_check( fake.wait_calls == wait_calls + 1 );
    rocketnet_unit_check( fake.last_wait_timeout == ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS );
    rocketnet_unit_check( !fake.last_wait_readable );
    rocketnet_unit_check( fake.wake_calls == wake_calls );

    rocketnet_client_io_return( &io );
    rocketnet_unit_check( fake.wake_calls == wake_calls + 1 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );

    rocketnet_client_receive_buffer_t * published = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( published != NULL );
    rocketnet_unit_check( published->num_packets == ROCKETNET_NUM_CLIENT_PACKETS );

    rocketnet_client_io_return( &io );
    rocketnet_client_transport_io_close( &transport );
    delete buffers[0];
    delete buffers[1];
}
REGISTER_UNIT_TEST( test_client_io_step_full_buffer_waits_on_wake_only );

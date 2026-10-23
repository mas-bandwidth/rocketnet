/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The I/O step (sdk/rocketnet_client_io.h) against the fake client transport
// (tests/fake_client_driver.h), the RNClientIo actions of tla/RNClientIo.tla
// and the exchanges of tla/RNBufferHandoff.tla. Single-threaded: no sockets,
// no real time.

#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "client_io_rig.h"

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
// next step only gives the ring back. RNClientIo HandOff, IoSend.
static void test_client_io_step_sends_handed_off_ring()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 11;
    rig->counts[0][1] = 0;
    rig->counts[0][2] = 7;
    for ( int i = 0; i < 11; i++ )
        rocketnet_client_io_rig_slot( rig, 0, 0 )[i] = (uint8_t) ( i + 1 );
    for ( int i = 0; i < 7; i++ )
        rocketnet_client_io_rig_slot( rig, 0, 2 )[i] = (uint8_t) ( 0x80 + i );

    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9000 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 3, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( fake.wake_calls == 1 );
    rocketnet_unit_check( io.send_fill == 1 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.send_ring == 0 );
    rocketnet_unit_check( fake.send_calls == 2 );
    rocketnet_unit_check( fake.send_batch_calls == 0 );
    rocketnet_unit_check( fake.num_sent == 2 );
    rocketnet_unit_check( fake.sent[0].bytes == 11 );
    rocketnet_unit_check( memcmp( fake.sent[0].data, rocketnet_client_io_rig_slot( rig, 0, 0 ), 11 ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &fake.sent[0].address, &to ) );
    rocketnet_unit_check( fake.sent[1].bytes == 7 );
    rocketnet_unit_check( memcmp( fake.sent[1].data, rocketnet_client_io_rig_slot( rig, 0, 2 ), 7 ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &fake.sent[1].address, &to ) );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( io.send_ring == -1 );
    rocketnet_unit_check( io.send.free_count == 2 );
    rocketnet_unit_check( fake.num_sent == 2 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_sends_handed_off_ring );

// The batch table takes the whole ring in one send_batch call and no send
// calls. RNClientIo IoSend with the transport's batch callback.
static void test_client_io_step_uses_send_batch_when_set()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_batch, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 9;
    rig->counts[0][1] = 13;
    for ( int i = 0; i < 9; i++ )
        rocketnet_client_io_rig_slot( rig, 0, 0 )[i] = (uint8_t) ( 0x10 + i );
    for ( int i = 0; i < 13; i++ )
        rocketnet_client_io_rig_slot( rig, 0, 1 )[i] = (uint8_t) ( 0x20 + i );

    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9001 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 2, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.send_batch_calls == 1 );
    rocketnet_unit_check( fake.send_calls == 0 );
    rocketnet_unit_check( fake.num_sent == 2 );
    rocketnet_unit_check( fake.sent[0].bytes == 9 );
    rocketnet_unit_check( fake.sent[1].bytes == 13 );
    rocketnet_unit_check( memcmp( fake.sent[1].data, rocketnet_client_io_rig_slot( rig, 0, 1 ), 13 ) == 0 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_uses_send_batch_when_set );

// Three hand-offs with no step between: two rings queue (MaxReady 2) and the
// third is deferred, its slots still in the Filling ring with the destination
// they were committed for. The steps send the queued rings oldest first, then
// the deferred one leaves with the next hand-off. RNBufferHandoff InOrder,
// MainNeverWaits; RNClientIo Defer, SentToCommitted.
static void test_client_io_step_sends_rings_in_order_and_defers()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        rig->counts[ring][0] = 4;
        rocketnet_client_io_rig_slot( rig, ring, 0 )[0] = (uint8_t) ( 0xA0 + ring );
    }

    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_address_t first = {};
    rocketnet_address_t second = {};
    rocketnet_address_load_ipv4( &first, 0x7f000001, 9010 );
    rocketnet_address_load_ipv4( &second, 0x7f000001, 9011 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &first ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &first ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( io.send_fill == 2 );
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &first ) == ROCKETNET_CLIENT_RINGS_LATE );
    rocketnet_unit_check( io.send.owed );
    rocketnet_unit_check( fake.wake_calls == 2 );

    // a later attempt for the deferred ring keeps the destination it was
    // committed with
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &second ) == ROCKETNET_CLIENT_RINGS_LATE );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.num_sent == 2 );
    rocketnet_unit_check( fake.sent[0].data[0] == 0xA0 );
    rocketnet_unit_check( fake.sent[1].data[0] == 0xA1 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &second ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( io.send_fill == 0 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.num_sent == 3 );
    rocketnet_unit_check( fake.sent[2].data[0] == 0xA2 );
    rocketnet_unit_check( rocketnet_address_equal( &fake.sent[2].address, &first ) );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_sends_rings_in_order_and_defers );

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// A hand-off while the I/O thread holds the mutex is deferred at once: the
// game thread never waits for the lock. RNBufferHandoff MainNeverWaits. The
// test holds the mutex on its own thread, so it runs where the platform mutex
// is not recursive (a Windows critical section is).
static void test_client_io_hand_off_never_waits_for_the_mutex()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 4;

    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9020 );

    // try-acquire fails while held; no other thread is needed to show it
    rocketnet_platform_mutex_acquire( &rig->mutex );
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_LATE );
    rocketnet_unit_check( io.send_fill == 0 && io.send.ready_count == 0 );
    rocketnet_unit_check( rocketnet_client_io_expose( &io ) == NULL );
    rocketnet_unit_check( fake.wake_calls == 0 );
    rocketnet_platform_mutex_release( &rig->mutex );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.num_sent == 1 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_hand_off_never_waits_for_the_mutex );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// The clock callback stamps receive_time: one datagram read with now = 1000.25
// is exposed with receive_time 1000.25. docs/CLIENT-IO.md _The clock_.
static void test_client_io_receive_time_from_callback()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

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
    rocketnet_client_io_rig_destroy( rig );
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

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

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
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_publishes_received );

// With a buffer already waiting ready, later datagrams join the I/O thread's
// own buffer instead of queueing a second one, so update's next expose takes
// everything published in one buffer and the rest in the next. RNBufferHandoff
// receive triple buffer (MaxReady 1), InOrder.
static void test_client_io_step_receive_triple_buffer()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9250 );
    uint8_t datagram[8] = {};
    for ( int i = 0; i < 3; i++ )
    {
        datagram[0] = (uint8_t) ( 0x10 + i );
        rocketnet_fake_client_push( &fake, &from, datagram, 8 );
        rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    }
    rocketnet_unit_check( io.receive.ready_count == 1 );
    rocketnet_unit_check( io.receive.owed );

    // the exchange that takes the ready buffer reports the owed publish
    const int wakes = fake.wake_calls;
    rocketnet_client_receive_buffer_t * received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( fake.wake_calls == wakes + 1 );
    rocketnet_unit_check( received != NULL && received->num_packets == 1 && received->packet_data[0][0] == 0x10 );
    rocketnet_client_io_return( &io );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL && received->num_packets == 2 );
    rocketnet_unit_check( received->packet_data[0][0] == 0x11 && received->packet_data[1][0] == 0x12 );
    rocketnet_client_io_return( &io );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_receive_triple_buffer );

// A blocked idle step waits once with the guard timeout and readable true; a
// step with a handed-off ring is worked and does not wait. RNClientIo IoWait.
static void test_client_io_step_waits_only_when_idle()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 5;
    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9300 );

    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( fake.wait_calls == 1 );
    rocketnet_unit_check( fake.last_wait_timeout == ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS );
    rocketnet_unit_check( fake.last_wait_readable );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.wait_calls == 1 );
    rocketnet_unit_check( fake.num_sent == 1 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_waits_only_when_idle );

// A full I/O buffer with one buffer ready and update holding the third: the
// next step leaves the datagram in the fake, waits with readable false, and
// does not wake; update's next exchange wakes the I/O thread exactly once,
// and the step after that publishes. RNClientIo FullWaitsOnWake,
// RNBufferHandoff ArrivedDelivered (the owed wake).
static void test_client_io_step_full_buffer_waits_on_wake_only()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &rocketnet_fake_client_operations_io, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    uint8_t datagram[16];
    for ( int i = 0; i < 16; i++ )
        datagram[i] = (uint8_t) ( 0x70 + i );
    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9400 );

    // update holds one buffer
    rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_client_receive_buffer_t * held = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( held != NULL );
    rocketnet_unit_check( held->num_packets == 1 );

    // one buffer published and waiting ready
    rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.receive.ready_count == 1 );

    // the I/O thread's own buffer fills to the brim
    for ( int batch = 0; batch < ROCKETNET_NUM_CLIENT_PACKETS / ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS; batch++ )
    {
        for ( int i = 0; i < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS; i++ )
            rocketnet_fake_client_push( &fake, &from, datagram, 16 );
        rocketnet_client_io_step( &io, true );
    }

    rocketnet_unit_check( io.fill_slots == ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_unit_check( io.buffer[io.receive_fill]->num_packets == ROCKETNET_NUM_CLIENT_PACKETS );

    const int wait_calls = fake.wait_calls;
    const int wake_calls = fake.wake_calls;

    rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( fake.inbound_count == 1 );
    rocketnet_unit_check( fake.wait_calls == wait_calls + 1 );
    rocketnet_unit_check( fake.last_wait_timeout == ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS );
    rocketnet_unit_check( !fake.last_wait_readable );
    rocketnet_unit_check( fake.wake_calls == wake_calls );

    // update's next frame: it gives back the held buffer and takes the ready
    // one, reads the owed publish and wakes the I/O thread once
    rocketnet_client_io_return( &io );
    rocketnet_unit_check( fake.wake_calls == wake_calls + 1 );
    rocketnet_client_receive_buffer_t * ready = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( ready != NULL && ready->num_packets == 1 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_client_io_return( &io );

    rocketnet_client_receive_buffer_t * published = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( published != NULL );
    rocketnet_unit_check( published->num_packets == ROCKETNET_NUM_CLIENT_PACKETS );

    rocketnet_client_io_return( &io );
    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_full_buffer_waits_on_wake_only );

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

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

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
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_idle_wait_is_a_guard_not_a_cadence );

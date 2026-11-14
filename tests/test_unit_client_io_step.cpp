/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The I/O step (sdk/rocketnet_client_io.h) against the fake client transport
// (tests/fake_client_driver.h), the RNClientIo actions of proofs/tla/RNClientIo.tla
// and the exchanges of proofs/tla/RNBufferHandoff.tla. Single-threaded: no sockets,
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
    {
        rocketnet_client_io_rig_slot( rig, 0, 0 )[i] = (uint8_t) ( i + 1 );
    }
    for ( int i = 0; i < 7; i++ )
    {
        rocketnet_client_io_rig_slot( rig, 0, 2 )[i] = (uint8_t) ( 0x80 + i );
    }

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
    {
        rocketnet_client_io_rig_slot( rig, 0, 0 )[i] = (uint8_t) ( 0x10 + i );
    }
    for ( int i = 0; i < 13; i++ )
    {
        rocketnet_client_io_rig_slot( rig, 0, 1 )[i] = (uint8_t) ( 0x20 + i );
    }

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

// A ring longer than one batch: the batch table without a progress callback
// flushes each full batch of ROCKETNET_CLIENT_IO_BATCH live slots during the
// walk and sends the rest after it, in slot order, skipping aborted slots.
// The rig's 4-slot rings never fill a batch, so this case lays out its own
// ring 0 (VERIFY-4 W7, sdk/rocketnet_client_io.h `num_sends ==
// ROCKETNET_CLIENT_IO_BATCH`).

#define CLIENT_IO_FULL_SLOTS ( 2 * ROCKETNET_CLIENT_IO_BATCH + 4 )

static int client_io_full_batch_sizes[8];
static int client_io_full_batch_calls;

static void client_io_full_send_batch( void * transport, const rocketnet_platform_send_t * sends, int count )
{
    rocketnet_unit_check( client_io_full_batch_calls < 8 );
    client_io_full_batch_sizes[client_io_full_batch_calls++] = count;
    rocketnet_fake_client_send_batch( transport, sends, count );
}

static void test_client_io_step_full_ring_flushes_whole_batches()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_batch;
    operations.send_batch = client_io_full_send_batch;
    client_io_full_batch_calls = 0;

    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &operations, &transport );

    // slot 5 is aborted (zero bytes), so the first batch closes at slot 64
    // and 2 * 64 + 3 live slots go out as 64, 64, 3

    uint8_t * data = new uint8_t[(size_t) CLIENT_IO_FULL_SLOTS * ROCKETNET_FRAME_STRIDE]();
    int * counts = new int[CLIENT_IO_FULL_SLOTS]();
    for ( int slot = 0; slot < CLIENT_IO_FULL_SLOTS; slot++ )
    {
        counts[slot] = slot == 5 ? 0 : 4 + slot % 7;
        uint8_t * bytes = data + (size_t) slot * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET;
        bytes[0] = (uint8_t) slot;
    }

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->view[0].data = data;
    rig->view[0].wire_bytes = (const uint8_t *) counts;

    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9001 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, CLIENT_IO_FULL_SLOTS, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );

    rocketnet_unit_check( client_io_full_batch_calls == 3 );
    rocketnet_unit_check( client_io_full_batch_sizes[0] == ROCKETNET_CLIENT_IO_BATCH );
    rocketnet_unit_check( client_io_full_batch_sizes[1] == ROCKETNET_CLIENT_IO_BATCH );
    rocketnet_unit_check( client_io_full_batch_sizes[2] == 3 );
    rocketnet_unit_check( fake.send_calls == 0 );
    rocketnet_unit_check( fake.num_sent == CLIENT_IO_FULL_SLOTS - 1 );

    // every live slot once, in slot order, byte exact in length and first byte

    for ( int slot = 0, sent = 0; slot < CLIENT_IO_FULL_SLOTS; slot++ )
    {
        if ( slot == 5 )
        {
            continue;
        }
        rocketnet_unit_check( fake.sent[sent].bytes == counts[slot] );
        rocketnet_unit_check( fake.sent[sent].data[0] == (uint8_t) slot );
        sent++;
    }

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
    delete[] counts;
    delete[] data;
}
REGISTER_UNIT_TEST( test_client_io_step_full_ring_flushes_whole_batches );

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

// A hand-off while the I/O thread holds the mutex is deferred at once: the
// game thread never waits for the lock. RNBufferHandoff MainNeverWaits. The
// test holds the mutex on the calling thread, so it runs on every platform:
// the platform mutex is not recursive.

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
    rocketnet_unit_check( rocketnet_client_io_rig_expose( rig, &io ) == NULL );
    rocketnet_unit_check( fake.wake_calls == 0 );
    rocketnet_platform_mutex_release( &rig->mutex );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.num_sent == 1 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_hand_off_never_waits_for_the_mutex );

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
    {
        datagram[i] = (uint8_t) ( 0x40 + i );
    }

    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9100 );
    rocketnet_fake_client_push( &fake, &from, datagram, 32 );

    clock.now = 1000.25;
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );

    const rocketnet_client_io_received_t * received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 1 );
    rocketnet_unit_check( received->receive_time[0] == 1000.25 );
    rocketnet_unit_check( received->packet_bytes[0] == 32 );
    rocketnet_unit_check( memcmp( received->packet_data[0], datagram, 32 ) == 0 );
    rocketnet_unit_check( rocketnet_address_equal( &received->from[0], &from ) );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_receive_time_from_callback );

// Two datagrams, one under the size prefilter: the expose delivers both, the
// runt as a record for the main thread's prefilter to refuse and count; a
// later datagram reaches the next expose. RNReceiveHandoff Bank, Commit, Call.

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
    {
        datagram[i] = (uint8_t) ( 0x60 + i );
    }

    uint8_t short_datagram[4] = { 1, 2, 3, 4 };
    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9200 );

    rocketnet_fake_client_push( &fake, &from, short_datagram, 4 );
    rocketnet_fake_client_push( &fake, &from, datagram, 16 );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );

    const rocketnet_client_io_received_t * received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 2 );
    rocketnet_unit_check( received->packet_bytes[0] == 4 );
    rocketnet_unit_check( memcmp( received->packet_data[0], short_datagram, 4 ) == 0 );
    rocketnet_unit_check( received->packet_bytes[1] == 16 );
    rocketnet_unit_check( memcmp( received->packet_data[1], datagram, 16 ) == 0 );

    rocketnet_fake_client_push( &fake, &from, datagram, 8 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );

    received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 1 );
    rocketnet_unit_check( received->packet_bytes[0] == 8 );
    rocketnet_unit_check( memcmp( received->packet_data[0], datagram, 8 ) == 0 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_publishes_received );

// Three datagrams committed in three steps across two of the three buffers:
// update's next expose returns all three, in arrival order, and needs no wake.
// This test asserted the rule before the receive handoff fix: with a buffer
// already waiting ready, later datagrams stayed in the I/O thread's own buffer
// and update's expose returned only the first, the other two a whole update
// later. Update now reads the I/O thread's buffer in place up to the
// committed count (sdk/rocketnet_receive_handoff.h), so the expose returns
// every datagram committed before it (RNReceiveHandoff DeliveredByNextCall,
// InOrder), and the rotation goes on: a fourth datagram reaches the next
// expose.

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

    // the first commit found update sharing the I/O thread's buffer and moved
    // the I/O thread to the free one, which holds the other two

    rocketnet_unit_check( io.receive.main != io.receive.worker );
    rocketnet_unit_check( io.receive.committed[io.receive.main] == 1 && io.receive.committed[io.receive.worker] == 2 );

    const int wakes = fake.wake_calls;
    const rocketnet_client_io_received_t * received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( fake.wake_calls == wakes );
    rocketnet_unit_check( received != NULL && received->num_packets == 3 );
    rocketnet_unit_check( received != NULL && received->packet_data[0][0] == 0x10 && received->packet_data[1][0] == 0x11 &&
                          received->packet_data[2][0] == 0x12 );

    datagram[0] = 0x13;
    rocketnet_fake_client_push( &fake, &from, datagram, 8 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( received != NULL && received->num_packets == 1 && received->packet_data[0][0] == 0x13 );
    rocketnet_unit_check( rocketnet_client_io_rig_expose( rig, &io ) == NULL );

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

// A full I/O buffer update has not started on: the next step leaves the
// datagram in the fake, waits with readable false, and does not wake;
// update's next expose starts on the full buffer and wakes the I/O thread
// exactly once, and the I/O thread then moves to the free buffer and reads
// the datagram that waited. RNClientIo FullWaitsOnWake, RNReceiveHandoff
// Commit (the move once update has started on the buffer).

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
    {
        datagram[i] = (uint8_t) ( 0x70 + i );
    }

    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9400 );

    // update starts on the buffer the I/O thread fills: one datagram,
    // committed, then exposed

    rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );

    const rocketnet_client_io_received_t * received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( received != NULL && received->num_packets == 1 );

    // the I/O thread's first batch finds update sharing its buffer, so it
    // moves to the free buffer, and its later batches fill that one to the
    // brim

    const int batch = ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS;
    for ( int b = 0; b <= ROCKETNET_NUM_CLIENT_PACKETS / batch; b++ )
    {
        for ( int i = 0; i < batch; i++ )
        {
            rocketnet_fake_client_push( &fake, &from, datagram, 16 );
        }
        rocketnet_client_io_step( &io, true );
    }

    rocketnet_unit_check( io.fill_slots == ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_unit_check( io.buffer[io.receive.worker]->num_packets == ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_unit_check( io.receive.main != io.receive.worker );

    const int wait_calls = fake.wait_calls;
    const int wake_calls = fake.wake_calls;

    rocketnet_fake_client_push( &fake, &from, datagram, 16 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( fake.inbound_count == 1 );
    rocketnet_unit_check( fake.wait_calls == wait_calls + 1 );
    rocketnet_unit_check( fake.last_wait_timeout == ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS );
    rocketnet_unit_check( !fake.last_wait_readable );
    rocketnet_unit_check( fake.wake_calls == wake_calls );

    // update's next expose starts on the full buffer and wakes the I/O thread
    // once; it returns the first batch and as much of the full buffer as its
    // budget leaves

    received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( fake.wake_calls == wake_calls + 1 );
    rocketnet_unit_check( received != NULL && received->num_packets == ROCKETNET_NUM_CLIENT_PACKETS );

    // woken, the I/O thread moves to the free buffer, then reads the datagram
    // that waited

    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.receive.main != io.receive.worker );
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.inbound_count == 0 );

    // the next expose returns the rest of the full buffer, then that datagram

    received = rocketnet_client_io_rig_expose( rig, &io );
    rocketnet_unit_check( received != NULL && received->num_packets == batch + 1 );

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

// A partial batch keeps its immutable ring; neither an aborted slot nor a
// second queued ring may make the already-consumed prefix repeat.
static int client_io_progress_allow;
static int client_io_progress_calls;

static int client_io_progress_send( void * instance, const rocketnet_platform_send_t * sends, int count )
{
    client_io_progress_calls++;
    const int consumed = count < client_io_progress_allow ? count : client_io_progress_allow;
    if ( consumed > 0 )
    {
        rocketnet_fake_client_send_batch( instance, sends, consumed );
    }
    return consumed;
}

static void test_client_io_step_retains_partial_send_prefix()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.send_batch = rocketnet_fake_client_send_batch;
    operations.send_batch_progress = client_io_progress_send;
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &operations, &transport );
    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );
    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9000 );
    rig->counts[0][0] = 1;
    rig->counts[0][2] = 1;
    rig->counts[0][3] = 1;
    rocketnet_client_io_rig_slot( rig, 0, 0 )[0] = 10;
    rocketnet_client_io_rig_slot( rig, 0, 2 )[0] = 20;
    rocketnet_client_io_rig_slot( rig, 0, 3 )[0] = 30;
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 4, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    rig->counts[1][0] = 1;
    rocketnet_client_io_rig_slot( rig, 1, 0 )[0] = 40;
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );

    client_io_progress_calls = 0;
    client_io_progress_allow = 1;
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.send_ring == 0 && io.send_slot == 2 );
    rocketnet_unit_check( io.send.free_count == 0 && fake.num_sent == 1 );
    rocketnet_unit_check( fake.sent[0].data[0] == 10 );

    client_io_progress_allow = 0;
    const int waits = fake.wait_calls;
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( fake.wait_calls == waits + 1 );
    rocketnet_unit_check( client_io_progress_calls == 2 );
    rocketnet_unit_check( io.send_ring == 0 && io.send_slot == 2 && io.send.free_count == 0 );
    rocketnet_unit_check( fake.num_sent == 1 );

    client_io_progress_allow = 4;
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.send_ring == 0 && io.send_slot == 4 && fake.num_sent == 3 );
    rocketnet_unit_check( fake.sent[1].data[0] == 20 && fake.sent[2].data[0] == 30 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.send_ring == 1 && fake.num_sent == 4 && fake.sent[3].data[0] == 40 );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( io.send_ring == -1 && io.send.free_count == 2 && fake.num_sent == 4 );
    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_retains_partial_send_prefix );

// A transient send refusal (the progress callback consuming nothing, as the
// platform does on EAGAIN/ENOBUFS) leaves committed slots unsent with no
// inbound packets: the blocked step must retry after 1-2 ms, not after the
// 50 ms idle guard, or the filling ring overflows with SEND_RING_FULL.

static void test_client_io_step_unsent_send_waits_with_retry_timeout()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.send_batch = rocketnet_fake_client_send_batch;
    operations.send_batch_progress = client_io_progress_send;
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &operations, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 5;
    for ( int i = 0; i < 5; i++ )
    {
        rocketnet_client_io_rig_slot( rig, 0, 0 )[i] = (uint8_t) ( 0x30 + i );
    }

    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9500 );

    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );

    client_io_progress_calls = 0;
    client_io_progress_allow = 0;
    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( io.send_ring == 0 && io.send_slot < io.committed[io.send_ring] );
    rocketnet_unit_check( fake.num_sent == 0 );
    rocketnet_unit_check( fake.inbound_count == 0 );
    rocketnet_unit_check( fake.wait_calls == 1 );
    rocketnet_unit_check( fake.last_wait_timeout >= 0.001 && fake.last_wait_timeout <= 0.002 );

    client_io_progress_allow = 1;
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.num_sent == 1 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_step_unsent_send_waits_with_retry_timeout );

// Quit attempts only the remaining suffix once through the terminal callback,
// even when the running callback would refuse forever.
static void test_client_io_stop_finishes_only_unconsumed_suffix()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.send_batch = rocketnet_fake_client_send_batch;
    operations.send_batch_progress = client_io_progress_send;
    rocketnet_client_transport_io_t transport = {};
    client_io_step_test_open( &fake, &operations, &transport );
    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    client_io_step_test_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_io_step_test_time, &clock );
    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9000 );
    for ( int i = 0; i < 3; i++ )
    {
        rig->counts[0][i] = 1;
        rocketnet_client_io_rig_slot( rig, 0, i )[0] = (uint8_t) ( i + 1 );
    }
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 3, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    client_io_progress_allow = 1;
    client_io_progress_calls = 0;
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( fake.num_sent == 1 && io.send_slot == 1 );
    client_io_progress_allow = 0;
    rocketnet_client_io_stop( &io, 0, &to );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( client_io_progress_calls == 1 && fake.num_sent == 3 );
    rocketnet_unit_check( fake.sent[0].data[0] == 1 && fake.sent[1].data[0] == 2 && fake.sent[2].data[0] == 3 );
    rocketnet_unit_check( io.stopped.load( std::memory_order_acquire ) );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    rocketnet_unit_check( fake.num_sent == 3 );
    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_stop_finishes_only_unconsumed_suffix );

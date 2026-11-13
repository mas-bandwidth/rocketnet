/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server's per-queue receive (sdk/rocketnet_uring_receive_queue.h),
// driven with fake completions on a queue whose buffer ring is plain memory:
// every refusal of create, the arm and the multishot's end, every datagram
// a completion carries banked or counted by name (the transport's refusals,
// each prefilter reason the worker can meet, a full bank, the socket's own
// drops), the bank's handoff to main and the buffer ring's 16-bit tail
// across its wrap. Pure memory: no ring, no socket, no thread, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_uring_receive_queue.h"

#if defined( __linux__ ) && ROCKETNET_DEV
#include <linux/io_uring.h>
#endif // #if defined( __linux__ ) && ROCKETNET_DEV

#if defined( __linux__ ) && ROCKETNET_DEV && defined( IORING_RECV_MULTISHOT )

#include "../sdk/rocketnet_uring_codec.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#define check rocketnet_unit_check

static const uint8_t receive_queue_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x5A, 0x11, 0xC3, 0x7E };

// the smallest buffer a queue without GRO takes, and room for a 2000 byte
// datagram past the receive ceiling
#define RECEIVE_QUEUE_PLAIN_BYTES ( 4096 )
#define RECEIVE_QUEUE_GRO_BYTES ( 70 * 1024 )

static struct rocketnet_uring_receive_queue_config_t receive_queue_config( int buffers, int slots, bool gro, bool socket_drops )
{
    struct rocketnet_uring_receive_queue_config_t config;
    memset( &config, 0, sizeof( config ) );
    config.queue_index = 3;
    config.fd = 17;
    config.bgid = 9;
    config.buffers = buffers;
    config.buffer_bytes = gro ? RECEIVE_QUEUE_GRO_BYTES : RECEIVE_QUEUE_PLAIN_BYTES;
    config.slots = slots;
    config.ipv6 = false;
    config.gro = gro;
    config.socket_drops = socket_drops;
    config.packet_filter = receive_queue_filter;
    return config;
}

static struct rocketnet_uring_receive_queue_t * receive_queue_create( int buffers, int slots, bool gro, bool socket_drops )
{
    const struct rocketnet_uring_receive_queue_config_t config = receive_queue_config( buffers, slots, gro, socket_drops );
    struct rocketnet_uring_receive_queue_t * queue = rocketnet_uring_receive_queue_create( NULL, NULL, &config );
    check( queue );
    return queue;
}

// a datagram the prefilter accepts: a payload packet of bytes with tag at
// byte 20, past every byte the prefilter reads
static int receive_queue_packet( uint8_t * packet, int bytes, uint8_t type, uint8_t tag )
{
    memset( packet, 0, (size_t) bytes );
    packet[0] = type;
    packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;
    memcpy( packet + 2, receive_queue_filter, ROCKETNET_FILTER_VALUE_BYTES );
    if ( bytes > 20 )
    {
        packet[20] = tag;
    }
    return bytes;
}

static int receive_queue_name( struct sockaddr_storage * name, uint16_t port )
{
    memset( name, 0, sizeof( *name ) );
    struct sockaddr_in * in = (struct sockaddr_in *) name;
    in->sin_family = AF_INET;
    in->sin_port = htons( port );
    in->sin_addr.s_addr = htonl( 0xC6336407 );       // 198.51.100.7
    return (int) sizeof( struct sockaddr_in );
}

static uint32_t receive_queue_flags( int buffer_id, bool more )
{
    return IORING_CQE_F_BUFFER | ( (uint32_t) buffer_id << IORING_CQE_BUFFER_SHIFT ) | ( more ? IORING_CQE_F_MORE : 0 );
}

// the fake kernel: takes the next buffer, writes payload from port into it
// and hands the worker its completion. Returns the datagrams banked.
static int receive_queue_deliver( struct rocketnet_uring_receive_queue_t * queue, uint16_t * ring_head, const uint8_t * payload, int payload_bytes, int segment_bytes = 0,
                                  uint32_t socket_drops = 0, uint32_t out_flags = 0, uint16_t port = 40000 )
{
    const int buffer_id = rocketnet_uring_receive_queue_test_take( queue, ring_head );
    check( buffer_id >= 0 );
    struct sockaddr_storage name;
    const int name_bytes = receive_queue_name( &name, port );
    const int res = rocketnet_uring_receive_queue_test_write( queue, buffer_id, &name, name_bytes, payload, payload_bytes, segment_bytes, socket_drops, out_flags );
    return rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( buffer_id, true ) );
}

static uint64_t receive_queue_counter( const struct rocketnet_uring_receive_queue_t * queue, int counter )
{
    uint64_t counters[ROCKETNET_URING_RECEIVE_NUM_COUNTERS];
    rocketnet_uring_receive_queue_counters( queue, counters );
    return counters[counter];
}

static uint64_t receive_queue_drop( const struct rocketnet_uring_receive_queue_t * queue, int drop )
{
    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS];
    memset( totals, 0, sizeof( totals ) );
    rocketnet_uring_receive_queue_drops( queue, totals );
    return totals[drop];
}

static uint16_t receive_queue_ring_tail( const struct rocketnet_uring_receive_queue_t * queue )
{
    return __atomic_load_n( &queue->buf_ring->tail, __ATOMIC_ACQUIRE );
}

// ---------------------------------------------------------------------------

// create refuses every size out of range, and a queue it makes gives every
// buffer to the ring: buffer b at entry b, the tail at buffers

static void test_uring_receive_queue_create_refuses_bad_sizes()
{
    struct rocketnet_uring_receive_queue_config_t config = receive_queue_config( 4, 3, false, false );

    const int bad_buffers[] = { 0, -1, 3, 6, ROCKETNET_URING_RECEIVE_MAX_BUFFERS * 2 };
    for ( size_t i = 0; i < sizeof( bad_buffers ) / sizeof( bad_buffers[0] ); i++ )
    {
        struct rocketnet_uring_receive_queue_config_t bad = config;
        bad.buffers = bad_buffers[i];
        check( rocketnet_uring_receive_queue_create( NULL, NULL, &bad ) == NULL );
        check( rocketnet_uring_receive_queue_allocation_bytes( &bad ) == 0 );
    }

    const int bad_slots[] = { 0, -5, ROCKETNET_URING_RECEIVE_MAX_SLOTS + 1 };
    for ( size_t i = 0; i < sizeof( bad_slots ) / sizeof( bad_slots[0] ); i++ )
    {
        struct rocketnet_uring_receive_queue_config_t bad = config;
        bad.slots = bad_slots[i];
        check( rocketnet_uring_receive_queue_create( NULL, NULL, &bad ) == NULL );
    }

    // a buffer under one datagram; with GRO, under one coalesced receive
    struct rocketnet_uring_receive_queue_config_t small = config;
    small.buffer_bytes = ROCKETNET_RECEIVE_CEILING;
    check( rocketnet_uring_receive_queue_create( NULL, NULL, &small ) == NULL );
    struct rocketnet_uring_receive_queue_config_t small_gro = config;
    small_gro.gro = true;
    check( rocketnet_uring_receive_queue_create( NULL, NULL, &small_gro ) == NULL );

    struct rocketnet_uring_receive_queue_config_t bad_index = config;
    bad_index.queue_index = 0x10000;
    check( rocketnet_uring_receive_queue_create( NULL, NULL, &bad_index ) == NULL );
    struct rocketnet_uring_receive_queue_config_t bad_group = config;
    bad_group.bgid = -1;
    check( rocketnet_uring_receive_queue_create( NULL, NULL, &bad_group ) == NULL );
    struct rocketnet_uring_receive_queue_config_t no_filter = config;
    no_filter.packet_filter = NULL;
    // no filter is the raw-datagram mode (the size pass alone), not a refusal
    struct rocketnet_uring_receive_queue_t * unfiltered = rocketnet_uring_receive_queue_create( NULL, NULL, &no_filter );
    check( unfiltered != NULL );
    if ( unfiltered )
    {
        rocketnet_uring_receive_queue_destroy( unfiltered );
    }

    check( rocketnet_uring_receive_queue_allocation_bytes( &config ) > 0 );
    check( rocketnet_uring_receive_queue_mapped_bytes( 0 ) == 0 );
    check( rocketnet_uring_receive_queue_mapped_bytes( ROCKETNET_URING_RECEIVE_MAX_BUFFERS + 1 ) == 0 );
    check( rocketnet_uring_receive_queue_mapped_bytes( 1 ) > 0 );
    check( rocketnet_uring_receive_queue_mapped_bytes( 1 ) % 4096 == 0 );

    struct rocketnet_uring_receive_queue_t * queue = rocketnet_uring_receive_queue_create( NULL, NULL, &config );
    check( queue );
    check( receive_queue_ring_tail( queue ) == 4 );
    const struct io_uring_buf * bufs = (const struct io_uring_buf *) (const void *) queue->buf_ring;
    for ( int b = 0; b < 4; b++ )
    {
        check( bufs[b].bid == b );
        check( bufs[b].len == (uint32_t) RECEIVE_QUEUE_PLAIN_BYTES );
        check( bufs[b].addr == (uint64_t) (uintptr_t) ( queue->buffer + (size_t) b * RECEIVE_QUEUE_PLAIN_BYTES ) );
    }
    check( queue->receive_message.msg_namelen == sizeof( struct sockaddr_storage ) );
    check( queue->receive_message.msg_controllen == 0 );
    rocketnet_uring_receive_queue_destroy( queue );

    rocketnet_uring_receive_queue_destroy( NULL );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_create_refuses_bad_sizes );

// the arm, the multishot's end and the re-arm: an arm with no submission
// entry is counted and the queue still needs one; each completion without
// F_MORE ends the multishot, whatever its result; a stopped queue never
// needs an arm again (RNUringQueueReceive Arm, Complete, Rearms)

static void test_uring_receive_queue_arm_and_rearm()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 4, false, false );

    check( rocketnet_uring_receive_queue_needs_arm( queue ) );
    check( !rocketnet_uring_receive_queue_armed( queue ) );

    check( !rocketnet_uring_receive_queue_arm( queue, NULL ) );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_REARM_FAILED ) == 1 );
    check( rocketnet_uring_receive_queue_needs_arm( queue ) );

    struct io_uring_sqe sqe;
    memset( &sqe, 0xFF, sizeof( sqe ) );
    check( rocketnet_uring_receive_queue_arm( queue, &sqe ) );
    check( sqe.opcode == IORING_OP_RECVMSG );
    check( sqe.fd == 17 );
    check( sqe.addr == (uint64_t) (uintptr_t) &queue->receive_message );
    check( sqe.len == 1 );
    check( sqe.ioprio == IORING_RECV_MULTISHOT );
    check( sqe.flags == IOSQE_BUFFER_SELECT );
    check( sqe.buf_group == 9 );
    check( sqe.msg_flags == 0 );
    check( sqe.off == 0 );
    check( sqe.personality == 0 );
    check( sqe.user_data == rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 3, 0 ) );
    check( rocketnet_uring_receive_queue_armed( queue ) );
    check( !rocketnet_uring_receive_queue_needs_arm( queue ) );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_REARMS ) == 0 );

    // a completion with an error and F_MORE: counted, still armed
    check( rocketnet_uring_receive_queue_cqe( queue, -EIO, IORING_CQE_F_MORE ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_ERRORS ) == 1 );
    check( rocketnet_uring_receive_queue_armed( queue ) );

    // the refused receive reaches the server's RECEIVE_FAILED through the drop totals
    uint64_t drops[ROCKETNET_RECEIVE_NUM_DROPS];
    memset( drops, 0, sizeof( drops ) );
    rocketnet_uring_receive_queue_drops( queue, drops );
    check( drops[ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED] == 1 );

    // ENOBUFS ends it: counted, needs an arm
    check( rocketnet_uring_receive_queue_cqe( queue, -ENOBUFS, 0 ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_NO_BUFFERS ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_MULTISHOT_ENDED ) == 1 );
    check( rocketnet_uring_receive_queue_needs_arm( queue ) );

    check( rocketnet_uring_receive_queue_arm( queue, &sqe ) );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_REARMS ) == 1 );

    // a refusal ends it too, counted ERRORS; a datagram completion without
    // F_MORE banks its datagram and ends it
    check( rocketnet_uring_receive_queue_cqe( queue, -EINVAL, 0 ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_ERRORS ) == 2 );
    check( rocketnet_uring_receive_queue_needs_arm( queue ) );
    check( rocketnet_uring_receive_queue_arm( queue, &sqe ) );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_REARMS ) == 2 );

    uint16_t ring_head = 0;
    uint8_t packet[64];
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );
    const int buffer_id = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
    struct sockaddr_storage name;
    const int name_bytes = receive_queue_name( &name, 40000 );
    const int res = rocketnet_uring_receive_queue_test_write( queue, buffer_id, &name, name_bytes, packet, 64, 0, 0, 0 );
    check( rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( buffer_id, false ) ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_MULTISHOT_ENDED ) == 3 );
    check( rocketnet_uring_receive_queue_needs_arm( queue ) );

    // the stop: the cancel's ECANCELED ends it, no error, and no arm again
    check( rocketnet_uring_receive_queue_arm( queue, &sqe ) );
    rocketnet_uring_receive_queue_stop( queue );
    check( !rocketnet_uring_receive_queue_needs_arm( queue ) );
    check( rocketnet_uring_receive_queue_cqe( queue, -ECANCELED, 0 ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_ERRORS ) == 2 );
    check( !rocketnet_uring_receive_queue_armed( queue ) );
    check( !rocketnet_uring_receive_queue_needs_arm( queue ) );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_MULTISHOT_ENDED ) == 4 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_arm_and_rearm );

// a datagram is banked with its source, bytes and wire bytes, handed to
// main by a drain, and its buffer goes back to the ring at the tail's entry

static void test_uring_receive_queue_banks_and_drains()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 4, false, false );
    uint16_t ring_head = 0;

    uint8_t packet[100];
    receive_queue_packet( packet, 100, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 42 );
    check( receive_queue_deliver( queue, &ring_head, packet, 100, 0, 0, 0, 40123 ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_BANKED ) == 1 );
    check( rocketnet_uring_receive_queue_waiting( queue ) == 1 );

    // buffer 0 is back, at entry 4 & 3 = 0
    check( receive_queue_ring_tail( queue ) == 5 );
    const struct io_uring_buf * bufs = (const struct io_uring_buf *) (const void *) queue->buf_ring;
    check( bufs[0].bid == 0 );

    struct rocketnet_address_t from[4];
    int bytes[4];
    uint8_t * data[4];
    check( rocketnet_uring_receive_queue_drain( queue, 4, from, bytes, data ) == 1 );
    check( from[0].type == ROCKETNET_ADDRESS_IPV4 );
    check( from[0].port == 40123 );
    check( from[0].data.ipv4[0] == 198 && from[0].data.ipv4[3] == 7 );
    check( bytes[0] == 100 );
    check( memcmp( data[0], packet, 100 ) == 0 );
    check( rocketnet_uring_receive_queue_waiting( queue ) == 0 );

    // max 0 releases and takes nothing
    check( rocketnet_uring_receive_queue_drain( queue, 0, NULL, NULL, NULL ) == 0 );
    check( queue->head.load() == 1 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_banks_and_drains );

// the bank is full at slots positions past head: the datagram is counted
// BANK_FULL, never written; the slots a drain hands main stay theirs until
// the next drain, so the worker drops while main reads them; the next drain
// releases them (RNUringQueueReceiveLayout BankOne, Drain)

static void test_uring_receive_queue_full_bank_drops_by_name()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 2, false, false );
    uint16_t ring_head = 0;
    uint8_t packet[64];

    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );
    check( receive_queue_deliver( queue, &ring_head, packet, 64 ) == 1 );
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 2 );
    check( receive_queue_deliver( queue, &ring_head, packet, 64 ) == 1 );
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3 );
    check( receive_queue_deliver( queue, &ring_head, packet, 64 ) == 0 );
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_BANK_FULL ) == 1 );

    struct rocketnet_address_t from[2];
    int bytes[2];
    uint8_t * data[2];
    check( rocketnet_uring_receive_queue_drain( queue, 2, from, bytes, data ) == 2 );
    check( data[0][20] == 1 && data[1][20] == 2 );

    // main is reading both slots: the worker may not write either
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 4 );
    check( receive_queue_deliver( queue, &ring_head, packet, 64 ) == 0 );
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_BANK_FULL ) == 2 );
    check( data[0][20] == 1 && data[1][20] == 2 );

    // the next drain releases them; the worker loads head again and banks
    check( rocketnet_uring_receive_queue_drain( queue, 0, NULL, NULL, NULL ) == 0 );
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 5 );
    check( receive_queue_deliver( queue, &ring_head, packet, 64 ) == 1 );
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_BANK_FULL ) == 2 );
    check( rocketnet_uring_receive_queue_drain( queue, 2, from, bytes, data ) == 1 );
    check( data[0][20] == 5 );
    check( data[0] == queue->slot_data );       // position 2, slot 0

    // every buffer went back
    check( (uint16_t) ( receive_queue_ring_tail( queue ) - ring_head ) == 4 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_full_bank_drops_by_name );

// drains of at most max, oldest first, across the ring's wrap: every
// datagram is read once, in the order it was banked

static void test_uring_receive_queue_drain_in_order_across_the_wrap()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 3, false, false );
    uint16_t ring_head = 0;
    uint8_t packet[64];
    uint8_t next = 1;
    uint8_t expect = 1;
    uint64_t arrived = 0;

    struct rocketnet_address_t from[3];
    int bytes[3];
    uint8_t * data[3];

    for ( int round = 0; round < 40; round++ )
    {
        const int arrivals = round % 3 + 1;
        for ( int i = 0; i < arrivals; i++ )
        {
            arrived++;
            receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, next );
            if ( receive_queue_deliver( queue, &ring_head, packet, 64 ) == 1 )
            {
                next++;
            }
        }
        const int n = rocketnet_uring_receive_queue_drain( queue, 2, from, bytes, data );
        check( n <= 2 );
        for ( int i = 0; i < n; i++ )
        {
            check( data[i][20] == expect );
            check( bytes[i] == 64 );
            expect++;
        }
    }

    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_BANKED ) == (uint64_t) ( next - 1 ) );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_BANKED ) + receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_BANK_FULL ) == arrived );
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_BANK_FULL ) > 0 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_drain_in_order_across_the_wrap );

// the transport's refusals, each counted by name, each buffer given back

static void test_uring_receive_queue_transport_refusals()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 4, false, false );
    uint16_t ring_head = 0;
    uint8_t packet[2000];

    // a completion that names no buffer, or a buffer outside the ring
    check( rocketnet_uring_receive_queue_cqe( queue, 100, IORING_CQE_F_MORE ) == 0 );
    check( rocketnet_uring_receive_queue_cqe( queue, 100, receive_queue_flags( 4, true ) ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_NO_BUFFER ) == 2 );
    check( receive_queue_ring_tail( queue ) == 4 );

    // a buffer shorter than the header
    int buffer_id = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
    check( rocketnet_uring_receive_queue_cqe( queue, 8, receive_queue_flags( buffer_id, true ) ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE ) == 0 );

    // truncated by the kernel: too large
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );
    check( receive_queue_deliver( queue, &ring_head, packet, 64, 0, 0, MSG_TRUNC ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE ) == 2 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE ) == 1 );

    // past the receive ceiling, and under the server's smallest packet
    receive_queue_packet( packet, 2000, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );
    check( receive_queue_deliver( queue, &ring_head, packet, ROCKETNET_RECEIVE_CEILING + 1 ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE ) == 3 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE ) == 2 );
    check( receive_queue_deliver( queue, &ring_head, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1 ) == 0 );
    check( receive_queue_deliver( queue, &ring_head, packet, 0 ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE ) == 5 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE ) == 2 );

    // the prefilter's own size rungs are a strict suffix: the transport's
    // refusal counted the datagram, the tally never sees it
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_TOO_SHORT ) == 0 );
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_TOO_LARGE ) == 0 );

    // a name the queue did not reserve room for, another family, a short name
    struct sockaddr_storage name;
    receive_queue_name( &name, 40000 );
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );
    buffer_id = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
    int res = rocketnet_uring_receive_queue_test_write( queue, buffer_id, &name, (int) sizeof( name ) + 1, packet, 64, 0, 0, 0 );
    check( rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( buffer_id, true ) ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE ) == 1 );

    name.ss_family = AF_UNIX;
    buffer_id = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
    res = rocketnet_uring_receive_queue_test_write( queue, buffer_id, &name, (int) sizeof( struct sockaddr_in ), packet, 64, 0, 0, 0 );
    check( rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( buffer_id, true ) ) == 0 );

    receive_queue_name( &name, 40000 );
    buffer_id = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
    res = rocketnet_uring_receive_queue_test_write( queue, buffer_id, &name, 4, packet, 64, 0, 0, 0 );
    check( rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( buffer_id, true ) ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE ) == 3 );

    // nothing was banked, and every buffer the kernel took went back
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_BANKED ) == 0 );
    check( (uint16_t) ( receive_queue_ring_tail( queue ) - ring_head ) == 4 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_transport_refusals );

// every prefilter reason a datagram of the server's smallest size or more
// can meet is counted in the tally by name, exactly once, and banks nothing

static void test_uring_receive_queue_prefilter_drops_by_name()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 4, false, false );
    uint16_t ring_head = 0;
    uint8_t packet[1024];

    struct case_t
    {
        uint8_t type;
        int bytes;
        int epoch;                  // -1: the right one
        bool wrong_value;
        enum rocketnet_receive_drop_t drop;
    };

    const case_t cases[] = {
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET, 64, 7, false, ROCKETNET_RECEIVE_DROP_WRONG_EPOCH },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET, 64, -1, true, ROCKETNET_RECEIVE_DROP_WRONG_VALUE },
        { ROCKETNET_FIRST_RESERVED_PACKET_TYPE, 64, -1, false, ROCKETNET_RECEIVE_DROP_RESERVED_TYPE },
        { 255, 64, -1, false, ROCKETNET_RECEIVE_DROP_RESERVED_TYPE },
        { ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES, -1, false, ROCKETNET_RECEIVE_DROP_WRONG_DIRECTION },
        { ROCKETNET_CONNECTION_CHALLENGE_PACKET, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES, -1, false, ROCKETNET_RECEIVE_DROP_WRONG_DIRECTION },
        { ROCKETNET_CONNECTION_COOKIE_PACKET, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES, -1, false, ROCKETNET_RECEIVE_DROP_WRONG_DIRECTION },
        { ROCKETNET_CONNECTION_REQUEST_PACKET, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1, -1, false, ROCKETNET_RECEIVE_DROP_REQUEST_WRONG_SIZE },
        { ROCKETNET_CONNECTION_RESPONSE_PACKET, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES + 1, -1, false, ROCKETNET_RECEIVE_DROP_RESPONSE_WRONG_SIZE },
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES - 1, -1, false, ROCKETNET_RECEIVE_DROP_KEEP_ALIVE_WRONG_SIZE },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES - 1, -1, false, ROCKETNET_RECEIVE_DROP_PAYLOAD_WRONG_SIZE },
    };

    uint64_t expected[ROCKETNET_RECEIVE_NUM_DROPS];
    memset( expected, 0, sizeof( expected ) );

    for ( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
    {
        check( cases[i].bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
        receive_queue_packet( packet, cases[i].bytes, cases[i].type, 1 );
        if ( cases[i].epoch >= 0 )
        {
            packet[1] = (uint8_t) cases[i].epoch;
        }
        if ( cases[i].wrong_value )
        {
            packet[3] ^= 0x01;
        }
        check( receive_queue_deliver( queue, &ring_head, packet, cases[i].bytes ) == 0 );
        expected[cases[i].drop]++;
    }

    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS];
    memset( totals, 0, sizeof( totals ) );
    rocketnet_uring_receive_queue_drops( queue, totals );
    for ( int d = 0; d < ROCKETNET_RECEIVE_NUM_DROPS; d++ )
    {
        check( totals[d] == expected[d] );
    }

    // each type the server takes, at its size, is accepted and banked
    const uint8_t accepted[] = { ROCKETNET_CONNECTION_REQUEST_PACKET, ROCKETNET_CONNECTION_RESPONSE_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                 ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_ACK_PACKET };
    const int sizes[] = { ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES,
                          ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES };
    struct rocketnet_address_t from[4];
    int bytes[4];
    uint8_t * data[4];
    for ( size_t i = 0; i < sizeof( accepted ); i++ )
    {
        receive_queue_packet( packet, sizes[i], accepted[i], 1 );
        check( receive_queue_deliver( queue, &ring_head, packet, sizes[i] ) == 1 );
        check( rocketnet_uring_receive_queue_drain( queue, 4, from, bytes, data ) == 1 );
        check( bytes[0] == sizes[i] );
    }

    // the two reasons whose wire floor is the server's smallest packet are
    // the transport's size refusal instead: the tally never sees them
    check( ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES <= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    check( ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES <= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    receive_queue_packet( packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1, ROCKETNET_CONNECTION_ACK_PACKET, 1 );
    check( receive_queue_deliver( queue, &ring_head, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1 ) == 0 );
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_ACK_WRONG_SIZE ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE ) == 1 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_prefilter_drops_by_name );

// a UDP_GRO coalesced receive: each segment banked or counted on its own,
// a short tail, the segments past the most one receive is taken with
// counted DROPPED_OVERFLOW, and a truncated control area taken as one
// datagram and counted GRO_CMSG_FAULTS

static void test_uring_receive_queue_coalesced_receive()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 128, true, false );
    uint16_t ring_head = 0;
    static uint8_t payload[ROCKETNET_URING_RECEIVE_MAX_SEGMENTS * 2 * 64];

    // three segments of 64, the third with a wrong filter value, and a tail of 40
    for ( int i = 0; i < 4; i++ )
    {
        receive_queue_packet( payload + i * 64, i < 3 ? 64 : 40, ROCKETNET_CONNECTION_PAYLOAD_PACKET, (uint8_t) ( 10 + i ) );
    }
    payload[2 * 64 + 3] ^= 0x01;
    check( receive_queue_deliver( queue, &ring_head, payload, 3 * 64 + 40, 64 ) == 3 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_GRO_BATCHES ) == 1 );
    check( receive_queue_drop( queue, ROCKETNET_RECEIVE_DROP_WRONG_VALUE ) == 1 );

    struct rocketnet_address_t from[128];
    int bytes[128];
    uint8_t * data[128];
    check( rocketnet_uring_receive_queue_drain( queue, 128, from, bytes, data ) == 3 );
    check( data[0][20] == 10 && data[1][20] == 11 && data[2][20] == 13 );
    check( bytes[0] == 64 && bytes[1] == 64 && bytes[2] == 40 );

    // one more segment than one receive is taken with
    const int segments = ROCKETNET_URING_RECEIVE_MAX_SEGMENTS + 1;
    for ( int i = 0; i < segments; i++ )
    {
        receive_queue_packet( payload + i * 64, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, (uint8_t) i );
    }
    check( receive_queue_deliver( queue, &ring_head, payload, segments * 64, 64 ) == ROCKETNET_URING_RECEIVE_MAX_SEGMENTS );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_OVERFLOW ) == 1 );
    check( rocketnet_uring_receive_queue_drain( queue, 128, from, bytes, data ) == ROCKETNET_URING_RECEIVE_MAX_SEGMENTS );

    // a truncated control area: one datagram of the whole payload
    receive_queue_packet( payload, 200, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 77 );
    check( receive_queue_deliver( queue, &ring_head, payload, 200, 64, 0, MSG_CTRUNC ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_GRO_CMSG_FAULTS ) == 1 );
    check( rocketnet_uring_receive_queue_drain( queue, 128, from, bytes, data ) == 1 );
    check( bytes[0] == 200 && data[0][20] == 77 );

    // a coalesced receive that meets a full bank banks what fits, and counts
    // each segment past it BANK_FULL
    struct rocketnet_uring_receive_queue_t * small = receive_queue_create( 4, 2, true, false );
    uint16_t small_head = 0;
    for ( int i = 0; i < 5; i++ )
    {
        receive_queue_packet( payload + i * 64, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, (uint8_t) i );
    }
    check( receive_queue_deliver( small, &small_head, payload, 5 * 64, 64 ) == 2 );
    check( receive_queue_drop( small, ROCKETNET_RECEIVE_DROP_BANK_FULL ) == 3 );
    rocketnet_uring_receive_queue_destroy( small );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_coalesced_receive );

// the socket's own drops (SO_RXQ_OVFL): each receive carries the socket's
// count when its datagram was queued; the growth is counted
// DROPPED_SOCKET, whatever becomes of the datagram, across the count's
// 32-bit wrap, and never from a truncated control area

static void test_uring_receive_queue_counts_socket_drops()
{
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 8, false, true );
    uint16_t ring_head = 0;
    uint8_t packet[64];
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );

    check( queue->receive_message.msg_controllen == CMSG_SPACE( sizeof( uint32_t ) ) );

    check( receive_queue_deliver( queue, &ring_head, packet, 64 ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET ) == 0 );

    check( receive_queue_deliver( queue, &ring_head, packet, 64, 0, 5 ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET ) == 5 );
    check( receive_queue_deliver( queue, &ring_head, packet, 64, 0, 5 ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET ) == 5 );

    // counted though the prefilter refuses the datagram that carries it
    uint8_t refused[64];
    receive_queue_packet( refused, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );
    refused[1] = 9;
    check( receive_queue_deliver( queue, &ring_head, refused, 64, 0, 8 ) == 0 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET ) == 8 );

    // a truncated control area is not read
    check( receive_queue_deliver( queue, &ring_head, packet, 64, 0, 20, MSG_CTRUNC ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET ) == 8 );

    // the 32-bit wrap
    queue->socket_drops_seen = 0xFFFFFFFEu;
    check( receive_queue_deliver( queue, &ring_head, packet, 64, 0, 1 ) == 1 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET ) == 11 );

    rocketnet_uring_receive_queue_destroy( queue );

    // a queue whose socket does not count reserves no control area
    struct rocketnet_uring_receive_queue_t * plain = receive_queue_create( 4, 8, false, false );
    uint16_t plain_head = 0;
    check( receive_queue_deliver( plain, &plain_head, packet, 64, 0, 5 ) == 1 );
    check( receive_queue_counter( plain, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET ) == 0 );
    rocketnet_uring_receive_queue_destroy( plain );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_counts_socket_drops );

// an ipv6 socket: a native ipv6 source, and an ipv4 sender as a
// v4-mapped source that becomes native ipv4

static void test_uring_receive_queue_ipv6_sources()
{
    struct rocketnet_uring_receive_queue_config_t config = receive_queue_config( 4, 4, false, false );
    config.ipv6 = true;
    struct rocketnet_uring_receive_queue_t * queue = rocketnet_uring_receive_queue_create( NULL, NULL, &config );
    check( queue );
    uint16_t ring_head = 0;
    uint8_t packet[64];
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );

    struct sockaddr_storage name;
    memset( &name, 0, sizeof( name ) );
    struct sockaddr_in6 * in6 = (struct sockaddr_in6 *) &name;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 50000 );
    in6->sin6_addr.s6_addr[0] = 0x20;
    in6->sin6_addr.s6_addr[1] = 0x01;
    in6->sin6_addr.s6_addr[15] = 0x09;

    int buffer_id = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
    int res = rocketnet_uring_receive_queue_test_write( queue, buffer_id, &name, (int) sizeof( struct sockaddr_in6 ), packet, 64, 0, 0, 0 );
    check( rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( buffer_id, true ) ) == 1 );

    memset( in6->sin6_addr.s6_addr, 0, 16 );
    in6->sin6_addr.s6_addr[10] = 0xFF;
    in6->sin6_addr.s6_addr[11] = 0xFF;
    in6->sin6_addr.s6_addr[12] = 10;
    in6->sin6_addr.s6_addr[15] = 4;
    buffer_id = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
    res = rocketnet_uring_receive_queue_test_write( queue, buffer_id, &name, (int) sizeof( struct sockaddr_in6 ), packet, 64, 0, 0, 0 );
    check( rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( buffer_id, true ) ) == 1 );

    struct rocketnet_address_t from[4];
    int bytes[4];
    uint8_t * data[4];
    check( rocketnet_uring_receive_queue_drain( queue, 4, from, bytes, data ) == 2 );
    check( from[0].type == ROCKETNET_ADDRESS_IPV6 );
    check( from[0].port == 50000 );
    check( from[0].data.ipv6[0] == 0x2001 && from[0].data.ipv6[7] == 0x0009 );
    check( from[1].type == ROCKETNET_ADDRESS_IPV4 );
    check( from[1].data.ipv4[0] == 10 && from[1].data.ipv4[3] == 4 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_ipv6_sources );

// the buffer ring's 16-bit tail wraps: entry tail & (buffers - 1) keeps
// naming the buffer last given back, so the kernel never takes one twice
// (RNUringQueueReceiveLayout NoDoubleTake, past TailWrap)

static void test_uring_receive_queue_buffer_ring_wraps()
{
    // a bank of two drains' worth: the three slots main reads stay out of
    // the worker's reach until the next drain
    struct rocketnet_uring_receive_queue_t * queue = receive_queue_create( 4, 6, false, false );
    uint16_t ring_head = 0;
    uint8_t packet[64];
    receive_queue_packet( packet, 64, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1 );

    struct rocketnet_address_t from[4];
    int bytes[4];
    uint8_t * data[4];
    bool held[4] = { false, false, false, false };
    int taken[4];

    for ( int round = 0; round < 70000 / 3; round++ )
    {
        // the kernel takes three buffers before the worker hands any back
        for ( int i = 0; i < 3; i++ )
        {
            taken[i] = rocketnet_uring_receive_queue_test_take( queue, &ring_head );
            check( taken[i] >= 0 && taken[i] < 4 );
            check( !held[taken[i]] );
            held[taken[i]] = true;
        }
        for ( int i = 0; i < 3; i++ )
        {
            struct sockaddr_storage name;
            const int name_bytes = receive_queue_name( &name, 40000 );
            const int res = rocketnet_uring_receive_queue_test_write( queue, taken[i], &name, name_bytes, packet, 64, 0, 0, 0 );
            check( rocketnet_uring_receive_queue_cqe( queue, res, receive_queue_flags( taken[i], true ) ) == 1 );
            held[taken[i]] = false;
        }
        check( rocketnet_uring_receive_queue_drain( queue, 4, from, bytes, data ) == 3 );
    }

    check( (uint16_t) ( receive_queue_ring_tail( queue ) - ring_head ) == 4 );
    check( receive_queue_counter( queue, ROCKETNET_URING_RECEIVE_COUNTER_BANKED ) == (uint64_t) ( 70000 / 3 ) * 3 );

    rocketnet_uring_receive_queue_destroy( queue );
}

REGISTER_UNIT_TEST( test_uring_receive_queue_buffer_ring_wraps );

#endif // #if defined( __linux__ ) && ROCKETNET_DEV && defined( IORING_RECV_MULTISHOT )

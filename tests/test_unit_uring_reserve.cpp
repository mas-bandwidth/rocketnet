/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring transport's send reservation (rocketnet_server_uring_start_packet)
// on a fake transport without sockets, a kernel ring or a clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_uring.h"

#if defined( __linux__ )
#include "../sdk/rocketnet_uring_codec.h"
#endif // #if defined( __linux__ )

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )

#include <string.h>
#include <errno.h>

// A caller that has taken 2^32 - 1 reservations without a flush is refused
// the next two: the reservation count does not wrap back onto a slot it
// handed out.

static void test_io_uring_transport_reservation_does_not_wrap()
{
    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create_fake( 1 );
    rocketnet_unit_check( uring != NULL );
    if ( !uring )
        return;

    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40001;

    for ( int phase = 0; phase < ROCKETNET_URING_SEND_PHASES; ++phase )
    {
        rocketnet_server_uring_test_set_reserved( uring, phase, 0xFFFFFFFFull );

        uint64_t packet_id = 0;
        rocketnet_unit_check( rocketnet_server_uring_start_packet( uring, &to, &packet_id ) == NULL );
        rocketnet_unit_check( rocketnet_server_uring_start_packet( uring, &to, &packet_id ) == NULL );
        rocketnet_unit_check( rocketnet_server_uring_send_room( uring ) == 0 );
        rocketnet_unit_check( !rocketnet_server_uring_send_idle( uring ) );
        rocketnet_server_uring_test_set_reserved( uring, phase, 0 );
    }

    rocketnet_server_uring_test_destroy_fake( uring );
}

// The fake completion source of the send-spread cases: a send completes at
// once on the socket named by its tag, and a socket with a finite capacity
// answers a send past it with -EAGAIN (a datagram the socket's send buffer
// could not take). No kernel ring, no socket, no clock.

#define URING_RESERVE_FAKE_CAPACITY 256

struct uring_reserve_fake_t
{
    struct rocketnet_server_uring_test_completion_t fifo[URING_RESERVE_FAKE_CAPACITY];
    int head;
    int tail;
    int send_capacity;                          // sends one socket takes in this burst; zero: unbounded
    int sends_on_socket[ROCKETNET_URING_MAX_QUEUES];
    int sends;
};

static int uring_reserve_fake_size( struct uring_reserve_fake_t * fake )
{
    return fake->tail - fake->head;
}

static void uring_reserve_fake_push( struct uring_reserve_fake_t * fake, uint64_t user_data, int32_t res, uint32_t flags )
{
    rocketnet_unit_check( uring_reserve_fake_size( fake ) < URING_RESERVE_FAKE_CAPACITY );

    struct rocketnet_server_uring_test_completion_t * completion = &fake->fifo[fake->tail % URING_RESERVE_FAKE_CAPACITY];
    completion->user_data = user_data;
    completion->res = res;
    completion->flags = flags;
    fake->tail++;
}

static int uring_reserve_fake_peek( void * context, struct rocketnet_server_uring_test_completion_t * completions, int max )
{
    struct uring_reserve_fake_t * fake = (struct uring_reserve_fake_t *) context;

    int count = uring_reserve_fake_size( fake );
    if ( count > max )
        count = max;

    for ( int i = 0; i < count; i++ )
        completions[i] = fake->fifo[( fake->head + i ) % URING_RESERVE_FAKE_CAPACITY];

    return count;
}

static void uring_reserve_fake_advance( void * context, int count )
{
    struct uring_reserve_fake_t * fake = (struct uring_reserve_fake_t *) context;
    rocketnet_unit_check( count >= 0 && count <= uring_reserve_fake_size( fake ) );
    fake->head += count;
}

static int uring_reserve_fake_ready( void * context )
{
    struct uring_reserve_fake_t * fake = (struct uring_reserve_fake_t *) context;
    return uring_reserve_fake_size( fake );
}

static void uring_reserve_fake_submitted( void * context, uint64_t user_data )
{
    struct uring_reserve_fake_t * fake = (struct uring_reserve_fake_t *) context;
    const int kind = rocketnet_uring_tag_kind( user_data );
    const int queue = rocketnet_uring_tag_queue( user_data );

    if ( kind == ROCKETNET_URING_TAG_SEND )
    {
        rocketnet_unit_check( queue >= 0 && queue < ROCKETNET_URING_MAX_QUEUES );
        fake->sends++;
        fake->sends_on_socket[queue]++;

        const int32_t res = ( fake->send_capacity > 0 && fake->sends_on_socket[queue] > fake->send_capacity ) ? -EAGAIN : 64;
        uring_reserve_fake_push( fake, user_data, res, 0 );
    }
    else if ( kind == ROCKETNET_URING_TAG_CANCEL )
    {
        uring_reserve_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue, 0 ), -ECANCELED, 0 );
        uring_reserve_fake_push( fake, user_data, 0, 0 );
    }
}

static void uring_reserve_fake_create( struct uring_reserve_fake_t * fake, struct rocketnet_server_uring_test_source_t * source )
{
    memset( fake, 0, sizeof( *fake ) );
    source->context = fake;
    source->peek = uring_reserve_fake_peek;
    source->advance = uring_reserve_fake_advance;
    source->ready = uring_reserve_fake_ready;
    source->submitted = uring_reserve_fake_submitted;
}

static void uring_reserve_fake_stop_and_destroy( struct rocketnet_server_uring_t * uring )
{
    rocketnet_server_uring_stop( uring );

    bool stopped = false;
    for ( int i = 0; i < 64 && !stopped; ++i )
        stopped = rocketnet_server_uring_stopped( uring );

    rocketnet_unit_check( stopped );
    rocketnet_server_uring_destroy( uring );
}

static void uring_reserve_stage_sends( struct rocketnet_server_uring_t * uring, int count )
{
    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40001;

    for ( int i = 0; i < count; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_uring_start_packet( uring, &to, &packet_id );
        rocketnet_unit_check( frame != NULL );
        if ( !frame )
            return;

        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, (uint8_t) i, 64 );
        rocketnet_server_uring_finish_packet( uring, packet_id, frame, 64 );
    }
}

// A burst staged in one chunk takes the next socket for every send it
// submits, not one socket for the whole chunk.

static void test_io_uring_send_burst_spreads_across_socket_pool()
{
    struct rocketnet_server_uring_test_source_t source;
    struct uring_reserve_fake_t fake;
    uring_reserve_fake_create( &fake, &source );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 2, &source );
    rocketnet_unit_check( uring != NULL );
    if ( !uring )
        return;

    const int sends = 8;
    uring_reserve_stage_sends( uring, sends );
    rocketnet_server_uring_send_packets( uring );

    int used = 0;
    for ( int queue = 0; queue < 2; queue++ )
    {
        if ( fake.sends_on_socket[queue] > 0 )
            used++;
    }

    rocketnet_unit_check( fake.sends == sends );
    rocketnet_unit_check( used == 2 );

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == (uint64_t) sends );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == 0 );

    uring_reserve_fake_stop_and_destroy( uring );
}

// A burst larger than one socket's send buffer does not drop datagrams while
// the sibling sockets of the pool have room.

static void test_io_uring_send_burst_beyond_one_socket_buffer_uses_siblings()
{
    struct rocketnet_server_uring_test_source_t source;
    struct uring_reserve_fake_t fake;
    uring_reserve_fake_create( &fake, &source );
    fake.send_capacity = 4;

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 2, &source );
    rocketnet_unit_check( uring != NULL );
    if ( !uring )
        return;

    const int sends = 8;
    uring_reserve_stage_sends( uring, sends );
    rocketnet_server_uring_send_packets( uring );

    for ( int queue = 0; queue < 2; queue++ )
        rocketnet_unit_check( fake.sends_on_socket[queue] <= fake.send_capacity );

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_unit_check( fake.sends == sends );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == (uint64_t) sends );
    rocketnet_unit_check( counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == 0 );

    uring_reserve_fake_stop_and_destroy( uring );
}

REGISTER_UNIT_TEST( test_io_uring_transport_reservation_does_not_wrap );
REGISTER_UNIT_TEST( test_io_uring_send_burst_spreads_across_socket_pool );
REGISTER_UNIT_TEST( test_io_uring_send_burst_beyond_one_socket_buffer_uses_siblings );

#endif // #if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )

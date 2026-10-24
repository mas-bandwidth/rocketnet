/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Receive capacity is at least twice the send budget, per transport
// (rocketnet-receive-budget-twice-send). The send budget is what one
// update can stage or submit; the receive capacity is the bank, ring,
// staging or completion budget the send path can fill while it waits.
// No real time, no socket, no network: the constants are compared, and
// on Linux the io_uring fake completion source drives one send_packets
// reap under full ingress. The clock is not read; the fake returns at
// once, so every case runs with time as data, never the wall clock.

#include "test_unit_registry.h"

#include "rocketnet_constants.h"
#ifndef ROCKETNET_SERVER_MAX_SEND_PACKETS
#define ROCKETNET_SERVER_MAX_SEND_PACKETS ROCKETNET_MAX_CLIENTS // BASE keeps this constant in the transport source
#endif
#include "rocketnet_driver_loopback.h"
#include "rocketnet_server_gso.h"
#include "rocketnet_server_iocp.h"
#include "rocketnet_server_rio.h"
#include "rocketnet_server_uring.h"
#include "rocketnet_uring_codec.h"

#define check rocketnet_unit_check

static void test_portable_transport_receive_budget_twice_send()
{
    check( ROCKETNET_SERVER_MAX_SEND_PACKETS > 0 );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
}

#if defined(__APPLE__)
static void test_msgx_transport_receive_budget_twice_send()
{
    // msgx shares the portable transport budgets.
    check( ROCKETNET_SERVER_MAX_SEND_PACKETS > 0 );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
}
#endif

static void test_loopback_transport_receive_budget_twice_send()
{
    check( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
}

#if defined(__linux__)
static void test_gso_transport_receive_budget_twice_send()
{
    check( ROCKETNET_GSO_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_GSO_RECV_QUEUE_SIZE >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE );
}

static void test_io_uring_transport_receive_budget_twice_send()
{
    check( ROCKETNET_URING_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_URING_RECV_BUFFERS >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( ROCKETNET_URING_RECEIVE_BUDGET >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( ROCKETNET_URING_RECEIVE_BUDGET >= ROCKETNET_URING_SEND_BUDGET );
}
#endif

#if defined(ROCKETNET_XDP_SEND_QUEUE_SIZE) && defined(ROCKETNET_XDP_RECV_QUEUE_SIZE)
static void test_xdp_transport_receive_budget_twice_send()
{
    check( ROCKETNET_XDP_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_XDP_RECV_QUEUE_SIZE >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE );
}
#endif

#if defined(_WIN32)
static void test_iocp_transport_receive_budget_twice_send()
{
    check( ROCKETNET_IOCP_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE >= 2 * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
}

static void test_rio_transport_receive_budget_twice_send()
{
    check( ROCKETNET_RIO_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_RIO_RECEIVES >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE );
}
#endif

REGISTER_UNIT_TEST( test_portable_transport_receive_budget_twice_send );
#if defined(__APPLE__)
REGISTER_UNIT_TEST( test_msgx_transport_receive_budget_twice_send );
#endif
REGISTER_UNIT_TEST( test_loopback_transport_receive_budget_twice_send );
#if defined(__linux__)
REGISTER_UNIT_TEST( test_gso_transport_receive_budget_twice_send );
REGISTER_UNIT_TEST( test_io_uring_transport_receive_budget_twice_send );
#endif
#if defined(ROCKETNET_XDP_SEND_QUEUE_SIZE) && defined(ROCKETNET_XDP_RECV_QUEUE_SIZE)
REGISTER_UNIT_TEST( test_xdp_transport_receive_budget_twice_send );
#endif
#if defined(_WIN32)
REGISTER_UNIT_TEST( test_iocp_transport_receive_budget_twice_send );
REGISTER_UNIT_TEST( test_rio_transport_receive_budget_twice_send );
#endif

#if defined(__linux__) && defined(IORING_RECV_MULTISHOT) && defined(ROCKETNET_URING_REAP_TEST_HOOKS) && defined(ROCKETNET_IO_URING)

#include <errno.h>

#define RB_FAKE_CAPACITY ( 1 << 18 )

struct receive_budget_fake_t
{
    struct rocketnet_server_uring_test_completion_t fifo[RB_FAKE_CAPACITY];
    uint64_t head;
    uint64_t tail;
    bool flood[ROCKETNET_URING_MAX_QUEUES];
    int next_buffer;
};

static int receive_budget_fake_size( struct receive_budget_fake_t * fake )
{
    return (int) ( fake->tail - fake->head );
}

static void receive_budget_fake_push( struct receive_budget_fake_t * fake, uint64_t user_data, int32_t res, uint32_t flags )
{
    check( receive_budget_fake_size( fake ) < RB_FAKE_CAPACITY );
    struct rocketnet_server_uring_test_completion_t * completion = &fake->fifo[fake->tail % RB_FAKE_CAPACITY];
    completion->user_data = user_data;
    completion->res = res;
    completion->flags = flags;
    fake->tail++;
}

static void receive_budget_fake_push_receive( struct receive_budget_fake_t * fake, int queue )
{
    const uint32_t flags = IORING_CQE_F_BUFFER | IORING_CQE_F_MORE | ( (uint32_t) fake->next_buffer << IORING_CQE_BUFFER_SHIFT );
    fake->next_buffer = ( fake->next_buffer + 1 ) % ROCKETNET_URING_RECV_BUFFERS;
    receive_budget_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue, 0 ), ROCKETNET_URING_RECV_BUFFER_BYTES, flags );
}

static int receive_budget_fake_peek( void * context, struct rocketnet_server_uring_test_completion_t * completions, int max )
{
    struct receive_budget_fake_t * fake = (struct receive_budget_fake_t*) context;
    int count = receive_budget_fake_size( fake );
    if ( count > max )
        count = max;
    for ( int i = 0; i < count; i++ )
        completions[i] = fake->fifo[( fake->head + (uint64_t) i ) % RB_FAKE_CAPACITY];
    return count;
}

static void receive_budget_fake_advance( void * context, int count )
{
    struct receive_budget_fake_t * fake = (struct receive_budget_fake_t*) context;
    check( count >= 0 && count <= receive_budget_fake_size( fake ) );
    fake->head += (uint64_t) count;
}

static int receive_budget_fake_ready( void * context )
{
    struct receive_budget_fake_t * fake = (struct receive_budget_fake_t*) context;
    return receive_budget_fake_size( fake );
}

static void receive_budget_fake_submitted( void * context, uint64_t user_data )
{
    struct receive_budget_fake_t * fake = (struct receive_budget_fake_t*) context;
    const int kind = rocketnet_uring_tag_kind( user_data );
    const int queue = rocketnet_uring_tag_queue( user_data );
    check( queue >= 0 && queue < ROCKETNET_URING_MAX_QUEUES );
    if ( kind == ROCKETNET_URING_TAG_SEND )
    {
        receive_budget_fake_push( fake, user_data, 64, 0 );
    }
    else if ( kind == ROCKETNET_URING_TAG_CANCEL )
    {
        fake->flood[queue] = false;
        receive_budget_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue, 0 ), -ECANCELED, 0 );
        receive_budget_fake_push( fake, user_data, 0, 0 );
    }
}

// One send_packets reap under full ingress never overflows the fill
// bank: the send completion budget stays inside the receive budget, so
// reaped receives fit the bank and RECEIVE_DROPPED_OVERFLOW stays zero.
static void test_io_uring_transport_receive_budget_twice_send_reap_no_overflow()
{
    struct rocketnet_server_uring_test_source_t source;
    struct receive_budget_fake_t * fake = (struct receive_budget_fake_t*) calloc( 1, sizeof(struct receive_budget_fake_t) );
    check( fake );
    source.context = fake;
    source.peek = receive_budget_fake_peek;
    source.advance = receive_budget_fake_advance;
    source.ready = receive_budget_fake_ready;
    source.submitted = receive_budget_fake_submitted;

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 1, &source );
    check( uring );

    // Full ingress ahead of the sends: more receive completions than
    // one send budget, so the reap must stay inside the bank.
    for ( int i = 0; i < 2 * ROCKETNET_URING_SEND_BUDGET; i++ )
        receive_budget_fake_push_receive( fake, 0 );

    rocketnet_address_t to;
    memset( &to, 0, sizeof(to) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 1001;
    for ( int i = 0; i < 16; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_uring_start_packet( uring, &to, &packet_id );
        check( frame );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, (uint8_t) i, 64 );
        rocketnet_server_uring_finish_packet( uring, packet_id, frame, 64 );
    }

    rocketnet_server_uring_send_packets( uring );

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    check( counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW] == 0 );

    rocketnet_server_uring_stop( uring );
    bool stopped = false;
    for ( int i = 0; i < 64 && !stopped; ++i ) stopped = rocketnet_server_uring_stopped( uring );
    check( stopped );
    rocketnet_server_uring_destroy( uring );
    free( fake );
}

REGISTER_UNIT_TEST( test_io_uring_transport_receive_budget_twice_send_reap_no_overflow );

#endif

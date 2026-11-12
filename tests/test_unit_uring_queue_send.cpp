/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server's per-queue send handoff (sdk/rocketnet_uring_queue_send.h,
// layer L1), at the rules proofs/tla/RNUringQueueSend.tla checks: a bank is
// published only with a spare to replace it, the worker takes the oldest,
// a destination's runs leave in staged order, a run handed back by -EAGAIN
// or a segment refusal is submitted again (a refused one as one packet a
// run, plain), and a bank goes back to the free FIFO only when every run of
// it has its completion. Every refusal is checked. The kernel is the test:
// it takes each run next hands out and answers it when the test says. Pure
// memory: no ring, no socket, no thread, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_uring_queue_send.h"
#include "../sdk/rocketnet_uring_send_queue.h"

#include <errno.h>
#include <string.h>
#include <vector>

#define check rocketnet_unit_check

static struct rocketnet_address_t queue_send_client( int d )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ROCKETNET_ADDRESS_IPV4;
    a.data.ipv4[0] = 203;
    a.data.ipv4[1] = 0;
    a.data.ipv4[2] = 113;
    a.data.ipv4[3] = (uint8_t) d;
    a.port = (uint16_t) ( 50000 + d );
    return a;
}

struct queue_send_test_t
{
    struct rocketnet_uring_send_queues_t * banks;
    struct rocketnet_uring_queue_send_t * send;

    queue_send_test_t( int slots, int max_segments, int max_bytes )
    {
        banks = rocketnet_uring_send_queues_create( NULL, 1, slots );
        send = banks ? rocketnet_uring_queue_send_create( NULL, banks, max_segments, max_bytes ) : NULL;
    }

    ~queue_send_test_t()
    {
        rocketnet_uring_queue_send_destroy( send );
        rocketnet_uring_send_queues_destroy( banks );
    }

    // start and finish one packet to destination d of bytes, or abort it
    // when bytes is 0. The slot it took, or -1 when refused.
    int stage( int d, int bytes )
    {
        const struct rocketnet_address_t to = queue_send_client( d );
        uint64_t id = 0;
        uint8_t * frame = rocketnet_uring_send_queues_start( banks, &to, &id );
        if ( !frame )
        {
            return -1;
        }
        if ( bytes > 0 )
        {
            check( rocketnet_uring_send_queues_finish( banks, id, frame, bytes ) );
        }
        else
        {
            check( rocketnet_uring_send_queues_abort( banks, id, frame ) );
        }
        const int b = banks->queue[0].producer;
        return (int) ( ( frame - banks->queue[0].bank[b].data ) / ROCKETNET_FRAME_STRIDE );
    }

    uint8_t slot_state( int b, int s ) const
    {
        return send->queue[0].state[b * send->slots + s];
    }

    uint64_t counter( int c ) const
    {
        return rocketnet_uring_queue_send_counter( send, 0, c );
    }
};

// the destination slot s of bank b was staged to
static int queue_send_dest( const queue_send_test_t & t, int b, int s )
{
    const struct rocketnet_address_t * to = &t.banks->queue[0].bank[b].slot[s].to;
    return to->data.ipv4[3];
}

static void test_uring_queue_send_create_refuses_bad_limits()
{
    struct rocketnet_uring_send_queues_t * banks = rocketnet_uring_send_queues_create( NULL, 1, 4 );
    check( banks != NULL );
    check( rocketnet_uring_queue_send_create( NULL, banks, 0, 1500 ) == NULL );
    check( rocketnet_uring_queue_send_create( NULL, banks, 4, 0 ) == NULL );
    struct rocketnet_uring_queue_send_t * send = rocketnet_uring_queue_send_create( NULL, banks, 1, 1 );
    check( send != NULL );
    rocketnet_uring_queue_send_destroy( send );
    rocketnet_uring_queue_send_destroy( NULL );
    rocketnet_uring_send_queues_destroy( banks );
}

REGISTER_UNIT_TEST( test_uring_queue_send_create_refuses_bad_limits );

// RNUringQueueSend Publish: nothing staged keeps the producer; a staged
// producer goes to the ready FIFO and the free head replaces it, reset; with
// no spare the producer is kept with its slots (the reversed witness
// "publish with no spare" is this refusal taken away)

static void test_uring_queue_send_publish_needs_a_spare()
{
    queue_send_test_t t( 2, 4, 1 << 16 );
    check( t.send != NULL );

    check( !rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( t.banks->queue[0].producer == 0 );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );

    check( t.stage( 1, 100 ) == 0 );
    check( !rocketnet_uring_queue_send_idle( t.send, 0 ) );
    t.banks->queue[0].bank[1].num_packets.store( 9 );      // a stale count on the free head
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( t.banks->queue[0].producer == 1 );
    check( t.banks->queue[0].bank[1].num_packets.load() == 0 );
    check( !rocketnet_uring_queue_send_idle( t.send, 0 ) );

    check( t.stage( 1, 100 ) == 0 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( t.banks->queue[0].producer == 2 );

    // banks 0 and 1 are ready, 2 the producer: no spare
    check( t.stage( 1, 100 ) == 0 );
    check( !rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( t.banks->queue[0].producer == 2 );
    check( rocketnet_uring_send_queues_staged( t.banks, 0 ) == 1 );
    check( t.stage( 2, 100 ) == 1 );                        // the kept producer still fills

    check( t.send->queue[0].handoff.ready_count == 2 );
    check( t.send->queue[0].handoff.ready_index[t.send->queue[0].handoff.ready_head] == 0 );
}

REGISTER_UNIT_TEST( test_uring_queue_send_publish_needs_a_spare );

// RNUringQueueSend WorkerClaim: the oldest ready bank, only when the worker
// holds none; its runs group each destination's packets in staged order and
// leave in that order, an aborted slot in none

static void test_uring_queue_send_claim_and_runs_in_staged_order()
{
    queue_send_test_t t( 8, 2, 1 << 16 );
    check( t.send != NULL );

    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == -1 );        // nothing ready

    // slots: 0 A, 1 B, 2 A, 3 aborted, 4 A, 5 B, 6 A
    check( t.stage( 1, 200 ) == 0 );
    check( t.stage( 2, 200 ) == 1 );
    check( t.stage( 1, 200 ) == 2 );
    check( t.stage( 1, 0 ) == 3 );
    check( t.stage( 1, 200 ) == 4 );
    check( t.stage( 2, 150 ) == 5 );
    check( t.stage( 1, 200 ) == 6 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );

    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) == 0 );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == -1 );        // already holds one
    check( t.slot_state( 0, 3 ) == ROCKETNET_URING_QUEUE_SEND_SLOT_ABORTED );
    check( t.slot_state( 0, 7 ) == ROCKETNET_URING_QUEUE_SEND_SLOT_EMPTY );

    // A's runs {0, 2} and {4, 6}, then B's {1, 5}
    const int expect_slots[3][2] = { { 0, 2 }, { 4, 6 }, { 1, 5 } };
    for ( int r = 0; r < 3; r++ )
    {
        struct rocketnet_uring_queue_send_run_t run;
        check( rocketnet_uring_queue_send_next( t.send, 0, &run ) );
        check( run.bank == 0 && run.count == 2 && !run.plain );
        const int * order = t.send->queue[0].order;
        check( order[run.first] == expect_slots[r][0] && order[run.first + 1] == expect_slots[r][1] );
        check( run.slot == expect_slots[r][0] );
        check( run.tag == (uint32_t) run.slot );
        check( queue_send_dest( t, 0, run.slot ) == ( r < 2 ? 1 : 2 ) );
        check( t.slot_state( 0, expect_slots[r][1] ) == ROCKETNET_URING_QUEUE_SEND_SLOT_RUN );
    }
    struct rocketnet_uring_queue_send_run_t none;
    check( !rocketnet_uring_queue_send_next( t.send, 0, &none ) );         // all submitted
    check( rocketnet_uring_queue_send_pending( t.send, 0 ) == 3 );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SEGMENTED ) == 3 );
}

REGISTER_UNIT_TEST( test_uring_queue_send_claim_and_runs_in_staged_order );

// RNUringQueueSend WorkerReap and WorkerRetire: the bank is never retired
// while a run of it is in the kernel (the reversed witness RetireAtSubmit),
// nor while a run handed back waits; completions in any order; a completion
// that names no run in the kernel changes nothing

static void test_uring_queue_send_retire_waits_for_every_completion()
{
    queue_send_test_t t( 4, 1, 1 << 16 );
    check( t.send != NULL );

    check( !rocketnet_uring_queue_send_retire( t.send, 0 ) );            // holds nothing

    check( t.stage( 1, 100 ) == 0 );
    check( t.stage( 2, 100 ) == 1 );
    check( t.stage( 1, 100 ) == 2 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );

    struct rocketnet_uring_queue_send_run_t run[3];
    for ( int i = 0; i < 3; i++ )
    {
        check( rocketnet_uring_queue_send_next( t.send, 0, &run[i] ) );
        check( run[i].count == 1 );
    }
    check( !rocketnet_uring_queue_send_retire( t.send, 0 ) );            // three runs in the kernel

    // the kernel answers the last first
    check( rocketnet_uring_queue_send_complete( t.send, 0, run[2].tag, ROCKETNET_URING_QUEUE_SEND_OK ) == 1 );
    check( rocketnet_uring_queue_send_complete( t.send, 0, run[2].tag, ROCKETNET_URING_QUEUE_SEND_OK ) == -1 );      // twice
    check( rocketnet_uring_queue_send_complete( t.send, 0, 3, ROCKETNET_URING_QUEUE_SEND_OK ) == -1 );               // no run there
    check( rocketnet_uring_queue_send_complete( t.send, 0, 3 * 4, ROCKETNET_URING_QUEUE_SEND_OK ) == -1 );           // past every bank
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_STRAY ) == 3 );
    check( rocketnet_uring_queue_send_pending( t.send, 0 ) == 2 );

    check( rocketnet_uring_queue_send_complete( t.send, 0, run[0].tag, ROCKETNET_URING_QUEUE_SEND_FAILED ) == 1 );
    check( !rocketnet_uring_queue_send_retire( t.send, 0 ) );            // one still in the kernel

    // the bank cannot come back to the fill meanwhile: two banks turn and
    // the third publish finds no spare
    check( t.stage( 1, 100 ) == 0 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( t.stage( 1, 100 ) == 0 );
    check( !rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( t.banks->queue[0].producer == 2 );

    check( rocketnet_uring_queue_send_complete( t.send, 0, run[1].tag, ROCKETNET_URING_QUEUE_SEND_OK ) == 1 );
    check( rocketnet_uring_queue_send_retire( t.send, 0 ) );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) == -1 );
    check( t.slot_state( 0, 0 ) == ROCKETNET_URING_QUEUE_SEND_SLOT_EMPTY );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 2 );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED ) == 1 );

    // now bank 0 is the spare
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( t.banks->queue[0].producer == 0 );
}

REGISTER_UNIT_TEST( test_uring_queue_send_retire_waits_for_every_completion );

// -EAGAIN hands the run back whole, and it goes again before any later run
// of its destination; the bank waits for it

static void test_uring_queue_send_again_resubmits_the_same_run_first()
{
    queue_send_test_t t( 6, 2, 1 << 16 );
    check( t.send != NULL );

    for ( int i = 0; i < 4; i++ )
    {
        check( t.stage( 1, 300 ) == i );
    }
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );

    struct rocketnet_uring_queue_send_run_t first;
    struct rocketnet_uring_queue_send_run_t second;
    check( rocketnet_uring_queue_send_next( t.send, 0, &first ) && first.slot == 0 && first.count == 2 );
    check( rocketnet_uring_queue_send_next( t.send, 0, &second ) && second.slot == 2 && second.count == 2 );

    check( rocketnet_uring_queue_send_result( -EAGAIN, true ) == ROCKETNET_URING_QUEUE_SEND_AGAIN );
    check( rocketnet_uring_queue_send_complete( t.send, 0, second.tag, ROCKETNET_URING_QUEUE_SEND_AGAIN ) == 2 );
    check( rocketnet_uring_queue_send_complete( t.send, 0, first.tag, ROCKETNET_URING_QUEUE_SEND_AGAIN ) == 2 );
    check( t.slot_state( 0, 1 ) == ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED );
    check( !rocketnet_uring_queue_send_retire( t.send, 0 ) );            // waiting to go again

    struct rocketnet_uring_queue_send_run_t again;
    check( rocketnet_uring_queue_send_next( t.send, 0, &again ) && again.slot == 0 && again.count == 2 && again.tag == first.tag );
    check( rocketnet_uring_queue_send_next( t.send, 0, &again ) && again.slot == 2 );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_AGAIN ) == 2 );
    check( rocketnet_uring_queue_send_complete( t.send, 0, first.tag, ROCKETNET_URING_QUEUE_SEND_OK ) == 2 );
    check( rocketnet_uring_queue_send_complete( t.send, 0, second.tag, ROCKETNET_URING_QUEUE_SEND_OK ) == 2 );
    check( rocketnet_uring_queue_send_retire( t.send, 0 ) );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 4 );
}

REGISTER_UNIT_TEST( test_uring_queue_send_again_resubmits_the_same_run_first );

// a refused segmented run comes back as one plain run per packet, in staged
// order, each its own completion; a refused one-packet run is dropped

static void test_uring_queue_send_refused_run_goes_plain()
{
    queue_send_test_t t( 4, 4, 1 << 16 );
    check( t.send != NULL );

    check( t.stage( 1, 500 ) == 0 );
    check( t.stage( 1, 500 ) == 1 );
    check( t.stage( 1, 200 ) == 2 );
    check( t.stage( 2, 500 ) == 3 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );

    struct rocketnet_uring_queue_send_run_t run;
    struct rocketnet_uring_queue_send_run_t lone;
    check( rocketnet_uring_queue_send_next( t.send, 0, &run ) && run.count == 3 && run.segment_bytes == 500 && run.last_bytes == 200 );
    check( rocketnet_uring_queue_send_next( t.send, 0, &lone ) && lone.count == 1 && lone.slot == 3 );

    check( rocketnet_uring_queue_send_result( -EIO, true ) == ROCKETNET_URING_QUEUE_SEND_REFUSED );
    check( rocketnet_uring_queue_send_result( -EINVAL, true ) == ROCKETNET_URING_QUEUE_SEND_REFUSED );
    check( rocketnet_uring_queue_send_result( -EOPNOTSUPP, true ) == ROCKETNET_URING_QUEUE_SEND_REFUSED );
    check( rocketnet_uring_queue_send_result( -EIO, false ) == ROCKETNET_URING_QUEUE_SEND_FAILED );
    check( rocketnet_uring_queue_send_result( -ENETUNREACH, true ) == ROCKETNET_URING_QUEUE_SEND_FAILED );
    check( rocketnet_uring_queue_send_result( 0, true ) == ROCKETNET_URING_QUEUE_SEND_OK );

    check( rocketnet_uring_queue_send_complete( t.send, 0, run.tag, ROCKETNET_URING_QUEUE_SEND_REFUSED ) == 3 );
    check( rocketnet_uring_queue_send_complete( t.send, 0, lone.tag, ROCKETNET_URING_QUEUE_SEND_REFUSED ) == 1 );
    check( t.slot_state( 0, 3 ) == ROCKETNET_URING_QUEUE_SEND_SLOT_DROPPED );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_REFUSED ) == 1 );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED ) == 1 );

    const int bytes[3] = { 500, 500, 200 };
    for ( int i = 0; i < 3; i++ )
    {
        struct rocketnet_uring_queue_send_run_t plain;
        check( rocketnet_uring_queue_send_next( t.send, 0, &plain ) );
        check( plain.count == 1 && plain.plain && plain.slot == i && plain.segment_bytes == bytes[i] && plain.last_bytes == bytes[i] );
        check( rocketnet_uring_queue_send_complete( t.send, 0, plain.tag, ROCKETNET_URING_QUEUE_SEND_OK ) == 1 );
    }
    check( rocketnet_uring_queue_send_retire( t.send, 0 ) );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 3 );
}

REGISTER_UNIT_TEST( test_uring_queue_send_refused_run_goes_plain );

// the stop's last resort: what waits to be submitted is abandoned and
// counted; runs in the kernel still hold the bank until they complete

static void test_uring_queue_send_abandon_keeps_kernel_runs()
{
    queue_send_test_t t( 4, 1, 1 << 16 );
    check( t.send != NULL );

    check( rocketnet_uring_queue_send_abandon( t.send, 0 ) == 0 );       // holds nothing

    for ( int i = 0; i < 3; i++ )
    {
        check( t.stage( 1, 100 ) == i );
    }
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );

    struct rocketnet_uring_queue_send_run_t run;
    check( rocketnet_uring_queue_send_next( t.send, 0, &run ) );
    check( rocketnet_uring_queue_send_abandon( t.send, 0 ) == 2 );
    check( rocketnet_uring_queue_send_abandon( t.send, 0 ) == 0 );
    check( t.counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 2 );

    struct rocketnet_uring_queue_send_run_t none;
    check( !rocketnet_uring_queue_send_next( t.send, 0, &none ) );
    check( !rocketnet_uring_queue_send_retire( t.send, 0 ) );
    check( rocketnet_uring_queue_send_complete( t.send, 0, run.tag, ROCKETNET_URING_QUEUE_SEND_OK ) == 1 );
    check( rocketnet_uring_queue_send_retire( t.send, 0 ) );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
}

REGISTER_UNIT_TEST( test_uring_queue_send_abandon_keeps_kernel_runs );

// a bank whose every claim was aborted, or refused past its slots, has no
// run and retires at once

static void test_uring_queue_send_bank_of_aborts_retires_at_once()
{
    queue_send_test_t t( 2, 4, 1 << 16 );
    check( t.send != NULL );

    check( t.stage( 1, 0 ) == 0 );
    check( t.stage( 2, 0 ) == 1 );
    check( t.stage( 1, 100 ) == -1 );                                    // the bank is full
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );
    struct rocketnet_uring_queue_send_run_t none;
    check( !rocketnet_uring_queue_send_next( t.send, 0, &none ) );
    check( rocketnet_uring_queue_send_retire( t.send, 0 ) );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
}

REGISTER_UNIT_TEST( test_uring_queue_send_bank_of_aborts_retires_at_once );

#if defined( __linux__ )
#include "../sdk/rocketnet_uring_codec.h"
#if defined( IORING_RECV_MULTISHOT )

// Layer L2: a run's SENDMSG over its scattered frames. A destination's
// packets interleaved with another's go out as one message whose iovecs
// point at each frame's wire bytes where it lies, with one UDP_SEGMENT cmsg
// of the segment size; a run sent plain carries no cmsg; a destination the
// socket's family cannot name has no message. Builds the message only: no
// socket, no ring.

static void test_uring_queue_send_message_over_scattered_frames()
{
    queue_send_test_t t( 6, 4, 1 << 16 );
    check( t.send != NULL );

    // slots: 0 A 300, 1 B 100, 2 A 300, 3 B 100, 4 A 120
    check( t.stage( 1, 300 ) == 0 );
    check( t.stage( 2, 100 ) == 1 );
    check( t.stage( 1, 300 ) == 2 );
    check( t.stage( 2, 100 ) == 3 );
    check( t.stage( 1, 120 ) == 4 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );

    struct rocketnet_uring_queue_send_run_t run;
    check( rocketnet_uring_queue_send_next( t.send, 0, &run ) && run.count == 3 );
    struct msghdr * message = rocketnet_uring_queue_send_message( t.send, 0, &run, false );
    check( message != NULL );
    check( message->msg_iovlen == 3 );
    const int slots[3] = { 0, 2, 4 };
    const size_t bytes[3] = { 300, 300, 120 };
    for ( int i = 0; i < 3; i++ )
    {
        check( message->msg_iov[i].iov_base == t.banks->queue[0].bank[0].data + slots[i] * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET );
        check( message->msg_iov[i].iov_len == bytes[i] );
    }
    check( message->msg_namelen == sizeof( struct sockaddr_in ) );
    const struct sockaddr_in * name = (const struct sockaddr_in *) message->msg_name;
    check( name->sin_family == AF_INET && ntohs( name->sin_port ) == 50001 );
    struct cmsghdr * cmsg = CMSG_FIRSTHDR( message );
    check( cmsg != NULL && cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_SEGMENT );
    uint16_t stride = 0;
    memcpy( &stride, CMSG_DATA( cmsg ), sizeof( stride ) );
    check( stride == 300 );

    // B's run on a dual stack socket: v4-mapped
    struct rocketnet_uring_queue_send_run_t b_run;
    check( rocketnet_uring_queue_send_next( t.send, 0, &b_run ) && b_run.count == 2 );
    struct msghdr * b_message = rocketnet_uring_queue_send_message( t.send, 0, &b_run, true );
    check( b_message != NULL && b_message->msg_namelen == sizeof( struct sockaddr_in6 ) );
    check( ( (const struct sockaddr_in6 *) b_message->msg_name )->sin6_family == AF_INET6 );
    check( b_message->msg_iov[0].iov_base == t.banks->queue[0].bank[0].data + 1 * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET );

    // A refuses: its packets go out one plain message each, no cmsg, and the
    // first run's message is untouched while B's is in the kernel
    check( rocketnet_uring_queue_send_complete( t.send, 0, run.tag, ROCKETNET_URING_QUEUE_SEND_REFUSED ) == 3 );
    for ( int i = 0; i < 3; i++ )
    {
        struct rocketnet_uring_queue_send_run_t plain;
        check( rocketnet_uring_queue_send_next( t.send, 0, &plain ) && plain.plain && plain.count == 1 );
        struct msghdr * one = rocketnet_uring_queue_send_message( t.send, 0, &plain, false );
        check( one != NULL && one->msg_iovlen == 1 && one->msg_control == NULL && one->msg_controllen == 0 );
        check( one->msg_iov[0].iov_len == bytes[i] );
    }
    check( b_message->msg_iov[1].iov_base == t.banks->queue[0].bank[0].data + 3 * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET );
}

REGISTER_UNIT_TEST( test_uring_queue_send_message_over_scattered_frames );

// an ipv6 destination on an ipv4 socket has no name: no message

static void test_uring_queue_send_message_refuses_an_unnamed_destination()
{
    queue_send_test_t t( 2, 4, 1 << 16 );
    check( t.send != NULL );

    struct rocketnet_address_t v6;
    memset( &v6, 0, sizeof( v6 ) );
    v6.type = ROCKETNET_ADDRESS_IPV6;
    v6.data.ipv6[0] = 0x2001;
    v6.data.ipv6[1] = 0xdb8;
    v6.data.ipv6[7] = 1;
    v6.port = 40000;
    uint64_t id = 0;
    uint8_t * frame = rocketnet_uring_send_queues_start( t.banks, &v6, &id );
    check( frame != NULL );
    check( rocketnet_uring_send_queues_finish( t.banks, id, frame, 100 ) );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );

    struct rocketnet_uring_queue_send_run_t run;
    check( rocketnet_uring_queue_send_next( t.send, 0, &run ) );
    check( rocketnet_uring_queue_send_message( t.send, 0, &run, false ) == NULL );
    check( rocketnet_uring_queue_send_message( t.send, 0, &run, true ) != NULL );
    check( rocketnet_uring_queue_send_complete( t.send, 0, run.tag, ROCKETNET_URING_QUEUE_SEND_FAILED ) == 1 );
    check( rocketnet_uring_queue_send_retire( t.send, 0 ) );
}

REGISTER_UNIT_TEST( test_uring_queue_send_message_refuses_an_unnamed_destination );

// the transport's segmentation off: later claims cut one packet a run

static void test_uring_queue_send_plain_cuts_single_runs()
{
    queue_send_test_t t( 4, 4, 1 << 16 );
    check( t.send != NULL );

    rocketnet_uring_queue_send_plain( t.send, 0 );
    check( t.stage( 1, 200 ) == 0 );
    check( t.stage( 1, 200 ) == 1 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    check( rocketnet_uring_queue_send_claim( t.send, 0 ) == 0 );
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_uring_queue_send_run_t run;
        check( rocketnet_uring_queue_send_next( t.send, 0, &run ) && run.count == 1 && run.slot == i );
        struct msghdr * message = rocketnet_uring_queue_send_message( t.send, 0, &run, false );
        check( message != NULL && message->msg_control == NULL );
    }
}

REGISTER_UNIT_TEST( test_uring_queue_send_plain_cuts_single_runs );

#endif // #if defined( IORING_RECV_MULTISHOT )
#endif // #if defined( __linux__ )

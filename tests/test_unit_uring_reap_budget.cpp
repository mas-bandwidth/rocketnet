/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server transport's per-call completion budgets
// (sdk/rocketnet_server_uring_linux.cpp, the bank size and
// ROCKETNET_URING_SEND_BUDGET in sdk/rocketnet_server_uring.h), driven
// through the fake completion source of rocketnet_server_uring_test_create:
// no kernel ring, no socket, no clock. The fake keeps a FIFO of completions
// and, while a queue's multishot receive is armed and flooding, tops the
// FIFO up so that every peek finds a full batch: sustained ingress that
// never lets the completion queue go empty. Each case counts the
// completions a call consumes, and an iteration guard stops a call that
// never returns (the guard trips, and the case fails at its check).

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_uring.h"

#if defined( __linux__ )
#include "../sdk/rocketnet_uring_codec.h"
#endif // #if defined( __linux__ )

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define check rocketnet_unit_check

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )

#define URING_FAKE_CAPACITY             ( 1 << 18 )

// the fake transport is sized for the default configuration's clients: one
// bank holds one update's receive budget, the most one call reaps
#define URING_TEST_BANK_SIZE            rocketnet_server_uring_bank_packets( ROCKETNET_DEFAULT_MAX_CLIENTS )

struct uring_fake_t
{
    struct rocketnet_server_uring_test_completion_t fifo[URING_FAKE_CAPACITY];
    uint64_t head;
    uint64_t tail;
    bool flood_on_arm;                          // an armed multishot floods
    bool flood[ROCKETNET_URING_MAX_QUEUES];     // this queue's multishot is armed and flooding
    int next_queue;
    int next_buffer;
    int64_t consumed;                           // completions advanced past since the case last zeroed it
    int64_t guard;                              // a call that consumes this many trips the guard
    bool guard_tripped;
    int sends_submitted;
    int cancels_submitted;
    bool hold_cancel;
    bool hold_send;
};

static int uring_fake_size( struct uring_fake_t * fake )
{
    return (int) ( fake->tail - fake->head );
}

static void uring_fake_push( struct uring_fake_t * fake, uint64_t user_data, int32_t res, uint32_t flags )
{
    check( uring_fake_size( fake ) < URING_FAKE_CAPACITY );

    struct rocketnet_server_uring_test_completion_t * completion = &fake->fifo[fake->tail % URING_FAKE_CAPACITY];
    completion->user_data = user_data;
    completion->res = res;
    completion->flags = flags;
    fake->tail++;
}

static void uring_fake_push_receive( struct uring_fake_t * fake, int queue )
{
    const uint32_t flags = IORING_CQE_F_BUFFER | IORING_CQE_F_MORE | ( (uint32_t) fake->next_buffer << IORING_CQE_BUFFER_SHIFT );
    fake->next_buffer = ( fake->next_buffer + 1 ) % ROCKETNET_URING_RECV_BUFFERS;
    uring_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue, 0 ), ROCKETNET_URING_RECV_BUFFER_BYTES, flags );
}

static bool uring_fake_flooding( struct uring_fake_t * fake )
{
    for ( int i = 0; i < ROCKETNET_URING_MAX_QUEUES; i++ )
    {
        if ( fake->flood[i] )
            return true;
    }

    return false;
}

static int uring_fake_peek( void * context, struct rocketnet_server_uring_test_completion_t * completions, int max )
{
    struct uring_fake_t * fake = (struct uring_fake_t *) context;

    if ( fake->consumed >= fake->guard )
    {
        fake->guard_tripped = true;
        return 0;
    }

    // sustained ingress: a flooding queue keeps a full batch in the queue

    while ( uring_fake_flooding( fake ) && uring_fake_size( fake ) < max )
    {
        while ( !fake->flood[fake->next_queue] )
        {
            fake->next_queue = ( fake->next_queue + 1 ) % ROCKETNET_URING_MAX_QUEUES;
        }

        uring_fake_push_receive( fake, fake->next_queue );
        fake->next_queue = ( fake->next_queue + 1 ) % ROCKETNET_URING_MAX_QUEUES;
    }

    int count = uring_fake_size( fake );
    if ( count > max )
    {
        count = max;
    }

    for ( int i = 0; i < count; i++ )
    {
        completions[i] = fake->fifo[( fake->head + (uint64_t) i ) % URING_FAKE_CAPACITY];
    }

    return count;
}

static void uring_fake_advance( void * context, int count )
{
    struct uring_fake_t * fake = (struct uring_fake_t *) context;
    check( count >= 0 && count <= uring_fake_size( fake ) );

    fake->head += (uint64_t) count;
    fake->consumed += count;
}

static int uring_fake_ready( void * context )
{
    struct uring_fake_t * fake = (struct uring_fake_t *) context;
    return uring_fake_size( fake ) + ( uring_fake_flooding( fake ) ? 1 : 0 );
}

// the kernel's side of a submission: a send completes at once (its
// completion joins the FIFO's tail, behind every receive already there); an
// arm starts a multishot; a cancel ends its queue's multishot with a
// terminating receive completion and posts its own

static void uring_fake_submitted( void * context, uint64_t user_data )
{
    struct uring_fake_t * fake = (struct uring_fake_t *) context;
    const int kind = rocketnet_uring_tag_kind( user_data );
    const int queue = rocketnet_uring_tag_queue( user_data );
    check( queue >= 0 && queue < ROCKETNET_URING_MAX_QUEUES );

    if ( kind == ROCKETNET_URING_TAG_SEND )
    {
        fake->sends_submitted++;
        if ( fake->hold_send )
            return;
        uring_fake_push( fake, user_data, 64, 0 );
    }
    else if ( kind == ROCKETNET_URING_TAG_RECV )
    {
        fake->flood[queue] = fake->flood_on_arm;
    }
    else if ( kind == ROCKETNET_URING_TAG_CANCEL )
    {
        fake->cancels_submitted++;
        fake->flood[queue] = false;
        if ( fake->hold_cancel )
            return;
        uring_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue, 0 ), -ECANCELED, 0 );
        uring_fake_push( fake, user_data, 0, 0 );
    }
}

// The guard of the cases that drive sends under receive pressure: four send
// budgets and, in a build whose receive budget is the larger
// (ROCKETNET_MAX_CLIENTS above 1280: 16 per client), a receive call's budget
// twice over.
static int64_t uring_send_case_guard()
{
    const int64_t send_guard = 4 * (int64_t) ROCKETNET_URING_SEND_BUDGET;
    const int64_t receive_guard = 2 * (int64_t) URING_TEST_BANK_SIZE;
    return send_guard > receive_guard ? send_guard : receive_guard;
}

static struct uring_fake_t * uring_fake_create( int64_t guard, struct rocketnet_server_uring_test_source_t * source )
{
    struct uring_fake_t * fake = (struct uring_fake_t *) calloc( 1, sizeof( struct uring_fake_t ) );
    check( fake );

    fake->flood_on_arm = true;
    fake->guard = guard;
    source->context = fake;
    source->peek = uring_fake_peek;
    source->advance = uring_fake_advance;
    source->ready = uring_fake_ready;
    source->submitted = uring_fake_submitted;

    return fake;
}

static void uring_fake_stop_and_destroy( rocketnet_server_uring_t * uring, uring_fake_t * fake )
{
    rocketnet_server_uring_stop( uring );

    bool stopped = false;
    for ( int i = 0; i < 64 && !stopped; ++i )
    {
        fake->consumed = 0;
        stopped = rocketnet_server_uring_stopped( uring );

        check( fake->consumed <= URING_TEST_BANK_SIZE );
        check( !fake->guard_tripped );
    }

    check( stopped );

    rocketnet_server_uring_destroy( uring );
}

static uint64_t uring_counter( struct rocketnet_server_uring_t * uring, int index )
{
    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    return counters[index];
}

static void uring_stage_sends( struct rocketnet_server_uring_t * uring, int count, uint8_t ** frames )
{
    rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 1001;

    for ( int i = 0; i < count; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_uring_start_packet( uring, &to, &packet_id );
        check( frame );

        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, (uint8_t) i, 64 );
        rocketnet_server_uring_finish_packet( uring, packet_id, frame, 64 );

        if ( frames )
        {
            frames[i] = frame;
        }
    }
}

// sustained ingress on every queue: each receive_packets returns after its
// budget or the fill bank's room, with that many datagrams exposed and the rest deferred,
// none dropped

static void test_io_uring_transport_reap_receive_flood_returns_within_budget()
{
    struct rocketnet_server_uring_test_source_t source;
    struct uring_fake_t * fake = uring_fake_create( 4 * (int64_t) URING_TEST_BANK_SIZE, &source );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 2, &source );
    rocketnet_unit_check( uring );
    check( rocketnet_server_uring_bank_size( uring ) == URING_TEST_BANK_SIZE );

    // the fill bank takes a reap only while it has room for a whole coalesced
    // receive, so a call exposes at least the bank's size less one coalesced
    // receive (each datagram here is its own CQE), and never more than the
    // budget; none dropped

    const int floor_packets = URING_TEST_BANK_SIZE - ROCKETNET_URING_MAX_SEGMENTS;
    uint64_t exposed = 0;

    for ( int call = 0; call < 4; call++ )
    {
        fake->consumed = 0;
        rocketnet_server_uring_receive_packets( uring );

        check( !fake->guard_tripped );
        check( fake->consumed <= URING_TEST_BANK_SIZE );
        check( fake->consumed >= floor_packets );

        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_uring_process_packets( uring );
        check( received->num_packets == fake->consumed );
        exposed += (uint64_t) received->num_packets;
        for ( int i = 0; i < received->num_packets; i++ )
        {
            check( received->packet_bytes[i] == ROCKETNET_URING_TEST_PAYLOAD_BYTES );
            check( received->from[i].type == ROCKETNET_ADDRESS_IPV4 );
            check( received->from[i].port == 1000 );
        }
    }

    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED ) == 4 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED ) == exposed );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW ) == 0 );

    fake->consumed = 0;
    uring_fake_stop_and_destroy( uring, fake );
    check( !fake->guard_tripped );
    check( fake->cancels_submitted == 2 );

    free( fake );
}

// sends staged while more than a send budget of receive completions sit
// ahead of theirs: send_packets returns within its budget with the sends
// deferred, the reservation index is not reset while the kernel owns their
// slots, and later updates, each within its budgets, retire every send

static void test_io_uring_transport_reap_send_retires_under_receive_pressure()
{
    struct rocketnet_server_uring_test_source_t source;
    struct uring_fake_t * fake = uring_fake_create( uring_send_case_guard(), &source );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 1, &source );
    rocketnet_unit_check( uring );

    for ( int i = 0; i < 2 * ROCKETNET_URING_SEND_BUDGET; i++ )
        uring_fake_push_receive( fake, 0 );

#define URING_TEST_SENDS 1000

    static uint8_t * frames[URING_TEST_SENDS];
    uring_stage_sends( uring, URING_TEST_SENDS, frames );

    fake->consumed = 0;
    rocketnet_server_uring_send_packets( uring );

    check( !fake->guard_tripped );
    check( fake->consumed <= ROCKETNET_URING_SEND_BUDGET );
    check( fake->sends_submitted == URING_TEST_SENDS );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT ) == 0 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_SEND_DEFERRED ) == 1 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED ) == 1 );

    // the kernel still owns the first phase's slots: the next start_packet
    // returns none of them

    uint8_t * next_frame = NULL;
    uring_stage_sends( uring, 1, &next_frame );
    for ( int i = 0; i < URING_TEST_SENDS; i++ )
        check( next_frame != frames[i] );

    int updates = 0;
    while ( updates < 8 && uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT ) < URING_TEST_SENDS + 1 )
    {
        fake->consumed = 0;
        rocketnet_server_uring_receive_packets( uring );

        check( !fake->guard_tripped );
        check( fake->consumed <= URING_TEST_BANK_SIZE );

        fake->consumed = 0;
        rocketnet_server_uring_send_packets( uring );

        check( !fake->guard_tripped );
        check( fake->consumed <= ROCKETNET_URING_SEND_BUDGET );

        updates++;
    }

    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT ) == URING_TEST_SENDS + 1 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED ) == 0 );
    check( fake->sends_submitted == URING_TEST_SENDS + 1 );

    // every send retired: the index was reset, so the next phase starts at
    // the first slot again

    uint8_t * first_again = NULL;
    uring_stage_sends( uring, 1, &first_again );
    check( first_again == frames[0] );

    rocketnet_server_uring_abort_packet( uring, 0, first_again );

    fake->consumed = 0;
    uring_fake_stop_and_destroy( uring, fake );
    check( !fake->guard_tripped );

    free( fake );
}

// destroy after saturation: a backlog of several receive budgets and sends
// still behind it when destroy runs. destroy drains it a budget at a time,
// the cancels end the floods, and the drained transport is freed

static void test_io_uring_transport_reap_destroy_after_saturation()
{
    struct rocketnet_server_uring_test_source_t source;
    struct uring_fake_t * fake = uring_fake_create( uring_send_case_guard(), &source );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 2, &source );
    rocketnet_unit_check( uring );

    fake->consumed = 0;
    rocketnet_server_uring_receive_packets( uring );

    check( !fake->guard_tripped );

    for ( int i = 0; i < 2 * ROCKETNET_URING_SEND_BUDGET; i++ )
    {
        uring_fake_push_receive( fake, i % 2 );
    }

    uring_stage_sends( uring, 100, NULL );

    fake->consumed = 0;
    rocketnet_server_uring_send_packets( uring );

    check( !fake->guard_tripped );
    check( fake->consumed <= ROCKETNET_URING_SEND_BUDGET );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_SEND_DEFERRED ) == 1 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT ) == 0 );

    fake->consumed = 0;
    uring_fake_stop_and_destroy( uring, fake );
    check( !fake->guard_tripped );
    check( fake->cancels_submitted == 2 );
    check( uring_fake_size( fake ) == 0 );

    free( fake );
}

// a receive completion with res >= 0 that names no provided buffer (no
// IORING_CQE_F_BUFFER, or a buffer id outside the ring) is dropped and
// counted in RECEIVE_DROPPED_NO_BUFFER, never delivered; a completion that
// names a buffer is still delivered

static void test_io_uring_transport_receive_without_a_buffer_is_counted()
{
    struct rocketnet_server_uring_test_source_t source;
    struct uring_fake_t * fake = uring_fake_create( 1000, &source );
    fake->flood_on_arm = false;

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 1, &source );
    rocketnet_unit_check( uring );

    const uint64_t recv_tag = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 );

    uring_fake_push( fake, recv_tag, ROCKETNET_URING_RECV_BUFFER_BYTES, IORING_CQE_F_MORE );
    rocketnet_server_uring_receive_packets( uring );

    check( !fake->guard_tripped );
    check( rocketnet_server_uring_process_packets( uring )->num_packets == 0 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_NO_BUFFER ) == 1 );

    const uint32_t outside = IORING_CQE_F_BUFFER | IORING_CQE_F_MORE | ( (uint32_t) ROCKETNET_URING_RECV_BUFFERS << IORING_CQE_BUFFER_SHIFT );
    uring_fake_push( fake, recv_tag, ROCKETNET_URING_RECV_BUFFER_BYTES, outside );
    rocketnet_server_uring_receive_packets( uring );

    check( !fake->guard_tripped );
    check( rocketnet_server_uring_process_packets( uring )->num_packets == 0 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_NO_BUFFER ) == 2 );

    uring_fake_push_receive( fake, 0 );
    rocketnet_server_uring_receive_packets( uring );

    check( !fake->guard_tripped );
    check( rocketnet_server_uring_process_packets( uring )->num_packets == 1 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_NO_BUFFER ) == 2 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE ) == 0 );

    uring_fake_stop_and_destroy( uring, fake );
    check( !fake->guard_tripped );

    free( fake );
}

// Withhold the kernel's cancellation replies. Every request/check returns,
// storage stays owned, and releasing both reply kinds allows final cleanup.
static void test_io_uring_stop_waits_for_kernel_ownership_without_waiting()
{
    rocketnet_server_uring_test_source_t source;
    uring_fake_t * fake = uring_fake_create( 4 * (int64_t) URING_TEST_BANK_SIZE, &source );
    fake->hold_cancel = true;

    rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 2, &source );
    check( uring );
    check( !rocketnet_server_uring_stopped( uring ) );

    rocketnet_server_uring_stop( uring );
    rocketnet_server_uring_stop( uring );

    for ( int i = 0; i < 1024; ++i )
    {
        fake->consumed = 0;

        check( !rocketnet_server_uring_stopped( uring ) );
        check( fake->consumed <= URING_TEST_BANK_SIZE );
        check( !fake->guard_tripped );
    }

    check( fake->cancels_submitted == 2 );
    check( rocketnet_server_uring_num_queues( uring ) == 2 );

    // terminal receives alone do not prove cancellation requests have completed

    fake->hold_cancel = false;

    for ( int i = 0; i < 2; ++i )
    {
        uring_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, i, 0 ), -ECANCELED, 0 );
    }

    check( !rocketnet_server_uring_stopped( uring ) );

    for ( int i = 0; i < 2; ++i )
    {
        uring_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, i, 0 ), 0, 0 );
    }

    check( rocketnet_server_uring_stopped( uring ) );
    check( rocketnet_server_uring_stopped( uring ) );

    rocketnet_server_uring_destroy( uring );
    free( fake );
}

static void test_io_uring_shutdown_flush_never_waits_for_saturated_sends()
{
    rocketnet_server_uring_test_source_t source;
    uring_fake_t * fake = uring_fake_create( 4 * (int64_t) URING_TEST_BANK_SIZE, &source );
    fake->flood_on_arm = false;
    fake->hold_send = true;

    rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 1, &source );
    check( uring );

    uring_stage_sends( uring, ROCKETNET_URING_SEND_QUEUE_SIZE, NULL );

    for ( int i = 0; i < 16; ++i )
    {
        rocketnet_server_uring_send_packets( uring );
    }

    check( fake->sends_submitted == ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( rocketnet_server_uring_test_wait_calls( uring ) == 0 );

    rocketnet_server_uring_stop( uring );
    check( !rocketnet_server_uring_stopped( uring ) );
    check( rocketnet_server_uring_test_wait_calls( uring ) == 0 );

    for ( int i = 0; i < ROCKETNET_URING_SEND_QUEUE_SIZE; ++i )
    {
        uring_fake_push( fake, rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, (uint32_t) i ), 64, 0 );
    }

    uring_fake_stop_and_destroy( uring, fake );
    free( fake );
}

REGISTER_UNIT_TEST( test_io_uring_shutdown_flush_never_waits_for_saturated_sends );
REGISTER_UNIT_TEST( test_io_uring_stop_waits_for_kernel_ownership_without_waiting );

// a send reap and a receive reap both fill the one fill bank between two
// swaps: with more datagrams in the completion queue than the bank holds,
// the reap stops short of the bank's room (a coalesced receive carries up
// to ROCKETNET_URING_MAX_SEGMENTS datagrams) and leaves the rest in the
// queue, deferred, so the next receive_packets exposes them; none dropped

static void test_io_uring_transport_reap_stops_at_bank_room()
{
    struct rocketnet_server_uring_test_source_t source;
    struct uring_fake_t * fake = uring_fake_create( 4 * (int64_t) URING_TEST_BANK_SIZE, &source );
    fake->flood_on_arm = false;

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_test_create( NULL, 1, &source );
    rocketnet_unit_check( uring );

    const int datagrams = URING_TEST_BANK_SIZE + 10;
    for ( int i = 0; i < datagrams; i++ )
        uring_fake_push_receive( fake, 0 );

    // the send's reap takes a send budget of receives into the fill bank

    uring_stage_sends( uring, 1, NULL );
    fake->consumed = 0;
    rocketnet_server_uring_send_packets( uring );
    check( !fake->guard_tripped );
    check( fake->consumed <= ROCKETNET_URING_SEND_BUDGET );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED ) == 1 );

    // the receive's reap stops at the bank's room and swaps what fits

    fake->consumed = 0;
    rocketnet_server_uring_receive_packets( uring );
    check( !fake->guard_tripped );

    struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_uring_process_packets( uring );
    check( received->num_packets <= URING_TEST_BANK_SIZE );
    check( received->num_packets + ROCKETNET_URING_MAX_SEGMENTS > URING_TEST_BANK_SIZE );
    const int first = received->num_packets;
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED ) == 2 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW ) == 0 );

    // the next receive exposes the rest and the send's CQE behind them

    fake->consumed = 0;
    rocketnet_server_uring_receive_packets( uring );
    check( !fake->guard_tripped );

    received = rocketnet_server_uring_process_packets( uring );
    check( first + received->num_packets == datagrams );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED ) == (uint64_t) datagrams );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW ) == 0 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED ) == 2 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT ) == 1 );

    // more datagrams than the bank holds wait in the completion queue between
    // updates, as they normally do; a send_packets with nothing staged and
    // nothing pending reaps nothing, and that is not a deferral

    // (this call retires the finished send and empties the send queue)

    rocketnet_server_uring_send_packets( uring );
    check( rocketnet_server_uring_test_send_idle( uring ) );

    for ( int i = 0; i < URING_TEST_BANK_SIZE + 10; i++ )
        uring_fake_push_receive( fake, 0 );

    fake->consumed = 0;
    rocketnet_server_uring_send_packets( uring );
    check( !fake->guard_tripped );
    check( fake->consumed == 0 );
    check( uring_counter( uring, ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED ) == 2 );

    fake->consumed = 0;
    uring_fake_stop_and_destroy( uring, fake );
    check( !fake->guard_tripped );

    free( fake );
}

REGISTER_UNIT_TEST( test_io_uring_transport_reap_receive_flood_returns_within_budget );
REGISTER_UNIT_TEST( test_io_uring_transport_reap_stops_at_bank_room );
REGISTER_UNIT_TEST( test_io_uring_transport_reap_send_retires_under_receive_pressure );
REGISTER_UNIT_TEST( test_io_uring_transport_reap_destroy_after_saturation );
REGISTER_UNIT_TEST( test_io_uring_transport_receive_without_a_buffer_is_counted );

#endif // #if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT ) && ROCKETNET_DEV && defined( ROCKETNET_IO_URING )

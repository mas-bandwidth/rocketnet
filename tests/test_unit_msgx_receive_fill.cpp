/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS batched server transport's receive worker over its three banks
// (sdk/rocketnet_msgx_receive_fill.h; model proofs/tla/RNMsgxReceiveFill.tla)
// over a fake kernel: each socket is a queue of datagram ids and a script of
// errors, each bank an array of ids. The drain, the commit
// (rocketnet_receive_handoff_commit), main's call (rocketnet_msgx_receive_call)
// and the stop count are the library's; the fake supplies the kernel, the
// banks and the mutex. No socket, no clock, no thread.
//
// Each test pins one rule: a read never asks for more than the bank has room
// for; a full bank is left for an empty one when main has started on it, and
// otherwise the drain holds and the socket keeps the rest; the held datagrams
// are read once main starts on the full bank; every receive call returns
// every datagram committed before it began (the shared delivery check); an
// ICMP error (EMSGSIZE too), EINTR or ENOBUFS is read past at the cost of one read of the
// budget; a dead socket ends its drain; the budget bounds one socket's reads
// in a round; after the stop every datagram banked and never returned is
// counted once. Then the review's case (Emma, E3): 64 sockets of 150
// datagrams in one update are all banked, where a bank of 8192 dropped 1408
// of them; and a seeded walk of arrivals, rounds and calls (some cut short,
// some finding the mutex busy) in which nothing read is ever dropped and
// every datagram is accounted for.

#include "test_unit_registry.h"

#include "rocketnet_msgx_receive_fill.h"
#include "rocketnet_receive_delivery_check.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define RECEIVE_FILL_TEST_MAX_SOCKETS 64
#define RECEIVE_FILL_TEST_MAX_QUEUE 1024
#define RECEIVE_FILL_TEST_MAX_SCRIPT 8
#define RECEIVE_FILL_TEST_MAX_BANK 16000
#define RECEIVE_FILL_TEST_MAX_BATCH 64
#define RECEIVE_FILL_TEST_ALL ( 1 << 30 )

struct receive_fill_test_socket_t
{
    uint32_t queue[RECEIVE_FILL_TEST_MAX_QUEUE];
    int head;
    int count;
    int script[RECEIVE_FILL_TEST_MAX_SCRIPT]; // errnos answered before any datagram, in order
    int num_script;
    int repeat_error;                          // answered on every read when non-zero
};

struct receive_fill_test_t
{
    struct receive_fill_test_socket_t sockets[RECEIVE_FILL_TEST_MAX_SOCKETS];
    int num_sockets;

    uint32_t banks[3][RECEIVE_FILL_TEST_MAX_BANK];
    uint32_t bank_count[3];
    struct rocketnet_receive_handoff_t hand;
    int capacity;

    uint32_t last_read[RECEIVE_FILL_TEST_MAX_BATCH];
    int last_count;

    // main's call: what it may take, where the ids go (NULL: nowhere), how
    // many it took; a busy mutex for the next tries
    int call_capacity;
    uint32_t * call_ids;
    int call_taken;
    bool lock_busy;

    uint32_t next_id;
    uint64_t arrived;
    uint64_t kernel_dropped;                   // the socket's buffer was full on arrival: the kernel's
    uint64_t consumed;                         // handed to the server by main's calls
    uint64_t stopped;                          // counted at the stop (StopCount)
    uint64_t full_dropped;                     // read and not banked for want of room: never
    uint64_t reads;
    uint64_t asked_past_room;                  // a read that asked for more than the room
    uint64_t failed[4];
    int last_failed_error;
    uint64_t moves;                            // commits that left a full or shared bank for an empty one
    uint64_t keeps;                            // commits that kept the worker's bank
    uint64_t folds;
};

static int receive_fill_test_banked( void * user )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    return (int) test->bank_count[test->hand.worker];
}

static int receive_fill_test_read( void * user, int fd, int want )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    rocketnet_unit_check( fd >= 0 && fd < test->num_sockets );
    rocketnet_unit_check( want >= 1 && want <= RECEIVE_FILL_TEST_MAX_BATCH );
    test->reads++;
    if ( want > test->capacity - (int) test->bank_count[test->hand.worker] )
    {
        test->asked_past_room++;
    }
    struct receive_fill_test_socket_t * socket = &test->sockets[fd];
    if ( socket->num_script > 0 )
    {
        const int error = socket->script[0];
        memmove( socket->script, socket->script + 1, sizeof( socket->script[0] ) * (size_t) ( socket->num_script - 1 ) );
        socket->num_script--;
        return -error;
    }
    if ( socket->repeat_error != 0 )
    {
        return -socket->repeat_error;
    }
    if ( socket->count == 0 )
    {
        return -EAGAIN;
    }
    const int n = socket->count < want ? socket->count : want;
    for ( int i = 0; i < n; i++ )
    {
        test->last_read[i] = socket->queue[( socket->head + i ) % RECEIVE_FILL_TEST_MAX_QUEUE];
    }
    socket->head = ( socket->head + n ) % RECEIVE_FILL_TEST_MAX_QUEUE;
    socket->count -= n;
    test->last_count = n;
    return n;
}

static void receive_fill_test_take( void * user, int i )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    rocketnet_unit_check( i >= 0 && i < test->last_count );
    const int bank = test->hand.worker;
    if ( (int) test->bank_count[bank] >= test->capacity )
    {
        test->full_dropped++;
        return;
    }
    test->banks[bank][test->bank_count[bank]++] = test->last_read[i];
}

// the worker's commit under the mutex (rocketnet_server_msgx_commit_receive)
static bool receive_fill_test_commit( void * user )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    const int next = rocketnet_receive_handoff_commit( &test->hand, test->bank_count[test->hand.worker] );
    if ( next < 0 )
    {
        test->keeps++;
        return false;
    }
    // the worker moves only to an empty bank
    rocketnet_unit_check( test->bank_count[next] == 0 );
    test->moves++;
    return true;
}

static void receive_fill_test_failed( void * user, int fd, int kind, int error )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    rocketnet_unit_check( fd >= 0 && fd < test->num_sockets );
    rocketnet_unit_check( kind > ROCKETNET_MSGX_RECEIVE_EMPTY && kind <= ROCKETNET_MSGX_RECEIVE_FAILED );
    test->failed[kind]++;
    test->last_failed_error = error;
}

static struct rocketnet_msgx_receive_io_t receive_fill_test_io( struct receive_fill_test_t * test )
{
    struct rocketnet_msgx_receive_io_t io;
    io.user = test;
    io.banked = receive_fill_test_banked;
    io.read = receive_fill_test_read;
    io.take = receive_fill_test_take;
    io.commit = receive_fill_test_commit;
    io.failed = receive_fill_test_failed;
    return io;
}

// main's side of one call (rocketnet_server_msgx_receive_packets)

static bool receive_fill_test_try_lock( void * user )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    return !test->lock_busy;
}

static void receive_fill_test_unlock( void * user )
{
    (void) user;
}

static void receive_fill_test_reset( void * user, int bank )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    // main never empties the bank the worker fills
    rocketnet_unit_check( bank != test->hand.worker );
    test->bank_count[bank] = 0;
}

static void receive_fill_test_fold( void * user, int bank )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    rocketnet_unit_check( bank != test->hand.worker );
    test->folds++;
}

static bool receive_fill_test_hand( void * user, int bank, uint32_t index )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    if ( test->call_taken >= test->call_capacity )
    {
        return false;
    }
    // main reads only below what the worker committed or left
    rocketnet_unit_check( index < test->bank_count[bank] );
    if ( test->call_ids )
    {
        test->call_ids[test->call_taken] = test->banks[bank][index];
    }
    test->call_taken++;
    test->consumed++;
    return true;
}

// one receive call of main taking at most capacity: how many it took
static int receive_fill_test_call( struct receive_fill_test_t * test, int capacity, uint32_t * ids )
{
    struct rocketnet_msgx_receive_call_io_t io;
    io.user = test;
    io.try_lock = receive_fill_test_try_lock;
    io.unlock = receive_fill_test_unlock;
    io.reset = receive_fill_test_reset;
    io.fold = receive_fill_test_fold;
    io.take = receive_fill_test_hand;
    test->call_capacity = capacity;
    test->call_ids = ids;
    test->call_taken = 0;
    bool busy = false;
    rocketnet_msgx_receive_call( &test->hand, &io, &busy );
    rocketnet_unit_check( busy == test->lock_busy );
    test->call_ids = NULL;
    return test->call_taken;
}

static bool receive_fill_test_reset_fixture( struct receive_fill_test_t * test, int num_sockets, int capacity )
{
    memset( test, 0, sizeof( *test ) );
    rocketnet_unit_check( num_sockets <= RECEIVE_FILL_TEST_MAX_SOCKETS && capacity <= RECEIVE_FILL_TEST_MAX_BANK );
    test->num_sockets = num_sockets;
    test->capacity = capacity;
    rocketnet_receive_handoff_init( &test->hand );
    return true;
}

static struct receive_fill_test_t * receive_fill_test_create( int num_sockets, int capacity )
{
    struct receive_fill_test_t * test = new receive_fill_test_t;
    receive_fill_test_reset_fixture( test, num_sockets, capacity );
    return test;
}

// count datagrams arrive on a socket; past its buffer the kernel drops them
static void receive_fill_test_arrive( struct receive_fill_test_t * test, int fd, int count, int buffer )
{
    struct receive_fill_test_socket_t * socket = &test->sockets[fd];
    for ( int i = 0; i < count; i++ )
    {
        test->arrived++;
        if ( socket->count >= buffer || socket->count >= RECEIVE_FILL_TEST_MAX_QUEUE )
        {
            test->kernel_dropped++;
            continue;
        }
        socket->queue[( socket->head + socket->count ) % RECEIVE_FILL_TEST_MAX_QUEUE] = test->next_id++;
        socket->count++;
    }
}

// the worker's round over every socket with data (the kevent list), as
// rocketnet_server_msgx_receive_worker runs it: a held drain ends the round,
// and a round that read and was not held commits its bank's count
static bool receive_fill_test_round( struct receive_fill_test_t * test, int batch, int budget )
{
    const struct rocketnet_msgx_receive_io_t io = receive_fill_test_io( test );
    const uint64_t reads = test->reads;
    bool held = false;
    for ( int fd = 0; fd < test->num_sockets && !held; fd++ )
    {
        const struct receive_fill_test_socket_t * socket = &test->sockets[fd];
        if ( socket->count == 0 && socket->num_script == 0 && socket->repeat_error == 0 )
        {
            continue;
        }
        held = !rocketnet_msgx_receive_drain( &io, fd, test->capacity, batch, budget );
    }
    if ( !held && test->reads != reads )
    {
        receive_fill_test_commit( test );
    }
    return held;
}

static uint64_t receive_fill_test_in_kernel( const struct receive_fill_test_t * test )
{
    uint64_t total = 0;
    for ( int fd = 0; fd < test->num_sockets; fd++ )
    {
        total += (uint64_t) test->sockets[fd].count;
    }
    return total;
}

// banked and not yet handed to the server: the unread tail of the bank the
// worker left, main's bank past its cursor, and the worker's bank when it is
// another
static uint64_t receive_fill_test_in_banks( const struct receive_fill_test_t * test )
{
    const struct rocketnet_receive_handoff_t * hand = &test->hand;
    uint64_t total = (uint64_t) ( hand->tail_end - hand->tail_cursor );
    total += (uint64_t) ( test->bank_count[hand->main] - hand->main_cursor );
    if ( hand->worker != hand->main )
    {
        total += (uint64_t) test->bank_count[hand->worker];
    }
    return total;
}

// every datagram that arrived is in a socket, in a bank, consumed by main,
// counted at the stop, or dropped by the kernel on arrival; none was read
// and lost
static void receive_fill_test_conserved( const struct receive_fill_test_t * test )
{
    rocketnet_unit_check( test->full_dropped == 0 );
    rocketnet_unit_check( test->asked_past_room == 0 );
    rocketnet_unit_check( test->arrived == receive_fill_test_in_kernel( test ) + receive_fill_test_in_banks( test ) + test->consumed + test->stopped +
                                               test->kernel_dropped );
}

static void test_rocketnet_msgx_receive_fill_want()
{
    rocketnet_unit_check( rocketnet_msgx_receive_want( 0, 100, 32 ) == 32 );
    rocketnet_unit_check( rocketnet_msgx_receive_want( 68, 100, 32 ) == 32 );
    rocketnet_unit_check( rocketnet_msgx_receive_want( 69, 100, 32 ) == 31 );
    rocketnet_unit_check( rocketnet_msgx_receive_want( 99, 100, 32 ) == 1 );
    rocketnet_unit_check( rocketnet_msgx_receive_want( 100, 100, 32 ) == 0 );
    rocketnet_unit_check( rocketnet_msgx_receive_want( 101, 100, 32 ) == 0 );
    rocketnet_unit_check( rocketnet_msgx_receive_want( 0, 16, 32 ) == 16 );
}

static void test_rocketnet_msgx_receive_fill_error_kinds()
{
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EAGAIN ) == ROCKETNET_MSGX_RECEIVE_EMPTY );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EWOULDBLOCK ) == ROCKETNET_MSGX_RECEIVE_EMPTY );

    // the platform's receive weather: cleared by the read that reports it
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EINTR ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( ENOBUFS ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( ECONNREFUSED ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EHOSTUNREACH ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( ENETUNREACH ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( ENETDOWN ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EMSGSIZE ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
#if defined( EHOSTDOWN )
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EHOSTDOWN ) == ROCKETNET_MSGX_RECEIVE_WEATHER );
#endif // #if defined( EHOSTDOWN )

    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( ENOTCONN ) == ROCKETNET_MSGX_RECEIVE_DEAD );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EPIPE ) == ROCKETNET_MSGX_RECEIVE_DEAD );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EDESTADDRREQ ) == ROCKETNET_MSGX_RECEIVE_DEAD );

    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EBADF ) == ROCKETNET_MSGX_RECEIVE_FAILED );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EINVAL ) == ROCKETNET_MSGX_RECEIVE_FAILED );
    rocketnet_unit_check( rocketnet_msgx_receive_error_kind( EFAULT ) == ROCKETNET_MSGX_RECEIVE_FAILED );
}

// R69 and StopCount: after the stop main hands the server nothing, and once
// the worker has exited every datagram it banked that main never returned is
// counted once: the unread tail of the bank the worker left, main's bank past
// its cursor, and the worker's bank, committed or not
static void test_rocketnet_msgx_receive_fill_stop()
{
    for ( int taken = 0; taken < 3; taken++ )
    {
        struct receive_fill_test_t * test = receive_fill_test_create( 1, 8 );

        // a round of 3, committed; main takes some of them
        receive_fill_test_arrive( test, 0, 3, 64 );
        rocketnet_unit_check( !receive_fill_test_round( test, 4, 8 ) );
        if ( taken > 0 )
        {
            rocketnet_unit_check( receive_fill_test_call( test, taken, NULL ) == taken );
        }

        // a round of 2, committed, and a drain of 4 the worker has not committed
        receive_fill_test_arrive( test, 0, 2, 64 );
        rocketnet_unit_check( !receive_fill_test_round( test, 4, 8 ) );
        receive_fill_test_arrive( test, 0, 4, 64 );
        const struct rocketnet_msgx_receive_io_t io = receive_fill_test_io( test );
        rocketnet_unit_check( rocketnet_msgx_receive_drain( &io, 0, test->capacity, 4, 8 ) );
        receive_fill_test_conserved( test );

        // the stop, then the worker's exit: main counts what it never returned
        test->stopped += rocketnet_msgx_receive_stop_count( &test->hand, test->bank_count );
        rocketnet_unit_check( test->stopped == (uint64_t) ( 9 - taken ) );
        rocketnet_unit_check( test->consumed == (uint64_t) taken );
        rocketnet_unit_check( test->arrived == receive_fill_test_in_kernel( test ) + test->consumed + test->stopped + test->kernel_dropped );
        delete test;
    }
}

// main's receive call: what it hands to the server this update
static int receive_fill_test_main_call( struct receive_fill_test_t * test )
{
    return receive_fill_test_call( test, RECEIVE_FILL_TEST_ALL, NULL );
}

// a datagram the worker banked before main's receive call begins is delivered
// by that call, whatever the rounds and commits before it: the worker banks
// one datagram and commits it, then banks more while main has not started on
// the first; main's next call must return them all (the receive handoff
// latency: under the ready-slot rule the later ones waited a whole update)
static void test_rocketnet_msgx_receive_fill_delivered_by_next_call()
{
    for ( int rounds = 2; rounds <= 4; rounds++ )
    {
        struct receive_fill_test_t * test = receive_fill_test_create( 1, 64 );
        rocketnet_unit_check( receive_fill_test_main_call( test ) == 0 );

        for ( int r = 0; r < rounds; r++ )
        {
            receive_fill_test_arrive( test, 0, 1, 64 );
            rocketnet_unit_check( !receive_fill_test_round( test, 8, 64 ) );
        }
        rocketnet_unit_check( receive_fill_test_in_kernel( test ) == 0 );
        rocketnet_unit_check( receive_fill_test_in_banks( test ) == (uint64_t) rounds );

        // every one of them was banked before this call began
        rocketnet_unit_check( receive_fill_test_main_call( test ) == rounds );

        delete test;
    }
}

// the shared receive delivery check (sdk/rocketnet_receive_delivery_check.h,
// proofs/tla/RNReceiveHandoff.tla DeliveredByNextCall) over this fake: a
// worker pass is the given datagrams arriving on one socket and one round of
// the worker (rocketnet_server_msgx_receive_worker's loop); a receive call is
// main's (rocketnet_server_msgx_receive_packets, rocketnet_msgx_receive_call)

static bool receive_fill_delivery_reset( void * user )
{
    return receive_fill_test_reset_fixture( (struct receive_fill_test_t *) user, 1, 64 );
}

static bool receive_fill_delivery_worker_pass( void * user, uint32_t first, int count )
{
    struct receive_fill_test_t * test = (struct receive_fill_test_t *) user;
    test->next_id = first;
    receive_fill_test_arrive( test, 0, count, RECEIVE_FILL_TEST_MAX_QUEUE );
    return !receive_fill_test_round( test, 32, 256 ) && receive_fill_test_in_kernel( test ) == 0;
}

static int receive_fill_delivery_main_call( void * user, uint32_t * ids, int capacity )
{
    return receive_fill_test_call( (struct receive_fill_test_t *) user, capacity, ids );
}

static void test_rocketnet_msgx_receive_fill_delivery()
{
    struct receive_fill_test_t * test = new receive_fill_test_t;
    struct rocketnet_receive_delivery_rig_t rig;
    rig.user = test;
    rig.worker_pass = receive_fill_delivery_worker_pass;
    rig.main_call = receive_fill_delivery_main_call;
    rocketnet_unit_check( rocketnet_receive_delivery_check_all( &rig, "msgx", receive_fill_delivery_reset ) );
    delete test;
}

// a busy mutex leaves main with what it already held, and the next call
// catches up: nothing is lost or returned twice
static void test_rocketnet_msgx_receive_fill_busy()
{
    struct receive_fill_test_t * test = receive_fill_test_create( 1, 64 );
    uint32_t ids[64];

    receive_fill_test_arrive( test, 0, 3, 64 );
    rocketnet_unit_check( !receive_fill_test_round( test, 8, 64 ) );
    rocketnet_unit_check( receive_fill_test_call( test, 2, ids ) == 2 && ids[0] == 0 && ids[1] == 1 );

    receive_fill_test_arrive( test, 0, 2, 64 );
    rocketnet_unit_check( !receive_fill_test_round( test, 8, 64 ) );
    test->lock_busy = true;
    rocketnet_unit_check( receive_fill_test_call( test, 64, ids ) == 1 && ids[0] == 2 );
    test->lock_busy = false;
    rocketnet_unit_check( receive_fill_test_call( test, 64, ids ) == 2 && ids[0] == 3 && ids[1] == 4 );
    receive_fill_test_conserved( test );
    delete test;
}

// a bank of 40 and a batch of 32 over a socket of 100: the second read asks
// for 8, the full bank is left for an empty one (main shares it), the next
// bank fills to 40, main has not started on it, and the drain holds with 20
// left in the socket
static void test_rocketnet_msgx_receive_fill_hold()
{
    struct receive_fill_test_t * test = receive_fill_test_create( 1, 40 );
    receive_fill_test_arrive( test, 0, 100, 1000 );
    const struct rocketnet_msgx_receive_io_t io = receive_fill_test_io( test );

    struct rocketnet_msgx_receive_drain_t drain;
    rocketnet_msgx_receive_drain_start( &drain, 0 );
    int count = 0;

    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 40, 32, 256, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ && count == 32 );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 40, 32, 256, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ && count == 8 );
    rocketnet_unit_check( drain.pc == ROCKETNET_MSGX_DRAIN_READ );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 40, 32, 256, &count ) == ROCKETNET_MSGX_DRAIN_STEP_MOVE_FULL );
    rocketnet_unit_check( test->hand.worker == 1 && test->hand.committed[0] == 40 && test->bank_count[0] == 40 );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 40, 32, 256, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ && count == 32 );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 40, 32, 256, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ && count == 8 );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 40, 32, 256, &count ) == ROCKETNET_MSGX_DRAIN_STEP_HOLD_FULL );
    rocketnet_unit_check( drain.pc == ROCKETNET_MSGX_DRAIN_HELD );

    rocketnet_unit_check( test->sockets[0].count == 20 );
    rocketnet_unit_check( test->reads == 4 );
    rocketnet_unit_check( test->moves == 1 && test->keeps == 1 );
    receive_fill_test_conserved( test );

    // the round is held: every socket keeps what it holds until main starts
    // on the full bank
    rocketnet_unit_check( receive_fill_test_round( test, 32, 256 ) );
    rocketnet_unit_check( test->sockets[0].count == 20 && test->reads == 4 );

    // main's call returns both full banks; the worker then leaves the second
    // and reads the rest, which the next call returns. The ids reach main in
    // arrival order
    uint32_t ids[100];
    rocketnet_unit_check( receive_fill_test_call( test, 100, ids ) == 80 );
    rocketnet_unit_check( !receive_fill_test_round( test, 32, 256 ) );
    rocketnet_unit_check( test->sockets[0].count == 0 );
    rocketnet_unit_check( receive_fill_test_call( test, 20, ids + 80 ) == 20 );
    for ( uint32_t i = 0; i < 100; i++ )
    {
        rocketnet_unit_check( ids[i] == i );
    }
    rocketnet_unit_check( receive_fill_test_main_call( test ) == 0 );
    receive_fill_test_conserved( test );
    delete test;
}

// ICMP errors queued for the client (EMSGSIZE among them: a forged "needs
// fragmentation"), EINTR and ENOBUFS are read past: each costs one read of
// the budget and the datagrams behind them are banked
static void test_rocketnet_msgx_receive_fill_weather()
{
    const int errors[] = { ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, ENETDOWN, EMSGSIZE, EINTR, ENOBUFS };
    for ( int error : errors )
    {
        struct receive_fill_test_t * test = receive_fill_test_create( 1, 1000 );
        test->sockets[0].script[0] = error;
        test->sockets[0].script[1] = error;
        test->sockets[0].num_script = 2;
        receive_fill_test_arrive( test, 0, 50, 1000 );

        const struct rocketnet_msgx_receive_io_t io = receive_fill_test_io( test );
        rocketnet_unit_check( rocketnet_msgx_receive_drain( &io, 0, 1000, 32, 256 ) );
        rocketnet_unit_check( test->failed[ROCKETNET_MSGX_RECEIVE_WEATHER] == 2 && test->last_failed_error == error );
        rocketnet_unit_check( test->failed[ROCKETNET_MSGX_RECEIVE_DEAD] == 0 && test->failed[ROCKETNET_MSGX_RECEIVE_FAILED] == 0 );
        rocketnet_unit_check( test->bank_count[test->hand.worker] == 50 && test->sockets[0].count == 0 );
        receive_fill_test_conserved( test );
        delete test;
    }
}

// a socket that answers EINTR on every read: the drain ends at the budget
static void test_rocketnet_msgx_receive_fill_weather_bounded()
{
    struct receive_fill_test_t * test = receive_fill_test_create( 1, 1000 );
    test->sockets[0].repeat_error = EINTR;
    const struct rocketnet_msgx_receive_io_t io = receive_fill_test_io( test );
    rocketnet_unit_check( rocketnet_msgx_receive_drain( &io, 0, 1000, 32, 16 ) );
    rocketnet_unit_check( test->reads == 16 && test->failed[ROCKETNET_MSGX_RECEIVE_WEATHER] == 16 );
    delete test;
}

// a dead connected socket or a failed read ends that socket's drain, counted
static void test_rocketnet_msgx_receive_fill_dead()
{
    const int errors[] = { ENOTCONN, EPIPE, EBADF };
    const int kinds[] = { ROCKETNET_MSGX_RECEIVE_DEAD, ROCKETNET_MSGX_RECEIVE_DEAD, ROCKETNET_MSGX_RECEIVE_FAILED };
    for ( int e = 0; e < 3; e++ )
    {
        struct receive_fill_test_t * test = receive_fill_test_create( 1, 1000 );
        test->sockets[0].script[0] = errors[e];
        test->sockets[0].num_script = 1;
        receive_fill_test_arrive( test, 0, 10, 1000 );
        const struct rocketnet_msgx_receive_io_t io = receive_fill_test_io( test );
        struct rocketnet_msgx_receive_drain_t drain;
        rocketnet_msgx_receive_drain_start( &drain, 0 );
        int count = 0;
        rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 1000, 32, 256, &count ) == ROCKETNET_MSGX_DRAIN_STEP_FAILED );
        rocketnet_unit_check( drain.pc == ROCKETNET_MSGX_DRAIN_DONE && drain.reads == 1 );
        rocketnet_unit_check( test->failed[kinds[e]] == 1 && test->last_failed_error == errors[e] );
        rocketnet_unit_check( test->sockets[0].count == 10 );
        delete test;
    }
}

// one flooded socket gets the budget's reads and the next socket its own
static void test_rocketnet_msgx_receive_fill_budget()
{
    struct receive_fill_test_t * test = receive_fill_test_create( 2, 16000 );
    receive_fill_test_arrive( test, 0, 1000, 1000 );
    receive_fill_test_arrive( test, 1, 10, 1000 );
    rocketnet_unit_check( !receive_fill_test_round( test, 32, 256 ) );
    rocketnet_unit_check( test->sockets[0].count == 1000 - 256 );
    rocketnet_unit_check( test->sockets[1].count == 0 );
    rocketnet_unit_check( test->hand.committed[0] == 266 && test->bank_count[0] == 266 );
    receive_fill_test_conserved( test );
    delete test;
}

// the budget is exact (Stella's review of 4119919c7, note 2): a batch that
// does not divide the budget asks only for what is left of it, so a socket's
// reads in a round never pass the budget. A budget of 6 and a batch of 4
// over a socket of 20: reads of 4 and 2, not 4 and 4
static void test_rocketnet_msgx_receive_fill_budget_exact()
{
    struct receive_fill_test_t * test = receive_fill_test_create( 1, 100 );
    receive_fill_test_arrive( test, 0, 20, 1000 );
    const struct rocketnet_msgx_receive_io_t io = receive_fill_test_io( test );

    struct rocketnet_msgx_receive_drain_t drain;
    rocketnet_msgx_receive_drain_start( &drain, 0 );
    int count = 0;
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 100, 4, 6, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ );
    rocketnet_unit_check( count == 4 && drain.reads == 4 );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 100, 4, 6, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ );
    rocketnet_unit_check( count == 2 && drain.reads == 6 );
    rocketnet_unit_check( drain.pc == ROCKETNET_MSGX_DRAIN_READ );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 100, 4, 6, &count ) == ROCKETNET_MSGX_DRAIN_STEP_BUDGET );
    rocketnet_unit_check( test->bank_count[test->hand.worker] == 6 && test->sockets[0].count == 14 );
    rocketnet_unit_check( test->asked_past_room == 0 );

    // a weather error spends one, so the reads after it ask for 4, then 1
    test->sockets[0].script[0] = EINTR;
    test->sockets[0].num_script = 1;
    rocketnet_msgx_receive_drain_start( &drain, 0 );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 100, 4, 6, &count ) == ROCKETNET_MSGX_DRAIN_STEP_WEATHER );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 100, 4, 6, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ );
    rocketnet_unit_check( count == 4 && drain.reads == 5 );
    rocketnet_unit_check( rocketnet_msgx_receive_drain_step( &drain, &io, 100, 4, 6, &count ) == ROCKETNET_MSGX_DRAIN_STEP_READ );
    rocketnet_unit_check( count == 1 && drain.reads == 6 );
    rocketnet_unit_check( test->bank_count[test->hand.worker] == 11 && test->sockets[0].count == 9 );
    receive_fill_test_conserved( test );
    delete test;
}

// the review's case (Emma, E3): 64 connected sockets of 150 datagrams each in
// one update, 9600 in all, against the bank of one update's whole budget.
// Every one is banked; a bank of 8192 dropped 1408 of them
static void test_rocketnet_msgx_receive_fill_review_case()
{
    struct receive_fill_test_t * test = receive_fill_test_create( 64, 16000 );
    for ( int fd = 0; fd < 64; fd++ )
    {
        receive_fill_test_arrive( test, fd, 150, 1000 );
    }
    rocketnet_unit_check( !receive_fill_test_round( test, 32, 256 ) );
    rocketnet_unit_check( test->hand.committed[0] == 9600 && test->bank_count[0] == 9600 );
    rocketnet_unit_check( receive_fill_test_in_kernel( test ) == 0 );
    receive_fill_test_conserved( test );

    // a second update before main's next call: the next bank fills to its
    // 16000, then the round holds and the kernel keeps the rest
    for ( int round = 0; round < 2; round++ )
    {
        for ( int fd = 0; fd < 64; fd++ )
        {
            receive_fill_test_arrive( test, fd, 150, 1000 );
        }
    }
    for ( int round = 0; round < 4; round++ )
    {
        receive_fill_test_round( test, 32, 256 );
    }
    rocketnet_unit_check( test->bank_count[test->hand.worker] == 16000 );
    rocketnet_unit_check( receive_fill_test_in_kernel( test ) == 19200 - 16000 );
    receive_fill_test_conserved( test );

    // main takes them; nothing read was lost, and the rest arrives in turn
    for ( int round = 0; round < 8; round++ )
    {
        receive_fill_test_main_call( test );
        receive_fill_test_round( test, 32, 256 );
    }
    receive_fill_test_main_call( test );
    receive_fill_test_main_call( test );
    rocketnet_unit_check( test->consumed == 3 * 9600 && test->kernel_dropped == 0 );
    receive_fill_test_conserved( test );
    delete test;
}

static uint32_t receive_fill_test_random( uint32_t * state )
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

// a seeded walk: random arrivals (some past a small kernel buffer), injected
// errors, rounds and calls (some cut short, some finding the mutex busy),
// small banks and batches; on half the seeds a stop and its count. Nothing
// read is ever dropped, no read asks past the room, and every datagram is
// accounted for
static void test_rocketnet_msgx_receive_fill_walk()
{
    const int weather[] = { ECONNREFUSED, EHOSTUNREACH, EINTR, ENOBUFS };
    for ( uint32_t seed = 1; seed <= 200; seed++ )
    {
        uint32_t state = seed;
        const int sockets = 1 + (int) ( receive_fill_test_random( &state ) % 8 );
        const int capacity = 1 + (int) ( receive_fill_test_random( &state ) % 48 );
        const int batch = 1 + (int) ( receive_fill_test_random( &state ) % 16 );
        const int budget = 1 + (int) ( receive_fill_test_random( &state ) % 40 );
        const int buffer = 1 + (int) ( receive_fill_test_random( &state ) % 100 );
        struct receive_fill_test_t * test = receive_fill_test_create( sockets, capacity );

        for ( int step = 0; step < 400; step++ )
        {
            const uint32_t pick = receive_fill_test_random( &state ) % 10;
            const int fd = (int) ( receive_fill_test_random( &state ) % (uint32_t) sockets );
            if ( pick < 4 )
            {
                receive_fill_test_arrive( test, fd, 1 + (int) ( receive_fill_test_random( &state ) % 40 ), buffer );
            }
            else if ( pick == 4 && test->sockets[fd].num_script < RECEIVE_FILL_TEST_MAX_SCRIPT )
            {
                test->sockets[fd].script[test->sockets[fd].num_script++] = weather[receive_fill_test_random( &state ) % 4];
            }
            else if ( pick < 8 )
            {
                receive_fill_test_round( test, batch, budget );
            }
            else
            {
                test->lock_busy = receive_fill_test_random( &state ) % 5 == 0;
                const int cut = receive_fill_test_random( &state ) % 3 == 0 ? 1 + (int) ( receive_fill_test_random( &state ) % 8 ) : RECEIVE_FILL_TEST_ALL;
                receive_fill_test_call( test, cut, NULL );
                test->lock_busy = false;
            }
            receive_fill_test_conserved( test );
            rocketnet_unit_check( (int) test->bank_count[test->hand.worker] <= capacity );
        }

        if ( seed % 2 == 0 )
        {
            // the stop: main calls no more, and counts what it never returned
            test->stopped += rocketnet_msgx_receive_stop_count( &test->hand, test->bank_count );
            rocketnet_unit_check( test->arrived == receive_fill_test_in_kernel( test ) + test->consumed + test->stopped + test->kernel_dropped );
            delete test;
            continue;
        }

        // stop arriving: rounds and calls empty every socket and bank
        for ( int step = 0; step < 4000 && ( receive_fill_test_in_kernel( test ) > 0 || receive_fill_test_in_banks( test ) > 0 ); step++ )
        {
            receive_fill_test_round( test, batch, budget );
            receive_fill_test_main_call( test );
        }
        rocketnet_unit_check( receive_fill_test_in_kernel( test ) == 0 && receive_fill_test_in_banks( test ) == 0 );
        rocketnet_unit_check( test->consumed + test->kernel_dropped == test->arrived );
        receive_fill_test_conserved( test );
        delete test;
    }
}

REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_want );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_error_kinds );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_stop );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_delivered_by_next_call );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_delivery );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_busy );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_hold );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_weather );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_weather_bounded );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_dead );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_budget );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_budget_exact );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_review_case );
REGISTER_UNIT_TEST( test_rocketnet_msgx_receive_fill_walk );

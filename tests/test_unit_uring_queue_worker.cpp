/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server's per-queue worker (sdk/rocketnet_uring_queue_worker.h,
// layer L3), at the rules proofs/tla/RNUringQueueWorker.tla checks: the
// worker sleeps only with its wake read armed, submits the runs of what main
// publishes, stops submitting after -EAGAIN until its POLLOUT poll fires,
// and after the stop cancels its receive and wake read, abandons what waits
// once the socket has stayed full past its bound, and exits only with
// nothing of its in the kernel (the witness "exit before drain" is the
// check that no pass says exit while anything is). Every refusal is checked.
// The kernel is the test: a fake submission queue that records what the
// worker prepares and a completion queue the test fills. No ring, no
// socket, no thread, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_uring_queue_send.h"
#include "../sdk/rocketnet_uring_queue_worker.h"
#include "../sdk/rocketnet_uring_send_queue.h"

#if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING
#include "../sdk/rocketnet_uring_codec.h"
#endif // #if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING

#if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING && defined( IORING_RECV_MULTISHOT )

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <vector>

#define check rocketnet_unit_check

#define WORKER_TEST_SQ_MAX 64

static struct rocketnet_address_t worker_test_client( int d )
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

// the kernel: a submission queue of capacity entries the worker prepares
// into, everything submitted so far, and the completions the test posts

struct worker_test_kernel_t
{
    int capacity = WORKER_TEST_SQ_MAX;
    int queued = 0;
    struct io_uring_sqe sq[WORKER_TEST_SQ_MAX];
    std::vector<struct io_uring_sqe> submitted;
    std::vector<struct rocketnet_uring_queue_worker_completion_t> ready;
    int submits = 0;
    int waits = 0;
};

static int worker_test_space( void * context )
{
    worker_test_kernel_t * k = (worker_test_kernel_t *) context;
    return k->capacity - k->queued;
}

static struct io_uring_sqe * worker_test_sqe( void * context )
{
    worker_test_kernel_t * k = (worker_test_kernel_t *) context;
    check( k->queued < k->capacity );
    struct io_uring_sqe * sqe = &k->sq[k->queued++];
    memset( sqe, 0, sizeof( *sqe ) );
    return sqe;
}

static int worker_test_submit( void * context, bool wait )
{
    worker_test_kernel_t * k = (worker_test_kernel_t *) context;
    const int n = k->queued;
    for ( int i = 0; i < n; i++ )
    {
        k->submitted.push_back( k->sq[i] );
    }
    k->queued = 0;
    k->submits++;
    k->waits += wait ? 1 : 0;
    return n;
}

static int worker_test_peek( void * context, struct rocketnet_uring_queue_worker_completion_t * completions, int max )
{
    worker_test_kernel_t * k = (worker_test_kernel_t *) context;
    int n = 0;
    while ( n < max && n < (int) k->ready.size() )
    {
        completions[n] = k->ready[n];
        n++;
    }
    return n;
}

static void worker_test_advance( void * context, int count )
{
    worker_test_kernel_t * k = (worker_test_kernel_t *) context;
    check( count <= (int) k->ready.size() );
    k->ready.erase( k->ready.begin(), k->ready.begin() + count );
}

// the queue's receive: armed while its multishot is in the kernel

struct worker_test_receive_t
{
    bool armed = false;
    bool stopping = false;
    int arms = 0;
    int arm_refused = 0;
    int cqes = 0;
};

static bool worker_test_receive_needs_arm( void * context )
{
    worker_test_receive_t * r = (worker_test_receive_t *) context;
    return !r->armed && !r->stopping;
}

static bool worker_test_receive_arm( void * context, struct io_uring_sqe * sqe )
{
    worker_test_receive_t * r = (worker_test_receive_t *) context;
    if ( !sqe )
    {
        r->arm_refused++;
        return false;
    }
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->user_data = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 );
    r->armed = true;
    r->arms++;
    return true;
}

static void worker_test_receive_cqe( void * context, int32_t res, uint32_t flags )
{
    worker_test_receive_t * r = (worker_test_receive_t *) context;
    (void) res;
    r->cqes++;
    if ( !( flags & IORING_CQE_F_MORE ) )
    {
        r->armed = false;
    }
}

static void worker_test_receive_stop( void * context )
{
    worker_test_receive_t * r = (worker_test_receive_t *) context;
    r->stopping = true;
}

static bool worker_test_receive_armed( void * context )
{
    worker_test_receive_t * r = (worker_test_receive_t *) context;
    return r->armed;
}

struct worker_test_t
{
    struct rocketnet_uring_send_queues_t * banks = NULL;
    struct rocketnet_uring_queue_send_t * send = NULL;
    worker_test_kernel_t kernel;
    worker_test_receive_t receive;
    struct rocketnet_uring_queue_worker_t worker;
    bool ok = false;

    worker_test_t( int slots, int max_in_kernel, int max_retry, bool segment = true, bool ipv6 = false )
    {
        banks = rocketnet_uring_send_queues_create( NULL, 1, slots );
        send = banks ? rocketnet_uring_queue_send_create( NULL, banks, 4, 1 << 16 ) : NULL;
        struct rocketnet_uring_queue_worker_config_t config = config_for( max_in_kernel, max_retry, segment, ipv6 );
        ok = send && rocketnet_uring_queue_worker_init( &worker, &config );
    }

    ~worker_test_t()
    {
        rocketnet_uring_queue_send_destroy( send );
        rocketnet_uring_send_queues_destroy( banks );
    }

    struct rocketnet_uring_queue_worker_config_t config_for( int max_in_kernel, int max_retry, bool segment, bool ipv6 )
    {
        struct rocketnet_uring_queue_worker_config_t config;
        memset( &config, 0, sizeof( config ) );
        config.queue_index = 0;
        config.fd = 7;
        config.ipv6 = ipv6;
        config.wake_fd = -1;
        config.max_in_kernel = max_in_kernel;
        config.max_retry = max_retry;
        config.segment = segment;
        config.send = send;
        config.receive.context = &receive;
        config.receive.needs_arm = worker_test_receive_needs_arm;
        config.receive.arm = worker_test_receive_arm;
        config.receive.cqe = worker_test_receive_cqe;
        config.receive.stop = worker_test_receive_stop;
        config.receive.armed = worker_test_receive_armed;
        config.kernel.context = &kernel;
        config.kernel.space = worker_test_space;
        config.kernel.sqe = worker_test_sqe;
        config.kernel.submit = worker_test_submit;
        config.kernel.peek = worker_test_peek;
        config.kernel.advance = worker_test_advance;
        return config;
    }

    void stage_to( const struct rocketnet_address_t * to, int bytes )
    {
        uint64_t id = 0;
        uint8_t * frame = rocketnet_uring_send_queues_start( banks, to, &id );
        check( frame != NULL );
        check( rocketnet_uring_send_queues_finish( banks, id, frame, bytes ) );
    }

    void stage( int d, int bytes )
    {
        const struct rocketnet_address_t to = worker_test_client( d );
        stage_to( &to, bytes );
    }

    // main's send_packets: publish, then the wake, which the worker's armed
    // read completes
    void publish()
    {
        check( rocketnet_uring_queue_send_publish( send, 0 ) );
        post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    }

    void post( uint64_t user_data, int32_t res, uint32_t flags )
    {
        struct rocketnet_uring_queue_worker_completion_t c;
        c.user_data = user_data;
        c.res = res;
        c.flags = flags;
        kernel.ready.push_back( c );
    }

    // one turn of the worker's loop: a pass, and the submit it asks for
    int step()
    {
        const int next = rocketnet_uring_queue_worker_pass( &worker );
        if ( next != ROCKETNET_URING_WORKER_EXIT )
        {
            worker_test_submit( &kernel, next == ROCKETNET_URING_WORKER_SLEEP );
        }
        return next;
    }

    // steps until a pass says sleep or exit, at most a bound
    int settle()
    {
        for ( int i = 0; i < 64; i++ )
        {
            const int next = step();
            if ( next != ROCKETNET_URING_WORKER_CONTINUE )
            {
                return next;
            }
        }
        check( false );
        return -1;
    }

    // the submitted entries of an opcode, from index from on
    std::vector<struct io_uring_sqe> of( int opcode, size_t from = 0 ) const
    {
        std::vector<struct io_uring_sqe> out;
        for ( size_t i = from; i < kernel.submitted.size(); i++ )
        {
            if ( kernel.submitted[i].opcode == opcode )
            {
                out.push_back( kernel.submitted[i] );
            }
        }
        return out;
    }

    // the send runs submitted from index from on, oldest first
    std::vector<struct io_uring_sqe> sends( size_t from = 0 ) const
    {
        return of( IORING_OP_SENDMSG, from );
    }

    uint64_t counter( int c ) const
    {
        return rocketnet_uring_queue_worker_counter( &worker, c );
    }

    uint64_t send_counter( int c ) const
    {
        return rocketnet_uring_queue_send_counter( send, 0, c );
    }
};

static int worker_test_iovecs( const struct io_uring_sqe & sqe )
{
    const struct msghdr * message = (const struct msghdr *) (uintptr_t) sqe.addr;
    return (int) message->msg_iovlen;
}

static bool worker_test_segmented( const struct io_uring_sqe & sqe )
{
    const struct msghdr * message = (const struct msghdr *) (uintptr_t) sqe.addr;
    return message->msg_controllen > 0;
}

static void test_uring_queue_worker_init_refuses_bad_config()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );

    struct rocketnet_uring_queue_worker_t worker;

    struct rocketnet_uring_queue_worker_config_t config = t.config_for( 0, 2, true, false );
    check( !rocketnet_uring_queue_worker_init( &worker, &config ) );

    config = t.config_for( 8, 0, true, false );
    check( !rocketnet_uring_queue_worker_init( &worker, &config ) );

    config = t.config_for( 8, 2, true, false );
    config.send = NULL;
    check( !rocketnet_uring_queue_worker_init( &worker, &config ) );

    config = t.config_for( 8, 2, true, false );
    config.kernel.peek = NULL;
    check( !rocketnet_uring_queue_worker_init( &worker, &config ) );

    config = t.config_for( 8, 2, true, false );
    config.receive.armed = NULL;
    check( !rocketnet_uring_queue_worker_init( &worker, &config ) );

    config = t.config_for( 8, 2, true, false );
    config.queue_index = 0x10000;
    check( !rocketnet_uring_queue_worker_init( &worker, &config ) );

    // no receive at all is a worker that only sends
    config = t.config_for( 8, 2, true, false );
    memset( &config.receive, 0, sizeof( config.receive ) );
    check( rocketnet_uring_queue_worker_init( &worker, &config ) );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_init_refuses_bad_config );

// RNUringQueueWorker ArmWake and Sleep: the first pass arms the receive and
// the wake read; with nothing else to do the next pass sleeps, and never
// before the wake read is armed

static void test_uring_queue_worker_sleeps_only_with_the_wake_armed()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );

    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( t.receive.arms == 1 );
    check( t.of( IORING_OP_RECVMSG ).size() == 1 );
    check( t.of( IORING_OP_READ ).size() == 1 );
    check( t.of( IORING_OP_READ )[0].user_data == rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ) );
    check( t.worker.wake_armed );

    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.kernel.waits == 1 );

    // the wake completes: the read is armed again before the next sleep
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( t.of( IORING_OP_READ ).size() == 2 );
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );

    // with no submission entry free the wake cannot be armed: the pass that
    // reaps it does not sleep, and the read is armed once there is room
    t.kernel.capacity = 0;
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( !t.worker.wake_armed );
    t.kernel.capacity = WORKER_TEST_SQ_MAX;
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( t.worker.wake_armed );
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );

    // a receive multishot that ends is armed again
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ENOBUFS, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 2 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_sleeps_only_with_the_wake_armed );

// RNUringQueueWorker Submit: what main publishes goes out one SENDMSG a run,
// grouped by destination in the worker, and the bank retires once each run
// has its completion

static void test_uring_queue_worker_sends_what_main_publishes()
{
    worker_test_t t( 8, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.stage( 1, 100 );
    t.stage( 2, 100 );
    t.stage( 1, 100 );
    t.stage( 1, 60 );
    t.publish();

    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> runs = t.sends();
    check( runs.size() == 2 );
    check( worker_test_iovecs( runs[0] ) == 3 );
    check( worker_test_segmented( runs[0] ) );
    check( worker_test_iovecs( runs[1] ) == 1 );
    check( !worker_test_segmented( runs[1] ) );
    check( runs[0].fd == 7 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS ) == 1 );
    check( t.worker.in_kernel == 2 );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) >= 0 );

    // one completion: the bank is still the kernel's
    t.post( runs[1].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) >= 0 );

    t.post( runs[0].user_data, 260, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) < 0 );
    check( t.worker.in_kernel == 0 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 4 );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_sends_what_main_publishes );

// runs in the kernel at once stay within max_in_kernel and the submission
// queue's room; the rest wait for a completion or a submit

static void test_uring_queue_worker_bounds_runs_in_the_kernel()
{
    worker_test_t t( 8, 2, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    for ( int d = 1; d <= 5; d++ )
    {
        t.stage( d, 100 );
    }
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 2 );

    t.post( t.sends()[0].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 3 );

    // one free submission entry a pass: each run goes out after a submit
    worker_test_t s( 8, 8, 2 );
    check( s.ok );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    for ( int d = 1; d <= 3; d++ )
    {
        s.stage( d, 100 );
    }
    s.kernel.capacity = 1;
    s.publish();
    check( s.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( s.sends().size() == 1 );
    check( s.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( s.sends().size() == 2 );
    check( s.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( s.sends().size() == 3 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_bounds_runs_in_the_kernel );

// RNUringQueueWorker ReapAgain and ReapPoll: -EAGAIN hands the run back and
// arms a POLLOUT poll; nothing is submitted until the poll fires, then the
// same run goes first

static void test_uring_queue_worker_again_waits_for_the_poll()
{
    worker_test_t t( 8, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.stage( 1, 100 );
    t.stage( 2, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> runs = t.sends();
    check( runs.size() == 2 );

    t.post( runs[0].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.blocked );
    check( t.worker.poll_armed );
    check( t.of( IORING_OP_POLL_ADD ).size() == 1 );
    check( t.of( IORING_OP_POLL_ADD )[0].user_data == rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, 0, 0 ) );
    check( t.sends().size() == 2 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL ) == 1 );

    // the other run completes; still blocked: nothing goes out
    t.post( runs[1].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 2 );

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, 0, 0 ), POLLOUT, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( !t.worker.blocked );
    check( t.sends().size() == 3 );
    check( t.sends()[2].user_data == runs[0].user_data );

    t.post( runs[0].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_again_waits_for_the_poll );

// RNUringQueueWorker Observe, CancelRecv, CancelWake, Exit: after the stop
// the worker cancels its receive and wake read, sends what is published, and
// says exit only once nothing of its is in the kernel. Each completion
// arrives on its own; no pass before the last says exit (the witness "exit
// before drain")

static void test_uring_queue_worker_stop_drains_before_exit()
{
    worker_test_t t( 8, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.stage( 1, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> runs = t.sends();
    check( runs.size() == 1 );

    // a bank published just before the stop is still sent
    t.stage( 2, 100 );
    check( rocketnet_uring_queue_send_publish( t.send, 0 ) );
    rocketnet_uring_queue_worker_stop( &t.worker );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );

    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.seen );
    check( t.receive.stopping );
    std::vector<struct io_uring_sqe> cancels = t.of( IORING_OP_ASYNC_CANCEL );
    check( cancels.size() == 1 );       // the receive's: the wake read had completed
    check( cancels[0].addr == rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ) );
    check( t.of( IORING_OP_READ ).size() == 2 );   // not armed again after the stop

    struct rocketnet_uring_queue_worker_completion_t last[3];
    last[0] = { runs[0].user_data, 100, 0 };
    last[1] = { rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_RECV ), 0, 0 };
    last[2] = { rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 };
    for ( int i = 0; i < 3; i++ )
    {
        t.kernel.ready.push_back( last[i] );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    }

    // the second bank's run is still the kernel's
    check( t.sends().size() == 2 );
    t.post( t.sends()[1].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 2 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_stop_drains_before_exit );

// the stop with the wake read still armed cancels it too, and an armed
// read's cancel and its own completion are both waited for

static void test_uring_queue_worker_stop_cancels_the_wake_read()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    rocketnet_uring_queue_worker_stop( &t.worker );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.of( IORING_OP_ASYNC_CANCEL ).size() == 2 );

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_RECV ), 0, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_WAKE ), 0, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_stop_cancels_the_wake_read );

// RNUringQueueWorker Abandon: after the stop, a socket that answers -EAGAIN
// max_retry times has what waits abandoned (counted) and its poll
// cancelled, so the stop finishes (the witness "unbounded retry" is a
// socket that holds the stop)

static void test_uring_queue_worker_stop_abandons_a_full_socket()
{
    worker_test_t t( 8, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.stage( 1, 100 );
    t.stage( 1, 100 );
    t.stage( 2, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> runs = t.sends();
    check( runs.size() == 2 );

    rocketnet_uring_queue_worker_stop( &t.worker );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_RECV ), 0, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 );

    // the first -EAGAIN after the stop: blocked, a poll armed
    t.post( runs[0].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.agains == 1 );
    check( t.worker.poll_armed );
    check( t.sends().size() == 2 );

    // the second: the bound reached with the poll armed, which is
    // cancelled; both handed-back runs are abandoned and the bank retires
    t.post( runs[1].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.agains == 2 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 3 );
    std::vector<struct io_uring_sqe> cancels = t.of( IORING_OP_ASYNC_CANCEL );
    check( cancels.size() == 2 );
    check( cancels[1].addr == rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, 0, 0 ) );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) < 0 );

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_POLL ), 0, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( t.sends().size() == 2 );

    // the bound reached with no poll armed: nothing to cancel, and the
    // poll the -EAGAIN asked for is never armed
    worker_test_t u( 8, 8, 1 );
    check( u.ok );
    check( u.settle() == ROCKETNET_URING_WORKER_SLEEP );
    u.stage( 1, 100 );
    u.publish();
    check( u.settle() == ROCKETNET_URING_WORKER_SLEEP );
    rocketnet_uring_queue_worker_stop( &u.worker );
    u.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( u.settle() == ROCKETNET_URING_WORKER_SLEEP );
    u.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_RECV ), 0, 0 );
    u.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 );
    check( u.settle() == ROCKETNET_URING_WORKER_SLEEP );
    u.post( u.sends()[0].user_data, -EAGAIN, 0 );
    check( u.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( u.of( IORING_OP_POLL_ADD ).size() == 0 );
    check( u.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 1 );
    check( u.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_stop_abandons_a_full_socket );

// a segmented run the destination refuses comes back as one plain run a
// packet; the destination is remembered, so its next segmented run is cut
// plain before any send

static void test_uring_queue_worker_refused_destination_goes_plain()
{
    worker_test_t t( 8, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.stage( 1, 100 );
    t.stage( 1, 100 );
    t.stage( 1, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 1 );
    check( worker_test_segmented( t.sends()[0] ) );

    t.post( t.sends()[0].user_data, -EIO, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> plain = t.sends( 1 );
    check( plain.size() == 3 );
    for ( size_t i = 0; i < plain.size(); i++ )
    {
        check( worker_test_iovecs( plain[i] ) == 1 );
        check( !worker_test_segmented( plain[i] ) );
        t.post( plain[i].user_data, 100, 0 );
    }
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_REFUSED ) == 1 );

    const size_t before = t.kernel.submitted.size();
    t.stage( 1, 100 );
    t.stage( 1, 100 );
    t.stage( 2, 100 );
    t.stage( 2, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> next = t.sends( before );
    check( next.size() == 3 );
    int segmented = 0;
    for ( size_t i = 0; i < next.size(); i++ )
    {
        segmented += worker_test_segmented( next[i] ) ? 1 : 0;
    }
    check( segmented == 1 );       // destination 2's run only
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_REFUSED ) == 1 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_refused_destination_goes_plain );

// rocketnet_server_gso.h's threshold: that many destinations refuse and none
// was accepted, and the queue stops segmenting

static void test_uring_queue_worker_refusals_turn_segmentation_off()
{
    const int destinations = ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
    worker_test_t t( 4 * destinations, 4 * destinations, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    for ( int d = 1; d <= destinations; d++ )
    {
        t.stage( d, 100 );
        t.stage( d, 100 );
    }
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> runs = t.sends();
    check( (int) runs.size() == destinations );
    for ( size_t i = 0; i < runs.size(); i++ )
    {
        check( t.counter( ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS ) == 0 );
        t.post( runs[i].user_data, -EINVAL, 0 );
    }
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS ) == 1 );
    check( !t.worker.config.segment );

    // the refused runs went out again one packet a run
    std::vector<struct io_uring_sqe> all = t.sends();
    check( (int) all.size() == 3 * destinations );
    for ( size_t i = (size_t) destinations; i < all.size(); i++ )
    {
        check( !worker_test_segmented( all[i] ) );
        t.post( all[i].user_data, 100, 0 );
    }
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
    const size_t before = t.kernel.submitted.size();

    // a destination that never refused now goes plain too
    t.stage( 99, 100 );
    t.stage( 99, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> next = t.sends( before );
    check( next.size() == 2 );
    check( !worker_test_segmented( next[0] ) );
    check( !worker_test_segmented( next[1] ) );

    // one accepted segmented send keeps segmentation on whatever follows
    worker_test_t a( 4 * destinations, 4 * destinations, 2 );
    check( a.ok );
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );
    a.stage( 100, 100 );
    a.stage( 100, 100 );
    a.publish();
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );
    a.post( a.sends()[0].user_data, 200, 0 );
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );
    for ( int d = 1; d <= destinations; d++ )
    {
        a.stage( d, 100 );
        a.stage( d, 100 );
    }
    a.publish();
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );
    for ( size_t i = 1; i < a.sends().size(); i++ )
    {
        a.post( a.sends()[i].user_data, -EINVAL, 0 );
    }
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( a.counter( ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS ) == 0 );
    check( a.worker.config.segment );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_refusals_turn_segmentation_off );

// a destination the socket's family cannot name is dropped without a send,
// and a worker created without segmentation cuts every run plain

static void test_uring_queue_worker_drops_an_unnamed_destination()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV6;
    to.data.ipv6[0] = 0x2001;
    to.data.ipv6[1] = 0xdb8;
    to.data.ipv6[7] = 1;
    to.port = 40000;
    t.stage_to( &to, 100 );
    t.stage_to( &to, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 0 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_SEND_DROPPED ) == 2 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED ) == 2 );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );

    worker_test_t p( 4, 8, 2, false );
    check( p.ok );
    check( p.settle() == ROCKETNET_URING_WORKER_SLEEP );
    p.stage( 1, 100 );
    p.stage( 1, 100 );
    p.publish();
    check( p.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( p.sends().size() == 2 );
    check( p.counter( ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS ) == 0 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_drops_an_unnamed_destination );

// completions that name nothing of the worker's are counted and change
// nothing

static void test_uring_queue_worker_counts_stray_completions()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 1, 0 ), 100, 0 );       // another queue's
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 ), 100, 0 );       // no run in the kernel
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, 0, 0 ), POLLOUT, 0 );   // no poll armed
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, 0 ), 0, 0 );       // no cancel
    t.post( rocketnet_uring_tag( 9, 0, 0 ), 0, 0 );                                // no such kind
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 5 );
    check( t.worker.in_kernel == 0 );
    check( !t.worker.blocked );
    check( t.worker.wake_armed );

    // a wake read that is not armed
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 6 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_counts_stray_completions );

// a full batch of completions leaves the rest for the next pass, counted

static void test_uring_queue_worker_reaps_a_batch_a_pass()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    for ( int i = 0; i < ROCKETNET_URING_WORKER_CQE_BATCH + 3; i++ )
    {
        t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), 100, IORING_CQE_F_MORE );
    }
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( t.receive.cqes == ROCKETNET_URING_WORKER_CQE_BATCH );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED ) == 1 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.cqes == ROCKETNET_URING_WORKER_CQE_BATCH + 3 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED ) == 1 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_reaps_a_batch_a_pass );

#endif // #if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING && defined( IORING_RECV_MULTISHOT )

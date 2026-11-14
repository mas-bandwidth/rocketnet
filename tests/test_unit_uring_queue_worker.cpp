/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server's per-queue worker (sdk/rocketnet_uring_queue_worker.h,
// layer L3), at the rules proofs/tla/RNUringQueueWorker.tla checks: the
// worker sleeps only with its wake read armed, submits the runs of what main
// publishes, stops submitting after a full answer (-EAGAIN, -EINTR,
// -ENOBUFS, -ENOMEM: every packet kept) until its retry timer fires (or,
// before shutdown, main's wake), never a POLLOUT poll; once shutdown has
// begun counts its waits as each begins, starting again when a run is done
// or a bank claimed, and abandons what waits once max_retry waits have begun
// (at the start of the last, not after its timer), so the queue turns idle before the stop; after the stop cancels its
// receive and wake read, never sleeps with a step the full submission queue had no
// room for still to take, and exits only with
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

#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_uring_counters.h"

#include <errno.h>
#include <string.h>
#include <vector>

extern const rocketnet_transport_backend_t rocketnet_io_uring_transport;

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
        completions[n] = k->ready[(size_t) n];
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
    int q = 0;              // the worker's queue
    bool owner = true;      // the banks and the send are this fixture's

    worker_test_t( int slots, int max_in_kernel, int max_retry, bool segment = true, bool ipv6 = false, int queues = 1 )
    {
        banks = rocketnet_uring_send_queues_create( NULL, queues, slots );
        send = banks ? rocketnet_uring_queue_send_create( NULL, banks, 4, 1 << 16 ) : NULL;
        struct rocketnet_uring_queue_worker_config_t config = config_for( max_in_kernel, max_retry, segment, ipv6 );
        ok = send && rocketnet_uring_queue_worker_init( &worker, &config );
    }

    // a worker on queue `queue` of another fixture's transport (its banks and
    // send), with a fake kernel of its own; destroyed before that fixture
    worker_test_t( worker_test_t & transport, int queue, int max_in_kernel, int max_retry )
    {
        banks = transport.banks;
        send = transport.send;
        q = queue;
        owner = false;
        struct rocketnet_uring_queue_worker_config_t config = config_for( max_in_kernel, max_retry, true, false );
        ok = send && rocketnet_uring_queue_worker_init( &worker, &config );
    }

    ~worker_test_t()
    {
        if ( owner )
        {
            rocketnet_uring_queue_send_destroy( send );
            rocketnet_uring_send_queues_destroy( banks );
        }
    }

    struct rocketnet_uring_queue_worker_config_t config_for( int max_in_kernel, int max_retry, bool segment, bool ipv6 )
    {
        struct rocketnet_uring_queue_worker_config_t config;
        memset( &config, 0, sizeof( config ) );
        config.queue_index = q;
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
        check( rocketnet_uring_queue_send_publish( send, q ) );
        post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, q, 0 ), 8, 0 );
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
        return rocketnet_uring_queue_send_counter( send, q, c );
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

    config = t.config_for( 8, 2, true, false );
    config.queue_index = -1;
    check( !rocketnet_uring_queue_worker_init( &worker, &config ) );

    // every kernel call is needed
    for ( int missing = 0; missing < 5; missing++ )
    {
        config = t.config_for( 8, 2, true, false );
        if ( missing == 0 )
        {
            config.kernel.space = NULL;
        }
        else if ( missing == 1 )
        {
            config.kernel.sqe = NULL;
        }
        else if ( missing == 2 )
        {
            config.kernel.submit = NULL;
        }
        else if ( missing == 3 )
        {
            config.kernel.advance = NULL;
        }
        else
        {
            config.kernel.peek = NULL;
        }
        check( !rocketnet_uring_queue_worker_init( &worker, &config ) );
    }

    // a receive needs every call
    for ( int missing = 0; missing < 4; missing++ )
    {
        config = t.config_for( 8, 2, true, false );
        if ( missing == 0 )
        {
            config.receive.needs_arm = NULL;
        }
        else if ( missing == 1 )
        {
            config.receive.arm = NULL;
        }
        else if ( missing == 2 )
        {
            config.receive.cqe = NULL;
        }
        else
        {
            config.receive.stop = NULL;
        }
        check( !rocketnet_uring_queue_worker_init( &worker, &config ) );
    }

    // no receive at all is a worker that only sends
    config = t.config_for( 8, 2, true, false );
    memset( &config.receive, 0, sizeof( config.receive ) );
    check( rocketnet_uring_queue_worker_init( &worker, &config ) );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_init_refuses_bad_config );

// RNUringQueueWorker ArmWake and Sleep: the first pass arms the receive and
// the wake read and then sleeps (its submit waits); it never sleeps before
// the wake read is armed

static void test_uring_queue_worker_sleeps_only_with_the_wake_armed()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );

    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 1 );
    check( t.of( IORING_OP_RECVMSG ).size() == 1 );
    check( t.of( IORING_OP_READ ).size() == 1 );
    check( t.of( IORING_OP_READ )[0].user_data == rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ) );
    check( t.worker.wake_armed );
    check( t.kernel.waits == 1 );
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.kernel.submitted.size() == 2 );

    // the wake completes: the read is armed again before the next sleep
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.of( IORING_OP_READ ).size() == 2 );

    // with no submission entry free the wake cannot be armed: the pass that
    // reaps it does not sleep, and the read is armed once there is room
    t.kernel.capacity = 0;
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( !t.worker.wake_armed );
    t.kernel.capacity = WORKER_TEST_SQ_MAX;
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.wake_armed );

    // a receive multishot that ends is armed again
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ENOBUFS, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 2 );

    // with no submission entry free the receive cannot be armed again: the
    // module is handed none and refuses, and the pass does not sleep (a
    // datagram first, so this end is again the first refused in a row)
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), 100, IORING_CQE_F_MORE );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    t.kernel.capacity = 0;
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ENOBUFS, 0 );
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( t.receive.arm_refused == 1 );
    check( !t.receive.armed );
    t.kernel.capacity = WORKER_TEST_SQ_MAX;
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 3 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_sleeps_only_with_the_wake_armed );

// the receive's retry timer's completion: the timeout expired
static uint64_t worker_test_receive_timer()
{
    return rocketnet_uring_tag( ROCKETNET_URING_TAG_TIMER, 0, ROCKETNET_URING_TIMER_RECV );
}

// RNUringQueueWorker ReapRecvEnd, ArmRecvTimer, ReapRecvTimer, ArmRecv
// (RecvNoSpin): a multishot that ends refused (no buffer free, or a kernel
// that refuses the receive) is armed again at once the first time; from the
// second refused end in a row with no datagram between, each arm waits for
// the receive's retry timer, so a receive the kernel keeps refusing is armed
// once a retry period and never at completion speed. The worker sleeps
// meanwhile. A datagram starts the count again. Each refusal is the receive
// module's to count (RECEIVE_FAILED, RECEIVE_NO_BUFFERS).

static void test_uring_queue_worker_refused_receive_waits_for_the_timer()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 1 );
    const uint64_t recv = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 );

    // the first refused end: armed again at once (a burst that took every
    // buffer gives them back as its completions are reaped)
    t.post( recv, -ENOBUFS, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 2 );
    check( t.of( IORING_OP_TIMEOUT ).size() == 0 );

    // the second in a row: no arm until the receive's timer fires, and the
    // worker sleeps with that timer in the kernel; passes before it arm nothing
    t.post( recv, -EINVAL, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 2 );
    check( !t.receive.armed );
    std::vector<struct io_uring_sqe> timers = t.of( IORING_OP_TIMEOUT );
    check( timers.size() == 1 );
    check( timers[0].user_data == worker_test_receive_timer() );
    const struct __kernel_timespec * spec = (const struct __kernel_timespec *) (uintptr_t) timers[0].addr;
    check( spec->tv_sec == 0 );
    check( spec->tv_nsec == (long long) ( ROCKETNET_URING_SEND_RETRY_SECONDS * 1000000000.0 ) );
    const size_t submitted = t.kernel.submitted.size();
    for ( int i = 0; i < 8; i++ )
    {
        check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    }
    check( t.kernel.submitted.size() == submitted );
    check( t.receive.arms == 2 );

    // the timer fires: one arm
    t.post( worker_test_receive_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 3 );

    // a kernel that keeps refusing: each arm waits a whole retry period, never
    // two arms in one
    for ( int i = 0; i < 100; i++ )
    {
        t.post( recv, -EINVAL, 0 );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        check( t.receive.arms == 3 + i );
        check( t.of( IORING_OP_TIMEOUT ).size() == (size_t) ( 2 + i ) );
        t.post( worker_test_receive_timer(), -ETIME, 0 );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        check( t.receive.arms == 4 + i );
    }
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );

    // a datagram starts the count again: the next refused end is armed at once
    t.post( recv, 100, IORING_CQE_F_MORE );
    t.post( recv, -ENOBUFS, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 104 );
    check( t.of( IORING_OP_TIMEOUT ).size() == 101 );

    // the send's retry timer is not the receive's: a full answer while the
    // receive waits arms its own, and neither's completion ends the other's wait
    t.post( recv, -EINVAL, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.of( IORING_OP_TIMEOUT ).size() == 102 );
    t.stage( 1, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 1 );
    t.post( t.sends()[0].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.blocked );
    check( t.of( IORING_OP_TIMEOUT ).size() == 103 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_TIMER, 0, ROCKETNET_URING_TIMER_SEND ), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( !t.worker.blocked );
    check( t.sends().size() == 2 );
    check( t.receive.arms == 104 );     // the receive still waits for its own
    t.post( worker_test_receive_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 105 );
    t.post( t.sends()[1].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );

    // with no submission entry free the receive's timer cannot be armed: the
    // pass does not sleep, and the timer is armed once there is room
    t.post( recv, -EINVAL, 0 );
    t.kernel.capacity = 0;
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( !t.worker.recv_timer_armed );
    t.kernel.capacity = WORKER_TEST_SQ_MAX;
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.recv_timer_armed );
    check( t.receive.arms == 105 );

    // a stop while the receive waits: never armed again, and the worker
    // exits once the timer it has in the kernel fires
    rocketnet_uring_queue_worker_stop( &t.worker );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );        // the receive's timer is still the kernel's
    t.post( worker_test_receive_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( t.receive.arms == 105 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );

    // a timer completion with nothing armed is stray
    worker_test_t s( 4, 8, 2 );
    check( s.ok );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    s.post( worker_test_receive_timer(), -ETIME, 0 );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( s.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 1 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_refused_receive_waits_for_the_timer );

// a multishot that ends with a result of 0 (no IORING_CQE_F_MORE) carried no
// datagram (a multishot RECVMSG result holds at least its header): it is an
// end like any refused one, so a kernel that keeps ending the receive that
// way is armed once a retry period too, never at completion speed

static void test_uring_queue_worker_empty_receive_end_waits_for_the_timer()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 1 );
    const uint64_t recv = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 );

    t.post( recv, 0, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 2 );

    t.post( recv, 0, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 2 );
    check( t.of( IORING_OP_TIMEOUT ).size() == 1 );

    t.post( worker_test_receive_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.receive.arms == 3 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_empty_receive_end_waits_for_the_timer );

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
    // MSG_DONTWAIT: a full socket answers -EAGAIN instead of a wait in the kernel
    check( ( runs[0].msg_flags & MSG_DONTWAIT ) != 0 );
    check( ( runs[1].msg_flags & MSG_DONTWAIT ) != 0 );
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

// the retry timer's completion: the timeout expired
static uint64_t worker_test_timer()
{
    return rocketnet_uring_tag( ROCKETNET_URING_TAG_TIMER, 0, 0 );
}

// RNUringQueueWorker ReapAgain, ArmTimer and ReapTimer: a full answer hands
// the run back and arms the retry timer (a 1 ms timeout, never a POLLOUT
// poll); nothing is submitted until the timer fires or, before shutdown,
// main's wake comes, then the same run goes first

static void test_uring_queue_worker_again_waits_for_the_timer()
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
    check( t.worker.timer_armed );
    std::vector<struct io_uring_sqe> timers = t.of( IORING_OP_TIMEOUT );
    check( timers.size() == 1 );
    check( timers[0].user_data == worker_test_timer() );
    const struct __kernel_timespec * spec = (const struct __kernel_timespec *) (uintptr_t) timers[0].addr;
    check( spec->tv_sec == 0 );
    check( spec->tv_nsec == (long long) ( ROCKETNET_URING_SEND_RETRY_SECONDS * 1000000000.0 ) );
    check( timers[0].len == 1 );
    check( timers[0].off == 0 );        // a pure timeout: no completion count ends it
    check( t.of( IORING_OP_POLL_ADD ).size() == 0 );
    check( t.sends().size() == 2 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL ) == 1 );
    check( t.worker.agains == 0 );      // before shutdown nothing counts toward the abandon

    // the other run completes; still blocked: nothing goes out
    t.post( runs[1].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 2 );

    t.post( worker_test_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( !t.worker.blocked );
    check( !t.worker.timer_armed );
    check( t.sends().size() == 3 );
    check( t.sends()[2].user_data == runs[0].user_data );

    t.post( runs[0].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
    check( t.worker.agains == 0 );

    // before shutdown main's wake ends the wait too: the run goes again with
    // the timer still in the kernel, and the timer's completion, when it
    // comes, is the worker's, not stray
    t.stage( 1, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 4 );
    t.post( t.sends()[3].user_data, -ENOBUFS, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.blocked );
    check( t.of( IORING_OP_TIMEOUT ).size() == 2 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( !t.worker.blocked );
    check( t.worker.timer_armed );
    check( t.sends().size() == 5 );
    // full again while the timer is in the kernel: that timer is the wait,
    // no second one
    t.post( t.sends()[4].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.blocked );
    check( t.of( IORING_OP_TIMEOUT ).size() == 2 );
    t.post( worker_test_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 6 );
    t.post( t.sends()[5].user_data, 100, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );

    // with no submission entry free the timer cannot be armed: the pass does
    // not sleep, and the timer is armed once there is room
    t.stage( 1, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    t.kernel.capacity = 0;
    t.post( t.sends()[6].user_data, -EAGAIN, 0 );
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( !t.worker.timer_armed );
    t.kernel.capacity = WORKER_TEST_SQ_MAX;
    check( t.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.timer_armed );
    check( t.of( IORING_OP_TIMEOUT ).size() == 3 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_again_waits_for_the_timer );

// the full answers (rocketnet_uring_queue_send_result SOCKET_FULL): -EAGAIN,
// and -EINTR, -ENOBUFS and -ENOMEM, which a send of many coalesced packets
// also gets while the socket or the device queue behind it has no room. Each
// keeps every packet of its run for the retry: none is dropped

static void test_uring_queue_worker_full_answers_keep_every_packet()
{
    const int32_t full[] = { -EAGAIN, -EINTR, -ENOBUFS, -ENOMEM };
    for ( int f = 0; f < 4; f++ )
    {
        worker_test_t t( 8, 8, 2 );
        check( t.ok );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

        for ( int i = 0; i < 3; i++ )
        {
            t.stage( 1, 100 );
        }
        t.publish();
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        check( t.sends().size() == 1 );
        check( worker_test_iovecs( t.sends()[0] ) == 3 );

        t.post( t.sends()[0].user_data, full[f], 0 );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        check( t.worker.blocked );
        check( t.counter( ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL ) == 1 );
        check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED ) == 0 );

        t.post( worker_test_timer(), -ETIME, 0 );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        check( t.sends().size() == 2 );
        check( t.sends()[1].user_data == t.sends()[0].user_data );
        check( worker_test_iovecs( t.sends()[1] ) == 3 );

        t.post( t.sends()[1].user_data, 300, 0 );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 3 );
        check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED ) == 0 );
        check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
    }
}

REGISTER_UNIT_TEST( test_uring_queue_worker_full_answers_keep_every_packet );

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

// the stop with the wake read still armed cancels it too, and the exit
// waits for the read's own completion, not for the cancels' (a cancel holds
// nothing of the kernel's)

static void test_uring_queue_worker_stop_cancels_the_wake_read()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    rocketnet_uring_queue_worker_stop( &t.worker );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.of( IORING_OP_ASYNC_CANCEL ).size() == 2 );

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );       // the wake read is still the kernel's

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );         // neither cancel's completion has come
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );

    // with no submission entry free the cancels cannot be prepared: the pass
    // does not sleep, and they go once there is room
    worker_test_t s( 4, 8, 2 );
    check( s.ok );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    s.kernel.capacity = 0;
    rocketnet_uring_queue_worker_stop( &s.worker );
    check( s.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( s.of( IORING_OP_ASYNC_CANCEL ).size() == 0 );
    s.kernel.capacity = WORKER_TEST_SQ_MAX;
    check( s.step() == ROCKETNET_URING_WORKER_SLEEP );
    check( s.of( IORING_OP_ASYNC_CANCEL ).size() == 2 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_stop_cancels_the_wake_read );

// RNUringQueueWorker StopFinishes with datagrams still coming (the model's
// fairness on CancelRecv and CancelWake): a pass reaps at most
// ROCKETNET_URING_WORKER_CQE_BATCH completions, then goes on to the rest of
// the step, so with more datagrams ready than a batch the one pass after the
// stop still cancels the receive and the wake read

static void test_uring_queue_worker_stop_cancels_under_a_flood()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    const uint64_t recv = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 );

    rocketnet_uring_queue_worker_stop( &t.worker );
    for ( int i = 0; i < ROCKETNET_URING_WORKER_CQE_BATCH + 44; i++ )
    {
        t.post( recv, 100, IORING_CQE_F_MORE );
    }
    check( t.step() == ROCKETNET_URING_WORKER_CONTINUE );
    check( t.of( IORING_OP_ASYNC_CANCEL ).size() == 2 );
    check( t.kernel.ready.size() == 44 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED ) == 1 );

    // the datagrams already posted are still reaped, then the cancelled
    // receive's and wake read's own ends, and the worker exits
    t.post( recv, -ECANCELED, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( t.receive.arms == 1 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_stop_cancels_under_a_flood );

// the public rows the transport reads for one queue
// (rocketnet_server_uring_counters through rocketnet_uring_counters_add_queue)
static std::vector<uint64_t> worker_test_rows( const worker_test_t & t )
{
    std::vector<uint64_t> rows( ROCKETNET_NUM_COUNTERS, 0 );
    uint64_t receive[ROCKETNET_URING_RECEIVE_NUM_COUNTERS];
    memset( receive, 0, sizeof( receive ) );
    uint64_t worker[ROCKETNET_URING_WORKER_NUM_COUNTERS];
    for ( int c = 0; c < ROCKETNET_URING_WORKER_NUM_COUNTERS; c++ )
    {
        worker[c] = t.counter( c );
    }
    rocketnet_uring_counters_add_queue( rows.data(), receive, worker, t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ),
                                        t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED ),
                                        t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) );
    return rows;
}

// the io_uring transport has the driver's begin_shutdown (it was NULL, so the
// worker never learned the server was stopping: finding E1)

static void test_uring_queue_worker_transport_begins_shutdown()
{
    check( rocketnet_io_uring_transport.server != NULL );
    check( rocketnet_io_uring_transport.server->begin_shutdown != NULL );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_transport_begins_shutdown );

// RNUringQueueWorker BeginStop, ReapAgain after shutdown, Abandon
// (ShutdownDrains; the witness "shutdown none" is the transport before the
// fix): a bank held on a socket that stays full, POLLOUT never ready. Once
// shutdown has begun, and with no stop yet (the driver asks it only once
// send_idle), each wait counts, one a wait however many runs the burst
// refused, and at max_retry what waits is abandoned and counted, so the
// queue turns idle and the server's stop goes on. No core is burned: each
// wait is one timer, and nothing is sent between a full answer and its
// timer

static void test_uring_queue_worker_shutdown_drains_a_full_socket()
{
    worker_test_t t( 8, 8, 3 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.stage( 1, 100 );
    t.stage( 1, 100 );
    t.stage( 2, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 2 );

    // full before shutdown: a wait that does not count
    t.post( t.sends()[0].user_data, -EAGAIN, 0 );
    t.post( t.sends()[1].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.agains == 0 );
    check( t.of( IORING_OP_TIMEOUT ).size() == 1 );

    // the server begins its stop: shutdown, and main's wake
    rocketnet_uring_queue_worker_begin_shutdown( &t.worker );
    check( t.worker.shutdown.load() );
    check( !t.worker.quit.load() );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( !t.worker.seen );

    // from shutdown on main's wakes (its last disconnect batches) do not end
    // a wait: only the timer does, so each counted wait is a whole period
    for ( int i = 0; i < 3; i++ )
    {
        t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    }
    check( t.worker.blocked );
    check( t.sends().size() == 2 );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );

    // the socket stays full: every run answers full, and the worker waits on
    // its timer between tries
    size_t answered = 2;
    int waits = 0;
    for ( int round = 0; round < 16 && !rocketnet_uring_queue_send_idle( t.send, 0 ); round++ )
    {
        const size_t sent = t.sends().size();
        for ( ; answered < sent; answered++ )
        {
            t.post( t.sends()[answered].user_data, -EAGAIN, 0 );
        }
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        check( t.sends().size() == sent );      // nothing goes out before the timer
        if ( t.worker.timer_armed )
        {
            t.post( worker_test_timer(), -ETIME, 0 );
            check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
            waits++;
        }
    }

    // the bound reached after exactly max_retry counted waits (the wait
    // before shutdown does not count): both runs abandoned and counted, the
    // bank retired, the queue idle, the worker still running (no stop yet).
    // Four timers in all, the last armed by the third counted wait's refusal
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );
    check( t.worker.agains == 3 );
    check( waits == 4 );
    check( answered == 8 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 3 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 0 );
    check( t.of( IORING_OP_POLL_ADD ).size() == 0 );
    check( t.of( IORING_OP_TIMEOUT ).size() == 4 );
    const std::vector<uint64_t> rows = worker_test_rows( t );
    check( rows[ROCKETNET_COUNTER_URING_SEND_DROPPED] == 3 );
    check( rows[ROCKETNET_COUNTER_URING_SEND_DEFERRED] == t.counter( ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL ) );
    check( rows[ROCKETNET_COUNTER_URING_SEND_DEFERRED] == (uint64_t) answered );
    check( rows[ROCKETNET_COUNTER_URING_PACKETS_SENT] == 0 );

    // the driver asks the stop now: the worker cancels its receive and wake
    // read, and exits once its last timer has fired
    rocketnet_uring_queue_worker_stop( &t.worker );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_RECV ), 0, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 );
    if ( t.worker.timer_armed )
    {
        check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
        t.post( worker_test_timer(), -ETIME, 0 );
    }
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_STRAY ) == 0 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_shutdown_drains_a_full_socket );

// RNUringQueueWorker ReapOk and Claim (finding L1): the bound counts waits
// begun with no run done. A socket that drains slowly after the stop, one run a
// wait, never loses a packet however many waits it takes; and a new bank
// claimed after an abandon gets the bound afresh instead of being abandoned
// unsent

static void test_uring_queue_worker_retry_bound_resets_on_progress()
{
    worker_test_t t( 8, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    for ( int d = 1; d <= 5; d++ )
    {
        t.stage( d, 100 );
    }
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 5 );

    rocketnet_uring_queue_worker_stop( &t.worker );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.seen );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_RECV ), 0, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    // each wait one run goes out and the rest answer full: five waits on a
    // bound of two, and every packet is sent
    size_t answered = 0;
    int waits = 0;
    for ( int round = 0; round < 16 && rocketnet_uring_queue_send_held( t.send, 0 ) >= 0; round++ )
    {
        const size_t sent = t.sends().size();
        t.post( t.sends()[answered++].user_data, 100, 0 );
        for ( ; answered < sent; answered++ )
        {
            t.post( t.sends()[answered].user_data, -EAGAIN, 0 );
        }
        const int next = t.settle();
        check( t.worker.agains <= 1 );
        if ( next == ROCKETNET_URING_WORKER_EXIT )
        {
            break;
        }
        if ( t.worker.timer_armed )
        {
            t.post( worker_test_timer(), -ETIME, 0 );
            waits++;
            if ( t.settle() == ROCKETNET_URING_WORKER_EXIT )
            {
                break;
            }
        }
    }
    check( waits == 4 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 5 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 0 );
    check( worker_test_rows( t )[ROCKETNET_COUNTER_URING_SEND_DROPPED] == 0 );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );

    // a bank abandoned once shutdown has begun, then a new one published
    // (the server's last disconnect batches): the new bank is sent. With
    // max_retry 1 the first full answer after shutdown begins the one wait
    // the bound allows, and what waits is abandoned at that wait's start,
    // before its timer: the bound counts waits begun, not waits completed
    worker_test_t s( 8, 8, 1 );
    check( s.ok );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    rocketnet_uring_queue_worker_begin_shutdown( &s.worker );
    s.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    s.stage( 1, 100 );
    s.publish();
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    s.post( s.sends()[0].user_data, -EAGAIN, 0 );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( s.worker.agains == 1 );
    check( s.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 1 );
    check( rocketnet_uring_queue_send_idle( s.send, 0 ) );

    s.stage( 2, 100 );
    s.publish();
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( s.worker.agains == 0 );
    check( s.sends().size() == 1 );     // claimed; the wait the abandon left goes on to its timer
    s.post( worker_test_timer(), -ETIME, 0 );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( s.sends().size() == 2 );     // sent, not abandoned
    s.post( s.sends()[1].user_data, 100, 0 );
    check( s.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( s.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 1 );
    check( s.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 1 );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_retry_bound_resets_on_progress );

// RNUringQueueWorker Observe, ReapAgain after the stop, Abandon: a socket
// full before the stop keeps its wait when the stop is seen (only the timer
// ends it); after the stop each wait counts, and at max_retry what waits is
// abandoned (counted), so the stop finishes (the witness "unbounded retry"
// is a socket that holds the stop)

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

    // full before the stop: blocked, the timer armed, nothing counted
    t.post( runs[0].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.blocked );
    check( t.worker.timer_armed );
    check( t.worker.agains == 0 );

    // the stop: the receive is cancelled, and the wake that brought it does
    // not end the wait: nothing goes out until the timer fires
    rocketnet_uring_queue_worker_stop( &t.worker );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.seen );
    check( t.worker.blocked );
    std::vector<struct io_uring_sqe> cancels = t.of( IORING_OP_ASYNC_CANCEL );
    check( cancels.size() == 1 );
    check( cancels[0].addr == rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ) );
    check( t.sends().size() == 2 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, ROCKETNET_URING_CANCEL_RECV ), 0, 0 );
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    t.post( worker_test_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 3 );
    check( t.sends()[2].user_data == runs[0].user_data );

    // the first full answer after the stop starts a wait and counts; the
    // second, in the same wait, does not
    t.post( runs[1].user_data, -EAGAIN, 0 );
    t.post( runs[0].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.agains == 1 );
    check( t.worker.blocked );
    check( t.worker.timer_armed );
    check( t.sends().size() == 3 );

    // the timer: both runs again, both full: the bound is reached, and what
    // waits is abandoned, not sent
    t.post( worker_test_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 5 );
    t.post( t.sends()[3].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.worker.agains == 2 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == (uint64_t) worker_test_iovecs( t.sends()[3] ) );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT ) == 0 );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) >= 0 );     // one run is still the kernel's

    // the last run in the kernel answers full too: abandoned, the bank
    // retires, and the worker exits once its timer has fired
    t.post( t.sends()[4].user_data, -EAGAIN, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 3 );
    check( rocketnet_uring_queue_send_held( t.send, 0 ) < 0 );
    check( t.sends().size() == 5 );
    t.post( worker_test_timer(), -ETIME, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( t.of( IORING_OP_POLL_ADD ).size() == 0 );

    // the bound reached on the first wait of a one-run bank: abandoned, and
    // the exit waits for that wait's timer
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
    check( u.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( u.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED ) == 1 );
    check( rocketnet_uring_queue_send_held( u.send, 0 ) < 0 );
    check( u.worker.timer_armed );
    u.post( worker_test_timer(), -ETIME, 0 );
    check( u.settle() == ROCKETNET_URING_WORKER_EXIT );
    check( u.of( IORING_OP_POLL_ADD ).size() == 0 );
    check( u.of( IORING_OP_TIMEOUT ).size() == 1 );
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
    std::vector<struct io_uring_sqe> all = t.sends();
    std::vector<struct io_uring_sqe> plain( all.begin() + 1, all.end() );
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
    // destinations that take distinct entries of the refused table: one that
    // displaces another is not a new refusing destination
    const int destinations = ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
    int dest[ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD];
    bool taken[ROCKETNET_SEGMENT_REFUSED_ENTRIES] = {};
    int found = 0;
    for ( int d = 1; d < 250 && found < destinations; d++ )
    {
        const struct rocketnet_address_t to = worker_test_client( d );
        const int e = rocketnet_segment_refused_entry( &to );
        if ( !taken[e] )
        {
            taken[e] = true;
            dest[found++] = d;
        }
    }
    check( found == destinations );

    worker_test_t t( 4 * destinations, 4 * destinations, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    for ( int i = 0; i < destinations; i++ )
    {
        t.stage( dest[i], 100 );
        t.stage( dest[i], 100 );
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
    for ( int i = 0; i < destinations; i++ )
    {
        a.stage( dest[i], 100 );
        a.stage( dest[i], 100 );
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

// the threshold counts destinations, not refused runs: one destination with
// more runs in the kernel than the threshold, every one refused before the
// first answer is reaped, is one refusing destination, and the queue keeps
// segmenting for everyone else (standard/TRANSPORT.md t2.4 rule 7)

static void test_uring_queue_worker_one_destination_refusing_many_runs_keeps_segmentation()
{
    const int runs_wanted = ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD + 1;
    const int packets = 4 * runs_wanted;        // four segments a run (the fixture's send)
    worker_test_t t( 2 * packets, 2 * packets, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    for ( int i = 0; i < packets; i++ )
    {
        t.stage( 1, 100 );
    }
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> runs = t.sends();
    check( (int) runs.size() == runs_wanted );
    for ( size_t i = 0; i < runs.size(); i++ )
    {
        check( worker_test_segmented( runs[i] ) );
        t.post( runs[i].user_data, -EINVAL, 0 );
    }
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.counter( ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS ) == 0 );
    check( t.worker.config.segment );
    check( t.worker.refused.destinations == 1 );

    // the refuser's packets went out again one a run; drain them
    std::vector<struct io_uring_sqe> again = t.sends( 0 );
    check( (int) again.size() == runs_wanted + packets );
    for ( size_t i = runs.size(); i < again.size(); i++ )
    {
        check( !worker_test_segmented( again[i] ) );
        t.post( again[i].user_data, 100, 0 );
    }
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    const size_t before = t.kernel.submitted.size();

    // another destination is still sent segmented
    t.stage( 2, 100 );
    t.stage( 2, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> next = t.sends( before );
    check( next.size() == 1 );
    check( worker_test_segmented( next[0] ) );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_one_destination_refusing_many_runs_keeps_segmentation );

// standard/TRANSPORT.md t2.4 rule 7: segmentation turns off only when the
// threshold of destinations refused and none was ever accepted, for the
// transport. A segmented send accepted on one queue keeps another queue
// segmenting however many of its destinations refuse (as the gso transport's
// segment_accepted does); each queue's table of refusers stays its own

static void test_uring_queue_worker_accepted_on_one_queue_keeps_every_queue_segmenting()
{
    const int destinations = ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
    int dest[ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD];
    bool taken[ROCKETNET_SEGMENT_REFUSED_ENTRIES] = {};
    int found = 0;
    int accepting = -1;
    for ( int d = 1; d < 1000 && ( found < destinations || accepting < 0 ); d++ )
    {
        const struct rocketnet_address_t to = worker_test_client( d );
        if ( rocketnet_send_queue_of( &to, 2 ) == 0 )
        {
            if ( accepting < 0 )
            {
                accepting = d;
            }
            continue;
        }
        const int e = rocketnet_segment_refused_entry( &to );
        if ( found < destinations && !taken[e] )
        {
            taken[e] = true;
            dest[found++] = d;
        }
    }
    check( found == destinations );
    check( accepting > 0 );

    worker_test_t a( 4 * destinations, 4 * destinations, 2, true, false, 2 );
    check( a.ok );
    worker_test_t b( a, 1, 4 * destinations, 2 );
    check( b.ok );
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( b.settle() == ROCKETNET_URING_WORKER_SLEEP );

    // queue 0: one segmented send accepted
    a.stage( accepting, 100 );
    a.stage( accepting, 100 );
    a.publish();
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( a.sends().size() == 1 );
    check( worker_test_segmented( a.sends()[0] ) );
    a.post( a.sends()[0].user_data, 200, 0 );
    check( a.settle() == ROCKETNET_URING_WORKER_SLEEP );

    // queue 1: the threshold of its destinations refuse, and it keeps
    // segmenting; the refused runs go out again plain
    for ( int i = 0; i < destinations; i++ )
    {
        b.stage( dest[i], 100 );
        b.stage( dest[i], 100 );
    }
    b.publish();
    check( b.settle() == ROCKETNET_URING_WORKER_SLEEP );
    std::vector<struct io_uring_sqe> runs = b.sends();
    check( (int) runs.size() == destinations );
    for ( size_t i = 0; i < runs.size(); i++ )
    {
        check( worker_test_segmented( runs[i] ) );
        b.post( runs[i].user_data, -EINVAL, 0 );
    }
    check( b.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( b.counter( ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS ) == 0 );
    check( b.worker.config.segment );
    check( a.worker.config.segment );
    check( (int) b.sends().size() == 3 * destinations );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_accepted_on_one_queue_keeps_every_queue_segmenting );

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

// a one-packet run refused is failed, not resent, and its destination is not
// taken as one that refuses segmentation

static void test_uring_queue_worker_refused_single_is_failed()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.stage( 1, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 1 );

    t.post( t.sends()[0].user_data, -EIO, 0 );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 1 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED ) == 1 );
    check( t.send_counter( ROCKETNET_URING_QUEUE_SEND_COUNTER_REFUSED ) == 0 );
    check( t.worker.refused.destinations == 0 );
    check( rocketnet_uring_queue_send_idle( t.send, 0 ) );

    // the destination's next segmented run still goes segmented
    t.stage( 1, 100 );
    t.stage( 1, 100 );
    t.publish();
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );
    check( t.sends().size() == 2 );
    check( worker_test_segmented( t.sends()[1] ) );
}

REGISTER_UNIT_TEST( test_uring_queue_worker_refused_single_is_failed );

// completions that name nothing of the worker's are counted and change
// nothing

static void test_uring_queue_worker_counts_stray_completions()
{
    worker_test_t t( 4, 8, 2 );
    check( t.ok );
    check( t.settle() == ROCKETNET_URING_WORKER_SLEEP );

    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 1, 0 ), 100, 0 );       // another queue's
    t.post( rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, 0, 0 ), 100, 0 );       // no run in the kernel
    t.post( worker_test_timer(), -ETIME, 0 );                                      // no timer armed
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

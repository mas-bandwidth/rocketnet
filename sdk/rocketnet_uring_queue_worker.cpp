/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_uring_queue_worker.h"
#include "rocketnet_internal.h"
#include "rocketnet_uring_queue_send.h"

#include <string.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING
#include <liburing.h>
#if defined( IORING_RECV_MULTISHOT )
#define ROCKETNET_URING_QUEUE_WORKER_BUILT 1
#endif // #if defined( IORING_RECV_MULTISHOT )
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING

#if defined( ROCKETNET_URING_QUEUE_WORKER_BUILT ) && ROCKETNET_URING_QUEUE_WORKER_BUILT

#include "rocketnet_uring_codec.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

bool rocketnet_uring_queue_worker_init( struct rocketnet_uring_queue_worker_t * worker, const struct rocketnet_uring_queue_worker_config_t * config )
{
    rocketnet_assert( worker );
    rocketnet_assert( config );

    if ( config->queue_index < 0 || config->queue_index > 0xFFFF || config->max_in_kernel < 1 || config->max_retry < 1 || !config->send ||
         !config->kernel.space || !config->kernel.sqe || !config->kernel.submit || !config->kernel.peek || !config->kernel.advance )
    {
        return false;
    }

    if ( config->receive.context &&
         ( !config->receive.needs_arm || !config->receive.arm || !config->receive.cqe || !config->receive.stop || !config->receive.armed ) )
    {
        return false;
    }

    worker->config = *config;
    worker->quit.store( false, std::memory_order_relaxed );
    worker->seen = false;
    worker->blocked = false;
    worker->poll_armed = false;
    worker->wake_armed = false;
    worker->cancelled_recv = false;
    worker->cancelled_wake = false;
    worker->cancelled_poll = false;
    worker->in_kernel = 0;
    worker->cancels = 0;
    worker->agains = 0;
    worker->wake_value = 0;
    memset( &worker->refused, 0, sizeof( worker->refused ) );
    for ( int i = 0; i < ROCKETNET_URING_WORKER_NUM_COUNTERS; i++ )
    {
        worker->counters[i].store( 0, std::memory_order_relaxed );
    }

    if ( !config->segment )
    {
        rocketnet_uring_queue_send_plain( config->send, config->queue_index );
    }

    return true;
}

// one writer per counter, the worker: no read-modify-write

static void rocketnet_uring_queue_worker_count( struct rocketnet_uring_queue_worker_t * worker, int counter, uint64_t value = 1 )
{
    std::atomic<uint64_t> & c = worker->counters[counter];
    c.store( c.load( std::memory_order_relaxed ) + value, std::memory_order_relaxed );
}

uint64_t rocketnet_uring_queue_worker_counter( const struct rocketnet_uring_queue_worker_t * worker, int counter )
{
    rocketnet_assert( worker );
    rocketnet_assert( counter >= 0 && counter < ROCKETNET_URING_WORKER_NUM_COUNTERS );
    return worker->counters[counter].load( std::memory_order_relaxed );
}

static bool rocketnet_uring_queue_worker_has_receive( const struct rocketnet_uring_queue_worker_t * worker )
{
    return worker->config.receive.context != NULL;
}

static bool rocketnet_uring_queue_worker_receive_armed( const struct rocketnet_uring_queue_worker_t * worker )
{
    return rocketnet_uring_queue_worker_has_receive( worker ) && worker->config.receive.armed( worker->config.receive.context );
}

// the next submission entry, or NULL when the submission queue is full: the
// caller retries on the next pass, after the submit that follows it

static struct io_uring_sqe * rocketnet_uring_queue_worker_sqe( struct rocketnet_uring_queue_worker_t * worker )
{
    const struct rocketnet_uring_queue_worker_kernel_t * kernel = &worker->config.kernel;
    if ( kernel->space( kernel->context ) <= 0 )
    {
        return NULL;
    }
    struct io_uring_sqe * sqe = kernel->sqe( kernel->context );
    rocketnet_assert( sqe );
    return sqe;
}

static bool rocketnet_uring_queue_worker_cancel( struct rocketnet_uring_queue_worker_t * worker, uint64_t target, int what )
{
    struct io_uring_sqe * sqe = rocketnet_uring_queue_worker_sqe( worker );
    if ( !sqe )
    {
        return false;
    }
    io_uring_prep_cancel64( sqe, target, 0 );
    io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, worker->config.queue_index, (uint32_t) what ) );
    worker->cancels++;
    return true;
}

// RNUringQueueWorker ReapOk, ReapAgain: a run's completion. -EAGAIN hands it
// back and blocks submits until a POLLOUT poll fires; a segmented run its
// destination refused comes back as one plain run per packet, and the
// destination is remembered so its later runs leave plain.

static void rocketnet_uring_queue_worker_send_cqe( struct rocketnet_uring_queue_worker_t * worker, uint32_t tag, int32_t res )
{
    struct rocketnet_uring_queue_send_t * send = worker->config.send;
    const int q = worker->config.queue_index;

    // complete makes a refused one-packet run failed: a run is classified as
    // segmented here and complete knows its count

    const int result = rocketnet_uring_queue_send_result( res, true );

    const int packets = rocketnet_uring_queue_send_complete( send, q, tag, result );
    if ( packets < 0 )
    {
        rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
        return;
    }

    rocketnet_assert( worker->in_kernel > 0 );
    worker->in_kernel--;

    if ( result == ROCKETNET_URING_QUEUE_SEND_OK && packets > 1 )
    {
        worker->refused.accepted = true;
    }

    if ( result == ROCKETNET_URING_QUEUE_SEND_REFUSED && packets > 1 )
    {
        const int slots = send->slots;
        const int b = (int) ( tag / (uint32_t) slots );
        const int slot = (int) ( tag % (uint32_t) slots );
        const struct rocketnet_address_t * to = &send->queues->queue[q].bank[b].slot[slot].to;
        if ( rocketnet_segment_refused_add( &worker->refused, to ) && worker->config.segment )
        {
            worker->config.segment = false;
            rocketnet_uring_queue_send_plain( send, q );
            rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS );
            rocketnet_warn( "io_uring transport: queue %d: %d destinations refused a segmented send and none was accepted: one SENDMSG per packet from now on",
                            q, worker->refused.destinations );
        }
    }

    if ( result == ROCKETNET_URING_QUEUE_SEND_AGAIN )
    {
        rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL );
        worker->blocked = true;
        if ( worker->seen && worker->agains < worker->config.max_retry )
        {
            worker->agains++;
        }
    }
}

static void rocketnet_uring_queue_worker_reap_one( struct rocketnet_uring_queue_worker_t * worker, const struct rocketnet_uring_queue_worker_completion_t * cqe )
{
    const int q = worker->config.queue_index;
    if ( rocketnet_uring_tag_queue( cqe->user_data ) != q )
    {
        rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
        return;
    }

    switch ( rocketnet_uring_tag_kind( cqe->user_data ) )
    {
        case ROCKETNET_URING_TAG_SEND:
            rocketnet_uring_queue_worker_send_cqe( worker, (uint32_t) rocketnet_uring_tag_index( cqe->user_data ), cqe->res );
            break;

        case ROCKETNET_URING_TAG_RECV:
            // RNUringQueueWorker ReapRecvEnd, when this is the multishot's last
            if ( rocketnet_uring_queue_worker_has_receive( worker ) )
            {
                worker->config.receive.cqe( worker->config.receive.context, cqe->res, cqe->flags );
            }
            else
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            }
            break;

        case ROCKETNET_URING_TAG_WAKE:
            // RNUringQueueWorker ReapWake
            if ( worker->wake_armed )
            {
                worker->wake_armed = false;
            }
            else
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            }
            break;

        case ROCKETNET_URING_TAG_POLL:
            // RNUringQueueWorker ReapPoll
            if ( worker->poll_armed )
            {
                worker->poll_armed = false;
                worker->blocked = false;
            }
            else
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            }
            break;

        case ROCKETNET_URING_TAG_CANCEL:
            if ( worker->cancels > 0 )
            {
                worker->cancels--;
            }
            else
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            }
            break;

        default:
            rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            break;
    }
}

// abandon: the stop seen and the socket full past the bound

static bool rocketnet_uring_queue_worker_abandoning( const struct rocketnet_uring_queue_worker_t * worker )
{
    return worker->seen && worker->agains >= worker->config.max_retry;
}

// what the worker has in the kernel or waiting, the stop seen: RNUringQueueWorker
// Drained. The caller has found no bank to claim and no completion ready.

static bool rocketnet_uring_queue_worker_drained( const struct rocketnet_uring_queue_worker_t * worker )
{
#if defined( ROCKETNET_URING_QUEUE_WORKER_PLANT )
    (void) worker;
    return true;                                    // the plant: ExitRule "early", the witness "exit before drain"
#else  // #if defined( ROCKETNET_URING_QUEUE_WORKER_PLANT )
    return worker->in_kernel == 0 && !worker->wake_armed && !worker->poll_armed &&
           !rocketnet_uring_queue_worker_receive_armed( worker ) && rocketnet_uring_queue_send_held( worker->config.send, worker->config.queue_index ) < 0;
#endif // #if defined( ROCKETNET_URING_QUEUE_WORKER_PLANT )
}

// RNUringQueueWorker Submit, Split, Drop: the next run of the held bank. A
// run to a destination known to refuse segmentation is cut plain before any
// send; one the socket's family cannot name is dropped without one.

static int rocketnet_uring_queue_worker_send_one( struct rocketnet_uring_queue_worker_t * worker, const struct rocketnet_uring_queue_send_run_t * run )
{
    struct rocketnet_uring_queue_send_t * send = worker->config.send;
    const int q = worker->config.queue_index;

    if ( run->count > 1 && rocketnet_segment_refused_contains( &worker->refused, run->to ) )
    {
        const bool split = rocketnet_uring_queue_send_split( send, q, run );
        rocketnet_assert( split );
        (void) split;
        return ROCKETNET_URING_WORKER_STEP_SPLIT;
    }

    struct msghdr * message = rocketnet_uring_queue_send_message( send, q, run, worker->config.ipv6 );
    if ( !message )
    {
        rocketnet_uring_queue_send_complete( send, q, run->tag, ROCKETNET_URING_QUEUE_SEND_FAILED );
        rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_SEND_DROPPED, (uint64_t) run->count );
        return ROCKETNET_URING_WORKER_STEP_DROP;
    }

    struct io_uring_sqe * sqe = rocketnet_uring_queue_worker_sqe( worker );
    rocketnet_assert( sqe );
    io_uring_prep_sendmsg( sqe, worker->config.fd, message, 0 );
    io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, q, run->tag ) );
    worker->in_kernel++;
    if ( run->count > 1 && !run->plain )
    {
        rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS );
    }
    return ROCKETNET_URING_WORKER_STEP_SUBMIT;
}

int rocketnet_uring_queue_worker_step( struct rocketnet_uring_queue_worker_t * worker, bool reap )
{
    rocketnet_assert( worker );

    struct rocketnet_uring_queue_send_t * send = worker->config.send;
    const int q = worker->config.queue_index;
    const struct rocketnet_uring_queue_worker_kernel_t * kernel = &worker->config.kernel;

    // RNUringQueueWorker Observe

    if ( !worker->seen && worker->quit.load( std::memory_order_acquire ) )
    {
        worker->seen = true;
        if ( rocketnet_uring_queue_worker_has_receive( worker ) )
        {
            worker->config.receive.stop( worker->config.receive.context );
        }
        return ROCKETNET_URING_WORKER_STEP_OBSERVE;
    }

    // RNUringQueueWorker ReapOk, ReapAgain, ReapRefused, ReapPoll,
    // ReapRecvEnd, ReapWake: the oldest ready completion

    struct rocketnet_uring_queue_worker_completion_t completion;
    const bool ready = kernel->peek( kernel->context, &completion, 1 ) == 1;
    if ( ready && reap )
    {
        rocketnet_uring_queue_worker_reap_one( worker, &completion );
        kernel->advance( kernel->context, 1 );
        return ROCKETNET_URING_WORKER_STEP_REAP;
    }

    // RNUringQueueWorker Retire, Claim, Abandon

    const int held = rocketnet_uring_queue_send_held( send, q );
    if ( held >= 0 && rocketnet_uring_queue_send_retire( send, q ) )
    {
        return ROCKETNET_URING_WORKER_STEP_RETIRE;
    }

    if ( held < 0 && rocketnet_uring_queue_send_claim( send, q ) >= 0 )
    {
        return ROCKETNET_URING_WORKER_STEP_CLAIM;
    }

    const bool abandoning = rocketnet_uring_queue_worker_abandoning( worker );
    if ( held >= 0 && abandoning && rocketnet_uring_queue_send_abandon( send, q ) > 0 )
    {
        return ROCKETNET_URING_WORKER_STEP_ABANDON;
    }

    // RNUringQueueWorker Submit, Split, Drop

    if ( held >= 0 && !worker->blocked && worker->in_kernel < worker->config.max_in_kernel && kernel->space( kernel->context ) > 0 )
    {
        struct rocketnet_uring_queue_send_run_t run;
        if ( rocketnet_uring_queue_send_next( send, q, &run ) )
        {
            return rocketnet_uring_queue_worker_send_one( worker, &run );
        }
    }

    // RNUringQueueWorker ArmPoll: the poll a -EAGAIN asked for, unless the
    // stop has given up on the socket

    if ( worker->blocked && !worker->poll_armed && !abandoning )
    {
        struct io_uring_sqe * sqe = rocketnet_uring_queue_worker_sqe( worker );
        if ( sqe )
        {
            io_uring_prep_poll_add( sqe, worker->config.fd, POLLOUT );
            io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, q, 0 ) );
            worker->poll_armed = true;
            return ROCKETNET_URING_WORKER_STEP_ARM_POLL;
        }
    }

    if ( !worker->seen )
    {
        // the receive's re-arm (RNUringQueueReceive Arm): the model's
        // receive stays armed until the stop

        if ( rocketnet_uring_queue_worker_has_receive( worker ) && worker->config.receive.needs_arm( worker->config.receive.context ) &&
             worker->config.receive.arm( worker->config.receive.context, rocketnet_uring_queue_worker_sqe( worker ) ) )
        {
            return ROCKETNET_URING_WORKER_STEP_ARM_RECV;
        }

        // RNUringQueueWorker ArmWake

        if ( !worker->wake_armed )
        {
            struct io_uring_sqe * sqe = rocketnet_uring_queue_worker_sqe( worker );
            if ( sqe )
            {
                io_uring_prep_read( sqe, worker->config.wake_fd, &worker->wake_value, sizeof( worker->wake_value ), 0 );
                io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, q, 0 ) );
                worker->wake_armed = true;
                return ROCKETNET_URING_WORKER_STEP_ARM_WAKE;
            }
        }

        return ROCKETNET_URING_WORKER_STEP_NONE;
    }

    // RNUringQueueWorker CancelRecv, CancelWake, CancelPoll

    if ( rocketnet_uring_queue_worker_receive_armed( worker ) && !worker->cancelled_recv &&
         rocketnet_uring_queue_worker_cancel( worker, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, q, 0 ), ROCKETNET_URING_CANCEL_RECV ) )
    {
        worker->cancelled_recv = true;
        return ROCKETNET_URING_WORKER_STEP_CANCEL_RECV;
    }

    if ( worker->wake_armed && !worker->cancelled_wake &&
         rocketnet_uring_queue_worker_cancel( worker, rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, q, 0 ), ROCKETNET_URING_CANCEL_WAKE ) )
    {
        worker->cancelled_wake = true;
        return ROCKETNET_URING_WORKER_STEP_CANCEL_WAKE;
    }

    if ( abandoning && worker->poll_armed && !worker->cancelled_poll &&
         rocketnet_uring_queue_worker_cancel( worker, rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, q, 0 ), ROCKETNET_URING_CANCEL_POLL ) )
    {
        worker->cancelled_poll = true;
        return ROCKETNET_URING_WORKER_STEP_CANCEL_POLL;
    }

    // RNUringQueueWorker Exit (ExitRule "drained"): no completion ready, no
    // bank to claim, and nothing of the worker's in the kernel

    if ( !ready && rocketnet_uring_queue_worker_drained( worker ) )
    {
        return ROCKETNET_URING_WORKER_STEP_EXIT;
    }

    // RNUringQueueWorker Sleep

    return ROCKETNET_URING_WORKER_STEP_NONE;
}

int rocketnet_uring_queue_worker_pass( struct rocketnet_uring_queue_worker_t * worker )
{
    rocketnet_assert( worker );

    int reaped = 0;
    for ( ;; )
    {
        const int step = rocketnet_uring_queue_worker_step( worker, reaped < ROCKETNET_URING_WORKER_CQE_BATCH );
        if ( step == ROCKETNET_URING_WORKER_STEP_EXIT )
        {
            return ROCKETNET_URING_WORKER_EXIT;
        }
        if ( step == ROCKETNET_URING_WORKER_STEP_REAP )
        {
            reaped++;
            continue;
        }
        if ( step != ROCKETNET_URING_WORKER_STEP_NONE )
        {
            continue;
        }

        // nothing more to do without the kernel: a full batch reaped leaves
        // the rest for a pass right after the submit, and a wake read the
        // full submission queue had no room for is armed by a pass right
        // after it (RNUringQueueWorker Sleep: never with the wake read
        // down before the stop); otherwise the submit waits for the next
        // completion
        if ( !worker->seen && !worker->wake_armed )
        {
            return ROCKETNET_URING_WORKER_CONTINUE;
        }
        if ( reaped == ROCKETNET_URING_WORKER_CQE_BATCH )
        {
            struct rocketnet_uring_queue_worker_completion_t completion;
            const struct rocketnet_uring_queue_worker_kernel_t * kernel = &worker->config.kernel;
            if ( kernel->peek( kernel->context, &completion, 1 ) == 1 )
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED );
                return ROCKETNET_URING_WORKER_CONTINUE;
            }
        }
        return ROCKETNET_URING_WORKER_SLEEP;
    }
}


void rocketnet_uring_queue_worker_run( struct rocketnet_uring_queue_worker_t * worker )
{
    rocketnet_assert( worker );

    const struct rocketnet_uring_queue_worker_kernel_t * kernel = &worker->config.kernel;
    bool logged = false;

    for ( ;; )
    {
        const int next = rocketnet_uring_queue_worker_pass( worker );
        if ( next == ROCKETNET_URING_WORKER_EXIT )
        {
            break;
        }

        const int result = kernel->submit( kernel->context, next == ROCKETNET_URING_WORKER_SLEEP );
        rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_SUBMITS );
        if ( result < 0 && result != -EINTR && result != -EAGAIN && result != -EBUSY && !logged )
        {
            rocketnet_error( "io_uring transport: queue %d: submit refused: %s", worker->config.queue_index, strerror( -result ) );
            logged = true;
        }
    }
}

void rocketnet_uring_queue_worker_wake( struct rocketnet_uring_queue_worker_t * worker )
{
    rocketnet_assert( worker );
    if ( worker->config.wake_fd < 0 )
    {
        return;
    }
    const uint64_t one = 1;
    // a full count (2^64 - 2 unread writes) refuses with EAGAIN: the
    // worker is then already woken
    const ssize_t written = write( worker->config.wake_fd, &one, sizeof( one ) );
    (void) written;
}

void rocketnet_uring_queue_worker_stop( struct rocketnet_uring_queue_worker_t * worker )
{
    rocketnet_assert( worker );
    worker->quit.store( true, std::memory_order_release );
    rocketnet_uring_queue_worker_wake( worker );
}

#else // #if defined( ROCKETNET_URING_QUEUE_WORKER_BUILT ) && ROCKETNET_URING_QUEUE_WORKER_BUILT

bool rocketnet_uring_queue_worker_init( struct rocketnet_uring_queue_worker_t * worker, const struct rocketnet_uring_queue_worker_config_t * config )
{
    (void) worker;
    (void) config;
    return false;
}

int rocketnet_uring_queue_worker_pass( struct rocketnet_uring_queue_worker_t * worker )
{
    (void) worker;
    return ROCKETNET_URING_WORKER_EXIT;
}

void rocketnet_uring_queue_worker_run( struct rocketnet_uring_queue_worker_t * worker )
{
    (void) worker;
}

void rocketnet_uring_queue_worker_wake( struct rocketnet_uring_queue_worker_t * worker )
{
    (void) worker;
}

void rocketnet_uring_queue_worker_stop( struct rocketnet_uring_queue_worker_t * worker )
{
    (void) worker;
}

uint64_t rocketnet_uring_queue_worker_counter( const struct rocketnet_uring_queue_worker_t * worker, int counter )
{
    (void) worker;
    (void) counter;
    return 0;
}

#endif // #if defined( ROCKETNET_URING_QUEUE_WORKER_BUILT ) && ROCKETNET_URING_QUEUE_WORKER_BUILT

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// One queue's io_uring worker (layer L3 of the per-queue send). The model is
// proofs/tla/RNUringQueueWorker.tla (README-RNUringQueueWorker.md): each
// branch of rocketnet_uring_queue_worker_step is one of its worker actions,
// named in the comment beside it, and tests/tla_trace_queue_worker.cpp
// checks the code's steps against it.

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
#include <unistd.h>

// the retry timer's timeout (ArmTimer), the same for every worker: the kernel
// copies it when the timeout is submitted, and nothing writes it
static struct __kernel_timespec rocketnet_uring_queue_worker_retry_timeout = { 0, (long long) ( ROCKETNET_URING_SEND_RETRY_SECONDS * 1000000000.0 ) };

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
    worker->shutdown.store( false, std::memory_order_relaxed );
    worker->quit.store( false, std::memory_order_relaxed );
    worker->seen = false;
    worker->blocked = false;
    worker->starved = false;
    worker->timer_armed = false;
    worker->recv_timer_armed = false;
    worker->recv_waiting = false;
    worker->recv_refused = 0;
    worker->wake_armed = false;
    worker->cancelled_recv = false;
    worker->cancelled_wake = false;
    worker->in_kernel = 0;
    worker->cancels = 0;
    worker->agains = 0;
    worker->wake_value = 0;
    memset( &worker->refused, 0, sizeof( worker->refused ) );
    worker->last_send_class = 0;
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
// pass is starved, so it says continue and the submit that follows frees the
// room without waiting; the caller retries on the next pass

static struct io_uring_sqe * rocketnet_uring_queue_worker_sqe( struct rocketnet_uring_queue_worker_t * worker )
{
    const struct rocketnet_uring_queue_worker_kernel_t * kernel = &worker->config.kernel;
    if ( kernel->space( kernel->context ) <= 0 )
    {
        worker->starved = true;
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

// RNUringQueueWorker.tla ReapOk, ReapAgain: a run's completion. A full
// answer (-EAGAIN, -EINTR, -ENOBUFS, -ENOMEM) hands it back and blocks
// submits until the retry timer fires (ArmTimer, ReapTimer); after shutdown
// the answer that starts a wait counts toward the bound, one a wait, not
// each answer of the burst refused with it. A run done (sent or failed)
// starts the count again: only a socket that stays full loses what waits. A
// segmented run its destination refused comes back as one plain run per
// packet, and the destination is remembered so its later runs leave plain
// (proofs/tla/RNSegmentRefusal.tla Refuse).

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
    worker->last_send_class = rocketnet_uring_queue_send_class( res, packets );

    if ( result == ROCKETNET_URING_QUEUE_SEND_OK && packets > 1 && !worker->refused.accepted )
    {
        worker->refused.accepted = true;
        send->segment_accepted.store( true, std::memory_order_relaxed );
    }

    // ReapOk: complete has made a refused one-packet run failed, so a run
    // done is any class but AGAIN and SPLIT
    if ( worker->last_send_class == ROCKETNET_URING_QUEUE_SEND_CLASS_OK )
    {
        worker->agains = 0;
    }

    if ( result == ROCKETNET_URING_QUEUE_SEND_REFUSED && packets > 1 )
    {
        const int slots = send->slots;
        const int b = (int) ( tag / (uint32_t) slots );
        const int slot = (int) ( tag % (uint32_t) slots );
        const struct rocketnet_address_t * to = &send->queues->queue[q].bank[b].slot[slot].to;
        // another queue's accepted segmented send counts as this queue's:
        // the transport can segment
        if ( send->segment_accepted.load( std::memory_order_relaxed ) )
        {
            worker->refused.accepted = true;
        }
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
        if ( !worker->blocked && worker->agains < worker->config.max_retry && worker->shutdown.load( std::memory_order_acquire ) )
        {
            worker->agains++;
        }
        worker->blocked = true;
    }
}

// RNUringQueueWorker.tla ReapRecvData, ReapRecvEnd: a datagram starts the
// count of refused ends again; a multishot that ends refused (a result
// other than -ECANCELED with no IORING_CQE_F_MORE and no datagram: no buffer
// free, a kernel that refuses the receive, or an end with nothing, 0, since
// a multishot RECVMSG result holds at least its header) counts one, and
// from the second in a row its next arm waits for the receive's retry timer
// (ArmRecvTimer). The first is armed again at once: a burst that took every
// buffer gives them back as its completions are reaped. The receive module
// counts the refusal itself.

static void rocketnet_uring_queue_worker_recv_cqe( struct rocketnet_uring_queue_worker_t * worker, int32_t res, uint32_t flags )
{
    if ( res > 0 )
    {
        worker->recv_refused = 0;
    }
    else if ( res != -ECANCELED && ( flags & IORING_CQE_F_MORE ) == 0 )
    {
        if ( worker->recv_refused < 2 )
        {
            worker->recv_refused++;
        }
        if ( worker->recv_refused >= 2 )
        {
            worker->recv_waiting = true;
        }
    }
    worker->config.receive.cqe( worker->config.receive.context, res, flags );
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
            // RNUringQueueWorker.tla ReapRecvEnd, when this is the multishot's
            // last, else ReapRecvData
            if ( rocketnet_uring_queue_worker_has_receive( worker ) )
            {
                rocketnet_uring_queue_worker_recv_cqe( worker, cqe->res, cqe->flags );
            }
            else
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            }
            break;

        case ROCKETNET_URING_TAG_WAKE:
            // RNUringQueueWorker.tla ReapWake. The eventfd is blocking: a read
            // never answers -EAGAIN (on a non-blocking one the worker would
            // spin, arming and reaping it)
            rocketnet_assert( cqe->res != -EAGAIN );
            if ( worker->wake_armed )
            {
                // main's wake ends a wait until shutdown begins (it published);
                // from then only the timer does, so each wait counted toward
                // the bound is a whole retry period, however often main wakes
                // the worker with its last disconnect batches
                worker->wake_armed = false;
                if ( !worker->shutdown.load( std::memory_order_acquire ) )
                {
                    worker->blocked = false;
                }
            }
            else
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            }
            break;

        case ROCKETNET_URING_TAG_TIMER:
            // RNUringQueueWorker.tla ReapTimer, ReapRecvTimer: the wait is
            // over (-ETIME), the send's or the receive's by the tag's index
            if ( rocketnet_uring_tag_index( cqe->user_data ) == ROCKETNET_URING_TIMER_SEND && worker->timer_armed )
            {
                worker->timer_armed = false;
                worker->blocked = false;
            }
            else if ( rocketnet_uring_tag_index( cqe->user_data ) == ROCKETNET_URING_TIMER_RECV && worker->recv_timer_armed )
            {
                worker->recv_timer_armed = false;
                worker->recv_waiting = false;
            }
            else
            {
                rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_STRAY );
            }
            break;

        case ROCKETNET_URING_TAG_CANCEL:
            // RNUringQueueWorker.tla ReapCancel: the completion of a cancel the
            // worker submitted (CancelRecv, CancelWake)
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

// abandon (RNUringQueueWorker.tla Abandoning): shutdown begun and max_retry
// waits begun with no run done; the last is abandoned at its start, not
// after its timer (the bound counts waits begun, not waits completed)

static bool rocketnet_uring_queue_worker_abandoning( const struct rocketnet_uring_queue_worker_t * worker )
{
    return worker->agains >= worker->config.max_retry && worker->shutdown.load( std::memory_order_acquire );
}

// what the worker has in the kernel or waiting, the stop seen: RNUringQueueWorker.tla
// Drained. The caller has found no bank to claim and no completion ready.

static bool rocketnet_uring_queue_worker_drained( const struct rocketnet_uring_queue_worker_t * worker )
{
    return worker->in_kernel == 0 && !worker->wake_armed && !worker->timer_armed && !worker->recv_timer_armed &&
           !rocketnet_uring_queue_worker_receive_armed( worker ) && rocketnet_uring_queue_send_held( worker->config.send, worker->config.queue_index ) < 0;
}

// RNUringQueueWorker.tla Submit, Split, Drop: the next run of the held bank. A
// run to a destination known to refuse segmentation is cut plain before any
// send; one the socket's family cannot name is dropped without one
// (proofs/tla/RNSegmentRefusal.tla Run).

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
    // MSG_DONTWAIT: a full socket answers -EAGAIN; without it io_uring waits
    // in the kernel for room and the run never comes back
    io_uring_prep_sendmsg( sqe, worker->config.fd, message, MSG_DONTWAIT );
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

    // RNUringQueueWorker.tla Observe

    if ( !worker->seen && worker->quit.load( std::memory_order_acquire ) )
    {
        // from here the receive and the wake read are cancelled; a wait
        // goes on until its timer fires
        worker->seen = true;
        if ( rocketnet_uring_queue_worker_has_receive( worker ) )
        {
            worker->config.receive.stop( worker->config.receive.context );
        }
        return ROCKETNET_URING_WORKER_STEP_OBSERVE;
    }

#if defined( ROCKETNET_URING_QUEUE_WORKER_PLANT )
    // the trace check's plant: ExitRule "early", exit once the stop is seen,
    // whatever is still in the kernel (the witness "exit before drain");
    // only tests/tla_trace_queue_worker.cpp in the plant build defines it
    if ( worker->seen )
    {
        return ROCKETNET_URING_WORKER_STEP_EXIT;
    }
#endif // #if defined( ROCKETNET_URING_QUEUE_WORKER_PLANT )

    // RNUringQueueWorker.tla ReapOk, ReapAgain, ReapRefused, ReapTimer,
    // ReapRecvEnd, ReapWake: the oldest ready completion

    struct rocketnet_uring_queue_worker_completion_t completion;
    const bool ready = kernel->peek( kernel->context, &completion, 1 ) == 1;
    if ( ready && reap )
    {
        rocketnet_uring_queue_worker_reap_one( worker, &completion );
        kernel->advance( kernel->context, 1 );
        return ROCKETNET_URING_WORKER_STEP_REAP;
    }

    // RNUringQueueWorker.tla Retire, Claim, Abandon

    const int held = rocketnet_uring_queue_send_held( send, q );
    if ( held >= 0 && rocketnet_uring_queue_send_retire( send, q ) )
    {
        return ROCKETNET_URING_WORKER_STEP_RETIRE;
    }

    if ( held < 0 && rocketnet_uring_queue_send_claim( send, q ) >= 0 )
    {
        // a new bank gets the bound's waits afresh (the gso worker's per-bank count)
        worker->agains = 0;
        return ROCKETNET_URING_WORKER_STEP_CLAIM;
    }

    const bool abandoning = rocketnet_uring_queue_worker_abandoning( worker );
    if ( held >= 0 && abandoning && rocketnet_uring_queue_send_abandon( send, q ) > 0 )
    {
        return ROCKETNET_URING_WORKER_STEP_ABANDON;
    }

    // RNUringQueueWorker.tla Submit, Split, Drop

    if ( held >= 0 && !worker->blocked && worker->in_kernel < worker->config.max_in_kernel )
    {
        struct rocketnet_uring_queue_send_run_t run;
        if ( kernel->space( kernel->context ) <= 0 )
        {
            worker->starved = true;
        }
        else if ( rocketnet_uring_queue_send_next( send, q, &run ) )
        {
            return rocketnet_uring_queue_worker_send_one( worker, &run );
        }
    }

    // RNUringQueueWorker.tla ArmTimer: the retry timer a full answer asked
    // for, before the stop and after it. A timeout, never a POLLOUT poll: a
    // socket reports room while the device queue behind it refuses
    // (-ENOBUFS), and the poll would fire again and again

    if ( worker->blocked && !worker->timer_armed )
    {
        struct io_uring_sqe * sqe = rocketnet_uring_queue_worker_sqe( worker );
        if ( sqe )
        {
            io_uring_prep_timeout( sqe, &rocketnet_uring_queue_worker_retry_timeout, 0, 0 );
            io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_TIMER, q, ROCKETNET_URING_TIMER_SEND ) );
            worker->timer_armed = true;
            return ROCKETNET_URING_WORKER_STEP_ARM_TIMER;
        }
    }

    if ( !worker->seen )
    {
        // RNUringQueueWorker.tla ArmRecvTimer: a receive refused again waits
        // a retry period before its arm (the same timeout as the send's)

        if ( rocketnet_uring_queue_worker_has_receive( worker ) && worker->recv_waiting && !worker->recv_timer_armed )
        {
            struct io_uring_sqe * sqe = rocketnet_uring_queue_worker_sqe( worker );
            if ( sqe )
            {
                io_uring_prep_timeout( sqe, &rocketnet_uring_queue_worker_retry_timeout, 0, 0 );
                io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_TIMER, q, ROCKETNET_URING_TIMER_RECV ) );
                worker->recv_timer_armed = true;
                return ROCKETNET_URING_WORKER_STEP_ARM_RECV_TIMER;
            }
        }

        // RNUringQueueWorker.tla ArmRecv: a multishot that ended before the stop
        // is armed again, unless it waits for its timer. With no submission
        // entry the receive module is handed NULL, counts the failed re-arm
        // and still needs one.

        if ( rocketnet_uring_queue_worker_has_receive( worker ) && !worker->recv_waiting && worker->config.receive.needs_arm( worker->config.receive.context ) )
        {
            struct io_uring_sqe * sqe = rocketnet_uring_queue_worker_sqe( worker );
            if ( worker->config.receive.arm( worker->config.receive.context, sqe ) )
            {
                return ROCKETNET_URING_WORKER_STEP_ARM_RECV;
            }
            rocketnet_assert( !sqe );       // the module refuses only a NULL entry: a taken one is never left unprepared
        }

        // RNUringQueueWorker.tla ArmWake

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

    // RNUringQueueWorker.tla CancelRecv, CancelWake (the timer fires on its own)

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

    // RNUringQueueWorker.tla Exit (ExitRule "drained"): no completion ready, no
    // bank to claim, and nothing of the worker's in the kernel

    if ( !ready && rocketnet_uring_queue_worker_drained( worker ) )
    {
        return ROCKETNET_URING_WORKER_STEP_EXIT;
    }

    // RNUringQueueWorker.tla Sleep

    return ROCKETNET_URING_WORKER_STEP_NONE;
}

int rocketnet_uring_queue_worker_pass( struct rocketnet_uring_queue_worker_t * worker )
{
    rocketnet_assert( worker );

    int reaped = 0;
    worker->starved = false;
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

        // nothing more to do without the kernel: a step the full submission
        // queue had no room for (a run, the poll, the receive, the wake read
        // or a cancel) is taken by a pass right after the submit, which
        // frees the room (RNUringQueueWorker Sleep: never with a worker
        // action still to do, so never with the wake read down before the
        // stop), and a full batch reaped leaves the rest for one too;
        // otherwise the submit waits for the next completion
        if ( worker->starved )
        {
            worker->starved = false;
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
            // drained: a cancel still prepared and not submitted names a
            // target that is already done, and the ring is torn down after
            // the join, so it is never sent
            break;
        }

        const int result = kernel->submit( kernel->context, next == ROCKETNET_URING_WORKER_SLEEP );
        rocketnet_uring_queue_worker_count( worker, ROCKETNET_URING_WORKER_COUNTER_SUBMITS );
        if ( result < 0 && result != -EINTR && result != -EAGAIN && result != -EBUSY && !logged )
        {
            rocketnet_error( "io_uring transport: queue %d: submit refused: %s", worker->config.queue_index, rocketnet_platform_socket_error_text( -result ).text );
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

void rocketnet_uring_queue_worker_begin_shutdown( struct rocketnet_uring_queue_worker_t * worker )
{
    rocketnet_assert( worker );
    worker->shutdown.store( true, std::memory_order_release );
    rocketnet_uring_queue_worker_wake( worker );
}

void rocketnet_uring_queue_worker_stop( struct rocketnet_uring_queue_worker_t * worker )
{
    rocketnet_assert( worker );
    worker->shutdown.store( true, std::memory_order_release );
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

int rocketnet_uring_queue_worker_step( struct rocketnet_uring_queue_worker_t * worker, bool reap )
{
    (void) worker;
    (void) reap;
    return ROCKETNET_URING_WORKER_STEP_EXIT;
}

void rocketnet_uring_queue_worker_run( struct rocketnet_uring_queue_worker_t * worker )
{
    (void) worker;
}

void rocketnet_uring_queue_worker_wake( struct rocketnet_uring_queue_worker_t * worker )
{
    (void) worker;
}

void rocketnet_uring_queue_worker_begin_shutdown( struct rocketnet_uring_queue_worker_t * worker )
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

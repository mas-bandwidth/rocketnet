/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer L3 of the io_uring per-queue send: the trace walk of one queue's
// worker (sdk/rocketnet_uring_queue_worker.cpp, over the handoff of
// sdk/rocketnet_uring_queue_send.cpp; the model
// proofs/tla/RNUringQueueWorker.tla), checked by
// proofs/tla/TraceRNUringQueueWorker.tla (proofs/tla/README-TraceCheck.md).
// No socket, no ring, no clock, no thread: the walk is main, the worker and
// the kernel, one call per step. The worker takes one
// rocketnet_uring_queue_worker_step a step. The kernel is a fake: every
// submission entry the worker prepares is in it at once, and each of its
// answers is a step of its own, in any order: a run sent, failed, -EAGAIN
// or refused; the poll firing or cancelled; the eventfd READ taking a wake
// or cancelled; the receive ending on its own or cancelled. A cancel's own
// completion comes with its target's and changes no model variable. The
// receive is a fake that is armed from the start, as the transport arms it,
// and armed again by the worker after it ends on its own. Every record is
// read off the fake kernel (what it holds and what it has posted) and the
// handoff, never off the worker's own flags, so a worker that miscounts
// what it put in the kernel shows as a step the model does not take.
//
// After the recorded steps the walk stops main and drives the worker and the
// kernel, unrecorded, until the worker exits; a worker that can do nothing
// with nothing of the kernel's left to come, or that does not exit within
// the bound, fails the walk (a hang is no trace prefix's failure). pass()
// and its batch limit (step with reap false) are the unit suite's
// (tests/test_unit_uring_queue_worker.cpp).
//
// It writes MCTraceQueueWorkerSeed<seed>, or MCTraceQueueWorkerPlantSeed<seed>
// from tla_trace_plant, whose code is the worker with
// ROCKETNET_URING_QUEUE_WORKER_PLANT (exit at the first step after the stop
// is seen, whatever is still in the kernel: ExitRule "early", the witness
// "exit before drain"). The walk prints PLANT-FIRED n, the exits with anything of the
// worker's still in the kernel or waiting: the planted code's own act, so the
// clean code's count is 0.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_URING_QUEUE_WORKER_PLANT 1
#include "../sdk/rocketnet_uring_queue_worker.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_uring_queue_send.h"
#include "../sdk/rocketnet_uring_queue_worker.h"
#include "../sdk/rocketnet_uring_send_queue.h"

#if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING
#include "../sdk/rocketnet_uring_codec.h"
#endif // #if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

#if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING && defined( IORING_RECV_MULTISHOT )

#include <poll.h>

// the instance: one queue, two banks published, each of three slots (so at
// most three runs), runs of at most two packets, two runs in the kernel at
// once, two -EAGAIN answers before the stop and two after it before the
// worker abandons, one segment refusal, one receive that ends on its own;
// two destinations the socket names and one it cannot

#define QWORK_U 2
#define QWORK_SLOTS 3
#define QWORK_K 2
#define QWORK_MAX_RETRY 2
#define QWORK_MAX_AGAIN 2
#define QWORK_MAX_REFUSE 1
#define QWORK_MAX_RECV_END 1
#define QWORK_DESTS 3
#define QWORK_MAX_STEPS 200
#define QWORK_FINISH_STEPS 100000
#define QWORK_SQ 64

// a run in the fake kernel
struct qwork_send_t
{
    uint64_t user_data;
    int count;
};

struct qwork_walk_t
{
    struct rocketnet_uring_send_queues_t * banks;
    struct rocketnet_uring_queue_send_t * send;
    struct rocketnet_uring_queue_worker_t worker;
    struct rocketnet_address_t dest[QWORK_DESTS];

    // the submission queue the worker prepares into; the walk moves every
    // entry into the kernel after each worker step
    struct io_uring_sqe sq[QWORK_SQ];
    int queued;

    // the completion queue, oldest first, and what of the run each send
    // completion names (its packet count) so a record can classify it
    std::vector<struct rocketnet_uring_queue_worker_completion_t> cq;
    std::vector<int> cq_count;

    // in the kernel, not yet answered
    std::vector<qwork_send_t> sends;
    bool poll_in;
    bool wake_in;
    bool recv_in;
    bool poll_cancel;
    bool wake_cancel;
    bool recv_cancel;

    // the fake receive's
    bool recv_armed;
    bool recv_stopping;
    int recv_ends;

    bool wake_count;
    const char * st;
    int handed;
    int staged;
    int agains_before;
    int refusals;
    int plant_fired;
};

static int qwork_space( void * context )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    return QWORK_SQ - walk->queued;
}

static struct io_uring_sqe * qwork_sqe( void * context )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    struct io_uring_sqe * sqe = &walk->sq[walk->queued++];
    memset( sqe, 0, sizeof( *sqe ) );
    return sqe;
}

static int qwork_submit( void * context, bool wait )
{
    (void) context;
    (void) wait;
    return 0;
}

static int qwork_peek( void * context, struct rocketnet_uring_queue_worker_completion_t * completions, int max )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    int n = 0;
    while ( n < max && n < (int) walk->cq.size() )
    {
        completions[n] = walk->cq[(size_t) n];
        n++;
    }
    return n;
}

static void qwork_advance( void * context, int count )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    walk->cq.erase( walk->cq.begin(), walk->cq.begin() + count );
    walk->cq_count.erase( walk->cq_count.begin(), walk->cq_count.begin() + count );
}

// the receive module's contract (rocketnet_uring_receive_queue.h): it needs
// an arm once its multishot ended, until the stop; arm refuses only a NULL
// entry

static bool qwork_receive_needs_arm( void * context )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    return !walk->recv_armed && !walk->recv_stopping;
}

static bool qwork_receive_arm( void * context, struct io_uring_sqe * sqe )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    if ( !sqe )
    {
        return false;
    }
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->user_data = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 );
    walk->recv_armed = true;
    return true;
}

static void qwork_receive_cqe( void * context, int32_t res, uint32_t flags )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    (void) res;
    if ( !( flags & IORING_CQE_F_MORE ) )
    {
        walk->recv_armed = false;
    }
}

static void qwork_receive_stop( void * context )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    walk->recv_stopping = true;
}

static bool qwork_receive_armed( void * context )
{
    qwork_walk_t * walk = (qwork_walk_t *) context;
    return walk->recv_armed;
}

static void qwork_post( qwork_walk_t & walk, uint64_t user_data, int32_t res, uint32_t flags, int count )
{
    struct rocketnet_uring_queue_worker_completion_t c;
    c.user_data = user_data;
    c.res = res;
    c.flags = flags;
    walk.cq.push_back( c );
    walk.cq_count.push_back( count );
}

static void qwork_post_cancel( qwork_walk_t & walk, int what, int32_t res )
{
    qwork_post( walk, rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, 0, (uint32_t) what ), res, 0, 0 );
}

// the worker's prepared entries, now the kernel's
static void qwork_take_sqes( qwork_walk_t & walk )
{
    for ( int i = 0; i < walk.queued; i++ )
    {
        const struct io_uring_sqe * sqe = &walk.sq[i];
        switch ( sqe->opcode )
        {
            case IORING_OP_SENDMSG:
            {
                const struct msghdr * message = (const struct msghdr *) (uintptr_t) sqe->addr;
                qwork_send_t send;
                send.user_data = sqe->user_data;
                send.count = (int) message->msg_iovlen;
                walk.sends.push_back( send );
                break;
            }
            case IORING_OP_POLL_ADD:
                walk.poll_in = true;
                walk.poll_cancel = false;
                break;
            case IORING_OP_RECVMSG:
                walk.recv_in = true;
                walk.recv_cancel = false;
                break;
            case IORING_OP_READ:
                walk.wake_in = true;
                walk.wake_cancel = false;
                break;
            case IORING_OP_ASYNC_CANCEL:
            {
                // a cancel of a target no longer in the kernel completes at once
                const int kind = rocketnet_uring_tag_kind( sqe->addr );
                const int what = rocketnet_uring_tag_index( sqe->user_data );
                if ( kind == ROCKETNET_URING_TAG_POLL && walk.poll_in )
                {
                    walk.poll_cancel = true;
                }
                else if ( kind == ROCKETNET_URING_TAG_WAKE && walk.wake_in )
                {
                    walk.wake_cancel = true;
                }
                else if ( kind == ROCKETNET_URING_TAG_RECV && walk.recv_in )
                {
                    walk.recv_cancel = true;
                }
                else
                {
                    qwork_post_cancel( walk, what, -ENOENT );
                }
                break;
            }
            default:
                break;
        }
    }
    walk.queued = 0;
}

// main's eventfd write (RNUringQueueWorker Signal)
static void qwork_signal( qwork_walk_t & walk )
{
    if ( walk.wake_in )
    {
        walk.wake_in = false;
        qwork_post( walk, rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0, 0 );
        if ( walk.wake_cancel )
        {
            qwork_post_cancel( walk, ROCKETNET_URING_CANCEL_WAKE, -EALREADY );
        }
    }
    else
    {
        walk.wake_count = true;
    }
}

// what a send completion is to the model: "ok", "again" or "ref"
static const char * qwork_class( int32_t res, int count )
{
    const int result = rocketnet_uring_queue_send_result( res, true );
    if ( result == ROCKETNET_URING_QUEUE_SEND_AGAIN )
    {
        return "again";
    }
    if ( result == ROCKETNET_URING_QUEUE_SEND_REFUSED && count > 1 )
    {
        return "ref";
    }
    return "ok";
}

static bool qwork_cq_has( const qwork_walk_t & walk, int kind, int32_t * res = NULL )
{
    for ( size_t i = 0; i < walk.cq.size(); i++ )
    {
        if ( rocketnet_uring_tag_kind( walk.cq[i].user_data ) == kind )
        {
            if ( res )
            {
                *res = walk.cq[i].res;
            }
            return true;
        }
    }
    return false;
}

// a completion the model sees (all but a cancel's own)
static bool qwork_cq_visible( const qwork_walk_t & walk )
{
    for ( size_t i = 0; i < walk.cq.size(); i++ )
    {
        if ( rocketnet_uring_tag_kind( walk.cq[i].user_data ) != ROCKETNET_URING_TAG_CANCEL )
        {
            return true;
        }
    }
    return false;
}

static std::string qwork_string( const char * text )
{
    return std::string( "\"" ) + text + "\"";
}

static tla_record_t qwork_record( const qwork_walk_t & walk )
{
    const struct rocketnet_uring_queue_send_queue_t * queue = &walk.send->queue[0];
    const struct rocketnet_uring_queue_worker_t * worker = &walk.worker;

    // u: the held bank's runs waiting to be submitted; p: its packets not
    // done (staged, or in a run whose completion is not yet taken)
    int u = 0;
    int p = 0;
    if ( queue->held >= 0 )
    {
        const size_t base = (size_t) queue->held * QWORK_SLOTS;
        for ( int s = 0; s < QWORK_SLOTS; s++ )
        {
            const struct rocketnet_uring_queue_send_entry_t * e = &queue->entry[base + (size_t) s];
            const uint8_t state = queue->state[base + (size_t) s];
            if ( e->count > 0 && !e->kernel && state == ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED )
            {
                u++;
            }
            if ( state == ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED || state == ROCKETNET_URING_QUEUE_SEND_SLOT_RUN )
            {
                p++;
            }
        }
    }

    int c_ok = 0;
    int c_again = 0;
    int c_ref = 0;
    for ( size_t i = 0; i < walk.cq.size(); i++ )
    {
        if ( rocketnet_uring_tag_kind( walk.cq[i].user_data ) != ROCKETNET_URING_TAG_SEND )
        {
            continue;
        }
        const char * c = qwork_class( walk.cq[i].res, walk.cq_count[i] );
        c_ok += strcmp( c, "ok" ) == 0 ? 1 : 0;
        c_again += strcmp( c, "again" ) == 0 ? 1 : 0;
        c_ref += strcmp( c, "ref" ) == 0 ? 1 : 0;
    }

    // the kernel's: the runs it holds, unanswered, and what it holds or has
    // posted of the poll, the receive and the wake read
    const int k = (int) walk.sends.size();

    const char * poll = "none";
    if ( qwork_cq_has( walk, ROCKETNET_URING_TAG_POLL ) )
    {
        poll = "posted";
    }
    else if ( walk.poll_in )
    {
        poll = walk.poll_cancel ? "cancelling" : "armed";
    }

    const char * recv = "ended";
    if ( qwork_cq_has( walk, ROCKETNET_URING_TAG_RECV ) )
    {
        recv = "posted";
    }
    else if ( walk.recv_in )
    {
        recv = walk.recv_cancel ? "cancelling" : "armed";
    }

    const char * wake = "none";
    if ( qwork_cq_has( walk, ROCKETNET_URING_TAG_WAKE ) )
    {
        wake = "posted";
    }
    else if ( walk.wake_in )
    {
        wake = walk.wake_cancel ? "cancelling" : "armed";
    }

    tla_record_t record;
    record.add( "quit", tla_bool( worker->quit.load() ) );
    record.add( "seen", tla_bool( worker->seen ) );
    record.add( "st", qwork_string( walk.st ) );
    record.add( "handed", tla_int( walk.handed ) );
    record.add( "ready", tla_int( queue->handoff.ready_count ) );
    record.add( "held", tla_bool( queue->held >= 0 ) );
    record.add( "u", tla_int( u ) );
    record.add( "k", tla_int( k ) );
    record.add( "cOk", tla_int( c_ok ) );
    record.add( "cAgain", tla_int( c_again ) );
    record.add( "cRef", tla_int( c_ref ) );
    record.add( "blocked", tla_bool( worker->blocked ) );
    record.add( "poll", qwork_string( poll ) );
    record.add( "recv", qwork_string( recv ) );
    record.add( "wakeRead", qwork_string( wake ) );
    record.add( "wakeCount", tla_bool( walk.wake_count ) );
    record.add( "agains", tla_int( worker->agains ) );
    record.add( "againsBefore", tla_int( walk.agains_before ) );
    record.add( "refusals", tla_int( walk.refusals ) );
    record.add( "recvEnds", tla_int( walk.recv_ends ) );
    record.add( "p", tla_int( p ) );
    return record;
}

// the kernel answers one run: sent (whole), failed, -EAGAIN or a segment
// refusal, as the model's guards allow
static void qwork_answer( qwork_walk_t & walk, tla_random_t & random )
{
    const size_t i = (size_t) random.below( (int) walk.sends.size() );
    const qwork_send_t send = walk.sends[i];
    walk.sends.erase( walk.sends.begin() + (long) i );

    std::vector<int32_t> results;
    results.push_back( 100 * send.count );
    results.push_back( -EPERM );
    if ( walk.worker.seen || walk.agains_before < QWORK_MAX_AGAIN )
    {
        results.push_back( -EAGAIN );
    }
    if ( send.count > 1 && walk.refusals < QWORK_MAX_REFUSE )
    {
        results.push_back( -EIO );
    }
    else if ( send.count == 1 )
    {
        results.push_back( -EIO );      // a one-packet run refused: failed
    }

    const int32_t res = results[(size_t) random.below( (int) results.size() )];
    if ( res == -EAGAIN && !walk.worker.seen )
    {
        walk.agains_before++;
    }
    if ( res == -EIO && send.count > 1 )
    {
        walk.refusals++;
    }
    qwork_post( walk, send.user_data, res, 0, send.count );
}

// what of the worker's is in the kernel or waiting when it exits
static bool qwork_undrained( const qwork_walk_t & walk )
{
    const struct rocketnet_uring_queue_send_queue_t * queue = &walk.send->queue[0];
    return !walk.sends.empty() || walk.poll_in || walk.wake_in || walk.recv_in || queue->held >= 0 || queue->handoff.ready_count > 0 ||
           qwork_cq_visible( walk );
}

enum
{
    QWORK_STAGE,
    QWORK_PUBLISH,
    QWORK_STOP,
    QWORK_WORKER,
    QWORK_WAKES_WORKER,
    QWORK_ANSWER,
    QWORK_POLL_FIRES,
    QWORK_RECV_ENDS,
    QWORK_RECV_ENDS_ITSELF,
    QWORK_WAKE_READS,
    QWORK_WAKE_ENDS
};

// what main, the worker and the kernel may do now; main stages and
// publishes only while recording
static void qwork_choices( const qwork_walk_t & walk, tla_random_t & random, bool main_works, std::vector<int> & can )
{
    can.clear();
    const bool quit = walk.worker.quit.load();
    if ( main_works && !quit && walk.handed < QWORK_U && walk.staged < QWORK_SLOTS )
    {
        can.push_back( QWORK_STAGE );
    }
    if ( main_works && !quit && walk.handed < QWORK_U && walk.staged > 0 )
    {
        can.push_back( QWORK_PUBLISH );
    }
    if ( !quit && ( !main_works || walk.handed == QWORK_U || random.below( 32 ) == 0 ) )
    {
        can.push_back( QWORK_STOP );
    }
    if ( strcmp( walk.st, "awake" ) == 0 )
    {
        can.push_back( QWORK_WORKER );
        can.push_back( QWORK_WORKER );
        can.push_back( QWORK_WORKER );
    }
    if ( strcmp( walk.st, "asleep" ) == 0 && qwork_cq_visible( walk ) )
    {
        can.push_back( QWORK_WAKES_WORKER );
    }
    if ( !walk.sends.empty() )
    {
        can.push_back( QWORK_ANSWER );
    }
    if ( walk.poll_in )
    {
        can.push_back( QWORK_POLL_FIRES );
    }
    if ( walk.recv_in && walk.recv_cancel )
    {
        can.push_back( QWORK_RECV_ENDS );
    }
    if ( walk.recv_in && !walk.recv_cancel && walk.recv_ends < QWORK_MAX_RECV_END )
    {
        can.push_back( QWORK_RECV_ENDS_ITSELF );
    }
    if ( walk.wake_in && walk.wake_count )
    {
        can.push_back( QWORK_WAKE_READS );
    }
    if ( walk.wake_in && walk.wake_cancel )
    {
        can.push_back( QWORK_WAKE_ENDS );
    }
}

static void qwork_act( qwork_walk_t & walk, tla_random_t & random, int action )
{
    switch ( action )
    {
        case QWORK_STAGE:
        {
            const struct rocketnet_address_t * to = &walk.dest[random.below( QWORK_DESTS )];
            uint64_t id = 0;
            uint8_t * frame = rocketnet_uring_send_queues_start( walk.banks, to, &id );
            if ( frame )
            {
                if ( random.below( 5 ) == 0 )
                {
                    (void) rocketnet_uring_send_queues_abort( walk.banks, id, frame );
                }
                else
                {
                    (void) rocketnet_uring_send_queues_finish( walk.banks, id, frame, 100 );
                }
                walk.staged++;
            }
            break;
        }
        case QWORK_PUBLISH:
            if ( rocketnet_uring_queue_send_publish( walk.send, 0 ) )
            {
                walk.handed++;
                walk.staged = 0;
                qwork_signal( walk );
            }
            break;
        case QWORK_STOP:
            rocketnet_uring_queue_worker_stop( &walk.worker );
            qwork_signal( walk );
            break;
        case QWORK_WORKER:
        {
            const int did = rocketnet_uring_queue_worker_step( &walk.worker, true );
            qwork_take_sqes( walk );
            if ( did == ROCKETNET_URING_WORKER_STEP_NONE )
            {
                walk.st = "asleep";
            }
            else if ( did == ROCKETNET_URING_WORKER_STEP_EXIT )
            {
                walk.plant_fired += qwork_undrained( walk ) ? 1 : 0;
                walk.st = "exited";
            }
            break;
        }
        case QWORK_WAKES_WORKER:
            walk.st = "awake";
            break;
        case QWORK_ANSWER:
            qwork_answer( walk, random );
            break;
        case QWORK_POLL_FIRES:
            walk.poll_in = false;
            qwork_post( walk, rocketnet_uring_tag( ROCKETNET_URING_TAG_POLL, 0, 0 ), walk.poll_cancel ? -ECANCELED : POLLOUT, 0, 0 );
            if ( walk.poll_cancel )
            {
                qwork_post_cancel( walk, ROCKETNET_URING_CANCEL_POLL, 0 );
            }
            break;
        case QWORK_RECV_ENDS:
            walk.recv_in = false;
            qwork_post( walk, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ECANCELED, 0, 0 );
            qwork_post_cancel( walk, ROCKETNET_URING_CANCEL_RECV, 0 );
            break;
        case QWORK_RECV_ENDS_ITSELF:
            walk.recv_in = false;
            walk.recv_ends++;
            qwork_post( walk, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, 0, 0 ), -ENOBUFS, 0, 0 );
            break;
        case QWORK_WAKE_READS:
            walk.wake_in = false;
            walk.wake_count = false;
            qwork_post( walk, rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), 8, 0, 0 );
            if ( walk.wake_cancel )
            {
                qwork_post_cancel( walk, ROCKETNET_URING_CANCEL_WAKE, -EALREADY );
            }
            break;
        default:
            walk.wake_in = false;
            qwork_post( walk, rocketnet_uring_tag( ROCKETNET_URING_TAG_WAKE, 0, 0 ), -ECANCELED, 0, 0 );
            qwork_post_cancel( walk, ROCKETNET_URING_CANCEL_WAKE, 0 );
            break;
    }
}

static int trace_queue_worker( uint64_t seed, int steps, const char * out )
{
    qwork_walk_t walk{};

    walk.banks = rocketnet_uring_send_queues_create( NULL, 1, QWORK_SLOTS );
    walk.send = walk.banks ? rocketnet_uring_queue_send_create( NULL, walk.banks, 2, 1 << 16 ) : NULL;
    if ( !walk.send )
    {
        fprintf( stderr, "tla_trace queue-worker: could not create the handoff\n" );
        rocketnet_uring_send_queues_destroy( walk.banks );
        return 1;
    }

    for ( int d = 0; d < QWORK_DESTS; d++ )
    {
        memset( &walk.dest[d], 0, sizeof( walk.dest[d] ) );
        walk.dest[d].type = ROCKETNET_ADDRESS_IPV4;
        walk.dest[d].data.ipv4[0] = 10;
        walk.dest[d].data.ipv4[3] = (uint8_t) ( d + 1 );
        walk.dest[d].port = (uint16_t) ( 40001 + d );
    }
    walk.dest[QWORK_DESTS - 1].type = ROCKETNET_ADDRESS_IPV6;     // the socket is ipv4: this one it cannot name
    walk.dest[QWORK_DESTS - 1].data.ipv6[0] = 0x2001;
    walk.dest[QWORK_DESTS - 1].data.ipv6[7] = 3;

    walk.recv_in = true;
    walk.recv_armed = true;
    walk.st = "awake";

    struct rocketnet_uring_queue_worker_config_t config;
    memset( &config, 0, sizeof( config ) );
    config.queue_index = 0;
    config.fd = 7;
    config.ipv6 = false;
    config.wake_fd = -1;
    config.max_in_kernel = QWORK_K;
    config.max_retry = QWORK_MAX_RETRY;
    config.segment = true;
    config.send = walk.send;
    config.receive.context = &walk;
    config.receive.needs_arm = qwork_receive_needs_arm;
    config.receive.arm = qwork_receive_arm;
    config.receive.cqe = qwork_receive_cqe;
    config.receive.stop = qwork_receive_stop;
    config.receive.armed = qwork_receive_armed;
    config.kernel.context = &walk;
    config.kernel.space = qwork_space;
    config.kernel.sqe = qwork_sqe;
    config.kernel.submit = qwork_submit;
    config.kernel.peek = qwork_peek;
    config.kernel.advance = qwork_advance;
    if ( !rocketnet_uring_queue_worker_init( &walk.worker, &config ) )
    {
        fprintf( stderr, "tla_trace queue-worker: could not init the worker\n" );
        rocketnet_uring_queue_send_destroy( walk.send );
        rocketnet_uring_send_queues_destroy( walk.banks );
        return 1;
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    records.push_back( qwork_record( walk ) );

    int failed = 0;
    const int limit = steps < QWORK_MAX_STEPS ? steps : QWORK_MAX_STEPS;
    std::vector<int> can;
    for ( int step = 0; (int) records.size() <= limit && step < 20 * limit; step++ )
    {
        if ( strcmp( walk.st, "exited" ) == 0 )
        {
            break;
        }

        qwork_choices( walk, random, true, can );
        if ( can.empty() && !walk.worker.quit.load() )
        {
            can.push_back( QWORK_STOP );
        }
        if ( can.empty() )
        {
            fprintf( stderr, "tla_trace queue-worker: the worker hangs: stopped, not exited, and nothing of the kernel's to come\n" );
            failed = 1;
            break;
        }
        qwork_act( walk, random, can[(size_t) random.below( (int) can.size() )] );

        // a step that changed no model variable (a stage, a cancel's own
        // completion reaped) is no model step: it is not recorded
        tla_record_t record = qwork_record( walk );
        if ( record.text() != records.back().text() )
        {
            records.push_back( record );
        }

        // every completion the walk posts names something of the worker's
        if ( rocketnet_uring_queue_worker_counter( &walk.worker, ROCKETNET_URING_WORKER_COUNTER_STRAY ) != 0 )
        {
            fprintf( stderr, "tla_trace queue-worker: a completion the walk posted was stray\n" );
            failed = 1;
            break;
        }
    }

    // StopFinishes, past the records: main stops, and the worker and the
    // kernel go on until the worker exits
    for ( int step = 0; !failed && strcmp( walk.st, "exited" ) != 0; step++ )
    {
        qwork_choices( walk, random, false, can );
        if ( can.empty() || step == QWORK_FINISH_STEPS )
        {
            fprintf( stderr, "tla_trace queue-worker: the worker did not exit after the stop (%s)\n",
                     can.empty() ? "nothing of the kernel's to come" : "step bound" );
            failed = 1;
            break;
        }
        qwork_act( walk, random, can[(size_t) random.below( (int) can.size() )] );
        if ( rocketnet_uring_queue_worker_counter( &walk.worker, ROCKETNET_URING_WORKER_COUNTER_STRAY ) != 0 )
        {
            fprintf( stderr, "tla_trace queue-worker: a completion the walk posted was stray\n" );
            failed = 1;
        }
    }

    rocketnet_uring_queue_send_destroy( walk.send );
    rocketnet_uring_send_queues_destroy( walk.banks );

    printf( "PLANT-FIRED %d\n", walk.plant_fired );

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    U = %d\n    R = %d\n    K = %d\n    MaxRetry = %d\n    MaxAgain = %d\n    MaxRefuse = %d\n    MaxRecvEnd = %d\n"
              "    ExitRule = \"drained\"\n    RetryRule = \"bounded\"\n    WakeRule = \"armed\"\n",
              QWORK_U, QWORK_SLOTS, QWORK_K, QWORK_MAX_RETRY, QWORK_MAX_AGAIN, QWORK_MAX_REFUSE, QWORK_MAX_RECV_END );

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceQueueWorkerPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceQueueWorkerSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    const bool written = tla_write_trace( out, name, "TraceRNUringQueueWorker", "", constants, records );
    return written && !failed ? 0 : 1;
}

#else // #if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING && defined( IORING_RECV_MULTISHOT )

static int trace_queue_worker( uint64_t seed, int steps, const char * out )
{
    (void) seed;
    (void) steps;
    (void) out;
    fprintf( stderr, "tla_trace queue-worker: the worker is built only on Linux with liburing (ROCKETNET_IO_URING)\n" );
    return 1;
}

#endif // #if defined( __linux__ ) && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING && defined( IORING_RECV_MULTISHOT )

TLA_TRACE_LAYER( "queue-worker", trace_queue_worker );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer L1 of the io_uring per-queue send: the trace walk of one queue's
// bank handoff and its worker (sdk/rocketnet_uring_queue_send.cpp over the
// banks of sdk/rocketnet_uring_send_queue.cpp, the model
// proofs/tla/RNUringQueueSend.tla), checked by
// proofs/tla/TraceRNUringQueueSend.tla (proofs/tla/README-TraceCheck.md).
// No socket, no ring, no clock, no thread: the walk is the game threads, main, the
// worker and the kernel, one call per step, and one record per step
// (tla_step): the call, what it took and gave back, the action its own
// result says it took, and every model variable after it. A stage (start,
// then finish or abort) is Stage(d, f, s, pid) with the slot of the pointer
// start returned and the id it wrote, or StageFull(d, f, pid) when start
// returned NULL; a publish is Publish when it returned true, PublishKept
// when false; a claim is WorkerClaim(b) with the bank it returned; a submit
// is WorkerSubmit(d, k, R) with the run next handed out (its destination,
// count and slots); a reap is WorkerReap(n) with the count complete
// returned; a retire is WorkerRetire when it returned true,
// WorkerRetireRefused when false. The walk, as the worker inside its claim
// or retire, takes the handoff mutex itself (WorkerLocks) and lets it go
// (WorkerUnlocks); a publish in between must come back false at once
// (PublishKept): main only tries the mutex. The worker claims and retires
// only with the mutex free (the one thread would block on itself).
//
// The kernel is a fake completion source: each run the worker hands out is
// in it until the walk posts the run's completion, as its own step
// (KernelComplete), in any order, with a result that is sent (a byte count,
// short or whole), failed, -EAGAIN or a segment refusal; the model's result
// is what rocketnet_uring_queue_send_result makes of it. The worker reaps
// the posted completions oldest first.
//
// The plant binary's code is the handoff with
// ROCKETNET_URING_QUEUE_SEND_PLANT (a bank retired once every slot is
// submitted, its runs still in the kernel). The walk reports (tla_report_t)
// the planted effect where it shows: a retire that returned true with a run
// of the bank still pending, which the clean code refuses.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_URING_QUEUE_SEND_PLANT 1
#include "../sdk/rocketnet_uring_queue_send.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_uring_queue_send.h"
#include "../sdk/rocketnet_uring_send_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the instance: one queue (the queues share nothing but main's loop), its
// three banks of three slots, two destinations, runs of at most two
// packets, four claims a game thread (so a full bank refuses one), every bank
// reused, and handed-back runs. The model's bound on flushes is the walk's
// length, which no walk reaches, so a step is always enabled

#define QSEND_NB ROCKETNET_URING_SEND_BANKS
#define QSEND_SLOTS 3
#define QSEND_DESTS 2
#define QSEND_MAX_SEG 2
#define QSEND_DEMAND 4
#define QSEND_MAX_EAGAIN 4

// a run in the fake kernel: the code's run, its slots and the model's ids
struct qsend_run_t
{
    struct rocketnet_uring_queue_send_run_t run;
    std::vector<int> slots;
    std::vector<int> ids;
};

// a posted completion: the run, the raw res and the model's result
struct qsend_cqe_t
{
    qsend_run_t run;
    int res;
    const char * result;
};

struct qsend_walk_t
{
    struct rocketnet_uring_send_queues_t * banks;
    struct rocketnet_uring_queue_send_t * send;
    struct rocketnet_address_t dest[QSEND_DESTS + 1];          // destinations 1..QSEND_DESTS
    int pkt[QSEND_NB][QSEND_SLOTS];                             // the model's packet id of each slot, 0 for none
    std::vector<const char *> status;                           // each model id 1..: its last status; [0] unused
    std::vector<qsend_run_t> kernel;
    std::vector<qsend_cqe_t> cq;
    int next_id;
    int eagains;
    int demand;
    int flushes;
    bool kernel_write;
    bool locked;                                                // the walk holds the handoff mutex as the worker
};

static int qsend_dest_index( const qsend_walk_t & walk, const struct rocketnet_address_t * to )
{
    for ( int d = 1; d <= QSEND_DESTS; d++ )
    {
        if ( rocketnet_address_equal( &walk.dest[d], to ) )
        {
            return d;
        }
    }
    return 0;
}

static std::string qsend_seq( const std::vector<int> & values )
{
    std::string text = "<<";
    for ( size_t i = 0; i < values.size(); i++ )
    {
        if ( i > 0 )
        {
            text += ", ";
        }
        text += tla_int( values[i] );
    }
    return text + ">>";
}

static std::string qsend_set( std::vector<int> values )
{
    std::string text = "{";
    for ( size_t i = 0; i < values.size(); i++ )
    {
        if ( i > 0 )
        {
            text += ", ";
        }
        text += tla_int( values[i] );
    }
    return text + "}";
}

static std::string qsend_run_text( const qsend_run_t & run )
{
    return "[b |-> " + tla_int( run.run.bank ) + ", slots |-> " + qsend_set( run.slots ) + ", ids |-> " + qsend_set( run.ids ) + "]";
}

static std::string qsend_string( const char * text )
{
    return std::string( "\"" ) + text + "\"";
}

// the slot's model state, read from the code: the worker's view of the bank
// it holds and of a free bank (empty since its retire); the game threads' records
// of the producer and of a ready bank (staged when finished, aborted when
// not, empty past the claims)
static const char * qsend_slot_state( const qsend_walk_t & walk, int b, int s, int role )
{
    const struct rocketnet_uring_queue_send_queue_t * queue = &walk.send->queue[0];
    if ( role == 0 )
    {
        switch ( queue->state[b * QSEND_SLOTS + s] )
        {
            case ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED:
                return "staged";
            case ROCKETNET_URING_QUEUE_SEND_SLOT_ABORTED:
                return "aborted";
            case ROCKETNET_URING_QUEUE_SEND_SLOT_RUN:
                return "run";
            case ROCKETNET_URING_QUEUE_SEND_SLOT_SENT:
            case ROCKETNET_URING_QUEUE_SEND_SLOT_DROPPED:
                return "done";
            default:
                return "empty";
        }
    }

    const struct rocketnet_uring_send_bank_t * bank = &walk.banks->queue[0].bank[b];
    const uint64_t claims = bank->num_packets.load();
    if ( (uint64_t) s >= claims )
    {
        return "empty";
    }
    return bank->slot[s].packet_bytes > 0 ? "staged" : "aborted";
}

// 0: the worker's view (held or free), 1: the game threads' records (producer or ready)
static int qsend_role( const qsend_walk_t & walk, int b )
{
    const struct rocketnet_uring_queue_send_queue_t * queue = &walk.send->queue[0];
    if ( b == walk.banks->queue[0].producer )
    {
        return 1;
    }
    for ( int i = 0; i < queue->handoff.ready_count; i++ )
    {
        if ( queue->handoff.ready_index[( queue->handoff.ready_head + i ) % QSEND_NB] == b )
        {
            return 1;
        }
    }
    return 0;
}

static tla_record_t qsend_record( qsend_walk_t & walk )
{
    const struct rocketnet_uring_queue_send_queue_t * queue = &walk.send->queue[0];
    const struct rocketnet_uring_send_queue_t * banks = &walk.banks->queue[0];

    std::vector<int> ready;
    for ( int i = 0; i < queue->handoff.ready_count; i++ )
    {
        ready.push_back( queue->handoff.ready_index[( queue->handoff.ready_head + i ) % QSEND_NB] );
    }
    std::vector<int> free_banks;
    for ( int i = 0; i < queue->handoff.free_count; i++ )
    {
        free_banks.push_back( queue->handoff.free_index[( queue->handoff.free_head + i ) % QSEND_NB] );
    }

    std::vector<std::string> bank_keys;
    std::vector<std::string> num;
    std::vector<std::string> st;
    std::vector<std::string> dest;
    std::vector<std::string> pkt;
    std::vector<std::string> pend;
    for ( int b = 0; b < QSEND_NB; b++ )
    {
        bank_keys.push_back( tla_int( b ) );
        const uint64_t claims = banks->bank[b].num_packets.load();
        num.push_back( tla_int( (int64_t) ( claims < QSEND_SLOTS ? claims : QSEND_SLOTS ) ) );
        pend.push_back( tla_int( queue->pending[b] ) );

        const int role = qsend_role( walk, b );
        std::vector<std::string> slot_keys;
        std::vector<std::string> slot_st;
        std::vector<std::string> slot_dest;
        std::vector<std::string> slot_pkt;
        for ( int s = 0; s < QSEND_SLOTS; s++ )
        {
            slot_keys.push_back( tla_int( s ) );
            const char * state = qsend_slot_state( walk, b, s, role );
            slot_st.push_back( qsend_string( state ) );
            const bool named = strcmp( state, "empty" ) != 0 && strcmp( state, "aborted" ) != 0;
            slot_dest.push_back( tla_int( named ? qsend_dest_index( walk, &banks->bank[b].slot[s].to ) : 0 ) );
            slot_pkt.push_back( tla_int( named ? walk.pkt[b][s] : 0 ) );
        }
        st.push_back( tla_function( slot_keys, slot_st ) );
        dest.push_back( tla_function( slot_keys, slot_dest ) );
        pkt.push_back( tla_function( slot_keys, slot_pkt ) );
    }

    // each id's status from the code's slot state while its bank holds it,
    // its last status after
    for ( int b = 0; b < QSEND_NB; b++ )
    {
        const int role = qsend_role( walk, b );
        for ( int s = 0; s < QSEND_SLOTS; s++ )
        {
            const int id = walk.pkt[b][s];
            if ( id == 0 )
            {
                continue;
            }
            if ( role == 1 )
            {
                walk.status[(size_t) id] = "staged";
                continue;
            }
            switch ( queue->state[b * QSEND_SLOTS + s] )
            {
                case ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED:
                    walk.status[(size_t) id] = "staged";
                    break;
                case ROCKETNET_URING_QUEUE_SEND_SLOT_RUN:
                    walk.status[(size_t) id] = "kernel";
                    break;
                case ROCKETNET_URING_QUEUE_SEND_SLOT_SENT:
                    walk.status[(size_t) id] = "sent";
                    break;
                case ROCKETNET_URING_QUEUE_SEND_SLOT_DROPPED:
                    walk.status[(size_t) id] = "dropped";
                    break;
                default:
                    break;
            }
        }
    }

    // status is a function on 1..Ids, a sequence: written as one, TLC
    // reads it in one pass where k :> v @@ ... rebuilds it per key
    std::vector<std::string> status;
    for ( size_t i = 1; i < walk.status.size(); i++ )
    {
        status.push_back( qsend_string( walk.status[i] ) );
    }

    std::string runs = "{";
    for ( size_t i = 0; i < walk.kernel.size(); i++ )
    {
        runs += ( i > 0 ? ", " : "" ) + qsend_run_text( walk.kernel[i] );
    }
    runs += "}";

    std::string cq = "<<";
    for ( size_t i = 0; i < walk.cq.size(); i++ )
    {
        cq += ( i > 0 ? ", " : "" ) + std::string( "[run |-> " ) + qsend_run_text( walk.cq[i].run ) + ", res |-> " + qsend_string( walk.cq[i].result ) + "]";
    }
    cq += ">>";

    tla_record_t record;
    record.add( "producer", tla_int( banks->producer ) );
    record.add( "ready", qsend_seq( ready ) );
    record.add( "worker", tla_int( queue->held ) );
    record.add( "free", qsend_seq( free_banks ) );
    record.add( "num", tla_function( bank_keys, num ) );
    record.add( "st", tla_function( bank_keys, st ) );
    record.add( "dest", tla_function( bank_keys, dest ) );
    record.add( "pkt", tla_function( bank_keys, pkt ) );
    record.add( "runs", runs );
    record.add( "cq", cq );
    record.add( "pend", tla_function( bank_keys, pend ) );
    record.add( "status", tla_sequence( status ) );
    record.add( "nextId", tla_int( walk.next_id ) );
    record.add( "eagains", tla_int( walk.eagains ) );
    record.add( "demand", tla_int( walk.demand ) );
    record.add( "flushes", tla_int( walk.flushes ) );
    record.add( "kernelWrite", tla_bool( walk.kernel_write ) );
    record.add( "pids", tla_int( (int64_t) walk.banks->packet_id.load() ) );
    record.add( "locked", tla_bool( walk.locked ) );
    record.add( "mainWaits", tla_bool( false ) );
    return record;
}

static bool qsend_kernel_owns( const qsend_walk_t & walk, int b, int s )
{
    for ( const qsend_run_t & run : walk.kernel )
    {
        for ( int slot : run.slots )
        {
            if ( run.run.bank == b && slot == s )
            {
                return true;
            }
        }
    }
    for ( const qsend_cqe_t & cqe : walk.cq )
    {
        for ( int slot : cqe.run.slots )
        {
            if ( cqe.run.run.bank == b && slot == s )
            {
                return true;
            }
        }
    }
    return false;
}

// RNUringQueueSend Stage(d, f, s, pid): start, then finish (f) or abort, on
// the producer, with s the slot of the pointer start returned and pid the
// id it wrote; StageFull(d, f, pid) when start returned NULL. NULL, the
// walk fails, when the finish or abort through start's own pointer is
// refused
static const char * qsend_stage( qsend_walk_t & walk, int d, bool finished, int packet_bytes, std::string & args )
{
    walk.demand++;
    uint64_t packet_id = 0;
    uint8_t * frame = rocketnet_uring_send_queues_start( walk.banks, &walk.dest[d], &packet_id );
    if ( !frame )
    {
        args = tla_sequence( { tla_int( d ), tla_bool( finished ), tla_int( (int64_t) packet_id ) } );
        return "StageFull";
    }

    const int b = walk.banks->queue[0].producer;
    const int s = (int) ( ( frame - walk.banks->queue[0].bank[b].data ) / ROCKETNET_FRAME_STRIDE );
    if ( qsend_kernel_owns( walk, b, s ) )
    {
        walk.kernel_write = true;
    }

    bool done = false;
    if ( finished )
    {
        done = rocketnet_uring_send_queues_finish( walk.banks, packet_id, frame, packet_bytes );
        walk.pkt[b][s] = walk.next_id;
        walk.status[(size_t) walk.next_id] = "staged";
        walk.next_id++;
    }
    else
    {
        done = rocketnet_uring_send_queues_abort( walk.banks, packet_id, frame );
        walk.pkt[b][s] = 0;
    }
    args = tla_sequence( { tla_int( d ), tla_bool( finished ), tla_int( s ), tla_int( (int64_t) packet_id ) } );
    return done ? "Stage" : NULL;
}

// RNUringQueueSend WorkerSubmit(d, k, R): the run next handed out goes into
// the kernel; next returning false is no model step, recorded with
// arguments no WorkerSubmit takes
static std::string qsend_submit( qsend_walk_t & walk )
{
    qsend_run_t run;
    if ( !rocketnet_uring_queue_send_next( walk.send, 0, &run.run ) )
    {
        return tla_sequence( { tla_int( 0 ), tla_int( 0 ), "{}" } );
    }
    const int * order = walk.send->queue[0].order + run.run.bank * QSEND_SLOTS;
    for ( int i = 0; i < run.run.count; i++ )
    {
        const int s = order[run.run.first + i];
        run.slots.push_back( s );
        run.ids.push_back( walk.pkt[run.run.bank][s] );
    }
    walk.kernel.push_back( run );
    return tla_sequence( { tla_int( qsend_dest_index( walk, run.run.to ) ), tla_int( run.run.count ), qsend_set( run.slots ) } );
}

// RNUringQueueSend KernelComplete(r, res): the kernel posts one run's
// completion, any run in it, with the res the walk picks; the model's result
// is what the code makes of that res
static std::string qsend_post( qsend_walk_t & walk, tla_random_t & random )
{
    const int index = random.below( (int) walk.kernel.size() );
    qsend_cqe_t cqe;
    cqe.run = walk.kernel[(size_t) index];

    int pick = random.below( walk.eagains < QSEND_MAX_EAGAIN ? 6 : 3 );
    const int whole = ( cqe.run.run.count - 1 ) * cqe.run.run.segment_bytes + cqe.run.run.last_bytes;
    switch ( pick )
    {
        case 0:
            cqe.res = whole;
            break;
        case 1:
            cqe.res = 1 + random.below( whole );     // short: still sent
            break;
        case 2:
            cqe.res = random.below( 2 ) == 0 ? -EPERM : -ENETUNREACH;
            break;
        case 3:
        case 4:
            cqe.res = -EAGAIN;
            break;
        default:
            cqe.res = -EIO;                          // a segment refusal; one packet's is failed
            break;
    }

    switch ( rocketnet_uring_queue_send_result( cqe.res, cqe.run.run.count > 1 ) )
    {
        case ROCKETNET_URING_QUEUE_SEND_OK:
            cqe.result = "ok";
            break;
        case ROCKETNET_URING_QUEUE_SEND_FAILED:
            cqe.result = "err";
            break;
        default:
            cqe.result = "eagain";
            walk.eagains++;
            break;
    }

    walk.kernel.erase( walk.kernel.begin() + index );
    walk.cq.push_back( cqe );
    return tla_sequence( { qsend_run_text( cqe.run ), qsend_string( cqe.result ) } );
}

// RNUringQueueSend WorkerReap(n): the oldest posted completion; n is the
// count complete returned
static int qsend_reap( qsend_walk_t & walk )
{
    const qsend_cqe_t cqe = walk.cq.front();
    walk.cq.erase( walk.cq.begin() );
    const int result = rocketnet_uring_queue_send_result( cqe.res, cqe.run.run.count > 1 );
    return rocketnet_uring_queue_send_complete( walk.send, 0, cqe.run.run.tag, result );
}

static int trace_queue_send( uint64_t seed, int steps, const char * out )
{
    qsend_walk_t walk;
    walk.banks = rocketnet_uring_send_queues_create( NULL, 1, QSEND_SLOTS );
    walk.send = walk.banks ? rocketnet_uring_queue_send_create( NULL, walk.banks, QSEND_MAX_SEG, 1 << 20 ) : NULL;
    if ( !walk.send )
    {
        rocketnet_uring_send_queues_destroy( walk.banks );
        return 1;
    }

    // the model's Ids: (MaxFlush + 1) * Demand, MaxFlush the walk's length
    const int ids = ( steps + 1 ) * QSEND_DEMAND;
    memset( walk.pkt, 0, sizeof( walk.pkt ) );
    walk.status.assign( (size_t) ids + 1, "none" );
    walk.next_id = 1;
    walk.eagains = 0;
    walk.demand = 0;
    walk.flushes = 0;
    walk.kernel_write = false;
    walk.locked = false;

    for ( int d = 1; d <= QSEND_DESTS; d++ )
    {
        memset( &walk.dest[d], 0, sizeof( walk.dest[d] ) );
        walk.dest[d].type = ROCKETNET_ADDRESS_IPV4;
        walk.dest[d].data.ipv4[0] = 10;
        walk.dest[d].data.ipv4[3] = (uint8_t) d;
        walk.dest[d].port = (uint16_t) ( 40000 + d );
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", qsend_record( walk ) ) );

    for ( int step = 0; step < steps; step++ )
    {
        struct rocketnet_uring_queue_send_queue_t * queue = &walk.send->queue[0];

        // the enabled actions, as the model's guards say; publish always is
        // (the bound on flushes is the walk's length), so a step always is
        enum
        {
            STAGE,
            PUBLISH,
            CLAIM,
            SUBMIT,
            POST,
            REAP,
            RETIRE,
            LOCK,
            UNLOCK
        };
        std::vector<int> can;
        if ( walk.demand < QSEND_DEMAND )
        {
            can.push_back( STAGE );
            can.push_back( STAGE );
        }
        if ( walk.demand == QSEND_DEMAND || random.below( 4 ) == 0 )
        {
            can.push_back( PUBLISH );
        }
        if ( queue->held < 0 && queue->handoff.ready_count > 0 && !walk.locked )
        {
            can.push_back( CLAIM );
        }
        if ( queue->held >= 0 && queue->unsubmitted > 0 )
        {
            can.push_back( SUBMIT );
        }
        if ( !walk.kernel.empty() )
        {
            can.push_back( POST );
        }
        if ( !walk.cq.empty() )
        {
            can.push_back( REAP );
        }
        if ( queue->held >= 0 && !walk.locked && ( queue->unsubmitted == 0 || random.below( 4 ) == 0 ) )
        {
            can.push_back( RETIRE );
        }
        // the worker inside its claim or retire: the mutex held; held, a
        // publish is tried at once more often, so the kept publish is reached
        if ( walk.locked )
        {
            can.push_back( UNLOCK );
            can.push_back( PUBLISH );
            can.push_back( PUBLISH );
        }
        else if ( ( queue->held < 0 || ( queue->unsubmitted == 0 && queue->pending[queue->held] == 0 ) ) && random.below( 4 ) == 0 )
        {
            // as the claim takes it (no bank held) or a retire that goes
            can.push_back( LOCK );
        }
        if ( can.empty() )
        {
            can.push_back( PUBLISH );
        }

        const char * op = "";
        const char * act = "";
        std::string args = "<<>>";
        bool planted = false;
        switch ( can[(size_t) random.below( (int) can.size() )] )
        {
            case STAGE:
            {
                const int d = 1 + random.below( QSEND_DESTS );
                const bool finished = random.below( 4 ) != 0;
                op = "stage";
                act = qsend_stage( walk, d, finished, 1 + random.below( ROCKETNET_RECEIVE_CEILING ), args );
                if ( !act )
                {
                    fprintf( stderr, "tla_trace queue-send: the finish or abort through start's own pointer was refused\n" );
                    rocketnet_uring_queue_send_destroy( walk.send );
                    rocketnet_uring_send_queues_destroy( walk.banks );
                    return 1;
                }
                break;
            }
            case PUBLISH:
                op = "publish";
                act = rocketnet_uring_queue_send_publish( walk.send, 0 ) ? "Publish" : "PublishKept";
                walk.flushes++;
                walk.demand = 0;
                break;
            case CLAIM:
                op = "claim";
                act = "WorkerClaim";
                args = tla_sequence( { tla_int( rocketnet_uring_queue_send_claim( walk.send, 0 ) ) } );
                break;
            case SUBMIT:
                op = "submit";
                act = "WorkerSubmit";
                args = qsend_submit( walk );
                break;
            case POST:
                op = "post";
                act = "KernelComplete";
                args = qsend_post( walk, random );
                break;
            case REAP:
                op = "reap";
                act = "WorkerReap";
                args = tla_sequence( { tla_int( qsend_reap( walk ) ) } );
                break;
            case LOCK:
                op = "lock";
                act = "WorkerLocks";
                rocketnet_platform_mutex_acquire( &queue->mutex );
                walk.locked = true;
                break;
            case UNLOCK:
                op = "unlock";
                act = "WorkerUnlocks";
                rocketnet_platform_mutex_release( &queue->mutex );
                walk.locked = false;
                break;
            default:
            {
                const int held = queue->held;
                const bool pending = queue->pending[held] != 0;
                op = "retire";
                const bool retired = rocketnet_uring_queue_send_retire( walk.send, 0 );
                act = retired ? "WorkerRetire" : "WorkerRetireRefused";
                if ( retired )
                {
                    planted = pending;
                    for ( int s = 0; s < QSEND_SLOTS; s++ )
                    {
                        walk.pkt[held][s] = 0;
                    }
                }
                break;
            }
        }

        report.act( act );
        records.push_back( tla_step( op, act, args, qsend_record( walk ) ) );
        if ( planted )
        {
            report.planted( (int) records.size() );
        }
    }

    if ( walk.locked )
    {
        rocketnet_platform_mutex_release( &walk.send->queue[0].mutex );
    }
    rocketnet_uring_queue_send_destroy( walk.send );
    rocketnet_uring_send_queues_destroy( walk.banks );

    report.print();

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    NB = %d\n    S = %d\n    Dests = {1, 2}\n    MaxSeg = %d\n    Demand = %d\n    MaxFlush = %d\n    MaxEagain = %d\n"
              "    RetireRule = \"cqe\"\n    EagainRule = \"retry\"\n    LockRule = \"try\"\n",
              QSEND_NB, QSEND_SLOTS, QSEND_MAX_SEG, QSEND_DEMAND, steps, QSEND_MAX_EAGAIN );

    return tla_write_trace( out, "MCTraceQueueSend", seed, "TraceRNUringQueueSend", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "queue-send", trace_queue_send );

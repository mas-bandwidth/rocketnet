/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7: processor groups (GetLogicalProcessorInformationEx, SetThreadGroupAffinity)
#endif // #ifndef _WIN32_WINNT

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS || ROCKETNET_DEV

#include "rocketnet_server_iocp.h"
#include "rocketnet_platform.h"
#include "rocketnet_receive_prefilter.h"
#include "rocketnet_read_write.h"
#include "rocketnet_receive_handoff.h"
#if ROCKETNET_DEV
#include "rocketnet_receive_delivery_check.h"
#endif // #if ROCKETNET_DEV
#include <atomic>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct rocketnet_iocp_ref_t
{
    uint64_t owner_epoch;
    uint64_t generation;
    uint32_t index;
};

enum rocketnet_iocp_operation_state_t
{
    ROCKETNET_IOCP_OPERATION_IDLE,
    ROCKETNET_IOCP_OPERATION_FREE,
    ROCKETNET_IOCP_OPERATION_STAGED,
    ROCKETNET_IOCP_OPERATION_SEND_POSTED,
    ROCKETNET_IOCP_OPERATION_RECEIVE_POSTED,
    ROCKETNET_IOCP_OPERATION_RETRY,
    ROCKETNET_IOCP_OPERATION_RETIRED
};

// Every cross-thread work list is a bounded MPMC ring of full operation
// references. A cell's sequence publishes its ref with release/acquire; the
// monotonically claimed enqueue/dequeue positions prevent ABA at a physical
// cell. free_sends has caller consumers and TX/completion producers;
// retry_receives has completion-worker producers and consumers. A successful
// operation touches one cell, but contention may cause CAS retries: it is
// lock-free bounded storage, not a hard constant-time claim.
// proofs/tla/RNIocpRefQueue.tla is the layout model of the cells, sequences
// and position counters, with the stall between a read and its CAS.
// The cells are capacity entries (a power of two, for the position mask) of
// the transport's sized allocation, bound at create.
struct rocketnet_iocp_ref_cell_t
{
    std::atomic<uint64_t> sequence{ 0 };
    rocketnet_iocp_ref_t ref = {};
};

struct rocketnet_iocp_ref_queue_t
{
    rocketnet_iocp_ref_cell_t * cells = NULL;
    uint64_t capacity = 0;
    std::atomic<uint64_t> enqueue{ 0 };
    std::atomic<uint64_t> dequeue{ 0 };
#if ROCKETNET_DEV
    std::atomic<uint64_t> pushes{ 0 };
    std::atomic<uint64_t> pops{ 0 };
    std::atomic<uint64_t> probes{ 0 };
    rocketnet_iocp_ref_cell_t * stalled_pop_cell = NULL;
    uint64_t stalled_pop_sequence = 0;
    bool stall_next_pop = false;
    int stalled_pop_holds = 0;                  // refused pushes the stalled pop outlasts before its store lands
    std::atomic<uint64_t> yields{ 0 };          // times a retried push gave up the processor
#endif // #if ROCKETNET_DEV
};

// Binds the queue to capacity cells of raw storage, constructs them and
// empties the queue. capacity is a power of two.
static void rocketnet_iocp_queue_initialize( rocketnet_iocp_ref_queue_t * queue, void * storage, uint64_t capacity )
{
    rocketnet_assert( capacity > 0 && ( capacity & ( capacity - 1 ) ) == 0 );
    queue->cells = (rocketnet_iocp_ref_cell_t *) storage;
    queue->capacity = capacity;
    queue->enqueue.store( 0, std::memory_order_relaxed );
    queue->dequeue.store( 0, std::memory_order_relaxed );
    for ( uint64_t i = 0; i < capacity; ++i )
    {
        new ( &queue->cells[i] ) rocketnet_iocp_ref_cell_t();
        queue->cells[i].sequence.store( i, std::memory_order_relaxed );
    }
}

static bool rocketnet_iocp_queue_push( rocketnet_iocp_ref_queue_t * queue, const rocketnet_iocp_ref_t & ref )
{
    // proofs/tla/RNIocpRefQueue.tla PushStart loads the position, PushRead
    // reads the sequence (free, full or passed), PushClaim is the CAS and
    // PushPublish the write and release store.
    uint64_t position = queue->enqueue.load( std::memory_order_relaxed );
    rocketnet_iocp_ref_cell_t * cell;

    for ( ;; )
    {
#if ROCKETNET_DEV
        queue->probes.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
        cell = &queue->cells[position & ( queue->capacity - 1 )];

        const uint64_t sequence = cell->sequence.load( std::memory_order_acquire );
        const int64_t difference = (int64_t) ( sequence - position );
        if ( difference == 0 )
        {
            if ( queue->enqueue.compare_exchange_weak( position, position + 1, std::memory_order_relaxed ) )
            {
                break;
            }
        }
        else if ( difference < 0 )
        {
#if ROCKETNET_DEV
            if ( queue->stalled_pop_cell != NULL && queue->stalled_pop_holds > 0 )
            {
                queue->stalled_pop_holds--;
            }
            else if ( queue->stalled_pop_cell != NULL )
            {
                queue->stalled_pop_cell->sequence.store( queue->stalled_pop_sequence, std::memory_order_release );
                queue->stalled_pop_cell = NULL;
            }
#endif // #if ROCKETNET_DEV
            return false;
        }
        else
        {
            position = queue->enqueue.load( std::memory_order_relaxed );
        }
    }

    cell->ref = ref;
    cell->sequence.store( position + 1, std::memory_order_release );
#if ROCKETNET_DEV
    queue->pushes.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    return true;
}

static bool rocketnet_iocp_queue_pop( rocketnet_iocp_ref_queue_t * queue, rocketnet_iocp_ref_t * ref )
{
    // proofs/tla/RNIocpRefQueue.tla PopStart loads the position, PopRead
    // reads the sequence (published, empty or passed), PopClaim is the CAS
    // and PopTake the read and the store that frees the cell a round later.
    uint64_t position = queue->dequeue.load( std::memory_order_relaxed );
    rocketnet_iocp_ref_cell_t * cell;

    for ( ;; )
    {
#if ROCKETNET_DEV
        queue->probes.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
        cell = &queue->cells[position & ( queue->capacity - 1 )];

        const uint64_t sequence = cell->sequence.load( std::memory_order_acquire );
        const int64_t difference = (int64_t) ( sequence - ( position + 1 ) );
        if ( difference == 0 )
        {
            if ( queue->dequeue.compare_exchange_weak( position, position + 1, std::memory_order_relaxed ) )
            {
                break;
            }
        }
        else if ( difference < 0 )
        {
            return false;
        }
        else
        {
            position = queue->dequeue.load( std::memory_order_relaxed );
        }
    }

    *ref = cell->ref;
#if ROCKETNET_DEV
    if ( queue->stall_next_pop )
    {
        queue->stall_next_pop = false;
        queue->stalled_pop_cell = cell;
        queue->stalled_pop_sequence = position + queue->capacity;
    }
    else
#endif // #if ROCKETNET_DEV
    {
        cell->sequence.store( position + queue->capacity, std::memory_order_release );
    }
#if ROCKETNET_DEV
    queue->pops.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    return true;
}

// proofs/tla/RNIocpRefQueue.tla PushRead's refusal branch: a refused push
// keeps its operation and tries again (RefusedNeverLost); giving up would
// lose the operation for good (MCRNIocpRefQueueBrokenDropRefused.cfg). Each
// queue holds at most capacity operations, so the only refusal is a pop that
// claimed this cell's last round and has not yet stored the sequence that
// frees it (RefusalOnlyWhilePopStalled): two statements of another thread,
// which last only while that thread is descheduled between them. The retry
// is therefore never an empty spin: every refusal pauses the processor, and
// every ROCKETNET_PLATFORM_SPIN_YIELD_AFTER refusals the caller gives up its
// processor so the preempted popper can run on it, as
// rocketnet_platform_spin_lock_u64 does for a preempted holder. That is the
// fairness the model's EveryOperationPopped assumes of the popper.
static void rocketnet_iocp_queue_push_retry( rocketnet_iocp_ref_queue_t * queue, const rocketnet_iocp_ref_t & ref )
{
    unsigned refusals = 0;

    while ( !rocketnet_iocp_queue_push( queue, ref ) )
    {
        if ( ++refusals < ROCKETNET_PLATFORM_SPIN_YIELD_AFTER )
        {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            YieldProcessor();
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            continue;
        }

        refusals = 0;
#if ROCKETNET_DEV
        queue->yields.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        SwitchToThread();
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }
}

static bool rocketnet_iocp_queue_release_send( rocketnet_iocp_ref_queue_t * free_sends,
                                               const rocketnet_iocp_ref_t & ref, int * op_state )
{
    *op_state = ROCKETNET_IOCP_OPERATION_FREE;
    rocketnet_iocp_queue_push_retry( free_sends, ref );
    return true;
}

// proofs/tla/RNIocpRetryStop.tla PostRefused, PushClaim and PushPublish: a
// refused receive post is RETRY and its reference goes onto retry_receives.
// The queue has a cell for every receive operation, so it is never full; one
// completion worker, production's count, is its only popper and never sees
// the push refused (MCRNIocpRefQueueOneConsumer.cfg). With more workers one
// stalled in its pop refuses the push, which is retried as a send release
// is: a refusal never retires the receive (RetiredOnlyByStop).
static void rocketnet_iocp_queue_retry_receive( rocketnet_iocp_ref_queue_t * retry_receives,
                                                const rocketnet_iocp_ref_t & ref, int * op_state )
{
    *op_state = ROCKETNET_IOCP_OPERATION_RETRY;
    rocketnet_iocp_queue_push_retry( retry_receives, ref );
}

static bool rocketnet_iocp_queue_send_idle( const rocketnet_iocp_ref_queue_t * free_sends,
                                            uint64_t num_staged, int send_queue_size )
{
    if ( num_staged != 0 )
    {
        return false;
    }
    const uint64_t dequeue = free_sends->dequeue.load( std::memory_order_acquire );
    const uint64_t enqueue = free_sends->enqueue.load( std::memory_order_acquire );
    return enqueue - dequeue >= (uint64_t) send_queue_size;
}

// The worker prefilter's state as one word, so a completion worker reads it
// with one load per batch: bit 32 set once the server's packet filter is
// published, its four bytes in bits 0-31 (rocketnet_server_iocp_set_packet_filter).
#define ROCKETNET_IOCP_FILTER_ON ( 1ull << 32 )

static uint64_t rocketnet_iocp_filter_state( const uint8_t * packet_filter )
{
    return ROCKETNET_IOCP_FILTER_ON | (uint64_t) packet_filter[0] | ( (uint64_t) packet_filter[1] << 8 ) |
           ( (uint64_t) packet_filter[2] << 16 ) | ( (uint64_t) packet_filter[3] << 24 );
}

// One received datagram's admission to its worker's receive bank, the
// completion worker's only decision about it. The prefilter runs first, so a
// datagram main would refuse on its stateless rungs never takes a slot; only
// a datagram it accepts (or every datagram, before the filter is published)
// meets the capacity check. ACCEPT means the caller banks it at num_packets.
// proofs/tla/RNIocpReceiveAdmit.tla Arrive: Refuse, then Bank or Full.
static rocketnet_receive_drop_t rocketnet_iocp_receive_admit( uint64_t filter_state, const uint8_t * packet_data, int packet_bytes,
                                                              int num_packets, int capacity )
{
#if defined( ROCKETNET_IOCP_RECEIVE_ADMIT_PLANT )
    // the trace check's plant (tests/tla_trace_iocp.cpp): the capacity check
    // first, the order the blast row measured. Its trace must fail.
    if ( num_packets >= capacity )
    {
        return ROCKETNET_RECEIVE_DROP_BANK_FULL;
    }
    (void) filter_state;
    (void) packet_data;
    (void) packet_bytes;
    return ROCKETNET_RECEIVE_ACCEPT;
#else  // #if defined( ROCKETNET_IOCP_RECEIVE_ADMIT_PLANT )
    if ( filter_state & ROCKETNET_IOCP_FILTER_ON )
    {
        const uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = { (uint8_t) filter_state, (uint8_t) ( filter_state >> 8 ),
                                                                      (uint8_t) ( filter_state >> 16 ), (uint8_t) ( filter_state >> 24 ) };
        const rocketnet_receive_drop_t drop = rocketnet_receive_prefilter( packet_data, packet_bytes, packet_filter );
        if ( drop != ROCKETNET_RECEIVE_ACCEPT )
        {
            return drop;
        }
    }
    return num_packets < capacity ? ROCKETNET_RECEIVE_ACCEPT : ROCKETNET_RECEIVE_DROP_BANK_FULL;
#endif // #if defined( ROCKETNET_IOCP_RECEIVE_ADMIT_PLANT )
}

// The send lane of a destination: FNV-1a over exactly the fields
// rocketnet_address_equal compares (the type, the address bytes of that type
// and the port), so equal addresses always share a lane and every packet of a
// client stages, and is posted, in one lane in the order it was started.
// proofs/tla/RNIocpSendLanes.tla DestLane; the reversed witness
// MCRNIocpSendLanesBrokenAnyLane breaks InOrder without it.
static int rocketnet_iocp_lane_index( const rocketnet_address_t * to, int lanes )
{
    if ( lanes <= 1 )
    {
        return 0;
    }
    uint32_t hash = 2166136261u;
    const auto fold = [&hash]( uint8_t byte )
    {
        hash ^= (uint32_t) byte;
        hash *= 16777619u;
    };
    fold( to->type );
    if ( to->type == ROCKETNET_ADDRESS_IPV4 )
    {
        for ( int i = 0; i < 4; ++i )
        {
            fold( to->data.ipv4[i] );
        }
    }
    else if ( to->type == ROCKETNET_ADDRESS_IPV6 )
    {
        for ( int i = 0; i < 8; ++i )
        {
            fold( (uint8_t) ( to->data.ipv6[i] >> 8 ) );
            fold( (uint8_t) to->data.ipv6[i] );
        }
    }
    fold( (uint8_t) ( to->port >> 8 ) );
    fold( (uint8_t) to->port );
    return (int) ( hash % (uint32_t) lanes );
}

// The lane a send operation belongs to: the operations are carved lane by
// lane, send_queue_size each, so operation i is lane i / send_queue_size's.
// A completion releases to it (create seeds each lane with the same carve).
// proofs/tla/RNIocpSendLanes.tla OpLane; OwnLane holds only while release
// reads the carve this way (MCRNIocpSendLanesBrokenReleaseMod, and the trace
// check's plant below).
static int rocketnet_iocp_op_lane( uint32_t index, int send_queue_size, int lanes )
{
#if defined( ROCKETNET_IOCP_SEND_LANES_PLANT )
    // the trace check's plant (tests/tla_trace_iocp_lanes.cpp): the carve read the
    // wrong way, index % lanes. Its trace must fail.
    (void) send_queue_size;
    return (int) ( index % (uint32_t) lanes );
#else  // #if defined( ROCKETNET_IOCP_SEND_LANES_PLANT )
    (void) lanes;
    return (int) ( index / (uint32_t) send_queue_size );
#endif // #if defined( ROCKETNET_IOCP_SEND_LANES_PLANT )
}

// The lanes send thread t owns, in order: t, t + threads, ... below lanes
// (lane l belongs to thread l % threads, rocketnet_server_iocp_send_thread_of_lane).
// Writes them to owned, room for ROCKETNET_IOCP_MAX_LANES, and returns how many.
// The one walk the send threads take at create; proofs/tla/RNIocpSendLanes.tla
// ThreadLanes, OnlyOwnerPosts and OneOwner hold only while it reads the
// ownership this way (MCRNIocpSendLanesBrokenOwnerShift, and the trace check's
// plant below).
static int rocketnet_iocp_send_thread_lanes( int thread, int threads, int lanes, int * owned )
{
    int count = 0;
#if defined( ROCKETNET_IOCP_SEND_THREAD_PLANT )
    // the trace check's plant (tests/tla_trace_iocp_lanes.cpp): a thread one
    // lane past its own. Its trace must fail.
    for ( int l = thread + 1; l < lanes; l += threads )
#else  // #if defined( ROCKETNET_IOCP_SEND_THREAD_PLANT )
    for ( int l = thread; l < lanes; l += threads )
#endif // #if defined( ROCKETNET_IOCP_SEND_THREAD_PLANT )
    {
        owned[count++] = l;
    }
    return count;
}

#if ROCKETNET_DEV
// The admission as the unit suite and the trace harness drive it, with no
// socket: packet_filter NULL is the filter not yet published.
int rocketnet_server_iocp_test_receive_admit( const uint8_t * packet_filter, const uint8_t * packet_data, int packet_bytes,
                                              int num_packets, int capacity )
{
    const uint64_t filter_state = packet_filter ? rocketnet_iocp_filter_state( packet_filter ) : 0;
    return (int) rocketnet_iocp_receive_admit( filter_state, packet_data, packet_bytes, num_packets, capacity );
}

int rocketnet_server_iocp_test_lane_index( const rocketnet_address_t * to, int lanes )
{
    return rocketnet_iocp_lane_index( to, lanes );
}

// the lanes a send thread owns, as the transport's send threads take them at
// create (the unit suite and the trace harness)
int rocketnet_server_iocp_test_send_thread_lanes( int thread, int threads, int lanes, int * owned )
{
    return rocketnet_iocp_send_thread_lanes( thread, threads, lanes, owned );
}

// The send lanes' free queues with no socket and no thread, for the trace
// harness (layer iocp-lanes): seeded as create seeds them, an acquire from
// the destination's lane as start_packet takes one, a release to the
// operation's lane as rocketnet_iocp_release_send gives it back. Each lane's
// queue is the code's own MPMC ref queue.
struct rocketnet_server_iocp_test_lanes_t
{
    int lanes;
    int send_queue_size;
    rocketnet_iocp_ref_cell_t * cells;
    rocketnet_iocp_ref_queue_t queues[ROCKETNET_IOCP_MAX_LANES];
};

rocketnet_server_iocp_test_lanes_t * rocketnet_server_iocp_test_lanes_create( int lanes, int send_queue_size )
{
    rocketnet_assert( lanes >= 1 && lanes <= ROCKETNET_IOCP_MAX_LANES );
    rocketnet_assert( send_queue_size > 0 && ( send_queue_size & ( send_queue_size - 1 ) ) == 0 );
    rocketnet_server_iocp_test_lanes_t * test = new rocketnet_server_iocp_test_lanes_t();
    test->lanes = lanes;
    test->send_queue_size = send_queue_size;
    test->cells = new rocketnet_iocp_ref_cell_t[(size_t) lanes * (size_t) send_queue_size];
    for ( int l = 0; l < lanes; ++l )
    {
        rocketnet_iocp_queue_initialize( &test->queues[l], test->cells + (size_t) l * (size_t) send_queue_size, (uint64_t) send_queue_size );
    }
    for ( int i = 0; i < lanes * send_queue_size; ++i )
    {
        // seeded as create seeds them: each lane its own carve
        const rocketnet_iocp_ref_t ref = { 1, 1, (uint32_t) i };
        if ( !rocketnet_iocp_queue_push( &test->queues[i / send_queue_size], ref ) )
        {
            rocketnet_server_iocp_test_lanes_destroy( test );
            return NULL;
        }
    }
    return test;
}

void rocketnet_server_iocp_test_lanes_destroy( rocketnet_server_iocp_test_lanes_t * test )
{
    delete[] test->cells;
    delete test;
}

int rocketnet_server_iocp_test_lanes_acquire( rocketnet_server_iocp_test_lanes_t * test, const rocketnet_address_t * to, int * lane_out )
{
    const int lane = rocketnet_iocp_lane_index( to, test->lanes );
    *lane_out = lane;
    rocketnet_iocp_ref_t ref;
    return rocketnet_iocp_queue_pop( &test->queues[lane], &ref ) ? (int) ref.index : -1;
}

// false when the lane the release names is full: only a release to the
// wrong lane can find it so
bool rocketnet_server_iocp_test_lanes_release( rocketnet_server_iocp_test_lanes_t * test, int index )
{
    const rocketnet_iocp_ref_t ref = { 1, 1, (uint32_t) index };
    return rocketnet_iocp_queue_push( &test->queues[rocketnet_iocp_op_lane( (uint32_t) index, test->send_queue_size, test->lanes )], ref );
}

// the lane's free queue in pop order
int rocketnet_server_iocp_test_lanes_free( const rocketnet_server_iocp_test_lanes_t * test, int lane, int * indexes, int max_indexes )
{
    const rocketnet_iocp_ref_queue_t * queue = &test->queues[lane];
    const uint64_t first = queue->dequeue.load( std::memory_order_relaxed );
    const uint64_t last = queue->enqueue.load( std::memory_order_relaxed );
    int count = 0;
    for ( uint64_t position = first; position != last && count < max_indexes; ++position )
    {
        indexes[count++] = (int) queue->cells[position & ( queue->capacity - 1 )].ref.index;
    }
    return count;
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
// A pop stalls between its CAS and its sequence store and outlasts refusals
// refused pushes of the release that needs its cell (one or more). Reports
// the release, the operation's state, send_idle once both operations are
// back, and how many times the retried release gave up the processor.
bool rocketnet_server_iocp_test_release_send_retries_stalled_pop( int refusals, bool * released_out, int * state_out, bool * send_idle_out,
                                                                  uint64_t * yields_out )
{
    const uint64_t capacity = 4;
    alignas( rocketnet_iocp_ref_cell_t ) uint8_t storage[capacity * sizeof( rocketnet_iocp_ref_cell_t )];
    rocketnet_iocp_ref_queue_t free_sends;
    rocketnet_iocp_queue_initialize( &free_sends, storage, capacity );

    int states[capacity];
    for ( uint32_t i = 0; i < capacity; ++i )
    {
        states[i] = ROCKETNET_IOCP_OPERATION_FREE;
        const rocketnet_iocp_ref_t init_ref = { 1, 0, i };
        if ( !rocketnet_iocp_queue_push( &free_sends, init_ref ) )
        {
            return false;
        }
    }

    free_sends.stall_next_pop = true;

    rocketnet_iocp_ref_t ref0 = {};
    if ( !rocketnet_iocp_queue_pop( &free_sends, &ref0 ) )
    {
        return false;
    }
    states[ref0.index] = ROCKETNET_IOCP_OPERATION_STAGED;
    ref0.generation = 1;

    rocketnet_iocp_ref_t ref1 = {};
    if ( !rocketnet_iocp_queue_pop( &free_sends, &ref1 ) )
    {
        return false;
    }
    states[ref1.index] = ROCKETNET_IOCP_OPERATION_STAGED;
    ref1.generation = 1;

    free_sends.stalled_pop_holds = refusals - 1;
    const bool released1 = rocketnet_iocp_queue_release_send( &free_sends, ref1, &states[ref1.index] );
    const bool released0 = rocketnet_iocp_queue_release_send( &free_sends, ref0, &states[ref0.index] );
    if ( !released0 )
    {
        return false;
    }

    *released_out = released1;
    *state_out = states[ref1.index];
    *send_idle_out = rocketnet_iocp_queue_send_idle( &free_sends, 0, (int) capacity );
    *yields_out = free_sends.yields.load( std::memory_order_relaxed );
    return true;
}

// The retry queue at its boundary, capacity 4 with all four receive
// operations on it at once. One worker's pop stalls between its CAS and its
// sequence store; a second worker takes the next operation, is refused the
// post again and pushes it back, onto the cell the stalled pop still holds.
// Reports that every push at the boundary was accepted, the state of the
// operation pushed against the stalled pop, and the order the four leave in.
bool rocketnet_server_iocp_test_retry_receive_keeps_operation( bool * boundary_accepted_out, int * state_out, uint32_t * order_out )
{
    const uint64_t capacity = 4;
    alignas( rocketnet_iocp_ref_cell_t ) uint8_t storage[capacity * sizeof( rocketnet_iocp_ref_cell_t )];
    rocketnet_iocp_ref_queue_t retry_receives;
    rocketnet_iocp_queue_initialize( &retry_receives, storage, capacity );

    int states[capacity];
    bool accepted = true;
    for ( uint32_t i = 0; i < capacity; ++i )
    {
        const rocketnet_iocp_ref_t ref = { 1, 1, i };
        states[i] = ROCKETNET_IOCP_OPERATION_RECEIVE_POSTED;
        rocketnet_iocp_queue_retry_receive( &retry_receives, ref, &states[i] );
        accepted = accepted && states[i] == ROCKETNET_IOCP_OPERATION_RETRY;
    }
    *boundary_accepted_out = accepted && retry_receives.enqueue.load( std::memory_order_relaxed ) == capacity;

    retry_receives.stall_next_pop = true;

    rocketnet_iocp_ref_t stalled = {};
    rocketnet_iocp_ref_t taken = {};
    if ( !rocketnet_iocp_queue_pop( &retry_receives, &stalled ) || !rocketnet_iocp_queue_pop( &retry_receives, &taken ) )
    {
        return false;
    }

    rocketnet_iocp_queue_retry_receive( &retry_receives, taken, &states[taken.index] );
    *state_out = states[taken.index];
    rocketnet_iocp_queue_retry_receive( &retry_receives, stalled, &states[stalled.index] );

    for ( uint32_t i = 0; i < capacity; ++i )
    {
        rocketnet_iocp_ref_t ref = {};
        order_out[i] = rocketnet_iocp_queue_pop( &retry_receives, &ref ) ? ref.index : UINT32_MAX;
    }

    return true;
}
#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS || ROCKETNET_DEV

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#include "rocketnet_coalesce.h"
#include "rocketnet_send_gather.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>

// Pop only an enqueue position that existed before stop was captured. A
// failed operation may enqueue a new reference concurrently, but its position
// is at or beyond stop and therefore cannot be consumed again in this pass.
static bool rocketnet_iocp_queue_pop_before( rocketnet_iocp_ref_queue_t * queue,
                                             uint64_t stop, rocketnet_iocp_ref_t * ref )
{
    // proofs/tla/RNIocpRetryStop.tla PassClaim is the stop test, the
    // published test and the CAS; PassTake reads the reference and frees
    // the cell. PassBounded: a pass never claims at or beyond its stop.
    uint64_t position = queue->dequeue.load( std::memory_order_relaxed );
    rocketnet_iocp_ref_cell_t * cell;

    for ( ;; )
    {
        if ( (int64_t) ( stop - position ) <= 0 )
        {
            return false;
        }
#if ROCKETNET_DEV
        queue->probes.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
        cell = &queue->cells[position & ( queue->capacity - 1 )];

        const uint64_t sequence = cell->sequence.load( std::memory_order_acquire );
        const int64_t difference = (int64_t) ( sequence - ( position + 1 ) );
        if ( difference == 0 )
        {
            if ( queue->dequeue.compare_exchange_weak( position, position + 1, std::memory_order_relaxed ) )
            {
                break;
            }
        }
        else if ( difference < 0 )
        {
            return false;
        }
        else
        {
            position = queue->dequeue.load( std::memory_order_relaxed );
        }
    }

    *ref = cell->ref;
    cell->sequence.store( position + queue->capacity, std::memory_order_release );
#if ROCKETNET_DEV
    queue->pops.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    return true;
}

static size_t rocketnet_iocp_queue_count( rocketnet_iocp_ref_queue_t * queue )
{
    const uint64_t enqueued = queue->enqueue.load( std::memory_order_acquire );
    const uint64_t dequeued = queue->dequeue.load( std::memory_order_acquire );
    const uint64_t count = enqueued - dequeued;
    return (size_t) ( count < queue->capacity ? count : queue->capacity );
}

// The 16-aligned members lead; an operation is a fixed record identified by
// index, owner epoch and generation rather than by an intrusive list node.
struct rocketnet_iocp_operation_t
{
    OVERLAPPED overlapped;
    alignas( 16 ) char control[64];
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE];
    WSABUF buffer;
    sockaddr_storage address;
    int address_bytes;
    DWORD flags;
    int index;
    bool send;
    int packet_bytes;
    uint64_t owner_epoch;
    uint64_t generation;
    rocketnet_iocp_ref_t completion_ref;
    int state;
    // A send post carries run_count frames and run_bytes bytes: one frame on
    // the per-datagram path, a USO run otherwise. A USO root names a flat
    // generation-tagged run sequence by ticket. Every member stays owned
    // until that one completion retires the exact recorded references.
    rocketnet_address_t to;
    int run_count;
    int run_bytes;
    uint64_t run_ticket;
    // WSASendMsg for a USO run, WSARecvMsg for a URO receive. uro_data is the
    // receive's coalesced buffer, NULL on the per-datagram receive path.
    WSAMSG message;
    uint8_t * uro_data;
};

static_assert( offsetof( rocketnet_iocp_operation_t, overlapped ) == 0, "completion identifies its operation" );
static_assert( alignof( rocketnet_iocp_operation_t ) % MEMORY_ALLOCATION_ALIGNMENT == 0 && alignof( rocketnet_iocp_operation_t ) >= 16, "an operation keeps its allocation alignment" );
static_assert( offsetof( rocketnet_iocp_operation_t, control ) % 16 == 0, "control keeps native ancillary-data alignment" );
static_assert( offsetof( rocketnet_iocp_operation_t, frame ) == offsetof( rocketnet_iocp_operation_t, control ) + 64, "frame follows control with no implicit padding" );
static_assert( ROCKETNET_RECEIVE_CEILING + 1 <= ROCKETNET_FRAME_STRIDE, "receive detects oversize datagrams" );

// a receive bank's arrays hold capacity packets
// (rocketnet_server_iocp_receive_queue_size for the configured max_clients),
// carved from the transport's sized allocation; data is 16-aligned
struct rocketnet_iocp_receive_buffer_t
{
    uint8_t ( *data )[ROCKETNET_FRAME_STRIDE];
    rocketnet_address_t * from;
    int * packet_bytes;
    int num_packets;
    int capacity;
};

// the bytes of a worker's members, which pack with no padding between them.
// Its receive banks are the three buffers of the receive handoff
// (sdk/rocketnet_receive_handoff.h): the worker's, main's and its tail, or
// the free one while main reads the worker's own.
#define ROCKETNET_IOCP_RECEIVE_BANKS ROCKETNET_RECEIVE_HANDOFF_BUFFERS
#define ROCKETNET_IOCP_WORKER_MEMBER_BYTES ( ROCKETNET_IOCP_RECEIVE_BANKS * sizeof( rocketnet_iocp_receive_buffer_t ) + sizeof( rocketnet_server_iocp_t * ) + \
                                             sizeof( rocketnet_platform_thread_t * ) + sizeof( rocketnet_platform_mutex_t ) +                       \
                                             sizeof( struct rocketnet_receive_handoff_t ) )

static_assert( ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64 != 0, "a worker's tail padding is a nonzero array" );

struct alignas( 64 ) rocketnet_iocp_worker_t
{
    rocketnet_iocp_receive_buffer_t buffers[ROCKETNET_IOCP_RECEIVE_BANKS];
    rocketnet_server_iocp_t * owner;
    rocketnet_platform_thread_t * thread;
    // Only the handoff's roles and committed counts move under this mutex.
    // Completion handling, packet copies, send retirement and reposts run
    // outside it; main only ever tries it.
    rocketnet_platform_mutex_t mutex;
    // The receive handoff over buffers[]: the worker appends to
    // buffers[receive_handoff.worker] and commits its count; main reads the
    // buffer the worker left, then the worker's own up to the committed
    // count. The cursors and limits are main's alone.
    struct rocketnet_receive_handoff_t receive_handoff;
    // the rest of the last cache line, explicit so the compiler adds no tail padding
    uint8_t padding[64 - ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64];
};

static_assert( sizeof( rocketnet_iocp_worker_t ) == ROCKETNET_IOCP_WORKER_MEMBER_BYTES + 64 - ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64 &&
                   sizeof( rocketnet_iocp_worker_t ) % 64 == 0,
               "a worker has no implicit padding" );

// These compact command banks contain only operation indices. The cold frame
// pool remains completion-owned until Windows retires each overlapped send.
// Main owns one bank, TX owns at most one, and ready banks are FIFO.
#define ROCKETNET_IOCP_SEND_BANKS 3
struct rocketnet_iocp_send_bank_t
{
    int count = 0;
    rocketnet_iocp_ref_t * staged = NULL;       // send_queue_size entries of the sized allocation
};

struct rocketnet_iocp_run_slot_t
{
    rocketnet_iocp_ref_t ref = {};
    uint64_t owner_epoch = 0;
    uint64_t ticket = 0;
    bool occupied = false;
};

// A send lane: its own staging banks, operation pool and run ring, posted by
// the send thread that owns it.
// Operations [base, base + send_queue_size) of the transport's sends are the
// lane's, so an operation's index names its lane.
struct rocketnet_iocp_lane_t
{
    rocketnet_server_iocp_t * owner = NULL;
    rocketnet_iocp_ref_queue_t free_sends;      // send_queue_size cells
    // The lane's send thread is its only run producer. Completion workers retire
    // runs. Tickets select fixed physical slots; the mutex publishes the whole
    // run and prevents an out-of-order completion from aliasing a reused slot.
    rocketnet_platform_mutex_t run_mutex = {};
    rocketnet_iocp_run_slot_t * run_refs = NULL;        // send_queue_size slots
    uint64_t next_run_ticket = 1;
    std::atomic<uint64_t> num_staged{ 0 };
    rocketnet_iocp_send_bank_t send_banks[ROCKETNET_IOCP_SEND_BANKS];
    rocketnet_platform_mutex_t send_mutex = {};
    rocketnet_platform_wake_t * send_wake = NULL; // its send thread's wake (rocketnet_iocp_send_thread_t), not owned
    int send_producer = 0; // caller-owned until stop publishes stopping
    // proofs/tla/RNBankFifoLayout.tla Init: as a stack this literal holds banks 1 and 2, right for three banks only.
    int send_free[ROCKETNET_IOCP_SEND_BANKS] = { 1, 2, 0 };
    int send_free_count = ROCKETNET_IOCP_SEND_BANKS - 1;
    int send_ready[ROCKETNET_IOCP_SEND_BANKS] = {};
    int send_ready_head = 0;
    int send_ready_count = 0;
    // The send thread's view of a bank, slot by slot, for run grouping: bytes
    // (zero for an aborted or empty slot) and destination.
    int * run_bytes = NULL;                     // send_queue_size entries
    rocketnet_address_t * run_to = NULL;
    int * order = NULL;                         // send_queue_size entries
    rocketnet_send_gather_run_t * runs = NULL;  // send_queue_size entries
    rocketnet_send_gather_scratch_t gather = {}; // over send_queue_size slots, in the sized allocation
};

// A send thread: it alone takes the ready banks of its lanes (lane l belongs
// to thread l % num_send_threads), groups and posts them, and owns the wake
// those lanes' flushes set.
struct rocketnet_iocp_send_thread_t
{
    rocketnet_server_iocp_t * owner = NULL;
    int index = 0;
    rocketnet_platform_wake_t * wake = NULL;
    rocketnet_platform_thread_t * thread = NULL;
    // the lanes it owns, in order (rocketnet_iocp_send_thread_lanes), set at create
    int lanes[ROCKETNET_IOCP_MAX_LANES] = {};
    int num_lanes = 0;
};

struct rocketnet_server_iocp_t
{
    rocketnet_iocp_ref_queue_t retry_receives;  // a power of two at least every receive operation
    // one lane's staging (rocketnet_server_iocp_send_queue_size), the lanes
    // (rocketnet_server_iocp_send_lanes) and each receive bank
    // (rocketnet_server_iocp_receive_queue_size) for the configured
    // max_clients, and the one allocation every client-scaled array below is
    // carved from (rocketnet_iocp_sized_layout)
    int send_queue_size = 0;
    int num_lanes = 0;
    int num_sends = 0;                          // num_lanes * send_queue_size
    int receive_queue_size = 0;
    void * sized_storage = NULL;
    uint64_t owner_epoch = 0;
    rocketnet_iocp_lane_t * lanes[ROCKETNET_IOCP_MAX_LANES] = {}; // each its own allocation
    // The send threads: ROCKETNET_IOCP_SEND_THREADS, or the lanes when there
    // are fewer. Each is pinned to a core of its own after the completion
    // workers' (rocketnet_iocp_send_processors); what each found when it
    // pinned itself is rocketnet_server_iocp_send_placement, and every
    // refusal is counted in send_pin_refusals.
    int num_send_threads = 0;
    rocketnet_iocp_send_thread_t send_threads[ROCKETNET_IOCP_MAX_SEND_THREADS];
    GROUP_AFFINITY send_pins[ROCKETNET_IOCP_MAX_SEND_THREADS] = {};
    std::atomic<int> send_placements[ROCKETNET_IOCP_MAX_SEND_THREADS] = {};
    std::atomic<unsigned long> send_placement_errors[ROCKETNET_IOCP_MAX_SEND_THREADS] = {};
    std::atomic<uint64_t> send_pin_refusals{ 0 };
    // the lanes whose send thread has not finished them; zero before any is created
    std::atomic<int> live_lanes{ 0 };
    void * context;
    // The endpoints: one socket and completion port with one worker. One
    // worker is one endpoint bound exclusively, as before. Several workers
    // are as many endpoints, each socket given its worker's processor with
    // SIO_CPU_AFFINITY before bind and all bound to the same address and
    // port, so RSS delivers each flow to the endpoint on its processor and a
    // client's datagrams stay with one worker, in order. Receive operation i
    // is endpoint i / ROCKETNET_IOCP_RECEIVES_PER_WORKER's; a send lane posts
    // on endpoint lane % num_endpoints.
    int num_endpoints = 0;
    SOCKET sockets[ROCKETNET_IOCP_MAX_WORKERS];
    HANDLE ports[ROCKETNET_IOCP_MAX_WORKERS] = {};
    int processors[ROCKETNET_IOCP_MAX_WORKERS] = {};
    GROUP_AFFINITY pins[ROCKETNET_IOCP_MAX_WORKERS] = {};
    GROUP_AFFINITY cores[ROCKETNET_IOCP_MAX_WORKERS] = {}; // every logical processor of the pin's core
    // what each worker found when it pinned itself
    // (rocketnet_server_iocp_worker_placement), and a refusal's Win32 error
    std::atomic<int> placements[ROCKETNET_IOCP_MAX_WORKERS] = {};
    std::atomic<unsigned long> placement_errors[ROCKETNET_IOCP_MAX_WORKERS] = {};
    std::atomic<uint64_t> receive_pin_refusals{ 0 }; // completion worker pins Windows refused
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    uint64_t socket_generations[ROCKETNET_IOCP_MAX_WORKERS] = {};
    uint64_t port_generations[ROCKETNET_IOCP_MAX_WORKERS] = {};
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    bool ipv6;
    int num_workers;
    rocketnet_iocp_worker_t * workers;
    rocketnet_iocp_operation_t * sends;
    rocketnet_iocp_operation_t * receives;
    std::atomic<bool> stopping{ false };
    std::atomic<int> pending{ 0 };
    std::atomic<int> active_workers{ 0 };
    std::atomic<uint64_t> counters[ROCKETNET_SERVER_IOCP_COUNTER_ROWS]{};
    // the worker prefilter (rocketnet_iocp_filter_state), 0 until published
    std::atomic<uint64_t> filter_state{ 0 };
    // one drop tally per completion worker, each written by its worker only
    rocketnet_receive_drop_tally_t * drop_tallies = NULL;
    rocketnet_server_socket_process_packets_t process;
    // main's: the worker its next receive call starts with, so no worker's
    // datagrams always wait behind the others' when the view fills; and
    // whether what the workers banked that main never returned is counted
    // (rocketnet_iocp_count_stopped, once)
    int receive_start = 0;
    bool stopped_counted = false;
    std::atomic<int> features{ 0 };
    LPFN_WSARECVMSG receive_message = NULL;
    uint8_t * uro_buffers = NULL;
#if ROCKETNET_DEV
    std::atomic<int> test_faults[ROCKETNET_IOCP_TEST_NUM_FAULTS]{};
    std::atomic<int> test_fault_status[ROCKETNET_IOCP_TEST_NUM_FAULTS]{};
    std::atomic<bool> test_hold_tx{ false };
    std::atomic<int> test_active_tx_batches{ 0 };
    std::atomic<int> test_pending_tx_banks{ 0 };
    std::atomic<bool> test_hold_completions{ false };
    bool test_refuse_worker_pins = false; // ROCKETNET_IOCP_REFUSE_WORKER_PINS, set at create
    std::atomic<int> test_active_completion_batches{ 0 };
#endif // #if ROCKETNET_DEV
};

#if ROCKETNET_DEV
// Unit seams only, set by rocketnet_server_iocp_test_stop_post_race on the
// test's own thread and cleared before it returns (file statics, so the
// transport's footprint does not move): a receive post runs the hook once
// admitted, before the kernel post, and a post is accepted without a socket.
static void ( *rocketnet_iocp_test_post_admitted )( rocketnet_server_iocp_t * iocp, void * context ) = NULL;
static void * rocketnet_iocp_test_post_admitted_context = NULL;
static bool rocketnet_iocp_test_accept_posts = false;
#endif // #if ROCKETNET_DEV

static_assert( ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 * ROCKETNET_MAX_CLIENTS ) <= UINT16_MAX + 1,
               "packet handles reserve sixteen bits for the operation index: every index of the largest send queue fits them" );

static std::atomic<uint64_t> rocketnet_iocp_next_owner_epoch{ 1 };

// proofs/tla/RNIocpEndpoints.tla PostOn and Owner: the endpoint an operation
// is posted on, whoever posts it: a receive's by its index, a send's
// by its lane (operations [lane * send_queue_size, ...) are the lane's)
static int rocketnet_iocp_op_endpoint( const rocketnet_server_iocp_t * iocp, const rocketnet_iocp_operation_t * op )
{
    if ( iocp->num_endpoints <= 1 )
    {
        return 0;
    }
    if ( op->send )
    {
        return ( op->index / iocp->send_queue_size ) % iocp->num_endpoints;
    }
    return op->index / ROCKETNET_IOCP_RECEIVES_PER_WORKER;
}

static SOCKET rocketnet_iocp_op_socket( const rocketnet_server_iocp_t * iocp, const rocketnet_iocp_operation_t * op )
{
    return iocp->sockets[rocketnet_iocp_op_endpoint( iocp, op )];
}

// The fault seam (rocketnet_server_iocp_test_fail_next) exists only in the
// test executable. Elsewhere the check is the constant false, so the
// production paths carry no test branch.
#if ROCKETNET_DEV
static bool rocketnet_iocp_test_fault( rocketnet_server_iocp_t * iocp, int fault, int * status )
{
    int left = iocp->test_faults[fault].load( std::memory_order_acquire );
    while ( left > 0 )
    {
        if ( iocp->test_faults[fault].compare_exchange_weak( left, left - 1, std::memory_order_acq_rel ) )
        {
            *status = iocp->test_fault_status[fault].load( std::memory_order_relaxed );
            return true;
        }
    }

    return false;
}
#define ROCKETNET_IOCP_TEST_FAULT( iocp, fault, status ) rocketnet_iocp_test_fault( iocp, fault, status )
#else
#define ROCKETNET_IOCP_TEST_FAULT( iocp, fault, status ) false
#endif // #if ROCKETNET_DEV

static rocketnet_iocp_ref_t rocketnet_iocp_operation_ref( const rocketnet_iocp_operation_t * op )
{
    rocketnet_iocp_ref_t ref;
    ref.owner_epoch = op->owner_epoch;
    ref.generation = op->generation;
    ref.index = (uint32_t) op->index;
    return ref;
}

static rocketnet_iocp_operation_t * rocketnet_iocp_send_from_ref( rocketnet_server_iocp_t * iocp,
                                                                  const rocketnet_iocp_ref_t & ref,
                                                                  int state )
{
    if ( ref.owner_epoch != iocp->owner_epoch || ref.index >= (uint32_t) iocp->num_sends )
    {
        return NULL;
    }
    rocketnet_iocp_operation_t * op = &iocp->sends[ref.index];
    return op->owner_epoch == ref.owner_epoch && op->generation == ref.generation && op->state == state ? op : NULL;
}

// the lane a send operation belongs to (rocketnet_iocp_op_lane)
static rocketnet_iocp_lane_t * rocketnet_iocp_lane_of_index( rocketnet_server_iocp_t * iocp, uint32_t index )
{
    rocketnet_assert( index < (uint32_t) iocp->num_sends );
    return iocp->lanes[rocketnet_iocp_op_lane( index, iocp->send_queue_size, iocp->num_lanes )];
}

static rocketnet_iocp_operation_t * rocketnet_iocp_receive_from_ref( rocketnet_server_iocp_t * iocp,
                                                                     const rocketnet_iocp_ref_t & ref,
                                                                     int state )
{
    const uint32_t capacity = (uint32_t) ( iocp->num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    if ( ref.owner_epoch != iocp->owner_epoch || ref.index >= capacity )
    {
        return NULL;
    }
    rocketnet_iocp_operation_t * op = &iocp->receives[ref.index];
    return op->owner_epoch == ref.owner_epoch && op->generation == ref.generation && op->state == state ? op : NULL;
}

static bool rocketnet_iocp_release_send( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t & ref, int state )
{
    // proofs/tla/RNIocpSendLease.tla ReleaseStaged and Retire: FREE, then the
    // reference at its unchanged generation goes back on free_sends.
    // proofs/tla/RNIocpSendLanes.tla Complete: the free_sends of the lane the
    // index names (rocketnet_iocp_lane_of_index).
    // proofs/tla/RNIocpSendLease.tla ReleasePush claims a position (a pop
    // stalled before its sequence store refuses it, and the push retries,
    // never retires: RetiredOnlyAtGenMax) and ReleasePublish writes the cell.
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, state );
    if ( !op )
    {
        return false;
    }
    return rocketnet_iocp_queue_release_send( &rocketnet_iocp_lane_of_index( iocp, ref.index )->free_sends, ref, &op->state );
}

static bool rocketnet_iocp_acquire_send( rocketnet_server_iocp_t * iocp, rocketnet_iocp_lane_t * lane, rocketnet_iocp_ref_t * ref )
{
    // proofs/tla/RNIocpSendLease.tla AcquireClaim is the pop's CAS and read,
    // Acquire its sequence store and one pass of this loop; its
    // StaleNeverNamesLease is why the GenMax refusal below must stay.
    rocketnet_iocp_ref_t free_ref;
    while ( rocketnet_iocp_queue_pop( &lane->free_sends, &free_ref ) )
    {
        rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, free_ref, ROCKETNET_IOCP_OPERATION_FREE );
        if ( !op )
        {
            continue;
        }
        // Sixteen packet-id bits hold the index; the rest hold this lease's
        // generation. Refuse the physically unreachable wrap rather than let
        // an ancient packet id name a new lease.

        if ( op->generation == ( UINT64_MAX >> 16 ) )
        {
            op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
            continue;
        }

        ++op->generation;
        op->state = ROCKETNET_IOCP_OPERATION_STAGED;
        *ref = rocketnet_iocp_operation_ref( op );
        return true;
    }

    return false;
}

static uint64_t rocketnet_iocp_packet_id( const rocketnet_iocp_ref_t & ref )
{
    return ( ref.generation << 16 ) | ref.index;
}

static bool rocketnet_iocp_packet_ref( rocketnet_server_iocp_t * iocp, uint64_t packet_id,
                                       rocketnet_iocp_ref_t * ref )
{
    ref->owner_epoch = iocp->owner_epoch;
    ref->generation = packet_id >> 16;
    ref->index = (uint32_t) ( packet_id & UINT16_MAX );
    return ref->generation != 0 && ref->index < (uint32_t) iocp->num_sends;
}

static bool rocketnet_iocp_allocate_run( rocketnet_server_iocp_t * iocp, rocketnet_iocp_lane_t * lane, const rocketnet_iocp_ref_t * refs,
                                         int count, uint64_t * first_ticket )
{
    // proofs/tla/RNIocpRunRing.tla Allocate: the overflow guard and the check
    // of all count slots (NoOverwrite) before any slot is written.
    rocketnet_assert( count > 1 && count <= ROCKETNET_IOCP_USO_MAX_SEGMENTS );

    rocketnet_platform_mutex_acquire( &lane->run_mutex );

    const uint64_t first = lane->next_run_ticket;
    bool available = first != 0 && first <= UINT64_MAX - (uint64_t) count;
    for ( int i = 0; available && i < count; ++i )
    {
        const size_t index = (size_t) ( ( first + (uint64_t) i - 1 ) % (uint64_t) iocp->send_queue_size );
        available = !lane->run_refs[index].occupied;
    }

    if ( available )
    {
        for ( int i = 0; i < count; ++i )
        {
            const uint64_t ticket = first + (uint64_t) i;
            rocketnet_iocp_run_slot_t * slot = &lane->run_refs[( ticket - 1 ) % (uint64_t) iocp->send_queue_size];
            slot->ref = refs[i];
            slot->owner_epoch = iocp->owner_epoch;
            slot->ticket = ticket;
            slot->occupied = true;
        }

        lane->next_run_ticket = first + (uint64_t) count;
        *first_ticket = first;
    }

    rocketnet_platform_mutex_release( &lane->run_mutex );

    return available;
}

static bool rocketnet_iocp_take_run( rocketnet_server_iocp_t * iocp, rocketnet_iocp_lane_t * lane, uint64_t first, int count,
                                     rocketnet_iocp_ref_t * refs )
{
    if ( first == 0 || count < 1 || count > ROCKETNET_IOCP_USO_MAX_SEGMENTS )
    {
        return false;
    }

    // proofs/tla/RNIocpRunRing.tla Take: a stale or repeated (first, count) is
    // refused by the ticket compare (TakeOnlyLive).
    rocketnet_platform_mutex_acquire( &lane->run_mutex );

    bool valid = true;
    for ( int i = 0; valid && i < count; ++i )
    {
        const uint64_t ticket = first + (uint64_t) i;
        const rocketnet_iocp_run_slot_t * slot = &lane->run_refs[( ticket - 1 ) % (uint64_t) iocp->send_queue_size];
        valid = slot->occupied && slot->owner_epoch == iocp->owner_epoch && slot->ticket == ticket;
    }

    if ( valid )
    {
        for ( int i = 0; i < count; ++i )
        {
            const uint64_t ticket = first + (uint64_t) i;
            rocketnet_iocp_run_slot_t * slot = &lane->run_refs[( ticket - 1 ) % (uint64_t) iocp->send_queue_size];
            refs[i] = slot->ref;
            slot->occupied = false;
        }
    }

    rocketnet_platform_mutex_release( &lane->run_mutex );

    return valid;
}

static bool rocketnet_iocp_retire_send( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t & root )
{
    // proofs/tla/RNIocpSendLease.tla Retire, and RNIocpRunRing.tla Take for a run.
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, root, ROCKETNET_IOCP_OPERATION_SEND_POSTED );
    if ( !op )
    {
        return false;
    }
    const int run_count = op->run_count;
    const uint64_t run_ticket = op->run_ticket;
    if ( run_count <= 1 )
    {
        return rocketnet_iocp_release_send( iocp, root, ROCKETNET_IOCP_OPERATION_SEND_POSTED );
    }

    rocketnet_iocp_ref_t refs[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
    if ( !rocketnet_iocp_take_run( iocp, rocketnet_iocp_lane_of_index( iocp, root.index ), run_ticket, run_count, refs ) )
    {
        return false;
    }
    bool retired = true;

    // Releasing refs[0] publishes the root back to free_sends, so no member
    // of op may be read after the first iteration: another caller may acquire
    // and restage it immediately.

    for ( int i = 0; i < run_count; ++i )
    {
        retired &= rocketnet_iocp_release_send( iocp, refs[i], ROCKETNET_IOCP_OPERATION_SEND_POSTED );
    }
    return retired;
}

// a source from its sockaddr; an IPv4-mapped IPv6 source passes the one
// receive rule, rocketnet_address_accept_received_source: canonicalised to
// IPv4 on the dual-stack socket, refused on any other
static bool rocketnet_iocp_address_read( const sockaddr_storage * storage, int bytes, bool dual_stack, rocketnet_address_t * address )
{
    memset( address, 0, sizeof( *address ) );

    if ( ( storage->ss_family == AF_INET && bytes < (int) sizeof( sockaddr_in ) ) ||
         ( storage->ss_family == AF_INET6 && bytes < (int) sizeof( sockaddr_in6 ) ) )
    {
        return false;
    }

    return rocketnet_platform_sockaddr_to_address( storage, dual_stack, address );
}

// post_error, when given, takes the Winsock error of a refused post (create
// maps it to its status)
static bool rocketnet_iocp_post_receive( rocketnet_server_iocp_t * iocp, rocketnet_iocp_operation_t * op, int * post_error = NULL )
{
    // proofs/tla/RNIocpRetryStop.tla PostCount is the pending increment,
    // PostStart the stop check and the new generation, PostAccepted an
    // accepted post and PostRefused a refused one, whose push onto
    // retry_receives is PushClaim and PushPublish.
    // proofs/tla/RNIocpEndpoints.tla PostCount, PostCheck, PostAccept and
    // PostRefuse are the same steps, any worker posting any endpoint's receive.
    // Only a completion worker or the creating thread posts a receive; no
    // publication mutex is held. Any worker may post a receive another
    // endpoint owns (from retry_receives), and only the owner cancels it.
    // The post is counted pending before stop is checked, both seq_cst, as
    // stop's store and the stop pass's loads are: if this check reads
    // stopping false, the increment precedes stop's store in the single total
    // order, so every stop pass sees it pending and the owner stays, reissuing
    // cancellation each pass until the post's completion is drained. A post
    // that reads stopping true, or finds its generation used up, takes its
    // count back and retires. Stack addresses/flags must not back pending I/O.
    iocp->pending.fetch_add( 1, std::memory_order_seq_cst );
    if ( iocp->stopping.load( std::memory_order_seq_cst ) )
    {
        iocp->pending.fetch_sub( 1, std::memory_order_release );
        op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
        return false;
    }

    rocketnet_assert( op->state == ROCKETNET_IOCP_OPERATION_IDLE || op->state == ROCKETNET_IOCP_OPERATION_RETRY );

    if ( op->generation == UINT64_MAX )
    {
        iocp->pending.fetch_sub( 1, std::memory_order_release );
        op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
        return false;
    }

    ++op->generation;
    op->completion_ref = rocketnet_iocp_operation_ref( op );
    op->state = ROCKETNET_IOCP_OPERATION_RECEIVE_POSTED;
    memset( &op->overlapped, 0, sizeof( op->overlapped ) );
    op->address_bytes = sizeof( op->address );
    op->flags = 0;

    int result;
    int error = 0;
    bool accepted_without_socket = false;
#if ROCKETNET_DEV
    if ( rocketnet_iocp_test_post_admitted )
    {
        rocketnet_iocp_test_post_admitted( iocp, rocketnet_iocp_test_post_admitted_context );
    }
    accepted_without_socket = rocketnet_iocp_test_accept_posts;
#endif // #if ROCKETNET_DEV
    if ( accepted_without_socket )
    {
        result = SOCKET_ERROR;
        error = WSA_IO_PENDING;
    }
    else if ( ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST, &error ) )
    {
        result = SOCKET_ERROR;
    }
    else if ( op->uro_data )
    {
        // URO: a buffer of the coalesced maximum and a control area for
        // UDP_COALESCED_INFO, the stride of all but the last datagram. The
        // area is cleared at every post, so a completion that writes no
        // control data never shows the walk the last completion's stride.
        memset( op->control, 0, sizeof( op->control ) );
        op->buffer.buf = (char *) op->uro_data;
        op->buffer.len = ROCKETNET_IOCP_URO_BUFFER_BYTES;
        memset( &op->message, 0, sizeof( op->message ) );
        op->message.name = (LPSOCKADDR) &op->address;
        op->message.namelen = sizeof( op->address );
        op->message.lpBuffers = &op->buffer;
        op->message.dwBufferCount = 1;
        op->message.Control.buf = op->control;
        op->message.Control.len = sizeof( op->control );
        result = iocp->receive_message( rocketnet_iocp_op_socket( iocp, op ), &op->message, NULL, &op->overlapped, NULL );
    }
    else
    {
        op->buffer.buf = (char *) op->frame;
        op->buffer.len = ROCKETNET_RECEIVE_CEILING + 1;
        result = WSARecvFrom( rocketnet_iocp_op_socket( iocp, op ), &op->buffer, 1, NULL, &op->flags,
                              (sockaddr *) &op->address, &op->address_bytes, &op->overlapped, NULL );
    }

    if ( result != 0 && error == 0 )
    {
        error = WSAGetLastError();
    }
    // A queued datagram longer than the buffer is WSA_IO_PENDING too on
    // Windows 11, not an immediate WSAEMSGSIZE: its one completion carries the
    // truncation (tests/test_iocp.cpp test_windows_oversize_receive_at_post).
    if ( result == 0 || error == WSA_IO_PENDING )
    {
        return true; // Immediate success also produces exactly one IOCP event.
    }
    iocp->pending.fetch_sub( 1, std::memory_order_release );
    if ( post_error )
    {
        *post_error = error;
    }

    // a post refused with an ICMP report of an earlier send is counted with
    // the reset statuses, then waits on the retry list like every refused post

    if ( rocketnet_platform_receive_status_is_reset( error ) )
    {
        rocketnet_platform_count_receive_reset();
    }
    iocp->counters[ROCKETNET_COUNTER_IOCP_RECEIVE_REPOST_FAILED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );
    // proofs/tla/RNIocpEndpoints.tla Retry: a refused repost is queued, and any
    // worker's retry pass posts it again on its owner endpoint's socket
    rocketnet_iocp_queue_retry_receive( &iocp->retry_receives, op->completion_ref, &op->state );
    return false;
}

// The UDP_COALESCED_INFO stride of a received message's control area, or 0
// when it has none. A UDP_COALESCED_INFO header whose cmsg_len is shorter
// than WSA_CMSG_LEN( sizeof( DWORD ) ) carries no whole DWORD and yields 0:
// only bytes the header claims are read. Not static: the unit suite calls it
// over a built WSAMSG (test_windows_coalesced_cmsg_short_length_is_not_read).
DWORD rocketnet_iocp_coalesced_stride( WSAMSG * message )
{
    for ( WSACMSGHDR * cmsg = WSA_CMSG_FIRSTHDR( message ); cmsg; cmsg = WSA_CMSG_NXTHDR( message, cmsg ) )
    {
        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_COALESCED_INFO )
        {
            DWORD stride = 0;
            if ( cmsg->cmsg_len >= WSA_CMSG_LEN( sizeof( stride ) ) )
            {
                memcpy( &stride, WSA_CMSG_DATA( cmsg ), sizeof( stride ) );
            }
            return stride;
        }
    }

    return 0;
}

// One completion batch's view of the worker's receive state: the prefilter
// word read once at the batch's start, the batch's counter growth (added to
// the transport counters once, at the batch's end, instead of one atomic add
// per datagram), the worker's own drop tally, and the worker, whose receive
// bank the batch appends to.
struct rocketnet_iocp_batch_t
{
    uint64_t filter_state;
    uint64_t tally[ROCKETNET_SERVER_IOCP_COUNTER_ROWS];
    rocketnet_receive_drop_tally_t * drops;
    rocketnet_iocp_worker_t * worker;
    // a commit in this batch found main not started on the full bank: the
    // rest of the batch is refused without trying again (the batch end
    // commits once more)
    bool held;
};

// the batch's counter growth added to the transport counters, so it is
// visible before the datagrams it counts are committed to main
static void rocketnet_iocp_batch_counters( rocketnet_server_iocp_t * iocp, rocketnet_iocp_batch_t * batch )
{
    for ( int c = 0; c < ROCKETNET_SERVER_IOCP_COUNTER_ROWS; ++c )
    {
        if ( batch->tally[c] )
        {
            iocp->counters[c].fetch_add( batch->tally[c], std::memory_order_relaxed );
            batch->tally[c] = 0;
        }
    }
}

// proofs/tla/RNReceiveHandoff.tla Commit: under the mutex the worker makes its
// bank's count visible to main, which reads it in place, and, when main has
// started on that bank, moves to the free bank (main emptied it before freeing
// it). Otherwise it keeps its bank and appends to it. Only indices and counts
// move under the mutex.
static void rocketnet_iocp_commit_receive( rocketnet_iocp_worker_t * worker )
{
    struct rocketnet_receive_handoff_t * handoff = &worker->receive_handoff;
    const uint32_t count = (uint32_t) worker->buffers[handoff->worker].num_packets;
    rocketnet_platform_mutex_acquire( &worker->mutex );
    rocketnet_receive_handoff_commit( handoff, count );
    rocketnet_platform_mutex_release( &worker->mutex );
}

// The bank a completed datagram goes to: the worker's own, or, when it is
// full, the free bank it moves to if main has started on the full one (the
// batch's counters made visible first). A full bank main has not started on
// stays, and its datagram and the rest of the batch's are refused BANK_FULL
// with no further try, so a flood costs no lock per refused datagram.
static rocketnet_iocp_receive_buffer_t * rocketnet_iocp_receive_bank( rocketnet_iocp_batch_t * batch )
{
    rocketnet_iocp_worker_t * worker = batch->worker;
    rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[worker->receive_handoff.worker];
    if ( buffer->num_packets >= buffer->capacity && !batch->held )
    {
        rocketnet_iocp_batch_counters( worker->owner, batch );
        rocketnet_iocp_commit_receive( worker );
        buffer = &worker->buffers[worker->receive_handoff.worker];
        batch->held = buffer->num_packets >= buffer->capacity;
    }
    return buffer;
}

// A completed datagram's bank admission (rocketnet_iocp_receive_admit),
// counted: banked and RECEIVE_COMPLETED, or RECEIVE_DROPPED with its reason
// in the worker's drop tally. proofs/tla/RNIocpReceiveAdmit.tla Arrive: Bank, or the
// count of Refuse and Full (EveryDropCounted).
static void rocketnet_iocp_bank_datagram( rocketnet_iocp_batch_t * batch, const rocketnet_address_t & from, const uint8_t * packet_data, int packet_bytes )
{
    rocketnet_iocp_receive_buffer_t * buffer = rocketnet_iocp_receive_bank( batch );
    const rocketnet_receive_drop_t drop = rocketnet_iocp_receive_admit( batch->filter_state, packet_data, packet_bytes,
                                                                        buffer->num_packets, buffer->capacity );
    if ( drop != ROCKETNET_RECEIVE_ACCEPT )
    {
        ++batch->tally[ROCKETNET_COUNTER_IOCP_RECEIVE_DROPPED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
        rocketnet_receive_drop_tally_count( batch->drops, drop );
        return;
    }
    const int index = buffer->num_packets++;
    buffer->from[index] = from;
    buffer->packet_bytes[index] = packet_bytes;
    memcpy( buffer->data[index], packet_data, (size_t) packet_bytes );
    ++batch->tally[ROCKETNET_COUNTER_IOCP_RECEIVE_COMPLETED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
}

// Publishes one URO completion: the datagrams of one source, each the
// UDP_COALESCED_INFO stride except a shorter last one, split by the shared
// coalescing rule and each checked against the bounds a single datagram is.
static void rocketnet_iocp_publish_uro( rocketnet_server_iocp_t * iocp, rocketnet_iocp_batch_t * batch,
                                        rocketnet_iocp_operation_t * op, bool success, int receive_error, DWORD bytes )
{
    rocketnet_address_t from;
    if ( !success || bytes == 0 || ( op->message.dwFlags & MSG_TRUNC ) ||
         !rocketnet_iocp_address_read( &op->address, op->message.namelen, iocp->ipv6, &from ) )
    {
        ++batch->tally[ROCKETNET_COUNTER_IOCP_RECEIVE_DROPPED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
        if ( success && bytes == 0 )
        {
            ++batch->tally[ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_SHORT - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
        }
        else if ( receive_error == WSAEMSGSIZE || ( success && ( op->message.dwFlags & MSG_TRUNC ) ) )
        {
            ++batch->tally[ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_LARGE - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
        }
        else
        {
            // an error completion or a source it cannot read
            rocketnet_receive_drop_tally_count( batch->drops, ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED );
        }
        return;
    }

    const DWORD stride = rocketnet_iocp_coalesced_stride( &op->message );
    const int message_bytes = (int) bytes;
    const int segment_bytes = stride > 0 && stride < bytes ? (int) stride : message_bytes;

    // Split window by window: each window but the last is a whole number of
    // strides, so the windows together are the split of the whole message.

    const int window_limit = segment_bytes * ROCKETNET_IOCP_URO_MAX_SEGMENTS;
    int offsets[ROCKETNET_IOCP_URO_MAX_SEGMENTS];
    int lengths[ROCKETNET_IOCP_URO_MAX_SEGMENTS];
    int segments = 0;

    for ( int base = 0; base < message_bytes; )
    {
        const int remaining = message_bytes - base;
        const int window = remaining < window_limit ? remaining : window_limit;
        const int count = rocketnet_coalesce_split( window, segment_bytes, offsets, lengths, ROCKETNET_IOCP_URO_MAX_SEGMENTS );
        for ( int i = 0; i < count; ++i )
        {
            ++segments;

            // rocketnet_coalesce_split writes only positive lengths

            const int length = lengths[i];
            if ( length < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || length > ROCKETNET_RECEIVE_CEILING )
            {
                // shorter than any server-bound packet, or past the ceiling:
                // never takes a bank slot (as gso, io_uring, RIO and XDP)
                ++batch->tally[ROCKETNET_COUNTER_IOCP_RECEIVE_DROPPED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
                ++batch->tally[length < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES ? ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_SHORT - ROCKETNET_SERVER_IOCP_FIRST_COUNTER : ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_LARGE - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
                continue;
            }
            rocketnet_iocp_bank_datagram( batch, from, op->uro_data + base + offsets[i], length );
        }

        base += window;
    }

    if ( segments > 1 )
    {
        ++batch->tally[ROCKETNET_COUNTER_IOCP_URO_BATCHES - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
    }
}

// The end of a completion batch: what the batch banked is committed to main,
// so main's next call returns it (proofs/tla/RNReceiveHandoff.tla
// DeliveredByNextCall). A batch that banked nothing takes no lock. Only the
// worker writes its committed counts, so it reads its own without the mutex.
static void rocketnet_iocp_publish_receive_bank( rocketnet_iocp_worker_t * worker )
{
    const struct rocketnet_receive_handoff_t * handoff = &worker->receive_handoff;
    if ( (uint32_t) worker->buffers[handoff->worker].num_packets == handoff->committed[handoff->worker] )
    {
        return;
    }
    rocketnet_iocp_commit_receive( worker );
}

// Main, once, when it first finds the transport stopped (every worker has
// exited, its acquire), so main owns every bank. What a worker banked that
// main never returned (the rest of the bank the worker left, main's bank past
// its cursor, committed or not, and the worker's own bank when it had moved
// on) is counted by name, RECEIVE_DROP_STOPPED in that worker's tally, so
// every datagram the socket delivered is in exactly one public counter. Each
// was RECEIVE_COMPLETED in the worker's own counters when banked. Main takes
// nothing after stop (rocketnet_server_iocp_receive_packets), and the count is
// made once.
static void rocketnet_iocp_count_stopped( rocketnet_server_iocp_t * iocp )
{
    if ( iocp->stopped_counted )
    {
        return;
    }
    iocp->stopped_counted = true;

    for ( int w = 0; iocp->workers && iocp->drop_tallies && w < iocp->num_workers; ++w )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[w];
        struct rocketnet_receive_handoff_t * handoff = &worker->receive_handoff;
        uint64_t datagrams = handoff->tail_end - handoff->tail_cursor;
        handoff->tail_cursor = handoff->tail_end;

        const rocketnet_iocp_receive_buffer_t * held = &worker->buffers[handoff->main];
        if ( handoff->main_cursor < (uint32_t) held->num_packets )
        {
            datagrams += (uint32_t) held->num_packets - handoff->main_cursor;
        }
        handoff->main_cursor = (uint32_t) held->num_packets;
        handoff->main_limit = handoff->main_cursor;

        if ( handoff->worker != handoff->main )
        {
            datagrams += (uint64_t) worker->buffers[handoff->worker].num_packets;
        }

        if ( datagrams )
        {
            rocketnet_receive_drop_tally_count( &iocp->drop_tallies[w], ROCKETNET_RECEIVE_DROP_STOPPED, datagrams );
        }
    }
}

// The top of a completion worker's pass once stop is published: true when
// the worker may exit. proofs/tla/RNIocpRetryStop.tla Exit: stopping and
// nothing pending; retries still queued are abandoned with the transport.
// The stopping and pending loads are seq_cst: with post_receive's increment
// and check, a post that saw stopping false is counted here.
static bool rocketnet_iocp_worker_stop_pass( rocketnet_server_iocp_t * iocp, int endpoint )
{
    if ( !iocp->stopping.load( std::memory_order_seq_cst ) ||
         iocp->live_lanes.load( std::memory_order_acquire ) != 0 )
    {
        return false;
    }
    // Every worker repeats cancellation after its last possible repost.
    // Cancel only receives, so the final posted sends can complete.
    // proofs/tla/RNIocpEndpoints.tla Cancel and Exit: the worker's
    // endpoint is its own: its receives are
    // [endpoint * ROCKETNET_IOCP_RECEIVES_PER_WORKER, ...)
    if ( iocp->sockets[endpoint] != INVALID_SOCKET )
    {
        for ( int i = endpoint * ROCKETNET_IOCP_RECEIVES_PER_WORKER; i < ( endpoint + 1 ) * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
        {
            if ( !CancelIoEx( (HANDLE) iocp->sockets[endpoint], &iocp->receives[i].overlapped ) &&
                 GetLastError() != ERROR_NOT_FOUND )
            {
                rocketnet_error( "IOCP cancellation failed: %lu", GetLastError() );
            }
        }
    }
    return iocp->pending.load( std::memory_order_seq_cst ) == 0;
}

static void rocketnet_iocp_worker( rocketnet_platform_thread_t * thread, void * argument )
{
    rocketnet_iocp_worker_t * worker = (rocketnet_iocp_worker_t *) argument;
    rocketnet_server_iocp_t * iocp = worker->owner;
    // one worker per endpoint (rocketnet_server_iocp_t sockets)
    const int endpoint = (int) ( worker - iocp->workers );

    // with several endpoints the worker runs where its pin says; it records
    // what it found (rocketnet_server_iocp_worker_placement), which create
    // waits for and logs as it is
    GROUP_AFFINITY worker_pin = iocp->pins[endpoint];
#if ROCKETNET_DEV
    if ( iocp->test_refuse_worker_pins )
    {
        worker_pin.Group = 0xFFFF;
    }
#endif // #if ROCKETNET_DEV
    if ( iocp->num_endpoints <= 1 || iocp->pins[endpoint].Mask == 0 )
    {
        iocp->placements[endpoint].store( ROCKETNET_IOCP_PLACEMENT_NOT_ASKED, std::memory_order_release );
    }
    else if ( SetThreadGroupAffinity( GetCurrentThread(), &worker_pin, NULL ) )
    {
        iocp->placements[endpoint].store( ROCKETNET_IOCP_PLACEMENT_PINNED, std::memory_order_release );
    }
    else
    {
        iocp->placement_errors[endpoint].store( GetLastError(), std::memory_order_relaxed );
        iocp->receive_pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        iocp->placements[endpoint].store( ROCKETNET_IOCP_PLACEMENT_REFUSED, std::memory_order_release );
    }

    for ( ;; )
    {
        // RNBufferHandoff WorkerExit sends every bank before receive cancellation.
        if ( rocketnet_iocp_worker_stop_pass( iocp, endpoint ) )
        {
            break;
        }

        OVERLAPPED_ENTRY entries[64];
        ULONG count = 0;
        const DWORD timeout = rocketnet_iocp_queue_count( &iocp->retry_receives ) ? 1 : 50;
        // proofs/tla/RNIocpEndpoints.tla Dequeue: only this endpoint's port
        const BOOL ok = GetQueuedCompletionStatusEx( iocp->ports[endpoint], entries, 64, &count, timeout, FALSE );
        if ( !ok )
        {
            count = 0;

            if ( GetLastError() != WAIT_TIMEOUT )
            {
                rocketnet_error( "IOCP completion dequeue failed: %lu", GetLastError() );
                rocketnet_platform_thread_sleep( thread, 0.001 );
            }
        }

        if ( count )
        {
            iocp->counters[ROCKETNET_COUNTER_IOCP_COMPLETION_BATCHES - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );
        }

#if ROCKETNET_DEV
        for ( ;; )
        {
            while ( iocp->test_hold_completions.load( std::memory_order_acquire ) )
            {
                rocketnet_platform_thread_sleep( thread, 0.001 );
            }
            iocp->test_active_completion_batches.fetch_add( 1, std::memory_order_acq_rel );
            if ( !iocp->test_hold_completions.load( std::memory_order_acquire ) )
            {
                break;
            }
            iocp->test_active_completion_batches.fetch_sub( 1, std::memory_order_release );
        }
#endif // #if ROCKETNET_DEV
        rocketnet_iocp_batch_t batch;
        batch.filter_state = iocp->filter_state.load( std::memory_order_acquire );
        memset( batch.tally, 0, sizeof( batch.tally ) );
        batch.drops = &iocp->drop_tallies[worker - iocp->workers];
        batch.worker = worker;
        batch.held = false;

        // proofs/tla/RNIocpRetryStop.tla Complete: each receive completion is
        // taken by this worker and reposted.
        for ( ULONG i = 0; i < count; ++i )
        {
            if ( !entries[i].lpOverlapped )
            {
                continue; // A shutdown wakeup, not an I/O operation.
            }
            rocketnet_iocp_operation_t * op = (rocketnet_iocp_operation_t *) entries[i].lpOverlapped;
            ULONG_PTR status = entries[i].Internal;
            int injected = 0;
            if ( ROCKETNET_IOCP_TEST_FAULT( iocp, op->send ? ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION : ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION, &injected ) )
            {
                status = (ULONG_PTR) injected;
            }
            const bool success = status == 0;
            const DWORD bytes = entries[i].dwNumberOfBytesTransferred;

            if ( op->send )
            {
                const rocketnet_iocp_ref_t completed = op->completion_ref;
                if ( !rocketnet_iocp_send_from_ref( iocp, completed, ROCKETNET_IOCP_OPERATION_SEND_POSTED ) )
                {
                    // cannot happen: pending stays raised, so stop never ends; say why
                    rocketnet_error( "IOCP send completion matched no posted send (index %d)", op->index );
                    continue;
                }
                const int counter = success && bytes == (DWORD) op->run_bytes ? ROCKETNET_COUNTER_IOCP_SEND_COMPLETED : ROCKETNET_COUNTER_IOCP_SEND_FAILED;
                iocp->counters[counter - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( (uint64_t) op->run_count, std::memory_order_relaxed );
                iocp->pending.fetch_sub( 1, std::memory_order_release );

                const bool retired = rocketnet_iocp_retire_send( iocp, completed );
                rocketnet_assert( retired );
                if ( !retired )
                {
                    rocketnet_error( "IOCP send completion carried a stale operation generation" );
                }
                continue;
            }

            const rocketnet_iocp_ref_t completed = op->completion_ref;
            if ( !rocketnet_iocp_receive_from_ref( iocp, completed, ROCKETNET_IOCP_OPERATION_RECEIVE_POSTED ) )
            {
                // cannot happen: pending stays raised, so stop never ends; say why
                rocketnet_error( "IOCP receive completion matched no posted receive (index %d)", op->index );
                continue;
            }

            rocketnet_address_t from;
            const bool running = !iocp->stopping.load( std::memory_order_acquire );
            int receive_error = 0;

            if ( !success && running && entries[i].Internal != 0 )
            {
                // a receive completed with an ICMP report of an earlier send
                // (port, host or network unreachable): counted, dropped and
                // reposted below like every failed receive, never the end of
                // the worker, on the URO path too
                DWORD transferred = 0;
                DWORD completion_flags = 0;
                if ( !WSAGetOverlappedResult( rocketnet_iocp_op_socket( iocp, op ), &op->overlapped, &transferred, FALSE, &completion_flags ) )
                {
                    receive_error = WSAGetLastError();
                    if ( rocketnet_platform_receive_status_is_reset( receive_error ) )
                    {
                        rocketnet_platform_count_receive_reset();
                    }
                }
            }

            if ( !running )
            {
                // stopping: a datagram that arrived is never handed to main,
                // so it is counted by name (a URO completion as the datagrams
                // its stride splits it into); a receive the stop cancelled
                // carries none and counts nothing
                if ( success && bytes > 0 )
                {
                    const DWORD stride = op->uro_data ? rocketnet_iocp_coalesced_stride( &op->message ) : 0;
                    const uint64_t datagrams = stride > 0 && stride < bytes ? ( (uint64_t) bytes + stride - 1 ) / stride : 1;
                    batch.tally[ROCKETNET_COUNTER_IOCP_RECEIVE_DROPPED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER] += datagrams;
                    rocketnet_receive_drop_tally_count( batch.drops, ROCKETNET_RECEIVE_DROP_STOPPED, datagrams );
                }
            }
            else if ( op->uro_data )
            {
                rocketnet_iocp_publish_uro( iocp, &batch, op, success, receive_error, bytes );
            }
            else if ( success && bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES && bytes <= ROCKETNET_RECEIVE_CEILING &&
                      rocketnet_iocp_address_read( &op->address, op->address_bytes, iocp->ipv6, &from ) )
            {
                rocketnet_iocp_bank_datagram( &batch, from, op->frame, (int) bytes );
            }
            else
            {
                ++batch.tally[ROCKETNET_COUNTER_IOCP_RECEIVE_DROPPED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
                if ( success && bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
                {
                    // shorter than any server-bound packet: never takes a
                    // bank slot (as gso, io_uring, RIO and XDP)
                    ++batch.tally[ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_SHORT - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
                }
                else if ( receive_error == WSAEMSGSIZE || ( success && bytes > ROCKETNET_RECEIVE_CEILING ) )
                {
                    ++batch.tally[ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_LARGE - ROCKETNET_SERVER_IOCP_FIRST_COUNTER];
                }
                else
                {
                    // an error completion or a source it cannot read
                    rocketnet_receive_drop_tally_count( batch.drops, ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED );
                }
            }

            iocp->pending.fetch_sub( 1, std::memory_order_release );
            op->state = ROCKETNET_IOCP_OPERATION_IDLE;
            rocketnet_iocp_post_receive( iocp, op );
        }

        // A transient posting failure must not permanently shrink the receive
        // pool. Capture the enqueue ticket before the first pop: an immediate
        // refusal requeues at a later ticket and cannot loop in this pass
        // (proofs/tla/RNIocpRetryStop.tla PassStart, PassBounded and RetryOwned).

        const uint64_t retry_stop = iocp->retry_receives.enqueue.load( std::memory_order_acquire );

        for ( ;; )
        {
            rocketnet_iocp_ref_t retry;
            if ( !rocketnet_iocp_queue_pop_before( &iocp->retry_receives, retry_stop, &retry ) )
            {
                break;
            }
            rocketnet_iocp_operation_t * op = rocketnet_iocp_receive_from_ref( iocp, retry, ROCKETNET_IOCP_OPERATION_RETRY );
            if ( !op )
            {
                continue;
            }
            rocketnet_iocp_post_receive( iocp, op );
        }

        // the batch's counter growth, visible before its bank is published

        rocketnet_iocp_batch_counters( iocp, &batch );

        if ( !iocp->stopping.load( std::memory_order_acquire ) )
        {
            rocketnet_iocp_publish_receive_bank( worker );
        }
#if ROCKETNET_DEV
        iocp->test_active_completion_batches.fetch_sub( 1, std::memory_order_release );
#endif // #if ROCKETNET_DEV
    }

    iocp->active_workers.fetch_sub( 1, std::memory_order_release );
}

// proofs/tla/RNIocpEndpoints.tla Stop: the stop flag is set once, and every
// worker's cancel and exit follow it
void rocketnet_server_iocp_stop( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp || iocp->stopping.load( std::memory_order_acquire ) )
    {
        return;
    }
    // proofs/tla/RNBufferHandoff.tla MainStop transfers the final producer with quit.
    // Fill threads have joined before stop. The release transfers the final
    // unpublished producer to TX, without scanning it or acquiring a mutex.

    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        rocketnet_iocp_lane_t * lane = iocp->lanes[l];
        if ( !lane )
        {
            continue;
        }
        uint64_t count = lane->num_staged.load( std::memory_order_relaxed );
        if ( count > (uint64_t) iocp->send_queue_size )
        {
            count = (uint64_t) iocp->send_queue_size;
        }
        lane->send_banks[lane->send_producer].count = (int) count;
    }
    // proofs/tla/RNIocpRetryStop.tla Stop: post_receive retires from here on.
    // seq_cst, in one total order with post_receive's count and check.
    iocp->stopping.store( true, std::memory_order_seq_cst );
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        if ( iocp->lanes[l] && iocp->lanes[l]->send_wake )
        {
            rocketnet_platform_wake_set( iocp->lanes[l]->send_wake );
        }
    }
    // Cancellation runs on the workers after their current pass. This request
    // never acquires a worker mutex or waits for an overlapped operation.

    for ( int e = 0; e < iocp->num_endpoints; ++e )
    {
        if ( iocp->ports[e] )
        {
            PostQueuedCompletionStatus( iocp->ports[e], 0, 0, NULL );
        }
    }
}

// main: every worker exited and nothing pending; the first time it holds,
// what the workers banked that main never returned is counted
// (rocketnet_iocp_count_stopped)
bool rocketnet_server_iocp_stopped( rocketnet_server_iocp_t * iocp )
{
    const bool stopped = iocp->stopping.load( std::memory_order_acquire ) &&
                         iocp->live_lanes.load( std::memory_order_acquire ) == 0 &&
                         iocp->pending.load( std::memory_order_acquire ) == 0 &&
                         iocp->active_workers.load( std::memory_order_acquire ) == 0;
    if ( stopped )
    {
        rocketnet_iocp_count_stopped( iocp );
    }
    return stopped;
}

void rocketnet_server_iocp_destroy( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp )
    {
        return;
    }
    const bool stopped = rocketnet_server_iocp_stopped( iocp );
    rocketnet_assert( stopped );
    if ( !stopped )
    {
        return;
    }
    for ( int t = 0; t < iocp->num_send_threads; ++t )
    {
        rocketnet_iocp_send_thread_t * send = &iocp->send_threads[t];
        if ( send->thread )
        {
            rocketnet_platform_thread_join( send->thread );
            rocketnet_platform_thread_destroy( send->thread );
        }
        if ( send->wake )
        {
            rocketnet_platform_wake_destroy( send->wake );
        }
    }
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        rocketnet_iocp_lane_t * lane = iocp->lanes[l];
        if ( !lane )
        {
            continue;
        }
        if ( lane->send_mutex.ok )
        {
            rocketnet_platform_mutex_destroy( &lane->send_mutex );
        }
        if ( lane->run_mutex.ok )
        {
            rocketnet_platform_mutex_destroy( &lane->run_mutex );
        }
    }
    if ( iocp->workers )
    {
        for ( int i = 0; i < iocp->num_workers; ++i )
        {
            rocketnet_iocp_worker_t * worker = &iocp->workers[i];
            if ( worker->thread )
            {
                rocketnet_platform_thread_join( worker->thread );
                rocketnet_platform_thread_destroy( worker->thread );
            }

            if ( worker->mutex.ok )
            {
                rocketnet_platform_mutex_destroy( &worker->mutex );
            }
            worker->~rocketnet_iocp_worker_t();
        }
    }
    rocketnet_assert( iocp->pending.load() == 0 );

    for ( int e = 0; e < iocp->num_endpoints; ++e )
    {
        if ( iocp->sockets[e] != INVALID_SOCKET )
        {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) iocp->sockets[e], iocp->socket_generations[e] ) )
            {
                const int closed = closesocket( iocp->sockets[e] );
                rocketnet_lifecycle_track_close_end( iocp->socket_generations[e], closed == 0 );
            }
#else
            closesocket( iocp->sockets[e] );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        }

        if ( iocp->ports[e] )
        {
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_IOCP, (uintptr_t) iocp->ports[e], iocp->port_generations[e] ) )
            {
                const BOOL closed = CloseHandle( iocp->ports[e] );
                rocketnet_lifecycle_track_close_end( iocp->port_generations[e], closed != 0 );
            }
#else
            CloseHandle( iocp->ports[e] );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        }
    }

    if ( iocp->sends )
    {
        for ( int i = 0; i < iocp->num_sends; ++i )
        {
            iocp->sends[i].~rocketnet_iocp_operation_t();
        }
    }
    if ( iocp->receives )
    {
        for ( int i = 0; i < iocp->num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
        {
            iocp->receives[i].~rocketnet_iocp_operation_t();
        }
    }
    void * context = iocp->context;
    rocketnet_free( context, iocp->workers );
    rocketnet_free( context, iocp->drop_tallies );
    rocketnet_free( context, iocp->sends );
    rocketnet_free( context, iocp->receives );
    rocketnet_free( context, iocp->uro_buffers );
    rocketnet_free( context, iocp->sized_storage );
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        if ( iocp->lanes[l] )
        {
            iocp->lanes[l]->~rocketnet_iocp_lane_t();
            rocketnet_free( context, iocp->lanes[l] );
        }
    }
    iocp->~rocketnet_server_iocp_t();
    rocketnet_free( context, iocp );
}

// The carves of the transport's sized allocation: every array that scales
// with the configured max_clients (and the retry queue, with the worker
// count). Create and the memory plan both take this layout.
struct rocketnet_iocp_sized_layout_t
{
    int send_queue_size, num_lanes, receive_queue_size, process_capacity;
    uint64_t retry_capacity;
    uint64_t retry_cells;
    uint64_t free_cells[ROCKETNET_IOCP_MAX_LANES], run_refs[ROCKETNET_IOCP_MAX_LANES];
    uint64_t staged[ROCKETNET_IOCP_MAX_LANES][ROCKETNET_IOCP_SEND_BANKS];
    uint64_t run_bytes[ROCKETNET_IOCP_MAX_LANES], run_to[ROCKETNET_IOCP_MAX_LANES];
    uint64_t order[ROCKETNET_IOCP_MAX_LANES], runs[ROCKETNET_IOCP_MAX_LANES], gather[ROCKETNET_IOCP_MAX_LANES];
    uint64_t process_from, process_packet_bytes, process_packet_data;
    uint64_t bank_data[ROCKETNET_IOCP_MAX_WORKERS][ROCKETNET_IOCP_RECEIVE_BANKS];
    uint64_t bank_from[ROCKETNET_IOCP_MAX_WORKERS][ROCKETNET_IOCP_RECEIVE_BANKS];
    uint64_t bank_packet_bytes[ROCKETNET_IOCP_MAX_WORKERS][ROCKETNET_IOCP_RECEIVE_BANKS];
    uint64_t bytes;
};

static bool rocketnet_iocp_sized_layout( int max_clients, int workers, rocketnet_iocp_sized_layout_t * layout )
{
    memset( layout, 0, sizeof( *layout ) );
    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS || workers < 1 || workers > ROCKETNET_IOCP_MAX_WORKERS )
    {
        return false;
    }

    layout->send_queue_size = rocketnet_server_iocp_send_queue_size( max_clients );
    layout->num_lanes = rocketnet_server_iocp_send_lanes( max_clients );
    layout->receive_queue_size = rocketnet_server_iocp_receive_queue_size( max_clients, workers );
    layout->process_capacity = rocketnet_server_receive_packets_per_update( max_clients );
    layout->retry_capacity = (uint64_t) rocketnet_power_of_two_at_least( workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );

    const uint64_t send = (uint64_t) layout->send_queue_size;
    const uint64_t receive = (uint64_t) layout->receive_queue_size;
    const uint64_t process = (uint64_t) layout->process_capacity;

    rocketnet_carve_t carve = { 0, false };
    layout->retry_cells = rocketnet_carve( carve, layout->retry_capacity, sizeof( rocketnet_iocp_ref_cell_t ) );
    for ( int l = 0; l < layout->num_lanes; ++l )
    {
        layout->free_cells[l] = rocketnet_carve( carve, send, sizeof( rocketnet_iocp_ref_cell_t ) );
        layout->run_refs[l] = rocketnet_carve( carve, send, sizeof( rocketnet_iocp_run_slot_t ) );
        for ( int i = 0; i < ROCKETNET_IOCP_SEND_BANKS; ++i )
        {
            layout->staged[l][i] = rocketnet_carve( carve, send, sizeof( rocketnet_iocp_ref_t ) );
        }
        layout->run_bytes[l] = rocketnet_carve( carve, send, sizeof( int ) );
        layout->run_to[l] = rocketnet_carve( carve, send, sizeof( rocketnet_address_t ) );
        layout->order[l] = rocketnet_carve( carve, send, sizeof( int ) );
        layout->runs[l] = rocketnet_carve( carve, send, sizeof( rocketnet_send_gather_run_t ) );
        layout->gather[l] = rocketnet_carve( carve, rocketnet_send_gather_scratch_bytes( layout->send_queue_size ), 1 );
    }
    layout->process_from = rocketnet_carve( carve, process, sizeof( rocketnet_address_t ) );
    layout->process_packet_bytes = rocketnet_carve( carve, process, sizeof( int ) );
    layout->process_packet_data = rocketnet_carve( carve, process, sizeof( uint8_t * ) );
    for ( int w = 0; w < workers; ++w )
    {
        for ( int b = 0; b < ROCKETNET_IOCP_RECEIVE_BANKS; ++b )
        {
            layout->bank_data[w][b] = rocketnet_carve( carve, receive, (uint64_t) ROCKETNET_FRAME_STRIDE );
            layout->bank_from[w][b] = rocketnet_carve( carve, receive, sizeof( rocketnet_address_t ) );
            layout->bank_packet_bytes[w][b] = rocketnet_carve( carve, receive, sizeof( int ) );
        }
    }

    layout->bytes = carve.bytes;
    return !carve.failed && carve.bytes <= SIZE_MAX;
}

uint64_t rocketnet_server_iocp_allocation_bytes( int workers, int max_clients )
{
    return rocketnet_server_iocp_allocation_bytes_threads( workers, 0, max_clients );
}

uint64_t rocketnet_server_iocp_allocation_bytes_threads( int workers, int asked_send_threads, int max_clients )
{
    if ( workers < 1 || workers > ROCKETNET_IOCP_MAX_WORKERS )
    {
        return 0;
    }
    rocketnet_iocp_sized_layout_t layout;
    if ( !rocketnet_iocp_sized_layout( max_clients, workers, &layout ) )
    {
        return 0;
    }
    if ( !rocketnet_server_iocp_send_threads_for( asked_send_threads, layout.num_lanes ) )
    {
        return 0;
    }
    uint64_t bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_iocp_t ),
                                                     layout.bytes,
                                                     (uint64_t) workers * sizeof( rocketnet_iocp_worker_t ),
                                                     (uint64_t) workers * sizeof( rocketnet_receive_drop_tally_t ),
                                                     (uint64_t) layout.num_lanes * (uint64_t) layout.send_queue_size * sizeof( rocketnet_iocp_operation_t ),
                                                     (uint64_t) workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * sizeof( rocketnet_iocp_operation_t ),
                                                     (uint64_t) workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * ROCKETNET_IOCP_URO_BUFFER_BYTES } );
    const uint64_t lanes = (uint64_t) layout.num_lanes;
    const uint64_t send_threads = (uint64_t) rocketnet_server_iocp_send_threads_for( asked_send_threads, layout.num_lanes );
    if ( !bytes || !rocketnet_memory_add( bytes, lanes, sizeof( rocketnet_iocp_lane_t ) + rocketnet_allocation_overhead() ) ||
         !rocketnet_memory_add( bytes, (uint64_t) workers + send_threads, rocketnet_platform_thread_allocation_bytes() ) ||
         !rocketnet_memory_add( bytes, send_threads, rocketnet_platform_wake_allocation_bytes() ) )
    {
        return 0;
    }
    return bytes <= SIZE_MAX ? bytes : 0;
}

static void rocketnet_iocp_send_worker( rocketnet_platform_thread_t * thread, void * argument );

// SIO_CPU_AFFINITY (mstcpip.h): a UDP socket given a processor before bind
// may bind an address and port that other such sockets hold, and receives
// the datagrams whose RSS processor is its own. It cannot be combined with
// SO_REUSEADDR or SO_EXCLUSIVEADDRUSE (measured on aws-win-rio-1: the second
// exclusive socket's bind is refused, 10013).
#ifndef SIO_CPU_AFFINITY
#define SIO_CPU_AFFINITY _WSAIOW( IOC_VENDOR, 21 )
#endif // #ifndef SIO_CPU_AFFINITY
static_assert( (unsigned long) SIO_CPU_AFFINITY == 0x98000015ul, "SIO_CPU_AFFINITY is the vendor IOCTL 21" );

// The unspecified address, 0.0.0.0 or ::.
static bool rocketnet_iocp_address_is_wildcard( const rocketnet_address_t * address )
{
    if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        return address->data.ip == 0;
    }
    for ( int i = 0; i < 8; ++i )
    {
        if ( address->data.ipv6[i] != 0 )
        {
            return false;
        }
    }
    return true;
}

// Binds a plain socket to the address with SO_EXCLUSIVEADDRUSE and closes
// it: refused when anything already holds the address and port, so endpoints
// that share the port by SIO_CPU_AFFINITY never join a socket someone else
// bound first. Never called for port 0.
static bool rocketnet_iocp_exclusive_probe( const rocketnet_address_t * bind_address, int * status )
{
    const bool ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    sockaddr_storage address;
    const int address_bytes = rocketnet_platform_sockaddr_for( ipv6, bind_address, &address );
    SOCKET probe = WSASocketW( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT );
    if ( probe == INVALID_SOCKET )
    {
        rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
        return false;
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const uint64_t generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) probe, "iocp exclusive probe socket" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    BOOL enabled = TRUE;
    DWORD v6_only = FALSE;
    bool bound = setsockopt( probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &enabled, sizeof( enabled ) ) != SOCKET_ERROR &&
                 ( !ipv6 || setsockopt( probe, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &v6_only, sizeof( v6_only ) ) != SOCKET_ERROR ) &&
                 bind( probe, (sockaddr *) &address, address_bytes ) != SOCKET_ERROR;
    if ( !bound )
    {
        // as rio's probe: a refusal for want of memory reads OUT_OF_MEMORY,
        // any other refusal TRANSPORT_BIND_REFUSED
        const int error = WSAGetLastError();
        rocketnet_create_status( status, ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
        rocketnet_platform_windows_create_status( status, (unsigned long) error );
        rocketnet_warn( "iocp transport refused: the exclusive probe could not bind port %d (Winsock %d): another socket holds it",
                        bind_address->port, error );
    }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) probe, generation ) )
    {
        const int closed = closesocket( probe );
        rocketnet_lifecycle_track_close_end( generation, closed == 0 );
    }
#else
    closesocket( probe );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return bound;
}

// A logical processor in the order the transport places its threads: the first
// logical processor of each physical core in processor-index order, then the
// cores' other logical processors in order.
struct rocketnet_iocp_core_t
{
    int index;           // the processor index SIO_CPU_AFFINITY takes
    GROUP_AFFINITY pin;  // the same processor
    GROUP_AFFINITY core; // every logical processor of its core
};

// The placement order, in *order (count entries, one per logical processor
// at most; freed with rocketnet_free on context). The index SIO_CPU_AFFINITY
// takes is the group's offset (the active processors of the groups before it)
// plus the processor's number in its group. False only when Windows cannot
// list its processors (status and an error naming who asked are set).
static bool rocketnet_iocp_core_order( void * context, const char * who, rocketnet_iocp_core_t ** order_out, int * count_out, int * cores_out, int * status )
{
    *order_out = NULL;
    *count_out = 0;
    *cores_out = 0;
    DWORD bytes = 0;
    if ( GetLogicalProcessorInformationEx( RelationProcessorCore, NULL, &bytes ) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 )
    {
        rocketnet_platform_windows_create_status( status, GetLastError() );
        rocketnet_error( "iocp transport refused: the processors could not be listed for %s (Win32 %lu)", who, GetLastError() );
        return false;
    }
    const DWORD logical = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
    uint8_t * info = (uint8_t *) rocketnet_malloc( context, (size_t) bytes );
    rocketnet_iocp_core_t * order = (rocketnet_iocp_core_t *) rocketnet_malloc( context, sizeof( rocketnet_iocp_core_t ) * ( (size_t) logical + 1 ) );
    if ( !info || !order )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_free( context, order );
        rocketnet_free( context, info );
        return false;
    }
    bool ok = logical > 0 && GetLogicalProcessorInformationEx( RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) info, &bytes );
    int count = 0;
    // pass 0 takes each core's first logical processor, pass 1 the others
    for ( int pass = 0; ok && pass < 2; ++pass )
    {
        const int first = count;
        for ( DWORD offset = 0; offset < bytes; )
        {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX * entry = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ( info + offset );
            if ( entry->Size == 0 )
            {
                break;
            }
            offset += entry->Size;
            if ( entry->Relationship != RelationProcessorCore || entry->Processor.GroupCount == 0 )
            {
                continue;
            }
            const GROUP_AFFINITY * mask = &entry->Processor.GroupMask[0];
            int group_offset = 0;
            for ( WORD g = 0; g < mask->Group; ++g )
            {
                group_offset += (int) GetActiveProcessorCount( g );
            }
            bool seen_first = false;
            for ( int bit = 0; bit < (int) ( sizeof( KAFFINITY ) * 8 ); ++bit )
            {
                if ( !( mask->Mask & ( (KAFFINITY) 1 << bit ) ) )
                {
                    continue;
                }
                const bool take = pass == 0 ? !seen_first : seen_first;
                seen_first = true;
                if ( !take || count >= (int) logical )
                {
                    continue;
                }
                rocketnet_iocp_core_t * p = &order[count++];
                memset( p, 0, sizeof( *p ) );
                p->index = group_offset + bit;
                p->pin.Group = mask->Group;
                p->pin.Mask = (KAFFINITY) 1 << bit;
                p->core.Group = mask->Group;
                p->core.Mask = mask->Mask;
                if ( pass == 0 )
                {
                    break;
                }
            }
        }
        if ( pass == 0 )
        {
            *cores_out = count; // the cores: the first pass takes one logical processor of each
        }
        // each pass in processor-index order
        for ( int i = first + 1; i < count; ++i )
        {
            const rocketnet_iocp_core_t key = order[i];
            int j = i - 1;
            while ( j >= first && order[j].index > key.index )
            {
                order[j + 1] = order[j];
                --j;
            }
            order[j + 1] = key;
        }
    }
    ok = ok && count > 0;
    if ( !ok )
    {
        rocketnet_platform_windows_create_status( status, GetLastError() );
        rocketnet_error( "iocp transport refused: the processors could not be listed for %s (Win32 %lu)", who, GetLastError() );
        rocketnet_free( context, order );
        rocketnet_free( context, info );
        return false;
    }
    rocketnet_free( context, info );
    *order_out = order;
    *count_out = count;
    return true;
}

// The processor of each endpoint, into iocp->processors and iocp->pins: the
// cores in placement order (rocketnet_iocp_core_order), wrapping past the last.
// Windows' default RSS layout (base processor 0, one processor per core: on
// the ENA adapter Get-NetAdapterRss lists 0, 2, 4, ...) hands receive queue i
// to the first processor of core i, so with as many workers as queues each
// queue's flows reach the endpoint on its processor. A flow whose RSS
// processor has no endpoint is delivered to the first endpoint bound
// (measured), so a different RSS layout costs spread, never datagrams. The
// same order as the RIO transport's lanes (rocketnet_rio_lane_processors).
// False only when Windows cannot list its processors.
static bool rocketnet_iocp_endpoint_processors( void * context, rocketnet_server_iocp_t * iocp, int * status )
{
    rocketnet_iocp_core_t * order = NULL;
    int count = 0;
    int cores = 0;
    if ( !rocketnet_iocp_core_order( context, "the workers", &order, &count, &cores, status ) )
    {
        return false;
    }
    (void) cores;
    for ( int i = 0; i < iocp->num_endpoints; ++i )
    {
        iocp->processors[i] = order[i % count].index;
        iocp->pins[i] = order[i % count].pin;
        iocp->cores[i] = order[i % count].core;
    }
    if ( iocp->num_endpoints > count )
    {
        // a fallback: named at warn, and each worker's processor is readable
        // after start (rocketnet_server_iocp_worker_placement)
        rocketnet_warn( "iocp transport: %d workers asked for %d logical processors; workers %d..%d share processors with workers 0..%d", iocp->num_endpoints,
                        count, count, iocp->num_endpoints - 1, iocp->num_endpoints - 1 - count );
    }
    rocketnet_free( context, order );
    return true;
}

// The core of each send thread, into iocp->send_pins: thread t takes the
// core t places after the last core a completion worker is pinned to (the
// cores in placement order, as above), one logical processor of it, so every
// thread has a core of its own and the receive threads' cores keep the
// network threads together in order, then the game's threads. One worker,
// which is not pinned (rocketnet_server_iocp_worker_placement NOT_ASKED),
// holds the first core. Past the last core the threads wrap onto the first
// ones, named at warn. Run after the workers' pins are final, and before the
// send threads start. False only when Windows cannot list its processors.
static bool rocketnet_iocp_send_processors( void * context, rocketnet_server_iocp_t * iocp, int flags, int * status )
{
    rocketnet_iocp_core_t * order = NULL;
    int count = 0;
    int cores = 0;
    if ( !rocketnet_iocp_core_order( context, "the send threads", &order, &count, &cores, status ) )
    {
        return false;
    }
    int first = 1;
    if ( iocp->num_endpoints > 1 )
    {
        first = 0;
        for ( int e = 0; e < iocp->num_endpoints; ++e )
        {
            for ( int i = 0; i < count; ++i )
            {
                const bool same_core = iocp->pins[e].Mask != 0 ? ( order[i].core.Group == iocp->pins[e].Group && ( order[i].core.Mask & iocp->pins[e].Mask ) != 0 )
                                                               : order[i].index == iocp->processors[e];
                if ( same_core )
                {
                    first = first > i + 1 ? first : i + 1;
                    break;
                }
            }
        }
    }
    if ( first + iocp->num_send_threads > cores )
    {
        // the count is of cores (the first pass of the order), not logical
        // processors: a send thread on a sibling logical processor of a
        // worker's core does not have a core of its own
        rocketnet_warn( "iocp transport: %d workers and %d send threads need %d cores after the first and the machine has %d: send threads past the last core share cores with earlier threads",
                        iocp->num_endpoints, iocp->num_send_threads, first + iocp->num_send_threads - 1, cores );
    }
    for ( int t = 0; t < iocp->num_send_threads; ++t )
    {
        iocp->send_pins[t] = order[( first + t ) % count].pin;
#if ROCKETNET_DEV
        if ( flags & ROCKETNET_IOCP_REFUSE_SEND_PINS )
        {
            iocp->send_pins[t].Group = 0xFFFF;
        }
#else
        (void) flags;
#endif // #if ROCKETNET_DEV
    }
    rocketnet_free( context, order );
    return true;
}

#if ROCKETNET_DEV
// development library only: reads a test layout "R0,R1,../W0,W1,..", processor
// numbers in group 0, into rss and workers (each up to max). False when a
// character is not a digit, comma or the one slash, or a number is past the
// group's processors.
static bool rocketnet_iocp_test_layout_read( const char * text, int * rss, int * num_rss, int * workers, int * num_workers, int max )
{
    const int group_processors = (int) GetActiveProcessorCount( 0 );
    *num_rss = 0;
    *num_workers = 0;
    bool after_slash = false;
    const char * p = text;
    while ( *p )
    {
        if ( *p == '/' && !after_slash )
        {
            after_slash = true;
            ++p;
            continue;
        }
        if ( *p == ',' )
        {
            ++p;
            continue;
        }
        if ( *p < '0' || *p > '9' )
        {
            return false;
        }
        int value = 0;
        while ( *p >= '0' && *p <= '9' )
        {
            if ( value > 1000 )
            {
                return false;
            }
            value = value * 10 + ( *p - '0' );
            ++p;
        }
        if ( value >= group_processors || value >= (int) ( sizeof( KAFFINITY ) * 8 ) )
        {
            return false;
        }
        if ( !after_slash && *num_rss < max )
        {
            rss[( *num_rss )++] = value;
        }
        else if ( after_slash && *num_workers < max )
        {
            workers[( *num_workers )++] = value;
        }
    }
    return true;
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static std::atomic<int> rocketnet_iocp_test_forced_flags{ 0 };

void rocketnet_server_iocp_test_force_flags( int flags )
{
    rocketnet_iocp_test_forced_flags.store( flags, std::memory_order_relaxed );
}
#endif // #if ROCKETNET_DEV

rocketnet_server_iocp_t * rocketnet_server_iocp_create( void * context, rocketnet_address_t * bind_address, int num_workers, int max_clients
#if ROCKETNET_DEV
                                                        ,
                                                        int fail_stage, int flags
#endif // #if ROCKETNET_DEV
                                                        ,
                                                        int * status )
{
#if ROCKETNET_DEV
    return rocketnet_server_iocp_create_threads( context, bind_address, num_workers, 0, max_clients, fail_stage, flags, status );
#else  // #if ROCKETNET_DEV
    return rocketnet_server_iocp_create_threads( context, bind_address, num_workers, 0, max_clients, status );
#endif // #if ROCKETNET_DEV
}

rocketnet_server_iocp_t * rocketnet_server_iocp_create_threads( void * context, rocketnet_address_t * bind_address, int num_workers,
                                                                int num_send_threads, int max_clients
#if ROCKETNET_DEV
                                                                ,
                                                                int fail_stage, int flags
#endif // #if ROCKETNET_DEV
                                                                ,
                                                                int * status )
{
#if ROCKETNET_DEV
    flags |= rocketnet_iocp_test_forced_flags.load( std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) )
    {
        return NULL;
    }
    if ( num_workers < 1 || num_workers > ROCKETNET_IOCP_MAX_WORKERS )
    {
        rocketnet_warn( "iocp transport refused: worker count %d is outside 1..%d", num_workers, ROCKETNET_IOCP_MAX_WORKERS );
        return NULL;
    }
    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        rocketnet_warn( "iocp transport refused: max_clients %d is outside 1..%d", max_clients, ROCKETNET_MAX_CLIENTS );
        return NULL;
    }

    rocketnet_iocp_sized_layout_t layout;
    if ( !rocketnet_iocp_sized_layout( max_clients, num_workers, &layout ) )
    {
        return NULL;
    }
    const int send_threads = rocketnet_server_iocp_send_threads_for( num_send_threads, layout.num_lanes );
    if ( !send_threads )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
        rocketnet_warn( "iocp transport refused: num_iocp_send_threads %d is outside 1..%d, the send lanes of a %d-client server "
                        "(ROCKETNET_SERVER_NUM_IOCP_SEND_THREADS; 0 selects the default)",
                        num_send_threads, layout.num_lanes, max_clients );
        return NULL;
    }
    // More than one worker shares the port by SIO_CPU_AFFINITY, which has no
    // exclusive form, so the endpoints take a specific local address: a
    // squatter on that exact address and port is refused by the probe below,
    // and a later plain bind of the wildcard at that port (which Windows
    // allows, measured) is less specific than the endpoints. An IPv4-mapped
    // address would give endpoint 0 a dual-stack socket whose bound name
    // comes back as IPv4. One worker binds exclusively and takes either.
    if ( num_workers > 1 && rocketnet_iocp_address_is_wildcard( bind_address ) )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_INVALID_ADDRESS );
        rocketnet_warn( "iocp transport refused: %d workers need a specific local address; set bind_ipv4_address or bind_ipv6_address in the server config", num_workers );
        return NULL;
    }
    if ( num_workers > 1 && bind_address->type == ROCKETNET_ADDRESS_IPV6 && rocketnet_address_is_ipv4_in_ipv6( bind_address ) )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_INVALID_ADDRESS );
        rocketnet_warn( "iocp transport refused: %d workers need a specific local address, not an IPv4-mapped one; set bind_ipv4_address in the server config", num_workers );
        return NULL;
    }
    // An explicit port is probed. Port 0 is not: endpoint 0 binds it with
    // SIO_CPU_AFFINITY and Windows hands it a port no socket holds, so there
    // is no gap between a probe and the endpoints. Once endpoint 0 is bound,
    // a later plain or SO_REUSEADDR bind of its address and port is refused
    // (measured, 10048 and 10013), but a socket of another process that also
    // sets SIO_CPU_AFFINITY can join the port and receive the flows RSS hands
    // its processor: Windows offers no exclusive form of a port shared by
    // processor (SO_EXCLUSIVEADDRUSE refuses the second endpoint).
    if ( num_workers > 1 && bind_address->port != 0 && !rocketnet_iocp_exclusive_probe( bind_address, status ) )
    {
        return NULL;
    }

    void * storage = rocketnet_malloc( context, sizeof( rocketnet_server_iocp_t ) );
    if ( !storage )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    rocketnet_server_iocp_t * iocp = new ( storage ) rocketnet_server_iocp_t();
    iocp->context = context;
    iocp->num_workers = num_workers;
    for ( int e = 0; e < ROCKETNET_IOCP_MAX_WORKERS; ++e )
    {
        iocp->sockets[e] = INVALID_SOCKET;
    }
    iocp->num_endpoints = num_workers;
    iocp->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    iocp->owner_epoch = rocketnet_iocp_next_owner_epoch.fetch_add( 1, std::memory_order_relaxed );
    iocp->send_queue_size = layout.send_queue_size;
    iocp->num_lanes = layout.num_lanes;
    iocp->num_send_threads = send_threads;
#if ROCKETNET_DEV
    iocp->test_refuse_worker_pins = ( flags & ROCKETNET_IOCP_REFUSE_WORKER_PINS ) != 0;
#endif // #if ROCKETNET_DEV
    iocp->num_sends = layout.num_lanes * layout.send_queue_size;
    iocp->receive_queue_size = layout.receive_queue_size;

    sockaddr_storage address;
    int address_bytes = rocketnet_platform_sockaddr_for( iocp->ipv6, bind_address, &address );

    // one update's inbound packets at the server's configured client count
    // (a request; the stack charges only what it holds)

    const int receive_buffer_bytes = rocketnet_server_transport_receive_buffer_request( "iocp", max_clients );
    int buffer_bytes = receive_buffer_bytes;
    int granted_receive_bytes = 0;
    int granted_length = sizeof( granted_receive_bytes );
    DWORD returned_bytes = 0;
    BOOL enabled = TRUE;
    DWORD v6_only = FALSE;
    bool uso_everywhere = true;
    bool uro_everywhere = true;
    if ( iocp->owner_epoch == 0 )
    {
        goto failed;
    }
    for ( int l = 0; l < layout.num_lanes; ++l )
    {
        void * lane_storage = rocketnet_malloc( context, sizeof( rocketnet_iocp_lane_t ) );
        if ( !lane_storage )
        {
            rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
            goto failed;
        }
        rocketnet_iocp_lane_t * lane = new ( lane_storage ) rocketnet_iocp_lane_t();
        iocp->lanes[l] = lane;
        lane->owner = iocp;
        if ( !rocketnet_platform_mutex_create( &lane->run_mutex ) || !rocketnet_platform_mutex_create( &lane->send_mutex ) )
        {
            goto failed;
        }
    }

    iocp->sized_storage = rocketnet_malloc( context, (size_t) layout.bytes );
    if ( !iocp->sized_storage )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        goto failed;
    }
    {
        uint8_t * base = (uint8_t *) iocp->sized_storage;
        rocketnet_iocp_queue_initialize( &iocp->retry_receives, base + layout.retry_cells, layout.retry_capacity );
        for ( int l = 0; l < layout.num_lanes; ++l )
        {
            rocketnet_iocp_lane_t * lane = iocp->lanes[l];
            rocketnet_iocp_queue_initialize( &lane->free_sends, base + layout.free_cells[l], (uint64_t) layout.send_queue_size );
            lane->run_refs = (rocketnet_iocp_run_slot_t *) ( base + layout.run_refs[l] );
            for ( int i = 0; i < layout.send_queue_size; ++i )
            {
                new ( &lane->run_refs[i] ) rocketnet_iocp_run_slot_t();
            }
            for ( int i = 0; i < ROCKETNET_IOCP_SEND_BANKS; ++i )
            {
                lane->send_banks[i].staged = (rocketnet_iocp_ref_t *) ( base + layout.staged[l][i] );
            }
            lane->run_bytes = (int *) ( base + layout.run_bytes[l] );
            lane->run_to = (rocketnet_address_t *) ( base + layout.run_to[l] );
            lane->order = (int *) ( base + layout.order[l] );
            lane->runs = (rocketnet_send_gather_run_t *) ( base + layout.runs[l] );
            const bool gather = rocketnet_send_gather_scratch_init( &lane->gather, base + layout.gather[l], layout.send_queue_size );
            rocketnet_assert( gather );
            (void) gather;
        }
        iocp->process.num_packets = 0;
        iocp->process.capacity = layout.process_capacity;
        iocp->process.from = (rocketnet_address_t *) ( base + layout.process_from );
        iocp->process.packet_bytes = (int *) ( base + layout.process_packet_bytes );
        iocp->process.packet_data = (uint8_t **) ( base + layout.process_packet_data );

        // ASan builds: the red zones between the carves (sdk/rocketnet_memory.h)
        ROCKETNET_RED_ZONE_CARVE( base, rocketnet_iocp_sized_layout( max_clients, num_workers, &layout ) );
    }

    iocp->workers = (rocketnet_iocp_worker_t *) rocketnet_malloc( context, sizeof( rocketnet_iocp_worker_t ) * (size_t) num_workers );
    if ( !iocp->workers )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        goto failed;
    }
    for ( int i = 0; i < num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = new ( &iocp->workers[i] ) rocketnet_iocp_worker_t();
        // proofs/tla/RNReceiveHandoff.tla Init: main and the worker share
        // bank 0, bank 1 is free and bank 2 is main's finished tail
        rocketnet_receive_handoff_init( &worker->receive_handoff );
        uint8_t * base = (uint8_t *) iocp->sized_storage;
        for ( int bank = 0; bank < ROCKETNET_IOCP_RECEIVE_BANKS; ++bank )
        {
            rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[bank];
            buffer->data = ( uint8_t( * )[ROCKETNET_FRAME_STRIDE] )( base + layout.bank_data[i][bank] );
            buffer->from = (rocketnet_address_t *) ( base + layout.bank_from[i][bank] );
            buffer->packet_bytes = (int *) ( base + layout.bank_packet_bytes[i][bank] );
            buffer->num_packets = 0;
            buffer->capacity = layout.receive_queue_size;
        }
    }

    iocp->drop_tallies = (rocketnet_receive_drop_tally_t *) rocketnet_malloc( context, sizeof( rocketnet_receive_drop_tally_t ) * (size_t) num_workers );
    if ( !iocp->drop_tallies )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        goto failed;
    }
    for ( int i = 0; i < num_workers; ++i )
    {
        rocketnet_receive_drop_tally_reset( new ( &iocp->drop_tallies[i] ) rocketnet_receive_drop_tally_t() );
    }

    iocp->sends = (rocketnet_iocp_operation_t *) rocketnet_malloc( context, sizeof( rocketnet_iocp_operation_t ) * (size_t) iocp->num_sends );
    if ( !iocp->sends )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        goto failed;
    }
    for ( int i = 0; i < iocp->num_sends; ++i )
    {
        rocketnet_iocp_operation_t * op = new ( &iocp->sends[i] ) rocketnet_iocp_operation_t();
        op->index = i;
        op->send = true;
        op->owner_epoch = iocp->owner_epoch;
        op->state = ROCKETNET_IOCP_OPERATION_FREE;
    }

    // each lane's free queue starts with its own carve, in index order
    // (proofs/tla/RNIocpSendLanes.tla Init)
    for ( int i = 0; i < iocp->num_sends; ++i )
    {
        rocketnet_iocp_lane_t * seed_lane = iocp->lanes[i / iocp->send_queue_size];
        if ( !rocketnet_iocp_queue_push( &seed_lane->free_sends, rocketnet_iocp_operation_ref( &iocp->sends[i] ) ) )
        {
            goto failed;
        }
    }
    iocp->receives = (rocketnet_iocp_operation_t *) rocketnet_malloc( context, sizeof( rocketnet_iocp_operation_t ) * (size_t) num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    if ( !iocp->receives )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        goto failed;
    }
    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
    {
        rocketnet_iocp_operation_t * op = new ( &iocp->receives[i] ) rocketnet_iocp_operation_t();
        op->index = i;
        op->owner_epoch = iocp->owner_epoch;
        op->state = ROCKETNET_IOCP_OPERATION_IDLE;
    }

    if ( iocp->num_endpoints > 1 && !rocketnet_iocp_endpoint_processors( context, iocp, status ) )
    {
        goto failed;
    }
#if ROCKETNET_DEV
    // development library only: ROCKETNET_TEST_IOCP_PIN places the completion
    // workers for a measurement: "sibling" on the core's other logical
    // processors (the RSS processor keeps the receive DPCs), "core" on the
    // whole core, "none" unpinned; unset is the RSS processor
    if ( iocp->num_endpoints > 1 )
    {
        char pin[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
        if ( rocketnet_platform_getenv( "ROCKETNET_TEST_IOCP_PIN", pin, sizeof( pin ) ) > 0 )
        {
            for ( int e = 0; e < iocp->num_endpoints; ++e )
            {
                if ( strcmp( pin, "sibling" ) == 0 && ( iocp->cores[e].Mask & ~iocp->pins[e].Mask ) != 0 )
                {
                    const KAFFINITY rss = iocp->pins[e].Mask;
                    iocp->pins[e] = iocp->cores[e];
                    iocp->pins[e].Mask &= ~rss;
                }
                else if ( strcmp( pin, "core" ) == 0 )
                {
                    iocp->pins[e] = iocp->cores[e];
                }
                else if ( strcmp( pin, "none" ) == 0 )
                {
                    iocp->pins[e].Mask = 0;
                }
            }
        }
        // ROCKETNET_TEST_IOCP_LAYOUT="R0,R1,../W0,W1,.." puts endpoint e's
        // socket on processor Re (SIO_CPU_AFFINITY: the processor RSS hands
        // its flows) and its completion worker on processor We alone, group 0,
        // for a measurement in the layout a game runs: the NIC's receive
        // processing on cores the operator chose (Set-NetAdapterRss), the
        // network threads on cores of their own. A list shorter than the
        // workers, or a processor past group 0's, leaves the placement above
        // and says so.
        char test_layout[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
        if ( rocketnet_platform_getenv( "ROCKETNET_TEST_IOCP_LAYOUT", test_layout, sizeof( test_layout ) ) > 0 )
        {
            int rss_cpus[ROCKETNET_IOCP_MAX_WORKERS];
            int worker_cpus[ROCKETNET_IOCP_MAX_WORKERS];
            int num_rss_cpus = 0;
            int num_worker_cpus = 0;
            if ( !rocketnet_iocp_test_layout_read( test_layout, rss_cpus, &num_rss_cpus, worker_cpus, &num_worker_cpus, ROCKETNET_IOCP_MAX_WORKERS ) ||
                 num_rss_cpus < iocp->num_endpoints || num_worker_cpus < iocp->num_endpoints )
            {
                rocketnet_warn( "iocp transport: ROCKETNET_TEST_IOCP_LAYOUT \"%s\" does not place %d workers in group 0; placement unchanged", test_layout,
                                iocp->num_endpoints );
            }
            else
            {
                for ( int e = 0; e < iocp->num_endpoints; ++e )
                {
                    iocp->processors[e] = rss_cpus[e];
                    memset( &iocp->pins[e], 0, sizeof( iocp->pins[e] ) );
                    iocp->pins[e].Group = 0;
                    iocp->pins[e].Mask = (KAFFINITY) 1 << worker_cpus[e];
                    memset( &iocp->cores[e], 0, sizeof( iocp->cores[e] ) );
                    iocp->cores[e].Group = 0;
                    iocp->cores[e].Mask = ( (KAFFINITY) 1 << rss_cpus[e] ) | ( (KAFFINITY) 1 << worker_cpus[e] );
                }
            }
        }
    }
#endif // #if ROCKETNET_DEV

    // the send threads' cores, after the workers' (the workers' pins are final)
#if ROCKETNET_DEV
    if ( !rocketnet_iocp_send_processors( context, iocp, flags, status ) )
#else // #if ROCKETNET_DEV
    if ( !rocketnet_iocp_send_processors( context, iocp, 0, status ) )
#endif // #if ROCKETNET_DEV
    {
        goto failed;
    }

    // One endpoint per worker. Endpoint 0 binds the address (and takes the
    // port when it is 0); the others bind the address endpoint 0 was given.
    // The probes run on every bound socket and a feature is used only when
    // every endpoint takes it; a refusal is a tier, not an error.
    for ( int e = 0; e < iocp->num_endpoints; ++e )
    {
        SOCKET socket = WSASocketW( iocp->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT );
        iocp->sockets[e] = socket;
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( socket != INVALID_SOCKET )
        {
            iocp->socket_generations[e] = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) socket, "iocp socket" );
        }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( socket == INVALID_SOCKET )
        {
            rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
            goto failed;
        }
#if ROCKETNET_DEV
        if ( fail_stage == 1 )
        {
            goto failed;
        }
#endif // #if ROCKETNET_DEV
        if ( iocp->num_endpoints > 1 )
        {
            // before bind, and instead of SO_EXCLUSIVEADDRUSE: a refusal is
            // named, never a quiet fallback to one endpoint
            USHORT processor = (USHORT) iocp->processors[e];
            DWORD returned = 0;
            if ( WSAIoctl( socket, SIO_CPU_AFFINITY, &processor, sizeof( processor ), NULL, 0, &returned, NULL, NULL ) == SOCKET_ERROR )
            {
                // as rio's: a refusal for want of memory reads OUT_OF_MEMORY,
                // never a bind refusal
                const int error = WSAGetLastError();
                rocketnet_create_status( status, ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
                rocketnet_platform_windows_create_status( status, (unsigned long) error );
                rocketnet_error( "iocp transport refused: SIO_CPU_AFFINITY to processor %d was refused (Winsock %d); workers share the port only through it",
                                 iocp->processors[e], error );
                goto failed;
            }
        }
        else if ( setsockopt( socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
        {
            goto failed;
        }
        if ( iocp->ipv6 && setsockopt( socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &v6_only, sizeof( v6_only ) ) == SOCKET_ERROR )
        {
            goto failed;
        }
        buffer_bytes = receive_buffer_bytes;
        if ( setsockopt( socket, SOL_SOCKET, SO_RCVBUF, (const char *) &buffer_bytes, sizeof( buffer_bytes ) ) == SOCKET_ERROR )
        {
            goto failed;
        }
        if ( e == 0 )
        {
            granted_length = sizeof( granted_receive_bytes );
            if ( getsockopt( socket, SOL_SOCKET, SO_RCVBUF, (char *) &granted_receive_bytes, &granted_length ) == SOCKET_ERROR )
            {
                granted_receive_bytes = 0;
            }
            rocketnet_server_receive_buffer_granted( "iocp", receive_buffer_bytes, granted_receive_bytes );
        }
        buffer_bytes = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
        if ( setsockopt( socket, SOL_SOCKET, SO_SNDBUF, (const char *) &buffer_bytes, sizeof( buffer_bytes ) ) == SOCKET_ERROR )
        {
            goto failed;
        }
        // Match the portable socket's don't-fragment rule, tolerating option
        // refusal: a refusal is a fallback, so it is named at warn.

        if ( iocp->ipv6 && setsockopt( socket, IPPROTO_IPV6, IPV6_DONTFRAG, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
        {
            rocketnet_warn( "iocp transport: IPV6_DONTFRAG refused (Winsock %d); IPv6 datagrams may be fragmented", WSAGetLastError() );
        }
        if ( setsockopt( socket, IPPROTO_IP, IP_DONTFRAGMENT, (const char *) &enabled, sizeof( enabled ) ) == SOCKET_ERROR )
        {
            rocketnet_warn( "iocp transport: IP_DONTFRAGMENT refused (Winsock %d); IPv4 datagrams may be fragmented", WSAGetLastError() );
        }
        // both ICMP reset reports off; a refusal is logged once and the receive
        // paths skip the statuses

        rocketnet_platform_socket_disable_udp_resets( (uintptr_t) socket );
        if ( bind( socket, (sockaddr *) &address, address_bytes ) == SOCKET_ERROR )
        {
            // a refusal for want of memory reads OUT_OF_MEMORY, a bind the
            // address refuses TRANSPORT_BIND_REFUSED
            rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
            if ( rocketnet_platform_bind_refused( WSAGetLastError() ) )
            {
                rocketnet_create_status( status, ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
            }
            goto failed;
        }
        if ( e == 0 )
        {
            // the others bind the port endpoint 0 was given
            address_bytes = sizeof( address );
            if ( getsockname( socket, (sockaddr *) &address, &address_bytes ) == SOCKET_ERROR )
            {
                goto failed;
            }
        }

        // UDP_SEND_MSG_SIZE (IPPROTO_UDP socket options, Winsock reference,
        // learn.microsoft.com): a DWORD; non-zero makes the stack break each
        // sent buffer into messages of that size, the last one possibly
        // shorter; zero, the default, is no segmentation. The probe writes zero.
        DWORD zero = 0;
        if ( setsockopt( socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (const char *) &zero, sizeof( zero ) ) == SOCKET_ERROR )
        {
            uso_everywhere = false;
        }

        // UDP_RECV_MAX_COALESCED_SIZE (same page): a DWORD; non-zero lets the
        // stack coalesce datagrams of one source address and port into one
        // message of at most that many bytes, all the same size but a shorter
        // last one; the stride reaches the application only through a receive
        // with control data (WSARecvMsg) as the DWORD control message
        // UDP_COALESCED_INFO. WSARecvMsg is the provider's, the same for
        // every endpoint.
        // The development flag skips the probe altogether, as it did before
        // the endpoints: the option is never set, so there is nothing to clear.
        DWORD maximum = ROCKETNET_IOCP_URO_BUFFER_BYTES;
        GUID guid = WSAID_WSARECVMSG;
        bool uro_probe = true;
#if ROCKETNET_DEV
        uro_probe = ( flags & ROCKETNET_IOCP_DISABLE_URO ) == 0;
#endif // #if ROCKETNET_DEV
        if ( !uro_probe )
        {
            uro_everywhere = false;
        }
        else if ( ( e == 0 && ( WSAIoctl( socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof( guid ), &iocp->receive_message, sizeof( iocp->receive_message ),
                                          &returned_bytes, NULL, NULL ) == SOCKET_ERROR ||
                                !iocp->receive_message ) ) ||
                  setsockopt( socket, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, (const char *) &maximum, sizeof( maximum ) ) == SOCKET_ERROR )
        {
            uro_everywhere = false;
        }

        // one completion port per endpoint, drained by its one worker
        iocp->ports[e] = CreateIoCompletionPort( (HANDLE) socket, NULL, 0, 1 );
        if ( !iocp->ports[e] )
        {
            rocketnet_platform_windows_create_status( status, GetLastError() );
        }
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( iocp->ports[e] )
        {
            iocp->port_generations[e] = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_IOCP, (uintptr_t) iocp->ports[e], "iocp completion port" );
        }
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( !iocp->ports[e]
#if ROCKETNET_DEV
             || fail_stage == 2
#endif // #if ROCKETNET_DEV
        )
        {
            goto failed;
        }
    }

#if ROCKETNET_DEV
    if ( flags & ROCKETNET_IOCP_DISABLE_USO )
    {
        uso_everywhere = false;
    }
    if ( flags & ROCKETNET_IOCP_DISABLE_URO )
    {
        uro_everywhere = false;
    }
#endif // #if ROCKETNET_DEV
    if ( uso_everywhere )
    {
        iocp->features.fetch_or( ROCKETNET_IOCP_FEATURE_USO, std::memory_order_relaxed );
    }
    if ( uro_everywhere )
    {
        iocp->uro_buffers = (uint8_t *) rocketnet_malloc( context, (size_t) num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * ROCKETNET_IOCP_URO_BUFFER_BYTES );
        if ( !iocp->uro_buffers )
        {
            rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
            goto failed;
        }
        iocp->features.fetch_or( ROCKETNET_IOCP_FEATURE_URO, std::memory_order_relaxed );
    }
    else
    {
        // a URO setting a probe left on one endpoint is cleared: every
        // endpoint receives one datagram per message, as WSARecvFrom expects
        // A refusal is named and refuses create: an endpoint left coalescing
        // would deliver a message of several datagrams to a receive that
        // reads one, and the rest would be lost as TOO_LARGE.
        DWORD off = 0;
        bool uro_flag_off = true;
#if ROCKETNET_DEV
        uro_flag_off = ( flags & ROCKETNET_IOCP_DISABLE_URO ) == 0;
#endif // #if ROCKETNET_DEV
        for ( int e = 0; uro_flag_off && e < iocp->num_endpoints; ++e )
        {
            if ( setsockopt( iocp->sockets[e], IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, (const char *) &off, sizeof( off ) ) == SOCKET_ERROR )
            {
                rocketnet_error( "iocp transport refused: UDP_RECV_MAX_COALESCED_SIZE could not be cleared on endpoint %d (Winsock %d)", e, WSAGetLastError() );
                rocketnet_platform_windows_create_status( status, (unsigned long) WSAGetLastError() );
                goto failed;
            }
        }
    }
    // The send threads: thread t owns lanes t, t + threads, ..., and the one
    // wake their flushes set.
    for ( int t = 0; t < iocp->num_send_threads; ++t )
    {
        rocketnet_iocp_send_thread_t * send = &iocp->send_threads[t];
        send->owner = iocp;
        send->index = t;
        send->wake = rocketnet_platform_wake_create( context, NULL, status );
        if ( !send->wake )
        {
            goto failed;
        }
        send->num_lanes = rocketnet_iocp_send_thread_lanes( t, iocp->num_send_threads, iocp->num_lanes, send->lanes );
        const int owned = send->num_lanes;
        for ( int k = 0; k < send->num_lanes; ++k )
        {
            iocp->lanes[send->lanes[k]]->send_wake = send->wake;
        }
        iocp->live_lanes.fetch_add( owned, std::memory_order_relaxed );
        send->thread = rocketnet_platform_thread_create( context, rocketnet_iocp_send_worker, send, status );
        if ( !send->thread )
        {
            iocp->live_lanes.fetch_sub( owned, std::memory_order_relaxed );
            goto failed;
        }

        rocketnet_platform_server_thread_priority( send->thread );
    }

    for ( int i = 0; i < num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[i];
        worker->owner = iocp;
        if ( !rocketnet_platform_mutex_create( &worker->mutex ) )
        {
            goto failed;
        }
        iocp->active_workers.fetch_add( 1, std::memory_order_relaxed );
        worker->thread = rocketnet_platform_thread_create( context, rocketnet_iocp_worker, worker, status );
        if ( !worker->thread )
        {
            iocp->active_workers.fetch_sub( 1, std::memory_order_relaxed );
            goto failed;
        }

        // each worker takes the server thread priority from this thread,
        // right after its create, as the GSO queue threads do

        rocketnet_platform_server_thread_priority( worker->thread );
#if ROCKETNET_DEV
        if ( fail_stage == 3 )
        {
            goto failed;
        }
#endif // #if ROCKETNET_DEV
    }

    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
    {
        if ( iocp->uro_buffers )
        {
            iocp->receives[i].uro_data = iocp->uro_buffers + (size_t) i * ROCKETNET_IOCP_URO_BUFFER_BYTES;
        }
        int post_error = 0;
        if ( !rocketnet_iocp_post_receive( iocp, &iocp->receives[i], &post_error ) )
        {
            rocketnet_platform_windows_create_status( status, (unsigned long) post_error );
            goto failed;
        }
#if ROCKETNET_DEV
        if ( fail_stage == 4 )
        {
            goto failed;
        }
#endif // #if ROCKETNET_DEV
    }

    if ( !rocketnet_iocp_address_read( &address, address_bytes, iocp->ipv6, bind_address ) )
    {
        goto failed;
    }
    rocketnet_info( "iocp transport: %d completion workers, %d send threads and %d send lanes of %d on port %d, USO %s, URO %s", num_workers, iocp->num_send_threads, iocp->num_lanes,
                    iocp->send_queue_size, bind_address->port,
                    ( iocp->features.load() & ROCKETNET_IOCP_FEATURE_USO ) ? "yes" : "no (WSASendTo per datagram)",
                    ( iocp->features.load() & ROCKETNET_IOCP_FEATURE_URO ) ? "yes" : "no (WSARecvFrom per datagram)" );
    // each worker's placement as it found it: create waits (at most a second
    // per worker) for every answer, and says what happened
    for ( int e = 0; iocp->num_endpoints > 1 && e < iocp->num_endpoints; ++e )
    {
        for ( int waits = 0; waits < 1000 && iocp->placements[e].load( std::memory_order_acquire ) == ROCKETNET_IOCP_PLACEMENT_PENDING; ++waits )
        {
            rocketnet_platform_sleep( 0.001 );
        }
        const int placement = iocp->placements[e].load( std::memory_order_acquire );
        if ( placement == ROCKETNET_IOCP_PLACEMENT_PINNED )
        {
            rocketnet_info( "iocp transport: worker %d socket on processor %d (SIO_CPU_AFFINITY), worker pinned to mask 0x%llx in group %d", e, iocp->processors[e],
                            (unsigned long long) iocp->pins[e].Mask, (int) iocp->pins[e].Group );
        }
        else if ( placement == ROCKETNET_IOCP_PLACEMENT_REFUSED )
        {
            rocketnet_warn( "iocp transport: worker %d socket on processor %d (SIO_CPU_AFFINITY), worker NOT pinned: SetThreadGroupAffinity to mask 0x%llx in group %d refused (Win32 %lu)",
                            e, iocp->processors[e], (unsigned long long) iocp->pins[e].Mask, (int) iocp->pins[e].Group,
                            iocp->placement_errors[e].load( std::memory_order_relaxed ) );
        }
        else if ( placement == ROCKETNET_IOCP_PLACEMENT_NOT_ASKED )
        {
            rocketnet_info( "iocp transport: worker %d socket on processor %d (SIO_CPU_AFFINITY), worker not pinned (not asked)", e, iocp->processors[e] );
        }
        else
        {
            rocketnet_warn( "iocp transport: worker %d socket on processor %d (SIO_CPU_AFFINITY), worker placement not reported within a second", e, iocp->processors[e] );
        }
    }
    // each send thread's placement as it found it, the same way
    for ( int t = 0; t < iocp->num_send_threads; ++t )
    {
        for ( int waits = 0; waits < 1000 && iocp->send_placements[t].load( std::memory_order_acquire ) == ROCKETNET_IOCP_PLACEMENT_PENDING; ++waits )
        {
            rocketnet_platform_sleep( 0.001 );
        }
        const int placement = iocp->send_placements[t].load( std::memory_order_acquire );
        if ( placement == ROCKETNET_IOCP_PLACEMENT_PINNED )
        {
            rocketnet_info( "iocp transport: send thread %d pinned to mask 0x%llx in group %d", t, (unsigned long long) iocp->send_pins[t].Mask, (int) iocp->send_pins[t].Group );
        }
        else if ( placement == ROCKETNET_IOCP_PLACEMENT_REFUSED )
        {
            rocketnet_warn( "iocp transport: send thread %d NOT pinned: SetThreadGroupAffinity to mask 0x%llx in group %d refused (Win32 %lu)", t,
                            (unsigned long long) iocp->send_pins[t].Mask, (int) iocp->send_pins[t].Group, iocp->send_placement_errors[t].load( std::memory_order_relaxed ) );
        }
        else
        {
            rocketnet_warn( "iocp transport: send thread %d placement not reported within a second", t );
        }
    }
    return iocp;

failed:
#if ROCKETNET_DEV
    rocketnet_warn( "iocp transport create failed (Winsock %d, Win32 %lu, test stage %d)", WSAGetLastError(), GetLastError(), fail_stage );
#else
    rocketnet_warn( "iocp transport create failed (Winsock %d, Win32 %lu)", WSAGetLastError(), GetLastError() );
#endif // #if ROCKETNET_DEV
    // The wait has no bound, unlike RIO's inline drain: here the workers and
    // the send worker may already run on this memory, so it can be neither
    // freed nor left while any of them is live. Every post raised pending and
    // every completion or refused post lowers it; a wait still running after
    // 10000 sleeps of a millisecond (ten seconds or more) says what it waits
    // on, once.
    rocketnet_server_iocp_stop( iocp );
    for ( int passes = 0; !rocketnet_server_iocp_stopped( iocp ); ++passes )
    {
        if ( passes == 10000 )
        {
            rocketnet_error( "iocp transport: create failure still waiting for %d pending operations and %d workers", iocp->pending.load(), iocp->active_workers.load() );
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_server_iocp_destroy( iocp );

    return NULL;
}

// proofs/tla/ServerStopSpread.tla Room: the producer bank's free slots,
// at most the free send operations (a frame flushed earlier holds its
// operation until its completion). The free count is the queue's positions,
// enqueue read first; a free whose push is claimed but not yet published can
// make one start refuse, and the stop step retries it on its next step.

// One lane's room. The transport's room is the lanes' together: a start for
// a destination whose lane is full refuses while another lane has room, and
// the driver's send queue operations give the server each lane's room.

static int rocketnet_iocp_lane_room( rocketnet_server_iocp_t * iocp, rocketnet_iocp_lane_t * lane )
{
    const uint64_t staged = lane->num_staged.load( std::memory_order_relaxed );
    if ( staged >= (uint64_t) iocp->send_queue_size )
    {
        return 0;
    }
    const uint64_t enqueue = lane->free_sends.enqueue.load( std::memory_order_acquire );
    const uint64_t dequeue = lane->free_sends.dequeue.load( std::memory_order_acquire );
    const uint64_t free_sends = enqueue > dequeue ? enqueue - dequeue : 0;
    const uint64_t slots = (uint64_t) iocp->send_queue_size - staged;
    return (int) ( free_sends < slots ? free_sends : slots );
}

int rocketnet_server_iocp_send_room( rocketnet_server_iocp_t * iocp )
{
    if ( iocp->stopping.load( std::memory_order_acquire ) )
    {
        return 0;
    }
    int room = 0;
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        room += rocketnet_iocp_lane_room( iocp, iocp->lanes[l] );
    }
    return room;
}

int rocketnet_server_iocp_send_lane_count( rocketnet_server_iocp_t * iocp )
{
    return iocp->num_lanes;
}

int rocketnet_server_iocp_send_lane( rocketnet_server_iocp_t * iocp, const rocketnet_address_t * to )
{
    return rocketnet_iocp_lane_index( to, iocp->num_lanes );
}

int rocketnet_server_iocp_send_lane_room( rocketnet_server_iocp_t * iocp, int lane )
{
    rocketnet_assert( lane >= 0 && lane < iocp->num_lanes );
    if ( lane < 0 || lane >= iocp->num_lanes || iocp->stopping.load( std::memory_order_acquire ) )
    {
        return 0;
    }
    return rocketnet_iocp_lane_room( iocp, iocp->lanes[lane] );
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: every send operation free.
// An operation holds its slot from its start through the bank, the post and
// its completion, so all of them free in every lane means no send is in
// flight. dequeue is read first: a free whose push is claimed and not yet
// published already finished its send.

bool rocketnet_server_iocp_send_idle( rocketnet_server_iocp_t * iocp )
{
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        rocketnet_iocp_lane_t * lane = iocp->lanes[l];
        if ( !rocketnet_iocp_queue_send_idle( &lane->free_sends, lane->num_staged.load( std::memory_order_relaxed ), iocp->send_queue_size ) )
        {
            return false;
        }
    }
    return true;
}

uint8_t * rocketnet_server_iocp_start_packet( rocketnet_server_iocp_t * iocp, const rocketnet_address_t * to, uint64_t * packet_id )
{
    if ( iocp->stopping.load( std::memory_order_acquire ) )
    {
        return NULL;
    }
    // every packet of a destination stages in its one lane, so the lane's
    // send thread posts a client's packets in the order they were started
    // (proofs/tla/RNIocpSendLanes.tla Start)
    rocketnet_iocp_lane_t * lane = iocp->lanes[rocketnet_iocp_lane_index( to, iocp->num_lanes )];
    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_acquire_send( iocp, lane, &ref ) )
    {
        // proofs/tla/RNIocpSendLanes.tla StartRefused: the destination's lane
        // has no free operation, and nothing changes
        return NULL;
    }
    rocketnet_iocp_operation_t * op = &iocp->sends[ref.index];
    op->address_bytes = rocketnet_platform_sockaddr_for( iocp->ipv6, to, &op->address );
    if ( !op->address_bytes )
    {
        // an address this socket cannot carry: counted SEND_FAILED, not a full staging
        iocp->counters[ROCKETNET_COUNTER_IOCP_SEND_FAILED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );
        rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
        return NULL;
    }

    // Reserve a staging slot only after acquiring a usable frame. A failed
    // pop/address must not consume capacity while completions return frames
    // during this same fill phase. Every accepted slot owns one pool entry.

    const uint64_t slot = lane->num_staged.fetch_add( 1, std::memory_order_relaxed );
    if ( slot >= (uint64_t) iocp->send_queue_size )
    {
        rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
        return NULL;
    }

    op->packet_bytes = 0;
    op->to = *to;
    lane->send_banks[lane->send_producer].staged[slot] = ref;
    *packet_id = rocketnet_iocp_packet_id( ref );

    return op->frame;
}

void rocketnet_server_iocp_finish_packet( rocketnet_server_iocp_t * iocp, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_packet_ref( iocp, packet_id, &ref ) )
    {
        return;
    }
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    rocketnet_assert( op && frame == op->frame );

    // Release callers may not use a frame or generation the transport did not issue.

    if ( !op || frame != op->frame )
    {
        return;
    }
    (void) frame;

    // a refused length loses the packet: the slot keeps zero bytes and the
    // send phase releases it, so the loss is counted here (as RIO finish)
    if ( packet_bytes <= 0 || packet_bytes > ROCKETNET_RECEIVE_CEILING )
    {
        iocp->counters[ROCKETNET_COUNTER_IOCP_SEND_FAILED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );
        return;
    }
    op->packet_bytes = packet_bytes;
}

void rocketnet_server_iocp_abort_packet( rocketnet_server_iocp_t * iocp, uint64_t packet_id, uint8_t * frame )
{
    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_packet_ref( iocp, packet_id, &ref ) )
    {
        return;
    }
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    rocketnet_assert( op && frame == op->frame );

    // Release callers may not use a frame or generation the transport did not issue.

    if ( !op || frame != op->frame )
    {
        return;
    }
    (void) frame;
    op->packet_bytes = 0;
}

// The per-datagram send: one frame, one WSASendTo.
static void rocketnet_iocp_post_send( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t & ref )
{
    // proofs/tla/RNIocpSendLease.tla Post: the caller keeps its packet id.
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    if ( !op )
    {
        return;
    }
    op->run_count = 1;
    op->run_bytes = op->packet_bytes;
    op->run_ticket = 0;

    memset( &op->overlapped, 0, sizeof( op->overlapped ) );
    op->buffer.buf = (char *) op->frame + ROCKETNET_FRAME_WIRE_OFFSET;
    op->buffer.len = (ULONG) op->packet_bytes;
    op->completion_ref = ref;
    op->state = ROCKETNET_IOCP_OPERATION_SEND_POSTED;
    iocp->pending.fetch_add( 1, std::memory_order_relaxed );

    int error = 0;
    int result = SOCKET_ERROR;

    if ( !ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, &error ) )
    {
        result = WSASendTo( rocketnet_iocp_op_socket( iocp, op ), &op->buffer, 1, NULL, 0,
                            (sockaddr *) &op->address, op->address_bytes, &op->overlapped, NULL );
        if ( result == SOCKET_ERROR )
        {
            error = WSAGetLastError();
        }
    }

    if ( result == SOCKET_ERROR && error != WSA_IO_PENDING )
    {
        // An unsuccessful submission has no completion. All accepted
        // submissions, including immediate success, retire in the worker.
        iocp->pending.fetch_sub( 1, std::memory_order_release );
        iocp->counters[ROCKETNET_COUNTER_IOCP_SEND_FAILED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );
        rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_SEND_POSTED );
    }
}

// A USO run: one overlapped WSASendMsg with one WSABUF per frame and the
// run's stride as its own IPPROTO_UDP / UDP_SEND_MSG_SIZE control message, so
// a send still pending when the next run posts keeps the stride it was posted
// with; the socket option stays the zero the create probe wrote. A run is one
// stride (the shared run rule's shorter tail is the documented shorter last
// segment). WSASendMsg captures the WSABUF array before it returns (WSASendMsg
// reference), so the array lives on this stack; the WSAMSG and its control
// message live in the run's first frame. Returns false when the host refused
// USO live: the feature is cleared, counted, and the caller sends the run
// frame by frame.
static bool rocketnet_iocp_post_send_run( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t * staged, int run )
{
    rocketnet_iocp_operation_t * first = rocketnet_iocp_send_from_ref( iocp, staged[0], ROCKETNET_IOCP_OPERATION_STAGED );
    if ( !first )
    {
        return false;
    }
    WSABUF buffers[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
    int bytes = 0;

    for ( int k = 0; k < run; ++k )
    {
        rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, staged[k], ROCKETNET_IOCP_OPERATION_STAGED );
        if ( !op )
        {
            return false;
        }
        buffers[k].buf = (char *) op->frame + ROCKETNET_FRAME_WIRE_OFFSET;
        buffers[k].len = (ULONG) op->packet_bytes;
        bytes += op->packet_bytes;
    }

    uint64_t run_ticket = 0;
    if ( !rocketnet_iocp_allocate_run( iocp, rocketnet_iocp_lane_of_index( iocp, staged[0].index ), staged, run, &run_ticket ) )
    {
        return false; // bounded run storage is busy; send frames individually
    }
    for ( int k = 0; k < run; ++k )
    {
        rocketnet_iocp_operation_t * op = &iocp->sends[staged[k].index];
        op->state = ROCKETNET_IOCP_OPERATION_SEND_POSTED;
    }

    first->run_count = run;
    first->run_bytes = bytes;
    first->run_ticket = run_ticket;
    first->completion_ref = staged[0];

    int error = 0;

    memset( &first->overlapped, 0, sizeof( first->overlapped ) );
    memset( &first->message, 0, sizeof( first->message ) );
    memset( first->control, 0, sizeof( first->control ) );
    first->message.name = (LPSOCKADDR) &first->address;
    first->message.namelen = first->address_bytes;
    first->message.lpBuffers = buffers;
    first->message.dwBufferCount = (ULONG) run;
    first->message.Control.buf = first->control;
    first->message.Control.len = WSA_CMSG_SPACE( sizeof( DWORD ) ); // 24 bytes of the 64
    WSACMSGHDR * cmsg = WSA_CMSG_FIRSTHDR( &first->message );
    cmsg->cmsg_len = WSA_CMSG_LEN( sizeof( DWORD ) );
    cmsg->cmsg_level = IPPROTO_UDP;
    cmsg->cmsg_type = UDP_SEND_MSG_SIZE;
    const DWORD stride = (DWORD) first->packet_bytes;
    memcpy( WSA_CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
    iocp->pending.fetch_add( 1, std::memory_order_relaxed );

    int result = SOCKET_ERROR;

    if ( !ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, &error ) )
    {
        result = WSASendMsg( rocketnet_iocp_op_socket( iocp, first ), &first->message, 0, NULL, &first->overlapped, NULL );
        if ( result == 0 )
        {
            iocp->counters[ROCKETNET_COUNTER_IOCP_USO_SENDS - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );
            return true;
        }

        error = WSAGetLastError();
    }

    if ( error == WSA_IO_PENDING )
    {
        iocp->counters[ROCKETNET_COUNTER_IOCP_USO_SENDS - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );
        return true;
    }

    // An unsuccessful submission has no completion.

    iocp->pending.fetch_sub( 1, std::memory_order_release );

    if ( error == WSAEINVAL || error == WSAEOPNOTSUPP )
    {
        // As the GSO transport does for a refused live UDP_SEGMENT send:
        // degrade the tier and resend losslessly, never drop the run.
        rocketnet_warn( "iocp transport: live USO send refused (Winsock %d). degrading to WSASendTo per datagram", error );
        iocp->features.fetch_and( ~ROCKETNET_IOCP_FEATURE_USO, std::memory_order_relaxed );
        iocp->counters[ROCKETNET_COUNTER_IOCP_USO_FALLBACKS - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( 1, std::memory_order_relaxed );

        rocketnet_iocp_ref_t refs[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
        const bool released = rocketnet_iocp_take_run( iocp, rocketnet_iocp_lane_of_index( iocp, staged[0].index ), run_ticket, run, refs );
        rocketnet_assert( released );
        for ( int k = 0; released && k < run; ++k )
        {
            iocp->sends[refs[k].index].state = ROCKETNET_IOCP_OPERATION_STAGED;
        }
        return false;
    }

    iocp->counters[ROCKETNET_COUNTER_IOCP_SEND_FAILED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].fetch_add( (uint64_t) run, std::memory_order_relaxed );

    const bool retired = rocketnet_iocp_retire_send( iocp, staged[0] );
    rocketnet_assert( retired );
    if ( !retired )
    {
        rocketnet_error( "IOCP refused USO post carried a stale run generation" );
    }
    return true;
}

// The lane's view of a bank, cut into the runs its posts carry: run r is
// slots order[runs[r].first .. runs[r].first + runs[r].count - 1] of the
// bank, one destination and one stride; an aborted slot is in no run. The
// send gather (sdk/rocketnet_send_gather.h) groups the bank by destination,
// each destination's slots in staged order, so a player's packets make one
// run wherever they sit in the bank. With USO off every live slot is a run of
// one. Returns the number of runs.
// proofs/tla/RNSendGatherLayout.tla; RNIocpSendLanes.tla Post: each
// destination's packets leave in the order they were started.
static int rocketnet_iocp_group_bank( rocketnet_iocp_lane_t * lane, int count, bool uso )
{
    // count is at most the lane's send_queue_size, the scratch's max_slots,
    // and both limits are positive: the gather does not refuse here
    const int num_runs = rocketnet_send_gather( lane->run_to, sizeof( rocketnet_address_t ), lane->run_bytes, sizeof( int ), count,
                                                uso ? ROCKETNET_IOCP_USO_MAX_SEGMENTS : 1, ROCKETNET_IOCP_USO_MAX_SUPER_BYTES,
                                                &lane->gather, lane->order, lane->runs );
    rocketnet_assert( num_runs >= 0 );
    if ( num_runs >= 0 )
    {
        return num_runs;
    }

    // unreachable by the bound above; were it reached, every live slot is
    // still sent, as a run of one, so no staged operation is stranded
    int runs = 0;
    for ( int i = 0; i < count; ++i )
    {
        if ( lane->run_bytes[i] > 0 )
        {
            lane->order[runs] = i;
            lane->runs[runs].first = runs;
            lane->runs[runs].count = 1;
            lane->runs[runs].segment_bytes = lane->run_bytes[i];
            lane->runs[runs].last_bytes = lane->run_bytes[i];
            ++runs;
        }
    }
    return runs;
}

static void rocketnet_iocp_send_bank( rocketnet_server_iocp_t * iocp, rocketnet_iocp_lane_t * lane, rocketnet_iocp_send_bank_t * bank )
{
    // proofs/tla/RNBufferHandoff.tla Consume and WorkerExit keep sending after quit.
    const int count = bank->count;
    const bool uso = ( iocp->features.load( std::memory_order_relaxed ) & ROCKETNET_IOCP_FEATURE_USO ) != 0;

    // every staged slot names a STAGED operation of this lane; an aborted or
    // refused one (zero bytes) goes back to the lane's free queue here

    for ( int i = 0; i < count; ++i )
    {
        const rocketnet_iocp_ref_t ref = bank->staged[i];
        rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
        lane->run_bytes[i] = op ? op->packet_bytes : 0;
        if ( op )
        {
            lane->run_to[i] = op->to;
            if ( op->packet_bytes == 0 )
            {
                rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
            }
        }
    }

    const int num_runs = rocketnet_iocp_group_bank( lane, count, uso );

    for ( int r = 0; r < num_runs; ++r )
    {
        const int run = lane->runs[r].count;
        rocketnet_iocp_ref_t refs[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
        for ( int k = 0; k < run; ++k )
        {
            refs[k] = bank->staged[lane->order[lane->runs[r].first + k]];
        }
        // a live refusal clears USO for the transport (USO_FALLBACKS, once);
        // the bank's later runs are then sent frame by frame, not tried again
        const bool uso_live = ( iocp->features.load( std::memory_order_relaxed ) & ROCKETNET_IOCP_FEATURE_USO ) != 0;
        if ( run > 1 && uso_live && rocketnet_iocp_post_send_run( iocp, refs, run ) )
        {
            continue;
        }
        for ( int k = 0; k < run; ++k )
        {
            rocketnet_iocp_post_send( iocp, refs[k] );
        }
    }

    bank->count = 0;
}

// Publish each lane's already finished producer with a constant-time
// ownership swap. A failed try leaves every frame in that lane's caller-owned
// bank for the next call; the other lanes publish regardless.
// proofs/tla/RNBankFifoLayout.tla Publish: the free stack's top and the ready tail % ROCKETNET_IOCP_SEND_BANKS,
// MCRNBankFifoLayoutLifoSend.cfg.
void rocketnet_server_iocp_send_packets( rocketnet_server_iocp_t * iocp )
{
    if ( iocp->stopping.load( std::memory_order_acquire ) )
    {
        return;
    }
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        rocketnet_iocp_lane_t * lane = iocp->lanes[l];
        uint64_t count = lane->num_staged.load( std::memory_order_relaxed );
        if ( count == 0 )
        {
            continue;
        }
        if ( count > (uint64_t) iocp->send_queue_size )
        {
            count = (uint64_t) iocp->send_queue_size;
        }
        if ( !rocketnet_platform_mutex_try_acquire( &lane->send_mutex ) )
        {
            continue;
        }
        if ( lane->send_free_count == 0 )
        {
            rocketnet_platform_mutex_release( &lane->send_mutex );
            continue;
        }

        const int producer = lane->send_producer;
        lane->send_banks[producer].count = (int) count;
        lane->send_producer = lane->send_free[--lane->send_free_count];
        lane->send_ready[( lane->send_ready_head + lane->send_ready_count ) % ROCKETNET_IOCP_SEND_BANKS] = producer;
        lane->send_ready_count++;
#if ROCKETNET_DEV
        iocp->test_pending_tx_banks.fetch_add( 1, std::memory_order_release );
#endif // #if ROCKETNET_DEV
        rocketnet_platform_mutex_release( &lane->send_mutex );
        lane->num_staged.store( 0, std::memory_order_relaxed );
        rocketnet_platform_wake_set( lane->send_wake );
    }
}

// One lane's turn on its send thread: the lane's ready bank, if there is one,
// grouped and posted, or, once stopping with none left, the lane's final
// producer bank. Returns 1 when it sent a bank, 0 when it had nothing, 2 when
// the lane is finished (stop's last bank is posted).
// proofs/tla/RNIocpSendLanes.tla Post: per lane, as before; the thread only
// chooses which of its lanes takes the next turn.
static int rocketnet_iocp_send_lane_turn( rocketnet_platform_thread_t * thread, rocketnet_server_iocp_t * iocp, rocketnet_iocp_lane_t * lane )
{
    (void) thread; // the development test hold sleeps on it
#if ROCKETNET_DEV
    for ( ;; )
    {
        while ( iocp->test_hold_tx.load( std::memory_order_acquire ) &&
                !iocp->stopping.load( std::memory_order_acquire ) )
        {
            rocketnet_platform_thread_sleep( thread, 0.001 );
        }
        iocp->test_active_tx_batches.fetch_add( 1, std::memory_order_acq_rel );
        if ( !iocp->test_hold_tx.load( std::memory_order_acquire ) ||
             iocp->stopping.load( std::memory_order_acquire ) )
        {
            break;
        }
        iocp->test_active_tx_batches.fetch_sub( 1, std::memory_order_release );
    }
#endif // #if ROCKETNET_DEV

    // proofs/tla/RNBankFifoLayout.tla Take: the ready head; Release: the bank back on the free stack after its sends.
    rocketnet_platform_mutex_acquire( &lane->send_mutex );

    int index = -1;

    if ( lane->send_ready_count )
    {
        index = lane->send_ready[lane->send_ready_head];
        lane->send_ready_head = ( lane->send_ready_head + 1 ) % ROCKETNET_IOCP_SEND_BANKS;
        lane->send_ready_count--;
    }

    const bool finish = index < 0 && iocp->stopping.load( std::memory_order_acquire );
    rocketnet_platform_mutex_release( &lane->send_mutex );

    if ( index >= 0 )
    {
        rocketnet_iocp_send_bank( iocp, lane, &lane->send_banks[index] );
        rocketnet_platform_mutex_acquire( &lane->send_mutex );
        lane->send_free[lane->send_free_count++] = index;
        rocketnet_platform_mutex_release( &lane->send_mutex );
#if ROCKETNET_DEV
        iocp->test_pending_tx_banks.fetch_sub( 1, std::memory_order_release );
#endif // #if ROCKETNET_DEV
    }
    else if ( finish )
    {
        // Stop's release transferred the last unpublished producer. It
        // contains no kernel-owned sends; post those frames outside the
        // mutex before advertising that no later post is possible.
        rocketnet_iocp_send_bank( iocp, lane, &lane->send_banks[lane->send_producer] );
    }
#if ROCKETNET_DEV
    iocp->test_active_tx_batches.fetch_sub( 1, std::memory_order_release );
#endif // #if ROCKETNET_DEV
    return finish ? 2 : ( index >= 0 ? 1 : 0 );
}

// One send thread: it alone takes the ready banks of the lanes it owns (lane
// t, t + threads, ...), one bank a lane in turn, and posts them. It pins
// itself first to the core rocketnet_iocp_send_processors chose and records
// what happened: a refusal is counted, never reported as a pin.
static void rocketnet_iocp_send_worker( rocketnet_platform_thread_t * thread, void * argument )
{
    // proofs/tla/RNBufferHandoff.tla WorkerExit drains ready banks and the final producer.
    rocketnet_iocp_send_thread_t * send = (rocketnet_iocp_send_thread_t *) argument;
    rocketnet_server_iocp_t * iocp = send->owner;
    const int t = send->index;

    if ( SetThreadGroupAffinity( GetCurrentThread(), &iocp->send_pins[t], NULL ) )
    {
        iocp->send_placements[t].store( ROCKETNET_IOCP_PLACEMENT_PINNED, std::memory_order_release );
    }
    else
    {
        iocp->send_placement_errors[t].store( GetLastError(), std::memory_order_relaxed );
        iocp->send_pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        iocp->send_placements[t].store( ROCKETNET_IOCP_PLACEMENT_REFUSED, std::memory_order_release );
    }

    bool finished[ROCKETNET_IOCP_MAX_LANES] = {};
    int live = send->num_lanes;
    while ( live > 0 )
    {
        bool sent = false;
        for ( int k = 0; k < send->num_lanes; ++k )
        {
            const int l = send->lanes[k];
            if ( finished[l] )
            {
                continue;
            }
            const int turn = rocketnet_iocp_send_lane_turn( thread, iocp, iocp->lanes[l] );
            sent = sent || turn == 1;
            if ( turn == 2 )
            {
                // the completion workers exit once every lane has: wake them on
                // each lane's exit, so the last one is never missed
                finished[l] = true;
                --live;
                iocp->live_lanes.fetch_sub( 1, std::memory_order_acq_rel );
                for ( int e = 0; e < iocp->num_endpoints; ++e )
                {
                    PostQueuedCompletionStatus( iocp->ports[e], 0, 0, NULL );
                }
            }
        }
        if ( live > 0 && !sent )
        {
            rocketnet_platform_wake_wait( send->wake, false, 0.05 );
        }
    }
}

// main's views of bank slots [*cursor, end) appended to the process view,
// within the view's room
static void rocketnet_iocp_take_range( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_receive_buffer_t * buffer, uint32_t * cursor, uint32_t end )
{
    rocketnet_server_socket_process_packets_t * view = &iocp->process;
    while ( *cursor < end && view->num_packets < view->capacity )
    {
        const uint32_t source = ( *cursor )++;
        const int index = view->num_packets++;
        view->from[index] = buffer->from[source];
        view->packet_bytes[index] = buffer->packet_bytes[source];
        view->packet_data[index] = buffer->data[source];
    }
}

// proofs/tla/RNReceiveHandoff.tla Call, for each worker: the previous call's
// views have expired, so a finished tail is emptied for the worker's later
// use; then one try of the worker's mutex brings main up to what the worker
// has committed (rocketnet_receive_handoff_begin), and main returns the rest
// of the bank the worker left, then the worker's own bank up to the
// committed count, as far as the process view has room. Main never waits
// behind a worker: a failed try leaves main returning what it already held,
// and the next call catches up. The views stay valid until the next call.
// The walk starts one worker later each call, so no worker's datagrams
// always wait behind the others' when the view fills. Once stop is asked
// main takes nothing; what is still banked is counted when the workers have
// exited (rocketnet_iocp_count_stopped).
// proofs/tla/RNIocpReceiveAdmit.tla Drain: the bank's datagrams main takes, abstracted to one step.
void rocketnet_server_iocp_receive_packets( rocketnet_server_iocp_t * iocp )
{
    iocp->process.num_packets = 0;
    if ( iocp->stopping.load( std::memory_order_acquire ) || iocp->num_workers <= 0 )
    {
        return;
    }

    for ( int k = 0; k < iocp->num_workers; ++k )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[( iocp->receive_start + k ) % iocp->num_workers];
        struct rocketnet_receive_handoff_t * handoff = &worker->receive_handoff;

        if ( rocketnet_receive_handoff_tail_finished( handoff ) )
        {
            worker->buffers[handoff->tail].num_packets = 0;
        }

        if ( rocketnet_platform_mutex_try_acquire( &worker->mutex ) )
        {
            rocketnet_receive_handoff_begin( handoff );
            rocketnet_platform_mutex_release( &worker->mutex );
        }

        rocketnet_iocp_take_range( iocp, &worker->buffers[handoff->tail], &handoff->tail_cursor, handoff->tail_end );
        rocketnet_iocp_take_range( iocp, &worker->buffers[handoff->main], &handoff->main_cursor, handoff->main_limit );
    }
    iocp->receive_start = ( iocp->receive_start + 1 ) % iocp->num_workers;
}

rocketnet_server_socket_process_packets_t * rocketnet_server_iocp_process_packets( rocketnet_server_iocp_t * iocp )
{
    return &iocp->process;
}

void rocketnet_server_iocp_counters( rocketnet_server_iocp_t * iocp, uint64_t * counters )
{
    for ( int i = 0; i < ROCKETNET_SERVER_IOCP_COUNTER_ROWS; ++i )
    {
        counters[ROCKETNET_SERVER_IOCP_FIRST_COUNTER + i] = iocp->counters[i].load( std::memory_order_relaxed );
    }

    // the thread pins Windows refused, the completion workers' and the send threads'
    counters[ROCKETNET_COUNTER_IOCP_PIN_REFUSALS] =
        iocp->receive_pin_refusals.load( std::memory_order_relaxed ) + iocp->send_pin_refusals.load( std::memory_order_relaxed );
}

void rocketnet_server_iocp_set_packet_filter( rocketnet_server_iocp_t * iocp, const uint8_t * packet_filter )
{
    // release: a worker that reads the word on at its next batch reads the
    // whole value with it
    iocp->filter_state.store( rocketnet_iocp_filter_state( packet_filter ), std::memory_order_release );
}

void rocketnet_server_iocp_receive_drop_totals( rocketnet_server_iocp_t * iocp, uint64_t * values )
{
    for ( int i = 0; i < ROCKETNET_RECEIVE_NUM_DROPS; ++i )
    {
        values[i] = 0;
    }
    rocketnet_receive_drop_tally_sum( iocp->drop_tallies, iocp->num_workers, values );
}

int rocketnet_server_iocp_pending( rocketnet_server_iocp_t * iocp )
{
    return iocp->pending.load( std::memory_order_acquire );
}

int rocketnet_server_iocp_worker_placement( rocketnet_server_iocp_t * iocp, int worker, uint64_t * mask, int * group, unsigned long * error )
{
    if ( !iocp || worker < 0 || worker >= iocp->num_workers || worker >= ROCKETNET_IOCP_MAX_WORKERS )
    {
        return ROCKETNET_IOCP_PLACEMENT_PENDING;
    }
    const int placement = iocp->placements[worker].load( std::memory_order_acquire );
    if ( mask )
    {
        *mask = (uint64_t) iocp->pins[worker].Mask;
    }
    if ( group )
    {
        *group = (int) iocp->pins[worker].Group;
    }
    if ( error )
    {
        *error = iocp->placement_errors[worker].load( std::memory_order_relaxed );
    }
    return placement;
}

int rocketnet_server_iocp_send_thread_count( rocketnet_server_iocp_t * iocp )
{
    return iocp ? iocp->num_send_threads : 0;
}

int rocketnet_server_iocp_send_placement( rocketnet_server_iocp_t * iocp, int thread, uint64_t * mask, int * group, unsigned long * error )
{
    if ( !iocp || thread < 0 || thread >= iocp->num_send_threads )
    {
        return ROCKETNET_IOCP_PLACEMENT_PENDING;
    }
    const int placement = iocp->send_placements[thread].load( std::memory_order_acquire );
    if ( mask )
    {
        *mask = (uint64_t) iocp->send_pins[thread].Mask;
    }
    if ( group )
    {
        *group = (int) iocp->send_pins[thread].Group;
    }
    if ( error )
    {
        *error = iocp->send_placement_errors[thread].load( std::memory_order_relaxed );
    }
    return placement;
}

uint64_t rocketnet_server_iocp_send_pin_refusals( rocketnet_server_iocp_t * iocp )
{
    return iocp ? iocp->send_pin_refusals.load( std::memory_order_relaxed ) : 0;
}

int rocketnet_server_iocp_features( rocketnet_server_iocp_t * iocp )
{
    return iocp->features.load( std::memory_order_relaxed );
}

#if ROCKETNET_DEV
uintptr_t rocketnet_server_iocp_test_socket( rocketnet_server_iocp_t * iocp )
{
    return (uintptr_t) iocp->sockets[0];
}

void rocketnet_server_iocp_test_hold_tx( rocketnet_server_iocp_t * iocp, bool hold )
{
    iocp->test_hold_tx.store( hold, std::memory_order_release );
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        rocketnet_platform_wake_set( iocp->lanes[l]->send_wake );
    }
    if ( hold )
    {
        while ( iocp->test_active_tx_batches.load( std::memory_order_acquire ) != 0 )
        {
            rocketnet_platform_sleep( 0.001 );
        }
    }
}

bool rocketnet_server_iocp_test_tx_idle( rocketnet_server_iocp_t * iocp )
{
    // Idle: the producer is published (nothing staged) and every published
    // bank is serviced.
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        if ( iocp->lanes[l]->num_staged.load( std::memory_order_relaxed ) != 0 )
        {
            return false;
        }
    }
    return iocp->test_pending_tx_banks.load( std::memory_order_acquire ) == 0;
}

int rocketnet_server_iocp_test_ready_tx_banks( rocketnet_server_iocp_t * iocp )
{
    int count = 0;
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        rocketnet_iocp_lane_t * lane = iocp->lanes[l];
        rocketnet_platform_mutex_acquire( &lane->send_mutex );
        count += lane->send_ready_count;
        rocketnet_platform_mutex_release( &lane->send_mutex );
    }
    return count;
}

// Only the dedicated test executable compiles this helper. The gate pauses
// completion work without making production work run under a publication
// mutex. A hold returns after all earlier batches have retired.
void rocketnet_server_iocp_test_hold_completions( rocketnet_server_iocp_t * iocp, bool hold )
{
    iocp->test_hold_completions.store( hold, std::memory_order_release );
    if ( hold )
    {
        while ( iocp->test_active_completion_batches.load( std::memory_order_acquire ) != 0 )
        {
            rocketnet_platform_sleep( 0.001 );
        }
    }
}

void rocketnet_server_iocp_test_hold_publication( rocketnet_server_iocp_t * iocp, bool hold )
{
    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        if ( hold )
        {
            rocketnet_platform_mutex_acquire( &iocp->workers[i].mutex );
        }
        else
        {
            rocketnet_platform_mutex_release( &iocp->workers[i].mutex );
        }
    }
}

int rocketnet_server_iocp_test_ready_banks( rocketnet_server_iocp_t * iocp, int worker_index )
{
    rocketnet_assert( worker_index >= 0 && worker_index < iocp->num_workers );

    // main's thread: the datagrams the worker has committed that main has not
    // yet returned (the cursors are main's; the roles and counts under the mutex)
    rocketnet_iocp_worker_t * worker = &iocp->workers[worker_index];
    const struct rocketnet_receive_handoff_t * handoff = &worker->receive_handoff;
    rocketnet_platform_mutex_acquire( &worker->mutex );

    uint32_t count = handoff->tail_end - handoff->tail_cursor;
    count += handoff->committed[handoff->main] - handoff->main_cursor;
    if ( handoff->worker != handoff->main )
    {
        count += handoff->committed[handoff->worker];
    }
    rocketnet_platform_mutex_release( &worker->mutex );

    return (int) count;
}

// main's thread: the view points into a slot main has returned and still
// views until its next call: below the cursor of its tail or of its own bank
bool rocketnet_server_iocp_test_receive_view_published( rocketnet_server_iocp_t * iocp, const uint8_t * view )
{
    bool published = false;
    const uintptr_t pointer = (uintptr_t) view;

    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        const rocketnet_iocp_worker_t * worker = &iocp->workers[i];
        const struct rocketnet_receive_handoff_t * handoff = &worker->receive_handoff;
        const int banks[2] = { handoff->tail, handoff->main };
        const uint32_t cursors[2] = { handoff->tail_cursor, handoff->main_cursor };

        for ( int j = 0; j < 2; ++j )
        {
            const uintptr_t begin = (uintptr_t) worker->buffers[banks[j]].data;
            published |= pointer >= begin && pointer < begin + (uintptr_t) cursors[j] * ROCKETNET_FRAME_STRIDE;
        }
    }

    return published;
}

bool rocketnet_server_iocp_test_stopping( rocketnet_server_iocp_t * iocp )
{
    return iocp->stopping.load( std::memory_order_acquire );
}

int rocketnet_server_iocp_test_packet_bytes( rocketnet_server_iocp_t * iocp, uint64_t packet_id )
{
    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_packet_ref( iocp, packet_id, &ref ) )
    {
        return -1;
    }
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    return op ? op->packet_bytes : -1;
}

void rocketnet_server_iocp_test_queue_stats( rocketnet_server_iocp_t * iocp,
                                             rocketnet_server_iocp_test_queue_stats_t * stats )
{
    memset( stats, 0, sizeof( *stats ) );
    stats->retry_pushes = iocp->retry_receives.pushes.load( std::memory_order_relaxed );
    stats->retry_pops = iocp->retry_receives.pops.load( std::memory_order_relaxed );
    stats->queue_probes = iocp->retry_receives.probes.load( std::memory_order_relaxed );
    stats->retry_count = (int) rocketnet_iocp_queue_count( &iocp->retry_receives );
    for ( int l = 0; l < iocp->num_lanes; ++l )
    {
        rocketnet_iocp_lane_t * lane = iocp->lanes[l];
        stats->free_pushes += lane->free_sends.pushes.load( std::memory_order_relaxed );
        stats->free_pops += lane->free_sends.pops.load( std::memory_order_relaxed );
        stats->queue_probes += lane->free_sends.probes.load( std::memory_order_relaxed );
        stats->free_count += (int) rocketnet_iocp_queue_count( &lane->free_sends );
        rocketnet_platform_mutex_acquire( &lane->run_mutex );
        for ( int i = 0; i < iocp->send_queue_size; ++i )
        {
            stats->live_run_refs += lane->run_refs[i].occupied ? 1 : 0;
        }
        rocketnet_platform_mutex_release( &lane->run_mutex );
    }
}

void rocketnet_server_iocp_test_fail_next( rocketnet_server_iocp_t * iocp, int fault, int count, int status )
{
    rocketnet_assert( fault >= 0 && fault < ROCKETNET_IOCP_TEST_NUM_FAULTS && count >= 0 );
    iocp->test_fault_status[fault].store( status, std::memory_order_relaxed );
    iocp->test_faults[fault].store( count, std::memory_order_release );
}

int rocketnet_server_iocp_test_faults_left( rocketnet_server_iocp_t * iocp, int fault )
{
    return iocp->test_faults[fault].load( std::memory_order_acquire );
}

// proofs/tla/MCRNIocpRetryStopBrokenRefuseRetires.cfg, its trace driven
// through rocketnet_iocp_post_receive itself with no socket, port or worker:
// two receive operations and a retry queue of two cells, every post refused
// by the post fault seam before any Winsock call. Both operations wait on
// the retry queue; one worker's pop claims the first cell and stalls before
// its sequence store; a second worker takes the other operation, whose
// repost is refused again, and post_receive pushes it onto the cell the
// stalled pop still holds. Reports that operation's state after the post,
// the pending count, the refused posts counted and the retries queued.
bool rocketnet_server_iocp_test_post_receive_retries_refused_push( int * state_out, int * pending_out, uint64_t * repost_failed_out,
                                                                   int * queued_out )
{
    void * storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_iocp_t ) );
    void * operations = rocketnet_malloc( NULL, 2 * sizeof( rocketnet_iocp_operation_t ) );
    if ( !storage || !operations )
    {
        rocketnet_free( NULL, storage );
        rocketnet_free( NULL, operations );
        return false;
    }

    rocketnet_server_iocp_t * iocp = new ( storage ) rocketnet_server_iocp_t();
    alignas( rocketnet_iocp_ref_cell_t ) uint8_t cells[2 * sizeof( rocketnet_iocp_ref_cell_t )];
    rocketnet_iocp_queue_initialize( &iocp->retry_receives, cells, 2 );
    iocp->num_workers = 1;
    iocp->owner_epoch = 1;
    iocp->receives = (rocketnet_iocp_operation_t *) operations;
    for ( int i = 0; i < 2; ++i )
    {
        rocketnet_iocp_operation_t * op = new ( &iocp->receives[i] ) rocketnet_iocp_operation_t();
        op->index = i;
        op->owner_epoch = iocp->owner_epoch;
        op->state = ROCKETNET_IOCP_OPERATION_IDLE;
    }

    rocketnet_server_iocp_test_fail_next( iocp, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST, 3, WSAENOBUFS );
    bool valid = !rocketnet_iocp_post_receive( iocp, &iocp->receives[0] ) && !rocketnet_iocp_post_receive( iocp, &iocp->receives[1] );

    iocp->retry_receives.stall_next_pop = true;

    rocketnet_iocp_ref_t stalled = {};
    rocketnet_iocp_ref_t taken = {};
    valid = valid && rocketnet_iocp_queue_pop( &iocp->retry_receives, &stalled ) && rocketnet_iocp_queue_pop( &iocp->retry_receives, &taken );

    rocketnet_iocp_operation_t * op = valid ? rocketnet_iocp_receive_from_ref( iocp, taken, ROCKETNET_IOCP_OPERATION_RETRY ) : NULL;
    valid = valid && op && !rocketnet_iocp_post_receive( iocp, op );

    *state_out = op ? op->state : -1;
    *pending_out = iocp->pending.load( std::memory_order_relaxed );
    *repost_failed_out = iocp->counters[ROCKETNET_COUNTER_IOCP_RECEIVE_REPOST_FAILED - ROCKETNET_SERVER_IOCP_FIRST_COUNTER].load( std::memory_order_relaxed );
    *queued_out = (int) rocketnet_iocp_queue_count( &iocp->retry_receives );

    for ( int i = 0; i < 2; ++i )
    {
        iocp->receives[i].~rocketnet_iocp_operation_t();
    }
    iocp->~rocketnet_server_iocp_t();
    rocketnet_free( NULL, operations );
    rocketnet_free( NULL, storage );

    return valid;
}

// The stop race on a receive post (A01 of the v12 review): worker B,
// retrying a receive endpoint A owns, is admitted to post it; then stop is
// published and A runs its stop pass; then B's post reaches the kernel.
// Stepped on this thread with no socket and no completion port: posts are
// accepted without a socket and the case delivers the completion of the
// cancelled receive.
struct rocketnet_iocp_stop_race_test_t
{
    int endpoint;            // the owner whose stop pass runs at the admitted point
    int exit_at_admitted;    // 1 when that pass let the owner exit
};

static void rocketnet_iocp_stop_race_test_admitted( rocketnet_server_iocp_t * iocp, void * context )
{
    rocketnet_iocp_stop_race_test_t * test = (rocketnet_iocp_stop_race_test_t *) context;
    rocketnet_server_iocp_stop( iocp );
    test->exit_at_admitted = rocketnet_iocp_worker_stop_pass( iocp, test->endpoint ) ? 1 : 0;
}

// mode 0: the race above. mode 1: stop is published before B's post begins.
// mode 2: the receive's generation is exhausted (no stop). Reports whether
// the owner's stop pass let it exit at the admitted point (mode 0), whether
// the post returned true, the receive's state and pending after the post,
// and whether the owner may exit once the cancelled receive has completed.
bool rocketnet_server_iocp_test_stop_post_race( int mode, int * exit_at_admitted_out, int * posted_out, int * state_out,
                                                int * pending_out, int * exit_after_out )
{
    if ( mode < 0 || mode > 2 || !exit_at_admitted_out || !posted_out || !state_out || !pending_out || !exit_after_out )
    {
        return false;
    }
    const int endpoints = 2;
    const int count = endpoints * ROCKETNET_IOCP_RECEIVES_PER_WORKER;
    void * storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_iocp_t ) );
    void * operations = rocketnet_malloc( NULL, (size_t) count * sizeof( rocketnet_iocp_operation_t ) );
    if ( !storage || !operations )
    {
        rocketnet_free( NULL, storage );
        rocketnet_free( NULL, operations );
        return false;
    }

    rocketnet_server_iocp_t * iocp = new ( storage ) rocketnet_server_iocp_t();
    alignas( rocketnet_iocp_ref_cell_t ) uint8_t cells[2 * sizeof( rocketnet_iocp_ref_cell_t )];
    rocketnet_iocp_queue_initialize( &iocp->retry_receives, cells, 2 );
    iocp->num_endpoints = endpoints;
    iocp->num_workers = endpoints;
    iocp->owner_epoch = 1;
    for ( int e = 0; e < ROCKETNET_IOCP_MAX_WORKERS; ++e )
    {
        iocp->sockets[e] = INVALID_SOCKET;
    }
    iocp->receives = (rocketnet_iocp_operation_t *) operations;
    for ( int i = 0; i < count; ++i )
    {
        rocketnet_iocp_operation_t * op = new ( &iocp->receives[i] ) rocketnet_iocp_operation_t();
        op->index = i;
        op->owner_epoch = iocp->owner_epoch;
        op->state = ROCKETNET_IOCP_OPERATION_IDLE;
    }
    rocketnet_iocp_test_accept_posts = true;

    // worker B (endpoint 1) took endpoint 0's receive from the retry queue
    rocketnet_iocp_operation_t * op = &iocp->receives[0];
    op->state = ROCKETNET_IOCP_OPERATION_RETRY;
    rocketnet_iocp_stop_race_test_t test = { 0, 0 };
    if ( mode == 0 )
    {
        rocketnet_iocp_test_post_admitted = rocketnet_iocp_stop_race_test_admitted;
        rocketnet_iocp_test_post_admitted_context = &test;
    }
    else if ( mode == 1 )
    {
        rocketnet_server_iocp_stop( iocp );
    }
    else
    {
        op->generation = UINT64_MAX;
    }

    const bool posted = rocketnet_iocp_post_receive( iocp, op );
    rocketnet_iocp_test_post_admitted = NULL;
    rocketnet_iocp_test_post_admitted_context = NULL;
    rocketnet_iocp_test_accept_posts = false;
    *exit_at_admitted_out = test.exit_at_admitted;
    *posted_out = posted ? 1 : 0;
    *state_out = op->state;
    *pending_out = iocp->pending.load( std::memory_order_relaxed );

    // the owner's later pass cancels the receive and its completion arrives
    // on the owner's port: dequeued, it is no longer pending
    if ( posted )
    {
        iocp->pending.fetch_sub( 1, std::memory_order_release );
        op->state = ROCKETNET_IOCP_OPERATION_IDLE;
    }
    if ( mode == 2 )
    {
        rocketnet_server_iocp_stop( iocp );
    }
    *exit_after_out = rocketnet_iocp_worker_stop_pass( iocp, 0 ) ? 1 : 0;

    for ( int i = 0; i < count; ++i )
    {
        iocp->receives[i].~rocketnet_iocp_operation_t();
    }
    iocp->~rocketnet_server_iocp_t();
    rocketnet_free( NULL, operations );
    rocketnet_free( NULL, storage );

    return true;
}

// The receive fixture: a transport of workers completion workers with their
// banks (ROCKETNET_IOCP_TEST_BANK datagrams each), their mutexes, drop
// tallies and a process view of ROCKETNET_IOCP_TEST_VIEW, but no socket, no
// port and no thread. The seams below step the real worker banking, the real
// end of a completion batch and main's real receive call on this thread.
#define ROCKETNET_IOCP_TEST_BANK 16
#define ROCKETNET_IOCP_TEST_VIEW 64

struct rocketnet_iocp_receive_fixture_t
{
    rocketnet_server_iocp_t * iocp;
    void * storage;
    void * worker_storage;
    void * tally_storage;
    uint8_t * arrays;
};

static void rocketnet_iocp_receive_fixture_free( rocketnet_iocp_receive_fixture_t * fixture )
{
    rocketnet_server_iocp_t * iocp = fixture->iocp;
    if ( iocp )
    {
        for ( int w = 0; w < iocp->num_workers; ++w )
        {
            if ( iocp->workers[w].mutex.ok )
            {
                rocketnet_platform_mutex_destroy( &iocp->workers[w].mutex );
            }
            iocp->workers[w].~rocketnet_iocp_worker_t();
        }
        iocp->~rocketnet_server_iocp_t();
    }
    rocketnet_free( NULL, fixture->arrays );
    rocketnet_free( NULL, fixture->tally_storage );
    rocketnet_free( NULL, fixture->worker_storage );
    rocketnet_free( NULL, fixture->storage );
    memset( fixture, 0, sizeof( *fixture ) );
}

static bool rocketnet_iocp_receive_fixture( rocketnet_iocp_receive_fixture_t * fixture, int workers, int bank, int view )
{
    memset( fixture, 0, sizeof( *fixture ) );
    if ( workers < 1 || workers > ROCKETNET_IOCP_MAX_WORKERS || bank < 1 || view < 1 )
    {
        return false;
    }
    const size_t bank_bytes = (size_t) bank * ( ROCKETNET_FRAME_STRIDE + sizeof( rocketnet_address_t ) + sizeof( int ) );
    const size_t view_bytes = (size_t) view * ( sizeof( rocketnet_address_t ) + sizeof( int ) + sizeof( uint8_t * ) );
    fixture->storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_iocp_t ) );
    fixture->worker_storage = rocketnet_malloc( NULL, sizeof( rocketnet_iocp_worker_t ) * (size_t) workers );
    fixture->tally_storage = rocketnet_malloc( NULL, sizeof( rocketnet_receive_drop_tally_t ) * (size_t) workers );
    fixture->arrays = (uint8_t *) rocketnet_malloc( NULL, bank_bytes * ROCKETNET_IOCP_RECEIVE_BANKS * (size_t) workers + view_bytes );
    if ( !fixture->storage || !fixture->worker_storage || !fixture->tally_storage || !fixture->arrays )
    {
        rocketnet_iocp_receive_fixture_free( fixture );
        return false;
    }

    rocketnet_server_iocp_t * iocp = new ( fixture->storage ) rocketnet_server_iocp_t();
    fixture->iocp = iocp;
    iocp->num_workers = workers;
    iocp->workers = (rocketnet_iocp_worker_t *) fixture->worker_storage;
    iocp->drop_tallies = (rocketnet_receive_drop_tally_t *) fixture->tally_storage;
    uint8_t * p = fixture->arrays;
    bool valid = true;
    for ( int w = 0; w < workers; ++w )
    {
        rocketnet_iocp_worker_t * worker = new ( &iocp->workers[w] ) rocketnet_iocp_worker_t();
        worker->owner = iocp;
        rocketnet_receive_handoff_init( &worker->receive_handoff );
        new ( &iocp->drop_tallies[w] ) rocketnet_receive_drop_tally_t();
        rocketnet_receive_drop_tally_reset( &iocp->drop_tallies[w] );
        for ( int b = 0; b < ROCKETNET_IOCP_RECEIVE_BANKS; ++b )
        {
            rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[b];
            buffer->data = (uint8_t( * )[ROCKETNET_FRAME_STRIDE]) p;
            p += (size_t) bank * ROCKETNET_FRAME_STRIDE;
            buffer->from = (rocketnet_address_t *) p;
            p += (size_t) bank * sizeof( rocketnet_address_t );
            buffer->packet_bytes = (int *) p;
            p += (size_t) bank * sizeof( int );
            buffer->num_packets = 0;
            buffer->capacity = bank;
        }
        valid = rocketnet_platform_mutex_create( &worker->mutex ) && valid;
    }
    iocp->process.num_packets = 0;
    iocp->process.capacity = view;
    iocp->process.from = (rocketnet_address_t *) p;
    p += (size_t) view * sizeof( rocketnet_address_t );
    iocp->process.packet_bytes = (int *) p;
    p += (size_t) view * sizeof( int );
    iocp->process.packet_data = (uint8_t **) p;

    if ( !valid )
    {
        rocketnet_iocp_receive_fixture_free( fixture );
    }
    return valid;
}

// one completion batch of worker w: datagrams first .. first + count - 1, each
// carrying its id in its first bytes, banked as the worker loop banks them,
// then the batch ended as the worker loop ends it
static void rocketnet_iocp_test_batch( rocketnet_server_iocp_t * iocp, int w, uint32_t first, int count )
{
    rocketnet_iocp_worker_t * worker = &iocp->workers[w];
    rocketnet_iocp_batch_t batch;
    batch.filter_state = 0;
    memset( batch.tally, 0, sizeof( batch.tally ) );
    batch.drops = &iocp->drop_tallies[w];
    batch.worker = worker;
    batch.held = false;

    rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );
    from.type = ROCKETNET_ADDRESS_IPV4;
    from.data.ipv4[0] = 127;
    from.data.ipv4[3] = 1;
    from.port = (uint16_t) ( 40000 + w );

    for ( int i = 0; i < count; ++i )
    {
        uint8_t datagram[ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES] = {};
        const uint32_t id = first + (uint32_t) i;
        uint8_t * stamp = datagram;
        rocketnet_write_uint32( &stamp, id );
        rocketnet_iocp_bank_datagram( &batch, from, datagram, (int) sizeof( datagram ) );
    }

    rocketnet_iocp_batch_counters( iocp, &batch );
    rocketnet_iocp_publish_receive_bank( worker );
}

// the ids main's last receive call returned, in the view's order
static int rocketnet_iocp_test_view_ids( rocketnet_server_iocp_t * iocp, uint32_t * ids, int capacity )
{
    const rocketnet_server_socket_process_packets_t * view = &iocp->process;
    if ( view->num_packets > capacity )
    {
        return -1;
    }
    for ( int i = 0; i < view->num_packets; ++i )
    {
        const uint8_t * stamp = view->packet_data[i];
        ids[i] = rocketnet_read_uint32( &stamp );
    }
    return view->num_packets;
}

struct rocketnet_iocp_delivery_test_t
{
    rocketnet_iocp_receive_fixture_t fixture;
    int workers;
    int passes;
};

// one worker pass of the script is one completion batch; the passes are
// dealt to the workers in turn
static bool rocketnet_iocp_delivery_test_pass( void * user, uint32_t first, int count )
{
    rocketnet_iocp_delivery_test_t * test = (rocketnet_iocp_delivery_test_t *) user;
    rocketnet_iocp_test_batch( test->fixture.iocp, test->passes++ % test->workers, first, count );
    return true;
}

// one receive call of main. With several workers the order across workers is
// the walk's, not the arrival's (each worker's own datagrams stay in order),
// so the ids are sorted before the check compares them.
static int rocketnet_iocp_delivery_test_call( void * user, uint32_t * ids, int capacity )
{
    rocketnet_iocp_delivery_test_t * test = (rocketnet_iocp_delivery_test_t *) user;
    rocketnet_server_iocp_receive_packets( test->fixture.iocp );
    const int returned = rocketnet_iocp_test_view_ids( test->fixture.iocp, ids, capacity );
    for ( int i = 1; i < returned; ++i )
    {
        const uint32_t id = ids[i];
        int j = i;
        while ( j > 0 && ids[j - 1] > id )
        {
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = id;
    }
    return returned;
}

static bool rocketnet_iocp_delivery_test_reset( void * user )
{
    rocketnet_iocp_delivery_test_t * test = (rocketnet_iocp_delivery_test_t *) user;
    rocketnet_iocp_receive_fixture_free( &test->fixture );
    test->passes = 0;
    return rocketnet_iocp_receive_fixture( &test->fixture, test->workers, ROCKETNET_IOCP_TEST_BANK, ROCKETNET_IOCP_TEST_VIEW );
}

// proofs/tla/RNReceiveHandoff.tla DeliveredByNextCall on the iocp transport:
// every receive call returns every datagram a completion batch banked before
// the call began, over every shared script, with workers completion workers
// (1 to 4) and no socket, no port, no thread and no clock.
bool rocketnet_server_iocp_test_receive_delivery( int workers )
{
    if ( workers < 1 || workers > 4 )
    {
        return false;
    }
    rocketnet_iocp_delivery_test_t test;
    memset( &test, 0, sizeof( test ) );
    test.workers = workers;
    const struct rocketnet_receive_delivery_rig_t rig = { &test, rocketnet_iocp_delivery_test_pass, rocketnet_iocp_delivery_test_call };
    const char * name = workers == 1 ? "iocp server" : "iocp server, several workers";
    bool valid = rocketnet_receive_delivery_check_all( &rig, name, rocketnet_iocp_delivery_test_reset );

    // with the passes dealt in turn, each worker meets a shared script's run
    // of passes between calls only in part: this run deals sixteen passes
    // before a call, four or more to every worker
    if ( workers > 1 )
    {
        valid = rocketnet_iocp_delivery_test_reset( &test ) &&
                rocketnet_receive_delivery_check( &rig, name, "w1 w1 w1 w1 w1 w1 w1 w1 w1 w1 w1 w1 w1 w1 w1 w1 m w2 w2 m" ) && valid;
    }
    rocketnet_iocp_receive_fixture_free( &test.fixture );
    return valid;
}

// The walk's rotation, with no socket, no port, no thread and no clock: two
// workers, banks and a process view of 8 datagrams. Each frame worker 0
// completes a batch of 8 and worker 1 a batch of 1, then main's call runs.
// *longest_wait is the most calls in a row that ended with worker 1 holding
// committed datagrams main had not returned, and *dropped the datagrams the
// workers refused BANK_FULL. frames is at most 32.
bool rocketnet_server_iocp_test_receive_rotation( int frames, int * longest_wait, uint64_t * dropped )
{
    if ( frames < 1 || frames > 32 || !longest_wait || !dropped )
    {
        return false;
    }
    rocketnet_iocp_receive_fixture_t fixture;
    if ( !rocketnet_iocp_receive_fixture( &fixture, 2, 8, 8 ) )
    {
        return false;
    }
    rocketnet_server_iocp_t * iocp = fixture.iocp;

    uint32_t next = 1;
    int wait = 0;
    *longest_wait = 0;
    for ( int f = 0; f < frames; ++f )
    {
        rocketnet_iocp_test_batch( iocp, 0, next, 8 );
        next += 8;
        rocketnet_iocp_test_batch( iocp, 1, next, 1 );
        next += 1;
        rocketnet_server_iocp_receive_packets( iocp );
        wait = rocketnet_server_iocp_test_ready_banks( iocp, 1 ) > 0 ? wait + 1 : 0;
        *longest_wait = wait > *longest_wait ? wait : *longest_wait;
    }

    *dropped = iocp->drop_tallies[0].values[ROCKETNET_RECEIVE_DROP_BANK_FULL].load() + iocp->drop_tallies[1].values[ROCKETNET_RECEIVE_DROP_BANK_FULL].load();
    rocketnet_iocp_receive_fixture_free( &fixture );
    return true;
}
#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

int rocketnet_server_iocp_windows_cpp_dummy = 0;

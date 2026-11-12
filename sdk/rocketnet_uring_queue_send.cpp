/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_uring_queue_send.h"
#include "rocketnet_internal.h"
#include "rocketnet_memory.h"

#include <errno.h>
#include <inttypes.h>
#include <new>
#include <string.h>

// proofs/tla/RNUringQueueSend.tla models one queue of this code: Publish,
// WorkerClaim, WorkerSubmit, WorkerReap and WorkerRetire are cited at the
// functions that are them, and KernelComplete is the kernel's (the trace
// walk's fake one, tests/tla_trace_queue_send.cpp). The model's run is a set
// of staged slots of one destination, the earliest of them first; here the
// gather cuts the runs (proofs/tla/RNSendGatherLayout.tla) and next hands
// them out by order position, so a destination's runs leave in staged
// order, and a run handed back is the earliest of its destination again.
// ROCKETNET_URING_QUEUE_SEND_PLANT is the trace check's plant (the reversed
// witness RetireAtSubmit: retire once every slot is submitted, runs still in
// the kernel): only tests/tla_trace_queue_send.cpp in the plant build
// defines it, and its traces must fail TraceConsistent.

struct rocketnet_uring_queue_send_layout_t
{
    uint64_t queue;
    uint64_t state;
    uint64_t order;
    uint64_t entry;
    uint64_t runs;
    uint64_t scratch;
    uint64_t scratch_bytes;
    uint64_t bytes;
};

static bool rocketnet_uring_queue_send_layout( int num_queues, int slots, rocketnet_uring_queue_send_layout_t * layout )
{
    const uint64_t cells = (uint64_t) num_queues * ROCKETNET_URING_SEND_BANKS * (uint64_t) slots;
    layout->scratch_bytes = (uint64_t) rocketnet_send_gather_scratch_bytes( slots );
    rocketnet_carve_t carve = { 0, false };
    rocketnet_carve( carve, 1, sizeof( struct rocketnet_uring_queue_send_t ) );
    layout->queue = rocketnet_carve( carve, (uint64_t) num_queues, sizeof( struct rocketnet_uring_queue_send_queue_t ) );
    layout->state = rocketnet_carve( carve, cells, sizeof( uint8_t ) );
    layout->order = rocketnet_carve( carve, cells, sizeof( int ) );
    layout->entry = rocketnet_carve( carve, cells, sizeof( struct rocketnet_uring_queue_send_entry_t ) );
    layout->runs = rocketnet_carve( carve, (uint64_t) num_queues * (uint64_t) slots, sizeof( struct rocketnet_send_gather_run_t ) );
    layout->scratch = rocketnet_carve( carve, (uint64_t) num_queues, layout->scratch_bytes );
    layout->bytes = carve.bytes;
    return !carve.failed && layout->scratch_bytes != 0 && carve.bytes <= SIZE_MAX;
}

struct rocketnet_uring_queue_send_t * rocketnet_uring_queue_send_create( void * context, struct rocketnet_uring_send_queues_t * queues, int max_segments, int max_bytes )
{
    rocketnet_assert( queues );

    if ( max_segments < 1 || max_bytes < 1 )
    {
        rocketnet_error( "io_uring queue send: max_segments %d and max_bytes %d must be at least 1", max_segments, max_bytes );
        return NULL;
    }

    rocketnet_uring_queue_send_layout_t layout;
    if ( !rocketnet_uring_queue_send_layout( queues->num_queues, queues->slots, &layout ) )
    {
        rocketnet_error( "io_uring queue send: the layout overflows" );
        return NULL;
    }

    uint8_t * base = (uint8_t *) rocketnet_malloc( context, (size_t) layout.bytes );
    if ( !base )
    {
        rocketnet_error( "io_uring queue send: could not allocate %" PRIu64 " bytes", layout.bytes );
        return NULL;
    }

    memset( base, 0, (size_t) layout.bytes );

    struct rocketnet_uring_queue_send_t * send = new ( base ) rocketnet_uring_queue_send_t();
    send->context = context;
    send->queues = queues;
    send->num_queues = queues->num_queues;
    send->slots = queues->slots;
    send->max_segments = max_segments;
    send->max_bytes = max_bytes;
    send->queue = (struct rocketnet_uring_queue_send_queue_t *) ( base + layout.queue );
    send->memory = base;

    const size_t cells = (size_t) ROCKETNET_URING_SEND_BANKS * (size_t) send->slots;
    int created = 0;
    bool ok = true;
    for ( int q = 0; q < send->num_queues; q++ )
    {
        struct rocketnet_uring_queue_send_queue_t * queue = new ( &send->queue[q] ) rocketnet_uring_queue_send_queue_t();
        rocketnet_buffer_handoff_init( &queue->handoff, queues->queue[q].producer );
        queue->held = -1;
        queue->state = (uint8_t *) ( base + layout.state ) + (size_t) q * cells;
        queue->order = (int *) ( base + layout.order ) + (size_t) q * cells;
        queue->entry = (struct rocketnet_uring_queue_send_entry_t *) ( base + layout.entry ) + (size_t) q * cells;
        queue->runs = (struct rocketnet_send_gather_run_t *) ( base + layout.runs ) + (size_t) q * (size_t) send->slots;
        void * scratch = base + layout.scratch + (uint64_t) q * layout.scratch_bytes;
        ok = ok && rocketnet_send_gather_scratch_init( &queue->scratch, scratch, send->slots );
        for ( int c = 0; c < ROCKETNET_URING_QUEUE_SEND_NUM_COUNTERS; c++ )
        {
            queue->counters[c].store( 0, std::memory_order_relaxed );
        }
        if ( ok && rocketnet_platform_mutex_create( &queue->mutex ) )
        {
            created++;
        }
        else
        {
            ok = false;
        }
    }

    if ( !ok )
    {
        rocketnet_error( "io_uring queue send: could not create the handoff of %d queues", send->num_queues );
        for ( int q = 0; q < created; q++ )
        {
            rocketnet_platform_mutex_destroy( &send->queue[q].mutex );
        }
        for ( int q = 0; q < send->num_queues; q++ )
        {
            send->queue[q].~rocketnet_uring_queue_send_queue_t();
        }
        send->~rocketnet_uring_queue_send_t();
        rocketnet_free( context, base );
        return NULL;
    }

    return send;
}

void rocketnet_uring_queue_send_destroy( struct rocketnet_uring_queue_send_t * send )
{
    if ( !send )
    {
        return;
    }

    void * context = send->context;
    void * memory = send->memory;

    for ( int q = 0; q < send->num_queues; q++ )
    {
        rocketnet_platform_mutex_destroy( &send->queue[q].mutex );
        send->queue[q].~rocketnet_uring_queue_send_queue_t();
    }

    send->~rocketnet_uring_queue_send_t();

    rocketnet_free( context, memory );
}

static inline void rocketnet_uring_queue_send_count( struct rocketnet_uring_queue_send_queue_t * queue, int counter, uint64_t amount )
{
    queue->counters[counter].store( queue->counters[counter].load( std::memory_order_relaxed ) + amount, std::memory_order_relaxed );
}

// RNUringQueueSend Publish: the producer goes to the ready FIFO only when it
// staged anything and a free bank replaces it; the replacement is reset
// before any fill sees it (L0's turn_to, outside the mutex: no one else
// holds that bank)

bool rocketnet_uring_queue_send_publish( struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_send_queue_t * banks = &send->queues->queue[queue_index];
    if ( banks->bank[banks->producer].num_packets.load( std::memory_order_acquire ) == 0 )
    {
        return false;
    }

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    rocketnet_platform_mutex_acquire( &queue->mutex );
    const int next = rocketnet_buffer_handoff_publish( &queue->handoff, banks->producer, ROCKETNET_URING_SEND_BANKS - 1 );
    rocketnet_platform_mutex_release( &queue->mutex );

    if ( next < 0 )
    {
        return false;
    }

    rocketnet_uring_send_queues_turn_to( send->queues, queue_index, next );
    return true;
}

bool rocketnet_uring_queue_send_idle( struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_send_queue_t * banks = &send->queues->queue[queue_index];
    if ( banks->bank[banks->producer].num_packets.load( std::memory_order_acquire ) != 0 )
    {
        return false;
    }

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    rocketnet_platform_mutex_acquire( &queue->mutex );
    const bool idle = queue->handoff.ready_count == 0 && queue->held < 0;
    rocketnet_platform_mutex_release( &queue->mutex );
    return idle;
}

// RNUringQueueSend WorkerClaim: the oldest ready bank. Its staged slots are
// the first min(count, slots) (L0 StagedMatches); a finished one (bytes > 0)
// is staged and an aborted one is in no run. The gather groups the staged
// slots by destination in staged order and cuts the runs, each kept at its
// first slot until its completion.

int rocketnet_uring_queue_send_claim( struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    if ( queue->held >= 0 )
    {
        return -1;
    }

    rocketnet_platform_mutex_acquire( &queue->mutex );
    const int b = rocketnet_buffer_handoff_take( &queue->handoff );
    queue->held = b;
    rocketnet_platform_mutex_release( &queue->mutex );

    if ( b < 0 )
    {
        return -1;
    }

    const int slots = send->slots;
    const struct rocketnet_uring_send_bank_t * bank = &send->queues->queue[queue_index].bank[b];
    const uint64_t claims = bank->num_packets.load( std::memory_order_acquire );
    const int staged = claims < (uint64_t) slots ? (int) claims : slots;

    uint8_t * state = queue->state + (size_t) b * (size_t) slots;
    int * order = queue->order + (size_t) b * (size_t) slots;
    struct rocketnet_uring_queue_send_entry_t * entry = queue->entry + (size_t) b * (size_t) slots;

    for ( int s = 0; s < slots; s++ )
    {
        state[s] = ROCKETNET_URING_QUEUE_SEND_SLOT_EMPTY;
        entry[s].count = 0;
        entry[s].kernel = false;
        entry[s].plain = false;
    }

    int ordered = 0;
    for ( int s = 0; s < staged; s++ )
    {
        const bool finished = bank->slot[s].packet_bytes > 0;
        state[s] = finished ? ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED : ROCKETNET_URING_QUEUE_SEND_SLOT_ABORTED;
        ordered += finished ? 1 : 0;
    }

    int num_runs = 0;
    if ( staged > 0 )
    {
        num_runs = rocketnet_send_gather( &bank->slot[0].to, sizeof( bank->slot[0] ), &bank->slot[0].packet_bytes, sizeof( bank->slot[0] ), staged, send->max_segments, send->max_bytes,
                                          &queue->scratch, order, queue->runs );
        rocketnet_assert( num_runs >= 0 );
    }

    int placed = 0;
    for ( int r = 0; r < num_runs; r++ )
    {
        const struct rocketnet_send_gather_run_t * run = &queue->runs[r];
        struct rocketnet_uring_queue_send_entry_t * at = &entry[order[run->first]];
        at->count = run->count;
        at->first = run->first;
        at->segment_bytes = run->segment_bytes;
        at->last_bytes = run->last_bytes;
        placed += run->count;
    }
    rocketnet_assert( placed == ordered );

    queue->ordered = ordered;
    queue->unsubmitted = placed;
    queue->cursor = 0;
    return b;
}

int rocketnet_uring_queue_send_held( const struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );
    return send->queue[queue_index].held;
}

// RNUringQueueSend WorkerSubmit(d, k): the run at the lowest order position
// that waits, its slots now the kernel's and one more run of the bank
// pending

bool rocketnet_uring_queue_send_next( struct rocketnet_uring_queue_send_t * send, int queue_index, struct rocketnet_uring_queue_send_run_t * run )
{
    rocketnet_assert( send );
    rocketnet_assert( run );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    const int b = queue->held;
    if ( b < 0 || queue->unsubmitted == 0 )
    {
        return false;
    }

    const int slots = send->slots;
    uint8_t * state = queue->state + (size_t) b * (size_t) slots;
    const int * order = queue->order + (size_t) b * (size_t) slots;
    struct rocketnet_uring_queue_send_entry_t * entry = queue->entry + (size_t) b * (size_t) slots;

    int position = queue->cursor;
    while ( position < queue->ordered )
    {
        const int slot = order[position];
        struct rocketnet_uring_queue_send_entry_t * at = &entry[slot];
        if ( at->count == 0 )
        {
            position++;
            continue;
        }

        if ( at->kernel || state[slot] != ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED )
        {
            position += at->count;
            continue;
        }

        at->kernel = true;
        for ( int i = 0; i < at->count; i++ )
        {
            state[order[at->first + i]] = ROCKETNET_URING_QUEUE_SEND_SLOT_RUN;
        }
        queue->pending[b]++;
        queue->unsubmitted -= at->count;
        queue->cursor = position + at->count;

        run->bank = b;
        run->slot = slot;
        run->first = at->first;
        run->count = at->count;
        run->segment_bytes = at->segment_bytes;
        run->last_bytes = at->last_bytes;
        run->plain = at->plain;
        run->tag = (uint32_t) ( b * slots + slot );
        if ( at->count > 1 )
        {
            rocketnet_uring_queue_send_count( queue, ROCKETNET_URING_QUEUE_SEND_COUNTER_SEGMENTED, 1 );
        }
        return true;
    }

    queue->cursor = position;
    return false;
}

int rocketnet_uring_queue_send_result( int res, bool segmented )
{
    if ( res >= 0 )
    {
        return ROCKETNET_URING_QUEUE_SEND_OK;
    }
    if ( res == -EAGAIN )
    {
        return ROCKETNET_URING_QUEUE_SEND_AGAIN;
    }
    if ( segmented && ( res == -EINVAL || res == -EIO || res == -EOPNOTSUPP ) )
    {
        return ROCKETNET_URING_QUEUE_SEND_REFUSED;
    }
    return ROCKETNET_URING_QUEUE_SEND_FAILED;
}

// RNUringQueueSend WorkerReap: one completion, by its tag. ok and failed
// finish the run's slots (sent, dropped); again hands them back to be
// submitted as the same run; refused hands them back as one run per packet,
// sent plain (RNSegmentRefusal, the refused run resent plain: the model's
// eagain hand-back, the same ownership step). A refused one-packet run is
// failed: nothing is left to split.

int rocketnet_uring_queue_send_complete( struct rocketnet_uring_queue_send_t * send, int queue_index, uint32_t tag, int result )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    const int slots = send->slots;

    if ( tag >= (uint32_t) ( ROCKETNET_URING_SEND_BANKS * slots ) )
    {
        rocketnet_uring_queue_send_count( queue, ROCKETNET_URING_QUEUE_SEND_COUNTER_STRAY, 1 );
        return -1;
    }

    const int b = (int) ( tag / (uint32_t) slots );
    const int slot = (int) ( tag % (uint32_t) slots );
    uint8_t * state = queue->state + (size_t) b * (size_t) slots;
    const int * order = queue->order + (size_t) b * (size_t) slots;
    struct rocketnet_uring_queue_send_entry_t * entry = queue->entry + (size_t) b * (size_t) slots;
    struct rocketnet_uring_queue_send_entry_t * at = &entry[slot];

    if ( at->count == 0 || !at->kernel || queue->pending[b] == 0 )
    {
        rocketnet_uring_queue_send_count( queue, ROCKETNET_URING_QUEUE_SEND_COUNTER_STRAY, 1 );
        return -1;
    }

    const int count = at->count;
    queue->pending[b]--;
    at->kernel = false;

    if ( result == ROCKETNET_URING_QUEUE_SEND_REFUSED && count == 1 )
    {
        result = ROCKETNET_URING_QUEUE_SEND_FAILED;
    }

    switch ( result )
    {
        case ROCKETNET_URING_QUEUE_SEND_OK:
        case ROCKETNET_URING_QUEUE_SEND_FAILED:
        {
            const bool ok = result == ROCKETNET_URING_QUEUE_SEND_OK;
            for ( int i = 0; i < count; i++ )
            {
                state[order[at->first + i]] = ok ? ROCKETNET_URING_QUEUE_SEND_SLOT_SENT : ROCKETNET_URING_QUEUE_SEND_SLOT_DROPPED;
            }
            at->count = 0;
            rocketnet_uring_queue_send_count( queue, ok ? ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT : ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED, (uint64_t) count );
            break;
        }

        case ROCKETNET_URING_QUEUE_SEND_AGAIN:
        {
            for ( int i = 0; i < count; i++ )
            {
                state[order[at->first + i]] = ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED;
            }
            queue->unsubmitted += count;
            if ( at->first < queue->cursor )
            {
                queue->cursor = at->first;
            }
            rocketnet_uring_queue_send_count( queue, ROCKETNET_URING_QUEUE_SEND_COUNTER_AGAIN, 1 );
            break;
        }

        default:
        {
            rocketnet_assert( result == ROCKETNET_URING_QUEUE_SEND_REFUSED );
            const int first = at->first;
            const int segment_bytes = at->segment_bytes;
            const int last_bytes = at->last_bytes;
            for ( int i = 0; i < count; i++ )
            {
                const int s = order[first + i];
                state[s] = ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED;
                struct rocketnet_uring_queue_send_entry_t * single = &entry[s];
                single->count = 1;
                single->first = first + i;
                single->segment_bytes = i + 1 < count ? segment_bytes : last_bytes;
                single->last_bytes = single->segment_bytes;
                single->kernel = false;
                single->plain = true;
            }
            queue->unsubmitted += count;
            if ( first < queue->cursor )
            {
                queue->cursor = first;
            }
            rocketnet_uring_queue_send_count( queue, ROCKETNET_URING_QUEUE_SEND_COUNTER_REFUSED, 1 );
            break;
        }
    }

    return count;
}

// RNUringQueueSend WorkerRetire, RetireRule "cqe": nothing of the bank waits
// to be submitted and none of its runs is pending

bool rocketnet_uring_queue_send_retire( struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    const int b = queue->held;
    if ( b < 0 || queue->unsubmitted != 0 )
    {
        return false;
    }

#if !defined( ROCKETNET_URING_QUEUE_SEND_PLANT )
    if ( queue->pending[b] != 0 )
    {
        return false;
    }
#endif // #if !defined( ROCKETNET_URING_QUEUE_SEND_PLANT )

    const int slots = send->slots;
    uint8_t * state = queue->state + (size_t) b * (size_t) slots;
    for ( int s = 0; s < slots; s++ )
    {
        state[s] = ROCKETNET_URING_QUEUE_SEND_SLOT_EMPTY;
    }
    queue->ordered = 0;
    queue->cursor = 0;

    rocketnet_platform_mutex_acquire( &queue->mutex );
    rocketnet_buffer_handoff_release( &queue->handoff, b );
    queue->held = -1;
    rocketnet_platform_mutex_release( &queue->mutex );
    return true;
}

int rocketnet_uring_queue_send_abandon( struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    const int b = queue->held;
    if ( b < 0 || queue->unsubmitted == 0 )
    {
        return 0;
    }

    const int slots = send->slots;
    uint8_t * state = queue->state + (size_t) b * (size_t) slots;
    const int * order = queue->order + (size_t) b * (size_t) slots;
    struct rocketnet_uring_queue_send_entry_t * entry = queue->entry + (size_t) b * (size_t) slots;

    int abandoned = 0;
    for ( int position = 0; position < queue->ordered; position++ )
    {
        const int slot = order[position];
        struct rocketnet_uring_queue_send_entry_t * at = &entry[slot];
        if ( at->count == 0 || at->kernel || state[slot] != ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED )
        {
            continue;
        }
        for ( int i = 0; i < at->count; i++ )
        {
            state[order[at->first + i]] = ROCKETNET_URING_QUEUE_SEND_SLOT_DROPPED;
        }
        abandoned += at->count;
        at->count = 0;
    }

    rocketnet_assert( abandoned == queue->unsubmitted );
    queue->unsubmitted = 0;
    rocketnet_uring_queue_send_count( queue, ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED, (uint64_t) abandoned );
    return abandoned;
}

int rocketnet_uring_queue_send_pending( const struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );
    const struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    return queue->held < 0 ? 0 : queue->pending[queue->held];
}

uint64_t rocketnet_uring_queue_send_counter( const struct rocketnet_uring_queue_send_t * send, int queue_index, int counter )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );
    rocketnet_assert( counter >= 0 && counter < ROCKETNET_URING_QUEUE_SEND_NUM_COUNTERS );
    return send->queue[queue_index].counters[counter].load( std::memory_order_relaxed );
}

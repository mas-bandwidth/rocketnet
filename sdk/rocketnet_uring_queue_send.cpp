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

// proofs/tla/RNUringQueueSend.tla models one queue of this code: Publish
// and PublishKept, WorkerClaim(b), WorkerSubmit(d, k, R), WorkerReap(n) and
// WorkerRetire and WorkerRetireRefused (WorkerStep is any of the worker's)
// are cited at the functions that are them, each by the value it returns,
// and KernelComplete is the kernel's (the trace walk's fake one,
// tests/tla_trace_queue_send.cpp). The model's run is a set
// of staged slots of one destination, the earliest of them first; here the
// gather cuts the runs (proofs/tla/RNSendGatherLayout.tla) and next hands
// them out by order position, so a destination's runs leave in staged
// order, and a run handed back is the earliest of its destination again.
// ROCKETNET_URING_QUEUE_SEND_PLANT is the trace check's plant (the reversed
// witness RetireAtSubmit: retire once every slot is submitted, runs still in
// the kernel): only tests/tla_trace_queue_send.cpp defines it, in its own
// plant binary (tla_trace_plant_queue_send), and its traces must fail
// TraceConsistent.

#if defined( __linux__ )
#include "rocketnet_uring_codec.h"
#if defined( IORING_RECV_MULTISHOT )
#define ROCKETNET_URING_QUEUE_SEND_MESSAGES 1
#endif // #if defined( IORING_RECV_MULTISHOT )
#endif // #if defined( __linux__ )

#if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )

// a run's SENDMSG, at its tag: the kernel reads the message, the name and
// the control until the run's completion

struct rocketnet_uring_queue_send_message_t
{
    struct msghdr message;
    struct sockaddr_in6 name;
    alignas( 8 ) uint8_t control[ROCKETNET_URING_GSO_CONTROL_BYTES];
};

#define ROCKETNET_URING_QUEUE_SEND_MESSAGE_BYTES sizeof( struct rocketnet_uring_queue_send_message_t )
#define ROCKETNET_URING_QUEUE_SEND_IOVEC_BYTES sizeof( struct iovec )

#else // #if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )

#define ROCKETNET_URING_QUEUE_SEND_MESSAGE_BYTES 0
#define ROCKETNET_URING_QUEUE_SEND_IOVEC_BYTES 0

#endif // #if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )

struct rocketnet_uring_queue_send_layout_t
{
    uint64_t queue;
    uint64_t state;
    uint64_t order;
    uint64_t entry;
    uint64_t runs;
    uint64_t scratch;
    uint64_t scratch_bytes;
    uint64_t message;
    uint64_t iovecs;
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
    layout->message = ROCKETNET_URING_QUEUE_SEND_MESSAGE_BYTES ? rocketnet_carve( carve, cells, ROCKETNET_URING_QUEUE_SEND_MESSAGE_BYTES ) : 0;
    layout->iovecs = ROCKETNET_URING_QUEUE_SEND_IOVEC_BYTES ? rocketnet_carve( carve, cells, ROCKETNET_URING_QUEUE_SEND_IOVEC_BYTES ) : 0;
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
        queue->max_segments = max_segments;
#if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )
        queue->message = (struct rocketnet_uring_queue_send_message_t *) ( base + layout.message ) + (size_t) q * cells;
        queue->iovecs = (struct iovec *) ( base + layout.iovecs ) + (size_t) q * cells;
#else  // #if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )
        queue->message = NULL;
        queue->iovecs = NULL;
#endif // #if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )
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

// RNUringQueueSend Publish when it returns true: the producer goes to the
// ready FIFO only when it staged anything and a free bank replaces it; the
// replacement is reset before any fill sees it (L0's turn_to, outside the
// mutex: no one else holds that bank). PublishKept when it returns false

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

// RNUringQueueSend WorkerClaim(b): the oldest ready bank, b, returned. Its staged slots are
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
        num_runs = rocketnet_send_gather( &bank->slot[0].to, sizeof( bank->slot[0] ), &bank->slot[0].packet_bytes, sizeof( bank->slot[0] ), staged, queue->max_segments, send->max_bytes,
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

// RNUringQueueSend WorkerSubmit(d, k, R): the run at the lowest order
// position that waits, handed out in run (its destination d, k slots R),
// its slots now the kernel's and one more run of the bank pending

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
        run->to = &send->queues->queue[queue_index].bank[b].slot[slot].to;
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

// a run's slots handed back as one plain run per packet, each waiting to be
// submitted again from its own order position: the refusal's hand-back
// (RNUringQueueSend WorkerReap, the eagain hand-back)

static void rocketnet_uring_queue_send_cut_plain( struct rocketnet_uring_queue_send_queue_t * queue, uint8_t * state, const int * order,
                                                  struct rocketnet_uring_queue_send_entry_t * entry, struct rocketnet_uring_queue_send_entry_t * at )
{
    const int count = at->count;
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
}

// RNUringQueueSend WorkerReap(n): one completion, by its tag; it returns n,
// the run's packets. ok and failed
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
            rocketnet_uring_queue_send_cut_plain( queue, state, order, entry, at );
            rocketnet_uring_queue_send_count( queue, ROCKETNET_URING_QUEUE_SEND_COUNTER_REFUSED, 1 );
            break;
        }
    }

    return count;
}

// RNUringQueueSend WorkerReap, refused, before the kernel saw the run: a
// segmented run handed out by next to a destination known to refuse
// segmentation goes back as one plain run per packet, with no send and no
// completion. Counted nowhere: no send was refused.

bool rocketnet_uring_queue_send_split( struct rocketnet_uring_queue_send_t * send, int queue_index, const struct rocketnet_uring_queue_send_run_t * run )
{
    rocketnet_assert( send );
    rocketnet_assert( run );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );

    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    const int slots = send->slots;
    if ( run->tag >= (uint32_t) ( ROCKETNET_URING_SEND_BANKS * slots ) )
    {
        return false;
    }

    const int b = (int) ( run->tag / (uint32_t) slots );
    const int slot = (int) ( run->tag % (uint32_t) slots );
    uint8_t * state = queue->state + (size_t) b * (size_t) slots;
    const int * order = queue->order + (size_t) b * (size_t) slots;
    struct rocketnet_uring_queue_send_entry_t * entry = queue->entry + (size_t) b * (size_t) slots;
    struct rocketnet_uring_queue_send_entry_t * at = &entry[slot];

    if ( at->count < 2 || !at->kernel || queue->pending[b] == 0 )
    {
        return false;
    }

    queue->pending[b]--;
    at->kernel = false;
    rocketnet_uring_queue_send_cut_plain( queue, state, order, entry, at );
    return true;
}

// RNUringQueueSend WorkerRetire, RetireRule "cqe", when it returns true:
// nothing of the bank waits to be submitted and none of its runs is
// pending; WorkerRetireRefused when it returns false

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

#if defined( ROCKETNET_URING_QUEUE_SEND_PLANT )
    const bool waits = false; // the plant: its runs may still be the kernel's
#else  // #if defined( ROCKETNET_URING_QUEUE_SEND_PLANT )
    const bool waits = queue->pending[b] != 0;
#endif // #if defined( ROCKETNET_URING_QUEUE_SEND_PLANT )
    if ( waits )
    {
        return false;
    }

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

void rocketnet_uring_queue_send_plain( struct rocketnet_uring_queue_send_t * send, int queue_index )
{
    rocketnet_assert( send );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );
    send->queue[queue_index].max_segments = 1;
}

// Layer L2: the run's SENDMSG over its scattered frames. The iovecs are at
// the run's order positions of its bank and the message at its tag: a run in
// the kernel owns its positions and its first slot, which no other run in
// the kernel shares (the gather's runs tile the order, a refusal's single
// runs sit at the positions and slots of the run they replace, and that run's
// completion is taken first), so building one run never writes what the
// kernel reads for another.

struct msghdr * rocketnet_uring_queue_send_message( struct rocketnet_uring_queue_send_t * send, int queue_index, const struct rocketnet_uring_queue_send_run_t * run, bool socket_ipv6 )
{
    rocketnet_assert( send );
    rocketnet_assert( run );
    rocketnet_assert( queue_index >= 0 && queue_index < send->num_queues );
    rocketnet_assert( run->bank >= 0 && run->bank < ROCKETNET_URING_SEND_BANKS );
    rocketnet_assert( run->count >= 1 && run->first >= 0 && run->first + run->count <= send->slots );

#if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )
    struct rocketnet_uring_queue_send_queue_t * queue = &send->queue[queue_index];
    const int slots = send->slots;
    const struct rocketnet_uring_send_bank_t * bank = &send->queues->queue[queue_index].bank[run->bank];
    const int * order = queue->order + (size_t) run->bank * (size_t) slots;
    struct iovec * iovecs = (struct iovec *) queue->iovecs + (size_t) run->bank * (size_t) slots;
    struct rocketnet_uring_queue_send_message_t * message = &queue->message[run->tag];

    const socklen_t namelen = rocketnet_uring_address_to_sockaddr( &bank->slot[run->slot].to, socket_ipv6, &message->name );
    if ( namelen == 0 )
    {
        return NULL;
    }

    for ( int i = 0; i < run->count; i++ )
    {
        const int slot = order[run->first + i];
        iovecs[run->first + i].iov_base = bank->data + (size_t) slot * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET;
        iovecs[run->first + i].iov_len = (size_t) bank->slot[slot].packet_bytes;
    }

    memset( &message->message, 0, sizeof( message->message ) );
    message->message.msg_name = &message->name;
    message->message.msg_namelen = namelen;
    message->message.msg_iov = &iovecs[run->first];
    message->message.msg_iovlen = (size_t) run->count;

    if ( run->count > 1 && !run->plain )
    {
        memset( message->control, 0, sizeof( message->control ) );
        message->message.msg_control = message->control;
        message->message.msg_controllen = sizeof( message->control );
        struct cmsghdr * cmsg = CMSG_FIRSTHDR( &message->message );
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN( sizeof( uint16_t ) );
        const uint16_t stride = (uint16_t) run->segment_bytes;
        memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
    }

    return &message->message;
#else  // #if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )
    (void) socket_ipv6;
    return NULL;
#endif // #if defined( ROCKETNET_URING_QUEUE_SEND_MESSAGES )
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

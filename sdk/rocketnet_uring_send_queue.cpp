/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_uring_send_queue.h"
#include "rocketnet_layers.h"
#include "rocketnet_memory.h"
#include "rocketnet_internal.h"

#include <inttypes.h>
#include <new>
#include <string.h>

// one allocation: the queues struct, the queue array, every bank's slot
// records, then every bank's frames laid end to end, bank b of queue q at
// ( q * ROCKETNET_URING_SEND_BANKS + b ) * slots * ROCKETNET_FRAME_STRIDE
// past the first: the layout proofs/tla/RNUringQueueBankLayout.tla Addr
// checks, where a pointer one byte before a bank is inside its neighbour

struct rocketnet_uring_send_queues_layout_t
{
    uint64_t queue;
    uint64_t slot;
    uint64_t data;
    uint64_t bytes;
};

static bool rocketnet_uring_send_queues_layout( int num_queues, int slots, rocketnet_uring_send_queues_layout_t * layout )
{
    const uint64_t banks = (uint64_t) num_queues * ROCKETNET_URING_SEND_BANKS;
    rocketnet_carve_t carve = { 0, false };
    rocketnet_carve( carve, 1, sizeof( struct rocketnet_uring_send_queues_t ) );
    layout->queue = rocketnet_carve( carve, (uint64_t) num_queues, sizeof( struct rocketnet_uring_send_queue_t ) );
    layout->slot = rocketnet_carve( carve, banks * (uint64_t) slots, sizeof( struct rocketnet_uring_send_slot_t ) );
    layout->data = rocketnet_carve( carve, banks * (uint64_t) slots, ROCKETNET_FRAME_STRIDE, 16 );
    layout->bytes = carve.bytes;
    return !carve.failed && carve.bytes <= SIZE_MAX;
}

struct rocketnet_uring_send_queues_t * rocketnet_uring_send_queues_create( void * context, int num_queues, int slots )
{
    if ( num_queues < 1 || num_queues > ROCKETNET_URING_SEND_MAX_QUEUES )
    {
        rocketnet_error( "io_uring send queues: %d queues is outside 1..%d", num_queues, ROCKETNET_URING_SEND_MAX_QUEUES );
        return NULL;
    }

    if ( slots < 1 || slots > ROCKETNET_URING_SEND_BANK_SLOTS )
    {
        rocketnet_error( "io_uring send queues: %d slots is outside 1..%d", slots, ROCKETNET_URING_SEND_BANK_SLOTS );
        return NULL;
    }

    rocketnet_uring_send_queues_layout_t layout;
    if ( !rocketnet_uring_send_queues_layout( num_queues, slots, &layout ) )
    {
        rocketnet_error( "io_uring send queues: the layout overflows" );
        return NULL;
    }

    uint8_t * base = (uint8_t *) rocketnet_malloc( context, (size_t) layout.bytes );
    if ( !base )
    {
        rocketnet_error( "io_uring send queues: could not allocate %" PRIu64 " bytes", layout.bytes );
        return NULL;
    }

    memset( base, 0, (size_t) layout.bytes );

    struct rocketnet_uring_send_queues_t * queues = new ( base ) rocketnet_uring_send_queues_t();
    queues->context = context;
    queues->num_queues = num_queues;
    queues->slots = slots;
    queues->packet_id.store( 0 );
    queues->refused.store( 0 );
    queues->refused_pointer.store( 0 );
    queues->queue = (struct rocketnet_uring_send_queue_t *) ( base + layout.queue );
    queues->memory = base;

    struct rocketnet_uring_send_slot_t * slot = (struct rocketnet_uring_send_slot_t *) ( base + layout.slot );
    uint8_t * data = base + layout.data;

    for ( int q = 0; q < num_queues; q++ )
    {
        struct rocketnet_uring_send_queue_t * queue = new ( &queues->queue[q] ) rocketnet_uring_send_queue_t();
        queue->producer = 0;
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            const size_t bank = (size_t) q * ROCKETNET_URING_SEND_BANKS + (size_t) b;
            queue->bank[b].num_packets.store( 0 );
            queue->bank[b].slot = slot + bank * (size_t) slots;
            queue->bank[b].data = data + bank * (size_t) slots * ROCKETNET_FRAME_STRIDE;
        }
    }

    return queues;
}

void rocketnet_uring_send_queues_destroy( struct rocketnet_uring_send_queues_t * queues )
{
    if ( !queues )
    {
        return;
    }

    void * context = queues->context;
    void * memory = queues->memory;

    for ( int q = 0; q < queues->num_queues; q++ )
    {
        queues->queue[q].~rocketnet_uring_send_queue_t();
    }

    queues->~rocketnet_uring_send_queues_t();

    rocketnet_free( context, memory );
}

// proofs/tla/RNUringQueueBankLayout.tla Start(t, d, ptr, pid), or
// StartFull(t, d, pid) when it returns NULL: the destination's queue
// (QueueOf), the id gid * Q + q (PidOf, IdRule "mul") and one claim on the
// producer bank's counter (ClaimCmp "ge", CounterWrap 0); a refused claim
// leaves the counter past slots until the bank is reset

uint8_t * rocketnet_uring_send_queues_start( struct rocketnet_uring_send_queues_t * queues, const struct rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( queues );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    const int q = rocketnet_send_queue_of( to, queues->num_queues );

    *packet_id = rocketnet_send_queue_packet_id( queues->packet_id.fetch_add( 1, std::memory_order_relaxed ), queues->num_queues, q );

    struct rocketnet_uring_send_queue_t * queue = &queues->queue[q];
    struct rocketnet_uring_send_bank_t * bank = &queue->bank[queue->producer];

    const int index = rocketnet_send_queue_claim( bank->num_packets, queues->slots, queues->refused );
    if ( index < 0 )
    {
        rocketnet_spam( "io_uring send queue %d bank full. packet dropped", q );
        return NULL;
    }

    bank->slot[index].to = *to;
    bank->slot[index].packet_bytes = 0;

    return bank->data + (size_t) index * ROCKETNET_FRAME_STRIDE;
}

// proofs/tla/RNUringQueueBankLayout.tla Finish(t, k), or FinishRefused(t, k)
// when finish or abort returns false: the queue from the id (id mod Q) and
// the slot from the pointer's offset past that queue's producer bank, tested
// before the division (SlotFrom, SlotRule "check"); a pointer that names no
// slot writes nothing

static struct rocketnet_uring_send_slot_t * rocketnet_uring_send_queues_slot( struct rocketnet_uring_send_queues_t * queues, uint64_t packet_id, const uint8_t * packet_data )
{
    const int q = rocketnet_send_queue_of_packet( packet_id, queues->num_queues );

    struct rocketnet_uring_send_queue_t * queue = &queues->queue[q];
    struct rocketnet_uring_send_bank_t * bank = &queue->bank[queue->producer];

    const int index = rocketnet_send_queue_slot( packet_data - bank->data, ROCKETNET_FRAME_STRIDE, queues->slots );
    if ( index < 0 )
    {
        queues->refused_pointer.fetch_add( 1, std::memory_order_relaxed );
        return NULL;
    }

    return &bank->slot[index];
}

bool rocketnet_uring_send_queues_finish( struct rocketnet_uring_send_queues_t * queues, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( queues );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    struct rocketnet_uring_send_slot_t * slot = rocketnet_uring_send_queues_slot( queues, packet_id, packet_data );
    if ( !slot )
    {
        return false;
    }

    slot->packet_bytes = packet_bytes;
    return true;
}

bool rocketnet_uring_send_queues_abort( struct rocketnet_uring_send_queues_t * queues, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( queues );

    if ( !packet_data )
    {
        return false;
    }

    struct rocketnet_uring_send_slot_t * slot = rocketnet_uring_send_queues_slot( queues, packet_id, packet_data );
    if ( !slot )
    {
        return false;
    }

    slot->packet_bytes = 0;
    return true;
}

// proofs/tla/RNUringQueueBankLayout.tla StagedMatches: the claimed slots of a
// producer bank are exactly the first min(counter, slots)

int rocketnet_uring_send_queues_staged( const struct rocketnet_uring_send_queues_t * queues, int queue )
{
    rocketnet_assert( queues );
    rocketnet_assert( queue >= 0 && queue < queues->num_queues );

    const struct rocketnet_uring_send_queue_t * q = &queues->queue[queue];
    const uint64_t num = q->bank[q->producer].num_packets.load( std::memory_order_acquire );
    return num < (uint64_t) queues->slots ? (int) num : queues->slots;
}

// proofs/tla/RNUringQueueBankLayout.tla Flush(n), n the queues it returns:
// every queue that staged turns its producer over to its next bank, whose
// counter is reset before any fill sees it. ROCKETNET_URING_SEND_QUEUE_PLANT is the trace check's plant
// (proofs/tla/README-TraceCheck.md): only tests/tla_trace_bank.cpp in the plant build
// defines it, and its traces must fail TraceConsistent

void rocketnet_uring_send_queues_turn_to( struct rocketnet_uring_send_queues_t * queues, int queue_index, int next )
{
    rocketnet_assert( queues );
    rocketnet_assert( queue_index >= 0 && queue_index < queues->num_queues );
    rocketnet_assert( next >= 0 && next < ROCKETNET_URING_SEND_BANKS );

    struct rocketnet_uring_send_queue_t * queue = &queues->queue[queue_index];
#if !defined( ROCKETNET_URING_SEND_QUEUE_PLANT )
    queue->bank[next].num_packets.store( 0, std::memory_order_relaxed );
#endif // #if !defined( ROCKETNET_URING_SEND_QUEUE_PLANT )
    queue->producer = next;
}

int rocketnet_uring_send_queues_turn( struct rocketnet_uring_send_queues_t * queues )
{
    rocketnet_assert( queues );

    int turned = 0;
    for ( int q = 0; q < queues->num_queues; q++ )
    {
        struct rocketnet_uring_send_queue_t * queue = &queues->queue[q];
        if ( queue->bank[queue->producer].num_packets.load( std::memory_order_acquire ) == 0 )
        {
            continue;
        }

        rocketnet_uring_send_queues_turn_to( queues, q, ( queue->producer + 1 ) % ROCKETNET_URING_SEND_BANKS );
        turned++;
    }

    return turned;
}

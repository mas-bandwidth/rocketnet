/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server's per-queue send banks (sdk/rocketnet_uring_send_queue.h)
// and the per-queue arithmetic they share with the XDP transport
// (sdk/rocketnet_send_queue.h), checked at the layout
// proofs/tla/RNUringQueueBankLayout.tla models: every refusal (a full bank, a
// pointer before, inside or past a bank, a pointer into a bank the producer
// has left, a queue or slot count out of range) and the routing of an id back
// to its queue. Pure memory: no ring, no socket, no thread, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_send_queue.h"
#include "../sdk/rocketnet_uring_send_queue.h"
#include "loopback_hub.h"

#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

static struct rocketnet_address_t uring_send_queue_client( uint16_t port )
{
    struct rocketnet_address_t client;
    memset( &client, 0, sizeof( client ) );
    client.type = ROCKETNET_ADDRESS_IPV4;
    client.data.ipv4[0] = 198;
    client.data.ipv4[1] = 51;
    client.data.ipv4[2] = 100;
    client.data.ipv4[3] = 7;
    client.port = port;
    return client;
}

// a client whose packets go through queue q of num_queues
static struct rocketnet_address_t uring_send_queue_client_on( int q, int num_queues )
{
    for ( int port = 40000; port < 50000; port++ )
    {
        struct rocketnet_address_t client = uring_send_queue_client( (uint16_t) port );
        if ( rocketnet_send_queue_of( &client, num_queues ) == q )
        {
            return client;
        }
    }
    check( false );
    return uring_send_queue_client( 0 );
}

// the slot arithmetic: a negative, unaligned or past-the-end offset is
// refused before it is divided (RNUringQueueBankLayout SlotFrom; the reversed
// witness BrokenDivideFirst catches the unaligned case, an offset of 1 byte
// truncating to the slot below)

static void test_send_queue_slot_refuses_before_dividing()
{
    const int stride = ROCKETNET_FRAME_STRIDE;
    check( rocketnet_send_queue_slot( 0, stride, 3 ) == 0 );
    check( rocketnet_send_queue_slot( stride, stride, 3 ) == 1 );
    check( rocketnet_send_queue_slot( 2 * stride, stride, 3 ) == 2 );

    // refused before any division (C would truncate -stride+1..-1 to slot 0)
    check( rocketnet_send_queue_slot( -1, stride, 3 ) == -1 );
    check( rocketnet_send_queue_slot( -( stride - 1 ), stride, 3 ) == -1 );
    check( rocketnet_send_queue_slot( -stride, stride, 3 ) == -1 );

    // unaligned
    check( rocketnet_send_queue_slot( 1, stride, 3 ) == -1 );
    check( rocketnet_send_queue_slot( stride + 1, stride, 3 ) == -1 );
    check( rocketnet_send_queue_slot( stride - 1, stride, 3 ) == -1 );

    // past the end
    check( rocketnet_send_queue_slot( 3 * stride, stride, 3 ) == -1 );
    check( rocketnet_send_queue_slot( 100 * stride, stride, 3 ) == -1 );
}

REGISTER_UNIT_TEST( test_send_queue_slot_refuses_before_dividing );

// the id carries its queue: the remainder by the queue count is the queue
// for every serial, where serial + queue would not be (RNUringQueueBankLayout
// PidOf, the reversed witness BrokenIdAdd)

static void test_send_queue_packet_id_carries_the_queue()
{
    for ( int num_queues = 1; num_queues <= ROCKETNET_URING_SEND_MAX_QUEUES; num_queues++ )
    {
        for ( uint64_t serial = 0; serial < 70; serial++ )
        {
            for ( int q = 0; q < num_queues; q++ )
            {
                const uint64_t id = rocketnet_send_queue_packet_id( serial, num_queues, q );
                check( rocketnet_send_queue_of_packet( id, num_queues ) == q );
            }
        }
    }

    // the high serials a long run reaches
    const uint64_t serial = UINT64_MAX / 64;
    check( rocketnet_send_queue_of_packet( rocketnet_send_queue_packet_id( serial, 32, 31 ), 32 ) == 31 );
}

REGISTER_UNIT_TEST( test_send_queue_packet_id_carries_the_queue );

// a claim at the limit is refused and counted, and the counter stays past the
// limit so every later claim is refused too (ClaimCmp "ge", the reversed
// witness BrokenClaimPastEnd)

static void test_send_queue_claim_refuses_at_the_limit()
{
    std::atomic<uint64_t> num( 0 );
    std::atomic<uint64_t> refused( 0 );
    check( rocketnet_send_queue_claim( num, 2, refused ) == 0 );
    check( rocketnet_send_queue_claim( num, 2, refused ) == 1 );
    check( rocketnet_send_queue_claim( num, 2, refused ) == -1 );
    check( rocketnet_send_queue_claim( num, 2, refused ) == -1 );
    check( refused.load() == 2 );
    check( num.load() == 4 );
}

REGISTER_UNIT_TEST( test_send_queue_claim_refuses_at_the_limit );

static void test_uring_send_queues_create_refuses_out_of_range()
{
    check( rocketnet_uring_send_queues_create( NULL, 0, 3 ) == NULL );
    check( rocketnet_uring_send_queues_create( NULL, -1, 3 ) == NULL );
    check( rocketnet_uring_send_queues_create( NULL, ROCKETNET_URING_SEND_MAX_QUEUES + 1, 3 ) == NULL );
    check( rocketnet_uring_send_queues_create( NULL, 2, 0 ) == NULL );
    check( rocketnet_uring_send_queues_create( NULL, 2, ROCKETNET_URING_SEND_MAX_BANK_SLOTS + 1 ) == NULL );

    struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( NULL, 2, 3 );
    check( queues != NULL );
    check( queues->num_queues == 2 );
    check( queues->slots == 3 );
    rocketnet_uring_send_queues_destroy( queues );

    rocketnet_uring_send_queues_destroy( NULL );
}

REGISTER_UNIT_TEST( test_uring_send_queues_create_refuses_out_of_range );

// the banks are laid end to end, every frame 16-aligned, so a pointer one
// byte before a bank is inside its neighbour: the case the refusals must cover

static void test_uring_send_queues_banks_are_end_to_end()
{
    struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( NULL, 2, 3 );
    check( queues != NULL );

    const uint8_t * first = queues->queue[0].bank[0].data;
    for ( int q = 0; q < 2; q++ )
    {
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            const ptrdiff_t at = queues->queue[q].bank[b].data - first;
            check( at == (ptrdiff_t) ( q * ROCKETNET_URING_SEND_BANKS + b ) * 3 * ROCKETNET_FRAME_STRIDE );
            check( ( (uintptr_t) queues->queue[q].bank[b].data ) % 16 == 0 );
        }
    }

    rocketnet_uring_send_queues_destroy( queues );
}

REGISTER_UNIT_TEST( test_uring_send_queues_banks_are_end_to_end );

// start routes to the destination's queue, the id finds it again, and a
// full producer bank refuses (NULL, counted) without touching any slot

static void test_uring_send_queues_start_routes_and_refuses_when_full()
{
    const int Q = 3;
    struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( NULL, Q, 3 );
    check( queues != NULL );

    for ( int q = 0; q < Q; q++ )
    {
        struct rocketnet_address_t client = uring_send_queue_client_on( q, Q );
        for ( int i = 0; i < 3; i++ )
        {
            uint64_t id = 0;
            uint8_t * frame = rocketnet_uring_send_queues_start( queues, &client, &id );
            check( frame == queues->queue[q].bank[0].data + i * ROCKETNET_FRAME_STRIDE );
            check( rocketnet_send_queue_of_packet( id, Q ) == q );
            check( queues->queue[q].bank[0].slot[i].packet_bytes == 0 );
            check( rocketnet_address_equal( &queues->queue[q].bank[0].slot[i].to, &client ) );
            check( rocketnet_uring_send_queues_staged( queues, q ) == i + 1 );
        }

        uint64_t id = 0;
        check( rocketnet_uring_send_queues_start( queues, &client, &id ) == NULL );
        check( rocketnet_send_queue_of_packet( id, Q ) == q );
        check( rocketnet_uring_send_queues_staged( queues, q ) == 3 );
    }

    check( queues->refused.load() == (uint64_t) Q );
    check( queues->packet_id.load() == (uint64_t) ( Q * 4 ) );

    rocketnet_uring_send_queues_destroy( queues );
}

REGISTER_UNIT_TEST( test_uring_send_queues_start_routes_and_refuses_when_full );

// finish and abort write only the slot their own claim holds: a pointer
// before the bank (inside the neighbour), unaligned, past the bank, or to a
// slot of another queue's bank is refused, counted, and writes nothing

static void test_uring_send_queues_finish_refuses_every_bad_pointer()
{
    const int Q = 2;
    struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( NULL, Q, 3 );
    check( queues != NULL );

    struct rocketnet_address_t a = uring_send_queue_client_on( 0, Q );
    struct rocketnet_address_t b = uring_send_queue_client_on( 1, Q );

    uint64_t id_a = 0;
    uint64_t id_b = 0;
    uint8_t * frame_a = rocketnet_uring_send_queues_start( queues, &a, &id_a );
    uint8_t * frame_b = rocketnet_uring_send_queues_start( queues, &b, &id_b );
    check( frame_a && frame_b );

    // queue 1's bank 0 is right after queue 0's three banks: one byte before
    // it is the last byte of queue 0's bank 2
    uint8_t * bank_b = queues->queue[1].bank[0].data;
    check( bank_b == frame_b );

    uint64_t refusals = 0;

    check( !rocketnet_uring_send_queues_finish( queues, id_b, bank_b - 1, 100 ) );
    check( queues->refused_pointer.load() == ++refusals );
    check( !rocketnet_uring_send_queues_finish( queues, id_b, bank_b - ( ROCKETNET_FRAME_STRIDE - 1 ), 100 ) );
    check( queues->refused_pointer.load() == ++refusals );
    check( !rocketnet_uring_send_queues_finish( queues, id_b, bank_b - ROCKETNET_FRAME_STRIDE, 100 ) );
    check( queues->refused_pointer.load() == ++refusals );
    check( !rocketnet_uring_send_queues_finish( queues, id_b, frame_b + 1, 100 ) );
    check( queues->refused_pointer.load() == ++refusals );
    check( !rocketnet_uring_send_queues_finish( queues, id_b, bank_b + 3 * ROCKETNET_FRAME_STRIDE, 100 ) );
    check( queues->refused_pointer.load() == ++refusals );

    // a's frame handed in with b's id: queue 1's bank does not hold it
    check( !rocketnet_uring_send_queues_finish( queues, id_b, frame_a, 100 ) );
    check( queues->refused_pointer.load() == ++refusals );

    check( !rocketnet_uring_send_queues_abort( queues, id_b, frame_b + 1 ) );
    check( queues->refused_pointer.load() == ++refusals );

    // a NULL abort is the caller's no-packet case: refused, not counted
    check( !rocketnet_uring_send_queues_abort( queues, id_b, NULL ) );
    check( queues->refused_pointer.load() == refusals );

    // nothing was written anywhere
    for ( int q = 0; q < Q; q++ )
    {
        for ( int bank = 0; bank < ROCKETNET_URING_SEND_BANKS; bank++ )
        {
            for ( int s = 0; s < 3; s++ )
            {
                check( queues->queue[q].bank[bank].slot[s].packet_bytes == 0 );
            }
        }
    }

    // the right pointers are taken
    check( rocketnet_uring_send_queues_finish( queues, id_a, frame_a, 100 ) );
    check( queues->queue[0].bank[0].slot[0].packet_bytes == 100 );
    check( rocketnet_uring_send_queues_finish( queues, id_b, frame_b, 200 ) );
    check( queues->queue[1].bank[0].slot[0].packet_bytes == 200 );
    check( rocketnet_uring_send_queues_abort( queues, id_b, frame_b ) );
    check( queues->queue[1].bank[0].slot[0].packet_bytes == 0 );
    check( queues->refused_pointer.load() == refusals );

    rocketnet_uring_send_queues_destroy( queues );
}

REGISTER_UNIT_TEST( test_uring_send_queues_finish_refuses_every_bad_pointer );

// turn moves only a queue that staged, resets its next bank before any
// claim, and a pointer into the bank the producer has left is refused

static void test_uring_send_queues_turn_resets_the_next_bank()
{
    const int Q = 2;
    struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( NULL, Q, 3 );
    check( queues != NULL );

    struct rocketnet_address_t a = uring_send_queue_client_on( 0, Q );

    // nothing staged: nothing turns
    check( rocketnet_uring_send_queues_turn( queues ) == 0 );
    check( queues->queue[0].producer == 0 && queues->queue[1].producer == 0 );

    uint64_t id = 0;
    uint8_t * old_frame = rocketnet_uring_send_queues_start( queues, &a, &id );
    check( old_frame != NULL );

    // a refused claim leaves the counter past the slots; the turn after
    // a full fill still resets the bank it moves to
    for ( int i = 0; i < 5; i++ )
    {
        uint64_t ignored = 0;
        (void) rocketnet_uring_send_queues_start( queues, &a, &ignored );
    }
    check( queues->queue[0].bank[0].num_packets.load() == 6 );
    check( rocketnet_uring_send_queues_staged( queues, 0 ) == 3 );

    // a stale count on bank 1 (as an earlier fill left it) is cleared by the turn
    queues->queue[0].bank[1].num_packets.store( 7 );

    check( rocketnet_uring_send_queues_turn( queues ) == 1 );
    check( queues->queue[0].producer == 1 );
    check( queues->queue[1].producer == 0 );
    check( queues->queue[0].bank[1].num_packets.load() == 0 );
    check( rocketnet_uring_send_queues_staged( queues, 0 ) == 0 );

    // the old pointer is refused: the producer bank no longer holds it
    check( !rocketnet_uring_send_queues_finish( queues, id, old_frame, 100 ) );
    check( queues->refused_pointer.load() == 1 );

    uint8_t * new_frame = rocketnet_uring_send_queues_start( queues, &a, &id );
    check( new_frame == queues->queue[0].bank[1].data );

    // three turns bring the producer back to bank 0, reset
    check( rocketnet_uring_send_queues_turn( queues ) == 1 );
    (void) rocketnet_uring_send_queues_start( queues, &a, &id );
    check( rocketnet_uring_send_queues_turn( queues ) == 1 );
    check( queues->queue[0].producer == 0 );
    check( queues->queue[0].bank[0].num_packets.load() == 0 );

    rocketnet_uring_send_queues_destroy( queues );
}

REGISTER_UNIT_TEST( test_uring_send_queues_turn_resets_the_next_bank );

// turn_to is the one step of a turn: the producer moves to the bank the
// queue's exchange handed over, which need not be the next in order, and that
// bank's counter is reset before any claim; the bank skipped and the other
// queues are untouched

static void test_uring_send_queues_turn_to_moves_one_queue_to_the_named_bank()
{
    const int Q = 2;
    struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( NULL, Q, 3 );
    check( queues != NULL );

    queues->queue[0].bank[1].num_packets.store( 5 );
    queues->queue[0].bank[2].num_packets.store( 9 );
    queues->queue[1].bank[2].num_packets.store( 4 );

    rocketnet_uring_send_queues_turn_to( queues, 0, 2 );
    check( queues->queue[0].producer == 2 );
    check( queues->queue[0].bank[2].num_packets.load() == 0 );
    check( queues->queue[0].bank[1].num_packets.load() == 5 );
    check( queues->queue[1].producer == 0 );
    check( queues->queue[1].bank[2].num_packets.load() == 4 );

    // the new producer bank is claimed from slot 0
    struct rocketnet_address_t a = uring_send_queue_client_on( 0, Q );
    uint64_t id = 0;
    uint8_t * frame = rocketnet_uring_send_queues_start( queues, &a, &id );
    check( frame == queues->queue[0].bank[2].data );
    check( rocketnet_uring_send_queues_staged( queues, 0 ) == 1 );

    // turn is turn_to the producer + 1, wrapping past the last bank
    check( rocketnet_uring_send_queues_turn( queues ) == 1 );
    check( queues->queue[0].producer == 0 );
    check( queues->queue[0].bank[0].num_packets.load() == 0 );

    rocketnet_uring_send_queues_destroy( queues );
}

REGISTER_UNIT_TEST( test_uring_send_queues_turn_to_moves_one_queue_to_the_named_bank );

// a transport sizes a queue by rate, from ROCKETNET_URING_SEND_BANK_SLOTS up
// to ROCKETNET_URING_SEND_MAX_BANK_SLOTS: the plan grows with the size and is
// 0 outside the range (what create asks the allocator for is
// test_uring_send_queues_create_allocates_its_plan)

static void test_uring_send_queues_allocation_bytes_follows_the_size()
{
    check( ROCKETNET_URING_SEND_MAX_BANK_SLOTS > ROCKETNET_URING_SEND_BANK_SLOTS );

    check( rocketnet_uring_send_queues_allocation_bytes( 0, 3 ) == 0 );
    check( rocketnet_uring_send_queues_allocation_bytes( ROCKETNET_URING_SEND_MAX_QUEUES + 1, 3 ) == 0 );
    check( rocketnet_uring_send_queues_allocation_bytes( 1, 0 ) == 0 );
    check( rocketnet_uring_send_queues_allocation_bytes( 1, ROCKETNET_URING_SEND_MAX_BANK_SLOTS + 1 ) == 0 );

    const int sizes[] = { 1, 3, ROCKETNET_URING_SEND_BANK_SLOTS, ROCKETNET_URING_SEND_BANK_SLOTS + 1, ROCKETNET_URING_SEND_MAX_BANK_SLOTS };
    uint64_t previous = 0;
    for ( int i = 0; i < (int) ( sizeof( sizes ) / sizeof( sizes[0] ) ); i++ )
    {
        const int slots = sizes[i];
        const uint64_t bytes = rocketnet_uring_send_queues_allocation_bytes( 2, slots );
        const uint64_t frames = (uint64_t) 2 * (uint64_t) ROCKETNET_URING_SEND_BANKS * (uint64_t) slots;
        check( bytes > previous );
        check( bytes >= frames * (uint64_t) ROCKETNET_FRAME_STRIDE );
        check( bytes <= frames * ( (uint64_t) ROCKETNET_FRAME_STRIDE + (uint64_t) sizeof( struct rocketnet_uring_send_slot_t ) ) + 4096 );
        previous = bytes;
    }

    // more queues, more bytes, for the same slots
    check( rocketnet_uring_send_queues_allocation_bytes( 3, 7 ) > rocketnet_uring_send_queues_allocation_bytes( 2, 7 ) );
    check( rocketnet_uring_send_queues_allocation_bytes( ROCKETNET_URING_SEND_MAX_QUEUES, ROCKETNET_URING_SEND_MAX_BANK_SLOTS ) > 0 );
}

REGISTER_UNIT_TEST( test_uring_send_queues_allocation_bytes_follows_the_size );

// create asks the installed allocator for exactly its plan: one block of
// allocation_bytes plus rocketnet_malloc's own overhead (the plan is the
// block; the transport's plan adds the overhead, rocketnet_memory_allocations),
// and destroy returns it. A counting allocator is installed for the case
// (its state is the context create is given, as the transport passes its
// own) and the default one put back after it

struct uring_send_queue_counting_t
{
    int calls;
    int live;
    uint64_t requested;
};

static void * uring_send_queue_count_allocate( void * context, size_t bytes )
{
    uring_send_queue_counting_t * counting = (uring_send_queue_counting_t *) context;
    counting->calls++;
    counting->live++;
    counting->requested += bytes;
    return malloc( bytes );
}

static void uring_send_queue_count_free( void * context, void * pointer )
{
    uring_send_queue_counting_t * counting = (uring_send_queue_counting_t *) context;
    counting->live--;
    free( pointer );
}

static void test_uring_send_queues_create_allocates_its_plan()
{
    uring_send_queue_counting_t counting;
    memset( &counting, 0, sizeof( counting ) );
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( &counting, uring_send_queue_count_allocate, uring_send_queue_count_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    // every queue count at small banks, and the base and past-base bank
    // sizes at one and two queues: the queue count's term and the bank
    // size's term of the plan each move, without a create of hundreds of
    // megabytes (MAX_QUEUES at a full bank) in a suite run one case a core
    const int plans[][2] = {
        { 1, 1 },
        { 1, 3 },
        { 2, 3 },
        { ROCKETNET_URING_SEND_MAX_QUEUES, 1 },
        { ROCKETNET_URING_SEND_MAX_QUEUES, 3 },
        { 1, ROCKETNET_URING_SEND_BANK_SLOTS },
        { 2, ROCKETNET_URING_SEND_BANK_SLOTS },
        { 1, ROCKETNET_URING_SEND_BANK_SLOTS + 1 },
        { 2, ROCKETNET_URING_SEND_BANK_SLOTS + 1 },
    };
    for ( int p = 0; p < (int) ( sizeof( plans ) / sizeof( plans[0] ) ); p++ )
    {
        const int queue_count = plans[p][0];
        const int slots = plans[p][1];
        const int live = counting.live;
        counting.calls = 0;
        counting.requested = 0;
        struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( &counting, queue_count, slots );
        check( queues != NULL );
        check( counting.calls == 1 );
        check( counting.requested == rocketnet_uring_send_queues_allocation_bytes( queue_count, slots ) + rocketnet_allocation_overhead() );
        rocketnet_uring_send_queues_destroy( queues );
        check( counting.live == live );
    }

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( counting.live == 0 );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_uring_send_queues_create_allocates_its_plan );

// a bank larger than the base size works end to end: every slot past
// ROCKETNET_URING_SEND_BANK_SLOTS up to the size is claimable, the next claim
// is refused and counted

static void test_uring_send_queues_a_bank_past_the_base_size_fills_to_its_size()
{
    const int slots = ROCKETNET_URING_SEND_BANK_SLOTS + 1;
    struct rocketnet_uring_send_queues_t * queues = rocketnet_uring_send_queues_create( NULL, 1, slots );
    check( queues != NULL );
    check( queues->slots == slots );

    struct rocketnet_address_t a = uring_send_queue_client( 40000 );
    uint8_t * last = NULL;
    for ( int i = 0; i < slots; i++ )
    {
        uint64_t id = 0;
        last = rocketnet_uring_send_queues_start( queues, &a, &id );
        check( last == queues->queue[0].bank[0].data + (size_t) i * ROCKETNET_FRAME_STRIDE );
    }
    check( last != NULL );
    check( rocketnet_uring_send_queues_staged( queues, 0 ) == slots );
    check( queues->refused.load() == 0 );

    uint64_t id = 0;
    check( rocketnet_uring_send_queues_start( queues, &a, &id ) == NULL );
    check( queues->refused.load() == 1 );
    check( rocketnet_uring_send_queues_staged( queues, 0 ) == slots );

    rocketnet_uring_send_queues_destroy( queues );
}

REGISTER_UNIT_TEST( test_uring_send_queues_a_bank_past_the_base_size_fills_to_its_size );

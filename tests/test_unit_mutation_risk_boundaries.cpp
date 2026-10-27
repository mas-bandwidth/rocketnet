/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */

// Boundary tests for the five risk survivors of the 2026-10-24 mutation
// audit (docs/audits/2026-10-24-mutation-testing.md, findings 1-5). Each
// case pins the exact boundary a surviving mutant blurred. No real time,
// sockets, network or threads: every time is passed as an argument.

#include "test_unit_registry.h"

#include "rocketnet_ack.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet_fragment.h"
#include "rocketnet_internal.h"
#include "rocketnet_message.h"
#include "rocketnet_window_ack.h"
#include "rocketnet_window_send.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

// ----------------------------------------------------------------
// Finding 1: loss threshold, exact boundary (rocketnet_ack.cpp)
// ----------------------------------------------------------------

static rocketnet_ack_endpoint_t * risk_ack_endpoint()
{
    auto * endpoint = (rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof(rocketnet_ack_endpoint_t) );
    check( endpoint );
    memset( (void *) endpoint, 0, sizeof(rocketnet_ack_endpoint_t) );
    rocketnet_ack_endpoint_reset( endpoint );
    memset( endpoint->sent_written, 0xFF, sizeof(endpoint->sent_written) );
    memset( endpoint->received_written, 0xFF, sizeof(endpoint->received_written) );
    memset( endpoint->aggregates_written, 0xFF, sizeof(endpoint->aggregates_written) );
    return endpoint;
}

static void risk_ack_record( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    check( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, 100, 0, 0, 0, time ) != ROCKETNET_ACK_SENT_REFUSED_STALE );
}

static void risk_ack_largest( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = sequence;
    block.first_range_length = 1;
    uint64_t ids[1], unknown = 0, full = 0;
    int count = 0;
    rocketnet_ack_endpoint_process_block( endpoint, &block, time, ids, 1, &count, &unknown, &full );
    check( unknown == 0 && full == 0 );
    check( endpoint->largest_acked_sequence == sequence );
}

// A record exactly ROCKETNET_LOSS_PACKET_THRESHOLD below the largest acked
// is EVICTED_LOST; one closer is EVICTED_UNKNOWN.
static void test_ack_loss_threshold_exact_boundary()
{
    const uint64_t base = 100;
    for ( int delta = ROCKETNET_LOSS_PACKET_THRESHOLD - 1; delta <= ROCKETNET_LOSS_PACKET_THRESHOLD; delta++ )
    {
        auto * endpoint = risk_ack_endpoint();
        risk_ack_record( endpoint, base, 1000.0 );
        risk_ack_record( endpoint, base + (uint64_t) delta, 1000.0 );
        risk_ack_largest( endpoint, base + (uint64_t) delta, 1000.1 );
        const enum rocketnet_ack_sent_result_t result = rocketnet_ack_endpoint_record_sent_classified(
            endpoint, base + ROCKETNET_SENT_PACKET_BUFFER_SIZE, 100, 0, 0, 0, 1000.2 );
        if ( delta == ROCKETNET_LOSS_PACKET_THRESHOLD )
            check( result == ROCKETNET_ACK_SENT_EVICTED_LOST );
        else
            check( result == ROCKETNET_ACK_SENT_EVICTED_UNKNOWN );
        rocketnet_free( NULL, endpoint );
    }
}

REGISTER_UNIT_TEST( test_ack_loss_threshold_exact_boundary );

// ----------------------------------------------------------------
// Finding 2: reassembly serial half range (rocketnet_fragment.cpp)
// ----------------------------------------------------------------

static const struct rocketnet_reassembly_counters_t risk_counters = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_FRAGMENT_STALE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL };

struct risk_reassembly_t
{
    struct rocketnet_counters_t counters_storage;
    uint64_t * counters;
    struct rocketnet_reassembly_set_t * set;
    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];
};

static void risk_reassembly_begin( struct risk_reassembly_t * h )
{
    memset( h, 0, sizeof(*h) );
    h->counters = h->counters_storage.values;
    h->set = rocketnet_reassembly_set_create( NULL, 1, 7, 8192, 4 );
    check( h->set != NULL );
}

static int risk_offer( struct risk_reassembly_t * h, uint32_t payload_id, int fragment_index )
{
    const uint8_t pattern = (uint8_t) ( payload_id * 7u + 3u );
    const int fragment_bytes = fragment_index == 0 ? 100 : 60;
    rocketnet_write_fragment_header( h->fragment, payload_id, fragment_index, 2 );
    for ( int i = 0; i < fragment_bytes; i++ )
        h->fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + i] = (uint8_t) ( pattern + 100 * fragment_index + i );
    rocketnet_reassembly_set_begin_update( h->set );
    const int result = rocketnet_reassembly_process_fragment( h->set, 0, h->fragment,
                                                              ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes,
                                                              h->counters, &risk_counters );
    if ( h->set->completed_bytes != 0 )
    {
        check( h->set->completed_bytes == 160 );
        for ( int i = 0; i < 160; i++ )
            check( h->set->completed_payload[i] == (uint8_t) ( pattern + i ) );
    }
    return result;
}

static void risk_complete( struct risk_reassembly_t * h, uint32_t payload_id )
{
    check( risk_offer( h, payload_id, 0 ) == 1 );
    check( risk_offer( h, payload_id, 1 ) == 1 );
    check( h->set->completed_bytes == 160 );
}

// The stale reference moves only on the serially newer side: a
// completion one reach ahead of a large reference moves it (the
// largest advance a fragment can carry past the stale reach guard),
// a completion behind it leaves it, and the audit's three far ids
// relative to 2 -- 2 + 0x7fffffff, 2 + 0x80000000 and
// 2 + 0x80000001 -- are refused stale and never move it. (A completion
// judged at exactly the half range cannot arrive: the reach guard
// refuses every fragment further than ROCKETNET_REASSEMBLY_REACH from
// the reference in both directions, so the completing fragment is
// always within one reach of the reference it is judged against.)
static void test_reassembly_serial_half_range()
{
    {
        struct risk_reassembly_t h;
        risk_reassembly_begin( &h );
        risk_complete( &h, 1 );
        risk_complete( &h, 2 );
        check( h.set->newest_completed[0] == 2 );

        // One full reach ahead, from a large reference: the serial
        // difference moves the reference; an added (not subtracted)
        // difference would land past the half range and not move it.
        const uint32_t reference = 2u + ROCKETNET_REASSEMBLY_REACH;
        risk_complete( &h, reference );
        check( h.set->newest_completed[0] == reference );
        const uint32_t target = reference + ROCKETNET_REASSEMBLY_REACH;
        risk_complete( &h, target );
        check( h.set->newest_completed[0] == target );

        // A completion behind the reference leaves it where it is.
        risk_complete( &h, target - 1 );
        check( h.set->newest_completed[0] == target );
        rocketnet_reassembly_set_destroy( h.set );
    }
    {
        struct risk_reassembly_t h;
        risk_reassembly_begin( &h );
        risk_complete( &h, 1 );
        risk_complete( &h, 2 );
        check( h.set->newest_completed[0] == 2 );

        const uint32_t offsets[3] = { 0x7fffffffu, 0x80000000u, 0x80000001u };
        for ( int k = 0; k < 3; k++ )
        {
            const uint64_t stale_before = h.counters[ROCKETNET_COUNTER_FRAGMENT_STALE];
            check( risk_offer( &h, 2u + offsets[k], 0 ) == 0 );
            check( h.counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == stale_before + 1 );
            check( h.set->newest_completed[0] == 2 );
        }
        rocketnet_reassembly_set_destroy( h.set );
    }
}

REGISTER_UNIT_TEST( test_reassembly_serial_half_range );

// ----------------------------------------------------------------
// Shared window fixture (findings 3, 4 and 5)
// ----------------------------------------------------------------

struct risk_window_t
{
    rocketnet_message_config_t config;
    struct rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_window_send_t send;
    rn_window_ack_t ack;
};

static void risk_window_begin( struct risk_window_t * w )
{
    rocketnet_message_default_config( &w->config );
    memset( &w->counters, 0, sizeof(w->counters) );
    w->arena = rn_arena_create( NULL, &w->config, 1, &w->counters );
    check( w->arena );
    w->region = rn_arena_region( w->arena, 0 );
    rn_region_carve( w->region );
    rn_window_send_init( &w->send, w->region, 0 );
    rn_window_ack_init( &w->ack, &w->send, w->region, &w->config );
}

static void risk_window_end( struct risk_window_t * w )
{
    rn_arena_destroy( w->arena );
}

// ----------------------------------------------------------------
// Finding 3: descriptor upper bound (rocketnet_window_ack.cpp)
// ----------------------------------------------------------------

// Descriptors queue - 1, queue and RN_DESCRIPTOR_NONE; only the first
// is dereferenced.
static void test_window_ack_descriptor_equal_queue()
{
    struct risk_window_t w;
    risk_window_begin( &w );
    rn_region_t * region = w.region;
    const uint32_t queue = region->queue;
    check( queue >= 2 );

    // The in-range descriptor names a live multi-fragment block.
    region->send_descriptors[queue - 1].in_use = 1;
    region->send_descriptors[queue - 1].fragments = 3;
    region->send_numbers[0].descriptor = queue - 1;
    check( rn_window_ack_slot_is_chunk( &w.ack, 0 ) == true );

    region->send_descriptors[queue - 1].fragments = 1;
    check( rn_window_ack_slot_is_chunk( &w.ack, 0 ) == false );
    region->send_descriptors[queue - 1].fragments = 3;

    // One past the queue must not be dereferenced. Plant the bytes a
    // dereference would read there (the send store follows the
    // descriptor array) as a live multi-fragment descriptor, so a
    // build that reads it answers true.
    uint8_t * past = (uint8_t *) &region->send_descriptors[queue];
    memset( past, 0, 64 );
    past[36] = 1;        // in_use, at its rn_send_descriptor_t offset
    past[38] = 3;        // fragments (little-endian low byte, any host
    past[39] = 0;        // reads the planted value the same way)
    region->send_numbers[0].descriptor = queue;
    check( rn_window_ack_slot_is_chunk( &w.ack, 0 ) == false );

    region->send_numbers[0].descriptor = RN_DESCRIPTOR_NONE;
    check( rn_window_ack_slot_is_chunk( &w.ack, 0 ) == false );

    risk_window_end( &w );
}

REGISTER_UNIT_TEST( test_window_ack_descriptor_equal_queue );

// ----------------------------------------------------------------
// Finding 4: pipelined reliable timeout (rocketnet_window_ack.cpp)
// ----------------------------------------------------------------

// Two reliable numbers in flight, the older acked, the newer still
// times out with the reliable-timeout disconnect reason.
static void test_window_ack_pipelined_reliable_timeout()
{
    struct risk_window_t w;
    risk_window_begin( &w );
    w.ack.reliable_timeout = 1.0;

    const uint32_t d0 = rn_region_send_alloc( w.region, 16 );
    const uint32_t d1 = rn_region_send_alloc( w.region, 16 );
    check( d0 != RN_DESCRIPTOR_NONE && d1 != RN_DESCRIPTOR_NONE );
    uint64_t n0 = 0, n1 = 0, ring = 0;
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d0, &n0, &ring ) == RN_WINDOW_SEND_NUMBERED );
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d1, &n1, &ring ) == RN_WINDOW_SEND_NUMBERED );
    rn_window_ack_commit_packet( &w.ack, 1, &n0, 1, 1000.0 );
    rn_window_ack_commit_packet( &w.ack, 2, &n1, 1, 1000.0 );

    check( rn_window_ack_check_timeout( &w.ack, 1000.5 ) == 0 );
    check( rn_window_ack_on_ack( &w.ack, 1, 1000.1 ) == 1 );   // older acked
    check( w.send.U == n1 );
    check( w.ack.has_oldest == 1 && w.ack.oldest_number == n1 );
    check( rn_window_ack_check_timeout( &w.ack, 1000.9 ) == 0 );
    check( w.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rn_window_ack_check_timeout( &w.ack, 1001.0 ) == 1 );
    check( w.ack.error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );

    risk_window_end( &w );
}

REGISTER_UNIT_TEST( test_window_ack_pipelined_reliable_timeout );

// ----------------------------------------------------------------
// Finding 5: out-of-order fragment chain (rocketnet_window_ack.cpp)
// ----------------------------------------------------------------

// The chain a descriptor still holds, in allocation order. The meta
// layout is sdk/rocketnet_allocator.cpp's: next index in the low 20
// bits, 0xFFFFF as the end mark.
static int risk_chain( rn_region_t * region, uint32_t d, uint32_t * out, int capacity )
{
    const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
    if ( descriptor->chunk == RN_CHUNK_NONE )
        return 0;
    const rn_store_class_t * cls = &region->store.classes[RN_STORE_LARGE];
    int count = 0;
    uint32_t cur = descriptor->chunk & 0x3FFFFFFFu;
    while ( cur != 0xFFFFFu && count < capacity )
    {
        out[count++] = cur;
        cur = cls->meta[cur] & 0xFFFFFu;
    }
    return count;
}

static void risk_check_chain( rn_region_t * region, uint32_t d, const uint32_t * expected, int expected_count )
{
    uint32_t chain[8];
    const int count = risk_chain( region, d, chain, 8 );
    check( count == expected_count );
    for ( int i = 0; i < expected_count; i++ )
        check( chain[i] == expected[i] );
}

// Out-of-order three-fragment ack with an interleaved descriptor names
// the right chain_index: after each ack the exact chunk released is
// checked against the chain, and the block completes only on the
// third distinct ack. This drives the legacy chunk-backed block path
// through rn_region_send_alloc directly; the production dispatcher
// (rn_region_send_take) routes multi-fragment sends to spans, so this
// path's liveness is reported with this test, not assumed.
static void test_window_ack_out_of_order_fragment_chain()
{
    struct risk_window_t w;
    risk_window_begin( &w );
    rn_region_t * region = w.region;
    const uint32_t large_total = rn_send_store_total_chunks( &region->store, RN_STORE_LARGE );
    const uint32_t desc_total = region->queue;

    const uint32_t d_other = rn_region_send_alloc( region, 64 );
    const uint32_t d_block = rn_region_send_alloc( region, 2500 );
    const uint32_t d_late = rn_region_send_alloc( region, 64 );
    check( d_other != RN_DESCRIPTOR_NONE && d_block != RN_DESCRIPTOR_NONE && d_late != RN_DESCRIPTOR_NONE );
    check( region->send_descriptors[d_block].fragments == 3 );

    uint32_t chunks[3];
    check( risk_chain( region, d_block, chunks, 3 ) == 3 );

    uint64_t n_other = 0, n_first = 0, n_late = 0, ring = 0;
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 64, d_other, &n_other, &ring ) == RN_WINDOW_SEND_NUMBERED );
    check( rn_window_send_first_write_block( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2500, d_block, &n_first, &ring ) == 3 );
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 64, d_late, &n_late, &ring ) == RN_WINDOW_SEND_NUMBERED );
    const uint64_t n_mid = n_first + 1, n_last = n_first + 2;
    rn_window_ack_commit_packet( &w.ack, 1, &n_other, 1, 1000.0 );
    rn_window_ack_commit_packet( &w.ack, 2, &n_first, 1, 1000.0 );
    rn_window_ack_commit_packet( &w.ack, 3, &n_mid, 1, 1000.0 );
    rn_window_ack_commit_packet( &w.ack, 4, &n_last, 1, 1000.0 );
    rn_window_ack_commit_packet( &w.ack, 5, &n_late, 1, 1000.0 );

    // Middle fragment acked first: its own chunk, and only it, goes.
    check( rn_window_ack_on_ack( &w.ack, 3, 1000.05 ) == 1 );
    { const uint32_t expected[2] = { chunks[0], chunks[2] }; risk_check_chain( region, d_block, expected, 2 ); }
    check( region->send_descriptors[d_block].in_use == 1 );
    check( region->send_descriptors[d_block].fragments_acked == 1 );

    // An interleaved unrelated descriptor acks without touching the chain.
    check( rn_window_ack_on_ack( &w.ack, 1, 1000.06 ) == 1 );
    { const uint32_t expected[2] = { chunks[0], chunks[2] }; risk_check_chain( region, d_block, expected, 2 ); }
    check( region->send_descriptors[d_block].fragments_acked == 1 );

    // Last fragment acked second: the tail chunk goes, the head stays.
    check( rn_window_ack_on_ack( &w.ack, 4, 1000.07 ) == 1 );
    { const uint32_t expected[1] = { chunks[0] }; risk_check_chain( region, d_block, expected, 1 ); }
    check( region->send_descriptors[d_block].in_use == 1 );
    check( region->send_descriptors[d_block].fragments_acked == 2 );

    // First fragment acked last: the block completes only now.
    check( rn_window_ack_on_ack( &w.ack, 2, 1000.08 ) == 1 );
    check( region->send_descriptors[d_block].in_use == 0 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_LARGE ) == large_total );
    check( region->descriptors_free == desc_total - 1 );   // d_late still held

    check( rn_window_ack_on_ack( &w.ack, 5, 1000.09 ) == 1 );
    check( region->descriptors_free == desc_total );

    risk_window_end( &w );
}

REGISTER_UNIT_TEST( test_window_ack_out_of_order_fragment_chain );

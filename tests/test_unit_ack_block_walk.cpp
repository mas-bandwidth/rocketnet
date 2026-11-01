/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The ack block walk visits only held, unacknowledged sent records, via the
// sent ring's shadow bitmaps (sent_held, sent_unacked). The three tests pin
// the walk's shape and the reconcile that keeps the bitmaps equal to the ring
// (the BitsMatchRing check).

#include "test_unit_registry.h"

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern thread_local unsigned long long g_ack_visit_count;

static void rec_sent( struct rocketnet_ack_endpoint_t * ep, uint64_t sequence )
{
    enum rocketnet_ack_sent_result_t r = rocketnet_ack_endpoint_record_sent_classified(
        ep, sequence, 100, 1000000 + sequence, 1, 1000.0 + (double) sequence * 0.001 );
    rocketnet_unit_check( r != ROCKETNET_ACK_SENT_REFUSED_STALE );
}

static void apply_block( struct rocketnet_ack_endpoint_t * ep, uint64_t largest, uint64_t first_len,
                         uint64_t * acked, int capacity, int * acked_count,
                         uint64_t * unknown, uint64_t * batch_full )
{
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = largest;
    block.first_range_length = (uint32_t) first_len;
    rocketnet_ack_endpoint_process_block( ep, &block, 3000.0, acked, capacity, acked_count, unknown, batch_full );
}

static void check_bits_match_ring( struct rocketnet_ack_endpoint_t * ep )
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    uint64_t window_low = 1;
    if ( ep->highest_sequence >= N )
    {
        window_low = ep->highest_sequence - N + 1;
    }

    for ( uint64_t i = 0; i < N; i++ )
    {
        uint64_t held_bit = ( ep->sent_held[i >> 6] >> ( i & 63 ) ) & 1;
        uint64_t unacked_bit = ( ep->sent_unacked[i >> 6] >> ( i & 63 ) ) & 1;

        // a slot not written since the reset is the empty record (sequence 0)

        const bool written = ( ep->sent_written[i >> 6] >> ( i & 63 ) ) & 1;
        int expect_held = written && ep->sent[i].sequence != 0 && ep->sent[i].sequence >= window_low;
        int expect_unacked = expect_held && !ep->sent[i].acked;
        rocketnet_unit_check( ( held_bit != 0 ) == ( expect_held != 0 ) );
        rocketnet_unit_check( ( unacked_bit != 0 ) == ( expect_unacked != 0 ) );
    }
}

// test_ack_block_walk_visits_only_unacknowledged: a full-window run visits every
// held record once and a second identical run visits nothing; record new
// records and the walk touches only them.

static void test_ack_block_walk_visits_only_unacknowledged()
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    struct rocketnet_ack_endpoint_t * ep = (struct rocketnet_ack_endpoint_t *)
        rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    uint64_t * acked = (uint64_t *) rocketnet_malloc( NULL, N * sizeof( uint64_t ) );
    rocketnet_unit_check( acked );

    for ( uint64_t s = 1; s <= N; s++ )
    {
        rec_sent( ep, s );
    }

    int acked_count = 0;
    uint64_t unknown = 0, batch_full = 0;
    g_ack_visit_count = 0;
    apply_block( ep, N, N, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == (int) N );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( g_ack_visit_count == N );

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    g_ack_visit_count = 0;
    apply_block( ep, N, N, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 0 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( g_ack_visit_count == 0 );

    for ( uint64_t s = N + 1; s <= N + 4; s++ )
    {
        rec_sent( ep, s );
    }

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    g_ack_visit_count = 0;
    apply_block( ep, N + 4, N, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 4 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( g_ack_visit_count == 4 );

    rocketnet_free( NULL, acked );
    rocketnet_free( NULL, ep );
}
REGISTER_UNIT_TEST( test_ack_block_walk_visits_only_unacknowledged );

// test_ack_block_walk_lost_packet_does_not_stall: a lost packet never stalls the
// walk -- rounds of new records each ack fully and visit exactly the new
// unacknowledged ones -- and once it leaves the window nothing visits it; a
// batch deferral re-surfaces the deferred record.

static void test_ack_block_walk_lost_packet_does_not_stall()
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    struct rocketnet_ack_endpoint_t * ep = (struct rocketnet_ack_endpoint_t *)
        rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    uint64_t * acked = (uint64_t *) rocketnet_malloc( NULL, N * sizeof( uint64_t ) );
    rocketnet_unit_check( acked );

    for ( uint64_t s = 1; s <= N; s++ )
    {
        rec_sent( ep, s );
    }

    // acknowledge everything but 100: two ranges, [1,99] and [101,N]

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = N;
    block.first_range_length = (uint32_t) ( N - 100 );
    block.num_ranges = 1;
    block.range_gap[0] = 1;
    block.range_length[0] = 99;

    int acked_count = 0;
    uint64_t unknown = 0, batch_full = 0;
    g_ack_visit_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3000.0, acked, (int) N, &acked_count, &unknown, &batch_full ) == (int) ( N - 1 ) );
    rocketnet_unit_check( acked_count == (int) ( N - 1 ) );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( g_ack_visit_count == N - 1 );

    uint64_t highest = N;

    for ( int round = 0; round < 4; round++ )
    {
        for ( int i = 0; i < 64; i++ )
        {
            highest++;
            rec_sent( ep, highest );
        }

        uint64_t window_low = highest >= N ? highest - N + 1 : 1;
        struct rocketnet_ack_block_t b;
        memset( &b, 0, sizeof( b ) );
        b.largest = highest;

        if ( 100 >= window_low && 100 <= highest )
        {
            b.first_range_length = (uint32_t) ( highest - 101 + 1 );
            b.num_ranges = 1;
            b.range_gap[0] = 1;
            b.range_length[0] = (uint16_t) ( 99 - window_low + 1 );
        }
        else
        {
            b.first_range_length = (uint32_t) ( highest - window_low + 1 );
        }

        acked_count = 0;
        unknown = 0;
        batch_full = 0;
        g_ack_visit_count = 0;
        rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &b, 3000.0, acked, (int) N, &acked_count, &unknown, &batch_full ) == 64 );
        rocketnet_unit_check( acked_count == 64 );
        rocketnet_unit_check( unknown == 0 );
        rocketnet_unit_check( g_ack_visit_count == 64 );
    }

    // the rounds took highest past N + 100, so 100 left the window; a further
    // block visits only the newest unacknowledged records

    for ( int i = 0; i < 64; i++ )
    {
        highest++;
        rec_sent( ep, highest );
    }

    uint64_t window_low = highest - N + 1;
    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    g_ack_visit_count = 0;
    apply_block( ep, highest, highest - window_low + 1, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 64 );
    rocketnet_unit_check( g_ack_visit_count == 64 );

    rocketnet_free( NULL, acked );
    rocketnet_free( NULL, ep );

    // a deferral: batch capacity 2, three new unacknowledged records

    ep = (struct rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    rec_sent( ep, 1 );
    rec_sent( ep, 2 );
    rec_sent( ep, 3 );

    struct rocketnet_ack_block_t db;
    memset( &db, 0, sizeof( db ) );
    db.largest = 3;
    db.first_range_length = 3;

    uint64_t da[4];
    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    g_ack_visit_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &db, 3000.0, da, 2, &acked_count, &unknown, &batch_full ) == 2 );
    rocketnet_unit_check( acked_count == 2 );
    rocketnet_unit_check( batch_full == 1 );
    rocketnet_unit_check( g_ack_visit_count == 3 );

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    g_ack_visit_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &db, 3100.0, da, 2, &acked_count, &unknown, &batch_full ) == 1 );
    rocketnet_unit_check( acked_count == 1 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( g_ack_visit_count == 1 );

    rocketnet_free( NULL, ep );
}
REGISTER_UNIT_TEST( test_ack_block_walk_lost_packet_does_not_stall );

// test_ack_block_walk_bits_match_ring: after each process_block, the shadow
// bitmaps equal the ring -- the held bit of a slot is set iff its record holds
// a sequence at or above the window low, and the unacked bit iff it is held
// and not acknowledged. Drives normal records, a burned sequence, an alias
// write N above an unacknowledged record, a refused stale write, and a
// deferral.

static void test_ack_block_walk_bits_match_ring()
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    struct rocketnet_ack_endpoint_t * ep = (struct rocketnet_ack_endpoint_t *)
        rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    uint64_t * acked = (uint64_t *) rocketnet_malloc( NULL, N * sizeof( uint64_t ) );
    rocketnet_unit_check( acked );

    int acked_count = 0;
    uint64_t unknown = 0, batch_full = 0;

    for ( uint64_t s = 1; s <= 200; s++ )
    {
        rec_sent( ep, s );
    }

    // burned sequence 201; record 202..400

    for ( uint64_t s = 202; s <= 400; s++ )
    {
        rec_sent( ep, s );
    }

    // alias write N above the unacknowledged record at slot 200

    rec_sent( ep, 200 + N );

    for ( uint64_t s = N + 401; s <= N + 450; s++ )
    {
        rec_sent( ep, s );
    }

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    apply_block( ep, N + 450, 64, acked, (int) N, &acked_count, &unknown, &batch_full );
    check_bits_match_ring( ep );

    // a stale write below the window is refused and sets nothing

    enum rocketnet_ack_sent_result_t refused;
    refused = rocketnet_ack_endpoint_record_sent_classified( ep, 5, 100, 5000005, 1, 2000.0 );
    rocketnet_unit_check( refused == ROCKETNET_ACK_SENT_REFUSED_STALE );

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    apply_block( ep, N + 450, 64, acked, (int) N, &acked_count, &unknown, &batch_full );
    check_bits_match_ring( ep );

    // a second alias, N above an unacknowledged in-window record

    rec_sent( ep, 1000 + N );

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    apply_block( ep, 1000 + N, 64, acked, (int) N, &acked_count, &unknown, &batch_full );
    check_bits_match_ring( ep );

    for ( uint64_t s = N + 451; s <= N + 520; s++ )
    {
        rec_sent( ep, s );
    }

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    apply_block( ep, N + 520, 64, acked, (int) N, &acked_count, &unknown, &batch_full );
    check_bits_match_ring( ep );

    rocketnet_free( NULL, acked );
    rocketnet_free( NULL, ep );

    // a deferral on a fresh endpoint: capacity 2 over three new unacknowledged
    // records (SessionAcks RecvAck's batch deferral)

    ep = (struct rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    acked = (uint64_t *) rocketnet_malloc( NULL, N * sizeof( uint64_t ) );
    rocketnet_unit_check( acked );

    rec_sent( ep, 1 );
    rec_sent( ep, 2 );
    rec_sent( ep, 3 );

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = 3;
    block.first_range_length = 3;

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    rocketnet_ack_endpoint_process_block( ep, &block, 3000.0, acked, 2, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 2 );
    rocketnet_unit_check( batch_full == 1 );
    rocketnet_unit_check( unknown == 0 );
    check_bits_match_ring( ep );

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    rocketnet_ack_endpoint_process_block( ep, &block, 3100.0, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 1 );
    check_bits_match_ring( ep );

    rocketnet_free( NULL, acked );
    rocketnet_free( NULL, ep );
}
REGISTER_UNIT_TEST( test_ack_block_walk_bits_match_ring );

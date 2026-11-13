/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The ack block walk visits only held, unacknowledged sent records, via the
// sent ring's shadow bitmaps (sent_held, sent_unacked). The first three tests
// pin the walk's shape and the reconcile that keeps the bitmaps equal to the
// ring (the BitsMatchRing check); the next pins that only message packets
// surface ids; the last two pin the block memo against the full apply and
// its clearing.

#include "test_unit_registry.h"

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned long long rocketnet_ack_visit_count();
extern void rocketnet_ack_visit_count_reset();

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
    rocketnet_ack_visit_count_reset();
    apply_block( ep, N, N, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == (int) N );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( rocketnet_ack_visit_count() == N );

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    rocketnet_ack_visit_count_reset();
    apply_block( ep, N, N, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 0 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( rocketnet_ack_visit_count() == 0 );

    for ( uint64_t s = N + 1; s <= N + 4; s++ )
    {
        rec_sent( ep, s );
    }

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    rocketnet_ack_visit_count_reset();
    apply_block( ep, N + 4, N, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 4 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( rocketnet_ack_visit_count() == 4 );

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
    rocketnet_ack_visit_count_reset();
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3000.0, acked, (int) N, &acked_count, &unknown, &batch_full ) == (int) ( N - 1 ) );
    rocketnet_unit_check( acked_count == (int) ( N - 1 ) );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( rocketnet_ack_visit_count() == N - 1 );

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
        rocketnet_ack_visit_count_reset();
        rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &b, 3000.0, acked, (int) N, &acked_count, &unknown, &batch_full ) == 64 );
        rocketnet_unit_check( acked_count == 64 );
        rocketnet_unit_check( unknown == 0 );
        rocketnet_unit_check( rocketnet_ack_visit_count() == 64 );
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
    rocketnet_ack_visit_count_reset();
    apply_block( ep, highest, highest - window_low + 1, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 64 );
    rocketnet_unit_check( rocketnet_ack_visit_count() == 64 );

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
    rocketnet_ack_visit_count_reset();
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &db, 3000.0, da, 2, &acked_count, &unknown, &batch_full ) == 2 );
    rocketnet_unit_check( acked_count == 2 );
    rocketnet_unit_check( batch_full == 1 );
    rocketnet_unit_check( rocketnet_ack_visit_count() == 3 );

    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    rocketnet_ack_visit_count_reset();
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &db, 3100.0, da, 2, &acked_count, &unknown, &batch_full ) == 1 );
    rocketnet_unit_check( acked_count == 1 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( rocketnet_ack_visit_count() == 1 );

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

// test_ack_block_walk_reports_only_message_packets: only a sealed payload frame
// is recorded with a packet id (has_packet_id 1); keep-alives, standalone acks
// and the other session packets are recorded without one. An ack block that
// covers both kinds acknowledges every sequence and surfaces the message
// packets' ids alone, and one that covers only a session packet surfaces
// none, so the message layer's silence rule (standard/MESSAGES.md m9.2: "an
// update that acknowledges a message packet") never sees it.

static void test_ack_block_walk_reports_only_message_packets()
{
    struct rocketnet_ack_endpoint_t * ep = (struct rocketnet_ack_endpoint_t *)
        rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( ep, 1, 100, 7001, 1, 1000.0 ) != ROCKETNET_ACK_SENT_REFUSED_STALE );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( ep, 2, 40, 0, 0, 1000.1 ) != ROCKETNET_ACK_SENT_REFUSED_STALE );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( ep, 3, 100, 7003, 1, 1000.2 ) != ROCKETNET_ACK_SENT_REFUSED_STALE );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( ep, 4, 40, 0, 0, 1000.3 ) != ROCKETNET_ACK_SENT_REFUSED_STALE );

    uint64_t acked[8];
    int acked_count = 0;
    uint64_t unknown = 0, batch_full = 0;
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = 3;
    block.first_range_length = 3;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 1001.0, acked, 8, &acked_count, &unknown, &batch_full ) == 3 );
    rocketnet_unit_check( acked_count == 2 );
    rocketnet_unit_check( ( acked[0] == 7001 && acked[1] == 7003 ) || ( acked[0] == 7003 && acked[1] == 7001 ) );

    acked_count = 0;
    block.largest = 4;
    block.first_range_length = 1;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 1001.5, acked, 8, &acked_count, &unknown, &batch_full ) == 1 );
    rocketnet_unit_check( acked_count == 0 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );

    rocketnet_free( NULL, ep );
}
REGISTER_UNIT_TEST( test_ack_block_walk_reports_only_message_packets );

// The block memo (process_block's repeat path) against the full apply. Two
// endpoints take the same sends, keep-alive marks and blocks; one applies
// through process_block, the other through process_block_full. After every
// block the returns, the acked id batches, both side counters and the whole
// endpoint state (state_equal) must be equal. The blocks mix fresh ones with
// the adversarial kinds: the last block again (the memo's case), the last
// block one sequence different, an older block again, a block above
// everything sent, a long block reaching below the window, a block with more
// ranges than the memo keeps, and a small batch that defers ids. Sequences
// run past the ring several times, with burned sequences and session
// (number-less) packets.

static uint64_t memo_next( uint64_t * state )
{
    uint64_t z = ( *state += 0x9E3779B97F4A7C15ull );
    z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
    z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
    return z ^ ( z >> 31 );
}

static uint64_t memo_below( uint64_t * state, uint64_t n )
{
    return n ? memo_next( state ) % n : 0;
}

static void memo_fresh_block( uint64_t * rng, uint64_t highest, struct rocketnet_ack_block_t * block )
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    memset( block, 0, sizeof( *block ) );

    uint64_t largest = highest;
    const uint64_t kind = memo_below( rng, 16 );
    if ( kind == 0 )
    {
        largest = highest + 1 + memo_below( rng, 3 );                    // above everything sent
    }
    else
    {
        const uint64_t lag = memo_below( rng, 24 );
        largest = highest > lag ? highest - lag : 1;
    }
    if ( largest == 0 )
    {
        largest = 1;
    }

    uint64_t first = 1 + memo_below( rng, 40 );
    if ( kind == 1 )
    {
        first = N - 64 + memo_below( rng, 256 );                         // reaches below the window
    }
    if ( first > largest )
    {
        first = largest;
    }
    block->largest = largest;
    block->first_range_length = (uint32_t) first;

    const int ranges = kind == 2 ? 5 + (int) memo_below( rng, 4 ) : (int) memo_below( rng, ROCKETNET_ACK_BLOCK_MEMO_RANGES + 1 );
    uint64_t low = largest - first + 1;
    for ( int i = 0; i < ranges; i++ )
    {
        const uint64_t gap = 1 + memo_below( rng, 8 );
        const uint64_t length = 1 + memo_below( rng, 24 );
        if ( low <= gap + length + 1 )
        {
            break;
        }
        block->range_gap[i] = (uint16_t) gap;
        block->range_length[i] = (uint16_t) length;
        block->num_ranges = i + 1;
        low = low - 1 - gap - length + 1;
    }
}

// the lowest sequence a block names
static uint64_t memo_block_low( const struct rocketnet_ack_block_t * block )
{
    uint64_t low = block->largest - block->first_range_length + 1;
    for ( int i = 0; i < block->num_ranges; i++ )
    {
        low = low - 1 - block->range_gap[i] - block->range_length[i] + 1;
    }
    return low;
}

// one sequence different: a run one longer or shorter at its low end, or the
// largest one lower (a smaller first range keeps the rest in place)
static bool memo_one_off( uint64_t * rng, struct rocketnet_ack_block_t * block )
{
    const uint64_t which = memo_below( rng, 3 );
    if ( which == 0 && block->num_ranges > 0 )
    {
        const int i = (int) memo_below( rng, (uint64_t) block->num_ranges );
        if ( block->range_length[i] > 1 && memo_below( rng, 2 ) )
        {
            block->range_length[i]--;
            return true;
        }
        if ( i == block->num_ranges - 1 && block->range_length[i] < 24 && memo_block_low( block ) > 1 )
        {
            block->range_length[i]++;                                    // the lowest run, one lower
            return true;
        }
        return false;
    }
    if ( which == 1 && block->first_range_length > 1 )
    {
        block->first_range_length--;
        block->largest--;                                                // the first run ends one lower
        return true;
    }
    if ( block->num_ranges == 0 && block->first_range_length < block->largest )
    {
        block->first_range_length++;                                     // the only run, one lower at its low end
        return true;
    }
    return false;
}

static void memo_differential( uint64_t seed, int steps )
{
    struct rocketnet_ack_endpoint_t * a = (struct rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    struct rocketnet_ack_endpoint_t * b = (struct rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( a && b );
    rocketnet_ack_endpoint_reset( a );
    rocketnet_ack_endpoint_reset( b );

    uint64_t rng = seed;
    uint64_t next_sequence = 1;
    double time = 1000.0;

    struct rocketnet_ack_block_t last;
    memset( &last, 0, sizeof( last ) );
    struct rocketnet_ack_block_t older[8];
    memset( older, 0, sizeof( older ) );
    int older_count = 0;

    int repeats_skipped = 0;
    int one_off = 0;
    int older_again = 0;
    int deferred = 0;
    int unmemoized = 0;

    for ( int step = 0; step < steps; step++ )
    {
        time += 0.001 * (double) ( 1 + memo_below( &rng, 20 ) );

        // sends: none (so the last block can repeat), or a few, some burned,
        // some number-less, some keep-alives

        const uint64_t sends = memo_below( &rng, 3 ) == 0 ? 0 : memo_below( &rng, 4 );
        for ( uint64_t k = 0; k < sends; k++ )
        {
            if ( memo_below( &rng, 10 ) == 0 )
            {
                next_sequence++;                                         // burned: never recorded
            }
            const uint64_t sequence = next_sequence++;
            const int has_id = memo_below( &rng, 5 ) != 0;
            const int bytes = 40 + (int) memo_below( &rng, 1200 );
            const enum rocketnet_ack_sent_result_t ra = rocketnet_ack_endpoint_record_sent_classified( a, sequence, bytes, has_id ? 500000 + sequence : 0, has_id, time );
            const enum rocketnet_ack_sent_result_t rb = rocketnet_ack_endpoint_record_sent_classified( b, sequence, bytes, has_id ? 500000 + sequence : 0, has_id, time );
            rocketnet_unit_check( ra == rb );
            if ( memo_below( &rng, 8 ) == 0 )
            {
                rocketnet_ack_endpoint_mark_keep_alive( a, sequence );
                rocketnet_ack_endpoint_mark_keep_alive( b, sequence );
            }
        }

        // the block

        struct rocketnet_ack_block_t block;
        const uint64_t kind = memo_below( &rng, 10 );
        bool from_last = false;
        if ( kind < 4 && last.largest )
        {
            block = last;                                                // the last block again
            from_last = true;
        }
        else if ( kind == 4 && last.largest )
        {
            block = last;
            if ( memo_one_off( &rng, &block ) )
            {
                one_off++;
            }
        }
        else if ( kind == 5 && older_count > 0 )
        {
            block = older[memo_below( &rng, (uint64_t) ( older_count < 8 ? older_count : 8 ) )];
            older_again++;
        }
        else
        {
            memo_fresh_block( &rng, a->highest_sequence ? a->highest_sequence : 1, &block );
        }

        if ( block.num_ranges > ROCKETNET_ACK_BLOCK_MEMO_RANGES )
        {
            unmemoized++;
        }

        const int capacity = memo_below( &rng, 6 ) == 0 ? 1 + (int) memo_below( &rng, 3 ) : 64;
        uint64_t ids_a[64];
        uint64_t ids_b[64];
        int count_a = 0;
        int count_b = 0;
        uint64_t unknown_a = 7;
        uint64_t unknown_b = 7;
        uint64_t full_a = 3;
        uint64_t full_b = 3;

        const bool memo_was_valid = a->block_memo.valid != 0;
        const int na = rocketnet_ack_endpoint_process_block( a, &block, time, ids_a, capacity, &count_a, &unknown_a, &full_a );
        const int nb = rocketnet_ack_endpoint_process_block_full( b, &block, time, ids_b, capacity, &count_b, &unknown_b, &full_b );

        rocketnet_unit_check( na == nb );
        rocketnet_unit_check( count_a == count_b );
        for ( int i = 0; i < count_a; i++ )
        {
            rocketnet_unit_check( ids_a[i] == ids_b[i] );
        }
        rocketnet_unit_check( unknown_a == unknown_b );
        rocketnet_unit_check( full_a == full_b );
        rocketnet_unit_check( rocketnet_ack_endpoint_state_equal( a, b ) );
        rocketnet_unit_check( a->largest_acked_sequence == b->largest_acked_sequence );
        rocketnet_unit_check( a->highest_sequence == b->highest_sequence );

        if ( from_last && memo_was_valid )
        {
            repeats_skipped++;
            rocketnet_unit_check( na == 0 );
        }
        if ( full_a != 3 )
        {
            deferred++;
            rocketnet_unit_check( !a->block_memo.valid );
        }

        if ( block.largest )
        {
            older[older_count % 8] = last;
            if ( last.largest )
            {
                older_count++;
            }
            last = block;
        }
    }

    // every kind ran, the repeat path many times, and the ring wrapped

    rocketnet_unit_check( repeats_skipped > steps / 10 );
    rocketnet_unit_check( one_off > steps / 40 );
    rocketnet_unit_check( older_again > steps / 40 );
    rocketnet_unit_check( deferred > steps / 100 );
    rocketnet_unit_check( unmemoized > 0 );
    rocketnet_unit_check( next_sequence > 3 * ROCKETNET_SENT_PACKET_BUFFER_SIZE );

    rocketnet_free( NULL, a );
    rocketnet_free( NULL, b );
}

static void test_ack_block_memo_matches_the_full_apply()
{
    memo_differential( 0x5EED0001ull, 12000 );
    memo_differential( 0xACC0FFEEull, 12000 );
    memo_differential( 0xBADC0DEull, 12000 );
}
REGISTER_UNIT_TEST( test_ack_block_memo_matches_the_full_apply );

// The memo itself: a repeated block is skipped only while nothing is sent.
// A send clears it, and the same block then acks the new sequence; a block
// that deferred an id is never remembered, so its repeat surfaces the id.

static void test_ack_block_memo_cleared_by_send_and_by_deferral()
{
    struct rocketnet_ack_endpoint_t * ep = (struct rocketnet_ack_endpoint_t *)
        rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );
    rocketnet_unit_check( !ep->block_memo.valid );

    for ( uint64_t s = 1; s <= 4; s++ )
    {
        rec_sent( ep, s );
    }

    uint64_t acked[8];
    int acked_count = 0;
    uint64_t unknown = 0, batch_full = 0;
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = 6;                       // 5 and 6 never sent: unknown 2
    block.first_range_length = 6;

    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3000.0, acked, 8, &acked_count, &unknown, &batch_full ) == 4 );
    rocketnet_unit_check( unknown == 2 && acked_count == 4 && ep->block_memo.valid );

    rocketnet_ack_visit_count_reset();
    acked_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3001.0, acked, 8, &acked_count, &unknown, &batch_full ) == 0 );
    rocketnet_unit_check( unknown == 4 && acked_count == 0 && rocketnet_ack_visit_count() == 0 );

    rec_sent( ep, 5 );
    rocketnet_unit_check( !ep->block_memo.valid );
    acked_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3002.0, acked, 8, &acked_count, &unknown, &batch_full ) == 1 );
    rocketnet_unit_check( unknown == 5 && acked_count == 1 && acked[0] == 1000005 );

    // a batch of one defers: the memo stays clear, and the repeat acks the rest

    rec_sent( ep, 6 );
    rec_sent( ep, 7 );
    block.largest = 7;
    block.first_range_length = 2;
    acked_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3003.0, acked, 1, &acked_count, &unknown, &batch_full ) == 1 );
    rocketnet_unit_check( batch_full == 1 && !ep->block_memo.valid );
    acked_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3004.0, acked, 1, &acked_count, &unknown, &batch_full ) == 1 );
    rocketnet_unit_check( batch_full == 1 && acked_count == 1 && ep->block_memo.valid );

    rocketnet_ack_endpoint_reset( ep );
    rocketnet_unit_check( !ep->block_memo.valid );

    rocketnet_free( NULL, ep );
}
REGISTER_UNIT_TEST( test_ack_block_memo_cleared_by_send_and_by_deferral );

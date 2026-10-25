/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_ack.h"
#include "rocketnet_read_write.h"
#include "rocketnet_replay.h"
#include "rocketnet_platform.h"
#include "rocketnet_internal.h"

#include <string.h>
#include <math.h>
#include <limits.h>

#if defined(ROCKETNET_ACK_VISIT_TEST_HOOKS)
// Unit-library instrumentation only: each sequence visit during process_block.
// Per thread, so independent endpoints on different threads never share it;
// production and development libraries carry no such state.
thread_local unsigned long long g_ack_visit_count = 0;
#endif

// -----------------------------------------
//                           the ack block
// -----------------------------------------

int rocketnet_ack_block_write( uint8_t * output, const struct rocketnet_ack_block_t * block )
{
    rocketnet_assert( output );
    rocketnet_assert( block );
    rocketnet_assert( block->num_ranges >= 0 && block->num_ranges <= ROCKETNET_ACK_BLOCK_MAX_RANGES );
    rocketnet_assert( block->first_range_length <= 0xFFFF );

    uint8_t * p = output;
    rocketnet_write_uint32( &p, (uint32_t) ( block->largest & 0xFFFFFFFF ) );
    rocketnet_write_uint8( &p, (uint8_t) block->num_ranges );
    rocketnet_write_uint16( &p, (uint16_t) block->first_range_length );
    for ( int i = 0; i < block->num_ranges; i++ )
    {
        rocketnet_write_uint16( &p, block->range_gap[i] );
        rocketnet_write_uint16( &p, block->range_length[i] );
    }

    return (int) ( p - output );
}

// the replay window is the received state the block is built from: bits are
// meaningful for sequences in (highest_opened - window, highest_opened], and
// the block is silent below that floor -- silence, not a loss claim

static int rocketnet_ack_replay_bit( const struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    int bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
    return ( replay->window[bit / 64] >> ( bit % 64 ) ) & 1;
}

// the empty block (largest 0, first range length 0, no ranges) is written
// exactly while nothing is opened, and every other block has a first range
// length of at least one (tla/AckBlockWrap.tla WriteBlock)

// tla/FragmentsAndAcks.tla SendAck: builds the block the session packet carries from the replay window
// tla/RNSentHistory.tla ReceiverAck: restates the received set inside the window as one ack block
int rocketnet_ack_block_build( uint8_t * output, int capacity, const struct rocketnet_replay_protection_t * replay )
{
    rocketnet_assert( output );
    rocketnet_assert( replay );
    rocketnet_assert( capacity >= ROCKETNET_ACK_BLOCK_MIN_BYTES );

    struct rocketnet_ack_block_t block;
    block.largest = replay->highest_opened;
    block.first_range_length = 0;
    block.num_ranges = 0;

    if ( block.largest != 0 )
    {
        // the enumerable floor: the lowest sequence the window can still
        // speak for, never below one

        uint64_t floor_sequence = 1;
        if ( block.largest >= ROCKETNET_REPLAY_WINDOW_BITS )
            floor_sequence = block.largest - ROCKETNET_REPLAY_WINDOW_BITS + 1;

        int max_ranges = ( capacity - ROCKETNET_ACK_BLOCK_MIN_BYTES ) / 4;
        if ( max_ranges > ROCKETNET_ACK_BLOCK_MAX_RANGES )
            max_ranges = ROCKETNET_ACK_BLOCK_MAX_RANGES;

        // walk downward from largest: the first run, then (gap, run) pairs,
        // newest first, stopping at the floor or when out of room --
        // truncation loses only the oldest information, which the next
        // block restates. a trailing gap with no run below it is dropped:
        // every entry states received sequences.

        uint64_t s = block.largest;

        uint32_t first_run = 0;
        while ( s >= floor_sequence && rocketnet_ack_replay_bit( replay, s ) && first_run < 0xFFFF )
        {
            first_run++;
            if ( s == 1 )
                break;
            s--;
        }
        rocketnet_assert( first_run >= 1 );     // largest itself is always marked received
        block.first_range_length = first_run;

        uint64_t next_high = block.largest - first_run;     // first sequence below the run, 0 when the run reached 1

        while ( next_high >= floor_sequence && block.num_ranges < max_ranges )
        {
            uint32_t gap = 0;
            while ( next_high >= floor_sequence && !rocketnet_ack_replay_bit( replay, next_high ) && gap < 0xFFFF )
            {
                gap++;
                if ( next_high == 1 )
                    break;
                next_high--;
            }

            if ( next_high < floor_sequence || rocketnet_ack_replay_bit( replay, next_high ) == 0 )
                break;                                      // the gap ran to the floor: unspoken, not lost

            uint32_t run = 0;
            while ( next_high >= floor_sequence && rocketnet_ack_replay_bit( replay, next_high ) && run < 0xFFFF )
            {
                run++;
                if ( next_high == 1 )
                {
                    next_high = 0;
                    break;
                }
                next_high--;
            }

            if ( run == 0 )
                break;

            block.range_gap[block.num_ranges] = (uint16_t) gap;
            block.range_length[block.num_ranges] = (uint16_t) run;
            block.num_ranges++;

            if ( next_high == 0 )
                break;
        }
    }

    return rocketnet_ack_block_write( output, &block );
}

uint64_t rocketnet_reconstruct_ack_largest( uint32_t wire_value, uint64_t highest_sent )
{
    // the wire sequence's reconstruction exactly, with expected = the
    // sender's own highest sent session sequence

    return rocketnet_reconstruct_near( wire_value, highest_sent );
}

int rocketnet_ack_block_read( struct rocketnet_ack_block_t * block, const uint8_t * data, int data_bytes, uint64_t highest_sent )
{
    rocketnet_assert( block );
    rocketnet_assert( data );

    if ( data_bytes < ROCKETNET_ACK_BLOCK_MIN_BYTES )
        return 0;

    const uint8_t * p = data;
    uint32_t wire_largest = rocketnet_read_uint32( &p );
    int num_ranges = rocketnet_read_uint8( &p );
    uint32_t first_range_length = rocketnet_read_uint16( &p );

    // the declared geometry must be contained in the opened size

    int block_bytes = ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * num_ranges;
    if ( block_bytes > data_bytes )
        return 0;

    // the empty block is recognised on its wire fields, at any highest sent
    // (tla/AckBlockWrap.tla ReadBlock, EmptyRecognised): wire largest 0,
    // first range length 0 and no ranges is the statement that nothing has
    // been opened yet. Reconstructing first would read a wire 0 as 2^32 once
    // highest sent reaches 2^31 and refuse it
    // (tla/MCAckBlockWrapBrokenHalfRange.cfg). A block naming a sequence
    // always has a first range length of at least one, so a zero first range
    // length with anything else nonzero is malformed, and a genuine largest
    // whose low 32 bits are zero still reads nonempty.

    if ( first_range_length == 0 )
    {
        if ( wire_largest != 0 || num_ranges != 0 )
            return 0;
        block->largest = 0;
        block->first_range_length = 0;
        block->num_ranges = 0;
        return block_bytes;
    }

    // every other block is evaluated on the reconstructed 64-bit largest
    // (tla/AckBlockWrap.tla ReadBlock, LargestExact); a largest that
    // reconstructs to zero cannot hold a first range of one or more and is
    // refused by the bound below

    uint64_t largest = rocketnet_reconstruct_ack_largest( wire_largest, highest_sent );

    if ( (uint64_t) first_range_length > largest )
        return 0;

    // the walk: first range length plus the sum of every gap and length
    // must not exceed largest -- equivalently, the lowest sequence the
    // block describes never falls below one. the sum fits comfortably in 64
    // bits (at most 255 entries of two u16s), so no subtraction is made
    // before it is known to be safe.

    uint64_t total = first_range_length;

    for ( int i = 0; i < num_ranges; i++ )
    {
        uint32_t gap = rocketnet_read_uint16( &p );
        uint32_t length = rocketnet_read_uint16( &p );
        if ( gap == 0 || length == 0 )
            return 0;
        block->range_gap[i] = (uint16_t) gap;
        block->range_length[i] = (uint16_t) length;
        total += gap;
        total += length;
    }

    if ( total > largest )
        return 0;

    block->largest = largest;
    block->first_range_length = first_range_length;
    block->num_ranges = num_ranges;

    return block_bytes;
}

// -----------------------------------------
//                     sender bookkeeping
// -----------------------------------------

static inline bool rocketnet_ack_bit( const uint64_t * bits, uint64_t slot )
{
    return ( bits[slot >> 6] >> ( slot & 63 ) ) & 1;
}

static inline void rocketnet_ack_set_bit( uint64_t * bits, uint64_t slot )
{
    bits[slot >> 6] |= 1ull << ( slot & 63 );
}

static bool rocketnet_sent_record_state_equal( const rocketnet_sent_packet_record_t * a, const rocketnet_sent_packet_record_t * b )
{
    return a->sequence == b->sequence &&
           a->packet_id == b->packet_id &&
           a->aggregate_id == b->aggregate_id &&
           a->send_time == b->send_time &&
           a->wire_bytes == b->wire_bytes &&
           a->acked == b->acked &&
           a->has_packet_id == b->has_packet_id &&
           a->was_keep_alive == b->was_keep_alive;
}

static bool rocketnet_received_record_state_equal( const rocketnet_received_packet_record_t * a, const rocketnet_received_packet_record_t * b )
{
    return a->sequence == b->sequence &&
           a->receive_time == b->receive_time &&
           a->wire_bytes == b->wire_bytes;
}

static bool rocketnet_aggregate_state_equal( const rocketnet_packet_aggregate_t * a, const rocketnet_packet_aggregate_t * b )
{
    return a->id == b->id &&
           a->packet_id == b->packet_id &&
           a->remaining == b->remaining;
}

bool rocketnet_ack_endpoint_state_equal( const struct rocketnet_ack_endpoint_t * a, const struct rocketnet_ack_endpoint_t * b )
{
    rocketnet_assert( a && b );
    if ( memcmp( a, b, offsetof( rocketnet_ack_endpoint_t, sent ) ) != 0 )
        return false;
    if ( memcmp( a->rtt_history, b->rtt_history, sizeof(a->rtt_history) ) != 0 )
        return false;
    for ( uint64_t i = 0; i < ROCKETNET_SENT_PACKET_BUFFER_SIZE; i++ )
        if ( rocketnet_ack_bit( a->sent_written, i ) && !rocketnet_sent_record_state_equal( &a->sent[i], &b->sent[i] ) )
            return false;
    for ( uint64_t i = 0; i < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE; i++ )
        if ( rocketnet_ack_bit( a->received_written, i ) && !rocketnet_received_record_state_equal( &a->received[i], &b->received[i] ) )
            return false;
    for ( uint64_t i = 0; i < ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE; i++ )
        if ( rocketnet_ack_bit( a->aggregates_written, i ) && !rocketnet_aggregate_state_equal( &a->aggregates[i], &b->aggregates[i] ) )
            return false;
    return true;
}

#define ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS 32
#define ROCKETNET_ACK_RECEIVED_STATS_BLOCKS \
    ( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS )
#define ROCKETNET_ACK_RECEIVED_STATS_WORDS_PER_BLOCK 8
#define ROCKETNET_ACK_RECEIVED_STATS_STORAGE_RECORDS \
    ( ROCKETNET_ACK_RECEIVED_STATS_BLOCKS * ROCKETNET_ACK_RECEIVED_STATS_WORDS_PER_BLOCK )

static_assert( ROCKETNET_ACK_RECEIVED_STATS_STORAGE_RECORDS <= ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE,
               "received block summaries fit in reclaimed record words" );

// tla/RNWrittenRing.tla Reset. Every field but the three record rings, which
// a written bit guards (Write sets it, every Read checks it first): the
// rings' payloads are never zeroed. Reset also clears the compact summaries
// in the first 512 records' reclaimed stats words (2 KB across 12 KB of ring),
// without touching sequence/time/bytes.
void rocketnet_ack_endpoint_reset( struct rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_assert( endpoint );
    static_assert( offsetof( rocketnet_ack_endpoint_t, received ) > offsetof( rocketnet_ack_endpoint_t, sent ) &&
                   offsetof( rocketnet_ack_endpoint_t, aggregates ) > offsetof( rocketnet_ack_endpoint_t, received ) &&
                   offsetof( rocketnet_ack_endpoint_t, rtt_history ) > offsetof( rocketnet_ack_endpoint_t, aggregates ),
                   "the record rings are one span, before the rtt history" );
    memset( (void*) endpoint, 0, offsetof( rocketnet_ack_endpoint_t, sent ) );
    for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        endpoint->rtt_history[i] = -1.0f;
    for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        endpoint->stats_rtt_min_tree[i] = 1e9f;                 // ROCKETNET_ACK_RTT_NONE: no positive sample
    for ( int i = 0; i < ROCKETNET_ACK_RECEIVED_STATS_STORAGE_RECORDS; ++i )
        endpoint->received[i].stats_word = 0;
}

// -----------------------------------------
//                 the running stats aggregates
// -----------------------------------------

// tla/RNServerStats.tla. The aggregates hold exactly what
// rocketnet_ack_endpoint_update_stats would count over the same windows, so
// the published numbers are current after every send, receive and ack. Every
// published field matches the recompute exactly except jitter, which matches
// within floating-point rounding: rocketnet_ack_stats_publish_rtt takes
// ( sum - count * min ) / count from a double sum, while the recompute sums
// sample - min in float.
// Every window is a function of its high-water mark. Sent counters move their
// edges incrementally. Received records use fixed 32-slot physical blocks:
// advancing retention clears whole summaries and rescans at most two edge
// blocks, a receive rebuilds its one block, and the older-half query reads
// summaries plus at most two edges. The fixed worst case is at most 256
// record/block visits and cannot grow into the old quadratic insertion walk.

#define ROCKETNET_ACK_STATS_SENT_HALF ( (uint64_t) ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 )
#define ROCKETNET_ACK_STATS_RECEIVED_HALF ( (uint64_t) ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2 )
#define ROCKETNET_ACK_RTT_NONE 1e9f

static inline uint64_t rocketnet_ack_stats_window_base( uint64_t highest, uint64_t ring )
{
    return highest >= ring ? highest - ring + 1 : 1;
}

static inline uint64_t rocketnet_ack_stats_window_top( uint64_t highest, uint64_t ring )
{
    const uint64_t top = rocketnet_ack_stats_window_base( highest, ring ) + ring / 2 - 1;
    return highest < top ? highest : top;
}

// the loss-eligible part of the sent window ends here: the newest half of
// the ring is still in flight (0 = nothing eligible)
static inline uint64_t rocketnet_ack_stats_eligible_top( uint64_t highest )
{
    return highest > ROCKETNET_ACK_STATS_SENT_HALF ? highest - ROCKETNET_ACK_STATS_SENT_HALF : 0;
}

static inline bool rocketnet_ack_stats_sent_present( const struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence )
{
    const uint64_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    return rocketnet_ack_bit( endpoint->sent_written, slot ) && endpoint->sent[slot].sequence == sequence;
}

// the window edges move one record per send or receive, each endpoint in
// turn: fetching the edge records a little ahead keeps the next updates'
// edge reads in cache instead of a memory miss per endpoint per edge
static inline void rocketnet_ack_stats_prefetch( const void * address )
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch( address );
#elif defined(_MSC_VER) && ( defined(_M_X64) || defined(_M_IX86) )
    _mm_prefetch( (const char*) address, _MM_HINT_T0 );
#else
    (void) address;
#endif
}

static inline int rocketnet_ack_ctz64( uint64_t v )
{
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward64( &idx, v );
    return (int) idx;
#else
    return __builtin_ctzll( v );
#endif
}

// the lowest acknowledged record in [from, to], or 0. Acknowledged records
// are held and not unacknowledged in the shadow bitmaps, so whole words of
// unacknowledged or empty slots are skipped; a candidate is checked against
// its record, since a slot's bits can still name a record below the window.
static uint64_t rocketnet_ack_stats_lowest_acked( const struct rocketnet_ack_endpoint_t * endpoint, uint64_t from, uint64_t to )
{
    uint64_t sequence = from;
    while ( sequence <= to )
    {
        const uint64_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
        const uint64_t word = ( endpoint->sent_held[slot >> 6] & ~endpoint->sent_unacked[slot >> 6] ) >> ( slot & 63 );
        if ( !word )
        {
            sequence += 64 - ( slot & 63 );
            continue;
        }
        sequence += (uint64_t) rocketnet_ack_ctz64( word );
        if ( sequence > to )
            break;
        if ( rocketnet_ack_stats_sent_present( endpoint, sequence ) && endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked )
            return sequence;
        sequence++;
    }
    return 0;
}

static void rocketnet_ack_stats_sent_add_acked( struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence,
                                                const struct rocketnet_sent_packet_record_t * record )
{
    endpoint->stats_acked_count++;
    endpoint->stats_acked_bytes += record->wire_bytes;
    if ( !endpoint->stats_acked_low || sequence < endpoint->stats_acked_low )
    {
        endpoint->stats_acked_low = sequence;
        endpoint->stats_acked_low_time = record->send_time;
    }
    if ( sequence > endpoint->stats_acked_high )
    {
        endpoint->stats_acked_high = sequence;
        endpoint->stats_acked_high_time = record->send_time;
    }
}

// counts a present record that lies in the window; eligible says whether it
// lies in the loss-eligible part too
static void rocketnet_ack_stats_sent_add( struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, bool eligible )
{
    const struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    endpoint->stats_sent_count++;
    endpoint->stats_sent_bytes += record->wire_bytes;
    if ( !endpoint->stats_sent_low || sequence < endpoint->stats_sent_low )
    {
        endpoint->stats_sent_low = sequence;
        endpoint->stats_sent_low_time = record->send_time;
    }
    if ( sequence > endpoint->stats_sent_high )
    {
        endpoint->stats_sent_high = sequence;
        endpoint->stats_sent_high_time = record->send_time;
    }
    if ( record->acked )
        rocketnet_ack_stats_sent_add_acked( endpoint, sequence, record );
    if ( eligible )
    {
        endpoint->stats_eligible_count++;
        if ( !record->acked )
            endpoint->stats_eligible_unacked++;
    }
}

static void rocketnet_ack_stats_sent_clear( struct rocketnet_ack_endpoint_t * endpoint )
{
    endpoint->stats_sent_bytes = 0;
    endpoint->stats_acked_bytes = 0;
    endpoint->stats_sent_low = 0;
    endpoint->stats_sent_high = 0;
    endpoint->stats_acked_low = 0;
    endpoint->stats_acked_high = 0;
    endpoint->stats_sent_low_time = 0.0;
    endpoint->stats_sent_high_time = 0.0;
    endpoint->stats_acked_low_time = 0.0;
    endpoint->stats_acked_high_time = 0.0;
    endpoint->stats_sent_count = 0;
    endpoint->stats_acked_count = 0;
    endpoint->stats_eligible_count = 0;
    endpoint->stats_eligible_unacked = 0;
}

// moves the sent window from the current high-water mark to highest, before
// the mark is raised and before the new record is written
// tla/RNServerStats.tla Slide: moves the sent window to the new high-water mark, removing, crossing and entering records
static void rocketnet_ack_stats_sent_advance( struct rocketnet_ack_endpoint_t * endpoint, uint64_t highest )
{
    const uint64_t ring = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    const uint64_t old_highest = endpoint->highest_sequence;
    if ( highest <= old_highest )
        return;

    const uint64_t old_base = rocketnet_ack_stats_window_base( old_highest, ring );
    const uint64_t old_top = rocketnet_ack_stats_window_top( old_highest, ring );
    const uint64_t old_eligible = rocketnet_ack_stats_eligible_top( old_highest );
    const uint64_t base = rocketnet_ack_stats_window_base( highest, ring );
    const uint64_t top = rocketnet_ack_stats_window_top( highest, ring );
    const uint64_t eligible = rocketnet_ack_stats_eligible_top( highest );

    // leaving: the counted records below the new base
    if ( base > old_top )
    {
        rocketnet_ack_stats_sent_clear( endpoint );
    }
    else
    {
        for ( uint64_t sequence = old_base; sequence < base; sequence++ )
        {
            if ( !rocketnet_ack_stats_sent_present( endpoint, sequence ) )
                continue;
            const struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ring];
            endpoint->stats_sent_count--;
            endpoint->stats_sent_bytes -= record->wire_bytes;
            if ( record->acked )
            {
                endpoint->stats_acked_count--;
                endpoint->stats_acked_bytes -= record->wire_bytes;
            }
            if ( sequence <= old_eligible )
            {
                endpoint->stats_eligible_count--;
                if ( !record->acked )
                    endpoint->stats_eligible_unacked--;
            }
        }

        // the low bounds left with their records: the next counted one is
        // the lowest survivor in [base, old_top]. the high bounds survive
        // unless nothing does.
        if ( endpoint->stats_sent_count == 0 )
        {
            rocketnet_ack_stats_sent_clear( endpoint );
        }
        else
        {
            if ( endpoint->stats_sent_low < base )
            {
                uint64_t sequence = base;
                while ( !rocketnet_ack_stats_sent_present( endpoint, sequence ) )
                    sequence++;
                endpoint->stats_sent_low = sequence;
                endpoint->stats_sent_low_time = endpoint->sent[sequence % ring].send_time;
            }
            if ( endpoint->stats_acked_count == 0 )
            {
                endpoint->stats_acked_low = 0;
                endpoint->stats_acked_high = 0;
                endpoint->stats_acked_low_time = 0.0;
                endpoint->stats_acked_high_time = 0.0;
            }
            else if ( endpoint->stats_acked_low < base )
            {
                const uint64_t sequence = rocketnet_ack_stats_lowest_acked( endpoint, base, old_top );
                rocketnet_assert( sequence != 0 );
                endpoint->stats_acked_low = sequence;
                endpoint->stats_acked_low_time = endpoint->sent[sequence % ring].send_time;
            }
        }

        // crossing into the loss-eligible part: survivors already counted
        // in the window
        const uint64_t cross_low = old_eligible + 1 > base ? old_eligible + 1 : base;
        const uint64_t cross_high = eligible < old_top ? eligible : old_top;
        for ( uint64_t sequence = cross_low; sequence <= cross_high; sequence++ )
        {
            if ( !rocketnet_ack_stats_sent_present( endpoint, sequence ) )
                continue;
            endpoint->stats_eligible_count++;
            if ( !endpoint->sent[sequence % ring].acked )
                endpoint->stats_eligible_unacked++;
        }
    }

    // entering: records between the old top and the new one. nothing above
    // the old mark is recorded yet, so a jump enters nothing it has to visit
    uint64_t enter_low = old_top + 1 > base ? old_top + 1 : base;
    const uint64_t enter_high = top < old_highest ? top : old_highest;
    for ( uint64_t sequence = enter_low; sequence <= enter_high; sequence++ )
        if ( rocketnet_ack_stats_sent_present( endpoint, sequence ) )
            rocketnet_ack_stats_sent_add( endpoint, sequence, sequence <= eligible );

    rocketnet_ack_stats_prefetch( &endpoint->sent[( base + 2 ) % ring] );
    rocketnet_ack_stats_prefetch( &endpoint->sent[( top + 2 ) % ring] );
}

static inline void rocketnet_ack_stats_received_visit( struct rocketnet_ack_endpoint_t * endpoint )
{
#if ROCKETNET_DEV
    endpoint->stats_received_operations++;
#else
    (void) endpoint;
#endif
}

struct rocketnet_ack_stats_received_block_t
{
    uint64_t bytes;
    double low_time;
    double high_time;
    uint32_t count;
    uint8_t first_is_nan;
    uint8_t pad[3];
};

static_assert( sizeof(rocketnet_ack_stats_received_block_t) == 32,
               "received block aggregate size" );

static inline rocketnet_ack_stats_received_block_t rocketnet_ack_stats_received_empty()
{
    rocketnet_ack_stats_received_block_t result = {};
    return result;
}

static inline double rocketnet_ack_stats_received_min( double a, double b )
{
    if ( isnan(a) ) return b;
    if ( isnan(b) ) return a;
    return b < a ? b : a;
}

static inline double rocketnet_ack_stats_received_max( double a, double b )
{
    if ( isnan(a) ) return b;
    if ( isnan(b) ) return a;
    return b > a ? b : a;
}

// Merge in physical-slot order, which is sequence order inside each
// non-wrapping query piece. first_is_nan keeps the scan oracle's first
// present value; low/high ignore later NaNs exactly as its comparisons do.
static rocketnet_ack_stats_received_block_t rocketnet_ack_stats_received_merge(
    const rocketnet_ack_stats_received_block_t & left,
    const rocketnet_ack_stats_received_block_t & right )
{
    if ( !left.count ) return right;
    if ( !right.count ) return left;
    rocketnet_ack_stats_received_block_t result;
    result.bytes = left.bytes + right.bytes;
    result.low_time = rocketnet_ack_stats_received_min( left.low_time, right.low_time );
    result.high_time = rocketnet_ack_stats_received_max( left.high_time, right.high_time );
    result.count = left.count + right.count;
    result.first_is_nan = left.first_is_nan;
    memset( result.pad, 0, sizeof(result.pad) );
    return result;
}

static inline uint32_t rocketnet_ack_stats_received_word( const struct rocketnet_ack_endpoint_t * endpoint,
                                                          int block, int word )
{
    return endpoint->received[block * ROCKETNET_ACK_RECEIVED_STATS_WORDS_PER_BLOCK + word].stats_word;
}

static inline void rocketnet_ack_stats_received_set_word( struct rocketnet_ack_endpoint_t * endpoint,
                                                          int block, int word, uint32_t value )
{
    endpoint->received[block * ROCKETNET_ACK_RECEIVED_STATS_WORDS_PER_BLOCK + word].stats_word = value;
}

static uint64_t rocketnet_ack_stats_received_u64( const struct rocketnet_ack_endpoint_t * endpoint,
                                                  int block, int word )
{
    return (uint64_t) rocketnet_ack_stats_received_word( endpoint, block, word ) |
           ( (uint64_t) rocketnet_ack_stats_received_word( endpoint, block, word + 1 ) << 32 );
}

static void rocketnet_ack_stats_received_set_u64( struct rocketnet_ack_endpoint_t * endpoint,
                                                  int block, int word, uint64_t value )
{
    rocketnet_ack_stats_received_set_word( endpoint, block, word, (uint32_t) value );
    rocketnet_ack_stats_received_set_word( endpoint, block, word + 1, (uint32_t) ( value >> 32 ) );
}

static double rocketnet_ack_stats_received_double( const struct rocketnet_ack_endpoint_t * endpoint,
                                                   int block, int word )
{
    const uint64_t bits = rocketnet_ack_stats_received_u64( endpoint, block, word );
    double value;
    memcpy( &value, &bits, sizeof(value) );
    return value;
}

static void rocketnet_ack_stats_received_set_double( struct rocketnet_ack_endpoint_t * endpoint,
                                                     int block, int word, double value )
{
    uint64_t bits;
    memcpy( &bits, &value, sizeof(bits) );
    rocketnet_ack_stats_received_set_u64( endpoint, block, word, bits );
}

static rocketnet_ack_stats_received_block_t rocketnet_ack_stats_received_read_block(
    const struct rocketnet_ack_endpoint_t * endpoint, int block )
{
    rocketnet_ack_stats_received_block_t result;
    result.bytes = rocketnet_ack_stats_received_u64( endpoint, block, 0 );
    result.low_time = rocketnet_ack_stats_received_double( endpoint, block, 2 );
    result.high_time = rocketnet_ack_stats_received_double( endpoint, block, 4 );
    result.count = rocketnet_ack_stats_received_word( endpoint, block, 6 );
    result.first_is_nan = (uint8_t) rocketnet_ack_stats_received_word( endpoint, block, 7 );
    memset( result.pad, 0, sizeof(result.pad) );
    return result;
}

static void rocketnet_ack_stats_received_write_block( struct rocketnet_ack_endpoint_t * endpoint, int block,
                                                      const rocketnet_ack_stats_received_block_t & value )
{
    rocketnet_ack_stats_received_set_u64( endpoint, block, 0, value.bytes );
    rocketnet_ack_stats_received_set_double( endpoint, block, 2, value.low_time );
    rocketnet_ack_stats_received_set_double( endpoint, block, 4, value.high_time );
    rocketnet_ack_stats_received_set_word( endpoint, block, 6, value.count );
    rocketnet_ack_stats_received_set_word( endpoint, block, 7, value.first_is_nan );
}

static inline rocketnet_ack_stats_received_block_t rocketnet_ack_stats_received_record(
    const struct rocketnet_received_packet_record_t * record )
{
    rocketnet_ack_stats_received_block_t result;
    result.bytes = record->wire_bytes;
    result.low_time = record->receive_time;
    result.high_time = record->receive_time;
    result.count = 1;
    result.first_is_nan = isnan(record->receive_time) ? 1 : 0;
    memset( result.pad, 0, sizeof(result.pad) );
    return result;
}

static void rocketnet_ack_stats_received_rebuild_block( struct rocketnet_ack_endpoint_t * endpoint,
                                                        int block, uint64_t highest )
{
    rocketnet_ack_stats_received_block_t aggregate = rocketnet_ack_stats_received_empty();
    const uint64_t base = rocketnet_ack_stats_window_base( highest, ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    const int first = block * ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS;
    for ( int slot = first; slot < first + ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS; ++slot )
    {
        rocketnet_ack_stats_received_visit( endpoint );
        const struct rocketnet_received_packet_record_t * record = &endpoint->received[slot];
        if ( rocketnet_ack_bit( endpoint->received_written, (uint64_t) slot ) &&
             record->sequence >= base && record->sequence <= highest )
            aggregate = rocketnet_ack_stats_received_merge( aggregate, rocketnet_ack_stats_received_record( record ) );
    }
    rocketnet_ack_stats_received_write_block( endpoint, block, aggregate );
}

static void rocketnet_ack_stats_received_clear_blocks( struct rocketnet_ack_endpoint_t * endpoint )
{
    const rocketnet_ack_stats_received_block_t empty = rocketnet_ack_stats_received_empty();
    for ( int block = 0; block < ROCKETNET_ACK_RECEIVED_STATS_BLOCKS; ++block )
    {
        rocketnet_ack_stats_received_visit( endpoint );
        rocketnet_ack_stats_received_write_block( endpoint, block, empty );
    }
}

static void rocketnet_ack_stats_received_clear_physical_range( struct rocketnet_ack_endpoint_t * endpoint,
                                                               int from, int to, uint64_t highest )
{
    if ( from >= to )
        return;
    const rocketnet_ack_stats_received_block_t empty = rocketnet_ack_stats_received_empty();
    const int first_block = from / ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS;
    const int last_block = ( to - 1 ) / ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS;
    for ( int block = first_block; block <= last_block; ++block )
    {
        rocketnet_ack_stats_received_visit( endpoint );
        const int block_from = block * ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS;
        const int block_to = block_from + ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS;
        if ( from <= block_from && block_to <= to )
            rocketnet_ack_stats_received_write_block( endpoint, block, empty );
        else
            rocketnet_ack_stats_received_rebuild_block( endpoint, block, highest );
    }
}

// Clears the physical slots leaving the full retained window. Whole blocks
// cost one visit and at most two edge blocks are rescanned.
// tla/RNServerStats.tla Slide: fixed-block retention update.
static void rocketnet_ack_stats_received_advance( struct rocketnet_ack_endpoint_t * endpoint, uint64_t highest )
{
    const uint64_t old_highest = endpoint->highest_received_sequence;
    if ( highest <= old_highest || old_highest == 0 )
        return;
    const uint64_t old_base = rocketnet_ack_stats_window_base( old_highest, ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    const uint64_t base = rocketnet_ack_stats_window_base( highest, ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    const uint64_t count = base - old_base;
    if ( !count )
        return;
    if ( count >= ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE )
    {
        rocketnet_ack_stats_received_clear_blocks( endpoint );
        return;
    }
    const int start = (int) ( old_base % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    const int end = start + (int) count;
    rocketnet_ack_stats_received_clear_physical_range(
        endpoint, start, end < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE ? end : ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE, highest );
    if ( end > ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE )
        rocketnet_ack_stats_received_clear_physical_range(
            endpoint, 0, end - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE, highest );
}

static rocketnet_ack_stats_received_block_t rocketnet_ack_stats_received_query(
    struct rocketnet_ack_endpoint_t * endpoint, uint64_t first, uint64_t last )
{
    rocketnet_ack_stats_received_block_t aggregate = rocketnet_ack_stats_received_empty();
    uint64_t sequence = first;
    uint64_t remaining = last - first + 1;
    while ( remaining )
    {
        const int slot = (int) ( sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
        const int offset = slot % ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS;
        uint64_t take = (uint64_t) ( ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS - offset );
        if ( take > remaining ) take = remaining;
        if ( offset == 0 && take == ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS )
        {
            rocketnet_ack_stats_received_visit( endpoint );
            aggregate = rocketnet_ack_stats_received_merge(
                aggregate, rocketnet_ack_stats_received_read_block(
                    endpoint, slot / ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS ) );
        }
        else
        {
            for ( uint64_t i = 0; i < take; ++i )
            {
                rocketnet_ack_stats_received_visit( endpoint );
                const int record_slot = slot + (int) i;
                const struct rocketnet_received_packet_record_t * record = &endpoint->received[record_slot];
                if ( rocketnet_ack_bit( endpoint->received_written, (uint64_t) record_slot ) &&
                     record->sequence == sequence + i )
                    aggregate = rocketnet_ack_stats_received_merge(
                        aggregate, rocketnet_ack_stats_received_record( record ) );
            }
        }
        remaining -= take;
        if ( remaining ) sequence += take;
    }
    return aggregate;
}

static void rocketnet_ack_stats_received_refresh( struct rocketnet_ack_endpoint_t * endpoint )
{
    const uint64_t base = rocketnet_ack_stats_window_base(
        endpoint->highest_received_sequence, ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    const uint64_t top = rocketnet_ack_stats_window_top(
        endpoint->highest_received_sequence, ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    const rocketnet_ack_stats_received_block_t aggregate =
        rocketnet_ack_stats_received_query( endpoint, base, top );
    endpoint->stats_received_count = aggregate.count;
    endpoint->stats_received_bytes = aggregate.bytes;
    if ( aggregate.count )
    {
        endpoint->stats_received_low_time = aggregate.first_is_nan ? nan("") : aggregate.low_time;
        endpoint->stats_received_high_time = aggregate.first_is_nan ? nan("") : aggregate.high_time;
    }
    else
    {
        endpoint->stats_received_low_time = 0.0;
        endpoint->stats_received_high_time = 0.0;
    }
}

static inline float rocketnet_ack_stats_rtt_node( const struct rocketnet_ack_endpoint_t * endpoint, int node )
{
    if ( node < ROCKETNET_RTT_HISTORY_SIZE )
        return endpoint->stats_rtt_min_tree[node];
    const float sample = endpoint->rtt_history[node - ROCKETNET_RTT_HISTORY_SIZE];
    return sample > 0.0f && sample < ROCKETNET_ACK_RTT_NONE ? sample : ROCKETNET_ACK_RTT_NONE;
}

// writes one rtt history entry and keeps the positive sum, count and the
// min tree with it
static void rocketnet_ack_stats_rtt_write( struct rocketnet_ack_endpoint_t * endpoint, int index, float sample )
{
    const float old_sample = endpoint->rtt_history[index];
    if ( old_sample > 0.0f )
    {
        endpoint->stats_rtt_sum -= (double) old_sample;
        endpoint->stats_rtt_count--;
    }
    endpoint->rtt_history[index] = sample;
    if ( sample > 0.0f )
    {
        endpoint->stats_rtt_sum += (double) sample;
        endpoint->stats_rtt_count++;
    }
    // a node whose minimum does not change leaves every ancestor unchanged
    for ( int node = ( index + ROCKETNET_RTT_HISTORY_SIZE ) >> 1; node >= 1; node >>= 1 )
    {
        const float left = rocketnet_ack_stats_rtt_node( endpoint, 2 * node );
        const float right = rocketnet_ack_stats_rtt_node( endpoint, 2 * node + 1 );
        const float minimum = left < right ? left : right;
        if ( endpoint->stats_rtt_min_tree[node] == minimum )
            break;
        endpoint->stats_rtt_min_tree[node] = minimum;
    }
}

// publish the aggregates to the fields the transports read, with the
// reference's arithmetic. each event republishes only what it can change:
// a send the sent numbers, a receive the received rate, an ack block the
// rtt and the sent numbers.

static void rocketnet_ack_stats_publish_rtt( struct rocketnet_ack_endpoint_t * endpoint )
{
    const float root = endpoint->stats_rtt_min_tree[1];
    const float min_rtt = root < ROCKETNET_ACK_RTT_NONE ? root : 0.0f;
    endpoint->rtt = min_rtt;
    endpoint->jitter = endpoint->stats_rtt_count > 0 ?
        (float) ( ( endpoint->stats_rtt_sum - (double) endpoint->stats_rtt_count * (double) min_rtt ) / (double) endpoint->stats_rtt_count ) : 0.0f;
}

static void rocketnet_ack_stats_publish_sent( struct rocketnet_ack_endpoint_t * endpoint )
{
    endpoint->packet_loss = endpoint->stats_eligible_count > 0 ?
        ( (float) endpoint->stats_eligible_unacked ) / ( (float) endpoint->stats_eligible_count ) * 100.0f : 0.0f;

    const double sent_span = endpoint->stats_sent_high_time - endpoint->stats_sent_low_time;
    endpoint->bandwidth_sent_kbps = ( endpoint->stats_sent_count > 0 && sent_span > 0.0 ) ?
        (float) ( ( (double) endpoint->stats_sent_bytes ) / sent_span * 8.0 / 1000.0 ) : 0.0f;

    const double acked_span = endpoint->stats_acked_high_time - endpoint->stats_acked_low_time;
    endpoint->bandwidth_acked_kbps = ( endpoint->stats_acked_count > 0 && acked_span > 0.0 ) ?
        (float) ( ( (double) endpoint->stats_acked_bytes ) / acked_span * 8.0 / 1000.0 ) : 0.0f;
}

static void rocketnet_ack_stats_publish_received( struct rocketnet_ack_endpoint_t * endpoint )
{
    const double received_span = endpoint->stats_received_high_time - endpoint->stats_received_low_time;
    endpoint->bandwidth_received_kbps = ( endpoint->stats_received_count > 0 && received_span > 0.0 ) ?
        (float) ( ( (double) endpoint->stats_received_bytes ) / received_span * 8.0 / 1000.0 ) : 0.0f;
}

// tla/RNSentHistory.tla Send
// tla/FragmentsAndAcks.tla StartPayload: writes the one sequence's record with its packet id and sets its held and unacked bits
// tla/RNServerStats.tla Send: takes the sent sequence (past the high-water mark, or a refused stale one)
// tla/RNServerStats.tla Write: removes the overwritten record from the aggregates, writes the ring slot and adds it
enum rocketnet_ack_sent_result_t rocketnet_ack_endpoint_record_sent_classified( struct rocketnet_ack_endpoint_t * endpoint,
                                                                                uint64_t sequence, int wire_bytes,
                                                                                uint64_t packet_id, int has_packet_id,
                                                                                uint64_t aggregate_id, double time )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( sequence != 0 );
    rocketnet_assert( !( has_packet_id && aggregate_id != 0 ) );        // a fragment's packet id lives in its aggregate

    if ( !sequence || !isfinite(time) )
        return ROCKETNET_ACK_SENT_REFUSED_STALE;
    rocketnet_platform_spin_lock_u64( &endpoint->sent_lock );
    struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    // an unwritten slot is the empty record: sequence 0, nothing to evict
    const bool written = rocketnet_ack_bit( endpoint->sent_written, sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE );
    if ( ( written && record->sequence >= sequence ) ||
         ( endpoint->highest_sequence >= ROCKETNET_SENT_PACKET_BUFFER_SIZE &&
           sequence <= endpoint->highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE ) )
    {
        rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );
        return ROCKETNET_ACK_SENT_REFUSED_STALE;
    }

    // the record this write replaces: empty or acknowledged is a clean
    // write; unacknowledged is an eviction, and the receiver having
    // acknowledged a sequence ROCKETNET_LOSS_PACKET_THRESHOLD above it is
    // what makes the eviction a known path loss (RFC 9002 6.1.1) rather than
    // an unknown fate. largest_acked_sequence is written by the update
    // thread alone, which never overlaps the fill phase.

    enum rocketnet_ack_sent_result_t result = ROCKETNET_ACK_SENT_RECORDED;
    if ( written && record->sequence && !record->acked )
    {
        result = record->sequence + ROCKETNET_LOSS_PACKET_THRESHOLD <= endpoint->largest_acked_sequence
                     ? ROCKETNET_ACK_SENT_EVICTED_LOST
                     : ROCKETNET_ACK_SENT_EVICTED_UNKNOWN;
    }

    // the stats window moves before the replaced record is overwritten:
    // that record lies below the new window, so it has already left it
    rocketnet_ack_stats_sent_advance( endpoint, sequence > endpoint->highest_sequence ? sequence : endpoint->highest_sequence );
    record->sequence = sequence;
    record->packet_id = packet_id;
    record->aggregate_id = aggregate_id;
    record->send_time = time;
    record->wire_bytes = (uint32_t) wire_bytes;
    record->acked = 0;
    record->has_packet_id = (uint8_t) ( has_packet_id != 0 );
    record->was_keep_alive = 0;

    // RNSentHistory Send writes the record; the shadow bitmaps mark the slot
    // held and unacknowledged so the walk (ReceiveAck) can skip everything
    // else. The write is under sent_lock with the record, so the update
    // thread's reconcile and walk never see a half-published slot.

    uint32_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    rocketnet_ack_set_bit( endpoint->sent_written, slot );
    endpoint->sent_held[slot >> 6] |= 1ull << ( slot & 63 );
    endpoint->sent_unacked[slot >> 6] |= 1ull << ( slot & 63 );

    // fill-phase threads record concurrently under distinct sequences, so
    // the high-water fold is a compare-exchange max: whatever the
    // interleaving, the mark ends at the largest recorded sequence

    rocketnet_platform_atomic_max_u64( &endpoint->highest_sequence, sequence );
    if ( sequence >= rocketnet_ack_stats_window_base( endpoint->highest_sequence, ROCKETNET_SENT_PACKET_BUFFER_SIZE ) &&
         sequence <= rocketnet_ack_stats_window_top( endpoint->highest_sequence, ROCKETNET_SENT_PACKET_BUFFER_SIZE ) )
        rocketnet_ack_stats_sent_add( endpoint, sequence, sequence <= rocketnet_ack_stats_eligible_top( endpoint->highest_sequence ) );
    rocketnet_ack_stats_publish_sent( endpoint );
    rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );
    return result;
}

// tla/RNSentHistory.tla Send
bool rocketnet_ack_endpoint_record_sent( struct rocketnet_ack_endpoint_t * endpoint,
                                         uint64_t sequence, int wire_bytes,
                                         uint64_t packet_id, int has_packet_id,
                                         uint64_t aggregate_id, double time )
{
    return rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, wire_bytes, packet_id, has_packet_id,
                                                          aggregate_id, time ) == ROCKETNET_ACK_SENT_RECORDED;
}

// tla/RNSentHistory.tla Send
void rocketnet_ack_count_sent_result( enum rocketnet_ack_sent_result_t result, uint64_t * lost_counter, uint64_t * overflow_counter )
{
    rocketnet_assert( lost_counter );
    rocketnet_assert( overflow_counter );

    switch ( result )
    {
        case ROCKETNET_ACK_SENT_RECORDED:
            break;
        case ROCKETNET_ACK_SENT_EVICTED_LOST:
            rocketnet_platform_atomic_add_u64( lost_counter, 1 );
            break;
        case ROCKETNET_ACK_SENT_EVICTED_UNKNOWN:
        case ROCKETNET_ACK_SENT_REFUSED_STALE:
            rocketnet_platform_atomic_add_u64( overflow_counter, 1 );
            break;
    }
}

void rocketnet_ack_endpoint_mark_keep_alive( struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( sequence != 0 );

    // Serialize the compare and tag with aliased record publication.
    rocketnet_platform_spin_lock_u64( &endpoint->sent_lock );
    auto * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    if ( rocketnet_ack_bit( endpoint->sent_written, sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE ) && record->sequence == sequence )
        record->was_keep_alive = 1;
    rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );
}

// tla/FragmentsAndAcks.tla StartFragmented: draws the aggregate id and pins the packet id behind cnt fragment acks
uint64_t rocketnet_ack_endpoint_open_aggregate( struct rocketnet_ack_endpoint_t * endpoint,
                                                uint64_t packet_id, int fragment_count )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( fragment_count >= 2 );

    // Claim and publish together: arbitrarily many fill threads may lap the
    // finite aggregate ring, and an older writer must not replace a newer one.
    rocketnet_platform_spin_lock_u64( &endpoint->sent_lock );
    uint64_t id = rocketnet_platform_atomic_add_u64( &endpoint->num_aggregates, 1 ) + 1;

    struct rocketnet_packet_aggregate_t * aggregate = &endpoint->aggregates[id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
    rocketnet_ack_set_bit( endpoint->aggregates_written, id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE );
    aggregate->id = id;
    aggregate->packet_id = packet_id;
    aggregate->remaining = (uint32_t) fragment_count;
    rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );

    return id;
}

// tla/RNServerStats.tla Send: takes any received sequence the remote end chose
// tla/RNServerStats.tla Write: writes the exact-tagged ring slot and rebuilds its block
void rocketnet_ack_endpoint_record_received( struct rocketnet_ack_endpoint_t * endpoint,
                                             uint64_t sequence, int wire_bytes, double time )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( sequence != 0 );
#if ROCKETNET_DEV
    endpoint->stats_received_operations = 0;
#endif

    // refused: a sequence at least one received ring behind the high-water
    // mark. Replay admits sequences that far back (its window is wider than
    // this ring), but such a sequence is below the stats window base, so it
    // is never counted, and its slot holds a newer sequence of [base,
    // highest] that the write would erase (tla/RNServerStats.tla Write,
    // invariant RetainedIsNewest)
    if ( endpoint->highest_received_sequence >= ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE &&
         sequence <= endpoint->highest_received_sequence - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE )
        return;

    // Retention moves first, lazily clearing physical slots that have fallen
    // out. The point write then replaces this exact slot generation and one
    // range query publishes the older-half cohort.
    const uint64_t highest = sequence > endpoint->highest_received_sequence ? sequence : endpoint->highest_received_sequence;
    rocketnet_ack_stats_received_advance( endpoint, highest );

    struct rocketnet_received_packet_record_t * record = &endpoint->received[sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    rocketnet_ack_set_bit( endpoint->received_written, sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    record->sequence = sequence;
    record->receive_time = time;
    record->wire_bytes = (uint32_t) wire_bytes;

    endpoint->highest_received_sequence = highest;
    rocketnet_ack_stats_received_rebuild_block(
        endpoint, (int) ( sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE ) /
                  ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS, highest );
    rocketnet_ack_stats_received_refresh( endpoint );
    rocketnet_ack_stats_publish_received( endpoint );
}

// Bitmap helpers over the sent ring: slot = sequence % N maps to word
// slot/64, bit slot%64. The sent ring is a multiple of 64 (static_assert in
// rocketnet_ack.h), so a window of at most N slots is a contiguous run of
// words, possibly wrapping at the ring boundary.

static inline int rn_clz64( uint64_t v )
{
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse64( &idx, v );
    return 63 - (int) idx;
#else
    return __builtin_clzll( v );
#endif
}

// the portable popcount: a branch-free bit sum that needs no instruction
// set extension, used where the compiler offers no popcount builtin and the
// target is not x64 (MSVC for ARM64 has no __popcnt64)

int rocketnet_ack_popcount64_portable( uint64_t v )
{
    v = v - ( ( v >> 1 ) & 0x5555555555555555ull );
    v = ( v & 0x3333333333333333ull ) + ( ( v >> 2 ) & 0x3333333333333333ull );
    v = ( v + ( v >> 4 ) ) & 0x0f0f0f0f0f0f0f0full;
    return (int) ( ( v * 0x0101010101010101ull ) >> 56 );
}

static inline int rn_popcount64( uint64_t v )
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll( v );
#elif defined(_MSC_VER) && defined(_M_X64)
    return (int) __popcnt64( v );
#else
    return rocketnet_ack_popcount64_portable( v );
#endif
}

// the popcount the ack path uses, for the unit test

int rocketnet_ack_popcount64( uint64_t v )
{
    return rn_popcount64( v );
}

// popcount of one bitmap over the linear slot range [a,b] (a<=b), masking the
// partial end words

static inline int rn_bitmap_range_popcount( const uint64_t * bitmap, uint32_t a, uint32_t b )
{
    uint32_t w_hi = b >> 6;
    uint32_t w_lo = a >> 6;
    int count = 0;
    for ( uint32_t w = w_lo; ; w++ )
    {
        uint64_t word = bitmap[w];
        if ( w == w_lo )
            word &= ~0ull << ( a & 63 );
        if ( w == w_hi )
        {
            uint32_t hi_bit = b & 63;
            word &= hi_bit == 63 ? ~0ull : ( ( 1ull << ( hi_bit + 1 ) ) - 1 );
        }
        count += rn_popcount64( word );
        if ( w == w_hi )
            break;
    }
    return count;
}

// invoke op(sequence) for every set bit of one bitmap in the linear slot range
// [a,b] (a<=b), newest slot first, word by word with clz. sequence at slot a is
// seq_at_a, so sequence = seq_at_a + (slot - a).

template <typename Fn>
static void rn_bitmap_walk_bits_desc( const uint64_t * bitmap, uint32_t a, uint32_t b,
                                       uint64_t seq_at_a, Fn && op )
{
    uint32_t w_hi = b >> 6;
    uint32_t w_lo = a >> 6;
    for ( uint32_t w = w_hi; ; w-- )
    {
        uint64_t word = bitmap[w];
        if ( w == w_lo )
            word &= ~0ull << ( a & 63 );
        if ( w == w_hi )
        {
            uint32_t hi_bit = b & 63;
            word &= hi_bit == 63 ? ~0ull : ( ( 1ull << ( hi_bit + 1 ) ) - 1 );
        }
        while ( word )
        {
            int bit = 63 - rn_clz64( word );
            word &= ~( 1ull << bit );
            uint64_t seq = seq_at_a + ( w * 64 + (uint32_t) bit ) - a;
            op( seq );
        }
        if ( w == w_lo )
            break;
    }
}

// marks one described run of received sequences [low, high], clamped to the
// retained window, and appends newly acknowledged packet ids to the batch
// (tla/FragmentsAndAcks.tla, RecvAck: SurfacedOnce, SurfacedOnlyIfOpened,
// AckedMeansSurfaced, BurnedNeverNamed)
// tla/RNSentHistory.tla ReceiveAck
// tla/RNServerStats.tla Ack: marks each held, unacknowledged record acked and moves it to the acked aggregates
static int rocketnet_ack_apply_run( struct rocketnet_ack_endpoint_t * endpoint,
                                    uint64_t low, uint64_t high, double time,
                                    uint64_t * acked_packet_ids, int acked_capacity, int * acked_count,
                                    uint64_t * unknown_sequence_counter,
                                    uint64_t * batch_full_counter )
{
    // sequences named above everything this end has sent were never sent:
    // counted arithmetically, never visited

    if ( high > endpoint->highest_sequence )
    {
        uint64_t above_low = low > endpoint->highest_sequence + 1 ? low : endpoint->highest_sequence + 1;
        *unknown_sequence_counter += high - above_low + 1;
        if ( low > endpoint->highest_sequence )
            return 0;
        high = endpoint->highest_sequence;
    }

    // everything below the retained window is silently ignored without
    // being visited

    uint64_t window_low = 1;
    if ( endpoint->highest_sequence >= ROCKETNET_SENT_PACKET_BUFFER_SIZE )
        window_low = endpoint->highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;

    if ( high < window_low )
        return 0;
    if ( low < window_low )
        low = window_low;

    int newly_acked = 0;

    // RNSentHistory ReceiveAck marks exactly the named records that are held
    // and unacknowledged; FragmentsAndAcks RecvAck carries the batch deferral
    // (SurfacedOnce, AckedMeansSurfaced, BurnedNeverNamed,
    // OpenedSurfacesOrLaps). The held count over the named slots yields the
    // unknown count arithmetically (the named-but-never-sent sequences); only
    // the unacknowledged slots are visited.

    uint64_t window_len = high - low + 1;
    uint32_t slot_low = (uint32_t)( low % ROCKETNET_SENT_PACKET_BUFFER_SIZE );
    uint32_t slot_high = (uint32_t)( high % ROCKETNET_SENT_PACKET_BUFFER_SIZE );

    int held;
    if ( slot_high >= slot_low )
        held = rn_bitmap_range_popcount( endpoint->sent_held, slot_low, slot_high );
    else
    {
        held = rn_bitmap_range_popcount( endpoint->sent_held, slot_low, ROCKETNET_SENT_PACKET_BUFFER_SIZE - 1 )
             + rn_bitmap_range_popcount( endpoint->sent_held, 0, slot_high );
    }
    *unknown_sequence_counter += window_len - (uint64_t) held;

    auto visit = [&]( uint64_t sequence )
    {
#if defined(ROCKETNET_ACK_VISIT_TEST_HOOKS)
        g_ack_visit_count++;
#endif
        struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];

        // a set bit names a held, unacknowledged record; a mismatch is a
        // reconcile failure, asserted in debug builds and never counted here
        // (the held count already told the unknown count)

        if ( record->sequence != sequence )
        {
            rocketnet_assert( record->sequence == sequence );
            return;
        }

        // a packet id that cannot fit the batch is deferred, never dropped:
        // the record stays unacknowledged, so a later block -- every block
        // restates the recent interval -- surfaces it into a drained batch.
        // the counter records the deferral. a fragment record defers on the
        // same condition, because its acknowledgment may complete its
        // aggregate and surface the payload's packet id.

        if ( ( record->has_packet_id || record->aggregate_id != 0 ) && *acked_count >= acked_capacity )
        {
            ( *batch_full_counter )++;
        }
        else
        {
            record->acked = 1;
            if ( sequence >= rocketnet_ack_stats_window_base( endpoint->highest_sequence, ROCKETNET_SENT_PACKET_BUFFER_SIZE ) &&
                 sequence <= rocketnet_ack_stats_window_top( endpoint->highest_sequence, ROCKETNET_SENT_PACKET_BUFFER_SIZE ) )
            {
                rocketnet_ack_stats_sent_add_acked( endpoint, sequence, record );
                if ( sequence <= rocketnet_ack_stats_eligible_top( endpoint->highest_sequence ) )
                    endpoint->stats_eligible_unacked--;
            }
            newly_acked++;

            uint32_t s = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
            endpoint->sent_unacked[s >> 6] &= ~( 1ull << ( s & 63 ) );

            // the loss rule's evidence: the highest sequence the receiver has
            // acknowledged. update thread only, per the threading comment on
            // the endpoint.

            if ( sequence > endpoint->largest_acked_sequence )
                endpoint->largest_acked_sequence = sequence;

            // the claim _Keep-alive generation_ turns on: this client says it
            // received a keep-alive, so it says it holds its client index.
            // unverifiable here, and by design

            if ( record->was_keep_alive )
                endpoint->keep_alive_acked = 1;

            // the first block that acknowledges a sequence yields one rtt
            // sample: the elapsed time between sending it and receiving the
            // block

            double sample = time - record->send_time;
            if ( sample >= 0.0 )
            {
                rocketnet_ack_stats_rtt_write( endpoint, (int) ( sequence % ROCKETNET_RTT_HISTORY_SIZE ), (float) ( sample * 1000.0 ) );
                endpoint->has_rtt_sample = 1;
            }

            if ( record->has_packet_id )
            {
                acked_packet_ids[(*acked_count)++] = record->packet_id;
            }
            else if ( record->aggregate_id != 0 )
            {
                // one fragment of an aggregated payload: the payload's packet
                // id surfaces only when every fragment's wire packet is
                // acknowledged. a recycled aggregate slot no longer names this
                // id and the ack is simply absorbed.

                struct rocketnet_packet_aggregate_t * aggregate = &endpoint->aggregates[record->aggregate_id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
                if ( rocketnet_ack_bit( endpoint->aggregates_written, record->aggregate_id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE ) &&
                     aggregate->id == record->aggregate_id && aggregate->remaining > 0 )
                {
                    aggregate->remaining--;
                    if ( aggregate->remaining == 0 )
                        acked_packet_ids[(*acked_count)++] = aggregate->packet_id;
                }
            }
        }
    };

    // visit the set bits (the base order: highest sequence first), word by
    // word with clz, splitting the slot range where it wraps at the ring

    if ( slot_high >= slot_low )
    {
        rn_bitmap_walk_bits_desc( endpoint->sent_unacked, slot_low, slot_high, low, visit );
    }
    else
    {
        rn_bitmap_walk_bits_desc( endpoint->sent_unacked, 0, slot_high, high - slot_high, visit );
        rn_bitmap_walk_bits_desc( endpoint->sent_unacked, slot_low, ROCKETNET_SENT_PACKET_BUFFER_SIZE - 1, low, visit );
    }

    return newly_acked;
}

int rocketnet_ack_endpoint_process_block( struct rocketnet_ack_endpoint_t * endpoint,
                                          const struct rocketnet_ack_block_t * block,
                                          double time,
                                          uint64_t * acked_packet_ids, int acked_capacity, int * acked_count,
                                          uint64_t * unknown_sequence_counter,
                                          uint64_t * batch_full_counter )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( block );
    rocketnet_assert( acked_packet_ids );
    rocketnet_assert( acked_count );
    rocketnet_assert( unknown_sequence_counter );
    rocketnet_assert( batch_full_counter );

    if ( block->largest == 0 )
        return 0;

    // Reconcile the shadow bitmaps to the ring (update thread only; update
    // and the fill phase never overlap). Every sequence at or above the window
    // low whose record does not hold that exact sequence -- a burned or
    // refused sequence, or a slot recycled below the window -- clears its
    // slot's bits. One check per sent sequence, amortized across blocks, so
    // the bitmaps stay equal to the ring (BitsMatchRing).

    {
        uint64_t reconcile_low = 1;
        if ( endpoint->highest_sequence >= ROCKETNET_SENT_PACKET_BUFFER_SIZE )
            reconcile_low = endpoint->highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;

        uint64_t x = endpoint->sent_bits_checked + 1;
        if ( x < reconcile_low )
            x = reconcile_low;

        for ( ; x <= endpoint->highest_sequence; x++ )
        {
            struct rocketnet_sent_packet_record_t * record = &endpoint->sent[x % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
            if ( !rocketnet_ack_bit( endpoint->sent_written, x % ROCKETNET_SENT_PACKET_BUFFER_SIZE ) || record->sequence != x )
            {
                uint32_t slot = x % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
                endpoint->sent_held[slot >> 6] &= ~( 1ull << ( slot & 63 ) );
                endpoint->sent_unacked[slot >> 6] &= ~( 1ull << ( slot & 63 ) );
            }
        }
        endpoint->sent_bits_checked = endpoint->highest_sequence;
    }

    int newly_acked = 0;

    // the first range: a received run ending at largest inclusive. validity
    // already proved every subtraction here safe.

    uint64_t high = block->largest;
    uint64_t low = high - block->first_range_length + 1;

    newly_acked += rocketnet_ack_apply_run( endpoint, low, high, time,
                                            acked_packet_ids, acked_capacity, acked_count,
                                            unknown_sequence_counter, batch_full_counter );

    for ( int i = 0; i < block->num_ranges; i++ )
    {
        high = low - 1 - block->range_gap[i];
        low = high - block->range_length[i] + 1;

        newly_acked += rocketnet_ack_apply_run( endpoint, low, high, time,
                                                acked_packet_ids, acked_capacity, acked_count,
                                                unknown_sequence_counter, batch_full_counter );
    }

    if ( newly_acked )
    {
        rocketnet_ack_stats_publish_rtt( endpoint );
        rocketnet_ack_stats_publish_sent( endpoint );
    }

    return newly_acked;
}

// The reference recompute: the endpoint and a standalone history use the same
// calculations, which the running aggregates must equal at every step.
// Typed array views keep their native strides and require no scratch copy.
// sent_written and received_written guard both kinds of ring: an
// unwritten record is empty without reading its cold bytes. A fully initialized
// standalone history may omit the masks.
template <typename SentRecord, typename ReceivedRecord>
static void rocketnet_ack_compute_stats( uint64_t highest_sent, uint64_t highest_received,
                                         const float * rtt_history, const SentRecord * sent,
                                         const ReceivedRecord * received,
                                         const uint64_t * sent_written, const uint64_t * received_written,
                                         rocketnet_ack_stats_result_t * result,
                                         const rocketnet_ack_stats_validity_t * validity = NULL )
{
    // rtt: the minimum over the history of first-ack samples. a sample of
    // exactly zero -- possible only on a held or loopback clock -- is
    // excluded from the minimum, the reference's rule, so the
    // published number stays a real round trip.

    float min_rtt = 0.0f;
    {
        float best = 1e9f;
        for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        {
            if (validity && !validity->rtt.test(i)) continue;
            const float sample = rtt_history[i];
            if ( sample > 0.0f && sample < best )
                best = sample;
        }
        if ( best < 1e9f )
            min_rtt = best;
        result->rtt = min_rtt;
    }

    // jitter: use the same positive samples as the minimum. A zero-time
    // ACK is not a positive RTT and must not contribute negative deviation.

    {
        float sum = 0.0f;
        int count = 0;
        for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        {
            if (validity && !validity->rtt.test(i)) continue;
            const float sample = rtt_history[i];
            if ( sample > 0.0f )
            {
                sum += sample - min_rtt;
                count++;
            }
        }
        result->jitter = count > 0 ? sum / (float) count : 0.0f;
    }

    // packet loss and the sent/acked bandwidth, over the older half of the
    // sent window -- the newer half's acks are still in flight, so counting
    // it would report loss that is only latency

    {
        uint64_t base = 1;
        if ( highest_sent >= ROCKETNET_SENT_PACKET_BUFFER_SIZE )
            base = highest_sent - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;

        int num_sent = 0;
        int num_dropped = 0;
        uint64_t bytes_sent = 0;
        uint64_t bytes_acked = 0;
        double sent_start = 0.0, sent_finish = 0.0;
        double acked_start = 0.0, acked_finish = 0.0;
        int have_sent_time = 0, have_acked_time = 0;

        for ( int i = 0; i < ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2; i++ )
        {
            uint64_t sequence = base + (uint64_t) i;
            if ( sequence > highest_sent )
                break;
            const SentRecord * record = &sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
            if (validity && !validity->sent.test((int)(sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE))) continue;
            if ( sent_written && !rocketnet_ack_bit( sent_written, sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE ) )
                continue;
            if ( record->sequence != sequence )
                continue;

            // A partial ring must exclude the newest half-window too.
            // Sequence holes are not sends; only held records enter loss.
            const bool loss_eligible = highest_sent > ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 &&
                sequence <= highest_sent - ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
            if ( loss_eligible )
                num_sent++;
            bytes_sent += record->wire_bytes;
            if ( !have_sent_time )
            {
                sent_start = record->send_time;
                sent_finish = record->send_time;
                have_sent_time = 1;
            }
            else
            {
                if ( record->send_time < sent_start )
                    sent_start = record->send_time;
                if ( record->send_time > sent_finish )
                    sent_finish = record->send_time;
            }

            if ( record->acked )
            {
                bytes_acked += record->wire_bytes;
                if ( !have_acked_time )
                {
                    acked_start = record->send_time;
                    acked_finish = record->send_time;
                    have_acked_time = 1;
                }
                else
                {
                    if ( record->send_time < acked_start )
                        acked_start = record->send_time;
                    if ( record->send_time > acked_finish )
                        acked_finish = record->send_time;
                }
            }
            else
            {
                if ( loss_eligible )
                    num_dropped++;
            }
        }

        result->packet_loss = num_sent > 0 ? ( (float) num_dropped ) / ( (float) num_sent ) * 100.0f : 0.0f;

        result->sent_kbps = ( have_sent_time && sent_finish > sent_start ) ?
            (float) ( ( (double) bytes_sent ) / ( sent_finish - sent_start ) * 8.0 / 1000.0 ) : 0.0f;

        result->acked_kbps = ( have_acked_time && acked_finish > acked_start ) ?
            (float) ( ( (double) bytes_acked ) / ( acked_finish - acked_start ) * 8.0 / 1000.0 ) : 0.0f;
    }

    // received bandwidth, over the older half of the received window

    {
        uint64_t base = 1;
        if ( highest_received >= ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE )
            base = highest_received - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 1;

        uint64_t bytes_received = 0;
        double start = 0.0, finish = 0.0;
        int have_time = 0;

        for ( int i = 0; i < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2; i++ )
        {
            uint64_t sequence = base + (uint64_t) i;
            if ( sequence > highest_received )
                break;
            const ReceivedRecord * record = &received[sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
            if (validity && !validity->received.test((int)(sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE))) continue;
            if ( received_written && !rocketnet_ack_bit( received_written, sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE ) )
                continue;
            if ( record->sequence != sequence )
                continue;

            bytes_received += record->wire_bytes;
            if ( !have_time )
            {
                start = record->receive_time;
                finish = record->receive_time;
                have_time = 1;
            }
            else
            {
                if ( record->receive_time < start )
                    start = record->receive_time;
                if ( record->receive_time > finish )
                    finish = record->receive_time;
            }
        }

        result->received_kbps = ( have_time && finish > start ) ?
            (float) ( ( (double) bytes_received ) / ( finish - start ) * 8.0 / 1000.0 ) : 0.0f;
    }
}

void rocketnet_ack_stats_history_compute( const rocketnet_ack_stats_history_t * history,
                                        rocketnet_ack_stats_result_t * result,
                                        const uint64_t * sent_written, const uint64_t * received_written,
                                        const rocketnet_ack_stats_validity_t * validity )
{
    rocketnet_assert(history && result);
    rocketnet_ack_compute_stats(history->highest_sent, history->highest_received,
                               history->rtt_history, history->sent, history->received, sent_written, received_written, result, validity);
}

void rocketnet_ack_endpoint_update_stats( struct rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_assert(endpoint);
    rocketnet_ack_stats_result_t result;
    rocketnet_ack_compute_stats(endpoint->highest_sequence, endpoint->highest_received_sequence,
                               endpoint->rtt_history, endpoint->sent, endpoint->received,
                               endpoint->sent_written, endpoint->received_written, &result);
    endpoint->rtt = result.rtt;
    endpoint->jitter = result.jitter;
    endpoint->packet_loss = result.packet_loss;
    endpoint->bandwidth_sent_kbps = result.sent_kbps;
    endpoint->bandwidth_received_kbps = result.received_kbps;
    endpoint->bandwidth_acked_kbps = result.acked_kbps;
}

// -----------------------------------------
//                    the server's acks
// -----------------------------------------

uint64_t rocketnet_server_acks_allocation_bytes(int clients)
{
    if (clients < 1 || clients > INT_MAX / ROCKETNET_SENT_PACKET_BUFFER_SIZE) return 0;
    const uint64_t n = (uint64_t)clients;
    return rocketnet_memory_allocations({sizeof(rocketnet_server_acks_t),
        n*ROCKETNET_SENT_PACKET_BUFFER_SIZE*sizeof(int), n*ROCKETNET_SENT_PACKET_BUFFER_SIZE*sizeof(uint64_t),
        n*sizeof(rocketnet_ack_endpoint_t)});
}

struct rocketnet_server_acks_t * rocketnet_server_acks_create( void * context, int max_clients )
{
    rocketnet_assert( max_clients > 0 );

    if ( max_clients < 1 || max_clients > INT_MAX / ROCKETNET_SENT_PACKET_BUFFER_SIZE )
        return NULL;

    struct rocketnet_server_acks_t * acks = (struct rocketnet_server_acks_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_acks_t) );
    if ( !acks )
        return NULL;

    memset( acks, 0, sizeof(struct rocketnet_server_acks_t) );

    acks->context = context;
    acks->max_clients = max_clients;

    // One delayed ACK may acknowledge every retained record. Capacity follows
    // retained state, not an assumed number of sends per update.
    acks->acked_capacity = ROCKETNET_SENT_PACKET_BUFFER_SIZE * max_clients;

    acks->acked_client_index = (int*) rocketnet_malloc( context, (size_t) acks->acked_capacity * sizeof(int) );
    acks->acked_packet_ids = (uint64_t*) rocketnet_malloc( context, (size_t) acks->acked_capacity * sizeof(uint64_t) );
    acks->endpoints = (struct rocketnet_ack_endpoint_t*) rocketnet_malloc( context, (size_t) max_clients * sizeof(struct rocketnet_ack_endpoint_t) );

    if ( !acks->acked_client_index || !acks->acked_packet_ids || !acks->endpoints )
    {
        rocketnet_server_acks_destroy( acks );
        return NULL;
    }

    for ( int i = 0; i < max_clients; i++ )
        rocketnet_ack_endpoint_reset( &acks->endpoints[i] );

    return acks;
}

void rocketnet_server_acks_destroy( struct rocketnet_server_acks_t * acks )
{
    rocketnet_assert( acks );
    if ( acks->acked_client_index )
        rocketnet_free( acks->context, acks->acked_client_index );
    if ( acks->acked_packet_ids )
        rocketnet_free( acks->context, acks->acked_packet_ids );
    if ( acks->endpoints )
        rocketnet_free( acks->context, acks->endpoints );
    rocketnet_free( acks->context, acks );
}

void rocketnet_server_acks_reset_slot( struct rocketnet_server_acks_t * acks, int client_index )
{
    rocketnet_assert( acks );
    rocketnet_assert( client_index >= 0 && client_index < acks->max_clients );
    rocketnet_ack_endpoint_reset( &acks->endpoints[client_index] );
}

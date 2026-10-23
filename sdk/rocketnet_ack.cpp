/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Visit counter for testing: increments on each sequence visit during process_block
unsigned long long g_ack_visit_count = 0;

#include "rocketnet_memory.h"
#include "rocketnet_ack.h"
#include "rocketnet_read_write.h"
#include "rocketnet_replay.h"
#include "rocketnet_platform.h"
#include "rocketnet.h"

#include <string.h>
#include <math.h>
#include <limits.h>

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

    // validity is evaluated on the reconstructed 64-bit largest, including
    // the zero test: a genuine sequence whose low 32 bits happen to be zero
    // does not read as a fresh connection

    uint64_t largest = rocketnet_reconstruct_ack_largest( wire_largest, highest_sent );

    if ( largest == 0 )
    {
        if ( first_range_length != 0 || num_ranges != 0 )
            return 0;
        block->largest = 0;
        block->first_range_length = 0;
        block->num_ranges = 0;
        return block_bytes;
    }

    if ( first_range_length == 0 )
        return 0;

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

void rocketnet_ack_endpoint_reset( struct rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_assert( endpoint );
    memset( (void*) endpoint, 0, sizeof(struct rocketnet_ack_endpoint_t) );
    for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        endpoint->rtt_history[i] = -1.0f;
    endpoint->sent_base = 1;
    endpoint->recv_base = 1;
}

// -----------------------------------------
//            incremental running aggregates
// -----------------------------------------

#define ROCKETNET_SENT_STATS_HALF ( ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 )
#define ROCKETNET_RECV_STATS_HALF ( ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2 )

static inline int rocketnet_ack_sent_present( const struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence )
{
    return endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].sequence == sequence;
}

static inline int rocketnet_ack_recv_present( const struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence )
{
    return endpoint->received[sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE].sequence == sequence;
}

static void rocketnet_ack_stats_sent_insert( struct rocketnet_ack_endpoint_t * endpoint,
                                             const struct rocketnet_sent_packet_record_t * record )
{
    const uint64_t seq = record->sequence;
    const uint64_t top = endpoint->sent_base + ROCKETNET_SENT_STATS_HALF - 1;
    if ( seq < endpoint->sent_base || seq > top || seq > endpoint->highest_sequence )
        return;

    const int was_first = ( endpoint->s_cnt == 0 );
    endpoint->s_cnt++;
    endpoint->s_bytes += record->wire_bytes;

    if ( !record->acked )
    {
        endpoint->s_unacked++;
    }
    else
    {
        endpoint->s_acked_bytes += record->wire_bytes;
        if ( !endpoint->have_a_time )
        {
            endpoint->a_lowseq = seq;
            endpoint->a_highseq = seq;
            endpoint->have_a_time = 1;
        }
        else
        {
            if ( seq < endpoint->a_lowseq )
                endpoint->a_lowseq = seq;
            if ( seq > endpoint->a_highseq )
                endpoint->a_highseq = seq;
        }
    }

    if ( was_first )
    {
        endpoint->s_lowseq = seq;
        endpoint->s_highseq = seq;
        endpoint->have_s_time = 1;
    }
    else
    {
        if ( seq < endpoint->s_lowseq )
            endpoint->s_lowseq = seq;
        if ( seq > endpoint->s_highseq )
            endpoint->s_highseq = seq;
    }
}

static void rocketnet_ack_stats_sent_remove( struct rocketnet_ack_endpoint_t * endpoint,
                                             const struct rocketnet_sent_packet_record_t * record )
{
    const uint64_t seq = record->sequence;
    const uint64_t top = endpoint->sent_base + ROCKETNET_SENT_STATS_HALF - 1;
    if ( seq < endpoint->sent_base || seq > top || seq > endpoint->highest_sequence )
        return;

    endpoint->s_cnt--;
    endpoint->s_bytes -= record->wire_bytes;
    if ( !record->acked )
        endpoint->s_unacked--;
    else
        endpoint->s_acked_bytes -= record->wire_bytes;

    if ( endpoint->s_cnt == 0 )
    {
        endpoint->have_s_time = 0;
        endpoint->have_a_time = 0;
        endpoint->s_lowseq = 0;
        endpoint->s_highseq = 0;
        endpoint->a_lowseq = 0;
        endpoint->a_highseq = 0;
        return;
    }

    if ( seq == endpoint->s_lowseq || endpoint->s_lowseq < endpoint->sent_base )
    {
        endpoint->s_lowseq = endpoint->sent_base;
        while ( endpoint->s_lowseq <= top && !rocketnet_ack_sent_present( endpoint, endpoint->s_lowseq ) )
            endpoint->s_lowseq++;
    }
    if ( seq == endpoint->s_highseq )
    {
        while ( endpoint->s_highseq >= endpoint->sent_base && !rocketnet_ack_sent_present( endpoint, endpoint->s_highseq ) )
            endpoint->s_highseq--;
    }

    if ( !record->acked )
        return;

    if ( endpoint->have_a_time && seq == endpoint->a_lowseq )
    {
        uint64_t s = endpoint->sent_base;
        int found = 0;
        while ( s <= top )
        {
            const uint64_t idx = s % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
            if ( endpoint->sent[idx].sequence == s && endpoint->sent[idx].acked )
            {
                endpoint->a_lowseq = s;
                found = 1;
                break;
            }
            s++;
        }
        if ( !found )
            endpoint->have_a_time = 0;
    }
    if ( endpoint->have_a_time && seq == endpoint->a_highseq )
    {
        uint64_t s = endpoint->a_highseq;
        int found = 0;
        while ( s >= endpoint->sent_base )
        {
            const uint64_t idx = s % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
            if ( endpoint->sent[idx].sequence == s && endpoint->sent[idx].acked )
            {
                endpoint->a_highseq = s;
                found = 1;
                break;
            }
            if ( s == endpoint->sent_base )
                break;
            s--;
        }
        if ( !found )
            endpoint->have_a_time = 0;
    }
}

static void rocketnet_ack_stats_sent_ack( struct rocketnet_ack_endpoint_t * endpoint, uint64_t seq, uint32_t wire_bytes )
{
    const uint64_t top = endpoint->sent_base + ROCKETNET_SENT_STATS_HALF - 1;
    if ( seq < endpoint->sent_base || seq > top )
        return;
    endpoint->s_unacked--;
    endpoint->s_acked_bytes += wire_bytes;
    if ( !endpoint->have_a_time )
    {
        endpoint->a_lowseq = seq;
        endpoint->a_highseq = seq;
        endpoint->have_a_time = 1;
    }
    else
    {
        if ( seq < endpoint->a_lowseq )
            endpoint->a_lowseq = seq;
        if ( seq > endpoint->a_highseq )
            endpoint->a_highseq = seq;
    }
}

// settles the sent-window's low edge after a send and folds the new record in

static void rocketnet_ack_stats_sent_sync( struct rocketnet_ack_endpoint_t * endpoint, uint64_t just_written )
{
    const uint64_t W = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    const uint64_t HALF = ROCKETNET_SENT_STATS_HALF;
    const uint64_t target = ( endpoint->highest_sequence >= W ) ? endpoint->highest_sequence - W + 1 : 1;

    while ( endpoint->sent_base < target )
    {
        const uint64_t low = endpoint->sent_base;
        const uint64_t entering = endpoint->sent_base + HALF;
        if ( low <= endpoint->highest_sequence )
        {
            const struct rocketnet_sent_packet_record_t * rec = &endpoint->sent[low % W];
            if ( rec->sequence == low )
                rocketnet_ack_stats_sent_remove( endpoint, rec );
        }
        if ( entering != just_written && entering <= endpoint->highest_sequence )
        {
            const struct rocketnet_sent_packet_record_t * rec = &endpoint->sent[entering % W];
            if ( rec->sequence == entering )
                rocketnet_ack_stats_sent_insert( endpoint, rec );
        }
        endpoint->sent_base++;
    }

    const struct rocketnet_sent_packet_record_t * rec = &endpoint->sent[just_written % W];
    rocketnet_ack_stats_sent_insert( endpoint, rec );
}

static void rocketnet_ack_stats_recv_insert( struct rocketnet_ack_endpoint_t * endpoint,
                                             const struct rocketnet_received_packet_record_t * record )
{
    const uint64_t seq = record->sequence;
    const uint64_t top = endpoint->recv_base + ROCKETNET_RECV_STATS_HALF - 1;
    if ( seq < endpoint->recv_base || seq > top || seq > endpoint->highest_received_sequence )
        return;

    const int was_first = ( endpoint->r_cnt == 0 );
    endpoint->r_cnt++;
    endpoint->r_bytes += record->wire_bytes;
    if ( was_first )
    {
        endpoint->r_lowseq = seq;
        endpoint->r_highseq = seq;
        endpoint->have_r_time = 1;
    }
    else
    {
        if ( seq < endpoint->r_lowseq )
            endpoint->r_lowseq = seq;
        if ( seq > endpoint->r_highseq )
            endpoint->r_highseq = seq;
    }
}

static void rocketnet_ack_stats_recv_remove( struct rocketnet_ack_endpoint_t * endpoint,
                                             const struct rocketnet_received_packet_record_t * record )
{
    const uint64_t seq = record->sequence;
    const uint64_t top = endpoint->recv_base + ROCKETNET_RECV_STATS_HALF - 1;
    if ( seq < endpoint->recv_base || seq > top || seq > endpoint->highest_received_sequence )
        return;

    endpoint->r_cnt--;
    endpoint->r_bytes -= record->wire_bytes;
    if ( endpoint->r_cnt == 0 )
    {
        endpoint->have_r_time = 0;
        endpoint->r_lowseq = 0;
        endpoint->r_highseq = 0;
        return;
    }
    if ( seq == endpoint->r_lowseq || endpoint->r_lowseq < endpoint->recv_base )
    {
        endpoint->r_lowseq = endpoint->recv_base;
        while ( endpoint->r_lowseq <= top && !rocketnet_ack_recv_present( endpoint, endpoint->r_lowseq ) )
            endpoint->r_lowseq++;
    }
    if ( seq == endpoint->r_highseq )
    {
        while ( endpoint->r_highseq >= endpoint->recv_base && !rocketnet_ack_recv_present( endpoint, endpoint->r_highseq ) )
            endpoint->r_highseq--;
    }
}

// settles the received-window's low edge after a receive and folds the new
// record in

static void rocketnet_ack_stats_recv_after( struct rocketnet_ack_endpoint_t * endpoint, uint64_t just_written )
{
    const uint64_t W = ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE;
    const uint64_t HALF = ROCKETNET_RECV_STATS_HALF;
    const uint64_t target = ( endpoint->highest_received_sequence >= W ) ? endpoint->highest_received_sequence - W + 1 : 1;

    while ( endpoint->recv_base < target )
    {
        const uint64_t low = endpoint->recv_base;
        const uint64_t entering = endpoint->recv_base + HALF;
        if ( low <= endpoint->highest_received_sequence )
        {
            const struct rocketnet_received_packet_record_t * rec = &endpoint->received[low % W];
            if ( rec->sequence == low )
                rocketnet_ack_stats_recv_remove( endpoint, rec );
        }
        if ( entering != just_written && entering <= endpoint->highest_received_sequence )
        {
            const struct rocketnet_received_packet_record_t * rec = &endpoint->received[entering % W];
            if ( rec->sequence == entering )
                rocketnet_ack_stats_recv_insert( endpoint, rec );
        }
        endpoint->recv_base++;
    }

    const struct rocketnet_received_packet_record_t * rec = &endpoint->received[just_written % W];
    rocketnet_ack_stats_recv_insert( endpoint, rec );
}

// publishes the running aggregates to the fields the transports read

static void rocketnet_ack_stats_publish( struct rocketnet_ack_endpoint_t * endpoint )
{
    // Reconcile the rtt aggregates with the history once per slot lifetime:
    // samples written straight into the history (the test fixtures that
    // inject rtt_history directly, and any future non-process_block writer)
    // are reflected in the published numbers exactly as the reference
    // recompute would. After this first pass the aggregates are authoritative
    // and process_block keeps them current in O(1) per sample.
    if ( !endpoint->rtt_reconciled )
    {
        float best = 1e9f;
        double sum = 0.0;
        int count = 0;
        for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        {
            const float v = endpoint->rtt_history[i];
            if ( v > 0.0f && v < best )
                best = v;
            if ( v >= 0.0f )
            {
                sum += v;
                count++;
            }
        }
        endpoint->rtt_min_pos = ( best < 1e9f ) ? best : 0.0f;
        endpoint->rtt_raw_sum = sum;
        endpoint->rtt_count = count;
        endpoint->rtt_reconciled = 1;
    }

    endpoint->rtt = endpoint->rtt_min_pos;
    if ( endpoint->rtt_count > 0 )
        endpoint->jitter = (float) ( ( endpoint->rtt_raw_sum - (double) endpoint->rtt_count * (double) endpoint->rtt_min_pos ) / (double) endpoint->rtt_count );
    else
        endpoint->jitter = 0.0f;

    const struct rocketnet_sent_packet_record_t * sent = endpoint->sent;
    if ( endpoint->s_cnt > 0 )
    {
        endpoint->packet_loss = ( (float) endpoint->s_unacked ) / ( (float) endpoint->s_cnt ) * 100.0f;
        const struct rocketnet_sent_packet_record_t * lo = &sent[endpoint->s_lowseq % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
        const struct rocketnet_sent_packet_record_t * hi = &sent[endpoint->s_highseq % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
        const double span = hi->send_time - lo->send_time;
        endpoint->bandwidth_sent_kbps = ( span > 0.0 ) ?
            (float) ( ( (double) endpoint->s_bytes ) / span * 8.0 / 1000.0 ) : 0.0f;
        if ( endpoint->have_a_time )
        {
            const struct rocketnet_sent_packet_record_t * alo = &sent[endpoint->a_lowseq % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
            const struct rocketnet_sent_packet_record_t * ahi = &sent[endpoint->a_highseq % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
            const double aspan = ahi->send_time - alo->send_time;
            endpoint->bandwidth_acked_kbps = ( aspan > 0.0 ) ?
                (float) ( ( (double) endpoint->s_acked_bytes ) / aspan * 8.0 / 1000.0 ) : 0.0f;
        }
        else
            endpoint->bandwidth_acked_kbps = 0.0f;
    }
    else
    {
        endpoint->packet_loss = 0.0f;
        endpoint->bandwidth_sent_kbps = 0.0f;
        endpoint->bandwidth_acked_kbps = 0.0f;
    }

    if ( endpoint->r_cnt > 0 && endpoint->have_r_time )
    {
        const struct rocketnet_received_packet_record_t * lo = &endpoint->received[endpoint->r_lowseq % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
        const struct rocketnet_received_packet_record_t * hi = &endpoint->received[endpoint->r_highseq % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
        const double span = hi->receive_time - lo->receive_time;
        endpoint->bandwidth_received_kbps = ( span > 0.0 ) ?
            (float) ( ( (double) endpoint->r_bytes ) / span * 8.0 / 1000.0 ) : 0.0f;
    }
    else
        endpoint->bandwidth_received_kbps = 0.0f;
}

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
    if ( record->sequence >= sequence ||
         ( endpoint->highest_sequence >= ROCKETNET_SENT_PACKET_BUFFER_SIZE &&
           sequence <= endpoint->highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE ) )
    {
        rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );
        return ROCKETNET_ACK_SENT_REFUSED_STALE;
    }

    // the record this write replaces: empty or acknowledged is a clean
    // write; unacknowledged is an eviction, and the peer having
    // acknowledged a sequence ROCKETNET_LOSS_PACKET_THRESHOLD above it is
    // what makes the eviction a known path loss (RFC 9002 6.1.1) rather than
    // an unknown fate. largest_acked_sequence is written by the update
    // thread alone, which never overlaps the fill phase.

    enum rocketnet_ack_sent_result_t result = ROCKETNET_ACK_SENT_RECORDED;
    if ( record->sequence && !record->acked )
    {
        result = record->sequence + ROCKETNET_LOSS_PACKET_THRESHOLD <= endpoint->largest_acked_sequence
                     ? ROCKETNET_ACK_SENT_EVICTED_LOST
                     : ROCKETNET_ACK_SENT_EVICTED_UNKNOWN;
    }
    if ( record->sequence != 0 && record->sequence >= endpoint->sent_base &&
         record->sequence <= endpoint->sent_base + ROCKETNET_SENT_STATS_HALF - 1 )
        rocketnet_ack_stats_sent_remove( endpoint, record );
    record->sequence = sequence;
    record->packet_id = packet_id;
    record->aggregate_id = aggregate_id;
    record->send_time = time;
    record->wire_bytes = (uint32_t) wire_bytes;
    record->acked = 0;
    record->has_packet_id = (uint8_t) ( has_packet_id != 0 );
    record->was_keep_alive = 0;

    // fill-phase threads record concurrently under distinct sequences, so
    // the high-water fold is a compare-exchange max: whatever the
    // interleaving, the mark ends at the largest recorded sequence

    rocketnet_platform_atomic_max_u64( &endpoint->highest_sequence, sequence );
    rocketnet_ack_stats_sent_sync( endpoint, sequence );
    rocketnet_ack_stats_publish( endpoint );
    rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );
    return result;
}

bool rocketnet_ack_endpoint_record_sent( struct rocketnet_ack_endpoint_t * endpoint,
                                         uint64_t sequence, int wire_bytes,
                                         uint64_t packet_id, int has_packet_id,
                                         uint64_t aggregate_id, double time )
{
    return rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, wire_bytes, packet_id, has_packet_id,
                                                          aggregate_id, time ) == ROCKETNET_ACK_SENT_RECORDED;
}

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
    if ( record->sequence == sequence )
        record->was_keep_alive = 1;
    rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );
}

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
    aggregate->id = id;
    aggregate->packet_id = packet_id;
    aggregate->remaining = (uint32_t) fragment_count;
    rocketnet_platform_spin_unlock_u64( &endpoint->sent_lock );

    return id;
}

void rocketnet_ack_endpoint_record_received( struct rocketnet_ack_endpoint_t * endpoint,
                                             uint64_t sequence, int wire_bytes, double time )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( sequence != 0 );

    struct rocketnet_received_packet_record_t * record = &endpoint->received[sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    if ( record->sequence != 0 && record->sequence >= endpoint->recv_base &&
         record->sequence <= endpoint->recv_base + ROCKETNET_RECV_STATS_HALF - 1 )
        rocketnet_ack_stats_recv_remove( endpoint, record );
    record->sequence = sequence;
    record->receive_time = time;
    record->wire_bytes = (uint32_t) wire_bytes;

    if ( sequence > endpoint->highest_received_sequence )
        endpoint->highest_received_sequence = sequence;
    rocketnet_ack_stats_recv_after( endpoint, sequence );
    rocketnet_ack_stats_publish( endpoint );
}

// marks one described run of received sequences [low, high], clamped to the
// retained window, and appends newly acknowledged packet ids to the batch

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

    for ( uint64_t sequence = high; ; sequence-- )
    {
        g_ack_visit_count++;
        struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];

        if ( record->sequence != sequence )
        {
            // named but never sent: a burned sequence, or reconstruction
            // against a hostile or badly-truncated block. never an error
            // that affects the connection.
            ( *unknown_sequence_counter )++;
        }
        else if ( !record->acked )
        {
            // a packet id that cannot fit the batch is deferred, never
            // dropped: the record stays unacknowledged, so a later block --
            // every block restates the recent interval -- surfaces it into
            // a drained batch. the counter records the deferral. a
            // fragment record defers on the same condition, because its
            // acknowledgment may complete its aggregate and surface the
            // payload's packet id.

            if ( ( record->has_packet_id || record->aggregate_id != 0 ) && *acked_count >= acked_capacity )
            {
                ( *batch_full_counter )++;
            }
            else
            {
                record->acked = 1;
                newly_acked++;
                rocketnet_ack_stats_sent_ack( endpoint, sequence, record->wire_bytes );

                // the loss rule's evidence: the highest sequence the peer
                // has acknowledged. update thread only, per the threading
                // comment on the endpoint.

                if ( sequence > endpoint->largest_acked_sequence )
                    endpoint->largest_acked_sequence = sequence;

                // the claim _Keep-alive generation_ turns on: this peer
                // says it received a keep-alive, so it says it holds its
                // client index. unverifiable here, and by design

                if ( record->was_keep_alive )
                    endpoint->keep_alive_acked = 1;

                // the first block that acknowledges a sequence yields one
                // rtt sample: the elapsed time between sending it and
                // receiving the block

                double sample = time - record->send_time;
                if ( sample >= 0.0 )
                {
                    const float sample_ms = (float) ( sample * 1000.0 );
                    const int hidx = (int) ( sequence % ROCKETNET_RTT_HISTORY_SIZE );
                    const float old_sample = endpoint->rtt_history[hidx];
                    if ( old_sample >= 0.0f )
                    {
                        endpoint->rtt_raw_sum -= old_sample;
                        endpoint->rtt_count--;
                    }
                    endpoint->rtt_history[hidx] = sample_ms;
                    if ( sample_ms >= 0.0f )
                    {
                        endpoint->rtt_raw_sum += sample_ms;
                        endpoint->rtt_count++;
                    }
                    if ( old_sample > 0.0f && old_sample == endpoint->rtt_min_pos )
                    {
                        float best = 1e9f;
                        for ( int k = 0; k < ROCKETNET_RTT_HISTORY_SIZE; k++ )
                        {
                            const float v = endpoint->rtt_history[k];
                            if ( v > 0.0f && v < best )
                                best = v;
                        }
                        endpoint->rtt_min_pos = ( best < 1e9f ) ? best : 0.0f;
                    }
                    if ( sample_ms > 0.0f &&
                         ( endpoint->rtt_min_pos == 0.0f || sample_ms < endpoint->rtt_min_pos ) )
                        endpoint->rtt_min_pos = sample_ms;
                    endpoint->has_rtt_sample = 1;
                }

                if ( record->has_packet_id )
                {
                    acked_packet_ids[(*acked_count)++] = record->packet_id;
                }
                else if ( record->aggregate_id != 0 )
                {
                    // one fragment of an aggregated payload: the payload's
                    // packet id surfaces only when every fragment's wire
                    // packet is acknowledged. a recycled aggregate slot no
                    // longer names this id and the ack is simply absorbed.

                    struct rocketnet_packet_aggregate_t * aggregate = &endpoint->aggregates[record->aggregate_id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
                    if ( aggregate->id == record->aggregate_id && aggregate->remaining > 0 )
                    {
                        aggregate->remaining--;
                        if ( aggregate->remaining == 0 )
                            acked_packet_ids[(*acked_count)++] = aggregate->packet_id;
                    }
                }
            }
        }

        if ( sequence == low )
            break;
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

    rocketnet_ack_stats_publish( endpoint );

    return newly_acked;
}

void rocketnet_ack_endpoint_update_stats( struct rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_assert( endpoint );

    // rtt: the minimum over the history of first-ack samples. a sample of
    // exactly zero -- possible only on a held or loopback clock -- is
    // excluded from the minimum, the reference's rule, so the
    // published number stays a real round trip.

    float min_rtt = 0.0f;
    {
        float best = 1e9f;
        for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        {
            const float sample = endpoint->rtt_history[i];
            if ( sample > 0.0f && sample < best )
                best = sample;
        }
        if ( best < 1e9f )
            min_rtt = best;
        endpoint->rtt = min_rtt;
    }

    // jitter: the mean deviation of the samples above the minimum

    {
        float sum = 0.0f;
        int count = 0;
        for ( int i = 0; i < ROCKETNET_RTT_HISTORY_SIZE; i++ )
        {
            const float sample = endpoint->rtt_history[i];
            if ( sample >= 0.0f )
            {
                sum += sample - min_rtt;
                count++;
            }
        }
        endpoint->jitter = count > 0 ? sum / (float) count : 0.0f;
    }

    // packet loss and the sent/acked bandwidth, over the older half of the
    // sent window -- the newer half's acks are still in flight, so counting
    // it would report loss that is only latency

    {
        uint64_t base = 1;
        if ( endpoint->highest_sequence >= ROCKETNET_SENT_PACKET_BUFFER_SIZE )
            base = endpoint->highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;

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
            if ( sequence > endpoint->highest_sequence )
                break;
            const struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
            if ( record->sequence != sequence )
                continue;

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
                num_dropped++;
            }
        }

        endpoint->packet_loss = num_sent > 0 ? ( (float) num_dropped ) / ( (float) num_sent ) * 100.0f : 0.0f;

        endpoint->bandwidth_sent_kbps = ( have_sent_time && sent_finish > sent_start ) ?
            (float) ( ( (double) bytes_sent ) / ( sent_finish - sent_start ) * 8.0 / 1000.0 ) : 0.0f;

        endpoint->bandwidth_acked_kbps = ( have_acked_time && acked_finish > acked_start ) ?
            (float) ( ( (double) bytes_acked ) / ( acked_finish - acked_start ) * 8.0 / 1000.0 ) : 0.0f;
    }

    // received bandwidth, over the older half of the received window

    {
        uint64_t base = 1;
        if ( endpoint->highest_received_sequence >= ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE )
            base = endpoint->highest_received_sequence - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 1;

        uint64_t bytes_received = 0;
        double start = 0.0, finish = 0.0;
        int have_time = 0;

        for ( int i = 0; i < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2; i++ )
        {
            uint64_t sequence = base + (uint64_t) i;
            if ( sequence > endpoint->highest_received_sequence )
                break;
            const struct rocketnet_received_packet_record_t * record = &endpoint->received[sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
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

        endpoint->bandwidth_received_kbps = ( have_time && finish > start ) ?
            (float) ( ( (double) bytes_received ) / ( finish - start ) * 8.0 / 1000.0 ) : 0.0f;
    }
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

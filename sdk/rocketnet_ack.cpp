/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_ack.h"
#include "rocketnet_read_write.h"
#include "rocketnet_replay.h"
#include "rocketnet_platform.h"
#include "rocketnet.h"

#include <string.h>

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
}

void rocketnet_ack_endpoint_record_sent( struct rocketnet_ack_endpoint_t * endpoint,
                                         uint64_t sequence, int wire_bytes,
                                         uint64_t packet_id, int has_packet_id,
                                         uint64_t aggregate_id, double time )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( sequence != 0 );
    rocketnet_assert( !( has_packet_id && aggregate_id != 0 ) );        // a fragment's packet id lives in its aggregate

    struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
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
}

void rocketnet_ack_endpoint_mark_keep_alive( struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( sequence != 0 );

    struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];

    // the record must still be this sequence's: a recycled entry belongs to
    // a later packet and must not be tagged. missing the tag only delays the
    // latch to the next keep-alive, which is the safe direction.
    //
    // RESIDUAL, stated rather than glossed: this read-then-write is not
    // atomic, so a concurrent record_sent recycling this entry between the
    // compare and the store could still tag the wrong packet -- a window
    // 4096 sequences wide, inside the pre-existing non-atomic record_sent
    // design rather than added by it. Unreachable under the current phase
    // contract, which is what makes it acceptable: update and the fill phase
    // never overlap, and keep-alives are written only by update. It would
    // become reachable the day a keep-alive is written from a fill thread,
    // which is also the day this function needs to be made atomic.

    if ( record->sequence == sequence )
        record->was_keep_alive = 1;
}

uint64_t rocketnet_ack_endpoint_open_aggregate( struct rocketnet_ack_endpoint_t * endpoint,
                                                uint64_t packet_id, int fragment_count )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( fragment_count >= 2 );

    // the id is claimed with one atomic add, so fill threads open
    // aggregates concurrently; ids ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE
    // apart cannot coexist in one fill phase, so the slot write is sole

    uint64_t id = rocketnet_platform_atomic_add_u64( &endpoint->num_aggregates, 1 ) + 1;

    struct rocketnet_packet_aggregate_t * aggregate = &endpoint->aggregates[id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
    aggregate->id = id;
    aggregate->packet_id = packet_id;
    aggregate->remaining = (uint32_t) fragment_count;

    return id;
}

void rocketnet_ack_endpoint_record_received( struct rocketnet_ack_endpoint_t * endpoint,
                                             uint64_t sequence, int wire_bytes, double time )
{
    rocketnet_assert( endpoint );
    rocketnet_assert( sequence != 0 );

    struct rocketnet_received_packet_record_t * record = &endpoint->received[sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
    record->sequence = sequence;
    record->receive_time = time;
    record->wire_bytes = (uint32_t) wire_bytes;

    if ( sequence > endpoint->highest_received_sequence )
        endpoint->highest_received_sequence = sequence;
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
                    endpoint->rtt_history[sequence % ROCKETNET_RTT_HISTORY_SIZE] = (float) ( sample * 1000.0 );
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

    return newly_acked;
}

void rocketnet_ack_endpoint_update_stats( struct rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_assert( endpoint );

    // rtt: the minimum over the history of first-ack samples. a sample of
    // exactly zero -- possible only on a held or loopback clock -- is
    // excluded from the minimum, the space reference's rule, so the
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

struct rocketnet_server_acks_t * rocketnet_server_acks_create( void * context, int max_clients )
{
    rocketnet_assert( max_clients > 0 );

    struct rocketnet_server_acks_t * acks = (struct rocketnet_server_acks_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_acks_t) );
    if ( !acks )
        return NULL;

    memset( acks, 0, sizeof(struct rocketnet_server_acks_t) );

    acks->context = context;
    acks->max_clients = max_clients;

    // the batch caps at the per-frame send budget times the slot count: in
    // steady state newly-acknowledged packets can never outrun sends

    acks->acked_capacity = ROCKETNET_SEND_PACKETS_PER_CLIENT * max_clients;

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

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    resend_timing_table_over_lossy_links: the measured latency and cost of the
    reliable resend rule (docs/CHANNELS.md 6.4; tla/RNResendTiming.tla) over
    the channel harness. For each RTT (20, 50, 100 and 200 ms; harness
    latency RTT/2 each way, jitter a tenth of that) and loss (10% and 20%,
    both directions), the client sends 400 reliable 64-byte messages, one per
    60 Hz update, while the server sends one 32-byte unreliable message per
    update, as a game's state stream does, so acks ride packets going the
    other way. Each row prints p50, p99 and p99.9 delivery latency in ms
    (send to delivery at the server), the copies sent per message, and the
    messages that never arrived (timeouts).

    Two schedules run on the same seed: "rule" is the shipped default
    (min( rto, resend_interval ), no back-off) and "fixed" is a resend every
    100 ms, set on the client's estimator (rto_min = rto_max = rto = 100 ms).
    The case prints the whole table beside the Monte Carlo model's p99 for
    the rule (one message's timeline, ack after RTT plus up to a tick), then
    asserts: zero timeouts on every row; the rule's p99 no later than the
    fixed 100 ms schedule's plus one tick; and the rule's p99 within the
    one-way latency, the harness floor of two ticks (a send waits for its
    update, a delivery is seen at the receiver's next one) and three resends
    at the cap of resend_interval plus a tick each. At 20% loss three sends
    reach 99.2% of messages, so the third resend is slack for the sample.

    The model's p99 is not asserted directly: the harness updates both ends
    at 60 Hz, so each measured RTT sample is the path's RTT plus up to two
    ticks and the ack's own update, and on a 20 ms path the RTO follows that
    longer measured RTT. The table shows the difference row by row.
*/

#include "channel_harness.h"

#include <math.h>

#include <algorithm>

static void resend_timing_fail( const char * condition, int line )
{
    printf( "resend timing check failed: %s at line %d\n", condition, line );
    fflush( stdout );
    exit( 1 );
}

#define resend_timing_check( condition ) do { if ( !( condition ) ) resend_timing_fail( #condition, __LINE__ ); } while ( 0 )

#define RESEND_TIMING_MESSAGES 400
#define RESEND_TIMING_BYTES 64

struct resend_timing_row_t
{
    const char * schedule;
    int rtt_ms;
    int loss_percent;
    int delivered;
    int timeouts;
    double p50_ms;
    double p99_ms;
    double p999_ms;
    double copies;
};

static double resend_timing_percentile( const double * sorted, int count, double fraction )
{
    if ( count <= 0 )
        return 0.0;
    int index = (int) ceil( fraction * (double) count ) - 1;
    if ( index < 0 )
        index = 0;
    if ( index >= count )
        index = count - 1;
    return sorted[index];
}

static void resend_timing_run( const char * schedule, int fixed_100ms, int rtt_ms, int loss_percent, resend_timing_row_t * row )
{
    const double tick = 1.0 / 60.0;
    rocketnet_channel_harness_t h;
    resend_timing_check( rocketnet_channel_harness_create( &h ) );
    resend_timing_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );

    if ( fixed_100ms )
    {
        rn_window_ack_t * ack = &h.message_client->object.connections[0].ordered.ack;
        ack->rto_min = 0.1;
        ack->rto_max = 0.1;
        ack->rto_initial = 0.1;
        ack->rto = 0.1;
    }

    const double latency = (double) rtt_ms / 2000.0;
    rocketnet_channel_harness_set_link( &h, latency, latency * 0.1, (float) loss_percent, 0.0f );

    const uint64_t resent_before = h.message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];

    static double send_time[RESEND_TIMING_MESSAGES];
    static double latency_ms[RESEND_TIMING_MESSAGES];
    static uint8_t seen[RESEND_TIMING_MESSAGES];
    memset( seen, 0, sizeof(seen) );

    uint8_t payload[RESEND_TIMING_BYTES];
    uint8_t state[32];
    memset( state, 0x5A, sizeof(state) );

    int sent = 0;
    int delivered = 0;
    int up = 1;
    const int max_updates = RESEND_TIMING_MESSAGES + 60 * 15;      // the sends, then up past reliable_timeout
    for ( int update = 0; update < max_updates && delivered < RESEND_TIMING_MESSAGES && up; update++ )
    {
        if ( sent < RESEND_TIMING_MESSAGES )
        {
            for ( int i = 0; i < RESEND_TIMING_BYTES; i++ )
                payload[i] = (uint8_t) ( sent * 13 + i );
            memcpy( payload, &sent, sizeof(sent) );
            resend_timing_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABLE, payload, RESEND_TIMING_BYTES ) != 0 );
            send_time[sent] = h.now;
            sent++;
        }
        rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_UNRELIABLE, state, (int) sizeof(state) );

        up = rocketnet_channel_harness_pump( &h, tick );

        rocketnet_channel_delivery_t delivery;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            resend_timing_check( delivery.bytes == RESEND_TIMING_BYTES );
            int index = -1;
            memcpy( &index, delivery.data, sizeof(index) );
            resend_timing_check( index >= 0 && index < sent && !seen[index] );
            seen[index] = 1;
            latency_ms[delivered++] = ( h.now - send_time[index] ) * 1000.0;
        }
        for ( int channel = 0; channel < h.message_config.num_channels; channel++ )
            while ( rocketnet_channel_harness_client_collect( &h, channel, &delivery ) )
            {
            }
        resend_timing_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    }

    std::sort( latency_ms, latency_ms + delivered );
    const uint64_t resent = h.message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT] - resent_before;

    row->schedule = schedule;
    row->rtt_ms = rtt_ms;
    row->loss_percent = loss_percent;
    row->delivered = delivered;
    row->timeouts = RESEND_TIMING_MESSAGES - delivered;
    row->p50_ms = resend_timing_percentile( latency_ms, delivered, 0.50 );
    row->p99_ms = resend_timing_percentile( latency_ms, delivered, 0.99 );
    row->p999_ms = resend_timing_percentile( latency_ms, delivered, 0.999 );
    row->copies = (double) ( RESEND_TIMING_MESSAGES + resent ) / (double) RESEND_TIMING_MESSAGES;

    rocketnet_channel_harness_destroy( &h );
}

void resend_timing_table_over_lossy_links()
{
    static const int rtts[] = { 20, 50, 100, 200 };
    static const int losses[] = { 10, 20 };
    // the Monte Carlo model's p99 for the rule (ms), by RTT
    static const double model_p99[] = { 77.0, 192.0, 250.0, 300.0 };
    const double two_ticks_ms = 2000.0 / 60.0;

    resend_timing_row_t rows[2][4][2];
    for ( int s = 0; s < 2; s++ )
        for ( int r = 0; r < 4; r++ )
            for ( int l = 0; l < 2; l++ )
                resend_timing_run( s == 0 ? "rule" : "fixed", s, rtts[r], losses[l], &rows[s][r][l] );

    printf( "resend_timing_table_over_lossy_links: %d reliable %d-byte messages per row, one per 60 Hz update\n",
            RESEND_TIMING_MESSAGES, RESEND_TIMING_BYTES );
    printf( "schedule rtt_ms loss_pct p50_ms p99_ms p999_ms copies timeouts model_p99_ms\n" );
    for ( int s = 0; s < 2; s++ )
        for ( int r = 0; r < 4; r++ )
            for ( int l = 0; l < 2; l++ )
            {
                const resend_timing_row_t * row = &rows[s][r][l];
                printf( "%-8s %6d %8d %6.1f %6.1f %7.1f %6.2f %8d %12.0f\n",
                        row->schedule, row->rtt_ms, row->loss_percent, row->p50_ms, row->p99_ms, row->p999_ms,
                        row->copies, row->timeouts, s == 0 ? model_p99[r] : 0.0 );
            }
    fflush( stdout );

    const double tick_ms = 1000.0 / 60.0;
    const double resend_interval_ms = 100.0;
    for ( int r = 0; r < 4; r++ )
        for ( int l = 0; l < 2; l++ )
        {
            const resend_timing_row_t * row = &rows[0][r][l];
            const resend_timing_row_t * fixed = &rows[1][r][l];
            const double ceiling_ms = rtts[r] / 2.0 + two_ticks_ms + 3.0 * ( resend_interval_ms + tick_ms );
            resend_timing_check( row->timeouts == 0 );
            resend_timing_check( fixed->timeouts == 0 );
            resend_timing_check( row->p99_ms <= fixed->p99_ms + tick_ms + 1e-6 );
            resend_timing_check( row->p99_ms <= ceiling_ms + 1e-6 );
        }
}

/*
    block_resend_cost_over_long_path: the bandwidth a resend policy spends per
    useful byte for a large block (docs/CHANNELS.md 6.4 "Resend" and "The
    cost"; tla/RNResendTiming.tla Delay("c")). The client sends one 256 KiB
    reliable-ordered block over a 200 ms path (harness latency 100 ms each
    way, jitter a tenth of that) at 10% loss both ways, while the server
    sends one 32-byte unreliable message per update so acks ride packets
    going the other way. Each run prints the client's wire bytes per block
    payload byte, the block's completion time and the copies sent per record.
    A fifth run ("rto+small") also sends one 64-byte reliable message per
    60 Hz update from 0.35 s, once the block's first sends are written, until
    the block is delivered, so the small records are resent beside chunks
    that wait for their acks; it prints those messages' p99 delivery latency.
    (Small messages queued while the block's first sends still fill every
    packet wait for numbering until they are written: that is first-write
    order, section 8, not the resend walk, and this case does not measure it.)

    Two schedules run on the same seed: "rto" is the shipped rule (a chunk
    waits the RTO alone; a small record min( rto, resend_interval )) and
    "cap100" puts the chunks back on the 100 ms cap, set on the client's
    estimator (rto_max = rto_initial = rto = 100 ms, which leaves the small
    records' delay at its 100 ms cap on this path). The case asserts: the
    block arrives whole in every run; under "rto" the client's wire bytes are
    at most 1.4 times the block's payload, and below the cap's; and the small
    messages beside the block are all delivered and never held behind its
    chunks: after every update's packets are written, the oldest unacked
    number of the client's small-record FIFO is not due
    (RNResendTiming!SmallNeverHeldByChunk on the product's walk; "small_held"
    counts the updates where it was). Their p99 is printed and not asserted:
    with a few dozen messages a run it is the slowest one, set by how many of
    its sends were lost.
*/

#define BLOCK_COST_BYTES ( 256 * 1024 )
#define BLOCK_COST_SMALL_MAX 4096
#define BLOCK_COST_SMALL_START 0.35

struct block_cost_row_t
{
    const char * schedule;
    const char * sender;
    int delivered;
    double completion_s;
    double wire_per_payload;
    double chunk_copies;
    int small_sent;
    int small_delivered;
    double small_p99_ms;
    int small_held;
};

// RNResendTiming!SmallNeverHeldByChunk on the product's walk: after an
// update's packets are written, the oldest unacked number in the client's
// small-record FIFO is not due (a due one would have been written). Returns 1
// when it is due, that is, when the update held it.
static int block_cost_small_held( const rocketnet_channel_harness_t * h )
{
    const rn_ordered_t * ordered = &h->message_client->object.connections[0].ordered;
    const rn_window_ack_t * ack = &ordered->ack;
    const rn_region_t * region = ordered->channels.region;
    for ( uint32_t cursor = ack->resend_head; cursor != RN_RESEND_NONE; cursor = region->resend_fifo[cursor].next )
    {
        const rn_send_number_t * slot = &region->send_numbers[cursor];
        if ( slot->acked )
            continue;
        return ( slot->last_send <= 0.0 || h->now >= slot->last_send + rn_window_ack_resend_delay( ack ) ) ? 1 : 0;
    }
    return 0;
}

// The two senders: "game" is the defaults (4 packets per update, the
// client's 1000 kbps), where the block's first sends take about two seconds of
// budget; "burst" writes up to 32 packets per update under 8000 kbps, so the
// whole block is in flight inside one RTT and the resend policy alone decides
// the copies.
static void block_cost_run( const char * schedule, int cap100, int burst, int with_small, block_cost_row_t * row )
{
    const double tick = 1.0 / 60.0;
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    if ( burst )
        config.max_packets_per_update = 32;
    rocketnet_channel_harness_t h;
    resend_timing_check( rocketnet_channel_harness_create_with_config( &h, &config ) );
    resend_timing_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );
    if ( burst )
        resend_timing_check( rocketnet_channel_harness_set_budgets( &h, 8000, 8000 ) );

    if ( cap100 )
    {
        rn_window_ack_t * ack = &h.message_client->object.connections[0].ordered.ack;
        ack->rto_max = 0.1;
        ack->rto_initial = 0.1;
        ack->rto = 0.1;
    }

    const double latency = 0.100;
    rocketnet_channel_harness_set_link( &h, latency, latency * 0.1, 10.0f, 0.0f );

    static uint8_t block[BLOCK_COST_BYTES];
    for ( int i = 0; i < BLOCK_COST_BYTES; i++ )
        block[i] = (uint8_t) ( i * 31 + 7 );
    const uint64_t block_hash = rocketnet_channel_hash( block, BLOCK_COST_BYTES );

    static double small_send_time[BLOCK_COST_SMALL_MAX];
    static double small_latency_ms[BLOCK_COST_SMALL_MAX];
    uint8_t small[RESEND_TIMING_BYTES];
    uint8_t state[32];
    memset( state, 0x5A, sizeof(state) );

    const uint64_t resent_before = h.message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];
    const int64_t wire_before = h.wire_bytes_to_server;
    const double start = h.now;
    resend_timing_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABLE_ORDERED, block, BLOCK_COST_BYTES ) != 0 );

    int delivered = 0;
    int small_sent = 0;
    int small_delivered = 0;
    int small_held = 0;
    double completion = 0.0;
    int64_t wire_at_completion = 0;
    uint64_t resent_at_completion = 0;
    int up = 1;
    for ( int update = 0; update < 60 * 15 && up && ( !delivered || small_delivered < small_sent ); update++ )
    {
        // the small messages start once the block's first sends are written
        // (the burst sender writes 256 KiB in about 0.27 s), so they are
        // numbered and resent beside chunks that wait for their acks
        if ( with_small && !delivered && h.now - start >= BLOCK_COST_SMALL_START && small_sent < BLOCK_COST_SMALL_MAX )
        {
            for ( int i = 0; i < RESEND_TIMING_BYTES; i++ )
                small[i] = (uint8_t) ( small_sent * 13 + i );
            memcpy( small, &small_sent, sizeof(small_sent) );
            resend_timing_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABLE, small, RESEND_TIMING_BYTES ) != 0 );
            small_send_time[small_sent++] = h.now;
        }
        rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_UNRELIABLE, state, (int) sizeof(state) );

        up = rocketnet_channel_harness_pump( &h, tick );
        if ( with_small && small_sent > 0 )
            small_held += block_cost_small_held( &h );

        // the small messages share channel 0 with the block; they are told
        // apart by size
        rocketnet_channel_delivery_t delivery;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            if ( delivery.bytes == RESEND_TIMING_BYTES )
            {
                int index = -1;
                memcpy( &index, delivery.data, sizeof(index) );
                resend_timing_check( index >= 0 && index < small_sent );
                small_latency_ms[small_delivered++] = ( h.now - small_send_time[index] ) * 1000.0;
                continue;
            }
            resend_timing_check( !delivered );
            resend_timing_check( delivery.bytes == BLOCK_COST_BYTES && delivery.hash == block_hash );
            resend_timing_check( memcmp( delivery.data, block, BLOCK_COST_BYTES ) == 0 );
            delivered = 1;
            completion = h.now - start;
            wire_at_completion = h.wire_bytes_to_server - wire_before;
            resent_at_completion = h.message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT] - resent_before;
        }
        for ( int channel = 0; channel < h.message_config.num_channels; channel++ )
            while ( rocketnet_channel_harness_client_collect( &h, channel, &delivery ) )
            {
            }
        resend_timing_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    }

    std::sort( small_latency_ms, small_latency_ms + small_delivered );

    // the block's chunks: one per fragment of the stride (8); every resend
    // counted while the block was in flight is a chunk's or a small record's
    const double chunks = ceil( (double) BLOCK_COST_BYTES / (double) RN_STORE_LARGE_BYTES );

    row->schedule = schedule;
    row->sender = burst ? "burst" : "game";
    row->delivered = delivered;
    row->completion_s = completion;
    row->wire_per_payload = (double) wire_at_completion / (double) BLOCK_COST_BYTES;
    row->chunk_copies = ( chunks + (double) resent_at_completion ) / chunks;
    row->small_sent = small_sent;
    row->small_delivered = small_delivered;
    row->small_p99_ms = resend_timing_percentile( small_latency_ms, small_delivered, 0.99 );
    row->small_held = small_held;

    rocketnet_channel_harness_destroy( &h );
}

void block_resend_cost_over_long_path()
{
    block_cost_row_t rows[5];
    block_cost_run( "rto", 0, 1, 0, &rows[0] );
    block_cost_run( "cap100", 1, 1, 0, &rows[1] );
    block_cost_run( "rto", 0, 0, 0, &rows[2] );
    block_cost_run( "cap100", 1, 0, 0, &rows[3] );
    block_cost_run( "rto+small", 0, 1, 1, &rows[4] );

    printf( "block_resend_cost_over_long_path: one %d-byte reliable-ordered block, rtt 200 ms, 10%% loss, 60 Hz\n", BLOCK_COST_BYTES );
    printf( "schedule  sender delivered completion_s wire_per_payload_byte copies_per_record small_sent small_p99_ms small_held\n" );
    for ( int s = 0; s < 5; s++ )
        printf( "%-9s %6s %9d %12.3f %21.3f %17.2f %10d %12.1f %10d\n", rows[s].schedule, rows[s].sender, rows[s].delivered, rows[s].completion_s,
                rows[s].wire_per_payload, rows[s].chunk_copies, rows[s].small_sent, rows[s].small_p99_ms, rows[s].small_held );
    fflush( stdout );

    for ( int s = 0; s < 5; s++ )
        resend_timing_check( rows[s].delivered );
    resend_timing_check( rows[0].wire_per_payload <= 1.4 );                 // the shipped rule, block in one RTT
    resend_timing_check( rows[2].wire_per_payload <= 1.4 );                 // the shipped rule, budget-paced
    resend_timing_check( rows[0].wire_per_payload < rows[1].wire_per_payload );
    resend_timing_check( rows[4].small_sent > 0 && rows[4].small_delivered == rows[4].small_sent );
    resend_timing_check( rows[4].small_held == 0 );
}

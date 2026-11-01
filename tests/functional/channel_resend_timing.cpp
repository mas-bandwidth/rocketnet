/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    test_resend_timing_table_over_lossy_links: the measured latency and cost of the
    reliable resend rule (standard/MESSAGES.md m9; proofs/tla/RNResendTiming.tla) over
    the channel harness. For each RTT (20, 50, 100 and 200 ms; harness
    latency RTT/2 each way, jitter a tenth of that) and loss (10% and 20%,
    both directions), the client sends 400 reliable 64-byte messages, one per
    60 Hz update, while the server sends one 32-byte unreliable message per
    update, as a game's state stream does, so acks ride packets going the
    other way. Each row prints p50, p99 and p99.9 delivery latency in ms
    (send to delivery at the server), the copies sent per message, and the
    messages that never arrived (timeouts).

    Two schedules run on the same seed: "rule" is the shipped default
    (the adaptive resend delay, no back-off) and "fixed" is a resend every
    100 ms, set on the client's estimator (resend_delay_min = resend_delay_max = resend_delay = 100 ms).
    The case prints the whole table beside the Monte Carlo model's p99 for
    the rule (one message's timeline, ack after RTT plus up to a tick), and
    the resend gaps each row measured.

    It asserts the rule itself, on every row of both schedules and on every
    seed, never a percentile:

    - zero timeouts;
    - every resend gap, from the client's resend gap record (struct
      rn_resend_gaps_t, measured where rn_window_ack_commit sets last_send),
      is at least the adaptive resend delay in force at the
      resend, and at most the delay in force at the update before plus one
      tick; the resend delay follows max(resend_delay_min, min(2*SRTT + 4*dev + 30 ms, resend_delay_max));
    - ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS gaps of the largest size fit
      inside reliable_timeout (RNResendTiming AttemptsBeforeTimeout);
    - every message's latency is within its own ceiling: the one-way latency
      and its jitter, the harness floor of two ticks (a send waits for its
      update, a delivery is seen at the receiver's next one) and, for each
      send of it before the one that arrived, the preceding update's formula-derived resend delay plus a tick.

    The p50, p99 and p99.9 columns are printed, not asserted. A p99 of 400
    samples is the fifth-slowest message, which moves by a whole resend when
    one more message loses one more copy, and loss in the simulator clusters,
    so neither a cross-schedule p99 comparison nor a ceiling on p99 from
    independent losses holds on every seed. The rule they stood in for is
    asserted per resend and per message above.

    The model's p99 is not asserted directly: the harness updates both ends
    at 60 Hz, so each measured RTT sample is the path's RTT plus up to two
    ticks and the ack's own update, and on a 20 ms path the resend delay follows that
    longer measured RTT. The table shows the difference row by row.

    Why the delay at the update before and not at the earlier send: the walk
    resends a number at the first update where now >= last_send + delay, with
    the delay in force at that update. An RTT sample between the two sends
    moves the resend delay; when it lowers it, the gap is longer than the new delay by
    up to the drop, and when it raises it, longer than the earlier send's
    delay. At the update before the resend the number was not due, so the
    gap is less than that update's delay plus the time to the next update.
*/

#include "channel_harness.h"

#include <math.h>

#if !ROCKETNET_DEV
#error "channel_resend_timing.cpp reads the resend gap record, which only development builds have"
#endif // #if !ROCKETNET_DEV

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
#define RESEND_TIMING_MAX_SENDS 64

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
    uint32_t gaps;                  // resends measured by the client's resend gap record
    double gap_min_ms;
    double gap_max_ms;
    double resend_delay_bound_ms;            // largest formula-derived resend delay during the run
    double early_min_ms;            // min of gap - delay at the resend
    double late_max_ms;             // max of gap - delay at the update before
    double reliable_message_timeout_ms;
    double ceiling_slack_ms;        // max over messages of latency - its own ceiling
    int max_sends;                  // most sends of one message before its delivery
};

static double resend_timing_percentile( const double * sorted, int count, double fraction )
{
    if ( count <= 0 )
        return 0.0;

    int index = (int) ceil( fraction * (double) count ) - 1;
    if ( index < 0 )
    {
        index = 0;
    }

    if ( index >= count )
    {
        index = count - 1;
    }

    return sorted[index];
}

// Independent oracle: keep the assertion separate from the production helper.
static double resend_timing_resend_delay_bound( const rn_window_ack_t * ack )
{
    if ( !ack->has_rtt )
        return ack->resend_delay_initial;
    return std::max( ack->resend_delay_min, std::min( 2.0 * ack->srtt + 4.0 * ack->rttvar + 0.030, ack->resend_delay_max ) );
}

static void resend_timing_run( const char * schedule, int fixed_100ms, int rtt_ms, int loss_percent, resend_timing_row_t * row )
{
    const double tick = 1.0 / 60.0;
    rocketnet_channel_harness_t h;
    resend_timing_check( rocketnet_channel_harness_create( &h ) );
    resend_timing_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );

    rn_ordered_t * ordered = &h.message_client->object.connections[0].ordered;
    rn_window_ack_t * ack = &ordered->ack;

    if ( fixed_100ms )
    {
        ack->resend_delay_min = 0.1;
        ack->resend_delay_max = 0.1;
        ack->resend_delay_initial = 0.1;
        ack->resend_delay = 0.1;
    }

    const double latency = (double) rtt_ms / 2000.0;
    const double jitter = latency * 0.1;
    rocketnet_channel_harness_set_link( &h, latency, jitter, (float) loss_percent, 0.0f );

    const uint64_t resent_before = h.message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];

    // the resend gap record measures this run's messages only

    for ( int c = 0; c < 2; c++ )
    {
        ack->gaps.count[c] = 0;
        ack->gaps.gap_min[c] = 1.0e30;
        ack->gaps.gap_max[c] = 0.0;
        ack->gaps.early_min[c] = 1.0e30;
        ack->gaps.late_max[c] = -1.0e30;
    }

    // the client's reliable messages are numbered in send order from here: one
    // per update, none other on the connection

    const rn_region_t * region = ordered->channels.region;
    const uint64_t first_number = ordered->send.next;

    static double send_time[RESEND_TIMING_MESSAGES];
    static double latency_ms[RESEND_TIMING_MESSAGES];
    static uint8_t seen[RESEND_TIMING_MESSAGES];
    static double sends_at[RESEND_TIMING_MESSAGES][RESEND_TIMING_MAX_SENDS];
    static int sends[RESEND_TIMING_MESSAGES];
    static double resend_ceiling_ms[RESEND_TIMING_MESSAGES][RESEND_TIMING_MAX_SENDS];
    memset( seen, 0, sizeof( seen ) );
    memset( sends, 0, sizeof( sends ) );

    uint8_t payload[RESEND_TIMING_BYTES];
    uint8_t state[32];
    memset( state, 0x5A, sizeof( state ) );

    const double tick_ms = tick * 1000.0;
    double resend_delay_bound_ms = resend_timing_resend_delay_bound( ack ) * 1000.0;
    double ceiling_slack_ms = -1.0e30;
    int max_sends = 0;

    int sent = 0;
    int delivered = 0;
    int undelivered_from = 0;
    int up = 1;
    const int max_updates = RESEND_TIMING_MESSAGES + 60 * 15;      // the sends, then up past reliable_timeout
    for ( int update = 0; update < max_updates && delivered < RESEND_TIMING_MESSAGES && up; update++ )
    {
        if ( sent < RESEND_TIMING_MESSAGES )
        {
            for ( int i = 0; i < RESEND_TIMING_BYTES; i++ )
            {
                payload[i] = (uint8_t) ( sent * 13 + i );
            }

            memcpy( payload, &sent, sizeof( sent ) );
            resend_timing_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE, payload, RESEND_TIMING_BYTES ) != 0 );
            send_time[sent] = h.now;
            sent++;
        }

        rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, state, (int) sizeof( state ) );

        const double previous_resend_delay_ms = resend_timing_resend_delay_bound( ack ) * 1000.0;

        up = rocketnet_channel_harness_pump( &h, tick );

        const double current_resend_delay_ms = resend_timing_resend_delay_bound( ack ) * 1000.0;
        resend_timing_check( fabs( ack->resend_delay * 1000.0 - current_resend_delay_ms ) <= 1e-6 );
        resend_delay_bound_ms = std::max( resend_delay_bound_ms, current_resend_delay_ms );

        // each send of each undelivered message: a send sets its number's
        // last_send to the update's time, at most once an update

        while ( undelivered_from < sent && seen[undelivered_from] )
        {
            undelivered_from++;
        }

        for ( int i = undelivered_from; i < sent; i++ )
        {
            if ( seen[i] )
                continue;

            const uint64_t number = first_number + (uint64_t) i;
            const rn_send_number_t * slot = &region->send_numbers[number & ( region->window - 1 )];
            if ( slot->number != number || slot->last_send != h.now )
                continue;
            resend_timing_check( sends[i] < RESEND_TIMING_MAX_SENDS );

            const int n = sends[i];
            if ( n > 0 )
            {
                const double gap_ms = ( h.now - sends_at[i][n - 1] ) * 1000.0;
                resend_timing_check( gap_ms >= current_resend_delay_ms - 1e-6 );
                resend_timing_check( gap_ms <= previous_resend_delay_ms + tick_ms + 1e-6 );
            }

            resend_ceiling_ms[i][n] = n == 0 ? 0.0 : resend_ceiling_ms[i][n - 1] + previous_resend_delay_ms + tick_ms;
            sends_at[i][sends[i]++] = h.now;
        }

        rocketnet_channel_delivery_t delivery = {};
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            resend_timing_check( delivery.bytes == RESEND_TIMING_BYTES );

            int index = -1;
            memcpy( &index, delivery.data, sizeof( index ) );
            resend_timing_check( index >= 0 && index < sent && !seen[index] );
            seen[index] = 1;

            const double message_ms = ( h.now - send_time[index] ) * 1000.0;
            latency_ms[delivered++] = message_ms;

            // the copy that arrived left at least the one-way latency ago:
            // the sends before it are the ones that could have carried it

            int k = 0;
            while ( k < sends[index] && sends_at[index][k] <= h.now - latency + 1e-9 )
            {
                k++;
            }

            resend_timing_check( k >= 1 );
            if ( k > max_sends )
            {
                max_sends = k;
            }

            const double ceiling_ms = ( latency + jitter ) * 1000.0 + 2.0 * tick_ms + resend_ceiling_ms[index][k - 1];
            if ( message_ms - ceiling_ms > ceiling_slack_ms )
            {
                ceiling_slack_ms = message_ms - ceiling_ms;
            }
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

    // the single-packet class (0) of the client's resend gap record

    const rn_resend_gaps_t * gaps = &ack->gaps;
    row->gaps = gaps->count[0];
    row->gap_min_ms = gaps->gap_min[0] * 1000.0;
    row->gap_max_ms = gaps->gap_max[0] * 1000.0;
    row->resend_delay_bound_ms = resend_delay_bound_ms;
    row->early_min_ms = gaps->early_min[0] * 1000.0;
    row->late_max_ms = gaps->late_max[0] * 1000.0;
    row->reliable_message_timeout_ms = ack->reliable_timeout * 1000.0;
    row->ceiling_slack_ms = ceiling_slack_ms;
    row->max_sends = max_sends;
    resend_timing_check( gaps->count[1] == 0 );         // no chunks in this case

    rocketnet_channel_harness_destroy( &h );
}

void resend_timing_table_over_lossy_links()
{
    static const int rtts[] = { 20, 50, 100, 200 };
    static const int losses[] = { 10, 20 };

    // the Monte Carlo model's p99 for the rule (ms), by RTT

    static const double model_p99[] = { 77.0, 192.0, 250.0, 300.0 };

    resend_timing_row_t rows[2][4][2];

    for ( int s = 0; s < 2; s++ )
        for ( int r = 0; r < 4; r++ )
            for ( int l = 0; l < 2; l++ )
            {
                resend_timing_run( s == 0 ? "rule" : "fixed", s, rtts[r], losses[l], &rows[s][r][l] );
            }

    printf( "test_resend_timing_table_over_lossy_links: %d reliable %d-byte messages per row, one per 60 Hz update\n",
            RESEND_TIMING_MESSAGES, RESEND_TIMING_BYTES );
    printf( "schedule rtt_ms loss_pct p50_ms p99_ms p999_ms copies timeouts model_p99_ms resends gap_min_ms gap_max_ms early_min_ms late_max_ms max_sends ceiling_slack_ms\n" );
    for ( int s = 0; s < 2; s++ )
        for ( int r = 0; r < 4; r++ )
            for ( int l = 0; l < 2; l++ )
            {
                const resend_timing_row_t * row = &rows[s][r][l];

                printf( "%-8s %6d %8d %6.1f %6.1f %7.1f %6.2f %8d %12.0f %7u %10.2f %10.2f %12.3f %11.3f %9d %16.2f\n",
                        row->schedule, row->rtt_ms, row->loss_percent, row->p50_ms, row->p99_ms, row->p999_ms,
                        row->copies, row->timeouts, s == 0 ? model_p99[r] : 0.0,
                        row->gaps, row->gaps ? row->gap_min_ms : 0.0, row->gap_max_ms, row->gaps ? row->early_min_ms : 0.0,
                        row->gaps ? row->late_max_ms : 0.0, row->max_sends, row->ceiling_slack_ms );
            }

    fflush( stdout );

    const double tick_ms = 1000.0 / 60.0;
    const double eps_ms = 1e-6;

    for ( int s = 0; s < 2; s++ )
        for ( int r = 0; r < 4; r++ )
            for ( int l = 0; l < 2; l++ )
            {
                const resend_timing_row_t * row = &rows[s][r][l];
                resend_timing_check( row->timeouts == 0 );

                // the rule, per resend: delay at the resend <= gap <= delay at the update before + a tick

                resend_timing_check( row->gaps > 0 );
                resend_timing_check( row->early_min_ms >= -eps_ms );
                resend_timing_check( row->late_max_ms <= tick_ms + eps_ms );
                resend_timing_check( row->gap_max_ms <= row->resend_delay_bound_ms + tick_ms + eps_ms );

                // the attempts rule: MIN_RESEND_ATTEMPTS of the largest gap fit inside reliable_timeout

                resend_timing_check( (double) ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS * row->gap_max_ms <= row->reliable_message_timeout_ms );

                // the rule, per message: within its own ceiling

                resend_timing_check( row->ceiling_slack_ms <= eps_ms );
            }
}

/*
    test_block_resend_cost_over_long_path: the bandwidth a resend policy spends per
    useful byte for a large block (standard/MESSAGES.md m9; proofs/tla/RNResendTiming.tla Delay("c")). The client sends one 256 KiB
    reliable-ordered block over a 200 ms path (harness latency 100 ms each
    way, jitter a tenth of that) at 10% loss both ways, while the server
    sends one 32-byte unreliable message per update so acks ride packets
    going the other way. Each run prints the client's wire bytes per block
    payload byte, the block's completion time and the copies sent per record.
    A fifth run ("resend_delay+small") also sends one 64-byte reliable message per
    60 Hz update from 0.35 s, once the block's first sends are written, until
    the block is delivered, so the small records are resent beside chunks
    that wait for their acks; it prints those messages' p99 delivery latency.
    (Small messages queued while the block's first sends still fill every
    packet wait for numbering until they are written: that is first-write
    order, section 8, not the resend walk, and this case does not measure it.)

    Two schedules run on the same seed: "resend_delay" is the shipped rule (a chunk
    waits the resend delay; a small record uses the same resend delay) and
    "cap100" puts the chunks back on the 100 ms cap, set on the client's
    estimator (resend_delay_max = resend_delay_initial = resend_delay = 100 ms, which leaves the small
    records' delay at 100 ms on this path). The case asserts: the
    block arrives whole in every run; under "resend_delay" the client's wire bytes are
    at most 1.4 times the block's payload, and below the cap's; and the small
    messages beside the block are all delivered and never held behind its
    chunks: after every update's packets are written, the oldest unacked
    number of the client's small-record FIFO is not due
    (RNResendTiming!NoDueRecordHeld on the product's walk, the small class; "small_held"
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

// RNResendTiming!NoDueRecordHeld on the product's walk, the small class: after an
// update's packets are written, the oldest unacked number in the client's
// small-record FIFO is not due (a due one would have been written). Returns 1
// when it is due, that is, when the update held it.
static int block_cost_small_held( const rocketnet_channel_harness_t * h )
{
    const rn_ordered_t * ordered = &h->message_client->object.connections[0].ordered;
    const rn_window_ack_t * ack = &ordered->ack;
    const rn_region_t * region = ordered->channels.region;
    for ( uint32_t cursor = rn_window_ack_resend_first( ack, false ); cursor != RN_RESEND_NONE; cursor = rn_window_ack_resend_next( ack, false, cursor ) )
    {
        const rn_send_number_t * slot = &region->send_numbers[cursor];
        if ( slot->acked )
            continue;
        return ( rn_send_number_unsent_due( slot->last_send ) || h->now >= slot->last_send + rn_window_ack_resend_delay( ack ) ) ? 1 : 0;
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
    {
        config.packets_per_update = 32;
    }

    rocketnet_channel_harness_t h;
    resend_timing_check( rocketnet_channel_harness_create_with_config( &h, &config ) );
    resend_timing_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );

    if ( burst )
    {
        resend_timing_check( rocketnet_channel_harness_set_budgets( &h, 8000, 8000 ) );
    }

    if ( cap100 )
    {
        rn_window_ack_t * ack = &h.message_client->object.connections[0].ordered.ack;
        ack->resend_delay_max = 0.1;
        ack->resend_delay_initial = 0.1;
        ack->resend_delay = 0.1;
    }

    const double latency = 0.100;
    rocketnet_channel_harness_set_link( &h, latency, latency * 0.1, 10.0f, 0.0f );

    static uint8_t block[BLOCK_COST_BYTES];

    for ( int i = 0; i < BLOCK_COST_BYTES; i++ )
    {
        block[i] = (uint8_t) ( i * 31 + 7 );
    }

    const uint64_t block_hash = rocketnet_channel_hash( block, BLOCK_COST_BYTES );

    static double small_send_time[BLOCK_COST_SMALL_MAX];
    static double small_latency_ms[BLOCK_COST_SMALL_MAX];
    uint8_t small[RESEND_TIMING_BYTES];
    uint8_t state[32];
    memset( state, 0x5A, sizeof( state ) );

    const uint64_t resent_before = h.message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];
    const int64_t wire_before = h.wire_bytes_to_server;
    const double start = h.now;

    resend_timing_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, block, BLOCK_COST_BYTES ) != 0 );

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
            {
                small[i] = (uint8_t) ( small_sent * 13 + i );
            }

            memcpy( small, &small_sent, sizeof( small_sent ) );
            resend_timing_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE, small, RESEND_TIMING_BYTES ) != 0 );
            small_send_time[small_sent++] = h.now;
        }

        rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, state, (int) sizeof( state ) );

        up = rocketnet_channel_harness_pump( &h, tick );

        if ( with_small && small_sent > 0 )
        {
            small_held += block_cost_small_held( &h );
        }

        // the small messages share channel 0 with the block; they are told
        // apart by size

        rocketnet_channel_delivery_t delivery = {};
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            if ( delivery.bytes == RESEND_TIMING_BYTES )
            {
                int index = -1;
                memcpy( &index, delivery.data, sizeof( index ) );
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
    block_cost_run( "resend_delay", 0, 1, 0, &rows[0] );
    block_cost_run( "cap100", 1, 1, 0, &rows[1] );
    block_cost_run( "resend_delay", 0, 0, 0, &rows[2] );
    block_cost_run( "cap100", 1, 0, 0, &rows[3] );
    block_cost_run( "resend_delay+small", 0, 1, 1, &rows[4] );

    printf( "test_block_resend_cost_over_long_path: one %d-byte reliable-ordered block, rtt 200 ms, 10%% loss, 60 Hz\n", BLOCK_COST_BYTES );
    printf( "schedule  sender delivered completion_s wire_per_payload_byte copies_per_record small_sent small_p99_ms small_held\n" );
    for ( int s = 0; s < 5; s++ )
        printf( "%-9s %6s %9d %12.3f %21.3f %17.2f %10d %12.1f %10d\n", rows[s].schedule, rows[s].sender, rows[s].delivered, rows[s].completion_s,
                rows[s].wire_per_payload, rows[s].chunk_copies, rows[s].small_sent, rows[s].small_p99_ms, rows[s].small_held );
    fflush( stdout );

    for ( int s = 0; s < 5; s++ )
    {
        resend_timing_check( rows[s].delivered );
    }

    resend_timing_check( rows[0].wire_per_payload <= 1.4 );                 // the shipped rule, block in one RTT
    resend_timing_check( rows[2].wire_per_payload <= 1.4 );                 // the shipped rule, budget-paced
    resend_timing_check( rows[0].wire_per_payload < rows[1].wire_per_payload );
    resend_timing_check( rows[4].small_sent > 0 && rows[4].small_delivered == rows[4].small_sent );
    resend_timing_check( rows[4].small_held == 0 );
}

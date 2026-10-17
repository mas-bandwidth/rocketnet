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

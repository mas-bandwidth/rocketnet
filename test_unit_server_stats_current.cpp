/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "rocketnet_ack.h"
#include <string.h>
#include <math.h>

// The server keeps per-slot statistics current as acks land, instead of
// recomputing them from the full histories on a round-robin phase sweep.
// This pins that a slot whose phase is far from the current one shows the new
// rtt, loss and rates immediately after one ack arrives -- the published
// numbers cost nothing per idle slot because they are maintained incrementally
// in O(1) per acked or received packet.

static void test_server_stats_current_after_one_update()
{
    const int num_clients = 1000;
    struct rocketnet_server_acks_t * acks = rocketnet_server_acks_create( NULL, num_clients );
    rocketnet_unit_check( acks );

    // a slot whose modulo phase is far from the first update's phase
    const int slot = 5;
    struct rocketnet_ack_endpoint_t * ep = &acks->endpoints[slot];

    const double t0 = 1000.0;
    uint64_t acked_ids[16];
    int acked_count = 0;
    uint64_t unknown = 0;
    uint64_t batch_full = 0;

    // two packets sent half a second apart, both acked one second after the
    // first was sent: known RTT samples of 1000 ms and 500 ms
    rocketnet_ack_endpoint_record_sent( ep, 1, 100, 7, 1, 0, t0 );
    rocketnet_ack_endpoint_record_sent( ep, 2, 100, 8, 1, 0, t0 + 0.5 );

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = 2;
    block.first_range_length = 2;

    int n = rocketnet_ack_endpoint_process_block( ep, &block, t0 + 1.0,
                                                  acked_ids, 16, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( n == 2 );
    rocketnet_unit_check( acked_count == 2 &&
                         ( ( acked_ids[0] == 7 && acked_ids[1] == 8 ) ||
                           ( acked_ids[0] == 8 && acked_ids[1] == 7 ) ) );

    // the published rtt is current immediately, with no phase sweep: the
    // minimum of the two samples is 500 ms
    rocketnet_unit_check( ep->rtt > 450.0f && ep->rtt < 550.0f );
    rocketnet_unit_check( ep->jitter >= 0.0f );
    rocketnet_unit_check( ep->packet_loss == 0.0f );      // every sent packet was acked
    rocketnet_unit_check( ep->bandwidth_sent_kbps > 0.0f );
    rocketnet_unit_check( ep->bandwidth_acked_kbps > 0.0f );

    // the running aggregates match the reference recompute after a longer
    // scripted history spanning the older-half window boundary

    rocketnet_ack_endpoint_reset( ep );

    const int M = 3000;
    for ( int s = 1; s <= M; s++ )
        rocketnet_ack_endpoint_record_sent( ep, (uint64_t) s, 100, (uint64_t) s, 1, 0, t0 + (double) s * 0.001 );

    // ack the first half of the history in one block
    uint64_t acked_ids2[3000];
    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    memset( &block, 0, sizeof(block) );
    block.largest = M / 2;
    block.first_range_length = M / 2;
    n = rocketnet_ack_endpoint_process_block( ep, &block, t0 + (double) M * 0.002,
                                              acked_ids2, 3000, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( n == M / 2 );

    // capture the incrementally-published values, then ask the reference
    // recompute for its answer to the same history
    const float inc_rtt = ep->rtt;
    const float inc_jitter = ep->jitter;
    const float inc_loss = ep->packet_loss;
    const float inc_sent = ep->bandwidth_sent_kbps;
    const float inc_acked = ep->bandwidth_acked_kbps;

    rocketnet_ack_endpoint_update_stats( ep );

    rocketnet_unit_check( fabs( inc_rtt - ep->rtt ) < 0.5f );
    rocketnet_unit_check( fabs( inc_jitter - ep->jitter ) < 0.5f );
    rocketnet_unit_check( fabs( inc_loss - ep->packet_loss ) < 0.5f );
    rocketnet_unit_check( fabs( inc_sent - ep->bandwidth_sent_kbps ) < 1.0f );
    rocketnet_unit_check( fabs( inc_acked - ep->bandwidth_acked_kbps ) < 1.0f );

    rocketnet_server_acks_destroy( acks );
}

REGISTER_UNIT_TEST( test_server_stats_current_after_one_update );

// The running aggregates stay equal to the reference recompute after the
// retained windows slide: a long history (well past the 4096-record sent and
// received buffers) with periodic acks, a loss pattern, gaps, late and
// duplicate receives and a jump of the high-water marks is checked against
// rocketnet_ack_endpoint_update_stats after every step.

static struct rocketnet_ack_endpoint_t g_slide_endpoint;

static int stats_close( float a, float b )
{
    const float scale = fabsf( b ) > 1.0f ? fabsf( b ) : 1.0f;
    return fabsf( a - b ) <= 0.001f * scale;
}

static void check_incremental_matches_reference( struct rocketnet_ack_endpoint_t * ep )
{
    const float inc_rtt = ep->rtt;
    const float inc_jitter = ep->jitter;
    const float inc_loss = ep->packet_loss;
    const float inc_sent = ep->bandwidth_sent_kbps;
    const float inc_acked = ep->bandwidth_acked_kbps;
    const float inc_received = ep->bandwidth_received_kbps;

    rocketnet_ack_endpoint_update_stats( ep );

    rocketnet_unit_check( ep->s_cnt >= 0 && ep->s_unacked >= 0 && ep->s_unacked <= ep->s_cnt && ep->r_cnt >= 0 );
    rocketnet_unit_check( stats_close( inc_rtt, ep->rtt ) );
    rocketnet_unit_check( stats_close( inc_jitter, ep->jitter ) );
    rocketnet_unit_check( stats_close( inc_loss, ep->packet_loss ) );
    rocketnet_unit_check( stats_close( inc_sent, ep->bandwidth_sent_kbps ) );
    rocketnet_unit_check( stats_close( inc_acked, ep->bandwidth_acked_kbps ) );
    rocketnet_unit_check( stats_close( inc_received, ep->bandwidth_received_kbps ) );
}

static void ack_run( struct rocketnet_ack_endpoint_t * ep, uint64_t low, uint64_t high, double time )
{
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = high;
    block.first_range_length = (uint32_t) ( high - low + 1 );
    uint64_t acked_ids[64];
    int acked_count = 0;
    uint64_t unknown = 0;
    uint64_t batch_full = 0;
    rocketnet_ack_endpoint_process_block( ep, &block, time, acked_ids, 64, &acked_count, &unknown, &batch_full );
}

static void test_server_stats_current_after_window_slides()
{
    struct rocketnet_ack_endpoint_t * ep = &g_slide_endpoint;
    rocketnet_ack_endpoint_reset( ep );

    const double t0 = 1000.0;
    const uint64_t N = 10000;
    uint64_t r = 0;

    for ( uint64_t s = 1; s <= N; s++ )
    {
        const double t = t0 + (double) s * 0.001;
        const int bytes = 100 + (int) ( s % 37 );

        rocketnet_ack_endpoint_record_sent( ep, s, bytes, s, 1, 0, t );
        check_incremental_matches_reference( ep );

        // acks trail the sends by five; one run in five is never acked, a
        // loss pattern that keeps acked and unacked records in every window
        if ( s % 10 == 0 && s > 15 && ( s / 10 ) % 5 != 0 )
        {
            ack_run( ep, s - 14, s - 5, t + 0.0005 );
            check_incremental_matches_reference( ep );
        }

        // received sequences with a gap every thirteenth, a late arrival
        // every 97th and a duplicate every 89th
        r++;
        if ( r % 13 == 0 )
            r++;
        rocketnet_ack_endpoint_record_received( ep, r, bytes, t );
        check_incremental_matches_reference( ep );
        if ( s % 97 == 0 && r > 200 )
        {
            rocketnet_ack_endpoint_record_received( ep, r - 150, bytes, t );
            check_incremental_matches_reference( ep );
        }
        if ( s % 89 == 0 )
        {
            rocketnet_ack_endpoint_record_received( ep, r, bytes, t );
            check_incremental_matches_reference( ep );
        }
    }

    // the high-water marks have slid far past both buffers
    rocketnet_unit_check( ep->highest_sequence > 2 * ROCKETNET_SENT_PACKET_BUFFER_SIZE );
    rocketnet_unit_check( ep->highest_received_sequence > 2 * ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    rocketnet_unit_check( ep->bandwidth_sent_kbps > 0.0f );
    rocketnet_unit_check( ep->bandwidth_acked_kbps > 0.0f );
    rocketnet_unit_check( ep->bandwidth_received_kbps > 0.0f );
    rocketnet_unit_check( ep->packet_loss > 0.0f );

    // jumps of the high-water marks: a short one (stepwise slide), under one
    // buffer (the window partly
    // keeps old records) and over the whole buffer (the window empties)
    const uint64_t jumps[] = { 100, 3000, 5000 };
    for ( int j = 0; j < 3; j++ )
    {
        const uint64_t s = ep->highest_sequence + jumps[j];
        const double t = t0 + (double) s * 0.001;
        rocketnet_ack_endpoint_record_sent( ep, s, 120, s, 1, 0, t );
        check_incremental_matches_reference( ep );
        rocketnet_ack_endpoint_record_sent( ep, s + 1, 120, s + 1, 1, 0, t + 0.001 );
        check_incremental_matches_reference( ep );
        ack_run( ep, s, s + 1, t + 0.002 );
        check_incremental_matches_reference( ep );

        const uint64_t q = ep->highest_received_sequence + jumps[j];
        rocketnet_ack_endpoint_record_received( ep, q, 120, t );
        check_incremental_matches_reference( ep );
        rocketnet_ack_endpoint_record_received( ep, q + 1, 120, t + 0.001 );
        check_incremental_matches_reference( ep );
    }
}

REGISTER_UNIT_TEST( test_server_stats_current_after_window_slides );

// rtt samples quantized to the update tick, the common case on a server that
// stamps sends and acks with its update time: many history entries equal the
// minimum, the minimum's last copy is overwritten again and again as the
// history wraps, and the published rtt and jitter still match the reference.

static void test_server_stats_current_rtt_quantized()
{
    struct rocketnet_ack_endpoint_t * ep = &g_slide_endpoint;
    rocketnet_ack_endpoint_reset( ep );

    const double tick = 0.125;
    const double t0 = 1000.0;
    const uint64_t N = 3 * ROCKETNET_RTT_HISTORY_SIZE;

    for ( uint64_t s = 1; s <= N; s++ )
    {
        const double t = t0 + (double) s * tick;
        rocketnet_ack_endpoint_record_sent( ep, s, 100, s, 1, 0, t );

        // one, two or three ticks of rtt, the shortest absent for a long
        // stretch so the history loses every copy of its minimum
        const uint64_t lag = ( s / 700 ) % 2 == 0 ? 1 + ( s % 3 ) : 2 + ( s % 2 );
        ack_run( ep, s, s, t + (double) lag * tick );
        check_incremental_matches_reference( ep );
    }
    rocketnet_unit_check( ep->rtt > 0.0f );
}

REGISTER_UNIT_TEST( test_server_stats_current_rtt_quantized );

/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The per-slot statistics are kept current by running aggregates as each
// send, receive and ack lands (sdk/rocketnet_ack.cpp, tla/RNServerStats.tla),
// instead of a round-robin recompute that reaches a slot every few dozen
// updates. These cases pin that the published numbers are current at once
// and equal the reference recompute (rocketnet_ack_endpoint_update_stats) at
// every step, including long after both retained windows have slid past
// their ring sizes. Time is an argument, starting at 1000.0.

#include "test_unit_registry.h"
#include "rocketnet_ack.h"
#include <string.h>
#include <math.h>

#define check rocketnet_unit_check

static void stats_ack_run( struct rocketnet_ack_endpoint_t * endpoint, uint64_t low, uint64_t high, double time )
{
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = high;
    block.first_range_length = (uint32_t) ( high - low + 1 );
    static uint64_t acked_ids[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    int acked_count = 0;
    uint64_t unknown = 0;
    uint64_t batch_full = 0;
    rocketnet_ack_endpoint_process_block( endpoint, &block, time, acked_ids, ROCKETNET_SENT_PACKET_BUFFER_SIZE,
                                          &acked_count, &unknown, &batch_full );
}

static void test_server_stats_current_after_one_update()
{
    const int num_clients = 1000;
    struct rocketnet_server_acks_t * acks = rocketnet_server_acks_create( NULL, num_clients );
    check( acks );

    // a slot far from any round-robin phase's first visit
    struct rocketnet_ack_endpoint_t * endpoint = &acks->endpoints[997];

    // two packets sent half a second apart, both acknowledged one second
    // after the first was sent: rtt samples of 1000 ms and 500 ms
    const double t0 = 1000.0;
    rocketnet_ack_endpoint_record_sent( endpoint, 1, 100, 7, 1, 0, t0 );
    rocketnet_ack_endpoint_record_sent( endpoint, 2, 100, 8, 1, 0, t0 + 0.5 );
    rocketnet_ack_endpoint_record_received( endpoint, 1, 200, t0 + 0.25 );
    rocketnet_ack_endpoint_record_received( endpoint, 2, 200, t0 + 0.75 );
    stats_ack_run( endpoint, 1, 2, t0 + 1.0 );

    // current at once, with no sweep: the minimum sample, the mean excess
    // over it, both packets acknowledged, and the rates over the spans
    check( fabsf( endpoint->rtt - 500.0f ) < 0.01f );
    check( fabsf( endpoint->jitter - 250.0f ) < 0.01f );
    check( endpoint->packet_loss == 0.0f );
    check( fabsf( endpoint->bandwidth_sent_kbps - 3.2f ) < 0.001f );            // 200 bytes over 0.5 s
    check( fabsf( endpoint->bandwidth_acked_kbps - 3.2f ) < 0.001f );
    check( fabsf( endpoint->bandwidth_received_kbps - 6.4f ) < 0.001f );        // 400 bytes over 0.5 s

    // and equal to the reference recompute
    const float rtt = endpoint->rtt, jitter = endpoint->jitter, sent = endpoint->bandwidth_sent_kbps;
    const float acked = endpoint->bandwidth_acked_kbps, received = endpoint->bandwidth_received_kbps;
    rocketnet_ack_endpoint_update_stats( endpoint );
    check( rtt == endpoint->rtt && fabsf( jitter - endpoint->jitter ) < 0.001f );
    check( sent == endpoint->bandwidth_sent_kbps && acked == endpoint->bandwidth_acked_kbps &&
           received == endpoint->bandwidth_received_kbps );

    // a reset slot reads as zeros again
    rocketnet_server_acks_reset_slot( acks, 997 );
    rocketnet_ack_endpoint_record_sent( endpoint, 1, 100, 9, 1, 0, t0 + 2.0 );
    check( endpoint->rtt == 0.0f && endpoint->jitter == 0.0f && endpoint->packet_loss == 0.0f );
    check( endpoint->bandwidth_sent_kbps == 0.0f && endpoint->bandwidth_received_kbps == 0.0f );

    rocketnet_server_acks_destroy( acks );
}

REGISTER_UNIT_TEST( test_server_stats_current_after_one_update );

// The window cases below compare against the reference after every call.

static struct rocketnet_ack_endpoint_t g_stats_endpoint;
static struct rocketnet_ack_endpoint_t g_stats_reference;

static bool stats_close( float a, float b )
{
    const float scale = fabsf( b ) > 1.0f ? fabsf( b ) : 1.0f;
    return fabsf( a - b ) <= 0.0001f * scale;
}

static int g_stats_comparisons;

static void stats_check_reference( const struct rocketnet_ack_endpoint_t * endpoint )
{
    memcpy( (void*) &g_stats_reference, (const void*) endpoint, sizeof(g_stats_reference) );
    rocketnet_ack_endpoint_update_stats( &g_stats_reference );
    g_stats_comparisons++;

    check( endpoint->stats_eligible_unacked <= endpoint->stats_eligible_count );
    check( endpoint->stats_eligible_count <= endpoint->stats_sent_count );
    check( endpoint->stats_acked_count <= endpoint->stats_sent_count );
    check( endpoint->stats_sent_count <= ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 );
    check( endpoint->stats_received_count <= ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2 );
    check( endpoint->rtt == g_stats_reference.rtt );
    check( stats_close( endpoint->jitter, g_stats_reference.jitter ) );
    check( endpoint->packet_loss == g_stats_reference.packet_loss );
    check( endpoint->bandwidth_sent_kbps == g_stats_reference.bandwidth_sent_kbps );
    check( endpoint->bandwidth_acked_kbps == g_stats_reference.bandwidth_acked_kbps );
    check( endpoint->bandwidth_received_kbps == g_stats_reference.bandwidth_received_kbps );
}

// Three times past both rings: acks trailing the sends with a loss pattern,
// acks landing out of order on old records, burned sequences, sends recorded
// out of sequence order, received gaps, late and duplicate arrivals, and
// jumps of both high-water marks that step, partly keep and empty the
// windows. Every ring record enters and leaves the windows many times over.

static void test_server_stats_current_after_window_slides()
{
    struct rocketnet_ack_endpoint_t * endpoint = &g_stats_endpoint;
    rocketnet_ack_endpoint_reset( endpoint );
    g_stats_comparisons = 0;

    const double t0 = 1000.0;
    const uint64_t count = 3 * ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    uint64_t sequence = 0;
    uint64_t received = 0;

    for ( uint64_t step = 1; step <= count; step++ )
    {
        const double time = t0 + (double) step * 0.001;
        const int bytes = 100 + (int) ( step % 37 );

        // every 61st sequence is burned; every 23rd pair is recorded out of
        // order, the higher sequence first, as concurrent fill writers can
        sequence++;
        if ( step % 61 == 0 )
            sequence++;
        if ( step % 23 == 0 )
        {
            rocketnet_ack_endpoint_record_sent( endpoint, sequence + 1, bytes, sequence + 1, 1, 0, time );
            stats_check_reference( endpoint );
            rocketnet_ack_endpoint_record_sent( endpoint, sequence, bytes, sequence, 1, 0, time );
            stats_check_reference( endpoint );
            sequence++;
        }
        else
        {
            rocketnet_ack_endpoint_record_sent( endpoint, sequence, bytes, sequence, 1, 0, time );
            stats_check_reference( endpoint );
        }

        // acks trail the sends by about half a ring, one run in five never
        // acknowledged; now and then an old record is acknowledged late
        if ( step % 10 == 0 && sequence > 2100 && ( step / 10 ) % 5 != 0 )
        {
            stats_ack_run( endpoint, sequence - 2100, sequence - 2091, time + 0.0005 );
            stats_check_reference( endpoint );
        }
        if ( step % 157 == 0 && sequence > 3500 )
        {
            stats_ack_run( endpoint, sequence - 3500, sequence - 3500, time + 0.0007 );
            stats_check_reference( endpoint );
        }

        // received sequences with a gap every thirteenth, a late arrival
        // every 97th, a very late one every 211th (it lands on a newer
        // record's slot) and a duplicate every 89th
        received++;
        if ( received % 13 == 0 )
            received++;
        rocketnet_ack_endpoint_record_received( endpoint, received, bytes, time );
        stats_check_reference( endpoint );
        if ( step % 97 == 0 && received > 200 )
        {
            rocketnet_ack_endpoint_record_received( endpoint, received - 150, bytes, time );
            stats_check_reference( endpoint );
        }
        if ( step % 211 == 0 && received > 2 * ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE )
        {
            rocketnet_ack_endpoint_record_received( endpoint, received - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE - 1500, bytes, time );
            stats_check_reference( endpoint );
        }
        if ( step % 89 == 0 )
        {
            rocketnet_ack_endpoint_record_received( endpoint, received, bytes, time );
            stats_check_reference( endpoint );
        }
    }

    // the high-water marks are far past both rings and every number is live
    check( endpoint->highest_sequence > 3 * ROCKETNET_SENT_PACKET_BUFFER_SIZE );
    check( endpoint->highest_received_sequence > 3 * ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    check( endpoint->rtt > 0.0f );
    check( endpoint->packet_loss > 0.0f && endpoint->packet_loss < 100.0f );
    check( endpoint->bandwidth_sent_kbps > 0.0f );
    check( endpoint->bandwidth_acked_kbps > 0.0f );
    check( endpoint->bandwidth_received_kbps > 0.0f );
    check( endpoint->stats_sent_count > ROCKETNET_SENT_PACKET_BUFFER_SIZE / 4 );
    check( endpoint->stats_received_count > ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 4 );

    // jumps of the high-water marks: a short one (a stepwise slide), one
    // under a ring (the window partly keeps old records) and one past the
    // whole ring (the window empties)
    const uint64_t jumps[] = { 100, 3000, 5000, 3 * ROCKETNET_SENT_PACKET_BUFFER_SIZE };
    double time = t0 + (double) count * 0.001;
    for ( int j = 0; j < 4; j++ )
    {
        time += 0.01;
        const uint64_t s = endpoint->highest_sequence + jumps[j];
        rocketnet_ack_endpoint_record_sent( endpoint, s, 120, s, 1, 0, time );
        stats_check_reference( endpoint );
        rocketnet_ack_endpoint_record_sent( endpoint, s + 1, 120, s + 1, 1, 0, time + 0.001 );
        stats_check_reference( endpoint );
        stats_ack_run( endpoint, s, s + 1, time + 0.002 );
        stats_check_reference( endpoint );

        const uint64_t q = endpoint->highest_received_sequence + jumps[j];
        rocketnet_ack_endpoint_record_received( endpoint, q, 120, time );
        stats_check_reference( endpoint );
        rocketnet_ack_endpoint_record_received( endpoint, q + 1, 120, time + 0.001 );
        stats_check_reference( endpoint );
    }

    check( g_stats_comparisons > 3 * (int) count );
}

REGISTER_UNIT_TEST( test_server_stats_current_after_window_slides );

// rtt samples quantized to the update tick, the common case on a server that
// stamps sends and acks with its update time: many history entries equal the
// minimum, and the minimum's last copy is overwritten again and again as the
// history wraps. Then a steadily rising rtt, where the minimum is always the
// oldest sample and every ack replaces it.

static void test_server_stats_current_rtt_minimum_moves()
{
    struct rocketnet_ack_endpoint_t * endpoint = &g_stats_endpoint;
    rocketnet_ack_endpoint_reset( endpoint );

    const double tick = 0.125;
    const double t0 = 1000.0;
    const uint64_t count = 3 * ROCKETNET_RTT_HISTORY_SIZE;
    uint64_t s = 0;

    for ( uint64_t step = 1; step <= count; step++ )
    {
        s++;
        const double time = t0 + (double) s * tick;
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, s, 1, 0, time );

        // one, two or three ticks of rtt, the shortest absent for long
        // stretches so the history loses every copy of its minimum
        const uint64_t lag = ( step / 700 ) % 2 == 0 ? 1 + ( step % 3 ) : 2 + ( step % 2 );
        stats_ack_run( endpoint, s, s, time + (double) lag * tick );
        stats_check_reference( endpoint );
    }
    check( endpoint->rtt > 124.0f && endpoint->rtt < 251.0f );

    for ( uint64_t step = 1; step <= count; step++ )
    {
        s++;
        const double time = t0 + (double) s * tick;
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, s, 1, 0, time );
        stats_ack_run( endpoint, s, s, time + 0.5 + (double) step * 0.0001 );
        stats_check_reference( endpoint );
    }
    check( endpoint->rtt > 500.0f && endpoint->jitter > 0.0f );
}

REGISTER_UNIT_TEST( test_server_stats_current_rtt_minimum_moves );

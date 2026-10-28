/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The per-connection send budget (sdk/rocketnet_send_budget.h) against its
// model, tla/RNSendBudget.tla. Time is always an argument: every test walks a
// simulated clock, nothing sleeps, nothing opens a socket.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_ack.h"
#include "rocketnet_protocol.h"
#include "rocketnet_replay.h"
#include "rocketnet_constants.h"

#include <stdint.h>
#include <string.h>
#include <limits.h>

#define check rocketnet_unit_check

// the bytes a budget of kbps may send in one second: kbps * 1000 / 8
static double budget_bytes_per_second( uint32_t kbps )
{
    return (double) kbps * 125.0;
}

// refill arithmetic is in doubles; a bucket value is compared to the byte
static bool near( double a, double b )
{
    return a - b < 1e-6 && b - a < 1e-6;
}

// empties a bucket through spend, one maximum packet at a time
static void drain( rocketnet_send_budget_t * budget )
{
    while ( rocketnet_send_budget_spend( budget, ROCKETNET_SEND_BUDGET ) )
    {
        ;
    }

    int rest = (int) budget->tokens;
    if ( rest > 0 )
    {
        check( rocketnet_send_budget_spend( budget, rest ) );
    }
}

// a deterministic generator for the workloads
static uint64_t g_lcg = 1;
static uint32_t lcg_next()
{
    g_lcg = g_lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t) ( g_lcg >> 33 );
}

// ---------------------------------------------------------------------------
// NeverOverspend: every one-second window of simulated time, under a mixed
// workload with uneven update spacing and stalls, sends at most the budget
// plus the bucket depth. ReliableNeverDropped rides along: every reliable item
// is eventually sent.
// ---------------------------------------------------------------------------

#define BUDGET_LOG_MAX 100000

struct budget_send_log_t
{
    int count;
    double time[BUDGET_LOG_MAX];
    int bytes[BUDGET_LOG_MAX];
};

static budget_send_log_t g_log;

static void log_send( budget_send_log_t * log, double now, int bytes )
{
    check( log->count < BUDGET_LOG_MAX );

    log->time[log->count] = now;
    log->bytes[log->count] = bytes;
    log->count++;
}

// the most bytes in any half-open window [t, t + 1 s), t at each send
static double max_window_bytes( const budget_send_log_t * log )
{
    double best = 0.0;
    int j = 0;
    double sum = 0.0;
    for ( int i = 0; i < log->count; i++ )
    {
        while ( j < log->count && log->time[j] < log->time[i] + 1.0 )
        {
            sum += log->bytes[j];
            j++;
        }

        if ( sum > best )
        {
            best = sum;
        }

        sum -= log->bytes[i];
    }

    return best;
}

static void test_never_exceeds_budget_per_window()
{
    const uint32_t rates[] = { 1, 64, 1000, 10000 };

    for ( int r = 0; r < 4; r++ )
    {
        const uint32_t kbps = rates[r];
        g_lcg = 1234 + (uint64_t) r;
        memset( &g_log, 0, sizeof( g_log ) );

        rocketnet_send_budget_t budget;
        check( rocketnet_send_budget_init( &budget, kbps, 0.0 ) );

        // offered load about 1.5x the budget while arrivals last, so reliable
        // items wait and unreliable ones drop; then arrivals stop and the
        // backlog must drain

        const double per_update_arrival = 1.5 * budget_bytes_per_second( kbps ) / ( 60.0 * 700.0 );
        const int arrival_whole = (int) per_update_arrival;
        const uint32_t arrival_permille = (uint32_t) ( ( per_update_arrival - arrival_whole ) * 1000.0 ) + 1;
        const double arrivals_end = kbps < 100 ? 120.0 : 12.0;

        int reliable_backlog = 0;           // reliable items queued, never dropped
        int reliable_arrived = 0;
        int reliable_sent = 0;
        double now = 0.0;
        for ( int update = 0; update < 1000000; update++ )
        {
            if ( now >= arrivals_end && reliable_backlog == 0 )
                break;

            // uneven update spacing: mostly about 16 ms, sometimes a stall

            const uint32_t roll = lcg_next() % 100;
            now += roll < 3 ? 0.4 + ( lcg_next() % 300 ) / 1000.0 : 0.001 + ( lcg_next() % 30 ) / 1000.0;

            int arrivals = 0;
            if ( now < arrivals_end )
            {
                arrivals = arrival_whole + ( lcg_next() % 1000 < arrival_permille ? 1 : 0 );
            }

            reliable_backlog += arrivals;
            reliable_arrived += arrivals;

            rocketnet_send_budget_begin_update( &budget, now );

            int sent_this_update = 0;
            while ( reliable_backlog > 0 )
            {
                const int bytes = 200 + (int) ( ( (uint32_t) reliable_sent * 2654435761u ) % 1000 );     // a fixed size per item
                const int decision = rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, bytes );
                check( decision == ROCKETNET_SEND_BUDGET_SEND || decision == ROCKETNET_SEND_BUDGET_WAIT );
                if ( decision != ROCKETNET_SEND_BUDGET_SEND )
                    break;
                reliable_backlog--;
                reliable_sent++;
                sent_this_update += bytes;
            }

            const int unreliable = arrivals;
            for ( int i = 0; i < unreliable; i++ )
            {
                const int bytes = 60 + (int) ( lcg_next() % 400 );
                const int decision = rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, bytes );
                check( decision == ROCKETNET_SEND_BUDGET_SEND || decision == ROCKETNET_SEND_BUDGET_DROP );
                if ( decision == ROCKETNET_SEND_BUDGET_SEND )
                {
                    sent_this_update += bytes;
                }
            }

            if ( sent_this_update > 0 )
            {
                log_send( &g_log, now, sent_this_update );
            }

            check( budget.tokens >= 0.0 && budget.tokens <= budget.depth );
        }

        const double limit = budget_bytes_per_second( kbps ) + budget.depth;
        check( max_window_bytes( &g_log ) <= limit );
        check( reliable_sent + reliable_backlog == reliable_arrived );
        check( reliable_backlog == 0 );                       // arrivals stopped: every reliable item was sent
        check( reliable_arrived > 0 && budget.unreliable_dropped > 0 );
        check( budget.bytes_spent > 0 );
    }
}

// ---------------------------------------------------------------------------
// A 256 KiB message, chunked into maximum payload packets, leaves over the
// window count the formula gives: the bucket starts full (depth bytes), then
// refills at the rate, so the last chunk leaves when depth + rate * t first
// covers every chunk's wire bytes.
// ---------------------------------------------------------------------------

static void large_message_case( uint32_t kbps, int expected_windows )
{
    const int message_bytes = 256 * 1024;
    const int chunk_payload = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES;
    const int chunks = ( message_bytes + chunk_payload - 1 ) / chunk_payload;

    double total_wire = 0.0;
    for ( int i = 0; i < chunks; i++ )
    {
        const int payload = i < chunks - 1 ? chunk_payload : message_bytes - chunk_payload * ( chunks - 1 );
        total_wire += rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload );
    }

    rocketnet_send_budget_t budget;
    check( rocketnet_send_budget_init( &budget, kbps, 0.0 ) );

    const double dt = 0.001;
    double now = 0.0;
    double finish = -1.0;
    int next = 0;
    memset( &g_log, 0, sizeof( g_log ) );

    for ( int step = 0; step < 100000 && next < chunks; step++ )
    {
        now = step * dt;
        rocketnet_send_budget_begin_update( &budget, now );

        int sent = 0;
        while ( next < chunks )
        {
            const int payload = next < chunks - 1 ? chunk_payload : message_bytes - chunk_payload * ( chunks - 1 );
            const int wire = rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload );
            const int decision = rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_ORDERED_PROGRESS, wire );
            if ( decision != ROCKETNET_SEND_BUDGET_SEND )
            {
                check( decision == ROCKETNET_SEND_BUDGET_WAIT );
                break;
            }

            sent += wire;
            next++;
        }

        if ( sent )
        {
            log_send( &g_log, now, sent );
        }

        if ( next == chunks )
        {
            finish = now;
        }
    }

    check( next == chunks );

    // the formula: the last byte is covered at ( total - depth ) / rate,
    // and leaves at the first update at or after that

    const double rate = budget_bytes_per_second( kbps );
    const double predicted = ( total_wire - budget.depth ) / rate;
    check( finish >= predicted - 1e-9 );
    check( finish <= predicted + dt + 1e-9 );
    check( (int) ( finish / 1.0 ) + 1 == expected_windows );
    check( max_window_bytes( &g_log ) <= rate + budget.depth );
    check( (double) budget.bytes_spent == total_wire );
}

static void test_large_message_paced_across_windows()
{
    // 1000 kbps: 269404 wire bytes, depth 12500, rate 125000 B/s: ~2.06 s, 3 windows

    large_message_case( 1000, 3 );

    // 10000 kbps: depth 125000, rate 1250000 B/s: ~0.12 s, 1 window

    large_message_case( 10000, 1 );

    check( rocketnet_send_budget_depth_bytes( 1000 ) == 12500.0 );
    check( rocketnet_send_budget_depth_bytes( 10000 ) == 125000.0 );
    check( rocketnet_send_budget_depth_bytes( 1 ) == (double) ROCKETNET_SEND_BUDGET );
}

// ---------------------------------------------------------------------------
// Each connection owns its budget: spending, refilling and changing one
// leaves another untouched.
// ---------------------------------------------------------------------------

static void test_budgets_are_per_connection()
{
    rocketnet_send_budget_t client;
    rocketnet_send_budget_t slot[2];
    check( rocketnet_send_budget_init( &client, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 ) );
    check( rocketnet_send_budget_init( &slot[0], ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, 0.0 ) );
    check( rocketnet_send_budget_init( &slot[1], ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, 0.0 ) );
    check( client.kbps == 1000 && slot[0].kbps == 10000 && slot[1].kbps == 10000 );

    // drain slot 0

    rocketnet_send_budget_begin_update( &slot[0], 0.0 );
    while ( rocketnet_send_budget_admit( &slot[0], ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1000 ) == ROCKETNET_SEND_BUDGET_SEND )
    {
        ;
    }

    check( slot[0].tokens < 1000.0 );
    check( slot[1].tokens == slot[1].depth );
    check( near( client.tokens, client.depth ) );

    // slot 1 still sends at once

    rocketnet_send_budget_begin_update( &slot[1], 0.0 );
    check( rocketnet_send_budget_admit( &slot[1], ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1000 ) == ROCKETNET_SEND_BUDGET_SEND );

    // a runtime change on one slot changes no other

    check( rocketnet_send_budget_set_kbps( &slot[0], 500, 0.0 ) );
    check( slot[0].kbps == 500 && slot[1].kbps == 10000 && client.kbps == 1000 );
    check( slot[1].depth == 125000.0 && client.depth == 12500.0 );

    // refilling one moves no other clock

    rocketnet_send_budget_refill( &slot[0], 5.0 );
    check( slot[0].last_time == 5.0 && slot[1].last_time == 0.0 && client.last_time == 0.0 );
    check( slot[1].bytes_spent == 1000 && client.bytes_spent == 0 );
}

// ---------------------------------------------------------------------------
// A reliable item that does not fit waits, and is sent once the bucket has
// refilled; an unreliable item that does not fit is dropped and counted;
// while a reliable item waits, unreliable items get none of the bucket.
// ---------------------------------------------------------------------------

static void test_reliable_waits_unreliable_drops()
{
    rocketnet_send_budget_t budget;
    check( rocketnet_send_budget_init( &budget, 1000, 0.0 ) );       // 125 bytes per ms, depth 12500

    rocketnet_send_budget_begin_update( &budget, 0.0 );
    while ( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1200 ) == ROCKETNET_SEND_BUDGET_SEND )
    {
        ;
    }

    check( near( budget.tokens, 12500.0 - 10 * 1200.0 ) );                  // 500 left

    // the next update, 1 ms later: 625 bytes. a 1200-byte reliable waits; a
    // 100-byte unreliable fits the bucket but the bucket is held: dropped

    rocketnet_send_budget_begin_update( &budget, 0.001 );
    check( near( budget.tokens, 625.0 ) );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1200 ) == ROCKETNET_SEND_BUDGET_WAIT );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 100 ) == ROCKETNET_SEND_BUDGET_DROP );
    check( budget.unreliable_dropped == 1 && budget.unreliable_dropped_bytes == 100 );
    check( near( budget.tokens, 625.0 ) );

    // 5 ms later the reliable item fits and is sent; it was never dropped

    rocketnet_send_budget_begin_update( &budget, 0.006 );
    check( near( budget.tokens, 625.0 + 625.0 ) );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1200 ) == ROCKETNET_SEND_BUDGET_SEND );
    check( near( budget.tokens, 50.0 ) );

    // no reliable waiting: an unreliable item that fits goes, one that does
    // not is dropped

    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 50 ) == ROCKETNET_SEND_BUDGET_SEND );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 1 ) == ROCKETNET_SEND_BUDGET_DROP );
    check( budget.unreliable_dropped == 2 && budget.unreliable_dropped_bytes == 101 );

    // a reliable item waits however long it takes, at the lowest rate: the
    // depth is at least one maximum packet, so it always fits eventually

    rocketnet_send_budget_t slow;
    check( rocketnet_send_budget_init( &slow, 1, 0.0 ) );

    rocketnet_send_budget_begin_update( &slow, 0.0 );
    check( rocketnet_send_budget_admit( &slow, ROCKETNET_SEND_CLASS_RETRANSMIT, ROCKETNET_SEND_BUDGET ) == ROCKETNET_SEND_BUDGET_SEND );

    int waits = 0;
    double now = 0.0;
    for ( ;; )
    {
        now += 0.5;
        rocketnet_send_budget_begin_update( &slow, now );
        if ( rocketnet_send_budget_admit( &slow, ROCKETNET_SEND_CLASS_RETRANSMIT, ROCKETNET_SEND_BUDGET ) == ROCKETNET_SEND_BUDGET_SEND )
            break;
        waits++;
        check( waits < 100 );
    }

    check( now >= (double) ROCKETNET_SEND_BUDGET / 125.0 );        // ROCKETNET_SEND_BUDGET bytes at 125 B/s: 9.856 s

    // refusals spend nothing

    const double before = budget.tokens;
    check( rocketnet_send_budget_admit( &budget, 99, 10 ) == ROCKETNET_SEND_BUDGET_REFUSED );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 0 ) == ROCKETNET_SEND_BUDGET_REFUSED );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_SEND_BUDGET + 1 ) == ROCKETNET_SEND_BUDGET_REFUSED );
    check( near( budget.tokens, before ) && budget.refusals == 3 );
    check( !rocketnet_send_budget_spend( &budget, 1000000 ) );
    check( near( budget.tokens, before ) );
}

// ---------------------------------------------------------------------------
// The class order: with room for one packet, the retransmit goes, the new
// reliable item waits, the unreliable item is dropped. A higher class offered
// after a lower one in the same update is counted and waits.
// ---------------------------------------------------------------------------

static void test_class_order_retransmit_first()
{
    check( rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_RETRANSMIT ) == 0 );
    check( rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_ORDERED_PROGRESS ) == 0 );
    check( rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_NEW_RELIABLE ) == 1 );
    check( rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_UNRELIABLE ) == 2 );

    rocketnet_send_budget_t budget;
    check( rocketnet_send_budget_init( &budget, 1000, 0.0 ) );

    rocketnet_send_budget_begin_update( &budget, 0.0 );
    while ( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1000 ) == ROCKETNET_SEND_BUDGET_SEND )
    {
        ;
    }

    // 500 left; 10 ms later 1750: one 1200-byte packet

    rocketnet_send_budget_begin_update( &budget, 0.010 );
    check( near( budget.tokens, 1750.0 ) );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_RETRANSMIT, 1200 ) == ROCKETNET_SEND_BUDGET_SEND );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_ORDERED_PROGRESS, 1200 ) == ROCKETNET_SEND_BUDGET_WAIT );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 100 ) == ROCKETNET_SEND_BUDGET_WAIT );   // fits, but the order holds
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 100 ) == ROCKETNET_SEND_BUDGET_DROP );
    check( near( budget.tokens, 550.0 ) );

    // a new update: offering new reliable, then a retransmit, breaks the order

    rocketnet_send_budget_begin_update( &budget, 0.020 );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 100 ) == ROCKETNET_SEND_BUDGET_SEND );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_RETRANSMIT, 100 ) == ROCKETNET_SEND_BUDGET_WAIT );
    check( budget.order_refusals == 1 );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 100 ) == ROCKETNET_SEND_BUDGET_SEND );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 100 ) == ROCKETNET_SEND_BUDGET_WAIT );
    check( budget.order_refusals == 2 );

    // and the next update starts the order over

    rocketnet_send_budget_begin_update( &budget, 0.021 );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_RETRANSMIT, 100 ) == ROCKETNET_SEND_BUDGET_SEND );
}

// ---------------------------------------------------------------------------
// A runtime change: the time before it refills at the old rate, the time
// after at the new one, from the next refill. Invalid budgets are refused.
// ---------------------------------------------------------------------------

static void test_runtime_change_takes_effect_next_window()
{
    // The capped token bucket keeps its rule: 0 is not a rate it holds.
    // Uncapping is the message layer's rule and holds no bucket at all
    // (sdk/rocketnet_send_budget.h's header comment).

    rocketnet_send_budget_t budget;
    check( !rocketnet_send_budget_init( &budget, 0, 0.0 ) );
    check( budget.kbps == 0 );
    check( rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 1 ) == ROCKETNET_SEND_BUDGET_REFUSED );
    check( !rocketnet_send_budget_init( &budget, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1, 0.0 ) );
    check( rocketnet_send_budget_kbps_valid( ROCKETNET_SEND_BUDGET_MAX_KBPS ) );
    check( !rocketnet_send_budget_kbps_valid( 0 ) );

    check( rocketnet_send_budget_init( &budget, 1000, 0.0 ) );

    rocketnet_send_budget_begin_update( &budget, 0.0 );
    drain( &budget );
    check( near( budget.tokens, 0.0 ) );

    // 40 ms at 1000 kbps: 5000 bytes, then the change to 10000 kbps

    check( rocketnet_send_budget_set_kbps( &budget, 10000, 0.040 ) );
    check( near( budget.tokens, 5000.0 ) );
    check( budget.depth == 125000.0 && budget.kbps == 10000 );

    // the next 40 ms refill at 10000 kbps: 50000 bytes

    rocketnet_send_budget_refill( &budget, 0.080 );
    check( near( budget.tokens, 55000.0 ) );

    // down to 1000 kbps: the bucket is capped at the new depth at once

    check( rocketnet_send_budget_set_kbps( &budget, 1000, 0.080 ) );
    check( near( budget.tokens, 12500.0 ) && budget.depth == 12500.0 );

    drain( &budget );
    rocketnet_send_budget_refill( &budget, 0.088 );
    check( near( budget.tokens, 1000.0 ) );

    // a refused change changes nothing

    check( !rocketnet_send_budget_set_kbps( &budget, 0, 0.1 ) );
    check( !rocketnet_send_budget_set_kbps( &budget, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1, 0.1 ) );
    check( budget.kbps == 1000 && near( budget.tokens, 1000.0 ) && budget.last_time == 0.088 );
    check( budget.refusals == 2 );

    // time going back adds nothing and does not move the clock

    rocketnet_send_budget_refill( &budget, 0.050 );
    check( near( budget.tokens, 1000.0 ) && budget.last_time == 0.088 );

    // a stall releases at most the depth

    rocketnet_send_budget_refill( &budget, 100.0 );
    check( near( budget.tokens, budget.depth ) );
}

// ---------------------------------------------------------------------------
// The wire-byte rule equals what the transport records: seal payload packets
// with the transport's own in-place seal and ack block builder, record them
// on an ack endpoint the way the client and server seal paths do, and compare
// each record's wire_bytes. Then pace packets through a budget, record them
// with their send times, and compare the transport's bandwidth_sent_kbps.
// ---------------------------------------------------------------------------

static rocketnet_ack_endpoint_t g_endpoint;
static rocketnet_replay_protection_t g_replay;
static uint8_t g_frame[ROCKETNET_FRAME_STRIDE];

static void test_wire_bytes_match_the_transport()
{
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 1, 2, 3, 4 };
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 5, 6, 7, 8, 9, 10, 11, 12 };
    uint8_t test_key[32];
    for ( int i = 0; i < 32; i++ )
    {
        test_key[i] = (uint8_t) ( 0x40 + i );
    }

    rocketnet_ack_endpoint_reset( &g_endpoint );
    memset( &g_replay, 0, sizeof( g_replay ) );

    // a receive history with gaps, so the ack block carries ranges

    for ( uint64_t s = 1; s <= 200; s++ )
        if ( s % 7 != 0 )
            rocketnet_replay_advance( &g_replay, s );

    const int payloads[] = { 1, 17, 100, 600, 1000, 1150, ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES };
    uint64_t sequence = 1;
    for ( int p = 0; p < (int) ( sizeof( payloads ) / sizeof( payloads[0] ) ); p++ )
    {
        const int payload_bytes = payloads[p];
        for ( int pass = 0; pass < 2; pass++ )
        {
            uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
            int capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - payload_bytes;
            if ( capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
            {
                capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;
            }

            rocketnet_replay_protection_t empty;
            memset( &empty, 0, sizeof( empty ) );
            const int block_bytes = rocketnet_ack_block_build( ack_block, capacity, pass == 0 ? &empty : &g_replay );
            check( block_bytes >= ROCKETNET_ACK_BLOCK_MIN_BYTES );

            if ( pass == 1 && payload_bytes < 1000 )
            {
                check( block_bytes > ROCKETNET_ACK_BLOCK_MIN_BYTES );
            }

            memset( g_frame, 0, sizeof( g_frame ) );
            uint8_t * wire_start = g_frame + ROCKETNET_FRAME_WIRE_OFFSET;
            uint8_t * payload = wire_start + ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET;
            for ( int i = 0; i < payload_bytes; i++ )
            {
                payload[i] = (uint8_t) i;
            }

            const int sealed = rocketnet_write_sealed_payload_in_place( wire_start, packet_filter, sequence,
                                                                        ack_block, block_bytes, payload_bytes,
                                                                        0x1122334455667788ULL, salt, test_key );
            check( sealed > 0 );

            rocketnet_ack_endpoint_record_sent( &g_endpoint, sequence, sealed, sequence, 1, 0, (double) sequence );
            const uint32_t recorded = g_endpoint.sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].wire_bytes;

            check( (int) recorded == sealed );
            check( rocketnet_send_budget_wire_bytes( block_bytes, payload_bytes ) == (int) recorded );
            check( rocketnet_send_budget_max_wire_bytes( payload_bytes ) >= (int) recorded );
            check( rocketnet_send_budget_max_wire_bytes( payload_bytes ) <= ROCKETNET_SEND_BUDGET );

            sequence++;
        }
    }

    check( rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES - 1, 10 ) == 0 );
    check( rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, 0 ) == 0 );
    check( rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, ROCKETNET_SEND_BUDGET ) == 0 );
    check( rocketnet_send_budget_max_wire_bytes( 0 ) == 0 );

    // the measured rate: pace full packets through a 1000 kbps budget from a
    // drained bucket and record each on the endpoint with its send time; the
    // transport's bandwidth_sent_kbps over the window it measures agrees

    const uint32_t kbps = 1000;
    rocketnet_ack_endpoint_reset( &g_endpoint );

    rocketnet_send_budget_t budget;
    check( rocketnet_send_budget_init( &budget, kbps, 0.0 ) );

    drain( &budget );

    const int wire = rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, 1192 );
    uint64_t seq = 1;
    for ( int step = 1; seq <= ROCKETNET_SENT_PACKET_BUFFER_SIZE; step++ )
    {
        const double now = step * 0.001;
        rocketnet_send_budget_begin_update( &budget, now );
        while ( seq <= ROCKETNET_SENT_PACKET_BUFFER_SIZE &&
                rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, wire ) == ROCKETNET_SEND_BUDGET_SEND )
        {
            rocketnet_ack_endpoint_record_sent( &g_endpoint, seq, wire, seq, 1, 0, now );
            seq++;
        }
    }

    rocketnet_ack_endpoint_update_stats( &g_endpoint );
    const double measured = (double) g_endpoint.bandwidth_sent_kbps;

    // n packets over n - 1 intervals reads slightly above the rate; 2048
    // packets bound that to well under 1%

    check( measured <= kbps * 1.01 );
    check( measured >= kbps * 0.99 );

    // settle: admitted at the bound, sealed smaller, the difference returns

    rocketnet_send_budget_t s;
    check( rocketnet_send_budget_init( &s, 1000, 0.0 ) );

    const int bound = rocketnet_send_budget_max_wire_bytes( 100 );
    check( rocketnet_send_budget_admit( &s, ROCKETNET_SEND_CLASS_NEW_RELIABLE, bound ) == ROCKETNET_SEND_BUDGET_SEND );

    const int actual = rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, 100 );
    check( rocketnet_send_budget_settle( &s, bound, actual ) );
    check( near( s.tokens, 12500.0 - actual ) && s.bytes_spent == (uint64_t) actual );
    check( !rocketnet_send_budget_settle( &s, actual, actual + 1 ) );
}

// ---------------------------------------------------------------------------
// R2: no allocator call after init. The library is restarted under a
// counting allocator (replacement is refused while initialized), the budget
// created, and the whole hot path driven; the count does not move.
// ---------------------------------------------------------------------------

static uint64_t g_alloc_calls = 0;

static void * counting_malloc( void * context, size_t bytes )
{
    (void) context;
    g_alloc_calls++;
    return malloc( bytes );
}

static void counting_free( void * context, void * p )
{
    (void) context;
    g_alloc_calls++;
    free( p );
}

static void test_send_budget_no_allocation_after_create()
{
    rocketnet_term();

    check( rocketnet_init( NULL, counting_malloc, counting_free ) == ROCKETNET_STATUS_OK );

    rocketnet_send_budget_t budgets[4];
    check( rocketnet_send_budget_init( &budgets[0], ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 ) );

    for ( int i = 1; i < 4; i++ )
    {
        check( rocketnet_send_budget_init( &budgets[i], ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, 0.0 ) );
    }

    // a live heap allocation through the hooks, so the counter is known to see them

    void * probe = rocketnet_malloc( NULL, 16 );
    check( probe && g_alloc_calls > 0 );

    rocketnet_free( NULL, probe );

    const uint64_t after_create = g_alloc_calls;
    g_lcg = 99;

    for ( int step = 1; step <= 5000; step++ )
    {
        const double now = step * 0.0167;
        for ( int i = 0; i < 4; i++ )
        {
            rocketnet_send_budget_t * b = &budgets[i];
            rocketnet_send_budget_begin_update( b, now );

            const int classes[] = { ROCKETNET_SEND_CLASS_RETRANSMIT, ROCKETNET_SEND_CLASS_ORDERED_PROGRESS,
                                    ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE };

            for ( int c = 0; c < 4; c++ )
            {
                const int payload = 1 + (int) ( lcg_next() % 1190 );
                const int bound = rocketnet_send_budget_max_wire_bytes( payload );
                if ( rocketnet_send_budget_admit( b, classes[c], bound ) == ROCKETNET_SEND_BUDGET_SEND )
                {
                    check( rocketnet_send_budget_settle( b, bound, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, payload ) ) );
                }
            }

            if ( step % 1000 == 0 )
            {
                check( rocketnet_send_budget_set_kbps( b, 1 + ( lcg_next() % 20000 ), now ) );
            }

            check( rocketnet_send_budget_can_send( b, 1 ) || b->tokens < 1.0 );
        }
    }

    check( g_alloc_calls == after_create );

    rocketnet_term();

    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// The wire-byte formula refuses any payload or ack block near INT_MAX: the
// bound is written as a subtraction, so no arithmetic on an untrusted int
// happens before its range is known, and a payload near INT_MAX cannot
// overflow the sum before the comparison (CERT INT32-C).
// ---------------------------------------------------------------------------

static void test_wire_bytes_refuses_int_max()
{
    const int imax = INT_MAX;
    check( rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, imax ) == 0 );
    check( rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, imax - 1 ) == 0 );
    check( rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, imax - ROCKETNET_PACKET_OVERHEAD_BYTES ) == 0 );
    check( rocketnet_send_budget_wire_bytes( imax, ROCKETNET_ACK_BLOCK_MIN_BYTES ) == 0 );
    check( rocketnet_send_budget_max_wire_bytes( imax ) == 0 );
    check( rocketnet_send_budget_max_wire_bytes( imax - 1 ) == 0 );
    check( rocketnet_send_budget_max_wire_bytes( imax - ROCKETNET_PACKET_OVERHEAD_BYTES ) == 0 );
}

// ---------------------------------------------------------------------------
// The boundaries, every bound computed from the macros, never a literal: the
// largest payload that fits the send budget with the minimum ack block returns
// overhead + block + payload, one more returns 0, and 0 and negative payloads
// return 0.
// ---------------------------------------------------------------------------

static void test_wire_bytes_boundaries()
{
    const int min_block = ROCKETNET_ACK_BLOCK_MIN_BYTES;
    const int fit = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - min_block;
    check( rocketnet_send_budget_wire_bytes( min_block, fit ) == ROCKETNET_PACKET_OVERHEAD_BYTES + min_block + fit );
    check( rocketnet_send_budget_wire_bytes( min_block, fit + 1 ) == 0 );
    check( rocketnet_send_budget_wire_bytes( min_block, 0 ) == 0 );
    check( rocketnet_send_budget_wire_bytes( min_block, -1 ) == 0 );
    check( rocketnet_send_budget_max_wire_bytes( fit ) == ROCKETNET_PACKET_OVERHEAD_BYTES + min_block + fit );
    check( rocketnet_send_budget_max_wire_bytes( fit + 1 ) == 0 );
    check( rocketnet_send_budget_max_wire_bytes( 0 ) == 0 );
    check( rocketnet_send_budget_max_wire_bytes( -1 ) == 0 );
}

REGISTER_UNIT_TEST( test_never_exceeds_budget_per_window );
REGISTER_UNIT_TEST( test_large_message_paced_across_windows );
REGISTER_UNIT_TEST( test_budgets_are_per_connection );
REGISTER_UNIT_TEST( test_reliable_waits_unreliable_drops );
REGISTER_UNIT_TEST( test_class_order_retransmit_first );
REGISTER_UNIT_TEST( test_runtime_change_takes_effect_next_window );
REGISTER_UNIT_TEST( test_wire_bytes_match_the_transport );
REGISTER_UNIT_TEST( test_send_budget_no_allocation_after_create );
REGISTER_UNIT_TEST( test_wire_bytes_refuses_int_max );
REGISTER_UNIT_TEST( test_wire_bytes_boundaries );

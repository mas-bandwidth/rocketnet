/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A reliable record waits for the adaptive resend delay on every resend:
// First deviation equals the RTT; later deviation moves 5% toward the
// absolute RTT error, then smoothed RTT moves 5% toward the sample. resend delay is min(2*srtt + 4*deviation + 30 ms, configured max),
// with no configured minimum floor by default and no back-off (standard/MESSAGES.md m9.2). Before the first sample it waits for
// resend_delay_initial. Validation reserves four initial resend delays before reliable_timeout.
//
// Time is an argument and the clock starts at 1000.0; nothing here reads a
// real clock, opens a socket or sleeps. Each case drives the layer 3 resend
// walk (rn_window_ack_resend) over a carved region, the same walk the
// ordered writer and the message server read the delay from.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_send.h"
#include "../sdk/rocketnet_window_ack.h"
#include "../sdk/rocketnet_channel.h"

#define check rocketnet_unit_check

// sizeof( rocketnet_message_config_t ) at BASE (ABI1): 15 int fields
#define ROCKETNET_MESSAGE_CONFIG_ABI1_BYTES 60

static const double resend_timing_start = 1000.0;
static const double resend_timing_tick = 1.0 / 60.0;

struct resend_timing_fixture_t
{
    rocketnet_message_config_t config;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    rn_window_send_t & send;
    rn_window_ack_t & ack;
    uint64_t next_packet_id;

    resend_timing_fixture_t() : send( ordered.send ), ack( ordered.ack ) {}
};

static void resend_timing_open( resend_timing_fixture_t * f )
{
    check( rocketnet_message_validate_config( &f->config ) );
    f->arena = rn_arena_create( NULL, &f->config, 1, NULL );
    check( f->arena != NULL );
    f->region = rn_arena_region( f->arena, 0 );
    rn_region_carve( f->region );
    check( rn_ordered_init( &f->ordered, f->region, &f->config, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    f->next_packet_id = 1;
}

static void resend_timing_close( resend_timing_fixture_t * f )
{
    rn_arena_destroy( f->arena );
    f->arena = NULL;
}

// Numbers one 16-byte reliable record and sends it in its own packet at now.
// Returns the packet id it went in.
static uint64_t resend_timing_send( resend_timing_fixture_t * f, double now, uint64_t * out_number )
{
    const uint32_t d = rn_region_send_alloc( f->region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &f->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    const uint64_t packet_id = f->next_packet_id++;
    rn_window_ack_commit_packet( &f->ack, packet_id, &number, 1, now );
    if ( out_number )
        *out_number = number;
    return packet_id;
}

// One resend pass at now through the product walk (rn_ordered_write), reading
// the numbers each written packet carries; returns how many it resent.
static uint32_t resend_timing_write( resend_timing_fixture_t * f, double now, uint64_t * out, uint32_t max_numbers )
{
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * payloads[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int i = 0; i < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; i++ )
        payloads[i] = frames[i];
    const int written = rn_ordered_write( &f->ordered, now, payloads, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    uint32_t resent = 0;
    for ( int p = 0; p < written; p++ )
    {
        uint32_t count = 0;
        const uint64_t * numbers = rn_packet_builder_numbers( &f->ordered.builder, p, &count );
        check( numbers != NULL || count == 0 );
        check( resent + count <= max_numbers );
        for ( uint32_t i = 0; i < count; i++ )
            out[resent++] = numbers[i];
        rn_ordered_commit( &f->ordered, p, f->next_packet_id++, now );
    }
    return resent;
}

static uint32_t resend_timing_pass( resend_timing_fixture_t * f, double now )
{
    uint64_t out[16];
    return resend_timing_write( f, now, out, 16 );
}

// test_resend_delay_is_adaptive_resend_delay: both paths use one resend delay equation, without a fixed cap.
static void test_resend_delay_is_adaptive_resend_delay()
{
    // short path: one 20 ms sample gives resend_delay = 40 + 80 + 30 = 150 ms
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );
        const double t0 = resend_timing_start;
        resend_timing_send( &f, t0, NULL );
        const uint64_t other = resend_timing_send( &f, t0, NULL );
        check( rn_window_ack_on_ack( &f.ack, other, t0 + 0.020 ) == 1 );
        check( f.ack.has_rtt == 1 );
        check( fabs( f.ack.resend_delay - 0.150 ) < 0.0005 );
        check( resend_timing_pass( &f, t0 + 0.149 ) == 0 );     // not before the resend delay
        check( resend_timing_pass( &f, t0 + 0.151 ) == 1 );     // the adaptive resend delay is due
        resend_timing_close( &f );
    }

    // long path: one sample of 400 ms exceeds the default 2 s resend delay maximum
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );
        const double t0 = resend_timing_start;
        resend_timing_send( &f, t0, NULL );
        const uint64_t other = resend_timing_send( &f, t0, NULL );
        check( rn_window_ack_on_ack( &f.ack, other, t0 + 0.400 ) == 1 );
        check( fabs( f.ack.resend_delay - 2.000 ) < 0.0005 );
        // the record is not due at its RTT sample; it resends at the resend delay
        check( resend_timing_pass( &f, t0 + 0.400 ) == 0 );
        check( resend_timing_pass( &f, t0 + 1.999 ) == 0 );
        check( resend_timing_pass( &f, t0 + 2.001 ) == 1 );
        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( test_resend_delay_is_adaptive_resend_delay );

// The equation honors caller bounds; the 2 s default is not an override
// of an application's explicit maximum.
static void test_resend_delay_uses_configured_bounds()
{
    check( fabs( rn_window_ack_compute_resend_delay( 1.0, 0.5, 0.0, 5.0 ) - 4.03 ) < 1e-12 );
    check( fabs( rn_window_ack_compute_resend_delay( 1.0, 0.5, 0.0, 3.0 ) - 3.0 ) < 1e-12 );
    check( fabs( rn_window_ack_compute_resend_delay( 0.001, 0.001, 0.0, 5.0 ) - 0.036 ) < 1e-12 );
    check( fabs( rn_window_ack_compute_resend_delay( 0.001, 0.001, 0.05, 5.0 ) - 0.05 ) < 1e-12 );
}

REGISTER_UNIT_TEST( test_resend_delay_uses_configured_bounds );

// The first deviation equals the first RTT; every subsequent sample moves
// deviation 5% toward the absolute RTT error and smoothed RTT 5% toward
// the sample.
static void test_rtt_estimator_moves_five_percent()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );

    const double t0 = resend_timing_start;
    resend_timing_send( &f, t0, NULL );
    const uint64_t first = resend_timing_send( &f, t0, NULL );
    check( rn_window_ack_on_ack( &f.ack, first, t0 + 0.200 ) == 1 );
    check( fabs( f.ack.srtt - 0.200 ) < 1e-12 );
    check( fabs( f.ack.rttvar - 0.200 ) < 1e-12 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-12 );

    const uint64_t second = resend_timing_send( &f, t0 + 0.300, NULL );
    check( rn_window_ack_on_ack( &f.ack, second, t0 + 0.700 ) == 1 );
    check( fabs( f.ack.srtt - 0.210 ) < 1e-12 );
    check( fabs( f.ack.rttvar - 0.200 ) < 1e-12 );
    check( fabs( f.ack.resend_delay - 1.250 ) < 1e-12 );
    resend_timing_close( &f );

    rocketnet_message_default_config( &f.config );
    f.config.message_resend_delay_min_ms = 200;
    resend_timing_open( &f );
    const double floored_start = resend_timing_start;
    resend_timing_send( &f, floored_start, NULL );
    const uint64_t short_sample = resend_timing_send( &f, floored_start, NULL );
    check( rn_window_ack_on_ack( &f.ack, short_sample, floored_start + 0.020 ) == 1 );
    check( fabs( f.ack.resend_delay - 0.200 ) < 1e-12 );
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_rtt_estimator_moves_five_percent );

// test_no_backoff_between_resends: a record that is never acked repeats its
// configured initial resend delay on every pass.
static void test_no_backoff_between_resends()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.message_resend_delay_initial_ms = 100;
    resend_timing_open( &f );

    const double t0 = resend_timing_start;
    resend_timing_send( &f, t0, NULL );

    double last = t0;
    int resends = 0;
    for ( int i = 1; i <= 2 * 60; i++ )
    {
        const double now = t0 + i * resend_timing_tick;
        const uint32_t resent = resend_timing_pass( &f, now );
        check( resent <= 1 );
        if ( resent == 1 )
        {
            const double gap = now - last;
            check( gap >= 0.1 - 1e-9 );
            check( gap <= 0.1 + resend_timing_tick + 1e-9 );       // the walk did not double the delay
            last = now;
            resends++;
        }
        else
        {
            check( now - last < 0.1 + 1e-9 );                      // due and not resent is a failure too
        }
    }
    check( resends >= 17 );                                        // 2 s of 100 ms gaps rounded to 60 Hz ticks
    check( f.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_no_backoff_between_resends );

// test_first_resend_before_any_sample_at_resend_delay_initial: a fresh connection waits for
// the configured default resend delay until its first sample.
static void test_first_resend_before_any_sample_at_resend_delay_initial()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );
    check( f.ack.has_rtt == 0 );
    check( f.ack.resend_delay == f.ack.resend_delay_initial );
    check( fabs( f.ack.resend_delay_initial - 2.000 ) < 1e-9 );

    const double t0 = resend_timing_start;
    resend_timing_send( &f, t0, NULL );
    check( resend_timing_pass( &f, t0 + 0.099 ) == 0 );
    check( resend_timing_pass( &f, t0 + 2.0001 ) == 1 );
    check( f.ack.has_rtt == 0 );
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_first_resend_before_any_sample_at_resend_delay_initial );

// test_config_refuses_fewer_than_four_attempts: reliable_message_timeout_ms must fit four
// sends of the longest first delay, max( the 100 ms resend interval,
// message_resend_delay_initial_ms ).
// Validation refuses and never clamps.
static void test_config_refuses_fewer_than_four_attempts()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    check( rocketnet_message_validate_config( &config ) );

    // resend_delay_initial 2 s and a 5 s timeout: two and a half first delays
    rocketnet_message_default_config( &config );
    config.message_resend_delay_min_ms = 10;
    config.message_resend_delay_initial_ms = 2000;
    config.message_resend_delay_max_ms = 2000;
    config.reliable_message_timeout_ms = 5000;
    check( !rocketnet_message_validate_config( &config ) );
    check( rocketnet_message_connection_bytes( &config ) == 0 );

    // one millisecond short of four is refused; exactly four is accepted
    config.reliable_message_timeout_ms = 7999;
    check( !rocketnet_message_validate_config( &config ) );
    config.reliable_message_timeout_ms = 8000;
    check( rocketnet_message_validate_config( &config ) );

    // the config is not repaired by validation
    check( config.message_resend_delay_initial_ms == 2000 && config.reliable_message_timeout_ms == 8000 );
}

REGISTER_UNIT_TEST( test_config_refuses_fewer_than_four_attempts );

// test_resend_timing_witness_replays: the schedule of
// tla/MCRNResendTimingBrokenBackoff.cfg against the code. The first version's
// resend delay settings (resend_delay_initial 1000 ms, resend_delay_min 50 ms, resend_delay_max 2000 ms,
// reliable_timeout 10 s), one RTT sample of 500 ms (which drives the first
// version's resend_delay to exactly 1.5 s), and the record's own sends all lost. The
// witness fits three sends before the timeout (0, 1.5 and 4.5 s); the code
// must make at least ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS.
static void test_resend_timing_witness_replays()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.message_resend_delay_initial_ms = 1000;
    f.config.message_resend_delay_min_ms = 50;
    f.config.message_resend_delay_max_ms = 2000;
    f.config.reliable_message_timeout_ms = 10000;
    resend_timing_open( &f );

    const double t0 = resend_timing_start;
    uint64_t record = 0;
    resend_timing_send( &f, t0, &record );
    const uint64_t other = resend_timing_send( &f, t0, NULL );

    int attempts = 1;
    bool sampled = false;
    bool timed_out = false;
    for ( int i = 1; i <= 11 * 60; i++ )
    {
        const double now = t0 + i * resend_timing_tick;
        if ( !sampled && now >= t0 + 0.5 )
        {
            check( rn_window_ack_on_ack( &f.ack, other, t0 + 0.5 ) == 1 );
            sampled = true;
        }
        if ( rn_window_ack_check_timeout( &f.ack, now ) )
        {
            timed_out = true;
            break;
        }
        uint64_t out[4];
        const uint32_t resent = resend_timing_write( &f, now, out, 4 );
        for ( uint32_t k = 0; k < resent; k++ )
            if ( out[k] == record )
                attempts++;
    }

    check( sampled );
    check( timed_out );                                            // every send lost: the timeout ends it
    check( f.ack.error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( attempts >= 4 );                                        // ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_resend_timing_witness_replays );

// test_message_config_abi_size_is_unchanged: the ABI canary. The resend schedule's
// interval and RTT scale are constants, so they add no field: revision 1's 15
// int fields keep their layout as the prefix ABI1 callers allocate, and the
// struct is revision 2 (the two R6 send budget fields, docs/ABI-EVOLUTION.md
// 4). A field added to ROCKETNET_MESSAGE_CONFIG_TABLE without a new revision
// grows this and fails here first.
static void test_message_config_abi_size_is_unchanged()
{
    check( ROCKETNET_MESSAGE_CONFIG_BYTES_1 == ROCKETNET_MESSAGE_CONFIG_ABI1_BYTES );
    check( offsetof( rocketnet_message_config_t, reliable_message_timeout_ms ) == ROCKETNET_MESSAGE_CONFIG_ABI1_BYTES - sizeof( int ) );
    check( sizeof( rocketnet_message_config_t ) == ROCKETNET_MESSAGE_CONFIG_BYTES_2 );
    check( sizeof( rocketnet_message_config_t ) == ROCKETNET_MESSAGE_CONFIG_ABI1_BYTES + 2 * sizeof( int ) );
}

REGISTER_UNIT_TEST( test_message_config_abi_size_is_unchanged );

// ---------------------------------------------------------------------------
// The chunks of a block wait the resend delay alone (docs/CHANNELS.md 6.4 "Resend";
// tla/RNResendTiming.tla Delay("c"), SmallNeverHeldByChunk and the reversed
// witness MCRNResendTimingBrokenOneFifo.cfg). These cases read only what
// every version of layer 3 has (the walk's output and resend_count), so they
// build against the one-FIFO walk too and show where it differs.
// ---------------------------------------------------------------------------

// Numbers a block of `bytes` (more than one fragment) and sends each chunk in
// its own packet at now. Writes the chunk numbers to out_numbers and returns
// how many there are.
static uint32_t resend_timing_send_block( resend_timing_fixture_t * f, double now, int bytes, uint64_t * out_numbers, uint32_t max_numbers )
{
    const uint32_t d = rn_region_send_take( f->region, bytes );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t first = 0, ring_start = 0;
    const uint32_t fragments = rn_window_send_first_write_block( &f->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, (uint32_t) bytes, d, &first, &ring_start );
    check( fragments > 1 && fragments <= max_numbers );
    check( f->region->send_descriptors[d].fragments == fragments );
    for ( uint32_t i = 0; i < fragments; i++ )
    {
        const uint64_t number = first + i;
        rn_window_ack_commit_packet( &f->ack, f->next_packet_id++, &number, 1, now );
        out_numbers[i] = number;
    }
    return fragments;
}

// Gives the estimator one RTT sample of `rtt` seconds, ending at the returned
// time: a small record sent at `now` and acked rtt later.
static double resend_timing_sample( resend_timing_fixture_t * f, double now, double rtt )
{
    const uint64_t packet_id = resend_timing_send( f, now, NULL );
    check( rn_window_ack_on_ack( &f->ack, packet_id, now + rtt ) == 1 );
    return now + rtt;
}

// One resend pass at now; writes what it resent to out (capacity 64).
static uint32_t resend_timing_pass_numbers( resend_timing_fixture_t * f, double now, uint64_t * out )
{
    return resend_timing_write( f, now, out, 64 );
}

static bool resend_timing_contains( const uint64_t * numbers, uint32_t count, uint64_t number )
{
    for ( uint32_t i = 0; i < count; i++ )
        if ( numbers[i] == number )
            return true;
    return false;
}

// On a 200 ms path, the adaptive resend delay is 1230 ms. Chunks wait the adaptive resend delay,
// with no fixed 100 ms cap or retry back-off.
static void test_chunk_resend_paced_by_rtt_not_interval()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );

    const double t1 = resend_timing_sample( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );

    uint64_t chunks[8];
    const uint32_t n = resend_timing_send_block( &f, t1, 2500, chunks, 8 );
    check( n == 3 );

    uint64_t out[64];
    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.229, out );
    check( resent == 0 );                                          // not at the 100 ms cap
    check( resend_timing_pass_numbers( &f, t1 + 1.229, out ) == 0 );
    resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == n );                                          // at the resend delay, every chunk
    for ( uint32_t i = 0; i < n; i++ )
        check( resend_timing_contains( out, resent, chunks[i] ) );

    // the same delay again: no back-off
    check( resend_timing_pass_numbers( &f, t1 + 1.231 + 0.101, out ) == 0 );
    check( resend_timing_pass_numbers( &f, t1 + 1.231 + 1.229, out ) == 0 );
    check( resend_timing_pass_numbers( &f, t1 + 1.231 + 1.231, out ) == n );

    // a single-packet record on the same path also waits the adaptive resend delay
    uint64_t small = 0;
    const double t2 = t1 + 2.0;
    resend_timing_send( &f, t2, &small );
    resent = resend_timing_pass_numbers( &f, t2 + 1.231, out );
    check( resend_timing_contains( out, resent, small ) );
    check( f.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_chunk_resend_paced_by_rtt_not_interval );

// A small record sent behind a block remains independently due in its own
// FIFO; its position in the small FIFO is independent of chunk timing.
static void test_small_record_behind_a_chunk_not_held()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );

    const double t1 = resend_timing_sample( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );

    uint64_t chunks[8];
    const uint32_t n = resend_timing_send_block( &f, t1, 2500, chunks, 8 );
    uint64_t small = 0;
    resend_timing_send( &f, t1 + 0.010, &small );

    uint64_t out[64];
    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == n );                                          // chunks at their resend delay
    for ( uint32_t i = 0; i < n; i++ )
        check( resend_timing_contains( out, resent, chunks[i] ) );
    resent = resend_timing_pass_numbers( &f, t1 + 1.251, out );
    check( resent == 1 && out[0] == small );                       // small record is not held by chunk FIFO
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_small_record_behind_a_chunk_not_held );

// test_both_fifos_walk_only_what_is_due: each class is walked from its head while
// due and no further. Tripwires show it: a number marked acked by hand stays
// linked (the product walk skips it without unlinking), so walk_visits grows
// past it only when a walk goes as far as the tripwire. One tripwire sits in
// each class behind numbers that are not yet due; while the walks stop before
// them only the due entries are visited, and a walk past a stop point would
// visit the tripwire too.
static void test_both_fifos_walk_only_what_is_due()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );

    const double t1 = resend_timing_sample( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );
    check( f.ack.resend_count == 0 );

    // chunks c0..c2 and small s1 at t1, small s2 at +20 ms; at +30 ms a small
    // tripwire s3 and a two-chunk block c3, c4 whose first chunk is the chunk
    // tripwire
    uint64_t c[8];
    check( resend_timing_send_block( &f, t1, 2500, c, 8 ) == 3 );
    uint64_t s1 = 0, s2 = 0, s3 = 0;
    resend_timing_send( &f, t1, &s1 );
    resend_timing_send( &f, t1 + 0.020, &s2 );
    resend_timing_send( &f, t1 + 0.030, &s3 );
    check( resend_timing_send_block( &f, t1 + 0.030, 1500, c + 3, 5 ) == 2 );
    const uint32_t W = f.region->window;
    f.region->send_numbers[s3 & ( W - 1 )].acked = 1;              // tripwires: acked, still linked
    f.region->send_numbers[c[3] & ( W - 1 )].acked = 1;
    const uint32_t linked = f.ack.resend_count;
    check( linked == 8 );

    uint64_t out[64];

    // No record is due before the 1230 ms resend delay.
    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.229, out );
    check( resent == 0 );
    check( f.ack.resend_count == linked );                         // no tripwire reached
    check( f.ack.walk_visits == 2 );                               // both heads, neither due

    // The walks stop at their heads before the resend delay
    check( resend_timing_pass_numbers( &f, t1 + 1.229, out ) == 0 );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 2 );

    // Still no record is due
    resent = resend_timing_pass_numbers( &f, t1 + 1.229, out );
    check( resent == 0 );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 2 );

    // +1231 ms: s1 and c0..c2 are due. s2 is due at +1250 ms; c3 is due at +1260 ms.
    resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == 4 );
    check( resend_timing_contains( out, resent, s1 ) && !resend_timing_contains( out, resent, s2 ) );
    for ( int i = 0; i < 3; i++ )
        check( resend_timing_contains( out, resent, c[i] ) );
    check( !resend_timing_contains( out, resent, c[4] ) );
    check( f.ack.resend_count == linked );                  // acked entries stay linked
    check( f.ack.walk_visits == 7 );                        // s1, s2 (stop), c0, c1, c2, c3 (acked), c4 (stop)

    // small records come out of the walk before chunks (R6 retransmit order)
    check( out[0] == s1 );
    resent = resend_timing_pass_numbers( &f, t1 + 1.251, out );
    check( resent == 1 && out[0] == s2 );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 5 );                        // s2, s3 (acked), s1 (stop); c3 (acked), c4 (stop)
    resent = resend_timing_pass_numbers( &f, t1 + 1.261, out );
    check( resent == 1 && out[0] == c[4] );              // the next chunk is now due
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 5 );                        // s3 (acked), s1 (stop); c3 (acked), c4
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_both_fifos_walk_only_what_is_due );

// Generation and epoch name a resend entry. A stale generation, a stale epoch,
// and the pre-wrap message number leave the FIFO alone. Time is the argument.
static void test_resend_named_generation_and_park()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );
    const double now = resend_timing_start;
    uint64_t n0 = 0, n1 = 0;
    resend_timing_send( &f, now, &n0 );
    resend_timing_send( &f, now, &n1 );
    const uint64_t numbers[2] = { n0, n1 };
    rn_window_ack_packet_failed( &f.ack, numbers, 2, now );
    rn_window_ack_packet_failed( &f.ack, numbers, 2, now );
    const uint32_t W = f.region->window;
    const uint32_t s0 = (uint32_t) ( n0 & ( W - 1 ) );
    const uint32_t s1 = (uint32_t) ( n1 & ( W - 1 ) );
    check( f.ack.resend_head == s0 );
    check( f.region->resend_fifo[s0].next == s1 );
    check( f.region->resend_fifo[s0].generation != 0 );
    check( f.region->resend_fifo[s0].generation == f.region->send_descriptors[f.region->send_numbers[s0].descriptor].generation );
    check( f.region->resend_fifo[s0].owner_epoch == f.region->schedule_epoch );
    const uint32_t generation = f.region->resend_fifo[s0].generation;
    const uint32_t epoch = f.region->schedule_epoch;
    check( rn_window_ack_cancel_resend( &f.ack, n0, generation + 1 ) == false );
    check( f.ack.resend_head == s0 );
    f.region->schedule_epoch = epoch + 1;
    check( rn_window_ack_cancel_resend( &f.ack, n0, generation ) == false );
    f.region->schedule_epoch = epoch;
    check( f.ack.resend_head == s0 );
    const uint64_t wide = n0 + ( 1ULL << 32 );
    check( rn_window_ack_cancel_resend( &f.ack, wide, generation ) == false );
    f.region->send_numbers[s0].number = wide;
    check( rn_window_ack_cancel_resend( &f.ack, n0, generation ) == false );
    check( rn_window_ack_cancel_resend( &f.ack, wide, generation ) == true );
    check( f.ack.resend_head == s1 );
    check( f.region->resend_fifo[s1].generation == f.region->send_descriptors[f.region->send_numbers[s1].descriptor].generation );
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_resend_named_generation_and_park );

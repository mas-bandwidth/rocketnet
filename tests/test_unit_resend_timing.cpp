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
#include "unit_arena.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_send.h"
#include "../sdk/rocketnet_window_ack.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_config.h"

#define check rocketnet_unit_check

// sizeof( rocketnet_message_config_t ) at BASE (ABI1): 15 int fields
#define ROCKETNET_MESSAGE_CONFIG_ABI1_BYTES 60

static const double resend_timing_start = 1000.0;
#if ROCKETNET_DEV
static const double resend_timing_tick = 1.0 / 60.0;
#endif // #if ROCKETNET_DEV

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

    f->arena = rn_unit_arena_create( &f->config, 1, NULL );
    check( f->arena != NULL );

    f->region = rn_arena_region( f->arena, 0 );
    rn_region_carve( f->region );
    check( rn_ordered_init( &f->ordered, f->region, &f->config, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, ROCKETNET_MESSAGE_SINGLE_BYTES ) );

    f->next_packet_id = 1;
}

static void resend_timing_close( resend_timing_fixture_t * f )
{
    rn_unit_arena_destroy( f->arena );
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
    {
        *out_number = number;
    }

    return packet_id;
}

#if ROCKETNET_DEV
// One resend pass at now through the product walk (rn_ordered_write), reading
// the numbers each written packet carries; returns how many it resent.
static uint32_t resend_timing_write( resend_timing_fixture_t * f, double now, uint64_t * out, uint32_t max_numbers )
{
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * payloads[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int i = 0; i < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; i++ )
    {
        payloads[i] = frames[i];
    }

    const int written = rn_ordered_write( &f->ordered, now, payloads, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    uint32_t resent = 0;
    for ( int p = 0; p < written; p++ )
    {
        uint32_t count = 0;
        const uint64_t * numbers = rn_packet_builder_numbers( &f->ordered.builder, p, &count );
        check( numbers != NULL || count == 0 );
        check( resent + count <= max_numbers );
        for ( uint32_t i = 0; i < count; i++ )
        {
            out[resent++] = numbers[i];
        }

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
#endif // #if ROCKETNET_DEV

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

#if ROCKETNET_DEV
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
#endif // #if ROCKETNET_DEV

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

// test_reliable_timeout_defaults_off: reliable_message_timeout_ms is 0 (off)
// by default in the message config and in the public client and server
// configs, and 0 validates. A nonzero value is 1000 or more and fits four
// initial resend delays; anything else is refused, never clamped.
static void test_reliable_timeout_defaults_off()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    check( config.reliable_message_timeout_ms == 0 );
    check( rocketnet_message_validate_config( &config ) );

    struct rocketnet_client_config_t client;
    check( rocketnet_client_default_config( &client ) == ROCKETNET_STATUS_OK );
    check( client.reliable_message_timeout_ms == 0 );

    struct rocketnet_server_config_t server;
    check( rocketnet_server_default_config( &server ) == ROCKETNET_STATUS_OK );
    check( server.reliable_message_timeout_ms == 0 );

#if ROCKETNET_DEV
    // the message configs the client and the server resolve from their defaults

    {
        struct rocketnet_client_transport_config_t client_transport;
        struct rocketnet_server_transport_config_t server_transport;
        rocketnet_message_config_t message;
        uint32_t kbps = 0;

        memset( &message, 0xFF, sizeof( message ) );
        check( rocketnet_client_config_resolve( NULL, false, &client_transport, &message, &kbps ) );
        check( message.reliable_message_timeout_ms == 0 );
        check( rocketnet_message_validate_config( &message ) );

        memset( &message, 0xFF, sizeof( message ) );
        check( rocketnet_server_config_resolve( NULL, false, &server_transport, &message, &kbps ) );
        check( message.reliable_message_timeout_ms == 0 );
        check( rocketnet_message_validate_config( &message ) );
    }
#endif // #if ROCKETNET_DEV

    // below 1000 is refused, even where four initial delays fit

    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 100;
    config.reliable_message_timeout_ms = 999;
    check( !rocketnet_message_validate_config( &config ) );
    check( config.reliable_message_timeout_ms == 999 && config.message_resend_delay_initial_ms == 100 );

    config.reliable_message_timeout_ms = 1;
    check( !rocketnet_message_validate_config( &config ) );
    check( config.reliable_message_timeout_ms == 1 );

    // 1000 with an initial delay of 250 ms: exactly four, accepted

    config.message_resend_delay_initial_ms = 250;
    config.reliable_message_timeout_ms = 1000;
    check( rocketnet_message_validate_config( &config ) );

    // at or above 1000 but below four initial delays is refused

    config.message_resend_delay_initial_ms = 500;
    config.reliable_message_timeout_ms = 1999;
    check( !rocketnet_message_validate_config( &config ) );
    check( config.reliable_message_timeout_ms == 1999 && config.message_resend_delay_initial_ms == 500 );

    config.reliable_message_timeout_ms = 2000;
    check( rocketnet_message_validate_config( &config ) );
}

REGISTER_UNIT_TEST( test_reliable_timeout_defaults_off );

#if ROCKETNET_DEV
// test_resend_timing_witness_replays: the schedule of
// proofs/tla/MCRNResendTimingBrokenBackoff.cfg against the code. The first version's
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
        {
            if ( out[k] == record )
            {
                attempts++;
            }
        }
    }

    check( sampled );
    check( timed_out );                                            // every send lost: the timeout ends it
    check( f.ack.error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( attempts >= 4 );                                        // ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_resend_timing_witness_replays );
#endif // #if ROCKETNET_DEV

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
// The chunks of a block wait the resend delay alone (standard/MESSAGES.md m9.3;
// proofs/tla/RNResendTiming.tla Delay("c"), NoDueRecordHeld and the reversed
// witness MCRNResendTimingBrokenOneFifo.cfg). These cases read only what
// every version of layer 3 has (the walk's output and resend_count), so they
// build against the one-FIFO walk too and show where it differs.
// ---------------------------------------------------------------------------

#if ROCKETNET_DEV
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
    {
        if ( numbers[i] == number )
        {
            return true;
        }
    }
    return false;
}

// On a 200 ms path, the adaptive resend delay is 1230 ms. Chunks wait the adaptive resend delay,
// with no fixed 100 ms cap or retry back-off. The case opts in to the 10 s
// reliable timeout: its 1230 ms delay is past the 1 s probe pacing window, which
// the default (0) would apply, and the 2.5 s chunk cap is above the delay.
static void test_chunk_resend_paced_by_rtt_not_interval()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.reliable_message_timeout_ms = 10000;
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
    {
        check( resend_timing_contains( out, resent, chunks[i] ) );
    }

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
// FIFO; its position in the small FIFO is independent of chunk timing. Opted
// in at 10 s so probe pacing (default 0, past 1 s with no progress) does not
// hold everything but the oldest number.
static void test_small_record_behind_a_chunk_not_held()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.reliable_message_timeout_ms = 10000;
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
    {
        check( resend_timing_contains( out, resent, chunks[i] ) );
    }

    resent = resend_timing_pass_numbers( &f, t1 + 1.251, out );
    check( resent == 1 && out[0] == small );                       // small record is not held by chunk FIFO

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_small_record_behind_a_chunk_not_held );

// test_chunk_behind_a_small_record_not_held: the one-FIFO hazard the other way
// round (proofs/tla/RNResendTiming.tla NoDueRecordHeld and the reversed witness
// MCRNResendTimingBrokenOneFifo.cfg). With reliable_message_timeout_ms 4000 the
// chunk cap is 1 s, below the 1230 ms resend delay a 200 ms path gives; a
// small record sent before a block is at the head of a single FIFO ordered by
// last send, not yet due when the chunks are, and would hold them back. With
// one FIFO per class the chunks go at their capped delay and the small record
// at its own.
static void test_chunk_behind_a_small_record_not_held()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.reliable_message_timeout_ms = 4000;
    f.config.message_resend_delay_initial_ms = 1000;
    f.config.message_resend_delay_max_ms = 2000;
    resend_timing_open( &f );

    const double t1 = resend_timing_sample( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );
    check( fabs( rn_window_ack_chunk_resend_delay( &f.ack ) - 1.000 ) < 1e-9 );

    uint64_t small = 0;
    resend_timing_send( &f, t1, &small );
    uint64_t chunks[8];
    const uint32_t n = resend_timing_send_block( &f, t1 + 0.010, 2500, chunks, 8 );

    uint64_t out[64];
    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.011, out );
    check( resent == n );                                          // chunks at the capped delay
    for ( uint32_t i = 0; i < n; i++ )
    {
        check( resend_timing_contains( out, resent, chunks[i] ) );
    }
    check( !resend_timing_contains( out, resent, small ) );

    resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == 1 && out[0] == small );                       // the small record at its own delay

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_chunk_behind_a_small_record_not_held );

// test_both_fifos_walk_only_what_is_due: each class's numbers on the wire are
// walked from its sent mark while due and no further. walk_visits counts every
// entry a walk examines: the due entries and one stop per class, so a walk
// past a stop point would count more. Opted in at 10 s so probe pacing (default
// 0, past 1 s with no progress) does not hold everything but the oldest number.
static void test_both_fifos_walk_only_what_is_due()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.reliable_message_timeout_ms = 10000;
    resend_timing_open( &f );

    const double t1 = resend_timing_sample( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );
    check( f.ack.resend_count == 0 );

    // chunks c0..c2 and small s1 at t1, small s2 at +20 ms; at +30 ms a small
    // s3 and a two-chunk block c3, c4

    uint64_t c[8];
    check( resend_timing_send_block( &f, t1, 2500, c, 8 ) == 3 );

    uint64_t s1 = 0, s2 = 0, s3 = 0;
    resend_timing_send( &f, t1, &s1 );
    resend_timing_send( &f, t1 + 0.020, &s2 );
    resend_timing_send( &f, t1 + 0.030, &s3 );
    check( resend_timing_send_block( &f, t1 + 0.030, 1500, c + 3, 5 ) == 2 );

    const uint32_t linked = f.ack.resend_count;
    check( linked == 8 );

    uint64_t out[64];

    // no record is due before the 1230 ms resend delay

    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.229, out );
    check( resent == 0 );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 2 );                               // both heads, neither due

    // the walks stop at their heads before the resend delay

    check( resend_timing_pass_numbers( &f, t1 + 1.229, out ) == 0 );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 2 );

    // still no record is due

    resent = resend_timing_pass_numbers( &f, t1 + 1.229, out );
    check( resent == 0 );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 2 );

    // +1231 ms: s1 and c0..c2 are due. s2 is due at +1250 ms; c3 is due at +1260 ms

    resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == 4 );
    check( resend_timing_contains( out, resent, s1 ) && !resend_timing_contains( out, resent, s2 ) );

    for ( int i = 0; i < 3; i++ )
    {
        check( resend_timing_contains( out, resent, c[i] ) );
    }

    check( !resend_timing_contains( out, resent, c[4] ) );
    check( f.ack.resend_count == linked );                  // a resend moves its number to the tail
    check( f.ack.walk_visits == 6 );                        // s1, s2 (stop), c0, c1, c2, c3 (stop)

    // small records come out of the walk before chunks (R6 retransmit order)

    check( out[0] == s1 );

    resent = resend_timing_pass_numbers( &f, t1 + 1.251, out );
    check( resent == 1 && out[0] == s2 );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 3 );                        // s2, s3 (stop); c3 (stop)

    resent = resend_timing_pass_numbers( &f, t1 + 1.261, out );
    check( resent == 3 && out[0] == s3 );                   // s3 and the block c3, c4 are now due
    check( resend_timing_contains( out, resent, c[3] ) && resend_timing_contains( out, resent, c[4] ) );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 5 );                        // s3, s1 (stop); c3, c4, c0 (stop)

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_both_fifos_walk_only_what_is_due );

// The three cases above under the default (no reliable timeout), where the
// 1230 ms resend delay is past the 1 s probe pacing window: once a second
// has passed since U's first send with no number newly acknowledged, only U
// (the oldest unacknowledged number) is resent, at its own delay; every
// other number on the wire is held, chunk or small record; the first
// acknowledgement of a number ends the hold and the held numbers that are
// due go at once.

// Returns the packet ids a block's chunks went in: resend_timing_send_block
// gives each chunk its own packet, in number order, from next_packet_id.
static uint64_t resend_timing_next_packet( const resend_timing_fixture_t * f )
{
    return f->next_packet_id;
}

// resend_timing_sample through the update's ack path (rn_ordered_acks), as a
// connection takes it, so the ack also counts as the last ack for the RTT
// filter: a later ack after an owed second then ends a silence, and is not
// the first ack of the connection (which ends none).
static double resend_timing_sample_acks( resend_timing_fixture_t * f, double now, double rtt )
{
    const uint64_t packet_id = resend_timing_send( f, now, NULL );
    check( rn_ordered_acks( &f->ordered, &packet_id, 1, now + rtt ) == 1 );
    check( f->ack.has_last_ack == 1 );
    return now + rtt;
}

static void test_chunk_resend_paced_by_rtt_not_interval_default()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    check( f.config.reliable_message_timeout_ms == 0 );
    resend_timing_open( &f );

    const double t1 = resend_timing_sample_acks( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );

    uint64_t chunks[8];
    const uint64_t first_packet = resend_timing_next_packet( &f );
    const uint32_t n = resend_timing_send_block( &f, t1, 2500, chunks, 8 );
    check( n == 3 );
    check( f.send.U == chunks[0] );

    uint64_t out[64];
    check( resend_timing_pass_numbers( &f, t1 + 1.229, out ) == 0 );   // not at the 100 ms cap
    check( !rn_window_ack_probing( &f.ack, t1 + 0.999 ) && rn_window_ack_probing( &f.ack, t1 + 1.000 ) );

    // at the resend delay, past the probe window: U alone

    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == 1 && out[0] == chunks[0] );

    // U again at the same delay, no back-off; its siblings stay held

    check( resend_timing_pass_numbers( &f, t1 + 1.231 + 0.101, out ) == 0 );
    check( resend_timing_pass_numbers( &f, t1 + 1.231 + 1.229, out ) == 0 );
    resent = resend_timing_pass_numbers( &f, t1 + 1.231 + 1.231, out );
    check( resent == 1 && out[0] == chunks[0] );

    // a single-packet record sent now is held past its delay as well

    uint64_t small = 0;
    const double t2 = t1 + 2.0;
    resend_timing_send( &f, t2, &small );
    check( resend_timing_pass_numbers( &f, t2 + 1.231, out ) == 0 );

    // progress: chunk 1's packet is acknowledged. The hold ends; chunk 2 and
    // the small record, past their delay, go at once; U, resent at
    // t1 + 2.462, is not due. The ack waited out an owed second, so it ends
    // a silence and gives no sample: the delay is unchanged.

    const double release = t1 + 3.300;
    const uint64_t chunk1_packet = first_packet + 1;
    check( rn_ordered_acks( &f.ordered, &chunk1_packet, 1, release ) == 1 );
    check( !rn_window_ack_probing( &f.ack, release ) );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );
    resent = resend_timing_pass_numbers( &f, release, out );
    check( resent == 2 );
    check( resend_timing_contains( out, resent, chunks[2] ) && resend_timing_contains( out, resent, small ) );
    check( f.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_chunk_resend_paced_by_rtt_not_interval_default );

static void test_small_record_behind_a_chunk_not_held_default()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    check( f.config.reliable_message_timeout_ms == 0 );
    resend_timing_open( &f );

    const double t1 = resend_timing_sample_acks( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );

    uint64_t chunks[8];
    const uint64_t first_packet = resend_timing_next_packet( &f );
    const uint32_t n = resend_timing_send_block( &f, t1, 2500, chunks, 8 );
    check( n == 3 );
    uint64_t small = 0;
    resend_timing_send( &f, t1 + 0.010, &small );

    // the chunk at U alone; the small record, due at +1251 ms, is held by
    // the probe, not by the chunk FIFO

    uint64_t out[64];
    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == 1 && out[0] == chunks[0] );
    check( resend_timing_pass_numbers( &f, t1 + 1.251, out ) == 0 );

    // progress ends the hold: the small record goes first (R6), then the
    // chunk still due; U is not due again until t1 + 2.461

    const uint64_t chunk1_packet = first_packet + 1;
    check( rn_ordered_acks( &f.ordered, &chunk1_packet, 1, t1 + 1.300 ) == 1 );
    resent = resend_timing_pass_numbers( &f, t1 + 1.300, out );
    check( resent == 2 && out[0] == small && out[1] == chunks[2] );

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_small_record_behind_a_chunk_not_held_default );

// In the probe state each class's walk still visits every number on the wire
// that is past its delay (held, it is skipped) and stops at the first that is
// not: at most the numbers linked plus one stop per class, never more, and
// one resend (U) per update in which U is due.
static void test_both_fifos_walk_only_what_is_due_default()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    check( f.config.reliable_message_timeout_ms == 0 );
    resend_timing_open( &f );

    const double t1 = resend_timing_sample_acks( &f, resend_timing_start, 0.200 );
    check( fabs( f.ack.resend_delay - 1.230 ) < 1e-9 );

    // chunks c0..c2 and small s1 at t1, small s2 at +20 ms; at +30 ms a small
    // s3 and a two-chunk block c3, c4. U is c0.

    uint64_t c[8];
    const uint64_t first_packet = resend_timing_next_packet( &f );
    check( resend_timing_send_block( &f, t1, 2500, c, 8 ) == 3 );

    uint64_t s1 = 0, s2 = 0, s3 = 0;
    resend_timing_send( &f, t1, &s1 );
    resend_timing_send( &f, t1 + 0.020, &s2 );
    resend_timing_send( &f, t1 + 0.030, &s3 );
    check( resend_timing_send_block( &f, t1 + 0.030, 1500, c + 3, 5 ) == 2 );
    check( f.send.U == c[0] );

    const uint32_t linked = f.ack.resend_count;
    check( linked == 8 );

    uint64_t out[64];

    // probing from t1 + 1.0, nothing due yet: both heads, neither due

    check( rn_window_ack_probing( &f.ack, t1 + 1.229 ) );
    check( resend_timing_pass_numbers( &f, t1 + 1.229, out ) == 0 );
    check( f.ack.walk_visits == 2 );

    // +1231 ms: only c0 (U) goes. s1 held, s2 (stop); c0, c1 and c2 held, c3 (stop)

    uint32_t resent = resend_timing_pass_numbers( &f, t1 + 1.231, out );
    check( resent == 1 && out[0] == c[0] );
    check( f.ack.resend_count == linked );
    check( f.ack.walk_visits == 6 );

    // +1251 ms: nothing. s1, s2 held, s3 (stop); c1, c2 held, c3 (stop)

    check( resend_timing_pass_numbers( &f, t1 + 1.251, out ) == 0 );
    check( f.ack.walk_visits == 6 );

    // +1261 ms: nothing. s1, s2, s3 held (the end); c1..c4 held, c0 (stop)

    check( resend_timing_pass_numbers( &f, t1 + 1.261, out ) == 0 );
    check( f.ack.walk_visits == 8 );
    check( f.ack.walk_visits <= linked + 2 );

    // progress (c1's packet acknowledged) ends the hold: every held number
    // past its delay goes, small records first; c0 is not due again yet

    const uint64_t c1_packet = first_packet + 1;
    check( rn_ordered_acks( &f.ordered, &c1_packet, 1, t1 + 1.300 ) == 1 );
    check( f.ack.resend_count == linked - 1 );
    resent = resend_timing_pass_numbers( &f, t1 + 1.300, out );
    check( resent == 6 );
    check( out[0] == s1 && out[1] == s2 && out[2] == s3 );
    check( resend_timing_contains( out, resent, c[2] ) && resend_timing_contains( out, resent, c[3] ) && resend_timing_contains( out, resent, c[4] ) );
    check( !resend_timing_contains( out, resent, c[0] ) );
    check( f.ack.walk_visits == 7 );                        // s1, s2, s3 (the end); c2, c3, c4, c0 (stop)

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_both_fifos_walk_only_what_is_due_default );

// ---------------------------------------------------------------------------
// Probe pacing and the RTT filter (standard/MESSAGES.md m9.2, m9.3), on only
// with the default reliable timeout (0, off). Once RN_PROBE_SECONDS pass with
// no reliable number newly acknowledged, counted from the later of the last
// such update and the first send of the oldest unacknowledged number U, only
// U is resent; a number never sent still goes once; the first update that
// newly acknowledges a number ends the hold. An ack after RN_PROBE_SECONDS of
// no ack ends a silence, and a packet sent before that end less srtt gives no
// RTT sample. An opted-in reliable timeout keeps v1.0 whole.
// ---------------------------------------------------------------------------

static uint32_t resend_timing_count( const uint64_t * numbers, uint32_t count, uint64_t number )
{
    uint32_t found = 0;
    for ( uint32_t i = 0; i < count; i++ )
    {
        if ( numbers[i] == number )
        {
            found++;
        }
    }
    return found;
}

// Starts and finishes one 16-byte RELIABLE_ORDERED message on channel 0: a
// number the next write pass sends for the first time.
static void resend_timing_queue( resend_timing_fixture_t * f )
{
    uint8_t * p = rn_channels_start( &f->ordered.channels, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 16 );
    check( p != NULL );
    memset( p, 0x5A, 16 );
    check( rn_channels_finish( &f->ordered.channels, 16 ) );
}

static void test_probe_pacing_resends_only_the_oldest_without_progress()
{
    // the default: no reliable timeout, probe pacing on
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        check( f.config.reliable_message_timeout_ms == 0 );
        resend_timing_open( &f );
        check( f.ack.reliable_timeout == 0.0 );

        // one 50 ms sample: the resend delay is 2 * 0.05 + 4 * 0.05 + 0.03 = 330 ms

        check( rn_window_ack_seed_rtt( &f.ack, 0.050 ) == 1 );
        check( fabs( f.ack.resend_delay - 0.330 ) < 1e-9 );

        const double t0 = resend_timing_start;
        uint64_t r0 = 0, r1 = 0, r2 = 0;
        resend_timing_send( &f, t0, &r0 );
        const uint64_t p1 = resend_timing_send( &f, t0, &r1 );
        resend_timing_send( &f, t0, &r2 );
        check( f.send.U == r0 );

        uint64_t out[64];
        uint32_t resent = 0;

        // inside the first second nothing is held: every due number is resent at its delay

        const double inside[2] = { t0 + 0.34, t0 + 0.68 };
        for ( int i = 0; i < 2; i++ )
        {
            check( !rn_window_ack_probing( &f.ack, inside[i] ) );
            resent = resend_timing_pass_numbers( &f, inside[i], out );
            check( resent == 3 );
            check( resend_timing_count( out, resent, r0 ) == 1 && resend_timing_count( out, resent, r1 ) == 1 && resend_timing_count( out, resent, r2 ) == 1 );
        }
        check( !rn_window_ack_probing( &f.ack, t0 + 0.999 ) );
        check( rn_window_ack_probing( &f.ack, t0 + 1.000 ) );       // 1.0 s from U's first send, no progress

        // past it, over five resend delays: only U, at its delay; r1 and r2 are
        // due every pass and held. A number never sent goes once, at t0 + 1.10.

        const uint64_t r3 = f.send.next;
        uint32_t count[4] = { 0, 0, 0, 0 };
        const double held[6] = { t0 + 1.02, t0 + 1.10, t0 + 1.36, t0 + 1.70, t0 + 2.04, t0 + 2.38 };
        for ( int i = 0; i < 6; i++ )
        {
            if ( i == 1 )
            {
                resend_timing_queue( &f );
            }
            check( rn_window_ack_probing( &f.ack, held[i] ) );
            resent = resend_timing_pass_numbers( &f, held[i], out );
            check( resent == 1 );
            count[0] += resend_timing_count( out, resent, r0 );
            count[1] += resend_timing_count( out, resent, r1 );
            count[2] += resend_timing_count( out, resent, r2 );
            count[3] += resend_timing_count( out, resent, r3 );
        }
        check( count[0] == 5 );                                     // U at every resend delay
        check( count[1] == 0 && count[2] == 0 );                    // the rest held
        check( count[3] == 1 );                                     // never sent: sent once, then held
        check( f.send.next == r3 + 1 );
        check( f.send.U == r0 );

        // an ack-only update that acknowledges no reliable number (the ack of
        // a packet the sent map never held, as a keep-alive's) is not progress

        const double last_progress = f.ack.last_progress;
        const uint64_t not_reliable = f.next_packet_id + 1000;
        check( rn_ordered_acks( &f.ordered, &not_reliable, 1, t0 + 2.40 ) == 0 );
        check( f.ack.last_progress == last_progress );
        check( rn_window_ack_probing( &f.ack, t0 + 2.40 ) );
        resent = resend_timing_pass_numbers( &f, t0 + 2.72, out );
        check( resent == 1 && out[0] == r0 );

        // the first update that newly acknowledges a number (r1, in its first
        // packet) ends the hold: the held numbers are due at once. It comes
        // more than 1 s after the last ack, so the RTT filter leaves the delay alone.

        const double release = t0 + 3.80;
        check( rn_ordered_acks( &f.ordered, &p1, 1, release ) == 1 );
        check( f.ack.last_progress == release );
        check( !rn_window_ack_probing( &f.ack, release ) );
        check( fabs( f.ack.resend_delay - 0.330 ) < 1e-9 );
        check( f.send.U == r0 );

        resent = resend_timing_pass_numbers( &f, release, out );
        check( resent == 3 );
        check( resend_timing_count( out, resent, r0 ) == 1 && resend_timing_count( out, resent, r2 ) == 1 && resend_timing_count( out, resent, r3 ) == 1 );
        check( resend_timing_count( out, resent, r1 ) == 0 );
        check( f.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );

        resend_timing_close( &f );
    }

    // opted in: no probe pacing, every due number resent as in v1.0
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        f.config.reliable_message_timeout_ms = 10000;
        resend_timing_open( &f );
        check( rn_window_ack_seed_rtt( &f.ack, 0.050 ) == 1 );

        const double t0 = resend_timing_start;
        uint64_t r[3] = { 0, 0, 0 };
        for ( int i = 0; i < 3; i++ )
        {
            resend_timing_send( &f, t0, &r[i] );
        }

        uint64_t out[64];
        const double passes[7] = { t0 + 0.34, t0 + 0.68, t0 + 1.02, t0 + 1.36, t0 + 1.70, t0 + 2.04, t0 + 2.38 };
        for ( int i = 0; i < 7; i++ )
        {
            check( !rn_window_ack_probing( &f.ack, passes[i] ) );
            const uint32_t resent = resend_timing_pass_numbers( &f, passes[i], out );
            check( resent == 3 );
            for ( int k = 0; k < 3; k++ )
            {
                check( resend_timing_count( out, resent, r[k] ) == 1 );
            }
        }
        check( f.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );

        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( test_probe_pacing_resends_only_the_oldest_without_progress );

static void test_rtt_filter_skips_packets_sent_before_an_ack_silence()
{
    // the default: the filter is on
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );

        const double t0 = resend_timing_start;
        const uint64_t pa = resend_timing_send( &f, t0, NULL );
        check( rn_ordered_acks( &f.ordered, &pa, 1, t0 + 0.100 ) == 1 );
        check( f.ack.has_rtt == 1 && fabs( f.ack.srtt - 0.100 ) < 1e-9 );

        const uint64_t pb = resend_timing_send( &f, t0 + 0.500, NULL );     // before the silence ends less srtt
        const uint64_t pc = resend_timing_send( &f, t0 + 1.950, NULL );     // after it

        // the next ack comes 1.9 s after the last: it ends a silence at t0 + 2.0,
        // and pb, sent before t0 + 2.0 - srtt = t0 + 1.9, gives no sample

        const double srtt = f.ack.srtt;
        const double rttvar = f.ack.rttvar;
        const double delay = f.ack.resend_delay;
        const double silence_end = t0 + 2.000;
        check( rn_ordered_acks( &f.ordered, &pb, 1, silence_end ) == 1 );
        check( f.ack.silence_end == silence_end );
        check( f.ack.srtt == srtt && f.ack.rttvar == rttvar && f.ack.resend_delay == delay );

        // pc, sent after it, samples (rtt 50 ms)

        check( rn_ordered_acks( &f.ordered, &pc, 1, silence_end ) == 1 );
        check( f.ack.silence_end == silence_end );
        const double rtt_c = silence_end - ( t0 + 1.950 );
        check( fabs( f.ack.rttvar - ( 0.95 * rttvar + 0.05 * fabs( srtt - rtt_c ) ) ) < 1e-12 );
        check( fabs( f.ack.srtt - ( 0.95 * srtt + 0.05 * rtt_c ) ) < 1e-12 );
        check( f.ack.srtt != srtt );

        // an ack inside the second leaves the silence where it was, and samples

        const double srtt_d = f.ack.srtt;
        const uint64_t pd = resend_timing_send( &f, silence_end + 0.100, NULL );
        check( rn_ordered_acks( &f.ordered, &pd, 1, silence_end + 0.300 ) == 1 );
        check( f.ack.silence_end == silence_end );
        check( fabs( f.ack.srtt - ( 0.95 * srtt_d + 0.05 * 0.200 ) ) < 1e-9 );

        resend_timing_close( &f );
    }

    // opted in: no silence is recorded and every acked packet samples, as in v1.0
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        f.config.reliable_message_timeout_ms = 10000;
        resend_timing_open( &f );

        const double t0 = resend_timing_start;
        const uint64_t pa = resend_timing_send( &f, t0, NULL );
        check( rn_ordered_acks( &f.ordered, &pa, 1, t0 + 0.100 ) == 1 );

        const uint64_t pb = resend_timing_send( &f, t0 + 0.500, NULL );
        const double srtt = f.ack.srtt;
        const double rttvar = f.ack.rttvar;
        check( rn_ordered_acks( &f.ordered, &pb, 1, t0 + 2.000 ) == 1 );
        check( f.ack.silence_end == 0.0 );
        const double rtt_b = ( t0 + 2.000 ) - ( t0 + 0.500 );
        check( fabs( f.ack.rttvar - ( 0.95 * rttvar + 0.05 * fabs( srtt - rtt_b ) ) ) < 1e-12 );
        check( fabs( f.ack.srtt - ( 0.95 * srtt + 0.05 * rtt_b ) ) < 1e-12 );

        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( test_rtt_filter_skips_packets_sent_before_an_ack_silence );

// A sender with one reliable message every `interval` seconds and an update
// every 10 ms. Every packet that carries a message (its first send and each
// resend) is acknowledged one round trip after it is sent: `rtt_before` for
// the first `step` messages, `rtt_after` from then on. Writes each message's
// resends to resends[] (all spurious: no packet is lost).
static void resend_timing_sparse( resend_timing_fixture_t * f, int messages, double interval, int step, double rtt_before, double rtt_after,
                                  uint32_t * resends )
{
    const double update = 0.010;
    const double t0 = resend_timing_start;
    for ( int m = 0; m < messages; m++ )
    {
        const double sent = t0 + m * interval;
        const double rtt = m < step ? rtt_before : rtt_after;

        uint64_t ids[64];
        double due[64];
        int count = 0;
        ids[count] = resend_timing_send( f, sent, NULL );
        due[count++] = sent + rtt;

        resends[m] = 0;
        for ( int u = 1; u * update < interval - 1e-9; u++ )
        {
            const double now = sent + u * update;
            for ( int i = 0; i < count; i++ )
            {
                if ( ids[i] != 0 && now >= due[i] - 1e-9 )
                {
                    rn_ordered_acks( &f->ordered, &ids[i], 1, now );
                    ids[i] = 0;
                }
            }

            const uint64_t first_id = f->next_packet_id;
            resends[m] += resend_timing_pass( f, now );
            for ( uint64_t id = first_id; id < f->next_packet_id; id++ )
            {
                check( count < 64 );
                ids[count] = id;
                due[count++] = now + rtt;
            }
        }
        check( f->send.U == f->send.next );                                 // every message acknowledged before the next
    }
}

// A message every 2 s, more than the 1 s after which an acknowledgement could
// end a silence. Nothing is outstanding between messages, so no acknowledgement
// is owed and none ends a silence: the estimate follows the path both ways
// exactly as with the filter off (opted in, v1.0's estimator).
static void test_rtt_filter_follows_a_sparse_sender()
{
    const int messages = 40;
    const int step = 10;

    // the round trip rises from 50 ms to 400 ms at message 10

    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );
        check( rn_window_ack_seed_rtt( &f.ack, 0.050 ) == 1 );
        uint32_t resends[messages];
        resend_timing_sparse( &f, messages, 2.0, step, 0.050, 0.400, resends );

        resend_timing_fixture_t v10;
        rocketnet_message_default_config( &v10.config );
        v10.config.reliable_message_timeout_ms = 10000;
        resend_timing_open( &v10 );
        check( rn_window_ack_seed_rtt( &v10.ack, 0.050 ) == 1 );
        uint32_t v10_resends[messages];
        resend_timing_sparse( &v10, messages, 2.0, step, 0.050, 0.400, v10_resends );

        // the bound: spurious resends only for the first two messages after the
        // rise (the time the estimate takes to cover 400 ms), none after; the
        // same counts and estimate as v1.0's estimator

        check( f.ack.silence_end == 0.0 );
        uint32_t total = 0;
        for ( int m = 0; m < messages; m++ )
        {
            check( resends[m] == v10_resends[m] );
            check( m < step || m >= step + 2 || resends[m] <= 2 );
            check( m < step + 2 || resends[m] == 0 );
            total += resends[m];
        }
        check( total <= 4 );
        check( f.ack.srtt == v10.ack.srtt && f.ack.resend_delay == v10.ack.resend_delay );
        check( f.ack.srtt > 0.200 && f.ack.resend_delay > 0.400 );

        resend_timing_close( &v10 );
        resend_timing_close( &f );
    }

    // the round trip falls from 400 ms to 50 ms at message 10

    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );
        check( rn_window_ack_seed_rtt( &f.ack, 0.400 ) == 1 );
        uint32_t resends[messages];
        resend_timing_sparse( &f, messages, 2.0, step, 0.400, 0.050, resends );

        resend_timing_fixture_t v10;
        rocketnet_message_default_config( &v10.config );
        v10.config.reliable_message_timeout_ms = 10000;
        resend_timing_open( &v10 );
        check( rn_window_ack_seed_rtt( &v10.ack, 0.400 ) == 1 );
        uint32_t v10_resends[messages];
        resend_timing_sparse( &v10, messages, 2.0, step, 0.400, 0.050, v10_resends );

        check( f.ack.silence_end == 0.0 );
        for ( int m = 0; m < messages; m++ )
        {
            check( resends[m] == 0 && v10_resends[m] == 0 );
        }
        check( f.ack.srtt == v10.ack.srtt && f.ack.resend_delay == v10.ack.resend_delay );
        check( f.ack.srtt < 0.200 );

        resend_timing_close( &v10 );
        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( test_rtt_filter_follows_a_sparse_sender );

// The case the filter exists for: steady traffic, a 9 s outage in both
// directions, then recovery. The packets sent in the last round trip before the
// outage reached the other side, but their acknowledgements were lost; the
// other side's first packet after the outage acknowledges them, 9 s late. They
// give no sample, so the resend delay after recovery stays at its value before
// the outage. With the filter off (opted in) the same acknowledgements inflate it.
static void test_rtt_filter_skips_the_samples_an_outage_delays()
{
    for ( int opted_in = 0; opted_in <= 1; opted_in++ )
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        f.config.reliable_message_timeout_ms = opted_in ? 30000 : 0;
        resend_timing_open( &f );
        check( rn_window_ack_seed_rtt( &f.ack, 0.100 ) == 1 );

        const double t0 = resend_timing_start;
        const double update = 0.050;
        const double rtt = 0.100;
        const double outage_start = t0 + 5.0;
        const double outage_end = outage_start + 9.0;
        const double end = outage_end + 5.0;

        uint64_t ids[4096];
        double due[4096];
        int count = 0;
        double delay_before = 0.0;
        double late_srtt = 0.0;
        double delay_after_max = 0.0;
        bool late_acked = false;

        for ( double now = t0; now < end - 1e-9; now += update )
        {
            // the first packet from the other side after the outage acknowledges
            // the ones it received just before it, then every packet sent since

            const bool late = !late_acked && now >= outage_end + rtt / 2 - 1e-9;
            for ( int i = 0; i < count; i++ )
            {
                const bool late_one = late && due[i] >= outage_start && due[i] - rtt < outage_start;
                if ( ids[i] != 0 && ( late_one || ( now >= due[i] - 1e-9 && ( due[i] < outage_start || due[i] - rtt >= outage_end ) ) ) )
                {
                    rn_ordered_acks( &f.ordered, &ids[i], 1, now );
                    ids[i] = 0;
                }
            }
            if ( late )
            {
                late_acked = true;
                late_srtt = f.ack.srtt;
            }

            // a sent packet is lost in the outage; one sent in the last round
            // trip before it is received, and only its ack is lost

            if ( now < outage_start - 1e-9 && now + update >= outage_start - 1e-9 )
            {
                delay_before = f.ack.resend_delay;
            }
            if ( now >= outage_end + 1.0 && f.ack.resend_delay > delay_after_max )
            {
                delay_after_max = f.ack.resend_delay;
            }

            static uint64_t written[4096];
            const uint64_t first_id = f.next_packet_id;
            resend_timing_write( &f, now, written, 4096 );
            check( count < 4096 );
            ids[count] = resend_timing_send( &f, now, NULL );
            due[count++] = now + rtt;
            for ( uint64_t id = first_id; id < f.next_packet_id - 1; id++ )
            {
                check( count < 4096 );
                ids[count] = id;
                due[count++] = now + rtt;
            }
        }

        check( late_acked );
        check( f.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
        check( delay_before > 0.0 && delay_before < 0.500 );
        if ( opted_in )
        {
            // the control: the late acknowledgements measure the outage
            check( f.ack.silence_end == 0.0 );
            check( late_srtt > 0.300 );
        }
        else
        {
            check( f.ack.silence_end > outage_end );
            check( late_srtt <= rtt + 1e-9 );
            check( delay_after_max <= delay_before + 0.050 );
        }

        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( test_rtt_filter_skips_the_samples_an_outage_delays );

// A connection that sent nothing for 30 s owed no acknowledgement, so the
// acknowledgement of its next message ends no silence and samples, above the
// estimate as below it.
static void test_rtt_filter_idle_connection_is_not_a_silence()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );

    const double t0 = resend_timing_start;
    const uint64_t pa = resend_timing_send( &f, t0, NULL );
    check( rn_ordered_acks( &f.ordered, &pa, 1, t0 + 0.050 ) == 1 );
    check( f.ack.has_rtt == 1 && fabs( f.ack.srtt - 0.050 ) < 1e-9 );
    check( f.send.U == f.send.next );

    // 30 s with nothing sent, then a message on a 400 ms path

    const double srtt = f.ack.srtt;
    const double rttvar = f.ack.rttvar;
    const double resume = t0 + 30.050;
    const uint64_t pb = resend_timing_send( &f, resume, NULL );
    check( rn_ordered_acks( &f.ordered, &pb, 1, resume + 0.400 ) == 1 );
    check( f.ack.silence_end == 0.0 );
    check( fabs( f.ack.rttvar - ( 0.95 * rttvar + 0.05 * fabs( srtt - 0.400 ) ) ) < 1e-12 );
    check( fabs( f.ack.srtt - ( 0.95 * srtt + 0.05 * 0.400 ) ) < 1e-12 );

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_rtt_filter_idle_connection_is_not_a_silence );

// The silence boundary: a number owed an acknowledgement for exactly
// RN_PROBE_SECONDS ends a silence when that acknowledgement arrives, and one
// owed for less does not (proofs/tla/RNRttFilter.tla EndsSilence, ">=").
// Times are exact in binary, so the boundary is met exactly.
static void test_rtt_filter_silence_starts_at_exactly_the_probe_interval()
{
    for ( int leg = 0; leg < 2; leg++ )
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );

        const double t0 = resend_timing_start;
        const double owed = leg == 0 ? RN_PROBE_SECONDS : RN_PROBE_SECONDS - 0.25;
        const uint64_t pa = resend_timing_send( &f, t0, NULL );
        check( f.send.U < f.send.next );
        check( t0 + owed - t0 == owed );
        check( rn_ordered_acks( &f.ordered, &pa, 1, t0 + owed ) == 1 );
        if ( leg == 0 )
        {
            check( f.ack.silence_end == t0 + owed );
        }
        else
        {
            check( f.ack.silence_end == 0.0 );
        }

        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( test_rtt_filter_silence_starts_at_exactly_the_probe_interval );

// The sample boundary: a packet sent exactly one srtt before the silence ended
// is sampled, and one sent a quarter second earlier is not
// (proofs/tla/RNRttFilter.tla Ack, ">=" on send time against silence_end - srtt).
// Times are exact in binary, so the boundary is met exactly.
static void test_rtt_filter_samples_a_packet_sent_exactly_one_srtt_before_the_silence_ended()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );

    const double t0 = resend_timing_start;
    const uint64_t pa = resend_timing_send( &f, t0, NULL );
    check( rn_ordered_acks( &f.ordered, &pa, 1, t0 + 0.25 ) == 1 );
    check( f.ack.srtt == 0.25 );

    // pb is owed from t0 + 0.5; its ack a probe interval later ends a silence at t0 + 1.5
    const double silence_end = t0 + 0.5 + RN_PROBE_SECONDS;
    const uint64_t pb = resend_timing_send( &f, t0 + 0.5, NULL );
    const uint64_t early = resend_timing_send( &f, silence_end - 0.5, NULL );
    const uint64_t edge = resend_timing_send( &f, silence_end - 0.25, NULL );
    check( rn_ordered_acks( &f.ordered, &pb, 1, silence_end ) == 1 );
    check( f.ack.silence_end == silence_end );
    check( f.ack.srtt == 0.25 );

    const double srtt = f.ack.srtt;
    const double rttvar = f.ack.rttvar;
    check( rn_ordered_acks( &f.ordered, &early, 1, silence_end ) == 1 );
    check( f.ack.srtt == srtt && f.ack.rttvar == rttvar );

    check( silence_end - 0.25 == f.ack.silence_end - f.ack.srtt );
    check( rn_ordered_acks( &f.ordered, &edge, 1, silence_end ) == 1 );
    check( f.ack.silence_end == silence_end );
    check( fabs( f.ack.srtt - ( 0.95 * srtt + 0.05 * 0.25 ) ) < 1e-12 );
    check( fabs( f.ack.rttvar - ( 0.95 * rttvar + 0.05 * fabs( srtt - 0.25 ) ) ) < 1e-12 );
    check( f.ack.rttvar < rttvar );                                         // the sample moved the deviation

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_rtt_filter_samples_a_packet_sent_exactly_one_srtt_before_the_silence_ended );
#endif // #if ROCKETNET_DEV

// The first acknowledgement of a connection after an outage (m9.2): the
// estimate is seeded from the transport's round trip at 50 ms, the first
// message goes out as a 3 s outage begins, and its acknowledgement arrives
// when the outage ends. A number was owed for 3 s since its first send with
// no acknowledgement, so the acknowledgement ends a silence and the packet,
// which waited it out, gives no sample. With no earlier acknowledgement the
// silence is measured from U's first send. Without a seed, the first sample
// is always taken.
static void test_rtt_filter_first_ack_after_an_outage_gives_no_sample()
{
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );
        check( rn_window_ack_seed_rtt( &f.ack, 0.050 ) == 1 );
        check( f.ack.has_last_ack == 0 );

        const double t0 = resend_timing_start;
        const double srtt = f.ack.srtt;
        const double rttvar = f.ack.rttvar;
        const uint64_t pa = resend_timing_send( &f, t0, NULL );
        const double outage_end = t0 + 3.000;
        check( rn_ordered_acks( &f.ordered, &pa, 1, outage_end ) == 1 );
        check( f.ack.srtt == srtt && f.ack.rttvar == rttvar );
        check( f.ack.silence_end == outage_end );
        check( f.send.U == f.send.next );

        resend_timing_close( &f );
    }

    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );

        const double t0 = resend_timing_start;
        const uint64_t pa = resend_timing_send( &f, t0, NULL );
        check( rn_ordered_acks( &f.ordered, &pa, 1, t0 + 3.000 ) == 1 );
        check( f.ack.has_rtt == 1 && fabs( f.ack.srtt - 3.000 ) < 1e-9 );

        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( test_rtt_filter_first_ack_after_an_outage_gives_no_sample );

// A number is sampled once (m9.1): one message is sent and resent in a second
// packet; the first packet's acknowledgement acknowledges the number and
// samples. The second copy then waits out a 3 s outage, with nothing owed, so
// no silence ends; its late acknowledgement acknowledges nothing and gives no
// sample, so the estimate does not move toward the outage.
static void test_rtt_filter_already_acknowledged_number_gives_no_sample()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );
    check( rn_window_ack_seed_rtt( &f.ack, 0.050 ) == 1 );

    const double t0 = resend_timing_start;
    uint64_t number = 0;
    const uint64_t pa = resend_timing_send( &f, t0, &number );
    const uint64_t pb = f.next_packet_id++;
    rn_window_ack_commit_packet( &f.ack, pb, &number, 1, t0 + 0.010 );

    check( rn_ordered_acks( &f.ordered, &pa, 1, t0 + 0.050 ) == 1 );
    check( f.send.U == f.send.next );
    const double srtt = f.ack.srtt;
    const double rttvar = f.ack.rttvar;
    const double delay = f.ack.resend_delay;
    check( fabs( srtt - 0.050 ) < 1e-9 );

    check( rn_ordered_acks( &f.ordered, &pb, 1, t0 + 3.010 ) == 1 );
    check( f.ack.silence_end == 0.0 );
    check( f.ack.srtt == srtt && f.ack.rttvar == rttvar && f.ack.resend_delay == delay );

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_rtt_filter_already_acknowledged_number_gives_no_sample );

// A sent_index with every cell used cannot happen (2P cells hold at most P
// entries), so it means the index memory was corrupted. Release builds
// compile the asserts out: the backward-shift scan that removes an acked
// packet is bounded by the table size and ends the connection through
// ack->error instead of probing forever.
static void test_sent_index_full_scan_is_bounded()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );

    const double t0 = resend_timing_start;
    const uint64_t packet_id = resend_timing_send( &f, t0, NULL );

    // white box: fill every empty cell with a packet id no ack will name

    rn_region_t * region = f.region;
    const uint32_t cap = region->sent_index_entries;
    check( cap > 0 );
    for ( uint32_t i = 0; i < cap; i++ )
    {
        rn_sent_index_t * cell = &region->sent_index[i];
        if ( cell->used )
        {
            continue;
        }
        cell->packet_id = 0x8000000000000000ULL + i;
        cell->entry = 0;
        cell->used = 1;
    }

    check( f.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rn_window_ack_on_ack( &f.ack, packet_id, t0 + 0.020 ) == 1 );
    check( f.ack.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( test_sent_index_full_scan_is_bounded );

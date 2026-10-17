/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The resend delay of a reliable record (docs/CHANNELS.md 6.4 "RTO" and
// "Resend"; tla/RNResendTiming.tla): every resend waits
// min( rto, ROCKETNET_MESSAGE_RESEND_INTERVAL_MS ), with no back-off, and
// rto = clamp( srtt x ROCKETNET_MESSAGE_RTO_SCALE_PERCENT / 100 + 2 rttvar,
// rto_min, rto_max ),
// rto_initial before the first sample. The validation refuses a config that
// fits fewer than ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS sends before
// reliable_timeout.
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
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_window_send.h"
#include "sdk/rocketnet_window_ack.h"

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
    rn_window_send_t send;
    rn_window_ack_t ack;
    uint64_t next_packet_id;
};

static void resend_timing_open( resend_timing_fixture_t * f )
{
    check( rocketnet_message_validate_config( &f->config ) );
    f->arena = rn_arena_create( NULL, &f->config, 1, NULL );
    check( f->arena != NULL );
    f->region = rn_arena_region( f->arena, 0 );
    rn_region_carve( f->region );
    rn_window_send_init( &f->send, f->region, 0 );
    rn_window_ack_init( &f->ack, &f->send, f->region, &f->config );
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
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &f->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    const uint64_t packet_id = f->next_packet_id++;
    rn_window_ack_commit_packet( &f->ack, packet_id, &number, 1, now );
    if ( out_number )
        *out_number = number;
    return packet_id;
}

// One resend pass at now; returns how many numbers it resent.
static uint32_t resend_timing_pass( resend_timing_fixture_t * f, double now )
{
    uint64_t out[16];
    return rn_window_ack_resend( &f->ack, now, &f->next_packet_id, out, 16 );
}

// resend_delay_is_min_of_interval_and_rtt: on a short path the delay is the
// RTO, 1.25 srtt + 2 rttvar; on a long path the RTO clamps at rto_max and the
// 100 ms resend interval is the delay.
static void resend_delay_is_min_of_interval_and_rtt()
{
    // short path: one sample of 20 ms gives rto = 25 + 20 = 45 ms, under the cap
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );
        const double t0 = resend_timing_start;
        resend_timing_send( &f, t0, NULL );
        const uint64_t other = resend_timing_send( &f, t0, NULL );
        check( rn_window_ack_on_ack( &f.ack, other, t0 + 0.020 ) == 1 );
        check( f.ack.has_rtt == 1 );
        check( fabs( f.ack.rto - 0.045 ) < 0.0005 );
        check( resend_timing_pass( &f, t0 + 0.040 ) == 0 );     // not before the RTO
        check( resend_timing_pass( &f, t0 + 0.046 ) == 1 );     // resent at it, well before 100 ms
        resend_timing_close( &f );
    }

    // long path: one sample of 400 ms gives 500 + 400 ms, clamped to rto_max
    // 250 ms; the delay is the 100 ms interval
    {
        resend_timing_fixture_t f;
        rocketnet_message_default_config( &f.config );
        resend_timing_open( &f );
        const double t0 = resend_timing_start;
        resend_timing_send( &f, t0, NULL );
        const uint64_t other = resend_timing_send( &f, t0, NULL );
        check( rn_window_ack_on_ack( &f.ack, other, t0 + 0.400 ) == 1 );
        check( fabs( f.ack.rto - 0.250 ) < 0.0005 );
        // the record went at t0 and is past its interval at the sample: due now
        check( resend_timing_pass( &f, t0 + 0.400 ) == 1 );
        check( resend_timing_pass( &f, t0 + 0.400 + 0.099 ) == 0 );
        check( resend_timing_pass( &f, t0 + 0.400 + 0.101 ) == 1 );
        resend_timing_close( &f );
    }
}

REGISTER_UNIT_TEST( resend_delay_is_min_of_interval_and_rtt );

// no_backoff_between_resends: a record that is never acked is resent at the
// same delay every time. With rto_initial at 100 ms and no sample, a 60 Hz
// update resends it every 100 ms (to the next tick) for as long as it is
// unacked; the first version doubled the delay after each pass.
static void no_backoff_between_resends()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.rto_initial_ms = 100;
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
    check( f.ack.error == ROCKETNET_MESSAGE_ERROR_NONE );
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( no_backoff_between_resends );

// first_resend_before_any_sample_at_rto_initial: a fresh connection has no RTT
// sample, so the delay is min( rto_initial, resend_interval ), 100 ms at the
// defaults, not the first version's one second.
static void first_resend_before_any_sample_at_rto_initial()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    resend_timing_open( &f );
    check( f.ack.has_rtt == 0 );
    check( f.ack.rto == f.ack.rto_initial );
    check( fabs( f.ack.rto_initial - 0.100 ) < 1e-9 );

    const double t0 = resend_timing_start;
    resend_timing_send( &f, t0, NULL );
    check( resend_timing_pass( &f, t0 + 0.099 ) == 0 );
    check( resend_timing_pass( &f, t0 + 0.1001 ) == 1 );
    check( f.ack.has_rtt == 0 );
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( first_resend_before_any_sample_at_rto_initial );

// config_refuses_fewer_than_four_attempts: reliable_timeout_ms must fit four
// sends of the longest first delay, max( the 100 ms resend interval,
// rto_initial_ms ).
// Validation refuses and never clamps.
static void config_refuses_fewer_than_four_attempts()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    check( rocketnet_message_validate_config( &config ) );

    // rto_initial 2 s and a 5 s timeout: two and a half first delays
    rocketnet_message_default_config( &config );
    config.rto_min_ms = 10;
    config.rto_initial_ms = 2000;
    config.rto_max_ms = 2000;
    config.reliable_timeout_ms = 5000;
    check( !rocketnet_message_validate_config( &config ) );
    check( rocketnet_message_connection_bytes( &config ) == 0 );

    // one millisecond short of four is refused; exactly four is accepted
    config.reliable_timeout_ms = 7999;
    check( !rocketnet_message_validate_config( &config ) );
    config.reliable_timeout_ms = 8000;
    check( rocketnet_message_validate_config( &config ) );

    // the config is not repaired by validation
    check( config.rto_initial_ms == 2000 && config.reliable_timeout_ms == 8000 );
}

REGISTER_UNIT_TEST( config_refuses_fewer_than_four_attempts );

// resend_timing_witness_replays: the schedule of
// tla/MCRNResendTimingBrokenBackoff.cfg against the code. The first version's
// RTO settings (rto_initial 1000 ms, rto_min 50 ms, rto_max 2000 ms,
// reliable_timeout 10 s), one RTT sample of 500 ms (which drives the first
// version's rto to exactly 1.5 s), and the record's own sends all lost. The
// witness fits three sends before the timeout (0, 1.5 and 4.5 s); the code
// must make at least ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS.
static void resend_timing_witness_replays()
{
    resend_timing_fixture_t f;
    rocketnet_message_default_config( &f.config );
    f.config.rto_initial_ms = 1000;
    f.config.rto_min_ms = 50;
    f.config.rto_max_ms = 2000;
    f.config.reliable_timeout_ms = 10000;
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
        const uint32_t resent = rn_window_ack_resend( &f.ack, now, &f.next_packet_id, out, 4 );
        for ( uint32_t k = 0; k < resent; k++ )
            if ( out[k] == record )
                attempts++;
    }

    printf( "resend_timing_witness_replays attempts=%d before reliable_timeout\n", attempts );
    check( sampled );
    check( timed_out );                                            // every send lost: the timeout ends it
    check( f.ack.error == ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT );
    check( attempts >= 4 );                                        // ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS
    resend_timing_close( &f );
}

REGISTER_UNIT_TEST( resend_timing_witness_replays );

// message_config_abi_size_is_unchanged: the ABI canary. The resend schedule's
// interval and RTT scale are constants, so the public message config keeps
// the layout ABI1 callers allocate: 15 int fields, as at BASE. A field added
// to ROCKETNET_MESSAGE_CONFIG_TABLE grows this and fails here first.
static void message_config_abi_size_is_unchanged()
{
    check( sizeof( rocketnet_message_config_t ) == ROCKETNET_MESSAGE_CONFIG_ABI1_BYTES );
    check( offsetof( rocketnet_message_config_t, reliable_timeout_ms ) == ROCKETNET_MESSAGE_CONFIG_ABI1_BYTES - sizeof( int ) );
}

REGISTER_UNIT_TEST( message_config_abi_size_is_unchanged );

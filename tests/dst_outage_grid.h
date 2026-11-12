/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    `./bin/dst --outage <set> [--seeds a-b]` (docs/DST.md, "Outage
    scenarios"): the outage world of tests/dst_outage.h over a grid of rows.
    Each seed moves the outage's start by a fiftieth of a second, so a seed
    range spreads it over the keep-alive and offer phases. One line per row
    and seed, then `PASS outage=<set> rows=N` or `FAIL outage=<set> rows=N
    failed=F` with exit 1. No wall clock, no socket, no thread.
*/

#pragma once

#ifndef ROCKETNET_TESTS_DST_OUTAGE_GRID_H
#define ROCKETNET_TESTS_DST_OUTAGE_GRID_H

#include "dst_outage.h"

#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

static const char * dst_outage_usage =
    "usage: dst --outage grid|complement|handshake|opted-in|refusal|largest|all [--seeds a-b]\n";

struct dst_outage_tally_t
{
    uint64_t rows;
    uint64_t failed;
};

static inline void dst_outage_row( dst_outage_tally_t * tally, const char * label, dst_outage_options_t * o, uint64_t seed )
{
    // a handshake row is armed by a state change, and keeps it exact
    o->phase_seconds = o->start == DST_OUTAGE_START_CONNECTED ? (double) ( ( seed - 1 ) % 50u ) * 0.02 : 0.0;
    dst_outage_result_t result;
    if ( !dst_outage_run( o, &result ) )
    {
        result.passed = 0;
        snprintf( result.failure, sizeof( result.failure ), "the world could not be built" );
    }
    char text[64];
    snprintf( text, sizeof( text ), "%s seed=%llu", label, (unsigned long long) seed );
    dst_outage_print( text, o, &result );
    fflush( stdout );
    tally->rows++;
    if ( !result.passed )
    {
        tally->failed++;
    }
}

static const int dst_outage_directions[3] = { DST_OUTAGE_C2S, DST_OUTAGE_S2C, DST_OUTAGE_BOTH };

// The owner's three tests at T - 1 for T = 2, 10, 30 and 60, at round trips
// 1, 150, 300, 600 and 900 ms, with 0 and 2% residual loss.

static inline void dst_outage_set_grid( dst_outage_tally_t * tally, struct rocketnet_loopback_hub_t * hub, uint64_t seed )
{
    static const int timeouts[4] = { 2, 10, 30, 60 };
    static const int rtts[5] = { 1, 150, 300, 600, 900 };
    for ( int t = 0; t < 4; t++ )
    {
        for ( int r = 0; r < 5; r++ )
        {
            for ( int loss = 0; loss <= 2; loss += 2 )
            {
                for ( int d = 0; d < 3; d++ )
                {
                    dst_outage_options_t o;
                    dst_outage_default_options( &o, hub );
                    o.timeout_seconds = timeouts[t];
                    o.outage_seconds = (double) ( timeouts[t] - 1 );
                    o.rtt_ms = rtts[r];
                    o.loss_percent = loss;
                    o.dark = dst_outage_directions[d];
                    dst_outage_row( tally, "grid", &o, seed );
                }
            }
        }
    }
}

// The complement: T + 2 s ends both sides by the timeout and nothing else.

static inline void dst_outage_set_complement( dst_outage_tally_t * tally, struct rocketnet_loopback_hub_t * hub, uint64_t seed )
{
    static const int timeouts[4] = { 2, 10, 30, 60 };
    static const int rtts[3] = { 1, 150, 600 };
    for ( int t = 0; t < 4; t++ )
    {
        for ( int r = 0; r < 3; r++ )
        {
            for ( int d = 0; d < 3; d++ )
            {
                dst_outage_options_t o;
                dst_outage_default_options( &o, hub );
                o.timeout_seconds = timeouts[t];
                o.outage_seconds = (double) timeouts[t] + 2.0;
                o.rtt_ms = rtts[r];
                o.dark = dst_outage_directions[d];
                o.expect = DST_OUTAGE_EXPECT_TIMEOUT;
                dst_outage_row( tally, "complement", &o, seed );
            }
        }
    }
}

// An outage of T - 1 starting at each stage of the handshake, a 30 s token,
// T = 10, 30 and 60, round trips 150, 600 and 900 ms. State 3 with the
// client's direction dark fails the attempt with CONNECTION_RESPONSE_TIMED_OUT
// at T (the server's 5 s handshake window closes first: USAGE.md and the
// design's establishment table); every other row survives.

static inline void dst_outage_set_handshake( dst_outage_tally_t * tally, struct rocketnet_loopback_hub_t * hub, uint64_t seed )
{
    static const int timeouts[3] = { 10, 30, 60 };
    static const int rtts[3] = { 150, 600, 900 };
    static const int starts[6] = { DST_OUTAGE_START_STATE_1,          DST_OUTAGE_START_STATE_2,          DST_OUTAGE_START_STATE_3,
                                   DST_OUTAGE_START_TRUE_X1,          DST_OUTAGE_START_SERVER_CONNECTED, DST_OUTAGE_START_CLIENT_CONNECTED };
    for ( int t = 0; t < 3; t++ )
    {
        for ( int r = 0; r < 3; r++ )
        {
            for ( int s = 0; s < 6; s++ )
            {
                for ( int d = 0; d < 3; d++ )
                {
                    dst_outage_options_t o;
                    dst_outage_default_options( &o, hub );
                    o.timeout_seconds = timeouts[t];
                    o.outage_seconds = (double) ( timeouts[t] - 1 );
                    o.token_lifetime_seconds = 30;
                    o.rtt_ms = rtts[r];
                    o.start = starts[s];
                    o.dark = dst_outage_directions[d];
                    o.large_bytes = 0;
                    if ( starts[s] == DST_OUTAGE_START_STATE_3 && ( o.dark & DST_OUTAGE_C2S ) != 0 )
                    {
                        o.expect = DST_OUTAGE_EXPECT_RESPONSE_TIMEOUT;
                    }
                    // states 1 and 2 survive when the outage, a round trip and
                    // a keep-alive fit inside both T and the connect clock (the
                    // design measured T = 10 and T = 30 at 150 ms); otherwise
                    // the attempt may fail as those states allow
                    const int early = starts[s] == DST_OUTAGE_START_STATE_1 || starts[s] == DST_OUTAGE_START_STATE_2;
                    if ( early && rtts[r] > 150 )
                    {
                        o.expect = DST_OUTAGE_EXPECT_CONNECT_OR_RETRY;
                    }
                    if ( early && timeouts[t] >= 30 )
                    {
                        o.expect = DST_OUTAGE_EXPECT_CONNECT_OR_RETRY;
                    }
                    dst_outage_row( tally, "handshake", &o, seed );
                }
            }
        }
    }
}

// Opted in: the reliable timeout ends the connection at the configured value
// from the oldest message's first send, outage or not.

static inline void dst_outage_set_opted_in( dst_outage_tally_t * tally, struct rocketnet_loopback_hub_t * hub, uint64_t seed )
{
    static const int timeouts[3] = { 10, 30, 60 };
    static const int rtts[3] = { 1, 150, 600 };
    for ( int t = 0; t < 3; t++ )
    {
        for ( int r = 0; r < 3; r++ )
        {
            for ( int d = 0; d < 3; d++ )
            {
                dst_outage_options_t o;
                dst_outage_default_options( &o, hub );
                o.timeout_seconds = timeouts[t];
                o.outage_seconds = (double) ( timeouts[t] - 1 );
                o.rtt_ms = rtts[r];
                o.dark = dst_outage_directions[d];
                // half of T, but at least the 8 s the config accepts (four
                // initial resend delays of 2 s)
                o.reliable_timeout_ms = timeouts[t] * 500 > 8000 ? timeouts[t] * 500 : 8000;
                o.expect = DST_OUTAGE_EXPECT_RELIABLE_TIMEOUT;
                dst_outage_row( tally, "opted-in", &o, seed );
            }
        }
    }
}

// T = 60, D = 59 with the default buffers: the send side fills and starts
// are refused, the connection stays, and nothing accepted is lost.

static inline void dst_outage_set_refusal( dst_outage_tally_t * tally, struct rocketnet_loopback_hub_t * hub, uint64_t seed )
{
    for ( int d = 0; d < 3; d++ )
    {
        dst_outage_options_t o;
        dst_outage_default_options( &o, hub );
        o.timeout_seconds = 60;
        o.outage_seconds = 59.0;
        o.rtt_ms = 150;
        o.dark = dst_outage_directions[d];
        o.offer_after_seconds = 20.0;
        dst_outage_row( tally, "refusal", &o, seed );
    }
}

// The largest T the server takes (max_timeout_seconds 3600), both dark for
// T - 1 at a 150 ms round trip.

static inline void dst_outage_set_largest( dst_outage_tally_t * tally, struct rocketnet_loopback_hub_t * hub, uint64_t seed )
{
    dst_outage_options_t o;
    dst_outage_default_options( &o, hub );
    o.timeout_seconds = 3600;
    o.outage_seconds = 3599.0;
    o.rtt_ms = 150;
    o.dark = DST_OUTAGE_BOTH;
    o.offer_after_seconds = 20.0;
    dst_outage_row( tally, "largest", &o, seed );
}

static inline int dst_outage_main( int argc, char ** argv )
{
    if ( argc < 3 )
    {
        fprintf( stderr, "%s", dst_outage_usage );
        return 2;
    }
    const char * set = argv[2];
    uint64_t seeds_a = 1;
    uint64_t seeds_b = 1;
    for ( int i = 3; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--seeds" ) == 0 && i + 1 < argc )
        {
            char * a_end = NULL;
            char * b_end = NULL;
            const char * range = argv[++i];
            const unsigned long long a = strtoull( range, &a_end, 10 );
            if ( a_end == range || *a_end != '-' )
            {
                fprintf( stderr, "dst: --seeds needs a-b with 1 <= a <= b\n" );
                return 2;
            }
            const unsigned long long b = strtoull( a_end + 1, &b_end, 10 );
            if ( b_end == a_end + 1 || a < 1 || a > b )
            {
                fprintf( stderr, "dst: --seeds needs a-b with 1 <= a <= b\n" );
                return 2;
            }
            seeds_a = a;
            seeds_b = b;
        }
        else
        {
            fprintf( stderr, "dst: unknown or malformed argument: %s\n%s", argv[i], dst_outage_usage );
            return 2;
        }
    }

    const int all = strcmp( set, "all" ) == 0;
    const int grid = all || strcmp( set, "grid" ) == 0;
    const int complement = all || strcmp( set, "complement" ) == 0;
    const int handshake = all || strcmp( set, "handshake" ) == 0;
    const int opted_in = all || strcmp( set, "opted-in" ) == 0;
    const int refusal = all || strcmp( set, "refusal" ) == 0;
    const int largest = all || strcmp( set, "largest" ) == 0;
    if ( !grid && !complement && !handshake && !opted_in && !refusal && !largest )
    {
        fprintf( stderr, "dst: --outage names a set that does not exist\n%s", dst_outage_usage );
        return 2;
    }

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        return 1;
    }
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_quiet( 1 );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 1, &status );
    if ( !hub )
    {
        fprintf( stderr, "dst: the loopback hub was refused: %s\n", rocketnet_status_name( status ) );
        rocketnet_term();
        return 1;
    }

    dst_outage_tally_t tally;
    memset( &tally, 0, sizeof( tally ) );
    for ( uint64_t seed = seeds_a; seed <= seeds_b; seed++ )
    {
        if ( grid )
        {
            dst_outage_set_grid( &tally, hub, seed );
        }
        if ( complement )
        {
            dst_outage_set_complement( &tally, hub, seed );
        }
        if ( handshake )
        {
            dst_outage_set_handshake( &tally, hub, seed );
        }
        if ( opted_in )
        {
            dst_outage_set_opted_in( &tally, hub, seed );
        }
        if ( refusal )
        {
            dst_outage_set_refusal( &tally, hub, seed );
        }
        if ( largest )
        {
            dst_outage_set_largest( &tally, hub, seed );
        }
        if ( seed == UINT64_MAX )
        {
            break;
        }
    }

    const bool destroyed = rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK;
    rocketnet_term();
    if ( tally.failed || !destroyed )
    {
        printf( "FAIL outage=%s seeds=%llu-%llu rows=%llu failed=%llu\n", set, (unsigned long long) seeds_a, (unsigned long long) seeds_b,
                (unsigned long long) tally.rows, (unsigned long long) tally.failed );
        return 1;
    }
    printf( "PASS outage=%s seeds=%llu-%llu rows=%llu\n", set, (unsigned long long) seeds_a, (unsigned long long) seeds_b,
            (unsigned long long) tally.rows );
    return 0;
}

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_TESTS_DST_OUTAGE_GRID_H

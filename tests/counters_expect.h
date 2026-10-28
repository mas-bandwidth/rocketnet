/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The exact-delta helper and the conservation assert, for every suite that
// owns a counters block.
//
//   rocketnet_counters_expect( before, after, { { ROCKETNET_COUNTER_X, 1 }, ... } );
//
// passes when each named counter moved by exactly its delta and every counter
// not named stayed where it was. "Nonzero" passes when one of ten thousand bad
// packets was counted; an exact delta with everything else at zero does not.
// On failure it prints every nonzero delta by name, so the stray move is in
// the failure output without a debugger.
//
//   rocketnet_counters_assert_conserved( counters );
//
// passes when every counter family balances (rocketnet_counters_conserved) and
// prints the first unbalanced family when it does not.
//
// The suites check differently (rocketnet_check in the sdk suite, the
// registry's check in the unit files, the harness's in the functional tier),
// so the including file names its check before the include:
//
//   #define ROCKETNET_COUNTERS_CHECK rocketnet_check
//   #include "tests/counters_expect.h"
//
// The core (rocketnet_counters_expect_core) does not abort: it returns false
// and writes the report, and the asserting forms wrap it.

#pragma once

#ifndef ROCKETNET_TESTS_COUNTERS_EXPECT_H
#define ROCKETNET_TESTS_COUNTERS_EXPECT_H

#ifndef ROCKETNET_COUNTERS_CHECK
#error "name the suite's check before including counters_expect.h: #define ROCKETNET_COUNTERS_CHECK <check>"
#endif // #ifndef ROCKETNET_COUNTERS_CHECK

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <string>

// one named move: a counter and the exact amount it moved. The constructor
// takes any integer type so a caller can pass a count it computed without a
// narrowing error in the braced list.

struct rocketnet_counter_delta_t
{
    int counter;
    uint64_t delta;
    template <typename T> rocketnet_counter_delta_t( int counter_in, T delta_in ) : counter( counter_in ), delta( (uint64_t) delta_in ) {}
};

// true when the named counters moved by exactly their deltas and nothing else
// moved. otherwise false, with report holding one line per counter that is
// named or moved: its expected and actual delta, and a mark on a counter that
// moved without being named. report may be NULL.

inline bool rocketnet_counters_expect_core( const struct rocketnet_counters_t * before,
                                            const struct rocketnet_counters_t * after,
                                            std::initializer_list<rocketnet_counter_delta_t> expected,
                                            char * report, int report_bytes )
{
    uint64_t named[ROCKETNET_NUM_COUNTERS];
    bool is_named[ROCKETNET_NUM_COUNTERS];
    memset( named, 0, sizeof( named ) );
    memset( is_named, 0, sizeof( is_named ) );

    std::string lines;
    bool ok = true;

    for ( const rocketnet_counter_delta_t * e = expected.begin(); e != expected.end(); ++e )
    {
        if ( e->counter < 0 || e->counter >= ROCKETNET_NUM_COUNTERS )
        {
            lines += "  a delta names an index outside the counter list\n";
            ok = false;
            continue;
        }

        if ( is_named[e->counter] )
        {
            lines += std::string( "  " ) + rocketnet_counter_name( e->counter ) + " is named twice\n";
            ok = false;
        }

        is_named[e->counter] = true;
        named[e->counter] += e->delta;
    }

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const uint64_t was = before->values[i];
        const uint64_t now = after->values[i];
        const bool moved = now != was;
        if ( !moved && !is_named[i] )
            continue;

        const bool backwards = now < was;
        const uint64_t actual = backwards ? was - now : now - was;
        const bool matches = is_named[i] ? ( !backwards && actual == named[i] ) : !moved;
        if ( !matches )
        {
            ok = false;
        }

        char line[256];
        if ( is_named[i] )
            snprintf( line, sizeof( line ), "  %s: expected +%llu, got %s%llu%s\n", rocketnet_counter_name( i ),
                      (unsigned long long) named[i], backwards ? "-" : "+", (unsigned long long) actual,
                      matches ? "" : "  <-- wrong delta" );
        else
            snprintf( line, sizeof( line ), "  %s: moved %s%llu and was not named  <-- unnamed move\n", rocketnet_counter_name( i ),
                      backwards ? "-" : "+", (unsigned long long) actual );
        lines += line;
    }

    if ( !ok && report && report_bytes > 0 )
        snprintf( report, (size_t) report_bytes, "counters did not move as named:\n%s", lines.c_str() );
    else if ( report && report_bytes > 0 )
        report[0] = '\0';

    return ok;
}

#define rocketnet_counters_expect( before, after, ... )                                                           \
do                                                                                                                \
{                                                                                                                 \
    char rocketnet_counters_report[16384];                                                                        \
    const bool rocketnet_counters_moved_as_named =                                                                \
        rocketnet_counters_expect_core( (before), (after), __VA_ARGS__,                                           \
                                        rocketnet_counters_report, (int) sizeof(rocketnet_counters_report) );     \
    if ( !rocketnet_counters_moved_as_named )                                                                     \
    {                                                                                                             \
        printf( "%s", rocketnet_counters_report );                                                                \
        fflush( stdout );                                                                                         \
    }                                                                                                             \
    ROCKETNET_COUNTERS_CHECK( rocketnet_counters_moved_as_named );                                                \
} while(0)

#define rocketnet_counters_assert_conserved( counters )                                                           \
do                                                                                                                \
{                                                                                                                 \
    char rocketnet_counters_report[256];                                                                          \
    const int rocketnet_counters_balanced =                                                                       \
        rocketnet_counters_conserved( (counters), rocketnet_counters_report, (int) sizeof(rocketnet_counters_report) ); \
    if ( !rocketnet_counters_balanced )                                                                           \
    {                                                                                                             \
        printf( "counters not conserved: %s\n", rocketnet_counters_report );                                      \
        fflush( stdout );                                                                                         \
    }                                                                                                             \
    ROCKETNET_COUNTERS_CHECK( rocketnet_counters_balanced );                                                      \
} while(0)

// The clean-run rule: every REFUSAL counter reads 0 on a clean path. Returns
// the number of refusal counters that moved, and writes one line per counter
// into report (NULL for none). A caller whose run injects a fault lists the
// refusals the fault explains in allowed (terminated by -1); a refusal not
// listed that moved is a failure.

inline int rocketnet_counters_refusals( const struct rocketnet_counters_t * counters, const int * allowed,
                                        char * report, int report_bytes )
{
    std::string lines;
    int moved = 0;

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        if ( rocketnet_counter_class( i ) != ROCKETNET_COUNTER_CLASS_REFUSAL || counters->values[i] == 0 )
            continue;

        bool explained = false;
        for ( int j = 0; allowed && allowed[j] != -1; j++ )
            if ( allowed[j] == i )
                explained = true;
        if ( explained )
            continue;

        char line[128];
        snprintf( line, sizeof( line ), "  %s = %llu\n", rocketnet_counter_name( i ), (unsigned long long) counters->values[i] );
        lines += line;
        moved++;
    }

    if ( report && report_bytes > 0 )
    {
        snprintf( report, (size_t) report_bytes, "%s", moved ? ( std::string( "refusals moved on a run that should be clean:\n" ) + lines ).c_str() : "" );
    }

    return moved;
}

#define ROCKETNET_COUNTERS_ASSERT_CLEAN( counters, allowed )                                                      \
do                                                                                                                \
{                                                                                                                 \
    char rocketnet_counters_report[4096];                                                                         \
    const int rocketnet_counters_refusals_moved =                                                                 \
        rocketnet_counters_refusals( (counters), (allowed), rocketnet_counters_report,                            \
                                     (int) sizeof(rocketnet_counters_report) );                                   \
    if ( rocketnet_counters_refusals_moved )                                                                      \
    {                                                                                                             \
        printf( "%s", rocketnet_counters_report );                                                                \
        fflush( stdout );                                                                                         \
    }                                                                                                             \
    ROCKETNET_COUNTERS_CHECK( rocketnet_counters_refusals_moved == 0 );                                           \
} while(0)

// every refusal reads 0
#define rocketnet_counters_assert_clean( counters )     ROCKETNET_COUNTERS_ASSERT_CLEAN( (counters), NULL )

// every refusal reads 0 but the listed ones, which the run's injected fault
// explains: rocketnet_counters_assert_clean_except( counters, ROCKETNET_COUNTER_A, ROCKETNET_COUNTER_B )
#define rocketnet_counters_assert_clean_except( counters, ... )                                                   \
do                                                                                                                \
{                                                                                                                 \
    static const int rocketnet_counters_allowed[] = { __VA_ARGS__, -1 };                                          \
    ROCKETNET_COUNTERS_ASSERT_CLEAN( (counters), rocketnet_counters_allowed );                                    \
} while(0)

#endif // ROCKETNET_TESTS_COUNTERS_EXPECT_H

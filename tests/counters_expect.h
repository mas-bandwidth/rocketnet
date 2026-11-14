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
#include "rocketnet_receive_prefilter.h"

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
        {
            continue;
        }

        const bool backwards = now < was;
        const uint64_t actual = backwards ? was - now : now - was;
        const bool matches = is_named[i] ? ( !backwards && actual == named[i] ) : !moved;
        if ( !matches )
        {
            ok = false;
        }

        char line[256];
        if ( is_named[i] )
        {
            snprintf( line, sizeof( line ), "  %s: expected +%llu, got %s%llu%s\n", rocketnet_counter_name( i ),
                      (unsigned long long) named[i], backwards ? "-" : "+", (unsigned long long) actual,
                      matches ? "" : "  <-- wrong delta" );
        }
        else
        {
            snprintf( line, sizeof( line ), "  %s: moved %s%llu and was not named  <-- unnamed move\n", rocketnet_counter_name( i ),
                      backwards ? "-" : "+", (unsigned long long) actual );
        }
        lines += line;
    }

    if ( !ok && report && report_bytes > 0 )
    {
        snprintf( report, (size_t) report_bytes, "counters did not move as named:\n%s", lines.c_str() );
    }
    else if ( report && report_bytes > 0 )
    {
        report[0] = '\0';
    }

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

// Whether a transport row counts the same events as a shared row
// (standard/TRANSPORT.md t18, the aliasing table): the shared row is the
// transport-blind total, so a walk over every refusal row reads the shared
// row and leaves these out, or it counts one event twice. IOCP_RECEIVE_DROPPED
// is not one of them: it also counts full-bank and failed receives, which
// iocp does not yet feed to a shared row.

inline bool rocketnet_counter_aliases_shared_row( int counter )
{
    switch ( counter )
    {
        case ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_SIZE:
        case ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SIZE:
        case ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_SHORT:
        case ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_LARGE:
        case ROCKETNET_COUNTER_RIO_RECEIVE_DROPPED_SIZE:
        case ROCKETNET_COUNTER_RIO_RECEIVE_TOO_SHORT:
        case ROCKETNET_COUNTER_RIO_RECEIVE_TOO_LARGE:
        case ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW:
        case ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_SOURCE:
        case ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SOURCE:
        case ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_NO_BUFFER:
        case ROCKETNET_COUNTER_RIO_RECEIVE_FAILED:
        case ROCKETNET_COUNTER_GSO_SEND_DROPPED:
        case ROCKETNET_COUNTER_URING_SEND_DROPPED:
        case ROCKETNET_COUNTER_IOCP_SEND_FAILED:
        case ROCKETNET_COUNTER_RIO_SEND_FAILED:
        case ROCKETNET_COUNTER_RIO_SEND_FULL:
            return true;
        default:
            return false;
    }
}

// The part of a refusal row's value that no shared row also counts, so a walk
// over every refusal row counts each event once: 0 for a row that aliases a
// shared row; for GSO_RECEIVE_DROPPED_OVERFLOW, which counts the full-bank
// drops (RECEIVE_DROPPED_FULL also counts them) and the kernel's drops at a
// full gso queue socket (SO_RXQ_OVFL, which no shared row counts), its excess
// over RECEIVE_DROPPED_FULL; the whole value for every other row.

inline uint64_t rocketnet_counter_unshared_value( const struct rocketnet_counters_t * counters, int counter )
{
    const uint64_t value = counters->values[counter];
    if ( counter == ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW )
    {
        const uint64_t full = counters->values[ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL];
        return value > full ? value - full : 0;
    }
    return rocketnet_counter_aliases_shared_row( counter ) ? 0 : value;
}

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

    // COOKIE_REPLY_WITHHELD on a clean run: an honest client's cookieless
    // requests (it sends again at its send rate) can land in one server
    // update when the receive worker banks them late, and the per-update
    // limit answers the first and withholds the copy (t12 rule 7). That is
    // the limit working, so it is explained when a cookie reply did go out:
    // each withheld request is also counted by its cookie reason, so the
    // reasons less the withheld count are the requests answered. An update
    // late by k send intervals banks about k copies of one request, so the
    // number of copies per answered request has no fixed bound; withheld
    // with none answered is still a failure. A bitmap that is never cleared
    // is caught by the handshake budget unit tests (exact WITHHELD values
    // across updates) and by the connect that then times out.

    const uint64_t withheld = counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD];
    const uint64_t cookie_reasons = counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] +
                                    counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] +
                                    counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED];
    const bool withheld_copies_of_answered = withheld > 0 && cookie_reasons > withheld;

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        if ( rocketnet_counter_class( i ) != ROCKETNET_COUNTER_CLASS_REFUSAL || counters->values[i] == 0 )
        {
            continue;
        }

        // a row a shared row also counts is read there; GSO_RECEIVE_DROPPED_OVERFLOW
        // is a refusal only past RECEIVE_DROPPED_FULL (rocketnet_counter_unshared_value)
        const uint64_t value = rocketnet_counter_unshared_value( counters, i );
        if ( value == 0 )
        {
            continue;
        }

        if ( i == ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD && withheld_copies_of_answered )
        {
            continue;
        }

        bool explained = false;
        for ( int j = 0; allowed && allowed[j] != -1; j++ )
        {
            if ( allowed[j] == i )
            {
                explained = true;
            }
        }
        if ( explained )
        {
            continue;
        }

        char line[128];
        snprintf( line, sizeof( line ), "  %s = %llu%s\n", rocketnet_counter_name( i ), (unsigned long long) value,
                  value != counters->values[i] ? " (past RECEIVE_DROPPED_FULL)" : "" );
        lines += line;
        moved++;
    }

    if ( report && report_bytes > 0 )
    {
#if ROCKETNET_DEV
        // the first refused datagrams (sdk/rocketnet_receive_prefilter.h), so a
        // refusal that should not have moved names its source in the log
        if ( moved )
        {
            lines += rocketnet_receive_drop_samples_text();
        }
#endif // #if ROCKETNET_DEV
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

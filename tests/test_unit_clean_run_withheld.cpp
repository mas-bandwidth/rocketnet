/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The clean-run rule (tests/counters_expect.h) and COOKIE_REPLY_WITHHELD: a
// withheld cookie reply is the per-update limit answering one request of a
// source and withholding its copies (t12 rule 7), so on a clean run it is
// explained when at least one cookie reply went out: the cookie reasons
// (which count every withheld request too) exceed the withheld count.
// Withheld with nothing answered, or any other refusal, still fails.

#include "test_unit_registry.h"

#include "rocketnet_counters.h"

#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#define check rocketnet_unit_check

static int clean_run_refusals( uint64_t withheld, uint64_t no_cookie, uint64_t did_not_verify, uint64_t expired )
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD] = withheld;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] = no_cookie;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] = did_not_verify;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED] = expired;
    char report[1024];
    return rocketnet_counters_refusals( &counters, NULL, report, (int) sizeof( report ) );
}

static void test_clean_run_explains_withheld_copies_of_answered_requests()
{
    // nothing moved
    check( clean_run_refusals( 0, 0, 0, 0 ) == 0 );

    // an honest client's two cookieless requests in one update: one answered, one withheld
    check( clean_run_refusals( 1, 2, 0, 0 ) == 0 );
    check( clean_run_refusals( 2, 4, 0, 0 ) == 0 );

    // an update late by several send intervals: one answered, its copies withheld
    check( clean_run_refusals( 2, 3, 0, 0 ) == 0 );
    check( clean_run_refusals( 5, 6, 0, 0 ) == 0 );

    // the copy of a stale cookie's request, answered once: the withheld copy
    // is explained, the stale cookie itself is a refusal of its own
    check( clean_run_refusals( 1, 0, 0, 2 ) == 1 );

    // withheld with nothing answered still fails
    check( clean_run_refusals( 1, 1, 0, 0 ) == 1 );
    check( clean_run_refusals( 2, 2, 0, 0 ) == 1 );
    check( clean_run_refusals( 1, 0, 0, 0 ) == 1 );

    // any other refusal still fails, explained withheld or not
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD] = 1;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] = 2;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] = 1;
    char report[1024];
    check( rocketnet_counters_refusals( &counters, NULL, report, (int) sizeof( report ) ) == 1 );
    check( strstr( report, "CONNECTION_REQUEST_TOKEN_EXPIRED" ) != NULL );
    check( strstr( report, "CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD" ) == NULL );
}

REGISTER_UNIT_TEST( test_clean_run_explains_withheld_copies_of_answered_requests );

// The receive overflow rows against the alias table (standard/TRANSPORT.md):
// URING_RECEIVE_DROPPED_OVERFLOW (GRO segments past one receive's most)
// aliases no shared row, so it is a refusal of its own; GSO_RECEIVE_DROPPED_OVERFLOW
// aliases RECEIVE_DROPPED_FULL only for the full-bank drops, and the rest are
// the kernel's socket drops (SO_RXQ_OVFL), a refusal of their own.

static int clean_run_overflow_refusals( int overflow_counter, uint64_t overflow, uint64_t full, const int * allowed, char * report, int report_bytes )
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    counters.values[overflow_counter] = overflow;
    counters.values[ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL] = full;
    return rocketnet_counters_refusals( &counters, allowed, report, report_bytes );
}

static void test_clean_run_flags_receive_overflows_no_shared_row_counts()
{
    char report[1024];
    static const int full_allowed[] = { ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL, -1 };

    // a planted uring overflow fails a clean run
    check( clean_run_overflow_refusals( ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_OVERFLOW, 3, 0, NULL, report, (int) sizeof( report ) ) == 1 );
    check( strstr( report, "URING_RECEIVE_DROPPED_OVERFLOW = 3" ) != NULL );

    // gso overflow that is all full-bank drops: only RECEIVE_DROPPED_FULL is reported
    check( clean_run_overflow_refusals( ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW, 5, 5, NULL, report, (int) sizeof( report ) ) == 1 );
    check( strstr( report, "GSO_RECEIVE_DROPPED_OVERFLOW" ) == NULL );
    check( clean_run_overflow_refusals( ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW, 5, 5, full_allowed, report, (int) sizeof( report ) ) == 0 );

    // planted socket drops past the full-bank drops are flagged, by the excess
    check( clean_run_overflow_refusals( ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW, 7, 5, full_allowed, report, (int) sizeof( report ) ) == 1 );
    check( strstr( report, "GSO_RECEIVE_DROPPED_OVERFLOW = 2 (past RECEIVE_DROPPED_FULL)" ) != NULL );
    check( clean_run_overflow_refusals( ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW, 4, 0, NULL, report, (int) sizeof( report ) ) == 1 );
    check( strstr( report, "GSO_RECEIVE_DROPPED_OVERFLOW = 4\n" ) != NULL );

    // a run whose fault explains the gso overflow lists it
    static const int gso_allowed[] = { ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW, -1 };
    check( clean_run_overflow_refusals( ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW, 4, 0, gso_allowed, report, (int) sizeof( report ) ) == 0 );
}

REGISTER_UNIT_TEST( test_clean_run_flags_receive_overflows_no_shared_row_counts );

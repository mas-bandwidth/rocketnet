/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The clean-run rule (tests/counters_expect.h) and COOKIE_REPLY_WITHHELD: a
// withheld cookie reply is the per-update limit answering one request of a
// source and withholding its copy (t12 rule 7), so on a clean run it is
// explained when it withholds no more than it answers: the cookie reasons
// (which count every withheld request too) are at least twice the withheld
// count. Withheld with nothing answered, more withheld than answered, or any
// other refusal, still fails.

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

    // more withheld than answered: a source shut out update after update
    check( clean_run_refusals( 2, 3, 0, 0 ) == 1 );
    check( clean_run_refusals( 5, 6, 0, 0 ) == 1 );

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

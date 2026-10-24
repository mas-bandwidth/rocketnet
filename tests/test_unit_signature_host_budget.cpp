/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#include "test_unit_registry.h"
#include "rocketnet_server.h"
#include <string.h>

static void test_signature_failure_budget_tracks_hosts()
{
    const char * hosts[] = { "198.51.100.7:10000", "[2001:db8::7]:10000" };
    for ( int family = 0; family < 2; ++family )
    {
        struct rocketnet_signature_verify_budget_t budget;
        memset( &budget, 0, sizeof(budget) );
        struct rocketnet_address_t source;
        rocketnet_unit_check( rocketnet_address_parse( &source, hosts[family] ) );
        for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET; ++i )
        {
            source.port = (uint16_t) ( 10000 + i );
            rocketnet_unit_check( rocketnet_signature_verify_budget_allow( &budget, &source, 1000 ) );
            rocketnet_signature_verify_budget_record_failure( &budget, &source, 1000 );
        }
        source.port = 20000;
        rocketnet_unit_check( !rocketnet_signature_verify_budget_allow( &budget, &source, 1000 ) );
        struct rocketnet_address_t other;
        rocketnet_unit_check( rocketnet_address_parse( &other, family ? "[2001:db8::8]:20000" : "198.51.100.8:20000" ) );
        rocketnet_unit_check( rocketnet_signature_verify_budget_allow( &budget, &other, 1000 ) );
        rocketnet_unit_check( rocketnet_signature_verify_budget_allow( &budget, &source,
                             1000 + ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS ) );
    }
}
REGISTER_UNIT_TEST( test_signature_failure_budget_tracks_hosts );

static void test_signature_budget_window_restarts_on_backwards_clock()
{
    // a host clock stepped back (an NTP step, a VM resume) starts a fresh
    // window: a recorded failure refuses its source for at most the window
    // length of clock movement in either direction
    // (tla/RNSecurityTables.tla FloodWindowBounded).
    struct rocketnet_signature_verify_budget_t budget;
    memset( &budget, 0, sizeof(budget) );
    struct rocketnet_address_t source;
    rocketnet_unit_check( rocketnet_address_parse( &source, "198.51.100.7:10000" ) );
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET; ++i )
    {
        rocketnet_unit_check( rocketnet_signature_verify_budget_allow( &budget, &source, 1000 ) );
        rocketnet_signature_verify_budget_record_failure( &budget, &source, 1000 );
    }
    rocketnet_unit_check( !rocketnet_signature_verify_budget_allow( &budget, &source, 1000 ) );
    rocketnet_unit_check( rocketnet_signature_verify_budget_allow( &budget, &source, 900 ) );
    rocketnet_unit_check( budget.window_start == 900 );
    rocketnet_unit_check( budget.failure_count == 0 );
}
REGISTER_UNIT_TEST( test_signature_budget_window_restarts_on_backwards_clock );

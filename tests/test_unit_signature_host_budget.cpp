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
        memset( &budget, 0, sizeof( budget ) );

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
    // (proofs/tla/RNSecurityTables.tla FloodWindowBounded).

    struct rocketnet_signature_verify_budget_t budget;
    memset( &budget, 0, sizeof( budget ) );

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

// One source takes one slot however often it fails, so the table holds as
// many sources as it has slots: S fails twice, 62 others once each, and B,
// the 64th source, is still remembered. With the global budget then spent
// (S fails again until 90), B is refused. The table full, a 65th source is
// counted but not remembered and writes nothing past the table. The next
// window forgets every source: S is verified again once the new window's
// budget is spent by others.

struct rn_signature_budget_guarded_t
{
    struct rocketnet_signature_verify_budget_t budget;
    uint8_t guard[64];
};

static void signature_budget_source( struct rocketnet_address_t * address, int index )
{
    memset( address, 0, sizeof( *address ) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = 10;
    address->data.ipv4[1] = (uint8_t) ( index >> 8 );
    address->data.ipv4[2] = (uint8_t) index;
    address->data.ipv4[3] = 1;
    address->port = 40000;
}

static void test_signature_budget_one_slot_per_source()
{
    struct rn_signature_budget_guarded_t guarded;
    memset( &guarded, 0, sizeof( guarded ) );
    struct rocketnet_signature_verify_budget_t * budget = &guarded.budget;

    struct rocketnet_address_t s, b, extra;
    signature_budget_source( &s, 1000 );
    signature_budget_source( &b, 2000 );
    signature_budget_source( &extra, 3000 );

    rocketnet_signature_verify_budget_record_failure( budget, &s, 1000 );
    rocketnet_signature_verify_budget_record_failure( budget, &s, 1000 );
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS - 2; ++i )
    {
        struct rocketnet_address_t other;
        signature_budget_source( &other, i );
        rocketnet_signature_verify_budget_record_failure( budget, &other, 1000 );
    }
    rocketnet_signature_verify_budget_record_failure( budget, &b, 1000 );
    rocketnet_signature_verify_budget_record_failure( budget, &extra, 1000 );

    while ( budget->failure_count < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET )
    {
        rocketnet_signature_verify_budget_record_failure( budget, &s, 1000 );
    }

    rocketnet_unit_check( !rocketnet_signature_verify_budget_allow( budget, &s, 1000 ) );
    rocketnet_unit_check( !rocketnet_signature_verify_budget_allow( budget, &b, 1000 ) );
    rocketnet_unit_check( rocketnet_signature_verify_budget_allow( budget, &extra, 1000 ) );
    for ( int i = 0; i < (int) sizeof( guarded.guard ); ++i )
    {
        rocketnet_unit_check( guarded.guard[i] == 0 );
    }

    // the next window: others spend its budget, S has not failed in it

    const uint64_t next = 1000 + ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS;
    while ( budget->failure_count < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET || budget->window_start != next )
    {
        rocketnet_signature_verify_budget_record_failure( budget, &b, next );
    }
    rocketnet_unit_check( rocketnet_signature_verify_budget_allow( budget, &s, next ) );
    rocketnet_unit_check( !rocketnet_signature_verify_budget_allow( budget, &b, next ) );
}

REGISTER_UNIT_TEST( test_signature_budget_one_slot_per_source );

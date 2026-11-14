/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The operator memory budget's verdict on a transport list
// (rocketnet_transport_budget_count and rocketnet_transport_budget_refuses in
// sdk/rocketnet_driver.h), which the client resolve
// (rocketnet_client_transport_resolve) and the server resolve
// (rocketnet_server_transport_resolve_staged) both take, candidate by
// candidate in list order, before any allocation.
//
// A custom transport (a row from ROCKETNET_TRANSPORT_REGISTRY_HEADER, past the
// built-in rows) has no SDK memory plan. Listed after built-in transports
// whose plans all exceed the budget, it is still tried: the create is not
// refused before the transport order reaches it. The unit build has no custom
// row, so the custom candidate here is the first id past the built-in rows,
// the id such a row takes. The end-to-end check with a real custom row is
// tests/driver_extension (external_transport_budget_fallback), a functional
// test.
//
// No sockets, no real time, no environment: pure tallies, and plan lookups
// that are size computations.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_driver.h"
#include "rocketnet_layers.h"

#include <stdint.h>

#define check rocketnet_unit_check

static const int budget_custom_candidate = ROCKETNET_TRANSPORT_RIO + 1;

// every built-in row is built in, and the id past the last of them is custom
static void test_transport_budget_custom_is_past_the_built_in_rows()
{
    // up to the last built-in row only: a build with a registry header has its
    // custom rows below ROCKETNET_NUM_TRANSPORTS
    for ( int transport = 0; transport <= ROCKETNET_TRANSPORT_RIO; ++transport )
    {
        check( !rocketnet_transport_custom( transport ) );
    }

    check( rocketnet_transport_custom( budget_custom_candidate ) );

    // a name no built-in row has gets no SDK plan on either side: the custom
    // candidate's plan is unavailable, never zero bytes that fit

    check( rocketnet_client_transport_required_memory( NULL, "external_test" ) == 0 );
    check( rocketnet_server_transport_required_memory( NULL, "external_test" ) == 0 );
}

// the client list "portable,<custom>" under a budget below portable's plan:
// portable is refused, the custom transport is still tried
static void test_transport_budget_client_tries_custom_after_refused()
{
    rocketnet_transport_budget_t tally = {};
    rocketnet_transport_budget_count( tally, ROCKETNET_TRANSPORT_PORTABLE, true, true );
    rocketnet_transport_budget_count( tally, budget_custom_candidate, false, false );

    check( tally.refused == 1 );
    check( tally.tryable == 1 );
    check( !rocketnet_transport_budget_refuses( 1, tally ) );
}

// the server list "gso,portable,<custom>" under a budget below both built-in
// plans: both are refused, the custom transport is still tried
static void test_transport_budget_server_tries_custom_after_refused()
{
    rocketnet_transport_budget_t tally = {};
    rocketnet_transport_budget_count( tally, ROCKETNET_TRANSPORT_GSO, true, true );
    rocketnet_transport_budget_count( tally, ROCKETNET_TRANSPORT_PORTABLE, true, true );
    rocketnet_transport_budget_count( tally, budget_custom_candidate, false, false );

    check( tally.refused == 2 );
    check( tally.tryable == 1 );
    check( !rocketnet_transport_budget_refuses( 1, tally ) );
}

// what the budget refused before still refuses: every built-in over the
// budget, and a built-in with no plan beside them (it does not fit and is
// not custom); a budget of zero refuses nothing; one fitting plan is enough
static void test_transport_budget_built_in_verdicts_unchanged()
{
    rocketnet_transport_budget_t all_over = {};
    rocketnet_transport_budget_count( all_over, ROCKETNET_TRANSPORT_GSO, true, true );
    rocketnet_transport_budget_count( all_over, ROCKETNET_TRANSPORT_PORTABLE, true, true );
    check( rocketnet_transport_budget_refuses( 1, all_over ) );

    rocketnet_transport_budget_t unplanned_built_in = {};
    rocketnet_transport_budget_count( unplanned_built_in, ROCKETNET_TRANSPORT_XDP, false, false );
    rocketnet_transport_budget_count( unplanned_built_in, ROCKETNET_TRANSPORT_PORTABLE, true, true );
    check( unplanned_built_in.tryable == 0 );
    check( rocketnet_transport_budget_refuses( 1, unplanned_built_in ) );

    rocketnet_transport_budget_t one_fits = {};
    rocketnet_transport_budget_count( one_fits, ROCKETNET_TRANSPORT_GSO, true, true );
    rocketnet_transport_budget_count( one_fits, ROCKETNET_TRANSPORT_PORTABLE, true, false );
    check( !rocketnet_transport_budget_refuses( 1, one_fits ) );

    check( !rocketnet_transport_budget_refuses( 0, all_over ) );

    // a custom transport alone, under any budget: nothing was refused
    rocketnet_transport_budget_t custom_alone = {};
    rocketnet_transport_budget_count( custom_alone, budget_custom_candidate, false, false );
    check( !rocketnet_transport_budget_refuses( 1, custom_alone ) );
}

REGISTER_UNIT_TEST( test_transport_budget_built_in_verdicts_unchanged );
REGISTER_UNIT_TEST( test_transport_budget_client_tries_custom_after_refused );
REGISTER_UNIT_TEST( test_transport_budget_custom_is_past_the_built_in_rows );
REGISTER_UNIT_TEST( test_transport_budget_server_tries_custom_after_refused );

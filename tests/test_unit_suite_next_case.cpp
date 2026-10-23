/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The suite runner's picker (suite_next_case, tests/suite_runner.h): a case
// starts only beside cases of its own group, and a SUITE_ONE_AT_A_TIME case
// never starts while another such case runs, while the rest of its group keeps
// starting around it. A skipped case starts once the case blocking it ends,
// before any later group. Pure: the picker reads vectors and starts nothing.

#include "test_unit_registry.h"

#include "suite_runner.h"

#include <vector>

#define check rocketnet_unit_check

struct next_case_running_t
{
    size_t index;
    int group;
};

static suite_case next_case_item( const char * name, int group, bool one_at_a_time )
{
    suite_case item;
    item.name = name;
    item.group = group;
    item.one_at_a_time = one_at_a_time;
    return item;
}

static void test_suite_next_case_one_at_a_time_waits()
{
    // group 0: a, b (one at a time), c (one at a time), d; group 1: e
    std::vector<suite_case> pending;
    pending.push_back( next_case_item( "test_a", 0, false ) );
    pending.push_back( next_case_item( "test_b", 0, true ) );
    pending.push_back( next_case_item( "test_c", 0, true ) );
    pending.push_back( next_case_item( "test_d", 0, false ) );
    pending.push_back( next_case_item( "test_e", 1, false ) );
    std::vector<bool> started( pending.size(), false );
    std::vector<next_case_running_t> running;

    // nothing runs: the first case in order
    check( suite_next_case( pending, started, 0, running ) == 0 );

    // a runs: b starts beside it
    started[0] = true;
    running.push_back( { 0, 0 } );
    check( suite_next_case( pending, started, 1, running ) == 1 );

    // a and b run: c waits for b, and d starts around it
    started[1] = true;
    running.push_back( { 1, 0 } );
    check( suite_next_case( pending, started, 2, running ) == 3 );

    // a, b and d run: c still waits, and group 1 does not start beside group 0
    started[3] = true;
    running.push_back( { 3, 0 } );
    check( suite_next_case( pending, started, 2, running ) == pending.size() );

    // b ends: c starts beside a and d
    running.erase( running.begin() + 1 );
    check( suite_next_case( pending, started, 2, running ) == 2 );

    // every group 0 case started and some still run: e waits
    started[2] = true;
    running.push_back( { 2, 0 } );
    check( suite_next_case( pending, started, 4, running ) == pending.size() );

    // group 0 ends: e starts
    running.clear();
    check( suite_next_case( pending, started, 4, running ) == 4 );

    // all started: nothing left
    started[4] = true;
    check( suite_next_case( pending, started, 5, running ) == pending.size() );
}

REGISTER_UNIT_TEST( test_suite_next_case_one_at_a_time_waits );

static void test_suite_next_case_skipped_case_starts_before_a_later_group()
{
    // group 0: a (one at a time), b (one at a time); group 1: c
    std::vector<suite_case> pending;
    pending.push_back( next_case_item( "test_a", 0, true ) );
    pending.push_back( next_case_item( "test_b", 0, true ) );
    pending.push_back( next_case_item( "test_c", 1, false ) );
    std::vector<bool> started( pending.size(), false );
    std::vector<next_case_running_t> running;

    started[0] = true;
    running.push_back( { 0, 0 } );
    // b waits for a; c is another group
    check( suite_next_case( pending, started, 1, running ) == pending.size() );

    // a ends: b, not c, starts next
    running.clear();
    check( suite_next_case( pending, started, 1, running ) == 1 );
}

REGISTER_UNIT_TEST( test_suite_next_case_skipped_case_starts_before_a_later_group );

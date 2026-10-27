/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The fixed registries that replaced the static pointer chains: the unit-file
// test registry (test_unit_registry.h, capacity 2,048) and the suite RSS bound
// registry (suite_runner.h, capacity 64). Each case runs on a local registry
// so the real static registrations are never touched: insertion preserves the
// chains' reverse-construction order, the unit registry refuses a duplicate
// name, the suite registry keeps the chain's later-registration-wins lookup,
// and a full registry is left unchanged and reports the omitted name through
// its overflow path rather than dropping it. No time, sockets or threads.

#include "test_unit_registry.h"
#include "suite_runner.h"

#include <stdio.h>
#include <string.h>

static void rocketnet_registry_dummy_test()
{
}

// The unit registry's fixed-capacity mechanics, independent of the real
// static constructors: insertion preserves the old reverse order, duplicates
// and overflow refuse without mutation, and every admitted entry is
// addressable exactly once.
static void test_unit_registry_sequence_contract()
{
    struct rocketnet_unit_file_registry_t registry = {};
    rocketnet_unit_check( rocketnet_unit_file_register( &registry, 2, "test_a", rocketnet_registry_dummy_test, NULL ) ==
                          ROCKETNET_UNIT_FILE_REGISTERED );
    rocketnet_unit_check( rocketnet_unit_file_register( &registry, 2, "test_b", rocketnet_registry_dummy_test, "card" ) ==
                          ROCKETNET_UNIT_FILE_REGISTERED );
    rocketnet_unit_check( registry.count == 2 );
    rocketnet_unit_check( strcmp( registry.tests[0].name, "test_b" ) == 0 );
    rocketnet_unit_check( strcmp( registry.tests[1].name, "test_a" ) == 0 );
    rocketnet_unit_check( registry.tests[0].expected_fail_card != NULL );
    rocketnet_unit_check( rocketnet_unit_file_register( &registry, 2, "test_a", rocketnet_registry_dummy_test, NULL ) ==
                          ROCKETNET_UNIT_FILE_DUPLICATE );
    rocketnet_unit_check( rocketnet_unit_file_register( &registry, 2, "test_c", rocketnet_registry_dummy_test, NULL ) ==
                          ROCKETNET_UNIT_FILE_FULL );
    rocketnet_unit_check( registry.count == 2 );
}

REGISTER_UNIT_TEST( test_unit_registry_sequence_contract );

// The live unit registry: within its stated capacity, and every registered
// name is found at its own index, so no name is enumerated twice.
static void test_unit_registry_sequence_live_names_unique()
{
    const int count = rocketnet_unit_tests_count();
    rocketnet_unit_check( count > 0 );
    rocketnet_unit_check( count <= ROCKETNET_UNIT_FILE_TEST_CAPACITY );
    const struct rocketnet_unit_file_registry_t * registry = rocketnet_unit_file_registry();
    for ( int i = 0; i < count; i++ )
    {
        const char * name = rocketnet_unit_tests_name( i );
        rocketnet_unit_check( name != NULL );
        rocketnet_unit_check( rocketnet_unit_file_find( name ) == &registry->tests[i] );
    }
}

REGISTER_UNIT_TEST( test_unit_registry_sequence_live_names_unique );

static long suite_bound_one_mb()
{
    return 1;
}

static long suite_bound_two_mb()
{
    return 2;
}

// Records an overflow instead of aborting, so the refusal path is observable
// in-process.
static int suite_overflow_calls;
static const char * suite_overflow_name;
static int suite_overflow_capacity;

static void suite_overflow_record( const char * name, int capacity )
{
    suite_overflow_calls++;
    suite_overflow_name = name;
    suite_overflow_capacity = capacity;
}

static void suite_overflow_reset()
{
    suite_overflow_calls = 0;
    suite_overflow_name = NULL;
    suite_overflow_capacity = 0;
}

// Fill the suite registry to its full capacity of 64: every entry is admitted
// at the front (reverse construction order), the 65th is refused through the
// overflow path naming it and the capacity, and nothing is mutated.
static void test_unit_registry_sequence_suite_capacity_refusal()
{
    static suite_case_rss_bound_registry_t registry;
    memset( &registry, 0, sizeof( registry ) );
    static char names[SUITE_CASE_RSS_BOUND_CAPACITY + 1][32];
    suite_overflow_reset();
    for ( int i = 0; i < SUITE_CASE_RSS_BOUND_CAPACITY; i++ )
    {
        snprintf( names[i], sizeof( names[i] ), "suite_case_%d", i );
        rocketnet_unit_check( suite_case_rss_bound_add( &registry, SUITE_CASE_RSS_BOUND_CAPACITY, names[i],
                                                        suite_bound_one_mb, suite_overflow_record ) );
    }
    rocketnet_unit_check( registry.count == SUITE_CASE_RSS_BOUND_CAPACITY );
    rocketnet_unit_check( suite_overflow_calls == 0 );
    for ( int i = 0; i < SUITE_CASE_RSS_BOUND_CAPACITY; i++ )
        rocketnet_unit_check( registry.entries[i].name == names[SUITE_CASE_RSS_BOUND_CAPACITY - 1 - i] );

    suite_case_rss_bound_registry_t before;
    memcpy( &before, &registry, sizeof( before ) );
    snprintf( names[SUITE_CASE_RSS_BOUND_CAPACITY], sizeof( names[0] ), "suite_case_omitted" );
    rocketnet_unit_check( !suite_case_rss_bound_add( &registry, SUITE_CASE_RSS_BOUND_CAPACITY,
                                                     names[SUITE_CASE_RSS_BOUND_CAPACITY], suite_bound_two_mb,
                                                     suite_overflow_record ) );
    rocketnet_unit_check( suite_overflow_calls == 1 );
    rocketnet_unit_check( suite_overflow_name == names[SUITE_CASE_RSS_BOUND_CAPACITY] );
    rocketnet_unit_check( suite_overflow_capacity == SUITE_CASE_RSS_BOUND_CAPACITY );
    rocketnet_unit_check( memcmp( &before, &registry, sizeof( before ) ) == 0 );
    rocketnet_unit_check( suite_case_rss_bound_find( &registry, "suite_case_omitted" ) == NULL );
    for ( int i = 0; i < SUITE_CASE_RSS_BOUND_CAPACITY; i++ )
        rocketnet_unit_check( suite_case_rss_bound_find( &registry, names[i] ) ==
                              &registry.entries[SUITE_CASE_RSS_BOUND_CAPACITY - 1 - i] );
    suite_overflow_reset();
}

REGISTER_UNIT_TEST( test_unit_registry_sequence_suite_capacity_refusal );

// A name registered twice keeps both entries, and a lookup finds the later
// registration, as the chain's head-first walk did.
static void test_unit_registry_sequence_suite_duplicate_later_wins()
{
    suite_case_rss_bound_registry_t registry = {};
    suite_overflow_reset();
    rocketnet_unit_check( suite_case_rss_bound_add( &registry, SUITE_CASE_RSS_BOUND_CAPACITY, "suite_case_a",
                                                    suite_bound_one_mb, suite_overflow_record ) );
    rocketnet_unit_check( suite_case_rss_bound_add( &registry, SUITE_CASE_RSS_BOUND_CAPACITY, "suite_case_b",
                                                    suite_bound_one_mb, suite_overflow_record ) );
    rocketnet_unit_check( suite_case_rss_bound_add( &registry, SUITE_CASE_RSS_BOUND_CAPACITY, "suite_case_a",
                                                    suite_bound_two_mb, suite_overflow_record ) );
    rocketnet_unit_check( registry.count == 3 );
    rocketnet_unit_check( suite_overflow_calls == 0 );
    suite_case_rss_bound_t * found = suite_case_rss_bound_find( &registry, "suite_case_a" );
    rocketnet_unit_check( found == &registry.entries[0] );
    rocketnet_unit_check( found->bound_mb() == 2 );
    rocketnet_unit_check( strcmp( registry.entries[1].name, "suite_case_b" ) == 0 );
    rocketnet_unit_check( strcmp( registry.entries[2].name, "suite_case_a" ) == 0 );
    rocketnet_unit_check( registry.entries[2].bound_mb() == 1 );
}

REGISTER_UNIT_TEST( test_unit_registry_sequence_suite_duplicate_later_wins );

// The loud path a static registrar takes on overflow: the line it prints
// before aborting names the capacity and the omitted case. The live registry
// stays within the stated capacity.
static void test_unit_registry_sequence_suite_overflow_is_loud()
{
    char text[256];
    const int written = suite_case_rss_bound_overflow_text( text, sizeof( text ), "suite_case_omitted",
                                                            SUITE_CASE_RSS_BOUND_CAPACITY );
    rocketnet_unit_check( written > 0 && (size_t) written < sizeof( text ) );
    rocketnet_unit_check( strcmp( text, "error: suite RSS bound registry capacity 64 exceeded while registering "
                                        "suite_case_omitted\n" ) == 0 );
    rocketnet_unit_check( SUITE_CASE_RSS_BOUND_CAPACITY == 64 );
    rocketnet_unit_check( suite_case_rss_bound_registry()->count <= SUITE_CASE_RSS_BOUND_CAPACITY );
}

REGISTER_UNIT_TEST( test_unit_registry_sequence_suite_overflow_is_loud );

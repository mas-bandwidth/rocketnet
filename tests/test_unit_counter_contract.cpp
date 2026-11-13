/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The counter contract, read from the built library. Counters are ABI: one
// table, identical on every platform and in every build, and an id never
// moves (tests/counter_contract.inc). The enum is held to the contract at
// compile time; the library's own name and class tables, compiled with this
// build's ROCKETNET_DEV, are read in full at run time. The case is registered
// in every build, so the development and production suites on Linux, macOS
// and Windows each read the same table against the same list, and each prints
// the table's digest for a side-by-side comparison.
//
// No clock, socket or file: the contract is compiled in.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// every contract id is the enum's id: a renumbered counter fails the build

#define ROCKETNET_COUNTER_CONTRACT( id, name, klass ) static_assert( ROCKETNET_COUNTER_##name == ( id ), "counter " #name " is not id " #id " (tests/counter_contract.inc)" );
#include "counter_contract.inc"
#undef ROCKETNET_COUNTER_CONTRACT

struct counter_contract_row_t
{
    int id;
    const char * name;
    int counter_class;
};

static const counter_contract_row_t counter_contract_rows[] = {
#define ROCKETNET_COUNTER_CONTRACT( id, name, klass ) { ( id ), #name, ROCKETNET_COUNTER_CLASS_##klass },
#include "counter_contract.inc"
#undef ROCKETNET_COUNTER_CONTRACT
};

#define COUNTER_CONTRACT_ROWS ( (int) ( sizeof( counter_contract_rows ) / sizeof( counter_contract_rows[0] ) ) )

static_assert( COUNTER_CONTRACT_ROWS == ROCKETNET_NUM_COUNTERS, "the counter enum and tests/counter_contract.inc differ in length" );

// FNV-1a over "<id>\t<name>\t<class>\n" per row: one figure that two builds
// print the same exactly when their tables agree

static uint64_t counter_contract_hash( uint64_t hash, const char * text )
{
    for ( const char * p = text; *p; p++ )
    {
        hash ^= (uint8_t) *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void test_counter_contract_the_built_table_is_the_contract()
{
    check( rocketnet_num_counters() == COUNTER_CONTRACT_ROWS );

    uint64_t digest = 14695981039346656037ULL;

    for ( int i = 0; i < COUNTER_CONTRACT_ROWS; i++ )
    {
        const counter_contract_row_t & row = counter_contract_rows[i];
        check( row.id == i );

        const char * name = rocketnet_counter_name( i );
        check( name != NULL );
        if ( strcmp( name, row.name ) != 0 )
        {
            printf( "counter %d is %s in the library and %s in the contract\n", i, name, row.name );
        }
        check( strcmp( name, row.name ) == 0 );

        const int counter_class = rocketnet_counter_class( i );
        if ( counter_class != row.counter_class )
        {
            printf( "counter %d %s has class %d in the library and %d in the contract\n", i, name, counter_class, row.counter_class );
        }
        check( counter_class == row.counter_class );

        char line[160];
        snprintf( line, sizeof( line ), "%d\t%s\t%d\n", i, name, counter_class );
        digest = counter_contract_hash( digest, line );
    }

    // past the table there is nothing

    check( rocketnet_counter_class( COUNTER_CONTRACT_ROWS ) == -1 );
    check( rocketnet_counter_class( -1 ) == -1 );

    printf( "counter contract: %d counters, digest %016llx, ROCKETNET_DEV=%d\n", COUNTER_CONTRACT_ROWS, (unsigned long long) digest, (int) ROCKETNET_DEV );
}

REGISTER_UNIT_TEST( test_counter_contract_the_built_table_is_the_contract );

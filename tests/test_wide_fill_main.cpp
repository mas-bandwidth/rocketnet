/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_wide_fill: the wide fill's cases (tests/wide_fill_cases.cpp), which
// start the game's job threads, so they live outside the unit suite (which
// starts none) in a functional-tier binary of their own. CI runs it in the
// functional legs and under TSan (the unit binary's own operator new, in
// tests/test_channels.cpp, would collide with the TSan runtime's in any
// case). No socket, no real time. ./bin/test_wide_fill [case ...]
//
// It fails as the unit worker does on a case that returned having run no
// check (tests/check_count.h), on a named case the registry does not hold,
// when it ran no case at all, and when rocketnet_term reports an allocation
// still live after the cases ran.

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"

#include <stdio.h>
#include <string.h>

int main( int argc, char ** argv )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "rocketnet_init failed\n" );
        return 1;
    }

    const struct rocketnet_unit_file_registry_t * registry = rocketnet_unit_file_registry();
    for ( int a = 1; a < argc; a++ )
    {
        bool known = false;
        for ( int i = 0; i < registry->count; i++ )
        {
            known = known || strcmp( argv[a], registry->tests[i].name ) == 0;
        }
        if ( !known )
        {
            printf( "check failed: %s is not a wide fill case\n", argv[a] );
            return 1;
        }
    }

    int ran = 0;
    for ( int i = registry->count - 1; i >= 0; i-- )
    {
        bool chosen = argc < 2;
        for ( int a = 1; a < argc; a++ )
        {
            chosen = chosen || strcmp( argv[a], registry->tests[i].name ) == 0;
        }
        if ( !chosen )
        {
            continue;
        }
        printf( "%s\n", registry->tests[i].name );
        fflush( stdout );
        const uint64_t checks_before = rocketnet_test_checks_run().load();
        registry->tests[i].function();
        if ( rocketnet_test_checks_run().load() == checks_before )
        {
            printf( "check failed: %s returned without running a single check\n", registry->tests[i].name );
            return 1;
        }
        ran++;
    }

    // every case destroyed what it made: anything still allocated is a leak
    // the cases' own checks missed, and rocketnet_term says so by its status
    const enum rocketnet_status_t term = rocketnet_term();
    if ( term != ROCKETNET_STATUS_OK )
    {
        printf( "check failed: rocketnet_term returned %s after the wide fill cases\n", rocketnet_status_name( term ) );
        return 1;
    }
    printf( "%d wide fill cases passed\n", ran );
    return ran > 0 ? 0 : 1;
}

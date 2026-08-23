/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Test entry, in the shape of flow's cmd/test/test.cpp. Exits non-zero on
    any failure so CI fails honestly.
*/

#include "rocketnet.h"
#include "rocketnet_tests.h"

#include <stdio.h>

int main()
{
    rocketnet_quiet( true );

    if ( rocketnet_init( NULL ) != ROCKETNET_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    printf( "\nRunning rocketnet stage 1 tests:\n\n" );

    rocketnet_run_tests();

    rocketnet_term();

    // init and term are re-entrant as a pair: a second full cycle works
    if ( rocketnet_init( NULL ) != ROCKETNET_OK )
    {
        printf( "error: failed to re-initialize rocketnet after term\n" );
        return 1;
    }

    rocketnet_term();

    printf( "\nAll tests passed.\n\n" );

    fflush( stdout );

    return 0;
}

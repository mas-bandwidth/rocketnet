/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_tests.h"

#include <stdio.h>
#include <string.h>

int main()
{
    rocketnet_quiet( true );

    if ( !rocketnet_init() )
    {
        printf( "error: failed to initialize rocketnet\n" );
    }

    printf( "\nRunning SDK tests:\n\n" );

    rocketnet_run_tests();

    rocketnet_term();

    printf( "\n" );

    fflush( stdout );

    return 0;
}

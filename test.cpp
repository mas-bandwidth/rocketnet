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
    // quiet the info chatter, never the errors: a create that returns NULL
    // explains itself through rocketnet_error, and a test binary that
    // swallows that explanation fails with a bare check line and no cause
    // (the port-collision flake of #107 hid exactly that way)

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

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

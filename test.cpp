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
    // the captured floor. the suite runs silent -- rocketnet_run_tests
    // captures the library's log into a ring instead of printing it, and a
    // failing check dumps that ring before the check line -- so this level
    // is what a failure gets to show: errors are kept, so a create that
    // returns NULL always explains itself where it matters, and a passing
    // run prints test names and nothing else. ROCKETNET_TEST_VERBOSE=1
    // skips the capture and leaves the log live at this level.

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    if ( !rocketnet_init() )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    printf( "\nRunning SDK tests:\n\n" );

    rocketnet_run_tests();

    rocketnet_term();

    printf( "\n" );

    fflush( stdout );

    return 0;
}

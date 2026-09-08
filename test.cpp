/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main()
{
    // init reads this before the suite installs its config isolation.
#if defined(_WIN32)
    const int cleared = _putenv_s( "ROCKETNET_LOG_LEVEL", "" );
#else
    const int cleared = unsetenv( "ROCKETNET_LOG_LEVEL" );
#endif
    if ( cleared != 0 ) return 1;

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

    rocketnet_run_tests();

    rocketnet_term();

    fflush( stdout );

    return 0;
}

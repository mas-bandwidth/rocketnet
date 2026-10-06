/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "test/suite_runner.h"

#include <stdio.h>
#include <stdlib.h>

int rocketnet_unit_test_count();
const char * rocketnet_unit_test_name( int index );
void rocketnet_unit_prepare();
int rocketnet_unit_run_named( const char * name );

static int rocketnet_unit_worker( const char * name )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    setvbuf( stderr, NULL, _IOLBF, 0 );

    // init reads this before the suite installs its config isolation.
#if defined(_WIN32)
    const int cleared = _putenv_s( "ROCKETNET_LOG_LEVEL", "" );
#else
    const int cleared = unsetenv( "ROCKETNET_LOG_LEVEL" );
#endif
    if ( cleared != 0 )
        return 1;

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    if ( !rocketnet_init() )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_unit_prepare();
    const int result = rocketnet_unit_run_named( name );
    rocketnet_term();
    fflush( stdout );
    return result;
}

// sibling binaries the suite runs as one case each: test_channels replaces
// its process's allocation functions to trap live allocation, so it cannot
// share a process with the unit tests

static const suite_exec rocketnet_unit_execs[] =
{
    { "test_channels", "test_channels", true },
};

int main( int argc, char ** argv )
{
    return suite_main( argc, argv, rocketnet_unit_worker, rocketnet_unit_test_count, rocketnet_unit_test_name,
        rocketnet_unit_execs, (int) ( sizeof(rocketnet_unit_execs) / sizeof(rocketnet_unit_execs[0]) ), NULL, 0, true );
}

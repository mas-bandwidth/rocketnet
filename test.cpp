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

// the message layer's tests, in test_channels.cpp

int rocketnet_channels_test_count();
const char * rocketnet_channels_test_name( int index );
const char * rocketnet_channels_test_resolve( const char * name );
int rocketnet_channels_run_named( const char * name );
void rocketnet_channels_prepare( const char * name );
void rocketnet_channels_prepare_log( const char * name );

static int rocketnet_suite_test_count()
{
    return rocketnet_unit_test_count() + rocketnet_channels_test_count();
}

static const char * rocketnet_suite_test_name( int index )
{
    const int units = rocketnet_unit_test_count();
    return index < units ? rocketnet_unit_test_name( index ) : rocketnet_channels_test_name( index - units );
}

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

    // allocator cases install their hooks here: replacement is refused after init
    rocketnet_channels_prepare( name );

    if ( !rocketnet_init() )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_unit_prepare();
    rocketnet_channels_prepare_log( name );
    int result = rocketnet_channels_run_named( name );
    if ( result == 2 )
        result = rocketnet_unit_run_named( name );
    rocketnet_term();
    fflush( stdout );
    return result;
}

int main( int argc, char ** argv )
{
    // ./bin/test <name> runs that one test alone, in this process: a suite
    // name, or a message-layer test's alias (the name a card's TEST line uses)

    if ( argc == 2 && argv[1][0] != '-' )
    {
        const char * resolved = rocketnet_channels_test_resolve( argv[1] );
        return rocketnet_unit_worker( resolved ? resolved : argv[1] );
    }

    return suite_main( argc, argv, rocketnet_unit_worker, rocketnet_suite_test_count, rocketnet_suite_test_name, NULL, 0, NULL, 0, true );
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_wide_fill: tests/test_unit_wide_fill.cpp alone, for ThreadSanitizer.
// The unit binary replaces the global operator new (tests/test_channels.cpp),
// which the TSan runtime also defines, so the concurrent fill cases run under
// TSan from this binary; the unit binary runs the same cases in every other
// build. No socket, no real time. ./bin/test_wide_fill [case ...]

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
        registry->tests[i].function();
        ran++;
    }

    rocketnet_term();
    printf( "%d wide fill cases passed\n", ran );
    return ran > 0 ? 0 : 1;
}

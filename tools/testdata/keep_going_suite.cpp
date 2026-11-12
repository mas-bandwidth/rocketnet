/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A four-case suite for tools/test_suite_keep_going.py: the second and third
// cases fail, the first and fourth pass. A case that runs prints "ran <name>"
// and, when KEEP_GOING_SUITE_MARKS names a directory, writes a file named
// for itself there, so the test sees which cases ran even when the runner
// does not print a passing case's output.

#include "../../tests/suite_runner.h"

static const char * const keep_going_names[] = {
    "test_keep_going_first_passes",
    "test_keep_going_second_fails",
    "test_keep_going_third_fails",
    "test_keep_going_fourth_passes",
};

static int keep_going_case( const char * name )
{
    printf( "ran %s\n", name );
    fflush( stdout );
    const char * marks = std::getenv( "KEEP_GOING_SUITE_MARKS" );
    if ( marks )
    {
        const std::string path = std::string( marks ) + "/" + name;
        FILE * file = rocketnet_test_fopen( path.c_str(), "w" );
        if ( file )
        {
            fclose( file );
        }
    }
    if ( std::strstr( name, "_fails" ) )
    {
        printf( "check failed: %s planted failure\n", name );
        return 1;
    }
    return 0;
}

static int keep_going_count()
{
    return 4;
}

static const char * keep_going_name( int i )
{
    return keep_going_names[i];
}

int main( int argc, char ** argv )
{
    return suite_main( argc, argv, keep_going_case, keep_going_count, keep_going_name, NULL, 0 );
}

#include "../../tests/suite_runner.h"

static int run_case( const char * )
{
    std::vector<char> memory;
    if ( std::getenv( "NIGHTLY_TIMEOUT_CONTROL_RSS" ) )
        memory.resize( 8 * 1024 * 1024, 1 );
    std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) );
    const char * fail = std::getenv( "NIGHTLY_TIMEOUT_CONTROL_FAIL" );
    return fail ? 7 : 0;
}

static int case_count() { return 1; }
static const char * case_name( int ) { return "test_nightly_timeout_control"; }

int main( int argc, char ** argv )
{
    return suite_main( argc, argv, run_case, case_count, case_name, NULL, 0,
                       NULL, 0.01, 1, false, false );
}

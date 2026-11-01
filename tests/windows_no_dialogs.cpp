/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Windows only. Each case ends its own process the way a broken test does:
    abort, a failed assertion, the CRT's invalid parameter handler (#201's
    setvbuf) and an access violation. Under suite_main each must exit with a
    nonzero code at once and never wait on a window (suite_no_dialogs,
    tests/suite_runner.h). test_no_dialogs_dialog_routing_restored explicitly
    routes CRT errors to a message box before failing an assertion. In Debug
    it waits on that box, or exits without the assertion report on stderr
    when a service runner cannot show it. ci.yml's Windows leg (c) runs each
    case with `--run <case>` under a deadline. Every case fails by design, so this binary
    is never run as a suite.
*/

#include "suite_runner.h"

#include <cassert>

static int no_dialogs_abort()
{
    abort();
}

static int no_dialogs_assert()
{
    const volatile int never = 0;
    assert( never == 1 );
    (void) never;
    return 2;                                       // Release: the assertion is compiled out
}

static int no_dialogs_invalid_parameter()
{
    // the MSVC CRT refuses a zero size for any mode but _IONBF (#201)
    setvbuf( stdout, NULL, _IOLBF, 0 );
    return 2;
}

static int no_dialogs_access_violation()
{
    int * volatile nowhere = NULL;
    *nowhere = 1;
    return 2;
}

static int no_dialogs_dialog_routing_restored()
{
    SetErrorMode( 0 );
#if defined( _MSC_VER )
    // _OUT_TO_DEFAULT follows the application type; a console may use stderr.
    _set_error_mode( _OUT_TO_MSGBOX );
    _set_abort_behavior( _WRITE_ABORT_MSG | _CALL_REPORTFAULT, _WRITE_ABORT_MSG | _CALL_REPORTFAULT );
#if defined( _DEBUG )
    _CrtSetReportMode( _CRT_ASSERT, _CRTDBG_MODE_WNDW );
    _CrtSetReportMode( _CRT_ERROR, _CRTDBG_MODE_WNDW );
#endif // #if defined( _DEBUG )
#endif // #if defined( _MSC_VER )
    return no_dialogs_assert();
}

struct no_dialogs_case
{
    const char * name;
    int ( *run )();
};

static const no_dialogs_case no_dialogs_cases[] = {
    { "test_no_dialogs_abort", no_dialogs_abort },
    { "test_no_dialogs_assert", no_dialogs_assert },
    { "test_no_dialogs_invalid_parameter", no_dialogs_invalid_parameter },
    { "test_no_dialogs_access_violation", no_dialogs_access_violation },
    { "test_no_dialogs_dialog_routing_restored", no_dialogs_dialog_routing_restored },
};

static int no_dialogs_count()
{
    return (int) ( sizeof( no_dialogs_cases ) / sizeof( no_dialogs_cases[0] ) );
}

static const char * no_dialogs_name( int index )
{
    return no_dialogs_cases[index].name;
}

static int no_dialogs_worker( const char * name )
{
    for ( int i = 0; i < no_dialogs_count(); i++ )
        if ( strcmp( name, no_dialogs_cases[i].name ) == 0 )
            return no_dialogs_cases[i].run();
    printf( "check failed: no case %s\n", name );
    return 1;
}

int main( int argc, char ** argv )
{
    return suite_main( argc, argv, no_dialogs_worker, no_dialogs_count, no_dialogs_name, NULL, 0 );
}

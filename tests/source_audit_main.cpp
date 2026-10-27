/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The source-audit binary. The repository text audits that used to run as
// unit cases read source, header and doc files through ROCKETNET_SOURCE_DIR;
// they are source-policy checks, not unit logic, so they live in their own
// static binary here, which reads files only, opens no socket and starts no
// thread. CI runs it on a Linux leg beside tools/check_client_server_words.py.
// A case keeps its body and checks unchanged; only where it runs changed.
//
// The cases are named here rather than enumerated from the registry: the
// binary runs the audits and nothing else, so a case that another binary
// registers never runs here and a case that moves out of the unit suite is
// named in one place.

#include "test_unit_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * const source_audit_cases[] =
{
    "test_client_server_say_messages_not_packets",
    "test_config_env_every_name_read_is_documented",
    "test_counters_every_counter_is_named_by_a_test",
    "test_counters_every_outcome_in_at_most_one_family",
    "test_tools_never_cast_public_client_to_internal",
    "test_xdp_transport_host_every_counter_has_a_case"
};

// The unit suite runs every case in its own worker process, with the
// transport pins, the log level and the served-address overrides cleared, so
// no case runs against an environment the operator chose. The audit cases
// read the environment the same way they did there, so this binary clears the
// same names before it runs them.
static int source_audit_clear_environment()
{
#if defined(_WIN32)
    const int cleared = _putenv_s( "ROCKETNET_LOG_LEVEL", "" );
    const int cleared_server = _putenv_s( "ROCKETNET_SERVER_TRANSPORT", "" );
    const int cleared_client = _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", "" );
    const int cleared_ipv4 = _putenv_s( "ROCKETNET_SERVER_IPV4_ADDRESS", "" );
    const int cleared_ipv6 = _putenv_s( "ROCKETNET_SERVER_IPV6_ADDRESS", "" );
#else
    const int cleared = unsetenv( "ROCKETNET_LOG_LEVEL" );
    const int cleared_server = unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
    const int cleared_client = unsetenv( "ROCKETNET_CLIENT_TRANSPORT" );
    const int cleared_ipv4 = unsetenv( "ROCKETNET_SERVER_IPV4_ADDRESS" );
    const int cleared_ipv6 = unsetenv( "ROCKETNET_SERVER_IPV6_ADDRESS" );
#endif
    return cleared || cleared_server || cleared_client || cleared_ipv4 || cleared_ipv6;
}

// --run <case> runs one named case of the list above and nothing else.
static int source_audit_case_named( const char * name )
{
    const size_t count = sizeof( source_audit_cases ) / sizeof( source_audit_cases[0] );
    for ( size_t i = 0; i < count; i++ )
        if ( strcmp( source_audit_cases[i], name ) == 0 )
            return 1;
    return 0;
}

int main( int argc, char ** argv )
{
    if ( source_audit_clear_environment() )
    {
        printf( "check failed: could not clear the audit environment\n" );
        fflush( stdout );
        return 1;
    }

    if ( argc == 3 && strcmp( argv[1], "--run" ) == 0 )
    {
        if ( !source_audit_case_named( argv[2] ) )
        {
            printf( "source audit: no case named %s\n", argv[2] );
            fflush( stdout );
            return 1;
        }
        const int rc = rocketnet_unit_tests_run_named( argv[2] );
        printf( "source audit case %s %s (rc=%d)\n", argv[2], rc == 0 ? "passed" : "failed", rc );
        fflush( stdout );
        return rc == 0 ? 0 : 1;
    }
    if ( argc != 1 )
    {
        printf( "usage: source_audit [--run <case>]\n" );
        fflush( stdout );
        return 1;
    }

    const size_t count = sizeof( source_audit_cases ) / sizeof( source_audit_cases[0] );
    int failed = 0;
    for ( size_t i = 0; i < count; i++ )
    {
        const int rc = rocketnet_unit_tests_run_named( source_audit_cases[i] );
        if ( rc != 0 )
        {
            printf( "source audit case %s failed (rc=%d)\n", source_audit_cases[i], rc );
            fflush( stdout );
            failed++;
        }
    }
    if ( failed == 0 )
    {
        printf( "\n*** SOURCE AUDIT PASSED ***\n\n" );
        fflush( stdout );
    }
    return failed ? 1 : 0;
}

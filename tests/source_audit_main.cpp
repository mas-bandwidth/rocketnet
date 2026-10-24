/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The source-audit binary. The four repository text audits that used to run
// as unit cases read source, header and doc files through
// ROCKETNET_SOURCE_DIR; they are source-policy checks, not unit logic, so
// they live in their own static binary here, which reads files only, opens no
// socket and starts no thread. CI runs it on a Linux leg beside
// tools/check_client_server_words.py. A case keeps its body and checks
// unchanged; only where it runs changed.

#include "test_unit_registry.h"

#include <stdio.h>
#include <stdlib.h>

static const char * const source_audit_cases[] =
{
    "test_client_server_say_messages_not_packets",
    "test_config_env_every_name_read_is_documented",
    "test_counters_every_counter_is_named_by_a_test",
    "test_xdp_transport_host_every_counter_has_a_case"
};

int main( void )
{
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

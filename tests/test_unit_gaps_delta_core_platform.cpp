/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Mutation gaps (delta, core): rocketnet_platform_socket_error_text
// (sdk/rocketnet_platform.cpp), the one form every socket failure line uses.
// Each error the table names prints its number and its name, the table's
// first and last entries included; a number the table does not name prints
// its number and "?". A pure function of its argument: no socket, no errno
// read, no real time.

#include "test_unit_registry.h"
#include "rocketnet_platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

static bool gaps_platform_error_text_is( int error, const char * name )
{
    char want[128];
    (void) snprintf( want, sizeof( want ), "errno %d %s", error, name );
    const rocketnet_platform_error_text_t text = rocketnet_platform_socket_error_text( error );
    return strcmp( text.text, want ) == 0;
}

// mutation gap: delta sdk/rocketnet_platform.cpp:5633:22:off_by_one:1 (the walk skips the first name)
// mutation gap: delta sdk/rocketnet_platform.cpp:5633:27:relational:> (the walk visits no name)
// mutation gap: delta sdk/rocketnet_platform.cpp:5633:132:arithmetic:-- (the walk visits the first name only)
// mutation gap: delta sdk/rocketnet_platform.cpp:5635:61:relational:!= and 5635:9:negate_condition (the walk takes the first other name)
// mutation gap: delta sdk/rocketnet_platform.cpp:5633:27:relational:<= and 5633:77:arithmetic:* (the walk reads past
//   the table: an unnamed number reads every entry and past it; the ASan unit legs report the overrun)

static void test_gaps_delta_core_platform_socket_error_text_names_every_entry()
{
    const bool first_entry_eaddrinuse_named = gaps_platform_error_text_is( EADDRINUSE, "EADDRINUSE" );
    rocketnet_unit_check( first_entry_eaddrinuse_named );

    const bool second_entry_eaddrnotavail_named = gaps_platform_error_text_is( EADDRNOTAVAIL, "EADDRNOTAVAIL" );
    rocketnet_unit_check( second_entry_eaddrnotavail_named );

    const bool middle_entry_einval_named = gaps_platform_error_text_is( EINVAL, "EINVAL" );
    rocketnet_unit_check( middle_entry_einval_named );

    const bool last_entry_eintr_named = gaps_platform_error_text_is( EINTR, "EINTR" );
    rocketnet_unit_check( last_entry_eintr_named );

    const bool unnamed_number_prints_question_mark = gaps_platform_error_text_is( 1234567, "?" );
    rocketnet_unit_check( unnamed_number_prints_question_mark );

    const bool zero_is_unnamed = gaps_platform_error_text_is( 0, "?" );
    rocketnet_unit_check( zero_is_unnamed );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_platform_socket_error_text_names_every_entry );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

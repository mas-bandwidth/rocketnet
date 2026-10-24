/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The api-polish follow-ups, pinned: the quiet switch is Next SDK named
// (rocketnet_quiet), the last status code is CONNECT_TOKEN_PORT_ZERO at -31,
// and the WARN level's name is "warn", matching its
// enum as every other level name does. The assert callback has one way in,
// rocketnet_assert_function; the internal pointer it writes is
// rn_assert_function, never exported. Every case runs in memory, with no
// clock, sockets or network.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"

#include <string.h>
#include <type_traits>

#define check rocketnet_unit_check

static_assert( std::is_same<decltype( &rocketnet_quiet ), void (*)( bool )>::value, "Next SDK naming: rocketnet_quiet( bool )" );
static_assert( ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO == -31, "the failures are dense down to the last code" );

static int api_polish_followups_count = 0;

static void api_polish_followups_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context; (void) level; (void) message; (void) length;
    api_polish_followups_count++;
}

static int api_polish_followups_asserts = 0;

static void api_polish_followups_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
    api_polish_followups_asserts++;
}

static void test_api_polish_followups()
{
    // rocketnet_quiet suppresses and resumes the one log sink
    const enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    rocketnet_log_function( api_polish_followups_log, NULL );
    api_polish_followups_count = 0;
    rocketnet_quiet( true );
    rocketnet_warn( "quiet" );
    check( api_polish_followups_count == 0 );
    rocketnet_quiet( false );
    rocketnet_warn( "heard" );
    check( api_polish_followups_count == 1 );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );

    // the last code is CONNECT_TOKEN_PORT_ZERO, and one past it is no code
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) -31 ), "CONNECT_TOKEN_PORT_ZERO" ) == 0 );
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) -32 ), "UNKNOWN" ) == 0 );

    // every level name matches its enum: WARN is "warn"
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_NONE ), "NONE" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_ERROR ), "ERROR" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_WARN ), "WARN" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_INFO ), "INFO" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_DEBUG ), "DEBUG" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_SPAM ), "SPAM" ) == 0 );

    // the setter is the one way in: it writes the internal pointer, and NULL restores the default
    rocketnet_assert_function_t original = rn_assert_function;
    rocketnet_assert_function( api_polish_followups_assert );
    check( rn_assert_function == api_polish_followups_assert );
    rocketnet_assert_function( NULL );
    check( rn_assert_function == original );
    check( api_polish_followups_asserts == 0 );
}
REGISTER_UNIT_TEST( test_api_polish_followups );

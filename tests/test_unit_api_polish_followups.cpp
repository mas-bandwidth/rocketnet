/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The api-polish follow-ups, pinned: the quiet switch is Next SDK named
// (rocketnet_quiet), the last status code is TRANSPORT_BIND_REFUSED at -35,
// and the WARN level's name is "WARN", matching its enum as every other level
// name does; "warn", lower case, is the word its log lines carry. The assert
// callback has one way in, rocketnet_assert_function; the internal pointer it
// writes is rn_assert_function, never exported. Every case runs in memory,
// with no clock, sockets or network.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"

#include <stdio.h>
#include <string.h>
#include <type_traits>

#define check rocketnet_unit_check

static_assert( std::is_same<decltype( &rocketnet_quiet ), void ( * )( bool )>::value, "Next SDK naming: rocketnet_quiet( bool )" );
static_assert( ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED == -35, "TRANSPORT_BIND_REFUSED is the last failure code" );

static int api_polish_followups_count = 0;

static void api_polish_followups_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    (void) message;
    (void) length;
    api_polish_followups_count++;
}

static int api_polish_followups_asserts = 0;

static void api_polish_followups_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
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

    // the last code is TRANSPORT_BIND_REFUSED, and one past it is no code

    check( strcmp( rocketnet_status_name( (rocketnet_status_t) -35 ), "TRANSPORT_BIND_REFUSED" ) == 0 );
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) -36 ), "UNKNOWN" ) == 0 );

    // every level name matches its enum: WARN is "WARN"; the word a log line
    // carries is the lower-case "warn"

    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_NONE ), "NONE" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_ERROR ), "ERROR" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_WARN ), "WARN" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_INFO ), "INFO" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_DEBUG ), "DEBUG" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_SPAM ), "SPAM" ) == 0 );
#if ROCKETNET_DEV
    check( strcmp( rocketnet_log_level_word_for_test( ROCKETNET_LOG_LEVEL_WARN ), "warn" ) == 0 );
#endif // #if ROCKETNET_DEV

    // the setter is the one way in: it writes the internal pointer, and NULL restores the default

    rocketnet_assert_function_t original = rn_assert_function;
    rocketnet_assert_function( api_polish_followups_assert );
    check( rn_assert_function == api_polish_followups_assert );
    rocketnet_assert_function( NULL );
    check( rn_assert_function == original );
    check( api_polish_followups_asserts == 0 );
}
REGISTER_UNIT_TEST( test_api_polish_followups );

// Every public entry point that takes an enum by value, called from C
// (tests/c_caller.c) with an int outside its enum, -1000 and 1000: a name
// lookup says "UNKNOWN", the log level in force stays, the one-record limit
// is 0, and on a connected pair a start is refused with NULL and the room
// query answers false. None of these is API misuse, none asserts, and the
// connection carries on.

extern "C" void rocketnet_test_c_caller_log_level( int level );
extern "C" const char * rocketnet_test_c_caller_log_level_name( int level );
extern "C" const char * rocketnet_test_c_caller_status_name( int status );
extern "C" const char * rocketnet_test_c_caller_client_state_name( int state );
extern "C" const char * rocketnet_test_c_caller_disconnect_reason_name( int reason );
extern "C" int rocketnet_test_c_caller_max_unfragmented_message_bytes( int reliability, int channel );
extern "C" uint8_t * rocketnet_test_c_caller_client_start_message( struct rocketnet_client_t * client, int channel, int reliability, int max_bytes );
extern "C" uint8_t * rocketnet_test_c_caller_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, int reliability, int max_bytes );
extern "C" bool rocketnet_test_c_caller_client_can_send_message( const struct rocketnet_client_t * client, int channel, int reliability, int max_bytes );
extern "C" bool rocketnet_test_c_caller_server_can_send_message( const struct rocketnet_server_t * server, int client_index, int channel, int reliability, int max_bytes );

#if ROCKETNET_DEV
#include "public_step_pair.h"
#endif // #if ROCKETNET_DEV

static void test_api_polish_followups_c_caller_enums_out_of_range()
{
    const int out_of_range[] = { -1000, 1000 };

    rocketnet_assert_function( api_polish_followups_assert );
    api_polish_followups_asserts = 0;

    const enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );

    const uint64_t before = rocketnet_api_misuse_count();

    for ( size_t i = 0; i < sizeof( out_of_range ) / sizeof( out_of_range[0] ); i++ )
    {
        const int value = out_of_range[i];

        rocketnet_test_c_caller_log_level( value );
        check( rocketnet_log_level_current() == ROCKETNET_LOG_LEVEL_WARN );
        check( strcmp( rocketnet_test_c_caller_log_level_name( value ), "UNKNOWN" ) == 0 );
        check( strcmp( rocketnet_test_c_caller_status_name( value ), "UNKNOWN" ) == 0 );
        check( strcmp( rocketnet_test_c_caller_client_state_name( value ), "UNKNOWN" ) == 0 );
        check( strcmp( rocketnet_test_c_caller_disconnect_reason_name( value ), "UNKNOWN" ) == 0 );
        check( rocketnet_test_c_caller_max_unfragmented_message_bytes( value, 0 ) == 0 );
    }

    // the wrappers pass an in-range value through unchanged

    check( strcmp( rocketnet_test_c_caller_status_name( ROCKETNET_STATUS_OK ), "OK" ) == 0 );
    check( strcmp( rocketnet_test_c_caller_client_state_name( ROCKETNET_CLIENT_STATE_CONNECTED ), "CONNECTED" ) == 0 );
    check( strcmp( rocketnet_test_c_caller_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_NONE ), "NONE" ) == 0 );
    check( rocketnet_test_c_caller_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE, 0 ) ==
           rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE, 0 ) );

    check( rocketnet_api_misuse_count() == before );
    rocketnet_log_level( saved );

#if ROCKETNET_DEV

    public_step_identity_t identity;
    public_step_identity( &identity );

    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity );
    public_step_connect( &pair );

    const uint64_t connected = rocketnet_api_misuse_count();

    for ( size_t i = 0; i < sizeof( out_of_range ) / sizeof( out_of_range[0] ); i++ )
    {
        const int value = out_of_range[i];

        check( rocketnet_test_c_caller_client_start_message( pair.client, 0, value, 4 ) == NULL );
        check( rocketnet_test_c_caller_server_start_message( pair.server, 0, 0, value, 4 ) == NULL );
        check( !rocketnet_test_c_caller_client_can_send_message( pair.client, 0, value, 4 ) );
        check( !rocketnet_test_c_caller_server_can_send_message( pair.server, 0, 0, value, 4 ) );
    }

    check( rocketnet_api_misuse_count() == connected );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair.server, 0 ) );

    // an in-range reliability through the same wrappers is given room

    check( rocketnet_test_c_caller_client_can_send_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) );
    check( rocketnet_test_c_caller_server_can_send_message( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) );
    check( rocketnet_test_c_caller_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) != NULL );
    rocketnet_client_abort_message( pair.client );
    check( rocketnet_test_c_caller_server_start_message( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) != NULL );
    rocketnet_server_abort_message( pair.server );
    check( rocketnet_api_misuse_count() == connected );

    public_step_pair_destroy( &pair );
    rocketnet_quiet( false );

#endif // #if ROCKETNET_DEV

    check( api_polish_followups_asserts == 0 );
    rocketnet_assert_function( NULL );
}
REGISTER_UNIT_TEST( test_api_polish_followups_c_caller_enums_out_of_range );

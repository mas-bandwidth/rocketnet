/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The preformatted, FFI-safe log callback (docs/ERRORS-AND-LOGGING.md 3.2
// and 3.3): a callback installed with rocketnet_set_log_callback receives each
// line formatted once, with the exact byte length clamped to 1023 and
// message[length] == '\0'; quiet and level are read before formatting; and
// the preformatted callback and the legacy rocketnet_log_function coexist with
// the last registration winning and NULL restoring the default sink.
//
// Every case runs in memory, with no clock, sockets or network.

#include "test_unit_registry.h"

#include "rocketnet.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// --- shared capture state -------------------------------------------------

static char g_buf[4096];
static void * g_ctx = NULL;
static int g_level = -1;
static size_t g_len = 0;

static void capture_callback( void * context, int level, const char * message, size_t length )
{
    g_ctx = context;
    g_level = level;
    g_len = length < sizeof( g_buf ) ? length : sizeof( g_buf ) - 1;
    memcpy( g_buf, message, g_len );
    g_buf[g_len] = '\0';
}

static void fill_string( char * out, size_t len )
{
    for ( size_t i = 0; i < len; i++ )
        out[i] = 'x';
    out[len] = '\0';
}

// --- contract: clamp, terminator, context, level --------------------------

static void log_callback_preformatted_contract()
{
    const char * ctx = "the-context";
    int saved = rocketnet_get_log_level();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_set_log_callback( capture_callback, (void*) ctx );

    char empty[1];
    empty[0] = '\0';
    rocketnet_error( "%s", empty );
    check( g_ctx == (void*) ctx );
    check( g_level == ROCKETNET_LOG_LEVEL_ERROR );
    check( g_len == 0 );
    check( g_buf[0] == '\0' );

    char str1023[1024];
    fill_string( str1023, 1023 );
    rocketnet_error( "%s", str1023 );
    check( g_ctx == (void*) ctx );
    check( g_len == 1023 );
    check( g_buf[1023] == '\0' );
    check( memcmp( g_buf, str1023, 1023 ) == 0 );

    char str1024[1025];
    fill_string( str1024, 1024 );
    rocketnet_error( "%s", str1024 );
    check( g_len == 1023 );
    check( g_buf[1023] == '\0' );
    check( memcmp( g_buf, str1024, 1023 ) == 0 );

    char str2048[2049];
    fill_string( str2048, 2048 );
    rocketnet_error( "%s", str2048 );
    check( g_len == 1023 );
    check( g_buf[1023] == '\0' );
    check( memcmp( g_buf, str2048, 1023 ) == 0 );

    rocketnet_set_log_callback( NULL, NULL );
    rocketnet_log_level( saved );
}

// --- filtering happens before formatting ----------------------------------

static int g_count = 0;
static int g_last_level = -1;
static char g_msg[32768];
static size_t g_msglen = 0;

static void counting_callback( void * context, int level, const char * message, size_t length )
{
    (void) context;
    g_count++;
    g_last_level = level;
    g_msglen = length < sizeof( g_msg ) ? length : sizeof( g_msg ) - 1;
    memcpy( g_msg, message, g_msglen );
    g_msg[g_msglen] = '\0';
}

static void log_callback_filters_before_formatting()
{
    int saved = rocketnet_get_log_level();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_set_log_callback( counting_callback, NULL );
    g_count = 0;

    rocketnet_debug( "hidden debug %d", 42 );
    check( g_count == 0 );
    rocketnet_info( "hidden info" );
    check( g_count == 0 );
    rocketnet_warn( "hidden warn" );
    check( g_count == 0 );

    // a format argument whose evaluation is visible: the token survives only
    // when the call is not filtered before formatting and dispatch
    const char * visible = "visible-token";
    rocketnet_error( "shows %s", visible );
    check( g_count == 1 );
    check( g_last_level == ROCKETNET_LOG_LEVEL_ERROR );
    check( strcmp( g_msg, "shows visible-token" ) == 0 );

    rocketnet_quiet( true );
    rocketnet_error( "quiet-hidden %s", visible );
    check( g_count == 1 );
    rocketnet_quiet( false );
    rocketnet_error( "after-quiet %s", visible );
    check( g_count == 2 );
    check( g_msglen > 0 );

    rocketnet_set_log_callback( NULL, NULL );
    rocketnet_log_level( saved );
}

// --- last registration wins, NULL restores default ------------------------

static int g_legacy_count = 0;
static int g_pf_count = 0;

static void legacy_callback( int level, const char * format, ... )
{
    (void) level;
    (void) format;
    g_legacy_count++;
}

static void pf_callback( void * context, int level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    (void) message;
    (void) length;
    g_pf_count++;
}

static void log_callback_and_legacy_function_last_wins()
{
    int saved = rocketnet_get_log_level();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );

    rocketnet_log_function( legacy_callback );
    g_legacy_count = g_pf_count = 0;
    rocketnet_error( "via legacy" );
    check( g_legacy_count == 1 );
    check( g_pf_count == 0 );

    rocketnet_set_log_callback( pf_callback, NULL );
    rocketnet_error( "via preformatted" );
    check( g_pf_count == 1 );
    check( g_legacy_count == 1 );

    rocketnet_log_function( legacy_callback );
    rocketnet_error( "legacy again" );
    check( g_legacy_count == 2 );
    check( g_pf_count == 1 );

    rocketnet_set_log_callback( NULL, NULL );
    // the default sink writes to stdout, and a passing suite prints only names
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_error( "default sink" );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    check( g_legacy_count == 2 );
    check( g_pf_count == 1 );

    rocketnet_log_function( legacy_callback );
    rocketnet_log_function( NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_error( "default again" );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    check( g_legacy_count == 2 );
    check( g_pf_count == 1 );

    rocketnet_set_log_callback( NULL, NULL );
    rocketnet_log_level( saved );
}

REGISTER_UNIT_TEST( log_callback_preformatted_contract );
REGISTER_UNIT_TEST( log_callback_filters_before_formatting );
REGISTER_UNIT_TEST( log_callback_and_legacy_function_last_wins );

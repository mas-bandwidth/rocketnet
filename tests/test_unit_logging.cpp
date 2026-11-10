/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The preformatted, FFI-safe log callback (docs/ERRORS-AND-LOGGING.md 3.2
// and 3.3): a callback installed with rocketnet_log_function receives each
// line formatted once, with the exact byte length clamped to 1023 and
// message[length] == '\0'; quiet and level are read before formatting; and
// of two callbacks the last registration wins, with NULL restoring the
// default sink.
//
// Every case runs in memory, with no clock, sockets or network.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#include "public_step_pair.h"

#if ROCKETNET_DEV
int rocketnet_server_transport_disconnect_burst_key_for_test( struct rocketnet_server_transport_t * server_transport,
                                                              int index, uint8_t * out, int out_bytes, int * head, int * count );
#endif // #if ROCKETNET_DEV

// --- shared capture state -------------------------------------------------

static char g_buf[4096];
static void * g_ctx = NULL;
static int g_level = -1;
static size_t g_len = 0;

static void capture_callback( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
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
    {
        out[i] = 'x';
    }
    out[len] = '\0';
}

// --- contract: clamp, terminator, context, level --------------------------

static void test_log_callback_preformatted_contract()
{
    static char ctx[] = "the-context";
    enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( capture_callback, ctx );

    char empty[1];
    empty[0] = '\0';
    rocketnet_error( "%s", empty );
    check( g_ctx == ctx );
    check( g_level == ROCKETNET_LOG_LEVEL_ERROR );
    check( g_len == 0 );
    check( g_buf[0] == '\0' );

    char str1023[1024];
    fill_string( str1023, 1023 );
    rocketnet_error( "%s", str1023 );
    check( g_ctx == ctx );
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

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );
}

// --- filtering happens before formatting ----------------------------------

static int g_count = 0;
static int g_last_level = -1;
static char g_msg[32768];
static size_t g_msglen = 0;

static void counting_callback( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    g_count++;
    g_last_level = level;
    g_msglen = length < sizeof( g_msg ) ? length : sizeof( g_msg ) - 1;
    memcpy( g_msg, message, g_msglen );
    g_msg[g_msglen] = '\0';
}

static void test_log_callback_filters_before_formatting()
{
    enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_log_function( counting_callback, NULL );
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

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );
}

// --- last registration wins, NULL restores default ------------------------

static int g_first_count = 0;
static int g_second_count = 0;

static void first_callback( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    (void) level;
    (void) message;
    g_first_count++;
}

static void second_callback( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    (void) message;
    (void) length;
    g_second_count++;
}

static void test_log_callback_last_registration_wins()
{
    enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );

    rocketnet_log_function( first_callback, NULL );
    g_first_count = g_second_count = 0;
    rocketnet_error( "via first" );
    check( g_first_count == 1 );
    check( g_second_count == 0 );

    rocketnet_log_function( second_callback, NULL );
    rocketnet_error( "via second" );
    check( g_second_count == 1 );
    check( g_first_count == 1 );

    rocketnet_log_function( first_callback, NULL );
    rocketnet_error( "first again" );
    check( g_first_count == 2 );
    check( g_second_count == 1 );

    rocketnet_log_function( NULL, NULL );

    // the default sink writes to stdout, and a passing suite prints only names

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_error( "default sink" );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    check( g_first_count == 2 );
    check( g_second_count == 1 );

    rocketnet_log_function( first_callback, NULL );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_error( "default again" );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    check( g_first_count == 2 );
    check( g_second_count == 1 );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );
}

// an unknown level reaches the sink as passed, and the default sink labels
// it "???" (rocketnet_printf's contract in sdk/rocketnet_layers.h)

static void test_log_unknown_level_reaches_the_sink_and_reads_unknown()
{
    enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( capture_callback, NULL );

    g_level = 99;
    rocketnet_printf( -1, "below error" );
    check( g_level == -1 );
    check( strcmp( g_buf, "below error" ) == 0 );

    rocketnet_printf( INT_MIN, "minimum int level" );
    check( g_level == INT_MIN );
    check( strcmp( g_buf, "minimum int level" ) == 0 );

    // Unknown positive levels still obey the configured SPAM ceiling.

    g_level = 99;
    rocketnet_printf( ROCKETNET_LOG_LEVEL_SPAM + 1, "above spam" );
    check( g_level == 99 );
    rocketnet_printf( INT_MAX, "maximum int level" );
    check( g_level == 99 );

    // Every int can cross the enum-typed API without becoming an invalid
    // enum load. Unknown settings are still refused, and names stay unknown.

    const int unknown_levels[] = { INT_MIN, -1, ROCKETNET_LOG_LEVEL_SPAM + 1, INT_MAX };
    for ( size_t i = 0; i < sizeof( unknown_levels ) / sizeof( unknown_levels[0] ); i++ )
    {
        const enum rocketnet_log_level_t level = (enum rocketnet_log_level_t) unknown_levels[i];
        check( strcmp( rocketnet_log_level_name( level ), "UNKNOWN" ) == 0 );
        rocketnet_log_level( level );
        check( rocketnet_log_level_current() == ROCKETNET_LOG_LEVEL_SPAM );
    }

    rocketnet_log_function( NULL, NULL );

    // Exercise the default sink's enum load as well as the installed callback.

    rocketnet_printf( -1, "default sink below error" );
    rocketnet_printf( INT_MIN, "default sink minimum int level" );
    rocketnet_log_level( saved );

#if ROCKETNET_DEV
    check( strcmp( rocketnet_log_level_word_for_test( -1 ), "???" ) == 0 );
    check( strcmp( rocketnet_log_level_word_for_test( INT_MIN ), "???" ) == 0 );
    check( strcmp( rocketnet_log_level_word_for_test( INT_MAX ), "???" ) == 0 );
    check( strcmp( rocketnet_log_level_word_for_test( ROCKETNET_LOG_LEVEL_SPAM + 1 ), "???" ) == 0 );
    check( strcmp( rocketnet_log_level_word_for_test( ROCKETNET_LOG_LEVEL_ERROR ), "error" ) == 0 );
#endif // #if ROCKETNET_DEV
}

// --- a log callback that disconnects the slot being disconnected ----------

static rocketnet_server_t * g_reentry_server = NULL;
static int g_reentry_slot = 0;
static int g_reentry_logs = 0;
static int g_reentry_calls = 0;

static void disconnect_reentry_callback( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;

    static const char prefix[] = "server disconnected client ";
    if ( length < sizeof( prefix ) - 1 || memcmp( message, prefix, sizeof( prefix ) - 1 ) != 0 )
        return;

    g_reentry_logs++;
    if ( g_reentry_calls != 0 )
        return;

    g_reentry_calls++;
    rocketnet_server_disconnect_client( g_reentry_server, g_reentry_slot );
}

static void reentry_pair_create( public_step_pair_t * pair )
{
    (void) &public_step_pair_create;

    public_step_identity_t identity;
    public_step_identity( &identity );

    rocketnet_server_config_t server_config;
    public_step_server_config( &server_config, &identity );
    server_config.disconnect_packets_per_update = 1;

    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    rocketnet_quiet( 1 );

    pair->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( pair->server != NULL );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    public_step_token_string( pair->server, &identity, token );

    rocketnet_client_config_t client_config;
    public_step_client_config( &client_config );
    pair->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( pair->client != NULL );
    check( rocketnet_client_connect( pair->client, token ) == ROCKETNET_STATUS_OK );

    public_step_connect( pair );
}

static void test_log_callback_disconnect_reentry_does_not_repeat()
{
    public_step_pair_t pair;
    reentry_pair_create( &pair );

    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( pair.server ) );
    check( transport != NULL );

    enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );

    rocketnet_quiet( 0 );

    g_reentry_server = pair.server;
    g_reentry_slot = 0;
    g_reentry_logs = 0;
    g_reentry_calls = 0;
    rocketnet_log_function( disconnect_reentry_callback, NULL );

    rocketnet_server_disconnect_client( pair.server, g_reentry_slot );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );

    check( g_reentry_calls == 1 );
    check( g_reentry_logs == 1 );
    check( !rocketnet_server_client_connected( pair.server, g_reentry_slot ) );

#if ROCKETNET_DEV
    uint8_t key[ROCKETNET_KEY_BYTES];
    int head = -1;
    int count = -1;
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, key,
                                                                     (int) sizeof( key ), &head, &count ) == 1 );
    check( count == 1 );
#endif // #if ROCKETNET_DEV

    g_reentry_server = NULL;
    public_step_pair_destroy( &pair );
}

REGISTER_UNIT_TEST( test_log_callback_preformatted_contract );
REGISTER_UNIT_TEST( test_log_unknown_level_reaches_the_sink_and_reads_unknown );
REGISTER_UNIT_TEST( test_log_callback_filters_before_formatting );
REGISTER_UNIT_TEST( test_log_callback_last_registration_wins );
REGISTER_UNIT_TEST( test_log_callback_disconnect_reentry_does_not_repeat );

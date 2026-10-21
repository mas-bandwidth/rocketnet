/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The v1.0.0-preview-1 API rulings, pinned. Each ruling has a compile-time check that
// the old form fails to compile (a static_assert on the declared type, or an
// #error on a removed name), and a run-time case where the old form returned
// a different value. Every case runs in memory, with no clock, sockets or
// network; the one time argument is 1000.0.
//
//   1. every call that can fail returns enum rocketnet_status_t; a config's
//      first field is config_bytes, set by the inline default_config
//   2. the dev connect-token writer takes connect_token_bytes and returns a status
//   3. typed enums replace the #defines, named ROCKETNET_<SET>_<VALUE>; the
//      updates return void, and the client state carries the disconnect reason
//   4. pure questions return bool; step_driven is an int flag (0/1)
//   5. Next SDK naming: rocketnet_log_level, rocketnet_assert_function
//   6. one log sink: rocketnet_log_function; the log level is an enum
//   7. every lookup ends in _name and returns "unknown" for an unknown value
//   8. memory and buffer sizes are size_t; create takes the config alone
//   9. a NULL client's state is ROCKETNET_CLIENT_STATE_DISCONNECTED, counted as misuse

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "test/token_string.h"

#include <stdio.h>
#include <string.h>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define check rocketnet_unit_check

// --- the removed names stay removed ----------------------------------------

#if defined( ROCKETNET_UNRELIABLE ) || defined( ROCKETNET_RELIABLE_ORDERED ) || defined( ROCKETNET_RECEIPT_ACKED ) || defined( ROCKETNET_MESSAGE_OK )
#error "ruling 3: the reliability, receipt and update #defines are enums now"
#endif
#if defined( ROCKETNET_MESSAGE_ERROR_NONE ) || defined( ROCKETNET_ERROR_MESSAGE_NOT_RELEASED ) || defined( ROCKETNET_LOG_LEVEL_SPAM )
#error "ruling 3 and 6: disconnect reasons, update results and log levels are enums now"
#endif

template <typename A, typename B> struct rn_same { static const bool value = std::is_same<A, B>::value; };

// --- ruling 1: status returns ------------------------------------------------

static_assert( rn_same<decltype( rocketnet_init( NULL, NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: init" );
static_assert( rn_same<decltype( rocketnet_term() ), rocketnet_status_t>::value, "ruling 1: term" );
static_assert( rn_same<decltype( rocketnet_generate_backend_keypair( NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: backend keypair" );
static_assert( rn_same<decltype( rocketnet_generate_backend_keypair_from_seed( NULL, NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: seeded keypair" );
static_assert( rn_same<decltype( rocketnet_generate_server_keypair( NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: server keypair" );
static_assert( rn_same<decltype( rocketnet_write_production_connect_token( NULL, NULL, 0, NULL, 0, 0, 0, 0, 0, NULL, NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: production token" );
static_assert( rn_same<decltype( rocketnet_client_default_config( NULL ) ), rocketnet_status_t>::value, "ruling 1: client default config" );
static_assert( rn_same<decltype( rocketnet_client_default_config_internal( NULL ) ), rocketnet_status_t>::value, "ruling 1: client default config internal" );
static_assert( rn_same<decltype( rocketnet_server_default_config( NULL ) ), rocketnet_status_t>::value, "ruling 1: server default config" );
static_assert( rn_same<decltype( rocketnet_server_default_config_internal( NULL ) ), rocketnet_status_t>::value, "ruling 1: server default config internal" );
static_assert( offsetof( rocketnet_client_config_t, config_bytes ) == 0, "ruling 1: config_bytes is the client config's first field" );
static_assert( offsetof( rocketnet_server_config_t, config_bytes ) == 0, "ruling 1: config_bytes is the server config's first field" );
static_assert( rn_same<decltype( ( (rocketnet_client_config_t *) 0 )->config_bytes ), size_t>::value, "ruling 1 and 8: client config_bytes is size_t" );
static_assert( rn_same<decltype( ( (rocketnet_server_config_t *) 0 )->config_bytes ), size_t>::value, "ruling 1 and 8: server config_bytes is size_t" );
static_assert( rn_same<decltype( rocketnet_client_set_send_kbps( NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: client set_send_kbps" );
static_assert( rn_same<decltype( rocketnet_server_set_send_kbps( NULL, 0, 0 ) ), rocketnet_status_t>::value, "ruling 1: server set_send_kbps" );
static_assert( rn_same<decltype( rocketnet_client_connect( NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: connect" );
static_assert( rn_same<decltype( rocketnet_server_start( NULL ) ), rocketnet_status_t>::value, "ruling 1: start" );
static_assert( rn_same<decltype( &rocketnet_status_name ), const char * (*)( rocketnet_status_t )>::value, "ruling 1 and 7: status_name takes the enum" );

// the published development keys, set by hand after the defaults
static void api_polish_dev_server_config( struct rocketnet_server_config_t * config )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
}

static void * api_polish_malloc( void * context, size_t bytes ) { (void) context; return malloc( bytes ); }
static void api_polish_free( void * context, void * p ) { (void) context; free( p ); }

// the worker process is initialized before a unit test runs: a second init
// says why it refused, where the old bool said only false
static void test_api_polish_status_returns()
{
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_ALREADY_INITIALIZED );
    check( rocketnet_init( NULL, api_polish_malloc, api_polish_free ) == ROCKETNET_STATUS_ALREADY_INITIALIZED );
    check( rocketnet_init( NULL, api_polish_malloc, NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // a live allocation refuses term, with the reason, and nothing changes
    void * held = rocketnet_malloc( NULL, 64 );
    check( held != NULL );
    check( rocketnet_term() == ROCKETNET_STATUS_LIVE_ALLOCATIONS );
    rocketnet_free( NULL, held );
    check( rocketnet_library_initialized() );

    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_server_keypair( public_key, secret_key ) == ROCKETNET_STATUS_OK );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    const char * addresses[] = { "127.0.0.1:40000" };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( backend_secret, public_key, 0x1234ULL, addresses, 1, 7, 1000, 2000, 10, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_write_production_connect_token( backend_secret, public_key, 0x1234ULL, addresses, 1, 7, 1000, 2000, 10, NULL, token, sizeof( token ) - 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // the default config and create's status: OK is 0, where the old form's success was 1
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    check( client_config.config_bytes == sizeof( client_config ) );
    enum rocketnet_status_t create_status = ROCKETNET_TEST_STATUS_UNSET;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
    client_config.num_channels = 0;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    check( rocketnet_client_create( NULL, &client_config, &create_status ) == NULL );
    check( create_status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    // a config never filled is refused by name
    client_config.config_bytes = 0;
    check( rocketnet_client_default_config_internal( &client_config ) == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    struct rocketnet_server_config_t server_config;
    api_polish_dev_server_config( &server_config );
    server_config.max_clients = 1; // This case checks status codes, not server capacity.
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );
}
REGISTER_UNIT_TEST( test_api_polish_status_returns );

// --- the dev token minter writes the public token string ---------------------

// The unit suite links the release archive, where the development minter is
// compiled out; the development shared library built beside it carries it, so
// the minter is loaded from there (as test_unit_connect_token_vectors.cpp
// does) and called through the public signature: it writes exactly the
// NUL-terminated string into ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES and
// names every refusal.
typedef rocketnet_status_t ( *api_polish_dev_minter_t )( const char *, const char *, const uint8_t *, int, int, uint64_t, uint64_t, uint64_t, const uint8_t *, char * );

#if ROCKETNET_DEVELOPMENT
static_assert( rn_same<decltype( &rocketnet_create_dev_connect_token ), api_polish_dev_minter_t>::value, "dev token minter signature" );
#endif // #if ROCKETNET_DEVELOPMENT

// the development library keeps its own log state, apart from the sink the
// suite installs in the archive it links, so the test captures the refusal
// line in that library
typedef void ( *api_polish_dev_set_log_t )( rocketnet_log_function_t, void * );
static api_polish_dev_set_log_t api_polish_dev_set_log = NULL;
static int api_polish_dev_error_lines = 0;

static void api_polish_dev_log_capture( void *, enum rocketnet_log_level_t level, const char *, size_t )
{
    if ( level == ROCKETNET_LOG_LEVEL_ERROR )
        api_polish_dev_error_lines++;
}

static api_polish_dev_minter_t api_polish_dev_minter()
{
    api_polish_dev_minter_t minter = NULL;
#if defined(_WIN32)
    HMODULE library = LoadLibraryA( ROCKETNET_DEV_LIBRARY_PATH );
    check( library != NULL );
    minter = (api_polish_dev_minter_t) (void *) GetProcAddress( library, "rocketnet_create_dev_connect_token" );
    api_polish_dev_set_log = (api_polish_dev_set_log_t) (void *) GetProcAddress( library, "rocketnet_log_function" );
#else
    void * library = dlopen( ROCKETNET_DEV_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL );
    check( library != NULL );
    minter = (api_polish_dev_minter_t) dlsym( library, "rocketnet_create_dev_connect_token" );
    api_polish_dev_set_log = (api_polish_dev_set_log_t) dlsym( library, "rocketnet_log_function" );
#endif
    check( minter != NULL );
    check( api_polish_dev_set_log != NULL );
    return minter;
}

static void test_api_polish_dev_token_string()
{
    api_polish_dev_minter_t create_dev = api_polish_dev_minter();
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    memset( server_key, 0x42, sizeof( server_key ) );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES + 1];
    memset( token, 0x5A, sizeof( token ) );
    api_polish_dev_error_lines = 0;
    api_polish_dev_set_log( api_polish_dev_log_capture, NULL );

    // no address at all: refused and named, nothing written
    check( create_dev( NULL, NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( api_polish_dev_error_lines == 1 );
    check( token[0] == 0x5A && token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1] == 0x5A );

    // an address that does not parse
    check( create_dev( "not an address", NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( token[0] == 0x5A );

    // a dead lifetime is refused
    check( create_dev( "127.0.0.1:40000", NULL, server_key, 0, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( token[0] == 0x5A );

    // the string fills the public buffer exactly, NUL-terminated, nothing past it
    check( create_dev( "127.0.0.1:40000", NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_OK );
    check( strlen( token ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    check( token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES] == 0x5A );
    api_polish_dev_set_log( NULL, NULL );
}
REGISTER_UNIT_TEST( test_api_polish_dev_token_string );

// --- ruling 3: typed enums, values unchanged ----------------------------------

static_assert( ROCKETNET_RELIABILITY_UNRELIABLE == 0 && ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED == 1 &&
               ROCKETNET_RELIABILITY_RELIABLE == 2 && ROCKETNET_RELIABILITY_RELIABLE_ORDERED == 3 &&
               ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED == 4 && ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT == 5 &&
               ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT == 6 && ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT == 7, "ruling 3: reliability values" );
static_assert( ROCKETNET_RECEIPT_STATUS_NONE == 0 && ROCKETNET_RECEIPT_STATUS_ACKED == 1 && ROCKETNET_RECEIPT_STATUS_LOSS == -1, "ruling 3: receipt values, the loss negative" );
static_assert( ROCKETNET_DISCONNECT_REASON_NONE == 0 && ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH == -1 &&
               ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION == -2 &&
               ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT == -3 && ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED == -4 &&
               ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED == -5, "disconnect reasons: dense, failures below zero" );
static_assert( ROCKETNET_UPDATE_OK == 0 && ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED == -1, "ruling 3: the internal message layer's update values" );
static_assert( rn_same<decltype( rocketnet_client_update( NULL, 1000.0 ) ), void>::value, "ruling 3: client update returns void" );
static_assert( rn_same<decltype( rocketnet_server_update( NULL, 1000.0 ) ), void>::value, "ruling 3: server update returns void" );
static_assert( rn_same<decltype( rocketnet_server_disconnect_reason( NULL, 0 ) ), rocketnet_disconnect_reason_t>::value, "ruling 3: server reason" );
static_assert( ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED == -8 && ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT == -9 &&
               ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION == -10 && ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH == -11 &&
               ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED == -12, "ruling 3: the connection-ended client states" );
static_assert( rn_same<decltype( &rocketnet_client_index ), int (*)( const rocketnet_client_t * )>::value, "ruling 3: the client index is int" );
static_assert( rn_same<decltype( &rocketnet_server_client_connected ), bool (*)( const rocketnet_server_t *, int )>::value, "ruling 3: per-client calls take an int index" );
static_assert( rn_same<decltype( &rocketnet_server_finish_message ), uint32_t (*)( rocketnet_server_t *, int )>::value, "ruling 3: server finish takes no destination" );
static_assert( rn_same<decltype( &rocketnet_server_abort_message ), void (*)( rocketnet_server_t * )>::value, "ruling 3: server abort takes no destination" );
static_assert( rn_same<decltype( &rocketnet_client_start_message ), uint8_t * (*)( rocketnet_client_t *, int, rocketnet_reliability_t, int )>::value, "ruling 3: client start" );
static_assert( rn_same<decltype( &rocketnet_server_start_message ), uint8_t * (*)( rocketnet_server_t *, uint64_t, int, rocketnet_reliability_t, int )>::value, "ruling 3: server start" );

// --- ruling 4: pure questions are bool ----------------------------------------

static_assert( rn_same<decltype( rocketnet_server_running( NULL ) ), bool>::value, "ruling 4: running" );
static_assert( rn_same<decltype( rocketnet_server_client_connected( NULL, 0 ) ), bool>::value, "ruling 4: client connected" );
static_assert( rn_same<decltype( ( (rocketnet_client_testing_t *) 0 )->step_driven ), bool>::value, "step_driven is a bool flag" );

static void test_api_polish_bool_field_abi()
{
    // the flag is a bool; send_kbps is an int
    check( sizeof( ( (rocketnet_client_testing_t *) 0 )->step_driven ) == sizeof( bool ) );
    check( sizeof( ( (rocketnet_client_config_t *) 0 )->send_kbps ) == sizeof( int ) );
    struct rocketnet_client_test_config_t config;
    check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.step_driven == 0 );
}
REGISTER_UNIT_TEST( test_api_polish_bool_field_abi );

// --- ruling 5 and 6: set_/get_ naming, one log sink, an enum level -------------

static_assert( rn_same<decltype( &rocketnet_log_level ), void (*)( rocketnet_log_level_t )>::value, "ruling 5: rocketnet_log_level" );
static_assert( rn_same<decltype( rocketnet_log_level_current() ), rocketnet_log_level_t>::value, "ruling 5: the internal rocketnet_log_level_current" );
static_assert( rn_same<decltype( &rocketnet_assert_function ), void (*)( rocketnet_assert_function_t )>::value, "ruling 5: rocketnet_assert_function" );
static_assert( rn_same<rocketnet_log_function_t, void (*)( void *, rocketnet_log_level_t, const char *, size_t )>::value, "ruling 6: the callback takes the enum level" );

static int g_polish_calls = 0;
static rocketnet_log_level_t g_polish_level = ROCKETNET_LOG_LEVEL_NONE;
static char g_polish_message[64];
static void * g_polish_context = NULL;

static void api_polish_log_capture( void * context, rocketnet_log_level_t level, const char * message, size_t length )
{
    g_polish_calls++;
    g_polish_level = level;
    g_polish_context = context;
    const size_t n = length < sizeof( g_polish_message ) - 1 ? length : sizeof( g_polish_message ) - 1;
    memcpy( g_polish_message, message, n );
    g_polish_message[n] = '\0';
}

static void test_api_polish_log_callback_receives_a_message()
{
    const rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    int context = 0;
    g_polish_calls = 0;
    rocketnet_log_function( api_polish_log_capture, &context );
    rocketnet_warn( "polish %d", 9 );
    rocketnet_log_function( NULL, NULL );
    check( g_polish_calls == 1 );
    check( g_polish_level == ROCKETNET_LOG_LEVEL_WARN );
    check( g_polish_context == &context );
    check( strcmp( g_polish_message, "polish 9" ) == 0 );

    // a level outside the enum is ignored; the one in force stays
    rocketnet_log_level( (rocketnet_log_level_t) 9 );
    check( rocketnet_log_level_current() == ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_level( saved );
}
REGISTER_UNIT_TEST( test_api_polish_log_callback_receives_a_message );

// --- ruling 7: every lookup is _name and says "unknown" ------------------------

static_assert( rn_same<decltype( &rocketnet_log_level_name ), const char * (*)( rocketnet_log_level_t )>::value, "ruling 7: log level name" );
static_assert( rn_same<decltype( &rocketnet_client_state_name ), const char * (*)( rocketnet_client_state_t )>::value, "ruling 7: client state name" );
static_assert( rn_same<decltype( &rocketnet_disconnect_reason_name ), const char * (*)( rocketnet_disconnect_reason_t )>::value, "ruling 7: disconnect reason name" );
static_assert( rn_same<decltype( ( (rocketnet_client_config_t *) 0 )->state_change_callback ),
                       void (*)( void *, rocketnet_client_state_t, rocketnet_client_state_t )>::value, "ruling 7: state_change_callback takes the enum" );

static void test_api_polish_lookups_say_unknown()
{
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_OK ), "OK" ) == 0 );
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_FLOW_ID_RESERVED ), "FLOW_ID_RESERVED" ) == 0 );
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) -28 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) 1 ), "UNKNOWN" ) == 0 );

    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_NONE ), "NONE" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_WARN ), "WARN" ) == 0 );
    check( strcmp( rocketnet_log_level_name( (rocketnet_log_level_t) 6 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_log_level_name( (rocketnet_log_level_t) -1 ), "UNKNOWN" ) == 0 );

    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_CONNECTED ), "CONNECTED" ) == 0 );
    check( strcmp( rocketnet_client_state_name( (rocketnet_client_state_t) 5 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED ), "SERVER_DISCONNECTED" ) == 0 );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT ), "RELIABLE_TIMED_OUT" ) == 0 );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION ), "PROTOCOL_VIOLATION" ) == 0 );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH ), "CONFIG_MISMATCH" ) == 0 );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED ), "MESSAGE_MEMORY_EXHAUSTED" ) == 0 );
    check( strcmp( rocketnet_client_state_name( (rocketnet_client_state_t) -14 ), "UNKNOWN" ) == 0 );

    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_NONE ), "NONE" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT ), "RELIABLE_TIMED_OUT" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED ), "SERVER_DISCONNECTED" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( (rocketnet_disconnect_reason_t) 7 ), "UNKNOWN" ) == 0 );
}
REGISTER_UNIT_TEST( test_api_polish_lookups_say_unknown );

// A zero-filled receipt names NONE. Value 2 is reserved and is not a
// disconnect reason. Every name lookup names every value of its enum.
static void test_api_polish_zero_receipt_and_dense_reasons()
{
    struct rocketnet_receipt_t receipt;
    memset( &receipt, 0, sizeof( receipt ) );
    check( receipt.status == ROCKETNET_RECEIPT_STATUS_NONE );

    const enum rocketnet_disconnect_reason_t reasons[] =
    {
        ROCKETNET_DISCONNECT_REASON_NONE,
        ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH,
        ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION,
        ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT,
        ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED,
        ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED,
    };
    const int reason_values[] = { 0, -1, -2, -3, -4, -5 };
    for ( int i = 0; i < (int) ( sizeof( reasons ) / sizeof( reasons[0] ) ); i++ )
    {
        check( (int) reasons[i] == reason_values[i] );
        const char * name = rocketnet_disconnect_reason_name( reasons[i] );
        check( name != NULL && name[0] != '\0' && strcmp( name, "UNKNOWN" ) != 0 );
    }
    check( strcmp( rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) 2 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) 7 ), "UNKNOWN" ) == 0 );

    for ( int status = (int) ROCKETNET_STATUS_OK; status >= (int) ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED; status-- )
    {
        const char * name = rocketnet_status_name( (enum rocketnet_status_t) status );
        check( name != NULL && strcmp( name, "UNKNOWN" ) != 0 );
    }
    for ( int level = (int) ROCKETNET_LOG_LEVEL_NONE; level <= (int) ROCKETNET_LOG_LEVEL_SPAM; level++ )
    {
        const char * name = rocketnet_log_level_name( (enum rocketnet_log_level_t) level );
        check( name != NULL && strcmp( name, "UNKNOWN" ) != 0 );
    }
    for ( int state = (int) ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED; state <= (int) ROCKETNET_CLIENT_STATE_CONNECTED; state++ )
    {
        const char * name = rocketnet_client_state_name( (enum rocketnet_client_state_t) state );
        check( name != NULL && strcmp( name, "UNKNOWN" ) != 0 && strcmp( name, "???" ) != 0 );
    }

    check( sizeof( enum rocketnet_status_t ) == sizeof( int ) );
    check( sizeof( enum rocketnet_log_level_t ) == sizeof( int ) );
    check( sizeof( enum rocketnet_reliability_t ) == sizeof( int ) );
    check( sizeof( enum rocketnet_receipt_status_t ) == sizeof( int ) );
    check( sizeof( enum rocketnet_disconnect_reason_t ) == sizeof( int ) );
    check( sizeof( enum rocketnet_update_result_t ) == sizeof( int ) );
    check( sizeof( enum rocketnet_client_state_t ) == sizeof( int ) );
}
REGISTER_UNIT_TEST( test_api_polish_zero_receipt_and_dense_reasons );

// --- ruling 8: sizes are size_t ------------------------------------------------

static_assert( rn_same<decltype( rocketnet_client_required_memory( (struct rocketnet_client_config_t *) NULL ) ), size_t>::value, "ruling 8: client required memory" );
static_assert( rn_same<decltype( rocketnet_server_required_memory( (struct rocketnet_server_config_t *) NULL ) ), size_t>::value, "ruling 8: server required memory" );
static_assert( rn_same<decltype( static_cast<rocketnet_client_t * (*)( void *, const rocketnet_client_config_t *, rocketnet_status_t * )>( &rocketnet_client_create ) ), rocketnet_client_t * (*)( void *, const rocketnet_client_config_t *, rocketnet_status_t * )>::value, "ruling 8: client create" );
static_assert( rn_same<decltype( &rocketnet_server_create ), rocketnet_server_t * (*)( void *, const char *, const char *, const rocketnet_server_config_t *, rocketnet_status_t * )>::value, "ruling 8: server create" );

// --- ruling 9: a NULL client reads DISCONNECTED, counted ----------------------------

static void api_polish_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
}

static void test_api_polish_null_client_state_is_disconnected()
{
    static_assert( ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED == -12, "dense: the last failure state is -12" );
    rocketnet_assert_function( api_polish_quiet_assert );
    const uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_client_state( NULL ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_api_misuse_count() == before + 1 );
    rocketnet_assert_function( NULL );
}
REGISTER_UNIT_TEST( test_api_polish_null_client_state_is_disconnected );

// --- a client index before CONNECTED is -1, never an assert --------------------

static int g_polish_asserts = 0;

static void api_polish_count_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
    g_polish_asserts++;
}

static double g_polish_index_now = 1000.0;
static double api_polish_index_clock( void * ) { return g_polish_index_now; }

#define API_POLISH_INDEX_PROTOCOL 0x1D3C0FFEE0A11CE5ULL

static void test_api_polish_client_index_is_minus_one_until_connected()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( api_polish_count_assert );
    g_polish_asserts = 0;
    g_polish_index_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5C ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = API_POLISH_INDEX_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, server, API_POLISH_INDEX_PROTOCOL, addresses, 1,
        0x1D, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = 1;
    client_config.time_callback = api_polish_index_clock;
    create_status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    // before connect: no connection, index -1
    check( rocketnet_client_index( client ) == -1 );
    check( g_polish_asserts == 0 );

    // while connecting: the client holds no server slot yet; index -1, no assert
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == -1 );
    check( g_polish_asserts == 0 );

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        rocketnet_client_update( client, g_polish_index_now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, g_polish_index_now );
        rocketnet_server_send_packets( server );
        if ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
            check( rocketnet_client_index( client ) == -1 );
        g_polish_index_now += 0.01;
    }
    check( g_polish_asserts == 0 );

    // connected: the index is the server slot the client holds
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == 0 );
    check( rocketnet_server_client_connected( server, 0 ) );
    check( g_polish_asserts == 0 );

    // disconnected again: -1
    rocketnet_client_disconnect( client );
    check( rocketnet_client_index( client ) == -1 );
    check( g_polish_asserts == 0 );

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}
REGISTER_UNIT_TEST( test_api_polish_client_index_is_minus_one_until_connected );
// --- server abort_message acts on the pending start: with none, a no-op ---------

static void test_api_polish_server_abort_message_without_start_is_a_no_op()
{
    rocketnet_server_config_t config;
    api_polish_dev_server_config( &config );
    config.max_clients = 16;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    rocketnet_assert_function( api_polish_quiet_assert );
    const uint64_t before = rocketnet_api_misuse_count();
    rocketnet_server_abort_message( server );
    check( rocketnet_api_misuse_count() == before );
    check( rocketnet_server_finish_message( server, 4 ) == 0 );
    check( rocketnet_api_misuse_count() == before );
    rocketnet_assert_function( NULL );

    rocketnet_server_destroy( server );
}
REGISTER_UNIT_TEST( test_api_polish_server_abort_message_without_start_is_a_no_op );

// --- the production minter refuses a token no peer accepts ---------------------

// a timeout of zero or below (t13.2: a production server refuses both), and an
// expire stamp at or before the create stamp (the token is already expired when
// it is made), are refused with INVALID_ARGUMENT and the output is untouched
static void test_api_polish_production_token_refuses_dead_tokens()
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    const char * addresses[] = { "127.0.0.1:40000" };
    const uint64_t create = 1000;

    struct dead_token_case_t { uint64_t expire; int32_t timeout_seconds; };
    const dead_token_case_t cases[] =
    {
        { create + 30, 0 },             // no lifetime: timeout zero
        { create + 30, -1 },            // no timeout: a production server refuses it
        { create, 10 },                 // expire == create
        { create - 1, 10 },             // expire < create
        { 0, 10 },                      // expire already past at the create stamp
    };

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    for ( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
    {
        memset( token, 0xA5, sizeof( token ) );
        check( rocketnet_write_production_connect_token( backend_secret, server_public, 0x1234ULL, addresses, 1, 7,
                                                         create, cases[i].expire, cases[i].timeout_seconds, NULL,
                                                         token, sizeof( token ) ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        for ( size_t j = 0; j < sizeof( token ); j++ )
            check( token[j] == 0xA5 );
    }

    // the smallest live token still mints
    check( rocketnet_write_production_connect_token( backend_secret, server_public, 0x1234ULL, addresses, 1, 7,
                                                     create, create + 1, 1, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
}
REGISTER_UNIT_TEST( test_api_polish_production_token_refuses_dead_tokens );

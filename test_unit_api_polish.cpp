/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The v1.0.0 API rulings, pinned. Each ruling has a compile-time check that
// the old form fails to compile (a static_assert on the declared type, or an
// #error on a removed name), and a run-time case where the old form returned
// a different value. Every case runs in memory, with no clock, sockets or
// network; the one time argument is 1000.0.
//
//   1. every call that can fail returns enum rocketnet_status_t
//   2. the dev connect-token writer takes connect_token_bytes and returns a status
//   3. typed enums replace the #defines, named ROCKETNET_<SET>_<VALUE>
//   4. pure questions return bool; step_driven is bool
//   5. set_ and get_ naming
//   6. one log sink: rocketnet_set_log_callback; the log level is an enum
//   7. every lookup ends in _name and returns "unknown" for an unknown value
//   8. memory and buffer sizes are size_t
//   9. a NULL client's state is ROCKETNET_CLIENT_STATE_INVALID

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"

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

static_assert( rn_same<decltype( rocketnet_init( NULL ) ), rocketnet_status_t>::value, "ruling 1: init" );
static_assert( rn_same<decltype( rocketnet_term() ), rocketnet_status_t>::value, "ruling 1: term" );
static_assert( rn_same<decltype( rocketnet_set_allocator( NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1 and 5: set_allocator" );
static_assert( rn_same<decltype( rocketnet_generate_backend_keypair( NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: backend keypair" );
static_assert( rn_same<decltype( rocketnet_generate_backend_keypair_from_seed( NULL, NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: seeded keypair" );
static_assert( rn_same<decltype( rocketnet_generate_server_keypair( NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: server keypair" );
static_assert( rn_same<decltype( rocketnet_write_production_connect_token( NULL, NULL, 0, NULL, 0, 0, 0, 0, 0, NULL, NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: production token" );
static_assert( rn_same<decltype( rocketnet_client_default_config( NULL ) ), rocketnet_status_t>::value, "ruling 1: client default config" );
static_assert( rn_same<decltype( rocketnet_client_default_config_sized( NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: client default config sized" );
static_assert( rn_same<decltype( rocketnet_server_default_config( NULL ) ), rocketnet_status_t>::value, "ruling 1: server default config" );
static_assert( rn_same<decltype( rocketnet_server_default_config_sized( NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: server default config sized" );
static_assert( rn_same<decltype( rocketnet_server_dev_config( NULL ) ), rocketnet_status_t>::value, "ruling 1: server dev config" );
static_assert( rn_same<decltype( rocketnet_server_dev_config_sized( NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: server dev config sized" );
static_assert( rn_same<decltype( rocketnet_client_validate_config( NULL ) ), rocketnet_status_t>::value, "ruling 1: client validate" );
static_assert( rn_same<decltype( rocketnet_client_validate_config_sized( NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: client validate sized" );
static_assert( rn_same<decltype( rocketnet_server_validate_config( NULL ) ), rocketnet_status_t>::value, "ruling 1: server validate" );
static_assert( rn_same<decltype( rocketnet_server_validate_config_sized( NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: server validate sized" );
static_assert( rn_same<decltype( rocketnet_client_set_send_kbps( NULL, 0 ) ), rocketnet_status_t>::value, "ruling 1: client set_send_kbps" );
static_assert( rn_same<decltype( rocketnet_server_set_send_kbps( NULL, 0, 0 ) ), rocketnet_status_t>::value, "ruling 1: server set_send_kbps" );
static_assert( rn_same<decltype( rocketnet_client_connect( NULL, NULL ) ), rocketnet_status_t>::value, "ruling 1: connect" );
static_assert( rn_same<decltype( rocketnet_server_start( NULL ) ), rocketnet_status_t>::value, "ruling 1: start" );
static_assert( rn_same<decltype( &rocketnet_status_name ), const char * (*)( rocketnet_status_t )>::value, "ruling 1 and 7: status_name takes the enum" );

static void * api_polish_malloc( void * context, size_t bytes ) { (void) context; return malloc( bytes ); }
static void api_polish_free( void * context, void * p ) { (void) context; free( p ); }

// the worker process is initialized before a unit test runs: a second init
// says why it refused, where the old bool said only false
static void api_polish_status_returns()
{
    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_ALREADY_INITIALIZED );
    check( rocketnet_set_allocator( api_polish_malloc, api_polish_free ) == ROCKETNET_STATUS_ALREADY_INITIALIZED );
    check( rocketnet_set_allocator( api_polish_malloc, NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // a live allocation refuses term, with the reason, and nothing changes
    void * held = rocketnet_malloc( NULL, 64 );
    check( held != NULL );
    check( rocketnet_term() == ROCKETNET_STATUS_LIVE_ALLOCATIONS );
    rocketnet_free( NULL, held );
    check( rocketnet_library_initialized() );

    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_server_keypair( public_key, secret_key ) == ROCKETNET_STATUS_OK );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    const char * addresses[] = { "127.0.0.1:40000" };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( backend_secret, public_key, 0x1234ULL, addresses, 1, 7, 1000, 2000, 10, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_write_production_connect_token( backend_secret, public_key, 0x1234ULL, addresses, 1, 7, 1000, 2000, 10, NULL, token, sizeof( token ) - 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // validate and the default config: OK is 0, where the old form's success was 1
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config_sized( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_validate_config_sized( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_default_config_sized( &client_config, sizeof( client_config ) - 4 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    client_config.num_channels = 0;
    check( rocketnet_client_validate_config_sized( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_dev_config_sized( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_validate_config_sized( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
}
REGISTER_UNIT_TEST( api_polish_status_returns );

// --- ruling 2: the dev token writer takes its buffer size ---------------------

// The unit suite links the release archive, where the development writer is
// compiled out; the development shared library built beside it carries it, so
// the writer is loaded from there (as test_unit_connect_token_vectors.cpp
// does) and called through the new signature. The old four-argument writer
// would ignore the size and return 1, failing every check below.
typedef rocketnet_status_t ( *api_polish_dev_writer_t )( const char *, uint64_t, uint64_t, uint8_t *, size_t );

#if ROCKETNET_DEVELOPMENT
static_assert( rn_same<decltype( &rocketnet_write_dev_connect_token ), api_polish_dev_writer_t>::value, "ruling 2: dev token signature" );
#endif // #if ROCKETNET_DEVELOPMENT

// the development library keeps its own log state, apart from the sink the
// suite installs in the archive it links, so the test captures the refusal
// line in that library
typedef void ( *api_polish_dev_set_log_t )( rocketnet_log_callback_t, void * );
static api_polish_dev_set_log_t api_polish_dev_set_log = NULL;
static int api_polish_dev_error_lines = 0;

static void api_polish_dev_log_capture( void *, enum rocketnet_log_level_t level, const char * message, size_t )
{
    if ( level == ROCKETNET_LOG_LEVEL_ERROR && strstr( message, "767" ) )
        api_polish_dev_error_lines++;
}

static api_polish_dev_writer_t api_polish_dev_writer()
{
    api_polish_dev_writer_t writer = NULL;
#if defined(_WIN32)
    HMODULE library = LoadLibraryA( "rocketnet_dev.dll" );
    if ( library == NULL )
        library = LoadLibraryA( "bin\\rocketnet_dev.dll" );
    check( library != NULL );
    writer = (api_polish_dev_writer_t) (void *) GetProcAddress( library, "rocketnet_write_dev_connect_token" );
    api_polish_dev_set_log = (api_polish_dev_set_log_t) (void *) GetProcAddress( library, "rocketnet_set_log_callback" );
#else
    const char * names[] = { "bin/librocketnet_dev.so", "librocketnet_dev.so",
                             "bin/librocketnet_dev.dylib", "librocketnet_dev.dylib" };
    void * library = NULL;
    for ( size_t i = 0; i < sizeof( names ) / sizeof( names[0] ) && library == NULL; i++ )
        library = dlopen( names[i], RTLD_NOW | RTLD_LOCAL );
    check( library != NULL );
    writer = (api_polish_dev_writer_t) dlsym( library, "rocketnet_write_dev_connect_token" );
    api_polish_dev_set_log = (api_polish_dev_set_log_t) dlsym( library, "rocketnet_set_log_callback" );
#endif
    check( writer != NULL );
    check( api_polish_dev_set_log != NULL );
    return writer;
}

static void api_polish_dev_token_buffer()
{
    api_polish_dev_writer_t write_dev = api_polish_dev_writer();
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES + 1];
    memset( token, 0xAB, sizeof( token ) );
    api_polish_dev_set_log( api_polish_dev_log_capture, NULL );
    check( write_dev( "127.0.0.1:40000", 1, 1000, token, ROCKETNET_CONNECT_TOKEN_BYTES - 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( api_polish_dev_error_lines == 1 );                                             // the short buffer is named
    check( token[0] == 0xAB && token[ROCKETNET_CONNECT_TOKEN_BYTES - 2] == 0xAB );      // nothing written
    check( write_dev( "not an address", 1, 1000, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( write_dev( "127.0.0.1:40000", 1, 1000, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( token[ROCKETNET_CONNECT_TOKEN_BYTES] == 0xAB );                                // nothing past the token
    api_polish_dev_set_log( NULL, NULL );
}
REGISTER_UNIT_TEST( api_polish_dev_token_buffer );

// --- ruling 3: typed enums, values unchanged ----------------------------------

static_assert( ROCKETNET_RELIABILITY_UNRELIABLE == 0 && ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED == 1 &&
               ROCKETNET_RELIABILITY_RELIABLE == 2 && ROCKETNET_RELIABILITY_RELIABLE_ORDERED == 3 &&
               ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED == 4 && ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT == 5 &&
               ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT == 6 && ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT == 7, "ruling 3: reliability values" );
static_assert( ROCKETNET_RECEIPT_STATUS_ACKED == 1 && ROCKETNET_RECEIPT_STATUS_LOSS == 2, "ruling 3: receipt values" );
static_assert( ROCKETNET_DISCONNECT_REASON_NONE == 0 && ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH == 1 &&
               ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION == 3 &&
               ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT == 4 && ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED == 5 &&
               ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED == 6, "ruling 3: disconnect reason values" );
static_assert( ROCKETNET_UPDATE_OK == 0 && ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED == -1, "ruling 3: update values" );
static_assert( rn_same<decltype( rocketnet_client_update( NULL, 1000.0 ) ), rocketnet_update_result_t>::value, "ruling 3: client update" );
static_assert( rn_same<decltype( rocketnet_server_update( NULL, 1000.0 ) ), rocketnet_update_result_t>::value, "ruling 3: server update" );
static_assert( rn_same<decltype( rocketnet_client_disconnect_reason( NULL ) ), rocketnet_disconnect_reason_t>::value, "ruling 3: client reason" );
static_assert( rn_same<decltype( rocketnet_server_disconnect_reason( NULL, 0 ) ), rocketnet_disconnect_reason_t>::value, "ruling 3: server reason" );
static_assert( rn_same<decltype( &rocketnet_client_start_message ), uint8_t * (*)( rocketnet_client_t *, int, rocketnet_reliability_t, int )>::value, "ruling 3: client start" );
static_assert( rn_same<decltype( &rocketnet_server_start_message ), uint8_t * (*)( rocketnet_server_t *, uint64_t, int, rocketnet_reliability_t, int )>::value, "ruling 3: server start" );

// --- ruling 4: pure questions are bool ----------------------------------------

static_assert( rn_same<decltype( rocketnet_server_running( NULL ) ), bool>::value, "ruling 4: running" );
static_assert( rn_same<decltype( rocketnet_server_client_connected( NULL, 0 ) ), bool>::value, "ruling 4: client connected" );
static_assert( rn_same<decltype( ( (rocketnet_client_config_t *) 0 )->step_driven ), bool>::value, "ruling 4: step_driven" );

static void api_polish_bool_field_abi()
{
    // the bool field's stated ABI size: one byte, at its compiler offset
    check( rocketnet_abi_field_size( "rocketnet_client_config_t", "step_driven" ) == 1 );
    check( rocketnet_abi_field_offset( "rocketnet_client_config_t", "step_driven" ) == offsetof( rocketnet_client_config_t, step_driven ) );
    check( rocketnet_abi_field_size( "rocketnet_client_config_t", "send_kbps" ) == sizeof( int ) );
    check( rocketnet_abi_type_size( "rocketnet_client_config_t" ) == sizeof( rocketnet_client_config_t ) );
    check( rocketnet_abi_version() == 1 );
    struct rocketnet_client_config_t config;
    check( rocketnet_client_default_config_sized( &config, sizeof( config ) ) == ROCKETNET_STATUS_OK );
    check( config.step_driven == false );
}
REGISTER_UNIT_TEST( api_polish_bool_field_abi );

// --- ruling 5 and 6: set_/get_ naming, one log sink, an enum level -------------

static_assert( rn_same<decltype( &rocketnet_set_log_level ), void (*)( rocketnet_log_level_t )>::value, "ruling 5: set_log_level" );
static_assert( rn_same<decltype( rocketnet_get_log_level() ), rocketnet_log_level_t>::value, "ruling 5: get_log_level" );
static_assert( rn_same<decltype( &rocketnet_set_assert_callback ), void (*)( rocketnet_assert_callback_t )>::value, "ruling 5: set_assert_callback" );
static_assert( rn_same<rocketnet_log_callback_t, void (*)( void *, rocketnet_log_level_t, const char *, size_t )>::value, "ruling 6: the callback takes the enum level" );

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

static void api_polish_log_callback_receives_a_message()
{
    const rocketnet_log_level_t saved = rocketnet_get_log_level();
    rocketnet_set_log_level( ROCKETNET_LOG_LEVEL_INFO );
    int context = 0;
    g_polish_calls = 0;
    rocketnet_set_log_callback( api_polish_log_capture, &context );
    rocketnet_warn( "polish %d", 9 );
    rocketnet_set_log_callback( NULL, NULL );
    check( g_polish_calls == 1 );
    check( g_polish_level == ROCKETNET_LOG_LEVEL_WARN );
    check( g_polish_context == &context );
    check( strcmp( g_polish_message, "polish 9" ) == 0 );

    // a level outside the enum is ignored; the one in force stays
    rocketnet_set_log_level( (rocketnet_log_level_t) 9 );
    check( rocketnet_get_log_level() == ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_set_log_level( saved );
}
REGISTER_UNIT_TEST( api_polish_log_callback_receives_a_message );

// --- ruling 7: every lookup is _name and says "unknown" ------------------------

static_assert( rn_same<decltype( &rocketnet_log_level_name ), const char * (*)( rocketnet_log_level_t )>::value, "ruling 7: log level name" );
static_assert( rn_same<decltype( &rocketnet_client_state_name ), const char * (*)( rocketnet_client_state_t )>::value, "ruling 7: client state name" );
static_assert( rn_same<decltype( &rocketnet_disconnect_reason_name ), const char * (*)( rocketnet_disconnect_reason_t )>::value, "ruling 7: disconnect reason name" );
static_assert( rn_same<decltype( ( (rocketnet_client_config_t *) 0 )->state_change_callback ),
                       void (*)( void *, rocketnet_client_state_t, rocketnet_client_state_t )>::value, "ruling 7: state_change_callback takes the enum" );

static void api_polish_lookups_say_unknown()
{
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_OK ), "OK" ) == 0 );
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_FLOW_ID_RESERVED ), "FLOW_ID_RESERVED" ) == 0 );
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_COUNT ), "unknown" ) == 0 );
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) -1 ), "unknown" ) == 0 );

    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_NONE ), "none" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_WARN ), "warn" ) == 0 );
    check( strcmp( rocketnet_log_level_name( (rocketnet_log_level_t) 6 ), "unknown" ) == 0 );
    check( strcmp( rocketnet_log_level_name( (rocketnet_log_level_t) -1 ), "unknown" ) == 0 );

    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_CONNECTED ), "connected" ) == 0 );
    check( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_INVALID ), "invalid" ) == 0 );
    check( strcmp( rocketnet_client_state_name( (rocketnet_client_state_t) 5 ), "unknown" ) == 0 );
    check( strcmp( rocketnet_client_state_name( (rocketnet_client_state_t) -8 ), "unknown" ) == 0 );

    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_NONE ), "none" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT ), "reliable timeout" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED ), "server disconnected" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( (rocketnet_disconnect_reason_t) 7 ), "unknown" ) == 0 );
}
REGISTER_UNIT_TEST( api_polish_lookups_say_unknown );

// --- ruling 8: sizes are size_t ------------------------------------------------

static_assert( rn_same<decltype( rocketnet_client_required_memory( NULL ) ), size_t>::value, "ruling 8: client required memory" );
static_assert( rn_same<decltype( rocketnet_client_required_memory_sized( NULL, 0 ) ), size_t>::value, "ruling 8: client required memory sized" );
static_assert( rn_same<decltype( rocketnet_server_required_memory( NULL ) ), size_t>::value, "ruling 8: server required memory" );
static_assert( rn_same<decltype( rocketnet_server_required_memory_sized( NULL, 0 ) ), size_t>::value, "ruling 8: server required memory sized" );
static_assert( rn_same<decltype( &rocketnet_client_create ), rocketnet_client_t * (*)( void *, const rocketnet_client_config_t *, void *, size_t )>::value, "ruling 8: client create" );
static_assert( rn_same<decltype( &rocketnet_client_create_sized ), rocketnet_client_t * (*)( void *, const rocketnet_client_config_t *, size_t, void *, size_t )>::value, "ruling 8: client create sized" );
static_assert( rn_same<decltype( &rocketnet_server_create ), rocketnet_server_t * (*)( void *, const char *, const rocketnet_server_config_t *, void *, size_t )>::value, "ruling 8: server create" );
static_assert( rn_same<decltype( &rocketnet_server_create_sized ), rocketnet_server_t * (*)( void *, const char *, const rocketnet_server_config_t *, size_t, void *, size_t )>::value, "ruling 8: server create sized" );

// --- ruling 9: a NULL client reads INVALID, counted ----------------------------

static void api_polish_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
}

static void api_polish_null_client_state_is_invalid()
{
    static_assert( ROCKETNET_CLIENT_STATE_INVALID == -100, "ruling 9: the value" );
    rocketnet_set_assert_callback( api_polish_quiet_assert );
    const uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_client_state( NULL ) == ROCKETNET_CLIENT_STATE_INVALID );
    check( rocketnet_api_misuse_count() == before + 1 );
    rocketnet_set_assert_callback( NULL );
}
REGISTER_UNIT_TEST( api_polish_null_client_state_is_invalid );

// --- a client index before CONNECTED is 0, never an assert ---------------------

static int g_polish_asserts = 0;

static void api_polish_count_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
    g_polish_asserts++;
}

static double g_polish_index_now = 1000.0;
static double api_polish_index_clock( void * ) { return g_polish_index_now; }

#define API_POLISH_INDEX_PROTOCOL 0x1D3C0FFEE0A11CE5ULL

static void api_polish_client_index_is_zero_until_connected()
{
    rocketnet_set_quiet( true );
    rocketnet_set_assert_callback( api_polish_count_assert );
    g_polish_asserts = 0;
    g_polish_index_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5C ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = API_POLISH_INDEX_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret, sizeof( server_config.server_secret_key ) );
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( backend_secret, server_public, API_POLISH_INDEX_PROTOCOL, addresses, 1,
        0x1D, 1000, 1ULL << 40, 300, NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = api_polish_index_clock;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( client != NULL );

    // before connect: no connection, index 0
    check( rocketnet_client_index( client ) == 0 );
    check( g_polish_asserts == 0 );

    // while connecting: the client holds no server slot yet; index 0, no assert
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == 0 );
    check( g_polish_asserts == 0 );

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        check( rocketnet_client_update( client, g_polish_index_now ) == ROCKETNET_UPDATE_OK );
        rocketnet_client_send_packets( client );
        check( rocketnet_server_update( server, g_polish_index_now ) == ROCKETNET_UPDATE_OK );
        rocketnet_server_send_packets( server );
        if ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
            check( rocketnet_client_index( client ) == 0 );
        g_polish_index_now += 0.01;
    }
    check( g_polish_asserts == 0 );

    // connected: the index is the server slot the client holds
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == 0 );
    check( rocketnet_server_client_connected( server, 0 ) );
    check( g_polish_asserts == 0 );

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_set_assert_callback( NULL );
    rocketnet_set_quiet( false );
}
REGISTER_UNIT_TEST( api_polish_client_index_is_zero_until_connected );

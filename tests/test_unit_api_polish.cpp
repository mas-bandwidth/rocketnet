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

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "token_string.h"

#include <stdio.h>
#include <string.h>
#include <type_traits>
#include "loopback_hub.h"
#include "fake_client_driver.h"
#include "rocketnet_ack.h"
#include "rocketnet_address.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_constants.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_message_object.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_wire.h"

#define check rocketnet_unit_check

// --- the removed names stay removed ----------------------------------------

#if defined( ROCKETNET_UNRELIABLE ) || defined( ROCKETNET_RELIABLE_ORDERED ) || defined( ROCKETNET_RECEIPT_ACKED ) || defined( ROCKETNET_MESSAGE_OK )
#error "ruling 3: the reliability, receipt and update #defines are enums now"
#endif // #if defined( ROCKETNET_UNRELIABLE ) || defined( ROCKETNET_RELIABLE_ORDERED ) || defined( ROCKETNET_RECEIPT_ACKED ) || defined( ROCKETNET_MESSAGE_OK )
#if defined( ROCKETNET_MESSAGE_ERROR_NONE ) || defined( ROCKETNET_ERROR_MESSAGE_NOT_RELEASED ) || defined( ROCKETNET_LOG_LEVEL_SPAM )
#error "ruling 3 and 6: disconnect reasons, update results and log levels are enums now"
#endif // #if defined( ROCKETNET_MESSAGE_ERROR_NONE ) || defined( ROCKETNET_ERROR_MESSAGE_NOT_RELEASED ) || defined( ROCKETNET_LOG_LEVEL_SPAM )

template <typename A, typename B> struct rn_same
{
    static const bool value = std::is_same<A, B>::value;
};

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
static_assert( rn_same<decltype( &rocketnet_status_name ), const char * (*) ( rocketnet_status_t )>::value, "ruling 1 and 7: status_name takes the enum" );

// the published development keys, set by hand after the defaults

static void api_polish_dev_server_config( struct rocketnet_server_config_t * config )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
}

static void * api_polish_malloc( void * context, size_t bytes )
{
    (void) context;
    return malloc( bytes );
}
static void api_polish_free( void * context, void * p )
{
    (void) context;
    free( p );
}

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
    rn_test_client_stop_and_destroy( client );
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
    rn_test_server_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_api_polish_status_returns );

// --- ruling 3: typed enums, values unchanged ----------------------------------

static_assert( ROCKETNET_RELIABILITY_UNRELIABLE == 0 && ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED == 1 &&
                   ROCKETNET_RELIABILITY_RELIABLE == 2 && ROCKETNET_RELIABILITY_RELIABLE_ORDERED == 3 &&
                   ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED == 4 && ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT == 5 &&
                   ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT == 6 && ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT == 7,
               "ruling 3: reliability values" );
static_assert( ROCKETNET_RECEIPT_STATUS_NONE == 0 && ROCKETNET_RECEIPT_STATUS_ACKED == 1 && ROCKETNET_RECEIPT_STATUS_LOSS == -1, "ruling 3: receipt values, the loss negative" );
static_assert( ROCKETNET_DISCONNECT_REASON_NONE == 0 && ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH == -1 &&
                   ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION == -2 &&
                   ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT == -3 && ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED == -4 &&
                   ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED == -5,
               "disconnect reasons: dense, failures below zero" );
static_assert( ROCKETNET_UPDATE_OK == 0 && ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED == -1, "ruling 3: the internal message layer's update values" );
static_assert( rn_same<decltype( rocketnet_client_update( NULL, 1000.0 ) ), void>::value, "ruling 3: client update returns void" );
static_assert( rn_same<decltype( rocketnet_server_update( NULL, 1000.0 ) ), void>::value, "ruling 3: server update returns void" );
static_assert( rn_same<decltype( rocketnet_server_disconnect_reason( NULL, 0 ) ), rocketnet_disconnect_reason_t>::value, "ruling 3: server reason" );
static_assert( ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED == -8 && ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT == -9 &&
                   ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION == -10 && ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH == -11 &&
                   ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED == -12,
               "ruling 3: the connection-ended client states" );
static_assert( rn_same<decltype( &rocketnet_client_index ), int ( * )( const rocketnet_client_t * )>::value, "ruling 3: the client index is int" );
static_assert( rn_same<decltype( &rocketnet_server_client_connected ), bool ( * )( const rocketnet_server_t *, int )>::value, "ruling 3: per-client calls take an int index" );
static_assert( rn_same<decltype( &rocketnet_server_finish_message ), uint32_t ( * )( rocketnet_server_t *, int )>::value, "ruling 3: server finish takes no destination" );
static_assert( rn_same<decltype( &rocketnet_server_abort_message ), void ( * )( rocketnet_server_t * )>::value, "ruling 3: server abort takes no destination" );
static_assert( rn_same<decltype( &rocketnet_client_start_message ), uint8_t * (*) ( rocketnet_client_t *, int, rocketnet_reliability_t, int )>::value, "ruling 3: client start" );
static_assert( rn_same<decltype( &rocketnet_server_start_message ), uint8_t * (*) ( rocketnet_server_t *, uint64_t, int, rocketnet_reliability_t, int )>::value, "ruling 3: server start" );

// --- ruling 4: pure questions are bool ----------------------------------------

static_assert( rn_same<decltype( rocketnet_server_running( NULL ) ), bool>::value, "ruling 4: running" );
static_assert( rn_same<decltype( rocketnet_server_client_connected( NULL, 0 ) ), bool>::value, "ruling 4: client connected" );
static_assert( rn_same<decltype( ( (rocketnet_client_config_t *) 0 )->step_driven ), bool>::value, "step_driven is a bool flag" );

static void test_api_polish_bool_field_abi()
{
    // the flag is a bool; send_kbps is an int

    check( sizeof( ( (rocketnet_client_config_t *) 0 )->step_driven ) == sizeof( bool ) );
    check( sizeof( ( (rocketnet_client_config_t *) 0 )->send_kbps ) == sizeof( int ) );

    struct rocketnet_client_config_t config;
    check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.step_driven == 0 );
}
REGISTER_UNIT_TEST( test_api_polish_bool_field_abi );

// --- ruling 5 and 6: set_/get_ naming, one log sink, an enum level -------------

static_assert( rn_same<decltype( &rocketnet_log_level ), void ( * )( rocketnet_log_level_t )>::value, "ruling 5: rocketnet_log_level" );
static_assert( rn_same<decltype( rocketnet_log_level_current() ), rocketnet_log_level_t>::value, "ruling 5: the internal rocketnet_log_level_current" );
static_assert( rn_same<decltype( &rocketnet_assert_function ), void ( * )( rocketnet_assert_function_t )>::value, "ruling 5: rocketnet_assert_function" );
static_assert( rn_same<rocketnet_log_function_t, void ( * )( void *, rocketnet_log_level_t, const char *, size_t )>::value, "ruling 6: the callback takes the enum level" );

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

// a level a C caller could pass that is outside the enum is passed from C
// (tests/c_caller.c): in C++ such a value cannot be formed
extern "C" void rocketnet_test_c_caller_log_level( int level );
extern "C" const char * rocketnet_test_c_caller_log_level_name( int level );

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

    rocketnet_test_c_caller_log_level( 9 );
    check( rocketnet_log_level_current() == ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_level( saved );
}
REGISTER_UNIT_TEST( test_api_polish_log_callback_receives_a_message );

// --- ruling 7: every lookup is _name and says "unknown" ------------------------

static_assert( rn_same<decltype( &rocketnet_log_level_name ), const char * (*) ( rocketnet_log_level_t )>::value, "ruling 7: log level name" );
static_assert( rn_same<decltype( &rocketnet_client_state_name ), const char * (*) ( rocketnet_client_state_t )>::value, "ruling 7: client state name" );
static_assert( rn_same<decltype( &rocketnet_disconnect_reason_name ), const char * (*) ( rocketnet_disconnect_reason_t )>::value, "ruling 7: disconnect reason name" );
static_assert( rn_same<decltype( ( (rocketnet_client_config_t *) 0 )->state_change_callback ),
                       void ( * )( void *, rocketnet_client_state_t, rocketnet_client_state_t )>::value,
               "ruling 7: state_change_callback takes the enum" );

static void test_api_polish_lookups_say_unknown()
{
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_OK ), "OK" ) == 0 );
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_FLOW_ID_RESERVED ), "FLOW_ID_RESERVED" ) == 0 );
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) -35 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_status_name( (rocketnet_status_t) 1 ), "UNKNOWN" ) == 0 );

    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_NONE ), "NONE" ) == 0 );
    check( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_WARN ), "WARN" ) == 0 );
    check( strcmp( rocketnet_log_level_name( (rocketnet_log_level_t) 6 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_test_c_caller_log_level_name( -1 ), "UNKNOWN" ) == 0 );

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

    const enum rocketnet_disconnect_reason_t reasons[] = {
        ROCKETNET_DISCONNECT_REASON_NONE,
        ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH,
        ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION,
        ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT,
        ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED,
        ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED,
        ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED,
        ROCKETNET_DISCONNECT_REASON_TIMED_OUT,
        ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED,
    };
    const int reason_values[] = { 0, -1, -2, -3, -4, -5, -6, -7, -8 };

    for ( int i = 0; i < (int) ( sizeof( reasons ) / sizeof( reasons[0] ) ); i++ )
    {
        check( (int) reasons[i] == reason_values[i] );

        const char * name = rocketnet_disconnect_reason_name( reasons[i] );
        check( name != NULL && name[0] != '\0' && strcmp( name, "UNKNOWN" ) != 0 );
    }

    check( strcmp( rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) 2 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) 7 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) 1 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_TIMED_OUT ), "TIMED_OUT" ) == 0 );
    check( strcmp( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED ), "CLIENT_DISCONNECTED" ) == 0 );

    // every status code from OK down to the last names itself; -9 and -22 are reserved

    for ( int status = (int) ROCKETNET_STATUS_OK; status >= (int) ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE; status-- )
    {
        const char * name = rocketnet_status_name( (enum rocketnet_status_t) status );
        check( name != NULL );
        check( ( strcmp( name, "UNKNOWN" ) == 0 ) == ( status == -9 || status == -22 ) );
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
static_assert( rn_same<decltype( static_cast<rocketnet_client_t * (*) ( void *, const rocketnet_client_config_t *, rocketnet_status_t * )>( &rocketnet_client_create ) ), rocketnet_client_t * (*) ( void *, const rocketnet_client_config_t *, rocketnet_status_t * )>::value, "ruling 8: client create" );
static_assert( rn_same<decltype( &rocketnet_server_create ), rocketnet_server_t * (*) ( void *, const char *, const char *, const rocketnet_server_config_t *, rocketnet_status_t * )>::value, "ruling 8: server create" );

// --- ruling 9: a NULL client reads DISCONNECTED, counted ----------------------------

static void api_polish_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
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

#if ROCKETNET_DEV
static int g_polish_asserts = 0;

static void api_polish_count_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
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
    {
        seed[i] = (uint8_t) ( 0x5C ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = API_POLISH_INDEX_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
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

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
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
        {
            check( rocketnet_client_index( client ) == -1 );
        }
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
    rn_test_client_wait_disconnected( client );
    check( rocketnet_client_index( client ) == -1 );
    check( g_polish_asserts == 0 );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}
REGISTER_UNIT_TEST( test_api_polish_client_index_is_minus_one_until_connected );

// --- a connected client in a server slot at or past this build's cap ------------

// The client has no opinion on the server's slot count (sdk/rocketnet.h
// rocketnet_client_index): a server with more slots than this build's
// ROCKETNET_MAX_CLIENTS assigns a slot at or past that cap, and reading the
// index back must return it without trapping. The server here is a protocol
// object with ROCKETNET_MAX_CLIENTS + 1000 slots whose first
// ROCKETNET_MAX_CLIENTS slots are already held, so the handshake assigns
// slot ROCKETNET_MAX_CLIENTS; the client is a step-driven transport on the
// fake driver, adopted as a public client once connected.

static double g_polish_big_index_now = 1000.0;
static double api_polish_big_index_clock( void * ) { return g_polish_big_index_now; }

static void test_api_polish_client_index_at_build_max_clients()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( api_polish_count_assert );
    g_polish_asserts = 0;
    g_polish_big_index_now = 1000.0;

    const int server_max_clients = ROCKETNET_MAX_CLIENTS + 1000;

    struct rocketnet_counters_t server_counters;
    memset( &server_counters, 0, sizeof( server_counters ) );

    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );
    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server.server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server.server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server.cookie_key, rocketnet_test_cookie_key, 32 );
    server.cookie_lifetime_seconds = 3;
    server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server.handshake_deadline_seconds = 5.0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server.client_slots = rocketnet_client_slots_create( NULL, server_max_clients );
    server.acks = rocketnet_server_acks_create( NULL, server_max_clients );
    server.counters = &server_counters;
    check( server.encryption_map && server.used_token_history && server.client_slots && server.acks );
    rocketnet_server_protocol_update( &server, 1000 );

    // every slot below this build's cap is already held

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; i++ )
    {
        const uint64_t dummy_id = UINT64_C( 0xD000000000000000 ) | (uint64_t) (uint32_t) i;
        check( rocketnet_client_slots_assign( server.client_slots, dummy_id, -1, dummy_id, user_data ) == i );
    }

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = 1000;
    signed_fields.expire_timestamp = 4600;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t token_server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, token_server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = UINT64_C( 0x1D3C0FFEE0A11CE5 );
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = 4600;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token ) );

    struct rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    struct rocketnet_client_transport_config_t transport_config;
    rocketnet_client_transport_default_config_current( &transport_config );
    snprintf( transport_config.transport, sizeof( transport_config.transport ), "%s", "loopback" );
    memcpy( transport_config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    transport_config.step_driven = 1;
    transport_config.time_callback = api_polish_big_index_clock;
    transport_config.token_epoch_unix_seconds = 1000;

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_current(
        NULL, token, &transport_config, g_polish_big_index_now, &rocketnet_fake_client_operations_io, fake );
    check( transport != NULL );

    rocketnet_address_t client_address;
    check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );
    rocketnet_address_t server_address;
    check( rocketnet_address_parse( &server_address, "203.0.113.10:40000" ) );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t server_payload[ROCKETNET_RECEIVE_CEILING];

    for ( int i = 0; i < 400 && rocketnet_client_transport_state( transport ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; i++ )
    {
        rocketnet_client_transport_step_legacy_for_test( transport, g_polish_big_index_now );
        rocketnet_client_transport_update( transport, g_polish_big_index_now );
        rocketnet_client_transport_send_packets( transport );

        for ( int sent = 0; sent < fake->num_sent; sent++ )
        {
            int response_bytes = 0;
            int payload_bytes = 0;
            int payload_client_index = -1;
            rocketnet_server_protocol_process_packet( &server, &client_address,
                                                      fake->sent[sent].data, fake->sent[sent].bytes,
                                                      (uint64_t) g_polish_big_index_now, g_polish_big_index_now,
                                                      response, &response_bytes,
                                                      server_payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
            {
                rocketnet_fake_client_push( fake, &server_address, response, response_bytes );
            }
        }
        fake->num_sent = 0;
        g_polish_big_index_now += 0.01;
    }

    check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_client_transport_client_index( transport ) == ROCKETNET_MAX_CLIENTS );

    rn_message_link_t link;
    rn_message_client_link( &link, transport );

    rocketnet_message_client_t * connection = (rocketnet_message_client_t *) rn_message_object_create(
        NULL, &link, 0, 1, NULL, NULL, 0, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    check( connection != NULL );

    rocketnet_client_t * client = rocketnet_client_adopt( NULL, connection );
    check( client != NULL );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    // the server-assigned slot is at this build's cap: reading it must not trap

    g_polish_asserts = 0;
    check( rocketnet_client_index( client ) == ROCKETNET_MAX_CLIENTS );
    check( g_polish_asserts == 0 );

    rn_test_client_stop_and_destroy( client );
    delete fake;

    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );

    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}
REGISTER_UNIT_TEST( test_api_polish_client_index_at_build_max_clients );

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

    rocketnet_assert_function( api_polish_count_assert );
    g_polish_asserts = 0;

    const uint64_t before = rocketnet_api_misuse_count();
    rocketnet_server_abort_message( server );
    check( rocketnet_api_misuse_count() == before );
    check( g_polish_asserts == 0 );
    check( rocketnet_server_finish_message( server, 4 ) == 0 );
    check( rocketnet_api_misuse_count() == before );
    check( g_polish_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    rocketnet_assert_function( NULL );

    rn_test_server_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_api_polish_server_abort_message_without_start_is_a_no_op );
#endif // #if ROCKETNET_DEV

// --- the production minter refuses a token no server accepts -------------------

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

    struct dead_token_case_t
    {
        uint64_t expire;
        int32_t timeout_seconds;
    };
    const dead_token_case_t cases[] = {
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
        {
            check( token[j] == 0xA5 );
        }
    }

    // the smallest live token still mints

    check( rocketnet_write_production_connect_token( backend_secret, server_public, 0x1234ULL, addresses, 1, 7,
                                                     create, create + 1, 1, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
}
REGISTER_UNIT_TEST( test_api_polish_production_token_refuses_dead_tokens );

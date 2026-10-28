/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public create reports its status through an out-parameter (owner ruling,
// 2026-10-20): one create per side, taking the config alone and a final
// status out-parameter; the config's first field, config_bytes, is set by the
// header's inline default_config. Each refusal path names its code here, for
// the client and the server: before init, a config never filled (config_bytes
// below its own field), a NULL or over-long address, a bad config value, a
// bad transport list, an over-budget config, and a failed block allocation
// (the failing allocator of test_unit_driver_create_out_of_memory.cpp,
// walking every allocation create makes). A NULL status is accepted on a
// refusal and on a success. No socket, no thread, no real time. The
// CONFIG_ENV_INVALID case sets one environment variable per side and restores
// it before returning, as test_unit_status_client_transport_create_ex.cpp
// does. The explicit budget range case temporarily clears the two budget
// variables and restores them.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "portable_crt.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_memory.h"
#include "rocketnet_connect_token.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#define check rocketnet_unit_check

static void create_status_quiet_assert( const char *, const char *, const char *, int ) {}

// the defaults, then the published development keys set by hand

static void create_status_dev_server_config( struct rocketnet_server_config_t * config )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
}

#if ROCKETNET_DEV
static int create_status_setenv( const char * name, const char * value ) { return rocketnet_test_env_setenv( name, value, 1 ); }
#endif // #if ROCKETNET_DEV
#if ROCKETNET_DEV
static int create_status_unsetenv( const char * name ) { return rocketnet_test_env_unsetenv( name ); }
#endif // #if ROCKETNET_DEV

// Whether the variable is set; its value, or empty, in value.
#if ROCKETNET_DEV
static bool create_status_getenv( const char * name, std::string & value )
{
    char buffer[4096];
    const bool present = rocketnet_test_env_getenv( name, buffer, sizeof( buffer ) ) != 0;
    value = buffer;
    return present;
}
#endif // #if ROCKETNET_DEV

// ---- the fault-injecting allocator -----------------------------------------

struct create_status_allocator_t
{
    int live;
    int calls;
    int fail_at;
    size_t last_bytes;
    size_t max_bytes;
};

static void * create_status_allocate( void * context, size_t bytes )
{
    create_status_allocator_t * state = (create_status_allocator_t *) context;
    const int call = ++state->calls;
    state->last_bytes = bytes;
    if ( bytes > state->max_bytes )
    {
        state->max_bytes = bytes;
    }
    if ( state->fail_at == call )
        return NULL;

    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memset( (void *) address, 0xA5, bytes );
    ++state->live;

    return (void *) address;
}

static void create_status_release( void * context, void * pointer )
{
    create_status_allocator_t * state = (create_status_allocator_t *) context;
    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    --state->live;
    free( base );
}

// ---- success writes OK -------------------------------------------------------

static void test_create_reports_status_success()
{
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );

    status = ROCKETNET_TEST_STATUS_UNSET;

    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_server_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_create_reports_status_success );

// ---- before init ------------------------------------------------------------

static void test_create_reports_status_not_initialized()
{
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( create_status_quiet_assert );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( !rocketnet_library_initialized() );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );

    // a NULL status is accepted on this refusal too

    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );

    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_assert_function( saved_assert );
}
REGISTER_UNIT_TEST( test_create_reports_status_not_initialized );

// ---- bad arguments ----------------------------------------------------------

static void test_create_reports_status_invalid_argument()
{
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;

    // a NULL server address (the null guard asserts in Debug: quieted here)

    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( create_status_quiet_assert );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, NULL, NULL, &server_config, &status ) == NULL );
    rocketnet_assert_function( saved_assert );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
}
REGISTER_UNIT_TEST( test_create_reports_status_invalid_argument );

// ---- a config never filled: config_bytes below its own field ----------------

// A memset-zero config (config_bytes 0), and every config_bytes too small to
// hold the field itself, is refused by name at create and sizes nothing at
// required_memory; the inline default_config sets config_bytes to the
// caller's sizeof, and the library half refuses a config_bytes that small
// without writing a byte.

static void test_create_reports_status_config_not_initialized()
{
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    check( client_config.config_bytes == sizeof( client_config ) );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    check( server_config.config_bytes == sizeof( server_config ) );
    server_config.max_clients = 4;

    const size_t too_small[] = { 0, 1, sizeof( size_t ) - 1 };
    for ( size_t i = 0; i < sizeof( too_small ) / sizeof( too_small[0] ); i++ )
    {
        client_config.config_bytes = too_small[i];

        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
        check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
        check( rocketnet_client_required_memory( &client_config ) == 0 );

        server_config.config_bytes = too_small[i];
        status = ROCKETNET_STATUS_OK;
        check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
        check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );
        check( rocketnet_server_required_memory( &server_config ) == 0 );
    }

    // the whole struct zeroed, as a caller who skipped default_config has it

    memset( &client_config, 0, sizeof( client_config ) );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    check( rocketnet_client_required_memory( &client_config ) == 0 );
    memset( &server_config, 0, sizeof( server_config ) );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    check( rocketnet_server_required_memory( &server_config ) == 0 );

    // the library half of default_config refuses the same sizes, nothing written

    memset( &client_config, 0xCD, sizeof( client_config ) );
    client_config.config_bytes = 0;
    check( rocketnet_client_default_config_internal( &client_config ) == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    check( client_config.config_bytes == 0 );
    check( client_config.num_channels == (int) 0xCDCDCDCD );
    memset( &server_config, 0xCD, sizeof( server_config ) );
    server_config.config_bytes = 0;
    check( rocketnet_server_default_config_internal( &server_config ) == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    check( server_config.config_bytes == 0 );
    check( server_config.max_clients == (int) 0xCDCDCDCD );

    // filled again, the same config creates

    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );
}
REGISTER_UNIT_TEST( test_create_reports_status_config_not_initialized );

// ---- an over-long address ---------------------------------------------------

static void test_create_reports_status_invalid_address()
{
    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;

    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH + 32];
    memset( address, 'a', sizeof( address ) - 1 );
    address[sizeof( address ) - 1] = '\0';

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, address, NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
}
REGISTER_UNIT_TEST( test_create_reports_status_invalid_address );

// ---- the sent-packet map against the window ---------------------------------

// A packets_in_flight below reliable_messages_in_flight is refused at create
// with CONFIG_OUT_OF_RANGE, client and server (docs/CHANNELS.md 14,
// standard/MESSAGES.md m9.1; tla/RNSentMap.tla, the reversed witness
// MCRNSentMapBrokenSmallMap.cfg): one entry short of the window is refused,
// and a map equal to the window creates. Red before the rule: both create.

static void test_create_refuses_sent_map_below_window()
{
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.reliable_messages_in_flight = 1024;
    client_config.packets_in_flight = 1023;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;
    server_config.reliable_messages_in_flight = 1024;
    server_config.packets_in_flight = 1023;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // the map equal to the window creates

    client_config.packets_in_flight = 1024;
    status = ROCKETNET_TEST_STATUS_UNSET;

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );
}
REGISTER_UNIT_TEST( test_create_refuses_sent_map_below_window );

// ---- config refusals --------------------------------------------------------

static void test_create_reports_status_config_refusals()
{
    // a field out of range

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.num_channels = 0;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 0;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // A value supplied by the caller is a config-range refusal, even though
    // create also checks environment overrides. Keep the environment absent
    // for this distinction, then restore it for the process running the suite.
#if ROCKETNET_DEV
    std::string saved_client_env;
    std::string saved_server_env;
    const bool had_client_env = create_status_getenv( "ROCKETNET_CLIENT_SEND_KBPS", saved_client_env );
    const bool had_server_env = create_status_getenv( "ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS", saved_server_env );
    check( create_status_unsetenv( "ROCKETNET_CLIENT_SEND_KBPS" ) == 0 );
    check( create_status_unsetenv( "ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS" ) == 0 );

#endif // #if ROCKETNET_DEV
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    // ROCKETNET_SEND_KBPS_UNLIMITED is no budget: accepted, with no ceiling

    client_config.send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    status = ROCKETNET_STATUS_INVALID_ARGUMENT;

    struct rocketnet_client_t * unlimited_client = rocketnet_client_create( NULL, &client_config, &status );
    check( unlimited_client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( unlimited_client );

    // four clients, as every server here: a full 1000-client server plans
    // 13 GB and touches about 1.5 GB of it at create

    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;
    server_config.per_client_send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    status = ROCKETNET_STATUS_INVALID_ARGUMENT;

    struct rocketnet_server_t * unlimited_server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( unlimited_server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_server_stop_and_destroy( unlimited_server );

    // a negative rate is refused as an invalid argument; 0 is the default rate

    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.send_kbps = -1;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    client_config.send_kbps = 0;
    status = ROCKETNET_TEST_STATUS_UNSET;

    struct rocketnet_client_t * default_rate_client = rocketnet_client_create( NULL, &client_config, &status );
    check( default_rate_client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( default_rate_client );

    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;
    server_config.per_client_send_kbps = -1;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

#if ROCKETNET_DEV
    if ( had_client_env )
        check( create_status_setenv( "ROCKETNET_CLIENT_SEND_KBPS", saved_client_env.c_str() ) == 0 );
    else
        check( create_status_unsetenv( "ROCKETNET_CLIENT_SEND_KBPS" ) == 0 );
    if ( had_server_env )
        check( create_status_setenv( "ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS", saved_server_env.c_str() ) == 0 );
    else
        check( create_status_unsetenv( "ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS" ) == 0 );
#endif // #if ROCKETNET_DEV

    // a transport list that does not parse

    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "no-such-transport";
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );

    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;
    server_config.transport = "no-such-transport";
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );

    // a config whose reassembly geometry is over the memory budget: the
    // server trigger (max clients x max slots) the transport case proves

    create_status_dev_server_config( &server_config );
    server_config.max_clients = ROCKETNET_MAX_CLIENTS;
    server_config.max_fragmented_messages = 256;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED );

    // MEMORY_BUDGET_EXCEEDED on the client is not reachable from a valid
    // config: at the largest geometry (1 connection, 256 slots, 305408
    // payload bytes) the reassembly set needs about 117 MiB, under the
    // 512 MiB budget (sdk/rocketnet_memory.h:8, sdk/rocketnet_api.cpp:405),
    // and every larger field value is refused as CONFIG_OUT_OF_RANGE first.
    // The server case above proves the code; here the largest client
    // geometry creates.

    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.max_fragmented_messages = 256;
    status = ROCKETNET_TEST_STATUS_UNSET;

    struct rocketnet_client_t * budget_client = rocketnet_client_create( NULL, &client_config, &status );
    check( budget_client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( budget_client );

#if ROCKETNET_DEV
    // a malformed environment override: CONFIG_ENV_INVALID, client and server

    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    std::string prior_client;
    const bool prior_client_present = create_status_getenv( "ROCKETNET_CLIENT_SEND_KBPS", prior_client );
    check( create_status_setenv( "ROCKETNET_CLIENT_SEND_KBPS", "not-a-number" ) == 0 );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    if ( prior_client_present )
        check( create_status_setenv( "ROCKETNET_CLIENT_SEND_KBPS", prior_client.c_str() ) == 0 );
    else
        check( create_status_unsetenv( "ROCKETNET_CLIENT_SEND_KBPS" ) == 0 );

    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;

    std::string prior_server;
    const bool prior_server_present = create_status_getenv( "ROCKETNET_SERVER_MAX_CLIENTS", prior_server );
    check( create_status_setenv( "ROCKETNET_SERVER_MAX_CLIENTS", "not-a-number" ) == 0 );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    if ( prior_server_present )
        check( create_status_setenv( "ROCKETNET_SERVER_MAX_CLIENTS", prior_server.c_str() ) == 0 );
    else
        check( create_status_unsetenv( "ROCKETNET_SERVER_MAX_CLIENTS" ) == 0 );
#endif // #if ROCKETNET_DEV
}
REGISTER_UNIT_TEST( test_create_reports_status_config_refusals );

// ---- a failed block allocation ----------------------------------------------

static void test_create_reports_status_out_of_memory()
{
    create_status_allocator_t state;
    memset( &state, 0, sizeof( state ) );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    int init_status = -1;
    check( rocketnet_init_ex( &state, create_status_allocate, create_status_release, &init_status ) );
    check( init_status == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;

    // walk every allocation client create makes: each failure is
    // OUT_OF_MEMORY with nothing left allocated, then success

    for ( int fail_at = 1;; fail_at++ )
    {
        check( fail_at < 64 );
        state.calls = 0;
        state.fail_at = fail_at;

        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        struct rocketnet_client_t * client = rocketnet_client_create( &state, &client_config, &status );
        if ( client )
        {
            check( status == ROCKETNET_STATUS_OK );
            rn_test_client_stop_and_destroy( client );
            check( state.live == 0 );
            break;
        }

        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( state.live == 0 );
    }

    for ( int fail_at = 1;; fail_at++ )
    {
        check( fail_at < 64 );
        state.calls = 0;
        state.fail_at = fail_at;

        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        struct rocketnet_server_t * server = rocketnet_server_create( &state, "127.0.0.1:0", NULL, &server_config, &status );
        if ( server )
        {
            check( status == ROCKETNET_STATUS_OK );
            rn_test_server_stop_and_destroy( server );
            check( state.live == 0 );
            break;
        }

        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( state.live == 0 );
    }

    state.fail_at = 0;
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}
REGISTER_UNIT_TEST( test_create_reports_status_out_of_memory );

// ---- the block allocation's refusal is one error line -----------------------

struct create_status_log_t
{
    int errors;
    char last[256];
};

static void create_status_count_errors( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) length;

    create_status_log_t * log = (create_status_log_t *) context;
    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
        return;
    log->errors++;
    snprintf( log->last, sizeof( log->last ), "%s", message );
}

static void create_oom_log_one_side( create_status_allocator_t * state, create_status_log_t * log, bool server,
                                     const struct rocketnet_client_config_t * client_config, const struct rocketnet_server_config_t * server_config,
                                     size_t block_bytes, const char * side )
{
    int block_fail = 0;

    for ( int fail_at = 1; fail_at < 64; fail_at++ )
    {
        state->calls = 0;
        state->fail_at = fail_at;
        state->last_bytes = 0;
        log->errors = 0;
        log->last[0] = '\0';

        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;

        if ( !server )
        {
            struct rocketnet_client_t * client = rocketnet_client_create( state, client_config, &status );
            if ( client )
            {
                check( status == ROCKETNET_STATUS_OK );
                rn_test_client_stop_and_destroy( client );
                check( state->live == 0 );
                break;
            }
        }
        else
        {
            struct rocketnet_server_t * created = rocketnet_server_create( state, "127.0.0.1:0", NULL, server_config, &status );
            if ( created )
            {
                check( status == ROCKETNET_STATUS_OK );
                rn_test_server_stop_and_destroy( created );
                check( state->live == 0 );
                break;
            }
        }

        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( state->live == 0 );

        if ( state->last_bytes == block_bytes )
        {
            block_fail = fail_at;
            check( log->errors == 1 );
            check( strstr( log->last, side ) != NULL );
        }
    }

    check( block_fail != 0 );
}

static void test_create_out_of_memory_logs_one_error()
{
    create_status_allocator_t state;
    memset( &state, 0, sizeof( state ) );

    create_status_log_t log;
    memset( &log, 0, sizeof( log ) );

    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_log_function( create_status_count_errors, &log );

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    int init_status = -1;
    check( rocketnet_init_ex( &state, create_status_allocate, create_status_release, &init_status ) );
    check( init_status == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;

    // required_memory is the lifecycle peak (create plus the transport connect
    // or start takes), so create's one block is read off a clean create: the
    // largest single allocation it makes, never above that peak

    state.max_bytes = 0;

    enum rocketnet_status_t sized_status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * sized_client = rocketnet_client_create( &state, &client_config, &sized_status );
    check( sized_client != NULL );
    check( sized_status == ROCKETNET_STATUS_OK );

    const size_t client_block = state.max_bytes;
    rn_test_client_stop_and_destroy( sized_client );
    check( state.live == 0 );
    state.max_bytes = 0;
    sized_status = ROCKETNET_TEST_STATUS_UNSET;

    struct rocketnet_server_t * sized_server = rocketnet_server_create( &state, "127.0.0.1:0", NULL, &server_config, &sized_status );
    check( sized_server != NULL );
    check( sized_status == ROCKETNET_STATUS_OK );

    const size_t server_block = state.max_bytes;
    rn_test_server_stop_and_destroy( sized_server );
    check( state.live == 0 );
    check( client_block != 0 && client_block <= rocketnet_client_required_memory( &client_config ) );
    check( server_block != 0 && server_block <= rocketnet_server_required_memory( &server_config ) );

    create_oom_log_one_side( &state, &log, false, &client_config, &server_config, client_block, "client create refused" );
    create_oom_log_one_side( &state, &log, true, &client_config, &server_config, server_block, "server create refused" );

    state.fail_at = 0;
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}
REGISTER_UNIT_TEST( test_create_out_of_memory_logs_one_error );

// ---- a NULL status, on a refusal and on a success ---------------------------

static void test_create_reports_status_null_status()
{
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    create_status_dev_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    rn_test_client_stop_and_destroy( client );

    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    rn_test_server_stop_and_destroy( server );

    client_config.num_channels = 0;
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    server_config.max_clients = 0;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );
}
REGISTER_UNIT_TEST( test_create_reports_status_null_status );

// ---- a NULL config -----------------------------------------------------------
// A NULL config is the defaults: this case compiles in C++ (this file) and
// the same calls in tools/header_alone/main.c compile as C99 and as C++11,
// each under -pedantic -Werror (tools/check_public_header.py). A server with
// a NULL config is not created here: its default client count sizes a block
// far past the unit suite's memory bound, so the server side is exercised
// through required_memory below.

static void test_create_reports_status_null_config_macro()
{
    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, NULL, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );

    check( rocketnet_client_required_memory( NULL ) > 0 );
    check( rocketnet_server_required_memory( NULL ) > 0 );
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( create_status_quiet_assert );
    check( rocketnet_client_default_config( NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_default_config( NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_client_default_config_internal( NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_default_config_internal( NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_assert_function( saved_assert );
}
REGISTER_UNIT_TEST( test_create_reports_status_null_config_macro );

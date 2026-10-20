/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public create reports its status through an out-parameter (owner ruling,
// 2026-10-20): one create per side, always taking config_bytes and a final
// status out-parameter, with the same-named convenience macros passing
// sizeof(*config). Each refusal path names its code here, for the client
// and the server, through the sized function and through the macro: before
// init, an unknown config size, a NULL or over-long address, a bad config
// value, a bad transport list, an over-budget config, a short or misaligned
// memory block, and a failed block allocation (the failing allocator of
// test_unit_driver_create_out_of_memory.cpp, walking every allocation
// create makes). A NULL status is accepted on a refusal and on a success.
// No socket, no thread, no real time. The CONFIG_ENV_INVALID case sets one
// environment variable per side and restores it before returning, as
// test_unit_status_client_transport_create_ex.cpp does; every other case
// sets no environment variable.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

static void create_status_quiet_assert( const char *, const char *, const char *, int ) {}

#if defined( _WIN32 )
static int create_status_setenv( const char * name, const char * value ) { return _putenv_s( name, value ); }
static int create_status_unsetenv( const char * name ) { return _putenv_s( name, "" ); }
#else
static int create_status_setenv( const char * name, const char * value ) { return setenv( name, value, 1 ); }
static int create_status_unsetenv( const char * name ) { return unsetenv( name ); }
#endif

// ---- the fault-injecting allocator -----------------------------------------

struct create_status_allocator_t
{
    int live;
    int calls;
    int fail_at;
};

static void * create_status_allocate( void * context, size_t bytes )
{
    create_status_allocator_t * state = (create_status_allocator_t *) context;
    const int call = ++state->calls;
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

// ---- success writes OK, through the macro and the sized function -----------

static void create_reports_status_success()
{
    struct rocketnet_client_config_t client_config;
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;

    enum rocketnet_status_t status = ROCKETNET_STATUS_COUNT;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );

    status = ROCKETNET_STATUS_COUNT;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );

    status = ROCKETNET_STATUS_COUNT;
    client = (rocketnet_client_create_sized)( NULL, &client_config, sizeof( client_config ), NULL, 0, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );

    status = ROCKETNET_STATUS_COUNT;
    server = (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), NULL, 0, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );
}
REGISTER_UNIT_TEST( create_reports_status_success );

// ---- before init ------------------------------------------------------------

static void create_reports_status_not_initialized()
{
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( create_status_quiet_assert );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( !rocketnet_library_initialized() );

    struct rocketnet_client_config_t client_config;
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_client_create_sized)( NULL, &client_config, sizeof( client_config ), NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );
    // a NULL status is accepted on this refusal too
    check( rocketnet_client_create( NULL, &client_config, NULL, 0, NULL ) == NULL );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, NULL ) == NULL );

    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_set_assert_callback( saved_assert );
}
REGISTER_UNIT_TEST( create_reports_status_not_initialized );

// ---- bad arguments ----------------------------------------------------------

static void create_reports_status_invalid_argument()
{
    struct rocketnet_client_config_t client_config;
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;

    // an unknown config size, before any other check
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( (rocketnet_client_create_sized)( NULL, &client_config, sizeof( client_config ) - 4, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) - 4, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // a NULL server address (the null guard asserts in Debug: quieted here)
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( create_status_quiet_assert );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, NULL, &server_config, NULL, 0, &status ) == NULL );
    rocketnet_set_assert_callback( saved_assert );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // a short memory block, then a misaligned one
    const size_t client_bytes = rocketnet_client_required_memory( &client_config );
    check( client_bytes > 0 );
    static uint8_t client_block[65536];
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_client_create_sized)( NULL, &client_config, sizeof( client_config ), client_block, client_bytes - 1, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_client_create_sized)( NULL, &client_config, sizeof( client_config ), client_block + 1, client_bytes, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    const size_t server_bytes = rocketnet_server_required_memory( &server_config );
    check( server_bytes > 0 );
    static uint8_t server_block[65536];
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), server_block, server_bytes - 1, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), server_block + 1, server_bytes, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
}
REGISTER_UNIT_TEST( create_reports_status_invalid_argument );

// ---- a known prefix size, and an unknown larger size ------------------------

static void create_reports_status_prefix_size()
{
    struct rocketnet_client_config_t client_config;
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;

    // a smaller known size reads the caller's prefix and defaults the tail
    enum rocketnet_status_t status = ROCKETNET_STATUS_COUNT;
    struct rocketnet_client_t * client = (rocketnet_client_create_sized)( NULL, &client_config, ROCKETNET_CLIENT_CONFIG_BYTES_1, NULL, 0, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );

    status = ROCKETNET_STATUS_COUNT;
    struct rocketnet_server_t * server = (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, ROCKETNET_SERVER_CONFIG_BYTES_1, NULL, 0, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );

    // the same prefix through required_memory: a known size sizes, an
    // unknown larger size returns 0
    check( (rocketnet_client_required_memory_sized)( &client_config, ROCKETNET_CLIENT_CONFIG_BYTES_1 ) > 0 );
    check( (rocketnet_server_required_memory_sized)( &server_config, ROCKETNET_SERVER_CONFIG_BYTES_1 ) > 0 );
    check( (rocketnet_client_required_memory_sized)( &client_config, sizeof( client_config ) + 8 ) == 0 );
    check( (rocketnet_server_required_memory_sized)( &server_config, sizeof( server_config ) + 8 ) == 0 );

    // an unknown larger size is refused before any other check
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_client_create_sized)( NULL, &client_config, sizeof( client_config ) + 8, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) + 8, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
}
REGISTER_UNIT_TEST( create_reports_status_prefix_size );

// ---- an over-long address ---------------------------------------------------

static void create_reports_status_invalid_address()
{
    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;

    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH + 32];
    memset( address, 'a', sizeof( address ) - 1 );
    address[sizeof( address ) - 1] = '\0';

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, address, &server_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_server_create_sized)( NULL, address, &server_config, sizeof( server_config ), NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
}
REGISTER_UNIT_TEST( create_reports_status_invalid_address );

// ---- config refusals --------------------------------------------------------

static void create_reports_status_config_refusals()
{
    // a field out of range
    struct rocketnet_client_config_t client_config;
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    client_config.num_channels = 0;
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_client_create_sized)( NULL, &client_config, sizeof( client_config ), NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 0;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    status = ROCKETNET_STATUS_OK;
    check( (rocketnet_server_create_sized)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // a transport list that does not parse
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    client_config.transport = "no-such-transport";
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );

    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;
    server_config.transport = "no-such-transport";
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );

    // a config whose reassembly geometry is over the memory budget: the
    // server trigger (max clients x max slots) the transport case proves
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = ROCKETNET_MAX_CLIENTS;
    server_config.num_reassembly_slots = 256;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED );
    // validate runs the same check and refuses the same config (as
    // CONFIG_OUT_OF_RANGE; create names the budget code above)
    check( (rocketnet_server_validate_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    check( rocketnet_server_validate_config( &server_config ) == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // MEMORY_BUDGET_EXCEEDED on the client is not reachable from a valid
    // config: at the largest geometry (1 connection, 256 slots, 305408
    // payload bytes) the reassembly set needs about 117 MiB, under the
    // 512 MiB budget (sdk/rocketnet_memory.h:8, sdk/rocketnet_api.cpp:405),
    // and every larger field value is refused as CONFIG_OUT_OF_RANGE first.
    // The server case above proves the code; here the largest client
    // geometry validates and creates.
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    client_config.num_reassembly_slots = 256;
    client_config.max_payload_bytes = 305408;
    check( rocketnet_client_validate_config( &client_config ) == ROCKETNET_STATUS_OK );
    status = ROCKETNET_STATUS_COUNT;
    struct rocketnet_client_t * budget_client = rocketnet_client_create( NULL, &client_config, NULL, 0, &status );
    check( budget_client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( budget_client );

    // a malformed environment override: CONFIG_ENV_INVALID, client and server
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    const char * prior_client = getenv( "ROCKETNET_CLIENT_SEND_KBPS" );
    check( create_status_setenv( "ROCKETNET_CLIENT_SEND_KBPS", "not-a-number" ) == 0 );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    if ( prior_client ) check( create_status_setenv( "ROCKETNET_CLIENT_SEND_KBPS", prior_client ) == 0 );
    else check( create_status_unsetenv( "ROCKETNET_CLIENT_SEND_KBPS" ) == 0 );

    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;
    const char * prior_server = getenv( "ROCKETNET_SERVER_MAX_CLIENTS" );
    check( create_status_setenv( "ROCKETNET_SERVER_MAX_CLIENTS", "not-a-number" ) == 0 );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    if ( prior_server ) check( create_status_setenv( "ROCKETNET_SERVER_MAX_CLIENTS", prior_server ) == 0 );
    else check( create_status_unsetenv( "ROCKETNET_SERVER_MAX_CLIENTS" ) == 0 );
}
REGISTER_UNIT_TEST( create_reports_status_config_refusals );

// ---- a failed block allocation ----------------------------------------------

static void create_reports_status_out_of_memory()
{
    create_status_allocator_t state;
    memset( &state, 0, sizeof( state ) );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_allocator_ex( create_status_allocate, create_status_release ) == ROCKETNET_STATUS_OK );
    int init_status = -1;
    check( rocketnet_init_ex( &state, &init_status ) );
    check( init_status == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;

    // walk every allocation client create makes: each failure is
    // OUT_OF_MEMORY with nothing left allocated, then success
    for ( int fail_at = 1; ; fail_at++ )
    {
        check( fail_at < 64 );
        state.calls = 0;
        state.fail_at = fail_at;
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        struct rocketnet_client_t * client = rocketnet_client_create( &state, &client_config, NULL, 0, &status );
        if ( client )
        {
            check( status == ROCKETNET_STATUS_OK );
            rocketnet_client_destroy( client );
            check( state.live == 0 );
            break;
        }
        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( state.live == 0 );
    }

    for ( int fail_at = 1; ; fail_at++ )
    {
        check( fail_at < 64 );
        state.calls = 0;
        state.fail_at = fail_at;
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        struct rocketnet_server_t * server = rocketnet_server_create( &state, "127.0.0.1:0", &server_config, NULL, 0, &status );
        if ( server )
        {
            check( status == ROCKETNET_STATUS_OK );
            rocketnet_server_destroy( server );
            check( state.live == 0 );
            break;
        }
        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( state.live == 0 );
    }

    state.fail_at = 0;
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
}
REGISTER_UNIT_TEST( create_reports_status_out_of_memory );

// ---- a NULL status, on a refusal and on a success ---------------------------

static void create_reports_status_null_status()
{
    struct rocketnet_client_config_t client_config;
    check( (rocketnet_client_default_config_sized)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    check( (rocketnet_server_dev_config_sized)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0, NULL );
    check( client != NULL );
    rocketnet_client_destroy( client );
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, NULL );
    check( server != NULL );
    rocketnet_server_destroy( server );

    client_config.num_channels = 0;
    check( rocketnet_client_create( NULL, &client_config, NULL, 0, NULL ) == NULL );
    server_config.max_clients = 0;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, NULL ) == NULL );
}
REGISTER_UNIT_TEST( create_reports_status_null_status );

// ---- a NULL config -----------------------------------------------------------
// A NULL config is the defaults: this case compiles in C++ (this file) and
// the same calls in tools/header_alone/main.c compile as C99 and as C++11,
// each under -pedantic -Werror (tools/check_public_header.py). A server with
// a NULL config is not created here: its default client count sizes a block
// far past the unit suite's memory bound, so the server side is exercised
// through validate_config and required_memory below.

static void create_reports_status_null_config_macro()
{
    enum rocketnet_status_t status = ROCKETNET_STATUS_COUNT;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, NULL, NULL, 0, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );

    check( rocketnet_client_validate_config( NULL ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_validate_config( NULL ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_required_memory( NULL ) > 0 );
    check( rocketnet_server_required_memory( NULL ) > 0 );
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( create_status_quiet_assert );
    check( rocketnet_client_default_config( NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_default_config( NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_dev_config( NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_set_assert_callback( saved_assert );
}
REGISTER_UNIT_TEST( create_reports_status_null_config_macro );

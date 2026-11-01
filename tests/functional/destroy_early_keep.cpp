/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// destroy_early_keep.cpp -- early destroy on the platform's default threaded
// transport. A destroy before completion logs and counts the misuse, then
// polls the stop for a bounded wall-clock budget. A worker that exits inside
// it is joined and the object freed, so "disconnect, destroy" frees and term
// returns OK. A worker that has not exited when the budget ends keeps the
// object: the keep cases set the budget to 0 (one pass, no sleep), the
// handle stays valid, the completion query later reads true, and a second
// destroy then frees the object and returns the allocator to its baseline.
// The loopback/step-driven path lives in
// tests/test_unit_destroy_before_completion.cpp.

#include "../token_string.h"
#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static void destroy_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define destroy_check( condition )                                                                             \
    do                                                                                                         \
    {                                                                                                          \
        if ( !( condition ) )                                                                                  \
            destroy_check_handler( #condition, (const char *) __FUNCTION__, __FILE__, __LINE__ );              \
    } while ( 0 )

#define check destroy_check

extern void rocketnet_functional_restore_log_function();
extern void rocketnet_functional_restore_assert_function();

// Clear the transport pins so create resolves the default threaded ladder.
class destroy_env_guard_t
{
public:
    explicit destroy_env_guard_t( const char * variable )
        : name( variable ), present( false )
    {
#if defined( _WIN32 )
        char * current = NULL;
        size_t current_bytes = 0;
        check( _dupenv_s( &current, &current_bytes, name.c_str() ) == 0 );
        if ( current )
        {
            present = true;
            value = current;
            free( current );
        }
        check( _putenv_s( name.c_str(), "" ) == 0 );
#else
        const char * current = getenv( name.c_str() );
        if ( current )
        {
            present = true;
            value = current;
        }
        check( unsetenv( name.c_str() ) == 0 );
#endif
    }

    ~destroy_env_guard_t()
    {
#if defined( _WIN32 )
        (void) _putenv_s( name.c_str(), present ? value.c_str() : "" );
#else
        if ( present )
            (void) setenv( name.c_str(), value.c_str(), 1 );
        else
            (void) unsetenv( name.c_str() );
#endif
    }

private:
    std::string name;
    bool present;
    std::string value;
};

// --- the counting allocator ------------------------------------------------

struct destroy_allocator_t
{
    int live;
};

static void * destroy_allocator_malloc( void * context, size_t bytes )
{
    destroy_allocator_t * state = (destroy_allocator_t *) context;
    void * memory = malloc( bytes );
    if ( memory && state )
        state->live++;
    return memory;
}

static void destroy_allocator_free( void * context, void * memory )
{
    destroy_allocator_t * state = (destroy_allocator_t *) context;
    if ( memory && state )
        state->live--;
    free( memory );
}

// --- the assert recorder: an early destroy runs the release path ----------
// The existing destroy-before-completion test handles the debug assert on
// this path the same way: it installs a recorder and never disables it.

static int destroy_assert_count = 0;

static void destroy_assert_recorder( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    destroy_assert_count++;
}

// --- the log capture -------------------------------------------------------

static int destroy_error_lines = 0;
static char destroy_last_error[1024];

static void destroy_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
        return;
    destroy_error_lines++;
    const size_t copied = length < sizeof( destroy_last_error ) - 1 ? length : sizeof( destroy_last_error ) - 1;
    memcpy( destroy_last_error, message, copied );
    destroy_last_error[copied] = '\0';
}

// --- the fixture -----------------------------------------------------------

static const uint64_t DESTROY_EARLY_PROTOCOL = 0x44424710C0DE0002ULL;

struct destroy_pair_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    double wall_start;
};

static void destroy_pair_create( destroy_pair_t * pair, void * context )
{
    memset( pair, 0, sizeof( *pair ) );
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5A ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, pair->backend_public, pair->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = DESTROY_EARLY_PROTOCOL;
    server_config.max_clients = 4;
    memcpy( server_config.backend_public_key, pair->backend_public, sizeof( server_config.backend_public_key ) );
    // transport is left at its default, so the threaded ladder decides.
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    pair->server = rocketnet_server_create( context, "127.0.0.1:0", NULL, &server_config, &status );
    check( pair->server != NULL && status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_transport_name( pair->server ) != NULL );
    check( strcmp( rocketnet_server_transport_name( pair->server ), "loopback" ) != 0 );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    pair->client = rocketnet_client_create( context, &client_config, &status );
    check( pair->client != NULL && status == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( pair->server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( pair->backend_secret, pair->server, DESTROY_EARLY_PROTOCOL,
                                        addresses, 1, 0xD7001ULL, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( pair->client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_transport_name( pair->client ) != NULL );
    check( strcmp( rocketnet_client_transport_name( pair->client ), "loopback" ) != 0 );

    pair->wall_start = rocketnet_time();
    const double start = rocketnet_time();
    while ( rocketnet_time() - start < 5.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - pair->wall_start );
        rocketnet_client_update( pair->client, now );
        rocketnet_client_send_packets( pair->client );
        rocketnet_server_update( pair->server, now );
        rocketnet_server_send_packets( pair->server );
        if ( rocketnet_client_state( pair->client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            break;
        rocketnet_sleep( 0.001 );
    }
    check( rocketnet_client_state( pair->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
}

static double destroy_now( const destroy_pair_t * pair )
{
    return 1000.0 + ( rocketnet_time() - pair->wall_start );
}

static void destroy_server_teardown( destroy_pair_t * pair )
{
    rocketnet_server_stop( pair->server );
    const double start = rocketnet_time();
    while ( !rocketnet_server_stopped( pair->server ) && rocketnet_time() - start < 5.0 )
    {
        rocketnet_server_update( pair->server, destroy_now( pair ) );
        rocketnet_server_send_packets( pair->server );
        rocketnet_sleep( 0.001 );
    }
    check( rocketnet_server_stopped( pair->server ) );
    rocketnet_server_destroy( pair->server );
}

static void destroy_client_teardown( destroy_pair_t * pair )
{
    rocketnet_client_disconnect( pair->client );
    const double start = rocketnet_time();
    while ( !rocketnet_client_disconnected( pair->client ) && rocketnet_time() - start < 5.0 )
    {
        rocketnet_client_update( pair->client, destroy_now( pair ) );
        rocketnet_client_send_packets( pair->client );
        rocketnet_sleep( 0.001 );
    }
    check( rocketnet_client_disconnected( pair->client ) );
    rocketnet_client_destroy( pair->client );
}

// Whether a worker exits within destroy's one pass is the scheduler's call: a
// destroy that sees it exit frees the handle (one error line), which is
// correct but not the keep path under test, so the pair is torn down and the
// case tries again, at most this many times.
static const int DESTROY_KEEP_ATTEMPTS = 20;

static void destroy_begin_counted( destroy_allocator_t * state, int * before )
{
    rocketnet_assert_function( destroy_assert_recorder );
    destroy_assert_count = 0;
    rocketnet_log_function( destroy_log_capture, NULL );
    destroy_error_lines = 0;
    int status = -1;
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init_ex( state, destroy_allocator_malloc, destroy_allocator_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );
    *before = state->live;
}

static void destroy_end_counted( destroy_allocator_t * state )
{
    rocketnet_destroy_stop_seconds_for_test( -1.0 );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state->live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_functional_restore_log_function();
    rocketnet_functional_restore_assert_function();
}

// --- cases -----------------------------------------------------------------

// disconnect or stop, then destroy at once with no completion poll: the
// destroy waits out the worker inside its budget, frees, and term is clean

void test_client_destroy_early_threaded_frees()
{
    destroy_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    destroy_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    destroy_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    destroy_allocator_t state = { 0 };
    int before = 0;
    destroy_begin_counted( &state, &before );

    destroy_pair_t pair;
    destroy_pair_create( &pair, &state );
    const uint64_t misuse = rocketnet_api_misuse_count();
    destroy_error_lines = 0;
    rocketnet_client_disconnect( pair.client );
    rocketnet_client_destroy( pair.client );
    check( destroy_error_lines == 1 );
    check( strstr( destroy_last_error, "rocketnet_client_destroy" ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );

    destroy_server_teardown( &pair );
    check( state.live == before );

    destroy_end_counted( &state );
}

void test_server_destroy_early_threaded_frees()
{
    destroy_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    destroy_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    destroy_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    destroy_allocator_t state = { 0 };
    int before = 0;
    destroy_begin_counted( &state, &before );

    destroy_pair_t pair;
    destroy_pair_create( &pair, &state );
    const uint64_t misuse = rocketnet_api_misuse_count();
    destroy_error_lines = 0;
    rocketnet_server_stop( pair.server );
    rocketnet_server_destroy( pair.server );
    check( destroy_error_lines == 1 );
    check( strstr( destroy_last_error, "rocketnet_server_destroy" ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );

    destroy_client_teardown( &pair );
    check( state.live == before );

    destroy_end_counted( &state );
}

// the keep path: with no budget the destroy makes one pass and does not sleep

void test_client_destroy_early_threaded_keeps_then_frees()
{
    destroy_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    destroy_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    destroy_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    destroy_allocator_t state = { 0 };
    int before = 0;
    destroy_begin_counted( &state, &before );
    rocketnet_destroy_stop_seconds_for_test( 0.0 );

    destroy_pair_t pair;
    uint64_t misuse = 0;
    for ( int attempt = 0;; attempt++ )
    {
        check( attempt < DESTROY_KEEP_ATTEMPTS );
        destroy_pair_create( &pair, &state );
        misuse = rocketnet_api_misuse_count();
        destroy_error_lines = 0;
        rocketnet_client_destroy( pair.client );
        if ( destroy_error_lines == 2 )
            break;
        check( destroy_error_lines == 1 );
        destroy_server_teardown( &pair );
        check( state.live == before );
    }
    check( strstr( destroy_last_error, "rocketnet_client_destroy" ) != NULL );
    check( strstr( destroy_last_error, "kept, not freed" ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    // The handle stays valid: its memory is not freed and its queries answer.
    check( state.live > before );
    (void) rocketnet_client_state( pair.client );

    const double start = rocketnet_time();
    while ( !rocketnet_client_disconnected( pair.client ) && rocketnet_time() - start < 5.0 )
    {
        rocketnet_server_update( pair.server, destroy_now( &pair ) );
        rocketnet_server_send_packets( pair.server );
        rocketnet_sleep( 0.001 );
    }
    check( rocketnet_client_disconnected( pair.client ) );

    destroy_error_lines = 0;
    rocketnet_client_destroy( pair.client );
    check( destroy_error_lines == 0 );
    check( rocketnet_api_misuse_count() == misuse + 1 );

    destroy_server_teardown( &pair );
    check( state.live == before );

    destroy_end_counted( &state );
}

void test_server_destroy_early_threaded_keeps_then_frees()
{
    destroy_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    destroy_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    destroy_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    destroy_allocator_t state = { 0 };
    int before = 0;
    destroy_begin_counted( &state, &before );
    rocketnet_destroy_stop_seconds_for_test( 0.0 );

    destroy_pair_t pair;
    uint64_t misuse = 0;
    for ( int attempt = 0;; attempt++ )
    {
        check( attempt < DESTROY_KEEP_ATTEMPTS );
        destroy_pair_create( &pair, &state );
        misuse = rocketnet_api_misuse_count();
        destroy_error_lines = 0;
        rocketnet_server_destroy( pair.server );
        if ( destroy_error_lines == 2 )
            break;
        check( destroy_error_lines == 1 );
        destroy_client_teardown( &pair );
        check( state.live == before );
    }
    check( strstr( destroy_last_error, "rocketnet_server_destroy" ) != NULL );
    check( strstr( destroy_last_error, "kept, not freed" ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( state.live > before );
    (void) rocketnet_server_running( pair.server );

    const double start = rocketnet_time();
    while ( !rocketnet_server_stopped( pair.server ) && rocketnet_time() - start < 5.0 )
    {
        rocketnet_client_update( pair.client, destroy_now( &pair ) );
        rocketnet_client_send_packets( pair.client );
        rocketnet_sleep( 0.001 );
    }
    check( rocketnet_server_stopped( pair.server ) );

    destroy_error_lines = 0;
    rocketnet_server_destroy( pair.server );
    check( destroy_error_lines == 0 );
    check( rocketnet_api_misuse_count() == misuse + 1 );

    destroy_client_teardown( &pair );
    check( state.live == before );

    destroy_end_counted( &state );
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_destroy_before_completion.cpp -- rocketnet_client_destroy and
// rocketnet_server_destroy stay safe and loud when the caller skipped the
// completion poll the contract requires. An early destroy logs one error
// naming the call and the skipped completion function, records one API
// misuse, requests shutdown itself, waits for the worker to complete, and
// then frees exactly as the normal path does. A destroy after completion
// logs nothing and counts nothing.
//
// Everything runs over the in-memory loopback transport with step-driven
// clients: no socket, no thread, no wall clock; time is an argument starting
// at 1000.0. This file covers the loopback/step-driven path, where the early
// destroy completes because the loopback stop is always complete. The
// threaded path, where an early destroy normally keeps the object and a
// second destroy after completion frees it, is covered by
// tests/functional/destroy_early_keep.cpp.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "token_string.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check
#define DESTROY_BEFORE_COMPLETION_PROTOCOL 0x44424710C0DE0001ULL
#define DESTROY_BEFORE_COMPLETION_MAX_CLIENTS 4

static const char * const DB_DISCONNECTED = "rocketnet_client_disconnected";
static const char * const DB_STOPPED = "rocketnet_server_stopped";

// --- the counting allocator ------------------------------------------------

struct db_allocator_t
{
    int live;
};

static void * db_allocator_malloc( void * context, size_t bytes )
{
    db_allocator_t * state = (db_allocator_t *) context;
    void * memory = malloc( bytes );
    if ( memory && state )
    {
        state->live++;
    }

    return memory;
}

static void db_allocator_free( void * context, void * memory )
{
    db_allocator_t * state = (db_allocator_t *) context;
    if ( memory && state )
    {
        state->live--;
    }
    free( memory );
}

// --- the assert recorder: an early destroy runs the release path ----------

static int db_assert_count = 0;

static void db_assert_recorder( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    db_assert_count++;
}

// --- the log capture -------------------------------------------------------

static int db_error_lines = 0;
static char db_last_error[1024];

static void db_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
        return;
    db_error_lines++;

    const size_t copied = length < sizeof( db_last_error ) - 1 ? length : sizeof( db_last_error ) - 1;
    memcpy( db_last_error, message, copied );
    db_last_error[copied] = '\0';
}

// --- the fixture -----------------------------------------------------------

struct db_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
};

static double db_clock( void * ) { return 1000.0; }

static void db_identity( db_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
}

struct db_world_t
{
    db_identity_t identity;
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    void * context;
    double time;
};

static void db_world_create( db_world_t * world, void * context )
{
    memset( world, 0, sizeof( *world ) );
    world->context = context;
    world->time = 1000.0;
    db_identity( &world->identity );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = DESTROY_BEFORE_COMPLETION_PROTOCOL;
    server_config.max_clients = DESTROY_BEFORE_COMPLETION_MAX_CLIENTS;
    memcpy( server_config.backend_public_key, world->identity.backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    world->server = rocketnet_server_create( context, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world->server != NULL );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = db_clock;
    world->client = rocketnet_client_create( context, &client_config, NULL );
    check( world->client != NULL );
    check( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
}

static void db_pump( db_world_t * world )
{
    rocketnet_client_update( world->client, world->time );
    rocketnet_client_send_packets( world->client );
    rocketnet_server_update( world->server, world->time );
    rocketnet_server_send_packets( world->server );
    world->time += 0.01;
}

static void db_connect( db_world_t * world, uint64_t client_id )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );

    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( world->identity.backend_secret, world->server,
                                        DESTROY_BEFORE_COMPLETION_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( world->client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( world->client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        db_pump( world );
    }
    check( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int index = rocketnet_client_index( world->client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world->server, index ); i++ )
    {
        db_pump( world );
    }
    check( rocketnet_server_client_connected( world->server, index ) != 0 );
}

static void db_complete_client( db_world_t * world )
{
    rocketnet_client_disconnect( world->client );
    rn_test_client_wait_disconnected( world->client );
    check( rocketnet_client_disconnected( world->client ) );
}

static void db_complete_server( db_world_t * world )
{
    rocketnet_server_stop( world->server );
    rn_test_server_wait_stopped( world->server );
    check( rocketnet_server_stopped( world->server ) );
}

// --- cases -----------------------------------------------------------------

// DONE-WHEN (1): a connected client and a running server are destroyed without
// the completion poll, and every allocation the pair made returns to the count
// before create.

static void test_destroy_before_completion_frees_everything()
{
    rocketnet_quiet( 1 );
    rocketnet_assert_function( db_assert_recorder );
    db_assert_count = 0;

    db_allocator_t state = { 0 };
    int status = -1;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init_ex( &state, db_allocator_malloc, db_allocator_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    const int before = state.live;

    db_world_t world;
    db_world_create( &world, &state );
    db_connect( &world, 0xD5001ULL );

    // the release path: no completion poll before either destroy

    rocketnet_client_destroy( world.client );
    rocketnet_server_destroy( world.server );

    check( state.live == before );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_destroy_before_completion_frees_everything );

// DONE-WHEN (2): one error line and one misuse count per early destroy, naming
// the call and the completion function that was skipped; neither moves for a
// destroy after completion.

static void test_destroy_before_completion_is_logged()
{
    rocketnet_assert_function( db_assert_recorder );
    rocketnet_quiet( false );
    rocketnet_log_function( db_log_capture, NULL );
    db_assert_count = 0;

    db_world_t early;
    db_world_create( &early, NULL );
    db_connect( &early, 0xD6001ULL );

    db_error_lines = 0;

    uint64_t misuse = rocketnet_api_misuse_count();
    rocketnet_client_destroy( early.client );
    check( db_error_lines == 1 );
    check( strstr( db_last_error, "rocketnet_client_destroy" ) != NULL );
    check( strstr( db_last_error, DB_DISCONNECTED ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );

    rocketnet_server_destroy( early.server );
    check( db_error_lines == 2 );
    check( strstr( db_last_error, "rocketnet_server_destroy" ) != NULL );
    check( strstr( db_last_error, DB_STOPPED ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 2 );

    db_world_t complete;
    db_world_create( &complete, NULL );
    db_connect( &complete, 0xD6002ULL );

    db_error_lines = 0;
    misuse = rocketnet_api_misuse_count();
    db_complete_client( &complete );
    rocketnet_client_destroy( complete.client );
    db_complete_server( &complete );
    rocketnet_server_destroy( complete.server );
    check( db_error_lines == 0 );
    check( rocketnet_api_misuse_count() == misuse );

    rocketnet_log_function( NULL, NULL );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_destroy_before_completion_is_logged );

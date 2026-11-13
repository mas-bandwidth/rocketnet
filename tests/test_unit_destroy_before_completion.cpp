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
// clients: no socket and no thread; the endpoints' time is an argument
// starting at 1000.0. An early destroy is the one reader of a clock here: it
// reads rocketnet_platform_time once for its bounded wait's deadline
// (sdk/rocketnet_api.cpp) and returns on its first poll, because the loopback
// stop is always complete; a poll that did not complete would wait on that
// clock and then log the "kept" error the cases count. On Linux development
// builds the suite links that clock to a synthetic tick
// (tests/test_unit_endpoint.cpp, -Wl,--wrap); elsewhere it is the platform's
// monotonic clock. Each early destroy also fires its one assert
// (ROCKETNET_ASSERTS builds), counted by the recorder below; a destroy after
// completion fires none. This file covers the loopback/step-driven path. The
// threaded path, where an early destroy normally keeps the object and a
// second destroy after completion frees it, is covered by
// tests/functional/destroy_early_keep.cpp.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "token_string.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_fill_phase.h"

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

// a build with asserts compiled out (ROCKETNET_ASSERTS 0) takes the same
// release path with no assert; a build with them fires one per early destroy
#if ROCKETNET_ASSERTS
#define DB_ASSERTS_PER_EARLY_DESTROY 1
#else // #if ROCKETNET_ASSERTS
#define DB_ASSERTS_PER_EARLY_DESTROY 0
#endif // #if ROCKETNET_ASSERTS

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
    {
        return;
    }
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
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init_internal( &state, db_allocator_malloc, db_allocator_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    const int before = state.live;

    db_world_t world;
    db_world_create( &world, &state );
    db_connect( &world, 0xD5001ULL );

    // the release path: no completion poll before either destroy

    check( db_assert_count == 0 );
    rocketnet_client_destroy( world.client );
    check( db_assert_count == DB_ASSERTS_PER_EARLY_DESTROY );
    rocketnet_server_destroy( world.server );
    check( db_assert_count == 2 * DB_ASSERTS_PER_EARLY_DESTROY );

    check( state.live == before );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
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
    check( db_assert_count == 0 );
    rocketnet_client_destroy( early.client );
    check( db_error_lines == 1 );
    check( strstr( db_last_error, "rocketnet_client_destroy" ) != NULL );
    check( strstr( db_last_error, DB_DISCONNECTED ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( db_assert_count == DB_ASSERTS_PER_EARLY_DESTROY );

    rocketnet_server_destroy( early.server );
    check( db_error_lines == 2 );
    check( strstr( db_last_error, "rocketnet_server_destroy" ) != NULL );
    check( strstr( db_last_error, DB_STOPPED ) != NULL );
    check( rocketnet_api_misuse_count() == misuse + 2 );
    check( db_assert_count == 2 * DB_ASSERTS_PER_EARLY_DESTROY );

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
    check( db_assert_count == 2 * DB_ASSERTS_PER_EARLY_DESTROY );

    rocketnet_log_function( NULL, NULL );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_destroy_before_completion_is_logged );

// DONE-WHEN (3): an early destroy whose first stop is refused (a fill call is
// inside the check) asks for the stop again inside its bounded wait: the fill
// call ends at the refusal, the next stop takes, and every allocation returns.
// The fill call is ended from the log line of the refusal, on this thread, so
// no second thread is needed; the wait between the refusal and the retry is
// destroy's own (a tenth of a second on its clock), as in every early destroy.

static struct rocketnet_fill_phase_t * db_held_fill = NULL;
static int db_refusals = 0;

static void db_end_fill_at_refusal( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    db_log_capture( context, level, message, length );
    if ( level == ROCKETNET_LOG_LEVEL_ERROR && strstr( message, "a fill call is running" ) != NULL && db_held_fill )
    {
        db_refusals++;
        rocketnet_fill_phase_fill_end( db_held_fill, 0 );
        db_held_fill = NULL;
    }
}

static void test_destroy_before_completion_retries_a_refused_stop()
{
    rocketnet_quiet( false );
    rocketnet_assert_function( db_assert_recorder );
    db_assert_count = 0;

    db_allocator_t state = { 0 };
    int status = -1;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init_internal( &state, db_allocator_malloc, db_allocator_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    const int before = state.live;

    db_world_t world;
    db_world_create( &world, &state );
    db_connect( &world, 0xD7001ULL );
    db_complete_client( &world );
    rocketnet_client_destroy( world.client );

    struct rocketnet_fill_phase_t * fill_phase = rocketnet_server_transport_fill_phase(
        rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( fill_phase != NULL );
    rocketnet_fill_phase_arm( fill_phase, true );
    check( rocketnet_fill_phase_fill_begin( fill_phase, 0 ) );
    db_held_fill = fill_phase;
    db_refusals = 0;
    db_error_lines = 0;
    db_last_error[0] = '\0';

    rocketnet_log_function( db_end_fill_at_refusal, NULL );
    rocketnet_server_destroy( world.server );
    rocketnet_log_function( NULL, NULL );

    check( db_refusals == 1 );
    check( db_held_fill == NULL );
    check( db_assert_count == 2 * DB_ASSERTS_PER_EARLY_DESTROY );   // the early destroy's, and the refused stop's (sdk/rocketnet_fill_phase.cpp)
    check( strstr( db_last_error, "is kept" ) == NULL );
    check( state.live == before );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_destroy_before_completion_retries_a_refused_stop );

// rocketnet_server_set_send_kbps is a main-thread operation for every slot:
// made while a fill call runs it is refused and counted
// (MAIN_THREAD_CALL_OVERLAP) on a slot with no client as on a connected one,
// before the slot's state is read. This file's loopback world is the fixture.

static void test_server_set_send_kbps_checked_on_an_empty_slot()
{
    rocketnet_quiet( 1 );
    rocketnet_assert_function( db_assert_recorder );
    db_assert_count = 0;

    db_world_t world;
    db_world_create( &world, NULL );
    db_connect( &world, 0xD8001ULL );
    const int connected = rocketnet_client_index( world.client );
    const int empty = connected == 0 ? 1 : 0;
    check( !rocketnet_server_client_connected( world.server, empty ) );

    struct rocketnet_fill_phase_t * fill_phase = rocketnet_server_transport_fill_phase(
        rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( fill_phase != NULL && fill_phase->overlap_counter != NULL );
    rocketnet_fill_phase_arm( fill_phase, true );
    const uint64_t overlap = *fill_phase->overlap_counter;

    // outside any fill call both slots answer OK and nothing is counted
    check( rocketnet_server_set_send_kbps( world.server, empty, 100 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( world.server, connected, 100 ) == ROCKETNET_STATUS_OK );
    check( *fill_phase->overlap_counter == overlap );

    check( rocketnet_fill_phase_fill_begin( fill_phase, connected ) );
    const enum rocketnet_status_t on_empty = rocketnet_server_set_send_kbps( world.server, empty, 200 );
    const enum rocketnet_status_t on_connected = rocketnet_server_set_send_kbps( world.server, connected, 200 );
    rocketnet_fill_phase_fill_end( fill_phase, connected );

    check( on_empty == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( on_connected == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( *fill_phase->overlap_counter == overlap + 2 );

    db_complete_client( &world );
    rocketnet_client_destroy( world.client );
    db_complete_server( &world );
    rocketnet_server_destroy( world.server );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_server_set_send_kbps_checked_on_an_empty_slot );

// The header's lifetime of an open message (rocketnet_server_start_message):
// a disconnect of its client aborts it at once, and the finish that would
// have followed is refused (0), as is a new start on the emptied slot.

static void test_server_disconnect_aborts_an_open_message()
{
    rocketnet_quiet( 1 );
    rocketnet_assert_function( db_assert_recorder );
    db_assert_count = 0;

    db_world_t world;
    db_world_create( &world, NULL );
    db_connect( &world, 0xD9001ULL );
    const int index = rocketnet_client_index( world.client );

    uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE, 8 );
    check( p != NULL );
    memset( p, 0x5A, 8 );

    rocketnet_server_disconnect_client( world.server, index );
    check( !rocketnet_server_client_connected( world.server, index ) );
    check( rocketnet_server_finish_client_message( world.server, index, 8 ) == 0 );

    // a new start on the slot is refused too: no client is connected there
    check( rocketnet_server_start_message( world.server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE, 8 ) == NULL );

    db_complete_client( &world );
    rocketnet_client_destroy( world.client );
    db_complete_server( &world );
    rocketnet_server_destroy( world.server );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_server_disconnect_aborts_an_open_message );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Status codes and client states that no other unit case reaches, each
// reached through the public API with no socket, no real time and no real
// thread, mutex or timer failure:
//   * ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT and
//     ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT: a step-driven
//     loopback client on a test clock starting at 1000.0 reaches handshake
//     state 2 or 3, the server stops answering, and the client's next update
//     past the token's timeout ends the attempt in -(state + 1)
//     (sdk/rocketnet_client_protocol.cpp:213-218);
//   * ROCKETNET_STATUS_THREAD_CREATE_FAILED: a threaded client's connect under
//     a fault-injecting allocator failing at each allocation in turn reaches
//     the network thread's block, whose refusal makes
//     rocketnet_platform_thread_create return NULL before any thread starts
//     (sdk/rocketnet_platform.cpp:1341-1344), and connect returns the status
//     (sdk/rocketnet_client_transport.cpp:1037-1043).
//
// Not reachable in the unit tier: MUTEX_CREATE_FAILED (pthread_mutex_init on
// an embedded handle, sdk/rocketnet_platform.cpp:1453-1474), PLATFORM_INIT_FAILED
// (the POSIX rocketnet_platform_init returns true, sdk/rocketnet_platform.cpp:385-393)
// and TIMER_CREATE_FAILED (the Windows CreateWaitableTimerExW refusal,
// sdk/rocketnet_frame_sync.cpp:327-341). The platform call table
// (rocketnet_platform_priority_calls_t) carries the priority calls only, so
// none of the three has a fault hook.

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define REACHED_PROTOCOL 0x51E7D41A92B3C6D9ULL
#define REACHED_TIMEOUT_SECONDS 5

struct reached_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
};

static void reached_identity( reached_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5A ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_t * reached_server_create( void * context, const reached_identity_t * identity )
{
    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = REACHED_PROTOCOL;
    config.max_clients = 1;
    memcpy( config.backend_public_key, identity->backend_public, sizeof( config.backend_public_key ) );
    memcpy( config.server_public_key, identity->server_public, sizeof( config.server_public_key ) );
    memcpy( config.server_secret_key, identity->server_secret, sizeof( config.server_secret_key ) );
    config.transport = "loopback";
    rocketnet_server_t * server = rocketnet_server_create( context, "127.0.0.1:0", &config, NULL, 0 );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    return server;
}

static void reached_token( rocketnet_server_t * server, const reached_identity_t * identity, uint8_t * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
        REACHED_PROTOCOL, addresses, 1, 0x5EED, 1000, 1ULL << 40, REACHED_TIMEOUT_SECONDS, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
}

static double reached_now = 1000.0;
static double reached_clock( void * ) { return reached_now; }

// a step-driven loopback client and server pumped on the test clock until the
// client reaches handshake_state; then only the client is updated, so the
// server never answers again, until the client leaves that state
static enum rocketnet_client_state_t reached_handshake_stall( enum rocketnet_client_state_t handshake_state, double * stalled_at, double * ended_at )
{
    rocketnet_set_quiet( 1 );
    reached_identity_t identity;
    reached_identity( &identity );
    rocketnet_server_t * server = reached_server_create( NULL, &identity );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    reached_token( server, &identity, token );

    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.step_driven = true;
    config.time_callback = reached_clock;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL, 0 );
    check( client != NULL );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    double time = 1000.0;
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != handshake_state; i++ )
    {
        reached_now = time;
        check( rocketnet_client_update( client, time ) == ROCKETNET_UPDATE_OK );
        rocketnet_client_send_packets( client );
        check( rocketnet_server_update( server, time ) == ROCKETNET_UPDATE_OK );
        rocketnet_server_send_packets( server );
        time += 0.01;
    }
    check( rocketnet_client_state( client ) == handshake_state );
    *stalled_at = time;

    // the server is no longer updated: the client hears nothing more
    for ( int i = 0; i < 100 * ( REACHED_TIMEOUT_SECONDS + 2 ) && rocketnet_client_state( client ) == handshake_state; i++ )
    {
        time += 0.01;
        reached_now = time;
        check( rocketnet_client_update( client, time ) == ROCKETNET_UPDATE_OK );
        rocketnet_client_send_packets( client );
    }
    *ended_at = time;

    const enum rocketnet_client_state_t state = rocketnet_client_state( client );
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    return state;
}

static void client_connection_request_timed_out_state_reached()
{
    double stalled_at = 0.0, ended_at = 0.0;
    const enum rocketnet_client_state_t state =
        reached_handshake_stall( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST, &stalled_at, &ended_at );
    check( state == ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT );
    check( ended_at - stalled_at < REACHED_TIMEOUT_SECONDS + 1.0 );
}

static void client_connection_response_timed_out_state_reached()
{
    double stalled_at = 0.0, ended_at = 0.0;
    const enum rocketnet_client_state_t state =
        reached_handshake_stall( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE, &stalled_at, &ended_at );
    check( state == ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT );
    check( ended_at - stalled_at < REACHED_TIMEOUT_SECONDS + 1.0 );
}

REGISTER_UNIT_TEST( client_connection_request_timed_out_state_reached );
REGISTER_UNIT_TEST( client_connection_response_timed_out_state_reached );

// the fault-injecting allocator: fails exactly the fail_at-th call
struct reached_allocator_t
{
    int live;
    int calls;
    int fail_at;
};

static void * reached_allocate( void * context, size_t bytes )
{
    reached_allocator_t * state = (reached_allocator_t*) context;
    const int call = ++state->calls;
    if ( state->fail_at == call )
        return NULL;
    void * p = malloc( bytes );
    if ( p )
        ++state->live;
    return p;
}

static void reached_release( void * context, void * pointer )
{
    reached_allocator_t * state = (reached_allocator_t*) context;
    if ( !pointer )
        return;
    --state->live;
    free( pointer );
}

static void client_connect_thread_create_failed_reached()
{
    reached_allocator_t state = { 0, 0, 0 };

    // the worker starts the process initialized: re-initialize with the
    // fault-injecting allocator so every allocation below goes through it
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_set_allocator( reached_allocate, reached_release ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( &state ) == ROCKETNET_STATUS_OK );
    rocketnet_set_quiet( 1 );

    reached_identity_t identity;
    reached_identity( &identity );
    rocketnet_server_t * server = reached_server_create( &state, &identity );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    reached_token( server, &identity, token );

    // a threaded client: its connect creates the network thread last
    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.step_driven = false;
    rocketnet_client_t * client = rocketnet_client_create( &state, &config, NULL, 0 );
    check( client != NULL );

    // fail each allocation of the connect in turn; the thread's own block is
    // the last before the thread starts, so THREAD_CREATE_FAILED comes before
    // any connect could succeed, and no thread is ever started
    int out_of_memory = 0;
    int thread_create_failed = 0;
    for ( int k = 1; k <= 256 && !thread_create_failed; k++ )
    {
        state.fail_at = state.calls + k;
        const enum rocketnet_status_t status = rocketnet_client_connect( client, token );
        check( status != ROCKETNET_STATUS_OK );
        if ( status == ROCKETNET_STATUS_THREAD_CREATE_FAILED )
            thread_create_failed = 1;
        else
        {
            check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
            out_of_memory++;
        }
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    }
    state.fail_at = 0;
    check( out_of_memory > 0 );
    check( thread_create_failed );

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );

    // a clean term, then restore the default allocator for the worker's term
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_set_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( client_connect_thread_create_failed_reached );

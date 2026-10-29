/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Status codes and client states that no other unit case reaches, each
// reached through the public API with no socket, no real time and no real
// thread, mutex or timer failure:
//   * ROCKETNET_STATUS_PLATFORM_INIT_FAILED: a refused platform_start fails
//     rocketnet_init_ex;
//   * ROCKETNET_STATUS_TIMER_CREATE_FAILED: a refused waitable timer create
//     fails rocketnet_frame_sync_create_ex;
//   * ROCKETNET_STATUS_MUTEX_CREATE_FAILED: a refused mutex create fails
//     rocketnet_client_connect;
//   * ROCKETNET_STATUS_THREAD_CREATE_FAILED: a threaded client connect under a
//     fault-injecting allocator fails when allocating the worker thread's
//     record, before the OS thread create; the sweep stops at that first
//     refusal, so no attempt reaches the OS create and no thread starts;
//   * ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT and
//     ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT: a step-driven
//     loopback client on a test clock starting at 1000.0 reaches handshake
//     state 2 or 3, the server stops answering, and the client's next update
//     past the token's timeout ends the attempt in -(state + 1).

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"

#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_frame_sync.h"
#include "rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check
#define REACHED_PROTOCOL 0x51E7D41A92B3C6D9ULL
#define REACHED_TIMEOUT_SECONDS 5

struct reached_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static void reached_identity( reached_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }

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
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( context, "127.0.0.1:0", NULL, &config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    return server;
}

static void reached_token( rocketnet_server_t * server, const reached_identity_t * identity, char * token_string )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, server_public,
                                                     REACHED_PROTOCOL, addresses, 1, 0x5EED, 1000, 1ULL << 40, REACHED_TIMEOUT_SECONDS, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

#if ROCKETNET_DEV
static int refuse_platform_start( void )
{
    return 1;
}

static int refuse_waitable_timer_calls;
static unsigned refuse_waitable_timer_flags[4];

static void * refuse_waitable_timer( unsigned flags )
{
    if ( refuse_waitable_timer_calls < 4 )
    {
        refuse_waitable_timer_flags[refuse_waitable_timer_calls] = flags;
    }

    ++refuse_waitable_timer_calls;
    if ( flags == 0x00000002u )
        return NULL;

    return (void *) 1;
}

static int refuse_mutex( rocketnet_platform_mutex_t * mutex )
{
    (void) mutex;
    return 1;
}

// a refused platform start fails rocketnet_init with PLATFORM_INIT_FAILED
static void test_platform_init_failed_reached()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    const struct rocketnet_platform_priority_calls_t * saved = rocketnet_platform_priority_calls_current();
    struct rocketnet_platform_priority_calls_t fake;
    fake = *saved;
    fake.platform_start = refuse_platform_start;
    rocketnet_platform_set_priority_calls_for_testing( &fake );

    int status = -1;
    check( !rocketnet_init_ex( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_PLATFORM_INIT_FAILED );

    rocketnet_platform_set_priority_calls_for_testing( saved );

    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_platform_init_failed_reached );

// a refused high-resolution timer create fails frame sync with TIMER_CREATE_FAILED after one request
static void test_timer_create_failed_reached()
{
    refuse_waitable_timer_calls = 0;
    memset( refuse_waitable_timer_flags, 0, sizeof( refuse_waitable_timer_flags ) );

    const struct rocketnet_platform_priority_calls_t * saved = rocketnet_platform_priority_calls_current();
    struct rocketnet_platform_priority_calls_t fake;
    fake = *saved;
    fake.create_waitable_timer = refuse_waitable_timer;
    rocketnet_platform_set_priority_calls_for_testing( &fake );

    int status = -1;
    check( rocketnet_frame_sync_create_ex( NULL, 60.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_TIMER_CREATE_FAILED );
    check( refuse_waitable_timer_calls == 1 );
    check( refuse_waitable_timer_flags[0] == 0x00000002u );

    check( rocketnet_frame_sync_create( NULL, 60.0 ) == NULL );
    check( refuse_waitable_timer_calls == 2 );
    check( refuse_waitable_timer_flags[1] == 0x00000002u );

    rocketnet_platform_set_priority_calls_for_testing( saved );
}

REGISTER_UNIT_TEST( test_timer_create_failed_reached );

// a refused mutex create fails client connect with MUTEX_CREATE_FAILED
static void test_client_mutex_create_failed_reached()
{
    rocketnet_quiet( 1 );

    reached_identity_t identity;
    reached_identity( &identity );
    rocketnet_server_t * server = reached_server_create( NULL, &identity );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    reached_token( server, &identity, token );

    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = true;

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    const struct rocketnet_platform_priority_calls_t * saved = rocketnet_platform_priority_calls_current();
    struct rocketnet_platform_priority_calls_t fake;
    fake = *saved;
    fake.mutex_init = refuse_mutex;
    rocketnet_platform_set_priority_calls_for_testing( &fake );

    const enum rocketnet_status_t status = rocketnet_client_connect( client, token );
    check( status == ROCKETNET_STATUS_MUTEX_CREATE_FAILED );

    rocketnet_platform_set_priority_calls_for_testing( saved );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_client_mutex_create_failed_reached );
#endif // #if ROCKETNET_DEV

// the fault-injecting allocator: fails exactly the fail_at-th call
struct reached_allocator_t
{
    int live;
    int calls;
    int fail_at;
};

static void * reached_allocate( void * context, size_t bytes )
{
    reached_allocator_t * state = (reached_allocator_t *) context;
    const int call = ++state->calls;
    if ( state->fail_at == call )
        return NULL;

    void * p = malloc( bytes );
    if ( p )
    {
        ++state->live;
    }

    return p;
}

static void reached_release( void * context, void * pointer )
{
    reached_allocator_t * state = (reached_allocator_t *) context;
    if ( !pointer )
        return;
    --state->live;
    free( pointer );
}

static void test_client_connect_thread_create_failed_reached()
{
    reached_allocator_t state = { 0, 0, 0 };

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( &state, reached_allocate, reached_release ) == ROCKETNET_STATUS_OK );
    rocketnet_test_loopback_hub_context( &state );

    rocketnet_quiet( 1 );

    reached_identity_t identity;
    reached_identity( &identity );
    rocketnet_server_t * server = reached_server_create( &state, &identity );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    reached_token( server, &identity, token );

    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = false;

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( &state, &config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    int out_of_memory = 0;
    int thread_create_failed = 0;
    enum rocketnet_status_t thread_create_status = ROCKETNET_STATUS_OK;
    for ( int k = 1; k <= 256 && !thread_create_failed; k++ )
    {
        state.fail_at = state.calls + k;

        const enum rocketnet_status_t status = rocketnet_client_connect( client, token );
        check( status != ROCKETNET_STATUS_OK );
        if ( status == ROCKETNET_STATUS_THREAD_CREATE_FAILED )
        {
            thread_create_failed = 1;
            thread_create_status = status;
        }
        else
        {
            check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );

            out_of_memory++;
        }

        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    }

    state.fail_at = 0;

    check( out_of_memory > 0 );
    check( thread_create_failed == 1 );
    check( thread_create_status == ROCKETNET_STATUS_THREAD_CREATE_FAILED );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_client_connect_thread_create_failed_reached );

#if ROCKETNET_DEV
static double reached_now = 1000.0;
static double reached_clock( void * ) { return reached_now; }

static enum rocketnet_client_state_t reached_handshake_stall( enum rocketnet_client_state_t handshake_state, double * stalled_at, double * ended_at )
{
    rocketnet_quiet( 1 );

    reached_identity_t identity;
    reached_identity( &identity );
    rocketnet_server_t * server = reached_server_create( NULL, &identity );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    reached_token( server, &identity, token );

    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = true;
    config.time_callback = reached_clock;

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    double time = 1000.0;
    int seen_requesting_cookie = 0;
    for ( int i = 0; i < 50 && rocketnet_client_state( client ) < handshake_state; i++ )
    {
        reached_now = time;

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE )
        {
            seen_requesting_cookie = 1;
        }

        rocketnet_client_update( client, time );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, time );
        rocketnet_server_send_packets( server );
        time += 0.01;
    }

    check( seen_requesting_cookie == 1 );

    if ( handshake_state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST )
    {
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST );
    }
    else if ( handshake_state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE );
    }

    check( rocketnet_client_state( client ) == handshake_state );

    *stalled_at = time;

    for ( int i = 0; i < 100 * ( REACHED_TIMEOUT_SECONDS + 2 ) && rocketnet_client_state( client ) == handshake_state; i++ )
    {
        time += 0.01;
        reached_now = time;
        rocketnet_client_update( client, time );
        rocketnet_client_send_packets( client );
    }

    *ended_at = time;

    const enum rocketnet_client_state_t state = rocketnet_client_state( client );
    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );

    return state;
}

static void test_client_connection_request_timed_out_state_reached()
{
    double stalled_at = 0.0, ended_at = 0.0;
    const enum rocketnet_client_state_t state =
        reached_handshake_stall( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST, &stalled_at, &ended_at );
    check( state == ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT );
    check( ended_at - stalled_at < REACHED_TIMEOUT_SECONDS + 1.0 );
}

REGISTER_UNIT_TEST( test_client_connection_request_timed_out_state_reached );

static void test_client_connection_response_timed_out_state_reached()
{
    double stalled_at = 0.0, ended_at = 0.0;
    const enum rocketnet_client_state_t state =
        reached_handshake_stall( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE, &stalled_at, &ended_at );
    check( state == ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT );
    check( ended_at - stalled_at < REACHED_TIMEOUT_SECONDS + 1.0 );
}

REGISTER_UNIT_TEST( test_client_connection_response_timed_out_state_reached );
#endif // #if ROCKETNET_DEV

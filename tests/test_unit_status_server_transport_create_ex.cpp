/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The structured server transport create surface
// (docs/ERRORS-AND-LOGGING.md 1.3, 2.1 and 2.3, Layer 4): every failure exit
// that is reachable without a socket or real time writes its exact status code,
// and the allocation-failure exits (S10, S14) are reached by a fault-injecting
// allocator failing at each allocation in turn.
//
// A non-port-zero backend-open refusal still needs a live socket. This unit
// test reaches TRANSPORT_LADDER_EXHAUSTED through the preallocation memory
// budget instead; the port-zero refusal (S12) also occurs before open.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "portable_crt.h"

#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the cases test struct transport lists, so the test sets the caller's env pin aside
// for its duration and puts it back after: the env pin wins over the struct
// by design, so a CI leg pinned by ROCKETNET_SERVER_TRANSPORT would otherwise
// never reach the struct's list

#if !ROCKETNET_DEV
struct env_pin_saved
{
    bool was_set;
    char value[256];
};

static void env_pin_set_aside( env_pin_saved * saved )
{
    saved->was_set = rocketnet_test_env_getenv( "ROCKETNET_SERVER_TRANSPORT", saved->value, sizeof(saved->value) ) != 0;
    rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
}

static void env_pin_put_back( const env_pin_saved * saved )
{
    if ( saved->was_set )
        rocketnet_test_env_setenv( "ROCKETNET_SERVER_TRANSPORT", saved->value, 1 );
}

#define check rocketnet_unit_check

struct allocator_state
{
    int live;
    int calls;
    int fail_at;
};

static void * allocate( void * context, size_t bytes )
{
    allocator_state * state = (allocator_state*) context;
    const int call = ++state->calls;
    if ( state->fail_at == call )
        return NULL;
    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;
    uintptr_t address = ( ( (uintptr_t) base + sizeof(void*) + 63 ) & ~(uintptr_t)63 ) + 16;
    memcpy( (void*) ( address - sizeof(void*) ), &base, sizeof(base) );
    memset( (void*) address, 0xA5, bytes );
    ++state->live;
    return (void*) address;
}

static void release( void * context, void * pointer )
{
    allocator_state * state = (allocator_state*) context;
    void * base = NULL;
    memcpy( &base, (uint8_t*) pointer - sizeof(void*), sizeof(base) );
    --state->live;
    free( base );
}

// production-like keys: non-zero and different from the published development
// keys, so the dev-keys refusal (S11) does not fire and S7/S12/S14 are reached
static void production_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config( config );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
}

// A named production server rung with a concrete memory plan. The budget=1
// case refuses before opening it, so this unit test needs no native backend.
static const char * production_budget_transport()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    return "gso";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    return "msgx";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return "iocp";
#else
#error "server budget status test needs a production server transport"
#endif
}

#if ROCKETNET_ASSERTS
// the NULL argument below is the documented counted no-op, which fires
// rocketnet_assert in a build with asserts on: count it instead of breaking
static int server_create_ex_expected_null_asserts = 0;

static void server_create_ex_expected_null_assert( const char *, const char *, const char *, int )
{
    server_create_ex_expected_null_asserts++;
}
#endif

static void test_server_transport_create_ex_reports_every_status()
{
    env_pin_saved pin;
    env_pin_set_aside( &pin );

    allocator_state state = { 0, 0, 0 };
    int status = -1;

    // the worker starts the process initialized; return to the pre-init state
    // so the lifecycle refusals below are reachable
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    // S1: a NULL public_address_string is a logged INVALID_ARGUMENT, even
    // before init (the public NULL guard runs first)
    status = -1;
#if ROCKETNET_ASSERTS
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    server_create_ex_expected_null_asserts = 0;
    rocketnet_assert_function( server_create_ex_expected_null_assert );
#endif
    check( rocketnet_server_transport_create_ex( NULL, NULL, NULL, 1000.0, &status ) == NULL );
#if ROCKETNET_ASSERTS
    rocketnet_assert_function( saved_assert );
    check( server_create_ex_expected_null_asserts == 1 );
#endif
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // install the fault-injecting allocator while uninitialized, so every
    // allocation below goes through allocate and release
    check( rocketnet_allocator_ex( allocate, release ) == ROCKETNET_STATUS_OK );

    // not initialized: a non-NULL create before rocketnet_init is refused
    status = -1;
    check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:12345", NULL, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );

    // init with the fault-injecting allocator's state as the context
    status = -1;
    check( rocketnet_init_ex( &state, allocate, release, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    // S2: an invalid environment value refuses apply_env
    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MAX_CLIENTS", "not-a-number", 1 ) == 0 );
    status = -1;
    check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", NULL, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_MAX_CLIENTS" ) == 0 );

    // S3: an out-of-range config value refuses validation
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_default_config( &config );
        config.max_clients = 50000;
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    }

    // S5: the reassembly cross-field allocation exceeds the operator memory budget
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_default_config( &config );
        config.max_clients = ROCKETNET_MAX_CLIENTS;
        config.max_fragmented_messages = 256;    // ROCKETNET_MAX_FRAGMENTS
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED );
    }

    // the env memory_budget parse refusal (the operator budget env's own check)
    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "not-a-number", 1 ) == 0 );
    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    }
    check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" ) == 0 );

    // the operator budget refusal: every candidate's memory plan exceeds it
    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "1", 1 ) == 0 );
    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        // The budget refusal is independent of the backend. This named
        // production rung has a plan and refuses before any socket opens.
        snprintf( config.transport, sizeof( config.transport ), "%s", production_budget_transport() );
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
    }
    check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" ) == 0 );

    // A production portable pin reaches its own memory plan. A one-byte
    // budget refuses before any socket opens, preserving unit purity.
    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "1", 1 ) == 0 );
    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
    }
    check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" ) == 0 );

    // S6: a keyless config (the default shape) is refused
    status = -1;
    check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", NULL, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_MISSING_KEYS );

    // S7: protocol_id 0 with production keys is refused
    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        config.protocol_id = 0;
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_INVALID_PROTOCOL_ID );
    }

    // S8: a transport-list typo refuses at parse
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.transport, sizeof( config.transport ), "%s", "bogus" );
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );
    }

    // S9: an unparsable public address is refused
    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "not-an-address", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
    }

    // S11: development keys in a production build (ROCKETNET_DEV=0) are refused
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
    }

    // S12: a port-zero address refused by a non-discovering transport (XDP) is
    // TRANSPORT_PORT_ZERO_UNSUPPORTED. The xdp row exists only on Linux:
    // elsewhere the ladder skips "xdp" for the platform and falls to the
    // auto order (sdk/rocketnet_tests.cpp checks that fall), whose transports
    // open a real socket, so the step runs on Linux only
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        snprintf( config.transport, sizeof( config.transport ), "%s", "xdp" );
        status = -1;
        check( rocketnet_server_transport_create_ex( &state, "127.0.0.1:0", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED );
    }
#endif

    // S10 and S14: a fault-injecting allocator failing at each allocation in
    // turn yields OUT_OF_MEMORY, and once every allocation succeeds the create
    // completes with ROCKETNET_STATUS_OK
    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
        config.max_clients = 1;

        int reached_success = 0;
        int out_of_memory = 0;
        for ( int k = 1; k <= 64 && !reached_success; k++ )
        {
            state.fail_at = state.calls + k;
            status = -1;
            struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex(
                &state, "127.0.0.1:12345", &config, 1000.0, &status );
            if ( server )
            {
                check( status == ROCKETNET_STATUS_OK );
                rn_test_server_transport_stop_and_destroy( server );
                check( state.live == 0 );
                reached_success = 1;
            }
            else
            {
                check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
                check( state.live == 0 );
                out_of_memory++;
            }
        }
        check( out_of_memory > 0 );
        check( reached_success );
        state.fail_at = 0;
    }

    // a clean term, then restore the default allocator for the worker's term
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );

    env_pin_put_back( &pin );
}

REGISTER_UNIT_TEST( test_server_transport_create_ex_reports_every_status );
#endif // !ROCKETNET_DEV: a development build accepts what this checks is refused

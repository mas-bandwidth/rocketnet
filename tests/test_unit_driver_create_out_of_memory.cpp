/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A transport create that fails for want of memory reports OUT_OF_MEMORY
// (rocketnet_server_transport_io_config_t.status, sdk/rocketnet_driver.h), and the
// transport ladder ends there instead of falling to the next candidate. A
// fault-injecting allocator fails each allocation in turn:
//
//   * test_transport_open_reports_out_of_memory: a transport open over an in-memory
//     transport with three allocations, server and client, gives OUT_OF_MEMORY
//     at each with nothing live, and a create that refuses without a failed
//     allocation leaves the status untouched;
//   * test_transport_create_server_ladder_stops_on_out_of_memory: the loopback
//     transport under rocketnet_server_transport_create_ex, pinned and as the
//     first of two candidates;
//   * test_transport_create_client_ladder_stops_on_out_of_memory: the same for
//     rocketnet_client_transport_create_ex;
//   * test_platform_init_survives_a_refusing_allocator: rocketnet_init_ex under an
//     allocator that refuses its first TRANSPORT_OOM_GIVE_WAY requests completes,
//     and platform init asks the allocator for nothing.
//
// The socket transports (portable, msgx, gso, iocp) and the kernel-object
// transports (xdp, io_uring, rio) open a socket or a kernel object in their
// create, which the unit tier never does; their allocations go through
// rocketnet_malloc, or for the xdp umem and the rio region are counted by
// rocketnet_allocation_refused (sdk/rocketnet.cpp:378), and the open reads
// that count (sdk/rocketnet_driver.cpp:126), the same path these cases walk.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#if ROCKETNET_DEV
#include "test_env_source.h"
#endif // #if ROCKETNET_DEV
#include "portable_crt.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_config.h"
#include "loopback_hub.h"
#include "rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// the cases set struct transport lists, so a CI leg's env pin is set aside
// for their duration and put back after: the env pin wins over the struct

#if ROCKETNET_DEV
struct transport_oom_env_pin_t
{
    bool was_set;
    char value[256];
};
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static void transport_oom_env_set_aside( const char * name, transport_oom_env_pin_t * saved )
{
    saved->was_set = rocketnet_test_env_getenv( name, saved->value, sizeof( saved->value ) ) != 0;
    rocketnet_test_env_unsetenv( name );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static void transport_oom_env_put_back( const char * name, const transport_oom_env_pin_t * saved )
{
    if ( saved->was_set )
    {
        rocketnet_test_env_setenv( name, saved->value, 1 );
    }
}
#endif // #if ROCKETNET_DEV

struct transport_oom_allocator_t
{
    int live;
    int calls;
    int fail_at;
    int refuse_through;
};

static void * transport_oom_allocate( void * context, size_t bytes )
{
    transport_oom_allocator_t * state = (transport_oom_allocator_t *) context;
    const int call = ++state->calls;
    if ( state->fail_at == call || call <= state->refuse_through )
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

static void transport_oom_release( void * context, void * pointer )
{
    transport_oom_allocator_t * state = (transport_oom_allocator_t *) context;
    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    --state->live;
    free( base );
}

// the worker starts initialized: terminate, install the fault-injecting
// allocator, and initialize with its state as the context

static void transport_oom_begin( transport_oom_allocator_t * state )
{
    memset( state, 0, sizeof( *state ) );
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    int status = -1;
    check( rocketnet_init_ex( state, transport_oom_allocate, transport_oom_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_test_loopback_hub_context( state );
}

// a clean term, then the default allocator back for the worker's own term,
// as test_unit_status_server_transport_create_ex.cpp ends

static void transport_oom_end( transport_oom_allocator_t * state )
{
    state->fail_at = 0;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state->live == 0 );
    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

// ---- an in-memory transport with three allocations -----------------------------

enum
{
    TRANSPORT_OOM_BLOCKS = 3
};

struct transport_oom_instance_t
{
    void * context;
    void * blocks[TRANSPORT_OOM_BLOCKS];
};

static bool transport_oom_refuse = false;

static void * transport_oom_create( void * context )
{
    if ( transport_oom_refuse )
        return NULL;

    transport_oom_instance_t * instance = (transport_oom_instance_t *) rocketnet_malloc( context, sizeof( transport_oom_instance_t ) );
    if ( !instance )
        return NULL;
    memset( instance, 0, sizeof( *instance ) );
    instance->context = context;

    for ( int i = 1; i < TRANSPORT_OOM_BLOCKS; i++ )
    {
        instance->blocks[i] = rocketnet_malloc( context, 64 );
        if ( !instance->blocks[i] )
        {
            for ( int j = 1; j < i; j++ )
            {
                rocketnet_free( context, instance->blocks[j] );
            }
            rocketnet_free( context, instance );
            return NULL;
        }
    }

    return instance;
}

static void transport_oom_destroy( void * transport )
{
    transport_oom_instance_t * instance = (transport_oom_instance_t *) transport;
    for ( int i = 1; i < TRANSPORT_OOM_BLOCKS; i++ )
    {
        rocketnet_free( instance->context, instance->blocks[i] );
    }
    rocketnet_free( instance->context, instance );
}

static void * transport_oom_server_create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * )
{
    return transport_oom_create( context );
}
static void * transport_oom_client_create( void * context, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * )
{
    return transport_oom_create( context );
}
static void transport_oom_stop( void * ) {}
static bool transport_oom_stopped( void * ) { return true; }
static void transport_oom_send_batch( void *, const rocketnet_platform_send_t *, int ) {}
static int transport_oom_receive_batch( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) { return 0; }
static void transport_oom_send( void *, const rocketnet_address_t *, const uint8_t *, int ) {}
static int transport_oom_receive( void *, rocketnet_address_t *, uint8_t *, int ) { return 0; }

static void test_transport_open_reports_out_of_memory()
{
    transport_oom_allocator_t state;
    transport_oom_begin( &state );

    rocketnet_server_transport_io_operations_t server_operations = {};
    server_operations.create = transport_oom_server_create;
    server_operations.destroy = transport_oom_destroy;
    server_operations.stop = transport_oom_stop;
    server_operations.stopped = transport_oom_stopped;
    server_operations.send_batch = transport_oom_send_batch;
    server_operations.receive_batch = transport_oom_receive_batch;

    rocketnet_client_transport_io_operations_t client_operations = {};
    client_operations.create = transport_oom_client_create;
    client_operations.destroy = transport_oom_destroy;
    client_operations.send = transport_oom_send;
    client_operations.receive = transport_oom_receive;

    struct rocketnet_server_transport_config_t server_transport_config;
    rocketnet_server_transport_default_config( &server_transport_config );

    struct rocketnet_client_transport_config_t client_transport_config;
    rocketnet_client_transport_default_config( &client_transport_config );

    // each allocation of the create, failed in turn, is OUT_OF_MEMORY with
    // nothing live; with none failed the open succeeds

    for ( int k = 1; k <= TRANSPORT_OOM_BLOCKS + 1; k++ )
    {
        rocketnet_address_t bind = {};
        bind.type = ROCKETNET_ADDRESS_IPV4;

        int status = ROCKETNET_STATUS_OK;
        const rocketnet_server_transport_io_config_t server_config = { "oom", &server_transport_config, NULL, &status };
        rocketnet_server_transport_io_t server = {};
        state.fail_at = state.calls + k;

        const bool server_opened = rocketnet_server_transport_io_open( &server_operations, &state, &server_config, &bind, &server );
        check( server_opened == ( k > TRANSPORT_OOM_BLOCKS ) );
        check( status == ( k > TRANSPORT_OOM_BLOCKS ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_OUT_OF_MEMORY ) );
        rn_test_server_io_stop_and_close( &server );
        check( state.live == 0 );

        status = ROCKETNET_STATUS_OK;

        const rocketnet_client_transport_io_config_t client_config = { "oom", &client_transport_config, &status };
        rocketnet_client_transport_io_t client = {};
        state.fail_at = state.calls + k;

        const bool client_opened = rocketnet_client_transport_io_open( &client_operations, &state, &client_config, &bind, &client );
        check( client_opened == ( k > TRANSPORT_OOM_BLOCKS ) );
        check( status == ( k > TRANSPORT_OOM_BLOCKS ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_OUT_OF_MEMORY ) );
        rocketnet_client_transport_io_close( &client );
        check( state.live == 0 );
    }

    state.fail_at = 0;

    // a create that refuses with no failed allocation is a refusal: the
    // status stays OK, so the ladder falls to the next candidate
    {
        transport_oom_refuse = true;

        rocketnet_address_t bind = {};
        bind.type = ROCKETNET_ADDRESS_IPV4;

        int status = ROCKETNET_STATUS_OK;
        const rocketnet_server_transport_io_config_t server_config = { "oom", &server_transport_config, NULL, &status };
        rocketnet_server_transport_io_t server = {};
        check( !rocketnet_server_transport_io_open( &server_operations, &state, &server_config, &bind, &server ) );
        check( status == ROCKETNET_STATUS_OK );

        const rocketnet_client_transport_io_config_t client_config = { "oom", &client_transport_config, &status };
        rocketnet_client_transport_io_t client = {};
        check( !rocketnet_client_transport_io_open( &client_operations, &state, &client_config, &bind, &client ) );
        check( status == ROCKETNET_STATUS_OK );
        transport_oom_refuse = false;
    }

    // a config with no status pointer opens and fails as before
    {
        rocketnet_address_t bind = {};
        bind.type = ROCKETNET_ADDRESS_IPV4;

        const rocketnet_server_transport_io_config_t server_config = { "oom", &server_transport_config, NULL, NULL };
        rocketnet_server_transport_io_t server = {};
        state.fail_at = state.calls + 1;
        check( !rocketnet_server_transport_io_open( &server_operations, &state, &server_config, &bind, &server ) );
        state.fail_at = 0;
        check( state.live == 0 );
    }

    transport_oom_end( &state );
    printf( "test_transport_open_reports_out_of_memory\n" );
}

// ---- the server ladder ---------------------------------------------------------

#if ROCKETNET_DEV
static void transport_oom_production_server_config( struct rocketnet_server_transport_config_t * config, const char * transport )
{
    rocketnet_server_transport_default_config_current( config );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
    config->max_clients = 1;
    snprintf( config->transport, sizeof( config->transport ), "%s", transport );
    config->loopback_hub = rocketnet_test_loopback_hub();
}

// the allocations of a pinned loopback create, counted by failing each in
// turn until the create succeeds: every failure is OUT_OF_MEMORY with
// nothing live and no loopback endpoint bound

static int transport_oom_walk_server( transport_oom_allocator_t * state, const char * transport )
{
    struct rocketnet_server_transport_config_t config;
    transport_oom_production_server_config( &config, transport );

    // the hub the config names is live from here on; the walk counts the rest

    const int hub_live = state->live;

    for ( int k = 1; k <= 256; k++ )
    {
        state->fail_at = state->calls + k;

        int status = -1;
        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_ex( state, "127.0.0.1:0", &config, 1000.0, &status );
        if ( server )
        {
            check( status == ROCKETNET_STATUS_OK );
            rn_test_server_transport_stop_and_destroy( server );
            state->fail_at = 0;
            check( state->live == hub_live );
            check( rocketnet_loopback_endpoints_live( rocketnet_test_loopback_hub() ) == 0 );
            return k - 1;
        }

        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( state->live == hub_live );
        check( rocketnet_loopback_endpoints_live( rocketnet_test_loopback_hub() ) == 0 );
    }

    check( false );

    return 0;
}

static void test_transport_create_server_ladder_stops_on_out_of_memory()
{
#if ROCKETNET_DEV
    transport_oom_env_pin_t pin;
    transport_oom_env_set_aside( "ROCKETNET_SERVER_TRANSPORT", &pin );
#endif // #if ROCKETNET_DEV

    transport_oom_allocator_t state;
    transport_oom_begin( &state );

    // the pin: a transport allocation that fails ends the create with
    // OUT_OF_MEMORY, never TRANSPORT_LADDER_EXHAUSTED

    const int allocations = transport_oom_walk_server( &state, "loopback" );
    check( allocations > 0 );

    // two candidates: the same walk, the same count. A failed transport
    // allocation in the first candidate never falls to the second

    check( transport_oom_walk_server( &state, "loopback, loopback" ) == allocations );

    transport_oom_end( &state );
#if ROCKETNET_DEV
    transport_oom_env_put_back( "ROCKETNET_SERVER_TRANSPORT", &pin );
#endif // #if ROCKETNET_DEV
    printf( "test_transport_create_server_ladder_stops_on_out_of_memory\n" );
}

// ---- the client ladder ---------------------------------------------------------

static double transport_oom_clock = 1000.0;
static double transport_oom_time_callback( void * ) { return transport_oom_clock; }

static int transport_oom_walk_client( transport_oom_allocator_t * state, const uint8_t * token, const char * transport )
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", transport );
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = 1;
    config.time_callback = transport_oom_time_callback;

    // the hub the config names is live from here on; the walk counts the rest

    const int hub_live = state->live;

    for ( int k = 1; k <= 256; k++ )
    {
        state->fail_at = state->calls + k;

        int status = -1;
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_current_ex( state, token, &config, 1000.0, &status );
        if ( client )
        {
            check( status == ROCKETNET_STATUS_OK );
            rn_test_client_transport_stop_and_destroy( client );
            state->fail_at = 0;
            check( state->live == hub_live );
            check( rocketnet_loopback_endpoints_live( rocketnet_test_loopback_hub() ) == 0 );
            return k - 1;
        }

        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( state->live == hub_live );
        check( rocketnet_loopback_endpoints_live( rocketnet_test_loopback_hub() ) == 0 );
    }

    check( false );

    return 0;
}

static void test_transport_create_client_ladder_stops_on_out_of_memory()
{
#if ROCKETNET_DEV
    transport_oom_env_pin_t pin;
    transport_oom_env_set_aside( "ROCKETNET_CLIENT_TRANSPORT", &pin );
#endif // #if ROCKETNET_DEV

    transport_oom_allocator_t state;
    transport_oom_begin( &state );

    const uint64_t protocol_id = 0x6f6f6d6f6f6d6f6fULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { "127.0.0.1:50000" };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x9e3779b97f4a7c15ULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    check( state.live == 0 );

    const int allocations = transport_oom_walk_client( &state, token, "loopback" );
    check( allocations > 0 );
    check( transport_oom_walk_client( &state, token, "loopback, loopback" ) == allocations );

    transport_oom_end( &state );
#if ROCKETNET_DEV
    transport_oom_env_put_back( "ROCKETNET_CLIENT_TRANSPORT", &pin );
#endif // #if ROCKETNET_DEV
    printf( "test_transport_create_client_ladder_stops_on_out_of_memory\n" );
}
#endif // #if ROCKETNET_DEV

// a refusing allocator gives way after this many refusals, rather than hanging
// the suite
enum
{
    TRANSPORT_OOM_GIVE_WAY = 64
};

// platform init under an allocator that refuses every request (until it gives
// way): init completes, and platform discovery asks for nothing.

static void test_platform_init_survives_a_refusing_allocator()
{
    transport_oom_allocator_t state;
    memset( &state, 0, sizeof( state ) );
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    state.refuse_through = TRANSPORT_OOM_GIVE_WAY;

    int status = -1;
    const bool initialized = rocketnet_init_ex( &state, transport_oom_allocate, transport_oom_release, &status );
    const int asked = state.calls;
    state.refuse_through = 0;
    check( initialized );
    check( status == ROCKETNET_STATUS_OK );
    check( asked == 0 );
    transport_oom_end( &state );
    printf( "test_platform_init_survives_a_refusing_allocator\n" );
}

REGISTER_UNIT_TEST( test_transport_open_reports_out_of_memory );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_transport_create_server_ladder_stops_on_out_of_memory );
REGISTER_UNIT_TEST( test_transport_create_client_ladder_stops_on_out_of_memory );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_platform_init_survives_a_refusing_allocator );

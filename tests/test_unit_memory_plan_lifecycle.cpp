/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The memory plan covers the whole lifecycle: rocketnet_server_required_memory
// and rocketnet_client_required_memory report the peak the caller's allocator
// sees over create, start or connect, stop or disconnect, the second start or
// connect, and destroy, and the second cycle takes no more than the first.
// A counting allocator observes every rocketnet_malloc; the transport is the
// in-memory loopback, the client is step-driven, and the unit build's stats
// worker is manual: no socket, no thread, no sleep, and the clock starts at
// 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "token_string.h"

#include <cstddef>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define MEMORY_PLAN_PROTOCOL 0x7A11C1C1E5EED0A1ULL

// ---------------------------------------------------------------------------
// The counting allocator: the bytes each request asks for, the bytes live and
// the highest live seen. rocketnet_init accepts it only on an uninitialized
// library with nothing live, so the worker's init is stopped first and
// restored after.
// ---------------------------------------------------------------------------

struct alignas( std::max_align_t ) memory_plan_allocation_t
{
    size_t bytes;
};

static uint64_t memory_plan_live = 0;
static uint64_t memory_plan_peak = 0;

static void * memory_plan_malloc( void * context, size_t bytes )
{
    (void) context;
    memory_plan_allocation_t * allocation = (memory_plan_allocation_t *) malloc( sizeof( memory_plan_allocation_t ) + bytes );
    if ( !allocation )
        return NULL;
    allocation->bytes = bytes;
    memory_plan_live += bytes;
    if ( memory_plan_live > memory_plan_peak )
        memory_plan_peak = memory_plan_live;
    return allocation + 1;
}

static void memory_plan_free( void * context, void * p )
{
    (void) context;
    if ( !p )
        return;
    memory_plan_allocation_t * allocation = (memory_plan_allocation_t *) p - 1;
    memory_plan_live -= allocation->bytes;
    free( allocation );
}

static double memory_plan_now = 1000.0;
static double memory_plan_clock( void * ) { return memory_plan_now; }

// the live bytes now, from which the next peak is measured
static uint64_t memory_plan_mark()
{
    memory_plan_peak = memory_plan_live;
    return memory_plan_live;
}

static uint64_t memory_plan_peak_above( uint64_t base )
{
    return memory_plan_peak - base;
}

static void test_memory_plan_covers_lifecycle()
{
    rocketnet_term();
    check( rocketnet_init( NULL, memory_plan_malloc, memory_plan_free ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( true );
    memory_plan_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5A ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    // the server: create, start, stop, stopped, start, stop, stopped, destroy

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = MEMORY_PLAN_PROTOCOL;
    server_config.max_clients = 4;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";

    const uint64_t server_required = rocketnet_server_required_memory( &server_config );
    check( server_required > 0 );

    const uint64_t server_base = memory_plan_mark();
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    rocketnet_server_update( server, memory_plan_now );
    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );
    const uint64_t server_first_peak = memory_plan_peak_above( server_base );

    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    rocketnet_server_update( server, memory_plan_now );
    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );
    const uint64_t server_second_peak = memory_plan_peak_above( server_base );

    if ( server_first_peak > server_required || server_second_peak != server_first_peak )
        printf( "server: required %llu, first cycle peak %llu, second cycle peak %llu\n", (unsigned long long) server_required,
                (unsigned long long) server_first_peak, (unsigned long long) server_second_peak );
    check( server_first_peak <= server_required );
    check( server_second_peak == server_first_peak );

    // the client: create, connect, disconnect, disconnected, connect,
    // disconnect, disconnected, destroy. The running server is the token's
    // target and is not updated, so it allocates nothing while the client
    // is measured.

    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, server, MEMORY_PLAN_PROTOCOL, addresses, 1, 0x4D454D, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = 1;
    client_config.time_callback = memory_plan_clock;

    const uint64_t client_required = rocketnet_client_required_memory( &client_config );
    check( client_required > 0 );

    const uint64_t client_base = memory_plan_mark();
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    rocketnet_client_update( client, memory_plan_now );
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );
    const uint64_t client_first_peak = memory_plan_peak_above( client_base );

    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    rocketnet_client_update( client, memory_plan_now );
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );
    const uint64_t client_second_peak = memory_plan_peak_above( client_base );

    if ( client_first_peak > client_required || client_second_peak != client_first_peak )
        printf( "client: required %llu, first cycle peak %llu, second cycle peak %llu\n", (unsigned long long) client_required,
                (unsigned long long) client_first_peak, (unsigned long long) client_second_peak );
    check( client_first_peak <= client_required );
    check( client_second_peak == client_first_peak );

    rocketnet_client_destroy( client );
    check( memory_plan_live == client_base );

    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );
    rocketnet_server_destroy( server );
    check( memory_plan_live == server_base );

    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_memory_plan_covers_lifecycle );

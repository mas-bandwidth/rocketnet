/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The lifecycle checks that start a real transport or open a platform socket.
    The allocation, alignment, thread, and arithmetic checks stay in
    test_lifecycle.cpp, which is part of the unit suite.
*/

#include "../native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_fragment.h"
#include "rocketnet_memory.h"
#if ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#endif // #if ROCKETNET_XDP
#include "../portable_crt.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// defined in tests/functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

#define check( c ) do { if (!(c)) { fprintf(stderr, "lifecycle transport check failed: %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct allocator_state
{
    std::atomic<int> live{ 0 };
    std::atomic<int> calls{ 0 };
    std::atomic<int> fail_at{ 0 };
};

static void * allocate( void * context, size_t bytes )
{
    auto & state = *(allocator_state *) context;

    const int call = ++state.calls;
    if ( state.fail_at == call )
        return NULL;

    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;
    static_assert( alignof( max_align_t ) <= 16, "test allocator needs a wider alignment offset" );

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memset( (void *) address, 0xA5, bytes );
    ++state.live;

    return (void *) address;
}

static void release( void * context, void * pointer )
{
    auto & state = *(allocator_state *) context;

    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    check( --state.live >= 0 );
    free( base );
}

void rocketnet_test_lifecycle_transport()
{
    // The runner already initialized the library. This test owns the
    // allocator, so shut that down and start again with the failing allocator.

    rocketnet_term();

    allocator_state state;

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    check( rocketnet_init( &state, allocate, release ) == ROCKETNET_STATUS_OK );
    rocketnet_forbid_xdp_commit();

    const int baseline = state.live;

    const uint64_t large_geometry_bytes = rocketnet_reassembly_memory_required( 1000, 256, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 256000 );
    const int before = state.calls;
    if ( large_geometry_bytes > (uint64_t) ROCKETNET_REASSEMBLY_MEMORY_MIB * 1024 * 1024 )
    {
        rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        config.max_clients = 1000;
        config.max_fragmented_messages = 256;
        snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
        check( !rocketnet_server_transport_create( &state, "127.0.0.1", &config ) );
        check( state.calls == before );
    }
#if ROCKETNET_XDP
    const uint64_t xdp_huge = rocketnet_server_socket_memory_required( 1024 );
    if ( xdp_huge > (uint64_t) ROCKETNET_XDP_MEMORY_MIB * 1024 * 1024 )
    {
        check( !rocketnet_server_socket_preflight( "127.0.0.1", 1024 ) );
    }

    check( state.calls == before );
#endif // #if ROCKETNET_XDP

    rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "127.0.0.1" ) );
    state.fail_at = state.calls + 1;
    check( !rocketnet_platform_socket_create( &state, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 65536, 65536 ) );
    state.fail_at = 0;
    check( state.live == baseline );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_test_connect_token( "127.0.0.1:40000", 1, rocketnet_functional_token_timestamp( 0, 0.0 ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "portable" );

    int client_success = 0;

    for ( int failure = 1; failure <= 128; ++failure )
    {
        state.fail_at = state.calls + failure;

        auto * client = rocketnet_client_transport_create( &state, token, &client_config, 0.0 );

        state.fail_at = 0;
        if ( client )
        {
            check( (uintptr_t) client % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
            rn_native_client_transport_stop_and_destroy( client );
            client_success = failure;
        }

        check( state.live == baseline );
        if ( client_success )
            break;
    }

    check( client_success );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "portable" );

    int server_success = 0;

    for ( int failure = 1; failure <= 128; ++failure )
    {
        state.fail_at = state.calls + failure;

        auto * server = rocketnet_server_transport_create( &state, "127.0.0.1", &server_config );

        state.fail_at = 0;
        if ( server )
        {
            check( (uintptr_t) server % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
            rn_native_server_transport_stop_and_destroy( server );
            server_success = failure;
        }

        check( state.live == baseline );
        if ( server_success )
            break;
    }

    check( server_success );
    rocketnet_term();

    char verbose[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", verbose, sizeof( verbose ) ) )
    {
        printf( "lifecycle transport OK: client %d, server %d allocation failure stages\n", client_success - 1, server_success - 1 );
    }
}

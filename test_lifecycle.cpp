/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#include "rocketnet.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define check(c) do { if (!(c)) { fprintf(stderr, "lifecycle check failed: %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct allocator_state
{
    std::atomic<int> live{0};
    std::atomic<int> calls{0};
    std::atomic<int> fail_at{0};
};

static void * allocate( void * context, size_t bytes )
{
    auto & state = *(allocator_state*) context;
    const int call = ++state.calls;
    if ( state.fail_at == call )
        return NULL;
    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;
    // Valid fundamental alignment, deliberately not cache-line alignment.
    static_assert( alignof(max_align_t) <= 16, "test allocator needs a wider alignment offset" );
    uintptr_t address = ( ( (uintptr_t) base + sizeof(void*) + 63 ) & ~(uintptr_t)63 ) + 16;
    memcpy( (void*) ( address - sizeof(void*) ), &base, sizeof(base) );
    memset( (void*) address, 0xA5, bytes );
    ++state.live;
    return (void*) address;
}

static void release( void * context, void * pointer )
{
    auto & state = *(allocator_state*) context;
    void * base = NULL;
    memcpy( &base, (uint8_t*) pointer - sizeof(void*), sizeof(base) );
    check( --state.live >= 0 );
    free( base );
}

static void * refuse_allocation( void *, size_t ) { return NULL; }
static void refuse_free( void *, void * ) { check(false); }
static void thread_body( void * ) {}

int main()
{
    allocator_state state;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_allocator( allocate, release );
    check( rocketnet_malloc( &state, SIZE_MAX ) == NULL );
    check( state.calls == 0 );
    for ( size_t size = 0; size < 256; ++size )
    {
        void * p = rocketnet_malloc( &state, size );
        check( p && (uintptr_t)p % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
        memset( p, 0, size );
        rocketnet_free( &state, p );
    }
    void * p = rocketnet_malloc( &state, 1 );
    rocketnet_allocator( refuse_allocation, refuse_free ); // live allocation prevents replacement
    rocketnet_free( &state, p );
    check( state.live == 0 );
    check( rocketnet_init( &state ) );
    check( !rocketnet_init( NULL ) );
    rocketnet_forbid_xdp_commit();
    rocketnet_allocator( refuse_allocation, refuse_free ); // initialized library prevents replacement
    p = rocketnet_malloc( &state, 16 );
    check( p );
    rocketnet_term(); // live allocation prevents premature shutdown
    check( !rocketnet_init( NULL ) );
    rocketnet_free( &state, p );
    const int baseline = state.live;

    rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "127.0.0.1" ) );
    state.fail_at = state.calls + 1;
    check( !rocketnet_platform_socket_create( &state, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 65536, 65536 ) );
    state.fail_at = 0;
    check( state.live == baseline );

    for ( int failure = 1; failure <= 2; ++failure )
    {
        state.fail_at = state.calls + failure;
        check( !rocketnet_platform_thread_create( &state, thread_body, NULL ) );
        state.fail_at = 0;
        check( state.live == baseline );
    }
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( &state, thread_body, NULL );
    check( thread );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    check( state.live == baseline );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_dev_connect_token( "127.0.0.1:40000", 1, (uint64_t) time(NULL), token ) );
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    strcpy( client_config.driver, "portable" );
    int client_success = 0;
    for ( int failure = 1; failure <= 64; ++failure )
    {
        state.fail_at = state.calls + failure;
        auto * client = rocketnet_client_transport_create( &state, token, &client_config, 0.0 );
        state.fail_at = 0;
        if ( client )
        {
            check( (uintptr_t)client % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
            rocketnet_client_transport_destroy( client );
            client_success = failure;
        }
        check( state.live == baseline );
        if ( client_success ) break;
    }
    check( client_success );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;
    strcpy( server_config.driver, "portable" );
    int server_success = 0;
    for ( int failure = 1; failure <= 64; ++failure )
    {
        state.fail_at = state.calls + failure;
        auto * server = rocketnet_server_transport_create( &state, "127.0.0.1", &server_config, 0.0 );
        state.fail_at = 0;
        if ( server )
        {
            check( (uintptr_t)server % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
            rocketnet_server_transport_destroy( server );
            server_success = failure;
        }
        check( state.live == baseline );
        if ( server_success ) break;
    }
    check( server_success );
    rocketnet_term();
    rocketnet_term();
    check( state.live == 0 );
    rocketnet_allocator( refuse_allocation, refuse_free );
    check( rocketnet_malloc( NULL, 16 ) == NULL );
    rocketnet_allocator( allocate, release );
    check( rocketnet_init( &state ) );
    rocketnet_term();
    check( state.live == 0 );
    printf( "lifecycle OK: client %d, server %d allocation failure stages; aligned allocator, thread/socket refusal and init/term\n", client_success - 1, server_success - 1 );
    return 0;
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The instance heap (sdk/rocketnet_allocator.h): tlsf over one block a
// client or server takes at create, so the installed allocator is called at
// create and destroy only.
//
//   * the heap itself: a plan of allocations fits rocketnet_heap_required_bytes
//     of it, freeing everything coalesces the pool back, a full heap refuses
//     and counts, and tlsf's integrity walk passes throughout;
//   * the client: under a counting allocator, connect, the handshake,
//     disconnect and three reconnects call the installed allocator not once
//     after create, and destroy frees the one block create took;
//   * the server: start, stop and three restarts, the same.
//
// No socket, no real time and no thread: the client is step-driven on the
// loopback transport, on a test clock starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_connect_token.h"
#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_memory.h"
#include "rocketnet_testing.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

// a plan of allocations in a heap of exactly the plan's required bytes: all of
// them fit, every free coalesces, and the same plan fits again and again

static void test_instance_heap_holds_its_plan()
{
    const uint64_t sizes[] = { 8650752, 1, 64, 4096, 65536, 24, 1500 * 256, 999, 3, 1 << 20 };
    const int num_sizes = (int) ( sizeof( sizes ) / sizeof( sizes[0] ) );
    uint64_t plan = 0;
    for ( int i = 0; i < num_sizes; i++ )
        check( rocketnet_memory_add( plan, 1, sizes[i] + rocketnet_allocation_overhead() ) );

    const uint64_t bytes = rocketnet_heap_required_bytes( plan );
    check( bytes > plan );

    void * block = malloc( (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT );
    check( block != NULL );
    uint8_t * aligned = (uint8_t *) ( ( (uintptr_t) block + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );

    rocketnet_heap_t * heap = rocketnet_heap_create( aligned, bytes );
    check( heap != NULL );
    check( rocketnet_heap_check( heap ) );

    void * pointers[sizeof( sizes ) / sizeof( sizes[0] )];
    for ( int cycle = 0; cycle < 4; cycle++ )
    {
        rocketnet_heap_scope_t scope( heap );

        for ( int i = 0; i < num_sizes; i++ )
        {
            pointers[i] = rocketnet_malloc( NULL, (size_t) sizes[i] );
            check( pointers[i] != NULL );
            check( ( (uintptr_t) pointers[i] % ROCKETNET_ALLOCATION_ALIGNMENT ) == 0 );
            memset( pointers[i], 0xA5, (size_t) sizes[i] );
        }

        check( rocketnet_heap_live_allocations( heap ) == (uint64_t) num_sizes );
        check( rocketnet_heap_refusals( heap ) == 0 );
        check( rocketnet_heap_check( heap ) );

        // freed in a different order every cycle, so coalescing is exercised from both sides

        for ( int i = 0; i < num_sizes; i++ )
        {
            const int index = ( cycle % 2 ) ? i : num_sizes - 1 - i;
            rocketnet_free( NULL, pointers[index] );
        }

        check( rocketnet_heap_live_allocations( heap ) == 0 );
        check( rocketnet_heap_check( heap ) );
    }

    check( rocketnet_heap_peak_bytes( heap ) >= plan - (uint64_t) num_sizes * rocketnet_allocation_overhead() );

    free( block );
}

REGISTER_UNIT_TEST( test_instance_heap_holds_its_plan );

// a full heap refuses, counts the refusal, and still takes what fits

static void test_instance_heap_refuses_when_full()
{
    const uint64_t plan = 4096 + rocketnet_allocation_overhead();
    const uint64_t bytes = rocketnet_heap_required_bytes( plan );

    void * block = malloc( (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT );
    check( block != NULL );
    uint8_t * aligned = (uint8_t *) ( ( (uintptr_t) block + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );

    check( rocketnet_heap_create( NULL, bytes ) == NULL );
    check( rocketnet_heap_create( aligned + 8, bytes ) == NULL );
    check( rocketnet_heap_create( aligned, 64 ) == NULL );

    rocketnet_heap_t * heap = rocketnet_heap_create( aligned, bytes );
    check( heap != NULL );

    {
        rocketnet_heap_scope_t scope( heap );

        void * whole = rocketnet_malloc( NULL, 4096 );
        check( whole != NULL );
        check( rocketnet_malloc( NULL, (size_t) bytes ) == NULL );
        check( rocketnet_heap_refusals( heap ) == 1 );

        rocketnet_free( NULL, whole );
        check( rocketnet_heap_live_allocations( heap ) == 0 );

        rocketnet_heap_refuse_allocation_for_test( heap, 2 );
        void * first = rocketnet_malloc( NULL, 16 );
        check( first != NULL );
        check( rocketnet_malloc( NULL, 16 ) == NULL );
        check( rocketnet_heap_refusals( heap ) == 2 );
        void * third = rocketnet_malloc( NULL, 16 );
        check( third != NULL );

        rocketnet_free( NULL, first );
        rocketnet_free( NULL, third );
    }

    check( rocketnet_heap_live_allocations( heap ) == 0 );
    check( rocketnet_heap_check( heap ) );
    check( rocketnet_heap_required_bytes( 0 ) == 0 );

    free( block );
}

REGISTER_UNIT_TEST( test_instance_heap_refuses_when_full );

// the installed allocator, counted

struct heap_counting_allocator_t
{
    uint64_t mallocs;
    uint64_t frees;
    uint64_t live;
};

// the library's own calls (init's context is NULL) are not counted

static void * heap_counting_malloc( void * context, size_t bytes )
{
    heap_counting_allocator_t * counts = (heap_counting_allocator_t *) context;
    if ( counts )
    {
        counts->mallocs++;
        counts->live++;
    }

    return malloc( bytes );
}

static void heap_counting_free( void * context, void * p )
{
    heap_counting_allocator_t * counts = (heap_counting_allocator_t *) context;
    if ( !p )
        return;

    if ( counts )
    {
        counts->frees++;
        counts->live--;
    }

    free( p );
}

#define HEAP_PROTOCOL 0x4EA9C11E47A110CAULL

static double heap_now = 1000.0;

static double heap_clock( void * context )
{
    (void) context;
    return heap_now;
}

static void heap_token( rocketnet_server_t * server, const uint8_t * backend_secret, uint64_t client_id, char * token_string )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( backend_secret, server_public, HEAP_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 30, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

// Finding 2 of the allocation audit: the client allocated its transport at
// every connect. Now create takes one block holding the heap, and connect,
// the handshake, disconnect and reconnect take nothing from the installed
// allocator; destroy frees the one block.

static void test_instance_heap_client_connect_allocates_nothing_after_create()
{
    heap_counting_allocator_t client_counts = {};

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, heap_counting_malloc, heap_counting_free ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( 1 );

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    // the server's allocations go through the same allocator with its own
    // context, the client's are counted apart

    heap_counting_allocator_t server_counts = {};
    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = HEAP_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( &server_counts, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL && status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = heap_clock;
    rocketnet_client_t * client = rocketnet_client_create( &client_counts, &client_config, &status );
    check( client != NULL && status == ROCKETNET_STATUS_OK );
    check( client_counts.mallocs == 1 );
    check( client_counts.frees == 0 );

    for ( int generation = 0; generation < 4; generation++ )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        heap_token( server, backend_secret, 0x1EA9 + (uint64_t) generation, token );
        check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

        for ( int i = 0; i < 500 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        {
            heap_now += 0.01;
            rocketnet_client_update( client, heap_now );
            rocketnet_client_send_packets( client );
            rocketnet_server_update( server, heap_now );
            rocketnet_server_send_packets( server );
        }

        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

        rocketnet_client_disconnect( client );
        for ( int i = 0; i < 64 && !rocketnet_client_disconnected( client ); i++ )
        {
            heap_now += 0.01;
            rocketnet_client_update( client, heap_now );
            rocketnet_server_update( server, heap_now );
        }

        check( rocketnet_client_disconnected( client ) );

        // the installed allocator saw create's one block and nothing since

        check( client_counts.mallocs == 1 );
        check( client_counts.frees == 0 );
    }

    check( rocketnet_heap_refusals( rocketnet_client_heap( client ) ) == 0 );
    check( rocketnet_heap_check( rocketnet_client_heap( client ) ) );

    rocketnet_client_destroy( client );
    check( client_counts.mallocs == 1 );
    check( client_counts.frees == 1 );
    check( client_counts.live == 0 );

    rn_test_server_stop_and_destroy( server );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_instance_heap_client_connect_allocates_nothing_after_create );

// Finding 3 of the allocation audit: the server allocated its transport at
// every start. Now create takes one block holding the heap, and start, stop
// and restart take nothing from the installed allocator; destroy frees the
// one block.

static void test_instance_heap_server_start_allocates_nothing_after_create()
{
    heap_counting_allocator_t server_counts = {};

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, heap_counting_malloc, heap_counting_free ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( 1 );

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5C ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = HEAP_PROTOCOL;
    server_config.max_clients = 4;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( &server_counts, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL && status == ROCKETNET_STATUS_OK );
    check( server_counts.mallocs == 1 );

    for ( int run = 0; run < 4; run++ )
    {
        check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

        for ( int i = 0; i < 10; i++ )
        {
            heap_now += 0.01;
            rocketnet_server_update( server, heap_now );
            rocketnet_server_send_packets( server );
        }

        rocketnet_server_stop( server );
        for ( int i = 0; i < 64 && !rocketnet_server_stopped( server ); i++ )
        {
            heap_now += 0.01;
            rocketnet_server_update( server, heap_now );
        }

        check( rocketnet_server_stopped( server ) );

        // the installed allocator saw create's one block and nothing since

        check( server_counts.mallocs == 1 );
        check( server_counts.frees == 0 );
    }

    rocketnet_server_destroy( server );
    check( server_counts.mallocs == 1 );
    check( server_counts.frees == 1 );
    check( server_counts.live == 0 );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_instance_heap_server_start_allocates_nothing_after_create );

#endif // #if ROCKETNET_DEV

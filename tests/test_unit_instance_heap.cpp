/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The instance heap (sdk/rocketnet_allocator.h): tlsf over one block a
// client or server takes at create, so the installed allocator is called at
// create and destroy only.
//
//   * the client: under a counting allocator, connect, the handshake,
//     disconnect and three reconnects call the installed allocator not once
//     after create, and destroy frees the one block create took.
//
// No socket, no real time and no thread: the client is step-driven on the
// loopback transport, on a test clock starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_connect_token.h"
#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_testing.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

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

    rocketnet_client_destroy( client );
    check( client_counts.mallocs == 1 );
    check( client_counts.frees == 1 );
    check( client_counts.live == 0 );

    rn_test_server_stop_and_destroy( server );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_instance_heap_client_connect_allocates_nothing_after_create );

#endif // #if ROCKETNET_DEV

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The instance heap (sdk/rocketnet_allocator.h): tlsf over one block a
// client takes at create, so the installed allocator is called at create and
// destroy only. Under a counting allocator, connect, the handshake,
// disconnect and three reconnects call the installed allocator not once
// after create, and destroy frees the one block create took.
//
// No socket, no real time and no thread: the client is step-driven on the
// loopback transport, on a test clock starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "loopback_hub.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

#define HEAP_PROTOCOL 0x4EA9C11E47A110CAULL

struct heap_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
};

static void heap_identity( heap_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );

    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
}

static void heap_token_string( const rocketnet_server_t * server, const heap_identity_t * identity, uint64_t client_id, char * token_string )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( identity->backend_secret, server_public, HEAP_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

static double heap_now = 1000.0;
static double heap_clock( void * ) { return heap_now; }

static void heap_server_config( rocketnet_server_config_t * config, const heap_identity_t * identity )
{
    rocketnet_server_default_config( config );
    config->protocol_id = HEAP_PROTOCOL;
    config->max_clients = 1;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
    config->loopback_hub = rocketnet_test_loopback_hub();
}

static void heap_client_config( rocketnet_client_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->loopback_hub = rocketnet_test_loopback_hub();
    config->step_driven = true;
    config->time_callback = heap_clock;
}

// the installed allocator, counted per object: each object passes its own
// counts as its context, and the library's own calls (context NULL) are not
// counted

struct heap_counting_allocator_t
{
    uint64_t mallocs;
    uint64_t frees;
    uint64_t live;
};

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

static void heap_pump( rocketnet_client_t * client, rocketnet_server_t * server )
{
    heap_now += 0.01;
    rocketnet_client_update( client, heap_now );
    rocketnet_client_send_packets( client );
    rocketnet_server_update( server, heap_now );
    rocketnet_server_send_packets( server );
}

// one connect, handshake, ten connected frames and disconnect, until the
// server's slot is free for the next one

static void heap_client_generation( rocketnet_client_t * client, rocketnet_server_t * server, const heap_identity_t * identity, uint64_t client_id )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    heap_token_string( server, identity, client_id, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        heap_pump( client, server );

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    for ( int i = 0; i < 10; i++ )
        heap_pump( client, server );

    rocketnet_client_disconnect( client );
    for ( int i = 0; i < 64 && !rocketnet_client_disconnected( client ); i++ )
        heap_pump( client, server );

    check( rocketnet_client_disconnected( client ) );

    for ( int i = 0; i < 64 && rocketnet_server_client_connected( server, 0 ); i++ )
        heap_pump( client, server );

    check( !rocketnet_server_client_connected( server, 0 ) );
}

// connect, the handshake, disconnect and reconnect take nothing from the
// installed allocator after create; destroy frees create's one block

static void test_instance_heap_client_connect_allocates_nothing_after_create()
{
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, heap_counting_malloc, heap_counting_free ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( 1 );

    heap_identity_t identity;
    heap_identity( &identity );

    heap_counting_allocator_t server_counts = {};
    rocketnet_server_config_t server_config;
    heap_server_config( &server_config, &identity );
    rocketnet_server_t * server = rocketnet_server_create( &server_counts, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    heap_counting_allocator_t client_counts = {};
    rocketnet_client_config_t client_config;
    heap_client_config( &client_config );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( &client_counts, &client_config, &status );
    check( client != NULL && status == ROCKETNET_STATUS_OK );
    check( client_counts.mallocs == 1 );
    check( client_counts.frees == 0 );

    for ( int generation = 0; generation < 4; generation++ )
    {
        heap_client_generation( client, server, &identity, 0x1EA9 + (uint64_t) generation );

        // the installed allocator saw create's one block and nothing since

        printf( "generation %d: client mallocs %llu frees %llu\n", generation, (unsigned long long) client_counts.mallocs, (unsigned long long) client_counts.frees );
        check( client_counts.mallocs == 1 );
        check( client_counts.frees == 0 );
    }

    rocketnet_client_destroy( client );
    check( client_counts.mallocs == 1 );
    check( client_counts.frees == 1 );
    check( client_counts.live == 0 );

    rn_test_server_stop_and_destroy( server );
    check( server_counts.live == 0 );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_instance_heap_client_connect_allocates_nothing_after_create );

#endif // #if ROCKETNET_DEV

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// rocketnet_message_server_required_memory and the caller's block at
// rocketnet_message_server_create (mutation gaps, group budget):
//
//   * a client count above ROCKETNET_MAX_CLIENTS has no layout (required
//     memory 0) though the message arena alone would size it;
//   * a send budget of exactly ROCKETNET_SEND_BUDGET_MAX_KBPS has a layout,
//     and one above it, 2^31 and up included (which would wrap negative as
//     an int32_t and size as uncapped), has none;
//   * the public create sizes the block for the budget it is given, not for
//     an unlimited one: a block of exactly required_memory makes the server
//     in place;
//   * the create sizes the block for the server's layout: a block one byte
//     short is refused with OUT_OF_MEMORY before the transport opens
//     (nothing allocated).
//
// The server transport is loopback: no socket, no thread, no sleep.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_config.h"
#include "loopback_hub.h"
#include "unit_shutdown.h"

#define check rocketnet_unit_check

#define gm_expect( condition, ... )                                                                                   \
    do                                                                                                                \
    {                                                                                                                 \
        if ( !( condition ) )                                                                                         \
        {                                                                                                             \
            printf( "mutation gap fault: " __VA_ARGS__ );                                                             \
            printf( "\n" );                                                                                           \
        }                                                                                                             \
        check( condition );                                                                                           \
    } while ( 0 )

static uint64_t gm_mallocs = 0;

static void * gm_malloc( void *, size_t bytes )
{
    gm_mallocs++;
    return malloc( bytes );
}

static void gm_free( void *, void * p )
{
    free( p );
}

static void * gm_aligned( std::vector<uint8_t> & storage, uint64_t bytes )
{
    storage.assign( (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT, 0 );
    const uintptr_t base = (uintptr_t) storage.data();
    return (void *) ( ( base + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
}

static void gm_server_config( struct rocketnet_server_transport_config_t * config, int max_clients )
{
    rocketnet_server_transport_default_config_current( config );
    config->protocol_id = 0x676d72657175ULL;
    config->max_clients = max_clients;
    memset( config->backend_public_key, 0x42, sizeof( config->backend_public_key ) );
    memset( config->server_public_key, 0x43, sizeof( config->server_public_key ) );
    memset( config->server_secret_key, 0x44, sizeof( config->server_secret_key ) );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );   // not the published test filter, which a production build refuses
    snprintf( config->transport, sizeof( config->transport ), "%s", "loopback" );
    config->loopback_hub = rocketnet_test_loopback_hub();
}

// mutation gap: hunt 1/791, 796 (a client count above the maximum sized),
// hunt 1/1924 (the maximum budget refused), hunt 1/1927 (a budget above the
// maximum sized as uncapped)

static void test_gaps_budget_message_server_required_memory_bounds()
{
    struct rocketnet_server_transport_config_t config;

    gm_server_config( &config, ROCKETNET_MAX_CLIENTS );
    gm_expect( rocketnet_message_server_required_memory( &config, NULL, 0 ) > 0, "%d clients, the maximum, has no layout", ROCKETNET_MAX_CLIENTS );

    const int over[] = { ROCKETNET_MAX_CLIENTS + 1, 1 << 20 };
    for ( int i = 0; i < 2; i++ )
    {
        gm_server_config( &config, over[i] );
        const uint64_t bytes = rocketnet_message_server_required_memory( &config, NULL, 0 );
        gm_expect( bytes == 0, "%d clients, above the maximum %d, sized %llu bytes", over[i], ROCKETNET_MAX_CLIENTS, (unsigned long long) bytes );
    }

    // the shortest resend window, so the maximum budget's resend reserve fits
    // (the defaults' two-second window has no layout above about 30000000 kbps)

    rocketnet_message_config_t fast;
    rocketnet_message_default_config( &fast );
    fast.message_resend_delay_min_ms = 0;
    fast.message_resend_delay_max_ms = 10;
    fast.message_resend_delay_initial_ms = 10;
    gm_server_config( &config, 1 );
    gm_expect( rocketnet_message_server_required_memory( &config, &fast, ROCKETNET_SEND_BUDGET_MAX_KBPS ) > 0, "a send budget of exactly the maximum %u kbps has no layout",
               (unsigned) ROCKETNET_SEND_BUDGET_MAX_KBPS );

    const uint32_t above[] = { (uint32_t) ROCKETNET_SEND_BUDGET_MAX_KBPS + 1, 0x80000000u, 0xFFFFFFFFu };
    for ( int i = 0; i < 3; i++ )
    {
        const uint64_t bytes = rocketnet_message_server_required_memory( &config, &fast, above[i] );
        gm_expect( bytes == 0, "a send budget of %u kbps, above the maximum, sized %llu bytes", (unsigned) above[i], (unsigned long long) bytes );
    }
}

REGISTER_UNIT_TEST( test_gaps_budget_message_server_required_memory_bounds );

// mutation gap: hunt 1/1930 (the public create reserves the unlimited
// layout), hunt 1/1949 (the block checked against a client's layout)

static void test_gaps_budget_message_server_block_is_the_server_layout()
{
    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, gm_malloc, gm_free ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t config;
    gm_server_config( &config, 2 );

    rocketnet_message_config_t message;
    rocketnet_message_default_config( &message );
    message.server_send_kbps = 1000;
    message.client_send_kbps = 1000;

    const uint64_t required = rocketnet_message_server_required_memory( &config, &message, 0 );
    check( required > 1 );

    std::vector<uint8_t> storage;
    void * block = gm_aligned( storage, required );

    // one byte short: refused before the transport opens

    int status = ROCKETNET_TEST_STATUS_UNSET;
    const uint64_t before = gm_mallocs;
    struct rocketnet_message_server_t * server = rocketnet_message_server_create_internal( NULL, "127.0.0.1:40000", &config, &message, block, required - 1, 0, &status, 0 );
    gm_expect( server == NULL, "a block of %llu bytes, one short of the server's %llu, made a server", (unsigned long long) ( required - 1 ), (unsigned long long) required );
    rn_test_message_server_stop_and_destroy( server );
    gm_expect( status == ROCKETNET_STATUS_OUT_OF_MEMORY, "a block one byte short: status %s, not OUT_OF_MEMORY", rocketnet_status_name( (enum rocketnet_status_t) status ) );
    gm_expect( gm_mallocs == before, "a block one byte short was refused after %llu allocations, not before the transport opened", (unsigned long long) ( gm_mallocs - before ) );

    // exactly required_memory: the public create makes the server in the block

    server = rocketnet_message_server_create( NULL, "127.0.0.1:40000", &config, &message, block, required, 0 );
    gm_expect( server != NULL, "the public create refused a block of exactly the %llu bytes required_memory gave", (unsigned long long) required );
    gm_expect( (void *) server == block, "the public create did not make the server in the caller's block" );
    rn_test_message_server_stop_and_destroy( server );

    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_gaps_budget_message_server_block_is_the_server_layout );

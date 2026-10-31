/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message client and server create refuse an out-of-range send budget or a
// config whose required memory is zero before a socket opens, allocating
// nothing. Time is only an argument: nothing sleeps, nothing opens a socket.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_config.h"
#include "loopback_hub.h"
#include "unit_shutdown.h"

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// The counted allocator, as test_unit_message_objects.cpp carries its mo_*
// pair: a create that refuses before the transport opens a socket allocates
// nothing.
// ---------------------------------------------------------------------------

static uint64_t mcr_mallocs = 0;

static void * mcr_malloc( void * context, size_t bytes )
{
    (void) context;
    mcr_mallocs++;
    return malloc( bytes );
}

static void mcr_free( void * context, void * p )
{
    (void) context;
    free( p );
}

// ---------------------------------------------------------------------------
// The client create refuses an out-of-range send budget or a config whose
// required memory is zero before rocketnet_client_transport_create opens a
// socket, allocating nothing (sdk/rocketnet_message_client.cpp,
// rocketnet_message_client_create).
// ---------------------------------------------------------------------------

static void test_message_client_create_refuses_budget_and_config()
{
    rocketnet_term();
    check( rocketnet_init( NULL, mcr_malloc, mcr_free ) == ROCKETNET_STATUS_OK );

    // a config whose required memory is zero: num_channels = 0 fails
    // rocketnet_message_validate_config (1..ROCKETNET_MAX_CHANNELS), so
    // rocketnet_message_client_required_memory returns 0

    rocketnet_message_config_t bad;
    rocketnet_message_default_config( &bad );
    bad.num_channels = 0;
    check( rocketnet_message_client_required_memory( &bad, 0 ) == 0 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );

    const uint64_t before = mcr_mallocs;

    // a send budget above the maximum: refused before any allocation

    check( rocketnet_message_client_create( NULL, token, NULL, NULL, NULL, 0, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == NULL );

    // an invalid message config (zero required memory): refused before any allocation

    check( rocketnet_message_client_create( NULL, token, NULL, &bad, NULL, 0, 0 ) == NULL );

    check( mcr_mallocs == before );

    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// The server create refuses an out-of-range per-client send budget or a config
// whose required memory is zero before rocketnet_server_transport_create opens
// a socket, allocating nothing (sdk/rocketnet_message_server.cpp,
// rocketnet_message_server_create).
// ---------------------------------------------------------------------------

static void test_message_server_create_refuses_budget_and_config()
{
    rocketnet_term();
    check( rocketnet_init( NULL, mcr_malloc, mcr_free ) == ROCKETNET_STATUS_OK );

    rocketnet_message_config_t bad;
    rocketnet_message_default_config( &bad );
    bad.num_channels = 0;
    check( rocketnet_message_server_required_memory( NULL, &bad, 0 ) == 0 );

    const uint64_t before = mcr_mallocs;

    // a send budget above the maximum: refused before any allocation

    check( rocketnet_message_server_create( NULL, "127.0.0.1:40000", NULL, NULL, NULL, 0, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == NULL );

    // an invalid message config (zero required memory): refused before any allocation

    check( rocketnet_message_server_create( NULL, "127.0.0.1:40000", NULL, &bad, NULL, 0, 0 ) == NULL );

    check( mcr_mallocs == before );

    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// required_memory takes the send budget create is given, and the two size the
// block alike: a create with a budget above the config's field, or with a
// positive budget over a config field of 0, makes its object in a block of
// exactly the bytes required_memory reported for that budget, and refuses a
// block of the config field's size before its transport opens (allocating
// nothing). The transports are loopback: no socket opens. Mutation that must
// bite: size with the config's field alone, whatever the create's budget; the
// checks on required_memory's answer go red.
// ---------------------------------------------------------------------------

static void * mcr_aligned( std::vector<uint8_t> & storage, uint64_t bytes )
{
    storage.assign( (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT, 0 );
    const uintptr_t base = (uintptr_t) storage.data();
    return (void *) ( ( base + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
}

static void test_message_required_memory_takes_the_create_budget()
{
    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, mcr_malloc, mcr_free ) == ROCKETNET_STATUS_OK );

    const uint64_t protocol_id = 0x6d63727265717531ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 2;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { "127.0.0.1:40000" };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x2545f4914f6cdd1dULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    // the config's field, then a create budget ten times it; and a config
    // field of 0 (no budget) with a positive create budget

    const int32_t fields[2] = { 1000, 0 };

    for ( int f = 0; f < 2; f++ )
    {
        rocketnet_message_config_t config;
        rocketnet_message_default_config( &config );
        config.server_send_kbps = fields[f];
        config.client_send_kbps = fields[f];
        const uint32_t create_kbps = 10000;

        // server

        const uint64_t at_field = rocketnet_message_server_required_memory( &server_config, &config, 0 );
        const uint64_t at_create = rocketnet_message_server_required_memory( &server_config, &config, create_kbps );
        check( at_field > 0 && at_create > at_field );

        std::vector<uint8_t> storage;
        void * block = mcr_aligned( storage, at_create );

        int status = ROCKETNET_TEST_STATUS_UNSET;
        const uint64_t before = mcr_mallocs;
        check( rocketnet_message_server_create_ex( NULL, "127.0.0.1:40000", &server_config, &config, block, at_field, create_kbps, &status, 0 ) == NULL );
        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( mcr_mallocs == before );

        rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "127.0.0.1:40000", &server_config, &config, block, at_create, create_kbps );
        check( server != NULL );
        check( (void *) server == block );

        // client

        const uint64_t client_at_field = rocketnet_message_client_required_memory( &config, 0 );
        const uint64_t client_at_create = rocketnet_message_client_required_memory( &config, create_kbps );
        check( client_at_field > 0 && client_at_create > client_at_field );

        std::vector<uint8_t> client_storage;
        void * client_block = mcr_aligned( client_storage, client_at_create );

        status = ROCKETNET_TEST_STATUS_UNSET;
        const uint64_t client_before = mcr_mallocs;
        check( rocketnet_message_client_create_ex( NULL, token, &client_config, &config, client_block, client_at_field, create_kbps, &status, 0 ) == NULL );
        check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        check( mcr_mallocs == client_before );

        rocketnet_message_client_t * client = rocketnet_message_client_create( NULL, token, &client_config, &config, client_block, client_at_create, create_kbps );
        check( client != NULL );
        check( (void *) client == client_block );

        rn_test_message_client_stop_and_destroy( client );
        rn_test_message_server_stop_and_destroy( server );
    }

    // a budget above the maximum has no layout

    check( rocketnet_message_server_required_memory( &server_config, NULL, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == 0 );
    check( rocketnet_message_client_required_memory( NULL, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == 0 );

    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_message_client_create_refuses_budget_and_config );
REGISTER_UNIT_TEST( test_message_server_create_refuses_budget_and_config );
REGISTER_UNIT_TEST( test_message_required_memory_takes_the_create_budget );

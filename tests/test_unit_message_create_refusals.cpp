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
    check( rocketnet_message_client_required_memory( &bad ) == 0 );

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
    check( rocketnet_message_server_required_memory( NULL, &bad ) == 0 );

    const uint64_t before = mcr_mallocs;

    // a send budget above the maximum: refused before any allocation

    check( rocketnet_message_server_create( NULL, "127.0.0.1:40000", NULL, NULL, NULL, 0, ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == NULL );

    // an invalid message config (zero required memory): refused before any allocation

    check( rocketnet_message_server_create( NULL, "127.0.0.1:40000", NULL, &bad, NULL, 0, 0 ) == NULL );

    check( mcr_mallocs == before );

    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_message_client_create_refuses_budget_and_config );
REGISTER_UNIT_TEST( test_message_server_create_refuses_budget_and_config );

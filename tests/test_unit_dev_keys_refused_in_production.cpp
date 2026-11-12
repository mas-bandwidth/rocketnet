/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A production build (ROCKETNET_DEV=0) refuses a server whose config carries
// either published development public key, each on its own: a token forged
// with the published development backend key is otherwise a full
// authentication bypass, and the published server pair lets anyone read the
// traffic. The refusal comes before any socket opens.
//
// A development build accepts what this checks is refused, so the case is
// registered in the production unit suite only.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "loopback_hub.h"

#include <stdlib.h>
#include <string.h>

#if !ROCKETNET_DEV

#define check rocketnet_unit_check

// production keys, a protocol id and a packet filter of the caller's own, so
// the development-key check is the only refusal the config can reach
static void production_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config_current( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
}

static void test_dev_keys_refused_in_production()
{
    // the development backend key with a production server pair

    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        memcpy( config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
        int status = -1;
        check( rocketnet_server_transport_create_current_internal( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
    }

    // the development server public key with a production backend key

    {
        struct rocketnet_server_transport_config_t config;
        production_config( &config );
        memcpy( config.server_public_key, rocketnet_dev_server_public_key, 32 );
        int status = -1;
        check( rocketnet_server_transport_create_current_internal( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        check( status == ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
    }
}

REGISTER_UNIT_TEST( test_dev_keys_refused_in_production );

// The refusal comes after the transport's own block is allocated, so the
// refused create must return it: a counting allocator sees nothing left live
// after each refusal (either key alone, both, and with no status pointer,
// which the refusal must not write through).

struct dev_keys_allocator_t
{
    int live;
    int calls;
};

static void * dev_keys_allocate( void * context, size_t bytes )
{
    dev_keys_allocator_t * state = (dev_keys_allocator_t *) context;
    ++state->calls;
    void * pointer = malloc( bytes );
    if ( pointer )
    {
        ++state->live;
    }

    return pointer;
}

static void dev_keys_release( void * context, void * pointer )
{
    dev_keys_allocator_t * state = (dev_keys_allocator_t *) context;
    --state->live;
    free( pointer );
}

static void test_dev_keys_refusal_leaves_nothing_live()
{
    dev_keys_allocator_t state = { 0, 0 };

    // the worker starts initialized: terminate, install the counting
    // allocator, and initialize with its state as the context
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( &state, dev_keys_allocate, dev_keys_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    for ( int keys = 1; keys <= 3; ++keys )
    {
        for ( int with_status = 0; with_status <= 1; ++with_status )
        {
            struct rocketnet_server_transport_config_t config;
            production_config( &config );
            if ( keys & 1 )
            {
                memcpy( config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
            }
            if ( keys & 2 )
            {
                memcpy( config.server_public_key, rocketnet_dev_server_public_key, 32 );
            }

            const int live = state.live;
            const int calls = state.calls;
            status = -1;
            check( rocketnet_server_transport_create_current_internal( &state, "127.0.0.1:12345", &config, 1000.0, with_status ? &status : NULL ) == NULL );
            check( status == ( with_status ? (int) ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION : -1 ) );
            check( state.calls > calls );
            check( state.live == live );
        }
    }

    // a clean term, then the default allocator back for the worker's own term
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_dev_keys_refusal_leaves_nothing_live );

#endif // #if !ROCKETNET_DEV

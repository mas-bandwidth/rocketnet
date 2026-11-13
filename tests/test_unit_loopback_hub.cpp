/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Unit tests for the loopback hub (rocketnet_loopback_hub_create and
// rocketnet_loopback_hub_destroy): its one allocation through the caller's
// allocator and context, its range, the refusal of a loopback create with no
// hub, the refusal of a destroy while an endpoint is registered, and the
// refusal of a hub whose header names another build. No socket, no thread, no
// clock. The registration lifecycle is modelled in proofs/tla/LoopbackHub.tla.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "plan_rss.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_connect_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

#define LOOPBACK_HUB_PROTOCOL 0x4C4F4F504855424ULL

// ---- a counting allocator: every hub allocation names its context ------------------

struct loopback_hub_allocator_t
{
    int allocations;
    int frees;
    int live;
    size_t bytes;
    int wrong_context;
};

static loopback_hub_allocator_t loopback_hub_allocator;

static void * loopback_hub_malloc( void * context, size_t bytes )
{
    if ( context != &loopback_hub_allocator )
    {
        loopback_hub_allocator.wrong_context++;
    }
    loopback_hub_allocator.allocations++;
    loopback_hub_allocator.live++;
    loopback_hub_allocator.bytes += bytes;
    // Map large blocks through the existing test allocator: Debug CRT malloc
    // fills every queue page before the SDK touches it. Keep counting the
    // full request while leaving untouched queues nonresident.
    return plan_rss_malloc( context, bytes );
}

static void loopback_hub_free( void * context, void * p )
{
    if ( !p )
    {
        return;
    }
    if ( context != &loopback_hub_allocator )
    {
        loopback_hub_allocator.wrong_context++;
    }
    loopback_hub_allocator.frees++;
    loopback_hub_allocator.live--;
    plan_rss_free( context, p );
}

// the hub's registered count, read through the development seam; a
// production build checks registration through destroy's refusal alone
static void loopback_hub_check_live( rocketnet_loopback_hub_t * hub, int live )
{
#if ROCKETNET_DEV
    check( rocketnet_loopback_endpoints_live( hub ) == live );
#else // #if ROCKETNET_DEV
    (void) hub;
    (void) live;
#endif // #if ROCKETNET_DEV
}

// a hub is one allocation through the caller's allocator and context, sized by
// rocketnet_loopback_hub_bytes, and destroy frees it with the same context

static void test_loopback_hub_create_and_destroy()
{
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    memset( &loopback_hub_allocator, 0, sizeof( loopback_hub_allocator ) );
    check( rocketnet_init( &loopback_hub_allocator, loopback_hub_malloc, loopback_hub_free ) == ROCKETNET_STATUS_OK );

    const int sizes[] = { 1, 3, 8, ROCKETNET_MAX_CLIENTS };
    for ( int i = 0; i < (int) ( sizeof( sizes ) / sizeof( sizes[0] ) ); i++ )
    {
        const int allocations = loopback_hub_allocator.allocations;
        const size_t bytes = loopback_hub_allocator.bytes;

        enum rocketnet_status_t status = ROCKETNET_STATUS_INVALID_ARGUMENT;
        rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( &loopback_hub_allocator, sizes[i], &status );
        check( hub != NULL );
        check( status == ROCKETNET_STATUS_OK );
        check( loopback_hub_allocator.allocations == allocations + 1 );
        check( loopback_hub_allocator.bytes - bytes >= rocketnet_loopback_hub_bytes( sizes[i] ) );
        check( rocketnet_loopback_hub_check( hub ) == ROCKETNET_STATUS_OK );
        loopback_hub_check_live( hub, 0 );

        // a NULL status is allowed. Below the largest size the two hubs are
        // live together and distinct; at ROCKETNET_MAX_CLIENTS the first goes
        // before the second is made, so the case holds one large hub at a
        // time (two at once passed 10 GB of RSS under MemorySanitizer)

        if ( sizes[i] == ROCKETNET_MAX_CLIENTS )
        {
            check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );
            hub = NULL;
        }

        rocketnet_loopback_hub_t * second = rocketnet_loopback_hub_create( &loopback_hub_allocator, sizes[i], NULL );
        check( second != NULL && second != hub );

        check( rocketnet_loopback_hub_destroy( second ) == ROCKETNET_STATUS_OK );
        if ( hub )
        {
            check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );
        }
        check( loopback_hub_allocator.live == 0 );
    }

    // destroy( NULL ) is a silent no-op, as free( NULL )

    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_loopback_hub_destroy( NULL ) == ROCKETNET_STATUS_OK );
    check( rocketnet_api_misuse_count() == misuse );

    check( loopback_hub_allocator.wrong_context == 0 );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( loopback_hub_allocator.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// an allocator that refuses the hub's block: create returns NULL with
// OUT_OF_MEMORY and holds nothing

static void * loopback_hub_refusing_malloc( void * context, size_t bytes )
{
    if ( bytes > 4096 )
    {
        loopback_hub_allocator.allocations++;
        return NULL;
    }

    return loopback_hub_malloc( context, bytes );
}

static void test_loopback_hub_refused_block()
{
    rocketnet_quiet( 1 );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    memset( &loopback_hub_allocator, 0, sizeof( loopback_hub_allocator ) );
    check( rocketnet_init( &loopback_hub_allocator, loopback_hub_refusing_malloc, loopback_hub_free ) == ROCKETNET_STATUS_OK );

    const int live = loopback_hub_allocator.live;
    const int allocations = loopback_hub_allocator.allocations;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_loopback_hub_create( &loopback_hub_allocator, 1, &status ) == NULL );
    check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
    check( loopback_hub_allocator.allocations == allocations + 1 );
    check( loopback_hub_allocator.live == live );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( loopback_hub_allocator.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// N is 1 .. ROCKETNET_MAX_CLIENTS; anything else is refused with
// INVALID_ARGUMENT and allocates nothing, and a hub before init is refused

static void test_loopback_hub_refuses_out_of_range()
{
    rocketnet_quiet( 1 );

    const int refused[] = { 0, -1, ROCKETNET_MAX_CLIENTS + 1, 1 << 30 };
    for ( int i = 0; i < (int) ( sizeof( refused ) / sizeof( refused[0] ) ); i++ )
    {
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        check( rocketnet_loopback_hub_create( NULL, refused[i], &status ) == NULL );
        check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_loopback_hub_bytes( refused[i] ) == 0 );
    }

    check( rocketnet_loopback_hub_bytes( 1 ) > 0 );
    check( rocketnet_loopback_hub_bytes( ROCKETNET_MAX_CLIENTS ) > rocketnet_loopback_hub_bytes( 1 ) );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_loopback_hub_create( NULL, 1, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// public configs for a loopback server and client, naming the hub given

static void loopback_hub_server_config( struct rocketnet_server_config_t * config, rocketnet_loopback_hub_t * hub )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = LOOPBACK_HUB_PROTOCOL;
    config->max_clients = 2;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, sizeof( config->backend_public_key ) );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );    // not the published test filter, which a production build refuses first
    config->transport = "loopback";
    config->loopback_hub = hub;
}

static void loopback_hub_client_config( struct rocketnet_client_config_t * config, rocketnet_loopback_hub_t * hub )
{
    check( rocketnet_client_default_config( config ) == ROCKETNET_STATUS_OK );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    config->transport = "loopback";
    config->loopback_hub = hub;
}

// a transport list naming loopback with a NULL hub is refused at create with
// LOOPBACK_HUB_MISSING; there is no process-wide table to fall back to. A list
// that does not name loopback never reads the hub.

static void test_loopback_hub_null_refused()
{
    rocketnet_quiet( 1 );

    struct rocketnet_server_config_t server_config;
    loopback_hub_server_config( &server_config, NULL );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_LOOPBACK_HUB_MISSING );
    check( rocketnet_server_required_memory( &server_config ) == 0 );

    struct rocketnet_client_config_t client_config;
    loopback_hub_client_config( &client_config, NULL );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_LOOPBACK_HUB_MISSING );
    check( rocketnet_client_required_memory( &client_config ) == 0 );

    // the transport layer refuses a loopback endpoint with no hub too

    rocketnet_server_transport_config_t transport_config;
    rocketnet_server_transport_default_config_current( &transport_config );
    const rocketnet_server_transport_io_config_t setup = { "loopback", &transport_config, NULL, NULL };
    rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t io = {};
    check( !rocketnet_server_transport_io_open( rocketnet_loopback_transport.server, NULL, &setup, &address, &io ) );
}

// destroy is refused with LOOPBACK_HUB_IN_USE, logged and counted as misuse,
// while a server or client is registered; the hub is kept and stays usable,
// and destroy succeeds once the last endpoint leaves

static void test_loopback_hub_destroy_refused_while_registered()
{
    rocketnet_quiet( 1 );

    rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 2, NULL );
    check( hub != NULL );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.loopback_hub = hub;
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    client_config.loopback_hub = hub;
    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };

    rocketnet_address_t server_address, client_address;
    check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    check( rocketnet_address_parse( &client_address, "127.0.0.1:0" ) );

    rocketnet_server_transport_io_t server = {};
    check( rocketnet_server_transport_io_open( rocketnet_loopback_transport.server, NULL, &server_setup, &server_address, &server ) );

    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_loopback_hub_check( hub ) == ROCKETNET_STATUS_OK );
    loopback_hub_check_live( hub, 1 );

    // the kept hub still takes a client, and the pair still exchanges

    rocketnet_client_transport_io_t client = {};
    check( rocketnet_client_transport_io_open( rocketnet_loopback_transport.client, NULL, &client_setup, &client_address, &client ) );
    loopback_hub_check_live( hub, 2 );

    const uint8_t data[] = { 1, 2, 3, 4 };
    rocketnet_address_t to = server_address;
    client.operations->send( client.instance, &to, data, (int) sizeof( data ) );
    rocketnet_address_t from;
    uint8_t buf[16];
    int bytes = 0;
    check( server.operations->receive_batch( server.instance, &from, buf, 16, &bytes, 1 ) == 1 );
    check( bytes == (int) sizeof( data ) && memcmp( buf, data, sizeof( data ) ) == 0 );

    rn_test_server_io_stop_and_close( &server );
    loopback_hub_check_live( hub, 1 );

    misuse = rocketnet_api_misuse_count();
    check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE );
    check( rocketnet_api_misuse_count() == misuse + 1 );

    rocketnet_client_transport_io_close( &client );
    loopback_hub_check_live( hub, 0 );
    check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );
}

// a hub whose magic, version or layout is not this build's is refused by
// every use: the public creates return LOOPBACK_HUB_MISMATCH, an endpoint
// open is refused, and destroy returns LOOPBACK_HUB_MISMATCH and keeps it.
// Restored, the same hub is destroyed.

static void test_loopback_hub_mismatch_refused()
{
    rocketnet_quiet( 1 );

    rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 1, NULL );
    check( hub != NULL );

    rocketnet_loopback_hub_header_t * header = (rocketnet_loopback_hub_header_t *) (void *) hub;
    check( header->magic == ROCKETNET_LOOPBACK_HUB_MAGIC );
    check( header->version == ROCKETNET_VERSION );
    const rocketnet_loopback_hub_header_t good = *header;

    for ( int field = 0; field < 3; field++ )
    {
        if ( field == 0 )
        {
            header->magic ^= 1;
        }
        else if ( field == 1 )
        {
            header->version += 1;
        }
        else
        {
            header->layout ^= 0x80000000u;
        }

        check( rocketnet_loopback_hub_check( hub ) == ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH );

        struct rocketnet_server_config_t server_config;
        loopback_hub_server_config( &server_config, hub );
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH );

        struct rocketnet_client_config_t client_config;
        loopback_hub_client_config( &client_config, hub );
        status = ROCKETNET_STATUS_OK;
        check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH );

        rocketnet_server_transport_config_t transport_config;
        rocketnet_server_transport_default_config_current( &transport_config );
        transport_config.loopback_hub = hub;
        const rocketnet_server_transport_io_config_t setup = { "loopback", &transport_config, NULL, NULL };
        rocketnet_address_t address;
        check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
        rocketnet_server_transport_io_t io = {};
        check( !rocketnet_server_transport_io_open( rocketnet_loopback_transport.server, NULL, &setup, &address, &io ) );

        const uint64_t misuse = rocketnet_api_misuse_count();
        check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH );
        check( rocketnet_api_misuse_count() == misuse + 1 );

        *header = good;
        check( rocketnet_loopback_hub_check( hub ) == ROCKETNET_STATUS_OK );
    }

    check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );
}

#if ROCKETNET_DEV
// a token naming 127.0.0.1:40000, [::1]:40000, or both
static void loopback_hub_family_token( uint8_t * token, bool ipv4, bool ipv6 )
{
    struct rocketnet_signed_connect_data_t fields;
    memset( &fields, 0, sizeof( fields ) );
    fields.version = ROCKETNET_WIRE_VERSION;
    fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    fields.create_timestamp = 1;
    fields.expire_timestamp = 1ULL << 40;
    fields.timeout_seconds = 300;
    fields.address_flags = (uint8_t) ( ( ipv4 ? ROCKETNET_ADDRESS_FLAG_IPV4 : 0 ) | ( ipv6 ? ROCKETNET_ADDRESS_FLAG_IPV6 : 0 ) );
    fields.ipv4_address[0] = 127;
    fields.ipv4_address[3] = 1;
    fields.ipv4_address[4] = 0x40;
    fields.ipv4_address[5] = 0x9c;
    fields.ipv6_address[14] = 1;
    fields.ipv6_address[16] = 0x40;
    fields.ipv6_address[17] = 0x9c;
    memcpy( fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &fields, token );
}

static rocketnet_client_transport_t * loopback_hub_family_client( rocketnet_loopback_hub_t * hub, bool ipv4, bool ipv6, int * status )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    loopback_hub_family_token( token, ipv4, ipv6 );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.step_driven = 1;
    config.loopback_hub = hub;
    return rocketnet_client_transport_create_resolved( NULL, token, &config, 1000.0, 0, 2, status );
}

// loopback opens no socket and runs no family race (the client memory plan
// plans none): a token naming both families takes one hub endpoint, so a hub
// made for two clients holds two, whatever their tokens name

static void test_loopback_two_family_client_takes_one_endpoint()
{
    rocketnet_quiet( 1 );

    rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 2, NULL );
    check( hub != NULL );

    int first_status = -1;
    rocketnet_client_transport_t * first = loopback_hub_family_client( hub, true, true, &first_status );
    check( first != NULL && first_status == ROCKETNET_STATUS_OK );
    loopback_hub_check_live( hub, 1 );

    int second_status = -1;
    rocketnet_client_transport_t * second = loopback_hub_family_client( hub, true, false, &second_status );
    check( second != NULL && second_status == ROCKETNET_STATUS_OK );
    loopback_hub_check_live( hub, 2 );

    rn_test_client_transport_stop_and_destroy( second );
    rn_test_client_transport_stop_and_destroy( first );
    loopback_hub_check_live( hub, 0 );
    check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_loopback_two_family_client_takes_one_endpoint );
#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_loopback_hub_create_and_destroy );
REGISTER_UNIT_TEST( test_loopback_hub_refuses_out_of_range );
REGISTER_UNIT_TEST( test_loopback_hub_refused_block );
REGISTER_UNIT_TEST( test_loopback_hub_null_refused );
REGISTER_UNIT_TEST( test_loopback_hub_destroy_refused_while_registered );
REGISTER_UNIT_TEST( test_loopback_hub_mismatch_refused );

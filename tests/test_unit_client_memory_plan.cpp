/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Client memory plan accounts for transport socket, wake, I/O client block and
// worker thread allocations across non-step-driven and step-driven modes.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_allocator.h"
#include "rocketnet_memory.h"
#include "rocketnet_layers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

struct plan_counting_allocator_t
{
    uint64_t total_bytes;
    int live;
};

static void * plan_alloc( void * context, size_t bytes )
{
    plan_counting_allocator_t * state = (plan_counting_allocator_t *) context;
    if ( state )
    {
        state->total_bytes += bytes;
        state->live++;
    }

    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );

    return (void *) address;
}

static void plan_free( void * context, void * pointer )
{
    plan_counting_allocator_t * state = (plan_counting_allocator_t *) context;
    if ( state )
    {
        state->live--;
    }

    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    free( base );
}

static void plan_make_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

extern void rocketnet_client_transport_set_memory_plan_seam_for_test( bool enable );

static void test_client_memory_plan_non_step_driven()
{
    rocketnet_client_transport_set_memory_plan_seam_for_test( true );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    plan_make_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.step_driven = 0;

    const uint64_t plan_bytes = rocketnet_client_transport_required_memory( &config, "portable" );
    check( plan_bytes > 0 );

    plan_counting_allocator_t state = { 0, 0 };
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    int status = -1;
    check( rocketnet_init_ex( &state, plan_alloc, plan_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    // the plan covers create alone, so the count starts here

    state.total_bytes = 0;

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_ex(
        &state, token, &config, 1000.0, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );

    const uint64_t allocated = state.total_bytes;
    rn_test_client_transport_stop_and_destroy( client );

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    rocketnet_client_transport_set_memory_plan_seam_for_test( false );

    check( plan_bytes >= allocated );
}

#if ROCKETNET_DEV
static void test_client_memory_plan_step_driven()
{
    rocketnet_client_transport_set_memory_plan_seam_for_test( true );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    plan_make_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.step_driven = 1;

    const uint64_t plan_bytes = rocketnet_client_transport_required_memory( &config, "portable" );
    check( plan_bytes > 0 );

    plan_counting_allocator_t state = { 0, 0 };
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    int status = -1;
    check( rocketnet_init_ex( &state, plan_alloc, plan_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    // the plan covers create alone, so the count starts here

    state.total_bytes = 0;

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_ex(
        &state, token, &config, 1000.0, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );

    const uint64_t allocated = state.total_bytes;
    rn_test_client_transport_stop_and_destroy( client );

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    rocketnet_client_transport_set_memory_plan_seam_for_test( false );

    check( plan_bytes >= allocated );
}

// RNClientReceiveQuit StopWake: stop on the legacy path (here the path an
// active link simulator selects) wakes the receive thread after it stores
// quit, so the thread never depends on its wait timing out. The seam stands in
// for the thread: no thread runs and no socket opens.
extern uint64_t rocketnet_client_transport_legacy_wakes_for_test();

static void test_client_legacy_stop_wakes_the_receive_thread()
{
    rocketnet_client_transport_set_memory_plan_seam_for_test( true );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    plan_make_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.step_driven = 0;
    config.simulated_latency_ms = 50;

    int status = -1;
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_current_ex(
        NULL, token, &config, 1000.0, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );

    const uint64_t before = rocketnet_client_transport_legacy_wakes_for_test();
    rn_test_client_transport_stop_and_destroy( client );
    check( rocketnet_client_transport_legacy_wakes_for_test() == before + 1 );

    rocketnet_client_transport_set_memory_plan_seam_for_test( false );
}

REGISTER_UNIT_TEST( test_client_legacy_stop_wakes_the_receive_thread );
#endif // #if ROCKETNET_DEV

// The client plan answers bytes for "msgx" only where the registry offers it
// (macOS); on every other platform, iOS included, the name has no plan.

static void test_client_memory_plan_msgx_only_on_macos()
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );

    check( rocketnet_client_transport_required_memory( &config, "portable" ) > 0 );

    const uint64_t msgx_bytes = rocketnet_client_transport_required_memory( &config, "msgx" );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    check( msgx_bytes > 0 );
#else
    check( msgx_bytes == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
}

REGISTER_UNIT_TEST( test_client_memory_plan_non_step_driven );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_client_memory_plan_step_driven );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_client_memory_plan_msgx_only_on_macos );

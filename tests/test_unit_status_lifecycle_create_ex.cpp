/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The structured _ex lifecycle and creation surface
// (docs/ERRORS-AND-LOGGING.md 2.1 and 2.3, Layer 4): every failure exit that
// is reachable without a socket or real time writes its exact status code, and
// a failure of the allocation path is reached by a fault-injecting allocator
// failing at each allocation in turn.
//
// Not reachable in the unit tier:
//   * rocketnet_init_ex PLATFORM_INIT_FAILED and CRYPTO_INIT_FAILED, which need
//     the platform or sodium start to fail (no fault hook exists);
//   * rocketnet_frame_sync_create_ex F3 TIMER_CREATE_FAILED, which is the
//     Windows CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
//     refusal without coarse fallback (covered by the status-plumbing unit
//     check in test_unit_status_codes_reached.cpp and the native resource
//     functional cases in functional/platform_primitives.cpp);
//   * rocketnet_frame_sync_create_ex success, which creates the real Windows
//     high-resolution waitable timer (functional case
//     test_frame_sync_create_ex_success_status in functional/platform_primitives.cpp);
//   * the client and server transport create exits (C1-C11, S1-S14), which are
//     Layer 4 work owned by the transport cards, not this card's surface.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

struct allocator_state
{
    int live;
    int calls;
    int fail_at;
};

static void * allocate( void * context, size_t bytes )
{
    allocator_state * state = (allocator_state *) context;
    const int call = ++state->calls;
    if ( state->fail_at == call )
        return NULL;

    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;
    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memset( (void *) address, 0xA5, bytes );
    ++state->live;

    return (void *) address;
}

static void release( void * context, void * pointer )
{
    allocator_state * state = (allocator_state *) context;
    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    --state->live;
    free( base );
}

static void check_message_validate_status()
{
    int status = -1;
    struct rocketnet_message_config_t config;

    // a NULL config means the defaults, and the defaults are accepted

    check( rocketnet_message_validate_config_ex( NULL, &status ) == 1 );
    check( status == ROCKETNET_STATUS_OK );

    rocketnet_message_default_config( &config );
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 1 );
    check( status == ROCKETNET_STATUS_OK );

    // the table's range refusal

    rocketnet_message_default_config( &config );
    config.num_channels = 0;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // the window count is not a power of two

    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 96;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // ring placements are not multiples of 16

    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = 262145;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // a maximal message cannot fit the receive ring

    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = 5000;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // more fragments than the window holds

    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.max_message_bytes = 100000;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // the large store class cannot hold a maximal block

    rocketnet_message_default_config( &config );
    config.send_message_buffer_bytes = 12288;
    config.max_message_bytes = 100000;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // resend_delay_min above resend_delay_max

    rocketnet_message_default_config( &config );
    config.message_resend_delay_min_ms = 3000;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // resend_delay_initial outside the min/max clamp

    rocketnet_message_default_config( &config );
    config.message_resend_delay_min_ms = 200;
    config.message_resend_delay_initial_ms = 150;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // reliable_timeout too short for the resend budget

    rocketnet_message_default_config( &config );
    config.message_resend_delay_min_ms = 200;
    config.message_resend_delay_max_ms = 60000;
    config.message_resend_delay_initial_ms = 5000;
    config.reliable_message_timeout_ms = 10000;
    status = -1;
    check( rocketnet_message_validate_config_ex( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
}

static void test_lifecycle_ex_reports_every_status()
{
    allocator_state state = { 0, 0, 0 };
    int status = -1;

    // the worker starts the process initialized; return to the pre-init state
    // so the lifecycle refusals below are reachable

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_NOT_INITIALIZED );

    // init: success, then a second init is the re-entry refusal

    status = -1;
    check( rocketnet_init_ex( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    status = -1;
    check( !rocketnet_init_ex( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_ALREADY_INITIALIZED );

    // term with a live allocation is refused and leaves the library up

    void * live = rocketnet_malloc( NULL, 16 );
    check( live != NULL );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_LIVE_ALLOCATIONS );

    status = -1;
    check( !rocketnet_init_ex( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_ALREADY_INITIALIZED );

    rocketnet_free( NULL, live );

    // a clean term is OK

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    // allocator while uninitialized: a partial set is INVALID_ARGUMENT, a
    // replacement with outstanding allocations is LIVE_ALLOCATIONS, and a
    // clean replacement is OK

    check( rocketnet_allocator_ex( NULL, NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_allocator_ex( allocate, release ) == ROCKETNET_STATUS_OK );

    void * pre_init = rocketnet_malloc( &state, 16 );
    check( pre_init != NULL );
    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_LIVE_ALLOCATIONS );

    rocketnet_free( &state, pre_init );
    check( state.live == 0 );

    check_message_validate_status();

    // frame sync before init is refused as NOT_INITIALIZED

    status = -1;
    check( rocketnet_frame_sync_create_ex( &state, 60.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_NOT_INITIALIZED );

    // init with the fault-injecting allocator in context

    status = -1;
    check( rocketnet_init_ex( &state, allocate, release, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    // allocator after init is ALREADY_INITIALIZED

    check( rocketnet_allocator_ex( allocate, release ) == ROCKETNET_STATUS_ALREADY_INITIALIZED );

    // frame sync F1: zero, negative, above the maximum and NaN

    status = -1;
    check( rocketnet_frame_sync_create_ex( &state, 0.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    status = -1;
    check( rocketnet_frame_sync_create_ex( &state, -60.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    status = -1;
    check( rocketnet_frame_sync_create_ex( &state, 1000000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    status = -1;
    check( rocketnet_frame_sync_create_ex( &state, (double) NAN, &status ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // frame sync F2: the single allocation fails

    state.fail_at = state.calls + 1;
    status = -1;
    check( rocketnet_frame_sync_create_ex( &state, 60.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );

    state.fail_at = 0;
    check( state.live == 0 );

    // frame sync success creates the real Windows high-resolution waitable
    // timer, so it is the functional case test_frame_sync_create_ex_success_status
    // (functional/platform_primitives.cpp), with the same allocator and status
    // checks

    // a clean term is OK and leaves no allocation behind

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );

    // restore the default allocator for the worker's own term

    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_lifecycle_ex_reports_every_status );

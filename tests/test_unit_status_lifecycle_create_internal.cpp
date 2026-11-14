/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The structured _ex lifecycle surface (docs/ERRORS-AND-LOGGING.md 2.1 and
// 2.3, Layer 4) and message-config validation: every failure exit that is
// reachable without a socket or real time writes its exact status code. A
// counting allocator, installed and then passed to init, shows the allocator
// refusals (partial set, live allocations, already initialized) and that a
// clean term leaves nothing behind. Init has no allocation to fail: it
// allocates nothing through the context.
//
// rocketnet_init_internal CRYPTO_INIT_FAILED is reached in a development
// build through the crypto source failure seam (sdk/rocketnet_test_seams.h),
// which makes the open of sodium's random source fail
// (test_lifecycle_init_crypto_failure_reports_and_unwinds).
//
// Not reachable in the unit tier:
//   * rocketnet_init_internal PLATFORM_INIT_FAILED, which needs the platform
//     start to fail;
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
};

static void * allocate( void * context, size_t bytes )
{
    allocator_state * state = (allocator_state *) context;
    ++state->calls;

    void * base = malloc( bytes + 128 );
    if ( !base )
    {
        return NULL;
    }
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

    check( rocketnet_message_validate_config_internal( NULL, &status ) == 1 );
    check( status == ROCKETNET_STATUS_OK );

    rocketnet_message_default_config( &config );
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 1 );
    check( status == ROCKETNET_STATUS_OK );

    // the table's range refusal

    rocketnet_message_default_config( &config );
    config.num_channels = 0;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // the window count is not a power of two

    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 96;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // ring placements are not multiples of 16

    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = 262145;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // a maximal message cannot fit the receive ring

    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = 5000;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // more fragments than the window holds

    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.max_message_bytes = 100000;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // a small send store is enough for a maximal block: the block takes a
    // span of the send spans, not the store's large class

    rocketnet_message_default_config( &config );
    config.send_message_buffer_bytes = 12288;
    config.max_message_bytes = 100000;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 1 );
    check( status == ROCKETNET_STATUS_OK );

    // resend_delay_min above resend_delay_max

    rocketnet_message_default_config( &config );
    config.message_resend_delay_min_ms = 3000;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // resend_delay_initial outside the min/max clamp

    rocketnet_message_default_config( &config );
    config.message_resend_delay_min_ms = 200;
    config.message_resend_delay_initial_ms = 150;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // reliable_timeout too short for the resend budget

    rocketnet_message_default_config( &config );
    config.message_resend_delay_min_ms = 200;
    config.message_resend_delay_max_ms = 60000;
    config.message_resend_delay_initial_ms = 5000;
    config.reliable_message_timeout_ms = 10000;
    status = -1;
    check( rocketnet_message_validate_config_internal( &config, &status ) == 0 );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
}

static void test_lifecycle_ex_reports_every_status()
{
    allocator_state state = { 0, 0 };
    int status = -1;

    // the worker starts the process initialized; return to the pre-init state
    // so the lifecycle refusals below are reachable

    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_NOT_INITIALIZED );

    // init: success, then a second init is the re-entry refusal

    status = -1;
    check( rocketnet_init_internal( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    status = -1;
    check( !rocketnet_init_internal( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_ALREADY_INITIALIZED );

    // term with a live allocation is refused and leaves the library up

    void * live = rocketnet_malloc( NULL, 16 );
    check( live != NULL );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_LIVE_ALLOCATIONS );

    status = -1;
    check( !rocketnet_init_internal( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_ALREADY_INITIALIZED );

    rocketnet_free( NULL, live );

    // a clean term is OK

    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );

    // allocator while uninitialized: a partial set is INVALID_ARGUMENT, a
    // replacement with outstanding allocations is LIVE_ALLOCATIONS, and a
    // clean replacement is OK

    check( rocketnet_allocator_internal( NULL, NULL ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_allocator_internal( allocate, release ) == ROCKETNET_STATUS_OK );

    void * pre_init = rocketnet_malloc( &state, 16 );
    check( pre_init != NULL );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_LIVE_ALLOCATIONS );

    rocketnet_free( &state, pre_init );
    check( state.live == 0 );

    check_message_validate_status();

    // init with the counting allocator in context

    status = -1;
    check( rocketnet_init_internal( &state, allocate, release, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    // allocator after init is ALREADY_INITIALIZED

    check( rocketnet_allocator_internal( allocate, release ) == ROCKETNET_STATUS_ALREADY_INITIALIZED );

    // a clean term is OK and leaves no allocation behind

    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );

    // restore the default allocator for the worker's own term

    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_lifecycle_ex_reports_every_status );

#if ROCKETNET_DEV

#include "rocketnet_platform.h"
#include "rocketnet_test_seams.h"

// the platform's timer period calls, counted through to the table in effect:
// a platform init that begins the period and a term that ends it pair up
// (Windows; elsewhere neither call is made and both counts stay 0)

static const struct rocketnet_platform_priority_calls_t * crypto_failure_previous_calls = NULL;
static int crypto_failure_period_begins = 0;
static int crypto_failure_period_ends = 0;

static unsigned int crypto_failure_time_begin_period( unsigned int period )
{
    const unsigned int result = crypto_failure_previous_calls->time_begin_period( period );
    if ( result == 0 )
    {
        crypto_failure_period_begins++;
    }
    return result;
}

static unsigned int crypto_failure_time_end_period( unsigned int period )
{
    crypto_failure_period_ends++;
    return crypto_failure_previous_calls->time_end_period( period );
}

// init with the crypto source failing: false with CRYPTO_INIT_FAILED, the
// platform it started is terminated again (its period ended), the library
// is not initialized (term answers NOT_INITIALIZED), and once the seam is
// cleared the same init succeeds

static void test_lifecycle_init_crypto_failure_reports_and_unwinds()
{
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );

    crypto_failure_previous_calls = rocketnet_platform_priority_calls_current();
    struct rocketnet_platform_priority_calls_t counting = *crypto_failure_previous_calls;
    counting.time_begin_period = crypto_failure_time_begin_period;
    counting.time_end_period = crypto_failure_time_end_period;
    crypto_failure_period_begins = 0;
    crypto_failure_period_ends = 0;
    rocketnet_platform_set_priority_calls_for_testing( &counting );

    rocketnet_dev_set_crypto_source_failure( true );
    rocketnet_quiet( true );
    int status = -1;
    const bool initialized = rocketnet_init_internal( NULL, NULL, NULL, &status );
    rocketnet_quiet( false );
    rocketnet_dev_set_crypto_source_failure( false );

    rocketnet_platform_set_priority_calls_for_testing( crypto_failure_previous_calls );

    check( !initialized );
    check( status == ROCKETNET_STATUS_CRYPTO_INIT_FAILED );
    check( crypto_failure_period_ends == crypto_failure_period_begins );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_NOT_INITIALIZED );

    status = -1;
    check( rocketnet_init_internal( NULL, NULL, NULL, &status ) );
    check( status == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_lifecycle_init_crypto_failure_reports_and_unwinds );

#endif // #if ROCKETNET_DEV

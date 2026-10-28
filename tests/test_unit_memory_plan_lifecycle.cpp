/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The memory plan covers the whole lifecycle: rocketnet_server_required_memory
// and rocketnet_client_required_memory report the peak the caller's allocator
// sees over create, start or connect, stop or disconnect, the second start or
// connect, and destroy, and the second cycle takes no more than the first.
// A counting allocator observes every rocketnet_malloc; the transport is the
// in-memory loopback, the client is step-driven, and the unit build's stats
// worker is manual: no socket, no thread, no sleep, and the clock starts at
// 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "token_string.h"

#include <cstddef>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define MEMORY_PLAN_PROTOCOL 0x7A11C1C1E5EED0A1ULL

// ---------------------------------------------------------------------------
// The counting allocator: the bytes each request asks for, the bytes live and
// the highest live seen. rocketnet_init accepts it only on an uninitialized
// library with nothing live, so the worker's init is stopped first and
// restored after.
// ---------------------------------------------------------------------------

struct alignas( std::max_align_t ) memory_plan_allocation_t
{
    size_t bytes;
};

static uint64_t memory_plan_live = 0;
static uint64_t memory_plan_peak = 0;

static void * memory_plan_malloc( void * context, size_t bytes )
{
    (void) context;

    memory_plan_allocation_t * allocation = (memory_plan_allocation_t *) malloc( sizeof( memory_plan_allocation_t ) + bytes );
    if ( !allocation )
        return NULL;
    allocation->bytes = bytes;
    memory_plan_live += bytes;
    if ( memory_plan_live > memory_plan_peak )
    {
        memory_plan_peak = memory_plan_live;
    }

    return allocation + 1;
}

static void memory_plan_free( void * context, void * p )
{
    (void) context;
    if ( !p )
        return;

    memory_plan_allocation_t * allocation = (memory_plan_allocation_t *) p - 1;
    memory_plan_live -= allocation->bytes;
    free( allocation );
}

static double memory_plan_now = 1000.0;
#if ROCKETNET_DEV
static double memory_plan_clock( void * ) { return memory_plan_now; }
#endif // #if ROCKETNET_DEV

// the live bytes now, from which the next peak is measured

static uint64_t memory_plan_mark()
{
    memory_plan_peak = memory_plan_live;
    return memory_plan_live;
}

static uint64_t memory_plan_peak_above( uint64_t base )
{
    return memory_plan_peak - base;
}

static void test_memory_plan_covers_lifecycle()
{
    rocketnet_term();
    check( rocketnet_init( NULL, memory_plan_malloc, memory_plan_free ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( true );
    memory_plan_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    // the server: create, start, stop, stopped, start, stop, stopped, destroy

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = MEMORY_PLAN_PROTOCOL;
    server_config.max_clients = 4;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";

    const uint64_t server_required = rocketnet_server_required_memory( &server_config );
    check( server_required > 0 );

    const uint64_t server_base = memory_plan_mark();
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    rocketnet_server_update( server, memory_plan_now );
    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );

    const uint64_t server_first_peak = memory_plan_peak_above( server_base );

    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    rocketnet_server_update( server, memory_plan_now );
    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );

    const uint64_t server_second_peak = memory_plan_peak_above( server_base );

    if ( server_first_peak > server_required || server_second_peak != server_first_peak )
        printf( "server: required %llu, first cycle peak %llu, second cycle peak %llu\n", (unsigned long long) server_required,
                (unsigned long long) server_first_peak, (unsigned long long) server_second_peak );
    check( server_first_peak <= server_required );
    check( server_second_peak == server_first_peak );

    // the client half relies on the development-only caller-stepped fixture;
    // the production unit retains the independent server lifecycle oracle
#if ROCKETNET_DEV
    // the client: create, connect, disconnect, disconnected, connect,
    // disconnect, disconnected, destroy. The running server is the token's
    // target and is not updated, so it allocates nothing while the client
    // is measured.

    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );

    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, server, MEMORY_PLAN_PROTOCOL, addresses, 1, 0x4D454D, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = 1;
    client_config.time_callback = memory_plan_clock;

    const uint64_t client_required = rocketnet_client_required_memory( &client_config );
    check( client_required > 0 );

    const uint64_t client_base = memory_plan_mark();
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    rocketnet_client_update( client, memory_plan_now );
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );

    const uint64_t client_first_peak = memory_plan_peak_above( client_base );

    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    rocketnet_client_update( client, memory_plan_now );
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );

    const uint64_t client_second_peak = memory_plan_peak_above( client_base );

    if ( client_first_peak > client_required || client_second_peak != client_first_peak )
        printf( "client: required %llu, first cycle peak %llu, second cycle peak %llu\n", (unsigned long long) client_required,
                (unsigned long long) client_first_peak, (unsigned long long) client_second_peak );
    check( client_first_peak <= client_required );
    check( client_second_peak == client_first_peak );

    rocketnet_client_destroy( client );
    check( memory_plan_live == client_base );

    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );
    rocketnet_server_destroy( server );
    check( memory_plan_live == server_base );
#else
    rocketnet_server_destroy( server );
    check( memory_plan_live == server_base );
#endif // #if ROCKETNET_DEV

    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_memory_plan_covers_lifecycle );

#if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// The light sufficiency oracle: each light endpoint is created with a
// context that routes its allocator calls into one 64-byte-aligned block of
// exactly rocketnet_*_required_memory bytes, dirty-filled, with guarded
// canaries immediately outside it. The arena refuses any byte beyond the
// block, tracks every request and free, and reports its high-water and
// final live bytes. Allocations with any other context (library init and
// other fixtures) fall back to a counting system route, whose live bytes
// must not move while an endpoint lifecycle is measured: an endpoint
// allocation that escaped its arena would show there. The transport is
// the in-memory loopback and the client is step-driven: no socket, no
// thread, no sleep, and the clock is the memory_plan_now argument.
// ---------------------------------------------------------------------------

#define BOUNDED_MAX_SEGMENTS 16384
#define BOUNDED_GUARD_BYTES ( (size_t) 64 )
#define BOUNDED_GUARD_BYTE 0xC7
#define BOUNDED_DIRTY_BYTE 0xA5

struct bounded_segment_t
{
    size_t offset;
    size_t bytes;
    int used;
};

struct bounded_arena_t
{
    uint8_t * allocation;
    uint8_t * base;
    size_t bytes;
    size_t live;
    size_t peak;
    size_t refused;
    size_t last_refused_bytes;
    size_t invalid_frees;
    struct bounded_segment_t * segments;
    int num_segments;
};

static struct bounded_arena_t bounded_server_arena;
static struct bounded_arena_t bounded_client_arena;
static uint64_t bounded_system_live = 0;
static uint64_t bounded_system_peak = 0;

static void bounded_arena_setup( struct bounded_arena_t * arena, size_t bytes )
{
    if ( !arena->segments )
    {
        arena->segments = (struct bounded_segment_t *) calloc( BOUNDED_MAX_SEGMENTS, sizeof( struct bounded_segment_t ) );
    }
    check( arena->segments != NULL );
    arena->allocation = (uint8_t *) malloc( bytes + 2 * BOUNDED_GUARD_BYTES + ROCKETNET_ALLOCATION_ALIGNMENT );
    check( arena->allocation != NULL );

    const uintptr_t first = (uintptr_t) arena->allocation + BOUNDED_GUARD_BYTES;
    arena->base = (uint8_t *) ( ( first + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
    arena->bytes = bytes;
    arena->live = 0;
    arena->peak = 0;
    arena->refused = 0;
    arena->invalid_frees = 0;
    memset( arena->base - BOUNDED_GUARD_BYTES, BOUNDED_GUARD_BYTE, BOUNDED_GUARD_BYTES );
    memset( arena->base + bytes, BOUNDED_GUARD_BYTE, BOUNDED_GUARD_BYTES );
    memset( arena->base, BOUNDED_DIRTY_BYTE, bytes );
    arena->segments[0].offset = 0;
    arena->segments[0].bytes = bytes;
    arena->segments[0].used = 0;
    arena->num_segments = 1;
}

static int bounded_arena_canaries_ok( const struct bounded_arena_t * arena )
{
    for ( size_t i = 0; i < BOUNDED_GUARD_BYTES; i++ )
    {
        if ( arena->base[-(ptrdiff_t) BOUNDED_GUARD_BYTES + (ptrdiff_t) i] != BOUNDED_GUARD_BYTE )
            return 0;
        if ( arena->base[arena->bytes + i] != BOUNDED_GUARD_BYTE )
            return 0;
    }

    return 1;
}

static void * bounded_arena_allocate( struct bounded_arena_t * arena, size_t bytes )
{
    if ( bytes == 0 )
    {
        bytes = 1;
    }

    // small requests take the smallest fitting hole (best fit); large
    // requests (1 MiB and up) take the largest hole from its end, so the
    // block's big spans stay packed at the top and keep re-forming one
    // contiguous span when they are freed

    const int large = bytes >= (size_t) 1024 * 1024;
    int pick = -1;

    for ( int i = 0; i < arena->num_segments; i++ )
    {
        const struct bounded_segment_t * candidate = &arena->segments[i];
        if ( candidate->used || candidate->bytes < bytes )
            continue;
        if ( pick < 0 || ( large ? candidate->bytes > arena->segments[pick].bytes : candidate->bytes < arena->segments[pick].bytes ) )
        {
            pick = i;
        }
    }

    if ( pick < 0 )
    {
        arena->refused++;
        arena->last_refused_bytes = bytes;
        return NULL;
    }

    const int i = pick;
    struct bounded_segment_t * segment = &arena->segments[i];
    size_t offset = segment->offset;

    if ( segment->bytes == bytes )
    {
        segment->used = 1;
    }
    else if ( large )
    {
        if ( arena->num_segments >= BOUNDED_MAX_SEGMENTS )
        {
            arena->refused++;
            arena->last_refused_bytes = bytes;
            return NULL;
        }

        offset = segment->offset + segment->bytes - bytes;
        memmove( &arena->segments[i + 2], &arena->segments[i + 1], (size_t) ( arena->num_segments - i - 1 ) * sizeof( struct bounded_segment_t ) );
        arena->segments[i + 1].offset = offset;
        arena->segments[i + 1].bytes = bytes;
        arena->segments[i + 1].used = 1;
        arena->num_segments++;
        segment->bytes -= bytes;
    }
    else
    {
        if ( arena->num_segments >= BOUNDED_MAX_SEGMENTS )
        {
            arena->refused++;
            arena->last_refused_bytes = bytes;
            return NULL;
        }

        memmove( &arena->segments[i + 2], &arena->segments[i + 1], (size_t) ( arena->num_segments - i - 1 ) * sizeof( struct bounded_segment_t ) );
        arena->segments[i + 1].offset = offset + bytes;
        arena->segments[i + 1].bytes = segment->bytes - bytes;
        arena->segments[i + 1].used = 0;
        arena->num_segments++;
        segment->bytes = bytes;
        segment->used = 1;
    }

    arena->live += bytes;
    if ( arena->live > arena->peak )
    {
        arena->peak = arena->live;
    }

    return arena->base + offset;
}

static void bounded_arena_release( struct bounded_arena_t * arena, void * p )
{
    const size_t offset = (size_t) ( (uint8_t *) p - arena->base );

    for ( int i = 0; i < arena->num_segments; i++ )
    {
        struct bounded_segment_t * segment = &arena->segments[i];
        if ( !segment->used || segment->offset != offset )
            continue;
        segment->used = 0;
        arena->live -= segment->bytes;
        if ( i + 1 < arena->num_segments && !arena->segments[i + 1].used && segment->offset + segment->bytes == arena->segments[i + 1].offset )
        {
            segment->bytes += arena->segments[i + 1].bytes;
            memmove( &arena->segments[i + 1], &arena->segments[i + 2], (size_t) ( arena->num_segments - i - 2 ) * sizeof( struct bounded_segment_t ) );
            arena->num_segments--;
        }

        if ( i > 0 && !arena->segments[i - 1].used && arena->segments[i - 1].offset + arena->segments[i - 1].bytes == segment->offset )
        {
            arena->segments[i - 1].bytes += segment->bytes;
            memmove( &arena->segments[i], &arena->segments[i + 1], (size_t) ( arena->num_segments - i - 1 ) * sizeof( struct bounded_segment_t ) );
            arena->num_segments--;
        }

        return;
    }

    arena->invalid_frees++;
}

static void bounded_arena_teardown( struct bounded_arena_t * arena )
{
    check( bounded_arena_canaries_ok( arena ) );
    check( arena->live == 0 );
    free( arena->allocation );
    arena->allocation = NULL;
    arena->base = NULL;
    arena->bytes = 0;
    arena->num_segments = 0;
}

static int bounded_pointer_in_arena( const struct bounded_arena_t * arena, const void * p )
{
    return arena->base && p >= arena->base && p < arena->base + arena->bytes;
}

static void * bounded_malloc( void * context, size_t bytes )
{
    if ( context == &bounded_server_arena )
        return bounded_arena_allocate( &bounded_server_arena, bytes );
    if ( context == &bounded_client_arena )
        return bounded_arena_allocate( &bounded_client_arena, bytes );

    memory_plan_allocation_t * allocation = (memory_plan_allocation_t *) malloc( sizeof( memory_plan_allocation_t ) + bytes );
    if ( !allocation )
        return NULL;
    allocation->bytes = bytes;
    bounded_system_live += bytes;
    if ( bounded_system_live > bounded_system_peak )
    {
        bounded_system_peak = bounded_system_live;
    }

    return allocation + 1;
}

static void bounded_free( void * context, void * p )
{
    (void) context;
    if ( !p )
        return;

    if ( bounded_pointer_in_arena( &bounded_server_arena, p ) )
    {
        bounded_arena_release( &bounded_server_arena, p );
        return;
    }

    if ( bounded_pointer_in_arena( &bounded_client_arena, p ) )
    {
        bounded_arena_release( &bounded_client_arena, p );
        return;
    }

    memory_plan_allocation_t * allocation = (memory_plan_allocation_t *) p - 1;
    bounded_system_live -= allocation->bytes;
    free( allocation );
}

static void bounded_pump( rocketnet_server_t * server, rocketnet_client_t * client )
{
    rocketnet_client_update( client, memory_plan_now );
    rocketnet_client_send_packets( client );
    rocketnet_server_update( server, memory_plan_now );
    rocketnet_server_send_packets( server );
    memory_plan_now += 0.01;
}

static void bounded_send_client( rocketnet_client_t * client, uint8_t value )
{
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 2 );
    check( out != NULL );
    if ( !out )
        return;
    out[0] = 0x4C;
    out[1] = value;
    check( rocketnet_client_finish_message( client, 2 ) != 0 );
}

static void bounded_send_server( rocketnet_server_t * server, uint64_t index, uint8_t value )
{
    uint8_t * out = rocketnet_server_start_message( server, index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 2 );
    check( out != NULL );
    if ( !out )
        return;
    out[0] = 0x53;
    out[1] = value;
    check( rocketnet_server_finish_message( server, 2 ) != 0 );
}

// one connected cycle: connect, exchange reliable receipted messages both
// ways until both ends receive and report receipts, at the fake clock

static void bounded_exchange( rocketnet_server_t * server, rocketnet_client_t * client, const uint8_t * backend_secret, uint8_t tag )
{
    // a fresh token each cycle: a restarted server binds a new loopback port

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );

    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, server, MEMORY_PLAN_PROTOCOL, addresses, 1, 0x4D454D, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        bounded_pump( server, client );
    }
    if ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
        printf( "light bounded connect: client state %d, server arena live %zu peak %zu refused %zu, client arena live %zu peak %zu refused %zu\n",
                rocketnet_client_state( client ), bounded_server_arena.live, bounded_server_arena.peak, bounded_server_arena.refused,
                bounded_client_arena.live, bounded_client_arena.peak, bounded_client_arena.refused );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int index = rocketnet_client_index( client );
    check( index >= 0 );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( server, index ); i++ )
    {
        bounded_pump( server, client );
    }
    check( rocketnet_server_client_connected( server, index ) );

    for ( int i = 0; i < 4; i++ )
    {
        bounded_send_client( client, (uint8_t) ( tag + i ) );
        bounded_send_server( server, (uint64_t) index, (uint8_t) ( tag + i ) );
    }

    int at_server = 0;
    int at_client = 0;
    int receipts = 0;
    for ( int i = 0; i < 400 && ( at_server < 4 || at_client < 4 || receipts == 0 ); i++ )
    {
        bounded_pump( server, client );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            at_server += batch->num_messages;
        }
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            at_client += batch->num_messages;
        }

        struct rocketnet_receipt_t found[4];
        const int client_receipts = rocketnet_client_receipts( client, found, 4 );
        const int server_receipts = rocketnet_server_receipts( server, index, found, 4 );
        if ( client_receipts > 0 || server_receipts > 0 )
        {
            receipts = 1;
        }
    }

    check( at_server == 4 );
    check( at_client == 4 );
    check( receipts == 1 );
}

static void test_light_required_memory_sufficient_bounded()
{
    rocketnet_term();
    memory_plan_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_light_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = MEMORY_PLAN_PROTOCOL;
    server_config.max_clients = 4;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";

    const size_t server_required = rocketnet_server_required_memory( &server_config );
    check( server_required > 0 );

    rocketnet_client_test_config_t client_config;
    memset( &client_config, 0, sizeof( client_config ) );
    check( rocketnet_client_light_config( (rocketnet_client_config_t *) &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = 1;
    client_config.time_callback = memory_plan_clock;

    const size_t client_required = rocketnet_client_required_memory( &client_config );
    check( client_required > 0 );

    bounded_arena_setup( &bounded_server_arena, server_required );
    bounded_arena_setup( &bounded_client_arena, client_required );
    check( ( (uintptr_t) bounded_server_arena.base % ROCKETNET_ALLOCATION_ALIGNMENT ) == 0 );
    check( ( (uintptr_t) bounded_client_arena.base % ROCKETNET_ALLOCATION_ALIGNMENT ) == 0 );

    check( rocketnet_init( NULL, bounded_malloc, bounded_free ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( true );

    const uint64_t system_base = bounded_system_live;
    bounded_system_peak = system_base;

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    rocketnet_server_t * server = rocketnet_server_create( &bounded_server_arena, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    rocketnet_server_update( server, memory_plan_now );

    status = ROCKETNET_TEST_STATUS_UNSET;

    rocketnet_client_t * client = rocketnet_client_create( &bounded_client_arena, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );

    bounded_exchange( server, client, backend_secret, 0x10 );
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );
    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );

    const size_t server_first_peak = bounded_server_arena.peak;
    const size_t client_first_peak = bounded_client_arena.peak;

    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    rocketnet_server_update( server, memory_plan_now );
    bounded_exchange( server, client, backend_secret, 0x20 );
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );
    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );

    printf( "light bounded loopback: server required %zu peak %zu/%zu, client required %zu peak %zu/%zu, refused %zu/%zu\n",
            server_required, server_first_peak, bounded_server_arena.peak,
            client_required, client_first_peak, bounded_client_arena.peak,
            bounded_server_arena.refused, bounded_client_arena.refused );
    check( server_first_peak > 0 && server_first_peak <= server_required );
    check( bounded_server_arena.peak == server_first_peak );
    check( client_first_peak > 0 && client_first_peak <= client_required );
    check( bounded_client_arena.peak == client_first_peak );
    check( bounded_server_arena.refused == 0 && bounded_client_arena.refused == 0 );
    check( bounded_server_arena.invalid_frees == 0 && bounded_client_arena.invalid_frees == 0 );
    check( bounded_system_peak == system_base );
    check( bounded_system_live == system_base );

    bounded_arena_teardown( &bounded_client_arena );
    bounded_arena_teardown( &bounded_server_arena );

    // The max supported light server population (1000 clients) is covered
    // at its exact required size by the native transport_memory checker on
    // the planned transports (tools/check_memory.py light rows). Loopback
    // is an unplanned custom transport whose own queue memory the public
    // contract does not count, so no 1000-client loopback row is asserted
    // here; test_unit_light_profile.cpp keeps the 1000-client required
    // bytes themselves under their recorded upper bound.

    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_light_required_memory_sufficient_bounded );

#endif // #if ROCKETNET_DEV

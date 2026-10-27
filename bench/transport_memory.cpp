/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Pre-allocation plan and optional allocator-observed server footprint.
#include "../tests/native_shutdown.h"
#include "../tests/token_string.h"
#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include <errno.h>
#include <limits.h>
#if defined( _WIN32 )
#include <windows.h>
#include <psapi.h>
#pragma comment( lib, "psapi.lib" )
#else
#include <sys/resource.h>
#endif
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined( _WIN32 )
extern "C" char ** environ;
#endif

struct alignas( max_align_t ) allocation_t
{
    size_t bytes;
};
static std::atomic<size_t> live{ 0 }, peak{ 0 }, count{ 0 };
static void * allocate( void *, size_t bytes )
{
    auto * block = (allocation_t *) malloc( sizeof( allocation_t ) + bytes );
    if ( !block )
        return NULL;
    block->bytes = bytes;
    const size_t current = live.fetch_add( bytes ) + bytes;
    size_t previous = peak.load();
    while ( current > previous && !peak.compare_exchange_weak( previous, current ) )
    {
    }
    count.fetch_add( 1 );
    return block + 1;
}
static void release( void *, void * pointer )
{
    if ( !pointer )
        return;
    auto * block = (allocation_t *) pointer - 1;
    live.fetch_sub( block->bytes );
    count.fetch_sub( 1 );
    free( block );
}
static uint64_t process_peak_rss()
{
#if defined( _WIN32 )
    PROCESS_MEMORY_COUNTERS counters = {};
    if ( !GetProcessMemoryInfo( GetCurrentProcess(), &counters, sizeof( counters ) ) )
        return 0;
    return (uint64_t) counters.PeakWorkingSetSize;
#else
    struct rusage usage;
    if ( getrusage( RUSAGE_SELF, &usage ) != 0 )
        return 0;
#if defined( __APPLE__ )
    return (uint64_t) usage.ru_maxrss;
#else
    return (uint64_t) usage.ru_maxrss * 1024;
#endif
#endif
}
static int number( const char * text )
{
    char * end;
    errno = 0;
    long value = strtol( text, &end, 10 );
    if ( errno || !*text || *end || value < 0 || value > INT_MAX )
    {
        fprintf( stderr, "invalid nonnegative integer\n" );
        exit( 1 );
    }
    return (int) value;
}

// The public endpoint and transport each resolve environment settings. A pure
// inventory cannot promise their equality while the process environment may
// change between planning and start. Development server impairment overrides
// also allocate simulator storage omitted by the concrete transport plan.
// Reject both classes rather than presenting an incomplete high-water figure.
static bool server_environment_overridden()
{
#if defined( _WIN32 )
    char ** variables = _environ;
#else
    char ** variables = environ;
#endif
    for ( char ** entry = variables; entry && *entry; ++entry )
        if ( strncmp( *entry, "ROCKETNET_SERVER_", 17 ) == 0 ||
             strncmp( *entry, "ROCKETNET_DEV_SERVER_", 21 ) == 0 )
            return true;
    return false;
}
// The lifecycle rows: the public server and client through two cycles each,
// observed by the counting allocator from a base after init. The server is
// created, started, stopped, started again, stopped and destroyed; the client
// connects to it, disconnects, connects again, disconnects and is destroyed.
// required_memory must bound the first cycle's peak, and the second cycle's
// peak must equal the first: a restart or reconnect takes the same bytes.
static bool wait_until( bool ( *done )( void * ), void ( *step )( void * ), void * object )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !done( object ) )
    {
        if ( rocketnet_time() >= deadline )
            return false;
        if ( step )
            step( object );
        rocketnet_sleep( 0.001 );
    }
    return true;
}
static bool server_stopped( void * server ) { return rocketnet_server_stopped( (rocketnet_server_t *) server ); }
static bool client_disconnected( void * client ) { return rocketnet_client_disconnected( (rocketnet_client_t *) client ); }
static void client_step( void * client ) { rocketnet_client_update( (rocketnet_client_t *) client, rocketnet_time() ); }

static int lifecycle( const rocketnet_server_transport_config_t & transport_config, const char * transport,
                      const rocketnet_server_memory_plan_t & plan )
{
    if ( rocketnet_init( NULL, allocate, release ) != ROCKETNET_STATUS_OK )
        return 1;
    rocketnet_forbid_xdp_commit();
    uint8_t seed[32], backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES], backend_secret[64];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x6C ^ i );
    const uint64_t protocol_id = 0x7A11C1C1E5EED0B1ULL;
    rocketnet_server_config_t server_config;
    if ( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) != ROCKETNET_STATUS_OK ||
         rocketnet_server_default_config( &server_config ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_term();
        return 1;
    }
    server_config.protocol_id = protocol_id;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = transport;
    server_config.max_clients = transport_config.max_clients;
    server_config.max_fragmented_messages = transport_config.max_fragmented_messages;
    server_config.num_xdp_queues = transport_config.num_xdp_queues;
    const size_t server_required = rocketnet_server_required_memory( &server_config );
    const size_t base = live.load();
    peak.store( base );
    rocketnet_server_t * server = server_required ? rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) : NULL;
    if ( !server )
    {
        fprintf( stderr, "lifecycle server create refused\n" );
        rocketnet_term();
        return 1;
    }
    size_t server_peaks[2] = { 0, 0 };
    bool ok = true;
    for ( int cycle = 0; cycle < 2 && ok; cycle++ )
    {
        ok = rocketnet_server_start( server ) == ROCKETNET_STATUS_OK;
        if ( ok )
        {
            rocketnet_server_update( server, 1000.0 );
            rocketnet_server_stop( server );
            ok = wait_until( server_stopped, NULL, server );
        }
        server_peaks[cycle] = peak.load() - base;
    }
    printf( "{\"lifecycle\":\"server\",\"transport\":\"%s\",\"required_bytes\":%zu,\"first_cycle_peak_bytes\":%zu,"
            "\"second_cycle_peak_bytes\":%zu,\"allocator_plan_bytes\":%llu,\"os_bytes\":%llu,\"thread_stack_bytes\":%llu,"
            "\"umem_bytes\":%llu,\"mapped_ring_bytes\":%llu,\"worker_threads\":%d}\n",
            transport, server_required, server_peaks[0], server_peaks[1], (unsigned long long) plan.allocator_bytes,
            (unsigned long long) plan.os_bytes, (unsigned long long) plan.thread_stack_bytes, (unsigned long long) plan.umem_bytes,
            (unsigned long long) plan.mapped_ring_bytes, plan.worker_threads );
    // the client, against the running server: the platform's default client transport
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char address[64];
    rocketnet_client_config_t client_config;
    size_t client_required = 0, client_base = 0, client_peaks[2] = { 0, 0 };
    rocketnet_client_t * client = NULL;
    if ( ok )
        ok = rocketnet_server_start( server ) == ROCKETNET_STATUS_OK && rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK;
    if ( ok )
    {
        snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
        const char * addresses[] = { address };
        const uint64_t now = (uint64_t) time( NULL );
        ok = rn_test_server_token_string( backend_secret, server, protocol_id, addresses, 1, 0x4C4946, now, now + 300, 30, NULL, token ) == ROCKETNET_STATUS_OK;
        client_required = rocketnet_client_required_memory( &client_config );
        client_base = live.load();
        peak.store( client_base );
        client = ok && client_required ? rocketnet_client_create( NULL, &client_config, NULL ) : NULL;
        ok = client != NULL;
    }
    for ( int cycle = 0; cycle < 2 && ok; cycle++ )
    {
        ok = rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK;
        if ( ok )
        {
            rocketnet_client_update( client, rocketnet_time() );
            rocketnet_client_disconnect( client );
            ok = wait_until( client_disconnected, client_step, client );
        }
        client_peaks[cycle] = peak.load() - client_base;
    }
    printf( "{\"lifecycle\":\"client\",\"required_bytes\":%zu,\"first_cycle_peak_bytes\":%zu,\"second_cycle_peak_bytes\":%zu}\n",
            client_required, client_peaks[0], client_peaks[1] );
    if ( client )
        rocketnet_client_destroy( client );
    const bool client_freed = live.load() == client_base;
    if ( rocketnet_server_running( server ) )
    {
        rocketnet_server_stop( server );
        ok = wait_until( server_stopped, NULL, server ) && ok;
    }
    rocketnet_server_destroy( server );
    const bool server_freed = live.load() == base;
    rocketnet_term();
    if ( !ok )
    {
        fprintf( stderr, "lifecycle step refused\n" );
        return 1;
    }
    if ( !client_freed || !server_freed )
    {
        fprintf( stderr, "lifecycle leaked allocations\n" );
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The light sufficiency rows: each light endpoint is created with a context
// that routes its allocator calls into one 64-byte-aligned block of exactly
// rocketnet_*_required_memory bytes for the pinned transport, dirty-filled,
// with guarded canaries immediately outside it. The arena is thread-safe
// (transport workers allocate and free concurrently), refuses any byte
// beyond the block, tracks every request and free, and reports high-water
// and final live bytes. Allocations with any other context (library init)
// fall back to the counting allocator above; OS-owned memory (thread
// stacks, kernel rings, UMEM) stays classified by the plan, never counted
// as an allocator underestimate.
// ---------------------------------------------------------------------------

#define LIGHT_MAX_SEGMENTS 16384
#define LIGHT_GUARD_BYTES ((size_t) 64)
#define LIGHT_GUARD_BYTE 0xC7
#define LIGHT_DIRTY_BYTE 0xA5

struct light_segment_t
{
    size_t offset;
    size_t bytes;
    int used;
};
struct light_arena_t
{
    uint8_t * allocation = nullptr;
    uint8_t * base = nullptr;
    size_t bytes = 0;
    size_t live = 0;
    size_t peak = 0;
    size_t refused = 0;
    size_t invalid_frees = 0;
    light_segment_t * segments = nullptr;
    int num_segments = 0;
    std::mutex mutex;
};
static light_arena_t light_server_arena, light_client_arena;

static bool light_arena_setup( light_arena_t & arena, size_t bytes )
{
    if ( !arena.segments )
        arena.segments = (light_segment_t *) calloc( LIGHT_MAX_SEGMENTS, sizeof( light_segment_t ) );
    arena.allocation = (uint8_t *) malloc( bytes + 2 * LIGHT_GUARD_BYTES + ROCKETNET_ALLOCATION_ALIGNMENT );
    if ( !arena.segments || !arena.allocation )
        return false;
    const uintptr_t first = (uintptr_t) arena.allocation + LIGHT_GUARD_BYTES;
    arena.base = (uint8_t *) ( ( first + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
    arena.bytes = bytes;
    arena.live = arena.peak = arena.refused = arena.invalid_frees = 0;
    memset( arena.base - LIGHT_GUARD_BYTES, LIGHT_GUARD_BYTE, LIGHT_GUARD_BYTES );
    memset( arena.base + bytes, LIGHT_GUARD_BYTE, LIGHT_GUARD_BYTES );
    memset( arena.base, LIGHT_DIRTY_BYTE, bytes );
    arena.segments[0] = { 0, bytes, 0 };
    arena.num_segments = 1;
    return true;
}

static bool light_arena_canaries_ok( const light_arena_t & arena )
{
    for ( size_t i = 0; i < LIGHT_GUARD_BYTES; i++ )
        if ( arena.base[-(ptrdiff_t) LIGHT_GUARD_BYTES + (ptrdiff_t) i] != LIGHT_GUARD_BYTE || arena.base[arena.bytes + i] != LIGHT_GUARD_BYTE )
            return false;
    return true;
}

static void * light_arena_allocate( light_arena_t & arena, size_t bytes )
{
    if ( bytes == 0 )
        bytes = 1;
    std::lock_guard<std::mutex> lock( arena.mutex );
    const bool large = bytes >= (size_t) 1024 * 1024;
    int pick = -1;
    for ( int i = 0; i < arena.num_segments; i++ )
    {
        const light_segment_t & candidate = arena.segments[i];
        if ( candidate.used || candidate.bytes < bytes )
            continue;
        if ( pick < 0 || ( large ? candidate.bytes > arena.segments[pick].bytes : candidate.bytes < arena.segments[pick].bytes ) )
            pick = i;
    }
    if ( pick < 0 )
    {
        arena.refused++;
        return NULL;
    }
    light_segment_t & segment = arena.segments[pick];
    size_t offset = segment.offset;
    if ( segment.bytes == bytes )
        segment.used = 1;
    else if ( arena.num_segments >= LIGHT_MAX_SEGMENTS )
    {
        arena.refused++;
        return NULL;
    }
    else if ( large )
    {
        offset = segment.offset + segment.bytes - bytes;
        memmove( &arena.segments[pick + 2], &arena.segments[pick + 1], (size_t) ( arena.num_segments - pick - 1 ) * sizeof( light_segment_t ) );
        arena.segments[pick + 1] = { offset, bytes, 1 };
        arena.num_segments++;
        segment.bytes -= bytes;
    }
    else
    {
        memmove( &arena.segments[pick + 2], &arena.segments[pick + 1], (size_t) ( arena.num_segments - pick - 1 ) * sizeof( light_segment_t ) );
        arena.segments[pick + 1] = { offset + bytes, segment.bytes - bytes, 0 };
        arena.num_segments++;
        segment.bytes = bytes;
        segment.used = 1;
    }
    arena.live += bytes;
    if ( arena.live > arena.peak )
        arena.peak = arena.live;
    return arena.base + offset;
}

static void light_arena_release( light_arena_t & arena, void * pointer )
{
    std::lock_guard<std::mutex> lock( arena.mutex );
    const size_t offset = (size_t) ( (uint8_t *) pointer - arena.base );
    for ( int i = 0; i < arena.num_segments; i++ )
    {
        light_segment_t & segment = arena.segments[i];
        if ( !segment.used || segment.offset != offset )
            continue;
        segment.used = 0;
        arena.live -= segment.bytes;
        if ( i + 1 < arena.num_segments && !arena.segments[i + 1].used && segment.offset + segment.bytes == arena.segments[i + 1].offset )
        {
            segment.bytes += arena.segments[i + 1].bytes;
            memmove( &arena.segments[i + 1], &arena.segments[i + 2], (size_t) ( arena.num_segments - i - 2 ) * sizeof( light_segment_t ) );
            arena.num_segments--;
        }
        if ( i > 0 && !arena.segments[i - 1].used && arena.segments[i - 1].offset + arena.segments[i - 1].bytes == segment.offset )
        {
            arena.segments[i - 1].bytes += segment.bytes;
            memmove( &arena.segments[i], &arena.segments[i + 1], (size_t) ( arena.num_segments - i - 1 ) * sizeof( light_segment_t ) );
            arena.num_segments--;
        }
        return;
    }
    arena.invalid_frees++;
}

static bool light_in_arena( const light_arena_t & arena, const void * pointer )
{
    return arena.base && pointer >= arena.base && pointer < arena.base + arena.bytes;
}

static void * light_allocate( void * context, size_t bytes )
{
    if ( context == &light_server_arena )
        return light_arena_allocate( light_server_arena, bytes );
    if ( context == &light_client_arena )
        return light_arena_allocate( light_client_arena, bytes );
    return allocate( context, bytes );
}

static void light_release( void * context, void * pointer )
{
    if ( !pointer )
        return;
    if ( light_in_arena( light_server_arena, pointer ) )
    {
        light_arena_release( light_server_arena, pointer );
        return;
    }
    if ( light_in_arena( light_client_arena, pointer ) )
    {
        light_arena_release( light_client_arena, pointer );
        return;
    }
    release( context, pointer );
}

static void light_arena_teardown( light_arena_t & arena )
{
    free( arena.allocation );
    arena.allocation = nullptr;
    arena.base = nullptr;
    arena.bytes = 0;
    arena.num_segments = 0;
}

static bool light_pump_until( rocketnet_server_t * server, rocketnet_client_t * client, bool ( *done )( void * ), void * object )
{
    const double deadline = rocketnet_time() + 10.0;
    while ( !done( object ) )
    {
        if ( rocketnet_time() >= deadline )
            return false;
        rocketnet_client_update( client, rocketnet_time() );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, rocketnet_time() );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }
    return true;
}
static bool light_client_connected( void * client ) { return rocketnet_client_state( (rocketnet_client_t *) client ) == ROCKETNET_CLIENT_STATE_CONNECTED; }

// one connected light cycle's work: four reliable receipted messages each
// way, all received, with receipts reported on at least one end
static bool light_exchange( rocketnet_server_t * server, rocketnet_client_t * client, int index )
{
    for ( int i = 0; i < 4; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 2 );
        if ( !out )
            return false;
        out[0] = 0x4C;
        out[1] = (uint8_t) i;
        if ( !rocketnet_client_finish_message( client, 2 ) )
            return false;
        out = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 2 );
        if ( !out )
            return false;
        out[0] = 0x53;
        out[1] = (uint8_t) i;
        if ( !rocketnet_server_finish_message( server, 2 ) )
            return false;
    }
    int at_server = 0, at_client = 0;
    bool receipts = false;
    const double deadline = rocketnet_time() + 10.0;
    while ( ( at_server < 4 || at_client < 4 || !receipts ) && rocketnet_time() < deadline )
    {
        rocketnet_client_update( client, rocketnet_time() );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, rocketnet_time() );
        rocketnet_server_send_packets( server );
        const rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            at_server += batch->num_messages;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
            at_client += batch->num_messages;
        rocketnet_receipt_t found[4];
        if ( rocketnet_client_receipts( client, found, 4 ) > 0 || rocketnet_server_receipts( server, index, found, 4 ) > 0 )
            receipts = true;
        rocketnet_sleep( 0.001 );
    }
    return at_server == 4 && at_client == 4 && receipts;
}

static int light_lifecycle( const rocketnet_server_transport_config_t & transport_config, const char * transport,
                            const rocketnet_server_memory_plan_t & plan )
{
    if ( rocketnet_init( NULL, light_allocate, light_release ) != ROCKETNET_STATUS_OK )
        return 1;
    rocketnet_forbid_xdp_commit();
    uint8_t seed[32], backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES], backend_secret[64];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x6C ^ i );
    const uint64_t protocol_id = 0x7A11C1C1E5EED0B1ULL;
    rocketnet_server_config_t server_config;
    if ( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) != ROCKETNET_STATUS_OK ||
         rocketnet_server_light_config( &server_config ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_term();
        return 1;
    }
    server_config.protocol_id = protocol_id;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = transport;
    server_config.max_clients = transport_config.max_clients;
    const size_t server_required = rocketnet_server_required_memory( &server_config );

    rocketnet_client_config_t client_config;
    if ( rocketnet_client_light_config( &client_config ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_term();
        return 1;
    }
    client_config.transport = transport;
    const size_t client_required = rocketnet_client_required_memory( &client_config );

    if ( !server_required || !client_required ||
         !light_arena_setup( light_server_arena, server_required ) ||
         !light_arena_setup( light_client_arena, client_required ) )
    {
        fprintf( stderr, "light lifecycle arena setup refused\n" );
        rocketnet_term();
        return 1;
    }

    rocketnet_server_t * server = rocketnet_server_create( &light_server_arena, "127.0.0.1:0", NULL, &server_config, NULL );
    rocketnet_client_t * client = NULL;
    bool ok = server != NULL;
    size_t server_peaks[2] = { 0, 0 }, client_peaks[2] = { 0, 0 };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    for ( int cycle = 0; cycle < 2 && ok; cycle++ )
    {
        ok = rocketnet_server_start( server ) == ROCKETNET_STATUS_OK;
        if ( ok && cycle == 0 )
        {
            client = rocketnet_client_create( &light_client_arena, &client_config, NULL );
            ok = client != NULL;
        }
        if ( ok )
        {
            // a fresh token each cycle: a restarted server may bind a new port
            char address[64];
            snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
            const char * addresses[] = { address };
            const uint64_t now = (uint64_t) time( NULL );
            ok = rn_test_server_token_string( backend_secret, server, protocol_id, addresses, 1, 0x4C4946, now, now + 300, 30, NULL, token ) == ROCKETNET_STATUS_OK;
        }
        if ( ok )
            ok = rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK && light_pump_until( server, client, light_client_connected, client );
        if ( ok )
        {
            const int index = rocketnet_client_index( client );
            ok = index >= 0 && rocketnet_server_client_connected( server, index ) && light_exchange( server, client, index );
        }
        if ( ok )
        {
            rocketnet_client_disconnect( client );
            ok = wait_until( client_disconnected, client_step, client );
        }
        if ( ok )
        {
            rocketnet_server_stop( server );
            ok = wait_until( server_stopped, NULL, server );
        }
        server_peaks[cycle] = light_server_arena.peak;
        client_peaks[cycle] = light_client_arena.peak;
    }
    if ( client )
        rocketnet_client_destroy( client );
    if ( server )
        rocketnet_server_destroy( server );
    const bool server_freed = light_server_arena.live == 0, client_freed = light_client_arena.live == 0;
    const bool canaries_ok = light_arena_canaries_ok( light_server_arena ) && light_arena_canaries_ok( light_client_arena );
    printf( "{\"lifecycle\":\"server\",\"profile\":\"light\",\"transport\":\"%s\",\"required_bytes\":%zu,\"first_cycle_peak_bytes\":%zu,"
            "\"second_cycle_peak_bytes\":%zu,\"arena_capacity_bytes\":%zu,\"final_live_bytes\":%zu,\"refused_allocations\":%zu,\"invalid_frees\":%zu,"
            "\"canaries_ok\":%s,\"allocator_plan_bytes\":%llu,\"os_bytes\":%llu,\"thread_stack_bytes\":%llu,"
            "\"umem_bytes\":%llu,\"mapped_ring_bytes\":%llu,\"worker_threads\":%d}\n",
            transport, server_required, server_peaks[0], server_peaks[1], light_server_arena.bytes, light_server_arena.live,
            light_server_arena.refused, light_server_arena.invalid_frees, light_arena_canaries_ok( light_server_arena ) ? "true" : "false",
            (unsigned long long) plan.allocator_bytes, (unsigned long long) plan.os_bytes, (unsigned long long) plan.thread_stack_bytes,
            (unsigned long long) plan.umem_bytes, (unsigned long long) plan.mapped_ring_bytes, plan.worker_threads );
    printf( "{\"lifecycle\":\"client\",\"profile\":\"light\",\"required_bytes\":%zu,\"first_cycle_peak_bytes\":%zu,\"second_cycle_peak_bytes\":%zu,"
            "\"arena_capacity_bytes\":%zu,\"final_live_bytes\":%zu,\"refused_allocations\":%zu,\"invalid_frees\":%zu,\"canaries_ok\":%s}\n",
            client_required, client_peaks[0], client_peaks[1], light_client_arena.bytes, light_client_arena.live,
            light_client_arena.refused, light_client_arena.invalid_frees, light_arena_canaries_ok( light_client_arena ) ? "true" : "false" );
    rocketnet_term();
    const bool fallback_freed = live.load() == 0 && count.load() == 0;
    light_arena_teardown( light_client_arena );
    light_arena_teardown( light_server_arena );
    if ( !ok )
    {
        fprintf( stderr, "light lifecycle step refused\n" );
        return 1;
    }
    if ( !server_freed || !client_freed || !canaries_ok || !fallback_freed )
    {
        fprintf( stderr, "light lifecycle leaked or damaged allocations\n" );
        return 1;
    }
    return 0;
}

int main( int argc, char ** argv )
{
    if ( argc < 4 || argc > 7 )
    {
        fprintf( stderr, "usage: transport_memory clients payload_bytes reassembly_slots [transport [queues [--plan-only|--session-plan-only|--lifecycle|--light-lifecycle]]]\n" );
        return 1;
    }
    const char * transport = argc >= 5 ? argv[4] : "portable";
    const bool plan_only = argc == 7 && strcmp( argv[6], "--plan-only" ) == 0;
    const bool session_plan_only = argc == 7 && strcmp( argv[6], "--session-plan-only" ) == 0;
    const bool lifecycle_only = argc == 7 && strcmp( argv[6], "--lifecycle" ) == 0;
    const bool light_lifecycle_only = argc == 7 && strcmp( argv[6], "--light-lifecycle" ) == 0;
    if ( argc == 7 && !plan_only && !session_plan_only && !lifecycle_only && !light_lifecycle_only )
        return 1;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    if ( strlen( transport ) >= sizeof( config.transport ) )
        return 1;
    snprintf( config.transport, sizeof( config.transport ), "%s", transport );
    config.max_clients = number( argv[1] );
    config.max_fragmented_messages = number( argv[3] );
    config.num_xdp_queues = argc >= 6 ? number( argv[5] ) : 0;
    rocketnet_server_memory_plan_t plan;
    if ( !rocketnet_server_transport_memory_plan( &config, transport, plan ) )
    {
        fprintf( stderr, "memory plan unavailable for this geometry/transport/platform\n" );
        return 1;
    }
    if ( session_plan_only )
    {
        if ( server_environment_overridden() )
        {
            fprintf( stderr, "session inventory requires no ROCKETNET_SERVER_ or ROCKETNET_DEV_SERVER_ environment overrides\n" );
            return 1;
        }
        rocketnet_server_config_t endpoint;
        if ( rocketnet_server_default_config( &endpoint ) != ROCKETNET_STATUS_OK )
            return 1;
        endpoint.transport = transport;
        endpoint.max_clients = config.max_clients;
        endpoint.max_fragmented_messages = config.max_fragmented_messages;
        endpoint.num_xdp_queues = config.num_xdp_queues;
        const size_t endpoint_payload = rocketnet_server_required_memory( &endpoint );
        rocketnet_host_memory_estimate_t host = {};
        uint64_t endpoint_callback_bytes = endpoint_payload;
        uint64_t known_active_requests = endpoint_payload;
        if ( !endpoint_payload || !rocketnet_server_transport_host_memory_estimate( &config, transport, &host ) ||
             !rocketnet_memory_add( endpoint_callback_bytes, 1, rocketnet_allocation_overhead() ) ||
             !rocketnet_memory_add( known_active_requests, 1, rocketnet_allocation_overhead() ) ||
             !rocketnet_memory_add( known_active_requests, 1, host.total_bytes ) )
        {
            fprintf( stderr, "session inventory unavailable for this geometry/transport/platform\n" );
            return 1;
        }
        // Public start refuses a running/stopping generation. Once stopped,
        // it destroys the retained generation before constructing the next.
        // The active/stopping high-water therefore owns one candidate's SDK
        // storage, but native retained-on-cancellation bytes remain unknown.
        printf( "{\"scope\":\"one_public_server_one_pinned_transport\",\"transport\":\"%s\","
                "\"create_payload_bytes\":%llu,\"create_callback_request_bytes\":%llu,"
                "\"active_transport_sdk_callback_bytes\":%llu,\"active_xdp_umem_bytes\":%llu,"
                "\"active_rio_registered_region_bytes\":%llu,\"active_socket_send_request_bytes\":%llu,"
                "\"active_socket_receive_request_bytes\":%llu,\"active_xdp_ring_request_bytes\":%llu,"
                "\"active_thread_stack_reserve_bytes\":%llu,\"known_active_or_stopping_requests_bytes\":%llu,"
                "\"concurrent_sdk_generations\":1,\"native_retained_on_failed_cancel_bytes\":null,"
                "\"io_uring_mapping_bytes\":null,\"libc_and_kernel_accounting_bytes\":null,"
                "\"config_copy_frozen_for_future_start\":false,\"one_block_sufficient\":false}\n",
                transport, (unsigned long long) endpoint_payload, (unsigned long long) endpoint_callback_bytes,
                (unsigned long long) host.allocator_bytes, (unsigned long long) host.umem_bytes,
                (unsigned long long) host.registered_region_bytes,
                (unsigned long long) host.socket_send_buffer_bytes, (unsigned long long) host.socket_receive_buffer_bytes,
                (unsigned long long) host.xdp_ring_bytes, (unsigned long long) host.thread_stack_bytes,
                (unsigned long long) known_active_requests );
        return 0;
    }
    printf( "{\"transport\":\"%s\",\"clients\":%d,\"payload_bytes\":%d,\"reassembly_slots\":%d,"
            "\"planned_bytes\":%llu,\"transport_bytes\":%llu,\"protocol_bytes\":%llu,\"reassembly_bytes\":%llu,"
            "\"send_arena_bytes\":%llu,\"transport_allocator_bytes\":%llu,\"umem_bytes\":%llu,"
            "\"worker_threads\":%d,\"sockets\":%d,\"allocator_bytes\":%llu,\"os_bytes\":%llu,"
            "\"thread_stack_bytes\":%llu,\"mapped_ring_bytes\":%llu,\"os_and_application_bytes\":null}\n",
            transport, config.max_clients, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES, config.max_fragmented_messages,
            (unsigned long long) plan.total_bytes, (unsigned long long) plan.transport_bytes,
            (unsigned long long) plan.protocol_bytes, (unsigned long long) plan.reassembly_bytes,
            (unsigned long long) plan.send_arena_bytes, (unsigned long long) plan.transport_bytes,
            (unsigned long long) plan.umem_bytes, plan.worker_threads, plan.sockets,
            (unsigned long long) plan.allocator_bytes, (unsigned long long) plan.os_bytes,
            (unsigned long long) plan.thread_stack_bytes, (unsigned long long) plan.mapped_ring_bytes );
    if ( plan_only )
        return 0; // No init, sockets, worker threads or UMEM registration.
    if ( lifecycle_only )
        return lifecycle( config, transport, plan );
    if ( light_lifecycle_only )
        return light_lifecycle( config, transport, plan );
    if ( rocketnet_init( NULL, allocate, release ) != ROCKETNET_STATUS_OK )
        return 1;
    rocketnet_forbid_xdp_commit();
    auto * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config );
    if ( !server )
    {
        fprintf( stderr, "memory profile configuration refused\n" );
        rocketnet_term();
        return 1;
    }
    const bool same_transport = strcmp( rocketnet_server_transport_io_name( server ), transport ) == 0;
    printf( "{\"live_allocator_bytes\":%zu,\"peak_allocator_bytes\":%zu,\"live_allocations\":%zu,\"process_peak_rss_bytes\":%llu}\n",
            live.load(), peak.load(), count.load(), (unsigned long long) process_peak_rss() );
    const uint64_t planned_allocator = plan.total_bytes - plan.umem_bytes;
    // Worker shims may have been freed before the constructor returns; all
    // other planned SDK heap storage stays live. The observer must agree.
    const bool matches = live.load() <= planned_allocator && peak.load() <= planned_allocator &&
                         planned_allocator - live.load() <= (uint64_t) plan.worker_threads * rocketnet_platform_thread_allocation_bytes();
    rn_native_server_transport_stop_and_destroy( server );
    rocketnet_term();
    if ( !same_transport || !matches )
    {
        fprintf( stderr, "allocator observation differs from the plan; check ambient config overrides\n" );
        return 1;
    }
    if ( live.load() || count.load() )
    {
        fprintf( stderr, "memory profile leaked allocations\n" );
        return 1;
    }
}

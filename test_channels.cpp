/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The message layer's tests (docs/CHANNELS.md), one process per case under
    the shared suite runner. Layer 1 first: the per-connection region.

    test_channels_allocator_no_live_allocation is the card's gate
    (ChannelsAllocatorNoLiveAllocation): it wraps the platform allocation
    hooks -- rocketnet_allocator, the global operator new and delete, and on
    glibc without a sanitizer libc malloc and free themselves -- and fails
    the moment any of them runs while a soak drives every carve-out of every
    region at full configured load. A witness inside the case proves each
    trap fires before the soak is trusted.
*/

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "suite_runner.h"

#include <atomic>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define check( condition ) do { if ( !( condition ) ) { channels_check_failed( #condition, __FILE__, __LINE__ ); } } while ( 0 )

// ----------------------------------------------------------------
//                       the allocation traps
// ----------------------------------------------------------------

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define CHANNELS_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CHANNELS_SANITIZED 1
#endif

// libc's own malloc and free are trapped on glibc when no sanitizer owns
// them: the definitions below interpose the process's malloc family and
// forward to glibc's internal entry points, so every allocation anywhere
// in the process -- the library, libstdc++, libc itself -- passes the trap.
#if defined(__GLIBC__) && !defined(CHANNELS_SANITIZED)
#define CHANNELS_TRAP_LIBC 1
extern "C" void * __libc_malloc( size_t bytes );
extern "C" void __libc_free( void * p );
extern "C" void * __libc_calloc( size_t count, size_t bytes );
extern "C" void * __libc_realloc( void * p, size_t bytes );
#endif

static std::atomic<int> channels_armed( 0 );
static std::atomic<int> channels_witness( 0 );
static std::atomic<int> channels_witnessed( 0 );
static const char * volatile channels_phase = "none";

static void channels_live_allocation( const char * what, size_t bytes )
{
    if ( channels_witness.load() )
    {
        channels_witnessed++;
        return;
    }
    channels_armed = 0;
    fprintf( stderr, "live allocation: %s (%d bytes) during %s\n", what, (int) bytes, channels_phase );
    fflush( stderr );
    _Exit( 1 );
}

static void * channels_raw_malloc( size_t bytes )
{
#if CHANNELS_TRAP_LIBC
    return __libc_malloc( bytes );
#else
    return malloc( bytes );
#endif
}

static void channels_raw_free( void * p )
{
#if CHANNELS_TRAP_LIBC
    __libc_free( p );
#else
    free( p );
#endif
}

#if CHANNELS_TRAP_LIBC

extern "C" void * malloc( size_t bytes ) noexcept
{
    if ( channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "malloc", bytes );
    return __libc_malloc( bytes );
}

extern "C" void free( void * p ) noexcept
{
    if ( p && channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "free", 0 );
    __libc_free( p );
}

extern "C" void * calloc( size_t count, size_t bytes ) noexcept
{
    if ( channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "calloc", count * bytes );
    return __libc_calloc( count, bytes );
}

extern "C" void * realloc( void * p, size_t bytes ) noexcept
{
    if ( channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "realloc", bytes );
    return __libc_realloc( p, bytes );
}

#endif // #if CHANNELS_TRAP_LIBC

void * operator new( size_t bytes )
{
    if ( channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "operator new", bytes );
    void * p = channels_raw_malloc( bytes ? bytes : 1 );
    if ( !p )
        throw std::bad_alloc();
    return p;
}

void * operator new[]( size_t bytes )
{
    if ( channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "operator new[]", bytes );
    void * p = channels_raw_malloc( bytes ? bytes : 1 );
    if ( !p )
        throw std::bad_alloc();
    return p;
}

void operator delete( void * p ) noexcept
{
    if ( p && channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "operator delete", 0 );
    channels_raw_free( p );
}

void operator delete[]( void * p ) noexcept
{
    if ( p && channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "operator delete[]", 0 );
    channels_raw_free( p );
}

void operator delete( void * p, size_t ) noexcept { operator delete( p ); }
void operator delete[]( void * p, size_t ) noexcept { operator delete[]( p ); }

// the platform allocation hooks, installed before init
static std::atomic<int> channels_hook_mallocs( 0 );
static std::atomic<int> channels_hook_frees( 0 );

static void * channels_hook_malloc( void * context, size_t bytes )
{
    (void) context;
    if ( channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "rocketnet_malloc", bytes );
    channels_hook_mallocs++;
    return channels_raw_malloc( bytes );
}

static void channels_hook_free( void * context, void * p )
{
    (void) context;
    if ( channels_armed.load( std::memory_order_relaxed ) )
        channels_live_allocation( "rocketnet_free", 0 );
    channels_hook_frees++;
    channels_raw_free( p );
}

// ----------------------------------------------------------------
//                     the log sink and the checks
// ----------------------------------------------------------------

// passing runs print the case name and nothing else: the library's log
// lands here, and the last line is printed only beside a failed check

static char channels_last_log[1024];
static int channels_log_lines[ROCKETNET_LOG_LEVEL_SPAM + 1];

static void channels_log_sink( int level, const char * format, ... )
{
    va_list args;
    va_start( args, format );
    vsnprintf( channels_last_log, sizeof(channels_last_log), format, args );
    va_end( args );
    if ( level >= 0 && level <= ROCKETNET_LOG_LEVEL_SPAM )
        channels_log_lines[level]++;
}

static void channels_check_failed( const char * condition, const char * file, int line )
{
    channels_armed = 0;
    if ( channels_last_log[0] )
        fprintf( stderr, "last log line: %s\n", channels_last_log );
    fprintf( stderr, "channels check failed: ( %s ), file %s, line %d\n", condition, file, line );
    fflush( stderr );
    exit( 1 );
}

static uint64_t channels_random_state = 0x9E3779B97F4A7C15ULL;

static uint64_t channels_random()
{
    uint64_t x = channels_random_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    channels_random_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static int channels_random_int( int a, int b )
{
    return a + (int) ( channels_random() % (uint64_t) ( b - a + 1 ) );
}

static int channels_store_class( uint32_t chunk )
{
    return (int) ( chunk >> 30 );
}

static void channels_min_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->num_channels = 1;
    config->window_messages = 64;
    config->receive_ring_bytes = 4096;
    config->max_message_bytes = 1191;
    config->send_buffer_bytes = 12288;
    config->send_queue_messages = 16;
    config->packets_in_flight = 16;
    config->unreliable_deliveries = 1;
    config->sequenced_holds = 0;
    config->receipt_ring = 16;
}

static void channels_max_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->num_channels = ROCKETNET_MAX_CHANNELS;
    config->window_messages = 1 << 20;
    config->receive_ring_bytes = 1 << 20;
    config->max_message_bytes = 1 << 20;
    config->send_buffer_bytes = 1 << 28;
    config->send_queue_messages = 1 << 20;
    config->packets_in_flight = 1 << 16;
    config->unreliable_deliveries = 1 << 16;
    config->sequenced_holds = 4096;
    config->receipt_ring = 1 << 16;
}

static void channels_lean_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->window_messages = 256;
    config->receive_ring_bytes = 32768;
    config->send_buffer_bytes = 65536;
    config->send_queue_messages = 256;
    config->packets_in_flight = 64;
    config->unreliable_deliveries = 128;

    // the design's lean profile kept the default max_message_bytes, 32768,
    // which needs 29 block chunks; a 65536-byte store has 18. Refused by
    // section 14's own rule, so the lean profile caps messages at 16384.
    config->max_message_bytes = 16384;
}

// every part 64-aligned, in carve order, disjoint, inside the region, and
// sized exactly as its entries
static void channels_check_layout( const rn_region_layout_t & layout )
{
    uint64_t end = 0;
    for ( int i = 0; i < RN_REGION_NUM_PARTS; i++ )
    {
        check( layout.part_offset[i] % RN_REGION_ALIGNMENT == 0 );
        check( layout.part_offset[i] >= end );
        end = layout.part_offset[i] + layout.part_bytes[i];
        check( end <= layout.region_bytes );
    }
    check( layout.region_bytes % RN_REGION_ALIGNMENT == 0 );
    check( layout.region_bytes - end < RN_REGION_ALIGNMENT );

    const uint64_t C = (uint64_t) layout.num_channels, W = (uint64_t) layout.window_messages;
    const uint64_t P = (uint64_t) layout.packets_in_flight;
    check( layout.part_bytes[RN_REGION_CHANNEL_SEND] == C * sizeof(rn_channel_send_t) );
    check( layout.part_bytes[RN_REGION_CHANNEL_RECEIVE] == C * sizeof(rn_channel_receive_t) );
    check( layout.part_bytes[RN_REGION_SEND_NUMBERS] == W * sizeof(rn_send_number_t) );
    check( layout.part_bytes[RN_REGION_RECEIVE_DESCRIPTORS] == W * sizeof(rn_receive_descriptor_t) );
    check( layout.part_bytes[RN_REGION_RECEIVED_BITS] == W / 8 );
    check( layout.part_bytes[RN_REGION_HELD_INDEX] == 2 * W * sizeof(rn_held_entry_t) );
    check( layout.part_bytes[RN_REGION_RECEIVE_RING] == (uint64_t) layout.receive_ring_bytes );
    check( layout.part_bytes[RN_REGION_SEND_DESCRIPTORS] == (uint64_t) layout.send_queue_messages * sizeof(rn_send_descriptor_t) );
    check( layout.part_bytes[RN_REGION_SEND_STORE] == (uint64_t) layout.send_buffer_bytes );
    check( layout.part_bytes[RN_REGION_SENT_PACKETS] == P * sizeof(rn_sent_packet_t) );
    check( layout.part_bytes[RN_REGION_SENT_INDEX] == 2 * P * sizeof(rn_sent_index_entry_t) );
    check( layout.part_bytes[RN_REGION_SENT_REFS] == 16 * P * sizeof(uint32_t) );
    check( layout.part_bytes[RN_REGION_DELIVERIES] == ( W + (uint64_t) layout.unreliable_deliveries ) * sizeof(rn_delivery_t) );
    check( layout.part_bytes[RN_REGION_SEQUENCED_HOLDS] == (uint64_t) layout.sequenced_holds * sizeof(rn_sequenced_hold_t) );
    check( layout.part_bytes[RN_REGION_RECEIPTS] == (uint64_t) layout.receipt_ring * sizeof(rn_receipt_t) );

    // the formula of section 14, term by term
    const uint64_t formula = 48 * C + 96 * W + W / 8 + (uint64_t) layout.receive_ring_bytes
                           + 64 * (uint64_t) layout.send_queue_messages + (uint64_t) layout.send_buffer_bytes + 128 * P
                           + 24 * ( W + (uint64_t) layout.unreliable_deliveries ) + 1208 * (uint64_t) layout.sequenced_holds
                           + 16 * (uint64_t) layout.receipt_ring;
    check( layout.formula_bytes == formula );
    check( layout.region_bytes >= layout.formula_bytes );

    // the store: three classes, each inside its third, the 1152 class's
    // links before its chunks, every chunk 64-aligned
    const uint64_t third = ( (uint64_t) layout.send_buffer_bytes / 3 ) & ~(uint64_t) ( RN_REGION_ALIGNMENT - 1 );
    check( layout.store_chunk_bytes[0] == 128 );
    check( layout.store_chunk_bytes[1] == 512 );
    check( layout.store_chunk_bytes[2] == ROCKETNET_MESSAGE_FRAGMENT_BYTES );
    for ( int c = 0; c < RN_SEND_STORE_NUM_CLASSES; c++ )
    {
        check( layout.store_chunks[c] >= 1 );
        check( layout.store_class_offset[c] % RN_REGION_ALIGNMENT == 0 );
        check( layout.store_class_offset[c] >= (uint64_t) c * third );
        check( layout.store_class_offset[c] + (uint64_t) layout.store_chunks[c] * (uint64_t) layout.store_chunk_bytes[c] <= (uint64_t) ( c + 1 ) * third );
    }
    check( layout.store_chunks[0] == (int) ( third / 128 ) );
    check( layout.store_chunks[1] == (int) ( third / 512 ) );
    check( layout.store_links_offset == 2 * third );
    check( layout.store_links_offset + 4 * (uint64_t) layout.store_chunks[2] <= layout.store_class_offset[2] );
    check( layout.store_chunks[2] * (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES >= layout.max_message_bytes );
}

// ----------------------------------------------------------------
//                              the cases
// ----------------------------------------------------------------

static void test_channels_allocator_layout()
{
    // the sizes section 14 works from, pinned
    check( sizeof(rn_channel_send_t) + sizeof(rn_channel_receive_t) == 48 );
    check( sizeof(rn_send_number_t) == 32 );
    check( sizeof(rn_receive_descriptor_t) == 32 );
    check( sizeof(rn_held_entry_t) == 16 );
    check( sizeof(rn_send_descriptor_t) == 64 );
    check( sizeof(rn_sent_packet_t) + 2 * sizeof(rn_sent_index_entry_t) + 16 * sizeof(uint32_t) == 128 );
    check( sizeof(rn_delivery_t) == 24 );
    check( sizeof(rn_sequenced_hold_t) == 1208 );
    check( sizeof(rn_receipt_t) == 16 );

    rocketnet_message_config_t config;
    rn_region_layout_t layout;

    // the defaults: section 14's worked total, to the byte, and no padding
    rocketnet_message_default_config( &config );
    check( rn_region_layout( &config, &layout ) );
    channels_check_layout( layout );
    check( layout.formula_bytes == 449024 );
    check( layout.region_bytes == 449024 );
    check( rocketnet_message_connection_bytes( &config ) == 449024 );
    check( rocketnet_message_connection_bytes( NULL ) == 449024 );
    check( rn_region_layout( NULL, &layout ) && layout.region_bytes == 449024 );

    // the lean profile: "about 177 KiB", and the design's own lean figures
    // with the default max_message_bytes are refused
    channels_lean_config( &config );
    config.max_message_bytes = 32768;
    check( !rn_region_layout( &config, &layout ) );
    channels_lean_config( &config );
    check( rn_region_layout( &config, &layout ) );
    channels_check_layout( layout );
    check( layout.formula_bytes == 181664 );

    // the minimum and the maximum the table admits
    channels_min_config( &config );
    check( rn_region_layout( &config, &layout ) );
    channels_check_layout( layout );
    check( layout.region_bytes == rocketnet_message_connection_bytes( &config ) );
    check( layout.store_chunks[2] == 3 );

    channels_max_config( &config );
    check( rn_region_layout( &config, &layout ) );
    channels_check_layout( layout );
    check( layout.region_bytes == rocketnet_message_connection_bytes( &config ) );
    check( layout.region_bytes > ( (uint64_t) 1 << 28 ) );

    // the channel term moves alone: 48 bytes a channel, nothing scales as channels x window
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    const uint64_t one = rocketnet_message_connection_bytes( &config );
    config.num_channels = ROCKETNET_MAX_CHANNELS;
    const uint64_t all = rocketnet_message_connection_bytes( &config );
    check( one != 0 && all != 0 );
    check( all - one <= 48 * (uint64_t) ( ROCKETNET_MAX_CHANNELS - 1 ) + 2 * RN_REGION_ALIGNMENT );
    check( all - one + 2 * RN_REGION_ALIGNMENT >= 48 * (uint64_t) ( ROCKETNET_MAX_CHANNELS - 1 ) );
}

static void test_channels_allocator_validation()
{
    rocketnet_message_config_t config;
    rn_region_layout_t layout;

    struct refusal_t { const char * what; int field; int value; };
    enum { CHANNELS, WINDOW, RING, MAX_MESSAGE, SEND_BUFFER, QUEUE, IN_FLIGHT, PER_UPDATE, DELIVERIES, HOLDS, RECEIPTS, RTO_INITIAL, RTO_MIN, RTO_MAX, TIMEOUT };
    const refusal_t refusals[] =
    {
        { "no channels",                  CHANNELS,    0 },
        { "channels past the maximum",    CHANNELS,    ROCKETNET_MAX_CHANNELS + 1 },
        { "window below 64",              WINDOW,      32 },
        { "window not a power of two",    WINDOW,      1000 },
        { "window past 2^20",             WINDOW,      ( 1 << 20 ) + 1 },
        { "ring not a multiple of 16",    RING,        65544 + 4 },
        { "ring below 4096",              RING,        4080 },
        { "max message past the ring",    MAX_MESSAGE, 65536 + 16 },
        { "max message below 1191",       MAX_MESSAGE, 1190 },
        { "send buffer below 12288",      SEND_BUFFER, 12287 },
        { "queue below 16",               QUEUE,       15 },
        { "packets in flight below 16",   IN_FLIGHT,   15 },
        { "packets per update 0",         PER_UPDATE,  0 },
        { "no unreliable deliveries",     DELIVERIES,  0 },
        { "negative sequenced holds",     HOLDS,       -1 },
        { "receipt ring below 16",        RECEIPTS,    15 },
        { "rto initial below 10",         RTO_INITIAL, 9 },
        { "rto min 0",                    RTO_MIN,     0 },
        { "rto max past a minute",        RTO_MAX,     60001 },
        { "reliable timeout below 1s",    TIMEOUT,     999 },
    };

    for ( size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++ )
    {
        rocketnet_message_default_config( &config );
        int * fields[] = { &config.num_channels, &config.window_messages, &config.receive_ring_bytes, &config.max_message_bytes,
                           &config.send_buffer_bytes, &config.send_queue_messages, &config.packets_in_flight, &config.max_packets_per_update,
                           &config.unreliable_deliveries, &config.sequenced_holds, &config.receipt_ring, &config.rto_initial_ms,
                           &config.rto_min_ms, &config.rto_max_ms, &config.reliable_timeout_ms };
        *fields[refusals[i].field] = refusals[i].value;
        const int errors = channels_log_lines[ROCKETNET_LOG_LEVEL_ERROR];
        if ( rn_region_layout( &config, &layout ) )
            channels_check_failed( refusals[i].what, __FILE__, __LINE__ );
        check( rocketnet_message_connection_bytes( &config ) == 0 );
        check( channels_log_lines[ROCKETNET_LOG_LEVEL_ERROR] > errors );     // refused loudly
    }

    // the cross rules of section 14, each alone

    // more fragments than the window holds: 911 > 64
    rocketnet_message_default_config( &config );
    config.window_messages = 64;
    config.receive_ring_bytes = 1 << 20;
    config.max_message_bytes = 1 << 20;
    config.send_buffer_bytes = 1 << 24;
    check( !rn_region_layout( &config, &layout ) );
    config.window_messages = 1024;
    check( rn_region_layout( &config, &layout ) );

    // a 1152 class that cannot hold one maximal block: 12288 bytes leave
    // three chunks, and 4096 bytes need four
    channels_min_config( &config );
    config.max_message_bytes = 4096;
    check( !rn_region_layout( &config, &layout ) );
    config.max_message_bytes = 3456;
    check( rn_region_layout( &config, &layout ) );
    check( layout.store_chunks[2] == 3 );

    // the rto bounds in order
    rocketnet_message_default_config( &config );
    config.rto_min_ms = 3000;
    check( !rn_region_layout( &config, &layout ) );
    rocketnet_message_default_config( &config );
    config.rto_initial_ms = 2001;
    check( !rn_region_layout( &config, &layout ) );

    // the arena refuses with nothing allocated
    rocketnet_message_default_config( &config );
    config.window_messages = 1000;
    rn_arena_t arena;
    const int before = channels_hook_mallocs.load();
    check( !rn_arena_create( &arena, NULL, &config, 1 ) );
    rocketnet_message_default_config( &config );
    check( !rn_arena_create( &arena, NULL, &config, 0 ) );
    check( channels_hook_mallocs.load() == before );
}

static void test_channels_allocator_send_store()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    uint64_t * full = &counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL];

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rn_arena_t arena;
    check( rn_arena_create( &arena, NULL, &config, 1 ) );
    rn_region_t region;
    rn_region_carve( &region, &arena.layout, rn_arena_region_memory( &arena, 0 ), counters.values );
    rn_send_store_t * store = &region.store;

    const int sizes[RN_SEND_STORE_NUM_CLASSES][2] = { { 1, 128 }, { 129, 512 }, { 513, ROCKETNET_MESSAGE_FRAGMENT_BYTES } };
    std::vector<uint32_t> handles;
    std::vector<uint8_t> seen;

    for ( int c = 0; c < RN_SEND_STORE_NUM_CLASSES; c++ )
    {
        const int chunks = arena.layout.store_chunks[c];
        check( store->chunks[c] == chunks );
        check( store->free_count[c] == chunks );

        // fill to refusal: every chunk distinct, in bounds, of the smallest class that fits
        handles.clear();
        seen.assign( (size_t) chunks, 0 );
        for ( int i = 0; i < chunks; i++ )
        {
            const int bytes = channels_random_int( sizes[c][0], sizes[c][1] );
            const uint32_t chunk = rn_send_store_alloc( store, bytes );
            check( chunk != RN_SEND_STORE_NONE );
            check( channels_store_class( chunk ) == c );
            const uint32_t index = chunk & 0x3FFFFFFFu;
            check( index < (uint32_t) chunks );
            check( !seen[index] );                                  // never double-allocated
            seen[index] = 1;
            check( rn_send_store_chunk_bytes( store, chunk ) == arena.layout.store_chunk_bytes[c] );
            uint8_t * data = rn_send_store_chunk_data( store, chunk );
            check( rn_region_contains( &region, data, (uint64_t) arena.layout.store_chunk_bytes[c] ) );
            memset( data, 0x5A, (size_t) bytes );
            handles.push_back( chunk );
            check( store->free_count[c] == chunks - i - 1 );
        }
        const uint64_t refusals = *full;
        check( rn_send_store_alloc( store, sizes[c][1] ) == RN_SEND_STORE_NONE );
        check( *full == refusals + 1 );                              // counted, once
        check( store->free_count[c] == 0 );

        // the other classes are untouched
        for ( int d = 0; d < RN_SEND_STORE_NUM_CLASSES; d++ )
            if ( d != c )
                check( store->free_count[d] == store->chunks[d] );

        // free in random order, refill to the same count
        for ( size_t i = handles.size(); i > 1; i-- )
        {
            const size_t j = (size_t) ( channels_random() % i );
            const uint32_t t = handles[i - 1]; handles[i - 1] = handles[j]; handles[j] = t;
        }
        for ( size_t i = 0; i < handles.size(); i++ )
        {
            rn_send_store_free( store, handles[i] );
            check( store->free_count[c] == (int) i + 1 );
        }
        int refilled = 0;
        while ( rn_send_store_alloc( store, sizes[c][0] ) != RN_SEND_STORE_NONE )
            refilled++;
        check( refilled == chunks );
        check( *full == refusals + 2 );
        rn_region_carve( &region, &arena.layout, rn_arena_region_memory( &arena, 0 ), counters.values );
    }

    // bad arguments are refused and not counted as a full store
    const uint64_t refusals = *full;
    check( rn_send_store_alloc( store, 0 ) == RN_SEND_STORE_NONE );
    check( rn_send_store_alloc( store, ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1 ) == RN_SEND_STORE_NONE );
    check( rn_send_store_alloc_block( store, ROCKETNET_MESSAGE_FRAGMENT_BYTES ) == RN_SEND_STORE_NONE );
    check( rn_send_store_alloc_block( store, config.max_message_bytes + 1 ) == RN_SEND_STORE_NONE );
    check( *full == refusals );

    // blocks: ceil(bytes / 1152) chained chunks of the 1152 class, all or none
    const int total = store->chunks[2];
    const int block_bytes = config.max_message_bytes;
    const int k = ( block_bytes + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t block = rn_send_store_alloc_block( store, block_bytes );
    check( block != RN_SEND_STORE_NONE );
    check( store->free_count[2] == total - k );
    int walked = 0;
    for ( uint32_t chunk = block; chunk != RN_SEND_STORE_NONE; chunk = rn_send_store_block_next( store, chunk ) )
    {
        check( channels_store_class( chunk ) == 2 );
        memset( rn_send_store_chunk_data( store, chunk ), walked, ROCKETNET_MESSAGE_FRAGMENT_BYTES );
        walked++;
        check( walked <= k );
    }
    check( walked == k );

    // take singles until fewer than k remain, then a block is refused whole
    std::vector<uint32_t> singles;
    while ( store->free_count[2] >= k )
        singles.push_back( rn_send_store_alloc( store, ROCKETNET_MESSAGE_FRAGMENT_BYTES ) );
    const int left = store->free_count[2];
    check( left > 0 );
    check( rn_send_store_alloc_block( store, block_bytes ) == RN_SEND_STORE_NONE );
    check( *full == refusals + 1 );
    check( store->free_count[2] == left );                           // nothing taken
    for ( size_t i = 0; i < singles.size(); i++ )
        rn_send_store_free( store, singles[i] );

    // the block's bytes survived the singles around it, and one free returns all k
    walked = 0;
    for ( uint32_t chunk = block; chunk != RN_SEND_STORE_NONE; chunk = rn_send_store_block_next( store, chunk ) )
    {
        const uint8_t * data = rn_send_store_chunk_data( store, chunk );
        for ( int i = 0; i < ROCKETNET_MESSAGE_FRAGMENT_BYTES; i++ )
            check( data[i] == (uint8_t) walked );
        walked++;
    }
    rn_send_store_free( store, block );
    check( store->free_count[2] == total );

    // the smallest block, two fragments
    const uint32_t two = rn_send_store_alloc_block( store, ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1 );
    check( two != RN_SEND_STORE_NONE );
    check( rn_send_store_block_next( store, two ) != RN_SEND_STORE_NONE );
    check( rn_send_store_block_next( store, rn_send_store_block_next( store, two ) ) == RN_SEND_STORE_NONE );
    check( store->free_count[2] == total - 2 );
    rn_send_store_free( store, two );
    check( store->free_count[2] == total );

    rn_arena_destroy( &arena );
}

static void test_channels_allocator_descriptor_pool()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rocketnet_message_config_t config;
    channels_min_config( &config );
    rn_arena_t arena;
    check( rn_arena_create( &arena, NULL, &config, 1 ) );
    rn_region_t region;
    rn_region_carve( &region, &arena.layout, rn_arena_region_memory( &arena, 0 ), counters.values );

    const int Q = config.send_queue_messages;
    std::vector<uint32_t> taken;
    std::vector<uint8_t> seen( (size_t) Q, 0 );
    for ( int round = 0; round < 3; round++ )
    {
        check( region.descriptor_free_count == Q );
        taken.clear();
        seen.assign( (size_t) Q, 0 );
        for ( int i = 0; i < Q; i++ )
        {
            const uint32_t d = rn_region_alloc_descriptor( &region );
            check( d < (uint32_t) Q );
            check( !seen[d] );
            seen[d] = 1;
            check( rn_region_contains( &region, &region.send_descriptors[d], sizeof(rn_send_descriptor_t) ) );
            taken.push_back( d );
        }
        const uint64_t before = counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL];
        check( rn_region_alloc_descriptor( &region ) == RN_SEND_STORE_NONE );
        check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == before + 1 );
        check( region.descriptor_free_count == 0 );
        while ( !taken.empty() )
        {
            const size_t j = (size_t) ( channels_random() % taken.size() );
            rn_region_free_descriptor( &region, taken[j] );
            taken[j] = taken.back();
            taken.pop_back();
        }
    }
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL] == 0 );
    rn_arena_destroy( &arena );
}

static void test_channels_allocator_arena()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    // one rocketnet_malloc at create for every region, one free at destroy
    const int mallocs = channels_hook_mallocs.load(), frees = channels_hook_frees.load();
    const int slots = 64;
    rn_arena_t arena;
    check( rn_arena_create( &arena, NULL, &config, slots ) );
    check( channels_hook_mallocs.load() == mallocs + 1 );
    check( arena.num_regions == slots );

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    std::vector<rn_region_t> regions( (size_t) slots );
    for ( int i = 0; i < slots; i++ )
    {
        uint8_t * memory = (uint8_t*) rn_arena_region_memory( &arena, i );
        check( ( (uintptr_t) memory ) % RN_REGION_ALIGNMENT == 0 );
        check( memory == arena.memory + (uint64_t) i * arena.layout.region_bytes );
        rn_region_t & region = regions[(size_t) i];
        rn_region_carve( &region, &arena.layout, memory, counters.values );

        // every carve-out where the layout says, typed, inside this region and no other
        check( region.base == memory );
        check( (uint8_t*) region.channel_send == memory + arena.layout.part_offset[RN_REGION_CHANNEL_SEND] );
        check( (uint8_t*) region.channel_receive == memory + arena.layout.part_offset[RN_REGION_CHANNEL_RECEIVE] );
        check( (uint8_t*) region.send_numbers == memory + arena.layout.part_offset[RN_REGION_SEND_NUMBERS] );
        check( (uint8_t*) region.receive_descriptors == memory + arena.layout.part_offset[RN_REGION_RECEIVE_DESCRIPTORS] );
        check( (uint8_t*) region.received_bits == memory + arena.layout.part_offset[RN_REGION_RECEIVED_BITS] );
        check( (uint8_t*) region.held_index == memory + arena.layout.part_offset[RN_REGION_HELD_INDEX] );
        check( region.receive_ring == memory + arena.layout.part_offset[RN_REGION_RECEIVE_RING] );
        check( (uint8_t*) region.send_descriptors == memory + arena.layout.part_offset[RN_REGION_SEND_DESCRIPTORS] );
        check( (uint8_t*) region.sent_packets == memory + arena.layout.part_offset[RN_REGION_SENT_PACKETS] );
        check( (uint8_t*) region.sent_index == memory + arena.layout.part_offset[RN_REGION_SENT_INDEX] );
        check( (uint8_t*) region.sent_refs == memory + arena.layout.part_offset[RN_REGION_SENT_REFS] );
        check( (uint8_t*) region.deliveries == memory + arena.layout.part_offset[RN_REGION_DELIVERIES] );
        check( (uint8_t*) region.sequenced_holds == memory + arena.layout.part_offset[RN_REGION_SEQUENCED_HOLDS] );
        check( (uint8_t*) region.receipts == memory + arena.layout.part_offset[RN_REGION_RECEIPTS] );
        check( region.store.chunk_base[0] == memory + arena.layout.part_offset[RN_REGION_SEND_STORE] + arena.layout.store_class_offset[0] );
        check( region.store.chunk_base[2] == memory + arena.layout.part_offset[RN_REGION_SEND_STORE] + arena.layout.store_class_offset[2] );
        check( rn_region_contains( &region, memory, arena.layout.region_bytes ) );
        check( !rn_region_contains( &region, memory, arena.layout.region_bytes + 1 ) );
        check( !rn_region_contains( &region, memory - 1, 1 ) );
        check( region.receive_ring == memory + arena.layout.part_offset[RN_REGION_RECEIVE_RING] && ( (uintptr_t) region.receive_ring ) % 16 == 0 );

        // the fixed tables carve zeroed, whatever the memory held
        for ( uint64_t b = 0; b < arena.layout.part_bytes[RN_REGION_CHANNEL_SEND]; b++ )
            check( ( (uint8_t*) region.channel_send )[b] == 0 );
        for ( uint64_t b = 0; b < arena.layout.part_bytes[RN_REGION_HELD_INDEX]; b++ )
            check( ( (uint8_t*) region.held_index )[b] == 0 );
    }

    // a reconnect is a carve over the same memory: dirty tables come back clean
    memset( regions[3].channel_send, 0xEE, arena.layout.part_bytes[RN_REGION_CHANNEL_SEND] );
    memset( regions[3].receipts, 0xEE, arena.layout.part_bytes[RN_REGION_RECEIPTS] );
    (void) rn_send_store_alloc( &regions[3].store, 64 );
    (void) rn_region_alloc_descriptor( &regions[3] );
    rn_region_carve( &regions[3], &arena.layout, rn_arena_region_memory( &arena, 3 ), counters.values );
    for ( uint64_t b = 0; b < arena.layout.part_bytes[RN_REGION_RECEIPTS]; b++ )
        check( ( (uint8_t*) regions[3].receipts )[b] == 0 );
    check( regions[3].store.free_count[0] == regions[3].store.chunks[0] );
    check( regions[3].descriptor_free_count == config.send_queue_messages );

    check( channels_hook_mallocs.load() == mallocs + 1 );
    rn_arena_destroy( &arena );
    check( channels_hook_frees.load() == frees + 1 );
    check( arena.memory == NULL );
}

static void test_channels_allocator_sizing()
{
    char text[4096];

    // the defaults: the formula, each term, the exact region, and the server's total
    const int length = rocketnet_message_connection_sizing( NULL, 1000, text, (int) sizeof(text) );
    check( length > 0 && length < (int) sizeof(text) );
    check( (int) strlen( text ) == length );
    check( strstr( text, "48C + 96.125W + B + 64Q + S + 128P + 24(W + Du) + 1208Ru + 16Rq" ) != NULL );
    check( strstr( text, "C=32 W=1024 B=65536 S=131072 Q=1024 P=256 Du=256 Ru=16 Rq=256" ) != NULL );
    check( strstr( text, "region bytes (exact)" ) != NULL );
    check( strstr( text, "449024" ) != NULL );
    check( strstr( text, "449024000" ) != NULL );

    // a short buffer is terminated and reports the full length
    char small[32];
    check( rocketnet_message_connection_sizing( NULL, 1000, small, (int) sizeof(small) ) == length );
    check( small[sizeof(small) - 1] == '\0' );
    check( rocketnet_message_connection_sizing( NULL, 1000, NULL, 0 ) == length );

    // refused configs and connection counts write nothing useful and say so
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.window_messages = 100;
    check( rocketnet_message_connection_sizing( &config, 1, text, (int) sizeof(text) ) == 0 );
    check( rocketnet_message_connection_sizing( NULL, 0, text, (int) sizeof(text) ) == 0 );

    // the print form: one info line per line of the text
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    const int lines = channels_log_lines[ROCKETNET_LOG_LEVEL_INFO];
    rocketnet_message_print_connection_sizing( NULL, 1000 );
    rocketnet_message_connection_sizing( NULL, 1000, text, (int) sizeof(text) );
    int newlines = 0;
    for ( int i = 0; i < length; i++ )
        newlines += text[i] == '\n';
    check( channels_log_lines[ROCKETNET_LOG_LEVEL_INFO] - lines == newlines );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
}

// ----------------------------------------------------------------
//                  the soak at full configured load
// ----------------------------------------------------------------

// Layers 2 to 8 are not built yet, so the soak stands in for them: each
// tick it drives every carve-out of every region the way those layers will,
// to its configured capacity -- sends until the store and the descriptor
// pool refuse, every queued message written fragment by fragment into a
// stand-in transport frame with its numbers, sent-packet entries, index and
// references recorded, every record received into the descriptors, the
// bitset, the ring, the held index, the delivered list, the sequenced holds
// and the receipt ring, then a random half acked and freed -- and every
// fiftieth tick one connection reconnects, which is a carve. The traps are
// armed from the first carve to the last check.

struct channels_soak_connection_t
{
    rn_region_t region;
    std::vector<uint32_t> outstanding;      // descriptors in flight, reserved before arming
    int num_outstanding;
    int store_used[RN_SEND_STORE_NUM_CLASSES];
    uint64_t next_number;
    uint64_t ring_position;
    uint64_t sent_cursor;
    uint64_t ref_cursor;
    uint64_t packet_id;
    uint64_t receipt_cursor;
    uint64_t hold_cursor;
    uint32_t serial;
    uint64_t store_refusals;
    uint64_t queue_refusals;
};

static uint8_t channels_pattern( uint32_t serial, int i )
{
    return (uint8_t) ( serial * 131u + (uint32_t) i * 7u + ( (uint32_t) i >> 8 ) );
}

static uint8_t channels_frame[ROCKETNET_MAX_PAYLOAD_BYTES];

static void channels_soak_connect( channels_soak_connection_t & c, const rn_arena_t & arena, int index, uint64_t * counters )
{
    rn_region_carve( &c.region, &arena.layout, rn_arena_region_memory( &arena, index ), counters );
    c.num_outstanding = 0;
    memset( c.store_used, 0, sizeof(c.store_used) );
    for ( int k = 0; k < RN_SEND_STORE_NUM_CLASSES; k++ )
        check( c.region.store.free_count[k] == c.region.store.chunks[k] );
    check( c.region.descriptor_free_count == arena.layout.send_queue_messages );
}

static void channels_soak_send( channels_soak_connection_t & c, const rn_region_layout_t & layout )
{
    rn_region_t & region = c.region;
    const int attempts = layout.send_queue_messages / 4 + 64;
    for ( int attempt = 0; attempt < attempts; attempt++ )
    {
        const int roll = channels_random_int( 0, 99 );
        int bytes;
        if ( roll < 60 )      bytes = channels_random_int( 1, 128 );
        else if ( roll < 85 ) bytes = channels_random_int( 129, 512 );
        else if ( roll < 97 ) bytes = channels_random_int( 513, ROCKETNET_MESSAGE_FRAGMENT_BYTES );
        else                  bytes = channels_random_int( ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1, layout.max_message_bytes );

        const uint32_t d = rn_region_alloc_descriptor( &region );
        if ( d == RN_SEND_STORE_NONE )
        {
            check( c.num_outstanding == layout.send_queue_messages );
            c.queue_refusals++;
            continue;
        }
        const int k = ( bytes + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        const int cls = bytes <= 128 ? 0 : bytes <= 512 ? 1 : 2;
        const uint32_t chunk = k > 1 ? rn_send_store_alloc_block( &region.store, bytes ) : rn_send_store_alloc( &region.store, bytes );
        if ( chunk == RN_SEND_STORE_NONE )
        {
            check( region.store.free_count[cls] < k );
            c.store_refusals++;
            rn_region_free_descriptor( &region, d );
            continue;
        }
        check( channels_store_class( chunk ) == cls );
        c.store_used[cls] += k;

        rn_send_descriptor_t & descriptor = region.send_descriptors[d];
        memset( &descriptor, 0, sizeof(descriptor) );
        descriptor.chunk = chunk;
        descriptor.bytes = (uint32_t) bytes;
        descriptor.serial = ++c.serial;
        descriptor.channel = (uint16_t) ( descriptor.serial % (uint32_t) layout.num_channels );
        descriptor.ordering = region.channel_send[descriptor.channel].ordered_write++;
        region.channel_send[descriptor.channel].sequenced_write = 0;
        descriptor.fragments = (uint16_t) k;
        descriptor.reliability = ROCKETNET_RELIABLE_ORDERED;

        int written = 0;
        for ( uint32_t link = chunk; link != RN_SEND_STORE_NONE; link = rn_send_store_block_next( &region.store, link ) )
        {
            uint8_t * data = rn_send_store_chunk_data( &region.store, link );
            const int span = rn_send_store_chunk_bytes( &region.store, link );
            check( rn_region_contains( &region, data, (uint64_t) span ) );
            const int n = bytes - written < span ? bytes - written : span;
            for ( int i = 0; i < n; i++ )
                data[i] = channels_pattern( descriptor.serial, written + i );
            written += n;
        }
        check( written == bytes );
        c.outstanding[(size_t) c.num_outstanding++] = d;
    }
}

static void channels_sent_index_insert( rn_region_t & region, const rn_region_layout_t & layout, uint64_t packet_id, uint32_t entry )
{
    const uint64_t slots = 2 * (uint64_t) layout.packets_in_flight;
    for ( uint64_t probe = 0; probe < slots; probe++ )
    {
        rn_sent_index_entry_t & slot = region.sent_index[( packet_id + probe ) % slots];
        if ( slot.packet_id == 0 )
        {
            slot.packet_id = packet_id;
            slot.entry = entry;
            return;
        }
    }
    check( false );                         // at most P of 2P slots are ever live
}

static void channels_sent_index_remove( rn_region_t & region, const rn_region_layout_t & layout, uint64_t packet_id )
{
    const uint64_t slots = 2 * (uint64_t) layout.packets_in_flight;
    for ( uint64_t probe = 0; probe < slots; probe++ )
    {
        rn_sent_index_entry_t & slot = region.sent_index[( packet_id + probe ) % slots];
        if ( slot.packet_id == packet_id )
        {
            // backward-shift delete keeps every probe chain whole
            uint64_t hole = ( packet_id + probe ) % slots;
            region.sent_index[hole].packet_id = 0;
            uint64_t next = ( hole + 1 ) % slots;
            while ( region.sent_index[next].packet_id != 0 )
            {
                const uint64_t home = region.sent_index[next].packet_id % slots;
                const bool movable = ( next > hole ) ? ( home <= hole || home > next ) : ( home <= hole && home > next );
                if ( movable )
                {
                    region.sent_index[hole] = region.sent_index[next];
                    region.sent_index[next].packet_id = 0;
                    hole = next;
                }
                next = ( next + 1 ) % slots;
            }
            return;
        }
    }
    check( false );
}

static void channels_soak_commit_packet( channels_soak_connection_t & c, const rn_region_layout_t & layout, uint32_t ref_first, uint16_t ref_count )
{
    rn_region_t & region = c.region;
    const uint64_t P = (uint64_t) layout.packets_in_flight;
    const uint32_t entry = (uint32_t) ( c.sent_cursor % P );
    rn_sent_packet_t & sent = region.sent_packets[entry];
    if ( sent.packet_id != 0 )
        channels_sent_index_remove( region, layout, sent.packet_id );       // the oldest is evicted
    sent.packet_id = ++c.packet_id;
    sent.send_time = (double) c.packet_id;
    sent.ref_first = ref_first;
    sent.ref_count = ref_count;
    sent.unreliable_receipts = 0;
    channels_sent_index_insert( region, layout, sent.packet_id, entry );
    c.sent_cursor++;

    rn_receipt_t & receipt = region.receipts[c.receipt_cursor++ % (uint64_t) layout.receipt_ring];
    receipt.serial = (uint32_t) c.packet_id;
    receipt.status = ROCKETNET_RECEIPT_ACKED;
    receipt.packet_id = sent.packet_id;
}

static void channels_soak_receive_record( channels_soak_connection_t & c, const rn_region_layout_t & layout, const rn_send_descriptor_t & descriptor,
                                          uint64_t number, int fragment, const uint8_t * data, int bytes, int & delivered )
{
    rn_region_t & region = c.region;
    const uint64_t W = (uint64_t) layout.window_messages;
    const uint64_t B = (uint64_t) layout.receive_ring_bytes;

    // the window: descriptor and bit at number % W
    rn_receive_descriptor_t & receive = region.receive_descriptors[number % W];
    receive.number = number;
    receive.bytes = (uint32_t) bytes;
    receive.ordering = descriptor.ordering;
    receive.channel = descriptor.channel;
    receive.code = 3;
    receive.live = 1;
    receive.block_fragments = descriptor.fragments > 1 ? descriptor.fragments : 0;
    receive.block_have = (uint16_t) ( fragment + 1 );
    region.received_bits[( number % W ) / 64] |= 1ULL << ( ( number % W ) % 64 );

    // the ring: 16-aligned spans, padded at the wrap, inside B
    const uint64_t span = ( (uint64_t) bytes + 15 ) & ~(uint64_t) 15;
    if ( c.ring_position % B + span > B )
        c.ring_position += B - c.ring_position % B;
    uint8_t * placed = region.receive_ring + c.ring_position % B;
    check( rn_region_contains( &region, placed, span ) );
    check( placed + span <= region.receive_ring + B );
    memcpy( placed, data, (size_t) bytes );
    receive.ring = (uint32_t) ( ( c.ring_position % B ) / 16 );
    c.ring_position += span;

    // the held index, keyed (channel, ordering): in, found, out
    const uint64_t slots = 2 * W;
    const uint64_t home = ( (uint64_t) descriptor.channel * 0x9E3779B1u + descriptor.ordering ) % slots;
    uint64_t at = home;
    while ( region.held_index[at].live )
        at = ( at + 1 ) % slots;
    region.held_index[at].number = number;
    region.held_index[at].ordering = descriptor.ordering;
    region.held_index[at].channel = descriptor.channel;
    region.held_index[at].live = 1;
    check( region.held_index[at].number == number );
    region.held_index[at].live = 0;

    // the delivered list for this update, W + Du entries
    if ( delivered < layout.window_messages + layout.unreliable_deliveries )
    {
        rn_delivery_t & delivery = region.deliveries[delivered++];
        delivery.data = placed;
        delivery.bytes = (uint32_t) bytes;
        delivery.channel = descriptor.channel;
        delivery.code = 3;
        check( rn_region_contains( &region, delivery.data, delivery.bytes ) );
    }

    // a sequenced hold takes a copy of a packet-sized record
    if ( layout.sequenced_holds > 0 && bytes <= ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        rn_sequenced_hold_t & hold = region.sequenced_holds[c.hold_cursor++ % (uint64_t) layout.sequenced_holds];
        hold.ordering = descriptor.ordering;
        hold.channel = descriptor.channel;
        hold.bytes = (uint16_t) bytes;
        hold.live = 1;
        memcpy( hold.data, data, (size_t) bytes );
        check( rn_region_contains( &region, &hold, sizeof(hold) ) );
    }

    // the received bytes are the sent bytes
    for ( int i = 0; i < bytes; i++ )
        check( placed[i] == channels_pattern( descriptor.serial, fragment * ROCKETNET_MESSAGE_FRAGMENT_BYTES + i ) );
}

static void channels_soak_write_and_receive( channels_soak_connection_t & c, const rn_region_layout_t & layout )
{
    rn_region_t & region = c.region;
    const uint64_t W = (uint64_t) layout.window_messages;
    const uint64_t refs = 16 * (uint64_t) layout.packets_in_flight;
    int frame_bytes = 0;
    uint32_t ref_first = (uint32_t) ( c.ref_cursor % refs );
    uint16_t ref_count = 0;
    int delivered = 0;

    for ( int o = 0; o < c.num_outstanding; o++ )
    {
        rn_send_descriptor_t & descriptor = region.send_descriptors[c.outstanding[(size_t) o]];
        if ( descriptor.flags & 1 )
            continue;                       // written on an earlier tick
        descriptor.flags |= 1;
        descriptor.first_number = c.next_number;

        int fragment = 0;
        int written = 0;
        for ( uint32_t link = descriptor.chunk; link != RN_SEND_STORE_NONE; link = rn_send_store_block_next( &region.store, link ), fragment++ )
        {
            const int span = rn_send_store_chunk_bytes( &region.store, link );
            const int n = (int) descriptor.bytes - written < span ? (int) descriptor.bytes - written : span;
            const uint8_t * data = rn_send_store_chunk_data( &region.store, link );

            // a record that does not fit ends the packet
            if ( frame_bytes + n > ROCKETNET_MAX_PAYLOAD_BYTES || ref_count == 16 )
            {
                channels_soak_commit_packet( c, layout, ref_first, ref_count );
                frame_bytes = 0;
                ref_first = (uint32_t) ( c.ref_cursor % refs );
                ref_count = 0;
            }
            memcpy( channels_frame + frame_bytes, data, (size_t) n );

            const uint64_t number = c.next_number++;
            rn_send_number_t & entry = region.send_numbers[number % W];
            entry.number = number;
            entry.descriptor = c.outstanding[(size_t) o];
            entry.fragment = (uint16_t) fragment;
            entry.ring_units = (uint8_t) ( ( n + 15 ) / 16 );
            entry.acked = 0;
            region.sent_refs[c.ref_cursor++ % refs] = (uint32_t) number;
            ref_count++;

            channels_soak_receive_record( c, layout, descriptor, number, fragment, channels_frame + frame_bytes, n, delivered );
            frame_bytes += n;
            written += n;
        }
        check( written == (int) descriptor.bytes );
        check( fragment == descriptor.fragments );
    }
    if ( ref_count > 0 )
        channels_soak_commit_packet( c, layout, ref_first, ref_count );
}

static void channels_soak_ack( channels_soak_connection_t & c, const rn_region_layout_t & layout )
{
    rn_region_t & region = c.region;
    const uint64_t W = (uint64_t) layout.window_messages;
    int acks = c.num_outstanding / 2;
    while ( acks-- > 0 )
    {
        const int o = channels_random_int( 0, c.num_outstanding - 1 );
        const uint32_t d = c.outstanding[(size_t) o];
        rn_send_descriptor_t & descriptor = region.send_descriptors[d];

        // the store kept every byte while the message was in flight
        int written = 0;
        for ( uint32_t link = descriptor.chunk; link != RN_SEND_STORE_NONE; link = rn_send_store_block_next( &region.store, link ) )
        {
            const uint8_t * data = rn_send_store_chunk_data( &region.store, link );
            const int span = rn_send_store_chunk_bytes( &region.store, link );
            const int n = (int) descriptor.bytes - written < span ? (int) descriptor.bytes - written : span;
            for ( int i = 0; i < n; i++ )
                check( data[i] == channels_pattern( descriptor.serial, written + i ) );
            written += n;
        }
        if ( descriptor.flags & 1 )
        {
            for ( int f = 0; f < descriptor.fragments; f++ )
            {
                const uint64_t number = descriptor.first_number + (uint64_t) f;
                if ( region.send_numbers[number % W].number == number )
                    region.send_numbers[number % W].acked = 1;
                region.received_bits[( number % W ) / 64] &= ~( 1ULL << ( ( number % W ) % 64 ) );
            }
        }
        const int cls = channels_store_class( descriptor.chunk );
        c.store_used[cls] -= descriptor.fragments;
        rn_send_store_free( &region.store, descriptor.chunk );
        rn_region_free_descriptor( &region, d );
        c.outstanding[(size_t) o] = c.outstanding[(size_t) --c.num_outstanding];
    }

    // conservation, every tick: free + used = total, per class and for the pool
    for ( int k = 0; k < RN_SEND_STORE_NUM_CLASSES; k++ )
        check( region.store.free_count[k] + c.store_used[k] == region.store.chunks[k] );
    check( region.descriptor_free_count + c.num_outstanding == layout.send_queue_messages );
}

static void test_channels_allocator_no_live_allocation()
{
    // the witness: each trap fires before the soak trusts its silence
    {
        channels_witness = 1;
        channels_witnessed = 0;
        channels_armed = 1;
        void * p = rocketnet_malloc( NULL, 64 );
        rocketnet_free( NULL, p );
        void * volatile q = ::operator new( 16 );
        ::operator delete( q );
        int expected = 4;
#if CHANNELS_TRAP_LIBC
        void * ( * volatile allocate )( size_t ) = malloc;
        void ( * volatile release )( void * ) = free;
        release( allocate( 64 ) );
        expected += 2;
#endif
        channels_armed = 0;
        channels_witness = 0;
        check( channels_witnessed.load() == expected );
    }

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    // created before the traps arm: a server's slots and a client, one
    // allocation each, the only two the message layer ever makes
    const int server_slots = 16;
    rn_arena_t server, client;
    check( rn_arena_create( &server, NULL, &config, server_slots ) );
    check( rn_arena_create( &client, NULL, &config, 1 ) );
    const int connections = server_slots + 1;

    rocketnet_counters_t server_counters, client_counters;
    memset( &server_counters, 0, sizeof(server_counters) );
    memset( &client_counters, 0, sizeof(client_counters) );

    std::vector<channels_soak_connection_t> soak( (size_t) connections );
    for ( int i = 0; i < connections; i++ )
    {
        memset( &soak[(size_t) i].region, 0, sizeof(rn_region_t) );
        soak[(size_t) i].outstanding.assign( (size_t) config.send_queue_messages, 0 );
        soak[(size_t) i].num_outstanding = 0;
        soak[(size_t) i].next_number = 0;
        soak[(size_t) i].ring_position = 0;
        soak[(size_t) i].sent_cursor = 0;
        soak[(size_t) i].ref_cursor = 0;
        soak[(size_t) i].packet_id = 0;
        soak[(size_t) i].receipt_cursor = 0;
        soak[(size_t) i].hold_cursor = 0;
        soak[(size_t) i].serial = 0;
        soak[(size_t) i].store_refusals = 0;
        soak[(size_t) i].queue_refusals = 0;
    }

    channels_armed = 1;

    channels_phase = "connect";
    for ( int i = 0; i < connections; i++ )
    {
        const bool is_client = i == server_slots;
        channels_soak_connect( soak[(size_t) i], is_client ? client : server, is_client ? 0 : i,
                               is_client ? client_counters.values : server_counters.values );
    }

    const int ticks = 300;
    for ( int tick = 0; tick < ticks; tick++ )
    {
        for ( int i = 0; i < connections; i++ )
        {
            channels_soak_connection_t & c = soak[(size_t) i];
            channels_phase = "send";
            channels_soak_send( c, server.layout );
            channels_phase = "write and receive";
            channels_soak_write_and_receive( c, server.layout );
            channels_phase = "ack";
            channels_soak_ack( c, server.layout );
        }

        // one connection reconnects every fiftieth tick: a carve at connect
        if ( tick % 50 == 49 )
        {
            channels_phase = "reconnect";
            const int i = ( tick / 50 ) % connections;
            const bool is_client = i == server_slots;
            channels_soak_connect( soak[(size_t) i], is_client ? client : server, is_client ? 0 : i,
                                   is_client ? client_counters.values : server_counters.values );
        }
    }

    channels_phase = "final checks";
    uint64_t store_refusals = 0, queue_refusals = 0;
    for ( int i = 0; i < server_slots; i++ )
    {
        store_refusals += soak[(size_t) i].store_refusals;
        queue_refusals += soak[(size_t) i].queue_refusals;
    }
    const uint64_t server_store_full = server_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL];
    const uint64_t server_queue_full = server_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL];
    const uint64_t client_store_full = client_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL];
    const uint64_t client_queue_full = client_counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL];

    channels_armed = 0;

    // full load was reached, and every refusal was counted exactly once, on its own owner
    check( store_refusals > 0 );
    check( server_store_full == store_refusals );
    check( server_queue_full == queue_refusals );
    check( client_store_full == soak[(size_t) server_slots].store_refusals );
    check( client_queue_full == soak[(size_t) server_slots].queue_refusals );
    check( client_store_full > 0 );

    rn_arena_destroy( &server );
    rn_arena_destroy( &client );
}

// ----------------------------------------------------------------
//                              the runner
// ----------------------------------------------------------------

struct channels_test_t
{
    const char * name;
    void ( *function )();
};

static const channels_test_t channels_tests[] =
{
    { "test_channels_allocator_layout", test_channels_allocator_layout },
    { "test_channels_allocator_validation", test_channels_allocator_validation },
    { "test_channels_allocator_send_store", test_channels_allocator_send_store },
    { "test_channels_allocator_descriptor_pool", test_channels_allocator_descriptor_pool },
    { "test_channels_allocator_arena", test_channels_allocator_arena },
    { "test_channels_allocator_sizing", test_channels_allocator_sizing },
    { "test_channels_allocator_no_live_allocation", test_channels_allocator_no_live_allocation },
};

static int channels_test_count()
{
    return (int) ( sizeof(channels_tests) / sizeof(channels_tests[0]) );
}

static const char * channels_test_name( int index )
{
    return channels_tests[index].name;
}

static int channels_worker( const char * name )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    setvbuf( stderr, NULL, _IOLBF, 0 );

    // the hooks go in before init, with nothing outstanding
    rocketnet_allocator( channels_hook_malloc, channels_hook_free );
    rocketnet_log_function( channels_log_sink );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    if ( !rocketnet_init() )
    {
        fprintf( stderr, "error: failed to initialize rocketnet\n" );
        return 1;
    }

    for ( int i = 0; i < channels_test_count(); i++ )
    {
        if ( strcmp( channels_tests[i].name, name ) != 0 )
            continue;
        printf( "%s\n", name );
        fflush( stdout );
        channels_tests[i].function();
        rocketnet_term();
        return 0;
    }
    fprintf( stderr, "unknown channels test %s\n", name );
    rocketnet_term();
    return 2;
}

int main( int argc, char ** argv )
{
    return suite_main( argc, argv, channels_worker, channels_test_count, channels_test_name, NULL, 0, NULL, 0, true );
}

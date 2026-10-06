/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The message layer's tests (docs/CHANNELS.md). Layer 1, the region: the
    layout against section 14's formula, validation, the send store's three
    classes, the descriptor pool, the arena, the sizing print, and the gate
    the card names, ChannelsAllocatorNoLiveAllocation (as a suite name,
    test_channels_allocator_no_live_allocation): a soak of send and receive at
    full configured load with every allocation path trapped.

    Its own process, run by ./bin/test as the exec row test_channels, because
    the trap replaces this process's allocation functions. With no arguments
    every case runs in order and prints its name; `--run <name>` runs one.
*/

#include "rocketnet.h"
#include "rocketnet_counters.h"
#include "rocketnet_allocator.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

// ----------------------------------------------------------------
//                          the allocation trap
// ----------------------------------------------------------------

// Armed for the soak. While armed, the first call to any allocation function
// ends the process with a line naming it. Three nets: the rocketnet allocator
// hooks (installed before init), the global operator new and delete, and on
// glibc without a sanitizer the C allocation functions themselves.

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_MEMORY__)
#define RN_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define RN_TEST_SANITIZED 1
#endif
#endif

#if defined(__GLIBC__) && !defined(RN_TEST_SANITIZED) && !defined(__clang_analyzer__)
#define RN_TEST_TRAP_LIBC 1
#else
#define RN_TEST_TRAP_LIBC 0
#endif

static volatile int rn_trap_armed = 0;
static const char * volatile rn_trap_phase = "idle";

static void rn_trap_write( const char * text )
{
#if defined(_WIN32)
    fputs( text, stderr );
#else
    size_t length = strlen( text );
    while ( length > 0 )
    {
        const ssize_t written = write( 2, text, length );
        if ( written <= 0 )
            break;
        text += written;
        length -= (size_t) written;
    }
#endif
}

static void rn_trap_fire( const char * function, size_t bytes )
{
    rn_trap_armed = 0;
    char line[160];
    // snprintf of integers and strings does not allocate
    snprintf( line, sizeof(line), "live allocation: %s (%llu bytes) during %s\n", function, (unsigned long long) bytes, rn_trap_phase );
    rn_trap_write( line );
    _Exit( 1 );
}

static uint64_t rn_hook_mallocs = 0;
static uint64_t rn_hook_frees = 0;

static void * rn_test_malloc_hook( void * context, size_t bytes )
{
    (void) context;
    if ( rn_trap_armed )
        rn_trap_fire( "rocketnet_malloc", bytes );
    rn_hook_mallocs++;
    return malloc( bytes );
}

static void rn_test_free_hook( void * context, void * p )
{
    (void) context;
    if ( rn_trap_armed )
        rn_trap_fire( "rocketnet_free", 0 );
    rn_hook_frees++;
    free( p );
}

#if RN_TEST_TRAP_LIBC

extern "C" void * __libc_malloc( size_t bytes );
extern "C" void __libc_free( void * p );
extern "C" void * __libc_calloc( size_t count, size_t bytes );
extern "C" void * __libc_realloc( void * p, size_t bytes );
extern "C" void * __libc_memalign( size_t alignment, size_t bytes );

extern "C" void * malloc( size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "malloc", bytes );
    return __libc_malloc( bytes );
}

extern "C" void free( void * p )
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "free", 0 );
    __libc_free( p );
}

extern "C" void * calloc( size_t count, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "calloc", count * bytes );
    return __libc_calloc( count, bytes );
}

extern "C" void * realloc( void * p, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "realloc", bytes );
    return __libc_realloc( p, bytes );
}

extern "C" void * memalign( size_t alignment, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "memalign", bytes );
    return __libc_memalign( alignment, bytes );
}

extern "C" void * aligned_alloc( size_t alignment, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "aligned_alloc", bytes );
    return __libc_memalign( alignment, bytes );
}

extern "C" int posix_memalign( void ** out, size_t alignment, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "posix_memalign", bytes );
    void * p = __libc_memalign( alignment, bytes );
    if ( !p )
        return 12; // ENOMEM
    *out = p;
    return 0;
}

#endif // #if RN_TEST_TRAP_LIBC

void * operator new( size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new", bytes );
    void * p = malloc( bytes ? bytes : 1 );
    if ( !p )
        throw std::bad_alloc();
    return p;
}

void * operator new[]( size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new[]", bytes );
    void * p = malloc( bytes ? bytes : 1 );
    if ( !p )
        throw std::bad_alloc();
    return p;
}

void * operator new( size_t bytes, const std::nothrow_t & ) noexcept
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new", bytes );
    return malloc( bytes ? bytes : 1 );
}

void * operator new[]( size_t bytes, const std::nothrow_t & ) noexcept
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new[]", bytes );
    return malloc( bytes ? bytes : 1 );
}

void operator delete( void * p ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete", 0 );
    free( p );
}

void operator delete[]( void * p ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete[]", 0 );
    free( p );
}

void operator delete( void * p, size_t ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete", 0 );
    free( p );
}

void operator delete[]( void * p, size_t ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete[]", 0 );
    free( p );
}

// ----------------------------------------------------------------
//                           test plumbing
// ----------------------------------------------------------------

// The library's log is captured, never printed: a passing run prints names only.

static char rn_log_last[1024];
static int rn_log_lines = 0;

static void rn_test_log_function( int level, const char * format, ... )
{
    (void) level;
    va_list args;
    va_start( args, format );
    vsnprintf( rn_log_last, sizeof(rn_log_last), format, args );
    va_end( args );
    rn_log_lines++;
}

#define check( condition )                                                                               \
    do                                                                                                   \
    {                                                                                                    \
        if ( !( condition ) )                                                                            \
        {                                                                                                \
            rn_trap_armed = 0;                                                                           \
            fprintf( stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition );              \
            if ( rn_log_last[0] )                                                                        \
                fprintf( stderr, "last log line: %s\n", rn_log_last );                                   \
            exit( 1 );                                                                                   \
        }                                                                                                \
    } while ( 0 )

static uint64_t rn_counter( const rocketnet_counters_t * counters, int counter )
{
    return rocketnet_counter_value( counters, counter );
}

static uint64_t rn_random_state = 0x9E3779B97F4A7C15ULL;

static uint32_t rn_random()
{
    // xorshift64*: no library call, so nothing on the soak's path can allocate
    rn_random_state ^= rn_random_state >> 12;
    rn_random_state ^= rn_random_state << 25;
    rn_random_state ^= rn_random_state >> 27;
    return (uint32_t) ( ( rn_random_state * 0x2545F4914F6CDD1DULL ) >> 32 );
}

static uint8_t rn_pattern( uint32_t serial, uint32_t offset )
{
    return (uint8_t) ( serial * 31u + offset * 7u + ( offset >> 8 ) );
}

// ----------------------------------------------------------------
//                              the cases
// ----------------------------------------------------------------

static void test_channels_allocator_layout()
{
    // section 14's formula, part by part, at the defaults: 449,024 bytes, no padding

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    check( config.num_channels == 32 );
    check( config.window_messages == 1024 );
    check( config.receive_ring_bytes == 65536 );

    rn_region_layout_t layout;
    check( rn_region_layout( &config, &layout ) );
    check( layout.formula_bytes == 449024 );
    check( layout.region_bytes == 449024 );
    check( rocketnet_message_connection_bytes( &config ) == 449024 );
    check( rocketnet_message_connection_bytes( NULL ) == 449024 );

    const uint64_t C = 32, W = 1024, B = 65536, Q = 1024, S = 131072, P = 256, Du = 256, Ru = 16, Rq = 256;
    check( layout.bytes[RN_PART_CHANNEL_SEND] + layout.bytes[RN_PART_CHANNEL_RECEIVE] == 48 * C );
    check( layout.bytes[RN_PART_SEND_NUMBERS] == 32 * W );
    check( layout.bytes[RN_PART_RECEIVE_DESCRIPTORS] == 32 * W );
    check( layout.bytes[RN_PART_RECEIVED_BITS] == W / 8 );
    check( layout.bytes[RN_PART_HELD_INDEX] == 16 * 2 * W );
    check( layout.bytes[RN_PART_RECEIVE_RING] == B );
    check( layout.bytes[RN_PART_SEND_DESCRIPTORS] == 64 * Q );
    check( layout.bytes[RN_PART_SEND_STORE] == S );
    check( layout.bytes[RN_PART_SENT_PACKETS] + layout.bytes[RN_PART_SENT_INDEX] + layout.bytes[RN_PART_SENT_REFS] == 128 * P );
    check( layout.bytes[RN_PART_DELIVERIES] == 24 * ( W + Du ) );
    check( layout.bytes[RN_PART_SEQUENCED_HOLDS] == 1208 * Ru );
    check( layout.bytes[RN_PART_RECEIPTS] == 16 * Rq );

    // every part 64-aligned, in order, disjoint, inside the region

    uint64_t end = 0;
    for ( int part = 0; part < RN_NUM_PARTS; part++ )
    {
        check( rn_region_part_name( part ) != NULL );
        check( layout.offset[part] % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
        check( layout.offset[part] >= end );
        end = layout.offset[part] + layout.bytes[part];
    }
    check( end <= layout.region_bytes );
    check( layout.region_bytes % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );

    // the store's classes at the defaults, as section 4 records them: 64-aligned
    // thirds of S, each chunk with its u32 link inside its third

    check( layout.store_chunks[RN_STORE_SMALL] == 330 );
    check( layout.store_chunks[RN_STORE_MEDIUM] == 84 );
    check( layout.store_chunks[RN_STORE_LARGE] == 37 );
    check( layout.store_chunks[RN_STORE_LARGE] >= ( 32768 + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES );

    // odd shapes pad, and the padding is all the region adds to the formula

    config.num_channels = 3;
    config.unreliable_deliveries = 1;
    config.sequenced_holds = 1;
    config.receipt_ring = 17;
    check( rn_region_layout( &config, &layout ) );
    check( layout.region_bytes > layout.formula_bytes );
    check( layout.region_bytes - layout.formula_bytes < 64 * RN_NUM_PARTS );
    check( layout.region_bytes % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
    end = 0;
    for ( int part = 0; part < RN_NUM_PARTS; part++ )
    {
        check( layout.offset[part] % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
        check( layout.offset[part] >= end );
        end = layout.offset[part] + layout.bytes[part];
    }
    check( end <= layout.region_bytes );

    // no sequenced holds is legal and takes no bytes

    config.sequenced_holds = 0;
    check( rn_region_layout( &config, &layout ) );
    check( layout.bytes[RN_PART_SEQUENCED_HOLDS] == 0 );
}

static void test_channels_allocator_validation()
{
    rocketnet_message_config_t config;

    // the defaults, the table's minimum where the cross rules allow it, and the maxima

    rocketnet_message_default_config( &config );
    check( rocketnet_message_validate_config( &config ) == 1 );

    struct rejected_t { const char * what; int field_offset; int value; };
    const rejected_t rejected[] =
    {
        { "num_channels 0",                         (int) offsetof( rocketnet_message_config_t, num_channels ), 0 },
        { "num_channels above the compile-time max", (int) offsetof( rocketnet_message_config_t, num_channels ), ROCKETNET_MAX_CHANNELS + 1 },
        { "window not a power of two",              (int) offsetof( rocketnet_message_config_t, window_messages ), 1000 },
        { "window below its minimum",               (int) offsetof( rocketnet_message_config_t, window_messages ), 32 },
        { "ring not a multiple of 16",              (int) offsetof( rocketnet_message_config_t, receive_ring_bytes ), 65536 + 8 },
        { "max message above the ring",             (int) offsetof( rocketnet_message_config_t, max_message_bytes ), 65536 + 16 },
        { "store too small for a maximal block",    (int) offsetof( rocketnet_message_config_t, send_buffer_bytes ), 12288 },
        { "queue below its minimum",                (int) offsetof( rocketnet_message_config_t, send_queue_messages ), 15 },
        { "rto_min above rto_max",                  (int) offsetof( rocketnet_message_config_t, rto_min_ms ), 3000 },
        { "rto_initial above rto_max",              (int) offsetof( rocketnet_message_config_t, rto_initial_ms ), 3000 },
        { "rto_initial below rto_min",              (int) offsetof( rocketnet_message_config_t, rto_initial_ms ), 20 },
    };

    for ( size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++ )
    {
        rocketnet_message_default_config( &config );
        memcpy( (uint8_t*) &config + rejected[i].field_offset, &rejected[i].value, sizeof(int) );
        const int lines_before = rn_log_lines;
        if ( rocketnet_message_validate_config( &config ) != 0 )
        {
            fprintf( stderr, "accepted: %s\n", rejected[i].what );
            exit( 1 );
        }
        // refused loudly, and the connection bytes are 0 for a refused config
        check( rn_log_lines > lines_before );
        check( rocketnet_message_connection_bytes( &config ) == 0 );
        rn_region_layout_t layout;
        check( !rn_region_layout( &config, &layout ) );
    }

    // ceil( max_message_bytes / F ) above W: a 64-message window holds at most 64 fragments

    rocketnet_message_default_config( &config );
    config.window_messages = 64;
    config.max_message_bytes = 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
    config.receive_ring_bytes = 1 << 20;
    config.send_buffer_bytes = 1 << 20;
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // the edge of the store rule: the large class holds exactly one maximal block

    rocketnet_message_default_config( &config );
    config.max_message_bytes = 4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config.send_buffer_bytes = 12288;               // a third of 12288 holds three 1152 chunks with their links
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // section 14's lean profile is refused at the default max_message_bytes: its
    // large class holds 18 chunks, a 32768-byte block needs 29 (section 4)

    rocketnet_message_default_config( &config );
    config.window_messages = 256;
    config.receive_ring_bytes = 32768;
    config.send_buffer_bytes = 65536;
    config.send_queue_messages = 256;
    config.packets_in_flight = 64;
    config.unreliable_deliveries = 128;
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 18 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );
    config.max_message_bytes = 18 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
    check( rocketnet_message_validate_config( &config ) == 0 );

    // the maximum channel count is legal

    rocketnet_message_default_config( &config );
    config.num_channels = ROCKETNET_MAX_CHANNELS;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // the smallest legal region: every minimum the cross rules allow

    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.window_messages = 64;
    config.receive_ring_bytes = 4096;
    config.max_message_bytes = 1191;
    config.send_buffer_bytes = 12288;
    config.send_queue_messages = 16;
    config.packets_in_flight = 16;
    config.max_packets_per_update = 1;
    config.unreliable_deliveries = 1;
    config.sequenced_holds = 0;
    config.receipt_ring = 16;
    config.rto_min_ms = 1;
    config.rto_initial_ms = 10;
    config.rto_max_ms = 10;
    config.reliable_timeout_ms = 1000;
    check( rocketnet_message_validate_config( &config ) == 1 );
    check( rocketnet_message_connection_bytes( &config ) > 0 );

    check( rocketnet_message_validate_config( NULL ) == 1 );       // NULL is the defaults
}

static void test_channels_allocator_send_store()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rn_region_layout_t layout;
    check( rn_region_layout( &config, &layout ) );

    uint8_t * memory = (uint8_t*) rocketnet_malloc( NULL, (size_t) layout.bytes[RN_PART_SEND_STORE] );
    check( memory );
    rn_send_store_t store;
    rn_send_store_init( &store, memory, layout.bytes[RN_PART_SEND_STORE] );

    const int class_bytes[RN_STORE_NUM_CLASSES] = { RN_STORE_SMALL_BYTES, RN_STORE_MEDIUM_BYTES, RN_STORE_LARGE_BYTES };
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        check( rn_send_store_total_chunks( &store, c ) == layout.store_chunks[c] );
        check( rn_send_store_free_chunks( &store, c ) == layout.store_chunks[c] );
    }

    // the smallest class that fits, by size

    check( rn_send_store_class_for( 1 ) == RN_STORE_SMALL );
    check( rn_send_store_class_for( 128 ) == RN_STORE_SMALL );
    check( rn_send_store_class_for( 129 ) == RN_STORE_MEDIUM );
    check( rn_send_store_class_for( 512 ) == RN_STORE_MEDIUM );
    check( rn_send_store_class_for( 513 ) == RN_STORE_LARGE );
    check( rn_send_store_class_for( 1152 ) == RN_STORE_LARGE );
    check( rn_send_store_class_for( 1153 ) == RN_STORE_LARGE );
    check( rn_send_store_chunks_needed( 1 ) == 1 );
    check( rn_send_store_chunks_needed( 1152 ) == 1 );
    check( rn_send_store_chunks_needed( 1153 ) == 2 );
    check( rn_send_store_chunks_needed( 32768 ) == 29 );

    // fill every class to refusal, with nothing else taken; the chunks are
    // distinct, inside the store and 16-aligned

    static uint32_t handles[RN_STORE_NUM_CLASSES][4096];
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const uint32_t total = rn_send_store_total_chunks( &store, c );
        check( total <= 4096 );
        for ( uint32_t i = 0; i < total; i++ )
        {
            handles[c][i] = rn_send_store_alloc( &store, class_bytes[c] );
            check( handles[c][i] != RN_CHUNK_NONE );
            uint8_t * data = rn_send_store_chunk_data( &store, handles[c][i] );
            check( data >= memory && data + class_bytes[c] <= memory + layout.bytes[RN_PART_SEND_STORE] );
            check( ( (uintptr_t) data & 15 ) == 0 );
            memset( data, (int) ( c * 64 + ( i & 63 ) ), (size_t) class_bytes[c] );
        }
        check( rn_send_store_free_chunks( &store, c ) == 0 );
        check( rn_send_store_alloc( &store, class_bytes[c] ) == RN_CHUNK_NONE );
        // no fallback into a larger class, and no other class moved
        for ( int other = 0; other < RN_STORE_NUM_CLASSES; other++ )
            if ( other > c )
                check( rn_send_store_free_chunks( &store, other ) == rn_send_store_total_chunks( &store, other ) );
    }

    // the bytes written are still there: no two handles share a chunk

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        for ( uint32_t i = 0; i < rn_send_store_total_chunks( &store, c ); i++ )
        {
            const uint8_t * data = rn_send_store_chunk_data( &store, handles[c][i] );
            for ( int b = 0; b < class_bytes[c]; b++ )
                check( data[b] == (uint8_t) ( c * 64 + ( i & 63 ) ) );
        }

    // free in random order, refill, and the counts hold at every step

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const uint32_t total = rn_send_store_total_chunks( &store, c );
        for ( uint32_t i = total; i > 1; i-- )
        {
            const uint32_t j = rn_random() % i;
            const uint32_t t = handles[c][i - 1]; handles[c][i - 1] = handles[c][j]; handles[c][j] = t;
        }
        for ( uint32_t i = 0; i < total; i++ )
        {
            rn_send_store_free( &store, handles[c][i] );
            check( rn_send_store_free_chunks( &store, c ) == i + 1 );
        }
        for ( uint32_t i = 0; i < total; i++ )
            check( rn_send_store_alloc( &store, class_bytes[c] ) != RN_CHUNK_NONE );
        check( rn_send_store_alloc( &store, class_bytes[c] ) == RN_CHUNK_NONE );
    }

    // blocks: chained large chunks, all or none

    rn_send_store_init( &store, memory, layout.bytes[RN_PART_SEND_STORE] );
    const uint32_t large = rn_send_store_total_chunks( &store, RN_STORE_LARGE );
    const uint32_t block = rn_send_store_alloc( &store, 32768 );
    check( block != RN_CHUNK_NONE );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == large - 29 );
    int links = 0;
    for ( uint32_t chunk = block; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &store, chunk ) )
    {
        memset( rn_send_store_chunk_data( &store, chunk ), links, RN_STORE_LARGE_BYTES );
        links++;
    }
    check( links == 29 );

    // a block bigger than what is left takes nothing

    const uint32_t left = rn_send_store_free_chunks( &store, RN_STORE_LARGE );
    check( rn_send_store_alloc( &store, (int) ( left + 1 ) * RN_STORE_LARGE_BYTES ) == RN_CHUNK_NONE );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == left );
    const uint32_t rest = rn_send_store_alloc( &store, (int) left * RN_STORE_LARGE_BYTES );
    check( rest != RN_CHUNK_NONE );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == 0 );
    check( rn_send_store_alloc( &store, 513 ) == RN_CHUNK_NONE );

    // freeing a block frees its whole chain; the first block's bytes survived the second
    links = 0;
    for ( uint32_t chunk = block; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &store, chunk ) )
    {
        const uint8_t * data = rn_send_store_chunk_data( &store, chunk );
        check( data[0] == (uint8_t) links && data[RN_STORE_LARGE_BYTES - 1] == (uint8_t) links );
        links++;
    }
    rn_send_store_free( &store, block );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == 29 );
    rn_send_store_free( &store, rest );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == large );

    // a refused double free changes nothing

    const uint32_t once = rn_send_store_alloc( &store, 100 );
    check( once != RN_CHUNK_NONE );
    rn_send_store_free( &store, once );
    const uint32_t small_free = rn_send_store_free_chunks( &store, RN_STORE_SMALL );
    check( !rn_send_store_free_checked( &store, once ) );
    check( rn_send_store_free_chunks( &store, RN_STORE_SMALL ) == small_free );
    check( !rn_send_store_free_checked( &store, RN_CHUNK_NONE ) );

    rocketnet_free( NULL, memory );
}

static void test_channels_allocator_descriptor_pool()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.send_queue_messages = 16;

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    // Q descriptors, then a counted refusal that takes no store chunk

    uint32_t descriptors[16];
    for ( int i = 0; i < 16; i++ )
    {
        descriptors[i] = rn_region_send_alloc( region, 10 );
        check( descriptors[i] != RN_DESCRIPTOR_NONE );
        check( descriptors[i] < 16 );
        for ( int j = 0; j < i; j++ )
            check( descriptors[j] != descriptors[i] );
        check( region->send_descriptors[descriptors[i]].bytes == 10 );
    }
    check( region->descriptors_free == 0 );
    const uint32_t small_free = rn_send_store_free_chunks( &region->store, RN_STORE_SMALL );
    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) == 1 );
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_free );

    // a free returns the descriptor and its chunk

    rn_region_send_free( region, descriptors[5] );
    check( region->descriptors_free == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_free + 1 );
    check( rn_region_send_alloc( region, 10 ) == descriptors[5] );

    // a store refusal takes no descriptor, and is counted as itself

    for ( int i = 0; i < 16; i++ )
        rn_region_send_free( region, descriptors[i] );
    check( region->descriptors_free == 16 );
    check( rn_region_send_alloc( region, config.max_message_bytes + 1 ) == RN_DESCRIPTOR_NONE );   // too large is a refusal, never a store attempt
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rn_region_send_alloc( region, 0 ) == RN_DESCRIPTOR_NONE );
    const uint32_t large = rn_send_store_free_chunks( &region->store, RN_STORE_LARGE );
    const uint32_t blocks = large / 29;
    for ( uint32_t i = 0; i < blocks; i++ )
        check( rn_region_send_alloc( region, 32768 ) != RN_DESCRIPTOR_NONE );
    check( rn_region_send_alloc( region, 32768 ) == RN_DESCRIPTOR_NONE );
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 1 );
    check( region->descriptors_free == 16 - blocks );

    // a carve gives everything back

    rn_region_carve( region );
    check( region->descriptors_free == 16 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_LARGE ) == large );

    rn_arena_destroy( arena );
}

static void test_channels_allocator_arena()
{
    // one allocation per arena, at create; none at carve; one free at destroy

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    const uint64_t mallocs = rn_hook_mallocs;
    const uint64_t frees = rn_hook_frees;
    rn_arena_t * arena = rn_arena_create( NULL, &config, 16, &counters );
    check( arena );
    check( rn_hook_mallocs == mallocs + 1 );
    check( rn_arena_num_regions( arena ) == 16 );
    check( rn_arena_bytes( arena ) >= 16 * rocketnet_message_connection_bytes( &config ) );
    check( rn_arena_bytes( arena ) < 16 * rocketnet_message_connection_bytes( &config ) + 65536 );

    for ( int i = 0; i < 16; i++ )
    {
        rn_region_t * region = rn_arena_region( arena, i );
        check( region );
        check( region->bytes == rocketnet_message_connection_bytes( &config ) );
        check( ( (uintptr_t) region->base & ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) ) == 0 );
        if ( i > 0 )
            check( rn_arena_region( arena, i - 1 )->base + rn_arena_region( arena, i - 1 )->bytes <= region->base );
        rn_region_carve( region );

        // every typed carve-out lies inside its own region, and the ring is 16-aligned
        check( rn_region_contains( region, region->channel_send, sizeof(rn_channel_send_t) * (uint64_t) region->num_channels ) );
        check( rn_region_contains( region, region->channel_receive, sizeof(rn_channel_receive_t) * (uint64_t) region->num_channels ) );
        check( rn_region_contains( region, region->send_numbers, sizeof(rn_send_number_t) * (uint64_t) region->window ) );
        check( rn_region_contains( region, region->receive_descriptors, sizeof(rn_receive_descriptor_t) * (uint64_t) region->window ) );
        check( rn_region_contains( region, region->received_bits, region->window / 8 ) );
        check( rn_region_contains( region, region->held_index, sizeof(rn_held_entry_t) * (uint64_t) region->held_entries ) );
        check( rn_region_contains( region, region->receive_ring, region->ring_bytes ) );
        check( ( (uintptr_t) region->receive_ring & 15 ) == 0 );
        check( rn_region_contains( region, region->send_descriptors, sizeof(rn_send_descriptor_t) * (uint64_t) region->queue ) );
        check( rn_region_contains( region, region->sent_packets, sizeof(rn_sent_packet_t) * (uint64_t) region->packets ) );
        check( rn_region_contains( region, region->sent_index, sizeof(rn_sent_index_t) * (uint64_t) region->sent_index_entries ) );
        check( rn_region_contains( region, region->sent_refs, sizeof(uint32_t) * (uint64_t) region->sent_ref_entries ) );
        check( rn_region_contains( region, region->deliveries, sizeof(rn_delivery_t) * (uint64_t) region->delivery_entries ) );
        check( rn_region_contains( region, region->sequenced_holds, sizeof(rn_sequenced_hold_t) * (uint64_t) region->holds ) );
        check( rn_region_contains( region, region->receipts, sizeof(rn_receipt_t) * (uint64_t) region->receipt_entries ) );
        check( !rn_region_contains( region, region->base + region->bytes, 1 ) );
        check( !rn_region_contains( region, region->base - 1, 1 ) );
        check( region->held_entries == 2 * region->window );
        check( region->sent_index_entries == 2 * region->packets );
        check( region->sent_ref_entries == 16 * region->packets );
        check( region->delivery_entries == region->window + (uint32_t) config.unreliable_deliveries );
    }

    // the receiver's ring bounds check

    rn_region_t * region = rn_arena_region( arena, 0 );
    check( rn_region_ring_span_valid( region, 0, 16 ) );
    check( rn_region_ring_span_valid( region, 65536 - 1152, 1152 ) );
    check( !rn_region_ring_span_valid( region, 65536 - 1152, 1153 ) );
    check( !rn_region_ring_span_valid( region, 65536, 1 ) );
    check( !rn_region_ring_span_valid( region, 8, 16 ) );                  // placements are multiples of 16
    check( !rn_region_ring_span_valid( region, 0xFFFFFFFFFFFFFFF0ULL, 32 ) ); // no wrap past 2^64

    rn_arena_destroy( arena );
    check( rn_hook_mallocs == mallocs + 1 );
    check( rn_hook_frees == frees + 1 );

    // a refused config allocates nothing

    config.window_messages = 1000;
    check( rn_arena_create( NULL, &config, 1, &counters ) == NULL );
    check( rn_hook_mallocs == mallocs + 1 );
    rocketnet_message_default_config( &config );
    check( rn_arena_create( NULL, &config, 0, &counters ) == NULL );
    check( rn_hook_mallocs == mallocs + 1 );
}

static void test_channels_allocator_sizing()
{
    // the formula and the region, printed before any connect; the buffer form allocates nothing

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    static char text[4096];
    rn_trap_phase = "sizing";
    rn_trap_armed = 1;
    const int length = rocketnet_message_connection_sizing( &config, 1000, text, (int) sizeof(text) );
    rn_trap_armed = 0;
    check( length > 0 && length < (int) sizeof(text) );
    check( (int) strlen( text ) == length );
    check( strstr( text, "48C + 96.125W + B + 64Q + S + 128P + 24(W + Du) + 1208Ru + 16Rq" ) != NULL );
    check( strstr( text, "region bytes 449024" ) != NULL );
    check( strstr( text, "formula bytes 449024" ) != NULL );
    check( strstr( text, "1000 connections 449024000" ) != NULL );

    // too small a buffer truncates and still reports the full length, as snprintf does

    char small[32];
    check( rocketnet_message_connection_sizing( &config, 1, small, (int) sizeof(small) ) == rocketnet_message_connection_sizing( &config, 1, NULL, 0 ) );
    check( strlen( small ) == sizeof(small) - 1 );

    // a refused config prints nothing and returns 0

    config.receive_ring_bytes = 100;
    check( rocketnet_message_connection_sizing( &config, 1, text, (int) sizeof(text) ) == 0 );
    check( text[0] == '\0' );

    // the print form logs the same lines at info level and returns the region bytes

    rocketnet_message_default_config( &config );
    const int old_level = rocketnet_get_log_level();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    const int lines_before = rn_log_lines;
    check( rocketnet_message_print_connection_sizing( &config, 16 ) == 449024 );
    rocketnet_log_level( old_level );
    check( rn_log_lines - lines_before >= 10 );
    check( strstr( rn_log_last, "16 connections" ) != NULL );
}

static void test_channels_allocator_trap_witness()
{
    // the trap fires: each net, armed in a child, ends it with exit 1

#if !defined(_WIN32)
    for ( int net = 0; net < 4; net++ )
    {
        if ( net == 2 && !RN_TEST_TRAP_LIBC )
            continue;
        fflush( stdout );
        fflush( stderr );
        const pid_t pid = fork();
        check( pid >= 0 );
        if ( pid == 0 )
        {
            // the child's trap line is the witness's expected output; keep the parent's stderr clean
            FILE * devnull = freopen( "/dev/null", "w", stderr );
            (void) devnull;
            rn_trap_phase = "witness";
            rn_trap_armed = 1;
            // volatile function pointers: a compiler may elide a malloc/free pair it can see
            void * (* volatile allocate)( size_t ) = malloc;
            void (* volatile release)( void * ) = free;
            switch ( net )
            {
                case 0: rocketnet_free( NULL, rocketnet_malloc( NULL, 8 ) ); break;
                case 1: { int * volatile p = new int( 1 ); delete p; } break;
                case 2: release( allocate( 8 ) ); break;
                case 3: { rocketnet_counters_t counters; rocketnet_message_config_t config; rocketnet_message_default_config( &config ); rn_arena_destroy( rn_arena_create( NULL, &config, 1, &counters ) ); } break;
            }
            _Exit( 0 );
        }
        int status = 0;
        check( waitpid( pid, &status, 0 ) == pid );
        check( WIFEXITED( status ) && WEXITSTATUS( status ) == 1 );
    }
#endif // #if !defined(_WIN32)
}

// ----------------------------------------------------------------
//                  the soak: send and receive at full load
// ----------------------------------------------------------------

// Layers 2 to 8 do not exist yet; this stands in for them by driving every
// carve-out of layer 1 to its configured capacity, the way they will
// (docs/CHANNELS.md 4, 6.1, 7.1, 9.1). Per tick, per connection, each way:
// sends until the descriptor pool or the store refuses, a write of every
// fragment from its store chunk into a frame with its number and sent-map
// entry, the receive side's descriptor, bitset, ring copy, held index,
// deliveries, holds and receipts, then acks of a random part and frees.
// The receive side checks every byte. Every refusal must equal its counter.
// A reconnect (a carve) every 50th tick. The trap is armed throughout.

struct rn_soak_side_t
{
    rn_region_t * sender;
    rn_region_t * receiver;
    rocketnet_counters_t * sender_counters;
    const rocketnet_message_config_t * config;
    uint64_t next_number;           // the next message number this side gives
    uint64_t oldest_unacked;
    uint64_t receiver_next;         // R: the lowest number not yet received
    uint64_t ring_next;             // the sender's absolute ring position
    uint64_t next_packet_id;
    uint32_t sent_head, sent_count; // the sent-packet ring, oldest first
    uint32_t ref_next;
    uint32_t serial;
    uint32_t receipt_head, receipt_count;
    uint64_t refused_store, refused_queue;
    uint64_t sent_bytes, delivered_messages, delivered_bytes, acked_fragments;
};

static void rn_soak_check_accounting( const rn_region_t * region )
{
    // free + used = total, for the store's classes and the descriptor pool

    uint32_t used[RN_STORE_NUM_CLASSES] = { 0, 0, 0 };
    uint32_t descriptors_used = 0;
    for ( uint32_t d = 0; d < region->queue; d++ )
    {
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        if ( !descriptor->in_use )
            continue;
        descriptors_used++;
        for ( uint32_t chunk = descriptor->chunk; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &region->store, chunk ) )
            used[rn_send_store_chunk_class( chunk )]++;
    }
    check( descriptors_used + region->descriptors_free == region->queue );
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        check( used[c] + rn_send_store_free_chunks( &region->store, c ) == rn_send_store_total_chunks( &region->store, c ) );
}

static uint32_t rn_held_slot( const rn_region_t * region, uint64_t key )
{
    uint32_t slot = (uint32_t) ( ( key * 0x9E3779B97F4A7C15ULL ) >> 40 ) & ( region->held_entries - 1 );
    while ( region->held_index[slot].used && region->held_index[slot].key != key )
        slot = ( slot + 1 ) & ( region->held_entries - 1 );
    return slot;
}

static uint32_t rn_sent_index_slot( const rn_region_t * region, uint64_t packet_id )
{
    uint32_t slot = (uint32_t) ( packet_id * 2654435761u ) % region->sent_index_entries;
    while ( region->sent_index[slot].used && region->sent_index[slot].packet_id != packet_id )
        slot = ( slot + 1 ) % region->sent_index_entries;
    return slot;
}

static void rn_soak_reset( rn_soak_side_t * side )
{
    side->next_number = 0;
    side->oldest_unacked = 0;
    side->receiver_next = 0;
    side->ring_next = 0;
    side->sent_head = 0;
    side->sent_count = 0;
    side->ref_next = 0;
    side->receipt_head = 0;
    side->receipt_count = 0;
}

static void rn_soak_send( rn_soak_side_t * side )
{
    rn_region_t * region = side->sender;
    const rocketnet_message_config_t * config = side->config;

    // send until refused: mostly small, some medium, some fragment-sized, some blocks up to the maximum

    for ( ;; )
    {
        const uint32_t pick = rn_random() % 16;
        int bytes;
        if ( pick < 8 )
            bytes = 1 + (int) ( rn_random() % RN_STORE_SMALL_BYTES );
        else if ( pick < 12 )
            bytes = RN_STORE_SMALL_BYTES + 1 + (int) ( rn_random() % ( RN_STORE_MEDIUM_BYTES - RN_STORE_SMALL_BYTES ) );
        else if ( pick < 15 )
            bytes = RN_STORE_MEDIUM_BYTES + 1 + (int) ( rn_random() % ( RN_STORE_LARGE_BYTES - RN_STORE_MEDIUM_BYTES ) );
        else
            bytes = RN_STORE_LARGE_BYTES + 1 + (int) ( rn_random() % (uint32_t) ( config->max_message_bytes - RN_STORE_LARGE_BYTES ) );

        const uint64_t store_full = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
        const uint64_t queue_full = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL );
        const uint32_t d = rn_region_send_alloc( region, bytes );
        if ( d == RN_DESCRIPTOR_NONE )
        {
            const uint64_t store_now = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
            const uint64_t queue_now = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL );
            check( ( store_now - store_full ) + ( queue_now - queue_full ) == 1 );       // exactly one reason, counted once
            side->refused_store += store_now - store_full;
            side->refused_queue += queue_now - queue_full;
            break;
        }

        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        check( descriptor->bytes == (uint32_t) bytes );
        descriptor->serial = ++side->serial;
        descriptor->reliability = ROCKETNET_RELIABLE_ORDERED;
        descriptor->priority = (uint8_t) ( rn_random() % 4 );
        descriptor->channel = (uint16_t) ( rn_random() % region->num_channels );

        // copy the message into its chunks, as send_message will

        uint32_t offset = 0;
        for ( uint32_t chunk = descriptor->chunk; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &region->store, chunk ) )
        {
            uint8_t * data = rn_send_store_chunk_data( &region->store, chunk );
            check( rn_region_contains( region, data, 1 ) );
            const uint32_t take = (uint32_t) bytes - offset < (uint32_t) RN_STORE_LARGE_BYTES ? (uint32_t) bytes - offset : (uint32_t) RN_STORE_LARGE_BYTES;
            for ( uint32_t b = 0; b < take; b++ )
                data[b] = rn_pattern( descriptor->serial, offset + b );
            offset += take;
        }
        check( offset == (uint32_t) bytes );

        // queue it on its channel's FIFO (layer 6's fields)

        rn_channel_send_t * channel = &region->channel_send[descriptor->channel];
        descriptor->next = RN_DESCRIPTOR_NONE;
        if ( channel->queue_tail == RN_DESCRIPTOR_NONE )
            channel->queue_head = d;
        else
            region->send_descriptors[channel->queue_tail].next = d;
        channel->queue_tail = d;
        channel->queued[descriptor->priority]++;
        descriptor->ordering = channel->ordered_write++;
        channel->sequenced_write = 0;
    }
}

static void rn_soak_write_and_receive( rn_soak_side_t * side, uint64_t tick )
{
    rn_region_t * region = side->sender;
    rn_region_t * receiver = side->receiver;
    uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];

    for ( uint32_t c = 0; c < region->num_channels; c++ )
    {
        rn_channel_send_t * channel = &region->channel_send[c];
        while ( channel->queue_head != RN_DESCRIPTOR_NONE )
        {
            const uint32_t d = channel->queue_head;
            rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
            const uint32_t fragments = ( descriptor->bytes + RN_STORE_LARGE_BYTES - 1 ) / RN_STORE_LARGE_BYTES;
            const uint64_t span = (uint64_t) fragments * RN_STORE_LARGE_BYTES;

            // the window's two bounds (6.2): the count, and the ring bytes

            if ( side->next_number + fragments > side->oldest_unacked + region->window )
                return;
            uint64_t ring = side->ring_next;
            if ( ring / region->ring_bytes != ( ring + span - 1 ) / region->ring_bytes )
                ring = ( ring / region->ring_bytes + 1 ) * region->ring_bytes;
            if ( side->sent_count == region->packets || side->ref_next + fragments > region->sent_ref_entries )
                return;

            channel->queue_head = descriptor->next;
            if ( channel->queue_head == RN_DESCRIPTOR_NONE )
                channel->queue_tail = RN_DESCRIPTOR_NONE;
            channel->queued[descriptor->priority]--;
            descriptor->first_number = side->next_number;
            descriptor->fragments = (uint16_t) fragments;
            descriptor->fragments_acked = 0;
            side->ring_next = ring + span;
            side->sent_bytes += descriptor->bytes;

            // one packet per message: a sent-map entry, its index slot and its references

            const uint32_t entry_index = ( side->sent_head + side->sent_count ) % region->packets;
            rn_sent_packet_t * entry = &region->sent_packets[entry_index];
            entry->packet_id = ++side->next_packet_id;
            entry->send_time = (double) tick;
            entry->ref_first = side->ref_next;
            entry->ref_count = (uint16_t) fragments;
            entry->unreliable_receipts = 0;
            side->sent_count++;
            const uint32_t index_slot = rn_sent_index_slot( region, entry->packet_id );
            check( !region->sent_index[index_slot].used );
            region->sent_index[index_slot].packet_id = entry->packet_id;
            region->sent_index[index_slot].entry = entry_index;
            region->sent_index[index_slot].used = 1;

            uint32_t chunk = descriptor->chunk;
            for ( uint32_t f = 0; f < fragments; f++ )
            {
                const uint64_t number = side->next_number++;
                region->sent_refs[side->ref_next++] = (uint32_t) number;

                rn_send_number_t * slot = &region->send_numbers[number & ( region->window - 1 )];
                check( slot->descriptor == RN_DESCRIPTOR_NONE || slot->acked );     // a live number is never overwritten
                slot->number = number;
                slot->descriptor = d;
                slot->fragment = (uint16_t) f;
                slot->ring_start = ring + (uint64_t) f * RN_STORE_LARGE_BYTES;
                slot->ring_units = RN_STORE_LARGE_BYTES / 16;
                slot->last_send = (double) tick;
                slot->acked = 0;

                // the write: from the chunk straight into the frame

                const uint32_t offset = f * RN_STORE_LARGE_BYTES;
                const uint32_t bytes = descriptor->bytes - offset < (uint32_t) RN_STORE_LARGE_BYTES ? descriptor->bytes - offset : (uint32_t) RN_STORE_LARGE_BYTES;
                check( chunk != RN_CHUNK_NONE );
                memcpy( frame, rn_send_store_chunk_data( &region->store, chunk ), bytes );
                chunk = rn_send_store_chunk_next( &region->store, chunk );

                // the receive: the window check, the bitset, the descriptor, the ring copy

                check( number >= side->receiver_next && number < side->receiver_next + receiver->window );
                const uint32_t r = (uint32_t) ( number & ( receiver->window - 1 ) );
                uint64_t * word = &receiver->received_bits[r / 64];
                check( ( *word & ( 1ULL << ( r % 64 ) ) ) == 0 );
                *word |= 1ULL << ( r % 64 );
                rn_receive_descriptor_t * rd = &receiver->receive_descriptors[r];
                check( !rd->live || rd->number == number );
                rd->number = number;
                rd->live = 1;
                rd->code = ROCKETNET_RELIABLE_ORDERED;
                rd->channel = (uint16_t) c;
                rd->ordering = descriptor->ordering;
                rd->sequencing = 0;
                rd->ring = (uint32_t) ( slot->ring_start % receiver->ring_bytes );
                rd->bytes = bytes;
                rd->block_have = (uint16_t) ( f + 1 );
                rd->block_count = (uint16_t) fragments;
                check( rn_region_ring_span_valid( receiver, rd->ring, bytes ) );
                memcpy( receiver->receive_ring + rd->ring, frame, bytes );
                for ( uint32_t b = 0; b < bytes; b++ )
                    check( receiver->receive_ring[rd->ring + b] == rn_pattern( descriptor->serial, offset + b ) );

                // the held index: ordered messages behind their predecessor are held, then drained

                const uint64_t key = ( (uint64_t) c << 32 ) | descriptor->ordering;
                const uint32_t held = rn_held_slot( receiver, key );
                receiver->held_index[held].key = key;
                receiver->held_index[held].number_slot = r;
                receiver->held_index[held].used = 1;
            }

            // the message is whole at the receiver: deliver from the ring, release the hold

            rn_channel_receive_t * rc = &receiver->channel_receive[c];
            check( rc->ordered_read == descriptor->ordering );
            const uint64_t key = ( (uint64_t) c << 32 ) | descriptor->ordering;
            const uint32_t held = rn_held_slot( receiver, key );
            check( receiver->held_index[held].used );
            receiver->held_index[held].used = 0;
            receiver->held_index[held].key = 0;
            rc->ordered_read = descriptor->ordering + 1;
            rc->sequenced_watermark = 0;

            const uint32_t delivery_index = (uint32_t) ( side->delivered_messages % receiver->delivery_entries );
            rn_delivery_t * delivery = &receiver->deliveries[delivery_index];
            delivery->data = receiver->receive_ring + ( ring % receiver->ring_bytes );
            delivery->bytes = (int32_t) descriptor->bytes;
            delivery->channel = (uint16_t) c;
            delivery->code = ROCKETNET_RELIABLE_ORDERED;
            delivery->serial = descriptor->serial;
            check( rn_region_contains( receiver, delivery->data, descriptor->bytes ) );
            for ( uint32_t b = 0; b < descriptor->bytes; b += 97 )
                check( delivery->data[b] == rn_pattern( descriptor->serial, b ) );
            side->delivered_messages++;
            side->delivered_bytes += descriptor->bytes;

            // retire the receive slots and advance R

            for ( uint32_t f = 0; f < fragments; f++ )
            {
                const uint32_t r = (uint32_t) ( ( descriptor->first_number + f ) & ( receiver->window - 1 ) );
                receiver->receive_descriptors[r].live = 0;
            }
            for ( ;; )
            {
                const uint32_t r = (uint32_t) ( side->receiver_next & ( receiver->window - 1 ) );
                uint64_t * word = &receiver->received_bits[r / 64];
                if ( ( *word & ( 1ULL << ( r % 64 ) ) ) == 0 )
                    break;
                *word &= ~( 1ULL << ( r % 64 ) );
                side->receiver_next++;
            }

            // a sequenced hold and a receipt per message, round the rings

            if ( receiver->holds > 0 )
            {
                rn_sequenced_hold_t * hold = &receiver->sequenced_holds[descriptor->serial % receiver->holds];
                const uint32_t bytes = descriptor->bytes < (uint32_t) sizeof(hold->data) ? descriptor->bytes : (uint32_t) sizeof(hold->data);
                memcpy( hold->data, delivery->data, bytes );
                hold->bytes = bytes;
                hold->channel = (uint16_t) c;
                hold->ordering = descriptor->ordering;
                hold->sequencing = 0;
                hold->used = 1;
                check( hold->data[0] == rn_pattern( descriptor->serial, 0 ) );
            }
        }
    }
}

static void rn_soak_ack( rn_soak_side_t * side, uint64_t tick, bool all )
{
    rn_region_t * region = side->sender;

    // ack the older half always, and a random part of the rest; free a message when every fragment is acked

    uint32_t acked_packets = 0;
    for ( uint32_t i = 0; i < side->sent_count; i++ )
    {
        rn_sent_packet_t * entry = &region->sent_packets[( side->sent_head + i ) % region->packets];
        if ( entry->packet_id == 0 )
            continue;
        if ( !all && entry->send_time >= (double) tick && ( rn_random() & 1 ) )
            continue;
        const uint32_t index_slot = rn_sent_index_slot( region, entry->packet_id );
        check( region->sent_index[index_slot].used && region->sent_index[index_slot].packet_id == entry->packet_id );
        for ( uint32_t r = 0; r < entry->ref_count; r++ )
        {
            const uint64_t number = region->sent_refs[entry->ref_first + r];
            rn_send_number_t * slot = &region->send_numbers[number & ( region->window - 1 )];
            check( (uint32_t) slot->number == (uint32_t) number && !slot->acked );
            slot->acked = 1;
            side->acked_fragments++;
            rn_send_descriptor_t * descriptor = &region->send_descriptors[slot->descriptor];
            check( descriptor->in_use );
            if ( ++descriptor->fragments_acked == descriptor->fragments )
            {
                if ( side->receipt_count == region->receipt_entries )
                {
                    side->receipt_head = ( side->receipt_head + 1 ) % region->receipt_entries;
                    side->receipt_count--;
                }
                rn_receipt_t * receipt = &region->receipts[( side->receipt_head + side->receipt_count ) % region->receipt_entries];
                receipt->serial = descriptor->serial;
                receipt->status = ROCKETNET_RECEIPT_ACKED;
                side->receipt_count++;
                rn_region_send_free( region, slot->descriptor );
            }
        }
        entry->packet_id = 0;
        acked_packets++;
    }

    // compact: pop acked entries off the head, rebuild the index and the reference ring from what remains

    for ( uint32_t s = 0; s < region->sent_index_entries; s++ )
        region->sent_index[s].used = 0;
    uint32_t kept = 0;
    uint32_t refs = 0;
    static uint32_t scratch_refs[1 << 16];
    for ( uint32_t i = 0; i < side->sent_count; i++ )
    {
        rn_sent_packet_t * entry = &region->sent_packets[( side->sent_head + i ) % region->packets];
        if ( entry->packet_id == 0 )
            continue;
        for ( uint32_t r = 0; r < entry->ref_count; r++ )
            scratch_refs[refs + r] = region->sent_refs[entry->ref_first + r];
        rn_sent_packet_t moved = *entry;
        moved.ref_first = refs;
        refs += moved.ref_count;
        const uint32_t to = ( side->sent_head + kept ) % region->packets;
        region->sent_packets[to] = moved;
        const uint32_t index_slot = rn_sent_index_slot( region, moved.packet_id );
        region->sent_index[index_slot].packet_id = moved.packet_id;
        region->sent_index[index_slot].entry = to;
        region->sent_index[index_slot].used = 1;
        kept++;
    }
    check( refs <= region->sent_ref_entries );
    memcpy( region->sent_refs, scratch_refs, refs * sizeof(uint32_t) );
    side->sent_count = kept;
    side->ref_next = refs;

    // advance the oldest unacked number

    while ( side->oldest_unacked < side->next_number && region->send_numbers[side->oldest_unacked & ( region->window - 1 )].acked )
        side->oldest_unacked++;
    (void) acked_packets;
}

static void test_channels_allocator_no_live_allocation()
{
    // A server arena of 16 slots at the defaults, and 16 client arenas, one
    // region each, as 16 client objects have. The clients share the server's
    // wire fields but run a short send queue, so between the two ends both
    // refusals are reached: at the defaults the store's 451 chunks run out
    // before the 1024 descriptors can (docs/CHANNELS.md 14), so the server
    // end is store-bound and the client end is queue-bound.

    const int num_slots = 16;
    rocketnet_message_config_t server_config;
    rocketnet_message_default_config( &server_config );
    rocketnet_message_config_t client_config = server_config;
    client_config.send_queue_messages = 64;

    static rocketnet_counters_t server_counters;
    static rocketnet_counters_t client_counters[16];
    memset( &server_counters, 0, sizeof(server_counters) );
    memset( client_counters, 0, sizeof(client_counters) );
    rn_arena_t * server = rn_arena_create( NULL, &server_config, num_slots, &server_counters );
    check( server );
    rn_arena_t * clients[16];
    for ( int s = 0; s < num_slots; s++ )
    {
        clients[s] = rn_arena_create( NULL, &client_config, 1, &client_counters[s] );
        check( clients[s] );
    }

    // even sides send server to client, odd sides client to server

    static rn_soak_side_t sides[2 * 16];
    memset( sides, 0, sizeof(sides) );
    for ( int s = 0; s < num_slots; s++ )
    {
        sides[2 * s].sender = rn_arena_region( server, s );
        sides[2 * s].receiver = rn_arena_region( clients[s], 0 );
        sides[2 * s].sender_counters = &server_counters;
        sides[2 * s].config = &server_config;
        sides[2 * s + 1].sender = rn_arena_region( clients[s], 0 );
        sides[2 * s + 1].receiver = rn_arena_region( server, s );
        sides[2 * s + 1].sender_counters = &client_counters[s];
        sides[2 * s + 1].config = &client_config;
    }

    const uint64_t hook_mallocs = rn_hook_mallocs;
    const uint64_t hook_frees = rn_hook_frees;

    rn_trap_phase = "connect";
    rn_trap_armed = 1;

    // connect: every region is carved, allocating nothing

    for ( int s = 0; s < num_slots; s++ )
    {
        rn_region_carve( rn_arena_region( server, s ) );
        rn_region_carve( rn_arena_region( clients[s], 0 ) );
    }

    const int ticks = 60;
    uint64_t time = 0;
    for ( int tick = 1; tick <= ticks; tick++ )
    {
        for ( int round = 0; round < 2; round++ )
        {
            time++;
            for ( int i = 0; i < 2 * num_slots; i++ )
            {
                rn_trap_phase = "send";
                rn_soak_send( &sides[i] );
            }
            for ( int i = 0; i < 2 * num_slots; i++ )
            {
                rn_trap_phase = "receive";
                rn_soak_write_and_receive( &sides[i], time );
                rn_soak_check_accounting( sides[i].sender );
            }
            for ( int i = 0; i < 2 * num_slots; i++ )
            {
                rn_trap_phase = "ack";
                rn_soak_ack( &sides[i], time, false );
                rn_soak_check_accounting( sides[i].sender );
            }
        }

        // a reconnect of every connection every 20th tick, with sends queued
        // and in flight: the carve takes everything back

        if ( tick % 20 == 0 )
        {
            rn_trap_phase = "reconnect";
            for ( int s = 0; s < num_slots; s++ )
            {
                rn_soak_send( &sides[2 * s] );
                rn_soak_send( &sides[2 * s + 1] );
                rn_region_t * ends[2] = { rn_arena_region( server, s ), rn_arena_region( clients[s], 0 ) };
                for ( int e = 0; e < 2; e++ )
                {
                    check( ends[e]->descriptors_free < ends[e]->queue );
                    rn_region_carve( ends[e] );
                    check( ends[e]->descriptors_free == ends[e]->queue );
                    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
                        check( rn_send_store_free_chunks( &ends[e]->store, c ) == rn_send_store_total_chunks( &ends[e]->store, c ) );
                    rn_soak_check_accounting( ends[e] );
                }
                rn_soak_reset( &sides[2 * s] );
                rn_soak_reset( &sides[2 * s + 1] );
            }
        }
    }

    // drain: write what is queued, ack everything, and every store and pool is whole again

    rn_trap_phase = "drain";
    for ( int pass = 0; pass < 64; pass++ )
    {
        time++;
        bool queued = false;
        for ( int i = 0; i < 2 * num_slots; i++ )
        {
            rn_soak_write_and_receive( &sides[i], time );
            rn_soak_ack( &sides[i], time, true );
            for ( uint32_t c = 0; c < sides[i].sender->num_channels; c++ )
                if ( sides[i].sender->channel_send[c].queue_head != RN_DESCRIPTOR_NONE )
                    queued = true;
        }
        if ( !queued )
            break;
    }
    for ( int i = 0; i < 2 * num_slots; i++ )
    {
        const rn_region_t * region = sides[i].sender;
        check( sides[i].sent_count == 0 );
        check( region->descriptors_free == region->queue );
        for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
            check( rn_send_store_free_chunks( &region->store, c ) == rn_send_store_total_chunks( &region->store, c ) );
    }

    rn_trap_armed = 0;
    rn_trap_phase = "idle";
    check( rn_hook_mallocs == hook_mallocs && rn_hook_frees == hook_frees );

    // every refusal is its own counter, on its own owner, and the load reached
    // capacity on both ends: the store on the server's, the queue on the clients'

    uint64_t server_store = 0, server_queue = 0, client_store = 0, client_queue = 0;
    for ( int s = 0; s < num_slots; s++ )
    {
        server_store += sides[2 * s].refused_store;
        server_queue += sides[2 * s].refused_queue;
        client_store += sides[2 * s + 1].refused_store;
        client_queue += sides[2 * s + 1].refused_queue;
        check( sides[2 * s + 1].refused_store == rn_counter( &client_counters[s], ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) );
        check( sides[2 * s + 1].refused_queue == rn_counter( &client_counters[s], ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) );
        check( sides[2 * s].delivered_messages > 0 && sides[2 * s + 1].delivered_messages > 0 );
        check( sides[2 * s].delivered_bytes == sides[2 * s].sent_bytes );
        check( sides[2 * s + 1].delivered_bytes == sides[2 * s + 1].sent_bytes );
    }
    check( server_store == rn_counter( &server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) );
    check( server_queue == rn_counter( &server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) );
    check( server_store > 0 && server_queue == 0 );
    check( client_queue > 0 );

    for ( int s = 0; s < num_slots; s++ )
        rn_arena_destroy( clients[s] );
    rn_arena_destroy( server );
}

// ----------------------------------------------------------------

struct rn_test_case_t
{
    const char * name;
    const char * alias;
    void (*function)();
};

static const rn_test_case_t rn_test_cases[] =
{
    { "test_channels_allocator_layout",             "ChannelsAllocatorLayout",            test_channels_allocator_layout },
    { "test_channels_allocator_validation",         "ChannelsAllocatorValidation",        test_channels_allocator_validation },
    { "test_channels_allocator_send_store",         "ChannelsAllocatorSendStore",         test_channels_allocator_send_store },
    { "test_channels_allocator_descriptor_pool",    "ChannelsAllocatorDescriptorPool",    test_channels_allocator_descriptor_pool },
    { "test_channels_allocator_arena",              "ChannelsAllocatorArena",             test_channels_allocator_arena },
    { "test_channels_allocator_sizing",             "ChannelsAllocatorSizing",            test_channels_allocator_sizing },
    { "test_channels_allocator_trap_witness",       "ChannelsAllocatorTrapWitness",       test_channels_allocator_trap_witness },
    { "test_channels_allocator_no_live_allocation", "ChannelsAllocatorNoLiveAllocation",  test_channels_allocator_no_live_allocation },
};

static const int rn_num_test_cases = (int) ( sizeof(rn_test_cases) / sizeof(rn_test_cases[0]) );

int main( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );

    // the hooks go in before init, and the library's log is captured (a passing run prints names only)

    rocketnet_allocator( rn_test_malloc_hook, rn_test_free_hook );
    rocketnet_log_function( rn_test_log_function );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    if ( !rocketnet_init() )
    {
        fprintf( stderr, "error: failed to initialize rocketnet\n" );
        return 1;
    }

    const char * only = NULL;
    if ( argc >= 3 && strcmp( argv[1], "--run" ) == 0 )
        only = argv[2];
    else if ( argc >= 2 )
        only = argv[1];

    int ran = 0;
    for ( int i = 0; i < rn_num_test_cases; i++ )
    {
        if ( only && strcmp( only, rn_test_cases[i].name ) != 0 && strcmp( only, rn_test_cases[i].alias ) != 0 )
            continue;
        rn_log_last[0] = '\0';
        rn_test_cases[i].function();
        printf( "%s\n", rn_test_cases[i].name );
        ran++;
    }

    rocketnet_term();

    if ( ran == 0 )
    {
        fprintf( stderr, "error: no case named %s\n", only );
        return 1;
    }
    return 0;
}

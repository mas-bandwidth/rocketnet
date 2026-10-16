/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_packet_lease.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_base64.h"
#include "rocketnet_config.h"
#include "rocketnet_hash.h"
#include "rocketnet_packet_loss_tracker.h"
#include "rocketnet_out_of_order_tracker.h"
#include "rocketnet_jitter_tracker.h"

#include <sodium.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include <inttypes.h>
#if defined( _MSC_VER )
#include <malloc.h>
#endif // #if defined( _MSC_VER )
#include <time.h>
#include <atomic>
#include <mutex>
#include <new>

#if defined( _MSC_VER )
#pragma warning(push)
#pragma warning(disable:4996)
#pragma warning(disable:4127)
#pragma warning(disable:4244)
#pragma warning(disable:4668)
#endif

// -------------------------------------------------

void rocketnet_printf( const char * format, ... );

static void default_assert_function( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_error( "assert failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    #if defined(_MSC_VER)
        __debugbreak();
    #elif defined(__clang__)
        __builtin_debugtrap();
    #elif defined(__GNUC__)
        __builtin_trap();
    #elif defined(linux) || defined(__linux) || defined(__linux__) || defined(__APPLE__)
        raise(SIGTRAP);
    #else
        #error "asserts not supported on this platform!"
    #endif
}

void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line ) = default_assert_function;

void rocketnet_assert_function( void (*function)( const char * condition, const char * function, const char * file, int line ) )
{
    if ( function )
    {
        rocketnet_assert_function_pointer = function;
    }
    else
    {
        rocketnet_assert_function_pointer = default_assert_function;
    }
}

// -------------------------------------------------------------

static int log_quiet = 0;

void rocketnet_quiet( bool flag )
{
   log_quiet = flag;
}

static int log_level = ROCKETNET_LOG_LEVEL_INFO;

void rocketnet_log_level( int level )
{
    // the defined levels are NONE (0) through SPAM (5): a value outside that
    // range is refused, leaving the level already in force. a caller that
    // hands over a typo or a parsed-out-of-range number must not silence the
    // library by accident.
    if ( level < ROCKETNET_LOG_LEVEL_NONE || level > ROCKETNET_LOG_LEVEL_SPAM )
        return;
    log_level = level;
}

// test_xdp and test_iocp compile this file without rocketnet_config.cpp, so
// they cannot link rocketnet_config_parse_int_value. the definition here is
// that function's whole-string integer parse: a leading minus is allowed, and
// a prefix, a plus and whitespace are refused. the library itself keeps the
// one definition in rocketnet_config.cpp.

#if defined( ROCKETNET_XDP_TESTS ) || defined( ROCKETNET_IOCP_TESTS )

bool rocketnet_config_parse_int_value( const char * value, int * out )
{
    const char * cursor = value;
    if ( cursor == NULL )
        return false;
    if ( *cursor == '-' )
        cursor++;
    if ( *cursor == '\0' )
        return false;
    for ( ; *cursor != '\0'; cursor++ )
    {
        if ( *cursor < '0' || *cursor > '9' )
            return false;
    }
    char * end = NULL;
    errno = 0;
    long long parsed = strtoll( value, &end, 10 );
    if ( end == value || *end != '\0' || errno == ERANGE )
        return false;
    if ( parsed < INT32_MIN || parsed > INT32_MAX )
        return false;
    *out = (int) parsed;
    return true;
}

#endif // #if defined( ROCKETNET_XDP_TESTS ) || defined( ROCKETNET_IOCP_TESTS )

// the ROCKETNET_LOG_LEVEL override's parse-and-apply step, factored out of
// rocketnet_init so the suite can drive it with a string directly. it is
// internal to the sdk, not part of the exported C surface. the value parses
// strictly, with the same whole-string integer parse the transport config
// overrides use: only a value inside NONE (0) .. SPAM (5) is applied, and
// anything else -- a word, the empty string, an out-of-range number -- keeps
// the current level and warns, naming the refused value, rather than letting
// a mistyped environment variable silence the library.

void rocketnet_log_level_override_apply( const char * value )
{
    int parsed;
    if ( rocketnet_config_parse_int_value( value, &parsed ) &&
         parsed >= ROCKETNET_LOG_LEVEL_NONE && parsed <= ROCKETNET_LOG_LEVEL_SPAM )
    {
        log_level = parsed;
        rocketnet_info( "log level overridden to %d", log_level );
        return;
    }
    rocketnet_warn( "ignoring ROCKETNET_LOG_LEVEL=\"%s\": not a level %d to %d, keeping %d",
                    value ? value : "", ROCKETNET_LOG_LEVEL_NONE, ROCKETNET_LOG_LEVEL_SPAM, log_level );
}

int rocketnet_get_log_level( void )
{
    return log_level;
}

const char * rocketnet_log_level_string( int level )
{
    if ( level == ROCKETNET_LOG_LEVEL_SPAM )
        return "spam";
    else if ( level == ROCKETNET_LOG_LEVEL_DEBUG )
        return "debug";
    else if ( level == ROCKETNET_LOG_LEVEL_INFO )
        return "info";
    else if ( level == ROCKETNET_LOG_LEVEL_ERROR )
        return "error";
    else if ( level == ROCKETNET_LOG_LEVEL_WARN )
        return "warning";
    else
        return "???";
}

static void default_log_function( int level, const char * format, ... )
{
#if !ROCKETNET_UNREAL_ENGINE     // IMPORTANT: Unreal doesn't like printf
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    if ( level != ROCKETNET_LOG_LEVEL_NONE )
    {
        if ( !log_quiet )
        {
            const char * level_string = rocketnet_log_level_string( level );
            printf( "%.6f: %s: %s\n", rocketnet_platform_time(), level_string, buffer );
        }
    }
    else
    {
        printf( "%s\n", buffer );
    }
    va_end( args );
    fflush( stdout );
#endif // #if !ROCKETNET_UNREAL_ENGINE
}

static void (*log_function)( int level, const char * format, ... ) = default_log_function;

void rocketnet_log_function( void (*function)( int level, const char * format, ... ) )
{
    if ( function )
    {
        log_function = function;
    }
    else
    {
        log_function = default_log_function;
    }
}

void rocketnet_printf( int level, const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;
    if ( level > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( level, "%s", buffer );
    va_end( args );
}

void rocketnet_info( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;
    if ( ROCKETNET_LOG_LEVEL_INFO > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_INFO, "%s", buffer );
    va_end( args );
}

void rocketnet_warn( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;
    if ( ROCKETNET_LOG_LEVEL_WARN > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_WARN, "%s", buffer );
    va_end( args );
}

void rocketnet_error( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;
    if ( ROCKETNET_LOG_LEVEL_ERROR > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_ERROR, "%s", buffer );
    va_end( args );
}

void rocketnet_debug( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;
    if ( ROCKETNET_LOG_LEVEL_DEBUG > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_DEBUG, "%s", buffer );
    va_end( args );
}

void rocketnet_spam( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;
    if ( ROCKETNET_LOG_LEVEL_SPAM > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_SPAM, "%s", buffer );
    va_end( args );
}

// ------------------------------------------------------------

void * rocketnet_default_malloc_function( void * context, size_t bytes )
{
    (void) context;
    return malloc( bytes );
}

void rocketnet_default_free_function( void * context, void * p )
{
    (void) context;
    free( p );
}

static void * (*rocketnet_malloc_function)( void * context, size_t bytes ) = rocketnet_default_malloc_function;
static void (*rocketnet_free_function)( void * context, void * p ) = rocketnet_default_free_function;

static std::mutex rocketnet_lifecycle_mutex;
static size_t rocketnet_allocations = 0; // includes allocator callbacks in flight
static bool rocketnet_initialized = false;
static bool rocketnet_transitioning = false;

// every refused API misuse counts here: a create before init, a late or partial
// allocator replacement, and any other call the SDK refuses. relaxed: the count
// is a diagnostic and orders against nothing.
static std::atomic<uint64_t> rocketnet_api_misuses{ 0 };

uint64_t rocketnet_api_misuse_count()
{
    return rocketnet_api_misuses.load( std::memory_order_relaxed );
}

void rocketnet_api_misuse_record()
{
    rocketnet_api_misuses.fetch_add( 1, std::memory_order_relaxed );
}

bool rocketnet_library_initialized()
{
    std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
    return rocketnet_initialized;
}

struct rocketnet_allocation_header_t
{
    void * base;
    void * context;
    void (*free_function)( void *, void * );
};

uint64_t rocketnet_allocation_overhead()
{
    return sizeof(rocketnet_allocation_header_t) + ROCKETNET_ALLOCATION_ALIGNMENT - 1;
}

bool rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) )
{
    bool refused;
    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
        refused = !malloc_function || !free_function || rocketnet_initialized || rocketnet_transitioning || rocketnet_allocations != 0;
        if ( !refused )
        {
            rocketnet_malloc_function = malloc_function;
            rocketnet_free_function = free_function;
        }
    }
    if ( refused )
    {
        rocketnet_error( "allocator replacement requires an uninitialized library with no live allocations" );
        rocketnet_api_misuse_record();
        return false;
    }
    return true;
}

void * rocketnet_malloc( void * context, size_t bytes )
{
    const size_t alignment = ROCKETNET_ALLOCATION_ALIGNMENT;
    const size_t overhead = (size_t) rocketnet_allocation_overhead();
    if ( bytes > SIZE_MAX - overhead )
        return NULL;
    void * (*allocate)( void *, size_t );
    void (*release)( void *, void * );
    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
        allocate = rocketnet_malloc_function;
        release = rocketnet_free_function;
        ++rocketnet_allocations;
    }
    void * base = allocate( context, bytes + overhead );
    if ( !base )
    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
        --rocketnet_allocations;
        return NULL;
    }
    const uintptr_t aligned = ( (uintptr_t) base + overhead ) & ~(uintptr_t)( alignment - 1 );
    auto * header = new ( (void*) ( aligned - sizeof(rocketnet_allocation_header_t) ) )
        rocketnet_allocation_header_t{ base, context, release };
    (void) header;
    return (void*) aligned;
}

void rocketnet_free( void * context, void * p )
{
    (void) context; // allocation ownership is captured at allocation time
    if ( !p )
        return;
    auto * header = (rocketnet_allocation_header_t*) ( (uint8_t*) p - sizeof(rocketnet_allocation_header_t) );
    const rocketnet_allocation_header_t allocation = *header;
    header->~rocketnet_allocation_header_t();
    allocation.free_function( allocation.context, allocation.base );
    std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
    --rocketnet_allocations;
}

double rocketnet_time()
{
    return rocketnet_platform_time();
}

void rocketnet_sleep( double seconds )
{
    rocketnet_platform_sleep( seconds );
}

void rocketnet_clear_and_free( void * context, void * p, size_t p_size )
{
    if ( !p )
        return;
    sodium_memzero( p, p_size );
    rocketnet_free( context, p );
}

// -------------------------------------------------------------

const char * rocketnet_user_id_string( uint64_t user_id, char * buffer, size_t buffer_size )
{
    if ( !rocketnet_api_null_guard( buffer ) )
        return NULL;
    snprintf( buffer, buffer_size, "%" PRIx64, user_id );
    return buffer;
}

uint64_t rocketnet_protocol_version()
{
#if !ROCKETNET_DEVELOPMENT
    #define VERSION_STRING(major,minor) #major #minor
    return rocketnet_hash_string( VERSION_STRING(ROCKETNET_VERSION_MAJOR_INT, ROCKETNET_VERSION_MINOR_INT) );
#else // #if !ROCKETNET_DEVELOPMENT
    return 0;
#endif // #if !ROCKETNET_DEVELOPMENT
}

float rocketnet_random_float()
{
    uint32_t uint32_value;
    randombytes_buf( &uint32_value, sizeof(uint32_value) );
    uint64_t uint64_value = uint64_t(uint32_value);
    double double_value = double(uint64_value) / 0xFFFFFFFF;
    return float(double_value);
}

uint8_t rocketnet_random_uint8()
{
    uint8_t value;
    randombytes_buf( &value, sizeof(value) );
    return value;
}

uint16_t rocketnet_random_uint16()
{
    uint16_t value;
    randombytes_buf( &value, sizeof(value) );
    return value;
}

uint32_t rocketnet_random_uint32()
{
    uint32_t value;
    randombytes_buf( &value, sizeof(value) );
    return value;
}

uint64_t rocketnet_random_uint64()
{
    uint64_t value;
    randombytes_buf( &value, sizeof(value) );
    return value;
}

void rocketnet_random_bytes( uint8_t * data, size_t bytes )
{
    if ( !rocketnet_api_null_guard( data ) )
        return;
    randombytes_buf( data, bytes );
}

// -------------------------------------------------------------

size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size )
{
    if ( !rocketnet_api_null_guard( dest ) )
        return 0;
    if ( !rocketnet_api_null_guard( source ) )
        return 0;
    if ( dest_size == 0 )
        return 0;
    memset( dest, 0, dest_size );
    size_t i = 0;
    for ( ; i < dest_size - 1; i++ )
    {
        if ( source[i] == '\0' )
            break;
        dest[i] = source[i];
    }
    return i;
}

// -------------------------------------------------------------

int rocketnet_signed_packets[256];

int rocketnet_encrypted_packets[256];

const char * rocketnet_platform_string( int platform_id )
{
    switch ( platform_id )
    {
        case ROCKETNET_PLATFORM_WINDOWS:       return "windows";
        case ROCKETNET_PLATFORM_MAC:           return "mac";
        case ROCKETNET_PLATFORM_LINUX:         return "linux";
        case ROCKETNET_PLATFORM_IOS:           return "ios";
        case ROCKETNET_PLATFORM_BSD:           return "bsd";
        default:
            break;
    }
    return "unknown";
}

const char * rocketnet_connection_string( int connection_type )
{
    switch ( connection_type )
    {
        case ROCKETNET_CONNECTION_TYPE_WIRED:    return "wired";
        case ROCKETNET_CONNECTION_TYPE_WIFI:     return "wi-fi";
        case ROCKETNET_CONNECTION_TYPE_CELLULAR: return "cellular";
        default:
            break;
    }
    return "unknown";
}

void * rocketnet_global_context = NULL;

bool rocketnet_init( void * context )
{
    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
        if ( rocketnet_initialized || rocketnet_transitioning )
            return false;
        rocketnet_transitioning = true;
    }

    // Platform discovery uses the caller's allocator context during startup.
    // No transport may be used until init returns true; every failure clears it.
    rocketnet_global_context = context;
    const bool platform_ready = rocketnet_platform_init();
    const bool sodium_ready = platform_ready && sodium_init() >= 0;
    if ( !sodium_ready )
    {
        if ( platform_ready )
            rocketnet_platform_term();
        rocketnet_global_context = NULL;
        {
            std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
            rocketnet_transitioning = false;
        }
        rocketnet_error( platform_ready ? "failed to initialize sodium" : "failed to initialize platform" );
        return false;
    }

    rocketnet_info( "rocketnet version is %s", ROCKETNET_VERSION_FULL );
    rocketnet_info( "platform is %s (%s)", rocketnet_platform_string( rocketnet_platform_id() ),
                    rocketnet_connection_string( rocketnet_platform_connection_type() ) );

    const char * log_level_override = rocketnet_platform_getenv( "ROCKETNET_LOG_LEVEL" );
    if ( log_level_override )
        rocketnet_log_level_override_apply( log_level_override );
    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
        rocketnet_initialized = true;
        rocketnet_transitioning = false;
    }
    return true;
}

void rocketnet_term()
{
    bool live_allocations = false;
    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
        if ( !rocketnet_initialized || rocketnet_transitioning )
            return;
        live_allocations = rocketnet_allocations != 0;
        if ( !live_allocations )
            rocketnet_transitioning = true;
    }
    if ( live_allocations )
    {
        rocketnet_error( "destroy all objects and allocations before terminating rocketnet" );
        return;
    }
    rocketnet_platform_term();
    rocketnet_global_context = NULL;
    std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );
    rocketnet_initialized = false;
    rocketnet_transitioning = false;
}

// ---------------------------------------------------------------

#ifdef _MSC_VER
#pragma warning(pop)
#endif

uint64_t rocketnet_next_packet_lease_id()
{
    static std::atomic<uint64_t> next{1};
    uint64_t ticket = next.load(std::memory_order_relaxed);
    while ( ticket != UINT64_MAX )
    {
        if ( next.compare_exchange_weak(ticket, ticket + 1, std::memory_order_relaxed) )
            return ticket;
    }
    return 0;
}

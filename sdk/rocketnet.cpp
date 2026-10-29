/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"
#include "rocketnet_packet_lease.h"
#include "rocketnet_platform.h"
#include "rocketnet_config.h"
#include "rocketnet_hash.h"

#include <sodium.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <atomic>
#include <mutex>

// -------------------------------------------------

static void default_assert_function( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_error( "assert failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );

    fflush( stdout );
#if defined( _MSC_VER )
    __debugbreak();
#elif defined( __clang__ )
    __builtin_debugtrap();
#elif defined( __GNUC__ )
    __builtin_trap();
#else
#error "asserts not supported on this platform!"
#endif // #if defined( _MSC_VER )
}

rocketnet_assert_function_t rn_assert_function = default_assert_function;

void rocketnet_assert_function( rocketnet_assert_function_t function )
{
    if ( function )
    {
        rn_assert_function = function;
    }
    else
    {
        rn_assert_function = default_assert_function;
    }
}

// -------------------------------------------------------------

static std::atomic<bool> log_quiet{ false };

void rocketnet_quiet( bool flag )
{
    log_quiet.store( flag, std::memory_order_relaxed );
}

static std::atomic<int> log_level{ ROCKETNET_LOG_LEVEL_INFO };

void rocketnet_log_level( enum rocketnet_log_level_t level )
{
    // a value outside NONE .. SPAM is ignored. read as an int: a C caller may
    // pass any int, and loading an out-of-range enum is undefined in C++

    int value;
    memcpy( &value, &level, sizeof( value ) );

    if ( value < ROCKETNET_LOG_LEVEL_NONE || value > ROCKETNET_LOG_LEVEL_SPAM )
        return;

    log_level.store( value, std::memory_order_relaxed );
}

// applies a ROCKETNET_LOG_LEVEL value: a whole integer in NONE .. SPAM sets
// the level, anything else keeps it and warns

void rocketnet_log_level_override_apply( const char * value )
{
    int parsed;
    if ( rocketnet_config_parse_int_value( value, &parsed ) &&
         parsed >= ROCKETNET_LOG_LEVEL_NONE && parsed <= ROCKETNET_LOG_LEVEL_SPAM )
    {
        log_level.store( parsed, std::memory_order_relaxed );

        rocketnet_info( "log level overridden to %d", log_level.load( std::memory_order_relaxed ) );
        return;
    }

    rocketnet_warn( "ignoring ROCKETNET_LOG_LEVEL=\"%s\": not a level %d to %d, keeping %d",
                    value ? value : "", ROCKETNET_LOG_LEVEL_NONE, ROCKETNET_LOG_LEVEL_SPAM, log_level.load( std::memory_order_relaxed ) );
}

void rocketnet_thread_priority( bool enabled )
{
    rocketnet_platform_set_thread_priority_enabled( enabled );
}

#if ROCKETNET_DEV
bool rocketnet_get_thread_priority( void )
{
    return rocketnet_platform_thread_priority_enabled();
}
#endif // #if ROCKETNET_DEV

uint64_t rocketnet_get_thread_priority_refusals( void )
{
    return rocketnet_platform_thread_priority_refusals();
}

enum rocketnet_log_level_t rocketnet_log_level_current( void )
{
    return (enum rocketnet_log_level_t) log_level.load( std::memory_order_relaxed );
}

const char * rocketnet_log_level_name( enum rocketnet_log_level_t level )
{
    int value;
    memcpy( &value, &level, sizeof( value ) );

    switch ( value )
    {
        case ROCKETNET_LOG_LEVEL_NONE:  return "NONE";
        case ROCKETNET_LOG_LEVEL_ERROR: return "ERROR";
        case ROCKETNET_LOG_LEVEL_WARN:  return "WARN";
        case ROCKETNET_LOG_LEVEL_INFO:  return "INFO";
        case ROCKETNET_LOG_LEVEL_DEBUG: return "DEBUG";
        case ROCKETNET_LOG_LEVEL_SPAM:  return "SPAM";
        default:                        return "UNKNOWN";
    }
}

static const char * rocketnet_log_level_word( enum rocketnet_log_level_t level )
{
    switch ( (int) level )
    {
        case ROCKETNET_LOG_LEVEL_ERROR: return "error";
        case ROCKETNET_LOG_LEVEL_WARN:  return "warn";
        case ROCKETNET_LOG_LEVEL_INFO:  return "info";
        case ROCKETNET_LOG_LEVEL_DEBUG: return "debug";
        case ROCKETNET_LOG_LEVEL_SPAM:  return "spam";
        default:                        return "none";
    }
}

const char * rocketnet_status_name( enum rocketnet_status_t status )
{
    int value;
    memcpy( &value, &status, sizeof( value ) );

    switch ( value )
    {
        case ROCKETNET_STATUS_OK:                               return "OK";
        case ROCKETNET_STATUS_ALREADY_INITIALIZED:              return "ALREADY_INITIALIZED";
        case ROCKETNET_STATUS_NOT_INITIALIZED:                  return "NOT_INITIALIZED";
        case ROCKETNET_STATUS_LIVE_ALLOCATIONS:                 return "LIVE_ALLOCATIONS";
        case ROCKETNET_STATUS_PLATFORM_INIT_FAILED:             return "PLATFORM_INIT_FAILED";
        case ROCKETNET_STATUS_CRYPTO_INIT_FAILED:               return "CRYPTO_INIT_FAILED";
        case ROCKETNET_STATUS_INVALID_ARGUMENT:                 return "INVALID_ARGUMENT";
        case ROCKETNET_STATUS_CONFIG_ENV_INVALID:               return "CONFIG_ENV_INVALID";
        case ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE:              return "CONFIG_OUT_OF_RANGE";
        case ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED:           return "MEMORY_BUDGET_EXCEEDED";
        case ROCKETNET_STATUS_MISSING_KEYS:                     return "MISSING_KEYS";
        case ROCKETNET_STATUS_INVALID_PROTOCOL_ID:              return "INVALID_PROTOCOL_ID";
        case ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION:           return "DEV_KEYS_IN_PRODUCTION";
        case ROCKETNET_STATUS_INVALID_ADDRESS:                  return "INVALID_ADDRESS";
        case ROCKETNET_STATUS_INVALID_TRANSPORT_LIST:           return "INVALID_TRANSPORT_LIST";
        case ROCKETNET_STATUS_INVALID_CONNECT_TOKEN:            return "INVALID_CONNECT_TOKEN";
        case ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED:            return "CONNECT_TOKEN_EXPIRED";
        case ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED:  return "TRANSPORT_PORT_ZERO_UNSUPPORTED";
        case ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED:       return "TRANSPORT_LADDER_EXHAUSTED";
        case ROCKETNET_STATUS_OUT_OF_MEMORY:                    return "OUT_OF_MEMORY";
        case ROCKETNET_STATUS_MUTEX_CREATE_FAILED:              return "MUTEX_CREATE_FAILED";
        case ROCKETNET_STATUS_THREAD_CREATE_FAILED:             return "THREAD_CREATE_FAILED";
        case ROCKETNET_STATUS_TIMER_CREATE_FAILED:              return "TIMER_CREATE_FAILED";
        case ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED:          return "CLIENT_NOT_DISCONNECTED";
        case ROCKETNET_STATUS_SERVER_NOT_STOPPED:               return "SERVER_NOT_STOPPED";
        case ROCKETNET_STATUS_FLOW_ID_RESERVED:                 return "FLOW_ID_RESERVED";
        case ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED:   return "TRANSPORT_FAMILIES_UNSUPPORTED";
        case ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED:           return "CONFIG_NOT_INITIALIZED";
        case ROCKETNET_STATUS_TRANSPORT_NOT_BUILT:              return "TRANSPORT_NOT_BUILT";
        case ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION: return "TEST_PACKET_FILTER_IN_PRODUCTION";
        case ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION:  return "DEV_CONNECT_TOKEN_IN_PRODUCTION";
        case ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO:          return "CONNECT_TOKEN_PORT_ZERO";
        case ROCKETNET_STATUS_LOOPBACK_HUB_MISSING:             return "LOOPBACK_HUB_MISSING";
        case ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH:            return "LOOPBACK_HUB_MISMATCH";
        case ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE:              return "LOOPBACK_HUB_IN_USE";
        default:
            break;
    }

    return "UNKNOWN";
}

static void default_log_callback( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
#if !ROCKETNET_UNREAL_ENGINE     // IMPORTANT: Unreal doesn't like printf
    (void) context;

    if ( level != ROCKETNET_LOG_LEVEL_NONE )
    {
        const char * level_string = rocketnet_log_level_word( level );
        printf( "%.6f: %s: %.*s\n", rocketnet_platform_time(), level_string, (int) length, message );
    }
    else
    {
        printf( "%.*s\n", (int) length, message );
    }

    fflush( stdout );
#endif // #if !ROCKETNET_UNREAL_ENGINE
}

static volatile uint64_t rocketnet_log_sink_lock = 0;
static rocketnet_log_function_t rocketnet_log_callback = NULL;
static void * rocketnet_log_callback_context = NULL;

// NULL restores the default sink. the spin lock keeps callback and context together

void rocketnet_log_function( rocketnet_log_function_t callback, void * context )
{
    rocketnet_platform_spin_lock_u64( &rocketnet_log_sink_lock );

    rocketnet_log_callback = callback;
    rocketnet_log_callback_context = callback ? context : NULL;

    rocketnet_platform_spin_unlock_u64( &rocketnet_log_sink_lock );
}

// filters before formatting, then formats once into a 1024-byte buffer

ROCKETNET_PRINTF_FORMAT( 2, 0 )
static void rocketnet_log_v( int level, const char * format, va_list args )
{
    if ( log_quiet.load( std::memory_order_relaxed ) )
        return;
    if ( level != ROCKETNET_LOG_LEVEL_NONE &&
         level > log_level.load( std::memory_order_relaxed ) )
        return;

    char buffer[1024];
    int n = vsnprintf( buffer, sizeof( buffer ), format, args );
    size_t length = ( n < 0 ) ? 0 : ( (size_t) n < sizeof( buffer ) ? (size_t) n : sizeof( buffer ) - 1 );

    rocketnet_platform_spin_lock_u64( &rocketnet_log_sink_lock );

    rocketnet_log_function_t cb = rocketnet_log_callback;
    void * ctx = rocketnet_log_callback_context;

    rocketnet_platform_spin_unlock_u64( &rocketnet_log_sink_lock );

    if ( cb )
        cb( ctx, (enum rocketnet_log_level_t) level, buffer, length );
    else
        default_log_callback( NULL, (enum rocketnet_log_level_t) level, buffer, length );
}

void rocketnet_printf( int level, const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;

    va_list args;
    va_start( args, format );

    rocketnet_log_v( level, format, args );

    va_end( args );
}

void rocketnet_info( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;

    va_list args;
    va_start( args, format );

    rocketnet_log_v( ROCKETNET_LOG_LEVEL_INFO, format, args );

    va_end( args );
}

void rocketnet_warn( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;

    va_list args;
    va_start( args, format );

    rocketnet_log_v( ROCKETNET_LOG_LEVEL_WARN, format, args );

    va_end( args );
}

void rocketnet_error( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;

    va_list args;
    va_start( args, format );

    rocketnet_log_v( ROCKETNET_LOG_LEVEL_ERROR, format, args );

    va_end( args );
}

void rocketnet_debug( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;

    va_list args;
    va_start( args, format );

    rocketnet_log_v( ROCKETNET_LOG_LEVEL_DEBUG, format, args );

    va_end( args );
}

void rocketnet_spam( const char * format, ... )
{
    if ( !rocketnet_api_null_guard( format ) )
        return;

    va_list args;
    va_start( args, format );

    rocketnet_log_v( ROCKETNET_LOG_LEVEL_SPAM, format, args );

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

static void * ( *rocketnet_malloc_function )( void * context, size_t bytes ) = rocketnet_default_malloc_function;

// the allocations refused on this thread, so a failed create can report OUT_OF_MEMORY
static thread_local uint64_t rocketnet_allocation_refusals_on_this_thread = 0;

uint64_t rocketnet_allocation_refusals()
{
    return rocketnet_allocation_refusals_on_this_thread;
}

// counts an allocation that failed outside rocketnet_malloc

void rocketnet_allocation_refused()
{
    ++rocketnet_allocation_refusals_on_this_thread;
}

static void ( *rocketnet_free_function )( void * context, void * p ) = rocketnet_default_free_function;

static std::mutex rocketnet_lifecycle_mutex;
static size_t rocketnet_allocations = 0; // includes allocator callbacks in flight
static bool rocketnet_initialized = false;
static bool rocketnet_transitioning = false;

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
    void ( *free_function )( void *, void * );
};

uint64_t rocketnet_allocation_overhead()
{
    return sizeof( rocketnet_allocation_header_t ) + ROCKETNET_ALLOCATION_ALIGNMENT - 1;
}

// call with the lifecycle lock held. NULL for both is the system allocator

static int rn_allocator_install_locked( void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) )
{
    if ( ( malloc_function == NULL ) != ( free_function == NULL ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( !malloc_function )
    {
        malloc_function = rocketnet_default_malloc_function;
        free_function = rocketnet_default_free_function;
    }

    if ( rocketnet_initialized || rocketnet_transitioning )
        return ROCKETNET_STATUS_ALREADY_INITIALIZED;
    if ( malloc_function == rocketnet_malloc_function && free_function == rocketnet_free_function )
        return ROCKETNET_STATUS_OK;
    if ( rocketnet_allocations != 0 )
        return ROCKETNET_STATUS_LIVE_ALLOCATIONS;

    rocketnet_malloc_function = malloc_function;
    rocketnet_free_function = free_function;

    return ROCKETNET_STATUS_OK;
}

int rocketnet_allocator_ex( void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) )
{
    int status = ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( malloc_function && free_function )
    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

        status = rn_allocator_install_locked( malloc_function, free_function );
    }

    if ( status != ROCKETNET_STATUS_OK )
    {
        rocketnet_error( "allocator replacement requires an uninitialized library with no live allocations" );

        rocketnet_api_misuse_record();
    }

    return status;
}

void * rocketnet_malloc( void * context, size_t bytes )
{
    const size_t alignment = ROCKETNET_ALLOCATION_ALIGNMENT;
    const size_t overhead = (size_t) rocketnet_allocation_overhead();
    if ( bytes > SIZE_MAX - overhead )
    {
        ++rocketnet_allocation_refusals_on_this_thread;
        return NULL;
    }

    void * ( *allocate )( void *, size_t );

    void ( *release )( void *, void * );

    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

        allocate = rocketnet_malloc_function;
        release = rocketnet_free_function;
        ++rocketnet_allocations;
    }

    void * base = allocate( context, bytes + overhead );
    if ( !base )
    {
        ++rocketnet_allocation_refusals_on_this_thread;

        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

        --rocketnet_allocations;
        return NULL;
    }

    const uintptr_t aligned = ( (uintptr_t) base + overhead ) & ~(uintptr_t) ( alignment - 1 );
    rocketnet_allocation_header_t * header = (rocketnet_allocation_header_t *) ( aligned - sizeof( rocketnet_allocation_header_t ) );
    header->base = base;
    header->context = context;
    header->free_function = release;

    return (void *) aligned;
}

void rocketnet_free( void * context, void * p )
{
    (void) context;

    if ( !p )
        return;

    const rocketnet_allocation_header_t allocation = *(rocketnet_allocation_header_t *) ( (uint8_t *) p - sizeof( rocketnet_allocation_header_t ) );

    allocation.free_function( allocation.context, allocation.base );

    std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

    --rocketnet_allocations;
}

double rocketnet_time()
{
    return rocketnet_platform_time();
}

double rocketnet_sleep_clamp( double seconds )
{
    if ( !( seconds > 0.0 ) )
        return 0.0;
    if ( seconds > ROCKETNET_SLEEP_MAX_SECONDS )
        return ROCKETNET_SLEEP_MAX_SECONDS;

    return seconds;
}

#if ROCKETNET_DEV
static thread_local uint64_t rocketnet_sleep_platform_dispatch_count = 0;
static thread_local void ( *rocketnet_sleep_platform_dispatch_override_for_test )( double seconds ) = nullptr;

uint64_t rocketnet_sleep_platform_dispatch_count_for_test()
{
    return rocketnet_sleep_platform_dispatch_count;
}

void rocketnet_sleep_set_platform_dispatch_for_test( void ( *dispatch )( double seconds ) )
{
    rocketnet_sleep_platform_dispatch_override_for_test = dispatch;
}
#endif // #if ROCKETNET_DEV

void rocketnet_sleep( double seconds )
{
    const double clamped = rocketnet_sleep_clamp( seconds );
    if ( clamped > 0.0 )
    {
#if ROCKETNET_DEV
        ++rocketnet_sleep_platform_dispatch_count;

        if ( rocketnet_sleep_platform_dispatch_override_for_test )
        {
            rocketnet_sleep_platform_dispatch_override_for_test( clamped );
            return;
        }
#endif // #if ROCKETNET_DEV
        rocketnet_platform_sleep( clamped );
    }
}

// -------------------------------------------------------------

uint64_t rocketnet_protocol_version_hash( int major, int minor )
{
    char text[64];
    snprintf( text, sizeof( text ), "%d.%d", major, minor );
    return rocketnet_hash_string( text );
}

uint64_t rocketnet_random_uint64()
{
    uint64_t value;
    randombytes_buf( &value, sizeof( value ) );
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

const char * rocketnet_platform_name( int platform_id )
{
    switch ( platform_id )
    {
        case ROCKETNET_PLATFORM_WINDOWS: return "windows";
        case ROCKETNET_PLATFORM_MAC:     return "mac";
        case ROCKETNET_PLATFORM_LINUX:   return "linux";
        case ROCKETNET_PLATFORM_IOS:     return "ios";
        case ROCKETNET_PLATFORM_BSD:     return "bsd";
        default:
            break;
    }

    return "unknown";
}

void * rocketnet_global_context = NULL;

enum rocketnet_status_t rocketnet_init( void * context, void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) )
{
    int status = ROCKETNET_STATUS_OK;
    (void) rocketnet_init_ex( context, malloc_function, free_function, &status );
    return (enum rocketnet_status_t) status;
}

bool rocketnet_init_ex( void * context, void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ), int * status )
{
    int refused = ROCKETNET_STATUS_OK;

    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

        refused = rn_allocator_install_locked( malloc_function, free_function );
        if ( refused == ROCKETNET_STATUS_OK )
            rocketnet_transitioning = true;
    }

    if ( refused != ROCKETNET_STATUS_OK )
    {
        if ( status )
            *status = refused;

        if ( refused == ROCKETNET_STATUS_ALREADY_INITIALIZED )
            rocketnet_error( "rocketnet_init: rocketnet is already initialized" );
        else if ( refused == ROCKETNET_STATUS_INVALID_ARGUMENT )
        {
            rocketnet_error( "rocketnet_init: pass both allocator functions, or NULL for both" );

            rocketnet_api_misuse_record();
        }
        else
        {
            rocketnet_error( "rocketnet_init: the allocator cannot change while allocations are live" );

            rocketnet_api_misuse_record();
        }

        return false;
    }

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

        if ( status )
            *status = platform_ready ? ROCKETNET_STATUS_CRYPTO_INIT_FAILED : ROCKETNET_STATUS_PLATFORM_INIT_FAILED;

        rocketnet_error( platform_ready ? "failed to initialize sodium" : "failed to initialize platform" );
        return false;
    }

    // the level first, so init's own lines obey it

    char log_level_override[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
    if ( rocketnet_platform_getenv( "ROCKETNET_LOG_LEVEL", log_level_override, sizeof( log_level_override ) ) != 0 )
        rocketnet_log_level_override_apply( log_level_override );

    rocketnet_info( "rocketnet version is %s", ROCKETNET_VERSION_FULL );
    rocketnet_info( "platform is %s", rocketnet_platform_name( rocketnet_platform_id() ) );

    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

        rocketnet_initialized = true;
        rocketnet_transitioning = false;
    }

    if ( status )
        *status = ROCKETNET_STATUS_OK;

    return true;
}

enum rocketnet_status_t rocketnet_term()
{
    return (enum rocketnet_status_t) rocketnet_term_ex();
}

int rocketnet_term_ex()
{
    bool not_initialized = false;
    bool live_allocations = false;

    {
        std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

        if ( !rocketnet_initialized || rocketnet_transitioning )
            not_initialized = true;
        else
        {
            live_allocations = rocketnet_allocations != 0;
            if ( !live_allocations )
                rocketnet_transitioning = true;
        }
    }

    if ( not_initialized )
    {
        rocketnet_error( "rocketnet_term: rocketnet is not initialized" );
        return ROCKETNET_STATUS_NOT_INITIALIZED;
    }

    if ( live_allocations )
    {
        rocketnet_error( "destroy all objects and allocations before terminating rocketnet" );
        return ROCKETNET_STATUS_LIVE_ALLOCATIONS;
    }

    // the bundled sodium keeps /dev/urandom open on macOS. a system libsodium
    // may be shared with the application, so only the bundled one is closed.
    // after the first term there is nothing open, and the close returns -1
#if defined( ROCKETNET_BUNDLED_SODIUM ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    (void) randombytes_close();
#endif // #if defined( ROCKETNET_BUNDLED_SODIUM ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    rocketnet_platform_term();

    rocketnet_global_context = NULL;

    std::lock_guard<std::mutex> lock( rocketnet_lifecycle_mutex );

    rocketnet_initialized = false;
    rocketnet_transitioning = false;

    return ROCKETNET_STATUS_OK;
}

// ---------------------------------------------------------------

uint64_t rocketnet_next_packet_lease_id()
{
    static std::atomic<uint64_t> next{ 1 };
    uint64_t ticket = next.load( std::memory_order_relaxed );
    while ( ticket != UINT64_MAX )
    {
        if ( next.compare_exchange_weak( ticket, ticket + 1, std::memory_order_relaxed ) )
            return ticket;
    }

    return 0;
}

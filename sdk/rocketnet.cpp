/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_base64.h"
#include "rocketnet_hash.h"
#include "rocketnet_packet_loss_tracker.h"
#include "rocketnet_out_of_order_tracker.h"
#include "rocketnet_jitter_tracker.h"
#include "rocketnet_hydrogen.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include <inttypes.h>
#if defined( _MSC_VER )
#include <malloc.h>
#endif // #if defined( _MSC_VER )
#include <time.h>
#include <atomic>

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
    log_level = level;
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
#if !NETWORK_FLOW_UNREAL_ENGINE     // IMPORTANT: Unreal doesn't like printf
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
#endif // #if !NETWORK_FLOW_UNREAL_ENGINE
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

void rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) )
{
    rocketnet_assert( malloc_function );
    rocketnet_assert( free_function );
    rocketnet_malloc_function = malloc_function;
    rocketnet_free_function = free_function;
}

void * rocketnet_malloc( void * context, size_t bytes )
{
    rocketnet_assert( rocketnet_malloc_function );
    return rocketnet_malloc_function( context, bytes );
}

void rocketnet_free( void * context, void * p )
{
    rocketnet_assert( rocketnet_free_function );
    if ( !p )
        return;
    return rocketnet_free_function( context, p );
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
    memset( p, 0, p_size );
    rocketnet_free( context, p );
}

// -------------------------------------------------------------

const char * rocketnet_user_id_string( uint64_t user_id, char * buffer, size_t buffer_size )
{
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
    hydro_random_buf( (uint8_t*)&uint32_value, sizeof(uint32_value) );
    uint64_t uint64_value = uint64_t(uint32_value);
    double double_value = double(uint64_value) / 0xFFFFFFFF;
    return float(double_value);
}

uint8_t rocketnet_random_uint8()
{
    uint8_t value;
    hydro_random_buf( &value, sizeof(value) );
    return value;
}

uint16_t rocketnet_random_uint16()
{
    uint16_t value;
    hydro_random_buf( (uint8_t*)&value, sizeof(value) );
    return value;
}

uint32_t rocketnet_random_uint32()
{
    uint32_t value;
    hydro_random_buf( (uint8_t*)&value, sizeof(value) );
    return value;
}

uint64_t rocketnet_random_uint64()
{
    uint64_t value;
    hydro_random_buf( (uint8_t*)&value, sizeof(value) );
    return value;
}

void rocketnet_random_bytes( uint8_t * data, size_t bytes )
{
    hydro_random_buf( (uint8_t*)data, bytes );
}

// -------------------------------------------------------------

size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size )
{
    rocketnet_assert( dest );
    rocketnet_assert( source );
    rocketnet_assert( dest_size >= 1 );
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
    rocketnet_assert( rocketnet_global_context == NULL );

    rocketnet_global_context = context;

    if ( !rocketnet_platform_init() )
    {
        rocketnet_error( "failed to initialize platform" );
        return false;
    }

    rocketnet_info( "rocketnet version is %s", ROCKETNET_VERSION_FULL );

    const char * platform_string = rocketnet_platform_string( rocketnet_platform_id() );
    const char * connection_string = rocketnet_connection_string( rocketnet_platform_connection_type() );

    rocketnet_info( "platform is %s (%s)", platform_string, connection_string );

    if ( hydro_init() != 0 ) 
    {
        rocketnet_error( "failed to initialize hydrogen" );
        return false;
    }

    const char * log_level_override = rocketnet_platform_getenv( "ROCKETNET_LOG_LEVEL" );
    if ( log_level_override )
    {
        log_level = atoi( log_level_override );
        rocketnet_info( "log level overridden to %d", log_level );
    }

    return true;
}

void rocketnet_term()
{
    rocketnet_platform_term();

    rocketnet_global_context = NULL;
}

// ---------------------------------------------------------------

#ifdef _MSC_VER
#pragma warning(pop)
#endif

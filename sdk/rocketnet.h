/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_H
#define ROCKETNET_H

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __linux__
#define ROCKETNET_XDP 1
#endif // #ifdef __linux__


#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 0
#endif // #if !defined( ROCKETNET_DEVELOPMENT )

#if !ROCKETNET_DEVELOPMENT

    #define ROCKETNET_VERSION_FULL                               "1.0.0"
    #define ROCKETNET_VERSION_MAJOR_INT                                1
    #define ROCKETNET_VERSION_MINOR_INT                                0
    #define ROCKETNET_VERSION_PATCH_INT                                0

#else // !ROCKETNET_DEVELOPMENT

    #define ROCKETNET_VERSION_FULL                                 "dev"
    #define ROCKETNET_VERSION_MAJOR_INT                              255
    #define ROCKETNET_VERSION_MINOR_INT                              255
    #define ROCKETNET_VERSION_PATCH_INT                              255

#endif // !ROCKETNET_DEVELOPMENT

#define ROCKETNET_MAX_CLIENTS                                       1000

#define ROCKETNET_MTU                                               1450

#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 8 - 20 )

#define ROCKETNET_LOG_LEVEL_NONE                                       0
#define ROCKETNET_LOG_LEVEL_ERROR                                      1
#define ROCKETNET_LOG_LEVEL_WARN                                       2
#define ROCKETNET_LOG_LEVEL_INFO                                       3
#define ROCKETNET_LOG_LEVEL_DEBUG                                      4
#define ROCKETNET_LOG_LEVEL_SPAM                                       5

#define ROCKETNET_ADDRESS_NONE                                         0
#define ROCKETNET_ADDRESS_IPV4                                         1
#define ROCKETNET_ADDRESS_IPV6                                         2

#define ROCKETNET_MAX_ADDRESS_STRING_LENGTH                          256

#define ROCKETNET_CONNECTION_TYPE_UNKNOWN                              0
#define ROCKETNET_CONNECTION_TYPE_WIRED                                1
#define ROCKETNET_CONNECTION_TYPE_WIFI                                 2
#define ROCKETNET_CONNECTION_TYPE_CELLULAR                             3
#define ROCKETNET_CONNECTION_TYPE_MAX                                  3

#define ROCKETNET_PLATFORM_UNKNOWN                                     0
#define ROCKETNET_PLATFORM_WINDOWS                                     1
#define ROCKETNET_PLATFORM_MAC                                         2
#define ROCKETNET_PLATFORM_LINUX                                       3
#define ROCKETNET_PLATFORM_IOS                                         4
#define ROCKETNET_PLATFORM_MAX                                         5

#if defined(_WIN32)
#define NOMINMAX
#endif

#if defined( ROCKETNET_SHARED )
    #if defined( _WIN32 )
        #ifdef ROCKETNET_EXPORT
            #if __cplusplus
            #define ROCKETNET_EXPORT_FUNC extern "C" __declspec(dllexport)
            #else
            #define ROCKETNET_EXPORT_FUNC extern __declspec(dllexport)
            #endif
        #else
            #if __cplusplus
            #define ROCKETNET_EXPORT_FUNC extern "C" __declspec(dllimport)
            #else
            #define ROCKETNET_EXPORT_FUNC extern __declspec(dllimport)
            #endif
        #endif
    #else
        #if __cplusplus
        #define ROCKETNET_EXPORT_FUNC extern "C"
        #else
        #define ROCKETNET_EXPORT_FUNC extern
        #endif
    #endif
#else
    #if __cplusplus
    #define ROCKETNET_EXPORT_FUNC extern "C"
    #else
    #define ROCKETNET_EXPORT_FUNC extern
    #endif
#endif

#if defined(_WIN32)
    #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_WINDOWS
#elif defined(__APPLE__)
    #include "TargetConditionals.h"
    #if TARGET_OS_IPHONE
        #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_IOS
    #else
        #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_MAC
    #endif
#else
    #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_LINUX
#endif

#define ROCKETNET_PLATFORM_HAS_IPV6 1

#define ROCKETNET_PLATFORM_CAN_RUN_SERVER 1

#if !defined(ROCKETNET_UNREAL_ENGINE)
#define ROCKETNET_UNREAL_ENGINE 0
#endif // #if !defined(ROCKETNET_UNREAL_ENGINE)

#if !defined(ROCKETNET_SPIKE_TRACKING)
#define ROCKETNET_SPIKE_TRACKING 0
#endif // #if !defined(ROCKETNET_SPIKE_TRACKING)

#if !defined(ROCKETNET_ENABLE_MEMORY_CHECKS)
#define ROCKETNET_ENABLE_MEMORY_CHECKS 0
#endif // #if !defined(ROCKETNET_ENABLE_MEMORY_CHECKS)

#if !defined ( ROCKETNET_LITTLE_ENDIAN ) && !defined( ROCKETNET_BIG_ENDIAN )

  #ifdef __BYTE_ORDER__
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      #define ROCKETNET_LITTLE_ENDIAN 1
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      #define ROCKETNET_BIG_ENDIAN 1
    #else
      #error Unknown machine endianess detected. Please define ROCKETNET_LITTLE_ENDIAN or ROCKETNET_BIG_ENDIAN.
    #endif // __BYTE_ORDER__

  // Detect with GLIBC's endian.h
  #elif defined(__GLIBC__)
    #include <endian.h>
    #if (__BYTE_ORDER == __LITTLE_ENDIAN)
      #define ROCKETNET_LITTLE_ENDIAN 1
    #elif (__BYTE_ORDER == __BIG_ENDIAN)
      #define ROCKETNET_BIG_ENDIAN 1
    #else
      #error Unknown machine endianess detected. Please define ROCKETNET_LITTLE_ENDIAN or ROCKETNET_BIG_ENDIAN.
    #endif // __BYTE_ORDER

  // Detect with _LITTLE_ENDIAN and _BIG_ENDIAN macro
  #elif defined(_LITTLE_ENDIAN) && !defined(_BIG_ENDIAN)
    #define ROCKETNET_LITTLE_ENDIAN 1
  #elif defined(_BIG_ENDIAN) && !defined(_LITTLE_ENDIAN)
    #define ROCKETNET_BIG_ENDIAN 1

  // Detect with architecture macros
  #elif    defined(__sparc)     || defined(__sparc__)                           \
        || defined(_POWER)      || defined(__powerpc__)                         \
        || defined(__ppc__)     || defined(__hpux)      || defined(__hppa)      \
        || defined(_MIPSEB)     || defined(_POWER)      || defined(__s390__)
    #define ROCKETNET_BIG_ENDIAN 1
  #elif    defined(__i386__)    || defined(__alpha__)   || defined(__ia64)      \
        || defined(__ia64__)    || defined(_M_IX86)     || defined(_M_IA64)     \
        || defined(_M_ALPHA)    || defined(__amd64)     || defined(__amd64__)   \
        || defined(_M_AMD64)    || defined(__x86_64)    || defined(__x86_64__)  \
        || defined(_M_X64)      || defined(__bfin__)
    #define ROCKETNET_LITTLE_ENDIAN 1
  #elif defined(_MSC_VER) && defined(_M_ARM)
    #define ROCKETNET_LITTLE_ENDIAN 1
  #else
    #error Unknown machine endianess detected. Please define ROCKETNET_LITTLE_ENDIAN or ROCKETNET_BIG_ENDIAN.
  #endif

#endif

#if !defined(ROCKETNET_BIG_ENDIAN)
#define ROCKETNET_BIG_ENDIAN 0
#endif // #if !defined(ROCKETNET_BIG_ENDIAN)

#if !defined(ROCKETNET_LITTLE_ENDIAN)
#define ROCKETNET_LITTLE_ENDIAN 0
#endif // #if !defined(ROCKETNET_LITTLE_ENDIAN)

#if defined( _MSC_VER ) && _MSC_VER < 1700
typedef __int32 int32_t;
typedef __int64 int64_t;
#define PRId64 "I64d"
#define SCNd64 "I64d"
#define PRIx64 "I64x"
#define SCNx64 "I64x"
#else
#include <inttypes.h>
#endif

#if defined( _MSC_VER ) && _MSC_VER < 1900

#define snprintf c99_snprintf
#define vsnprintf c99_vsnprintf

__inline int c99_vsnprintf(char *outBuf, size_t size, const char *format, va_list ap)
{
    int count = -1;

    if (size != 0)
        count = _vsnprintf_s(outBuf, size, _TRUNCATE, format, ap);
    if (count == -1)
        count = _vscprintf(format, ap);

    return count;
}

__inline int c99_snprintf(char *outBuf, size_t size, const char *format, ...)
{
    int count;
    va_list ap;

    va_start(ap, format);
    count = c99_vsnprintf(outBuf, size, format, ap);
    va_end(ap);

    return count;
}

#endif

// -----------------------------------------

#ifdef __cplusplus
ROCKETNET_EXPORT_FUNC bool rocketnet_init( void * context = NULL );
#else
ROCKETNET_EXPORT_FUNC bool rocketnet_init( void * context );
#endif

ROCKETNET_EXPORT_FUNC void rocketnet_term();

// -----------------------------------------

extern void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line );

#ifndef ROCKETNET_ASSERTS
    #ifdef NDEBUG
        #define ROCKETNET_ASSERTS 0
    #else
        #define ROCKETNET_ASSERTS 1
    #endif
#endif

#if ROCKETNET_ASSERTS
#define rocketnet_assert( condition )                                                            \
do                                                                                          \
{                                                                                           \
    if ( !(condition) )                                                                     \
    {                                                                                       \
        rocketnet_assert_function_pointer( #condition, __FUNCTION__, __FILE__, __LINE__ );       \
    }                                                                                       \
} while(0)
#else
#define rocketnet_assert( ignore ) ((void)0)
#endif

ROCKETNET_EXPORT_FUNC void rocketnet_assert_function( void (*function)( const char * condition, const char * function, const char * file, int line ) );

// ------------------------------------------

ROCKETNET_EXPORT_FUNC void rocketnet_quiet( bool flag );

ROCKETNET_EXPORT_FUNC void rocketnet_log_level( int level );

ROCKETNET_EXPORT_FUNC void rocketnet_log_function( void (*function)( int level, const char * format, ... ) );

ROCKETNET_EXPORT_FUNC void rocketnet_printf( int level, const char * format, ... );

ROCKETNET_EXPORT_FUNC void rocketnet_info( const char * format, ... );

ROCKETNET_EXPORT_FUNC void rocketnet_warn( const char * format, ... );

ROCKETNET_EXPORT_FUNC void rocketnet_error( const char * format, ... );

ROCKETNET_EXPORT_FUNC void rocketnet_debug( const char * format, ... );

ROCKETNET_EXPORT_FUNC void rocketnet_spam( const char * format, ... );

// ------------------------------------------

ROCKETNET_EXPORT_FUNC void rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

ROCKETNET_EXPORT_FUNC void * rocketnet_malloc( void * context, size_t bytes );

ROCKETNET_EXPORT_FUNC void rocketnet_free( void * context, void * p );

ROCKETNET_EXPORT_FUNC void rocketnet_clear_and_free( void * context, void * p, size_t p_size );

// -----------------------------------------

ROCKETNET_EXPORT_FUNC const char * rocketnet_user_id_string( uint64_t user_id, char * buffer, size_t buffer_size );

ROCKETNET_EXPORT_FUNC float rocketnet_random_float();

ROCKETNET_EXPORT_FUNC uint8_t rocketnet_random_uint8();

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_random_uint16();

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_random_uint32();

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_random_uint64();

ROCKETNET_EXPORT_FUNC void rocketnet_random_bytes( uint8_t * data, size_t bytes );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_protocol_version();

ROCKETNET_EXPORT_FUNC double rocketnet_time();

ROCKETNET_EXPORT_FUNC void rocketnet_sleep( double seconds );

// -----------------------------------------

#ifdef __cplusplus

inline void rocketnet_endian_fix( uint16_t * value )
{
#if ROCKETNET_BIG_ENDIAN
    *value = ( *value & 0x00ff ) << 8 | ( *value & 0xff00 ) >> 8;
#endif // #if ROCKETNET_BIG_ENDIAN
}

inline void rocketnet_endian_fix( uint32_t * value )
{
#if ROCKETNET_BIG_ENDIAN
#ifdef __GNUC__
    *value = __builtin_bswap32( *value );
#else // #ifdef __GNUC__
    *value = ( *value & 0x000000ff ) << 24 | ( *value & 0x0000ff00 ) << 8 | ( *value & 0x00ff0000 ) >> 8 | ( *value & 0xff000000 ) >> 24;
#endif // #ifdef __GNUC__
#endif // #if ROCKETNET_BIG_ENDIAN
}

inline void rocketnet_endian_fix( uint64_t * value )
{
#if ROCKETNET_BIG_ENDIAN
#ifdef __GNUC__
    *value = __builtin_bswap64( value );
#else // #ifdef __GNUC__
    *value = ( *value & 0x00000000FFFFFFFF ) << 32 | ( *value & 0xFFFFFFFF00000000 ) >> 32;
    *value = ( *value & 0x0000FFFF0000FFFF ) << 16 | ( *value & 0xFFFF0000FFFF0000 ) >> 16;
    *value = ( *value & 0x00FF00FF00FF00FF ) << 8  | ( *value & 0xFF00FF00FF00FF00 ) >> 8;
#endif // #ifdef __GNUC__
#endif // #if ROCKETNET_BIG_ENDIAN
}

#endif // #ifdef __cplusplus

// -----------------------------------------

ROCKETNET_EXPORT_FUNC size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size );

// -----------------------------------------
//                                  client
// -----------------------------------------

#define ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED         -7
#define ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN         -6
#define ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT          -5
#define ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT -4
#define ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT  -3
#define ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT      -2
#define ROCKETNET_CLIENT_STATE_CONNECTION_DENIED             -1
#define ROCKETNET_CLIENT_STATE_DISCONNECTED                   0
#define ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE              1
#define ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST     2
#define ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE    3
#define ROCKETNET_CLIENT_STATE_CONNECTED                      4

ROCKETNET_EXPORT_FUNC const char * rocketnet_client_state_name( int state );

struct rocketnet_client_transport_t;
struct rocketnet_counters_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const char * connect_address_string );

ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_destroy( struct rocketnet_client_transport_t * client_transport );      // disconnect and pump a few updates first

ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_update( struct rocketnet_client_transport_t * client_transport );

ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_send_packet( struct rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes );

ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_receive_packet( struct rocketnet_client_transport_t * client_transport, uint8_t * packet_data );

ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_state( struct rocketnet_client_transport_t * client_transport );

ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_client_index( struct rocketnet_client_transport_t * client_transport );

ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_disconnect( struct rocketnet_client_transport_t * client_transport );

ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_client_transport_counters( struct rocketnet_client_transport_t * client_transport );

// -----------------------------------------
//                                  server
// -----------------------------------------

#define ROCKETNET_SERVER_TRANSPORT_RUNNING                    0
#define ROCKETNET_SERVER_TRANSPORT_STOPPING                   1
#define ROCKETNET_SERVER_TRANSPORT_STOPPED                    2

struct rocketnet_server_transport_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context, const char * public_address_string, int max_clients );

ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport );      // stop and wait for stopped first

ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport );

ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport );

ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport );

struct rocketnet_server_transport_payloads_t                                    // delivered by the last update, valid until the next
{
    int num_payloads;
    int client_index[256];
    uint8_t * payload_data[256];
    int payload_bytes[256];
};

ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_payloads_t * rocketnet_server_transport_payloads( struct rocketnet_server_transport_t * server_transport );

ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_send_packet( struct rocketnet_server_transport_t * server_transport, int client_index, const uint8_t * payload_data, int payload_bytes );

ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport );

ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index );

ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport );

// -----------------------------------------

ROCKETNET_EXPORT_FUNC const char * rocketnet_counter_name( int counter );       // the full counter enum is in rocketnet_counters.h

// -----------------------------------------

#endif // #ifndef ROCKETNET_H

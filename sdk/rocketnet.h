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

#define ROCKETNET_MAX_PAYLOAD_BYTES                        1400

ROCKETNET_EXPORT_FUNC const char * rocketnet_client_state_name( int state );

struct rocketnet_client_t;
struct rocketnet_counters_t;
struct rocketnet_packet_context_t;      // defined below, with the send surface

#define ROCKETNET_CONNECT_TOKEN_BYTES 768

// The client connects with a token, never an address: the token names the
// server, carries the keys, and is the credential -- minted by a backend in
// production, or locally by rocketnet_write_dev_connect_token against the
// published development keys. The config carries what the token does not:
// the per-title filter value and an optional state-change callback. NULL
// config is the development posture: the published test filter, no callback.

struct rocketnet_client_config_t
{
    uint8_t filter_value[4];
    void (*state_change_callback)( void * context, int previous_state, int current_state );
};

ROCKETNET_EXPORT_FUNC void rocketnet_client_default_config( struct rocketnet_client_config_t * config );

ROCKETNET_EXPORT_FUNC int rocketnet_write_dev_connect_token( const char * server_address_string, uint64_t client_id, uint64_t unix_timestamp, uint8_t * connect_token );

// time is the game's clock, a double in seconds, monotonic; pass the same
// clock to every call that takes one. The library never reads a clock of
// its own on these paths, so a game owns time and a test can hold it still.

ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, double time );

ROCKETNET_EXPORT_FUNC void rocketnet_client_destroy( struct rocketnet_client_t * client_transport );      // disconnect and pump a few updates first

ROCKETNET_EXPORT_FUNC void rocketnet_client_update( struct rocketnet_client_t * client_transport, double time );

ROCKETNET_EXPORT_FUNC void rocketnet_client_send_packet( struct rocketnet_client_t * client_transport, const uint8_t * packet_data, int packet_bytes );

ROCKETNET_EXPORT_FUNC int rocketnet_client_receive_packet( struct rocketnet_client_t * client_transport, uint8_t * packet_data );

// the batch behind the copying cursor above: by count and index, and as
// parallel arrays in one call. valid until the next update.

ROCKETNET_EXPORT_FUNC int rocketnet_client_num_payloads( struct rocketnet_client_t * client_transport );

ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_payload( struct rocketnet_client_t * client_transport, int payload_index, int * payload_bytes );

ROCKETNET_EXPORT_FUNC int rocketnet_client_payloads( struct rocketnet_client_t * client_transport, uint8_t * const ** payload_data, const int ** payload_bytes );

// the client's acquire/commit send family: the same shape, layout and
// threading contract as the server's, minus the client index. NOTE:
// rocketnet_client_send_packet (the convenience above) flushes the ring
// inline and is therefore main-thread only, never during an open fill --
// it is the simple form, not a fill-phase participant.

ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_start_packet( struct rocketnet_client_t * client_transport, struct rocketnet_packet_context_t * context );

ROCKETNET_EXPORT_FUNC int rocketnet_client_finish_packet( struct rocketnet_client_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes );

ROCKETNET_EXPORT_FUNC void rocketnet_client_abort_packet( struct rocketnet_client_t * client_transport, struct rocketnet_packet_context_t * context );

ROCKETNET_EXPORT_FUNC void rocketnet_client_send_packets( struct rocketnet_client_t * client_transport );

ROCKETNET_EXPORT_FUNC int rocketnet_client_state( struct rocketnet_client_t * client_transport );

ROCKETNET_EXPORT_FUNC int rocketnet_client_client_index( struct rocketnet_client_t * client_transport );

ROCKETNET_EXPORT_FUNC void rocketnet_client_disconnect( struct rocketnet_client_t * client_transport );

ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_client_counters( struct rocketnet_client_t * client_transport );

// The client's connection quality: rtt and jitter in milliseconds, packet
// loss as a percentage, and the last second's packet rates and bandwidth
// both ways. The rates come from a ring of per-frame buckets filled as the
// existing send and receive walks run and summed over the trailing second,
// so the cost per update is flat and no walk is added for them. rtt is
// measured from the handshake's round trips -- the exchanges the server
// answers immediately -- and holds from there; jitter is inter-arrival
// jitter over the session sequence; packet loss is receive-side, from gaps
// in the session sequence. These numbers feed time synchronization.

struct rocketnet_client_stats_t
{
    float rtt;                                      // round trip time (milliseconds)
    float jitter;                                   // inter-arrival jitter (milliseconds)
    float packet_loss;                              // percent
    int packets_sent_per_second;
    int packets_received_per_second;
    int bandwidth_sent_kbps;
    int bandwidth_received_kbps;
};

ROCKETNET_EXPORT_FUNC void rocketnet_client_stats( struct rocketnet_client_t * client_transport, struct rocketnet_client_stats_t * stats );

// -----------------------------------------
//                                  server
// -----------------------------------------

#define ROCKETNET_SERVER_RUNNING                    0
#define ROCKETNET_SERVER_STOPPING                   1
#define ROCKETNET_SERVER_STOPPED                    2

struct rocketnet_server_t;

// The server is configured explicitly. rocketnet_server_default_config gives
// the shape with no keys -- create refuses it until keys are supplied,
// because default keys are how every deployment of a protocol ends up
// sharing one identity. rocketnet_server_dev_config fills the published
// development keys and test filter for local work, loudly. num_xdp_queues
// and num_os_cpus configure the XDP transport's queue and core layout;
// zero means the transport default, and the portable build ignores both.

struct rocketnet_server_config_t
{
    uint64_t protocol_id;
    int max_clients;
    uint8_t filter_value[4];
    uint8_t backend_public_key[32];
    uint8_t server_public_key[32];
    uint8_t server_secret_key[32];
    int num_xdp_queues;
    int num_os_cpus;
};

ROCKETNET_EXPORT_FUNC void rocketnet_server_default_config( struct rocketnet_server_config_t * config );

ROCKETNET_EXPORT_FUNC void rocketnet_server_dev_config( struct rocketnet_server_config_t * config );

ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context, const char * public_address_string, const struct rocketnet_server_config_t * config, double time );

ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server_transport );      // stop and wait for stopped first

ROCKETNET_EXPORT_FUNC void rocketnet_server_stop( struct rocketnet_server_t * server_transport );

ROCKETNET_EXPORT_FUNC int rocketnet_server_state( struct rocketnet_server_t * server_transport );

ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server_transport, double time );

// The payloads delivered by the last update, valid until the next update.
// Iterated by index so the storage behind them stays the library's own --
// its layout and its capacity are not part of this surface.
//
//     int num_payloads = rocketnet_server_num_payloads( server );
//     for ( int i = 0; i < num_payloads; i++ )
//     {
//         int client_index, payload_bytes;
//         uint8_t * payload = rocketnet_server_payload( server, i, &client_index, &payload_bytes );
//         ...
//     }

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_payloads( struct rocketnet_server_t * server_transport );

ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_payload( struct rocketnet_server_t * server_transport, int payload_index, int * client_index, int * payload_bytes );

// The whole receive batch as parallel arrays in one call, valid until the
// next update -- the zero-per-item form of the accessors above, for
// bindings and tight loops. Returns the number of payloads.

ROCKETNET_EXPORT_FUNC int rocketnet_server_payloads( struct rocketnet_server_t * server_transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes );

// -----------------------------------------
//                        sending packets
// -----------------------------------------

// The send surface is acquire, write, commit: start_packet hands back a
// 16-aligned frame; the payload is written at frame + payload_index --
// itself 16-aligned, capacity a multiple of eight, so any word-based
// serializer roots there directly -- and finish_packet seals it in place
// on the calling thread and stages it. Nothing is copied at any step; the
// packet is encrypted where it was written. send_packets flushes
// everything staged.
//
// Threading contract: update and send_packets run on the main thread, and
// so do disconnect, disconnect_client and stop -- never while a fill is
// open. start_packet, finish_packet and abort_packet may run on any number
// of game threads strictly between an update and the following
// send_packets, and the handoff between fill and flush must be a
// synchronizing operation (a join or a barrier), not a wall-clock wait.
// send_packets is required once per frame, after update: packets are never
// sent without it. Calling it again in the same frame is a no-op. The
// fields of the context are read-only to the caller.
//
// finish_packet returns the sealed wire size, or zero when the packet was
// dropped instead (payload out of range, or the connection gone) -- the
// sequence is burned either way, which the receiver's replay window
// tolerates.

struct rocketnet_packet_context_t
{
    uint8_t * frame;
    int payload_index;
    int payload_capacity;
    uint64_t packet_id;
    uint64_t sequence;
    int client_index;
};

ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_start_packet( struct rocketnet_server_t * server_transport, int client_index, struct rocketnet_packet_context_t * context );

ROCKETNET_EXPORT_FUNC int rocketnet_server_finish_packet( struct rocketnet_server_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes );

ROCKETNET_EXPORT_FUNC void rocketnet_server_abort_packet( struct rocketnet_server_t * server_transport, struct rocketnet_packet_context_t * context );

ROCKETNET_EXPORT_FUNC void rocketnet_server_send_packets( struct rocketnet_server_t * server_transport );

ROCKETNET_EXPORT_FUNC void rocketnet_server_send_packet( struct rocketnet_server_t * server_transport, int client_index, const uint8_t * payload_data, int payload_bytes );

// Disconnects one connected client deliberately: the redundant disconnect
// burst goes out on the next flush and the slot frees immediately. Why is
// the game's decision; this is only the mechanism. Main thread only, never
// while a fill phase is open -- it tears down the connection state the
// fill relies on.

ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_client( struct rocketnet_server_t * server_transport, int client_index );

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_connected_clients( struct rocketnet_server_t * server_transport );

ROCKETNET_EXPORT_FUNC int rocketnet_server_is_client_connected( struct rocketnet_server_t * server_transport, int client_index );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_server_client_id( struct rocketnet_server_t * server_transport, int client_index );

ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_server_counters( struct rocketnet_server_t * server_transport );

// The server's whole-socket rates: packets and bandwidth per second, both
// directions, over the trailing second. Accumulation rides the walks the
// transport already makes -- the receive drain and the send flush -- into a
// ring of per-frame buckets, and the published numbers sum only buckets
// younger than one second, so the cost per frame is flat by construction:
// no per-client sweep, no extra pass over any buffer. At 1000 clients the
// update pays the same fixed bucket sum it pays at one. These numbers feed
// time synchronization.

struct rocketnet_server_stats_t
{
    uint64_t packets_sent_per_second;
    uint64_t packets_received_per_second;
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
};

ROCKETNET_EXPORT_FUNC void rocketnet_server_stats( struct rocketnet_server_t * server_transport, struct rocketnet_server_stats_t * stats );

// -----------------------------------------
//                              frame sync
// -----------------------------------------

// Pegs a headless server loop to real time: an absolute deadline per frame,
// the platform sleep that honors it, thread priority so the wakeups are
// sharp, and a capped adaptive spin guard for the last microseconds.
// Optional -- examples may still just sleep. Create at a frame rate, then
// call wait once per loop iteration; it returns at the frame boundary.
// Dropped frames are detected, counted and resynchronized, never chased.
// Priority is taken on the waiting thread at the first wait; where the
// platform refuses it the primitive degrades to plain absolute sleep --
// never an unprivileged spin -- and the stats say so.

struct rocketnet_frame_sync_t;

ROCKETNET_EXPORT_FUNC struct rocketnet_frame_sync_t * rocketnet_frame_sync_create( void * context, double frame_rate );

ROCKETNET_EXPORT_FUNC void rocketnet_frame_sync_destroy( struct rocketnet_frame_sync_t * frame_sync );

ROCKETNET_EXPORT_FUNC void rocketnet_frame_sync_wait( struct rocketnet_frame_sync_t * frame_sync );

struct rocketnet_frame_sync_stats_t
{
    uint64_t frame;                                 // frames completed
    uint64_t dropped_frames;                        // frames lost to overruns, total
    int priority_held;                              // platform thread priority obtained
    int spinning;                                   // spin guard active (requires priority)
    double guard_seconds;                           // the current adaptive guard
    double wakeup_error_seconds;                    // the last wake's lateness past its deadline
};

ROCKETNET_EXPORT_FUNC void rocketnet_frame_sync_stats( struct rocketnet_frame_sync_t * frame_sync, struct rocketnet_frame_sync_stats_t * stats );

// -----------------------------------------
//            the serialize read (C++)
// -----------------------------------------

// Include serialize.h before this header and the stream forms of the
// acquire/commit surface appear: the same calls, with a WriteStream bound
// at the frame's aligned payload root on start and measured on finish, and
// a ReadStream bound over each received payload. Plain C and C# see none
// of this; it compiles to nothing but the calls above.

#if defined(__cplusplus) && defined(SERIALIZE_H)

namespace rocketnet
{
    inline bool server_start_packet( struct rocketnet_server_t * server, int client_index, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        uint8_t * frame = rocketnet_server_start_packet( server, client_index, &context );
        if ( !frame )
            return false;
        stream.Initialize( frame + context.payload_index, context.payload_capacity );
        return true;
    }

    // serialize's write side checks bounds with asserts only, by design: a
    // Release overflow past the payload capacity is not caught here or
    // there. Size packets to the capacity the context reports.

    inline bool server_finish_packet( struct rocketnet_server_t * server, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        stream.Flush();
        return rocketnet_server_finish_packet( server, &context, (int) stream.GetBytesProcessed() ) != 0;
    }

    inline void server_abort_packet( struct rocketnet_server_t * server, struct rocketnet_packet_context_t & context )
    {
        rocketnet_server_abort_packet( server, &context );
    }

    inline bool server_payload( struct rocketnet_server_t * server, int payload_index, serialize::ReadStream & stream, int & client_index )
    {
        int payload_bytes;
        uint8_t * payload = rocketnet_server_payload( server, payload_index, &client_index, &payload_bytes );
        if ( !payload )
            return false;
        stream.Initialize( payload, payload_bytes );
        return true;
    }

    inline bool client_start_packet( struct rocketnet_client_t * client, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        uint8_t * frame = rocketnet_client_start_packet( client, &context );
        if ( !frame )
            return false;
        stream.Initialize( frame + context.payload_index, context.payload_capacity );
        return true;
    }

    inline bool client_finish_packet( struct rocketnet_client_t * client, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        stream.Flush();
        return rocketnet_client_finish_packet( client, &context, (int) stream.GetBytesProcessed() ) != 0;
    }

    inline void client_abort_packet( struct rocketnet_client_t * client, struct rocketnet_packet_context_t & context )
    {
        rocketnet_client_abort_packet( client, &context );
    }

    inline bool client_payload( struct rocketnet_client_t * client, int payload_index, serialize::ReadStream & stream )
    {
        int payload_bytes;
        uint8_t * payload = rocketnet_client_payload( client, payload_index, &payload_bytes );
        if ( !payload )
            return false;
        stream.Initialize( payload, payload_bytes );
        return true;
    }
}

#endif // #if defined(__cplusplus) && defined(SERIALIZE_H)

// -----------------------------------------

ROCKETNET_EXPORT_FUNC const char * rocketnet_counter_name( int counter );       // the full counter enum is in rocketnet_counters.h

// -----------------------------------------

#endif // #ifndef ROCKETNET_H

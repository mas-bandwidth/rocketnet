/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The SDK's internal header: everything the library's sources, tests and tools
// share that is not for callers. It includes the public rocketnet.h and adds
// the platform detection, the asserts, the config tables and the internal
// helpers. Callers include rocketnet.h alone.

#ifndef ROCKETNET_INTERNAL_H
#define ROCKETNET_INTERNAL_H

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "rocketnet.h"
#include "rocketnet_layers.h"

#ifdef __linux__
#define ROCKETNET_XDP 1
#endif // #ifdef __linux__

// a compile-time assert that also builds in C compilers without C11 mode

#ifdef __cplusplus
#define ROCKETNET_STATIC_ASSERT( condition, message )  static_assert( condition, message )
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define ROCKETNET_STATIC_ASSERT( condition, message )  _Static_assert( condition, message )
#else
#define ROCKETNET_STATIC_ASSERT_JOIN_( a, b ) a##b
#define ROCKETNET_STATIC_ASSERT_JOIN( a, b ) ROCKETNET_STATIC_ASSERT_JOIN_( a, b )
#define ROCKETNET_STATIC_ASSERT( condition, message ) \
    typedef char ROCKETNET_STATIC_ASSERT_JOIN( rocketnet_static_assert_, __LINE__ )[(condition) ? 1 : -1]
#endif

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PACKET_BYTES_IPV6 <= ROCKETNET_MAX_PACKET_BYTES, "the IPv6 packet is the smaller of the two families" );

/** @brief Connection classification when the type is unknown. */
#define ROCKETNET_CONNECTION_TYPE_UNKNOWN                              0
/** @brief Connection classification for wired access. */
#define ROCKETNET_CONNECTION_TYPE_WIRED                                1
/** @brief Connection classification for Wi-Fi access. */
#define ROCKETNET_CONNECTION_TYPE_WIFI                                 2
/** @brief Connection classification for cellular access. */
#define ROCKETNET_CONNECTION_TYPE_CELLULAR                             3
/** @brief Largest defined connection classification value. */
#define ROCKETNET_CONNECTION_TYPE_MAX                                  3

#define ROCKETNET_PLATFORM_UNKNOWN                                     0
#define ROCKETNET_PLATFORM_WINDOWS                                     1
#define ROCKETNET_PLATFORM_MAC                                         2
#define ROCKETNET_PLATFORM_LINUX                                       3
#define ROCKETNET_PLATFORM_IOS                                         4
#define ROCKETNET_PLATFORM_BSD                                         5
#define ROCKETNET_PLATFORM_MAX                                         6

#if defined(_WIN32)
#define NOMINMAX
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
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    // the BSDs verified per #81 ("Free, open. Net."): one platform bucket,
    // pure POSIX, portable driver only -- the linux bucket keeps netlink,
    // GSO and XDP, none of which exist here.
    #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_BSD
#else
    #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_LINUX
#endif

#define ROCKETNET_PLATFORM_HAS_IPV6 1


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

// Internal to the SDK. rocketnet_library_initialized reads the lifecycle state
// once for an entry point that must refuse before init; the create paths call
// it at entry, never per packet. rocketnet_api_misuse_record increments the
// count above from any refused misuse. The default allocator callbacks let a
// caller restore them after taking the allocator.
/** @brief Internal lifecycle-state query used by SDK entry points. @return Whether initialization has completed and the library is active. */
bool rocketnet_library_initialized();
/** @brief Internal helper that increments the process-wide refused-call count. */
void rocketnet_api_misuse_record();

// The public boundary's NULL guard. Every exported entry point calls this once
// per pointer it would dereference, at the top, before any work: a NULL is a
// counted no-op that increments rocketnet_api_misuse_count and, in a debug
// build, fires rocketnet_assert. The entry point returns its documented value
// when the guard says false. The check is one predictable branch, never in a
// loop and never on an internal call.
static inline bool rocketnet_api_null_guard( const void * p )
{
    if ( p != NULL )
        return true;
    rocketnet_api_misuse_record();
    rocketnet_assert( p );
    return false;
}
/** @brief Internal default allocator callback. @param context Opaque context, ignored. @param bytes Number of bytes requested. @return Allocated storage or NULL. */
void * rocketnet_default_malloc_function( void * context, size_t bytes );
/** @brief Internal default deallocator callback. @param context Opaque context, ignored. @param p Original pointer returned by the default allocator. */
void rocketnet_default_free_function( void * context, void * p );

// Internal to the SDK. Both transport updates clamp the caller's clock once per
// update, before any read of it, through this helper: a non-finite time, or a
// time below the last good one, becomes the last good time. A clamp sets
// *clamped, counts one misuse and fires rocketnet_assert; a release build
// clamps and goes on, a debug build is loud. It is also the client create's
// check, seeded with a zero last good time. One compare per update, none per
// packet.
/** @brief Internal monotonic-time guard used by transport updates. @param last_good Previous accepted time in seconds. @param time Candidate time in seconds. @param clamped Optional output set true when candidate is rejected; NULL is allowed. @return Candidate time if finite and nondecreasing, otherwise last_good. Invalid time records misuse and fires the configured assertion. */
double rocketnet_clamp_update_time( double last_good, double time, bool * clamped );

#ifdef __cplusplus

/** @brief Converts a 16-bit integer between host and little-endian representation in place. @param value Writable non-NULL value; conversion is symmetric and a no-op on little-endian hosts. */
inline void rocketnet_endian_fix( uint16_t * value )
{
#if ROCKETNET_BIG_ENDIAN
    *value = ( *value & 0x00ff ) << 8 | ( *value & 0xff00 ) >> 8;
#else // #if ROCKETNET_BIG_ENDIAN
    (void) value;
#endif // #if ROCKETNET_BIG_ENDIAN
}

/** @brief Converts a 32-bit integer between host and little-endian representation in place. @param value Writable non-NULL value; conversion is symmetric and a no-op on little-endian hosts. */
inline void rocketnet_endian_fix( uint32_t * value )
{
#if ROCKETNET_BIG_ENDIAN
#ifdef __GNUC__
    *value = __builtin_bswap32( *value );
#else // #ifdef __GNUC__
    *value = ( *value & 0x000000ff ) << 24 | ( *value & 0x0000ff00 ) << 8 | ( *value & 0x00ff0000 ) >> 8 | ( *value & 0xff000000 ) >> 24;
#endif // #ifdef __GNUC__
#else // #if ROCKETNET_BIG_ENDIAN
    (void) value;
#endif // #if ROCKETNET_BIG_ENDIAN
}

/** @brief Converts a 64-bit integer between host and little-endian representation in place. @param value Writable non-NULL value; conversion is symmetric and a no-op on little-endian hosts. */
inline void rocketnet_endian_fix( uint64_t * value )
{
#if ROCKETNET_BIG_ENDIAN
#ifdef __GNUC__
    *value = __builtin_bswap64( *value );
#else // #ifdef __GNUC__
    *value = ( *value & 0x00000000FFFFFFFF ) << 32 | ( *value & 0xFFFFFFFF00000000 ) >> 32;
    *value = ( *value & 0x0000FFFF0000FFFF ) << 16 | ( *value & 0xFFFF0000FFFF0000 ) >> 16;
    *value = ( *value & 0x00FF00FF00FF00FF ) << 8  | ( *value & 0xFF00FF00FF00FF00 ) >> 8;
#endif // #ifdef __GNUC__
#else // #if ROCKETNET_BIG_ENDIAN
    (void) value;
#endif // #if ROCKETNET_BIG_ENDIAN
}

#endif // #ifdef __cplusplus

// the public payload constants, written in rocketnet.h with their derivation

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PAYLOAD_BYTES % 8 == 0, "the payload capacity is a multiple of eight" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PAYLOAD_BYTES < ROCKETNET_MAX_PACKET_BYTES_IPV6, "the payload capacity fits the send budget" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES - ROCKETNET_MAX_PAYLOAD_BYTES >=
                         ROCKETNET_MAX_PACKET_BYTES - ROCKETNET_MAX_PACKET_BYTES_IPV6, "the receive payload is the capacity plus the family difference and the rounding" );

// -----------------------------------------
//                  transport config tables
// -----------------------------------------

// The per-transport config system (#106). Each transport's config is
// declared ONCE, in a table below: field name, type, default, valid range.
// Everything else derives from the table mechanically -- the struct field,
// the environment variable name (ROCKETNET_ prefix plus the endpoint and
// field name uppercased: cfg.driver -> ROCKETNET_SERVER_TRANSPORT or
// ROCKETNET_CLIENT_TRANSPORT), the parse, the validation,
// and the field descriptors a test walks to prove every field round-trips
// from environment. Adding a field here IS adding it everywhere; there is
// no second list to forget.
//
// Precedence at create is DEFAULT < CONFIG STRUCT < ENVIRONMENT: the app
// ships what it means in code, and an operator standing at an already-built
// binary overrides with env vars, no rebuild. Every env override is logged
// loudly at create with the variable name and the value it displaced
// (secret fields log redacted). Validation refuses loudly at create on any
// out-of-range value -- never clamps, never silently corrects.

// field flags: ZERO_MEANS_DEFAULT admits zero as "use the built-in
// default" beside the stated range; SECRET redacts the value in override
// logging

/** @brief Config-field flag that gives zero the field default meaning. */
#define ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT     (1<<0)
/** @brief Config-field flag that redacts its value in override logs. */
#define ROCKETNET_CONFIG_SECRET                 (1<<1)

/** @brief Zero initializer for fixed-size byte-array configuration fields. */
#define ROCKETNET_CONFIG_ZERO_BYTES             {0}

// The client transport config table: filter value, fragmentation shape
// (zero means the defaults above), and the driver candidate list.
//
//   INT_FIELD( name, default, min, max, flags )
//   UINT64_FIELD( name, default, flags )
//   BYTES_FIELD( name, size, default, flags )
//   STRING_FIELD( name, capacity, default, flags )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V1( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                    \
    BYTES_FIELD(  filter_value,          4,                              ROCKETNET_TEST_FILTER_VALUE,                          0 )                          \
    INT_FIELD(    max_payload_bytes,     0,                              ROCKETNET_MAX_PAYLOAD_BYTES,                                                       \
                                                                         ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES,               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    num_reassembly_slots,  0,                              1, ROCKETNET_MAX_FRAGMENTS,                            ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( driver,                ROCKETNET_DRIVER_STRING_LENGTH, "auto",                                               0 )                          \
    UINT64_FIELD( game_clock_epoch,      0,                                                                                    0 )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V2( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                    \
    INT_FIELD(    simulated_loss_percent,      0,                         0, 100,                                              0 )                          \
    INT_FIELD(    simulated_latency_ms,        0,                         0, 60000,                                            0 )                          \
    INT_FIELD(    simulated_jitter_ms,         0,                         0, 60000,                                            0 )                          \
    INT_FIELD(    simulated_duplicate_percent, 0,                         0, 100,                                              0 )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                       \
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V1( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                        \
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V2( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )

#define ROCKETNET_CONFIG_STRUCT_INT( name, def, min, max, flags )       int name;
#define ROCKETNET_CONFIG_STRUCT_UINT64( name, def, flags )              uint64_t name;
#define ROCKETNET_CONFIG_STRUCT_BYTES( name, size, def, flags )         uint8_t name[size];
#define ROCKETNET_CONFIG_STRUCT_STRING( name, capacity, def, flags )    char name[capacity];

// Sized configs (docs/ABI-EVOLUTION.md). Each config struct has the sized
// entry points beside its unsized ones: the caller passes sizeof the struct it
// was compiled against. A struct only grows at its end, so every size it has
// had is a prefix of today's; ROCKETNET_<STRUCT>_KNOWN_BYTES lists those sizes,
// oldest first, each one the end of that revision's last field rounded up to
// that revision's alignment (ROCKETNET_CONFIG_REVISION_BYTES). A known size
// reads the caller's prefix and defaults the rest; an unknown size, larger or
// smaller, is refused with a log line and nothing is read or written. The
// unsized entry points stay, and read and write exactly revision 1.


// Internal to the SDK: the one read and the one write every sized entry point
// makes (docs/ABI-EVOLUTION.md 3). write copies the first config_bytes of
// defaults into config; read sets config to defaults, then copies the first
// source_bytes of source over it. Each returns 1, or 0 with a log line naming
// type when the size is not in known (and then write leaves config untouched,
// read leaves it at the defaults).
int rocketnet_config_write_sized( const char * type, void * config, size_t config_bytes, const void * defaults, size_t defaults_bytes, const size_t * known, int num_known );
int rocketnet_config_read_sized( const char * type, void * config, const void * defaults, size_t defaults_bytes, const void * source, size_t source_bytes, const size_t * known, int num_known );

// docs/ABI-EVOLUTION.md 4: the sizes this struct has had, oldest first.
/** @brief Size in bytes of the client transport config before game_clock_epoch. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_LEGACY ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, time_callback ) + sizeof( void * ), sizeof( void * ) )
/** @brief Size in bytes of client transport config revision 1: with game_clock_epoch and step_driven, before the public link simulator fields. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, step_driven ) + sizeof( uint64_t ), sizeof( uint64_t ) )
/** @brief Current known client transport config size, in bytes: the public link simulator fields. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( void * ) )
/** @brief Current known client transport config size, in bytes. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2 ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1
/** @brief Current known client transport config size, in bytes. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_KNOWN_BYTES ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_LEGACY, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1

// The server transport config table -- the single declaration everything
// derives from, per the config system comment above. num_xdp_queues' range
// here is structural; the tighter bounds are per driver, enforced at the
// ladder rung (ROCKETNET_GSO_MAX_QUEUES).

#define ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                       \
    UINT64_FIELD( protocol_id,           0,                                                                                    0 )                          \
    INT_FIELD(    max_clients,        1000,                              1, ROCKETNET_MAX_CLIENTS,                             0 )                          \
    BYTES_FIELD(  filter_value,          4,                              ROCKETNET_TEST_FILTER_VALUE,                          0 )                          \
    BYTES_FIELD(  backend_public_key,   32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          0 )                          \
    BYTES_FIELD(  server_public_key,    32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          0 )                          \
    BYTES_FIELD(  server_secret_key,    32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          ROCKETNET_CONFIG_SECRET )    \
    INT_FIELD(    num_xdp_queues,        0,                              1, 1024,                                              ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    num_os_cpus,           0,                              1, 256,                                               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    max_payload_bytes,     0,                              ROCKETNET_MAX_PAYLOAD_BYTES,                                                       \
                                                                         ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES,               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    num_reassembly_slots,  0,                              1, ROCKETNET_MAX_FRAGMENTS,                            ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( driver,                ROCKETNET_DRIVER_STRING_LENGTH, "auto",                                               0 )                          \
    STRING_FIELD( public_address,        ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "",                                              0 )                          \
    INT_FIELD(    max_timeout_seconds,   60,                             1, 3600,                                              0 )                          \
    INT_FIELD(    handshake_deadline_seconds, 5,                         1, 60,                                                0 )                          \
    UINT64_FIELD( game_clock_epoch,           0,                                                                              0 )                          \
    INT_FIELD(    simulated_loss_percent,      0,                         0, 100,                                              0 )                          \
    INT_FIELD(    simulated_latency_ms,        0,                         0, 60000,                                            0 )                          \
    INT_FIELD(    simulated_jitter_ms,         0,                         0, 60000,                                            0 )                          \
    INT_FIELD(    simulated_duplicate_percent, 0,                         0, 100,                                              0 )

/** @brief Size in bytes of the server transport config before game_clock_epoch. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1_LEGACY ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, handshake_deadline_seconds ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Size in bytes of server transport config revision 1: before the public link simulator fields. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, game_clock_epoch ) + sizeof( uint64_t ), sizeof( uint64_t ) )
/** @brief Current known server transport config size, in bytes: the public link simulator fields. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Current known server transport config size, in bytes. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_KNOWN_BYTES ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1_LEGACY, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2

#if ROCKETNET_MAX_CHANNELS < 1 || ROCKETNET_MAX_CHANNELS > 65536
#error ROCKETNET_MAX_CHANNELS must be between 1 and 65536
#endif

ROCKETNET_STATIC_ASSERT( ROCKETNET_MESSAGE_SINGLE_BYTES >= 1400, "a 1400-byte game packet is one SINGLE message, zero copy, unfragmented" );

// The message config table, one declaration deriving struct, defaults and
// validation, as the transports' tables (docs/CHANNELS.md 12). Fields marked
// WIRE must be identical on both ends; CONFIG verifies them (layer 7).
// Validation applies the ranges here and the cross rules of section 14, and
// refuses, never clamps.

#define ROCKETNET_MESSAGE_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                       \
    INT_FIELD( num_channels,               32,      1, ROCKETNET_MAX_CHANNELS, 0 )   /* WIRE */                            \
    INT_FIELD( window_messages,          1024,     64, 1 << 20,                0 )   /* WIRE, power of two */              \
    INT_FIELD( receive_ring_bytes,     262144,   4096, 1 << 20,                0 )   /* WIRE, multiple of 16 */            \
    INT_FIELD( max_message_bytes,      262144,   ROCKETNET_MESSAGE_SINGLE_BYTES, 1 << 20, 0 )   /* WIRE, <= receive_ring_bytes */     \
    INT_FIELD( send_buffer_bytes,      800000,  12288, 1 << 28,                0 )                                         \
    INT_FIELD( send_queue_messages,      1024,     16, 1 << 20,                0 )                                         \
    INT_FIELD( packets_in_flight,         256,     16, 1 << 16,                0 )                                         \
    INT_FIELD( max_packets_per_update,      4,      1, 256,                    0 )                                         \
    INT_FIELD( unreliable_deliveries,     256,      1, 1 << 16,                0 )   /* per connection per update */       \
    INT_FIELD( sequenced_holds,            16,      0, 4096,                   0 )                                         \
    INT_FIELD( receipt_ring,              256,     16, 1 << 16,                0 )                                         \
    INT_FIELD( rto_initial_ms,            100,     10, 60000,                  0 )                                         \
    INT_FIELD( rto_min_ms,                 10,      1, 60000,                  0 )                                         \
    INT_FIELD( rto_max_ms,                250,     10, 60000,                  0 )                                         \
    INT_FIELD( reliable_timeout_ms,     10000,   1000, 600000,                 0 )                                         \
    INT_FIELD( client_send_kbps,         ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, INT32_MIN, ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 ) \
    INT_FIELD( server_send_kbps,         ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS, INT32_MIN, ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 )

#define ROCKETNET_MESSAGE_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_message_config_t, reliable_timeout_ms ) + sizeof( int ), sizeof( int ) )
#define ROCKETNET_MESSAGE_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_message_config_t, server_send_kbps ) + sizeof( int ), sizeof( int ) )
#define ROCKETNET_MESSAGE_CONFIG_KNOWN_BYTES ROCKETNET_MESSAGE_CONFIG_BYTES_1, ROCKETNET_MESSAGE_CONFIG_BYTES_2

// The public config structs in rocketnet.h are written out field by field;
// the tables above are the declarations the parse, the validation, the env
// surface and the ABI descriptors derive from. sdk/rocketnet_config.cpp
// asserts, field by field, that each public struct and its table agree in
// name, order, offset and size, so the two cannot drift.

// the longest single rocketnet_sleep, in seconds: its microsecond count fits
// a 32-bit int
#define ROCKETNET_SLEEP_MAX_SECONDS 2000.0

// the duration rocketnet_sleep passes to the platform: 0 for a negative, zero
// or NaN request, ROCKETNET_SLEEP_MAX_SECONDS for a longer or infinite one
double rocketnet_sleep_clamp( double seconds );

#endif // #ifndef ROCKETNET_INTERNAL_H

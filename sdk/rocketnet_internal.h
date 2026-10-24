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
#include "rocketnet_testing.h"
#include "rocketnet_layers.h"

#ifndef ROCKETNET_XDP
#ifdef __linux__
#define ROCKETNET_XDP 1
#else
#define ROCKETNET_XDP 0
#endif
#endif

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

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PACKET_BYTES == ROCKETNET_RECEIVE_PACKET_BYTES, "send and receive packet bounds match" );

// -----------------------------------------
//          status-returning lifecycle forms
// -----------------------------------------

// The int-status forms of rocketnet_init and rocketnet_term, which the
// public calls wrap, and the allocator seam the unit tests install directly.
// The public calls return the same code as enum rocketnet_status_t.

#ifdef __cplusplus
extern "C" {
#endif

// rocketnet_init with the exact status code in status, when not NULL, on
// every return path. It returns true on success. A failure logs one error
// line. context goes to init's own allocations, platform discovery among
// them. NULL for both allocator functions is the system allocator.
bool rocketnet_init_ex( void * context, void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ), int * status );

/**
 * @brief Terminates the library and returns why termination did or did not occur.
 *
 * Call after every client and server is destroyed and every SDK allocation is
 * released. A failure logs one error line. Do not race termination with other
 * SDK work.
 *
 * @return The section-2.1 code: ROCKETNET_STATUS_OK on termination,
 * ROCKETNET_STATUS_NOT_INITIALIZED if the library is not initialized (nothing
 * to terminate) or is transitioning, or ROCKETNET_STATUS_LIVE_ALLOCATIONS while
 * SDK allocations remain and the term refuses.
 */
int rocketnet_term_ex( void );

// Replaces the allocator outside init, for tests that allocate without
// initializing the library. Both functions are required. Returns
// ROCKETNET_STATUS_OK, INVALID_ARGUMENT for a NULL function,
// ALREADY_INITIALIZED after init or during the transition, or
// LIVE_ALLOCATIONS with outstanding allocations. A refusal logs one line.
int rocketnet_allocator_ex( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

// The connection a public client holds (NULL while DISCONNECTED) and the
// running message server a public server holds (NULL while stopped), for
// tests and tools that read the layers underneath.
struct rocketnet_message_client_t * rocketnet_client_message_client( struct rocketnet_client_t * client );
struct rocketnet_message_server_t * rocketnet_server_message_server( struct rocketnet_server_t * server );

// The address a created server serves for one family, after the
// environment overrides. Writes it and returns true, or returns false when
// the server does not serve that family. Reads no socket.
bool rocketnet_server_served_address( const struct rocketnet_server_t * server, int family, char * address, size_t address_bytes );

// For tests that build the layers by hand: a public client holding an
// existing connection, and a public server holding an existing running
// message server; destroy destroys what was adopted. The adopted object has
// no block of its own, so a connect or start after the adopted one ends
// allocates its own, with default configs.
struct rocketnet_client_t * rocketnet_client_adopt( void * context, struct rocketnet_message_client_t * connection );
struct rocketnet_server_t * rocketnet_server_adopt( void * context, struct rocketnet_message_server_t * running );

// The secret bytes of an object, as spans of its allocation (offset, bytes):
// what destroy must leave zero when it frees the allocation, and the only
// bytes it may write. Each returns the span count, at most max. For the
// destroy tests, which see the allocation only in the allocator's free.
int rocketnet_server_secret_spans_for_test( size_t * offsets, size_t * bytes, int max );
int rocketnet_server_transport_secret_spans_for_test( size_t * offsets, size_t * bytes, int max );
int rocketnet_client_transport_secret_spans_for_test( size_t * offsets, size_t * bytes, int max );

#ifdef __cplusplus
}
#endif

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
    // pure POSIX, portable transport only -- the linux bucket keeps netlink,
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
  #elif defined(_MSC_VER) && ( defined(_M_ARM) || defined(_M_ARM64) )
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

// The process-wide assert callback the rocketnet_assert macro calls. It is internal
// and never exported: a caller installs a callback with rocketnet_assert_function,
// the only way in, and NULL there restores the default.
#if defined( __GNUC__ ) || defined( __clang__ )
    #define ROCKETNET_INTERNAL __attribute__((visibility("hidden")))
#else
    #define ROCKETNET_INTERNAL
#endif

extern ROCKETNET_INTERNAL rocketnet_assert_function_t rn_assert_function;

#if ROCKETNET_ASSERTS
#define rocketnet_assert( condition )                                                            \
do                                                                                          \
{                                                                                           \
    if ( !(condition) )                                                                     \
    {                                                                                       \
        rn_assert_function( #condition, __func__, __FILE__, __LINE__ );                     \
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
bool rocketnet_library_initialized( void );
/** @brief Internal helper that increments the process-wide refused-call count. */
void rocketnet_api_misuse_record( void );

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

// Internal to the SDK. The transport updates and the message object's update
// clamp the caller's clock once per update, before any read of it, through this
// helper: a non-finite time, or a time below the last good one, becomes the
// last good time. A clamp sets *clamped, counts one misuse and fires
// rocketnet_assert; a release build clamps and goes on, a debug build is loud.
// It is also the client create's check, seeded with a zero last good time. The
// message object clamps first and passes the clamped value on to its transport,
// so a bad time is counted once, never stored, and never becomes the clock
// origin. One compare per update, none per packet.
/** @brief Internal monotonic-time guard used by transport and message-object updates. @param last_good Previous accepted time in seconds. @param time Candidate time in seconds. @param clamped Optional output set true when candidate is rejected; NULL is allowed. @return Candidate time if finite and nondecreasing, otherwise last_good. Invalid time records misuse and fires the configured assertion. */
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

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES % 8 == 0, "the payload capacity is a multiple of eight" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES < ROCKETNET_MAX_PACKET_BYTES, "the payload capacity fits the send budget" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_RECEIVE_PAYLOAD_BYTES == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES,
                         "send and receive payload bounds match" );

// -----------------------------------------
//                  transport config tables
// -----------------------------------------

// The per-transport config system (#106). Each transport's config is
// declared ONCE, in a table below: field name, type, default, valid range.
// Everything else derives from the table mechanically -- the struct field,
// the environment variable name (ROCKETNET_ prefix plus the endpoint and
// field name uppercased: cfg.transport -> ROCKETNET_SERVER_TRANSPORT or
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
/** @brief Config-field flag for a field no environment variable sets: the public server's own keypair, which create generates. */
#define ROCKETNET_CONFIG_NO_ENV                 (1<<2)

/** @brief Zero initializer for fixed-size byte-array configuration fields. */
#define ROCKETNET_CONFIG_ZERO_BYTES             {0}

// The client transport config table: filter value, fragmentation shape
// (zero means the defaults above), and the transport candidate list.
//
//   INT_FIELD( name, default, min, max, flags )
//   UINT64_FIELD( name, default, flags )
//   BYTES_FIELD( name, size, default, flags )
//   STRING_FIELD( name, capacity, default, flags )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V1( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                    \
    BYTES_FIELD(  packet_filter,          4,                              ROCKETNET_TEST_FILTER_VALUE,                          0 )                          \
    INT_FIELD(    max_fragmented_messages,  0,                              1, ROCKETNET_MAX_FRAGMENTS,                            ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( transport,                ROCKETNET_TRANSPORT_STRING_LENGTH, "auto",                                               0 )                          \
    UINT64_FIELD( game_clock_epoch,      0,                                                                                    0 )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V2( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                    \
    INT_FIELD(    simulated_loss_percent,      0,                         0, 100,                                              ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_latency_ms,        0,                         0, 60000,                                            ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_jitter_ms,         0,                         0, 60000,                                            ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_duplicate_percent, 0,                         0, 100,                                              ROCKETNET_CONFIG_NO_ENV )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V3( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                    \
    INT_FIELD(    simulated_drop_every_nth,    0,                         2, 1000000,                                          ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    simulated_reverse_window,    0,                         2, 64,                                               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                       \
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V1( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                        \
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V2( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                        \
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V3( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )

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
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_ORIGINAL ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, time_callback ) + sizeof( void * ), sizeof( void * ) )
/** @brief Size in bytes of client transport config revision 1: with game_clock_epoch and step_driven, before the public link simulator fields. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, step_driven ) + sizeof( uint64_t ), sizeof( uint64_t ) )
/** @brief Size in bytes of the client transport config with the public link simulator fields, before the deterministic drop and reverse patterns. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_PATTERNS ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( void * ) )
/** @brief Current known client transport config size, in bytes: the deterministic drop and reverse patterns. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, simulated_reverse_window ) + sizeof( int ), sizeof( void * ) )
/** @brief Current known client transport config size, in bytes. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2 ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1
/** @brief Current known client transport config size, in bytes. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_KNOWN_BYTES ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_ORIGINAL, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_PATTERNS, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1

// The server transport config table -- the single declaration everything
// derives from, per the config system comment above. num_xdp_queues' range
// here is structural; the tighter bounds are per transport, enforced at the
// ladder rung (ROCKETNET_GSO_MAX_QUEUES).

#define ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                       \
    UINT64_FIELD( protocol_id,           0,                                                                                    0 )                          \
    INT_FIELD(    max_clients,        1000,                              1, ROCKETNET_MAX_CLIENTS,                             0 )                          \
    BYTES_FIELD(  packet_filter,          4,                              ROCKETNET_TEST_FILTER_VALUE,                          0 )                          \
    BYTES_FIELD(  backend_public_key,   32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          0 )                          \
    BYTES_FIELD(  server_public_key,    32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          ROCKETNET_CONFIG_NO_ENV )               \
    BYTES_FIELD(  server_secret_key,    32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          ROCKETNET_CONFIG_SECRET | ROCKETNET_CONFIG_NO_ENV )    \
    INT_FIELD(    num_xdp_queues,        0,                              1, 1024,                                              ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    num_os_cpus,           0,                              1, 256,                                               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    max_fragmented_messages,  0,                              1, ROCKETNET_MAX_FRAGMENTS,                            ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( transport,                ROCKETNET_TRANSPORT_STRING_LENGTH, "auto",                                               0 )                          \
    STRING_FIELD( public_address,        ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "",                                              ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    max_timeout_seconds,   60,                             1, 3600,                                              0 )                          \
    INT_FIELD(    handshake_deadline_seconds, 5,                         1, 60,                                                0 )                          \
    UINT64_FIELD( game_clock_epoch,           0,                                                                              0 )                          \
    INT_FIELD(    simulated_loss_percent,      0,                         0, 100,                                              ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_latency_ms,        0,                         0, 60000,                                            ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_jitter_ms,         0,                         0, 60000,                                            ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_duplicate_percent, 0,                         0, 100,                                              ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_drop_every_nth,    0,                         2, 1000000,                                          ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    simulated_reverse_window,    0,                         2, 64,                                               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( second_public_address, ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "",                                              ROCKETNET_CONFIG_NO_ENV )

/** @brief Size in bytes of the server transport config before game_clock_epoch. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1_ORIGINAL ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, handshake_deadline_seconds ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Size in bytes of server transport config revision 1: before the public link simulator fields. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, game_clock_epoch ) + sizeof( uint64_t ), sizeof( uint64_t ) )
/** @brief Size in bytes of the server transport config with the public link simulator fields, before the deterministic drop and reverse patterns. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2_PRE_PATTERNS ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Server transport config revision 2, in bytes: the deterministic drop and reverse patterns. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, simulated_reverse_window ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Server transport config revision 3, in bytes: the other family's public address, appended. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_3 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, second_public_address ) + ROCKETNET_MAX_ADDRESS_STRING_LENGTH, sizeof( uint64_t ) )
/** @brief Known server transport config sizes, in bytes. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_KNOWN_BYTES ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1_ORIGINAL, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2_PRE_PATTERNS, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_3

#if ROCKETNET_MAX_CHANNELS < 1 || ROCKETNET_MAX_CHANNELS > 65536
#error ROCKETNET_MAX_CHANNELS must be between 1 and 65536
#endif

ROCKETNET_STATIC_ASSERT( ROCKETNET_MESSAGE_SINGLE_BYTES >= 1400, "a 1400-byte game packet is one SINGLE message, zero copy, unfragmented" );

// The message config table, one declaration deriving struct, defaults and
// validation, as the transports' tables (docs/CHANNELS.md 12). Fields marked
// WIRE must be identical on both ends; CONFIG verifies them (layer 7).
// Validation applies the ranges here and the cross rules of section 14, and
// refuses, never clamps. The public env pass reads every row but the two
// budget rows under the endpoint's prefix (sdk/rocketnet_api.cpp:119
// and :125); the budget's one variable is the
// endpoint's budget field, ROCKETNET_CLIENT_SEND_KBPS or
// ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS.

#define ROCKETNET_MESSAGE_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                       \
    INT_FIELD( num_channels,               32,      1, ROCKETNET_MAX_CHANNELS, 0 )   /* WIRE */                            \
    INT_FIELD( reliable_messages_in_flight,          1024,     64, 1 << 20,                0 )   /* WIRE, power of two */              \
    INT_FIELD( receive_message_buffer_bytes,    1048576,   4096, 1 << 20,                0 )   /* WIRE, multiple of 16 */            \
    INT_FIELD( max_message_bytes,     1048576,   ROCKETNET_MESSAGE_SINGLE_BYTES, ROCKETNET_MAX_MESSAGE_BYTES, 0 )   /* WIRE, <= receive_message_buffer_bytes */     \
    INT_FIELD( send_message_buffer_bytes,     4194304,  12288, 1 << 28,                0 )                                         \
    INT_FIELD( send_queue_messages,      1024,     16, 1 << 20,                0 )                                         \
    INT_FIELD( packets_in_flight,         256,     16, 1 << 16,                0 )                                         \
    INT_FIELD( packets_per_update,    256,      1, 1024,                   0 )                                         \
    INT_FIELD( unreliable_messages_per_update,    1024,      1, 1 << 16,                0 )   /* per connection per update */       \
    INT_FIELD( sequenced_messages_held,            16,      0, 4096,                   0 )                                         \
    INT_FIELD( max_message_receipts,              256,     16, 1 << 16,                0 )                                         \
    INT_FIELD( message_resend_delay_initial_ms,           2000,     10, 60000,                  0 )                                         \
    INT_FIELD( message_resend_delay_min_ms,                  0,      0, 60000,                  0 )                                         \
    INT_FIELD( message_resend_delay_max_ms,               2000,     10, 60000,                  0 )                                         \
    INT_FIELD( reliable_message_timeout_ms,     10000,   1000, 600000,                 0 )                                         \
    INT_FIELD( client_send_kbps,         ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, INT32_MIN, ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 ) \
    INT_FIELD( server_send_kbps,         ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, INT32_MIN, ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 )

#define ROCKETNET_MESSAGE_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_message_config_t, reliable_message_timeout_ms ) + sizeof( int ), sizeof( int ) )
#define ROCKETNET_MESSAGE_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_message_config_t, server_send_kbps ) + sizeof( int ), sizeof( int ) )
#define ROCKETNET_MESSAGE_CONFIG_KNOWN_BYTES ROCKETNET_MESSAGE_CONFIG_BYTES_1, ROCKETNET_MESSAGE_CONFIG_BYTES_2

// The platform's network lookups, for unit tests only. On Linux,
// rocketnet_platform_init (sdk/rocketnet_platform.cpp:391) classifies the
// connection with get_connection_type (sdk/rocketnet_platform.cpp:1763):
// getifaddrs (:1774), an AF_INET ioctl socket (:1782) and a netlink route
// dump (:1851). rocketnet_platform_client_ipv6_available
// (sdk/rocketnet_platform.cpp:4345) opens and closes an AF_INET6 socket
// (:4361). With a table set, each reads its entry instead and opens nothing
// (:391, :4352); NULL, the default, runs the real lookups. The unit suite sets
// a fake before rocketnet_init (test.cpp:106) so ./bin/test opens no socket.
// Not exported.

struct rocketnet_platform_network_lookups_t
{
    int ( *connection_type )( void );
    bool ( *ipv6_available )( void );
};

void rocketnet_platform_set_network_lookups_for_testing( const struct rocketnet_platform_network_lookups_t * lookups );

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

// Internal final-burst operations for public server_stop. They preserve the
// normal transport API while selecting a nonwaiting backend send on shutdown.
void rocketnet_server_transport_disconnect_client_shutdown( struct rocketnet_server_transport_t * transport, int client_index );
void rocketnet_server_transport_send_packets_shutdown( struct rocketnet_server_transport_t * transport );

#endif // #ifndef ROCKETNET_INTERNAL_H

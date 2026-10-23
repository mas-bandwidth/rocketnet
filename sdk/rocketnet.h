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


// The packet size: the UDP payload of one datagram on a 1500-byte MTU link,
// the number every other size limit derives from (STANDARD.md _Packet
// sizes_). Over IPv4 it is 1500 - 20 - 8 = 1472 bytes, the receive ceiling;
// over IPv6 it is 1500 - 40 - 8 = 1452 bytes, the send budget, so a maximum
// packet crosses a 1500-MTU path unfragmented on either family. The internal
// wire header (rocketnet_wire.h) derives the same two numbers from the same
// MTU, and rocketnet_constants.h asserts they agree.

#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 20 - 8 )
#define ROCKETNET_MAX_PACKET_BYTES_IPV6                ( 1500 - 40 - 8 )

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
#define ROCKETNET_PLATFORM_BSD                                         5
#define ROCKETNET_PLATFORM_MAX                                         6

#if defined(_WIN32)
#define NOMINMAX
#endif

#if defined( ROCKETNET_SHARED ) && defined( _WIN32 )
    #ifdef ROCKETNET_EXPORT
        #define ROCKETNET_SYMBOL __declspec(dllexport)
    #else
        #define ROCKETNET_SYMBOL __declspec(dllimport)
    #endif
#elif defined( ROCKETNET_SHARED ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
    #define ROCKETNET_SYMBOL __attribute__((visibility("default")))
#else
    #define ROCKETNET_SYMBOL
#endif

#ifdef __cplusplus
    #define ROCKETNET_EXPORT_FUNC extern "C" ROCKETNET_SYMBOL
    #define ROCKETNET_EXPORT_DATA extern "C" ROCKETNET_SYMBOL
#else
    #define ROCKETNET_EXPORT_FUNC extern ROCKETNET_SYMBOL
    #define ROCKETNET_EXPORT_DATA extern ROCKETNET_SYMBOL
#endif

// The number of unique ROCKETNET_EXPORT_FUNC entry points compiled into the
// non-development library (ROCKETNET_DEVELOPMENT=0). The public-API NULL audit
// in sdk/rocketnet_tests.cpp holds a mirror of these names and fails when the
// two drift, so a new exported entry point without a NULL case is caught at
// test time. rocketnet_init is declared once for C and once for C++ and counts
// once; the development-only rocketnet_write_dev_connect_token is not in this
// count (its NULL behaviour is covered by the development library's own tests).
#define ROCKETNET_NUM_EXPORTED_FUNCTIONS 132

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

// -----------------------------------------

// SDK ABI revision, independent of the wire version. Existing public layouts
// remain unchanged. Increment on any incompatible layout/calling-contract change.
#define ROCKETNET_ABI_VERSION 1

// Safe before init; no allocation and no caller-owned struct writes. Type names
// are the public C struct names. Unknown types/fields return SIZE_MAX for size
// and offset, and -1 for field count. This describes the eight transport/config/
// packet/frame-sync/message-config structs in this header, not opaque internal
// objects such as the message client and server.
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_abi_version( void );
// a NULL type is a counted no-op that returns SIZE_MAX.
ROCKETNET_EXPORT_FUNC size_t rocketnet_abi_type_size( const char * type );
// a NULL type is a counted no-op that returns -1.
ROCKETNET_EXPORT_FUNC int rocketnet_abi_field_count( const char * type );
// a NULL type or field is a counted no-op that returns SIZE_MAX.
ROCKETNET_EXPORT_FUNC size_t rocketnet_abi_field_offset( const char * type, const char * field );
// a NULL type or field is a counted no-op that returns SIZE_MAX.
ROCKETNET_EXPORT_FUNC size_t rocketnet_abi_field_size( const char * type, const char * field );

#ifdef __cplusplus
ROCKETNET_EXPORT_FUNC bool rocketnet_init( void * context = NULL );
#else
ROCKETNET_EXPORT_FUNC bool rocketnet_init( void * context );
#endif

ROCKETNET_EXPORT_FUNC void rocketnet_term();

// -----------------------------------------

ROCKETNET_EXPORT_DATA void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line );

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

// the level currently in force, so a caller that moves the level for a
// bounded stretch (the test suite's log-asserting tests) can restore what
// it found rather than guessing

ROCKETNET_EXPORT_FUNC int rocketnet_get_log_level( void );

// the name of a level, so a replacement log function renders it the way the
// default one does

ROCKETNET_EXPORT_FUNC const char * rocketnet_log_level_string( int level );

ROCKETNET_EXPORT_FUNC void rocketnet_log_function( void (*function)( int level, const char * format, ... ) );

// a NULL format is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_printf( int level, const char * format, ... );

// a NULL format is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_info( const char * format, ... );

// a NULL format is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_warn( const char * format, ... );

// a NULL format is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_error( const char * format, ... );

// a NULL format is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_debug( const char * format, ... );

// a NULL format is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_spam( const char * format, ... );

// ------------------------------------------

// Configure before init, with no outstanding allocations. Invalid or late
// replacements are refused: the call returns false, logs one line and counts
// the refusal in rocketnet_api_misuse_count. Refused after init, while
// transitioning, with live allocations, or with a partial set (either callback
// NULL). Returns true when both callbacks were taken. Callbacks must be
// thread-safe and return storage aligned for fundamental types. RocketNet adds
// alignment/ownership metadata, so callbacks receive larger sizes and their
// original pointer on free.
// a NULL malloc_function or free_function is a counted no-op that returns false.
ROCKETNET_EXPORT_FUNC bool rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

// Counts every refused API misuse since the process started: a create before
// rocketnet_init, a late or partial rocketnet_allocator, and any other call the
// SDK refuses. A relaxed read; a diagnostic, not a synchronization point.
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_api_misuse_count();

// Internal to the SDK. rocketnet_library_initialized reads the lifecycle state
// once for an entry point that must refuse before init; the create paths call
// it at entry, never per packet. rocketnet_api_misuse_record increments the
// count above from any refused misuse. The default allocator callbacks let a
// caller restore them after taking the allocator.
bool rocketnet_library_initialized();
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
void * rocketnet_default_malloc_function( void * context, size_t bytes );
void rocketnet_default_free_function( void * context, void * p );

// Internal to the SDK. Both transport updates clamp the caller's clock once per
// update, before any read of it, through this helper: a non-finite time, or a
// time below the last good one, becomes the last good time. A clamp sets
// *clamped, counts one misuse and fires rocketnet_assert; a release build
// clamps and goes on, a debug build is loud. It is also the client create's
// check, seeded with a zero last good time. One compare per update, none per
// packet.
double rocketnet_clamp_update_time( double last_good, double time, bool * clamped );

#define ROCKETNET_ALLOCATION_ALIGNMENT 64

// Returns 64-byte-aligned storage, or NULL on overflow/allocation failure.
// Free only through rocketnet_free; the original allocator/context are retained.
ROCKETNET_EXPORT_FUNC void * rocketnet_malloc( void * context, size_t bytes );

ROCKETNET_EXPORT_FUNC void rocketnet_free( void * context, void * p );

ROCKETNET_EXPORT_FUNC void rocketnet_clear_and_free( void * context, void * p, size_t p_size );

// -----------------------------------------

// a NULL buffer is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC const char * rocketnet_user_id_string( uint64_t user_id, char * buffer, size_t buffer_size );

ROCKETNET_EXPORT_FUNC float rocketnet_random_float();

ROCKETNET_EXPORT_FUNC uint8_t rocketnet_random_uint8();

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_random_uint16();

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_random_uint32();

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_random_uint64();

// a NULL data is a counted no-op that writes nothing.
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
    *value = __builtin_bswap64( *value );
#else // #ifdef __GNUC__
    *value = ( *value & 0x00000000FFFFFFFF ) << 32 | ( *value & 0xFFFFFFFF00000000 ) >> 32;
    *value = ( *value & 0x0000FFFF0000FFFF ) << 16 | ( *value & 0xFFFF0000FFFF0000 ) >> 16;
    *value = ( *value & 0x00FF00FF00FF00FF ) << 8  | ( *value & 0xFF00FF00FF00FF00 ) >> 8;
#endif // #ifdef __GNUC__
#endif // #if ROCKETNET_BIG_ENDIAN
}

#endif // #ifdef __cplusplus

// -----------------------------------------

// a NULL dest or source is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size );

// -----------------------------------------
//                                  client
// -----------------------------------------

#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED         -7
#define ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN         -6
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT          -5
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT -4
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT  -3
#define ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT      -2
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED             -1
#define ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED                   0
#define ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE              1
#define ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST     2
#define ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE    3
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED                      4

// The send-side payload capacity: the send budget (ROCKETNET_MAX_PACKET_BYTES_IPV6,
// 1452) less the 26-byte fixed overhead and the 7-byte minimum ack block
// leaves 1419; the capacity is the largest multiple of eight inside that,
// 1416, keeping the documented guarantee that any word-based serializer can
// root at the payload pointer directly. The receive side is not configurable
// and accepts payloads up to the receive ceiling (ROCKETNET_MAX_PACKET_BYTES,
// 1472) less the same overhead, 1439 bytes, whatever this end's own budget,
// so receive buffers size by the receive constant. The overhead and the block
// are internal constants this header cannot see: the values are written here
// with their derivation, and rocketnet_constants.h asserts each equals it.

#define ROCKETNET_MAX_PAYLOAD_BYTES                        1416    // ( ( 1452 - 26 - 7 ) / 8 ) * 8
#define ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES                1439    // 1472 - 26 - 7

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PAYLOAD_BYTES % 8 == 0, "the payload capacity is a multiple of eight" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PAYLOAD_BYTES < ROCKETNET_MAX_PACKET_BYTES_IPV6, "the payload capacity fits the send budget" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES - ROCKETNET_MAX_PAYLOAD_BYTES >=
                         ROCKETNET_MAX_PACKET_BYTES - ROCKETNET_MAX_PACKET_BYTES_IPV6, "the receive payload is the capacity plus the family difference and the rounding" );

// Payloads above the wire capacity fragment transparently: request the
// capacity at start_packet and the transport routes the payload through
// its arena, splitting it into type 9 fragments at finish -- unreliable
// semantics stand, a lost fragment loses the whole payload, and the
// fragment counters make it visible. The configured maximum payload
// bounds both what this end will fragment and what it will reassemble;
// storage for both is allocated once at create, to the maximum. The
// hard ceiling is 256 fragments at the full stride: 305,408 bytes.

#define ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES                16384
#define ROCKETNET_DEFAULT_REASSEMBLY_SLOTS                 8

ROCKETNET_EXPORT_FUNC const char * rocketnet_client_transport_state_name( int state );

struct rocketnet_client_transport_t;
struct rocketnet_counters_t;
struct rocketnet_packet_context_t;      // defined below, with the send surface

#define ROCKETNET_CONNECT_TOKEN_BYTES 768

// -----------------------------------------
//                        transport config
// -----------------------------------------

// The per-transport config system (#106). Each transport's config is
// declared ONCE, in a table below: field name, type, default, valid range.
// Everything else derives from the table mechanically -- the struct field,
// the environment variable name (ROCKETNET_ prefix plus the endpoint and
// field name uppercased: cfg.driver -> ROCKETNET_SERVER_DRIVER or
// ROCKETNET_CLIENT_DRIVER), the parse, the validation,
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
//
// Env encodings: integers are decimal; byte fields are base64 of exactly
// the field's length (keygen prints keys in base64); the driver field is
// the ordered candidate list documented at the driver section below. An
// env var set to the empty string is ignored as if unset, with a log line.
//
// The one field class the table cannot carry is pointers (the client's
// state-change callback below): a pointer cannot ride an env var, so it
// sits outside the table and outside the env surface, stated here rather
// than discovered.

// The driver field: an ORDERED CANDIDATE LIST, comma separated, tried left
// to right at create -- first grant wins, and a list exhausted by REFUSAL
// fails loudly. "auto" (the default, also the empty string) expands to
// this platform's recommended order for the endpoint: linux server
// xdp,gso,portable; macOS msgx,portable; everywhere else portable
// (clients: portable everywhere, plus msgx on macOS). A one-element list
// is a pin: that driver or loud failure, never a silent fall. A name in
// the known transport set (xdp, gso, msgx, portable) but absent on this
// platform -- or not applicable to this endpoint, like xdp for a client --
// is IGNORED with one soft log line, so one fleet-wide config line deploys
// unmodified to heterogeneous boxes; a list whose EVERY name is ignored
// names no driver for this endpoint at all and falls to the endpoint's own
// auto order, loudly, rather than bricking an endpoint the line was not
// addressing. A name in no transport set anywhere is a typo and fails
// loudly at parse, listing the valid names.

#define ROCKETNET_DRIVER_STRING_LENGTH 64

// field flags: ZERO_MEANS_DEFAULT admits zero as "use the built-in
// default" beside the stated range; SECRET redacts the value in override
// logging

#define ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT     (1<<0)
#define ROCKETNET_CONFIG_SECRET                 (1<<1)

#define ROCKETNET_CONFIG_ZERO_BYTES             {0}

// The client transport config table: filter value, fragmentation shape
// (zero means the defaults above), and the driver candidate list.
//
//   INT_FIELD( name, default, min, max, flags )
//   UINT64_FIELD( name, default, flags )
//   BYTES_FIELD( name, size, default, flags )
//   STRING_FIELD( name, capacity, default, flags )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                       \
    BYTES_FIELD(  filter_value,          4,                              ROCKETNET_TEST_FILTER_VALUE,                          0 )                          \
    INT_FIELD(    max_payload_bytes,     0,                              ROCKETNET_MAX_PAYLOAD_BYTES,                                                       \
                                                                         ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES,               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    num_reassembly_slots,  0,                              1, ROCKETNET_MAX_FRAGMENTS,                            ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( driver,                ROCKETNET_DRIVER_STRING_LENGTH, "auto",                                               0 )

#define ROCKETNET_CONFIG_STRUCT_INT( name, def, min, max, flags )       int name;
#define ROCKETNET_CONFIG_STRUCT_UINT64( name, def, flags )              uint64_t name;
#define ROCKETNET_CONFIG_STRUCT_BYTES( name, size, def, flags )         uint8_t name[size];
#define ROCKETNET_CONFIG_STRUCT_STRING( name, capacity, def, flags )    char name[capacity];

// The client connects with a token, never an address: the token names the
// server, carries the keys, and is the credential -- minted by a backend in
// production, or locally by rocketnet_write_dev_connect_token against the
// published development keys. The config carries what the token does not:
// the per-title filter value, the fragmentation shape -- the largest
// payload this client sends and reassembles, and the per-connection
// reassembly slot count, zero meaning the defaults above; the client's
// delivered mirror sizes itself to the client's own drain bound, about
// 8.4MB at the default maximum -- the driver candidate list, and an
// optional state-change callback. NULL config is the development posture:
// the published test filter, the defaults, no callback.

struct rocketnet_client_transport_config_t
{
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
    void (*state_change_callback)( void * context, int previous_state, int current_state );
};

// a NULL config is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_default_config( struct rocketnet_client_transport_config_t * config );

#if ROCKETNET_DEVELOPMENT
// a NULL server_address_string or connect_token is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_write_dev_connect_token( const char * server_address_string, uint64_t client_id, uint64_t unix_timestamp, uint8_t * connect_token );
#endif // #if ROCKETNET_DEVELOPMENT

// time is the game's clock, a double in seconds, monotonic; pass the same
// clock to every call that takes one. The library never reads a clock of
// its own on these paths, so a game owns time and a test can hold it still.

// a NULL connect_token is a counted no-op that returns NULL; a NULL config is
// the default shape.
ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time );

// a NULL client_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_destroy( struct rocketnet_client_transport_t * client_transport );      // disconnect and pump a few updates first

// a NULL client_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_update( struct rocketnet_client_transport_t * client_transport, double time );

// a NULL client_transport or packet_data is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_send_packet( struct rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes );

// The copying cursor: pops the next delivered payload into packet_data,
// returning its length, or zero when the update's batch is drained.
// packet_capacity is the caller's buffer size: a payload larger than it --
// possible only for reassembled fragmented payloads, which reach the
// configured maximum -- is refused whole and counted on
// RECEIVE_CURSOR_TOO_SMALL, never truncated, and the cursor moves on to
// the next payload that fits. Size the buffer by
// rocketnet_client_transport_max_payload_bytes to never refuse.

// a NULL client_transport or packet_data is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_receive_packet( struct rocketnet_client_transport_t * client_transport, uint8_t * packet_data, int packet_capacity );

// the configured maximum payload this transport sends and reassembles --
// the config's max_payload_bytes after defaulting -- so a caller can size
// its receive buffers without carrying the config around

// a NULL client_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_max_payload_bytes( struct rocketnet_client_transport_t * client_transport );

// the batch behind the copying cursor above: by count and index, and as
// parallel arrays in one call. valid until the next update. reassembled
// fragmented payloads take their place here in arrival position, at the
// moment they complete, up to the configured maximum payload size.

// a NULL client_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_num_payloads( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport or payload_bytes is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_transport_payload( struct rocketnet_client_transport_t * client_transport, int payload_index, int * payload_bytes );

// a NULL client_transport, payload_data or payload_bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_payloads( struct rocketnet_client_transport_t * client_transport, uint8_t * const ** payload_data, const int ** payload_bytes );

// the client's acquire/commit send family: the same shape, layout and
// threading contract as the server's, minus the client index. NOTE:
// rocketnet_client_transport_send_packet (the convenience above) flushes the ring
// inline and is therefore main-thread only, never during an open fill --
// it is the simple form, not a fill-phase participant.

// a NULL client_transport or context is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_transport_start_packet( struct rocketnet_client_transport_t * client_transport, int request_bytes, struct rocketnet_packet_context_t * context );

// Validates an active packet lease and returns its payload view. NULL/zero on
// stale, consumed, modified or wrong-owner context. The view expires at finish,
// abort, update, flush or destroy; callers must join before those operations.
// a NULL transport, context or capacity is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_client_transport_packet_payload( struct rocketnet_client_transport_t * transport, const struct rocketnet_packet_context_t * context, int * capacity );

// a NULL client_transport or context is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_finish_packet( struct rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes );

// a NULL client_transport or context is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_abort_packet( struct rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context );

// a NULL client_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_send_packets( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_state( struct rocketnet_client_transport_t * client_transport );

// the granted driver, request-vs-granted style: the name of the candidate
// that won the ladder at create ("portable", "msgx"), never the requested
// list. any performance claim attaches to what this returns.

// a NULL client_transport is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC const char * rocketnet_client_transport_driver( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_client_index( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_disconnect( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_client_transport_counters( struct rocketnet_client_transport_t * client_transport );

// The client's connection quality: rtt and jitter in milliseconds, packet
// loss as a percentage, and the last second's packet rates and bandwidth
// both ways. The rates come from elapsed-time buckets (about 4 ms boundary
// resolution), filled by existing send/receive walks over the trailing second,
// so the cost per update is flat and no walk is added for them. rtt seeds
// from the handshake's round trips and is continuously refreshed by the
// acks -- the first block acknowledging each sent sequence yields a
// sample; jitter is inter-arrival jitter over the session sequence; packet
// loss is receive-side, from gaps in the session sequence. These numbers
// feed time synchronization.

struct rocketnet_client_transport_stats_t
{
    float rtt;                                      // round trip time (milliseconds)
    float jitter;                                   // inter-arrival jitter (milliseconds)
    float packet_loss;                              // percent
    int packets_sent_per_second;
    int packets_received_per_second;
    int bandwidth_sent_kbps;
    int bandwidth_received_kbps;
};

// a NULL client_transport or stats is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_stats( struct rocketnet_client_transport_t * client_transport, struct rocketnet_client_transport_stats_t * stats );

// The client's delivery notification batch: the packet ids newly
// acknowledged by the last update, valid until the next update. The packet
// id is the one handed out in rocketnet_packet_context_t at start_packet.
// Overflow past the batch's capacity is deferred to a later update rather
// than lost -- ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL counts each
// deferral.

// a NULL client_transport or packet_ids is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_client_transport_acked_packets( struct rocketnet_client_transport_t * client_transport, const uint64_t ** packet_ids );

// -----------------------------------------
//                                  server
// -----------------------------------------

#define ROCKETNET_SERVER_TRANSPORT_RUNNING                    0
#define ROCKETNET_SERVER_TRANSPORT_STOPPING                   1
#define ROCKETNET_SERVER_TRANSPORT_STOPPED                    2

struct rocketnet_server_transport_t;
struct rocketnet_address_t;

// The admission gate: a callback the application installs on a server
// transport, consulted for every received packet whose source has no
// encryption mapping -- traffic that has not cleared the connect handshake --
// before it reaches the protocol's process step. Return
// ROCKETNET_SERVER_ADMISSION_ADMIT to process the packet, or
// ROCKETNET_SERVER_ADMISSION_DROP to drop it before any crypto work runs.
// A mapped source (an in-flight handshake or an established connection)
// bypasses the gate: it has already cleared the connect ladder, and this gate
// is about admission, not session policing. The callback runs on the receive
// thread and must be fast and thread-safe.

#define ROCKETNET_SERVER_ADMISSION_ADMIT 0
#define ROCKETNET_SERVER_ADMISSION_DROP  1
typedef int (*rocketnet_server_admission_callback_t)( void * context, const struct rocketnet_address_t * from, const uint8_t * packet_data, int packet_bytes );

// The server is configured explicitly. rocketnet_server_transport_default_config gives
// the shape with no keys -- create refuses it until keys are supplied
// (through the struct or through the env surface), because default keys are
// how every deployment of a protocol ends up sharing one identity.
// rocketnet_server_transport_dev_config fills the published
// development keys and test filter for local work, loudly. NULL config is
// the default shape. num_xdp_queues
// is the staged drivers' queue count -- XDP queues on the xdp driver,
// reuseport sockets on the gso driver -- kept under its historic name for
// ABI stability with the language bindings; num_os_cpus configures the XDP
// driver's core layout. zero means the driver default, and the portable
// driver ignores both.
//
// The fragmentation shape rides max_payload_bytes and
// num_reassembly_slots: the largest payload this server sends and
// reassembles, and the per-client reassembly slot count. zero means the
// defaults. budget arithmetic, all spent once at create, approximately --
// slot records ride along: the reassembly arena is max_clients x slots x
// (max payload + 1433 staging, ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES) per the standard; this implementation's
// delivered mirror of slots x max payload per client -- the per-update
// home of completed payloads, sized to the receive drain's completion
// bound so a completed payload always has a home; and the ack endpoints'
// fragment aggregate rings, about 48KB per client slot.
//
// The server transport config table -- the single declaration everything
// derives from, per the config system comment above. public_address, empty
// by default, displaces the create call's address parameter when set --
// struct or ROCKETNET_PUBLIC_ADDRESS (deprecated alias:
// ROCKETNET_SERVER_PUBLIC_ADDRESS) -- so the address the driver ladder
// verifies, the address the driver binds, and the address connect tokens
// validate against are always the same one. the ENV form is scoped to the
// first server create in the process and then consumed, loudly: the
// deployed binary's one server gets its redirect, and a multi-transport
// process is never wholesale redirected by one ambient variable. the
// struct form is per create. num_xdp_queues' range
// here is structural; the tighter bounds are PER DRIVER, enforced at the
// ladder rung and refused loudly, never clamped: the xdp driver takes any
// count from 1, the gso driver refuses past its 32 queue geometry
// (ROCKETNET_GSO_MAX_QUEUES).

// The handshake deadline is fixed at the challenge, never refreshed, and discharged on connect.
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                       \
    UINT64_FIELD( protocol_id,           0,                                                                                    0 )                          \
    INT_FIELD(    max_clients,          64,                              1, ROCKETNET_MAX_CLIENTS,                             0 )                          \
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
    INT_FIELD(    handshake_deadline_seconds, 5,                         1, 60,                                                0 )

struct rocketnet_server_transport_config_t
{
    ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

// a NULL config is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_default_config( struct rocketnet_server_transport_config_t * config );

// a NULL config is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_dev_config( struct rocketnet_server_transport_config_t * config );

// Allocation-free constructor memory plan for one built-in driver name.
// Includes SDK allocator requests (alignment/ownership overhead and transient
// worker shims) plus XDP UMEM. Excludes OS stacks, kernel/libbpf/libxdp resources,
// loader/libc bookkeeping and application memory; this is not a host RAM limit.
// Uses the supplied numeric config exactly (NULL means defaults); no environment
// overlay, key validation, socket probe or budget refusal. Zero means invalid
// geometry, overflow, unknown/custom driver, or driver not built for this platform.
// Query each candidate separately: "auto" and driver lists are not concrete plans.
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_server_transport_required_memory( const struct rocketnet_server_transport_config_t * config, const char * driver );

// a NULL public_address_string is a counted no-op that returns NULL; a NULL
// config is the default shape.
ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config);

// a NULL server_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport );      // stop and wait for stopped first

// a NULL server_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport );

// The port the transport actually bound. A public address string carrying
// port zero asks the kernel for an ephemeral port at create; the granted
// port is adopted into the advertised address -- so connect tokens minted
// for it validate -- and this reads it back. Tests and single-box tools
// build their token address from it instead of hardcoding a port and
// racing every other process on the machine; production servers know
// their public address already.
//
// Port-zero discovery is a per-driver property, and since #109 the ladder
// enforces it rather than documenting it: a driver whose create binds the
// port it is given and discovers nothing -- the XDP driver -- cannot serve
// a port-zero address, so that rung REFUSES one at create, loudly, naming
// the rule, before it binds anything. It is an ordinary rung refusal: an
// "auto" ladder falls past it to a driver that does discover, and only a
// PIN on a non-discovering driver refuses the create outright. The
// portable, msgx and GSO drivers discover; the XDP driver does not
// (rocketnet_driver_discovers_bound_port is the table).
//
// So this returns a real port by one of two routes, not one: either the
// public address carried a port, and that is the port -- including on an
// XDP grant, which binds what it was given and discovers nothing -- or it
// carried zero, in which case the rule above guarantees the granting
// driver was one that discovers. The port-zero route is asserted for
// every discovering driver a platform has by
// rocketnet_test_server_port_zero_needs_a_discovering_driver.

// a NULL server_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC uint16_t rocketnet_server_transport_port( struct rocketnet_server_transport_t * server_transport );

// the granted driver, request-vs-granted style: the name of the candidate
// that won the ladder at create ("xdp", "gso", "msgx", "portable"), never
// the requested list. the 1000s-of-players claims attach to the granted
// tier this returns, not the requested one -- one log line at create names
// the same grant and why every earlier candidate refused.

// a NULL server_transport is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC const char * rocketnet_server_transport_driver( struct rocketnet_server_transport_t * server_transport );

// the commit barrier for host processes that must never commit an XDP data
// plane (test harnesses, smokes): installed once, the xdp driver's create
// refuses before its first act; the non-mutating pre-flight stays
// reachable. a no-op on platforms without the xdp driver.

ROCKETNET_EXPORT_FUNC void rocketnet_forbid_xdp_commit( void );

// a NULL server_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport, double time );

// The admission gate, set per transport (see the callback type above). NULL
// callback -- the default -- admits every source: the connect ladder alone
// decides. May be set or cleared at any time between updates.

// a NULL server_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_set_admission_callback( struct rocketnet_server_transport_t * server_transport, rocketnet_server_admission_callback_t callback, void * context );

// The payloads delivered by the last update, valid until the next update.
// Iterated by index so the storage behind them stays the library's own --
// its layout and its capacity are not part of this surface. Reassembled
// fragmented payloads take their place in this batch in arrival position,
// at the moment they complete, up to the configured maximum payload size
// -- size any buffer you copy into by that configuration, not by the
// wire's ceiling.
//
//     int num_payloads = rocketnet_server_transport_num_payloads( server );
//     for ( int i = 0; i < num_payloads; i++ )
//     {
//         int client_index, payload_bytes;
//         uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &payload_bytes );
//         ...
//     }

// a NULL server_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_num_payloads( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport, client_index or payload_bytes is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_transport_payload( struct rocketnet_server_transport_t * server_transport, int payload_index, int * client_index, int * payload_bytes );

// The whole receive batch as parallel arrays in one call, valid until the
// next update -- the zero-per-item form of the accessors above, for
// bindings and tight loops. Returns the number of payloads.

// a NULL server_transport, client_index, payload_data or payload_bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_payloads( struct rocketnet_server_transport_t * server_transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes );

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
// request_bytes declares the capacity the caller needs, and the cost is
// declared with it. A request at or under ROCKETNET_MAX_PAYLOAD_BYTES is
// a real wire frame: written in place, sealed in place, zero copies. A
// request above it -- up to the configured maximum payload -- routes to
// the transport's arena, and finish_packet splits the written payload
// into type 9 fragments, each an ordinary sealed session packet, the
// copy spent only where splitting makes it unavoidable; an over-capacity
// request whose write turns out to fit one packet collapses to a single
// ordinary payload packet with one copy. Either way the context's one
// packet_id is the payload's: a fragmented payload's id surfaces in the
// acked batch when every fragment's wire packet is acknowledged. A lost
// fragment loses the whole payload -- the transport never resends.
//
// The context's payload_capacity reports the frame's real capacity -- the
// wire capacity on the wire route, the configured maximum on the arena
// route -- never the request itself: a caller may write up to it, and
// finish_packet routes on what was actually written.
//
// Threading contract: update and send_packets run on the main thread, and
// so do disconnect, disconnect_client and stop -- never while a fill is
// open. start_packet, finish_packet and abort_packet may run on any number
// of game threads strictly between an update and the following
// send_packets, and the handoff between fill and flush must be a
// synchronizing operation (a join or a barrier), not a wall-clock wait.
// Call send_packets after each joined fill phase. There is no required frame,
// update, send or ACK rate. When staged
// packets actually leave is per driver, and a caller owns only the
// guarantee: everything staged before send_packets is sent by it, and
// nothing is guaranteed sent without it. The portable and GSO drivers
// also flush update's own staging -- protocol responses and keep-alives
// -- inline in update; the XDP driver sends nothing until send_packets.
// Code against the guarantee, not a driver's eagerness. Calling
// send_packets again without newly staged packets is a no-op. The fields of the
// context are read-only to the caller. The transport privately owns and checks
// all metadata; a copy can be consumed once, and stale, modified or wrong-owner
// contexts are refused before dereferencing a frame. packet_id is an opaque,
// process-unique lease identifier, never driver bookkeeping. Active payload views
// expire at finish/abort or the next update/flush; raw pointers cannot be revoked.
//
// finish_packet returns the sealed wire size, or zero when the packet was
// dropped instead (payload out of range, the connection gone, or a wire
// frame it could not reserve) -- the sequences already taken are burned
// either way, which the receiver's replay window tolerates.
//
// HOW MANY are burned is not always one, and the difference is large
// enough to reason about. An unfragmented payload burns the single
// sequence start_packet took. A FRAGMENTED one burns up to
// fragment_count: finish reserves a block of fragment_count-1 above that
// first sequence, and a seal failure inside the fragment loop withdraws
// the whole reservation (on the staged xdp path, fragments already
// committed to its asynchronous staging stay committed, so the burn is
// the remainder). Since receive-side loss statistics are computed from
// gaps in the session sequence, a failed fragmented send moves that
// figure by UP TO its fragment count rather than by one -- at the default
// 16384-byte maximum payload that ceiling is 14. USAGE.md carries the
// worked arithmetic.

struct rocketnet_packet_context_t
{
    uint8_t * frame;
    int payload_index;
    int payload_capacity;
    uint64_t packet_id;
    uint64_t sequence;
    int client_index;
};

// a NULL server_transport or context is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_transport_start_packet( struct rocketnet_server_transport_t * server_transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context );

// Validates an active packet lease and returns its payload view. NULL/zero on
// stale, consumed, modified or wrong-owner context. The view expires at finish,
// abort, update, flush or destroy; callers must join before those operations.
// a NULL transport, context or capacity is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_server_transport_packet_payload( struct rocketnet_server_transport_t * transport, const struct rocketnet_packet_context_t * context, int * capacity );

// a NULL server_transport or context is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_finish_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes );

// a NULL server_transport or context is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_abort_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context );

// a NULL server_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_send_packets( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport or payload_data is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_send_packet( struct rocketnet_server_transport_t * server_transport, int client_index, const uint8_t * payload_data, int payload_bytes );

// Disconnects one connected client deliberately: the redundant disconnect
// burst is staged and the slot frees immediately. Buffered drivers may send
// full batches during this call; remaining packets leave on the next flush.
// Why is the game's decision; this is only the mechanism. Main thread only, never
// while a fill phase is open -- it tears down the connection state the
// fill relies on.

// a NULL server_transport is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_disconnect_client( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport );

// the configured maximum payload this transport sends and reassembles --
// the config's max_payload_bytes after defaulting -- so a caller can size
// the buffers it copies payloads into

// a NULL server_transport is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_max_payload_bytes( struct rocketnet_server_transport_t * server_transport );

// The server's whole-socket rates: packets and bandwidth per second, both
// directions, over the trailing second. Accumulation rides the walks the
// transport already makes -- the receive drain and the send flush -- into a
// ring of time-based buckets, and the published numbers sum only buckets
// younger than one second (about 4 ms boundary resolution), at bounded cost:
// no per-client sweep, no extra pass over any buffer. At 1000 clients the
// update pays the same fixed bucket sum it pays at one. These numbers feed
// time synchronization.

struct rocketnet_server_transport_stats_t
{
    uint64_t packets_sent_per_second;
    uint64_t packets_received_per_second;
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
};

// a NULL server_transport or stats is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_stats( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_transport_stats_t * stats );

// Per-client connection quality, per slot: rtt and jitter in milliseconds,
// packet loss as the percentage of this end's sent packets never
// acknowledged, and the trailing bandwidth sent, received and acknowledged.
// Fed by the ack blocks every session packet carries; the numbers are kept
// current incrementally as acks and receives land, so the published value is
// exact and costs nothing per idle slot.

struct rocketnet_server_transport_client_stats_t
{
    float rtt;                                      // round trip time (milliseconds)
    float jitter;                                   // deviation of rtt samples above the minimum (milliseconds)
    float packet_loss;                              // percent of sent packets never acknowledged
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
    float bandwidth_acked_kbps;
};

// a NULL server_transport or stats is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_server_transport_client_stats( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_server_transport_client_stats_t * stats );

// The delivery notification primitive: the batch of packet ids newly
// acknowledged by the last update, as parallel arrays with the client index
// of each, valid until the next update. The packet id is the one handed out
// in rocketnet_packet_context_t at start_packet -- the join key back to
// whatever the game sent. The transport tells; it never resends. Keep-alive,
// disconnect and standalone-ack sequences have no packet id and never
// appear here. When more packets are acknowledged in one update than the
// batch holds, the overflow is deferred to a later update rather than lost
// -- ROCKETNET_COUNTER_ACKED_BATCH_FULL counts each deferral.

// a NULL server_transport, client_index or packet_ids is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_server_transport_acked_packets( struct rocketnet_server_transport_t * server_transport, const int ** client_index, const uint64_t ** packet_ids );

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

// a NULL frame_sync is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_frame_sync_destroy( struct rocketnet_frame_sync_t * frame_sync );

// a NULL frame_sync is a counted no-op.
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

// a NULL frame_sync or stats is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_frame_sync_stats( struct rocketnet_frame_sync_t * frame_sync, struct rocketnet_frame_sync_stats_t * stats );

// -----------------------------------------
//            the serialize read (C++)
// -----------------------------------------

// Include serialize.h before this header and the stream forms of the
// acquire/commit surface appear: the same calls, with a WriteStream bound
// at the frame's aligned payload root on start and measured on finish, and
// a ReadStream bound over each received payload. Plain C callers see none
// of this; it compiles to nothing but the calls above.

#if defined(__cplusplus) && defined(SERIALIZE_H)

namespace rocketnet
{
    inline bool server_start_packet( struct rocketnet_server_transport_t * server, int client_index, int request_bytes, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        uint8_t * frame = rocketnet_server_transport_start_packet( server, client_index, request_bytes, &context );
        if ( !frame )
            return false;
        stream.Initialize( frame + context.payload_index, context.payload_capacity );
        return true;
    }

    // serialize's write side checks bounds with asserts only, by design: a
    // Release overflow past the payload capacity is not caught here or
    // there. Size packets to the capacity the context reports.

    inline bool server_finish_packet( struct rocketnet_server_transport_t * server, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        stream.Flush();
        return rocketnet_server_transport_finish_packet( server, &context, (int) stream.GetBytesProcessed() ) != 0;
    }

    inline void server_abort_packet( struct rocketnet_server_transport_t * server, struct rocketnet_packet_context_t & context )
    {
        rocketnet_server_transport_abort_packet( server, &context );
    }

    inline bool server_payload( struct rocketnet_server_transport_t * server, int payload_index, serialize::ReadStream & stream, int & client_index )
    {
        int payload_bytes;
        uint8_t * payload = rocketnet_server_transport_payload( server, payload_index, &client_index, &payload_bytes );
        if ( !payload )
            return false;
        stream.Initialize( payload, payload_bytes );
        return true;
    }

    inline bool client_start_packet( struct rocketnet_client_transport_t * client, int request_bytes, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        uint8_t * frame = rocketnet_client_transport_start_packet( client, request_bytes, &context );
        if ( !frame )
            return false;
        stream.Initialize( frame + context.payload_index, context.payload_capacity );
        return true;
    }

    inline bool client_finish_packet( struct rocketnet_client_transport_t * client, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        stream.Flush();
        return rocketnet_client_transport_finish_packet( client, &context, (int) stream.GetBytesProcessed() ) != 0;
    }

    inline void client_abort_packet( struct rocketnet_client_transport_t * client, struct rocketnet_packet_context_t & context )
    {
        rocketnet_client_transport_abort_packet( client, &context );
    }

    inline bool client_payload( struct rocketnet_client_transport_t * client, int payload_index, serialize::ReadStream & stream )
    {
        int payload_bytes;
        uint8_t * payload = rocketnet_client_transport_payload( client, payload_index, &payload_bytes );
        if ( !payload )
            return false;
        stream.Initialize( payload, payload_bytes );
        return true;
    }
}

#endif // #if defined(__cplusplus) && defined(SERIALIZE_H)

// -----------------------------------------
//                                messages
// -----------------------------------------

// The message layer above the transports (docs/CHANNELS.md). What is here
// today is layer 1's surface: the constants every layer shares, the message
// config, and the sizing of one connection's region, callable before any
// connect. The client and server objects follow the counters below.

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                                         32
#endif // #ifndef ROCKETNET_MAX_CHANNELS

#if ROCKETNET_MAX_CHANNELS < 1 || ROCKETNET_MAX_CHANNELS > 65536
#error ROCKETNET_MAX_CHANNELS must be between 1 and 65536
#endif

// reliability, per send -- RakNet's values and numbering (PacketPriority.h:46-85)

#define ROCKETNET_UNRELIABLE                                            0
#define ROCKETNET_UNRELIABLE_SEQUENCED                                  1
#define ROCKETNET_RELIABLE                                              2
#define ROCKETNET_RELIABLE_ORDERED                                      3
#define ROCKETNET_RELIABLE_SEQUENCED                                    4
#define ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT                           5
#define ROCKETNET_RELIABLE_WITH_ACK_RECEIPT                             6
#define ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT                     7

#define ROCKETNET_PRIORITY_IMMEDIATE                                    0
#define ROCKETNET_PRIORITY_HIGH                                         1
#define ROCKETNET_PRIORITY_MEDIUM                                       2
#define ROCKETNET_PRIORITY_LOW                                          3

#define ROCKETNET_RECEIPT_ACKED                                         1
#define ROCKETNET_RECEIPT_LOSS                                          2

// One outcome for a send's nonzero serial. The list returned after an update
// is valid until the next update; a full receipt ring drops its oldest entry.
struct rocketnet_message_receipt_t
{
    uint32_t serial;
    uint32_t status;            // ROCKETNET_RECEIPT_ACKED or ROCKETNET_RECEIPT_LOSS
};

// Why a message connection ended, the reason its DISCONNECTED event carries
// (docs/CHANNELS.md 12). Values 0 to 4 are the window layer's
// (sdk/rocketnet_window_ack.h defines the same values when this header is
// absent). MESSAGE_MEMORY_EXHAUSTED: the connection's region had no room for
// an operation it must not drop (docs/CHANNELS.md 4,
// tla/RNConnectionMemory.tla). A server disconnects that client's slot with
// this reason and resets the slot's region; a client disconnects itself.

#define ROCKETNET_MESSAGE_ERROR_NONE                0
#define ROCKETNET_MESSAGE_ERROR_CONFIG_MISMATCH     1
#define ROCKETNET_MESSAGE_ERROR_CONFIG_TIMEOUT      2
#define ROCKETNET_MESSAGE_ERROR_PROTOCOL_VIOLATION  3
#define ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT    4
#define ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED 5

// the largest message start_message writes into a real wire frame, zero copy:
// the payload capacity less the one form byte

#define ROCKETNET_MESSAGE_SINGLE_BYTES          ( ROCKETNET_MAX_PAYLOAD_BYTES - 1 )      // 1415

ROCKETNET_STATIC_ASSERT( ROCKETNET_MESSAGE_SINGLE_BYTES >= 1400, "a 1400-byte game packet is one SINGLE message, zero copy, unfragmented" );

// the block stride, a wire constant

#define ROCKETNET_MESSAGE_FRAGMENT_BYTES                             1152

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
    INT_FIELD( rto_initial_ms,           1000,     10, 60000,                  0 )                                         \
    INT_FIELD( rto_min_ms,                 50,      1, 60000,                  0 )                                         \
    INT_FIELD( rto_max_ms,               2000,     10, 60000,                  0 )                                         \
    INT_FIELD( reliable_timeout_ms,     10000,   1000, 600000,                 0 )

struct rocketnet_message_config_t
{
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

// a NULL config is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_message_default_config( struct rocketnet_message_config_t * config );

// 1 when the config is accepted, else 0 with a log line naming the rule.
// NULL means the defaults. Allocation-free; safe before init.
ROCKETNET_EXPORT_FUNC int rocketnet_message_validate_config( const struct rocketnet_message_config_t * config );

// The exact bytes of one connection's region (docs/CHANNELS.md 14): the
// formula plus the 64-byte alignment of each part. 0 for a refused config;
// NULL means the defaults. Allocation-free; callable before init or connect.
// A server holds one region per client slot, a client one.
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_connection_bytes( const struct rocketnet_message_config_t * config );

// The formula, each term at this config, the padding, the region and the
// total for num_connections regions, as text into buffer. Returns the full
// length (as snprintf: a short buffer is truncated and terminated, and NULL
// with 0 measures), or 0 for a refused config. Allocation-free.
ROCKETNET_EXPORT_FUNC int rocketnet_message_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections, char * buffer, int buffer_size );

// The same lines, logged at info level. Returns the region bytes, or 0 refused.
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_print_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections );

// -----------------------------------------

struct rocketnet_counters_t;

// Runtime introspection avoids assuming a counters-struct length across FFI.
// Read on the main thread outside update/fill, as for the counters view.
ROCKETNET_EXPORT_FUNC int rocketnet_num_counters();
// a NULL counters is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_counter_value( const struct rocketnet_counters_t * counters, int counter );

ROCKETNET_EXPORT_FUNC const char * rocketnet_counter_name( int counter );       // the full counter enum is in rocketnet_counters.h

// What a clean run shows of a counter. Every counter has exactly one class.
//   refusal    reads 0 on a clean path at any round trip (a resource
//              exhaustion is a refusal)
//   expected   moves on a clean path by design (success outcomes, lifecycle
//              and side counters)
//   path_loss  moves only when the path loses or reorders packets
//   arrival    a RECEIVED counter: a packet of the type arrived
enum
{
    ROCKETNET_COUNTER_CLASS_REFUSAL = 0,
    ROCKETNET_COUNTER_CLASS_EXPECTED,
    ROCKETNET_COUNTER_CLASS_PATH_LOSS,
    ROCKETNET_COUNTER_CLASS_ARRIVAL,
    ROCKETNET_NUM_COUNTER_CLASSES
};

// The class of a counter, one of the ROCKETNET_COUNTER_CLASS_ values, or -1 for
// an index outside the counter list. Allocation-free; callable before init.
ROCKETNET_EXPORT_FUNC int rocketnet_counter_class( int counter );

// "refusal", "expected", "path_loss" or "arrival"; NULL for a value that is not a class.
ROCKETNET_EXPORT_FUNC const char * rocketnet_counter_class_name( int counter_class );

// 1 when every counter family balances: for each RECEIVED counter, its value
// equals the sum of the outcomes that name it as their family. Otherwise 0,
// with the first unbalanced family (in counter order) written to report as
// "<RECEIVED name> received=<n> outcomes=<m>"; report may be NULL. Read the
// block at a quiescent point, on the thread that owns it: after an update
// returns, when every packet counted as arrived has taken its outcome. A NULL
// counters is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_counters_conserved( const struct rocketnet_counters_t * counters, char * report, int report_bytes );

// -----------------------------------------
//                         message objects
// -----------------------------------------

// The client and server message objects over the transports (docs/CHANNELS.md
// 10 to 12): a message is bytes, a pointer and a length (R4). Each object is
// one block made at create -- the object, its connections and one region per
// connection (the client's one, one per server slot) -- and allocates nothing
// after create (R2). memory is optional: NULL makes the one allocation, and a
// caller block of at least *_required_memory bytes, 64-aligned, makes none
// (the transport still allocates its own at its create).
//
// The update order, on the main thread: update (the transport's update, the
// transport's acked packet ids into the channels -- the only ack there is, R1
// -- then the received payloads into deliveries, then the exhaustion and
// error checks that disconnect), receive and release, start/write/finish,
// then send_packets (each connection's packets under its send budget into
// frames from the transport's start_packet, then the transport's
// send_packets).
//
// Sending mirrors the transport's start_packet and finish_packet: start
// returns a pointer into the connection's region; write the message there
// once, in place; finish(bytes_written) queues it and returns its receipt
// serial, nonzero (CHANNELS 7.4); abort gives the room back. One start is
// pending per object at a time. The enabled types are ROCKETNET_UNRELIABLE,
// ROCKETNET_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABLE, ROCKETNET_RELIABLE_ORDERED, and
// ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT; the others are refused (counted
// MESSAGE_SEND_TYPE_NOT_ENABLED) until their cards enable them. Plain
// UNRELIABLE is one record in one packet, up to 1411 bytes. UNRELIABLE_SEQUENCED
// can use the transport's fragments for one larger unreliable RECORDS payload.
// Receipt-bearing UNRELIABLE uses the code-0 wire form at single-packet size and promotes a larger
// message to a reliable code-2 block with an ACKED-only receipt. Both are
// received on channel 0. The reliable types and promoted blocks are bounded
// by the config's max_message_bytes.
//
// A start the connection's region has no room for ends the connection (R5):
// the server disconnects that client, the client disconnects itself, with the
// reason ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED, and start returns
// NULL. The send budget governs the wire, never the queue: a reliable message
// waits for budget and is never dropped; an unreliable one over budget is
// dropped and counted (MESSAGE_UNRELIABLE_OVER_BUDGET).
//
// Receiving: receive hands out the next message of the update on a channel as
// a const view and its length, valid until the next update. Every received
// message is released before the next update; update with one still held
// asserts in a debug build and returns ROCKETNET_ERROR_MESSAGE_NOT_RELEASED in
// a release build (tla/RNSlots.tla, RejectUnreleasedUpdate). Copy what is kept
// past the update.
//
// Budgets are kilobits per second of the transport's wire bytes (1 kbps =
// 1000 bit/s), per connection: 0 at create means the default.

#ifndef ROCKETNET_DEFAULT_CLIENT_SEND_KBPS
#define ROCKETNET_DEFAULT_CLIENT_SEND_KBPS                  1000
#endif
#ifndef ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS
#define ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS       10000
#endif

#define ROCKETNET_MESSAGE_OK                                0
#define ROCKETNET_ERROR_MESSAGE_NOT_RELEASED                -1

struct rocketnet_message_client_t;
struct rocketnet_message_server_t;

// ---- client ----

// the bytes a caller block for a client object needs; 0 for a refused config
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_client_required_memory( const struct rocketnet_message_config_t * message_config );

// a NULL connect_token is a counted no-op that returns NULL. NULL configs are the defaults.
// create reads no clock: the connect attempt starts at the first update, at whatever time the game passes it, and every time comes in through update.
ROCKETNET_EXPORT_FUNC struct rocketnet_message_client_t * rocketnet_message_client_create( void * context, const uint8_t * connect_token,
                                                                                         const struct rocketnet_client_transport_config_t * transport_config,
                                                                                         const struct rocketnet_message_config_t * message_config,
                                                                                         void * memory, uint64_t memory_bytes, uint32_t send_kbps );

// a NULL client is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_message_client_destroy( struct rocketnet_message_client_t * client );

// the client's transport, for its stats and state; never its batches. a NULL client is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC struct rocketnet_client_transport_t * rocketnet_message_client_transport( struct rocketnet_message_client_t * client );

// ROCKETNET_CLIENT_TRANSPORT_STATE_*. a NULL client is a counted no-op that returns DISCONNECTED.
ROCKETNET_EXPORT_FUNC int rocketnet_message_client_state( struct rocketnet_message_client_t * client );

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL client is a counted no-op that returns ROCKETNET_MESSAGE_OK.
ROCKETNET_EXPORT_FUNC int rocketnet_message_client_update( struct rocketnet_message_client_t * client, double time );

// a NULL client is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_message_client_send_packets( struct rocketnet_message_client_t * client );

// a NULL client is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_message_client_start( struct rocketnet_message_client_t * client, int channel, int reliability, int bytes );

// the receipt serial, nonzero; 0 when no start is pending or bytes_written is outside 1 to the bytes started. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_message_client_finish( struct rocketnet_message_client_t * client, int bytes_written );

// a NULL client is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_message_client_abort( struct rocketnet_message_client_t * client );

// The convenience over start and finish. It copies data into the region
// record: start, write in place and finish do not. a NULL client or data is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_message_client_send( struct rocketnet_message_client_t * client, int channel, int reliability, const uint8_t * data, int bytes );

// 1 with the next message on channel, 0 when there is none. a NULL client, data or bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_message_client_receive( struct rocketnet_message_client_t * client, int channel, const uint8_t ** data, int * bytes );

// Copies this update's receipt list into out, up to capacity entries; returns
// the copied count. Repeated calls see the same list until the next update.
ROCKETNET_EXPORT_FUNC int rocketnet_message_client_receipts( struct rocketnet_message_client_t * client, struct rocketnet_message_receipt_t * out, int capacity );

// a NULL client or data is a counted no-op; a pointer that is not a held view is a counted misuse.
ROCKETNET_EXPORT_FUNC void rocketnet_message_client_release( struct rocketnet_message_client_t * client, const uint8_t * data );

// 1 when the budget took the new rate, from now on. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_message_client_set_send_kbps( struct rocketnet_message_client_t * client, uint32_t kbps );

// ROCKETNET_MESSAGE_ERROR_* the client ended its connection with, NONE while it has not. a NULL client is a counted no-op that returns NONE.
ROCKETNET_EXPORT_FUNC int rocketnet_message_client_disconnect_reason( struct rocketnet_message_client_t * client );

// the message counters (rocketnet_counters.h). a NULL client is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_message_client_counters( struct rocketnet_message_client_t * client );

// ---- server ----

// the bytes a caller block for a server object needs, at the transport config's max_clients; 0 for a refused config
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_message_server_required_memory( const struct rocketnet_server_transport_config_t * transport_config, const struct rocketnet_message_config_t * message_config );

// a NULL address is a counted no-op that returns NULL. NULL configs are the defaults.
ROCKETNET_EXPORT_FUNC struct rocketnet_message_server_t * rocketnet_message_server_create( void * context, const char * address,
                                                                                         const struct rocketnet_server_transport_config_t * transport_config,
                                                                                         const struct rocketnet_message_config_t * message_config,
                                                                                         void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps );

// a NULL server is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_message_server_destroy( struct rocketnet_message_server_t * server );

// the server's transport, for its port, stats and stop; never its batches. a NULL server is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC struct rocketnet_server_transport_t * rocketnet_message_server_transport( struct rocketnet_message_server_t * server );

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL server is a counted no-op that returns ROCKETNET_MESSAGE_OK.
ROCKETNET_EXPORT_FUNC int rocketnet_message_server_update( struct rocketnet_message_server_t * server, double time );

// a NULL server is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_message_server_send_packets( struct rocketnet_message_server_t * server );

// a NULL server is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC uint8_t * rocketnet_message_server_start( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, int bytes );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_message_server_finish( struct rocketnet_message_server_t * server, int client_index, int bytes_written );

// a NULL server is a counted no-op.
ROCKETNET_EXPORT_FUNC void rocketnet_message_server_abort( struct rocketnet_message_server_t * server, int client_index );

// The convenience over start and finish; it copies data into the region record. a NULL server or data is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_message_server_send( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, const uint8_t * data, int bytes );

// a NULL server, data or bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_message_server_receive( struct rocketnet_message_server_t * server, int client_index, int channel, const uint8_t ** data, int * bytes );

// As above for one connected client slot. Invalid inputs return zero.
ROCKETNET_EXPORT_FUNC int rocketnet_message_server_receipts( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_message_receipt_t * out, int capacity );

// a NULL server or data is a counted no-op; a pointer that is not a held view is a counted misuse.
ROCKETNET_EXPORT_FUNC void rocketnet_message_server_release( struct rocketnet_message_server_t * server, int client_index, const uint8_t * data );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_message_server_set_send_kbps( struct rocketnet_message_server_t * server, int client_index, uint32_t kbps );

// ROCKETNET_MESSAGE_ERROR_* the slot's last connection ended with, until the slot's next connection. a NULL server is a counted no-op that returns NONE.
ROCKETNET_EXPORT_FUNC int rocketnet_message_server_disconnect_reason( struct rocketnet_message_server_t * server, int client_index );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT_FUNC int rocketnet_message_server_client_connected( struct rocketnet_message_server_t * server, int client_index );

// a NULL server is a counted no-op that returns NULL.
ROCKETNET_EXPORT_FUNC const struct rocketnet_counters_t * rocketnet_message_server_counters( struct rocketnet_message_server_t * server );

// -----------------------------------------

#endif // #ifndef ROCKETNET_H

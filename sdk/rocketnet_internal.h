/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// the SDK's internal header: everything the library's sources, tests and tools
// share that is not for callers. It includes the public rocketnet.h and adds
// the platform detection, the asserts, the config tables and the internal
// helpers. Callers include rocketnet.h alone

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_INTERNAL_H
#define ROCKETNET_SDK_ROCKETNET_INTERNAL_H

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif // #ifndef __STDC_FORMAT_MACROS

#include "rocketnet.h"
#include "rocketnet_layers.h"

#ifndef ROCKETNET_XDP
#ifdef __linux__
#define ROCKETNET_XDP 1
#else
#define ROCKETNET_XDP 0
#endif // #ifdef __linux__
#endif // #ifndef ROCKETNET_XDP

// a compile-time assert that also builds in C compilers without C11 mode

#ifdef __cplusplus
#define ROCKETNET_STATIC_ASSERT( condition, message )  static_assert( condition, message )
#elif defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 201112L
#define ROCKETNET_STATIC_ASSERT( condition, message )  _Static_assert( condition, message )
#else
#define ROCKETNET_STATIC_ASSERT_JOIN_( a, b ) a##b
#define ROCKETNET_STATIC_ASSERT_JOIN( a, b ) ROCKETNET_STATIC_ASSERT_JOIN_( a, b )
#define ROCKETNET_STATIC_ASSERT( condition, message ) \
    typedef char ROCKETNET_STATIC_ASSERT_JOIN( rocketnet_static_assert_, __LINE__ )[(condition) ? 1 : -1]
#endif // #ifdef __cplusplus

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_PACKET_BYTES == ROCKETNET_RECEIVE_PACKET_BYTES, "send and receive packet bounds match" );

// the smallest power of two at least n, at compile time, for 1 <= n <= 1 << 30:
// n - 1 with every bit below its highest set bit also set, plus one. The
// limits that derive from ROCKETNET_MAX_CLIENTS and must be powers of two
// round up with it

#define ROCKETNET_POWER_OF_TWO_SMEAR_( x, shift )  ( (x) | ( (x) >> (shift) ) )
#define ROCKETNET_POWER_OF_TWO_AT_LEAST( n )                                                             \
    ( ROCKETNET_POWER_OF_TWO_SMEAR_( ROCKETNET_POWER_OF_TWO_SMEAR_( ROCKETNET_POWER_OF_TWO_SMEAR_(       \
      ROCKETNET_POWER_OF_TWO_SMEAR_( ROCKETNET_POWER_OF_TWO_SMEAR_( (n) - 1, 1 ), 2 ), 4 ), 8 ), 16 ) + 1 )

ROCKETNET_STATIC_ASSERT( ROCKETNET_POWER_OF_TWO_AT_LEAST( 1 ) == 1 && ROCKETNET_POWER_OF_TWO_AT_LEAST( 2 ) == 2 &&
                             ROCKETNET_POWER_OF_TWO_AT_LEAST( 3 ) == 4 && ROCKETNET_POWER_OF_TWO_AT_LEAST( 2000 ) == 2048 &&
                             ROCKETNET_POWER_OF_TWO_AT_LEAST( 2048 ) == 2048 && ROCKETNET_POWER_OF_TWO_AT_LEAST( 4000 ) == 4096 &&
                             ROCKETNET_POWER_OF_TWO_AT_LEAST( 20000 ) == 32768 && ROCKETNET_POWER_OF_TWO_AT_LEAST( 262144 ) == 262144,
                         "ROCKETNET_POWER_OF_TWO_AT_LEAST rounds up to a power of two" );

// -----------------------------------------
//          status-returning lifecycle forms
// -----------------------------------------

// the int-status forms the public rocketnet_init and rocketnet_term wrap

#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus

// returns true on success; writes the status when status is not NULL
bool rocketnet_init_ex( void * context, void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ), int * status );

int rocketnet_term_ex( void );

// replaces the allocator without init, for tests. both functions are required
int rocketnet_allocator_ex( void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) );

// the connection a public client holds (NULL while DISCONNECTED) and the
// running message server a public server holds (NULL while stopped), for
// tests and tools that read the layers underneath
struct rocketnet_message_client_t * rocketnet_client_message_client( struct rocketnet_client_t * client );
struct rocketnet_message_server_t * rocketnet_server_message_server( struct rocketnet_server_t * server );

#if ROCKETNET_DEV
// the label the default log sink prints for a level: "???" for a level
// outside ERROR .. SPAM
const char * rocketnet_log_level_word_for_test( int level );

// calls sodium's misuse path, as an unavailable OS random source does: the
// handler rocketnet_init installed runs, then sodium aborts the process. For
// the functional case that runs it in a child process
void rocketnet_sodium_misuse_for_test( void );

// the address a created server serves for one family, after the
// environment overrides. Writes it and returns true, or leaves address empty
// and returns false when the server does not serve that family or the address
// does not fit in address_bytes (logged). Reads no socket
bool rocketnet_server_served_address( const struct rocketnet_server_t * server, int family, char * address, size_t address_bytes );
#endif // #if ROCKETNET_DEV

// for tests that build the layers by hand: a public client holding an
// existing connection, and a public server holding an existing running
// message server; destroy destroys what was adopted. The adopted object has
// no block of its own, so a connect or start after the adopted one ends
// allocates its own, with default configs
#if ROCKETNET_DEV
struct rocketnet_client_t * rocketnet_client_adopt( void * context, struct rocketnet_message_client_t * connection );
struct rocketnet_server_t * rocketnet_server_adopt( void * context, struct rocketnet_message_server_t * running );

// the secret bytes of an object, as spans of its allocation (offset, bytes):
// what destroy must leave zero when it frees the allocation, and the only
// bytes it may write. Each returns the span count, at most max. The
// destroy tests inspect these spans in the allocator's free callback. The
// object is the live object, whose own config sizes any span that scales with
// it (the server transport's disconnect keys).
int rocketnet_server_secret_spans_for_test( const void * object, size_t * offsets, size_t * bytes, int max );
int rocketnet_server_transport_secret_spans_for_test( const void * object, size_t * offsets, size_t * bytes, int max );
int rocketnet_client_transport_secret_spans_for_test( const void * object, size_t * offsets, size_t * bytes, int max );

// the wall-clock budget an early destroy polls a worker's exit for: 0 makes
// one pass with no sleep (the keep path's tests), a negative value restores
// the default
void rocketnet_destroy_stop_seconds_for_test( double seconds );
#endif // #if ROCKETNET_DEV

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#define ROCKETNET_PLATFORM_WINDOWS                                     1
#define ROCKETNET_PLATFORM_MAC                                         2
#define ROCKETNET_PLATFORM_LINUX                                       3
#define ROCKETNET_PLATFORM_IOS                                         4
#define ROCKETNET_PLATFORM_BSD                                         5
#define ROCKETNET_PLATFORM_MAX                                         6

#if defined( _WIN32 )
#define NOMINMAX
#endif // #if defined( _WIN32 )

#if defined( _WIN32 )
#define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_WINDOWS
#elif defined( __APPLE__ )
#include "TargetConditionals.h"
#if TARGET_OS_IPHONE
#define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_IOS
#else
#define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_MAC
#endif // #if TARGET_OS_IPHONE
#elif defined( __FreeBSD__ ) || defined( __OpenBSD__ ) || defined( __NetBSD__ )
    // the BSDs verified ("Free, open. Net."): one platform bucket,
    // pure POSIX, portable transport only -- the linux bucket keeps netlink,
    // GSO and XDP, none of which exist here
#define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_BSD
#else
#define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_LINUX
#endif // #if defined( _WIN32 )

#if !defined( ROCKETNET_UNREAL_ENGINE )
#define ROCKETNET_UNREAL_ENGINE 0
#endif // #if !defined(ROCKETNET_UNREAL_ENGINE)

#if !defined( ROCKETNET_LITTLE_ENDIAN ) && !defined( ROCKETNET_BIG_ENDIAN )

#ifdef __BYTE_ORDER__
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ROCKETNET_LITTLE_ENDIAN 1
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define ROCKETNET_BIG_ENDIAN 1
#else
#error Unknown machine endianess detected. Please define ROCKETNET_LITTLE_ENDIAN or ROCKETNET_BIG_ENDIAN.
#endif // #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

  // detect with GLIBC's endian.h
#elif defined( __GLIBC__ )
#include <endian.h>
#if ( __BYTE_ORDER == __LITTLE_ENDIAN )
#define ROCKETNET_LITTLE_ENDIAN 1
#elif ( __BYTE_ORDER == __BIG_ENDIAN )
#define ROCKETNET_BIG_ENDIAN 1
#else
#error Unknown machine endianess detected. Please define ROCKETNET_LITTLE_ENDIAN or ROCKETNET_BIG_ENDIAN.
#endif // #if ( __BYTE_ORDER == __LITTLE_ENDIAN )

  // detect with _LITTLE_ENDIAN and _BIG_ENDIAN macro
#elif defined( _LITTLE_ENDIAN ) && !defined( _BIG_ENDIAN )
#define ROCKETNET_LITTLE_ENDIAN 1
#elif defined( _BIG_ENDIAN ) && !defined( _LITTLE_ENDIAN )
#define ROCKETNET_BIG_ENDIAN 1

  // detect with architecture macros
#elif defined( __sparc ) || defined( __sparc__ ) || defined( _POWER ) || defined( __powerpc__ ) || defined( __ppc__ ) || defined( __hpux ) || defined( __hppa ) || defined( _MIPSEB ) || defined( _POWER ) || defined( __s390__ )
#define ROCKETNET_BIG_ENDIAN 1
#elif defined( __i386__ ) || defined( __alpha__ ) || defined( __ia64 ) || defined( __ia64__ ) || defined( _M_IX86 ) || defined( _M_IA64 ) || defined( _M_ALPHA ) || defined( __amd64 ) || defined( __amd64__ ) || defined( _M_AMD64 ) || defined( __x86_64 ) || defined( __x86_64__ ) || defined( _M_X64 ) || defined( __bfin__ )
#define ROCKETNET_LITTLE_ENDIAN 1
#elif defined( _MSC_VER ) && ( defined( _M_ARM ) || defined( _M_ARM64 ) )
#define ROCKETNET_LITTLE_ENDIAN 1
#else
#error Unknown machine endianess detected. Please define ROCKETNET_LITTLE_ENDIAN or ROCKETNET_BIG_ENDIAN.
#endif // #ifdef __BYTE_ORDER__

#endif // #if !defined( ROCKETNET_LITTLE_ENDIAN ) && !defined( ROCKETNET_BIG_ENDIAN )

#if !defined( ROCKETNET_BIG_ENDIAN )
#define ROCKETNET_BIG_ENDIAN 0
#endif // #if !defined(ROCKETNET_BIG_ENDIAN)

#if !defined( ROCKETNET_LITTLE_ENDIAN )
#define ROCKETNET_LITTLE_ENDIAN 0
#endif // #if !defined(ROCKETNET_LITTLE_ENDIAN)

#include <inttypes.h>

#ifndef ROCKETNET_ASSERTS
#ifdef NDEBUG
#define ROCKETNET_ASSERTS 0
#else
#define ROCKETNET_ASSERTS 1
#endif // #ifdef NDEBUG
#endif // #ifndef ROCKETNET_ASSERTS

// the assert callback, set through rocketnet_assert_function; never exported
#if defined( __GNUC__ ) || defined( __clang__ )
#define ROCKETNET_INTERNAL __attribute__((visibility("hidden")))
#else
#define ROCKETNET_INTERNAL
#endif // #if defined( __GNUC__ ) || defined( __clang__ )

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
#endif // #if ROCKETNET_ASSERTS

// an invariant the code relies on: asserted where asserts are on, and stated
// to clang in every build, so the static analyzer of a release build sees it.
// The condition has no side effects.
#if defined( __clang__ )
#define rocketnet_assume( condition )                                                            \
do                                                                                          \
{                                                                                           \
    rocketnet_assert( condition );                                                          \
    __builtin_assume( condition );                                                          \
} while(0)
#else
#define rocketnet_assume( condition ) rocketnet_assert( condition )
#endif // #if defined( __clang__ )

bool rocketnet_library_initialized( void );

// whether malloc_function and free_function are the installed allocator;
// allocates nothing and takes no lock (sdk/rocketnet.cpp)
bool rocketnet_allocator_installed( void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) );

#if ROCKETNET_DEV
// the lifecycle lock takes on the calling thread, ever (sdk/rocketnet.cpp):
// init, term and allocator installation take it; no allocation does
uint64_t rocketnet_lifecycle_locks_for_test( void );

// a function install calls between reading a zero live count and publishing
// the new pair (sdk/rocketnet.cpp); NULL for none
void rocketnet_allocator_set_install_hook_for_test( void ( *hook )( void ) );

// a function rocketnet_malloc calls at each step of its wait for a pair
// (sdk/rocketnet.cpp): counted, before the pair read, and after meeting the
// installing sentinel and uncounting; NULL for none
#define ROCKETNET_ALLOCATOR_STEP_COUNTED 1
#define ROCKETNET_ALLOCATOR_STEP_WAITED 2
void rocketnet_allocator_set_call_hook_for_test( void ( *hook )( int step ) );
#endif // #if ROCKETNET_DEV

void rocketnet_api_misuse_record( void );

// an exported call's NULL check: a NULL counts one misuse and asserts
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

#if ROCKETNET_DEV
// internal to the SDK. The transport updates and the message object's update
// clamp the caller's clock once per update, before any read of it, through this
// helper: a non-finite time, or a time below the last good one, becomes the
// last good time. A clamp sets *clamped, counts one misuse and fires
// rocketnet_assert; a release build clamps and goes on, a debug build is loud.
// It is also the client create's check, seeded with a zero last good time. The
// message object clamps first and passes the clamped value on to its transport,
// so a bad time is counted once, never stored, and never becomes the clock
// origin. One compare per update, none per packet
/** @brief Internal monotonic-time guard used by transport and message-object updates. @param last_good Previous accepted time in seconds. @param time Candidate time in seconds. @param clamped Optional output set true when candidate is rejected; NULL is allowed. @return Candidate time if finite and nondecreasing, otherwise last_good. Invalid time records misuse and fires the configured assertion. */
double rocketnet_clamp_update_time( double last_good, double time, bool * clamped );
#endif // #if ROCKETNET_DEV

/**
 * @brief Advances one owner's update clock by the caller's raw step, beside
 * rocketnet_clamp_update_time. The owner keeps the caller's raw time and the
 * clamped time it hands out; this helper keeps them in step: a forward raw
 * step of d advances the clamped time by exactly d, a backwards raw step
 * returns the last good time and records the new raw time, and a non-finite
 * sample changes neither value. While no backwards step has happened the
 * returned time is the caller's time, bit for bit, so every caller that reads
 * an exact time still reads it.
 * @param last_good The clamped time the owner keeps.
 * @param raw_time The owner's last raw caller time, read and written.
 * @param time_offset The owner's backwards offset, read and written: exactly
 * 0.0 until the first backwards step, and the distance the caller's clock
 * fell behind after one.
 * @param time The caller's new raw time.
 * @param clamped Optional output set true when the sample was rejected; NULL
 * is allowed.
 * @return The owner's new clamped time. A non-finite or backwards sample
 * records one misuse and fires the configured assertion.
 */
double rocketnet_update_clock_advance( double last_good, double * raw_time, double * time_offset, double time, bool * clamped );

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
    *value = ( *value & 0x00FF00FF00FF00FF ) << 8 | ( *value & 0xFF00FF00FF00FF00 ) >> 8;
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

// the per-transport config system. Each transport's config is
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
// out-of-range value -- never clamps, never silently corrects

// field flags: ZERO_MEANS_DEFAULT admits zero as "use the built-in
// default" beside the stated range; SECRET redacts the value in override
// logging

/** @brief Config-field flag that gives zero the field default meaning. */
#define ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT     (1<<0)
/** @brief Config-field flag that redacts its value in override logs. */
#define ROCKETNET_CONFIG_SECRET                 (1<<1)
/** @brief Config-field flag for a field no environment variable sets: the server's keypair, which create generates; the public addresses, which create takes as arguments; and the simulated_* link fields, which the development network simulator sets. */
#define ROCKETNET_CONFIG_NO_ENV                 (1<<2)

/** @brief Zero initializer for fixed-size byte-array configuration fields. */
#define ROCKETNET_CONFIG_ZERO_BYTES             {0}

// the client transport config table: filter value, the transport candidate
// list and the token epoch
//
//   INT_FIELD( name, default, min, max, flags )
//   UINT64_FIELD( name, default, flags )
//   BYTES_FIELD( name, size, default, flags )
//   STRING_FIELD( name, capacity, default, flags )

#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V1( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                    \
    BYTES_FIELD(  packet_filter,          4,                              ROCKETNET_TEST_FILTER_VALUE,                          0 )                          \
    STRING_FIELD( transport,                ROCKETNET_TRANSPORT_STRING_LENGTH, "auto",                                               0 )                          \
    UINT64_FIELD( token_epoch_unix_seconds,      0,                                                                                    0 )

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

// sized configs (docs/ABI-EVOLUTION.md). Each config struct has the sized
// entry points beside its unsized ones: the caller passes sizeof the struct it
// was compiled against. A struct only grows at its end, so every size it has
// had is a prefix of today's; ROCKETNET_<STRUCT>_KNOWN_BYTES lists those sizes,
// oldest first, each one the end of that revision's last field rounded up to
// that revision's alignment (ROCKETNET_CONFIG_REVISION_BYTES). A known size
// reads the caller's prefix and defaults the rest; an unknown size, larger or
// smaller, is refused with a log line and nothing is read or written. The
// unsized entry points read and write the whole struct of this header

// internal to the SDK: the one read and the one write every sized entry point
// makes (docs/ABI-EVOLUTION.md 3). write copies the first config_bytes of
// defaults into config; read sets config to defaults, then copies the first
// source_bytes of source over it. Each returns 1, or 0 with a log line naming
// type when the size is not in known (and then write leaves config untouched,
// read leaves it at the defaults)
int rocketnet_config_write_sized( const char * type, void * config, size_t config_bytes, const void * defaults, size_t defaults_bytes, const size_t * known, int num_known );
int rocketnet_config_read_sized( const char * type, void * config, const void * defaults, size_t defaults_bytes, const void * source, size_t source_bytes, const size_t * known, int num_known );

// docs/ABI-EVOLUTION.md 4: the sizes this struct has had, oldest first
/** @brief Size in bytes of the client transport config before token_epoch_unix_seconds. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_ORIGINAL ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, time_callback ) + sizeof( void * ), sizeof( void * ) )
/** @brief Size in bytes of client transport config revision 1: with token_epoch_unix_seconds and step_driven, before the public link simulator fields. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, step_driven ) + sizeof( uint64_t ), sizeof( uint64_t ) )
/** @brief Size in bytes of the client transport config with the public link simulator fields, before the deterministic drop and reverse patterns. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_PATTERNS ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( void * ) )
/** @brief Size in bytes of the client transport config with the deterministic drop and reverse patterns, before the loopback hub. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, simulated_reverse_window ) + sizeof( int ), sizeof( void * ) )
/** @brief Current known client transport config size, in bytes: the loopback hub, appended. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_transport_config_t, loopback_hub ) + sizeof( void * ), sizeof( void * ) )
/** @brief Known client transport config sizes, in bytes. */
#define ROCKETNET_CLIENT_TRANSPORT_CONFIG_KNOWN_BYTES ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_ORIGINAL, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_PATTERNS, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2

// the server transport config table -- the single declaration everything
// derives from, per the config system comment above. num_xdp_queues' range
// here is structural; the tighter bounds are per transport, enforced at the
// ladder rung (ROCKETNET_GSO_MAX_QUEUES)

// the default max_clients: 1000, the supported server size, or the build's
// ROCKETNET_MAX_CLIENTS when that is smaller (512 .. 999). A build for more clients
// (-DROCKETNET_MAX_CLIENTS=10000) still defaults to 1000; the config field
// asks for more

#if ROCKETNET_MAX_CLIENTS < 1000
#define ROCKETNET_DEFAULT_MAX_CLIENTS          ROCKETNET_MAX_CLIENTS
#else // #if ROCKETNET_MAX_CLIENTS < 1000
#define ROCKETNET_DEFAULT_MAX_CLIENTS          1000
#endif // #if ROCKETNET_MAX_CLIENTS < 1000

// the handshake work budget (handshakes_per_update): the most connection
// requests one server update takes to the signature verify, each one Ed25519 verify
// and, when it verifies, the sealed-block open, a keypair and the key
// exchange. A request past it is dropped before any asymmetric work and
// counted; the client's next request, at its send rate, is processed in a
// later update (proofs/tla/RNSecurityTables.tla HonestRequest). One request costs
// about 130 to 190 us with a valid token and 42 to 67 us with a forged
// signature on a Ryzen 7 255 (benchmarks/handshake_cost.cpp), so the old fixed
// budget of 1024, now the ceiling, let one update run about 190 ms of
// handshakes. The default of 16 bounds it to about 3 ms, and still seats a
// 1000-client mass join in about 6.3 s when every client's request lands in
// the same update each 10 Hz send interval, inside a 10-second token timeout
// (tests/test_unit_server_mass_disconnect.cpp).

#define ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE   16
#define ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE       1024

#define ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                                       \
    UINT64_FIELD( protocol_id,           0,                                                                                    0 )                          \
    INT_FIELD(    max_clients,        ROCKETNET_DEFAULT_MAX_CLIENTS,     1, ROCKETNET_MAX_CLIENTS,                             0 )                          \
    BYTES_FIELD(  packet_filter,          4,                              ROCKETNET_TEST_FILTER_VALUE,                          0 )                          \
    BYTES_FIELD(  backend_public_key,   32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          0 )                          \
    BYTES_FIELD(  server_public_key,    32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          ROCKETNET_CONFIG_NO_ENV )               \
    BYTES_FIELD(  server_secret_key,    32,                              ROCKETNET_CONFIG_ZERO_BYTES,                          ROCKETNET_CONFIG_SECRET | ROCKETNET_CONFIG_NO_ENV )    \
    INT_FIELD(    num_xdp_queues,        0,                              1, 1024,                                              ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    num_os_cpus,           0,                              1, 256,                                               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( transport,                ROCKETNET_TRANSPORT_STRING_LENGTH, "auto",                                               0 )                          \
    STRING_FIELD( public_address,        ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "",                                              ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    max_timeout_seconds,   60,                             1, 3600,                                              0 )                          \
    INT_FIELD(    handshake_deadline_seconds, 5,                         1, 60,                                                0 )                          \
    UINT64_FIELD( token_epoch_unix_seconds,           0,                                                                              0 )                          \
    INT_FIELD(    simulated_loss_percent,      0,                         0, 100,                                              ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_latency_ms,        0,                         0, 60000,                                            ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_jitter_ms,         0,                         0, 60000,                                            ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_duplicate_percent, 0,                         0, 100,                                              ROCKETNET_CONFIG_NO_ENV )                          \
    INT_FIELD(    simulated_drop_every_nth,    0,                         2, 1000000,                                          ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    INT_FIELD(    simulated_reverse_window,    0,                         2, 64,                                               ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) \
    STRING_FIELD( second_public_address, ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "",                                              ROCKETNET_CONFIG_NO_ENV )             \
    INT_FIELD(    disconnect_packets_per_update, ROCKETNET_DEFAULT_SERVER_DISCONNECT_PACKETS_PER_UPDATE, 1, 1024,              0 ) \
    INT_FIELD(    handshakes_per_update, ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE, 1, ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE, 0 )

/** @brief Size in bytes of the server transport config before token_epoch_unix_seconds. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1_ORIGINAL ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, handshake_deadline_seconds ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Size in bytes of server transport config revision 1: before the public link simulator fields. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, token_epoch_unix_seconds ) + sizeof( uint64_t ), sizeof( uint64_t ) )
/** @brief Size in bytes of the server transport config with the public link simulator fields, before the deterministic drop and reverse patterns. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2_PRE_PATTERNS ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Server transport config revision 2, in bytes: the deterministic drop and reverse patterns. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, simulated_reverse_window ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Server transport config revision 3, in bytes: the other family's public address, appended. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_3 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, second_public_address ) + ROCKETNET_MAX_ADDRESS_STRING_LENGTH, sizeof( uint64_t ) )
/** @brief Server transport config revision 4, in bytes: the disconnect notification packet allowance and the handshake work budget, appended. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_4 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, handshakes_per_update ) + sizeof( int ), sizeof( uint64_t ) )
/** @brief Server transport config revision 5, in bytes: the loopback hub, appended. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_5 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_transport_config_t, loopback_hub ) + sizeof( void * ), sizeof( uint64_t ) )
/** @brief Known server transport config sizes, in bytes. */
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_KNOWN_BYTES ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1_ORIGINAL, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2_PRE_PATTERNS, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_3, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_4, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_5

// the largest build-specific channel count: a WIDE record carries a u16 channel

#define ROCKETNET_MAX_CHANNELS_LIMIT 65536

ROCKETNET_STATIC_ASSERT( ROCKETNET_MESSAGE_SINGLE_BYTES >= 1400, "a 1400-byte game packet is one SINGLE message, zero copy, unfragmented" );

// the message config table, one declaration deriving struct, defaults and
// validation, as the transports' tables. Fields marked
// WIRE must be identical on both ends; CONFIG verifies them (layer 7).
// Validation applies the ranges here and the cross rules of docs/LOAD.md Table 3, and
// refuses, never clamps. The public env pass reads every row but the two
// budget rows under the endpoint's prefix (rn_apply_message_and_budget_env);
// the budget's one variable is the endpoint's budget field,
// ROCKETNET_CLIENT_SEND_KBPS or ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS

// the default num_channels: 32, or the build's ROCKETNET_MAX_CHANNELS when that
// is smaller, so every build's default config is one its create accepts

#if ROCKETNET_MAX_CHANNELS < 32
#define ROCKETNET_DEFAULT_NUM_CHANNELS         ROCKETNET_MAX_CHANNELS
#else
#define ROCKETNET_DEFAULT_NUM_CHANNELS         32
#endif // #if ROCKETNET_MAX_CHANNELS < 32

// the largest reliable window W, the maximum of reliable_messages_in_flight

#define ROCKETNET_MESSAGE_WINDOW_MAX           ( 1 << 20 )         // 1048576

#define ROCKETNET_MESSAGE_CONFIG_TABLE( INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD )                                       \
    INT_FIELD( num_channels, ROCKETNET_DEFAULT_NUM_CHANNELS, 1, ROCKETNET_MAX_CHANNELS, 0 ) /* WIRE */                            \
    INT_FIELD( reliable_messages_in_flight,          1024,     64, ROCKETNET_MESSAGE_WINDOW_MAX, 0 ) /* WIRE, power of two */         \
    INT_FIELD( receive_message_buffer_bytes,    1048576,   4096, 1 << 20,                0 ) /* WIRE, multiple of 16 */            \
    INT_FIELD( max_message_bytes,     1048576,   ROCKETNET_MESSAGE_SINGLE_BYTES, ROCKETNET_MAX_MESSAGE_BYTES, 0 ) /* WIRE, <= receive_message_buffer_bytes */     \
    INT_FIELD( send_message_buffer_bytes,     4194304,  12288, 1 << 28,                0 )                                         \
    INT_FIELD( send_queue_messages,      1024,     16, 1 << 20,                0 )                                         \
    INT_FIELD( packets_in_flight,        1024,     64, 1 << 20,                0 ) /* >= reliable_messages_in_flight */  \
    INT_FIELD( packets_per_update,    256,      1, ROCKETNET_CONNECTION_MAX_PACKETS_PER_UPDATE, 0 )                   \
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

// the platform's network lookups, for unit tests only.
// rocketnet_platform_client_ipv6_available opens and closes an AF_INET6 socket.
// With a table set, it reads its entry instead and opens nothing; NULL, the
// default, runs the real lookup. The unit suite sets a fake before
// rocketnet_init (tests/test.cpp) so the DEV unit suite opens no socket. Not exported

#if ROCKETNET_DEV
struct rocketnet_platform_network_lookups_t
{
    bool ( *ipv6_available )( void );
};

void rocketnet_platform_set_network_lookups_for_testing( const struct rocketnet_platform_network_lookups_t * lookups );

// the wall clock the connect token checks read when token_epoch_unix_seconds
// is zero (rocketnet_platform_token_epoch). With a function set, they read it
// instead of the real clock; NULL, the default, reads the real clock. The unit
// suite sets a fixed clock before rocketnet_init (tests/test.cpp) so no unit
// case reads the real time. Not exported
void rocketnet_platform_set_wall_clock_for_testing( uint64_t ( *wall_clock )( void ) );

#endif // #if ROCKETNET_DEV

// the public config structs in rocketnet.h are written out field by field;
// the tables above are the declarations the parse, the validation, the env
// surface and the ABI descriptors derive from. sdk/rocketnet_config.cpp
// asserts, field by field, that each public struct and its table agree in
// name, order, offset and size, so the two cannot drift

// the longest single rocketnet_sleep: its microsecond count fits a 32-bit int.
// rocketnet_sleep_clamp makes a negative, zero or NaN request 0 and caps the rest
#define ROCKETNET_SLEEP_MAX_SECONDS 2000.0

double rocketnet_sleep_clamp( double seconds );
#if ROCKETNET_DEV
uint64_t rocketnet_sleep_platform_dispatch_count_for_test( void );
void rocketnet_sleep_set_platform_dispatch_for_test( void ( *dispatch )( double seconds ) );
#endif // #if ROCKETNET_DEV

// server receive buffer sizing (sdk/rocketnet_constants.h, USAGE.md "Receive
// buffer sizing"). rocketnet_server_receive_buffer_bytes is pure arithmetic:
// ceil( max_clients x receive_kbps_per_client x 125 x
// ROCKETNET_SERVER_RECEIVE_BUFFER_MARGIN / update_hz ) bytes, at least
// ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE and at most INT_MAX; arguments below 1
// count as 1. rocketnet_server_receive_buffer_request is that size at
// max_clients, the default client send budget
// (ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS) and the slowest supported update
// rate (ROCKETNET_SERVER_RECEIVE_MIN_UPDATE_HZ).
//
// rocketnet_server_transport_receive_buffer_request is the SO_RCVBUF every
// server socket of a built-in transport asks for, and the size the host memory
// estimate charges per socket. Each socket is sized for the config's full
// max_clients, every gso and io_uring queue socket included. A queue socket is not sized
// for a share of the clients: SO_REUSEPORT hashes flows to sockets, so any
// one socket may carry any number of them. xdp opens no UDP socket and returns
// 0, as does a name that is not a built-in transport.
int rocketnet_server_receive_buffer_bytes( int max_clients, int receive_kbps_per_client, int update_hz );
int rocketnet_server_receive_buffer_request( int max_clients );
int rocketnet_server_transport_receive_buffer_request( const char * transport, int max_clients );

// logs one line per server socket with what the kernel granted and what was
// requested. A grant below the request is a warning naming the limit to
// raise (net.core.rmem_max on Linux, kern.ipc.maxsockbuf on macOS and BSD);
// a grant at or above the request is an information line
void rocketnet_server_receive_buffer_granted( const char * name, int requested_bytes, int granted_bytes );

#if ROCKETNET_DEV
// test-only decision override. The production socket grant is still observed;
// this changes only the requested-versus-granted warning branch
void rocketnet_server_receive_buffer_warning_grant_override_for_testing( int granted_bytes );
#endif // #if ROCKETNET_DEV

// the receive buffer the kernel granted a socket, in the bytes requested
// (Linux reports double its grant; this halves it); 0 when unreadable
struct rocketnet_platform_socket_t;
int rocketnet_platform_socket_receive_buffer_bytes( struct rocketnet_platform_socket_t * socket );

// internal final-burst operation for public server_stop: the slot ends now
// and its burst always waits in the disconnect ring, which holds one for
// every client beside the bursts a running server keeps, for the stop steps
// to send (proofs/tla/ServerStopSpread.tla Stop)
void rocketnet_server_transport_disconnect_client_shutdown( struct rocketnet_server_transport_t * transport, int client_index );

// A create's reason for failing, written through its caller's status (NULL:
// not asked). OUT_OF_MEMORY, once written, is never replaced: a create that
// was refused an allocation anywhere fails for want of memory, and the
// transport ladder ends there instead of falling to the next candidate.
static inline void rocketnet_create_status( int * status, int value )
{
    if ( status && *status != ROCKETNET_STATUS_OUT_OF_MEMORY )
        *status = value;
}

#if ROCKETNET_DEV
// the server config staging seam (sdk/rocketnet_server_transport.cpp): each
// stack copy of a server config is wiped on every exit of the function that
// holds it, then its server_secret_key is handed to the hook with the stage
// that held it. NULL by default; set only by unit cases.
enum
{
    ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_RESOLVE = 0,      // the resolve's layered config
    ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_CREATE = 1,       // the create's resolved inputs
    ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_COUNT = 2
};

extern void ( *rocketnet_server_transport_config_staging_hook )( const uint8_t * secret_key, int stage );
#endif // #if ROCKETNET_DEV

// held across the draws of every caller that may run before rocketnet_init
// or beside rocketnet_term (the key and token generators): opens sodium and
// its random source on first use, and keeps term from closing the source
// until the matching release (sdk/rocketnet.cpp). Returns false, holding
// nothing, when sodium cannot initialize.
bool rocketnet_crypto_acquire( void );
void rocketnet_crypto_release( void );

#endif // ROCKETNET_SDK_ROCKETNET_INTERNAL_H

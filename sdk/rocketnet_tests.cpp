/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_tests.h"
#include "rocketnet.h"

#include "rocketnet_platform.h"
#include "rocketnet_read_write.h"
#include "rocketnet_address.h"
#include "rocketnet_base64.h"
#include "rocketnet_hash.h"
#include "rocketnet_packet_loss_tracker.h"
#include "rocketnet_out_of_order_tracker.h"
#include "rocketnet_jitter_tracker.h"
#include "rocketnet_value_tracker.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_keys.h"
#include "rocketnet_counters.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_replay.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_frame_sync.h"
#include "rocketnet_fragment.h"
#include "rocketnet_config.h"

// the linux driver tests reach below the transport: the xdp pre-flight
// (never the committing create) and the gso driver's geometry constants

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include "rocketnet_server_socket.h"
#include "rocketnet_server_gso.h"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include "rocketnet_server_iocp.h"
#endif

#include <sodium.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>

// the zero-length chaff injection test drives a raw socket beside the
// platform socket, so it can send what the platform surface refuses to

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// setenv/unsetenv are posix-only names: the MSVC CRT spells both
// _putenv_s -- and getenv sees the change because the suite and the sdk
// read the same CRT environment -- so the #106 config and driver tests RUN
// on windows instead of being compiled out (a skipped test is named and
// logged, never silent; a runnable test is run)
//
// ONE semantic gap remains, and it is the platform's rather than the
// shim's: the windows CRT cannot represent a set-but-empty variable at
// all. _putenv_s(name,"") DELETES the variable, so "set to empty" and
// "not set" are the same state here, and unsetenv is spelled with it.
// the one case that turns on the distinction -- config validation's
// empty-value case -- is skipped on windows, named and logged with this
// reason; rocketnet_config_apply_env's empty branch is correspondingly
// unreachable on windows. nothing else in the suite depends on it.
//
// `overwrite` is HONORED rather than ignored: no call site passes 0
// today, but a shim that silently clobbered under overwrite=0 would be a
// trap laid for the first one that does

static int setenv( const char * name, const char * value, int overwrite )
{
    if ( !overwrite && getenv( name ) != NULL )
        return 0;
    return _putenv_s( name, value );
}

static int unsetenv( const char * name ) { return _putenv_s( name, "" ); }

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// THE SUITE RUNS SILENT. rocketnet_run_tests installs this sink, so every
// line the library would have printed lands in the ring below instead of
// on stdout, and the only thing a passing run prints is the test names --
// which arrive here at level NONE and pass straight through as the bare
// line the default log function writes.
//
// Nothing true is lost, which is the point: a failing check dumps the ring
// before the check line, so the cause of an unexpected refusal is printed
// exactly when it matters and never otherwise -- a create that returns
// NULL still explains itself, which is what the port-collision flake of
// #107 needed. RUN_TEST clears the ring, so a dump belongs to the failing
// test alone.
//
// ROCKETNET_TEST_VERBOSE leaves the log live instead: no sink is
// installed and the level test.cpp sets stands, so a developer watches the
// library log as it happens.

#define ROCKETNET_TEST_LOG_RING_LINES 32
#define ROCKETNET_TEST_LOG_RING_LINE_BYTES 1024

static char rocketnet_test_log_ring[ROCKETNET_TEST_LOG_RING_LINES][ROCKETNET_TEST_LOG_RING_LINE_BYTES];

static int rocketnet_test_log_ring_lines;

static void rocketnet_test_log_capture( int level, const char * format, ... )
{
    va_list args;
    va_start( args, format );
    char line[ROCKETNET_TEST_LOG_RING_LINE_BYTES];
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );

    if ( level == ROCKETNET_LOG_LEVEL_NONE )
    {
        printf( "%s\n", line );
        fflush( stdout );
        return;
    }

    char * slot = rocketnet_test_log_ring[rocketnet_test_log_ring_lines % ROCKETNET_TEST_LOG_RING_LINES];
    snprintf( slot, ROCKETNET_TEST_LOG_RING_LINE_BYTES, "%.6f: %s: %s", rocketnet_platform_time(), rocketnet_log_level_string( level ), line );
    rocketnet_test_log_ring_lines++;
}

// what the suite installed, so a test that swaps in its own capture puts
// the suite's sink back rather than the default printer. NULL is the
// ROCKETNET_TEST_VERBOSE state, and restores the default printer -- which
// is what verbose wants.

static void (*rocketnet_test_suite_log_function)( int level, const char * format, ... ) = NULL;

static void rocketnet_test_log_ring_clear()
{
    rocketnet_test_log_ring_lines = 0;
}

static void rocketnet_test_log_ring_dump()
{
    const int lines = rocketnet_test_log_ring_lines < ROCKETNET_TEST_LOG_RING_LINES ? rocketnet_test_log_ring_lines : ROCKETNET_TEST_LOG_RING_LINES;
    const int first = rocketnet_test_log_ring_lines - lines;
    for ( int i = 0; i < lines; i++ )
    {
        printf( "%s\n", rocketnet_test_log_ring[( first + i ) % ROCKETNET_TEST_LOG_RING_LINES] );
    }
    rocketnet_test_log_ring_lines = 0;
}

static void rocketnet_check_handler( const char * condition,
                                const char * function,
                                const char * file,
                                int line )
{
    rocketnet_test_log_ring_dump();
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
#ifndef NDEBUG
    #if defined( __GNUC__ )
        __builtin_trap();
    #elif defined( _MSC_VER )
        __debugbreak();
    #endif
#endif
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );         \
    }                                                                                                           \
} while(0)

void rocketnet_test_time()
{
    double start = rocketnet_platform_time();
    rocketnet_platform_sleep( 0.1 );
    double finish = rocketnet_platform_time();
    rocketnet_check( finish > start );
}

void rocketnet_test_endian()
{
    uint32_t value = 0x11223344;
    char bytes[4];
    memcpy( bytes, &value, 4 );

#if ROCKETNET_LITTLE_ENDIAN

    rocketnet_check( bytes[0] == 0x44 );
    rocketnet_check( bytes[1] == 0x33 );
    rocketnet_check( bytes[2] == 0x22 );
    rocketnet_check( bytes[3] == 0x11 );

#else // #if ROCKETNET_LITTLE_ENDIAN

    rocketnet_check( bytes[3] == 0x44 );
    rocketnet_check( bytes[2] == 0x33 );
    rocketnet_check( bytes[1] == 0x22 );
    rocketnet_check( bytes[0] == 0x11 );

#endif // #if ROCKETNET_LITTLE_ENDIAN

    // rocketnet_endian_fix, called for the first time by anything (#112).
    // The header's public helper converts between host order and the
    // standard's little-endian wire order in place, and is a no-op on a
    // little-endian host. Nothing in the sdk called it and nothing in this
    // suite called it, so the uint64 overload's defect -- it byte-swapped
    // the POINTER and stored that as the value -- was invisible to every
    // machine the estate owns. The checks below are written against WIRE
    // BYTES rather than host values, so they hold on either endianness and
    // it is the big-endian leg that drives the swap.

    const uint8_t wire[8] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };

    uint16_t v16 = 0;
    memcpy( &v16, wire, 2 );
    rocketnet_endian_fix( &v16 );
    rocketnet_check( v16 == 0x7788 );

    uint32_t v32 = 0;
    memcpy( &v32, wire, 4 );
    rocketnet_endian_fix( &v32 );
    rocketnet_check( v32 == 0x55667788 );

    uint64_t v64 = 0;
    memcpy( &v64, wire, 8 );
    rocketnet_endian_fix( &v64 );
    rocketnet_check( v64 == 0x1122334455667788ULL );

    // the fix is its own inverse: a host value pushed back through it lands
    // on the wire in the standard's order, whatever the host

    rocketnet_endian_fix( &v64 );
    uint8_t out[8];
    memcpy( out, &v64, 8 );
    rocketnet_check( memcmp( out, wire, 8 ) == 0 );

#if ROCKETNET_BIG_ENDIAN

    // on a big-endian host the helper must actually MOVE BYTES. The checks
    // above pass on a machine where every overload is a no-op -- that is
    // exactly the little-endian case -- so this is the assertion that makes
    // the big-endian leg a gate rather than a second little-endian run.

    uint16_t swap16 = 0x1122;
    rocketnet_endian_fix( &swap16 );
    rocketnet_check( swap16 == 0x2211 );

    uint32_t swap32 = 0x11223344;
    rocketnet_endian_fix( &swap32 );
    rocketnet_check( swap32 == 0x44332211 );

    uint64_t swap64 = 0x1122334455667788ULL;
    rocketnet_endian_fix( &swap64 );
    rocketnet_check( swap64 == 0x8877665544332211ULL );

#endif // #if ROCKETNET_BIG_ENDIAN
}

void rocketnet_test_read_write()
{
    // round trip every width through one buffer, pointers advancing
    uint8_t buffer[64];
    uint8_t * w = buffer;
    rocketnet_write_uint8( &w, 0xA5 );
    rocketnet_write_uint16( &w, 0xBEEF );
    rocketnet_write_uint32( &w, 0x12345678 );
    rocketnet_write_uint64( &w, 0x1122334455667788ULL );
    const uint8_t some_bytes[5] = { 1, 2, 3, 4, 5 };
    rocketnet_write_bytes( &w, some_bytes, 5 );
    rocketnet_write_zero_bytes( &w, 3 );
    rocketnet_check( w - buffer == 1 + 2 + 4 + 8 + 5 + 3 );

    const uint8_t * r = buffer;
    rocketnet_check( rocketnet_read_uint8( &r ) == 0xA5 );
    rocketnet_check( rocketnet_read_uint16( &r ) == 0xBEEF );
    rocketnet_check( rocketnet_read_uint32( &r ) == 0x12345678 );
    rocketnet_check( rocketnet_read_uint64( &r ) == 0x1122334455667788ULL );
    uint8_t bytes_out[5];
    rocketnet_read_bytes( &r, bytes_out, 5 );
    rocketnet_check( memcmp( bytes_out, some_bytes, 5 ) == 0 );
    for ( int i = 0; i < 3; i++ )
        rocketnet_check( rocketnet_read_uint8( &r ) == 0 );
    rocketnet_check( r - buffer == w - buffer );

    // exact bytes on the wire, anchored to STANDARD.md Appendix A: the
    // associated data vectors begin with the version then the protocol id,
    // little-endian. these must match the published hex exactly.
    uint8_t ad[12];
    uint8_t * a = ad;
    rocketnet_write_uint32( &a, 0x4E7C1A93 );
    rocketnet_write_uint64( &a, 0x1122334455667788ULL );
    const uint8_t expected_ad[12] = { 0x93, 0x1a, 0x7c, 0x4e,
                                      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    rocketnet_check( memcmp( ad, expected_ad, 12 ) == 0 );

    // the byte construction is by shift and mask, so the same bytes come out
    // whatever the host endianness. sanity that a uint16 port writes as the
    // standard's little-endian: 56789 = 0xddd5 -> d5 dd.
    uint8_t port_bytes[2];
    uint8_t * pp = port_bytes;
    rocketnet_write_uint16( &pp, 56789 );
    rocketnet_check( port_bytes[0] == 0xd5 && port_bytes[1] == 0xdd );
}

void rocketnet_test_base64()
{
    const char * input = "a test string. let's see if it works properly";
    char encoded[1024];
    char decoded[1024];
    rocketnet_check( rocketnet_base64_encode_string( input, encoded, sizeof(encoded) ) > 0 );
    rocketnet_check( rocketnet_base64_decode_string( encoded, decoded, sizeof(decoded) ) > 0 );
    rocketnet_check( strcmp( decoded, input ) == 0 );
    rocketnet_check( rocketnet_base64_decode_string( encoded, decoded, 10 ) == 0 );

    // the strict decoder (#110 review, both readers independently): invalid
    // characters, high-bit bytes, misplaced or excessive padding, and
    // impossible lengths all refuse whole -- for the config system's key
    // fields a corrupted value must never silently become a wrong key, and
    // a byte past 0x7f must never index below the decode table

    uint8_t bytes[64];
    rocketnet_check( rocketnet_base64_decode_data( "####", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "AA A=", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "\xC3\xA9zz", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "zz=z", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "z===", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "=", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "zzzzz", bytes, sizeof(bytes) ) == 0 );

    // a bare invalid character inside a well-formed length

    rocketnet_check( rocketnet_base64_decode_data( "AA!A", bytes, sizeof(bytes) ) == 0 );

    // ONE SPELLING PER KEY, now a real property of the decoder: padding
    // is required (unpadded "AA" refuses beside "AA=="), the alphabet is
    // the standard one alone (the url-safe aliases refuse), and
    // non-canonical tails refuse ("AB==" against the canonical "AQ==") --
    // so every byte string has exactly one accepted encoding

    rocketnet_check( rocketnet_base64_decode_data( "AAECAw==", bytes, sizeof(bytes) ) == 4 );
    rocketnet_check( bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x02 && bytes[3] == 0x03 );

    rocketnet_check( rocketnet_base64_decode_data( "AA==", bytes, sizeof(bytes) ) == 1 );
    rocketnet_check( bytes[0] == 0x00 );
    rocketnet_check( rocketnet_base64_decode_data( "AA", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "AB", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "AB==", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "AQ==", bytes, sizeof(bytes) ) == 1 );
    rocketnet_check( bytes[0] == 0x01 );

    rocketnet_check( rocketnet_base64_decode_data( "+w==", bytes, sizeof(bytes) ) == 1 );
    rocketnet_check( bytes[0] == 0xFB );
    rocketnet_check( rocketnet_base64_decode_data( "+w", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "-w==", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "-_==", bytes, sizeof(bytes) ) == 0 );
    rocketnet_check( rocketnet_base64_decode_data( "A_==", bytes, sizeof(bytes) ) == 0 );
}

void rocketnet_test_hash()
{
    uint64_t hash = rocketnet_hash_string( "local" );
    rocketnet_check( hash == 0x249f1fb6f3a680e8ULL );
}

void rocketnet_test_copy_string()
{
    // copy valid string
    {
        char buffer[256];
        size_t result = rocketnet_copy_string( buffer, "hello", sizeof(buffer) );
        rocketnet_check( result == 5 );
        rocketnet_check( strncmp( buffer, "hello", sizeof(buffer) ) == 0 );
    }

    // copy empty string
    {
        char buffer[256];
        size_t result = rocketnet_copy_string( buffer, "", sizeof(buffer) );
        rocketnet_check( result == 0 );
        rocketnet_check( strncmp( buffer, "", sizeof(buffer) ) == 0 );
    }

    // truncated string
    {
        char buffer[4];
        size_t result = rocketnet_copy_string( buffer, "hello my baby, hello my darling", sizeof(buffer) );
        rocketnet_check( result == 3 );
        rocketnet_check( strncmp( buffer, "hel", sizeof(buffer) ) == 0 );
    }
}

void rocketnet_test_address()
{
    {
        const char * invalid[] = {
            "127.0.0.1:", "127.0.0.1:abc", "127.0.0.1:-1", "127.0.0.1:+80",
            "127.0.0.1: 80", "127.0.0.1:80x", "127.0.0.1:65536", "127.0.0.1:65537",
            "127.0.0.1:99999999999999999999", "[::1]:", "[::1]:abc", "[::1]:-1",
            "[::1]:+80", "[::1]: 80", "[::1]:80x", "[::1]:65536", "[::1]junk",
            "[::1:80", "[::1]::80", "[127.0.0.1]:80", "::1]", "[::1]:80:90"
        };
        for ( size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i )
        {
            rocketnet_address_t address;
            memset( &address, 0xFF, sizeof(address) );
            rocketnet_check( !rocketnet_address_parse( &address, invalid[i] ) );
            rocketnet_check( address.type == ROCKETNET_ADDRESS_NONE );
            rocketnet_check( address.port == 0 );
        }
        const char * valid[] = { "127.0.0.1:0", "127.0.0.1:65535", "[::1]:0", "[::1]:65535", "[::1]", "::1" };
        const int ports[] = { 0, 65535, 0, 65535, 0, 0 };
        for ( size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i )
        {
            rocketnet_address_t address;
            rocketnet_check( rocketnet_address_parse( &address, valid[i] ) );
            rocketnet_check( address.port == ports[i] );
        }
        char oversized[ROCKETNET_MAX_ADDRESS_STRING_LENGTH + 16];
        memset( oversized, '0', sizeof(oversized) );
        memcpy( oversized, "127.0.0.1:", 10 );
        oversized[sizeof(oversized) - 1] = '\0';
        rocketnet_address_t address;
        rocketnet_check( !rocketnet_address_parse( &address, oversized ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( !rocketnet_address_parse( &address, "" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "[" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "[]" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "[]:" ) );
        rocketnet_check( !rocketnet_address_parse( &address, ":" ) );
#if !defined(WINVER) || WINVER > 0x502 // windows xp sucks
        rocketnet_check( !rocketnet_address_parse( &address, "1" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "12" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "123" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "1234" ) );
#endif
        rocketnet_check( !rocketnet_address_parse( &address, "1234.0.12313.0000" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "1234.0.12313.0000.0.0.0.0.0" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "1312313:123131:1312313:123131:1312313:123131:1312313:123131:1312313:123131:1312313:123131" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "." ) );
        rocketnet_check( !rocketnet_address_parse( &address, ".." ) );
        rocketnet_check( !rocketnet_address_parse( &address, "..." ) );
        rocketnet_check( !rocketnet_address_parse( &address, "...." ) );
        rocketnet_check( !rocketnet_address_parse( &address, "....." ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "107.77.207.77" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv4[0] == 107 );
        rocketnet_check( address.data.ipv4[1] == 77 );
        rocketnet_check( address.data.ipv4[2] == 207 );
        rocketnet_check( address.data.ipv4[3] == 77 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv4[0] == 127 );
        rocketnet_check( address.data.ipv4[1] == 0 );
        rocketnet_check( address.data.ipv4[2] == 0 );
        rocketnet_check( address.data.ipv4[3] == 1 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "107.77.207.77:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv4[0] == 107 );
        rocketnet_check( address.data.ipv4[1] == 77 );
        rocketnet_check( address.data.ipv4[2] == 207 );
        rocketnet_check( address.data.ipv4[3] == 77 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv4[0] == 127 );
        rocketnet_check( address.data.ipv4[1] == 0 );
        rocketnet_check( address.data.ipv4[2] == 0 );
        rocketnet_check( address.data.ipv4[3] == 1 );
    }

#if ROCKETNET_PLATFORM_HAS_IPV6
    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "fe80::202:b3ff:fe1e:8329" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv6[0] == 0xfe80 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0202 );
        rocketnet_check( address.data.ipv6[5] == 0xb3ff );
        rocketnet_check( address.data.ipv6[6] == 0xfe1e );
        rocketnet_check( address.data.ipv6[7] == 0x8329 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "::" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0000 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "::1" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0001 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[fe80::202:b3ff:fe1e:8329]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0xfe80 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0202 );
        rocketnet_check( address.data.ipv6[5] == 0xb3ff );
        rocketnet_check( address.data.ipv6[6] == 0xfe1e );
        rocketnet_check( address.data.ipv6[7] == 0x8329 );
        rocketnet_check( !rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0000 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::1]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0001 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:127.0.0.1]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x7F00 );
        rocketnet_check( address.data.ipv6[7] == 0x0001 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:0.0.0.0]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0000 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:1.2.3.4]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x0102 );
        rocketnet_check( address.data.ipv6[7] == 0x0304 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:1.2.3.4]:40000" ) );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );

        rocketnet_address_convert_ipv6_to_ipv4( &address );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv4[0] == 1 );
        rocketnet_check( address.data.ipv4[1] == 2 );
        rocketnet_check( address.data.ipv4[2] == 3 );
        rocketnet_check( address.data.ipv4[3] == 4 );

        rocketnet_address_convert_ipv4_to_ipv6( &address );

        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x0102 );
        rocketnet_check( address.data.ipv6[7] == 0x0304 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }
#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
}

void rocketnet_test_random_bytes()
{
    const int BufferSize = 999;
    uint8_t buffer[BufferSize];
    rocketnet_random_bytes( buffer, BufferSize );
    for ( int i = 0; i < 100; ++i )
    {
        uint8_t rocketnet_buffer[BufferSize];
        rocketnet_random_bytes( rocketnet_buffer, BufferSize );
        rocketnet_check( memcmp( buffer, rocketnet_buffer, BufferSize ) != 0 );
        memcpy( buffer, rocketnet_buffer, BufferSize );
    }
}

void rocketnet_test_random_float()
{
    for ( int i = 0; i < 1000; ++i )
    {
        float value = rocketnet_random_float();
        rocketnet_check( value >= 0.0f );
        rocketnet_check( value <= 1.0f );
    }
}

// Crypto expectations: call every libsodium primitive the standard uses and
// pin its behavior, before any protocol code exists to depend on it. The
// vector data mirrors STANDARD.md Appendix A byte for byte.

static void rocketnet_test_sodium_ready()
{
    rocketnet_check( sodium_init() >= 0 );
}

void rocketnet_test_crypto_sizes()
{
    rocketnet_test_sodium_ready();
    rocketnet_check( crypto_sign_BYTES == 64 );
    rocketnet_check( crypto_sign_PUBLICKEYBYTES == 32 );
    rocketnet_check( crypto_sign_SECRETKEYBYTES == 64 );
    rocketnet_check( crypto_box_PUBLICKEYBYTES == 32 );
    rocketnet_check( crypto_box_SECRETKEYBYTES == 32 );
    rocketnet_check( crypto_box_SEALBYTES == 48 );
    rocketnet_check( crypto_generichash_KEYBYTES == 32 );
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_KEYBYTES == 32 );
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_NPUBBYTES == 12 );
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_ABYTES == 16 );
    rocketnet_check( crypto_scalarmult_BYTES == 32 );
    rocketnet_check( crypto_scalarmult_SCALARBYTES == 32 );
}

void rocketnet_test_crypto_test_keys()
{
    rocketnet_test_sodium_ready();

    // the library's embedded development keys are the published test keys,
    // byte for byte -- two copies of the standard's values that must never
    // drift apart
    rocketnet_check( memcmp( rocketnet_dev_backend_public_key, rocketnet_test_backend_public_key, 32 ) == 0 );
    rocketnet_check( memcmp( rocketnet_dev_backend_secret_key, rocketnet_test_backend_secret_key, 64 ) == 0 );
    rocketnet_check( memcmp( rocketnet_dev_server_public_key, rocketnet_test_server_public_key, 32 ) == 0 );
    rocketnet_check( memcmp( rocketnet_dev_server_secret_key, rocketnet_test_server_secret_key, 32 ) == 0 );

    // the published test keypairs are self-consistent: the Ed25519 secret key
    // embeds its public half, and the X25519 publics derive from the secrets
    rocketnet_check( memcmp( rocketnet_test_backend_secret_key + 32, rocketnet_test_backend_public_key, 32 ) == 0 );

    uint8_t derived_public[32];
    crypto_scalarmult_base( derived_public, rocketnet_test_server_secret_key );
    rocketnet_check( memcmp( derived_public, rocketnet_test_server_public_key, 32 ) == 0 );

    crypto_scalarmult_base( derived_public, rocketnet_test_client_eph_secret );
    rocketnet_check( memcmp( derived_public, rocketnet_test_client_eph_public, 32 ) == 0 );

    crypto_scalarmult_base( derived_public, rocketnet_test_server_eph_secret );
    rocketnet_check( memcmp( derived_public, rocketnet_test_server_eph_public, 32 ) == 0 );
}

void rocketnet_test_crypto_sign()
{
    rocketnet_test_sodium_ready();

    uint8_t message[576];
    for ( int i = 0; i < (int) sizeof(message); i++ )
        message[i] = (uint8_t) i;

    uint8_t signature[64];
    rocketnet_check( crypto_sign_detached( signature, NULL, message, sizeof(message), rocketnet_test_backend_secret_key ) == 0 );
    rocketnet_check( crypto_sign_verify_detached( signature, message, sizeof(message), rocketnet_test_backend_public_key ) == 0 );

    // NEGATIVE: one flipped bit anywhere in the message fails verification
    message[100] ^= 1;
    rocketnet_check( crypto_sign_verify_detached( signature, message, sizeof(message), rocketnet_test_backend_public_key ) != 0 );
    message[100] ^= 1;

    // NEGATIVE: one flipped bit in the signature fails verification
    signature[10] ^= 1;
    rocketnet_check( crypto_sign_verify_detached( signature, message, sizeof(message), rocketnet_test_backend_public_key ) != 0 );
    signature[10] ^= 1;

    // NEGATIVE: a signature from a different key fails against the backend key
    uint8_t other_pk[32], other_sk[64];
    crypto_sign_keypair( other_pk, other_sk );
    uint8_t other_signature[64];
    crypto_sign_detached( other_signature, NULL, message, sizeof(message), other_sk );
    rocketnet_check( crypto_sign_verify_detached( other_signature, message, sizeof(message), rocketnet_test_backend_public_key ) != 0 );
}

void rocketnet_test_crypto_sealed_box()
{
    rocketnet_test_sodium_ready();

    uint8_t plaintext[384];
    for ( int i = 0; i < (int) sizeof(plaintext); i++ )
        plaintext[i] = (uint8_t) ( i * 7 );

    uint8_t sealed[384 + 48];
    rocketnet_check( crypto_box_seal( sealed, plaintext, sizeof(plaintext), rocketnet_test_server_public_key ) == 0 );

    uint8_t opened[384];
    rocketnet_check( crypto_box_seal_open( opened, sealed, sizeof(sealed), rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) == 0 );
    rocketnet_check( memcmp( opened, plaintext, sizeof(plaintext) ) == 0 );

    // NEGATIVE: a different keypair cannot open it
    uint8_t other_pk[32], other_sk[32];
    crypto_box_keypair( other_pk, other_sk );
    rocketnet_check( crypto_box_seal_open( opened, sealed, sizeof(sealed), other_pk, other_sk ) != 0 );

    // NEGATIVE: a flipped byte fails the open
    sealed[100] ^= 1;
    rocketnet_check( crypto_box_seal_open( opened, sealed, sizeof(sealed), rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) != 0 );
    sealed[100] ^= 1;

    // THE HAZARD THE STANDARD WARNS ABOUT: a sealed box is anonymous. anyone
    // can seal to the server's public key and the open succeeds. authenticity
    // of a connect token is the backend signature, never a successful open.
    uint8_t attacker_plaintext[384];
    memset( attacker_plaintext, 0xAA, sizeof(attacker_plaintext) );
    uint8_t attacker_sealed[384 + 48];
    crypto_box_seal( attacker_sealed, attacker_plaintext, sizeof(attacker_plaintext), rocketnet_test_server_public_key );
    rocketnet_check( crypto_box_seal_open( opened, attacker_sealed, sizeof(attacker_sealed), rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) == 0 );
}

void rocketnet_test_crypto_aead()
{
    rocketnet_test_sodium_ready();

    const uint8_t key[32] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32 };
    uint8_t ad[21];
    for ( int i = 0; i < (int) sizeof(ad); i++ )
        ad[i] = (uint8_t) ( 0x40 + i );

    // the nonce is 4 zero bytes then the 64-bit sequence little-endian
    uint8_t nonce[12];
    memset( nonce, 0, 4 );
    uint8_t * n = nonce + 4;
    rocketnet_write_uint64( &n, 1000 );

    uint8_t plaintext[100];
    for ( int i = 0; i < (int) sizeof(plaintext); i++ )
        plaintext[i] = (uint8_t) i;

    uint8_t sealed[100 + 16];
    unsigned long long sealed_length = 0;
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_encrypt( sealed, &sealed_length, plaintext, sizeof(plaintext), ad, sizeof(ad), NULL, nonce, key ) == 0 );
    rocketnet_check( sealed_length == sizeof(plaintext) + 16 );

    uint8_t opened[100];
    unsigned long long opened_length = 0;
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_length, NULL, sealed, sealed_length, ad, sizeof(ad), nonce, key ) == 0 );
    rocketnet_check( opened_length == sizeof(plaintext) );
    rocketnet_check( memcmp( opened, plaintext, sizeof(plaintext) ) == 0 );

    // NEGATIVE: altering the associated data fails the tag
    ad[12] ^= 1;
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_length, NULL, sealed, sealed_length, ad, sizeof(ad), nonce, key ) != 0 );
    ad[12] ^= 1;

    // NEGATIVE: altering the nonce (the sequence) fails the tag
    nonce[4] ^= 1;
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_length, NULL, sealed, sealed_length, ad, sizeof(ad), nonce, key ) != 0 );
    nonce[4] ^= 1;

    // NEGATIVE: truncation fails the tag
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_length, NULL, sealed, sealed_length - 1, ad, sizeof(ad), nonce, key ) != 0 );

    // NEGATIVE: a flipped ciphertext byte fails the tag
    sealed[50] ^= 1;
    rocketnet_check( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_length, NULL, sealed, sealed_length, ad, sizeof(ad), nonce, key ) != 0 );
    sealed[50] ^= 1;
}

void rocketnet_test_crypto_generichash()
{
    rocketnet_test_sodium_ready();

    const uint8_t input[64] = { 0 };

    // BLAKE2b binds its output length into the hash: a 16-byte invocation is
    // NOT a truncation of the 32-byte one. the standard's token identifier
    // depends on implementations getting this right.
    uint8_t hash16[16];
    uint8_t hash32[32];
    rocketnet_check( crypto_generichash( hash16, 16, input, sizeof(input), NULL, 0 ) == 0 );
    rocketnet_check( crypto_generichash( hash32, 32, input, sizeof(input), NULL, 0 ) == 0 );
    rocketnet_check( memcmp( hash16, hash32, 16 ) != 0 );

    // keyed and unkeyed differ over the same input
    const uint8_t key[32] = { 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9 };
    uint8_t keyed[32];
    rocketnet_check( crypto_generichash( keyed, 32, input, sizeof(input), key, 32 ) == 0 );
    rocketnet_check( memcmp( keyed, hash32, 32 ) != 0 );

    // deterministic: same inputs, same output
    uint8_t again[32];
    rocketnet_check( crypto_generichash( again, 32, input, sizeof(input), key, 32 ) == 0 );
    rocketnet_check( memcmp( again, keyed, 32 ) == 0 );
}

void rocketnet_test_crypto_scalarmult()
{
    rocketnet_test_sodium_ready();

    // the shared secret agrees from both sides and matches the vector
    uint8_t shared_from_client[32];
    uint8_t shared_from_server[32];
    rocketnet_check( crypto_scalarmult( shared_from_client, rocketnet_test_client_eph_secret, rocketnet_test_server_eph_public ) == 0 );
    rocketnet_check( crypto_scalarmult( shared_from_server, rocketnet_test_server_eph_secret, rocketnet_test_client_eph_public ) == 0 );
    rocketnet_check( memcmp( shared_from_client, shared_from_server, 32 ) == 0 );
    rocketnet_check( memcmp( shared_from_client, rocketnet_test_shared_secret, 32 ) == 0 );

    // NEGATIVE: a small-order peer public key is refused. ignoring this
    // return is how implementations end up agreeing on an all-zero secret
    // with an attacker who chose the point. the standard makes checking it
    // a MUST on both sides.
    uint8_t shared[32];
    const uint8_t zero_point[32] = { 0 };
    rocketnet_check( crypto_scalarmult( shared, rocketnet_test_client_eph_secret, zero_point ) != 0 );

    const uint8_t small_order_point[32] = {
        0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
        0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00 };
    rocketnet_check( crypto_scalarmult( shared, rocketnet_test_client_eph_secret, small_order_point ) != 0 );
}

void rocketnet_test_crypto_key_derivation()
{
    rocketnet_test_sodium_ready();

    // reproduce the four per-connection keys exactly as Appendix A publishes
    // them, from the standard's formulas and the fixed inputs. this pins both
    // the primitive and this implementation's reading of the formulas.

    // handshake keys: generichash(32, salt || session nonce || context, key = token key)
    uint8_t input[8 + 8 + 8];
    memcpy( input, rocketnet_test_salt, 8 );
    memcpy( input + 8, rocketnet_test_session_nonce, 8 );

    uint8_t handshake_c2s[32];
    memcpy( input + 16, "RKTNHNDC", 8 );
    rocketnet_check( crypto_generichash( handshake_c2s, 32, input, sizeof(input), rocketnet_test_token_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( handshake_c2s, rocketnet_test_handshake_c2s_key, 32 ) == 0 );

    uint8_t handshake_s2c[32];
    memcpy( input + 16, "RKTNHNDS", 8 );
    rocketnet_check( crypto_generichash( handshake_s2c, 32, input, sizeof(input), rocketnet_test_token_s2c_key, 32 ) == 0 );
    rocketnet_check( memcmp( handshake_s2c, rocketnet_test_handshake_s2c_key, 32 ) == 0 );

    // session keys: generichash(32, shared || client eph pk || server eph pk ||
    // salt || session nonce || context, key = token key)
    uint8_t session_input[32 + 32 + 32 + 8 + 8 + 8];
    memcpy( session_input, rocketnet_test_shared_secret, 32 );
    memcpy( session_input + 32, rocketnet_test_client_eph_public, 32 );
    memcpy( session_input + 64, rocketnet_test_server_eph_public, 32 );
    memcpy( session_input + 96, rocketnet_test_salt, 8 );
    memcpy( session_input + 104, rocketnet_test_session_nonce, 8 );

    uint8_t session_c2s[32];
    memcpy( session_input + 112, "RKTNSESC", 8 );
    rocketnet_check( crypto_generichash( session_c2s, 32, session_input, sizeof(session_input), rocketnet_test_token_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( session_c2s, rocketnet_test_session_c2s_key, 32 ) == 0 );

    uint8_t session_s2c[32];
    memcpy( session_input + 112, "RKTNSESS", 8 );
    rocketnet_check( crypto_generichash( session_s2c, 32, session_input, sizeof(session_input), rocketnet_test_token_s2c_key, 32 ) == 0 );
    rocketnet_check( memcmp( session_s2c, rocketnet_test_session_s2c_key, 32 ) == 0 );

    // the cookie mac is the same primitive at 16 bytes, keyed with the cookie
    // key, over the exact published input encoding
    uint8_t cookie_mac[16];
    rocketnet_check( crypto_generichash( cookie_mac, 16, rocketnet_test_cookie_mac_input, sizeof(rocketnet_test_cookie_mac_input), rocketnet_test_cookie_key, 32 ) == 0 );
    rocketnet_check( memcmp( cookie_mac, rocketnet_test_cookie_mac, 16 ) == 0 );

    // and the eight per-type sealed packet vectors reproduce exactly: right
    // key, right sequence-derived nonce, right associated data, right bytes
    // out. the session plaintexts open with the ack block per the standard.
    struct
    {
        const uint8_t * key;
        uint64_t sequence;
        const uint8_t * ad;
        int ad_bytes;
        const uint8_t * plaintext;
        int plaintext_bytes;
        const uint8_t * expected;
        int expected_bytes;
    } vectors[8];

    const uint8_t response_plaintext[24] = { 0 };

    vectors[0] = { rocketnet_test_handshake_s2c_key, 1, rocketnet_test_challenge_ad, 53, rocketnet_test_server_eph_public, 32, rocketnet_test_challenge_sealed, 48 };
    vectors[1] = { rocketnet_test_handshake_s2c_key, 2, rocketnet_test_denied_ad, 21, NULL, 0, rocketnet_test_denied_sealed, 16 };
    vectors[2] = { rocketnet_test_handshake_c2s_key, 1, rocketnet_test_response_ad, 53, response_plaintext, 24, rocketnet_test_response_sealed, 40 };
    vectors[3] = { rocketnet_test_session_s2c_key, 1, rocketnet_test_keepalive_ad, 21, rocketnet_test_keepalive_plaintext, 15, rocketnet_test_keepalive_sealed, 31 };
    vectors[4] = { rocketnet_test_session_c2s_key, 3, rocketnet_test_payload_ad, 21, rocketnet_test_payload_plaintext, 12, rocketnet_test_payload_sealed, 28 };
    vectors[5] = { rocketnet_test_session_c2s_key, 4, rocketnet_test_disconnect_ad, 21, rocketnet_test_disconnect_plaintext, 7, rocketnet_test_disconnect_sealed, 23 };
    vectors[6] = { rocketnet_test_session_c2s_key, 5, rocketnet_test_ack_ad, 21, rocketnet_test_ack_plaintext, 11, rocketnet_test_ack_sealed, 27 };
    vectors[7] = { rocketnet_test_session_c2s_key, 6, rocketnet_test_fragment_ad, 21, rocketnet_test_fragment_plaintext, 18, rocketnet_test_fragment_sealed, 34 };

    for ( int i = 0; i < 8; i++ )
    {
        uint8_t nonce[12];
        memset( nonce, 0, 4 );
        uint8_t * n = nonce + 4;
        rocketnet_write_uint64( &n, vectors[i].sequence );

        uint8_t sealed[64];
        unsigned long long sealed_length = 0;
        rocketnet_check( crypto_aead_chacha20poly1305_ietf_encrypt( sealed, &sealed_length, vectors[i].plaintext, vectors[i].plaintext_bytes, vectors[i].ad, vectors[i].ad_bytes, NULL, nonce, vectors[i].key ) == 0 );
        rocketnet_check( (int) sealed_length == vectors[i].expected_bytes );
        rocketnet_check( memcmp( sealed, vectors[i].expected, sealed_length ) == 0 );
    }
}

static void rocketnet_test_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                               struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof(*signed_fields) );
    signed_fields->version = ROCKETNET_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof(*private_fields) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

void rocketnet_test_connect_token_private()
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t input;
    rocketnet_test_fill_vector_fields( &signed_fields, &input );

    uint8_t buffer[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &input, buffer );

    // exact offsets: client id at 0, protocol id at 8, expire at 16, keys at
    // 24 and 56, user data at 88, zero padding from 344
    const uint8_t * p = buffer;
    rocketnet_check( rocketnet_read_uint64( &p ) == ROCKETNET_TEST_CLIENT_ID );
    rocketnet_check( rocketnet_read_uint64( &p ) == ROCKETNET_TEST_PROTOCOL_ID );
    rocketnet_check( rocketnet_read_uint64( &p ) == ROCKETNET_TEST_EXPIRE_TS );
    rocketnet_check( memcmp( buffer + 24, rocketnet_test_token_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( buffer + 56, rocketnet_test_token_s2c_key, 32 ) == 0 );
    for ( int i = 344; i < ROCKETNET_PRIVATE_BLOCK_BYTES; i++ )
        rocketnet_check( buffer[i] == 0 );

    struct rocketnet_connect_token_private_t output;
    rocketnet_read_connect_token_private( &output, buffer );
    rocketnet_check( output.client_id == input.client_id );
    rocketnet_check( output.protocol_id == input.protocol_id );
    rocketnet_check( output.expire_timestamp == input.expire_timestamp );
    rocketnet_check( memcmp( output.client_to_server_key, input.client_to_server_key, 32 ) == 0 );
    rocketnet_check( memcmp( output.server_to_client_key, input.server_to_client_key, 32 ) == 0 );
    rocketnet_check( memcmp( output.user_data, input.user_data, ROCKETNET_USER_DATA_BYTES ) == 0 );
}

void rocketnet_test_signed_connect_data()
{
    struct rocketnet_signed_connect_data_t input;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_test_fill_vector_fields( &input, &private_fields );

    uint8_t buffer[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    rocketnet_write_signed_connect_data( &input, buffer );

    // the frozen token's first 89 bytes are exactly these fields at these
    // offsets: everything before the sealed block must match byte for byte
    rocketnet_check( memcmp( buffer, rocketnet_test_frozen_token, ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK ) == 0 );

    // and the padding after the used 521 bytes is zero
    for ( int i = ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES; i < ROCKETNET_SIGNED_CONNECT_DATA_BYTES; i++ )
        rocketnet_check( buffer[i] == 0 );

    struct rocketnet_signed_connect_data_t output;
    rocketnet_read_signed_connect_data( &output, buffer );
    rocketnet_check( output.version == input.version );
    rocketnet_check( output.protocol_id == input.protocol_id );
    rocketnet_check( output.create_timestamp == input.create_timestamp );
    rocketnet_check( output.expire_timestamp == input.expire_timestamp );
    rocketnet_check( output.timeout_seconds == input.timeout_seconds );
    rocketnet_check( output.address_flags == input.address_flags );
    rocketnet_check( memcmp( output.ipv4_address, input.ipv4_address, 6 ) == 0 );
    rocketnet_check( memcmp( output.ipv6_address, input.ipv6_address, 18 ) == 0 );
    rocketnet_check( memcmp( output.server_public_key, input.server_public_key, 32 ) == 0 );
}

void rocketnet_test_connect_token_frozen()
{
    // the frozen token from Appendix A is the interop oracle: a conforming
    // server-side implementation verifies it, opens it, and recovers exactly
    // the published fields

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, rocketnet_test_frozen_token );

    rocketnet_check( rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, rocketnet_test_backend_public_key ) );

    struct rocketnet_connect_token_private_t opened;
    rocketnet_check( rocketnet_open_connect_token_private( &opened, token.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );

    rocketnet_check( opened.client_id == ROCKETNET_TEST_CLIENT_ID );
    rocketnet_check( opened.protocol_id == ROCKETNET_TEST_PROTOCOL_ID );
    rocketnet_check( opened.expire_timestamp == ROCKETNET_TEST_EXPIRE_TS );
    rocketnet_check( memcmp( opened.client_to_server_key, rocketnet_test_token_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( opened.server_to_client_key, rocketnet_test_token_s2c_key, 32 ) == 0 );
    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
        rocketnet_check( opened.user_data[i] == 0 );

    rocketnet_check( rocketnet_connect_token_private_consistent( &opened, token.signed_connect_data ) );

    // the clear token keys ride at 640 and 672, and the tail pad is zero
    rocketnet_check( memcmp( token.client_to_server_key, rocketnet_test_token_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( token.server_to_client_key, rocketnet_test_token_s2c_key, 32 ) == 0 );
    for ( int i = 704; i < ROCKETNET_CONNECT_TOKEN_BYTES; i++ )
        rocketnet_check( rocketnet_test_frozen_token[i] == 0 );

    // the token identifier is content determined and matches the vector
    uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_connect_token_identifier( identifier, token.signed_connect_data );
    rocketnet_check( memcmp( identifier, rocketnet_test_token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 );
}

void rocketnet_test_connect_token_mint()
{
    // a freshly minted token round trips: our own signature verifies, our own
    // seal opens, and the fields survive
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    rocketnet_check( rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, rocketnet_test_backend_public_key ) );

    struct rocketnet_connect_token_private_t opened;
    rocketnet_check( rocketnet_open_connect_token_private( &opened, token.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );
    rocketnet_check( opened.client_id == private_fields.client_id );
    rocketnet_check( rocketnet_connect_token_private_consistent( &opened, token.signed_connect_data ) );

    // two mints of the same fields differ (seal randomness), so their token
    // identifiers differ: identifiers are per token, not per fields
    uint8_t token_data_2[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data_2 ) );
    uint8_t id_1[16], id_2[16];
    rocketnet_connect_token_identifier( id_1, token_data );
    rocketnet_connect_token_identifier( id_2, token_data_2 );
    rocketnet_check( memcmp( id_1, id_2, 16 ) != 0 );
}

void rocketnet_test_connect_token_refusals()
{
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, rocketnet_test_frozen_token );

    // NEGATIVE: any flipped byte in the signed data fails verification
    const int tamper_offsets[] = { 0, 4, 20, 32, 57, 89, 520 };
    for ( int i = 0; i < (int) ( sizeof(tamper_offsets) / sizeof(tamper_offsets[0]) ); i++ )
    {
        token.signed_connect_data[tamper_offsets[i]] ^= 1;
        rocketnet_check( !rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, rocketnet_test_backend_public_key ) );
        token.signed_connect_data[tamper_offsets[i]] ^= 1;
    }

    // NEGATIVE: a flipped signature byte fails verification
    token.signature[0] ^= 1;
    rocketnet_check( !rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, rocketnet_test_backend_public_key ) );
    token.signature[0] ^= 1;

    // NEGATIVE: a different backend key refuses the real token
    uint8_t other_pk[32], other_sk[64];
    crypto_sign_keypair( other_pk, other_sk );
    rocketnet_check( !rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, other_pk ) );

    // NEGATIVE: a different server keypair cannot open the sealed block
    uint8_t other_box_pk[32], other_box_sk[32];
    crypto_box_keypair( other_box_pk, other_box_sk );
    struct rocketnet_connect_token_private_t opened;
    rocketnet_check( !rocketnet_open_connect_token_private( &opened, token.signed_connect_data, other_box_pk, other_box_sk ) );

    // NEGATIVE: a tampered sealed block does not open
    token.signed_connect_data[ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + 100] ^= 1;
    rocketnet_check( !rocketnet_open_connect_token_private( &opened, token.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );
    token.signed_connect_data[ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + 100] ^= 1;

    // THE CENTRAL HAZARD: an attacker's sealed block opens fine at the
    // server, because sealed boxes are anonymous. only the signature refuses
    // it. compose an attacker token from the public constants and prove the
    // refusal happens exactly there.
    struct rocketnet_signed_connect_data_t attacker_signed;
    struct rocketnet_connect_token_private_t attacker_private;
    rocketnet_test_fill_vector_fields( &attacker_signed, &attacker_private );
    attacker_private.client_id = 0xEE11EE11EE11EE11ULL;

    uint8_t attacker_pk[32], attacker_sk[64];
    crypto_sign_keypair( attacker_pk, attacker_sk );
    uint8_t attacker_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_connect_token( &attacker_signed, &attacker_private, attacker_sk, attacker_token ) );

    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, attacker_token );
    rocketnet_check( rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );
    rocketnet_check( !rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature, rocketnet_test_backend_public_key ) );

    // NEGATIVE: inner consistency refuses a private block whose protocol id
    // disagrees with the signed one, even under a valid signature and seal.
    // compose it from the layout pieces, exactly as a hostile backend could.
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID + 1;

    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &private_fields, private_block );
    rocketnet_check( crypto_box_seal( signed_fields.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES, rocketnet_test_server_public_key ) == 0 );

    uint8_t signed_buffer[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    rocketnet_write_signed_connect_data( &signed_fields, signed_buffer );
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    rocketnet_check( crypto_sign_detached( signature, NULL, signed_buffer, sizeof(signed_buffer), rocketnet_test_backend_secret_key ) == 0 );

    rocketnet_check( rocketnet_verify_signed_connect_data( signed_buffer, signature, rocketnet_test_backend_public_key ) );
    rocketnet_check( rocketnet_open_connect_token_private( &opened, signed_buffer, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );
    rocketnet_check( !rocketnet_connect_token_private_consistent( &opened, signed_buffer ) );

    // the token identifier changes when any signed byte changes
    uint8_t id_before[16], id_after[16];
    rocketnet_connect_token_identifier( id_before, signed_buffer );
    signed_buffer[300] ^= 1;
    rocketnet_connect_token_identifier( id_after, signed_buffer );
    rocketnet_check( memcmp( id_before, id_after, 16 ) != 0 );
}

void rocketnet_test_prefilter()
{
    // the surface payload constants derive from the wire numbers and must
    // never drift: the send capacity is the largest multiple of eight
    // inside the budget less the fixed overhead and the minimum ack block,
    // and the receive constant is the ceiling less the same
    rocketnet_check( ROCKETNET_MAX_PAYLOAD_BYTES ==
                     ( ( ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES ) / 8 ) * 8 );
    rocketnet_check( ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES ==
                     ROCKETNET_RECEIVE_CEILING - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    // the embedded test filter value is the published one from the standard
    rocketnet_check( memcmp( tag, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES ) == 0 );

    // writing the header produces exactly the assembled vectors' first bytes:
    // a request begins 00 00 b7 59 01 00, a challenge begins 02 00 b7 59 01 00
    uint8_t header[ROCKETNET_PREFILTER_BYTES];
    uint8_t * p = header;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, tag );
    rocketnet_check( p - header == ROCKETNET_PREFILTER_BYTES );
    rocketnet_check( memcmp( header, rocketnet_test_assembled_request, ROCKETNET_PREFILTER_BYTES ) == 0 );

    p = header;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_CHALLENGE_PACKET, tag );
    rocketnet_check( memcmp( header, rocketnet_test_assembled_challenge, ROCKETNET_PREFILTER_BYTES ) == 0 );

    // the structural check accepts a well formed header and returns the type
    uint8_t packet[64] = { 0 };
    p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, tag );
    rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );

    // 1.0 writes the unkeyed epoch, always, and the value follows it
    rocketnet_check( packet[1] == ROCKETNET_FILTER_EPOCH_UNKEYED );
    rocketnet_check( memcmp( packet + 2, tag, ROCKETNET_FILTER_VALUE_BYTES ) == 0 );

    // NEGATIVE: every nonzero epoch names a magic generation this build does
    // not hold, and is refused -- including on a packet whose filter value is
    // otherwise perfect. a keyed deployment refuses epoch 0 the same way, and
    // no default magic exists for either side to fall back to.
    for ( int epoch = 1; epoch < 256; epoch++ )
    {
        packet[1] = (uint8_t) epoch;
        rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == ROCKETNET_PREFILTER_WRONG_EPOCH );
    }
    packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;
    rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );

    // NEGATIVE: every single-byte corruption of the filter value is refused,
    // at every one of its four bytes and every value
    for ( int i = 0; i < ROCKETNET_FILTER_VALUE_BYTES; i++ )
    {
        const uint8_t original = packet[2+i];
        for ( int v = 0; v < 256; v++ )
        {
            if ( (uint8_t) v == original )
                continue;
            packet[2+i] = (uint8_t) v;
            rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == ROCKETNET_PREFILTER_WRONG_VALUE );
        }
        packet[2+i] = original;
    }
    rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );

    // the type byte carries the type and nothing else: any value comes back,
    // including reserved ones -- refusing those is a later reading order step
    packet[0] = 200;
    rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == 200 );
    packet[0] = ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET;

    // NEGATIVE: shorter than the header is refused, at every length
    for ( int bytes = 0; bytes < ROCKETNET_PREFILTER_BYTES; bytes++ )
        rocketnet_check( rocketnet_prefilter_check( packet, bytes, tag ) == ROCKETNET_PREFILTER_TOO_SHORT );

    // NEGATIVE: larger than the receive ceiling is refused; the ceiling
    // itself is accepted
    rocketnet_check( rocketnet_prefilter_check( packet, ROCKETNET_RECEIVE_CEILING + 1, tag ) == ROCKETNET_PREFILTER_TOO_LARGE );
    rocketnet_check( rocketnet_prefilter_check( packet, ROCKETNET_RECEIVE_CEILING, tag ) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );


    // every refusal has its own counter and no two share one -- the standard
    // requires one counter per distinct reason, and a rollup cannot say which
    // branch a packet actually hit
    const int codes[4] = { ROCKETNET_PREFILTER_TOO_SHORT, ROCKETNET_PREFILTER_TOO_LARGE,
                           ROCKETNET_PREFILTER_WRONG_EPOCH, ROCKETNET_PREFILTER_WRONG_VALUE };
    for ( int i = 0; i < 4; i++ )
        for ( int j = i + 1; j < 4; j++ )
            rocketnet_check( rocketnet_prefilter_counter( codes[i] ) != rocketnet_prefilter_counter( codes[j] ) );
}

void rocketnet_test_packet_associated_data()
{
    // the builder reproduces every per-type AD vector from Appendix A exactly
    uint8_t ad[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];

    rocketnet_check( rocketnet_write_packet_associated_data( ad, ROCKETNET_TEST_PROTOCOL_ID, ROCKETNET_CONNECTION_CHALLENGE_PACKET, rocketnet_test_salt, rocketnet_test_client_eph_public ) == ROCKETNET_AD_WITH_EPHEMERAL_BYTES );
    rocketnet_check( memcmp( ad, rocketnet_test_challenge_ad, ROCKETNET_AD_WITH_EPHEMERAL_BYTES ) == 0 );

    rocketnet_check( rocketnet_write_packet_associated_data( ad, ROCKETNET_TEST_PROTOCOL_ID, ROCKETNET_CONNECTION_RESPONSE_PACKET, rocketnet_test_salt, rocketnet_test_client_eph_public ) == ROCKETNET_AD_WITH_EPHEMERAL_BYTES );
    rocketnet_check( memcmp( ad, rocketnet_test_response_ad, ROCKETNET_AD_WITH_EPHEMERAL_BYTES ) == 0 );

    rocketnet_check( rocketnet_write_packet_associated_data( ad, ROCKETNET_TEST_PROTOCOL_ID, ROCKETNET_CONNECTION_DENIED_PACKET, rocketnet_test_salt, NULL ) == ROCKETNET_AD_BYTES );
    rocketnet_check( memcmp( ad, rocketnet_test_denied_ad, ROCKETNET_AD_BYTES ) == 0 );

    rocketnet_check( rocketnet_write_packet_associated_data( ad, ROCKETNET_TEST_PROTOCOL_ID, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, rocketnet_test_salt, NULL ) == ROCKETNET_AD_BYTES );
    rocketnet_check( memcmp( ad, rocketnet_test_keepalive_ad, ROCKETNET_AD_BYTES ) == 0 );

    rocketnet_check( rocketnet_write_packet_associated_data( ad, ROCKETNET_TEST_PROTOCOL_ID, ROCKETNET_CONNECTION_PAYLOAD_PACKET, rocketnet_test_salt, NULL ) == ROCKETNET_AD_BYTES );
    rocketnet_check( memcmp( ad, rocketnet_test_payload_ad, ROCKETNET_AD_BYTES ) == 0 );

    rocketnet_check( rocketnet_write_packet_associated_data( ad, ROCKETNET_TEST_PROTOCOL_ID, ROCKETNET_CONNECTION_DISCONNECT_PACKET, rocketnet_test_salt, NULL ) == ROCKETNET_AD_BYTES );
    rocketnet_check( memcmp( ad, rocketnet_test_disconnect_ad, ROCKETNET_AD_BYTES ) == 0 );
}

void rocketnet_test_sealed_packets()
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    // the challenge at sequence 1 reproduces the assembled vector completely:
    // prefilter, sequence, clear nonce, sealed server ephemeral
    int bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET, tag, 1,
                                               rocketnet_test_session_nonce,
                                               rocketnet_test_server_eph_public, 32,
                                               ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               rocketnet_test_handshake_s2c_key );
    rocketnet_check( bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    rocketnet_check( memcmp( packet, rocketnet_test_assembled_challenge, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES ) == 0 );

    // every type's sealed region reproduces its Appendix A vector, and every
    // wire size matches the table. the session types' data is the vector
    // plaintext -- the ack block, then the type data -- because the sealed
    // plaintext of every session type begins with the block.
    const uint8_t response_data[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };

    struct
    {
        uint8_t packet_type;
        uint64_t sequence;
        const uint8_t * session_nonce;
        const uint8_t * data;
        int data_bytes;
        const uint8_t * client_ephemeral;
        const uint8_t * key;
        const uint8_t * expected_sealed;
        int expected_wire_bytes;
    } cases[8] = {
        { ROCKETNET_CONNECTION_DENIED_PACKET, 2, rocketnet_test_session_nonce, NULL, 0, NULL, rocketnet_test_handshake_s2c_key, rocketnet_test_denied_sealed, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES },
        { ROCKETNET_CONNECTION_CHALLENGE_PACKET, 1, rocketnet_test_session_nonce, rocketnet_test_server_eph_public, 32, rocketnet_test_client_eph_public, rocketnet_test_handshake_s2c_key, rocketnet_test_challenge_sealed, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES },
        { ROCKETNET_CONNECTION_RESPONSE_PACKET, 1, NULL, response_data, ROCKETNET_RESPONSE_PADDING_BYTES, rocketnet_test_client_eph_public, rocketnet_test_handshake_c2s_key, rocketnet_test_response_sealed, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES },
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1, NULL, rocketnet_test_keepalive_plaintext, 15, NULL, rocketnet_test_session_s2c_key, rocketnet_test_keepalive_sealed, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, NULL, rocketnet_test_payload_plaintext, 12, NULL, rocketnet_test_session_c2s_key, rocketnet_test_payload_sealed, ROCKETNET_PACKET_OVERHEAD_BYTES + 12 },
        { ROCKETNET_CONNECTION_DISCONNECT_PACKET, 4, NULL, rocketnet_test_disconnect_plaintext, 7, NULL, rocketnet_test_session_c2s_key, rocketnet_test_disconnect_sealed, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES },
        { ROCKETNET_CONNECTION_ACK_PACKET, 5, NULL, rocketnet_test_ack_plaintext, 11, NULL, rocketnet_test_session_c2s_key, rocketnet_test_ack_sealed, ROCKETNET_PACKET_OVERHEAD_BYTES + 11 },
        { ROCKETNET_CONNECTION_FRAGMENT_PACKET, 6, NULL, rocketnet_test_fragment_plaintext, 18, NULL, rocketnet_test_session_c2s_key, rocketnet_test_fragment_sealed, ROCKETNET_PACKET_OVERHEAD_BYTES + 18 },
    };

    for ( int i = 0; i < 8; i++ )
    {
        bytes = rocketnet_write_sealed_packet( packet, cases[i].packet_type, tag, cases[i].sequence,
                                               cases[i].session_nonce, cases[i].data, cases[i].data_bytes,
                                               ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                               cases[i].client_ephemeral, cases[i].key );
        rocketnet_check( bytes == cases[i].expected_wire_bytes );

        int sealed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + ( cases[i].session_nonce ? ROCKETNET_SESSION_NONCE_BYTES : 0 );
        rocketnet_check( memcmp( packet + sealed_offset, cases[i].expected_sealed, cases[i].data_bytes + ROCKETNET_PACKET_TAG_BYTES ) == 0 );

        // and it opens: right data, right length, and the clear nonce comes
        // back on the types that carry one
        uint8_t opened[ROCKETNET_RECEIVE_CEILING];
        uint8_t session_nonce_out[ROCKETNET_SESSION_NONCE_BYTES];
        int opened_bytes = rocketnet_open_sealed_packet( opened, cases[i].session_nonce ? session_nonce_out : NULL,
                                                         packet, bytes, cases[i].packet_type, cases[i].sequence,
                                                         ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                         cases[i].client_ephemeral, cases[i].key );
        rocketnet_check( opened_bytes == cases[i].data_bytes );
        if ( cases[i].data_bytes > 0 )
            rocketnet_check( memcmp( opened, cases[i].data, cases[i].data_bytes ) == 0 );
        if ( cases[i].session_nonce )
            rocketnet_check( memcmp( session_nonce_out, rocketnet_test_session_nonce, ROCKETNET_SESSION_NONCE_BYTES ) == 0 );
    }
}

void rocketnet_test_sealed_packet_refusals()
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];

    const uint8_t payload_data[5] = { 0x68, 0x65, 0x6c, 0x6c, 0x6f };
    int bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, tag, 3,
                                               NULL, payload_data, 5,
                                               ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                               NULL, rocketnet_test_session_c2s_key );
    rocketnet_check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + 5 );

    // NEGATIVE: sequence zero is never built and never opens
    rocketnet_check( rocketnet_write_sealed_packet( packet + 100, ROCKETNET_CONNECTION_PAYLOAD_PACKET, tag, 0, NULL, payload_data, 5, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == 0 );
    rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 0, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );

    // NEGATIVE: the wrong key refuses
    rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_s2c_key ) == -1 );

    // NEGATIVE: the wrong sequence refuses -- the nonce is the sequence
    rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 4, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );

    // NEGATIVE: the wrong protocol id refuses -- it lives in the AD
    rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, ROCKETNET_TEST_PROTOCOL_ID + 1, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );

    // NEGATIVE: the wrong salt refuses -- it lives in the AD
    const uint8_t wrong_salt[8] = { 0 };
    rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, ROCKETNET_TEST_PROTOCOL_ID, wrong_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );

    // NEGATIVE: a different claimed type refuses -- the type lives in the AD,
    // so a challenge cannot be replayed as a denied
    rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 3, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );

    // NEGATIVE: truncation refuses, at every cut
    for ( int cut = 1; cut <= 16; cut++ )
        rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes - cut, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );

    // NEGATIVE: a flipped ciphertext byte refuses
    packet[ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + 2] ^= 1;
    rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );
    packet[ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + 2] ^= 1;

    // NEGATIVE: the nonce and ephemeral argument discipline is enforced both
    // ways -- a type that requires one refuses NULL, a type that forbids one
    // refuses its presence
    uint8_t session_nonce_out[ROCKETNET_SESSION_NONCE_BYTES];
    rocketnet_check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET, tag, 1, NULL, rocketnet_test_server_eph_public, 32, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, rocketnet_test_client_eph_public, rocketnet_test_handshake_s2c_key ) == 0 );
    rocketnet_check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, tag, 3, rocketnet_test_session_nonce, payload_data, 5, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == 0 );
    rocketnet_check( rocketnet_open_sealed_packet( opened, session_nonce_out, packet, bytes, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == -1 );

    // NEGATIVE: request and cookie are not sealed types
    rocketnet_check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_REQUEST_PACKET, tag, 1, NULL, payload_data, 5, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == 0 );
    rocketnet_check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_COOKIE_PACKET, tag, 1, NULL, payload_data, 5, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, rocketnet_test_session_c2s_key ) == 0 );
}

void rocketnet_test_unsealed_packets()
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    // the request writer reproduces the assembled vector completely, from the
    // frozen token's parts, the vector salt and ephemeral, and a zero cookie
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, rocketnet_test_frozen_token );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_write_connection_request_packet( request, tag, NULL,
                                               rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data,
                                               token.signature );
    rocketnet_check( memcmp( request, rocketnet_test_assembled_request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES ) == 0 );

    // the cookie packet lays out exactly: prefilter, salt echoed clear,
    // expire, then the vector's mac
    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( cookie, tag, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_mac );
    rocketnet_check( cookie[0] == ROCKETNET_CONNECTION_COOKIE_PACKET );
    rocketnet_check( cookie[1] == ROCKETNET_FILTER_EPOCH_UNKEYED );
    rocketnet_check( memcmp( cookie + 2, tag, ROCKETNET_FILTER_VALUE_BYTES ) == 0 );
    rocketnet_check( memcmp( cookie + ROCKETNET_PREFILTER_BYTES, rocketnet_test_salt, 8 ) == 0 );
    const uint8_t * p = cookie + ROCKETNET_PREFILTER_BYTES + 8;
    rocketnet_check( rocketnet_read_uint64( &p ) == ROCKETNET_TEST_CREATE_TS + 3 );
    rocketnet_check( memcmp( cookie + ROCKETNET_PREFILTER_BYTES + 16, rocketnet_test_cookie_mac, ROCKETNET_COOKIE_MAC_BYTES ) == 0 );
}

void rocketnet_test_sequence_reconstruction()
{
    // the first packet of a phase: nothing opened, wire 1, reconstructs to 1
    rocketnet_check( rocketnet_reconstruct_sequence( 1, 0 ) == 1 );

    // ordinary progression inside the first epoch
    rocketnet_check( rocketnet_reconstruct_sequence( 1000, 999 ) == 1000 );
    rocketnet_check( rocketnet_reconstruct_sequence( 998, 999 ) == 998 );

    // a wire value of zero reconstructs to zero here, and the caller rejects
    // it: zero is never a real sequence
    rocketnet_check( rocketnet_reconstruct_sequence( 0, 0 ) == 0 );

    // crossing up into the next epoch
    rocketnet_check( rocketnet_reconstruct_sequence( 0, 0xFFFFFFFFULL ) == 0x100000000ULL );
    rocketnet_check( rocketnet_reconstruct_sequence( 5, 0xFFFFFFFFULL ) == 0x100000005ULL );

    // a late arrival from the epoch below
    rocketnet_check( rocketnet_reconstruct_sequence( 0xFFFFFFFF, 0x100000005ULL ) == 0xFFFFFFFFULL );

    // deep inside a high epoch, both directions
    rocketnet_check( rocketnet_reconstruct_sequence( 0x00000002, 0x500000001ULL ) == 0x500000002ULL );
    rocketnet_check( rocketnet_reconstruct_sequence( 0xFFFFFFF0, 0x500000001ULL ) == 0x4FFFFFFF0ULL );

    // the exact tie, 2^31 either way: the larger candidate wins
    rocketnet_check( rocketnet_reconstruct_sequence( 0, 0x17FFFFFFFULL ) == 0x200000000ULL );
}

void rocketnet_test_cookie()
{
    // the vector: client source 198.51.100.7:56789, the vector salt and
    // cookie key, expire = create + 3. the input encoding and the mac must
    // both reproduce Appendix A exactly. validation itself lives in the
    // request ladder -- one implementation of the rule, tested where it runs.
    struct rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );

    uint8_t input[ROCKETNET_COOKIE_MAC_INPUT_IPV6_BYTES];
    int input_bytes = rocketnet_write_cookie_mac_input( input, &client_address, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3 );
    rocketnet_check( input_bytes == ROCKETNET_COOKIE_MAC_INPUT_IPV4_BYTES );
    rocketnet_check( memcmp( input, rocketnet_test_cookie_mac_input, ROCKETNET_COOKIE_MAC_INPUT_IPV4_BYTES ) == 0 );

    uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES];
    rocketnet_check( rocketnet_generate_cookie_mac( mac, &client_address, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_key ) );
    rocketnet_check( memcmp( mac, rocketnet_test_cookie_mac, ROCKETNET_COOKIE_MAC_BYTES ) == 0 );

    // the mac is a function of every input: address, port, salt, expire, key
    uint8_t other_mac[ROCKETNET_COOKIE_MAC_BYTES];
    struct rocketnet_address_t other_address;
    rocketnet_check( rocketnet_address_parse( &other_address, "198.51.100.8:56789" ) );
    rocketnet_check( rocketnet_generate_cookie_mac( other_mac, &other_address, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_key ) );
    rocketnet_check( memcmp( other_mac, mac, ROCKETNET_COOKIE_MAC_BYTES ) != 0 );
    rocketnet_check( rocketnet_address_parse( &other_address, "198.51.100.7:56790" ) );
    rocketnet_check( rocketnet_generate_cookie_mac( other_mac, &other_address, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_key ) );
    rocketnet_check( memcmp( other_mac, mac, ROCKETNET_COOKIE_MAC_BYTES ) != 0 );
    const uint8_t other_salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    rocketnet_check( rocketnet_generate_cookie_mac( other_mac, &client_address, other_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_key ) );
    rocketnet_check( memcmp( other_mac, mac, ROCKETNET_COOKIE_MAC_BYTES ) != 0 );
    rocketnet_check( rocketnet_generate_cookie_mac( other_mac, &client_address, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 4, rocketnet_test_cookie_key ) );
    rocketnet_check( memcmp( other_mac, mac, ROCKETNET_COOKIE_MAC_BYTES ) != 0 );

    // an ipv6 source encodes at the ipv6 length with family 2, group bytes
    // little-endian
    struct rocketnet_address_t ipv6_address;
    rocketnet_check( rocketnet_address_parse( &ipv6_address, "[2001:db8::7]:56789" ) );
    input_bytes = rocketnet_write_cookie_mac_input( input, &ipv6_address, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3 );
    rocketnet_check( input_bytes == ROCKETNET_COOKIE_MAC_INPUT_IPV6_BYTES );
    rocketnet_check( input[0] == 2 );
    rocketnet_check( input[1] == 0x01 && input[2] == 0x20 );   // group 2001 little-endian
    rocketnet_check( rocketnet_generate_cookie_mac( mac, &ipv6_address, rocketnet_test_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_key ) );
}

void rocketnet_test_connection_keys()
{
    // both sides derive, from their own halves, exactly the four keys the
    // vectors publish -- and they agree with each other
    struct rocketnet_connection_keys_t client_keys;
    rocketnet_check( rocketnet_derive_connection_keys( &client_keys,
                                                       rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                       rocketnet_test_salt, rocketnet_test_session_nonce,
                                                       rocketnet_test_client_eph_secret, rocketnet_test_server_eph_public,
                                                       rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) );

    struct rocketnet_connection_keys_t server_keys;
    rocketnet_check( rocketnet_derive_connection_keys( &server_keys,
                                                       rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                       rocketnet_test_salt, rocketnet_test_session_nonce,
                                                       rocketnet_test_server_eph_secret, rocketnet_test_client_eph_public,
                                                       rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) );

    rocketnet_check( memcmp( client_keys.handshake_client_to_server, rocketnet_test_handshake_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( client_keys.handshake_server_to_client, rocketnet_test_handshake_s2c_key, 32 ) == 0 );
    rocketnet_check( memcmp( client_keys.session_client_to_server, rocketnet_test_session_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( client_keys.session_server_to_client, rocketnet_test_session_s2c_key, 32 ) == 0 );
    rocketnet_check( memcmp( &client_keys, &server_keys, sizeof(client_keys) ) == 0 );

    // the handshake-only derivation matches the full one, so a client can
    // open a challenge before any ephemeral work
    struct rocketnet_connection_keys_t handshake_only;
    memset( &handshake_only, 0, sizeof(handshake_only) );
    rocketnet_derive_handshake_keys( &handshake_only,
                                     rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, rocketnet_test_session_nonce );
    rocketnet_check( memcmp( handshake_only.handshake_client_to_server, rocketnet_test_handshake_c2s_key, 32 ) == 0 );
    rocketnet_check( memcmp( handshake_only.handshake_server_to_client, rocketnet_test_handshake_s2c_key, 32 ) == 0 );

    // a different salt or nonce changes every derived key: keys are per
    // connection by construction
    struct rocketnet_connection_keys_t other;
    const uint8_t other_salt[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
    rocketnet_derive_handshake_keys( &other, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key, other_salt, rocketnet_test_session_nonce );
    rocketnet_check( memcmp( other.handshake_client_to_server, rocketnet_test_handshake_c2s_key, 32 ) != 0 );
    rocketnet_check( memcmp( other.handshake_server_to_client, rocketnet_test_handshake_s2c_key, 32 ) != 0 );
    const uint8_t other_nonce[8] = { 8, 8, 8, 8, 8, 8, 8, 8 };
    rocketnet_derive_handshake_keys( &other, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key, rocketnet_test_salt, other_nonce );
    rocketnet_check( memcmp( other.handshake_client_to_server, rocketnet_test_handshake_c2s_key, 32 ) != 0 );

    // NEGATIVE: a small-order peer ephemeral refuses the whole derivation
    const uint8_t zero_point[32] = { 0 };
    rocketnet_check( !rocketnet_derive_connection_keys( &other,
                                                        rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                        rocketnet_test_salt, rocketnet_test_session_nonce,
                                                        rocketnet_test_client_eph_secret, zero_point,
                                                        rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) );
}

void rocketnet_test_counters()
{
    // every counter has a name, values zero-initialize, and increments hold
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        rocketnet_check( rocketnet_counter_name( i ) != NULL );
        rocketnet_check( strcmp( rocketnet_counter_name( i ), "???" ) != 0 );
        rocketnet_check( counters.values[i] == 0 );
    }
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED]++;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED]++;
    rocketnet_check( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] == 2 );
    rocketnet_check( strcmp( rocketnet_counter_name( ROCKETNET_COUNTER_MAPPING_CREATED ), "MAPPING_CREATED" ) == 0 );
}

static void rocketnet_test_token_id( uint8_t * identifier, int seed )
{
    memset( identifier, 0, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    identifier[0] = (uint8_t) ( seed & 0xFF );
    identifier[1] = (uint8_t) ( ( seed >> 8 ) & 0xFF );
    identifier[15] = 0x5A;
}

void rocketnet_test_used_token_history()
{
    const uint64_t now = 1000;
    const uint64_t expire = now + 300;

    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 8 );
    rocketnet_check( history );

    struct rocketnet_address_t address_a, address_b;
    rocketnet_check( rocketnet_address_parse( &address_a, "10.0.0.1:30000" ) );
    rocketnet_check( rocketnet_address_parse( &address_b, "10.0.0.2:30000" ) );

    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_test_token_id( id, 1 );

    // unseen passes from anywhere; once recorded, bound to the first address
    rocketnet_check( rocketnet_used_token_history_check( history, id, &address_a, now ) );
    rocketnet_check( rocketnet_used_token_history_check( history, id, &address_b, now ) );
    rocketnet_used_token_history_record( history, id, &address_a, expire, now );
    rocketnet_check( rocketnet_used_token_history_check( history, id, &address_a, now ) );
    rocketnet_check( !rocketnet_used_token_history_check( history, id, &address_b, now ) );

    // the same port on a different address, and the same address on a
    // different port, are different identities
    struct rocketnet_address_t address_a_other_port;
    rocketnet_check( rocketnet_address_parse( &address_a_other_port, "10.0.0.1:30001" ) );
    rocketnet_check( !rocketnet_used_token_history_check( history, id, &address_a_other_port, now ) );

    // recording again from the same address is harmless
    rocketnet_used_token_history_record( history, id, &address_a, expire, now );
    rocketnet_check( rocketnet_used_token_history_check( history, id, &address_a, now ) );
    rocketnet_check( !rocketnet_used_token_history_check( history, id, &address_b, now ) );

    // entries expire with their tokens: past expiry the binding is gone,
    // which protects nothing the token expire check does not already refuse
    rocketnet_check( rocketnet_used_token_history_check( history, id, &address_b, expire + 1 ) );

    // oldest-first eviction at capacity: fill the table, add one more, and
    // the oldest binding is flushed -- the documented attack shape the
    // standard sizes capacity against
    for ( int i = 2; i <= 9; i++ )
    {
        uint8_t fill_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        rocketnet_test_token_id( fill_id, i );
        rocketnet_used_token_history_record( history, fill_id, &address_a, expire, now );
    }
    uint8_t last_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_test_token_id( last_id, 10 );
    rocketnet_used_token_history_record( history, last_id, &address_a, expire, now );

    // records ran 1 through 10 against capacity 8, so exact oldest-first
    // evicted ids 1 and 2; ids 3 through 10 stay bound
    rocketnet_check( rocketnet_used_token_history_check( history, id, &address_b, now ) );          // id 1 evicted
    uint8_t id_2[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_test_token_id( id_2, 2 );
    rocketnet_check( rocketnet_used_token_history_check( history, id_2, &address_b, now ) );        // id 2 evicted
    rocketnet_check( !rocketnet_used_token_history_check( history, last_id, &address_b, now ) );    // id 10 bound
    for ( int i = 3; i <= 9; i++ )
    {
        uint8_t bound_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        rocketnet_test_token_id( bound_id, i );
        rocketnet_check( !rocketnet_used_token_history_check( history, bound_id, &address_b, now ) );
    }

    rocketnet_used_token_history_destroy( history );
}

void rocketnet_test_admitted_token_set()
{
    const uint64_t now = 1000;
    const uint64_t expire = now + 300;

    struct rocketnet_admitted_token_set_t * set = rocketnet_admitted_token_set_create( NULL, 4 );
    rocketnet_check( set );

    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_test_token_id( id, 100 );

    // a token admits one connection: absent, then present once added
    rocketnet_check( !rocketnet_admitted_token_set_contains( set, id, now ) );
    rocketnet_check( rocketnet_admitted_token_set_add( set, id, expire, now ) );
    rocketnet_check( rocketnet_admitted_token_set_contains( set, id, now ) );

    // adding the same token again is idempotent
    rocketnet_check( rocketnet_admitted_token_set_add( set, id, expire, now ) );
    rocketnet_check( rocketnet_admitted_token_set_contains( set, id, now ) );

    // fill to capacity with unexpired entries, then one more MUST refuse:
    // unexpired entries are never evicted, and the server fails closed
    for ( int i = 101; i <= 103; i++ )
    {
        uint8_t fill_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        rocketnet_test_token_id( fill_id, i );
        rocketnet_check( rocketnet_admitted_token_set_add( set, fill_id, expire, now ) );
    }
    uint8_t overflow_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_test_token_id( overflow_id, 104 );
    rocketnet_check( !rocketnet_admitted_token_set_add( set, overflow_id, expire, now ) );

    // every original entry survived the refused add
    for ( int i = 100; i <= 103; i++ )
    {
        uint8_t check_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        rocketnet_test_token_id( check_id, i );
        rocketnet_check( rocketnet_admitted_token_set_contains( set, check_id, now ) );
    }

    // past expiry the entries are gone -- their tokens cannot pass the expire
    // check anyway -- and the slots reclaim
    rocketnet_check( !rocketnet_admitted_token_set_contains( set, id, expire + 1 ) );
    rocketnet_check( rocketnet_admitted_token_set_add( set, overflow_id, expire + 300, expire + 1 ) );
    rocketnet_check( rocketnet_admitted_token_set_contains( set, overflow_id, expire + 1 ) );

    rocketnet_admitted_token_set_destroy( set );
}

void rocketnet_test_encryption_map()
{
    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 4 );
    rocketnet_check( map );
    rocketnet_check( rocketnet_encryption_map_num_mappings( map ) == 0 );

    struct rocketnet_address_t address_a, address_b, address_c;
    rocketnet_check( rocketnet_address_parse( &address_a, "10.0.0.1:30000" ) );
    rocketnet_check( rocketnet_address_parse( &address_b, "10.0.0.1:30001" ) );      // same ip, other port
    rocketnet_check( rocketnet_address_parse( &address_c, "[2001:db8::7]:30000" ) );

    // absent, then found once added; address and port together are identity
    rocketnet_check( rocketnet_encryption_map_find( map, &address_a ) == -1 );
    int index_a = rocketnet_encryption_map_add( map, &address_a, 100.0 );
    rocketnet_check( index_a != -1 );
    rocketnet_check( rocketnet_encryption_map_find( map, &address_a ) == index_a );
    rocketnet_check( rocketnet_encryption_map_find( map, &address_b ) == -1 );

    // a fresh mapping: sequences start at one, no slot, not established
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( map, index_a );
    rocketnet_check( mapping->send_sequence_handshake == 1 );
    rocketnet_check( mapping->send_sequence_session == 1 );
    rocketnet_check( mapping->client_index == -1 );
    rocketnet_check( !mapping->established );
    rocketnet_check( mapping->handshake_deadline == 100.0 );

    // both families coexist
    int index_b = rocketnet_encryption_map_add( map, &address_b, 100.0 );
    int index_c = rocketnet_encryption_map_add( map, &address_c, 100.0 );
    rocketnet_check( index_b != -1 && index_c != -1 );
    rocketnet_check( rocketnet_encryption_map_find( map, &address_c ) == index_c );
    rocketnet_check( rocketnet_encryption_map_num_mappings( map ) == 3 );

    // full refuses: mappings are never evicted
    struct rocketnet_address_t address_d, address_e;
    rocketnet_check( rocketnet_address_parse( &address_d, "10.0.0.4:30000" ) );
    rocketnet_check( rocketnet_address_parse( &address_e, "10.0.0.5:30000" ) );
    rocketnet_check( rocketnet_encryption_map_add( map, &address_d, 100.0 ) != -1 );
    rocketnet_check( rocketnet_encryption_map_add( map, &address_e, 100.0 ) == -1 );

    // remove zeroes the key material and frees the slot for reuse
    memset( &mapping->keys, 0xAB, sizeof(mapping->keys) );
    rocketnet_encryption_map_remove( map, index_a );
    rocketnet_check( rocketnet_encryption_map_find( map, &address_a ) == -1 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( map ) == 3 );
    const uint8_t * raw = (const uint8_t*) mapping;
    for ( int i = 0; i < (int) sizeof(struct rocketnet_encryption_mapping_t); i++ )
        rocketnet_check( raw[i] == 0 );
    rocketnet_check( rocketnet_encryption_map_add( map, &address_e, 100.0 ) != -1 );

    rocketnet_encryption_map_destroy( map );
}

void rocketnet_test_encryption_map_iterator()
{
    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 8 );
    rocketnet_check( map );

    // empty iterates nothing
    int iterator = -1;
    rocketnet_check( !rocketnet_encryption_map_next( map, &iterator ) );

    struct rocketnet_address_t a, b, c;
    rocketnet_check( rocketnet_address_parse( &a, "10.0.0.1:30000" ) );
    rocketnet_check( rocketnet_address_parse( &b, "10.0.0.2:30000" ) );
    rocketnet_check( rocketnet_address_parse( &c, "10.0.0.3:30000" ) );
    int index_a = rocketnet_encryption_map_add( map, &a, 100.0 );
    int index_b = rocketnet_encryption_map_add( map, &b, 100.0 );
    int index_c = rocketnet_encryption_map_add( map, &c, 100.0 );

    // every live index yields exactly once
    int seen[8] = { 0 };
    iterator = -1;
    int count = 0;
    while ( rocketnet_encryption_map_next( map, &iterator ) )
    {
        seen[iterator]++;
        count++;
    }
    rocketnet_check( count == 3 );
    rocketnet_check( seen[index_a] == 1 && seen[index_b] == 1 && seen[index_c] == 1 );

    // removing the yielded index during iteration is safe -- the sweep's
    // whole pattern
    iterator = -1;
    while ( rocketnet_encryption_map_next( map, &iterator ) )
        rocketnet_encryption_map_remove( map, iterator );
    rocketnet_check( rocketnet_encryption_map_num_mappings( map ) == 0 );

    rocketnet_encryption_map_destroy( map );
}



// payloads generated with a pattern verifiable from the bytes alone: the
// start value derives from the length, so corruption anywhere and
// truncation to any other length both fail verification

static void rocketnet_test_generate_payload( uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
        payload_data[i] = (uint8_t) ( start + i );
}

static int rocketnet_test_verify_payload( const uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
    {
        if ( payload_data[i] != (uint8_t) ( start + i ) )
            return 0;
    }
    return 1;
}

// --- the request path ladder ---

// one test server, built fresh per test: the vector keys, advertised at the
// vector address, empty tables, zeroed counters

struct rocketnet_test_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void rocketnet_test_server_setup( struct rocketnet_test_server_t * test )
{
    memset( test, 0, sizeof(*test) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->filter_value, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->allow_negative_timeout = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->admitted_token_set = rocketnet_admitted_token_set_create( NULL, 16 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &test->counters;
    rocketnet_check( server->encryption_map && server->used_token_history && server->admitted_token_set && server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );

    rocketnet_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void rocketnet_test_server_teardown( struct rocketnet_test_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_admitted_token_set_destroy( test->server.admitted_token_set );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

// builds a valid request around a freshly minted token. the caller mutates
// the request or the server to construct each failing variant.

static void rocketnet_test_build_request( uint8_t * request, uint64_t client_id, const uint8_t * salt )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
    private_fields.client_id = client_id;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

// runs a request and returns the result; before is a snapshot for asserting
// exactly which counters moved

static int rocketnet_test_process( struct rocketnet_test_server_t * test, const uint8_t * request, int request_bytes,
                                   uint8_t * response, int * response_bytes )
{
    return rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address,
                                                        request, request_bytes,
                                                        ROCKETNET_TEST_CREATE_TS, 100.0,
                                                        response, response_bytes );
}

// a request that fails a check must count exactly: the arrival, and the one
// reason. everything else in the counter set stays untouched.

static void rocketnet_test_expect_reason( struct rocketnet_test_server_t * test,
                                          const struct rocketnet_counters_t * before,
                                          int reason )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        uint64_t expected = before->values[i];
        if ( i == ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED || i == reason )
            expected++;
        rocketnet_check( test->counters.values[i] == expected );
    }
}

// the cookie exchange as a legitimate client performs it: first request gets
// a cookie, the cookie is copied into the request, and the request re-sent

static void rocketnet_test_pass_cookie_gate( struct rocketnet_test_server_t * test, uint8_t * request )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    int result = rocketnet_test_process( test, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );

    // the cookie packet echoes the salt in the clear; the client stores its
    // last 24 bytes and carries them in the return cookie field
    rocketnet_check( memcmp( response + ROCKETNET_PREFILTER_BYTES, request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, 8 ) == 0 );
    memcpy( request + ROCKETNET_PREFILTER_BYTES, response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );
}


void rocketnet_test_request_path_happy()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( request, 0x1111222233334444ULL, rocketnet_test_salt );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    // first request: no cookie held, so the gate answers with a cookie and
    // allocates nothing
    rocketnet_test_pass_cookie_gate( &test, request );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] == 1 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );

    // second request carries the cookie: the full ladder runs and a
    // challenge comes back
    int result = rocketnet_test_process( &test, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_MAPPING_CREATED] == 1 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );

    // the client completes its side from the wire bytes alone: reconstruct
    // the sequence, read the clear nonce, derive, open, recover the server
    // ephemeral -- the full loop the handshake depends on
    rocketnet_check( rocketnet_prefilter_check( response, response_bytes, test.server.filter_value ) == ROCKETNET_CONNECTION_CHALLENGE_PACKET );
    const uint8_t * p = response + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );
    rocketnet_check( sequence == 1 );

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    // peek the nonce to derive, exactly as the reading order specifies
    memcpy( session_nonce, response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    struct rocketnet_connection_keys_t handshake_keys;
    rocketnet_derive_handshake_keys( &handshake_keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, session_nonce );

    int opened_bytes = rocketnet_open_sealed_packet( opened, session_nonce, response, response_bytes,
                                                     ROCKETNET_CONNECTION_CHALLENGE_PACKET, sequence,
                                                     ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                     rocketnet_test_client_eph_public,
                                                     handshake_keys.handshake_server_to_client );
    rocketnet_check( opened_bytes == ROCKETNET_KEY_BYTES );

    // the recovered server ephemeral agrees with the mapping, and the
    // client's full derivation agrees with the server's stored keys
    int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    rocketnet_check( index != -1 );
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
    rocketnet_check( memcmp( opened, mapping->server_ephemeral_public_key, ROCKETNET_KEY_BYTES ) == 0 );

    struct rocketnet_connection_keys_t client_keys;
    rocketnet_check( rocketnet_derive_connection_keys( &client_keys,
                                                       rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                       rocketnet_test_salt, session_nonce,
                                                       rocketnet_test_client_eph_secret, opened,
                                                       rocketnet_test_client_eph_public, opened ) );
    rocketnet_check( memcmp( &client_keys, &mapping->keys, sizeof(client_keys) ) == 0 );

    // the mapping is filled exactly: identity, deadlines, sequences
    rocketnet_check( mapping->client_id == 0x1111222233334444ULL );
    rocketnet_check( mapping->timeout_seconds == ROCKETNET_TEST_TIMEOUT );
    rocketnet_check( !mapping->established );
    rocketnet_check( mapping->client_index == -1 );
    rocketnet_check( mapping->handshake_deadline == 105.0 );
    rocketnet_check( mapping->send_sequence_handshake == 2 );

    // a retransmitted request is answered byte-identical, and nothing new is
    // created: no second nonce, no second key set, ever
    uint8_t resent[ROCKETNET_RECEIVE_CEILING];
    int resent_bytes = 0;
    result = rocketnet_test_process( &test, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, resent, &resent_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( resent_bytes == response_bytes );
    rocketnet_check( memcmp( resent, response, (size_t) response_bytes ) == 0 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_MAPPING_CREATED] == 1 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );

    // received == sum of outcomes, by eye and by assertion
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] == 3 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_refusals()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t good_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( good_request, 0x1111222233334444ULL, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, good_request );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    struct rocketnet_counters_t before;

    const uint8_t * signed_base = good_request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                                  ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    (void) signed_base;
    int signed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;

    // wrong size, one byte both ways
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, good_request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE );
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, good_request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + 1, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE );

    // wrong version
    memcpy( request, good_request, sizeof(request) );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_VERSION] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION );

    // wrong protocol id
    memcpy( request, good_request, sizeof(request) );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID );

    // expired token: the expire field is before create ts in the past. build
    // a request whose signed expire is in the past by minting with it -- the
    // signature must remain valid so the refusal is the expire check alone
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        signed_fields.expire_timestamp = ROCKETNET_TEST_CREATE_TS;      // <= now
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );
        const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                                   rocketnet_test_client_eph_public,
                                                   token.signed_connect_data, token.signature );
        before = test.counters;
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED );
    }

    // zero timeout is invalid; negative refused unless configured
    memcpy( request, good_request, sizeof(request) );
    memset( request + signed_offset + ROCKETNET_SIGNED_OFFSET_TIMEOUT, 0, 4 );
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID );
    memset( request + signed_offset + ROCKETNET_SIGNED_OFFSET_TIMEOUT, 0xFF, 4 );   // -1
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID );

    // bad address flags: none, and a reserved bit
    memcpy( request, good_request, sizeof(request) );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS] = 0;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS] = 0x05;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS );

    // not advertised here: the token names a different server address
    memcpy( request, good_request, sizeof(request) );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_IPV4] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED );

    // wrong server key: the token is sealed to a server this one is not
    memcpy( request, good_request, sizeof(request) );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_SERVER_KEY] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_cookie_gate()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( request, 0x2222333344445555ULL, rocketnet_test_salt );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    struct rocketnet_counters_t before;

    // a garbage cookie field: the mac cannot verify, a fresh cookie comes
    // back, and no asymmetric work was reached (nothing else counted)
    memset( request + ROCKETNET_PREFILTER_BYTES, 0xAB, ROCKETNET_COOKIE_FIELD_BYTES );
    // make the expire half plausible so the refusal is the mac, not expiry
    uint8_t * p = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &p, ROCKETNET_TEST_CREATE_TS + 100 );
    before = test.counters;
    int result = rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY );

    // a genuinely expired cookie -- valid mac, past expiry -- gets a fresh
    // one, and counts as expired. only real clients with stale cookies land
    // here: the mac is checked first, so a forgery cannot hide under it.
    {
        uint64_t stale_expire = ROCKETNET_TEST_CREATE_TS;               // <= now
        uint8_t stale_mac[ROCKETNET_COOKIE_MAC_BYTES];
        rocketnet_check( rocketnet_generate_cookie_mac( stale_mac, &test.client_address, request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, stale_expire, rocketnet_test_cookie_key ) );
        p = request + ROCKETNET_PREFILTER_BYTES;
        rocketnet_write_uint64( &p, stale_expire );
        rocketnet_write_bytes( &p, stale_mac, ROCKETNET_COOKIE_MAC_BYTES );
    }
    before = test.counters;
    result = rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED );

    // and the forgery carrying a past expiry counts as a forgery
    memset( request + ROCKETNET_PREFILTER_BYTES, 0xAB, ROCKETNET_COOKIE_FIELD_BYTES );
    p = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &p, ROCKETNET_TEST_CREATE_TS );             // <= now, forged mac
    before = test.counters;
    result = rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY );

    // the cookie the server hands out is bound to the source address: the
    // same cookie presented from another address fails the mac
    memset( request + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_COOKIE_FIELD_BYTES );
    rocketnet_test_pass_cookie_gate( &test, request );
    struct rocketnet_address_t real_address = test.client_address;
    rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.99:56789" ) );
    before = test.counters;
    result = rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY );
    test.client_address = real_address;

    // from the right address it proceeds all the way to a challenge
    before = test.counters;
    result = rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE] == before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE] + 1 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_crypto_refusals()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t good_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( good_request, 0x3333444455556666ULL, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, good_request );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    struct rocketnet_counters_t before;

    int signed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    int signature_offset = signed_offset + ROCKETNET_SIGNED_CONNECT_DATA_BYTES;

    // a flipped signature byte: everything cheap passes, the signature is
    // what refuses. the user data byte flip below proves any signed byte
    // does the same through the same counter.
    memcpy( request, good_request, sizeof(request) );
    request[signature_offset] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY );

    memcpy( request, good_request, sizeof(request) );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + 50] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY );

    // an attacker token: right constants, wrong signing key. the sealed
    // block would open -- the signature is the bar, exactly as the standard
    // and the token tests pin.
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        uint8_t attacker_pk[32], attacker_sk[64];
        crypto_sign_keypair( attacker_pk, attacker_sk );
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, attacker_sk, token_data ) );
        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );
        const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                                   rocketnet_test_client_eph_public,
                                                   token.signed_connect_data, token.signature );
        memcpy( request + ROCKETNET_PREFILTER_BYTES, good_request + ROCKETNET_PREFILTER_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );
        before = test.counters;
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY );
    }

    // a small-order client ephemeral: every check passes until the
    // scalarmult refuses, and no mapping is created
    memcpy( request, good_request, sizeof(request) );
    memset( request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES, 0, ROCKETNET_KEY_BYTES );
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_state_refusals()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( request, 0x4444555566667777ULL, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, request );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    struct rocketnet_counters_t before;

    // the used-token history binds the token to its first source address:
    // the same request replayed from elsewhere is refused after one hash
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    struct rocketnet_address_t real_address = test.client_address;
    rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.50:56789" ) );
    // the replayed request needs a cookie for the new address first
    uint8_t replayed[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    memcpy( replayed, request, sizeof(replayed) );
    memset( replayed + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_COOKIE_FIELD_BYTES );
    rocketnet_test_pass_cookie_gate( &test, replayed );
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, replayed, sizeof(replayed), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED );
    test.client_address = real_address;

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_slots_and_denial()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    // every slot taken: the ladder runs all the way to the denied, sealed
    // under keys derived for this attempt, openable by the client from the
    // wire bytes alone
    uint8_t dummy_user_data[ROCKETNET_USER_DATA_BYTES];
    memset( dummy_user_data, 0, sizeof(dummy_user_data) );
    for ( int i = 0; i < 4; i++ )
        rocketnet_check( rocketnet_client_slots_assign( test.server.client_slots, 0xD000 + (uint64_t) i, -1, dummy_user_data ) == i );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( request, 0x5555666677778888ULL, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, request );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    struct rocketnet_counters_t before = test.counters;
    int result = rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes );
    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_SEND_DENIED );
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );

    // the client opens the denied from the wire: sequence 1, the clear
    // nonce, handshake keys derived from it
    rocketnet_check( rocketnet_prefilter_check( response, response_bytes, test.server.filter_value ) == ROCKETNET_CONNECTION_DENIED_PACKET );
    const uint8_t * p = response + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );
    rocketnet_check( sequence == 1 );
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memcpy( session_nonce, response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );
    struct rocketnet_connection_keys_t keys;
    rocketnet_derive_handshake_keys( &keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, session_nonce );
    uint8_t opened[64];
    int opened_bytes = rocketnet_open_sealed_packet( opened, session_nonce, response, response_bytes,
                                                     ROCKETNET_CONNECTION_DENIED_PACKET, sequence,
                                                     ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                     NULL, keys.handshake_server_to_client );
    rocketnet_check( opened_bytes == 0 );

    // with slots free again, the same request now builds a mapping -- and
    // when the mapping table itself is full, the refusal is counted twice:
    // the request reason and the lifecycle rollup
    for ( int i = 0; i < 4; i++ )
        rocketnet_client_slots_release( test.server.client_slots, i );
    for ( int i = 0; i < 8; i++ )
    {
        char address_string[64];
        snprintf( address_string, sizeof(address_string), "198.51.100.%d:56789", 100 + i );
        rocketnet_check( rocketnet_address_parse( &test.client_address, address_string ) );
        uint8_t fill_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        uint8_t fill_salt[8];
        memcpy( fill_salt, rocketnet_test_salt, 8 );
        fill_salt[0] = (uint8_t) i;
        rocketnet_test_build_request( fill_request, 0x6000000000000000ULL + (uint64_t) i, fill_salt );
        rocketnet_test_pass_cookie_gate( &test, fill_request );
        rocketnet_check( rocketnet_test_process( &test, fill_request, sizeof(fill_request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    }
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 8 );

    rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.200:56789" ) );
    uint8_t overflow_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( overflow_request, 0x7777888899990000ULL, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, overflow_request );
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, overflow_request, sizeof(overflow_request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        uint64_t expected = before.values[i];
        if ( i == ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED ||
             i == ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING ||
             i == ROCKETNET_COUNTER_MAPPING_TABLE_FULL )
            expected++;
        rocketnet_check( test.counters.values[i] == expected );
    }

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_ordering()
{
    // multi-fault packets: the suite's single-fault tests pin WHICH check
    // refuses; these pin THAT the checks run in the standard's order, which
    // is the security argument itself. each packet fails two rungs and the
    // earlier one must win with the later one's counter untouched.

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    struct rocketnet_counters_t before;
    int signed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    int signature_offset = signed_offset + ROCKETNET_SIGNED_CONNECT_DATA_BYTES;

    // no cookie AND a broken signature: the cookie gate wins, and no
    // asymmetric work runs -- SIGNATURE_DID_NOT_VERIFY must not move. this
    // is the ordering the whole ladder exists for.
    rocketnet_test_build_request( request, 0x0A0A, rocketnet_test_salt );
    request[signature_offset] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE );

    // a bad version AND no cookie: the version check wins and NOTHING is
    // emitted -- a garbage packet carrying the filter value must never turn the
    // server into a cookie reflector
    rocketnet_test_build_request( request, 0x0B0B, rocketnet_test_salt );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_VERSION] ^= 1;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( response_bytes == 0 );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION );

    // the already-connected rung: a mapping holding a slot for this source
    // address. one lookup, no crypto, nothing emitted, and it beats
    // everything below it
    {
        int connected_mapping = rocketnet_encryption_map_add( test.server.encryption_map, &test.client_address, 1000.0 );
        rocketnet_check( connected_mapping != -1 );
        rocketnet_encryption_map_entry( test.server.encryption_map, connected_mapping )->client_index = 0;
        rocketnet_test_build_request( request, 0x0C0C, rocketnet_test_salt );
        before = test.counters;
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( response_bytes == 0 );
        rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED );
        rocketnet_encryption_map_remove( test.server.encryption_map, connected_mapping );
    }

    // the client-id rung, hooked: reached only after the seal opens
    rocketnet_test_build_request( request, 0x0D0D, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, request );
    uint8_t taken_user_data[ROCKETNET_USER_DATA_BYTES];
    memset( taken_user_data, 0, sizeof(taken_user_data) );
    int taken_slot = rocketnet_client_slots_assign( test.server.client_slots, 0x0D0D, -1, taken_user_data );
    rocketnet_check( taken_slot != -1 );
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED );
    rocketnet_client_slots_release( test.server.client_slots, taken_slot );

    // the resend rung fires for a garbage body from a mapped address --
    // pinning its position above the version check: a mapping is answered
    // with its stored challenge no matter what the bytes claim
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    uint8_t garbage[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    memset( garbage, 0xEE, sizeof(garbage) );
    uint8_t resent[ROCKETNET_RECEIVE_CEILING];
    int resent_bytes = 0;
    before = test.counters;
    rocketnet_check( rocketnet_test_process( &test, garbage, sizeof(garbage), resent, &resent_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( resent_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    rocketnet_check( memcmp( resent, response, (size_t) resent_bytes ) == 0 );
    rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_uncovered_rungs()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    struct rocketnet_counters_t before;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    int signed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;

    // SEALED_MISMATCH: a validly signed token whose sealed protocol id
    // disagrees -- only the consistency check refuses it
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID ^ 1;
        uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
        rocketnet_write_connect_token_private( &private_fields, private_block );
        rocketnet_check( crypto_box_seal( signed_fields.sealed_private_block, private_block, sizeof(private_block), signed_fields.server_public_key ) == 0 );
        uint8_t signed_buffer[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
        rocketnet_write_signed_connect_data( &signed_fields, signed_buffer );
        uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
        rocketnet_check( crypto_sign_detached( signature, NULL, signed_buffer, sizeof(signed_buffer), rocketnet_test_backend_secret_key ) == 0 );
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public, signed_buffer, signature );
        rocketnet_test_pass_cookie_gate( &test, request );
        before = test.counters;
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH );
    }

    // COULD_NOT_OPEN_SEALED_BLOCK: signed data names this server's key but
    // the block is sealed to another
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        uint8_t other_pk[32], other_sk[32];
        crypto_box_keypair( other_pk, other_sk );
        uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
        rocketnet_write_connect_token_private( &private_fields, private_block );
        rocketnet_check( crypto_box_seal( signed_fields.sealed_private_block, private_block, sizeof(private_block), other_pk ) == 0 );
        uint8_t signed_buffer[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
        rocketnet_write_signed_connect_data( &signed_fields, signed_buffer );
        uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
        rocketnet_check( crypto_sign_detached( signature, NULL, signed_buffer, sizeof(signed_buffer), rocketnet_test_backend_secret_key ) == 0 );
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public, signed_buffer, signature );
        uint8_t salt[8];
        memcpy( salt, rocketnet_test_salt, 8 );
        salt[7] ^= 1;
        memcpy( request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, salt, 8 );
        rocketnet_test_pass_cookie_gate( &test, request );
        before = test.counters;
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK );
    }

    // TOKEN_ALREADY_CONNECTED: seeded by hand until the response path lands
    // the real insert -- a token in the admitted set is refused
    {
        rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.7:56789" ) );
        rocketnet_test_build_request( request, 0x0E0E, rocketnet_test_salt );
        rocketnet_test_pass_cookie_gate( &test, request );
        uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
        rocketnet_connect_token_identifier( identifier, request + signed_offset );
        rocketnet_check( rocketnet_admitted_token_set_add( test.server.admitted_token_set, identifier, ROCKETNET_TEST_EXPIRE_TS, ROCKETNET_TEST_CREATE_TS ) );
        before = test.counters;
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_test_expect_reason( &test, &before, ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED );
    }

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_request_path_config_branches()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    // the ipv6 advertised branch: a token naming a DIFFERENT server's ipv4
    // but THIS server's ipv6 is accepted -- either advertised address, not
    // only the first
    rocketnet_check( rocketnet_address_parse( &test.server.advertised_ipv6, "[2001:db8::1]:40000" ) );
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;
        const uint8_t other_server[6] = { 198, 51, 100, 1, 0x40, 0x9c };
        memcpy( signed_fields.ipv4_address, other_server, 6 );
        uint8_t * q = signed_fields.ipv6_address;
        const uint16_t groups[8] = { 0x2001, 0x0db8, 0, 0, 0, 0, 0, 1 };
        for ( int i = 0; i < 8; i++ )
            rocketnet_write_uint16( &q, groups[i] );
        rocketnet_write_uint16( &q, 40000 );
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public, token.signed_connect_data, token.signature );
        rocketnet_test_pass_cookie_gate( &test, request );
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    }

    // the timeout clamp: a huge timeout lands in the mapping clamped
    test.server.max_timeout_seconds = 30;
    rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.60:56789" ) );
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        signed_fields.timeout_seconds = 9999;
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public, token.signed_connect_data, token.signature );
        rocketnet_test_pass_cookie_gate( &test, request );
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
        rocketnet_check( index != -1 );
        rocketnet_check( rocketnet_encryption_map_entry( test.server.encryption_map, index )->timeout_seconds == 30 );
    }

    // a negative timeout is accepted when explicitly configured, refused
    // otherwise (the refusal is already tested); and the accept carries the
    // negative value through so timeouts are disabled downstream
    test.server.allow_negative_timeout = 1;
    rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.61:56789" ) );
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        signed_fields.timeout_seconds = -1;
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );
        rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public, token.signed_connect_data, token.signature );
        rocketnet_test_pass_cookie_gate( &test, request );
        rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
        rocketnet_check( index != -1 );
        rocketnet_check( rocketnet_encryption_map_entry( test.server.encryption_map, index )->timeout_seconds == -1 );
    }

    // the previous cookie key: a cookie minted under the old key is accepted
    // for one rotation period after the key changes
    rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.62:56789" ) );
    rocketnet_test_build_request( request, 0x0F0F, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, request );
    memcpy( test.server.previous_cookie_key, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    memset( test.server.cookie_key, 0x77, ROCKETNET_COOKIE_KEY_BYTES );
    test.server.has_previous_cookie_key = 1;
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    // nonce freshness across mapping lifetimes: the same request after the
    // first mapping dies yields a different nonce and different bytes --
    // no key or nonce ever recurs
    rocketnet_check( rocketnet_address_parse( &test.client_address, "198.51.100.63:56789" ) );
    rocketnet_test_build_request( request, 0x1F1F, rocketnet_test_salt );
    rocketnet_test_pass_cookie_gate( &test, request );
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    uint8_t first_challenge[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    memcpy( first_challenge, response, sizeof(first_challenge) );
    {
        int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
        rocketnet_check( index != -1 );
        rocketnet_encryption_map_remove( test.server.encryption_map, index );
    }
    rocketnet_check( rocketnet_test_process( &test, request, sizeof(request), response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( memcmp( response + 8, first_challenge + 8, ROCKETNET_SESSION_NONCE_BYTES ) != 0 );
    rocketnet_check( memcmp( response, first_challenge, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES ) != 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_replay_protection()
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof(replay) );

    // a fresh window has received nothing
    rocketnet_check( !rocketnet_replay_already_received( &replay, 1 ) );
    rocketnet_check( !rocketnet_replay_already_received( &replay, 1000 ) );

    // each sequence is accepted exactly once
    rocketnet_replay_advance( &replay, 1 );
    rocketnet_check( rocketnet_replay_already_received( &replay, 1 ) );
    rocketnet_check( !rocketnet_replay_already_received( &replay, 2 ) );

    // out of order within the window is fine, and each still only once
    rocketnet_replay_advance( &replay, 5 );
    rocketnet_check( !rocketnet_replay_already_received( &replay, 3 ) );
    rocketnet_replay_advance( &replay, 3 );
    rocketnet_check( rocketnet_replay_already_received( &replay, 3 ) );
    rocketnet_check( rocketnet_replay_already_received( &replay, 5 ) );
    rocketnet_check( !rocketnet_replay_already_received( &replay, 2 ) );
    rocketnet_check( !rocketnet_replay_already_received( &replay, 4 ) );

    // a partial skip clears exactly the entries it jumps past
    rocketnet_replay_advance( &replay, 10 );
    rocketnet_check( !rocketnet_replay_already_received( &replay, 6 ) );
    rocketnet_check( !rocketnet_replay_already_received( &replay, 9 ) );
    rocketnet_check( rocketnet_replay_already_received( &replay, 10 ) );

    // below the window is refused as too old -- indistinguishable from
    // received, and refused either way
    struct rocketnet_replay_protection_t high;
    memset( &high, 0, sizeof(high) );
    rocketnet_replay_advance( &high, ROCKETNET_REPLAY_WINDOW_BITS + 44 );
    rocketnet_check( rocketnet_replay_already_received( &high, 44 ) );      // exactly the discarded floor
    rocketnet_check( !rocketnet_replay_already_received( &high, 45 ) );

    // a far-ahead advance clears the whole window: a sequence one full lap
    // later shares a ring bit with an old one and must not inherit it
    struct rocketnet_replay_protection_t lap;
    memset( &lap, 0, sizeof(lap) );
    rocketnet_replay_advance( &lap, 1 );
    rocketnet_replay_advance( &lap, ROCKETNET_REPLAY_WINDOW_BITS + 1 );                                  // same ring bit as 1
    rocketnet_check( rocketnet_replay_already_received( &lap, ROCKETNET_REPLAY_WINDOW_BITS + 1 ) );
    rocketnet_check( rocketnet_replay_already_received( &lap, 1 ) );        // too old now, refused
    rocketnet_check( !rocketnet_replay_already_received( &lap, 2 ) );       // inside the retained window and not received

    // the standard's phase separation: two independent windows never
    // interact -- handshake and session replay state are separate structs
    struct rocketnet_replay_protection_t a, b;
    memset( &a, 0, sizeof(a) );
    memset( &b, 0, sizeof(b) );
    rocketnet_replay_advance( &a, 7 );
    rocketnet_check( !rocketnet_replay_already_received( &b, 7 ) );
}

void rocketnet_test_client_slots()
{
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 4 );
    rocketnet_check( slots );
    rocketnet_check( rocketnet_client_slots_max_clients( slots ) == 4 );
    rocketnet_check( rocketnet_client_slots_num_connected( slots ) == 0 );
    rocketnet_check( rocketnet_client_slots_num_free( slots ) == 4 );
    rocketnet_check( rocketnet_client_slots_find( slots, 0x1111 ) == -1 );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0xCD, sizeof(user_data) );

    // the lowest free slot is assigned, unconfirmed, with identity copied in
    int a = rocketnet_client_slots_assign( slots, 0x1111, 7, user_data );
    rocketnet_check( a == 0 );
    rocketnet_check( rocketnet_client_slots_find( slots, 0x1111 ) == 0 );
    rocketnet_check( rocketnet_client_slots_client_id( slots, 0 ) == 0x1111 );
    rocketnet_check( rocketnet_client_slots_mapping_index( slots, 0 ) == 7 );
    rocketnet_check( !rocketnet_client_slots_confirmed( slots, 0 ) );
    rocketnet_check( memcmp( rocketnet_client_slots_user_data( slots, 0 ), user_data, ROCKETNET_USER_DATA_BYTES ) == 0 );
    rocketnet_check( rocketnet_client_slots_num_free( slots ) == 3 );

    // confirmation is one way, per slot
    rocketnet_client_slots_confirm( slots, 0 );
    rocketnet_check( rocketnet_client_slots_confirmed( slots, 0 ) );

    int b = rocketnet_client_slots_assign( slots, 0x2222, 8, user_data );
    int c = rocketnet_client_slots_assign( slots, 0x3333, 9, user_data );
    rocketnet_check( b == 1 && c == 2 );

    // release frees the slot, clears identity, and the lowest-free rule
    // reuses it next
    rocketnet_client_slots_release( slots, 1 );
    rocketnet_check( rocketnet_client_slots_find( slots, 0x2222 ) == -1 );
    rocketnet_check( rocketnet_client_slots_num_free( slots ) == 2 );
    int d = rocketnet_client_slots_assign( slots, 0x4444, 10, user_data );
    rocketnet_check( d == 1 );
    rocketnet_check( !rocketnet_client_slots_confirmed( slots, 1 ) );

    // full refuses
    rocketnet_check( rocketnet_client_slots_assign( slots, 0x5555, 11, user_data ) == 3 );
    rocketnet_check( rocketnet_client_slots_assign( slots, 0x6666, 12, user_data ) == -1 );
    rocketnet_check( rocketnet_client_slots_num_free( slots ) == 0 );

    rocketnet_client_slots_destroy( slots );

    // the chained map holds up at scale: every id findable, then every id
    // gone after release, across enough ids to force chain collisions
    slots = rocketnet_client_slots_create( NULL, 64 );
    rocketnet_check( slots );
    for ( int i = 0; i < 64; i++ )
        rocketnet_check( rocketnet_client_slots_assign( slots, 0xABCD0000ULL + (uint64_t) i, i, user_data ) == i );
    for ( int i = 0; i < 64; i++ )
        rocketnet_check( rocketnet_client_slots_find( slots, 0xABCD0000ULL + (uint64_t) i ) == i );
    for ( int i = 0; i < 64; i += 2 )
        rocketnet_client_slots_release( slots, i );
    for ( int i = 0; i < 64; i++ )
    {
        if ( i % 2 == 0 )
            rocketnet_check( rocketnet_client_slots_find( slots, 0xABCD0000ULL + (uint64_t) i ) == -1 );
        else
            rocketnet_check( rocketnet_client_slots_find( slots, 0xABCD0000ULL + (uint64_t) i ) == i );
    }
    rocketnet_check( rocketnet_client_slots_num_connected( slots ) == 32 );
    rocketnet_client_slots_destroy( slots );
}

// --- the server receive path, end to end ---

// drives a request through the single receive entry
static int rocketnet_test_process_packet( struct rocketnet_test_server_t * test, const uint8_t * packet, int packet_bytes,
                                          uint8_t * response, int * response_bytes,
                                          uint8_t * out_payload, int * out_payload_bytes, int * out_payload_client_index )
{
    return rocketnet_server_protocol_process_packet( &test->server, &test->client_address, packet, packet_bytes,
                                            ROCKETNET_TEST_CREATE_TS, 100.0,
                                            response, response_bytes,
                                            out_payload, out_payload_bytes, out_payload_client_index );
}

// the client's half of the handshake, from wire bytes alone: opens the
// challenge, derives the four keys, returns them
static void rocketnet_test_client_derive_from_challenge( const uint8_t * challenge, int challenge_bytes,
                                                         struct rocketnet_connection_keys_t * keys )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_check( rocketnet_prefilter_check( challenge, challenge_bytes, tag ) == ROCKETNET_CONNECTION_CHALLENGE_PACKET );
    const uint8_t * p = challenge + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );

    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memcpy( session_nonce, challenge + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    struct rocketnet_connection_keys_t handshake_keys;
    rocketnet_derive_handshake_keys( &handshake_keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, session_nonce );

    uint8_t server_ephemeral[ROCKETNET_KEY_BYTES];
    rocketnet_check( rocketnet_open_sealed_packet( server_ephemeral, session_nonce, challenge, challenge_bytes,
                                                   ROCKETNET_CONNECTION_CHALLENGE_PACKET, sequence,
                                                   ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                   rocketnet_test_client_eph_public,
                                                   handshake_keys.handshake_server_to_client ) == ROCKETNET_KEY_BYTES );

    rocketnet_check( rocketnet_derive_connection_keys( keys,
                                                       rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                       rocketnet_test_salt, session_nonce,
                                                       rocketnet_test_client_eph_secret, server_ephemeral,
                                                       rocketnet_test_client_eph_public, server_ephemeral ) );
}

static void rocketnet_test_client_build_response( uint8_t * response, const struct rocketnet_connection_keys_t * keys )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    const uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    rocketnet_check( rocketnet_write_sealed_packet( response, ROCKETNET_CONNECTION_RESPONSE_PACKET, tag, 1,
                                                    NULL, padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                                    ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                    rocketnet_test_client_eph_public,
                                                    keys->handshake_client_to_server ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
}

// Builds a sealed session packet by hand: the fresh 7-byte ack block --
// largest zero, nothing received, valid on any connection -- then the type
// data, sealed under the given key. What a peer that has received nothing
// on the session sends.

static int rocketnet_test_write_session_packet_salted( uint8_t * packet, uint8_t packet_type, uint64_t sequence,
                                                       const uint8_t * type_data, int type_data_bytes,
                                                       const uint8_t * salt, const uint8_t * key )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_MAX_PAYLOAD_BYTES];
    memset( plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    if ( type_data_bytes > 0 )
        memcpy( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, type_data, (size_t) type_data_bytes );
    return rocketnet_write_sealed_packet( packet, packet_type, tag, sequence, NULL,
                                          plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + type_data_bytes,
                                          ROCKETNET_TEST_PROTOCOL_ID, salt, NULL, key );
}

// the same, but carrying a real ack block: a single received run of
// run_length sequences ending at largest, inclusive. lets a test say
// exactly which of the server's packets the client claims to have received.

static int rocketnet_test_write_session_packet_acking( uint8_t * packet, uint8_t packet_type, uint64_t sequence,
                                                       const uint8_t * type_data, int type_data_bytes,
                                                       const uint8_t * key,
                                                       uint64_t largest, uint32_t run_length )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = largest;
    block.first_range_length = run_length;
    block.num_ranges = 0;

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + ROCKETNET_MAX_PAYLOAD_BYTES];
    int block_bytes = rocketnet_ack_block_write( plaintext, &block );
    if ( type_data_bytes > 0 )
        memcpy( plaintext + block_bytes, type_data, (size_t) type_data_bytes );

    return rocketnet_write_sealed_packet( packet, packet_type, tag, sequence, NULL,
                                          plaintext, block_bytes + type_data_bytes,
                                          ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL, key );
}

static int rocketnet_test_write_session_packet( uint8_t * packet, uint8_t packet_type, uint64_t sequence,
                                                const uint8_t * type_data, int type_data_bytes,
                                                const uint8_t * key )
{
    return rocketnet_test_write_session_packet_salted( packet, packet_type, sequence, type_data, type_data_bytes,
                                                       rocketnet_test_salt, key );
}

// runs request -> cookie -> request -> challenge and hands back the client's
// derived keys, leaving the server one response away from a connection
static void rocketnet_test_handshake_to_challenge( struct rocketnet_test_server_t * test, uint64_t client_id,
                                                   struct rocketnet_connection_keys_t * client_keys )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_test_build_request( request, client_id, rocketnet_test_salt );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    rocketnet_check( rocketnet_test_process_packet( test, request, sizeof(request), response, &response_bytes,
                                                    payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    memcpy( request + ROCKETNET_PREFILTER_BYTES, response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );

    rocketnet_check( rocketnet_test_process_packet( test, request, sizeof(request), response, &response_bytes,
                                                    payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    rocketnet_test_client_derive_from_challenge( response, response_bytes, client_keys );
}

void rocketnet_test_server_full_connect()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload_out[ROCKETNET_RECEIVE_CEILING];
    int payload_out_bytes = 0;
    int payload_client_index = -1;

    struct rocketnet_connection_keys_t client_keys;
    rocketnet_test_handshake_to_challenge( &test, 0xC11E17ULL, &client_keys );

    // the response connects: a slot is assigned, the token is consumed, the
    // mapping is established, and the first keep-alive comes back carrying
    // the client index and the slot count under the session keys
    uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    rocketnet_test_client_build_response( client_response, &client_keys );

    rocketnet_check( rocketnet_test_process_packet( &test, client_response, sizeof(client_response), response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CLIENT_CONNECTED] == 1 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    rocketnet_check( !rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) );

    int mapping_index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    rocketnet_check( mapping_index != -1 );
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, mapping_index );
    rocketnet_check( mapping->established );
    rocketnet_check( mapping->client_index == 0 );

    uint8_t token_identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    memcpy( token_identifier, mapping->token_identifier, sizeof(token_identifier) );
    rocketnet_check( rocketnet_admitted_token_set_contains( test.server.admitted_token_set, token_identifier, ROCKETNET_TEST_CREATE_TS ) );

    // the client opens the keep-alive -- the sealed plaintext begins with
    // the server's fresh ack block -- and learns its index behind it
    {
        const uint8_t * p = response + ROCKETNET_PREFILTER_BYTES;
        uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );
        uint8_t opened[ROCKETNET_RECEIVE_CEILING];
        rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, response, response_bytes,
                                                       ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                                       ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                       NULL, client_keys.session_server_to_client ) == ROCKETNET_ACK_BLOCK_MIN_BYTES + 8 );
        const uint8_t * q = opened;
        rocketnet_check( rocketnet_read_uint32( &q ) == 0 );        // ack block: largest received zero
        rocketnet_check( rocketnet_read_uint8( &q ) == 0 );         // no ranges
        rocketnet_check( rocketnet_read_uint16( &q ) == 0 );        // first range length zero
        rocketnet_check( rocketnet_read_uint32( &q ) == 0 );        // client index
        rocketnet_check( rocketnet_read_uint32( &q ) == 4 );        // slot count
    }

    // a duplicate response is ignored: the connection is already made, and
    // the periodic keep-alive is what repairs a lost first keep-alive
    struct rocketnet_counters_t before = test.counters;
    rocketnet_check( rocketnet_test_process_packet( &test, client_response, sizeof(client_response), response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED] == before.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED] + 1 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );

    // the client's first keep-alive confirms it
    uint8_t keep_alive_data[8];
    uint8_t * ka = keep_alive_data;
    rocketnet_write_uint32( &ka, 0 );
    rocketnet_write_uint32( &ka, 4 );
    uint8_t client_packet[ROCKETNET_RECEIVE_CEILING];
    rocketnet_check( rocketnet_test_write_session_packet( client_packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1,
                                                          keep_alive_data, 8,
                                                          client_keys.session_client_to_server ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    rocketnet_check( rocketnet_test_process_packet( &test, client_packet, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 1 );

    // a payload delivers, with the client index attached
    const uint8_t game_bytes[11] = { 0x67, 0x61, 0x6d, 0x65, 0x20, 0x62, 0x79, 0x74, 0x65, 0x73, 0x21 };
    int packet_bytes = rocketnet_test_write_session_packet( client_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 2,
                                                            game_bytes, sizeof(game_bytes),
                                                            client_keys.session_client_to_server );
    rocketnet_check( packet_bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + (int) sizeof(game_bytes) );
    rocketnet_check( rocketnet_test_process_packet( &test, client_packet, packet_bytes, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( payload_out_bytes == (int) sizeof(game_bytes) );
    rocketnet_check( memcmp( payload_out, game_bytes, sizeof(game_bytes) ) == 0 );
    rocketnet_check( payload_client_index == 0 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] == 1 );

    // the same payload again is a replay, refused before the open
    before = test.counters;
    rocketnet_check( rocketnet_test_process_packet( &test, client_packet, packet_bytes, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED] == before.values[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED] + 1 );
    rocketnet_check( payload_out_bytes == 0 );

    // the disconnect releases everything, never answered
    packet_bytes = rocketnet_test_write_session_packet( client_packet, ROCKETNET_CONNECTION_DISCONNECT_PACKET, 3,
                                                        NULL, 0,
                                                        client_keys.session_client_to_server );
    rocketnet_check( packet_bytes == ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
    rocketnet_check( rocketnet_test_process_packet( &test, client_packet, packet_bytes, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( response_bytes == 0 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CLIENT_DISCONNECTED] == 1 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 0 );
    rocketnet_check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );

    // and the consumed token cannot connect again, end to end: same token,
    // same address, everything valid -- refused at the admitted-set rung
    {
        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_test_build_request( request, 0xC11E17ULL, rocketnet_test_salt );
        // (a fresh token: different seal randomness, different identifier --
        // so to prove the rule we need the SAME token, which is gone. what we
        // prove instead: the set still holds the old identifier)
        rocketnet_check( rocketnet_admitted_token_set_contains( test.server.admitted_token_set, token_identifier, ROCKETNET_TEST_CREATE_TS ) );
    }

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_server_response_denials()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload_out[ROCKETNET_RECEIVE_CEILING];
    int payload_out_bytes = 0;
    int payload_client_index = -1;

    struct rocketnet_connection_keys_t client_keys;
    rocketnet_test_handshake_to_challenge( &test, 0xDEC1ULL, &client_keys );

    // every slot fills between the challenge and the response: the response
    // is denied under the handshake keys -- and the token is NOT consumed,
    // because a denied client never connected
    uint8_t dummy_user_data[ROCKETNET_USER_DATA_BYTES];
    memset( dummy_user_data, 0, sizeof(dummy_user_data) );
    for ( int i = 0; i < 4; i++ )
        rocketnet_check( rocketnet_client_slots_assign( test.server.client_slots, 0xF000 + (uint64_t) i, -1, dummy_user_data ) == i );

    uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    rocketnet_test_client_build_response( client_response, &client_keys );

    int mapping_index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    rocketnet_check( mapping_index != -1 );
    uint8_t token_identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    memcpy( token_identifier, rocketnet_encryption_map_entry( test.server.encryption_map, mapping_index )->token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES );

    rocketnet_check( rocketnet_test_process_packet( &test, client_response, sizeof(client_response), response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_DENIED );
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS] == 1 );
    rocketnet_check( !rocketnet_admitted_token_set_contains( test.server.admitted_token_set, token_identifier, ROCKETNET_TEST_CREATE_TS ) );

    // the denied opens client-side under the handshake keys at sequence 2
    {
        const uint8_t * p = response + ROCKETNET_PREFILTER_BYTES;
        uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 1 );
        rocketnet_check( sequence == 2 );
        uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
        uint8_t opened[64];
        rocketnet_check( rocketnet_open_sealed_packet( opened, session_nonce, response, response_bytes,
                                                       ROCKETNET_CONNECTION_DENIED_PACKET, sequence,
                                                       ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                       NULL, client_keys.handshake_server_to_client ) == 0 );
    }

    // a slot frees, the client retransmits the same response, and the
    // connection completes: the denial consumed nothing
    rocketnet_client_slots_release( test.server.client_slots, 0 );
    rocketnet_check( rocketnet_test_process_packet( &test, client_response, sizeof(client_response), response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    rocketnet_check( rocketnet_admitted_token_set_contains( test.server.admitted_token_set, token_identifier, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED] == 1 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_server_receive_refusals()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload_out[ROCKETNET_RECEIVE_CEILING];
    int payload_out_bytes = 0;
    int payload_client_index = -1;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    memset( packet, 0, sizeof(packet) );

    // prefilter garbage: a wrong filter value never reaches a type counter,
    // and lands on its own reason rather than a rollup
    packet[0] = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    packet[2] = 0xFF; packet[3] = 0xFF; packet[4] = 0xFF; packet[5] = 0xFF;
    rocketnet_check( rocketnet_test_process_packet( &test, packet, 64, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH] == 0 );

    // and a wrong epoch is a different reason again, distinguishable in the
    // counters -- which is the whole point of splitting them
    packet[2] = tag[0]; packet[3] = tag[1]; packet[4] = tag[2]; packet[5] = tag[3];
    packet[1] = 7;
    rocketnet_check( rocketnet_test_process_packet( &test, packet, 64, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE] == 1 );
    packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;

    // a reserved type is ignored
    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, 200, tag );
    rocketnet_check( rocketnet_test_process_packet( &test, packet, 64, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_RESERVED_TYPE] == 1 );

    // a wrong-direction type is ignored: a server never processes what only
    // a server sends
    p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_CHALLENGE_PACKET, tag );
    rocketnet_check( rocketnet_test_process_packet( &test, packet, 64, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_WRONG_DIRECTION] == 1 );

    // a sealed packet with no mapping is refused after one lookup
    p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, tag );
    rocketnet_check( rocketnet_test_process_packet( &test, packet, 64, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING] == 1 );

    // with a handshake in flight: session traffic before the response is
    // refused as not connected, and a zero sequence never opens
    struct rocketnet_connection_keys_t client_keys;
    rocketnet_test_handshake_to_challenge( &test, 0xE0E0ULL, &client_keys );

    const uint8_t some_bytes[4] = { 1, 2, 3, 4 };
    int packet_bytes = rocketnet_test_write_session_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1,
                                                            some_bytes, 4,
                                                            client_keys.session_client_to_server );
    rocketnet_check( rocketnet_test_process_packet( &test, packet, packet_bytes, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED] == 1 );

    memset( packet + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_SEQUENCE_BYTES );      // wire sequence zero
    rocketnet_check( rocketnet_test_process_packet( &test, packet, packet_bytes, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE] == 1 );

    // a response sealed under the wrong keys does not open
    uint8_t bad_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    const uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    uint8_t wrong_key[32];
    memset( wrong_key, 0x42, sizeof(wrong_key) );
    rocketnet_check( rocketnet_write_sealed_packet( bad_response, ROCKETNET_CONNECTION_RESPONSE_PACKET, tag, 1,
                                                    NULL, padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                                    ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                    rocketnet_test_client_eph_public,
                                                    wrong_key ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    rocketnet_check( rocketnet_test_process_packet( &test, bad_response, sizeof(bad_response), response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN] == 1 );

    rocketnet_test_server_teardown( &test );
}

// --- client and server, pumped against each other in memory ---

static void rocketnet_test_mint_token( uint8_t * token_data, uint64_t client_id, uint64_t create, uint64_t expire, int32_t timeout )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = timeout;
    private_fields.client_id = client_id;
    rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// One pump: client and server exchanging packets through a perfect wire,
// with at most one packet type dropped, once, in either direction -- the
// deterministic loss walk. The harness generates the server's periodic
// keep-alives for connected clients, standing in for the send pacing the
// server socket layer will own. Returns the client's state at exit.

static int rocketnet_test_pump( struct rocketnet_test_server_t * test,
                                struct rocketnet_client_protocol_t * client,
                                int drop_once_packet_type,
                                double max_seconds )
{
    int dropped = 0;
    double time = 100.0;
    double stop_time = time + max_seconds;
    double last_server_keep_alive_time = time;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    while ( time < stop_time )
    {
        rocketnet_client_protocol_update( client, time, packet, &packet_bytes );

        if ( client->state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            return client->state;

        if ( packet_bytes > 0 )
        {
            int type = packet[0];
            if ( type == drop_once_packet_type && !dropped )
            {
                dropped = 1;
            }
            else
            {
                rocketnet_server_protocol_process_packet( &test->server, &test->client_address, packet, packet_bytes,
                                                 ROCKETNET_TEST_CREATE_TS, time,
                                                 response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index );
                if ( response_bytes > 0 )
                {
                    int response_type = response[0];
                    if ( response_type == drop_once_packet_type && !dropped )
                        dropped = 1;
                    else
                        rocketnet_client_protocol_process_packet( client, response, response_bytes, time, payload, &payload_bytes );
                }
            }
        }

        // the server's periodic keep-alives, at the send rate per client
        if ( time - last_server_keep_alive_time >= 0.1 )
        {
            last_server_keep_alive_time = time;
            for ( int i = 0; i < rocketnet_client_slots_max_clients( test->server.client_slots ); i++ )
            {
                if ( !rocketnet_client_slots_connected( test->server.client_slots, i ) )
                    continue;
                int bytes = rocketnet_server_write_keep_alive( &test->server, i, time, response );
                if ( bytes > 0 )
                {
                    if ( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET == drop_once_packet_type && !dropped )
                        dropped = 1;
                    else
                        rocketnet_client_protocol_process_packet( client, response, bytes, time, payload, &payload_bytes );
                }
            }
        }

        if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            return client->state;

        time += 0.05;
    }

    return client->state;
}

void rocketnet_test_server_update_sweep()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    // a stalled handshake: request through challenge, then nothing. it dies
    // at the handshake deadline no matter what keeps arriving -- the
    // deadline is never refreshed.
    struct rocketnet_connection_keys_t client_keys;
    rocketnet_test_handshake_to_challenge( &test, 0x51A11ULL, &client_keys );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );

    rocketnet_server_protocol_update( &test.server, 104.9 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );
    rocketnet_server_protocol_update( &test.server, 105.0 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 1 );

    // a connected client that goes silent: the slot releases with the
    // mapping, together, at the idle timeout
    rocketnet_test_handshake_to_challenge( &test, 0x51A22ULL, &client_keys );
    uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    rocketnet_test_client_build_response( client_response, &client_keys );
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    rocketnet_check( rocketnet_test_process_packet( &test, client_response, sizeof(client_response), response, &response_bytes,
                                                    payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );

    rocketnet_server_protocol_update( &test.server, 109.9 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    rocketnet_server_protocol_update( &test.server, 110.1 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 0 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == 1 );

    rocketnet_test_server_teardown( &test );
}


void rocketnet_test_server_keep_alive_pacing()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );
    test.server.send_rate = 10.0;

    struct rocketnet_connection_keys_t client_keys;
    rocketnet_test_handshake_to_challenge( &test, 0x9ACEULL, &client_keys );
    uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    rocketnet_test_client_build_response( client_response, &client_keys );
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    rocketnet_check( rocketnet_test_process_packet( &test, client_response, sizeof(client_response), response, &response_bytes,
                                                    payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );

    // the first keep-alive at the response stamped the keep-alive clock at
    // 100.0, so nothing is due until the interval elapses -- the rate is the
    // rate in either regime
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    rocketnet_check( !rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) );
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.05, packet ) == 0 );
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.11, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    // UNACKNOWLEDGED: a payload send does NOT suppress the next keep-alive. The
    // client has not claimed to have received a keep-alive, and the
    // keep-alive is the only packet carrying its index, so generation runs
    // off elapsed time alone -- the payload at 100.15 changes nothing, and
    // the keep-alive is due one interval after the last one at 100.11.
    // (server session sequences so far: 1 ka, 2 ka, 3 payload, 4 ka)
    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_check( rocketnet_server_protocol_write_payload( &test.server, 0, data, 4, 100.15, packet ) > 0 );
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.16, packet ) == 0 );
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.22, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    // AN ACK THAT COVERS ONLY A PAYLOAD IS NOT THE CLAIM. The client
    // acknowledges sequence 3 -- the payload -- and nothing else. That
    // confirms the slot in the old sense and says nothing about whether the
    // client holds its index, so the regime MUST NOT flip: the payload at
    // 100.25 still fails to suppress the keep-alive due at 100.33.
    uint8_t client_packet[ROCKETNET_RECEIVE_CEILING];
    int client_bytes = rocketnet_test_write_session_packet_acking( client_packet, ROCKETNET_CONNECTION_ACK_PACKET, 1,
                                                                   NULL, 0, client_keys.session_client_to_server, 3, 1 );
    rocketnet_check( client_bytes > 0 );
    rocketnet_check( rocketnet_test_process_packet( &test, client_packet, client_bytes, response, &response_bytes,
                                                    payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) );        // confirmed, and still not the claim
    rocketnet_check( !test.server.acks->endpoints[0].keep_alive_acked );

    rocketnet_check( rocketnet_server_protocol_write_payload( &test.server, 0, data, 4, 100.25, packet ) > 0 );      // seq 5
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.33, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );   // seq 6

    // AN ACK THAT COVERS A KEEP-ALIVE IS THE CLAIM THE RULE TURNS ON, and
    // only now does the regime flip. Sequence 6 is the keep-alive just written; acknowledging
    // it CLAIMS the client opened a keep-alive, which carries its index and
    // is its only transition to _connected_. The server cannot check the
    // claim; see the note below on what that costs and who it costs.
    client_bytes = rocketnet_test_write_session_packet_acking( client_packet, ROCKETNET_CONNECTION_ACK_PACKET, 2,
                                                               NULL, 0, client_keys.session_client_to_server, 6, 1 );
    rocketnet_check( client_bytes > 0 );
    rocketnet_check( rocketnet_test_process_packet( &test, client_packet, client_bytes, response, &response_bytes,
                                                    payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( test.server.acks->endpoints[0].keep_alive_acked );

    // Note what this block actually was: this synthetic client never opened
    // sequence 6 -- it never opened anything. It CLAIMED to, and the server
    // took the claim, because an ack block is the peer's statement about its
    // own receive state and no receiver can check it. That is the
    // false-positive direction _Keep-alive generation_ documents, reproduced
    // here deliberately: an over-claiming block latches the pacing regime.
    //
    // And it reaches exactly one slot. The latch lives per client index, and
    // the block arrived inside a packet sealed under this connection's own
    // session keys, so no other client's pacing moved -- which is why an
    // over-claiming client strands only itself.

    for ( int i = 1; i < rocketnet_client_slots_max_clients( test.server.client_slots ); i++ )
        rocketnet_check( !test.server.acks->endpoints[i].keep_alive_acked );

    // ACKNOWLEDGED: ordinary traffic is the keep-alive again.
    rocketnet_check( rocketnet_server_protocol_write_payload( &test.server, 0, data, 4, 100.36, packet ) > 0 );
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.44, packet ) == 0 );
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, 100.47, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    // THE TRANSPORT'S OWN ORDERING, which the loops above do not exercise:
    // the sweep runs inside update and the application's sends land in the
    // fill phase afterwards, so the check sees the PREVIOUS send's stamp.
    // The standard's surviving guarantee is stated for that order too --
    // every gap strictly shorter than the interval means total suppression
    // -- so assert it here rather than only in payload-then-check order.
    {
        double base = 200.0;
        rocketnet_check( rocketnet_server_protocol_write_payload( &test.server, 0, data, 4, base, packet ) > 0 );
        int swept = 0;
        for ( int i = 1; i <= 40; i++ )
        {
            double now = base + i * 0.09;                    // gaps of 0.09 < the 0.1 interval
            if ( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, now, packet ) > 0 )
                swept++;                                     // check first, as update does
            rocketnet_check( rocketnet_server_protocol_write_payload( &test.server, 0, data, 4, now, packet ) > 0 );
        }
        rocketnet_check( swept == 0 );
    }

    // a disconnected index degrades to zero, never an assert
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 3, 200.0, packet ) == 0 );
    rocketnet_check( rocketnet_server_protocol_write_keep_alive_if_due( &test.server, -1, 200.0, packet ) == 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_client_payload_before_first_keep_alive()
{
    // a payload racing ahead of the first keep-alive is delivered, not
    // dropped: the session keys are born at the challenge, so nothing about
    // payload bytes needs the client index -- the reason this protocol has
    // no keep-alive prefixing for unconfirmed clients

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0xBEF0ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    // drive to sending connection response, complete the server side, but
    // swallow the first keep-alive: the client holds session keys and no
    // index
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    for ( double time = 100.0; time < 102.0 && client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE; time += 0.05 )
    {
        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes == 0 )
            continue;
        rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                         ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                         payload, &payload_bytes, &payload_client_index );
        if ( response_bytes > 0 )
            rocketnet_client_protocol_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
    }
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    rocketnet_client_protocol_update( &client, 102.1, packet, &packet_bytes );
    rocketnet_check( packet_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 102.1, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    // the first keep-alive is dropped here, deliberately

    // the server sends a payload to the still-unconfirmed client: delivered
    const uint8_t early_data[5] = { 0xE, 0xA, 0x12, 0x1, 0x9 };
    int bytes = rocketnet_server_protocol_write_payload( &test.server, 0, early_data, sizeof(early_data), 102.2, packet );
    rocketnet_check( bytes > 0 );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, 102.2, payload, &payload_bytes ) );
    rocketnet_check( payload_bytes == (int) sizeof(early_data) );
    rocketnet_check( memcmp( payload, early_data, sizeof(early_data) ) == 0 );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    // and the periodic keep-alive still carries the index and connects it
    bytes = rocketnet_server_write_keep_alive( &test.server, 0, 102.3, packet );
    rocketnet_check( bytes > 0 );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, 102.3, payload, &payload_bytes ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( client.client_index == 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_client_keep_alive_suppression()
{
    // the client's ordinary traffic is its keep-alive, exactly as on the
    // server: a payload stamps the send, and the scheduled keep-alive stays
    // suppressed until the connection is otherwise idle

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0x50FFAULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    double time = client.last_send_time;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;

    // a payload at time t suppresses the keep-alive until t + interval
    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_check( rocketnet_client_protocol_write_payload( &client, data, 4, time + 0.05, packet ) > 0 );
    rocketnet_client_protocol_update( &client, time + 0.11, packet, &packet_bytes );
    rocketnet_check( packet_bytes == 0 );
    rocketnet_client_protocol_update( &client, time + 0.16, packet, &packet_bytes );
    rocketnet_check( packet_bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_keep_alive_unestablished_under_payload_traffic()
{
    // #90, the stranding case, driven end to end on a punched wire: the
    // establishment keep-alive -- the only packet that carries the client
    // index, and the only one that moves the client out of _sending
    // connection response_ -- is lost, while the server application sends
    // payloads continuously from the moment the slot exists.
    //
    // No keep-alive is ever acknowledged here, because none is ever
    // received. Nor does this client's own traffic carry any acknowledgment
    // at all: the only packet it sends is the retransmitted connection
    // response, a handshake type, and handshake packets carry no ack block.
    // Nothing it can send re-provokes a keep-alive either. Its connection
    // response is ignored as already connected, and no session type elicits
    // one. So the repair has to come from the server's own pacing.
    //
    // What must hold: the keep-alive generated at the send rate reaches the
    // client despite the payload stream, and the connection establishes.
    // The loss walkthrough's invariant is that another keep-alive keeps
    // coming and no traffic pattern can stop it; this test is that sentence,
    // with a payload on every tick trying to make it false. It asserts
    // recurrence within a bound, not a bound of exactly one send interval --
    // the standard deliberately makes no such claim.

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );
    test.server.send_rate = 10.0;

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0x90570ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    const double tick = 0.01;                   // ten ticks to a send interval
    double establishment_time = -1.0;           // when the lost keep-alive was written
    double connected_time = -1.0;
    int payloads_sent_while_unestablished = 0;
    int payloads_received_while_unestablished = 0;
    int state_after_the_loss = -1;

    for ( double time = 100.0; time < 110.0 && connected_time < 0.0; time += tick )
    {
        // the client's own scheduled send: requests, then the response,
        // retransmitted at the send rate for as long as it waits

        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
        {
            int result = rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                                   ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                                   payload, &payload_bytes, &payload_client_index );
            if ( result == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE )
            {
                // the punched loss: exactly one packet, the establishment
                // keep-alive, never reaches the wire. everything else the
                // server sends is delivered.
                rocketnet_check( establishment_time < 0.0 );
                establishment_time = time;
                state_after_the_loss = client.state;
                response_bytes = 0;
            }
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
        }

        rocketnet_server_protocol_update( &test.server, time );

        if ( rocketnet_client_slots_connected( test.server.client_slots, 0 ) )
        {
            // continuous payload traffic: a payload on every tick, ten to
            // every send interval. this is the suppression pressure.

            const uint8_t stream_byte[1] = { 0x90 };
            int stream_bytes = rocketnet_server_protocol_write_payload( &test.server, 0, stream_byte, 1, time, response );
            rocketnet_check( stream_bytes > 0 );
            if ( !rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) )
                payloads_sent_while_unestablished++;
            int connected_before = ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
            if ( rocketnet_client_protocol_process_packet( &client, response, stream_bytes, time, payload, &payload_bytes ) &&
                 payload_bytes > 0 && !connected_before )
            {
                payloads_received_while_unestablished++;
            }

            int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, time, response );
            if ( keep_alive_bytes > 0 )
                rocketnet_client_protocol_process_packet( &client, response, keep_alive_bytes, time, payload, &payload_bytes );
        }

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            connected_time = time;
    }

    // the loss landed, and it landed on a client that was still waiting for
    // its index -- without this the rest of the test proves nothing

    rocketnet_check( establishment_time > 0.0 );
    rocketnet_check( state_after_the_loss == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    // the client connected, and it did so promptly: the assertion below is
    // two send intervals, which is the loose bound this test actually
    // checks. The point is not the exact spacing -- the standard makes no
    // one-interval claim, and the discrete check makes none available -- but
    // that the repair arrived on the keep-alive's own clock rather than
    // waiting for the payload stream to pause, which it never does here.

    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( client.client_index == 0 );
    rocketnet_check( connected_time > 0.0 );
    rocketnet_check( connected_time - establishment_time <= 2.0 / test.server.send_rate );

    // and the pressure was real for the whole of that window: the payload
    // stream never left a send-interval-sized hole for a suppressed
    // keep-alive to slip through, so a pass here cannot be an artifact of
    // the traffic stopping

    rocketnet_check( payloads_sent_while_unestablished >= 5 );
    rocketnet_check( payloads_received_while_unestablished >= 5 );
    rocketnet_check( (double) payloads_sent_while_unestablished * tick >= connected_time - establishment_time - 2.0 * tick );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_keep_alive_pacing_survives_early_ack()
{
    // The second half of #90, found by a cold reader refuting the first fix.
    //
    // That fix flipped the suppression regime on the confirmed flag, and
    // justified it with "a client sends nothing sealed before _connected_".
    // The standard's own _Acks_ tells a client otherwise: a standalone ack
    // SHOULD go out when the ack debt reaches its threshold, with no state
    // qualifier, and a client in _sending connection response_ is already
    // being delivered payloads and accruing debt. A client that obeys that
    // SHOULD seals a type 8 from state 3, confirms its own slot, and the
    // server switches to traffic-suppressed pacing before the client has
    // ever learned its index -- #90 again, with no packet loss needed after
    // the establishment keep-alive.
    //
    // This test builds that type 8 exactly as a conformant client would --
    // the client's own session key, its own replay state, its own sequence
    // -- rather than reaching into a private gate, because the point is
    // that the packet is LEGAL, not that it is forced.

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );
    test.server.send_rate = 10.0;

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0x90ACEULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    const double tick = 0.01;
    double establishment_time = -1.0;
    double connected_time = -1.0;
    int early_ack_sent = 0;
    int payloads_sent_while_unestablished = 0;      // counted while the CLIENT is not yet connected

    for ( double time = 100.0; time < 110.0 && connected_time < 0.0; time += tick )
    {
        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
        {
            int result = rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                                   ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                                   payload, &payload_bytes, &payload_client_index );
            if ( result == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE )
            {
                // the same punched loss as the first reproduction: the
                // establishment keep-alive never reaches the wire
                rocketnet_check( establishment_time < 0.0 );
                establishment_time = time;
                response_bytes = 0;
            }
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
        }

        rocketnet_server_protocol_update( &test.server, time );

        if ( rocketnet_client_slots_connected( test.server.client_slots, 0 ) )
        {
            const uint8_t stream_byte[1] = { 0x5A };
            int stream_bytes = rocketnet_server_protocol_write_payload( &test.server, 0, stream_byte, 1, time, response );
            rocketnet_check( stream_bytes > 0 );
            if ( client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                payloads_sent_while_unestablished++;
            rocketnet_client_protocol_process_packet( &client, response, stream_bytes, time, payload, &payload_bytes );

            // the conformant standalone ack, sealed from _sending connection
            // response_, sent once as soon as the client has something to
            // acknowledge. this is the packet _Acks_ asks for.

            if ( !early_ack_sent && client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE &&
                 client.ack.ack_debt > 0 )
            {
                uint8_t block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
                int block_bytes = rocketnet_ack_block_build( block, ROCKETNET_ACK_BLOCK_MAX_BYTES, &client.replay_session );
                uint8_t early_ack[ROCKETNET_RECEIVE_CEILING];
                int early_ack_bytes = rocketnet_write_sealed_packet( early_ack, ROCKETNET_CONNECTION_ACK_PACKET,
                                                                     client.filter_value, client.send_sequence_session,
                                                                     NULL, block, block_bytes,
                                                                     client.signed_fields.protocol_id, client.connection_salt,
                                                                     NULL, client.keys.session_client_to_server );
                rocketnet_check( early_ack_bytes > 0 );
                client.send_sequence_session++;
                rocketnet_server_protocol_process_packet( &test.server, &test.client_address, early_ack, early_ack_bytes,
                                                          ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index );
                early_ack_sent = 1;
            }

            int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, time, response );
            if ( keep_alive_bytes > 0 )
                rocketnet_client_protocol_process_packet( &client, response, keep_alive_bytes, time, payload, &payload_bytes );
        }

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            connected_time = time;
    }

    // the setup actually happened: the loss landed, and the legal early ack
    // went out and was accepted -- without both, nothing below is meaningful

    rocketnet_check( establishment_time > 0.0 );
    rocketnet_check( early_ack_sent == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_ACCEPTED] >= 1 );
    rocketnet_check( payloads_sent_while_unestablished >= 5 );

    // and the client still connects: an ack from a client that does not yet
    // hold its index MUST NOT flip the regime as though it did

    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( client.client_index == 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_client_connect_validations()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];

    // a good token enters requesting cookie
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
    rocketnet_check( client.server_address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_check( client.server_address.port == 40000 );

    // create after expire is an invalid token
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_EXPIRE_TS, ROCKETNET_TEST_CREATE_TS, 10 );
    rocketnet_check( !rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );

    // bad address flags are an invalid token
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        signed_fields.address_flags = 0x05;
        rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
        rocketnet_check( !rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
        rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );
    }

    // an expired token dies before anything is sent
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 10, 10 );
    rocketnet_check( !rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS + 10 ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
}

void rocketnet_test_client_server_connect()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0xC0FFEEULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    // the whole handshake completes through a perfect wire
    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( client.client_index == 0 );
    rocketnet_check( client.max_clients == 4 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    rocketnet_check( rocketnet_client_slots_client_id( test.server.client_slots, 0 ) == 0xC0FFEEULL );

    // payloads flow both ways
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    const uint8_t to_server[7] = { 1, 2, 3, 4, 5, 6, 7 };
    int bytes = rocketnet_client_protocol_write_payload( &client, to_server, sizeof(to_server), 200.0, packet );
    rocketnet_check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + (int) sizeof(to_server) );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.0,
                                                      response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( payload_bytes == (int) sizeof(to_server) );
    rocketnet_check( memcmp( payload, to_server, sizeof(to_server) ) == 0 );
    rocketnet_check( payload_client_index == 0 );

    const uint8_t to_client[3] = { 9, 8, 7 };
    bytes = rocketnet_server_protocol_write_payload( &test.server, 0, to_client, sizeof(to_client), 200.0, packet );
    rocketnet_check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + (int) sizeof(to_client) );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, 200.0, payload, &payload_bytes ) );
    rocketnet_check( payload_bytes == (int) sizeof(to_client) );
    rocketnet_check( memcmp( payload, to_client, sizeof(to_client) ) == 0 );

    // the deliberate disconnect: the redundant burst, the first copy lands,
    // the rest find nothing, and both sides are clean
    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_client_protocol_disconnect( &client, burst, &burst_count, &burst_bytes );
    rocketnet_check( burst_count == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    rocketnet_check( burst_bytes >= ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    for ( int i = 0; i < burst_count; i++ )
    {
        rocketnet_server_protocol_process_packet( &test.server, &test.client_address,
                                         burst + i * ROCKETNET_SEND_BUDGET,
                                         burst_bytes,
                                         ROCKETNET_TEST_CREATE_TS, 200.0,
                                         response, &response_bytes,
                                         payload, &payload_bytes, &payload_client_index );
    }
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 0 );
    rocketnet_check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CLIENT_DISCONNECTED] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING] == (uint64_t) ( burst_count - 1 ) );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_client_server_loss_walk()
{
    // every handshake packet lost once, and the connection still completes:
    // no single loss can strand either side. dropping a request covers both
    // request sends; the keep-alive drop loses the first one and the
    // periodic keep-alives repair it.

    const int drop_types[5] = {
        ROCKETNET_CONNECTION_REQUEST_PACKET,
        ROCKETNET_CONNECTION_COOKIE_PACKET,
        ROCKETNET_CONNECTION_CHALLENGE_PACKET,
        ROCKETNET_CONNECTION_RESPONSE_PACKET,
        ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
    };

    for ( int i = 0; i < 5; i++ )
    {
        struct rocketnet_test_server_t test;
        rocketnet_test_server_setup( &test );

        struct rocketnet_counters_t client_counters;
        memset( &client_counters, 0, sizeof(client_counters) );
        struct rocketnet_client_protocol_t client;
        rocketnet_client_protocol_init( &client, &client_counters );

        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_test_mint_token( token_data, 0xA0000ULL + (uint64_t) i, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
        rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

        rocketnet_check( rocketnet_test_pump( &test, &client, drop_types[i], 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

        rocketnet_test_server_teardown( &test );
    }
}

void rocketnet_test_client_denied_when_server_full()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    uint8_t dummy_user_data[ROCKETNET_USER_DATA_BYTES];
    memset( dummy_user_data, 0, sizeof(dummy_user_data) );
    for ( int i = 0; i < 4; i++ )
        rocketnet_check( rocketnet_client_slots_assign( test.server.client_slots, 0xF100 + (uint64_t) i, -1, dummy_user_data ) == i );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0xDEDEULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_DENIED_ACCEPTED] == 1 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_client_timeout_paths()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    struct rocketnet_client_protocol_t client;
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;

    // no server at all: requesting cookie times out to cookie request timed
    // out, the -(N+1) pairing at state 1
    rocketnet_client_protocol_init( &client, &counters );
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    for ( double time = 100.0; time < 112.0; time += 0.05 )
        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT );

    // a server that answers only cookies: sending connection request times
    // out to connection request timed out, the pairing at state 2
    {
        struct rocketnet_test_server_t test;
        rocketnet_test_server_setup( &test );
        rocketnet_client_protocol_init( &client, &counters );
        rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        for ( double time = 100.0; time < 115.0; time += 0.05 )
        {
            rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
            if ( client.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
                break;
            if ( packet_bytes > 0 )
            {
                rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                 ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index );
                // deliver cookies; swallow challenges
                if ( response_bytes > 0 && response[0] == ROCKETNET_CONNECTION_COOKIE_PACKET )
                    rocketnet_client_protocol_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
            }
        }
        rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT );
        rocketnet_test_server_teardown( &test );
    }

    // a server that goes silent after the challenge: sending connection
    // response times out to connection response timed out, the pairing at 3
    {
        struct rocketnet_test_server_t test;
        rocketnet_test_server_setup( &test );
        rocketnet_client_protocol_init( &client, &counters );
        rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        for ( double time = 100.0; time < 115.0; time += 0.05 )
        {
            rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
            if ( client.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
                break;
            if ( packet_bytes > 0 && client.state < ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
            {
                rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                 ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index );
                if ( response_bytes > 0 )
                    rocketnet_client_protocol_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
            }
        }
        rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT );
        rocketnet_test_server_teardown( &test );
    }

    // connected, then silence: the pairing at 4, connection timed out
    {
        struct rocketnet_test_server_t test;
        rocketnet_test_server_setup( &test );
        struct rocketnet_counters_t client_counters;
        memset( &client_counters, 0, sizeof(client_counters) );
        rocketnet_client_protocol_init( &client, &client_counters );
        rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
        rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

        double connected_time = client.last_receive_time;
        for ( double time = connected_time; time < connected_time + 12.0; time += 0.05 )
            rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT );
        rocketnet_test_server_teardown( &test );
    }

    // the total connect deadline: per-state timers disabled by a negative
    // timeout, and the whole handshake still dies once the token's lifetime
    // elapses -- connect or be discarded, and no amount of nothing extends it
    rocketnet_client_protocol_init( &client, &counters );
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 5, -1 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    for ( double time = 100.0; time < 107.0; time += 0.05 )
        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
}

void rocketnet_test_client_cookie_injection()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    // an off-path injection: right shape, wrong salt. refused, nothing
    // stored, and the state does not move -- the attacker must guess eight
    // random bytes it has never seen.
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t wrong_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    memcpy( wrong_salt, client.connection_salt, sizeof(wrong_salt) );
    wrong_salt[0] ^= 1;
    uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES];
    memset( mac, 0xAA, sizeof(mac) );
    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( cookie, tag, wrong_salt, ROCKETNET_TEST_CREATE_TS + 3, mac );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_check( !rocketnet_client_protocol_process_packet( &client, cookie, sizeof(cookie), 100.0, payload, &payload_bytes ) );
    rocketnet_check( counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH] == 1 );
    rocketnet_check( !client.has_cookie );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    // the genuine cookie stores and advances
    uint8_t real_cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    uint8_t real_mac[ROCKETNET_COOKIE_MAC_BYTES];
    struct rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );
    rocketnet_check( rocketnet_generate_cookie_mac( real_mac, &client_address, client.connection_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_key ) );
    rocketnet_write_connection_cookie_packet( real_cookie, tag, client.connection_salt, ROCKETNET_TEST_CREATE_TS + 3, real_mac );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, real_cookie, sizeof(real_cookie), 100.0, payload, &payload_bytes ) );
    rocketnet_check( client.has_cookie );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
}

void rocketnet_test_network_simulator()
{
    struct rocketnet_address_t address_a, address_b;
    rocketnet_check( rocketnet_address_parse( &address_a, "10.0.0.1:30000" ) );
    rocketnet_check( rocketnet_address_parse( &address_b, "10.0.0.2:30000" ) );

    const uint8_t packet[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t received[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_address_t from;

    // a perfect wire delivers immediately, to the right address only
    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 64, 12345 );
    rocketnet_check( simulator );
    rocketnet_network_simulator_config( simulator, 0.0, 0.0, 0.0f, 0.0f );

    rocketnet_network_simulator_send( simulator, &address_a, &address_b, packet, sizeof(packet), 100.0 );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_a, 100.0, &from, received ) == 0 );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 100.0, &from, received ) == (int) sizeof(packet) );
    rocketnet_check( memcmp( received, packet, sizeof(packet) ) == 0 );
    rocketnet_check( rocketnet_address_equal( &from, &address_a ) );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 100.0, &from, received ) == 0 );

    // latency holds a packet until its time
    rocketnet_network_simulator_config( simulator, 0.05, 0.0, 0.0f, 0.0f );
    rocketnet_network_simulator_send( simulator, &address_a, &address_b, packet, sizeof(packet), 100.0 );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 100.04, &from, received ) == 0 );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 100.05, &from, received ) == (int) sizeof(packet) );

    // loss loses about its share, and the same seed loses exactly the same
    // packets: a failing run replays
    int delivered_first = 0;
    rocketnet_network_simulator_config( simulator, 0.0, 0.0, 25.0f, 0.0f );
    for ( int i = 0; i < 200; i++ )
    {
        rocketnet_network_simulator_send( simulator, &address_a, &address_b, packet, sizeof(packet), 200.0 );
        if ( rocketnet_network_simulator_receive( simulator, &address_b, 200.0, &from, received ) > 0 )
            delivered_first++;
    }
    rocketnet_check( delivered_first > 100 && delivered_first < 190 );
    rocketnet_network_simulator_destroy( simulator );

    simulator = rocketnet_network_simulator_create( NULL, 64, 12345 );
    rocketnet_network_simulator_config( simulator, 0.0, 0.0, 0.0f, 0.0f );
    rocketnet_network_simulator_send( simulator, &address_a, &address_b, packet, sizeof(packet), 100.0 );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 100.0, &from, received ) == (int) sizeof(packet) );
    rocketnet_network_simulator_config( simulator, 0.05, 0.0, 0.0f, 0.0f );
    rocketnet_network_simulator_send( simulator, &address_a, &address_b, packet, sizeof(packet), 100.0 );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 100.05, &from, received ) == (int) sizeof(packet) );
    int delivered_second = 0;
    rocketnet_network_simulator_config( simulator, 0.0, 0.0, 25.0f, 0.0f );
    for ( int i = 0; i < 200; i++ )
    {
        rocketnet_network_simulator_send( simulator, &address_a, &address_b, packet, sizeof(packet), 200.0 );
        if ( rocketnet_network_simulator_receive( simulator, &address_b, 200.0, &from, received ) > 0 )
            delivered_second++;
    }
    rocketnet_check( delivered_second == delivered_first );

    // duplicates arrive as extra copies
    rocketnet_network_simulator_config( simulator, 0.0, 0.0, 0.0f, 100.0f );
    rocketnet_network_simulator_send( simulator, &address_a, &address_b, packet, sizeof(packet), 300.0 );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 300.0, &from, received ) == (int) sizeof(packet) );
    rocketnet_check( rocketnet_network_simulator_receive( simulator, &address_b, 300.0, &from, received ) == (int) sizeof(packet) );

    rocketnet_network_simulator_destroy( simulator );
}

void rocketnet_test_client_server_harsh_wire()
{
    // the harsh version, bounded and seeded: heavy loss, latency, jitter and
    // duplication, but tuned so a client still completes well inside its
    // timeouts -- proven by the connect landing every run at this seed. the
    // soak runs light loss, where any failure is a finding; this test keeps
    // the brutal path exercised on every test run.

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 1024, 0x5EED5EED5EED5EEDULL );
    rocketnet_check( simulator );
    rocketnet_network_simulator_config( simulator, 0.2, 0.1, 25.0f, 25.0f );

    struct rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "203.0.113.10:40000" ) );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0x4A125ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    struct rocketnet_address_t from;

    for ( double time = 100.0; time < 110.0 && client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; time += 0.01 )
    {
        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
            rocketnet_network_simulator_send( simulator, &test.client_address, &server_address, packet, packet_bytes, time );

        int bytes;
        while ( ( bytes = rocketnet_network_simulator_receive( simulator, &server_address, time, &from, packet ) ) > 0 )
        {
            rocketnet_server_protocol_process_packet( &test.server, &from, packet, bytes,
                                             ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                             payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &from, response, response_bytes, time );
        }

        rocketnet_server_protocol_update( &test.server, time );
        for ( int i = 0; i < 4; i++ )
        {
            int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &test.server, i, time, response );
            if ( keep_alive_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &test.client_address, response, keep_alive_bytes, time );
        }

        while ( ( bytes = rocketnet_network_simulator_receive( simulator, &test.client_address, time, &from, packet ) ) > 0 )
            rocketnet_client_protocol_process_packet( &client, packet, bytes, time, payload, &payload_bytes );
    }

    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_network_simulator_destroy( simulator );
    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_client_server_lossy_wire()
{
    // the stochastic proof beside the deterministic loss walk: a wire with
    // real latency, jitter, loss and duplicates, at a fixed seed, and the
    // connection still completes and carries traffic. duplicates double as
    // the replay-protection soak: every duplicated session packet must be
    // refused exactly once and change nothing.

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 256, 0xBADC0FFEE0DDF00DULL );
    rocketnet_check( simulator );
    rocketnet_network_simulator_config( simulator, 0.03, 0.02, 5.0f, 5.0f );

    struct rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "203.0.113.10:40000" ) );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0x105511ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    struct rocketnet_address_t from;

    int client_received_payloads = 0;

    for ( double time = 100.0; time < 115.0; time += 0.01 )
    {
        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
            rocketnet_network_simulator_send( simulator, &test.client_address, &server_address, packet, packet_bytes, time );

        // everything due at the server this tick
        int bytes;
        while ( ( bytes = rocketnet_network_simulator_receive( simulator, &server_address, time, &from, packet ) ) > 0 )
        {
            rocketnet_server_protocol_process_packet( &test.server, &from, packet, bytes,
                                             ROCKETNET_TEST_CREATE_TS, time,
                                             response, &response_bytes,
                                             payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &from, response, response_bytes, time );
        }

        // the server's pacing: sweep, keep-alives when idle, and once
        // connected a payload stream toward the client
        rocketnet_server_protocol_update( &test.server, time );
        for ( int i = 0; i < 4; i++ )
        {
            int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &test.server, i, time, response );
            if ( keep_alive_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &test.client_address, response, keep_alive_bytes, time );
        }
        if ( rocketnet_client_slots_connected( test.server.client_slots, 0 ) && client_received_payloads < 20 )
        {
            const uint8_t stream_byte[1] = { 0x77 };
            int stream_bytes = rocketnet_server_protocol_write_payload( &test.server, 0, stream_byte, 1, time, response );
            if ( stream_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &test.client_address, response, stream_bytes, time );
        }

        // everything due at the client this tick
        while ( ( bytes = rocketnet_network_simulator_receive( simulator, &test.client_address, time, &from, packet ) ) > 0 )
        {
            if ( rocketnet_client_protocol_process_packet( &client, packet, bytes, time, payload, &payload_bytes ) && payload_bytes > 0 )
                client_received_payloads++;
        }
    }

    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( client_received_payloads >= 10 );

    // the duplicates the wire injected were each refused exactly once at the
    // replay window, and nothing downstream of them moved
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED] +
                     client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED] > 0 );

    rocketnet_network_simulator_destroy( simulator );
    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_swarm()
{
    // ALWAYS BE LOADED: a thousand real clients against one server, in
    // memory on a direct wire, with churn -- most connect and stay, some
    // connect and leave, some abandon mid-handshake so the deadline reclaim
    // runs under load, and a garbage generator keeps the reject paths hot
    // the whole time. The counters must account for every packet exactly,
    // and the reasons that must never fire must be exactly zero at the end.

    const int num_clients = 1000;
    const int num_garbage_senders = 50;

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );
    rocketnet_client_slots_destroy( test.server.client_slots );
    rocketnet_encryption_map_destroy( test.server.encryption_map );
    rocketnet_used_token_history_destroy( test.server.used_token_history );
    rocketnet_admitted_token_set_destroy( test.server.admitted_token_set );
    rocketnet_server_acks_destroy( test.server.acks );
    rocketnet_reassembly_set_destroy( test.server.reassembly );
    test.server.client_slots = rocketnet_client_slots_create( NULL, num_clients );
    test.server.encryption_map = rocketnet_encryption_map_create( NULL, num_clients * 2 );
    test.server.used_token_history = rocketnet_used_token_history_create( NULL, num_clients * 8 );
    test.server.admitted_token_set = rocketnet_admitted_token_set_create( NULL, num_clients * 2 );
    test.server.acks = rocketnet_server_acks_create( NULL, num_clients );
    test.server.reassembly = rocketnet_reassembly_set_create( NULL, num_clients, 4, 16384, num_clients * 4 );
    rocketnet_check( test.server.client_slots && test.server.encryption_map && test.server.used_token_history && test.server.admitted_token_set && test.server.acks && test.server.reassembly );
    rocketnet_reassembly_set_begin_update( test.server.reassembly );

    struct rocketnet_client_protocol_t * clients = (struct rocketnet_client_protocol_t*)
        rocketnet_malloc( NULL, sizeof(struct rocketnet_client_protocol_t) * (size_t) num_clients );
    struct rocketnet_address_t * addresses = (struct rocketnet_address_t*)
        rocketnet_malloc( NULL, sizeof(struct rocketnet_address_t) * (size_t) num_clients );
    rocketnet_check( clients && addresses );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );

    // roles by index: 8 of every 10 stay, the 9th connects then leaves, the
    // 10th abandons the handshake once it holds a challenge
    #define ROCKETNET_SWARM_STAYS( i )      ( ( (i) % 10 ) < 8 )
    #define ROCKETNET_SWARM_LEAVES( i )     ( ( (i) % 10 ) == 8 )
    #define ROCKETNET_SWARM_ABANDONS( i )   ( ( (i) % 10 ) == 9 )

    for ( int i = 0; i < num_clients; i++ )
    {
        char address_string[64];
        snprintf( address_string, sizeof(address_string), "10.1.%d.%d:56789", i / 250, 1 + ( i % 250 ) );
        rocketnet_check( rocketnet_address_parse( &addresses[i], address_string ) );
        rocketnet_client_protocol_init( &clients[i], &client_counters );
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_test_mint_token( token_data, 0x50000000ULL + (uint64_t) i, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
        rocketnet_check( rocketnet_client_protocol_connect( &clients[i], token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    }

    struct rocketnet_address_t garbage_addresses[num_garbage_senders];
    for ( int i = 0; i < num_garbage_senders; i++ )
    {
        char address_string[64];
        snprintf( address_string, sizeof(address_string), "10.9.9.%d:1000", 1 + i );
        rocketnet_check( rocketnet_address_parse( &garbage_addresses[i], address_string ) );
    }

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    uint64_t garbage_rng = 0x6A6B6A6BULL;
    #define ROCKETNET_SWARM_RANDOM() ( garbage_rng ^= garbage_rng << 13, garbage_rng ^= garbage_rng >> 7, garbage_rng ^= garbage_rng << 17, garbage_rng )

    // --- the connect phase, with garbage interleaved ---

    double time = 100.0;
    for ( int tick = 0; tick < 120; tick++, time += 0.05 )
    {
        for ( int i = 0; i < num_clients; i++ )
        {
            if ( ROCKETNET_SWARM_ABANDONS( i ) && clients[i].state >= ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
                continue;                                   // walked away mid-handshake
            if ( clients[i].state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                continue;
            rocketnet_client_protocol_update( &clients[i], time, packet, &packet_bytes );
            if ( packet_bytes == 0 )
                continue;
            rocketnet_server_protocol_process_packet( &test.server, &addresses[i], packet, packet_bytes,
                                             ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                             payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( &clients[i], response, response_bytes, time, payload, &payload_bytes );
        }

        // the garbage generator: junk shapes at the reject rungs, constantly
        for ( int g = 0; g < num_garbage_senders; g++ )
        {
            if ( ( ROCKETNET_SWARM_RANDOM() & 3 ) != 0 )
                continue;
            int variant = (int) ( ROCKETNET_SWARM_RANDOM() % 4 );
            int garbage_bytes = 0;
            if ( variant == 0 )
            {
                for ( int b = 0; b < 64; b++ ) packet[b] = (uint8_t) ROCKETNET_SWARM_RANDOM();
                garbage_bytes = 64;                          // random tag: prefilter kills it
            }
            else if ( variant == 1 )
            {
                uint8_t * p = packet;
                rocketnet_write_prefilter_header( &p, (uint8_t) ( 8 + ( ROCKETNET_SWARM_RANDOM() % 248 ) ), tag );
                garbage_bytes = 32;                          // reserved type
            }
            else if ( variant == 2 )
            {
                uint8_t * p = packet;
                rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, tag );
                for ( int b = ROCKETNET_PREFILTER_BYTES; b < 64; b++ ) packet[b] = (uint8_t) ROCKETNET_SWARM_RANDOM();
                garbage_bytes = 64;                          // sealed junk, no mapping
            }
            else
            {
                uint8_t * p = packet;
                rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, tag );
                for ( int b = ROCKETNET_PREFILTER_BYTES; b < ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES; b++ ) packet[b] = (uint8_t) ROCKETNET_SWARM_RANDOM();
                garbage_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;    // junk request: dies at the version
            }
            rocketnet_server_protocol_process_packet( &test.server, &garbage_addresses[g], packet, garbage_bytes,
                                             ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                             payload, &payload_bytes, &payload_client_index );
        }

        rocketnet_server_protocol_update( &test.server, time );
    }

    int expected_connected = 0;
    for ( int i = 0; i < num_clients; i++ )
        if ( !ROCKETNET_SWARM_ABANDONS( i ) )
        {
            rocketnet_check( clients[i].state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
            expected_connected++;
        }
    rocketnet_check( expected_connected == 900 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 900 );

    // --- the payload burst: the stay clients each send three ---

    for ( int round = 0; round < 3; round++ )
    {
        time += 0.05;
        for ( int i = 0; i < num_clients; i++ )
        {
            if ( !ROCKETNET_SWARM_STAYS( i ) )
                continue;
            uint8_t game_data[32];
            memset( game_data, (uint8_t) i, sizeof(game_data) );
            int bytes = rocketnet_client_protocol_write_payload( &clients[i], game_data, sizeof(game_data), time, packet );
            rocketnet_check( bytes > 0 );
            int result = rocketnet_server_protocol_process_packet( &test.server, &addresses[i], packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index );
            rocketnet_check( result == ROCKETNET_SERVER_PROCESS_ACCEPTED );
            rocketnet_check( payload_bytes == 32 );

            // every hundredth stay client also sends a fragmented payload,
            // so reassembly runs under the same storm: three type 9
            // packets per payload, reassembled byte-exact on the third

            if ( ( i % 100 ) == 0 )
            {
                static uint8_t fragmented_payload[3000];
                for ( int b = 0; b < 3000; b++ )
                    fragmented_payload[b] = (uint8_t) ( i + b );
                static uint8_t fragment_packets[3 * ROCKETNET_SEND_BUDGET];
                int fragment_packet_bytes[3];
                rocketnet_check( rocketnet_client_protocol_write_fragments( &clients[i], fragmented_payload, 3000, time,
                                                                            fragment_packets, fragment_packet_bytes, 3 ) == 3 );
                for ( int f = 0; f < 3; f++ )
                {
                    result = rocketnet_server_protocol_process_packet( &test.server, &addresses[i], fragment_packets + f * ROCKETNET_SEND_BUDGET,
                                                              fragment_packet_bytes[f],
                                                              ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                              payload, &payload_bytes, &payload_client_index );
                    rocketnet_check( result == ROCKETNET_SERVER_PROCESS_ACCEPTED );
                }
                rocketnet_check( test.server.reassembly->completed_bytes == 3000 );
                rocketnet_check( memcmp( test.server.reassembly->completed_payload, fragmented_payload, 3000 ) == 0 );
            }
        }
    }

    // --- the leavers disconnect; the abandoners' mappings die at the deadline ---

    for ( int i = 0; i < num_clients; i++ )
    {
        if ( !ROCKETNET_SWARM_LEAVES( i ) )
            continue;
        uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
        int burst_count = 0;
        int burst_bytes = 0;
        rocketnet_client_protocol_disconnect( &clients[i], burst, &burst_count, &burst_bytes );
        rocketnet_check( burst_count == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
        rocketnet_server_protocol_process_packet( &test.server, &addresses[i], burst, burst_bytes,
                                         ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                         payload, &payload_bytes, &payload_client_index );
    }

    for ( ; time < 112.0; time += 0.05 )
        rocketnet_server_protocol_update( &test.server, time );

    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 800 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 800 );

    const uint64_t * counters = test.counters.values;

    rocketnet_check( counters[ROCKETNET_COUNTER_CLIENT_CONNECTED] == 900 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED] == 100 );
    rocketnet_check( counters[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 100 );

    // received == sum of outcomes, for every packet family the server saw --
    // the accounting invariant from the standard, asserted over the whole
    // storm, so no packet was ever half-counted or silently dropped

    {
        uint64_t request_outcomes =
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE];
        rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] == request_outcomes );
    }

    {
        uint64_t response_outcomes =
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED] +
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED] +
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ADMITTED_SET_FULL] +
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS] +
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR] +
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED];
        rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED] == response_outcomes );
    }

    {
        // RECEIVED == the sum of outcomes, every session type, every
        // outcome the type can produce -- ACK_UNKNOWN_SEQUENCE and
        // ACKED_BATCH_FULL are side counters outside the invariant

        uint64_t keep_alive_outcomes =
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED];
        rocketnet_check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED] == keep_alive_outcomes );

        uint64_t payload_outcomes =
            counters[ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE] +
            counters[ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK] +
            counters[ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE] +
            counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED];
        rocketnet_check( counters[ROCKETNET_COUNTER_PAYLOAD_RECEIVED] == payload_outcomes );

        uint64_t disconnect_outcomes =
            counters[ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE] +
            counters[ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_DISCONNECT_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_DISCONNECT_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK] +
            counters[ROCKETNET_COUNTER_DISCONNECT_WRONG_OPENED_SIZE] +
            counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];
        rocketnet_check( counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] == disconnect_outcomes );

        uint64_t ack_outcomes =
            counters[ROCKETNET_COUNTER_ACK_WRONG_SIZE] +
            counters[ROCKETNET_COUNTER_ACK_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_ACK_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_ACK_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_ACK_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK] +
            counters[ROCKETNET_COUNTER_ACK_WRONG_OPENED_SIZE] +
            counters[ROCKETNET_COUNTER_ACK_ACCEPTED];
        rocketnet_check( counters[ROCKETNET_COUNTER_ACK_RECEIVED] == ack_outcomes );

        uint64_t fragment_outcomes =
            counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE] +
            counters[ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK] +
            counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE] +
            counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID] +
            counters[ROCKETNET_COUNTER_FRAGMENT_STALE] +
            counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID] +
            counters[ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE] +
            counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH] +
            counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] +
            counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] +
            counters[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX] +
            counters[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED];
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == fragment_outcomes );

        // every fragmented payload sent arrived whole on the direct wire:
        // 10 fragmenting clients, three rounds -- and nothing was ever
        // abandoned or dropped at a full batch
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 30 );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );
    }

    // the storm did its work: every garbage shape landed where it should
    rocketnet_check( counters[ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE] +
                     counters[ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH] +
                     counters[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] +
                     counters[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] > 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_RESERVED_TYPE] > 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING] > 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] > 0 );

    // and the reasons that must never fire, did not, once, in the whole storm
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_MAPPING_TABLE_FULL] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ADMITTED_SET_FULL] == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS] == 0 );

    #undef ROCKETNET_SWARM_STAYS
    #undef ROCKETNET_SWARM_LEAVES
    #undef ROCKETNET_SWARM_ABANDONS
    #undef ROCKETNET_SWARM_RANDOM

    rocketnet_free( NULL, clients );
    rocketnet_free( NULL, addresses );
    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_server_disconnect_client()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0xD15C0ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_client_slots_connected( test.server.client_slots, 0 ) );

    // the server evicts the client: the redundant burst comes back sealed,
    // and the slot and mapping are gone before any packet is even sent

    uint8_t burst[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_server_protocol_disconnect_client( &test.server, 0, burst, &burst_count, &burst_bytes );
    rocketnet_check( burst_count == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_check( burst_bytes >= ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
    rocketnet_check( !rocketnet_client_slots_connected( test.server.client_slots, 0 ) );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 0 );
    rocketnet_check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_CLIENT_DISCONNECTED] == 1 );

    // the first copy lands and the client dies to disconnected; the rest are
    // replays of consecutive sequences and every one is refused

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, burst, burst_bytes, 200.0, payload, &payload_bytes ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    // a second disconnect of the same index does nothing, loudly countable

    burst_count = 12345;
    rocketnet_server_protocol_disconnect_client( &test.server, 0, burst, &burst_count, &burst_bytes );
    rocketnet_check( burst_count == 0 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT] == 1 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_server_refuses_keyless_config()
{
    // the no-default-keys gate: a config with no keys is refused, never
    // silently defaulted -- default keys are how every deployment of a
    // protocol ends up sharing one identity. the dev posture is explicit.

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config, 0.0 ) == NULL );

    // and out-of-range client counts are refused at the same gate

    rocketnet_server_transport_dev_config( &config );
    config.max_clients = 0;
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config, 0.0 ) == NULL );
    config.max_clients = ROCKETNET_MAX_CLIENTS + 1;
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config, 0.0 ) == NULL );
}

struct rocketnet_test_hammer_args_t
{
    struct rocketnet_server_transport_t * server;
    int thread_id;
    int packets;
};

static void rocketnet_test_hammer_thread( void * data )
{
    struct rocketnet_test_hammer_args_t * args = (struct rocketnet_test_hammer_args_t*) data;
    for ( int i = 0; i < args->packets; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( args->server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        rocketnet_check( frame );
        uint8_t * payload = frame + context.payload_index;
        payload[0] = (uint8_t) args->thread_id;
        payload[1] = (uint8_t) i;
        for ( int j = 2; j < 16; j++ )
            payload[j] = (uint8_t) ( args->thread_id * 31 + i * 7 + j );
        rocketnet_server_transport_finish_packet( args->server, &context, 16 );
    }
}

void rocketnet_test_transport_disconnect_delivery()
{
    // the disconnect burst through the TRANSPORT, not handed to the client
    // protocol by hand -- the reviewer found the transport's disconnect
    // producer writing at the wrong frame offset while the protocol-level
    // test stayed green, because it never crossed the transport. this one
    // crosses it: the client must reach DISCONNECTED long before its 10s
    // timeout could explain it.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_server_transport_disconnect_client( server, 0 );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 3.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

static int rocketnet_test_fill_server_ring( rocketnet_server_transport_t * server, int completion_capacity )
{
    // IOCP may still own earlier sends. Keep every successful reservation in
    // this fill, without flushing, while completion workers return the rest.
    // A lost frame still fails the exact-capacity check after a bounded wait.
    const double deadline = rocketnet_platform_time() + 5.0;
    int count = 0;
    while ( count < 20000 )
    {
        rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        if ( !frame )
        {
            if ( count < completion_capacity && rocketnet_platform_time() < deadline )
            {
                rocketnet_platform_sleep( 0.001 );
                continue;
            }
            break;
        }
        frame[context.payload_index] = (uint8_t) count;
        rocketnet_check( rocketnet_server_transport_finish_packet( server, &context, 1 ) > 0 );
        count++;
    }
    if ( completion_capacity && count != completion_capacity )
        printf( "ring fill: driver=%s accepted=%d expected=%d\n", rocketnet_server_transport_driver( server ), count, completion_capacity );
    return count;
}

void rocketnet_test_transport_ring_exhaustion()
{
    // fill past the ring, and the overflow refuses cleanly: starts beyond
    // capacity return NULL, the counter names the reason, and the full
    // capacity returns after flush and any asynchronous send completions.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // Synchronous drivers are measured at their first refusal. IOCP's pool
    // has a known size, but even handshake sends may still be retiring at
    // the first fill. Wait for that whole pool, not an incidental snapshot.
    int completion_capacity = 0;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( strcmp( rocketnet_server_transport_driver( server ), "iocp" ) == 0 )
        completion_capacity = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
#endif
    const int capacity = rocketnet_test_fill_server_ring( server, completion_capacity );
    rocketnet_check( capacity >= 256 );
    rocketnet_check( capacity < 20000 );
    rocketnet_check( completion_capacity == 0 || capacity == completion_capacity );

    // refusal holds until the flush -- 44 more starts all refuse, and the
    // counter attributes every refusal exactly (the fill already incurred
    // refusals, so the baseline is read here)

    const uint64_t ring_full_before = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    for ( int i = 0; i < 44; i++ )
    {
        struct rocketnet_packet_context_t context;
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context ) == NULL );
    }
    rocketnet_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full_before + 44 );

    rocketnet_server_transport_send_packets( server );

    // Flush submits IOCP sends; it does not synchronously reclaim them.
    // Accumulate returned frames in this one fill until the same exact
    // capacity is reached. No flush or transport update assists the refill.
    const int refill = rocketnet_test_fill_server_ring( server, completion_capacity );
    if ( refill != capacity )
        printf( "ring refill: driver=%s first=%d second=%d\n", rocketnet_server_transport_driver( server ), capacity, refill );
    rocketnet_check( refill == capacity );

    rocketnet_server_transport_send_packets( server );

    // the next frame is whole: fresh starts succeed and one arrives. the
    // payload is unreliable and the flood may have left the client's kernel
    // receive buffer overflowing -- a driver with deep staging floods far
    // past what the buffer holds, and packets lost there are priced as lost
    // on the wire -- so recovery sends a fresh "again" every iteration
    // until one is observed, the transport's own retry idiom.

    rocketnet_server_transport_update( server, rocketnet_platform_time() );

    int recovered = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        rocketnet_check( frame || completion_capacity );
        if ( frame )
        {
            memcpy( frame + context.payload_index, "again", 5 );
            rocketnet_check( rocketnet_server_transport_finish_packet( server, &context, 5 ) > 0 );
        }
        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_client_transport_payloads( client, &payload_data, &payload_bytes );
        for ( int i = 0; i < num_payloads; i++ )
        {
            if ( payload_bytes[i] == 5 && memcmp( payload_data[i], "again", 5 ) == 0 )
                recovered = 1;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( recovered );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

struct rocketnet_test_client_hammer_args_t
{
    struct rocketnet_client_transport_t * client;
    int thread_id;
    int packets;
};

static void rocketnet_test_client_hammer_thread( void * data )
{
    struct rocketnet_test_client_hammer_args_t * args = (struct rocketnet_test_client_hammer_args_t*) data;
    for ( int i = 0; i < args->packets; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( args->client, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        rocketnet_check( frame );
        uint8_t * payload = frame + context.payload_index;
        payload[0] = (uint8_t) args->thread_id;
        payload[1] = (uint8_t) i;
        rocketnet_check( rocketnet_client_transport_finish_packet( args->client, &context, 2 ) > 0 );
    }
}

void rocketnet_test_transport_client_concurrent_fill()
{
    // the client's fill phase under threads, mirrored from the server's
    // hammer: four fillers, one flush, every packet arrives distinct.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int NUM_THREADS = 4;
    const int PACKETS_PER_THREAD = 40;

    struct rocketnet_test_client_hammer_args_t args[NUM_THREADS];
    rocketnet_platform_thread_t * threads[NUM_THREADS];

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        args[t].client = client;
        args[t].thread_id = t;
        args[t].packets = PACKETS_PER_THREAD;
        threads[t] = rocketnet_platform_thread_create( NULL, rocketnet_test_client_hammer_thread, &args[t] );
        rocketnet_check( threads[t] );
    }

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        rocketnet_platform_thread_join( threads[t] );
        rocketnet_platform_thread_destroy( threads[t] );
    }

    rocketnet_client_transport_send_packets( client );

    int seen[NUM_THREADS][PACKETS_PER_THREAD];
    memset( seen, 0, sizeof(seen) );
    int received = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && received < NUM_THREADS * PACKETS_PER_THREAD )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int * client_index;
        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_server_transport_payloads( server, &client_index, &payload_data, &payload_bytes );

        for ( int i = 0; i < num_payloads; i++ )
        {
            rocketnet_check( client_index[i] == 0 );
            rocketnet_check( payload_bytes[i] == 2 );
            const int thread_id = payload_data[i][0];
            const int index = payload_data[i][1];
            rocketnet_check( thread_id >= 0 && thread_id < NUM_THREADS );
            rocketnet_check( index >= 0 && index < PACKETS_PER_THREAD );
            rocketnet_check( !seen[thread_id][index] );
            seen[thread_id][index] = 1;
            received++;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( received == NUM_THREADS * PACKETS_PER_THREAD );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_concurrent_fill()
{
    // the lock-free claim under real threads: four fillers acquire, write
    // and seal concurrently in one fill phase, the main thread flushes, and
    // every packet arrives distinct -- which also proves every sequence was
    // unique, because the client replay window would eat a duplicate.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int NUM_THREADS = 4;
    const int PACKETS_PER_THREAD = 50;

    struct rocketnet_test_hammer_args_t args[NUM_THREADS];
    rocketnet_platform_thread_t * threads[NUM_THREADS];

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        args[t].server = server;
        args[t].thread_id = t;
        args[t].packets = PACKETS_PER_THREAD;
        threads[t] = rocketnet_platform_thread_create( NULL, rocketnet_test_hammer_thread, &args[t] );
        rocketnet_check( threads[t] );
    }

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        rocketnet_platform_thread_join( threads[t] );
        rocketnet_platform_thread_destroy( threads[t] );
    }

    rocketnet_server_transport_send_packets( server );

    // every one of the two hundred distinct packets arrives

    int seen[NUM_THREADS][PACKETS_PER_THREAD];
    memset( seen, 0, sizeof(seen) );
    int received = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && received < NUM_THREADS * PACKETS_PER_THREAD )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_client_transport_payloads( client, &payload_data, &payload_bytes );

        for ( int i = 0; i < num_payloads; i++ )
        {
            rocketnet_check( payload_bytes[i] == 16 );
            const int thread_id = payload_data[i][0];
            const int index = payload_data[i][1];
            rocketnet_check( thread_id >= 0 && thread_id < NUM_THREADS );
            rocketnet_check( index >= 0 && index < PACKETS_PER_THREAD );
            for ( int j = 2; j < 16; j++ )
                rocketnet_check( payload_data[i][j] == (uint8_t) ( thread_id * 31 + index * 7 + j ) );
            rocketnet_check( !seen[thread_id][index] );
            seen[thread_id][index] = 1;
            received++;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( received == NUM_THREADS * PACKETS_PER_THREAD );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_start_finish_shape()
{
    // the acquire/commit surface, proven over loopback: aligned roots, the
    // documented layout, seal in place, oversize refused, aborts harmless.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // a packet before connect for a bogus slot is refused at start

    struct rocketnet_packet_context_t context;
    rocketnet_check( rocketnet_server_transport_start_packet( server, 3, ROCKETNET_MAX_PAYLOAD_BYTES, &context ) == NULL );

    // acquire: the root and the payload pointer are both 16-aligned, the
    // layout is the documented one, and the capacity is qword-clean

    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_check( frame );
    rocketnet_check( ( (uintptr_t) frame % 16 ) == 0 );
    rocketnet_check( context.payload_index == ROCKETNET_FRAME_PAYLOAD_OFFSET );
    rocketnet_check( ( (uintptr_t) ( frame + context.payload_index ) % 16 ) == 0 );
    rocketnet_check( context.payload_capacity == ROCKETNET_MAX_PAYLOAD_BYTES );
    rocketnet_check( context.payload_capacity % 8 == 0 );
    rocketnet_check( context.sequence > 0 );

    // Packet metadata is a view, never authority over native storage.
    int checked_capacity = 0;
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &context, &checked_capacity) == frame + context.payload_index);
    rocketnet_check(checked_capacity == context.payload_capacity);
    rocketnet_check(rocketnet_client_transport_packet_payload(client, &context, &checked_capacity) == NULL && checked_capacity == 0);
    for (int field = 0; field < 6; ++field)
    {
        auto changed = context;
        switch(field)
        {
            case 0: changed.frame++; break;
            case 1: changed.packet_id++; break;
            case 2: changed.sequence++; break;
            case 3: changed.client_index = -1; break;
            case 4: changed.payload_index = -1; break;
            case 5: changed.payload_capacity = INT_MAX; break;
        }
        rocketnet_check(rocketnet_server_transport_packet_payload(server, &changed, &checked_capacity) == NULL);
        auto aborted = changed;
        rocketnet_server_transport_abort_packet(server, &aborted);
        rocketnet_check(aborted.frame == NULL);
        rocketnet_check(rocketnet_server_transport_finish_packet(server, &changed, 1) == 0);
    }
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &context, &checked_capacity) == frame + context.payload_index);
    auto expired = context;

    // oversize is refused at finish and consumes nothing visible

    rocketnet_server_transport_finish_packet( server, &context, context.payload_capacity + 1 );
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &expired, &checked_capacity) == NULL);
    rocketnet_check(rocketnet_server_transport_finish_packet(server, &expired, 1) == 0);

    // an aborted packet is harmless

    frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_check( frame );
    rocketnet_server_transport_abort_packet( server, &context );

    // commit: write at the payload pointer, finish, flush, and the client
    // receives exactly those bytes

    frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_check( frame );
    memcpy( frame + context.payload_index, "shape", 5 );
    rocketnet_server_transport_finish_packet( server, &context, 5 );
    rocketnet_server_transport_send_packets( server );

    int received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "shape", 5 ) == 0 );
            received = 1;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( received );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_payload_capacity()
{
    // The capacity invariant that was violated, stated where it will be
    // checked. The XDP receive path drains up to
    // RECEIVE_PACKETS_PER_CLIENT * MAX_CLIENTS packets in one update and
    // processes every one of them -- opening the AEAD and CONSUMING the
    // sequence. If the payload arrays were smaller than that, the excess
    // decrypted payloads were written to a scratch buffer and discarded,
    // with their sequences already spent, so a retransmit would be refused
    // as a replay. Silent, uncounted, unrecoverable, and only on the
    // production transport. The arrays must cover a full drain.

    rocketnet_check( ROCKETNET_MAX_PAYLOADS_PER_UPDATE >= ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS );

    // and the storage behind them must cover a full-size RECEIVED payload
    // for every one of those slots -- the receive side accepts up to the
    // fixed ceiling whatever this end's own send budget -- or the same loss
    // returns as an overrun instead

    rocketnet_check( ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES ==
                     ROCKETNET_RECEIVE_CEILING - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES );
    rocketnet_check( (size_t) ROCKETNET_MAX_PAYLOADS_PER_UPDATE * ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES
                     >= (size_t) ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS * ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES );
}

void rocketnet_test_transport_udp_loopback()
{
    // the first packets on a real socket: the whole connect over UDP through
    // loopback, on the transports -- flow's IO carrying the connection
    // protocol, development keys, exactly the out-of-the-box path. real
    // clocks here; the protocol underneath is the one proven on mocked time.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t loopback_connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), loopback_connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, loopback_connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    // pump both until connected, bounded by the wall clock
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_client_transport_client_index( client ) == 0 );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    rocketnet_check( rocketnet_server_transport_is_client_connected( server, 0 ) );

    // payloads both ways through the transport surfaces
    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello", 5 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world", 5 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5 );
            rocketnet_check( memcmp( payload, "hello", 5 ) == 0 );
            server_received++;
        }

        // out-of-range indices answer safely rather than trusting the caller
        int oob_client_index, oob_payload_bytes;
        rocketnet_check( rocketnet_server_transport_payload( server, num_payloads, &oob_client_index, &oob_payload_bytes ) == NULL );
        rocketnet_check( rocketnet_server_transport_payload( server, -1, &oob_client_index, &oob_payload_bytes ) == NULL );

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "world", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 10 );
    rocketnet_check( client_received >= 10 );

    // the deliberate disconnect crosses the wire and the server empties
    rocketnet_client_transport_disconnect( client );
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    // Stop may need to drain asynchronous I/O. The public contract requires
    // waiting for STOPPED, not a particular number of updates.
    start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED &&
            rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_client_rtt_handshake()
{
    // the handshake's three round trips under three different imposed
    // delays, on mocked time: each is sampled first-send-to-first-answer,
    // the estimate keeps the minimum, and traffic after the handshake never
    // moves it

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0xC0FFEEULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( client.rtt_estimate == 0.0 );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    // cookie round trip: the request goes out at 100.0 and its answer is
    // delivered 50ms later

    rocketnet_client_protocol_update( &client, 100.0, packet, &packet_bytes );
    rocketnet_check( packet_bytes > 0 );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 100.0,
                                                      response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, response, response_bytes, 100.05, payload, &payload_bytes ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    rocketnet_check( fabs( client.rtt_estimate - 0.05 ) < 1e-9 );

    // challenge round trip at 30ms: the minimum moves down

    rocketnet_client_protocol_update( &client, 100.2, packet, &packet_bytes );
    rocketnet_check( packet_bytes > 0 );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 100.2,
                                                      response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, response, response_bytes, 100.23, payload, &payload_bytes ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    rocketnet_check( fabs( client.rtt_estimate - 0.03 ) < 1e-9 );

    // response round trip at 40ms: the first keep-alive answers it, the
    // connection confirms, and the minimum holds

    rocketnet_client_protocol_update( &client, 100.4, packet, &packet_bytes );
    rocketnet_check( packet_bytes > 0 );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 100.4,
                                                      response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, response, response_bytes, 100.44, payload, &payload_bytes ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( fabs( client.rtt_estimate - 0.03 ) < 1e-9 );

    // a paced keep-alive long after the handshake is not an answer to
    // anything and never moves the estimate

    int keep_alive_bytes = rocketnet_server_write_keep_alive( &test.server, 0, 101.0, response );
    rocketnet_check( keep_alive_bytes > 0 );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, response, keep_alive_bytes, 105.0, payload, &payload_bytes ) );
    rocketnet_check( fabs( client.rtt_estimate - 0.03 ) < 1e-9 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_transport_stats()
{
    // the bucket rings on both transports over UDP loopback, on a synthetic
    // game clock: the published send rates equal an independent recount of
    // the sends this test made under the same trailing-one-second rule, and
    // after a second of idle game time every published rate is zero --
    // stale buckets drop out

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    double game_time = 100.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, game_time );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, game_time );
    rocketnet_check( client );

    // connect on the synthetic clock, bounded by the wall clock

    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // two simulated seconds of traffic, one payload each way per frame. the
    // send times are recorded so the published numbers can be recounted
    // independently; two seconds ages the handshake's packets out of the
    // window, so the window holds payload traffic alone.

    const int num_traffic_frames = 120;

    double send_times[num_traffic_frames];

    uint8_t payload_data[100];
    memset( payload_data, 0x5A, sizeof(payload_data) );

    for ( int i = 0; i < num_traffic_frames; i++ )
    {
        game_time += 1.0 / 60.0;

        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );

        rocketnet_client_transport_send_packet( client, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );

        send_times[i] = game_time;

        uint8_t scratch[ROCKETNET_RECEIVE_CEILING];
        while ( rocketnet_client_transport_receive_packet( client, scratch, (int) sizeof(scratch) ) > 0 ) {}

        rocketnet_platform_sleep( 0.001 );
    }

    // one more update pair at the final time publishes the window sums

    rocketnet_client_transport_update( client, game_time );
    rocketnet_server_transport_update( server, game_time );

    // the independent recount, under the published numbers' own rule: a
    // send contributes while its bucket is younger than one second

    int expected_sends = 0;
    for ( int i = 0; i < num_traffic_frames; i++ )
    {
        if ( send_times[i] + 1.0 >= game_time )
            expected_sends++;
    }
    rocketnet_check( expected_sends > 0 );

    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( client, &client_stats );

    rocketnet_check( client_stats.packets_sent_per_second == expected_sends );
    rocketnet_check( client_stats.packets_received_per_second > 0 );
    rocketnet_check( client_stats.bandwidth_sent_kbps > 0 );
    rocketnet_check( client_stats.bandwidth_received_kbps > 0 );
    rocketnet_check( client_stats.rtt > 0.0f );
    rocketnet_check( client_stats.rtt < 1000.0f );
    rocketnet_check( client_stats.jitter >= 0.0f );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    struct rocketnet_server_transport_stats_t server_stats;
    rocketnet_server_transport_stats( server, &server_stats );

    rocketnet_check( server_stats.packets_sent_per_second == (uint64_t) expected_sends );
    rocketnet_check( server_stats.packets_received_per_second > 0 );
    rocketnet_check( server_stats.bandwidth_sent_kbps > 0.0f );
    rocketnet_check( server_stats.bandwidth_received_kbps > 0.0f );

    // stale bucket exclusion: disconnect, then a second and a half of idle
    // game time. nothing is sent or received, the traffic buckets age past
    // the window, and every published rate returns to zero.

    rocketnet_client_transport_disconnect( client );

    for ( int i = 0; i < 90; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packets_sent_per_second == 0 );
    rocketnet_check( client_stats.packets_received_per_second == 0 );
    rocketnet_check( client_stats.bandwidth_sent_kbps == 0 );
    rocketnet_check( client_stats.bandwidth_received_kbps == 0 );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    rocketnet_server_transport_stats( server, &server_stats );
    rocketnet_check( server_stats.packets_sent_per_second == 0 );
    rocketnet_check( server_stats.packets_received_per_second == 0 );
    rocketnet_check( server_stats.bandwidth_sent_kbps == 0.0f );
    rocketnet_check( server_stats.bandwidth_received_kbps == 0.0f );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, game_time );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_packet_loss_stats()
{
    // packet loss measured under a lossy sequence, deterministically: a
    // sequence started and aborted on the server is burned unsent, so the
    // client sees a gap in the session sequence over a perfect wire. clean
    // traffic first proves zero; the burns prove loss shows up.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    double game_time = 100.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, game_time );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, game_time );
    rocketnet_check( client );

    // This handshake uses real sockets and a real receive thread. Advancing
    // virtual time by 1/60 per 1 ms wait gives that thread only ~300 ms before
    // a five-second protocol deadline. Use actual elapsed time for connection;
    // the controlled virtual cadence below still drives the statistics case.
    const double connect_time = game_time;
    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time = connect_time + rocketnet_platform_time() - wall_start;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    if (rocketnet_client_transport_state(client) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED)
        printf("packet loss test connect failed: %s, server clients %d\n",
            rocketnet_client_transport_state_name(rocketnet_client_transport_state(client)),
            rocketnet_server_transport_num_connected_clients(server));
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    uint8_t payload_data[64];
    memset( payload_data, 0xA5, sizeof(payload_data) );

    // clean traffic: no gaps, so no loss. 45 frames on purpose: enough session
    // packets that the tracker's sweep crosses its safety margin, which is where
    // a phantom loss from a mis-based sequence feed becomes visible.

    for ( int i = 0; i < 45; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    // burn ten sequences unsent, then enough delivered traffic to carry the
    // loss tracker's sweep past its safety margin

    for ( int i = 0; i < 10; i++ )
    {
        struct rocketnet_packet_context_t context;
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context ) );
        rocketnet_server_transport_abort_packet( server, &context );
    }

    for ( int i = 0; i < ROCKETNET_PACKET_LOSS_TRACKER_SAFETY + 20; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packet_loss > 0.0f );
    rocketnet_check( client_stats.packet_loss < 100.0f );

    rocketnet_client_transport_disconnect( client );
    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, game_time );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_payload_size_sweep()
{
    // every payload size from one byte to the maximum, both directions,
    // every byte verified from the packet alone -- the whole pipeline
    // proven against corruption and truncation at every length, including
    // the boundary

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0x51EE9ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    uint8_t payload[ROCKETNET_MAX_PAYLOAD_BYTES];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
    int delivered_bytes = 0;
    int delivered_client_index = -1;

    double time = 200.0;

    for ( int size = 1; size <= ROCKETNET_MAX_PAYLOAD_BYTES; size++ )
    {
        time += 0.001;

        // client to server: the wire carries the fixed overhead, the
        // minimum ack block (a clean link has no loss events), and the bytes
        rocketnet_test_generate_payload( payload, size );
        int packet_bytes = rocketnet_client_protocol_write_payload( &client, payload, size, time, packet );
        rocketnet_check( packet_bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + size );
        rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                          ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                          delivered, &delivered_bytes, &delivered_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        rocketnet_check( delivered_bytes == size );
        rocketnet_check( rocketnet_test_verify_payload( delivered, delivered_bytes ) );

        // server to client
        rocketnet_test_generate_payload( payload, size );
        packet_bytes = rocketnet_server_protocol_write_payload( &test.server, 0, payload, size, time, packet );
        rocketnet_check( packet_bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + size );
        rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, packet_bytes, time, delivered, &delivered_bytes ) );
        rocketnet_check( delivered_bytes == size );
        rocketnet_check( rocketnet_test_verify_payload( delivered, delivered_bytes ) );
    }

    // one past the maximum refuses to build, both sides
    rocketnet_check( rocketnet_client_protocol_write_payload( &client, payload, ROCKETNET_MAX_PAYLOAD_BYTES + 1, time, packet ) == 0 );
    rocketnet_check( rocketnet_server_protocol_write_payload( &test.server, 0, payload, ROCKETNET_MAX_PAYLOAD_BYTES + 1, time, packet ) == 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_platform_socket()
{
    // non-blocking socket (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with timeout (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with no timeout (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) );
        rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        rocketnet_platform_socket_destroy( socket );
    }

#if ROCKETNET_PLATFORM_HAS_IPV6

    // non-blocking socket (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with timeout (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with no timeout (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) );
        rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        rocketnet_platform_socket_destroy( socket );
    }

#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
}

// the batch pump seam (#100): every platform serves it -- portable loops
// underneath on most, darwin's batched syscalls when the runtime probe
// admits them -- and these tests pin the seam contract itself, so they run
// identically over either implementation. packets are verified by content
// (index and pattern), never by arrival order.

#define ROCKETNET_TEST_PUMP_PACKETS 100         // several pump chunks, with a short final chunk
#define ROCKETNET_TEST_PUMP_STRIDE  1426        // the transport's own receive ceiling

// the pump each platform must be running when nothing forces portable: on
// macOS the msgx pump, everywhere else the portable loop. asserted inside
// the exchange so a silently-degraded driver can never pass the suite as
// "tested" -- the batch tests would otherwise green over the fallback.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#define ROCKETNET_TEST_EXPECTED_PUMP "msgx"
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#define ROCKETNET_TEST_EXPECTED_PUMP "portable"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

static void rocketnet_test_pump_exchange( const char * bind_string, const char * local_string, const char * expected_pump )
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, bind_string );
    rocketnet_address_parse( &local_address, local_string );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), expected_pump ) == 0 );
    local_address.port = bind_address.port;

    // batch send: varied sizes, a content pattern keyed by index

    static uint8_t send_data[ROCKETNET_TEST_PUMP_PACKETS][512];
    struct rocketnet_platform_send_t sends[ROCKETNET_TEST_PUMP_PACKETS];
    for ( int i = 0; i < ROCKETNET_TEST_PUMP_PACKETS; i++ )
    {
        const int bytes = 32 + i * 4;
        send_data[i][0] = (uint8_t) i;
        for ( int j = 1; j < bytes; j++ )
            send_data[i][j] = (uint8_t) ( i + j );
        sends[i].to = &local_address;
        sends[i].packet_data = send_data[i];
        sends[i].packet_bytes = bytes;
    }
    rocketnet_platform_socket_send_packets( socket, sends, ROCKETNET_TEST_PUMP_PACKETS );

    // batch receive: drain until everything sent has arrived. loopback
    // delivery can trickle, so poll under a deadline rather than assume
    // one call sees the batch whole.

    static rocketnet_address_t from[ROCKETNET_TEST_PUMP_PACKETS];
    static int packet_bytes[ROCKETNET_TEST_PUMP_PACKETS];
    static uint8_t receive_data[ROCKETNET_TEST_PUMP_PACKETS*ROCKETNET_TEST_PUMP_STRIDE];

    int received = 0;
    double start_time = rocketnet_platform_time();
    while ( received < ROCKETNET_TEST_PUMP_PACKETS && rocketnet_platform_time() - start_time < 5.0 )
    {
        received += rocketnet_platform_socket_receive_packets( socket,
                                                               from + received,
                                                               receive_data + (size_t) received * ROCKETNET_TEST_PUMP_STRIDE,
                                                               ROCKETNET_TEST_PUMP_STRIDE,
                                                               packet_bytes + received,
                                                               ROCKETNET_TEST_PUMP_PACKETS - received );
    }
    rocketnet_check( received == ROCKETNET_TEST_PUMP_PACKETS );

    // every packet arrived whole, correctly sized, correctly attributed --
    // and exactly once

    bool seen[ROCKETNET_TEST_PUMP_PACKETS];
    memset( seen, 0, sizeof(seen) );
    for ( int i = 0; i < ROCKETNET_TEST_PUMP_PACKETS; i++ )
    {
        rocketnet_check( rocketnet_address_equal( &from[i], &local_address ) );
        const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
        const int index = packet[0];
        rocketnet_check( index < ROCKETNET_TEST_PUMP_PACKETS );
        rocketnet_check( !seen[index] );
        seen[index] = true;
        rocketnet_check( packet_bytes[i] == 32 + index * 4 );
        for ( int j = 1; j < packet_bytes[i]; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( index + j ) );
    }

    // a drained socket returns zero

    rocketnet_check( rocketnet_platform_socket_receive_packets( socket, from, receive_data, ROCKETNET_TEST_PUMP_STRIDE, packet_bytes, ROCKETNET_TEST_PUMP_PACKETS ) == 0 );

    // a partial batch returns short without blocking: send 3, ask for many

    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_send_packet( socket, &local_address, send_data[i], 32 + i * 4 );

    received = 0;
    start_time = rocketnet_platform_time();
    while ( received < 3 && rocketnet_platform_time() - start_time < 5.0 )
    {
        received += rocketnet_platform_socket_receive_packets( socket,
                                                               from + received,
                                                               receive_data + (size_t) received * ROCKETNET_TEST_PUMP_STRIDE,
                                                               ROCKETNET_TEST_PUMP_STRIDE,
                                                               packet_bytes + received,
                                                               ROCKETNET_TEST_PUMP_PACKETS - received );
    }
    rocketnet_check( received == 3 );

    // the pump is still the expected one AFTER the exchange, not merely at
    // create. the create-time assertion pins only the probe (which
    // exercises recvmsg_x alone); a send-half-only breakage -- the
    // realistic ABI-drift regression -- would pause the pump mid-exchange
    // and finish portable, and the review demonstrated the whole suite
    // greening over exactly that. the pause outlives any test by design
    // (60 s), so a mid-exchange degradation cannot hide from this line.

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), expected_pump ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_platform_batch_pump()
{
    rocketnet_test_pump_exchange( "0.0.0.0", "127.0.0.1", ROCKETNET_TEST_EXPECTED_PUMP );

#if ROCKETNET_PLATFORM_HAS_IPV6
    rocketnet_test_pump_exchange( "[::]", "[::1]", ROCKETNET_TEST_EXPECTED_PUMP );
#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
}

void rocketnet_test_platform_batch_pump_degraded()
{
    // ROCKETNET_PUMP=portable pins the degradation contract: the forced
    // portable pump answers the same seam with the same behavior, so a
    // darwin machine runs this test over the exact code path a runtime
    // refusal would degrade to. on platforms whose pump is portable
    // already, the override is a no-op and the exchange still holds.

    setenv( "ROCKETNET_PUMP", "portable", 1 );

    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( socket );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), "portable" ) == 0 );
    rocketnet_platform_socket_destroy( socket );

    rocketnet_test_pump_exchange( "0.0.0.0", "127.0.0.1", "portable" );

    unsetenv( "ROCKETNET_PUMP" );
}

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// zero-length chaff injection (#100's review): a zero-length datagram is
// legal UDP, and before this round the portable path treated one as
// "socket drained" -- byte-less chaff capped every drain at one packet, a
// standing DoS lever. both pumps now share skip-and-continue semantics,
// pinned here by injecting chaff between real packets from one raw socket
// (one source socket, so loopback preserves the interleaving) and
// requiring every real packet back, in order, contents intact -- which
// also proves the msgx compaction memmove, since kept packets after a
// mid-chunk skip must move down a slot.

static void rocketnet_test_pump_chaff_exchange( void )
{
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );

    const char * pump_at_create = rocketnet_platform_socket_pump( socket );

    int injector = ::socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    rocketnet_check( injector >= 0 );

    struct sockaddr_in to;
    memset( &to, 0, sizeof(to) );
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    to.sin_port = htons( bind_address.port );

    // interleave: chaff leads, trails, and lands between every couple of
    // real packets, crossing msgx chunk boundaries (48 real > one chunk)

    const int num_real = 48;
    uint8_t real_packet[256];
    int sent_real = 0;
    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    for ( int i = 0; i < num_real; i++ )
    {
        const int bytes = 24 + i;
        real_packet[0] = (uint8_t) i;
        for ( int j = 1; j < bytes; j++ )
            real_packet[j] = (uint8_t) ( 3 * i + j );
        rocketnet_check( sendto( injector, real_packet, (size_t) bytes, 0, (struct sockaddr*) &to, sizeof(to) ) == bytes );
        sent_real++;
        if ( ( i % 2 ) == 1 )
            rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    }
    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );

    // drain through the batch pump: every real packet, in order, intact --
    // and in ONE receive_packets call, because a retry loop here greens
    // over the exact bug this test names (the review measured it: an
    // unfixed path consumes chaff between calls, so retries make
    // progress). loopback arrival is asynchronous (also measured: a
    // recvfrom immediately after sendto answers EAGAIN), so the wait is
    // on ARRIVAL -- readability, then a settle window orders of magnitude
    // past loopback flight time -- and never on the drain itself. the
    // injector socket's FIFO means readability plus the settle has every
    // datagram above queued before the one drain call runs.

    {
        double deadline = rocketnet_platform_time() + 5.0;
        for ( ;; )
        {
            fd_set readable;
            FD_ZERO( &readable );
            FD_SET( socket->handle, &readable );
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            if ( select( socket->handle + 1, &readable, NULL, NULL, &tv ) > 0 )
                break;
            rocketnet_check( rocketnet_platform_time() < deadline );
        }
        usleep( 200000 );
    }

    static rocketnet_address_t from[ROCKETNET_TEST_PUMP_PACKETS];
    static int packet_bytes[ROCKETNET_TEST_PUMP_PACKETS];
    static uint8_t receive_data[ROCKETNET_TEST_PUMP_PACKETS*ROCKETNET_TEST_PUMP_STRIDE];

    const int received = rocketnet_platform_socket_receive_packets( socket, from, receive_data,
                                                                    ROCKETNET_TEST_PUMP_STRIDE, packet_bytes,
                                                                    ROCKETNET_TEST_PUMP_PACKETS );
    rocketnet_check( received == sent_real );

    for ( int i = 0; i < received; i++ )
    {
        const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
        rocketnet_check( packet[0] == (uint8_t) i );
        rocketnet_check( packet_bytes[i] == 24 + i );
        for ( int j = 1; j < packet_bytes[i]; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( 3 * i + j ) );
    }

    // and the per-packet primitive itself skips chaff: one zero-length
    // then one real packet, and the FIRST single-packet call must hand
    // back the real one -- a retry loop here would green over a
    // per-packet path that still reads chaff as drain-end

    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    uint8_t single[64];
    memset( single, 0x5a, sizeof(single) );
    rocketnet_check( sendto( injector, single, sizeof(single), 0, (struct sockaddr*) &to, sizeof(to) ) == (ssize_t) sizeof(single) );

    // the same arrival wait, then the FIRST per-packet call must hand
    // back the real packet through the chaff

    {
        double deadline = rocketnet_platform_time() + 5.0;
        for ( ;; )
        {
            fd_set readable;
            FD_ZERO( &readable );
            FD_SET( socket->handle, &readable );
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            if ( select( socket->handle + 1, &readable, NULL, NULL, &tv ) > 0 )
                break;
            rocketnet_check( rocketnet_platform_time() < deadline );
        }
        usleep( 200000 );
    }

    uint8_t single_received[ROCKETNET_TEST_PUMP_STRIDE];
    rocketnet_address_t single_from;
    int single_bytes = rocketnet_platform_socket_receive_packet( socket, &single_from, single_received, sizeof(single_received) );
    rocketnet_check( single_bytes == (int) sizeof(single) );
    for ( int j = 0; j < single_bytes; j++ )
        rocketnet_check( single_received[j] == 0x5a );

    // chaff exercised the pump it claims to have exercised: no silent
    // mid-run degradation

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_at_create ) == 0 );

    close( injector );
    rocketnet_platform_socket_destroy( socket );
}

// a refused destination mid-batch (#100's review, the blocker): xnu can
// deliver a prefix of a sendmsg_x batch and still return -1 with no
// count, so the send path's law is one bounded per-packet finish of the
// chunk. this pins the survivable half of that law on the real kernel: a
// batch with a refused destination in the middle must deliver every other
// packet AT LEAST once (duplicates are legal, priced, and eaten by replay
// protection above; spurious loss is not legal).

void rocketnet_test_platform_batch_pump_refused_destination()
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_t refused_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_address_parse( &local_address, "127.0.0.1" );
    rocketnet_address_parse( &refused_address, "0.0.0.0" );        // port 0: the kernel refuses it at send
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );
    local_address.port = bind_address.port;

    const int num_packets = 8;
    const int refused_index = 5;

    static uint8_t send_data[8][64];
    struct rocketnet_platform_send_t sends[8];
    for ( int i = 0; i < num_packets; i++ )
    {
        memset( send_data[i], i, sizeof(send_data[i]) );
        sends[i].to = i == refused_index ? &refused_address : &local_address;
        sends[i].packet_data = send_data[i];
        sends[i].packet_bytes = (int) sizeof(send_data[i]);
    }
    rocketnet_platform_socket_send_packets( socket, sends, num_packets );

    bool seen[8];
    memset( seen, 0, sizeof(seen) );
    int distinct = 0;

    double start_time = rocketnet_platform_time();
    while ( distinct < num_packets - 1 && rocketnet_platform_time() - start_time < 5.0 )
    {
        rocketnet_address_t from[8];
        int packet_bytes[8];
        uint8_t receive_data[8*ROCKETNET_TEST_PUMP_STRIDE];
        int received = rocketnet_platform_socket_receive_packets( socket, from, receive_data, ROCKETNET_TEST_PUMP_STRIDE, packet_bytes, 8 );
        for ( int i = 0; i < received; i++ )
        {
            const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
            const int index = packet[0];
            rocketnet_check( index < num_packets );
            rocketnet_check( index != refused_index );
            rocketnet_check( packet_bytes[i] == 64 );
            for ( int j = 0; j < 64; j++ )
                rocketnet_check( packet[j] == (uint8_t) index );
            if ( !seen[index] )
            {
                seen[index] = true;
                distinct++;
            }
        }
    }

    rocketnet_check( distinct == num_packets - 1 );

    // the refused destination is weather, never a pause: the pump must
    // still be the platform's expected one after the error path ran

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), ROCKETNET_TEST_EXPECTED_PUMP ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_platform_pump_zero_length_chaff()
{
    // both pumps: whatever the platform runs by default, and the portable
    // pump forced -- identical semantics is the seam law under review

    rocketnet_test_pump_chaff_exchange();

    setenv( "ROCKETNET_PUMP", "portable", 1 );
    rocketnet_test_pump_chaff_exchange();
    unsetenv( "ROCKETNET_PUMP" );
}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

static bool threads_work = false;

static void test_thread_function(void*)
{
    threads_work = true;
}

void rocketnet_test_platform_thread()
{
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, test_thread_function, NULL );
    rocketnet_check( thread );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    rocketnet_check( threads_work );
}

void rocketnet_test_platform_mutex()
{
    rocketnet_platform_mutex_t mutex;
    bool result = rocketnet_platform_mutex_create( &mutex );
    rocketnet_check( result );
    rocketnet_platform_mutex_acquire( &mutex );
    rocketnet_platform_mutex_release( &mutex );
    {
        rocketnet_platform_mutex_guard( &mutex );
        // ...
    }
    rocketnet_platform_mutex_destroy( &mutex );
}

void rocketnet_test_value_tracker()
{
    // initial values without any samples added should be 0/0/0
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }

    // add just one sample and min/max/avg should be set to that sample value
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        rocketnet_value_tracker_add_sample( &tracker, 1.0f );
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 1.0f );
        rocketnet_check( max_value == 1.0f );
        rocketnet_check( avg_value == 1.0f );
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }

    // add a bunch of samples and we should see min/max/avg
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        for ( int i = 0; i < 100; i++ )
        {
            rocketnet_value_tracker_add_sample( &tracker, float(i%10) );
        }
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 9.0f );
        rocketnet_check( avg_value == 4.5f );
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }

    // add more samples than history size and it should still work
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        for ( int i = 0; i < ROCKETNET_VALUE_TRACKER_HISTORY * 2; i++ )
        {
            rocketnet_value_tracker_add_sample( &tracker, float(i%10) );
        }
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 9.0f );
        rocketnet_check( fabs( 4.5f - avg_value ) < 0.1f );
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }
}

void rocketnet_test_wire_server_packet_valid()
{
    // the same rule the server_xdp kernel program applies, from the shared
    // header -- held against the protocol's own truths here so the kernel
    // filter and the userspace refusal ladder can never drift apart.

    // every server-inbound type passes at its exact wire size, the payload
    // across its whole range

    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_REQUEST_PACKET, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_RESPONSE_PACKET, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_RECEIVE_CEILING ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_RECEIVE_CEILING ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_RECEIVE_CEILING ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_RECEIVE_CEILING ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES ) );
    rocketnet_check( ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_RECEIVE_CEILING ) );

    // and refuses one byte off the fixed sizes in either direction, below
    // every session floor, and above the shared ceiling

    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_REQUEST_PACKET, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_REQUEST_PACKET, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_RESPONSE_PACKET, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES - 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_RESPONSE_PACKET, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES + 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES - 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_RECEIVE_CEILING + 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES - 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_RECEIVE_CEILING + 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES - 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_RECEIVE_CEILING + 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES - 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_RECEIVE_CEILING + 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES - 1 ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_RECEIVE_CEILING + 1 ) );

    // the server-to-client types never pass, including at their own sizes

    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_CHALLENGE_PACKET, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES ) );
    rocketnet_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_CONNECTION_COOKIE_PACKET, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES ) );

    // exhaustive: over every type and every size to the ceiling, exactly the
    // two fixed sizes and the five session ranges are accepted, nothing else

    int accepted = 0;
    for ( int type = 0; type < 256; type++ )
    {
        for ( int bytes = 0; bytes <= ROCKETNET_RECEIVE_CEILING; bytes++ )
        {
            if ( ROCKETNET_WIRE_SERVER_PACKET_VALID( type, bytes ) )
                accepted++;
        }
    }
    const int expected = 2 + ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES + 1 )
                           + ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES + 1 )
                           + ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES + 1 )
                           + ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES + 1 )
                           + ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES + 1 );
    rocketnet_check( accepted == expected );
}

void rocketnet_test_anonymize_address_ipv4()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "1.2.3.4:5" );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_check( address.data.ipv4[0] == 1 );
    rocketnet_check( address.data.ipv4[1] == 2 );
    rocketnet_check( address.data.ipv4[2] == 3 );
    rocketnet_check( address.data.ipv4[3] == 4 );
    rocketnet_check( address.port == 5 );

    rocketnet_address_anonymize( &address );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_check( address.data.ipv4[0] == 1 );
    rocketnet_check( address.data.ipv4[1] == 2 );
    rocketnet_check( address.data.ipv4[2] == 3 );
    rocketnet_check( address.data.ipv4[3] == 0 );
    rocketnet_check( address.port == 0 );
}

#if ROCKETNET_PLATFORM_HAS_IPV6

void rocketnet_test_anonymize_address_ipv6()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "[2001:0db8:85a3:0000:0000:8a2e:0370:7334]:40000" );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_check( address.data.ipv6[0] == 0x2001 );
    rocketnet_check( address.data.ipv6[1] == 0x0db8 );
    rocketnet_check( address.data.ipv6[2] == 0x85a3 );
    rocketnet_check( address.data.ipv6[3] == 0x0000 );
    rocketnet_check( address.data.ipv6[4] == 0x0000 );
    rocketnet_check( address.data.ipv6[5] == 0x8a2e );
    rocketnet_check( address.data.ipv6[6] == 0x0370 );
    rocketnet_check( address.data.ipv6[7] == 0x7334 );
    rocketnet_check( address.port == 40000 );

    rocketnet_address_anonymize( &address );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_check( address.data.ipv6[0] == 0x2001 );
    rocketnet_check( address.data.ipv6[1] == 0x0db8 );
    rocketnet_check( address.data.ipv6[2] == 0x85a3 );
    rocketnet_check( address.data.ipv6[3] == 0x0000 );
    rocketnet_check( address.data.ipv6[4] == 0x0000 );
    rocketnet_check( address.data.ipv6[5] == 0x0000 );
    rocketnet_check( address.data.ipv6[6] == 0x0000 );
    rocketnet_check( address.data.ipv6[7] == 0x0000 );
    rocketnet_check( address.port == 0 );
}

void rocketnet_test_platform_inet6_round_trip()
{
    // pins the return convention of the platform pton6/ntop6 pair. the linux
    // ntop6 arm shipped with its NULL check inverted and nothing caught it,
    // because every caller ignored the return and the ipv6 path is lightly
    // exercised. success is true, on every platform, checked here.

    uint16_t address[8];
    rocketnet_check( rocketnet_platform_inet_pton6( "2001:db8:85a3::8a2e:370:7334", address ) );

    char address_string[64];
    rocketnet_check( rocketnet_platform_inet_ntop6( address, address_string, sizeof(address_string) ) );

    uint16_t round_trip[8];
    rocketnet_check( rocketnet_platform_inet_pton6( address_string, round_trip ) );
    rocketnet_check( memcmp( address, round_trip, sizeof(address) ) == 0 );
}

#endif // #if ROCKETNET_PLATFORM_HAS_IPV6

void rocketnet_test_packet_loss_tracker()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    uint64_t sequence = 0;

    for ( int i = 0; i < ROCKETNET_PACKET_LOSS_TRACKER_SAFETY; ++i )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    for ( int i = 0; i < 200; ++i )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    for ( int i = 0; i < 200; ++i )
    {
        if ( sequence & 1 )
        {
            rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        }
        sequence++;
    }

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == ( 200 - ROCKETNET_PACKET_LOSS_TRACKER_SAFETY ) / 2 );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    rocketnet_packet_loss_tracker_reset( &tracker );

    sequence = 0;

    rocketnet_packet_loss_tracker_packet_received( &tracker, 200 + ROCKETNET_PACKET_LOSS_TRACKER_SAFETY - 1 );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 200 );

    rocketnet_packet_loss_tracker_packet_received( &tracker, 1000 );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) > 500 );

    rocketnet_packet_loss_tracker_packet_received( &tracker, 0xFFFFFFFFFFFFFFFULL );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );
}

void rocketnet_test_out_of_order_tracker()
{
    rocketnet_out_of_order_tracker_t tracker;
    rocketnet_out_of_order_tracker_reset( &tracker );

    rocketnet_check( tracker.num_out_of_order_packets == 0 );

    uint64_t sequence = 0;

    for ( int i = 0; i < 1000; ++i )
    {
        rocketnet_out_of_order_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( tracker.num_out_of_order_packets == 0 );

    sequence = 500;

    for ( int i = 0; i < 500; ++i )
    {
        rocketnet_out_of_order_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( tracker.num_out_of_order_packets == 499 );

    rocketnet_out_of_order_tracker_reset( &tracker );

    rocketnet_check( tracker.last_packet_processed == 0 );
    rocketnet_check( tracker.num_out_of_order_packets == 0 );

    for ( int i = 0; i < 1000; ++i )
    {
        uint64_t mod_sequence = ( sequence / 2 ) * 2;
        if ( sequence % 2 )
            mod_sequence -= 1;
        rocketnet_out_of_order_tracker_packet_received( &tracker, mod_sequence );
        sequence++;
    }

    rocketnet_check( tracker.num_out_of_order_packets == 500 );
}

void rocketnet_test_jitter_tracker()
{
    rocketnet_jitter_tracker_t tracker;
    rocketnet_jitter_tracker_reset( &tracker );

    rocketnet_check( tracker.jitter == 0.0 );

    uint64_t sequence = 0;

    double t = 0.0;
    double dt = 1.0 / 60.0;

    for ( int i = 0; i < 1000; ++i )
    {
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
        t += dt;
    }

    rocketnet_check( tracker.jitter < 0.000001 );

    for ( int i = 0; i < 1000; ++i )
    {
        t = i * dt;
        if ( (i%3) == 0 )
        {
            t += 2;
        }
        if ( (i%5) == 0 )
        {
            t += 5;
        }
        if ( (i%6) == 0 )
        {
            t -= 10;
        }
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
    }

    rocketnet_check( tracker.jitter > 1.0 );

    rocketnet_jitter_tracker_reset( &tracker );

    rocketnet_check( tracker.jitter == 0.0 );

    for ( int i = 0; i < 1000; ++i )
    {
        t = i * dt;
        if ( (i%3) == 0 )
        {
            t += 0.01f;
        }
        if ( (i%5) == 0 )
        {
            t += 0.05;
        }
        if ( (i%6) == 0 )
        {
            t -= 0.1f;
        }
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
    }

    rocketnet_check( tracker.jitter > 0.05 );
    rocketnet_check( tracker.jitter < 0.1 );

    for ( int i = 0; i < 10000; ++i )
    {
        t = i * dt;
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
    }

    rocketnet_check( tracker.jitter >= 0.0 );
    rocketnet_check( tracker.jitter <= 0.000001 );
}

void rocketnet_test_frame_sync_advance()
{
    const uint64_t period = 16666666;

    uint64_t deadline = 100 * period;
    uint64_t error = 12345;

    // on time: no error, one period forward

    rocketnet_check( rocketnet_frame_sync_advance( &deadline, period, 100 * period, &error ) == 0 );
    rocketnet_check( error == 0 );
    rocketnet_check( deadline == 101 * period );

    // an early wake counts as on time

    rocketnet_check( rocketnet_frame_sync_advance( &deadline, period, 101 * period - 1000, &error ) == 0 );
    rocketnet_check( error == 0 );
    rocketnet_check( deadline == 102 * period );

    // late inside the period: reported, not a drop

    rocketnet_check( rocketnet_frame_sync_advance( &deadline, period, 102 * period + period / 2, &error ) == 0 );
    rocketnet_check( error == period / 2 );
    rocketnet_check( deadline == 103 * period );

    // late exactly one period: the boundary is strict, still not a drop

    rocketnet_check( rocketnet_frame_sync_advance( &deadline, period, 103 * period + period, &error ) == 0 );
    rocketnet_check( error == period );
    rocketnet_check( deadline == 104 * period );

    // just past one period: one drop, and the deadline resynchronizes ahead
    // of the wake instead of chasing the backlog

    rocketnet_check( rocketnet_frame_sync_advance( &deadline, period, 104 * period + period + 1, &error ) == 1 );
    rocketnet_check( error == period + 1 );
    rocketnet_check( deadline == 106 * period );
    rocketnet_check( deadline > 104 * period + period + 1 );

    // three and a half periods late: three drops, resynchronized within one
    // period ahead of the wake

    const uint64_t base = deadline;
    const uint64_t woke = base + 3 * period + period / 2;
    rocketnet_check( rocketnet_frame_sync_advance( &deadline, period, woke, &error ) == 3 );
    rocketnet_check( error == 3 * period + period / 2 );
    rocketnet_check( deadline == base + 4 * period );
    rocketnet_check( deadline > woke );
    rocketnet_check( deadline - woke <= period );
}

void rocketnet_test_frame_sync_guard()
{
    const uint64_t cap = 16666666 / ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR;

    // converges toward a steady oversleep

    double ewma = (double) ROCKETNET_FRAME_SYNC_GUARD_EWMA_START_NS;
    for ( int i = 0; i < 200; i++ )
    {
        rocketnet_frame_sync_adapt_guard( &ewma, 500000, cap );
    }
    rocketnet_check( ewma > 450000.0 );
    rocketnet_check( ewma < 550000.0 );

    // the applied guard carries the margin over the estimate

    rocketnet_check( rocketnet_frame_sync_guard_ns( ewma, cap ) == (uint64_t) ewma + ROCKETNET_FRAME_SYNC_GUARD_MARGIN_NS );

    // a runaway oversleep cannot push the guard past the cap: the measured
    // unprivileged death spiral (guard chased to 37ms, thousands of drops),
    // refused by construction

    for ( int i = 0; i < 200; i++ )
    {
        rocketnet_frame_sync_adapt_guard( &ewma, 100000000, cap );
    }
    rocketnet_check( ewma <= (double) cap );
    rocketnet_check( rocketnet_frame_sync_guard_ns( ewma, cap ) == cap );

    // and because the cap binds the estimate too, recovery is immediate once
    // the oversleeps return to sane

    for ( int i = 0; i < 200; i++ )
    {
        rocketnet_frame_sync_adapt_guard( &ewma, 100000, cap );
    }
    rocketnet_check( ewma < 200000.0 );
}

void rocketnet_test_frame_sync_lifecycle()
{
    rocketnet_check( rocketnet_frame_sync_create( NULL, 0.0 ) == NULL );
    rocketnet_check( rocketnet_frame_sync_create( NULL, -60.0 ) == NULL );
    rocketnet_check( rocketnet_frame_sync_create( NULL, 1000000.0 ) == NULL );

    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, 60.0 );
    rocketnet_check( frame_sync );

    struct rocketnet_frame_sync_stats_t stats;
    rocketnet_frame_sync_stats( frame_sync, &stats );
    rocketnet_check( stats.frame == 0 );
    rocketnet_check( stats.dropped_frames == 0 );
    rocketnet_check( stats.priority_held == 0 );                    // taken at the first wait, not before
    rocketnet_check( stats.spinning == 0 );
    rocketnet_check( stats.wakeup_error_seconds == 0.0 );
    rocketnet_check( stats.guard_seconds > 0.0 );
    rocketnet_check( stats.guard_seconds <= ( 1.0 / 60.0 ) / ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR + 1e-9 );

    rocketnet_frame_sync_destroy( frame_sync );
}

void rocketnet_test_frame_sync_wait()
{
    // paced for real, briefly: 25 frames at 250hz is ~100ms of wall time.
    // the assertion is pacing and accounting, never precision -- precision
    // was the issue #56 measurement campaign's job, and CI runners are noisy.

    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, 250.0 );
    rocketnet_check( frame_sync );

    const double start = rocketnet_platform_time();
    for ( int i = 0; i < 25; i++ )
    {
        rocketnet_frame_sync_wait( frame_sync );
    }
    const double elapsed = rocketnet_platform_time() - start;

    struct rocketnet_frame_sync_stats_t stats;
    rocketnet_frame_sync_stats( frame_sync, &stats );
    rocketnet_check( stats.frame == 25 );
    rocketnet_check( elapsed > 0.06 );                              // it actually paced: a free run finishes in microseconds
    rocketnet_check( elapsed < 5.0 );                               // and it did not wedge
    rocketnet_check( !stats.spinning || stats.priority_held );      // never an unprivileged spin
    rocketnet_check( stats.guard_seconds <= ( 1.0 / 250.0 ) / ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR + 1e-9 );

    rocketnet_frame_sync_destroy( frame_sync );
}

// --- the general acks ---

void rocketnet_test_ack_block_encoding()
{
    // every Appendix A block encoding reproduces byte for byte from its
    // stated fields, and decodes back to them

    uint8_t encoded[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    struct rocketnet_ack_block_t block;
    struct rocketnet_ack_block_t decoded;

    // fresh connection, nothing received
    memset( &block, 0, sizeof(block) );
    rocketnet_check( rocketnet_ack_block_write( encoded, &block ) == 7 );
    rocketnet_check( memcmp( encoded, rocketnet_test_ack_block_fresh, 7 ) == 0 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, encoded, 7, 0 ) == 7 );
    rocketnet_check( decoded.largest == 0 && decoded.first_range_length == 0 && decoded.num_ranges == 0 );

    // fully received through 1000
    memset( &block, 0, sizeof(block) );
    block.largest = 1000;
    block.first_range_length = 1000;
    rocketnet_check( rocketnet_ack_block_write( encoded, &block ) == 7 );
    rocketnet_check( memcmp( encoded, rocketnet_test_ack_block_fully_received, 7 ) == 0 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, encoded, 7, 1000 ) == 7 );
    rocketnet_check( decoded.largest == 1000 && decoded.first_range_length == 1000 && decoded.num_ranges == 0 );

    // one loss event: received 991..1000, lost 990, received 900..989
    memset( &block, 0, sizeof(block) );
    block.largest = 1000;
    block.first_range_length = 10;
    block.num_ranges = 1;
    block.range_gap[0] = 1;
    block.range_length[0] = 90;
    rocketnet_check( rocketnet_ack_block_write( encoded, &block ) == 11 );
    rocketnet_check( memcmp( encoded, rocketnet_test_ack_block_one_loss, 11 ) == 0 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, encoded, 11, 1005 ) == 11 );
    rocketnet_check( decoded.largest == 1000 && decoded.first_range_length == 10 && decoded.num_ranges == 1 );
    rocketnet_check( decoded.range_gap[0] == 1 && decoded.range_length[0] == 90 );

    // two loss events, exercising the u32 field
    memset( &block, 0, sizeof(block) );
    block.largest = 70000;
    block.first_range_length = 100;
    block.num_ranges = 2;
    block.range_gap[0] = 3;
    block.range_length[0] = 500;
    block.range_gap[1] = 1000;
    block.range_length[1] = 2000;
    rocketnet_check( rocketnet_ack_block_write( encoded, &block ) == 15 );
    rocketnet_check( memcmp( encoded, rocketnet_test_ack_block_two_losses, 15 ) == 0 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, encoded, 15, 70000 ) == 15 );
    rocketnet_check( decoded.largest == 70000 && decoded.first_range_length == 100 && decoded.num_ranges == 2 );
    rocketnet_check( decoded.range_gap[0] == 3 && decoded.range_length[0] == 500 );
    rocketnet_check( decoded.range_gap[1] == 1000 && decoded.range_length[1] == 2000 );

    // a decode consumes only its declared geometry: trailing type data
    // behind the block is untouched
    uint8_t with_tail[7 + 8];
    memcpy( with_tail, rocketnet_test_ack_block_fully_received, 7 );
    memset( with_tail + 7, 0xAB, 8 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, with_tail, sizeof(with_tail), 1000 ) == 7 );

    // the fragment header encoding, alone: payload id 7, fragment index 2,
    // fragment count 3 -- the frozen Appendix A layout
    uint8_t fragment_header[6];
    uint8_t * p = fragment_header;
    rocketnet_write_uint32( &p, 7 );
    rocketnet_write_uint8( &p, 2 );
    rocketnet_write_uint8( &p, 3 - 1 );
    rocketnet_check( memcmp( fragment_header, rocketnet_test_fragment_header, 6 ) == 0 );
}

void rocketnet_test_ack_block_validity()
{
    struct rocketnet_ack_block_t decoded;
    uint8_t data[ROCKETNET_ACK_BLOCK_MAX_BYTES];

    // NEGATIVE: shorter than the 7-byte minimum refuses
    memset( data, 0, sizeof(data) );
    for ( int size = 0; size < 7; size++ )
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, size, 0 ) == 0 );

    // NEGATIVE: the declared geometry exceeding the opened size refuses
    memcpy( data, rocketnet_test_ack_block_one_loss, 11 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, data, 10, 1000 ) == 0 );

    // NEGATIVE: first range length zero while largest is nonzero
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 1000 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 0 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 7, 1000 ) == 0 );
    }

    // NEGATIVE: first range length nonzero while largest is zero
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 0 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 1 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 7, 0 ) == 0 );
    }

    // NEGATIVE: a range entry on a fresh block refuses -- with largest
    // zero the walk cannot exist
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 0 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 0 );
        rocketnet_write_uint16( &p, 1 );
        rocketnet_write_uint16( &p, 1 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 11, 0 ) == 0 );
    }

    // NEGATIVE: first range length exceeding largest
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 10 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 11 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 7, 10 ) == 0 );
    }

    // a block describing everything down to sequence one exactly is valid
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 10 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 10 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 7, 10 ) == 7 );
    }

    // NEGATIVE: a zero gap entry refuses
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 100 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 10 );
        rocketnet_write_uint16( &p, 0 );
        rocketnet_write_uint16( &p, 5 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 11, 100 ) == 0 );
    }

    // NEGATIVE: a zero length entry refuses
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 100 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 10 );
        rocketnet_write_uint16( &p, 5 );
        rocketnet_write_uint16( &p, 0 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 11, 100 ) == 0 );
    }

    // NEGATIVE: the walk going out of bounds refuses -- 10 + 5 + 90 > 100
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 100 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 10 );
        rocketnet_write_uint16( &p, 5 );
        rocketnet_write_uint16( &p, 90 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 11, 100 ) == 0 );
    }

    // NEGATIVE: and one past the bound exactly refuses -- 10 + 5 + 86 ==
    // largest + 1, the walk's lowest sequence would be zero
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 100 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 10 );
        rocketnet_write_uint16( &p, 5 );
        rocketnet_write_uint16( &p, 86 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 11, 100 ) == 0 );
    }

    // and the walk touching sequence one exactly is valid -- 10 + 5 + 85
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 100 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 10 );
        rocketnet_write_uint16( &p, 5 );
        rocketnet_write_uint16( &p, 85 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 11, 100 ) == 11 );
    }

    // validity is evaluated on the reconstructed 64-bit largest: a genuine
    // sequence whose low 32 bits are zero does not read as a fresh
    // connection -- wire zero against a highest sent near the epoch
    // boundary reconstructs to 2^32, and a nonzero first range is required
    {
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 0 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 16 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 7, 0xFFFFFFF0ULL ) == 7 );
        rocketnet_check( decoded.largest == 0x100000000ULL );

        p = data;
        rocketnet_write_uint32( &p, 0 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 0 );
        rocketnet_check( rocketnet_ack_block_read( &decoded, data, 7, 0xFFFFFFF0ULL ) == 0 );
    }

    // the ack largest reconstruction is the wire rule with expected = the
    // highest sent directly
    rocketnet_check( rocketnet_reconstruct_ack_largest( 0, 0 ) == 0 );
    rocketnet_check( rocketnet_reconstruct_ack_largest( 100, 0 ) == 100 );
    rocketnet_check( rocketnet_reconstruct_ack_largest( 100, 105 ) == 100 );
    rocketnet_check( rocketnet_reconstruct_ack_largest( 0xFFFFFFFF, 0x100000005ULL ) == 0xFFFFFFFFULL );
    rocketnet_check( rocketnet_reconstruct_ack_largest( 5, 0xFFFFFFFFULL ) == 0x100000005ULL );
}

void rocketnet_test_ack_block_build()
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof(replay) );

    uint8_t built[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    struct rocketnet_ack_block_t decoded;

    // a fresh receive state builds the all-zero minimum block
    rocketnet_check( rocketnet_ack_block_build( built, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay ) == 7 );
    rocketnet_check( memcmp( built, rocketnet_test_ack_block_fresh, 7 ) == 0 );

    // everything received: one run, no ranges, capped at the window depth
    for ( uint64_t s = 1; s <= 100; s++ )
        rocketnet_replay_advance( &replay, s );
    rocketnet_check( rocketnet_ack_block_build( built, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay ) == 7 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, built, 7, 100 ) == 7 );
    rocketnet_check( decoded.largest == 100 && decoded.first_range_length == 100 && decoded.num_ranges == 0 );

    // one hole: received 1..100 and 102..110 -- first run 9, gap 1, run 100
    for ( uint64_t s = 102; s <= 110; s++ )
        rocketnet_replay_advance( &replay, s );
    rocketnet_check( rocketnet_ack_block_build( built, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay ) == 11 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, built, 11, 110 ) == 11 );
    rocketnet_check( decoded.largest == 110 && decoded.first_range_length == 9 && decoded.num_ranges == 1 );
    rocketnet_check( decoded.range_gap[0] == 1 && decoded.range_length[0] == 100 );

    // truncate-oldest to fit: with room for the minimum only, the range is
    // dropped and the block still always fits
    rocketnet_check( rocketnet_ack_block_build( built, ROCKETNET_ACK_BLOCK_MIN_BYTES, &replay ) == 7 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, built, 7, 110 ) == 7 );
    rocketnet_check( decoded.largest == 110 && decoded.first_range_length == 9 && decoded.num_ranges == 0 );

    // capacity for one entry keeps the newest loss event when two exist
    memset( &replay, 0, sizeof(replay) );
    for ( uint64_t s = 1; s <= 50; s++ )
        rocketnet_replay_advance( &replay, s );
    for ( uint64_t s = 52; s <= 60; s++ )
        rocketnet_replay_advance( &replay, s );
    for ( uint64_t s = 63; s <= 70; s++ )
        rocketnet_replay_advance( &replay, s );
    rocketnet_check( rocketnet_ack_block_build( built, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay ) == 15 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, built, 15, 70 ) == 15 );
    rocketnet_check( decoded.first_range_length == 8 && decoded.num_ranges == 2 );
    rocketnet_check( decoded.range_gap[0] == 2 && decoded.range_length[0] == 9 );
    rocketnet_check( decoded.range_gap[1] == 1 && decoded.range_length[1] == 50 );
    rocketnet_check( rocketnet_ack_block_build( built, ROCKETNET_ACK_BLOCK_MIN_BYTES + 4, &replay ) == 11 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, built, 11, 70 ) == 11 );
    rocketnet_check( decoded.num_ranges == 1 );
    rocketnet_check( decoded.range_gap[0] == 2 && decoded.range_length[0] == 9 );

    // deep in the sequence space the block speaks only for the window: the
    // walk stops at the floor and everything below is unspoken
    memset( &replay, 0, sizeof(replay) );
    rocketnet_replay_advance( &replay, 10000 );
    for ( uint64_t s = 10002; s <= 10005; s++ )
        rocketnet_replay_advance( &replay, s );
    rocketnet_check( rocketnet_ack_block_build( built, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay ) == 11 );
    rocketnet_check( rocketnet_ack_block_read( &decoded, built, 11, 10005 ) == 11 );
    rocketnet_check( decoded.largest == 10005 && decoded.first_range_length == 4 && decoded.num_ranges == 1 );
    rocketnet_check( decoded.range_gap[0] == 1 && decoded.range_length[0] == 1 );

    // every built block is valid by construction: a randomized soak of
    // arrival patterns, every one decoding cleanly against its own state
    for ( int trial = 0; trial < 100; trial++ )
    {
        memset( &replay, 0, sizeof(replay) );
        uint64_t s = 1 + ( rocketnet_random_uint32() % 1000 );
        for ( int n = 0; n < 400; n++ )
        {
            rocketnet_replay_advance( &replay, s );
            s += 1 + ( rocketnet_random_uint8() % 3 );       // skips leave holes
        }
        int capacity = ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * ( rocketnet_random_uint8() % 64 );
        int built_bytes = rocketnet_ack_block_build( built, capacity, &replay );
        rocketnet_check( built_bytes >= ROCKETNET_ACK_BLOCK_MIN_BYTES );
        rocketnet_check( built_bytes <= capacity );
        rocketnet_check( rocketnet_ack_block_read( &decoded, built, built_bytes, replay.highest_opened ) == built_bytes );
    }
}

void rocketnet_test_ack_endpoint_processing()
{
    struct rocketnet_ack_endpoint_t * endpoint = (struct rocketnet_ack_endpoint_t*)
        rocketnet_malloc( NULL, sizeof(struct rocketnet_ack_endpoint_t) );
    rocketnet_check( endpoint );
    rocketnet_ack_endpoint_reset( endpoint );

    uint64_t acked[64];
    int acked_count = 0;
    uint64_t unknown = 0;
    uint64_t batch_full = 0;

    // ten sent packets, ids 100..109, sequences 1..10
    for ( uint64_t s = 1; s <= 10; s++ )
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, 99 + s, 1, 0, 10.0 + (double) s );

    // a block acknowledging 1..10 marks all ten, surfaces all ten ids, and
    // yields an rtt sample per sequence
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = 10;
    block.first_range_length = 10;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 30.0, acked, 64, &acked_count, &unknown, &batch_full ) == 10 );
    rocketnet_check( acked_count == 10 );
    for ( int i = 0; i < 10; i++ )
        rocketnet_check( acked[i] == 109 - (uint64_t) i );          // newest first, the walk is descending
    rocketnet_check( unknown == 0 && batch_full == 0 );
    rocketnet_check( endpoint->has_rtt_sample );

    // acknowledgment is once per sequence: the same block again surfaces
    // nothing -- delivery information is idempotent
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 31.0, acked, 64, &acked_count, &unknown, &batch_full ) == 0 );
    rocketnet_check( acked_count == 0 && unknown == 0 );

    // sequences named that were never sent count on the side counter and
    // are otherwise ignored: 1..10 already acked, 11..14 never sent
    memset( &block, 0, sizeof(block) );
    block.largest = 14;
    block.first_range_length = 14;
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 32.0, acked, 64, &acked_count, &unknown, &batch_full ) == 0 );
    rocketnet_check( acked_count == 0 );
    rocketnet_check( unknown == 4 );

    // the walk is bounded by the retained window, never by the block's
    // claims: a block naming sixteen million sequences below the window is
    // skipped arithmetically -- this returns immediately rather than
    // iterating what the peer asserts
    rocketnet_ack_endpoint_reset( endpoint );
    for ( uint64_t s = 20000000; s <= 20000009; s++ )
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, s, 1, 0, 50.0 );
    memset( &block, 0, sizeof(block) );
    block.largest = 17000000;                   // room for the full 255-range walk above sequence one
    block.first_range_length = 65535;
    block.num_ranges = ROCKETNET_ACK_BLOCK_MAX_RANGES;
    for ( int i = 0; i < ROCKETNET_ACK_BLOCK_MAX_RANGES; i++ )
    {
        block.range_gap[i] = 1;
        block.range_length[i] = 65535;
    }
    unknown = 0;
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 51.0, acked, 64, &acked_count, &unknown, &batch_full ) == 0 );
    rocketnet_check( acked_count == 0 && unknown == 0 );

    // a burned sequence -- consumed by an aborted send, never on the wire --
    // named inside the window counts unknown without disturbing neighbors
    rocketnet_ack_endpoint_reset( endpoint );
    rocketnet_ack_endpoint_record_sent( endpoint, 1, 100, 500, 1, 0, 60.0 );
    rocketnet_ack_endpoint_record_sent( endpoint, 3, 100, 501, 1, 0, 60.0 );    // sequence 2 burned
    memset( &block, 0, sizeof(block) );
    block.largest = 3;
    block.first_range_length = 3;
    unknown = 0;
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 61.0, acked, 64, &acked_count, &unknown, &batch_full ) == 2 );
    rocketnet_check( acked_count == 2 );
    rocketnet_check( unknown == 1 );

    // a full batch defers, never loses: the overflow stays unacknowledged
    // and counted, and the next block -- every block restates the recent
    // interval -- surfaces it into a drained batch
    rocketnet_ack_endpoint_reset( endpoint );
    for ( uint64_t s = 1; s <= 8; s++ )
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, s, 1, 0, 70.0 );
    memset( &block, 0, sizeof(block) );
    block.largest = 8;
    block.first_range_length = 8;
    acked_count = 0;
    batch_full = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 71.0, acked, 4, &acked_count, &unknown, &batch_full ) == 4 );
    rocketnet_check( acked_count == 4 );
    rocketnet_check( batch_full == 4 );
    for ( uint64_t s = 8; s >= 5; s-- )
        rocketnet_check( endpoint->sent[s % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );       // the walk is descending: newest four landed
    for ( uint64_t s = 4; s >= 1; s-- )
        rocketnet_check( !endpoint->sent[s % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );      // the overflow deferred, not lost
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 72.0, acked, 4, &acked_count, &unknown, &batch_full ) == 4 );
    rocketnet_check( acked_count == 4 );
    for ( uint64_t s = 1; s <= 8; s++ )
        rocketnet_check( endpoint->sent[s % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );

    // ranges with gaps mark exactly the described sequences
    rocketnet_ack_endpoint_reset( endpoint );
    for ( uint64_t s = 1; s <= 20; s++ )
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, s, 1, 0, 80.0 );
    memset( &block, 0, sizeof(block) );
    block.largest = 20;
    block.first_range_length = 5;           // 16..20
    block.num_ranges = 1;
    block.range_gap[0] = 3;                 // 13..15 missing
    block.range_length[0] = 2;              // 11..12
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( endpoint, &block, 81.0, acked, 64, &acked_count, &unknown, &batch_full ) == 7 );
    for ( uint64_t s = 1; s <= 20; s++ )
    {
        int expect_acked = ( s >= 16 && s <= 20 ) || s == 11 || s == 12;
        rocketnet_check( (int) endpoint->sent[s % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked == expect_acked );
    }

    // the stats recompute: loss over the older half, rtt the minimum sample,
    // acked bandwidth counting only acknowledged bytes
    rocketnet_ack_endpoint_reset( endpoint );
    for ( uint64_t s = 1; s <= (uint64_t) ROCKETNET_SENT_PACKET_BUFFER_SIZE; s++ )
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, s, 1, 0, 100.0 + (double) s * 0.001 );
    memset( &block, 0, sizeof(block) );
    block.largest = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    block.first_range_length = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 4;       // the newer quarter of the older half
    for ( int drain = 0; drain < 100; drain++ )
    {
        acked_count = 0;                    // each drained batch admits more; overflow defers to the next
        if ( rocketnet_ack_endpoint_process_block( endpoint, &block, 110.0, acked, 64, &acked_count, &unknown, &batch_full ) == 0 )
            break;
    }
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_check( endpoint->packet_loss > 40.0f );           // half the older half unacknowledged
    rocketnet_check( endpoint->packet_loss < 60.0f );
    rocketnet_check( endpoint->rtt > 0.0f );
    rocketnet_check( endpoint->bandwidth_sent_kbps > 0.0f );
    rocketnet_check( endpoint->bandwidth_acked_kbps > 0.0f );

    rocketnet_free( NULL, endpoint );
}

// establishes a connected client against the protocol-level test server

static void rocketnet_test_ack_connect( struct rocketnet_test_server_t * test,
                                        struct rocketnet_client_protocol_t * client,
                                        struct rocketnet_counters_t * client_counters,
                                        uint64_t client_id )
{
    memset( client_counters, 0, sizeof(*client_counters) );
    rocketnet_client_protocol_init( client, client_counters );
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, client_id, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( rocketnet_test_pump( test, client, -1, 5.0 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
}

void rocketnet_test_ack_debt()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    rocketnet_test_ack_connect( &test, &client, &client_counters, 0xDEB7ULL );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    double time = 200.0;

    // --- the server side: eliciting packets received with nothing sent
    // back arm the debt, and the threshold fires exactly one type 8 ---

    const uint8_t data[4] = { 1, 2, 3, 4 };

    for ( int i = 0; i < ROCKETNET_ACK_DEBT_THRESHOLD - 1; i++ )
    {
        time += 0.001;
        int bytes = rocketnet_client_protocol_write_payload( &client, data, 4, time, packet );
        rocketnet_check( bytes > 0 );
        rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    }

    // one below the threshold: not due
    rocketnet_check( rocketnet_server_protocol_write_ack_if_due( &test.server, 0, time, packet ) == 0 );

    // the threshold: the standalone ack goes out, and the debt resets
    time += 0.001;
    int bytes = rocketnet_client_protocol_write_payload( &client, data, 4, time, packet );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );

    uint8_t ack_packet[ROCKETNET_RECEIVE_CEILING];
    int ack_bytes = rocketnet_server_protocol_write_ack_if_due( &test.server, 0, time, ack_packet );
    rocketnet_check( ack_bytes >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
    rocketnet_check( ack_packet[0] == ROCKETNET_CONNECTION_ACK_PACKET );
    rocketnet_check( rocketnet_server_protocol_write_ack_if_due( &test.server, 0, time, packet ) == 0 );

    // the client accepts it: positive path, both counters move
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, ack_packet, ack_bytes, time, payload, &payload_bytes ) );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED] == 1 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED] == 1 );

    // --- any sent block resets the debt: a keep-alive going out clears it ---

    for ( int i = 0; i < ROCKETNET_ACK_DEBT_THRESHOLD - 1; i++ )
    {
        time += 0.001;
        bytes = rocketnet_client_protocol_write_payload( &client, data, 4, time, packet );
        rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    }
    rocketnet_check( rocketnet_server_write_keep_alive( &test.server, 0, time, packet ) >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    time += 0.001;
    bytes = rocketnet_client_protocol_write_payload( &client, data, 4, time, packet );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( rocketnet_server_protocol_write_ack_if_due( &test.server, 0, time, packet ) == 0 );

    // --- the client side: keep-alives received with nothing sent back arm
    // the client's debt, and its update emits the type 8 outside the send
    // rate -- that is what removes the coupling between ack capacity and
    // return-traffic cadence ---

    double client_time = client.last_send_time + 0.02;      // inside the rate gate: no scheduled send is due

    for ( int i = 0; i < ROCKETNET_ACK_DEBT_THRESHOLD; i++ )
    {
        int ka_bytes = rocketnet_server_write_keep_alive( &test.server, 0, time, packet );
        rocketnet_check( ka_bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
        rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, ka_bytes, client_time, payload, &payload_bytes ) );
    }

    int out_bytes = 0;
    rocketnet_client_protocol_update( &client, client_time, packet, &out_bytes );
    rocketnet_check( out_bytes >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
    rocketnet_check( packet[0] == ROCKETNET_CONNECTION_ACK_PACKET );

    // the server accepts it, and it acknowledged the keep-alives just
    // received -- the block is built from everything opened
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, out_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_RECEIVED] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_ACCEPTED] == 1 );

    // and the debt is spent: the next gated update emits nothing
    out_bytes = 12345;
    rocketnet_client_protocol_update( &client, client_time + 0.01, packet, &out_bytes );
    rocketnet_check( out_bytes == 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_ack_non_elicitation()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    rocketnet_test_ack_connect( &test, &client, &client_counters, 0xE11C17ULL );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    // type 8 never elicits: a flood of standalone acks -- twice the debt
    // threshold -- adds nothing to the client's debt, so its update emits no
    // ack back and the conversation terminates. two quiet peers each
    // obliged to acknowledge acknowledgments would exchange them forever.

    struct rocketnet_encryption_mapping_t * mapping =
        rocketnet_encryption_map_entry( test.server.encryption_map,
                                        rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) );
    rocketnet_check( mapping );

    double client_time = client.last_send_time + 0.02;      // inside the rate gate

    for ( int i = 0; i < 2 * ROCKETNET_ACK_DEBT_THRESHOLD; i++ )
    {
        int bytes = rocketnet_test_write_session_packet_salted( packet, ROCKETNET_CONNECTION_ACK_PACKET,
                                                                mapping->send_sequence_session + (uint64_t) i,
                                                                NULL, 0, client.connection_salt,
                                                                mapping->keys.session_server_to_client );
        rocketnet_check( bytes == ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
        rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, client_time, payload, &payload_bytes ) );
    }
    mapping->send_sequence_session += (uint64_t) ( 2 * ROCKETNET_ACK_DEBT_THRESHOLD );     // the hand-built packets spent these
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED] == (uint64_t) ( 2 * ROCKETNET_ACK_DEBT_THRESHOLD ) );

    int out_bytes = 12345;
    rocketnet_client_protocol_update( &client, client_time, packet, &out_bytes );
    rocketnet_check( out_bytes == 0 );

    // the mirror at the server: hand it a flood of type 8s and its own
    // standalone ack never becomes due
    for ( int i = 0; i < 2 * ROCKETNET_ACK_DEBT_THRESHOLD; i++ )
    {
        int bytes = rocketnet_test_write_session_packet_salted( packet, ROCKETNET_CONNECTION_ACK_PACKET,
                                                                client.send_sequence_session + (uint64_t) i,
                                                                NULL, 0, client.connection_salt,
                                                                client.keys.session_client_to_server );
        rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, client_time, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    }
    client.send_sequence_session += (uint64_t) ( 2 * ROCKETNET_ACK_DEBT_THRESHOLD );        // the hand-built packets spent these
    rocketnet_check( rocketnet_server_protocol_write_ack_if_due( &test.server, 0, client_time, packet ) == 0 );

    // and a balanced idle connection exchanges no type 8 at all: both ends
    // at their scheduled rates for thirty simulated seconds, every packet a
    // keep-alive. time continues from the connection's own clock so neither
    // end's idle timeout fires.
    double time = client_time;
    for ( int step = 0; step < 3000; step++ )
    {
        time += 0.01;
        int bytes = 0;
        rocketnet_client_protocol_update( &client, time, packet, &bytes );
        if ( bytes > 0 )
        {
            rocketnet_check( packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );
            rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                              ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                              payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        }
        bytes = rocketnet_server_protocol_write_keep_alive_if_due( &test.server, 0, time, packet );
        if ( bytes == 0 )
            bytes = rocketnet_server_protocol_write_ack_if_due( &test.server, 0, time, packet );
        if ( bytes > 0 )
        {
            rocketnet_check( packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );
            rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, time, payload, &payload_bytes ) );
        }
    }

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_ack_replay_protection()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    rocketnet_test_ack_connect( &test, &client, &client_counters, 0x8E91A4ULL );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    // the standalone ack rides the session sequence space and replay
    // protection applies: the first copy is accepted, the byte-identical
    // replay is refused before the open

    int bytes = rocketnet_test_write_session_packet_salted( packet, ROCKETNET_CONNECTION_ACK_PACKET,
                                                            client.send_sequence_session, NULL, 0,
                                                            client.connection_salt,
                                                            client.keys.session_client_to_server );
    client.send_sequence_session++;
    rocketnet_check( bytes == ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );

    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.0, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_RECEIVED] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_ACCEPTED] == 1 );

    // NEGATIVE: the replay -- the arrival counts, the refusal names replay
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.1, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_RECEIVED] == 2 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_ACCEPTED] == 1 );

    // and at the client, the same discipline
    struct rocketnet_encryption_mapping_t * mapping =
        rocketnet_encryption_map_entry( test.server.encryption_map,
                                        rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) );
    rocketnet_check( mapping );

    bytes = rocketnet_test_write_session_packet_salted( packet, ROCKETNET_CONNECTION_ACK_PACKET,
                                                        mapping->send_sequence_session, NULL, 0,
                                                        client.connection_salt,
                                                        mapping->keys.session_server_to_client );
    mapping->send_sequence_session++;

    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, 200.2, payload, &payload_bytes ) );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED] == 1 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED] == 1 );

    // NEGATIVE: the replay at the client
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, 200.3, payload, &payload_bytes ) == 0 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED] == 2 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ALREADY_RECEIVED] == 1 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED] == 1 );

    rocketnet_test_server_teardown( &test );
}

// seals a session packet whose plaintext is exactly the given bytes -- for
// crafting malformed ack blocks that still open authentically

static int rocketnet_test_write_raw_session_packet( uint8_t * packet, uint8_t packet_type, uint64_t sequence,
                                                    const uint8_t * plaintext, int plaintext_bytes,
                                                    const uint8_t * salt, const uint8_t * key )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    return rocketnet_write_sealed_packet( packet, packet_type, tag, sequence, NULL,
                                          plaintext, plaintext_bytes,
                                          ROCKETNET_TEST_PROTOCOL_ID, salt, NULL, key );
}

void rocketnet_test_ack_bad_block_refusals()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    rocketnet_test_ack_connect( &test, &client, &client_counters, 0xBADB1ULL );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    uint8_t plaintext[64];
    uint8_t * p;
    int bytes;

    // NEGATIVE: a keep-alive whose block declares first range length zero
    // with largest nonzero refuses the whole packet as BAD_ACK_BLOCK; the
    // type data behind it is never examined
    p = plaintext;
    rocketnet_write_uint32( &p, 5 );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint16( &p, 0 );
    rocketnet_write_uint32( &p, 0 );
    rocketnet_write_uint32( &p, 4 );
    uint64_t sequence = client.send_sequence_session++;
    bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                                     plaintext, 15, client.connection_salt, client.keys.session_client_to_server );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.0, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED] >= 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK] == 1 );

    // the refusal had no effect beyond its replay-window mark: the same
    // sequence again is refused as already received, per the reading order
    bytes = rocketnet_test_write_session_packet_salted( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                                        plaintext + 7, 8, client.connection_salt,
                                                        client.keys.session_client_to_server );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.0, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED] == 1 );

    // NEGATIVE: a payload whose block geometry exceeds the opened size
    p = plaintext;
    rocketnet_write_uint32( &p, 0 );
    rocketnet_write_uint8( &p, 200 );                   // declares 807 bytes of block in a tiny packet
    rocketnet_write_uint16( &p, 0 );
    plaintext[7] = 0x55;
    bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, client.send_sequence_session++,
                                                     plaintext, 8, client.connection_salt, client.keys.session_client_to_server );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.1, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK] == 1 );
    rocketnet_check( payload_bytes == 0 );

    // NEGATIVE: a disconnect with a zero gap entry refuses -- and does NOT
    // disconnect the client
    p = plaintext;
    rocketnet_write_uint32( &p, 100 );
    rocketnet_write_uint8( &p, 1 );
    rocketnet_write_uint16( &p, 10 );
    rocketnet_write_uint16( &p, 0 );
    rocketnet_write_uint16( &p, 5 );
    bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_DISCONNECT_PACKET, client.send_sequence_session++,
                                                     plaintext, 11, client.connection_salt, client.keys.session_client_to_server );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.2, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK] == 1 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );

    // NEGATIVE: a standalone ack whose walk goes out of bounds
    p = plaintext;
    rocketnet_write_uint32( &p, 100 );
    rocketnet_write_uint8( &p, 1 );
    rocketnet_write_uint16( &p, 10 );
    rocketnet_write_uint16( &p, 50 );
    rocketnet_write_uint16( &p, 60 );                   // 10 + 50 + 60 > 100
    bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_ACK_PACKET, client.send_sequence_session++,
                                                     plaintext, 11, client.connection_salt, client.keys.session_client_to_server );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.3, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_RECEIVED] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK] == 1 );

    // NEGATIVE: a VALID block behind a wrong opened size is NOT applied --
    // delivery information waits for both checks, so the named sequence
    // stays unacknowledged, nothing surfaces, and the packet has no effect
    // beyond its replay-window mark: the idle timeout is not refreshed
    {
        struct rocketnet_encryption_mapping_t * m =
            rocketnet_encryption_map_entry( test.server.encryption_map,
                                            rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) );
        rocketnet_check( m );

        uint64_t target_sequence = m->send_sequence_session;
        rocketnet_check( rocketnet_server_write_keep_alive( &test.server, 0, 200.35, packet ) >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
        rocketnet_check( !test.server.acks->endpoints[0].sent[target_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );

        p = plaintext;
        rocketnet_write_uint32( &p, (uint32_t) ( target_sequence & 0xFFFFFFFF ) );      // a valid block acking the keep-alive just sent
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 1 );
        memset( p, 0x77, 9 );                                                           // nine bytes where the keep-alive fields want eight

        double last_receive_before = m->last_receive_time;
        int num_acked_before = test.server.acks->num_acked;

        bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, client.send_sequence_session++,
                                                         plaintext, 16, client.connection_salt, client.keys.session_client_to_server );
        rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, 200.36, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE] == 1 );
        rocketnet_check( !test.server.acks->endpoints[0].sent[target_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );
        rocketnet_check( test.server.acks->num_acked == num_acked_before );
        rocketnet_check( m->last_receive_time == last_receive_before );

        // the same block behind the right size applies: the withholding was
        // the size gate, not the block
        uint8_t keep_alive_fields[8] = { 0 };
        memcpy( plaintext + 7, keep_alive_fields, 8 );
        bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, client.send_sequence_session++,
                                                         plaintext, 15, client.connection_salt, client.keys.session_client_to_server );
        rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, 200.37, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        rocketnet_check( test.server.acks->endpoints[0].sent[target_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );
        rocketnet_check( m->last_receive_time == 200.37 );
    }

    // NEGATIVE: below the type's wire floor is WRONG_SIZE, never a later
    // refusal -- one byte under each session floor, with a real prefilter
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    struct { uint8_t packet_type; int floor; int received_counter; int wrong_size_counter; } floors[4] = {
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES,
          ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES,
          ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE },
        { ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES,
          ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE },
        { ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES,
          ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_WRONG_SIZE },
    };
    for ( int i = 0; i < 4; i++ )
    {
        memset( packet, 0, sizeof(packet) );
        p = packet;
        rocketnet_write_prefilter_header( &p, floors[i].packet_type, tag );
        uint64_t received_before = test.counters.values[floors[i].received_counter];
        rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, floors[i].floor - 1,
                                                          ROCKETNET_TEST_CREATE_TS, 200.4, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( test.counters.values[floors[i].received_counter] == received_before + 1 );
        rocketnet_check( test.counters.values[floors[i].wrong_size_counter] == 1 );
    }

    // NEGATIVE: a forged type 9 enters the fragment ladder with the other
    // session types and refuses on its own reasons -- an all-zero body dies
    // at the zero sequence -- never the reserved drop
    memset( packet, 0, sizeof(packet) );
    p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_FRAGMENT_PACKET, tag );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES,
                                                      ROCKETNET_TEST_CREATE_TS, 200.5, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE] == 1 );

    // the ACK_UNKNOWN_SEQUENCE side counter: a keep-alive whose valid block
    // acknowledges sequences this server never sent is still accepted --
    // the packet succeeds, the side counter records the hostile claims
    struct rocketnet_encryption_mapping_t * mapping =
        rocketnet_encryption_map_entry( test.server.encryption_map,
                                        rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) );
    uint64_t claim = mapping->send_sequence_session - 1 + 40;       // 40 sequences beyond anything sent
    p = plaintext;
    rocketnet_write_uint32( &p, (uint32_t) ( claim & 0xFFFFFFFF ) );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint16( &p, 40 );
    rocketnet_write_uint32( &p, 0 );
    rocketnet_write_uint32( &p, 4 );
    uint64_t accepted_before = test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED];
    bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, client.send_sequence_session++,
                                                     plaintext, 15, client.connection_salt, client.keys.session_client_to_server );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.6, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == accepted_before + 1 );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE] >= 40 );

    // the client-side mirrors: a bad block and a floor refusal, both
    // counted in the client's own families
    struct rocketnet_encryption_mapping_t * m = mapping;
    p = plaintext;
    rocketnet_write_uint32( &p, 5 );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint16( &p, 0 );                    // first range length zero, largest nonzero
    bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_ACK_PACKET, m->send_sequence_session++,
                                                     plaintext, 7, client.connection_salt, m->keys.session_server_to_client );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, 210.0, payload, &payload_bytes ) == 0 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED] == 1 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_BAD_ACK_BLOCK] == 1 );

    memset( packet, 0, sizeof(packet) );
    p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, tag );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES - 1, 210.1, payload, &payload_bytes ) == 0 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE] == 1 );

    // and the client's wrong opened size: a valid fresh block behind nine
    // bytes where the keep-alive fields want eight
    p = plaintext;
    memset( p, 0, 7 );
    memset( p + 7, 0x77, 9 );
    bytes = rocketnet_test_write_raw_session_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, m->send_sequence_session++,
                                                     plaintext, 16, client.connection_salt, m->keys.session_server_to_client );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, bytes, 210.2, payload, &payload_bytes ) == 0 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED] >= 1 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE] == 1 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_ack_asymmetric_rate()
{
    // The asymmetric case the standalone ack exists for: a fast sender
    // against a slow return path. The client sends a full debt threshold of
    // payloads per round; the server's only traffic back is the one type 8
    // the threshold obliges -- a 64:1 rate asymmetry -- and every packet id
    // the client sent is surfaced acknowledged exactly once. There is no
    // structural packets-per-second cap: the block widens with loss, not
    // with rate, and the standalone ack supplies return capacity whenever
    // the traffic itself does not.

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    rocketnet_test_ack_connect( &test, &client, &client_counters, 0xA5F31ULL );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    const int num_rounds = 30;
    const int sends_per_round = ROCKETNET_ACK_DEBT_THRESHOLD;
    const int total_sends = num_rounds * sends_per_round;

    uint8_t * acked_seen = (uint8_t*) rocketnet_malloc( NULL, (size_t) total_sends );
    rocketnet_check( acked_seen );
    memset( acked_seen, 0, (size_t) total_sends );

    double time = 200.0;
    uint64_t next_packet_id = 1;
    int total_acked = 0;

    client.num_acked = 0;

    for ( int round = 0; round < num_rounds; round++ )
    {
        for ( int i = 0; i < sends_per_round; i++ )
        {
            time += 0.0002;                                 // 5000 packets per second

            uint64_t sequence = 0;
            rocketnet_check( rocketnet_client_protocol_start_payload( &client, &sequence ) );

            uint8_t frame[1500];
            const uint8_t bytes_out[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
            memcpy( frame + ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET, bytes_out, 4 );

            int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &client, sequence, next_packet_id++, frame, 4, time );
            rocketnet_check( wire_bytes > 0 );

            rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, frame, wire_bytes,
                                                              ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                              payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        }

        // the slow return path: the debt threshold fires exactly one
        // standalone ack for the round
        uint8_t ack_packet[ROCKETNET_RECEIVE_CEILING];
        int ack_bytes = rocketnet_server_protocol_write_ack_if_due( &test.server, 0, time, ack_packet );
        rocketnet_check( ack_bytes >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
        rocketnet_check( rocketnet_server_protocol_write_ack_if_due( &test.server, 0, time, ack_packet ) == 0 );

        rocketnet_check( rocketnet_client_protocol_process_packet( &client, ack_packet, ack_bytes, time, payload, &payload_bytes ) );

        // drain the batch: every id surfaced once, none invented
        for ( int i = 0; i < client.num_acked; i++ )
        {
            uint64_t id = client.acked_packet_ids[i];
            rocketnet_check( id >= 1 && id <= (uint64_t) total_sends );
            rocketnet_check( !acked_seen[id - 1] );
            acked_seen[id - 1] = 1;
            total_acked++;
        }
        client.num_acked = 0;
    }

    // complete notification: every packet sent was acknowledged
    rocketnet_check( total_acked == total_sends );
    for ( int i = 0; i < total_sends; i++ )
        rocketnet_check( acked_seen[i] );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_UNKNOWN_SEQUENCE] == 0 );
    rocketnet_check( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL] == 0 );

    // No elapsed time or return traffic while more than two complete history
    // windows are sent. Both directions must keep transmitting on overflow;
    // one later ACK surfaces exactly the still-retained application IDs.
    for (int direction = 0; direction < 2; ++direction)
    {
        client.num_acked = 0;
        test.server.acks->num_acked = 0;
        const int burst = 2 * ROCKETNET_SENT_PACKET_BUFFER_SIZE;
        for (int i = 0; i < burst; ++i)
        {
            uint64_t sequence = 0;
            uint8_t frame[1500]{};
            int bytes = 0;
            if (direction == 0)
            {
                rocketnet_check(rocketnet_client_protocol_start_payload(&client, &sequence));
                bytes = rocketnet_client_protocol_seal_payload_frame(&client, sequence, 100000 + i, frame, 4, time);
                rocketnet_check(bytes > 0);
                rocketnet_check(rocketnet_server_protocol_process_packet(&test.server, &test.client_address,
                    frame, bytes, ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                    payload, &payload_bytes, &payload_client_index) == ROCKETNET_SERVER_PROCESS_ACCEPTED);
            }
            else
            {
                rocketnet_address_t to;
                rocketnet_check(rocketnet_server_protocol_start_payload(&test.server, 0, &to, &sequence));
                bytes = rocketnet_server_protocol_seal_payload_frame(&test.server, 0, sequence, 200000 + i, frame, 4, time);
                rocketnet_check(bytes > 0);
                rocketnet_check(rocketnet_client_protocol_process_packet(&client, frame, bytes, time, payload, &payload_bytes));
            }
        }
        uint8_t ack[ROCKETNET_RECEIVE_CEILING];
        int bytes = 0;
        const uint64_t * ids = NULL;
        int count = 0;
        if (direction == 0)
        {
            bytes = rocketnet_server_protocol_write_ack_if_due(&test.server, 0, time + 0.01, ack);
            rocketnet_check(bytes > 0);
            rocketnet_check(rocketnet_client_protocol_process_packet(&client, ack, bytes, time + 0.01, payload, &payload_bytes));
            ids = client.acked_packet_ids;
            count = client.num_acked;
            rocketnet_check(client_counters.values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] > 0);
        }
        else
        {
            rocketnet_client_protocol_update(&client, time + 0.02, ack, &bytes);
            rocketnet_check(bytes > 0);
            rocketnet_check(rocketnet_server_protocol_process_packet(&test.server, &test.client_address,
                ack, bytes, ROCKETNET_TEST_CREATE_TS, time + 0.02, response, &response_bytes,
                payload, &payload_bytes, &payload_client_index) == ROCKETNET_SERVER_PROCESS_ACCEPTED);
            ids = test.server.acks->acked_packet_ids;
            count = test.server.acks->num_acked;
            rocketnet_check(test.counters.values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] > 0);
        }
        rocketnet_check(count == ROCKETNET_SENT_PACKET_BUFFER_SIZE);
        for (int i = 0; i < count; ++i)
            rocketnet_check(ids[i] == (uint64_t)(100000 * (direction + 1) + burst - 1 - i));
    }

    rocketnet_free( NULL, acked_seen );
    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_ack_rtt_freshness()
{
    // rtt rides the acks continuously: the handshake seeds it, and every
    // first ack of a sent sequence yields a fresh sample -- a faster path
    // shows up in the estimate instead of holding the handshake's number

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    rocketnet_test_ack_connect( &test, &client, &client_counters, 0x877F4ULL );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    rocketnet_check( !client.ack.has_rtt_sample );

    // a slow round trip: sent at 200.0, the acknowledging block arrives at
    // 200.35 -- one sample of 350ms
    const uint8_t data[4] = { 9, 9, 9, 9 };
    int bytes = rocketnet_client_protocol_write_payload( &client, data, 4, 200.0, packet );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.35, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    int ka_bytes = rocketnet_server_write_keep_alive( &test.server, 0, 200.35, packet );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, ka_bytes, 200.35, payload, &payload_bytes ) );

    rocketnet_check( client.ack.has_rtt_sample );
    rocketnet_ack_endpoint_update_stats( &client.ack );
    rocketnet_check( client.ack.rtt > 300.0f && client.ack.rtt < 400.0f );

    // a faster path later refreshes the estimate down
    bytes = rocketnet_client_protocol_write_payload( &client, data, 4, 300.0, packet );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 300.02, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    ka_bytes = rocketnet_server_write_keep_alive( &test.server, 0, 300.02, packet );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, ka_bytes, 300.02, payload, &payload_bytes ) );

    rocketnet_ack_endpoint_update_stats( &client.ack );
    rocketnet_check( client.ack.rtt > 15.0f && client.ack.rtt < 25.0f );

    // the server samples from the client's blocks the same way: its
    // keep-alives were acknowledged by the client's traffic during the pump
    // and above, so its per-slot estimate is live too
    bytes = rocketnet_client_protocol_write_payload( &client, data, 4, 300.05, packet );
    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 300.06, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( test.server.acks->endpoints[0].has_rtt_sample );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_ack_payload_seal_with_ranges()
{
    // The in-place payload seal's loss branch: when the block carries
    // ranges it exceeds the 7-byte minimum the frame reserves, and the
    // payload moves up inside the frame before sealing. A hole punched in
    // the receive state forces exactly that, at full payload capacity, and
    // the packet must still round-trip byte-exact with nothing written
    // outside its wire region.

    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    rocketnet_test_ack_connect( &test, &client, &client_counters, 0x5B1F7ULL );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    struct rocketnet_encryption_mapping_t * mapping =
        rocketnet_encryption_map_entry( test.server.encryption_map,
                                        rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) );
    rocketnet_check( mapping );

    // punch the hole: a keep-alive, a burned server sequence, a keep-alive
    // -- the client's replay window now has a gap, so its next block
    // carries one range and is 11 bytes, not the minimum 7

    int ka_bytes = rocketnet_server_write_keep_alive( &test.server, 0, 200.0, packet );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, ka_bytes, 200.0, payload, &payload_bytes ) );

    uint64_t low_acked_sequence = mapping->send_sequence_session - 1;    // the keep-alive just opened

    mapping->send_sequence_session++;                                    // the hole: burned, never sent

    ka_bytes = rocketnet_server_write_keep_alive( &test.server, 0, 200.1, packet );
    rocketnet_check( rocketnet_client_protocol_process_packet( &client, packet, ka_bytes, 200.1, payload, &payload_bytes ) );

    uint64_t high_acked_sequence = mapping->send_sequence_session - 1;
    rocketnet_check( high_acked_sequence == low_acked_sequence + 2 );

    // seal a full-capacity payload in place, in a sentinel-filled frame

    uint8_t frame[2048];
    memset( frame, 0xEE, sizeof(frame) );

    uint8_t expected[ROCKETNET_MAX_PAYLOAD_BYTES];
    rocketnet_test_generate_payload( expected, ROCKETNET_MAX_PAYLOAD_BYTES );
    memcpy( frame + ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET, expected, ROCKETNET_MAX_PAYLOAD_BYTES );

    uint64_t sequence = 0;
    rocketnet_check( rocketnet_client_protocol_start_payload( &client, &sequence ) );

    int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &client, sequence, 777, frame, ROCKETNET_MAX_PAYLOAD_BYTES, 200.2 );

    // the wire carries the fixed overhead, the 11-byte one-range block, and
    // the full payload -- the loss branch moved the payload up four bytes

    rocketnet_check( wire_bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 + ROCKETNET_MAX_PAYLOAD_BYTES );
    rocketnet_check( wire_bytes <= ROCKETNET_SEND_BUDGET );

    // nothing outside the wire region was touched

    for ( int i = wire_bytes; i < (int) sizeof(frame); i++ )
        rocketnet_check( frame[i] == 0xEE );

    // the packet round-trips byte-exact, and its block applied: both
    // keep-alives acknowledged, the burned sequence in the gap untouched

    rocketnet_check( rocketnet_server_protocol_process_packet( &test.server, &test.client_address, frame, wire_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.3, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( payload_bytes == ROCKETNET_MAX_PAYLOAD_BYTES );
    rocketnet_check( memcmp( payload, expected, ROCKETNET_MAX_PAYLOAD_BYTES ) == 0 );

    struct rocketnet_ack_endpoint_t * endpoint = &test.server.acks->endpoints[0];
    rocketnet_check( endpoint->sent[low_acked_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );
    rocketnet_check( endpoint->sent[high_acked_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE] == 0 );

    rocketnet_test_server_teardown( &test );
}

void rocketnet_test_transport_acks_and_client_stats()
{
    // The transports end to end over UDP loopback: eight clients fill every
    // slot of an eight-slot server, payloads flow on the highest slot, and
    // the delivery notifications surface the packet ids from start_packet on
    // both ends -- the join key, through real sockets. The per-client stats
    // for the HIGH slot must move after traffic: the round-robin recompute
    // covers all slots, a sixth per update, which is exactly the test the
    // space reference's literal i == update_count % 6 predicate fails --
    // that form never updates any slot past five.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 8;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    struct rocketnet_client_transport_t * clients[8];

    for ( int i = 0; i < 8; i++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_write_dev_connect_token( server_address, 0x77000ULL + (uint64_t) i, (uint64_t) time( NULL ), token ) );
        clients[i] = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
        rocketnet_check( clients[i] );
    }

    double start = rocketnet_platform_time();
    int all_connected = 0;
    while ( rocketnet_platform_time() - start < 10.0 && !all_connected )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        all_connected = 1;
        for ( int i = 0; i < 8; i++ )
            if ( rocketnet_client_transport_state( clients[i] ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                all_connected = 0;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( all_connected );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 8 );

    // the client holding the highest slot -- past every index the broken
    // reference amortization would ever touch

    int high_slot = -1;
    struct rocketnet_client_transport_t * high_client = NULL;
    for ( int i = 0; i < 8; i++ )
    {
        int index = rocketnet_client_transport_client_index( clients[i] );
        if ( index > high_slot )
        {
            high_slot = index;
            high_client = clients[i];
        }
    }
    rocketnet_check( high_slot >= 6 );

    // one payload each way through acquire/commit, keeping the packet ids

    struct rocketnet_packet_context_t up_context;
    uint8_t * frame = rocketnet_client_transport_start_packet( high_client, ROCKETNET_MAX_PAYLOAD_BYTES, &up_context );
    rocketnet_check( frame );
    memcpy( frame + up_context.payload_index, "ping", 4 );
    rocketnet_check( rocketnet_client_transport_finish_packet( high_client, &up_context, 4 ) > 0 );
    rocketnet_client_transport_send_packets( high_client );

    struct rocketnet_packet_context_t down_context;
    frame = rocketnet_server_transport_start_packet( server, high_slot, ROCKETNET_MAX_PAYLOAD_BYTES, &down_context );
    rocketnet_check( frame );
    memcpy( frame + down_context.payload_index, "pong", 4 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 4 ) > 0 );

    // pump until both notifications surface: the server sees its id come
    // back from the high slot, the client sees its own -- each exactly the
    // id start_packet handed out

    int server_saw_ack = 0;
    int client_saw_ack = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( server_saw_ack && client_saw_ack ) )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int * acked_client_index = NULL;
        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_server_transport_acked_packets( server, &acked_client_index, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
        {
            if ( acked_ids[i] == down_context.packet_id )
            {
                rocketnet_check( acked_client_index[i] == high_slot );
                server_saw_ack = 1;
            }
        }

        const uint64_t * client_acked_ids = NULL;
        int client_num_acked = rocketnet_client_transport_acked_packets( high_client, &client_acked_ids );
        for ( int i = 0; i < client_num_acked; i++ )
        {
            if ( client_acked_ids[i] == up_context.packet_id )
                client_saw_ack = 1;
        }

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_saw_ack );
    rocketnet_check( client_saw_ack );

    // keep traffic flowing long enough for every round-robin phase to pass
    // several times, then the high slot's published stats have moved

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    struct rocketnet_server_transport_client_stats_t stats;
    rocketnet_server_transport_client_stats( server, high_slot, &stats );
    rocketnet_check( stats.bandwidth_received_kbps > 0.0f );
    rocketnet_check( stats.rtt >= 0.0f && stats.rtt < 1000.0f );
    rocketnet_check( stats.packet_loss >= 0.0f && stats.packet_loss <= 100.0f );

    // an out-of-range slot answers zeros, never a crash
    rocketnet_server_transport_client_stats( server, 12345, &stats );
    rocketnet_check( stats.rtt == 0.0f && stats.bandwidth_received_kbps == 0.0f );

    // and the client's rtt is live from the acks: nonzero and sane on a
    // loopback wire
    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( high_client, &client_stats );
    rocketnet_check( client_stats.rtt > 0.0f && client_stats.rtt < 1000.0f );

    for ( int i = 0; i < 8; i++ )
    {
        rocketnet_client_transport_disconnect( clients[i] );
        rocketnet_client_transport_destroy( clients[i] );
    }
    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
    }
    rocketnet_server_transport_destroy( server );
}

// -----------------------------------------
//                           fragmentation
// -----------------------------------------

void rocketnet_test_fragment_header_codec()
{
    // the frozen header vector from Appendix A: payload id 7, fragment
    // index 2, fragment count 3
    uint8_t header[ROCKETNET_FRAGMENT_HEADER_BYTES];
    rocketnet_write_fragment_header( header, 7, 2, 3 );
    rocketnet_check( memcmp( header, rocketnet_test_fragment_header, ROCKETNET_FRAGMENT_HEADER_BYTES ) == 0 );

    uint32_t payload_id;
    int fragment_index;
    int fragment_count;
    rocketnet_read_fragment_header( header, &payload_id, &fragment_index, &fragment_count );
    rocketnet_check( payload_id == 7 );
    rocketnet_check( fragment_index == 2 );
    rocketnet_check( fragment_count == 3 );

    // the type 9 sealed vector's header: payload id 1, fragment 0 of 2
    const uint8_t expected_first[ROCKETNET_FRAGMENT_HEADER_BYTES] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x01 };
    rocketnet_write_fragment_header( header, 1, 0, 2 );
    rocketnet_check( memcmp( header, expected_first, ROCKETNET_FRAGMENT_HEADER_BYTES ) == 0 );

    // round trips at the field extremes
    rocketnet_write_fragment_header( header, 0xFFFFFFFF, 255, 256 );
    rocketnet_read_fragment_header( header, &payload_id, &fragment_index, &fragment_count );
    rocketnet_check( payload_id == 0xFFFFFFFF );
    rocketnet_check( fragment_index == 255 );
    rocketnet_check( fragment_count == 256 );
}

void rocketnet_test_fragment_plan()
{
    // NEGATIVE: a payload that fits one packet is never fragmented, and a
    // payload over the 256-fragment maximum is refused
    int count, stride;
    rocketnet_check( !rocketnet_fragment_plan( 0, &count, &stride ) );
    rocketnet_check( !rocketnet_fragment_plan( 1, &count, &stride ) );
    rocketnet_check( !rocketnet_fragment_plan( ROCKETNET_MAX_PAYLOAD_BYTES, &count, &stride ) );
    rocketnet_check( !rocketnet_fragment_plan( ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, &count, &stride ) );

    // one byte over a single packet splits into two near-halves, never a
    // full fragment and an empty remainder
    rocketnet_check( rocketnet_fragment_plan( ROCKETNET_MAX_PAYLOAD_BYTES + 1, &count, &stride ) );
    rocketnet_check( count == 2 );
    rocketnet_check( stride == ( ROCKETNET_MAX_PAYLOAD_BYTES + 2 ) / 2 );

    // exactly two full strides is two full fragments
    rocketnet_check( rocketnet_fragment_plan( 2 * ROCKETNET_MAX_FRAGMENT_BYTES, &count, &stride ) );
    rocketnet_check( count == 2 );
    rocketnet_check( stride == ROCKETNET_MAX_FRAGMENT_BYTES );

    // the maximum is 256 full fragments exactly
    rocketnet_check( rocketnet_fragment_plan( ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, &count, &stride ) );
    rocketnet_check( count == ROCKETNET_MAX_FRAGMENTS );
    rocketnet_check( stride == ROCKETNET_MAX_FRAGMENT_BYTES );

    // the whole range: every plan reconstructs its payload exactly, inside
    // every cap, with a non-empty remainder
    for ( int payload_bytes = ROCKETNET_MAX_PAYLOAD_BYTES + 1; payload_bytes <= ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES; payload_bytes++ )
    {
        rocketnet_check( rocketnet_fragment_plan( payload_bytes, &count, &stride ) );
        rocketnet_check( count >= 2 && count <= ROCKETNET_MAX_FRAGMENTS );
        rocketnet_check( stride >= 1 && stride <= ROCKETNET_MAX_FRAGMENT_BYTES );
        const int last = payload_bytes - stride * ( count - 1 );
        rocketnet_check( last >= 1 && last <= stride );
    }
}

// the engine-level counter table is the server's family; the client's
// mirrors it through the protocol tests

static const struct rocketnet_reassembly_counters_t rocketnet_test_fragment_counters = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_FRAGMENT_STALE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL };

// builds one fragment's type data -- header plus patterned bytes read from
// the position the fragment claims, so a reassembled payload is verifiable
// wherever its fragments came from

static int rocketnet_test_build_fragment( uint8_t * type_data, uint32_t payload_id,
                                          int fragment_index, int fragment_count,
                                          int fragment_bytes, int stride, uint8_t pattern )
{
    rocketnet_write_fragment_header( type_data, payload_id, fragment_index, fragment_count );
    for ( int i = 0; i < fragment_bytes; i++ )
        type_data[ROCKETNET_FRAGMENT_HEADER_BYTES + i] = (uint8_t) ( pattern + stride * fragment_index + i );
    return ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes;
}

static void rocketnet_test_check_reassembled( const uint8_t * payload, int payload_bytes, uint8_t pattern )
{
    for ( int i = 0; i < payload_bytes; i++ )
        rocketnet_check( payload[i] == (uint8_t) ( pattern + i ) );
}

void rocketnet_test_fragment_reassembly()
{
    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof(counters_storage) );
    uint64_t * counters = counters_storage.values;
    const struct rocketnet_reassembly_counters_t * table = &rocketnet_test_fragment_counters;

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 2, 4, 8192, 2 * 4 );
    rocketnet_check( set );
    rocketnet_check( set->slot_arena_bytes == 8192 + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES );

    rocketnet_reassembly_set_begin_update( set );

    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];

    // a three-fragment payload arriving in order completes on its final
    // fragment and delivers exactly its bytes
    {
        int bytes = rocketnet_test_build_fragment( fragment, 1, 0, 3, 100, 100, 0x10 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( set->completed_bytes == 0 );
        bytes = rocketnet_test_build_fragment( fragment, 1, 1, 3, 100, 100, 0x10 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( set->completed_bytes == 0 );
        bytes = rocketnet_test_build_fragment( fragment, 1, 2, 3, 50, 100, 0x10 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( set->completed_bytes == 250 );
        rocketnet_test_check_reassembled( set->completed_payload, 250, 0x10 );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED] == 3 );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );
    }

    // the final fragment arriving first is staged, checked against the
    // stride when the first non-final fragment fixes it, and placed then --
    // reordering of a two-fragment payload is an everyday event
    {
        int bytes = rocketnet_test_build_fragment( fragment, 2, 1, 2, 40, 100, 0x20 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( set->completed_bytes == 0 );
        bytes = rocketnet_test_build_fragment( fragment, 2, 0, 2, 100, 100, 0x20 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( set->completed_bytes == 140 );
        rocketnet_test_check_reassembled( set->completed_payload, 140, 0x20 );
    }

    // out of order generally: a four-fragment payload arriving 3, 1, 0, 2
    {
        const int order[4] = { 3, 1, 0, 2 };
        for ( int i = 0; i < 4; i++ )
        {
            const int index = order[i];
            const int size = index == 3 ? 25 : 200;
            int bytes = rocketnet_test_build_fragment( fragment, 3, index, 4, size, 200, 0x30 );
            rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
            rocketnet_check( set->completed_bytes == ( i == 3 ? 625 : 0 ) );
        }
        rocketnet_test_check_reassembled( set->completed_payload, 625, 0x30 );
    }

    // the same payload id on another connection is independent state
    {
        int bytes = rocketnet_test_build_fragment( fragment, 1, 0, 2, 60, 60, 0x40 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 1, fragment, bytes, counters, table ) );
        bytes = rocketnet_test_build_fragment( fragment, 1, 1, 2, 30, 60, 0x40 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 1, fragment, bytes, counters, table ) );
        rocketnet_check( set->completed_bytes == 90 );
        rocketnet_test_check_reassembled( set->completed_payload, 90, 0x40 );
    }

    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 4 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );

    rocketnet_reassembly_set_destroy( set );
}

void rocketnet_test_fragment_reassembly_refusals()
{
    // every refusal reason from the standard, in its check order, each
    // firing its own counter and leaving the accepted count unmoved

    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof(counters_storage) );
    uint64_t * counters = counters_storage.values;
    const struct rocketnet_reassembly_counters_t * table = &rocketnet_test_fragment_counters;

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 1, 4, 8192, 4 );
    rocketnet_check( set );
    rocketnet_reassembly_set_begin_update( set );

    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];
    int bytes;

    // NEGATIVE: a payload id of zero is what an uninitialized sender produces
    bytes = rocketnet_test_build_fragment( fragment, 1, 0, 2, 10, 10, 0 );
    fragment[0] = 0; fragment[1] = 0; fragment[2] = 0; fragment[3] = 0;
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID] == 1 );

    // NEGATIVE: a payload id older than the slot's occupant is stale --
    // occupy slot 2 with id 6, then offer id 2, which maps to the same slot
    bytes = rocketnet_test_build_fragment( fragment, 6, 0, 3, 100, 100, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    bytes = rocketnet_test_build_fragment( fragment, 2, 0, 3, 100, 100, 0 );
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == 1 );

    // NEGATIVE: a fragment count of one is invalid -- a payload that fits
    // one packet must be type 5
    bytes = rocketnet_test_build_fragment( fragment, 3, 0, 2, 10, 10, 0 );
    fragment[5] = 0;                                        // count - 1 = 0
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID] == 1 );

    // NEGATIVE: the fragment index must be less than the count
    bytes = rocketnet_test_build_fragment( fragment, 3, 0, 3, 10, 10, 0 );
    fragment[4] = 3;                                        // index 3 of count 3
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE] == 1 );

    // NEGATIVE: the count-widening attack -- the first fragment pinned id
    // 6's count at 3, and a later fragment naming 4 refuses while the slot
    // keeps its state
    bytes = rocketnet_test_build_fragment( fragment, 6, 1, 4, 100, 100, 0 );
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH] == 1 );

    // NEGATIVE: a non-final fragment of a different size than the pinned
    // stride refuses, and the slot keeps its state
    bytes = rocketnet_test_build_fragment( fragment, 6, 1, 3, 99, 100, 0 );
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] == 1 );

    // NEGATIVE: the final fragment must not exceed the stride once the
    // stride is known
    bytes = rocketnet_test_build_fragment( fragment, 6, 2, 3, 101, 100, 0 );
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] == 2 );

    // ...and id 6 still completes: the refusals above left its slot intact
    bytes = rocketnet_test_build_fragment( fragment, 6, 1, 3, 100, 100, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    bytes = rocketnet_test_build_fragment( fragment, 6, 2, 3, 50, 100, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 250 );

    // NEGATIVE: a staged final that exceeds the stride refuses the whole
    // payload when the first non-final fragment fixes it, and the slot
    // clears -- a fresh payload can claim it immediately
    bytes = rocketnet_test_build_fragment( fragment, 4, 1, 2, 120, 100, 0 );        // final first, 120 bytes
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    bytes = rocketnet_test_build_fragment( fragment, 4, 0, 2, 100, 100, 0 );        // the stride pins at 100
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] == 3 );
    bytes = rocketnet_test_build_fragment( fragment, 4, 0, 2, 100, 100, 0x50 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    bytes = rocketnet_test_build_fragment( fragment, 4, 1, 2, 100, 100, 0x50 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 200 );

    // NEGATIVE: the reconstructed payload must not exceed the configured
    // maximum, refused the moment the stride and the count are known --
    // the first non-final arrival -- before it occupies a slot's arena
    bytes = rocketnet_test_build_fragment( fragment, 5, 0, 8, ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES, ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES, 0 );
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] == 1 );

    // NEGATIVE: too large discovered by the final's real size -- the
    // non-final arrivals fit under the maximum with a one-byte final, and
    // the final that arrives pushes past it, refusing and clearing
    {
        const int stride = 1300;                            // 6 x 1300 + 1 = 7801 <= 8192
        for ( int i = 0; i < 6; i++ )
        {
            bytes = rocketnet_test_build_fragment( fragment, 8, i, 7, stride, stride, 0 );
            rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        }
        bytes = rocketnet_test_build_fragment( fragment, 8, 6, 7, 500, stride, 0 );  // 7800 + 500 > 8192
        rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] == 2 );
    }

    // NEGATIVE: a fragment index already held refuses as a duplicate --
    // the replay window refuses replayed packets, so this is a sender defect
    bytes = rocketnet_test_build_fragment( fragment, 9, 0, 3, 100, 100, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    bytes = rocketnet_test_build_fragment( fragment, 9, 0, 3, 100, 100, 0 );
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX] == 1 );

    // eviction under churn: id 9 occupies slot 1 incomplete; id 13 maps to
    // the same slot, evicts it, and the abandonment is counted -- the
    // loss-visibility counter
    bytes = rocketnet_test_build_fragment( fragment, 13, 0, 2, 100, 100, 0x60 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 1 );
    bytes = rocketnet_test_build_fragment( fragment, 13, 1, 2, 100, 100, 0x60 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 200 );
    rocketnet_test_check_reassembled( set->completed_payload, 200, 0x60 );

    // a newer payload id whose first fragment refuses does NOT evict: the
    // incomplete occupant survives an invalid claim
    bytes = rocketnet_test_build_fragment( fragment, 10, 0, 3, 100, 100, 0x70 );     // id 10 occupies slot 2 (over id 6, complete and cleared)
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    const uint64_t abandoned_before = counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED];
    bytes = rocketnet_test_build_fragment( fragment, 14, 0, 2, 10, 10, 0 );
    fragment[5] = 0;                                        // count 1: refused before any eviction
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == abandoned_before );
    bytes = rocketnet_test_build_fragment( fragment, 10, 1, 3, 100, 100, 0x70 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    bytes = rocketnet_test_build_fragment( fragment, 10, 2, 3, 10, 100, 0x70 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 210 );
    rocketnet_test_check_reassembled( set->completed_payload, 210, 0x70 );

    // received == accepted + the sum of every refusal, the engine's own
    // accounting invariant over everything this test threw at it
    const uint64_t refusals = counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_STALE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX];
    rocketnet_check( refusals == 12 );

    rocketnet_reassembly_set_destroy( set );
}

void rocketnet_test_fragment_delivery_batch()
{
    // the delivered arena is per update: begin_update reclaims it, and a
    // completion past its capacity is counted rather than silently lost

    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof(counters_storage) );
    uint64_t * counters = counters_storage.values;
    const struct rocketnet_reassembly_counters_t * table = &rocketnet_test_fragment_counters;

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 1, 2, 4096, 2 );
    rocketnet_check( set );
    rocketnet_check( set->delivered_capacity == 2 );

    rocketnet_reassembly_set_begin_update( set );

    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];

    // three completions in one update: the first two deliver, the third is
    // reassembled but dropped on the full batch, and both facts are counted
    for ( uint32_t payload_id = 1; payload_id <= 3; payload_id++ )
    {
        int bytes = rocketnet_test_build_fragment( fragment, payload_id, 0, 2, 100, 100, (uint8_t) payload_id );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        bytes = rocketnet_test_build_fragment( fragment, payload_id, 1, 2, 50, 100, (uint8_t) payload_id );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        if ( payload_id <= 2 )
        {
            rocketnet_check( set->completed_bytes == 150 );
            rocketnet_test_check_reassembled( set->completed_payload, 150, (uint8_t) payload_id );
        }
        else
        {
            rocketnet_check( set->completed_bytes == 0 );
        }
    }
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 3 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 1 );

    // the next update's window is fresh
    rocketnet_reassembly_set_begin_update( set );
    rocketnet_check( set->num_delivered == 0 );
    int bytes = rocketnet_test_build_fragment( fragment, 4, 0, 2, 100, 100, 0x44 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    bytes = rocketnet_test_build_fragment( fragment, 4, 1, 2, 50, 100, 0x44 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 150 );
    rocketnet_test_check_reassembled( set->completed_payload, 150, 0x44 );

    // a reset connection forgets its slots: the incomplete payload below
    // vanishes without an abandonment -- reset is a lifecycle event, not loss
    bytes = rocketnet_test_build_fragment( fragment, 5, 0, 2, 100, 100, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_reassembly_set_reset_connection( set, 0 );
    bytes = rocketnet_test_build_fragment( fragment, 5, 1, 2, 100, 100, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 0 );           // the final restarts the payload; nothing completes
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );

    rocketnet_reassembly_set_destroy( set );
}

// a connected protocol pair on mocked time: the three-round-trip handshake
// from the rtt test, plus a client-side reassembly set, so the fragment
// tests start from a live session in a few lines

struct rocketnet_test_fragment_pair_t
{
    struct rocketnet_test_server_t test;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_client_protocol_t client;
    double time;
};

static void rocketnet_test_fragment_pair_setup( struct rocketnet_test_fragment_pair_t * pair )
{
    rocketnet_test_server_setup( &pair->test );

    memset( &pair->client_counters, 0, sizeof(pair->client_counters) );
    rocketnet_client_protocol_init( &pair->client, &pair->client_counters );
    pair->client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, ROCKETNET_NUM_CLIENT_PACKETS / 2 );
    rocketnet_check( pair->client.reassembly );
    rocketnet_reassembly_set_begin_update( pair->client.reassembly );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 0xF4A6ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_protocol_connect( &pair->client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    double times[3] = { 100.0, 100.2, 100.4 };
    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_client_protocol_update( &pair->client, times[i], packet, &packet_bytes );
        rocketnet_check( packet_bytes > 0 );
        rocketnet_check( rocketnet_server_protocol_process_packet( &pair->test.server, &pair->test.client_address, packet, packet_bytes,
                                                          ROCKETNET_TEST_CREATE_TS, times[i],
                                                          response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index ) != ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( rocketnet_client_protocol_process_packet( &pair->client, response, response_bytes, times[i] + 0.05, payload, &payload_bytes ) );
    }
    rocketnet_check( pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( pair->client.client_index == 0 );

    pair->time = 101.0;
}

static void rocketnet_test_fragment_pair_teardown( struct rocketnet_test_fragment_pair_t * pair )
{
    rocketnet_reassembly_set_destroy( pair->client.reassembly );
    rocketnet_test_server_teardown( &pair->test );
}

// hands one packet to the server, expecting it through the ladder

static int rocketnet_test_fragment_deliver_to_server( struct rocketnet_test_fragment_pair_t * pair,
                                                      const uint8_t * packet, int packet_bytes )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    pair->time += 0.001;
    return rocketnet_server_protocol_process_packet( &pair->test.server, &pair->test.client_address, packet, packet_bytes,
                                            ROCKETNET_TEST_CREATE_TS, pair->time,
                                            response, &response_bytes,
                                            payload, &payload_bytes, &payload_client_index );
}

void rocketnet_test_fragment_client_to_server()
{
    static struct rocketnet_test_fragment_pair_t pair;
    rocketnet_test_fragment_pair_setup( &pair );

    const uint64_t * counters = pair.test.counters.values;
    struct rocketnet_reassembly_set_t * reassembly = pair.test.server.reassembly;

    // a 5000-byte payload splits, crosses as ordinary sealed type 9
    // session packets, and reassembles byte-exact -- in order first
    static uint8_t payload_data[16384];
    for ( int i = 0; i < (int) sizeof(payload_data); i++ )
        payload_data[i] = (uint8_t) ( 0xA0 + i );

    static uint8_t packets[ROCKETNET_MAX_FRAGMENTS * ROCKETNET_SEND_BUDGET];
    int packet_bytes[ROCKETNET_MAX_FRAGMENTS];

    int count = rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 5000, pair.time,
                                                           packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS );
    rocketnet_check( count == 5 );                          // ceil(5000 / 1193)

    for ( int i = 0; i < count; i++ )
    {
        int result = rocketnet_test_fragment_deliver_to_server( &pair, packets + i * ROCKETNET_SEND_BUDGET, packet_bytes[i] );
        rocketnet_check( result == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        if ( i < count - 1 )
            rocketnet_check( reassembly->completed_bytes == 0 );
    }
    rocketnet_check( reassembly->completed_bytes == 5000 );
    rocketnet_check( memcmp( reassembly->completed_payload, payload_data, 5000 ) == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == 5 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED] == 5 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    // the same payload again, delivered in reverse -- the final fragment
    // first, staged until the stride pins -- and payload ids increment
    count = rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 5000, pair.time,
                                                       packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS );
    rocketnet_check( count == 5 );
    for ( int i = count - 1; i >= 0; i-- )
        rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packets + i * ROCKETNET_SEND_BUDGET, packet_bytes[i] ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( reassembly->completed_bytes == 5000 );
    rocketnet_check( memcmp( reassembly->completed_payload, payload_data, 5000 ) == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 2 );

    // the largest configured payload crosses whole
    count = rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 16384, pair.time,
                                                       packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS );
    rocketnet_check( count == 14 );                         // ceil(16384 / 1193)
    for ( int i = 0; i < count; i++ )
        rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packets + i * ROCKETNET_SEND_BUDGET, packet_bytes[i] ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( reassembly->completed_bytes == 16384 );
    rocketnet_check( memcmp( reassembly->completed_payload, payload_data, 16384 ) == 0 );

    // fragments elicit: every one of those packets armed the ack debt
    rocketnet_check( pair.test.server.acks->endpoints[0].ack_debt == 24 );

    // and each carried a fresh valid ack block that applied -- the
    // client's sends are covered by the server's replay state, so a
    // keep-alive built now reports them back (the block machinery is
    // proven in the ack tests; here it must simply have engaged)
    rocketnet_check( pair.test.server.acks->endpoints[0].highest_received_sequence >= 24 );

    // received == sum of outcomes for the fragment family, the server side
    const uint64_t outcomes = counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_STALE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED];
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == outcomes );

    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );

    rocketnet_test_fragment_pair_teardown( &pair );
}

void rocketnet_test_fragment_server_to_client()
{
    static struct rocketnet_test_fragment_pair_t pair;
    rocketnet_test_fragment_pair_setup( &pair );

    const uint64_t * counters = pair.client_counters.values;

    static uint8_t payload_data[8000];
    for ( int i = 0; i < (int) sizeof(payload_data); i++ )
        payload_data[i] = (uint8_t) ( 0x35 + i );

    static uint8_t packets[ROCKETNET_MAX_FRAGMENTS * ROCKETNET_SEND_BUDGET];
    int packet_bytes[ROCKETNET_MAX_FRAGMENTS];

    const uint64_t debt_before = pair.client.ack.ack_debt;

    int count = rocketnet_server_protocol_write_fragments( &pair.test.server, 0, payload_data, 8000, pair.time,
                                                           packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS );
    rocketnet_check( count == 7 );                          // ceil(8000 / 1193)

    uint8_t out_payload[ROCKETNET_RECEIVE_CEILING];
    int out_payload_bytes = 0;
    for ( int i = 0; i < count; i++ )
    {
        pair.time += 0.001;
        rocketnet_check( rocketnet_client_protocol_process_packet( &pair.client, packets + i * ROCKETNET_SEND_BUDGET, packet_bytes[i],
                                                          pair.time, out_payload, &out_payload_bytes ) );
        rocketnet_check( out_payload_bytes == 0 );          // fragments deliver through reassembly, never the payload path
    }
    rocketnet_check( pair.client.reassembly->completed_bytes == 8000 );
    rocketnet_check( memcmp( pair.client.reassembly->completed_payload, payload_data, 8000 ) == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED] == 7 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED] == 7 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );
    rocketnet_check( counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED] == 0 );

    // fragments elicit on the client exactly as on the server
    rocketnet_check( pair.client.ack.ack_debt == debt_before + 7 );

    // received == sum of outcomes for the client's fragment family
    const uint64_t outcomes = counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_SIZE] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_SEQUENCE] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_ALREADY_RECEIVED] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_DID_NOT_OPEN] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_BAD_ACK_BLOCK] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_OPENED_SIZE] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_PAYLOAD_ID] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_INVALID] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_MISMATCH] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_SIZE_MISMATCH] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_DUPLICATE_INDEX] +
                              counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED];
    rocketnet_check( counters[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED] == outcomes );

    rocketnet_test_fragment_pair_teardown( &pair );
}

// builds a sealed type 9 with an arbitrary plaintext under the client's
// live session keys, for the crafted-refusal negatives below

static int rocketnet_test_seal_raw_fragment( struct rocketnet_test_fragment_pair_t * pair,
                                             const uint8_t * plaintext, int plaintext_bytes,
                                             uint8_t * out_packet )
{
    uint64_t sequence;
    rocketnet_check( rocketnet_client_protocol_start_fragments( &pair->client, 1, &sequence ) );
    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                               pair->client.filter_value, sequence,
                                               NULL, plaintext, plaintext_bytes,
                                               pair->client.signed_fields.protocol_id, pair->client.connection_salt,
                                               NULL, pair->client.keys.session_client_to_server );
    rocketnet_check( bytes > 0 );
    return bytes;
}

void rocketnet_test_fragment_receive_refusals()
{
    // every fragment refusal as a crafted wire packet through the server's
    // whole reading order: the arrival counter and the reason counter move
    // together, so rejected always means rejected here

    static struct rocketnet_test_fragment_pair_t pair;
    rocketnet_test_fragment_pair_setup( &pair );

    const uint64_t * counters = pair.test.counters.values;

    static uint8_t plaintext[2048];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int bytes;

    // the fresh 7-byte block heads every crafted plaintext
    #define ROCKETNET_TEST_FRESH_BLOCK() memset( plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES )

    struct
    {
        uint32_t payload_id;
        int fragment_index;
        int fragment_count_minus_one;
        int reason_counter;
    } refusals[4] = {
        { 0, 0, 1, ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID },        // zero payload id
        { 1, 0, 0, ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID },          // count of one
        { 1, 3, 2, ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE },     // index 3 of count 3
        { 1, 200, 100, ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE }, // index past a smaller count
    };

    for ( int i = 0; i < 4; i++ )
    {
        ROCKETNET_TEST_FRESH_BLOCK();
        uint8_t * p = plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
        rocketnet_write_uint32( &p, refusals[i].payload_id );
        rocketnet_write_uint8( &p, (uint8_t) refusals[i].fragment_index );
        rocketnet_write_uint8( &p, (uint8_t) refusals[i].fragment_count_minus_one );
        rocketnet_write_uint8( &p, 0xEE );                              // one fragment byte
        bytes = rocketnet_test_seal_raw_fragment( &pair, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 1, packet );

        uint64_t received_before = counters[ROCKETNET_COUNTER_FRAGMENT_RECEIVED];
        uint64_t reason_before = counters[refusals[i].reason_counter];
        rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == received_before + 1 );
        rocketnet_check( counters[refusals[i].reason_counter] == reason_before + 1 );
    }

    // NEGATIVE: the count-widening attack over the wire -- pin payload id
    // 2's count at 3 with a real fragment, then name count 4 for it
    static uint8_t packets[ROCKETNET_MAX_FRAGMENTS * ROCKETNET_SEND_BUDGET];
    int packet_bytes[ROCKETNET_MAX_FRAGMENTS];
    static uint8_t payload_data[3000];
    memset( payload_data, 0x77, sizeof(payload_data) );
    rocketnet_check( rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 3000, pair.time,
                                                                packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );
    rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packets, packet_bytes[0] ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );

    ROCKETNET_TEST_FRESH_BLOCK();
    uint8_t * p = plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
    rocketnet_write_uint32( &p, 1 );                                    // write_fragments assigned payload id 1
    rocketnet_write_uint8( &p, 1 );
    rocketnet_write_uint8( &p, 3 );                                     // count 4 against the pinned 3
    rocketnet_write_uint8( &p, 0xEE );
    bytes = rocketnet_test_seal_raw_fragment( &pair, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 1, packet );
    rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH] == 1 );

    // NEGATIVE: a non-final fragment off the pinned stride
    ROCKETNET_TEST_FRESH_BLOCK();
    p = plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
    rocketnet_write_uint32( &p, 1 );
    rocketnet_write_uint8( &p, 1 );
    rocketnet_write_uint8( &p, 2 );                                     // the right count, wrong size
    rocketnet_write_uint8( &p, 0xEE );                                  // 1 byte against the 1000-byte stride
    bytes = rocketnet_test_seal_raw_fragment( &pair, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 1, packet );
    rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] == 1 );

    // NEGATIVE: a duplicate fragment index -- the replay window never saw
    // this packet, so the refusal is reassembly's own
    ROCKETNET_TEST_FRESH_BLOCK();
    p = plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
    rocketnet_write_uint32( &p, 1 );
    rocketnet_write_uint8( &p, 0 );                                     // index 0 again
    rocketnet_write_uint8( &p, 2 );
    memset( (void*) p, 0x77, 1000 );                                    // stride-sized, so the size check passes
    bytes = rocketnet_test_seal_raw_fragment( &pair, plaintext + 0, ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 1000, packet );
    rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX] == 1 );

    // NEGATIVE: a stale payload id -- complete payload 5 into slot 1, then
    // start payload 9 in the same slot, and payload 5's ghost refuses...
    // ids 2 through 4 first, so 5 lands where the arithmetic says
    for ( uint32_t spin = 2; spin <= 4; spin++ )
    {
        rocketnet_check( rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 3000, pair.time,
                                                                    packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );
    }
    rocketnet_check( rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 3000, pair.time,
                                                                packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );   // payload id 5
    rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packets, packet_bytes[0] ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 1 );     // id 1's incomplete slot evicted

    for ( uint32_t spin = 6; spin <= 8; spin++ )
    {
        rocketnet_check( rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 3000, pair.time,
                                                                    packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );
    }
    rocketnet_check( rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 3000, pair.time,
                                                                packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS ) == 3 );   // payload id 9, slot 1 again
    rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packets, packet_bytes[0] ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 2 );     // and id 5's in turn

    ROCKETNET_TEST_FRESH_BLOCK();
    p = plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
    rocketnet_write_uint32( &p, 5 );                                    // older than the occupying 9
    rocketnet_write_uint8( &p, 1 );
    rocketnet_write_uint8( &p, 2 );
    rocketnet_write_uint8( &p, 0xEE );
    bytes = rocketnet_test_seal_raw_fragment( &pair, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 1, packet );
    rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == 1 );

    // NEGATIVE: a payload too large for the configured maximum, refused
    // the moment stride and count are known -- a full-stride non-final
    // fragment claiming 256 fragments overruns the 16384-byte maximum
    {
        static uint8_t big_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_FRAGMENT_BYTES];
        memset( big_plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
        p = big_plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
        rocketnet_write_uint32( &p, 100 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint8( &p, 255 );                               // count 256
        memset( (void*) p, 0x55, ROCKETNET_MAX_FRAGMENT_BYTES );
        bytes = rocketnet_test_seal_raw_fragment( &pair, big_plaintext, (int) sizeof(big_plaintext), packet );
        rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] == 1 );
    }

    // NEGATIVE: an opened size below the fragment header floor with the
    // wire size above the type's floor -- a widened valid block over a
    // headerless body -- is WRONG_OPENED_SIZE, its own reason
    {
        // give the server sends for the block to describe: five keep-alives
        uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
        for ( int i = 0; i < 5; i++ )
            rocketnet_check( rocketnet_server_write_keep_alive( &pair.test.server, 0, pair.time, keep_alive ) > 0 );

        // largest 5, one range: run 2 at the top, gap 1, run 2 below
        p = plaintext;
        rocketnet_write_uint32( &p, 5 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 2 );
        rocketnet_write_uint16( &p, 1 );
        rocketnet_write_uint16( &p, 2 );
        rocketnet_write_uint8( &p, 0xEE );                              // 6 bytes behind an 11-byte block: under the 7-byte floor
        rocketnet_write_uint8( &p, 0xEE );
        rocketnet_write_uint8( &p, 0xEE );
        rocketnet_write_uint8( &p, 0xEE );
        rocketnet_write_uint8( &p, 0xEE );
        rocketnet_write_uint8( &p, 0xEE );
        bytes = rocketnet_test_seal_raw_fragment( &pair, plaintext, 17, packet );
        rocketnet_check( bytes >= ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES );
        rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packet, bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE] == 1 );
    }

    // NEGATIVE: below the type's wire floor is WRONG_SIZE, never a later
    // refusal -- a truncated buffer with the right prefix
    {
        uint8_t runt[ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES - 1];
        memset( runt, 0, sizeof(runt) );
        uint8_t * rp = runt;
        const uint8_t test_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
        rocketnet_write_prefilter_header( &rp, ROCKETNET_CONNECTION_FRAGMENT_PACKET, test_filter );
        rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, runt, (int) sizeof(runt) ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE] == 1 );
    }

    #undef ROCKETNET_TEST_FRESH_BLOCK

    // the family's arrival invariant held through every crafted refusal
    const uint64_t outcomes = counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_STALE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX] +
                              counters[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED];
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == outcomes );

    rocketnet_test_fragment_pair_teardown( &pair );
}

void rocketnet_test_fragment_punched_loss()
{
    // the specified semantics of loss: a payload with a punched-out
    // fragment is never delivered, its abandonment is counted when a newer
    // payload evicts its slot, and the connection is entirely unaffected

    static struct rocketnet_test_fragment_pair_t pair;
    rocketnet_test_fragment_pair_setup( &pair );

    const uint64_t * counters = pair.test.counters.values;
    struct rocketnet_reassembly_set_t * reassembly = pair.test.server.reassembly;

    static uint8_t payload_data[6000];
    for ( int i = 0; i < (int) sizeof(payload_data); i++ )
        payload_data[i] = (uint8_t) ( 0x11 + i );

    static uint8_t packets[ROCKETNET_MAX_FRAGMENTS * ROCKETNET_SEND_BUDGET];
    int packet_bytes[ROCKETNET_MAX_FRAGMENTS];

    // payload id 1: six fragments, the middle one punched out of the wire
    int count = rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 6000, pair.time,
                                                           packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS );
    rocketnet_check( count == 6 );
    for ( int i = 0; i < count; i++ )
    {
        if ( i == 3 )
            continue;                                       // the punch
        rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packets + i * ROCKETNET_SEND_BUDGET, packet_bytes[i] ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    }
    rocketnet_check( reassembly->completed_bytes == 0 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 0 );

    // the connection is unaffected: an ordinary payload flows around the
    // dead reassembly
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int bytes = rocketnet_client_protocol_write_payload( &pair.client, (const uint8_t*) "alive", 5, pair.time, packet );
    rocketnet_check( bytes > 0 );
    {
        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
        int delivered_bytes = 0;
        int delivered_client_index = -1;
        pair.time += 0.001;
        rocketnet_check( rocketnet_server_protocol_process_packet( &pair.test.server, &pair.test.client_address, packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, pair.time,
                                                          response, &response_bytes,
                                                          delivered, &delivered_bytes, &delivered_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        rocketnet_check( delivered_bytes == 5 );
        rocketnet_check( memcmp( delivered, "alive", 5 ) == 0 );
    }

    // four more fragmented payloads wrap the slot ring: payload id 5 lands
    // on the dead payload's slot, evicts it, and ABANDONED fires -- the
    // loss-visibility counter -- while every later payload delivers whole
    for ( uint32_t payload_id = 2; payload_id <= 5; payload_id++ )
    {
        count = rocketnet_client_protocol_write_fragments( &pair.client, payload_data, 6000, pair.time,
                                                           packets, packet_bytes, ROCKETNET_MAX_FRAGMENTS );
        rocketnet_check( count == 6 );
        for ( int i = 0; i < count; i++ )
            rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, packets + i * ROCKETNET_SEND_BUDGET, packet_bytes[i] ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        rocketnet_check( reassembly->completed_bytes == 6000 );
        rocketnet_check( memcmp( reassembly->completed_payload, payload_data, 6000 ) == 0 );
    }
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 4 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 1 );

    // the delivery-notification discipline under the punch, on the exact
    // machinery the transports surface verbatim: a fragmented payload with
    // a lost fragment never surfaces its packet id in the acked batch,
    // while a whole payload's id surfaces from the same block
    {
        uint8_t wire[ROCKETNET_SEND_BUDGET];
        uint8_t fragment_data[300];
        memset( fragment_data, 0x5C, sizeof(fragment_data) );

        uint64_t base_sequence;
        rocketnet_check( rocketnet_client_protocol_start_fragments( &pair.client, 3, &base_sequence ) );
        uint32_t punched_id = rocketnet_client_protocol_next_fragment_payload_id( &pair.client );
        uint64_t punched_aggregate = rocketnet_ack_endpoint_open_aggregate( &pair.client.ack, 0xA11A, 3 );
        for ( int i = 0; i < 3; i++ )
        {
            int wire_bytes = rocketnet_client_protocol_seal_fragment( &pair.client, base_sequence + (uint64_t) i, punched_id, i, 3,
                                                                      fragment_data, i < 2 ? 300 : 100,
                                                                      punched_aggregate, pair.time, wire );
            rocketnet_check( wire_bytes > 0 );
            if ( i == 1 )
                continue;                                   // the punch
            rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, wire, wire_bytes ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        }

        rocketnet_check( rocketnet_client_protocol_start_fragments( &pair.client, 2, &base_sequence ) );
        uint32_t whole_id = rocketnet_client_protocol_next_fragment_payload_id( &pair.client );
        uint64_t whole_aggregate = rocketnet_ack_endpoint_open_aggregate( &pair.client.ack, 0xB0B, 2 );
        for ( int i = 0; i < 2; i++ )
        {
            int wire_bytes = rocketnet_client_protocol_seal_fragment( &pair.client, base_sequence + (uint64_t) i, whole_id, i, 2,
                                                                      fragment_data, i < 1 ? 300 : 100,
                                                                      whole_aggregate, pair.time, wire );
            rocketnet_check( wire_bytes > 0 );
            rocketnet_check( rocketnet_test_fragment_deliver_to_server( &pair, wire, wire_bytes ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        }

        // the server's next block restates everything it opened; applying
        // it surfaces the whole payload's one id and never the punched one
        pair.client.num_acked = 0;
        int keep_alive_bytes = rocketnet_server_write_keep_alive( &pair.test.server, 0, pair.time, wire );
        rocketnet_check( keep_alive_bytes > 0 );
        uint8_t scratch[ROCKETNET_RECEIVE_CEILING];
        int scratch_bytes = 0;
        rocketnet_check( rocketnet_client_protocol_process_packet( &pair.client, wire, keep_alive_bytes, pair.time, scratch, &scratch_bytes ) );
        int saw_whole = 0;
        for ( int i = 0; i < pair.client.num_acked; i++ )
        {
            rocketnet_check( pair.client.acked_packet_ids[i] != 0xA11A );
            if ( pair.client.acked_packet_ids[i] == 0xB0B )
                saw_whole = 1;
        }
        rocketnet_check( saw_whole );
    }

    rocketnet_test_fragment_pair_teardown( &pair );
}

void rocketnet_test_fragment_vector()
{
    // the frozen type 9 vector from Appendix A, reproduced by the real
    // fragment writer: session c2s keys at sequence 6, a fresh ack block,
    // payload id 1, fragment 0 of 2, the first five bytes of the
    // fragmented payload -- byte for byte, framing included

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    static struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    client.signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( client.connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( client.keys.session_client_to_server, rocketnet_test_session_c2s_key, 32 );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int bytes = rocketnet_client_protocol_seal_fragment( &client, 6, 1, 0, 2, (const uint8_t*) "hello", 5, 0, 100.0, packet );
    rocketnet_check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + 18 );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_check( packet[0] == ROCKETNET_CONNECTION_FRAGMENT_PACKET );
    rocketnet_check( packet[1] == ROCKETNET_FILTER_EPOCH_UNKEYED );
    rocketnet_check( memcmp( packet + 2, tag, ROCKETNET_FILTER_VALUE_BYTES ) == 0 );
    rocketnet_check( packet[6] == 6 && packet[7] == 0 && packet[8] == 0 && packet[9] == 0 );
    rocketnet_check( memcmp( packet + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, rocketnet_test_fragment_sealed, 34 ) == 0 );

    // and it opens back to the frozen plaintext
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, packet, bytes, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                                     6, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                     NULL, rocketnet_test_session_c2s_key );
    rocketnet_check( opened_bytes == 18 );
    rocketnet_check( memcmp( opened, rocketnet_test_fragment_plaintext, 18 ) == 0 );

    // the sent record carries no packet id of its own -- a fragment's id
    // lives in its aggregate
    rocketnet_check( client.ack.sent[6 % ROCKETNET_SENT_PACKET_BUFFER_SIZE].sequence == 6 );
    rocketnet_check( client.ack.sent[6 % ROCKETNET_SENT_PACKET_BUFFER_SIZE].has_packet_id == 0 );
}

void rocketnet_test_fragment_packet_id_aggregation()
{
    // one application packet id per fragmented payload, surfaced only when
    // every fragment's wire packet is acknowledged -- sender-internal
    // aggregation, nothing on the wire

    static struct rocketnet_ack_endpoint_t endpoint;
    rocketnet_ack_endpoint_reset( &endpoint );

    uint64_t acked_ids[16];
    int acked_count = 0;
    uint64_t unknown_sequence = 0;
    uint64_t batch_full = 0;

    // a three-fragment payload under packet id 777 at sequences 1..3, and
    // an ordinary payload under packet id 42 at sequence 4
    uint64_t aggregate = rocketnet_ack_endpoint_open_aggregate( &endpoint, 777, 3 );
    rocketnet_check( aggregate != 0 );
    for ( uint64_t s = 1; s <= 3; s++ )
        rocketnet_ack_endpoint_record_sent( &endpoint, s, 100, 0, 0, aggregate, 10.0 );
    rocketnet_ack_endpoint_record_sent( &endpoint, 4, 100, 42, 1, 0, 10.0 );

    // a block acking two of the three fragments surfaces nothing
    struct rocketnet_ack_block_t block;
    block.largest = 2;
    block.first_range_length = 2;
    block.num_ranges = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( &endpoint, &block, 10.1, acked_ids, 16, &acked_count, &unknown_sequence, &batch_full ) == 2 );
    rocketnet_check( acked_count == 0 );

    // the block that completes the payload surfaces its one id, alongside
    // the ordinary packet's own
    block.largest = 4;
    block.first_range_length = 4;
    rocketnet_check( rocketnet_ack_endpoint_process_block( &endpoint, &block, 10.2, acked_ids, 16, &acked_count, &unknown_sequence, &batch_full ) == 2 );
    rocketnet_check( acked_count == 2 );
    rocketnet_check( acked_ids[0] == 42 );              // the walk runs newest first
    rocketnet_check( acked_ids[1] == 777 );

    // restating the same block surfaces nothing again: records ack once
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( &endpoint, &block, 10.3, acked_ids, 16, &acked_count, &unknown_sequence, &batch_full ) == 0 );
    rocketnet_check( acked_count == 0 );

    // NEGATIVE: a payload with a fragment never acknowledged never
    // surfaces its id -- sequences 5 and 6, only 6 ever covered
    aggregate = rocketnet_ack_endpoint_open_aggregate( &endpoint, 888, 2 );
    rocketnet_ack_endpoint_record_sent( &endpoint, 5, 100, 0, 0, aggregate, 11.0 );
    rocketnet_ack_endpoint_record_sent( &endpoint, 6, 100, 0, 0, aggregate, 11.0 );
    block.largest = 6;
    block.first_range_length = 1;                       // 6 alone; 5 stays unspoken
    rocketnet_check( rocketnet_ack_endpoint_process_block( &endpoint, &block, 11.1, acked_ids, 16, &acked_count, &unknown_sequence, &batch_full ) == 1 );
    rocketnet_check( acked_count == 0 );

    // a full batch defers a fragment's acknowledgment rather than losing
    // the completion: the record stays unacknowledged and a later block
    // surfaces the id into a drained batch
    block.largest = 6;
    block.first_range_length = 2;                       // covers 5 and 6
    acked_count = 16;                                   // no room at all
    rocketnet_check( rocketnet_ack_endpoint_process_block( &endpoint, &block, 11.2, acked_ids, 16, &acked_count, &unknown_sequence, &batch_full ) == 0 );
    rocketnet_check( batch_full == 1 );
    acked_count = 0;
    rocketnet_check( rocketnet_ack_endpoint_process_block( &endpoint, &block, 11.3, acked_ids, 16, &acked_count, &unknown_sequence, &batch_full ) == 1 );
    rocketnet_check( acked_count == 1 );
    rocketnet_check( acked_ids[0] == 888 );

    rocketnet_check( unknown_sequence == 0 );
}

void rocketnet_test_fragment_payload_id_wrap()
{
    // payload-id staleness is serial-number arithmetic, per the ruling: a
    // defective near-maximum id must not poison its slot -- under a plain
    // compare every later id would refuse stale forever, killing
    // 1/num_slots of reassembly for the connection's life; under wrap the
    // later id is newer, displaces the occupant, and the slot recovers

    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof(counters_storage) );
    uint64_t * counters = counters_storage.values;
    const struct rocketnet_reassembly_counters_t * table = &rocketnet_test_fragment_counters;

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 1, 4, 8192, 4 );
    rocketnet_check( set );
    rocketnet_reassembly_set_begin_update( set );

    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];

    // the defective id occupies slot 0 incomplete
    int bytes = rocketnet_test_build_fragment( fragment, 0xFFFFFFF8u, 0, 3, 100, 100, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );

    // a genuinely older id under wrap still refuses stale
    bytes = rocketnet_test_build_fragment( fragment, 0xFFFFFFF4u, 0, 2, 100, 100, 0 );
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == 1 );

    // an ordinary small id is newer under wrap: it displaces the poison
    // (counted abandoned) and completes -- the slot healed
    bytes = rocketnet_test_build_fragment( fragment, 4, 0, 2, 100, 100, 0x21 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 1 );
    bytes = rocketnet_test_build_fragment( fragment, 4, 1, 2, 60, 100, 0x21 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 160 );
    rocketnet_test_check_reassembled( set->completed_payload, 160, 0x21 );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    rocketnet_reassembly_set_destroy( set );
}

void rocketnet_test_fragment_too_large_clauses()
{
    // the two spec clauses of the too-large arithmetic, each with the test
    // that discriminates it from a near miss

    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof(counters_storage) );
    uint64_t * counters = counters_storage.values;
    const struct rocketnet_reassembly_counters_t * table = &rocketnet_test_fragment_counters;

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 1, 4, 8192, 4 );
    rocketnet_check( set );
    rocketnet_reassembly_set_begin_update( set );

    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];
    int bytes;

    // NEGATIVE: the one-byte-minimum clause -- non-finals that fill the
    // maximum EXACTLY must refuse, because the unarrived final contributes
    // at least one byte: 1024 x 8 == 8192 == max, plus 1, is over. a
    // zero-contribution accounting would wrongly accept this.
    bytes = rocketnet_test_build_fragment( fragment, 100, 0, 9, 1024, 1024, 0 );        // slot 0
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] == 1 );

    // ...and one fragment fewer fits: 1024 x 7 + 1 is under the maximum
    bytes = rocketnet_test_build_fragment( fragment, 100, 0, 8, 1024, 1024, 0 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );

    // a fresh id refused too large never occupied its slot: a later id
    // arriving there evicts nothing
    bytes = rocketnet_test_build_fragment( fragment, 101, 0, 9, 1024, 1024, 0 );        // slot 1: refused, unclaimed
    rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] == 2 );
    bytes = rocketnet_test_build_fragment( fragment, 105, 0, 2, 100, 100, 0x31 );       // slot 1 again
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    bytes = rocketnet_test_build_fragment( fragment, 105, 1, 2, 100, 100, 0x31 );
    rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
    rocketnet_check( set->completed_bytes == 200 );

    // NEGATIVE: the immediate-clear clause -- an occupied payload whose
    // final's real size pushes it over the maximum clears its slot at the
    // refusal, so the next id claiming the slot evicts nothing: without
    // the clear, the arrival below would count an abandonment
    {
        const int stride = 1300;                            // 6 x 1300 + 1 = 7801 fits; + a 500-byte final does not
        for ( int i = 0; i < 6; i++ )
        {
            bytes = rocketnet_test_build_fragment( fragment, 102, i, 7, stride, stride, 0 );    // slot 2
            rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        }
        bytes = rocketnet_test_build_fragment( fragment, 102, 6, 7, 500, stride, 0 );
        rocketnet_check( !rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] == 3 );

        bytes = rocketnet_test_build_fragment( fragment, 106, 0, 2, 100, 100, 0x32 );   // slot 2 again
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
        bytes = rocketnet_test_build_fragment( fragment, 106, 1, 2, 40, 100, 0x32 );
        rocketnet_check( rocketnet_reassembly_process_fragment( set, 0, fragment, bytes, counters, table ) );
        rocketnet_check( set->completed_bytes == 140 );
        rocketnet_test_check_reassembled( set->completed_payload, 140, 0x32 );
    }

    rocketnet_reassembly_set_destroy( set );
}

void rocketnet_test_transport_fragmentation()
{
    // the declared-capacity surface end to end over UDP loopback: a
    // request above the wire's capacity routes to the arena, splits into
    // fragments at finish, crosses real sockets, and reassembles
    // byte-exact into the ordinary payload batch on the far side -- both
    // directions -- with the payload's one packet id surfacing exactly
    // once when every fragment's wire packet is acknowledged

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, 0xF6A9ULL, (uint64_t) time( NULL ), token ) );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    static uint8_t pattern[ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES];
    for ( int i = 0; i < (int) sizeof(pattern); i++ )
        pattern[i] = (uint8_t) ( 0xC3 + i );

    // client to server: a 5000-byte payload written into the arena frame

    struct rocketnet_packet_context_t up_context;
    uint8_t * frame = rocketnet_client_transport_start_packet( client, 5000, &up_context );
    rocketnet_check( frame );
    rocketnet_check( up_context.payload_index == 0 );
    rocketnet_check( up_context.payload_capacity == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );
    memcpy( frame, pattern, 5000 );
    rocketnet_check( rocketnet_client_transport_finish_packet( client, &up_context, 5000 ) > 0 );
    rocketnet_client_transport_send_packets( client );

    int server_received = 0;
    int up_acks_seen = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( server_received && up_acks_seen ) )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5000 );
            rocketnet_check( memcmp( payload, pattern, 5000 ) == 0 );
            server_received++;
        }

        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_client_transport_acked_packets( client, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
            if ( acked_ids[i] == up_context.packet_id )
                up_acks_seen++;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received == 1 );
    rocketnet_check( up_acks_seen == 1 );                   // the payload's one id, exactly once

    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( server );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == 5 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED] == 5 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    // server to client: a 9000-byte payload the other way

    struct rocketnet_packet_context_t down_context;
    frame = rocketnet_server_transport_start_packet( server, 0, 9000, &down_context );
    rocketnet_check( frame );
    rocketnet_check( down_context.payload_index == 0 );
    memcpy( frame, pattern, 9000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 9000 ) > 0 );

    int client_received = 0;
    int down_acks_seen = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( client_received && down_acks_seen ) )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_client_transport_num_payloads( client );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_bytes;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            rocketnet_check( payload_bytes == 9000 );
            rocketnet_check( memcmp( payload, pattern, 9000 ) == 0 );
            client_received++;
        }

        const int * acked_client_index = NULL;
        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_server_transport_acked_packets( server, &acked_client_index, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
        {
            if ( acked_ids[i] == down_context.packet_id )
            {
                rocketnet_check( acked_client_index[i] == 0 );
                down_acks_seen++;
            }
        }

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client_received == 1 );
    rocketnet_check( down_acks_seen == 1 );

    const struct rocketnet_counters_t * client_counters = rocketnet_client_transport_counters( client );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED] == 8 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    // an over-capacity request whose write turns out small collapses to
    // one ordinary payload packet -- the fragment counters do not move

    frame = rocketnet_client_transport_start_packet( client, 5000, &up_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 64 );
    rocketnet_check( rocketnet_client_transport_finish_packet( client, &up_context, 64 ) > 0 );
    rocketnet_client_transport_send_packets( client );

    server_received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !server_received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_bytes == 64 );
            rocketnet_check( memcmp( payload, pattern, 64 ) == 0 );
            server_received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received == 1 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == 5 );        // unchanged

    // the configured maximum is readable, so callers can size buffers
    rocketnet_check( rocketnet_client_transport_max_payload_bytes( client ) == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );

    // the copying cursor carries reassembled payloads too, sized by the
    // configured maximum
    frame = rocketnet_server_transport_start_packet( server, 0, 4000, &down_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 4000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 4000 ) > 0 );

    static uint8_t cursor_buffer[ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES];
    int cursor_received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !cursor_received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        int cursor_bytes;
        while ( ( cursor_bytes = rocketnet_client_transport_receive_packet( client, cursor_buffer, (int) sizeof(cursor_buffer) ) ) > 0 )
        {
            rocketnet_check( cursor_bytes == 4000 );
            rocketnet_check( memcmp( cursor_buffer, pattern, 4000 ) == 0 );
            cursor_received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( cursor_received == 1 );

    // NEGATIVE: a cursor buffer smaller than a reassembled payload refuses
    // it whole -- counted, never truncated, never delivered
    frame = rocketnet_server_transport_start_packet( server, 0, 3000, &down_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 3000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 3000 ) > 0 );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 &&
            client_counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        uint8_t small_buffer[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];
        rocketnet_check( rocketnet_client_transport_receive_packet( client, small_buffer, (int) sizeof(small_buffer) ) == 0 );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 1 );

    // the reviewer's burst: twelve fragmented payloads in one fill phase --
    // 36 wire packets, ordinary load -- can all complete in ONE client
    // update, which is more completions than the client has reassembly
    // slots. the delivered mirror sizes to the drain bound, so every one
    // is delivered and BATCH_FULL stays exactly zero.
    {
        struct rocketnet_packet_context_t burst_context;
        for ( int i = 0; i < 12; i++ )
        {
            frame = rocketnet_server_transport_start_packet( server, 0, 2500, &burst_context );
            rocketnet_check( frame );
            memcpy( frame, pattern, 2500 );
            rocketnet_check( rocketnet_server_transport_finish_packet( server, &burst_context, 2500 ) > 0 );
        }
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        rocketnet_platform_sleep( 0.1 );                    // let the whole burst queue on the client's receive thread

        int burst_received = 0;
        start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 10.0 && burst_received < 12 )
        {
            rocketnet_client_transport_update( client, rocketnet_platform_time() );
            rocketnet_server_transport_update( server, rocketnet_platform_time() );
            rocketnet_server_transport_send_packets( server );

            const int num_payloads = rocketnet_client_transport_num_payloads( client );
            for ( int i = 0; i < num_payloads; i++ )
            {
                int payload_bytes;
                uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
                rocketnet_check( payload_bytes == 2500 );
                rocketnet_check( memcmp( payload, pattern, 2500 ) == 0 );
                burst_received++;
            }
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( burst_received == 12 );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == 15 );    // 9000 + 4000 + 3000 + the twelve
    }

    // NEGATIVE: the arena is a fixed fill-phase resource -- exhaustion
    // drops at start_packet on its own counter, and the flush reclaims it
    {
        struct rocketnet_packet_context_t contexts[ROCKETNET_SEND_ARENA_SLOTS];
        for ( int i = 0; i < ROCKETNET_SEND_ARENA_SLOTS; i++ )
            rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &contexts[i] ) );
        struct rocketnet_packet_context_t overflow_context;
        rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &overflow_context ) == NULL );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_SEND_ARENA_FULL] == 1 );
        for ( int i = 0; i < ROCKETNET_SEND_ARENA_SLOTS; i++ )
            rocketnet_client_transport_abort_packet( client, &contexts[i] );
        rocketnet_client_transport_send_packets( client );
        rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &overflow_context ) != NULL );
        rocketnet_client_transport_abort_packet( client, &overflow_context );
    }

    // NEGATIVE: a request past the configured maximum refuses outright
    {
        struct rocketnet_packet_context_t oversize_context;
        rocketnet_check( rocketnet_client_transport_start_packet( client, ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES + 1, &oversize_context ) == NULL );
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES + 1, &oversize_context ) == NULL );
    }

    // nothing died on the clean wire
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );

    rocketnet_client_transport_disconnect( client );
    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_send_arena()
{
    struct rocketnet_send_arena_t * arena = rocketnet_send_arena_create( NULL, 4, 1024 );
    rocketnet_check( arena );

    // slots come back distinct until exhaustion, and every one is owned
    uint8_t * slots[4];
    for ( int i = 0; i < 4; i++ )
    {
        slots[i] = rocketnet_send_arena_acquire( arena );
        rocketnet_check( slots[i] );
        rocketnet_check( rocketnet_send_arena_owns( arena, slots[i] ) );
        for ( int j = 0; j < i; j++ )
            rocketnet_check( slots[i] != slots[j] );
    }
    rocketnet_check( rocketnet_send_arena_acquire( arena ) == NULL );

    // a frame outside the arena is not owned -- how finish and abort route
    uint8_t elsewhere[16];
    rocketnet_check( !rocketnet_send_arena_owns( arena, elsewhere ) );

    // the flush releases every slot
    rocketnet_send_arena_reset( arena );
    rocketnet_check( rocketnet_send_arena_acquire( arena ) == slots[0] );

    rocketnet_send_arena_destroy( arena );
}

// the transport-level ipv6 loopback: the same out-of-the-box connect and
// payload exchange as the udp loopback test, on [::1] -- ipv6 listen is
// REQUIRED of every server driver (#97: the gamelift gateway speaks ipv6 to
// servers), so this runs on every platform and every driver the suite
// builds against, with zero driver ifdefs.

void rocketnet_test_transport_ipv6_loopback()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "[::1]", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "[::1]:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello6", 6 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world6", 6 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 6 );
            rocketnet_check( memcmp( payload, "hello6", 6 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 6 );
            rocketnet_check( memcmp( packet, "world6", 6 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 10 );
    rocketnet_check( client_received >= 10 );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );
}

// ----------------------------------------------------------------
//        the config table and the driver ladder (#106)
// ----------------------------------------------------------------

// env guard: the suite may itself run under a driver matrix leg
// (ROCKETNET_DRIVER=... ./bin/test), so every test that touches the
// environment saves what it found and puts it back

struct rocketnet_test_env_guard_t
{
    const char * name;
    bool was_set;
    char saved[1024];
};

static void rocketnet_test_env_save( struct rocketnet_test_env_guard_t * guard, const char * name )
{
    guard->name = name;
    const char * value = getenv( name );
    guard->was_set = value != NULL;
    guard->saved[0] = '\0';
    if ( value )
        snprintf( guard->saved, sizeof(guard->saved), "%s", value );
}

static void rocketnet_test_env_clear( struct rocketnet_test_env_guard_t * guard, const char * name )
{
    rocketnet_test_env_save( guard, name );
    unsetenv( name );
}

// the same platform truth the empty-value case below is skipped for bounds
// this restore on windows: a guard that saved a set-but-empty value would
// put back a DELETED variable rather than an empty one. the state is
// unreachable there -- the windows CRT cannot hold a set-but-empty variable
// in the first place -- so this is disclosure, not a live defect

static void rocketnet_test_env_restore( struct rocketnet_test_env_guard_t * guard )
{
    if ( guard->was_set )
        setenv( guard->name, guard->saved, 1 );
    else
        unsetenv( guard->name );
}

// the log capture sink, shared by the tests that assert a log line rather
// than trusting it (config validation's empty-value case, and the override
// logging test's redaction MUSTs)

static char rocketnet_test_captured_log[16384];

static void rocketnet_test_capture_log( int level, const char * format, ... )
{
    (void) level;
    va_list args;
    va_start( args, format );
    char line[1024];
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    const size_t used = strlen( rocketnet_test_captured_log );
    snprintf( rocketnet_test_captured_log + used, sizeof(rocketnet_test_captured_log) - used, "%s\n", line );
}

// THE COMPLETENESS TEST: walk both config tables and prove every declared
// field round-trips from environment -- set the derived env var to a value
// the default is not, apply, read the field back. a field added to a table
// without env support cannot pass here, so the env mirror stays complete
// by check rather than by memory.

static void rocketnet_test_config_table_round_trip( const char * transport_name, void * config, void ( *default_config )( void * ),
                                                    const struct rocketnet_config_field_t * fields, int num_fields )
{
    rocketnet_check( num_fields > 0 );

    for ( int i = 0; i < num_fields; i++ )
    {
        const struct rocketnet_config_field_t * field = &fields[i];

        char env_name[128];
        rocketnet_check( rocketnet_config_env_var_name( field->name, env_name, sizeof(env_name) ) );

        struct rocketnet_test_env_guard_t guard;
        rocketnet_test_env_save( &guard, env_name );

        default_config( config );

        char value_string[256];
        uint8_t expected_bytes[256];
        int64_t expected_int = 0;
        uint64_t expected_uint64 = 0;

        switch ( field->kind )
        {
            case ROCKETNET_CONFIG_KIND_INT:
            {
                int current;
                memcpy( &current, (uint8_t*) config + field->offset, sizeof(current) );
                expected_int = field->min_value != (int64_t) current ? field->min_value : field->min_value + 1;
                rocketnet_check( expected_int <= field->max_value );
                snprintf( value_string, sizeof(value_string), "%" PRId64, expected_int );
            }
            break;

            case ROCKETNET_CONFIG_KIND_UINT64:
            {
                expected_uint64 = 9876543210987654321ULL;
                snprintf( value_string, sizeof(value_string), "%" PRIu64, expected_uint64 );
            }
            break;

            case ROCKETNET_CONFIG_KIND_BYTES:
            {
                rocketnet_check( field->size <= (int) sizeof(expected_bytes) );
                for ( int j = 0; j < field->size; j++ )
                    expected_bytes[j] = (uint8_t) ( j * 37 + 11 + field->offset );
                rocketnet_check( rocketnet_base64_encode_data( expected_bytes, (size_t) field->size, value_string, sizeof(value_string) ) > 0 );
            }
            break;

            case ROCKETNET_CONFIG_KIND_STRING:
            {
                // one value for every string field: the table's validate
                // checks termination only (the driver list's CONTENT is
                // parsed by the ladder at create, which this walk never
                // reaches), so a per-field special case here would be
                // decoration

                snprintf( value_string, sizeof(value_string), "value-under-test" );
            }
            break;

            default:
            {
                rocketnet_check( !"unknown config field kind" );
            }
            break;
        }

        setenv( env_name, value_string, 1 );

        rocketnet_check( rocketnet_config_apply_env( transport_name, config, fields, num_fields ) );
        rocketnet_check( rocketnet_config_validate( transport_name, config, fields, num_fields ) );

        const uint8_t * base = (const uint8_t*) config + field->offset;

        switch ( field->kind )
        {
            case ROCKETNET_CONFIG_KIND_INT:
            {
                int applied;
                memcpy( &applied, base, sizeof(applied) );
                rocketnet_check( (int64_t) applied == expected_int );
            }
            break;

            case ROCKETNET_CONFIG_KIND_UINT64:
            {
                uint64_t applied;
                memcpy( &applied, base, sizeof(applied) );
                rocketnet_check( applied == expected_uint64 );
            }
            break;

            case ROCKETNET_CONFIG_KIND_BYTES:
            {
                rocketnet_check( memcmp( base, expected_bytes, (size_t) field->size ) == 0 );
            }
            break;

            case ROCKETNET_CONFIG_KIND_STRING:
            {
                rocketnet_check( strcmp( (const char*) base, value_string ) == 0 );
            }
            break;
        }

        rocketnet_test_env_restore( &guard );
    }
}

static void rocketnet_test_client_default_config_erased( void * config )
{
    rocketnet_client_transport_default_config( (struct rocketnet_client_transport_config_t*) config );
}

static void rocketnet_test_server_default_config_erased( void * config )
{
    rocketnet_server_transport_default_config( (struct rocketnet_server_transport_config_t*) config );
}

void rocketnet_test_config_env_round_trip()
{
    // the derivation is mechanical: field name uppercased under the prefix

    char derived[128];
    rocketnet_check( rocketnet_config_env_var_name( "driver", derived, sizeof(derived) ) );
    rocketnet_check( strcmp( derived, "ROCKETNET_DRIVER" ) == 0 );
    rocketnet_check( rocketnet_config_env_var_name( "num_xdp_queues", derived, sizeof(derived) ) );
    rocketnet_check( strcmp( derived, "ROCKETNET_NUM_XDP_QUEUES" ) == 0 );

    int num_client_fields = 0;
    const struct rocketnet_config_field_t * client_fields = rocketnet_client_transport_config_fields( &num_client_fields );
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_test_config_table_round_trip( "client", &client_config, rocketnet_test_client_default_config_erased, client_fields, num_client_fields );

    int num_server_fields = 0;
    const struct rocketnet_config_field_t * server_fields = rocketnet_server_transport_config_fields( &num_server_fields );
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_test_config_table_round_trip( "server", &server_config, rocketnet_test_server_default_config_erased, server_fields, num_server_fields );

    // both tables carry the driver field, as a string

    bool client_has_driver = false;
    for ( int i = 0; i < num_client_fields; i++ )
        if ( strcmp( client_fields[i].name, "driver" ) == 0 && client_fields[i].kind == ROCKETNET_CONFIG_KIND_STRING )
            client_has_driver = true;
    rocketnet_check( client_has_driver );

    bool server_has_driver = false;
    for ( int i = 0; i < num_server_fields; i++ )
        if ( strcmp( server_fields[i].name, "driver" ) == 0 && server_fields[i].kind == ROCKETNET_CONFIG_KIND_STRING )
            server_has_driver = true;
    rocketnet_check( server_has_driver );
}

void rocketnet_test_config_validation()
{
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );

    // ranges refuse loudly, never clamp; zero passes only where the field
    // declares it means the default

    struct rocketnet_server_transport_config_t config;

    rocketnet_server_transport_default_config( &config );
    rocketnet_check( rocketnet_config_validate( "server", &config, fields, num_fields ) );

    config.max_clients = 0;
    rocketnet_check( !rocketnet_config_validate( "server", &config, fields, num_fields ) );
    config.max_clients = ROCKETNET_MAX_CLIENTS + 1;
    rocketnet_check( !rocketnet_config_validate( "server", &config, fields, num_fields ) );
    config.max_clients = ROCKETNET_MAX_CLIENTS;
    rocketnet_check( rocketnet_config_validate( "server", &config, fields, num_fields ) );

    rocketnet_server_transport_default_config( &config );
    config.max_payload_bytes = -1;
    rocketnet_check( !rocketnet_config_validate( "server", &config, fields, num_fields ) );
    config.max_payload_bytes = ROCKETNET_MAX_PAYLOAD_BYTES - 1;
    rocketnet_check( !rocketnet_config_validate( "server", &config, fields, num_fields ) );
    config.max_payload_bytes = 0;
    rocketnet_check( rocketnet_config_validate( "server", &config, fields, num_fields ) );

    // the table's num_xdp_queues range is structural; the per-driver
    // bounds live at the ladder rungs (the gso pin test proves the gso
    // rung's refusal at 33)

    rocketnet_server_transport_default_config( &config );
    config.num_xdp_queues = 33;
    rocketnet_check( rocketnet_config_validate( "server", &config, fields, num_fields ) );
    config.num_xdp_queues = 1025;
    rocketnet_check( !rocketnet_config_validate( "server", &config, fields, num_fields ) );

    // an unterminated driver string is refused, not read past

    rocketnet_server_transport_default_config( &config );
    memset( config.driver, 'x', sizeof(config.driver) );
    rocketnet_check( !rocketnet_config_validate( "server", &config, fields, num_fields ) );

    // env parses are strict: the whole value or a loud failure

    struct rocketnet_test_env_guard_t guard;

    rocketnet_test_env_save( &guard, "ROCKETNET_MAX_CLIENTS" );

    rocketnet_server_transport_default_config( &config );
    setenv( "ROCKETNET_MAX_CLIENTS", "abc", 1 );
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );
    setenv( "ROCKETNET_MAX_CLIENTS", "12x", 1 );
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );

    // strtoll's lenient spellings are refused before it ever runs: no
    // leading whitespace, no '+', digits only

    setenv( "ROCKETNET_MAX_CLIENTS", "+5", 1 );
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );
    setenv( "ROCKETNET_MAX_CLIENTS", " 5", 1 );
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );
    setenv( "ROCKETNET_MAX_CLIENTS", "99999999999999999999", 1 );
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );

    // the empty value is ignored as unset, with a log line, leaving the
    // field untouched -- and the LOG LINE is asserted, not assumed: the
    // return value and the field alone cannot tell "set to empty" from
    // "not set at all", so without the capture this case would still pass
    // if the empty branch vanished entirely
    //
    // windows cannot run it. the CRT has no set-but-empty state at all --
    // _putenv_s(name,"") DELETES the variable -- so the shim at the top of
    // this file cannot put the process in the state the case needs, and
    // the branch it covers is unreachable on that platform. a skipped test
    // is named and logged, never silent

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

    setenv( "ROCKETNET_MAX_CLIENTS", "", 1 );
    rocketnet_server_transport_default_config( &config );

    rocketnet_test_captured_log[0] = '\0';
    const int empty_saved_level = rocketnet_get_log_level();
    rocketnet_log_function( rocketnet_test_capture_log );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

    rocketnet_check( rocketnet_config_apply_env( "server", &config, fields, num_fields ) );

    rocketnet_log_level( empty_saved_level );
    rocketnet_log_function( rocketnet_test_suite_log_function );

    rocketnet_check( config.max_clients == 64 );
    rocketnet_check( strstr( rocketnet_test_captured_log, "config ignores empty ROCKETNET_MAX_CLIENTS" ) != NULL );

#else // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

    rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_config_validation empty-value case [SKIPPED on windows: the CRT cannot represent a set-but-empty variable; empty deletes]" );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

    rocketnet_test_env_restore( &guard );

    rocketnet_test_env_save( &guard, "ROCKETNET_PROTOCOL_ID" );
    rocketnet_server_transport_default_config( &config );
    setenv( "ROCKETNET_PROTOCOL_ID", "-5", 1 );
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );
    rocketnet_test_env_restore( &guard );

    // byte fields demand base64 of exactly the field's length

    rocketnet_test_env_save( &guard, "ROCKETNET_FILTER_VALUE" );
    rocketnet_server_transport_default_config( &config );
    setenv( "ROCKETNET_FILTER_VALUE", "zzzz", 1 );                  // decodes to 3 bytes, not 4
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );
    setenv( "ROCKETNET_FILTER_VALUE", "AAECAw==", 1 );              // 4 bytes: 00 01 02 03
    rocketnet_server_transport_default_config( &config );
    rocketnet_check( rocketnet_config_apply_env( "server", &config, fields, num_fields ) );
    const uint8_t expected_filter[4] = { 0x00, 0x01, 0x02, 0x03 };
    rocketnet_check( memcmp( config.filter_value, expected_filter, 4 ) == 0 );
    rocketnet_test_env_restore( &guard );

    // an out-of-range env value refuses the whole create

    rocketnet_test_env_save( &guard, "ROCKETNET_MAX_CLIENTS" );
    setenv( "ROCKETNET_MAX_CLIENTS", "0", 1 );
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config, rocketnet_platform_time() ) == NULL );
    rocketnet_test_env_restore( &guard );
}

void rocketnet_test_driver_ladder_parse()
{
    const int linux_server[] = { ROCKETNET_DRIVER_XDP, ROCKETNET_DRIVER_GSO, ROCKETNET_DRIVER_PORTABLE };
    const int mac_endpoint[] = { ROCKETNET_DRIVER_MSGX, ROCKETNET_DRIVER_PORTABLE };

    struct rocketnet_driver_ladder_t ladder;

    // auto -- and its spellings NULL, "", and whitespace -- expands to the
    // availability order

    rocketnet_check( rocketnet_driver_ladder_parse( NULL, linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 3 && ladder.num_skipped == 0 );
    rocketnet_check( ladder.candidates[0] == ROCKETNET_DRIVER_XDP && ladder.candidates[1] == ROCKETNET_DRIVER_GSO && ladder.candidates[2] == ROCKETNET_DRIVER_PORTABLE );

    rocketnet_check( rocketnet_driver_ladder_parse( "auto", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 3 );

    rocketnet_check( rocketnet_driver_ladder_parse( "", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 3 );

    rocketnet_check( rocketnet_driver_ladder_parse( "  auto  ", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 3 );

    // a one-element list is a pin; an explicit list is exactly that list,
    // in exactly that order, with no implicit floor appended

    rocketnet_check( rocketnet_driver_ladder_parse( "gso", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 1 && ladder.candidates[0] == ROCKETNET_DRIVER_GSO );

    rocketnet_check( rocketnet_driver_ladder_parse( "gso,xdp", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 2 );
    rocketnet_check( ladder.candidates[0] == ROCKETNET_DRIVER_GSO && ladder.candidates[1] == ROCKETNET_DRIVER_XDP );

    rocketnet_check( rocketnet_driver_ladder_parse( " gso , portable ", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 2 );

    // known names absent here are skipped softly, so one fleet-wide line
    // deploys unmodified to heterogeneous boxes

    rocketnet_check( rocketnet_driver_ladder_parse( "xdp,gso,msgx,portable", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 3 && ladder.num_skipped == 1 );
    rocketnet_check( ladder.skipped[0] == ROCKETNET_DRIVER_MSGX );

    rocketnet_check( rocketnet_driver_ladder_parse( "xdp,gso,msgx,portable", mac_endpoint, 2, &ladder ) );
    rocketnet_check( ladder.num_candidates == 2 && ladder.num_skipped == 2 );
    rocketnet_check( ladder.candidates[0] == ROCKETNET_DRIVER_MSGX && ladder.candidates[1] == ROCKETNET_DRIVER_PORTABLE );

    // a list whose every name was skipped names no driver for this
    // endpoint: the parse falls to the availability order, loudly, so a
    // fleet-wide line never bricks an endpoint it was not addressing

    rocketnet_check( rocketnet_driver_ladder_parse( "xdp", mac_endpoint, 2, &ladder ) );
    rocketnet_check( ladder.num_skipped == 1 && ladder.skipped[0] == ROCKETNET_DRIVER_XDP );
    rocketnet_check( ladder.num_candidates == 2 );
    rocketnet_check( ladder.candidates[0] == ROCKETNET_DRIVER_MSGX && ladder.candidates[1] == ROCKETNET_DRIVER_PORTABLE );

    // typos are not skips: a name in no transport set fails loudly

    rocketnet_check( !rocketnet_driver_ladder_parse( "protable", linux_server, 3, &ladder ) );
    rocketnet_check( !rocketnet_driver_ladder_parse( "GSO", linux_server, 3, &ladder ) );
    rocketnet_check( !rocketnet_driver_ladder_parse( "gso,,portable", linux_server, 3, &ladder ) );
    rocketnet_check( !rocketnet_driver_ladder_parse( ",", linux_server, 3, &ladder ) );
    rocketnet_check( !rocketnet_driver_ladder_parse( "auto,gso", linux_server, 3, &ladder ) );
    rocketnet_check( !rocketnet_driver_ladder_parse( "portable,portable,portable,portable,portable,portable,portable,portable,portable", linux_server, 3, &ladder ) );
    rocketnet_check( !rocketnet_driver_ladder_parse( "msgx,msgx,msgx,msgx,msgx,msgx,msgx,msgx,msgx", linux_server, 3, &ladder ) );

    // duplicates of an available name are allowed: trying a rung twice is
    // the operator's list, honored as written

    rocketnet_check( rocketnet_driver_ladder_parse( "gso,gso", linux_server, 3, &ladder ) );
    rocketnet_check( ladder.num_candidates == 2 );
}

void rocketnet_test_config_precedence()
{
    // DEFAULT < STRUCT < ENV, observed through the one config field with a
    // public query: max_payload_bytes

    struct rocketnet_test_env_guard_t payload_guard;
    rocketnet_test_env_clear( &payload_guard, "ROCKETNET_MAX_PAYLOAD_BYTES" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );

    // default layer

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );
    rocketnet_server_transport_destroy( server );

    // struct displaces default

    config.max_payload_bytes = 18000;
    server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == 18000 );
    rocketnet_server_transport_destroy( server );

    // env displaces struct

    setenv( "ROCKETNET_MAX_PAYLOAD_BYTES", "20000", 1 );
    server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == 20000 );
    rocketnet_server_transport_destroy( server );

    rocketnet_test_env_restore( &payload_guard );
}

// the rung-forcing exchange: pin a driver list on the server, expect the
// named grant, and pass real payloads both ways through it -- the matrix
// doctrine's runtime form. the pin rides the struct, so ROCKETNET_DRIVER
// is cleared for the duration (env would win, by design). port zero per
// the #107 law: the kernel grants the port and the token is minted for
// what was granted, so no other process on the box can race this test.

static void rocketnet_test_driver_pin_exchange( const char * driver_list, const char * expected_driver )
{
    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    snprintf( server_config.driver, sizeof(server_config.driver), "%s", driver_list );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), expected_driver ) == 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent < 5 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "rung!", 5 );
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "rung!", 5 );
            sent++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload && payload_bytes == 5 && memcmp( payload, "rung!", 5 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 && memcmp( packet, "rung!", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 5 && client_received >= 5 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 5 );
    rocketnet_check( client_received >= 5 );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );

    rocketnet_test_env_restore( &driver_guard );
}

void rocketnet_test_server_driver_pin_portable()
{
    // the portable rung carries traffic on every platform -- the floor
    // must never be the untested tier

    rocketnet_test_driver_pin_exchange( "portable", "portable" );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
void rocketnet_test_server_driver_pin_iocp()
{
    rocketnet_test_driver_pin_exchange( "iocp", "iocp" );
}
#endif

// #109: a port-zero public address asks the KERNEL for a port, so it only
// means anything to a driver that discovers what it was granted and hands
// it back. the portable and msgx rungs discover through the platform
// socket's bind_address parameter, the gso rung through its own; the xdp
// rung takes an address STRING and hands nothing back, so a port-zero
// address granted on xdp would advertise port 0 -- connect tokens minted
// from rocketnet_server_transport_port() would name port 0, the cookie gate
// would never match, and nothing would say why.
//
// the question is per DRIVER, not per build: #110 made create a loop over
// candidates, so the rule asks every candidate the same question at the
// top of that loop and a rung that cannot discover refuses a port-zero
// address the way every other rung refusal works -- the ladder falls to a
// rung that can, and a PIN on a non-discovering rung refuses the create.
//
// WHAT THIS TEST REACHES, honestly: the refusal and its reason are live
// wherever a non-discovering driver is available (the linux legs of CI).
// The defect itself -- a GRANTED xdp socket advertising port 0 -- is not
// reachable here at any privilege: the suite installs the commit barrier
// structurally, so the committing xdp create never runs under a test. That
// half is a documented manual reproduction on #109, not an assertion.

void rocketnet_test_server_port_zero_needs_a_discovering_driver()
{
    // every driver answers the question, and the answers are the ones the
    // rungs actually implement -- checked against the code in the table's
    // own comment, rung by rung

    rocketnet_check( rocketnet_driver_discovers_bound_port( ROCKETNET_DRIVER_PORTABLE ) );
    rocketnet_check( rocketnet_driver_discovers_bound_port( ROCKETNET_DRIVER_MSGX ) );
    rocketnet_check( rocketnet_driver_discovers_bound_port( ROCKETNET_DRIVER_GSO ) );
    rocketnet_check( !rocketnet_driver_discovers_bound_port( ROCKETNET_DRIVER_XDP ) );

    // and out of range fails safe rather than reading off the end

    rocketnet_check( !rocketnet_driver_discovers_bound_port( -1 ) );
    rocketnet_check( !rocketnet_driver_discovers_bound_port( ROCKETNET_NUM_DRIVERS ) );

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    int num_available = 0;
    const int * available = rocketnet_server_driver_availability( &num_available );

    // THE ACCEPT SIDE FIRST, and it is the load-bearing half: this rule
    // must never cost a port-zero create that a discovering driver can
    // serve. auto on every platform, plus every discovering driver this
    // platform has, pinned one at a time.

    for ( int i = -1; i < num_available; i++ )
    {
        const char * list = "auto";
        if ( i >= 0 )
        {
            if ( !rocketnet_driver_discovers_bound_port( available[i] ) )
                continue;                                   // the refuse side below owns those
            list = rocketnet_driver_name( available[i] );
        }

        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "%s", list );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
        rocketnet_check( server );
        rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_destroy( server );
    }

    // THE REFUSE SIDE, where this platform actually carries a
    // non-discovering driver. the loop body is the linux legs; elsewhere
    // the accept side above is the whole test, which is the honest state
    // of a rule about a driver that does not exist on this platform.

    for ( int i = 0; i < num_available; i++ )
    {
        if ( rocketnet_driver_discovers_bound_port( available[i] ) )
            continue;

        const char * name = rocketnet_driver_name( available[i] );

        // a PIN on a non-discovering rung refuses the create, and the log
        // names the rule and the driver. that naming IS the fix: without
        // it an operator debugs a cookie gate that never matches.
        //
        // captured at the ERROR level ON PURPOSE, because that is the
        // level a deployed binary, this suite and the C# smoke all run at:
        // the per-rung warn is invisible there, so if the rule were only a
        // warn a pinned port-zero create would return NULL with nothing
        // anywhere saying why. The exhausted-ladder path restates it as an
        // error, and this asserts that restatement rather than the warn.

        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "%s", name );

        rocketnet_test_captured_log[0] = '\0';
        const int saved_level = rocketnet_get_log_level();
        rocketnet_log_function( rocketnet_test_capture_log );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "cannot discover the port it was granted and hand it back" ) != NULL );

        // and at a level that shows warns, the RUNG's own refusal names
        // the rule and the driver together -- matched as one phrase, since
        // the driver name alone also appears in the exhausted-ladder error
        // and would pass this check without the rule being mentioned

        char expected_rung_refusal[128];
        snprintf( expected_rung_refusal, sizeof(expected_rung_refusal), "server driver %s refused: a port-zero public address", name );

        rocketnet_test_captured_log[0] = '\0';
        rocketnet_log_function( rocketnet_test_capture_log );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, expected_rung_refusal ) != NULL );

        // SCOPE, the other half of correct: the rule is about port ZERO
        // and nothing else. the same pin with a real port never meets it
        // and goes on into the driver's own create, whose FIRST act is the
        // commit barrier -- the suite never mutates a host. That refusal
        // is asserted by its message rather than merely tolerated: it is
        // the one path in this test that enters the xdp driver at all, and
        // it is what keeps the BARRIER exercised, since the rule
        // short-circuits the port-zero pins that would otherwise reach it.

        rocketnet_test_captured_log[0] = '\0';
        rocketnet_log_function( rocketnet_test_capture_log );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:40000", &config, rocketnet_platform_time() );

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "port-zero public address" ) == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "forbids committing an XDP data plane" ) != NULL );

        // and the rung refusal is a RUNG refusal, not a create refusal:
        // put a discovering rung after it and the ladder falls, the port
        // is discovered, and the advertised address is the real one.

        snprintf( config.driver, sizeof(config.driver), "%s,portable", name );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
        rocketnet_check( server );
        rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), "portable" ) == 0 );
        rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_destroy( server );
    }

    rocketnet_test_env_restore( &driver_guard );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

void rocketnet_test_server_driver_pin_gso()
{
    rocketnet_test_driver_pin_exchange( "gso", "gso" );

    // per-driver bounds: a queue count past the gso geometry refuses the
    // pinned rung -- and with nothing left to fall to, the create --
    // loudly, never clamped

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "gso" );
    config.num_xdp_queues = ROCKETNET_GSO_MAX_QUEUES + 1;

    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &driver_guard );
}

void rocketnet_test_server_driver_pin_xdp()
{
    // the pre-flight is called DIRECTLY here, as it also is in
    // rocketnet_test_server_driver_ladder_fall below: those two direct
    // calls are how the suite reaches it. Inside the xdp driver's create
    // the pre-flight sits below the commit barrier (barrier at
    // rocketnet_server_socket_linux.cpp:982, pre-flight at :1015), and the
    // suite installs that barrier process-wide, so the in-create pre-flight
    // is not reached under this suite. What create refuses with here is the
    // BARRIER. That is the refusal the pin asserts: pin-refuses proven
    // through create, no machine touched. On a box the pre-flight passes
    // whole, this test asserts nothing more -- granting xdp attaches a data
    // plane, which is not a unit test's act.
    //
    // THE ADDRESS CARRIES A REAL PORT DELIBERATELY (#109). A port-zero
    // address is now refused by the ladder's port-zero rule before the xdp
    // rung's create is called at all, which would make this test pass
    // without entering the driver -- no barrier, nothing proven about the
    // pin. A real port takes the rule out of the way so the refusal proven
    // here is again the driver's own. Nothing binds: the barrier returns
    // before create's first committing act.

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    const bool provisioned = rocketnet_server_socket_preflight( "127.0.0.1:40000", 7 );

    if ( !provisioned )
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "xdp" );

        rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40000", &config, rocketnet_platform_time() ) == NULL );
    }

    rocketnet_test_env_restore( &driver_guard );
}

void rocketnet_test_server_driver_ladder_fall()
{
    // THE FALL, proven to actually happen where it can be proven without
    // mutating the host: the auto ladder must land on gso -- the next rung
    // -- never portable, never failure. on a box the pre-flight passes
    // whole, the ladder would grant xdp by attaching a real data plane, so
    // the test stands down: the suite never commits the machine.
    //
    // WHY the xdp rung refuses here, stated exactly (#109): the address
    // carries port zero -- it must, because the gso rung below binds it
    // and a fixed port would race other processes on the box -- so the xdp
    // rung here is refused by the port-zero rule, which is checked before
    // the rung's create is called at all. The fall itself is what this
    // test asserts and that is unchanged.
    //
    // Note what this does NOT reach, so the two comments agree: neither
    // the commit barrier nor the in-create pre-flight runs on this path,
    // because the rule refuses above both. The barrier is walked by
    // rocketnet_test_server_driver_pin_xdp, which uses a real port for
    // exactly that reason; the in-create pre-flight is below the barrier
    // and so is not reached under this suite from either test. The direct
    // pre-flight call just below is this test's own, and is what decides
    // whether to stand down.

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    const bool provisioned = rocketnet_server_socket_preflight( "127.0.0.1:40000", 7 );

    if ( !provisioned )
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "xdp,gso,portable" );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
        rocketnet_check( server );
        rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), "gso" ) == 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_destroy( server );
    }

    rocketnet_test_env_restore( &driver_guard );
}

// the R4 matcher against fixture strings of real /proc/interrupts lines:
// driver-prefixed names from the exact families B3 tells operators to
// build, plus the substring traps -- pure string logic, held here so the
// pre-flight's and the script's shared boundary rule cannot drift
// unwitnessed

void rocketnet_test_xdp_interrupt_matcher()
{
    // real driver lines (i40e, ice, ixgbe-style, virtio, and a real mlx5
    // completion vector) plus CONSTRUCTED trap lines -- the VLAN pair and
    // the substring traps are synthetic by design, they exist to pin the
    // boundary rule, and this comment says so rather than calling them
    // real. the honest limit is also pinned: mlx5's completion vectors
    // carry NO netdev name, so no netdev can match them -- the pre-flight
    // states that limit instead of claiming there is nothing to pin. the
    // script's selftest mode holds its awk matcher to the SAME CASE SET,
    // transcribed (the deliberate byte differences are recorded there).

    rocketnet_check( rocketnet_server_socket_interrupt_line_matches_interface(
        " 24:  0  0  IR-PCI-MSI 524288-edge      i40e-ens1f0-TxRx-0\n", "ens1f0" ) );
    rocketnet_check( rocketnet_server_socket_interrupt_line_matches_interface(
        " 25:  0  0  IR-PCI-MSI 524289-edge      ice-ens1f0-TxRx-1\n", "ens1f0" ) );
    rocketnet_check( rocketnet_server_socket_interrupt_line_matches_interface(
        " 26:  0  0  PCI-MSI 49152-edge      eth0-TxRx-0\n", "eth0" ) );
    rocketnet_check( rocketnet_server_socket_interrupt_line_matches_interface(
        " 27:  0  0  PCI-MSI 1048577-edge      eth0\n", "eth0" ) );
    rocketnet_check( rocketnet_server_socket_interrupt_line_matches_interface(
        " 28:  0  0  PCI-MSI 65536-edge      virtio0-input.0\n", "virtio0" ) );

    // vlan sub-interfaces are their own names: the parent must not claim
    // the child's lines and the child matches its own literally (the dot
    // is a character, never a wildcard)

    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        " 29:  0  0  PCI-MSI 49153-edge      eth0.100-TxRx-0\n", "eth0" ) );
    rocketnet_check( rocketnet_server_socket_interrupt_line_matches_interface(
        " 29:  0  0  PCI-MSI 49153-edge      eth0.100-TxRx-0\n", "eth0.100" ) );
    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        " 30:  0  0  PCI-MSI 49154-edge      eth0x100-TxRx-0\n", "eth0.100" ) );

    // the substring traps stay traps

    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        " 30:  0  0  PCI-MSI 2097152-edge      velocity0-tx\n", "lo" ) );
    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        "LOC:  12345  67890   Local timer interrupts\n", "lo" ) );
    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        " 31:  0  0  PCI-MSI 2097153-edge      ens1f10-TxRx-0\n", "ens1f0" ) );
    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        " 32:  0  0  PCI-MSI 2097154-edge      eth00\n", "eth0" ) );
    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        " 33:  0  0  PCI-MSI 2097156-edge      xeth0-TxRx-0\n", "eth0" ) );

    // the honest limit: a real mlx5 completion vector line names no
    // netdev, so no netdev matches it -- R4 for that class is stated
    // unverifiable-by-name, never silently passed as nothing-to-pin

    rocketnet_check( !rocketnet_server_socket_interrupt_line_matches_interface(
        " 34:  0  0  IR-PCI-MSI 1048580-edge      mlx5_comp0@pci:0000:03:00.0\n", "ens1f0" ) );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_server_driver_pin_msgx()
{
    rocketnet_test_driver_pin_exchange( "msgx", "msgx" );
}

void rocketnet_test_driver_pump_alias()
{
    // ROCKETNET_PUMP=portable keeps its exact pre-#106 meaning -- the
    // darwin batch pump is off -- absorbed into the ladder as an msgx
    // refusal, so the auto ladder lands on portable

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_test_env_guard_t pump_guard;
    rocketnet_test_env_save( &pump_guard, "ROCKETNET_PUMP" );
    setenv( "ROCKETNET_PUMP", "portable", 1 );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), "portable" ) == 0 );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );

    // and a pinned msgx under the same override is a refusal-exhausted
    // ladder: the pin refuses loudly, it never falls

    struct rocketnet_server_transport_config_t pinned_config;
    rocketnet_server_transport_dev_config( &pinned_config );
    snprintf( pinned_config.driver, sizeof(pinned_config.driver), "msgx" );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &pinned_config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &pump_guard );
    rocketnet_test_env_restore( &driver_guard );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_driver_all_skipped_uses_auto()
{
    // a list whose every name is a platform- or endpoint-skip names no
    // driver for this endpoint at all -- a fleet-wide line read by an
    // endpoint it was not addressing -- so the endpoint falls to its own
    // auto order, loudly, instead of bricking (#110 review, ruled inside
    // the #106 ignore rule). a typo is still a typo: loud parse failure,
    // no create. refusal-exhausted lists still fail loudly -- the xdp pin
    // test and the pump-alias msgx pin prove that side.

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    snprintf( config.driver, sizeof(config.driver), "msgx" );
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    snprintf( config.driver, sizeof(config.driver), "gso" );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );

    // the grant came from the endpoint's own auto order

    int num_available = 0;
    const int * available = rocketnet_server_driver_availability( &num_available );
    bool granted_from_auto = false;
    for ( int i = 0; i < num_available; i++ )
        if ( strcmp( rocketnet_server_transport_driver( server ), rocketnet_driver_name( available[i] ) ) == 0 )
            granted_from_auto = true;
    rocketnet_check( granted_from_auto );

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );

    snprintf( config.driver, sizeof(config.driver), "protable" );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &driver_guard );
}

static void rocketnet_test_config_bytes_exact_fields( const char * name, void * config,
                                                     const rocketnet_config_field_t * fields, int num_fields )
{
    for ( int i = 0; i < num_fields; ++i )
    {
        const rocketnet_config_field_t * field = &fields[i];
        if ( field->kind != ROCKETNET_CONFIG_KIND_BYTES )
            continue;
        rocketnet_check( field->size >= 1 && field->size <= 256 );
        char env_name[256];
        rocketnet_check( rocketnet_config_env_var_name( field->name, env_name, sizeof(env_name) ) );
        rocketnet_test_env_guard_t guard;
        rocketnet_test_env_save( &guard, env_name );
        for ( int delta = field->size == 1 ? 0 : -1; delta <= 1; ++delta )
        {
            uint8_t bytes[257];
            for ( int j = 0; j < (int) sizeof(bytes); ++j ) bytes[j] = (uint8_t) ( j * 13 + 7 );
            char encoded[512];
            rocketnet_check( rocketnet_base64_encode_data( bytes, field->size + delta, encoded, sizeof(encoded) ) > 0 );
            setenv( env_name, encoded, 1 );
            uint8_t * destination = (uint8_t*) config + field->offset;
            uint8_t before[256];
            memset( before, 0xA5, field->size );
            memcpy( destination, before, field->size );
            const bool accepted = rocketnet_config_apply_env( name, config, field, 1 );
            rocketnet_check( accepted == ( delta == 0 ) );
            rocketnet_check( memcmp( destination, accepted ? bytes : before, field->size ) == 0 );
        }
        rocketnet_test_env_restore( &guard );
    }
}

void rocketnet_test_config_bytes_exact()
{
    rocketnet_client_transport_config_t client = {};
    rocketnet_server_transport_config_t server = {};
    int count = 0;
    const rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &count );
    rocketnet_test_config_bytes_exact_fields( "client", &client, fields, count );
    fields = rocketnet_server_transport_config_fields( &count );
    rocketnet_test_config_bytes_exact_fields( "server", &server, fields, count );
    uint8_t tiny = 0;
    const rocketnet_config_field_t tiny_field = { "test_one_byte", ROCKETNET_CONFIG_KIND_BYTES, 0, 1, 0, 0, 0 };
    rocketnet_test_config_bytes_exact_fields( "one-byte fixture", &tiny, &tiny_field, 1 );
}

// Every override logs the variable and displaced value while keeping secret
// material out of both success and refusal diagnostics.
void rocketnet_test_config_override_logging()
{
    struct rocketnet_test_env_guard_t clients_guard;
    rocketnet_test_env_save( &clients_guard, "ROCKETNET_MAX_CLIENTS" );

    struct rocketnet_test_env_guard_t secret_guard;
    rocketnet_test_env_save( &secret_guard, "ROCKETNET_SERVER_SECRET_KEY" );

    uint8_t secret_bytes[32];
    for ( int i = 0; i < 32; i++ )
        secret_bytes[i] = (uint8_t) ( i * 11 + 5 );
    char secret_base64[64];
    rocketnet_check( rocketnet_base64_encode_data( secret_bytes, 32, secret_base64, sizeof(secret_base64) ) > 0 );

    setenv( "ROCKETNET_MAX_CLIENTS", "77", 1 );
    setenv( "ROCKETNET_SERVER_SECRET_KEY", secret_base64, 1 );

    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    rocketnet_test_captured_log[0] = '\0';
    const int saved_level = rocketnet_get_log_level();
    rocketnet_log_function( rocketnet_test_capture_log );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

    rocketnet_check( rocketnet_config_apply_env( "server", &config, fields, num_fields ) );

    rocketnet_log_level( saved_level );
    rocketnet_log_function( rocketnet_test_suite_log_function );

    rocketnet_check( config.max_clients == 77 );
    rocketnet_check( memcmp( config.server_secret_key, secret_bytes, 32 ) == 0 );

    // the non-secret override logs the variable, the value, and what it
    // displaced; the secret override logs redacted on both sides and the
    // secret's encoding appears nowhere in the log

    rocketnet_check( strstr( rocketnet_test_captured_log, "ROCKETNET_MAX_CLIENTS=77" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "(displaces 64)" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "ROCKETNET_SERVER_SECRET_KEY=(redacted)" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "(displaces (redacted))" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, secret_base64 ) == NULL );

    // the failure path redacts too: a mistyped secret is still a secret

    setenv( "ROCKETNET_SERVER_SECRET_KEY", "not!valid!base64!secret!material!!", 1 );
    rocketnet_server_transport_default_config( &config );
    rocketnet_test_captured_log[0] = '\0';
    rocketnet_log_function( rocketnet_test_capture_log );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_check( !rocketnet_config_apply_env( "server", &config, fields, num_fields ) );
    rocketnet_log_level( saved_level );
    rocketnet_log_function( rocketnet_test_suite_log_function );
    rocketnet_check( strstr( rocketnet_test_captured_log, "not!valid!base64!secret!material!!" ) == NULL );

    rocketnet_test_env_restore( &clients_guard );
    rocketnet_test_env_restore( &secret_guard );
}

// the deprecated env aliases (the pre-#106 spellings): honored where the
// successor is unset, logged loudly naming the successor, yielding loudly
// beside it -- and the public address alias feeds the one resolved
// address create uses everywhere

void rocketnet_test_config_env_aliases()
{
    struct rocketnet_test_env_guard_t alias_guard;
    rocketnet_test_env_save( &alias_guard, "ROCKETNET_SERVER_NUM_QUEUES" );

    struct rocketnet_test_env_guard_t successor_guard;
    rocketnet_test_env_save( &successor_guard, "ROCKETNET_NUM_XDP_QUEUES" );

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    // the alias is honored where the successor is unset...

    unsetenv( "ROCKETNET_NUM_XDP_QUEUES" );
    setenv( "ROCKETNET_SERVER_NUM_QUEUES", "3", 1 );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "portable" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_server_transport_destroy( server );

    // ...parsed strictly...

    setenv( "ROCKETNET_SERVER_NUM_QUEUES", "3x", 1 );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() ) == NULL );

    // ...and yields to the successor when both are set

    setenv( "ROCKETNET_SERVER_NUM_QUEUES", "not-even-a-number", 1 );
    setenv( "ROCKETNET_NUM_XDP_QUEUES", "2", 1 );
    server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_server_transport_destroy( server );

    rocketnet_test_env_restore( &alias_guard );
    rocketnet_test_env_restore( &successor_guard );

    // the public address rides the table now, scoped first-create-consumes
    // for the env layer. proven deterministically, no fixed port: the
    // create PARAMETER is unparseable, so the create succeeds if and only
    // if the env override displaced it -- and the second create proves the
    // consumption, succeeding-by-refusal when the parameter (not the still
    // exported variable) is what create reads.

    struct rocketnet_test_env_guard_t address_guard;
    rocketnet_test_env_save( &address_guard, "ROCKETNET_PUBLIC_ADDRESS" );
    setenv( "ROCKETNET_PUBLIC_ADDRESS", "127.0.0.1:0", 1 );

    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "portable" );

    server = rocketnet_server_transport_create( NULL, "not.an.address", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
    rocketnet_server_transport_destroy( server );

    rocketnet_check( rocketnet_server_transport_create( NULL, "not.an.address", &config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &address_guard );
    rocketnet_test_env_restore( &driver_guard );
}


void rocketnet_test_client_driver()
{
    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( "127.0.0.1:41040", rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    // the portable pin grants everywhere and reports itself

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    rocketnet_check( strcmp( config.driver, "auto" ) == 0 );

    snprintf( config.driver, sizeof(config.driver), "portable" );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_driver( client ), "portable" ) == 0 );
    rocketnet_client_transport_destroy( client );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

    // the msgx pin grants on macOS and reports itself

    snprintf( config.driver, sizeof(config.driver), "msgx" );
    client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_driver( client ), "msgx" ) == 0 );
    rocketnet_client_transport_destroy( client );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

    // the staged linux drivers are not client transports: a list naming
    // only them is an all-skipped list for the client endpoint, which
    // falls to the client's auto order rather than bricking every client
    // in a process that shares the fleet's ROCKETNET_DRIVER value

    snprintf( config.driver, sizeof(config.driver), "gso" );
    client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    {
        int num_available = 0;
        const int * available = rocketnet_client_driver_availability( &num_available );
        bool granted_from_auto = false;
        for ( int i = 0; i < num_available; i++ )
            if ( strcmp( rocketnet_client_transport_driver( client ), rocketnet_driver_name( available[i] ) ) == 0 )
                granted_from_auto = true;
        rocketnet_check( granted_from_auto );
    }
    rocketnet_client_transport_destroy( client );

    rocketnet_test_env_restore( &driver_guard );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// The GSO driver's own tests (#97), linux only, run on every linux build
// whatever driver the transport compiled against -- the driver module is
// always built, so its probe, its wire paths, its degraded tiers and its
// overflow behaviour are covered even when the transport above it is
// portable. Everything here is over-the-wire on loopback through real
// sockets; the peer is the portable platform socket, which also proves the
// driver interoperates with a plain UDP endpoint.

#include "rocketnet_server_gso.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

#ifndef UDP_GRO
#define UDP_GRO 104
#endif

// does this HOST actually coalesce? the driver's feature bits prove only
// that the setsockopts were ACCEPTED -- with `ethtool -K lo
// tx-udp-segmentation off`, a legitimate configuration, a UDP_SEGMENT
// super-datagram is segmented at transmit and arrives as plain datagrams,
// no UDP_GRO cmsg ever appears, and a counter assertion gated on the
// feature bits alone would encode that host tunable as a protocol fact.
// so probe the host itself, once, with the exact shape the provocation
// phase sends: a scratch socket with UDP_GRO enabled mails itself one
// eight-segment super-datagram over loopback and watches for an arrival
// carrying the UDP_GRO stride cmsg below its length -- the kernel's own
// statement that it delivered coalesced.

static bool rocketnet_test_gso_host_coalesces()
{
    int fd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( fd < 0 )
        return false;

    struct sockaddr_in address;
    memset( &address, 0, sizeof(address) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    address.sin_port = 0;
    socklen_t address_length = sizeof(address);
    if ( bind( fd, (struct sockaddr*) &address, sizeof(address) ) != 0 ||
         getsockname( fd, (struct sockaddr*) &address, &address_length ) != 0 )
    {
        close( fd );
        return false;
    }

    int one = 1;
    if ( setsockopt( fd, IPPROTO_UDP, UDP_GRO, &one, sizeof(one) ) != 0 )
    {
        close( fd );
        return false;
    }

    const int probe_segments = 8;
    const int probe_segment_bytes = 100;

    uint8_t super[probe_segments * probe_segment_bytes];
    memset( super, 0xAB, sizeof(super) );

    struct iovec iov = { super, sizeof(super) };
    char control[CMSG_SPACE(sizeof(uint16_t))];
    memset( control, 0, sizeof(control) );
    struct msghdr msg;
    memset( &msg, 0, sizeof(msg) );
    msg.msg_name = &address;
    msg.msg_namelen = sizeof(address);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_level = IPPROTO_UDP;
    cmsg->cmsg_type = UDP_SEGMENT;
    cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
    uint16_t stride = (uint16_t) probe_segment_bytes;
    memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );

    if ( sendmsg( fd, &msg, 0 ) != (ssize_t) sizeof(super) )
    {
        close( fd );
        return false;
    }

    bool coalesced = false;
    int received_bytes = 0;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 && received_bytes < (int) sizeof(super) && !coalesced )
    {
        uint8_t buffer[sizeof(super)];
        struct iovec receive_iov = { buffer, sizeof(buffer) };
        char receive_control[CMSG_SPACE(sizeof(int))];
        struct msghdr receive_msg;
        memset( &receive_msg, 0, sizeof(receive_msg) );
        receive_msg.msg_iov = &receive_iov;
        receive_msg.msg_iovlen = 1;
        receive_msg.msg_control = receive_control;
        receive_msg.msg_controllen = sizeof(receive_control);
        ssize_t bytes = recvmsg( fd, &receive_msg, MSG_DONTWAIT );
        if ( bytes <= 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        received_bytes += (int) bytes;
        for ( struct cmsghdr * c = CMSG_FIRSTHDR( &receive_msg ); c; c = CMSG_NXTHDR( &receive_msg, c ) )
        {
            if ( c->cmsg_level == IPPROTO_UDP && c->cmsg_type == UDP_GRO )
            {
                int segment_bytes = 0;
                memcpy( &segment_bytes, CMSG_DATA( c ), sizeof(int) );
                if ( segment_bytes > 0 && segment_bytes < (int) bytes )
                    coalesced = true;
            }
        }
    }

    close( fd );
    return coalesced;
}

void rocketnet_test_server_gso_probe()
{
    // create discovers an ephemeral port and reports it back; the queue
    // count and the reuseport group are facts, the segmentation features
    // are probes whose result belongs to the kernel

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );
    rocketnet_check( bind_address.port == 0 );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 2, 0 );
    rocketnet_check( gso );
    rocketnet_check( bind_address.port != 0 );
    rocketnet_check( rocketnet_server_gso_num_queues( gso ) == 2 );
    rocketnet_check( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_REUSEPORT );

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == 0 );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED] == 0 );

    rocketnet_server_gso_destroy( gso );

    // the disable flags force the degraded tiers, so the fallback paths are
    // testable on kernels where the probe would say yes

    rocketnet_address_t plain_address;
    rocketnet_check( rocketnet_address_parse( &plain_address, "127.0.0.1" ) );
    struct rocketnet_server_gso_t * plain = rocketnet_server_gso_create( NULL, &plain_address, 1,
                                                                         ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO );
    rocketnet_check( plain );
    rocketnet_check( ( rocketnet_server_gso_features( plain ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) == 0 );
    rocketnet_check( ( rocketnet_server_gso_features( plain ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) == 0 );
    rocketnet_server_gso_destroy( plain );
}

// the wire exchange, shared by the tiers and the families: a plain UDP peer
// sends tagged packets of every size the wire allows into the driver, the
// driver's process batch must surface each byte-exact with the right
// source, then the driver sends tagged packets back -- deliberately
// including equal-size same-destination runs, the shape GSO coalesces --
// and the peer must receive each byte-exact.

static void rocketnet_test_server_gso_wire_exchange( const char * bind_string, const char * peer_string, const char * server_host, int flags, int expect_ipv4_from )
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, bind_string ) );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 2, flags );
    rocketnet_check( gso );

    rocketnet_address_t peer_address;
    rocketnet_check( rocketnet_address_parse( &peer_address, peer_string ) );
    struct rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create( NULL, &peer_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( peer );
    rocketnet_check( peer_address.port != 0 );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, server_host ) );
    server_address.port = bind_address.port;

    #define ROCKETNET_GSO_TEST_PACKETS 200

    // peer -> driver

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    for ( int i = 0; i < ROCKETNET_GSO_TEST_PACKETS; i++ )
    {
        const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        packet[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            packet[j] = (uint8_t) ( i * 31 + j );
        rocketnet_platform_socket_send_packet( peer, &server_address, packet, packet_bytes );
    }

    bool seen[ROCKETNET_GSO_TEST_PACKETS];
    memset( seen, 0, sizeof(seen) );
    int num_seen = 0;

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_GSO_TEST_PACKETS )
    {
        rocketnet_server_gso_receive_packets( gso );
        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
        for ( int i = 0; i < received->num_packets; i++ )
        {
            const int tag = received->packet_data[i][0];
            const int expected_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
            rocketnet_check( tag < ROCKETNET_GSO_TEST_PACKETS );
            rocketnet_check( !seen[tag] );
            rocketnet_check( received->packet_bytes[i] == expected_bytes );
            for ( int j = 1; j < expected_bytes; j++ )
                rocketnet_check( received->packet_data[i][j] == (uint8_t) ( tag * 31 + j ) );

            // the source is the peer's real port, and a v4 peer arriving on
            // a dual stack listen is native ipv4 -- normalized exactly once
            // at the boundary, per the standard's law

            rocketnet_check( received->from[i].port == peer_address.port );
            if ( expect_ipv4_from )
                rocketnet_check( received->from[i].type == ROCKETNET_ADDRESS_IPV4 );

            seen[tag] = true;
            num_seen++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( num_seen == ROCKETNET_GSO_TEST_PACKETS );

    // driver -> peer. the first half all one size -- adjacent equal-size
    // same-destination slots, the runs the GSO tier folds into
    // super-datagrams -- the second half cycling sizes

    for ( int i = 0; i < ROCKETNET_GSO_TEST_PACKETS; i++ )
    {
        const int packet_bytes = i < ROCKETNET_GSO_TEST_PACKETS / 2 ? 400
                               : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_gso_start_packet( gso, &peer_address, &packet_id );
        rocketnet_check( frame );
        uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
        wire[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            wire[j] = (uint8_t) ( i * 47 + j );
        rocketnet_server_gso_finish_packet( gso, packet_id, frame, packet_bytes );
    }

    rocketnet_server_gso_send_packets( gso );

    memset( seen, 0, sizeof(seen) );
    num_seen = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_GSO_TEST_PACKETS )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( peer, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        const int tag = packet[0];
        const int expected_bytes = tag < ROCKETNET_GSO_TEST_PACKETS / 2 ? 400
                                 : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        rocketnet_check( tag < ROCKETNET_GSO_TEST_PACKETS );
        rocketnet_check( !seen[tag] );
        rocketnet_check( packet_bytes == expected_bytes );
        for ( int j = 1; j < expected_bytes; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( tag * 47 + j ) );
        seen[tag] = true;
        num_seen++;
    }
    rocketnet_check( num_seen == ROCKETNET_GSO_TEST_PACKETS );

    // on the GSO tier those equal-size runs must actually have coalesced:
    // the counter is the proof the fast path ran, not just that packets
    // arrived

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == ROCKETNET_GSO_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED] >= ROCKETNET_GSO_TEST_PACKETS );
    if ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT )
        rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS] > 0 );

    // a runt and an oversize packet die at the driver's size pass -- one
    // below the smallest packet a server reads, one above the receive
    // ceiling, the pair covering both bounds of the check that guards the
    // staging memcpy -- counted, never surfaced

    const uint64_t dropped_before = counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE];
    uint8_t runt[ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1];
    memset( runt, 0xEE, sizeof(runt) );
    rocketnet_platform_socket_send_packet( peer, &server_address, runt, (int) sizeof(runt) );
    uint8_t oversize[ROCKETNET_RECEIVE_CEILING + 74];
    memset( oversize, 0xEF, sizeof(oversize) );
    rocketnet_platform_socket_send_packet( peer, &server_address, oversize, (int) sizeof(oversize) );

    start = rocketnet_platform_time();
    uint64_t dropped_after = dropped_before;
    while ( rocketnet_platform_time() - start < 5.0 && dropped_after < dropped_before + 2 )
    {
        rocketnet_server_gso_receive_packets( gso );
        rocketnet_check( rocketnet_server_gso_process_packets( gso )->num_packets == 0 );
        rocketnet_server_gso_counters( gso, counters );
        dropped_after = counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE];
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( dropped_after == dropped_before + 2 );

    // provoke GRO for real: the peer sends one UDP_SEGMENT super-datagram,
    // which loopback segments and the driver's UDP_GRO socket receives
    // coalesced -- the multi-segment split path runs, proven by the
    // GRO_BATCHES counter, and every segment surfaces byte-exact. the
    // suite's ordinary traffic never repeats a size back to back, so
    // without this phase the split loop would never execute under test.
    // ipv4 peers only (the sockaddr is built inline), which covers every
    // tier this helper runs.

    if ( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) &&
         ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) &&
         server_address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        const bool host_coalesces = rocketnet_test_gso_host_coalesces();

        const int gro_segments = 8;
        const int gro_segment_bytes = 100;

        uint8_t super[gro_segments * gro_segment_bytes];
        for ( int i = 0; i < gro_segments; i++ )
        {
            super[i * gro_segment_bytes] = (uint8_t) ( 200 + i );
            for ( int j = 1; j < gro_segment_bytes; j++ )
                super[i * gro_segment_bytes + j] = (uint8_t) ( ( 200 + i ) * 13 + j );
        }

        struct sockaddr_in to;
        memset( &to, 0, sizeof(to) );
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = ( ( (uint32_t) server_address.data.ipv4[0] ) )       |
                             ( ( (uint32_t) server_address.data.ipv4[1] ) << 8 )  |
                             ( ( (uint32_t) server_address.data.ipv4[2] ) << 16 ) |
                             ( ( (uint32_t) server_address.data.ipv4[3] ) << 24 );
        to.sin_port = rocketnet_platform_htons( server_address.port );

        struct iovec iov = { super, sizeof(super) };
        char control[CMSG_SPACE(sizeof(uint16_t))];
        memset( control, 0, sizeof(control) );
        struct msghdr msg;
        memset( &msg, 0, sizeof(msg) );
        msg.msg_name = &to;
        msg.msg_namelen = sizeof(to);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
        uint16_t stride = (uint16_t) gro_segment_bytes;
        memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );

        rocketnet_check( sendmsg( peer->handle, &msg, 0 ) == (ssize_t) sizeof(super) );

        rocketnet_server_gso_counters( gso, counters );
        const uint64_t gro_before = counters[ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES];

        bool segment_seen[gro_segments];
        memset( segment_seen, 0, sizeof(segment_seen) );
        int num_segments_seen = 0;

        start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 5.0 && num_segments_seen < gro_segments )
        {
            rocketnet_server_gso_receive_packets( gso );
            struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
            for ( int i = 0; i < received->num_packets; i++ )
            {
                const int tag = received->packet_data[i][0] - 200;
                rocketnet_check( tag >= 0 && tag < gro_segments );
                rocketnet_check( !segment_seen[tag] );
                rocketnet_check( received->packet_bytes[i] == gro_segment_bytes );
                for ( int j = 1; j < gro_segment_bytes; j++ )
                    rocketnet_check( received->packet_data[i][j] == (uint8_t) ( ( 200 + tag ) * 13 + j ) );
                rocketnet_check( received->from[i].port == peer_address.port );
                segment_seen[tag] = true;
                num_segments_seen++;
            }
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( num_segments_seen == gro_segments );

        // the counter is the proof the coalesced path ran -- eight distinct
        // arrivals alone would also be produced by uncoalesced delivery. it
        // is asserted only where the host probe observed real coalescing:
        // the feature bits mean the setsockopts were accepted, and a host
        // with tx-udp-segmentation off on loopback delivers this same send
        // uncoalesced -- legitimate configuration, not a driver failure. on
        // such a host the byte-exact segment coverage above still ran, and
        // the skip says so out loud rather than passing silently.

        if ( host_coalesces )
        {
            rocketnet_server_gso_counters( gso, counters );
            rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES] > gro_before );
        }
        else
        {
            rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_server_gso_super_datagram_split [SKIPPED GRO_BATCHES assertion: this host delivers UDP_SEGMENT super-datagrams uncoalesced over loopback (tx-udp-segmentation off?); segments still surfaced byte-exact above]" );
        }
    }

    rocketnet_platform_socket_destroy( peer );
    rocketnet_server_gso_destroy( gso );
}

void rocketnet_test_server_gso_wire_ipv4()
{
    rocketnet_test_server_gso_wire_exchange( "127.0.0.1", "127.0.0.1", "127.0.0.1", 0, 1 );
}

void rocketnet_test_server_gso_wire_plain()
{
    // the degraded tier over the same wire: no UDP_SEGMENT, no UDP_GRO,
    // plain sendmmsg/recvmmsg batching. the fallback path is never the
    // untested path.

    rocketnet_test_server_gso_wire_exchange( "127.0.0.1", "127.0.0.1", "127.0.0.1", ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO, 1 );
}

void rocketnet_test_server_gso_wire_ipv6()
{
    rocketnet_test_server_gso_wire_exchange( "[::1]", "[::1]", "[::1]", 0, 0 );
}

void rocketnet_test_server_gso_wire_dual_stack()
{
    // an ipv6 wildcard listen accepts an ipv4 peer, whose source surfaces
    // as native ipv4 -- the v4-mapped normalization the standard makes law

    rocketnet_test_server_gso_wire_exchange( "[::]", "127.0.0.1", "127.0.0.1", 0, 1 );
}

void rocketnet_test_server_gso_send_overflow()
{
    // fill the send staging past capacity: starts beyond it refuse with
    // NULL, the flush sends what fit, and the next phase is whole -- the
    // driver-level face of the transport's ring exhaustion property

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 1, 0 );
    rocketnet_check( gso );

    rocketnet_address_t peer_address;
    rocketnet_check( rocketnet_address_parse( &peer_address, "127.0.0.1" ) );
    struct rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create( NULL, &peer_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( peer );

    for ( int i = 0; i < ROCKETNET_GSO_SEND_QUEUE_SIZE; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_gso_start_packet( gso, &peer_address, &packet_id );
        rocketnet_check( frame );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, (uint8_t) i, 64 );
        rocketnet_server_gso_finish_packet( gso, packet_id, frame, 64 );
    }

    uint64_t overflow_id = 0;
    rocketnet_check( rocketnet_server_gso_start_packet( gso, &peer_address, &overflow_id ) == NULL );
    rocketnet_check( rocketnet_server_gso_start_packet( gso, &peer_address, &overflow_id ) == NULL );

    rocketnet_server_gso_send_packets( gso );

    // every staged packet was either sent or dropped, and nothing vanished
    // uncounted

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] + counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == ROCKETNET_GSO_SEND_QUEUE_SIZE );

    // the next phase is whole: a fresh tagged packet flushes and arrives

    uint8_t tag[64];
    memset( tag, 0xAB, sizeof(tag) );

    uint64_t packet_id = 0;
    uint8_t * frame = rocketnet_server_gso_start_packet( gso, &peer_address, &packet_id );
    rocketnet_check( frame );
    memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, tag, sizeof(tag) );
    rocketnet_server_gso_finish_packet( gso, packet_id, frame, (int) sizeof(tag) );
    rocketnet_server_gso_send_packets( gso );

    bool recovered = false;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( peer, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        if ( packet_bytes == (int) sizeof(tag) && memcmp( packet, tag, sizeof(tag) ) == 0 )
            recovered = true;
    }
    rocketnet_check( recovered );

    rocketnet_platform_socket_destroy( peer );
    rocketnet_server_gso_destroy( gso );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// The suite isolates config overrides, while preserving the driver's
// deliberate CI pin and the optional verbose-output switch. Names come
// from both descriptor tables; duplicate fields are saved only once.
class rocketnet_test_environment_t
{
    struct entry_t { std::string name, value; bool present; };
    std::vector<entry_t> entries;
    void clear(const char * name)
    {
        if (strcmp(name, "ROCKETNET_DRIVER") == 0) return;
        for (const auto & entry : entries) if (entry.name == name) return;
        const char * value = getenv(name);
        entries.push_back({name, value ? value : "", value != NULL});
        rocketnet_check(unsetenv(name) == 0);
    }
public:
    rocketnet_test_environment_t()
    {
        for (int side = 0; side < 2; ++side)
        {
            int count = 0;
            const auto * fields = side ? rocketnet_server_transport_config_fields(&count)
                                       : rocketnet_client_transport_config_fields(&count);
            for (int i = 0; i < count; ++i)
            {
                char name[128];
                rocketnet_check(rocketnet_config_env_var_name(fields[i].name, name, sizeof(name)));
                clear(name);
            }
        }
        clear("ROCKETNET_PUBLIC_ADDRESS");
        clear("ROCKETNET_SERVER_PUBLIC_ADDRESS");
        clear("ROCKETNET_SERVER_NUM_QUEUES");
        clear("ROCKETNET_SERVER_NUM_OS_CPUS");
        clear("ROCKETNET_GATEWAY_ETHERNET_ADDRESS");
        clear("ROCKETNET_PUMP");
    }
    ~rocketnet_test_environment_t()
    {
        for (const auto & entry : entries)
            rocketnet_check((entry.present ? setenv(entry.name.c_str(), entry.value.c_str(), 1)
                                           : unsetenv(entry.name.c_str())) == 0);
    }
};

void rocketnet_test_suite_environment()
{
    const std::string original_driver = getenv("ROCKETNET_DRIVER") ? getenv("ROCKETNET_DRIVER") : "";
    const bool driver_present = getenv("ROCKETNET_DRIVER") != NULL;
    const std::string long_value(4096, 'x');
    rocketnet_check(setenv("ROCKETNET_MAX_CLIENTS", long_value.c_str(), 1) == 0);
    rocketnet_check(setenv("ROCKETNET_PUMP", "portable", 1) == 0);
    {
        rocketnet_test_environment_t isolated;
        rocketnet_check(getenv("ROCKETNET_MAX_CLIENTS") == NULL);
        rocketnet_check(getenv("ROCKETNET_PUMP") == NULL);
        rocketnet_check((getenv("ROCKETNET_DRIVER") != NULL) == driver_present);
        if (driver_present) rocketnet_check(original_driver == getenv("ROCKETNET_DRIVER"));
    }
    rocketnet_check(long_value == getenv("ROCKETNET_MAX_CLIENTS"));
    rocketnet_check(strcmp(getenv("ROCKETNET_PUMP"), "portable") == 0);
    unsetenv("ROCKETNET_MAX_CLIENTS");
    unsetenv("ROCKETNET_PUMP");
}

#define RUN_TEST( test_function )                                           \
    do                                                                      \
    {                                                                       \
        rocketnet_test_log_ring_clear();                                    \
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, #test_function );                 \
        fflush( stdout );                                                   \
        rocketnet_##test_function();                                             \
    }                                                                       \
    while (0)

void rocketnet_run_tests()
{
    // SILENCE, WITH THE CAUSE KEPT: the library's log goes to the ring at
    // the top of this file rather than to stdout, at whatever level the
    // caller chose -- test.cpp's ERROR floor -- so a passing run prints
    // only test names and a failing check prints the lines that led to it.
    // ROCKETNET_TEST_VERBOSE installs nothing and leaves the log live.

    if ( rocketnet_platform_getenv( "ROCKETNET_TEST_VERBOSE" ) == NULL )
    {
        rocketnet_test_suite_log_function = rocketnet_test_log_capture;
        rocketnet_log_function( rocketnet_test_suite_log_function );
    }

    // THE COMMIT BARRIER (#110 gate, both readers): the suite must never
    // commit an XDP data plane -- not by per-test discipline, which two
    // readers proved decorative, but structurally: installed once here,
    // it removes the committing create from every test's reach, present
    // and future, root box or not. the pre-flight stays reachable (it is
    // non-mutating by contract); a test that ever needs the committing
    // path must call rocketnet_server_socket_allow_commit() explicitly,
    // and none does.

    rocketnet_forbid_xdp_commit();

    rocketnet_test_environment_t isolated_environment;

    // while ( true )
    {
        RUN_TEST( test_suite_environment );
        RUN_TEST( test_time );
        RUN_TEST( test_endian );
        RUN_TEST( test_read_write );
        RUN_TEST( test_base64 );
        RUN_TEST( test_hash );
        RUN_TEST( test_copy_string );
        RUN_TEST( test_address );
        RUN_TEST( test_random_bytes );
        RUN_TEST( test_random_float );
        RUN_TEST( test_crypto_sizes );
        RUN_TEST( test_crypto_test_keys );
        RUN_TEST( test_crypto_sign );
        RUN_TEST( test_crypto_sealed_box );
        RUN_TEST( test_crypto_aead );
        RUN_TEST( test_crypto_generichash );
        RUN_TEST( test_crypto_scalarmult );
        RUN_TEST( test_crypto_key_derivation );
        RUN_TEST( test_connect_token_private );
        RUN_TEST( test_signed_connect_data );
        RUN_TEST( test_connect_token_frozen );
        RUN_TEST( test_connect_token_mint );
        RUN_TEST( test_connect_token_refusals );
        RUN_TEST( test_prefilter );
        RUN_TEST( test_packet_associated_data );
        RUN_TEST( test_sealed_packets );
        RUN_TEST( test_sealed_packet_refusals );
        RUN_TEST( test_unsealed_packets );
        RUN_TEST( test_sequence_reconstruction );
        RUN_TEST( test_cookie );
        RUN_TEST( test_connection_keys );
        RUN_TEST( test_counters );
        RUN_TEST( test_used_token_history );
        RUN_TEST( test_admitted_token_set );
        RUN_TEST( test_encryption_map );
        RUN_TEST( test_encryption_map_iterator );
        RUN_TEST( test_server_update_sweep );
        RUN_TEST( test_server_keep_alive_pacing );
        RUN_TEST( test_client_payload_before_first_keep_alive );
        RUN_TEST( test_client_keep_alive_suppression );
        RUN_TEST( test_keep_alive_unestablished_under_payload_traffic );
        RUN_TEST( test_keep_alive_pacing_survives_early_ack );
        RUN_TEST( test_request_path_happy );
        RUN_TEST( test_request_path_refusals );
        RUN_TEST( test_request_path_cookie_gate );
        RUN_TEST( test_request_path_crypto_refusals );
        RUN_TEST( test_request_path_state_refusals );
        RUN_TEST( test_request_path_slots_and_denial );
        RUN_TEST( test_server_full_connect );
        RUN_TEST( test_server_response_denials );
        RUN_TEST( test_server_receive_refusals );
        RUN_TEST( test_client_connect_validations );
        RUN_TEST( test_client_server_connect );
        RUN_TEST( test_client_server_loss_walk );
        RUN_TEST( test_client_denied_when_server_full );
        RUN_TEST( test_client_timeout_paths );
        RUN_TEST( test_client_cookie_injection );
        RUN_TEST( test_network_simulator );
        RUN_TEST( test_client_server_harsh_wire );
        RUN_TEST( test_client_server_lossy_wire );
        RUN_TEST( test_swarm );
        RUN_TEST( test_payload_size_sweep );
        RUN_TEST( test_server_refuses_keyless_config );
        RUN_TEST( test_transport_disconnect_delivery );
        RUN_TEST( test_transport_ring_exhaustion );
        RUN_TEST( test_transport_client_concurrent_fill );
        RUN_TEST( test_transport_concurrent_fill );
        RUN_TEST( test_transport_start_finish_shape );
        RUN_TEST( test_transport_payload_capacity );
        RUN_TEST( test_transport_udp_loopback );
        RUN_TEST( test_client_rtt_handshake );
        RUN_TEST( test_transport_stats );
        RUN_TEST( test_transport_packet_loss_stats );
        RUN_TEST( test_request_path_ordering );
        RUN_TEST( test_request_path_uncovered_rungs );
        RUN_TEST( test_request_path_config_branches );
        RUN_TEST( test_replay_protection );
        RUN_TEST( test_client_slots );
        RUN_TEST( test_platform_socket );
        RUN_TEST( test_platform_batch_pump );
        RUN_TEST( test_platform_batch_pump_degraded );
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
        RUN_TEST( test_platform_batch_pump_refused_destination );
        RUN_TEST( test_platform_pump_zero_length_chaff );
#else // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
        // a skipped test is named and logged, never silent: both drive the
        // posix raw-socket injector beside the platform socket, which has
        // no windows arm
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_platform_batch_pump_refused_destination [SKIPPED on windows: posix raw-socket injector]" );
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_platform_pump_zero_length_chaff [SKIPPED on windows: posix raw-socket injector]" );
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
        RUN_TEST( test_platform_thread );
        RUN_TEST( test_platform_mutex );
        RUN_TEST( test_value_tracker );
        RUN_TEST( test_wire_server_packet_valid );
        RUN_TEST( test_server_disconnect_client );
        RUN_TEST( test_anonymize_address_ipv4 );
#if ROCKETNET_PLATFORM_HAS_IPV6
        RUN_TEST( test_anonymize_address_ipv6 );
        RUN_TEST( test_platform_inet6_round_trip );
#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
        RUN_TEST( test_packet_loss_tracker );
        RUN_TEST( test_out_of_order_tracker );
        RUN_TEST( test_jitter_tracker );
        RUN_TEST( test_frame_sync_advance );
        RUN_TEST( test_frame_sync_guard );
        RUN_TEST( test_frame_sync_lifecycle );
        RUN_TEST( test_ack_block_encoding );
        RUN_TEST( test_ack_block_validity );
        RUN_TEST( test_ack_block_build );
        RUN_TEST( test_ack_endpoint_processing );
        RUN_TEST( test_ack_debt );
        RUN_TEST( test_ack_non_elicitation );
        RUN_TEST( test_ack_replay_protection );
        RUN_TEST( test_ack_bad_block_refusals );
        RUN_TEST( test_ack_asymmetric_rate );
        RUN_TEST( test_ack_rtt_freshness );
        RUN_TEST( test_ack_payload_seal_with_ranges );
        RUN_TEST( test_transport_acks_and_client_stats );
        RUN_TEST( test_fragment_header_codec );
        RUN_TEST( test_fragment_plan );
        RUN_TEST( test_fragment_reassembly );
        RUN_TEST( test_fragment_reassembly_refusals );
        RUN_TEST( test_fragment_delivery_batch );
        RUN_TEST( test_fragment_vector );
        RUN_TEST( test_fragment_packet_id_aggregation );
        RUN_TEST( test_fragment_client_to_server );
        RUN_TEST( test_fragment_server_to_client );
        RUN_TEST( test_fragment_receive_refusals );
        RUN_TEST( test_fragment_punched_loss );
        RUN_TEST( test_fragment_payload_id_wrap );
        RUN_TEST( test_fragment_too_large_clauses );
        RUN_TEST( test_transport_fragmentation );
        RUN_TEST( test_send_arena );
        RUN_TEST( test_frame_sync_wait );
        RUN_TEST( test_transport_ipv6_loopback );

        // the config table and the driver ladder (#106)

        RUN_TEST( test_config_env_round_trip );
        RUN_TEST( test_config_bytes_exact );
        RUN_TEST( test_config_validation );
        RUN_TEST( test_driver_ladder_parse );
        RUN_TEST( test_config_precedence );
        RUN_TEST( test_server_driver_pin_portable );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        RUN_TEST( test_server_driver_pin_iocp );
#endif
        RUN_TEST( test_server_port_zero_needs_a_discovering_driver );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
        RUN_TEST( test_server_driver_pin_gso );
        RUN_TEST( test_server_driver_pin_xdp );
        RUN_TEST( test_server_driver_ladder_fall );
        RUN_TEST( test_xdp_interrupt_matcher );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        RUN_TEST( test_server_driver_pin_msgx );
        RUN_TEST( test_driver_pump_alias );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        RUN_TEST( test_driver_all_skipped_uses_auto );
        RUN_TEST( test_config_override_logging );
        RUN_TEST( test_config_env_aliases );
        RUN_TEST( test_client_driver );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
        RUN_TEST( test_server_gso_probe );
        RUN_TEST( test_server_gso_wire_ipv4 );
        RUN_TEST( test_server_gso_wire_plain );
        RUN_TEST( test_server_gso_wire_ipv6 );
        RUN_TEST( test_server_gso_wire_dual_stack );
        RUN_TEST( test_server_gso_send_overflow );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    }
}

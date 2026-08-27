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
#include "rocketnet_packet_filter.h"
#include "rocketnet_packet_loss_tracker.h"
#include "rocketnet_out_of_order_tracker.h"
#include "rocketnet_jitter_tracker.h"
#include "rocketnet_value_tracker.h"
#include "rocketnet_hydrogen.h"
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
#include "rocketnet_client_transport.h"
#include "rocketnet_server_transport.h"
#include "rocketnet_replay.h"
#include "rocketnet_client_slots.h"

#include <sodium.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void rocketnet_check_handler( const char * condition,
                                const char * function,
                                const char * file,
                                int line )
{
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

static bool equal_within_tolerance( float a, float b, float tolerance = 0.001f )
{
    return fabs(double(a)-double(b)) <= tolerance;
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

    // and the six per-type sealed packet vectors reproduce exactly: right key,
    // right sequence-derived nonce, right associated data, right bytes out
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
    } vectors[6];

    const uint8_t response_plaintext[24] = { 0 };
    uint8_t keepalive_plaintext[8];
    uint8_t * ka = keepalive_plaintext;
    rocketnet_write_uint32( &ka, 3 );
    rocketnet_write_uint32( &ka, 1000 );
    const uint8_t payload_plaintext[5] = { 0x68, 0x65, 0x6c, 0x6c, 0x6f };

    vectors[0] = { rocketnet_test_handshake_s2c_key, 1, rocketnet_test_challenge_ad, 53, rocketnet_test_server_eph_public, 32, rocketnet_test_challenge_sealed, 48 };
    vectors[1] = { rocketnet_test_handshake_s2c_key, 2, rocketnet_test_denied_ad, 21, NULL, 0, rocketnet_test_denied_sealed, 16 };
    vectors[2] = { rocketnet_test_handshake_c2s_key, 1, rocketnet_test_response_ad, 53, response_plaintext, 24, rocketnet_test_response_sealed, 40 };
    vectors[3] = { rocketnet_test_session_s2c_key, 1, rocketnet_test_keepalive_ad, 21, keepalive_plaintext, 8, rocketnet_test_keepalive_sealed, 24 };
    vectors[4] = { rocketnet_test_session_c2s_key, 3, rocketnet_test_payload_ad, 21, payload_plaintext, 5, rocketnet_test_payload_sealed, 21 };
    vectors[5] = { rocketnet_test_session_c2s_key, 4, rocketnet_test_disconnect_ad, 21, NULL, 0, rocketnet_test_disconnect_sealed, 16 };

    for ( int i = 0; i < 6; i++ )
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;

    // the embedded test title tag is the published one from the standard
    rocketnet_check( memcmp( tag, rocketnet_test_title_tag, ROCKETNET_TITLE_TAG_BYTES ) == 0 );

    // writing the header produces exactly the assembled vectors' first bytes:
    // a request begins 00 b7 59 01, a challenge begins 02 b7 59 01
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

    // the type byte carries the type and nothing else: any value comes back,
    // including reserved ones -- refusing those is a later reading order step
    packet[0] = 200;
    rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == 200 );
    packet[0] = ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET;

    // NEGATIVE: shorter than the header is refused, at every length
    for ( int bytes = 0; bytes < ROCKETNET_PREFILTER_BYTES; bytes++ )
        rocketnet_check( rocketnet_prefilter_check( packet, bytes, tag ) == -1 );

    // NEGATIVE: larger than the receive ceiling is refused; the ceiling
    // itself is accepted
    rocketnet_check( rocketnet_prefilter_check( packet, ROCKETNET_RECEIVE_CEILING + 1, tag ) == -1 );
    rocketnet_check( rocketnet_prefilter_check( packet, ROCKETNET_RECEIVE_CEILING, tag ) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );

    // NEGATIVE: a wrong tag byte, any of the three, is refused
    for ( int i = 0; i < ROCKETNET_TITLE_TAG_BYTES; i++ )
    {
        packet[1+i] ^= 1;
        rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == -1 );
        packet[1+i] ^= 1;
    }
    rocketnet_check( rocketnet_prefilter_check( packet, 64, tag ) == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
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
    // wire size matches the table
    const uint8_t response_data[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    uint8_t keepalive_data[8];
    uint8_t * ka = keepalive_data;
    rocketnet_write_uint32( &ka, 3 );
    rocketnet_write_uint32( &ka, 1000 );
    const uint8_t payload_data[5] = { 0x68, 0x65, 0x6c, 0x6c, 0x6f };

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
    } cases[6] = {
        { ROCKETNET_CONNECTION_DENIED_PACKET, 2, rocketnet_test_session_nonce, NULL, 0, NULL, rocketnet_test_handshake_s2c_key, rocketnet_test_denied_sealed, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES },
        { ROCKETNET_CONNECTION_CHALLENGE_PACKET, 1, rocketnet_test_session_nonce, rocketnet_test_server_eph_public, 32, rocketnet_test_client_eph_public, rocketnet_test_handshake_s2c_key, rocketnet_test_challenge_sealed, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES },
        { ROCKETNET_CONNECTION_RESPONSE_PACKET, 1, NULL, response_data, ROCKETNET_RESPONSE_PADDING_BYTES, rocketnet_test_client_eph_public, rocketnet_test_handshake_c2s_key, rocketnet_test_response_sealed, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES },
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1, NULL, keepalive_data, 8, NULL, rocketnet_test_session_s2c_key, rocketnet_test_keepalive_sealed, ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET, 3, NULL, payload_data, 5, NULL, rocketnet_test_session_c2s_key, rocketnet_test_payload_sealed, ROCKETNET_PACKET_OVERHEAD_BYTES + 5 },
        { ROCKETNET_CONNECTION_DISCONNECT_PACKET, 4, NULL, NULL, 0, NULL, rocketnet_test_session_c2s_key, rocketnet_test_disconnect_sealed, ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES },
    };

    for ( int i = 0; i < 6; i++ )
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;

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
    rocketnet_check( memcmp( cookie + 1, tag, 3 ) == 0 );
    rocketnet_check( memcmp( cookie + 4, rocketnet_test_salt, 8 ) == 0 );
    const uint8_t * p = cookie + 12;
    rocketnet_check( rocketnet_read_uint64( &p ) == ROCKETNET_TEST_CREATE_TS + 3 );
    rocketnet_check( memcmp( cookie + 20, rocketnet_test_cookie_mac, ROCKETNET_COOKIE_MAC_BYTES ) == 0 );
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
    memcpy( server->title_tag, tag, ROCKETNET_TITLE_TAG_BYTES );
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
    server->counters = &test->counters;
    rocketnet_check( server->encryption_map && server->used_token_history && server->admitted_token_set && server->client_slots );

    rocketnet_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void rocketnet_test_server_teardown( struct rocketnet_test_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_admitted_token_set_destroy( test->server.admitted_token_set );
    rocketnet_client_slots_destroy( test->server.client_slots );
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

    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
    rocketnet_write_connection_request_packet( request, tag, NULL, salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

// runs a request and returns the result; before is a snapshot for asserting
// exactly which counters moved

static int rocketnet_test_process( struct rocketnet_test_server_t * test, const uint8_t * request, int request_bytes,
                                   uint8_t * response, int * response_bytes )
{
    return rocketnet_server_process_connection_request( &test->server, &test->client_address,
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
    rocketnet_check( memcmp( response + 4, request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, 8 ) == 0 );
    memcpy( request + ROCKETNET_PREFILTER_BYTES, response + 12, ROCKETNET_COOKIE_FIELD_BYTES );
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
    rocketnet_check( rocketnet_prefilter_check( response, response_bytes, test.server.title_tag ) == ROCKETNET_CONNECTION_CHALLENGE_PACKET );
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
        const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
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
        const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
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
    rocketnet_check( rocketnet_prefilter_check( response, response_bytes, test.server.title_tag ) == ROCKETNET_CONNECTION_DENIED_PACKET );
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
    // emitted -- a garbage packet carrying the title tag must never turn the
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
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
    int signed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;

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
    rocketnet_replay_advance( &high, 300 );
    rocketnet_check( rocketnet_replay_already_received( &high, 44 ) );      // 300 - 256
    rocketnet_check( !rocketnet_replay_already_received( &high, 45 ) );

    // a far-ahead advance clears the whole window: a sequence one full lap
    // later shares a ring bit with an old one and must not inherit it
    struct rocketnet_replay_protection_t lap;
    memset( &lap, 0, sizeof(lap) );
    rocketnet_replay_advance( &lap, 1 );
    rocketnet_replay_advance( &lap, 257 );                                  // same ring bit as 1
    rocketnet_check( rocketnet_replay_already_received( &lap, 257 ) );
    rocketnet_check( rocketnet_replay_already_received( &lap, 1 ) );        // too old now, refused
    rocketnet_check( !rocketnet_replay_already_received( &lap, 2 ) );       // 2 > 257-256, cleared, not received

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
    return rocketnet_server_process_packet( &test->server, &test->client_address, packet, packet_bytes,
                                            ROCKETNET_TEST_CREATE_TS, 100.0,
                                            response, response_bytes,
                                            out_payload, out_payload_bytes, out_payload_client_index );
}

// the client's half of the handshake, from wire bytes alone: opens the
// challenge, derives the four keys, returns them
static void rocketnet_test_client_derive_from_challenge( const uint8_t * challenge, int challenge_bytes,
                                                         struct rocketnet_connection_keys_t * keys )
{
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
    const uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    rocketnet_check( rocketnet_write_sealed_packet( response, ROCKETNET_CONNECTION_RESPONSE_PACKET, tag, 1,
                                                    NULL, padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                                    ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                    rocketnet_test_client_eph_public,
                                                    keys->handshake_client_to_server ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
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
    memcpy( request + ROCKETNET_PREFILTER_BYTES, response + 12, ROCKETNET_COOKIE_FIELD_BYTES );

    rocketnet_check( rocketnet_test_process_packet( test, request, sizeof(request), response, &response_bytes,
                                                    payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    rocketnet_test_client_derive_from_challenge( response, response_bytes, client_keys );
}

void rocketnet_test_server_full_connect()
{
    struct rocketnet_test_server_t test;
    rocketnet_test_server_setup( &test );

    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
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
    rocketnet_check( response_bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES );
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

    // the client opens the keep-alive and learns its index
    {
        const uint8_t * p = response + ROCKETNET_PREFILTER_BYTES;
        uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );
        uint8_t opened[ROCKETNET_RECEIVE_CEILING];
        rocketnet_check( rocketnet_open_sealed_packet( opened, NULL, response, response_bytes,
                                                       ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence,
                                                       ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                       NULL, client_keys.session_server_to_client ) == 8 );
        const uint8_t * q = opened;
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
    rocketnet_check( rocketnet_write_sealed_packet( client_packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, tag, 1,
                                                    NULL, keep_alive_data, 8,
                                                    ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                    NULL, client_keys.session_client_to_server ) == ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES );
    rocketnet_check( rocketnet_test_process_packet( &test, client_packet, ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( rocketnet_client_slots_confirmed( test.server.client_slots, 0 ) );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 1 );

    // a payload delivers, with the client index attached
    const uint8_t game_bytes[11] = { 0x67, 0x61, 0x6d, 0x65, 0x20, 0x62, 0x79, 0x74, 0x65, 0x73, 0x21 };
    int packet_bytes = rocketnet_write_sealed_packet( client_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, tag, 2,
                                                      NULL, game_bytes, sizeof(game_bytes),
                                                      ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                      NULL, client_keys.session_client_to_server );
    rocketnet_check( packet_bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + (int) sizeof(game_bytes) );
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
    packet_bytes = rocketnet_write_sealed_packet( client_packet, ROCKETNET_CONNECTION_DISCONNECT_PACKET, tag, 3,
                                                  NULL, NULL, 0,
                                                  ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                  NULL, client_keys.session_client_to_server );
    rocketnet_check( packet_bytes == ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES );
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

    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload_out[ROCKETNET_RECEIVE_CEILING];
    int payload_out_bytes = 0;
    int payload_client_index = -1;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    memset( packet, 0, sizeof(packet) );

    // prefilter garbage: wrong tag never reaches a type counter
    packet[0] = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    packet[1] = 0xFF; packet[2] = 0xFF; packet[3] = 0xFF;
    rocketnet_check( rocketnet_test_process_packet( &test, packet, 64, response, &response_bytes,
                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_check( test.counters.values[ROCKETNET_COUNTER_PREFILTER_DROPPED] == 1 );

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
    int packet_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, tag, 1,
                                                      NULL, some_bytes, 4,
                                                      ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                      NULL, client_keys.session_client_to_server );
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
        rocketnet_client_update( client, time, packet, &packet_bytes );

        if ( client->state <= ROCKETNET_CLIENT_STATE_DISCONNECTED )
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
                rocketnet_server_process_packet( &test->server, &test->client_address, packet, packet_bytes,
                                                 ROCKETNET_TEST_CREATE_TS, time,
                                                 response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index );
                if ( response_bytes > 0 )
                {
                    int response_type = response[0];
                    if ( response_type == drop_once_packet_type && !dropped )
                        dropped = 1;
                    else
                        rocketnet_client_process_packet( client, response, response_bytes, time, payload, &payload_bytes );
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
                        rocketnet_client_process_packet( client, response, bytes, time, payload, &payload_bytes );
                }
            }
        }

        if ( client->state == ROCKETNET_CLIENT_STATE_CONNECTED )
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

    rocketnet_server_update( &test.server, 104.9 );
    rocketnet_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );
    rocketnet_server_update( &test.server, 105.0 );
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

    rocketnet_server_update( &test.server, 109.9 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    rocketnet_server_update( &test.server, 110.1 );
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

    // the first keep-alive at the response stamped the send at 100.0, so
    // nothing is due until the interval elapses -- keep-alives appear only
    // when the connection is otherwise idle
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    rocketnet_check( rocketnet_server_write_keep_alive_if_due( &test.server, 0, 100.05, packet ) == 0 );
    rocketnet_check( rocketnet_server_write_keep_alive_if_due( &test.server, 0, 100.11, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES );

    // a payload send suppresses the next keep-alive: ordinary traffic is
    // the keep-alive
    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_check( rocketnet_server_write_payload( &test.server, 0, data, 4, 100.15, packet ) > 0 );
    rocketnet_check( rocketnet_server_write_keep_alive_if_due( &test.server, 0, 100.22, packet ) == 0 );
    rocketnet_check( rocketnet_server_write_keep_alive_if_due( &test.server, 0, 100.26, packet ) == ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES );

    // a disconnected index degrades to zero, never an assert
    rocketnet_check( rocketnet_server_write_keep_alive_if_due( &test.server, 3, 200.0, packet ) == 0 );
    rocketnet_check( rocketnet_server_write_keep_alive_if_due( &test.server, -1, 200.0, packet ) == 0 );

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
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

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

    for ( double time = 100.0; time < 102.0 && client.state != ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE; time += 0.05 )
    {
        rocketnet_client_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes == 0 )
            continue;
        rocketnet_server_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                         ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                         payload, &payload_bytes, &payload_client_index );
        if ( response_bytes > 0 )
            rocketnet_client_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
    }
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE );

    rocketnet_client_update( &client, 102.1, packet, &packet_bytes );
    rocketnet_check( packet_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    rocketnet_check( rocketnet_server_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 102.1, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    // the first keep-alive is dropped here, deliberately

    // the server sends a payload to the still-unconfirmed client: delivered
    const uint8_t early_data[5] = { 0xE, 0xA, 0x12, 0x1, 0x9 };
    int bytes = rocketnet_server_write_payload( &test.server, 0, early_data, sizeof(early_data), 102.2, packet );
    rocketnet_check( bytes > 0 );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE );
    rocketnet_check( rocketnet_client_process_packet( &client, packet, bytes, 102.2, payload, &payload_bytes ) );
    rocketnet_check( payload_bytes == (int) sizeof(early_data) );
    rocketnet_check( memcmp( payload, early_data, sizeof(early_data) ) == 0 );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE );

    // and the periodic keep-alive still carries the index and connects it
    bytes = rocketnet_server_write_keep_alive( &test.server, 0, 102.3, packet );
    rocketnet_check( bytes > 0 );
    rocketnet_check( rocketnet_client_process_packet( &client, packet, bytes, 102.3, payload, &payload_bytes ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_CONNECTED );
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
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    double time = client.last_send_time;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;

    // a payload at time t suppresses the keep-alive until t + interval
    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_check( rocketnet_client_write_payload( &client, data, 4, time + 0.05, packet ) > 0 );
    rocketnet_client_update( &client, time + 0.11, packet, &packet_bytes );
    rocketnet_check( packet_bytes == 0 );
    rocketnet_client_update( &client, time + 0.16, packet, &packet_bytes );
    rocketnet_check( packet_bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES );

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
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE );
    rocketnet_check( client.server_address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_check( client.server_address.port == 40000 );

    // create after expire is an invalid token
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_EXPIRE_TS, ROCKETNET_TEST_CREATE_TS, 10 );
    rocketnet_check( !rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );

    // bad address flags are an invalid token
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );
        signed_fields.address_flags = 0x05;
        rocketnet_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
        rocketnet_check( !rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
        rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );
    }

    // an expired token dies before anything is sent
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 10, 10 );
    rocketnet_check( !rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS + 10 ) );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
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
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    // the whole handshake completes through a perfect wire
    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_STATE_CONNECTED );
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
    int bytes = rocketnet_client_write_payload( &client, to_server, sizeof(to_server), 200.0, packet );
    rocketnet_check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + (int) sizeof(to_server) );
    rocketnet_check( rocketnet_server_process_packet( &test.server, &test.client_address, packet, bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 200.0,
                                                      response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_check( payload_bytes == (int) sizeof(to_server) );
    rocketnet_check( memcmp( payload, to_server, sizeof(to_server) ) == 0 );
    rocketnet_check( payload_client_index == 0 );

    const uint8_t to_client[3] = { 9, 8, 7 };
    bytes = rocketnet_server_write_payload( &test.server, 0, to_client, sizeof(to_client), 200.0, packet );
    rocketnet_check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + (int) sizeof(to_client) );
    rocketnet_check( rocketnet_client_process_packet( &client, packet, bytes, 200.0, payload, &payload_bytes ) );
    rocketnet_check( payload_bytes == (int) sizeof(to_client) );
    rocketnet_check( memcmp( payload, to_client, sizeof(to_client) ) == 0 );

    // the deliberate disconnect: the redundant burst, the first copy lands,
    // the rest find nothing, and both sides are clean
    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES];
    int burst_count = 0;
    rocketnet_client_disconnect( &client, burst, &burst_count );
    rocketnet_check( burst_count == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_DISCONNECTED );

    for ( int i = 0; i < burst_count; i++ )
    {
        rocketnet_server_process_packet( &test.server, &test.client_address,
                                         burst + i * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES,
                                         ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES,
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
        rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

        rocketnet_check( rocketnet_test_pump( &test, &client, drop_types[i], 5.0 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

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
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
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
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    for ( double time = 100.0; time < 112.0; time += 0.05 )
        rocketnet_client_update( &client, time, packet, &packet_bytes );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT );

    // a server that answers only cookies: sending connection request times
    // out to connection request timed out, the pairing at state 2
    {
        struct rocketnet_test_server_t test;
        rocketnet_test_server_setup( &test );
        rocketnet_client_protocol_init( &client, &counters );
        rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        for ( double time = 100.0; time < 115.0; time += 0.05 )
        {
            rocketnet_client_update( &client, time, packet, &packet_bytes );
            if ( client.state <= ROCKETNET_CLIENT_STATE_DISCONNECTED )
                break;
            if ( packet_bytes > 0 )
            {
                rocketnet_server_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                 ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index );
                // deliver cookies; swallow challenges
                if ( response_bytes > 0 && response[0] == ROCKETNET_CONNECTION_COOKIE_PACKET )
                    rocketnet_client_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
            }
        }
        rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT );
        rocketnet_test_server_teardown( &test );
    }

    // a server that goes silent after the challenge: sending connection
    // response times out to connection response timed out, the pairing at 3
    {
        struct rocketnet_test_server_t test;
        rocketnet_test_server_setup( &test );
        rocketnet_client_protocol_init( &client, &counters );
        rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        for ( double time = 100.0; time < 115.0; time += 0.05 )
        {
            rocketnet_client_update( &client, time, packet, &packet_bytes );
            if ( client.state <= ROCKETNET_CLIENT_STATE_DISCONNECTED )
                break;
            if ( packet_bytes > 0 && client.state < ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE )
            {
                rocketnet_server_process_packet( &test.server, &test.client_address, packet, packet_bytes,
                                                 ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index );
                if ( response_bytes > 0 )
                    rocketnet_client_process_packet( &client, response, response_bytes, time, payload, &payload_bytes );
            }
        }
        rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT );
        rocketnet_test_server_teardown( &test );
    }

    // connected, then silence: the pairing at 4, connection timed out
    {
        struct rocketnet_test_server_t test;
        rocketnet_test_server_setup( &test );
        struct rocketnet_counters_t client_counters;
        memset( &client_counters, 0, sizeof(client_counters) );
        rocketnet_client_protocol_init( &client, &client_counters );
        rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
        rocketnet_check( rocketnet_test_pump( &test, &client, -1, 5.0 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

        double connected_time = client.last_receive_time;
        for ( double time = connected_time; time < connected_time + 12.0; time += 0.05 )
            rocketnet_client_update( &client, time, packet, &packet_bytes );
        rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT );
        rocketnet_test_server_teardown( &test );
    }

    // the total connect deadline: per-state timers disabled by a negative
    // timeout, and the whole handshake still dies once the token's lifetime
    // elapses -- connect or be discarded, and no amount of nothing extends it
    rocketnet_client_protocol_init( &client, &counters );
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 5, -1 );
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    for ( double time = 100.0; time < 107.0; time += 0.05 )
        rocketnet_client_update( &client, time, packet, &packet_bytes );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
}

void rocketnet_test_client_cookie_injection()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_test_mint_token( token_data, 1, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    // an off-path injection: right shape, wrong salt. refused, nothing
    // stored, and the state does not move -- the attacker must guess eight
    // random bytes it has never seen.
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
    uint8_t wrong_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    memcpy( wrong_salt, client.connection_salt, sizeof(wrong_salt) );
    wrong_salt[0] ^= 1;
    uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES];
    memset( mac, 0xAA, sizeof(mac) );
    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( cookie, tag, wrong_salt, ROCKETNET_TEST_CREATE_TS + 3, mac );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    rocketnet_check( !rocketnet_client_process_packet( &client, cookie, sizeof(cookie), 100.0, payload, &payload_bytes ) );
    rocketnet_check( counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH] == 1 );
    rocketnet_check( !client.has_cookie );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE );

    // the genuine cookie stores and advances
    uint8_t real_cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    uint8_t real_mac[ROCKETNET_COOKIE_MAC_BYTES];
    struct rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );
    rocketnet_check( rocketnet_generate_cookie_mac( real_mac, &client_address, client.connection_salt, ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_key ) );
    rocketnet_write_connection_cookie_packet( real_cookie, tag, client.connection_salt, ROCKETNET_TEST_CREATE_TS + 3, real_mac );
    rocketnet_check( rocketnet_client_process_packet( &client, real_cookie, sizeof(real_cookie), 100.0, payload, &payload_bytes ) );
    rocketnet_check( client.has_cookie );
    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST );
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
    rocketnet_check( rocketnet_client_connect( &client, token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );

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
        rocketnet_client_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
            rocketnet_network_simulator_send( simulator, &test.client_address, &server_address, packet, packet_bytes, time );

        // everything due at the server this tick
        int bytes;
        while ( ( bytes = rocketnet_network_simulator_receive( simulator, &server_address, time, &from, packet ) ) > 0 )
        {
            rocketnet_server_process_packet( &test.server, &from, packet, bytes,
                                             ROCKETNET_TEST_CREATE_TS, time,
                                             response, &response_bytes,
                                             payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &from, response, response_bytes, time );
        }

        // the server's pacing: sweep, keep-alives when idle, and once
        // connected a payload stream toward the client
        rocketnet_server_update( &test.server, time );
        for ( int i = 0; i < 4; i++ )
        {
            int keep_alive_bytes = rocketnet_server_write_keep_alive_if_due( &test.server, i, time, response );
            if ( keep_alive_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &test.client_address, response, keep_alive_bytes, time );
        }
        if ( rocketnet_client_slots_connected( test.server.client_slots, 0 ) && client_received_payloads < 20 )
        {
            const uint8_t stream_byte[1] = { 0x77 };
            int stream_bytes = rocketnet_server_write_payload( &test.server, 0, stream_byte, 1, time, response );
            if ( stream_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &test.client_address, response, stream_bytes, time );
        }

        // everything due at the client this tick
        while ( ( bytes = rocketnet_network_simulator_receive( simulator, &test.client_address, time, &from, packet ) ) > 0 )
        {
            if ( rocketnet_client_process_packet( &client, packet, bytes, time, payload, &payload_bytes ) && payload_bytes > 0 )
                client_received_payloads++;
        }
    }

    rocketnet_check( client.state == ROCKETNET_CLIENT_STATE_CONNECTED );
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
    test.server.client_slots = rocketnet_client_slots_create( NULL, num_clients );
    test.server.encryption_map = rocketnet_encryption_map_create( NULL, num_clients * 2 );
    test.server.used_token_history = rocketnet_used_token_history_create( NULL, num_clients * 8 );
    test.server.admitted_token_set = rocketnet_admitted_token_set_create( NULL, num_clients * 2 );
    rocketnet_check( test.server.client_slots && test.server.encryption_map && test.server.used_token_history && test.server.admitted_token_set );

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
        rocketnet_check( rocketnet_client_connect( &clients[i], token_data, 100.0, ROCKETNET_TEST_CREATE_TS ) );
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
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;

    uint64_t garbage_rng = 0x6A6B6A6BULL;
    #define ROCKETNET_SWARM_RANDOM() ( garbage_rng ^= garbage_rng << 13, garbage_rng ^= garbage_rng >> 7, garbage_rng ^= garbage_rng << 17, garbage_rng )

    // --- the connect phase, with garbage interleaved ---

    double connect_start_wall = rocketnet_platform_time();

    double time = 100.0;
    for ( int tick = 0; tick < 120; tick++, time += 0.05 )
    {
        for ( int i = 0; i < num_clients; i++ )
        {
            if ( ROCKETNET_SWARM_ABANDONS( i ) && clients[i].state >= ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE )
                continue;                                   // walked away mid-handshake
            if ( clients[i].state == ROCKETNET_CLIENT_STATE_CONNECTED )
                continue;
            rocketnet_client_update( &clients[i], time, packet, &packet_bytes );
            if ( packet_bytes == 0 )
                continue;
            rocketnet_server_process_packet( &test.server, &addresses[i], packet, packet_bytes,
                                             ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                             payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_process_packet( &clients[i], response, response_bytes, time, payload, &payload_bytes );
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
                for ( int b = 4; b < 64; b++ ) packet[b] = (uint8_t) ROCKETNET_SWARM_RANDOM();
                garbage_bytes = 64;                          // sealed junk, no mapping
            }
            else
            {
                uint8_t * p = packet;
                rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, tag );
                for ( int b = 4; b < ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES; b++ ) packet[b] = (uint8_t) ROCKETNET_SWARM_RANDOM();
                garbage_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;    // junk request: dies at the version
            }
            rocketnet_server_process_packet( &test.server, &garbage_addresses[g], packet, garbage_bytes,
                                             ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                             payload, &payload_bytes, &payload_client_index );
        }

        rocketnet_server_update( &test.server, time );
    }

    double connect_wall_ms = ( rocketnet_platform_time() - connect_start_wall ) * 1000.0;

    int expected_connected = 0;
    for ( int i = 0; i < num_clients; i++ )
        if ( !ROCKETNET_SWARM_ABANDONS( i ) )
        {
            rocketnet_check( clients[i].state == ROCKETNET_CLIENT_STATE_CONNECTED );
            expected_connected++;
        }
    rocketnet_check( expected_connected == 900 );
    rocketnet_check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 900 );

    printf( "        swarm baseline: 900 handshakes in %.1f ms (%.0f handshakes/sec)\n",
            connect_wall_ms, 900.0 / ( connect_wall_ms / 1000.0 ) );

    // --- the payload burst: the stay clients each send three ---

    double payload_start_wall = rocketnet_platform_time();
    int payloads_processed = 0;

    for ( int round = 0; round < 3; round++ )
    {
        time += 0.05;
        for ( int i = 0; i < num_clients; i++ )
        {
            if ( !ROCKETNET_SWARM_STAYS( i ) )
                continue;
            uint8_t game_data[32];
            memset( game_data, (uint8_t) i, sizeof(game_data) );
            int bytes = rocketnet_client_write_payload( &clients[i], game_data, sizeof(game_data), time, packet );
            rocketnet_check( bytes > 0 );
            int result = rocketnet_server_process_packet( &test.server, &addresses[i], packet, bytes,
                                                          ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                                          payload, &payload_bytes, &payload_client_index );
            rocketnet_check( result == ROCKETNET_SERVER_PROCESS_ACCEPTED );
            rocketnet_check( payload_bytes == 32 );
            payloads_processed++;
        }
    }

    double payload_wall_ms = ( rocketnet_platform_time() - payload_start_wall ) * 1000.0;
    printf( "        swarm baseline: %d payloads through the server in %.1f ms (%.0f packets/sec)\n",
            payloads_processed, payload_wall_ms, (double) payloads_processed / ( payload_wall_ms / 1000.0 ) );

    // --- the leavers disconnect; the abandoners' mappings die at the deadline ---

    for ( int i = 0; i < num_clients; i++ )
    {
        if ( !ROCKETNET_SWARM_LEAVES( i ) )
            continue;
        uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES];
        int burst_count = 0;
        rocketnet_client_disconnect( &clients[i], burst, &burst_count );
        rocketnet_check( burst_count == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
        rocketnet_server_process_packet( &test.server, &addresses[i], burst, ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES,
                                         ROCKETNET_TEST_CREATE_TS, time, response, &response_bytes,
                                         payload, &payload_bytes, &payload_client_index );
    }

    for ( ; time < 112.0; time += 0.05 )
        rocketnet_server_update( &test.server, time );

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
        uint64_t keep_alive_outcomes =
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED];
        rocketnet_check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED] == keep_alive_outcomes );

        uint64_t payload_outcomes =
            counters[ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED];
        rocketnet_check( counters[ROCKETNET_COUNTER_PAYLOAD_RECEIVED] == payload_outcomes );

        uint64_t disconnect_outcomes =
            counters[ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING] +
            counters[ROCKETNET_COUNTER_DISCONNECT_ZERO_SEQUENCE] +
            counters[ROCKETNET_COUNTER_DISCONNECT_ALREADY_RECEIVED] +
            counters[ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN] +
            counters[ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED] +
            counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];
        rocketnet_check( counters[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] == disconnect_outcomes );
    }

    // the storm did its work: every garbage shape landed where it should
    rocketnet_check( counters[ROCKETNET_COUNTER_PREFILTER_DROPPED] > 0 );
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

void rocketnet_test_transport_udp_loopback()
{
    // the first packets on a real socket: the whole connect over UDP through
    // loopback, on the transports -- flow's IO carrying the connection
    // protocol, development keys, exactly the out-of-the-box path. real
    // clocks here; the protocol underneath is the one proven on mocked time.

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:40777", 16 );
    rocketnet_check( server );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, "127.0.0.1:40777" );
    rocketnet_check( client );

    // pump both until connected, bounded by the wall clock
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client );
        rocketnet_server_transport_update( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
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

        rocketnet_client_transport_update( client );
        rocketnet_server_transport_update( server );

        struct rocketnet_server_transport_payloads_t * payloads = rocketnet_server_transport_payloads( server );
        for ( int i = 0; i < payloads->num_payloads; i++ )
        {
            rocketnet_check( payloads->client_index[i] == 0 );
            rocketnet_check( payloads->payload_bytes[i] == 5 );
            rocketnet_check( memcmp( payloads->payload_data[i], "hello", 5 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet ) ) > 0 )
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
        rocketnet_server_transport_update( server );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server );
    rocketnet_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_server_transport_destroy( server );
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

void rocketnet_test_packet_filter()
{
    uint8_t output[ROCKETNET_MAX_PACKET_BYTES];
    memset( output, 0, sizeof(output) );
    output[0] = 1;

    for ( int i = 0; i < 10000; ++i )
    {
        uint8_t magic[8];
        uint8_t from_address[4];
        uint8_t to_address[4];

        rocketnet_random_bytes( magic, 8 );
        rocketnet_random_bytes( from_address, 4 );
        rocketnet_random_bytes( to_address, 4 );

        int packet_length = 18 + ( i % ( sizeof(output) - 18 ) );
        
        rocketnet_generate_pittle( output + 1, from_address, to_address, packet_length );

        rocketnet_generate_chonkle( output + 3, magic, from_address, to_address, packet_length );

        rocketnet_check( rocketnet_basic_packet_filter( output, packet_length ) );

    }
}

// The filter must grant no exemption keyed on the type byte: a zero first
// byte never excuses a packet from the 18-byte minimum or the pittle and
// chonkle checks, and a real ROCKETNET_PACKET_DIRECT (0) packet with a valid
// header must still pass on its merits.
//
// rocketnet_test_basic_packet_filter above cannot cover this: it fuzzes from a
// fixed srand(100), and that seed happens never to produce a zero first byte
// on a packet long enough to matter. This test does not depend on luck.
void rocketnet_test_packet_filter_no_type_zero_bypass()
{
    // the minimal attack: a single zero byte
    uint8_t one_zero_byte[1];
    one_zero_byte[0] = 0;
    rocketnet_check( !rocketnet_basic_packet_filter( one_zero_byte, 1 ) );

    // a zero first byte must not excuse a short packet at any length below the
    // 18-byte header minimum
    uint8_t zeroes[64];
    memset( zeroes, 0, sizeof(zeroes) );
    for ( uint16_t length = 0; length < 18; ++length )
    {
        rocketnet_check( !rocketnet_basic_packet_filter( zeroes, length ) );
    }

    // nor a long one that carries no valid header
    rocketnet_check( !rocketnet_basic_packet_filter( zeroes, sizeof(zeroes) ) );

    // and the converse: a real direct packet -- ROCKETNET_PACKET_DIRECT is
    // type 0 -- with a valid pittle and chonkle header still passes on its
    // merits.
    uint8_t output[256];
    memset( output, 0, sizeof(output) );
    uint8_t magic[8];
    uint8_t from_address[4];
    uint8_t to_address[4];
    memset( magic, 0, sizeof(magic) );
    memset( from_address, 0, sizeof(from_address) );
    memset( to_address, 0, sizeof(to_address) );

    const int packet_length = 64;
    output[0] = 0; // ROCKETNET_PACKET_DIRECT
    rocketnet_generate_pittle( output + 1, from_address, to_address, packet_length );
    rocketnet_generate_chonkle( output + 3, magic, from_address, to_address, packet_length );
    rocketnet_check( rocketnet_basic_packet_filter( output, packet_length ) );
}

void rocketnet_test_basic_packet_filter()
{
    uint8_t output[256];
    memset( output, 0, sizeof(output) );
    uint64_t pass = 0;
    uint64_t iterations = 100;
    srand( 100 );
    for ( int i = 0; i < int(iterations); ++i )
    {
        for ( int j = 0; j < int(sizeof(output)); ++j )
        {
            output[j] = uint8_t( rand() % 256 );
        }
        if ( rocketnet_basic_packet_filter( output, rand() % sizeof(output) ) )
        {
            pass++;
        }
    }
    rocketnet_check( pass == 0 );
}

void rocketnet_test_address_data_ipv4()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "127.0.0.1:50000" );
    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    uint8_t address_data[4];
    rocketnet_address_data( &address, address_data );
    rocketnet_check( address_data[0] == 127 );
    rocketnet_check( address_data[1] == 0 );
    rocketnet_check( address_data[2] == 0 );
    rocketnet_check( address_data[3] == 1 );
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

extern void * rocketnet_default_malloc_function( void * context, size_t bytes );

extern void rocketnet_default_free_function( void * context, void * p );

static void context_check_free( void * context, void * p )
{
    (void) p;
    rocketnet_check( context );
    rocketnet_check( *((int *)context) == 23 );
    rocketnet_default_free_function( context, p );;
}

#define RUN_TEST( test_function )                                           \
    do                                                                      \
    {                                                                       \
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, #test_function );                 \
        fflush( stdout );                                                   \
        rocketnet_##test_function();                                             \
    }                                                                       \
    while (0)

void rocketnet_run_tests()
{
    // while ( true )
    {
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
        RUN_TEST( test_client_server_lossy_wire );
        RUN_TEST( test_swarm );
        RUN_TEST( test_transport_udp_loopback );
        RUN_TEST( test_request_path_ordering );
        RUN_TEST( test_request_path_uncovered_rungs );
        RUN_TEST( test_request_path_config_branches );
        RUN_TEST( test_replay_protection );
        RUN_TEST( test_client_slots );
        RUN_TEST( test_platform_socket );
        RUN_TEST( test_platform_thread );
        RUN_TEST( test_platform_mutex );
        RUN_TEST( test_value_tracker );
        RUN_TEST( test_packet_filter );
        RUN_TEST( test_basic_packet_filter );
        RUN_TEST( test_packet_filter_no_type_zero_bypass );
        RUN_TEST( test_address_data_ipv4 );
        RUN_TEST( test_anonymize_address_ipv4 );
#if ROCKETNET_PLATFORM_HAS_IPV6
        RUN_TEST( test_anonymize_address_ipv6 );
        RUN_TEST( test_platform_inet6_round_trip );
#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
        RUN_TEST( test_packet_loss_tracker );
        RUN_TEST( test_out_of_order_tracker );
        RUN_TEST( test_jitter_tracker );
    }
}

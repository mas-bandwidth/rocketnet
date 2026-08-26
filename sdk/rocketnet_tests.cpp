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

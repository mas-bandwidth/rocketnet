/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The keyed filter value, STANDARD.md "The keyed filter value" (lines 489 to
// 557): SipHash-2-4 under the 16-byte magic over the 7-byte input of packet
// type, packet length as a little-endian uint16, then the four bytes following
// the prefilter header; the low 4 bytes of the 64-bit result, little-endian,
// are the filter value. Pure computation, no wiring into a receive path, no
// default magic, no change to the wire.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_read_write.h"

#if defined(ROCKETNET_USE_SYSTEM_SODIUM)
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

// the published SipHash-2-4 reference key 00..0f and message 00..06, whose
// 64-bit value is 0xab0200f58b01d137; the low 4 bytes little-endian are the
// expected filter value
static const uint8_t ref_magic[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                       0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
static const uint8_t ref_expected[4] = { 0x37, 0xd1, 0x01, 0x8b };

// the 7-byte SipHash input exactly as the standard composes it: packet type,
// packet length little-endian uint16, then the four bytes following the
// prefilter header
static void compose_input( uint8_t * input, uint8_t packet_type, uint16_t packet_length, const uint8_t * four_bytes )
{
    uint8_t * p = input;
    rocketnet_write_uint8( &p, packet_type );
    rocketnet_write_uint16( &p, packet_length );
    rocketnet_write_bytes( &p, four_bytes, 4 );
}

// expected filter value computed directly from crypto_shorthash_siphash24, the
// same primitive the implementation uses, as the standard's definition
static void expected_from_sodium( const uint8_t * magic, uint8_t packet_type, uint16_t packet_length, const uint8_t * four_bytes, uint8_t * out )
{
    uint8_t input[7];
    compose_input( input, packet_type, packet_length, four_bytes );

    uint8_t hash[8];
    check( crypto_shorthash_siphash24( hash, input, sizeof( input ), magic ) == 0 );
    out[0] = hash[0];
    out[1] = hash[1];
    out[2] = hash[2];
    out[3] = hash[3];
}

static void check_equal( const uint8_t * a, const uint8_t * b )
{
    check( a[0] == b[0] );
    check( a[1] == b[1] );
    check( a[2] == b[2] );
    check( a[3] == b[3] );
}

static void test_keyed_filter_value_known_answers()
{
    uint8_t out[4];

    // the reference vector: magic 00..0f, message 00 01 02 03 04 05 06.
    // packet type 0, packet length 0x0201 (little-endian bytes 01 02), the
    // four bytes 03 04 05 06.
    static const uint8_t ref_four[4] = { 0x03, 0x04, 0x05, 0x06 };
    rocketnet_keyed_filter_value( ref_magic, 0x00, 0x0201, ref_four, out );
    check_equal( out, ref_expected );

    // two further fixed magics and inputs whose expectation is computed from
    // crypto_shorthash directly
    static const uint8_t magic_b[16] = { 0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67,
                                         0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98 };
    static const uint8_t four_b[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t expected_b[4];
    expected_from_sodium( magic_b, 0x07, 0x1234, four_b, expected_b );
    rocketnet_keyed_filter_value( magic_b, 0x07, 0x1234, four_b, out );
    check_equal( out, expected_b );

    static const uint8_t magic_c[16] = { 0xff, 0x00, 0x55, 0xaa, 0x10, 0x20, 0x30, 0x40,
                                         0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0 };
    static const uint8_t four_c[4] = { 0x01, 0x00, 0xff, 0xfe };
    uint8_t expected_c[4];
    expected_from_sodium( magic_c, 0xf0, 0x0001, four_c, expected_c );
    rocketnet_keyed_filter_value( magic_c, 0xf0, 0x0001, four_c, out );
    check_equal( out, expected_c );
}

static void test_keyed_filter_value_inputs_are_the_standards()
{
    static const uint8_t magic[16] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
                                       0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    static const uint8_t four[4] = { 0x77, 0x66, 0x55, 0x44 };

    uint8_t base[4];
    rocketnet_keyed_filter_value( magic, 0x05, 0x0123, four, base );

    uint8_t out[4];

    // changing any one of the inputs changes the filter value
    rocketnet_keyed_filter_value( magic, 0x06, 0x0123, four, out );
    check( memcmp( base, out, 4 ) != 0 );

    rocketnet_keyed_filter_value( magic, 0x05, 0x0023, four, out );
    check( memcmp( base, out, 4 ) != 0 );

    rocketnet_keyed_filter_value( magic, 0x05, 0x0122, four, out );
    check( memcmp( base, out, 4 ) != 0 );

    for ( int i = 0; i < 4; i++ )
    {
        uint8_t altered[4];
        memcpy( altered, four, 4 );
        altered[i] ^= 0x40;
        rocketnet_keyed_filter_value( magic, 0x05, 0x0123, altered, out );
        check( memcmp( base, out, 4 ) != 0 );
    }

    // the bytes after the four do not: two buffers sharing the leading 7
    // bytes but differing beyond them give the same filter value
    uint8_t buf_a[12];
    uint8_t buf_b[12];
    {
        uint8_t * p = buf_a;
        rocketnet_write_uint8( &p, 0x05 );
        rocketnet_write_uint16( &p, 0x0123 );
        rocketnet_write_bytes( &p, four, 4 );
        for ( int i = 7; i < 12; i++ )
            buf_a[i] = (uint8_t) i;
    }
    {
        uint8_t * p = buf_b;
        rocketnet_write_uint8( &p, 0x05 );
        rocketnet_write_uint16( &p, 0x0123 );
        rocketnet_write_bytes( &p, four, 4 );
        for ( int i = 7; i < 12; i++ )
            buf_b[i] = (uint8_t) ( 0xff - i );
    }
    rocketnet_keyed_filter_value( magic, 0x05, 0x0123, buf_a + 3, out );
    check( memcmp( base, out, 4 ) == 0 );
    rocketnet_keyed_filter_value( magic, 0x05, 0x0123, buf_b + 3, out );
    check( memcmp( base, out, 4 ) == 0 );

    // the length is little-endian: swapping the two length bytes changes the
    // value (0x0123 vs 0x2301)
    rocketnet_keyed_filter_value( magic, 0x05, 0x2301, four, out );
    check( memcmp( base, out, 4 ) != 0 );
}

REGISTER_UNIT_TEST( test_keyed_filter_value_known_answers );
REGISTER_UNIT_TEST( test_keyed_filter_value_inputs_are_the_standards );

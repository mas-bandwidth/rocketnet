/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_address_and_base64_contracts.cpp -- the error arms' contracts.
//
// Two error arms at BASE, pinned here:
//
//   * rocketnet_base64_decode_data (sdk/rocketnet_base64.cpp:89) never
//     returns a negative value: every refusal is `return 0` and every
//     success returns a non-negative length. The `< 0` arm in
//     rocketnet_base64_decode_string (sdk/rocketnet_base64.cpp:175) is
//     therefore unreachable, and the table below makes that provable by
//     driving decode_data over empty, valid, invalid, mis-padded and
//     over-long inputs and asserting each result is >= 0.
//
//   * rocketnet_address_to_string (sdk/rocketnet_address.cpp:101) tests
//     snprintf's return `< 0` and logs "truncated". snprintf signals
//     truncation by returning a value >= the buffer size; a negative
//     return is a formatting or encoding error. The longest bracketed
//     IPv6 rendering with a port is 47 characters against a
//     ROCKETNET_MAX_ADDRESS_STRING_LENGTH of 256 (sdk/rocketnet.h:60), so
//     truncation is unreachable today -- but the arm must name what it
//     checks. The longest rendering is asserted whole and in-bounds.
//
// Both contracts hold at BASE, so both tests pass there; that is expected.
// No socket, no clock: the inputs are fixed and the functions are pure.

#include "test_unit_registry.h"

#include "rocketnet_address.h"
#include "rocketnet_base64.h"
#include "wycheproof.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// rocketnet_base64_decode_data never returns a negative value.
// ---------------------------------------------------------------------------

struct base64_decode_case_t
{
    const char * input;     // the input string, never NULL
    size_t output_size;     // the output buffer capacity
    const char * what;      // the case, for a failing line
};

static const struct base64_decode_case_t k_decode_cases[] =
{
    { "",       8, "empty input" },
    { "AAAA",   8, "valid whole group" },
    { "AA==",   8, "valid two-pad group" },
    { "AAA=",   8, "valid one-pad group" },
    { "!!!!",   8, "all-invalid characters" },
    { "AA*A",   8, "one invalid character" },
    { "AAA",    8, "mis-padded length, not a multiple of four" },
    { "AA=",    8, "mis-padded length, one pad and short" },
    { "A===",   8, "too many pad characters" },
    { "AAAA",   2, "over-long for the output buffer" },
};

static void test_base64_decode_data_never_returns_negative()
{
    const int count = (int) ( sizeof(k_decode_cases) / sizeof(k_decode_cases[0]) );
    for ( int i = 0; i < count; ++i )
    {
        uint8_t output[16];
        memset( output, 0xEE, sizeof(output) );
        const int result = rocketnet_base64_decode_data( k_decode_cases[i].input,
                                                          output,
                                                          k_decode_cases[i].output_size );
        if ( result < 0 )
        {
            printf( "base64 decode case %d (%s) returned %d, negative\n",
                    i, k_decode_cases[i].what, result );
            fflush( stdout );
        }
        rocketnet_unit_check( result >= 0 );
    }
}

// ---------------------------------------------------------------------------
// rocketnet_base64_encode_data refuses an encoded length over INT_MAX.
//
// The return is an int, so an encoded length above INT_MAX cannot be
// returned whole. 3 * 2^29 input bytes encode to exactly 2^31 characters,
// INT_MAX + 1. The test cannot allocate 2 GB, so it relies on the refusal
// touching nothing: the input and output are 16 bytes each and output_size
// claims SIZE_MAX, so a guard that reads or writes a byte walks off both
// buffers. At head it returns -1 with the output untouched.
// ---------------------------------------------------------------------------

static void test_base64_encode_refuses_length_over_int_max()
{
    uint8_t input[16];
    memset( input, 0x5A, sizeof(input) );
    char output[16];
    memset( output, 0x7E, sizeof(output) );

    const size_t input_length = (size_t) 3 * ( (size_t) 1 << 29 );     // encodes to 2^31 characters

    const int result = rocketnet_base64_encode_data( input, input_length, output, SIZE_MAX );

    rocketnet_unit_check( result == -1 );
    rocketnet_unit_check( output[0] == 0x7E );
    rocketnet_unit_check( output[15] == 0x7E );
}

// ---------------------------------------------------------------------------
// the longest bracketed IPv6 address with a port fits the buffer.
// ---------------------------------------------------------------------------

static void test_address_ipv6_with_port_fits_the_buffer()
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof(address) );
    address.type = ROCKETNET_ADDRESS_IPV6;
    for ( int i = 0; i < 8; ++i )
        address.data.ipv6[i] = 0xFFFF;
    address.port = 65535;

    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const char * rendered = rocketnet_address_to_string( &address, buffer );

    const char * expected = "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535";

    if ( strcmp( rendered, expected ) != 0 )
    {
        printf( "longest ipv6 rendering was \"%s\", expected \"%s\"\n", rendered, expected );
        fflush( stdout );
    }
    rocketnet_unit_check( strcmp( rendered, expected ) == 0 );
    rocketnet_unit_check( strlen( rendered ) < ROCKETNET_MAX_ADDRESS_STRING_LENGTH );
}

// ---------------------------------------------------------------------------
// rocketnet_address_load_ipv4 (sdk/rocketnet_address.cpp:13) round-trips:
// its wire-big-endian inputs land as the matching typed address, parse the
// same dotted-quad text back to equality, and rocketnet_address_ipv4
// (sdk/rocketnet_address.cpp:318) returns the same four bytes.
// ---------------------------------------------------------------------------

static void test_address_load_ipv4_round_trips()
{
    uint32_t ipv4;
    const uint8_t ipv4_bytes[4] = { 203, 0, 113, 10 };
    memcpy( &ipv4, ipv4_bytes, 4 );

    uint16_t port_big_endian;
    const uint8_t port_bytes[2] = { 0x9c, 0x40 };
    memcpy( &port_big_endian, port_bytes, 2 );

    struct rocketnet_address_t address;
    memset( &address, 0, sizeof(address) );
    rocketnet_address_load_ipv4( &address, ipv4, port_big_endian );

    rocketnet_unit_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( address.data.ipv4[0] == 203 );
    rocketnet_unit_check( address.data.ipv4[1] == 0 );
    rocketnet_unit_check( address.data.ipv4[2] == 113 );
    rocketnet_unit_check( address.data.ipv4[3] == 10 );
    rocketnet_unit_check( address.port == 40000 );

    struct rocketnet_address_t parsed;
    rocketnet_unit_check( rocketnet_address_parse( &parsed, "203.0.113.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_equal( &address, &parsed ) );

    const uint32_t round_trip = rocketnet_address_ipv4( &address );
    const uint8_t * round_trip_bytes = (const uint8_t*) &round_trip;
    rocketnet_unit_check( round_trip_bytes[0] == 203 );
    rocketnet_unit_check( round_trip_bytes[1] == 0 );
    rocketnet_unit_check( round_trip_bytes[2] == 113 );
    rocketnet_unit_check( round_trip_bytes[3] == 10 );
}

// ---------------------------------------------------------------------------
// rocketnet_address_to_string_without_port (sdk/rocketnet_address.cpp:165)
// renders each address type without its port: dotted-quad, compressed IPv6
// and the NONE fallback for a zeroed address.
// ---------------------------------------------------------------------------

static void test_address_to_string_without_port_each_type()
{
    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];

    struct rocketnet_address_t ipv4;
    rocketnet_unit_check( rocketnet_address_parse( &ipv4, "203.0.113.10:40000" ) );
    const char * ipv4_text = rocketnet_address_to_string_without_port( &ipv4, buffer );
    rocketnet_unit_check( ipv4_text == buffer );
    rocketnet_unit_check( strcmp( ipv4_text, "203.0.113.10" ) == 0 );

    struct rocketnet_address_t ipv6;
    rocketnet_unit_check( rocketnet_address_parse( &ipv6, "[2001:db8::1]:40000" ) );
    const char * ipv6_text = rocketnet_address_to_string_without_port( &ipv6, buffer );
    rocketnet_unit_check( ipv6_text == buffer );
    rocketnet_unit_check( strcmp( ipv6_text, "2001:db8::1" ) == 0 );

    struct rocketnet_address_t none;
    memset( &none, 0, sizeof(none) );
    const char * none_text = rocketnet_address_to_string_without_port( &none, buffer );
    rocketnet_unit_check( none_text == buffer );
    rocketnet_unit_check( strcmp( none_text, "NONE" ) == 0 );
}

// ---------------------------------------------------------------------------
// rocketnet_address_to_string and rocketnet_address_to_string_without_port
// render every address form exactly: IPv4 and IPv6 with and without a port,
// the longest forms, an IPv4-mapped IPv6 address, and NONE (with and without
// a port). These strings are pinned so that folding the two functions onto
// one formatting helper changes no output.
// ---------------------------------------------------------------------------

struct address_to_string_case_t
{
    const char * input;             // parsed text, or NULL for a zeroed NONE address
    uint16_t none_port;             // the port set on the NONE address
    const char * with_port;         // expected rocketnet_address_to_string
    const char * without_port;      // expected rocketnet_address_to_string_without_port
};

static const struct address_to_string_case_t k_address_to_string_cases[] =
{
    { "203.0.113.10:40000",                            0,     "203.0.113.10:40000",                              "203.0.113.10" },
    { "203.0.113.10",                                  0,     "203.0.113.10",                                    "203.0.113.10" },
    { "255.255.255.255:65535",                         0,     "255.255.255.255:65535",                           "255.255.255.255" },
    { "0.0.0.0:1",                                     0,     "0.0.0.0:1",                                       "0.0.0.0" },
    { "[2001:db8::1]:40000",                           0,     "[2001:db8::1]:40000",                             "2001:db8::1" },
    { "2001:db8::1",                                   0,     "2001:db8::1",                                     "2001:db8::1" },
    { "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535", 0,   "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff" },
    { "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",       0,     "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",         "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff" },
    { "[::ffff:192.0.2.1]:40000",                      0,     "[::ffff:192.0.2.1]:40000",                        "::ffff:192.0.2.1" },
    { "::ffff:192.0.2.1",                              0,     "::ffff:192.0.2.1",                                "::ffff:192.0.2.1" },
    { "[::1]:1",                                       0,     "[::1]:1",                                         "::1" },
    { NULL,                                            0,     "NONE",                                            "NONE" },
    { NULL,                                            40000, "NONE",                                            "NONE" },
};

static void test_address_to_string_forms_pinned()
{
    const int count = (int) ( sizeof(k_address_to_string_cases) / sizeof(k_address_to_string_cases[0]) );
    for ( int i = 0; i < count; ++i )
    {
        const struct address_to_string_case_t & c = k_address_to_string_cases[i];

        struct rocketnet_address_t address;
        memset( &address, 0, sizeof(address) );
        if ( c.input )
        {
            rocketnet_unit_check( rocketnet_address_parse( &address, c.input ) );
        }
        else
        {
            address.port = c.none_port;
        }

        char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];

        memset( buffer, 'x', sizeof(buffer) );
        const char * with_port = rocketnet_address_to_string( &address, buffer );
        if ( with_port != buffer || strcmp( with_port, c.with_port ) != 0 )
        {
            printf( "address case %d to_string was \"%s\", expected \"%s\"\n", i, with_port, c.with_port );
            fflush( stdout );
        }
        rocketnet_unit_check( with_port == buffer );
        rocketnet_unit_check( strcmp( with_port, c.with_port ) == 0 );

        memset( buffer, 'x', sizeof(buffer) );
        const char * without_port = rocketnet_address_to_string_without_port( &address, buffer );
        if ( without_port != buffer || strcmp( without_port, c.without_port ) != 0 )
        {
            printf( "address case %d to_string_without_port was \"%s\", expected \"%s\"\n", i, without_port, c.without_port );
            fflush( stdout );
        }
        rocketnet_unit_check( without_port == buffer );
        rocketnet_unit_check( strcmp( without_port, c.without_port ) == 0 );
    }
}

// ---------------------------------------------------------------------------
// rocketnet_address_anonymize touches only the data of the type it names: a
// NONE address keeps every data byte and loses only its port, while IPv4,
// mapped IPv6 and plain IPv6 are cut exactly as before.
// ---------------------------------------------------------------------------

static void test_address_anonymize_leaves_other_types_untouched()
{
    struct rocketnet_address_t none;
    memset( &none, 0, sizeof(none) );
    none.type = ROCKETNET_ADDRESS_NONE;
    memset( &none.data, 0xA5, sizeof(none.data) );
    none.port = 7;
    rocketnet_address_anonymize( &none );
    const uint8_t * none_bytes = (const uint8_t*) &none.data;
    for ( size_t i = 0; i < sizeof(none.data); ++i )
    {
        if ( none_bytes[i] != 0xA5 )
        {
            printf( "anonymized NONE address data byte %d was 0x%02x, expected 0xa5\n", (int) i, none_bytes[i] );
            fflush( stdout );
        }
        rocketnet_unit_check( none_bytes[i] == 0xA5 );
    }
    rocketnet_unit_check( none.port == 0 );
    rocketnet_unit_check( none.type == ROCKETNET_ADDRESS_NONE );

    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];

    struct rocketnet_address_t ipv4;
    rocketnet_unit_check( rocketnet_address_parse( &ipv4, "203.0.113.10:40000" ) );
    rocketnet_address_anonymize( &ipv4 );
    rocketnet_unit_check( strcmp( rocketnet_address_to_string( &ipv4, buffer ), "203.0.113.0" ) == 0 );

    struct rocketnet_address_t mapped;
    rocketnet_unit_check( rocketnet_address_parse( &mapped, "[::ffff:203.0.113.10]:40000" ) );
    rocketnet_address_anonymize( &mapped );
    rocketnet_unit_check( mapped.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( mapped.data.ipv6[5] == 0xFFFF );
    rocketnet_unit_check( mapped.data.ipv6[6] == 0xCB00 );
    rocketnet_unit_check( mapped.data.ipv6[7] == 0x7100 );
    rocketnet_unit_check( mapped.port == 0 );

    struct rocketnet_address_t ipv6;
    rocketnet_unit_check( rocketnet_address_parse( &ipv6, "[2001:db8:1:2:3:4:5:6]:40000" ) );
    rocketnet_address_anonymize( &ipv6 );
    rocketnet_unit_check( strcmp( rocketnet_address_to_string( &ipv6, buffer ), "2001:db8:1:2::" ) == 0 );
}

// ---------------------------------------------------------------------------
// the widest IPv4 text, with and without its port, comes back whole from
// rocketnet_address_to_string and rocketnet_address_to_string_without_port.
// ---------------------------------------------------------------------------

static void test_address_to_string_ipv4_widest_is_complete()
{
    struct rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "255.255.255.255:65535" ) );

    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const char * with_port = rocketnet_address_to_string( &address, buffer );
    if ( strcmp( with_port, "255.255.255.255:65535" ) != 0 )
    {
        printf( "widest ipv4 rendering was \"%s\", expected \"255.255.255.255:65535\"\n", with_port );
        fflush( stdout );
    }
    rocketnet_unit_check( with_port == buffer );
    rocketnet_unit_check( strcmp( with_port, "255.255.255.255:65535" ) == 0 );

    const char * without_port = rocketnet_address_to_string_without_port( &address, buffer );
    rocketnet_unit_check( without_port == buffer );
    rocketnet_unit_check( strcmp( without_port, "255.255.255.255" ) == 0 );

    address.port = 0;
    const char * zero_port = rocketnet_address_to_string( &address, buffer );
    rocketnet_unit_check( strcmp( zero_port, "255.255.255.255" ) == 0 );
}

// ---------------------------------------------------------------------------
// the vendored Wycheproof files load and hold the counts, id range, result
// set and tallies at the pin, and every hex field the sibling cards read
// decodes.
// ---------------------------------------------------------------------------

static void test_wycheproof_files_load_and_count()
{
    std::vector<wycheproof_test_t> tests;
    int num_tests = 0;
    int num_groups = 0;

    // chacha20_poly1305_test.json pins the aead hex fields key iv aad msg ct
    // tag.
    rocketnet_unit_check( wycheproof_load( "chacha20_poly1305_test.json", tests, &num_tests, &num_groups ) );
    rocketnet_unit_check( (int) tests.size() == num_tests );
    rocketnet_unit_check( num_tests == 325 );
    rocketnet_unit_check( num_groups == 10 );
    static const char * chacha_fields[] = { "key", "iv", "aad", "msg", "ct", "tag" };
    int chacha_valid = 0, chacha_invalid = 0, chacha_acceptable = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        rocketnet_unit_check( tests[i].tc_id == (int) i + 1 );
        if ( tests[i].result == "valid" ) chacha_valid++;
        else if ( tests[i].result == "invalid" ) chacha_invalid++;
        else if ( tests[i].result == "acceptable" ) chacha_acceptable++;
        else rocketnet_unit_check( 0 );
        std::vector<uint8_t> bytes;
        for ( int f = 0; f < 6; f++ )
        {
            std::map<std::string, std::string>::const_iterator it = tests[i].fields.find( chacha_fields[f] );
            rocketnet_unit_check( it != tests[i].fields.end() );
            rocketnet_unit_check( wycheproof_hex( it->second, bytes ) );
        }
    }
    rocketnet_unit_check( chacha_valid == 256 && chacha_invalid == 69 && chacha_acceptable == 0 );

    // x25519_test.json pins the hex fields public private shared.
    rocketnet_unit_check( wycheproof_load( "x25519_test.json", tests, &num_tests, &num_groups ) );
    rocketnet_unit_check( (int) tests.size() == num_tests );
    rocketnet_unit_check( num_tests == 518 );
    static const char * x25519_fields[] = { "public", "private", "shared" };
    int x25519_valid = 0, x25519_invalid = 0, x25519_acceptable = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        rocketnet_unit_check( tests[i].tc_id == (int) i + 1 );
        if ( tests[i].result == "valid" ) x25519_valid++;
        else if ( tests[i].result == "invalid" ) x25519_invalid++;
        else if ( tests[i].result == "acceptable" ) x25519_acceptable++;
        else rocketnet_unit_check( 0 );
        std::vector<uint8_t> bytes;
        for ( int f = 0; f < 3; f++ )
        {
            std::map<std::string, std::string>::const_iterator it = tests[i].fields.find( x25519_fields[f] );
            rocketnet_unit_check( it != tests[i].fields.end() );
            rocketnet_unit_check( wycheproof_hex( it->second, bytes ) );
        }
    }
    rocketnet_unit_check( x25519_valid == 264 && x25519_acceptable == 254 && x25519_invalid == 0 );

    // ed25519_test.json pins msg and sig, and the group publicKey.pk.
    rocketnet_unit_check( wycheproof_load( "ed25519_test.json", tests, &num_tests, &num_groups ) );
    rocketnet_unit_check( (int) tests.size() == num_tests );
    rocketnet_unit_check( num_tests == 151 );
    rocketnet_unit_check( num_groups == 78 );
    int ed25519_valid = 0, ed25519_invalid = 0, ed25519_acceptable = 0;
    for ( size_t i = 0; i < tests.size(); i++ )
    {
        rocketnet_unit_check( tests[i].tc_id == (int) i + 1 );
        if ( tests[i].result == "valid" ) ed25519_valid++;
        else if ( tests[i].result == "invalid" ) ed25519_invalid++;
        else if ( tests[i].result == "acceptable" ) ed25519_acceptable++;
        else rocketnet_unit_check( 0 );
        std::vector<uint8_t> bytes;
        std::map<std::string, std::string>::const_iterator msg = tests[i].fields.find( "msg" );
        std::map<std::string, std::string>::const_iterator sig = tests[i].fields.find( "sig" );
        std::map<std::string, std::string>::const_iterator pk = tests[i].group.find( "publicKey.pk" );
        rocketnet_unit_check( msg != tests[i].fields.end() );
        rocketnet_unit_check( sig != tests[i].fields.end() );
        rocketnet_unit_check( pk != tests[i].group.end() );
        rocketnet_unit_check( wycheproof_hex( msg->second, bytes ) );
        rocketnet_unit_check( wycheproof_hex( sig->second, bytes ) );
        rocketnet_unit_check( wycheproof_hex( pk->second, bytes ) );
    }
    rocketnet_unit_check( ed25519_valid == 88 && ed25519_invalid == 63 && ed25519_acceptable == 0 );
}

// ---------------------------------------------------------------------------
// the strict reader refuses malformed JSON and bad hex.
// ---------------------------------------------------------------------------

static void test_wycheproof_reader_refuses_malformed_json()
{
    wyj::node_t node;
    rocketnet_unit_check( !wyj::parse_text( "{\"a\": [1, 2", node ) );
    rocketnet_unit_check( !wyj::parse_text( "{\"a\": 1,}", node ) );
    rocketnet_unit_check( !wyj::parse_text( "{\"a\": 1} x", node ) );
    rocketnet_unit_check( !wyj::parse_text( "{\"a\": 1, \"a\": 2}", node ) );
    rocketnet_unit_check( !wyj::parse_text( "{\"a\": 1.5}", node ) );
    rocketnet_unit_check( !wyj::parse_text( "{\"a\": \"é\"}", node ) );
    rocketnet_unit_check( !wyj::parse_text( "[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]", node ) );
    rocketnet_unit_check( wyj::parse_text( "{\"a\": [1, -2, \"x\\n\", true, false, null]}", node ) );

    std::vector<uint8_t> bytes;
    rocketnet_unit_check( !wycheproof_hex( "abc", bytes ) );
    rocketnet_unit_check( !wycheproof_hex( "zz", bytes ) );
    rocketnet_unit_check( wycheproof_hex( "", bytes ) );
    rocketnet_unit_check( wycheproof_hex( "0aFf", bytes ) );
    rocketnet_unit_check( bytes.size() == 2 && bytes[0] == 0x0a );
    rocketnet_unit_check( bytes[1] == 0xff );
}

REGISTER_UNIT_TEST( test_base64_decode_data_never_returns_negative );
REGISTER_UNIT_TEST( test_base64_encode_refuses_length_over_int_max );
REGISTER_UNIT_TEST( test_address_ipv6_with_port_fits_the_buffer );
REGISTER_UNIT_TEST( test_address_load_ipv4_round_trips );
REGISTER_UNIT_TEST( test_address_to_string_without_port_each_type );
REGISTER_UNIT_TEST( test_address_to_string_forms_pinned );
REGISTER_UNIT_TEST( test_address_anonymize_leaves_other_types_untouched );
REGISTER_UNIT_TEST( test_address_to_string_ipv4_widest_is_complete );
REGISTER_UNIT_TEST( test_wycheproof_files_load_and_count );
REGISTER_UNIT_TEST( test_wycheproof_reader_refuses_malformed_json );

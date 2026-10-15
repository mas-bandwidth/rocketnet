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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

REGISTER_UNIT_TEST( test_base64_decode_data_never_returns_negative );
REGISTER_UNIT_TEST( test_address_ipv6_with_port_fits_the_buffer );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The test side of the standard's vectors (standard/STYLE.md section 9).
//
// Every `vector <id>` block in standard/TRANSPORT.md and standard/MESSAGES.md
// has a twin in a test_unit_standard_<topic>.cpp file:
//
//     static const rn_standard_vector_t vectors[] =
//     {
//         RN_STANDARD_VECTOR( "t3.token-identifier", "f520da9d95fd072256b47d755f810f61" ),
//     };
//
// tools/conformance/check_rn_standard.py compares the document's bytes with
// the twin's text, both ways. The test case compares the twin with the bytes
// the implementation regenerates: rn_standard_vectors_walk hands each id to
// the case's generator, and fails on an id the generator does not know, on a
// length that differs, and on the first byte that differs, naming it. So the
// chain document -> twin -> implementation has no unchecked link.
//
// No socket, no thread, no clock: a generator computes from fixed inputs.

#pragma once

#ifndef ROCKETNET_TESTS_TEST_UNIT_STANDARD_H
#define ROCKETNET_TESTS_TEST_UNIT_STANDARD_H

#include "test_unit_registry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct rn_standard_vector_t
{
    const char * id;
    const char * hex;           // lower-case hex, spaces allowed
};

#define RN_STANDARD_VECTOR( id, hex ) { id, hex }

#define RN_STANDARD_VECTOR_MAX_BYTES 4096

// a generator writes the bytes of vector `id` into `out` and returns their
// count, or -1 when it does not know the id
typedef int ( *rn_standard_vector_generator_t )( const char * id, uint8_t * out, int capacity );

inline int rn_standard_hex_nibble( char c )
{
    if ( c >= '0' && c <= '9' )
        return c - '0';
    if ( c >= 'a' && c <= 'f' )
        return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' )
        return c - 'A' + 10;
    return -1;
}

// decodes hex with optional spaces; returns the byte count, or -1 for a bad digit,
// an odd digit count or more than capacity bytes
inline int rn_standard_hex_decode( const char * hex, uint8_t * out, int capacity )
{
    int count = 0;
    int high = -1;
    for ( const char * p = hex; *p; p++ )
    {
        if ( *p == ' ' )
            continue;
        const int nibble = rn_standard_hex_nibble( *p );
        if ( nibble < 0 )
            return -1;
        if ( high < 0 )
        {
            high = nibble;
            continue;
        }
        if ( count >= capacity )
            return -1;
        out[count++] = (uint8_t) ( ( high << 4 ) | nibble );
        high = -1;
    }
    return high < 0 ? count : -1;
}

// compares one regenerated vector with its twin; returns the index of the
// first differing byte, the shorter length when one is a prefix of the other,
// or -1 when they are equal
inline int rn_standard_first_difference( const uint8_t * expected, int expected_bytes, const uint8_t * actual, int actual_bytes )
{
    const int n = expected_bytes < actual_bytes ? expected_bytes : actual_bytes;
    for ( int i = 0; i < n; i++ )
        if ( expected[i] != actual[i] )
            return i;
    return expected_bytes == actual_bytes ? -1 : n;
}

// walks the table: every twin is regenerated and compared. returns the
// number of vectors checked; a failure prints the vector and the first
// differing byte, then fails the case
inline int rn_standard_vectors_walk( const rn_standard_vector_t * table, int count, rn_standard_vector_generator_t generator )
{
    static uint8_t expected[RN_STANDARD_VECTOR_MAX_BYTES];
    static uint8_t actual[RN_STANDARD_VECTOR_MAX_BYTES];
    for ( int i = 0; i < count; i++ )
    {
        const int expected_bytes = rn_standard_hex_decode( table[i].hex, expected, RN_STANDARD_VECTOR_MAX_BYTES );
        if ( expected_bytes < 0 )
            printf( "vector %s: its twin's hex does not decode\n", table[i].id );
        rocketnet_unit_check( expected_bytes >= 0 );

        const int actual_bytes = generator( table[i].id, actual, RN_STANDARD_VECTOR_MAX_BYTES );
        if ( actual_bytes < 0 )
            printf( "vector %s: no generator for this id\n", table[i].id );
        rocketnet_unit_check( actual_bytes >= 0 );

        const int first = rn_standard_first_difference( expected, expected_bytes, actual, actual_bytes );
        if ( first >= 0 )
            printf( "vector %s: first difference at byte %d (twin %d bytes, regenerated %d bytes)\n",
                    table[i].id, first, expected_bytes, actual_bytes );
        rocketnet_unit_check( first < 0 );
    }
    return count;
}

#endif // ROCKETNET_TESTS_TEST_UNIT_STANDARD_H

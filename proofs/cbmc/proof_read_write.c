/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the byte readers and writers, the bounded readers, and
    rocketnet_copy_string.

    The readers and writers are the inline leaves in sdk/rocketnet_read_write.h.
    rocketnet_copy_string is the leaf in sdk/rocketnet.cpp; its body is
    copied here because that file is C++. The bounded readers are what
    variable-geometry parses call: a read that would pass the end pointer
    returns 0 and leaves the cursor and the output alone. dest_size 0
    returns 0 and writes nothing, which is the underflow the size check
    closes (a size of 0 used to make dest_size - 1 wrap).

    Buffers are 8 bytes. A bounded read's backing object has 8 further
    bytes past the end pointer so the guard's pointer add stays inside
    the object; the end pointer is still the caller's length. Values,
    lengths, and cursors inside that window are symbolic.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 9. copy_string and the checks run at most 8 iterations.
*/

#include <stdint.h>
#include <string.h>

#include "rocketnet_read_write.h"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static uint8_t nondet_uchar( void ) { return 0; }
static uint16_t nondet_ushort( void ) { return 0; }
static uint32_t nondet_uint( void ) { return 0; }
static uint64_t nondet_uint64( void ) { return 0; }
static int nondet_int( void ) { return 0; }
static unsigned nondet_unsigned( void ) { return 0; }
#else
uint8_t nondet_uchar( void );
uint16_t nondet_ushort( void );
uint32_t nondet_uint( void );
uint64_t nondet_uint64( void );
int nondet_int( void );
unsigned nondet_unsigned( void );
#endif

/* sdk/rocketnet.cpp, rocketnet_copy_string. */

size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size )
{
    if ( !dest || !source || dest_size == 0 )
        return 0;
    memset( dest, 0, dest_size );
    size_t i = 0;
    for ( ; i < dest_size - 1; i++ )
    {
        if ( source[i] == '\0' )
            break;
        dest[i] = source[i];
    }
    return i;
}

#define LOG   8
#define SLACK 8
#define OBJ   ( LOG + SLACK )

static void harness_round_trip( void )
{
    uint8_t buf8[1];
    uint8_t v8 = nondet_uchar();
    uint8_t * w8 = buf8;
    rocketnet_write_uint8( &w8, v8 );
    __CPROVER_assert( w8 == buf8 + 1, "uint8 writer advances one byte" );
    const uint8_t * r8 = buf8;
    __CPROVER_assert( rocketnet_read_uint8( &r8 ) == v8, "uint8 round-trips" );
    __CPROVER_assert( r8 == buf8 + 1, "uint8 reader advances one byte" );

    uint8_t buf16[2];
    uint16_t v16 = nondet_ushort();
    uint8_t * w16 = buf16;
    rocketnet_write_uint16( &w16, v16 );
    __CPROVER_assert( w16 == buf16 + 2, "uint16 writer advances two bytes" );
    const uint8_t * r16 = buf16;
    __CPROVER_assert( rocketnet_read_uint16( &r16 ) == v16, "uint16 round-trips" );
    __CPROVER_assert( buf16[0] == (uint8_t) ( v16 & 0xFF ), "uint16 low byte is little-endian" );
    __CPROVER_assert( buf16[1] == (uint8_t) ( ( v16 >> 8 ) & 0xFF ), "uint16 high byte is little-endian" );

    uint8_t buf32[4];
    uint32_t v32 = nondet_uint();
    uint8_t * w32 = buf32;
    rocketnet_write_uint32( &w32, v32 );
    __CPROVER_assert( w32 == buf32 + 4, "uint32 writer advances four bytes" );
    const uint8_t * r32 = buf32;
    __CPROVER_assert( rocketnet_read_uint32( &r32 ) == v32, "uint32 round-trips" );

    uint8_t buf64[8];
    uint64_t v64 = nondet_uint64();
    uint8_t * w64 = buf64;
    rocketnet_write_uint64( &w64, v64 );
    __CPROVER_assert( w64 == buf64 + 8, "uint64 writer advances eight bytes" );
    const uint8_t * r64 = buf64;
    __CPROVER_assert( rocketnet_read_uint64( &r64 ) == v64, "uint64 round-trips" );

    uint8_t src[LOG];
    uint8_t dst[LOG];
    int n = nondet_int();
    __CPROVER_assume( n >= 0 && n <= LOG );
    uint8_t * wb = dst;
    rocketnet_write_bytes( &wb, src, n );
    __CPROVER_assert( wb == dst + n, "write_bytes advances by the length" );
    uint8_t out[LOG];
    const uint8_t * rb = dst;
    rocketnet_read_bytes( &rb, out, n );
    __CPROVER_assert( rb == dst + n, "read_bytes advances by the length" );
    for ( int i = 0; i < LOG; i++ )
    {
        if ( i < n )
            __CPROVER_assert( out[i] == src[i], "read_bytes returns the bytes written" );
    }

    uint8_t zeros[LOG];
    memset( zeros, 0x5a, sizeof( zeros ) );
    uint8_t * wz = zeros;
    rocketnet_write_zero_bytes( &wz, n );
    __CPROVER_assert( wz == zeros + n, "write_zero_bytes advances by the length" );
    for ( int i = 0; i < LOG; i++ )
    {
        if ( i < n )
            __CPROVER_assert( zeros[i] == 0, "write_zero_bytes clears the prefix" );
        else
            __CPROVER_assert( zeros[i] == 0x5a, "write_zero_bytes leaves the tail" );
    }
}

static void harness_bounded( void )
{
    uint8_t raw[OBJ];
    const uint8_t * end = raw + LOG;
    int off = nondet_int();
    __CPROVER_assume( off >= 0 && off <= LOG );

    const uint8_t * p = raw + off;
    uint8_t v8 = 0xa5;
    int rc8 = rocketnet_read_uint8_bounded( &p, end, &v8 );
    if ( LOG - off >= 1 )
    {
        __CPROVER_assert( rc8 == 1, "uint8 bounded read succeeds inside the end" );
        __CPROVER_assert( p == raw + off + 1, "uint8 bounded read advances one" );
        __CPROVER_assert( v8 == raw[off], "uint8 bounded read returns the byte" );
    }
    else
    {
        __CPROVER_assert( rc8 == 0, "uint8 bounded read refuses past the end" );
        __CPROVER_assert( p == raw + off, "uint8 bounded refusal leaves the cursor" );
        __CPROVER_assert( v8 == 0xa5, "uint8 bounded refusal leaves the output" );
    }

    p = raw + off;
    uint16_t v16 = 0xa5a5;
    int rc16 = rocketnet_read_uint16_bounded( &p, end, &v16 );
    if ( LOG - off >= 2 )
    {
        uint16_t expect = (uint16_t) raw[off] | (uint16_t) raw[off + 1] << 8;
        __CPROVER_assert( rc16 == 1, "uint16 bounded read succeeds inside the end" );
        __CPROVER_assert( p == raw + off + 2, "uint16 bounded read advances two" );
        __CPROVER_assert( v16 == expect, "uint16 bounded read is little-endian" );
    }
    else
    {
        __CPROVER_assert( rc16 == 0, "uint16 bounded read refuses past the end" );
        __CPROVER_assert( p == raw + off, "uint16 bounded refusal leaves the cursor" );
        __CPROVER_assert( v16 == 0xa5a5, "uint16 bounded refusal leaves the output" );
    }

    p = raw + off;
    uint32_t v32 = 0xa5a5a5a5u;
    int rc32 = rocketnet_read_uint32_bounded( &p, end, &v32 );
    if ( LOG - off >= 4 )
    {
        __CPROVER_assert( rc32 == 1, "uint32 bounded read succeeds inside the end" );
        __CPROVER_assert( p == raw + off + 4, "uint32 bounded read advances four" );
    }
    else
    {
        __CPROVER_assert( rc32 == 0, "uint32 bounded read refuses past the end" );
        __CPROVER_assert( p == raw + off, "uint32 bounded refusal leaves the cursor" );
        __CPROVER_assert( v32 == 0xa5a5a5a5u, "uint32 bounded refusal leaves the output" );
    }

    p = raw + off;
    uint64_t v64 = 0xa5a5a5a5a5a5a5a5ULL;
    int rc64 = rocketnet_read_uint64_bounded( &p, end, &v64 );
    if ( LOG - off >= 8 )
    {
        __CPROVER_assert( rc64 == 1, "uint64 bounded read succeeds inside the end" );
        __CPROVER_assert( p == raw + off + 8, "uint64 bounded read advances eight" );
    }
    else
    {
        __CPROVER_assert( rc64 == 0, "uint64 bounded read refuses past the end" );
        __CPROVER_assert( p == raw + off, "uint64 bounded refusal leaves the cursor" );
        __CPROVER_assert( v64 == 0xa5a5a5a5a5a5a5a5ULL, "uint64 bounded refusal leaves the output" );
    }

    /* The bounded variable reader: a symbolic byte count. */
    int n = nondet_int();
    __CPROVER_assume( n >= 0 && n <= LOG );
    uint8_t out[LOG];
    memset( out, 0x5a, sizeof( out ) );
    p = raw + off;
    int rcn = rocketnet_read_bytes_bounded( &p, end, out, n );
    if ( n <= LOG - off )
    {
        __CPROVER_assert( rcn == 1, "variable bounded read succeeds inside the end" );
        __CPROVER_assert( p == raw + off + n, "variable bounded read advances by the count" );
        for ( int i = 0; i < LOG; i++ )
        {
            if ( i < n )
                __CPROVER_assert( out[i] == raw[off + i], "variable bounded read copies only its count" );
            else
                __CPROVER_assert( out[i] == 0x5a, "variable bounded read does not write past its count" );
        }
    }
    else
    {
        __CPROVER_assert( rcn == 0, "variable bounded read refuses past the end" );
        __CPROVER_assert( p == raw + off, "variable bounded refusal leaves the cursor" );
        for ( int i = 0; i < LOG; i++ )
            __CPROVER_assert( out[i] == 0x5a, "variable bounded refusal writes nothing" );
    }
}

static void harness_copy_string( void )
{
    char sentinel[4];
    memset( sentinel, 0x5a, sizeof( sentinel ) );
    char source_nz[4];
    source_nz[0] = 'h';
    source_nz[1] = 'i';
    source_nz[2] = '\0';
    source_nz[3] = '\0';

    __CPROVER_assert( rocketnet_copy_string( sentinel, source_nz, 0 ) == 0, "zero size returns 0" );
    __CPROVER_assert( sentinel[0] == 0x5a && sentinel[3] == 0x5a, "zero size writes nothing" );
    __CPROVER_assert( rocketnet_copy_string( NULL, source_nz, 4 ) == 0, "null dest returns 0" );
    __CPROVER_assert( sentinel[0] == 0x5a, "null dest writes nothing" );
    __CPROVER_assert( rocketnet_copy_string( sentinel, NULL, 4 ) == 0, "null source returns 0" );
    __CPROVER_assert( sentinel[0] == 0x5a, "null source writes nothing" );

    unsigned dest_size = nondet_unsigned();
    __CPROVER_assume( dest_size >= 1 && dest_size <= LOG );
    char dest[LOG];
    char source[LOG];
    memset( dest, 0x5a, sizeof( dest ) );
    size_t copied = rocketnet_copy_string( dest, source, dest_size );
    __CPROVER_assert( copied < dest_size, "copy length fits in the buffer" );
    __CPROVER_assert( dest[copied] == '\0', "copy is terminated" );
    for ( unsigned i = 0; i < LOG; i++ )
    {
        if ( i < copied )
        {
            __CPROVER_assert( dest[i] == source[i], "copy keeps the source prefix" );
            __CPROVER_assert( source[i] != '\0', "copy stops at the source terminator" );
        }
        else if ( i < dest_size )
            __CPROVER_assert( dest[i] == '\0', "copy clears the rest of the buffer" );
        else
            __CPROVER_assert( dest[i] == 0x5a, "copy does not write past dest_size" );
    }
    if ( copied < dest_size - 1 )
        __CPROVER_assert( source[copied] == '\0', "a short copy stopped on a terminator" );
}

int main( void )
{
    harness_round_trip();
    harness_bounded();
    harness_copy_string();
#if defined(REACHABILITY_CONTROL)
    __CPROVER_assert( 0, "read-write reachability control" );
#endif
    return 0;
}

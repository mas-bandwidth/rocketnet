/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of rocketnet_reconstruct_sequence (k = 32) and of the 24-bit
    window reconstruction (k = 24, signed half range).

    rocketnet_reconstruct_near and rocketnet_reconstruct_sequence are the
    leaf in sdk/rocketnet_protocol.cpp. The 24-bit function is the standard's
    half-range reading: delta is the signed residue in -2^23 .. 2^23-1, and
    a tie at 2^23 goes to the lower value. The sdk has no symbol for that
    reading yet; this file proves the arithmetic.

    Both satisfy |result - reference| <= 2^(k-1), and every in-window value
    round-trips, including values whose low k bits sit across the modulus
    boundary. The 32-bit tie goes to the larger candidate, so the strictly
    lower value at distance 2^31 is outside the round-trip window. The 24-bit
    tie goes to the lower value, so the strictly upper value at distance
    2^23 is outside it.

    The 32-bit proof assumes the expected value sits where both neighbouring
    epochs exist and the above-candidate add does not wrap:
    2^32 <= expected < 2^64 - 2^32. The 24-bit proof assumes
    2^23 <= reference <= 2^64 - 2^23, so a negative delta does not underflow
    and a positive delta does not overflow. The uint64 floor and ceiling
    outside those bounds are not proved.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
    Unwinding: 1. Both reconstructions are straight-line.
*/

#include <stdint.h>

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static uint64_t nondet_uint64( void ) { return 0; }
static uint32_t nondet_uint32( void ) { return 0; }
static unsigned char nondet_uchar( void ) { return 0; }
#else
uint64_t nondet_uint64( void );
uint32_t nondet_uint32( void );
unsigned char nondet_uchar( void );
#endif

#define U32_MOD  0x100000000ULL
#define U32_HALF 0x80000000ULL
#define U24_MOD  0x1000000ULL
#define U24_HALF 0x800000ULL
#define U24_MASK 0xFFFFFFULL

uint64_t rocketnet_reconstruct_near( uint32_t wire_value, uint64_t expected )
{
    uint64_t base = expected & 0xFFFFFFFF00000000ULL;

    uint64_t candidate = base | wire_value;

    uint64_t best = candidate;
    uint64_t best_distance = candidate > expected ? candidate - expected : expected - candidate;

    if ( base >= 0x100000000ULL )
    {
        uint64_t below = ( base - 0x100000000ULL ) | wire_value;
        uint64_t distance = below > expected ? below - expected : expected - below;
        if ( distance < best_distance || ( distance == best_distance && below > best ) )
        {
            best = below;
            best_distance = distance;
        }
    }

    {
        uint64_t above = ( base + 0x100000000ULL ) | wire_value;
        uint64_t distance = above > expected ? above - expected : expected - above;
        if ( distance < best_distance || ( distance == best_distance && above > best ) )
        {
            best = above;
            best_distance = distance;
        }
    }

    return best;
}

uint64_t rocketnet_reconstruct_sequence( uint32_t wire_sequence, uint64_t highest_opened )
{
    return rocketnet_reconstruct_near( wire_sequence, highest_opened + 1 );
}

/* Signed half range. delta is in [-2^23, 2^23). A residue of exactly 2^23
   is the lower candidate. */
uint64_t rocketnet_reconstruct_u24( uint32_t wire, uint64_t reference )
{
    uint32_t w = wire & (uint32_t) U24_MASK;
    uint32_t rlow = (uint32_t) ( reference & U24_MASK );
    uint32_t d;

    if ( w >= rlow )
        d = w - rlow;
    else
        d = ( w + (uint32_t) U24_MOD ) - rlow;

    if ( d >= (uint32_t) U24_HALF )
        return reference - ( U24_MOD - (uint64_t) d );
    return reference + (uint64_t) d;
}

static uint64_t abs_diff( uint64_t a, uint64_t b )
{
    return a > b ? a - b : b - a;
}

int main( void )
{
    uint32_t wire;
    uint64_t expected;
    uint64_t result;
    uint64_t highest;
    uint64_t mag;
    unsigned char neg;
    uint64_t n;
    uint64_t reference;

    /* --- 32-bit nearest candidate, every wire value --- */
    wire = nondet_uint32();
    expected = nondet_uint64();
    __CPROVER_assume( expected >= U32_MOD );
    __CPROVER_assume( expected < 0xFFFFFFFF00000000ULL );

    result = rocketnet_reconstruct_near( wire, expected );
    __CPROVER_assert( ( result & 0xFFFFFFFFULL ) == (uint64_t) wire,
                      "32-bit result keeps the wire low bits" );
    __CPROVER_assert( abs_diff( result, expected ) <= U32_HALF,
                      "32-bit |result - reference| <= 2^31" );

    /* Round-trip every in-window value. neg selects the lower side.
       Distance 2^31 is included only on the upper side (the tie winner). */
    mag = nondet_uint64();
    neg = nondet_uchar();
    __CPROVER_assume( neg <= 1 );
    __CPROVER_assume( mag <= U32_HALF );
    __CPROVER_assume( !( neg && mag == U32_HALF ) );
    __CPROVER_assume( !neg || mag <= expected );
    __CPROVER_assume( neg || expected <= UINT64_MAX - mag );
    n = neg ? expected - mag : expected + mag;
    __CPROVER_assert( rocketnet_reconstruct_near( (uint32_t) n, expected ) == n,
                      "32-bit in-window value round-trips across the wrap" );

    /* --- rocketnet_reconstruct_sequence, reference = highest_opened + 1 --- */
    highest = nondet_uint64();
    __CPROVER_assume( highest >= U32_MOD - 1 );
    __CPROVER_assume( highest < 0xFFFFFFFEFFFFFFFFULL );
    expected = highest + 1;

    wire = nondet_uint32();
    result = rocketnet_reconstruct_sequence( wire, highest );
    __CPROVER_assert( ( result & 0xFFFFFFFFULL ) == (uint64_t) wire,
                      "reconstruct_sequence keeps the wire low bits" );
    __CPROVER_assert( abs_diff( result, expected ) <= U32_HALF,
                      "reconstruct_sequence |result - reference| <= 2^31" );

    mag = nondet_uint64();
    neg = nondet_uchar();
    __CPROVER_assume( neg <= 1 );
    __CPROVER_assume( mag <= U32_HALF );
    __CPROVER_assume( !( neg && mag == U32_HALF ) );
    __CPROVER_assume( !neg || mag <= expected );
    __CPROVER_assume( neg || expected <= UINT64_MAX - mag );
    n = neg ? expected - mag : expected + mag;
    __CPROVER_assert( rocketnet_reconstruct_sequence( (uint32_t) n, highest ) == n,
                      "reconstruct_sequence round-trips every in-window value" );

    /* --- 24-bit signed half range --- */
    reference = nondet_uint64();
    __CPROVER_assume( reference >= U24_HALF );
    __CPROVER_assume( reference <= UINT64_MAX - ( U24_HALF - 1 ) );

    wire = nondet_uint32();
    result = rocketnet_reconstruct_u24( wire, reference );
    __CPROVER_assert( ( result & U24_MASK ) == ( (uint64_t) wire & U24_MASK ),
                      "24-bit result keeps the wire low bits" );
    __CPROVER_assert( abs_diff( result, reference ) <= U24_HALF,
                      "24-bit |result - reference| <= 2^23" );

    mag = nondet_uint64();
    neg = nondet_uchar();
    __CPROVER_assume( neg <= 1 );
    __CPROVER_assume( mag <= U24_HALF );
    __CPROVER_assume( !( !neg && mag == U24_HALF ) );
    __CPROVER_assume( !neg || mag <= reference );
    __CPROVER_assume( neg || reference <= UINT64_MAX - mag );
    n = neg ? reference - mag : reference + mag;
    __CPROVER_assert( rocketnet_reconstruct_u24( (uint32_t) n, reference ) == n,
                      "24-bit in-window value round-trips across the wrap" );

#if defined( REACHABILITY_CONTROL )
    __CPROVER_assert( 0, "reconstruct reachability control" );
#endif
    return 0;
}

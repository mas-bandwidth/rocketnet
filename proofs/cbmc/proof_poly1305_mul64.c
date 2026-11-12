/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the limb arithmetic of the AArch64 Poly1305 (RFC 8439 2.5)
    in sodium/local/mul64.h.in, which this harness includes, never copies:
    the block step RN_POLY_STEP, shared by the mul64 Poly1305 and the one-pass
    AEAD, and the final reduction rn_mul64_finish.

    harness_step, for every accumulator h0, h1 with h2 <= 4, every 16-byte
    block, hibit 0 or 1, and every clamped r (r0 & 0x0ffffffc0fffffff,
    r1 & 0x0ffffffc0ffffffc, s1 = r1 + r1 / 4):
      - no unsigned operation wraps (--unsigned-overflow-check): the 128-bit
        products and sums and the 64-bit h2 * s1, h2 * r0 and fold stay in
        range, so the truncating casts drop only bits the next limb carries;
      - on exit h2 <= 4, the invariant the next step and the final reduction
        assume (h < 5 * 2^128 < 2p).
    The congruence (h' = (h + m + hibit * 2^128) * r mod p) is not proved
    here: harness_congruence states it as an equality of 256-bit products,
    and CBMC 6.11.0 on the studio did not finish the propositional reduction
    in 30 minutes (2026-11-12, proofs/cbmc/INVENTORY.md). It is
    checked by test_sodium_dispatch against the donna Poly1305: the RFC 8439
    A.3 edge vectors, the r = 1 carry patterns, and the sweep.

    harness_finish, for every h0, h1 and h2 <= 4: the tag is
    ((h mod p) + pad) mod 2^128, with h mod p written as a comparison
    (h >= p exactly when h2 >= 4, or h2 = 3 with h1 all ones and
    h0 >= 2^64 - 5), against the code's mask.

    Flags: the Makefile's CBMC_FLAGS for harness_step. harness_finish drops
    --unsigned-overflow-check: the tag is defined mod 2^128 (RFC 8439 2.5.1),
    so the pad addition wraps by design, and the mask is 0 - (g2 >> 2); its
    assertions state the wrapped value exactly instead.
    Unwinding: 17 (the 16-byte block fill; the loads and stores take 8).
    Entry: harness_step, harness_finish (one run each, --function).
    REACHABILITY_CONTROL adds one named assert(0) after the code under proof.
*/

#include <stdint.h>
#include <string.h>

typedef unsigned __int128 uint128_t;

static inline uint64_t load64_le( const unsigned char src[8] )
{
    uint64_t w = 0;
    for ( int i = 7; i >= 0; i-- )
    {
        w = ( w << 8 ) | src[i];
    }
    return w;
}

static inline void store64_le( unsigned char dst[8], uint64_t w )
{
    for ( int i = 0; i < 8; i++ )
    {
        dst[i] = (unsigned char) ( w >> ( 8 * i ) );
    }
}

#define SODIUM_ROCKETNET_ARM64 1
#include "../../sodium/local/mul64.h.in"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static uint64_t nondet_uint64( void ) { return 0; }
static unsigned char nondet_uchar( void ) { return 0; }
#else
uint64_t nondet_uint64( void );
unsigned char nondet_uchar( void );
#endif // #ifndef __CPROVER__

void harness_step( void )
{
    uint64_t h0 = nondet_uint64();
    uint64_t h1 = nondet_uint64();
    uint64_t h2 = nondet_uint64();
    const uint64_t hibit = nondet_uint64();
    const uint64_t r0 = nondet_uint64() & 0x0FFFFFFC0FFFFFFFULL;
    const uint64_t r1 = nondet_uint64() & 0x0FFFFFFC0FFFFFFCULL;
    const uint64_t s1 = r1 + ( r1 >> 2 );
    unsigned char m[16];
    for ( int i = 0; i < 16; i++ )
    {
        m[i] = nondet_uchar();
    }
    __CPROVER_assume( h2 <= 4 );
    __CPROVER_assume( hibit <= 1 );

    RN_POLY_STEP( h0, h1, h2, r0, r1, s1, m, hibit );

    __CPROVER_assert( h2 <= 4, "after a block step h2 <= 4, so h < 2p" );
#if defined( REACHABILITY_CONTROL )
    __CPROVER_assert( 0, "poly1305-mul64-step reachability control" );
#endif
}

void harness_finish( void )
{
    const uint64_t h0 = nondet_uint64();
    const uint64_t h1 = nondet_uint64();
    const uint64_t h2 = nondet_uint64();
    const uint64_t pad0 = nondet_uint64();
    const uint64_t pad1 = nondet_uint64();
    __CPROVER_assume( h2 <= 4 );

    // h mod p, as a comparison: h - p = h + 5 - 2^130, whose low 128 bits are
    // those of h + 5
    const int at_least_p = h2 >= 4 || ( h2 == 3 && h1 == UINT64_MAX && h0 >= UINT64_MAX - 4 );
    uint128_t reduced = ( (uint128_t) h1 << 64 ) | h0;
    if ( at_least_p )
    {
        reduced += 5;
    }
    const uint128_t tag = reduced + ( ( (uint128_t) pad1 << 64 ) | pad0 );

    unsigned char out[16];
    rn_mul64_finish( h0, h1, h2, pad0, pad1, out );

    __CPROVER_assert( load64_le( out ) == (uint64_t) tag, "tag low 64 bits are ((h mod p) + pad) mod 2^128" );
    __CPROVER_assert( load64_le( out + 8 ) == (uint64_t) ( tag >> 64 ), "tag high 64 bits are ((h mod p) + pad) mod 2^128" );
#if defined( REACHABILITY_CONTROL )
    __CPROVER_assert( 0, "poly1305-mul64-finish reachability control" );
#endif
}

#if defined( __CPROVER__ ) && defined( CONGRUENCE )
// The attempt at the congruence, kept so the budget can be measured again
// (the poly1305-mul64-congruence target). With r1 = 4 * (r1 / 4) and
// s1 = 5 * (r1 / 4), the product of the summed accumulator S and r is
//   S * r = D + p * (h1 * (r1 / 4) + h2 * (r1 / 4) * 2^64)
// where D is the step's d0 + d1 * 2^64 + d2 * 2^128 before the fold, and the
// fold takes (D >> 130) * p off D. So h' + p * Q = S * r with
// Q = h1 * (r1 / 4) + h2 * (r1 / 4) * 2^64 + (D >> 130), computed here in
// 256-bit words of its own: an equality of products, no division.
typedef unsigned __CPROVER_bitvector[ 256 ] u256;

void harness_congruence( void )
{
    uint64_t h0 = nondet_uint64();
    uint64_t h1 = nondet_uint64();
    uint64_t h2 = nondet_uint64();
    const uint64_t hibit = nondet_uint64();
    const uint64_t r0 = nondet_uint64() & 0x0FFFFFFC0FFFFFFFULL;
    const uint64_t r1 = nondet_uint64() & 0x0FFFFFFC0FFFFFFCULL;
    const uint64_t s1 = r1 + ( r1 >> 2 );
    unsigned char m[16];
    for ( int i = 0; i < 16; i++ )
    {
        m[i] = nondet_uchar();
    }
    __CPROVER_assume( h2 <= 4 );
    __CPROVER_assume( hibit <= 1 );

    const u256 two64 = ( (u256) 1 ) << 64;
    const u256 two128 = ( (u256) 1 ) << 128;
    const u256 p = ( ( (u256) 1 ) << 130 ) - 5;
    const u256 r = (u256) r0 + (u256) r1 * two64;
    const u256 s = (u256) h0 + (u256) h1 * two64 + (u256) h2 * two128 + (u256) load64_le( m ) +
                   (u256) load64_le( m + 8 ) * two64 + (u256) hibit * two128;
    const u256 a0 = s % two64;
    const u256 a1 = ( s >> 64 ) % two64;
    const u256 a2 = s >> 128;
    const u256 d = a0 * r0 + a1 * s1 + ( a0 * r1 + a1 * r0 + a2 * s1 ) * two64 + a2 * r0 * two128;
    const u256 q = a1 * ( r1 >> 2 ) + a2 * ( r1 >> 2 ) * two64 + ( d >> 130 );

    RN_POLY_STEP( h0, h1, h2, r0, r1, s1, m, hibit );

    const u256 out = (u256) h0 + (u256) h1 * two64 + (u256) h2 * two128;
    __CPROVER_assert( s * r == out + p * q, "a block step is (h + m + hibit * 2^128) * r mod p" );
}
#endif // #if defined( __CPROVER__ ) && defined( CONGRUENCE )

int main( void )
{
    harness_step();
    harness_finish();
    return 0;
}

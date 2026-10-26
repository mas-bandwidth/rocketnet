/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC harness for rocketnet_replay_advance at the real window: 4096 bits,
    64 words. The harness includes sdk/rocketnet_replay.cpp below, so it
    proves the sdk itself rather than a copy.

    Specification. After an advance with delta = sequence - highest_opened:
      delta >= 4096 clears every word, then sets the new sequence bit;
      a smaller delta clears exactly the vacated ring bits (the residues of
      highest_opened+1 .. sequence-1) and sets the new sequence bit.

    The near harness is the smaller-delta claim. The quick default target
    admits deltas 1..3 and labels that reduced bound. The explicit
    replay-near-4096 target admits every delta 1..4095 (strictly less than the
    4096-bit window). The production range-clear implementation visits at
    most 64 words, so unwind 65 covers the full declared near range. The far
    harness is the whole-window claim: a delta of at least 4096 clears every
    word, then sets the new sequence bit.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check --unwinding-assertions
    Unwinding: the production near clear loop visits at most 64 words. Unwind
    65 also initializes all 64 symbolic words. The near harness checks one
    arbitrary word, which quantifies all 64 words without a reference loop.
    The far harness has no data-dependent loop; its check runs 64 times
    (unwind 65).
*/

#include <stdint.h>

#ifndef REPLAY_DELTA_BOUND
#define REPLAY_DELTA_BOUND 4096
#endif

#define static_assert _Static_assert
#include "rocketnet_replay.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static uint64_t nondet_uint64( void ) { return 0; }
#else
uint64_t nondet_uint64( void );
#endif

#define ROCKETNET_REPLAY_WINDOW_WORDS ( ROCKETNET_REPLAY_WINDOW_BITS / 64 )

/* Bits [lo, hi] inclusive inside one word. lo and hi are in 0..63. */
static uint64_t bits_from_to( int lo, int hi )
{
    uint64_t high_ones;
    uint64_t low_ones;

    if ( lo > hi )
        return 0;
    high_ones = ( hi == 63 ) ? ~0ULL : ( ( 1ULL << ( hi + 1 ) ) - 1ULL );
    low_ones = ( lo == 0 ) ? ~0ULL : ( ~0ULL << (unsigned) lo );
    return high_ones & low_ones;
}

/* Bits of this word that lie in the inclusive interval [a, b], a <= b. */
static uint64_t mask_intersect( uint64_t a, uint64_t b, int word )
{
    uint64_t w0 = (uint64_t) word * 64ULL;
    uint64_t w1 = w0 + 63ULL;
    uint64_t lo = a > w0 ? a : w0;
    uint64_t hi = b < w1 ? b : w1;

    if ( lo > hi )
        return 0;
    return bits_from_to( (int) ( lo - w0 ), (int) ( hi - w0 ) );
}

static uint64_t reference_near_word( uint64_t old_word, uint64_t old_high,
                                     uint64_t sequence, int word )
{
    uint64_t delta = sequence - old_high;
    uint64_t vacated = 0;
    uint64_t set_at = sequence % ROCKETNET_REPLAY_WINDOW_BITS;

    if ( delta > 1 )
    {
        uint64_t a = ( old_high + 1 ) % ROCKETNET_REPLAY_WINDOW_BITS;
        uint64_t b = ( sequence - 1 ) % ROCKETNET_REPLAY_WINDOW_BITS;

        if ( a <= b )
            vacated = mask_intersect( a, b, word );
        else
            vacated = mask_intersect( a, ROCKETNET_REPLAY_WINDOW_BITS - 1, word ) |
                      mask_intersect( 0, b, word );
    }

    uint64_t result = old_word & ~vacated;
    if ( set_at / 64ULL == (uint64_t) word )
        result |= 1ULL << ( set_at % 64ULL );
    return result;
}

int harness_replay_near( void )
{
    struct rocketnet_replay_protection_t replay;
    uint64_t old_word;
    uint64_t expected_word;
    uint64_t sequence;
    uint64_t old_high;
    uint64_t chosen_word;
    int w;

    sequence = nondet_uint64();
    replay.highest_opened = nondet_uint64();
    for ( w = 0; w < ROCKETNET_REPLAY_WINDOW_WORDS; w++ )
        replay.window[w] = nondet_uint64();

    __CPROVER_assume( replay.highest_opened < UINT64_MAX );
    __CPROVER_assume( sequence > replay.highest_opened );
    __CPROVER_assume( sequence - replay.highest_opened < REPLAY_DELTA_BOUND );
    __CPROVER_assume( REPLAY_DELTA_BOUND <= ROCKETNET_REPLAY_WINDOW_BITS );
    chosen_word = nondet_uint64();
    __CPROVER_assume( chosen_word < ROCKETNET_REPLAY_WINDOW_WORDS );

    old_high = replay.highest_opened;
    old_word = replay.window[chosen_word];

    rocketnet_replay_advance( &replay, sequence );
    expected_word = reference_near_word( old_word, old_high, sequence, (int) chosen_word );

    __CPROVER_assert( replay.highest_opened == sequence,
                      "highest_opened advances to the received sequence" );
    __CPROVER_assert( replay.window[chosen_word] == expected_word,
                      "a smaller delta clears exactly the vacated bits in every chosen word" );
    return 0;
}

int harness_replay_far( void )
{
    struct rocketnet_replay_protection_t replay;
    uint64_t sequence;
    uint64_t set_at;
    int w;

    sequence = nondet_uint64();
    replay.highest_opened = nondet_uint64();
    for ( w = 0; w < ROCKETNET_REPLAY_WINDOW_WORDS; w++ )
        replay.window[w] = nondet_uint64();

    __CPROVER_assume( replay.highest_opened <= UINT64_MAX - ROCKETNET_REPLAY_WINDOW_BITS );
    __CPROVER_assume( sequence > replay.highest_opened );
    __CPROVER_assume( sequence - replay.highest_opened >= ROCKETNET_REPLAY_WINDOW_BITS );

    rocketnet_replay_advance( &replay, sequence );

    set_at = sequence % ROCKETNET_REPLAY_WINDOW_BITS;
    __CPROVER_assert( replay.highest_opened == sequence,
                      "highest_opened advances on a far delta" );
    for ( w = 0; w < ROCKETNET_REPLAY_WINDOW_WORDS; w++ )
    {
        uint64_t word = 0;
        if ( set_at / 64ULL == (uint64_t) w )
            word = 1ULL << ( set_at % 64ULL );
        __CPROVER_assert( replay.window[w] == word,
                          "a delta at least the width clears every word" );
    }
    return 0;
}

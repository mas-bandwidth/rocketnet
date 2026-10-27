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
    4096-bit window), partitioned across 13 disjoint and exhaustive cases by
    the production range-clear loop shape and the arbitrary selected word's
    relation to the cleared word span(s) and the newly set word (including
    coincident boundary cases). `harness_replay_near_partition_completeness`
    machine-checks in CBMC that the 13 partition predicates are pairwise
    disjoint and jointly exhaustive over the full unconstrained near-window
    domain and that each partition's concrete reachability witness lands in its
    named partition. The production implementation visits at most 64 words, so
    unwind 65 covers every path. The far harness is the whole-window claim: a
    delta of at least 4096 clears every word, then sets the new bit.

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

/*
    Disjoint and exhaustive partition of the full near-window domain
    (1 <= sequence - highest_opened < 4096, 0 <= chosen_word < 64) by
    production range-clear loop shape and the arbitrary selected word's
    relation to the cleared word span(s) and `set_word`:

      Nonwrapping (end <= 4096):
        0: nw_empty_clear
           begin == end (delta == 1); range-clear returns at `begin >= end`.
        1: nw_single_partial_cleared_set
           end > begin, single partial word (first_word == last_word and
           end - begin < 64), chosen_word == first_word and chosen_word == set_word.
        2: nw_single_partial_cleared_only
           end > begin, single partial word, chosen_word == first_word and
           chosen_word != set_word.
        3: nw_single_partial_set_only
           end > begin, single partial word, chosen_word != first_word and
           chosen_word == set_word.
        4: nw_single_partial_untouched
           end > begin, single partial word, chosen_word != first_word and
           chosen_word != set_word.
        5: nw_multi_first_word
           end > begin, multi-word or full-word clear (!single_partial),
           chosen_word == first_word.
        6: nw_multi_interior_word
           end > begin, !single_partial, first_word < chosen_word < last_word.
        7: nw_multi_last_word
           end > begin, !single_partial, chosen_word > first_word and
           chosen_word == last_word.
        8: nw_multi_outside_word
           end > begin, !single_partial, chosen_word < first_word or
           chosen_word > last_word.

      Wrapping (end > 4096, upper span [first_word, 63] and lower span
      [0, wrap_last_word] where wrap_last_word = (end - 4096 - 1) / 64):
        9: wrap_coincident_word
           chosen_word >= first_word and chosen_word <= wrap_last_word
           (both range-clear calls touch word 0 when first_word == 0).
       10: wrap_high_span_only
           chosen_word >= first_word and chosen_word > wrap_last_word.
       11: wrap_low_span_only
           chosen_word < first_word and chosen_word <= wrap_last_word.
       12: wrap_outside_spans
           chosen_word < first_word and chosen_word > wrap_last_word.
*/
#define REPLAY_NEAR_PARTITION_COUNT 13

static int replay_near_partition_matches( int part, int begin, int end,
                                          int set_word, int chosen )
{
    int first_word = begin / 64;

    if ( end <= ROCKETNET_REPLAY_WINDOW_BITS )
    {
        if ( begin == end )
            return part == 0;

        int last_word = ( end - 1 ) / 64;
        int single_partial = ( first_word == last_word ) && ( ( end - begin ) < 64 );

        if ( single_partial )
        {
            if ( chosen == first_word && chosen == set_word )
                return part == 1;
            if ( chosen == first_word && chosen != set_word )
                return part == 2;
            if ( chosen != first_word && chosen == set_word )
                return part == 3;
            if ( chosen != first_word && chosen != set_word )
                return part == 4;
            return 0;
        }

        if ( chosen == first_word )
            return part == 5;
        if ( chosen > first_word && chosen < last_word )
            return part == 6;
        if ( chosen > first_word && chosen == last_word )
            return part == 7;
        if ( chosen < first_word || chosen > last_word )
            return part == 8;
        return 0;
    }
    else
    {
        int wrap_last_word = ( end - ROCKETNET_REPLAY_WINDOW_BITS - 1 ) / 64;

        if ( chosen >= first_word && chosen <= wrap_last_word )
            return part == 9;
        if ( chosen >= first_word && chosen > wrap_last_word )
            return part == 10;
        if ( chosen < first_word && chosen <= wrap_last_word )
            return part == 11;
        if ( chosen < first_word && chosen > wrap_last_word )
            return part == 12;
        return 0;
    }
}

static void replay_near_partition_witness( int part, uint64_t *highest_opened,
                                           uint64_t *sequence,
                                           uint64_t *chosen_word )
{
    switch ( part )
    {
    case 0: /* nw_empty_clear: begin=2, end=2 */
        *highest_opened = 1;
        *sequence = 2;
        *chosen_word = 0;
        break;
    case 1: /* nw_single_partial_cleared_set: begin=1, end=2, first=0, set=0 */
        *highest_opened = 0;
        *sequence = 2;
        *chosen_word = 0;
        break;
    case 2: /* nw_single_partial_cleared_only: begin=62, end=64, first=0, set=1 */
        *highest_opened = 61;
        *sequence = 64;
        *chosen_word = 0;
        break;
    case 3: /* nw_single_partial_set_only: begin=62, end=64, first=0, set=1 */
        *highest_opened = 61;
        *sequence = 64;
        *chosen_word = 1;
        break;
    case 4: /* nw_single_partial_untouched: begin=1, end=2, first=0, set=0 */
        *highest_opened = 0;
        *sequence = 2;
        *chosen_word = 1;
        break;
    case 5: /* nw_multi_first_word: begin=1, end=66, first=0, last=1 */
        *highest_opened = 0;
        *sequence = 66;
        *chosen_word = 0;
        break;
    case 6: /* nw_multi_interior_word: begin=1, end=130, first=0, last=2 */
        *highest_opened = 0;
        *sequence = 130;
        *chosen_word = 1;
        break;
    case 7: /* nw_multi_last_word: begin=1, end=66, first=0, last=1 */
        *highest_opened = 0;
        *sequence = 66;
        *chosen_word = 1;
        break;
    case 8: /* nw_multi_outside_word: begin=1, end=66, first=0, last=1 */
        *highest_opened = 0;
        *sequence = 66;
        *chosen_word = 2;
        break;
    case 9: /* wrap_coincident_word: begin=63, delta=4095, end=4157, first=0, wrap_last=0 */
        *highest_opened = 62;
        *sequence = 4157;
        *chosen_word = 0;
        break;
    case 10: /* wrap_high_span_only: begin=4095, end=4097, first=63, wrap_last=0 */
        *highest_opened = 4094;
        *sequence = 4097;
        *chosen_word = 63;
        break;
    case 11: /* wrap_low_span_only: begin=4095, end=4097, first=63, wrap_last=0 */
        *highest_opened = 4094;
        *sequence = 4097;
        *chosen_word = 0;
        break;
    default: /* 12: wrap_outside_spans: begin=4095, end=4097, first=63, wrap_last=0 */
        *highest_opened = 4094;
        *sequence = 4097;
        *chosen_word = 1;
        break;
    }
}

int harness_replay_near_partition_completeness( void )
{
    uint64_t highest_opened = nondet_uint64();
    uint64_t sequence = nondet_uint64();
    uint64_t chosen_word = nondet_uint64();
    uint64_t delta;
    int begin;
    int end;
    int set_word;
    int matches;

    __CPROVER_assume( highest_opened < UINT64_MAX );
    __CPROVER_assume( sequence > highest_opened );
    __CPROVER_assume( sequence - highest_opened < REPLAY_DELTA_BOUND );
    __CPROVER_assume( REPLAY_DELTA_BOUND <= ROCKETNET_REPLAY_WINDOW_BITS );
    __CPROVER_assume( chosen_word < ROCKETNET_REPLAY_WINDOW_WORDS );

    delta = sequence - highest_opened;
    begin = (int) ( ( highest_opened + 1 ) % ROCKETNET_REPLAY_WINDOW_BITS );
    end = begin + (int) delta - 1;
    set_word = (int) ( ( sequence % ROCKETNET_REPLAY_WINDOW_BITS ) / 64ULL );

    matches =
        replay_near_partition_matches( 0, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 1, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 2, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 3, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 4, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 5, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 6, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 7, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 8, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 9, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 10, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 11, begin, end, set_word, (int) chosen_word ) +
        replay_near_partition_matches( 12, begin, end, set_word, (int) chosen_word );

    __CPROVER_assert( matches >= 1,
                      "near-window partitions are jointly exhaustive over the full domain" );
    __CPROVER_assert( matches <= 1,
                      "near-window partitions are pairwise disjoint over the full domain" );

    uint64_t part_id = nondet_uint64();
    __CPROVER_assume( part_id < REPLAY_NEAR_PARTITION_COUNT );
    uint64_t w_high = 0;
    uint64_t w_seq = 0;
    uint64_t w_word = 0;
    replay_near_partition_witness( (int) part_id, &w_high, &w_seq, &w_word );
    __CPROVER_assert( w_high < UINT64_MAX && w_seq > w_high &&
                      ( w_seq - w_high ) < ROCKETNET_REPLAY_WINDOW_BITS &&
                      w_word < ROCKETNET_REPLAY_WINDOW_WORDS,
                      "each partition reachability witness lies inside the full near-window domain" );
    int w_begin = (int) ( ( w_high + 1 ) % ROCKETNET_REPLAY_WINDOW_BITS );
    int w_end = w_begin + (int) ( w_seq - w_high ) - 1;
    int w_set = (int) ( ( w_seq % ROCKETNET_REPLAY_WINDOW_BITS ) / 64ULL );
    __CPROVER_assert(
        replay_near_partition_matches( (int) part_id, w_begin, w_end, w_set, (int) w_word ) == 1,
        "each partition reachability witness satisfies its own partition predicate" );
    return 0;
}

static int harness_replay_near_case( int mode )
{
    struct rocketnet_replay_protection_t replay;
    uint64_t old_word;
    uint64_t expected_word;
    uint64_t sequence;
    uint64_t old_high;
    uint64_t delta;
    uint64_t chosen_word;
    int begin;
    int end;
    int set_word;
    int w;

#if defined(REPLAY_REACHABILITY_WITNESS)
    if ( mode >= 0 && mode < REPLAY_NEAR_PARTITION_COUNT )
    {
        replay_near_partition_witness( mode, &replay.highest_opened, &sequence,
                                       &chosen_word );
    }
    else if ( mode == -3 )
    {
        replay.highest_opened = 4094;
        sequence = 4097;
        chosen_word = 0;
    }
    else
    {
        replay.highest_opened = 1;
        sequence = 3;
        chosen_word = 0;
    }
#else
    sequence = nondet_uint64();
    replay.highest_opened = nondet_uint64();
    chosen_word = nondet_uint64();
#endif
    for ( w = 0; w < ROCKETNET_REPLAY_WINDOW_WORDS; w++ )
        replay.window[w] = nondet_uint64();

    __CPROVER_assume( replay.highest_opened < UINT64_MAX );
    __CPROVER_assume( sequence > replay.highest_opened );
    __CPROVER_assume( sequence - replay.highest_opened < REPLAY_DELTA_BOUND );
    __CPROVER_assume( REPLAY_DELTA_BOUND <= ROCKETNET_REPLAY_WINDOW_BITS );
    __CPROVER_assume( chosen_word < ROCKETNET_REPLAY_WINDOW_WORDS );

    old_high = replay.highest_opened;
    delta = sequence - old_high;
    begin = (int) ( ( old_high + 1 ) % ROCKETNET_REPLAY_WINDOW_BITS );
    end = begin + (int) delta - 1;
    set_word = (int) ( ( sequence % ROCKETNET_REPLAY_WINDOW_BITS ) / 64ULL );

    if ( mode >= 0 && mode < REPLAY_NEAR_PARTITION_COUNT )
    {
        __CPROVER_assume( replay_near_partition_matches(
            mode, begin, end, set_word, (int) chosen_word ) );
    }
    else if ( mode == -3 )
    {
        __CPROVER_assume( end > ROCKETNET_REPLAY_WINDOW_BITS );
    }
    else if ( mode == -2 )
    {
        __CPROVER_assume( end <= ROCKETNET_REPLAY_WINDOW_BITS );
    }
    old_word = replay.window[chosen_word];

    rocketnet_replay_advance( &replay, sequence );
    expected_word = reference_near_word( old_word, old_high, sequence, (int) chosen_word );

    __CPROVER_assert( replay.highest_opened == sequence,
                      "highest_opened advances to the received sequence" );
    __CPROVER_assert( replay.window[chosen_word] == expected_word,
                      "a smaller delta clears exactly the vacated bits in every chosen word" );
#if defined(REPLAY_REACHABILITY_WITNESS)
    __CPROVER_assert( 0, "replay path reachability negative control" );
#endif
    return 0;
}

int harness_replay_near( void )
{
    return harness_replay_near_case( -1 );
}

int harness_replay_near_nonwrapping( void )
{
    return harness_replay_near_case( -2 );
}

int harness_replay_near_wrapping( void )
{
    return harness_replay_near_case( -3 );
}

int harness_replay_near_nw_empty_clear( void ) { return harness_replay_near_case( 0 ); }
int harness_replay_near_nw_single_partial_cleared_set( void ) { return harness_replay_near_case( 1 ); }
int harness_replay_near_nw_single_partial_cleared_only( void ) { return harness_replay_near_case( 2 ); }
int harness_replay_near_nw_single_partial_set_only( void ) { return harness_replay_near_case( 3 ); }
int harness_replay_near_nw_single_partial_untouched( void ) { return harness_replay_near_case( 4 ); }
int harness_replay_near_nw_multi_first_word( void ) { return harness_replay_near_case( 5 ); }
int harness_replay_near_nw_multi_interior_word( void ) { return harness_replay_near_case( 6 ); }
int harness_replay_near_nw_multi_last_word( void ) { return harness_replay_near_case( 7 ); }
int harness_replay_near_nw_multi_outside_word( void ) { return harness_replay_near_case( 8 ); }
int harness_replay_near_wrap_coincident_word( void ) { return harness_replay_near_case( 9 ); }
int harness_replay_near_wrap_high_span_only( void ) { return harness_replay_near_case( 10 ); }
int harness_replay_near_wrap_low_span_only( void ) { return harness_replay_near_case( 11 ); }
int harness_replay_near_wrap_outside_spans( void ) { return harness_replay_near_case( 12 ); }

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
#if defined(REPLAY_FAR_REACHABILITY)
    __CPROVER_assert( 0, "replay-far reachability control" );
#endif
    return 0;
}

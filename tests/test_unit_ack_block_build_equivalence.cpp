/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The ack block builder reads the replay window a 64-bit word at a time
// (sdk/rocketnet_ack.cpp rocketnet_ack_block_build). The block it writes is
// the wire statement of the received set, so the word scan must write the
// same bytes the bit-by-bit walk wrote before it. This file keeps a copy of
// that walk as the reference and compares the two over fixed-seed random
// windows and every edge shape: sparse loss, dense loss, alternating bits, a
// full window, an empty window, a window wrapped across the 4096-bit
// boundary, a largest below 4096, a largest near the top of the 64-bit range,
// and every capacity from the minimum block to past the range cap.
// No real time, sockets or threads: a unit case.

#include "test_unit_registry.h"

#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_read_write.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define ACK_BUILD_EQUIVALENCE_RANDOM_WINDOWS 100000

// -----------------------------------------
//    the reference: the bit-by-bit walk the
//    builder used before the word scan
// -----------------------------------------

static int reference_replay_bit( const struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    int bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
    return ( replay->window[bit / 64] >> ( bit % 64 ) ) & 1;
}

static int reference_ack_block_build( uint8_t * output, int capacity, const struct rocketnet_replay_protection_t * replay )
{
    struct rocketnet_ack_block_t block;
    block.largest = replay->highest_opened;
    block.first_range_length = 0;
    block.num_ranges = 0;

    if ( block.largest != 0 )
    {
        uint64_t floor_sequence = 1;
        if ( block.largest >= ROCKETNET_REPLAY_WINDOW_BITS )
            floor_sequence = block.largest - ROCKETNET_REPLAY_WINDOW_BITS + 1;

        int max_ranges = ( capacity - ROCKETNET_ACK_BLOCK_MIN_BYTES ) / 4;
        if ( max_ranges > ROCKETNET_ACK_BLOCK_MAX_RANGES )
            max_ranges = ROCKETNET_ACK_BLOCK_MAX_RANGES;

        uint64_t s = block.largest;

        uint32_t first_run = 0;
        while ( s >= floor_sequence && reference_replay_bit( replay, s ) && first_run < 0xFFFF )
        {
            first_run++;
            if ( s == 1 )
                break;
            s--;
        }
        block.first_range_length = first_run;

        uint64_t next_high = block.largest - first_run;

        while ( next_high >= floor_sequence && block.num_ranges < max_ranges )
        {
            uint32_t gap = 0;
            while ( next_high >= floor_sequence && !reference_replay_bit( replay, next_high ) && gap < 0xFFFF )
            {
                gap++;
                if ( next_high == 1 )
                    break;
                next_high--;
            }

            if ( next_high < floor_sequence || reference_replay_bit( replay, next_high ) == 0 )
                break;

            uint32_t run = 0;
            while ( next_high >= floor_sequence && reference_replay_bit( replay, next_high ) && run < 0xFFFF )
            {
                run++;
                if ( next_high == 1 )
                {
                    next_high = 0;
                    break;
                }
                next_high--;
            }

            if ( run == 0 )
                break;

            block.range_gap[block.num_ranges] = (uint16_t) gap;
            block.range_length[block.num_ranges] = (uint16_t) run;
            block.num_ranges++;

            if ( next_high == 0 )
                break;
        }
    }

    return rocketnet_ack_block_write( output, &block );
}

// -----------------------------------------
//    fixed-seed windows
// -----------------------------------------

static uint64_t equivalence_random_state = 0x9E3779B97F4A7C15ULL;

static uint64_t equivalence_random()
{
    equivalence_random_state ^= equivalence_random_state << 13;
    equivalence_random_state ^= equivalence_random_state >> 7;
    equivalence_random_state ^= equivalence_random_state << 17;
    return equivalence_random_state;
}

static void window_set( struct rocketnet_replay_protection_t * replay, uint64_t sequence, int value )
{
    const int bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
    const uint64_t mask = 1ULL << ( bit % 64 );
    if ( value )
        replay->window[bit / 64] |= mask;
    else
        replay->window[bit / 64] &= ~mask;
}

// every capacity from the minimum block to past the range cap: the largest
// first, then each count of ranges a capacity can hold

static const int equivalence_capacities[] =
{
    ROCKETNET_ACK_BLOCK_MAX_BYTES + 64,
    ROCKETNET_ACK_BLOCK_MAX_BYTES,
    ROCKETNET_ACK_BLOCK_MIN_BYTES,
    ROCKETNET_ACK_BLOCK_MIN_BYTES + 3,
    ROCKETNET_ACK_BLOCK_MIN_BYTES + 4,
    ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * 2 + 1,
    ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * 17,
    ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * 100,
};

static int compare_one( const struct rocketnet_replay_protection_t * replay, int capacity )
{
    uint8_t expected[ROCKETNET_ACK_BLOCK_MAX_BYTES + 64];
    uint8_t actual[ROCKETNET_ACK_BLOCK_MAX_BYTES + 64];
    memset( expected, 0xA5, sizeof( expected ) );
    memset( actual, 0xA5, sizeof( actual ) );
    const int expected_bytes = reference_ack_block_build( expected, capacity, replay );
    const int actual_bytes = rocketnet_ack_block_build( actual, capacity, replay );
    if ( expected_bytes != actual_bytes || memcmp( expected, actual, sizeof( expected ) ) != 0 )
    {
        printf( "ack block build equivalence: largest %llu capacity %d: word scan wrote %d bytes, bit walk wrote %d bytes\n",
                (unsigned long long) replay->highest_opened, capacity, actual_bytes, expected_bytes );
        return 0;
    }
    return 1;
}

static int compare_all_capacities( const struct rocketnet_replay_protection_t * replay )
{
    int same = 1;
    for ( size_t i = 0; i < sizeof( equivalence_capacities ) / sizeof( equivalence_capacities[0] ); i++ )
        same &= compare_one( replay, equivalence_capacities[i] );
    return same;
}

// a largest from one of the regions the builder treats differently: below
// the window (floor 1), just either side of a multiple of the window (the
// circular wrap at bit 0), either side of the wire's 32-bit wrap, a random
// 48-bit value, and the top of the 64-bit range

static uint64_t random_largest()
{
    const uint64_t window = ROCKETNET_REPLAY_WINDOW_BITS;
    switch ( equivalence_random() % 6 )
    {
        case 0:  return 1 + equivalence_random() % ( window + 200 );
        case 1:  return window * ( 1 + equivalence_random() % 1000 ) + ( equivalence_random() % 257 ) - 128;
        case 2:  return 0x100000000ULL + ( equivalence_random() % 20000 ) - 10000;
        case 3:  return 1 + ( equivalence_random() >> 16 );
        case 4:  return 0xFFFFFFFFFFFFFFFFULL - equivalence_random() % 10000;
        default: return 1 + equivalence_random() % 64;
    }
}

// random bits across the whole window (inside and outside the enumerable
// range, so the masks at the floor and at largest are exercised), either as
// independent bits at one of several loss rates or as runs of random length

static void random_window( struct rocketnet_replay_protection_t * replay )
{
    const int words = ROCKETNET_REPLAY_WINDOW_BITS / 64;
    const int kind = (int) ( equivalence_random() % 8 );
    if ( kind < 5 )
    {
        static const int shift[5] = { 6, 3, 1, 3, 6 };     // loss (or receipt) at 1/64, 1/8, 1/2
        for ( int w = 0; w < words; w++ )
        {
            uint64_t word = ~0ULL;
            for ( int k = 0; k < shift[kind]; k++ )
                word &= equivalence_random();
            replay->window[w] = kind < 2 ? word : ( kind == 2 ? equivalence_random() : ~word );
        }
    }
    else
    {
        static const uint64_t longest[3] = { 3, 90, 5000 };
        const uint64_t longest_run = longest[kind - 5];
        uint64_t sequence = 0;
        int value = (int) ( equivalence_random() & 1 );
        while ( sequence < ROCKETNET_REPLAY_WINDOW_BITS )
        {
            const uint64_t length = 1 + equivalence_random() % longest_run;
            for ( uint64_t i = 0; i < length && sequence < ROCKETNET_REPLAY_WINDOW_BITS; i++ )
                window_set( replay, sequence++, value );
            value = !value;
        }
    }
}

// one of the named edge shapes at a given largest; the largest is always
// marked received, as rocketnet_replay_advance leaves it

enum edge_shape_t
{
    EDGE_SPARSE_LOSS,
    EDGE_DENSE_LOSS,
    EDGE_ALTERNATING,
    EDGE_FULL,
    EDGE_EMPTY,
    EDGE_ALTERNATING_PAIRS,
    EDGE_COUNT
};

static void edge_window( struct rocketnet_replay_protection_t * replay, int shape, uint64_t largest )
{
    replay->highest_opened = largest;
    const int words = ROCKETNET_REPLAY_WINDOW_BITS / 64;
    for ( int w = 0; w < words; w++ )
    {
        uint64_t word = 0;
        switch ( shape )
        {
            case EDGE_SPARSE_LOSS:       word = ~( ( 1ULL << ( ( w * 7 ) % 64 ) ) | ( w % 3 == 0 ? 1ULL << 40 : 0 ) ); break;
            case EDGE_DENSE_LOSS:        word = ( 1ULL << ( ( w * 11 ) % 64 ) ) | ( w % 2 == 0 ? 1ULL << 63 : 0 ); break;
            case EDGE_ALTERNATING:       word = 0x5555555555555555ULL; break;
            case EDGE_FULL:              word = ~0ULL; break;
            case EDGE_EMPTY:             word = 0; break;
            case EDGE_ALTERNATING_PAIRS: word = 0x3333333333333333ULL; break;
            default:                     break;
        }
        replay->window[w] = word;
    }
    window_set( replay, largest, 1 );
}

// -----------------------------------------
//    the case
// -----------------------------------------

static void test_ack_block_build_word_scan_matches_bit_walk()
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    // nothing opened: the empty block

    check( compare_all_capacities( &replay ) );

    // every edge shape at every edge largest: just above zero, below the
    // window, at and either side of each multiple of the window and of each
    // word, across the 32-bit wrap, and at the top of the 64-bit range

    const uint64_t window = ROCKETNET_REPLAY_WINDOW_BITS;
    const uint64_t edge_largest[] =
    {
        1, 2, 3, 63, 64, 65, 127, 128, 129, 1000,
        window - 65, window - 64, window - 63, window - 2, window - 1,
        window, window + 1, window + 2, window + 63, window + 64, window + 65,
        2 * window - 1, 2 * window, 2 * window + 1, 2 * window + 37,
        7 * window + 4031, 7 * window + 4032, 7 * window + 4033,
        0xFFFFFFFFULL, 0x100000000ULL, 0x100000001ULL,
        0xFFFFFFFFFFFFFFFFULL - window, 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL,
    };

    int edge_cases = 0;
    for ( int shape = 0; shape < EDGE_COUNT; shape++ )
    {
        for ( size_t i = 0; i < sizeof( edge_largest ) / sizeof( edge_largest[0] ); i++ )
        {
            edge_window( &replay, shape, edge_largest[i] );
            check( compare_all_capacities( &replay ) );
            edge_cases++;
        }
    }

    // a window full except for one hole at every offset from largest, and
    // empty except for one received sequence at every offset: each word
    // boundary and the circular wrap fall at every position of the scan

    const uint64_t hole_largest[] = { 100, window - 1, window + 17, 5 * window + 4000 };
    for ( size_t i = 0; i < sizeof( hole_largest ) / sizeof( hole_largest[0] ); i++ )
    {
        const uint64_t largest = hole_largest[i];
        for ( uint64_t offset = 1; offset < window && offset < largest; offset++ )
        {
            edge_window( &replay, EDGE_FULL, largest );
            window_set( &replay, largest - offset, 0 );
            check( compare_one( &replay, ROCKETNET_ACK_BLOCK_MAX_BYTES ) );

            edge_window( &replay, EDGE_EMPTY, largest );
            window_set( &replay, largest - offset, 1 );
            check( compare_one( &replay, ROCKETNET_ACK_BLOCK_MAX_BYTES ) );
            edge_cases += 2;
        }
    }

    // fixed-seed random windows

    equivalence_random_state = 0x9E3779B97F4A7C15ULL;
    int random_cases = 0;
    for ( int n = 0; n < ACK_BUILD_EQUIVALENCE_RANDOM_WINDOWS; n++ )
    {
        random_window( &replay );
        replay.highest_opened = random_largest();
        window_set( &replay, replay.highest_opened, 1 );
        const int capacity = equivalence_capacities[equivalence_random() % ( sizeof( equivalence_capacities ) / sizeof( equivalence_capacities[0] ) )];
        check( compare_one( &replay, capacity ) );
        random_cases++;
    }

    check( random_cases == ACK_BUILD_EQUIVALENCE_RANDOM_WINDOWS );
    printf( "ack block build equivalence: %d edge windows and %d random windows write identical blocks\n", edge_cases, random_cases );
}

REGISTER_UNIT_TEST( test_ack_block_build_word_scan_matches_bit_walk );

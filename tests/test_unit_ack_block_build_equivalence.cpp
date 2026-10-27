/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The word-level ack block builder must emit byte-identical blocks to the
// original bit-by-bit walk. This case keeps a copy of that base walk and
// compares the two over fixed-seed randomized replay windows plus every
// edge shape the bench names. No real time, sockets or threads.

#include "test_unit_registry.h"

#include "rocketnet_ack.h"
#include "rocketnet_replay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// ---- the base bit-by-bit builder, kept verbatim in shape from base ----

static int base_replay_bit( const struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    int bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
    return ( replay->window[bit / 64] >> ( bit % 64 ) ) & 1;
}

static int base_ack_block_build_bit_walk( uint8_t * output, int capacity, const struct rocketnet_replay_protection_t * replay )
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
        while ( s >= floor_sequence && base_replay_bit( replay, s ) && first_run < 0xFFFF )
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
            while ( next_high >= floor_sequence && !base_replay_bit( replay, next_high ) && gap < 0xFFFF )
            {
                gap++;
                if ( next_high == 1 )
                    break;
                next_high--;
            }

            if ( next_high < floor_sequence || base_replay_bit( replay, next_high ) == 0 )
                break;

            uint32_t run = 0;
            while ( next_high >= floor_sequence && base_replay_bit( replay, next_high ) && run < 0xFFFF )
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

// ---- fixed-seed generation ----

static uint64_t equiv_rng_state;

static uint64_t equiv_random()
{
    uint64_t z = ( equiv_rng_state += 0x9E3779B97F4A7C15ull );
    z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
    z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
    return z ^ ( z >> 31 );
}

static void equiv_set_bit( struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    const uint64_t bit = sequence % ROCKETNET_REPLAY_WINDOW_BITS;
    replay->window[bit / 64] |= 1ull << ( bit % 64 );
}

static void equiv_compare( const struct rocketnet_replay_protection_t * replay, int capacity )
{
    uint8_t expected[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    uint8_t actual[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    const int expected_bytes = base_ack_block_build_bit_walk( expected, capacity, replay );
    const int actual_bytes = rocketnet_ack_block_build( actual, capacity, replay );
    if ( expected_bytes != actual_bytes || memcmp( expected, actual, (size_t) expected_bytes ) != 0 )
        printf( "ack block equivalence: mismatch highest=%llu capacity=%d expected=%d actual=%d\n",
                (unsigned long long) replay->highest_opened, capacity, expected_bytes, actual_bytes );
    check( expected_bytes == actual_bytes );
    check( memcmp( expected, actual, (size_t) expected_bytes ) == 0 );
}

static void equiv_fill_pattern( struct rocketnet_replay_protection_t * replay, uint64_t highest, int percent_received, int alternating, uint64_t seed )
{
    memset( replay, 0, sizeof( *replay ) );
    replay->highest_opened = highest;
    if ( highest == 0 )
        return;
    uint64_t floor_sequence = 1;
    if ( highest >= ROCKETNET_REPLAY_WINDOW_BITS )
        floor_sequence = highest - ROCKETNET_REPLAY_WINDOW_BITS + 1;
    equiv_rng_state = seed;
    for ( uint64_t s = floor_sequence; s <= highest; s++ )
    {
        int received;
        if ( alternating )
            received = ( ( s & 1 ) == ( highest & 1 ) );
        else if ( percent_received >= 100 )
            received = 1;
        else
            received = ( equiv_random() % 100 ) < (uint64_t) percent_received;
        if ( received )
            equiv_set_bit( replay, s );
    }
    equiv_set_bit( replay, highest );
}

static void test_ack_block_build_word_scan_matches_bit_walk()
{
    struct rocketnet_replay_protection_t replay;

    // every edge shape the bench names, at full and truncated capacities
    const int capacities[] = { ROCKETNET_ACK_BLOCK_MIN_BYTES, ROCKETNET_ACK_BLOCK_MIN_BYTES + 4,
                               ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * 8, ROCKETNET_ACK_BLOCK_MAX_BYTES };
    for ( int c = 0; c < 4; c++ )
    {
        equiv_fill_pattern( &replay, 20000, 98, 0, 0xA11CE001ull );  equiv_compare( &replay, capacities[c] ); // sparse loss
        equiv_fill_pattern( &replay, 20000, 20, 0, 0xA11CE002ull );  equiv_compare( &replay, capacities[c] ); // dense loss
        equiv_fill_pattern( &replay, 20000, 0, 1, 0xA11CE003ull );   equiv_compare( &replay, capacities[c] ); // alternating
        equiv_fill_pattern( &replay, 20000, 100, 0, 0xA11CE004ull ); equiv_compare( &replay, capacities[c] ); // full window
        equiv_fill_pattern( &replay, 0, 0, 0, 0xA11CE005ull );       equiv_compare( &replay, capacities[c] ); // empty window
        equiv_fill_pattern( &replay, 4223, 50, 0, 0xA11CE006ull );   equiv_compare( &replay, capacities[c] ); // wrapped across 4096
        equiv_fill_pattern( &replay, 2000, 50, 0, 0xA11CE007ull );   equiv_compare( &replay, capacities[c] ); // largest below 4096
    }

    // boundary largests and single-bit windows
    const uint64_t largests[] = { 1, 2, 63, 64, 65, 127, 128, 4095, 4096, 4097, 8192, 0x100000004ull };
    for ( size_t i = 0; i < sizeof( largests ) / sizeof( largests[0] ); i++ )
    {
        equiv_fill_pattern( &replay, largests[i], 50, 0, 0xB0000000ull + i );
        equiv_compare( &replay, ROCKETNET_ACK_BLOCK_MAX_BYTES );
        equiv_fill_pattern( &replay, largests[i], 100, 0, 0xB1000000ull + i );
        equiv_compare( &replay, ROCKETNET_ACK_BLOCK_MAX_BYTES );
        // only largest received: the rest is one trailing gap, dropped
        memset( &replay, 0, sizeof( replay ) );
        replay.highest_opened = largests[i];
        equiv_set_bit( &replay, largests[i] );
        equiv_compare( &replay, ROCKETNET_ACK_BLOCK_MAX_BYTES );
    }

    // at least 100000 fixed-seed randomized windows
    equiv_rng_state = 0xE0012026ull;
    for ( int iter = 0; iter < 100000; iter++ )
    {
        memset( &replay, 0, sizeof( replay ) );
        uint64_t highest;
        switch ( iter % 4 )
        {
            case 0:  highest = 1 + equiv_random() % 200; break;
            case 1:  highest = 1 + equiv_random() % 5000; break;
            case 2:  highest = 1 + equiv_random() % 100000; break;
            default: highest = ( equiv_random() % 4 ) * 0x100000000ull + 1 + equiv_random() % 5000; break;
        }
        replay.highest_opened = highest;
        for ( size_t w = 0; w < sizeof( replay.window ) / sizeof( replay.window[0] ); w++ )
        {
            uint64_t word = equiv_random();
            // vary density by iteration so sparse and dense windows both occur
            if ( ( iter % 3 ) == 0 ) word &= equiv_random();
            if ( ( iter % 3 ) == 1 ) word |= equiv_random();
            replay.window[w] = word;
        }
        equiv_set_bit( &replay, highest );
        const int capacity = ( iter % 16 == 0 ) ? ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * ( iter % 64 )
                                                : ROCKETNET_ACK_BLOCK_MAX_BYTES;
        equiv_compare( &replay, capacity );
    }
}

REGISTER_UNIT_TEST( test_ack_block_build_word_scan_matches_bit_walk );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The ack block's largest received across the wire's 32-bit wrap
// (standard/TRANSPORT.md t15; proofs/tla/AckBlockWrap.tla).
//
// The empty block -- wire largest 0, first range length 0, range count 0 --
// is recognised by its wire fields at any highest sent session sequence of
// the reader. The rule before 2026-10-24 made the empty test on the
// reconstructed largest, so a reader whose highest sent had reached 2^31
// reconstructed the wire 0 to 2^32 and refused the block
// (proofs/tla/MCAckBlockWrapBrokenHalfRange.cfg). Every other block reconstructs its
// largest by the nearest-to-highest-sent rule, so a genuine largest whose low
// 32 bits are zero (2^32) still reads nonempty, because a block naming a
// sequence always has a first range length of at least one.

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

#define ACK_BLOCK_WRAP_2_31 0x80000000ULL
#define ACK_BLOCK_WRAP_2_32 0x100000000ULL
#define ACK_BLOCK_WRAP_2_33 0x200000000ULL

// the all-zero 7-byte block reads empty whatever the reader's highest sent

static void test_ack_block_empty_read_across_wrap()
{
    const uint64_t highest_sent[] = {
        0,
        1,
        ACK_BLOCK_WRAP_2_31 - 1,
        ACK_BLOCK_WRAP_2_31,
        ACK_BLOCK_WRAP_2_32 - 10,
        ACK_BLOCK_WRAP_2_32,
        ACK_BLOCK_WRAP_2_32 + 5,
        ACK_BLOCK_WRAP_2_33 + 7,
    };

    uint8_t data[ROCKETNET_ACK_BLOCK_MIN_BYTES];
    memset( data, 0, sizeof( data ) );

    for ( size_t i = 0; i < sizeof( highest_sent ) / sizeof( highest_sent[0] ); i++ )
    {
        struct rocketnet_ack_block_t block;
        memset( &block, 0xA5, sizeof( block ) );

        const int bytes = rocketnet_ack_block_read( &block, data, (int) sizeof( data ), highest_sent[i] );
        if ( bytes != ROCKETNET_ACK_BLOCK_MIN_BYTES )
        {
            printf( "ack block wrap: empty block refused at highest sent %llu\n", (unsigned long long) highest_sent[i] );
        }
        check( bytes == ROCKETNET_ACK_BLOCK_MIN_BYTES );
        check( block.largest == 0 );
        check( block.first_range_length == 0 );
        check( block.num_ranges == 0 );
    }
}

// blocks built from a replay window whose highest opened sits either side of
// 2^32 reconstruct exactly; a zero first range length with anything else
// nonzero is refused

static void test_ack_block_nonempty_read_across_wrap()
{
    const uint64_t highest_opened[] = { ACK_BLOCK_WRAP_2_32 - 3, ACK_BLOCK_WRAP_2_32, ACK_BLOCK_WRAP_2_32 + 4 };
    for ( size_t i = 0; i < sizeof( highest_opened ) / sizeof( highest_opened[0] ); i++ )
    {
        const uint64_t h = highest_opened[i];

        // opened: h-9 .. h-7 (a run of three), not h-6 and h-5, then h-4 .. h
        // (a run of five): first range length 5, one range of gap 2 and length 3

        struct rocketnet_replay_protection_t replay;
        memset( &replay, 0, sizeof( replay ) );
        for ( uint64_t s = h - 9; s <= h - 7; s++ )
        {
            rocketnet_replay_advance( &replay, s );
        }
        for ( uint64_t s = h - 4; s <= h; s++ )
        {
            rocketnet_replay_advance( &replay, s );
        }
        check( replay.highest_opened == h );

        uint8_t data[ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * 8];
        const int written = rocketnet_ack_block_build( data, (int) sizeof( data ), &replay );
        check( written == ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 );

        for ( uint64_t ahead = 0; ahead <= 3; ahead++ )
        {
            struct rocketnet_ack_block_t block;
            memset( &block, 0, sizeof( block ) );

            const int bytes = rocketnet_ack_block_read( &block, data, written, h + ahead );
            check( bytes == written );
            check( block.largest == h );
            check( block.first_range_length == 5 );
            check( block.num_ranges == 1 );
            check( block.range_gap[0] == 2 );
            check( block.range_length[0] == 3 );
        }
    }

    // malformed: first range length 0 with a nonzero range count, or with a
    // nonzero wire largest, at a highest sent below 2^31 and past 2^32

    const uint64_t highest_sent[] = { 0, 1, ACK_BLOCK_WRAP_2_32 + 5 };
    for ( size_t i = 0; i < sizeof( highest_sent ) / sizeof( highest_sent[0] ); i++ )
    {
        uint8_t data[ROCKETNET_ACK_BLOCK_MIN_BYTES + 4];
        uint8_t * p = data;
        rocketnet_write_uint32( &p, 0 );
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_uint16( &p, 0 );
        rocketnet_write_uint16( &p, 1 );
        rocketnet_write_uint16( &p, 1 );

        struct rocketnet_ack_block_t block;
        memset( &block, 0, sizeof( block ) );
        check( rocketnet_ack_block_read( &block, data, (int) sizeof( data ), highest_sent[i] ) == 0 );

        p = data;
        rocketnet_write_uint32( &p, 5 );
        rocketnet_write_uint8( &p, 0 );
        rocketnet_write_uint16( &p, 0 );
        check( rocketnet_ack_block_read( &block, data, ROCKETNET_ACK_BLOCK_MIN_BYTES, highest_sent[i] + 5 ) == 0 );
    }
}

REGISTER_UNIT_TEST( test_ack_block_empty_read_across_wrap );
REGISTER_UNIT_TEST( test_ack_block_nonempty_read_across_wrap );

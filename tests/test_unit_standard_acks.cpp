/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Acknowledgement block vectors for RN-1 t15.

#include "test_unit_standard.h"

#include "rocketnet_ack.h"
#include "rocketnet_replay.h"

#include <string.h>

static const rn_standard_vector_t standard_acks_table[] = {
    RN_STANDARD_VECTOR( "t15.fresh", "00000000000000" ),
    RN_STANDARD_VECTOR( "t15.fully-received", "e803000000e803" ),
    RN_STANDARD_VECTOR( "t15.one-loss", "e8030000010a0001005a00" ),
    RN_STANDARD_VECTOR( "t15.two-losses", "701101000264000300f401e803d007" ),
    RN_STANDARD_VECTOR( "t15.sequence-wrap", "00000000001000" ),
    RN_STANDARD_VECTOR( "t15.invalid-short", "000000000000" ),
    RN_STANDARD_VECTOR( "t15.invalid-geometry", "64000000010100010001" ),
    RN_STANDARD_VECTOR( "t15.invalid-zero-first", "64000000000000" ),
    RN_STANDARD_VECTOR( "t15.invalid-fresh-first", "00000000000100" ),
    RN_STANDARD_VECTOR( "t15.invalid-fresh-range", "0000000001000001000100" ),
    RN_STANDARD_VECTOR( "t15.invalid-first-too-long", "0a000000000b00" ),
    RN_STANDARD_VECTOR( "t15.invalid-zero-gap", "64000000010a0000000500" ),
    RN_STANDARD_VECTOR( "t15.invalid-zero-length", "64000000010a0005000000" ),
    RN_STANDARD_VECTOR( "t15.invalid-walk", "64000000010a0005005600" ),
    RN_STANDARD_VECTOR( "t15.truncated-oldest", "0b00000001020001000200" ),
};

static int standard_acks_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t15.invalid-short" ) == 0 )
    {
        if ( capacity < 6 )
            return -1;
        memset( out, 0, 6 );
        struct rocketnet_ack_block_t decoded;
        return rocketnet_ack_block_read( &decoded, out, 6, 0 ) == 0 ? 6 : -1;
    }

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    uint64_t highest_sent = 0;
    bool invalid = false;

    if ( strcmp( id, "t15.fresh" ) == 0 )
    {
    }
    else if ( strcmp( id, "t15.fully-received" ) == 0 )
    {
        block.largest = 1000;
        block.first_range_length = 1000;
        highest_sent = 1000;
    }
    else if ( strcmp( id, "t15.one-loss" ) == 0 )
    {
        block.largest = 1000;
        block.first_range_length = 10;
        block.num_ranges = 1;
        block.range_gap[0] = 1;
        block.range_length[0] = 90;
        highest_sent = 1005;
    }
    else if ( strcmp( id, "t15.two-losses" ) == 0 )
    {
        block.largest = 70000;
        block.first_range_length = 100;
        block.num_ranges = 2;
        block.range_gap[0] = 3;
        block.range_length[0] = 500;
        block.range_gap[1] = 1000;
        block.range_length[1] = 2000;
        highest_sent = 70000;
    }
    else if ( strcmp( id, "t15.sequence-wrap" ) == 0 )
    {
        block.largest = 0x100000000ULL;
        block.first_range_length = 16;
        highest_sent = 0xFFFFFFF0ULL;
    }
    else if ( strcmp( id, "t15.invalid-geometry" ) == 0 )
    {
        block.largest = 100;
        block.first_range_length = 1;
        block.num_ranges = 1;
        block.range_gap[0] = 1;
        block.range_length[0] = 1;
        highest_sent = 100;
        invalid = true;
    }
    else if ( strcmp( id, "t15.invalid-zero-first" ) == 0 )
    {
        block.largest = 100;
        highest_sent = 100;
        invalid = true;
    }
    else if ( strcmp( id, "t15.invalid-fresh-first" ) == 0 )
    {
        block.first_range_length = 1;
        invalid = true;
    }
    else if ( strcmp( id, "t15.invalid-fresh-range" ) == 0 )
    {
        block.num_ranges = 1;
        block.range_gap[0] = 1;
        block.range_length[0] = 1;
        invalid = true;
    }
    else if ( strcmp( id, "t15.invalid-first-too-long" ) == 0 )
    {
        block.largest = 10;
        block.first_range_length = 11;
        highest_sent = 10;
        invalid = true;
    }
    else if ( strcmp( id, "t15.invalid-zero-gap" ) == 0 )
    {
        block.largest = 100;
        block.first_range_length = 10;
        block.num_ranges = 1;
        block.range_length[0] = 5;
        highest_sent = 100;
        invalid = true;
    }
    else if ( strcmp( id, "t15.invalid-zero-length" ) == 0 )
    {
        block.largest = 100;
        block.first_range_length = 10;
        block.num_ranges = 1;
        block.range_gap[0] = 5;
        highest_sent = 100;
        invalid = true;
    }
    else if ( strcmp( id, "t15.invalid-walk" ) == 0 )
    {
        block.largest = 100;
        block.first_range_length = 10;
        block.num_ranges = 1;
        block.range_gap[0] = 5;
        block.range_length[0] = 86;
        highest_sent = 100;
        invalid = true;
    }
    else if ( strcmp( id, "t15.truncated-oldest" ) == 0 )
    {
        struct rocketnet_replay_protection_t replay;
        memset( &replay, 0, sizeof( replay ) );
        for ( uint64_t sequence = 1; sequence <= 5; sequence++ )
            rocketnet_replay_advance( &replay, sequence );
        rocketnet_replay_advance( &replay, 7 );
        rocketnet_replay_advance( &replay, 8 );
        rocketnet_replay_advance( &replay, 10 );
        rocketnet_replay_advance( &replay, 11 );
        if ( capacity < 11 )
            return -1;
        const int bytes = rocketnet_ack_block_build( out, 11, &replay );
        struct rocketnet_ack_block_t decoded;
        if ( bytes != 11 || rocketnet_ack_block_read( &decoded, out, bytes, 11 ) != bytes ||
             decoded.num_ranges != 1 || decoded.range_gap[0] != 1 || decoded.range_length[0] != 2 )
            return -1;
        return bytes;
    }
    else
    {
        return -1;
    }

    if ( capacity < ROCKETNET_ACK_BLOCK_MAX_BYTES )
        return -1;
    const int bytes = rocketnet_ack_block_write( out, &block );
    const int actual_bytes = strcmp( id, "t15.invalid-geometry" ) == 0 ? bytes - 1 : bytes;
    struct rocketnet_ack_block_t decoded;
    const int parsed = rocketnet_ack_block_read( &decoded, out, actual_bytes, highest_sent );
    if ( invalid ? parsed != 0 : parsed != actual_bytes )
        return -1;
    if ( strcmp( id, "t15.sequence-wrap" ) == 0 && decoded.largest != 0x100000000ULL )
        return -1;
    return actual_bytes;
}

static void test_standard_acks_vectors()
{
    const int count = (int) ( sizeof( standard_acks_table ) / sizeof( standard_acks_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_acks_table, count, standard_acks_generate ) == count );
}

REGISTER_UNIT_TEST( test_standard_acks_vectors );

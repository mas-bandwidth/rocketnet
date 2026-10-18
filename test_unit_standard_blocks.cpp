/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_blocks.cpp -- the vector twins of RN-2 m8
// (standard/MESSAGES.md, Blocks), regenerated through the implementation. The
// two vectors are each one RECORDS payload holding one fragment of a reliable
// ordered block: a non-final fragment carrying exactly the stride, and a
// final fragment shorter than the stride. The generator writes each record
// with the message writer and finishes the payload, then the standard's walk
// compares the result with the twin.

#include "test_unit_standard.h"

#include "rocketnet_message.h"

static const rn_standard_vector_t standard_blocks_table[] =
{
    RN_STANDARD_VECTOR( "m8.split-nonfinal", "0b800409000048000700000301000200000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f01" ),
    RN_STANDARD_VECTOR( "m8.final-short", "0b64000a000048000800000303000300000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f6061626301" ),
};

static int standard_blocks_generate( const char * id, uint8_t * out, int capacity )
{
    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.block = 1;
    static uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    if ( strcmp( id, "m8.split-nonfinal" ) == 0 )
    {
        record.number = 9;
        record.ring = 72;
        record.ordering = 7;
        record.channel = 3;
        record.fragment = 1;
        record.fragments = 2;
        record.bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        for ( int i = 0; i < ROCKETNET_MESSAGE_FRAGMENT_BYTES; i++ )
            data[i] = (uint8_t) ( i & 0x0F );
        record.data = data;
    }
    else if ( strcmp( id, "m8.final-short" ) == 0 )
    {
        record.number = 10;
        record.ring = 72;
        record.ordering = 8;
        record.channel = 3;
        record.fragment = 3;
        record.fragments = 3;
        record.bytes = 100;
        for ( int i = 0; i < 100; i++ )
            data[i] = (uint8_t) i;
        record.data = data;
    }
    else
        return -1;

    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, out, capacity );
    if ( !rocketnet_message_writer_add( &writer, &record ) )
        return -1;
    return rocketnet_message_writer_finish( &writer );
}

static void standard_blocks_vectors()
{
    const int count = (int) ( sizeof( standard_blocks_table ) / sizeof( standard_blocks_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_blocks_table, count, standard_blocks_generate ) == count );
}

REGISTER_UNIT_TEST( standard_blocks_vectors );

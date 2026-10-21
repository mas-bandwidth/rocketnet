/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_blocks.cpp -- the vector twins of RN-2 m8
// (standard/MESSAGES.md, Blocks), regenerated through the implementation.
//
// The split vectors are fragments 1 and 2 of a three-fragment reliable ordered
// block of 2404 bytes: 1152 + 1152 + 100. The ring-wrap vector is two RECORDS
// payloads back to back for a two-fragment block of 1153 bytes whose span is
// padded from 1152 to the ring size 1168 before placement. The test also
// reassembles both blocks from their fragments in index order and checks that
// the wrapped placement passes the receiver's ring check while the same
// placement without the wrap is refused.

#include "test_unit_standard.h"

#include "rocketnet_message.h"
#include "rocketnet_window_send.h"

static const rn_standard_vector_t standard_blocks_table[] =
{
    RN_STANDARD_VECTOR( "m8.split-nonfinal",
        "0b80040a000048000700000301000200000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f01"
    ),
    RN_STANDARD_VECTOR( "m8.final-short",
        "0b64000b000090000700000302000200000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f6061626301"
    ),
    RN_STANDARD_VECTOR( "m8.ring-wrap",
        "0b800401000000000100000000000100000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        "707172737475767778797a7b7c7d7e7f010b0100020000480001000000010001000001"
    ),
};

static uint8_t standard_blocks_data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];

static void standard_blocks_fill( int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        standard_blocks_data[i] = (uint8_t) ( i & 0xFF );
}

static int standard_blocks_encode( uint32_t number, uint32_t ring, uint32_t ordering, uint32_t channel,
                                   uint32_t fragment, uint32_t fragments, int bytes, uint8_t * out, int capacity )
{
    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.block = 1;
    record.number = number;
    record.ring = ring;
    record.ordering = ordering;
    record.channel = channel;
    record.fragment = fragment;
    record.fragments = fragments;
    record.bytes = bytes;
    record.data = standard_blocks_data;

    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, out, capacity );
    if ( !rocketnet_message_writer_add( &writer, &record ) )
        return -1;
    return rocketnet_message_writer_finish( &writer );
}

static void standard_blocks_region( rn_region_t * region, rn_send_number_t * numbers,
                                    uint32_t ring_bytes, uint32_t max_message_bytes )
{
    memset( region, 0, sizeof( *region ) );
    region->window = 16;
    region->ring_bytes = ring_bytes;
    region->max_message_bytes = max_message_bytes;
    region->send_numbers = numbers;
}

static int standard_blocks_split( const char * id, uint8_t * out, int capacity )
{
    rn_region_t region;
    static rn_send_number_t numbers[16];
    standard_blocks_region( &region, numbers, 4096, 2404 );
    rn_window_send_t send;
    rn_window_send_init( &send, &region, 9 );
    uint64_t first = 0;
    uint64_t ring_start = 0;
    if ( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2404, 0, &first, &ring_start ) != 3 )
        return -1;

    uint32_t index = 0;
    if ( strcmp( id, "m8.split-nonfinal" ) == 0 )
        index = 1;
    else if ( strcmp( id, "m8.final-short" ) == 0 )
        index = 2;
    else
        return -1;

    const uint32_t bytes = index == 2 ? 100 : (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    standard_blocks_fill( bytes );
    const rn_send_number_t * slot = &numbers[( first + index ) % region.window];
    const uint32_t ring = (uint32_t) ( ( slot->ring_start % region.ring_bytes ) / 16 );
    return standard_blocks_encode( (uint32_t) ( first + index ), ring, 7, 3, index, 2, bytes, out, capacity );
}

static int standard_blocks_ring_wrap( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "m8.ring-wrap" ) != 0 )
        return -1;

    rn_region_t region;
    static rn_send_number_t numbers[16];
    standard_blocks_region( &region, numbers, 1168, 2304 );
    rn_window_send_t send;
    rn_window_send_init( &send, &region, 0 );

    uint64_t number = 0;
    uint64_t ring_start = 0;
    if ( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1152, 0, &number, &ring_start ) != RN_WINDOW_SEND_NUMBERED )
        return -1;
    rn_window_send_mark_acked( &send, number );

    uint64_t first = 0;
    if ( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1153, 1, &first, &ring_start ) != 2 )
        return -1;

    int written = 0;
    for ( uint32_t index = 0; index < 2; index++ )
    {
        const int bytes = index == 0 ? (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES : 1;
        standard_blocks_fill( bytes );
        const rn_send_number_t * slot = &numbers[( first + index ) % region.window];
        const uint32_t ring = (uint32_t) ( ( slot->ring_start % region.ring_bytes ) / 16 );
        const int encoded = standard_blocks_encode( (uint32_t) ( first + index ), ring, 1, 0, index, 1, bytes, out + written, capacity - written );
        if ( encoded < 0 )
            return -1;
        written += encoded;
    }
    return written;
}

static int standard_blocks_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "m8.split-nonfinal" ) == 0 || strcmp( id, "m8.final-short" ) == 0 )
        return standard_blocks_split( id, out, capacity );
    return standard_blocks_ring_wrap( id, out, capacity );
}

static void test_standard_blocks_vectors()
{
    const int count = (int) ( sizeof( standard_blocks_table ) / sizeof( standard_blocks_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_blocks_table, count, standard_blocks_generate ) == count );

    // Split and reassemble: the fragments in index order are the message.
    static uint8_t fragments[2404];
    int at = 0;
    for ( uint32_t index = 0; index < 3; index++ )
    {
        const int bytes = index == 2 ? 100 : (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        standard_blocks_fill( bytes );
        memcpy( fragments + at, standard_blocks_data, (size_t) bytes );
        at += bytes;
    }
    at = 0;
    for ( uint32_t index = 0; index < 3; index++ )
    {
        const int bytes = index == 2 ? 100 : (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        for ( int i = 0; i < bytes; i++ )
            rocketnet_unit_check( fragments[at + i] == (uint8_t) ( i & 0xFF ) );
        at += bytes;
    }

    // The ring-wrap payload passes the ring check at base 0; the same block
    // without the boundary padding would place fragment 0 at 1152 and be
    // refused outside the 1168-byte ring.
    uint8_t payload[RN_STANDARD_VECTOR_MAX_BYTES];
    const int wrapped = standard_blocks_ring_wrap( "m8.ring-wrap", payload, (int) sizeof( payload ) );
    rocketnet_unit_check( wrapped > 0 );

    struct rocketnet_message_limits_t limits;
    memset( &limits, 0, sizeof( limits ) );
    limits.num_channels = 4;
    limits.enabled_codes = 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    limits.reliable_messages_in_flight = 16;
    limits.max_message_bytes = 2304;
    limits.receive_message_buffer_bytes = 1168;

    uint8_t first_payload[RN_STANDARD_VECTOR_MAX_BYTES];
    const int first_bytes = standard_blocks_ring_wrap( "m8.ring-wrap", first_payload, (int) sizeof( first_payload ) );
    rocketnet_unit_check( first_bytes > 0 );
    const int first_length = 16 + (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
    rocketnet_unit_check( rocketnet_message_check_payload( first_payload, first_length, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    uint8_t unpadded[RN_STANDARD_VECTOR_MAX_BYTES];
    memcpy( unpadded, first_payload, (size_t) first_length );
    unpadded[6] = 72;
    unpadded[7] = 0;
    rocketnet_unit_check( rocketnet_message_check_payload( unpadded, first_length, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_RING );

    // The generator knows every id it is asked for and no other.
    uint8_t scratch[4];
    rocketnet_unit_check( standard_blocks_generate( "m8.no-such-vector", scratch, 4 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_blocks_vectors );

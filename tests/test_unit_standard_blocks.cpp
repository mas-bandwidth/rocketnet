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
// reassembles both blocks by feeding their payloads, in index order, to a real
// receiving endpoint (rn_ordered_receive over a unit arena, which runs the
// reader and the channel's block assembly, sdk/rocketnet_channel.cpp), and
// checks the one delivery each block makes, byte for byte; and it checks that
// the wrapped placement passes the receiver's ring check while the same
// placement without the wrap is refused.

#include "test_unit_standard.h"
#include "unit_arena.h"

#include "rocketnet.h"
#include "rocketnet_counters.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_message.h"
#include "rocketnet_window_send.h"

static const rn_standard_vector_t standard_blocks_table[] = {
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
                        "707172737475767778797a7b7c7d7e7f01" ),
    RN_STANDARD_VECTOR( "m8.final-short",
                        "0b64000b000090000700000302000200000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
                        "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f6061626301" ),
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
                        "707172737475767778797a7b7c7d7e7f010b0100020000480001000000010001000001" ),
};

static uint8_t standard_blocks_data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];

static void standard_blocks_fill( int bytes )
{
    for ( int i = 0; i < bytes; i++ )
    {
        standard_blocks_data[i] = (uint8_t) ( i & 0xFF );
    }
}

static int standard_blocks_encode( uint32_t number, uint32_t ring, uint32_t ordering, uint32_t channel,
                                   uint32_t fragment, uint32_t fragments, int bytes, uint8_t * out, int capacity,
                                   int block = 1 )
{
    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.block = block;
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
    {
        return -1;
    }
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

// fragment index of the three-fragment block, placed by the sender's window
// exactly as the vectors are, on the given ordering and channel
static int standard_blocks_split_fragment( uint32_t index, uint32_t ordering, uint32_t channel, uint8_t * out, int capacity )
{
    rn_region_t region;
    static rn_send_number_t numbers[16];
    standard_blocks_region( &region, numbers, 4096, 2404 );

    rn_window_send_t send;
    rn_window_send_init( &send, &region, 9 );

    uint64_t first = 0;
    uint64_t ring_start = 0;
    if ( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2404, 0, &first, &ring_start ) != 3 )
    {
        return -1;
    }

    const int bytes = index == 2 ? 100 : (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    standard_blocks_fill( bytes );

    const rn_send_number_t * slot = &numbers[( first + index ) % region.window];
    const uint32_t ring = (uint32_t) ( ( slot->ring_start % region.ring_bytes ) / 16 );
    return standard_blocks_encode( (uint32_t) ( first + index ), ring, ordering, channel, index, 2, bytes, out, capacity );
}

static int standard_blocks_split( const char * id, uint8_t * out, int capacity )
{
    uint32_t index = 0;
    if ( strcmp( id, "m8.split-nonfinal" ) == 0 )
    {
        index = 1;
    }
    else if ( strcmp( id, "m8.final-short" ) == 0 )
    {
        index = 2;
    }
    else
    {
        return -1;
    }

    return standard_blocks_split_fragment( index, 7, 3, out, capacity );
}

static int standard_blocks_ring_wrap( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "m8.ring-wrap" ) != 0 )
    {
        return -1;
    }

    rn_region_t region;
    static rn_send_number_t numbers[16];
    standard_blocks_region( &region, numbers, 1168, 2304 );

    rn_window_send_t send;
    rn_window_send_init( &send, &region, 0 );

    uint64_t number = 0;
    uint64_t ring_start = 0;
    if ( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1152, 0, &number, &ring_start ) != RN_WINDOW_SEND_NUMBERED )
    {
        return -1;
    }
    rn_window_send_mark_acked( &send, number );

    uint64_t first = 0;
    if ( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1153, 1, &first, &ring_start ) != 2 )
    {
        return -1;
    }

    int written = 0;
    for ( uint32_t index = 0; index < 2; index++ )
    {
        const int bytes = index == 0 ? (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES : 1;
        standard_blocks_fill( bytes );

        const rn_send_number_t * slot = &numbers[( first + index ) % region.window];
        const uint32_t ring = (uint32_t) ( ( slot->ring_start % region.ring_bytes ) / 16 );
        const int encoded = standard_blocks_encode( (uint32_t) ( first + index ), ring, 1, 0, index, 1, bytes, out + written, capacity - written );
        if ( encoded < 0 )
        {
            return -1;
        }
        written += encoded;
    }

    return written;
}

static int standard_blocks_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "m8.split-nonfinal" ) == 0 || strcmp( id, "m8.final-short" ) == 0 )
    {
        return standard_blocks_split( id, out, capacity );
    }
    return standard_blocks_ring_wrap( id, out, capacity );
}

// A real receiving endpoint: the reader and the channels of an rn_ordered_t
// over a one-region unit arena. The vectors' rings (1168 bytes for the wrap)
// are below the config floor of 4096, so the arena is carved at 4096 and the
// region and the reader are then held to the vector's ring: a smaller ring
// inside the carved one.

struct standard_blocks_receiver_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
};

static void standard_blocks_receiver_init( standard_blocks_receiver_t * receiver, uint32_t ring_bytes )
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 4;
    config.reliable_messages_in_flight = 64;
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = 4096;
    config.send_message_buffer_bytes = 65536;

    memset( receiver, 0, sizeof( *receiver ) );
    receiver->arena = rn_unit_arena_create( &config, 1, &receiver->counters );
    rocketnet_unit_check( receiver->arena != NULL );
    receiver->region = rn_arena_region( receiver->arena, 0 );
    rocketnet_unit_check( rn_ordered_init( &receiver->ordered, receiver->region, &config, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );

    rocketnet_unit_check( ring_bytes <= receiver->region->ring_bytes );
    receiver->region->ring_bytes = ring_bytes;
    receiver->ordered.limits.receive_message_buffer_bytes = (int) ring_bytes;

    // the ring starts poisoned, so a delivered byte the reader never placed
    // (a fragment skipped or written elsewhere) reads 0xEE, never a zero
    // that matches the vector's data by chance
    memset( receiver->region->receive_ring, 0xEE, ring_bytes );
}

// one payload received: copied to a 16-aligned packet the caller keeps (a
// delivery may point into it), the reader's verdict checked, the deliveries
// it made returned
static uint32_t standard_blocks_receive( standard_blocks_receiver_t * receiver, uint8_t * packet, const uint8_t * payload, int bytes )
{
    rocketnet_unit_check( bytes > 0 && bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    memcpy( packet, payload, (size_t) bytes );
    rocketnet_unit_check( rn_ordered_receive( &receiver->ordered, packet, bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    return rn_ordered_deliveries( &receiver->ordered );
}

static void standard_blocks_receiver_destroy( standard_blocks_receiver_t * receiver )
{
    rocketnet_unit_check( receiver->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );
    rocketnet_unit_check( receiver->counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK] == 0 );
    rocketnet_unit_check( receiver->counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_RING] == 0 );
    rn_ordered_end_update( &receiver->ordered );
    rn_unit_arena_destroy( receiver->arena );
}

static void test_standard_blocks_vectors()
{
    const int count = (int) ( sizeof( standard_blocks_table ) / sizeof( standard_blocks_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_blocks_table, count, standard_blocks_generate ) == count );

    // split and reassemble: the three fragments, placed by the sender's window
    // as the split vectors are (channel 3, on ordering 0 so the block is the
    // channel's next), go to the receiver in index order. Nothing is delivered
    // until the last is in; then the one delivery is the 2404-byte message.

    {
        static uint8_t expected[2404];
        int at = 0;
        for ( uint32_t index = 0; index < 3; index++ )
        {
            const int bytes = index == 2 ? 100 : (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES;
            for ( int i = 0; i < bytes; i++ )
            {
                expected[at + i] = (uint8_t) ( i & 0xFF );
            }
            at += bytes;
        }

        standard_blocks_receiver_t receiver;
        standard_blocks_receiver_init( &receiver, 4096 );

        alignas( 16 ) static uint8_t packets[3][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        for ( uint32_t index = 0; index < 3; index++ )
        {
            uint8_t payload[RN_STANDARD_VECTOR_MAX_BYTES];
            const int bytes = standard_blocks_split_fragment( index, 0, 3, payload, (int) sizeof( payload ) );
            const uint32_t deliveries = standard_blocks_receive( &receiver, packets[index], payload, bytes );
            rocketnet_unit_check( deliveries == ( index == 2 ? 1u : 0u ) );
        }

        const rn_delivery_t & delivery = receiver.region->deliveries[0];
        rocketnet_unit_check( delivery.channel == 3 );
        rocketnet_unit_check( delivery.bytes == 2404 );
        rocketnet_unit_check( memcmp( delivery.data, expected, sizeof( expected ) ) == 0 );

        standard_blocks_receiver_destroy( &receiver );
    }

    // ring wrap and reassemble: the 1152-byte message the vector's block
    // follows is received and released first (ordering 0, ring 0), then the
    // vector's two payloads. The one delivery is the 1153-byte block, placed
    // at the wrapped base 0 of the 1168-byte ring: fragment 0's bytes, then
    // fragment 1's one byte.

    {
        standard_blocks_receiver_t receiver;
        standard_blocks_receiver_init( &receiver, 1168 );

        alignas( 16 ) static uint8_t packets[3][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        uint8_t payload[RN_STANDARD_VECTOR_MAX_BYTES];

        const int single_bytes = (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        standard_blocks_fill( single_bytes );
        const int first = standard_blocks_encode( 0, 0, 0, 0, 0, 0, single_bytes, payload, (int) sizeof( payload ), 0 );
        rocketnet_unit_check( standard_blocks_receive( &receiver, packets[0], payload, first ) == 1 );
        rocketnet_unit_check( receiver.region->deliveries[0].bytes == single_bytes );
        rocketnet_unit_check( memcmp( receiver.region->deliveries[0].data, standard_blocks_data, (size_t) single_bytes ) == 0 );
        rn_ordered_end_update( &receiver.ordered );

        // the vector is the block's two payloads back to back, one per
        // fragment: each goes to the receiver as its own packet

        const int wrapped = standard_blocks_ring_wrap( "m8.ring-wrap", payload, (int) sizeof( payload ) );
        const int fragment_0_bytes = 16 + (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
        rocketnet_unit_check( wrapped > fragment_0_bytes );
        rocketnet_unit_check( standard_blocks_receive( &receiver, packets[1], payload, fragment_0_bytes ) == 0 );
        rocketnet_unit_check( standard_blocks_receive( &receiver, packets[2], payload + fragment_0_bytes, wrapped - fragment_0_bytes ) == 1 );

        const rn_delivery_t & delivery = receiver.region->deliveries[0];
        rocketnet_unit_check( delivery.channel == 0 );
        rocketnet_unit_check( delivery.bytes == (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1 );
        rocketnet_unit_check( delivery.data == receiver.region->receive_ring );
        for ( int i = 0; i < (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES; i++ )
        {
            rocketnet_unit_check( delivery.data[i] == (uint8_t) ( i & 0xFF ) );
        }
        rocketnet_unit_check( delivery.data[ROCKETNET_MESSAGE_FRAGMENT_BYTES] == 0 );

        standard_blocks_receiver_destroy( &receiver );
    }

    // The ring-wrap payload passes the ring check at base 0; the same block
    // without the boundary padding would place fragment 0 at 1152 and be
    // refused outside the 1168-byte ring.

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

    // the generator knows every id it is asked for and no other

    uint8_t scratch[4];
    rocketnet_unit_check( standard_blocks_generate( "m8.no-such-vector", scratch, 4 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_blocks_vectors );

// A region whose ring is zero bytes (a carve outside the validated config
// path; proofs/tla/RNRing.tla ASSUME B > 0 does not hold) stalls a holdable first
// write and a block first write instead of dividing by zero, counting the
// stall once. A block of more numbers than the window holds stalls at first
// write instead of being admitted: without that guard W - count wraps.
// rocketnet_assert compiles out in Release (NDEBUG), so the init assert is
// no guard there; a quiet hook keeps builds with asserts running.

static void standard_blocks_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}

static void test_window_send_zero_ring_stalls()
{
    rocketnet_assert_function( standard_blocks_quiet_assert );

    rn_region_t region;
    static rn_send_number_t numbers[16];
    static rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    standard_blocks_region( &region, numbers, 0, 64 );
    region.counters = &counters;

    rn_window_send_t send;
    rn_window_send_init( &send, &region, 0 );

    uint64_t number = 0;
    uint64_t ring_start = 0;
    rocketnet_unit_check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, 0, &number, &ring_start ) == RN_WINDOW_SEND_STALL );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );
    rocketnet_unit_check( send.next == 0 );
    rocketnet_unit_check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, 0, &number, &ring_start ) == RN_WINDOW_SEND_STALL );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );
    rocketnet_unit_check( send.next == 0 );

    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_window_send_zero_ring_stalls );

static void test_window_send_zero_ring_block_stalls()
{
    rocketnet_assert_function( standard_blocks_quiet_assert );

    rn_region_t region;
    static rn_send_number_t numbers[16];
    static rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    standard_blocks_region( &region, numbers, 0, 64 );
    region.counters = &counters;

    rn_window_send_t send;
    rn_window_send_init( &send, &region, 0 );

    // two fragments: larger than one fragment, so the block path is taken

    const uint32_t bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES + 32;
    uint64_t first = 0;
    uint64_t ring_start = 0;
    rocketnet_unit_check( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, bytes, 0, &first, &ring_start ) == (uint32_t) RN_WINDOW_SEND_STALL );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );
    rocketnet_unit_check( send.next == 0 );
    rocketnet_unit_check( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, bytes, 0, &first, &ring_start ) == (uint32_t) RN_WINDOW_SEND_STALL );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );
    rocketnet_unit_check( send.next == 0 );

    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_window_send_zero_ring_block_stalls );

static void test_window_send_block_over_window_stalls()
{
    rocketnet_assert_function( standard_blocks_quiet_assert );

    rn_region_t region;
    static rn_send_number_t numbers[16];
    static rn_send_number_t numbers_before[16];
    static rocketnet_counters_t counters;
    memset( numbers, 0xA5, sizeof( numbers ) );
    memcpy( numbers_before, numbers, sizeof( numbers ) );
    memset( &counters, 0, sizeof( counters ) );
    standard_blocks_region( &region, numbers, 8192, 4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES );
    region.window = 2;
    region.counters = &counters;

    rn_window_send_t send;
    rn_window_send_init( &send, &region, 0 );

    // four fragments: 3 * F + 1 bytes needs count 4 against a window of 2.

    const uint32_t bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
    uint64_t first = 0;
    uint64_t ring_start = 0;
    rocketnet_unit_check( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, bytes, 0, &first, &ring_start ) == 0 );
    rocketnet_unit_check( send.next == 0 );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );
    rocketnet_unit_check( memcmp( numbers, numbers_before, sizeof( numbers ) ) == 0 );

    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_window_send_block_over_window_stalls );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The live receive path (rn_ordered_receive, sdk/rocketnet_channel.cpp) and
// the dispatch by wire code (rn_channels_dispatch, same file): a violation
// moves its own counter as well as MESSAGE_PROTOCOL_VIOLATION, a connection
// that enables only a receipt variant takes the base code it travels as, and
// a RELIABLE_SEQUENCED message on a wide channel is always sendable. Time is
// an argument: the clock starts at 1000.0 and no socket is opened.

#include <string.h>

#include "test_unit_registry.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_read_write.h"

#define check rocketnet_unit_check

struct violation_packet_t
{
    uint64_t id;
    int bytes;
    uint8_t bytes_pad[16 - sizeof( uint64_t ) - sizeof( int )];                 // explicit: data starts 16-aligned
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t tail_pad[16 - ROCKETNET_MAX_PACKET_PAYLOAD_BYTES % 16];                     // explicit: the size rounds up to 16
};
static_assert( sizeof( violation_packet_t ) == 16 + ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 15 ) / 16 * 16, "no implicit padding" );
static_assert( alignof( violation_packet_t ) == 16, "data is 16-aligned" );

struct violation_node_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    uint64_t next_packet_id;
};

static void violation_config( rocketnet_message_config_t * config, int num_channels )
{
    rocketnet_message_default_config( config );
    config->num_channels = num_channels;
    config->reliable_messages_in_flight = 1024;
    config->receive_message_buffer_bytes = 262144;
    config->max_message_bytes = 262144;
    config->send_message_buffer_bytes = 800000;
    config->message_resend_delay_min_ms = 20;
    config->message_resend_delay_initial_ms = 100;
    config->message_resend_delay_max_ms = 400;
}

static void violation_init( violation_node_t * node, const rocketnet_message_config_t * config )
{
    memset( node, 0, sizeof( *node ) );
    node->arena = rn_arena_create( NULL, config, 1, &node->counters );
    check( node->arena != NULL );
    node->region = rn_arena_region( node->arena, 0 );
    check( rn_ordered_init( &node->ordered, node->region, config, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    node->next_packet_id = 1;
}

static void violation_send( violation_node_t * node, uint32_t channel, int type, const uint8_t * data, uint32_t bytes )
{
    uint8_t * pointer = rn_channels_start( &node->ordered.channels, channel, type, bytes );
    check( pointer != NULL );
    if ( !pointer )
        return;
    memcpy( pointer, data, bytes );
    check( rn_channels_finish( &node->ordered.channels, bytes ) );
}

static int violation_write( violation_node_t * node, double now, violation_packet_t * packets, int capacity )
{
    alignas( 16 ) uint8_t frames[8][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[8];
    check( capacity > 0 && capacity <= 8 );
    for ( int i = 0; i < capacity; i++ )
        pointers[i] = frames[i];
    const int count = rn_ordered_write( &node->ordered, now, pointers, capacity );
    for ( int i = 0; i < count; i++ )
    {
        packets[i].id = node->next_packet_id++;
        packets[i].bytes = (int) node->ordered.builder.packet[i].payload_bytes;
        check( packets[i].bytes > 0 && packets[i].bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        memcpy( packets[i].data, frames[i], (size_t) packets[i].bytes );
        rn_ordered_commit( &node->ordered, i, packets[i].id, now );
    }
    return count;
}

// A RELIABLE_ORDERED record on a channel past the receiver's count fails the
// reader's check 5 (MESSAGE_BAD_CHANNEL) before any dispatch. The live path
// counts that violation by its own counter, then MESSAGE_PROTOCOL_VIOLATION
// once for the ended connection (rn_channels_count_violation).
static void test_violation_malformed_ordered_record_moves_its_own_counter()
{
    rocketnet_message_config_t sender_config, receiver_config;
    violation_config( &sender_config, 16 );
    violation_config( &receiver_config, 8 );
    violation_node_t sender, receiver;
    violation_init( &sender, &sender_config );
    violation_init( &receiver, &receiver_config );

    const uint8_t value = 42;
    violation_send( &sender, 12, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, &value, 1 );
    violation_packet_t packet;
    check( violation_write( &sender, 1000.0, &packet, 1 ) == 1 );

    check( rn_ordered_receive( &receiver.ordered, packet.data, packet.bytes ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL] == 1 );
    check( rn_ordered_decide( &receiver.ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_violation_malformed_ordered_record_moves_its_own_counter );

// A connection whose mask enables RELIABLE_WITH_RECEIPT and not RELIABLE takes
// wire code 2 (rocketnet_message_enabled_codes), so the reader admits the
// record and the dispatch delivers it: no TYPE_NOT_ENABLED, no violation.
static void test_violation_receipt_only_reliable_connection_takes_its_records()
{
    rocketnet_message_config_t config;
    violation_config( &config, 8 );
    violation_node_t sender, receiver;
    violation_init( &sender, &config );
    violation_init( &receiver, &config );

    const uint32_t mask = 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT;
    check( rn_channels_init( &receiver.ordered.channels, receiver.region, receiver.region->num_channels, mask ) );
    check( rn_channels_set_payload_capacity( &receiver.ordered.channels, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    receiver.ordered.limits.enabled_codes = rn_channels_enabled_codes( &receiver.ordered.channels );
    check( ( receiver.ordered.limits.enabled_codes & ( 1u << ROCKETNET_MESSAGE_CODE_RELIABLE ) ) != 0 );
    check( ( receiver.ordered.channels.enabled_types & ( 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE ) ) == 0 );

    const uint8_t value = 7;
    violation_send( &sender, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT, &value, 1 );
    violation_packet_t packet;
    check( violation_write( &sender, 1000.0, &packet, 1 ) == 1 );

    check( rn_ordered_receive( &receiver.ordered, packet.data, packet.bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED] == 0 );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );
    check( rn_ordered_deliveries( &receiver.ordered ) == 1 );
    const rn_delivery_t & delivery = receiver.region->deliveries[0];
    check( delivery.code == ROCKETNET_CHANNEL_TYPE_RELIABLE && delivery.bytes == 1 && delivery.data[0] == value );
    rn_ordered_end_update( &receiver.ordered );

    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_violation_receipt_only_reliable_connection_takes_its_records );

#if ROCKETNET_MAX_CHANNELS > 256

// A RELIABLE_SEQUENCED record on channel 256 carries the WIDE channel field:
// a 16-byte header (rocketnet_message_flags_header_bytes,
// sdk/rocketnet_message.cpp:52) and the form byte.
// The largest single record is payload_capacity - 17 bytes; one byte more is
// a block. The boundary size payload_capacity - 16 is written and delivered.
static void test_violation_wide_reliable_sequenced_boundary_is_sent()
{
    rocketnet_message_config_t config;
    violation_config( &config, 300 );
    violation_node_t sender, receiver;
    violation_init( &sender, &config );
    violation_init( &receiver, &config );

    const uint32_t channel = 256;
    const uint32_t bytes = sender.ordered.channels.payload_capacity - 16;
    static uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    for ( uint32_t i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( i * 13 + 5 );
    violation_send( &sender, channel, ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED, data, bytes );

    static violation_packet_t packets[8];
    const int count = violation_write( &sender, 1000.0, packets, 8 );
    check( count >= 1 );
    for ( int i = 0; i < count; i++ )
        check( rn_ordered_receive( &receiver.ordered, packets[i].data, packets[i].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_ordered_deliveries( &receiver.ordered ) == 1 );
    const rn_delivery_t & delivery = receiver.region->deliveries[0];
    check( delivery.channel == channel && delivery.bytes == (int) bytes && memcmp( delivery.data, data, bytes ) == 0 );
    rn_ordered_end_update( &receiver.ordered );

    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_violation_wide_reliable_sequenced_boundary_is_sent );

#endif // #if ROCKETNET_MAX_CHANNELS > 256

// The remaining wire-check violations (STANDARD.md _Messages_ checks 1 to 4,
// 9, 10 and 12) each move their own counter beside MESSAGE_PROTOCOL_VIOLATION,
// which is counted once for the connection the violation ends. A fresh node
// per case, because the first violation ends the connection.
static void violation_feed_refuses( violation_node_t * receiver, const uint8_t * data, int bytes,
                                    int expected_result, int expected_counter )
{
    const int result = rn_ordered_receive( &receiver->ordered, data, bytes );
    if ( expected_result >= 0 )
        check( result == expected_result );
    else
        check( result != ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( receiver->counters.values[expected_counter] == 1 );
    check( receiver->counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
    check( rn_ordered_decide( &receiver->ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
}

static void test_violation_reader_and_window_checks_move_their_own_counters()
{
    // check 1: BAD_FORM
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_config( &config, 8 );
        violation_init( &receiver, &config );
        const uint8_t payload[1] = { 0x00 };
        violation_feed_refuses( &receiver, payload, 1, ROCKETNET_MESSAGE_VIOLATION_BAD_FORM, ROCKETNET_COUNTER_MESSAGE_BAD_FORM );
        rn_arena_destroy( receiver.arena );
    }

    // check 2: TRUNCATED
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_config( &config, 8 );
        violation_init( &receiver, &config );
        const uint8_t payload[2] = { 0x00, ROCKETNET_MESSAGE_FORM_RECORDS };
        violation_feed_refuses( &receiver, payload, 2, ROCKETNET_MESSAGE_VIOLATION_TRUNCATED, ROCKETNET_COUNTER_MESSAGE_TRUNCATED );
        rn_arena_destroy( receiver.arena );
    }

    // check 3: BAD_FLAGS (reserved bits)
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_config( &config, 8 );
        violation_init( &receiver, &config );
        const uint8_t payload[4] = { 0x80, 0x00, 0x00, ROCKETNET_MESSAGE_FORM_RECORDS };
        violation_feed_refuses( &receiver, payload, 4, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS, ROCKETNET_COUNTER_MESSAGE_BAD_FLAGS );
        rn_arena_destroy( receiver.arena );
    }

    // check 4: BAD_LENGTH (a RELIABLE_ORDERED record with length zero)
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_config( &config, 8 );
        violation_init( &receiver, &config );
        uint8_t payload[13];
        uint8_t * p = payload;
        rocketnet_write_uint8( &p, 0x03 );
        rocketnet_write_uint16( &p, 0 );
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        rocketnet_write_uint16( &p, 0 );
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = ROCKETNET_MESSAGE_FORM_RECORDS;
        violation_feed_refuses( &receiver, payload, (int) ( p - payload ), ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH, ROCKETNET_COUNTER_MESSAGE_BAD_LENGTH );
        rn_arena_destroy( receiver.arena );
    }

    // check 9: BAD_SIZE (a reliable record on a payload past the wire maximum)
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_config( &config, 8 );
        violation_init( &receiver, &config );
        static uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 64];
        uint8_t * p = payload;
        rocketnet_write_uint8( &p, 0x03 );
        rocketnet_write_uint16( &p, 1 );
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        rocketnet_write_uint16( &p, 0 );
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0x00;
        const int total = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1;
        memset( p, 0xFF, (size_t) ( total - 1 - (int) ( p - payload ) ) );
        payload[total - 1] = ROCKETNET_MESSAGE_FORM_RECORDS;
        violation_feed_refuses( &receiver, payload, total, ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE, ROCKETNET_COUNTER_MESSAGE_BAD_SIZE );
        rn_arena_destroy( receiver.arena );
    }

    // check 12: BAD_CONTROL (a CONFIG control record with an invalid body)
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_config( &config, 8 );
        violation_init( &receiver, &config );
        uint8_t payload[8];
        uint8_t * p = payload;
        rocketnet_write_uint8( &p, 0x22 );
        rocketnet_write_uint16( &p, 1 );
        *p++ = 8;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0x00;
        *p++ = ROCKETNET_MESSAGE_FORM_RECORDS;
        violation_feed_refuses( &receiver, payload, (int) ( p - payload ), -1, ROCKETNET_COUNTER_MESSAGE_BAD_CONTROL );
        rn_arena_destroy( receiver.arena );
    }

    // check 10: NUMBER_OUTSIDE_WINDOW
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_config( &config, 8 );
        violation_init( &receiver, &config );
        uint8_t payload[14];
        uint8_t * p = payload;
        rocketnet_write_uint8( &p, 0x03 );
        rocketnet_write_uint16( &p, 1 );
        *p++ = 0x00;
        *p++ = 0x04;
        *p++ = 0x00;
        rocketnet_write_uint16( &p, 0 );
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0x00;
        *p++ = ROCKETNET_MESSAGE_FORM_RECORDS;
        violation_feed_refuses( &receiver, payload, (int) ( p - payload ), ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW, ROCKETNET_COUNTER_MESSAGE_NUMBER_OUTSIDE_WINDOW );
        rn_arena_destroy( receiver.arena );
    }
}

REGISTER_UNIT_TEST( test_violation_reader_and_window_checks_move_their_own_counters );

// A message above the receiver's max_message_bytes is a violation on every
// path (STANDARD.md _Messages_ check 9), not only on the block path (check 7).
// An unreliable payload may be larger than one packet (the transport carries
// it in fragments), so a SINGLE message or an unreliable record of 2000 bytes
// reaches a receiver configured for 1416: it is refused and counted
// MESSAGE_BAD_SIZE, exactly as a block that adds up past the limit is refused
// and counted MESSAGE_BAD_BLOCK. A message at the limit is still delivered.
static void violation_over_max_config( rocketnet_message_config_t * config )
{
    violation_config( config, 8 );
    config->max_message_bytes = 1416;
}

static void test_violation_single_message_over_max_message_bytes_is_refused()
{
    static uint8_t data[4096];
    memset( data, 0x5A, sizeof( data ) );
    alignas( 16 ) static uint8_t payload[4096];

    // the SINGLE form, one byte over the limit and well over it
    const int over[2] = { 1417, 2000 };
    for ( int i = 0; i < 2; i++ )
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_over_max_config( &config );
        violation_init( &receiver, &config );
        memcpy( payload, data, (size_t) over[i] );
        const int bytes = rocketnet_message_single_finish( payload, (int) sizeof( payload ), over[i] );
        check( bytes == over[i] + 1 );
        violation_feed_refuses( &receiver, payload, bytes, ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE, ROCKETNET_COUNTER_MESSAGE_BAD_SIZE );
        rn_arena_destroy( receiver.arena );
    }

    // the SINGLE form at the limit is delivered
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_over_max_config( &config );
        violation_init( &receiver, &config );
        memcpy( payload, data, 1416 );
        const int bytes = rocketnet_message_single_finish( payload, (int) sizeof( payload ), 1416 );
        check( rn_ordered_receive( &receiver.ordered, payload, bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );
        check( receiver.ordered.channels.deliveries == 1 );
        rn_arena_destroy( receiver.arena );
    }

    // one UNRELIABLE or UNRELIABLE_SEQUENCED record over the limit, in RECORDS form
    const int codes[2] = { ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED };
    for ( int i = 0; i < 2; i++ )
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_over_max_config( &config );
        violation_init( &receiver, &config );
        rocketnet_message_record_t record;
        memset( &record, 0, sizeof( record ) );
        record.code = codes[i];
        record.data = data;
        record.bytes = 2000;
        rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );
        check( rocketnet_message_writer_add( &writer, &record ) == 1 );
        const int bytes = rocketnet_message_writer_finish( &writer );
        violation_feed_refuses( &receiver, payload, bytes, ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE, ROCKETNET_COUNTER_MESSAGE_BAD_SIZE );
        rn_arena_destroy( receiver.arena );
    }

    // the block path refuses a message over the limit the same way: three
    // fragments of F cannot add up to at most 1416 bytes
    {
        violation_node_t receiver;
        rocketnet_message_config_t config;
        violation_over_max_config( &config );
        violation_init( &receiver, &config );
        rocketnet_message_record_t record;
        memset( &record, 0, sizeof( record ) );
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        record.block = 1;
        record.fragment = 0;
        record.fragments = 2;
        record.data = data;
        record.bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        check( rocketnet_message_writer_add( &writer, &record ) == 1 );
        const int bytes = rocketnet_message_writer_finish( &writer );
        violation_feed_refuses( &receiver, payload, bytes, ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        rn_arena_destroy( receiver.arena );
    }
}

REGISTER_UNIT_TEST( test_violation_single_message_over_max_message_bytes_is_refused );

// Append one code 3 (RELIABLE_ORDERED) record: flags, length 1, number, ring 0,
// ordering, channel, then its one data byte.
static int violation_put_ordered( uint8_t * p, uint32_t number, uint32_t ordering, uint8_t channel, uint8_t data )
{
    uint8_t * start = p;
    rocketnet_write_uint8( &p, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
    rocketnet_write_uint16( &p, 1 );
    rocketnet_write_uint8( &p, (uint8_t) ( number & 0xff ) );
    rocketnet_write_uint8( &p, (uint8_t) ( ( number >> 8 ) & 0xff ) );
    rocketnet_write_uint8( &p, (uint8_t) ( ( number >> 16 ) & 0xff ) );
    rocketnet_write_uint16( &p, 0 );
    rocketnet_write_uint8( &p, (uint8_t) ( ordering & 0xff ) );
    rocketnet_write_uint8( &p, (uint8_t) ( ( ordering >> 8 ) & 0xff ) );
    rocketnet_write_uint8( &p, (uint8_t) ( ( ordering >> 16 ) & 0xff ) );
    rocketnet_write_uint8( &p, channel );
    rocketnet_write_uint8( &p, data );
    return (int) ( p - start );
}

static int violation_finish_records( uint8_t * p )
{
    *p = ROCKETNET_MESSAGE_FORM_RECORDS;
    return 1;
}

// A hostile sender sends two RELIABLE_ORDERED records with one channel and
// ordering index but different numbers: the second is refused at the held
// index (rn_channels_dispatch, sdk/rocketnet_channel.cpp), counted
// MESSAGE_DUPLICATE_ORDERING and, once for the ended connection,
// MESSAGE_PROTOCOL_VIOLATION.
static void test_violation_two_ordered_records_with_one_ordering_index_get_their_own_counter()
{
    rocketnet_message_config_t config;
    violation_config( &config, 8 );
    violation_node_t receiver;
    violation_init( &receiver, &config );

    uint8_t payload[64];
    uint8_t * p = payload;
    p += violation_put_ordered( p, 0, 5, 0, 0xaa );
    p += violation_put_ordered( p, 1, 5, 0, 0xbb );
    p += violation_finish_records( p );

    check( rn_ordered_receive( &receiver.ordered, payload, (int) ( p - payload ) ) == ROCKETNET_CHANNEL_VIOLATION );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE_ORDERING] == 1 );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION] == 0 );
    check( rn_ordered_decide( &receiver.ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_violation_two_ordered_records_with_one_ordering_index_get_their_own_counter );

// SLOT_COLLISION (check 10) is reachable from the wire: a held record keeps its
// descriptor live after the receive cursor passes its number, so a later
// number one window above lands on the same slot. Record 0 delivers on channel
// 0 and advances R; record 1 waits on channel 1 and keeps slot 1 live; number
// W+1 on channel 2 maps to slot 1 and is refused as SLOT_COLLISION.
static void test_violation_slot_collision_is_reachable_from_the_wire()
{
    rocketnet_message_config_t config;
    violation_config( &config, 8 );
    violation_node_t receiver;
    violation_init( &receiver, &config );

    const uint32_t W = receiver.region->window;

    uint8_t payload[64];
    uint8_t * p = payload;
    p += violation_put_ordered( p, 0, 0, 0, 0x11 );
    p += violation_finish_records( p );
    check( rn_ordered_receive( &receiver.ordered, payload, (int) ( p - payload ) ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    p = payload;
    p += violation_put_ordered( p, 1, 5, 1, 0x22 );
    p += violation_finish_records( p );
    check( rn_ordered_receive( &receiver.ordered, payload, (int) ( p - payload ) ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    p = payload;
    p += violation_put_ordered( p, 1 + W, 0, 2, 0x33 );
    p += violation_finish_records( p );
    check( rn_ordered_receive( &receiver.ordered, payload, (int) ( p - payload ) ) == ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION] == 1 );
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
    check( rn_ordered_decide( &receiver.ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_violation_slot_collision_is_reachable_from_the_wire );

// Finding 7: the three reliable codes refuse the same malformed blocks. The
// shared shape helper (rn_block_shape_valid) counts the shape breaches as
// BAD_BLOCK on all three; the block-span stays BAD_RING on code 2 and 3
// (BAD_BLOCK on code 4), and the added fragment-ring-span check is BAD_BLOCK on
// code 3 and 4. Each record is accepted by layer 3 (its descriptor live) and
// dispatched.
static rocketnet_message_record_t block_shape_record( int code, uint32_t fragments, uint32_t fragment, uint32_t ring, int bytes, uint32_t ordering )
{
    alignas( 16 ) static uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES + 64];
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = code;
    record.block = 1;
    record.fragments = fragments;
    record.fragment = fragment;
    record.ring = ring;
    record.ordering = ordering;
    record.channel = 0;
    record.number = 0;
    record.data = data;
    record.bytes = bytes;
    return record;
}

static void block_shape_refusal( const rocketnet_message_record_t * record, int counter )
{
    violation_node_t node;
    rocketnet_message_config_t config;
    violation_config( &config, 8 );
    violation_init( &node, &config );
    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &node.ordered.receive, record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    const uint64_t before = node.counters.values[counter];
    rn_delivery_t delivery;
    check( rn_channels_dispatch( &node.ordered.channels, record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.counters.values[counter] == before + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rn_arena_destroy( node.arena );
}

static void test_block_shape_refusals_match_across_reliable_codes()
{
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const int codes[3] = { ROCKETNET_MESSAGE_CODE_RELIABLE, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED };

    violation_node_t probe;
    rocketnet_message_config_t pconfig;
    violation_config( &pconfig, 8 );
    violation_init( &probe, &pconfig );
    const uint32_t W = probe.region->window;
    const uint32_t ring_bytes = probe.region->ring_bytes;
    rn_arena_destroy( probe.arena );

    // shape: count 1, count over W, index at count, short non-last, last over F
    const uint32_t fragments[] = { 0, W, 2, 2, 2 };
    const uint32_t fragment[] = { 0, 0, 3, 0, 2 };
    const int bytesv[] = { (int) F, (int) F, (int) F, 100, (int) F + 1 };
    for ( int c = 0; c < 3; c++ )
        for ( int i = 0; i < 5; i++ )
        {
            const uint32_t ordering = codes[c] == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED ? 1u : 0u;
            const rocketnet_message_record_t record = block_shape_record( codes[c], fragments[i], fragment[i], 0, bytesv[i], ordering );
            block_shape_refusal( &record, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        }

    // block span past the ring: BAD_RING on code 2/3, BAD_BLOCK on code 4
    const uint32_t span_ring = ring_bytes / 16 - F / 16;
    for ( int c = 0; c < 3; c++ )
    {
        const uint32_t ordering = codes[c] == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED ? 1u : 0u;
        const rocketnet_message_record_t record = block_shape_record( codes[c], 1, 0, span_ring, (int) F, ordering );
        block_shape_refusal( &record,
                             codes[c] == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED ? ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK : ROCKETNET_COUNTER_MESSAGE_BAD_RING );
    }

    // fragment ring span past the ring: BAD_BLOCK on code 3 and 4, BAD_RING on code 2
    for ( int c = 0; c < 3; c++ )
    {
        const uint32_t ordering = codes[c] == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED ? 1u : 0u;
        const rocketnet_message_record_t record = block_shape_record( codes[c], 1, 0, ring_bytes / 16, (int) F, ordering );
        block_shape_refusal( &record,
                             codes[c] == ROCKETNET_MESSAGE_CODE_RELIABLE ? ROCKETNET_COUNTER_MESSAGE_BAD_RING : ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    }
}

REGISTER_UNIT_TEST( test_block_shape_refusals_match_across_reliable_codes );

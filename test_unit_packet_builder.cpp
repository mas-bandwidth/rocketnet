/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer 6's packet builder, unit tier (docs/CHANNELS.md 9.3; tla/RNChannel.tla
// WritePacket with tla/RNSendBudget.tla). No sockets, no threads, no wall
// clock: time is an argument.

#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_counters.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_packet_builder.h"
#include "sdk/rocketnet_send_budget.h"

static rocketnet_counters_t builder_counters;

static size_t builder_allocs = 0;

static void * builder_malloc( void * context, size_t bytes )
{
    (void) context;
    builder_allocs++;
    return rocketnet_default_malloc_function( NULL, bytes );
}

static void builder_free( void * context, void * p )
{
    (void) context;
    rocketnet_default_free_function( NULL, p );
}

static void builder_item( struct rn_packet_item_t * item, uint16_t channel, int code, int send_class, const uint8_t * data, int bytes )
{
    memset( item, 0, sizeof( *item ) );
    item->channel = channel;
    item->code = (uint8_t) code;
    item->send_class = (uint8_t) send_class;
    item->data = data;
    item->bytes = (uint32_t) bytes;
    item->number = ROCKETNET_PACKET_NUMBER_NONE;
    item->descriptor = RN_DESCRIPTOR_NONE;
}

// a zeroed record for a code, data a static 16-byte buffer: the base record
// the validity refusals copy and corrupt one field of.

static void valid_base_record( struct rocketnet_message_record_t * record, int code )
{
    static uint8_t base_data[16];
    memset( record, 0, sizeof( *record ) );
    record->code = code;
    record->data = base_data;
    record->bytes = 16;
}

// several small records pack into one packet (9.3), each whole, in class order,
// round robin across channels.

static void test_small_records_packed_several_per_packet()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &builder_counters );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, 4, 1191 ) );

    static uint8_t payloads[4][1192];
    uint8_t * frames[4] = { payloads[0], payloads[1], payloads[2], payloads[3] };

    static uint8_t data[4][16];
    memset( data, 0x33, sizeof( data ) );

    struct rn_packet_item_t items[4];
    for ( int i = 0; i < 4; i++ )
        builder_item( &items[i], (uint16_t) i, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, data[i], 16 );

    const int packets = rn_packet_builder_write( &builder, items, 4, frames, 4 );

    // all four small records fit one packet: one write, not four
    rocketnet_unit_check( packets == 1 );
    for ( int i = 0; i < 4; i++ )
        rocketnet_unit_check( items[i].taken == 1 );

    // and the packet reads back as four records of 16 bytes each
    struct rocketnet_message_limits_t limits;
    memset( &limits, 0, sizeof( limits ) );
    limits.num_channels = (int) region->num_channels;
    limits.enabled_codes = rocketnet_message_enabled_codes( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION );
    limits.reliable_messages_in_flight = (int) region->window;
    limits.max_message_bytes = (int) region->max_message_bytes;
    limits.receive_message_buffer_bytes = (int) region->ring_bytes;

    const int payload_bytes = (int) builder.packet[0].payload_bytes;
    rocketnet_unit_check( payload_bytes > 0 );

    struct rocketnet_message_reader_t reader;
    rocketnet_unit_check( rocketnet_message_reader_begin( &reader, frames[0], payload_bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    int records = 0;
    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        rocketnet_unit_check( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        rocketnet_unit_check( record.bytes == 16 );
        rocketnet_unit_check( record.data[0] == 0x33 );
        records++;
    }
    rocketnet_unit_check( records == 4 );

    // a pure-unreliable packet carries no reliable number, so it takes no
    // sent-map entry (I3)
    uint32_t count = 99;
    const uint64_t * numbers = rn_packet_builder_numbers( &builder, 0, &count );
    rocketnet_unit_check( numbers != NULL );
    rocketnet_unit_check( count == 0 );

    rn_arena_destroy( arena );
}

// the builder obeys the budget and its class order: retransmits and ordered
// progress first, then new reliable, then unreliable; a packet the budget
// cannot pay is not written, an unreliable one dropped and counted, a reliable
// one waits (R6).

static void test_builder_obeys_budget_and_class_order()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &builder_counters );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );

    memset( &builder_counters, 0, sizeof( builder_counters ) );

    struct rocketnet_send_budget_t budget;
    const double now = 50.0;
    rocketnet_unit_check( rocketnet_send_budget_init( &budget, 1000, now ) );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, &budget, 4, 1191 ) );

    static uint8_t payloads[4][1192];
    uint8_t * frames[4] = { payloads[0], payloads[1], payloads[2], payloads[3] };

    static uint8_t data[3][32];
    memset( data, 0x77, sizeof( data ) );

    // offered out of order on purpose: the unreliable first, the retransmit last
    struct rn_packet_item_t items[3];
    builder_item( &items[0], 0, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, data[0], 32 );
    builder_item( &items[1], 1, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_SEND_CLASS_NEW_RELIABLE, data[1], 32 );
    items[1].number = 0;
    builder_item( &items[2], 2, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_SEND_CLASS_RETRANSMIT, data[2], 32 );
    items[2].number = 1;

    const int packets = rn_packet_builder_write( &builder, items, 3, frames, 4 );
    rocketnet_unit_check( packets == 1 );

    // the packet reads back in class order: the retransmit, then the new
    // reliable, then the unreliable
    struct rocketnet_message_limits_t limits;
    memset( &limits, 0, sizeof( limits ) );
    limits.num_channels = (int) region->num_channels;
    limits.enabled_codes = rocketnet_message_enabled_codes( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION );
    limits.reliable_messages_in_flight = (int) region->window;
    limits.max_message_bytes = (int) region->max_message_bytes;
    limits.receive_message_buffer_bytes = (int) region->ring_bytes;

    struct rocketnet_message_reader_t reader;
    rocketnet_unit_check( rocketnet_message_reader_begin( &reader, frames[0], (int) builder.packet[0].payload_bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    struct rocketnet_message_record_t record;
    rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
    rocketnet_unit_check( record.channel == 2 );
    rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
    rocketnet_unit_check( record.channel == 1 );
    rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
    rocketnet_unit_check( record.channel == 0 );

    // the packet charged the bucket
    rocketnet_unit_check( budget.bytes_spent > 0 );

    // a budget that cannot pay one packet writes nothing: the unreliable item
    // is dropped and counted, and the reliable one waits, never dropped
    struct rocketnet_send_budget_t poor;
    rocketnet_unit_check( rocketnet_send_budget_init( &poor, ROCKETNET_SEND_BUDGET_MIN_KBPS, now ) );
    poor.tokens = 1.0;                                          // less than any packet's wire bytes

    rn_packet_builder_t starved;
    rocketnet_unit_check( rn_packet_builder_init( &starved, region, NULL, &poor, 4, 1191 ) );

    struct rn_packet_item_t one;
    builder_item( &one, 0, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, data[0], 32 );

    rocketnet_unit_check( rn_packet_builder_write( &starved, &one, 1, frames, 4 ) == 0 );
    rocketnet_unit_check( one.taken == 0 );
    rocketnet_unit_check( starved.unreliable_dropped == 1 );
    rocketnet_unit_check( builder_counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET] == 1 );

    // a reliable item under the same budget waits and is never dropped, and no
    // unreliable item after it spends the bucket
    struct rn_packet_item_t reliable;
    builder_item( &reliable, 0, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_SEND_CLASS_NEW_RELIABLE, data[1], 32 );
    reliable.number = 0;

    rocketnet_unit_check( rn_packet_builder_write( &starved, &reliable, 1, frames, 4 ) == 0 );
    rocketnet_unit_check( reliable.taken == 0 );
    rocketnet_unit_check( poor.bytes_spent == 0 );                       // nothing was spent: the item waits

    rn_arena_destroy( arena );
}

// R2's acceptance: a counting allocator is installed, the connection is
// created, the builder and the dispatch run, and the allocator is not called
// again after create.

static void test_packet_builder_no_allocation_after_create()
{
    rocketnet_term();                                           // the allocator is replaced only before init
    rocketnet_unit_check( rocketnet_init( NULL, builder_malloc, builder_free ) == ROCKETNET_STATUS_OK );

    builder_allocs = 0;
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &builder_counters );
    rocketnet_unit_check( arena != NULL );
    const size_t after_create = builder_allocs;
    rocketnet_unit_check( after_create > 0 );                   // create is the one allocation

    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, 4, 1191 ) );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    static uint8_t payloads[4][1192];
    uint8_t * frames[4] = { payloads[0], payloads[1], payloads[2], payloads[3] };
    static uint8_t data[2][16];

    struct rn_packet_item_t items[2];

    // the hot path: write packets, dispatch records, feed acks, end the update.
    // each update queues its items afresh: the builder skips an item it marked
    // taken (rn_builder_writable, sdk/rocketnet_packet_builder.cpp:131).
    for ( int update = 0; update < 8; update++ )
    {
        for ( int i = 0; i < 2; i++ )
            builder_item( &items[i], (uint16_t) i, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, data[i], 16 );

        rocketnet_unit_check( rn_packet_builder_write( &builder, items, 2, frames, 4 ) == 1 );

        struct rocketnet_message_record_t record;
        memset( &record, 0, sizeof( record ) );
        record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        record.channel = (uint32_t) ( update % 4 );
        record.data = payloads[0] + 3;
        record.bytes = 16;

        struct rn_delivery_t delivery;
        rn_channels_dispatch( &channels, &record, &delivery );

        const uint64_t acked[1] = { (uint64_t) update };
        rn_channels_feed_acks( NULL, acked, 1, 1.0 + update );

        rn_channels_end_update( &channels );
    }

    // zero allocator calls after create
    rocketnet_unit_check( builder_allocs == after_create );

    rn_arena_destroy( arena );
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// rocketnet_message_record_valid (sdk/rocketnet_message.cpp) is the writer's
// gate: absent fields are zero, present ones inside their wire widths, a block
// has at least two fragments. Only valid records reach it in the suite, so
// this test hands it each malformed shape and checks it is refused.

static void test_message_record_valid_refuses_each_shape()
{
    // NULL is refused
    rocketnet_unit_check( !rocketnet_message_record_valid( NULL ) );

    // a zeroed base record is valid for every code 0..NUM_CODES-1
    for ( int code = 0; code < ROCKETNET_MESSAGE_NUM_CODES; code++ )
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, code );
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 1 );
    }

    // code -1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, -1 );
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code ROCKETNET_MESSAGE_NUM_CODES
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_NUM_CODES );
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // bytes 0
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        record.bytes = 0;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // bytes ROCKETNET_MESSAGE_MAX_RECORD_BYTES + 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        record.bytes = ROCKETNET_MESSAGE_MAX_RECORD_BYTES + 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // data NULL
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        record.data = NULL;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code with a number (RELIABLE): number = U24_MASK + 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE );
        record.number = ROCKETNET_MESSAGE_U24_MASK + 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code without a number (UNRELIABLE): number 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        record.number = 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code with sequencing (UNRELIABLE_SEQUENCED): sequencing = U24_MASK + 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED );
        record.sequencing = ROCKETNET_MESSAGE_U24_MASK + 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code without sequencing (UNRELIABLE): sequencing 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        record.sequencing = 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code with a channel (UNRELIABLE_SEQUENCED): ordering = U24_MASK + 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED );
        record.ordering = ROCKETNET_MESSAGE_U24_MASK + 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code with a channel: channel = 0x10000
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED );
        record.channel = 0x10000;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code without a channel (UNRELIABLE): channel 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        record.channel = 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // holdable code (RELIABLE_ORDERED): ring = 0x10000
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
        record.ring = 0x10000;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // code neither holdable nor a block (UNRELIABLE): ring 1
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        record.ring = 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // fragments 1 with block 0 (RELIABLE)
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE );
        record.fragments = 1;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // block 1 with fragments 0
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE );
        record.block = 1;
        record.fragments = 0;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }

    // block 1 with fragments 1 and fragment 2
    {
        struct rocketnet_message_record_t record;
        valid_base_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE );
        record.block = 1;
        record.fragments = 1;
        record.fragment = 2;
        rocketnet_unit_check( rocketnet_message_record_valid( &record ) == 0 );
    }
}

REGISTER_UNIT_TEST( test_small_records_packed_several_per_packet );
REGISTER_UNIT_TEST( test_builder_obeys_budget_and_class_order );
REGISTER_UNIT_TEST( test_packet_builder_no_allocation_after_create );
REGISTER_UNIT_TEST( test_message_record_valid_refuses_each_shape );

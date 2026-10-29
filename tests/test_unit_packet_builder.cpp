/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer 6's packet builder, unit tier (docs/CHANNELS.md 9.3; proofs/tla/RNChannel.tla
// WritePacket with proofs/tla/RNSendBudget.tla). No sockets, no threads, no wall
// clock: time is an argument.

#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_packet_builder.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_test_seams.h"

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

#if ROCKETNET_DEV
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
    {
        builder_item( &items[i], (uint16_t) i, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, data[i], 16 );
    }

    const int packets = rn_packet_builder_write( &builder, items, 4, frames, 4 );

    // all four small records fit one packet: one write, not four

    rocketnet_unit_check( packets == 1 );
    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_unit_check( items[i].taken == 1 );
    }

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
#endif // #if ROCKETNET_DEV

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
        {
            builder_item( &items[i], (uint16_t) i, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, data[i], 16 );
        }

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

// The pick order is proofs/tla/RNScheduler.tla's IsHead, Boost, PickLevel and PickIn.
// What follows is BASE's picker (sdk/rocketnet_packet_builder.cpp before the
// linear class pass), kept here verbatim in its logic: for every item, a scan
// of every other item for the channel's head and boost, quadratic in the plan.
// The builder must take items in exactly the order this takes them.

struct reference_scheduler_t
{
    uint64_t level_clock[ROCKETNET_PACKET_BUILDER_LEVELS];
    uint64_t clock_base;
    uint32_t round_robin;
    uint32_t num_channels;
    int packets_per_update;
    int payload_capacity;
    uint64_t visits;                                            // the items the picker looked at, the quadratic cost
};

static const uint64_t reference_stride[ROCKETNET_PACKET_BUILDER_LEVELS] = { 1, 5, 14, 35 };
static const uint64_t reference_seed[ROCKETNET_PACKET_BUILDER_LEVELS] = { 0, 3, 10, 27 };

static void reference_reset_clocks( struct reference_scheduler_t * scheduler )
{
    for ( int p = 0; p < ROCKETNET_PACKET_BUILDER_LEVELS; p++ )
    {
        scheduler->level_clock[p] = reference_seed[p];
    }
    scheduler->clock_base = 0;
}

static bool reference_writable( const struct rn_packet_item_t * item )
{
    return !item->taken && item->bytes >= 1 && item->data && item->priority < ROCKETNET_PACKET_BUILDER_LEVELS;
}

static uint64_t reference_effective_clock( const struct reference_scheduler_t * scheduler, int level )
{
    const uint64_t clock = scheduler->level_clock[level];
    return clock > scheduler->clock_base ? clock : scheduler->clock_base;
}

static int reference_next_in_class( struct reference_scheduler_t * scheduler, const struct rn_packet_item_t * items, int num_items, int send_class, int * out_level )
{
    const uint32_t channels = scheduler->num_channels > 0 ? scheduler->num_channels : 1;

    int best = -1;
    int best_level = 0;
    uint64_t best_clock = 0;
    uint32_t best_distance = 0;

    for ( int i = 0; i < num_items; i++ )
    {
        scheduler->visits++;
        if ( items[i].send_class != (uint8_t) send_class || !reference_writable( &items[i] ) )
            continue;

        bool head = true;
        int level = items[i].priority;

        for ( int j = 0; j < num_items; j++ )
        {
            scheduler->visits++;
            if ( j == i || items[j].channel != items[i].channel || items[j].send_class != (uint8_t) send_class || !reference_writable( &items[j] ) )
                continue;

            if ( j < i )
            {
                head = false;
                break;
            }

            if ( items[j].priority < level )
            {
                level = items[j].priority;
            }
        }

        if ( !head )
            continue;

        const uint64_t clock = reference_effective_clock( scheduler, level );
        const uint32_t distance = ( items[i].channel % channels + channels - scheduler->round_robin % channels - 1 ) % channels;
        if ( best < 0 || clock < best_clock || ( clock == best_clock && level < best_level ) ||
             ( clock == best_clock && level == best_level && distance < best_distance ) )
        {
            best = i;
            best_level = level;
            best_clock = clock;
            best_distance = distance;
        }
    }

    *out_level = best_level;

    return best;
}

static void reference_take_level( struct reference_scheduler_t * scheduler, int level )
{
    const uint64_t clock = reference_effective_clock( scheduler, level );
    scheduler->clock_base = clock;
    scheduler->level_clock[level] = clock + reference_stride[level];
}

// BASE's rn_packet_builder_write with no budget (the budget's answers are the
// other tests' business): the class loop, the packet limits, the fit, the
// clock step and Tick's reset. The plans below are code 0 (UNRELIABLE), whose
// record carries its length alone, so the record is the item's bytes.

static int reference_write( struct reference_scheduler_t * scheduler, struct rn_packet_item_t * items, int num_items, uint8_t ** payloads, int num_payloads, uint32_t * out_payload_bytes )
{
    int packets = 0;
    const int limit = scheduler->packets_per_update < num_payloads ? scheduler->packets_per_update : num_payloads;
    for ( int p = 0; p < limit; p++ )
    {
        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payloads[p], scheduler->payload_capacity );

        uint32_t count = 0;
        uint32_t records = 0;

        for ( int send_class = ROCKETNET_SEND_CLASS_RETRANSMIT; send_class <= ROCKETNET_SEND_CLASS_UNRELIABLE; send_class++ )
        {
            for ( ;; )
            {
                int level = 0;
                const int i = reference_next_in_class( scheduler, items, num_items, send_class, &level );
                if ( i < 0 )
                    break;

                if ( records >= ROCKETNET_PACKET_BUILDER_MAX_RECORDS ||
                     ( items[i].number != ROCKETNET_PACKET_NUMBER_NONE && count >= ROCKETNET_PACKET_BUILDER_MAX_NUMBERS ) )
                    goto packet_done;

                if ( items[i].alone && records > 0 )
                    goto packet_done;

                struct rocketnet_message_record_t record;
                memset( &record, 0, sizeof( record ) );
                record.code = items[i].code;
                record.data = items[i].data;
                record.bytes = (int) items[i].bytes;
                if ( !rocketnet_message_writer_add( &writer, &record ) )
                {
                    items[i].taken = 0;
                    goto packet_done;
                }

                items[i].taken = 1;
                records++;

                if ( items[i].number != ROCKETNET_PACKET_NUMBER_NONE && count < ROCKETNET_PACKET_BUILDER_MAX_NUMBERS )
                {
                    count++;
                }

                scheduler->round_robin = items[i].channel;
                reference_take_level( scheduler, level );

                if ( items[i].alone )
                    goto packet_done;
            }
        }
    packet_done:

        const int payload_bytes = rocketnet_message_writer_finish( &writer );
        if ( payload_bytes <= 0 )
            break;

        out_payload_bytes[p] = (uint32_t) payload_bytes;
        packets++;
    }

    bool drained = true;
    for ( int i = 0; i < num_items && drained; i++ )
        if ( reference_writable( &items[i] ) )
            drained = false;
    if ( drained )
    {
        reference_reset_clocks( scheduler );
    }

    return packets;
}

static uint32_t plan_random_state;

static uint32_t plan_random()
{
    plan_random_state ^= plan_random_state << 13;
    plan_random_state ^= plan_random_state >> 17;
    plan_random_state ^= plan_random_state << 5;
    return plan_random_state;
}

#define PLAN_MAX_ITEMS          ROCKETNET_PACKET_BUILDER_MAX_ITEMS
#define PLAN_PAYLOADS           ROCKETNET_PACKET_BUILDER_MAX_PACKETS
#define PLAN_PAYLOAD_BYTES      1191

static uint8_t plan_data[PLAN_MAX_ITEMS][256];
static uint8_t plan_builder_frames[PLAN_PAYLOADS][PLAN_PAYLOAD_BYTES + 1];
static uint8_t plan_reference_frames[PLAN_PAYLOADS][PLAN_PAYLOAD_BYTES + 1];
static struct rn_packet_item_t plan_builder_items[PLAN_MAX_ITEMS];
static struct rn_packet_item_t plan_reference_items[PLAN_MAX_ITEMS];

// one seeded random plan: channels below the modulus and a few at or above it
// (the round robin's distance takes the channel mod the modulus, the channel
// FIFO the channel itself), the four classes, the four priorities and one out
// of range, some items already taken or empty, numbered items to reach the
// packet's number limit, and a few that travel alone. Every record's first two
// bytes are its index, so two equal frames took the same items in the same
// order.

static void plan_fill( int num_items, uint32_t channels )
{
    for ( int i = 0; i < num_items; i++ )
    {
        const uint32_t roll = plan_random() % 100;
        uint16_t channel = (uint16_t) ( plan_random() % channels );
        if ( roll < 3 )
        {
            channel = (uint16_t) ( channels + plan_random() % 3 );
        }

        const int send_class = (int) ( plan_random() % 4 );
        const int bytes = 2 + (int) ( plan_random() % 120 );
        uint8_t * data = plan_data[i];
        data[0] = (uint8_t) ( i & 0xFF );
        data[1] = (uint8_t) ( i >> 8 );
        for ( int b = 2; b < bytes; b++ )
        {
            data[b] = (uint8_t) ( i + b );
        }

        struct rn_packet_item_t * item = &plan_builder_items[i];
        builder_item( item, channel, ROCKETNET_MESSAGE_CODE_UNRELIABLE, send_class, data, bytes );
        item->priority = (uint8_t) ( plan_random() % 4 );
        if ( plan_random() % 50 == 0 )
            item->priority = 4;                                 // never taken
        if ( plan_random() % 40 == 0 )
            item->taken = 1;                                    // already taken
        if ( plan_random() % 60 == 0 )
            item->bytes = 0;                                    // nothing to write
        if ( plan_random() % 3 == 0 )
            item->number = (uint64_t) i;                        // numbered: the packet's number limit
        if ( plan_random() % 30 == 0 )
        {
            item->alone = 1;
        }

        plan_reference_items[i] = *item;
    }
}

// the builder and BASE's picker take every plan's items in the same order:
// the same frames byte for byte, the same items taken, the same clocks and
// round robin after every write, write after write until the plan drains.

static void test_packet_builder_matches_reference_order()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &builder_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );

    plan_random_state = 0x2545F491u;

    uint8_t * builder_frames[PLAN_PAYLOADS];
    uint8_t * reference_frames[PLAN_PAYLOADS];

    for ( int p = 0; p < PLAN_PAYLOADS; p++ )
    {
        builder_frames[p] = plan_builder_frames[p];
        reference_frames[p] = plan_reference_frames[p];
    }

    static const int sizes[] = { 1, 2, 5, 17, 32, 64, 95, ROCKETNET_PACKET_BUILDER_MAX_ITEMS };
    static const uint32_t moduli[] = { 1, 2, 3, 8, 32, 65536 };
    static const int packet_counts[] = { 1, 4, 16 };

    const int num_sizes = (int) ( sizeof( sizes ) / sizeof( sizes[0] ) );
    const int num_moduli = (int) ( sizeof( moduli ) / sizeof( moduli[0] ) );
    const int num_packet_counts = (int) ( sizeof( packet_counts ) / sizeof( packet_counts[0] ) );

    for ( int round = 0; round < 60; round++ )
    {
        const int num_items = sizes[round % num_sizes];
        const uint32_t modulus = moduli[( round / 3 ) % num_moduli];
        const int packets_per_update = packet_counts[round % num_packet_counts];

        rn_packet_builder_t builder;
        rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, packets_per_update, PLAN_PAYLOAD_BYTES ) );
        builder.num_channels = round % 7 == 6 ? 0 : modulus;    // 0: every channel one turn, the round robin's modulus 1
        builder.round_robin = builder.num_channels > 0 ? builder.num_channels - 1 : 0;

        struct reference_scheduler_t reference;
        memset( &reference, 0, sizeof( reference ) );
        reference.num_channels = builder.num_channels;
        reference.round_robin = builder.round_robin;
        reference.packets_per_update = builder.packets_per_update;
        reference.payload_capacity = PLAN_PAYLOAD_BYTES;
        reference_reset_clocks( &reference );

        plan_fill( num_items, modulus + 1 );

        for ( int write = 0; write < 64; write++ )
        {
            uint32_t reference_payload_bytes[PLAN_PAYLOADS];
            const int reference_packets = reference_write( &reference, plan_reference_items, num_items, reference_frames, PLAN_PAYLOADS, reference_payload_bytes );
            const int builder_packets = rn_packet_builder_write( &builder, plan_builder_items, num_items, builder_frames, PLAN_PAYLOADS );

            rocketnet_unit_check( builder_packets == reference_packets );

            for ( int p = 0; p < builder_packets; p++ )
            {
                rocketnet_unit_check( builder.packet[p].payload_bytes == reference_payload_bytes[p] );
                rocketnet_unit_check( memcmp( builder_frames[p], reference_frames[p], reference_payload_bytes[p] ) == 0 );
            }

            for ( int i = 0; i < num_items; i++ )
            {
                rocketnet_unit_check( plan_builder_items[i].taken == plan_reference_items[i].taken );
            }
            for ( int l = 0; l < ROCKETNET_PACKET_BUILDER_LEVELS; l++ )
            {
                rocketnet_unit_check( builder.level_clock[l] == reference.level_clock[l] );
            }
            rocketnet_unit_check( builder.clock_base == reference.clock_base );
            rocketnet_unit_check( builder.round_robin == reference.round_robin );

            if ( builder_packets == 0 )
                break;

            // the application queues more between updates: a few new items
            // replace taken ones, so the clocks and the round robin carry over

            for ( int i = 0; i < num_items; i++ )
            {
                if ( plan_builder_items[i].taken && plan_random() % 8 == 0 )
                {
                    plan_builder_items[i].taken = 0;
                    plan_builder_items[i].priority = (uint8_t) ( plan_random() % 4 );
                    plan_reference_items[i] = plan_builder_items[i];
                }
            }
        }
    }

    rn_arena_destroy( arena );
}

// the builder's plan cost is linear: one item scan, six stable radix touches,
// one epoch/index tag and one suffix-minimum step per writable item. Selection
// inspects one contiguous group head per pick, never the items of that group.
// BASE's picker, counted the same way, is quadratic in the plan per pick.

#if ROCKETNET_DEV
static void test_packet_builder_operations_bounded()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &builder_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );

    plan_random_state = 0x9E3779B9u;

    uint8_t * builder_frames[PLAN_PAYLOADS];
    uint8_t * reference_frames[PLAN_PAYLOADS];

    for ( int p = 0; p < PLAN_PAYLOADS; p++ )
    {
        builder_frames[p] = plan_builder_frames[p];
        reference_frames[p] = plan_reference_frames[p];
    }

    const uint32_t channels = 8;
    static const int sizes[] = { 32, 64, ROCKETNET_PACKET_BUILDER_MAX_ITEMS };
    static const int packet_counts[] = { 1, 16 };

    for ( int s = 0; s < 3; s++ )
    {
        for ( int c = 0; c < 2; c++ )
        {
            const int num_items = sizes[s];
            const int packets_per_update = packet_counts[c];

            rn_packet_builder_t builder;
            rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, packets_per_update, PLAN_PAYLOAD_BYTES ) );
            builder.num_channels = channels;
            builder.round_robin = channels - 1;

            struct reference_scheduler_t reference;
            memset( &reference, 0, sizeof( reference ) );
            reference.num_channels = channels;
            reference.round_robin = channels - 1;
            reference.packets_per_update = builder.packets_per_update;
            reference.payload_capacity = PLAN_PAYLOAD_BYTES;
            reference_reset_clocks( &reference );

            plan_fill( num_items, channels );       // channels 0 .. 10, below 32: no link slot is shared

            uint32_t reference_payload_bytes[PLAN_PAYLOADS];
            const int reference_packets = reference_write( &reference, plan_reference_items, num_items, reference_frames, PLAN_PAYLOADS, reference_payload_bytes );
            const int packets = rn_packet_builder_write( &builder, plan_builder_items, num_items, builder_frames, PLAN_PAYLOADS );
            rocketnet_unit_check( packets == reference_packets );
            rocketnet_unit_check( packets >= 1 );

            uint64_t taken = 0;
            for ( int i = 0; i < num_items; i++ )
            {
                rocketnet_unit_check( plan_builder_items[i].taken == plan_reference_items[i].taken );
            }
            for ( int p = 0; p < packets; p++ )
            {
                taken += builder.packet[p].records;
            }
            rocketnet_unit_check( taken >= 1 );

            const struct rn_packet_builder_plan_t * plan = rn_packet_builder_test_last_plan();
            const uint64_t groups = plan->group_count;
            rocketnet_unit_check( plan->plan_operations <= (uint64_t) num_items * 9 );
            rocketnet_unit_check( plan->selection_operations <= ( taken + (uint64_t) packets_per_update ) * groups );

            // at the maximum production plan, grouping plus selection costs
            // under one tenth of BASE's repeated item scans

            if ( num_items == ROCKETNET_PACKET_BUILDER_MAX_ITEMS )
            {
                rocketnet_unit_check( ( plan->plan_operations + plan->selection_operations ) * 10 < reference.visits );
            }
        }
    }

    rn_arena_destroy( arena );
}
#endif // #if ROCKETNET_DEV

// The flat plan is tagged by the call's full-width epoch and exact caller
// item index. Channel/class groups are contiguous across sparse channels all
// the way through 65535, their suffix levels are exact, an earlier larger
// call cannot leave a live entry in a later smaller one, and neither capacity
// nor epoch exhaustion is silently truncated or wrapped.

static void test_packet_builder_flat_plan_tags_ranges_and_bounds()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &builder_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, 1, PLAN_PAYLOAD_BYTES ) );
    builder.num_channels = 65536;
    builder.round_robin = 65535;

    static const uint16_t channels[] = { 65535, 0, 32768, 256, 255, 65535, 0, 32768 };
    struct rn_packet_item_t items[8];

    for ( int i = 0; i < 8; i++ )
    {
        builder_item( &items[i], channels[i], ROCKETNET_MESSAGE_CODE_UNRELIABLE, i % 4, plan_data[i], 2 );
        items[i].priority = (uint8_t) ( 3 - i % 4 );
        plan_data[i][0] = (uint8_t) i;
        plan_data[i][1] = (uint8_t) ( i + 1 );
    }

    uint8_t * frames[1] = { plan_builder_frames[0] };
    rocketnet_unit_check( rn_packet_builder_write( &builder, items, 8, frames, 1 ) == 1 );
    rocketnet_unit_check( builder.call_epoch == 1 );
#if ROCKETNET_DEV
    const struct rn_packet_builder_plan_t * plan = rn_packet_builder_test_last_plan();
    rocketnet_unit_check( plan->group_count == 8 );

    for ( uint32_t g = 0; g < plan->group_count; g++ )
    {
        const struct rn_packet_builder_group_t * group = &plan->groups[g];
        rocketnet_unit_check( group->begin < group->end );
        rocketnet_unit_check( group->end <= 8 );

        uint8_t suffix = ROCKETNET_PACKET_BUILDER_LEVELS;

        for ( uint32_t p = group->end; p > group->begin; p-- )
        {
            const struct rn_packet_builder_sequence_t * entry = &plan->sequence[p - 1];
            rocketnet_unit_check( entry->call_epoch == builder.call_epoch );
            rocketnet_unit_check( entry->item_index < 8 );
            rocketnet_unit_check( entry->group_index == g );

            const struct rn_packet_item_t * item = &items[entry->item_index];
            rocketnet_unit_check( item->send_class == group->send_class );
            rocketnet_unit_check( item->channel == group->channel );
            suffix = item->priority < suffix ? item->priority : suffix;
            rocketnet_unit_check( entry->suffix_level == suffix );
        }
    }
#endif // #if ROCKETNET_DEV

    // a one-item next call advances the epoch and exposes only its one group;
    // every slot outside that range remains invalid for the current epoch

    builder_item( &items[0], 65535, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_NEW_RELIABLE, plan_data[0], 2 );
    rocketnet_unit_check( rn_packet_builder_write( &builder, items, 1, frames, 1 ) == 1 );
    rocketnet_unit_check( builder.call_epoch == 2 );
#if ROCKETNET_DEV
    plan = rn_packet_builder_test_last_plan();
    rocketnet_unit_check( plan->group_count == 1 );
    rocketnet_unit_check( plan->sequence[0].call_epoch == 2 );
    rocketnet_unit_check( plan->sequence[0].item_index == 0 );
    rocketnet_unit_check( plan->sequence[1].call_epoch != builder.call_epoch );
#endif // #if ROCKETNET_DEV

    struct rn_packet_item_t over[ROCKETNET_PACKET_BUILDER_MAX_ITEMS + 1];
    for ( int i = 0; i <= ROCKETNET_PACKET_BUILDER_MAX_ITEMS; i++ )
    {
        builder_item( &over[i], (uint16_t) i, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, plan_data[i % PLAN_MAX_ITEMS], 2 );
    }

    const uint64_t before_over = builder.call_epoch;
    rocketnet_unit_check( rn_packet_builder_write( &builder, over, ROCKETNET_PACKET_BUILDER_MAX_ITEMS + 1, frames, 1 ) == 0 );
    rocketnet_unit_check( builder.call_epoch == before_over );
    for ( int i = 0; i <= ROCKETNET_PACKET_BUILDER_MAX_ITEMS; i++ )
    {
        rocketnet_unit_check( !over[i].taken );
    }

    builder.call_epoch = UINT64_MAX;
    items[0].taken = 0;
    rocketnet_unit_check( rn_packet_builder_write( &builder, items, 1, frames, 1 ) == 0 );
    rocketnet_unit_check( builder.call_epoch == UINT64_MAX );
    rocketnet_unit_check( !items[0].taken );

    rn_arena_destroy( arena );
}

#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_small_records_packed_several_per_packet );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_builder_obeys_budget_and_class_order );
REGISTER_UNIT_TEST( test_packet_builder_no_allocation_after_create );
REGISTER_UNIT_TEST( test_message_record_valid_refuses_each_shape );
REGISTER_UNIT_TEST( test_packet_builder_matches_reference_order );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_packet_builder_operations_bounded );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_packet_builder_flat_plan_tags_ranges_and_bounds );

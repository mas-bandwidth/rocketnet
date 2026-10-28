/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer 6's scheduler, unit tier: the four send priorities inside the
// budget's class order (docs/CHANNELS.md 9.2, tla/RNScheduler.tla). No
// sockets, no threads, no wall clock.
//
// Every case writes packets that hold one record each, so the order of the
// packets is the order the scheduler took the items. Each record's first byte
// is its item's index.

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

#define PRIORITIES_RECORD_BYTES     40          // one record per packet at PRIORITIES_CAPACITY
#define PRIORITIES_CAPACITY         64
#define PRIORITIES_MAX_ITEMS        64

static rocketnet_counters_t priorities_counters;

static uint8_t priorities_data[PRIORITIES_MAX_ITEMS][PRIORITIES_RECORD_BYTES];
static uint8_t priorities_payloads[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][PRIORITIES_CAPACITY];

static void priorities_item( struct rn_packet_item_t * item, int index, uint16_t channel, int send_class, int priority )
{
    memset( priorities_data[index], 0, PRIORITIES_RECORD_BYTES );
    priorities_data[index][0] = (uint8_t) index;

    memset( item, 0, sizeof( *item ) );
    item->channel = channel;
    item->code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;              // carries no field but its length: the index rides in the bytes
    item->send_class = (uint8_t) send_class;
    item->priority = (uint8_t) priority;
    item->data = priorities_data[index];
    item->bytes = PRIORITIES_RECORD_BYTES;
    item->number = ROCKETNET_PACKET_NUMBER_NONE;
    item->descriptor = RN_DESCRIPTOR_NONE;
}

// the index each written packet's one record carries, in packet order

static int priorities_read( const rn_region_t * region, const rn_packet_builder_t * builder, int packets, int * order, int count, int max )
{
    struct rocketnet_message_limits_t limits;
    memset( &limits, 0, sizeof( limits ) );
    limits.num_channels = (int) region->num_channels;
    limits.enabled_codes = rocketnet_message_enabled_codes( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION );
    limits.reliable_messages_in_flight = (int) region->window;
    limits.max_message_bytes = (int) region->max_message_bytes;
    limits.receive_message_buffer_bytes = (int) region->ring_bytes;

    for ( int p = 0; p < packets; p++ )
    {
        rocketnet_unit_check( builder->packet[p].records == 1 );

        struct rocketnet_message_reader_t reader;
        rocketnet_unit_check( rocketnet_message_reader_begin( &reader, priorities_payloads[p], (int) builder->packet[p].payload_bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

        struct rocketnet_message_record_t record;
        rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
        rocketnet_unit_check( record.bytes == PRIORITIES_RECORD_BYTES );
        rocketnet_unit_check( count < max );

        order[count++] = record.data[0];
    }

    return count;
}

// runs updates until every item is taken; returns the take order's length

static int priorities_drain( rn_region_t * region, rn_packet_builder_t * builder, struct rn_packet_item_t * items, int num_items, int * order, int max )
{
    uint8_t * frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int p = 0; p < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; p++ )
    {
        frames[p] = priorities_payloads[p];
    }

    int count = 0;
    for ( int update = 0; update < 64; update++ )
    {
        const int packets = rn_packet_builder_write( builder, items, num_items, frames, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
        if ( packets == 0 )
            break;
        count = priorities_read( region, builder, packets, order, count, max );
    }

    return count;
}

static int priorities_position( const int * order, int count, int index )
{
    for ( int i = 0; i < count; i++ )
        if ( order[i] == index )
            return i;
    return -1;
}

// within one class the more urgent level goes first, whatever the channel
// order: IMMEDIATE, then HIGH, then LOW.

static void test_higher_priority_first_within_class()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &priorities_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );
    rocketnet_unit_check( region->num_channels >= 3 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, PRIORITIES_CAPACITY ) );

    struct rn_packet_item_t items[3];
    priorities_item( &items[0], 0, 0, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_IMMEDIATE );
    priorities_item( &items[1], 1, 1, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_LOW );
    priorities_item( &items[2], 2, 2, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_HIGH );

    int order[8];
    rocketnet_unit_check( priorities_drain( region, &builder, items, 3, order, 8 ) == 3 );
    rocketnet_unit_check( order[0] == 0 );
    rocketnet_unit_check( order[1] == 2 );
    rocketnet_unit_check( order[2] == 1 );

    rn_arena_destroy( arena );
}

// a channel's records are first-written in the order they were queued. An
// IMMEDIATE send behind a LOW one boosts the whole channel: the LOW record goes
// first, at IMMEDIATE, then the IMMEDIATE one, both ahead of another channel's
// HIGH record (17.3 item 3; RNScheduler FirstWriteInOrderPerChannel).

static void test_channel_fifo_kept_per_channel()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &priorities_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, PRIORITIES_CAPACITY ) );

    struct rn_packet_item_t items[3];
    priorities_item( &items[0], 0, 0, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_LOW );
    priorities_item( &items[1], 1, 0, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_IMMEDIATE );
    priorities_item( &items[2], 2, 1, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_HIGH );

    int order[8];
    rocketnet_unit_check( priorities_drain( region, &builder, items, 3, order, 8 ) == 3 );
    rocketnet_unit_check( order[0] == 0 );
    rocketnet_unit_check( order[1] == 1 );
    rocketnet_unit_check( order[2] == 2 );

    rn_arena_destroy( arena );
}

// within one level, channels take turns, one record each, starting after the
// channel the last record came from (17.3 item 4).

static void test_round_robin_across_channels()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &priorities_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );
    rocketnet_unit_check( region->num_channels >= 3 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, PRIORITIES_CAPACITY ) );

    // channel 0 queues 0 and 1, channel 1 queues 2 and 3, channel 2 queues 4
    // and 5, all MEDIUM

    struct rn_packet_item_t items[6];
    for ( int i = 0; i < 6; i++ )
    {
        priorities_item( &items[i], i, (uint16_t) ( i / 2 ), ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_MEDIUM );
    }

    int order[8];
    rocketnet_unit_check( priorities_drain( region, &builder, items, 6, order, 8 ) == 6 );

    const int expected[6] = { 0, 2, 4, 1, 3, 5 };

    for ( int i = 0; i < 6; i++ )
    {
        rocketnet_unit_check( order[i] == expected[i] );
    }

    rn_arena_destroy( arena );
}

// a LOW channel is not starved by an IMMEDIATE backlog: the level clocks give
// it a turn once IMMEDIATE's clock passes LOW's seed of 27, and its next turn
// one LOW stride (35) later. The backlog is still queued when LOW goes.

static void test_no_starvation_of_low()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &priorities_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, PRIORITIES_CAPACITY ) );

    // 0 to 39 IMMEDIATE on channel 0, 40 and 41 LOW on channel 1

    struct rn_packet_item_t items[42];
    for ( int i = 0; i < 40; i++ )
    {
        priorities_item( &items[i], i, 0, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_IMMEDIATE );
    }

    priorities_item( &items[40], 40, 1, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_LOW );
    priorities_item( &items[41], 41, 1, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_LOW );

    int order[PRIORITIES_MAX_ITEMS];
    rocketnet_unit_check( priorities_drain( region, &builder, items, 42, order, PRIORITIES_MAX_ITEMS ) == 42 );

    // IMMEDIATE's clock runs 0..27 before LOW's seed of 27 is the smallest
    // (a tie goes to the more urgent level): 28 IMMEDIATE, then LOW

    rocketnet_unit_check( priorities_position( order, 42, 40 ) == 28 );

    // with 12 IMMEDIATE still queued, LOW's next clock is 27 + 35 = 62, past
    // the backlog: the second LOW goes last

    rocketnet_unit_check( priorities_position( order, 42, 41 ) == 41 );

    // channel FIFO inside the IMMEDIATE backlog

    int last = -1;
    for ( int i = 0; i < 42; i++ )
    {
        if ( order[i] >= 40 )
            continue;
        rocketnet_unit_check( order[i] == last + 1 );

        last = order[i];
    }

    // the queue drained: the clocks are back at their seeds

    rocketnet_unit_check( builder.level_clock[ROCKETNET_PRIORITY_IMMEDIATE] == 0 );
    rocketnet_unit_check( builder.level_clock[ROCKETNET_PRIORITY_LOW] == 27 );

    rn_arena_destroy( arena );
}

// the budget's class order is the outer rule: a LOW retransmit goes before a
// MEDIUM ordered-progress record, before a HIGH new reliable one, before an
// IMMEDIATE unreliable one (R6; RNScheduler ClassOrderBeforePriority).

static void test_budget_class_order_still_rules()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &priorities_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, PRIORITIES_CAPACITY ) );

    struct rn_packet_item_t items[4];
    priorities_item( &items[0], 0, 0, ROCKETNET_SEND_CLASS_UNRELIABLE, ROCKETNET_PRIORITY_IMMEDIATE );
    priorities_item( &items[1], 1, 1, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_HIGH );
    priorities_item( &items[2], 2, 2, ROCKETNET_SEND_CLASS_ORDERED_PROGRESS, ROCKETNET_PRIORITY_MEDIUM );
    priorities_item( &items[3], 3, 0, ROCKETNET_SEND_CLASS_RETRANSMIT, ROCKETNET_PRIORITY_LOW );

    int order[8];
    rocketnet_unit_check( priorities_drain( region, &builder, items, 4, order, 8 ) == 4 );
    rocketnet_unit_check( order[0] == 3 );
    rocketnet_unit_check( order[1] == 2 );
    rocketnet_unit_check( order[2] == 1 );
    rocketnet_unit_check( order[3] == 0 );

    rn_arena_destroy( arena );
}

// the two reversed witnesses of tla/RNScheduler.tla, replayed against the
// code on MCRNScheduler's instance: channel 0 queues LOW then IMMEDIATE,
// channel 1 queues HIGH, IMMEDIATE unreliable items arrive one per update, and
// the budget is one record per update.
//
// BrokenPerMessagePriority's trace first-writes channel 0's IMMEDIATE record
// (ordering 1) ahead of its LOW one (ordering 0). The code writes ordering 0
// first.
//
// BrokenUnreliableStarvesOrdered's trace spends every update's one record on
// the unreliable item that arrived with it, forever. The code writes the three
// reliable records in the first three updates and the unreliable item only
// when none is queued.

static void test_rnscheduler_witness_replays()
{
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &priorities_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, NULL, 1, PRIORITIES_CAPACITY ) );

    struct rn_packet_item_t items[4];
    priorities_item( &items[0], 0, 0, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_LOW );
    priorities_item( &items[1], 1, 0, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_IMMEDIATE );
    priorities_item( &items[2], 2, 1, ROCKETNET_SEND_CLASS_NEW_RELIABLE, ROCKETNET_PRIORITY_HIGH );

    uint8_t * frames[1] = { priorities_payloads[0] };
    int order[8];
    int count = 0;

    for ( int update = 0; update < 4; update++ )
    {
        // this update's unreliable item: last update's, if unwritten, is
        // dropped by the caller and a new one arrives

        priorities_item( &items[3], 3, 0, ROCKETNET_SEND_CLASS_UNRELIABLE, ROCKETNET_PRIORITY_IMMEDIATE );

        const int packets = rn_packet_builder_write( &builder, items, 4, frames, 1 );
        rocketnet_unit_check( packets == 1 );

        count = priorities_read( region, &builder, packets, order, count, 8 );
    }

    rocketnet_unit_check( count == 4 );
    rocketnet_unit_check( order[0] == 0 );          // ordering 0 before ordering 1 on channel 0
    rocketnet_unit_check( order[1] == 1 );
    rocketnet_unit_check( order[2] == 2 );          // every reliable record written before ...
    rocketnet_unit_check( order[3] == 3 );          // ... the unreliable class gets the budget

    rn_arena_destroy( arena );
}

// R2's acceptance for the scheduler: a counting allocator is installed, the
// connection is created, then many updates of prioritised writes, a budget
// refusal and the clocks' reset run, and the allocator is not called again.

static size_t priorities_allocs = 0;

static void * priorities_malloc( void * context, size_t bytes )
{
    (void) context;
    priorities_allocs++;
    return rocketnet_default_malloc_function( NULL, bytes );
}

static void priorities_free( void * context, void * p )
{
    (void) context;
    rocketnet_default_free_function( NULL, p );
}

static void test_send_priorities_no_allocation_after_create()
{
    // the suite initialised the library before this test; replacement is
    // refused after init, so the test terminates it, installs the counting
    // allocator, and initialises again

    rocketnet_term();

    rocketnet_unit_check( rocketnet_init( NULL, priorities_malloc, priorities_free ) == ROCKETNET_STATUS_OK );

    priorities_allocs = 0;

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &priorities_counters );
    rocketnet_unit_check( arena != NULL );

    const size_t after_create = priorities_allocs;
    rocketnet_unit_check( after_create > 0 );

    rn_region_t * region = rn_arena_region( arena, 0 );

    struct rocketnet_send_budget_t budget;
    rocketnet_unit_check( rocketnet_send_budget_init( &budget, 1000, 10.0 ) );

    rn_packet_builder_t builder;
    rocketnet_unit_check( rn_packet_builder_init( &builder, region, NULL, &budget, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, PRIORITIES_CAPACITY ) );

    uint8_t * frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int p = 0; p < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; p++ )
    {
        frames[p] = priorities_payloads[p];
    }

    struct rn_packet_item_t items[8];
    for ( int update = 0; update < 16; update++ )
    {
        for ( int i = 0; i < 8; i++ )
        {
            priorities_item( &items[i], i, (uint16_t) ( i % 3 ), i < 2 ? ROCKETNET_SEND_CLASS_RETRANSMIT : ( i < 6 ? ROCKETNET_SEND_CLASS_NEW_RELIABLE : ROCKETNET_SEND_CLASS_UNRELIABLE ), i % 4 );
        }

        rocketnet_send_budget_begin_update( &budget, 10.0 + 0.1 * update );
        if ( update == 8 )
            budget.tokens = 1.0;                                // a refusal: the packet is not written and the clocks roll back

        rn_packet_builder_write( &builder, items, 8, frames, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    }

    rocketnet_unit_check( priorities_allocs == after_create );

    rn_arena_destroy( arena );

    // the suite's own term follows: hand it back an initialised library on
    // the default allocator

    rocketnet_term();

    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_higher_priority_first_within_class );
REGISTER_UNIT_TEST( test_channel_fifo_kept_per_channel );
REGISTER_UNIT_TEST( test_round_robin_across_channels );
REGISTER_UNIT_TEST( test_no_starvation_of_low );
REGISTER_UNIT_TEST( test_budget_class_order_still_rules );
REGISTER_UNIT_TEST( test_rnscheduler_witness_replays );
REGISTER_UNIT_TEST( test_send_priorities_no_allocation_after_create );

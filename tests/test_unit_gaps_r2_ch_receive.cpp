/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Mutation gaps, group r2-ch: the receive side of the channel layer
// (sdk/rocketnet_channel.cpp, sdk/rocketnet_channel_reliable.cpp,
// sdk/rocketnet_channel_sequenced.cpp, sdk/rocketnet_window_receive.cpp).
// Each case names the mutants it kills. A record reaches the dispatch through
// the window (rn_window_receive_accept) where the case says so; otherwise it
// is dispatched directly, which the dispatch allows (rn_channels_descriptor
// fills a descriptor from the record). Time is never read; nothing opens a
// socket or sleeps.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_unit_registry.h"
#include "unit_arena.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_receive.h"

#define check rocketnet_unit_check

static const uint32_t RCV_F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
static const uint32_t RCV_HALF = ROCKETNET_MESSAGE_HALF_RANGE;

struct rcv_node_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
};

// W = 64; max_message_bytes and the unreliable allowance Du as the case needs

static void rcv_init( rcv_node_t * node, int max_message_bytes = 32768, int unreliable_per_update = -1 )
{
    memset( node, 0, sizeof( *node ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.packets_in_flight = 64;
    config.max_message_bytes = max_message_bytes;
    if ( unreliable_per_update >= 0 )
    {
        config.unreliable_messages_per_update = unreliable_per_update;
    }
    check( rocketnet_message_validate_config( &config ) == 1 );

    node->arena = rn_unit_arena_create( &config, 1, &node->counters );
    check( node->arena != NULL );
    node->region = rn_arena_region( node->arena, 0 );
    check( rn_ordered_init( &node->ordered, node->region, &config, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    check( node->region->window == 64 );
    check( node->ordered.channels.num_channels >= 4 );
}

static void rcv_free( rcv_node_t * node )
{
    rn_unit_arena_destroy( node->arena );
}

static uint64_t rcv_counter( const rcv_node_t * node, int which )
{
    return node->counters.values[which];
}

static rocketnet_message_record_t rcv_record( int code, uint32_t number, uint32_t channel, uint32_t ordering, uint32_t sequencing,
                                              const uint8_t * data, int bytes, uint32_t ring )
{
    rocketnet_message_record_t record = {};
    record.code = code;
    record.number = number;
    record.channel = channel;
    record.ordering = ordering;
    record.sequencing = sequencing;
    record.ring = ring;
    record.data = data;
    record.bytes = bytes;
    return record;
}

// chunk `index` of a `count`-chunk block whose span starts at ring unit `base`

static rocketnet_message_record_t rcv_chunk( int code, uint32_t number, uint32_t channel, uint32_t ordering, uint32_t sequencing,
                                             uint32_t index, uint32_t count, const uint8_t * data, int bytes, uint32_t base )
{
    rocketnet_message_record_t record = rcv_record( code, number, channel, ordering, sequencing, data, bytes, base + index * ( RCV_F / 16 ) );
    record.block = 1;
    record.fragment = index;
    record.fragments = count - 1;
    return record;
}

static void rcv_accept( rcv_node_t * node, const rocketnet_message_record_t & record )
{
    uint32_t slot = RN_DESCRIPTOR_NONE;
    const int accepted = rn_window_receive_accept( &node->ordered.receive, &record, &slot );
    if ( accepted != RN_WINDOW_RECEIVE_ACCEPT )
    {
        printf( "the window refused number %u (%d) while the case set up its records\n", record.number, accepted );
    }
    check( accepted == RN_WINDOW_RECEIVE_ACCEPT );
}

static int rcv_dispatch( rcv_node_t * node, const rocketnet_message_record_t & record, rn_delivery_t * out = NULL )
{
    rn_delivery_t delivery;
    return rn_channels_dispatch( &node->ordered.channels, &record, out ? out : &delivery );
}

static rn_channel_receive_t * rcv_rx( rcv_node_t * node, uint32_t channel )
{
    return &node->region->channel_receive[channel];
}

// ---------------------------------------------------------------------------
// the delivery table full: every reliable path refuses, exhausts the region,
// and writes nothing past the table
// ---------------------------------------------------------------------------

// The bytes just past the delivery table lie inside the region's block (the
// next carved part), where AddressSanitizer sees no overflow: the case keeps a
// copy and compares it, so a delivery appended past the table is seen in every
// build. The append's debug assert is recorded, not fatal, so a mutant reaches
// the case's own check.

struct rcv_guard_t
{
    const uint8_t * at;
    uint8_t saved[sizeof( rn_delivery_t )];
};

static int rcv_asserts = 0;

static void rcv_assert_record( const char *, const char *, const char *, int )
{
    rcv_asserts++;
}

static void rcv_guard_arm( rcv_node_t * node, rcv_guard_t * guard )
{
    guard->at = (const uint8_t *) ( node->region->deliveries + node->ordered.channels.delivery_capacity );
    check( guard->at >= node->region->base && guard->at + sizeof( rn_delivery_t ) <= node->region->base + node->region->bytes );
    memcpy( guard->saved, guard->at, sizeof( rn_delivery_t ) );
}

static void rcv_guard_check( const rcv_guard_t * guard, const char * path )
{
    const bool intact = memcmp( guard->saved, guard->at, sizeof( rn_delivery_t ) ) == 0;
    if ( !intact )
    {
        printf( "%s wrote a delivery past the full delivery table (into the next carved part)\n", path );
    }
    check( intact );
    if ( rcv_asserts != 0 )
    {
        printf( "%s appended to the full delivery table: rn_channels_append_delivery's bound fired %d times\n", path, rcv_asserts );
    }
    check( rcv_asserts == 0 );
}

// fills the table with repeated deliveries of one reliable number (a direct
// dispatch delivers a live number again), `less` short of full

static void rcv_fill_table( rcv_node_t * node, uint32_t number, uint32_t less )
{
    const uint8_t byte = 0x33;
    const rocketnet_message_record_t record = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, number, 0, 0, 0, &byte, 1, 0 );
    while ( node->ordered.channels.deliveries + less < node->ordered.channels.delivery_capacity )
    {
        check( rcv_dispatch( node, record ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    }
    check( node->ordered.channels.deliveries + less == node->ordered.channels.delivery_capacity );
    check( !rn_region_exhausted( node->region ) );
}

static void rcv_expect_refused_full( rcv_node_t * node, int result, const rcv_guard_t * guard, const char * path, bool exhausts )
{
    if ( result != ROCKETNET_CHANNEL_DISPATCH_NONE )
    {
        printf( "%s on a full delivery table returned %d, not NONE\n", path, result );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    if ( node->ordered.channels.deliveries != node->ordered.channels.delivery_capacity )
    {
        printf( "%s on a full delivery table left %u deliveries in a table of %u\n", path, node->ordered.channels.deliveries,
                node->ordered.channels.delivery_capacity );
    }
    check( node->ordered.channels.deliveries == node->ordered.channels.delivery_capacity );
    if ( exhausts && !rn_region_exhausted( node->region ) )
    {
        printf( "%s on a full delivery table did not exhaust the region (R5: the connection must be kicked)\n", path );
    }
    check( !exhausts || rn_region_exhausted( node->region ) );
    rcv_guard_check( guard, path );
}

// mutation gap: hunt 1/3045 (room check < to <=), hunt 1/3048 (exhaust deleted),
// hunt 1/3050 (return NULL deleted), hunt 1/3376 (in-turn NULL return deleted)

static void test_gaps_r2_ch_full_table_in_turn_ordered()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t data[4] = { 1, 2, 3, 4 };

    const rocketnet_message_record_t fill = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, data, 1, 0 );
    const rocketnet_message_record_t turn = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 0, 0, data, 4, 0 );
    rcv_accept( &node, fill );
    rcv_accept( &node, turn );
    rcv_fill_table( &node, 0, 0 );

    rcv_guard_t guard;
    rcv_guard_arm( &node, &guard );
    const rocketnet_assert_function_t saved = rn_assert_function;
    rcv_asserts = 0;
    rocketnet_assert_function( rcv_assert_record );

    const int result = rcv_dispatch( &node, turn );

    rocketnet_assert_function( saved );
    rcv_expect_refused_full( &node, result, &guard, "an in-turn reliable ordered record", true );
    if ( rcv_rx( &node, 1 )->ordered_read != 0 )
    {
        printf( "a refused in-turn record moved channel 1's read index\n" );
    }
    check( rcv_rx( &node, 1 )->ordered_read == 0 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_full_table_in_turn_ordered );

// mutation gap: hunt 1/4205 (reliable.cpp small record's NONE return deleted)

static void test_gaps_r2_ch_full_table_small_reliable()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t data[4] = { 1, 2, 3, 4 };

    const rocketnet_message_record_t fill = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, data, 1, 0 );
    const rocketnet_message_record_t small = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, 0, 0, data, 4, 0 );
    rcv_accept( &node, fill );
    rcv_accept( &node, small );
    rcv_fill_table( &node, 0, 0 );

    rcv_guard_t guard;
    rcv_guard_arm( &node, &guard );
    rcv_asserts = 0;

    const int result = rcv_dispatch( &node, small );

    rcv_expect_refused_full( &node, result, &guard, "a small reliable record", true );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_full_table_small_reliable );

// mutation gap: hunt 1/3305 (unreliable room deliveries >= capacity to >)

static void test_gaps_r2_ch_full_table_unreliable()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t data[4] = { 1, 2, 3, 4 };

    rcv_accept( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, data, 1, 0 ) );
    rcv_fill_table( &node, 0, 0 );
    check( node.ordered.channels.unreliable_delivered == 0 );

    rcv_guard_t guard;
    rcv_guard_arm( &node, &guard );
    const rocketnet_assert_function_t saved = rn_assert_function;
    rcv_asserts = 0;
    rocketnet_assert_function( rcv_assert_record );

    const uint64_t full = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 1, 0, 0, data, 4, 0 ) );

    rocketnet_assert_function( saved );
    rcv_expect_refused_full( &node, result, &guard, "an unreliable record", false );
    if ( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL ) != full + 1 )
    {
        printf( "an unreliable record refused by a full delivery table was not counted UNRELIABLE_DELIVERY_FULL\n" );
    }
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL ) == full + 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_full_table_unreliable );

// mutation gap: hunt 1/3508 (in-turn ordered block's NONE return deleted)

static void test_gaps_r2_ch_full_table_in_turn_ordered_block()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x44, sizeof( chunk ) );
    const uint8_t byte = 1;

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t c1 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 1, 0, 0, 1, 2, chunk, 100, 0 );
    rcv_accept( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, &byte, 1, 0 ) );
    rcv_accept( &node, c0 );
    rcv_accept( &node, c1 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rcv_fill_table( &node, 0, 0 );

    rcv_guard_t guard;
    rcv_guard_arm( &node, &guard );
    const rocketnet_assert_function_t saved = rn_assert_function;
    rcv_asserts = 0;
    rocketnet_assert_function( rcv_assert_record );

    const int result = rcv_dispatch( &node, c1 );

    rocketnet_assert_function( saved );
    rcv_expect_refused_full( &node, result, &guard, "an in-turn ordered block completing", true );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_full_table_in_turn_ordered_block );

// mutation gap: hunt 1/4172 (reliable.cpp completed block's NONE return deleted)

static void test_gaps_r2_ch_full_table_reliable_block()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x55, sizeof( chunk ) );
    const uint8_t byte = 1;

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t c1 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE, 2, 0, 0, 0, 1, 2, chunk, 100, 0 );
    rcv_accept( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, &byte, 1, 0 ) );
    rcv_accept( &node, c0 );
    rcv_accept( &node, c1 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rcv_fill_table( &node, 0, 0 );

    rcv_guard_t guard;
    rcv_guard_arm( &node, &guard );
    rcv_asserts = 0;

    const int result = rcv_dispatch( &node, c1 );

    rcv_expect_refused_full( &node, result, &guard, "a reliable block completing", true );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_full_table_reliable_block );

// The drain fills the last row of the table with the in-turn message, then
// finds the table full at the held successor: it stops there, the successor
// stays held and the region is exhausted.
// mutation gap: hunt 1/3338 (the drain's break on a full table deleted)

static void test_gaps_r2_ch_full_table_stops_the_drain()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t data[4] = { 9, 8, 7, 6 };

    const rocketnet_message_record_t fill = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, data, 1, 0 );
    const rocketnet_message_record_t held1 = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 2, 1, 0, data, 4, 100 );
    const rocketnet_message_record_t held2 = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 2, 2, 0, data, 4, 101 );
    const rocketnet_message_record_t turn = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 3, 2, 0, 0, data, 4, 0 );
    rcv_accept( &node, fill );
    rcv_accept( &node, held1 );
    rcv_accept( &node, held2 );
    rcv_accept( &node, turn );
    check( rcv_dispatch( &node, held1 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_dispatch( &node, held2 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( node.region->held_count == 2 );
    rcv_fill_table( &node, 0, 1 );

    rcv_guard_t guard;
    rcv_guard_arm( &node, &guard );
    const rocketnet_assert_function_t saved = rn_assert_function;
    rcv_asserts = 0;
    rocketnet_assert_function( rcv_assert_record );

    const int result = rcv_dispatch( &node, turn );

    rocketnet_assert_function( saved );
    check( result == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( node.ordered.channels.deliveries == node.ordered.channels.delivery_capacity );
    if ( !rn_region_exhausted( node.region ) )
    {
        printf( "the drain found the delivery table full and did not exhaust the region\n" );
    }
    check( rn_region_exhausted( node.region ) );
    if ( node.region->held_count != 2 || rcv_rx( &node, 2 )->ordered_read != 1 )
    {
        printf( "the drain went on past a full delivery table: %u held left, read index %u\n", node.region->held_count,
                rcv_rx( &node, 2 )->ordered_read );
    }
    check( node.region->held_count == 2 );
    check( rcv_rx( &node, 2 )->ordered_read == 1 );
    rcv_guard_check( &guard, "the drain" );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_full_table_stops_the_drain );

// ---------------------------------------------------------------------------
// a descriptor filled by the dispatch from the record (no window accept): a
// released descriptor keeps its old fields, and every one the record carries
// must be replaced
// ---------------------------------------------------------------------------

static void rcv_plant_released( rcv_node_t * node, uint32_t number, uint32_t ring, uint32_t bytes, uint16_t channel )
{
    rn_receive_descriptor_t * descriptor = &node->region->receive_descriptors[number & ( node->region->window - 1 )];
    descriptor->live = 0;
    descriptor->number = number + node->region->window;      // an earlier use of the slot, released at its update's end
    descriptor->ring = ring;
    descriptor->bytes = bytes;
    descriptor->channel = channel;
    descriptor->code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
}

// mutation gap: hunt 1/3131 (descriptor->ring = record->ring deleted)

static void test_gaps_r2_ch_direct_block_anchor_takes_the_record_ring()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x66, sizeof( chunk ) );

    const uint32_t base = 100;
    rcv_plant_released( &node, 11, 999, 1, 1 );

    rn_delivery_t out;
    check( rcv_dispatch( &node, rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 11, 1, 0, 0, 1, 2, chunk, 100, base ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    const int result = rcv_dispatch( &node, rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 10, 1, 0, 0, 0, 2, chunk, (int) RCV_F, base ), &out );
    if ( result != ROCKETNET_CHANNEL_DISPATCH_DELIVERED )
    {
        printf( "a block anchored by a directly dispatched chunk was refused (%d, bad block %llu): the anchor kept a released descriptor's ring\n",
                result, (unsigned long long) rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( out.bytes == (int) RCV_F + 100 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_direct_block_anchor_takes_the_record_ring );

// mutation gap: hunt 1/3132 (descriptor->bytes = record->bytes deleted)

static void test_gaps_r2_ch_direct_block_final_chunk_takes_the_record_bytes()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x77, sizeof( chunk ) );

    const uint32_t base = 300;
    rcv_plant_released( &node, 22, 5, 999, 1 );

    rn_delivery_t out;
    check( rcv_dispatch( &node, rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 22, 1, 0, 0, 2, 3, chunk, 200, base ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_dispatch( &node, rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 20, 1, 0, 0, 0, 3, chunk, (int) RCV_F, base ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    const int result = rcv_dispatch( &node, rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 21, 1, 0, 0, 1, 3, chunk, (int) RCV_F, base ), &out );
    check( result == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    if ( out.bytes != 2 * (int) RCV_F + 200 )
    {
        printf( "a block delivered %d bytes, not %d: its final chunk's length was read from a released descriptor\n", out.bytes,
                2 * (int) RCV_F + 200 );
    }
    check( out.bytes == 2 * (int) RCV_F + 200 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_direct_block_final_chunk_takes_the_record_bytes );

// mutation gap: hunt 1/3133 (descriptor->channel = record->channel deleted)

static void test_gaps_r2_ch_direct_held_record_takes_the_record_channel()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t a = 0xA1;
    const uint8_t b = 0xB2;

    rcv_plant_released( &node, 30, 7, 1, 3 );

    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 30, 1, 1, 0, &a, 1, 200 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 31, 1, 0, 0, &b, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    if ( node.ordered.channels.deliveries != 2 || node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_NONE )
    {
        printf( "the held message was not drained (%u deliveries, error %d): its descriptor kept a released descriptor's channel\n",
                node.ordered.channels.deliveries, node.ordered.channels.error );
    }
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( node.ordered.channels.deliveries == 2 );
    check( node.region->deliveries[1].data[0] == a );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_direct_held_record_takes_the_record_channel );

// ---------------------------------------------------------------------------
// block anchors: a fresh anchor, a refused insert, a dead anchor, a collision
// ---------------------------------------------------------------------------

// A code-2 block completes and is delivered; its first chunk again in the same
// update (a direct dispatch of a live number) opens a fresh anchor on the first
// number's descriptor, which still says the block's count is in.
// mutation gap: hunt 1/3406 (fresh anchor's block_have = 0 deleted)

static void test_gaps_r2_ch_fresh_anchor_counts_from_zero()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x12, sizeof( chunk ) );

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE, 40, 0, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t c1 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE, 41, 0, 0, 0, 1, 2, chunk, 50, 0 );
    for ( uint32_t n = 0; n < 40; n++ )
    {
        rcv_accept( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, n, 0, 0, 0, chunk, 1, 0 ) );
    }
    rcv_accept( &node, c0 );
    rcv_accept( &node, c1 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_dispatch( &node, c1 ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( node.ordered.channels.deliveries == 1 );

    const int again = rcv_dispatch( &node, c0 );
    if ( again == ROCKETNET_CHANNEL_DISPATCH_DELIVERED )
    {
        printf( "one chunk of a two-chunk block completed it again: the fresh anchor kept the settled block's chunk count\n" );
    }
    check( again != ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( node.ordered.channels.deliveries == 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_fresh_anchor_counts_from_zero );

// The held index refuses an entry past W live ones (RNHeldOwnership HeldBound):
// the count is set at the bound and the first chunk of a block cannot anchor.
// mutation gap: hunt 1/3402 (the refused anchor insert's violation deleted)

static void test_gaps_r2_ch_refused_anchor_insert_ends_the_connection()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x13, sizeof( chunk ) );

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    rcv_accept( &node, c0 );

    const uint32_t held_count = node.region->held_count;
    node.region->held_count = node.region->window;
    const uint64_t violations = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
    const int result = rcv_dispatch( &node, c0 );
    node.region->held_count = held_count;

    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    if ( node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "a block anchor the held index refused (HeldBound) did not end the connection\n" );
    }
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) == violations + 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_refused_anchor_insert_ends_the_connection );

// mutation gap: hunt 1/3410 (a held anchor no longer live: violation deleted)

static void test_gaps_r2_ch_dead_anchor_ends_the_connection()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x14, sizeof( chunk ) );

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t c1 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, 0, 0, 1, 2, chunk, 60, 0 );
    rcv_accept( &node, c0 );
    rcv_accept( &node, c1 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    node.region->receive_descriptors[0].live = 0;                       // the anchor's descriptor released under it

    const int result = rcv_dispatch( &node, c1 );
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    if ( node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "a chunk whose held anchor is no longer live (HeldNamesLive) was dropped without ending the connection\n" );
    }
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_dead_anchor_ends_the_connection );

// mutation gap: hunt 1/3474 (a chunk's slot collision: SLOT_COLLISION count deleted)

static void test_gaps_r2_ch_chunk_slot_collision_is_counted()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x15, sizeof( chunk ) );
    const uint8_t byte = 3;

    const rocketnet_message_record_t live = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 6, 1, 1, 0, &byte, 1, 400 );
    for ( uint32_t n = 0; n < 6; n++ )
    {
        rcv_accept( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, n, 0, 0, 0, &byte, 1, 0 ) );
    }
    rcv_accept( &node, live );
    check( rcv_dispatch( &node, live ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    const uint64_t collisions = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION );
    const int result = rcv_dispatch( &node, rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 6 + 64, 2, 0, 0, 0, 2, chunk, (int) RCV_F, 0 ) );
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    if ( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION ) != collisions + 1 ||
         node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "a chunk numbered into a slot live for another number was refused without MESSAGE_SLOT_COLLISION and the violation\n" );
    }
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION ) == collisions + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_chunk_slot_collision_is_counted );

// A completed block longer than max_message_bytes: the least max (1415) admits a count of
// two, and a last chunk of F bytes makes 2F. Each code's completion check
// refuses it (MESSAGE_BAD_BLOCK, the connection ends) and delivers nothing.
// mutation gap: hunt 1/3501 (code 3), hunt 1/4166 (code 2), hunt 1/4400 (code 4)

static void rcv_oversized_block( int code, uint32_t channel )
{
    rcv_node_t node;
    rcv_init( &node, ROCKETNET_MESSAGE_SINGLE_BYTES );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x16, sizeof( chunk ) );

    const rocketnet_message_record_t c0 = rcv_chunk( code, 0, channel, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t c1 = rcv_chunk( code, 1, channel, 0, 0, 1, 2, chunk, (int) RCV_F, 0 );
    rcv_accept( &node, c0 );
    rcv_accept( &node, c1 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const int result = rcv_dispatch( &node, c1 );
    if ( result != ROCKETNET_CHANNEL_DISPATCH_NONE || node.ordered.channels.deliveries != 0 ||
         rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) != bad_block + 1 ||
         node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "a code-%d block of %u bytes past max_message_bytes %u was not refused as MESSAGE_BAD_BLOCK (result %d, %u deliveries)\n",
                code, 2 * RCV_F, (uint32_t) ROCKETNET_MESSAGE_SINGLE_BYTES, result, node.ordered.channels.deliveries );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.ordered.channels.deliveries == 0 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) == bad_block + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rcv_free( &node );
}

static void test_gaps_r2_ch_oversized_block_is_bad_block()
{
    rcv_oversized_block( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1 );
    rcv_oversized_block( ROCKETNET_MESSAGE_CODE_RELIABLE, 0 );
    rcv_oversized_block( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 1 );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_oversized_block_is_bad_block );

// ---------------------------------------------------------------------------
// argument refusals of the receive half
// ---------------------------------------------------------------------------

// mutation gap: hunt 1/3290 (dispatch's NONE for bytes < 1 or no data deleted)

static void test_gaps_r2_ch_dispatch_refuses_an_empty_record()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 1;

    const int empty = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 0, &byte, 0, 0 ) );
    const int nodata = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 0, NULL, 4, 0 ) );
    if ( empty != ROCKETNET_CHANNEL_DISPATCH_NONE || nodata != ROCKETNET_CHANNEL_DISPATCH_NONE || node.ordered.channels.deliveries != 0 )
    {
        printf( "a record with no bytes or no data was dispatched (%d, %d, %u deliveries)\n", empty, nodata, node.ordered.channels.deliveries );
    }
    check( empty == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( nodata == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.ordered.channels.deliveries == 0 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_dispatch_refuses_an_empty_record );

// A binding of fewer channels than the region carves: channel k = num_channels
// is outside the binding though inside the region's arrays, so a missing
// refusal changes the region's state for a channel this binding does not own.
// mutation gap: hunt 1/3241, 1/3244 (deliver_ordered), hunt 1/3253 (deliver_sequenced),
// hunt 1/3323 (drain)

static void test_gaps_r2_ch_receive_calls_refuse_a_channel_outside_the_binding()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 0x5C;
    const uint32_t k = 2;

    // a held in-turn message on channel k, put there by the full binding

    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 4, k, 1, 0, &byte, 1, 100 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rcv_rx( &node, k )->ordered_read = 1;
    rcv_rx( &node, k )->sequenced_watermark = 5;
    const rn_channel_receive_t before = *rcv_rx( &node, k );

    rn_channels_t narrow;
    check( rn_channels_init( &narrow, node.region, k, 0 ) );

    rn_channels_deliver_ordered( &narrow, k, 40 );
    const bool sequenced = rn_channels_deliver_sequenced( &narrow, k, 1, 9 );
    rn_channels_drain( &narrow, k );

    const rn_channel_receive_t after = *rcv_rx( &node, k );
    if ( memcmp( &before, &after, sizeof( before ) ) != 0 || sequenced || narrow.deliveries != 0 || node.region->held_count != 1 )
    {
        printf( "a channel outside the binding was written: read %u -> %u, watermark %u -> %u, sequenced %d, %u deliveries\n",
                before.ordered_read, after.ordered_read, before.sequenced_watermark, after.sequenced_watermark, sequenced ? 1 : 0,
                narrow.deliveries );
    }
    check( memcmp( &before, &after, sizeof( before ) ) == 0 );
    check( !sequenced );
    check( narrow.deliveries == 0 );
    check( node.region->held_count == 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_receive_calls_refuse_a_channel_outside_the_binding );

// ---------------------------------------------------------------------------
// sequenced (codes 1 and 4)
// ---------------------------------------------------------------------------

// A code-4 hold naming slot W (memory corruption) is refused before
// receive_descriptors[W] is read. That entry lies inside the region's block
// (the next carved part), so the case plants a valid-looking code-4
// descriptor there: a check that let W through would read it and accept.
// mutation gap: hunt 1/4249 (hold_valid slot >= W to >), hunt 1/4251 (its return false deleted)

static void test_gaps_r2_ch_sequenced_hold_at_window_is_refused()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 0x21;
    const uint32_t W = node.region->window;

    const rocketnet_message_record_t future = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 2, 0, &byte, 1, 100 );
    rcv_accept( &node, future );

    rn_receive_descriptor_t * past = node.region->receive_descriptors + W;
    check( (const uint8_t *) ( past + 1 ) <= node.region->base + node.region->bytes );
    rn_receive_descriptor_t saved;
    memcpy( &saved, past, sizeof( saved ) );
    rn_receive_descriptor_t planted = {};
    planted.number = 0;
    planted.ordering = 1;
    planted.sequencing = 0;
    planted.channel = 1;
    planted.code = ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
    planted.live = 1;
    memcpy( past, &planted, sizeof( planted ) );

    rcv_rx( &node, 1 )->held_sequenced = W | RN_SEQUENCED_RELIABLE_HOLD;
    rcv_rx( &node, 1 )->held_sequenced_first = 5;                       // a span whose numbers never arrived: a release of it frees nothing
    const int result = rcv_dispatch( &node, future );
    const uint32_t hold = rcv_rx( &node, 1 )->held_sequenced;

    memcpy( past, &saved, sizeof( saved ) );

    if ( result != ROCKETNET_CHANNEL_DISPATCH_NONE || node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION ||
         hold != RN_DESCRIPTOR_NONE )
    {
        printf( "a sequenced hold naming slot W was accepted (result %d, error %d, hold %08x): receive_descriptors[W] was read\n", result,
                node.ordered.channels.error, hold );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( hold == RN_DESCRIPTOR_NONE );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_hold_at_window_is_refused );

// Holds naming a descriptor that is not live (a broken invariant): the in-turn
// dispatch, the future dispatch and the drain each drop the hold and end the
// connection.
// mutation gap: hunt 1/4423, 1/4424 (in turn), hunt 1/4483, 1/4484 (future),
// hunt 1/4549, 1/4550, 1/4551 (drain)

static void rcv_expect_hold_dropped( rcv_node_t * node, uint32_t channel, const char * where )
{
    const rn_channel_receive_t * rx = rcv_rx( node, channel );
    if ( rx->block_in_turn != RN_DESCRIPTOR_NONE || rx->held_sequenced != RN_DESCRIPTOR_NONE ||
         node->ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "%s kept a hold naming a dead descriptor (in turn %08x, held %08x, error %d)\n", where, rx->block_in_turn,
                rx->held_sequenced, node->ordered.channels.error );
    }
    check( rx->block_in_turn == RN_DESCRIPTOR_NONE );
    check( rx->held_sequenced == RN_DESCRIPTOR_NONE );
    check( node->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
}

static void test_gaps_r2_ch_sequenced_dead_holds_end_the_connection()
{
    const uint8_t byte = 0x22;

    {
        rcv_node_t node;
        rcv_init( &node );
        rcv_rx( &node, 1 )->block_in_turn = 3 | RN_SEQUENCED_RELIABLE_HOLD;
        check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, 0, &byte, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        rcv_expect_hold_dropped( &node, 1, "the in-turn dispatch" );
        rcv_free( &node );
    }

    {
        rcv_node_t node;
        rcv_init( &node );
        rcv_rx( &node, 1 )->held_sequenced = 3 | RN_SEQUENCED_RELIABLE_HOLD;
        check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, 0, &byte, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
        rcv_expect_hold_dropped( &node, 1, "the future dispatch" );
        rcv_free( &node );
    }

    {
        rcv_node_t node;
        rcv_init( &node );
        rcv_rx( &node, 1 )->block_in_turn = 3 | RN_SEQUENCED_RELIABLE_HOLD;
        rcv_rx( &node, 1 )->held_sequenced = 4 | RN_SEQUENCED_RELIABLE_HOLD;
        check( !rn_channels_drain_sequenced( &node.ordered.channels, 1 ) );
        rcv_expect_hold_dropped( &node, 1, "the drain" );
        rcv_free( &node );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_dead_holds_end_the_connection );

// block_in_turn holding a code-1 hold index (no reliable flag) while a code-4
// fragment of the same sequencing arrives in turn: the block refuses it.
// mutation gap: hunt 1/4363 (the non-reliable hold's BAD_BLOCK count deleted)

static void test_gaps_r2_ch_sequenced_block_refuses_an_unreliable_in_turn_hold()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x23, sizeof( chunk ) );

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 0, 7, 0, 2, chunk, (int) RCV_F, 0 );
    rcv_accept( &node, c0 );
    node.region->receive_descriptors[1].sequencing = 7;                 // what the hold index reads as its sequencing
    rcv_rx( &node, 1 )->block_in_turn = 1;                              // a code-1 hold index

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const int result = rcv_dispatch( &node, c0 );
    if ( result != ROCKETNET_CHANNEL_DISPATCH_NONE || rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) != bad_block + 1 ||
         node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "a code-4 fragment joined an in-turn hold that is not a code-4 block without MESSAGE_BAD_BLOCK (result %d)\n", result );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) == bad_block + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_block_refuses_an_unreliable_in_turn_hold );

// mutation gap: hunt 1/4377 (a fragment disagreeing with its anchor: BAD_BLOCK count deleted)

static void test_gaps_r2_ch_sequenced_fragment_must_agree_with_its_anchor()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x24, sizeof( chunk ) );

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 0, 0, 0, 3, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t c1 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 1, 1, 0, 0, 1, 4, chunk, (int) RCV_F, 0 );
    rcv_accept( &node, c0 );
    rcv_accept( &node, c1 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const int result = rcv_dispatch( &node, c1 );
    if ( result != ROCKETNET_CHANNEL_DISPATCH_NONE || rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) != bad_block + 1 ||
         node.ordered.channels.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "a code-4 fragment whose count disagrees with its anchor was not refused as MESSAGE_BAD_BLOCK (result %d)\n", result );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) == bad_block + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_fragment_must_agree_with_its_anchor );

// An in-turn block collecting at sequencing 0; an in-turn record exactly half
// the range away is not newer: stale, and the block is kept.
// mutation gap: hunt 1/4433 (held_gap >= HALF to >)

static void test_gaps_r2_ch_sequenced_half_range_from_the_in_turn_block_is_stale()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x25, sizeof( chunk ) );
    const uint8_t byte = 1;

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    rcv_accept( &node, c0 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    const uint32_t block = rcv_rx( &node, 1 )->block_in_turn;
    check( block != RN_DESCRIPTOR_NONE );

    const uint64_t superseded = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, RCV_HALF, &byte, 1, 0 ) );
    if ( rcv_rx( &node, 1 )->block_in_turn != block || !node.region->receive_descriptors[0].live ||
         rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) != stale )
    {
        printf( "a record half the range from the in-turn block's sequencing was taken as newer: the block was released\n" );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( rcv_rx( &node, 1 )->block_in_turn == block );
    check( node.region->receive_descriptors[0].live != 0 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED ) == superseded + 1 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_half_range_from_the_in_turn_block_is_stale );

// A code-4 block on channel 0 claims numbers base and base + 1, but number base
// is a code-3 message held on channel 1 (one number given to two records): the
// chunk at index 1 anchors the block, and releasing that block fails.

static void rcv_claim_foreign_number( rcv_node_t * node, uint32_t base, uint32_t count, uint32_t ordering, uint32_t sequencing, uint32_t index )
{
    static uint8_t chunk[RCV_F];
    memset( chunk, 0x26, sizeof( chunk ) );
    static const uint8_t foreign = 0xF0;

    rcv_accept( node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, base, 1, 1, 0, &foreign, 1, 1000 ) );
    check( rcv_dispatch( node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, base, 1, 1, 0, &foreign, 1, 1000 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    const bool last = index + 1 == count;
    const rocketnet_message_record_t c = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, base + index, 0, ordering, sequencing, index, count,
                                                    chunk, last ? 40 : (int) RCV_F, 0 );
    rcv_accept( node, c );
    check( rcv_dispatch( node, c ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
}

static void rcv_expect_failed_release( rcv_node_t * node, int result, uint64_t bad_block, uint64_t superseded, uint64_t stale, const char * where )
{
    if ( result != ROCKETNET_CHANNEL_DISPATCH_NONE || rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) != bad_block + 1 ||
         rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED ) != superseded ||
         rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) != stale || node->ordered.channels.deliveries != 0 )
    {
        printf( "%s went on after a block release that failed MESSAGE_BAD_BLOCK (result %d, superseded +%llu, stale +%llu, %u deliveries)\n",
                where, result, (unsigned long long) ( rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED ) - superseded ),
                (unsigned long long) ( rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) - stale ), node->ordered.channels.deliveries );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) == bad_block + 1 );
    check( rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED ) == superseded );
    check( rcv_counter( node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale );
    check( node->ordered.channels.deliveries == 0 );
    check( node->ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
}

// mutation gap: hunt 1/4440 (the newer in-turn record's NONE after a failed release deleted)

static void test_gaps_r2_ch_sequenced_failed_in_turn_release_stops_a_newer_record()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 1;
    rcv_claim_foreign_number( &node, 0, 2, 0, 0, 1 );
    check( rcv_rx( &node, 0 )->block_in_turn != RN_DESCRIPTOR_NONE );

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const uint64_t superseded = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, 1, &byte, 1, 0 ) );
    rcv_expect_failed_release( &node, result, bad_block, superseded, stale, "a newer in-turn record" );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_failed_in_turn_release_stops_a_newer_record );

// The watermark is half the range past the in-turn block (as if moved there),
// so a continuation fragment is stale and releases the block: that fails.
// mutation gap: hunt 1/4447 (the stale path's NONE after a failed release deleted)

static void test_gaps_r2_ch_sequenced_failed_in_turn_release_stops_a_stale_continuation()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x27, sizeof( chunk ) );
    rcv_claim_foreign_number( &node, 0, 3, 0, 0, 2 );
    check( rcv_rx( &node, 0 )->block_in_turn != RN_DESCRIPTOR_NONE );
    rcv_rx( &node, 0 )->sequenced_watermark = RCV_HALF;

    const rocketnet_message_record_t c1 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 1, 0, 0, 0, 1, 3, chunk, (int) RCV_F, 0 );
    rcv_accept( &node, c1 );

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const uint64_t superseded = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    const int result = rcv_dispatch( &node, c1 );
    rcv_expect_failed_release( &node, result, bad_block, superseded, stale, "a stale continuation" );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_failed_in_turn_release_stops_a_stale_continuation );

// mutation gap: hunt 1/4514 (the future hold's failed release: NONE return deleted)

static void test_gaps_r2_ch_sequenced_failed_future_release_stops_a_newer_record()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 1;
    rcv_claim_foreign_number( &node, 0, 2, 1, 0, 1 );
    check( rcv_rx( &node, 0 )->held_sequenced != RN_DESCRIPTOR_NONE );

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const uint64_t superseded = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 2, 0, &byte, 1, 0 ) );
    rcv_expect_failed_release( &node, result, bad_block, superseded, stale, "a newer future record" );
    check( rcv_rx( &node, 0 )->held_sequenced == RN_DESCRIPTOR_NONE );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_failed_future_release_stops_a_newer_record );

// mutation gap: hunt 1/4442 (sequence_gap >= HALF to >)

static void test_gaps_r2_ch_sequenced_half_range_from_the_watermark_is_stale()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 1;

    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, RCV_HALF, &byte, 1, 0 ) );
    if ( result != ROCKETNET_CHANNEL_DISPATCH_STALE || node.ordered.channels.deliveries != 0 )
    {
        printf( "an in-turn record half the range past the watermark was delivered as fresh (result %d)\n", result );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( node.ordered.channels.deliveries == 0 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale + 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_half_range_from_the_watermark_is_stale );

// mutation gap: hunt 1/4453 (the in-turn code-1 record's NONE on a full unreliable share deleted)

static void test_gaps_r2_ch_sequenced_in_turn_unreliable_share_is_bounded()
{
    rcv_node_t node;
    rcv_init( &node, 32768, 1 );
    const uint8_t byte = 1;

    const uint64_t full = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, 0, &byte, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 2, 0, 0, &byte, 1, 0 ) );
    if ( result != ROCKETNET_CHANNEL_DISPATCH_NONE || node.ordered.channels.deliveries != 1 )
    {
        printf( "a second in-turn code-1 record was delivered past the unreliable share Du = 1 (result %d, %u deliveries)\n", result,
                node.ordered.channels.deliveries );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.ordered.channels.deliveries == 1 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL ) == full + 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_in_turn_unreliable_share_is_bounded );

// mutation gap: hunt 1/4467 (delta >= HALF to >)

static void test_gaps_r2_ch_sequenced_half_range_from_the_read_index_is_stale()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 1;

    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    const int unreliable = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, RCV_HALF, 0, &byte, 1, 0 ) );

    const rocketnet_message_record_t reliable = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 2, RCV_HALF, 0, &byte, 1, 100 );
    rcv_accept( &node, reliable );
    const int result = rcv_dispatch( &node, reliable );

    if ( unreliable != ROCKETNET_CHANNEL_DISPATCH_STALE || result != ROCKETNET_CHANNEL_DISPATCH_STALE ||
         rcv_rx( &node, 1 )->held_sequenced != RN_DESCRIPTOR_NONE || rcv_rx( &node, 2 )->held_sequenced != RN_DESCRIPTOR_NONE )
    {
        printf( "a record half the range ahead of the read index was held as future, not stale (%d, %d)\n", unreliable, result );
    }
    check( unreliable == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( result == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( rcv_rx( &node, 1 )->held_sequenced == RN_DESCRIPTOR_NONE );
    check( rcv_rx( &node, 2 )->held_sequenced == RN_DESCRIPTOR_NONE );
    check( node.region->receive_descriptors[0].live == 0 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale + 2 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_half_range_from_the_read_index_is_stale );

// A future code-1 hold at (ordering 1, sequencing 0); a record of the same
// ordering half the sequencing range away is stale, and the hold is kept.
// mutation gap: hunt 1/4502 (sequence_gap >= HALF to > against the hold)

static void test_gaps_r2_ch_sequenced_half_range_from_the_hold_is_stale()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t first = 0x31;
    const uint8_t second = 0x32;

    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, 0, &first, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    const uint32_t hold = rcv_rx( &node, 1 )->held_sequenced;
    check( hold != RN_DESCRIPTOR_NONE );

    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, RCV_HALF, &second, 1, 0 ) );
    const uint32_t now_held = rcv_rx( &node, 1 )->held_sequenced;
    if ( result != ROCKETNET_CHANNEL_DISPATCH_STALE || now_held == RN_DESCRIPTOR_NONE || node.region->sequenced_messages_held[now_held].sequencing != 0 ||
         node.region->sequenced_message_payloads[now_held].data[0] != first )
    {
        printf( "a record half the sequencing range from the hold replaced it (result %d)\n", result );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( now_held == hold );
    check( node.region->sequenced_messages_held[now_held].sequencing == 0 );
    check( node.region->sequenced_message_payloads[now_held].data[0] == first );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_half_range_from_the_hold_is_stale );

// A code-4 block partly received in turn on channel 0, then channel 0's next
// ordered message delivered: the drain releases the block of the closed epoch
// and counts it stale.
// mutation gap: hunt 1/4559 (SEQUENCED_STALE count deleted)

static void test_gaps_r2_ch_sequenced_drain_releases_a_closed_epoch_block()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x28, sizeof( chunk ) );
    const uint8_t byte = 1;

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t ordered = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 0, 0, 0, &byte, 1, 0 );   // past the block's span (0, 1)
    rcv_accept( &node, c0 );
    rcv_accept( &node, ordered );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_rx( &node, 0 )->block_in_turn != RN_DESCRIPTOR_NONE );

    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    check( rcv_dispatch( &node, ordered ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    if ( rcv_rx( &node, 0 )->block_in_turn != RN_DESCRIPTOR_NONE || node.region->receive_descriptors[0].live ||
         rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) != stale + 1 )
    {
        printf( "the drain did not release and count the in-turn block of the closed epoch (stale +%llu)\n",
                (unsigned long long) ( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) - stale ) );
    }
    check( rcv_rx( &node, 0 )->block_in_turn == RN_DESCRIPTOR_NONE );
    check( node.region->receive_descriptors[0].live == 0 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale + 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_drain_releases_a_closed_epoch_block );

// rn_channels_drain_sequenced's contract: an in-turn block of the open epoch
// (its ordering is the read index) is not stale and stays.
// mutation gap: hunt 1/4555 (the closed-epoch test != 0 to != 1)

static void test_gaps_r2_ch_sequenced_drain_keeps_the_open_epoch_block()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x29, sizeof( chunk ) );

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 0, 0, 0, 2, chunk, (int) RCV_F, 0 );
    rcv_accept( &node, c0 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    const uint32_t block = rcv_rx( &node, 0 )->block_in_turn;
    check( block != RN_DESCRIPTOR_NONE );

    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    check( !rn_channels_drain_sequenced( &node.ordered.channels, 0 ) );
    if ( rcv_rx( &node, 0 )->block_in_turn != block || !node.region->receive_descriptors[0].live )
    {
        printf( "the drain released an in-turn block of the open epoch as stale\n" );
    }
    check( rcv_rx( &node, 0 )->block_in_turn == block );
    check( node.region->receive_descriptors[0].live != 0 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_drain_keeps_the_open_epoch_block );

// mutation gap: hunt 1/4558 (the drain's failed release of a closed-epoch block: return false deleted)

static void test_gaps_r2_ch_sequenced_drain_failed_release_counts_no_stale()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 1;
    rcv_claim_foreign_number( &node, 0, 2, 0, 0, 1 );
    check( rcv_rx( &node, 0 )->block_in_turn != RN_DESCRIPTOR_NONE );

    const rocketnet_message_record_t ordered = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 0, 0, 0, &byte, 1, 0 );
    rcv_accept( &node, ordered );
    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    check( rcv_dispatch( &node, ordered ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    if ( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) != bad_block + 1 ||
         rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) != stale )
    {
        printf( "the drain counted a closed-epoch block stale after its release failed MESSAGE_BAD_BLOCK\n" );
    }
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) == bad_block + 1 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_drain_failed_release_counts_no_stale );

// A future hold the read index has passed (rn_channels_deliver_ordered moves
// it there) is released by the drain and counted stale: one half the range
// behind, and one just behind.
// mutation gap: hunt 1/4566 (gap >= HALF to >), hunt 1/4568 (release negated),
// hunt 1/4570 (SEQUENCED_STALE count deleted)

static void rcv_passed_hold( uint32_t read_past )
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 0x41;

    const uint32_t free_before = node.region->sequenced_hold_free_count;
    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 3, 0, &byte, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( node.region->sequenced_hold_free_count == free_before - 1 );

    rn_channels_deliver_ordered( &node.ordered.channels, 1, read_past );

    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    check( !rn_channels_drain_sequenced( &node.ordered.channels, 1 ) );
    if ( rcv_rx( &node, 1 )->held_sequenced != RN_DESCRIPTOR_NONE || node.region->sequenced_hold_free_count != free_before ||
         rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) != stale + 1 )
    {
        printf( "a hold %u behind the read index was not released and counted stale by the drain\n",
                ( rcv_rx( &node, 1 )->ordered_read - 3 ) & ROCKETNET_MESSAGE_U24_MASK );
    }
    check( rcv_rx( &node, 1 )->held_sequenced == RN_DESCRIPTOR_NONE );
    check( node.region->sequenced_hold_free_count == free_before );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale + 1 );
    rcv_free( &node );
}

static void test_gaps_r2_ch_sequenced_drain_releases_a_passed_hold()
{
    rcv_passed_hold( 3 + RCV_HALF - 1 );                                // read index 3 + HALF: the hold is exactly half the range behind
    rcv_passed_hold( 3 );                                               // read index 4: the hold is one behind
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_drain_releases_a_passed_hold );

// mutation gap: hunt 1/4569 (the passed hold's failed release: return false deleted)

static void test_gaps_r2_ch_sequenced_drain_failed_release_of_a_passed_hold()
{
    rcv_node_t node;
    rcv_init( &node );
    rcv_claim_foreign_number( &node, 0, 2, 3, 0, 1 );
    check( rcv_rx( &node, 0 )->held_sequenced != RN_DESCRIPTOR_NONE );

    rn_channels_deliver_ordered( &node.ordered.channels, 0, 3 );

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    check( !rn_channels_drain_sequenced( &node.ordered.channels, 0 ) );
    if ( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) != bad_block + 1 ||
         rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) != stale )
    {
        printf( "the drain counted a passed hold stale after its release failed MESSAGE_BAD_BLOCK\n" );
    }
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) == bad_block + 1 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_drain_failed_release_of_a_passed_hold );

// Du = 1: one code-1 delivery this update; channel 1's ordered delivery then
// drains its held code-1 message, which finds the share full: the hold goes
// back to the free stack, never leaked.
// mutation gap: hunt 1/4584 (held_sequenced = slot before the release deleted)

static void test_gaps_r2_ch_sequenced_drain_on_a_full_share_returns_the_hold()
{
    rcv_node_t node;
    rcv_init( &node, 32768, 1 );
    const uint8_t byte = 0x51;

    const uint32_t free_before = node.region->sequenced_hold_free_count;
    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, 0, &byte, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 2, 0, 0, &byte, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );

    const rocketnet_message_record_t ordered = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 1, 0, 0, &byte, 1, 0 );
    rcv_accept( &node, ordered );
    const uint64_t full = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
    check( rcv_dispatch( &node, ordered ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rn_ordered_end_update( &node.ordered );

    if ( node.region->sequenced_hold_free_count != free_before || rcv_rx( &node, 1 )->held_sequenced != RN_DESCRIPTOR_NONE )
    {
        printf( "a held code-1 message the drain could not deliver (share full) leaked its hold: %u of %u free\n",
                node.region->sequenced_hold_free_count, free_before );
    }
    check( node.region->sequenced_hold_free_count == free_before );
    check( rcv_rx( &node, 1 )->held_sequenced == RN_DESCRIPTOR_NONE );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL ) == full + 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_drain_on_a_full_share_returns_the_hold );

// A complete future code-4 block whose sequencing is behind the new epoch's
// watermark and whose span names a descriptor it does not own: the drain's
// delivery is refused, the release fails, and nothing is counted stale.
// mutation gap: hunt 1/4589 (the refused hold's failed release: return false deleted)

static void test_gaps_r2_ch_sequenced_drain_refused_hold_failed_release()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x2A, sizeof( chunk ) );
    const uint8_t byte = 1;
    const uint32_t sequencing = RCV_HALF + 1;

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 1, sequencing, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t c1 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 1, 0, 1, sequencing, 1, 2, chunk, 30, 0 );
    const rocketnet_message_record_t ordered = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 0, 0, 0, &byte, 1, 0 );
    rcv_accept( &node, c0 );
    rcv_accept( &node, c1 );
    rcv_accept( &node, ordered );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_dispatch( &node, c1 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_rx( &node, 0 )->held_sequenced != RN_DESCRIPTOR_NONE );

    node.region->receive_descriptors[1].channel = 1;                    // number 1 belongs to another record now

    const uint64_t bad_block = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
    const uint64_t stale = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
    check( rcv_dispatch( &node, ordered ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    if ( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) != bad_block + 1 ||
         rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) != stale )
    {
        printf( "the drain counted a refused hold stale after its release failed MESSAGE_BAD_BLOCK\n" );
    }
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ) == bad_block + 1 );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) == stale );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_drain_refused_hold_failed_release );

// ---------------------------------------------------------------------------
// the window's accept starts a descriptor's block bookkeeping empty
// ---------------------------------------------------------------------------

// A future code-4 chunk at number 0 anchors a two-chunk block (block_count 2 on
// slot 0) and is superseded, which leaves slot 0 not live with that count.
// Number 64 reuses slot 0 as a small code-2 message and number 65 is a code-3
// message held on channel 1. When the update ends, freeing number 64 must not
// free 65 as if 64 were a block: 65 is still held and drains intact.
// mutation gap: hunt 1/761 (accept's block_count = 0 deleted)

static void test_gaps_r2_ch_reused_slot_starts_with_no_block()
{
    rcv_node_t node;
    rcv_init( &node );
    uint8_t chunk[RCV_F];
    memset( chunk, 0x2B, sizeof( chunk ) );
    const uint8_t small = 0x61;
    const uint8_t held = 0x62;
    const uint8_t turn = 0x63;
    const uint8_t newer = 0x64;

    const rocketnet_message_record_t c0 = rcv_chunk( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 1, 0, 0, 2, chunk, (int) RCV_F, 0 );
    const rocketnet_message_record_t one = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, 0, 0, &small, 1, 0 );
    rcv_accept( &node, c0 );
    check( rcv_dispatch( &node, c0 ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 2, 0, &newer, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( node.region->receive_descriptors[0].live == 0 );
    check( node.region->receive_descriptors[0].block_count == 2 );     // the superseded anchor's count stays on the slot
    rcv_accept( &node, one );
    check( rcv_dispatch( &node, one ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rn_ordered_end_update( &node.ordered );
    check( node.region->receive_descriptors[1].live == 0 );

    const rocketnet_message_record_t reuse = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 64, 0, 0, 0, &small, 1, 0 );
    const rocketnet_message_record_t later = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 65, 1, 1, 0, &held, 1, 500 );
    rcv_accept( &node, reuse );
    rcv_accept( &node, later );
    check( rcv_dispatch( &node, reuse ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( rcv_dispatch( &node, later ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rn_ordered_end_update( &node.ordered );

    if ( !node.region->receive_descriptors[1].live )
    {
        printf( "the end of the update freed held number 65 with number 64: slot 0 kept a released block's count after the accept\n" );
    }
    check( node.region->receive_descriptors[1].live != 0 );

    // channel 1's in-turn message (dispatched directly: slot 2 was never taken) drains 65

    const rocketnet_message_record_t trigger = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 66, 1, 0, 0, &turn, 1, 0 );
    check( rcv_dispatch( &node, trigger ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( node.ordered.channels.deliveries == 2 );
    check( node.region->deliveries[1].bytes == 1 && node.region->deliveries[1].data[0] == held );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_reused_slot_starts_with_no_block );

// A delivery whose descriptor is no longer live when the update ends (the
// state rn_sequenced_release_slot leaves behind: live cleared, block_count
// kept) frees nothing: number 1, held on channel 1, stays live.
// mutation gap: hunt 1/3532 (the release's return for a descriptor not live deleted)

static void test_gaps_r2_ch_end_update_skips_a_released_delivery()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 0x71;

    const rocketnet_message_record_t held = rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 1, 0, &byte, 1, 200 );
    rcv_accept( &node, held );
    check( rcv_dispatch( &node, held ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    rn_receive_descriptor_t * released = &node.region->receive_descriptors[0];
    released->live = 0;
    released->number = 0;
    released->block_count = 2;
    released->block_have = 2;
    node.region->deliveries[0].descriptor = 0;
    node.ordered.channels.deliveries = 1;

    rn_ordered_end_update( &node.ordered );
    if ( !node.region->receive_descriptors[1].live )
    {
        printf( "the end of the update walked a released delivery's stale block count and freed held number 1\n" );
    }
    check( node.region->receive_descriptors[1].live != 0 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_end_update_skips_a_released_delivery );

// A future code-1 hold at ordering 3 the read index has passed (read index
// 4); a record at ordering 3 + HALF is ahead of the read index (by HALF - 1)
// but exactly half the range from the hold: not newer than it, so stale,
// and the hold is kept.
// mutation gap: hunt 1/4498 (gap >= HALF to > against the hold)

static void test_gaps_r2_ch_sequenced_half_range_from_the_hold_ordering_is_stale()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t first = 0x81;
    const uint8_t second = 0x82;

    check( rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 3, 0, &first, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    const uint32_t hold = rcv_rx( &node, 1 )->held_sequenced;
    check( hold != RN_DESCRIPTOR_NONE );
    rn_channels_deliver_ordered( &node.ordered.channels, 1, 3 );

    const uint64_t superseded = rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
    const int result = rcv_dispatch( &node, rcv_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, ( 3 + RCV_HALF ) & ROCKETNET_MESSAGE_U24_MASK, 0, &second, 1, 0 ) );
    const uint32_t now_held = rcv_rx( &node, 1 )->held_sequenced;
    if ( result != ROCKETNET_CHANNEL_DISPATCH_STALE || now_held != hold || node.region->sequenced_message_payloads[hold].data[0] != first )
    {
        printf( "a record half the ordering range from the hold replaced it (result %d)\n", result );
    }
    check( result == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( now_held == hold );
    check( node.region->sequenced_message_payloads[hold].data[0] == first );
    check( rcv_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED ) == superseded + 1 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_sequenced_half_range_from_the_hold_ordering_is_stale );

#if ROCKETNET_DEV

// rn_window_receive_release frees only the exact live number: a stale number
// whose slot now holds number + W leaves that descriptor live.
// mutation gap: hunt 1/781 (live && number == n to ||); dev-only helper

static void test_gaps_r2_ch_window_release_of_a_stale_number()
{
    rcv_node_t node;
    rcv_init( &node );
    const uint8_t byte = 1;

    rcv_accept( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, &byte, 1, 0 ) );
    rn_window_receive_release( &node.ordered.receive, 0 );
    check( node.region->receive_descriptors[0].live == 0 );
    rcv_accept( &node, rcv_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 64, 0, 0, 0, &byte, 1, 0 ) );
    check( node.region->receive_descriptors[0].live != 0 && node.region->receive_descriptors[0].number == 64 );

    rn_window_receive_release( &node.ordered.receive, 0 );
    if ( !node.region->receive_descriptors[0].live )
    {
        printf( "releasing stale number 0 freed number 64's live descriptor in the same slot\n" );
    }
    check( node.region->receive_descriptors[0].live != 0 );
    rcv_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_window_release_of_a_stale_number );

#endif // #if ROCKETNET_DEV

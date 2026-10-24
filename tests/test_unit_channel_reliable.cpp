/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include <string.h>
#include <stdlib.h>

#include "test_unit_registry.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_message.h"

#define check rocketnet_unit_check

static void test_reliable_disabled_before_and_enabled_after()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
    check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_channels_t disabled;
    check( rn_channels_init( &disabled, region, 1, 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED ) );
    check( rn_channels_start( &disabled, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE, 16 ) == NULL );

    rn_channels_t enabled;
    check( rn_channels_init( &enabled, region, 1, 0 ) );
    uint8_t * payload = rn_channels_start( &enabled, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE, 16 );
    check( payload != NULL );
    memset( payload, 0xA5, 16 );
    check( rn_channels_finish( &enabled, 16 ) );
    rn_arena_destroy( arena );
}

REGISTER_UNIT_TEST( test_reliable_disabled_before_and_enabled_after );

struct reliable_test_packet_t
{
    uint64_t id;
    int bytes;
    uint8_t bytes_pad[16 - sizeof( uint64_t ) - sizeof( int )];                 // explicit: data starts 16-aligned
    alignas( 16 ) uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t tail_pad[16 - ROCKETNET_MAX_PACKET_PAYLOAD_BYTES % 16];                     // explicit: the size rounds up to 16
};
static_assert( sizeof( reliable_test_packet_t ) == 16 + ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 15 ) / 16 * 16, "no implicit padding" );
static_assert( alignof( reliable_test_packet_t ) == 16, "data is 16-aligned" );

struct reliable_test_node_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    uint64_t next_packet_id;
};

static void reliable_test_init( reliable_test_node_t * node, const rocketnet_message_config_t * config )
{
    memset( node, 0, sizeof( *node ) );
    node->arena = rn_arena_create( NULL, config, 1, &node->counters );
    check( node->arena != NULL );
    node->region = rn_arena_region( node->arena, 0 );
    check( rn_ordered_init( &node->ordered, node->region, config, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    node->next_packet_id = 1;
}

static void reliable_test_send( reliable_test_node_t * node, const uint8_t * data, uint32_t bytes )
{
    uint8_t * pointer = rn_channels_start( &node->ordered.channels, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE, bytes );
    check( pointer != NULL );
    memcpy( pointer, data, bytes );
    check( rn_channels_finish( &node->ordered.channels, bytes ) );
}

static int reliable_test_write( reliable_test_node_t * node, double now, reliable_test_packet_t * packets, int capacity )
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

static void reliable_test_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->reliable_messages_in_flight = 1024;
    config->receive_message_buffer_bytes = 262144;
    config->max_message_bytes = 262144;
    config->send_message_buffer_bytes = 800000;
    config->message_resend_delay_min_ms = 20;
    config->message_resend_delay_initial_ms = 100;
    config->message_resend_delay_max_ms = 400;
}

static void test_reliable_exactly_once_any_order_under_loss_dup_reorder()
{
    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t sender, receiver;
    reliable_test_init( &sender, &config );
    reliable_test_init( &receiver, &config );
    reliable_test_packet_t packets[3];
    const uint8_t values[3] = { 11, 22, 33 };

    for ( int i = 0; i < 3; i++ )
    {
        reliable_test_send( &sender, &values[i], 1 );
        check( reliable_test_write( &sender, 1.0 + i * 0.01, &packets[i], 1 ) == 1 );
    }

    // The first number is lost initially. Later numbers deliver without it.
    for ( int i = 2; i >= 0; i-- )
    {
        check( rn_ordered_receive( &receiver.ordered, packets[i].data, packets[i].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( rn_ordered_deliveries( &receiver.ordered ) == 1 );
        const rn_delivery_t & delivery = receiver.region->deliveries[0];
        check( delivery.code == ROCKETNET_CHANNEL_TYPE_RELIABLE );
        check( delivery.channel == 0 && delivery.bytes == 1 && delivery.data[0] == values[i] );
        check( delivery.data >= packets[i].data && delivery.data < packets[i].data + packets[i].bytes );
        rn_ordered_end_update( &receiver.ordered );
    }

    // A different packet carrying the same reliable number is still a duplicate.
    reliable_test_packet_t resent[3];
    const int n = reliable_test_write( &sender, 1.5, resent, 3 );
    check( n > 0 );
    for ( int i = 0; i < n; i++ )
    {
        check( rn_ordered_receive( &receiver.ordered, resent[i].data, resent[i].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( rn_ordered_deliveries( &receiver.ordered ) == 0 );
        rn_ordered_end_update( &receiver.ordered );
    }
    check( receiver.counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE] >= 1 );
    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_reliable_exactly_once_any_order_under_loss_dup_reorder );

static void test_reliable_chunked_delivered_whole()
{
    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t sender, receiver;
    reliable_test_init( &sender, &config );
    reliable_test_init( &receiver, &config );
    const uint32_t bytes = 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17;
    uint8_t data[2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17];
    for ( uint32_t i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( i * 7 + 3 );
    reliable_test_send( &sender, data, bytes );
    reliable_test_packet_t packets[4];
    const int n = reliable_test_write( &sender, 1.0, packets, 4 );
    check( n == 3 );
    for ( int i = 2; i >= 0; i-- )
    {
        check( rn_ordered_receive( &receiver.ordered, packets[i].data, packets[i].bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( rn_ordered_deliveries( &receiver.ordered ) == ( i == 0 ? 1u : 0u ) );
        if ( i == 0 )
        {
            const rn_delivery_t & delivery = receiver.region->deliveries[0];
            check( delivery.code == ROCKETNET_CHANNEL_TYPE_RELIABLE && delivery.bytes == (int) bytes );
            check( delivery.data >= receiver.region->receive_ring && delivery.data < receiver.region->receive_ring + receiver.region->ring_bytes );
            check( memcmp( delivery.data, data, bytes ) == 0 );
        }
        rn_ordered_end_update( &receiver.ordered );
    }
    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
}

REGISTER_UNIT_TEST( test_reliable_chunked_delivered_whole );

static void test_reliable_packet_fit_boundary()
{
    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t sender;
    reliable_test_init( &sender, &config );
    uint8_t data[1409];
    memset( data, 0x5A, sizeof( data ) );
    reliable_test_packet_t packets[2];

    reliable_test_send( &sender, data, 1408 );
    check( sender.region->send_descriptors[sender.region->channel_send[0].queue_head].fragments == 1 );
    check( reliable_test_write( &sender, 1.0, packets, 2 ) == 1 );
    rocketnet_message_reader_t reader;
    rocketnet_message_record_t record;
    check( rocketnet_message_reader_begin( &reader, packets[0].data, packets[0].bytes, &sender.ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rocketnet_message_reader_next( &reader, &record ) && record.code == ROCKETNET_MESSAGE_CODE_RELIABLE && !record.block && record.bytes == 1408 );

    reliable_test_send( &sender, data, 1409 );
    check( sender.region->send_descriptors[sender.region->channel_send[0].queue_head].fragments == 2 );
    check( reliable_test_write( &sender, 1.01, packets, 2 ) == 2 );
    for ( int i = 0; i < 2; i++ )
    {
        check( rocketnet_message_reader_begin( &reader, packets[i].data, packets[i].bytes, &sender.ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        check( rocketnet_message_reader_next( &reader, &record ) && record.code == ROCKETNET_MESSAGE_CODE_RELIABLE && record.block );
        check( record.fragment == (uint32_t) i && record.fragments == 1 );
    }

    // A sender may schedule code 2 on a valid channel, but the wire has no
    // channel field and the receiver presents it on channel 0.
    check( sender.region->num_channels > 1 );
    uint8_t * other = rn_channels_start( &sender.ordered.channels, 1, ROCKETNET_CHANNEL_TYPE_RELIABLE, 1 );
    check( other != NULL );
    *other = 9;
    check( rn_channels_finish( &sender.ordered.channels, 1 ) );
    check( reliable_test_write( &sender, 1.02, packets, 1 ) == 1 );
    check( rocketnet_message_reader_begin( &reader, packets[0].data, packets[0].bytes, &sender.ordered.limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rocketnet_message_reader_next( &reader, &record ) && record.code == ROCKETNET_MESSAGE_CODE_RELIABLE && record.channel == 0 );
    rn_arena_destroy( sender.arena );
}

REGISTER_UNIT_TEST( test_reliable_packet_fit_boundary );

static void test_rnchannel_reliable_witness_replays()
{
    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t node;
    reliable_test_init( &node, &config );

    // RNChannelTypes BrokenReliableTwice: a new packet id cannot make the same
    // reliable number fresh. The receive window refuses the second arrival.
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    const uint8_t byte = 7;
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.data = &byte;
    record.bytes = 1;
    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_DUPLICATE );

    // Exactly one window beyond the next read number is a violation, not a
    // stale or fresh application delivery.
    record.number = 1 + node.region->window;
    check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_VIOLATION );
    check( node.ordered.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW );

    // At the 24-bit wrap, adjacent numbers remain adjacent in the receiver's
    // reconstructed sequence. A retry of the pre-wrap number is still stale.
    rn_region_reset( node.region );
    rn_window_receive_init( &node.ordered.receive, node.region, ROCKETNET_MESSAGE_U24_MASK );
    record.number = ROCKETNET_MESSAGE_U24_MASK;
    check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    rn_window_receive_release( &node.ordered.receive, ROCKETNET_MESSAGE_U24_MASK );
    record.number = 0;
    check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    record.number = ROCKETNET_MESSAGE_U24_MASK;
    check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_DUPLICATE );
    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_rnchannel_reliable_witness_replays );

static uint64_t reliable_allocator_calls;

static void * reliable_counted_malloc( void * context, size_t bytes )
{
    (void) context;
    reliable_allocator_calls++;
    return malloc( bytes );
}

static void reliable_counted_free( void * context, void * memory )
{
    (void) context;
    reliable_allocator_calls++;
    free( memory );
}

static void test_reliable_no_allocation_after_create()
{
    rocketnet_term();
    check( rocketnet_init( NULL, reliable_counted_malloc, reliable_counted_free ) == ROCKETNET_STATUS_OK );

    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t sender, receiver;
    reliable_test_init( &sender, &config );
    reliable_test_init( &receiver, &config );
    const uint64_t created = reliable_allocator_calls;

    const uint8_t data[3] = { 1, 2, 3 };
    reliable_test_send( &sender, data, 3 );
    reliable_test_packet_t packet;
    check( reliable_test_write( &sender, 1.0, &packet, 1 ) == 1 );
    check( rn_ordered_receive( &receiver.ordered, packet.data, packet.bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( receiver.region->deliveries[0].data >= packet.data && receiver.region->deliveries[0].data < packet.data + packet.bytes );
    rn_ordered_end_update( &receiver.ordered );
    check( rn_ordered_acks( &sender.ordered, &packet.id, 1, 1.01 ) == 1 );
    check( reliable_allocator_calls == created );

    rn_arena_destroy( sender.arena );
    rn_arena_destroy( receiver.arena );
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_reliable_no_allocation_after_create );

// A control record frees its number's descriptor and is never an application
// view (rn_channels_dispatch_reliable, sdk/rocketnet_channel_reliable.cpp).
static void test_reliable_control_record_frees_its_number()
{
    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t node;
    reliable_test_init( &node, &config );

    static const uint8_t data[16] = { 0 };
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.channel = 0;
    record.number = 3;
    record.control = 1;
    record.data = data;
    record.bytes = 16;

    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &node.ordered.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( node.region->receive_descriptors[slot].live == 1 );

    rn_delivery_t delivery;
    check( rn_channels_dispatch( &node.ordered.channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_CONTROL );
    check( node.region->receive_descriptors[slot].live == 0 );
    check( node.ordered.channels.deliveries == 0 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );

    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_reliable_control_record_frees_its_number );

// A channel set whose mask does not enable code 2 refuses it
// (rn_channels_dispatch, sdk/rocketnet_channel.cpp, by wire code): counted
// TYPE_NOT_ENABLED, the error set, and no delivery.
static void test_reliable_record_on_a_disabled_mask_is_refused()
{
    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t node;
    reliable_test_init( &node, &config );

    rn_channels_t ch;
    check( rn_channels_init( &ch, node.region, 1, 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED ) );

    static const uint8_t data[16] = { 0 };
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.channel = 0;
    record.number = 0;
    record.data = data;
    record.bytes = 16;

    const uint64_t before = node.counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED];
    rn_delivery_t delivery;
    check( rn_channels_dispatch( &ch, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED] == before + 1 );
    check( ch.deliveries == 0 );
    check( ch.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_reliable_record_on_a_disabled_mask_is_refused );

// A record whose number layer 3 never accepted is a violation
// (rn_channels_dispatch_reliable, sdk/rocketnet_channel_reliable.cpp): counted
// PROTOCOL_VIOLATION, the error set, and no delivery.
static void test_reliable_record_without_a_live_number_is_refused()
{
    rocketnet_message_config_t config;
    reliable_test_config( &config );
    reliable_test_node_t node;
    reliable_test_init( &node, &config );

    static const uint8_t data[16] = { 0 };
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.channel = 0;
    record.number = 7;
    record.data = data;
    record.bytes = 16;

    const uint64_t before = node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION];
    rn_delivery_t delivery;
    check( rn_channels_dispatch( &node.ordered.channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == before + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( node.ordered.channels.deliveries == 0 );

    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_reliable_record_without_a_live_number_is_refused );

// ---------------------------------------------------------------
//        code-2 blocks: one delivery, linear work in the chunk count
// ---------------------------------------------------------------

// Each chunk of a code-2 block is accepted by layer 3 and dispatched as it
// arrives (rn_channels_dispatch_reliable). The records are built here, not
// parsed, so the block's size is the receiver's limit and nothing else.

static void reliable_block_config( rocketnet_message_config_t * config )
{
    reliable_test_config( config );
    config->max_message_bytes = 1048576;
    config->receive_message_buffer_bytes = 1048576;
    config->send_message_buffer_bytes = 8388608;
}

static rocketnet_message_record_t reliable_block_record( const uint8_t * data, uint32_t total, uint32_t first_number, uint32_t index )
{
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = ( total + F - 1 ) / F;
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.block = 1;
    record.fragments = count - 1;
    record.fragment = index;
    record.number = first_number + index;
    record.ring = index * ( F / 16 );
    record.data = data + (uint64_t) index * F;
    record.bytes = (int) ( index + 1 < count ? F : total - index * F );
    return record;
}

static int reliable_block_arrive( reliable_test_node_t * node, const rocketnet_message_record_t * record, rn_delivery_t * delivery )
{
    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &node->ordered.receive, record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    return rn_channels_dispatch( &node->ordered.channels, record, delivery );
}

enum { RELIABLE_BLOCK_IN_ORDER, RELIABLE_BLOCK_REVERSED, RELIABLE_BLOCK_SHUFFLED };

static void reliable_block_case( int order )
{
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t total = 1048576;
    const uint32_t count = ( total + F - 1 ) / F;

    rocketnet_message_config_t config;
    reliable_block_config( &config );
    reliable_test_node_t node;
    reliable_test_init( &node, &config );
    check( count <= node.region->window );

    uint8_t * data = (uint8_t*) malloc( total );
    uint32_t * arrival = (uint32_t*) malloc( count * sizeof( uint32_t ) );
    check( data && arrival );
    for ( uint32_t i = 0; i < total; i++ )
        data[i] = (uint8_t) ( i * 131 + ( i >> 8 ) * 7 + 1 );
    for ( uint32_t i = 0; i < count; i++ )
        arrival[i] = order == RELIABLE_BLOCK_REVERSED ? count - 1 - i : i;
    if ( order == RELIABLE_BLOCK_SHUFFLED )
    {
        uint32_t seed = 0x2545F491u;
        for ( uint32_t i = count - 1; i > 0; i-- )
        {
            seed = seed * 1664525u + 1013904223u;
            const uint32_t j = ( seed >> 8 ) % ( i + 1 );
            const uint32_t swap = arrival[i];
            arrival[i] = arrival[j];
            arrival[j] = swap;
        }
    }

#if defined(ROCKETNET_BLOCK_COST_TEST_HOOKS)
    rn_channels_block_cost_reset();
#endif

    uint32_t delivered = 0;
    for ( uint32_t i = 0; i < count; i++ )
    {
        const rocketnet_message_record_t record = reliable_block_record( data, total, 0, arrival[i] );
        rn_delivery_t delivery;
        const int result = reliable_block_arrive( &node, &record, &delivery );
        if ( i + 1 < count )
        {
            check( result == ROCKETNET_CHANNEL_DISPATCH_HELD );
            continue;
        }
        check( result == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        check( delivery.code == ROCKETNET_CHANNEL_TYPE_RELIABLE && delivery.bytes == (int) total );
        check( delivery.data == node.region->receive_ring );
        check( memcmp( delivery.data, data, total ) == 0 );
        delivered++;
    }
    check( delivered == 1 );
    check( node.ordered.channels.deliveries == 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK] == 0 );

#if defined(ROCKETNET_BLOCK_COST_TEST_HOOKS)
    // Every chunk does bounded work: the block's completion never rescans the
    // descriptors already in (a 1 MiB block in order was ~415k visits).
    const rn_block_cost_t cost = rn_channels_block_cost_read();
    check( cost.chunks == count );
    check( cost.descriptor_visits <= 3 * (uint64_t) count );
#endif

    // The update's end releases every number of the block, and no block
    // state outlives it.
    rn_ordered_end_update( &node.ordered );
    for ( uint32_t i = 0; i < count; i++ )
        check( node.region->receive_descriptors[i].live == 0 );
    for ( uint32_t i = 0; i < node.region->held_entries; i++ )
        check( node.region->held_index[i].used == 0 );

    free( arrival );
    free( data );
    rn_arena_destroy( node.arena );
}

static void test_reliable_block_in_order_delivered_once()
{
    reliable_block_case( RELIABLE_BLOCK_IN_ORDER );
}

static void test_reliable_block_reversed_delivered_once()
{
    reliable_block_case( RELIABLE_BLOCK_REVERSED );
}

static void test_reliable_block_shuffled_delivered_once()
{
    reliable_block_case( RELIABLE_BLOCK_SHUFFLED );
}

REGISTER_UNIT_TEST( test_reliable_block_in_order_delivered_once );
REGISTER_UNIT_TEST( test_reliable_block_reversed_delivered_once );
REGISTER_UNIT_TEST( test_reliable_block_shuffled_delivered_once );

// A chunk that disagrees with the rest of its block is refused when it
// completes the block: MESSAGE_BAD_BLOCK counted once, the error set, and
// nothing delivered.

enum { RELIABLE_BLOCK_WRONG_RING, RELIABLE_BLOCK_WRONG_COUNT };

static void reliable_block_refused_case( int wrong )
{
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t total = 2 * F + 100;

    rocketnet_message_config_t config;
    reliable_block_config( &config );
    reliable_test_node_t node;
    reliable_test_init( &node, &config );

    static uint8_t data[4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    memset( data, 0x3C, sizeof( data ) );

    rn_delivery_t delivery;
    for ( uint32_t i = 0; i < 2; i++ )
    {
        const rocketnet_message_record_t record = reliable_block_record( data, total, 0, i );
        check( reliable_block_arrive( &node, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    }

    rocketnet_message_record_t last = reliable_block_record( data, total, 0, 2 );
    if ( wrong == RELIABLE_BLOCK_WRONG_RING )
        last.ring += F / 16;                    // the block's base moves by one chunk
    else
    {
        last.fragments = 3;                     // a block of four, where this chunk is not the last
        last.bytes = (int) F;
    }

    const uint64_t before = node.counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK];
    check( reliable_block_arrive( &node, &last, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK] == before + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( node.ordered.channels.deliveries == 0 );

    rn_arena_destroy( node.arena );
}

static void test_reliable_block_chunk_with_wrong_ring_is_refused()
{
    reliable_block_refused_case( RELIABLE_BLOCK_WRONG_RING );
}

static void test_reliable_block_chunk_with_wrong_count_is_refused()
{
    reliable_block_refused_case( RELIABLE_BLOCK_WRONG_COUNT );
}

REGISTER_UNIT_TEST( test_reliable_block_chunk_with_wrong_ring_is_refused );
REGISTER_UNIT_TEST( test_reliable_block_chunk_with_wrong_count_is_refused );

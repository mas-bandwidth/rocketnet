/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

#include "test_unit_registry.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_test_seams.h"
#include <string.h>

#if ROCKETNET_TEST_SEAMS
extern thread_local unsigned long long g_sequenced_hold_visit_count;
#endif

static rocketnet_message_record_t hot_cold_record( int code, uint32_t channel,
                                                   uint32_t ordering, uint32_t sequencing, const uint8_t * data, int bytes )
{
    rocketnet_message_record_t record = {};
    record.code = code;
    record.channel = channel;
    record.ordering = ordering;
    record.sequencing = sequencing;
    record.data = data;
    record.bytes = bytes;
    return record;
}

static void test_sequenced_hot_cold_layout_and_reset()
{
    // Odd and maximum hold counts catch incorrect carve offsets and a lost
    // cold-payload term in the exact planner, including the empty part.
    const int hold_counts[] = { 0, 1, 3, 16, 4096 };
    for ( int count : hold_counts )
    {
        rocketnet_message_config_t config;
        rocketnet_message_default_config( &config );
        config.sequenced_messages_held = count;
        rn_region_layout_t layout;
        rocketnet_unit_check( rn_region_layout( &config, &layout ) );
        rocketnet_unit_check( layout.bytes[RN_PART_SEQUENCED_HOLDS] == (uint64_t) count * 1440 );
        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
        rocketnet_unit_check( arena != NULL );
        rn_region_t * region = rn_arena_region( arena, 0 );
        rocketnet_unit_check( sizeof( rn_sequenced_hold_t ) == 16 );
        const uint8_t * hot = (const uint8_t *) region->sequenced_messages_held;
        uint8_t * cold = (uint8_t *) region->sequenced_message_payloads;
        rocketnet_unit_check( hot == region->base + layout.offset[RN_PART_SEQUENCED_HOLDS] );
        rocketnet_unit_check( cold == hot + (size_t) count * sizeof( rn_sequenced_hold_t ) );
        rocketnet_unit_check( cold + (size_t) count * sizeof( rn_sequenced_payload_t ) +
                                  (size_t) count * 2 * sizeof( uint32_t ) ==
                              hot + layout.bytes[RN_PART_SEQUENCED_HOLDS] );
        rocketnet_unit_check( (const uint8_t *) region->sequenced_hold_free_stack ==
                              cold + (size_t) count * sizeof( rn_sequenced_payload_t ) );
        rocketnet_unit_check( (const uint8_t *) region->sequenced_holds_delivered ==
                              cold + (size_t) count * sizeof( rn_sequenced_payload_t ) + (size_t) count * sizeof( uint32_t ) );
        if ( count )
        {
            memset( cold, 0xA7, (size_t) count * sizeof( rn_sequenced_payload_t ) );
            for ( int slot = 0; slot < count; ++slot )
            {
                region->sequenced_messages_held[slot].used = (uint8_t) ( 1 + slot % 2 );
                region->sequenced_messages_held[slot].bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
            }
            rn_region_reset( region );
            for ( int slot = 0; slot < count; ++slot )
            {
                const rn_sequenced_hold_t & hold = region->sequenced_messages_held[slot];
                rocketnet_unit_check( hold.used == 0 && hold.bytes == 0 );
                // Reset invalidates metadata without touching cold payloads.
                for ( int byte = 0; byte < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES; ++byte )
                    rocketnet_unit_check( region->sequenced_message_payloads[slot].data[byte] == 0xA7 );
            }
        }
        rn_arena_destroy( arena );
    }
}

static void test_sequenced_hot_cold_view_lifetime()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.sequenced_messages_held = 1;
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    uint8_t original[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    memset( original, 0x35, sizeof( original ) );
    rocketnet_message_record_t record = hot_cold_record(
        ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, original, sizeof( original ) );
    rn_delivery_t delivery;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    memset( original, 0xF1, sizeof( original ) ); // the input batch is already reusable

    const uint8_t ordered = 0x11;
    record = hot_cold_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, &ordered, 1 );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( channels.deliveries == 2 );
    const rn_delivery_t & held_delivery = region->deliveries[1];
    const uint8_t * held_view = held_delivery.data;
    rocketnet_unit_check( held_delivery.bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    rocketnet_unit_check( held_view == region->sequenced_message_payloads[0].data );
    rocketnet_unit_check( region->sequenced_messages_held[0].used == 2 );

    // A delivered slot is still pinned. A future state on another channel
    // cannot reuse its cold storage until the documented view expiry.
    const uint8_t next = 0x62;
    record = hot_cold_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 1, 1, 0, &next, 1 );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    for ( int byte = 0; byte < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES; ++byte )
        rocketnet_unit_check( held_view[byte] == 0x35 );
    rn_channels_end_update( &channels );
    rocketnet_unit_check( region->sequenced_messages_held[0].used == 0 );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rocketnet_unit_check( region->sequenced_messages_held[0].used == 1 );
    rocketnet_unit_check( region->sequenced_message_payloads[0].data[0] == next );
    rn_arena_destroy( arena );
}

static void test_sequenced_hot_cold_payload_slots_are_independent()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.sequenced_messages_held = 3;
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );
    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_delivery_t delivery;
    for ( uint32_t channel = 0; channel < 3; ++channel )
    {
        memset( payload, (int) channel + 0x40, sizeof( payload ) );
        rocketnet_message_record_t record = hot_cold_record(
            ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, channel, 1, 0, payload, sizeof( payload ) );
        rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    }
    // Superseding one held state must not change the adjacent cold payloads
    // or their compact metadata, even at the full payload boundary.
    memset( payload, 0x73, sizeof( payload ) );
    rocketnet_message_record_t replacement = hot_cold_record(
        ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 1, 1, 1, payload, sizeof( payload ) );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &replacement, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    for ( uint32_t channel = 0; channel < 3; ++channel )
    {
        const uint32_t slot = region->channel_receive[channel].held_sequenced;
        rocketnet_unit_check( slot < region->holds );
        const rn_sequenced_hold_t & hold = region->sequenced_messages_held[slot];
        rocketnet_unit_check( hold.used == 1 && hold.channel == channel && hold.ordering == 1 );
        rocketnet_unit_check( hold.sequencing == ( channel == 1 ? 1u : 0u ) );
        const uint8_t expected = (uint8_t) ( channel == 1 ? 0x73 : channel + 0x40 );
        for ( int byte = 0; byte < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES; ++byte )
            rocketnet_unit_check( region->sequenced_message_payloads[slot].data[byte] == expected );
    }
    rn_arena_destroy( arena );
}

static void test_sequenced_hold_pool_constant_time()
{
    // The hold pool at its configured maximum: 4096 holds are taken and
    // delivered in one update, the 4097th take is refused and counted
    // MESSAGE_SEQUENCED_HOLD_FULL, the delivered holds return at the end of
    // the update and are reusable, and an update with no delivered hold
    // visits no hold entry (RNChannel BuffersBounded and EndUpdate: the
    // pool's bounds, not how a free slot is found).
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate explicit capacity
    config.sequenced_messages_held = 4096;
    config.unreliable_messages_per_update = 8192;
    config.reliable_messages_in_flight = 8192;
    config.packets_in_flight = 8192;
    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rocketnet_unit_check( region->holds == 4096 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint8_t held_byte = 0x5A, ordered_byte = 0x4B;
    rn_delivery_t delivery;
    // Each round holds one future message on every channel, then the ordered
    // message of its epoch delivers it; the delivered view stays live until
    // the end of the update, so the rounds accumulate delivered holds.
    for ( uint32_t round = 0; round < 128; ++round )
    {
        for ( uint32_t channel = 0; channel < region->num_channels; ++channel )
        {
            rocketnet_message_record_t record = hot_cold_record(
                ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, channel, round + 1, round, &held_byte, 1 );
            rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        }
        for ( uint32_t channel = 0; channel < region->num_channels; ++channel )
        {
            rocketnet_message_record_t record = hot_cold_record(
                ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, channel, round, 0, &ordered_byte, 1 );
            rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        }
    }
    rocketnet_unit_check( channels.deliveries == 8192 );

    // The pool is full: one more future message is refused and counted.
    const uint64_t full_before = counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL];
    rocketnet_message_record_t record = hot_cold_record(
        ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 129, 128, &held_byte, 1 );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL] == full_before + 1 );

    // The delivered holds return at the end of the update; the pass visits
    // exactly the delivered holds, no more.
#if ROCKETNET_TEST_SEAMS
    g_sequenced_hold_visit_count = 0;
#endif
    rn_channels_end_update( &channels );
#if ROCKETNET_TEST_SEAMS
    rocketnet_unit_check( g_sequenced_hold_visit_count == 4096 );
#endif
    for ( uint32_t slot = 0; slot < region->holds; ++slot )
        rocketnet_unit_check( region->sequenced_messages_held[slot].used == 0 );
    rocketnet_unit_check( region->sequenced_hold_free_count == region->holds );
    rocketnet_unit_check( region->sequenced_holds_delivered_count == 0 );

    // A returned hold is reusable, and its delivered view returns in turn.
    record = hot_cold_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 129, 128, &held_byte, 1 );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    record = hot_cold_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 128, 0, &ordered_byte, 1 );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rn_channels_end_update( &channels );
    for ( uint32_t slot = 0; slot < region->holds; ++slot )
        rocketnet_unit_check( region->sequenced_messages_held[slot].used == 0 );

    // An update with no delivered hold touches no hold entry, and a take
    // from the returned pool visits none either.
#if ROCKETNET_TEST_SEAMS
    g_sequenced_hold_visit_count = 0;
#endif
    rn_channels_end_update( &channels );
#if ROCKETNET_TEST_SEAMS
    rocketnet_unit_check( g_sequenced_hold_visit_count == 0 );
    record = hot_cold_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 130, 129, &held_byte, 1 );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &delivery ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rocketnet_unit_check( g_sequenced_hold_visit_count == 0 );
#endif
    rn_arena_destroy( arena );
}

REGISTER_UNIT_TEST( test_sequenced_hold_pool_constant_time );
REGISTER_UNIT_TEST( test_sequenced_hot_cold_layout_and_reset );
REGISTER_UNIT_TEST( test_sequenced_hot_cold_view_lifetime );
REGISTER_UNIT_TEST( test_sequenced_hot_cold_payload_slots_are_independent );

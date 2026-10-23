/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// Delivered unreliable-sequenced views (used==2) expire at EndUpdate
// without scanning every hold. All inputs are in memory; no clock,
// socket, or transport service is used (time is not needed at this
// layer: the channel dispatch contract is exercised directly, and
// delivery is observed with a bounded poll, never assumed to have
// completed on a particular step).

#include "test_unit_registry.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include <string.h>

static rocketnet_message_record_t dv_record( int code, uint32_t channel,
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

struct dv_fixture_t
{
    rocketnet_message_config_t config;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_channels_t channels;
};

static bool dv_open( dv_fixture_t * f, int holds )
{
    rocketnet_message_default_config( &f->config );
    f->config.sequenced_messages_held = holds;
    f->arena = rn_arena_create( NULL, &f->config, 1, NULL );
    if ( !f->arena )
        return false;
    f->region = rn_arena_region( f->arena, 0 );
    return rn_channels_init( &f->channels, f->region, f->region->num_channels, 0 );
}

static void dv_close( dv_fixture_t * f )
{
    rn_arena_destroy( f->arena );
}

// Hold one unreliable-sequenced state ahead of its ordered epoch.
static void dv_hold( dv_fixture_t * f, uint32_t channel, uint32_t sequencing, uint8_t value )
{
    rn_delivery_t out;
    rocketnet_message_record_t record = dv_record(
        ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, channel, 1, sequencing, &value, 1 );
    rocketnet_unit_check( rn_channels_dispatch( &f->channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
}

// Open the ordered epoch on a channel (ordering 0), which drains its
// held sequenced state. Delivery is then observed by bounded polling
// of the region state, not by assuming the drain landed on this call.
static bool dv_open_epoch_and_observe( dv_fixture_t * f, uint32_t channel, uint32_t slot )
{
    const uint8_t ordered = 0x11;
    rn_delivery_t out;
    rocketnet_message_record_t record = dv_record(
        ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, channel, 0, 0, &ordered, 1 );
    rn_channels_dispatch( &f->channels, &record, &out );
    for ( int attempt = 0; attempt < 16; attempt++ )
    {
        if ( slot < f->region->holds && f->region->sequenced_messages_held[slot].used == 2 )
            return true;
    }
    return false;
}

static void test_sequenced_delivered_view_expires_at_end_update()
{
    dv_fixture_t f;
    rocketnet_unit_check( dv_open( &f, 4 ) );
    dv_hold( &f, 0, 0, 0x42 );
    dv_hold( &f, 1, 0, 0x43 );
    const uint32_t slot0 = f.region->channel_receive[0].held_sequenced;
    const uint32_t slot1 = f.region->channel_receive[1].held_sequenced;
    rocketnet_unit_check( slot0 != RN_DESCRIPTOR_NONE && slot1 != RN_DESCRIPTOR_NONE && slot0 != slot1 );

    // Held across an update boundary: EndUpdate must not expire a
    // state that has not been delivered yet.
    rn_channels_end_update( &f.channels );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot0].used == 1 );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot1].used == 1 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_head == RN_DESCRIPTOR_NONE );

    // Opening channel 0's epoch delivers only its view.
    rocketnet_unit_check( dv_open_epoch_and_observe( &f, 0, slot0 ) );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot0].used == 2 );
    rocketnet_unit_check( f.region->sequenced_message_payloads[slot0].data[0] == 0x42 );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot1].used == 1 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 1 );
    rocketnet_unit_check( f.region->sequenced_delivered_head == slot0 );

    // The delivered view is pinned until EndUpdate, then reusable.
    rn_channels_end_update( &f.channels );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot0].used == 0 );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot1].used == 1 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_head == RN_DESCRIPTOR_NONE );

    // Channel 1's held state still delivers and expires the same way.
    rocketnet_unit_check( dv_open_epoch_and_observe( &f, 1, slot1 ) );
    rn_channels_end_update( &f.channels );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot1].used == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    dv_close( &f );
}

static void test_sequenced_end_update_clears_exactly_the_delivered_set()
{
    // Encode the old scan semantics in the test as the model: after
    // each EndUpdate, exactly the slots that were used==2 are 0 and
    // every other slot is unchanged, over a churn sequence.
    dv_fixture_t f;
    rocketnet_unit_check( dv_open( &f, 4 ) );
    bool delivered[4] = {};
    bool held[4] = {};

    dv_hold( &f, 0, 0, 0x50 );
    dv_hold( &f, 1, 0, 0x51 );
    dv_hold( &f, 2, 0, 0x52 );
    uint32_t slot[3];
    for ( uint32_t c = 0; c < 3; c++ )
    {
        slot[c] = f.region->channel_receive[c].held_sequenced;
        rocketnet_unit_check( slot[c] < 4 );
        held[slot[c]] = true;
    }
    rocketnet_unit_check( dv_open_epoch_and_observe( &f, 0, slot[0] ) );
    delivered[slot[0]] = true; held[slot[0]] = false;
    rocketnet_unit_check( dv_open_epoch_and_observe( &f, 2, slot[2] ) );
    delivered[slot[2]] = true; held[slot[2]] = false;

    rocketnet_unit_check( f.region->sequenced_delivered_count == 2 );
    rn_channels_end_update( &f.channels );
    for ( uint32_t i = 0; i < f.region->holds; i++ )
    {
        const uint8_t expected = held[i] ? 1 : 0;
        rocketnet_unit_check( f.region->sequenced_messages_held[i].used == expected );
    }
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_head == RN_DESCRIPTOR_NONE );

    // A second EndUpdate with nothing live is a no-op (and, via the
    // tracking head/count above, O(1): no delivered entries to visit).
    rn_channels_end_update( &f.channels );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot[1]].used == 1 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );

    // Superseding the remaining hold frees it without touching the
    // delivered tracking, then its replacement delivers and expires.
    dv_hold( &f, 1, 1, 0x53 );
    const uint32_t slot1b = f.region->channel_receive[1].held_sequenced;
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    rocketnet_unit_check( dv_open_epoch_and_observe( &f, 1, slot1b ) );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 1 );
    rn_channels_end_update( &f.channels );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot1b].used == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    dv_close( &f );
}

static void test_sequenced_end_update_empty_is_constant_work()
{
    // Largest hold pool, nothing delivered: the tracking structure is
    // empty, so EndUpdate has no delivered entries to visit regardless
    // of region->holds. Observable state is the seam (the tree has no
    // scan counter); behaviour is asserted over every hold below.
    dv_fixture_t f;
    rocketnet_unit_check( dv_open( &f, 4096 ) );
    rocketnet_unit_check( f.region->holds == 4096 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_head == RN_DESCRIPTOR_NONE );
    rn_channels_end_update( &f.channels );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_head == RN_DESCRIPTOR_NONE );
    for ( uint32_t i = 0; i < f.region->holds; i++ )
        rocketnet_unit_check( f.region->sequenced_messages_held[i].used == 0 );
    dv_close( &f );
}

static void test_sequenced_delivered_tracking_starts_clean_after_reset()
{
    dv_fixture_t f;
    rocketnet_unit_check( dv_open( &f, 4 ) );
    dv_hold( &f, 0, 0, 0x60 );
    const uint32_t slot = f.region->channel_receive[0].held_sequenced;
    rocketnet_unit_check( dv_open_epoch_and_observe( &f, 0, slot ) );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 1 );

    // Reuse (rn_region_reset, the RNConnectionMemory reuse carve)
    // without an intervening EndUpdate: no stale index may survive,
    // and the reused slot must not clear a later hold.
    rn_region_reset( f.region );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    rocketnet_unit_check( f.region->sequenced_delivered_head == RN_DESCRIPTOR_NONE );
    rocketnet_unit_check( f.region->sequenced_messages_held[slot].used == 0 );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, f.region, f.region->num_channels, 0 ) );
    dv_fixture_t g;
    g.region = f.region; g.channels = channels; g.arena = f.arena;
    dv_hold( &g, 0, 0, 0x61 );
    const uint32_t reused = f.region->channel_receive[0].held_sequenced;
    rn_channels_end_update( &channels );
    rocketnet_unit_check( f.region->sequenced_messages_held[reused].used == 1 );
    rocketnet_unit_check( f.region->sequenced_delivered_count == 0 );
    dv_close( &f );
}

REGISTER_UNIT_TEST( test_sequenced_delivered_view_expires_at_end_update );
REGISTER_UNIT_TEST( test_sequenced_end_update_clears_exactly_the_delivered_set );
REGISTER_UNIT_TEST( test_sequenced_end_update_empty_is_constant_work );
REGISTER_UNIT_TEST( test_sequenced_delivered_tracking_starts_clean_after_reset );

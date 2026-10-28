/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Code 1's channel transitions (RNChannelTypes!Dispatch and !Drain).
// All inputs are in memory; no clock, socket, or transport service is used.

#include <string.h>
#include <stdlib.h>

#include "test_unit_registry.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message_object.h"

static rocketnet_counters_t sequenced_counters;

static rn_arena_t * sequenced_arena = NULL;

static rn_region_t * sequenced_region()
{
    if ( !sequenced_arena )
    {
        sequenced_arena = rn_arena_create( NULL, NULL, 1, &sequenced_counters );
    }
    return sequenced_arena ? rn_arena_region( sequenced_arena, 0 ) : NULL;
}

static void sequenced_record( rocketnet_message_record_t * record, int code, uint32_t channel,
                              uint32_t ordering, uint32_t sequencing, const uint8_t * payload )
{
    memset( record, 0, sizeof( *record ) );
    record->code = code;
    record->channel = channel;
    record->ordering = ordering;
    record->sequencing = sequencing;
    record->data = payload;
    record->bytes = 1;
}

static void test_older_dropped_newest_delivered()
{
    rn_region_t * region = sequenced_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint8_t newest = 7, older = 3;
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 1, &newest );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( out.data == &newest );
    rocketnet_unit_check( region->channel_receive[0].sequenced_watermark == 2 );

    const uint64_t stale_before = sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE];
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, &older );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_STALE );
    rocketnet_unit_check( sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE] == stale_before + 1 );
    rocketnet_unit_check( channels.deliveries == 1 );
    rn_region_reset( region );
}

static void test_watermark_resets_on_ordered_delivery()
{
    rn_region_t * region = sequenced_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint8_t before = 1, ordered = 2, after = 3;
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 4, &before );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, &ordered );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( region->channel_receive[0].sequenced_watermark == 0 );
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, &after );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( out.data == &after );
    rn_region_reset( region );
}

static void test_held_newest_ahead_of_epoch()
{
    rn_region_t * region = sequenced_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint8_t old_state = 1, new_state = 2, ordered = 3;
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, &old_state );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    const uint64_t superseded_before = sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED];
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, &new_state );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    rocketnet_unit_check( sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED] == superseded_before + 1 );
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, &ordered );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( channels.deliveries == 2 );
    rocketnet_unit_check( region->deliveries[1].data[0] == new_state );
    rocketnet_unit_check( region->channel_receive[0].sequenced_watermark == 2 );
    rn_region_reset( region );
}

static void test_reliable_sequenced_uses_shared_watermark()
{
    rn_region_t * region = sequenced_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint8_t payload = 1;
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 0, &payload );
    record.number = 0;
    region->receive_descriptors[0].number = 0;
    region->receive_descriptors[0].code = ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
    region->receive_descriptors[0].live = 1;
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( out.code == ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED && out.descriptor == 0 );
    rocketnet_unit_check( region->channel_receive[0].sequenced_watermark == 1 );
    rn_region_reset( region );
}

static void test_hold_full_drops_and_counts()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.sequenced_messages_held = 1;

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint8_t a = 1, b = 2;
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, &a );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 1, 1, 0, &b );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL] == 1 );
    rocketnet_unit_check( region->channel_receive[1].held_sequenced == RN_DESCRIPTOR_NONE );
    rn_arena_destroy( arena );
}

static void test_large_future_state_drops_without_violation()
{
    rn_region_t * region = sequenced_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1] = {};
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, payload );
    record.bytes = (int) sizeof( payload );

    const uint64_t before = sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL];
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL] == before + 1 );
    rocketnet_unit_check( channels.violations == 0 );
    rocketnet_unit_check( channels.error == ROCKETNET_DISCONNECT_REASON_NONE );

    const uint8_t small = 9;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, &small );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    const uint64_t superseded = sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED];
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 2, payload );
    record.bytes = (int) sizeof( payload );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    rocketnet_unit_check( sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL] == before + 2 );
    rocketnet_unit_check( sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED] == superseded );
    rocketnet_unit_check( region->sequenced_message_payloads[region->channel_receive[0].held_sequenced].data[0] == small );
    rn_region_reset( region );
}

static void test_never_takes_a_reliable_number()
{
    // the public C API's start/finish accepts code 1 while the per-channel
    // queue and reliable number state stay independent (RNChannelTypes!Enqueue)

    rn_message_link_t link = {};
    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1, NULL, NULL, 0,
                                                             ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
    rocketnet_unit_check( object != NULL );

    rocketnet_message_client_t * client = (rocketnet_message_client_t *) object;
    rn_message_connection_t * connection = &object->connections[0];
    const uint64_t next = connection->ordered.send.next;
    uint8_t * p = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 4 );
    rocketnet_unit_check( p != NULL );

    if ( p )
    {
        memcpy( p, "new!", 4 );
        rocketnet_unit_check( rocketnet_message_client_finish( client, 4 ) != 0 );
        rocketnet_unit_check( connection->unreliable_count == 1 );

        const uint32_t d = connection->unreliable_head;
        rocketnet_unit_check( d != RN_DESCRIPTOR_NONE );
        rocketnet_unit_check( connection->region->send_descriptors[d].reliability == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED );
        rocketnet_unit_check( connection->ordered.send.next == next );

        uint64_t number = 0, ring = 0;
        rocketnet_unit_check( rn_window_send_first_write( &connection->ordered.send, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED,
                                                          4, d, &number, &ring ) == RN_WINDOW_SEND_UNRELIABLE );
        rocketnet_unit_check( connection->ordered.send.next == next );
    }

    rn_message_object_destroy( object );
}

static int sequenced_large_start_calls;

static int sequenced_large_max_payload( void * )
{
    return ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES;
}

static uint64_t sequenced_large_connected( void *, int )
{
    return 1;
}

static uint8_t * sequenced_large_start_packet( void *, int, int, rocketnet_packet_context_t * )
{
    sequenced_large_start_calls++;
    return NULL;
}

static void sequenced_large_send_packets( void * )
{
}

static void test_large_state_promotes_to_reliable_sequenced()
{
    rn_message_link_t link = {};
    link.max_payload_bytes = sequenced_large_max_payload;
    link.connection = sequenced_large_connected;
    link.start_packet = sequenced_large_start_packet;
    link.send_packets = sequenced_large_send_packets;

    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1, NULL, NULL, 0, 1, 0.0 );
    rocketnet_unit_check( object != NULL );

    // an oversize unreliable send is promoted to its reliable twin (the
    // promotion rule), never dropped as unreliable

    uint8_t * pointer = rn_message_object_start( object, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED,
                                                 ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES );
    rocketnet_unit_check( pointer != NULL );

    if ( pointer )
    {
        memset( pointer, 0, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES );
        rocketnet_unit_check( rn_message_object_finish( object, 0, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES ) != 0 );
    }

    rn_message_connection_t * connection = &object->connections[0];
    rocketnet_unit_check( object->counters.values[ROCKETNET_COUNTER_MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED] == 1 );
    rocketnet_unit_check( connection->unreliable_count == 0 );
    rocketnet_unit_check( object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET] == 0 );
    rn_message_object_destroy( object );
}

static void test_sequenced_drain_refusal_counts_stale()
{
    // the ordered record opens the epoch; the drain then refuses the held
    // record's sequencing (upper half) and must count it, not drop it silently

    rn_region_t * region = sequenced_region();
    rocketnet_unit_check( region != NULL );

    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint8_t ahead = 1, ordered = 2;
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0x800000, &ahead );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    const uint64_t stale_before = sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE];
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, &ordered );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rocketnet_unit_check( channels.deliveries == 1 );
    rocketnet_unit_check( region->channel_receive[0].held_sequenced == RN_DESCRIPTOR_NONE );
    rocketnet_unit_check( sequenced_counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE] == stale_before + 1 );
    rn_region_reset( region );
}

static void test_rnchannel_sequenced_witness_replays()
{
    // The reversed traces in README-RNChannelTypes: reject backwards state,
    // reset the watermark on the ordered drain, and keep one future state.
    // Keep this combined run because the last case must see ordered_read reset
    // after the middle case advances it to 1; otherwise ordering 1 is delivered
    // instead of held. The cases reuse the same static region.

    test_older_dropped_newest_delivered();
    test_watermark_resets_on_ordered_delivery();
    test_held_newest_ahead_of_epoch();
}

static uint64_t sequenced_allocations;

static void * sequenced_counting_malloc( void * context, size_t bytes )
{
    (void) context;
    sequenced_allocations++;
    return malloc( bytes );
}

static void sequenced_counting_free( void * context, void * memory )
{
    (void) context;
    sequenced_allocations++;
    free( memory );
}

static void test_sequenced_no_allocation_after_create()
{
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, sequenced_counting_malloc, sequenced_counting_free ) == ROCKETNET_STATUS_OK );

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &sequenced_counters );
    rocketnet_unit_check( arena != NULL );

    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    rocketnet_unit_check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    const uint64_t after_create = sequenced_allocations;
    const uint8_t state = 5, ordered = 6;
    rocketnet_message_record_t record;
    rn_delivery_t out;
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, &state );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    sequenced_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, &ordered );
    rocketnet_unit_check( rn_channels_dispatch( &channels, &record, &out ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rn_channels_end_update( &channels );
    rocketnet_unit_check( sequenced_allocations == after_create );
    rn_arena_destroy( arena );
    rocketnet_term();
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_older_dropped_newest_delivered );
REGISTER_UNIT_TEST( test_watermark_resets_on_ordered_delivery );
REGISTER_UNIT_TEST( test_held_newest_ahead_of_epoch );
REGISTER_UNIT_TEST( test_reliable_sequenced_uses_shared_watermark );
REGISTER_UNIT_TEST( test_hold_full_drops_and_counts );
REGISTER_UNIT_TEST( test_large_future_state_drops_without_violation );
REGISTER_UNIT_TEST( test_never_takes_a_reliable_number );
REGISTER_UNIT_TEST( test_large_state_promotes_to_reliable_sequenced );
REGISTER_UNIT_TEST( test_sequenced_drain_refusal_counts_stale );
REGISTER_UNIT_TEST( test_rnchannel_sequenced_witness_replays );
REGISTER_UNIT_TEST( test_sequenced_no_allocation_after_create );

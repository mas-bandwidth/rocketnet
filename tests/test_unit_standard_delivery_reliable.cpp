/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_delivery_reliable.cpp -- the vector twins of RN-2 m7
// (standard/MESSAGES.md, Delivery: reliable codes 2, 3 and 4), regenerated
// through the implementation. Each vector is one delivery trace: a listed
// record sequence fed, one record to a payload, through the receive window
// and the channel dispatch, with an update boundary after every record. The
// bytes are the outcome log: for each input record a dispatch result, a
// violation flag and the deliveries that record produced, each with its
// application code, channel, length and bytes.

#include "test_unit_standard.h"

#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_receive.h"

struct delivery_step_t
{
    int code;
    uint32_t channel;
    uint32_t number;
    uint32_t ring;
    uint32_t ordering;
    uint32_t sequencing;
    uint8_t data;
};

// Code 2: delivered as its number is accepted, out of number order, once.
static const delivery_step_t reliable_steps[] =
{
    { 2, 0, 2, 0, 0, 0, 0x12 },
    { 2, 0, 0, 0, 0, 0, 0x10 },
    { 2, 0, 3, 0, 0, 0, 0x13 },
    { 2, 0, 1, 0, 0, 0, 0x11 },
    { 2, 0, 2, 0, 0, 0, 0x12 },
    { 2, 0, 68, 0, 0, 0, 0x44 },
};

// Code 3: ahead of its ordering index it is held; a delivery in turn drains
// the held successors; behind is a violation.
static const delivery_step_t ordered_steps[] =
{
    { 3, 0, 0, 1, 1, 0, 0x21 },
    { 3, 0, 1, 3, 3, 0, 0x23 },
    { 3, 0, 2, 0, 0, 0, 0x20 },
    { 3, 0, 3, 2, 2, 0, 0x22 },
    { 3, 0, 2, 0, 0, 0, 0x20 },
    { 3, 0, 4, 4, 0, 0, 0x24 },
};

// Code 4: delivered in its epoch while its sequencing index is not behind the
// watermark; held as the newest future state; dropped when superseded; drained
// when an ordered delivery opens its epoch.
static const delivery_step_t sequenced_steps[] =
{
    { 3, 0, 0, 0, 0, 0, 0x30 },
    { 4, 0, 1, 1, 1, 0, 0x31 },
    { 4, 0, 2, 1, 1, 1, 0x32 },
    { 4, 0, 3, 1, 1, 0, 0x33 },
    { 4, 0, 4, 2, 2, 0, 0x34 },
    { 4, 0, 5, 2, 2, 1, 0x35 },
    { 4, 0, 6, 2, 2, 0, 0x36 },
    { 3, 0, 7, 1, 1, 0, 0x37 },
    { 4, 0, 8, 2, 2, 3, 0x38 },
    { 4, 0, 9, 3, 66, 0, 0x39 },
};

static const rn_standard_vector_t standard_delivery_reliable_table[] =
{
    RN_STANDARD_VECTOR( "m7.reliable-loss-reorder",
        "01 00 01 02 00 01 00 12 01 00 01 02 00 01 00 10"
        "01 00 01 02 00 01 00 13 01 00 01 02 00 01 00 11"
        "00 00 00 00 01 00" ),
    RN_STANDARD_VECTOR( "m7.reliable-ordered-hold-drain",
        "02 00 00 02 00 00 01 00 02 03 00 01 00 20 03 00"
        "01 00 21 01 00 02 03 00 01 00 22 03 00 01 00 23"
        "00 00 00 00 01 00" ),
    RN_STANDARD_VECTOR( "m7.reliable-sequenced-hold-drain",
        "01 00 01 03 00 01 00 30 01 00 01 04 00 01 00 31"
        "01 00 01 04 00 01 00 32 03 00 00 02 00 00 02 00"
        "00 03 00 00 01 00 02 03 00 01 00 37 04 00 01 00"
        "35 01 00 01 04 00 01 00 38 00 01 00" ),
};

static bool trace_append( uint8_t * out, int capacity, int * used, const uint8_t * bytes, int count )
{
    if ( *used + count > capacity )
        return false;
    memcpy( out + *used, bytes, (size_t) count );
    *used += count;
    return true;
}

static bool trace_append_u8( uint8_t * out, int capacity, int * used, uint8_t value )
{
    return trace_append( out, capacity, used, &value, 1 );
}

static bool trace_append_u16( uint8_t * out, int capacity, int * used, uint16_t value )
{
    const uint8_t bytes[2] = { (uint8_t) ( value & 0xFF ), (uint8_t) ( value >> 8 ) };
    return trace_append( out, capacity, used, bytes, 2 );
}

static int trace_run( const delivery_step_t * steps, int count, uint8_t * out, int capacity )
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.reliable_messages_in_flight = 64;
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = 4096;
    config.send_message_buffer_bytes = 65536;
    config.sequenced_messages_held = 16;

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    if ( !arena )
        return -1;
    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_channels_t channels;
    rn_window_receive_t receive;
    if ( !rn_channels_init( &channels, region, (uint32_t) config.num_channels, 0 ) )
    {
        rn_arena_destroy( arena );
        return -1;
    }
    rn_channels_set_payload_capacity( &channels, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    rn_window_receive_init( &receive, region, 0 );

    rocketnet_message_limits_t limits;
    limits.num_channels = (int) region->num_channels;
    limits.enabled_codes = rn_channels_enabled_codes( &channels );
    limits.reliable_messages_in_flight = (int) region->window;
    limits.max_message_bytes = (int) region->max_message_bytes;
    limits.receive_message_buffer_bytes = (int) region->ring_bytes;

    int used = 0;

    for ( int i = 0; i < count; i++ )
    {
        const delivery_step_t * step = &steps[i];

        uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );

        rocketnet_message_record_t record;
        memset( &record, 0, sizeof( record ) );
        record.code = step->code;
        record.number = step->number;
        if ( step->code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED )
        {
            record.ring = step->ring;
            record.ordering = step->ordering;
            record.channel = step->channel;
        }
        else if ( step->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        {
            record.ring = step->ring;
            record.sequencing = step->sequencing;
            record.ordering = step->ordering;
            record.channel = step->channel;
        }
        record.data = &step->data;
        record.bytes = 1;

        if ( !rocketnet_message_writer_add( &writer, &record ) )
        {
            rn_arena_destroy( arena );
            return -1;
        }
        const int payload_bytes = rocketnet_message_writer_finish( &writer );
        if ( payload_bytes < 1 )
        {
            rn_arena_destroy( arena );
            return -1;
        }

        int outcome = ROCKETNET_CHANNEL_DISPATCH_NONE;
        int connection_violation = 0;
        const uint32_t deliveries_before = channels.deliveries;

        rocketnet_message_reader_t reader;
        const int reader_violation = rocketnet_message_reader_begin( &reader, payload, payload_bytes, &limits );
        if ( reader_violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        {
            rn_channels_count_violation( &channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
            connection_violation = 1;
        }
        else
        {
            rocketnet_message_record_t parsed;
            if ( rocketnet_message_reader_next( &reader, &parsed ) )
            {
                const bool numbered = parsed.code == ROCKETNET_MESSAGE_CODE_RELIABLE ||
                    parsed.code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED ||
                    parsed.code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
                bool dispatch = true;
                if ( numbered )
                {
                    uint32_t slot = RN_DESCRIPTOR_NONE;
                    const int accepted = rn_window_receive_accept( &receive, &parsed, &slot );
                    if ( accepted == RN_WINDOW_RECEIVE_DUPLICATE )
                        dispatch = false;
                    else if ( accepted == RN_WINDOW_RECEIVE_VIOLATION )
                    {
                        rn_channels_count_violation( &channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
                        connection_violation = 1;
                        dispatch = false;
                    }
                }
                if ( dispatch )
                {
                    rn_delivery_t delivery;
                    outcome = rn_channels_dispatch( &channels, &parsed, &delivery );
                    if ( channels.error != ROCKETNET_DISCONNECT_REASON_NONE )
                        connection_violation = 1;
                }
            }
        }

        const uint32_t deliveries = channels.deliveries - deliveries_before;
        if ( !trace_append_u8( out, capacity, &used, (uint8_t) outcome ) ||
             !trace_append_u8( out, capacity, &used, (uint8_t) connection_violation ) ||
             !trace_append_u8( out, capacity, &used, (uint8_t) deliveries ) )
        {
            rn_arena_destroy( arena );
            return -1;
        }

        for ( uint32_t d = deliveries_before; d < channels.deliveries; d++ )
        {
            const rn_delivery_t * delivery = &region->deliveries[d];
            if ( !trace_append_u8( out, capacity, &used, delivery->code ) ||
                 !trace_append_u8( out, capacity, &used, (uint8_t) delivery->channel ) ||
                 !trace_append_u16( out, capacity, &used, (uint16_t) delivery->bytes ) ||
                 !trace_append( out, capacity, &used, delivery->data, delivery->bytes ) )
            {
                rn_arena_destroy( arena );
                return -1;
            }
        }

        rn_channels_end_update( &channels );
    }

    rn_arena_destroy( arena );
    return used;
}

static int standard_delivery_reliable_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "m7.reliable-loss-reorder" ) == 0 )
        return trace_run( reliable_steps, (int) ( sizeof( reliable_steps ) / sizeof( reliable_steps[0] ) ), out, capacity );
    if ( strcmp( id, "m7.reliable-ordered-hold-drain" ) == 0 )
        return trace_run( ordered_steps, (int) ( sizeof( ordered_steps ) / sizeof( ordered_steps[0] ) ), out, capacity );
    if ( strcmp( id, "m7.reliable-sequenced-hold-drain" ) == 0 )
        return trace_run( sequenced_steps, (int) ( sizeof( sequenced_steps ) / sizeof( sequenced_steps[0] ) ), out, capacity );
    return -1;
}

static void test_standard_delivery_reliable_vectors()
{
    const int count = (int) ( sizeof( standard_delivery_reliable_table ) / sizeof( standard_delivery_reliable_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_delivery_reliable_table, count, standard_delivery_reliable_generate ) == count );
}

REGISTER_UNIT_TEST( test_standard_delivery_reliable_vectors );

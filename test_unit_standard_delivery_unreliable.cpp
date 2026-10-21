/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The vector twins of RN-2 m6 (standard/MESSAGES.md, Delivery: unreliable
// codes 0 and 1), regenerated through the implementation. Each vector is one
// delivery trace: the generator feeds the record sequence to the channel
// dispatch and writes the outcome byte and the deliveries of every step. No
// socket, no thread, no clock.

#include "test_unit_standard.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_counters.h"
#include "sdk/rocketnet_message.h"

struct rn_delivery_step_t
{
    int code;
    uint32_t channel;
    uint32_t ordering;
    uint32_t sequencing;
    uint32_t number;
    const uint8_t * data;
    int bytes;
};

struct rn_delivery_trace_t
{
    uint8_t * out;
    int capacity;
    int at;
};

static int trace_bytes( rn_delivery_trace_t * trace, const void * data, int bytes )
{
    if ( trace->at + bytes > trace->capacity )
        return -1;
    memcpy( trace->out + trace->at, data, (size_t) bytes );
    trace->at += bytes;
    return 0;
}

static int trace_byte( rn_delivery_trace_t * trace, uint8_t value )
{
    return trace_bytes( trace, &value, 1 );
}

// One input record through the dispatch, then the outcome and the deliveries
// it appended. The outcome byte is the section's: 0 delivered, 1 held,
// 2 stale, 3 dropped by the unreliable delivery allowance, 4 dropped by the
// sequenced hold pool, 5 violation.
static int delivery_step( rn_channels_t * channels, rocketnet_counters_t * counters,
                          const rn_delivery_step_t * step, rn_delivery_trace_t * trace )
{
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = step->code;
    record.channel = step->channel;
    record.ordering = step->ordering;
    record.sequencing = step->sequencing;
    record.number = step->number;
    record.data = step->data;
    record.bytes = step->bytes;

    const uint32_t before = channels->deliveries;
    const uint64_t unreliable_full = counters->values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL];
    const uint64_t hold_full = counters->values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL];

    rn_delivery_t out;
    const int result = rn_channels_dispatch( channels, &record, &out );

    uint8_t outcome = 0xFF;
    if ( channels->error != ROCKETNET_DISCONNECT_REASON_NONE )
        outcome = 5;
    else if ( result == ROCKETNET_CHANNEL_DISPATCH_DELIVERED )
        outcome = 0;
    else if ( result == ROCKETNET_CHANNEL_DISPATCH_HELD )
        outcome = 1;
    else if ( result == ROCKETNET_CHANNEL_DISPATCH_STALE )
        outcome = 2;
    else if ( counters->values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL] > unreliable_full )
        outcome = 3;
    else if ( counters->values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL] > hold_full )
        outcome = 4;
    rocketnet_unit_check( outcome != 0xFF );

    const uint32_t appended = channels->deliveries - before;
    rocketnet_unit_check( appended <= 255 );
    const uint8_t count = (uint8_t) appended;

    if ( trace_byte( trace, outcome ) < 0 || trace_byte( trace, count ) < 0 )
        return -1;

    for ( uint32_t i = before; i < channels->deliveries; i++ )
    {
        const rn_delivery_t * delivery = &channels->region->deliveries[i];
        rocketnet_unit_check( delivery->channel <= 255 && delivery->code <= 255 );
        rocketnet_unit_check( delivery->bytes >= 0 && delivery->bytes <= 65535 );
        const uint8_t header[4] = { (uint8_t) delivery->channel, (uint8_t) delivery->code,
                                    (uint8_t) ( delivery->bytes & 0xFF ), (uint8_t) ( ( delivery->bytes >> 8 ) & 0xFF ) };
        if ( trace_bytes( trace, header, 4 ) < 0 || trace_bytes( trace, delivery->data, delivery->bytes ) < 0 )
            return -1;
    }

    return 0;
}

static int run_trace( const rocketnet_message_config_t * config, const rn_delivery_step_t * steps, int count,
                      rn_delivery_trace_t * trace )
{
    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, config, 1, &counters );
    if ( !arena )
        return -1;
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_channels_t channels;
    int result = rn_channels_init( &channels, region, region->num_channels, 0 ) ? 0 : -1;
    for ( int i = 0; result == 0 && i < count; i++ )
        result = delivery_step( &channels, &counters, &steps[i], trace );
    rn_arena_destroy( arena );
    return result;
}

// m6.code0-delivery-trace: two code-0 records are delivered on channel 0;
// with an allowance of two, the next two are dropped and counted.
static int code0_delivery_trace( rn_delivery_trace_t * trace )
{
    static const uint8_t one[] = { 0x6f, 0x6e, 0x65 };
    static const uint8_t two[] = { 0x74, 0x77, 0x6f };
    static const uint8_t three[] = { 0x74, 0x68, 0x72, 0x65, 0x65 };
    static const uint8_t four[] = { 0x66, 0x6f, 0x75, 0x72 };
    const rn_delivery_step_t steps[] =
    {
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 0, one, (int) sizeof( one ) },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 0, two, (int) sizeof( two ) },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 0, three, (int) sizeof( three ) },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 0, four, (int) sizeof( four ) },
    };
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.unreliable_messages_per_update = 2;
    return run_trace( &config, steps, (int) ( sizeof( steps ) / sizeof( steps[0] ) ), trace );
}

// m6.code1-sequenced-trace: delivery, stale sequencing, an ahead-of-epoch hold
// that only one message may occupy, its release by an ordered delivery, and
// an ordering W ahead as a violation.
static int code1_sequenced_trace( rn_delivery_trace_t * trace )
{
    static const uint8_t a[] = { 0x61 }, b[] = { 0x62 }, c[] = { 0x63 }, d[] = { 0x64 };
    static const uint8_t e[] = { 0x65 }, f[] = { 0x66 }, o[] = { 0x6f }, g[] = { 0x67 };
    static const uint8_t h[] = { 0x68 }, i[] = { 0x69 }, p[] = { 0x70 }, j[] = { 0x6a };
    static const uint8_t k[] = { 0x6b }, x[] = { 0x78 };
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    const rn_delivery_step_t steps[] =
    {
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, 0, a, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 1, 0, b, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 0, 0, 0, c, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, 0, d, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, 0, e, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, 0, f, 1 },
        { ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, 0, o, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, 0, g, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 2, 0, h, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 2, 0, 0, i, 1 },
        { ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 1, 0, 1, p, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 2, 0, 0, j, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 2, 1, 0, k, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 2 + (uint32_t) config.reliable_messages_in_flight, 0, 0, x, 1 },
    };
    return run_trace( &config, steps, (int) ( sizeof( steps ) / sizeof( steps[0] ) ), trace );
}

// m6.code1-hold-full-trace: one hold slot, a second channel with no slot is
// dropped, and a newer state in the held channel supersedes the older one.
static int code1_hold_full_trace( rn_delivery_trace_t * trace )
{
    static const uint8_t x[] = { 0x78 }, y[] = { 0x79 }, z[] = { 0x7a }, o[] = { 0x6f };
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.sequenced_messages_held = 1;
    const rn_delivery_step_t steps[] =
    {
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 0, 0, x, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 1, 1, 0, 0, y, 1 },
        { ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 1, 1, 0, z, 1 },
        { ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, 0, o, 1 },
    };
    return run_trace( &config, steps, (int) ( sizeof( steps ) / sizeof( steps[0] ) ), trace );
}

static int standard_delivery_generate( const char * id, uint8_t * out, int capacity )
{
    rn_delivery_trace_t trace = { out, capacity, 0 };
    int result = -1;
    if ( strcmp( id, "m6.code0-delivery-trace" ) == 0 )
        result = code0_delivery_trace( &trace );
    else if ( strcmp( id, "m6.code1-sequenced-trace" ) == 0 )
        result = code1_sequenced_trace( &trace );
    else if ( strcmp( id, "m6.code1-hold-full-trace" ) == 0 )
        result = code1_hold_full_trace( &trace );
    return result == 0 ? trace.at : -1;
}

static const rn_standard_vector_t standard_delivery_table[] =
{
    RN_STANDARD_VECTOR( "m6.code0-delivery-trace", "0001000003006f6e6500010000030074776f03000300" ),
    RN_STANDARD_VECTOR( "m6.code1-sequenced-trace", "000100010100610001000101006202000100010002000002000301006f0001010065020000010001010068010000020003010070000101006902000001000101006b0500" ),
    RN_STANDARD_VECTOR( "m6.code1-hold-full-trace", "0100040001000002000301006f000101007a" ),
};

static void test_standard_delivery_unreliable_vectors()
{
    const int count = (int) ( sizeof( standard_delivery_table ) / sizeof( standard_delivery_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_delivery_table, count, standard_delivery_generate ) == count );
}

REGISTER_UNIT_TEST( test_standard_delivery_unreliable_vectors );

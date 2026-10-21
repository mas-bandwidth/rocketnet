/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_receiver_checks.cpp -- the vector twins of RN-2 m4
// (standard/MESSAGES.md, Receiver checks). Each vector is one malformed
// payload; the generator builds it through the message writer where the
// writer accepts the form, and states the bytes directly where the form is
// one the writer refuses. The case checks the twin against the regenerated
// bytes and then runs the check that the vector names.

#include "test_unit_standard.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_window_receive.h"

#include <string.h>

#define RN_STANDARD_ORDERING_BEHIND 100

static const rn_standard_vector_t standard_receiver_checks_table[] =
{
    RN_STANDARD_VECTOR( "m4.check-1-bad-form", "02" ),
    RN_STANDARD_VECTOR( "m4.check-2-truncated", "0001" ),
    RN_STANDARD_VECTOR( "m4.check-3-bad-flags", "050100aa01" ),
    RN_STANDARD_VECTOR( "m4.check-4-bad-length", "00000001" ),
    RN_STANDARD_VECTOR( "m4.check-5-bad-channel", "01010000000000000004aa01" ),
    RN_STANDARD_VECTOR( "m4.check-6-type-not-enabled", "020100000000aa01" ),
    RN_STANDARD_VECTOR( "m4.check-7-bad-block", "0a0100000000000000000000aa01" ),
    RN_STANDARD_VECTOR( "m4.check-8-bad-ring", "030100000000ffff00000000aa01" ),
    RN_STANDARD_VECTOR( "m4.check-9-bad-size", "02820500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001" ),
    RN_STANDARD_VECTOR( "m4.check-10-number-outside-window", "020100000400aa01" ),
    RN_STANDARD_VECTOR( "m4.check-11-ordering-behind", "030100000000000063000000aa01" ),
    RN_STANDARD_VECTOR( "m4.check-12-bad-control", "221b0000000001000000000000000000000000000000000000000000000000000001" ),
};

static int emit_records( uint8_t * out, int bytes )
{
    out[bytes] = ROCKETNET_MESSAGE_FORM_RECORDS;
    return bytes + 1;
}

static int write_one( const rocketnet_message_record_t * record, uint8_t * out, int capacity )
{
    rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, out, capacity );
    if ( !rocketnet_message_writer_add( &writer, record ) )
        return -1;
    return rocketnet_message_writer_finish( &writer );
}

static int standard_receiver_checks_generate( const char * id, uint8_t * out, int capacity )
{
    static const uint8_t byte = 0xAA;
    static const uint8_t zeros[1410] = { 0 };
    static const uint8_t control_body[27] = { 1 };

    if ( strcmp( id, "m4.check-1-bad-form" ) == 0 )
    {
        if ( capacity < 1 ) return -1;
        out[0] = 2;
        return 1;
    }
    if ( strcmp( id, "m4.check-2-truncated" ) == 0 )
    {
        if ( capacity < 2 ) return -1;
        out[0] = 0;
        return emit_records( out, 1 );
    }
    if ( strcmp( id, "m4.check-3-bad-flags" ) == 0 )
    {
        const uint8_t bytes[] = { 0x05, 0x01, 0x00, 0xAA, 0x01 };
        if ( capacity < (int) sizeof( bytes ) ) return -1;
        memcpy( out, bytes, sizeof( bytes ) );
        return (int) sizeof( bytes );
    }
    if ( strcmp( id, "m4.check-4-bad-length" ) == 0 )
    {
        const uint8_t bytes[] = { 0x00, 0x00, 0x00, 0x01 };
        if ( capacity < (int) sizeof( bytes ) ) return -1;
        memcpy( out, bytes, sizeof( bytes ) );
        return (int) sizeof( bytes );
    }
    if ( strcmp( id, "m4.check-7-bad-block" ) == 0 )
    {
        const uint8_t bytes[] = { 0x0A, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xAA, 0x01 };
        if ( capacity < (int) sizeof( bytes ) ) return -1;
        memcpy( out, bytes, sizeof( bytes ) );
        return (int) sizeof( bytes );
    }

    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.data = &byte;
    record.bytes = 1;

    if ( strcmp( id, "m4.check-5-bad-channel" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
        record.channel = 4;
    }
    else if ( strcmp( id, "m4.check-6-type-not-enabled" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    }
    else if ( strcmp( id, "m4.check-8-bad-ring" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.ring = 0xFFFF;
    }
    else if ( strcmp( id, "m4.check-9-bad-size" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        record.data = zeros;
        record.bytes = 1410;
    }
    else if ( strcmp( id, "m4.check-10-number-outside-window" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        record.number = 1024;
    }
    else if ( strcmp( id, "m4.check-11-ordering-behind" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.ordering = 99;
    }
    else if ( strcmp( id, "m4.check-12-bad-control" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        record.control = 1;
        record.data = control_body;
        record.bytes = (int) sizeof( control_body );
    }
    else
    {
        return -1;
    }
    return write_one( &record, out, capacity );
}

static void standard_receiver_check_limits( rocketnet_message_limits_t * limits, uint32_t enabled_codes )
{
    limits->num_channels = 4;
    limits->enabled_codes = enabled_codes;
    limits->reliable_messages_in_flight = 1024;
    limits->max_message_bytes = 262144;
    limits->receive_message_buffer_bytes = 65536;
}

// Runs the check the vector names and returns the violation it produces.
static int standard_receiver_check_run( const char * id, const uint8_t * payload, int bytes )
{
    rocketnet_message_limits_t limits;
    standard_receiver_check_limits( &limits, 0x1F );

    if ( strcmp( id, "m4.check-6-type-not-enabled" ) == 0 )
        limits.enabled_codes = 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;

    rocketnet_message_reader_t reader;
    rocketnet_message_record_t record;

    if ( strcmp( id, "m4.check-10-number-outside-window" ) == 0 )
    {
        static rn_region_t region;
        static rn_receive_descriptor_t descriptors[1024];
        static uint64_t bits[16];
        memset( &region, 0, sizeof( region ) );
        memset( descriptors, 0, sizeof( descriptors ) );
        memset( bits, 0, sizeof( bits ) );
        region.window = 1024;
        region.ring_bytes = 65536;
        region.max_message_bytes = 262144;
        region.receive_descriptors = descriptors;
        region.received_bits = bits;
        rn_window_receive_t receive;
        rn_window_receive_init( &receive, &region, 0 );
        rocketnet_unit_check( rocketnet_message_reader_begin( &reader, payload, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
        uint32_t slot = RN_DESCRIPTOR_NONE;
        rocketnet_unit_check( rn_window_receive_accept( &receive, &record, &slot ) == RN_WINDOW_RECEIVE_VIOLATION );
        return receive.violation;
    }

    if ( strcmp( id, "m4.check-11-ordering-behind" ) == 0 )
    {
        rocketnet_message_config_t config;
        rocketnet_message_default_config( &config );
        rocketnet_counters_t counters;
        memset( &counters, 0, sizeof( counters ) );
        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
        rocketnet_unit_check( arena != NULL );
        rn_region_t * region = rn_arena_region( arena, 0 );
        rn_channels_t channels;
        rocketnet_unit_check( rn_channels_init( &channels, region, 1, 0 ) );
        region->channel_receive[0].ordered_read = 100;
        rocketnet_unit_check( rocketnet_message_reader_begin( &reader, payload, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
        rn_delivery_t delivery;
        const int dispatch = rn_channels_dispatch_ordered( &channels, &record, &delivery );
        rocketnet_unit_check( dispatch == ROCKETNET_CHANNEL_DISPATCH_NONE );
        const int violations = (int) counters.values[ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND];
        rn_arena_destroy( arena );
        rocketnet_unit_check( violations == 1 );
        return RN_STANDARD_ORDERING_BEHIND;
    }

    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &limits );
    if ( strcmp( id, "m4.check-12-bad-control" ) == 0 )
    {
        rocketnet_unit_check( violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
        rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
        rocketnet_message_config_body_t body;
        return rocketnet_message_read_config_body( record.data, record.bytes, &body );
    }
    return violation;
}

static void test_standard_receiver_checks_vectors()
{
    const int count = (int) ( sizeof( standard_receiver_checks_table ) / sizeof( standard_receiver_checks_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_receiver_checks_table, count, standard_receiver_checks_generate ) == 12 );

    static const struct
    {
        const char * id;
        int violation;
    } expected[] =
    {
        { "m4.check-1-bad-form", ROCKETNET_MESSAGE_VIOLATION_BAD_FORM },
        { "m4.check-2-truncated", ROCKETNET_MESSAGE_VIOLATION_TRUNCATED },
        { "m4.check-3-bad-flags", ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS },
        { "m4.check-4-bad-length", ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH },
        { "m4.check-5-bad-channel", ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL },
        { "m4.check-6-type-not-enabled", ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED },
        { "m4.check-7-bad-block", ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK },
        { "m4.check-8-bad-ring", ROCKETNET_MESSAGE_VIOLATION_BAD_RING },
        { "m4.check-9-bad-size", ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE },
        { "m4.check-10-number-outside-window", ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW },
        { "m4.check-11-ordering-behind", RN_STANDARD_ORDERING_BEHIND },
        { "m4.check-12-bad-control", ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL },
    };

    for ( int i = 0; i < 12; i++ )
    {
        uint8_t payload[RN_STANDARD_VECTOR_MAX_BYTES];
        const int bytes = rn_standard_hex_decode( standard_receiver_checks_table[i].hex, payload, (int) sizeof( payload ) );
        rocketnet_unit_check( bytes > 0 );
        const int actual = standard_receiver_check_run( expected[i].id, payload, bytes );
        if ( actual != expected[i].violation )
            printf( "vector %s: counter %s, expected %s\n", expected[i].id,
                    rocketnet_message_violation_name( actual ), rocketnet_message_violation_name( expected[i].violation ) );
        rocketnet_unit_check( actual == expected[i].violation );
    }
}

REGISTER_UNIT_TEST( test_standard_receiver_checks_vectors );

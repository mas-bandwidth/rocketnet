/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message record rules, writer and reader at the refusals no other case
// reached (mutation gaps, round 2, r2-tables): flags the standard does not
// allow, a write of an invalid record and a payload changed under a reader
// with asserts quieted (the paths a build without asserts takes), and an
// in-place add whose data is not where its header ends. Pure: no sockets, no
// time, no allocation. Each failing check prints the fault first.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_message.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static void gaps_r2_tables_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}

static const uint8_t gaps_r2_tables_data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

static void gaps_r2_tables_record( struct rocketnet_message_record_t * record, int code )
{
    memset( record, 0, sizeof( *record ) );
    record->code = code;
    record->data = gaps_r2_tables_data;
    record->bytes = 4;
}

// mutation gap: hunt 2/2906
// mutation gap: hunt 2/2907
//
// Flags the standard does not allow make a record invalid even when every
// field is inside its width: a block on code 0 (only reliable messages are
// blocks) and CONTROL on code 3 (CONTROL is code 2). The same shapes on code 2
// are valid.

static void test_gaps_r2_tables_message_record_valid_refuses_disallowed_flags()
{
    struct rocketnet_message_record_t record;

    gaps_r2_tables_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
    record.block = 1;
    record.fragments = 1;
    const int unreliable_block = rocketnet_message_record_valid( &record );
    if ( unreliable_block != 0 )
    {
        printf( "fault: a block on code 0 (unreliable) was reported a valid record: its flags were not checked\n" );
    }
    check( unreliable_block == 0 );

    gaps_r2_tables_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
    record.control = 1;
    const int ordered_control = rocketnet_message_record_valid( &record );
    if ( ordered_control != 0 )
    {
        printf( "fault: CONTROL on code 3 was reported a valid record: its flags were not checked\n" );
    }
    check( ordered_control == 0 );

    gaps_r2_tables_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE );
    record.block = 1;
    record.fragments = 1;
    check( rocketnet_message_record_valid( &record ) == 1 );
    gaps_r2_tables_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE );
    record.control = 1;
    check( rocketnet_message_record_valid( &record ) == 1 );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_message_record_valid_refuses_disallowed_flags );

// mutation gap: hunt 2/3014
// mutation gap: hunt 2/3015
//
// Writing an invalid record is a programmer error (an assert), and with the
// assert quieted, as a build without asserts runs, the writer still refuses
// it: 0, nothing written.

static void test_gaps_r2_tables_message_writer_refuses_an_invalid_record()
{
    uint8_t payload[64];
    memset( payload, 0xA5, sizeof( payload ) );
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );

    struct rocketnet_message_record_t record;
    gaps_r2_tables_record( &record, 9 );

    rocketnet_assert_function( gaps_r2_tables_quiet_assert );
    const int added = rocketnet_message_writer_add( &writer, &record );
    rocketnet_assert_function( NULL );

    if ( added != 0 || writer.bytes != 0 || writer.records != 0 )
    {
        printf( "fault: the message writer took a record of code 9 (returned %d, %d bytes written)\n", added, writer.bytes );
    }
    check( added == 0 );
    check( writer.bytes == 0 && writer.records == 0 );
    check( payload[0] == 0xA5 );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_message_writer_refuses_an_invalid_record );

// mutation gap: hunt 2/3023
// mutation gap: hunt 2/3024
//
// An in-place add takes a record only when its data starts where its header
// ends; data anywhere else is refused, with nothing written.

static void test_gaps_r2_tables_message_add_in_place_refuses_data_elsewhere()
{
    uint8_t payload[64];
    memset( payload, 0xA5, sizeof( payload ) );
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );

    struct rocketnet_message_record_t record;
    gaps_r2_tables_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
    const int added = rocketnet_message_writer_add_in_place( &writer, &record );
    if ( added != 0 || writer.bytes != 0 || writer.records != 0 )
    {
        printf( "fault: an in-place add took a record whose data is not after its header (returned %d, %d bytes written)\n", added, writer.bytes );
    }
    check( added == 0 );
    check( writer.bytes == 0 && writer.records == 0 );
    check( payload[0] == 0xA5 );

    // the same record with its data in place is taken
    const int header = rocketnet_message_record_header_bytes( &record );
    memcpy( payload + header, gaps_r2_tables_data, 4 );
    record.data = payload + header;
    check( rocketnet_message_writer_add_in_place( &writer, &record ) == 1 );
    check( writer.bytes == header + 4 && writer.records == 1 );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_message_add_in_place_refuses_data_elsewhere );

// mutation gap: hunt 2/3286
// mutation gap: hunt 2/3287
//
// The reader checks every record at begin. A payload changed under it
// afterwards (a programmer error, an assert) is still refused record by
// record with the assert quieted: the record that no longer decodes is not
// handed out, and the reader keeps its violation so nothing after it is.

static void test_gaps_r2_tables_message_reader_refuses_a_record_changed_after_begin()
{
    uint8_t payload[64];
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );
    struct rocketnet_message_record_t record;
    gaps_r2_tables_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE );
    check( rocketnet_message_writer_add( &writer, &record ) == 1 );
    const int second_at = writer.bytes;
    check( rocketnet_message_writer_add( &writer, &record ) == 1 );
    const int bytes = rocketnet_message_writer_finish( &writer );
    check( bytes > 0 );

    struct rocketnet_message_limits_t limits;
    memset( &limits, 0, sizeof( limits ) );
    limits.num_channels = 4;
    limits.enabled_codes = 0x1F;
    limits.reliable_messages_in_flight = 8;
    limits.max_message_bytes = 1024;
    limits.receive_message_buffer_bytes = 1024;

    struct rocketnet_message_reader_t reader;
    check( rocketnet_message_reader_begin( &reader, payload, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    payload[second_at] |= 0x40;                                    // a reserved flag bit

    rocketnet_assert_function( gaps_r2_tables_quiet_assert );
    struct rocketnet_message_record_t read;
    const int first = rocketnet_message_reader_next( &reader, &read );
    const int second = rocketnet_message_reader_next( &reader, &read );
    const int violation = reader.violation;
    rocketnet_assert_function( NULL );

    check( first == 1 );
    if ( second != 0 )
    {
        printf( "fault: the reader handed out a record that failed decoding (its flags changed after begin)\n" );
    }
    check( second == 0 );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS )
    {
        printf( "fault: the reader met a record that failed decoding and kept violation %d, not BAD_FLAGS: the next call decodes it again\n",
                violation );
    }
    check( violation == ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_message_reader_refuses_a_record_changed_after_begin );

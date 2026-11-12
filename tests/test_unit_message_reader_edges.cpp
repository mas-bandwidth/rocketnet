/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The receiver's limits at their edges (standard/MESSAGES.md m4 checks 7 and
// 9, m2, m3): a value at a limit is taken and one past it is refused, so a
// limit can neither tighten (refusing what a correct sender may send) nor
// loosen (taking what it may not). Also the writer's room at its smallest
// capacities. Pure functions over buffers: no time, no socket.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_message.h"

#include <string.h>

#define check rocketnet_unit_check

static void reader_edges_limits( struct rocketnet_message_limits_t * limits, int in_flight, int max_message_bytes, int buffer_bytes )
{
    memset( limits, 0, sizeof( *limits ) );
    limits->num_channels = 4;
    limits->enabled_codes = 0x1F;
    limits->reliable_messages_in_flight = in_flight;
    limits->max_message_bytes = max_message_bytes;
    limits->receive_message_buffer_bytes = buffer_bytes;
}

// one record in a RECORDS payload, through the writer; then the reader's verdict
static int reader_edges_verdict( const struct rocketnet_message_record_t * record, const struct rocketnet_message_limits_t * limits )
{
    uint8_t payload[2048];
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );
    check( rocketnet_message_writer_add( &writer, record ) );
    const int bytes = rocketnet_message_writer_finish( &writer );
    check( bytes > 0 );

    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, limits );
    check( rocketnet_message_check_payload( payload, bytes, limits ) == violation );
    if ( violation == ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        struct rocketnet_message_record_t read;
        check( rocketnet_message_reader_next( &reader, &read ) == 1 );
        check( read.bytes == record->bytes );
        check( read.fragments == record->fragments );
        check( rocketnet_message_reader_next( &reader, &read ) == 0 );
    }
    return violation;
}

// the first fragment of a reliable block of k fragments, each F bytes
static void reader_edges_block( struct rocketnet_message_record_t * record, const uint8_t * data, int k )
{
    memset( record, 0, sizeof( *record ) );
    record->code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record->block = 1;
    record->fragments = (uint32_t) ( k - 1 );
    record->fragment = 0;
    record->data = data;
    record->bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
}

static void test_message_reader_block_count_at_the_window()
{
    const int F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    memset( data, 0x5A, sizeof( data ) );
    struct rocketnet_message_limits_t limits;
    struct rocketnet_message_record_t record;

    // W = 8 and a maximum far above: a block of exactly W fragments is taken, W + 1 refused

    reader_edges_limits( &limits, 8, 64 * F, 64 * F );
    reader_edges_block( &record, data, 8 );
    check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    reader_edges_block( &record, data, 9 );
    check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
    reader_edges_block( &record, data, 2 );
    check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    // the maximum: k fragments carry at least ( k - 1 ) * F + 1 bytes, so a
    // maximum of exactly that takes a block of 4, and one byte less refuses it

    reader_edges_limits( &limits, 8, 3 * F + 1, 64 * F );
    reader_edges_block( &record, data, 4 );
    check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    reader_edges_limits( &limits, 8, 3 * F, 64 * F );
    check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
}

REGISTER_UNIT_TEST( test_message_reader_block_count_at_the_window );

static void test_message_reader_length_at_the_maximum()
{
    uint8_t data[256];
    memset( data, 0xA5, sizeof( data ) );
    struct rocketnet_message_limits_t limits;
    reader_edges_limits( &limits, 8, 100, 4096 );

    const int codes[3] = { ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_MESSAGE_CODE_RELIABLE, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED };
    for ( int i = 0; i < 3; i++ )
    {
        struct rocketnet_message_record_t record;
        memset( &record, 0, sizeof( record ) );
        record.code = codes[i];
        record.data = data;

        record.bytes = 100;
        check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        record.bytes = 101;
        check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE );
        record.bytes = 1;
        check( reader_edges_verdict( &record, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    }
}

REGISTER_UNIT_TEST( test_message_reader_length_at_the_maximum );

// SINGLE: the message, then the form byte. One byte is the smallest message,
// and the capacity is the payload's less the form byte (none below 2 bytes).

static void test_message_single_and_writer_room_at_small_capacities()
{
    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    memset( payload, 0x11, sizeof( payload ) );

    check( rocketnet_message_single_capacity( 0 ) == 0 );
    check( rocketnet_message_single_capacity( 1 ) == 0 );
    check( rocketnet_message_single_capacity( 2 ) == 1 );
    check( rocketnet_message_single_capacity( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 );

    check( rocketnet_message_single_finish( payload, 2, 1 ) == 2 );
    check( payload[1] == ROCKETNET_MESSAGE_FORM_SINGLE );
    check( rocketnet_message_single_finish( payload, 2, 2 ) == 0 );
    check( rocketnet_message_single_finish( payload, 2, 0 ) == 0 );
    check( rocketnet_message_single_finish( payload, 1, 1 ) == 0 );
    check( rocketnet_message_single_finish( payload, 0, 1 ) == 0 );

    // the 1-byte SINGLE payload reads back as one message of 1 byte

    struct rocketnet_message_limits_t limits;
    reader_edges_limits( &limits, 8, 1024, 4096 );
    struct rocketnet_message_reader_t reader;
    check( rocketnet_message_reader_begin( &reader, payload, 2, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    struct rocketnet_message_record_t read;
    check( rocketnet_message_reader_next( &reader, &read ) == 1 );
    check( read.bytes == 1 && read.data == payload );
    check( rocketnet_message_reader_next( &reader, &read ) == 0 );

    // the writer's room: the capacity less the form byte, never below 0

    const int capacities[4] = { 0, 1, 2, 3 };
    const int rooms[4] = { 0, 0, 1, 2 };
    for ( int i = 0; i < 4; i++ )
    {
        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payload, capacities[i] );
        check( rocketnet_message_writer_space( &writer ) == rooms[i] );
    }
}

REGISTER_UNIT_TEST( test_message_single_and_writer_room_at_small_capacities );

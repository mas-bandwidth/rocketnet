/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The test server's raw echo (tests/echo_payload.h): unreliable records,
    control records and raw transport bytes are echoed; a numbered
    application message (a reliable code, or the reliable twin an unreliable
    message over 1,411 bytes is promoted to, whole or as a block fragment) is
    refused, so the echo never hands a client its own send numbers.
*/

#include "test_unit_registry.h"

#include "echo_payload.h"

#include <stdint.h>
#include <string.h>

static int echo_payload_write( uint8_t * payload, int capacity, const struct rocketnet_message_record_t * records, int count )
{
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, capacity );
    for ( int i = 0; i < count; i++ )
    {
        if ( !rocketnet_message_writer_add( &writer, &records[i] ) )
        {
            return 0;
        }
    }
    return rocketnet_message_writer_finish( &writer );
}

static void test_echo_payload_refuses_numbered_messages( void )
{
    static uint8_t body[ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES];
    memset( body, 0x5A, sizeof( body ) );
    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    // the largest unreliable message, one record: echoed
    struct rocketnet_message_record_t unreliable;
    memset( &unreliable, 0, sizeof( unreliable ) );
    unreliable.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
    unreliable.data = body;
    unreliable.bytes = ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES;
    int bytes = echo_payload_write( payload, (int) sizeof( payload ), &unreliable, 1 );
    rocketnet_unit_check( bytes > 0 );
    rocketnet_unit_check( rocketnet_echo_payload_refusal( payload, bytes ) == NULL );

    // an unreliable sequenced record and a control record beside a small
    // unreliable one: echoed
    uint8_t config_body[ROCKETNET_MESSAGE_CONFIG_BYTES];
    rocketnet_message_config_body_t config = { ROCKETNET_MESSAGE_CONFIG_VERSION, 0xffu, 32u, 1024u, 1048576u, 1048576u, 1152u };
    rocketnet_message_write_config_body( config_body, &config );
    struct rocketnet_message_record_t mixed[3];
    memset( mixed, 0, sizeof( mixed ) );
    mixed[0].code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    mixed[0].control = 1;
    mixed[0].data = config_body;
    mixed[0].bytes = (int) sizeof( config_body );
    mixed[1].code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
    mixed[1].channel = 3;
    mixed[1].sequencing = 7;
    mixed[1].ordering = 1;
    mixed[1].data = body;
    mixed[1].bytes = 100;
    mixed[2].code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
    mixed[2].data = body;
    mixed[2].bytes = 100;
    bytes = echo_payload_write( payload, (int) sizeof( payload ), mixed, 3 );
    rocketnet_unit_check( bytes > 0 );
    rocketnet_unit_check( rocketnet_echo_payload_refusal( payload, bytes ) == NULL );

    // a reliable application record (what a promoted message travels as): refused
    struct rocketnet_message_record_t reliable;
    memset( &reliable, 0, sizeof( reliable ) );
    reliable.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    reliable.number = 5;
    reliable.data = body;
    reliable.bytes = 200;
    bytes = echo_payload_write( payload, (int) sizeof( payload ), &reliable, 1 );
    rocketnet_unit_check( bytes > 0 );
    rocketnet_unit_check( rocketnet_echo_payload_refusal( payload, bytes ) != NULL );

    // the same beside an unreliable record: the whole payload is refused
    struct rocketnet_message_record_t both[2] = { unreliable, reliable };
    both[0].bytes = 100;
    bytes = echo_payload_write( payload, (int) sizeof( payload ), both, 2 );
    rocketnet_unit_check( bytes > 0 );
    rocketnet_unit_check( rocketnet_echo_payload_refusal( payload, bytes ) != NULL );

    // a reliable ordered record on a channel: refused
    struct rocketnet_message_record_t ordered;
    memset( &ordered, 0, sizeof( ordered ) );
    ordered.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    ordered.number = 9;
    ordered.channel = 2;
    ordered.ordering = 1;
    ordered.data = body;
    ordered.bytes = 64;
    bytes = echo_payload_write( payload, (int) sizeof( payload ), &ordered, 1 );
    rocketnet_unit_check( bytes > 0 );
    rocketnet_unit_check( rocketnet_echo_payload_refusal( payload, bytes ) != NULL );

    // a fragment of a numbered block (a message spread over packets): refused
    struct rocketnet_message_record_t fragment;
    memset( &fragment, 0, sizeof( fragment ) );
    fragment.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    fragment.block = 1;
    fragment.number = 11;
    fragment.fragment = 0;
    fragment.fragments = 1;
    fragment.data = body;
    fragment.bytes = 1000;
    bytes = echo_payload_write( payload, (int) sizeof( payload ), &fragment, 1 );
    rocketnet_unit_check( bytes > 0 );
    rocketnet_unit_check( rocketnet_echo_payload_refusal( payload, bytes ) != NULL );

    // raw bytes that are not message records: echoed as they came
    uint8_t raw[64];
    memset( raw, 0xEE, sizeof( raw ) );
    rocketnet_unit_check( rocketnet_echo_payload_refusal( raw, (int) sizeof( raw ) ) == NULL );
}
REGISTER_UNIT_TEST( test_echo_payload_refuses_numbered_messages );

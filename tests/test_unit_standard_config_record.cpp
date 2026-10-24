/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_config_record.cpp -- the vector twins of RN-2 m11
// (standard/MESSAGES.md, CONFIG control record), regenerated through the
// implementation. Each vector is the CONFIG record's payload: code 2 with
// CONTROL set, the first reliable number, the 28-byte body and the RECORDS
// form byte. The defaults come from the library's default message config;
// each mismatch vector changes exactly one compared field.

#include "test_unit_standard.h"

#include <string.h>

#include "rocketnet.h"
#include "rocketnet_channel.h"
#include "rocketnet_message.h"

static const rn_standard_vector_t standard_config_record_table[] =
{
    RN_STANDARD_VECTOR( "m11.config-defaults", "221c000000000101ff0020000000000400000000040000000400800400000000000001" ),
    RN_STANDARD_VECTOR( "m11.version-mismatch", "221c000000000102ff0020000000000400000000040000000400800400000000000001" ),
    RN_STANDARD_VECTOR( "m11.enabled-mismatch", "221c000000000101010020000000000400000000040000000400800400000000000001" ),
    RN_STANDARD_VECTOR( "m11.num-channels-mismatch", "221c000000000101ff0010000000000400000000040000000400800400000000000001" ),
    RN_STANDARD_VECTOR( "m11.window-messages-mismatch", "221c000000000101ff0020000000000200000000040000000400800400000000000001" ),
    RN_STANDARD_VECTOR( "m11.receive-ring-bytes-mismatch", "221c000000000101ff0020000000000400000000020000000400800400000000000001" ),
    RN_STANDARD_VECTOR( "m11.max-message-bytes-mismatch", "221c000000000101ff0020000000000400000000040000000200800400000000000001" ),
    RN_STANDARD_VECTOR( "m11.fragment-bytes-mismatch", "221c000000000101ff0020000000000400000000040000000400000400000000000001" ),
};

static void standard_config_record_default_body( struct rocketnet_message_config_body_t * body )
{
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    memset( body, 0, sizeof( *body ) );
    body->version = ROCKETNET_MESSAGE_CONFIG_VERSION;
    body->enabled = ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION & 0xFFu;
    body->num_channels = (uint32_t) config.num_channels;
    body->reliable_messages_in_flight = (uint32_t) config.reliable_messages_in_flight;
    body->receive_message_buffer_bytes = (uint32_t) config.receive_message_buffer_bytes;
    body->max_message_bytes = (uint32_t) config.max_message_bytes;
    body->fragment_bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
}

static int standard_config_record_payload( const struct rocketnet_message_config_body_t * body, uint8_t * out, int capacity )
{
    uint8_t data[ROCKETNET_MESSAGE_CONFIG_BYTES];
    rocketnet_message_write_config_body( data, body );

    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.control = 1;
    record.number = 0;
    record.bytes = ROCKETNET_MESSAGE_CONFIG_BYTES;
    record.data = data;

    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, out, capacity );
    if ( !rocketnet_message_writer_add( &writer, &record ) )
        return -1;
    return rocketnet_message_writer_finish( &writer );
}

static int standard_config_record_generate( const char * id, uint8_t * out, int capacity )
{
    struct rocketnet_message_config_body_t body;
    standard_config_record_default_body( &body );

    if ( strcmp( id, "m11.config-defaults" ) == 0 )
    {
    }
    else if ( strcmp( id, "m11.version-mismatch" ) == 0 )
        body.version = 2;
    else if ( strcmp( id, "m11.enabled-mismatch" ) == 0 )
        body.enabled = 1;
    else if ( strcmp( id, "m11.num-channels-mismatch" ) == 0 )
        body.num_channels = 16;
    else if ( strcmp( id, "m11.window-messages-mismatch" ) == 0 )
        body.reliable_messages_in_flight = 512;
    else if ( strcmp( id, "m11.receive-ring-bytes-mismatch" ) == 0 )
        body.receive_message_buffer_bytes = 131072;
    else if ( strcmp( id, "m11.max-message-bytes-mismatch" ) == 0 )
        body.max_message_bytes = 131072;
    else if ( strcmp( id, "m11.fragment-bytes-mismatch" ) == 0 )
        body.fragment_bytes = 1024;
    else
        return -1;

    return standard_config_record_payload( &body, out, capacity );
}

static int standard_config_record_body_equal( const struct rocketnet_message_config_body_t * a, const struct rocketnet_message_config_body_t * b )
{
    return a->version == b->version &&
        a->enabled == b->enabled &&
        a->num_channels == b->num_channels &&
        a->reliable_messages_in_flight == b->reliable_messages_in_flight &&
        a->receive_message_buffer_bytes == b->receive_message_buffer_bytes &&
        a->max_message_bytes == b->max_message_bytes &&
        a->fragment_bytes == b->fragment_bytes;
}

static void test_standard_config_record_vectors()
{
    const int count = (int) ( sizeof( standard_config_record_table ) / sizeof( standard_config_record_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_config_record_table, count, standard_config_record_generate ) == count );

    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    const struct rocketnet_message_limits_t limits =
    {
        config.num_channels,
        rocketnet_message_enabled_codes( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION ),
        config.reliable_messages_in_flight,
        config.max_message_bytes,
        config.receive_message_buffer_bytes
    };
    for ( int i = 0; i < count; ++i )
    {
        uint8_t payload[ROCKETNET_MESSAGE_CONFIG_BYTES + 7];
        const int payload_bytes = standard_config_record_generate( standard_config_record_table[i].id, payload, (int) sizeof( payload ) );
        rocketnet_unit_check( payload_bytes == ROCKETNET_MESSAGE_CONFIG_BYTES + 7 );
        if ( payload_bytes != ROCKETNET_MESSAGE_CONFIG_BYTES + 7 )
            continue;

        struct rocketnet_message_reader_t reader;
        const int reader_violation = rocketnet_message_reader_begin( &reader, payload, payload_bytes, &limits );
        rocketnet_unit_check( reader_violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
        if ( reader_violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
            continue;
        struct rocketnet_message_record_t record;
        const int has_record = rocketnet_message_reader_next( &reader, &record );
        rocketnet_unit_check( has_record );
        if ( !has_record )
            continue;
        rocketnet_unit_check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE && record.control && record.number == 0 );
        rocketnet_unit_check( record.bytes == ROCKETNET_MESSAGE_CONFIG_BYTES );
        struct rocketnet_message_config_body_t decoded;
        const int body_violation = rocketnet_message_read_config_body( record.data, record.bytes, &decoded );
        rocketnet_unit_check( body_violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
        if ( body_violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
            continue;
        struct rocketnet_message_config_body_t expected;
        standard_config_record_default_body( &expected );
        if ( strcmp( standard_config_record_table[i].id, "m11.version-mismatch" ) == 0 )
            expected.version = 2;
        else if ( strcmp( standard_config_record_table[i].id, "m11.enabled-mismatch" ) == 0 )
            expected.enabled = 1;
        else if ( strcmp( standard_config_record_table[i].id, "m11.num-channels-mismatch" ) == 0 )
            expected.num_channels = 16;
        else if ( strcmp( standard_config_record_table[i].id, "m11.window-messages-mismatch" ) == 0 )
            expected.reliable_messages_in_flight = 512;
        else if ( strcmp( standard_config_record_table[i].id, "m11.receive-ring-bytes-mismatch" ) == 0 )
            expected.receive_message_buffer_bytes = 131072;
        else if ( strcmp( standard_config_record_table[i].id, "m11.max-message-bytes-mismatch" ) == 0 )
            expected.max_message_bytes = 131072;
        else if ( strcmp( standard_config_record_table[i].id, "m11.fragment-bytes-mismatch" ) == 0 )
            expected.fragment_bytes = 1024;
        rocketnet_unit_check( standard_config_record_body_equal( &decoded, &expected ) );
        rocketnet_unit_check( !rocketnet_message_reader_next( &reader, &record ) );
    }

    // a vector the generator does not know is refused
    uint8_t scratch[ROCKETNET_MESSAGE_CONFIG_BYTES + 7];
    rocketnet_unit_check( standard_config_record_generate( "m11.no-such-vector", scratch, (int) sizeof( scratch ) ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_config_record_vectors );

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
    body->window_messages = (uint32_t) config.window_messages;
    body->receive_ring_bytes = (uint32_t) config.receive_ring_bytes;
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
        body.window_messages = 512;
    else if ( strcmp( id, "m11.receive-ring-bytes-mismatch" ) == 0 )
        body.receive_ring_bytes = 131072;
    else if ( strcmp( id, "m11.max-message-bytes-mismatch" ) == 0 )
        body.max_message_bytes = 131072;
    else if ( strcmp( id, "m11.fragment-bytes-mismatch" ) == 0 )
        body.fragment_bytes = 1024;
    else
        return -1;

    return standard_config_record_payload( &body, out, capacity );
}

static void standard_config_record_vectors()
{
    const int count = (int) ( sizeof( standard_config_record_table ) / sizeof( standard_config_record_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_config_record_table, count, standard_config_record_generate ) == count );

    // a vector the generator does not know is refused
    uint8_t scratch[ROCKETNET_MESSAGE_CONFIG_BYTES];
    rocketnet_unit_check( standard_config_record_generate( "m11.no-such-vector", scratch, (int) sizeof( scratch ) ) == -1 );
}

REGISTER_UNIT_TEST( standard_config_record_vectors );

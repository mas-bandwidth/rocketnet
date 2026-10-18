/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The RN-2 m10 (channels), m12 (receipts) vector twins of
// standard/MESSAGES.md, regenerated through the record writer
// (sdk/rocketnet_message.cpp). No socket, no thread, no clock: a generator
// computes from fixed inputs.

#include "test_unit_standard.h"

#include "rocketnet_message.h"

static const rn_standard_vector_t standard_channels_receipts_security_table[] =
{
    RN_STANDARD_VECTOR( "m10.narrow-channel", "030200050000000000000003686901" ),
    RN_STANDARD_VECTOR( "m10.wide-channel", "13020005000000000000002c01686901" ),
    RN_STANDARD_VECTOR( "m12.promotion", "0a01000000004800010001004101" ),
};

static int standard_channels_receipts_security_generate( const char * id, uint8_t * out, int capacity )
{
    struct rocketnet_message_writer_t writer;
    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );

    if ( strcmp( id, "m10.narrow-channel" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.number = 5;
        record.ring = 0;
        record.ordering = 0;
        record.channel = 3;
        record.data = (const uint8_t*) "hi";
        record.bytes = 2;
        rocketnet_message_writer_begin( &writer, out, capacity );
        if ( !rocketnet_message_writer_add( &writer, &record ) )
            return -1;
        return rocketnet_message_writer_finish( &writer );
    }
    if ( strcmp( id, "m10.wide-channel" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.number = 5;
        record.ring = 0;
        record.ordering = 0;
        record.channel = 300;
        record.data = (const uint8_t*) "hi";
        record.bytes = 2;
        rocketnet_message_writer_begin( &writer, out, capacity );
        if ( !rocketnet_message_writer_add( &writer, &record ) )
            return -1;
        return rocketnet_message_writer_finish( &writer );
    }
    if ( strcmp( id, "m12.promotion" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
        record.block = 1;
        record.number = 0;
        record.ring = 72;
        record.fragment = 1;
        record.fragments = 1;
        record.data = (const uint8_t*) "A";
        record.bytes = 1;
        rocketnet_message_writer_begin( &writer, out, capacity );
        if ( !rocketnet_message_writer_add( &writer, &record ) )
            return -1;
        return rocketnet_message_writer_finish( &writer );
    }
    return -1;
}

static void standard_channels_receipts_security_vectors()
{
    const int count = (int) ( sizeof( standard_channels_receipts_security_table ) / sizeof( standard_channels_receipts_security_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_channels_receipts_security_table, count, standard_channels_receipts_security_generate ) == 3 );
}

REGISTER_UNIT_TEST( standard_channels_receipts_security_vectors );

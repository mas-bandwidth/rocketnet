/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RN-2 m10 channel records and m12 receipt-promotion vectors.

#include "test_unit_standard.h"

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_message.h"

#define check rocketnet_unit_check

static const rn_standard_vector_t standard_channels_receipts_security_table[] = {
    RN_STANDARD_VECTOR( "m10.narrow-channel", "030200050000000000000003686901" ),
    RN_STANDARD_VECTOR( "m10.wide-channel", "13020005000000000000002c01686901" ),
    RN_STANDARD_VECTOR( "m12.promotion",
                        "0a04010100004800010001004141414141414141414141414141414141414141"
                        "4141414141414141414141414141414141414141414141414141414141414141"
                        "4141414141414141414141414141414141414141414141414141414141414141"
                        "4141414141414141414141414141414141414141414141414141414141414141"
                        "4141414141414141414141414141414141414141414141414141414141414141"
                        "4141414141414141414141414141414141414141414141414141414141414141"
                        "4141414141414141414141414141414141414141414141414141414141414141"
                        "4141414141414141414141414141414141414141414141414141414141414141"
                        "4141414141414141414141414141414101" ),

};

static int standard_channels_receipts_security_generate( const char * id, uint8_t * out, int capacity )
{
    struct rocketnet_message_writer_t writer;
    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );

    if ( strcmp( id, "m10.narrow-channel" ) == 0 || strcmp( id, "m10.wide-channel" ) == 0 )
    {
        record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        record.number = 5;
        record.ring = 0;
        record.ordering = 0;
        record.channel = strcmp( id, "m10.wide-channel" ) == 0 ? 300 : 3;
        record.data = (const uint8_t *) "hi";
        record.bytes = 2;
        rocketnet_message_writer_begin( &writer, out, capacity );
        if ( !rocketnet_message_writer_add( &writer, &record ) )
            return -1;
        return rocketnet_message_writer_finish( &writer );
    }

    if ( strcmp( id, "m12.promotion" ) == 0 )
    {
        rocketnet_message_config_t config;
        rocketnet_message_default_config( &config );
        check( rocketnet_message_validate_config( &config ) );

        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
        check( arena != NULL );
        if ( !arena )
            return -1;

        rn_endpoint_config_t endpoint_config;
        memset( &endpoint_config, 0, sizeof( endpoint_config ) );
        endpoint_config.config = &config;
        endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

        rn_endpoint_t endpoint;
        rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
        rn_endpoint_reset( &endpoint, 1000.0 );

        const int maximum = ROCKETNET_MESSAGE_SINGLE_BYTES - 1 - ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
        const int bytes = maximum + 1;
        uint8_t data[ROCKETNET_MESSAGE_SINGLE_BYTES];
        memset( data, 0x41, (size_t) bytes );
        check( rn_endpoint_send( &endpoint, data, bytes, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
                                 ROCKETNET_PRIORITY_MEDIUM, 0, 0 ) == 1 );

        rn_endpoint_begin_update( &endpoint );
        static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        int packet_count = 0;
        int selected_bytes = -1;
        for ( int fragment = 0; fragment < 2; fragment++ )
        {
            const int request = rn_endpoint_next_packet_bytes( &endpoint, 1000.0 );
            check( request > 0 && request <= (int) sizeof( frame ) );
            if ( request <= 0 || request > (int) sizeof( frame ) )
                break;

            const int written = rn_endpoint_write_packet( &endpoint, frame, (int) sizeof( frame ),
                                                          (uint64_t) fragment + 1, 1000.0 );
            check( written > 12 && written <= request );
            if ( written <= 12 || written > request )
                break;

            check( ( frame[0] & ROCKETNET_MESSAGE_FLAG_CODE_MASK ) == ROCKETNET_MESSAGE_CODE_RELIABLE );
            check( ( frame[0] & ROCKETNET_MESSAGE_FLAG_BLOCK ) != 0 );
            const uint32_t chunk_bytes = (uint32_t) frame[1] + ( (uint32_t) frame[2] << 8 );
            check( chunk_bytes == ( fragment == 0 ? ROCKETNET_MESSAGE_FRAGMENT_BYTES : (uint32_t) bytes - ROCKETNET_MESSAGE_FRAGMENT_BYTES ) );
            check( frame[8] == (uint8_t) fragment && frame[9] == 0 );
            check( frame[10] == 1 && frame[11] == 0 );
            check( memcmp( frame + 12, data + fragment * ROCKETNET_MESSAGE_FRAGMENT_BYTES,
                           (size_t) chunk_bytes ) == 0 );
            if ( fragment == 1 )
            {
                check( frame[3] == 1 && frame[4] == 0 && frame[5] == 0 );
                check( frame[6] == ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16 && frame[7] == 0 );
                selected_bytes = written;
                if ( capacity < written )
                {
                    rn_arena_destroy( arena );
                    return -1;
                }
                memcpy( out, frame, (size_t) written );
            }
            rn_endpoint_packet_sent( &endpoint, (uint64_t) fragment + 1, written + 64, 1000.0 );
            packet_count++;
        }
        check( packet_count == 2 );
        check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) == 0 );
        rn_arena_destroy( arena );
        return selected_bytes;
    }

    return -1;
}

static void test_standard_channels_receipts_security_vectors()
{
    const int count = (int) ( sizeof( standard_channels_receipts_security_table ) /
                              sizeof( standard_channels_receipts_security_table[0] ) );
    check( rn_standard_vectors_walk( standard_channels_receipts_security_table, count,
                                     standard_channels_receipts_security_generate ) == count );
}

REGISTER_UNIT_TEST( test_standard_channels_receipts_security_vectors );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_prefilter.cpp -- the vector twins of RN-1 t5
// (standard/TRANSPORT.md, Prefilter header). The accept vector is the header
// the implementation writes for the embedded test title. Each reject vector
// is the packet a receiver is given, rebuilt from the same writer and the
// alteration the section names, after the implementation has refused it with
// the stated reason.

#include "test_unit_standard.h"

#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"

#include <string.h>

#define STANDARD_PREFILTER_TEST_VALUE ROCKETNET_TEST_FILTER_VALUE
#define STANDARD_PREFILTER_LONG_BYTES ( ROCKETNET_RECEIVE_CEILING + 1 )

static void standard_prefilter_write_value( uint8_t * packet, uint8_t packet_type, uint8_t epoch,
                                            const uint8_t * packet_filter )
{
    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, packet_type, packet_filter );
    packet[1] = epoch;
}

static int standard_prefilter_generate( const char * id, uint8_t * out, int capacity )
{
    static const uint8_t test_value[ROCKETNET_FILTER_VALUE_BYTES] = STANDARD_PREFILTER_TEST_VALUE;
    static uint8_t long_packet[STANDARD_PREFILTER_LONG_BYTES];

    if ( strcmp( id, "t5.unkeyed-accept" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;

        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, test_value );
        rocketnet_unit_check( rocketnet_prefilter_check( out, ROCKETNET_PREFILTER_BYTES, test_value ) == ROCKETNET_CONNECTION_PAYLOAD_PACKET );
        return ROCKETNET_PREFILTER_BYTES;
    }

    if ( strcmp( id, "t5.reject-short" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES - 1 )
            return -1;
        standard_prefilter_write_value( out, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_FILTER_EPOCH_UNKEYED, test_value );

        const int bytes = ROCKETNET_PREFILTER_BYTES - 1;
        rocketnet_unit_check( rocketnet_prefilter_check( out, bytes, test_value ) == ROCKETNET_PREFILTER_TOO_SHORT );
        return bytes;
    }

    if ( strcmp( id, "t5.reject-long" ) == 0 )
    {
        if ( capacity < STANDARD_PREFILTER_LONG_BYTES )
            return -1;
        memset( long_packet, 0, sizeof( long_packet ) );
        standard_prefilter_write_value( long_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_FILTER_EPOCH_UNKEYED, test_value );
        rocketnet_unit_check( rocketnet_prefilter_check( long_packet, STANDARD_PREFILTER_LONG_BYTES, test_value ) == ROCKETNET_PREFILTER_TOO_LARGE );

        memcpy( out, long_packet, STANDARD_PREFILTER_LONG_BYTES );
        return STANDARD_PREFILTER_LONG_BYTES;
    }

    if ( strcmp( id, "t5.reject-epoch" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;
        standard_prefilter_write_value( out, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1, test_value );
        rocketnet_unit_check( rocketnet_prefilter_check( out, ROCKETNET_PREFILTER_BYTES, test_value ) == ROCKETNET_PREFILTER_WRONG_EPOCH );
        return ROCKETNET_PREFILTER_BYTES;
    }

    if ( strcmp( id, "t5.reject-value" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;

        static const uint8_t zero_value[ROCKETNET_FILTER_VALUE_BYTES] = { 0, 0, 0, 0 };
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, zero_value );

        rocketnet_unit_check( rocketnet_prefilter_check( out, ROCKETNET_PREFILTER_BYTES, test_value ) == ROCKETNET_PREFILTER_WRONG_VALUE );
        return ROCKETNET_PREFILTER_BYTES;
    }

    return -1;
}

static const rn_standard_vector_t standard_prefilter_table[] = {
    RN_STANDARD_VECTOR( "t5.unkeyed-accept", "0500b7590100" ),
    RN_STANDARD_VECTOR( "t5.reject-short", "0500b75901" ),
    RN_STANDARD_VECTOR( "t5.reject-long",
                        "0500b75901000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000"
                        "00000000000000000000000000" ),
    RN_STANDARD_VECTOR( "t5.reject-epoch", "0501b7590100" ),
    RN_STANDARD_VECTOR( "t5.reject-value", "050000000000" ),
};

static void test_standard_prefilter_vectors()
{
    const int count = (int) ( sizeof( standard_prefilter_table ) / sizeof( standard_prefilter_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_prefilter_table, count, standard_prefilter_generate ) == count );

    // the walk's own refusals, as every vector file checks them

    const uint8_t a[3] = { 1, 2, 3 };
    const uint8_t b[3] = { 1, 9, 3 };
    rocketnet_unit_check( rn_standard_first_difference( a, 3, a, 3 ) == -1 );
    rocketnet_unit_check( rn_standard_first_difference( a, 3, b, 3 ) == 1 );
    rocketnet_unit_check( rn_standard_first_difference( a, 3, a, 2 ) == 2 );

    uint8_t scratch[2];
    rocketnet_unit_check( rn_standard_hex_decode( "0g", scratch, 2 ) == -1 );
    rocketnet_unit_check( rn_standard_hex_decode( "012", scratch, 2 ) == -1 );
    rocketnet_unit_check( rn_standard_hex_decode( "010203", scratch, 2 ) == -1 );
    rocketnet_unit_check( rn_standard_hex_decode( "0a 0b", scratch, 2 ) == 2 && scratch[0] == 0x0a && scratch[1] == 0x0b );
    rocketnet_unit_check( standard_prefilter_generate( "t5.no-such-vector", scratch, 2 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_prefilter_vectors );

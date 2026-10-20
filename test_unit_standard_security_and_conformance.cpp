/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_security_and_conformance.cpp -- the vector twin of RN-1
// t19 (standard/TRANSPORT.md, Security considerations): the amplification
// table, regenerated from the six packet wire sizes of the sdk. One vector.

#include "test_unit_standard.h"

#include "rocketnet_wire.h"

static const rn_standard_vector_t standard_security_and_conformance_table[] =
{
    RN_STANDARD_VECTOR( "t19.amplification", "00c602072600024200033200042900012200" ),
};

static int standard_security_and_conformance_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t19.amplification" ) == 0 )
    {
        static const uint16_t wires[6] =
        {
            ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
            ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES,
            ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
            ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES,
            ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES,
            ROCKETNET_CONNECTION_DENIED_WIRE_BYTES,
        };
        static const uint8_t types[6] =
        {
            ROCKETNET_CONNECTION_REQUEST_PACKET,
            ROCKETNET_CONNECTION_COOKIE_PACKET,
            ROCKETNET_CONNECTION_CHALLENGE_PACKET,
            ROCKETNET_CONNECTION_RESPONSE_PACKET,
            ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
            ROCKETNET_CONNECTION_DENIED_PACKET,
        };
        if ( capacity < 18 )
            return -1;
        for ( int i = 0; i < 6; i++ )
        {
            out[i * 3 + 0] = types[i];
            out[i * 3 + 1] = (uint8_t) ( wires[i] & 0xff );
            out[i * 3 + 2] = (uint8_t) ( wires[i] >> 8 );
        }
        return 18;
    }
    return -1;
}

static void test_standard_security_and_conformance_vectors()
{
    const int count = (int) ( sizeof( standard_security_and_conformance_table ) / sizeof( standard_security_and_conformance_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_security_and_conformance_table, count, standard_security_and_conformance_generate ) == 1 );
    rocketnet_unit_check( standard_security_and_conformance_generate( "t19.no-such-vector", 0, 0 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_security_and_conformance_vectors );

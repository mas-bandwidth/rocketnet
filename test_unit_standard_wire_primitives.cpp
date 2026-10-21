/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_wire_primitives.cpp -- the vector twins of RN-1 t4
// (standard/TRANSPORT.md, Wire encoding primitives), regenerated through the
// implementation. The sequence vectors rebuild the 64-bit value from the
// truncated wire field; the address vectors encode one IPv4 and one IPv6
// address through the sdk.

#include "test_unit_standard.h"

#include "rocketnet.h"
#include "rocketnet_address.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"

static const rn_standard_vector_t standard_wire_primitives_table[] =
{
    RN_STANDARD_VECTOR( "t4.sequence-start", "0100000000000000" ),
    RN_STANDARD_VECTOR( "t4.sequence-steady", "e803000000000000" ),
    RN_STANDARD_VECTOR( "t4.sequence-tie", "0000000002000000" ),
    RN_STANDARD_VECTOR( "t4.sequence-wrap", "0000000001000000" ),
    RN_STANDARD_VECTOR( "t4.sequence-zero", "0000000000000000" ),
    RN_STANDARD_VECTOR( "t4.address-ipv4", "01c6336407d5dd" ),
    RN_STANDARD_VECTOR( "t4.address-ipv6", "020120b80d000000000000000000000700d5dd" ),
};

static int standard_wire_primitives_sequence( uint8_t * out, int capacity, uint32_t wire_field, uint64_t highest_opened )
{
    if ( capacity < 8 )
        return -1;
    uint8_t * p = out;
    rocketnet_write_uint64( &p, rocketnet_reconstruct_sequence( wire_field, highest_opened ) );
    return (int) ( p - out );
}

static int standard_wire_primitives_address( uint8_t * out, int capacity, const struct rocketnet_address_t * address )
{
    if ( capacity < ROCKETNET_ADDRESS_BYTES_IPV6 )
        return -1;
    return rocketnet_write_address_bytes( out, address );
}

static int standard_wire_primitives_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t4.sequence-start" ) == 0 )
        return standard_wire_primitives_sequence( out, capacity, 1u, 0u );
    if ( strcmp( id, "t4.sequence-steady" ) == 0 )
        return standard_wire_primitives_sequence( out, capacity, 1000u, 999u );
    if ( strcmp( id, "t4.sequence-tie" ) == 0 )
        return standard_wire_primitives_sequence( out, capacity, 0u, 0x17fffffffull );
    if ( strcmp( id, "t4.sequence-wrap" ) == 0 )
        return standard_wire_primitives_sequence( out, capacity, 0u, 0xffffffffull );
    if ( strcmp( id, "t4.sequence-zero" ) == 0 )
        return standard_wire_primitives_sequence( out, capacity, 0u, 0u );
    if ( strcmp( id, "t4.address-ipv4" ) == 0 )
    {
        struct rocketnet_address_t address;
        memset( &address, 0, sizeof( address ) );
        address.type = ROCKETNET_ADDRESS_IPV4;
        address.data.ipv4[0] = 198;
        address.data.ipv4[1] = 51;
        address.data.ipv4[2] = 100;
        address.data.ipv4[3] = 7;
        address.port = 56789;
        return standard_wire_primitives_address( out, capacity, &address );
    }
    if ( strcmp( id, "t4.address-ipv6" ) == 0 )
    {
        struct rocketnet_address_t address;
        memset( &address, 0, sizeof( address ) );
        address.type = ROCKETNET_ADDRESS_IPV6;
        address.data.ipv6[0] = 0x2001;
        address.data.ipv6[1] = 0x0db8;
        address.data.ipv6[7] = 0x0007;
        address.port = 56789;
        return standard_wire_primitives_address( out, capacity, &address );
    }
    return -1;
}

static void test_standard_wire_primitives_vectors()
{
    const int count = (int) ( sizeof( standard_wire_primitives_table ) / sizeof( standard_wire_primitives_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_wire_primitives_table, count, standard_wire_primitives_generate ) == count );

    // zero is never a valid sequence: the wire field of zero with nothing
    // opened reconstructs to zero, which the receiver ignores
    uint8_t zero[8];
    rocketnet_unit_check( standard_wire_primitives_generate( "t4.sequence-zero", zero, (int) sizeof( zero ) ) == 8 );
    for ( int i = 0; i < 8; i++ )
        rocketnet_unit_check( zero[i] == 0 );

    // the exact tie takes the larger candidate, and the wrap takes the next
    // 64-bit value
    rocketnet_unit_check( rocketnet_reconstruct_sequence( 0u, 0x17fffffffull ) == 0x200000000ull );
    rocketnet_unit_check( rocketnet_reconstruct_sequence( 0u, 0xffffffffull ) == 0x100000000ull );

    // an unknown id is refused by the generator
    uint8_t scratch[ROCKETNET_ADDRESS_BYTES_IPV6];
    rocketnet_unit_check( standard_wire_primitives_generate( "t4.no-such-vector", scratch, (int) sizeof( scratch ) ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_wire_primitives_vectors );

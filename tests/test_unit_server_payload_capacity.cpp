/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's payload copy (sdk/rocketnet_server_protocol.cpp): the out
// buffer's capacity is checked before any byte is written. Type data above
// the capacity is refused whole and counted as RECEIVE_CURSOR_TOO_SMALL, and
// the buffer beside the copy is untouched; a fitting copy lands byte-exact
// and counts PAYLOAD_DELIVERED. No transport, packet, socket or clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_protocol.h"

#include <stdint.h>
#include <string.h>

int rocketnet_server_payload_copy( uint8_t * out_payload, int out_payload_capacity,
                                   const uint8_t * type_data, int type_data_bytes,
                                   int * out_payload_bytes, uint64_t * counters );

static void test_server_payload_copy_refuses_a_small_capacity()
{
    uint8_t type_data[64];
    for ( int i = 0; i < (int) sizeof( type_data ); i++ )
        type_data[i] = (uint8_t) ( i + 1 );

    struct
    {
        uint8_t buffer[16];
        uint8_t canary[8];
    } out;
    memset( &out, 0xA5, sizeof( out ) );

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    memset( counters, 0, sizeof( counters ) );

    int out_payload_bytes = 12345;

    rocketnet_unit_check( rocketnet_server_payload_copy( out.buffer, (int) sizeof( out.buffer ),
                                                         type_data, (int) sizeof( type_data ),
                                                         &out_payload_bytes, counters ) == 0 );
    rocketnet_unit_check( out_payload_bytes == 12345 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 1 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] == 0 );
    for ( int i = 0; i < (int) sizeof( out ); i++ )
        rocketnet_unit_check( ( (uint8_t *) &out )[i] == 0xA5 );

    uint8_t full[sizeof( type_data )];
    memset( full, 0, sizeof( full ) );
    out_payload_bytes = 0;

    rocketnet_unit_check( rocketnet_server_payload_copy( full, (int) sizeof( full ),
                                                         type_data, (int) sizeof( type_data ),
                                                         &out_payload_bytes, counters ) == 1 );
    rocketnet_unit_check( out_payload_bytes == (int) sizeof( type_data ) );
    rocketnet_unit_check( memcmp( full, type_data, sizeof( type_data ) ) == 0 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] == 1 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 1 );
}

REGISTER_UNIT_TEST( test_server_payload_copy_refuses_a_small_capacity );

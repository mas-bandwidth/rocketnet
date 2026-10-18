/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_receive_processing.cpp -- the vector twins of RN-1 t10
// (standard/TRANSPORT.md, Receive processing), regenerated through the
// implementation. Each vector is one malformed packet and the counter its
// refusal fires. The bytes come from the wire writers, the counter from the
// same prefilter, server and client entry points a live packet takes.

#include "test_unit_standard.h"

#include "rocketnet.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"

static const rn_standard_vector_t standard_receive_processing_table[] =
{
    RN_STANDARD_VECTOR( "t10.prefilter-too-short", "0500b75901" ),
    RN_STANDARD_VECTOR( "t10.prefilter-wrong-epoch", "0501b7590100" ),
    RN_STANDARD_VECTOR( "t10.prefilter-wrong-value", "050000000000" ),
    RN_STANDARD_VECTOR( "t10.reserved-type", "0a00b7590100" ),
    RN_STANDARD_VECTOR( "t10.server-wrong-direction", "0100b7590100" ),
    RN_STANDARD_VECTOR( "t10.client-session-wrong-size", "0400b7590100" ),
};

static int standard_receive_processing_generate( const char * id, uint8_t * out, int capacity )
{
    const uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    const uint8_t no_value[ROCKETNET_FILTER_VALUE_BYTES] = { 0, 0, 0, 0 };
    uint8_t * p = out;

    if ( strcmp( id, "t10.prefilter-too-short" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, filter_value );
        return ROCKETNET_PREFILTER_BYTES - 1;
    }
    if ( strcmp( id, "t10.prefilter-wrong-epoch" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, filter_value );
        out[1] = 1;
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.prefilter-wrong-value" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, no_value );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.reserved-type" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_FRAGMENT_PACKET + 1, filter_value );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.server-wrong-direction" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_DENIED_PACKET, filter_value );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.client-session-wrong-size" ) == 0 )
    {
        if ( capacity < ROCKETNET_PREFILTER_BYTES )
            return -1;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, filter_value );
        return ROCKETNET_PREFILTER_BYTES;
    }
    return -1;
}

static void standard_receive_processing_counters()
{
    uint8_t packet[RN_STANDARD_VECTOR_MAX_BYTES];
    const uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    // the prefilter's four refusals, through the shipped comparison
    {
        const int bytes = standard_receive_processing_generate( "t10.prefilter-too-short", packet, (int) sizeof( packet ) );
        const int verdict = rocketnet_prefilter_check( packet, bytes, filter_value );
        rocketnet_unit_check( verdict == ROCKETNET_PREFILTER_TOO_SHORT );
        rocketnet_unit_check( rocketnet_prefilter_counter( verdict ) == ROCKETNET_COUNTER_PREFILTER_TOO_SHORT );
    }
    {
        const int bytes = standard_receive_processing_generate( "t10.prefilter-wrong-epoch", packet, (int) sizeof( packet ) );
        const int verdict = rocketnet_prefilter_check( packet, bytes, filter_value );
        rocketnet_unit_check( verdict == ROCKETNET_PREFILTER_WRONG_EPOCH );
        rocketnet_unit_check( rocketnet_prefilter_counter( verdict ) == ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH );
    }
    {
        const int bytes = standard_receive_processing_generate( "t10.prefilter-wrong-value", packet, (int) sizeof( packet ) );
        const int verdict = rocketnet_prefilter_check( packet, bytes, filter_value );
        rocketnet_unit_check( verdict == ROCKETNET_PREFILTER_WRONG_VALUE );
        rocketnet_unit_check( rocketnet_prefilter_counter( verdict ) == ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE );
    }

    // the reserved type and the wrong direction, through the server's entry
    struct rocketnet_counters_t counters;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];

    memset( &counters, 0, sizeof( counters ) );
    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );
    memcpy( server.filter_value, filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    server.counters = &counters;

    struct rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );

    {
        const int bytes = standard_receive_processing_generate( "t10.reserved-type", packet, (int) sizeof( packet ) );
        int response_bytes = 0;
        int payload_bytes = 0;
        int payload_client_index = -1;
        rocketnet_server_protocol_process_packet( &server, &from, packet, bytes, 0, 1000.0,
                                                  response, &response_bytes, payload, &payload_bytes, &payload_client_index );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_RESERVED_TYPE] == 1 );
    }

    memset( &counters, 0, sizeof( counters ) );
    {
        const int bytes = standard_receive_processing_generate( "t10.server-wrong-direction", packet, (int) sizeof( packet ) );
        int response_bytes = 0;
        int payload_bytes = 0;
        int payload_client_index = -1;
        rocketnet_server_protocol_process_packet( &server, &from, packet, bytes, 0, 1000.0,
                                                  response, &response_bytes, payload, &payload_bytes, &payload_client_index );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_WRONG_DIRECTION] == 1 );
    }

    // the session-type wire floor, through the client's entry: size before
    // the state gate, and no integrity failure
    memset( &counters, 0, sizeof( counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );
    client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    {
        const int bytes = standard_receive_processing_generate( "t10.client-session-wrong-size", packet, (int) sizeof( packet ) );
        int payload_bytes = 0;
        rocketnet_client_protocol_process_packet( &client, packet, bytes, 1000.0, payload, (int) sizeof( payload ), &payload_bytes );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE] == 1 );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN] == 0 );
        rocketnet_unit_check( client.integrity_failures == 0 );
    }
}

static void standard_receive_processing_vectors()
{
    const int count = (int) ( sizeof( standard_receive_processing_table ) / sizeof( standard_receive_processing_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_receive_processing_table, count, standard_receive_processing_generate ) == count );

    standard_receive_processing_counters();
}

REGISTER_UNIT_TEST( standard_receive_processing_vectors );

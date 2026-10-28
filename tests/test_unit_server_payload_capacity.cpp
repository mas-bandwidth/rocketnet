/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server transport's in-place receive entry refuses a payload that does
// not fit the caller's cursor whole, before publishing any payload metadata.
// A fitting sealed payload is delivered byte-exact from the open buffer with
// its live slot and connection generation, and its deferred delivery ACK is
// accepted only when the caller explicitly accepts it.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

static void check_counter_delta( const struct rocketnet_counters_t * before,
                                 const struct rocketnet_counters_t * after,
                                 int first, int second, int third )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const uint64_t expected = ( i == first || i == second || i == third ) ? 1 : 0;
        rocketnet_unit_check( after->values[i] - before->values[i] == expected );
    }
}

static int seal_payload( uint8_t * packet, uint64_t sequence, const uint8_t * data, int data_bytes )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 64];
    rocketnet_unit_check( data_bytes <= 64 );

    memset( plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    memcpy( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, data, (size_t) data_bytes );
    return rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                          rocketnet_test_filter_value, sequence, NULL,
                                          plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + data_bytes,
                                          ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                          rocketnet_test_session_c2s_key );
}

static void test_server_payload_capacity_uses_live_session_path()
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    memset( &server, 0, sizeof( server ) );
    memset( &counters, 0, sizeof( counters ) );

    uint64_t connection_ids[1] = { 41 };
    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    server.encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server.client_slots = rocketnet_client_slots_create( NULL, 1 );
    server.acks = rocketnet_server_acks_create( NULL, 1 );
    server.counters = &counters;
    server.connection_ids = connection_ids;
    server.defer_payload_acks = 1;
    rocketnet_unit_check( server.encryption_map && server.client_slots && server.acks );

    struct rocketnet_address_t from;
    rocketnet_unit_check( rocketnet_address_parse( &from, "198.51.100.7:56789" ) );

    const int mapping_index = rocketnet_encryption_map_add( server.encryption_map, &from, 1005.0 );
    rocketnet_unit_check( mapping_index >= 0 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server.encryption_map, mapping_index );
    memcpy( mapping->keys.session_client_to_server, rocketnet_test_session_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( mapping->connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    const int client_index = rocketnet_client_slots_assign( server.client_slots, 0x1400, mapping_index, user_data );
    rocketnet_unit_check( client_index == 0 );

    mapping->control->client_index = client_index;
    mapping->control->established = 1;

    const uint8_t expected[32] = {
        0x01,
        0x23,
        0x45,
        0x67,
        0x89,
        0xab,
        0xcd,
        0xef,
        0x10,
        0x32,
        0x54,
        0x76,
        0x98,
        0xba,
        0xdc,
        0xfe,
        0xff,
        0xee,
        0xdd,
        0xcc,
        0xbb,
        0xaa,
        0x99,
        0x88,
        0x77,
        0x66,
        0x55,
        0x44,
        0x33,
        0x22,
        0x11,
        0x00,
    };
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 777;
    const uint8_t * payload = response;
    int payload_bytes = 777;
    int payload_client_index = 777;

    int packet_bytes = seal_payload( packet, 1, expected, (int) sizeof( expected ) );
    rocketnet_unit_check( packet_bytes > 0 );

    const struct rocketnet_counters_t before_small = counters;
    rocketnet_unit_check( rocketnet_server_protocol_process_received( &server, &from, mapping_index,
                                                                      packet, packet_bytes,
                                                                      ROCKETNET_TEST_CREATE_TS, 100.0,
                                                                      response, &response_bytes,
                                                                      opened, (int) sizeof( expected ) - 1,
                                                                      &payload, &payload_bytes,
                                                                      &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( response_bytes == 0 );
    rocketnet_unit_check( payload == NULL );
    rocketnet_unit_check( payload_bytes == 0 );
    rocketnet_unit_check( payload_client_index == -1 );
    rocketnet_unit_check( server.payload_sequence == 0 );
    rocketnet_unit_check( server.payload_connection_id == 0 );
    rocketnet_unit_check( mapping->replay_session.highest_opened == 1 );
    rocketnet_unit_check( mapping->accepted_session.highest_opened == 0 );
    rocketnet_unit_check( server.acks->endpoints[client_index].ack_debt == 0 );

    check_counter_delta( &before_small, &counters,
                         ROCKETNET_COUNTER_PACKETS_RECEIVED,
                         ROCKETNET_COUNTER_PAYLOAD_RECEIVED,
                         ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL );

    packet_bytes = seal_payload( packet, 2, expected, (int) sizeof( expected ) );
    rocketnet_unit_check( packet_bytes > 0 );

    const struct rocketnet_counters_t before_fit = counters;
    rocketnet_unit_check( rocketnet_server_protocol_process_received( &server, &from, mapping_index,
                                                                      packet, packet_bytes,
                                                                      ROCKETNET_TEST_CREATE_TS, 100.1,
                                                                      response, &response_bytes,
                                                                      opened, (int) sizeof( expected ),
                                                                      &payload, &payload_bytes,
                                                                      &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_unit_check( payload >= opened && payload + payload_bytes <= opened + sizeof( opened ) );
    rocketnet_unit_check( payload_bytes == (int) sizeof( expected ) );
    rocketnet_unit_check( memcmp( payload, expected, sizeof( expected ) ) == 0 );
    rocketnet_unit_check( payload_client_index == client_index );
    rocketnet_unit_check( server.payload_sequence == 2 );
    rocketnet_unit_check( server.payload_connection_id == connection_ids[client_index] );
    rocketnet_unit_check( mapping->replay_session.highest_opened == 2 );
    rocketnet_unit_check( mapping->accepted_session.highest_opened == 0 );
    rocketnet_unit_check( server.acks->endpoints[client_index].ack_debt == 0 );

    check_counter_delta( &before_fit, &counters,
                         ROCKETNET_COUNTER_PACKETS_RECEIVED,
                         ROCKETNET_COUNTER_PAYLOAD_RECEIVED,
                         ROCKETNET_COUNTER_PAYLOAD_DELIVERED );

    rocketnet_server_protocol_accept_payload( &server, payload_client_index,
                                              server.payload_connection_id,
                                              server.payload_sequence );

    rocketnet_unit_check( mapping->accepted_session.highest_opened == 2 );
    rocketnet_unit_check( server.acks->endpoints[client_index].ack_debt == 1 );

    rocketnet_server_acks_destroy( server.acks );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_encryption_map_destroy( server.encryption_map );
}

REGISTER_UNIT_TEST( test_server_payload_capacity_uses_live_session_path );

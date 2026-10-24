/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_session.cpp -- the vector twins of RN-1 t13
// (standard/TRANSPORT.md, Session: keep-alive, timeout, disconnect, slot
// reset), regenerated through the implementation. The vector is the
// keep-alive suppression transcript: one byte per due check, 01 sent and 00
// suppressed, before and after a keep-alive is acknowledged.

#include "test_unit_standard.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <string.h>

struct rocketnet_session_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
    int mapping_index;
};

static void rocketnet_session_server_setup_clients( struct rocketnet_session_server_t * test, int max_clients )
{
    memset( test, 0, sizeof(*test) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = 0x1122334455667788ULL;
    server->send_rate = 10.0;
    memset( server->packet_filter, 0x11, ROCKETNET_FILTER_VALUE_BYTES );
    server->encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server->client_slots = rocketnet_client_slots_create( NULL, max_clients );
    server->acks = rocketnet_server_acks_create( NULL, max_clients );
    server->counters = &test->counters;
    rocketnet_unit_check( server->encryption_map && server->client_slots && server->acks );

    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );

    test->mapping_index = rocketnet_encryption_map_add( server->encryption_map, &test->client_address, 1005.0 );
    rocketnet_unit_check( test->mapping_index >= 0 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, test->mapping_index );
    mapping->control->established = 1;
    mapping->control->client_index = 0;
    mapping->control->timeout_seconds = 10;
    mapping->control->last_receive_time = 1000.0;
    mapping->last_send_time = 1000.0;
    mapping->last_keep_alive_send_time = 1000.0;
    mapping->send_sequence_session = 1;
    memset( mapping->keys.session_server_to_client, 0x42, 32 );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );
    rocketnet_unit_check( rocketnet_client_slots_assign( server->client_slots, 0x1234, test->mapping_index, user_data ) == 0 );
}

static void rocketnet_session_server_setup( struct rocketnet_session_server_t * test )
{
    rocketnet_session_server_setup_clients( test, 1 );
}

static void rocketnet_session_server_teardown( struct rocketnet_session_server_t * test )
{
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
}

static int rocketnet_session_keep_alive_due( struct rocketnet_session_server_t * test, double now )
{
    uint8_t packet[ROCKETNET_SEND_BUDGET];
    return rocketnet_server_protocol_write_keep_alive_if_due( &test->server, 0, now, packet ) > 0;
}

static void rocketnet_session_send_payload( struct rocketnet_session_server_t * test, double now )
{
    uint8_t packet[ROCKETNET_SEND_BUDGET];
    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_unit_check( rocketnet_server_protocol_write_payload( &test->server, 0, data, 4, now, packet ) > 0 );
}

static int rocketnet_session_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t13.keep-alive-suppression" ) != 0 )
        return -1;
    if ( capacity < 7 )
        return -1;

    struct rocketnet_session_server_t test;
    rocketnet_session_server_setup( &test );

    int bytes = 0;

    out[bytes++] = (uint8_t) rocketnet_session_keep_alive_due( &test, 1000.05 );
    rocketnet_session_send_payload( &test, 1000.10 );
    out[bytes++] = (uint8_t) rocketnet_session_keep_alive_due( &test, 1000.15 );
    out[bytes++] = (uint8_t) rocketnet_session_keep_alive_due( &test, 1000.20 );
    out[bytes++] = (uint8_t) rocketnet_session_keep_alive_due( &test, 1000.30 );

    test.server.acks->endpoints[0].keep_alive_acked = 1;

    out[bytes++] = (uint8_t) rocketnet_session_keep_alive_due( &test, 1000.35 );
    rocketnet_session_send_payload( &test, 1000.40 );
    out[bytes++] = (uint8_t) rocketnet_session_keep_alive_due( &test, 1000.45 );
    out[bytes++] = (uint8_t) rocketnet_session_keep_alive_due( &test, 1000.55 );

    rocketnet_session_server_teardown( &test );
    return bytes;
}

static const rn_standard_vector_t standard_session_table[] =
{
    RN_STANDARD_VECTOR( "t13.keep-alive-suppression", "00010001000001" ),
};

static void test_standard_session_vectors()
{
    const int count = (int) ( sizeof( standard_session_table ) / sizeof( standard_session_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_session_table, count, rocketnet_session_generate ) == 1 );

    rocketnet_unit_check( rocketnet_session_generate( "t13.no-such-vector", NULL, 0 ) == -1 );

    uint8_t bytes[7];
    rocketnet_unit_check( rn_standard_hex_decode( standard_session_table[0].hex, bytes, (int) sizeof( bytes ) ) == 7 );
    rocketnet_unit_check( bytes[0] == 0 && bytes[1] == 1 && bytes[2] == 0 && bytes[3] == 1 &&
                          bytes[4] == 0 && bytes[5] == 0 && bytes[6] == 1 );
}

REGISTER_UNIT_TEST( test_standard_session_vectors );

// A connected slot may hold mapping index -1: rocketnet_client_slots_assign
// stores any index it is given. Accepting a payload for such a slot must
// return before any mapping is read through the index, and add no ack debt.

static void test_standard_session_accept_payload_without_mapping_touches_nothing()
{
    struct rocketnet_session_server_t test;
    rocketnet_session_server_setup_clients( &test, 2 );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );
    const int client_index = rocketnet_client_slots_assign( test.server.client_slots, 0x5678, -1, user_data );
    rocketnet_unit_check( client_index == 1 );
    rocketnet_unit_check( rocketnet_client_slots_connected( test.server.client_slots, client_index ) );
    rocketnet_unit_check( rocketnet_client_slots_mapping_index( test.server.client_slots, client_index ) == -1 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, test.mapping_index );
    struct rocketnet_replay_protection_t accepted_before;
    memcpy( &accepted_before, &mapping->accepted_session, sizeof( accepted_before ) );

    rocketnet_server_protocol_accept_payload( &test.server, client_index, 0, 7 );

    rocketnet_unit_check( test.server.acks->endpoints[client_index].ack_debt == 0 );
    rocketnet_unit_check( test.server.acks->endpoints[0].ack_debt == 0 );
    rocketnet_unit_check( memcmp( &accepted_before, &mapping->accepted_session, sizeof( accepted_before ) ) == 0 );

    // the slot with a live mapping still takes the same payload
    rocketnet_server_protocol_accept_payload( &test.server, 0, 0, 7 );
    rocketnet_unit_check( test.server.acks->endpoints[0].ack_debt == 1 );
    rocketnet_unit_check( test.server.acks->endpoints[client_index].ack_debt == 0 );

    rocketnet_session_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_standard_session_accept_payload_without_mapping_touches_nothing );

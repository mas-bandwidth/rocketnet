/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server slot gate runs before decryption (TRANSPORT.md t10 step 12): a
// session packet for a mapping that holds no client slot -- a half-open
// connection, challenged but not yet answered -- is dropped as the type's
// NOT_CONNECTED without an AEAD open. It is never counted DID_NOT_OPEN, it
// never feeds the mapping's integrity-failure count, and the session window
// never moves (tla/ReplayWindow.tla NoAdvanceBeforeConnect and
// NoOpenBeforeConnect). The connection response is the one sealed type a
// half-open mapping exists to receive, so it alone still reaches the open.
//
// The open is observed through the crypto tally every AEAD open increments
// (sdk/rocketnet_protocol.cpp, rocketnet_open_sealed_packet). Time is an
// argument; the clock starts at 1000.0 and nothing here sleeps, opens a
// socket or reads a clock.

#include "test_unit_registry.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

struct half_open_gate_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void half_open_gate_server_setup( struct half_open_gate_server_t * test )
{
    memset( test, 0, sizeof(*test) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots );

    rocketnet_server_protocol_update( server, 1000.0 );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void half_open_gate_server_teardown( struct half_open_gate_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
}

// a valid connection request carrying a valid return cookie: the server
// answers it with a challenge and holds a mapping with no slot
static void half_open_gate_challenge( struct half_open_gate_server_t * test )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    check( rocketnet_generate_cookie_mac( cookie_mac, &test->client_address, rocketnet_test_salt, cookie_expire, test->server.cookie_key ) );
    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    check( rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address,
                                                                 request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                 response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
}

// a sealed packet of the given type and size from the client's address: a
// valid prefilter header, a nonzero sequence, and sealed bytes nobody sealed
static int half_open_gate_send( struct half_open_gate_server_t * test, uint8_t packet_type, int packet_bytes, uint32_t sequence )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    memset( packet, 0x5A, sizeof(packet) );
    uint8_t * p = packet;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_prefilter_header( &p, packet_type, tag );
    rocketnet_write_uint32( &p, sequence );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    return rocketnet_server_protocol_process_packet( &test->server, &test->client_address, packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, 1000.5, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client_index );
}

static void test_server_half_open_session_packet_dropped_unopened()
{
    static struct half_open_gate_server_t test;
    half_open_gate_server_setup( &test );
    half_open_gate_challenge( &test );

    const int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    check( index != -1 );
    check( rocketnet_encryption_map_entry( test.server.encryption_map, index )->control->client_index == -1 );

    const struct { uint8_t type; int bytes; int received; int not_connected; int did_not_open; } rows[] =
    {
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES,
          ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED, ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES + 64,
          ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED, ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN },
        { ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES,
          ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED, ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN },
        { ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES,
          ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_NOT_CONNECTED, ROCKETNET_COUNTER_ACK_DID_NOT_OPEN },
        { ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES + 64,
          ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED, ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN },
    };

    for ( size_t i = 0; i < sizeof( rows ) / sizeof( rows[0] ); i++ )
    {
        const struct rocketnet_counters_t before = test.counters;
        const struct rocketnet_crypto_tally_t tally_before = rocketnet_crypto_tally_get();

        check( half_open_gate_send( &test, rows[i].type, rows[i].bytes, (uint32_t) ( i + 1 ) ) == ROCKETNET_SERVER_PROCESS_IGNORED );

        // dropped at the gate: no AEAD open was attempted
        const struct rocketnet_crypto_tally_t tally_after = rocketnet_crypto_tally_get();
        check( tally_after.aead_open == tally_before.aead_open );

        // counted as what it is: a packet for a mapping with no slot, never
        // as a packet that failed to open
        check( test.counters.values[rows[i].received] == before.values[rows[i].received] + 1 );
        check( test.counters.values[rows[i].not_connected] == before.values[rows[i].not_connected] + 1 );
        check( test.counters.values[rows[i].did_not_open] == before.values[rows[i].did_not_open] );

        // the mapping lives, unslotted, with no integrity failure and a
        // session window that has not moved
        check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == index );
        const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
        check( mapping->control->client_index == -1 );
        check( mapping->integrity_failures == 0 );
        check( mapping->replay_session.highest_opened == 0 );
    }

    // the connection response is not gated: a forged one still reaches the
    // open and is counted as a response that did not open
    {
        const uint64_t did_not_open_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN];
        const struct rocketnet_crypto_tally_t tally_before = rocketnet_crypto_tally_get();
        check( half_open_gate_send( &test, ROCKETNET_CONNECTION_RESPONSE_PACKET, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, 1 ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        const struct rocketnet_crypto_tally_t tally_after = rocketnet_crypto_tally_get();
        check( tally_after.aead_open == tally_before.aead_open + 1 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN] == did_not_open_before + 1 );
        check( rocketnet_encryption_map_entry( test.server.encryption_map, index )->integrity_failures == 1 );
    }

    half_open_gate_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_half_open_session_packet_dropped_unopened );

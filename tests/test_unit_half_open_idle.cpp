/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A half-open mapping is bound by an idle deadline as well as its fixed
// handshake deadline (STANDARD.md _Half-open state_): the earlier of the
// two removes it, a request inside the idle timeout refreshes it, and the
// removal is counted as MAPPING_IDLE_TIMEOUT rather than as a handshake
// expiry. Time is always an argument; the test clock starts at 1000.0 and
// nothing here sleeps, opens a socket or reads a clock.

#include "test_unit_registry.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

struct half_open_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void half_open_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                          struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof( *private_fields ) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

static void half_open_server_setup( struct half_open_server_t * test, double handshake_deadline_seconds )
{
    memset( test, 0, sizeof( *test ) );

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
    server->handshake_deadline_seconds = handshake_deadline_seconds;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots );

    rocketnet_server_protocol_update( server, 1000.0 );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void half_open_server_teardown( struct half_open_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
}

static void half_open_build_request( uint8_t * request,
                                     const struct rocketnet_address_t * address,
                                     const uint8_t * cookie_key )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    half_open_fill_vector_fields( &signed_fields, &private_fields );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    check( rocketnet_generate_cookie_mac( cookie_mac, address, rocketnet_test_salt, cookie_expire, cookie_key ) );

    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
}

static int half_open_send_request( struct half_open_server_t * test, const uint8_t * request, double now )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    return rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address,
                                                                 request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 ROCKETNET_TEST_CREATE_TS, now,
                                                                 response, &response_bytes );
}

static void half_open_idle_timeout_removes_and_refreshes()
{
    check( ROCKETNET_HALF_OPEN_IDLE_TIMEOUT_SECONDS == 10.0 );

    struct half_open_server_t test;
    half_open_server_setup( &test, 60.0 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    half_open_build_request( request, &test.client_address, test.server.cookie_key );

    check( half_open_send_request( &test, request, 1000.0 ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    check( index != -1 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
    check( mapping->control->established == 0 );
    check( mapping->control->handshake_deadline == 1060.0 );
    check( mapping->control->last_receive_time == 1000.0 );

    // one second inside the idle timeout: the mapping lives

    rocketnet_server_protocol_update( &test.server, 1001.0 );
    check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == index );

    // a repeated request is traffic: it refreshes the idle clock, so the
    // mapping survives a point past the original idle deadline

    check( half_open_send_request( &test, request, 1005.0 ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
    check( mapping->control->last_receive_time == 1005.0 );
    check( mapping->control->handshake_deadline == 1060.0 );

    rocketnet_server_protocol_update( &test.server, 1014.5 );
    check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == index );

    // no request for ten seconds after 1005: the idle deadline, earlier
    // than the handshake deadline, removes it and counts it as idle

    rocketnet_server_protocol_update( &test.server, 1015.0 );
    check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );
    check( test.counters.values[ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 0 );

    half_open_server_teardown( &test );
}

static void half_open_handshake_deadline_wins_when_earlier()
{
    struct half_open_server_t test;
    half_open_server_setup( &test, 1.0 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    half_open_build_request( request, &test.client_address, test.server.cookie_key );
    check( half_open_send_request( &test, request, 1000.0 ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    check( index != -1 );
    check( rocketnet_encryption_map_entry( test.server.encryption_map, index )->control->handshake_deadline == 1001.0 );

    // the handshake deadline fires first, so its own counter names the removal

    rocketnet_server_protocol_update( &test.server, 1001.5 );
    check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );
    check( test.counters.values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT] == 0 );

    half_open_server_teardown( &test );
}

static void test_half_open_mapping_idle_deadline()
{
    half_open_idle_timeout_removes_and_refreshes();
    half_open_handshake_deadline_wins_when_earlier();
}

REGISTER_UNIT_TEST( test_half_open_mapping_idle_deadline );

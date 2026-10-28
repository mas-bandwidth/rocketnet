/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// STANDARD.md, "Processing a connection request packet": a request from an
// address that already holds an encryption mapping is answered with the
// mapping's stored challenge, byte-identical, whatever salt the request
// carries. The mapping is created once and replaced never, so the challenge
// it stored is the only one this address can ever see while it lives.

#include "test_unit_registry.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#define check rocketnet_unit_check

struct salt_mismatch_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void salt_mismatch_server_setup( struct salt_mismatch_server_t * test )
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
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots );

    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void salt_mismatch_server_teardown( struct salt_mismatch_server_t * test )
{
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
}

static void salt_mismatch_build_request( uint8_t * request, const uint8_t * salt )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;

    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

static int salt_mismatch_process( struct salt_mismatch_server_t * test,
                                  const uint8_t * request, uint8_t * response, int * response_bytes )
{
    return rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address,
                                                                 request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                 response, response_bytes );
}

static const uint8_t salt_mismatch_alternate_salt[ROCKETNET_CONNECTION_SALT_BYTES] = {
    0x53, 0x41, 0x4c, 0x54, 0x30, 0x30, 0x30, 0x32
};

static void test_salt_mismatch_resends_stored_challenge()
{
    struct salt_mismatch_server_t test;
    salt_mismatch_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    salt_mismatch_build_request( request, rocketnet_test_salt );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    // the cookie exchange, then the request that creates the half-open mapping

    check( salt_mismatch_process( &test, request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    memcpy( request + ROCKETNET_PREFILTER_BYTES,
            response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
            ROCKETNET_COOKIE_FIELD_BYTES );
    check( salt_mismatch_process( &test, request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    uint8_t stored[ROCKETNET_RECEIVE_CEILING];
    memcpy( stored, response, (size_t) response_bytes );
    const int stored_bytes = response_bytes;

    // a same-salt duplicate is answered with the stored challenge

    check( salt_mismatch_process( &test, request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( response_bytes == stored_bytes );
    check( memcmp( response, stored, (size_t) stored_bytes ) == 0 );

    // a request from the same address with another salt gets the same bytes:
    // the mapping's stored challenge, not a new one and not silence

    uint8_t other_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    salt_mismatch_build_request( other_request, salt_mismatch_alternate_salt );
    memcpy( other_request + ROCKETNET_PREFILTER_BYTES, request + ROCKETNET_PREFILTER_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );

    const uint64_t resent_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT];
    check( salt_mismatch_process( &test, other_request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( response_bytes == stored_bytes );
    check( memcmp( response, stored, (size_t) stored_bytes ) == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT] == resent_before + 1 );

    // one mapping, still carrying the salt and nonce it was created with

    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );

    const int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    check( index != -1 );

    const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
    check( memcmp( mapping->connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES ) == 0 );

    salt_mismatch_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_salt_mismatch_resends_stored_challenge );

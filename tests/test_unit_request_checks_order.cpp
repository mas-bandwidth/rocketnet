/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// standard/TRANSPORT.md t12 rules 1 to 6 and t17 rule 4: the connection
// request check sequence and the half-open refresh.
//
// The refresh: a request for the mapping's token from an address holding a
// half-open mapping is answered with the stored challenge whatever its salt, and only a request
// whose connection salt matches the mapping's refreshes the half-open idle
// deadline (sdk/rocketnet_server_protocol.cpp, the mapping branch of
// rocketnet_server_protocol_process_connection_request).
//
// The check sequence: a request that fails two rungs counts the rung the code checks
// first, so the counter a refusal lands on names the check sequence's order.

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

struct request_checks_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void request_checks_server_setup( struct request_checks_server_t * test )
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

    rocketnet_server_protocol_update( server, 1000.0 );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void request_checks_server_teardown( struct request_checks_server_t * test )
{
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
}

static void request_checks_build_request( uint8_t * request, const uint8_t * salt )
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

static int request_checks_process( struct request_checks_server_t * test, const uint8_t * request,
                                   uint64_t current_timestamp, double current_time,
                                   uint8_t * response, int * response_bytes )
{
    return rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address,
                                                                 request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 current_timestamp, current_time,
                                                                 response, response_bytes );
}

static const uint8_t request_checks_other_salt[ROCKETNET_CONNECTION_SALT_BYTES] = {
    0x4c, 0x41, 0x44, 0x44, 0x45, 0x52, 0x30, 0x32
};

// t17 rule 4: a wrong salt is answered, and does not keep the mapping alive
static void test_request_checks_wrong_salt_does_not_refresh_half_open()
{
    struct request_checks_server_t test;
    request_checks_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    request_checks_build_request( request, rocketnet_test_salt );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    // the cookie exchange, then the request that creates the half-open mapping at 1000.0

    check( request_checks_process( &test, request, ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    memcpy( request + ROCKETNET_PREFILTER_BYTES,
            response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
            ROCKETNET_COOKIE_FIELD_BYTES );
    check( request_checks_process( &test, request, ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    const int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    check( index != -1 );

    const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
    check( mapping->control->last_receive_time == 1000.0 );

    // the same token with another salt from the same address: the stored
    // challenge goes back, the idle clock stays

    uint8_t other_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    memcpy( other_request, request, sizeof( other_request ) );
    memcpy( other_request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, request_checks_other_salt, ROCKETNET_CONNECTION_SALT_BYTES );

    const uint64_t resent_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT];
    check( request_checks_process( &test, other_request, ROCKETNET_TEST_CREATE_TS, 1003.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT] == resent_before + 1 );
    check( mapping->control->last_receive_time == 1000.0 );

    // the matching salt refreshes it

    check( request_checks_process( &test, request, ROCKETNET_TEST_CREATE_TS, 1004.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( mapping->control->last_receive_time == 1004.0 );

    request_checks_server_teardown( &test );
}

// t12 rules 4 to 6: expiry is checked before the server public key, and the
// version before expiry; each two-rung failure counts the earlier rung only
static void test_request_checks_two_rung_failure_counts_first_rung()
{
    struct request_checks_server_t test;
    request_checks_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    const int signed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                              ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;

    // expired (current timestamp at the expire timestamp) and sealed to another server key

    request_checks_build_request( request, rocketnet_test_salt );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_SERVER_KEY] ^= 1;
    check( request_checks_process( &test, request, ROCKETNET_TEST_EXPIRE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( response_bytes == 0 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] == 0 );

    // a wrong version and expired

    request_checks_build_request( request, rocketnet_test_salt );
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_VERSION] ^= 1;
    check( request_checks_process( &test, request, ROCKETNET_TEST_EXPIRE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] == 1 );

    request_checks_server_teardown( &test );
}

// t12: the signed zero pad and the server public key are compared over their
// whole width (plain memcmp: both operands are public). A flip of any one
// byte of either is refused, unanswered, on its own rung and no other; with
// both wrong the pad's rung counts; the unmodified request still gets its
// cookie.
static void test_request_checks_pad_and_server_key_compare_every_byte()
{
    struct request_checks_server_t test;
    request_checks_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    const int signed_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                              ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    const int pad_offset = ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES;
    const int pad_bytes = ROCKETNET_SIGNED_CONNECT_DATA_BYTES - pad_offset;
    check( pad_bytes > 0 );

    struct rocketnet_counters_t before;
    memcpy( &before, &test.counters, sizeof( before ) );

    for ( int i = 0; i < pad_bytes; i++ )
    {
        request_checks_build_request( request, rocketnet_test_salt );
        request[signed_offset + pad_offset + i] = 0x80;
        response_bytes = -1;
        check( request_checks_process( &test, request, ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
    }

    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        request_checks_build_request( request, rocketnet_test_salt );
        request[signed_offset + ROCKETNET_SIGNED_OFFSET_SERVER_KEY + i] ^= 0x01;
        response_bytes = -1;
        check( request_checks_process( &test, request, ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
    }

    // both wrong: the pad is the earlier rung

    request_checks_build_request( request, rocketnet_test_salt );
    request[signed_offset + pad_offset + pad_bytes - 1] = 0x01;
    request[signed_offset + ROCKETNET_SIGNED_OFFSET_SERVER_KEY + ROCKETNET_KEY_BYTES - 1] ^= 0x01;
    check( request_checks_process( &test, request, ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );

    const uint64_t * after = test.counters.values;
    check( after[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] == before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] + (uint64_t) ( pad_bytes + ROCKETNET_KEY_BYTES + 1 ) );
    check( after[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] == before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] + (uint64_t) ( pad_bytes + 1 ) );
    check( after[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] == before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] + (uint64_t) ROCKETNET_KEY_BYTES );
    check( after[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] == before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] );
    check( after[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED] == before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED] );

    // the unmodified request passes both rungs and is answered with a cookie

    request_checks_build_request( request, rocketnet_test_salt );
    check( request_checks_process( &test, request, ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] == before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] + 1 );

    request_checks_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_request_checks_wrong_salt_does_not_refresh_half_open );
REGISTER_UNIT_TEST( test_request_checks_two_rung_failure_counts_first_rung );
REGISTER_UNIT_TEST( test_request_checks_pad_and_server_key_compare_every_byte );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The two fixed-size server reply writers have impossible-size arms. These
// cases reach them through valid request and response packets, then use the
// private one-shot development seam to make the selected writer return zero.
// The fault is downstream of parsing and authentication. Each case proves the
// exact positive RECEIVED + INTERNAL_ERROR delta, the fail-closed result, and
// that no malformed output escaped. Time is passed in and no socket is used.

#include "rocketnet.h"

#if ROCKETNET_DEV

#include "test_unit_registry.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check
#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

struct internal_error_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void internal_error_server_setup( struct internal_error_server_t * test )
{
    memset( test, 0, sizeof(*test) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server->cookie_lifetime_seconds = 3;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 8 );
    server->client_slots = rocketnet_client_slots_create( NULL, 1 );
    server->acks = rocketnet_server_acks_create( NULL, 1 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 1, 1, 4096, 1 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots &&
           server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, 1000.0 );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void internal_error_server_teardown( struct internal_error_server_t * test )
{
    check( test->server.test_fail_fixed_writer == ROCKETNET_SERVER_TEST_FAIL_WRITER_NONE );
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

static void internal_error_build_request( uint8_t * request, uint64_t client_id )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, sizeof(server_address) );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = client_id;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields,
                                          rocketnet_test_backend_secret_key, token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );
    rocketnet_write_connection_request_packet( request, rocketnet_test_filter_value, NULL,
                                               rocketnet_test_salt, rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

static int internal_error_process( struct internal_error_server_t * test,
                                   const uint8_t * packet, int packet_bytes,
                                   uint8_t * response, int * response_bytes )
{
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 99;
    int payload_client_index = 99;
    const int result = rocketnet_server_protocol_process_packet( &test->server, &test->client_address,
                                                                  packet, packet_bytes,
                                                                  ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                  response, response_bytes,
                                                                  payload, &payload_bytes, &payload_client_index );
    check( payload_bytes == 0 );
    check( payload_client_index == -1 );
    return result;
}

static void internal_error_pass_cookie( struct internal_error_server_t * test, uint8_t * request )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    check( internal_error_process( test, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                   response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    memcpy( request + ROCKETNET_PREFILTER_BYTES,
            response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
            ROCKETNET_COOKIE_FIELD_BYTES );
}

static void internal_error_derive_keys( const uint8_t * challenge, int challenge_bytes,
                                        struct rocketnet_connection_keys_t * keys )
{
    check( rocketnet_prefilter_check( challenge, challenge_bytes, rocketnet_test_filter_value ) ==
           ROCKETNET_CONNECTION_CHALLENGE_PACKET );
    const uint8_t * p = challenge + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memcpy( session_nonce, challenge + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES,
            sizeof(session_nonce) );

    struct rocketnet_connection_keys_t handshake_keys;
    rocketnet_derive_handshake_keys( &handshake_keys,
                                     rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, session_nonce );
    uint8_t server_ephemeral[ROCKETNET_KEY_BYTES];
    check( rocketnet_open_sealed_packet( server_ephemeral, session_nonce, sizeof(server_ephemeral),
                                         challenge, challenge_bytes, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
                                         sequence, ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                         rocketnet_test_client_eph_public,
                                         handshake_keys.handshake_server_to_client ) == ROCKETNET_KEY_BYTES );
    check( rocketnet_derive_connection_keys( keys,
                                             rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                             rocketnet_test_salt, session_nonce,
                                             rocketnet_test_client_eph_secret, server_ephemeral,
                                             rocketnet_test_client_eph_public, server_ephemeral ) );
}

static void internal_error_build_response( uint8_t * response,
                                           const struct rocketnet_connection_keys_t * keys )
{
    const uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    check( rocketnet_write_sealed_packet( response, ROCKETNET_CONNECTION_RESPONSE_PACKET,
                                          rocketnet_test_filter_value, 1, NULL,
                                          padding, sizeof(padding), ROCKETNET_TEST_PROTOCOL_ID,
                                          rocketnet_test_salt, rocketnet_test_client_eph_public,
                                          keys->handshake_client_to_server ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
}

static void internal_error_fill_only_slot( struct internal_error_server_t * test )
{
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );
    check( rocketnet_client_slots_assign( test->server.client_slots, UINT64_C( 0xD00D ), -1, user_data ) == 0 );
}

static void internal_error_check_sentinel( const uint8_t * bytes, int count, uint8_t sentinel )
{
    for ( int i = 0; i < count; i++ )
        check( bytes[i] == sentinel );
}

#if ROCKETNET_ASSERTS
static int internal_error_assert_count;
static const char * internal_error_assert_function;
static rocketnet_assert_function_t internal_error_saved_assert_function;

static void internal_error_capture_assert( const char * condition, const char * function,
                                           const char * file, int line )
{
    (void) file;
    (void) line;
    check( strcmp( condition, "0" ) == 0 );
    check( strcmp( function, internal_error_assert_function ) == 0 );
    internal_error_assert_count++;
}
#endif // #if ROCKETNET_ASSERTS

static void internal_error_begin_assert_capture( const char * function )
{
#if ROCKETNET_ASSERTS
    internal_error_assert_count = 0;
    internal_error_assert_function = function;
    internal_error_saved_assert_function = rn_assert_function;
    rocketnet_assert_function( internal_error_capture_assert );
#else
    (void) function;
#endif // #if ROCKETNET_ASSERTS
}

static void internal_error_end_assert_capture()
{
#if ROCKETNET_ASSERTS
    rocketnet_assert_function( internal_error_saved_assert_function );
    check( internal_error_assert_count == 1 );
#endif // #if ROCKETNET_ASSERTS
}

static void test_connection_request_internal_error_counter_executes()
{
    struct internal_error_server_t test;
    internal_error_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    internal_error_build_request( request, ROCKETNET_TEST_CLIENT_ID );
    internal_error_pass_cookie( &test, request );
    internal_error_fill_only_slot( &test );

    struct rocketnet_counters_t before = test.counters;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    memset( response, 0xA5, sizeof(response) );
    int response_bytes = 99;
    test.server.test_fail_fixed_writer = ROCKETNET_SERVER_TEST_FAIL_REQUEST_DENIED_WRITER;
    internal_error_begin_assert_capture( "rocketnet_server_process_opened_request" );
    const int result = internal_error_process( &test, request, sizeof(request), response, &response_bytes );
    internal_error_end_assert_capture();

    check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( response_bytes == 0 );
    internal_error_check_sentinel( response, sizeof(response), 0xA5 );
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );
    rocketnet_counters_expect( &before, &test.counters,
                               { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR, 1 } } );

    internal_error_server_teardown( &test );
}

static void test_connection_response_internal_error_counter_executes()
{
    struct internal_error_server_t test;
    internal_error_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    internal_error_build_request( request, ROCKETNET_TEST_CLIENT_ID );
    internal_error_pass_cookie( &test, request );

    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int challenge_bytes = 0;
    check( internal_error_process( &test, request, sizeof(request), challenge, &challenge_bytes ) ==
           ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( challenge_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    struct rocketnet_connection_keys_t keys;
    internal_error_derive_keys( challenge, challenge_bytes, &keys );
    uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    internal_error_build_response( client_response, &keys );

    const int mapping_index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    check( mapping_index >= 0 );
    struct rocketnet_encryption_mapping_t * mapping =
        rocketnet_encryption_map_entry( test.server.encryption_map, mapping_index );
    const uint64_t sequence_before = mapping->send_sequence_handshake;
    internal_error_fill_only_slot( &test );

    struct rocketnet_counters_t before = test.counters;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    memset( response, 0x5A, sizeof(response) );
    int response_bytes = 99;
    test.server.test_fail_fixed_writer = ROCKETNET_SERVER_TEST_FAIL_RESPONSE_DENIED_WRITER;
    internal_error_begin_assert_capture( "rocketnet_server_process_connection_response" );
    const int result = internal_error_process( &test, client_response, sizeof(client_response),
                                               response, &response_bytes );
    internal_error_end_assert_capture();

    check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( response_bytes == 0 );
    internal_error_check_sentinel( response, sizeof(response), 0x5A );
    check( mapping->send_sequence_handshake == sequence_before );
    check( mapping->control->client_index == -1 );
    check( !mapping->control->established );
    rocketnet_counters_expect( &before, &test.counters,
                               { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_RESPONSE_INTERNAL_ERROR, 1 } } );

    internal_error_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_connection_request_internal_error_counter_executes );
REGISTER_UNIT_TEST( test_connection_response_internal_error_counter_executes );

#endif // #if ROCKETNET_DEV

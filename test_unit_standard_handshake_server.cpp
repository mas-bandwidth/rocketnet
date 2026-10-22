/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The vector twins of RN-1 t12 (standard/TRANSPORT.md, Handshake: server),
// regenerated through the implementation. No socket, no thread, no clock:
// every generator computes from the fixed inputs the section states, and the
// server decisions it exercises take time as an argument starting at 1000.0.

#include "test_unit_standard.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_keys.h"
#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

struct handshake_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void handshake_server_fill_token( struct rocketnet_signed_connect_data_t * signed_fields,
                                         struct rocketnet_connect_token_private_t * private_fields,
                                         uint64_t create_timestamp,
                                         uint64_t expire_timestamp )
{
    memset( signed_fields, 0, sizeof(*signed_fields) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = create_timestamp;
    signed_fields->expire_timestamp = expire_timestamp;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t advertised[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields->ipv4_address, advertised, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( private_fields, 0, sizeof(*private_fields) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = expire_timestamp;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );
}

static void handshake_server_build_request( uint8_t * request,
                                            const char * address_text,
                                            uint64_t create_timestamp,
                                            uint64_t expire_timestamp,
                                            int64_t cookie_expire_offset )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    handshake_server_fill_token( &signed_fields, &private_fields, create_timestamp, expire_timestamp );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields,
                                                        rocketnet_test_backend_secret_key,
                                                        rocketnet_test_token_vector_ephemeral_secret[0], token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    rocketnet_write_connection_request_packet( request, rocketnet_test_filter_value, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );

    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, address_text ) );

    const uint64_t cookie_expire = (uint64_t) ( (int64_t) ROCKETNET_TEST_CREATE_TS + cookie_expire_offset );
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    check( rocketnet_generate_cookie_mac( cookie_mac, &address, rocketnet_test_salt, cookie_expire, rocketnet_test_cookie_key ) );
    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
}

static void handshake_server_setup( struct handshake_server_t * test )
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
    server->handshake_deadline_seconds = 5;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots );

    rocketnet_server_protocol_update( server, 1000.0 );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void handshake_server_teardown( struct handshake_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
}

static int handshake_server_process( struct handshake_server_t * test, const uint8_t * request,
                                     uint8_t * response, int * response_bytes )
{
    *response_bytes = 0;
    return rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address,
                                                                 request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                 response, response_bytes );
}

static int handshake_server_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t12.cookie" ) == 0 )
    {
        struct handshake_server_t test;
        handshake_server_setup( &test );
        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        handshake_server_build_request( request, "198.51.100.7:56789",
                                        ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 3 );
        memset( request + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_COOKIE_FIELD_BYTES );
        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        check( handshake_server_process( &test, request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
        check( response_bytes <= capacity );
        memcpy( out, response, (size_t) response_bytes );
        handshake_server_teardown( &test );
        return response_bytes;
    }

    if ( strcmp( id, "t12.stale-cookie" ) == 0 )
    {
        struct handshake_server_t test;
        handshake_server_setup( &test );
        check( rocketnet_address_parse( &test.client_address, "203.0.113.44:40001" ) );
        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        handshake_server_build_request( request, "203.0.113.44:40001",
                                        ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, -100 );
        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        check( handshake_server_process( &test, request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
        check( response_bytes <= capacity );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED] == 1 );
        memcpy( out, response, (size_t) response_bytes );
        handshake_server_teardown( &test );
        return response_bytes;
    }

    if ( strcmp( id, "t12.challenge" ) == 0 )
    {
        struct rocketnet_connection_keys_t keys;
        rocketnet_derive_handshake_keys( &keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                         rocketnet_test_salt, rocketnet_test_session_nonce );
        int bytes = rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
                                                   rocketnet_test_filter_value, 1, rocketnet_test_session_nonce,
                                                   rocketnet_test_server_eph_public, ROCKETNET_KEY_BYTES,
                                                   ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                   rocketnet_test_client_eph_public, keys.handshake_server_to_client );
        check( bytes <= capacity );
        return bytes;
    }

    if ( strcmp( id, "t12.keep-alive" ) == 0 )
    {
        struct rocketnet_connection_keys_t keys;
        check( rocketnet_derive_connection_keys( &keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                                 rocketnet_test_salt, rocketnet_test_session_nonce,
                                                 rocketnet_test_server_eph_secret, rocketnet_test_client_eph_public,
                                                 rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) );

        struct rocketnet_replay_protection_t replay;
        memset( &replay, 0, sizeof(replay) );
        uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 8];
        const int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - 8;
        int block_bytes = rocketnet_ack_block_build( plaintext, block_capacity, &replay );
        uint8_t * p = plaintext + block_bytes;
        rocketnet_write_uint32( &p, 0 );
        rocketnet_write_uint32( &p, 4 );

        int bytes = rocketnet_write_sealed_packet( out, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                   rocketnet_test_filter_value, 1, NULL,
                                                   plaintext, block_bytes + 8,
                                                   ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                                   NULL, keys.session_server_to_client );
        check( bytes <= capacity );
        return bytes;
    }

    if ( strcmp( id, "t12.token-created-in-future" ) == 0 )
    {
        handshake_server_build_request( out, "198.51.100.7:56789",
                                        ROCKETNET_TEST_CREATE_TS + 6, ROCKETNET_TEST_CREATE_TS + 300, 3 );
        return ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
    }

    if ( strcmp( id, "t12.token-expired" ) == 0 )
    {
        handshake_server_build_request( out, "198.51.100.7:56789",
                                        ROCKETNET_TEST_CREATE_TS - 100, ROCKETNET_TEST_CREATE_TS - 50, 3 );
        return ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
    }

    return -1;
}

static const rn_standard_vector_t standard_handshake_server_table[] =
{
    RN_STANDARD_VECTOR( "t12.cookie", "0700b759010053414c543030303103f9ac68000000000fb2b28f32dbd6c9eb364f577486f3e9" ),
    RN_STANDARD_VECTOR( "t12.stale-cookie", "0700b759010053414c543030303103f9ac6800000000592e1f69fa21b0744b9209f79a3394fb" ),
    RN_STANDARD_VECTOR( "t12.challenge", "0200b7590100010000004e4f4e4345303031686fb709435ff00e1ac260eeeafbb29120201086c8276255d7d387c39232947b3da13ddd6856b09415f3086c59f76f88" ),
    RN_STANDARD_VECTOR( "t12.keep-alive", "0400b759010001000000d248d810b069788627a0513c6dbcea6d282c391bb9288efb4c5dba9c70724b" ),
    RN_STANDARD_VECTOR( "t12.token-created-in-future", "0000b759010003f9ac68000000000fb2b28f32dbd6c9eb364f577486f3e953414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24931a7c4e887766554433221106f9ac68000000002cfaac68000000000a00000001cb00710a409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801c34e42d4af5ef94a07a3a84201b889d4cd1a743cb27b11b6a10438a8feb8e5847d5da889783414a57bb78b01146db8f9a3f0cbdb60f45eecbe38ea0a72cec1b6b42e34afd6efb436d00345c404c4de3925786d259cc06b1239e259ca8eb137f9349191a211ca7f01704ae96ddd52c86e16934789b7c3a0774d088e03528a4afa2dac5aa04d241805930ca24ff62b9417c1eb7f36248717557ad340a1a2111eb1f6b5b71abab8f6044450e18005898b6886f8baa2de5bf461ee4f9b32638732e4f4d9ea83761dea2a173c2e74f1383bb450926b3cd3889ef3b4dd3d84fb85aad0a12e5a77be701402d5207057841e35889d99f824309bf6b60e2403c771653beac7b23680070faf4a9195e3169fb4dd83691b77e19bfa6510342216a0349391e2e9cc7c968b016637d9f8ae49c24528a357542aec01ef8324a946af6acd6156b9c177f06cf3b829bfce60ec009d5792d162e6d9b58852d11b4df6c623a1d393ae315c246ed696057f4f8d7b918af57431a7ec50cdd40aa72bfed55c0cf4aa8c9188bd94c0563fe96fecc32277abfcc8bfb18d6e3dbbeb2b58465f35711e62e91411c4d8076ed827432f33d0cf73ba881af00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000119eb3025cab63a665635d30d7735c6f889ee21ceb7d8c7e1e9f53f226e85fb492ed6bd343f85192b391d2feff819eea88f4026eba4bcfa8f01cb7a1abc47409" ),
    RN_STANDARD_VECTOR( "t12.token-expired", "0000b759010003f9ac68000000000fb2b28f32dbd6c9eb364f577486f3e953414c54303030313a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24931a7c4e88776655443322119cf8ac6800000000cef8ac68000000000a00000001cb00710a409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801c34e42d4af5ef94a07a3a84201b889d4cd1a743cb27b11b6a10438a8feb8e584708dfebb386fe9d3282325914c792005b3f0cbdb60f45eecbe38ea0a72cec1b6ba0e14afd6efb436d00345c404c4de3925786d259cc06b1239e259ca8eb137f9349191a211ca7f01704ae96ddd52c86e16934789b7c3a0774d088e03528a4afa2dac5aa04d241805930ca24ff62b9417c1eb7f36248717557ad340a1a2111eb1f6b5b71abab8f6044450e18005898b6886f8baa2de5bf461ee4f9b32638732e4f4d9ea83761dea2a173c2e74f1383bb450926b3cd3889ef3b4dd3d84fb85aad0a12e5a77be701402d5207057841e35889d99f824309bf6b60e2403c771653beac7b23680070faf4a9195e3169fb4dd83691b77e19bfa6510342216a0349391e2e9cc7c968b016637d9f8ae49c24528a357542aec01ef8324a946af6acd6156b9c177f06cf3b829bfce60ec009d5792d162e6d9b58852d11b4df6c623a1d393ae315c246ed696057f4f8d7b918af57431a7ec50cdd40aa72bfed55c0cf4aa8c9188bd94c0563fe96fecc32277abfcc8bfb18d6e3dbbeb2b58465f35711e62e91411c4d8076ed827432f33d0cf73ba881af00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000b289102eed08cc04faf63db4bcfe50bd70e8ef8980a3759f79f55d90ef7c76822564c401681b65ff3781c6cb4d2be96e69dadc39959dd67f85eaf8242b069d06" ),
};

static void standard_handshake_server_token_refusals()
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    {
        struct handshake_server_t test;
        handshake_server_setup( &test );
        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        handshake_server_build_request( request, "198.51.100.7:56789",
                                        ROCKETNET_TEST_CREATE_TS + 6, ROCKETNET_TEST_CREATE_TS + 300, 3 );
        check( handshake_server_process( &test, request, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE] == 1 );
        handshake_server_teardown( &test );
    }
}

static void test_standard_handshake_server_vectors()
{
    const int count = (int) ( sizeof( standard_handshake_server_table ) / sizeof( standard_handshake_server_table[0] ) );
    check( rn_standard_vectors_walk( standard_handshake_server_table, count, handshake_server_generate ) == count );

    standard_handshake_server_token_refusals();
}

REGISTER_UNIT_TEST( test_standard_handshake_server_vectors );

// Refusals after a token claim must not reserve a token with no mapping.
static void handshake_refusal_releases_claim( int refusal )
{
    struct handshake_server_t test;
    handshake_server_setup( &test );
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    handshake_server_build_request( request, "198.51.100.7:56789",
                                    ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 3 );
    const uint8_t * signed_data = request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                                  ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_connect_token_identifier( identifier, signed_data );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = { 0 };
    int counter = 0;
    int expected = ROCKETNET_SERVER_PROCESS_IGNORED;
    switch ( refusal )
    {
        case 0:
            memset( test.server.server_secret_key, 0, ROCKETNET_KEY_BYTES );
            counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK;
            break;
        case 1:
            check( rocketnet_client_slots_assign( test.server.client_slots, ROCKETNET_TEST_CLIENT_ID, -1, user_data ) >= 0 );
            counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED;
            break;
        case 2:
            for ( int i = 0; i < 4; ++i )
                check( rocketnet_client_slots_assign( test.server.client_slots, 100 + i, -1, user_data ) >= 0 );
            counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS;
            expected = ROCKETNET_SERVER_PROCESS_SEND_DENIED;
            break;
        default:
            memset( request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
                    0, ROCKETNET_KEY_BYTES );
            counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY;
            break;
    }
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    check( handshake_server_process( &test, request, response, &response_bytes ) == expected );
    check( test.counters.values[counter] == 1 );
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );
    struct rocketnet_address_t other;
    check( rocketnet_address_parse( &other, "198.51.100.8:56789" ) );
    check( rocketnet_used_token_history_claim_check( test.server.used_token_history, identifier, &other, 1000.0 ) );
    handshake_server_teardown( &test );
}

static void test_claim_released_after_private_open_refusal() { handshake_refusal_releases_claim( 0 ); }
static void test_claim_released_after_client_id_refusal() { handshake_refusal_releases_claim( 1 ); }
static void test_claim_released_after_full_server_refusal() { handshake_refusal_releases_claim( 2 ); }
static void test_claim_released_after_small_order_refusal() { handshake_refusal_releases_claim( 3 ); }
REGISTER_UNIT_TEST( test_claim_released_after_private_open_refusal );
REGISTER_UNIT_TEST( test_claim_released_after_client_id_refusal );
REGISTER_UNIT_TEST( test_claim_released_after_full_server_refusal );
REGISTER_UNIT_TEST( test_claim_released_after_small_order_refusal );

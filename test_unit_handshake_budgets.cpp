/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The two server handshake budgets STANDARD.md publishes: the post-cookie
// handshake work budget per update and the failed-signature budget per
// window. Both are local policy with defaults the standard states, and this
// pins the defaults and the refusal each one produces -- no answer, and the
// counter it fires. Time is always an argument; the test clock starts at
// 1000.0 and nothing here sleeps, opens a socket or reads a clock.

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
#include "rocketnet_crypto_tally.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

struct budgets_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void budgets_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                        struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof(*signed_fields) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof(*private_fields) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

static void budgets_server_setup( struct budgets_server_t * test )
{
    memset( test, 0, sizeof(*test) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->filter_value, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->max_token_lifetime_seconds = 0;     // the budget test mints its own create timestamps
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->allow_negative_timeout = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots );

    // the update resets the per-update handshake work budget, exactly as the
    // transport does at the top of every update
    rocketnet_server_protocol_update( server, 1000.0 );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void budgets_server_teardown( struct budgets_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
}

// builds a valid request around a fresh token, with a return cookie valid for
// this request's own source address, so the request reaches the post-cookie
// rungs rather than the cookie exchange.

static void budgets_build_request( uint8_t * request, int index,
                                   const struct rocketnet_address_t * address,
                                   const uint8_t * cookie_key )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    budgets_fill_vector_fields( &signed_fields, &private_fields );
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS - 1 - (uint64_t) index;
    private_fields.client_id = (uint64_t) index;

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

// one window, one source: the 91st failed signature is refused before the
// verify, unanswered and counted as the limit rather than as another failed
// verify.

static void budgets_check_signature_budget()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int budget = ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET;
    const int window_seconds = ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS;
    check( budget == 90 );
    check( window_seconds == 10 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_request( request, 0, &test.client_address, test.server.cookie_key );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    const int signature_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                                 ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES +
                                 ROCKETNET_SIGNED_CONNECT_DATA_BYTES;

    for ( int i = 0; i <= budget; i++ )
    {
        uint8_t mutated[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        memcpy( mutated, request, sizeof(mutated) );
        mutated[signature_offset] ^= 1;

        response_bytes = 0;
        int result = rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                    mutated, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                    ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                    response, &response_bytes );

        // a refused request is never answered, whether the verify failed or
        // the budget refused it before the verify
        check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
    }

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == (uint64_t) budget );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == 1 );

    budgets_server_teardown( &test );
}

// one window at time 1000.0: the post-cookie work budget admits 1024 requests
// and the 1025th is refused before any asymmetric work, unanswered and
// counted. The counter is the refusal's evidence, not the return code alone.

static void handshake_budgets_pinned()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int budget = ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE;
    check( budget == 1024 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    for ( int i = 0; i <= budget; i++ )
    {
        struct rocketnet_address_t address;
        char addr_str[64];
        snprintf( addr_str, sizeof(addr_str), "10.1.%d.%d:56789", ( i / 255 ) + 1, ( i % 255 ) + 1 );
        check( rocketnet_address_parse( &address, addr_str ) );
        budgets_build_request( request, i, &address, test.server.cookie_key );

        response_bytes = 0;
        int result = rocketnet_server_protocol_process_connection_request( &test.server, &address,
                                                    request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                    ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                    response, &response_bytes );
        if ( i < budget )
        {
            check( result != ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
        }
        else
        {
            // the 1025th request in one update: no answer, and the counter
            check( result == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
            check( response_bytes == 0 );
        }
    }

    check( test.server.counters->handshakes_processed_this_update == (uint64_t) budget );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 1 );

    budgets_server_teardown( &test );

    budgets_check_signature_budget();
}

REGISTER_UNIT_TEST( handshake_budgets_pinned );

// one connect token, many source addresses. The token is minted once and the
// same signed data and signature are presented from each address with that
// address's own valid return cookie, which is what an on-path observer or a
// token holder with many source ports can send. The first address is
// admitted to a half-open mapping; while that mapping lives, every other
// address is refused before the signature, the seal open and the keypair
// (sdk/rocketnet_server_protocol.cpp, the token claim rung; tla/ReceiveOrder.tla
// OneHalfOpenPerToken). A full encryption map refuses a fresh token before
// any of that work too (the capacity rung). Once the first mapping passes
// its handshake deadline and is swept, the token is admitted from another
// address.

static void one_token_build_request( uint8_t * request,
                                     const struct rocketnet_connect_token_t * token,
                                     const struct rocketnet_address_t * address,
                                     const uint8_t * cookie_key )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token->signed_connect_data, token->signature );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    check( rocketnet_generate_cookie_mac( cookie_mac, address, rocketnet_test_salt, cookie_expire, cookie_key ) );
    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
}

static uint64_t one_token_crypto_spent()
{
    return rocketnet_crypto_tally.signature_verify + rocketnet_crypto_tally.seal_open +
           rocketnet_crypto_tally.keypair + rocketnet_crypto_tally.scalarmult;
}

static void handshake_one_token_one_address()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int capacity = 8;     // budgets_server_setup's encryption map

    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    budgets_fill_vector_fields( &signed_fields, &private_fields );
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint64_t * counters = test.counters.values;

    // the first address: admitted, one half-open mapping, full crypto spent

    struct rocketnet_address_t first;
    check( rocketnet_address_parse( &first, "10.2.0.1:50000" ) );
    one_token_build_request( request, &token, &first, test.server.cookie_key );
    uint64_t spent_before = one_token_crypto_spent();
    int result = rocketnet_server_protocol_process_connection_request( &test.server, &first,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                response, &response_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( one_token_crypto_spent() > spent_before );
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );

    // the same token from every other source port: refused before any crypto,
    // unanswered, and no second mapping

    for ( int i = 2; i <= capacity + 1; i++ )
    {
        struct rocketnet_address_t address;
        char addr_str[64];
        snprintf( addr_str, sizeof(addr_str), "10.2.0.%d:%d", i, 50000 + i );
        check( rocketnet_address_parse( &address, addr_str ) );
        one_token_build_request( request, &token, &address, test.server.cookie_key );

        spent_before = one_token_crypto_spent();
        response_bytes = 0;
        result = rocketnet_server_protocol_process_connection_request( &test.server, &address,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                response, &response_bytes );
        check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
        check( one_token_crypto_spent() == spent_before );
    }

    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );
    check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] == (uint64_t) capacity );

    // other tokens fill the map; the next fresh token is refused before any
    // crypto, unanswered and counted as a full mapping table

    for ( int i = 1; i < capacity; i++ )
    {
        struct rocketnet_address_t address;
        char addr_str[64];
        snprintf( addr_str, sizeof(addr_str), "10.3.0.%d:50000", i );
        check( rocketnet_address_parse( &address, addr_str ) );
        budgets_build_request( request, i, &address, test.server.cookie_key );
        result = rocketnet_server_protocol_process_connection_request( &test.server, &address,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                response, &response_bytes );
        check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    }
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == capacity );

    {
        struct rocketnet_address_t address;
        check( rocketnet_address_parse( &address, "10.3.1.1:50000" ) );
        budgets_build_request( request, capacity, &address, test.server.cookie_key );
        spent_before = one_token_crypto_spent();
        response_bytes = 0;
        result = rocketnet_server_protocol_process_connection_request( &test.server, &address,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                response, &response_bytes );
        check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
        check( one_token_crypto_spent() == spent_before );
        check( counters[ROCKETNET_COUNTER_MAPPING_TABLE_FULL] == 1 );
        check( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING] == 1 );
    }

    // past the handshake deadline the sweep removes every half-open mapping
    // and the claim with it: the token is admitted from a second address

    rocketnet_server_protocol_update( &test.server, 1000.0 + test.server.handshake_deadline_seconds );
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );

    {
        struct rocketnet_address_t second;
        check( rocketnet_address_parse( &second, "10.2.0.2:50002" ) );
        one_token_build_request( request, &token, &second, test.server.cookie_key );
        result = rocketnet_server_protocol_process_connection_request( &test.server, &second,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0 + test.server.handshake_deadline_seconds,
                                                response, &response_bytes );
        check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );
    }

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( handshake_one_token_one_address );

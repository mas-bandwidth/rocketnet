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
#include "rocketnet_crypto_tally.h"
#include "rocketnet_test_vectors.h"

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

// mints one token; the request builder below can then replay the same signed
// data from many sources, each with its own return cookie.

static void budgets_mint_token( uint8_t * token_data, int index )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    budgets_fill_vector_fields( &signed_fields, &private_fields );
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS - 1 - (uint64_t) index;
    private_fields.client_id = (uint64_t) index;
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void budgets_build_request_from_token( uint8_t * request, const uint8_t * token_data,
                                              const struct rocketnet_address_t * address,
                                              const uint8_t * cookie_key )
{
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

static int budgets_crypto_unchanged( const struct rocketnet_crypto_tally_t * before )
{
    return rocketnet_crypto_tally.keypair == before->keypair &&
           rocketnet_crypto_tally.scalarmult == before->scalarmult &&
           rocketnet_crypto_tally.aead_seal == before->aead_seal &&
           rocketnet_crypto_tally.aead_open == before->aead_open &&
           rocketnet_crypto_tally.signature_verify == before->signature_verify &&
           rocketnet_crypto_tally.seal_open == before->seal_open;
}

// one token buys one half-open mapping: the first source address whose valid
// request is admitted owns the token until that mapping's handshake deadline.
// The same token from a second address is refused at the cheap claim rung with
// no crypto spent and no second mapping. With distinct tokens, a full map
// refuses at the capacity rung, also with no crypto spent. Time is the 1000.0
// argument and no socket, clock or thread is involved.

static void handshake_one_token_one_address()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    budgets_mint_token( token_data, 100 );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    uint8_t token_identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_connect_token_identifier( token_identifier, token.signed_connect_data );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    struct rocketnet_address_t first_address;
    check( rocketnet_address_parse( &first_address, "10.9.0.1:56789" ) );

    budgets_build_request_from_token( request, token_data, &first_address, test.server.cookie_key );

    struct rocketnet_crypto_tally_t before = rocketnet_crypto_tally;

    int result = rocketnet_server_protocol_process_connection_request( &test.server, &first_address,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                response, &response_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    // the first address paid for the full asymmetric handshake and owns the
    // token's only live half-open mapping
    check( rocketnet_crypto_tally.keypair == before.keypair + 1 );
    check( rocketnet_crypto_tally.signature_verify == before.signature_verify + 1 );
    check( rocketnet_crypto_tally.seal_open == before.seal_open + 1 );
    check( rocketnet_crypto_tally.scalarmult > before.scalarmult );
    check( rocketnet_crypto_tally.aead_seal > before.aead_seal );
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );

    // the same token from a second address: refused at the claim rung, no
    // crypto counted, no mapping, and no answer
    struct rocketnet_address_t second_address;
    check( rocketnet_address_parse( &second_address, "10.9.0.2:56789" ) );
    budgets_build_request_from_token( request, token_data, &second_address, test.server.cookie_key );

    before = rocketnet_crypto_tally;
    const uint64_t already_used_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED];

    result = rocketnet_server_protocol_process_connection_request( &test.server, &second_address,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                response, &response_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( response_bytes == 0 );
    check( budgets_crypto_unchanged( &before ) );
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );
    check( rocketnet_encryption_map_find( test.server.encryption_map, &second_address ) == -1 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] == already_used_before + 1 );

    // exactly one live mapping carries the token
    {
        int token_mappings = 0;
        int iterator = -1;
        while ( rocketnet_encryption_map_next( test.server.encryption_map, &iterator ) )
        {
            const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, iterator );
            if ( memcmp( mapping->token_identifier, token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 )
                token_mappings++;
        }
        check( token_mappings == 1 );
    }

    // distinct tokens from distinct addresses fill the map; each pays crypto
    const int capacity = 8;
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );
    for ( int i = 0; i < capacity - 1; i++ )
    {
        uint8_t fill_token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        budgets_mint_token( fill_token_data, i );

        struct rocketnet_address_t address;
        char address_text[64];
        snprintf( address_text, sizeof(address_text), "10.10.0.%d:56789", i + 1 );
        check( rocketnet_address_parse( &address, address_text ) );

        budgets_build_request_from_token( request, fill_token_data, &address, test.server.cookie_key );
        response_bytes = 0;
        check( rocketnet_server_protocol_process_connection_request( &test.server, &address,
                                            request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                            ROCKETNET_TEST_CREATE_TS, 1000.0,
                                            response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    }

    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == capacity );
    check( rocketnet_encryption_map_full( test.server.encryption_map ) );

    // a full map refuses at the capacity rung before any signature, seal or
    // keypair work, counted as a full-table refusal
    uint8_t overflow_token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    budgets_mint_token( overflow_token_data, capacity );

    struct rocketnet_address_t overflow_address;
    check( rocketnet_address_parse( &overflow_address, "10.10.1.1:56789" ) );
    budgets_build_request_from_token( request, overflow_token_data, &overflow_address, test.server.cookie_key );

    before = rocketnet_crypto_tally;
    const uint64_t mapping_full_before = test.counters.values[ROCKETNET_COUNTER_MAPPING_TABLE_FULL];
    const uint64_t could_not_add_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING];

    response_bytes = 0;
    result = rocketnet_server_protocol_process_connection_request( &test.server, &overflow_address,
                                                request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                response, &response_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( response_bytes == 0 );
    check( budgets_crypto_unchanged( &before ) );
    check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == capacity );
    check( test.counters.values[ROCKETNET_COUNTER_MAPPING_TABLE_FULL] == mapping_full_before + 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING] == could_not_add_before + 1 );

    budgets_server_teardown( &test );
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
REGISTER_UNIT_TEST( handshake_one_token_one_address );

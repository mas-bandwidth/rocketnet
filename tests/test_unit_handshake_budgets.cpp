/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The two server handshake budgets STANDARD.md publishes: the post-cookie
// handshake work budget per update (the server config's
// handshakes_per_update) and the failed-signature budget per window. Both
// are local policy with defaults the standard states, and this pins the
// defaults and the refusal each one produces -- no answer, and the counter
// it fires. Time is always an argument; the test clock starts at 1000.0 and
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
#include "rocketnet_server.h"
#include "rocketnet_keys.h"
#include "rocketnet_ack.h"

// Match the header to the sodium implementation linked into the SDK.
#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

struct budgets_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void budgets_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
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

static void budgets_server_setup( struct budgets_server_t * test )
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
    server->handshakes_per_update = ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    // room for a half-open mapping per request that reaches the verify: a
    // full table refuses before it, and only a verified request spends budget
    server->encryption_map = rocketnet_encryption_map_create( NULL, 64 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots &&
           server->acks );

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
    rocketnet_server_acks_destroy( test->server.acks );
}

// writes a return cookie valid for this source address and salt into a
// request, so one request's token can be presented from another address.
// Every token written is a distinct token: its sealed private block is
// randomized, so a replay of one token reuses the request's bytes.

static void budgets_write_cookie( uint8_t * request, const struct rocketnet_address_t * address,
                                  const uint8_t * cookie_key, const uint8_t * salt )
{
    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    check( rocketnet_generate_cookie_mac( cookie_mac, address, salt, cookie_expire, cookie_key ) );

    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
}

// builds a valid request around a fresh token, with a return cookie valid for
// this request's own source address and salt, so the request reaches the
// post-cookie rungs rather than the cookie exchange.

static void budgets_build_request_salted( uint8_t * request, int index,
                                          const struct rocketnet_address_t * address,
                                          const uint8_t * cookie_key,
                                          const uint8_t * salt )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    budgets_fill_vector_fields( &signed_fields, &private_fields );
    signed_fields.create_timestamp = (uint64_t) ROCKETNET_TEST_CREATE_TS - 1 - (uint64_t) index;
    private_fields.client_id = (uint64_t) index;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );

    budgets_write_cookie( request, address, cookie_key, salt );
}

static void budgets_build_request( uint8_t * request, int index,
                                   const struct rocketnet_address_t * address,
                                   const uint8_t * cookie_key )
{
    budgets_build_request_salted( request, index, address, cookie_key, rocketnet_test_salt );
}

// one window, one source: the 91st failed signature is refused before the
// verify, unanswered and counted as the limit rather than as another failed
// verify.

static void budgets_check_signature_budget()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    // the 91 requests arrive in one update: the work budget at its ceiling
    // keeps this case about the failed-signature budget alone
    test.server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;

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
        memcpy( mutated, request, sizeof( mutated ) );
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

// one update at time 1000.0: the post-cookie work budget admits its default
// of 16 requests and the 17th is refused before any asymmetric work,
// unanswered and counted. The counter is the refusal's evidence, not the
// return code alone. The configured range ends at the old fixed budget, 1024.

static void test_handshake_budgets_pinned()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int budget = test.server.handshakes_per_update;
    check( budget == 16 );
    check( ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE == 1024 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    for ( int i = 0; i <= budget; i++ )
    {
        struct rocketnet_address_t address;
        char addr_str[64];
        snprintf( addr_str, sizeof( addr_str ), "10.1.%d.%d:56789", ( i / 255 ) + 1, ( i % 255 ) + 1 );
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
            // the request past the budget in one update: no answer, and the counter
            check( result == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
            check( response_bytes == 0 );
        }
    }

    check( test.server.counters->handshakes_processed_this_update == (uint64_t) budget );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 1 );

    budgets_server_teardown( &test );

    budgets_check_signature_budget();
}

REGISTER_UNIT_TEST( test_handshake_budgets_pinned );

// the client's half of the handshake from wire bytes alone: opens the
// challenge, derives the connection keys and seals the response, so a test
// can drive one honest client from its request to connected.

static void budgets_client_response_from_challenge( const uint8_t * challenge, int challenge_bytes, uint8_t * response )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    check( rocketnet_prefilter_check( challenge, challenge_bytes, tag ) == ROCKETNET_CONNECTION_CHALLENGE_PACKET );

    const uint8_t * p = challenge + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );

    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memcpy( session_nonce, challenge + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    struct rocketnet_connection_keys_t handshake_keys;
    rocketnet_derive_handshake_keys( &handshake_keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     rocketnet_test_salt, session_nonce );

    uint8_t server_ephemeral[ROCKETNET_KEY_BYTES];
    check( rocketnet_open_sealed_packet( server_ephemeral, session_nonce, ROCKETNET_KEY_BYTES,
                                         challenge, challenge_bytes,
                                         ROCKETNET_CONNECTION_CHALLENGE_PACKET, sequence,
                                         ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                         rocketnet_test_client_eph_public,
                                         handshake_keys.handshake_server_to_client ) == ROCKETNET_KEY_BYTES );

    struct rocketnet_connection_keys_t keys;
    check( rocketnet_derive_connection_keys( &keys,
                                             rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                             rocketnet_test_salt, session_nonce,
                                             rocketnet_test_client_eph_secret, server_ephemeral,
                                             rocketnet_test_client_eph_public, server_ephemeral ) );

    const uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    check( rocketnet_write_sealed_packet( response, ROCKETNET_CONNECTION_RESPONSE_PACKET, tag, 1,
                                          NULL, padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                          ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt,
                                          rocketnet_test_client_eph_public,
                                          keys.handshake_client_to_server ) == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
}

static void budgets_forged_address( struct rocketnet_address_t * address, int i )
{
    char addr_str[64];
    snprintf( addr_str, sizeof( addr_str ), "10.2.%d.%d:56789", ( i / 250 ) + 1, ( i % 250 ) + 1 );
    check( rocketnet_address_parse( address, addr_str ) );
}

// one forged request from this address: a valid cookie for it, a signature
// that does not verify. returns the ladder's result; it is never answered.

static int budgets_send_forged( struct budgets_server_t * test, const struct rocketnet_address_t * address, int index )
{
    const int signature_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                                 ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES +
                                 ROCKETNET_SIGNED_CONNECT_DATA_BYTES;
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_request( request, index, address, test->server.cookie_key );
    request[signature_offset] ^= 1;

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    int result = rocketnet_server_protocol_process_connection_request( &test->server, address,
                                                                       request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                       ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                       response, &response_bytes );
    check( response_bytes == 0 );

    return result;
}

// The failed-signature budget is no global lockout (standard/TRANSPORT.md
// t17 item 11, proofs/tla/RNSecurityTables.tla HonestConnectsUnderFlood): 64 forging
// sources, each with a valid cookie for its own address, fill the source
// table and spend the window's budget. An honest client from a 65th address,
// with a valid token, is still verified and completes its handshake to
// connected. A source that has failed is still refused before the verify, and
// the verifies one update runs stay bounded by the handshake work budget.

static void test_handshake_failure_budget_no_global_lockout()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    // the forging sources and the honest client arrive in one update: the
    // work budget at its ceiling lets the failed-signature budget be spent
    // first; the flood below still checks the work budget bounds the verifies
    test.server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;

    const int budget = ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET;
    const int slots = ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS;
    check( budget == 90 );
    check( slots == 64 );

    // every slot of the table takes one forging source, then the first
    // sources fail again until the window's budget is spent

    int index = 0;

    for ( int i = 0; i < budget; i++ )
    {
        struct rocketnet_address_t address;
        budgets_forged_address( &address, i % slots );
        check( budgets_send_forged( &test, &address, ++index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    }

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == (uint64_t) budget );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == 0 );
    check( test.server.signature_verify_budget.failure_count == (uint32_t) budget );
    for ( int i = 0; i < slots; i++ )
    {
        check( test.server.signature_verify_budget.sources[i].fail_count > 0 );
    }

    // a source that has failed this window is refused before the verify
    {
        struct rocketnet_address_t address;
        budgets_forged_address( &address, 7 );
        check( budgets_send_forged( &test, &address, ++index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == 1 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == (uint64_t) budget );
    }

    // the honest client, from a 65th address with a valid token and a valid
    // cookie for that address: it has no recorded failure, so the full table
    // does not refuse it, and it gets the challenge

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_request( request, 0x70000, &test.client_address, test.server.cookie_key );

    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int challenge_bytes = 0;
    int result = rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                       request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                       ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                       challenge, &challenge_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( challenge_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == 1 );

    // and its response seats it: connected
    {
        uint8_t response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
        budgets_client_response_from_challenge( challenge, challenge_bytes, response );

        uint8_t out[ROCKETNET_RECEIVE_CEILING];
        int out_bytes = 0;
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;
        result = rocketnet_server_protocol_process_packet( &test.server, &test.client_address, response, sizeof( response ),
                                                           ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                           out, &out_bytes, payload, &payload_bytes, &payload_client_index );
        check( result == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
        check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );
    }

    // a flood of forging sources the full table cannot remember: in one
    // update no more of them reach the verify than the handshake work budget
    // admits, and the rest are refused before it
    {
        const uint64_t work_budget = (uint64_t) test.server.handshakes_per_update;
        const uint64_t processed = test.server.counters->handshakes_processed_this_update;
        const uint64_t failed_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY];
        const uint64_t exceeded_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED];
        const int flood = (int) work_budget + 16;
        for ( int i = 0; i < flood; i++ )
        {
            struct rocketnet_address_t address;
            budgets_forged_address( &address, slots + i );
            budgets_send_forged( &test, &address, ++index );
        }

        const uint64_t failed = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] - failed_before;
        const uint64_t exceeded = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] - exceeded_before;
        check( failed == work_budget - processed );
        check( test.server.counters->handshakes_processed_this_update == work_budget );
        check( exceeded == (uint64_t) flood - ( work_budget - processed ) );
    }

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_failure_budget_no_global_lockout );

// A source past the failed-signature budget is refused before the verify,
// and that refusal spends none of the update's handshake work budget: the
// budget bounds asymmetric work, so only a request that reaches the
// signature verify is charged (standard/TRANSPORT.md t12 rule 9;
// proofs/tla/ReceiveOrder.tla WorkBudgetOnlyForVerify). One unspoofed
// source with valid cookies spends the window's failures, then floods the
// next update with more forged requests; an honest client in that update
// is still challenged, not refused for budget.

static void test_handshake_refused_source_spends_no_work_budget()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    struct rocketnet_address_t forger;
    check( rocketnet_address_parse( &forger, "10.4.0.1:56789" ) );

    // the window's failures, all from one source, in one wide update

    test.server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;

    int index = 0;
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET; i++ )
        check( budgets_send_forged( &test, &forger, ++index ) == ROCKETNET_SERVER_PROCESS_IGNORED );

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET );

    // the next update, at the default budget: the forger floods past it, and
    // every request is refused by the failed-signature budget before the
    // verify, none of them charged

    test.server.handshakes_per_update = ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE;
    rocketnet_server_protocol_update( &test.server, 1000.1 );

    const int flood = 4 * ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE;
    for ( int i = 0; i < flood; i++ )
        check( budgets_send_forged( &test, &forger, ++index ) == ROCKETNET_SERVER_PROCESS_IGNORED );

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == (uint64_t) flood );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 0 );
    check( test.server.counters->handshakes_processed_this_update == 0 );

    // the honest client in the same update: verified and challenged

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_request( request, 0x40000, &test.client_address, test.server.cookie_key );

    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int challenge_bytes = 0;
    const int result = rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                             request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                             ROCKETNET_TEST_CREATE_TS, 1000.1,
                                                                             challenge, &challenge_bytes );
    check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( challenge_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    check( test.server.counters->handshakes_processed_this_update == 1 );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_refused_source_spends_no_work_budget );

// The refusals past the verify that the sender makes for itself charge the
// failed-signature budget as a failed signature does (standard/TRANSPORT.md
// t17 item 12; proofs/tla/RNSecurityTables.tla ChargedRefusal): a sealed
// block that does not open and a small-order client key. Such a request
// carries a token whose signature verifies, so without the charge one source
// with one valid token spends the work budget, a verify, a sealed-box open and
// a key exchange on every request of every update.

#define BUDGETS_REFUSE_SMALL_ORDER_KEY 0
#define BUDGETS_REFUSE_SEALED_BLOCK    1

static void budgets_build_refused_request( uint8_t * request, int index, const struct rocketnet_address_t * address,
                                           const uint8_t * cookie_key, int refusal )
{
    budgets_build_request( request, index, address, cookie_key );

    const int key_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES;
    uint8_t * signed_data = request + key_offset + ROCKETNET_KEY_BYTES;

    if ( refusal == BUDGETS_REFUSE_SMALL_ORDER_KEY )
    {
        // the client's ephemeral key is outside the signed data: zero is a
        // small-order point, refused at the key exchange
        memset( request + key_offset, 0, ROCKETNET_KEY_BYTES );
        return;
    }

    // a sealed block that does not open, under a signature that verifies
    struct rocketnet_signed_connect_data_t fields;
    rocketnet_read_signed_connect_data( &fields, signed_data );
    fields.sealed_private_block[0] ^= 1;
    rocketnet_write_signed_connect_data( &fields, signed_data );
    check( crypto_sign_detached( signed_data + ROCKETNET_SIGNED_CONNECT_DATA_BYTES, NULL, signed_data,
                                 ROCKETNET_SIGNED_CONNECT_DATA_BYTES, rocketnet_test_backend_secret_key ) == 0 );
}

static int budgets_send( struct budgets_server_t * test, const struct rocketnet_address_t * address,
                         const uint8_t * request, uint64_t timestamp, double time, int * response_bytes )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    *response_bytes = 0;
    return rocketnet_server_protocol_process_connection_request( &test->server, address, request,
                                                                 ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 timestamp, time, response, response_bytes );
}

static void budgets_charged_refusal_limits_its_source( int refusal )
{
    struct budgets_server_t test;
    budgets_server_setup( &test );
    test.server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;

    const int reason = refusal == BUDGETS_REFUSE_SMALL_ORDER_KEY ? ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY
                                                                 : ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK;
    const uint64_t budget = (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET;
    const int sent = 100;

    struct rocketnet_address_t source;
    check( rocketnet_address_parse( &source, "10.6.0.1:56789" ) );
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_refused_request( request, 1, &source, test.server.cookie_key, refusal );

    // one window, one update: the source is verified until the window's
    // budget is spent, then refused before the verify and the work budget

    for ( int i = 0; i < sent; i++ )
    {
        int response_bytes = 0;
        check( budgets_send( &test, &source, request, ROCKETNET_TEST_CREATE_TS, 1000.0, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( response_bytes == 0 );
    }

    check( test.counters.values[reason] == budget );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == (uint64_t) sent - budget );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == 0 );
    check( test.server.counters->handshakes_processed_this_update == budget );

    // an honest source in the same update is still verified and challenged
    {
        uint8_t honest[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        budgets_build_request( honest, 2, &test.client_address, test.server.cookie_key );
        int response_bytes = 0;
        check( budgets_send( &test, &test.client_address, honest, ROCKETNET_TEST_CREATE_TS, 1000.0, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
        check( test.server.counters->handshakes_processed_this_update == budget + 1 );
    }

    // once the window rolls over the source is verified again
    {
        const uint64_t later = ROCKETNET_TEST_CREATE_TS + (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS;
        // a cookie current at that time, for the same address and salt
        uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
        check( rocketnet_generate_cookie_mac( cookie_mac, &source, rocketnet_test_salt, later + 3, test.server.cookie_key ) );
        uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
        rocketnet_write_uint64( &cookie, later + 3 );
        memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

        rocketnet_server_protocol_update( &test.server, 1010.0 );
        int response_bytes = 0;
        check( budgets_send( &test, &source, request, later, 1010.0, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        check( test.counters.values[reason] == budget + 1 );
        check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == (uint64_t) sent - budget );
        check( test.server.counters->handshakes_processed_this_update == 1 );
    }

    budgets_server_teardown( &test );
}

static void test_handshake_small_order_key_charges_failure_budget() { budgets_charged_refusal_limits_its_source( BUDGETS_REFUSE_SMALL_ORDER_KEY ); }
static void test_handshake_unopened_sealed_block_charges_failure_budget() { budgets_charged_refusal_limits_its_source( BUDGETS_REFUSE_SEALED_BLOCK ); }
REGISTER_UNIT_TEST( test_handshake_small_order_key_charges_failure_budget );
REGISTER_UNIT_TEST( test_handshake_unopened_sealed_block_charges_failure_budget );

// The refusals past the verify that the sender does not choose charge
// nothing: a client id already connected, and a full server's denial. Sent
// 100 times in one window from one source, neither is ever limited.

static void budgets_uncharged_refusal_never_limits( int full_server )
{
    struct budgets_server_t test;
    budgets_server_setup( &test );
    test.server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = { 0 };
    if ( full_server )
    {
        for ( int i = 0; i < 4; ++i )
            check( rocketnet_client_slots_assign( test.server.client_slots, (uint64_t) ( 100 + i ), -1, (uint64_t) ( 100 + i ), user_data ) >= 0 );
    }
    else
    {
        check( rocketnet_client_slots_assign( test.server.client_slots, 1, -1, 1, user_data ) >= 0 );
    }

    struct rocketnet_address_t source;
    check( rocketnet_address_parse( &source, "10.6.0.2:56789" ) );
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_request( request, 1, &source, test.server.cookie_key );

    const int sent = 100;
    for ( int i = 0; i < sent; i++ )
    {
        int response_bytes = 0;
        const int result = budgets_send( &test, &source, request, ROCKETNET_TEST_CREATE_TS, 1000.0, &response_bytes );
        check( result == ( full_server ? ROCKETNET_SERVER_PROCESS_SEND_DENIED : ROCKETNET_SERVER_PROCESS_IGNORED ) );
        check( response_bytes == ( full_server ? ROCKETNET_CONNECTION_DENIED_WIRE_BYTES : 0 ) );
    }

    const int reason = full_server ? ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS
                                   : ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED;
    check( test.counters.values[reason] == (uint64_t) sent );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT] == 0 );
    check( test.server.signature_verify_budget.failure_count == 0 );

    budgets_server_teardown( &test );
}

static void test_handshake_client_id_connected_never_limits() { budgets_uncharged_refusal_never_limits( 0 ); }
static void test_handshake_full_server_denial_never_limits() { budgets_uncharged_refusal_never_limits( 1 ); }
REGISTER_UNIT_TEST( test_handshake_client_id_connected_never_limits );
REGISTER_UNIT_TEST( test_handshake_full_server_denial_never_limits );

// The four other cheap refusals between the cookie and the verify spend none
// of the handshake work budget either (standard/TRANSPORT.md t12 rule 9;
// proofs/tla/ReceiveOrder.tla WorkBudgetOnlyForVerify): a token bound to
// another address, a token claimed by another address, a full mapping table
// and a full claim table. Each case stages its refusal in one update, then
// floods the next update past the budget with requests that hit it, each with
// a valid cookie and a valid signature, and an honest request in that same
// update is still verified and challenged.

// one request from this address, at this time. returns the ladder's result;
// a refused request is never answered.

static int budgets_send_request( struct budgets_server_t * test, const struct rocketnet_address_t * address,
                                 const uint8_t * request, double current_time,
                                 uint8_t * response, int * response_bytes )
{
    *response_bytes = 0;
    const int result = rocketnet_server_protocol_process_connection_request( &test->server, address,
                                                                             request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                             ROCKETNET_TEST_CREATE_TS, current_time,
                                                                             response, response_bytes );
    if ( result != ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
        check( *response_bytes == 0 );

    return result;
}

// a fresh token's request from this address, verified and challenged; the
// request's bytes are left in request for a replay of its token

static void budgets_challenge( struct budgets_server_t * test, const struct rocketnet_address_t * address,
                               int index, const uint8_t * salt, double current_time,
                               uint8_t * request, uint8_t * challenge, int * challenge_bytes )
{
    budgets_build_request_salted( request, index, address, test->server.cookie_key, salt );
    check( budgets_send_request( test, address, request, current_time, challenge, challenge_bytes ) ==
           ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    check( *challenge_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
}

// the client's response to its challenge, which seats it: connected, the
// token bound to its address and its claim released

static void budgets_connect( struct budgets_server_t * test, const struct rocketnet_address_t * address,
                             const uint8_t * challenge, int challenge_bytes, double current_time )
{
    uint8_t response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    budgets_client_response_from_challenge( challenge, challenge_bytes, response );

    uint8_t out[ROCKETNET_RECEIVE_CEILING];
    int out_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    check( rocketnet_server_protocol_process_packet( &test->server, address, response, sizeof( response ),
                                                     ROCKETNET_TEST_CREATE_TS, current_time,
                                                     out, &out_bytes, payload, &payload_bytes, &payload_client_index ) ==
           ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
}

static void budgets_honest_address( struct rocketnet_address_t * address, int i )
{
    char addr_str[64];
    snprintf( addr_str, sizeof( addr_str ), "10.3.0.%d:56789", i + 1 );
    check( rocketnet_address_parse( address, addr_str ) );
}

// the flood: four budgets' worth of requests from distinct addresses, each
// refused by the staged rung, its counter firing once per request, and none
// of them charged to the update's handshake work budget

// replay is a request whose token every flood request presents, or NULL for
// a fresh token in each

static void budgets_flood_spends_nothing( struct budgets_server_t * test, const uint8_t * replay,
                                          int counter, double current_time )
{
    const uint64_t counter_before = test->counters.values[counter];
    const uint64_t verified_before = test->counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY];

    const int flood = 4 * test->server.handshakes_per_update;
    for ( int i = 0; i < flood; i++ )
    {
        struct rocketnet_address_t address;
        budgets_forged_address( &address, i );
        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        if ( replay )
        {
            memcpy( request, replay, sizeof( request ) );
            budgets_write_cookie( request, &address, test->server.cookie_key, rocketnet_test_salt );
        }
        else
        {
            budgets_build_request( request, 0x60000 + i, &address, test->server.cookie_key );
        }

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        check( budgets_send_request( test, &address, request, current_time, response, &response_bytes ) ==
               ROCKETNET_SERVER_PROCESS_IGNORED );
    }

    check( test->counters.values[counter] - counter_before == (uint64_t) flood );
    check( test->counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == verified_before );
    check( test->counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 0 );
    check( test->server.counters->handshakes_processed_this_update == 0 );
}

// the honest client in the flood's update: a fresh token from a new address,
// verified (charged one unit of the budget) and challenged

static void budgets_honest_challenged( struct budgets_server_t * test, const struct rocketnet_address_t * address,
                                       int index, const uint8_t * salt, double current_time )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int challenge_bytes = 0;
    budgets_challenge( test, address, index, salt, current_time, request, challenge, &challenge_bytes );
    check( test->server.counters->handshakes_processed_this_update == 1 );
}

// a token bound to another address: one client connects with it, then a
// flood replays it from other addresses

static void test_handshake_used_token_spends_no_work_budget()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int token = 0x50000;

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int challenge_bytes = 0;
    budgets_challenge( &test, &test.client_address, token, rocketnet_test_salt, 1000.0, request, challenge, &challenge_bytes );
    budgets_connect( &test, &test.client_address, challenge, challenge_bytes, 1000.0 );
    check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );

    rocketnet_server_protocol_update( &test.server, 1000.1 );

    budgets_flood_spends_nothing( &test, request, ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED, 1000.1 );

    struct rocketnet_address_t honest;
    budgets_honest_address( &honest, 0 );
    budgets_honest_challenged( &test, &honest, token + 1, rocketnet_test_salt, 1000.1 );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_used_token_spends_no_work_budget );

// a token claimed by another address: one client's request verifies and holds
// the claim with its half-open mapping, then a flood presents the same token
// from other addresses before the handshake deadline

static void test_handshake_claimed_token_spends_no_work_budget()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int token = 0x50000;

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    int challenge_bytes = 0;
    budgets_challenge( &test, &test.client_address, token, rocketnet_test_salt, 1000.0, request, challenge, &challenge_bytes );

    rocketnet_server_protocol_update( &test.server, 1000.1 );

    budgets_flood_spends_nothing( &test, request, ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED, 1000.1 );

    struct rocketnet_address_t honest;
    budgets_honest_address( &honest, 0 );
    budgets_honest_challenged( &test, &honest, token + 1, rocketnet_test_salt, 1000.1 );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_claimed_token_spends_no_work_budget );

// a full mapping table: four half-open handshakes fill a table of four, then
// a flood of fresh tokens from new addresses finds no room. The honest
// request is a client reconnecting from its own half-open address with a new
// salt and a new token: a replacement reuses its address's mapping, so it
// needs no free one.

static void test_handshake_map_full_spends_no_work_budget()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int capacity = 4;
    rocketnet_encryption_map_destroy( test.server.encryption_map );
    test.server.encryption_map = rocketnet_encryption_map_create( NULL, capacity );
    check( test.server.encryption_map );

    for ( int i = 0; i < capacity; i++ )
    {
        struct rocketnet_address_t address;
        budgets_honest_address( &address, i );
        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
        int challenge_bytes = 0;
        budgets_challenge( &test, &address, 0x50000 + i, rocketnet_test_salt, 1000.0, request, challenge, &challenge_bytes );
    }
    check( rocketnet_encryption_map_full( test.server.encryption_map ) );

    rocketnet_server_protocol_update( &test.server, 1000.1 );

    budgets_flood_spends_nothing( &test, NULL, ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING, 1000.1 );

    uint8_t salt[sizeof( rocketnet_test_salt )];
    memcpy( salt, rocketnet_test_salt, sizeof( salt ) );
    salt[0] ^= 1;

    struct rocketnet_address_t honest;
    budgets_honest_address( &honest, 0 );
    budgets_honest_challenged( &test, &honest, 0x50000 + capacity, salt, 1000.1 );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_map_full_spends_no_work_budget );

// a full claim table, the mapping table with room: four half-open handshakes
// hold the four claims, then a flood of fresh tokens from new addresses finds
// no claim to take. In the same update one of the four completes its
// handshake, which binds its token and frees its claim, and a new honest
// client takes it.

static void test_handshake_claim_table_full_spends_no_work_budget()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int capacity = 4;
    rocketnet_used_token_history_destroy( test.server.used_token_history );
    test.server.used_token_history = rocketnet_used_token_history_create( NULL, capacity );
    check( test.server.used_token_history );

    uint8_t first_challenge[ROCKETNET_RECEIVE_CEILING];
    int first_challenge_bytes = 0;
    struct rocketnet_address_t first;
    budgets_honest_address( &first, 0 );

    for ( int i = 0; i < capacity; i++ )
    {
        struct rocketnet_address_t address;
        budgets_honest_address( &address, i );
        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
        int challenge_bytes = 0;
        budgets_challenge( &test, &address, 0x50000 + i, rocketnet_test_salt, 1000.0, request, challenge, &challenge_bytes );
        if ( i == 0 )
        {
            memcpy( first_challenge, challenge, (size_t) challenge_bytes );
            first_challenge_bytes = challenge_bytes;
        }
    }
    check( !rocketnet_encryption_map_full( test.server.encryption_map ) );

    rocketnet_server_protocol_update( &test.server, 1000.1 );

    budgets_flood_spends_nothing( &test, NULL, ROCKETNET_COUNTER_CONNECTION_REQUEST_CLAIM_TABLE_FULL, 1000.1 );

    budgets_connect( &test, &first, first_challenge, first_challenge_bytes, 1000.1 );
    check( rocketnet_client_slots_num_connected( test.server.client_slots ) == 1 );

    struct rocketnet_address_t honest;
    budgets_honest_address( &honest, capacity );
    budgets_honest_challenged( &test, &honest, 0x50000 + capacity, rocketnet_test_salt, 1000.1 );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_claim_table_full_spends_no_work_budget );

// A burst over the cap: 40 honest clients, each with a valid cookie for its
// own address, send a request every update until they are challenged, as a
// client resends at its send rate. Each update processes at most the
// configured budget of them, the rest are dropped and counted, and the burst
// is spread across ceil( 40 / budget ) updates with every client challenged
// exactly once (proofs/tla/RNSecurityTables.tla HonestRequest). A client already
// challenged is answered from its mapping, before the budget, and spends
// none of it.

static void test_handshake_burst_spreads_across_updates()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int budget = 4;
    const int clients = 40;
    test.server.handshakes_per_update = budget;

    // room for every client's half-open mapping and request claim
    rocketnet_encryption_map_destroy( test.server.encryption_map );
    rocketnet_used_token_history_destroy( test.server.used_token_history );
    test.server.encryption_map = rocketnet_encryption_map_create( NULL, 2 * clients );
    test.server.used_token_history = rocketnet_used_token_history_create( NULL, 4 * clients );
    check( test.server.encryption_map && test.server.used_token_history );

    bool challenged[clients] = {};
    int num_challenged = 0;
    double time = 1000.0;
    int update = 0;

    while ( num_challenged < clients )
    {
        check( update < clients );
        time += 0.1;
        rocketnet_server_protocol_update( &test.server, time );
        update++;

        int admitted = 0;
        for ( int i = 0; i < clients; i++ )
        {
            struct rocketnet_address_t address;
            char addr_str[64];
            snprintf( addr_str, sizeof( addr_str ), "10.3.0.%d:56789", i + 1 );
            check( rocketnet_address_parse( &address, addr_str ) );
            uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
            budgets_build_request( request, 0x30000 + i, &address, test.server.cookie_key );

            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0;
            const int result = rocketnet_server_protocol_process_connection_request( &test.server, &address,
                                                                                     request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                                     ROCKETNET_TEST_CREATE_TS, time,
                                                                                     response, &response_bytes );
            if ( result == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED )
            {
                check( !challenged[i] );
                check( response_bytes == 0 );
                continue;
            }
            check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
            check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
            if ( !challenged[i] )
            {
                challenged[i] = true;
                num_challenged++;
                admitted++;
            }
        }

        // the budget's worth each update until the last, which takes the rest
        check( admitted == ( num_challenged < clients ? budget : clients - ( update - 1 ) * budget ) );
        check( test.server.counters->handshakes_processed_this_update == (uint64_t) admitted );
    }

    check( update == ( clients + budget - 1 ) / budget );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE] == (uint64_t) clients );

    // dropped: every not-yet-challenged client past the budget, each update
    uint64_t dropped = 0;
    for ( int u = 0; u < update; u++ )
    {
        const int remaining = clients - u * budget;
        dropped += (uint64_t) ( remaining - ( remaining < budget ? remaining : budget ) );
    }
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == dropped );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_burst_spreads_across_updates );

// The cookie reply limits (standard/TRANSPORT.md t12 rule 7;
// proofs/tla/RNCookieReplyLimits.tla): a request that is about to be answered
// with a fresh cookie is refused, unanswered and counted as
// CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED, when its source address and
// port has already had a cookie reply this update, or when the server has
// already sent max_clients cookie replies this update. Neither limit touches
// the handshake work budget, and the work budget touches neither.

// a first-contact request from this address: a fresh token and an all-zero
// return cookie. returns the ladder's result; only a cookie is answered.

static int budgets_send_cookieless( struct budgets_server_t * test, const struct rocketnet_address_t * address,
                                    int index, double current_time )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_request( request, index, address, test->server.cookie_key );
    memset( request + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_COOKIE_FIELD_BYTES );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int result = rocketnet_server_protocol_process_connection_request( &test->server, address,
                                                                             request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                             ROCKETNET_TEST_CREATE_TS, current_time,
                                                                             response, &response_bytes );
    if ( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
        check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    else
        check( response_bytes == 0 );

    return result;
}

// one source address and port: five first-contact requests in one update earn
// one cookie, and the other four are dropped and counted; the next update
// answers it again. Two ports on one IP, as behind a NAT, are two sources.

static void test_handshake_cookie_reply_once_per_source_per_update()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    int replies = 0;
    for ( int i = 0; i < 5; i++ )
    {
        const int result = budgets_send_cookieless( &test, &test.client_address, 0x90000 + i, 1000.0 );
        if ( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
            replies++;
        else
            check( result == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    }

    check( replies == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] == 1 );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 4 );
    check( test.server.counters->handshakes_processed_this_update == 0 );

    rocketnet_server_protocol_update( &test.server, 1000.1 );

    check( budgets_send_cookieless( &test, &test.client_address, 0x90010, 1000.1 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    struct rocketnet_address_t first_port;
    struct rocketnet_address_t second_port;
    check( rocketnet_address_parse( &first_port, "198.51.100.9:50001" ) );
    check( rocketnet_address_parse( &second_port, "198.51.100.9:50002" ) );
    check( budgets_send_cookieless( &test, &first_port, 0x90011, 1000.1 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( budgets_send_cookieless( &test, &second_port, 0x90012, 1000.1 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 4 );
    rocketnet_counters_assert_conserved( &test.counters );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_cookie_reply_once_per_source_per_update );

// max_clients + 5 distinct sources in one update: max_clients cookies, five
// dropped and counted. A request with a valid cookie in the same update is
// still verified and challenged, the flood having spent none of the work
// budget, and the next update answers new sources again.

static void test_handshake_cookie_replies_capped_at_max_clients()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );

    const int max_clients = rocketnet_client_slots_max_clients( test.server.client_slots );
    check( max_clients == 4 );

    int replies = 0;
    for ( int i = 0; i < max_clients + 5; i++ )
    {
        struct rocketnet_address_t address;
        char addr_str[64];
        snprintf( addr_str, sizeof( addr_str ), "10.5.0.%d:56789", i + 1 );
        check( rocketnet_address_parse( &address, addr_str ) );
        const int result = budgets_send_cookieless( &test, &address, 0xA0000 + i, 1000.0 );
        if ( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
            replies++;
        else
            check( result == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    }

    check( replies == max_clients );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 5 );
    check( test.server.counters->handshakes_processed_this_update == 0 );

    budgets_honest_challenged( &test, &test.client_address, 0xA0100, rocketnet_test_salt, 1000.0 );

    rocketnet_server_protocol_update( &test.server, 1000.1 );

    struct rocketnet_address_t late;
    check( rocketnet_address_parse( &late, "10.5.1.1:56789" ) );
    check( budgets_send_cookieless( &test, &late, 0xA0200, 1000.1 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 5 );
    rocketnet_counters_assert_conserved( &test.counters );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_cookie_replies_capped_at_max_clients );

// the reverse: a spent handshake work budget refuses the next cookie-valid
// request, and a first-contact request in the same update is still answered
// with its cookie

static void test_handshake_spent_work_budget_still_sends_cookies()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );
    test.server.handshakes_per_update = 1;

    budgets_honest_challenged( &test, &test.client_address, 0xB0000, rocketnet_test_salt, 1000.0 );

    struct rocketnet_address_t over;
    budgets_honest_address( &over, 1 );
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    budgets_build_request( request, 0xB0001, &over, test.server.cookie_key );
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    check( budgets_send_request( &test, &over, request, 1000.0, response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );

    struct rocketnet_address_t first_contact;
    budgets_honest_address( &first_contact, 2 );
    check( budgets_send_cookieless( &test, &first_contact, 0xB0002, 1000.0 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );

    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 1 );
    rocketnet_counters_assert_conserved( &test.counters );

    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_spent_work_budget_still_sends_cookies );

// two sources whose addresses share a bit under a fixed key: the first one in
// an update gets the cookie and the second is refused for that update only,
// answered in the next update whichever arrives first. The bit is keyed:
// under another key the same pair does not collide.

static void test_handshake_cookie_reply_collision_refuses_one_update()
{
    struct budgets_server_t test;
    budgets_server_setup( &test );
    for ( int i = 0; i < ROCKETNET_SERVER_COOKIE_REPLY_KEY_BYTES; i++ )
        test.server.cookie_reply_source_key[i] = (uint8_t) ( 0x51 + i );

    struct rocketnet_address_t first;
    check( rocketnet_address_parse( &first, "192.0.2.1:40001" ) );
    const int first_index = rocketnet_server_protocol_cookie_reply_source_index( &test.server, &first );
    check( first_index >= 0 && first_index < ROCKETNET_SERVER_COOKIE_REPLY_SOURCE_BITS );

    struct rocketnet_address_t second;
    bool found = false;
    for ( int port = 1; port <= 65535 && !found; port++ )
    {
        char addr_str[64];
        snprintf( addr_str, sizeof( addr_str ), "192.0.2.2:%d", port );
        check( rocketnet_address_parse( &second, addr_str ) );
        found = rocketnet_server_protocol_cookie_reply_source_index( &test.server, &second ) == first_index;
    }
    check( found );

    check( budgets_send_cookieless( &test, &first, 0xC0000, 1000.0 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( budgets_send_cookieless( &test, &second, 0xC0001, 1000.0 ) == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );

    rocketnet_server_protocol_update( &test.server, 1000.1 );

    check( budgets_send_cookieless( &test, &second, 0xC0002, 1000.1 ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( budgets_send_cookieless( &test, &first, 0xC0003, 1000.1 ) == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 2 );

    test.server.cookie_reply_source_key[0] ^= 1;
    check( rocketnet_server_protocol_cookie_reply_source_index( &test.server, &first ) !=
           rocketnet_server_protocol_cookie_reply_source_index( &test.server, &second ) );

    rocketnet_counters_assert_conserved( &test.counters );
    budgets_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_handshake_cookie_reply_collision_refuses_one_update );

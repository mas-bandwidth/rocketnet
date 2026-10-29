/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Unit tests for server cookie key rotation (proofs/tla/RNSecurityTables.tla):
// the current key, the one-period previous-key acceptance window, the stall
// branch past a full window, and key freshness across rotations.

#include "test_unit_registry.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

static const double ROCKETNET_ROTATION_TEST_START_TIME = 100.0;
static const int ROCKETNET_ROTATION_TEST_PERIOD_SECONDS = 60;

struct rocketnet_rotation_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void rocketnet_rotation_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                                   struct rocketnet_connect_token_private_t * private_fields,
                                                   uint64_t client_id )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof( *private_fields ) );
    private_fields->client_id = client_id;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

static void rocketnet_rotation_server_setup( struct rocketnet_rotation_server_t * test, int period_seconds )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE;
    server->cookie_rotation_seconds = period_seconds;
    server->cookie_key_rotation_time = ROCKETNET_ROTATION_TEST_START_TIME + (double) period_seconds;
    server->previous_cookie_key_expire_time = 0.0;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &test->counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history &&
                          server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );

    rocketnet_server_protocol_update( server, ROCKETNET_ROTATION_TEST_START_TIME );

    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void rocketnet_rotation_server_teardown( struct rocketnet_rotation_server_t * test )
{
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

static void rocketnet_rotation_build_request_with_key( struct rocketnet_rotation_server_t * test,
                                                       uint8_t * request,
                                                       uint64_t client_id,
                                                       const uint8_t * cookie_key )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_rotation_fill_vector_fields( &signed_fields, &private_fields, client_id );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    rocketnet_unit_check( rocketnet_generate_cookie_mac( cookie_mac, &test->client_address, rocketnet_test_salt, cookie_expire, cookie_key ) );

    uint8_t return_cookie[ROCKETNET_COOKIE_FIELD_BYTES];
    uint8_t * p = return_cookie;
    rocketnet_write_uint64( &p, cookie_expire );
    rocketnet_write_bytes( &p, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, return_cookie, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

static int rocketnet_rotation_process_at( struct rocketnet_rotation_server_t * test,
                                          const uint8_t * request,
                                          double current_time )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    return rocketnet_server_protocol_process_connection_request( &test->server,
                                                                 &test->client_address,
                                                                 request,
                                                                 ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 ROCKETNET_TEST_CREATE_TS,
                                                                 current_time,
                                                                 response,
                                                                 &response_bytes );
}

static void rocketnet_rotation_clear_mapping( struct rocketnet_rotation_server_t * test )
{
    int index = rocketnet_encryption_map_find( test->server.encryption_map, &test->client_address );
    if ( index != -1 )
    {
        rocketnet_encryption_map_remove_current( test->server.encryption_map, index );
    }
}

static void rocketnet_rotation_build_request_no_cookie( uint8_t * request, uint64_t client_id )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_rotation_fill_vector_fields( &signed_fields, &private_fields, client_id );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

static void rocketnet_rotation_set_client_address( struct rocketnet_rotation_server_t * test, int i )
{
    char addr_str[64];
    snprintf( addr_str, sizeof( addr_str ), "10.1.%d.%d:56789", ( i / 255 ) + 1, ( i % 255 ) + 1 );
    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, addr_str ) );
}

static int rocketnet_rotation_process_from_client_address( struct rocketnet_rotation_server_t * test,
                                                           const uint8_t * request )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    return rocketnet_server_protocol_process_connection_request( &test->server,
                                                                 &test->client_address,
                                                                 request,
                                                                 ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                 ROCKETNET_TEST_CREATE_TS,
                                                                 1000.0,
                                                                 response,
                                                                 &response_bytes );
}

// 1. A cookie minted under the server's current cookie key passes the gate.

static void test_cookie_rotation_current_key_accepted()
{
    struct rocketnet_rotation_server_t test;
    rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_rotation_build_request_with_key( &test, request, 0x1001ULL, test.server.cookie_key );

    uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
    int result = rocketnet_rotation_process_at( &test, request, ROCKETNET_ROTATION_TEST_START_TIME );
    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before );

    rocketnet_rotation_server_teardown( &test );
}

// 2. A cookie minted under K0 passes after one rotation at start + period
// and at start + 2 * period minus a small step.

static void test_cookie_rotation_previous_key_accepted_in_window()
{
    struct rocketnet_rotation_server_t test;
    rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

    const double start = ROCKETNET_ROTATION_TEST_START_TIME;
    const double period = (double) ROCKETNET_ROTATION_TEST_PERIOD_SECONDS;

    uint8_t k0[ROCKETNET_COOKIE_KEY_BYTES];
    memcpy( k0, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_rotation_build_request_with_key( &test, request, 0x2001ULL, k0 );

    rocketnet_server_protocol_update( &test.server, start + period );
    rocketnet_unit_check( test.server.has_previous_cookie_key == 1 );

    uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
    rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, start + period ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before );

    rocketnet_rotation_clear_mapping( &test );
    rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, start + 2.0 * period - 0.001 ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before );

    rocketnet_rotation_server_teardown( &test );
}

// 3. A cookie minted under K0 is refused (COOKIE_DID_NOT_VERIFY) after a
// second update at start + 2 * period, after a single update that jumps from
// start to start + 2 * period (the stall branch), and when presented at
// current_time >= previous_cookie_key_expire_time with no update in between.

static void test_cookie_rotation_previous_key_rejected_after_window()
{
    const double start = ROCKETNET_ROTATION_TEST_START_TIME;
    const double period = (double) ROCKETNET_ROTATION_TEST_PERIOD_SECONDS;

    // case A: normal rotation at start + period, then second rotation at start + 2 * period
    {
        struct rocketnet_rotation_server_t test;
        rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

        uint8_t k0[ROCKETNET_COOKIE_KEY_BYTES];
        memcpy( k0, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_rotation_build_request_with_key( &test, request, 0x3001ULL, k0 );

        rocketnet_server_protocol_update( &test.server, start + period );
        rocketnet_server_protocol_update( &test.server, start + 2.0 * period );

        uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
        rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, start + 2.0 * period ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
        rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before + 1 );

        rocketnet_rotation_server_teardown( &test );
    }

    // case B: stall branch — a single update jumps from start to start + 2 * period
    {
        struct rocketnet_rotation_server_t test;
        rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

        uint8_t k0[ROCKETNET_COOKIE_KEY_BYTES];
        memcpy( k0, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_rotation_build_request_with_key( &test, request, 0x3002ULL, k0 );

        rocketnet_server_protocol_update( &test.server, start + 2.0 * period );
        rocketnet_unit_check( test.server.has_previous_cookie_key == 0 );

        uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
        rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, start + 2.0 * period ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
        rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before + 1 );

        rocketnet_rotation_server_teardown( &test );
    }

    // case C: rotation at start + period, then presented at current_time >= previous_cookie_key_expire_time
    // with no update in between
    {
        struct rocketnet_rotation_server_t test;
        rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

        uint8_t k0[ROCKETNET_COOKIE_KEY_BYTES];
        memcpy( k0, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_rotation_build_request_with_key( &test, request, 0x3003ULL, k0 );

        rocketnet_server_protocol_update( &test.server, start + period );
        rocketnet_unit_check( test.server.has_previous_cookie_key == 1 );
        rocketnet_unit_check( test.server.previous_cookie_key_expire_time == start + 2.0 * period );

        uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
        rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, test.server.previous_cookie_key_expire_time ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
        rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before + 1 );

        rocketnet_rotation_server_teardown( &test );
    }
}

// 4. A cookie minted under a key the server never held is refused both before
// any rotation and while a previous key is live.

static void test_cookie_rotation_never_used_key_rejected()
{
    struct rocketnet_rotation_server_t test;
    rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

    const double start = ROCKETNET_ROTATION_TEST_START_TIME;
    const double period = (double) ROCKETNET_ROTATION_TEST_PERIOD_SECONDS;

    uint8_t never_used_key[ROCKETNET_COOKIE_KEY_BYTES];
    memset( never_used_key, 0xA5, sizeof( never_used_key ) );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_rotation_build_request_with_key( &test, request, 0x4001ULL, never_used_key );

    uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
    rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, start ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before + 1 );

    rocketnet_server_protocol_update( &test.server, start + period );
    rocketnet_unit_check( test.server.has_previous_cookie_key == 1 );

    bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
    rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, start + period ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before + 1 );

    rocketnet_rotation_server_teardown( &test );
}

// 5. Each rotation draws a fresh key different from previous_cookie_key and
// from the key before, and with a period of 0 update never changes the key.

static void test_cookie_rotation_draws_a_fresh_key()
{
    const double start = ROCKETNET_ROTATION_TEST_START_TIME;
    const double period = (double) ROCKETNET_ROTATION_TEST_PERIOD_SECONDS;

    {
        struct rocketnet_rotation_server_t test;
        rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

        uint8_t k0[ROCKETNET_COOKIE_KEY_BYTES];
        uint8_t k1[ROCKETNET_COOKIE_KEY_BYTES];
        memcpy( k0, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

        rocketnet_server_protocol_update( &test.server, start + period );
        memcpy( k1, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
        rocketnet_unit_check( memcmp( test.server.previous_cookie_key, k0, ROCKETNET_COOKIE_KEY_BYTES ) == 0 );
        rocketnet_unit_check( memcmp( k1, test.server.previous_cookie_key, ROCKETNET_COOKIE_KEY_BYTES ) != 0 );

        rocketnet_server_protocol_rotate_cookie_key_if_due( &test.server, start + 2.0 * period );
        rocketnet_unit_check( memcmp( test.server.previous_cookie_key, k1, ROCKETNET_COOKIE_KEY_BYTES ) == 0 );
        rocketnet_unit_check( memcmp( test.server.cookie_key, test.server.previous_cookie_key, ROCKETNET_COOKIE_KEY_BYTES ) != 0 );
        rocketnet_unit_check( memcmp( test.server.cookie_key, k0, ROCKETNET_COOKIE_KEY_BYTES ) != 0 );

        rocketnet_rotation_server_teardown( &test );
    }

    {
        struct rocketnet_rotation_server_t test;
        rocketnet_rotation_server_setup( &test, 0 );

        uint8_t k0[ROCKETNET_COOKIE_KEY_BYTES];
        memcpy( k0, test.server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

        rocketnet_server_protocol_update( &test.server, start + period );
        rocketnet_server_protocol_update( &test.server, start + 10.0 * period );
        rocketnet_unit_check( memcmp( test.server.cookie_key, k0, ROCKETNET_COOKIE_KEY_BYTES ) == 0 );
        rocketnet_unit_check( test.server.has_previous_cookie_key == 0 );

        rocketnet_rotation_server_teardown( &test );
    }
}

// The transport receives before its sweep. A receive near the end of a
// delayed rotation window or after a stall must issue a usable cookie.

static void test_cookie_rotation_receive_before_update_keeps_fresh_cookie()
{
    const double boundary = ROCKETNET_ROTATION_TEST_START_TIME + 2.0 * ROCKETNET_ROTATION_TEST_PERIOD_SECONDS;
    const double receive_times[] = { boundary - 0.1, boundary + 0.1 };

    for ( int i = 0; i < 2; ++i )
    {
        struct rocketnet_rotation_server_t test;
        rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_rotation_build_request_with_key( &test, request, 0x5001ULL, test.server.cookie_key );
        memset( request + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_COOKIE_FIELD_BYTES );

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;
        rocketnet_unit_check( rocketnet_server_protocol_process_connection_request(
                                  &test.server, &test.client_address, request, sizeof( request ),
                                  ROCKETNET_TEST_CREATE_TS, receive_times[i], response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
        rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
        memcpy( request + ROCKETNET_PREFILTER_BYTES,
                response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
                ROCKETNET_COOKIE_FIELD_BYTES );

        rocketnet_server_protocol_update( &test.server, receive_times[i] );

        const double return_time = receive_times[i] + 0.2;
        rocketnet_server_protocol_update( &test.server, return_time );

        const uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
        rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, return_time ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before );
        rocketnet_rotation_server_teardown( &test );
    }
}

static void test_cookie_rotation_receive_after_stall_rejects_outgoing_key()
{
    struct rocketnet_rotation_server_t test;
    rocketnet_rotation_server_setup( &test, ROCKETNET_ROTATION_TEST_PERIOD_SECONDS );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_rotation_build_request_with_key( &test, request, 0x5002ULL, test.server.cookie_key );

    const double after_stall = ROCKETNET_ROTATION_TEST_START_TIME + 2.0 * ROCKETNET_ROTATION_TEST_PERIOD_SECONDS;
    const uint64_t bad_mac_before = test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY];
    rocketnet_unit_check( rocketnet_rotation_process_at( &test, request, after_stall ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] == bad_mac_before + 1 );
    rocketnet_rotation_server_teardown( &test );
}

// The handshake work budget is per update: it reopens at the top of
// rocketnet_server_protocol_update (sdk/rocketnet_server_protocol.cpp,
// rocketnet_server_protocol_update) and is spent only after the cookie
// validates (the check under "the handshake budget check" in
// rocketnet_server_protocol_process_connection_request).

static void test_handshake_budget_reopens_after_update()
{
    struct rocketnet_rotation_server_t test;
    rocketnet_rotation_server_setup( &test, 0 );

    const int budget = ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE;

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];

    for ( int i = 0; i < budget; ++i )
    {
        rocketnet_rotation_set_client_address( &test, i );
        rocketnet_rotation_build_request_with_key( &test, request, UINT64_C( 0x10000 ) + (uint64_t) i, test.server.cookie_key );

        int result = rocketnet_rotation_process_from_client_address( &test, request );
        rocketnet_unit_check( result != ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    }

    rocketnet_rotation_set_client_address( &test, budget );
    rocketnet_rotation_build_request_with_key( &test, request, 0x10000ULL + (uint64_t) budget, test.server.cookie_key );
    rocketnet_unit_check( rocketnet_rotation_process_from_client_address( &test, request ) == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 1 );

    rocketnet_server_protocol_update( &test.server, 1000.0 );

    rocketnet_rotation_set_client_address( &test, budget + 1 );
    rocketnet_rotation_build_request_with_key( &test, request, 0x10000ULL + (uint64_t) ( budget + 1 ), test.server.cookie_key );
    rocketnet_unit_check( rocketnet_rotation_process_from_client_address( &test, request ) != ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 1 );

    rocketnet_rotation_server_teardown( &test );
}

// The handshake budget is spent only past the cookie: a request that fails
// the cookie gate (no return cookie) is answered with a cookie and spends no
// handshake budget (sdk/rocketnet_server_protocol.cpp,
// rocketnet_server_protocol_process_connection_request).

static void test_cookie_requests_spend_no_handshake_budget()
{
    struct rocketnet_rotation_server_t test;
    rocketnet_rotation_server_setup( &test, 0 );

    const int budget = ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE;

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];

    for ( int i = 0; i < budget; ++i )
    {
        rocketnet_rotation_set_client_address( &test, i );
        rocketnet_rotation_build_request_with_key( &test, request, UINT64_C( 0x10000 ) + (uint64_t) i, test.server.cookie_key );

        int result = rocketnet_rotation_process_from_client_address( &test, request );
        rocketnet_unit_check( result != ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    }

    rocketnet_rotation_set_client_address( &test, budget );
    rocketnet_rotation_build_request_with_key( &test, request, 0x10000ULL + (uint64_t) budget, test.server.cookie_key );
    rocketnet_unit_check( rocketnet_rotation_process_from_client_address( &test, request ) == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 1 );

    rocketnet_unit_check( rocketnet_address_parse( &test.client_address, "10.200.0.1:56789" ) );
    rocketnet_rotation_build_request_no_cookie( request, 0x20000ULL );
    rocketnet_unit_check( rocketnet_rotation_process_from_client_address( &test, request ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED] == 1 );

    rocketnet_rotation_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_cookie_rotation_receive_before_update_keeps_fresh_cookie );
REGISTER_UNIT_TEST( test_cookie_rotation_receive_after_stall_rejects_outgoing_key );

REGISTER_UNIT_TEST( test_cookie_rotation_current_key_accepted );
REGISTER_UNIT_TEST( test_cookie_rotation_previous_key_accepted_in_window );
REGISTER_UNIT_TEST( test_cookie_rotation_previous_key_rejected_after_window );
REGISTER_UNIT_TEST( test_cookie_rotation_never_used_key_rejected );
REGISTER_UNIT_TEST( test_cookie_rotation_draws_a_fresh_key );

REGISTER_UNIT_TEST( test_handshake_budget_reopens_after_update );
REGISTER_UNIT_TEST( test_cookie_requests_spend_no_handshake_budget );

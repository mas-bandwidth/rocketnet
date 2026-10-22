/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The two token-time refusals on the connection request path are two
// distinct counters: an expired token and a token created more than thirty
// seconds in the future are each their own reason (STANDARD.md, Counters:
// refusal reasons are never merged; Processing Connection Requests, the
// request order). There is no maximum token lifetime: a long-lived token is
// answered. The fixture is the production configuration. The server is
// driven with time as an argument from 1000.0: no sockets, no network and no
// real clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_keys.h"
#include "rocketnet_read_write.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// the test clock starts at 1000.0; the token timestamps are the same clock in
// whole seconds
#define TOKEN_TIME_NOW              1000ULL
#define TOKEN_TIME_NOW_SECONDS      1000.0

struct token_time_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

// a token whose signed and private expire timestamps agree, minted with the
// chosen create and expire times

static void token_time_mint( uint8_t * token_data, uint64_t create, uint64_t expire )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = expire;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void token_time_setup( struct token_time_server_t * t )
{
    memset( t, 0, sizeof(*t) );
    struct rocketnet_server_protocol_t * server = &t->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 8 );
    server->client_slots = rocketnet_client_slots_create( NULL, 1 );
    server->acks = rocketnet_server_acks_create( NULL, 1 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, 4 );
    server->counters = &t->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots &&
           server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, TOKEN_TIME_NOW );
    check( rocketnet_address_parse( &t->client_address, "198.51.100.7:56789" ) );
}

static void token_time_teardown( struct token_time_server_t * t )
{
    rocketnet_counters_assert_conserved( &t->counters );
    rocketnet_encryption_map_destroy( t->server.encryption_map );
    rocketnet_used_token_history_destroy( t->server.used_token_history );
    rocketnet_client_slots_destroy( t->server.client_slots );
    rocketnet_server_acks_destroy( t->server.acks );
    rocketnet_reassembly_set_destroy( t->server.reassembly );
}

static void token_time_build_request( uint8_t * request, uint64_t create, uint64_t expire )
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    token_time_mint( token_data, create, expire );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

// one request at the test clock, and the exact move it must make: the arrival
// and its own refusal counter, and nothing else

static void token_time_refuse_at( struct token_time_server_t * t, uint64_t create, uint64_t expire,
                                  uint64_t current_timestamp, int counter )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    token_time_build_request( request, create, expire );

    const struct rocketnet_counters_t before = t->counters;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = -1;
    const int result = rocketnet_server_protocol_process_connection_request( &t->server, &t->client_address,
                                                                             request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                             current_timestamp, TOKEN_TIME_NOW_SECONDS,
                                                                             response, &response_bytes );

    check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( response_bytes == 0 );
    rocketnet_counters_expect( &before, &t->counters,
                               { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                 { counter, 1 } } );
}

static void token_time_refuse( struct token_time_server_t * t, uint64_t create, uint64_t expire, int counter )
{
    token_time_refuse_at( t, create, expire, TOKEN_TIME_NOW, counter );
}

// the accept side of a boundary: the server answers with a cookie, so the move
// is the arrival and the cookie issue, and no refusal counter

static void token_time_accept_at( struct token_time_server_t * t, uint64_t create, uint64_t expire,
                                  uint64_t current_timestamp )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    token_time_build_request( request, create, expire );

    const struct rocketnet_counters_t before = t->counters;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = -1;
    const int result = rocketnet_server_protocol_process_connection_request( &t->server, &t->client_address,
                                                                             request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                             current_timestamp, TOKEN_TIME_NOW_SECONDS,
                                                                             response, &response_bytes );

    check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    rocketnet_counters_expect( &before, &t->counters,
                               { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                 { ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE, 1 } } );
}

static void token_time_accept( struct token_time_server_t * t, uint64_t create, uint64_t expire )
{
    token_time_accept_at( t, create, expire, TOKEN_TIME_NOW );
}

static void test_token_time_refusals_are_distinct()
{
    struct token_time_server_t t;
    token_time_setup( &t );

    // expire <= now: the expired token, and only that counter; one second past
    // the boundary the same token is accepted
    token_time_refuse( &t, TOKEN_TIME_NOW - 100, TOKEN_TIME_NOW,
                       ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED );

    // create more than thirty seconds ahead: the skew, not expiry; exactly thirty
    // seconds ahead is inside the allowance
    token_time_refuse( &t, TOKEN_TIME_NOW + 31, TOKEN_TIME_NOW + 40,
                       ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE );

    // the accepted side of each boundary: expire now+1 and create now+30, each
    // answering with a cookie
    token_time_accept( &t, TOKEN_TIME_NOW - 100, TOKEN_TIME_NOW + 1 );
    token_time_accept( &t, TOKEN_TIME_NOW + 30, TOKEN_TIME_NOW + 40 );

    token_time_teardown( &t );
}

// the accepted side of each boundary as its own check: the request is answered
// with a cookie, so it is not ignored and no token-time refusal counter moves

static void test_token_expiring_one_second_ahead_is_accepted()
{
    struct token_time_server_t t;
    token_time_setup( &t );
    token_time_accept( &t, TOKEN_TIME_NOW - 100, TOKEN_TIME_NOW + 1 );
    token_time_teardown( &t );
}

static void test_token_created_at_the_skew_limit_is_accepted()
{
    struct token_time_server_t t;
    token_time_setup( &t );
    token_time_accept( &t, TOKEN_TIME_NOW + 30, TOKEN_TIME_NOW + 40 );
    token_time_teardown( &t );
}

// no maximum token lifetime: a token valid for 2^40 seconds is answered, and
// the retired CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG stays zero
static void test_token_with_a_long_lifetime_is_accepted()
{
    struct token_time_server_t t;
    token_time_setup( &t );
    token_time_accept( &t, TOKEN_TIME_NOW, TOKEN_TIME_NOW + ( 1ULL << 40 ) );
    check( t.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG] == 0 );
    token_time_teardown( &t );
}

REGISTER_UNIT_TEST( test_token_time_refusals_are_distinct );
REGISTER_UNIT_TEST( test_token_expiring_one_second_ahead_is_accepted );
REGISTER_UNIT_TEST( test_token_created_at_the_skew_limit_is_accepted );
REGISTER_UNIT_TEST( test_token_with_a_long_lifetime_is_accepted );

static void test_token_future_creation_without_lifetime_limit()
{
    struct token_time_server_t t;
    token_time_setup( &t );
    token_time_refuse( &t, TOKEN_TIME_NOW + 31, TOKEN_TIME_NOW + 40,
                       ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE );
    token_time_accept( &t, TOKEN_TIME_NOW + 30, TOKEN_TIME_NOW + 10000 );
    token_time_teardown( &t );
}

REGISTER_UNIT_TEST( test_token_future_creation_without_lifetime_limit );

static void test_token_creation_skew_is_overflow_safe()
{
    struct token_time_server_t t;
    token_time_setup( &t );
    // Adding the allowance to this clock would wrap, but the ordered
    // subtraction keeps a token 29 seconds ahead inside the allowance.
    token_time_accept_at( &t, UINT64_MAX, UINT64_MAX, UINT64_MAX - 29 );
    token_time_refuse_at( &t, UINT64_MAX, UINT64_MAX, UINT64_MAX - 31,
                          ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE );
    token_time_teardown( &t );
}

REGISTER_UNIT_TEST( test_token_creation_skew_is_overflow_safe );

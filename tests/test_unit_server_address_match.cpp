/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A dual-stack server advertises its IPv4 wildcard alongside its IPv6
// address, so a connect token naming the IPv4 loopback is accepted, a
// v4-mapped token address normalises to native IPv4 and matches the same
// way, and a foreign address is still refused. Runs on the real protocol
// objects with time passed in and no sockets, no network and no real clock.

#include "test_unit_registry.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

struct rocketnet_server_address_match_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void rocketnet_server_address_match_server_setup( struct rocketnet_server_address_match_server_t * test )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv6, "[::]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv4, "0.0.0.0:40000" ) );

    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = &test->counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history &&
                          server->client_slots && server->acks );

    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );

    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void rocketnet_server_address_match_server_teardown( struct rocketnet_server_address_match_server_t * test )
{
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
}

static void rocketnet_server_address_match_mint_request( uint8_t * request, uint8_t address_flags,
                                                         const uint8_t * ipv4, const uint8_t * ipv6 )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    memset( &private_fields, 0, sizeof( private_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = address_flags;

    if ( ipv4 )
    {
        memcpy( signed_fields.ipv4_address, ipv4, 6 );
    }

    if ( ipv6 )
    {
        memcpy( signed_fields.ipv6_address, ipv6, 18 );
    }

    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

// IPv4 loopback and v4-mapped IPv6 both name the loopback the dual-stack
// server serves; a foreign IPv4 names an address it does not.

void test_rocketnet_test_server_address_match_v4_mapped()
{
    struct rocketnet_server_address_match_server_t test;
    rocketnet_server_address_match_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int addr_counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED;

    // loopback 127.0.0.1:40000

    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, loopback_ipv4, NULL );

    const uint64_t before = test.counters.values[addr_counter];
    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ), ROCKETNET_TEST_CREATE_TS, 100.0,
                                                                                response, &response_bytes ) != ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[addr_counter] == before );

    // v4-mapped ::ffff:127.0.0.1:40000, normalised to the same loopback

    const uint8_t mapped_ipv6[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0x00, 0x7F, 0x01, 0x00, 0x40, 0x9c };
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV6, NULL, mapped_ipv6 );

    const uint64_t before_mapped = test.counters.values[addr_counter];
    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ), ROCKETNET_TEST_CREATE_TS, 100.0,
                                                                                response, &response_bytes ) != ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[addr_counter] == before_mapped );

    // a foreign IPv4 is still refused

    const uint8_t foreign_ipv4[6] = { 198, 51, 100, 1, 0x40, 0x9c };
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, foreign_ipv4, NULL );

    const uint64_t before_foreign = test.counters.values[addr_counter];
    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ), ROCKETNET_TEST_CREATE_TS, 100.0,
                                                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[addr_counter] == before_foreign + 1 );

    rocketnet_server_address_match_server_teardown( &test );
}

// A token naming both families (rocketnet_write_production_connect_token
// with two addresses) is accepted by a server that serves either one: an
// IPv4 server matches the IPv4 address of a request arriving over IPv4
// although the IPv6 flag is set, an IPv6 server matches the IPv6 address of
// a request arriving over IPv6, and a token whose two addresses are both
// foreign is still refused (rocketnet_server_token_address_is_advertised
// in sdk/rocketnet_server_protocol.cpp).

void test_rocketnet_test_server_address_match_two_address_token()
{
    struct rocketnet_server_address_match_server_t test;
    rocketnet_server_address_match_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int addr_counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED;
    const uint8_t both = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;

    // 127.0.0.1:40000 and [::1]:40000

    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    const uint8_t loopback_ipv6[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };

    // an IPv4 server at 127.0.0.1:40000, no IPv6

    memset( &test.server.advertised_ipv6, 0, sizeof( test.server.advertised_ipv6 ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv4, "127.0.0.1:40000" ) );

    rocketnet_server_address_match_mint_request( request, both, loopback_ipv4, loopback_ipv6 );

    const uint64_t before_ipv4 = test.counters.values[addr_counter];
    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ), ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                                response, &response_bytes ) != ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[addr_counter] == before_ipv4 );

    // an IPv6 server at [::1]:40000, no IPv4; the request arrives over IPv6,
    // so the token's IPv6 address is the one checked (TokenAddressAdvertised)

    memset( &test.server.advertised_ipv4, 0, sizeof( test.server.advertised_ipv4 ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv6, "[::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.client_address, "[2001:db8::7]:56789" ) );

    rocketnet_server_address_match_mint_request( request, both, loopback_ipv4, loopback_ipv6 );

    const uint64_t before_ipv6 = test.counters.values[addr_counter];
    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ), ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                                response, &response_bytes ) != ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[addr_counter] == before_ipv6 );

    // both addresses foreign to the IPv6 server: still refused

    const uint8_t foreign_ipv4[6] = { 198, 51, 100, 1, 0x40, 0x9c };
    const uint8_t foreign_ipv6[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };
    rocketnet_server_address_match_mint_request( request, both, foreign_ipv4, foreign_ipv6 );

    const uint64_t before_foreign = test.counters.values[addr_counter];
    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ), ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[addr_counter] == before_foreign + 1 );

    rocketnet_server_address_match_server_teardown( &test );
}

// The arrival family's address is the only one checked, so each family
// mismatch is refused even when the token's other bytes name this server: a
// request over IPv4 with a token naming only an IPv6 address the server
// advertises; a request over IPv6 with a token whose IPv6 flag is clear,
// although its IPv6 bytes name the server; a request over IPv6 with a token
// whose IPv6 address is v4-mapped (that is an IPv4 address) naming the
// server's IPv4 loopback. Each is ADDRESS_NOT_ADVERTISED once, IGNORED, and
// sends nothing; the same server accepts the matching family.

static void rocketnet_server_address_match_refused( struct rocketnet_server_address_match_server_t * test, const uint8_t * request, bool refused )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int addr_counter = ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED;
    const uint64_t before = test->counters.values[addr_counter];
    const int result = rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address, request,
                                                                             ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, ROCKETNET_TEST_CREATE_TS,
                                                                             1000.0, response, &response_bytes );
    if ( refused )
    {
        rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( test->counters.values[addr_counter] == before + 1 );
        rocketnet_unit_check( response_bytes == 0 );
    }
    else
    {
        rocketnet_unit_check( result != ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( test->counters.values[addr_counter] == before );
        rocketnet_unit_check( response_bytes > 0 && response_bytes <= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    }
}

void test_rocketnet_test_server_address_match_family_mismatch_refused()
{
    struct rocketnet_server_address_match_server_t test;
    rocketnet_server_address_match_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    const uint8_t loopback_ipv6[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };
    const uint8_t mapped_ipv6[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0x00, 0x7F, 0x01, 0x00, 0x40, 0x9c };

    // a server naming both families, 127.0.0.1:40000 and [::1]:40000

    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv4, "127.0.0.1:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv6, "[::1]:40000" ) );

    // over IPv4: a token naming only [::1]:40000 is refused, one naming
    // 127.0.0.1:40000 accepted

    rocketnet_unit_check( rocketnet_address_parse( &test.client_address, "198.51.100.7:56789" ) );
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV6, NULL, loopback_ipv6 );
    rocketnet_server_address_match_refused( &test, request, true );
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, loopback_ipv4, NULL );
    rocketnet_server_address_match_refused( &test, request, false );

    // over IPv6: a token with only the IPv4 flag is refused although its IPv6
    // bytes are [::1]:40000; with the IPv6 flag it is accepted

    rocketnet_unit_check( rocketnet_address_parse( &test.client_address, "[2001:db8::7]:56789" ) );
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, loopback_ipv4, loopback_ipv6 );
    rocketnet_server_address_match_refused( &test, request, true );
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV6, NULL, loopback_ipv6 );
    rocketnet_server_address_match_refused( &test, request, false );

    // over IPv6 to a dual-stack server ([::]:40000 beside 0.0.0.0:40000): a
    // v4-mapped ::ffff:127.0.0.1:40000, which over IPv4 is the loopback the
    // socket serves, is refused

    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv6, "[::]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv4, "0.0.0.0:40000" ) );
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV6, NULL, mapped_ipv6 );
    rocketnet_server_address_match_refused( &test, request, true );

    rocketnet_server_address_match_server_teardown( &test );
}

// A request whose source is v4-mapped (::ffff:a.b.c.d, an IPv4 client seen
// through an IPv6 socket) arrives over IPv4: the token's IPv4 address is the
// one checked. A token naming 127.0.0.1:40000 is accepted; one naming only
// [::1]:40000, an address the server does advertise, is refused.

void test_rocketnet_test_server_address_match_v4_mapped_arrival()
{
    struct rocketnet_server_address_match_server_t test;
    rocketnet_server_address_match_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    const uint8_t loopback_ipv6[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };

    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv4, "127.0.0.1:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv6, "[::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.client_address, "[::ffff:198.51.100.7]:56789" ) );
    rocketnet_unit_check( test.client_address.type == ROCKETNET_ADDRESS_IPV6 );

    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV6, NULL, loopback_ipv6 );
    rocketnet_server_address_match_refused( &test, request, true );
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, loopback_ipv4, NULL );
    rocketnet_server_address_match_refused( &test, request, false );

    rocketnet_server_address_match_server_teardown( &test );
}

// Near misses. Over IPv4, a token naming only an IPv6 address that is not
// v4-mapped is refused even when its low four bytes and port spell this
// server's IPv4 address ([2001:db8::7f00:1]:40000 beside 127.0.0.1:40000).
// On a dual-stack server the IPv4 loopback is exactly 127.0.0.1 at the
// server's port: 127.5.0.1, 127.0.5.1, 127.0.0.2 and 127.0.0.1:40001 are
// refused, 127.0.0.1:40000 accepted.

void test_rocketnet_test_server_address_match_near_misses_refused()
{
    struct rocketnet_server_address_match_server_t test;
    rocketnet_server_address_match_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];

    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv4, "127.0.0.1:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv6, "[::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.client_address, "198.51.100.7:56789" ) );

    const uint8_t spelled_ipv6[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x7F, 0x01, 0x00, 0x40, 0x9c };
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV6, NULL, spelled_ipv6 );
    rocketnet_server_address_match_refused( &test, request, true );

    // dual stack: [::]:40000 beside 0.0.0.0:40000

    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv6, "[::]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test.server.advertised_ipv4, "0.0.0.0:40000" ) );

    const uint8_t near[4][6] = {
        { 127, 5, 0, 1, 0x40, 0x9c },
        { 127, 0, 5, 1, 0x40, 0x9c },
        { 127, 0, 0, 2, 0x40, 0x9c },
        { 127, 0, 0, 1, 0x41, 0x9c },
    };
    for ( int i = 0; i < 4; ++i )
    {
        rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, near[i], NULL );
        rocketnet_server_address_match_refused( &test, request, true );
    }

    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, loopback_ipv4, NULL );
    rocketnet_server_address_match_refused( &test, request, false );

    rocketnet_server_address_match_server_teardown( &test );
}

// The server's timeout cap: the token asks for ROCKETNET_TEST_TIMEOUT (10)
// seconds; a cap of 0 means no cap, a cap below the token's request lowers
// it (1 gives 1), a cap at or above it leaves it. The accepted request's
// half-open mapping holds the result.

void test_rocketnet_test_server_address_match_timeout_cap()
{
    const int32_t caps[4] = { 0, 1, ROCKETNET_TEST_TIMEOUT, ROCKETNET_TEST_TIMEOUT + 1 };
    const int32_t want[4] = { ROCKETNET_TEST_TIMEOUT, 1, ROCKETNET_TEST_TIMEOUT, ROCKETNET_TEST_TIMEOUT };
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };

    for ( int i = 0; i < 4; ++i )
    {
        struct rocketnet_server_address_match_server_t test;
        rocketnet_server_address_match_server_setup( &test );
        test.server.max_timeout_seconds = caps[i];

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_server_address_match_mint_request( request, ROCKETNET_ADDRESS_FLAG_IPV4, loopback_ipv4, NULL );

        // a valid cookie for this address, so the request opens a mapping
        const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
        uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
        rocketnet_unit_check( rocketnet_generate_cookie_mac( cookie_mac, &test.client_address, rocketnet_test_salt, cookie_expire,
                                                             test.server.cookie_key ) );
        uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
        rocketnet_write_uint64( &cookie, cookie_expire );
        memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

        rocketnet_server_address_match_refused( &test, request, false );

        const int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
        rocketnet_unit_check( index >= 0 );
        const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
        rocketnet_unit_check( mapping->control->timeout_seconds == want[i] );

        rocketnet_server_address_match_server_teardown( &test );
    }
}

REGISTER_UNIT_TEST( test_rocketnet_test_server_address_match_v4_mapped );
REGISTER_UNIT_TEST( test_rocketnet_test_server_address_match_timeout_cap );
REGISTER_UNIT_TEST( test_rocketnet_test_server_address_match_near_misses_refused );
REGISTER_UNIT_TEST( test_rocketnet_test_server_address_match_v4_mapped_arrival );
REGISTER_UNIT_TEST( test_rocketnet_test_server_address_match_two_address_token );
REGISTER_UNIT_TEST( test_rocketnet_test_server_address_match_family_mismatch_refused );

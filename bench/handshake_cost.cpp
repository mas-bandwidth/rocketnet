/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The cost of one unit of the server's per-update handshake work budget
// (handshakes_per_update): one cookie-validated connection request through
// the protocol's request ladder, the shape a flood or a mass join spends.
//
// Two request shapes, each from its own source address with a valid cookie
// for that address, so every request passes the cookie gate and is counted
// against the budget:
//
//   valid   a validly signed token: Ed25519 verify, the sealed private block
//           opened (X25519), an ephemeral keypair, the connection keys
//           derived and the challenge sealed. The most a request costs.
//   forged  a signature that does not verify: the verify alone. What a
//           forging source that the failed-verify table cannot hold costs.
//
// The requests are built before the timed window. Time is simulated (1000.0);
// the wall clock is read only to time the batch.
//
// handshake_cost [requests] [rounds]
// prints the microseconds per request, best and median over the rounds.

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
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet.h"


#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define CHECK( x ) do { if (!(x)) { fprintf(stderr, "handshake cost bench failed: %s, line %d\n", #x, __LINE__); exit(1); } } while (0)

struct cost_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
};

static void cost_server_setup( cost_server_t * test, int requests )
{
    memset( test, 0, sizeof( *test ) );
    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    CHECK( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, requests * 2 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, requests * 4 );
    server->client_slots = rocketnet_client_slots_create( NULL, requests );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &test->counters;
    CHECK( server->encryption_map && server->used_token_history && server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, 1000.0 );
}

static void cost_server_teardown( cost_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

static void cost_address( struct rocketnet_address_t * address, int i )
{
    char text[64];
    snprintf( text, sizeof( text ), "10.%d.%d.%d:56789", ( i / 62500 ) + 1, ( ( i / 250 ) % 250 ) + 1, ( i % 250 ) + 1 );
    CHECK( rocketnet_address_parse( address, text ) );
}

static void cost_build_request( uint8_t * request, int index, const struct rocketnet_address_t * address, bool forged )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = (uint64_t) ROCKETNET_TEST_CREATE_TS - 1 - (uint64_t) index;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = (uint64_t) index + 1;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    CHECK( rocketnet_generate_cookie_mac( cookie_mac, address, rocketnet_test_salt, cookie_expire, rocketnet_test_cookie_key ) );
    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

    if ( forged )
    {
        const int signature_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES +
                                     ROCKETNET_KEY_BYTES + ROCKETNET_SIGNED_CONNECT_DATA_BYTES;
        request[signature_offset] ^= 1;
    }
}

static double cost_round( int requests, bool forged, int expected )
{
    std::vector<struct rocketnet_address_t> addresses( (size_t) requests );
    std::vector<uint8_t> packets( (size_t) requests * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    for ( int i = 0; i < requests; i++ )
    {
        cost_address( &addresses[(size_t) i], i );
        cost_build_request( &packets[(size_t) i * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES], i, &addresses[(size_t) i], forged );
    }

    cost_server_t * test = (cost_server_t *) calloc( 1, sizeof( cost_server_t ) );
    CHECK( test );
    cost_server_setup( test, requests );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int matched = 0;
    const auto start = std::chrono::steady_clock::now();
    for ( int i = 0; i < requests; i++ )
    {
        int response_bytes = 0;
        const int result = rocketnet_server_protocol_process_connection_request( &test->server, &addresses[(size_t) i],
                                                                                 &packets[(size_t) i * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES],
                                                                                 ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                                 ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                                 response, &response_bytes );
        matched += result == expected;
    }
    const auto end = std::chrono::steady_clock::now();
    CHECK( matched == requests );

    cost_server_teardown( test );
    free( test );
    return std::chrono::duration<double, std::micro>( end - start ).count() / requests;
}

int main( int argc, char ** argv )
{
    const int requests = argc > 1 ? atoi( argv[1] ) : 512;
    const int rounds = argc > 2 ? atoi( argv[2] ) : 9;
    CHECK( requests >= 1 && requests <= ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE );
    CHECK( rounds >= 1 );
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    const char * names[2] = { "valid", "forged" };
    const int expected[2] = { ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE, ROCKETNET_SERVER_PROCESS_IGNORED };
    for ( int shape = 0; shape < 2; shape++ )
    {
        std::vector<double> us;
        for ( int r = 0; r < rounds; r++ )
            us.push_back( cost_round( requests, shape == 1, expected[shape] ) );
        std::sort( us.begin(), us.end() );
        printf( "%-6s requests %d rounds %d us_per_request best %.1f median %.1f\n",
                names[shape], requests, rounds, us[0], us[us.size() / 2] );
    }

    rocketnet_term();
    return 0;
}

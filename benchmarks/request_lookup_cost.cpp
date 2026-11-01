/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The cost of a connection request on the server's receive path, as the
// transport drives it: one encryption-map lookup of the source, then
// rocketnet_server_protocol_process_received with that answer.
//
// Two request shapes, the cheap ones a flood or a resending client chooses:
//
//   no_cookie  first contact with an all-zero cookie: the cheapest refusal,
//              answered with a cookie (the cookie MAC is the one other keyed
//              hash it pays).
//   resend     a request from an address whose handshake is in flight: the
//              existing-mapping rung answers with the stored challenge.
//
// The requests and, for resend, the mappings are made before the timed
// window. Time is simulated (1000.0); the wall clock is read only to time the
// batch.
//
// request_lookup_cost [requests] [rounds]
// prints the nanoseconds per request, best and median over the rounds.

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
#include "rocketnet.h"

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define CHECK( x ) do { if (!(x)) { fprintf(stderr, "request lookup cost bench failed: %s, line %d\n", #x, __LINE__); exit(1); } } while (0)

struct lookup_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
};

static void lookup_server_setup( lookup_server_t * test, int requests )
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
    server->counters = &test->counters;
    CHECK( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, 1000.0 );
}

static void lookup_server_teardown( lookup_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
}

static void lookup_address( struct rocketnet_address_t * address, int i )
{
    char text[64];
    snprintf( text, sizeof( text ), "10.%d.%d.%d:56789", ( i / 62500 ) + 1, ( ( i / 250 ) % 250 ) + 1, ( i % 250 ) + 1 );
    CHECK( rocketnet_address_parse( address, text ) );
}

static void lookup_build_request( uint8_t * request, int index, const struct rocketnet_address_t * address, bool cookie_valid )
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
    if ( !cookie_valid )
        return;

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    CHECK( rocketnet_generate_cookie_mac( cookie_mac, address, rocketnet_test_salt, cookie_expire, rocketnet_test_cookie_key ) );
    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
}

// one request as the transport receives it: its lookup, then process_received
static int lookup_receive( lookup_server_t * test, const struct rocketnet_address_t * address, const uint8_t * request )
{
    static uint8_t response[ROCKETNET_RECEIVE_CEILING];
    static uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    int payload_client_index = -1;
    const int known = rocketnet_encryption_map_find( test->server.encryption_map, address );
    return rocketnet_server_protocol_process_received( &test->server, address, known, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                       ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes, opened,
                                                       ROCKETNET_RECEIVE_CEILING, &payload, &payload_bytes, &payload_client_index );
}

static double lookup_round( int requests, bool resend, int expected )
{
    std::vector<struct rocketnet_address_t> addresses( (size_t) requests );
    std::vector<uint8_t> packets( (size_t) requests * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    for ( int i = 0; i < requests; i++ )
    {
        lookup_address( &addresses[(size_t) i], i );
        lookup_build_request( &packets[(size_t) i * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES], i, &addresses[(size_t) i], resend );
    }

    lookup_server_t * test = (lookup_server_t *) calloc( 1, sizeof( lookup_server_t ) );
    CHECK( test );
    lookup_server_setup( test, requests );

    // resend: every address's handshake is in flight before the window

    if ( resend )
    {
        for ( int i = 0; i < requests; i++ )
            CHECK( lookup_receive( test, &addresses[(size_t) i], &packets[(size_t) i * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES] ) ==
                   ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    }

    int matched = 0;
    const auto start = std::chrono::steady_clock::now();
    for ( int i = 0; i < requests; i++ )
        matched += lookup_receive( test, &addresses[(size_t) i], &packets[(size_t) i * ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES] ) == expected;
    const auto end = std::chrono::steady_clock::now();
    CHECK( matched == requests );

    lookup_server_teardown( test );
    free( test );
    return std::chrono::duration<double, std::nano>( end - start ).count() / requests;
}

int main( int argc, char ** argv )
{
    const int requests = argc > 1 ? atoi( argv[1] ) : 1024;
    const int rounds = argc > 2 ? atoi( argv[2] ) : 21;
    CHECK( requests >= 1 && requests <= ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE );
    CHECK( rounds >= 1 );
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    const char * names[2] = { "no_cookie", "resend" };
    const int expected[2] = { ROCKETNET_SERVER_PROCESS_SEND_COOKIE, ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE };
    for ( int shape = 0; shape < 2; shape++ )
    {
        std::vector<double> ns;
        for ( int r = 0; r < rounds; r++ )
            ns.push_back( lookup_round( requests, shape == 1, expected[shape] ) );
        std::sort( ns.begin(), ns.end() );
        printf( "%-9s requests %d rounds %d ns_per_request best %.1f median %.1f\n", names[shape], requests, rounds, ns[0], ns[ns.size() / 2] );
    }

    rocketnet_term();
    return 0;
}

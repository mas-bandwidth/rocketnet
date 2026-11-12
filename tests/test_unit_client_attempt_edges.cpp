/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client attempt at its edges (sdk/rocketnet_client_protocol.cpp,
// standard/TRANSPORT.md t8.5, t11 rule 2, t7 rule 10): every attempt carries a
// fresh nonzero salt; the keep-alive carries the slot index and slot count; a
// token whose create timestamp equals its expire timestamp is valid; a token
// zero pad that is not zero is refused with 0; a failed reconnect wipes the
// token it read, as the first failure did; an update of a client at rest
// changes nothing; and a send falls due at exactly the send interval.
//
// A client and a server protocol pair run in memory on a mocked clock that
// starts at 1000.0: no sockets, no network, no real time. The pair is heap
// held.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"

#include <sodium.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#define ATTEMPT_EDGES_SLOTS 4

struct attempt_edges_pair_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
};

static void attempt_edges_mint( uint8_t * token_data, uint64_t create, uint64_t expire, int32_t timeout )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = timeout;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 0xA77E9ULL;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = expire;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void attempt_edges_mint_valid( uint8_t * token_data )
{
    attempt_edges_mint( token_data, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
}

// the server on the test vector keys, and a client connected to it through
// the three round trips: the client sends at 1000.0, 1000.2 and 1000.4, and
// each answer reaches it the round's delay later

static const double attempt_edges_delays[3] = { 0.05, 0.05, 0.05 };

static struct attempt_edges_pair_t * attempt_edges_pair_create_answered( const double * answer_delays )
{
    struct attempt_edges_pair_t * pair = (struct attempt_edges_pair_t *) malloc( sizeof( struct attempt_edges_pair_t ) );
    check( pair != NULL );
    memset( pair, 0, sizeof( *pair ) );

    struct rocketnet_server_protocol_t * server = &pair->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, ATTEMPT_EDGES_SLOTS );
    server->acks = rocketnet_server_acks_create( NULL, ATTEMPT_EDGES_SLOTS );
    server->counters = &pair->server_counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );
    check( rocketnet_address_parse( &pair->client_address, "198.51.100.7:56789" ) );

    rocketnet_client_protocol_init( &pair->client, &pair->client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    attempt_edges_mint_valid( token_data );
    check( rocketnet_client_protocol_connect( &pair->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );

    int packet_bytes = 0, response_bytes = 0, payload_bytes = 0, payload_client_index = -1;
    const double times[3] = { 1000.0, 1000.2, 1000.4 };

    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_client_protocol_update( &pair->client, times[i], pair->packet, &packet_bytes );
        check( packet_bytes > 0 );
        check( rocketnet_server_protocol_process_packet( server, &pair->client_address, pair->packet, packet_bytes,
                                                         ROCKETNET_TEST_CREATE_TS, times[i],
                                                         pair->response, &response_bytes,
                                                         pair->payload, &payload_bytes, &payload_client_index ) != ROCKETNET_SERVER_PROCESS_IGNORED );
        check( rocketnet_client_protocol_process_packet( &pair->client, pair->response, response_bytes, times[i] + answer_delays[i],
                                                         pair->payload, (int) sizeof( pair->payload ), &payload_bytes ) );
    }

    check( pair->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    return pair;
}

static void attempt_edges_pair_destroy( struct attempt_edges_pair_t * pair )
{
    rocketnet_counters_assert_conserved( &pair->server_counters );
    rocketnet_counters_assert_conserved( &pair->client_counters );
    rocketnet_encryption_map_destroy( pair->server.encryption_map );
    rocketnet_used_token_history_destroy( pair->server.used_token_history );
    rocketnet_client_slots_destroy( pair->server.client_slots );
    rocketnet_server_acks_destroy( pair->server.acks );
    free( pair );
}

// a client on its own, heap held with its counters

struct attempt_edges_client_t
{
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
};

static struct attempt_edges_client_t * attempt_edges_client_create()
{
    struct attempt_edges_client_t * c = (struct attempt_edges_client_t *) malloc( sizeof( struct attempt_edges_client_t ) );
    check( c != NULL );
    memset( c, 0, sizeof( *c ) );
    rocketnet_client_protocol_init( &c->client, &c->counters );
    return c;
}

static void attempt_edges_client_destroy( struct attempt_edges_client_t * c )
{
    rocketnet_counters_assert_conserved( &c->counters );
    free( c );
}

// every attempt draws its own salt (t7): never all zero, never the salt of
// another attempt, and the one the connection request carries

static void test_client_attempt_salt_is_fresh_and_nonzero()
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    attempt_edges_mint_valid( token_data );

    struct attempt_edges_client_t * a = attempt_edges_client_create();
    struct attempt_edges_client_t * b = attempt_edges_client_create();

    check( rocketnet_client_protocol_connect( &a->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
    check( rocketnet_client_protocol_connect( &b->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );

    check( sodium_is_zero( a->client.connection_salt, sizeof( a->client.connection_salt ) ) == 0 );
    check( sodium_is_zero( b->client.connection_salt, sizeof( b->client.connection_salt ) ) == 0 );
    check( memcmp( a->client.connection_salt, b->client.connection_salt, sizeof( a->client.connection_salt ) ) != 0 );

    // a second attempt on the same client, after it ends, draws again

    uint8_t first[ROCKETNET_CONNECTION_SALT_BYTES];
    memcpy( first, a->client.connection_salt, sizeof( first ) );
    int packet_count = -1, packet_bytes = -1;
    rocketnet_client_protocol_disconnect( &a->client, a->packet, &packet_count, &packet_bytes );
    check( packet_count == 0 );     // requesting cookie: no session keys, nothing to seal
    check( a->client.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( rocketnet_client_protocol_connect( &a->client, token_data, 1000.2, ROCKETNET_TEST_CREATE_TS ) );
    check( sodium_is_zero( a->client.connection_salt, sizeof( a->client.connection_salt ) ) == 0 );
    check( memcmp( first, a->client.connection_salt, sizeof( first ) ) != 0 );

    attempt_edges_client_destroy( a );
    attempt_edges_client_destroy( b );
}

REGISTER_UNIT_TEST( test_client_attempt_salt_is_fresh_and_nonzero );

// t8.5: the keep-alive's sealed plaintext ends with the client index and the
// slot count, each a u32. The client's own keep-alive, opened with its
// client-to-server session key, carries slot 0 of 4.

static void test_client_keep_alive_carries_index_and_slot_count()
{
    struct attempt_edges_pair_t * pair = attempt_edges_pair_create_answered( attempt_edges_delays );
    check( pair->client.client_index == 0 );
    check( pair->client.max_clients == ATTEMPT_EDGES_SLOTS );

    int bytes = 0;
    rocketnet_client_protocol_update( &pair->client, 1001.0, pair->packet, &bytes );
    check( bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    check( pair->packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );

    const uint8_t * p = pair->packet + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_read_uint32( &p );
    check( sequence > 0 );

    const int opened = rocketnet_open_sealed_packet( pair->payload, NULL, (int) sizeof( pair->payload ), pair->packet, bytes,
                                                     ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence, ROCKETNET_TEST_PROTOCOL_ID,
                                                     pair->client.connection_salt, NULL, pair->client.keys.session_client_to_server );
    check( opened >= ROCKETNET_ACK_BLOCK_MIN_BYTES + 8 );

    const uint8_t * q = pair->payload + opened - 8;
    check( rocketnet_read_uint32( &q ) == 0 );
    check( rocketnet_read_uint32( &q ) == ATTEMPT_EDGES_SLOTS );

    attempt_edges_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_client_keep_alive_carries_index_and_slot_count );

// t11 rule 2: a token is invalid when its create timestamp is later than its
// expire timestamp, so one whose create equals its expire is valid. Its
// lifetime is zero, so the first update ends the attempt in connect token
// expired (-7) with nothing sent (rule 6).

static void test_client_token_with_create_equal_to_expire_is_valid()
{
    const uint64_t at = ROCKETNET_TEST_CREATE_TS + 5;
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    attempt_edges_mint( token_data, at, at, 10 );

    struct attempt_edges_client_t * c = attempt_edges_client_create();
    check( rocketnet_client_protocol_connect( &c->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) == 1 );
    check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    int bytes = -1;
    rocketnet_client_protocol_update( &c->client, 1000.0, c->packet, &bytes );
    check( bytes == 0 );
    check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );

    // one second later than create, the same token is refused as invalid
    attempt_edges_mint( token_data, at + 1, at, 10 );
    check( rocketnet_client_protocol_connect( &c->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) == 0 );
    check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );

    attempt_edges_client_destroy( c );
}

REGISTER_UNIT_TEST( test_client_token_with_create_equal_to_expire_is_valid );

// t1 rule 3 and t6.2/t6.3: a token whose outer zero pad or signed zero pad
// holds a nonzero byte is refused: connect returns 0 and the client is in
// invalid connect token (-6)

static void test_client_token_zero_pad_refused_with_zero()
{
    const int outer_pad_offset = ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES + 2 * ROCKETNET_KEY_BYTES;
    const int signed_pad_offset = ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES;
    check( outer_pad_offset < ROCKETNET_CONNECT_TOKEN_BYTES );
    check( signed_pad_offset < ROCKETNET_SIGNED_CONNECT_DATA_BYTES );

    const int offsets[2] = { ROCKETNET_CONNECT_TOKEN_BYTES - 1, ROCKETNET_SIGNED_CONNECT_DATA_BYTES - 1 };

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        attempt_edges_mint_valid( token_data );
        check( token_data[offsets[i]] == 0 );
        token_data[offsets[i]] = 1;

        struct attempt_edges_client_t * c = attempt_edges_client_create();
        check( rocketnet_client_protocol_connect( &c->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) == 0 );
        check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );
        attempt_edges_client_destroy( c );
    }
}

REGISTER_UNIT_TEST( test_client_token_zero_pad_refused_with_zero );

// t7 rule 10: an attempt that ends erases the token keys. A token read and
// then refused (here a timeout of zero) is erased on the first refusal, and
// again on a second refusal from that failure state.

static void test_client_failed_reconnect_wipes_the_token()
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    attempt_edges_mint( token_data, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 0 );

    struct attempt_edges_client_t * c = attempt_edges_client_create();

    for ( int i = 0; i < 2; i++ )
    {
        check( rocketnet_client_protocol_connect( &c->client, token_data, 1000.0 + i, ROCKETNET_TEST_CREATE_TS ) == 0 );
        check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );
        check( sodium_is_zero( (const unsigned char *) &c->client.token, sizeof( c->client.token ) ) == 1 );
        check( sodium_is_zero( (const unsigned char *) &c->client.signed_fields, sizeof( c->client.signed_fields ) ) == 1 );
    }

    // the same for an expired token, read and then refused

    attempt_edges_mint( token_data, ROCKETNET_TEST_CREATE_TS - 10, ROCKETNET_TEST_CREATE_TS, 10 );

    for ( int i = 0; i < 2; i++ )
    {
        check( rocketnet_client_protocol_connect( &c->client, token_data, 1002.0 + i, ROCKETNET_TEST_CREATE_TS ) == 0 );
        check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
        check( sodium_is_zero( (const unsigned char *) &c->client.token, sizeof( c->client.token ) ) == 1 );
    }

    attempt_edges_client_destroy( c );
}

REGISTER_UNIT_TEST( test_client_failed_reconnect_wipes_the_token );

// a client that is disconnected (0) or failed (below 0) is at rest: an update
// sends nothing and changes no byte of its state

static void attempt_edges_update_at_rest( struct attempt_edges_client_t * c, double time )
{
    struct rocketnet_client_protocol_t * before = (struct rocketnet_client_protocol_t *) malloc( sizeof( struct rocketnet_client_protocol_t ) );
    check( before != NULL );
    memcpy( before, &c->client, sizeof( *before ) );

    int bytes = -1;
    rocketnet_client_protocol_update( &c->client, time, c->packet, &bytes );
    check( bytes == 0 );
    check( memcmp( before, &c->client, sizeof( *before ) ) == 0 );

    free( before );
}

static void test_client_update_at_rest_changes_nothing()
{
    struct attempt_edges_client_t * c = attempt_edges_client_create();
    check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    attempt_edges_update_at_rest( c, 1000.0 );
    attempt_edges_update_at_rest( c, 1005.0 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    attempt_edges_mint( token_data, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 0 );
    check( rocketnet_client_protocol_connect( &c->client, token_data, 1010.0, ROCKETNET_TEST_CREATE_TS ) == 0 );
    check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );
    attempt_edges_update_at_rest( c, 1010.0 );
    attempt_edges_update_at_rest( c, 1020.0 );

    attempt_edges_client_destroy( c );
}

REGISTER_UNIT_TEST( test_client_update_at_rest_changes_nothing );

// a send falls due once the send interval has elapsed, at exactly the
// interval included. A rate of 8 makes the interval, 0.125 seconds, exact on
// a clock at 1000 (the rate is kept across attempts, as connection reset
// keeps it).

static void test_client_send_falls_due_at_exactly_the_interval()
{
    struct attempt_edges_client_t * c = attempt_edges_client_create();
    c->client.send_rate = 8.0;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    attempt_edges_mint_valid( token_data );
    check( rocketnet_client_protocol_connect( &c->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
    check( c->client.send_rate == 8.0 );

    int bytes = 0;
    rocketnet_client_protocol_update( &c->client, 1000.0, c->packet, &bytes );
    check( bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    rocketnet_client_protocol_update( &c->client, 1000.0625, c->packet, &bytes );
    check( bytes == 0 );
    rocketnet_client_protocol_update( &c->client, 1000.125, c->packet, &bytes );
    check( bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    attempt_edges_client_destroy( c );
}

REGISTER_UNIT_TEST( test_client_send_falls_due_at_exactly_the_interval );

// t11 (the cookie paragraph): round-trip samples come from the challenge and
// the packets after it that open under the attempt's keys, never from the
// cookie. The cookie answers in 0.03 s, the challenge in 0.04 s and the
// first session packet, answering the connection response, in 0.01 s: the
// handshake estimate is 0.01. The accepted session packet is also recorded
// received in the delivery bookkeeping.

static void test_client_third_round_trip_is_sampled_and_recorded()
{
    const double delays[3] = { 0.03, 0.04, 0.01 };
    struct attempt_edges_pair_t * pair = attempt_edges_pair_create_answered( delays );

    check( pair->client.rtt_estimate > 0.0099 && pair->client.rtt_estimate < 0.0101 );
    check( pair->client.received_session_sequence > 0 );
    check( pair->client.ack.highest_received_sequence == pair->client.received_session_sequence );

    attempt_edges_pair_destroy( pair );
}

REGISTER_UNIT_TEST( test_client_third_round_trip_is_sampled_and_recorded );

// a disconnect before the session keys exist has nothing to seal: the burst
// is empty, and the count and wire size say so whatever the caller's
// variables held. At rest, and while requesting a cookie.

static void test_client_disconnect_before_keys_reports_an_empty_burst()
{
    struct attempt_edges_client_t * c = attempt_edges_client_create();
    uint8_t * burst = (uint8_t *) malloc( ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET );
    check( burst != NULL );

    for ( int i = 0; i < 2; i++ )
    {
        if ( i == 1 )
        {
            uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
            attempt_edges_mint_valid( token_data );
            check( rocketnet_client_protocol_connect( &c->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
            check( c->client.state > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
            check( c->client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
            check( c->client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        }

        int burst_count = 77;
        int burst_bytes = 77;
        rocketnet_client_protocol_disconnect( &c->client, burst, &burst_count, &burst_bytes );
        check( burst_count == 0 );
        check( burst_bytes == 0 );
        check( c->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    }

    free( burst );
    attempt_edges_client_destroy( c );
}

REGISTER_UNIT_TEST( test_client_disconnect_before_keys_reports_an_empty_burst );

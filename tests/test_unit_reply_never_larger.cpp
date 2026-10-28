/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// STANDARD.md, "No response may be larger than the packet that provoked it":
// the amplification table is normative, and every packet type is re-checked
// against it whenever any size changes. These tests send each provoking
// packet through the real server and client protocol objects, capture every
// byte the far side sends in answer, and compare lengths. Time is an argument
// and starts at 1000.0; there is no socket, thread or real clock.
//
// The first case walks every row of the table: request answered by cookie, by
// a fresh challenge and its stored resend, by denied with the server full, and
// by nothing when the token is refused; challenge answered by the client's
// response; response answered by the first keep-alive and by denied; cookie
// answered by nothing. Each row also states the table's exact size.
//
// The second case drives each unauthenticated type through truncation,
// extension, a wrong type byte, and fixed-seed random bytes of every length
// from one to the largest provoking packet, and asserts that no answer is
// longer than its provoker.

#include "test_unit_registry.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// Keep the table's normative sizes independent of the shared wire constants.
static_assert( ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES == 710, "STANDARD.md request size" );
static_assert( ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES == 38, "STANDARD.md cookie size" );
static_assert( ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES == 66, "STANDARD.md challenge size" );
static_assert( ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES == 50, "STANDARD.md response size" );
static_assert( ROCKETNET_CONNECTION_DENIED_WIRE_BYTES == 34, "STANDARD.md denied size" );
static_assert( ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES == 41, "STANDARD.md fresh keep-alive size" );

// ---------------------------------------------------------------------------
// The fixture: a server with the two token tables, one slot table and a small
// map, driven through the protocol objects with mocked time.
// ---------------------------------------------------------------------------

struct reply_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_address_t second_address;
    struct rocketnet_address_t third_address;
};

static void reply_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
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

static void reply_mint_token( uint8_t * token_data, uint64_t client_id,
                              uint64_t create, uint64_t expire, int32_t timeout )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    reply_fill_vector_fields( &signed_fields, &private_fields );
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = timeout;
    private_fields.client_id = client_id;
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void reply_server_setup( struct reply_server_t * test )
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

    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
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

    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );

    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test->second_address, "198.51.100.8:56789" ) );
    rocketnet_unit_check( rocketnet_address_parse( &test->third_address, "198.51.100.9:56789" ) );
}

static void reply_server_teardown( struct reply_server_t * test )
{
    rocketnet_counters_assert_conserved( &test->counters );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

static int reply_process_at( struct reply_server_t * test,
                             const struct rocketnet_address_t * from,
                             const uint8_t * packet, int packet_bytes,
                             uint8_t * response, int * response_bytes )
{
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    return rocketnet_server_protocol_process_packet( &test->server, from, packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                     response, response_bytes,
                                                     payload, &payload_bytes, &payload_client_index );
}

static void reply_build_request( uint8_t * request, uint64_t client_id, const uint8_t * salt,
                                 uint64_t create, uint64_t expire, int32_t timeout )
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    reply_mint_token( token_data, client_id, create, expire, timeout );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
}

static void reply_build_standard_request( uint8_t * request, uint64_t client_id, const uint8_t * salt )
{
    reply_build_request( request, client_id, salt, ROCKETNET_TEST_CREATE_TS,
                         ROCKETNET_TEST_EXPIRE_TS, ROCKETNET_TEST_TIMEOUT );
}

static void reply_pass_cookie_gate( struct reply_server_t * test, const struct rocketnet_address_t * from,
                                    uint8_t * request, uint8_t * cookie, int * cookie_bytes )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    rocketnet_unit_check( reply_process_at( test, from, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                            response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );

    memcpy( request + ROCKETNET_PREFILTER_BYTES,
            response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
            ROCKETNET_COOKIE_FIELD_BYTES );

    if ( cookie )
    {
        memcpy( cookie, response, (size_t) response_bytes );
    }

    if ( cookie_bytes )
    {
        *cookie_bytes = response_bytes;
    }
}

// the client's own half of the handshake, driven with time passed in: connect,
// answer the cookie, answer the challenge, and hand back the stored response
// the next scheduled update sends. the server answers through the real receive
// entry so the mapping and the client share one attempt.

static int reply_client_to_response( struct reply_server_t * test,
                                     struct rocketnet_client_protocol_t * client,
                                     struct rocketnet_counters_t * client_counters,
                                     uint64_t client_id,
                                     uint8_t * response_out )
{
    memset( client_counters, 0, sizeof( *client_counters ) );
    rocketnet_client_protocol_init( client, client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    reply_mint_token( token_data, client_id, ROCKETNET_TEST_CREATE_TS,
                      ROCKETNET_TEST_EXPIRE_TS, ROCKETNET_TEST_TIMEOUT );
    rocketnet_unit_check( rocketnet_client_protocol_connect( client, token_data, 1000.0,
                                                             ROCKETNET_TEST_CREATE_TS ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    int challenge_bytes = 0;

    for ( int step = 0; step < 20; step++ )
    {
        int packet_bytes = 0;
        rocketnet_client_protocol_update( client, 1000.0 + 0.1 * step, packet, &packet_bytes );

        if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE &&
             packet_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES )
        {
            rocketnet_unit_check( challenge_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

            memcpy( response_out, packet, (size_t) packet_bytes );
            return packet_bytes;
        }

        if ( packet_bytes > 0 )
        {
            int payload_bytes = 0;
            int payload_client_index = -1;
            int result = rocketnet_server_protocol_process_packet( &test->server, &test->client_address,
                                                                   packet, packet_bytes,
                                                                   ROCKETNET_TEST_CREATE_TS, 1000.0 + 0.1 * step,
                                                                   response, &response_bytes,
                                                                   payload, &payload_bytes, &payload_client_index );
            (void) result;

            if ( response_bytes > 0 && response[0] == ROCKETNET_CONNECTION_CHALLENGE_PACKET )
            {
                challenge_bytes = response_bytes;
            }

            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( client, response, response_bytes,
                                                          1000.0 + 0.1 * step,
                                                          payload, sizeof( payload ), &payload_bytes );
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Every row of the amplification table, provoked through the protocol objects.
// ---------------------------------------------------------------------------

static void test_reply_never_larger_than_request_every_unauthenticated_type()
{
    struct reply_server_t test;
    reply_server_setup( &test );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    // request -> cookie. 710 provoking, 38 answering

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    reply_build_standard_request( request, 0xA100ULL, rocketnet_test_salt );
    rocketnet_unit_check( reply_process_at( &test, &test.client_address, request, sizeof( request ),
                                            response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    // the same request again, now carrying the cookie -> a fresh challenge

    memcpy( request + ROCKETNET_PREFILTER_BYTES,
            response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
            ROCKETNET_COOKIE_FIELD_BYTES );
    rocketnet_unit_check( reply_process_at( &test, &test.client_address, request, sizeof( request ),
                                            response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    uint8_t stored_challenge[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    memcpy( stored_challenge, response, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    // the stored challenge is resent byte-identical for as long as the mapping
    // lives, still 66 bytes against the 710-byte request.

    rocketnet_unit_check( reply_process_at( &test, &test.client_address, request, sizeof( request ),
                                            response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    rocketnet_unit_check( memcmp( stored_challenge, response, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES ) == 0 );
    rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    // a refused token: the expired token is refused before the cookie is
    // issued, and STANDARD.md's rule that a server never answers a request it
    // has rejected leaves the response empty. 710 provoking, 0 answering.
    {
        uint8_t refused_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        reply_build_request( refused_request, 0xA200ULL, rocketnet_test_salt,
                             ROCKETNET_TEST_CREATE_TS - 20, ROCKETNET_TEST_CREATE_TS - 10,
                             ROCKETNET_TEST_TIMEOUT );
        rocketnet_unit_check( reply_process_at( &test, &test.third_address, refused_request, sizeof( refused_request ),
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( response_bytes == 0 );
        rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    }

    // request -> denied because the server is full. 710 provoking, 34
    // answering, and no mapping is created.
    {
        uint8_t denied_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        reply_build_standard_request( denied_request, 0xB200ULL, rocketnet_test_salt );
        reply_pass_cookie_gate( &test, &test.second_address, denied_request, NULL, NULL );

        uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
        memset( user_data, 0, sizeof( user_data ) );
        for ( int i = 0; i < 4; i++ )
            rocketnet_unit_check( rocketnet_client_slots_assign( test.server.client_slots, 0xF200 + (uint64_t) i, -1,
                                                                 0xF200 + (uint64_t) i, user_data ) == i );

        rocketnet_unit_check( reply_process_at( &test, &test.second_address, denied_request, sizeof( denied_request ),
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_DENIED );
        rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );
        rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

        for ( int i = 0; i < 4; i++ )
        {
            rocketnet_client_slots_release( test.server.client_slots, i );
        }
    }

    // cookie -> nothing. the cookie is server-to-client only, so a server sees
    // a reflection and ignores it. 38 provoking, 0 answering.
    {
        uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
        int cookie_bytes = 0;
        uint8_t cookie_request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        reply_build_standard_request( cookie_request, 0xC200ULL, rocketnet_test_salt );
        reply_pass_cookie_gate( &test, &test.third_address, cookie_request, cookie, &cookie_bytes );
        rocketnet_unit_check( cookie_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );

        rocketnet_unit_check( reply_process_at( &test, &test.third_address, cookie, cookie_bytes,
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( response_bytes == 0 );
        rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    }

    reply_server_teardown( &test );

    // challenge -> the client's response. 66 provoking, 50 answering
    {
        struct reply_server_t keep_alive_test;
        reply_server_setup( &keep_alive_test );

        struct rocketnet_client_protocol_t client;
        struct rocketnet_counters_t client_counters;
        uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
        int client_response_bytes = reply_client_to_response( &keep_alive_test, &client, &client_counters,
                                                              0xD200ULL, client_response );
        rocketnet_unit_check( client_response_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
        rocketnet_unit_check( client_response_bytes <= ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

        // response -> the first keep-alive, fresh block. 50 provoking, 41
        // answering.

        rocketnet_unit_check( reply_process_at( &keep_alive_test, &keep_alive_test.client_address,
                                                client_response, client_response_bytes,
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
        rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
        rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

        rocketnet_counters_assert_conserved( &client_counters );
        reply_server_teardown( &keep_alive_test );
    }

    // response -> denied because the server is full. 50 provoking, 34
    // answering; the mapping stays for the retry.
    {
        struct reply_server_t denied_test;
        reply_server_setup( &denied_test );

        struct rocketnet_client_protocol_t client;
        struct rocketnet_counters_t client_counters;
        uint8_t client_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
        int client_response_bytes = reply_client_to_response( &denied_test, &client, &client_counters,
                                                              0xE200ULL, client_response );
        rocketnet_unit_check( client_response_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

        uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
        memset( user_data, 0, sizeof( user_data ) );
        for ( int i = 0; i < 4; i++ )
            rocketnet_unit_check( rocketnet_client_slots_assign( denied_test.server.client_slots, 0xF300 + (uint64_t) i, -1,
                                                                 0xF300 + (uint64_t) i, user_data ) == i );

        rocketnet_unit_check( reply_process_at( &denied_test, &denied_test.client_address,
                                                client_response, client_response_bytes,
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_DENIED );
        rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );
        rocketnet_unit_check( response_bytes <= ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

        rocketnet_counters_assert_conserved( &client_counters );
        reply_server_teardown( &denied_test );
    }
}

// ---------------------------------------------------------------------------
// Hostile provokers: truncate, extend, change the type byte, and send fixed
// seed random bytes at every length. No answer may outgrow its provoker.
// ---------------------------------------------------------------------------

static uint64_t reply_rng_state;

static uint8_t reply_rng_next()
{
    reply_rng_state ^= reply_rng_state >> 12;
    reply_rng_state ^= reply_rng_state << 25;
    reply_rng_state ^= reply_rng_state >> 27;
    return (uint8_t) ( ( reply_rng_state * 0x2545F4914F6CDD1DULL ) >> 56 );
}

static void reply_fill_random( uint8_t * bytes, int count )
{
    for ( int i = 0; i < count; i++ )
    {
        bytes[i] = reply_rng_next();
    }
}

static void reply_write_header( uint8_t * packet, int packet_bytes, uint8_t type )
{
    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES )
        return;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, type, tag );
}

static void test_reply_never_larger_than_request_hostile_inputs()
{
    static const uint8_t types[] = {
        ROCKETNET_CONNECTION_REQUEST_PACKET,
        ROCKETNET_CONNECTION_DENIED_PACKET,
        ROCKETNET_CONNECTION_CHALLENGE_PACKET,
        ROCKETNET_CONNECTION_RESPONSE_PACKET,
        ROCKETNET_CONNECTION_COOKIE_PACKET,
    };
    static const int sizes[] = {
        ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
        ROCKETNET_CONNECTION_DENIED_WIRE_BYTES,
        ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
        ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES,
        ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES,
    };
    const int type_count = (int) ( sizeof( types ) / sizeof( types[0] ) );

    struct reply_server_t test;
    reply_server_setup( &test );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    for ( int t = 0; t < type_count; t++ )
    {
        const int size = sizes[t];
        const uint8_t type = types[t];
        const uint8_t wrong_type = type == ROCKETNET_CONNECTION_REQUEST_PACKET ? ROCKETNET_CONNECTION_RESPONSE_PACKET : ROCKETNET_CONNECTION_REQUEST_PACKET;

        uint8_t packet[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + 2];
        memset( packet, 0, sizeof( packet ) );
        reply_write_header( packet, size, type );

        // truncated by one byte: no answer, and never a longer one

        rocketnet_unit_check( reply_process_at( &test, &test.client_address, packet, size - 1,
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( response_bytes == 0 );
        rocketnet_unit_check( response_bytes <= size - 1 );

        // extended by one byte: no answer, and never a longer one

        memset( packet, 0, sizeof( packet ) );
        reply_write_header( packet, size + 1, type );
        rocketnet_unit_check( reply_process_at( &test, &test.client_address, packet, size + 1,
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( response_bytes == 0 );
        rocketnet_unit_check( response_bytes <= size + 1 );

        // a wrong type byte at the type's own size: no answer, and never a
        // longer one.

        memset( packet, 0, sizeof( packet ) );
        reply_write_header( packet, size, wrong_type );
        rocketnet_unit_check( reply_process_at( &test, &test.client_address, packet, size,
                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( response_bytes == 0 );
        rocketnet_unit_check( response_bytes <= size );
    }

    // fixed-seed random bytes at every length from one to the largest
    // provoking size, with the type and filter header set so each length
    // reaches its type's reader. No answer may be longer than its provoker.

    reply_rng_state = 0x9E3779B97F4A7C15ULL;

    for ( int t = 0; t < type_count; t++ )
    {
        const uint8_t type = types[t];
        for ( int length = 1; length <= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES; length++ )
        {
            uint8_t packet[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
            reply_fill_random( packet, length );
            reply_write_header( packet, length, type );

            reply_process_at( &test, &test.client_address, packet, length, response, &response_bytes );
            rocketnet_unit_check( response_bytes <= length );
        }
    }

    reply_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_reply_never_larger_than_request_every_unauthenticated_type );
REGISTER_UNIT_TEST( test_reply_never_larger_than_request_hostile_inputs );

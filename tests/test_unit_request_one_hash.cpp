/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A connection request pays one keyed address hash on the receive path: the
// transport looks the source up once and hands the answer to
// rocketnet_server_protocol_process_received, and the request ladder's
// existing-mapping rung takes that answer instead of looking the address up
// again. Counted through the encryption map's DEV hash count, for three
// shapes: a first-contact request with no cookie, a cookie-valid request
// whose signature fails, and a valid request, whose new mapping adds the one
// hash of its insert. The protocol runs on its own, time passed as 1000.0, no
// socket.

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
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV

#define REQUEST_ONE_HASH_REQUESTS 8

struct request_one_hash_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
};

static void request_one_hash_setup( struct request_one_hash_server_t * test )
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
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 4 * REQUEST_ONE_HASH_REQUESTS );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 4 * REQUEST_ONE_HASH_REQUESTS );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 * REQUEST_ONE_HASH_REQUESTS );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &test->counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, 1000.0 );
}

static void request_one_hash_teardown( struct request_one_hash_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_reassembly_set_destroy( test->server.reassembly );
}

static void request_one_hash_address( struct rocketnet_address_t * address, int i )
{
    char text[64];
    snprintf( text, sizeof( text ), "10.7.%d.%d:56789", ( i / 250 ) + 1, ( i % 250 ) + 1 );
    check( rocketnet_address_parse( address, text ) );
}

// a request around a fresh token: with no cookie (first contact), or with a
// cookie valid for this address and a signature that verifies or not

enum
{
    REQUEST_NO_COOKIE,
    REQUEST_FORGED,
    REQUEST_VALID
};

static void request_one_hash_build( uint8_t * request, int index, const struct rocketnet_address_t * address, int shape )
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
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
    if ( shape == REQUEST_NO_COOKIE )
        return;

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    check( rocketnet_generate_cookie_mac( cookie_mac, address, rocketnet_test_salt, cookie_expire, rocketnet_test_cookie_key ) );
    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

    if ( shape == REQUEST_FORGED )
    {
        const int signature_offset = ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES +
                                     ROCKETNET_KEY_BYTES + ROCKETNET_SIGNED_CONNECT_DATA_BYTES;
        request[signature_offset] ^= 1;
    }
}

// the transport's receive of one request: one lookup, then process_received
// with its answer. Returns the keyed hashes the whole receive computed.

static uint64_t request_one_hash_receive( struct request_one_hash_server_t * test, const struct rocketnet_address_t * address,
                                          const uint8_t * request, int * result )
{
    struct rocketnet_encryption_map_t * map = test->server.encryption_map;
    const uint64_t before = rocketnet_encryption_map_test_hashes( map );

    const int known = rocketnet_encryption_map_find( map, address );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    static uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    int payload_client_index = -1;
    *result = rocketnet_server_protocol_process_received( &test->server, address, known, request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                          ROCKETNET_TEST_CREATE_TS, 1000.0, response, &response_bytes, opened,
                                                          ROCKETNET_RECEIVE_CEILING, &payload, &payload_bytes, &payload_client_index );

    return rocketnet_encryption_map_test_hashes( map ) - before;
}

static void test_server_request_one_hash()
{
    struct request_one_hash_server_t test;
    request_one_hash_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    int result = -1;

    // first contact, no cookie: the transport's lookup is the only hash

    for ( int i = 0; i < REQUEST_ONE_HASH_REQUESTS; i++ )
    {
        struct rocketnet_address_t address;
        request_one_hash_address( &address, i );
        request_one_hash_build( request, i, &address, REQUEST_NO_COOKIE );
        check( request_one_hash_receive( &test, &address, request, &result ) == 1 );
        check( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    }

    // a valid cookie and a signature that fails: still the one hash

    for ( int i = 0; i < REQUEST_ONE_HASH_REQUESTS; i++ )
    {
        struct rocketnet_address_t address;
        request_one_hash_address( &address, 100 + i );
        request_one_hash_build( request, 100 + i, &address, REQUEST_FORGED );
        check( request_one_hash_receive( &test, &address, request, &result ) == 1 );
        check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    }

    // a valid request: the lookup, and the insert of its new mapping. The
    // same request again finds that mapping on the rung, from the transport's
    // lookup, and is answered with the stored challenge: one hash

    for ( int i = 0; i < REQUEST_ONE_HASH_REQUESTS; i++ )
    {
        struct rocketnet_address_t address;
        request_one_hash_address( &address, 200 + i );
        request_one_hash_build( request, 200 + i, &address, REQUEST_VALID );
        check( request_one_hash_receive( &test, &address, request, &result ) == 2 );
        check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        check( request_one_hash_receive( &test, &address, request, &result ) == 1 );
        check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    }

    request_one_hash_teardown( &test );
}

REGISTER_UNIT_TEST( test_server_request_one_hash );

#endif // #if ROCKETNET_DEV

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_gaps_secrets_stack.cpp -- the secrets a function stages on its own
// stack are wiped before it returns. Each such function hands its staging to
// the stack secret seam (sdk/rocketnet_test_seams.h, ROCKETNET_STACK_SECRET_SEEN)
// right after its wipe; the hook here copies what the frame still holds, and
// every case checks the copy is all zero:
//
//   - the token writer's private block and private data, on the written exit
//     and on the failed seal's exit (sdk/rocketnet_connect_token.cpp);
//   - the production writer's private fields (both session keys) and the
//     development token's config (the published backend secret key);
//   - the server's opened private data, its handshake and session keys and
//     its ephemeral secret on a request that is answered with a challenge,
//     with a full server's denial, and with a small-order client key's
//     refusal (sdk/rocketnet_server_protocol.cpp).
//
// The seam is development only. No socket, no clock: the server protocol is
// driven directly with time as an argument from 100.0.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

// a check whose failure names the fault before the condition is printed
#define gaps_stack_check( condition, fault )                                                                      \
do                                                                                                                \
{                                                                                                                 \
    if ( !( condition ) )                                                                                         \
    {                                                                                                             \
        printf( "mutation gap fault: %s\n", fault );                                                              \
        fflush( stdout );                                                                                         \
    }                                                                                                             \
    rocketnet_unit_check( condition );                                                                            \
} while ( 0 )

#define GAPS_STACK_SITES 12

static int gaps_stack_seen[GAPS_STACK_SITES];
static int gaps_stack_left[GAPS_STACK_SITES];      // nonzero bytes the staging still held, summed over the calls

static void gaps_stack_hook( int site, const void * staging, int bytes )
{
    rocketnet_unit_check( site > 0 && site < GAPS_STACK_SITES && bytes > 0 );

    const uint8_t * p = (const uint8_t *) staging;
    for ( int i = 0; i < bytes; i++ )
    {
        gaps_stack_left[site] += p[i] != 0;
    }
    gaps_stack_seen[site]++;
}

static void gaps_stack_arm()
{
    memset( gaps_stack_seen, 0, sizeof( gaps_stack_seen ) );
    memset( gaps_stack_left, 0, sizeof( gaps_stack_left ) );
    rocketnet_stack_secret_hook = gaps_stack_hook;
}

static void gaps_stack_disarm()
{
    rocketnet_stack_secret_hook = NULL;
}

// ---------------------------------------------------------------------------
// the token writers
// ---------------------------------------------------------------------------

static void gaps_stack_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
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

// mutation gap: hunt 2/121 hunt 2/122
static void test_gaps_secrets_stack_token_writer()
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    gaps_stack_vector_fields( &signed_fields, &private_fields, 0x51AC0001ULL );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    // written

    gaps_stack_arm();
    const int written = rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token );
    gaps_stack_disarm();

    rocketnet_unit_check( written == 1 );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_BLOCK] == 1 && gaps_stack_seen[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_DATA] == 1 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_BLOCK] == 0, "the token writer left the private block (the client's session keys) on its stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_DATA] == 0, "the token writer left its private data copy (the client's session keys) on its stack" );

    // the seal refused: a small-order (all-zero) server key

    memset( signed_fields.server_public_key, 0, sizeof( signed_fields.server_public_key ) );
    gaps_stack_arm();
    const int refused = rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token );
    gaps_stack_disarm();

    rocketnet_unit_check( refused == 0 );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_BLOCK] == 1 && gaps_stack_seen[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_DATA] == 1 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_BLOCK] == 0, "the token writer's failed-seal exit left the private block on its stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_TOKEN_PRIVATE_DATA] == 0, "the token writer's failed-seal exit left its private data copy on its stack" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_stack_token_writer );

// mutation gap: hunt 2/228
static void test_gaps_secrets_stack_production_writer()
{
    uint8_t backend_public[ROCKETNET_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );

    const char * addresses[] = { "127.0.0.1:40000" };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    gaps_stack_arm();
    const enum rocketnet_status_t status = rocketnet_write_production_connect_token( backend_secret, server_public, 0x6A9500CE7ULL, addresses, 1,
                                                                                     0x51AC0002ULL, 1000, 1ULL << 40, 300, NULL,
                                                                                     token, sizeof( token ) );
    gaps_stack_disarm();

    rocketnet_unit_check( status == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_PRODUCTION_PRIVATE_FIELDS] == 1 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_PRODUCTION_PRIVATE_FIELDS] == 0, "the production token writer left its private fields (both session keys) on its stack" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_stack_production_writer );

// mutation gap: hunt 2/624
static void test_gaps_secrets_stack_dev_token_config()
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];

    gaps_stack_arm();
    const enum rocketnet_status_t status = rocketnet_create_dev_connect_token( "127.0.0.1:40000", NULL, rocketnet_dev_server_public_key, 30, 10,
                                                                               0x51AC0003ULL, ROCKETNET_DEV_PROTOCOL_ID, 1000000, NULL, token );
    gaps_stack_disarm();

    rocketnet_unit_check( status == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_DEV_TOKEN_CONFIG] == 1 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_DEV_TOKEN_CONFIG] == 0, "the development token writer left its config (the backend secret key) on its stack" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_stack_dev_token_config );

// ---------------------------------------------------------------------------
// the server's connection request
// ---------------------------------------------------------------------------

static const double GAPS_STACK_START_TIME = 100.0;

struct gaps_stack_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static struct gaps_stack_server_t gaps_stack_test;

static void gaps_stack_server_setup( struct gaps_stack_server_t * test, int max_clients )
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
    server->cookie_rotation_seconds = 0;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, max_clients );
    server->acks = rocketnet_server_acks_create( NULL, max_clients );
    server->counters = &test->counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );

    rocketnet_server_protocol_update( server, GAPS_STACK_START_TIME );

    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void gaps_stack_server_teardown( struct gaps_stack_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
    memset( test, 0, sizeof( *test ) );
}

// a request with a cookie under the server's current key, so it passes the
// cookie gate and reaches the open
static void gaps_stack_request( struct gaps_stack_server_t * test, uint8_t * request, uint64_t client_id, const uint8_t * client_ephemeral_public )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    gaps_stack_vector_fields( &signed_fields, &private_fields, client_id );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    rocketnet_unit_check( rocketnet_generate_cookie_mac( cookie_mac, &test->client_address, rocketnet_test_salt, cookie_expire, test->server.cookie_key ) );

    uint8_t return_cookie[ROCKETNET_COOKIE_FIELD_BYTES];
    uint8_t * p = return_cookie;
    rocketnet_write_uint64( &p, cookie_expire );
    rocketnet_write_bytes( &p, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, return_cookie, rocketnet_test_salt,
                                               client_ephemeral_public,
                                               token.signed_connect_data, token.signature );
}

static int gaps_stack_process( struct gaps_stack_server_t * test, const uint8_t * request )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    gaps_stack_arm();
    const int result = rocketnet_server_protocol_process_connection_request( &test->server, &test->client_address, request,
                                                                             ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, ROCKETNET_TEST_CREATE_TS,
                                                                             GAPS_STACK_START_TIME, response, &response_bytes );
    gaps_stack_disarm();

    return result;
}

// mutation gap: hunt 2/1393 hunt 2/1432 hunt 2/1459
static void test_gaps_secrets_stack_request_challenged()
{
    struct gaps_stack_server_t * test = &gaps_stack_test;
    gaps_stack_server_setup( test, 4 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    gaps_stack_request( test, request, 0x51AC0101ULL, rocketnet_test_client_eph_public );
    const int result = gaps_stack_process( test, request );

    gaps_stack_server_teardown( test );

    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_PRIVATE_DATA] == 1 );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_EPHEMERAL] == 1 );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_KEYS] == 1 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_PRIVATE_DATA] == 0, "a challenged request left the opened private data (the token's session keys) on the server's stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_EPHEMERAL] == 0, "a challenged request left the server's ephemeral secret on its stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_KEYS] == 0, "a challenged request left the connection keys on the server's stack after the mapping took them" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_stack_request_challenged );

// mutation gap: hunt 2/1414
static void test_gaps_secrets_stack_request_denied_full()
{
    struct gaps_stack_server_t * test = &gaps_stack_test;
    gaps_stack_server_setup( test, 1 );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );
    rocketnet_unit_check( rocketnet_client_slots_assign( test->server.client_slots, 0x51AC0F00ULL, -1, 1, user_data ) == 0 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    gaps_stack_request( test, request, 0x51AC0102ULL, rocketnet_test_client_eph_public );
    const int result = gaps_stack_process( test, request );

    gaps_stack_server_teardown( test );

    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_SEND_DENIED );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_KEYS_DENIED] == 1 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_KEYS_DENIED] == 0, "a full server's denial left the handshake keys on its stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_PRIVATE_DATA] == 0, "a full server's denial left the opened private data on its stack" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_stack_request_denied_full );

// mutation gap: hunt 2/1428 hunt 2/1429
static void test_gaps_secrets_stack_request_small_order()
{
    struct gaps_stack_server_t * test = &gaps_stack_test;
    gaps_stack_server_setup( test, 4 );

    uint8_t small_order[ROCKETNET_KEY_BYTES];
    memset( small_order, 0, sizeof( small_order ) );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    gaps_stack_request( test, request, 0x51AC0103ULL, small_order );
    const int result = gaps_stack_process( test, request );
    const uint64_t small_order_refusals = test->counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY];

    gaps_stack_server_teardown( test );

    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED && small_order_refusals == 1 );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_EPHEMERAL_SMALL_ORDER] == 1 && gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_KEYS_SMALL_ORDER] == 1 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_EPHEMERAL_SMALL_ORDER] == 0, "a small-order client key's refusal left the server's ephemeral secret on its stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_KEYS_SMALL_ORDER] == 0, "a small-order client key's refusal left the handshake keys on the server's stack" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_stack_request_small_order );

// mutation gap: hunt 2/1443
// The add-failure exit: the request passes every rung, the keys are derived,
// and the encryption map refuses the add (the DEV add failure seam), so the
// server ignores the request and must leave no keys on its stack.
static void test_gaps_secrets_stack_request_not_mapped()
{
    struct gaps_stack_server_t * test = &gaps_stack_test;
    gaps_stack_server_setup( test, 4 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    gaps_stack_request( test, request, 0x51AC0104ULL, rocketnet_test_client_eph_public );

    rocketnet_dev_fail_encryption_map_adds( 1 );
    const int result = gaps_stack_process( test, request );
    rocketnet_dev_fail_encryption_map_adds( 0 );

    const uint64_t could_not_add = test->counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING];
    const int num_mappings = rocketnet_encryption_map_num_mappings( test->server.encryption_map );

    gaps_stack_server_teardown( test );

    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED && could_not_add == 1 && num_mappings == 0 );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_KEYS_NOT_MAPPED] == 1 );
    rocketnet_unit_check( gaps_stack_seen[ROCKETNET_STACK_SECRET_REQUEST_KEYS] == 0 );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_KEYS_NOT_MAPPED] == 0, "a request the encryption map could not take left the connection keys on the server's stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_EPHEMERAL] == 0, "a request the encryption map could not take left the server's ephemeral secret on its stack" );
    gaps_stack_check( gaps_stack_left[ROCKETNET_STACK_SECRET_REQUEST_PRIVATE_DATA] == 0, "a request the encryption map could not take left the opened private data on its stack" );
}

REGISTER_UNIT_TEST( test_gaps_secrets_stack_request_not_mapped );

#endif // #if ROCKETNET_DEV

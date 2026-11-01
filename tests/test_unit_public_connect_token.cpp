/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_public_connect_token.cpp -- the public keygen and production
// connect-token minting in sdk/rocketnet.h, checked against the internal
// reader, verifier and opener. Every case carries its own timestamps: no case
// reads a clock, opens a socket or touches anything outside the process.

#include "test_unit_registry.h"
#include "../sdk/rocketnet_base64.h"

#include "rocketnet_internal.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

static const uint64_t public_token_protocol_id = 0x1122334455667788ULL;
static const uint64_t public_token_other_protocol_id = 0x8877665544332211ULL;
static const uint64_t public_token_client_id = 0x0123456789abcdefULL;
static const uint64_t public_token_create_timestamp = 1756166400ULL;
static const uint64_t public_token_expire_timestamp = 1756166700ULL;
static const int32_t public_token_timeout_seconds = 10;

static const char * const public_token_addresses[] = {
    "203.0.113.10:40000",
    "[2001:db8::1]:65535",
};

static const char * const public_token_one_address[] = {
    "203.0.113.10:40000",
};

static void public_token_fill_user_data( uint8_t * user_data )
{
    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
    {
        user_data[i] = (uint8_t) ( i * 7 + 3 );
    }
}

static void public_token_make_keys( uint8_t * backend_public_key, uint8_t * backend_secret_key,
                                    uint8_t * server_public_key, uint8_t * server_secret_key )
{
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );
}

static int public_token_mint( const uint8_t * backend_secret_key,
                              const uint8_t * server_public_key,
                              uint64_t protocol_id,
                              uint8_t * token,
                              size_t token_bytes )
{
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    public_token_fill_user_data( user_data );
    return rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id,
                                                     public_token_addresses, 2,
                                                     public_token_client_id, public_token_create_timestamp,
                                                     public_token_expire_timestamp, public_token_timeout_seconds,
                                                     user_data, token, token_bytes );
}

// a server protocol with the minting keys, a different protocol id and only
// the tables the request path touches before the protocol id check

struct public_token_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
};

static void public_token_server_setup( struct public_token_server_t * t, uint64_t protocol_id,
                                       const uint8_t * backend_public_key,
                                       const uint8_t * server_public_key,
                                       const uint8_t * server_secret_key )
{
    memset( t, 0, sizeof( *t ) );
    struct rocketnet_server_protocol_t * server = &t->server;
    server->protocol_id = protocol_id;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_public_key, server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_secret_key, server_secret_key, ROCKETNET_KEY_BYTES );
    server->encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 8 );
    server->client_slots = rocketnet_client_slots_create( NULL, 2 );
    server->counters = &t->counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history && server->client_slots );
}

static void public_token_server_teardown( struct public_token_server_t * t )
{
    rocketnet_encryption_map_destroy( t->server.encryption_map );
    rocketnet_used_token_history_destroy( t->server.used_token_history );
    rocketnet_client_slots_destroy( t->server.client_slots );
}

// the first connection request the client protocol sends for a token, built
// on mocked time; the caller passes it to a server protocol directly

static int public_token_connection_request( const uint8_t * token, uint8_t * packet, int * packet_bytes )
{
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    rocketnet_client_protocol_init( &client, &counters );

    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, token, 1000.0,
                                                             public_token_create_timestamp ) == 1 );

    rocketnet_client_protocol_update( &client, 1000.0, packet, packet_bytes );

    return *packet_bytes;
}

void test_public_connect_token_round_trip()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, token );

    rocketnet_unit_check( rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature,
                                                                backend_public_key ) == 1 );

    struct rocketnet_signed_connect_data_t signed_fields;
    rocketnet_read_signed_connect_data( &signed_fields, parsed.signed_connect_data );
    rocketnet_unit_check( signed_fields.version == ROCKETNET_WIRE_VERSION );
    rocketnet_unit_check( signed_fields.protocol_id == public_token_protocol_id );
    rocketnet_unit_check( signed_fields.create_timestamp == public_token_create_timestamp );
    rocketnet_unit_check( signed_fields.expire_timestamp == public_token_expire_timestamp );
    rocketnet_unit_check( signed_fields.timeout_seconds == public_token_timeout_seconds );
    rocketnet_unit_check( signed_fields.address_flags ==
                          ( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) );
    rocketnet_unit_check( memcmp( signed_fields.server_public_key, server_public_key, ROCKETNET_KEY_BYTES ) == 0 );

    static const uint8_t expected_ipv4[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    static const uint8_t expected_ipv6[18] = {
        0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0xff, 0xff
    };
    rocketnet_unit_check( memcmp( signed_fields.ipv4_address, expected_ipv4, sizeof( expected_ipv4 ) ) == 0 );
    rocketnet_unit_check( memcmp( signed_fields.ipv6_address, expected_ipv6, sizeof( expected_ipv6 ) ) == 0 );

    struct rocketnet_connect_token_private_t opened;
    rocketnet_unit_check( rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data,
                                                                server_public_key, server_secret_key ) == 1 );
    rocketnet_unit_check( opened.client_id == public_token_client_id );
    rocketnet_unit_check( opened.protocol_id == public_token_protocol_id );
    rocketnet_unit_check( opened.expire_timestamp == public_token_expire_timestamp );
    rocketnet_unit_check( rocketnet_connect_token_private_consistent( &opened, parsed.signed_connect_data ) == 1 );
    rocketnet_unit_check( memcmp( opened.client_to_server_key, parsed.client_to_server_key, ROCKETNET_KEY_BYTES ) == 0 );
    rocketnet_unit_check( memcmp( opened.server_to_client_key, parsed.server_to_client_key, ROCKETNET_KEY_BYTES ) == 0 );

    uint8_t expected_user_data[ROCKETNET_USER_DATA_BYTES];
    public_token_fill_user_data( expected_user_data );
    rocketnet_unit_check( memcmp( opened.user_data, expected_user_data, ROCKETNET_USER_DATA_BYTES ) == 0 );
}

void test_public_connect_token_wrong_server_key_refused()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t other_server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t other_server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_unit_check( rocketnet_generate_server_keypair( other_server_public_key, other_server_secret_key ) == ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, token );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature,
                                                                backend_public_key ) == 1 );

    struct rocketnet_connect_token_private_t opened;
    rocketnet_unit_check( rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data,
                                                                server_public_key, server_secret_key ) == 1 );
    rocketnet_unit_check( rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data,
                                                                other_server_public_key, other_server_secret_key ) == 0 );
}

void test_public_connect_token_wrong_protocol_id_refused()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, 0,
                                             token, sizeof( token ) ) == ROCKETNET_STATUS_INVALID_PROTOCOL_ID );

    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    // the same keys, a server listening for a different protocol id: the
    // request is ignored before any crypto runs and the counter names why

    struct public_token_server_t server;
    public_token_server_setup( &server, public_token_other_protocol_id, backend_public_key,
                               server_public_key, server_secret_key );

    uint8_t request[ROCKETNET_RECEIVE_CEILING];
    int request_bytes = 0;
    rocketnet_unit_check( public_token_connection_request( token, request, &request_bytes ) > 0 );

    struct rocketnet_address_t from;
    rocketnet_unit_check( rocketnet_address_parse( &from, "198.51.100.7:56789" ) );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    int payload_bytes = 0;
    int payload_client_index = -1;
    const int result = rocketnet_server_protocol_process_packet( &server.server, &from, request, request_bytes,
                                                                 public_token_create_timestamp, 1000.0,
                                                                 response, &response_bytes,
                                                                 payload, &payload_bytes, &payload_client_index );
    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( response_bytes == 0 );
    rocketnet_unit_check( server.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID] == 1 );

    public_token_server_teardown( &server );
}

#if ROCKETNET_ASSERTS
// The NULL calls below hit rocketnet_api_null_guard, which fires
// rocketnet_assert in a build with asserts (Debug) before it refuses. Count
// those expected asserts instead of trapping, as test_unit_counter_classes.cpp
// does; each NULL call must assert exactly once.
static int public_token_expected_null_asserts = 0;
static void public_token_expected_null_assert( const char *, const char *, const char *, int )
{
    public_token_expected_null_asserts++;
}
#endif // #if ROCKETNET_ASSERTS

void test_public_connect_token_null_or_short_buffer_refused()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    public_token_fill_user_data( user_data );

#if ROCKETNET_ASSERTS
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    public_token_expected_null_asserts = 0;
    rocketnet_assert_function( public_token_expected_null_assert );
#endif // #if ROCKETNET_ASSERTS
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_addresses, 2,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, NULL,
                                                                    ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
#if ROCKETNET_ASSERTS
    rocketnet_assert_function( saved_assert );

    rocketnet_unit_check( public_token_expected_null_asserts == 1 );
#endif // #if ROCKETNET_ASSERTS

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0xa5, sizeof( token ) );
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_addresses, 2,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    ROCKETNET_CONNECT_TOKEN_BYTES - 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    for ( int i = 0; i < (int) sizeof( token ); i++ )
    {
        rocketnet_unit_check( token[i] == 0xa5 );
    }

#if ROCKETNET_ASSERTS
    saved_assert = rn_assert_function;
    public_token_expected_null_asserts = 0;
    rocketnet_assert_function( public_token_expected_null_assert );
#endif // #if ROCKETNET_ASSERTS
    rocketnet_unit_check( rocketnet_write_production_connect_token( NULL, server_public_key,
                                                                    public_token_protocol_id, public_token_one_address, 1,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    sizeof( token ) ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
#if ROCKETNET_ASSERTS
    rocketnet_assert_function( saved_assert );

    rocketnet_unit_check( public_token_expected_null_asserts == 1 );
#endif // #if ROCKETNET_ASSERTS

    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_one_address, 0,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    sizeof( token ) ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_one_address, 1,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    sizeof( token ) ) == ROCKETNET_STATUS_OK );
}

// two mints of the same inputs draw fresh sealing and session material: a
// fixed ephemeral key would repeat the sealed block, tied to every token the
// backend ever minted for that server

void test_public_connect_token_sealed_block_varies()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t token_a[ROCKETNET_CONNECT_TOKEN_BYTES];
    uint8_t token_b[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token_a, sizeof( token_a ) ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token_b, sizeof( token_b ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_connect_token_t parsed_a;
    struct rocketnet_connect_token_t parsed_b;
    rocketnet_read_connect_token( &parsed_a, token_a );
    rocketnet_read_connect_token( &parsed_b, token_b );

    struct rocketnet_signed_connect_data_t fields_a;
    struct rocketnet_signed_connect_data_t fields_b;
    rocketnet_read_signed_connect_data( &fields_a, parsed_a.signed_connect_data );
    rocketnet_read_signed_connect_data( &fields_b, parsed_b.signed_connect_data );

    rocketnet_unit_check( memcmp( fields_a.sealed_private_block, fields_b.sealed_private_block,
                                  ROCKETNET_SEALED_BLOCK_BYTES ) != 0 );

    // the sealed block opens with the ephemeral public key, so a fixed
    // ephemeral key shows here even though the session keys still differ

    rocketnet_unit_check( memcmp( fields_a.sealed_private_block, fields_b.sealed_private_block,
                                  ROCKETNET_KEY_BYTES ) != 0 );
    rocketnet_unit_check( memcmp( token_a, token_b, ROCKETNET_CONNECT_TOKEN_BYTES ) != 0 );
}

REGISTER_UNIT_TEST( test_public_connect_token_round_trip );
REGISTER_UNIT_TEST( test_public_connect_token_wrong_server_key_refused );
REGISTER_UNIT_TEST( test_public_connect_token_wrong_protocol_id_refused );
REGISTER_UNIT_TEST( test_public_connect_token_null_or_short_buffer_refused );
REGISTER_UNIT_TEST( test_public_connect_token_sealed_block_varies );

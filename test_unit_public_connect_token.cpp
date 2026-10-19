/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_public_connect_token.cpp -- the public keygen and production
// connect-token minting in sdk/rocketnet.h, checked against the internal
// reader, verifier and opener. Every case carries its own timestamps: no case
// reads a clock, opens a socket or touches anything outside the process.

#include "test_unit_registry.h"

#include "rocketnet_connect_token.h"

#include <stdint.h>
#include <string.h>

static const uint64_t public_token_protocol_id = 0x1122334455667788ULL;
static const uint64_t public_token_other_protocol_id = 0x8877665544332211ULL;
static const uint64_t public_token_client_id = 0x0123456789abcdefULL;
static const uint64_t public_token_create_timestamp = 1756166400ULL;
static const uint64_t public_token_expire_timestamp = 1756166700ULL;
static const int32_t public_token_timeout_seconds = 10;

static const char * const public_token_addresses[] =
{
    "203.0.113.10:40000",
    "[2001:db8::1]:65535",
};

static const char * const public_token_one_address[] =
{
    "203.0.113.10:40000",
};

static void public_token_fill_user_data( uint8_t * user_data )
{
    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
        user_data[i] = (uint8_t) ( i * 7 + 3 );
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

void public_connect_token_round_trip()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token, sizeof(token) ) == ROCKETNET_STATUS_OK );

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
    static const uint8_t expected_ipv6[18] =
    {
        0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0xff, 0xff
    };
    rocketnet_unit_check( memcmp( signed_fields.ipv4_address, expected_ipv4, sizeof(expected_ipv4) ) == 0 );
    rocketnet_unit_check( memcmp( signed_fields.ipv6_address, expected_ipv6, sizeof(expected_ipv6) ) == 0 );

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

void public_connect_token_wrong_server_key_refused()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t other_server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t other_server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_server_keypair( other_server_public_key, other_server_secret_key ) == ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token, sizeof(token) ) == ROCKETNET_STATUS_OK );

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

void public_connect_token_wrong_protocol_id_refused()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, 0,
                                             token, sizeof(token) ) == ROCKETNET_STATUS_INVALID_PROTOCOL_ID );

    rocketnet_unit_check( public_token_mint( backend_secret_key, server_public_key, public_token_protocol_id,
                                             token, sizeof(token) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, token );
    struct rocketnet_signed_connect_data_t signed_fields;
    rocketnet_read_signed_connect_data( &signed_fields, parsed.signed_connect_data );
    rocketnet_unit_check( signed_fields.protocol_id == public_token_protocol_id );
    rocketnet_unit_check( signed_fields.protocol_id != public_token_other_protocol_id );
}

void public_connect_token_null_or_short_buffer_refused()
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    public_token_make_keys( backend_public_key, backend_secret_key, server_public_key, server_secret_key );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    public_token_fill_user_data( user_data );

    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_addresses, 2,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, NULL,
                                                                    ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0xa5, sizeof(token) );
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_addresses, 2,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    ROCKETNET_CONNECT_TOKEN_BYTES - 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    for ( int i = 0; i < (int) sizeof(token); i++ )
        rocketnet_unit_check( token[i] == 0xa5 );

    rocketnet_unit_check( rocketnet_write_production_connect_token( NULL, server_public_key,
                                                                    public_token_protocol_id, public_token_one_address, 1,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    sizeof(token) ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_one_address, 0,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    sizeof(token) ) == ROCKETNET_STATUS_INVALID_ARGUMENT );

    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key,
                                                                    public_token_protocol_id, public_token_one_address, 1,
                                                                    public_token_client_id, public_token_create_timestamp,
                                                                    public_token_expire_timestamp, public_token_timeout_seconds,
                                                                    user_data, token,
                                                                    sizeof(token) ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( public_connect_token_round_trip );
REGISTER_UNIT_TEST( public_connect_token_wrong_server_key_refused );
REGISTER_UNIT_TEST( public_connect_token_wrong_protocol_id_refused );
REGISTER_UNIT_TEST( public_connect_token_null_or_short_buffer_refused );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// tests/token_string.h -- the SDK tests' production connect token for a public
// client: minted with the internal production writer (sdk/rocketnet_connect_token.h),
// sealed to the running server's own ephemeral key (rocketnet_server_public_key),
// and written as the NUL-terminated base64 string rocketnet_client_connect takes.

#ifndef ROCKETNET_TEST_TOKEN_STRING_H
#define ROCKETNET_TEST_TOKEN_STRING_H

#include "rocketnet.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_base64.h"

#include <stdint.h>
#include <string.h>

// Encode a binary connect token as the public token string. Returns true on
// success; token_string holds ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES.
static inline bool rn_test_token_to_string( const uint8_t * token, char * token_string )
{
    return rocketnet_base64_encode_data( token, ROCKETNET_CONNECT_TOKEN_BYTES, token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) > 0;
}

// Mint a production token sealed to server_public_key and write it as a string.
static inline enum rocketnet_status_t rn_test_production_token_string( const uint8_t * backend_secret_key,
                                                                       const uint8_t * server_public_key,
                                                                       uint64_t protocol_id,
                                                                       const char * const * server_address_strings,
                                                                       int server_address_count,
                                                                       uint64_t client_id,
                                                                       uint64_t create_timestamp,
                                                                       uint64_t expire_timestamp,
                                                                       int32_t timeout_seconds,
                                                                       const uint8_t * user_data,
                                                                       char * token_string )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    enum rocketnet_status_t status = rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id,
                                                                               server_address_strings, server_address_count,
                                                                               client_id, create_timestamp, expire_timestamp,
                                                                               timeout_seconds, user_data, token, sizeof( token ) );
    if ( status != ROCKETNET_STATUS_OK )
        return status;
    if ( !rn_test_token_to_string( token, token_string ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    return ROCKETNET_STATUS_OK;
}

// As above, sealed to a created server's own key.
static inline enum rocketnet_status_t rn_test_server_token_string( const uint8_t * backend_secret_key,
                                                                   const struct rocketnet_server_t * server,
                                                                   uint64_t protocol_id,
                                                                   const char * const * server_address_strings,
                                                                   int server_address_count,
                                                                   uint64_t client_id,
                                                                   uint64_t create_timestamp,
                                                                   uint64_t expire_timestamp,
                                                                   int32_t timeout_seconds,
                                                                   const uint8_t * user_data,
                                                                   char * token_string )
{
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public_key );
    return rn_test_production_token_string( backend_secret_key, server_public_key, protocol_id, server_address_strings,
                                            server_address_count, client_id, create_timestamp, expire_timestamp,
                                            timeout_seconds, user_data, token_string );
}

#endif // #ifndef ROCKETNET_TEST_TOKEN_STRING_H

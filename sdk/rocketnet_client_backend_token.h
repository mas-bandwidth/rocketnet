/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CLIENT_BACKEND_TOKEN_H
#define ROCKETNET_CLIENT_BACKEND_TOKEN_H

#include "rocketnet.h"

#define ROCKETNET_CLIENT_BACKEND_TOKEN_CRYPTO_HEADER_BYTES 36

#pragma pack(push,1)
struct rocketnet_client_backend_token_t
{
    uint8_t crypto_header[ROCKETNET_CLIENT_BACKEND_TOKEN_CRYPTO_HEADER_BYTES];
    uint64_t version;
    uint64_t expire_timestamp;
    uint64_t buyer_id;
    uint64_t server_id;
    uint64_t rocketnet_id;
    uint64_t user_hash;
    uint32_t client_address;                        // big endian
    uint16_t client_port;                           // big endian
};
#pragma pack(pop)

int rocketnet_write_client_backend_token( rocketnet_client_backend_token_t * token, uint8_t * output, const uint8_t * private_key );

bool rocketnet_read_client_backend_token( rocketnet_client_backend_token_t * token, const uint8_t * input, int input_bytes, const uint8_t * private_key );

void rocketnet_endian_fix( rocketnet_client_backend_token_t * token );

#endif // #ifndef ROCKETNET_ROUTE_TOKEN_H

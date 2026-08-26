/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONNECT_TOKEN_H
#define ROCKETNET_CONNECT_TOKEN_H

#include "rocketnet.h"

#define ROCKETNET_MAX_CONNECT_TOKEN_BYTES 500

#define ROCKETNET_MAX_CONNECT_TOKEN_BACKENDS 32

#define ROCKETNET_CONNECT_TOKEN_SIGNATURE_BYTES 64

#pragma pack(push,1)
struct rocketnet_connect_token_t
{
    uint64_t version;
    uint64_t expire_timestamp;
    uint64_t buyer_id;
    uint64_t server_id;
    uint64_t rocketnet_id;
    uint64_t user_hash;
    uint32_t client_public_address;                                          // big endian
    uint32_t backend_addresses[ROCKETNET_MAX_CONNECT_TOKEN_BACKENDS];             // big endian
    uint16_t backend_ports[ROCKETNET_MAX_CONNECT_TOKEN_BACKENDS];                 // big endian
    uint8_t pings_per_second;
    uint8_t pongs_before_select;
    uint8_t max_connect_seconds;
    uint8_t backend_token_refresh_seconds;
    uint8_t signature[ROCKETNET_CONNECT_TOKEN_SIGNATURE_BYTES];
};
#pragma pack(pop)

bool rocketnet_write_connect_token( rocketnet_connect_token_t * token, char * output, const uint8_t * private_key );

bool rocketnet_read_connect_token( rocketnet_connect_token_t * token, const char * input, const uint8_t * public_key );

void rocketnet_endian_fix( rocketnet_connect_token_t * token );

#endif // #ifndef ROCKETNET_ROUTE_TOKEN_H

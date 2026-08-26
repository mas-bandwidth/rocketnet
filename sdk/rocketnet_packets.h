/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include "rocketnet_connect_token.h"
#include "rocketnet_client_backend_token.h"

#ifndef ROCKETNET_PACKETS_H
#define ROCKETNET_PACKETS_H

#define ROCKETNET_PACKET_CLIENT_BACKEND_INIT_REQUEST                 0
#define ROCKETNET_PACKET_CLIENT_BACKEND_INIT_RESPONSE                1
#define ROCKETNET_PACKET_CLIENT_BACKEND_PING                         2
#define ROCKETNET_PACKET_CLIENT_BACKEND_PONG                         3
#define ROCKETNET_PACKET_CLIENT_BACKEND_REFRESH_TOKEN_REQUEST        4
#define ROCKETNET_PACKET_CLIENT_BACKEND_REFRESH_TOKEN_RESPONSE       5

#define ROCKETNET_PACKET_DIRECT                                      6

#pragma pack(push,1)

// --------------------------------------------------------------------------------------------------

struct rocketnet_client_backend_init_request_packet_t
{
    uint8_t type;
    uint8_t prefix[17];
    uint8_t sdk_version_major;
    uint8_t sdk_version_minor;
    uint8_t sdk_version_patch;
    struct rocketnet_connect_token_t connect_token;
    uint64_t request_id;
};

inline void rocketnet_endian_fix( rocketnet_client_backend_init_request_packet_t * packet )
{
    rocketnet_endian_fix( &packet->connect_token );
    rocketnet_endian_fix( &packet->request_id );
}

// --------------------------------------------------------------------------------------------------

struct rocketnet_client_backend_init_response_packet_t
{
    uint8_t type;
    uint8_t prefix[17];
    uint64_t request_id;
    struct rocketnet_client_backend_token_t backend_token;
};

inline void rocketnet_endian_fix( rocketnet_client_backend_init_response_packet_t * packet )
{
    rocketnet_endian_fix( &packet->request_id );
    rocketnet_endian_fix( &packet->backend_token );
}

// --------------------------------------------------------------------------------------------------

struct rocketnet_client_backend_ping_packet_t
{
    uint8_t type;
    uint8_t prefix[17];
    uint8_t sdk_version_major;
    uint8_t sdk_version_minor;
    uint8_t sdk_version_patch;
    uint64_t request_id;
    uint64_t ping_sequence;
    struct rocketnet_client_backend_token_t backend_token;
};

inline void rocketnet_endian_fix( rocketnet_client_backend_ping_packet_t * packet )
{
    rocketnet_endian_fix( &packet->request_id );
    rocketnet_endian_fix( &packet->ping_sequence );
    rocketnet_endian_fix( &packet->backend_token );
}

// --------------------------------------------------------------------------------------------------

struct rocketnet_client_backend_pong_packet_t
{
    uint8_t type;
    uint8_t prefix[17];
    uint64_t request_id;
    uint64_t ping_sequence;
};

inline void rocketnet_endian_fix( rocketnet_client_backend_pong_packet_t * packet )
{
    rocketnet_endian_fix( &packet->request_id );
    rocketnet_endian_fix( &packet->ping_sequence );
}

// --------------------------------------------------------------------------------------------------

struct rocketnet_client_backend_refresh_token_request_packet_t
{
    uint8_t type;
    uint8_t prefix[17];
    uint8_t sdk_version_major;
    uint8_t sdk_version_minor;
    uint8_t sdk_version_patch;
    uint64_t request_id;
    struct rocketnet_client_backend_token_t backend_token;
};

inline void rocketnet_endian_fix( rocketnet_client_backend_refresh_token_request_packet_t * packet )
{
    rocketnet_endian_fix( &packet->request_id );
    rocketnet_endian_fix( &packet->backend_token );
}

// --------------------------------------------------------------------------------------------------

struct rocketnet_client_backend_refresh_token_response_packet_t
{
    uint8_t type;
    uint8_t prefix[17];
    uint64_t request_id;
    struct rocketnet_client_backend_token_t backend_token;
};

inline void rocketnet_endian_fix( rocketnet_client_backend_refresh_token_response_packet_t * packet )
{
    rocketnet_endian_fix( &packet->request_id );
    rocketnet_endian_fix( &packet->backend_token );
}

// --------------------------------------------------------------------------------------------------

struct rocketnet_direct_packet_t
{
    uint8_t type;
    uint8_t prefix[17];
    uint8_t payload[ROCKETNET_MTU];
};

inline void rocketnet_endian_fix( rocketnet_direct_packet_t * packet )
{
    // ...
}

// --------------------------------------------------------------------------------------------------

#pragma pack(pop)

#endif // #ifndef ROCKETNET_PACKETS_H

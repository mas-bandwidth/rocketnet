/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_HEADER_H
#define ROCKETNET_HEADER_H

#if 0

#include "rocketnet.h"
#include "rocketnet_crypto.h"
#include "rocketnet_read_write.h"

#pragma pack(push, 1)
struct header_data
{
    uint8_t session_private_key[ROCKETNET_SESSION_PRIVATE_KEY_BYTES];
    uint8_t packet_type;
    uint64_t packet_sequence;
    uint64_t rocketnet_id;
    uint8_t session_version;
};
#pragma pack(pop)

inline int rocketnet_write_header( uint8_t packet_type, uint64_t packet_sequence, uint64_t rocketnet_id, uint8_t rocketnet_version, const uint8_t * private_key, uint8_t * header )
{
    rocketnet_assert( private_key );
    rocketnet_assert( header );

    uint8_t * p = header;

    rocketnet_write_uint64( &p, packet_sequence );
    rocketnet_write_uint64( &p, rocketnet_id );
    rocketnet_write_uint8( &p, rocketnet_version );

    struct header_data data;
    memcpy( data.session_private_key, private_key, ROCKETNET_SESSION_PRIVATE_KEY_BYTES );
    data.packet_type = packet_type;
    data.packet_sequence = packet_sequence;
    data.rocketnet_id = rocketnet_id;
    data.rocketnet_version = rocketnet_version;

    rocketnet_crypto_hash_sha256( p, (const unsigned char*) &data, sizeof(struct header_data) );

    return ROCKETNET_OK;
}

inline void rocketnet_peek_header( uint64_t * sequence, uint64_t * rocketnet_id, uint8_t * session_version, const uint8_t * header, int header_length )
{
    (void) header_length;

    rocketnet_assert( header );
    rocketnet_assert( header_length >= ROCKETNET_HEADER_BYTES );

    uint64_t packet_sequence = rocketnet_read_uint64( &header );

    *sequence = packet_sequence;
    *rocketnet_id = rocketnet_read_uint64( &header );
    *session_version = rocketnet_read_uint8( &header );
}

inline int rocketnet_read_header( int packet_type, uint64_t * sequence, uint64_t * rocketnet_id, uint8_t * session_version, const uint8_t * private_key, uint8_t * header, int header_length )
{
    rocketnet_assert( private_key );
    rocketnet_assert( header );
    rocketnet_assert( header_length >= ROCKETNET_HEADER_BYTES );

    (void) header_length;

    struct header_data data;

    memcpy( data.session_private_key, private_key, ROCKETNET_SESSION_PRIVATE_KEY_BYTES );

    data.packet_type = packet_type;

    data.packet_sequence  = header[0];
    data.packet_sequence |= ( ( (uint64_t)( header[1] ) ) << 8  );
    data.packet_sequence |= ( ( (uint64_t)( header[2] ) ) << 16 );
    data.packet_sequence |= ( ( (uint64_t)( header[3] ) ) << 24 );
    data.packet_sequence |= ( ( (uint64_t)( header[4] ) ) << 32 );
    data.packet_sequence |= ( ( (uint64_t)( header[5] ) ) << 40 );
    data.packet_sequence |= ( ( (uint64_t)( header[6] ) ) << 48 );
    data.packet_sequence |= ( ( (uint64_t)( header[7] ) ) << 56 );

    data.rocketnet_id  = header[8];
    data.rocketnet_id |= ( ( (uint64_t)( header[8+1] ) ) << 8  );
    data.rocketnet_id |= ( ( (uint64_t)( header[8+2] ) ) << 16 );
    data.rocketnet_id |= ( ( (uint64_t)( header[8+3] ) ) << 24 );
    data.rocketnet_id |= ( ( (uint64_t)( header[8+4] ) ) << 32 );
    data.rocketnet_id |= ( ( (uint64_t)( header[8+5] ) ) << 40 );
    data.rocketnet_id |= ( ( (uint64_t)( header[8+6] ) ) << 48 );
    data.rocketnet_id |= ( ( (uint64_t)( header[8+7] ) ) << 56 );

    data.session_version = header[8+8];

    uint8_t hash[32];
    rocketnet_crypto_hash_sha256( hash, (const unsigned char*) &data, sizeof(struct header_data) );

    if ( memcmp( hash, header + 8 + 8 + 1, 8 ) != 0 )
    {
        return ROCKETNET_ERROR;
    }

    *sequence = data.packet_sequence;
    *rocketnet_id = data.rocketnet_id;
    *rocketnet_version = data.rocketnet_version;

    return ROCKETNET_OK;
}

#endif // #if 0

#endif // #ifndef ROCKETNET_HEADER_H

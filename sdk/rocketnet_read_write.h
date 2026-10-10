/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_READ_WRITE_H
#define ROCKETNET_READ_WRITE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Pointer-advancing readers and writers for the byte-aligned wire format in
// STANDARD.md. Values are written little-endian by shift and mask, so these
// are endian independent by construction and need no detection or fixup.
// The pointer advances past what was written or read, so callers never manage
// offsets by hand.

inline void rocketnet_write_uint8( uint8_t ** p, uint8_t value )
{
    **p = value;
    ++(*p);
}

inline void rocketnet_write_uint16( uint8_t ** p, uint16_t value )
{
    (*p)[0] = value & 0xFF;
    (*p)[1] = ( value >> 8 ) & 0xFF;
    *p += 2;
}

inline void rocketnet_write_uint32( uint8_t ** p, uint32_t value )
{
    (*p)[0] = value & 0xFF;
    (*p)[1] = ( value >> 8  ) & 0xFF;
    (*p)[2] = ( value >> 16 ) & 0xFF;
    (*p)[3] = ( value >> 24 ) & 0xFF;
    *p += 4;
}

inline void rocketnet_write_uint64( uint8_t ** p, uint64_t value )
{
    (*p)[0] = value & 0xFF;
    (*p)[1] = ( value >> 8  ) & 0xFF;
    (*p)[2] = ( value >> 16 ) & 0xFF;
    (*p)[3] = ( value >> 24 ) & 0xFF;
    (*p)[4] = ( value >> 32 ) & 0xFF;
    (*p)[5] = ( value >> 40 ) & 0xFF;
    (*p)[6] = ( value >> 48 ) & 0xFF;
    (*p)[7] = ( value >> 56 ) & 0xFF;
    *p += 8;
}

inline void rocketnet_write_bytes( uint8_t ** p, const uint8_t * data, int bytes )
{
    memcpy( *p, data, (size_t) bytes );
    *p += bytes;
}

inline void rocketnet_write_zero_bytes( uint8_t ** p, int bytes )
{
    memset( *p, 0, (size_t) bytes );
    *p += bytes;
}

inline uint8_t rocketnet_read_uint8( const uint8_t ** p )
{
    uint8_t value = **p;
    ++(*p);
    return value;
}

inline uint16_t rocketnet_read_uint16( const uint8_t ** p )
{
    uint16_t value = (uint16_t) (*p)[0];
    value |= (uint16_t) ( (*p)[1] ) << 8;
    *p += 2;
    return value;
}

inline uint32_t rocketnet_read_uint32( const uint8_t ** p )
{
    uint32_t value = (uint32_t) (*p)[0];
    value |= (uint32_t) ( (*p)[1] ) << 8;
    value |= (uint32_t) ( (*p)[2] ) << 16;
    value |= (uint32_t) ( (*p)[3] ) << 24;
    *p += 4;
    return value;
}

inline uint64_t rocketnet_read_uint64( const uint8_t ** p )
{
    uint64_t value = (uint64_t) (*p)[0];
    value |= (uint64_t) ( (*p)[1] ) << 8;
    value |= (uint64_t) ( (*p)[2] ) << 16;
    value |= (uint64_t) ( (*p)[3] ) << 24;
    value |= (uint64_t) ( (*p)[4] ) << 32;
    value |= (uint64_t) ( (*p)[5] ) << 40;
    value |= (uint64_t) ( (*p)[6] ) << 48;
    value |= (uint64_t) ( (*p)[7] ) << 56;
    *p += 8;
    return value;
}

inline void rocketnet_read_bytes( const uint8_t ** p, uint8_t * data, int bytes )
{
    memcpy( data, *p, (size_t) bytes );
    *p += bytes;
}

// Bounded readers. The readers above trust a size check made elsewhere; these
// take end, one past the last readable byte, and refuse a read that does not
// fit whole. A refused read returns false and leaves *p and *value exactly as
// they were, so a parse that runs out of input stops on the byte it ran out on
// instead of reading past the buffer. A read that fits succeeds and advances
// *p past the value, exactly as the unbounded reader above would.

inline bool rocketnet_read_bounded_available( const uint8_t * p, const uint8_t * end, int bytes )
{
    if ( bytes < 0 || p > end )
        return false;
    return (size_t) ( end - p ) >= (size_t) bytes;
}

inline bool rocketnet_read_uint8_bounded( const uint8_t ** p, const uint8_t * end, uint8_t * value )
{
    if ( !rocketnet_read_bounded_available( *p, end, 1 ) )
        return false;
    *value = rocketnet_read_uint8( p );
    return true;
}

inline bool rocketnet_read_uint16_bounded( const uint8_t ** p, const uint8_t * end, uint16_t * value )
{
    if ( !rocketnet_read_bounded_available( *p, end, 2 ) )
        return false;
    *value = rocketnet_read_uint16( p );
    return true;
}

inline bool rocketnet_read_uint32_bounded( const uint8_t ** p, const uint8_t * end, uint32_t * value )
{
    if ( !rocketnet_read_bounded_available( *p, end, 4 ) )
        return false;
    *value = rocketnet_read_uint32( p );
    return true;
}

inline bool rocketnet_read_uint64_bounded( const uint8_t ** p, const uint8_t * end, uint64_t * value )
{
    if ( !rocketnet_read_bounded_available( *p, end, 8 ) )
        return false;
    *value = rocketnet_read_uint64( p );
    return true;
}

inline bool rocketnet_read_bytes_bounded( const uint8_t ** p, const uint8_t * end, uint8_t * data, int bytes )
{
    if ( !rocketnet_read_bounded_available( *p, end, bytes ) )
        return false;
    rocketnet_read_bytes( p, data, bytes );
    return true;
}

#endif // #ifndef ROCKETNET_READ_WRITE_H

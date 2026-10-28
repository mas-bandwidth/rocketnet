/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_hash.h"

// 64 bit FNV-1a
uint64_t rocketnet_hash_string( const char * string )
{
    uint64_t hash = 0xCBF29CE484222325;

    for ( const uint8_t * p = (const uint8_t *) string; *p; p++ )
    {
        hash ^= *p;
        hash *= 0x00000100000001B3;
    }

    return hash;
}

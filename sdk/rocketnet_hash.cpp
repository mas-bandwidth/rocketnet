/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_hash.h"

#include <string.h>

typedef uint64_t rocketnet_fnv_t;

void rocketnet_fnv_init( rocketnet_fnv_t * fnv )
{
    *fnv = 0xCBF29CE484222325;
}

void rocketnet_fnv_write( rocketnet_fnv_t * fnv, const uint8_t * data, size_t size )
{
    for ( size_t i = 0; i < size; i++ )
    {
        (*fnv) ^= data[i];
        (*fnv) *= 0x00000100000001B3;
    }
}

uint64_t rocketnet_fnv_finalize( rocketnet_fnv_t * fnv )
{
    return *fnv;
}

uint64_t rocketnet_hash_string( const char * string )
{
    rocketnet_fnv_t fnv;
    rocketnet_fnv_init( &fnv );
    rocketnet_fnv_write( &fnv, (const uint8_t *)( string ), strlen( string ) );
    return rocketnet_fnv_finalize( &fnv );
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_HASH_H
#define ROCKETNET_SDK_ROCKETNET_HASH_H

#include "rocketnet_internal.h"

uint64_t rocketnet_hash_string( const char * string );

typedef uint64_t rocketnet_fnv_t;

void rocketnet_fnv_init( rocketnet_fnv_t * fnv );

void rocketnet_fnv_write( rocketnet_fnv_t * fnv, const uint8_t * data, size_t size );

uint64_t rocketnet_fnv_finalize( rocketnet_fnv_t * fnv );

#endif // ROCKETNET_SDK_ROCKETNET_HASH_H

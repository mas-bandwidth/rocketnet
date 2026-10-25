/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_MEMORY_CHECKS_H
#define ROCKETNET_MEMORY_CHECKS_H

#include "rocketnet_internal.h"

#if ROCKETNET_ENABLE_MEMORY_CHECKS

#define ROCKETNET_DECLARE_SENTINEL( n ) uint32_t rocketnet_sentinel_##n[64];

#define ROCKETNET_INITIALIZE_SENTINEL( pointer, n ) for ( int i = 0; i < 64; ++i ) { pointer->rocketnet_sentinel_##n[i] = 0xBAADF00D; }

#define ROCKETNET_VERIFY_SENTINEL( pointer, n ) for ( int i = 0; i < 64; ++i ) { rocketnet_assert( pointer->rocketnet_sentinel_##n[i] == 0xBAADF00D ); }

#else // #if ROCKETNET_ENABLE_MEMORY_CHECKS

#define ROCKETNET_DECLARE_SENTINEL( n )

#define ROCKETNET_INITIALIZE_SENTINEL( pointer, n )

#define ROCKETNET_VERIFY_SENTINEL( pointer, n )

#endif // #if ROCKETNET_ENABLE_MEMORY_CHECKS

#endif // #ifndef ROCKETNET_MEMORY_CHECKS_H

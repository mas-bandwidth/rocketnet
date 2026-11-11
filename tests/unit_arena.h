/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_TESTS_UNIT_ARENA_H
#define ROCKETNET_TESTS_UNIT_ARENA_H

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_allocator.h"

// A unit case's message arena, in a development and a production build alike.
// rn_arena_create is development-only (sdk/rocketnet_allocator.h), so the
// case allocates the arena's exact block itself and builds the same arena in
// it with rn_arena_create_in_block, which the production library carries.
// The arena header is the start of the block, so destroy frees the block
// through the arena pointer. NULL for a refused config or count, or no memory.

static inline rn_arena_t * rn_unit_arena_create( const rocketnet_message_config_t * config, int num_regions, rocketnet_counters_t * counters )
{
    const uint64_t bytes = rn_arena_required_bytes( config, num_regions );
    if ( bytes == 0 || bytes > (uint64_t) SIZE_MAX )
    {
        return NULL;
    }

    void * block = rocketnet_malloc( NULL, (size_t) bytes );
    if ( !block )
    {
        return NULL;
    }

    rn_arena_t * arena = rn_arena_create_in_block( block, bytes, config, num_regions, counters );
    if ( !arena )
    {
        rocketnet_free( NULL, block );
    }

    return arena;
}

static inline void rn_unit_arena_destroy( rn_arena_t * arena )
{
    if ( !arena )
    {
        return;
    }

    rn_arena_destroy( arena );
    rocketnet_free( NULL, arena );
}

#endif // ROCKETNET_TESTS_UNIT_ARENA_H

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include <stdint.h>
#include "rocketnet_constants.h"

// Returns true only when the frame fits in the list and passes all checks.
// Otherwise returns false and leaves the list unchanged.
static inline bool rocketnet_xdp_free_list_push( uint64_t * frames, int * count, int capacity, uint64_t frame, uint64_t region_begin, uint64_t region_end )
{
    if ( *count >= capacity )
        return false;
    if ( frame < region_begin || frame >= region_end )
        return false;
    if ( ( frame - region_begin ) % ROCKETNET_XDP_FRAME_SIZE != 0 )
        return false;
    frames[( *count )++] = frame;
    return true;
}

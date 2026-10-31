/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the XDP free list.

    Push is rocketnet_xdp_free_list_push in sdk/rocketnet_xdp_free_list.h.
    Pop is the alloc_send_frame pattern (sdk/rocketnet_server_socket_linux.cpp): if the
    count is positive, decrement it, take that slot, and store an invalid
    frame there. Otherwise return the invalid frame and leave the count
    at zero.

    Capacity is 3. The region is the first 3 frames of 4096 bytes, so the
    only aligned in-region frames are 0, 4096, and 8192. Four symbolic
    steps, each a push of a symbolic frame or a pop. After every step the
    list is exactly the frames that were pushed and not yet popped, the
    count stays inside 0..capacity, and a refused push leaves the slots
    unchanged.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 5. The step loop runs 4 times; the slot compare runs 3.
*/

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "rocketnet_xdp_free_list.h"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static uint64_t nondet_uint64( void ) { return 0; }
static _Bool nondet_bool( void ) { return 0; }
#else
uint64_t nondet_uint64( void );
_Bool nondet_bool( void );
#endif // #ifndef __CPROVER__

#define CAP     3
#define STEPS   4
#define INVALID UINT64_MAX

/* The alloc_*_frame pop: decrement, take the slot, store invalid. */

static uint64_t rocketnet_xdp_free_list_pop( uint64_t * frames, int * count )
{
    uint64_t frame = INVALID;

    if ( *count > 0 )
    {
        ( *count )--;
        frame = frames[*count];
        frames[*count] = INVALID;
    }

    return frame;
}

int main( void )
{
    uint64_t frames[CAP];
    uint64_t model[CAP];
    int count = 0;
    int model_n = 0;
    const uint64_t region_begin = 0;
    const uint64_t region_end = (uint64_t) CAP * ROCKETNET_XDP_FRAME_SIZE;

    for ( int step = 0; step < STEPS; step++ )
    {
        if ( nondet_bool() )
        {
            uint64_t frame = nondet_uint64();
            int before = count;
            uint64_t saved[CAP];

            for ( int i = 0; i < CAP; i++ )
            {
                saved[i] = frames[i];
            }

            bool ok = rocketnet_xdp_free_list_push( frames, &count, CAP, frame, region_begin, region_end );
            if ( ok )
            {
                __CPROVER_assert( count == before + 1, "push grows the count by one" );
                __CPROVER_assert( before < CAP, "push only succeeds below capacity" );
                __CPROVER_assert( frame >= region_begin && frame < region_end, "push keeps the frame inside the region" );
                __CPROVER_assert( ( frame - region_begin ) % ROCKETNET_XDP_FRAME_SIZE == 0, "push keeps the frame aligned" );
                __CPROVER_assert( frames[before] == frame, "push stores the frame at the new top" );
                model[model_n] = frame;
                model_n++;
            }
            else
            {
                __CPROVER_assert( count == before, "refused push leaves the count" );

                for ( int i = 0; i < CAP; i++ )
                {
                    __CPROVER_assert( frames[i] == saved[i], "refused push leaves the slots" );
                }

                __CPROVER_assert( before >= CAP || frame < region_begin || frame >= region_end || ( frame - region_begin ) % ROCKETNET_XDP_FRAME_SIZE != 0,
                                  "refused push was full, outside the region, or misaligned" );
            }
        }
        else
        {
            int before = count;
            uint64_t expect = before > 0 ? frames[before - 1] : INVALID;
            uint64_t got = rocketnet_xdp_free_list_pop( frames, &count );
            if ( before == 0 )
            {
                __CPROVER_assert( got == INVALID, "pop of an empty list is invalid" );
                __CPROVER_assert( count == 0, "pop of an empty list leaves the count" );
            }
            else
            {
                __CPROVER_assert( got == expect, "pop returns the frame that was pushed" );
                __CPROVER_assert( count == before - 1, "pop shrinks the count by one" );
                __CPROVER_assert( frames[count] == INVALID, "pop clears the vacated slot" );
                __CPROVER_assert( model_n > 0 && got == model[model_n - 1], "pop conserves the frame" );
                model_n--;
            }
        }

        __CPROVER_assert( count == model_n, "count matches the frames still held" );
        __CPROVER_assert( count >= 0 && count <= CAP, "count stays inside capacity" );
        for ( int i = 0; i < CAP; i++ )
        {
            if ( i < count )
            {
                __CPROVER_assert( frames[i] == model[i], "held prefix matches the push order" );
            }
        }
    }
#if defined( REACHABILITY_CONTROL )
    __CPROVER_assert( 0, "xdp-free-list reachability control" );
#endif // #if defined( REACHABILITY_CONTROL )
    return 0;
}

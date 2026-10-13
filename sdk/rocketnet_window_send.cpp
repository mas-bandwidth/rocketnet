/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_window_send.h"

// Red-first implementation skeleton. The tests below must fail on this
// revision; the rules in docs/CHANNELS.md 6.2 and RNRing/RNWindow follow.
void rn_window_send_init( rn_window_send_t * send, rn_region_t * region, uint64_t first_number )
{
    send->region = region;
    send->next = first_number;
    send->U = first_number;
    send->U_prime = first_number;
    send->ring_next = 0;
    send->stalled = 0;
}

int rn_window_send_first_write( rn_window_send_t *, int, uint32_t, uint32_t, uint64_t *, uint64_t * )
{
    return RN_WINDOW_SEND_STALL;
}

uint32_t rn_window_send_first_write_block( rn_window_send_t *, int, uint32_t, uint32_t, uint64_t *, uint64_t * )
{
    return 0;
}

int rn_window_send_mark_acked( rn_window_send_t *, uint64_t )
{
    return 0;
}

int rn_window_send_can_write_unreliable( const rn_window_send_t * )
{
    return 0;
}

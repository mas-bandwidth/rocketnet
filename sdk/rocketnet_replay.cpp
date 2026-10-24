/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_replay.h"
#include "rocketnet_internal.h"

#include <string.h>

static int rocketnet_replay_bit( const struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    int bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
    return ( replay->window[bit / 64] >> ( bit % 64 ) ) & 1;
}

static void rocketnet_replay_set_bit( struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    int bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
    replay->window[bit / 64] |= 1ULL << ( bit % 64 );
}

static void rocketnet_replay_clear_bit( struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    int bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
    replay->window[bit / 64] &= ~( 1ULL << ( bit % 64 ) );
}

// tla/ReplayWindow.tla AlreadyReceived
int rocketnet_replay_already_received( const struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    rocketnet_assert( replay );
    rocketnet_assert( sequence != 0 );

    if ( sequence > replay->highest_opened )
        return 0;

    if ( replay->highest_opened >= ROCKETNET_REPLAY_WINDOW_BITS &&
         sequence <= replay->highest_opened - ROCKETNET_REPLAY_WINDOW_BITS )
        return 1;

    return rocketnet_replay_bit( replay, sequence );
}

// tla/ReplayWindow.tla Advance
// tla/ReplayWindow.tla Receive: advances highest_opened, clears the skipped bits and sets the received sequence's bit
void rocketnet_replay_advance( struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    rocketnet_assert( replay );
    rocketnet_assert( sequence != 0 );
    rocketnet_assert( !rocketnet_replay_already_received( replay, sequence ) );

    if ( sequence > replay->highest_opened )
    {
        // clear every entry the advance skips past, so a sequence far ahead
        // does not inherit bits from the previous lap of the ring

        uint64_t skipped = sequence - replay->highest_opened;
        if ( skipped >= ROCKETNET_REPLAY_WINDOW_BITS )
        {
            // TRANSPORT.md t14 Replay protection and tla/ReplayWindow.tla (Cleared):
            // a full-window skip clears the whole ring (ClearedWords = Words).
            memset( replay->window, 0, sizeof( replay->window ) );
        }
        else
        {
            for ( uint64_t s = replay->highest_opened + 1; s < sequence; s++ )
                rocketnet_replay_clear_bit( replay, s );
        }
        replay->highest_opened = sequence;
    }

    rocketnet_replay_set_bit( replay, sequence );
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message tier's half-range reconstruction is safe only while the
// transport replay window stays inside its two published bounds and remains
// a multiple of the 64-bit ring words (STANDARD.md _Numbers_ and _Replay
// window sizing_). This pins the compiled depth to those macros.

#include "test_unit_registry.h"

#include "rocketnet_history.h"
#include "rocketnet_replay.h"

#include <string.h>

static void test_replay_window_bounds_pinned()
{
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS_MIN == 4096 );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS_MAX == 32768 );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS >= ROCKETNET_HISTORY_PACKETS_MIN );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS <= ROCKETNET_HISTORY_PACKETS_MAX );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS % 64 == 0 );
}

REGISTER_UNIT_TEST( test_replay_window_bounds_pinned );

static void test_replay_partial_skip_clears_like_bitwise()
{
    const int starts[] = { 0, 1, 63, 64, 65, ROCKETNET_REPLAY_WINDOW_BITS - 1 };
    const int skips[] = { 1, 2, 63, 64, 65, 127, 128,
                          ROCKETNET_REPLAY_WINDOW_BITS / 2, ROCKETNET_REPLAY_WINDOW_BITS - 1 };
    for ( const int start : starts )
    {
        for ( const int skip : skips )
        {
            struct rocketnet_replay_protection_t actual;
            memset( actual.window, 0xFF, sizeof( actual.window ) );
            actual.highest_opened = (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS + (uint64_t) start;
            struct rocketnet_replay_protection_t expected = actual;
            const uint64_t sequence = actual.highest_opened + (uint64_t) skip;
            for ( uint64_t s = expected.highest_opened + 1; s < sequence; s++ )
            {
                const int bit = (int) ( s % ROCKETNET_REPLAY_WINDOW_BITS );
                expected.window[bit / 64] &= ~( 1ULL << ( bit % 64 ) );
            }
            const int received_bit = (int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS );
            expected.window[received_bit / 64] |= 1ULL << ( received_bit % 64 );
            expected.highest_opened = sequence;

            rocketnet_replay_advance( &actual, sequence );

            rocketnet_unit_check( actual.highest_opened == expected.highest_opened );
            for ( int word = 0; word < ROCKETNET_REPLAY_WINDOW_BITS / 64; word++ )
                rocketnet_unit_check( actual.window[word] == expected.window[word] );
            rocketnet_unit_check( rocketnet_replay_already_received( &actual, sequence ) );
        }
    }
}

REGISTER_UNIT_TEST( test_replay_partial_skip_clears_like_bitwise );

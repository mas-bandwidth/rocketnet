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

static void test_replay_window_bounds_pinned()
{
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS_MIN == 4096 );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS_MAX == 32768 );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS >= ROCKETNET_HISTORY_PACKETS_MIN );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS <= ROCKETNET_HISTORY_PACKETS_MAX );
    rocketnet_unit_check( ROCKETNET_HISTORY_PACKETS % 64 == 0 );
}

REGISTER_UNIT_TEST( test_replay_window_bounds_pinned );

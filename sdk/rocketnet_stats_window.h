/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_STATS_WINDOW_H
#define ROCKETNET_SDK_ROCKETNET_STATS_WINDOW_H

#include <stddef.h>
#include <stdint.h>

// Buckets advance with elapsed time, never update count. N buckets therefore
// retain at least a second even with arbitrarily many updates at one timestamp.
// The oldest boundary has at most 1/(N-1) second of quantization. A clock
// that steps backwards is not elapsed time: the current bucket is kept, and
// the window advances again once time reaches that bucket's time plus the
// bucket width, so a rewound or step-driven clock never wipes the window.
// Main thread. The client and the server both advance through this one rule.
template <class Bucket, size_t N>
int rocketnet_stats_window_advance( Bucket ( &buckets )[N], int index, uint64_t frame, double time )
{
    static_assert( N > 1, "a stats window needs multiple buckets" );
    if ( !buckets[index].frame || time - buckets[index].time >= 1.0 / double( N - 1 ) )
    {
        index = ( index + 1 ) % int( N );
        buckets[index] = Bucket{};
        buckets[index].frame = frame;
        buckets[index].time = time;
    }
    return index;
}

#endif // ROCKETNET_SDK_ROCKETNET_STATS_WINDOW_H

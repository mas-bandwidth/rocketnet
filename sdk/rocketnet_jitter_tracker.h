/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_JITTER_TRACKER_H
#define ROCKETNET_JITTER_TRACKER_H

#include "rocketnet_internal.h"
#include "rocketnet_memory_checks.h"

#include <math.h>

struct rocketnet_jitter_tracker_t
{
    ROCKETNET_DECLARE_SENTINEL(0)

    uint64_t last_packet_processed;
    double last_packet_time;
    double last_packet_delta;
    double jitter;

    ROCKETNET_DECLARE_SENTINEL(1)
};

inline void rocketnet_jitter_tracker_initialize_sentinels( rocketnet_jitter_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 0 )
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 1 )
}

inline void rocketnet_jitter_tracker_verify_sentinels( rocketnet_jitter_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_VERIFY_SENTINEL( tracker, 0 )
    ROCKETNET_VERIFY_SENTINEL( tracker, 1 )
}

inline void rocketnet_jitter_tracker_reset( rocketnet_jitter_tracker_t * tracker )
{
    rocketnet_assert( tracker );

    rocketnet_jitter_tracker_initialize_sentinels( tracker );

    tracker->last_packet_processed = 0;
    tracker->last_packet_time = 0.0;
    tracker->last_packet_delta = 0.0;
    tracker->jitter = 0.0;

    rocketnet_jitter_tracker_verify_sentinels( tracker );
}

inline void rocketnet_jitter_tracker_packet_received( rocketnet_jitter_tracker_t * tracker, uint64_t sequence, double time )
{
    rocketnet_jitter_tracker_verify_sentinels( tracker );

    if ( sequence == tracker->last_packet_processed + 1 && tracker->last_packet_time > 0.0 )
    {
        const double delta = time - tracker->last_packet_time;

        // a stamp that steps backwards is not a sample: re-seed and skip it,
        // as the ack rtt sample discards a negative value
        if ( delta < 0.0 )
        {
            tracker->last_packet_processed = sequence;
            tracker->last_packet_time = time;
            return;
        }

        const double jitter = fabs( delta - tracker->last_packet_delta );
        tracker->last_packet_delta = delta;

        if ( fabs( jitter - tracker->jitter ) > 0.00001 )
        {
            tracker->jitter += ( jitter - tracker->jitter ) * 0.01;
        }
        else
        {
            tracker->jitter = jitter;
        }
    }

    tracker->last_packet_processed = sequence;
    tracker->last_packet_time = time;
}

#endif // #ifndef ROCKETNET_JITTER_TRACKER_H

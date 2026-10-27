/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_ROCKETNET_VALUE_TRACKER_H
#define ROCKETNET_TESTS_ROCKETNET_VALUE_TRACKER_H

#include "rocketnet_internal.h"
#include "rocketnet_memory_checks.h"
#include <memory.h>
#include <float.h>

struct rocketnet_value_tracker_t
{
    ROCKETNET_DECLARE_SENTINEL(0)

    int index;
    int num_samples;
    float minimum;
    float maximum;

    ROCKETNET_DECLARE_SENTINEL(1)

    float values[ROCKETNET_VALUE_TRACKER_HISTORY];

    ROCKETNET_DECLARE_SENTINEL(2)
};

inline void rocketnet_value_tracker_initialize_sentinels( rocketnet_value_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 0 )
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 1 )
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 2 )
}

inline void rocketnet_value_tracker_verify_sentinels( rocketnet_value_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_VERIFY_SENTINEL( tracker, 0 )
    ROCKETNET_VERIFY_SENTINEL( tracker, 1 )
    ROCKETNET_VERIFY_SENTINEL( tracker, 2 )
}

inline void rocketnet_value_tracker_reset( rocketnet_value_tracker_t * tracker )
{
    rocketnet_assert( tracker );

    memset( tracker, 0, sizeof(rocketnet_value_tracker_t) );

    rocketnet_value_tracker_initialize_sentinels( tracker );

    tracker->maximum = -FLT_MAX;
    tracker->minimum = +FLT_MAX;

    rocketnet_value_tracker_verify_sentinels( tracker );
}

inline void rocketnet_value_tracker_add_sample( rocketnet_value_tracker_t * tracker, float value )
{
    rocketnet_value_tracker_verify_sentinels( tracker );

    if ( value > tracker->maximum )
    {
        tracker->maximum = value;
    }

    if ( value < tracker->minimum )
    {
        tracker->minimum = value;
    }

    tracker->values[tracker->index] = value;
    tracker->index = ( tracker->index + 1 ) % ROCKETNET_VALUE_TRACKER_HISTORY;
    tracker->num_samples++;
}

inline void rocketnet_value_tracker_calculate( rocketnet_value_tracker_t * tracker, float * min_value, float * max_value, float * avg_value )
{
    rocketnet_value_tracker_verify_sentinels( tracker );

    rocketnet_assert( min_value );
    rocketnet_assert( max_value );
    rocketnet_assert( avg_value );

    if ( tracker->num_samples == 0 )
    {
        *min_value = 0.0f;
        *max_value = 0.0f;
        *avg_value = 0.0f;
        return;
    }

    if ( tracker->num_samples > ROCKETNET_VALUE_TRACKER_HISTORY )
    {
        tracker->num_samples = ROCKETNET_VALUE_TRACKER_HISTORY;
    }

    *min_value = tracker->minimum;
    *max_value = tracker->maximum;

    rocketnet_assert( tracker->num_samples > 0 );

    double sum = 0.0;
    for ( int i = 0; i < tracker->num_samples; i++ )
    {
        sum += (double) tracker->values[i];
    }
    *avg_value = float( sum / double(tracker->num_samples) );

    rocketnet_value_tracker_reset( tracker );
}

#endif // ROCKETNET_TESTS_ROCKETNET_VALUE_TRACKER_H

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_PACKET_LOSS_TRACKER_H
#define ROCKETNET_PACKET_LOSS_TRACKER_H

#include "rocketnet_constants.h"
#include "rocketnet_memory_checks.h"

struct rocketnet_packet_loss_tracker_t
{
    ROCKETNET_DECLARE_SENTINEL(0)

    uint64_t last_packet_processed;
    uint64_t most_recent_packet_received;

    ROCKETNET_DECLARE_SENTINEL(1)

    uint64_t received_packets[ROCKETNET_PACKET_LOSS_TRACKER_HISTORY];

    ROCKETNET_DECLARE_SENTINEL(2)
};

inline void rocketnet_packet_loss_tracker_initialize_sentinels( rocketnet_packet_loss_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 0 )
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 1 )
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 2 )
}

inline void rocketnet_packet_loss_tracker_verify_sentinels( rocketnet_packet_loss_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_VERIFY_SENTINEL( tracker, 0 )
    ROCKETNET_VERIFY_SENTINEL( tracker, 1 )
    ROCKETNET_VERIFY_SENTINEL( tracker, 2 )
}

inline void rocketnet_packet_loss_tracker_reset( rocketnet_packet_loss_tracker_t * tracker )
{
    rocketnet_assert( tracker );

    rocketnet_packet_loss_tracker_initialize_sentinels( tracker );

    tracker->last_packet_processed = 0;
    tracker->most_recent_packet_received = 0;

    for ( int i = 0; i < ROCKETNET_PACKET_LOSS_TRACKER_HISTORY; ++i )
    {
        tracker->received_packets[i] = 0xFFFFFFFFFFFFFFFFUL;
    }

    rocketnet_packet_loss_tracker_verify_sentinels( tracker );
}

inline void rocketnet_packet_loss_tracker_packet_received( rocketnet_packet_loss_tracker_t * tracker, uint64_t sequence )
{
    rocketnet_packet_loss_tracker_verify_sentinels( tracker );

    sequence++;

    // a sequence update has already judged is neither stamped nor counted: its
    // slot may hold the received, not yet judged sequence HISTORY above it
    if ( sequence <= tracker->last_packet_processed )
    {
        return;
    }

    const int index = int( sequence % ROCKETNET_PACKET_LOSS_TRACKER_HISTORY );

    tracker->received_packets[index] = sequence;
    // the high-water mark never moves backwards: a reordered older packet
    // stays a hole in the judged window, not a new most-recent sequence, so
    // the mark is kept at the highest sequence seen (each sequence judged
    // once, never re-judged)
    if ( sequence > tracker->most_recent_packet_received )
    {
        tracker->most_recent_packet_received = sequence;
    }
}

inline int rocketnet_packet_loss_tracker_update( rocketnet_packet_loss_tracker_t * tracker )
{
    rocketnet_packet_loss_tracker_verify_sentinels( tracker );

    int lost_packets = 0;

    uint64_t start = tracker->last_packet_processed + 1;
    uint64_t finish = ( tracker->most_recent_packet_received > ROCKETNET_PACKET_LOSS_TRACKER_SAFETY ) ? ( tracker->most_recent_packet_received - ROCKETNET_PACKET_LOSS_TRACKER_SAFETY ) : 0;

    if ( finish > start && finish - start >= ROCKETNET_PACKET_LOSS_TRACKER_HISTORY )
    {
        tracker->last_packet_processed = tracker->most_recent_packet_received;
        return 0;
    }

    for ( uint64_t sequence = start; sequence <= finish; ++sequence )
    {
        const int index = int( sequence % ROCKETNET_PACKET_LOSS_TRACKER_HISTORY );
        if ( tracker->received_packets[index] != sequence )
        {
            lost_packets++;
        }
    }

    tracker->last_packet_processed = finish;

    return lost_packets;
}

#endif // #ifndef ROCKETNET_PACKET_LOSS_TRACKER_H

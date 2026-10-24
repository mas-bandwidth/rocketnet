/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_PACKET_LOSS_TRACKER_H
#define ROCKETNET_PACKET_LOSS_TRACKER_H

#include "rocketnet_constants.h"
#include "rocketnet_memory_checks.h"
#include <limits.h>

struct rocketnet_packet_loss_tracker_t
{
    ROCKETNET_DECLARE_SENTINEL(0)

    uint64_t last_packet_processed;
    uint64_t most_recent_packet_received;
    // distinct sequences received above last_packet_processed (not yet
    // judged); lets an update judge a span longer than the history ring
    uint64_t packets_received_ahead;

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
    tracker->packets_received_ahead = 0;

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

    // count a sequence not yet judged, once: a duplicate whose slot already
    // holds it is not counted again
    if ( sequence > tracker->last_packet_processed && tracker->received_packets[index] != sequence )
    {
        tracker->packets_received_ahead++;
    }

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

// the number of sequences in (finish, most_recent] the history ring holds: the
// safety margin, at most ROCKETNET_PACKET_LOSS_TRACKER_SAFETY slots
inline uint64_t rocketnet_packet_loss_tracker_margin_received( const rocketnet_packet_loss_tracker_t * tracker, uint64_t finish )
{
    uint64_t received = 0;
    for ( uint64_t sequence = finish + 1; sequence <= tracker->most_recent_packet_received; ++sequence )
    {
        const int index = int( sequence % ROCKETNET_PACKET_LOSS_TRACKER_HISTORY );
        if ( tracker->received_packets[index] == sequence )
        {
            received++;
        }
    }
    return received;
}

inline int rocketnet_packet_loss_tracker_update( rocketnet_packet_loss_tracker_t * tracker )
{
    rocketnet_packet_loss_tracker_verify_sentinels( tracker );

    int lost_packets = 0;

    uint64_t start = tracker->last_packet_processed + 1;
    uint64_t finish = ( tracker->most_recent_packet_received > ROCKETNET_PACKET_LOSS_TRACKER_SAFETY ) ? ( tracker->most_recent_packet_received - ROCKETNET_PACKET_LOSS_TRACKER_SAFETY ) : 0;

    if ( finish > start && finish - start >= ROCKETNET_PACKET_LOSS_TRACKER_HISTORY )
    {
        // the span [start, finish] is longer than the history ring, so the
        // ring alone cannot judge its older part (those slots were
        // overwritten). Judge it by count instead: lost is the span length
        // minus the distinct sequences received in it, which is the running
        // count of sequences received above last_packet_processed less the
        // ones received in the safety margin (finish, most_recent] (read from
        // the ring, at most SAFETY slots). The margin's count is carried
        // forward and the margin is judged next time, as in the scan below.
        // Returning zero here would read a long stall as no loss; counting the
        // whole span as lost would read a long received burst as total loss.
        const uint64_t span = finish - start + 1;
        const uint64_t margin_received = rocketnet_packet_loss_tracker_margin_received( tracker, finish );
        const uint64_t span_received = ( tracker->packets_received_ahead > margin_received ) ? ( tracker->packets_received_ahead - margin_received ) : 0;
        const uint64_t span_lost = ( span > span_received ) ? ( span - span_received ) : 0;

        tracker->last_packet_processed = finish;
        tracker->packets_received_ahead = margin_received;

        return ( span_lost > (uint64_t) INT_MAX ) ? INT_MAX : (int) span_lost;
    }

    for ( uint64_t sequence = start; sequence <= finish; ++sequence )
    {
        const int index = int( sequence % ROCKETNET_PACKET_LOSS_TRACKER_HISTORY );
        if ( tracker->received_packets[index] != sequence )
        {
            lost_packets++;
        }
    }

    if ( finish >= start )
    {
        tracker->packets_received_ahead = rocketnet_packet_loss_tracker_margin_received( tracker, finish );
    }

    tracker->last_packet_processed = finish;

    return lost_packets;
}

#endif // #ifndef ROCKETNET_PACKET_LOSS_TRACKER_H

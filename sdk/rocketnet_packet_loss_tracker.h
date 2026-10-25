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

// The greatest number of packets one update reports lost. Between two updates
// a sequence distance is unbounded, but the server's lease table admits at
// most ROCKETNET_SERVER_PACKET_LEASES / 2 = 65536 packets in one update, the
// largest per-update send burst the protocol has, and a 1500-byte stream
// cannot carry more than that in the time a stalled update covers. A corrupt
// or renamed sequence beyond it is therefore clamped here rather than
// saturating INT_MAX and poisoning the lifetime loss rate. The count stays
// exact for every burst a client can actually receive.
#define ROCKETNET_PACKET_LOSS_TRACKER_MAX_LOST_PER_UPDATE                65536

ROCKETNET_STATIC_ASSERT( ROCKETNET_PACKET_LOSS_TRACKER_MAX_LOST_PER_UPDATE <= INT_MAX,
                         "one update's lost count fits the int the client adds to its stats" );

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

    // A sequence already judged cannot affect either the count or a ring slot.
    if ( sequence <= tracker->last_packet_processed )
    {
        return;
    }

    const int index = int( sequence % ROCKETNET_PACKET_LOSS_TRACKER_HISTORY );
    const uint64_t resident = tracker->received_packets[index];

    // Count a sequence not yet judged once while its slot still identifies it.
    // Production callers reject replay duplicates before this tracker, so an
    // older sequence sharing a slot with a newer resident is a distinct receive.
    if ( resident != sequence )
    {
        tracker->packets_received_ahead++;
    }

    // An older sequence must not erase a newer unjudged receive in the same
    // slot. Empty and older slots can safely take this sequence.
    if ( resident == 0xFFFFFFFFFFFFFFFFUL || resident < sequence )
    {
        tracker->received_packets[index] = sequence;
    }
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

    if ( tracker->most_recent_packet_received >= start &&
         tracker->most_recent_packet_received - start >= ROCKETNET_PACKET_LOSS_TRACKER_HISTORY )
    {
        // The unjudged high-water span is at least the history ring, so newer
        // safety-margin records may have reused slots needed by [start,
        // finish]. Judge it by count instead: lost is the span length
        // minus the distinct sequences received in it, which is the running
        // count of sequences received above last_packet_processed less the
        // ones received in the safety margin (finish, most_recent] (read from
        // the ring, at most SAFETY slots). The margin's count is carried
        // forward and the margin is judged next time, as in the scan below.
        // Returning zero here would read a long stall as no loss; counting the
        // whole span as lost would read a long received burst as total loss.
        // The answer is capped at ROCKETNET_PACKET_LOSS_TRACKER_MAX_LOST_PER_
        // UPDATE: one update reports at most the loss a real stream at the
        // assumed MTU can produce, so a corrupt sequence jump cannot saturate
        // the count.
        const uint64_t span = finish - start + 1;
        const uint64_t margin_received = rocketnet_packet_loss_tracker_margin_received( tracker, finish );
        const uint64_t span_received = ( tracker->packets_received_ahead > margin_received ) ? ( tracker->packets_received_ahead - margin_received ) : 0;
        const uint64_t span_lost = ( span > span_received ) ? ( span - span_received ) : 0;

        tracker->last_packet_processed = finish;
        tracker->packets_received_ahead = margin_received;

        // bound the answer by what one update can actually account for: a
        // sequence jump beyond a real stream's reach is capped, never
        // saturated to INT_MAX
        return (int) ( span_lost > ROCKETNET_PACKET_LOSS_TRACKER_MAX_LOST_PER_UPDATE ? ROCKETNET_PACKET_LOSS_TRACKER_MAX_LOST_PER_UPDATE : span_lost );
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

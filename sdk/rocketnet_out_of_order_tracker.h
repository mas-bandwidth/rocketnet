/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_OUT_OF_ORDER_TRACKER_H
#define ROCKETNET_OUT_OF_ORDER_TRACKER_H

struct rocketnet_out_of_order_tracker_t
{
    ROCKETNET_DECLARE_SENTINEL(0)

    uint64_t last_packet_processed;
    uint64_t num_out_of_order_packets;

    ROCKETNET_DECLARE_SENTINEL(1)
};

inline void rocketnet_out_of_order_tracker_initialize_sentinels( rocketnet_out_of_order_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 0 )
    ROCKETNET_INITIALIZE_SENTINEL( tracker, 1 )
}

inline void rocketnet_out_of_order_tracker_verify_sentinels( rocketnet_out_of_order_tracker_t * tracker )
{
    (void) tracker;
    rocketnet_assert( tracker );
    ROCKETNET_VERIFY_SENTINEL( tracker, 0 )
    ROCKETNET_VERIFY_SENTINEL( tracker, 1 )
}

inline void rocketnet_out_of_order_tracker_reset( rocketnet_out_of_order_tracker_t * tracker )
{
    rocketnet_assert( tracker );

    rocketnet_out_of_order_tracker_initialize_sentinels( tracker );

    tracker->last_packet_processed = 0;
    tracker->num_out_of_order_packets = 0;

    rocketnet_out_of_order_tracker_verify_sentinels( tracker );
}

inline void rocketnet_out_of_order_tracker_packet_received( rocketnet_out_of_order_tracker_t * tracker, uint64_t sequence )
{
    rocketnet_out_of_order_tracker_verify_sentinels( tracker );

    if ( sequence < tracker->last_packet_processed )
    {
        tracker->num_out_of_order_packets++;
        return;
    }

    tracker->last_packet_processed = sequence;
}

#endif // #ifndef ROCKETNET_OUT_OF_ORDER_TRACKER_H

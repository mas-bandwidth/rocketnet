/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_STANDARD_RULES_H
#define ROCKETNET_SDK_ROCKETNET_STANDARD_RULES_H

#include <string.h>

#include "rocketnet.h"
#include "rocketnet_message.h"

#ifdef __cplusplus

// The unreliable send path's one-record limit, from the standard's m2.1 rule:
// the largest message whose record header and form byte fit the one-packet
// capacity. The header is the one rocketnet_message_record_header_bytes
// writes for the record's code and channel.
static inline int rn_endpoint_unreliable_record_limit( int reliability, int channel, int capacity )
{
    const bool sequenced = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED;

    struct rocketnet_message_record_t shape;
    memset( &shape, 0, sizeof( shape ) );
    shape.code = sequenced ? ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED : ROCKETNET_MESSAGE_CODE_UNRELIABLE;
    shape.channel = (uint32_t) channel;

    const int one_packet_capacity = sequenced ? capacity : ( capacity < ROCKETNET_MESSAGE_SINGLE_BYTES ? capacity : ROCKETNET_MESSAGE_SINGLE_BYTES );

    return one_packet_capacity - rocketnet_message_record_header_bytes( &shape ) - 1;
}

// The unreliable send path's record-size comparison: over when the message is
// above its one-record limit.
static inline bool rn_endpoint_unreliable_over_record_capacity( int reliability, int channel, int bytes, int capacity )
{
    return (int64_t) (uint32_t) bytes > (int64_t) rn_endpoint_unreliable_record_limit( reliability, channel, capacity );
}

#endif // #ifdef __cplusplus

#endif // ROCKETNET_SDK_ROCKETNET_STANDARD_RULES_H

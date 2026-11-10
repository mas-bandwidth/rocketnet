/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_STANDARD_RULES_H
#define ROCKETNET_SDK_ROCKETNET_STANDARD_RULES_H

#include <string.h>

#include "rocketnet.h"
#include "rocketnet_channel.h"
#include "rocketnet_message.h"

#ifdef __cplusplus

// The record header bytes of the standard's m2.1 limit rule for one code and
// channel width. This is the same geometry rocketnet_message_flags_header_bytes
// applies to a record's flags; only the fields a one-record shape sets count.
static inline int rn_standard_record_header_bytes( int code, int channel )
{
    const bool wide = channel >= ROCKETNET_MESSAGE_WIDE_CHANNEL;
    int bytes = ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
    if ( code == ROCKETNET_MESSAGE_CODE_RELIABLE || code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED || code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        bytes += ROCKETNET_MESSAGE_NUMBER_BYTES;
    if ( code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED || code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        bytes += 2;
    if ( code == ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED || code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        bytes += 3;
    if ( code == ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED || code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED || code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        bytes += 3 + ( wide ? 2 : 1 );
    return bytes;
}

// The one-record limits of standard/MESSAGES.md m2.1, from the formulas the
// send paths use. Pure: no state and no misuse count. The unchecked form is
// the rule itself; the checked form is the public function's channel range.
static inline int rn_standard_max_unfragmented_message_bytes_unchecked( enum rocketnet_reliability_t reliability, int channel )
{
    static_assert( sizeof( reliability ) == sizeof( int ), "rocketnet_reliability_t is int-sized" );
    int value = 0;
    memcpy( &value, &reliability, sizeof( value ) );

    switch ( value )
    {
        case ROCKETNET_RELIABILITY_UNRELIABLE:
        case ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT:
            return ROCKETNET_MESSAGE_SINGLE_BYTES - 1 - ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;

        case ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED:
            return ROCKETNET_MESSAGE_SINGLE_BYTES - rn_standard_record_header_bytes( ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, channel );

        case ROCKETNET_RELIABILITY_RELIABLE:
        case ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT:
            return ROCKETNET_MESSAGE_SINGLE_BYTES - 7;

        case ROCKETNET_RELIABILITY_RELIABLE_ORDERED:
        case ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT:
            return ROCKETNET_MESSAGE_SINGLE_BYTES - 1 - ROCKETNET_CHANNEL_ORDERED_HEADER_BYTES;

        case ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED:
            return ROCKETNET_MESSAGE_SINGLE_BYTES - 1 - rn_standard_record_header_bytes( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, channel );

        default:
            return 0;
    }
}

static inline int rn_standard_max_unfragmented_message_bytes( enum rocketnet_reliability_t reliability, int channel )
{
    if ( channel < 0 || channel >= ROCKETNET_MAX_CHANNELS )
        return 0;
    return rn_standard_max_unfragmented_message_bytes_unchecked( reliability, channel );
}

// The unreliable send path's record-size comparison, from the standard's m2.1
// rule: the record header and the form byte fit the one-packet capacity.
static inline bool rn_endpoint_unreliable_over_record_capacity( int reliability, int channel, int bytes, int capacity )
{
    const int header = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? 3 + 3 + 3 + ( channel >= ROCKETNET_MESSAGE_WIDE_CHANNEL ? 2 : 1 ) : ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
    const int one_packet_capacity = reliability == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ? capacity : ( capacity < ROCKETNET_MESSAGE_SINGLE_BYTES ? capacity : ROCKETNET_MESSAGE_SINGLE_BYTES );

    return (uint64_t) (uint32_t) bytes + (uint64_t) header + 1u > (uint64_t) one_packet_capacity;
}

#endif // #ifdef __cplusplus

#endif // ROCKETNET_SDK_ROCKETNET_STANDARD_RULES_H

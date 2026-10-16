/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

// RNReceipts Send: code 5 is accepted by the send mask, then travels as wire
// code 0. Raw wire codes 5, 6 and 7 are never valid dispatch inputs: the
// receiver has no receipt flag and delivers the underlying base record.

int rn_channels_dispatch_receipt( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    (void) record;
    (void) out;

    if ( !channels )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
    return ROCKETNET_CHANNEL_DISPATCH_NONE;
}

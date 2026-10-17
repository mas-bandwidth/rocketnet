/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

// RNReceipts Send and EffectiveKind: receipt types travel as base wire codes
// 0, 2 and 3. Raw wire codes 5, 6 and 7 are never valid dispatch inputs: the
// receiver has no receipt flag and delivers the underlying base record.

// tla/RNReceipts.tla WritePacket
int rn_channels_reliable_receipt_base( int type )
{
    // RNReceipts EffectiveKind, CHANNELS 7.4: the send descriptor keeps the
    // receipt intent in a flag; no receipt code reaches wire.
    if ( type == ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT ||
         type == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT )
        return ROCKETNET_CHANNEL_RECEIPT_BASE( type );
    return -1;
}

// tla/RNReceipts.tla TakeReceipt
int rn_channels_dispatch_receipt( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    (void) record;
    (void) out;

    if ( !channels )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
    return ROCKETNET_CHANNEL_DISPATCH_NONE;
}

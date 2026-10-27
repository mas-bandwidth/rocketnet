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
    // RNReceipts EffectiveKind, CHANNELS 7.4: the send descriptor keeps the receipt intent in a flag; no receipt code reaches wire

    if ( type == ROCKETNET_CHANNEL_TYPE_RELIABLE_WITH_RECEIPT ||
         type == ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED_WITH_RECEIPT )
        return ROCKETNET_CHANNEL_RECEIPT_BASE( type );

    return -1;
}

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

// Codes 5, 6 and 7's dispatch case (docs/CHANNELS.md 7.2, 7.4). This card
// writes the stub: the case's signature, its enabled flag (false) and its
// refusal. The receipts card fills the body -- the receipt ring, ACKED on
// every number the message took, LOSS on eviction or RTO, the ring-full drop
// (MESSAGE_RECEIPT_RING_FULL) -- without touching sdk/rocketnet_channel.cpp.

// false in the first version: no receipt type is enabled (7.5).

static const bool rn_channel_receipts_enabled = false;

int rn_channels_dispatch_receipt( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    (void) record;
    (void) out;

    if ( !channels )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    if ( !rn_channel_receipts_enabled )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    return ROCKETNET_CHANNEL_DISPATCH_NONE;
}

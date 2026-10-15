/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

// Codes 1 and 4's dispatch case (docs/CHANNELS.md 7.2, 7.3). This card writes
// the stub: the case's signature, its enabled flag (false) and its refusal.
// The sequenced card fills the body -- hold the newest per channel, drop the
// superseded (MESSAGE_SEQUENCED_SUPERSEDED), drop the stale
// (MESSAGE_SEQUENCED_STALE), drop when the hold slots are all in use
// (MESSAGE_SEQUENCED_HOLD_FULL) -- without touching sdk/rocketnet_channel.cpp.

// false in the first version: no sequenced type is enabled (7.5).

static const bool rn_channel_sequenced_enabled = false;

int rn_channels_dispatch_sequenced( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    (void) record;
    (void) out;

    if ( !channels )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    if ( !rn_channel_sequenced_enabled )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    return ROCKETNET_CHANNEL_DISPATCH_NONE;
}

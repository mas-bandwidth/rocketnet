/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

// Code 2's dispatch case (docs/CHANNELS.md 7.2). This card writes the stub:
// the case's signature, its enabled flag (false) and its refusal. The
// reliable-unordered card fills the body without touching
// sdk/rocketnet_channel.cpp.

// false in the first version: RELIABLE is live internally only, because the
// CONFIG control record travels as code 2 (7.5). An application record with
// code 2 is the violation MESSAGE_TYPE_NOT_ENABLED.

static const bool rn_channel_reliable_enabled = false;

int rn_channels_dispatch_reliable( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    (void) record;
    (void) out;

    if ( !channels )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    if ( !rn_channel_reliable_enabled )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    return ROCKETNET_CHANNEL_DISPATCH_NONE;
}

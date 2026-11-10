/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_STANDARD_PINS_H
#define ROCKETNET_SDK_ROCKETNET_STANDARD_PINS_H

#include "rocketnet_standard_rules.h"

#ifdef __cplusplus

// The standard's pin translation unit compiles the exported rule function
// against the same inline implementation the send paths use, so a pin names
// rocketnet_max_unfragmented_message_bytes and never reimplements it. The WIDE
// form asks the same rule at the standard's first WIDE channel, which a default
// build's channel count does not admit through the public function.
extern "C" {

inline int rocketnet_max_unfragmented_message_bytes( enum rocketnet_reliability_t reliability, int channel )
{
    return rn_standard_max_unfragmented_message_bytes( reliability, channel );
}

inline int rocketnet_max_unfragmented_message_bytes_wide( enum rocketnet_reliability_t reliability )
{
    return rn_standard_max_unfragmented_message_bytes_unchecked( reliability, ROCKETNET_MESSAGE_WIDE_CHANNEL );
}

}

#endif // #ifdef __cplusplus

#endif // ROCKETNET_SDK_ROCKETNET_STANDARD_PINS_H

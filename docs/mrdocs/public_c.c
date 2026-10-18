/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

/* Translation unit that includes the five C-compatible public SDK headers
   (rocketnet.h, rocketnet_address.h, rocketnet_connect_token.h,
   rocketnet_platform.h, rocketnet_counters.h).
   rocketnet.hpp and rocketnet_route.h are C++ headers (rocketnet.hpp defines
   namespace rocketnet and C++ classes; rocketnet_route.h uses C++ functional
   casts uint32_t(...) and unprefixed struct type names). */

#include "rocketnet.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "rocketnet_counters.h"

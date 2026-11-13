/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/if_vlan.h>: the XDP filter includes it and uses
// nothing from it (the uapi header has no tag struct; the filter defines its
// own). ETH_P_8021Q comes from the shim's <linux/if_ether.h>.

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_IF_VLAN_H
#define ROCKETNET_XDP_HOST_LINUX_IF_VLAN_H

#include "linux/if_ether.h"

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_IF_VLAN_H

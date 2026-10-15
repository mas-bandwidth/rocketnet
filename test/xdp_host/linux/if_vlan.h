/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/if_vlan.h>: the XDP filter includes it and uses
// nothing from it. A VLAN-tagged frame carries ETH_P_8021Q in h_proto, which
// the filter passes untouched.

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_IF_VLAN_H
#define ROCKETNET_XDP_HOST_LINUX_IF_VLAN_H

#include "linux/if_ether.h"

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_IF_VLAN_H

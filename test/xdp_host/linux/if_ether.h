/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/if_ether.h>: the Ethernet header and the
// ethertypes the XDP filter names. Packed, as the kernel's is.

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_IF_ETHER_H
#define ROCKETNET_XDP_HOST_LINUX_IF_ETHER_H

#include "linux/bpf.h"

#define ETH_ALEN        6
#define ETH_P_IP        0x0800
#define ETH_P_8021Q     0x8100
#define ETH_P_IPV6      0x86DD

struct ethhdr
{
    unsigned char h_dest[ETH_ALEN];
    unsigned char h_source[ETH_ALEN];
    __be16 h_proto;
} __attribute__((packed));

_Static_assert( sizeof(struct ethhdr) == 14, "the Ethernet header is 14 bytes" );

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_IF_ETHER_H

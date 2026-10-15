/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/in.h>: the IP protocol numbers the XDP filter
// names (IANA protocol numbers; RFC 8200 section 4 for the IPv6 extension
// headers).

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_IN_H
#define ROCKETNET_XDP_HOST_LINUX_IN_H

#include "linux/bpf.h"

enum
{
    IPPROTO_HOPOPTS = 0,
    IPPROTO_UDP = 17,
    IPPROTO_ROUTING = 43,
    IPPROTO_FRAGMENT = 44,
    IPPROTO_ESP = 50,
    IPPROTO_AH = 51,
    IPPROTO_ICMPV6 = 58,
    IPPROTO_DSTOPTS = 60,
};

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_IN_H

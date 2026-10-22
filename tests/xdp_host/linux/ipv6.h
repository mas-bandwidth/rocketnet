/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/ipv6.h>: the fixed IPv6 header (RFC 8200 section
// 3) with the kernel's in6_addr union. Packed so a header at any frame offset
// is read safely; the layout has no padding either way.

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_IPV6_H
#define ROCKETNET_XDP_HOST_LINUX_IPV6_H

#include "linux/bpf.h"
#include "linux/in.h"

struct in6_addr
{
    union
    {
        __u8 u6_addr8[16];
        __be16 u6_addr16[8];
        __be32 u6_addr32[4];
    } __attribute__((packed)) in6_u;
} __attribute__((packed));

struct ipv6hdr
{
    __u8 priority:4,
         version:4;
    __u8 flow_lbl[3];
    __be16 payload_len;
    __u8 nexthdr;
    __u8 hop_limit;
    struct in6_addr saddr;
    struct in6_addr daddr;
} __attribute__((packed));

_Static_assert( sizeof(struct in6_addr) == 16, "an IPv6 address is 16 bytes" );
_Static_assert( sizeof(struct ipv6hdr) == 40, "the fixed IPv6 header is 40 bytes" );

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_IPV6_H

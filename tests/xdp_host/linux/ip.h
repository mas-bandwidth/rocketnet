/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/ip.h>: the IPv4 header (RFC 791 section 3.1) in
// the kernel's little-endian bitfield order, ihl in the low nibble of the
// first byte and version in the high. The shim asserts a little-endian host
// in linux/bpf.h. Packed so a header at any frame offset is read safely; the
// layout has no padding either way.

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_IP_H
#define ROCKETNET_XDP_HOST_LINUX_IP_H

#include "linux/bpf.h"

struct iphdr
{
    __u8 ihl:4,
         version:4;
    __u8 tos;
    __be16 tot_len;
    __be16 id;
    __be16 frag_off;
    __u8 ttl;
    __u8 protocol;
    __sum16 check;
    __be32 saddr;
    __be32 daddr;
} __attribute__((packed));

_Static_assert( sizeof(struct iphdr) == 20, "the IPv4 header without options is 20 bytes" );

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_IP_H

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/bpf.h>: only what server_xdp/server_xdp.c uses.
// Part of the shadow include directory that compiles the XDP filter as
// ordinary host C (tests/xdp_host/server_xdp_host.c); never on the include
// path of anything else.

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_BPF_H
#define ROCKETNET_XDP_HOST_LINUX_BPF_H

#include <stddef.h>

typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u16 __sum16;

_Static_assert( sizeof(__u8) == 1 && sizeof(__u16) == 2 && sizeof(__u32) == 4 && sizeof(__u64) == 8, "the __u types are their names' widths" );

// the host's byte order: every network-order field in the shim is read as
// the kernel reads it on a little-endian machine

_Static_assert( __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the XDP host shim models a little-endian host" );

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#define __constant_htons(x) ((__be16)((((__u16)(x) & 0x00ffu) << 8) | (((__u16)(x) & 0xff00u) >> 8)))

enum bpf_map_type
{
    BPF_MAP_TYPE_HASH = 1,
    BPF_MAP_TYPE_ARRAY = 2,
    BPF_MAP_TYPE_PERCPU_ARRAY = 6,
    BPF_MAP_TYPE_XSKMAP = 17,
};

enum xdp_action
{
    XDP_ABORTED = 0,
    XDP_DROP,
    XDP_PASS,
    XDP_TX,
    XDP_REDIRECT,
};

enum
{
    BPF_F_INGRESS = (1ULL << 0),
};

// The one deliberate difference from the kernel's struct: data and data_end
// are unsigned long, wide enough for a host pointer, so the filter's
// (void*)(long)ctx->data yields the frame's real address.

struct xdp_md
{
    unsigned long data;
    unsigned long data_end;
    __u32 data_meta;
    __u32 ingress_ifindex;
    __u32 rx_queue_index;
    __u32 egress_ifindex;
};

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_BPF_H

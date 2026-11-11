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

// The byte order is the target's, never a copy of one machine's. Where the
// target's own kernel headers are on the include path (any Linux build,
// native or cross), <asm/byteorder.h> is the kernel's: it brings the __u and
// __be types, __constant_htons and the bitfield order exactly as the BPF
// compile of the filter sees them on that machine. Elsewhere (macOS) the
// same three are defined here from the compiler's __BYTE_ORDER__, as the
// kernel defines them. Either way a big-endian target reads every
// network-order field without a swap, so a conversion in the filter that
// swaps whatever the byte order is fails the filter cases there.

#if defined( __has_include )
#if __has_include( <asm/byteorder.h> )
#define ROCKETNET_XDP_HOST_KERNEL_BYTEORDER 1
#endif // #if __has_include( <asm/byteorder.h> )
#endif // #if defined( __has_include )

#if defined( ROCKETNET_XDP_HOST_KERNEL_BYTEORDER )

#include <asm/byteorder.h>

#else // #if defined( ROCKETNET_XDP_HOST_KERNEL_BYTEORDER )

typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u16 __sum16;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define __LITTLE_ENDIAN_BITFIELD
#define __constant_htons(x) ((__be16)((((__u16)(x) & 0x00ffu) << 8) | (((__u16)(x) & 0xff00u) >> 8)))
#define __cpu_to_be16(x) ((__be16)__builtin_bswap16((__u16)(x)))
#define __be16_to_cpu(x) ((__u16)__builtin_bswap16((__u16)(x)))
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define __BIG_ENDIAN_BITFIELD
#define __constant_htons(x) ((__be16)(__u16)(x))
#define __cpu_to_be16(x) ((__be16)(__u16)(x))
#define __be16_to_cpu(x) ((__u16)(__be16)(x))
#else // #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#error "the XDP host shim needs __BYTE_ORDER__ to be little or big endian"
#endif // #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#endif // #if defined( ROCKETNET_XDP_HOST_KERNEL_BYTEORDER )

_Static_assert( sizeof(__u8) == 1 && sizeof(__u16) == 2 && sizeof(__u32) == 4 && sizeof(__u64) == 8, "the __u types are their names' widths" );

#if defined( __LITTLE_ENDIAN_BITFIELD ) == defined( __BIG_ENDIAN_BITFIELD )
#error "the XDP host shim needs exactly one of __LITTLE_ENDIAN_BITFIELD and __BIG_ENDIAN_BITFIELD"
#endif // #if defined( __LITTLE_ENDIAN_BITFIELD ) == defined( __BIG_ENDIAN_BITFIELD )

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

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

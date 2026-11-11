/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for libbpf's <bpf/bpf_endian.h>. Where libbpf's own header
// is on the include path it is the one used, so the filter's conversions are
// libbpf's for the target. Elsewhere (macOS) bpf_ntohs and its siblings are
// defined here the way libbpf defines them, from the compiler's
// __BYTE_ORDER__: a swap on a little-endian target, the identity on a
// big-endian one.

#pragma once

#ifndef ROCKETNET_XDP_HOST_BPF_ENDIAN_H
#define ROCKETNET_XDP_HOST_BPF_ENDIAN_H

#include "linux/bpf.h"

#if defined( __has_include_next )
#if __has_include_next( <bpf/bpf_endian.h> )
#define ROCKETNET_XDP_HOST_LIBBPF_ENDIAN 1
#endif // #if __has_include_next( <bpf/bpf_endian.h> )
#endif // #if defined( __has_include_next )

#if defined( ROCKETNET_XDP_HOST_LIBBPF_ENDIAN )

#include_next <bpf/bpf_endian.h>

#else // #if defined( ROCKETNET_XDP_HOST_LIBBPF_ENDIAN )

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define bpf_htons(x) ((__be16)__builtin_bswap16((__u16)(x)))
#define bpf_ntohs(x) ((__u16)__builtin_bswap16((__u16)(x)))
#define bpf_htonl(x) ((__be32)__builtin_bswap32((__u32)(x)))
#define bpf_ntohl(x) ((__u32)__builtin_bswap32((__u32)(x)))
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define bpf_htons(x) ((__be16)(__u16)(x))
#define bpf_ntohs(x) ((__u16)(x))
#define bpf_htonl(x) ((__be32)(__u32)(x))
#define bpf_ntohl(x) ((__u32)(x))
#else // #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#error "the XDP host shim needs __BYTE_ORDER__ to be little or big endian"
#endif // #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#endif // #if defined( ROCKETNET_XDP_HOST_LIBBPF_ENDIAN )

#endif // #ifndef ROCKETNET_XDP_HOST_BPF_ENDIAN_H

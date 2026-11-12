/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for libbpf's <bpf/bpf_endian.h>: bpf_ntohs as libbpf defines
// it, a swap on a little-endian target and none on a big-endian one, keyed on
// the compiler's __BYTE_ORDER__ as libbpf's is.

#pragma once

#ifndef ROCKETNET_XDP_HOST_BPF_ENDIAN_H
#define ROCKETNET_XDP_HOST_BPF_ENDIAN_H

#include "linux/bpf.h"

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define bpf_ntohs(x) ((__u16) __builtin_bswap16((__u16)(x)))
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define bpf_ntohs(x) ((__u16)(x))
#else
#error "the XDP host shim needs a little-endian or big-endian host"
#endif

#endif // #ifndef ROCKETNET_XDP_HOST_BPF_ENDIAN_H

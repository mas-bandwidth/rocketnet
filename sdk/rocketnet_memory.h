/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once
#include <stdint.h>

#ifndef ROCKETNET_REASSEMBLY_MEMORY_MIB
#define ROCKETNET_REASSEMBLY_MEMORY_MIB 512
#endif
#ifndef ROCKETNET_XDP_MEMORY_MIB
#define ROCKETNET_XDP_MEMORY_MIB 4096
#endif

// Component budgets are explicit build choices; they do not estimate available
// host RAM or authorize overcommit. Callers must leave room for other components,
// allocator overhead, thread stacks, kernel buffers and their application.
#if ROCKETNET_REASSEMBLY_MEMORY_MIB < 1 || ROCKETNET_REASSEMBLY_MEMORY_MIB > 1048576 || ROCKETNET_XDP_MEMORY_MIB < 1 || ROCKETNET_XDP_MEMORY_MIB > 1048576
#error RocketNet memory budgets must be 1..1048576 MiB
#endif

inline bool rocketnet_memory_add(uint64_t & total, uint64_t count, uint64_t stride)
{
    if (stride && count > (UINT64_MAX - total) / stride) return false;
    total += count * stride;
    return true;
}

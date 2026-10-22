/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <initializer_list>

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

// The allocator's alignment and ownership header, measured in its defining TU.
uint64_t rocketnet_allocation_overhead();
inline uint64_t rocketnet_memory_allocations(std::initializer_list<uint64_t> sizes)
{
    uint64_t total = 0;
    const uint64_t overhead = rocketnet_allocation_overhead();
    for (uint64_t bytes : sizes)
    {
        if (bytes > SIZE_MAX - overhead || !rocketnet_memory_add(total, 1, bytes + overhead)) return 0;
    }
    return total <= SIZE_MAX ? total : 0;
}

// Internal component accounting stays beside the corresponding concrete types.
uint64_t rocketnet_client_slots_allocation_bytes(int clients);
uint64_t rocketnet_encryption_map_allocation_bytes(int capacity);
uint64_t rocketnet_token_table_allocation_bytes(int capacity);
uint64_t rocketnet_server_acks_allocation_bytes(int clients);
uint64_t rocketnet_send_arena_allocation_bytes(int slots, int bytes);
uint64_t rocketnet_platform_socket_allocation_bytes();
uint64_t rocketnet_platform_thread_allocation_bytes();
uint64_t rocketnet_server_gso_allocation_bytes(int queues);
uint64_t rocketnet_server_iocp_allocation_bytes(int workers);
uint64_t rocketnet_server_xdp_allocation_bytes(int queues);
uint64_t rocketnet_server_rio_registered_region_bytes();
// UMEM bytes for a queue count. Plan and the XDP preflight share this; do not restate the product.
uint64_t rocketnet_xdp_umem_bytes(int queues);

struct rocketnet_server_transport_config_t;
struct rocketnet_server_memory_plan_t
{
    uint64_t transport_bytes, protocol_bytes, reassembly_bytes, send_arena_bytes;
    uint64_t transport_io_bytes, umem_bytes, registered_region_bytes, total_bytes;
    int worker_threads, sockets;
};
bool rocketnet_server_transport_memory_plan(const rocketnet_server_transport_config_t * config,
    const char * transport, rocketnet_server_memory_plan_t & plan);
// Native built-in adapter resource policy; fills transport, UMEM, RIO registered
// region, worker and socket fields.
// the xdp transport's queue count when num_xdp_queues is zero or negative; the
// xdp create and rocketnet_builtin_server_memory_plan both take it
#ifndef ROCKETNET_XDP_DEFAULT_QUEUES
#define ROCKETNET_XDP_DEFAULT_QUEUES 7
#endif

bool rocketnet_builtin_server_memory_plan(const rocketnet_server_transport_config_t * config,
    const char * transport, rocketnet_server_memory_plan_t & plan);

/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_MEMORY_H
#define ROCKETNET_SDK_ROCKETNET_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <initializer_list>
#include <string.h>

#include "rocketnet.h"                          // ROCKETNET_MAX_CLIENTS

#ifndef ROCKETNET_XDP_MEMORY_MIB
#define ROCKETNET_XDP_MEMORY_MIB 4096
#endif // #ifndef ROCKETNET_XDP_MEMORY_MIB

// Component budgets are explicit build choices; they do not estimate available
// host RAM or authorize overcommit. Callers must leave room for other components,
// allocator overhead, thread stacks, kernel buffers and their application.
#if ROCKETNET_XDP_MEMORY_MIB < 1 || ROCKETNET_XDP_MEMORY_MIB > 1048576
#error RocketNet memory budgets must be 1..1048576 MiB
#endif // #if ROCKETNET_XDP_MEMORY_MIB < 1 || ROCKETNET_XDP_MEMORY_MIB > 1048576

inline bool rocketnet_memory_add( uint64_t & total, uint64_t count, uint64_t stride )
{
    if ( stride && count > ( UINT64_MAX - total ) / stride )
        return false;

    total += count * stride;
    return true;
}

// the bytes rocketnet_malloc adds to each allocation
uint64_t rocketnet_allocation_overhead();

inline uint64_t rocketnet_memory_allocations( std::initializer_list<uint64_t> sizes )
{
    uint64_t total = 0;
    const uint64_t overhead = rocketnet_allocation_overhead();

    for ( uint64_t bytes : sizes )
    {
        if ( bytes > SIZE_MAX - overhead || !rocketnet_memory_add( total, 1, bytes + overhead ) )
            return 0;
    }

    return total <= SIZE_MAX ? total : 0;
}

// The bytes the SDK holds in blocks it takes from the OS itself, outside the
// installed allocator: the blocks a transport registers with or maps from the
// kernel (the AF_XDP UMEM and rings, the io_uring rings and provided buffer
// rings; the RIO registered region comes from the allocator).
// A create adds a block's bytes once the OS grants it and its destroy removes
// them at the release, both on the creating or destroying thread, never in
// update work, so the count is back at its start once every transport is
// destroyed. The plan names the UMEM apart from allocator bytes; this count is what a memory check adds to the
// allocator's to cover every byte the library holds. The reader is extern "C"
// so the C soak tool reads it beside its allocator count.
void rocketnet_direct_allocation_add( uint64_t bytes );
void rocketnet_direct_allocation_remove( uint64_t bytes );
extern "C" uint64_t rocketnet_direct_allocation_bytes( void );

// internal component accounting stays beside the corresponding concrete types
uint64_t rocketnet_client_slots_allocation_bytes( int clients );
uint64_t rocketnet_encryption_map_allocation_bytes( int capacity );
uint64_t rocketnet_token_table_allocation_bytes( int capacity );
uint64_t rocketnet_server_acks_allocation_bytes( int clients );
uint64_t rocketnet_platform_socket_allocation_bytes();
uint64_t rocketnet_platform_thread_allocation_bytes();
uint64_t rocketnet_server_gso_allocation_bytes( int queues );
uint64_t rocketnet_server_iocp_allocation_bytes( int workers );
uint64_t rocketnet_server_xdp_allocation_bytes( int queues );
uint64_t rocketnet_server_rio_registered_region_bytes();

// UMEM bytes for a queue count. Plan and the XDP preflight share this; do not restate the product.
uint64_t rocketnet_xdp_umem_bytes( int queues );

// The OS-owned memory a transport takes at connect or start that never comes
// from the caller's allocator: the default stack of each thread
// rocketnet_platform_thread_create starts (pthread_create with NULL
// attributes, CreateThread with zero: the platform default), and on Linux the
// io_uring provided buffer rings, which liburing or the fallback mmap.
uint64_t rocketnet_thread_stack_bytes();
uint64_t rocketnet_server_uring_mapped_bytes( int queues );

// allocator_bytes is the most the caller's allocator holds for one transport
// generation, every rocketnet_malloc with its overhead; os_bytes is what the
// OS owns beside it (page-aligned UMEM, kernel-mapped rings, thread stacks).
// total_bytes is the operator budget's figure: allocator bytes plus UMEM.
struct rocketnet_server_transport_config_t;
struct rocketnet_server_memory_plan_t
{
    uint64_t transport_bytes, protocol_bytes;
    uint64_t transport_io_bytes, umem_bytes, registered_region_bytes, total_bytes;
    uint64_t allocator_bytes, thread_stack_bytes, mapped_ring_bytes, os_bytes;
    int worker_threads, sockets;
};
bool rocketnet_server_transport_memory_plan( const rocketnet_server_transport_config_t * config,
                                             const char * transport, rocketnet_server_memory_plan_t & plan );

// Native built-in adapter resource policy; fills transport, UMEM, RIO registered
// region, worker and socket fields.
// the xdp transport's queue count when num_xdp_queues is zero or negative; the
// xdp create and rocketnet_builtin_server_memory_plan both take it
#ifndef ROCKETNET_XDP_DEFAULT_QUEUES
#define ROCKETNET_XDP_DEFAULT_QUEUES 7
#endif // #ifndef ROCKETNET_XDP_DEFAULT_QUEUES

bool rocketnet_builtin_server_memory_plan( const rocketnet_server_transport_config_t * config,
                                           const char * transport, rocketnet_server_memory_plan_t & plan );

// The most the caller's allocator holds for any candidate of the config's
// transport list, the one start may grant: the transport generation start
// creates, which stop retires and the next start frees before it creates the
// next. A candidate with no plan (a custom transport) adds nothing.
uint64_t rocketnet_server_transport_ladder_allocator_bytes( const rocketnet_server_transport_config_t * config );

struct rocketnet_client_transport_config_t;
uint64_t rocketnet_client_transport_ladder_allocator_bytes( const rocketnet_client_transport_config_t * config );

// Zero an object's raw storage before it is constructed, except its buffers:
// skip lists them as (offset, bytes) spans, ascending and disjoint. A buffer
// is written before it is read, so its pages are touched only by use.
//
// Construct with `new ( p ) T;`, never `T()`, which would zero the buffers
// too. The types built this way have implicit constructors only (std::atomic
// members set their own value), so GCC's lifetime dead-store elimination
// never discards these stores.
struct rocketnet_span_t
{
    size_t offset;
    size_t bytes;
};

inline void rocketnet_zero_except( void * storage, size_t bytes, const rocketnet_span_t * skip, int num_skip )
{
    uint8_t * base = (uint8_t *) storage;
    size_t at = 0;

    for ( int i = 0; i < num_skip; i++ )
    {
        if ( skip[i].offset > at )
            memset( base + at, 0, skip[i].offset - at );
        at = skip[i].offset + skip[i].bytes;
    }

    if ( at < bytes )
        memset( base + at, 0, bytes - at );
}

#endif // ROCKETNET_SDK_ROCKETNET_MEMORY_H

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
    {
        return false;
    }

    total += count * stride;
    return true;
}

// Red zones, in AddressSanitizer builds only. Arrays carved from one
// allocation sit back to back, so a read or write one past an array lands in
// the next array's bytes, which ASan sees as addressable. In an ASan build
// every carve helper leaves ROCKETNET_RED_ZONE_BYTES after each carved array
// and poisons them, so ASan reports a one-past access; rocketnet_malloc does
// the same after each block it returns (its alignment slack is otherwise
// addressable). The plans count the pads through the same layout functions,
// so a plan stays exact in every build. In every other build the size is 0
// and none of this is compiled: layouts, plans and the library are unchanged.
#if defined( __SANITIZE_ADDRESS__ )
#define ROCKETNET_RED_ZONES 1
#elif defined( __has_feature )
#if __has_feature( address_sanitizer )
#define ROCKETNET_RED_ZONES 1
#endif // #if __has_feature( address_sanitizer )
#endif // #if defined( __SANITIZE_ADDRESS__ )

#ifndef ROCKETNET_RED_ZONES
#define ROCKETNET_RED_ZONES 0
#endif // #ifndef ROCKETNET_RED_ZONES

#if ROCKETNET_RED_ZONES

#include <sanitizer/asan_interface.h>

// one allocation alignment, so every offset after a pad keeps its alignment
#define ROCKETNET_RED_ZONE_BYTES 64

inline void rocketnet_red_zone_poison( const void * p, uint64_t bytes )
{
    ASAN_POISON_MEMORY_REGION( p, (size_t) bytes );
}

inline void rocketnet_red_zone_unpoison( const void * p, uint64_t bytes )
{
    ASAN_UNPOISON_MEMORY_REGION( p, (size_t) bytes );
}

#if ROCKETNET_DEV

// the block a create is carving: while a scope names it on this thread, each
// rocketnet_carve poisons the pad it leaves at that block's offset. A plan
// carves with no scope and poisons nothing. A thread-local, so only in a
// development build (tools/check_no_thread_local.py): a production ASan build
// plans the same pads but leaves the transports' carved pads unpoisoned
// (rocketnet_malloc's, the message region's and the message object's are
// poisoned in every ASan build).
inline uint8_t *& rocketnet_red_zone_carve_base()
{
    static thread_local uint8_t * base = nullptr;
    return base;
}

struct rocketnet_red_zone_scope_t
{
    explicit rocketnet_red_zone_scope_t( void * base ) { rocketnet_red_zone_carve_base() = (uint8_t *) base; }
    ~rocketnet_red_zone_scope_t() { rocketnet_red_zone_carve_base() = nullptr; }
    rocketnet_red_zone_scope_t( const rocketnet_red_zone_scope_t & ) = delete;
    rocketnet_red_zone_scope_t & operator=( const rocketnet_red_zone_scope_t & ) = delete;
};

// run a layout function again over the created block, after create has
// zeroed what it zeroes, so its carves poison their pads in the block
#define ROCKETNET_RED_ZONE_CARVE( base, layout_call ) \
do                                                     \
{                                                      \
    rocketnet_red_zone_scope_t rn_red_zone_scope( (void *) ( base ) ); \
    (void) ( layout_call );                            \
} while ( 0 )

#else

#define ROCKETNET_RED_ZONE_CARVE( base, layout_call ) do { } while ( 0 )

#endif // #if ROCKETNET_DEV

#else

#define ROCKETNET_RED_ZONE_BYTES 0

#define ROCKETNET_RED_ZONE_CARVE( base, layout_call ) do { } while ( 0 )

#endif // #if ROCKETNET_RED_ZONES

// A create-time layout: an owner's fixed part followed by the buffers its
// config sizes, carved out of the one allocation create makes. The owner's
// layout function takes the carves in order and returns each offset; its
// create and its memory plan both call it, so the plan counts exactly the
// bytes create allocates. Each offset is aligned to align (a power of two at
// most ROCKETNET_ALLOCATION_ALIGNMENT, the alignment rocketnet_malloc gives
// the base). An overflow sets failed and every later carve returns 0. In an
// ASan build each carve is followed by a poisoned red zone (above).
struct rocketnet_carve_t
{
    uint64_t bytes;
    bool failed;
};

inline uint64_t rocketnet_carve( rocketnet_carve_t & carve, uint64_t count, uint64_t stride, uint64_t align = ROCKETNET_ALLOCATION_ALIGNMENT )
{
    if ( carve.failed || align == 0 || ( align & ( align - 1 ) ) != 0 || align > ROCKETNET_ALLOCATION_ALIGNMENT ||
         carve.bytes > UINT64_MAX - ( align - 1 ) )
    {
        carve.failed = true;
        return 0;
    }

    const uint64_t offset = ( carve.bytes + align - 1 ) & ~( align - 1 );
    uint64_t end = offset;
    if ( !rocketnet_memory_add( end, count, stride ) )
    {
        carve.failed = true;
        return 0;
    }

#if ROCKETNET_RED_ZONES
    if ( !rocketnet_memory_add( end, 1, ROCKETNET_RED_ZONE_BYTES ) )
    {
        carve.failed = true;
        return 0;
    }
#if ROCKETNET_DEV
    if ( rocketnet_red_zone_carve_base() )
    {
        rocketnet_red_zone_poison( rocketnet_red_zone_carve_base() + end - ROCKETNET_RED_ZONE_BYTES, ROCKETNET_RED_ZONE_BYTES );
    }
#endif // #if ROCKETNET_DEV
#endif // #if ROCKETNET_RED_ZONES

    carve.bytes = end;
    return offset;
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
        {
            return 0;
        }
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
uint64_t rocketnet_server_gso_allocation_bytes( int queues, int max_clients );
uint64_t rocketnet_server_iocp_allocation_bytes( int workers, int max_clients );
uint64_t rocketnet_server_xdp_allocation_bytes( int queues, int max_clients );
uint64_t rocketnet_server_rio_registered_region_bytes( int max_clients );

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
        {
            memset( base + at, 0, skip[i].offset - at );
        }
        at = skip[i].offset + skip[i].bytes;
    }

    if ( at < bytes )
    {
        memset( base + at, 0, bytes - at );
    }
}

#endif // ROCKETNET_SDK_ROCKETNET_MEMORY_H

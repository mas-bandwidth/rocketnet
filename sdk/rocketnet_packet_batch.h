/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_PACKET_BATCH_H
#define ROCKETNET_SDK_ROCKETNET_PACKET_BATCH_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_memory.h"

#include <string.h>

// Shared receive view. The historic type name is retained for native transport
// source compatibility; this layout is independent of the XDP implementation.
// Its arrays hold capacity packets, rocketnet_server_receive_packets_per_update
// for the server's configured max_clients unless the owner's own batch is
// larger (io_uring's bank): one allocation its owner makes at
// create (rocketnet_server_process_packets_create), counted in the owner's
// memory plan by rocketnet_server_process_packets_allocation_bytes.
struct rocketnet_server_socket_process_packets_t
{
    int num_packets;
    int capacity;
    rocketnet_address_t * from;
    int * packet_bytes;
    uint8_t ** packet_data;
};

static inline uint64_t rocketnet_server_process_packets_bytes( int capacity )
{
    if ( capacity < 1 )
        return 0;
    rocketnet_carve_t carve = { 0, false };
    rocketnet_carve( carve, (uint64_t) capacity, sizeof( rocketnet_address_t ) );
    rocketnet_carve( carve, (uint64_t) capacity, sizeof( int ) );
    rocketnet_carve( carve, (uint64_t) capacity, sizeof( uint8_t * ) );
    return carve.failed || carve.bytes > SIZE_MAX ? 0 : carve.bytes;
}

// the allocator bytes the view's one allocation holds at a capacity
static inline uint64_t rocketnet_server_process_packets_allocation_bytes( int capacity )
{
    const uint64_t bytes = rocketnet_server_process_packets_bytes( capacity );
    return bytes ? rocketnet_memory_allocations( { bytes } ) : 0;
}

// false, with the view empty, when the allocation is refused
static inline bool rocketnet_server_process_packets_create( void * context, struct rocketnet_server_socket_process_packets_t * view, int capacity )
{
    memset( view, 0, sizeof( *view ) );
    const uint64_t bytes = rocketnet_server_process_packets_bytes( capacity );
    if ( !bytes )
        return false;

    uint8_t * base = (uint8_t *) rocketnet_malloc( context, (size_t) bytes );
    if ( !base )
        return false;

    rocketnet_carve_t carve = { 0, false };
    view->from = (rocketnet_address_t *) ( base + rocketnet_carve( carve, (uint64_t) capacity, sizeof( rocketnet_address_t ) ) );
    view->packet_bytes = (int *) ( base + rocketnet_carve( carve, (uint64_t) capacity, sizeof( int ) ) );
    view->packet_data = (uint8_t **) ( base + rocketnet_carve( carve, (uint64_t) capacity, sizeof( uint8_t * ) ) );
    view->capacity = capacity;
    return true;
}

static inline void rocketnet_server_process_packets_destroy( void * context, struct rocketnet_server_socket_process_packets_t * view )
{
    if ( view->from )
        rocketnet_free( context, view->from );
    memset( view, 0, sizeof( *view ) );
}

#endif // ROCKETNET_SDK_ROCKETNET_PACKET_BATCH_H

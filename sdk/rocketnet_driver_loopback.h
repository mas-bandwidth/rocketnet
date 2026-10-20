/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

// The loopback transport carries datagrams between endpoints of the same
// process through memory; it uses no socket and no system network call. The
// transport variable is defined in rocketnet_driver_loopback.cpp. The two
// declarations below are for the unit suite only; none of this file is a
// public C export.
//
// The process-wide table is ROCKETNET_LOOPBACK_MAX_ENDPOINTS records of 72
// bytes each (72,072 bytes) under one mutex. Each live endpoint allocates a
// queue of ROCKETNET_LOOPBACK_QUEUE_INITIAL_PACKETS datagrams at create
// (loopback_create). When a datagram arrives at a full queue below
// ROCKETNET_LOOPBACK_QUEUE_PACKETS, the queue doubles, capped at
// ROCKETNET_LOOPBACK_QUEUE_PACKETS, keeping arrival order
// (loopback_queue_grow); a datagram that arrives with
// ROCKETNET_LOOPBACK_QUEUE_PACKETS queued, or that finds a full queue whose
// growth allocation is refused, is dropped and counted (loopback_queue_tail).
// A queue keeps its capacity until the endpoint is destroyed. The cost is paid
// only by a process that instantiates the loopback transport, which is tests
// and examples, never a game on auto. Lookups scan the table linearly under
// the mutex.

#include "rocketnet.h"
#include "rocketnet_constants.h"

#include <stddef.h>
#include <stdint.h>

enum
{
    ROCKETNET_LOOPBACK_QUEUE_PACKETS = 2048,
    ROCKETNET_LOOPBACK_QUEUE_INITIAL_PACKETS = 8
};

ROCKETNET_STATIC_ASSERT( ( ROCKETNET_LOOPBACK_QUEUE_PACKETS & ( ROCKETNET_LOOPBACK_QUEUE_PACKETS - 1 ) ) == 0 &&
                         ( ROCKETNET_LOOPBACK_QUEUE_INITIAL_PACKETS & ( ROCKETNET_LOOPBACK_QUEUE_INITIAL_PACKETS - 1 ) ) == 0 &&
                         ROCKETNET_LOOPBACK_QUEUE_INITIAL_PACKETS <= ROCKETNET_LOOPBACK_QUEUE_PACKETS,
                         "loopback queue capacities are powers of two, so the free-running head and tail index every capacity" );

ROCKETNET_STATIC_ASSERT( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS,
                         "receive budget twice send: loopback queue holds at least twice the send batch" );

// A bound endpoint's count of datagrams dropped by the backend: over the
// queue ceiling on send, or larger than the receive buffer on drain.
uint64_t rocketnet_loopback_endpoint_dropped( void * instance );

// The number of live endpoints in the process-wide table.
int rocketnet_loopback_endpoints_live();

// The bytes a queue of this many datagrams allocates, before the allocator
// header rocketnet_malloc adds.
size_t rocketnet_loopback_queue_bytes( int packets );

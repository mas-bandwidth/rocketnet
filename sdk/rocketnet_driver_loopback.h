/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

// The loopback transport carries datagrams between endpoints of the same
// process through memory; it uses no socket and no system network call. The
// transport variable is defined in rocketnet_driver_loopback.cpp. The two
// declarations below are for the unit suite only; none of this file is a
// public C export.
//
// The process-wide endpoint table is guarded by one mutex. Each live endpoint
// allocates its full ROCKETNET_LOOPBACK_QUEUE_PACKETS queue at create and frees
// it at destroy. Send and receive never allocate or grow storage. A datagram
// arriving at the queue ceiling is dropped and counted; queued datagrams keep
// their arrival order. Production local play must budget this capacity for
// each live endpoint. Lookups scan the table linearly under the mutex.

#include "rocketnet.h"
#include "rocketnet_constants.h"

// The loopback transport's process-wide endpoint table: one server and
// ROCKETNET_MAX_CLIENTS clients. Loopback is a production transport, chosen
// by name ("loopback"); auto never selects it.
#define ROCKETNET_LOOPBACK_MAX_ENDPOINTS          ( ROCKETNET_MAX_CLIENTS + 1 )

#include <stddef.h>
#include <stdint.h>

enum
{
    ROCKETNET_LOOPBACK_QUEUE_PACKETS = 2048
};

ROCKETNET_STATIC_ASSERT( ( ROCKETNET_LOOPBACK_QUEUE_PACKETS & ( ROCKETNET_LOOPBACK_QUEUE_PACKETS - 1 ) ) == 0,
                         "loopback queue capacity is a power of two for free-running head and tail indices" );

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

#if defined(ROCKETNET_LOOPBACK_TEST_HOOKS)
// Deterministic send-lock contention seam; test library only.
void rocketnet_loopback_test_send_contention( bool busy );
#endif

/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

// The loopback transport carries datagrams between endpoints of the same
// process through memory; it uses no socket and no system network call. The
// transport variable is defined in rocketnet_driver_loopback.cpp. The two
// declarations below are for the unit suite only; none of this file is a
// public C export.
//
// Each live endpoint allocates its inbox at create (a server's sized from its
// plan) and frees it at destroy. Datagrams move on the caller threads of the
// two endpoints, with no thread of the transport's own
// (tla/RNLoopbackWire.tla): a send fills a slot of the receiver's inbox and
// marks it before it returns, and the receiver's next update takes it. Only
// slot indices cross threads, under the inbox mutex. Send and receive never
// allocate or grow storage. A datagram arriving at a full inbox is dropped
// and counted; each sender's datagrams keep their order. Production local
// play must budget this capacity for each live endpoint. The process-wide
// endpoint table is guarded by one mutex, taken at create, destroy and a
// sender's first send to each address.

#include "rocketnet.h"
#include "rocketnet_constants.h"

// The loopback transport's process-wide endpoint table: one server and
// ROCKETNET_MAX_CLIENTS clients. Loopback is a production transport, chosen
// by name ("loopback"); auto never selects it.
#define ROCKETNET_LOOPBACK_MAX_ENDPOINTS          ( ROCKETNET_MAX_CLIENTS + 1 )

#include <stddef.h>
#include <stdint.h>

// A client endpoint's queue: the server-wide send staging (2048 at 1000
// clients). A server endpoint's queue is sized from its plan at create
// (rocketnet_loopback_server_queue_packets).
enum
{
    ROCKETNET_LOOPBACK_QUEUE_PACKETS = ROCKETNET_SERVER_SEND_STAGING_PACKETS
};

ROCKETNET_STATIC_ASSERT( ( ROCKETNET_LOOPBACK_QUEUE_PACKETS & ( ROCKETNET_LOOPBACK_QUEUE_PACKETS - 1 ) ) == 0,
                         "loopback queue capacity is a power of two for free-running head and tail indices" );

ROCKETNET_STATIC_ASSERT( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS,
                         "receive budget twice send: loopback queue holds at least twice the send batch" );

// A bound endpoint's count of datagrams dropped by the backend: over the
// queue ceiling on send, or larger than the receive buffer on drain.
uint64_t rocketnet_loopback_endpoint_dropped( void * instance );

// The inbox slots an endpoint has ever used: the most datagrams it held at
// once, since freed slots are reused before a fresh one is taken.
uint32_t rocketnet_loopback_endpoint_slots_touched( void * instance );

// The number of live endpoints in the process-wide table.
int rocketnet_loopback_endpoints_live();

// The bytes an endpoint with an inbox of this many datagrams allocates at
// create (the inbox, its index queues and the endpoint's route cache), before
// the allocator header rocketnet_malloc adds.
size_t rocketnet_loopback_queue_bytes( int packets );

// The datagrams a server endpoint's queue holds for a server of this many
// clients: one update's receive budget, a power of two, at least
// ROCKETNET_LOOPBACK_QUEUE_PACKETS. A client endpoint's queue holds
// ROCKETNET_LOOPBACK_QUEUE_PACKETS.
int rocketnet_loopback_server_queue_packets( int max_clients );

#if defined(ROCKETNET_LOOPBACK_TEST_HOOKS)
// Deterministic send-lock contention seam; test library only.
void rocketnet_loopback_test_send_contention( bool busy );
#endif

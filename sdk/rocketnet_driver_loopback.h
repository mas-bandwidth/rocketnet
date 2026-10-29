/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_DRIVER_LOOPBACK_H
#define ROCKETNET_SDK_ROCKETNET_DRIVER_LOOPBACK_H

// The loopback transport carries datagrams between endpoints of the same
// process through memory; it uses no socket and no system network call. The
// transport variable is defined in rocketnet_driver_loopback.cpp. The hub calls
// are public (sdk/rocketnet.h); the rest of this file is for the SDK and its
// unit suite only and is not a public C export.
//
// The endpoints find each other through a caller-owned hub
// (rocketnet_loopback_hub_create): one server and up to max_clients clients,
// each with its full queue allocated in the hub's one block at hub create (the
// server's sized from the hub's clients). Endpoint create and destroy only
// register and unregister a record. Send and receive never allocate or grow
// storage. A datagram arriving at the queue ceiling is dropped and counted;
// queued datagrams keep their arrival order. One hub mutex guards the records
// and queues.

#include "rocketnet.h"
#include "rocketnet_constants.h"

#include <stddef.h>
#include <stdint.h>

// A client endpoint's queue: the server-wide send staging (2048 at 1000
// clients). A server endpoint's queue is sized from the hub's clients at hub
// create (rocketnet_loopback_server_queue_packets).
enum
{
    ROCKETNET_LOOPBACK_QUEUE_PACKETS = ROCKETNET_SERVER_SEND_STAGING_PACKETS
};

ROCKETNET_STATIC_ASSERT( ( ROCKETNET_LOOPBACK_QUEUE_PACKETS & ( ROCKETNET_LOOPBACK_QUEUE_PACKETS - 1 ) ) == 0,
                         "loopback queue capacity is a power of two for free-running head and tail indices" );

ROCKETNET_STATIC_ASSERT( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS,
                         "receive budget twice send: loopback queue holds at least twice the send batch" );

#if ROCKETNET_DEV
// A bound endpoint's count of datagrams dropped by the backend: over the
// queue ceiling on send, or larger than the receive buffer on drain.
uint64_t rocketnet_loopback_endpoint_dropped( void * instance );

// The number of endpoints registered with the hub.
int rocketnet_loopback_endpoints_live( rocketnet_loopback_hub_t * hub );
#endif // #if ROCKETNET_DEV

// The start of every hub, checked by every use: two copies of the library in
// one process share a hub only when all three match.
#define ROCKETNET_LOOPBACK_HUB_MAGIC 0x4255484B4F4F4C52ULL     // "RLOOKHUB"

struct rocketnet_loopback_hub_header_t
{
    uint64_t magic;         // ROCKETNET_LOOPBACK_HUB_MAGIC
    uint32_t version;       // ROCKETNET_VERSION of the library that created it
    uint32_t layout;        // the hub's sizes and offsets, hashed at compile time
};

// OK for a hub this library can use; LOOPBACK_HUB_MISSING for NULL;
// LOOPBACK_HUB_MISMATCH when the header is not this build's. Logs nothing.
enum rocketnet_status_t rocketnet_loopback_hub_check( const rocketnet_loopback_hub_t * hub );

// The bytes of the one block a hub of this many clients allocates, before the
// allocator header rocketnet_malloc adds; 0 outside 1 .. ROCKETNET_MAX_CLIENTS.
uint64_t rocketnet_loopback_hub_bytes( int max_clients );

// The bytes a queue of this many datagrams takes in the hub.
size_t rocketnet_loopback_queue_bytes( int packets );

// The datagrams a server endpoint's queue holds for a hub of this many
// clients: one update's receive budget, a power of two, at least
// ROCKETNET_LOOPBACK_QUEUE_PACKETS. A client endpoint's queue holds
// ROCKETNET_LOOPBACK_QUEUE_PACKETS.
int rocketnet_loopback_server_queue_packets( int max_clients );

#if ROCKETNET_DEV
// Deterministic send-lock contention seam; test library only.
void rocketnet_loopback_test_send_contention( bool busy );

// Read-only queue-index seam; test library only. Writes the endpoint's
// free-running head and tail and its fixed capacity.
void rocketnet_loopback_test_queue_indices( void * instance, uint32_t * head, uint32_t * tail, uint32_t * capacity );
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_DRIVER_LOOPBACK_H

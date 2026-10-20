/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

// The loopback transport carries datagrams between endpoints of the same
// process through memory; it uses no socket and no system network call. The
// transport variable is defined in rocketnet_transport_loopback.cpp. The two
// declarations below are for the unit suite only; none of this file is a
// public C export.
//
// The process-wide table is ROCKETNET_LOOPBACK_MAX_ENDPOINTS records of 72
// bytes each (72,072 bytes) under one mutex. Each live endpoint allocates its
// queue at create, ROCKETNET_LOOPBACK_QUEUE_PACKETS x
// sizeof( rocketnet_loopback_packet_t ) = 1,531,904 bytes, so a full table of
// live endpoints is about 1,533,435,904 bytes. The cost is paid only by a
// process that instantiates the loopback transport, which is tests and
// examples, never a game on auto. Lookups scan the table linearly under the
// mutex.

#include "rocketnet.h"
#include "rocketnet_constants.h"

#include <stdint.h>

enum
{
    ROCKETNET_LOOPBACK_QUEUE_PACKETS = 2048
};

ROCKETNET_STATIC_ASSERT( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS,
                         "receive budget twice send: loopback queue holds at least twice the send batch" );

// A bound endpoint's count of datagrams dropped by the backend: over the
// queue ceiling on send, or larger than the receive buffer on drain.
uint64_t rocketnet_loopback_endpoint_dropped( void * instance );

// The number of live endpoints in the process-wide table.
int rocketnet_loopback_endpoints_live();

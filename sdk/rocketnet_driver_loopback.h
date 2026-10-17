/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

// The loopback driver backend carries datagrams between endpoints of the same
// process through memory; it uses no socket and no system network call. The
// backend variable is defined in rocketnet_driver_loopback.cpp. The two
// declarations below are for the unit suite only; none of this file is a
// public C export.

#include <stdint.h>

enum
{
    ROCKETNET_LOOPBACK_MAX_ENDPOINTS = 64,
    ROCKETNET_LOOPBACK_QUEUE_PACKETS = 1024
};

// A bound endpoint's count of datagrams dropped by the backend: over the
// queue ceiling on send, or larger than the receive buffer on drain.
uint64_t rocketnet_loopback_endpoint_dropped( void * instance );

// The number of live endpoints in the process-wide table.
int rocketnet_loopback_endpoints_live();

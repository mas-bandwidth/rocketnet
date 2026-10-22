/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for <linux/udp.h>: the UDP header (RFC 768). Packed so a
// header at any frame offset is read safely.

#pragma once

#ifndef ROCKETNET_XDP_HOST_LINUX_UDP_H
#define ROCKETNET_XDP_HOST_LINUX_UDP_H

#include "linux/bpf.h"

struct udphdr
{
    __be16 source;
    __be16 dest;
    __be16 len;
    __sum16 check;
} __attribute__((packed));

_Static_assert( sizeof(struct udphdr) == 8, "the UDP header is 8 bytes" );

#endif // #ifndef ROCKETNET_XDP_HOST_LINUX_UDP_H

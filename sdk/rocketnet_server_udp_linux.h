/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_UDP_LINUX_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_UDP_LINUX_H

#include "rocketnet_internal.h"

#include <stdint.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_address.h"

#include <sys/socket.h>

// The Linux server transports' shared socket steps, create time only: the
// gso transport (sdk/rocketnet_server_gso_linux.cpp) and the io_uring
// transport (sdk/rocketnet_server_uring_linux.cpp) open the same reuseport
// group and probe the same offloads. Internal, not in rocketnet.h.

// One socket of a reuseport group: family from the bind address, dual stack
// when ipv6, the configured buffer sizes requested and the grant read back and
// reported (queue 0 only), the first queue bound before SO_REUSEPORT and the
// rest joining it, an ephemeral port discovered by the first queue and handed
// back through bound_port, non-blocking, don't-fragment, close-on-exec.
// transport names the transport in every log line and in the receive buffer
// request ("gso", "io_uring"). Returns the descriptor, or -1 with the socket
// closed and the reason logged.
int rocketnet_server_udp_open( const char * transport, const rocketnet_address_t * bind_address, int queue_index,
                               int max_clients, uint16_t * bound_port );

// The signature of setsockopt(2): the probe's one system call, a parameter so
// a unit case can answer it without a socket.
typedef int ( *rocketnet_server_udp_setsockopt_t )( int fd, int level, int option_name, const void * value, socklen_t length );

// The UDP_SEGMENT and UDP_GRO probes over the whole group, each all or
// nothing. Sends rotate across the group and every socket receives, so a tier
// claimed for the group must hold on every socket. The UDP_SEGMENT probe
// writes segment size zero, the value every socket already holds, so a
// refusal leaves nothing to roll back. A partial UDP_GRO enable is rolled back
// on every socket, so no socket keeps state the reported tier denies. A probe
// not asked for is not made and reports false. Failure is a tier, not an
// error. set is setsockopt outside the unit suite.
void rocketnet_server_udp_probe_offloads( const int * fds, int num_fds, bool probe_segment, bool probe_gro,
                                          rocketnet_server_udp_setsockopt_t set, bool * segment, bool * gro );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#endif // #ifndef ROCKETNET_SDK_ROCKETNET_SERVER_UDP_LINUX_H

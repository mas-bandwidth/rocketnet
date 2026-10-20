/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP socket config for one bind attempt. Zero copy is a bind flag
// (XDP_ZEROCOPY in bind_flags, linux/if_xdp.h); xdp_flags holds the program
// attach flags (XDP_FLAGS_*), which libxdp never reads here because the
// program is loaded by the server, not by libxdp
// (XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD). The first attempt asks for zero copy;
// a driver without zero-copy support refuses that bind, and the fallback asks
// for copy. Both ask for the need-wakeup flag. Header only, so the kernel-tier
// suite binds through the same config as the server.
//
// The umem config for the same bind (the server's xsk_umem__create,
// sdk/rocketnet_server_socket_linux.cpp). It sizes the fill and
// completion rings explicitly: libxdp 1.4.2's xsk_umem__create returns
// -EINVAL for a zeroed config's zero ring sizes, so the server never starts
// there without them. The fill ring asks for XSK_RING_PROD__DEFAULT_NUM_DESCS
// (2048) entries and the completion ring for XSK_RING_CONS__DEFAULT_NUM_DESCS
// (2048), both from xdp/xsk.h. The server reserves ROCKETNET_XDP_FILL_QUEUE_SIZE
// fill entries at start (sdk/rocketnet_server_socket_linux.cpp); the
// static assert below keeps the fill ring at least that large.

#ifndef ROCKETNET_XDP_SOCKET_CONFIG_H
#define ROCKETNET_XDP_SOCKET_CONFIG_H

#include <stdint.h>
#include <string.h>
#include <linux/if_xdp.h>
#include <xdp/xsk.h>

#include "rocketnet_constants.h"

static inline void rocketnet_xdp_socket_config( struct xsk_socket_config * config, uint32_t rx_size, uint32_t tx_size, bool zero_copy )
{
    memset( config, 0, sizeof(*config) );
    config->rx_size = rx_size;
    config->tx_size = tx_size;
    config->libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
    config->xdp_flags = 0;
    config->bind_flags = (uint16_t) ( XDP_USE_NEED_WAKEUP | ( zero_copy ? XDP_ZEROCOPY : XDP_COPY ) );
}

static inline void rocketnet_xdp_umem_config( struct xsk_umem_config * config )
{
    memset( config, 0, sizeof(*config) );
    config->fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    config->comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    config->frame_size = ROCKETNET_XDP_FRAME_SIZE;
    config->frame_headroom = 0;
}

ROCKETNET_STATIC_ASSERT( XSK_RING_PROD__DEFAULT_NUM_DESCS >= ROCKETNET_XDP_FILL_QUEUE_SIZE, "the fill ring holds the fill entries the server reserves at start" );

#endif // #ifndef ROCKETNET_XDP_SOCKET_CONFIG_H

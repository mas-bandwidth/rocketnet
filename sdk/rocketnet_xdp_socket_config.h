/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP socket config for one bind attempt. Zero copy is a bind flag
// (XDP_ZEROCOPY in bind_flags, linux/if_xdp.h); xdp_flags holds the program
// attach flags (XDP_FLAGS_*), which libxdp never reads here because the
// program is loaded by the server, not by libxdp
// (XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD). The first attempt asks for zero copy;
// a transport without zero-copy support refuses that bind, and the fallback asks
// for copy. Both ask for the need-wakeup flag. Header only, so the kernel-tier
// suite binds through the same config as the server.
//
// The umem config for the same bind (the server's xsk_umem__create,
// sdk/rocketnet_server_socket_linux.cpp). It sizes the fill and
// completion rings explicitly: libxdp 1.4.2's xsk_umem__create returns
// -EINVAL for a zeroed config's zero ring sizes, so the server never starts
// there without them. The fill ring asks for ROCKETNET_XDP_FILL_RING_SIZE
// entries, twice the ROCKETNET_XDP_FILL_FRAMES receive frames the server puts
// on it at start (sdk/rocketnet_server_socket_linux.cpp): the kernel publishes
// the fill ring's consumer index only when its cached batch is used up, so the
// worker needs a free slot beyond that batch for every frame it hands back
// (rocketnet_constants.h, tla/RNXdpReceiveRing.tla). The completion ring asks
// for XSK_RING_CONS__DEFAULT_NUM_DESCS (2048) entries, from xdp/xsk.h.

#ifndef ROCKETNET_XDP_SOCKET_CONFIG_H
#define ROCKETNET_XDP_SOCKET_CONFIG_H

#include <stdint.h>
#include <string.h>
#include <linux/if_xdp.h>
#include "rocketnet_xdp_compat.h"

#include "rocketnet_constants.h"

static inline void rocketnet_xdp_socket_config( struct xsk_socket_config * config, uint32_t rx_size, uint32_t tx_size, bool zero_copy )
{
    memset( config, 0, sizeof( *config ) );
    config->rx_size = rx_size;
    config->tx_size = tx_size;
    config->libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
    config->xdp_flags = 0;
    config->bind_flags = (uint16_t) ( XDP_USE_NEED_WAKEUP | ( zero_copy ? XDP_ZEROCOPY : XDP_COPY ) );
}

static inline void rocketnet_xdp_umem_config( struct xsk_umem_config * config )
{
    memset( config, 0, sizeof( *config ) );
    config->fill_size = ROCKETNET_XDP_FILL_RING_SIZE;
    config->comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    config->frame_size = ROCKETNET_XDP_FRAME_SIZE;
    config->frame_headroom = 0;
}

// R3 of the pre-flight: the interface is free only when no program is attached
// in any mode. prog_id alone reads 0 for a multi-mode attachment.
static inline bool rocketnet_xdp_attached_any( const rocketnet_xdp_attached_t * attached )
{
    return attached->prog_id != 0 || attached->drv_prog_id != 0 || attached->skb_prog_id != 0 || attached->hw_prog_id != 0;
}

ROCKETNET_STATIC_ASSERT( ( ROCKETNET_XDP_FILL_RING_SIZE & ( ROCKETNET_XDP_FILL_RING_SIZE - 1 ) ) == 0, "the kernel takes only power of two ring sizes" );

#endif // #ifndef ROCKETNET_XDP_SOCKET_CONFIG_H

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_XDP_BIND_H
#define ROCKETNET_XDP_BIND_H

#include <stdint.h>
#include <string.h>

/*
    The AF_XDP bind mode, kept free of kernel headers so the unit tests drive
    it with no socket (test_unit_xdp_bind.cpp).

    Copy mode is the default; zero-copy is opt-in by ROCKETNET_XDP_ZEROCOPY=1.
    space (Intel X710, stock in-tree i40e on 7.0.0-31-generic) panicked twice
    in i40e_construct_skb_zc, the driver's zero-copy XDP_PASS path (pstore,
    2026-10-15 23:31Z under bin/server with ROCKETNET_DRIVER=xdp, 2026-10-16
    18:53Z under the space game server). Copy mode never enters that path.
    The fix Glenn named is the driver: Intel's out-of-tree i40e built from
    source (rowan-tools fleet/nic-driver.yml). This default is the second
    belt.

    Before this, rocketnet put XDP_ZEROCOPY in xsk_socket_config.xdp_flags.
    That field is libxdp's attach mode for its own program, unused under
    XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD, so the bind carried only
    XDP_USE_NEED_WAKEUP and the kernel's default (try zero-copy, fall back to
    copy) chose the mode. The mode now goes in bind_flags, where the kernel
    reads it, and a zero-copy request on a driver without support fails the
    bind instead of silently copying.

    The values are linux/if_xdp.h's; rocketnet_server_socket_linux.cpp
    static_asserts that they match.
*/

#define ROCKETNET_XDP_BIND_COPY             ( 1u << 1 )     // XDP_COPY
#define ROCKETNET_XDP_BIND_ZEROCOPY         ( 1u << 2 )     // XDP_ZEROCOPY
#define ROCKETNET_XDP_BIND_USE_NEED_WAKEUP  ( 1u << 3 )     // XDP_USE_NEED_WAKEUP

// Zero-copy only when the operator asked for it by name: exactly "1". Unset,
// empty, "0", "yes", "true" and anything else bind in copy mode.
static inline bool rocketnet_xdp_want_zerocopy( const char * env_value )
{
    return env_value != NULL && strcmp( env_value, "1" ) == 0;
}

// The sxdp_flags for one AF_XDP bind: need-wakeup always, and exactly one of
// copy or zero-copy.
static inline uint16_t rocketnet_xdp_bind_flags( const char * env_value )
{
    return (uint16_t) ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP |
                        ( rocketnet_xdp_want_zerocopy( env_value ) ? ROCKETNET_XDP_BIND_ZEROCOPY : ROCKETNET_XDP_BIND_COPY ) );
}

#endif // #ifndef ROCKETNET_XDP_BIND_H

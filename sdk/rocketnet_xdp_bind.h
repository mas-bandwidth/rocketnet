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

// The AF_XDP socket config for one queue, built in one place so the call site
// cannot drift from the tested choice: rx/tx ring sizes, xdp_flags 0 (libxdp's
// attach mode for its own program, which inhibit_prog_load turns off), the
// bind mode above in bind_flags, and the inhibit flag. Config is libxdp's
// xsk_socket_config at the call site, and any struct with those fields in the
// unit test (test_unit_xdp_bind.cpp), so this header needs no kernel header.
template<class Config> static inline void rocketnet_xdp_socket_config_init( Config * config, uint32_t rx_size, uint32_t tx_size,
                                                                            uint32_t inhibit_prog_load, const char * zerocopy_env )
{
    memset( config, 0, sizeof(*config) );
    config->rx_size = rx_size;
    config->tx_size = tx_size;
    config->xdp_flags = 0;
    config->bind_flags = rocketnet_xdp_bind_flags( zerocopy_env );
    config->libbpf_flags = inhibit_prog_load;
}

// The umem config, every field explicit. libxdp before 1.5 copies a caller's
// config as given (xdp-tools v1.4.2 lib/libxdp/xsk.c:154-171), so a zeroed
// fill_size or comp_size reaches the kernel, which refuses a 0-entry ring
// (EINVAL from XDP_UMEM_FILL_RING); 1.5 and later substitute the defaults.
template<class UmemConfig> static inline void rocketnet_xdp_umem_config_init( UmemConfig * config, uint32_t fill_size, uint32_t comp_size,
                                                                              uint32_t frame_size )
{
    memset( config, 0, sizeof(*config) );
    config->fill_size = fill_size;
    config->comp_size = comp_size;
    config->frame_size = frame_size;
    config->frame_headroom = 0;
    config->flags = 0;
}

// The initial fill, run at create after xsk_umem__create and BEFORE
// xsk_socket__create: `count` receive frames go onto the fill ring, so the
// driver finds them when the bind sets up its Rx ring. All or nothing:
// reserve( count, &index ) must return count (libxdp reserves all or zero);
// alloc() returns a frame or `invalid`; write( index, frame ) fills one slot;
// submit( count ) publishes them. Returns false, with nothing submitted, when
// the reservation or any allocation fails (create then destroys the socket).
template<class Reserve, class Alloc, class Write, class Submit>
static inline bool rocketnet_xdp_prefill( uint32_t count, uint64_t invalid, Reserve reserve, Alloc alloc, Write write, Submit submit )
{
    uint32_t index = 0;
    if ( reserve( count, &index ) != count )
        return false;
    for ( uint32_t i = 0; i < count; i++ )
    {
        const uint64_t frame = alloc();
        if ( frame == invalid )
            return false;
        write( index + i, frame );
    }
    submit( count );
    return true;
}

#endif // #ifndef ROCKETNET_XDP_BIND_H

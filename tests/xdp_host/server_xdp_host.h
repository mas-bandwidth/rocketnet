/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP filter as host C: server_xdp/server_xdp.c, compiled unchanged
// behind the shim in this directory, driven through a plain C interface. No
// kernel, no root, no socket, no BPF syscall: the maps are host storage and
// the two helpers are host functions with the kernel's semantics.
//
// Built (and ROCKETNET_XDP_FILTER_HOST defined for the unit suite) only where
// the C compiler is GCC or Clang on a little-endian host, because the filter
// is GNU C (void pointer arithmetic, typeof) and the shim's IPv4 bitfields
// model a little-endian machine.

#pragma once

#ifndef ROCKETNET_SERVER_XDP_HOST_H
#define ROCKETNET_SERVER_XDP_HOST_H

#include <stdint.h>

#include "rocketnet_wire.h"

#ifdef __cplusplus
extern "C"
{
#endif

// config zeroed, every socket slot empty, every counter zeroed, every replay
// entry removed, and the config lookup succeeding

void rocketnet_xdp_host_reset( void );

void rocketnet_xdp_host_set_config( const struct rocketnet_server_xdp_config_t * config );

// nonzero makes the config map lookup return NULL, the branch the kernel's
// one-entry array never shows

void rocketnet_xdp_host_set_config_lookup_fails( int fails );

// populates (nonzero) or empties (zero) socket slot index of the XSKMAP;
// an index at or past ROCKETNET_XDP_MAX_QUEUES is ignored

void rocketnet_xdp_host_set_socket_slot( uint32_t index, int populated );

// writes or overwrites the replay table entry for the key; a new key is
// appended while the table is not full and ignored once it is

void rocketnet_xdp_host_set_replay_entry( const struct rocketnet_server_xdp_replay_key_t * key, uint32_t high_water );

// 1 with *high_water set when the key is present, 0 with *high_water
// untouched when it is not

int rocketnet_xdp_host_get_replay_entry( const struct rocketnet_server_xdp_replay_key_t * key, uint32_t * high_water );

// runs one frame through server_xdp_filter and returns its verdict (the XDP
// action number). The frame is copied into a heap allocation of exactly
// frame_bytes first, so a read past data_end is a heap overflow under the
// sanitizers. frame_bytes below 1 returns -1 without running the filter.

int rocketnet_xdp_host_run( const uint8_t * frame, int frame_bytes, uint32_t rx_queue_index );

// the stats map's counter, ROCKETNET_SERVER_XDP_COUNTER_*; 0 outside the range

uint64_t rocketnet_xdp_host_counter( int counter );

// how many times the filter called bpf_redirect_map since the reset, and the
// key of the last call

int rocketnet_xdp_host_redirect_calls( void );

uint64_t rocketnet_xdp_host_last_redirect_key( void );

#ifdef __cplusplus
}
#endif

#endif // #ifndef ROCKETNET_SERVER_XDP_HOST_H

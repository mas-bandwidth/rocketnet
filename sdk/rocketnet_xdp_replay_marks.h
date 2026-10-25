/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_XDP_REPLAY_MARKS_H
#define ROCKETNET_XDP_REPLAY_MARKS_H

#include <stdint.h>
#include <string.h>

#include "rocketnet_address.h"
#include "rocketnet_wire.h"
#include "rocketnet_xdp_limits.h"

// The userspace pending-marks buffer for the kernel replay map. The transport
// fills it once per server update -- a FORGET per slot that held a mark and
// is no longer connected or whose address changed, then a MARK per connected
// slot whose replay high-water mark advanced -- and the transport applies every
// forget, then every mark, in one commit, then resets keeping every forget the
// kernel refused that no applied mark for the same key superseded, so it is
// tried again first in the next commit (tla/RNXdpReplay.tla, Commit,
// ForgetOwedUntilApplied and RetryNeverDeletesNewerMark). This is pure data:
// no syscall, no allocation, no kernel program.

// The key layout for one source address and port, in network order.

static inline void rocketnet_xdp_replay_key_from_address( const struct rocketnet_address_t * address, struct rocketnet_server_xdp_replay_key_t * key )
{
    memset( key, 0, sizeof(*key) );

    if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        key->family = 4;
        memcpy( key->address, address->data.ipv4, 4 );
    }
    else
    {
        key->family = 6;
        for ( int i = 0; i < 8; i++ )
        {
            const uint16_t word = address->data.ipv6[i];
            key->address[i * 2] = (uint8_t) ( word >> 8 );
            key->address[i * 2 + 1] = (uint8_t) ( word & 0xff );
        }
    }

    // the port is network order, the high byte first
    uint8_t * port_bytes = (uint8_t *) &key->port;
    port_bytes[0] = (uint8_t) ( address->port >> 8 );
    port_bytes[1] = (uint8_t) ( address->port & 0xff );
}

struct rocketnet_xdp_replay_marks_t
{
    struct rocketnet_server_xdp_replay_key_t forgets[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    struct rocketnet_server_xdp_replay_key_t mark_keys[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    struct rocketnet_server_xdp_replay_value_t mark_values[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    uint32_t forget_count;
    uint32_t mark_count;
    uint32_t refused;
};

static inline void rocketnet_xdp_replay_marks_reset( struct rocketnet_xdp_replay_marks_t * marks )
{
    marks->forget_count = 0;
    marks->mark_count = 0;
    marks->refused = 0;
}

// The end of a commit: forget_failed[i] says the kernel refused forget i,
// mark_applied[j] says the kernel took mark j. The refused forgets move to
// the front in their order and stay pending, so the next commit applies them
// before any mark (tla/RNXdpReplay.tla, Commit) -- except a refused forget
// whose key a mark applied in the same commit: that mark, taken after the
// forget was queued, is a newer session's and now owns the kernel entry, so
// the stale forget is superseded and dropped, and a later retry can never
// delete it (RetryNeverDeletesNewerMark). The marks are cleared. Returns the
// refusals counted since the last reset and zeroes them, so the caller adds
// them to its own count.

static inline uint32_t rocketnet_xdp_replay_marks_reset_keeping( struct rocketnet_xdp_replay_marks_t * marks, const bool * forget_failed, const bool * mark_applied )
{
    uint32_t kept = 0;
    for ( uint32_t i = 0; i < marks->forget_count; i++ )
    {
        if ( !forget_failed[i] )
            continue;
        bool superseded = false;
        for ( uint32_t j = 0; j < marks->mark_count && !superseded; j++ )
            superseded = mark_applied[j] && memcmp( &marks->mark_keys[j], &marks->forgets[i], sizeof(marks->forgets[i]) ) == 0;
        if ( superseded )
            continue;
        if ( kept != i )
            marks->forgets[kept] = marks->forgets[i];
        kept++;
    }
    marks->forget_count = kept;
    marks->mark_count = 0;
    const uint32_t refused = marks->refused;
    marks->refused = 0;
    return refused;
}

// Adds a forget for the source the address names. Returns false, and counts
// the refusal, when the forget list is full.

static inline bool rocketnet_xdp_replay_marks_forget( struct rocketnet_xdp_replay_marks_t * marks, const struct rocketnet_address_t * address )
{
    if ( marks->forget_count >= ROCKETNET_XDP_REPLAY_MAX_ENTRIES )
    {
        marks->refused++;
        return false;
    }
    rocketnet_xdp_replay_key_from_address( address, &marks->forgets[marks->forget_count] );
    marks->forget_count++;
    return true;
}

// Adds a mark of the high-water value for the source the address names.
// Returns false, and counts the refusal, when the mark list is full.

static inline bool rocketnet_xdp_replay_marks_mark( struct rocketnet_xdp_replay_marks_t * marks, const struct rocketnet_address_t * address, uint32_t high_water )
{
    if ( marks->mark_count >= ROCKETNET_XDP_REPLAY_MAX_ENTRIES )
    {
        marks->refused++;
        return false;
    }
    rocketnet_xdp_replay_key_from_address( address, &marks->mark_keys[marks->mark_count] );
    marks->mark_values[marks->mark_count].high_water = high_water;
    marks->mark_values[marks->mark_count].pad = 0;
    marks->mark_count++;
    return true;
}

// The pending forgets, in insertion order, any kept from the last commit
// first.

static inline void rocketnet_xdp_replay_marks_forgets( const struct rocketnet_xdp_replay_marks_t * marks,
                                                       const struct rocketnet_server_xdp_replay_key_t ** keys,
                                                       uint32_t * count )
{
    *keys = marks->forgets;
    *count = marks->forget_count;
}

// The pending marks, in insertion order.

static inline void rocketnet_xdp_replay_marks_marks( const struct rocketnet_xdp_replay_marks_t * marks,
                                                     const struct rocketnet_server_xdp_replay_key_t ** keys,
                                                     const struct rocketnet_server_xdp_replay_value_t ** values,
                                                     uint32_t * count )
{
    *keys = marks->mark_keys;
    *values = marks->mark_values;
    *count = marks->mark_count;
}

#endif // #ifndef ROCKETNET_XDP_REPLAY_MARKS_H

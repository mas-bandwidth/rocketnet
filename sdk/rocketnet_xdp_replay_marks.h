/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_XDP_REPLAY_MARKS_H
#define ROCKETNET_SDK_ROCKETNET_XDP_REPLAY_MARKS_H

#include <stdint.h>
#include <string.h>

#include "rocketnet_address.h"
#include "rocketnet_wire.h"
#include "rocketnet_xdp_limits.h"

// The userspace pending-marks buffer for the kernel replay map. The transport
// fills it once per server update -- a FORGET per slot seen connected that is
// no longer connected or whose address changed, whether or not it held a mark,
// then a MARK per connected slot first seen (zero included) or whose
// highest_opened differs from its last mark -- and the transport applies every
// forget, then every mark, in one commit, then resets keeping every forget the
// kernel refused that no applied mark for the same key superseded, so it is
// tried again first in the next commit (proofs/tla/RNXdpReplay.tla, Commit,
// ForgetOwedUntilApplied and RetryNeverDeletesNewerMark), and every mark the
// kernel refused, so it is tried again in the next commit until it lands, a
// newer mark of its key replaces it, or a forget of its key drops it
// (MarkOwedUntilApplied, KeepRefusedMarks, ForgetDropsKeptMark): the walk's
// per-slot cache has already advanced and will not take that mark again.
// This is pure data: no syscall, no allocation, no kernel program. Its arrays
// are its owner's, bound at create, capacity entries each: one per client of
// the server's configured max_clients, rounded up to a multiple of 1024
// (rocketnet_xdp_replay_entries).

// The key layout for one source address and port, in network order.

static inline void rocketnet_xdp_replay_key_from_address( const struct rocketnet_address_t * address, struct rocketnet_server_xdp_replay_key_t * key )
{
    memset( key, 0, sizeof( *key ) );

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

// the entries of a server's pending buffer and of its kernel replay map: one
// per client of its configured max_clients, rounded up to a multiple of 1024
// (1024 at 1000 clients), never more than the embedded program's
// ROCKETNET_XDP_REPLAY_MAX_ENTRIES
static inline uint32_t rocketnet_xdp_replay_entries( int max_clients )
{
    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
        return 0;
    return (uint32_t) ( max_clients + 1023 ) / 1024 * 1024;
}

// the entries at the default configuration, for storage a test declares statically
#define ROCKETNET_XDP_REPLAY_DEFAULT_ENTRIES ( ( ROCKETNET_DEFAULT_MAX_CLIENTS + 1023 ) / 1024 * 1024 )

struct rocketnet_xdp_replay_marks_t
{
    struct rocketnet_server_xdp_replay_key_t * forgets;            // capacity entries each
    struct rocketnet_server_xdp_replay_key_t * mark_keys;
    struct rocketnet_server_xdp_replay_value_t * mark_values;
    uint32_t capacity;
    uint32_t forget_count;
    uint32_t mark_count;
    uint32_t kept_mark_count;           // marks [0, kept_mark_count) were refused by the last commit
    uint32_t refused;
};

// Binds the buffer to its owner's arrays of capacity entries, empty.

static inline void rocketnet_xdp_replay_marks_reset( struct rocketnet_xdp_replay_marks_t * marks );

static inline void rocketnet_xdp_replay_marks_bind( struct rocketnet_xdp_replay_marks_t * marks,
                                                    struct rocketnet_server_xdp_replay_key_t * forgets,
                                                    struct rocketnet_server_xdp_replay_key_t * mark_keys,
                                                    struct rocketnet_server_xdp_replay_value_t * mark_values,
                                                    uint32_t capacity )
{
    memset( marks, 0, sizeof( *marks ) );
    marks->forgets = forgets;
    marks->mark_keys = mark_keys;
    marks->mark_values = mark_values;
    marks->capacity = forgets && mark_keys && mark_values ? capacity : 0;
    rocketnet_xdp_replay_marks_reset( marks );
}

static inline void rocketnet_xdp_replay_marks_reset( struct rocketnet_xdp_replay_marks_t * marks )
{
    marks->forget_count = 0;
    marks->mark_count = 0;
    marks->kept_mark_count = 0;
    marks->refused = 0;
}

// The end of a commit: forget_failed[i] says the kernel refused forget i,
// mark_applied[j] says the kernel took mark j. The refused forgets move to
// the front in their order and stay pending, so the next commit applies them
// before any mark (proofs/tla/RNXdpReplay.tla, Commit) -- except a refused forget
// whose key a mark applied in the same commit: that mark, taken after the
// forget was queued, is a newer session's and now owns the kernel entry, so
// the stale forget is superseded and dropped, and a later retry can never
// delete it (RetryNeverDeletesNewerMark). The marks the kernel refused move to
// the front in their order and stay pending for the next commit
// (MarkOwedUntilApplied); the applied marks are cleared. Returns the refusals
// counted since the last reset and zeroes them, so the caller adds them to
// its own count.

// proofs/tla/RNXdpReplayMarksLayout.tla Commit, CompactForgetStep and
// CompactMarkStep: each loop writes at kept, never above the index it reads, so
// no entry is overwritten before it is read (LayoutIsQueue)
static inline uint32_t rocketnet_xdp_replay_marks_reset_keeping( struct rocketnet_xdp_replay_marks_t * marks, const bool * forget_failed, const bool * mark_applied )
{
    uint32_t kept = 0;
    for ( uint32_t i = 0; i < marks->forget_count; i++ )
    {
        if ( !forget_failed[i] )
            continue;

        bool superseded = false;
        for ( uint32_t j = 0; j < marks->mark_count && !superseded; j++ )
            superseded = mark_applied[j] && memcmp( &marks->mark_keys[j], &marks->forgets[i], sizeof( marks->forgets[i] ) ) == 0;
        if ( superseded )
            continue;

        if ( kept != i )
            marks->forgets[kept] = marks->forgets[i];

        kept++;
    }

    marks->forget_count = kept;

    uint32_t kept_marks = 0;
    for ( uint32_t j = 0; j < marks->mark_count; j++ )
    {
        if ( mark_applied[j] )
            continue;

        if ( kept_marks != j )
        {
            marks->mark_keys[kept_marks] = marks->mark_keys[j];
            marks->mark_values[kept_marks] = marks->mark_values[j];
        }

        kept_marks++;
    }

    marks->mark_count = kept_marks;
    marks->kept_mark_count = kept_marks;

    const uint32_t refused = marks->refused;
    marks->refused = 0;

    return refused;
}

// Drops the kept mark at index i, keeping the order of the rest.

// proofs/tla/RNXdpReplayMarksLayout.tla Forget: the shift down over [i + 1, mark_count)
static inline void rocketnet_xdp_replay_marks_drop_kept( struct rocketnet_xdp_replay_marks_t * marks, uint32_t i )
{
    for ( uint32_t j = i + 1; j < marks->mark_count; j++ )
    {
        marks->mark_keys[j - 1] = marks->mark_keys[j];
        marks->mark_values[j - 1] = marks->mark_values[j];
    }

    marks->mark_count--;
    marks->kept_mark_count--;
}

// Queues one forget per source key, coalescing repeats even when the list is
// full. Returns false, and counts the refusal, when a new key cannot fit.
// A queued forget, including a repeat, drops a kept mark
// of the same key: that mark was taken before the slot let the address go,
// and the commit applies forgets before marks, so kept it would rewrite the
// released address's entry (proofs/tla/RNXdpReplay.tla, ForgetDropsKeptMark).

// proofs/tla/RNXdpReplayMarksLayout.tla Forget: search below forget_count before
// refusing a new key at capacity (InBounds); the kept search stops at
// kept_mark_count, never reading stale cells past it (LayoutIsQueue)
static inline bool rocketnet_xdp_replay_marks_forget( struct rocketnet_xdp_replay_marks_t * marks, const struct rocketnet_address_t * address )
{
    struct rocketnet_server_xdp_replay_key_t key;
    rocketnet_xdp_replay_key_from_address( address, &key );

    bool pending = false;
    for ( uint32_t i = 0; i < marks->forget_count && !pending; i++ )
        pending = memcmp( &marks->forgets[i], &key, sizeof( key ) ) == 0;

    if ( !pending )
    {
        if ( marks->forget_count >= marks->capacity )
        {
            marks->refused++;
            return false;
        }

        marks->forgets[marks->forget_count++] = key;
    }

    for ( uint32_t i = 0; i < marks->kept_mark_count; i++ )
    {
        if ( memcmp( &marks->mark_keys[i], &key, sizeof( key ) ) == 0 )
        {
            rocketnet_xdp_replay_marks_drop_kept( marks, i );
            break;
        }
    }

    return true;
}

// Adds a mark of the high-water value for the source the address names. A
// kept mark of the same key, refused by the last commit, is replaced in
// place: the newer value is the one owed (proofs/tla/RNXdpReplay.tla, Walk). Only
// the kept prefix is searched; the walk marks each key once per update.
// Returns false, and counts the refusal, when the mark list is full.

// proofs/tla/RNXdpReplayMarksLayout.tla Mark: replaced in place in the kept
// prefix, else appended below MAX
static inline bool rocketnet_xdp_replay_marks_mark( struct rocketnet_xdp_replay_marks_t * marks, const struct rocketnet_address_t * address, uint32_t high_water )
{
    struct rocketnet_server_xdp_replay_key_t key;
    rocketnet_xdp_replay_key_from_address( address, &key );

    for ( uint32_t i = 0; i < marks->kept_mark_count; i++ )
    {
        if ( memcmp( &marks->mark_keys[i], &key, sizeof( key ) ) == 0 )
        {
            marks->mark_values[i].high_water = high_water;
            marks->mark_values[i].pad = 0;
            return true;
        }
    }

    if ( marks->mark_count >= marks->capacity )
    {
        marks->refused++;
        return false;
    }

    marks->mark_keys[marks->mark_count] = key;
    marks->mark_values[marks->mark_count].high_water = high_water;
    marks->mark_values[marks->mark_count].pad = 0;
    marks->mark_count++;

    return true;
}

// The pending forgets, in insertion order, any kept from the last commit
// first.

#if ROCKETNET_DEV
static inline void rocketnet_xdp_replay_marks_forgets( const struct rocketnet_xdp_replay_marks_t * marks,
                                                       const struct rocketnet_server_xdp_replay_key_t ** keys,
                                                       uint32_t * count )
{
    *keys = marks->forgets;
    *count = marks->forget_count;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNXdpReplay.tla Commit, HoldGreetingUntilMarked: a kept mark of 0 is a
// new session's first mark that the kernel refused, so the old session's
// entry may still stand for its key, and the session must not be greeted.
// True when any kept mark is 0 (the kept prefix is empty in steady state).

static inline bool rocketnet_xdp_replay_marks_holding_greetings( const struct rocketnet_xdp_replay_marks_t * marks )
{
    for ( uint32_t i = 0; i < marks->kept_mark_count; i++ )
    {
        if ( marks->mark_values[i].high_water == 0 )
            return true;
    }

    return false;
}

// True when a kept mark of 0 names the address: its greeting is held.

static inline bool rocketnet_xdp_replay_marks_holds_greeting( const struct rocketnet_xdp_replay_marks_t * marks, const struct rocketnet_address_t * address )
{
    struct rocketnet_server_xdp_replay_key_t key;
    rocketnet_xdp_replay_key_from_address( address, &key );

    for ( uint32_t i = 0; i < marks->kept_mark_count; i++ )
    {
        if ( marks->mark_values[i].high_water == 0 && memcmp( &marks->mark_keys[i], &key, sizeof( key ) ) == 0 )
            return true;
    }

    return false;
}

// The pending marks: any kept from the last commit first, then in insertion
// order.

#if ROCKETNET_DEV
static inline void rocketnet_xdp_replay_marks_marks( const struct rocketnet_xdp_replay_marks_t * marks,
                                                     const struct rocketnet_server_xdp_replay_key_t ** keys,
                                                     const struct rocketnet_server_xdp_replay_value_t ** values,
                                                     uint32_t * count )
{
    *keys = marks->mark_keys;
    *values = marks->mark_values;
    *count = marks->mark_count;
}
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_XDP_REPLAY_MARKS_H

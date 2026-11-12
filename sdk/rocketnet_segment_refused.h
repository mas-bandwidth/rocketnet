/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The destinations that refused a segmented send, shared by every server
// transport that segments: gso (UDP_SEGMENT), io_uring (UDP_SEGMENT) and iocp
// (USO). Pure: no syscall, no allocation, no global state, no platform socket
// header. standard/TRANSPORT.md t2.4 rule 7.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SEGMENT_REFUSED_H
#define ROCKETNET_SDK_ROCKETNET_SEGMENT_REFUSED_H

#include <stdint.h>

#include "rocketnet_address.h"

// One destination refusing a segmented send (gso and io_uring: EINVAL, EIO or
// EOPNOTSUPP; iocp: WSAEINVAL or WSAEOPNOTSUPP: its route cannot take a
// segmented send, an IPsec policy say) is that destination's, never another's:
// its refused run is resent plain, and the destination is remembered so its
// later runs leave plain without a refusal. Segmentation turns off for the
// transport only when this many destinations have refused and no segmented
// send has been accepted since create: a kernel or device that cannot segment
// at all. One accepted segmented send shows the transport can segment, and
// from then on no refusal turns it off.
#define ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD                  8

// The refused destinations, a direct-mapped table of this many entries owned
// by the thread that sends (the gso TX worker, the io_uring main thread, the
// iocp TX worker). A destination maps to one entry by its address and the
// transport's seed and is matched on the whole address, so a destination that
// never refused is never sent plain by it. A refusing destination that maps to
// an occupied entry takes it, and the one it displaces pays one refusal again
// on its next run. Fixed size: no allocation, and a lookup per run whatever
// the client count. Modelled in proofs/tla/RNSegmentRefusal.tla: Run reads the
// table, Refuse adds to it.
#define ROCKETNET_SEGMENT_REFUSED_ENTRIES                                64

struct rocketnet_segment_refused_t
{
    struct rocketnet_address_t destination[ROCKETNET_SEGMENT_REFUSED_ENTRIES];     // type ROCKETNET_ADDRESS_NONE: empty
    uint32_t seed;                  // drawn at create (rocketnet_random_uint64): which entry an address takes
    int destinations;               // refusals that filled an empty entry: distinct refusing destinations, up to the table size
    bool accepted;                  // a segmented send was accepted since create
};

// The entry an address takes: FNV-1a over the seed, the address words and the
// port, then a finalizer that spreads every bit of the state over the entry
// index. The seed is per transport instance and random, so a client holding
// several refusing addresses cannot pick ones that land on another refusing
// client's entry (which would cost that client one plain resend per run).

static inline int rocketnet_segment_refused_entry( const struct rocketnet_segment_refused_t * refused, const struct rocketnet_address_t * address )
{
    uint32_t hash = ( 2166136261u ^ refused->seed ) * 16777619u;
    const int words = address->type == ROCKETNET_ADDRESS_IPV6 ? 8 : 2;
    for ( int i = 0; i < words; i++ )
    {
        hash = ( hash ^ address->data.ipv6[i] ) * 16777619u;
    }
    hash = ( hash ^ address->port ) * 16777619u;
    hash ^= hash >> 16;
    hash *= 0x85ebca6bu;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35u;
    hash ^= hash >> 16;
    return (int) ( hash % ROCKETNET_SEGMENT_REFUSED_ENTRIES );
}

static inline bool rocketnet_segment_refused_contains( const struct rocketnet_segment_refused_t * refused, const struct rocketnet_address_t * address )
{
    const struct rocketnet_address_t * entry = &refused->destination[rocketnet_segment_refused_entry( refused, address )];
    return entry->type != ROCKETNET_ADDRESS_NONE && rocketnet_address_equal( entry, address );
}

// remembers a refusing destination. true when segmentation should now turn
// off for the transport: the threshold of distinct destinations reached with
// no segmented send ever accepted
static inline bool rocketnet_segment_refused_add( struct rocketnet_segment_refused_t * refused, const struct rocketnet_address_t * address )
{
    struct rocketnet_address_t * entry = &refused->destination[rocketnet_segment_refused_entry( refused, address )];
    if ( entry->type == ROCKETNET_ADDRESS_NONE )
    {
        refused->destinations++;
    }
    *entry = *address;
    return !refused->accepted && refused->destinations >= ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
}

#endif // ROCKETNET_SDK_ROCKETNET_SEGMENT_REFUSED_H

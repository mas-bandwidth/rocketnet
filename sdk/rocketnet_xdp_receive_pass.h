/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP worker's receive pass over its RX ring and fill ring
// (rocketnet_server_socket_xdp_thread_function,
// sdk/rocketnet_server_socket_linux.cpp), one function for each step of
// proofs/tla/RNXdpReceiveRing.tla the worker takes: the peek (Peek), one
// descriptor read (Take), the end of the pass (Finish) and the flush of the
// pending return into the fill ring (Finish's flush and Refill). The worker
// calls them in that order; the trace check of the model
// (tests/tla_trace_xdp_receive_ring.cpp) calls the same functions over rings
// and a staging buffer it lays out itself. The staging buffer is any type
// with num_packets and the per-packet arrays eth, from, packet_data and
// packet_bytes, each of at least staging_capacity entries; the pending
// return is a rocketnet_pending_frames_t.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_XDP_RECEIVE_PASS_H
#define ROCKETNET_SDK_ROCKETNET_XDP_RECEIVE_PASS_H

#include "rocketnet_internal.h"
#include "rocketnet_xdp_compat.h"
#include "rocketnet_xdp_frame.h"

#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <stdint.h>
#include <string.h>

void rocketnet_xdp_receive_ring_finish( uint32_t num_packets, uint32_t num_consumed,
                                        void ( *cancel )( void * ring, uint32_t count ),
                                        void ( *release )( void * ring, uint32_t count ),
                                        void * ring );

// Peek: xsk_ring_cons__peek, at most the staging room and the pending
// return's room, whichever is less. Returns the descriptors peeked from
// *receive_index on, 0 when there is no room or nothing to read.

template <class Staging, class Pending>
static inline uint32_t rocketnet_xdp_receive_peek( struct xsk_ring_cons * ring, const Staging * receive_buffer, uint32_t staging_capacity,
                                                   const Pending * pending_return, uint32_t * receive_index )
{
    const uint32_t receive_capacity = staging_capacity - receive_buffer->num_packets;
#if defined( ROCKETNET_TLA_PLANT_XDP_RECEIVE_RING )
    // the trace check's plant (tests/tla_trace_xdp_receive_ring.cpp), the
    // reversed witness MCRNXdpReceiveRingBrokenPeekPastStaging: the peek
    // asks for the pending room alone, past the staging room
    (void) receive_capacity;
    const uint32_t receive_limit = (uint32_t) pending_return->available();
#else  // #if defined( ROCKETNET_TLA_PLANT_XDP_RECEIVE_RING )
    const uint32_t receive_limit = (uint32_t) pending_return->available() < receive_capacity ? (uint32_t) pending_return->available() : receive_capacity;
#endif // #if defined( ROCKETNET_TLA_PLANT_XDP_RECEIVE_RING )
    return receive_limit ? xsk_ring_cons__peek( ring, receive_limit, receive_index ) : 0;
}

// Take: reads the next peeked descriptor, receive_index + *num_consumed,
// and counts it consumed. Its frame goes into frame[*num_returned] when it
// lies in the receive region [region_begin, region_end), and a packet the
// codec takes is staged.

template <class Staging>
static inline void rocketnet_xdp_receive_take( struct xsk_ring_cons * ring, uint32_t receive_index, uint32_t * num_consumed,
                                               const void * umem, uint64_t region_begin, uint64_t region_end, int address_type,
                                               Staging * receive_buffer, uint32_t staging_capacity,
                                               uint64_t * frame, uint32_t * num_returned, struct rocketnet_xdp_receive_drops_t * drops )
{
    rocketnet_assert( receive_buffer->num_packets < staging_capacity );
    (void) staging_capacity;

    const struct xdp_desc * __restrict__ desc = xsk_ring_cons__rx_desc( ring, receive_index + *num_consumed );

    ( *num_consumed )++;

    // only a receive-region frame goes back to the fill queue; a
    // descriptor naming any other frame is dropped, never refilled
    // (proofs/tla/RNXdpReceiveRing.tla FillHoldsOnlyReceiveFrames)

    if ( rocketnet_xdp_fill_return( desc->addr, region_begin, region_end, &frame[*num_returned] ) )
    {
        ( *num_returned )++;
    }

    // the descriptor is checked against the receive region and
    // its own frame before a byte of it is read; a refused one is
    // counted once, in receive_drops.descriptor (a refused one in
    // the receive region still went back to the fill queue above,
    // like any dropped frame)

    const uint8_t * __restrict__ packet_data = rocketnet_xdp_receive_descriptor( umem, region_begin, region_end, desc->addr, desc->len, drops );
    if ( !packet_data )
    {
        return;
    }

    // the codec dispatches on the ethertype: IPv4 at its
    // offsets, IPv6 at the 40-byte fixed header, the from-address
    // in the family it read. a frame it does not take (another
    // ethertype, an IPv6 extension header, the family this server
    // does not serve, a frame too short for any server packet) is
    // counted by reason in receive_drops, never parsed at the
    // wrong offsets

    rocketnet_address_t from;
    const uint8_t * payload_data = NULL;
    int payload_bytes = 0;

    if ( rocketnet_xdp_frame_receive( packet_data, (size_t) desc->len, address_type, &from, &payload_data, &payload_bytes, drops ) )
    {
        const int index = (int) receive_buffer->num_packets;

        receive_buffer->from[index] = from;

        memcpy( receive_buffer->eth[index], packet_data + ETH_ALEN, ETH_ALEN );      // the ethernet source

        memcpy( receive_buffer->packet_data[index], payload_data, (size_t) payload_bytes );

        receive_buffer->packet_bytes[index] = (size_t) payload_bytes;

        receive_buffer->num_packets++;
    }
}

// Finish: rocketnet_xdp_receive_ring_finish cancels the num_packets -
// num_consumed descriptors not taken (xsk_ring_cons__cancel) and releases
// num_consumed (xsk_ring_cons__release), then the num_returned frames go
// into the pending return. Returns false when the pending return could not
// hold them: the peek was bounded by its room, so that is a broken
// invariant.

template <class Pending>
static inline bool rocketnet_xdp_receive_end( struct xsk_ring_cons * ring, uint32_t num_packets, uint32_t num_consumed,
                                              const uint64_t * frame, uint32_t num_returned, Pending * pending_return )
{
    rocketnet_xdp_receive_ring_finish( num_packets, num_consumed, []( void * r, uint32_t n )
                                       { xsk_ring_cons__cancel( (struct xsk_ring_cons *) r, n ); }, []( void * r, uint32_t n )
                                       { xsk_ring_cons__release( (struct xsk_ring_cons *) r, n ); }, ring );

    // The peek was bounded by available pending storage, so every
    // returned frame remains owned until a full fill reservation. The
    // fill credit is the returned count, not num_consumed: a refused
    // descriptor outside the receive region is released from the RX
    // ring above but never refilled.

    const bool retained = pending_return->append( frame, num_returned );
    rocketnet_assert( retained );

    return retained;
}

// The flush: every pending frame onto the fill ring, or none when libxdp
// reserves fewer than all of them (the pending return keeps them for a
// later pass). Returns false for a reservation of zero.

template <class Pending>
static inline bool rocketnet_xdp_fill_flush( Pending * pending_return, struct xsk_ring_prod * fill_queue )
{
    return pending_return->flush(
        [&]( uint32_t count, uint32_t * index )
        { return xsk_ring_prod__reserve( fill_queue, count, index ); },
        [&]( uint32_t index, uint64_t frame )
        { *xsk_ring_prod__fill_addr( fill_queue, index ) = frame; },
        [&]( uint32_t count )
        { xsk_ring_prod__submit( fill_queue, count ); } );
}

#endif // ROCKETNET_SDK_ROCKETNET_XDP_RECEIVE_PASS_H

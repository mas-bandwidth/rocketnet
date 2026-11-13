/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Windows Registered I/O (RIO) server transport's region layout and request
// contexts, pure: no syscall, no allocation, no global state, no Windows
// header, so the Windows transport and the Linux unit suite share it. RIO
// addresses a registered buffer by 32-bit Offset and Length (the RIO_BUF
// struct), so every offset here fits in 32 bits. This is the byte-level
// counterpart of sdk/rocketnet_uring_codec.h: the byte-level parts of the
// transport (carving one registered region, the 64-bit request contexts) are
// written and unit tested here, and the transport links the RIO
// API for the socket and completion queues itself.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_RIO_LAYOUT_H
#define ROCKETNET_SDK_ROCKETNET_RIO_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include "rocketnet_network_threads.h"

// the kind fields carried in the upper bits of every RIO request's 64-bit
// RequestContext.
#define ROCKETNET_RIO_CONTEXT_SEND      1
#define ROCKETNET_RIO_CONTEXT_RECEIVE   2

// The layout of the single registered region: four sub-regions in order
// (send frames, send addresses, receive slices, receive addresses), each
// starting on a 64-byte boundary, each element of a sub-region its element
// size rounded up to 64. total_bytes is the end rounded up to page_bytes.
struct rocketnet_rio_layout_t
{
    uint64_t send_frames_offset;
    uint64_t send_addresses_offset;
    uint64_t receive_slices_offset;
    uint64_t receive_addresses_offset;
    uint64_t total_bytes;
    int send_frames;
    int receives;
    int frame_bytes;
    int receive_slice_bytes;
    int address_bytes;
};

static inline uint64_t rocketnet_rio_round_up64( uint64_t value, uint64_t boundary )
{
    return ( value + boundary - 1 ) & ~( boundary - 1 );
}

// Plans the region layout into out. Refuses (returns false, out untouched)
// any count or size <= 0, a page_bytes that is zero or not a power of two,
// and any total past UINT32_MAX.
static inline bool rocketnet_rio_layout_plan(
    int send_frames, int receives, int frame_bytes, int receive_slice_bytes,
    int address_bytes, uint64_t page_bytes,
    struct rocketnet_rio_layout_t * out )
{
    if ( send_frames <= 0 || receives <= 0 || frame_bytes <= 0 || receive_slice_bytes <= 0 || address_bytes <= 0 )
    {
        return false;
    }
    if ( page_bytes == 0 || ( page_bytes & ( page_bytes - 1 ) ) != 0 )
    {
        return false;
    }

    uint64_t frame_stride = rocketnet_rio_round_up64( (uint64_t) frame_bytes, 64 );
    uint64_t addr_stride = rocketnet_rio_round_up64( (uint64_t) address_bytes, 64 );
    uint64_t slice_stride = rocketnet_rio_round_up64( (uint64_t) receive_slice_bytes, 64 );

    uint64_t send_frames_count = (uint64_t) send_frames;
    uint64_t receives_count = (uint64_t) receives;

    // the send-frames sub-region starts at offset 0.

    uint64_t send_frames_bytes;
    if ( send_frames_count > UINT64_MAX / frame_stride )
    {
        return false;
    }
    send_frames_bytes = send_frames_count * frame_stride;

    uint64_t send_addresses_bytes;
    if ( send_frames_count > UINT64_MAX / addr_stride )
    {
        return false;
    }
    send_addresses_bytes = send_frames_count * addr_stride;

    uint64_t receive_slices_bytes;
    if ( receives_count > UINT64_MAX / slice_stride )
    {
        return false;
    }
    receive_slices_bytes = receives_count * slice_stride;

    uint64_t receive_addresses_bytes;
    if ( receives_count > UINT64_MAX / addr_stride )
    {
        return false;
    }
    receive_addresses_bytes = receives_count * addr_stride;

    uint64_t send_addresses_offset = send_frames_bytes;
    uint64_t receive_slices_offset;
    if ( send_addresses_offset > UINT64_MAX - send_addresses_bytes )
    {
        return false;
    }
    receive_slices_offset = send_addresses_offset + send_addresses_bytes;

    uint64_t receive_addresses_offset;
    if ( receive_slices_offset > UINT64_MAX - receive_slices_bytes )
    {
        return false;
    }
    receive_addresses_offset = receive_slices_offset + receive_slices_bytes;

    uint64_t end;
    if ( receive_addresses_offset > UINT64_MAX - receive_addresses_bytes )
    {
        return false;
    }
    end = receive_addresses_offset + receive_addresses_bytes;

    // the end is refused past UINT32_MAX before it is rounded, so the
    // rounding cannot pass UINT64_MAX and wrap to a small total

    if ( end > UINT32_MAX )
    {
        return false;
    }

    uint64_t total = rocketnet_rio_round_up64( end, page_bytes );
    if ( total > UINT32_MAX )
    {
        return false;
    }

    out->send_frames_offset = 0;
    out->send_addresses_offset = send_addresses_offset;
    out->receive_slices_offset = receive_slices_offset;
    out->receive_addresses_offset = receive_addresses_offset;
    out->total_bytes = total;
    out->send_frames = send_frames;
    out->receives = receives;
    out->frame_bytes = frame_bytes;
    out->receive_slice_bytes = receive_slice_bytes;
    out->address_bytes = address_bytes;

    return true;
}

static inline uint64_t rocketnet_rio_element_stride( int element_bytes )
{
    return rocketnet_rio_round_up64( (uint64_t) element_bytes, 64 );
}

// the byte offset of element index in its sub-region; UINT64_MAX for an
// index below 0 or past the count.
static inline uint64_t rocketnet_rio_send_frame_offset(
    const struct rocketnet_rio_layout_t * p, int index )
{
    if ( index < 0 || index >= p->send_frames )
    {
        return UINT64_MAX;
    }
    return p->send_frames_offset + (uint64_t) index * rocketnet_rio_element_stride( p->frame_bytes );
}

static inline uint64_t rocketnet_rio_send_address_offset(
    const struct rocketnet_rio_layout_t * p, int index )
{
    if ( index < 0 || index >= p->send_frames )
    {
        return UINT64_MAX;
    }
    return p->send_addresses_offset + (uint64_t) index * rocketnet_rio_element_stride( p->address_bytes );
}

static inline uint64_t rocketnet_rio_receive_slice_offset(
    const struct rocketnet_rio_layout_t * p, int index )
{
    if ( index < 0 || index >= p->receives )
    {
        return UINT64_MAX;
    }
    return p->receive_slices_offset + (uint64_t) index * rocketnet_rio_element_stride( p->receive_slice_bytes );
}

static inline uint64_t rocketnet_rio_receive_address_offset(
    const struct rocketnet_rio_layout_t * p, int index )
{
    if ( index < 0 || index >= p->receives )
    {
        return UINT64_MAX;
    }
    return p->receive_addresses_offset + (uint64_t) index * rocketnet_rio_element_stride( p->address_bytes );
}

// packs kind in bits 56..63, phase in bits 48..55 and index in bits 0..31.
static inline uint64_t rocketnet_rio_context( int kind, int phase, uint32_t index )
{
    return ( (uint64_t) (uint8_t) kind << 56 ) | ( (uint64_t) (uint8_t) phase << 48 ) | index;
}

static inline int rocketnet_rio_context_kind( uint64_t context )
{
    return (int) ( ( context >> 56 ) & 0xFF );
}

static inline int rocketnet_rio_context_phase( uint64_t context )
{
    return (int) ( ( context >> 48 ) & 0xFF );
}

static inline int rocketnet_rio_context_index( uint64_t context )
{
    return (int) ( context & 0xFFFFFFFF );
}

// Lanes. A transport of lanes runs one socket, region, queue set and worker
// per lane, every lane socket bound to the one server address and port. A
// datagram to a destination is staged in the lane its destination hashes to,
// so every frame to one client leaves through one lane in order. The packet
// id start_packet returns carries the lane: lane * lane_slots + index, where
// index < lane_slots (the lane's phases times its phase size).
// proofs/tla/RNRioLaneLayout.tla models the arithmetic. The most lanes,
// ROCKETNET_RIO_MAX_LANES (32), is the server's num_rio_lanes limit, written
// in sdk/rocketnet_network_threads.h.

// FNV-1a over the family, the port and the address bytes the family uses
// (the same function the xdp transport's reply table hashes with), so the
// lane of a destination is fixed for the transport's life. family is the
// address type, port the port in host order, bytes 4 or 16 address bytes.
static inline uint32_t rocketnet_rio_lane_hash( uint8_t family, uint16_t port, const uint8_t * address, int bytes )
{
    uint32_t hash = 2166136261u;
    hash = ( hash ^ family ) * 16777619u;
    hash = ( hash ^ (uint8_t) ( port & 0xFF ) ) * 16777619u;
    hash = ( hash ^ (uint8_t) ( port >> 8 ) ) * 16777619u;
    for ( int i = 0; i < bytes; ++i )
    {
        hash = ( hash ^ address[i] ) * 16777619u;
    }
    return hash;
}

// proofs/tla/RNRioLaneLayout.tla LaneOf: the lane of a hash, 0 .. lanes - 1.
// The high half is folded into the low first: FNV-1a's low bits depend only
// on the low bits of its input bytes, and the modulo of a power-of-two lane
// count reads only those.
static inline int rocketnet_rio_lane_of( uint32_t hash, int lanes )
{
    return lanes <= 1 ? 0 : (int) ( ( hash ^ ( hash >> 16 ) ) % (uint32_t) lanes );
}

// proofs/tla/RNRioLaneLayout.tla Encode: the transport's packet id of a
// lane's slot index
static inline uint64_t rocketnet_rio_lane_packet_id( int lane, int index, int lane_slots )
{
    return (uint64_t) lane * (uint64_t) lane_slots + (uint64_t) index;
}

// proofs/tla/RNRioLaneLayout.tla Decode: the lane and slot index of a packet
// id. Refuses (returns false, outputs untouched) an id past the last lane's
// last slot, or lanes or lane_slots below 1.
static inline bool rocketnet_rio_lane_packet_decode( uint64_t packet_id, int lanes, int lane_slots, int * lane, int * index )
{
    if ( lanes < 1 || lane_slots < 1 || packet_id >= (uint64_t) lanes * (uint64_t) lane_slots )
    {
        return false;
    }
#if !defined( ROCKETNET_RIO_LANE_PLANT )
    *lane = (int) ( packet_id / (uint64_t) lane_slots );
#else  // #if !defined( ROCKETNET_RIO_LANE_PLANT )
    // the trace check's plant (tests/tla_trace_rio_lane.cpp): the lane read
    // as the id modulo the lane count, which sends a finish to another lane
    *lane = (int) ( packet_id % (uint64_t) lanes );
#endif // #if !defined( ROCKETNET_RIO_LANE_PLANT )
    *index = (int) ( packet_id % (uint64_t) lane_slots );
    return true;
}

#endif // ROCKETNET_SDK_ROCKETNET_RIO_LAYOUT_H

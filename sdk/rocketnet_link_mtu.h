/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_LINK_MTU_H
#define ROCKETNET_SDK_ROCKETNET_LINK_MTU_H

#include "rocketnet_wire.h"

// The link MTU list (standard/TRANSPORT.md t2.3, t11.4, t12 rule 15): the
// client and server configs each carry an ordered list of link MTUs, largest
// first, the same list on both ends. The default is the one entry 1500, which
// is exactly the behaviour of a build without the list: no probe, nothing new
// on the wire. A longer list is tried first to last by the client's path
// probe after the challenge (proofs/tla/RNPathProbe.tla), and the size the
// probe proves carried both ways is the connection's packet size for its
// whole life, at both ends.
//
// Everything here is plain arithmetic on the list, shared by the client, the
// server and the tests, so the two ends can never derive different sizes.

// the most entries a list holds (rocketnet.h ROCKETNET_MAX_LINK_MTUS)
#define ROCKETNET_LINK_MTU_MAX_ENTRIES          8

// the largest entry: the 1500-byte link the packet size derives from
#define ROCKETNET_LINK_MTU_MAX                  ROCKETNET_LINK_MTU_BYTES

// the smallest entry: an IPv4 link whose UDP payload carries the connection
// request, the largest handshake packet, so the smallest entry of any list
// can still connect over IPv4 (546 = 518 + 20 + 8)
#define ROCKETNET_LINK_MTU_MIN                  ( ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES )

// every IPv6 link carries at least 1280 bytes (RFC 8200 section 5): an IPv6
// connection skips the entries below it, and uses 1280 when none is left
#define ROCKETNET_LINK_MTU_IPV6_MIN             1280

// the packet size one link MTU gives one address family: the MTU less the IP
// and UDP headers (28 for IPv4, 48 for IPv6), never above the ceiling
// ROCKETNET_MAX_PACKET_BYTES (1452): a 1500 link and a 1492 link both give
// 1452 over IPv4

static inline int rocketnet_link_mtu_packet_bytes( int link_mtu, int address_type )
{
    const int bytes = link_mtu - rocketnet_wire_header_bytes( address_type );
    return bytes < ROCKETNET_SEND_BUDGET ? bytes : ROCKETNET_SEND_BUDGET;
}

// The candidate packet sizes of one address family, in the order the probe
// tries them: each entry mapped to its packet size, IPv6 skipping entries
// below 1280 (1280 alone when none is left), and an entry whose size equals
// the one before it collapsed into it. Returns the count, 1 to
// ROCKETNET_LINK_MTU_MAX_ENTRIES, for a list that passed
// rocketnet_link_mtus_check; 0 for an address with no family.
//
//   { 1500 }               IPv4 { 1452 }               IPv6 { 1452 }
//   { 1492, 1200, 576 }    IPv4 { 1452, 1172, 548 }    IPv6 { 1444 }

static inline int rocketnet_link_mtu_candidates( const int * link_mtus, int num_link_mtus, int address_type, int * out_packet_bytes )
{
    if ( rocketnet_wire_header_bytes( address_type ) == 0 )
    {
        return 0;
    }

    int count = 0;

    for ( int i = 0; i < num_link_mtus && i < ROCKETNET_LINK_MTU_MAX_ENTRIES; i++ )
    {
        if ( address_type == 2 && link_mtus[i] < ROCKETNET_LINK_MTU_IPV6_MIN )   // ROCKETNET_ADDRESS_IPV6
        {
            continue;
        }

        const int bytes = rocketnet_link_mtu_packet_bytes( link_mtus[i], address_type );

        if ( count > 0 && out_packet_bytes[count - 1] == bytes )
        {
            continue;
        }

        out_packet_bytes[count++] = bytes;
    }

    if ( count == 0 && address_type == 2 )
    {
        out_packet_bytes[count++] = rocketnet_link_mtu_packet_bytes( ROCKETNET_LINK_MTU_IPV6_MIN, address_type );
    }

    return count;
}

// whether packet_bytes is one of the candidate sizes this list gives the family

static inline bool rocketnet_link_mtu_allows( const int * link_mtus, int num_link_mtus, int address_type, int packet_bytes )
{
    int candidates[ROCKETNET_LINK_MTU_MAX_ENTRIES];
    const int count = rocketnet_link_mtu_candidates( link_mtus, num_link_mtus, address_type, candidates );
    for ( int i = 0; i < count; i++ )
    {
        if ( candidates[i] == packet_bytes )
        {
            return true;
        }
    }
    return false;
}

// the list checks, each with its own refusal (rocketnet.h, the
// ROCKETNET_STATUS_LINK_MTU_* statuses): 0 for a valid list

#define ROCKETNET_LINK_MTUS_VALID               0
#define ROCKETNET_LINK_MTUS_EMPTY               1   // no entry, or more than ROCKETNET_LINK_MTU_MAX_ENTRIES
#define ROCKETNET_LINK_MTUS_NOT_DESCENDING      2   // an entry not below the one before it
#define ROCKETNET_LINK_MTUS_TOO_LARGE           3   // an entry above 1500
#define ROCKETNET_LINK_MTUS_TOO_SMALL           4   // an entry below ROCKETNET_LINK_MTU_MIN

static inline int rocketnet_link_mtus_check( const int * link_mtus, int num_link_mtus )
{
    if ( num_link_mtus < 1 || num_link_mtus > ROCKETNET_LINK_MTU_MAX_ENTRIES )
    {
        return ROCKETNET_LINK_MTUS_EMPTY;
    }

    for ( int i = 1; i < num_link_mtus; i++ )
    {
        if ( link_mtus[i] >= link_mtus[i - 1] )
        {
            return ROCKETNET_LINK_MTUS_NOT_DESCENDING;
        }
    }

    if ( link_mtus[0] > ROCKETNET_LINK_MTU_MAX )
    {
        return ROCKETNET_LINK_MTUS_TOO_LARGE;
    }

    if ( link_mtus[num_link_mtus - 1] < ROCKETNET_LINK_MTU_MIN )
    {
        return ROCKETNET_LINK_MTUS_TOO_SMALL;
    }

    return ROCKETNET_LINK_MTUS_VALID;
}

// the smallest packet size any family takes from a valid list: its last
// entry over IPv4 (an IPv6 size is never smaller, the IPv6 floor being 1280)

static inline int rocketnet_link_mtus_smallest_packet_bytes( const int * link_mtus, int num_link_mtus )
{
    return rocketnet_link_mtu_packet_bytes( link_mtus[num_link_mtus - 1], 1 );     // ROCKETNET_ADDRESS_IPV4
}

// What one packet size gives the layers above the transport, per connection:
//
//   payload capacity  C(P) = (P - 33) / 8 * 8: the packet less its prefilter,
//                     sequence and tag (26) and the smallest ack block (7),
//                     rounded down to a multiple of 8; 1416 at the ceiling
//   block stride      F(P) = 1152, the stride of every connection at the
//                     ceiling, while C(P) >= 1173 (the stride, its 20-byte
//                     header and the packet's one-byte record tag); below,
//                     (C(P) - 21) / 16 * 16, a multiple of 16 as the ring
//                     placement requires (standard/MESSAGES.md m2.2)
//
//   P 1452  C 1416  F 1152        P 1172  C 1136  F 1104
//   P 1444  C 1408  F 1152        P  548  C  512  F  480

#define ROCKETNET_LINK_MTU_PAYLOAD_OVERHEAD_BYTES   33      // prefilter 6 + sequence 4 + tag 16 + the smallest ack block 7
#define ROCKETNET_LINK_MTU_FULL_STRIDE_BYTES        1152    // ROCKETNET_MESSAGE_FRAGMENT_BYTES
#define ROCKETNET_LINK_MTU_STRIDE_OVERHEAD_BYTES    21      // the block's 20-byte header and the packet's one-byte record tag

static inline int rocketnet_link_mtu_payload_bytes( int packet_bytes )
{
    return ( packet_bytes - ROCKETNET_LINK_MTU_PAYLOAD_OVERHEAD_BYTES ) / 8 * 8;
}

static inline int rocketnet_link_mtu_block_stride( int packet_bytes )
{
    const int capacity = rocketnet_link_mtu_payload_bytes( packet_bytes );
    if ( capacity >= ROCKETNET_LINK_MTU_FULL_STRIDE_BYTES + ROCKETNET_LINK_MTU_STRIDE_OVERHEAD_BYTES )
    {
        return ROCKETNET_LINK_MTU_FULL_STRIDE_BYTES;
    }
    return ( capacity - ROCKETNET_LINK_MTU_STRIDE_OVERHEAD_BYTES ) / 16 * 16;
}

ROCKETNET_WIRE_STATIC_ASSERT( ( ROCKETNET_SEND_BUDGET - ROCKETNET_LINK_MTU_PAYLOAD_OVERHEAD_BYTES ) / 8 * 8 == 1416,
                              "the ceiling's payload capacity is ROCKETNET_MAX_PACKET_PAYLOAD_BYTES" );
ROCKETNET_WIRE_STATIC_ASSERT( ROCKETNET_PREFILTER_BYTES + 4 + 16 + ROCKETNET_ACK_BLOCK_MIN_BYTES == ROCKETNET_LINK_MTU_PAYLOAD_OVERHEAD_BYTES,
                              "the payload overhead is the packet overhead and the smallest ack block" );

ROCKETNET_WIRE_STATIC_ASSERT( ROCKETNET_LINK_MTU_MIN - ROCKETNET_IPV4_HEADER_BYTES - ROCKETNET_UDP_HEADER_BYTES == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                              "the smallest link MTU carries the connection request over IPv4" );
ROCKETNET_WIRE_STATIC_ASSERT( ROCKETNET_LINK_MTU_IPV6_MIN - ROCKETNET_IPV6_HEADER_BYTES - ROCKETNET_UDP_HEADER_BYTES >= ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                              "the IPv6 minimum link carries the connection request" );

#endif // ROCKETNET_SDK_ROCKETNET_LINK_MTU_H

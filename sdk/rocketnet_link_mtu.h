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
// on the wire. On a longer list a connection starts at its smallest candidate
// size P0, so it joins as fast as the default, and once connected the
// client's path probe raises it, largest candidate first, to the largest size
// proven carried both ways (proofs/tla/RNPathProbe.tla). The size only ever
// rises, at both ends, and the probe ends for good once the size is the
// largest candidate or every larger one is ruled out.
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

// The entry of the list a connection of this family at packet_bytes is using:
// the first entry, so the largest, that gives the family that size (1500, not
// 1492, for 1452 over IPv4 on { 1500, 1492 }); 1280 for an IPv6 connection
// whose list has no entry of 1280 or more (the size the candidates fall back
// to); 0 for a size no entry gives, or no connection (packet_bytes 0).

static inline int rocketnet_link_mtu_entry( const int * link_mtus, int num_link_mtus, int address_type, int packet_bytes )
{
    if ( packet_bytes <= 0 || rocketnet_wire_header_bytes( address_type ) == 0 )
    {
        return 0;
    }

    for ( int i = 0; i < num_link_mtus && i < ROCKETNET_LINK_MTU_MAX_ENTRIES; i++ )
    {
        if ( address_type == 2 && link_mtus[i] < ROCKETNET_LINK_MTU_IPV6_MIN )   // ROCKETNET_ADDRESS_IPV6
        {
            continue;
        }
        if ( rocketnet_link_mtu_packet_bytes( link_mtus[i], address_type ) == packet_bytes )
        {
            return link_mtus[i];
        }
    }

    if ( address_type == 2 && rocketnet_link_mtu_packet_bytes( ROCKETNET_LINK_MTU_IPV6_MIN, address_type ) == packet_bytes )
    {
        return ROCKETNET_LINK_MTU_IPV6_MIN;
    }

    return 0;
}

// The connection's first packet size, P0: the smallest candidate the list
// gives the family, the last; the ceiling for an address with no family.

static inline int rocketnet_link_mtu_first_packet_bytes( const int * link_mtus, int num_link_mtus, int address_type )
{
    int candidates[ROCKETNET_LINK_MTU_MAX_ENTRIES];
    const int count = rocketnet_link_mtu_candidates( link_mtus, num_link_mtus, address_type, candidates );
    return count > 0 ? candidates[count - 1] : ROCKETNET_SEND_BUDGET;
}

// The path probe after connect (standard/TRANSPORT.md t11.4;
// proofs/tla/RNPathProbe.tla). The client sends a pair -- a small probe,
// which every path carries, and a large probe at the candidate, largest
// first -- at connect, so on a clean path the size is the largest within one
// round trip and the confirm. A pair resolves when the next is due. The next
// is due FIRST_INTERVAL after connect (or twice the handshake's round trip,
// when that is longer), and every pair that resolves without success
// doubles the interval up to MAX_INTERVAL: one pair a second at most under
// loss, so the first pair after a loss clears goes out within a second.
// While the small probes are lost too nothing counts and the probe keeps
// going: it has no time limit, and ends only at the largest candidate or
// when every larger one is ruled out (or with the connection).
//
// A pair counts only when its small probe came back: then a missing large
// reply is a strike against the candidate, and STRIKES counted strikes rule
// it out. A strike counts only when its pair was sent STRIKE_SPACING after
// the pair of the strike counted before it, so four counted strikes span at
// least 15 s and a loss burst shorter than that never rules a carried size
// out; under independent loss p each way a carried size is ruled out with
// probability at most (1 - (1 - p)^2)^4: 0.13% at p = 10%, 1.7% at 20%. A
// size the path does not carry, on a path that carries the small probes, is
// ruled out about 16 s after its first pair. A large reply at the candidate
// from any earlier pair still proves it carried, so a round trip longer
// than the interval costs nothing. A pair costs at most 1452 + 34
// bytes each way, one a second at most, and none once the walk ends. An
// unechoed confirm is resent every CONFIRM_INTERVAL.

#define ROCKETNET_LINK_MTU_PROBE_FIRST_INTERVAL_SECONDS 0.25
#define ROCKETNET_LINK_MTU_PROBE_MAX_INTERVAL_SECONDS   1.0
#define ROCKETNET_LINK_MTU_PROBE_STRIKE_SPACING_SECONDS 5.0
#define ROCKETNET_LINK_MTU_PROBE_STRIKES                4
#define ROCKETNET_LINK_MTU_CONFIRM_INTERVAL_SECONDS     1.0

// The probe's opened plaintext: an 8-byte header, then zero padding to the
// wire size. kind 0 is a probe, whose size field is its own wire size and
// whose pair field pairs a small probe with its large one; kind 1 is a
// confirm, whose size field is the packet size the client confirms and whose
// pair field is zero. The server's echo carries the header unchanged.

#define ROCKETNET_PROBE_HEADER_BYTES                    8
#define ROCKETNET_PROBE_KIND_PROBE                      0
#define ROCKETNET_PROBE_KIND_CONFIRM                    1

// reads and checks an opened probe plaintext of a packet of wire_bytes: the
// header well formed for its kind (a probe's size field its own wire size; a
// confirm small, its pair zero) and the padding zero. False on any fault.

static inline bool rocketnet_probe_header_read( const uint8_t * opened, int opened_bytes, int wire_bytes,
                                                int * out_kind, int * out_size, uint32_t * out_pair )
{
    if ( opened_bytes < ROCKETNET_PROBE_HEADER_BYTES )
    {
        return false;
    }

    const int kind = opened[0];
    const int size = (int) opened[2] | ( (int) opened[3] << 8 );
    const uint32_t pair = (uint32_t) opened[4] | ( (uint32_t) opened[5] << 8 ) | ( (uint32_t) opened[6] << 16 ) | ( (uint32_t) opened[7] << 24 );

    if ( opened[1] != 0 )
    {
        return false;
    }

    if ( kind == ROCKETNET_PROBE_KIND_PROBE )
    {
        if ( size != wire_bytes )
        {
            return false;
        }
    }
    else if ( kind == ROCKETNET_PROBE_KIND_CONFIRM )
    {
        if ( wire_bytes != ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES || pair != 0 )
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    for ( int i = ROCKETNET_PROBE_HEADER_BYTES; i < opened_bytes; i++ )
    {
        if ( opened[i] != 0 )
        {
            return false;
        }
    }

    *out_kind = kind;
    *out_size = size;
    *out_pair = pair;
    return true;
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

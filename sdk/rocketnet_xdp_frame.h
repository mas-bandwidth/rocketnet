/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP frame codec: writes and reads Ethernet + IPv4/IPv6 + UDP at
// fixed byte offsets. It is plain portable C++ with no Linux headers, no
// libbpf and no allocation, so the unit suite builds it on every platform.
//
// IPv4 is ihl 5, the DF bit, ttl 64, the header checksum and UDP checksum 0.
// IPv6 carries the mandatory
// UDP checksum computed in software over the RFC 8200 section 8.1
// pseudo-header (RFC 768, RFC 1071); a computed 0x0000 is sent as 0xFFFF.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_XDP_FRAME_H
#define ROCKETNET_SDK_ROCKETNET_XDP_FRAME_H

#include "rocketnet_address.h"
#include "rocketnet_wire.h"
#include "rocketnet_constants.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ROCKETNET_ETHERTYPE_IP      0x0800
#define ROCKETNET_ETHERTYPE_IPV6    0x86DD
#define ROCKETNET_ETHERTYPE_VLAN    0x8100      // one IEEE 802.1Q tag
#define ROCKETNET_VLAN_TAG_BYTES    4
#define ROCKETNET_VLAN_ID_MAX       4094
#define ROCKETNET_UDP_PROTOCOL      17
#define ROCKETNET_IPV4_VERSION_IHL  0x45
#define ROCKETNET_IPV6_VERSION       0x60

// MAC PARSE: one strict link-layer address parse. Accepts exactly 17
// characters after a single trailing newline is stripped: six hex pairs
// separated by ':' or '.'. Returns false, leaving mac untouched, for
// anything else.

static inline int rocketnet_xdp_mac_hex_digit( char c )
{
    if ( c >= '0' && c <= '9' )
    {
        return c - '0';
    }
    if ( c >= 'a' && c <= 'f' )
    {
        return c - 'a' + 10;
    }
    if ( c >= 'A' && c <= 'F' )
    {
        return c - 'A' + 10;
    }
    return -1;
}

static inline bool rocketnet_xdp_parse_mac_address( const char * text, uint8_t mac[6] )
{
    if ( !text || !mac )
    {
        return false;
    }

    size_t length = strlen( text );
    if ( length > 0 && text[length - 1] == '\n' )
    {
        length--;
    }

    if ( length != 17 )
    {
        return false;
    }

    for ( size_t i = 0; i < length; ++i )
    {
        if ( ( i % 3 ) == 2 )
        {
            if ( text[i] != ':' && text[i] != '.' )
            {
                return false;
            }
        }
        else if ( rocketnet_xdp_mac_hex_digit( text[i] ) < 0 )
        {
            return false;
        }
    }

    uint8_t parsed[6];
    for ( size_t i = 0; i < 6; ++i )
    {
        const int high = rocketnet_xdp_mac_hex_digit( text[3 * i] );
        const int low = rocketnet_xdp_mac_hex_digit( text[3 * i + 1] );
        parsed[i] = (uint8_t) ( ( high << 4 ) | low );
    }

    memcpy( mac, parsed, sizeof( parsed ) );
    return true;
}

// the frame header length per family: ethernet + (IPv4 or IPv6) + UDP.
// the flush copies a payload to this offset before the build writes the
// headers in front of it.

static inline int rocketnet_xdp_frame_header_bytes( int address_type )
{
    return ( address_type == ROCKETNET_ADDRESS_IPV4 )
               ? ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES
               : ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
}

// VLAN: a transport's vlan_id is 0 on an untagged network, else the 802.1Q
// VLAN ID (1 to ROCKETNET_VLAN_ID_MAX) of the VLAN device that owns the
// server's address. A tagged transport writes one 802.1Q tag after the MACs
// of every frame it builds (AF_XDP transmit bypasses the kernel's VLAN
// offload, so the frame must carry it), and every offset after the MACs
// moves 4 bytes. The payload size limits are those of the IP packet and do
// not change; the tagged frame is 4 bytes longer on the wire, the 1522-byte
// tagged frame of IEEE 802.3 at a 1500-byte MTU.

static inline int rocketnet_xdp_frame_tag_bytes( int vlan_id )
{
    return vlan_id > 0 ? ROCKETNET_VLAN_TAG_BYTES : 0;
}

static inline int rocketnet_xdp_frame_header_bytes_vlan( int address_type, int vlan_id )
{
    return rocketnet_xdp_frame_header_bytes( address_type ) + rocketnet_xdp_frame_tag_bytes( vlan_id );
}

// ones'-complement checksum over big-endian 16-bit words (RFC 1071).

static inline uint16_t rocketnet_xdp_frame_checksum( const uint8_t * data, size_t bytes )
{
    uint32_t sum = 0;
    while ( bytes >= 2 )
    {
        sum += ( (uint32_t) data[0] << 8 ) | data[1];
        data += 2;
        bytes -= 2;
    }

    if ( bytes )
    {
        sum += ( (uint32_t) data[0] << 8 );
    }

    while ( sum >> 16 )
    {
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    }

    return (uint16_t) ~sum;
}

// the IPv6 UDP checksum: the pseudo-header, the 8-byte UDP header with its
// checksum field zero, and the payload (RFC 8200 section 8.1).

static inline uint16_t rocketnet_xdp_frame_ipv6_udp_checksum(
    const uint8_t * src, const uint8_t * dst, uint32_t udp_length,
    const uint8_t * udp_header, int payload_bytes )
{
    uint32_t sum = 0;
    int i;

    for ( i = 0; i < 16; i += 2 )
    {
        sum += ( (uint32_t) src[i] << 8 ) | src[i + 1];
    }
    for ( i = 0; i < 16; i += 2 )
    {
        sum += ( (uint32_t) dst[i] << 8 ) | dst[i + 1];
    }

    sum += (uint16_t) ( udp_length >> 16 );
    sum += (uint16_t) ( udp_length & 0xFFFF );
    sum += 0x0011; // three zero bytes then next header 17

    for ( i = 0; i < 8; i += 2 )
    {
        sum += ( (uint32_t) udp_header[i] << 8 ) | udp_header[i + 1];
    }

    for ( i = 0; i + 1 < payload_bytes; i += 2 )
    {
        sum += ( (uint32_t) udp_header[8 + i] << 8 ) | udp_header[9 + i];
    }
    if ( payload_bytes & 1 )
    {
        sum += ( (uint32_t) udp_header[8 + payload_bytes - 1] << 8 );
    }

    while ( sum >> 16 )
    {
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    }

    uint16_t result = (uint16_t) ~sum;
    if ( result == 0 )
    {
        result = 0xFFFF;
    }

    return result;
}

// writes the IPv4 wire bytes for a host-order uint16_t group (the rocketnet
// address stores IPv6 words in host order).

static inline void rocketnet_xdp_frame_write_be16( uint8_t * out, uint16_t value )
{
    out[0] = (uint8_t) ( value >> 8 );
    out[1] = (uint8_t) ( value & 0xFF );
}

// A built frame of frame_bytes fits a link of this MTU: its IP packet, the
// frame less the ethernet header, is at most mtu bytes. An mtu of 0 is
// unknown and fits every frame. The kernel's AF_XDP transmit does not check
// the MTU: in copy mode it hands the frame to the device, which drops it
// uncounted by the server (veth) or puts it on the wire, so the server
// refuses it before the TX ring and counts it.

static inline int rocketnet_xdp_frame_fits_mtu_vlan( int frame_bytes, int mtu, int vlan_id )
{
    return mtu <= 0 || frame_bytes - ROCKETNET_ETHERNET_HEADER_BYTES - rocketnet_xdp_frame_tag_bytes( vlan_id ) <= mtu;
}

static inline int rocketnet_xdp_frame_fits_mtu( int frame_bytes, int mtu )
{
    return rocketnet_xdp_frame_fits_mtu_vlan( frame_bytes, mtu, 0 );
}

// BUILD: writes ethernet (with the 802.1Q tag of vlan_id when it is above 0)
// + IP + UDP in front of a payload already at the family's header offset
// (rocketnet_xdp_frame_header_bytes_vlan). Returns the frame length, or 0 for
// a refusal; a vlan_id past ROCKETNET_VLAN_ID_MAX refuses.

static inline int rocketnet_xdp_frame_build_vlan(
    void * frame, size_t frame_bytes,
    const uint8_t * server_mac, const uint8_t * gateway_mac,
    const rocketnet_address_t * source, const rocketnet_address_t * destination,
    int payload_bytes, int vlan_id )
{
    uint8_t * f = (uint8_t *) frame;
    int payload_index;
    int total;

    if ( !frame || !server_mac || !gateway_mac || !source || !destination )
    {
        return 0;
    }
    if ( source->type != destination->type )
    {
        return 0;
    }
    if ( payload_bytes < 0 || vlan_id < 0 || vlan_id > ROCKETNET_VLAN_ID_MAX )
    {
        return 0;
    }

    const int ipv6 = ( source->type == ROCKETNET_ADDRESS_IPV6 );
    const int tag_bytes = rocketnet_xdp_frame_tag_bytes( vlan_id );
    const int header_bytes = rocketnet_xdp_frame_header_bytes_vlan( source->type, vlan_id );
    const int max_payload = ipv6
                                ? ROCKETNET_MAX_PACKET_BYTES
                                : ROCKETNET_RECEIVE_PACKET_BYTES;
    if ( payload_bytes > max_payload )
    {
        return 0;
    }
    if ( (size_t) ( header_bytes + payload_bytes ) > frame_bytes )
    {
        return 0;
    }

    memcpy( f, gateway_mac, 6 );
    memcpy( f + 6, server_mac, 6 );
    uint16_t ethertype = ipv6 ? ROCKETNET_ETHERTYPE_IPV6 : ROCKETNET_ETHERTYPE_IP;

    // the tag: 0x8100, then priority 0, DEI 0 and the VLAN ID. e is the frame
    // as an untagged one would be laid out from the ethertype on: every
    // offset below is the untagged frame's

    if ( tag_bytes )
    {
        rocketnet_xdp_frame_write_be16( f + 12, ROCKETNET_ETHERTYPE_VLAN );
        rocketnet_xdp_frame_write_be16( f + 14, (uint16_t) vlan_id );
    }

    uint8_t * e = f + tag_bytes;

    payload_index = header_bytes;
    total = header_bytes + payload_bytes;

    if ( ipv6 )
    {
        rocketnet_xdp_frame_write_be16( e + 12, ethertype );
        e[14] = ROCKETNET_IPV6_VERSION;
        e[15] = 0;
        e[16] = 0;
        e[17] = 0;
        rocketnet_xdp_frame_write_be16( e + 18, (uint16_t) ( ROCKETNET_UDP_HEADER_BYTES + payload_bytes ) );
        e[20] = ROCKETNET_UDP_PROTOCOL;
        e[21] = 64;

        for ( int i = 0; i < 8; ++i )
        {
            rocketnet_xdp_frame_write_be16( e + 22 + 2 * (size_t) i, source->data.ipv6[i] );
        }
        for ( int i = 0; i < 8; ++i )
        {
            rocketnet_xdp_frame_write_be16( e + 38 + 2 * (size_t) i, destination->data.ipv6[i] );
        }
    }
    else
    {
        rocketnet_xdp_frame_write_be16( e + 12, ethertype );
        e[14] = ROCKETNET_IPV4_VERSION_IHL;
        e[15] = 0x00;
        rocketnet_xdp_frame_write_be16( e + 16, (uint16_t) ( ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES + payload_bytes ) );
        e[18] = 0;
        e[19] = 0;
        e[20] = 0x40;
        e[21] = 0x00;
        e[22] = 64;
        e[23] = ROCKETNET_UDP_PROTOCOL;
        e[24] = 0;
        e[25] = 0;
        e[26] = source->data.ipv4[0];
        e[27] = source->data.ipv4[1];
        e[28] = source->data.ipv4[2];
        e[29] = source->data.ipv4[3];
        e[30] = destination->data.ipv4[0];
        e[31] = destination->data.ipv4[1];
        e[32] = destination->data.ipv4[2];
        e[33] = destination->data.ipv4[3];

        uint16_t ip_check = rocketnet_xdp_frame_checksum( e + 14, ROCKETNET_IPV4_HEADER_BYTES );
        rocketnet_xdp_frame_write_be16( e + 24, ip_check );
    }

    // UDP header

    rocketnet_xdp_frame_write_be16( f + payload_index - 8, source->port );
    rocketnet_xdp_frame_write_be16( f + payload_index - 6, destination->port );
    rocketnet_xdp_frame_write_be16( f + payload_index - 4, (uint16_t) ( ROCKETNET_UDP_HEADER_BYTES + payload_bytes ) );

    if ( ipv6 )
    {
        // the checksum sums the UDP header with its checksum field zero: a
        // reused frame still holds the previous datagram's checksum there

        f[payload_index - 2] = 0;
        f[payload_index - 1] = 0;
        uint16_t udp_check = rocketnet_xdp_frame_ipv6_udp_checksum(
            e + 22, e + 38, (uint32_t) ( ROCKETNET_UDP_HEADER_BYTES + payload_bytes ),
            f + payload_index - 8, payload_bytes );
        rocketnet_xdp_frame_write_be16( f + payload_index - 2, udp_check );
    }
    else
    {
        f[payload_index - 2] = 0;
        f[payload_index - 1] = 0;
    }

    return total;
}

// the untagged build: every frame of a transport on an untagged network

static inline int rocketnet_xdp_frame_build(
    void * frame, size_t frame_bytes,
    const uint8_t * server_mac, const uint8_t * gateway_mac,
    const rocketnet_address_t * source, const rocketnet_address_t * destination,
    int payload_bytes )
{
    return rocketnet_xdp_frame_build_vlan( frame, frame_bytes, server_mac, gateway_mac, source, destination, payload_bytes, 0 );
}

// the 802.1Q tag of a frame, read for a transport of vlan_id: returns the
// tag's length (0 for an untagged frame, ROCKETNET_VLAN_TAG_BYTES for one
// tag of vlan_id), or -1 when the frame is tagged for anything else: a
// tagged frame on an untagged transport, a VLAN ID that is not the
// transport's, or a tag cut by the frame end. An untagged frame is taken on
// a tagged transport too: a NIC that strips the tag on receive (rx VLAN
// offload, the ixgbe and i40e default) hands XDP the frame without it. The
// frame holds at least ROCKETNET_ETHERNET_HEADER_BYTES.

static inline int rocketnet_xdp_frame_vlan_tag( const uint8_t * f, size_t frame_bytes, int vlan_id )
{
    if ( ( ( f[12] << 8 ) | f[13] ) != ROCKETNET_ETHERTYPE_VLAN )
    {
        return 0;
    }
    if ( vlan_id <= 0 || frame_bytes < (size_t) ( ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_VLAN_TAG_BYTES ) ||
         ( ( ( f[14] << 8 ) | f[15] ) & 0x0FFF ) != vlan_id )
    {
        return -1;
    }

    return ROCKETNET_VLAN_TAG_BYTES;
}

// PARSE: reads ethernet (one 802.1Q tag of vlan_id, or none:
// rocketnet_xdp_frame_vlan_tag) + IP + UDP. Returns false for anything
// unrecognized, extension headers included; an IPv6 header with any traffic
// class and flow label is read. Fills the source address and a pointer/length
// into the payload bytes (inside frame).
// Length guards: the up-front check proves the frame holds the whole IPv4
// header set (ethernet + IPv4 + UDP) after the tag, enough to name the
// ethertype and the IP version; the IPv6 branch then proves the frame holds
// its larger header set before it reads any byte past the ethernet header,
// so a truncated IPv6 frame is refused before its source address or UDP port
// is read. Below, e and e_bytes are the frame from its first MAC with the tag
// stepped over, so every offset is the untagged frame's.

static inline bool rocketnet_xdp_frame_parse_vlan(
    const void * frame, size_t frame_bytes, int vlan_id,
    rocketnet_address_t * out_from,
    const uint8_t ** out_payload, int * out_payload_length )
{
    const uint8_t * f = (const uint8_t *) frame;
    int header_bytes;
    int payload_bytes;
    uint16_t udp_len;

    if ( !frame || !out_from || !out_payload || !out_payload_length )
    {
        return false;
    }
    if ( frame_bytes < (size_t) ROCKETNET_ETHERNET_HEADER_BYTES )
    {
        return false;
    }
    if ( f[0] == 0xFF && f[1] == 0xFF )
    {
        return false;
    }

    const int tag_bytes = rocketnet_xdp_frame_vlan_tag( f, frame_bytes, vlan_id );
    if ( tag_bytes < 0 )
    {
        return false;
    }

    const uint8_t * e = f + tag_bytes;
    const size_t e_bytes = frame_bytes - (size_t) tag_bytes;

    if ( e_bytes < (size_t) ( ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES ) )
    {
        return false;
    }

    uint16_t ethertype = (uint16_t) ( ( e[12] << 8 ) | e[13] );
    memset( out_from, 0, sizeof( *out_from ) );

    if ( ethertype == ROCKETNET_ETHERTYPE_IP )
    {
        if ( e[14] != ROCKETNET_IPV4_VERSION_IHL )
        {
            return false;
        }
        if ( e[23] != ROCKETNET_UDP_PROTOCOL )
        {
            return false;
        }

        header_bytes = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
        udp_len = (uint16_t) ( ( e[header_bytes - 4] << 8 ) | e[header_bytes - 3] );
        if ( udp_len < ROCKETNET_UDP_HEADER_BYTES )
        {
            return false;
        }

        out_from->type = ROCKETNET_ADDRESS_IPV4;
        out_from->data.ipv4[0] = e[26];
        out_from->data.ipv4[1] = e[27];
        out_from->data.ipv4[2] = e[28];
        out_from->data.ipv4[3] = e[29];
        out_from->port = (uint16_t) ( ( e[header_bytes - 8] << 8 ) | e[header_bytes - 7] );
    }
    else if ( ethertype == ROCKETNET_ETHERTYPE_IPV6 )
    {
        // the version nibble only: the traffic class and the 20-bit flow
        // label carry any value (a Linux sender sets a flow label by default)

        if ( ( e[14] & 0xF0 ) != ROCKETNET_IPV6_VERSION )
        {
            return false;
        }
        if ( e[20] != ROCKETNET_UDP_PROTOCOL )
        {
            return false;
        }

        header_bytes = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
        if ( e_bytes < (size_t) header_bytes )
        {
            return false;
        }

        udp_len = (uint16_t) ( ( e[header_bytes - 4] << 8 ) | e[header_bytes - 3] );
        if ( udp_len < ROCKETNET_UDP_HEADER_BYTES )
        {
            return false;
        }

        out_from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            out_from->data.ipv6[i] = (uint16_t) ( ( e[22 + 2 * i] << 8 ) | e[23 + 2 * i] );
        }
        out_from->port = (uint16_t) ( ( e[header_bytes - 8] << 8 ) | e[header_bytes - 7] );
    }
    else
    {
        return false;
    }

    // Bytes past the UDP length (link-layer padding) are not payload; a UDP
    // length past the frame end is refused, as server_xdp_udp drops it

    payload_bytes = (int) ( e_bytes - (size_t) header_bytes );
    if ( (int) udp_len - ROCKETNET_UDP_HEADER_BYTES > payload_bytes )
    {
        return false;
    }

    payload_bytes = (int) udp_len - ROCKETNET_UDP_HEADER_BYTES;

    *out_payload = e + header_bytes;
    *out_payload_length = payload_bytes;
    return true;
}

// the untagged parse: a tagged frame is refused

static inline bool rocketnet_xdp_frame_parse(
    const void * frame, size_t frame_bytes,
    rocketnet_address_t * out_from,
    const uint8_t ** out_payload, int * out_payload_length )
{
    return rocketnet_xdp_frame_parse_vlan( frame, frame_bytes, 0, out_from, out_payload, out_payload_length );
}

// RECEIVE: the AF_XDP receive loop's per-frame dispatch, for a server that
// serves address_type (ROCKETNET_ADDRESS_IPV4 or ROCKETNET_ADDRESS_IPV6). It
// dispatches on the ethertype: IPv4 at its offsets, IPv6 at the 40-byte fixed
// header, through the parse above, and the from-address carries the family
// it read. A frame is taken only when it holds the smallest server-inbound
// packet. Every frame it does not take is dropped and counted by name:
//
//   not_ip          the ethertype is neither IPv4 nor IPv6, or the frame is
//                   too short to name one, or it carries an 802.1Q tag that
//                   is not the transport's (rocketnet_xdp_frame_vlan_tag)
//   ipv6_extension  an IPv6 frame whose next header is an extension header
//                   (hop-by-hop, routing, fragment, destination options, AH,
//                   ESP): never parsed at the UDP offsets
//   other_family    a frame of the family this server does not serve
//   malformed       an IPv4 or IPv6 frame the parse refuses (IPv4 options, a
//                   next header that is not UDP), one too short for the
//                   smallest server packet, or an IPv6 frame from an
//                   IPv4-mapped source (sdk/rocketnet_address.h)
//   descriptor      a receive descriptor rocketnet_xdp_receive_descriptor
//                   refuses before the frame is read (below)
//
// The caller serialises writes to drops.

struct rocketnet_xdp_receive_drops_t
{
    uint64_t not_ip;
    uint64_t ipv6_extension;
    uint64_t other_family;
    uint64_t malformed;
    uint64_t descriptor;
};

// every frame the receive dropped, whatever the reason: the transport's
// XDP_RECEIVE_FRAME_DROPPED
static inline uint64_t rocketnet_xdp_receive_drops_total( const struct rocketnet_xdp_receive_drops_t * drops )
{
    return drops->not_ip + drops->ipv6_extension + drops->other_family + drops->malformed + drops->descriptor;
}

static inline bool rocketnet_xdp_frame_ipv6_is_extension( uint8_t next_header )
{
    return next_header == 0 ||     // hop-by-hop options
           next_header == 43 ||     // routing
           next_header == 44 ||     // fragment
           next_header == 60 ||     // destination options
           next_header == 51 ||     // AH
           next_header == 50;       // ESP
}

static inline bool rocketnet_xdp_frame_receive_vlan(
    const void * frame, size_t frame_bytes, int address_type, int vlan_id,
    rocketnet_address_t * out_from,
    const uint8_t ** out_payload, int * out_payload_length,
    struct rocketnet_xdp_receive_drops_t * drops )
{
    const uint8_t * f = (const uint8_t *) frame;

    if ( !frame || !out_from || !out_payload || !out_payload_length || !drops )
    {
        return false;
    }

    if ( frame_bytes < (size_t) ROCKETNET_ETHERNET_HEADER_BYTES )
    {
        drops->not_ip++;
        return false;
    }

    const int tag_bytes = rocketnet_xdp_frame_vlan_tag( f, frame_bytes, vlan_id );
    if ( tag_bytes < 0 )
    {
        drops->not_ip++;
        return false;
    }

    // e and e_bytes: the frame with the tag stepped over, at the untagged
    // frame's offsets

    const uint8_t * e = f + tag_bytes;
    const size_t e_bytes = frame_bytes - (size_t) tag_bytes;

    const uint16_t ethertype = (uint16_t) ( ( e[12] << 8 ) | e[13] );

    int family;
    if ( ethertype == ROCKETNET_ETHERTYPE_IP )
    {
        family = ROCKETNET_ADDRESS_IPV4;
    }
    else if ( ethertype == ROCKETNET_ETHERTYPE_IPV6 )
    {
        family = ROCKETNET_ADDRESS_IPV6;

        if ( e_bytes >= (size_t) ( ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES ) && rocketnet_xdp_frame_ipv6_is_extension( e[20] ) )
        {
            drops->ipv6_extension++;
            return false;
        }
    }
    else
    {
        drops->not_ip++;
        return false;
    }

    // address_type below zero accepts either family (ServedFamilies of two)

    if ( address_type >= 0 && family != address_type )
    {
        drops->other_family++;
        return false;
    }

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;

    if ( !rocketnet_xdp_frame_parse_vlan( frame, frame_bytes, vlan_id, &from, &payload, &payload_bytes ) || payload_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
    {
        drops->malformed++;
        return false;
    }

    // the real IPv6 header is seen here, so an IPv4-mapped source is spoofed
    // and dropped (rocketnet_address_accept_received_source, as every socket
    // transport drops it); counted with the frames whose header is refused

    if ( !rocketnet_address_accept_received_source( &from, false ) )
    {
        drops->malformed++;
        return false;
    }

    *out_from = from;
    *out_payload = payload;
    *out_payload_length = payload_bytes;
    return true;
}

// the untagged receive

static inline bool rocketnet_xdp_frame_receive(
    const void * frame, size_t frame_bytes, int address_type,
    rocketnet_address_t * out_from,
    const uint8_t ** out_payload, int * out_payload_length,
    struct rocketnet_xdp_receive_drops_t * drops )
{
    return rocketnet_xdp_frame_receive_vlan( frame, frame_bytes, address_type, 0, out_from, out_payload, out_payload_length, drops );
}

// TRANSPORT BOUNDS: the checks the AF_XDP transport makes before it copies into
// or reads from a UMEM frame, sets a cpu bit or sizes the stats read. Each
// holds in a Release build; none relies on an assert.
//
// SEND COPY: copies a staged packet of packet_bytes into a UMEM frame of
// frame_bytes at the header offset of address_type, where
// rocketnet_xdp_frame_build then writes the headers in front of it. A
// length of 0, one past ROCKETNET_RECEIVE_CEILING (a negative int stored
// into the size_t lands here too) or one that does not fit the frame after
// the header is refused before the copy: *refused is incremented and -1 is
// returned. Else it returns the payload length. The _vlan form copies to the
// header offset of a transport of vlan_id (rocketnet_xdp_frame_build_vlan).

static inline int rocketnet_xdp_send_copy_vlan(
    uint8_t * frame, size_t frame_bytes, int address_type, int vlan_id,
    const uint8_t * packet, size_t packet_bytes, uint64_t * refused )
{
    const int header_bytes = rocketnet_xdp_frame_header_bytes_vlan( address_type, vlan_id );
    if ( !frame || !packet || packet_bytes == 0 || packet_bytes > (size_t) ROCKETNET_RECEIVE_CEILING ||
         (size_t) header_bytes > frame_bytes || packet_bytes > frame_bytes - (size_t) header_bytes )
    {
        if ( refused )
        {
            ( *refused )++;
        }
        return -1;
    }

    memcpy( frame + header_bytes, packet, packet_bytes );
    return (int) packet_bytes;
}

static inline int rocketnet_xdp_send_copy(
    uint8_t * frame, size_t frame_bytes, int address_type,
    const uint8_t * packet, size_t packet_bytes, uint64_t * refused )
{
    return rocketnet_xdp_send_copy_vlan( frame, frame_bytes, address_type, 0, packet, packet_bytes, refused );
}

// RECEIVE DESCRIPTOR: the bytes a receive descriptor names inside the UMEM
// at umem. The receive frames are the region [region_begin, region_end),
// both multiples of ROCKETNET_XDP_FRAME_SIZE. A descriptor is taken only
// when its length is at most ROCKETNET_XDP_FRAME_SIZE, its address lies in
// the region and its bytes end inside the frame that holds its address (a
// received address carries the headroom offset into its frame, so it is
// not itself a frame boundary). Anything else is refused before the frame
// is read: drops->descriptor is incremented and NULL is returned.

static inline const uint8_t * rocketnet_xdp_receive_descriptor(
    const void * umem, uint64_t region_begin, uint64_t region_end,
    uint64_t addr, uint32_t len, struct rocketnet_xdp_receive_drops_t * drops )
{
    if ( !umem || len > ROCKETNET_XDP_FRAME_SIZE || addr < region_begin || addr >= region_end ||
         ( addr % ROCKETNET_XDP_FRAME_SIZE ) + len > ROCKETNET_XDP_FRAME_SIZE )
    {
        if ( drops )
        {
            drops->descriptor++;
        }
        return NULL;
    }

    return (const uint8_t *) umem + addr;
}

// FILL RETURN: whether a receive descriptor's frame goes back to the fill
// ring. Only a frame of the receive region [region_begin, region_end) is
// the receive path's to hand back; a descriptor naming any other frame (a
// send-region frame, which only a driver or kernel defect delivers) is
// never returned, so a frame on the send free list is never received into.
// On true *frame_addr is the address of the frame that holds addr (a
// received address carries the headroom offset into its frame). Counting
// a refused descriptor is rocketnet_xdp_receive_descriptor's, not this
// rule's. proofs/tla/RNXdpReceiveRing.tla: Take, FillHoldsOnlyReceiveFrames.

static inline bool rocketnet_xdp_fill_return( uint64_t addr, uint64_t region_begin, uint64_t region_end, uint64_t * frame_addr )
{
    if ( !frame_addr || addr < region_begin || addr >= region_end )
    {
        return false;
    }

    *frame_addr = addr - addr % ROCKETNET_XDP_FRAME_SIZE;
    return true;
}

// RECEIVE DRAIN: the queue a receive call visits at this step. start is the
// first queue of the call and step is how far around the ring it has walked.

static inline int rocketnet_xdp_drain_queue( int start, int step, int num_queues )
{
    if ( num_queues <= 0 )
    {
        return 0;
    }
    const int sum = start + step;
    const int mod = sum % num_queues;
    return mod < 0 ? mod + num_queues : mod;
}

// PIN CPU: a cpu is set in a cpu_set_t of set_size bits (CPU_SETSIZE on
// Linux) only when it lies in 0 .. set_size - 1.

static inline bool rocketnet_xdp_cpu_in_set( int cpu, int set_size )
{
    return cpu >= 0 && cpu < set_size;
}

// CPU LIST ENTRY: parse one kernel cpu-list entry ("3" or "1-7") at *cursor,
// advancing past it and any following comma. The list ends with the string
// terminator; only trailing whitespace (the sysfs newline) may follow. A
// malformed entry, a reversed range or trailing garbage refuses the list.

static inline bool rocketnet_xdp_cpu_list_entry( const char ** cursor, int * first, int * last )
{
    const char * p = *cursor;
    if ( *p < '0' || *p > '9' )
    {
        return false;
    }

    int value = 0;
    while ( *p >= '0' && *p <= '9' )
    {
        const int digit = *p - '0';
        if ( value > ( 0x7FFFFFFF - digit ) / 10 )
        {
            return false;
        }
        value = value * 10 + digit;

        p++;
    }
    *first = value;
    *last = value;

    if ( *p == '-' )
    {
        p++;
        if ( *p < '0' || *p > '9' )
        {
            return false;
        }

        value = 0;
        while ( *p >= '0' && *p <= '9' )
        {
            const int digit = *p - '0';
            if ( value > ( 0x7FFFFFFF - digit ) / 10 )
            {
                return false;
            }
            value = value * 10 + digit;

            p++;
        }
        *last = value;
    }

    if ( *last < *first )
    {
        return false;
    }

    if ( *p == ',' )
    {
        *cursor = p + 1;
        return true;
    }

    while ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' )
    {
        p++;
    }

    if ( *p != '\0' )
    {
        return false;
    }

    *cursor = p;
    return true;
}

// The cpu an XDP thread pins to: queue q takes cpu num_os_cpus + q. The
// operating system keeps core 0 on Linux, so the first XDP thread is on cpu 1
// at the least: a num_os_cpus below 1 reads as 1 and no queue ever lands on
// core 0.

static inline int rocketnet_xdp_thread_cpu( int num_os_cpus, int queue )
{
    return ( num_os_cpus < 1 ? 1 : num_os_cpus ) + queue;
}

// The mask an XDP thread falls back to when its own pin is refused (a cpu
// outside the set or offline, or the affinity call failing): the cpus it is
// allowed on less core 0, which the operating system keeps on Linux. allowed
// and out are bit arrays of bytes bytes, cpu n at bit n % 8 of byte n / 8 (a
// Linux cpu_set_t's layout). Returns how many cpus out holds; with 0 the
// thread has nowhere but core 0 and the caller leaves its mask alone.

static inline int rocketnet_xdp_mask_without_core_zero( const uint8_t * allowed, uint8_t * out, int bytes )
{
    int count = 0;
    for ( int i = 0; i < bytes; i++ )
    {
        out[i] = i == 0 ? (uint8_t) ( allowed[i] & 0xFE ) : allowed[i];
        for ( int bit = 0; bit < 8; bit++ )
        {
            count += ( out[i] >> bit ) & 1;
        }
    }
    return count;
}

// PIN TARGETS: every cpu this transport will pin to, num_os_cpus ..
// num_os_cpus + num_queues - 1, is in the kernel's isolated cpu-list
// ("1-7", "1,3,5-6", a trailing newline). A malformed or empty list never
// verifies. first_missing names the first target not in the list, or -1
// when every target is there.

static inline bool rocketnet_xdp_pin_targets_isolated( const char * isolated_list, int num_os_cpus, int num_queues, int * first_missing )
{
    if ( first_missing )
    {
        *first_missing = -1;
    }

    if ( !isolated_list || isolated_list[0] == '\0' || num_os_cpus < 0 || num_queues < 0 ||
         num_os_cpus > 0x7FFFFFFF - num_queues )
    {
        if ( first_missing )
        {
            *first_missing = num_os_cpus > 0 ? num_os_cpus : 0;
        }
        return false;
    }

    const char * cursor = isolated_list;
    while ( *cursor == ' ' || *cursor == '\t' )
    {
        cursor++;
    }

    // validate the whole list before trusting any of it: a malformed line is
    // an unmet condition, not a pass
    {
        const char * check = cursor;
        while ( true )
        {
            int first = 0;
            int last = 0;

            if ( !rocketnet_xdp_cpu_list_entry( &check, &first, &last ) )
            {
                if ( first_missing )
                {
                    *first_missing = num_os_cpus > 0 ? num_os_cpus : 0;
                }
                return false;
            }

            if ( *check == '\0' )
            {
                break;
            }
        }
    }

    const int first_cpu = rocketnet_xdp_thread_cpu( num_os_cpus, 0 );

    for ( int cpu = first_cpu; cpu < first_cpu + num_queues; cpu++ )
    {
        bool found = false;
        const char * scan = cursor;
        while ( true )
        {
            int first = 0;
            int last = 0;
            if ( !rocketnet_xdp_cpu_list_entry( &scan, &first, &last ) )
            {
                break; // validated above
            }

            if ( cpu >= first && cpu <= last )
            {
                found = true;
                break;
            }

            if ( *scan == '\0' )
            {
                break;
            }
        }

        if ( !found )
        {
            if ( first_missing )
            {
                *first_missing = cpu;
            }
            return false;
        }
    }

    return true;
}

// CPU LIST MEMBERSHIP: 1 when cpu is in the kernel cpu-list, 0 when it is not,
// -1 when the list is empty or malformed (the whole list is validated first,
// as PIN TARGETS does: a malformed list never answers).

static inline int rocketnet_xdp_cpu_list_has( const char * list, int cpu )
{
    if ( !list )
    {
        return -1;
    }

    const char * cursor = list;
    while ( *cursor == ' ' || *cursor == '\t' )
    {
        cursor++;
    }

    if ( *cursor == '\0' )
    {
        return -1;
    }

    int found = 0;
    while ( true )
    {
        int first = 0;
        int last = 0;
        if ( !rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) )
        {
            return -1;
        }

        if ( cpu >= first && cpu <= last )
        {
            found = 1;
        }

        if ( *cursor == '\0' )
        {
            break;
        }
    }

    return found;
}

// IRQAFFINITY (B1 of scripts/rocketnet-provision): the kernel command line's
// irqaffinity=<cpu-list>, left-bounded (start or a space before it) like every
// other B1 token. It keeps the machine's interrupts off the isolated cpus the
// xdp threads spin on, so any well-formed list with no cpu in the isolated set
// is met: irqaffinity=0 for the default num_os_cpus of 1, irqaffinity=0-1 for
// num_os_cpus 2. An isolated list that is NULL or empty judges the form only
// (B2 names the missing set). The script's B1 check is the same rule.

#define ROCKETNET_XDP_IRQAFFINITY_MET         1
#define ROCKETNET_XDP_IRQAFFINITY_ABSENT      0
#define ROCKETNET_XDP_IRQAFFINITY_MALFORMED  -1
#define ROCKETNET_XDP_IRQAFFINITY_ISOLATED   -2

static inline int rocketnet_xdp_cmdline_irqaffinity( const char * cmdline, const char * isolated_list, int * isolated_cpu )
{
    static const char token[] = "irqaffinity=";
    const size_t token_length = sizeof( token ) - 1;

    if ( isolated_cpu )
    {
        *isolated_cpu = -1;
    }

    if ( !cmdline )
    {
        return ROCKETNET_XDP_IRQAFFINITY_ABSENT;
    }

    const char * match = strstr( cmdline, token );
    while ( match && match != cmdline && match[-1] != ' ' )
    {
        match = strstr( match + 1, token );
    }

    if ( !match )
    {
        return ROCKETNET_XDP_IRQAFFINITY_ABSENT;
    }

    // the value, up to the next blank, copied so the list parser sees its end

    char value[256];
    size_t length = 0;
    const char * cursor = match + token_length;
    while ( cursor[length] != '\0' && cursor[length] != ' ' && cursor[length] != '\n' && cursor[length] != '\t' )
    {
        if ( length + 1 >= sizeof( value ) )
        {
            return ROCKETNET_XDP_IRQAFFINITY_MALFORMED;
        }
        value[length] = cursor[length];
        length++;
    }
    value[length] = '\0';

    if ( length == 0 || rocketnet_xdp_cpu_list_has( value, 0 ) < 0 )
    {
        return ROCKETNET_XDP_IRQAFFINITY_MALFORMED;
    }

    if ( !isolated_list || rocketnet_xdp_cpu_list_has( isolated_list, 0 ) < 0 )
    {
        return ROCKETNET_XDP_IRQAFFINITY_MET;
    }

    // every entry of the value against every entry of the isolated set,
    // range against range (a value of 0-2147483647 is one comparison per
    // isolated entry, never a walk over its cpus): the first overlap names
    // the lowest cpu the two share

    const char * scan = value;
    while ( true )
    {
        int first = 0;
        int last = 0;
        if ( !rocketnet_xdp_cpu_list_entry( &scan, &first, &last ) )
        {
            return ROCKETNET_XDP_IRQAFFINITY_MALFORMED;      // validated above
        }

        const char * isolated_scan = isolated_list;
        while ( *isolated_scan == ' ' || *isolated_scan == '\t' )
        {
            isolated_scan++;
        }
        while ( true )
        {
            int isolated_first = 0;
            int isolated_last = 0;
            if ( !rocketnet_xdp_cpu_list_entry( &isolated_scan, &isolated_first, &isolated_last ) )
            {
                return ROCKETNET_XDP_IRQAFFINITY_MET;        // validated above
            }

            const int low = first > isolated_first ? first : isolated_first;
            const int high = last < isolated_last ? last : isolated_last;
            if ( low <= high )
            {
                if ( isolated_cpu )
                {
                    *isolated_cpu = low;
                }
                return ROCKETNET_XDP_IRQAFFINITY_ISOLATED;
            }

            if ( *isolated_scan == '\0' )
            {
                break;
            }
        }

        if ( *scan == '\0' )
        {
            break;
        }
    }

    return ROCKETNET_XDP_IRQAFFINITY_MET;
}

// QUEUE VECTOR (R4): the queue k of an interrupts line that names the interface
// as <interface>-TxRx-<k> (ixgbe "eth0-TxRx-3", i40e "i40e-eth0-TxRx-3", ice
// "ice-eth0-TxRx-3"): the name bounded before by the line start, a blank or
// '-', exactly -TxRx- and digits after it, then nothing but trailing blanks.
// -1 for any other line: XDP's tx-only vectors ("eth0-tx-9"), the misc vector
// ("eth0"), other drivers' names. scripts/rocketnet-provision's
// queue_irq_lines_for_interface is the same rule.

static inline int rocketnet_xdp_queue_vector( const char * line, const char * interface_name )
{
    if ( !line || !interface_name || interface_name[0] == '\0' )
    {
        return -1;
    }

    const size_t name_length = strlen( interface_name );
    size_t line_length = strlen( line );
    while ( line_length > 0 && ( line[line_length - 1] == ' ' || line[line_length - 1] == '\t' ||
                                 line[line_length - 1] == '\n' || line[line_length - 1] == '\r' ) )
    {
        line_length--;
    }

    static const char suffix[] = "-TxRx-";
    const size_t suffix_length = sizeof( suffix ) - 1;

    for ( size_t start = 0; start + name_length <= line_length; start++ )
    {
        if ( memcmp( line + start, interface_name, name_length ) != 0 )
        {
            continue;
        }

        const char before = start == 0 ? ' ' : line[start - 1];
        if ( before != ' ' && before != '\t' && before != '-' )
        {
            continue;
        }

        size_t at = start + name_length;
        if ( line_length - at <= suffix_length || memcmp( line + at, suffix, suffix_length ) != 0 )
        {
            continue;
        }
        at += suffix_length;

        int queue = 0;
        bool digits = at < line_length;
        for ( size_t i = at; i < line_length; i++ )
        {
            if ( line[i] < '0' || line[i] > '9' || queue > ( 0x7FFFFFFF - ( line[i] - '0' ) ) / 10 )
            {
                digits = false;
                break;
            }
            queue = queue * 10 + ( line[i] - '0' );
        }

        if ( digits )
        {
            return queue;
        }
    }

    return -1;
}

// R8, THE REAL-TIME THROTTLE, by kernel release (scripts/rocketnet-provision
// r8_throttles, the same rule): every kernel before 6.12 stops a real-time
// thread that never sleeps for sched_rt_period_us - sched_rt_runtime_us in
// every period, and so does a later one built with CONFIG_RT_GROUP_SCHED. A
// release that does not parse as <major>.<minor> is taken as throttling
// (fails closed).

#define ROCKETNET_XDP_RT_THROTTLES            0     // before 6.12, or unparseable
#define ROCKETNET_XDP_RT_READ_CONFIG          1     // 6.12 or later: the build config decides

static inline int rocketnet_xdp_rt_release( const char * release )
{
    if ( !release )
    {
        return ROCKETNET_XDP_RT_THROTTLES;
    }

    int major = 0;
    int minor = 0;
    const char * p = release;

    if ( *p < '0' || *p > '9' )
    {
        return ROCKETNET_XDP_RT_THROTTLES;
    }
    while ( *p >= '0' && *p <= '9' )
    {
        if ( major < 100000 )      // a longer number is as large: it never wraps
        {
            major = major * 10 + ( *p - '0' );
        }
        p++;
    }

    if ( *p != '.' )
    {
        return ROCKETNET_XDP_RT_THROTTLES;
    }
    p++;

    if ( *p < '0' || *p > '9' )
    {
        return ROCKETNET_XDP_RT_THROTTLES;
    }
    while ( *p >= '0' && *p <= '9' )
    {
        if ( minor < 100000 )
        {
            minor = minor * 10 + ( *p - '0' );
        }
        p++;
    }

    if ( major < 6 || ( major == 6 && minor < 12 ) )
    {
        return ROCKETNET_XDP_RT_THROTTLES;
    }

    return ROCKETNET_XDP_RT_READ_CONFIG;
}

// one line of a kernel build config: is it exactly CONFIG_RT_GROUP_SCHED=y
// (the script's grep -qx)? a trailing newline is not part of the line

static inline bool rocketnet_xdp_rt_config_line( const char * line )
{
    static const char wanted[] = "CONFIG_RT_GROUP_SCHED=y";
    const size_t wanted_length = sizeof( wanted ) - 1;
    if ( !line || strncmp( line, wanted, wanted_length ) != 0 )
    {
        return false;
    }
    const char after = line[wanted_length];
    return after == '\0' || after == '\n';
}

// STATS READ: the per-cpu stats map is read into a buffer of
// ROCKETNET_XDP_STATS_MAX_CPUS values held inside the server's create-time
// allocation (8192 is the largest CONFIG_NR_CPUS the kernel allows). A
// possible-cpu count outside 1 .. ROCKETNET_XDP_STATS_MAX_CPUS is refused.

#define ROCKETNET_XDP_STATS_MAX_CPUS        8192

static inline bool rocketnet_xdp_stats_cpus_fit( int num_cpus )
{
    return num_cpus > 0 && num_cpus <= ROCKETNET_XDP_STATS_MAX_CPUS;
}

// STATS PUBLISH: a read that succeeded sums the per-cpu values into counters
// and keeps the sum as the last good read. A read that failed (the map
// lookup refused, or a cpu count that does not fit) publishes the last good
// read, never zeros, since a count the kernel could not report is not a
// count of nothing. The return is true when this read starts a run of
// failures, so the caller logs once per run.

static inline bool rocketnet_xdp_stats_publish( bool read, const struct rocketnet_server_xdp_stats_t * values, int num_cpus,
                                                uint64_t * last, bool * failing, uint64_t * counters )
{
    if ( read && rocketnet_xdp_stats_cpus_fit( num_cpus ) )
    {
        memset( last, 0, ROCKETNET_SERVER_XDP_NUM_COUNTERS * sizeof( uint64_t ) );
        for ( int cpu = 0; cpu < num_cpus; cpu++ )
        {
            for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
            {
                last[i] += values[cpu].counters[i];
            }
        }

        *failing = false;
        memcpy( counters, last, ROCKETNET_SERVER_XDP_NUM_COUNTERS * sizeof( uint64_t ) );
        return false;
    }

    memcpy( counters, last, ROCKETNET_SERVER_XDP_NUM_COUNTERS * sizeof( uint64_t ) );
    const bool starts = !*failing;
    *failing = true;
    return starts;
}

// REPLY MAC: the destination MAC address of a reply frame. A client's MAC is learned
// from the source MAC of a packet the transport accepted for a connection
// (it opened under the connection's keys), never from a packet it dropped or
// answered without authenticating; a reply goes to the newest MAC learned for
// its address, and with none learned to the gateway, as for routed traffic.
// With no gateway (an on-link segment with no default route, where the server
// refused to start before) a direct response to a received frame goes to that
// frame's source MAC, which is how an on-link handshake gets its first
// replies, and any other reply to an unlearned address is refused and
// counted, never sent to a zero MAC. The unauthenticated frame's MAC is never
// preferred over the gateway, so a spoofed frame steers no reply further
// than it can through the gateway.
//
// A reply can leave after the receive view it answers has gone: the
// development link simulator holds each reply for its latency, may send it
// twice, and so looks its MAC up only when it sends. So, with no gateway, an
// address keeps the MAC its replies last went to as an answered entry: the
// source MAC of the frame a direct response answered, and the learned MAC
// of a client its forget released. A reply to that address with no frame in
// hand goes there. An answered entry is never a learned one: a learned or
// gateway MAC always wins over it, it never displaces a learned entry, a
// learn of its address takes its slot, and a newer answer replaces it. A
// reply to an address no frame ever came from is still refused and counted.
// proofs/tla/RNXdpReplay.tla models the rule: NeverSeenNeverSentTo and
// HeldReplyFindsItsMac.
//
// The table is fixed-size from its bind and its entries live in its owner's
// create-time allocation: open addressing over a power-of-two count of
// slots (the index mask is the count less one), a lookup stopping at the
// first never-used slot and probing at most ROCKETNET_XDP_REPLY_MAC_PROBES
// slots. A learn takes the first never-used slot of its probe window, else
// replaces the window's oldest answered entry, else the entry learned longest
// ago; a forget leaves the address's entry in place as its answered entry.
// No allocation, no clock and no lock: its
// owner serialises writes.

// The table holds a learned MAC for every client of the server's configured
// max_clients at most a quarter full: four slots per sized client, rounded
// up to a power of two for the index mask (4096 at 1000 clients). The
// smallest table (at ROCKETNET_SERVER_MIN_SIZED_CLIENTS) is far wider than
// one probe window, so a window never wraps onto itself.

#define ROCKETNET_XDP_REPLY_MAC_PROBES      16

static inline int rocketnet_xdp_reply_mac_entries( int max_clients )
{
    return rocketnet_power_of_two_at_least( 4 * rocketnet_server_sized_clients( max_clients ) );
}

// the entries at the default configuration, for storage a test declares statically
#define ROCKETNET_XDP_REPLY_MAC_DEFAULT_ENTRIES     ROCKETNET_POWER_OF_TWO_AT_LEAST( 4 * ROCKETNET_DEFAULT_MAX_CLIENTS )

ROCKETNET_STATIC_ASSERT( ROCKETNET_POWER_OF_TWO_AT_LEAST( 4 * ROCKETNET_SERVER_MIN_SIZED_CLIENTS ) >= 4 * ROCKETNET_XDP_REPLY_MAC_PROBES,
                         "the smallest reply MAC table is wider than a probe window" );

#define ROCKETNET_XDP_REPLY_MAC_EMPTY       0
#define ROCKETNET_XDP_REPLY_MAC_LIVE        1
#define ROCKETNET_XDP_REPLY_MAC_ANSWERED    3

struct rocketnet_xdp_reply_mac_entry_t
{
    rocketnet_address_t address;
    uint64_t learned;
    uint8_t mac[6];
    uint8_t state;
};

struct rocketnet_xdp_reply_macs_t
{
    uint8_t gateway_mac[6];
    bool gateway_known;
    uint64_t learn_count;
    struct rocketnet_xdp_reply_mac_entry_t * entries;      // the owner's storage, bound by rocketnet_xdp_reply_macs_bind
    uint32_t mask;                                          // the entry count less one; the count is a power of two
    // Per-family gateways. Zero after reset, so a one-family table still uses gateway_mac.
    bool gateway_ipv4_known;
    bool gateway_ipv6_known;
    uint8_t gateway_ipv4[6];
    uint8_t gateway_ipv6[6];
};

// Binds the table to count entries of its owner's storage (count a power of
// two, at least four probe windows) and resets it with no gateway.

static inline void rocketnet_xdp_reply_macs_reset( struct rocketnet_xdp_reply_macs_t * table, const uint8_t * gateway_mac );

static inline bool rocketnet_xdp_reply_macs_bind( struct rocketnet_xdp_reply_macs_t * table, struct rocketnet_xdp_reply_mac_entry_t * entries, int count )
{
    memset( table, 0, sizeof( *table ) );
    if ( !entries || count < 4 * ROCKETNET_XDP_REPLY_MAC_PROBES || ( count & ( count - 1 ) ) != 0 )
    {
        return false;
    }
    table->entries = entries;
    table->mask = (uint32_t) count - 1;
    rocketnet_xdp_reply_macs_reset( table, NULL );
    return true;
}

// Every entry never used, the gateways cleared, the binding kept.
// gateway_mac NULL: no gateway is known for this interface.

// proofs/tla/RNXdpReplyMacsLayout.tla Reset: every slot EMPTY
static inline void rocketnet_xdp_reply_macs_reset( struct rocketnet_xdp_reply_macs_t * table, const uint8_t * gateway_mac )
{
    struct rocketnet_xdp_reply_mac_entry_t * entries = table->entries;
    const uint32_t mask = table->mask;
    memset( table, 0, sizeof( *table ) );
    table->entries = entries;
    table->mask = mask;
    if ( entries )
    {
        memset( entries, 0, ( (size_t) mask + 1 ) * sizeof( *entries ) );
    }

    if ( gateway_mac )
    {
        memcpy( table->gateway_mac, gateway_mac, 6 );
        table->gateway_known = true;
    }
}

static inline uint32_t rocketnet_xdp_reply_macs_slot( const struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address )
{
    // FNV-1a over the family, the port and the address bytes the family uses

    uint32_t hash = 2166136261u;
    const int bytes = ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? 16 : 4;
    const uint8_t * data = (const uint8_t *) &address->data;

    hash = ( hash ^ address->type ) * 16777619u;
    hash = ( hash ^ (uint8_t) ( address->port & 0xFF ) ) * 16777619u;
    hash = ( hash ^ (uint8_t) ( address->port >> 8 ) ) * 16777619u;
    for ( int i = 0; i < bytes; ++i )
    {
        hash = ( hash ^ data[i] ) * 16777619u;
    }

    return hash & table->mask;
}

// proofs/tla/RNXdpReplyMacsLayout.tla TableIsMap and NoEntryPastEmpty: the probe
// passes every used slot and ends only at an EMPTY one or after the window
static inline struct rocketnet_xdp_reply_mac_entry_t * rocketnet_xdp_reply_macs_find_state( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address, uint8_t state )
{
    const uint32_t slot = rocketnet_xdp_reply_macs_slot( table, address );

    for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; ++i )
    {
        struct rocketnet_xdp_reply_mac_entry_t * entry = &table->entries[( slot + (uint32_t) i ) & table->mask];
        if ( entry->state == ROCKETNET_XDP_REPLY_MAC_EMPTY )
        {
            return NULL;
        }

        if ( entry->state == state && rocketnet_address_equal( &entry->address, address ) )
        {
            return entry;
        }
    }

    return NULL;
}

// the learned entry for an address, NULL for none (an answered entry is not learned)

static inline struct rocketnet_xdp_reply_mac_entry_t * rocketnet_xdp_reply_macs_find( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address )
{
    return rocketnet_xdp_reply_macs_find_state( table, address, ROCKETNET_XDP_REPLY_MAC_LIVE );
}

static inline bool rocketnet_xdp_reply_mac_usable( const uint8_t * mac )
{
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };
    return mac && !( mac[0] & 1 ) && memcmp( mac, zero_mac, 6 ) != 0;
}

// proofs/tla/RNXdpReplay.tla Connect: a unicast, nonzero MAC is learned; anything
// else is ignored. The address's answered entry, if any, becomes its
// learned entry.

// proofs/tla/RNXdpReplyMacsLayout.tla Learn: the address's own entry, else the
// first EMPTY slot of the window, else its oldest ANSWERED entry, else the
// LIVE entry learned longest ago (EvictOnlyOldest)
static inline void rocketnet_xdp_reply_macs_learn( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address, const uint8_t * mac )
{
    if ( !table || !address || !rocketnet_xdp_reply_mac_usable( mac ) )
    {
        return;
    }

    table->learn_count++;

    struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( table, address );
    if ( !entry )
    {
        entry = rocketnet_xdp_reply_macs_find_state( table, address, ROCKETNET_XDP_REPLY_MAC_ANSWERED );
    }

    if ( !entry )
    {
        const uint32_t slot = rocketnet_xdp_reply_macs_slot( table, address );
        struct rocketnet_xdp_reply_mac_entry_t * oldest_answered = NULL;
        struct rocketnet_xdp_reply_mac_entry_t * oldest = NULL;

        for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; ++i )
        {
            struct rocketnet_xdp_reply_mac_entry_t * candidate = &table->entries[( slot + (uint32_t) i ) & table->mask];
            if ( candidate->state == ROCKETNET_XDP_REPLY_MAC_EMPTY )
            {
                entry = candidate;
                break;
            }

            if ( candidate->state == ROCKETNET_XDP_REPLY_MAC_ANSWERED )
            {
                if ( !oldest_answered || candidate->learned < oldest_answered->learned )
                {
                    oldest_answered = candidate;
                }
            }
            // the trace check's plant (tests/tla_trace_xdp_reply_macs.cpp):
            // the eviction takes the live entry learned last, the reversed
            // witness MCRNXdpReplyMacsLayoutBrokenEvictNewest.cfg
#if defined( ROCKETNET_XDP_REPLY_MACS_PLANT )
            else if ( !oldest || candidate->learned > oldest->learned )
#else  // #if defined( ROCKETNET_XDP_REPLY_MACS_PLANT )
            else if ( !oldest || candidate->learned < oldest->learned )
#endif // #if defined( ROCKETNET_XDP_REPLY_MACS_PLANT )
            {
                oldest = candidate;
            }
        }

        if ( !entry )
        {
            entry = oldest_answered ? oldest_answered : oldest;
        }

        entry->address = *address;
    }

    entry->state = ROCKETNET_XDP_REPLY_MAC_LIVE;
    memcpy( entry->mac, mac, 6 );
    entry->learned = table->learn_count;
}

// keeps mac as the address's answered entry: in its own answered entry, else
// a never-used slot, else the oldest answered entry of its probe window. A learned entry is never displaced; with no room the answer
// is not kept.

// proofs/tla/RNXdpReplyMacsLayout.tla Answer: the address's ANSWERED entry, else
// the first EMPTY slot of the window, else its oldest ANSWERED entry, else none
static inline void rocketnet_xdp_reply_macs_answer( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address, const uint8_t * mac )
{
    if ( rocketnet_xdp_reply_macs_find( table, address ) )
    {
        return;
    }

    table->learn_count++;

    struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find_state( table, address, ROCKETNET_XDP_REPLY_MAC_ANSWERED );
    if ( !entry )
    {
        const uint32_t slot = rocketnet_xdp_reply_macs_slot( table, address );
        struct rocketnet_xdp_reply_mac_entry_t * oldest = NULL;

        for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; ++i )
        {
            struct rocketnet_xdp_reply_mac_entry_t * candidate = &table->entries[( slot + (uint32_t) i ) & table->mask];
            if ( candidate->state == ROCKETNET_XDP_REPLY_MAC_EMPTY )
            {
                entry = candidate;
                break;
            }

            if ( candidate->state == ROCKETNET_XDP_REPLY_MAC_ANSWERED && ( !oldest || candidate->learned < oldest->learned ) )
            {
                oldest = candidate;
            }
        }

        if ( !entry )
        {
            entry = oldest;
        }

        if ( !entry )
        {
            return;
        }

        entry->address = *address;
        entry->state = ROCKETNET_XDP_REPLY_MAC_ANSWERED;
    }

    memcpy( entry->mac, mac, 6 );
    entry->learned = table->learn_count;
}

// proofs/tla/RNXdpReplay.tla Commit (ForgetLeavesAnswered): the released client's
// learned MAC is forgotten: no reply is addressed by it as a learned client's
// again. It stays as the address's answered entry, for a reply still held for
// the released client. The learned entry becomes the answered one in place:
// an address never holds both, since a learn takes over the address's
// answered entry and an answer is not kept for a learned address.

// proofs/tla/RNXdpReplyMacsLayout.tla Forget: LIVE becomes ANSWERED in place
static inline void rocketnet_xdp_reply_macs_forget( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address )
{
    struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( table, address );
    if ( entry )
    {
        entry->state = ROCKETNET_XDP_REPLY_MAC_ANSWERED;
    }
}

// proofs/tla/RNXdpReplay.tla Answer (answered_mac given) and Drain (none given);
// proofs/tla/RNXdpReplyMacsLayout.tla ReplyLearned (the learned MAC returned),
// ReplyAnswered (the answered entry's MAC returned) and Refuse (no MAC, the
// reply not sent):
// the reply MAC for a destination: the learned MAC, else the gateway MAC,
// else answered_mac (the source MAC of the received frame this reply
// answers, NULL for none), kept as the address's answered entry; a zero or
// multicast answered_mac is refused. With no frame in hand (answered_mac
// NULL), the address's answered entry. Else NULL with *refused incremented
// -- the reply is not sent.

static inline const uint8_t * rocketnet_xdp_reply_mac( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * destination, const uint8_t * answered_mac, uint64_t * refused )
{
    const struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( table, destination );
    if ( entry )
    {
        return entry->mac;
    }

    if ( destination->type == ROCKETNET_ADDRESS_IPV4 && table->gateway_ipv4_known )
    {
        return table->gateway_ipv4;
    }
    if ( destination->type == ROCKETNET_ADDRESS_IPV6 && table->gateway_ipv6_known )
    {
        return table->gateway_ipv6;
    }
    if ( table->gateway_known )
    {
        return table->gateway_mac;
    }

    if ( answered_mac )
    {
        if ( rocketnet_xdp_reply_mac_usable( answered_mac ) )
        {
            rocketnet_xdp_reply_macs_answer( table, destination, answered_mac );
            return answered_mac;
        }
    }
    else
    {
        entry = rocketnet_xdp_reply_macs_find_state( table, destination, ROCKETNET_XDP_REPLY_MAC_ANSWERED );
        if ( entry )
        {
            return entry->mac;
        }
    }

    if ( refused )
    {
        ( *refused )++;
    }
    return NULL;
}

// GATEWAY REFRESH: the table's gateway MACs are resolved again every
// ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS, so when the gateway's MAC changes
// while the server runs (a router replaced, a failover that moves the
// gateway address to another box) replies to unlearned addresses follow it
// within one period. The refresh runs on the main thread inside the receive
// call, where the table's other writes run: the fill threads that read the
// table through start and finish are joined before each receive and start
// again after it, so plain stores and that join publish the new MAC whole,
// as they publish every learned entry. The xdp threads never read the
// table; they send the copies start and finish wrote into each staged
// packet. A resolve that fails, finds no default route or yields an
// unusable MAC keeps what the table has: a refresh replaces a gateway or
// adds one, never removes one. proofs/tla/RNXdpGatewayRefresh.tla.

#define ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS   5.0

// one family's gateway MAC: false is a failed resolve, true with *found
// false is no default route for the family

typedef bool ( *rocketnet_xdp_gateway_resolve_t )( void * context, int address_type, uint8_t * mac, bool * found );

struct rocketnet_xdp_gateway_change_t
{
    int address_type;
    bool was_known;
    uint8_t was[6];
    uint8_t mac[6];
};

// serves_both: the per-family gateways, IPv4 and IPv6; else gateway_mac,
// resolved for address_type. Disabled while ROCKETNET_GATEWAY_ETHERNET_ADDRESS
// names the gateway.

struct rocketnet_xdp_gateway_refresh_t
{
    bool enabled;
    bool scheduled;
    bool serves_both;
    int address_type;
    double next_time;
};

// proofs/tla/RNXdpGatewayRefresh.tla Refresh and WriteLow: at or past the
// deadline, resolves each served family and writes every changed gateway
// MAC into the table, whole before the call returns, recording each change
// in changes (room for two). The first call only schedules: create has just
// resolved. Returns the number of changes.

static inline int rocketnet_xdp_gateway_refresh( struct rocketnet_xdp_gateway_refresh_t * refresh, struct rocketnet_xdp_reply_macs_t * table, double now,
                                                 rocketnet_xdp_gateway_resolve_t resolve, void * context, struct rocketnet_xdp_gateway_change_t * changes )
{
    if ( !refresh->enabled )
    {
        return 0;
    }

    if ( !refresh->scheduled )
    {
        refresh->scheduled = true;
        refresh->next_time = now + ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS;
        return 0;
    }

    if ( now < refresh->next_time )
    {
        return 0;
    }

    // tests/tla_trace_xdp_gateway_refresh.cpp plants a refresh that keeps
    // its old deadline, so every later call resolves again
#if !defined( ROCKETNET_XDP_GATEWAY_REFRESH_PLANT )
    refresh->next_time = now + ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS;
#endif // #if !defined( ROCKETNET_XDP_GATEWAY_REFRESH_PLANT )

    int num_changes = 0;
    const int num_families = refresh->serves_both ? 2 : 1;
    for ( int i = 0; i < num_families; ++i )
    {
        int address_type = refresh->address_type;
        uint8_t * slot = table->gateway_mac;
        bool * known = &table->gateway_known;
        if ( refresh->serves_both )
        {
            address_type = ( i == 0 ) ? ROCKETNET_ADDRESS_IPV4 : ROCKETNET_ADDRESS_IPV6;
            slot = ( i == 0 ) ? table->gateway_ipv4 : table->gateway_ipv6;
            known = ( i == 0 ) ? &table->gateway_ipv4_known : &table->gateway_ipv6_known;
        }

        uint8_t mac[6];
        bool found = false;
        if ( !resolve( context, address_type, mac, &found ) || !found || !rocketnet_xdp_reply_mac_usable( mac ) )
        {
            continue;
        }

        if ( *known && memcmp( slot, mac, 6 ) == 0 )
        {
            continue;
        }

        struct rocketnet_xdp_gateway_change_t * change = &changes[num_changes++];
        change->address_type = address_type;
        change->was_known = *known;
        memcpy( change->was, slot, 6 );
        memcpy( change->mac, mac, 6 );

        memcpy( slot, mac, 6 );
        *known = true;
    }

    return num_changes;
}

#endif // ROCKETNET_SDK_ROCKETNET_XDP_FRAME_H

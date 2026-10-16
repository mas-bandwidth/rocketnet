/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP frame codec: writes and reads Ethernet + IPv4/IPv6 + UDP at
// fixed byte offsets. It is plain portable C++ with no Linux headers, no
// libbpf and no allocation, so the unit suite builds it on every platform.
//
// IPv4 is byte-identical to the server's historical builder (ihl 5, the DF
// bit, ttl 64, header checksum, UDP checksum 0). IPv6 carries the mandatory
// UDP checksum computed in software over the RFC 8200 section 8.1
// pseudo-header (RFC 768, RFC 1071); a computed 0x0000 is sent as 0xFFFF.

#ifndef ROCKETNET_XDP_FRAME_H
#define ROCKETNET_XDP_FRAME_H

#include "rocketnet_address.h"
#include "rocketnet_wire.h"
#include "rocketnet_constants.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ROCKETNET_ETHERTYPE_IP      0x0800
#define ROCKETNET_ETHERTYPE_IPV6    0x86DD
#define ROCKETNET_UDP_PROTOCOL      17
#define ROCKETNET_IPV4_VERSION_IHL  0x45
#define ROCKETNET_IPV6_VERSION       0x60

// the frame header length per family: ethernet + (IPv4 or IPv6) + UDP.
// the flush copies a payload to this offset before the build writes the
// headers in front of it.

static inline int rocketnet_xdp_frame_header_bytes( int address_type )
{
    return ( address_type == ROCKETNET_ADDRESS_IPV4 )
        ? ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES
        : ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
}

// ones'-complement checksum over big-endian 16-bit words (RFC 1071).

static inline uint16_t rocketnet_xdp_frame_checksum( const uint8_t * data, size_t bytes )
{
    uint32_t sum = 0;
    while ( bytes >= 2 )
    {
        sum += ( (uint16_t) data[0] << 8 ) | data[1];
        data += 2;
        bytes -= 2;
    }
    if ( bytes )
        sum += ( (uint16_t) data[0] << 8 );
    while ( sum >> 16 )
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
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
        sum += ( (uint16_t) src[i] << 8 ) | src[i + 1];
    for ( i = 0; i < 16; i += 2 )
        sum += ( (uint16_t) dst[i] << 8 ) | dst[i + 1];
    sum += (uint16_t) ( udp_length >> 16 );
    sum += (uint16_t) ( udp_length & 0xFFFF );
    sum += 0x0011; // three zero bytes then next header 17

    for ( i = 0; i < 8; i += 2 )
        sum += ( (uint16_t) udp_header[i] << 8 ) | udp_header[i + 1];

    for ( i = 0; i + 1 < payload_bytes; i += 2 )
        sum += ( (uint16_t) udp_header[8 + i] << 8 ) | udp_header[9 + i];
    if ( payload_bytes & 1 )
        sum += ( (uint16_t) udp_header[8 + payload_bytes - 1] << 8 );

    while ( sum >> 16 )
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    uint16_t result = (uint16_t) ~sum;
    if ( result == 0 )
        result = 0xFFFF;
    return result;
}

// writes the IPv4 wire bytes for a host-order uint16_t group (the rocketnet
// address stores IPv6 words in host order).

static inline void rocketnet_xdp_frame_write_be16( uint8_t * out, uint16_t value )
{
    out[0] = (uint8_t) ( value >> 8 );
    out[1] = (uint8_t) ( value & 0xFF );
}

// BUILD: writes ethernet + IP + UDP in front of a payload already at the
// family's header offset. Returns the frame length, or 0 for a refusal.

static inline int rocketnet_xdp_frame_build(
    void * frame, size_t frame_bytes,
    const uint8_t * server_mac, const uint8_t * gateway_mac,
    const rocketnet_address_t * source, const rocketnet_address_t * destination,
    int payload_bytes )
{
    uint8_t * f = (uint8_t*) frame;
    int payload_index;
    int total;

    if ( !frame || !server_mac || !gateway_mac || !source || !destination )
        return 0;
    if ( source->type != destination->type )
        return 0;
    if ( payload_bytes < 0 )
        return 0;

    const int ipv6 = ( source->type == ROCKETNET_ADDRESS_IPV6 );
    const int header_bytes = rocketnet_xdp_frame_header_bytes( source->type );
    const int max_payload = ipv6
        ? ROCKETNET_MAX_PACKET_BYTES_IPV6
        : ROCKETNET_MAX_PACKET_BYTES;
    if ( payload_bytes > max_payload )
        return 0;
    if ( (size_t) ( header_bytes + payload_bytes ) > frame_bytes )
        return 0;

    memcpy( f, gateway_mac, 6 );
    memcpy( f + 6, server_mac, 6 );
    uint16_t ethertype = ipv6 ? ROCKETNET_ETHERTYPE_IPV6 : ROCKETNET_ETHERTYPE_IP;

    payload_index = header_bytes;
    total = header_bytes + payload_bytes;

    if ( ipv6 )
    {
        rocketnet_xdp_frame_write_be16( f + 12, ethertype );
        f[14] = ROCKETNET_IPV6_VERSION;
        f[15] = 0;
        f[16] = 0;
        f[17] = 0;
        rocketnet_xdp_frame_write_be16( f + 18, (uint16_t) ( ROCKETNET_UDP_HEADER_BYTES + payload_bytes ) );
        f[20] = ROCKETNET_UDP_PROTOCOL;
        f[21] = 64;
        for ( int i = 0; i < 8; ++i )
            rocketnet_xdp_frame_write_be16( f + 22 + 2 * i, source->data.ipv6[i] );
        for ( int i = 0; i < 8; ++i )
            rocketnet_xdp_frame_write_be16( f + 38 + 2 * i, destination->data.ipv6[i] );
    }
    else
    {
        rocketnet_xdp_frame_write_be16( f + 12, ethertype );
        f[14] = ROCKETNET_IPV4_VERSION_IHL;
        f[15] = 0x00;
        rocketnet_xdp_frame_write_be16( f + 16, (uint16_t) ( ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES + payload_bytes ) );
        f[18] = 0;
        f[19] = 0;
        f[20] = 0x40;
        f[21] = 0x00;
        f[22] = 64;
        f[23] = ROCKETNET_UDP_PROTOCOL;
        f[24] = 0;
        f[25] = 0;
        f[26] = source->data.ipv4[0];
        f[27] = source->data.ipv4[1];
        f[28] = source->data.ipv4[2];
        f[29] = source->data.ipv4[3];
        f[30] = destination->data.ipv4[0];
        f[31] = destination->data.ipv4[1];
        f[32] = destination->data.ipv4[2];
        f[33] = destination->data.ipv4[3];
        uint16_t ip_check = rocketnet_xdp_frame_checksum( f + 14, ROCKETNET_IPV4_HEADER_BYTES );
        rocketnet_xdp_frame_write_be16( f + 24, ip_check );
    }

    // UDP header
    rocketnet_xdp_frame_write_be16( f + payload_index - 8, source->port );
    rocketnet_xdp_frame_write_be16( f + payload_index - 6, destination->port );
    rocketnet_xdp_frame_write_be16( f + payload_index - 4, (uint16_t) ( ROCKETNET_UDP_HEADER_BYTES + payload_bytes ) );
    if ( ipv6 )
    {
        uint16_t udp_check = rocketnet_xdp_frame_ipv6_udp_checksum(
            f + 22, f + 38, ROCKETNET_UDP_HEADER_BYTES + payload_bytes,
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

// PARSE: reads ethernet + IP + UDP. Returns false for anything unrecognized,
// extension headers included. Fills the source address and a pointer/length
// into the payload bytes (inside frame).

static inline bool rocketnet_xdp_frame_parse(
    const void * frame, size_t frame_bytes,
    rocketnet_address_t * out_from,
    const uint8_t ** out_payload, int * out_payload_length )
{
    const uint8_t * f = (const uint8_t*) frame;
    int header_bytes;
    int payload_bytes;

    if ( !frame || !out_from || !out_payload || !out_payload_length )
        return false;
    if ( frame_bytes < (size_t) ( ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES ) )
        return false;
    if ( f[0] == 0xFF && f[1] == 0xFF )
        return false;

    uint16_t ethertype = ( (uint16_t) f[12] << 8 ) | f[13];
    memset( out_from, 0, sizeof( *out_from ) );

    if ( ethertype == ROCKETNET_ETHERTYPE_IP )
    {
        if ( f[14] != ROCKETNET_IPV4_VERSION_IHL )
            return false;
        if ( f[23] != ROCKETNET_UDP_PROTOCOL )
            return false;
        header_bytes = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
        out_from->type = ROCKETNET_ADDRESS_IPV4;
        out_from->data.ipv4[0] = f[26];
        out_from->data.ipv4[1] = f[27];
        out_from->data.ipv4[2] = f[28];
        out_from->data.ipv4[3] = f[29];
        out_from->port = ( (uint16_t) f[header_bytes - 8] << 8 ) | f[header_bytes - 7];
    }
    else if ( ethertype == ROCKETNET_ETHERTYPE_IPV6 )
    {
        if ( f[14] != ROCKETNET_IPV6_VERSION || f[15] != 0 || f[16] != 0 || f[17] != 0 )
            return false;
        if ( f[20] != ROCKETNET_UDP_PROTOCOL )
            return false;
        header_bytes = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
        out_from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
            out_from->data.ipv6[i] = ( (uint16_t) f[22 + 2 * i] << 8 ) | f[23 + 2 * i];
        out_from->port = ( (uint16_t) f[header_bytes - 8] << 8 ) | f[header_bytes - 7];
    }
    else
    {
        return false;
    }

    if ( frame_bytes < (size_t) header_bytes )
        return false;
    payload_bytes = (int) ( frame_bytes - header_bytes );
    *out_payload = f + header_bytes;
    *out_payload_length = payload_bytes;
    return true;
}

#endif // ROCKETNET_XDP_FRAME_H

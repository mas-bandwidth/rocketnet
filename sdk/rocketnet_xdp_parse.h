/* RocketNet. Copyright 2026 Mas Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once
#ifndef ROCKETNET_XDP_PARSE_H
#define ROCKETNET_XDP_PARSE_H

#include "rocketnet_wire.h"
#include <stdint.h>

// The per-packet checks shared by the XDP program (compiled as BPF) and
// host tests. Only preprocessor constants and byte accesses live here, so
// the one file serves both compilers and the two can never drift apart.
//
// Verdicts for the payload checks equal the XDP counter index they feed,
// so the kernel program counts a refusal with server_xdp_count(verdict).
// PASS and DROP_HEADER carry no counter: PASS is traffic for another
// destination, and DROP_HEADER is a structurally impossible frame whose
// destination port was never established.

#define ROCKETNET_XDP_VERDICT_REDIRECT      ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED
#define ROCKETNET_XDP_VERDICT_TOO_SMALL     ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL
#define ROCKETNET_XDP_VERDICT_TOO_LARGE     ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE
#define ROCKETNET_XDP_VERDICT_WRONG_FILTER  ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER
#define ROCKETNET_XDP_VERDICT_INVALID_TYPE  ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE
#define ROCKETNET_XDP_VERDICT_WRONG_EPOCH   ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH
#define ROCKETNET_XDP_VERDICT_PASS          100
#define ROCKETNET_XDP_VERDICT_DROP_HEADER   101

// The extension-header walk bound: at most this many extension headers are
// walked before the frame is refused, so the walk always terminates.
#define ROCKETNET_XDP_MAX_EXTENSION_HEADERS 4

// Accepted extension headers are Hop-by-Hop (0), Routing (43) and
// Destination Options (60). A Fragment header (44) is refused, matching
// the IPv4 strictness that only ihl == 5 (no IP options, no fragment
// offset handling in the filter) is accepted. Any other next-header value
// that is not UDP is not traffic for this program and passes.

struct rocketnet_xdp_parse_config_t
{
    uint16_t public_port;       // big endian; 0 means unconfigured
    uint32_t public_address;    // big endian IPv4; 0 accepts any IPv4 destination
    uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES];
    uint8_t filter_epoch;
};

struct rocketnet_xdp_parse_result_t
{
    int verdict;
    int payload_offset;         // offset of the UDP payload in the frame
    int payload_bytes;
    uint16_t source_port;       // big endian
    uint8_t source_address[16]; // IPv4 address in the first 4 bytes for IPv4
    int address_family;         // 4 or 6; 0 when no address was established
};

// The checks after the UDP header: the wire size bounds, the filter epoch
// and value, and the stateless structural check. payload_bytes is the UDP
// payload length. The caller guarantees payload holds payload_bytes bytes.
static inline int rocketnet_xdp_check_payload( const uint8_t * payload, int payload_bytes, const struct rocketnet_xdp_parse_config_t * config )
{
    if ( payload_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
        return ROCKETNET_XDP_VERDICT_TOO_SMALL;
    if ( payload_bytes > ROCKETNET_RECEIVE_CEILING )
        return ROCKETNET_XDP_VERDICT_TOO_LARGE;
    if ( payload[1] != config->filter_epoch )
        return ROCKETNET_XDP_VERDICT_WRONG_EPOCH;
    if ( payload[2] != config->filter_value[0] ||
         payload[3] != config->filter_value[1] ||
         payload[4] != config->filter_value[2] ||
         payload[5] != config->filter_value[3] )
        return ROCKETNET_XDP_VERDICT_WRONG_FILTER;
    if ( !ROCKETNET_WIRE_SERVER_PACKET_VALID( payload[0], payload_bytes ) )
        return ROCKETNET_XDP_VERDICT_INVALID_TYPE;
    return ROCKETNET_XDP_VERDICT_REDIRECT;
}

static inline uint16_t rocketnet_xdp_read_u16( const uint8_t * p )
{
    return (uint16_t) ( ( (uint16_t) p[0] << 8 ) | (uint16_t) p[1] );
}

// Parses one Ethernet frame. frame holds frame_bytes bytes. On REDIRECT
// the result locates the UDP payload and the source address and port.
static inline int rocketnet_xdp_parse_frame( const uint8_t * frame, int frame_bytes, const struct rocketnet_xdp_parse_config_t * config, struct rocketnet_xdp_parse_result_t * result )
{
    result->verdict = ROCKETNET_XDP_VERDICT_PASS;
    result->payload_offset = 0;
    result->payload_bytes = 0;
    result->source_port = 0;
    result->address_family = 0;
    for ( int i = 0; i < 16; i++ )
        result->source_address[i] = 0;

    if ( frame_bytes < 14 )
    {
        result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
        return result->verdict;
    }

    const uint16_t ethertype = rocketnet_xdp_read_u16( frame + 12 );

    if ( ethertype == 0x0800 ) // IPv4
    {
        if ( frame_bytes < 14 + 20 )
        {
            result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
            return result->verdict;
        }
        const uint8_t * ip = frame + 14;
        if ( ip[0] != 0x45 ) // version 4, ihl == 5 only
            return result->verdict;
        if ( ip[9] != 17 ) // UDP only
            return result->verdict;
        if ( frame_bytes < 14 + 20 + 8 )
        {
            result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
            return result->verdict;
        }
        const uint8_t * udp = ip + 20;
        // public_port is stored big endian, so the wire bytes compare directly.
        const uint8_t * want_port = (const uint8_t *) &config->public_port;
        if ( config->public_port == 0 || udp[2] != want_port[0] || udp[3] != want_port[1] )
            return result->verdict;
        if ( config->public_address != 0 )
        {
            const uint32_t dest = ( (uint32_t) ip[16] << 24 ) | ( (uint32_t) ip[17] << 16 ) | ( (uint32_t) ip[18] << 8 ) | (uint32_t) ip[19];
            const uint32_t want = ( (uint32_t) ( (const uint8_t *) &config->public_address )[0] << 24 ) | ( (uint32_t) ( (const uint8_t *) &config->public_address )[1] << 16 ) | ( (uint32_t) ( (const uint8_t *) &config->public_address )[2] << 8 ) | (uint32_t) ( (const uint8_t *) &config->public_address )[3];
            // public_address is stored big endian, so a byte-wise compare
            // against the wire destination is the address compare.
            (void) dest; (void) want;
            if ( ip[16] != ((const uint8_t *) &config->public_address)[0] ||
                 ip[17] != ((const uint8_t *) &config->public_address)[1] ||
                 ip[18] != ((const uint8_t *) &config->public_address)[2] ||
                 ip[19] != ((const uint8_t *) &config->public_address)[3] )
                return result->verdict;
        }
        result->address_family = 4;
        for ( int i = 0; i < 4; i++ )
            result->source_address[i] = ip[12 + i];
        { const uint8_t * sp = udp; uint8_t * dp = (uint8_t *) &result->source_port; dp[0] = sp[0]; dp[1] = sp[1]; }
        result->payload_offset = 14 + 20 + 8;
        result->payload_bytes = frame_bytes - result->payload_offset;
        result->verdict = rocketnet_xdp_check_payload( frame + result->payload_offset, result->payload_bytes, config );
        return result->verdict;
    }

    if ( ethertype == 0x86dd ) // IPv6
    {
        if ( frame_bytes < 14 + 40 )
        {
            result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
            return result->verdict;
        }
        const uint8_t * ip6 = frame + 14;
        if ( ( ip6[0] & 0xf0 ) != 0x60 )
        {
            result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
            return result->verdict;
        }
        // No IPv6 public address is configured in this revision (the config
        // map holds an IPv4 address), so any destination address is
        // accepted for IPv6 and the destination port alone selects.
        uint8_t next = ip6[6];
        int offset = 14 + 40;
        int walked = 0;
        while ( next != 17 ) // UDP
        {
            if ( next == 44 ) // Fragment: refused, matching IPv4 ihl == 5 strictness
            {
                result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
                return result->verdict;
            }
            if ( next != 0 && next != 43 && next != 60 )
                return result->verdict; // not UDP traffic for this program
            if ( walked >= ROCKETNET_XDP_MAX_EXTENSION_HEADERS )
            {
                result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
                return result->verdict;
            }
            if ( offset + 8 > frame_bytes )
            {
                result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
                return result->verdict;
            }
            const int ext_bytes = ( (int) frame[offset + 1] + 1 ) * 8;
            if ( ext_bytes < 8 || offset + ext_bytes > frame_bytes )
            {
                result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
                return result->verdict;
            }
            next = frame[offset];
            offset += ext_bytes;
            walked++;
        }
        if ( offset + 8 > frame_bytes )
        {
            result->verdict = ROCKETNET_XDP_VERDICT_DROP_HEADER;
            return result->verdict;
        }
        const uint8_t * udp = frame + offset;
        const uint8_t * want_port6 = (const uint8_t *) &config->public_port;
        if ( config->public_port == 0 || udp[2] != want_port6[0] || udp[3] != want_port6[1] )
            return result->verdict;
        result->address_family = 6;
        for ( int i = 0; i < 16; i++ )
            result->source_address[i] = ip6[8 + i];
        { const uint8_t * sp = udp; uint8_t * dp = (uint8_t *) &result->source_port; dp[0] = sp[0]; dp[1] = sp[1]; }
        result->payload_offset = offset + 8;
        result->payload_bytes = frame_bytes - result->payload_offset;
        result->verdict = rocketnet_xdp_check_payload( frame + result->payload_offset, result->payload_bytes, config );
        return result->verdict;
    }

    return result->verdict;
}

#endif // #ifndef ROCKETNET_XDP_PARSE_H

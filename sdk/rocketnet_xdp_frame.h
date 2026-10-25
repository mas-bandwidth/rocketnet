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

// MAC PARSE: one strict link-layer address parse. Accepts exactly 17
// characters after a single trailing newline is stripped: six hex pairs
// separated by ':' or '.'. Returns false, leaving mac untouched, for
// anything else.

static inline int rocketnet_xdp_mac_hex_digit( char c )
{
    if ( c >= '0' && c <= '9' )
        return c - '0';
    if ( c >= 'a' && c <= 'f' )
        return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' )
        return c - 'A' + 10;
    return -1;
}

static inline bool rocketnet_xdp_parse_mac_address( const char * text, uint8_t mac[6] )
{
    if ( !text || !mac )
        return false;

    size_t length = strlen( text );
    if ( length > 0 && text[length - 1] == '\n' )
        length--;
    if ( length != 17 )
        return false;

    for ( size_t i = 0; i < length; ++i )
    {
        if ( ( i % 3 ) == 2 )
        {
            if ( text[i] != ':' && text[i] != '.' )
                return false;
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
    memcpy( mac, parsed, sizeof(parsed) );
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
        sum += ( (uint32_t) data[0] << 8 );
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
        sum += ( (uint32_t) src[i] << 8 ) | src[i + 1];
    for ( i = 0; i < 16; i += 2 )
        sum += ( (uint32_t) dst[i] << 8 ) | dst[i + 1];
    sum += (uint16_t) ( udp_length >> 16 );
    sum += (uint16_t) ( udp_length & 0xFFFF );
    sum += 0x0011; // three zero bytes then next header 17

    for ( i = 0; i < 8; i += 2 )
        sum += ( (uint32_t) udp_header[i] << 8 ) | udp_header[i + 1];

    for ( i = 0; i + 1 < payload_bytes; i += 2 )
        sum += ( (uint32_t) udp_header[8 + i] << 8 ) | udp_header[9 + i];
    if ( payload_bytes & 1 )
        sum += ( (uint32_t) udp_header[8 + payload_bytes - 1] << 8 );

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
        ? ROCKETNET_MAX_PACKET_BYTES
        : ROCKETNET_RECEIVE_PACKET_BYTES;
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
        // the checksum sums the UDP header with its checksum field zero: a
        // reused frame still holds the previous datagram's checksum there
        f[payload_index - 2] = 0;
        f[payload_index - 1] = 0;
        uint16_t udp_check = rocketnet_xdp_frame_ipv6_udp_checksum(
            f + 22, f + 38, (uint32_t) ( ROCKETNET_UDP_HEADER_BYTES + payload_bytes ),
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
// extension headers included; an IPv6 header with any traffic class and flow
// label is read. Fills the source address and a pointer/length
// into the payload bytes (inside frame).
// Length guards: the up-front check proves the frame holds at least the IPv4
// minimum (ethernet + IPv4 + UDP), enough to name the ethertype at bytes
// 12-13 and the version at byte 14; each family branch then proves the frame
// holds that family's own ethernet + IP + UDP header before it reads any byte
// past the ethernet header, so a truncated IPv6 frame is refused before its
// source address or UDP port is read.

static inline bool rocketnet_xdp_frame_parse(
    const void * frame, size_t frame_bytes,
    rocketnet_address_t * out_from,
    const uint8_t ** out_payload, int * out_payload_length )
{
    const uint8_t * f = (const uint8_t*) frame;
    int header_bytes;
    int payload_bytes;
    uint16_t udp_len;

    if ( !frame || !out_from || !out_payload || !out_payload_length )
        return false;
    if ( frame_bytes < (size_t) ( ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES ) )
        return false;
    if ( f[0] == 0xFF && f[1] == 0xFF )
        return false;

    uint16_t ethertype = (uint16_t) ( ( f[12] << 8 ) | f[13] );
    memset( out_from, 0, sizeof( *out_from ) );

    if ( ethertype == ROCKETNET_ETHERTYPE_IP )
    {
        if ( f[14] != ROCKETNET_IPV4_VERSION_IHL )
            return false;
        if ( f[23] != ROCKETNET_UDP_PROTOCOL )
            return false;
        header_bytes = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
        if ( frame_bytes < (size_t) header_bytes )
            return false;

        udp_len = (uint16_t) ( ( f[header_bytes - 4] << 8 ) | f[header_bytes - 3] );
        if ( udp_len < ROCKETNET_UDP_HEADER_BYTES )
            return false;

        out_from->type = ROCKETNET_ADDRESS_IPV4;
        out_from->data.ipv4[0] = f[26];
        out_from->data.ipv4[1] = f[27];
        out_from->data.ipv4[2] = f[28];
        out_from->data.ipv4[3] = f[29];
        out_from->port = (uint16_t) ( ( f[header_bytes - 8] << 8 ) | f[header_bytes - 7] );
    }
    else if ( ethertype == ROCKETNET_ETHERTYPE_IPV6 )
    {
        // the version nibble only: the traffic class and the 20-bit flow
        // label carry any value (a Linux sender sets a flow label by default)
        if ( ( f[14] & 0xF0 ) != ROCKETNET_IPV6_VERSION )
            return false;
        if ( f[20] != ROCKETNET_UDP_PROTOCOL )
            return false;
        header_bytes = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES;
        if ( frame_bytes < (size_t) header_bytes )
            return false;

        udp_len = (uint16_t) ( ( f[header_bytes - 4] << 8 ) | f[header_bytes - 3] );
        if ( udp_len < ROCKETNET_UDP_HEADER_BYTES )
            return false;

        out_from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
            out_from->data.ipv6[i] = (uint16_t) ( ( f[22 + 2 * i] << 8 ) | f[23 + 2 * i] );
        out_from->port = (uint16_t) ( ( f[header_bytes - 8] << 8 ) | f[header_bytes - 7] );
    }
    else
    {
        return false;
    }

    // header_bytes and udp_len are carried from the family branch, which
    // already proved the frame holds the full header and read the UDP length.
    // Bytes past the UDP length (link-layer padding) are not payload; a UDP
    // length past the frame end is refused, as server_xdp_udp drops it

    payload_bytes = (int) ( frame_bytes - (size_t) header_bytes );
    if ( (int) udp_len - ROCKETNET_UDP_HEADER_BYTES > payload_bytes )
        return false;
    payload_bytes = (int) udp_len - ROCKETNET_UDP_HEADER_BYTES;

    *out_payload = f + header_bytes;
    *out_payload_length = payload_bytes;
    return true;
}

// RECEIVE: the AF_XDP receive loop's per-frame dispatch, for a server that
// serves address_type (ROCKETNET_ADDRESS_IPV4 or ROCKETNET_ADDRESS_IPV6). It
// dispatches on the ethertype: IPv4 at its offsets, IPv6 at the 40-byte fixed
// header, through the parse above, and the from-address carries the family
// it read. A frame is taken only when it holds the smallest server-inbound
// packet. Every frame it does not take is dropped and counted by name:
//
//   not_ip          the ethertype is neither IPv4 nor IPv6, or the frame is
//                   too short to name one
//   ipv6_extension  an IPv6 frame whose next header is an extension header
//                   (hop-by-hop, routing, fragment, destination options, AH,
//                   ESP): never parsed at the UDP offsets
//   other_family    a frame of the family this server does not serve
//   malformed       an IPv4 or IPv6 frame the parse refuses (IPv4 options, a
//                   next header that is not UDP) or one too short for the
//                   smallest server packet
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

static inline bool rocketnet_xdp_frame_ipv6_is_extension( uint8_t next_header )
{
    return next_header == 0  ||     // hop-by-hop options
           next_header == 43 ||     // routing
           next_header == 44 ||     // fragment
           next_header == 60 ||     // destination options
           next_header == 51 ||     // AH
           next_header == 50;       // ESP
}

static inline bool rocketnet_xdp_frame_receive(
    const void * frame, size_t frame_bytes, int address_type,
    rocketnet_address_t * out_from,
    const uint8_t ** out_payload, int * out_payload_length,
    struct rocketnet_xdp_receive_drops_t * drops )
{
    const uint8_t * f = (const uint8_t*) frame;

    if ( !frame || !out_from || !out_payload || !out_payload_length || !drops )
        return false;

    if ( frame_bytes < (size_t) ROCKETNET_ETHERNET_HEADER_BYTES )
    {
        drops->not_ip++;
        return false;
    }

    const uint16_t ethertype = (uint16_t) ( ( f[12] << 8 ) | f[13] );

    int family;
    if ( ethertype == ROCKETNET_ETHERTYPE_IP )
    {
        family = ROCKETNET_ADDRESS_IPV4;
    }
    else if ( ethertype == ROCKETNET_ETHERTYPE_IPV6 )
    {
        family = ROCKETNET_ADDRESS_IPV6;
        if ( frame_bytes >= (size_t) ( ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES ) && rocketnet_xdp_frame_ipv6_is_extension( f[20] ) )
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

    // address_type below zero accepts either family (ServedFamilies of two).
    if ( address_type >= 0 && family != address_type )
    {
        drops->other_family++;
        return false;
    }

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    if ( !rocketnet_xdp_frame_parse( frame, frame_bytes, &from, &payload, &payload_bytes ) || payload_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
    {
        drops->malformed++;
        return false;
    }

    *out_from = from;
    *out_payload = payload;
    *out_payload_length = payload_bytes;
    return true;
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
// returned. Else it returns the payload length.

static inline int rocketnet_xdp_send_copy(
    uint8_t * frame, size_t frame_bytes, int address_type,
    const uint8_t * packet, size_t packet_bytes, uint64_t * refused )
{
    const int header_bytes = rocketnet_xdp_frame_header_bytes( address_type );
    if ( !frame || !packet || packet_bytes == 0 || packet_bytes > (size_t) ROCKETNET_RECEIVE_CEILING ||
         (size_t) header_bytes > frame_bytes || packet_bytes > frame_bytes - (size_t) header_bytes )
    {
        if ( refused )
            ( *refused )++;
        return -1;
    }
    memcpy( frame + header_bytes, packet, packet_bytes );
    return (int) packet_bytes;
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
            drops->descriptor++;
        return NULL;
    }
    return (const uint8_t*) umem + addr;
}

// FILL RETURN: whether a receive descriptor's frame goes back to the fill
// ring. Only a frame of the receive region [region_begin, region_end) is
// the receive path's to hand back; a descriptor naming any other frame (a
// send-region frame, which only a driver or kernel defect delivers) is
// never returned, so a frame on the send free list is never received into.
// On true *frame_addr is the address of the frame that holds addr (a
// received address carries the headroom offset into its frame). Counting
// a refused descriptor is rocketnet_xdp_receive_descriptor's, not this
// rule's. tla/RNXdpReceiveRing.tla: Take, FillHoldsOnlyReceiveFrames.

static inline bool rocketnet_xdp_fill_return( uint64_t addr, uint64_t region_begin, uint64_t region_end, uint64_t * frame_addr )
{
    if ( !frame_addr || addr < region_begin || addr >= region_end )
        return false;
    *frame_addr = addr - addr % ROCKETNET_XDP_FRAME_SIZE;
    return true;
}

// RECEIVE DRAIN: the queue a receive call visits at this step. start is the
// first queue of the call and step is how far around the ring it has walked.

static inline int rocketnet_xdp_drain_queue( int start, int step, int num_queues )
{
    if ( num_queues <= 0 )
        return 0;
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
        return false;

    int value = 0;
    while ( *p >= '0' && *p <= '9' )
    {
        value = value * 10 + ( *p - '0' );
        if ( value > 0x7FFFFFFF )
            return false;
        p++;
    }
    *first = value;
    *last = value;

    if ( *p == '-' )
    {
        p++;
        if ( *p < '0' || *p > '9' )
            return false;
        value = 0;
        while ( *p >= '0' && *p <= '9' )
        {
            value = value * 10 + ( *p - '0' );
            if ( value > 0x7FFFFFFF )
                return false;
            p++;
        }
        *last = value;
    }

    if ( *last < *first )
        return false;

    if ( *p == ',' )
    {
        *cursor = p + 1;
        return true;
    }

    while ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' )
        p++;

    if ( *p != '\0' )
        return false;

    *cursor = p;
    return true;
}

// PIN TARGETS: every cpu this transport will pin to, num_os_cpus ..
// num_os_cpus + num_queues - 1, is in the kernel's isolated cpu-list
// ("1-7", "1,3,5-6", a trailing newline). A malformed or empty list never
// verifies. first_missing names the first target not in the list, or -1
// when every target is there.

static inline bool rocketnet_xdp_pin_targets_isolated( const char * isolated_list, int num_os_cpus, int num_queues, int * first_missing )
{
    if ( first_missing )
        *first_missing = -1;

    if ( !isolated_list || isolated_list[0] == '\0' || num_os_cpus < 0 || num_queues < 0 ||
         num_os_cpus > 0x7FFFFFFF - num_queues )
    {
        if ( first_missing )
            *first_missing = num_os_cpus > 0 ? num_os_cpus : 0;
        return false;
    }

    const char * cursor = isolated_list;
    while ( *cursor == ' ' || *cursor == '\t' )
        cursor++;

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
                    *first_missing = num_os_cpus > 0 ? num_os_cpus : 0;
                return false;
            }
            if ( *check == '\0' )
                break;
        }
    }

    for ( int cpu = num_os_cpus; cpu < num_os_cpus + num_queues; cpu++ )
    {
        bool found = false;
        const char * scan = cursor;
        while ( true )
        {
            int first = 0;
            int last = 0;
            if ( !rocketnet_xdp_cpu_list_entry( &scan, &first, &last ) )
                break; // validated above
            if ( cpu >= first && cpu <= last )
            {
                found = true;
                break;
            }
            if ( *scan == '\0' )
                break;
        }
        if ( !found )
        {
            if ( first_missing )
                *first_missing = cpu;
            return false;
        }
    }

    return true;
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
//
// The table is fixed-size and lives inside its owner's create-time
// allocation: open addressing over ROCKETNET_XDP_REPLY_MAC_ENTRIES slots,
// a lookup stopping at the first never-used slot and probing at most
// ROCKETNET_XDP_REPLY_MAC_PROBES slots. A learn into a full probe window
// replaces the entry learned longest ago; a forget leaves a tombstone. No
// allocation, no clock and no lock: its owner serialises writes.

// The table holds a learned MAC for every client at most a quarter full:
// four slots per client, rounded up to a power of two for the index mask
// (4096 at 1000 clients).

#define ROCKETNET_XDP_REPLY_MAC_ENTRIES     ROCKETNET_POWER_OF_TWO_AT_LEAST( 4 * ROCKETNET_MAX_CLIENTS )
#define ROCKETNET_XDP_REPLY_MAC_PROBES      16

ROCKETNET_STATIC_ASSERT( ROCKETNET_XDP_REPLY_MAC_ENTRIES >= 4 * ROCKETNET_MAX_CLIENTS,
                         "the reply MAC table holds every client's MAC at most a quarter full" );
ROCKETNET_STATIC_ASSERT( ( ROCKETNET_XDP_REPLY_MAC_ENTRIES & ( ROCKETNET_XDP_REPLY_MAC_ENTRIES - 1 ) ) == 0,
                         "the reply MAC table size is a power of two for its index mask" );

#define ROCKETNET_XDP_REPLY_MAC_EMPTY       0
#define ROCKETNET_XDP_REPLY_MAC_LIVE        1
#define ROCKETNET_XDP_REPLY_MAC_TOMBSTONE   2
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
    struct rocketnet_xdp_reply_mac_entry_t entries[ROCKETNET_XDP_REPLY_MAC_ENTRIES];
    // Per-family gateways. Zero after reset, so a one-family table still uses gateway_mac.
    bool gateway_ipv4_known;
    bool gateway_ipv6_known;
    uint8_t gateway_ipv4[6];
    uint8_t gateway_ipv6[6];
};

// gateway_mac NULL: no gateway is known for this interface.

static inline void rocketnet_xdp_reply_macs_reset( struct rocketnet_xdp_reply_macs_t * table, const uint8_t * gateway_mac )
{
    memset( table, 0, sizeof( *table ) );
    if ( gateway_mac )
    {
        memcpy( table->gateway_mac, gateway_mac, 6 );
        table->gateway_known = true;
    }
}

static inline uint32_t rocketnet_xdp_reply_macs_slot( const rocketnet_address_t * address )
{
    // FNV-1a over the family, the port and the address bytes the family uses
    uint32_t hash = 2166136261u;
    const int bytes = ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? 16 : 4;
    const uint8_t * data = (const uint8_t*) &address->data;
    hash = ( hash ^ address->type ) * 16777619u;
    hash = ( hash ^ (uint8_t) ( address->port & 0xFF ) ) * 16777619u;
    hash = ( hash ^ (uint8_t) ( address->port >> 8 ) ) * 16777619u;
    for ( int i = 0; i < bytes; ++i )
        hash = ( hash ^ data[i] ) * 16777619u;
    return hash & ( ROCKETNET_XDP_REPLY_MAC_ENTRIES - 1 );
}

static inline struct rocketnet_xdp_reply_mac_entry_t * rocketnet_xdp_reply_macs_find_state( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address, uint8_t state )
{
    const uint32_t slot = rocketnet_xdp_reply_macs_slot( address );
    for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; ++i )
    {
        struct rocketnet_xdp_reply_mac_entry_t * entry = &table->entries[( slot + (uint32_t) i ) & ( ROCKETNET_XDP_REPLY_MAC_ENTRIES - 1 )];
        if ( entry->state == ROCKETNET_XDP_REPLY_MAC_EMPTY )
            return NULL;
        if ( entry->state == state && rocketnet_address_equal( &entry->address, address ) )
            return entry;
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

// a unicast, nonzero MAC is learned; anything else is ignored. The address's
// answered entry, if any, becomes its learned entry.

static inline void rocketnet_xdp_reply_macs_learn( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address, const uint8_t * mac )
{
    if ( !table || !address || !rocketnet_xdp_reply_mac_usable( mac ) )
        return;

    table->learn_count++;

    struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( table, address );
    if ( !entry )
        entry = rocketnet_xdp_reply_macs_find_state( table, address, ROCKETNET_XDP_REPLY_MAC_ANSWERED );
    if ( !entry )
    {
        const uint32_t slot = rocketnet_xdp_reply_macs_slot( address );
        struct rocketnet_xdp_reply_mac_entry_t * oldest = NULL;
        for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; ++i )
        {
            struct rocketnet_xdp_reply_mac_entry_t * candidate = &table->entries[( slot + (uint32_t) i ) & ( ROCKETNET_XDP_REPLY_MAC_ENTRIES - 1 )];
            if ( candidate->state != ROCKETNET_XDP_REPLY_MAC_LIVE )
            {
                entry = candidate;
                break;
            }
            if ( !oldest || candidate->learned < oldest->learned )
                oldest = candidate;
        }
        if ( !entry )
            entry = oldest;
        entry->address = *address;
    }

    entry->state = ROCKETNET_XDP_REPLY_MAC_LIVE;
    memcpy( entry->mac, mac, 6 );
    entry->learned = table->learn_count;
}

// keeps mac as the address's answered entry: in its own answered entry, else
// a never-used or forgotten slot, else the oldest answered entry of its
// probe window. A learned entry is never displaced; with no room the answer
// is not kept.

static inline void rocketnet_xdp_reply_macs_answer( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address, const uint8_t * mac )
{
    if ( rocketnet_xdp_reply_macs_find( table, address ) )
        return;

    table->learn_count++;

    struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find_state( table, address, ROCKETNET_XDP_REPLY_MAC_ANSWERED );
    if ( !entry )
    {
        const uint32_t slot = rocketnet_xdp_reply_macs_slot( address );
        struct rocketnet_xdp_reply_mac_entry_t * oldest = NULL;
        for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; ++i )
        {
            struct rocketnet_xdp_reply_mac_entry_t * candidate = &table->entries[( slot + (uint32_t) i ) & ( ROCKETNET_XDP_REPLY_MAC_ENTRIES - 1 )];
            if ( candidate->state == ROCKETNET_XDP_REPLY_MAC_EMPTY || candidate->state == ROCKETNET_XDP_REPLY_MAC_TOMBSTONE )
            {
                entry = candidate;
                break;
            }
            if ( candidate->state == ROCKETNET_XDP_REPLY_MAC_ANSWERED && ( !oldest || candidate->learned < oldest->learned ) )
                oldest = candidate;
        }
        if ( !entry )
            entry = oldest;
        if ( !entry )
            return;
        entry->address = *address;
        entry->state = ROCKETNET_XDP_REPLY_MAC_ANSWERED;
    }

    memcpy( entry->mac, mac, 6 );
    entry->learned = table->learn_count;
}

// the released client's learned MAC is forgotten: no reply is addressed by
// it as a learned client's again. It stays as the address's answered entry,
// for a reply still held for the released client.

static inline void rocketnet_xdp_reply_macs_forget( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * address )
{
    struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( table, address );
    if ( !entry )
        return;
    if ( rocketnet_xdp_reply_macs_find_state( table, address, ROCKETNET_XDP_REPLY_MAC_ANSWERED ) )
    {
        entry->state = ROCKETNET_XDP_REPLY_MAC_TOMBSTONE;
        return;
    }
    entry->state = ROCKETNET_XDP_REPLY_MAC_ANSWERED;
}

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
        return entry->mac;
    if ( destination && destination->type == ROCKETNET_ADDRESS_IPV4 && table->gateway_ipv4_known )
        return table->gateway_ipv4;
    if ( destination && destination->type == ROCKETNET_ADDRESS_IPV6 && table->gateway_ipv6_known )
        return table->gateway_ipv6;
    if ( table->gateway_known )
        return table->gateway_mac;
    if ( answered_mac )
    {
        if ( rocketnet_xdp_reply_mac_usable( answered_mac ) )
        {
            if ( destination )
                rocketnet_xdp_reply_macs_answer( table, destination, answered_mac );
            return answered_mac;
        }
    }
    else if ( destination )
    {
        entry = rocketnet_xdp_reply_macs_find_state( table, destination, ROCKETNET_XDP_REPLY_MAC_ANSWERED );
        if ( entry )
            return entry->mac;
    }
    if ( refused )
        ( *refused )++;
    return NULL;
}

#endif // ROCKETNET_XDP_FRAME_H

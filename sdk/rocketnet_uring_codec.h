/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server transport's completion codec, pure: no syscall, no
// allocation, no global state. It is compiled only on Linux with kernel uapi
// headers recent enough for multishot recvmsg (IORING_RECV_MULTISHOT); older
// uapi headers and non-Linux platforms compile the body out. This is the
// byte-level counterpart of sdk/rocketnet_xdp_frame.h: the byte-level parts
// of the transport (completion tags, the multishot recvmsg buffer layout, the
// source address conversion, the segmented send's msghdr) are written and
// unit tested here, and the transport links liburing for the ring itself.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_URING_CODEC_H
#define ROCKETNET_SDK_ROCKETNET_URING_CODEC_H

#include <stdint.h>
#include <string.h>

#include "rocketnet_address.h"
#include "rocketnet_platform.h"
#include "rocketnet_coalesce.h"

#if defined( __linux__ )

#include <linux/io_uring.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

// the control bytes of one segmented send: one UDP_SEGMENT cmsg holding the
// 16-bit stride
#define ROCKETNET_URING_GSO_CONTROL_BYTES    CMSG_SPACE( sizeof(uint16_t) )

// the control area a multishot recvmsg reserves in every receive buffer when
// UDP_GRO is on: one UDP_GRO cmsg holding the int stride
#define ROCKETNET_URING_GRO_CONTROL_BYTES    CMSG_SPACE( sizeof(int) )

// the kind fields carried in the upper bits of every SQE's user_data.
#define ROCKETNET_URING_TAG_SEND      1
#define ROCKETNET_URING_TAG_RECV      2
#define ROCKETNET_URING_TAG_CANCEL    3

// recvmsg buffer parse verdicts.
#define ROCKETNET_URING_RECV_OK                  0
#define ROCKETNET_URING_RECV_SHORT_BUFFER        1
#define ROCKETNET_URING_RECV_NAME_TRUNCATED      2
#define ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED   3

#ifdef IORING_RECV_MULTISHOT

// packs kind in bits 56..63, queue in bits 32..47 and index in bits 0..31.
static inline uint64_t rocketnet_uring_tag( int kind, int queue, uint32_t index )
{
    return ( (uint64_t) (uint8_t) kind << 56 ) | ( (uint64_t) (uint16_t) queue << 32 ) | index;
}

static inline int rocketnet_uring_tag_kind( uint64_t tag )
{
    return (int) ( ( tag >> 56 ) & 0xFF );
}

static inline int rocketnet_uring_tag_queue( uint64_t tag )
{
    return (int) ( ( tag >> 32 ) & 0xFFFF );
}

static inline int rocketnet_uring_tag_index( uint64_t tag )
{
    return (int) ( tag & 0xFFFFFFFF );
}

// returns the provided-buffer id from the CQE flags when the buffer flag is
// set, else -1.
static inline int rocketnet_uring_buffer_id( uint32_t cqe_flags )
{
    if ( cqe_flags & IORING_CQE_F_BUFFER )
        return (int) ( cqe_flags >> IORING_CQE_BUFFER_SHIFT );
    return -1;
}

// the GRO stride in the control area of a multishot recvmsg buffer:
// control_bytes is what the kernel wrote (io_uring_recvmsg_out.controllen),
// at most the area the msghdr reserved. The cmsg headers are read with memcpy,
// never cast. No UDP_GRO cmsg is 0 (one datagram). A truncated control area
// (MSG_CTRUNC in out_flags), a cmsg whose length is under its header or runs
// past the control area, or a UDP_GRO cmsg too short for an int, is one
// fault, the gso transport's: *gro_faults is incremented and the stride is 0.
static inline int rocketnet_uring_recvmsg_gro_stride( const uint8_t * control, int control_bytes, uint32_t out_flags, uint64_t * gro_faults )
{
    if ( out_flags & MSG_CTRUNC )
    {
        if ( gro_faults )
            *gro_faults += 1;
        return 0;
    }

    int offset = 0;
    while ( control_bytes - offset >= (int) sizeof( struct cmsghdr ) )
    {
        struct cmsghdr header;
        memcpy( &header, control + offset, sizeof( header ) );
        if ( header.cmsg_len < sizeof( struct cmsghdr ) || header.cmsg_len > (size_t) ( control_bytes - offset ) )
        {
            if ( gro_faults )
                *gro_faults += 1;
            return 0;
        }

        if ( header.cmsg_level == IPPROTO_UDP && header.cmsg_type == UDP_GRO )
        {
            if ( header.cmsg_len < CMSG_LEN( sizeof( int ) ) )
            {
                if ( gro_faults )
                    *gro_faults += 1;
                return 0;
            }

            int stride = 0;
            memcpy( &stride, control + offset + CMSG_LEN( 0 ), sizeof( stride ) );
            return stride > 0 ? stride : 0;
        }

        offset += (int) CMSG_ALIGN( header.cmsg_len );
    }

    return 0;
}

// parses the multishot recvmsg buffer layout: struct io_uring_recvmsg_out,
// then msg_namelen bytes of name area, then msg_controllen bytes of control
// area, then the payload. The header is read with memcpy so an unaligned
// buffer pointer is never cast. On OK the name and payload views have exactly
// out->namelen and out->payloadlen bytes, and *segment_bytes (when not NULL)
// is the UDP_GRO stride the control area carries, 0 for none
// (rocketnet_uring_recvmsg_gro_stride, which counts its faults into
// *gro_faults when not NULL); otherwise each refusal has its own value.
static inline int rocketnet_uring_recvmsg_parse(
    const uint8_t * buffer, int buffer_bytes,
    int msg_namelen, int msg_controllen,
    const uint8_t ** name, int * name_bytes,
    const uint8_t ** payload, int * payload_bytes,
    int * segment_bytes, uint64_t * gro_faults )
{
    if ( segment_bytes )
        *segment_bytes = 0;

    if ( msg_namelen < 0 || msg_controllen < 0 )
        return ROCKETNET_URING_RECV_SHORT_BUFFER;

    struct io_uring_recvmsg_out out;
    if ( buffer_bytes < (int) sizeof( out ) + msg_namelen + msg_controllen )
        return ROCKETNET_URING_RECV_SHORT_BUFFER;

    memcpy( &out, buffer, sizeof( out ) );
    if ( out.namelen > (uint32_t) msg_namelen )
        return ROCKETNET_URING_RECV_NAME_TRUNCATED;
    if ( out.flags & MSG_TRUNC )
        return ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED;

    int payload_left = buffer_bytes - (int) sizeof( out ) - msg_namelen - msg_controllen;
    if ( (int) out.payloadlen > payload_left )
        return ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED;

    *name = buffer + sizeof( out );
    *name_bytes = (int) out.namelen;
    *payload = buffer + sizeof( out ) + msg_namelen + msg_controllen;
    *payload_bytes = (int) out.payloadlen;

    if ( segment_bytes && msg_controllen > 0 )
    {
        const int control_bytes = out.controllen < (uint32_t) msg_controllen ? (int) out.controllen : msg_controllen;
        *segment_bytes = rocketnet_uring_recvmsg_gro_stride( buffer + sizeof( out ) + msg_namelen, control_bytes, out.flags, gro_faults );
    }

    return ROCKETNET_URING_RECV_OK;
}

// converts a source sockaddr to a rocketnet address, the same rule as the
// GSO transport. Ports and address words are network order in the sockaddr and
// host order in rocketnet_address_t; a v4-mapped source on a dual stack
// socket becomes native IPv4 exactly once; a name shorter than its family's
// sockaddr, or another family, returns false.
static inline bool rocketnet_uring_sockaddr_to_address(
    const struct sockaddr_storage * input, int name_bytes,
    bool socket_ipv6, struct rocketnet_address_t * out )
{
    if ( !input || !out )
        return false;

    if ( input->ss_family == AF_INET6 )
    {
        if ( name_bytes < (int) sizeof( struct sockaddr_in6 ) )
            return false;

        const struct sockaddr_in6 * addr = (const struct sockaddr_in6 *) input;
        out->type = ROCKETNET_ADDRESS_IPV6;

        for ( int i = 0; i < 8; ++i )
            out->data.ipv6[i] = ntohs( ( (const uint16_t *) &addr->sin6_addr )[i] );

        out->port = ntohs( addr->sin6_port );

        return rocketnet_address_accept_received_source( out, socket_ipv6 );
    }

    if ( input->ss_family == AF_INET )
    {
        if ( name_bytes < (int) sizeof( struct sockaddr_in ) )
            return false;

        rocketnet_platform_sockaddr_in_to_address( (const struct sockaddr_in *) input, out );
        return true;
    }

    return false;
}

// one SENDMSG's msghdr for the send run at the front of a queue of count
// packets: wire[i] and packet_bytes[i] are packet i's wire bytes, to[i] its
// destination. The run is rocketnet_coalesce_send_run's, bounded by
// max_segments and max_super_bytes: the same destination, every packet the
// size of the first except at most one shorter tail. The message gets one
// iovec per packet of the run, written to iovecs[0..run), the name the caller
// converted (name, namelen: the destination of packet 0), and, for a run
// longer than one, a UDP_SEGMENT cmsg in control holding the stride
// (packet_bytes[0]); a one-packet run carries no control. A control buffer
// smaller than ROCKETNET_URING_GSO_CONTROL_BYTES, or NULL, limits the run to
// one packet. Returns the packets in the run, 0 (message untouched) when the
// first packet has no bytes or count is 0.
static inline int rocketnet_uring_gso_message(
    struct msghdr * message, struct iovec * iovecs,
    uint8_t * control, int control_bytes,
    void * name, socklen_t namelen,
    uint8_t * const * wire, const int * packet_bytes,
    const struct rocketnet_address_t * to, int count,
    int max_segments, int max_super_bytes )
{
    const bool has_control = control != NULL && control_bytes >= (int) ROCKETNET_URING_GSO_CONTROL_BYTES;
    if ( !has_control || max_segments < 1 )
        max_segments = 1;

    const int run = rocketnet_coalesce_send_run( packet_bytes, to, count, max_segments, max_super_bytes );
    if ( run <= 0 )
        return 0;

    for ( int i = 0; i < run; i++ )
    {
        iovecs[i].iov_base = wire[i];
        iovecs[i].iov_len = (size_t) packet_bytes[i];
    }

    memset( message, 0, sizeof( *message ) );
    message->msg_name = name;
    message->msg_namelen = namelen;
    message->msg_iov = iovecs;
    message->msg_iovlen = (size_t) run;

    if ( run > 1 && has_control )
    {
        memset( control, 0, ROCKETNET_URING_GSO_CONTROL_BYTES );
        message->msg_control = control;
        message->msg_controllen = ROCKETNET_URING_GSO_CONTROL_BYTES;

        struct cmsghdr * cmsg = CMSG_FIRSTHDR( message );
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN( sizeof( uint16_t ) );

        const uint16_t stride = (uint16_t) packet_bytes[0];
        memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
    }

    return run;
}

#endif // #ifdef IORING_RECV_MULTISHOT
#endif // #if defined( __linux__ )
#endif // #ifndef ROCKETNET_SDK_ROCKETNET_URING_CODEC_H

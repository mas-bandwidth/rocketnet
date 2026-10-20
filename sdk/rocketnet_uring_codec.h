/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring server driver's completion codec, pure: no syscall, no
// allocation, no global state. It is compiled only on Linux with kernel uapi
// headers recent enough for multishot recvmsg (IORING_RECV_MULTISHOT); older
// uapi headers and non-Linux platforms compile the body out. This is the
// byte-level counterpart of sdk/rocketnet_xdp_frame.h: the byte-level parts
// of the driver (completion tags, the multishot recvmsg buffer layout, the
// source address conversion) are written and unit tested here, and the
// driver links liburing for the ring itself.

#ifndef ROCKETNET_URING_CODEC_H
#define ROCKETNET_URING_CODEC_H

#include <stdint.h>
#include <string.h>

#include "rocketnet_address.h"
#include "rocketnet_platform.h"

#if defined(__linux__)

#include <linux/io_uring.h>
#include <sys/socket.h>
#include <arpa/inet.h>

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
    return ( (uint64_t)(uint8_t) kind << 56 )
         | ( (uint64_t)(uint16_t) queue << 32 )
         | index;
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

// parses the multishot recvmsg buffer layout: struct io_uring_recvmsg_out,
// then msg_namelen bytes of name area, then msg_controllen bytes of control
// area, then the payload. The header is read with memcpy so an unaligned
// buffer pointer is never cast. On OK the name and payload views have exactly
// out->namelen and out->payloadlen bytes; otherwise each refusal has its own
// value.
static inline int rocketnet_uring_recvmsg_parse(
    const uint8_t * buffer, int buffer_bytes,
    int msg_namelen, int msg_controllen,
    const uint8_t ** name, int * name_bytes,
    const uint8_t ** payload, int * payload_bytes )
{
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
    return ROCKETNET_URING_RECV_OK;
}

// converts a source sockaddr to a rocketnet address, the same rule as the
// GSO driver. Ports and address words are network order in the sockaddr and
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
        const struct sockaddr_in6 * addr = (const struct sockaddr_in6*) input;
        out->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
            out->data.ipv6[i] = ntohs( ( (const uint16_t*) &addr->sin6_addr )[i] );
        out->port = ntohs( addr->sin6_port );
        return rocketnet_address_accept_received_source( out, socket_ipv6 );
    }
    if ( input->ss_family == AF_INET )
    {
        if ( name_bytes < (int) sizeof( struct sockaddr_in ) )
            return false;
        rocketnet_platform_sockaddr_in_to_address( (const struct sockaddr_in*) input, out );
        return true;
    }
    return false;
}

#endif // IORING_RECV_MULTISHOT
#endif // __linux__
#endif // ROCKETNET_URING_CODEC_H

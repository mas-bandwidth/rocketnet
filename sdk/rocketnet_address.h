/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_ADDRESS_H
#define ROCKETNET_SDK_ROCKETNET_ADDRESS_H

#include "rocketnet_internal.h"

#include <stdint.h>

// struct rocketnet_address_t is the SDK's own (rocketnet_layers.h); the
// public API takes and gives addresses only as strings.

#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus

// big endian ipv4 and port, as they sit on the wire
void rocketnet_address_load_ipv4( struct rocketnet_address_t * address, uint32_t big_endian_ipv4, uint16_t big_endian_port );

// dotted ipv4 with optional :port, bare ipv6, or bracketed ipv6 with optional :port.
// false for empty, overlong or malformed text, with the address cleared.
bool rocketnet_address_parse( struct rocketnet_address_t * address, const char * address_string );

// buffer must be at least ROCKETNET_MAX_ADDRESS_STRING_LENGTH bytes. ipv6 with a port is bracketed.
const char * rocketnet_address_to_string( const struct rocketnet_address_t * address, char * buffer );

// same type, port and address bytes; false for any type but ipv4 and ipv6
bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b );

// true for ::ffff:a.b.c.d
bool rocketnet_address_is_ipv4_in_ipv6( struct rocketnet_address_t * address );

// address must be a mapped ipv6 address. the port is preserved.
void rocketnet_address_convert_ipv6_to_ipv4( struct rocketnet_address_t * address );

void rocketnet_address_convert_ipv4_to_ipv6( struct rocketnet_address_t * address );

// the four ipv4 bytes as stored, copied into a uint32_t
uint32_t rocketnet_address_ipv4( struct rocketnet_address_t * address );

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

// the one rule for an ipv4-mapped source (::ffff:a.b.c.d) on receive.
//
// on a dual stack socket (IPV6_V6ONLY off) the kernel delivers every ipv4
// client as a mapped address: it is accepted and canonicalised to native
// ipv4 here, at the edge, so connection keys, rate limits, cookies and token
// binding all see one address per client.
//
// anywhere the real ipv6 header is seen -- a raw or xdp ipv6 path, or an
// ipv6-only socket -- a mapped source never occurs legitimately (RFC 4291
// 2.5.5.2): it is spoofed, and dropped.
//
// returns false when the source must be dropped. any other source passes
// through unchanged.

static inline bool rocketnet_address_accept_received_source( struct rocketnet_address_t * from, bool dual_stack )
{
    if ( !rocketnet_address_is_ipv4_in_ipv6( from ) )
        return true;
    if ( !dual_stack )
        return false;

    rocketnet_address_convert_ipv6_to_ipv4( from );

    return true;
}

#endif // ROCKETNET_SDK_ROCKETNET_ADDRESS_H

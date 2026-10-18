/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_ADDRESS_H
#define ROCKETNET_ADDRESS_H

#include "rocketnet_internal.h"

#include <stdint.h>

// struct rocketnet_address_t is the SDK's own (rocketnet_layers.h); the
// public API takes and gives addresses only as strings.

#ifdef __cplusplus
extern "C" {
#endif

void rocketnet_address_load_ipv4( struct rocketnet_address_t * address, uint32_t big_endian_ipv4, uint16_t big_endian_port );

bool rocketnet_address_parse( struct rocketnet_address_t * address, const char * address_string );

// both of the following write into the caller's buffer bounded by
// ROCKETNET_MAX_ADDRESS_STRING_LENGTH: the buffer must be at least that many
// bytes, terminator included. the constant arrives with rocketnet.h, above.

const char * rocketnet_address_to_string( const struct rocketnet_address_t * address, char * buffer );

const char * rocketnet_address_to_string_without_port( const struct rocketnet_address_t * address, char * buffer );

bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b );

void rocketnet_address_anonymize( struct rocketnet_address_t * address );

bool rocketnet_address_is_ipv4_in_ipv6( struct rocketnet_address_t * address );

void rocketnet_address_convert_ipv6_to_ipv4( struct rocketnet_address_t * address );

void rocketnet_address_convert_ipv4_to_ipv6( struct rocketnet_address_t * address );

uint32_t rocketnet_address_ipv4( struct rocketnet_address_t * address );

#ifdef __cplusplus
}
#endif

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

#endif // #ifndef ROCKETNET_ADDRESS_H

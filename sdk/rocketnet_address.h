/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_ADDRESS_H
#define ROCKETNET_ADDRESS_H

#include "rocketnet.h"

#include <stdint.h>

#ifndef ROCKETNET_ADDRESS_ALREADY_DEFINED
#define ROCKETNET_ADDRESS_ALREADY_DEFINED
struct rocketnet_address_t
{
    union { uint32_t ip; uint8_t ipv4[4]; uint16_t ipv6[8]; } data;
    uint16_t port;
    uint8_t type;
};
#endif 

ROCKETNET_EXPORT_FUNC void rocketnet_address_load_ipv4( struct rocketnet_address_t * address, uint32_t big_endian_ipv4, uint16_t big_endian_port );

ROCKETNET_EXPORT_FUNC bool rocketnet_address_parse( struct rocketnet_address_t * address, const char * address_string );

ROCKETNET_EXPORT_FUNC const char * rocketnet_address_to_string( const struct rocketnet_address_t * address, char * buffer );

ROCKETNET_EXPORT_FUNC const char * rocketnet_address_to_string_without_port( const struct rocketnet_address_t * address, char * buffer );

ROCKETNET_EXPORT_FUNC bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b );

ROCKETNET_EXPORT_FUNC void rocketnet_address_anonymize( struct rocketnet_address_t * address );

ROCKETNET_EXPORT_FUNC bool rocketnet_address_is_ipv4_in_ipv6( struct rocketnet_address_t * address );

ROCKETNET_EXPORT_FUNC void rocketnet_address_convert_ipv6_to_ipv4( struct rocketnet_address_t * address );

ROCKETNET_EXPORT_FUNC void rocketnet_address_convert_ipv4_to_ipv6( struct rocketnet_address_t * address );

ROCKETNET_EXPORT_FUNC uint32_t rocketnet_address_ipv4( struct rocketnet_address_t * address );

#endif // #ifndef ROCKETNET_ADDRESS_H

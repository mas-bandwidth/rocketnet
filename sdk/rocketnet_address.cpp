/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_address.h"
#include "rocketnet_platform.h"

#include <stdlib.h>
#include <memory.h>
#include <stdio.h>

void rocketnet_address_load_ipv4( struct rocketnet_address_t * address, uint32_t big_endian_ipv4, uint16_t big_endian_port )
{
    rocketnet_assert( address );
    memset( address, 0, sizeof(rocketnet_address_t) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    memcpy( address->data.ipv4, (uint8_t*) &big_endian_ipv4, 4 );
    address->port = rocketnet_platform_ntohs( big_endian_port );
}

static bool rocketnet_address_parse_port( const char * text, uint16_t & port )
{
    if ( !*text )
        return false;
    unsigned int value = 0;
    for ( ; *text; ++text )
    {
        if ( *text < '0' || *text > '9' )
            return false;
        value = value * 10 + (unsigned int) ( *text - '0' );
        if ( value > 65535 )
            return false;
    }
    port = (uint16_t) value;
    return true;
}

bool rocketnet_address_parse( rocketnet_address_t * address, const char * address_string_in )
{
    rocketnet_assert( address );
    rocketnet_assert( address_string_in );
    if ( !address )
        return false;
    memset( address, 0, sizeof( rocketnet_address_t ) );
    if ( !address_string_in )
        return false;

    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    size_t length = 0;
    while ( length < sizeof(buffer) && address_string_in[length] )
        ++length;
    if ( !length || length == sizeof(buffer) )
        return false;
    memcpy( buffer, address_string_in, length + 1 );

    char * host = buffer;
    uint16_t port = 0;
    const bool bracketed = buffer[0] == '[';
    if ( bracketed )
    {
        host++;
        char * close = strchr( host, ']' );
        if ( !close )
            return false;
        if ( close[1] && ( close[1] != ':' || !rocketnet_address_parse_port( close + 2, port ) ) )
            return false;
        *close = '\0';
    }

    uint16_t addr6[8];
    if ( rocketnet_platform_inet_pton6( host, addr6 ) )
    {
        address->type = ROCKETNET_ADDRESS_IPV6;
        address->port = port;
        for ( int i = 0; i < 8; ++i )
            address->data.ipv6[i] = rocketnet_platform_ntohs( addr6[i] );
        return true;
    }
    if ( bracketed )
        return false;

    char * colon = strchr( host, ':' );
    if ( colon )
    {
        if ( !rocketnet_address_parse_port( colon + 1, port ) )
            return false;
        *colon = '\0';
    }
    uint32_t addr4;
    if ( !rocketnet_platform_inet_pton4( host, &addr4 ) )
        return false;

    address->type = ROCKETNET_ADDRESS_IPV4;
    address->port = port;
    // inet_pton returns network order, matching the byte-oriented IPv4 storage.
    memcpy( address->data.ipv4, &addr4, 4 );
    return true;
}

const char * rocketnet_address_to_string( const rocketnet_address_t * address, char * buffer )
{
    rocketnet_assert( buffer );

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        uint16_t ipv6_network_order[8];
        for ( int i = 0; i < 8; ++i )
            ipv6_network_order[i] = rocketnet_platform_htons( address->data.ipv6[i] );
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_platform_inet_ntop6( ipv6_network_order, address_string, sizeof( address_string ) );
        if ( address->port == 0 )
        {
            rocketnet_copy_string( buffer, address_string, ROCKETNET_MAX_ADDRESS_STRING_LENGTH );
            return buffer;
        }
        else
        {
            const int result = snprintf( buffer, ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "[%s]:%hu", address_string, address->port );
            if ( result < 0 || result >= ROCKETNET_MAX_ADDRESS_STRING_LENGTH )
            {
                rocketnet_error( "address formatting failed or string truncated: [%s]:%hu", address_string, address->port );
            }
            return buffer;
        }
    }
    else if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        if ( address->port != 0 )
        {
            snprintf( buffer,
                      ROCKETNET_MAX_ADDRESS_STRING_LENGTH,
                      "%d.%d.%d.%d:%d",
                      address->data.ipv4[0],
                      address->data.ipv4[1],
                      address->data.ipv4[2],
                      address->data.ipv4[3],
                      address->port );
        }
        else
        {
            snprintf( buffer,
                      ROCKETNET_MAX_ADDRESS_STRING_LENGTH,
                      "%d.%d.%d.%d",
                      address->data.ipv4[0],
                      address->data.ipv4[1],
                      address->data.ipv4[2],
                      address->data.ipv4[3] );
        }
        return buffer;
    }
    else
    {
        snprintf( buffer, ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "%s", "NONE" );
        return buffer;
    }
}

const char * rocketnet_address_to_string_without_port( const rocketnet_address_t * address, char * buffer )
{
    rocketnet_assert( buffer );

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        uint16_t ipv6_network_order[8];
        for ( int i = 0; i < 8; ++i )
            ipv6_network_order[i] = rocketnet_platform_htons( address->data.ipv6[i] );
        char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_platform_inet_ntop6( ipv6_network_order, address_string, sizeof( address_string ) );
        rocketnet_copy_string( buffer, address_string, ROCKETNET_MAX_ADDRESS_STRING_LENGTH );
        return buffer;
    }
    else if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        snprintf( buffer,
                  ROCKETNET_MAX_ADDRESS_STRING_LENGTH,
                  "%d.%d.%d.%d",
                  address->data.ipv4[0],
                  address->data.ipv4[1],
                  address->data.ipv4[2],
                  address->data.ipv4[3] );
        return buffer;
    }
    else
    {
        snprintf( buffer, ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "%s", "NONE" );
        return buffer;
    }
}

bool rocketnet_address_equal( const rocketnet_address_t * a, const rocketnet_address_t * b )
{
    rocketnet_assert( a );
    rocketnet_assert( b );

    if ( a->type != b->type )
        return false;

    if ( a->type != ROCKETNET_ADDRESS_IPV4 && a->type != ROCKETNET_ADDRESS_IPV6 )
        return false;

    if ( a->type == ROCKETNET_ADDRESS_IPV4 )
    {
        if ( a->port != b->port )
            return false;

        for ( int i = 0; i < 4; ++i )
        {
            if ( a->data.ipv4[i] != b->data.ipv4[i] )
                return false;
        }
    }
    else if ( a->type == ROCKETNET_ADDRESS_IPV6 )
    {
        if ( a->port != b->port )
            return false;

        for ( int i = 0; i < 8; ++i )
        {
            if ( a->data.ipv6[i] != b->data.ipv6[i] )
                return false;
        }
    }

    return true;
}

void rocketnet_address_anonymize( rocketnet_address_t * address )
{
    rocketnet_assert( address );
    if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        address->data.ipv4[3] = 0;
    }
    else
    {
        if ( rocketnet_address_is_ipv4_in_ipv6( address ) )
        {
            address->data.ipv6[7] &= 0xFF00;
        }
        else
        {
            address->data.ipv6[4] = 0;
            address->data.ipv6[5] = 0;
            address->data.ipv6[6] = 0;
            address->data.ipv6[7] = 0;
        }
    }
    address->port = 0;
}

bool rocketnet_address_is_ipv4_in_ipv6( struct rocketnet_address_t * address )
{
    rocketnet_assert( address );
    if ( address->type != ROCKETNET_ADDRESS_IPV6 )
        return false;
    if ( address->data.ipv6[0] != 0x0000 )
        return false;
    if ( address->data.ipv6[1] != 0x0000 )
        return false;
    if ( address->data.ipv6[2] != 0x0000 )
        return false;
    if ( address->data.ipv6[3] != 0x0000 )
        return false;
    if ( address->data.ipv6[4] != 0x0000 )
        return false;
    if ( address->data.ipv6[5] != 0xFFFF )
        return false;
    return true;
}

void rocketnet_address_convert_ipv4_to_ipv6( struct rocketnet_address_t * address )
{
    rocketnet_assert( address );
    const uint8_t a = uint8_t( address->data.ipv4[0] );
    const uint8_t b = uint8_t( address->data.ipv4[1] );
    const uint8_t c = uint8_t( address->data.ipv4[2] );
    const uint8_t d = uint8_t( address->data.ipv4[3] );
    address->type = ROCKETNET_ADDRESS_IPV6;
    address->data.ipv6[0] = 0x0000;
    address->data.ipv6[1] = 0x0000;
    address->data.ipv6[2] = 0x0000;
    address->data.ipv6[3] = 0x0000;
    address->data.ipv6[4] = 0x0000;
    address->data.ipv6[5] = 0xFFFF;
    address->data.ipv6[6] = (uint16_t) ( ( uint16_t(a) << 8 ) | uint16_t(b) );
    address->data.ipv6[7] = (uint16_t) ( ( uint16_t(c) << 8 ) | uint16_t(d) );
}

void rocketnet_address_convert_ipv6_to_ipv4( struct rocketnet_address_t * address )
{
    // IMPORTANT: this function is *only* for converting ipv4 mapped addresses in ipv6 back to native ipv4
    rocketnet_assert( address );
    rocketnet_assert( rocketnet_address_is_ipv4_in_ipv6( address ) );
    const uint8_t a = (uint8_t) ( ( address->data.ipv6[6] & 0xFF00 ) >> 8 );
    const uint8_t b = (uint8_t) ( address->data.ipv6[6] & 0x00FF );
    const uint8_t c = (uint8_t) ( ( address->data.ipv6[7] & 0xFF00 ) >> 8 );
    const uint8_t d = (uint8_t) ( address->data.ipv6[7] & 0x00FF );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = a;
    address->data.ipv4[1] = b;
    address->data.ipv4[2] = c;
    address->data.ipv4[3] = d;
}

uint32_t rocketnet_address_ipv4( struct rocketnet_address_t * address )
{
    rocketnet_assert( address );
    uint32_t ipv4;
    memcpy( (char*) &ipv4, address->data.ipv4, 4 );
    return ipv4;
}

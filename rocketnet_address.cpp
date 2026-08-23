/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Addresses, moved whole from flow's flow_address.cpp and converted to the
    rocketnet seam on arrival:

      - flow_address_parse returned bool; rocketnet_address_parse returns int,
        ROCKETNET_OK or ROCKETNET_ERROR_INVALID_ARGUMENT, per the header.
      - the struct is opaque at the seam; rocketnet_address_bytes() is new and
        trivial, so a foreign caller can allocate one without the layout.
      - flow_address_equal stays bool because the header says bool.
      - flow's load_ipv4 / anonymize / ipv4-in-ipv6 conversions are NOT in
        rocketnet.h and are not moved. They stay in flow until a stage needs
        them at which point they arrive through the header, not around it.

    Hostname resolution is deliberately absent, exactly as the header says:
    this library does not make blocking calls on a caller's behalf.
*/

#include "rocketnet_address.h"
#include "rocketnet_platform.h"
#include "rocketnet_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int rocketnet_address_parse( struct rocketnet_address_t * address, const char * address_string_in )
{
    rocketnet_assert( address );
    rocketnet_assert( address_string_in );

    if ( !address )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !address_string_in )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    memset( address, 0, sizeof( rocketnet_address_t ) );

    // first try to parse the string as an IPv6 address:
    // 1. if the first character is '[' then it's probably an ipv6 in form "[addr6]:portnum"
    // 2. otherwise try to parse as an IPv6 address using inet_pton

    #define ROCKETNET_ADDRESS_BUFFER_SAFETY 32

    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH + ROCKETNET_ADDRESS_BUFFER_SAFETY*2];

    char * address_string = buffer + ROCKETNET_ADDRESS_BUFFER_SAFETY;
    rocketnet_copy_string( address_string, address_string_in, ROCKETNET_MAX_ADDRESS_STRING_LENGTH );

    int address_string_length = (int) strlen( address_string );

    if ( address_string[0] == '[' )
    {
        const int base_index = address_string_length - 1;

        // note: no need to search past 6 characters as ":65535" is longest possible port value
        for ( int i = 0; i < 6; ++i )
        {
            const int index = base_index - i;
            if ( index < 0 )
            {
                return ROCKETNET_ERROR_INVALID_ARGUMENT;
            }
            if ( address_string[index] == ':' )
            {
                address->port = (uint16_t) ( atoi( &address_string[index + 1] ) );
                address_string[index-1] = '\0';
                break;
            }
            else if ( address_string[index] == ']' )
            {
                // no port number
                address->port = 0;
                address_string[index] = '\0';
                break;
            }
        }
        address_string += 1;
    }

    uint16_t addr6[8];
    if ( rocketnet_platform_inet_pton6( address_string, addr6 ) )
    {
        address->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            address->data.ipv6[i] = rocketnet_platform_ntohs( addr6[i] );
        }
        return ROCKETNET_OK;
    }

    // otherwise it's probably an IPv4 address:
    // 1. look for ":portnum", if found save the portnum and strip it out
    // 2. parse remaining ipv4 address via inet_pton

    address_string_length = (int) strlen( address_string );
    const int base_index = address_string_length - 1;
    for ( int i = 0; i < 6; ++i )
    {
        const int index = base_index - i;
        if ( index < 0 )
            break;
        if ( address_string[index] == ':' )
        {
            address->port = (uint16_t)( atoi( &address_string[index + 1] ) );
            address_string[index] = '\0';
        }
    }

    uint32_t addr4;
    if ( rocketnet_platform_inet_pton4( address_string, &addr4 ) )
    {
        address->type = ROCKETNET_ADDRESS_IPV4;
        address->data.ipv4[3] = (uint8_t) ( ( addr4 & 0xFF000000 ) >> 24 );
        address->data.ipv4[2] = (uint8_t) ( ( addr4 & 0x00FF0000 ) >> 16 );
        address->data.ipv4[1] = (uint8_t) ( ( addr4 & 0x0000FF00 ) >> 8  );
        address->data.ipv4[0] = (uint8_t) ( ( addr4 & 0x000000FF )      );
        return ROCKETNET_OK;
    }

    return ROCKETNET_ERROR_INVALID_ARGUMENT;
}

const char * rocketnet_address_to_string( const struct rocketnet_address_t * address, char * buffer )
{
    rocketnet_assert( address );
    rocketnet_assert( buffer );

    if ( !buffer )
        return NULL;

    if ( !address )
    {
        buffer[0] = '\0';
        return buffer;
    }

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
            if ( snprintf( buffer, ROCKETNET_MAX_ADDRESS_STRING_LENGTH, "[%s]:%hu", address_string, address->port ) < 0 )
            {
                rocketnet_error( "address string truncated: [%s]:%hu", address_string, address->port );
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

bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b )
{
    rocketnet_assert( a );
    rocketnet_assert( b );

    if ( !a || !b )
        return false;

    if ( a->type != b->type )
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

size_t rocketnet_address_bytes( void )
{
    return sizeof( struct rocketnet_address_t );
}

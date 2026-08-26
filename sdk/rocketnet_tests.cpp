/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_tests.h"
#include "rocketnet.h"

#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_base64.h"
#include "rocketnet_hash.h"
#include "rocketnet_packet_filter.h"
#include "rocketnet_packet_loss_tracker.h"
#include "rocketnet_out_of_order_tracker.h"
#include "rocketnet_jitter_tracker.h"
#include "rocketnet_value_tracker.h"
#include "rocketnet_hydrogen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void rocketnet_check_handler( const char * condition,
                                const char * function,
                                const char * file,
                                int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
#ifndef NDEBUG
    #if defined( __GNUC__ )
        __builtin_trap();
    #elif defined( _MSC_VER )
        __debugbreak();
    #endif
#endif
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );         \
    }                                                                                                           \
} while(0)

void rocketnet_test_time()
{
    double start = rocketnet_platform_time();
    rocketnet_platform_sleep( 0.1 );
    double finish = rocketnet_platform_time();
    rocketnet_check( finish > start );
}

void rocketnet_test_endian()
{
    uint32_t value = 0x11223344;
    char bytes[4];
    memcpy( bytes, &value, 4 );

#if ROCKETNET_LITTLE_ENDIAN

    rocketnet_check( bytes[0] == 0x44 );
    rocketnet_check( bytes[1] == 0x33 );
    rocketnet_check( bytes[2] == 0x22 );
    rocketnet_check( bytes[3] == 0x11 );

#else // #if ROCKETNET_LITTLE_ENDIAN

    rocketnet_check( bytes[3] == 0x44 );
    rocketnet_check( bytes[2] == 0x33 );
    rocketnet_check( bytes[1] == 0x22 );
    rocketnet_check( bytes[0] == 0x11 );

#endif // #if ROCKETNET_LITTLE_ENDIAN
}

void rocketnet_test_base64()
{
    const char * input = "a test string. let's see if it works properly";
    char encoded[1024];
    char decoded[1024];
    rocketnet_check( rocketnet_base64_encode_string( input, encoded, sizeof(encoded) ) > 0 );
    rocketnet_check( rocketnet_base64_decode_string( encoded, decoded, sizeof(decoded) ) > 0 );
    rocketnet_check( strcmp( decoded, input ) == 0 );
    rocketnet_check( rocketnet_base64_decode_string( encoded, decoded, 10 ) == 0 );
}

void rocketnet_test_hash()
{
    uint64_t hash = rocketnet_hash_string( "local" );
    rocketnet_check( hash == 0x249f1fb6f3a680e8ULL );
}

void rocketnet_test_copy_string()
{
    // copy valid string
    {
        char buffer[256];
        size_t result = rocketnet_copy_string( buffer, "hello", sizeof(buffer) );
        rocketnet_check( result == 5 );
        rocketnet_check( strncmp( buffer, "hello", sizeof(buffer) ) == 0 );
    }

    // copy empty string
    {
        char buffer[256];
        size_t result = rocketnet_copy_string( buffer, "", sizeof(buffer) );
        rocketnet_check( result == 0 );
        rocketnet_check( strncmp( buffer, "", sizeof(buffer) ) == 0 );
    }

    // truncated string
    {
        char buffer[4];
        size_t result = rocketnet_copy_string( buffer, "hello my baby, hello my darling", sizeof(buffer) );
        rocketnet_check( result == 3 );
        rocketnet_check( strncmp( buffer, "hel", sizeof(buffer) ) == 0 );
    }
}

void rocketnet_test_address()
{
    {
        struct rocketnet_address_t address;
        rocketnet_check( !rocketnet_address_parse( &address, "" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "[" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "[]" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "[]:" ) );
        rocketnet_check( !rocketnet_address_parse( &address, ":" ) );
#if !defined(WINVER) || WINVER > 0x502 // windows xp sucks
        rocketnet_check( !rocketnet_address_parse( &address, "1" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "12" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "123" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "1234" ) );
#endif
        rocketnet_check( !rocketnet_address_parse( &address, "1234.0.12313.0000" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "1234.0.12313.0000.0.0.0.0.0" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "1312313:123131:1312313:123131:1312313:123131:1312313:123131:1312313:123131:1312313:123131" ) );
        rocketnet_check( !rocketnet_address_parse( &address, "." ) );
        rocketnet_check( !rocketnet_address_parse( &address, ".." ) );
        rocketnet_check( !rocketnet_address_parse( &address, "..." ) );
        rocketnet_check( !rocketnet_address_parse( &address, "...." ) );
        rocketnet_check( !rocketnet_address_parse( &address, "....." ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "107.77.207.77" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv4[0] == 107 );
        rocketnet_check( address.data.ipv4[1] == 77 );
        rocketnet_check( address.data.ipv4[2] == 207 );
        rocketnet_check( address.data.ipv4[3] == 77 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv4[0] == 127 );
        rocketnet_check( address.data.ipv4[1] == 0 );
        rocketnet_check( address.data.ipv4[2] == 0 );
        rocketnet_check( address.data.ipv4[3] == 1 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "107.77.207.77:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv4[0] == 107 );
        rocketnet_check( address.data.ipv4[1] == 77 );
        rocketnet_check( address.data.ipv4[2] == 207 );
        rocketnet_check( address.data.ipv4[3] == 77 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv4[0] == 127 );
        rocketnet_check( address.data.ipv4[1] == 0 );
        rocketnet_check( address.data.ipv4[2] == 0 );
        rocketnet_check( address.data.ipv4[3] == 1 );
    }

#if ROCKETNET_PLATFORM_HAS_IPV6
    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "fe80::202:b3ff:fe1e:8329" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv6[0] == 0xfe80 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0202 );
        rocketnet_check( address.data.ipv6[5] == 0xb3ff );
        rocketnet_check( address.data.ipv6[6] == 0xfe1e );
        rocketnet_check( address.data.ipv6[7] == 0x8329 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "::" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0000 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "::1" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0001 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[fe80::202:b3ff:fe1e:8329]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0xfe80 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0202 );
        rocketnet_check( address.data.ipv6[5] == 0xb3ff );
        rocketnet_check( address.data.ipv6[6] == 0xfe1e );
        rocketnet_check( address.data.ipv6[7] == 0x8329 );
        rocketnet_check( !rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0000 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::1]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0x0000 );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0001 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:127.0.0.1]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x7F00 );
        rocketnet_check( address.data.ipv6[7] == 0x0001 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:0.0.0.0]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0000 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:1.2.3.4]:40000" ) );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x0102 );
        rocketnet_check( address.data.ipv6[7] == 0x0304 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::ffff:1.2.3.4]:40000" ) );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );

        rocketnet_address_convert_ipv6_to_ipv4( &address );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv4[0] == 1 );
        rocketnet_check( address.data.ipv4[1] == 2 );
        rocketnet_check( address.data.ipv4[2] == 3 );
        rocketnet_check( address.data.ipv4[3] == 4 );

        rocketnet_address_convert_ipv4_to_ipv6( &address );

        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0x0000 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0000 );
        rocketnet_check( address.data.ipv6[5] == 0xFFFF );
        rocketnet_check( address.data.ipv6[6] == 0x0102 );
        rocketnet_check( address.data.ipv6[7] == 0x0304 );
        rocketnet_check( rocketnet_address_is_ipv4_in_ipv6( &address ) );
    }
#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
}

static bool equal_within_tolerance( float a, float b, float tolerance = 0.001f )
{
    return fabs(double(a)-double(b)) <= tolerance;
}

void rocketnet_test_random_bytes()
{
    const int BufferSize = 999;
    uint8_t buffer[BufferSize];
    rocketnet_random_bytes( buffer, BufferSize );
    for ( int i = 0; i < 100; ++i )
    {
        uint8_t rocketnet_buffer[BufferSize];
        rocketnet_random_bytes( rocketnet_buffer, BufferSize );
        rocketnet_check( memcmp( buffer, rocketnet_buffer, BufferSize ) != 0 );
        memcpy( buffer, rocketnet_buffer, BufferSize );
    }
}

void rocketnet_test_random_float()
{
    for ( int i = 0; i < 1000; ++i )
    {
        float value = rocketnet_random_float();
        rocketnet_check( value >= 0.0f );
        rocketnet_check( value <= 1.0f );
    }
}

void rocketnet_test_platform_socket()
{
    // non-blocking socket (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with timeout (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with no timeout (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) );
        rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        rocketnet_platform_socket_destroy( socket );
    }

#if ROCKETNET_PLATFORM_HAS_IPV6

    // non-blocking socket (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with timeout (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with no timeout (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) );
        rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        rocketnet_platform_socket_destroy( socket );
    }

#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
}

static bool threads_work = false;

static void test_thread_function(void*)
{
    threads_work = true;
}

void rocketnet_test_platform_thread()
{
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, test_thread_function, NULL );
    rocketnet_check( thread );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    rocketnet_check( threads_work );
}

void rocketnet_test_platform_mutex()
{
    rocketnet_platform_mutex_t mutex;
    bool result = rocketnet_platform_mutex_create( &mutex );
    rocketnet_check( result );
    rocketnet_platform_mutex_acquire( &mutex );
    rocketnet_platform_mutex_release( &mutex );
    {
        rocketnet_platform_mutex_guard( &mutex );
        // ...
    }
    rocketnet_platform_mutex_destroy( &mutex );
}

void rocketnet_test_value_tracker()
{
    // initial values without any samples added should be 0/0/0
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }

    // add just one sample and min/max/avg should be set to that sample value
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        rocketnet_value_tracker_add_sample( &tracker, 1.0f );
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 1.0f );
        rocketnet_check( max_value == 1.0f );
        rocketnet_check( avg_value == 1.0f );
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }

    // add a bunch of samples and we should see min/max/avg
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        for ( int i = 0; i < 100; i++ )
        {
            rocketnet_value_tracker_add_sample( &tracker, float(i%10) );
        }
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 9.0f );
        rocketnet_check( avg_value == 4.5f );
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }

    // add more samples than history size and it should still work
    {
        rocketnet_value_tracker_t tracker;
        rocketnet_value_tracker_reset( &tracker );
        for ( int i = 0; i < ROCKETNET_VALUE_TRACKER_HISTORY * 2; i++ )
        {
            rocketnet_value_tracker_add_sample( &tracker, float(i%10) );
        }
        float min_value, max_value, avg_value;
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 9.0f );
        rocketnet_check( fabs( 4.5f - avg_value ) < 0.1f );
        rocketnet_value_tracker_calculate( &tracker, &min_value, &max_value, &avg_value );
        rocketnet_check( min_value == 0.0f );
        rocketnet_check( max_value == 0.0f );
        rocketnet_check( avg_value == 0.0f );
    }
}

void rocketnet_test_packet_filter()
{
    uint8_t output[ROCKETNET_MAX_PACKET_BYTES];
    memset( output, 0, sizeof(output) );
    output[0] = 1;

    for ( int i = 0; i < 10000; ++i )
    {
        uint8_t magic[8];
        uint8_t from_address[4];
        uint8_t to_address[4];

        rocketnet_random_bytes( magic, 8 );
        rocketnet_random_bytes( from_address, 4 );
        rocketnet_random_bytes( to_address, 4 );

        int packet_length = 18 + ( i % ( sizeof(output) - 18 ) );
        
        rocketnet_generate_pittle( output + 1, from_address, to_address, packet_length );

        rocketnet_generate_chonkle( output + 3, magic, from_address, to_address, packet_length );

        rocketnet_check( rocketnet_basic_packet_filter( output, packet_length ) );

    }
}

// The filter must grant no exemption keyed on the type byte: a zero first
// byte never excuses a packet from the 18-byte minimum or the pittle and
// chonkle checks, and a real ROCKETNET_PACKET_DIRECT (0) packet with a valid
// header must still pass on its merits.
//
// rocketnet_test_basic_packet_filter above cannot cover this: it fuzzes from a
// fixed srand(100), and that seed happens never to produce a zero first byte
// on a packet long enough to matter. This test does not depend on luck.
void rocketnet_test_packet_filter_no_type_zero_bypass()
{
    // the minimal attack: a single zero byte
    uint8_t one_zero_byte[1];
    one_zero_byte[0] = 0;
    rocketnet_check( !rocketnet_basic_packet_filter( one_zero_byte, 1 ) );

    // a zero first byte must not excuse a short packet at any length below the
    // 18-byte header minimum
    uint8_t zeroes[64];
    memset( zeroes, 0, sizeof(zeroes) );
    for ( uint16_t length = 0; length < 18; ++length )
    {
        rocketnet_check( !rocketnet_basic_packet_filter( zeroes, length ) );
    }

    // nor a long one that carries no valid header
    rocketnet_check( !rocketnet_basic_packet_filter( zeroes, sizeof(zeroes) ) );

    // and the converse: a real direct packet -- ROCKETNET_PACKET_DIRECT is
    // type 0 -- with a valid pittle and chonkle header still passes on its
    // merits.
    uint8_t output[256];
    memset( output, 0, sizeof(output) );
    uint8_t magic[8];
    uint8_t from_address[4];
    uint8_t to_address[4];
    memset( magic, 0, sizeof(magic) );
    memset( from_address, 0, sizeof(from_address) );
    memset( to_address, 0, sizeof(to_address) );

    const int packet_length = 64;
    output[0] = 0; // ROCKETNET_PACKET_DIRECT
    rocketnet_generate_pittle( output + 1, from_address, to_address, packet_length );
    rocketnet_generate_chonkle( output + 3, magic, from_address, to_address, packet_length );
    rocketnet_check( rocketnet_basic_packet_filter( output, packet_length ) );
}

void rocketnet_test_basic_packet_filter()
{
    uint8_t output[256];
    memset( output, 0, sizeof(output) );
    uint64_t pass = 0;
    uint64_t iterations = 100;
    srand( 100 );
    for ( int i = 0; i < int(iterations); ++i )
    {
        for ( int j = 0; j < int(sizeof(output)); ++j )
        {
            output[j] = uint8_t( rand() % 256 );
        }
        if ( rocketnet_basic_packet_filter( output, rand() % sizeof(output) ) )
        {
            pass++;
        }
    }
    rocketnet_check( pass == 0 );
}

void rocketnet_test_address_data_ipv4()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "127.0.0.1:50000" );
    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    uint8_t address_data[4];
    rocketnet_address_data( &address, address_data );
    rocketnet_check( address_data[0] == 127 );
    rocketnet_check( address_data[1] == 0 );
    rocketnet_check( address_data[2] == 0 );
    rocketnet_check( address_data[3] == 1 );
}

void rocketnet_test_anonymize_address_ipv4()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "1.2.3.4:5" );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_check( address.data.ipv4[0] == 1 );
    rocketnet_check( address.data.ipv4[1] == 2 );
    rocketnet_check( address.data.ipv4[2] == 3 );
    rocketnet_check( address.data.ipv4[3] == 4 );
    rocketnet_check( address.port == 5 );

    rocketnet_address_anonymize( &address );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_check( address.data.ipv4[0] == 1 );
    rocketnet_check( address.data.ipv4[1] == 2 );
    rocketnet_check( address.data.ipv4[2] == 3 );
    rocketnet_check( address.data.ipv4[3] == 0 );
    rocketnet_check( address.port == 0 );
}

#if ROCKETNET_PLATFORM_HAS_IPV6

void rocketnet_test_anonymize_address_ipv6()
{
    rocketnet_address_t address;
    rocketnet_address_parse( &address, "[2001:0db8:85a3:0000:0000:8a2e:0370:7334]:40000" );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_check( address.data.ipv6[0] == 0x2001 );
    rocketnet_check( address.data.ipv6[1] == 0x0db8 );
    rocketnet_check( address.data.ipv6[2] == 0x85a3 );
    rocketnet_check( address.data.ipv6[3] == 0x0000 );
    rocketnet_check( address.data.ipv6[4] == 0x0000 );
    rocketnet_check( address.data.ipv6[5] == 0x8a2e );
    rocketnet_check( address.data.ipv6[6] == 0x0370 );
    rocketnet_check( address.data.ipv6[7] == 0x7334 );
    rocketnet_check( address.port == 40000 );

    rocketnet_address_anonymize( &address );

    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_check( address.data.ipv6[0] == 0x2001 );
    rocketnet_check( address.data.ipv6[1] == 0x0db8 );
    rocketnet_check( address.data.ipv6[2] == 0x85a3 );
    rocketnet_check( address.data.ipv6[3] == 0x0000 );
    rocketnet_check( address.data.ipv6[4] == 0x0000 );
    rocketnet_check( address.data.ipv6[5] == 0x0000 );
    rocketnet_check( address.data.ipv6[6] == 0x0000 );
    rocketnet_check( address.data.ipv6[7] == 0x0000 );
    rocketnet_check( address.port == 0 );
}

void rocketnet_test_platform_inet6_round_trip()
{
    // pins the return convention of the platform pton6/ntop6 pair. the linux
    // ntop6 arm shipped with its NULL check inverted and nothing caught it,
    // because every caller ignored the return and the ipv6 path is lightly
    // exercised. success is true, on every platform, checked here.

    uint16_t address[8];
    rocketnet_check( rocketnet_platform_inet_pton6( "2001:db8:85a3::8a2e:370:7334", address ) );

    char address_string[64];
    rocketnet_check( rocketnet_platform_inet_ntop6( address, address_string, sizeof(address_string) ) );

    uint16_t round_trip[8];
    rocketnet_check( rocketnet_platform_inet_pton6( address_string, round_trip ) );
    rocketnet_check( memcmp( address, round_trip, sizeof(address) ) == 0 );
}

#endif // #if ROCKETNET_PLATFORM_HAS_IPV6

void rocketnet_test_packet_loss_tracker()
{
    rocketnet_packet_loss_tracker_t tracker;
    rocketnet_packet_loss_tracker_reset( &tracker );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    uint64_t sequence = 0;

    for ( int i = 0; i < ROCKETNET_PACKET_LOSS_TRACKER_SAFETY; ++i )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    for ( int i = 0; i < 200; ++i )
    {
        rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    for ( int i = 0; i < 200; ++i )
    {
        if ( sequence & 1 )
        {
            rocketnet_packet_loss_tracker_packet_received( &tracker, sequence );
        }
        sequence++;
    }

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == ( 200 - ROCKETNET_PACKET_LOSS_TRACKER_SAFETY ) / 2 );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );

    rocketnet_packet_loss_tracker_reset( &tracker );

    sequence = 0;

    rocketnet_packet_loss_tracker_packet_received( &tracker, 200 + ROCKETNET_PACKET_LOSS_TRACKER_SAFETY - 1 );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 200 );

    rocketnet_packet_loss_tracker_packet_received( &tracker, 1000 );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) > 500 );

    rocketnet_packet_loss_tracker_packet_received( &tracker, 0xFFFFFFFFFFFFFFFULL );

    rocketnet_check( rocketnet_packet_loss_tracker_update( &tracker ) == 0 );
}

void rocketnet_test_out_of_order_tracker()
{
    rocketnet_out_of_order_tracker_t tracker;
    rocketnet_out_of_order_tracker_reset( &tracker );

    rocketnet_check( tracker.num_out_of_order_packets == 0 );

    uint64_t sequence = 0;

    for ( int i = 0; i < 1000; ++i )
    {
        rocketnet_out_of_order_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( tracker.num_out_of_order_packets == 0 );

    sequence = 500;

    for ( int i = 0; i < 500; ++i )
    {
        rocketnet_out_of_order_tracker_packet_received( &tracker, sequence );
        sequence++;
    }

    rocketnet_check( tracker.num_out_of_order_packets == 499 );

    rocketnet_out_of_order_tracker_reset( &tracker );

    rocketnet_check( tracker.last_packet_processed == 0 );
    rocketnet_check( tracker.num_out_of_order_packets == 0 );

    for ( int i = 0; i < 1000; ++i )
    {
        uint64_t mod_sequence = ( sequence / 2 ) * 2;
        if ( sequence % 2 )
            mod_sequence -= 1;
        rocketnet_out_of_order_tracker_packet_received( &tracker, mod_sequence );
        sequence++;
    }

    rocketnet_check( tracker.num_out_of_order_packets == 500 );
}

void rocketnet_test_jitter_tracker()
{
    rocketnet_jitter_tracker_t tracker;
    rocketnet_jitter_tracker_reset( &tracker );

    rocketnet_check( tracker.jitter == 0.0 );

    uint64_t sequence = 0;

    double t = 0.0;
    double dt = 1.0 / 60.0;

    for ( int i = 0; i < 1000; ++i )
    {
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
        t += dt;
    }

    rocketnet_check( tracker.jitter < 0.000001 );

    for ( int i = 0; i < 1000; ++i )
    {
        t = i * dt;
        if ( (i%3) == 0 )
        {
            t += 2;
        }
        if ( (i%5) == 0 )
        {
            t += 5;
        }
        if ( (i%6) == 0 )
        {
            t -= 10;
        }
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
    }

    rocketnet_check( tracker.jitter > 1.0 );

    rocketnet_jitter_tracker_reset( &tracker );

    rocketnet_check( tracker.jitter == 0.0 );

    for ( int i = 0; i < 1000; ++i )
    {
        t = i * dt;
        if ( (i%3) == 0 )
        {
            t += 0.01f;
        }
        if ( (i%5) == 0 )
        {
            t += 0.05;
        }
        if ( (i%6) == 0 )
        {
            t -= 0.1f;
        }
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
    }

    rocketnet_check( tracker.jitter > 0.05 );
    rocketnet_check( tracker.jitter < 0.1 );

    for ( int i = 0; i < 10000; ++i )
    {
        t = i * dt;
        rocketnet_jitter_tracker_packet_received( &tracker, sequence, t );
        sequence++;
    }

    rocketnet_check( tracker.jitter >= 0.0 );
    rocketnet_check( tracker.jitter <= 0.000001 );
}

extern void * rocketnet_default_malloc_function( void * context, size_t bytes );

extern void rocketnet_default_free_function( void * context, void * p );

static void context_check_free( void * context, void * p )
{
    (void) p;
    rocketnet_check( context );
    rocketnet_check( *((int *)context) == 23 );
    rocketnet_default_free_function( context, p );;
}

#define RUN_TEST( test_function )                                           \
    do                                                                      \
    {                                                                       \
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, #test_function );                 \
        fflush( stdout );                                                   \
        rocketnet_##test_function();                                             \
    }                                                                       \
    while (0)

void rocketnet_run_tests()
{
    // while ( true )
    {
        RUN_TEST( test_time );
        RUN_TEST( test_endian );
        RUN_TEST( test_base64 );
        RUN_TEST( test_hash );
        RUN_TEST( test_copy_string );
        RUN_TEST( test_address );
        RUN_TEST( test_random_bytes );
        RUN_TEST( test_random_float );
        RUN_TEST( test_platform_socket );
        RUN_TEST( test_platform_thread );
        RUN_TEST( test_platform_mutex );
        RUN_TEST( test_value_tracker );
        RUN_TEST( test_packet_filter );
        RUN_TEST( test_basic_packet_filter );
        RUN_TEST( test_packet_filter_no_type_zero_bypass );
        RUN_TEST( test_address_data_ipv4 );
        RUN_TEST( test_anonymize_address_ipv4 );
#if ROCKETNET_PLATFORM_HAS_IPV6
        RUN_TEST( test_anonymize_address_ipv6 );
        RUN_TEST( test_platform_inet6_round_trip );
#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
        RUN_TEST( test_packet_loss_tracker );
        RUN_TEST( test_out_of_order_tracker );
        RUN_TEST( test_jitter_tracker );
    }
}

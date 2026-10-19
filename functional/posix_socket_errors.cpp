/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Datagram weather at the platform socket: the test-only send-failure seam
    reports ENOBUFS and EAGAIN without reaching the kernel, and the pump keeps
    serving after each event. The macOS-only cells (msgx forced portable, the
    transport receive arms, the loopback burst) land here.
*/

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_constants.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)

static void rocketnet_posix_check_impl( bool condition, const char * text, const char * file, int line )
{
    if ( condition )
        return;
    printf( "check failed: ( %s ), file %s, line %d\n", text, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_posix_check( condition ) rocketnet_posix_check_impl( (condition), #condition, __FILE__, __LINE__ )

static bool rocketnet_posix_receive_one( rocketnet_platform_socket_t * socket, uint8_t * data, int capacity, int * bytes )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_address_t from;
        int count = rocketnet_platform_socket_receive_packets( socket, &from, data, capacity, bytes, 1 );
        if ( count > 0 )
            return true;
        rocketnet_platform_sleep( 0.001 );
    }
    return false;
}

static void rocketnet_posix_send_seam_family( const char * bind_string, const char * local_string )
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, bind_string );
    rocketnet_address_parse( &local_address, local_string );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    if ( socket == NULL )
        return;
    local_address.port = bind_address.port;
    local_address.type = bind_address.type;

    const char * pump_before = rocketnet_platform_socket_pump( socket );

    uint8_t packet[64];
    for ( int i = 0; i < (int) sizeof(packet); i++ )
        packet[i] = (uint8_t) ( 0xa0 + i );

    uint8_t receive_data[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;

    // ENOBUFS: the next three sends are dropped, not paused.

    rocketnet_platform_socket_fail_next_sends( 3, ENOBUFS );
    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, (int) sizeof(packet) );
    rocketnet_posix_check( rocketnet_platform_socket_send_failures() == 3 );
    rocketnet_posix_check( rocketnet_platform_socket_receive_packets( socket, &local_address, receive_data, ROCKETNET_RECEIVE_CEILING, &packet_bytes, 1 ) == 0 );
    rocketnet_posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_before ) == 0 );

    // EAGAIN: the next two sends are dropped, not paused.

    rocketnet_platform_socket_fail_next_sends( 2, EAGAIN );
    for ( int i = 0; i < 2; i++ )
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, (int) sizeof(packet) );
    rocketnet_posix_check( rocketnet_platform_socket_send_failures() == 2 );
    rocketnet_posix_check( rocketnet_platform_socket_receive_packets( socket, &local_address, receive_data, ROCKETNET_RECEIVE_CEILING, &packet_bytes, 1 ) == 0 );
    rocketnet_posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_before ) == 0 );

    // traffic resumes on the next send, on the same pump.

    rocketnet_platform_socket_send_packet( socket, &local_address, packet, (int) sizeof(packet) );
    rocketnet_posix_check( rocketnet_posix_receive_one( socket, receive_data, ROCKETNET_RECEIVE_CEILING, &packet_bytes ) );
    rocketnet_posix_check( packet_bytes == (int) sizeof(packet) );
    for ( int i = 0; i < packet_bytes; i++ )
        rocketnet_posix_check( receive_data[i] == packet[i] );
    rocketnet_posix_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_before ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_posix_platform_send_seam()
{
    // the seam is a platform seam: this is its contract, before any
    // transport-level case drives a server or client with it.

    rocketnet_posix_send_seam_family( "0.0.0.0", "127.0.0.1" );
    rocketnet_posix_send_seam_family( "::", "::1" );
}

#endif // #if !defined(_WIN32) && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)

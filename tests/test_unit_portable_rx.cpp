/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "suite_runner.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( __linux__ )
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif // #if defined( __linux__ )

// Uses the private portable transport test-hook build (proofs/tla/RNPortableRx.tla).
// Both cases run on this thread with no socket read, no worker thread and no
// clock: the handoff case drives the FIFO, full, contended, partial and
// oversize transitions; the worker-steps case steps the worker pass through
// a full FIFO, the wake on a return to an empty free FIFO, and stop; the
// chaff case reads through the platform receive seam, with no descriptor.
bool rocketnet_portable_rx_test_handoff();
bool rocketnet_portable_rx_test_worker_steps();
bool rocketnet_portable_rx_test_chaff_drain();

static void test_portable_rx_handoff()
{
    rocketnet_unit_check( rocketnet_portable_rx_test_handoff() );
}

static void test_portable_rx_worker_steps()
{
    rocketnet_unit_check( rocketnet_portable_rx_test_worker_steps() );
}

static void test_portable_rx_chaff_drain()
{
    rocketnet_unit_check( rocketnet_portable_rx_test_chaff_drain() );
}

REGISTER_UNIT_TEST( test_portable_rx_handoff );
REGISTER_UNIT_TEST( test_portable_rx_worker_steps );
REGISTER_UNIT_TEST( test_portable_rx_chaff_drain );

#if defined( __linux__ )

// mutation gap: me rocketnet_platform.cpp:2446 copy_bytes > packet_stride
//
// recvmmsg runs with MSG_TRUNC, so a datagram longer than the stride reports
// its true length, past its slot. The compaction that moves a kept datagram
// down over a dropped one copies at most one stride. The receive seam plays the
// kernel: an empty datagram (dropped, counted), then one of stride + 100 bytes
// whose first stride bytes fill the second slot. The kept copy is the first
// slot; nothing is read past the slots the call was given, and nothing past
// the kept slot is written. A copy of the reported length reads 100 bytes past
// the second slot and writes them over it: the guard slot's bytes appear in
// the second slot (every build), and when the second slot ends the buffer the
// read is a heap-buffer-overflow (the asan + ubsan unit suite legs).

#define PORTABLE_RX_TRUNCATED_STRIDE 256
#define PORTABLE_RX_TRUNCATED_EXTRA 100

struct portable_rx_truncated_script_t
{
    int calls;
};

static int portable_rx_truncated_receive( void * user, void * packet_data, int max_packet_size, struct sockaddr_storage * from )
{
    portable_rx_truncated_script_t * script = (portable_rx_truncated_script_t *) user;
    const int index = script->calls++;
    if ( index >= 2 )
    {
        return -1;
    }

    struct sockaddr_in6 * in6 = (struct sockaddr_in6 *) from;
    memset( in6, 0, sizeof( *in6 ) );
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40001 );
    in6->sin6_addr.s6_addr[15] = 1;

    if ( index == 0 )
    {
        return 0;
    }

    memset( packet_data, 0x5A, (size_t) max_packet_size );
    return max_packet_size + PORTABLE_RX_TRUNCATED_EXTRA;
}

static int portable_rx_truncated_read( uint8_t * packet_data, rocketnet_address_t * from, int * packet_bytes )
{
    struct rocketnet_platform_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.ipv6 = true;
    socket.handle = -1;

    portable_rx_truncated_script_t script = { 0 };
    rocketnet_platform_socket_set_receive_hook( &socket, portable_rx_truncated_receive, &script );
    const int received = rocketnet_platform_socket_receive_packets_mmsg( &socket, from, packet_data, PORTABLE_RX_TRUNCATED_STRIDE, packet_bytes, 2 );
    rocketnet_platform_socket_set_receive_hook( &socket, NULL, NULL );

    rocketnet_unit_check( script.calls == 2 );

    return received;
}

static bool portable_rx_all( const uint8_t * bytes, int count, uint8_t value )
{
    for ( int i = 0; i < count; ++i )
    {
        if ( bytes[i] != value )
        {
            return false;
        }
    }

    return true;
}

static void test_portable_rx_truncated_datagram_moves_one_stride()
{
    const int stride = PORTABLE_RX_TRUNCATED_STRIDE;
    static uint8_t packet_data[3 * PORTABLE_RX_TRUNCATED_STRIDE];
    memset( packet_data, 0xC3, sizeof( packet_data ) );
    memset( packet_data + 2 * stride, 0xEE, (size_t) stride );

    rocketnet_address_t from[2];
    int packet_bytes[2] = { -1, -1 };
    rocketnet_unit_check( portable_rx_truncated_read( packet_data, from, packet_bytes ) == 1 );
    rocketnet_unit_check( packet_bytes[0] == stride + PORTABLE_RX_TRUNCATED_EXTRA );
    rocketnet_unit_check( from[0].type == ROCKETNET_ADDRESS_IPV6 && from[0].port == 40001 );
    rocketnet_unit_check( portable_rx_all( packet_data, stride, 0x5A ) );

    const bool second_slot_kept = portable_rx_all( packet_data + stride, stride, 0x5A );
    if ( !second_slot_kept )
    {
        printf( "fault: the compaction of a truncated datagram moved more than one stride: the bytes past its slot were copied over the next slot\n" );
    }
    rocketnet_unit_check( second_slot_kept );
    rocketnet_unit_check( portable_rx_all( packet_data + 2 * stride, stride, 0xEE ) );
}

static void test_portable_rx_truncated_datagram_in_the_last_slot_reads_inside_the_buffer()
{
    // exactly the two slots the call is given, on the heap: under ASan a copy
    // past the second slot is a heap-buffer-overflow at this read

    const int stride = PORTABLE_RX_TRUNCATED_STRIDE;
    uint8_t * packet_data = (uint8_t *) malloc( (size_t) ( 2 * stride ) );
    rocketnet_unit_check( packet_data != NULL );
    memset( packet_data, 0xC3, (size_t) ( 2 * stride ) );

    rocketnet_address_t from[2];
    int packet_bytes[2] = { -1, -1 };
    rocketnet_unit_check( portable_rx_truncated_read( packet_data, from, packet_bytes ) == 1 );
    rocketnet_unit_check( packet_bytes[0] == stride + PORTABLE_RX_TRUNCATED_EXTRA );
    rocketnet_unit_check( portable_rx_all( packet_data, stride, 0x5A ) );

    free( packet_data );
}

REGISTER_UNIT_TEST( test_portable_rx_truncated_datagram_moves_one_stride );
REGISTER_UNIT_TEST( test_portable_rx_truncated_datagram_in_the_last_slot_reads_inside_the_buffer );

#endif // #if defined( __linux__ )

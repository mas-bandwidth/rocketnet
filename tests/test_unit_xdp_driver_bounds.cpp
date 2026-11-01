/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP transport's bounds (sdk/rocketnet_xdp_frame.h, TRANSPORT BOUNDS), over
// plain heap buffers sized exactly to a model UMEM, with no AF_XDP socket. A
// send length is checked before the copy into its frame, a receive
// descriptor before its frame is read, a cpu before CPU_SET and the possible
// cpu count before the stats read. Each case follows the transport's own order
// (sdk/rocketnet_server_socket_linux.cpp, the xdp thread's send and receive
// loops): with a check removed, the copy that follows runs past the buffer,
// which ASan reports.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_xdp_frame.h"

// the AF_XDP code is Linux only, so its tests are built there only
#if defined( __linux__ )

static const uint8_t bounds_server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t bounds_gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

// the send loop: copy the staged packet into its frame, then build the
// headers in front of it. a refused length frees the frame unbuilt.

static int bounds_send( uint8_t * frame, const uint8_t * staged, size_t packet_bytes, uint64_t * refused )
{
    rocketnet_address_t source, destination;
    rocketnet_address_parse( &source, "192.168.1.10:40000" );
    rocketnet_address_parse( &destination, "192.168.1.20:40001" );

    const int copied = rocketnet_xdp_send_copy( frame, ROCKETNET_XDP_FRAME_SIZE, ROCKETNET_ADDRESS_IPV4, staged, packet_bytes, refused );
    if ( copied < 0 )
        return -1;
    return rocketnet_xdp_frame_build( frame, ROCKETNET_XDP_FRAME_SIZE, bounds_server_mac, bounds_gateway_mac, &source, &destination, copied );
}

static void test_xdp_transport_send_copy_refuses_bad_lengths()
{
    uint8_t * frame = (uint8_t *) malloc( ROCKETNET_XDP_FRAME_SIZE );
    uint8_t * staged = (uint8_t *) malloc( ROCKETNET_XDP_FRAME_SIZE );
    rocketnet_unit_check( frame && staged );

    memset( frame, 0, ROCKETNET_XDP_FRAME_SIZE );
    memset( staged, 0xA5, ROCKETNET_XDP_FRAME_SIZE );

    uint64_t refused = 0;

    // a negative int length as the transport stores it (packet_info.packet_bytes is a size_t)

    rocketnet_unit_check( bounds_send( frame, staged, (size_t) (int) -1, &refused ) == -1 );
    rocketnet_unit_check( refused == 1 );

    // one past the ceiling

    rocketnet_unit_check( bounds_send( frame, staged, (size_t) ROCKETNET_RECEIVE_CEILING + 1, &refused ) == -1 );
    rocketnet_unit_check( refused == 2 );

    // past the frame itself

    rocketnet_unit_check( bounds_send( frame, staged, (size_t) ROCKETNET_XDP_FRAME_SIZE * 2, &refused ) == -1 );
    rocketnet_unit_check( refused == 3 );

    // zero

    rocketnet_unit_check( bounds_send( frame, staged, 0, &refused ) == -1 );
    rocketnet_unit_check( refused == 4 );

    // the ceiling itself is copied and built

    const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( bounds_send( frame, staged, (size_t) ROCKETNET_RECEIVE_CEILING, &refused ) == header_bytes + ROCKETNET_RECEIVE_CEILING );
    rocketnet_unit_check( refused == 4 );
    rocketnet_unit_check( memcmp( frame + header_bytes, staged, ROCKETNET_RECEIVE_CEILING ) == 0 );

    free( staged );
    free( frame );
}

// the receive loop over a model UMEM of two frames whose receive region is
// the second: check the descriptor, run the codec, copy the payload into a
// frame-sized receive slot. returns the payload bytes copied, or -1.

#define BOUNDS_UMEM_BYTES       ( 2 * ROCKETNET_XDP_FRAME_SIZE )
#define BOUNDS_REGION_BEGIN     ( (uint64_t) ROCKETNET_XDP_FRAME_SIZE )
#define BOUNDS_REGION_END       ( (uint64_t) 2 * ROCKETNET_XDP_FRAME_SIZE )

static int bounds_receive( const uint8_t * umem, uint64_t addr, uint32_t len, struct rocketnet_xdp_receive_drops_t * drops, uint8_t * slot )
{
    const uint8_t * packet_data = rocketnet_xdp_receive_descriptor( umem, BOUNDS_REGION_BEGIN, BOUNDS_REGION_END, addr, len, drops );
    if ( !packet_data )
        return -1;

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    if ( !rocketnet_xdp_frame_receive( packet_data, (size_t) len, ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_bytes, drops ) )
        return -1;
    memcpy( slot, payload, (size_t) payload_bytes );
    return payload_bytes;
}

// writes an IPv4 server-bound frame at umem + addr whose IP and UDP lengths
// name payload_bytes of payload, so the codec takes it and the copy runs to
// the end of those bytes (the frame is built at the smallest server packet
// and its length fields are then set, since rocketnet_xdp_frame_build stops
// at the path MTU)

static void bounds_write_frame( uint8_t * umem, uint64_t addr, int payload_bytes )
{
    rocketnet_address_t source, destination;
    rocketnet_address_parse( &source, "192.168.1.20:40001" );
    rocketnet_address_parse( &destination, "192.168.1.10:40000" );

    const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV4 );
    memset( umem + addr + header_bytes, 0x3C, (size_t) payload_bytes );
    rocketnet_unit_check( rocketnet_xdp_frame_build( umem + addr, ROCKETNET_XDP_FRAME_SIZE - ( addr % ROCKETNET_XDP_FRAME_SIZE ), bounds_gateway_mac, bounds_server_mac, &source, &destination, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES ) == header_bytes + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );

    const int ip_bytes = 20 + 8 + payload_bytes;
    const int udp_bytes = 8 + payload_bytes;
    uint8_t * f = umem + addr;
    f[16] = (uint8_t) ( ip_bytes >> 8 );
    f[17] = (uint8_t) ip_bytes;
    f[header_bytes - 4] = (uint8_t) ( udp_bytes >> 8 );
    f[header_bytes - 3] = (uint8_t) udp_bytes;
}

static void test_xdp_transport_receive_descriptor_refuses_bad_descriptors()
{
    uint8_t * umem = (uint8_t *) malloc( BOUNDS_UMEM_BYTES );
    uint8_t * slot = (uint8_t *) malloc( ROCKETNET_XDP_FRAME_SIZE );
    rocketnet_unit_check( umem && slot );

    memset( umem, 0, BOUNDS_UMEM_BYTES );

    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );

    const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV4 );

    // longer than a frame, at the frame boundary

    bounds_write_frame( umem, BOUNDS_REGION_BEGIN, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    rocketnet_unit_check( bounds_receive( umem, BOUNDS_REGION_BEGIN, ROCKETNET_XDP_FRAME_SIZE + 1, &drops, slot ) == -1 );
    rocketnet_unit_check( drops.descriptor == 1 );

    // within a frame's length, but at an offset that carries it past its frame's end

    const uint64_t offset_addr = BOUNDS_REGION_BEGIN + 256;
    bounds_write_frame( umem, offset_addr, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    rocketnet_unit_check( bounds_receive( umem, offset_addr, ROCKETNET_XDP_FRAME_SIZE - 100, &drops, slot ) == -1 );
    rocketnet_unit_check( drops.descriptor == 2 );

    // below the receive region (a send frame) and past it

    rocketnet_unit_check( bounds_receive( umem, 0, 100, &drops, slot ) == -1 );
    rocketnet_unit_check( drops.descriptor == 3 );
    rocketnet_unit_check( bounds_receive( umem, BOUNDS_REGION_END, 100, &drops, slot ) == -1 );
    rocketnet_unit_check( drops.descriptor == 4 );

    rocketnet_unit_check( drops.not_ip == 0 && drops.ipv6_extension == 0 && drops.other_family == 0 && drops.malformed == 0 );

    // the same headroom offset with a length ending exactly at its frame's end
    // is taken, and the copy runs to the frame's last byte (the frame's UDP
    // length names every byte up to it)

    const uint32_t fitting = (uint32_t) ( ROCKETNET_XDP_FRAME_SIZE - 256 );
    bounds_write_frame( umem, offset_addr, (int) fitting - header_bytes );
    rocketnet_unit_check( bounds_receive( umem, offset_addr, fitting, &drops, slot ) == (int) fitting - header_bytes );
    rocketnet_unit_check( drops.descriptor == 4 );

    // and a whole frame at its boundary

    bounds_write_frame( umem, BOUNDS_REGION_BEGIN, ROCKETNET_XDP_FRAME_SIZE - header_bytes );
    rocketnet_unit_check( bounds_receive( umem, BOUNDS_REGION_BEGIN, ROCKETNET_XDP_FRAME_SIZE, &drops, slot ) == ROCKETNET_XDP_FRAME_SIZE - header_bytes );
    rocketnet_unit_check( drops.descriptor == 4 && drops.malformed == 0 );

    free( slot );
    free( umem );
}

// the server's UMEM: send frames in the first half, receive frames in the
// second (sdk/rocketnet_server_socket_linux.cpp); only a receive frame goes
// back to the fill ring, at its frame boundary.
// ROCKETNET_XDP_NUM_FRAMES exists only when the XDP transport is compiled in
// (sdk/rocketnet_constants.h). The frame codec above stays portable.

#if ROCKETNET_XDP

static void test_xdp_transport_refused_descriptor_not_refilled()
{
    const uint64_t frame_bytes = (uint64_t) ROCKETNET_XDP_FRAME_SIZE;
    const uint64_t receive_begin = (uint64_t) ROCKETNET_XDP_NUM_FRAMES / 2 * frame_bytes;
    const uint64_t receive_end = (uint64_t) ROCKETNET_XDP_NUM_FRAMES * frame_bytes;

    // a receive-region address carrying the headroom offset: returned, aligned

    uint64_t frame_addr = 0;
    rocketnet_unit_check( rocketnet_xdp_fill_return( receive_begin + frame_bytes + 256, receive_begin, receive_end, &frame_addr ) );
    rocketnet_unit_check( frame_addr == receive_begin + frame_bytes );

    // the receive region's first and last frames: returned

    rocketnet_unit_check( rocketnet_xdp_fill_return( receive_begin, receive_begin, receive_end, &frame_addr ) );
    rocketnet_unit_check( frame_addr == receive_begin );
    rocketnet_unit_check( rocketnet_xdp_fill_return( receive_end - frame_bytes + 64, receive_begin, receive_end, &frame_addr ) );
    rocketnet_unit_check( frame_addr == receive_end - frame_bytes );

    // the first send-region frame, the send region's last frame and an
    // address past the UMEM: never returned, and the frame address is untouched

    frame_addr = 1;
    rocketnet_unit_check( !rocketnet_xdp_fill_return( 0, receive_begin, receive_end, &frame_addr ) );
    rocketnet_unit_check( !rocketnet_xdp_fill_return( receive_begin - frame_bytes + 256, receive_begin, receive_end, &frame_addr ) );
    rocketnet_unit_check( !rocketnet_xdp_fill_return( receive_end, receive_begin, receive_end, &frame_addr ) );
    rocketnet_unit_check( !rocketnet_xdp_fill_return( receive_end + 256, receive_begin, receive_end, &frame_addr ) );
    rocketnet_unit_check( frame_addr == 1 );

    rocketnet_unit_check( !rocketnet_xdp_fill_return( receive_begin, receive_begin, receive_end, NULL ) );
}

#endif // #if ROCKETNET_XDP

static void test_xdp_transport_pin_cpu_refuses_outside_the_set()
{
    rocketnet_unit_check( !rocketnet_xdp_cpu_in_set( -1, 1024 ) );
    rocketnet_unit_check( !rocketnet_xdp_cpu_in_set( 1024, 1024 ) );
    rocketnet_unit_check( !rocketnet_xdp_cpu_in_set( 0x7FFFFFFF, 1024 ) );
    rocketnet_unit_check( rocketnet_xdp_cpu_in_set( 0, 1024 ) );
    rocketnet_unit_check( rocketnet_xdp_cpu_in_set( 1023, 1024 ) );
}

static void test_xdp_transport_preflight_refuses_pin_targets_outside_isolated()
{
    int first_missing = -1;

    // the audit's example: two OS cpus, seven queues and isolcpus=1-7 on an
    // eight-cpu machine pass the old count-only check, but the last thread
    // targets cpu 8

    rocketnet_unit_check( !rocketnet_xdp_pin_targets_isolated( "1-7\n", 2, 7, &first_missing ) );
    rocketnet_unit_check( first_missing == 8 );

    // a layout that holds every pin target: 2 .. 8

    rocketnet_unit_check( rocketnet_xdp_pin_targets_isolated( "2-8\n", 2, 7, &first_missing ) );
    rocketnet_unit_check( first_missing == -1 );

    // a gap at 5

    rocketnet_unit_check( !rocketnet_xdp_pin_targets_isolated( "2-4,6-8", 2, 7, &first_missing ) );
    rocketnet_unit_check( first_missing == 5 );

    // no list, a malformed list: neither verifies

    rocketnet_unit_check( !rocketnet_xdp_pin_targets_isolated( "", 2, 7, &first_missing ) );
    rocketnet_unit_check( first_missing == 2 );
    rocketnet_unit_check( !rocketnet_xdp_pin_targets_isolated( "2-8x", 2, 7, &first_missing ) );
    rocketnet_unit_check( first_missing == 2 );

    // a cpu number past int is refused, not wrapped: the largest int parses,
    // one more does not

    rocketnet_unit_check( !rocketnet_xdp_pin_targets_isolated( "2-2147483648", 2, 7, &first_missing ) );
    rocketnet_unit_check( first_missing == 2 );
    rocketnet_unit_check( rocketnet_xdp_pin_targets_isolated( "2-2147483647", 2, 7, &first_missing ) );
    rocketnet_unit_check( first_missing == -1 );
}

static void test_xdp_transport_stats_cpus_are_bounded()
{
    rocketnet_unit_check( !rocketnet_xdp_stats_cpus_fit( -1 ) );
    rocketnet_unit_check( !rocketnet_xdp_stats_cpus_fit( 0 ) );
    rocketnet_unit_check( !rocketnet_xdp_stats_cpus_fit( ROCKETNET_XDP_STATS_MAX_CPUS + 1 ) );
    rocketnet_unit_check( rocketnet_xdp_stats_cpus_fit( 1 ) );
    rocketnet_unit_check( rocketnet_xdp_stats_cpus_fit( ROCKETNET_XDP_STATS_MAX_CPUS ) );
}

// a failed stats read publishes the last good sums, never zeros, and reports
// only the read that starts a run of failures; a good read ends the run

static void test_xdp_transport_stats_failed_read_keeps_last()
{
    struct rocketnet_server_xdp_stats_t values[2];
    memset( values, 0, sizeof( values ) );
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
    {
        values[0].counters[i] = (uint64_t) ( i + 1 );
        values[1].counters[i] = (uint64_t) ( 100 * ( i + 1 ) );
    }

    uint64_t last[ROCKETNET_SERVER_XDP_NUM_COUNTERS] = {};
    bool failing = false;
    uint64_t counters[ROCKETNET_SERVER_XDP_NUM_COUNTERS];

    rocketnet_unit_check( !rocketnet_xdp_stats_publish( true, values, 2, last, &failing, counters ) );
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        rocketnet_unit_check( counters[i] == (uint64_t) ( 101 * ( i + 1 ) ) );

    // the lookup fails twice: the sums stand and only the first is reported

    memset( values, 0xFF, sizeof( values ) );
    memset( counters, 0, sizeof( counters ) );
    rocketnet_unit_check( rocketnet_xdp_stats_publish( false, values, 2, last, &failing, counters ) );
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        rocketnet_unit_check( counters[i] == (uint64_t) ( 101 * ( i + 1 ) ) );
    rocketnet_unit_check( !rocketnet_xdp_stats_publish( false, values, 2, last, &failing, counters ) );
    rocketnet_unit_check( counters[0] == 101 );

    // a cpu count that does not fit is a failed read too

    rocketnet_unit_check( !rocketnet_xdp_stats_publish( true, values, ROCKETNET_XDP_STATS_MAX_CPUS + 1, last, &failing, counters ) );
    rocketnet_unit_check( counters[0] == 101 );

    // a good read ends the run and replaces the sums; the next failure is
    // reported again

    memset( values, 0, sizeof( values ) );
    values[0].counters[0] = 7;
    rocketnet_unit_check( !rocketnet_xdp_stats_publish( true, values, 1, last, &failing, counters ) );
    rocketnet_unit_check( counters[0] == 7 && counters[1] == 0 );
    rocketnet_unit_check( rocketnet_xdp_stats_publish( false, values, 1, last, &failing, counters ) );
    rocketnet_unit_check( counters[0] == 7 );
}

static void test_xdp_transport_receive_drain_rotates_first_queue()
{
    rocketnet_unit_check( rocketnet_xdp_drain_queue( 0, 0, 1 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_drain_queue( 0, 1, 1 ) == 0 );

    rocketnet_unit_check( rocketnet_xdp_drain_queue( 0, 0, 2 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_drain_queue( 0, 1, 2 ) == 1 );
    rocketnet_unit_check( rocketnet_xdp_drain_queue( 1, 1, 2 ) == 0 );

    for ( int step = 0; step < 7; step++ )
    {
        rocketnet_unit_check( rocketnet_xdp_drain_queue( 0, step, 7 ) == step );
        rocketnet_unit_check( rocketnet_xdp_drain_queue( 3, step, 7 ) == ( 3 + step ) % 7 );
    }

    // three queues, each holding more than one call's budget. The call that
    // starts at S fills on S, so the next call starts at S + 1. Over three
    // calls every queue is first once.

    int start = 0;
    int served_first[3] = { 0, 0, 0 };

    for ( int call = 0; call < 3; call++ )
    {
        const int first = rocketnet_xdp_drain_queue( start, 0, 3 );
        rocketnet_unit_check( first >= 0 && first < 3 );

        served_first[first] += 1;
        start = rocketnet_xdp_drain_queue( first, 1, 3 );
    }

    rocketnet_unit_check( served_first[0] == 1 && served_first[1] == 1 && served_first[2] == 1 );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_drain_rotates_first_queue );
REGISTER_UNIT_TEST( test_xdp_transport_send_copy_refuses_bad_lengths );
REGISTER_UNIT_TEST( test_xdp_transport_receive_descriptor_refuses_bad_descriptors );
#if ROCKETNET_XDP
REGISTER_UNIT_TEST( test_xdp_transport_refused_descriptor_not_refilled );
#endif // #if ROCKETNET_XDP
REGISTER_UNIT_TEST( test_xdp_transport_pin_cpu_refuses_outside_the_set );
REGISTER_UNIT_TEST( test_xdp_transport_preflight_refuses_pin_targets_outside_isolated );
REGISTER_UNIT_TEST( test_xdp_transport_stats_cpus_are_bounded );
REGISTER_UNIT_TEST( test_xdp_transport_stats_failed_read_keeps_last );

#endif // #if defined(__linux__)

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The IOCP server's send lane of a destination (rocketnet_iocp_lane_index,
// proofs/tla/RNIocpSendLanes.tla DestLane): equal addresses always share a
// lane, every lane is in range, and a thousand players spread evenly over the
// lanes whether they differ by address or by port. Pure: no socket, no
// thread, no time. The hash compiles in every development build, so these run
// on Linux and macOS too.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_server_iocp.h"

#include <string.h>

#if ROCKETNET_DEV

int rocketnet_server_iocp_test_lane_index( const rocketnet_address_t * to, int lanes );
int rocketnet_server_iocp_test_send_thread_lanes( int thread, int threads, int lanes, int * owned );

// Equal addresses share a lane even when the bytes rocketnet_address_equal
// does not compare differ: an ipv4 address in a union whose ipv6 tail holds
// other bytes, and an ipv6 address the same.

static void test_iocp_send_lane_equal_addresses_share_a_lane()
{
    rocketnet_address_t a;
    rocketnet_address_t b;

    memset( &a, 0x00, sizeof( a ) );
    memset( &b, 0xFF, sizeof( b ) );
    rocketnet_unit_check( rocketnet_address_parse( &a, "172.31.91.165:40123" ) );
    b.type = a.type;
    memcpy( b.data.ipv4, a.data.ipv4, sizeof( a.data.ipv4 ) );
    b.port = a.port;
    rocketnet_unit_check( rocketnet_address_equal( &a, &b ) );
    for ( int lanes = 1; lanes <= ROCKETNET_IOCP_MAX_LANES; ++lanes )
    {
        rocketnet_unit_check( rocketnet_server_iocp_test_lane_index( &a, lanes ) == rocketnet_server_iocp_test_lane_index( &b, lanes ) );
    }

    memset( &a, 0x00, sizeof( a ) );
    memset( &b, 0xFF, sizeof( b ) );
    rocketnet_unit_check( rocketnet_address_parse( &a, "[2001:db8::7]:40123" ) );
    b.type = a.type;
    memcpy( b.data.ipv6, a.data.ipv6, sizeof( a.data.ipv6 ) );
    b.port = a.port;
    rocketnet_unit_check( rocketnet_address_equal( &a, &b ) );
    for ( int lanes = 1; lanes <= ROCKETNET_IOCP_MAX_LANES; ++lanes )
    {
        rocketnet_unit_check( rocketnet_server_iocp_test_lane_index( &a, lanes ) == rocketnet_server_iocp_test_lane_index( &b, lanes ) );
    }
}

REGISTER_UNIT_TEST( test_iocp_send_lane_equal_addresses_share_a_lane );

// One lane is lane 0; every lane is in range.

static void test_iocp_send_lane_in_range()
{
    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "10.0.0.1:40000" ) );
    rocketnet_unit_check( rocketnet_server_iocp_test_lane_index( &address, 1 ) == 0 );
    rocketnet_unit_check( rocketnet_server_iocp_test_lane_index( &address, 0 ) == 0 );
    for ( int port = 40000; port < 40256; ++port )
    {
        address.port = (uint16_t) port;
        for ( int lanes = 1; lanes <= ROCKETNET_IOCP_MAX_LANES; ++lanes )
        {
            const int lane = rocketnet_server_iocp_test_lane_index( &address, lanes );
            rocketnet_unit_check( lane >= 0 && lane < lanes );
        }
    }
}

REGISTER_UNIT_TEST( test_iocp_send_lane_in_range );

// A thousand players over the lanes the sizing gives at 1000 clients, and
// over 4 and 16: every lane holds within a quarter of the mean, players on a
// thousand addresses at one port and players on one address at a thousand
// ports alike (one address behind a NAT, or a load row's one blaster).

static void test_iocp_send_lane_spreads_players()
{
    const int players = 1000;
    const int lane_counts[] = { 4, rocketnet_server_iocp_send_lanes( players ), ROCKETNET_IOCP_MAX_LANES };
    for ( int c = 0; c < (int) ( sizeof( lane_counts ) / sizeof( lane_counts[0] ) ); ++c )
    {
        const int lanes = lane_counts[c];
        int by_address[ROCKETNET_IOCP_MAX_LANES] = {};
        int by_port[ROCKETNET_IOCP_MAX_LANES] = {};
        for ( int i = 0; i < players; ++i )
        {
            rocketnet_address_t address;
            memset( &address, 0, sizeof( address ) );
            address.type = ROCKETNET_ADDRESS_IPV4;
            address.data.ipv4[0] = 10;
            address.data.ipv4[1] = 0;
            address.data.ipv4[2] = (uint8_t) ( i / 250 );
            address.data.ipv4[3] = (uint8_t) ( i % 250 );
            address.port = 40000;
            by_address[rocketnet_server_iocp_test_lane_index( &address, lanes )]++;

            address.data.ipv4[0] = 172;
            address.data.ipv4[1] = 31;
            address.data.ipv4[2] = 91;
            address.data.ipv4[3] = 165;
            address.port = (uint16_t) ( 40000 + i );
            by_port[rocketnet_server_iocp_test_lane_index( &address, lanes )]++;
        }
        const int mean = players / lanes;
        for ( int lane = 0; lane < lanes; ++lane )
        {
            rocketnet_unit_check( by_address[lane] * 4 >= mean * 3 && by_address[lane] * 4 <= mean * 5 );
            rocketnet_unit_check( by_port[lane] * 4 >= mean * 3 && by_port[lane] * 4 <= mean * 5 );
        }
    }
}

REGISTER_UNIT_TEST( test_iocp_send_lane_spreads_players );

// The lane sizing at every client count a build can size: a lane holds every
// client slot plus the headroom, so the protocol reserve (which counts each
// slot not connected in every lane) always leaves it fill; the lanes'
// operations fit the packet handle's sixteen index bits; 1000 clients is 8
// lanes of 4096. Before the floor, a lane of 4096 at 5000 clients was all
// reserve.

static void test_iocp_send_lane_sizing_holds_the_reserve()
{
    rocketnet_unit_check( rocketnet_server_iocp_lanes_for( 1000 ) == 8 );
    rocketnet_unit_check( rocketnet_server_iocp_lane_size_for( 1000 ) == 4096 );
    rocketnet_unit_check( rocketnet_server_iocp_lanes_for( 10000 ) == 4 );
    rocketnet_unit_check( rocketnet_server_iocp_lane_size_for( 10000 ) == 16384 );

    for ( int sized = 1; sized <= ROCKETNET_IOCP_MAX_OPERATIONS - ROCKETNET_IOCP_LANE_HEADROOM; sized += 37 )
    {
        const int lanes = rocketnet_server_iocp_lanes_for( sized );
        const int size = rocketnet_server_iocp_lane_size_for( sized );
        rocketnet_unit_check( lanes >= 1 && lanes <= ROCKETNET_IOCP_MAX_LANES );
        rocketnet_unit_check( size > 0 && ( size & ( size - 1 ) ) == 0 );
        rocketnet_unit_check( size >= sized + ROCKETNET_IOCP_LANE_HEADROOM );
        rocketnet_unit_check( (int64_t) lanes * size <= ROCKETNET_IOCP_MAX_OPERATIONS );
    }
}

REGISTER_UNIT_TEST( test_iocp_send_lane_sizing_holds_the_reserve );

// Which send thread owns which lane (rocketnet_server_iocp_send_thread_*, the
// arithmetic the transport's send threads, proofs/tla/RNIocpSendLanes.tla and
// the iocp-lanes trace harness share): the thread count is the constant or
// the lanes when fewer; lane l belongs to thread l % threads; a thread's walk
// takes exactly the lanes it owns, in order, and the walks of all threads
// cover every lane once, so none is starved and none is posted by two.

static void test_iocp_send_thread_owns_lanes_by_modulo()
{
    rocketnet_unit_check( ROCKETNET_IOCP_SEND_THREADS == 2 );
    rocketnet_unit_check( rocketnet_server_iocp_send_threads( 1 ) == 1 );
    rocketnet_unit_check( rocketnet_server_iocp_send_threads( 2 ) == 2 );
    rocketnet_unit_check( rocketnet_server_iocp_send_threads( 8 ) == 2 );
    rocketnet_unit_check( rocketnet_server_iocp_send_threads( ROCKETNET_IOCP_MAX_LANES ) == 2 );

    for ( int lanes = 1; lanes <= ROCKETNET_IOCP_MAX_LANES; ++lanes )
    {
        const int threads = rocketnet_server_iocp_send_threads( lanes );
        int owners[ROCKETNET_IOCP_MAX_LANES];
        for ( int l = 0; l < lanes; ++l )
        {
            owners[l] = 0;
        }

        for ( int t = 0; t < threads; ++t )
        {
            int owned[ROCKETNET_IOCP_MAX_LANES];
            const int count = rocketnet_server_iocp_test_send_thread_lanes( t, threads, lanes, owned );
            rocketnet_unit_check( count >= 1 );
            for ( int k = 0; k < count; ++k )
            {
                rocketnet_unit_check( owned[k] == t + k * threads );
                rocketnet_unit_check( owned[k] < lanes );
                rocketnet_unit_check( rocketnet_server_iocp_send_thread_of_lane( owned[k], threads ) == t );
                owners[owned[k]]++;
            }
            rocketnet_unit_check( t + count * threads >= lanes );
        }

        for ( int l = 0; l < lanes; ++l )
        {
            rocketnet_unit_check( owners[l] == 1 );
        }
    }

    // by value at the measured layout: 8 lanes, thread 0 takes 0 2 4 6, thread 1 takes 1 3 5 7
    const int expected_lanes[2][4] = { { 0, 2, 4, 6 }, { 1, 3, 5, 7 } };
    for ( int t = 0; t < 2; ++t )
    {
        int owned[ROCKETNET_IOCP_MAX_LANES];
        rocketnet_unit_check( rocketnet_server_iocp_test_send_thread_lanes( t, 2, 8, owned ) == 4 );
        for ( int k = 0; k < 4; ++k )
        {
            rocketnet_unit_check( owned[k] == expected_lanes[t][k] );
        }
    }
}

REGISTER_UNIT_TEST( test_iocp_send_thread_owns_lanes_by_modulo );

#endif // #if ROCKETNET_DEV

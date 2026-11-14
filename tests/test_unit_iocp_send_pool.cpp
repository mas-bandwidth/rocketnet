/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The iocp send operation pool by value: ROCKETNET_IOCP_SEND_PACKETS_PER_CLIENT
// (64) operations for every sized client, in lanes of at most
// ROCKETNET_IOCP_LANE_PACKETS, all within the 65536 the packet handle's index
// bits name. An operation holds its slot from its start through its post to
// its completion, so the pool holds one update's sends and the sends of the
// update before still in flight; at 32 a client the peak row (1000 players,
// 15 messages a player an update) refused starts with no network thread
// busy, at 64 it refused none (2026-11-14, AWS m7i.16xlarge). Every size is
// at least the pool 32 a client gave, so no count lost operations; past 1024
// clients the 65536 cap holds both. Pure sizing: no socket, no OS call.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_iocp.h"

// the pool 32 a client gave, the same arithmetic with the old figure: the
// floor every size keeps
static int iocp_send_pool_at_32( int sized )
{
    const int packets = 32 * sized;
    int lanes = ( packets + ROCKETNET_IOCP_LANE_PACKETS - 1 ) / ROCKETNET_IOCP_LANE_PACKETS;
    lanes = lanes < 1 ? 1 : ( lanes > ROCKETNET_IOCP_MAX_LANES ? ROCKETNET_IOCP_MAX_LANES : lanes );
    int size = rocketnet_power_of_two_at_least( ( packets + lanes - 1 ) / lanes );
    size = size < ROCKETNET_IOCP_LANE_PACKETS ? size : ROCKETNET_IOCP_LANE_PACKETS;
    const int floor = rocketnet_power_of_two_at_least( sized + ROCKETNET_IOCP_LANE_HEADROOM );
    size = size > floor ? size : floor;
    lanes = ( packets + ROCKETNET_IOCP_LANE_PACKETS - 1 ) / ROCKETNET_IOCP_LANE_PACKETS;
    lanes = lanes > ROCKETNET_IOCP_MAX_LANES ? ROCKETNET_IOCP_MAX_LANES : lanes;
    lanes = lanes > ROCKETNET_IOCP_MAX_OPERATIONS / size ? ROCKETNET_IOCP_MAX_OPERATIONS / size : lanes;
    lanes = lanes < 1 ? 1 : lanes;
    return lanes * size;
}

static void test_iocp_send_pool_is_64_a_client_within_the_handle()
{
    rocketnet_unit_check( ROCKETNET_IOCP_SEND_PACKETS_PER_CLIENT == 64 );

    struct
    {
        int sized;
        int lanes;
        int lane_size;
        int pool_at_32;
    } cases[] = {
        { 8, 1, 2048, 2048 },           // every slot plus the headroom, as before
        { 512, 8, 4096, 16384 },        // 64 a client, twice the 32 before
        { 1000, 16, 4096, 32768 },      // the peak row's count: 65.5 a client
        { 1024, 16, 4096, 32768 },      // 64 a client, the cap reached exactly
        { 2048, 16, 4096, 65536 },      // the cap: 32 a client, as before
        { 32768, 1, 65536, 65536 },     // one lane of every slot: 2 a client, as before
    };
    for ( const auto & c : cases )
    {
        const int lanes = rocketnet_server_iocp_lanes_for( c.sized );
        const int lane_size = rocketnet_server_iocp_lane_size_for( c.sized );
        rocketnet_unit_check( lanes == c.lanes );
        rocketnet_unit_check( lane_size == c.lane_size );
        rocketnet_unit_check( iocp_send_pool_at_32( c.sized ) == c.pool_at_32 );
        rocketnet_unit_check( lanes * lane_size >= c.pool_at_32 );
        rocketnet_unit_check( lanes * lane_size <= ROCKETNET_IOCP_MAX_OPERATIONS );
        rocketnet_unit_check( lane_size >= c.sized + ROCKETNET_IOCP_LANE_HEADROOM );
    }

    // every count from 1 to the handle's limit: never fewer operations than
    // 32 a client gave, never more than the handle names
    for ( int sized = 1; sized + ROCKETNET_IOCP_LANE_HEADROOM <= ROCKETNET_IOCP_MAX_OPERATIONS; ++sized )
    {
        const int pool = rocketnet_server_iocp_lanes_for( sized ) * rocketnet_server_iocp_lane_size_for( sized );
        rocketnet_unit_check( pool >= iocp_send_pool_at_32( sized ) );
        rocketnet_unit_check( pool <= ROCKETNET_IOCP_MAX_OPERATIONS );
    }
}

REGISTER_UNIT_TEST( test_iocp_send_pool_is_64_a_client_within_the_handle );

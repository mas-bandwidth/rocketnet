/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The UDP coalescing rules (sdk/rocketnet_coalesce.h): the receive split by
// segment stride and the send run. Pure: no sockets, no syscall, no wall
// clock. Every expected value is written as a literal, never computed by the
// code under test.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet_coalesce.h"

static void test_coalesce_split_by_stride_round_trips()
{
    int offsets[64];
    int lengths[64];

    // 3000 bytes at stride 1200: two full segments and a 600-byte tail

    rocketnet_unit_check( rocketnet_coalesce_split( 3000, 1200, offsets, lengths, 64 ) == 3 );
    rocketnet_unit_check( offsets[0] == 0 );
    rocketnet_unit_check( offsets[1] == 1200 );
    rocketnet_unit_check( offsets[2] == 2400 );
    rocketnet_unit_check( lengths[0] == 1200 );
    rocketnet_unit_check( lengths[1] == 1200 );
    rocketnet_unit_check( lengths[2] == 600 );
    rocketnet_unit_check( lengths[0] + lengths[1] + lengths[2] == 3000 );

    // 2400 at 1200: two equal segments

    rocketnet_unit_check( rocketnet_coalesce_split( 2400, 1200, offsets, lengths, 64 ) == 2 );
    rocketnet_unit_check( offsets[0] == 0 );
    rocketnet_unit_check( offsets[1] == 1200 );
    rocketnet_unit_check( lengths[0] == 1200 );
    rocketnet_unit_check( lengths[1] == 1200 );
    rocketnet_unit_check( lengths[0] + lengths[1] == 2400 );

    // stride 0 (no stride reported): one segment of the whole message

    rocketnet_unit_check( rocketnet_coalesce_split( 1000, 0, offsets, lengths, 64 ) == 1 );
    rocketnet_unit_check( offsets[0] == 0 );
    rocketnet_unit_check( lengths[0] == 1000 );

    // a negative stride is the same as no stride

    rocketnet_unit_check( rocketnet_coalesce_split( 1000, -5, offsets, lengths, 64 ) == 1 );
    rocketnet_unit_check( offsets[0] == 0 );
    rocketnet_unit_check( lengths[0] == 1000 );

    // stride past the length: one segment of the whole message

    rocketnet_unit_check( rocketnet_coalesce_split( 1000, 1500, offsets, lengths, 64 ) == 1 );
    rocketnet_unit_check( offsets[0] == 0 );
    rocketnet_unit_check( lengths[0] == 1000 );

    // exactly max segments is accepted

    rocketnet_unit_check( rocketnet_coalesce_split( 6400, 100, offsets, lengths, 64 ) == 64 );
    rocketnet_unit_check( offsets[63] == 6300 );
    rocketnet_unit_check( lengths[63] == 100 );
}

static void test_coalesce_split_refuses_past_max()
{
    int offsets[64];
    int lengths[64];

    // 64 segments with max 63: refused, and the arrays are untouched

    for ( int i = 0; i < 64; i++ )
    {
        offsets[i] = -7;
        lengths[i] = -9;
    }

    rocketnet_unit_check( rocketnet_coalesce_split( 6400, 100, offsets, lengths, 63 ) == -1 );

    for ( int i = 0; i < 64; i++ )
    {
        rocketnet_unit_check( offsets[i] == -7 );
        rocketnet_unit_check( lengths[i] == -9 );
    }

    // a short tail that makes the count pass max is refused too: 6301 at
    // 100 is 64 segments

    rocketnet_unit_check( rocketnet_coalesce_split( 6301, 100, offsets, lengths, 63 ) == -1 );
    rocketnet_unit_check( offsets[0] == -7 );
    rocketnet_unit_check( lengths[0] == -9 );

    // message_bytes 0 (and negative) returns 0 and writes nothing

    rocketnet_unit_check( rocketnet_coalesce_split( 0, 1200, offsets, lengths, 64 ) == 0 );
    rocketnet_unit_check( rocketnet_coalesce_split( -1, 1200, offsets, lengths, 64 ) == 0 );
    rocketnet_unit_check( offsets[0] == -7 );
    rocketnet_unit_check( lengths[0] == -9 );
}

static void coalesce_test_address( struct rocketnet_address_t * address, uint8_t last, uint16_t port )
{
    memset( address, 0, sizeof( *address ) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = 10;
    address->data.ipv4[1] = 0;
    address->data.ipv4[2] = 0;
    address->data.ipv4[3] = last;
    address->port = port;
}

static void test_coalesce_send_run_equal_sizes_and_tail()
{
    struct rocketnet_address_t to[5];
    for ( int i = 0; i < 5; i++ )
    {
        coalesce_test_address( &to[i], 1, 40000 );
    }

    const int sizes[5] = { 100, 100, 100, 40, 100 };

    // sdk/rocketnet_server_gso_linux.cpp, the run builder:
    //     if ( next->packet_bytes > segment_bytes )
    //         break;
    //     ...
    //     run_count++;
    //     run_bytes += next->packet_bytes;
    //     batch_end++;
    //     if ( next->packet_bytes < segment_bytes )
    //         break;                      // the shorter tail closes the run
    // so 100, 100, 100 are taken, the 40 is taken and closes the run, and
    // the last 100 starts the next run: 4.

    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 5, 64, 65000 ) == 4 );

    // the next run starts at the last 100: a run of 1

    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes + 4, to + 4, 1, 64, 65000 ) == 1 );

    // max_segments 2 caps it at 2

    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 5, 2, 65000 ) == 2 );

    // max_super_bytes 250 caps it at 2: 100 + 100 = 200, a third 100 is 300

    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 5, 64, 250 ) == 2 );

    // count bounds the run

    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 2, 64, 65000 ) == 2 );

    // an empty queue or an aborted first slot is no run

    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 0, 64, 65000 ) == 0 );

    const int aborted_first[2] = { 0, 100 };
    rocketnet_unit_check( rocketnet_coalesce_send_run( aborted_first, to, 2, 64, 65000 ) == 0 );
}

static void test_coalesce_send_run_stops_on_destination_and_abort()
{
    struct rocketnet_address_t to[4];
    for ( int i = 0; i < 4; i++ )
    {
        coalesce_test_address( &to[i], 1, 40000 );
    }

    const int sizes[4] = { 100, 100, 100, 100 };

    // a different destination at index 2 ends the run at 2: another host,
    // then the same host on another port

    coalesce_test_address( &to[2], 2, 40000 );
    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 4, 64, 65000 ) == 2 );
    coalesce_test_address( &to[2], 1, 40001 );
    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 4, 64, 65000 ) == 2 );
    coalesce_test_address( &to[2], 1, 40000 );
    rocketnet_unit_check( rocketnet_coalesce_send_run( sizes, to, 4, 64, 65000 ) == 4 );

    // a zero (aborted slot) at index 1 ends the run at 1

    const int aborted[4] = { 100, 0, 100, 100 };
    rocketnet_unit_check( rocketnet_coalesce_send_run( aborted, to, 4, 64, 65000 ) == 1 );

    // a larger packet at index 1 ends the run at 1

    const int larger[4] = { 100, 101, 100, 100 };
    rocketnet_unit_check( rocketnet_coalesce_send_run( larger, to, 4, 64, 65000 ) == 1 );
}

REGISTER_UNIT_TEST( test_coalesce_split_by_stride_round_trips );
REGISTER_UNIT_TEST( test_coalesce_split_refuses_past_max );
REGISTER_UNIT_TEST( test_coalesce_send_run_equal_sizes_and_tail );
REGISTER_UNIT_TEST( test_coalesce_send_run_stops_on_destination_and_abort );

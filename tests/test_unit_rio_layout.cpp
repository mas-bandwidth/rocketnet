/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Windows Registered I/O (RIO) transport's region layout and request
// contexts (sdk/rocketnet_rio_layout.h): pure, no sockets, no ring, no wall
// clock. Every expected value is written as a literal, never computed by the
// code under test.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet_rio_layout.h"

static void test_rio_transport_layout_slices_tile_the_region()
{
    // 2 * 16384 frames of 1536 bytes, 4096 slices of 2048 bytes, 28-byte
    // addresses, page 4096.

    struct rocketnet_rio_layout_t p;
    rocketnet_unit_check( rocketnet_rio_layout_plan(
        32768, 4096, 1536, 2048, 28, 4096, &p ) );

    // region offsets

    rocketnet_unit_check( p.send_frames_offset == 0 );
    rocketnet_unit_check( p.send_addresses_offset == 50331648 );
    rocketnet_unit_check( p.receive_slices_offset == 52428800 );
    rocketnet_unit_check( p.receive_addresses_offset == 60817408 );
    rocketnet_unit_check( p.total_bytes == 61079552 );

    // every offset of every element is 64-aligned: first and last of each
    // sub-region.

    rocketnet_unit_check( rocketnet_rio_send_frame_offset( &p, 0 ) == 0 );
    rocketnet_unit_check( rocketnet_rio_send_frame_offset( &p, 32767 ) == 50330112 );
    rocketnet_unit_check( rocketnet_rio_send_address_offset( &p, 0 ) == 50331648 );
    rocketnet_unit_check( rocketnet_rio_send_address_offset( &p, 32767 ) == 52428736 );
    rocketnet_unit_check( rocketnet_rio_receive_slice_offset( &p, 0 ) == 52428800 );
    rocketnet_unit_check( rocketnet_rio_receive_slice_offset( &p, 4095 ) == 60815360 );
    rocketnet_unit_check( rocketnet_rio_receive_address_offset( &p, 0 ) == 60817408 );
    rocketnet_unit_check( rocketnet_rio_receive_address_offset( &p, 4095 ) == 61079488 );

    rocketnet_unit_check( ( rocketnet_rio_send_frame_offset( &p, 0 ) & 63 ) == 0 );
    rocketnet_unit_check( ( rocketnet_rio_send_frame_offset( &p, 32767 ) & 63 ) == 0 );
    rocketnet_unit_check( ( rocketnet_rio_send_address_offset( &p, 0 ) & 63 ) == 0 );
    rocketnet_unit_check( ( rocketnet_rio_send_address_offset( &p, 32767 ) & 63 ) == 0 );
    rocketnet_unit_check( ( rocketnet_rio_receive_slice_offset( &p, 0 ) & 63 ) == 0 );
    rocketnet_unit_check( ( rocketnet_rio_receive_slice_offset( &p, 4095 ) & 63 ) == 0 );
    rocketnet_unit_check( ( rocketnet_rio_receive_address_offset( &p, 0 ) & 63 ) == 0 );
    rocketnet_unit_check( ( rocketnet_rio_receive_address_offset( &p, 4095 ) & 63 ) == 0 );

    // consecutive elements of one sub-region are exactly one rounded element
    // apart.

    rocketnet_unit_check( rocketnet_rio_send_frame_offset( &p, 1 ) == rocketnet_rio_send_frame_offset( &p, 0 ) + 1536 );
    rocketnet_unit_check( rocketnet_rio_send_address_offset( &p, 1 ) == rocketnet_rio_send_address_offset( &p, 0 ) + 64 );
    rocketnet_unit_check( rocketnet_rio_receive_slice_offset( &p, 1 ) == rocketnet_rio_receive_slice_offset( &p, 0 ) + 2048 );
    rocketnet_unit_check( rocketnet_rio_receive_address_offset( &p, 1 ) == rocketnet_rio_receive_address_offset( &p, 0 ) + 64 );

    // no two sub-regions overlap: each region's start is at least the end
    // of the previous one.

    rocketnet_unit_check( p.receive_slices_offset >= p.send_addresses_offset + 32768 * 64 );
    rocketnet_unit_check( p.receive_addresses_offset >= p.receive_slices_offset + 4096 * 2048 );
    rocketnet_unit_check( p.total_bytes >= p.receive_addresses_offset + 4096 * 64 );

    // total_bytes is a multiple of 4096 and at least the sum of the regions

    rocketnet_unit_check( ( p.total_bytes % 4096 ) == 0 );
    rocketnet_unit_check( p.total_bytes >= 61079552 );

    // a small plan (1, 1, 1, 1, 1, page 64) checked offset by offset

    struct rocketnet_rio_layout_t q;
    rocketnet_unit_check( rocketnet_rio_layout_plan( 1, 1, 1, 1, 1, 64, &q ) );
    rocketnet_unit_check( q.send_frames_offset == 0 );
    rocketnet_unit_check( q.send_addresses_offset == 64 );
    rocketnet_unit_check( q.receive_slices_offset == 128 );
    rocketnet_unit_check( q.receive_addresses_offset == 192 );
    rocketnet_unit_check( q.total_bytes == 256 );
    rocketnet_unit_check( rocketnet_rio_send_frame_offset( &q, 0 ) == 0 );
    rocketnet_unit_check( rocketnet_rio_send_address_offset( &q, 0 ) == 64 );
    rocketnet_unit_check( rocketnet_rio_receive_slice_offset( &q, 0 ) == 128 );
    rocketnet_unit_check( rocketnet_rio_receive_address_offset( &q, 0 ) == 192 );
}

static void test_rio_transport_layout_refuses_bad_geometry()
{
    struct rocketnet_rio_layout_t p;
    struct rocketnet_rio_layout_t before;

    // a zero count

    memset( &before, 0x5A, sizeof( before ) );
    p = before;
    rocketnet_unit_check( !rocketnet_rio_layout_plan( 0, 1, 1, 1, 1, 64, &p ) );
    rocketnet_unit_check( memcmp( &p, &before, sizeof( p ) ) == 0 );

    // a negative size

    memset( &before, 0x5A, sizeof( before ) );
    p = before;
    rocketnet_unit_check( !rocketnet_rio_layout_plan( 1, 1, -1, 1, 1, 64, &p ) );
    rocketnet_unit_check( memcmp( &p, &before, sizeof( p ) ) == 0 );

    // page 0.

    memset( &before, 0x5A, sizeof( before ) );
    p = before;
    rocketnet_unit_check( !rocketnet_rio_layout_plan( 1, 1, 1, 1, 1, 0, &p ) );
    rocketnet_unit_check( memcmp( &p, &before, sizeof( p ) ) == 0 );

    // page 3000 is not a power of two

    memset( &before, 0x5A, sizeof( before ) );
    p = before;
    rocketnet_unit_check( !rocketnet_rio_layout_plan( 1, 1, 1, 1, 1, 3000, &p ) );
    rocketnet_unit_check( memcmp( &p, &before, sizeof( p ) ) == 0 );

    // a total past UINT32_MAX

    memset( &before, 0x5A, sizeof( before ) );
    p = before;
    rocketnet_unit_check( !rocketnet_rio_layout_plan( 2000000, 1, 4096, 64, 64, 4096, &p ) );
    rocketnet_unit_check( memcmp( &p, &before, sizeof( p ) ) == 0 );
}

static void test_rio_transport_layout_refuses_a_page_that_wraps_the_total()
{
    // the largest counts and sizes end the region past 2^63, and a page of
    // 2^63 rounds that end past 2^64: the total wraps to 0, which is not a
    // total within UINT32_MAX, and the plan refuses with out untouched

    struct rocketnet_rio_layout_t p;
    struct rocketnet_rio_layout_t before;

    memset( &before, 0x5A, sizeof( before ) );
    p = before;
    rocketnet_unit_check( !rocketnet_rio_layout_plan( INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, UINT64_C( 1 ) << 63, &p ) );
    rocketnet_unit_check( memcmp( &p, &before, sizeof( p ) ) == 0 );

    // a small region on the same page is past UINT32_MAX without any wrap

    memset( &before, 0x5A, sizeof( before ) );
    p = before;
    rocketnet_unit_check( !rocketnet_rio_layout_plan( 1, 1, 1, 1, 1, UINT64_C( 1 ) << 63, &p ) );
    rocketnet_unit_check( memcmp( &p, &before, sizeof( p ) ) == 0 );
}

static void test_rio_transport_layout_index_bounds()
{
    struct rocketnet_rio_layout_t p;
    rocketnet_unit_check( rocketnet_rio_layout_plan( 2, 2, 64, 128, 32, 64, &p ) );

    // index -1 and index count give UINT64_MAX

    rocketnet_unit_check( rocketnet_rio_send_frame_offset( &p, -1 ) == UINT64_MAX );
    rocketnet_unit_check( rocketnet_rio_send_address_offset( &p, -1 ) == UINT64_MAX );
    rocketnet_unit_check( rocketnet_rio_receive_slice_offset( &p, -1 ) == UINT64_MAX );
    rocketnet_unit_check( rocketnet_rio_receive_address_offset( &p, -1 ) == UINT64_MAX );

    rocketnet_unit_check( rocketnet_rio_send_frame_offset( &p, 2 ) == UINT64_MAX );
    rocketnet_unit_check( rocketnet_rio_send_address_offset( &p, 2 ) == UINT64_MAX );
    rocketnet_unit_check( rocketnet_rio_receive_slice_offset( &p, 2 ) == UINT64_MAX );
    rocketnet_unit_check( rocketnet_rio_receive_address_offset( &p, 2 ) == UINT64_MAX );

    // index count - 1 gives the last element

    rocketnet_unit_check( rocketnet_rio_send_frame_offset( &p, 1 ) == 64 );
    rocketnet_unit_check( rocketnet_rio_send_address_offset( &p, 1 ) == 192 );
    rocketnet_unit_check( rocketnet_rio_receive_slice_offset( &p, 1 ) == 384 );
    rocketnet_unit_check( rocketnet_rio_receive_address_offset( &p, 1 ) == 576 );
}

static void test_rio_transport_context_round_trips()
{
    uint64_t c = rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 0, 0 );
    rocketnet_unit_check( rocketnet_rio_context_kind( c ) == ROCKETNET_RIO_CONTEXT_SEND );
    rocketnet_unit_check( rocketnet_rio_context_phase( c ) == 0 );
    rocketnet_unit_check( rocketnet_rio_context_index( c ) == 0 );

    c = rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 255, 0xffffffff );
    rocketnet_unit_check( rocketnet_rio_context_kind( c ) == ROCKETNET_RIO_CONTEXT_RECEIVE );
    rocketnet_unit_check( rocketnet_rio_context_phase( c ) == 255 );
    rocketnet_unit_check( (uint32_t) rocketnet_rio_context_index( c ) == 0xffffffffu );

    c = rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 255, 0 );
    rocketnet_unit_check( rocketnet_rio_context_kind( c ) == ROCKETNET_RIO_CONTEXT_SEND );
    rocketnet_unit_check( rocketnet_rio_context_phase( c ) == 255 );
    rocketnet_unit_check( rocketnet_rio_context_index( c ) == 0 );

    c = rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 0, 0xffffffff );
    rocketnet_unit_check( rocketnet_rio_context_kind( c ) == ROCKETNET_RIO_CONTEXT_SEND );
    rocketnet_unit_check( rocketnet_rio_context_phase( c ) == 0 );
    rocketnet_unit_check( (uint32_t) rocketnet_rio_context_index( c ) == 0xffffffffu );

    rocketnet_unit_check( rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 0, 0 ) != rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0, 0 ) );
    rocketnet_unit_check( rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 0, 0 ) != rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 255, 0 ) );
    rocketnet_unit_check( rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 0, 0 ) != rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_SEND, 0, 0xffffffff ) );
}

static void test_rio_transport_context_phase_never_reaches_kind()
{
    // the phase owns bits 48..55 only: a phase wider than a byte must not
    // spill into the kind byte at bits 56..63.

    const uint64_t c = rocketnet_rio_context( ROCKETNET_RIO_CONTEXT_RECEIVE, 0x1FF, 7 );
    rocketnet_unit_check( rocketnet_rio_context_kind( c ) == ROCKETNET_RIO_CONTEXT_RECEIVE );
    rocketnet_unit_check( rocketnet_rio_context_phase( c ) == 0xFF );
    rocketnet_unit_check( rocketnet_rio_context_index( c ) == 7 );
}

// The lane hash is FNV-1a over the family, the port (low byte first) and the
// address bytes; the expected values were computed apart from this code.
static void test_rio_transport_lane_hash_and_lane()
{
    const uint8_t v4[4] = { 10, 0, 0, 1 };
    const uint8_t v6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    rocketnet_unit_check( rocketnet_rio_lane_hash( 1, 40000, v4, 4 ) == 0x1fff2155u );
    rocketnet_unit_check( rocketnet_rio_lane_hash( 1, 40008, v4, 4 ) == 0x2e57136du );
    rocketnet_unit_check( rocketnet_rio_lane_hash( 2, 40000, v6, 16 ) == 0x545fe330u );

    rocketnet_unit_check( rocketnet_rio_lane_of( 0x1fff2155u, 8 ) == 2 );
    rocketnet_unit_check( rocketnet_rio_lane_of( 0x1fff2155u, 3 ) == 2 );
    rocketnet_unit_check( rocketnet_rio_lane_of( 0x2e57136du, 8 ) == 2 );
    rocketnet_unit_check( rocketnet_rio_lane_of( 0x545fe330u, 8 ) == 7 );
    rocketnet_unit_check( rocketnet_rio_lane_of( 0xffffffffu, 32 ) == 0 );

    // one lane, or a count below one, is lane 0 whatever the hash
    rocketnet_unit_check( rocketnet_rio_lane_of( 0x545fe330u, 1 ) == 0 );
    rocketnet_unit_check( rocketnet_rio_lane_of( 0x545fe330u, 0 ) == 0 );

    // every lane in range for every hash bit pattern tried
    for ( uint32_t h = 0; h < 4096; ++h )
    {
        const uint32_t hash = h * 0x9e3779b9u;
        for ( int lanes = 1; lanes <= ROCKETNET_RIO_MAX_LANES; ++lanes )
        {
            const int lane = rocketnet_rio_lane_of( hash, lanes );
            rocketnet_unit_check( lane >= 0 && lane < lanes );
        }
    }
}

// proofs/tla/RNRioLaneLayout.tla Encode and Decode: every lane's every slot
// round trips, and an id past the last lane's last slot is refused with the
// outputs untouched.
static void test_rio_transport_lane_packet_ids()
{
    rocketnet_unit_check( rocketnet_rio_lane_packet_id( 0, 0, 12288 ) == 0 );
    rocketnet_unit_check( rocketnet_rio_lane_packet_id( 1, 0, 12288 ) == 12288 );
    rocketnet_unit_check( rocketnet_rio_lane_packet_id( 7, 12287, 12288 ) == 98303 );

    int lane = -1;
    int index = -1;
    rocketnet_unit_check( rocketnet_rio_lane_packet_decode( 98303, 8, 12288, &lane, &index ) );
    rocketnet_unit_check( lane == 7 && index == 12287 );
    rocketnet_unit_check( rocketnet_rio_lane_packet_decode( 12288, 8, 12288, &lane, &index ) );
    rocketnet_unit_check( lane == 1 && index == 0 );
    rocketnet_unit_check( rocketnet_rio_lane_packet_decode( 12287, 8, 12288, &lane, &index ) );
    rocketnet_unit_check( lane == 0 && index == 12287 );

    lane = -1;
    index = -1;
    rocketnet_unit_check( !rocketnet_rio_lane_packet_decode( 98304, 8, 12288, &lane, &index ) );
    rocketnet_unit_check( !rocketnet_rio_lane_packet_decode( 0xFFFFFFFFFFFFFFFFull, 8, 12288, &lane, &index ) );
    rocketnet_unit_check( !rocketnet_rio_lane_packet_decode( 0, 0, 12288, &lane, &index ) );
    rocketnet_unit_check( !rocketnet_rio_lane_packet_decode( 0, 8, 0, &lane, &index ) );
    rocketnet_unit_check( lane == -1 && index == -1 );

    for ( int lanes = 1; lanes <= 4; ++lanes )
    {
        for ( int slots = 1; slots <= 4; ++slots )
        {
            for ( int l = 0; l < lanes; ++l )
            {
                for ( int i = 0; i < slots; ++i )
                {
                    const uint64_t id = rocketnet_rio_lane_packet_id( l, i, slots );
                    rocketnet_unit_check( id < (uint64_t) ( lanes * slots ) );
                    rocketnet_unit_check( rocketnet_rio_lane_packet_decode( id, lanes, slots, &lane, &index ) );
                    rocketnet_unit_check( lane == l && index == i );
                }
            }
            rocketnet_unit_check( !rocketnet_rio_lane_packet_decode( (uint64_t) ( lanes * slots ), lanes, slots, &lane, &index ) );
        }
    }
}

REGISTER_UNIT_TEST( test_rio_transport_layout_slices_tile_the_region );
REGISTER_UNIT_TEST( test_rio_transport_layout_refuses_bad_geometry );
REGISTER_UNIT_TEST( test_rio_transport_layout_refuses_a_page_that_wraps_the_total );
REGISTER_UNIT_TEST( test_rio_transport_layout_index_bounds );
REGISTER_UNIT_TEST( test_rio_transport_context_round_trips );
REGISTER_UNIT_TEST( test_rio_transport_context_phase_never_reaches_kind );
REGISTER_UNIT_TEST( test_rio_transport_lane_hash_and_lane );
REGISTER_UNIT_TEST( test_rio_transport_lane_packet_ids );

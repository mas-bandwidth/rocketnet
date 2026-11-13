/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring transport's send slots per queue (rocketnet_server_uring_send_slots
// in sdk/rocketnet_server_uring.h): twice a queue's share of one frame at the
// bar rate, between the floor and the module's most. Pure: no ring, no socket,
// no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_server_uring.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_uring_receive_queue.h"

#define check rocketnet_unit_check

static void test_uring_send_slots()
{
    // the bar: 1000 players at 2 queues stage 18000 a queue, so one frame of
    // 1.04M packets/s at 60 Hz (17,334) fits on either queue's share twice
    check( rocketnet_server_uring_send_slots( 1000, 2 ) == 18000 );
    check( (int64_t) rocketnet_server_uring_send_slots( 1000, 2 ) * 2 * 60 >= 1040000 * 2 );

    // at 4 and 8 queues the share falls, never below the floor
    check( rocketnet_server_uring_send_slots( 1000, 4 ) == 9000 );
    check( rocketnet_server_uring_send_slots( 1000, 8 ) == 4500 );
    check( rocketnet_server_uring_send_slots( 1000, 32 ) == ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( rocketnet_server_uring_send_slots( 1, 1 ) == ROCKETNET_URING_SEND_QUEUE_SIZE );

    // one queue at 1000 players would want 36000: the module's most
    check( rocketnet_server_uring_send_slots( 1000, 1 ) == ROCKETNET_URING_SEND_MAX_BANK_SLOTS );

    // a share that does not divide rounds up
    check( rocketnet_server_uring_send_slots( 1000, 3 ) == 12000 );
    check( rocketnet_server_uring_send_slots( 999, 7 ) == 5138 );

    // nonsense counts get the floor
    check( rocketnet_server_uring_send_slots( 0, 2 ) == ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( rocketnet_server_uring_send_slots( 1000, 0 ) == ROCKETNET_URING_SEND_QUEUE_SIZE );
}

// the memory plan's bound of one ring's kernel mappings covers what the kernel
// maps for ROCKETNET_URING_ENTRIES (64 bytes a submission entry, 4 an index,
// 16 a completion entry and twice as many completion entries), in whole pages

static void test_uring_ring_mapped_bound_covers_the_rings()
{
    const uint64_t entries = (uint64_t) ROCKETNET_URING_ENTRIES;
    const uint64_t needed = entries * 64 + entries * 4 + entries * 2 * 16;
    const uint64_t bound = rocketnet_server_uring_ring_mapped_bound( ROCKETNET_URING_ENTRIES );
    check( bound >= needed );
    check( bound % 4096 == 0 );
    check( bound < needed + 8 * 4096 );
    check( bound > 400000 && bound < 440000 );
    check( rocketnet_server_uring_ring_mapped_bound( 0 ) == 0 );
    check( rocketnet_server_uring_ring_mapped_bound( -1 ) == 0 );
    // more entries, more bytes
    check( rocketnet_server_uring_ring_mapped_bound( 8192 ) > bound );

#if defined( ROCKETNET_IO_URING )
    // the plan: every queue's buffer ring and its ring mappings
    const uint64_t one = rocketnet_server_uring_mapped_bytes( 1 );
    check( one == rocketnet_uring_receive_queue_mapped_bytes( ROCKETNET_URING_RECV_BUFFERS ) + bound );
    check( rocketnet_server_uring_mapped_bytes( 3 ) == 3 * one );
    check( rocketnet_server_uring_mapped_bytes( 0 ) == 0 );
#endif // #if defined( ROCKETNET_IO_URING )
}

REGISTER_UNIT_TEST( test_uring_send_slots );
REGISTER_UNIT_TEST( test_uring_ring_mapped_bound_covers_the_rings );

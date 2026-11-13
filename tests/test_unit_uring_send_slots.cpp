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

REGISTER_UNIT_TEST( test_uring_send_slots );

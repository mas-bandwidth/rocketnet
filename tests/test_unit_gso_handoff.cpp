/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "../sdk/rocketnet_server_gso.h"

#include <stdint.h>
#include <stdio.h>

#define check rocketnet_unit_check

#if defined( __linux__ ) && ROCKETNET_DEV

// The GSO worker's handoffs (proofs/tla/RNGsoHandoff.tla), stepped on this thread
// with a faked transport: no socket, no worker thread, no clock.
static void test_server_gso_worker_handoff()
{
    check( rocketnet_server_gso_test_worker_handoff() );
}

REGISTER_UNIT_TEST( test_server_gso_worker_handoff );

// A refused sendmmsg through the send-failure seam: an errno other than a
// full socket's drops one message and sends the rest.
static void test_server_gso_send_refusals()
{
    check( rocketnet_server_gso_test_send_refusals() );
}

REGISTER_UNIT_TEST( test_server_gso_send_refusals );

// A full socket mid-flush on a fake socket (proofs/tla/RNGsoHandoff.tla
// SentOrHeld): the bank keeps its unsent tail, nothing is dropped, and the
// next attempt sends the rest in order.
static void test_server_gso_send_full_keeps_rest()
{
    check( rocketnet_server_gso_test_send_full_keeps_rest() );
}

REGISTER_UNIT_TEST( test_server_gso_send_full_keeps_rest );

// The receive drain's read budget: full batches of zero-length, refused-source
// and undersized datagrams, far more than the budget, through the receive
// seam; each drain returns after ROCKETNET_GSO_RECV_QUEUE_SIZE messages.
static void test_server_gso_drain_budget()
{
    check( rocketnet_server_gso_test_drain_budget() );
}

REGISTER_UNIT_TEST( test_server_gso_drain_budget );

// a drain asks recvmmsg only for messages whose every segment fits the
// buffer, so nothing it reads is dropped as overflow
static void test_server_gso_drain_room()
{
    check( rocketnet_server_gso_test_drain_room() );
}

REGISTER_UNIT_TEST( test_server_gso_drain_room );

// Four full receive queues, and the real per-update budget, which two queues
// exhaust (8192 + 8192 is past 16000). The queues the first update consumes
// are published full again: a walk that always starts at 0 then stays on 0
// and 1, and queues 2 and 3 never deliver. Across two updates every queue
// must. No socket, no thread, no clock.
static void test_server_gso_receive_queue_rotation()
{
    const int queues = 4;
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( queues );
    check( gso );

    struct rocketnet_server_socket_process_packets_t * view = rocketnet_server_gso_process_packets( gso );
    check( view->capacity > ROCKETNET_GSO_RECV_QUEUE_SIZE );
    check( view->capacity < 2 * ROCKETNET_GSO_RECV_QUEUE_SIZE );

    int seen[4] = { 0, 0, 0, 0 };

    for ( int q = 0; q < queues; q++ )
        check( rocketnet_server_gso_test_publish_marked( gso, q, (uint8_t) ( q + 1 ), (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );

    rocketnet_server_gso_receive_packets( gso );
    check( view->num_packets == view->capacity );
    for ( int i = 0; i < view->num_packets; i++ )
    {
        const int marker = (int) view->packet_data[i][0];
        check( marker >= 1 && marker <= queues );
        seen[marker - 1]++;
    }
    check( seen[0] > 0 );
    check( seen[1] > 0 );
    check( seen[2] == 0 );
    check( seen[3] == 0 );

    // 0 and 1 have a free buffer again; 2 and 3 still hold the ready buffer
    // the first update did not claim.
    check( rocketnet_server_gso_test_publish_marked( gso, 0, (uint8_t) 1, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );
    check( rocketnet_server_gso_test_publish_marked( gso, 1, (uint8_t) 2, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );
    check( !rocketnet_server_gso_test_publish_marked( gso, 2, (uint8_t) 3, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );
    check( !rocketnet_server_gso_test_publish_marked( gso, 3, (uint8_t) 4, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );

    rocketnet_server_gso_receive_packets( gso );
    check( view->num_packets == view->capacity );
    for ( int i = 0; i < view->num_packets; i++ )
    {
        const int marker = (int) view->packet_data[i][0];
        check( marker >= 1 && marker <= queues );
        seen[marker - 1]++;
    }

    check( seen[0] > 0 );
    check( seen[1] > 0 );
    check( seen[2] > 0 );
    check( seen[3] > 0 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_receive_queue_rotation );

#endif

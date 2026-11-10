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

bool rocketnet_server_gso_test_receive_backoff();

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

static void test_server_gso_receive_backoff()
{
    check( rocketnet_server_gso_test_receive_backoff() );
}

REGISTER_UNIT_TEST( test_server_gso_receive_backoff );

#endif

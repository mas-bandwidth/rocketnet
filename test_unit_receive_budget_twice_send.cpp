/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_constants.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_server_gso.h"
#include "rocketnet_server_uring.h"
#include "rocketnet_server_iocp.h"
#include "rocketnet_server_rio.h"

#define check rocketnet_unit_check

static void receive_budget_twice_send_portable()
{
    check( ROCKETNET_SERVER_MAX_SEND_PACKETS > 0 );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ( ROCKETNET_SERVER_MAX_SEND_PACKETS + ROCKETNET_SERVER_PAYLOAD_RING_SIZE ) );
    check( ROCKETNET_NUM_CLIENT_PACKETS >= 2 * ROCKETNET_CLIENT_PAYLOAD_RING_SIZE );
}

static void receive_budget_twice_send_msgx()
{
    check( ROCKETNET_MSGX_SEND_BATCH > 0 );
    check( ROCKETNET_MSGX_RECV_BATCH >= 2 * ROCKETNET_MSGX_SEND_BATCH );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ( ROCKETNET_SERVER_MAX_SEND_PACKETS + ROCKETNET_SERVER_PAYLOAD_RING_SIZE ) );
    check( ROCKETNET_NUM_CLIENT_PACKETS >= 2 * ROCKETNET_CLIENT_PAYLOAD_RING_SIZE );
}

static void receive_budget_twice_send_loopback()
{
    check( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
    check( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ( ROCKETNET_SERVER_MAX_SEND_PACKETS + ROCKETNET_SERVER_PAYLOAD_RING_SIZE ) );
    check( ROCKETNET_LOOPBACK_QUEUE_PACKETS >= 2 * ROCKETNET_CLIENT_PAYLOAD_RING_SIZE );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS );
}

static void receive_budget_twice_send_gso()
{
    check( ROCKETNET_GSO_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_GSO_RECV_QUEUE_SIZE >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE );
    check( ROCKETNET_GSO_RECV_QUEUE_SIZE * ROCKETNET_GSO_DEFAULT_QUEUES >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE );
    check( ROCKETNET_GSO_RECV_BATCH >= 2 * ROCKETNET_GSO_SEND_BATCH );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE );
}

static void receive_budget_twice_send_io_uring()
{
    check( ROCKETNET_URING_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_URING_RECV_BUFFERS >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( ROCKETNET_URING_RECV_BUFFERS * ROCKETNET_URING_DEFAULT_QUEUES >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE );
}

static void receive_budget_twice_send_xdp()
{
    check( ROCKETNET_XDP_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_XDP_RECV_QUEUE_SIZE >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_XDP_SEND_QUEUE_SIZE );
}

static void receive_budget_twice_send_iocp()
{
    check( ROCKETNET_IOCP_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE >= 2 * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    check( ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE * ROCKETNET_IOCP_DEFAULT_WORKERS >= 2 * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
}

static void receive_budget_twice_send_rio()
{
    check( ROCKETNET_RIO_SEND_QUEUE_SIZE > 0 );
    check( ROCKETNET_RIO_RECEIVES >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE );
    check( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS >= 2 * ROCKETNET_RIO_SEND_QUEUE_SIZE );
}

REGISTER_UNIT_TEST( receive_budget_twice_send_portable );
REGISTER_UNIT_TEST( receive_budget_twice_send_msgx );
REGISTER_UNIT_TEST( receive_budget_twice_send_loopback );
REGISTER_UNIT_TEST( receive_budget_twice_send_gso );
REGISTER_UNIT_TEST( receive_budget_twice_send_io_uring );
REGISTER_UNIT_TEST( receive_budget_twice_send_xdp );
REGISTER_UNIT_TEST( receive_budget_twice_send_iocp );
REGISTER_UNIT_TEST( receive_budget_twice_send_rio );

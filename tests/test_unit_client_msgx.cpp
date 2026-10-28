/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS msgx client, as the registry builds it: the msgx backend's client
// table is the portable client table (sdk/rocketnet_driver_builtin.cpp,
// rocketnet_msgx_transport), so a pin differs from portable only in the
// transport name its create hands the socket create, which picks the pump
// (rocketnet_platform_socket_create_with_pump: "msgx" the sendmsg_x/recvmsg_x
// pump, "portable" the per-packet loop). The table runs the client I/O thread
// (docs/CLIENT-IO.md, RNClientIo) with the batch callbacks that reach that
// pump, and the auto choice on macOS is msgx. No transport is created, no
// socket is opened and no clock is read: the pin-to-pump reads of a live
// socket and the wire are functional cases (functional/rocketnet_functional_tests.cpp,
// test_msgx_client_pin_selects_batch_pump and the cases beside it). The file
// builds empty on every other platform.

#include "test_unit_registry.h"

#include "rocketnet_platform.h"
#include "rocketnet_driver.h"

#define check rocketnet_unit_check

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// one client table for both names: the pump is the socket's, chosen at create

static void test_msgx_client_shares_the_portable_client_table()
{
    const rocketnet_transport_definition_t * msgx = rocketnet_transport_definition( ROCKETNET_TRANSPORT_MSGX );
    const rocketnet_transport_definition_t * portable = rocketnet_transport_definition( ROCKETNET_TRANSPORT_PORTABLE );
    check( msgx && portable && msgx->backend && portable->backend );
    check( msgx->backend->client != NULL );
    check( msgx->backend->client == portable->backend->client );
}

// the table fills the client I/O thread's callbacks: the batches go through
// the socket's pump seam, and the wait and the wake are the kqueue wake

static void test_msgx_client_table_runs_the_io_thread()
{
    const rocketnet_client_transport_io_operations_t * operations = rocketnet_transport_definition( ROCKETNET_TRANSPORT_MSGX )->backend->client;
    check( operations->create && operations->destroy && operations->send && operations->receive );
    check( operations->send_batch && operations->receive_batch );
    check( operations->wait && operations->wake );
    check( operations->send_failures && operations->connect );
}

// the client ladder on macOS: msgx (rank 10) above portable (rank 30), so an
// unpinned client is the msgx client

static void test_msgx_client_is_the_macos_auto_choice()
{
    const rocketnet_transport_definition_t * msgx = rocketnet_transport_definition( ROCKETNET_TRANSPORT_MSGX );
    const rocketnet_transport_definition_t * portable = rocketnet_transport_definition( ROCKETNET_TRANSPORT_PORTABLE );
    check( msgx->client_preference > 0 && msgx->client_preference < portable->client_preference );

    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );
    check( num_available >= 2 );
    check( available[0] == ROCKETNET_TRANSPORT_MSGX );
    check( available[1] == ROCKETNET_TRANSPORT_PORTABLE );
}

REGISTER_UNIT_TEST( test_msgx_client_shares_the_portable_client_table );
REGISTER_UNIT_TEST( test_msgx_client_table_runs_the_io_thread );
REGISTER_UNIT_TEST( test_msgx_client_is_the_macos_auto_choice );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

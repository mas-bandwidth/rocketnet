/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Mutation gaps (delta, core): the loopback backend (sdk/rocketnet_driver_loopback.cpp).
//
//   - a hub's ephemeral range: a port 0 bind is given 49152 first, or, for a
//     hub of more endpoints than 49152 .. 65535 holds (more than 16383
//     clients, reachable in a build of ROCKETNET_MAX_CLIENTS above that), the
//     port that leaves exactly one port per endpoint below 65536
//     (proofs/tla/RNLoopbackBindLayout.tla Low, HoldsEveryRecord)
//   - a contended shutdown batch counts a failure for each packet of 1 ..
//     ROCKETNET_RECEIVE_CEILING bytes, the two edge sizes included
//
// Driven through the backend's operations as test_unit_driver_loopback.cpp
// does: no socket, no thread, no real time.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"
#include "loopback_hub.h"

#include <string.h>

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

// the port a hub of max_clients gives its first port 0 bind (a server's)

static int gaps_loopback_first_bound_port( int max_clients )
{
    rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, max_clients, &status );
    rocketnet_unit_check( hub != NULL && status == ROCKETNET_STATUS_OK );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.loopback_hub = hub;
    const rocketnet_server_transport_io_config_t setup = { "loopback", &server_config, NULL, NULL };

    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    rocketnet_server_transport_io_t server = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( rocketnet_loopback_transport.server, NULL, &setup, &address, &server ) );
    const int port = (int) address.port;

    rocketnet_server_transport_io_stop( &server );
    rocketnet_unit_check( rocketnet_server_transport_io_stopped( &server ) );
    rocketnet_server_transport_io_close( &server );
    rocketnet_unit_check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );
    return port;
}

// mutation gap: delta sdk/rocketnet_driver_loopback.cpp:96:44:off_by_one:1 (the range seam's initial value)
// mutation gap: delta sdk/rocketnet_driver_loopback.cpp:115:5:negate_condition
// mutation gap: delta sdk/rocketnet_driver_loopback.cpp:115:75:relational:>
// mutation gap: delta sdk/rocketnet_driver_loopback.cpp:114:* , 115:* , 117:* (in a build of ROCKETNET_MAX_CLIENTS above 16384)
//
// The hub sizes around the edge where 49152 .. 65535 (16384 ports) stops
// holding every endpoint (one server and max_clients clients): 16383 clients
// still fit from 49152; 16384 start one lower; the build's largest hub starts
// so its last endpoint takes 65535. Sizes above the build's
// ROCKETNET_MAX_CLIENTS are refused at create, so a default build checks the
// sizes it can make and a ROCKETNET_MAX_CLIENTS=32768 build checks them all.

static void test_gaps_delta_core_loopback_ephemeral_range_holds_every_endpoint()
{
    struct gaps_range_t
    {
        int max_clients;
        int first_port;
    };

    const gaps_range_t ranges[] = {
        { 1, 49152 },
        { 16382, 49152 },
        { 16383, 49152 },
        { 16384, 49151 },
        { 16385, 49150 },
        { 32768, 32767 },
    };

    for ( size_t i = 0; i < sizeof( ranges ) / sizeof( ranges[0] ); ++i )
    {
        if ( ranges[i].max_clients > (int) ROCKETNET_MAX_CLIENTS )
        {
            continue;
        }

        const bool first_ephemeral_port_is_the_range_that_holds_every_endpoint = gaps_loopback_first_bound_port( ranges[i].max_clients ) == ranges[i].first_port;
        rocketnet_unit_check( first_ephemeral_port_is_the_range_that_holds_every_endpoint );
    }

    // the build's own largest hub: one port per endpoint, the last at 65535

    const int endpoints = (int) ROCKETNET_MAX_CLIENTS + 1;
    const int largest_first_port = endpoints > 16384 ? 65536 - endpoints : 49152;
    const bool largest_hub_starts_where_its_range_ends_at_65535_or_at_49152 = gaps_loopback_first_bound_port( (int) ROCKETNET_MAX_CLIENTS ) == largest_first_port;
    rocketnet_unit_check( largest_hub_starts_where_its_range_ends_at_65535_or_at_49152 );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_loopback_ephemeral_range_holds_every_endpoint );

#if ROCKETNET_DEV

// mutation gap: delta sdk/rocketnet_driver_loopback.cpp:765:44:off_by_one:1 (packet_bytes > 1)
// mutation gap: delta sdk/rocketnet_driver_loopback.cpp:765:73:relational:< (packet_bytes < ROCKETNET_RECEIVE_CEILING)
//
// A contended shutdown batch of a 1-byte packet and a ROCKETNET_RECEIVE_CEILING
// one counts both as failed sends: each is a send its size allows.

static void test_gaps_delta_core_loopback_contended_shutdown_counts_edge_sizes()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.loopback_hub = rocketnet_test_loopback_hub();
    const rocketnet_server_transport_io_config_t setup = { "loopback", &config, NULL, NULL };
    rocketnet_address_t a;
    rocketnet_address_t gone;
    rocketnet_unit_check( rocketnet_address_parse( &a, "127.0.0.1:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &gone, "127.0.0.1:40999" ) );

    rocketnet_server_transport_io_t sender = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( rocketnet_loopback_transport.server, NULL, &setup, &a, &sender ) );

    static uint8_t data[ROCKETNET_RECEIVE_CEILING] = { 1, 2, 3 };
    rocketnet_platform_send_t packets[2] = {};
    const int sizes[2] = { 1, ROCKETNET_RECEIVE_CEILING };
    for ( int i = 0; i < 2; ++i )
    {
        packets[i].to = &gone;
        packets[i].packet_data = data;
        packets[i].packet_bytes = sizes[i];
    }

    rocketnet_loopback_test_send_contention( true );
    sender.operations->send_batch_shutdown( sender.instance, packets, 1 );
    const bool contended_one_byte_send_counted_as_failed = sender.operations->send_failures( sender.instance ) == 1;
    sender.operations->send_batch_shutdown( sender.instance, packets + 1, 1 );
    const bool contended_receive_ceiling_send_counted_as_failed = sender.operations->send_failures( sender.instance ) == 2;
    rocketnet_loopback_test_send_contention( false );
    rocketnet_unit_check( contended_one_byte_send_counted_as_failed );
    rocketnet_unit_check( contended_receive_ceiling_send_counted_as_failed );

    rocketnet_server_transport_io_stop( &sender );
    rocketnet_unit_check( rocketnet_server_transport_io_stopped( &sender ) );
    rocketnet_server_transport_io_close( &sender );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_loopback_contended_shutdown_counts_edge_sizes );

#endif // #if ROCKETNET_DEV

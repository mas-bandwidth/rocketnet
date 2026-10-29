/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include "../sdk/rocketnet_platform.h"
#include "../sdk/rocketnet_config.h"
#include "loopback_hub.h"
#include <string.h>

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

#if ROCKETNET_DEV
// The sender is the hub's server and the receiver one of its clients: a hub
// holds one server.
static void test_loopback_shutdown_send_drops_contended_without_waiting()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.loopback_hub = rocketnet_test_loopback_hub();
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    const rocketnet_server_transport_io_config_t setup = { "loopback", &config, NULL, NULL };
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };
    rocketnet_address_t a, b;
    rocketnet_unit_check( rocketnet_address_parse( &a, "127.0.0.1:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &b, "127.0.0.1:0" ) );

    rocketnet_server_transport_io_t sender = {};
    rocketnet_client_transport_io_t receiver = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( rocketnet_loopback_transport.server, NULL, &setup, &a, &sender ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( rocketnet_loopback_transport.client, NULL, &client_setup, &b, &receiver ) );

    uint8_t data[32] = { 1, 2, 3 };
    rocketnet_platform_send_t packets[3] = {};

    for ( int i = 0; i < 3; ++i )
    {
        packets[i].to = &b;
        packets[i].packet_data = data;
        packets[i].packet_bytes = sizeof( data );
    }

    rocketnet_loopback_test_send_contention( true );

    rocketnet_unit_check( sender.operations->send_batch_shutdown );

    sender.operations->send_batch_shutdown( sender.instance, packets, 3 );
    rocketnet_loopback_test_send_contention( false );

    rocketnet_unit_check( sender.operations->send_failures( sender.instance ) == 3 );

    rocketnet_address_t from;
    uint8_t received[32];
    rocketnet_unit_check( receiver.operations->receive( receiver.instance, &from, received, 32 ) == 0 );

    // The simulated failed try-acquire applies to shutdown only. An ordinary
    // batch still reaches the receiver under the same test contention flag.

    rocketnet_loopback_test_send_contention( true );
    sender.operations->send_batch( sender.instance, packets, 1 );
    rocketnet_loopback_test_send_contention( false );

    rocketnet_unit_check( receiver.operations->receive( receiver.instance, &from, received, 32 ) == 32 && memcmp( received, data, 32 ) == 0 );
    rocketnet_unit_check( receiver.operations->receive( receiver.instance, &from, received, 32 ) == 0 );
    rocketnet_unit_check( sender.operations->send_failures( sender.instance ) == 3 );

    // pressure has gone: the same storage remains usable and sends exactly once

    sender.operations->send_batch( sender.instance, packets, 1 );
    rocketnet_unit_check( receiver.operations->receive( receiver.instance, &from, received, 32 ) == 32 && memcmp( received, data, 32 ) == 0 );
    rocketnet_unit_check( receiver.operations->receive( receiver.instance, &from, received, 32 ) == 0 );
    rocketnet_unit_check( sender.operations->send_failures( sender.instance ) == 3 );

    rocketnet_server_transport_io_stop( &sender );
    rocketnet_unit_check( rocketnet_server_transport_io_stopped( &sender ) );

    rocketnet_server_transport_io_close( &sender );
    rocketnet_client_transport_io_close( &receiver );
}

REGISTER_UNIT_TEST( test_loopback_shutdown_send_drops_contended_without_waiting );
#endif // #if ROCKETNET_DEV

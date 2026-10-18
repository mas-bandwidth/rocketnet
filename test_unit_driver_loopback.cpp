/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Unit tests for the loopback driver backend, called directly through the
// driver open helpers with the backend's operations; no transport, no thread,
// no sleep, no clock.

#include "test_unit_registry.h"

#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_platform.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet.h"

#include <stdio.h>
#include <string.h>

extern const rocketnet_driver_backend_t rocketnet_loopback_driver;

static void set_ipv4_loopback( rocketnet_address_t & address )
{
    memset( &address.data, 0, sizeof( address.data ) );
    address.type = ROCKETNET_ADDRESS_IPV4;
    address.data.ipv4[0] = 127;
    address.data.ipv4[3] = 1;
}

static void driver_loopback_binds_and_discovers_a_port()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t setup = { "loopback", &server_config, NULL };

    rocketnet_address_t a;
    rocketnet_unit_check( rocketnet_address_parse( &a, "127.0.0.1:0" ) );
    rocketnet_server_driver_t sa = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &setup, &a, &sa ) );
    rocketnet_unit_check( a.port != 0 );

    rocketnet_address_t b = a;
    rocketnet_server_driver_t sb = {};
    rocketnet_unit_check( !rocketnet_server_driver_open( backend->server, NULL, &setup, &b, &sb ) );

    rocketnet_address_t c;
    rocketnet_unit_check( rocketnet_address_parse( &c, "[::]:0" ) );
    rocketnet_server_driver_t sc = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &setup, &c, &sc ) );
    rocketnet_unit_check( c.port != 0 );
    rocketnet_unit_check( c.port != a.port );

    rocketnet_server_driver_close( &sa );
    rocketnet_server_driver_close( &sc );
    printf( "driver_loopback_binds_and_discovers_a_port\n" );
}

static void driver_loopback_exchanges_both_ways()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };

    rocketnet_address_t server_address, client_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "0.0.0.0:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_server_driver_t server = {};
    rocketnet_client_driver_t client = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    const uint8_t data[] = { 10, 20, 30, 40 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    client.operations->send( client.instance, &to, data, sizeof( data ) );

    rocketnet_address_t from;
    uint8_t buf[64];
    int bytes = 0;
    const int received = server.operations->receive_batch( server.instance, &from, buf, 64, &bytes, 1 );
    rocketnet_unit_check( received == 1 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
    rocketnet_unit_check( from.port == client_address.port );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.data.ipv4[0] == 127 && from.data.ipv4[3] == 1 );

    rocketnet_platform_send_t packet = { &from, data, (int) sizeof( data ) };
    server.operations->send_batch( server.instance, &packet, 1 );

    bytes = client.operations->receive( client.instance, &from, buf, 64 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
    rocketnet_unit_check( from.port == server_address.port );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.data.ipv4[0] == 127 && from.data.ipv4[3] == 1 );

    rocketnet_unit_check( client.operations->receive( client.instance, &from, buf, 64 ) == 0 );

    rocketnet_server_driver_close( &server );
    rocketnet_client_driver_close( &client );
    printf( "driver_loopback_exchanges_both_ways\n" );
}

static void driver_loopback_full_queue_drops_and_counts()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_driver_t server = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &server_address, &server ) );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };
    rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_client_driver_t client = {};
    rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    const int total = ROCKETNET_LOOPBACK_QUEUE_PACKETS + 5;
    uint8_t data[4];
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    for ( int i = 0; i < total; ++i )
    {
        data[0] = (uint8_t) ( i & 0xff );
        data[1] = (uint8_t) ( ( i >> 8 ) & 0xff );
        data[2] = (uint8_t) ( ( i >> 16 ) & 0xff );
        data[3] = 0;
        client.operations->send( client.instance, &to, data, 4 );
    }
    rocketnet_unit_check( rocketnet_loopback_endpoint_dropped( server.instance ) == 5 );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_QUEUE_PACKETS; ++i )
    {
        uint8_t buf[4];
        int bytes = 0;
        rocketnet_address_t from;
        const int n = server.operations->receive_batch( server.instance, &from, buf, 4, &bytes, 1 );
        rocketnet_unit_check( n == 1 );
        rocketnet_unit_check( bytes == 4 );
        const uint32_t v = (uint32_t) buf[0] | ( (uint32_t) buf[1] << 8 ) | ( (uint32_t) buf[2] << 16 );
        rocketnet_unit_check( v == (uint32_t) i );
        rocketnet_unit_check( from.port == client_address.port );
    }

    rocketnet_address_t from;
    uint8_t buf[4];
    int bytes = 0;
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, buf, 4, &bytes, 1 ) == 0 );

    rocketnet_server_driver_close( &server );
    rocketnet_client_driver_close( &client );
    printf( "driver_loopback_full_queue_drops_and_counts\n" );
}

static void driver_loopback_destroy_frees_the_port()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    const int live_before = rocketnet_loopback_endpoints_live();

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_driver_t server = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    const uint16_t port = server_address.port;

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };
    rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_client_driver_t client = {};
    rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    rocketnet_server_driver_close( &server );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + 1 );

    uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    client.operations->send( client.instance, &to, data, 4 );
    rocketnet_unit_check( client.operations->receive( client.instance, &to, data, 4 ) == 0 );

    rocketnet_address_t again;
    rocketnet_unit_check( rocketnet_address_parse( &again, "127.0.0.1:0" ) );
    again.port = port;
    rocketnet_server_driver_t server2 = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &again, &server2 ) );
    rocketnet_unit_check( again.port == port );

    rocketnet_server_driver_close( &server2 );
    rocketnet_client_driver_close( &client );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before );
    printf( "driver_loopback_destroy_frees_the_port\n" );
}


REGISTER_UNIT_TEST( driver_loopback_binds_and_discovers_a_port );
REGISTER_UNIT_TEST( driver_loopback_exchanges_both_ways );
REGISTER_UNIT_TEST( driver_loopback_full_queue_drops_and_counts );
REGISTER_UNIT_TEST( driver_loopback_destroy_frees_the_port );

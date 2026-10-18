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
#include "rocketnet_connect_token.h"
#include "sodium/sodium.h"

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


// Cumulative wire totals over the in-process loopback driver: time is only an
// argument, no socket is opened, and every datagram the driver carries is
// counted on both ends, handshake and keep-alives included.

struct wire_totals_t
{
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

static void wire_totals_client( struct rocketnet_client_transport_t * client, wire_totals_t * totals )
{
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( client, &stats );
    totals->packets_sent = stats.packets_sent;
    totals->packets_received = stats.packets_received;
    totals->bytes_sent = stats.bytes_sent;
    totals->bytes_received = stats.bytes_received;
}

static void wire_totals_server( struct rocketnet_server_transport_t * server, wire_totals_t * totals )
{
    struct rocketnet_server_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_stats( server, &stats );
    totals->packets_sent = stats.packets_sent;
    totals->packets_received = stats.packets_received;
    totals->bytes_sent = stats.bytes_sent;
    totals->bytes_received = stats.bytes_received;
}

static void wire_totals_slot( struct rocketnet_server_transport_t * server, int slot, wire_totals_t * totals )
{
    struct rocketnet_server_transport_client_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_client_stats( server, slot, &stats );
    totals->packets_sent = stats.packets_sent;
    totals->packets_received = stats.packets_received;
    totals->bytes_sent = stats.bytes_sent;
    totals->bytes_received = stats.bytes_received;
}

static void wire_totals_pump( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, double * time, int steps )
{
    for ( int i = 0; i < steps; i++ )
    {
        rocketnet_client_transport_update( client, *time );
        rocketnet_server_transport_update( server, *time );
        *time += 0.001;
    }
}

static bool wire_totals_connect( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, double * time )
{
    for ( int i = 0; i < 5000; i++ )
    {
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
        {
            wire_totals_pump( client, server, time, 4 );
            return true;
        }
        wire_totals_pump( client, server, time, 1 );
    }
    return false;
}

static const uint8_t wire_totals_backend_seed[32] =
{
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
};

static struct rocketnet_server_transport_t * wire_totals_server_create( const char * public_address, double time, uint8_t * server_public_key, uint8_t * server_secret_key )
{
    uint8_t backend_public_key[32];
    uint8_t backend_secret_key[64];
    crypto_sign_seed_keypair( backend_public_key, backend_secret_key, wire_totals_backend_seed );
    crypto_box_keypair( server_public_key, server_secret_key );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = 1;
    config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    memcpy( config.server_public_key, server_public_key, 32 );
    memcpy( config.server_secret_key, server_secret_key, 32 );
    snprintf( config.driver, sizeof( config.driver ), "%s", "loopback" );

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, public_address, &config, time, &status );
    rocketnet_unit_check( status == ROCKETNET_STATUS_OK );
    return server;
}

static struct rocketnet_client_transport_t * wire_totals_client_create( const char * address, int server_port, const uint8_t * server_public_key, uint64_t client_id, double time )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = (uint64_t) 1 << 40;
    signed_fields.expire_timestamp = signed_fields.create_timestamp + 30;
    signed_fields.timeout_seconds = 10;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = (uint8_t) ( server_port & 0xff );
    signed_fields.ipv4_address[5] = (uint8_t) ( ( server_port >> 8 ) & 0xff );
    memcpy( signed_fields.server_public_key, server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = client_id;
    randombytes_buf( private_fields.client_to_server_key, ROCKETNET_KEY_BYTES );
    randombytes_buf( private_fields.server_to_client_key, ROCKETNET_KEY_BYTES );

    uint8_t backend_secret_key[64];
    uint8_t backend_public_key[32];
    crypto_sign_seed_keypair( backend_public_key, backend_secret_key, wire_totals_backend_seed );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_connect_token( &signed_fields, &private_fields, backend_secret_key, token ) )
        return NULL;

    (void) address;
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.driver, sizeof( config.driver ), "%s", "loopback" );
    return rocketnet_client_transport_create( NULL, token, &config, time );
}

static void wire_totals_send_payloads( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, double * time )
{
    uint8_t payload[32];
    for ( int round = 0; round < 3; round++ )
    {
        memset( payload, round + 1, sizeof( payload ) );
        for ( int i = 0; i < 4; i++ )
        {
            rocketnet_client_transport_send_packet( client, payload, (int) sizeof( payload ) );
            rocketnet_server_transport_send_packet( server, 0, payload, (int) sizeof( payload ) );
        }
        rocketnet_server_transport_send_packets( server );
        wire_totals_pump( client, server, time, 4 );
    }

    const int num_payloads = rocketnet_server_transport_num_payloads( server );
    for ( int i = 0; i < num_payloads; i++ )
    {
        int payload_client_index = -1;
        int payload_bytes = 0;
        rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
    }
    uint8_t received[64];
    while ( rocketnet_client_transport_receive_packet( client, received, (int) sizeof( received ) ) > 0 ) {}
}

static void wire_totals_count_every_packet_and_byte()
{
    uint8_t server_public_key[32];
    uint8_t server_secret_key[32];
    struct rocketnet_server_transport_t * server = wire_totals_server_create( "127.0.0.1:0", 1000.0, server_public_key, server_secret_key );
    rocketnet_unit_check( server != NULL );

    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const int server_port = rocketnet_server_transport_port( server );
    snprintf( address, sizeof( address ), "127.0.0.1:%d", server_port );

    double time = 1000.0;
    struct rocketnet_client_transport_t * client = wire_totals_client_create( address, server_port, server_public_key, 0x5701ULL, time );
    rocketnet_unit_check( client != NULL );
    rocketnet_unit_check( wire_totals_connect( client, server, &time ) );

    wire_totals_t client_totals, server_totals, slot_totals;
    wire_totals_client( client, &client_totals );
    wire_totals_server( server, &server_totals );
    wire_totals_slot( server, 0, &slot_totals );

    rocketnet_unit_check( client_totals.packets_sent > 0 );
    rocketnet_unit_check( client_totals.packets_received > 0 );

    wire_totals_send_payloads( client, server, &time );
    wire_totals_client( client, &client_totals );
    wire_totals_server( server, &server_totals );
    wire_totals_slot( server, 0, &slot_totals );

    rocketnet_unit_check( client_totals.packets_sent == server_totals.packets_received );
    rocketnet_unit_check( client_totals.bytes_sent == server_totals.bytes_received );
    rocketnet_unit_check( server_totals.packets_sent == client_totals.packets_received );
    rocketnet_unit_check( server_totals.bytes_sent == client_totals.bytes_received );
    rocketnet_unit_check( client_totals.packets_sent == slot_totals.packets_received );
    rocketnet_unit_check( client_totals.bytes_sent == slot_totals.bytes_received );
    rocketnet_unit_check( server_totals.packets_sent == slot_totals.packets_sent );
    rocketnet_unit_check( server_totals.bytes_sent == slot_totals.bytes_sent );
    rocketnet_unit_check( server_totals.packets_received == slot_totals.packets_received );
    rocketnet_unit_check( server_totals.bytes_received == slot_totals.bytes_received );

    const uint64_t packets_before_disconnect = client_totals.packets_sent;
    rocketnet_client_transport_disconnect( client );
    wire_totals_pump( client, server, &time, 8 );
    wire_totals_client( client, &client_totals );
    wire_totals_server( server, &server_totals );

    rocketnet_unit_check( client_totals.packets_sent > packets_before_disconnect );
    rocketnet_unit_check( client_totals.packets_sent == server_totals.packets_received );
    rocketnet_unit_check( client_totals.bytes_sent == server_totals.bytes_received );
    rocketnet_unit_check( server_totals.packets_sent == client_totals.packets_received );
    rocketnet_unit_check( server_totals.bytes_sent == client_totals.bytes_received );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
    printf( "wire_totals_count_every_packet_and_byte\n" );
}

static void wire_totals_reset_per_connection_not_per_server()
{
    uint8_t server_public_key[32];
    uint8_t server_secret_key[32];
    struct rocketnet_server_transport_t * server = wire_totals_server_create( "127.0.0.1:0", 1000.0, server_public_key, server_secret_key );
    rocketnet_unit_check( server != NULL );

    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const int server_port = rocketnet_server_transport_port( server );
    snprintf( address, sizeof( address ), "127.0.0.1:%d", server_port );

    double time = 1000.0;
    struct rocketnet_client_transport_t * first = wire_totals_client_create( address, server_port, server_public_key, 0x5702ULL, time );
    rocketnet_unit_check( first != NULL );
    rocketnet_unit_check( wire_totals_connect( first, server, &time ) );
    wire_totals_send_payloads( first, server, &time );

    wire_totals_t first_slot, first_server;
    wire_totals_slot( server, 0, &first_slot );
    wire_totals_server( server, &first_server );
    rocketnet_unit_check( first_slot.packets_sent > 0 );
    rocketnet_unit_check( first_slot.packets_received > 0 );

    rocketnet_client_transport_disconnect( first );
    for ( int i = 0; i < 200 && rocketnet_server_transport_num_connected_clients( server ) > 0; i++ )
        wire_totals_pump( first, server, &time, 1 );

    wire_totals_t free_slot;
    wire_totals_slot( server, 0, &free_slot );
    rocketnet_unit_check( free_slot.packets_sent == 0 );
    rocketnet_unit_check( free_slot.packets_received == 0 );
    rocketnet_unit_check( free_slot.bytes_sent == 0 );
    rocketnet_unit_check( free_slot.bytes_received == 0 );

    struct rocketnet_client_transport_t * second = wire_totals_client_create( address, server_port, server_public_key, 0x5703ULL, time );
    rocketnet_unit_check( second != NULL );
    rocketnet_unit_check( wire_totals_connect( second, server, &time ) );

    wire_totals_t second_slot, second_server, second_client;
    wire_totals_slot( server, 0, &second_slot );
    wire_totals_server( server, &second_server );
    wire_totals_client( second, &second_client );

    rocketnet_unit_check( second_slot.packets_sent == second_client.packets_received );
    rocketnet_unit_check( second_slot.packets_received == second_client.packets_sent );
    rocketnet_unit_check( second_slot.bytes_sent == second_client.bytes_received );
    rocketnet_unit_check( second_slot.bytes_received == second_client.bytes_sent );
    rocketnet_unit_check( second_slot.packets_sent < first_slot.packets_sent );
    rocketnet_unit_check( second_slot.packets_received < first_slot.packets_received );
    rocketnet_unit_check( second_server.packets_sent > first_server.packets_sent );
    rocketnet_unit_check( second_server.packets_received > first_server.packets_received );

    rocketnet_client_transport_destroy( second );
    rocketnet_client_transport_destroy( first );
    rocketnet_server_transport_destroy( server );
    printf( "wire_totals_reset_per_connection_not_per_server\n" );
}

REGISTER_UNIT_TEST( wire_totals_count_every_packet_and_byte );
REGISTER_UNIT_TEST( wire_totals_reset_per_connection_not_per_server );

REGISTER_UNIT_TEST( driver_loopback_binds_and_discovers_a_port );
REGISTER_UNIT_TEST( driver_loopback_exchanges_both_ways );
REGISTER_UNIT_TEST( driver_loopback_full_queue_drops_and_counts );
REGISTER_UNIT_TEST( driver_loopback_destroy_frees_the_port );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A destroyed client's receive thread always exits. Destroy stores quit and
// wakes the receive thread's wait explicitly before the join, so the join
// does not depend on the wait's timeout firing. The clients run on the
// in-memory loopback driver: no socket opens, and the game clock starts at
// 1000.0. The only waits are the receive thread's bounded 1 ms wait and the
// platform thread wait that a wake ends.

#include "test_unit_registry.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

struct client_receive_thread_quit_waiter_t
{
    std::atomic<int> waits_done;
};

static void client_receive_thread_quit_long_wait( void * data )
{
    client_receive_thread_quit_waiter_t * waiter = (client_receive_thread_quit_waiter_t*) data;
    // an hour-long wait: only the wake ends it within the test
    rocketnet_platform_thread_wait( 3600.0 );
    // once woken, every later wait returns at once
    rocketnet_platform_thread_wait( 3600.0 );
    waiter->waits_done.store( 2, std::memory_order_release );
}

// the wake ends a wait whose timeout would not fire for an hour, whether the
// wake lands before the wait starts or during it, and stays set for later waits
static void client_receive_thread_quit_wake_ends_a_long_wait()
{
    for ( int i = 0; i < 100; i++ )
    {
        client_receive_thread_quit_waiter_t waiter;
        waiter.waits_done.store( 0 );
        rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, client_receive_thread_quit_long_wait, &waiter );
        check( thread );
        rocketnet_platform_thread_wake( thread );
        rocketnet_platform_thread_join( thread );
        rocketnet_platform_thread_destroy( thread );
        check( waiter.waits_done.load( std::memory_order_acquire ) == 2 );
    }
    printf( "client_receive_thread_quit_wake_ends_a_long_wait\n" );
}

// 1000 create and destroy cycles of a client transport whose receive thread
// polls the loopback driver: every destroy returns
static void client_receive_thread_quit_destroy_joins_1000_times()
{
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.driver, sizeof( client_config.driver ), "%s", "loopback" );

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = 4102444800ULL;      // the client checks expiry against the unix clock
    signed_fields.timeout_seconds = 10;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 127, 0, 0, 1, 0x40, 0x9c };     // 127.0.0.1:40000, never opened
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = 1;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = 4102444800ULL;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token ) );

    const double time = 1000.0;
    for ( int i = 0; i < 1000; i++ )
    {
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, time );
        check( client != NULL );
        rocketnet_client_transport_destroy( client );
    }
    printf( "client_receive_thread_quit_destroy_joins_1000_times\n" );
}

REGISTER_UNIT_TEST( client_receive_thread_quit_wake_ends_a_long_wait );
REGISTER_UNIT_TEST( client_receive_thread_quit_destroy_joins_1000_times );

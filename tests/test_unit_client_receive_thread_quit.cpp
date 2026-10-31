/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Step-driven destroy without a worker: this unit case does NOT exercise
// the threaded quit flag. On the threaded path destroy stores quit and wakes
// the receive thread's wait explicitly before the join; that path uses real
// threads and real waits, so its cases (a wake before and during a long
// wait, and 1000 create and destroy cycles) remain the quit/wake oracle in
// the functional suite (client_receive_thread_quit_*). This unit case's
// real contract is narrower: a DEV-only step-driven client on the fake
// transport has no worker thread, caller stepping drives the I/O core to
// stopped on the calling thread, and destroy then closes the fake
// transport exactly once per cycle. Single threaded: no thread, no socket,
// no sleep and no wall clock; the game clock starts at 1000.0 and the
// token's unix time is supplied.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_wire.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// four create and destroy cycles of a step-driven client without a worker:
// every destroy returns and closes its fake transport exactly once

static void test_client_step_driven_destroy_returns_and_closes_once_without_worker()
{
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    client_config.step_driven = 1;

    // the token's create time is the supplied unix time; it expires 30 s later

    const uint64_t unix_timestamp = ROCKETNET_TEST_CREATE_TS;

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = unix_timestamp;
    signed_fields.expire_timestamp = unix_timestamp + 30;
    signed_fields.timeout_seconds = 10;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 127, 0, 0, 1, 0x40, 0x9c };     // 127.0.0.1:40000; the fake transport opens no socket
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 1;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = unix_timestamp + 30;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token ) );

    rocketnet_fake_client_transport_io_t fake = {};
    const double time = 1000.0;

    for ( int i = 0; i < 4; i++ )
    {
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_step_driven_at_timestamp(
            NULL, token, &client_config, time, unix_timestamp, &rocketnet_fake_client_operations_io, &fake );
        check( client != NULL );
        rn_test_client_transport_stop_and_destroy( client );
        check( fake.destroy_calls == i + 1 );
    }
}

REGISTER_UNIT_TEST( test_client_step_driven_destroy_returns_and_closes_once_without_worker );

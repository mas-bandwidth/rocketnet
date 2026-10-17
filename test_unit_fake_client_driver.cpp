/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The in-memory fake client driver (test/fake_client_driver.h) pins the I/O
// step's driver contract (docs/CLIENT-IO.md "The I/O step"): each table opens
// through rocketnet_client_driver_open, wake is level-triggered and cleared by
// wait, and receive_batch drains in order with per-packet stride layout.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "rocketnet_address.h"

// Each table opens through rocketnet_client_driver_open with the fake as the
// context and closes with destroy counted once.
static void fake_client_driver_opens_with_all_eight()
{
    const rocketnet_fake_client_driver_operations_t * tables[] =
    {
        &rocketnet_fake_client_operations_base,
        &rocketnet_fake_client_operations_io,
        &rocketnet_fake_client_operations_batch
    };
    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_fake_client_driver_t fake = {};
        rocketnet_client_driver_config_t config = { "fake", NULL };
        rocketnet_address_t bind = {};
        rocketnet_client_driver_t driver = {};
        const bool opened = rocketnet_client_driver_open(
            (const rocketnet_client_driver_operations_t *) tables[i],
            &fake, &config, &bind, &driver );
        rocketnet_unit_check( opened );
        rocketnet_unit_check( driver.instance == &fake );
        rocketnet_client_driver_close( &driver );
        rocketnet_unit_check( fake.destroy_calls == 1 );
    }
}

// wait is false with nothing; wake makes the next wait true; the following
// wait is false again; a pushed datagram makes wait true without consuming it.
static void fake_client_wake_is_level_triggered()
{
    rocketnet_fake_client_driver_t fake = {};
    rocketnet_unit_check( !rocketnet_fake_client_operations_io.wait( &fake, 1.0 ) );
    rocketnet_unit_check( fake.wait_calls == 1 );
    rocketnet_unit_check( fake.last_wait_timeout == 1.0 );

    rocketnet_fake_client_operations_io.wake( &fake );
    rocketnet_unit_check( fake.wake_calls == 1 );
    rocketnet_unit_check( rocketnet_fake_client_operations_io.wait( &fake, 0.5 ) );
    rocketnet_unit_check( !rocketnet_fake_client_operations_io.wait( &fake, 0.25 ) );

    uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_address_t from = {};
    from.port = 99;
    rocketnet_fake_client_push( &fake, &from, data, 4 );
    rocketnet_unit_check( rocketnet_fake_client_operations_io.wait( &fake, 0.0 ) );
    rocketnet_unit_check( fake.inbound_count == 1 );
}

// Three pushed datagrams of 10, 20 and 30 bytes from distinct ports drain by
// receive_batch in order with stride 2048: 2, then 1, then 0, addresses and
// bytes matching.
static void fake_client_receive_batch_layout()
{
    rocketnet_fake_client_driver_t fake = {};
    uint8_t d1[10] = {}, d2[20] = {}, d3[30] = {};
    rocketnet_address_t a1 = {}, a2 = {}, a3 = {};
    rocketnet_address_load_ipv4( &a1, 0x7f000001, 101 );
    rocketnet_address_load_ipv4( &a2, 0x7f000001, 202 );
    rocketnet_address_load_ipv4( &a3, 0x7f000001, 303 );
    rocketnet_fake_client_push( &fake, &a1, d1, 10 );
    rocketnet_fake_client_push( &fake, &a2, d2, 20 );
    rocketnet_fake_client_push( &fake, &a3, d3, 30 );

    uint8_t storage[2][2048];
    rocketnet_address_t from[2];
    int bytes[2];

    int got = rocketnet_fake_client_operations_batch.receive_batch( &fake, from, (uint8_t*) storage, 2048, bytes, 2 );
    rocketnet_unit_check( got == 2 );
    rocketnet_unit_check( bytes[0] == 10 && bytes[1] == 20 );
    rocketnet_unit_check( rocketnet_address_equal( &from[0], &a1 ) );
    rocketnet_unit_check( rocketnet_address_equal( &from[1], &a2 ) );

    got = rocketnet_fake_client_operations_batch.receive_batch( &fake, from, (uint8_t*) storage, 2048, bytes, 2 );
    rocketnet_unit_check( got == 1 );
    rocketnet_unit_check( bytes[0] == 30 );
    rocketnet_unit_check( rocketnet_address_equal( &from[0], &a3 ) );

    got = rocketnet_fake_client_operations_batch.receive_batch( &fake, from, (uint8_t*) storage, 2048, bytes, 2 );
    rocketnet_unit_check( got == 0 );
}

REGISTER_UNIT_TEST( fake_client_driver_opens_with_all_eight );
REGISTER_UNIT_TEST( fake_client_wake_is_level_triggered );
REGISTER_UNIT_TEST( fake_client_receive_batch_layout );

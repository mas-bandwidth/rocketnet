/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "../sdk/rocketnet_driver.h"

namespace
{
int g_create_calls = 0;
int g_destroy_calls = 0;
int g_fake_instance = 0;

void * fake_create( void *, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * )
{
    ++g_create_calls;
    return &g_fake_instance;
}

void fake_destroy( void * )
{
    ++g_destroy_calls;
}

void fake_send( void *, const rocketnet_address_t *, const uint8_t *, int )
{
}

int fake_receive( void *, rocketnet_address_t *, uint8_t *, int )
{
    return 0;
}

bool fake_wait( void *, bool, double )
{
    return false;
}

void fake_wake( void * )
{
}

rocketnet_client_transport_io_operations_t base_table()
{
    rocketnet_client_transport_io_operations_t ops = {};
    ops.create = fake_create;
    ops.destroy = fake_destroy;
    ops.send = fake_send;
    ops.receive = fake_receive;
    return ops;
}
}

static void test_client_transport_io_open_refuses_wait_without_wake()
{
    g_create_calls = 0;
    g_destroy_calls = 0;
    rocketnet_client_transport_io_operations_t ops = base_table();
    ops.wait = fake_wait;
    const rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_unit_check(!rocketnet_client_transport_io_open(&ops, NULL, &config, &bind, &transport));
    rocketnet_unit_check(g_create_calls == 0);
    rocketnet_unit_check(transport.instance == NULL);
    rocketnet_unit_check(transport.operations == NULL);
}
REGISTER_UNIT_TEST(test_client_transport_io_open_refuses_wait_without_wake);

static void test_client_transport_io_open_refuses_wake_without_wait()
{
    g_create_calls = 0;
    g_destroy_calls = 0;
    rocketnet_client_transport_io_operations_t ops = base_table();
    ops.wake = fake_wake;
    const rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_unit_check(!rocketnet_client_transport_io_open(&ops, NULL, &config, &bind, &transport));
    rocketnet_unit_check(g_create_calls == 0);
    rocketnet_unit_check(transport.instance == NULL);
    rocketnet_unit_check(transport.operations == NULL);
}
REGISTER_UNIT_TEST(test_client_transport_io_open_refuses_wake_without_wait);

static void test_client_transport_io_open_accepts_wait_and_wake()
{
    g_create_calls = 0;
    g_destroy_calls = 0;
    rocketnet_client_transport_io_operations_t ops = base_table();
    ops.wait = fake_wait;
    ops.wake = fake_wake;
    const rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_unit_check(rocketnet_client_transport_io_open(&ops, NULL, &config, &bind, &transport));
    rocketnet_unit_check(g_create_calls == 1);
    rocketnet_unit_check(transport.instance == &g_fake_instance);
    rocketnet_client_transport_io_close(&transport);
    rocketnet_unit_check(g_destroy_calls == 1);
    rocketnet_unit_check(transport.instance == NULL);
}
REGISTER_UNIT_TEST(test_client_transport_io_open_accepts_wait_and_wake);

static void test_client_transport_io_open_accepts_base_four()
{
    g_create_calls = 0;
    g_destroy_calls = 0;
    rocketnet_client_transport_io_operations_t ops = base_table();
    const rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_unit_check(rocketnet_client_transport_io_open(&ops, NULL, &config, &bind, &transport));
    rocketnet_unit_check(g_create_calls == 1);
    rocketnet_unit_check(transport.instance == &g_fake_instance);
    rocketnet_client_transport_io_close(&transport);
    rocketnet_unit_check(g_destroy_calls == 1);
    rocketnet_unit_check(transport.instance == NULL);
}
REGISTER_UNIT_TEST(test_client_transport_io_open_accepts_base_four);

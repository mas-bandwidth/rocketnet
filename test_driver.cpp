/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include "rocketnet_config.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "driver check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

struct lifecycle_t { bool fail; int creates; int destroys; int stops; };
static void * create( void * context, const rocketnet_server_driver_config_t *, rocketnet_address_t * bind )
{
    lifecycle_t * state = (lifecycle_t*) context;
    ++state->creates;
    bind->port = 12345;
    return state->fail ? NULL : context;
}
static void destroy( void * instance ) { ++((lifecycle_t*) instance)->destroys; }
static bool stop( void * instance ) { return ++((lifecycle_t*) instance)->stops >= 3; }
static void send( void *, const rocketnet_platform_send_t *, int ) {}
static int receive( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) { return 0; }

static void test_driver_lifecycle()
{
    rocketnet_server_driver_operations_t operations = {};
    operations.create = create;
    operations.destroy = destroy;
    operations.stop = stop;
    operations.send_batch = send;
    operations.receive_batch = receive;
    rocketnet_server_driver_t driver = {};
    rocketnet_address_t bind;
    CHECK(rocketnet_address_parse(&bind, "127.0.0.1:0"));
    const rocketnet_address_t original = bind;
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    const rocketnet_server_driver_config_t setup = { "fixture", &config, "127.0.0.1:0" };
    lifecycle_t state = {};

    // A malformed extension is refused before invoking its factory.
    operations.flags = ROCKETNET_DRIVER_STAGED;
    CHECK(!rocketnet_server_driver_open(&operations, &state, &setup, &bind, &driver));
    CHECK(state.creates == 0);
    operations.flags = 0;
    operations.destroy = NULL;
    CHECK(!rocketnet_server_driver_open(&operations, &state, &setup, &bind, &driver));
    CHECK(state.creates == 0);
    operations.destroy = destroy;

    // A factory may modify its tentative address then refuse. The next
    // candidate must still see the original port, with no live handle.
    state.fail = true;
    CHECK(!rocketnet_server_driver_open(&operations, &state, &setup, &bind, &driver));
    CHECK(rocketnet_address_equal(&bind, &original));
    CHECK(!driver.instance && !driver.operations);
    rocketnet_server_driver_close(&driver);
    CHECK(state.destroys == 0);

    state.fail = false;
    CHECK(rocketnet_server_driver_open(&operations, &state, &setup, &bind, &driver));
    CHECK(driver.instance == &state && bind.port == 12345);
    CHECK(!rocketnet_server_driver_open(&operations, &state, &setup, &bind, &driver));
    CHECK(state.creates == 2); // An existing instance is never overwritten.
    CHECK(!rocketnet_server_driver_stop(&driver));
    CHECK(!rocketnet_server_driver_stop(&driver));
    CHECK(rocketnet_server_driver_stop(&driver));
    rocketnet_server_driver_close(&driver);
    rocketnet_server_driver_close(&driver);
    CHECK(state.destroys == 1 && !driver.instance && !driver.operations);
    CHECK(rocketnet_server_driver_stop(&driver));
    printf("test_driver_lifecycle\n");
}

static void test_driver_registry()
{
    int field_count;
    const auto * fields = rocketnet_server_transport_config_fields(&field_count);
    bool found_queues = false;
    for (int i = 0; i < field_count; ++i)
        if (strcmp(fields[i].name, "num_xdp_queues") == 0)
        {
            CHECK(fields[i].max_value == ROCKETNET_XDP_MAX_QUEUES);
            found_queues = true;
        }
    CHECK(found_queues); // The BPF map must cover every admitted queue index.
    CHECK(ROCKETNET_DRIVER_PORTABLE == 0 && ROCKETNET_DRIVER_GSO == 1 &&
          ROCKETNET_DRIVER_XDP == 2 && ROCKETNET_DRIVER_MSGX == 3 && ROCKETNET_DRIVER_IOCP == 4);
    for ( int i = 0; i < ROCKETNET_NUM_DRIVERS; ++i )
    {
        const rocketnet_driver_definition_t * definition = rocketnet_driver_definition(i);
        CHECK(definition && definition->name[0]);
        CHECK(strstr(rocketnet_driver_names(), definition->name));
        for ( int j = 0; j < i; ++j ) CHECK(strcmp(definition->name, rocketnet_driver_name(j)) != 0);
        CHECK(rocketnet_driver_discovers_bound_port(i) == definition->discovers_bound_port);
    }
    CHECK(!rocketnet_driver_definition(-1) && !rocketnet_driver_definition(ROCKETNET_NUM_DRIVERS));
    int count;
    const int * servers = rocketnet_server_driver_availability(&count);
    CHECK(count > 0);
    for ( int i = 0; i < count; ++i )
    {
        const rocketnet_driver_definition_t * definition = rocketnet_driver_definition(servers[i]);
        CHECK(definition->backend && definition->backend->server);
        if (i) CHECK(rocketnet_driver_definition(servers[i-1])->server_preference <= definition->server_preference);
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    CHECK(count == 3 && servers[0] == ROCKETNET_DRIVER_XDP && servers[1] == ROCKETNET_DRIVER_GSO && servers[2] == ROCKETNET_DRIVER_PORTABLE);
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    CHECK(count == 2 && servers[0] == ROCKETNET_DRIVER_MSGX && servers[1] == ROCKETNET_DRIVER_PORTABLE);
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    CHECK(count == 2 && servers[0] == ROCKETNET_DRIVER_PORTABLE && servers[1] == ROCKETNET_DRIVER_IOCP);
#else
    CHECK(count == 1 && servers[0] == ROCKETNET_DRIVER_PORTABLE);
#endif
    const int * clients = rocketnet_client_driver_availability(&count);
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    CHECK(count == 2 && clients[0] == ROCKETNET_DRIVER_MSGX && clients[1] == ROCKETNET_DRIVER_PORTABLE);
#else
    CHECK(count == 1 && clients[0] == ROCKETNET_DRIVER_PORTABLE);
#endif
    for ( int i = 0; i < count; ++i ) CHECK(rocketnet_driver_definition(clients[i])->backend->client);
    printf("test_driver_registry\n");
}

static void test_buffered_driver()
{
    const rocketnet_driver_backend_t * backend = rocketnet_driver_definition(ROCKETNET_DRIVER_PORTABLE)->backend;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config(&server_config);
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config(&client_config);
    const rocketnet_server_driver_config_t server_setup = { "portable", &server_config, "127.0.0.1:0" };
    const rocketnet_client_driver_config_t client_setup = { "portable", &client_config };
    rocketnet_address_t server_address, client_address;
    CHECK(rocketnet_address_parse(&server_address, "127.0.0.1:0"));
    client_address = server_address;
    rocketnet_server_driver_t server = {};
    rocketnet_client_driver_t client = {};
    rocketnet_client_driver_operations_t incomplete = *backend->client;
    incomplete.receive = NULL;
    CHECK(!rocketnet_client_driver_open(&incomplete, NULL, &client_setup, &client_address, &client));
    CHECK(!client.instance && client_address.port == 0);
    CHECK(rocketnet_server_driver_open(backend->server, NULL, &server_setup, &server_address, &server));
    CHECK(rocketnet_client_driver_open(backend->client, NULL, &client_setup, &client_address, &client));
    CHECK(server_address.port && client_address.port);
    const uint8_t data[] = { 10, 20, 30, 40 };
    client.operations->send(client.instance, &server_address, data, sizeof(data));
    rocketnet_address_t from[4];
    uint8_t packets[4][64];
    int bytes[4];
    int received = 0;
    double deadline = rocketnet_platform_time() + 5.0;
    while (!received && rocketnet_platform_time() < deadline)
    {
        received = server.operations->receive_batch(server.instance, from, &packets[0][0], 64, bytes, 4);
        if (!received) rocketnet_platform_sleep(0.001);
    }
    CHECK(received == 1 && bytes[0] == sizeof(data));
    CHECK(memcmp(packets[0], data, sizeof(data)) == 0 && rocketnet_address_equal(&from[0], &client_address));
    rocketnet_platform_send_t packet = { &client_address, data, sizeof(data) };
    server.operations->send_batch(server.instance, &packet, 1);
    deadline = rocketnet_platform_time() + 5.0;
    received = 0;
    while (!received && rocketnet_platform_time() < deadline)
    {
        received = client.operations->receive(client.instance, from, packets[0], 64);
        if (!received) rocketnet_platform_sleep(0.001);
    }
    CHECK(received == sizeof(data) && memcmp(packets[0], data, sizeof(data)) == 0);
    CHECK(rocketnet_address_equal(from, &server_address));
    CHECK(rocketnet_server_driver_stop(&server));
    rocketnet_client_driver_close(&client);
    rocketnet_server_driver_close(&server);
    printf("test_buffered_driver\n");
}

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init());
    rocketnet_forbid_xdp_commit();
    test_driver_registry();
    test_driver_lifecycle();
    test_buffered_driver();
    rocketnet_term();
}

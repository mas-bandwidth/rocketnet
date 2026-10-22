/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "native_shutdown.h"
#include "rocketnet_driver.h"
#include "rocketnet_config.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "transport check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

#include "route_checks.h"

struct lifecycle_t { bool fail; int creates; int destroys; int stops; int polls; };
static void * create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * bind )
{
    lifecycle_t * state = (lifecycle_t*) context;
    ++state->creates;
    bind->port = 12345;
    return state->fail ? NULL : context;
}
static void destroy( void * instance ) { ++((lifecycle_t*) instance)->destroys; }
static void stop( void * instance ) { ++((lifecycle_t*) instance)->stops; }
static bool stopped( void * instance ) { return ++((lifecycle_t*) instance)->polls >= 3; }
static void send( void *, const rocketnet_platform_send_t *, int ) {}
static int receive( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) { return 0; }

static void test_transport_lifecycle()
{
    rocketnet_server_transport_io_operations_t operations = {};
    operations.create = create;
    operations.destroy = destroy;
    operations.stop = stop;
    operations.stopped = stopped;
    operations.send_batch = send;
    operations.receive_batch = receive;
    rocketnet_server_transport_io_t transport = {};
    rocketnet_address_t bind;
    CHECK(rocketnet_address_parse(&bind, "127.0.0.1:0"));
    const rocketnet_address_t original = bind;
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config(&config);
    const rocketnet_server_transport_io_config_t setup = { "fixture", &config, "127.0.0.1:0", NULL };
    lifecycle_t state = {};

    // A malformed extension is refused before invoking its factory.
    operations.flags = ROCKETNET_TRANSPORT_STAGED;
    CHECK(!rocketnet_server_transport_io_open(&operations, &state, &setup, &bind, &transport));
    CHECK(state.creates == 0);
    operations.flags = 0;
    operations.destroy = NULL;
    CHECK(!rocketnet_server_transport_io_open(&operations, &state, &setup, &bind, &transport));
    CHECK(state.creates == 0);
    operations.destroy = destroy;

    // A factory may modify its tentative address then refuse. The next
    // candidate must still see the original port, with no live handle.
    state.fail = true;
    CHECK(!rocketnet_server_transport_io_open(&operations, &state, &setup, &bind, &transport));
    CHECK(rocketnet_address_equal(&bind, &original));
    CHECK(!transport.instance && !transport.operations);
    rocketnet_server_transport_io_close(&transport);
    CHECK(state.destroys == 0);

    state.fail = false;
    CHECK(rocketnet_server_transport_io_open(&operations, &state, &setup, &bind, &transport));
    CHECK(transport.instance == &state && bind.port == 12345);
    CHECK(!rocketnet_server_transport_io_open(&operations, &state, &setup, &bind, &transport));
    CHECK(state.creates == 2); // An existing instance is never overwritten.
    rocketnet_server_transport_io_stop(&transport);
    CHECK(state.stops == 1 && state.polls == 0);
    CHECK(!rocketnet_server_transport_io_stopped(&transport));
    CHECK(!rocketnet_server_transport_io_stopped(&transport));
    CHECK(rocketnet_server_transport_io_stopped(&transport));
    rocketnet_server_transport_io_close(&transport);
    rocketnet_server_transport_io_close(&transport);
    CHECK(state.destroys == 1 && !transport.instance && !transport.operations);
    rocketnet_server_transport_io_stop(&transport);
    CHECK(rocketnet_server_transport_io_stopped(&transport));
    printf("test_transport_lifecycle\n");
}

static void test_transport_registry()
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
    CHECK(ROCKETNET_TRANSPORT_PORTABLE == 0 && ROCKETNET_TRANSPORT_GSO == 1 &&
          ROCKETNET_TRANSPORT_XDP == 2 && ROCKETNET_TRANSPORT_MSGX == 3 && ROCKETNET_TRANSPORT_IOCP == 4 &&
          ROCKETNET_TRANSPORT_LOOPBACK == 5 && ROCKETNET_TRANSPORT_IO_URING == 6 && ROCKETNET_TRANSPORT_RIO == 7);
    for ( int i = 0; i < ROCKETNET_NUM_TRANSPORTS; ++i )
    {
        const rocketnet_transport_definition_t * definition = rocketnet_transport_definition(i);
        CHECK(definition && definition->name[0]);
        CHECK(strstr(rocketnet_transport_names(), definition->name));
        for ( int j = 0; j < i; ++j ) CHECK(strcmp(definition->name, rocketnet_transport_name(j)) != 0);
        CHECK(rocketnet_transport_discovers_bound_port(i) == definition->discovers_bound_port);
    }
    CHECK(!rocketnet_transport_definition(-1) && !rocketnet_transport_definition(ROCKETNET_NUM_TRANSPORTS));
    int count;
    const int * servers = rocketnet_server_transport_io_availability(&count);
    CHECK(count > 0);
    for ( int i = 0; i < count; ++i )
    {
        const rocketnet_transport_definition_t * definition = rocketnet_transport_definition(servers[i]);
        CHECK(definition->backend && definition->backend->server);
        if (i) CHECK(rocketnet_transport_definition(servers[i-1])->server_preference <= definition->server_preference);
    }
    // The production library linked by test_transport omits the portable
    // server from availability; development gives it rank 30. Read the linked
    // registry so these exact lists also cover a development-linked fixture.
    const bool portable_server = rocketnet_transport_definition(ROCKETNET_TRANSPORT_PORTABLE)->server_preference != 0;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    if (rocketnet_transport_definition(ROCKETNET_TRANSPORT_IO_URING)->backend) // built with liburing
    {
        if (portable_server)
            CHECK(count == 5 && servers[0] == ROCKETNET_TRANSPORT_XDP && servers[1] == ROCKETNET_TRANSPORT_GSO &&
                  servers[2] == ROCKETNET_TRANSPORT_IO_URING && servers[3] == ROCKETNET_TRANSPORT_PORTABLE && servers[4] == ROCKETNET_TRANSPORT_LOOPBACK);
        else
            CHECK(count == 4 && servers[0] == ROCKETNET_TRANSPORT_XDP && servers[1] == ROCKETNET_TRANSPORT_GSO &&
                  servers[2] == ROCKETNET_TRANSPORT_IO_URING && servers[3] == ROCKETNET_TRANSPORT_LOOPBACK);
    }
    else
    {
        if (portable_server)
            CHECK(count == 4 && servers[0] == ROCKETNET_TRANSPORT_XDP && servers[1] == ROCKETNET_TRANSPORT_GSO && servers[2] == ROCKETNET_TRANSPORT_PORTABLE && servers[3] == ROCKETNET_TRANSPORT_LOOPBACK);
        else
            CHECK(count == 3 && servers[0] == ROCKETNET_TRANSPORT_XDP && servers[1] == ROCKETNET_TRANSPORT_GSO && servers[2] == ROCKETNET_TRANSPORT_LOOPBACK);
    }
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    if (portable_server)
        CHECK(count == 3 && servers[0] == ROCKETNET_TRANSPORT_MSGX && servers[1] == ROCKETNET_TRANSPORT_PORTABLE && servers[2] == ROCKETNET_TRANSPORT_LOOPBACK);
    else
        CHECK(count == 2 && servers[0] == ROCKETNET_TRANSPORT_MSGX && servers[1] == ROCKETNET_TRANSPORT_LOOPBACK);
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if (portable_server)
        CHECK(count == 4 && servers[0] == ROCKETNET_TRANSPORT_PORTABLE && servers[1] == ROCKETNET_TRANSPORT_IOCP &&
              servers[2] == ROCKETNET_TRANSPORT_RIO && servers[3] == ROCKETNET_TRANSPORT_LOOPBACK);
    else
        CHECK(count == 3 && servers[0] == ROCKETNET_TRANSPORT_IOCP && servers[1] == ROCKETNET_TRANSPORT_RIO &&
              servers[2] == ROCKETNET_TRANSPORT_LOOPBACK);
#else
    if (portable_server)
        CHECK(count == 2 && servers[0] == ROCKETNET_TRANSPORT_PORTABLE && servers[1] == ROCKETNET_TRANSPORT_LOOPBACK);
    else
        CHECK(count == 1 && servers[0] == ROCKETNET_TRANSPORT_LOOPBACK);
#endif
    const int * clients = rocketnet_client_transport_io_availability(&count);
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    CHECK(count == 3 && clients[0] == ROCKETNET_TRANSPORT_MSGX && clients[1] == ROCKETNET_TRANSPORT_PORTABLE && clients[2] == ROCKETNET_TRANSPORT_LOOPBACK);
#else
    CHECK(count == 2 && clients[0] == ROCKETNET_TRANSPORT_PORTABLE && clients[1] == ROCKETNET_TRANSPORT_LOOPBACK);
#endif
    for ( int i = 0; i < count; ++i ) CHECK(rocketnet_transport_definition(clients[i])->backend->client);
    printf("test_transport_registry\n");
}

static void test_transport_pin_only()
{
    CHECK(rocketnet_transport_pin_only(ROCKETNET_TRANSPORT_LOOPBACK));
    for ( int i = 0; i < ROCKETNET_NUM_TRANSPORTS; ++i )
        if ( i != ROCKETNET_TRANSPORT_LOOPBACK ) CHECK(!rocketnet_transport_pin_only(i));

    int num;
    const int * servers = rocketnet_server_transport_io_availability(&num);
    struct rocketnet_transport_ladder_t ladder;
    int auto_count = 0;
    for ( int i = 0; i < num; ++i )
        if ( !rocketnet_transport_pin_only(servers[i]) ) ++auto_count;

    CHECK(rocketnet_transport_ladder_parse("auto", servers, num, &ladder));
    CHECK(ladder.num_candidates == auto_count);
    for ( int i = 0; i < ladder.num_candidates; ++i ) CHECK(!rocketnet_transport_pin_only(ladder.candidates[i]));
    int ai = 0;
    for ( int i = 0; i < num; ++i )
        if ( !rocketnet_transport_pin_only(servers[i]) )
            CHECK(ladder.candidates[ai++] == servers[i]);

    CHECK(rocketnet_transport_ladder_parse(NULL, servers, num, &ladder));
    CHECK(ladder.num_candidates == auto_count);

    CHECK(rocketnet_transport_ladder_parse("loopback", servers, num, &ladder));
    CHECK(ladder.num_candidates == 1 && ladder.candidates[0] == ROCKETNET_TRANSPORT_LOOPBACK);

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    CHECK(rocketnet_transport_ladder_parse("gso,loopback", servers, num, &ladder));
    CHECK(ladder.num_candidates == 2 && ladder.candidates[0] == ROCKETNET_TRANSPORT_GSO && ladder.candidates[1] == ROCKETNET_TRANSPORT_LOOPBACK);
    CHECK(rocketnet_transport_ladder_parse("iocp", servers, num, &ladder));
    CHECK(ladder.num_candidates == auto_count);
#endif

    const int * clients = rocketnet_client_transport_io_availability(&num);
    CHECK(rocketnet_transport_ladder_parse("auto", clients, num, &ladder));
    for ( int i = 0; i < ladder.num_candidates; ++i ) CHECK(!rocketnet_transport_pin_only(ladder.candidates[i]));
    printf("test_transport_pin_only\n");
}

static void test_buffered_transport()
{
    const rocketnet_transport_backend_t * backend = rocketnet_transport_definition(ROCKETNET_TRANSPORT_PORTABLE)->backend;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config(&server_config);
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config(&client_config);
    const rocketnet_server_transport_io_config_t server_setup = { "portable", &server_config, "127.0.0.1:0", NULL };
    const rocketnet_client_transport_io_config_t client_setup = { "portable", &client_config, NULL };
    rocketnet_address_t server_address, client_address;
    CHECK(rocketnet_address_parse(&server_address, "127.0.0.1:0"));
    client_address = server_address;
    rocketnet_server_transport_io_t server = {};
    rocketnet_client_transport_io_t client = {};
    rocketnet_client_transport_io_operations_t incomplete = *backend->client;
    incomplete.receive = NULL;
    CHECK(!rocketnet_client_transport_io_open(&incomplete, NULL, &client_setup, &client_address, &client));
    CHECK(!client.instance && client_address.port == 0);
    CHECK(rocketnet_server_transport_io_open(backend->server, NULL, &server_setup, &server_address, &server));
    CHECK(rocketnet_client_transport_io_open(backend->client, NULL, &client_setup, &client_address, &client));
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
    rocketnet_server_transport_io_stop(&server);
    rn_native_server_io_wait_stopped(&server);
    rocketnet_client_transport_io_close(&client);
    rocketnet_server_transport_io_close(&server);
    printf("test_buffered_transport\n");
}

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK);
    rocketnet_forbid_xdp_commit();
    test_route_prefixes();
#if defined(__linux__)
    test_route_netlink();
#endif
    test_transport_registry();
    test_transport_pin_only();
    test_transport_lifecycle();
    test_buffered_transport();
    rocketnet_term();
}

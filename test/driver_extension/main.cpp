/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "extension check failed: %s, line %d\n", #x, __LINE__); exit(1); } } while (0)
extern int external_test_creates;
extern int external_test_destroys;
extern int external_test_replay_forgets;
extern int external_test_replay_marks;
extern uint32_t external_test_replay_max_high_water;
extern rocketnet_address_t external_test_replay_last_address;
extern rocketnet_address_t external_test_replay_forgotten_address;
extern bool external_test_replay_has_address;
extern bool external_test_replay_has_forgotten;

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK);
    rocketnet_forbid_xdp_commit();
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config(&server_config);
    server_config.max_clients = 4;
    strcpy(server_config.transport, "external_test");
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &server_config);
    CHECK(server && strcmp(rocketnet_server_transport_io_name(server), "external_test") == 0);
    char address[64];
    snprintf(address, sizeof(address), "127.0.0.1:%u", rocketnet_server_transport_port(server));
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK(rocketnet_write_test_connect_token(address, 1234, (uint64_t)time(NULL), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK);
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config(&client_config);
    strcpy(client_config.transport, "external_test");
    auto * client = rocketnet_client_transport_create(NULL, token, &client_config, rocketnet_time());
    CHECK(client && strcmp(rocketnet_client_transport_io_name(client), "external_test") == 0);
    const uint8_t payload[] = { 1, 4, 9, 16 };
    bool sent = false, echoed = false;
    double deadline = rocketnet_time() + 10.0;
    while (!echoed && rocketnet_time() < deadline)
    {
        rocketnet_client_transport_update(client, rocketnet_time());
        rocketnet_server_transport_update(server, rocketnet_time());
        if (!sent && rocketnet_client_transport_state(client) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED)
        {
            rocketnet_client_transport_send_packet(client, payload, sizeof(payload));
            sent = true;
        }
        for (int i = 0; i < rocketnet_server_transport_num_payloads(server); ++i)
        {
            int index, bytes;
            const uint8_t * data = rocketnet_server_transport_payload(server, i, &index, &bytes);
            CHECK(bytes == sizeof(payload) && memcmp(data, payload, sizeof(payload)) == 0);
            rocketnet_server_transport_send_packet(server, index, data, bytes);
        }
        rocketnet_server_transport_send_packets(server);
        uint8_t packet[64];
        int bytes = rocketnet_client_transport_receive_packet(client, packet, sizeof(packet));
        if (bytes)
        {
            CHECK(bytes == sizeof(payload) && memcmp(packet, payload, sizeof(payload)) == 0);
            echoed = true;
        }
        rocketnet_sleep(0.001);
    }
    CHECK(echoed);
    CHECK(external_test_replay_has_address);
    CHECK(external_test_replay_max_high_water > 0);
    rocketnet_client_transport_disconnect(client);
    deadline = rocketnet_time() + 5.0;
    while (!external_test_replay_has_forgotten && rocketnet_time() < deadline)
    {
        rocketnet_server_transport_update(server, rocketnet_time());
        rocketnet_sleep(0.001);
    }
    CHECK(external_test_replay_has_forgotten);
    CHECK(rocketnet_address_equal(&external_test_replay_forgotten_address, &external_test_replay_last_address));
    rocketnet_client_transport_destroy(client);
    rocketnet_server_transport_stop(server);
    deadline = rocketnet_time() + 5.0;
    while (rocketnet_server_transport_state(server) != ROCKETNET_SERVER_TRANSPORT_STOPPED && rocketnet_time() < deadline)
    {
        rocketnet_server_transport_update(server, rocketnet_time());
        rocketnet_sleep(0.001);
    }
    CHECK(rocketnet_server_transport_state(server) == ROCKETNET_SERVER_TRANSPORT_STOPPED);
    rocketnet_server_transport_destroy(server);
    CHECK(external_test_creates == 2 && external_test_destroys == 2);
    rocketnet_term();
    puts("test_external_transport_transport_exchange");
}

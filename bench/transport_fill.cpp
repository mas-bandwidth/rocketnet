/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Public-API packet-fill microbenchmark. See transport_fill.md for its limits.
#include "../tests/native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "fill bench failed: %s, line %d\n", #x, __LINE__); exit(1); } } while (0)

int main(int argc, char ** argv)
{
    const char * transport = argc > 1 ? argv[1] : "portable";
    const int payload_bytes = argc > 2 ? atoi(argv[2]) : 64;
    const int batches = argc > 3 ? atoi(argv[3]) : 4000;
    const bool plant = argc > 4 && strcmp(argv[4], "plant") == 0;
    const bool drain = argc > 4 && strcmp(argv[4], "drain") == 0;
    CHECK(payload_bytes >= 1 && payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES && batches > 0);
    CHECK(strlen(transport) < sizeof(rocketnet_server_transport_config_t().transport));
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK);
    rocketnet_forbid_xdp_commit();
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    config.max_clients = 2;
    snprintf(config.transport, sizeof(config.transport), "%s", transport);
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config);
    CHECK(server && strcmp(rocketnet_server_transport_io_name(server), transport) == 0);
    char address[64];
    snprintf(address, sizeof(address), "127.0.0.1:%u", rocketnet_server_transport_port(server));
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK(rocketnet_write_test_connect_token(address, 1234, (uint64_t)time(NULL), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK);
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config(&client_config);
    memcpy(client_config.transport, "portable", sizeof("portable"));
    auto * client = rocketnet_client_transport_create(NULL, token, &client_config, rocketnet_time());
    CHECK(client);
    int client_index = -1;
    const double deadline = rocketnet_time() + 10.0;
    while (rocketnet_time() < deadline)
    {
        rocketnet_client_transport_update(client, rocketnet_time());
        rocketnet_server_transport_update(server, rocketnet_time());
        rocketnet_server_transport_send_packets(server);
        if (rocketnet_client_transport_state(client) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED)
        {
            client_index = rocketnet_client_transport_client_index(client);
            if (rocketnet_server_transport_is_client_connected(server, client_index)) break;
        }
        rocketnet_sleep(0.001);
    }
    CHECK(client_index >= 0 && rocketnet_server_transport_is_client_connected(server, client_index));
    const int batch_size = 128;
    uint8_t received[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    uint64_t wire_bytes = 0, received_packets = 0, drained_packets = 0;
    volatile uint64_t plant_state = 1;
    double elapsed_ns = 0;
    double drain_ns = 0;
    // Warm-up is discarded. Only acquire/write/commit is timed; the flush,
    // update and receive work below supplies realistic storage turnover.
    for (int batch = -500; batch < batches; ++batch)
    {
        rocketnet_client_transport_update(client, rocketnet_time());
        rocketnet_server_transport_update(server, rocketnet_time());
        CHECK(rocketnet_server_transport_is_client_connected(server, client_index));
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < batch_size; ++i)
        {
            rocketnet_packet_context_t context;
            uint8_t * frame = rocketnet_server_transport_start_packet(server, client_index, payload_bytes, &context);
            CHECK(frame);
            memset(frame + context.payload_index, i, size_t(payload_bytes));
            // Instrument honesty: an optional dependent volatile workload must
            // move the timing. It is caller work, never a production SDK branch.
            if (plant)
                for (int j = 0; j < 4096; ++j) plant_state = plant_state * 1664525 + 1013904223;
            const int bytes = rocketnet_server_transport_finish_packet(server, &context, payload_bytes);
            CHECK(bytes > payload_bytes);
            if (batch >= 0) wire_bytes += uint64_t(bytes);
        }
        const auto end = std::chrono::steady_clock::now();
        if (batch >= 0) elapsed_ns += std::chrono::duration<double, std::nano>(end - start).count();
        rocketnet_server_transport_send_packets(server);
        int bytes;
        int drained_here = 0;
        const auto drain_start = std::chrono::steady_clock::now();
        while ((bytes = rocketnet_client_transport_receive_packet(client, received, sizeof(received))) > 0)
        {
            CHECK(bytes == payload_bytes);
            for (int i = 1; i < bytes; ++i) CHECK(received[i] == received[0]);
            ++received_packets;
            ++drained_here;
        }
        const auto drain_end = std::chrono::steady_clock::now();
        if (batch >= 0)
        {
            drained_packets += uint64_t(drained_here);
            if (drain)
                drain_ns += std::chrono::duration<double, std::nano>(drain_end - drain_start).count();
        }
    }
    CHECK(received_packets > 0 && wire_bytes > 0);
    printf("transport=%s bytes=%d packets=%llu fill_ns=%.3f wire_bytes=%llu received=%llu plant=%d plant_state=%llu\n",
        transport, payload_bytes, (unsigned long long)batches * batch_size, elapsed_ns / (batches * batch_size),
        (unsigned long long)wire_bytes, (unsigned long long)received_packets, plant, (unsigned long long)plant_state);
    if (drain)
        printf("drain transport=%s bytes=%d drained=%llu drain_ns=%.3f\n",
            transport, payload_bytes, (unsigned long long)drained_packets,
            drained_packets ? drain_ns / (double)drained_packets : 0.0);
    rn_native_client_transport_stop_and_destroy(client);
    rn_native_server_transport_stop_and_destroy(server);
    rocketnet_term();
}

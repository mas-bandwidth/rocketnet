/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Public-API per-frame main-thread cost microbenchmark. See client_main_thread_cost.md.
#include "../test/native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "client main-thread cost bench failed: %s, line %d\n", #x, __LINE__); exit(1); } } while (0)

// Nearest-rank percentile on the sorted per-frame samples.
static double percentile(std::vector<double> samples, double p)
{
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    if (n == 0) return 0.0;
    const double rank = (p / 100.0) * (double)n;
    size_t index = (size_t)rank;
    if ((double)index < rank) index++;
    if (index > 0) index--;
    return samples[index < n ? index : n - 1];
}

int main(int argc, char ** argv)
{
    const char * transport = argc > 1 ? argv[1] : "portable";
    const int packets_per_frame = argc > 2 ? atoi(argv[2]) : 8;
    const int frames = argc > 3 ? atoi(argv[3]) : 2000;
    const bool plant = argc > 4 && strcmp(argv[4], "plant") == 0;
    CHECK(packets_per_frame >= 1);
    CHECK(frames > 0);
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
    snprintf(client_config.transport, sizeof(client_config.transport), "%s", transport);
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

    const int payload_bytes = 200;
    volatile uint64_t plant_state = 1;
    std::vector<double> send_us;
    std::vector<double> update_us;
    std::vector<double> total_us;
    send_us.reserve(frames);
    update_us.reserve(frames);
    total_us.reserve(frames);
    uint64_t received = 0;
    uint8_t scratch[ROCKETNET_RECEIVE_PAYLOAD_BYTES];

    // Warm-up is discarded. The payload writes, the lockstep server loop
    // (update, reply) and the client drain run outside the timed windows.
    for (int frame = -200; frame < frames; ++frame)
    {
        for (int i = 0; i < packets_per_frame; ++i)
        {
            rocketnet_packet_context_t context;
            uint8_t * payload = rocketnet_client_transport_start_packet(client, payload_bytes, &context);
            CHECK(payload);
            memset(payload + context.payload_index, i, payload_bytes);
            CHECK(rocketnet_client_transport_finish_packet(client, &context, payload_bytes) > payload_bytes);
        }

        const auto send_start = std::chrono::steady_clock::now();
        rocketnet_client_transport_send_packets(client);
        // Instrument honesty: an optional dependent volatile workload must
        // move the timing. It is caller work, never a production SDK branch.
        if (plant)
            for (int j = 0; j < 4096; ++j) plant_state = plant_state * 1664525 + 1013904223;
        const auto send_end = std::chrono::steady_clock::now();

        const auto update_start = std::chrono::steady_clock::now();
        rocketnet_client_transport_update(client, rocketnet_time());
        const auto update_end = std::chrono::steady_clock::now();

        const double send_ns = std::chrono::duration<double, std::nano>(send_end - send_start).count();
        const double update_ns = std::chrono::duration<double, std::nano>(update_end - update_start).count();
        if (frame >= 0)
        {
            send_us.push_back(send_ns / 1000.0);
            update_us.push_back(update_ns / 1000.0);
            total_us.push_back((send_ns + update_ns) / 1000.0);
        }

        // Lockstep pacing, outside both timed windows. The I/O thread sends
        // asynchronously, so send_packets is a hand-off, and an unpaced loop
        // refills the Filling ring faster than one ring can be sent. The
        // server side loops until this frame's payloads have arrived, or a
        // 0.5 s deadline passes, replying to each payload as it comes.
        int frame_payloads = 0;
        const double pace_deadline = rocketnet_time() + 0.5;
        while (true)
        {
            rocketnet_server_transport_update(server, rocketnet_time());
            CHECK(rocketnet_server_transport_is_client_connected(server, client_index));

            const int * reply_client_index;
            uint8_t * const * received_data;
            const int * received_bytes;
            const int num_payloads = rocketnet_server_transport_payloads(server, &reply_client_index, &received_data, &received_bytes);
            for (int i = 0; i < num_payloads; ++i)
            {
                CHECK(received_bytes[i] == payload_bytes);
                rocketnet_packet_context_t context;
                uint8_t * payload = rocketnet_server_transport_start_packet(server, reply_client_index[i], payload_bytes, &context);
                CHECK(payload);
                memset(payload + context.payload_index, 'r', payload_bytes);
                CHECK(rocketnet_server_transport_finish_packet(server, &context, payload_bytes) > payload_bytes);
            }
            rocketnet_server_transport_send_packets(server);
            frame_payloads += num_payloads;
            if (frame_payloads >= packets_per_frame || rocketnet_time() >= pace_deadline) break;
            rocketnet_sleep(0.0001);
        }

        int bytes;
        while ((bytes = rocketnet_client_transport_receive_packet(client, scratch, (int)sizeof(scratch))) > 0)
        {
            CHECK(bytes == payload_bytes);
            ++received;
        }
    }

    CHECK(!send_us.empty());
    CHECK(received > 0);

    printf("client_main_thread_cost transport=%s packets_per_frame=%d frames=%d"
           " send_us_p50=%.3f send_us_p99=%.3f send_us_max=%.3f"
           " update_us_p50=%.3f update_us_p99=%.3f update_us_max=%.3f"
           " total_us_p50=%.3f total_us_p99=%.3f total_us_max=%.3f"
           " received=%llu\n",
           transport, packets_per_frame, frames,
           percentile(send_us, 50.0), percentile(send_us, 99.0), *std::max_element(send_us.begin(), send_us.end()),
           percentile(update_us, 50.0), percentile(update_us, 99.0), *std::max_element(update_us.begin(), update_us.end()),
           percentile(total_us, 50.0), percentile(total_us, 99.0), *std::max_element(total_us.begin(), total_us.end()),
           (unsigned long long)received);
    (void)plant_state;
    rn_native_client_transport_stop_and_destroy(client);
    rn_native_server_transport_stop_and_destroy(server);
    rocketnet_term();
}

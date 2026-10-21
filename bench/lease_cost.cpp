/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Valid-call diagnostic. See lease_cost.md; this does not measure NIC throughput.
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_packet_lease.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "lease bench failed: %s, line %d\n", #x, __LINE__); exit(1); } } while (0)

struct alignas(max_align_t) allocation_t { size_t bytes; };
static std::atomic<size_t> live_bytes{0};
static void * allocate(void *, size_t bytes)
{
    auto * block = (allocation_t *) malloc(sizeof(allocation_t) + bytes);
    if (!block) return NULL;
    block->bytes = bytes;
    live_bytes.fetch_add(bytes);
    return block + 1;
}
static void release(void *, void * pointer)
{
    if (!pointer) return;
    auto * block = (allocation_t *) pointer - 1;
    live_bytes.fetch_sub(block->bytes);
    free(block);
}

class workers_t
{
    std::mutex mutex;
    std::condition_variable ready, done;
    std::vector<std::thread> workers;
    std::function<void(int)> work;
    unsigned generation = 0;
    int completed = 0;
    bool stopping = false;
public:
    workers_t(int count, std::function<void(int)> task) : work(task)
    {
        if (count == 1) return;
        for (int id = 0; id < count; ++id)
            workers.emplace_back([this, id]() {
                unsigned seen = 0;
                std::unique_lock<std::mutex> lock(mutex);
                for (;;)
                {
                    ready.wait(lock, [&] { return stopping || generation != seen; });
                    if (stopping) return;
                    seen = generation;
                    lock.unlock();
                    work(id);
                    lock.lock();
                    if (++completed == (int)workers.size()) done.notify_one();
                }
            });
    }
    void run()
    {
        if (workers.empty()) { work(0); return; }
        std::unique_lock<std::mutex> lock(mutex);
        completed = 0;
        ++generation;
        ready.notify_all();
        done.wait(lock, [&] { return completed == (int)workers.size(); });
    }
    ~workers_t()
    {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        ready.notify_all();
        for (auto & worker : workers) worker.join();
    }
};

int main(int argc, char ** argv)
{
    CHECK(argc >= 6);
    const bool from_server = strcmp(argv[1], "server") == 0;
    CHECK(from_server || strcmp(argv[1], "client") == 0);
    const bool finish = strcmp(argv[2], "finish") == 0;
    const bool access = strcmp(argv[2], "access") == 0;
    CHECK(finish || access || strcmp(argv[2], "abort") == 0);
    const int payload_bytes = atoi(argv[3]), threads = atoi(argv[4]), batches = atoi(argv[5]);
    const bool plant = argc > 6 && strcmp(argv[6], "plant") == 0;
    CHECK(payload_bytes > 0 && payload_bytes <= 4096 && (threads == 1 || threads == 4) && batches > 0);
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    rocketnet_allocator(allocate, release);
    CHECK(rocketnet_init( NULL ) == ROCKETNET_STATUS_OK);
    rocketnet_forbid_xdp_commit();
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    config.max_clients = 2;
    config.max_payload_bytes = 4096;
    memcpy(config.transport, "portable", sizeof("portable"));
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config);
    CHECK(server && strcmp(rocketnet_server_transport_io_name(server), "portable") == 0);
    const size_t server_bytes = live_bytes.load();
    char address[64];
    snprintf(address, sizeof(address), "127.0.0.1:%u", rocketnet_server_transport_port(server));
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK(rocketnet_write_test_connect_token(address, 1234, (uint64_t)time(NULL), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK);
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config(&client_config);
    client_config.max_payload_bytes = 4096;
    memcpy(client_config.transport, "portable", sizeof("portable"));
    auto * client = rocketnet_client_transport_create(NULL, token, &client_config, rocketnet_time());
    CHECK(client);
    const size_t client_bytes = live_bytes.load() - server_bytes;
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
    if (client_index < 0 || !rocketnet_server_transport_is_client_connected(server, client_index))
    {
        fprintf(stderr, "handshake deadline: client state %d, slot %d\n",
                rocketnet_client_transport_state(client), client_index);
        const rocketnet_counters_t * counters[] = {
            rocketnet_client_transport_counters(client), rocketnet_server_transport_counters(server)
        };
        for (int side = 0; side < 2; ++side)
            for (int counter = 0; counter < rocketnet_num_counters(); ++counter)
            {
                const uint64_t value = rocketnet_counter_value(counters[side], counter);
                if (value)
                    fprintf(stderr, "%s %s=%llu\n", side ? "server" : "client",
                            rocketnet_counter_name(counter), (unsigned long long)value);
            }
        rocketnet_client_transport_destroy(client);
        rocketnet_server_transport_destroy(server);
        rocketnet_term();
        CHECK(live_bytes.load() == 0);
        return 1;
    }
    const int batch_size = payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? 64 : 16;
    std::atomic<uint64_t> committed{0}, accessed{0};
    double elapsed_ns = 0;
    uint64_t received = 0;
    {
        workers_t workers(threads, [&](int) {
            uint64_t local_committed = 0, local_accessed = 0;
            volatile uint64_t plant_state = 1;
            for (int i = 0; i < batch_size / threads; ++i)
            {
                rocketnet_packet_context_t context;
                uint8_t * frame = from_server
                    ? rocketnet_server_transport_start_packet(server, client_index, payload_bytes, &context)
                    : rocketnet_client_transport_start_packet(client, payload_bytes, &context);
                CHECK(frame);
                uint8_t * payload = frame + context.payload_index;
                if (access)
                    for (int read = 0; read < 8; ++read)
                    {
                        int capacity = 0;
                        uint8_t * view = from_server
                            ? rocketnet_server_transport_packet_payload(server, &context, &capacity)
                            : rocketnet_client_transport_packet_payload(client, &context, &capacity);
                        CHECK(view == payload && capacity >= payload_bytes);
                        ++local_accessed;
                    }
                memset(payload, 0x5a, payload_bytes);
                if (plant)
                    for (int j = 0; j < 4096; ++j) plant_state = plant_state * 1664525 + 1013904223;
                if (finish)
                {
                    const int bytes = from_server
                        ? rocketnet_server_transport_finish_packet(server, &context, payload_bytes)
                        : rocketnet_client_transport_finish_packet(client, &context, payload_bytes);
                    CHECK(bytes > payload_bytes);
                    ++local_committed;
                }
                else if (from_server) rocketnet_server_transport_abort_packet(server, &context);
                else rocketnet_client_transport_abort_packet(client, &context);
                CHECK(!context.frame);
            }
            committed.fetch_add(local_committed, std::memory_order_relaxed);
            accessed.fetch_add(local_accessed, std::memory_order_relaxed);
        });
        for (int batch = -100; batch < batches; ++batch)
        {
            rocketnet_client_transport_update(client, rocketnet_time());
            rocketnet_server_transport_update(server, rocketnet_time());
            CHECK(rocketnet_server_transport_is_client_connected(server, client_index));
            const auto start = std::chrono::steady_clock::now();
            workers.run();
            const auto end = std::chrono::steady_clock::now();
            if (batch >= 0) elapsed_ns += std::chrono::duration<double, std::nano>(end - start).count();
            rocketnet_server_transport_send_packets(server);
            rocketnet_client_transport_send_packets(client);
            const int count = from_server ? rocketnet_client_transport_num_payloads(client)
                                          : rocketnet_server_transport_num_payloads(server);
            for (int packet = 0; packet < count; ++packet)
            {
                int length = 0, received_client = -1;
                uint8_t * bytes = from_server ? rocketnet_client_transport_payload(client, packet, &length)
                    : rocketnet_server_transport_payload(server, packet, &received_client, &length);
                CHECK(bytes && length == payload_bytes);
                CHECK(from_server || received_client == client_index);
                for (int i = 0; i < length; ++i) CHECK(bytes[i] == 0x5a);
                ++received;
            }
        }
    }
    CHECK(committed.load() == (finish ? uint64_t(batches + 100) * batch_size : 0));
    CHECK(accessed.load() == (access ? uint64_t(batches + 100) * batch_size * 8 : 0));
    CHECK(!finish || received > 0);
    rocketnet_client_transport_destroy(client);
    rocketnet_server_transport_destroy(server);
    rocketnet_term();
    CHECK(live_bytes.load() == 0);
    printf("{\"endpoint\":\"%s\",\"operation\":\"%s\",\"payload_bytes\":%d,\"threads\":%d,\"packets\":%d,\"ns_per_packet\":%.3f,\"received\":%llu,\"server_allocation_bytes\":%zu,\"client_allocation_bytes\":%zu,\"server_lease_bytes\":%zu,\"client_lease_bytes\":%zu,\"plant\":%s}\n",
        argv[1], argv[2], payload_bytes, threads, batches * batch_size, elapsed_ns / (batches * batch_size),
        (unsigned long long) received, server_bytes, client_bytes,
        sizeof(rocketnet_packet_leases_t<131072>), sizeof(rocketnet_packet_leases_t<1024>), plant ? "true" : "false");
}

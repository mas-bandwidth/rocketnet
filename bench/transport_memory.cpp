/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Allocator-observed portable server footprint; excludes OS and application memory.
#include "rocketnet.h"
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct alignas(max_align_t) allocation_t { size_t bytes; };
static std::atomic<size_t> live{0}, peak{0}, count{0};
static void * allocate(void *, size_t bytes)
{
    auto * block = (allocation_t *) malloc(sizeof(allocation_t) + bytes);
    if (!block) return NULL;
    block->bytes = bytes;
    const size_t current = live.fetch_add(bytes) + bytes;
    size_t previous = peak.load();
    while (current > previous && !peak.compare_exchange_weak(previous, current)) {}
    count.fetch_add(1);
    return block + 1;
}
static void release(void *, void * pointer)
{
    if (!pointer) return;
    auto * block = (allocation_t *) pointer - 1;
    live.fetch_sub(block->bytes);
    count.fetch_sub(1);
    free(block);
}
int main(int argc, char ** argv)
{
    if (argc != 4) return 1;
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    rocketnet_allocator(allocate, release);
    if (!rocketnet_init()) return 1;
    rocketnet_forbid_xdp_commit();
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    strcpy(config.driver, "portable");
    config.max_clients = atoi(argv[1]);
    config.max_payload_bytes = atoi(argv[2]);
    config.num_reassembly_slots = atoi(argv[3]);
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config, rocketnet_time());
    if (!server) { fprintf(stderr, "memory profile configuration refused\n"); rocketnet_term(); return 1; }
    printf("{\"clients\":%d,\"payload_bytes\":%d,\"reassembly_slots\":%d,\"live_allocator_bytes\":%zu,\"peak_allocator_bytes\":%zu,\"live_allocations\":%zu}\n",
        config.max_clients, config.max_payload_bytes, config.num_reassembly_slots, live.load(), peak.load(), count.load());
    rocketnet_server_transport_destroy(server);
    rocketnet_term();
    if (live.load() || count.load()) { fprintf(stderr, "memory profile leaked allocations\n"); return 1; }
}

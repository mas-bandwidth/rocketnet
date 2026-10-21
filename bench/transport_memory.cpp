/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Pre-allocation plan and optional allocator-observed server footprint.
#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include <errno.h>
#include <limits.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif
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
static uint64_t process_peak_rss()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters = {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0;
    return (uint64_t)counters.PeakWorkingSetSize;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return (uint64_t)usage.ru_maxrss;
#else
    return (uint64_t)usage.ru_maxrss * 1024;
#endif
#endif
}
static int number(const char * text)
{
    char * end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !*text || *end || value < 0 || value > INT_MAX)
    { fprintf(stderr, "invalid nonnegative integer\n"); exit(1); }
    return (int)value;
}
int main(int argc, char ** argv)
{
    if (argc < 4 || argc > 7)
    {
        fprintf(stderr, "usage: transport_memory clients payload_bytes reassembly_slots [transport [queues [--plan-only]]]\n");
        return 1;
    }
    const char * transport = argc >= 5 ? argv[4] : "portable";
    const bool plan_only = argc == 7 && strcmp(argv[6], "--plan-only") == 0;
    if (argc == 7 && !plan_only) return 1;
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    if (strlen(transport) >= sizeof(config.transport)) return 1;
    snprintf(config.transport, sizeof(config.transport), "%s", transport);
    config.max_clients = number(argv[1]);
    config.max_payload_bytes = number(argv[2]);
    config.max_fragmented_messages = number(argv[3]);
    config.num_xdp_queues = argc >= 6 ? number(argv[5]) : 0;
    rocketnet_server_memory_plan_t plan;
    if (!rocketnet_server_transport_memory_plan(&config, transport, plan))
    { fprintf(stderr, "memory plan unavailable for this geometry/transport/platform\n"); return 1; }
    printf("{\"transport\":\"%s\",\"clients\":%d,\"payload_bytes\":%d,\"reassembly_slots\":%d,"
        "\"planned_bytes\":%llu,\"transport_bytes\":%llu,\"protocol_bytes\":%llu,\"reassembly_bytes\":%llu,"
        "\"send_arena_bytes\":%llu,\"transport_allocator_bytes\":%llu,\"umem_bytes\":%llu,"
        "\"worker_threads\":%d,\"sockets\":%d,\"os_and_application_bytes\":null}\n",
        transport, config.max_clients, config.max_payload_bytes, config.max_fragmented_messages,
        (unsigned long long)plan.total_bytes, (unsigned long long)plan.transport_bytes,
        (unsigned long long)plan.protocol_bytes, (unsigned long long)plan.reassembly_bytes,
        (unsigned long long)plan.send_arena_bytes, (unsigned long long)plan.transport_bytes,
        (unsigned long long)plan.umem_bytes, plan.worker_threads, plan.sockets);
    if (plan_only) return 0; // No init, sockets, worker threads or UMEM registration.
    rocketnet_allocator(allocate, release);
    if (rocketnet_init( NULL ) != ROCKETNET_STATUS_OK) return 1;
    rocketnet_forbid_xdp_commit();
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config);
    if (!server) { fprintf(stderr, "memory profile configuration refused\n"); rocketnet_term(); return 1; }
    const bool same_transport = strcmp(rocketnet_server_transport_io_name(server), transport) == 0;
    printf("{\"live_allocator_bytes\":%zu,\"peak_allocator_bytes\":%zu,\"live_allocations\":%zu,\"process_peak_rss_bytes\":%llu}\n",
        live.load(), peak.load(), count.load(), (unsigned long long)process_peak_rss());
    const uint64_t planned_allocator = plan.total_bytes - plan.umem_bytes;
    // Worker shims may have been freed before the constructor returns; all
    // other planned SDK heap storage stays live. The observer must agree.
    const bool matches = live.load() <= planned_allocator && peak.load() <= planned_allocator &&
        planned_allocator - live.load() <= (uint64_t)plan.worker_threads * rocketnet_platform_thread_allocation_bytes();
    rocketnet_server_transport_destroy(server);
    rocketnet_term();
    if (!same_transport || !matches) { fprintf(stderr, "allocator observation differs from the plan; check ambient config overrides\n"); return 1; }
    if (live.load() || count.load()) { fprintf(stderr, "memory profile leaked allocations\n"); return 1; }
}

/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Pre-allocation plan and optional allocator-observed server footprint.
#include "../tests/native_shutdown.h"
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
#if !defined(_WIN32)
extern "C" char ** environ;
#endif

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

// The public endpoint and transport each resolve environment settings. A pure
// inventory cannot promise their equality while the process environment may
// change between planning and start. Development server impairment overrides
// also allocate simulator storage omitted by the concrete transport plan.
// Reject both classes rather than presenting an incomplete high-water figure.
static bool server_environment_overridden()
{
#if defined(_WIN32)
    char ** variables = _environ;
#else
    char ** variables = environ;
#endif
    for ( char ** entry = variables; entry && *entry; ++entry )
        if ( strncmp( *entry, "ROCKETNET_SERVER_", 17 ) == 0 ||
             strncmp( *entry, "ROCKETNET_DEV_SERVER_", 21 ) == 0 )
            return true;
    return false;
}
int main(int argc, char ** argv)
{
    if (argc < 4 || argc > 7)
    {
        fprintf(stderr, "usage: transport_memory clients payload_bytes reassembly_slots [transport [queues [--plan-only|--session-plan-only]]]\n");
        return 1;
    }
    const char * transport = argc >= 5 ? argv[4] : "portable";
    const bool plan_only = argc == 7 && strcmp(argv[6], "--plan-only") == 0;
    const bool session_plan_only = argc == 7 && strcmp(argv[6], "--session-plan-only") == 0;
    if (argc == 7 && !plan_only && !session_plan_only) return 1;
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    if (strlen(transport) >= sizeof(config.transport)) return 1;
    snprintf(config.transport, sizeof(config.transport), "%s", transport);
    config.max_clients = number(argv[1]);
    config.max_fragmented_messages = number(argv[3]);
    config.num_xdp_queues = argc >= 6 ? number(argv[5]) : 0;
    rocketnet_server_memory_plan_t plan;
    if (!rocketnet_server_transport_memory_plan(&config, transport, plan))
    { fprintf(stderr, "memory plan unavailable for this geometry/transport/platform\n"); return 1; }
    if (session_plan_only)
    {
        if (server_environment_overridden())
        { fprintf(stderr, "session inventory requires no ROCKETNET_SERVER_ or ROCKETNET_DEV_SERVER_ environment overrides\n"); return 1; }
        rocketnet_server_config_t endpoint;
        if (rocketnet_server_default_config(&endpoint) != ROCKETNET_STATUS_OK)
            return 1;
        endpoint.transport = transport;
        endpoint.max_clients = config.max_clients;
        endpoint.max_fragmented_messages = config.max_fragmented_messages;
        endpoint.num_xdp_queues = config.num_xdp_queues;
        const size_t endpoint_payload = rocketnet_server_required_memory(&endpoint);
        rocketnet_host_memory_estimate_t host = {};
        uint64_t endpoint_callback_bytes = endpoint_payload;
        uint64_t known_active_requests = endpoint_payload;
        if (!endpoint_payload || !rocketnet_server_transport_host_memory_estimate(&config, transport, &host) ||
            !rocketnet_memory_add(endpoint_callback_bytes, 1, rocketnet_allocation_overhead()) ||
            !rocketnet_memory_add(known_active_requests, 1, rocketnet_allocation_overhead()) ||
            !rocketnet_memory_add(known_active_requests, 1, host.total_bytes))
        { fprintf(stderr, "session inventory unavailable for this geometry/transport/platform\n"); return 1; }
        // Public start refuses a running/stopping generation. Once stopped,
        // it destroys the retained generation before constructing the next.
        // The active/stopping high-water therefore owns one candidate's SDK
        // storage, but native retained-on-cancellation bytes remain unknown.
        printf("{\"scope\":\"one_public_server_one_pinned_transport\",\"transport\":\"%s\","
            "\"create_payload_bytes\":%llu,\"create_callback_request_bytes\":%llu,"
            "\"active_transport_sdk_callback_bytes\":%llu,\"active_xdp_umem_bytes\":%llu,"
            "\"active_rio_registered_region_bytes\":%llu,\"active_socket_send_request_bytes\":%llu,"
            "\"active_socket_receive_request_bytes\":%llu,\"active_xdp_ring_request_bytes\":%llu,"
            "\"active_thread_stack_reserve_bytes\":%llu,\"known_active_or_stopping_requests_bytes\":%llu,"
            "\"concurrent_sdk_generations\":1,\"native_retained_on_failed_cancel_bytes\":null,"
            "\"io_uring_mapping_bytes\":null,\"libc_and_kernel_accounting_bytes\":null,"
            "\"config_copy_frozen_for_future_start\":false,\"one_block_sufficient\":false}\n",
            transport, (unsigned long long) endpoint_payload, (unsigned long long) endpoint_callback_bytes,
            (unsigned long long) host.allocator_bytes, (unsigned long long) host.umem_bytes,
            (unsigned long long) host.registered_region_bytes,
            (unsigned long long) host.socket_send_buffer_bytes, (unsigned long long) host.socket_receive_buffer_bytes,
            (unsigned long long) host.xdp_ring_bytes, (unsigned long long) host.thread_stack_bytes,
            (unsigned long long) known_active_requests);
        return 0;
    }
    printf("{\"transport\":\"%s\",\"clients\":%d,\"payload_bytes\":%d,\"reassembly_slots\":%d,"
        "\"planned_bytes\":%llu,\"transport_bytes\":%llu,\"protocol_bytes\":%llu,\"reassembly_bytes\":%llu,"
        "\"send_arena_bytes\":%llu,\"transport_allocator_bytes\":%llu,\"umem_bytes\":%llu,"
        "\"worker_threads\":%d,\"sockets\":%d,\"os_and_application_bytes\":null}\n",
        (unsigned long long)plan.total_bytes, (unsigned long long)plan.transport_bytes,
        (unsigned long long)plan.protocol_bytes, (unsigned long long)plan.reassembly_bytes,
        (unsigned long long)plan.send_arena_bytes, (unsigned long long)plan.transport_bytes,
        (unsigned long long)plan.umem_bytes, plan.worker_threads, plan.sockets);
    if (plan_only) return 0; // No init, sockets, worker threads or UMEM registration.
    if (rocketnet_init( NULL, allocate, release ) != ROCKETNET_STATUS_OK) return 1;
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
    rn_native_server_transport_stop_and_destroy(server);
    rocketnet_term();
    if (!same_transport || !matches) { fprintf(stderr, "allocator observation differs from the plan; check ambient config overrides\n"); return 1; }
    if (live.load() || count.load()) { fprintf(stderr, "memory profile leaked allocations\n"); return 1; }
}

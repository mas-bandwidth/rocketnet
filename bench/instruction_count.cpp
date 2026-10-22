/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Deterministic instruction-count scenario on the in-process loopback transport:
    one process, no sockets, no threads (step_driven = true), no wall clock
    (caller-passed time starting at 1000.0 with fixed dt = 0.01), and all
    random bytes drawn from a fixed splitmix64 seed installed before
    rocketnet_init.

    Regions:
      - client_frame: 1 connected loopback client writing P = 32 payloads of
        200 bytes per frame; counts rocketnet_client_transport_send_packets and
        rocketnet_client_transport_update over 64 measured frames after 32
        warm-up frames (matching the window timed by client_main_thread_cost).
      - server_tick: 16 connected loopback clients (inside
        ROCKETNET_LOOPBACK_MAX_ENDPOINTS), each sending 4 payloads of 200 bytes
        per tick; counts rocketnet_server_transport_update, payload reply
        framing, and rocketnet_server_transport_send_packets over 64 measured
        ticks after 16 warm-up ticks.

    The server's ack stats worker runs inline (rocketnet_dev_stats_manual):
    the bench steps it outside the counted window after every server update,
    so each measured tick publishes the previous batch and submits the next,
    as a worker that keeps up does, and no thread runs inside the window.

    Usage:
      ./bin/instruction_count <client_frame|server_tick> [setup]
*/

#define ROCKETNET_DEV 1
#include "../tests/native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "sodium.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>
#define ROCKETNET_HAVE_CACHEGRIND_CLIENT_REQUESTS 1
#endif
#endif

#ifndef ROCKETNET_HAVE_CACHEGRIND_CLIENT_REQUESTS
#define ROCKETNET_HAVE_CACHEGRIND_CLIENT_REQUESTS 0
#define CACHEGRIND_START_INSTRUMENTATION do {} while (0)
#define CACHEGRIND_STOP_INSTRUMENTATION do {} while (0)
#endif

#ifndef ROCKETNET_COMPILER_ID
#if defined(__clang__)
#define ROCKETNET_STR_HELPER(x) #x
#define ROCKETNET_STR(x) ROCKETNET_STR_HELPER(x)
#define ROCKETNET_COMPILER_ID "Clang-" ROCKETNET_STR(__clang_major__) "." ROCKETNET_STR(__clang_minor__) "." ROCKETNET_STR(__clang_patchlevel__)
#elif defined(__GNUC__)
#define ROCKETNET_STR_HELPER(x) #x
#define ROCKETNET_STR(x) ROCKETNET_STR_HELPER(x)
#define ROCKETNET_COMPILER_ID "GNU-" ROCKETNET_STR(__GNUC__) "." ROCKETNET_STR(__GNUC_MINOR__) "." ROCKETNET_STR(__GNUC_PATCHLEVEL__)
#else
#define ROCKETNET_COMPILER_ID "Unknown"
#endif
#endif

#ifndef ROCKETNET_BUILD_TYPE
#if defined(NDEBUG)
#define ROCKETNET_BUILD_TYPE "Release"
#else
#define ROCKETNET_BUILD_TYPE "Debug"
#endif
#endif

#define CHECK(cond)                                                                     \
    do                                                                                  \
    {                                                                                   \
        if (!(cond))                                                                    \
        {                                                                               \
            fprintf(stderr, "instruction_count check failed at %s:%d: %s\n",            \
                    __FILE__, __LINE__, #cond);                                         \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

namespace
{

constexpr uint64_t FIXED_SEED = 0x524F434B45544E54ULL;
constexpr double INITIAL_TIME = 1000.0;
constexpr double FIXED_DT = 0.01;
constexpr int PAYLOAD_BYTES = 200;

constexpr int CLIENT_FRAME_PACKETS = 32;
constexpr int CLIENT_FRAME_WARMUP = 32;
constexpr int CLIENT_FRAME_UNITS = 64;

constexpr int SERVER_TICK_CLIENTS = 16;
constexpr int SERVER_TICK_PACKETS_PER_CLIENT = 4;
constexpr int SERVER_TICK_WARMUP = 16;
constexpr int SERVER_TICK_UNITS = 64;

uint64_t g_rng_state = FIXED_SEED;
double g_sim_time = INITIAL_TIME;

uint64_t next_u64()
{
    uint64_t z = (g_rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

const char * fixed_rng_implementation_name(void)
{
    return "instruction_count_fixed";
}

uint32_t fixed_rng_random(void)
{
    return (uint32_t)next_u64();
}

void fixed_rng_stir(void) {}

uint32_t fixed_rng_uniform(const uint32_t upper_bound)
{
    return upper_bound ? (fixed_rng_random() % upper_bound) : 0;
}

void fixed_rng_buf(void * const buf, const size_t size)
{
    uint8_t * out = (uint8_t *)buf;
    size_t i = 0;
    while (i + 8 <= size)
    {
        uint64_t v = next_u64();
        memcpy(out + i, &v, 8);
        i += 8;
    }
    if (i < size)
    {
        uint64_t v = next_u64();
        memcpy(out + i, &v, size - i);
    }
}

int fixed_rng_close(void)
{
    return 0;
}

randombytes_implementation g_fixed_rng = {
    fixed_rng_implementation_name,
    fixed_rng_random,
    fixed_rng_stir,
    fixed_rng_uniform,
    fixed_rng_buf,
    fixed_rng_close,
};

double sim_time_callback(void *)
{
    return g_sim_time;
}

void init_deterministic_runtime()
{
    g_rng_state = FIXED_SEED;
    g_sim_time = INITIAL_TIME;
    CHECK(randombytes_set_implementation(&g_fixed_rng) == 0);
    CHECK(rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK);
    rocketnet_quiet(1);
}

void run_client_frame(bool setup_only)
{
    init_deterministic_runtime();

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config(&server_config);
    server_config.max_clients = 2;
    server_config.game_clock_epoch = 1000;
    memcpy(server_config.transport, "loopback", sizeof("loopback"));
    rocketnet_server_transport_t * server =
        rocketnet_server_transport_create(NULL, "127.0.0.1:42000", &server_config);
    CHECK(server != NULL);
    CHECK(strcmp(rocketnet_server_transport_io_name(server), "loopback") == 0);

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK(rocketnet_write_test_connect_token("127.0.0.1:42000", 1001ULL, 2000ULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK);

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config(&client_config);
    memcpy(client_config.transport, "loopback", sizeof("loopback"));
    client_config.step_driven = true;
    client_config.game_clock_epoch = 1000;
    client_config.time_callback = sim_time_callback;

    rocketnet_client_transport_t * client =
        rocketnet_client_transport_create(NULL, token, &client_config, g_sim_time);
    CHECK(client != NULL);

    int client_index = -1;
    for (int step = 0; step < 200; ++step)
    {
        g_sim_time += FIXED_DT;
        rocketnet_client_transport_update(client, g_sim_time);
        rocketnet_server_transport_update(server, g_sim_time);
        rocketnet_server_transport_send_packets(server);
        if (rocketnet_client_transport_state(client) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED)
        {
            client_index = rocketnet_client_transport_client_index(client);
            if (rocketnet_server_transport_is_client_connected(server, client_index))
                break;
        }
    }
    CHECK(client_index >= 0 && rocketnet_server_transport_is_client_connected(server, client_index));

    const int measured_frames = setup_only ? 0 : CLIENT_FRAME_UNITS;
    uint8_t scratch[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    uint64_t received = 0;

    for (int frame = -CLIENT_FRAME_WARMUP; frame < measured_frames; ++frame)
    {
        g_sim_time += FIXED_DT;
        for (int i = 0; i < CLIENT_FRAME_PACKETS; ++i)
        {
            rocketnet_packet_context_t context;
            uint8_t * payload =
                rocketnet_client_transport_start_packet(client, PAYLOAD_BYTES, &context);
            CHECK(payload != NULL);
            memset(payload + context.payload_index, (uint8_t)i, PAYLOAD_BYTES);
            CHECK(rocketnet_client_transport_finish_packet(client, &context, PAYLOAD_BYTES) >
                  PAYLOAD_BYTES);
        }

        if (frame >= 0)
            CACHEGRIND_START_INSTRUMENTATION;

        rocketnet_client_transport_send_packets(client);
        rocketnet_client_transport_update(client, g_sim_time);

        if (frame >= 0)
            CACHEGRIND_STOP_INSTRUMENTATION;

        rocketnet_server_transport_update(server, g_sim_time);
        const int * reply_client_index = NULL;
        uint8_t * const * received_data = NULL;
        const int * received_bytes = NULL;
        const int num_payloads = rocketnet_server_transport_payloads(
            server, &reply_client_index, &received_data, &received_bytes);
        CHECK(num_payloads == CLIENT_FRAME_PACKETS);
        for (int i = 0; i < num_payloads; ++i)
        {
            CHECK(received_bytes[i] == PAYLOAD_BYTES);
            rocketnet_packet_context_t context;
            uint8_t * payload = rocketnet_server_transport_start_packet(
                server, reply_client_index[i], PAYLOAD_BYTES, &context);
            CHECK(payload != NULL);
            memset(payload + context.payload_index, 0x72, PAYLOAD_BYTES);
            CHECK(rocketnet_server_transport_finish_packet(server, &context, PAYLOAD_BYTES) >
                  PAYLOAD_BYTES);
        }
        rocketnet_server_transport_send_packets(server);

        rocketnet_client_transport_update(client, g_sim_time);
        int bytes = 0;
        while ((bytes = rocketnet_client_transport_receive_packet(
                    client, scratch, (int)sizeof(scratch))) > 0)
        {
            CHECK(bytes == PAYLOAD_BYTES);
            ++received;
        }
    }

    CHECK(received == (uint64_t)(CLIENT_FRAME_WARMUP + measured_frames) * CLIENT_FRAME_PACKETS);

    rn_native_client_transport_stop_and_destroy(client);
    rn_native_server_transport_stop_and_destroy(server);
    rocketnet_term();

    printf("instruction_count region=client_frame units=%d mode=%s compiler_id=%s build_type=%s\n",
           CLIENT_FRAME_UNITS,
           ROCKETNET_HAVE_CACHEGRIND_CLIENT_REQUESTS ? "client_requests" : "subtract_setup",
           ROCKETNET_COMPILER_ID,
           ROCKETNET_BUILD_TYPE);
}

#if !defined(ROCKETNET_STATS_MANUAL_TEST_HOOKS)
#error "instruction_count links rocketnet_dev_stats_manual: the stats worker must step inline"
#endif

// Completes every submitted stats batch on the calling thread, outside the
// counted window, and fails if an update found the worker behind.
void drain_stats_worker(rocketnet_server_transport_t * server)
{
    while (rocketnet_server_transport_stats_worker_step_for_test(server))
    {
    }
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t busy_skips = 0;
    CHECK(rocketnet_server_transport_stats_worker_status_for_test(server, &submitted, &completed, NULL, &busy_skips));
    CHECK(completed == submitted);
    CHECK(busy_skips == 0);
}

void run_server_tick(bool setup_only)
{
    static_assert(SERVER_TICK_CLIENTS + 1 <= ROCKETNET_LOOPBACK_MAX_ENDPOINTS,
                  "server_tick client count must fit inside ROCKETNET_LOOPBACK_MAX_ENDPOINTS");

    init_deterministic_runtime();

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config(&server_config);
    server_config.max_clients = SERVER_TICK_CLIENTS;
    server_config.game_clock_epoch = 1000;
    memcpy(server_config.transport, "loopback", sizeof("loopback"));
    rocketnet_server_transport_t * server =
        rocketnet_server_transport_create(NULL, "127.0.0.1:42001", &server_config);
    CHECK(server != NULL);
    CHECK(strcmp(rocketnet_server_transport_io_name(server), "loopback") == 0);

    rocketnet_client_transport_t * clients[SERVER_TICK_CLIENTS] = {};
    for (int c = 0; c < SERVER_TICK_CLIENTS; ++c)
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        CHECK(rocketnet_write_test_connect_token(
            "127.0.0.1:42001", (uint64_t)(2000 + c), 2000ULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK);

        rocketnet_client_transport_config_t client_config;
        rocketnet_client_transport_default_config(&client_config);
        memcpy(client_config.transport, "loopback", sizeof("loopback"));
        client_config.step_driven = true;
        client_config.game_clock_epoch = 1000;
        client_config.time_callback = sim_time_callback;

        clients[c] = rocketnet_client_transport_create(NULL, token, &client_config, g_sim_time);
        CHECK(clients[c] != NULL);
    }

    for (int step = 0; step < 200; ++step)
    {
        g_sim_time += FIXED_DT;
        for (int c = 0; c < SERVER_TICK_CLIENTS; ++c)
            rocketnet_client_transport_update(clients[c], g_sim_time);
        rocketnet_server_transport_update(server, g_sim_time);
        rocketnet_server_transport_send_packets(server);
        drain_stats_worker(server);
        if (rocketnet_server_transport_num_connected_clients(server) == SERVER_TICK_CLIENTS)
        {
            bool all_connected = true;
            for (int c = 0; c < SERVER_TICK_CLIENTS; ++c)
            {
                if (rocketnet_client_transport_state(clients[c]) !=
                    ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED)
                {
                    all_connected = false;
                    break;
                }
            }
            if (all_connected)
                break;
        }
    }
    CHECK(rocketnet_server_transport_num_connected_clients(server) == SERVER_TICK_CLIENTS);

    const int measured_ticks = setup_only ? 0 : SERVER_TICK_UNITS;
    uint8_t scratch[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    uint64_t total_server_received = 0;

    for (int tick = -SERVER_TICK_WARMUP; tick < measured_ticks; ++tick)
    {
        g_sim_time += FIXED_DT;
        for (int c = 0; c < SERVER_TICK_CLIENTS; ++c)
        {
            for (int p = 0; p < SERVER_TICK_PACKETS_PER_CLIENT; ++p)
            {
                rocketnet_packet_context_t context;
                uint8_t * payload = rocketnet_client_transport_start_packet(
                    clients[c], PAYLOAD_BYTES, &context);
                CHECK(payload != NULL);
                memset(payload + context.payload_index, (uint8_t)(c + p), PAYLOAD_BYTES);
                CHECK(rocketnet_client_transport_finish_packet(
                          clients[c], &context, PAYLOAD_BYTES) > PAYLOAD_BYTES);
            }
            rocketnet_client_transport_send_packets(clients[c]);
            rocketnet_client_transport_update(clients[c], g_sim_time);
        }

        if (tick >= 0)
            CACHEGRIND_START_INSTRUMENTATION;

        rocketnet_server_transport_update(server, g_sim_time);
        const int * reply_client_index = NULL;
        uint8_t * const * received_data = NULL;
        const int * received_bytes = NULL;
        const int num_payloads = rocketnet_server_transport_payloads(
            server, &reply_client_index, &received_data, &received_bytes);
        for (int i = 0; i < num_payloads; ++i)
        {
            rocketnet_packet_context_t context;
            uint8_t * payload = rocketnet_server_transport_start_packet(
                server, reply_client_index[i], PAYLOAD_BYTES, &context);
            CHECK(payload != NULL);
            memset(payload + context.payload_index, 0x73, PAYLOAD_BYTES);
            CHECK(rocketnet_server_transport_finish_packet(server, &context, PAYLOAD_BYTES) >
                  PAYLOAD_BYTES);
        }
        rocketnet_server_transport_send_packets(server);

        if (tick >= 0)
            CACHEGRIND_STOP_INSTRUMENTATION;

        drain_stats_worker(server);

        CHECK(num_payloads == SERVER_TICK_CLIENTS * SERVER_TICK_PACKETS_PER_CLIENT);
        total_server_received += (uint64_t)num_payloads;

        for (int c = 0; c < SERVER_TICK_CLIENTS; ++c)
        {
            rocketnet_client_transport_update(clients[c], g_sim_time);
            while (rocketnet_client_transport_receive_packet(
                       clients[c], scratch, (int)sizeof(scratch)) > 0)
            {
            }
        }
    }

    CHECK(total_server_received ==
          (uint64_t)(SERVER_TICK_WARMUP + measured_ticks) *
              (SERVER_TICK_CLIENTS * SERVER_TICK_PACKETS_PER_CLIENT));

    // the measured ticks published stats: the window holds the worker's
    // hand-off, not an idle path
    uint64_t published = 0;
    CHECK(rocketnet_server_transport_stats_worker_status_for_test(server, NULL, NULL, &published, NULL));
    CHECK(published > 0);

    for (int c = 0; c < SERVER_TICK_CLIENTS; ++c)
        rn_native_client_transport_stop_and_destroy(clients[c]);
    rn_native_server_transport_stop_and_destroy(server);
    rocketnet_term();

    printf("instruction_count region=server_tick units=%d mode=%s compiler_id=%s build_type=%s\n",
           SERVER_TICK_UNITS,
           ROCKETNET_HAVE_CACHEGRIND_CLIENT_REQUESTS ? "client_requests" : "subtract_setup",
           ROCKETNET_COMPILER_ID,
           ROCKETNET_BUILD_TYPE);
}

} // namespace

int main(int argc, char ** argv)
{
    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: %s <client_frame|server_tick> [setup]\n", argv[0]);
        return 2;
    }
    const bool setup_only = (argc == 3 && strcmp(argv[2], "setup") == 0);
    if (argc == 3 && !setup_only)
    {
        fprintf(stderr, "unknown mode: %s\n", argv[2]);
        return 2;
    }

    if (strcmp(argv[1], "client_frame") == 0)
    {
        run_client_frame(setup_only);
        return 0;
    }
    if (strcmp(argv[1], "server_tick") == 0)
    {
        run_server_tick(setup_only);
        return 0;
    }

    fprintf(stderr, "unknown region: %s\n", argv[1]);
    return 2;
}

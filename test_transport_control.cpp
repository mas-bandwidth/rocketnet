/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Compile the transport here to seed protocol state and substitute a
// deterministic driver. The seal override is local to this translation unit.

#include "sdk/rocketnet_server_protocol.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "control check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)
#define ROCKETNET_FAKE_DRIVER_CHECK(x) CHECK(x)

static bool fail_seal;
static int checked_seals;
static bool check_arena_copy;
static int arena_pattern;
static int controlled_seal(rocketnet_server_protocol_t * server, int client_index, uint64_t sequence,
    uint64_t packet_id, uint8_t * wire, int bytes, double time);
#define rocketnet_server_protocol_seal_payload_frame controlled_seal
#include "sdk/rocketnet_server_transport.cpp"
#undef rocketnet_server_protocol_seal_payload_frame
#include "test_fake_drivers.h"

static int controlled_seal(rocketnet_server_protocol_t * server, int client_index, uint64_t sequence,
    uint64_t packet_id, uint8_t * wire, int bytes, double time)
{
    if (check_arena_copy)
    {
        CHECK(bytes == 64);
        for (int i = 0; i < bytes; ++i)
            CHECK(wire[ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET + i] == uint8_t(i + arena_pattern));
        ++checked_seals;
    }
    if (fail_seal) return 0;
    return rocketnet_server_protocol_seal_payload_frame(server, client_index, sequence, packet_id, wire, bytes, time);
}

static void test_buffered_control_pressure(int clients = 300)
{
    auto * f = new control_fixture_t();
    rocketnet_server_driver_operations_t operations;
    auto * server = make_server(f, &operations, clients, false);
    f->requests = ROCKETNET_SERVER_MAX_RECEIVE_PACKETS;
    const double begin = rocketnet_time();
    for (int step = 0; step < 6; ++step)
    {
        double now = 1.0 + step;
        // Half the clients owe standalone ACKs while their keep-alive clock
        // is not due; the other half need keep-alives. All indices are due
        // for one kind of control packet on every update.
        for (int i = 1; i < clients; i += 2)
        {
            auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map,
                rocketnet_client_slots_mapping_index(server->protocol.client_slots, i));
            mapping->last_keep_alive_send_time = now;
            server->protocol.acks->endpoints[i].ack_debt = ROCKETNET_ACK_DEBT_THRESHOLD;
        }
        rocketnet_server_transport_update(server, now);
        CHECK(f->responses == (step + 1) * f->requests);
        for (int i = 0; i < clients; ++i)
            CHECK((i % 2 ? f->acks[i] : f->keep_alives[i]) == step + 1);
    }
    const double elapsed_ms = (rocketnet_time() - begin) * 1000.0;
    for (int i = 0; i < clients; ++i) CHECK(server->protocol.acks->endpoints[i].rtt == 17.0f);
    rocketnet_server_transport_destroy(server); delete f;
    printf("test_buffered_control_pressure clients=%d updates=6 controls=%d elapsed_ms=%.3f\n",
        clients, clients * 6, elapsed_ms);
}

static void test_staged_control_fairness()
{
    auto * f = new control_fixture_t(); f->capacity = 2;
    rocketnet_server_driver_operations_t operations;
    auto * server = make_server(f, &operations, 12, true);
    // Every slot remains due, but only two frames are available per update.
    for (int step = 0; step < 6; ++step) rocketnet_server_transport_update(server, 1.0 + step);
    for (int i = 0; i < 12; ++i)
    {
        CHECK(f->keep_alives[i] == 1);
        CHECK(server->protocol.acks->endpoints[i].rtt == 17.0f);
    }
    rocketnet_server_transport_destroy(server); delete f;
    printf("test_staged_control_fairness\n");
}

static void test_single_frame_arena()
{
    for (int staged = 0; staged < 2; ++staged)
    {
        auto * f = new control_fixture_t(); f->capacity = 1;
        rocketnet_server_driver_operations_t operations;
        auto * server = make_server(f, &operations, 1, staged != 0, 5000);
        checked_seals = 0; check_arena_copy = true;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            rocketnet_packet_context_t context;
            CHECK(rocketnet_server_transport_start_packet(server, 0, 5000, &context));
            CHECK(context.payload_index == 0); // The oversized reservation selected the arena.
            int capacity;
            uint8_t * payload = rocketnet_server_transport_packet_payload(server, &context, &capacity);
            CHECK(payload && capacity >= 5000);
            arena_pattern = 37 + attempt * 17;
            for (int i = 0; i < 64; ++i) payload[i] = uint8_t(i + arena_pattern);
            fail_seal = attempt == 0;
            int bytes = rocketnet_server_transport_finish_packet(server, &context, 64);
            CHECK((bytes > 0) == (attempt == 1) && !context.frame);
            if (staged)
            {
                CHECK(f->aborts == 1);
                CHECK(f->used == attempt);
            }
            else if (attempt == 0) CHECK(server->payload_ring.info[0].wire_bytes == 0);
            rocketnet_server_transport_send_packets(server);
            CHECK(f->payloads == attempt); // Failed seal never reached the driver.
        }
        CHECK(checked_seals == 2);
        check_arena_copy = false; fail_seal = false;
        rocketnet_server_transport_destroy(server); delete f;
    }
    printf("test_single_frame_arena_seal_and_abort\n");
}

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init());
    rocketnet_forbid_xdp_commit();
    test_buffered_control_pressure();
    test_buffered_control_pressure(1000);
    test_staged_control_fairness();
    test_single_frame_arena();
    rocketnet_term();
}

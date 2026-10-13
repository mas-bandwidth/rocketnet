/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Compile the actual transport here to seed protocol state and substitute a
// deterministic driver. No test hook or additional export enters the SDK.
#include "sdk/rocketnet_server_protocol.h"
static bool fail_seal;
static int checked_seals;
static bool check_arena_copy;
static int arena_pattern;
static int controlled_seal(rocketnet_server_protocol_t * server, int client_index, uint64_t sequence,
    uint64_t packet_id, uint8_t * wire, int bytes, double time);
#define rocketnet_server_protocol_seal_payload_frame controlled_seal
#include "sdk/rocketnet_server_transport.cpp"
#undef rocketnet_server_protocol_seal_payload_frame

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "control check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

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

struct control_fixture_t
{
    rocketnet_address_t request_address;
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    int requests;
    int responses;
    int payloads;
    int aborts;
    int disconnects;
    int pending_disconnects;
    bool staged;
    int keep_alives[ROCKETNET_MAX_CLIENTS];
    int acks[ROCKETNET_MAX_CLIENTS];
    int capacity;
    int used;
    rocketnet_address_t reserved_to;
    uint8_t frame[ROCKETNET_FRAME_STRIDE];
    rocketnet_server_socket_process_packets_t received;
};

static void record(control_fixture_t * f, const rocketnet_address_t * to, const uint8_t * packet)
{
    if (to->port == f->request_address.port) { ++f->responses; return; }
    const int client = to->port - 1000;
    CHECK(client >= 0 && client < ROCKETNET_MAX_CLIENTS);
    if (packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET) ++f->keep_alives[client];
    else if (packet[0] == ROCKETNET_CONNECTION_ACK_PACKET) ++f->acks[client];
    else if (packet[0] == ROCKETNET_CONNECTION_PAYLOAD_PACKET) ++f->payloads;
    else if (packet[0] == ROCKETNET_CONNECTION_DISCONNECT_PACKET)
    {
        if (f->staged) ++f->pending_disconnects;
        else ++f->disconnects;
    }
    else CHECK(false);
}
static void fixture_destroy(void *) {}
static bool fixture_stop(void *) { return true; }
static void send_batch(void * p, const rocketnet_platform_send_t * sends, int count)
{
    CHECK(count > 0 && count <= ROCKETNET_SERVER_MAX_SEND_PACKETS);
    for (int i = 0; i < count; ++i) record((control_fixture_t*)p, sends[i].to, sends[i].packet_data);
}
static int receive_batch(void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity)
{
    auto * f = (control_fixture_t*)p;
    CHECK(f->requests <= capacity);
    for (int i = 0; i < f->requests; ++i)
    {
        from[i] = f->request_address;
        memcpy(data + stride * i, f->request, sizeof(f->request));
        bytes[i] = sizeof(f->request);
    }
    return f->requests;
}
static uint8_t * start_packet(void * p, const rocketnet_address_t * to, uint64_t * id)
{
    auto * f = (control_fixture_t*)p;
    if (f->used == f->capacity) return NULL;
    f->reserved_to = *to; *id = f->used;
    return f->frame;
}
static void finish_packet(void * p, uint64_t, uint8_t * frame, int)
{
    auto * f = (control_fixture_t*)p;
    record(f, &f->reserved_to, frame + ROCKETNET_FRAME_WIRE_OFFSET); ++f->used;
}
static void abort_packet(void * p, uint64_t, uint8_t *) { ++((control_fixture_t*)p)->aborts; }
static void flush(void * p)
{
    auto * f = (control_fixture_t*)p;
    f->disconnects += f->pending_disconnects;
    f->pending_disconnects = 0;
    f->used = 0;
}
static rocketnet_server_socket_process_packets_t * receive(void * p) { return &((control_fixture_t*)p)->received; }

static rocketnet_server_transport_t * make_server(control_fixture_t * f, rocketnet_server_driver_operations_t * operations, int clients, bool staged, int payload_limit = ROCKETNET_MAX_PAYLOAD_BYTES)
{
    f->staged = staged;
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    config.max_clients = clients;
    config.max_payload_bytes = payload_limit;
    config.num_reassembly_slots = 1;
    strcpy(config.driver, "portable");
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config, 0.0);
    CHECK(server);
    rocketnet_server_driver_close(&server->io);
    *operations = {};
    operations->destroy = fixture_destroy; operations->stop = fixture_stop;
    operations->send_batch = send_batch; operations->receive_batch = receive_batch;
    if (staged)
    {
        operations->flags = ROCKETNET_DRIVER_STAGED | ROCKETNET_DRIVER_FLUSH_ON_UPDATE;
        operations->start_packet = start_packet; operations->finish_packet = finish_packet;
        operations->abort_packet = abort_packet; operations->flush = flush; operations->receive = receive;
    }
    server->io.instance = f; server->io.operations = operations;
    for (int i = 0; i < clients; ++i)
    {
        rocketnet_address_t address;
        CHECK(rocketnet_address_parse(&address, "127.0.0.1:0")); address.port = 1000 + i;
        int index = rocketnet_encryption_map_add(server->protocol.encryption_map, &address, 100.0);
        CHECK(index >= 0);
        auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, index);
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
        CHECK(rocketnet_client_slots_assign(server->protocol.client_slots, i + 1, index, user_data) == i);
        mapping->client_index = i; mapping->client_id = i + 1; mapping->established = 1;
        mapping->timeout_seconds = 1000;
        // Synthetic connections have test keys; this fixture does not
        // model authentication. It exercises the real update/send scheduler.
        memset(&mapping->keys, i + 1, sizeof(mapping->keys));
        server->protocol.acks->endpoints[i].rtt_history[0] = 17.0f;
    }
    CHECK(rocketnet_address_parse(&f->request_address, "127.0.0.1:20000"));
    int pending = rocketnet_encryption_map_add(server->protocol.encryption_map, &f->request_address, 100.0);
    CHECK(pending >= 0);
    auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, pending);
    mapping->challenge_packet[0] = ROCKETNET_CONNECTION_CHALLENGE_PACKET;
    uint8_t * cursor = f->request;
    rocketnet_write_prefilter_header(&cursor, ROCKETNET_CONNECTION_REQUEST_PACKET, server->protocol.filter_value);
    return server;
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

static void test_disconnect_burst_next_send(bool staged)
{
    auto * f = new control_fixture_t(); f->capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 1;
    rocketnet_server_driver_operations_t operations;
    auto * server = make_server(f, &operations, 1, staged);
    rocketnet_server_transport_update(server, 1.0);
    const int stats_index = server->current_stats_index;
    const uint64_t before = server->stats_frames[stats_index].packets_sent;

    rocketnet_server_transport_disconnect_client(server, 0);
    CHECK(rocketnet_server_transport_num_connected_clients(server) == 0);
    CHECK(f->disconnects == 0);
    CHECK(server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);

    rocketnet_server_transport_send_packets(server);
    CHECK(f->disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
    CHECK(server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);

    rocketnet_server_transport_destroy(server); delete f;
    printf("test_disconnect_burst_next_send staged=%d disconnects=%d\n", staged, ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
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

int main(int argc, char ** argv)
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init());
    rocketnet_forbid_xdp_commit();
    if (argc == 2)
    {
        if (strcmp(argv[1], "disconnect-burst-portable") == 0) test_disconnect_burst_next_send(false);
        else if (strcmp(argv[1], "disconnect-burst-staged") == 0) test_disconnect_burst_next_send(true);
        else CHECK(false);
        rocketnet_term();
        return 0;
    }
    CHECK(argc == 1);
    test_buffered_control_pressure();
    test_buffered_control_pressure(1000);
    test_disconnect_burst_next_send(false);
    test_disconnect_burst_next_send(true);
    test_staged_control_fairness();
    test_single_frame_arena();
    rocketnet_term();
}

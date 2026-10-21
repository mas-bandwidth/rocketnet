/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Compile the actual transport here to seed protocol state and substitute a
// deterministic transport. The local stats hook keeps worker progress explicit.
#include "test/native_shutdown.h"
#include "sdk/rocketnet_server_protocol.h"
#include "sdk/rocketnet_replay.h"
#include "sdk/rocketnet_fragment.h"
// This target compiles the transport into its own deterministic fixture.
#define ROCKETNET_XDP_STAGING_TEST_HOOKS 1
static bool fail_seal;
static int checked_seals;
static bool check_arena_copy;
static int arena_pattern;
static int controlled_seal(rocketnet_server_protocol_t * server, int client_index, uint64_t sequence,
    uint64_t packet_id, uint8_t * wire, int bytes, double time);
#define rocketnet_server_protocol_seal_payload_frame controlled_seal
#include "sdk/rocketnet_server_transport.cpp"
#undef rocketnet_server_protocol_seal_payload_frame
#undef ROCKETNET_XDP_STAGING_TEST_HOOKS

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

// The fake staged frame must satisfy the public packet lease's 16-byte
// alignment contract. Its intentional struct padding triggers MSVC C4324.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4324)
#endif
struct control_fixture_t
{
    rocketnet_address_t request_address;
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    int requests;
    int responses;
    int payloads;
    int aborts;
    int start_calls;
    int disconnects;
    int pending_disconnects;
    bool staged;
    int keep_alives[ROCKETNET_MAX_CLIENTS];
    int acks[ROCKETNET_MAX_CLIENTS];
    int capacity;
    int used;
    uint8_t * lease_frames;
    int lease_starts;
    rocketnet_address_t reserved_to;
    alignas(16) uint8_t frame[ROCKETNET_FRAME_STRIDE];
    rocketnet_server_socket_process_packets_t received;
    int feed_count;
    rocketnet_address_t feed_from[4];
    uint8_t feed_data[4][ROCKETNET_RECEIVE_CEILING];
    int feed_bytes[4];
    bool unreachable;
    int accepted[4];
    int num_accepted;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
static void fixture_stop( void * ) {}
static bool fixture_stopped( void * ) { return true; }
static void send_batch(void * p, const rocketnet_platform_send_t * sends, int count)
{
    CHECK(count > 0 && count <= ROCKETNET_SERVER_MAX_SEND_PACKETS);
    for (int i = 0; i < count; ++i) record((control_fixture_t*)p, sends[i].to, sends[i].packet_data);
}
static int receive_batch(void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity)
{
    auto * f = (control_fixture_t*)p;
    if (f->feed_count)
    {
        // fed client packets, delivered once
        const int n = f->feed_count;
        CHECK(n <= capacity);
        for (int i = 0; i < n; ++i)
        {
            from[i] = f->feed_from[i];
            memcpy(data + stride * i, f->feed_data[i], (size_t) f->feed_bytes[i]);
            bytes[i] = f->feed_bytes[i];
        }
        f->feed_count = 0;
        return n;
    }
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
    ++f->start_calls;
    if (f->unreachable) { *id = ROCKETNET_TRANSPORT_PACKET_UNREACHABLE; return NULL; }
    if (f->lease_frames)
    {
        if (f->lease_starts == f->capacity) return NULL;
        f->reserved_to = *to;
        *id = (uint64_t) f->lease_starts;
        return f->lease_frames + (size_t) f->lease_starts++ * ROCKETNET_FRAME_STRIDE;
    }
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
static void peer_accepted(void * p, int view_index)
{
    auto * f = (control_fixture_t*)p;
    CHECK(f->num_accepted < 4);
    f->accepted[f->num_accepted++] = view_index;
}
static void flush(void * p)
{
    auto * f = (control_fixture_t*)p;
    f->disconnects += f->pending_disconnects;
    f->pending_disconnects = 0;
    f->used = 0;
}
static rocketnet_server_socket_process_packets_t * receive(void * p)
{
    auto * f = (control_fixture_t*)p;
    if (f->feed_count)
    {
        // fed client packets, delivered once
        f->received.num_packets = f->feed_count;
        for (int i = 0; i < f->feed_count; ++i)
        {
            f->received.from[i] = f->feed_from[i];
            f->received.packet_data[i] = f->feed_data[i];
            f->received.packet_bytes[i] = f->feed_bytes[i];
        }
        f->feed_count = 0;
    }
    else f->received.num_packets = 0;
    return &f->received;
}

static rocketnet_server_transport_t * make_server(control_fixture_t * f, rocketnet_server_transport_io_operations_t * operations, int clients, bool staged, int payload_limit = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES)
{
    f->staged = staged;
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    config.max_clients = clients;
    config.max_payload_bytes = payload_limit;
    config.max_fragmented_messages = 1;
    // The real adapter is closed immediately below; all behavior under test
    // runs through the deterministic buffered or staged fixture operations.
    snprintf(config.transport, sizeof(config.transport), "%s", "loopback");
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config);
    CHECK(server);
    rn_native_server_io_stop_and_close(&server->io);
    *operations = {};
    operations->destroy = fixture_destroy; operations->stop = fixture_stop; operations->stopped = fixture_stopped;
    operations->send_batch = send_batch; operations->receive_batch = receive_batch;
    if (staged)
    {
        operations->flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
        operations->start_packet = start_packet; operations->finish_packet = finish_packet;
        operations->abort_packet = abort_packet; operations->flush = flush; operations->receive = receive;
    }
    server->io.instance = f; server->io.operations = operations;
    for (int i = 0; i < clients; ++i)
    {
        rocketnet_address_t address;
        CHECK(rocketnet_address_parse(&address, "127.0.0.1:0")); address.port = (uint16_t)( 1000 + i );
        int index = rocketnet_encryption_map_add(server->protocol.encryption_map, &address, 100.0);
        CHECK(index >= 0);
        auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, index);
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
        CHECK(rocketnet_client_slots_assign(server->protocol.client_slots, i + 1, index, user_data) == i);
        mapping->control->client_index = i; mapping->client_id = i + 1; mapping->control->established = 1;
        mapping->control->timeout_seconds = 1000;
        // Synthetic connections have test keys; this fixture does not
        // model authentication. It exercises the real update/send scheduler.
        memset(&mapping->keys, i + 1, sizeof(mapping->keys));
    }
    CHECK(rocketnet_address_parse(&f->request_address, "127.0.0.1:20000"));
    int pending = rocketnet_encryption_map_add(server->protocol.encryption_map, &f->request_address, 100.0);
    CHECK(pending >= 0);
    auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, pending);
    mapping->challenge_packet[0] = ROCKETNET_CONNECTION_CHALLENGE_PACKET;
    uint8_t * cursor = f->request;
    rocketnet_write_prefilter_header(&cursor, ROCKETNET_CONNECTION_REQUEST_PACKET, server->protocol.packet_filter);
    return server;
}

static void seed_stats_connection_ids(rocketnet_server_transport_t * server, int clients)
{
    // The fixture assigns slots directly. Production assigns these generation
    // ids at connect; async stats only publishes a still-matching generation.
    for (int i = 0; i < clients; ++i)
        server->protocol.connection_ids[i] = ++server->protocol.next_connection_id;
}

static void seed_stats_rtt_samples(rocketnet_server_transport_t * server, int clients)
{
    // Generate the sample through the ACK producer so both the live endpoint
    // and its active stats delta see the same 17 ms observation.
    rocketnet_ack_block_t block = {};
    block.largest = 1;
    block.first_range_length = 1;
    for (int i = 0; i < clients; ++i)
    {
        auto * endpoint = &server->protocol.acks->endpoints[i];
        CHECK(rocketnet_ack_endpoint_record_sent_classified(endpoint, 1, 100, 0, 0, 0, 0.0)
            == ROCKETNET_ACK_SENT_RECORDED);
        uint64_t acked_ids[1] = {};
        uint64_t unknown = 0, batch_full = 0;
        int acked_count = 0;
        CHECK(rocketnet_ack_endpoint_process_block(endpoint, &block, 0.017,
            acked_ids, 1, &acked_count, &unknown, &batch_full) == 1);
        CHECK(acked_count == 0 && unknown == 0 && batch_full == 0);
        CHECK(endpoint->rtt_history[1 % ROCKETNET_RTT_HISTORY_SIZE] == 17.0f);
    }
}

static void test_buffered_control_pressure(int clients = 300)
{
    auto * f = new control_fixture_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_set_stats_worker_manual_for_test(true);
    auto * server = make_server(f, &operations, clients, false);
    seed_stats_connection_ids(server, clients);
    seed_stats_rtt_samples(server, clients);
    f->requests = ROCKETNET_SERVER_MAX_RECEIVE_PACKETS;
    // Publication still follows the transport's modulo phases. Exercise
    // every configured slot before checking the high slots' RTT; keep the
    // response and ACK/keep-alive checks on every individual update.
    const int stats_phases = ( clients + ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE - 1 ) /
                             ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE;
    const double begin = rocketnet_time();
    for (int step = 0; step <= stats_phases; ++step)
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
        // Snapshot N is computed after update N and published by update N+1.
        // The last update above publishes the final modulo phase while still
        // exercising the same response and control assertions as every frame.
        if (step < stats_phases)
            CHECK(rocketnet_server_transport_stats_worker_step_for_test(server));
    }
    uint64_t submitted = 0, completed = 0, published = 0, busy_skips = 0;
    CHECK(rocketnet_server_transport_stats_worker_status_for_test(server,
        &submitted, &completed, &published, &busy_skips));
    CHECK(submitted == (uint64_t) stats_phases + 1 && completed == (uint64_t) stats_phases);
    CHECK(published == (uint64_t) clients && busy_skips == 0);
    const double elapsed_ms = (rocketnet_time() - begin) * 1000.0;
    for (int i = 0; i < clients; ++i) CHECK(server->protocol.acks->endpoints[i].rtt == 17.0f);
    rn_native_server_transport_stop_and_destroy(server); delete f;
    (void) elapsed_ms;
    printf("test_buffered_control_pressure_%d_clients\n", clients);
}

static void test_disconnect_burst_next_send(bool staged)
{
    auto * f = new control_fixture_t(); f->capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 1;
    rocketnet_server_transport_io_operations_t operations;
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

    rn_native_server_transport_stop_and_destroy(server); delete f;
    printf("test_disconnect_burst_next_send_%s\n", staged ? "staged" : "portable");
}

static void test_staged_control_fairness()
{
    auto * f = new control_fixture_t(); f->capacity = 2;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_set_stats_worker_manual_for_test(true);
    auto * server = make_server(f, &operations, 12, true);
    seed_stats_connection_ids(server, 12);
    seed_stats_rtt_samples(server, 12);
    // Every slot remains due, but only two frames are available per update.
    for (int step = 0; step < 6; ++step)
    {
        rocketnet_server_transport_update(server, 1.0 + step);
        CHECK(rocketnet_server_transport_stats_worker_step_for_test(server));
    }
    for (int i = 0; i < 12; ++i)
    {
        CHECK(f->keep_alives[i] == 1);
        CHECK(server->protocol.acks->endpoints[i].rtt == 17.0f);
    }
    rn_native_server_transport_stop_and_destroy(server); delete f;
    printf("test_staged_control_fairness\n");
}

static void test_staged_idle_maintenance_preflight()
{
    // A connected but idle slot should not touch staged frame ownership.
    // The boundary, the pre-ACK keep-alive clock, the post-ACK send clock,
    // and ACK debt each still request exactly one frame when due.
    auto * f = new control_fixture_t(); f->capacity = 16;
    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server(f, &operations, 1, true);
    server->protocol.send_rate = 1.0;
    auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map,
        rocketnet_client_slots_mapping_index(server->protocol.client_slots, 0));
    mapping->control->last_receive_time = 1000.0;
    mapping->last_keep_alive_send_time = 1000.0;
    mapping->last_send_time = 1000.0;

    rocketnet_server_transport_update(server, 1000.0);
    rocketnet_server_transport_update(server, 1000.5);
    CHECK(f->start_calls == 0 && f->aborts == 0);

    // Before any keep-alive is acknowledged, ordinary traffic does not
    // suppress the keep-alive at the exact one-second boundary.
    mapping->last_send_time = 1000.75;
    rocketnet_server_transport_update(server, 1001.0);
    CHECK(f->start_calls == 1 && f->keep_alives[0] == 1 && f->acks[0] == 0);

    // Once acknowledged, any send stamps the clock used for suppression.
    server->protocol.acks->endpoints[0].keep_alive_acked = 1;
    mapping->last_send_time = 1001.0;
    rocketnet_server_transport_update(server, 1001.5);
    CHECK(f->start_calls == 1 && f->aborts == 0);
    rocketnet_server_transport_update(server, 1002.0);
    CHECK(f->start_calls == 2 && f->keep_alives[0] == 2);

    // A single owed ACK is due before the next paced keep-alive.
    server->protocol.acks->endpoints[0].ack_debt = 1;
    rocketnet_server_transport_update(server, 1002.25);
    CHECK(f->start_calls == 3 && f->acks[0] == 1);
    CHECK(server->protocol.acks->endpoints[0].ack_debt == 0);

    // When both are due, the existing writer keeps keep-alive priority and
    // its ACK block clears the debt without a second reservation.
    server->protocol.acks->endpoints[0].ack_debt = 1;
    rocketnet_server_transport_update(server, 1003.25);
    CHECK(f->start_calls == 4 && f->keep_alives[0] == 3 && f->acks[0] == 1);
    CHECK(server->protocol.acks->endpoints[0].ack_debt == 0 && f->aborts == 0);

    // The slot API permits a connected slot with no mapping. The preflight
    // must still count that broken state, without indexing
    // an address at -1 or reserving a staged frame.
    rocketnet_client_slots_release(server->protocol.client_slots, 0);
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    CHECK(rocketnet_client_slots_assign(server->protocol.client_slots, 9, -1, user_data) == 0);
    const uint64_t missing_before = server->protocol.counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING];
    rocketnet_server_transport_update(server, 1003.5);
    CHECK(server->protocol.counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING] == missing_before + 1);
    CHECK(f->start_calls == 4);

    rn_native_server_transport_stop_and_destroy(server); delete f;
    printf("test_staged_idle_maintenance_preflight\n");
}

static void test_single_frame_arena()
{
    for (int staged = 0; staged < 2; ++staged)
    {
        auto * f = new control_fixture_t(); f->capacity = 1;
        rocketnet_server_transport_io_operations_t operations;
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
            CHECK(f->payloads == attempt); // Failed seal never reached the transport.
        }
        CHECK(checked_seals == 2);
        check_arena_copy = false; fail_seal = false;
        rn_native_server_transport_stop_and_destroy(server); delete f;
    }
    printf("test_single_frame_arena_seal_and_abort\n");
}

static void test_staged_lease_table_full()
{
    // Keep every staged frame outstanding. Distinct 16-aligned frames make
    // this a real lease-capacity check rather than a hash-collision stress.
    const int lease_limit = 131072 / 2;
    auto * f = new control_fixture_t();
    f->capacity = lease_limit + 1;
    f->lease_frames = (uint8_t*) rocketnet_malloc(NULL, (size_t) f->capacity * ROCKETNET_FRAME_STRIDE);
    CHECK(f->lease_frames);
    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server(f, &operations, 1, true);
    for (int i = 0; i < lease_limit; ++i)
    {
        rocketnet_packet_context_t context;
        CHECK(rocketnet_server_transport_start_packet(server, 0, 64, &context));
    }
    rocketnet_packet_context_t refused;
    CHECK(!rocketnet_server_transport_start_packet(server, 0, 64, &refused));
    CHECK(server->protocol.counters->values[ROCKETNET_COUNTER_SEND_LEASE_FULL] == 1);
    CHECK(f->aborts == 1);
    rn_native_server_transport_stop_and_destroy(server);
    rocketnet_free(NULL, f->lease_frames);
    delete f;
    printf("test_staged_lease_table_full\n");
}

static void test_entries_carry_connection_id(bool staged)
{
    // docs/CHANNELS.md P9a, P9b: the transport's own batches carry each
    // entry's connection id beside its client index. Two slots hold
    // connection ids 1 and 2; the server sends each client three packets
    // through the public send path, then each client's sealed payload,
    // whose ack block acknowledges those three, arrives in one update. Read
    // through the public accessors, the payload and acked id batches run
    // parallel to their client index arrays, and every entry's id is the id
    // its slot holds. Mutation that must bite: drop the tag copy at the
    // transport's payload append sites (buffered or staged); the payload id
    // checks go red. Time is an argument; the clock starts at 1000.0.
    auto * f = new control_fixture_t(); f->capacity = 16;
    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server(f, &operations, 2, staged);
    auto * protocol = &server->protocol;
    const double time = 1000.0;

    // the fixture assigns its slots directly, so each takes its id here the
    // way the protocol takes one when it assigns a slot
    for (int i = 0; i < 2; ++i)
    {
        protocol->connection_ids[i] = ++protocol->next_connection_id;
        auto * mapping = rocketnet_encryption_map_entry(protocol->encryption_map,
            rocketnet_client_slots_mapping_index(protocol->client_slots, i));
        mapping->control->last_receive_time = time;
    }
    CHECK(rocketnet_server_transport_client_connection_id(server, 0) == 1);
    CHECK(rocketnet_server_transport_client_connection_id(server, 1) == 2);

    uint64_t sequences[2][3];
    for (int c = 0; c < 2; ++c)
    {
        for (int k = 0; k < 3; ++k)
        {
            rocketnet_packet_context_t context;
            CHECK(rocketnet_server_transport_start_packet(server, c, 8, &context));
            int capacity = 0;
            uint8_t * payload = rocketnet_server_transport_packet_payload(server, &context, &capacity);
            CHECK(payload && capacity >= 8);
            memset(payload, 0x50 + k, 8);
            // the server assigns the sequence when finish accepts the payload
            // (test_unit_sequence_burn.cpp; tla/FragmentsAndAcks.tla
            // BurnedNeverNamed), so it is read after finish, never at start
            CHECK(rocketnet_server_transport_finish_packet(server, &context, 8) > 0);
            sequences[c][k] = context.sequence;
            CHECK(sequences[c][k] != 0);
        }
    }
    rocketnet_server_transport_send_packets(server);

    for (int c = 0; c < 2; ++c)
    {
        auto * mapping = rocketnet_encryption_map_entry(protocol->encryption_map,
            rocketnet_client_slots_mapping_index(protocol->client_slots, c));
        rocketnet_replay_protection_t replay;
        memset(&replay, 0, sizeof(replay));
        for (int k = 0; k < 3; ++k) rocketnet_replay_advance(&replay, sequences[c][k]);
        uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 4];
        const int block = rocketnet_ack_block_build(plaintext, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay);
        CHECK(block > 0);
        memset(plaintext + block, 0xA0 + c, 4);
        CHECK(rocketnet_address_parse(&f->feed_from[c], "127.0.0.1:0")); f->feed_from[c].port = (uint16_t)( 1000 + c );
        f->feed_bytes[c] = rocketnet_write_sealed_packet(f->feed_data[c], ROCKETNET_CONNECTION_PAYLOAD_PACKET,
            protocol->packet_filter, 1, NULL, plaintext, block + 4, protocol->protocol_id,
            mapping->connection_salt, NULL, mapping->keys.session_client_to_server);
        CHECK(f->feed_bytes[c] > 0);
    }
    f->feed_count = 2;
    rocketnet_server_transport_update(server, time + 0.001);

    const int * client_index = NULL;
    uint8_t * const * payload_data = NULL;
    const int * payload_bytes = NULL;
    const uint64_t * connection_id = NULL;
    const int num_payloads = rocketnet_server_transport_payloads(server, &client_index, &payload_data, &payload_bytes);
    CHECK(num_payloads == 2);
    CHECK(rocketnet_server_transport_payload_connection_ids(server, &connection_id) == num_payloads);
    for (int i = 0; i < num_payloads; ++i)
    {
        CHECK(payload_bytes[i] == 4 && payload_data[i][0] == 0xA0 + client_index[i]);
        CHECK(connection_id[i] != 0);
        CHECK(connection_id[i] == rocketnet_server_transport_client_connection_id(server, client_index[i]));
    }

    const int * acked_client_index = NULL;
    const uint64_t * acked_ids = NULL;
    const int num_acked = rocketnet_server_transport_acked_packets(server, &acked_client_index, &acked_ids);
    CHECK(num_acked == 6);
    CHECK(rocketnet_server_transport_acked_connection_ids(server, &connection_id) == num_acked);
    int acked_per_client[2] = {};
    for (int i = 0; i < num_acked; ++i)
    {
        CHECK(acked_client_index[i] >= 0 && acked_client_index[i] < 2);
        ++acked_per_client[acked_client_index[i]];
        CHECK(connection_id[i] != 0);
        CHECK(connection_id[i] == rocketnet_server_transport_client_connection_id(server, acked_client_index[i]));
    }
    CHECK(acked_per_client[0] == 3 && acked_per_client[1] == 3);

    // the fragment-completion path on its own: client 1 sends one payload
    // as two fragments in the next update, so the only batch entry is the
    // reassembled payload, and its tag comes from the transport's
    // fragment-completion append site (buffered or staged). Mutation that
    // must bite: drop the tag copy there; this id check goes red.
    {
        const int c = 1;
        auto * mapping = rocketnet_encryption_map_entry(protocol->encryption_map,
            rocketnet_client_slots_mapping_index(protocol->client_slots, c));
        rocketnet_replay_protection_t replay;
        memset(&replay, 0, sizeof(replay));
        for (int k = 0; k < 3; ++k) rocketnet_replay_advance(&replay, sequences[c][k]);
        uint8_t fragmented[24];
        for (int b = 0; b < 24; ++b) fragmented[b] = (uint8_t) (0xC0 + b);
        const int stride = 16;
        for (int k = 0; k < 2; ++k)
        {
            uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 24];
            const int block = rocketnet_ack_block_build(plaintext, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay);
            CHECK(block > 0);
            rocketnet_write_fragment_header(plaintext + block, 1, k, 2);
            const int fragment_bytes = k == 0 ? stride : 24 - stride;
            memcpy(plaintext + block + ROCKETNET_FRAGMENT_HEADER_BYTES, fragmented + k * stride, (size_t) fragment_bytes);
            f->feed_from[k] = f->feed_from[c];
            f->feed_bytes[k] = rocketnet_write_sealed_packet(f->feed_data[k], ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                protocol->packet_filter, 2 + (uint64_t) k, NULL, plaintext, block + ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes,
                protocol->protocol_id, mapping->connection_salt, NULL, mapping->keys.session_client_to_server);
            CHECK(f->feed_bytes[k] > 0);
        }
        f->feed_count = 2;
        rocketnet_server_transport_update(server, time + 0.002);

        const int num_fragmented = rocketnet_server_transport_payloads(server, &client_index, &payload_data, &payload_bytes);
        CHECK(num_fragmented == 1);
        CHECK(rocketnet_server_transport_payload_connection_ids(server, &connection_id) == num_fragmented);
        CHECK(client_index[0] == c);
        CHECK(payload_bytes[0] == 24 && memcmp(payload_data[0], fragmented, 24) == 0);
        CHECK(connection_id[0] == 2);
        CHECK(connection_id[0] == rocketnet_server_transport_client_connection_id(server, c));
    }

    rn_native_server_transport_stop_and_destroy(server); delete f;
    printf("test_entries_carry_connection_id_%s\n", staged ? "staged" : "portable");
}

static void test_staged_link_address_hooks()
{
    // TRANSPORTS.md, xdp (the reply-MAC rule), from the transport's side. Fed
    // one update holding a packet from an unknown address (dropped) and a
    // payload that opens under client 0's keys (view index 1), the transport
    // tells the transport about the accepted packet only. Then the transport
    // refuses every start for want of a link-layer destination: the
    // transport counts SEND_NO_LINK_ADDRESS, never SEND_RING_FULL, and sends
    // nothing. Mutation that must bite: drop the peer_accepted call, or the
    // unreachable test in the start wrapper; a check below goes red. Time
    // is an argument; the clock starts at 1000.0.
    auto * f = new control_fixture_t(); f->capacity = 16;
    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server(f, &operations, 1, true);
    operations.peer_accepted = peer_accepted;
    auto * protocol = &server->protocol;
    const double time = 1000.0;
    protocol->connection_ids[0] = ++protocol->next_connection_id;
    auto * mapping = rocketnet_encryption_map_entry(protocol->encryption_map,
        rocketnet_client_slots_mapping_index(protocol->client_slots, 0));
    mapping->control->last_receive_time = time;

    CHECK(rocketnet_address_parse(&f->feed_from[0], "127.0.0.1:3000"));
    memset(f->feed_data[0], 0x5A, 64);
    f->feed_bytes[0] = 64;

    rocketnet_replay_protection_t replay;
    memset(&replay, 0, sizeof(replay));
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 4];
    const int block = rocketnet_ack_block_build(plaintext, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay);
    CHECK(block > 0);
    memset(plaintext + block, 0xA0, 4);
    CHECK(rocketnet_address_parse(&f->feed_from[1], "127.0.0.1:1000"));
    f->feed_bytes[1] = rocketnet_write_sealed_packet(f->feed_data[1], ROCKETNET_CONNECTION_PAYLOAD_PACKET,
        protocol->packet_filter, 1, NULL, plaintext, block + 4, protocol->protocol_id,
        mapping->connection_salt, NULL, mapping->keys.session_client_to_server);
    CHECK(f->feed_bytes[1] > 0);
    f->feed_count = 2;
    rocketnet_server_transport_update(server, time + 0.001);
    CHECK(f->num_accepted == 1 && f->accepted[0] == 1);

    const int keep_alives = f->keep_alives[0], acks = f->acks[0], payloads = f->payloads;
    f->unreachable = true;
    const uint64_t ring_full = protocol->counters->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    rocketnet_packet_context_t context;
    CHECK(!rocketnet_server_transport_start_packet(server, 0, 64, &context));
    CHECK(protocol->counters->values[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == 1);
    rocketnet_server_transport_update(server, time + 1.0);
    CHECK(protocol->counters->values[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] > 1);
    CHECK(protocol->counters->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full);
    CHECK(f->keep_alives[0] == keep_alives && f->acks[0] == acks && f->payloads == payloads);

    rn_native_server_transport_stop_and_destroy(server); delete f;
    printf("test_staged_link_address_hooks\n");
}

int main(int argc, char ** argv)
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK);
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
    test_staged_idle_maintenance_preflight();
    test_single_frame_arena();
    test_staged_lease_table_full();
    test_entries_carry_connection_id(false);
    test_entries_carry_connection_id(true);
    test_staged_link_address_hooks();
    rocketnet_term();
}

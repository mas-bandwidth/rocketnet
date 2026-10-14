/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Schedule matrix over the fake portable and staged drivers. Every sequence
// of length 1..4 is applied, and every step checks that a committed control
// frame leaves on the next eligible send, leaves once, and is counted once.
// Time is an argument. The run opens no socket of its own beyond the two
// transports it creates and then replaces with the fakes.

#include "test_unit_registry.h"

// Read the transport layout and set the publish hook on the library this
// binary already links. Do not compile the transport source here: that
// second copy includes <sodium.h> on the test target's include path and,
// under the pinned amalgam rename, references rocketnet_sodium_randombytes_buf.
#define ROCKETNET_FAKE_DRIVER_CHECK(x) rocketnet_unit_check(x)
#include "sdk/rocketnet_server_transport_internal.h"
#include "test_fake_drivers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum phase_op_t
{
    OP_UPDATE = 0,
    OP_DISCONNECT,
    OP_SEND_EMPTY,
    OP_SEND_PAYLOAD,
    OP_REPEAT_SEND,
    OP_FAIL_SEND,
    OP_PARTIAL_SEND,
    OP_COUNT
};

struct phase_fixture_t
{
    control_fixture_t f;
    rocketnet_server_driver_operations_t operations;
    rocketnet_server_transport_t * server;
};

static int g_phase_staged;
static int g_phase_len;
static int g_phase_step;
static int g_phase_seq[4];

static void phase_fail(const char * text, int line)
{
    printf("check failed: ( %s ), function TestDriverPhaseScheduleMatrix, file test_unit_driver_phases.cpp, line %d staged=%d len=%d step=%d ops=%d,%d,%d,%d\n",
        text, line, g_phase_staged, g_phase_len, g_phase_step,
        g_phase_seq[0], g_phase_seq[1], g_phase_seq[2], g_phase_seq[3]);
    fflush(stdout);
    exit(1);
}

#define PHASE_CHECK(x) do { if (!(x)) phase_fail(#x, __LINE__); } while (0)

static void phase_sums(const rocketnet_server_transport_t * server, uint64_t * packets, uint64_t * bytes)
{
    uint64_t packet_sum = server->stats_pending_packets_sent;
    uint64_t byte_sum = server->stats_pending_bytes_sent;
    for (int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; ++i)
    {
        packet_sum += server->stats_frames[i].packets_sent;
        byte_sum += server->stats_frames[i].bytes_sent;
    }
    *packets = packet_sum;
    *bytes = byte_sum;
}

static uint64_t phase_accounted(const phase_fixture_t * p)
{
    uint64_t n = (uint64_t) (p->f.responses + p->f.payloads + p->f.disconnects + p->f.pending_disconnects);
    for (int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i)
        n += (uint64_t) (p->f.keep_alives[i] + p->f.acks[i]);
    if (!p->f.staged)
        n += (uint64_t) p->server->send_buffer.num_packets;
    return n;
}

static int phase_keep_sum(const control_fixture_t * f)
{
    int n = 0;
    for (int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i)
        n += f->keep_alives[i];
    return n;
}

static bool phase_full_publish(phase_op_t op)
{
    return op == OP_UPDATE || op == OP_SEND_EMPTY || op == OP_SEND_PAYLOAD || op == OP_REPEAT_SEND;
}

static void phase_reset(phase_fixture_t * p)
{
    p->f.requests = 0;
    p->f.responses = 0;
    p->f.payloads = 0;
    p->f.aborts = 0;
    p->f.disconnects = 0;
    p->f.pending_disconnects = 0;
    p->f.used = 0;
    p->f.max_send = -1;
    memset(p->f.keep_alives, 0, sizeof(p->f.keep_alives));
    memset(p->f.acks, 0, sizeof(p->f.acks));
    memset(p->f.staged_kind, 0, sizeof(p->f.staged_kind));
    seed_connected_client(p->server, 0);
    p->server->send_staged = 0;
    p->server->send_flushed = 0;
    p->server->stats_pending_packets_sent = 0;
    p->server->stats_pending_bytes_sent = 0;
    p->server->send_buffer.num_packets = 0;
    p->server->payload_ring.num_packets = 0;
    p->server->maintenance_next_client = 0;
    p->server->packet_leases.reset();
    for (int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; ++i)
    {
        p->server->stats_frames[i].packets_sent = 0;
        p->server->stats_frames[i].bytes_sent = 0;
    }
}

static void phase_send_payload(phase_fixture_t * p)
{
    if (!rocketnet_server_transport_is_client_connected(p->server, 0))
        return;
    rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet(p->server, 0, 4, &context);
    if (!frame)
        return;
    memcpy(frame + context.payload_index, "data", 4);
    rocketnet_server_transport_finish_packet(p->server, &context, 4);
}

static void phase_apply(phase_fixture_t * p, phase_op_t op, double time, bool * owe)
{
    const bool connected = rocketnet_server_transport_is_client_connected(p->server, 0) != 0;
    const bool owe_before = *owe;
    const int disc_before = p->f.disconnects;
    const int pend_before = p->f.pending_disconnects;
    const int pay_before = p->f.payloads;
    const int buf_before = p->server->send_buffer.num_packets;
    const int used_before = p->f.used;
    const int keeps_before = phase_keep_sum(&p->f);
    const uint64_t flush_before = p->server->send_flushed;
    uint64_t packets_before = 0;
    uint64_t bytes_before = 0;
    phase_sums(p->server, &packets_before, &bytes_before);

    switch (op)
    {
    case OP_UPDATE:
        rocketnet_server_transport_update(p->server, time);
        break;
    case OP_DISCONNECT:
        rocketnet_server_transport_disconnect_client(p->server, 0);
        break;
    case OP_SEND_EMPTY:
        rocketnet_server_transport_send_packets(p->server);
        break;
    case OP_SEND_PAYLOAD:
        phase_send_payload(p);
        rocketnet_server_transport_send_packets(p->server);
        break;
    case OP_REPEAT_SEND:
    {
        rocketnet_server_transport_send_packets(p->server);
        const int disc_mid = p->f.disconnects;
        const int pay_mid = p->f.payloads;
        const int keeps_mid = phase_keep_sum(&p->f);
        uint64_t packets_mid = 0;
        uint64_t bytes_mid = 0;
        phase_sums(p->server, &packets_mid, &bytes_mid);
        rocketnet_server_transport_send_packets(p->server);
        PHASE_CHECK(p->f.disconnects == disc_mid);
        PHASE_CHECK(p->f.payloads == pay_mid);
        PHASE_CHECK(phase_keep_sum(&p->f) == keeps_mid);
        uint64_t packets_after = 0;
        uint64_t bytes_after = 0;
        phase_sums(p->server, &packets_after, &bytes_after);
        PHASE_CHECK(packets_after == packets_mid);
        PHASE_CHECK(bytes_after == bytes_mid);
        break;
    }
    case OP_FAIL_SEND:
        p->f.max_send = 0;
        rocketnet_server_transport_send_packets(p->server);
        p->f.max_send = -1;
        break;
    case OP_PARTIAL_SEND:
        p->f.max_send = 1;
        rocketnet_server_transport_send_packets(p->server);
        p->f.max_send = -1;
        break;
    default:
        break;
    }

    if (op == OP_DISCONNECT && connected)
        *owe = true;

    uint64_t packets = 0;
    uint64_t bytes = 0;
    phase_sums(p->server, &packets, &bytes);
    PHASE_CHECK(packets == phase_accounted(p));
    if (packets == packets_before)
        PHASE_CHECK(bytes == bytes_before);
    else
        PHASE_CHECK(bytes > bytes_before);
    PHASE_CHECK(p->server->stats_pending_packets_sent == 0);
    PHASE_CHECK(p->server->stats_pending_bytes_sent == 0);
    PHASE_CHECK(p->f.disconnects <= ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
    PHASE_CHECK(p->f.disconnects + p->f.pending_disconnects <= ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
    PHASE_CHECK(p->server->send_flushed <= p->server->send_staged);
    if (p->f.staged)
        PHASE_CHECK(p->f.used == p->f.pending_disconnects);
    else
        PHASE_CHECK(p->server->payload_ring.num_packets == 0);

    if (op == OP_SEND_PAYLOAD)
        PHASE_CHECK(p->f.payloads == pay_before || p->f.payloads == pay_before + 1);
    else
        PHASE_CHECK(p->f.payloads == pay_before);

    if (op == OP_DISCONNECT && connected)
    {
        PHASE_CHECK(p->f.disconnects == disc_before);
        if (p->f.staged)
            PHASE_CHECK(p->f.pending_disconnects == pend_before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
        else
            PHASE_CHECK(p->server->send_buffer.num_packets == buf_before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
    }

    if (owe_before && phase_full_publish(op))
    {
        PHASE_CHECK(p->f.disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
        PHASE_CHECK(p->f.pending_disconnects == 0);
        if (!p->f.staged)
            PHASE_CHECK(p->server->send_buffer.num_packets == 0);
    }

    if (op == OP_FAIL_SEND)
    {
        PHASE_CHECK(p->f.disconnects == disc_before);
        PHASE_CHECK(p->f.pending_disconnects == pend_before);
        PHASE_CHECK(phase_keep_sum(&p->f) == keeps_before);
        if (p->f.staged)
        {
            PHASE_CHECK(p->server->send_flushed == flush_before);
            PHASE_CHECK(p->f.used == used_before);
        }
        else
            PHASE_CHECK(p->server->send_buffer.num_packets == buf_before);
    }

    if (op == OP_PARTIAL_SEND)
    {
        const int gained = p->f.disconnects - disc_before;
        PHASE_CHECK(gained == 0 || gained == 1);
        if (p->f.staged)
        {
            if (pend_before > 1)
            {
                PHASE_CHECK(gained == 1);
                PHASE_CHECK(p->f.pending_disconnects == pend_before - 1);
                PHASE_CHECK(p->server->send_flushed == flush_before);
            }
            else if (pend_before == 1)
            {
                PHASE_CHECK(gained == 1);
                PHASE_CHECK(p->f.pending_disconnects == 0);
                PHASE_CHECK(p->server->send_flushed == p->server->send_staged);
            }
            else
            {
                PHASE_CHECK(gained == 0);
                PHASE_CHECK(p->f.pending_disconnects == 0);
                PHASE_CHECK(p->server->send_flushed == flush_before);
            }
        }
        else
        {
            const int buf = p->server->send_buffer.num_packets;
            if (buf_before > 1)
            {
                PHASE_CHECK(gained == 1);
                PHASE_CHECK(buf == buf_before - 1);
            }
            else if (buf_before == 1)
            {
                PHASE_CHECK(gained == 1);
                PHASE_CHECK(buf == 0);
            }
            else
            {
                PHASE_CHECK(gained == 0);
                PHASE_CHECK(buf == 0);
            }
        }
    }

    if (!rocketnet_server_transport_is_client_connected(p->server, 0))
        PHASE_CHECK(rocketnet_server_transport_num_connected_clients(p->server) == 0);
}

static void run_phase_matrix(bool staged)
{
    auto * p = new phase_fixture_t();
    p->f.capacity = 64;
    p->server = make_server(&p->f, &p->operations, 1, staged);
    g_phase_staged = staged ? 1 : 0;

    for (int len = 1; len <= 4; ++len)
    {
        int span = 1;
        for (int i = 0; i < len; ++i)
            span *= OP_COUNT;
        for (int n = 0; n < span; ++n)
        {
            int seq[4];
            int value = n;
            for (int i = 0; i < len; ++i)
            {
                seq[i] = value % OP_COUNT;
                value /= OP_COUNT;
            }
            for (int i = len; i < 4; ++i)
                seq[i] = -1;
            g_phase_len = len;
            g_phase_step = 0;
            memcpy(g_phase_seq, seq, sizeof(seq));
            phase_reset(p);
            bool owe = false;
            double time = 1.0;
            for (int step = 0; step < len; ++step)
            {
                g_phase_step = step;
                phase_apply(p, (phase_op_t) seq[step], time, &owe);
                time += 1.0;
            }
        }
    }

    rocketnet_server_transport_destroy(p->server);
    delete p;
}

static void TestDriverPhaseScheduleMatrix()
{
    run_phase_matrix(true);
    run_phase_matrix(false);
}

// update, then disconnect_client, then send_packets. The burst leaves on
// the next send and a second send emits nothing new. Staged runs first.
static void TestDisconnectBurstLeavesOnNextSend()
{
    for (int staged = 1; staged >= 0; --staged)
    {
        auto * f = new control_fixture_t();
        f->capacity = 64;
        rocketnet_server_driver_operations_t operations;
        auto * server = make_server(f, &operations, 1, staged != 0);

        rocketnet_server_transport_update(server, 1.0);
        const int stats_index = server->current_stats_index;
        const uint64_t before = server->stats_frames[stats_index].packets_sent;
        const uint64_t before_bytes = server->stats_frames[stats_index].bytes_sent;
        const int keeps = f->keep_alives[0];
        uint64_t packets_before = 0;
        uint64_t bytes_before_sum = 0;
        {
            packets_before = server->stats_pending_packets_sent;
            bytes_before_sum = server->stats_pending_bytes_sent;
            for (int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; ++i)
            {
                packets_before += server->stats_frames[i].packets_sent;
                bytes_before_sum += server->stats_frames[i].bytes_sent;
            }
        }

        rocketnet_server_transport_disconnect_client(server, 0);
        rocketnet_unit_check(rocketnet_server_transport_num_connected_clients(server) == 0);
        rocketnet_unit_check(f->disconnects == 0);
        rocketnet_unit_check(server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
        rocketnet_unit_check(server->stats_frames[stats_index].bytes_sent > before_bytes);
        if (staged)
            rocketnet_unit_check(f->pending_disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
        else
            rocketnet_unit_check(server->send_buffer.num_packets == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);

        rocketnet_server_transport_send_packets(server);
        rocketnet_unit_check(f->disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
        rocketnet_unit_check(f->pending_disconnects == 0);
        rocketnet_unit_check(server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);

        const int payloads = f->payloads;
        uint64_t packets_after = server->stats_pending_packets_sent;
        uint64_t bytes_after = server->stats_pending_bytes_sent;
        for (int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; ++i)
        {
            packets_after += server->stats_frames[i].packets_sent;
            bytes_after += server->stats_frames[i].bytes_sent;
        }
        rocketnet_unit_check(packets_after == packets_before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
        rocketnet_unit_check(bytes_after > bytes_before_sum);

        rocketnet_server_transport_send_packets(server);
        rocketnet_unit_check(f->disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);
        rocketnet_unit_check(f->pending_disconnects == 0);
        rocketnet_unit_check(f->payloads == payloads);
        rocketnet_unit_check(f->keep_alives[0] == keeps);
        uint64_t packets_second = server->stats_pending_packets_sent;
        uint64_t bytes_second = server->stats_pending_bytes_sent;
        for (int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; ++i)
        {
            packets_second += server->stats_frames[i].packets_sent;
            bytes_second += server->stats_frames[i].bytes_sent;
        }
        rocketnet_unit_check(packets_second == packets_after);
        rocketnet_unit_check(bytes_second == bytes_after);
        rocketnet_unit_check(server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS);

        rocketnet_server_transport_destroy(server);
        delete f;
    }
}

// A second connection attempt against a stored challenge is answered with
// that challenge, byte-identical (tla/ClientConnection.tla ServerRecvRequest,
// the half-open branch). The request carries the mapping's salt, which is
// what the server requires before it resends the stored bytes.
static void TestSecondConnectionAttemptResendsStoredChallenge()
{
    auto * f = new control_fixture_t();
    f->capacity = 64;
    rocketnet_server_driver_operations_t operations;
    auto * server = make_server(f, &operations, 1, false);

    const int mapping_index = rocketnet_encryption_map_find(server->protocol.encryption_map, &f->request_address);
    rocketnet_unit_check(mapping_index >= 0);
    auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, mapping_index);
    rocketnet_unit_check(mapping->client_index == -1);

    const uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    memcpy(mapping->connection_salt, salt, sizeof(salt));
    for (int i = 0; i < ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES; ++i)
        mapping->challenge_packet[i] = (uint8_t) (i * 3 + 1);

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    memset(request, 0, sizeof(request));
    memcpy(request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, salt, sizeof(salt));

    uint8_t response_a[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    uint8_t response_b[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    int response_bytes = 0;

    const int result_a = rocketnet_server_protocol_process_connection_request(
        &server->protocol, &f->request_address, request, (int) sizeof(request), 1000, 100.0,
        response_a, &response_bytes);
    rocketnet_unit_check(result_a == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE);
    rocketnet_unit_check(response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES);

    const int result_b = rocketnet_server_protocol_process_connection_request(
        &server->protocol, &f->request_address, request, (int) sizeof(request), 1000, 100.0,
        response_b, &response_bytes);
    rocketnet_unit_check(result_b == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE);
    rocketnet_unit_check(response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES);

    rocketnet_unit_check(memcmp(response_a, mapping->challenge_packet, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES) == 0);
    rocketnet_unit_check(memcmp(response_a, response_b, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES) == 0);

    rocketnet_server_transport_destroy(server);
    delete f;
}

// An established mapping idle past its timeout releases its slot and loses
// its mapping (tla/ClientConnection.tla Tick, the stimed branch). The slot
// can take a new client. The timeout does not emit a disconnect burst.
static void TestReconnectAfterIdleTimeout()
{
    auto * f = new control_fixture_t();
    f->capacity = 64;
    rocketnet_server_driver_operations_t operations;
    auto * server = make_server(f, &operations, 1, false);

    rocketnet_unit_check(rocketnet_server_transport_num_connected_clients(server) == 1);

    const int mapping_index = rocketnet_client_slots_mapping_index(server->protocol.client_slots, 0);
    rocketnet_unit_check(mapping_index >= 0);
    auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, mapping_index);
    mapping->timeout_seconds = 5;
    mapping->last_receive_time = 100.0;

    rocketnet_server_transport_update(server, 104.0);
    rocketnet_unit_check(rocketnet_server_transport_num_connected_clients(server) == 1);
    rocketnet_unit_check(rocketnet_server_transport_is_client_connected(server, 0));

    rocketnet_server_transport_update(server, 106.0);
    rocketnet_unit_check(rocketnet_server_transport_num_connected_clients(server) == 0);
    rocketnet_unit_check(!rocketnet_server_transport_is_client_connected(server, 0));
    rocketnet_unit_check(f->disconnects == 0);

    rocketnet_address_t address;
    rocketnet_unit_check(rocketnet_address_parse(&address, "127.0.0.1:1000"));
    rocketnet_unit_check(rocketnet_encryption_map_find(server->protocol.encryption_map, &address) == -1);

    const int index = rocketnet_encryption_map_add(server->protocol.encryption_map, &address, 200.0);
    rocketnet_unit_check(index >= 0);
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    rocketnet_unit_check(rocketnet_client_slots_assign(server->protocol.client_slots, 7, index, user_data) == 0);
    rocketnet_unit_check(rocketnet_server_transport_num_connected_clients(server) == 1);

    rocketnet_server_transport_destroy(server);
    delete f;
}

REGISTER_UNIT_TEST(TestDriverPhaseScheduleMatrix);
REGISTER_UNIT_TEST(TestDisconnectBurstLeavesOnNextSend);
REGISTER_UNIT_TEST(TestSecondConnectionAttemptResendsStoredChallenge);
REGISTER_UNIT_TEST(TestReconnectAfterIdleTimeout);

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Fake portable and fake staged server drivers. The caller includes the
// transport source in this translation unit first, then this header, and
// substitutes the fake for the driver create granted before any update runs.
// The fake records every packet it sees instead of sending it.

#pragma once

#include "sdk/rocketnet_server_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ROCKETNET_FAKE_DRIVER_CHECK
#define ROCKETNET_FAKE_DRIVER_CHECK(x) do { if (!(x)) { fprintf(stderr, "fake driver check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)
#endif

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
    // Deliver at most this many frames from the next publish. Negative means
    // the whole publish. Zero accepts nothing. A short count leaves the tail
    // queued and reports that count as the publish result.
    int max_send;
    uint8_t staged_kind[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    rocketnet_address_t reserved_to;
    uint8_t frame[ROCKETNET_FRAME_STRIDE];
    rocketnet_server_socket_process_packets_t received;
};

static void observe(control_fixture_t * f, const rocketnet_address_t * to, const uint8_t * packet, bool at_seal)
{
    if (to->port == f->request_address.port) { ++f->responses; return; }
    const int client = to->port - 1000;
    ROCKETNET_FAKE_DRIVER_CHECK(client >= 0 && client < ROCKETNET_MAX_CLIENTS);
    if (packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET) ++f->keep_alives[client];
    else if (packet[0] == ROCKETNET_CONNECTION_ACK_PACKET) ++f->acks[client];
    else if (packet[0] == ROCKETNET_CONNECTION_PAYLOAD_PACKET) ++f->payloads;
    else if (packet[0] == ROCKETNET_CONNECTION_DISCONNECT_PACKET)
    {
        // A staged seal only queues the frame. Flush promotes the prefix
        // that the publish actually accepts.
        if (at_seal && f->staged) ++f->pending_disconnects;
        else ++f->disconnects;
    }
    else ROCKETNET_FAKE_DRIVER_CHECK(false);
}

static void fixture_destroy(void *) {}
static bool fixture_stop(void *) { return true; }

// A short publish stores its count where the transport reads it. A transport
// build that has no publish hook treats every publish as complete, which is
// the behavior of a driver that does not report a short send.
static void note_short_publish(int accepted, int offered)
{
#ifdef ROCKETNET_SERVER_PUBLISH_HOOK
    if (accepted < offered)
        rocketnet_server_publish_accepted = accepted;
#else
    (void) accepted;
    (void) offered;
#endif
}

static void send_batch(void * p, const rocketnet_platform_send_t * sends, int count)
{
    ROCKETNET_FAKE_DRIVER_CHECK(count > 0 && count <= ROCKETNET_SERVER_MAX_SEND_PACKETS);
    auto * f = (control_fixture_t *) p;
    int deliver = count;
    if (f->max_send >= 0 && f->max_send < count)
        deliver = f->max_send;
    for (int i = 0; i < deliver; ++i)
        observe(f, sends[i].to, sends[i].packet_data, false);
    note_short_publish(deliver, count);
}

static int receive_batch(void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity)
{
    auto * f = (control_fixture_t *) p;
    ROCKETNET_FAKE_DRIVER_CHECK(f->requests <= capacity);
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
    auto * f = (control_fixture_t *) p;
    if (f->used >= f->capacity) return NULL;
    f->reserved_to = *to;
    *id = (uint64_t) f->used;
    return f->frame;
}

static void finish_packet(void * p, uint64_t, uint8_t * frame, int)
{
    auto * f = (control_fixture_t *) p;
    const uint8_t * packet = frame + ROCKETNET_FRAME_WIRE_OFFSET;
    const int disconnect = packet[0] == ROCKETNET_CONNECTION_DISCONNECT_PACKET;
    observe(f, &f->reserved_to, packet, true);
    ROCKETNET_FAKE_DRIVER_CHECK(f->used >= 0 && f->used < ROCKETNET_SERVER_MAX_SEND_PACKETS);
    f->staged_kind[f->used] = (uint8_t) (disconnect ? 1 : 0);
    ++f->used;
}

static void abort_packet(void * p, uint64_t, uint8_t *) { ++((control_fixture_t *) p)->aborts; }

static void flush(void * p)
{
    auto * f = (control_fixture_t *) p;
    const int n = f->used;
    int accept = n;
    if (f->max_send >= 0 && f->max_send < n)
        accept = f->max_send;
    for (int i = 0; i < accept; ++i)
    {
        if (f->staged_kind[i])
        {
            ROCKETNET_FAKE_DRIVER_CHECK(f->pending_disconnects > 0);
            --f->pending_disconnects;
            ++f->disconnects;
        }
    }
    const int remain = n - accept;
    for (int i = 0; i < remain; ++i)
        f->staged_kind[i] = f->staged_kind[accept + i];
    f->used = remain;
    note_short_publish(accept, n);
}

static rocketnet_server_socket_process_packets_t * receive(void * p) { return &((control_fixture_t *) p)->received; }

// Release a connected slot and its mapping, then connect client i again at
// 127.0.0.1:(1000+i). The lowest free slot is i when the caller fills slots
// in order, or when i is the only client and it was just released.
static void seed_connected_client(rocketnet_server_transport_t * server, int i)
{
    if (rocketnet_client_slots_connected(server->protocol.client_slots, i))
    {
        const int old_index = rocketnet_client_slots_mapping_index(server->protocol.client_slots, i);
        rocketnet_client_slots_release(server->protocol.client_slots, i);
        if (old_index >= 0)
            rocketnet_encryption_map_remove(server->protocol.encryption_map, old_index);
    }
    rocketnet_address_t address;
    ROCKETNET_FAKE_DRIVER_CHECK(rocketnet_address_parse(&address, "127.0.0.1:0"));
    address.port = 1000 + i;
    const int existing = rocketnet_encryption_map_find(server->protocol.encryption_map, &address);
    if (existing >= 0)
        rocketnet_encryption_map_remove(server->protocol.encryption_map, existing);
    const int index = rocketnet_encryption_map_add(server->protocol.encryption_map, &address, 100.0);
    ROCKETNET_FAKE_DRIVER_CHECK(index >= 0);
    auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, index);
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    ROCKETNET_FAKE_DRIVER_CHECK(rocketnet_client_slots_assign(server->protocol.client_slots, i + 1, index, user_data) == i);
    mapping->client_index = i;
    mapping->client_id = i + 1;
    mapping->established = 1;
    mapping->timeout_seconds = 1000;
    memset(&mapping->keys, i + 1, sizeof(mapping->keys));
    server->protocol.acks->endpoints[i].rtt_history[0] = 17.0f;
    server->protocol.acks->endpoints[i].keep_alive_acked = 0;
    server->protocol.acks->endpoints[i].ack_debt = 0;
}

static rocketnet_server_transport_t * make_server(control_fixture_t * f, rocketnet_server_driver_operations_t * operations, int clients, bool staged, int payload_limit = ROCKETNET_MAX_PAYLOAD_BYTES)
{
    f->staged = staged;
    f->max_send = -1;
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config(&config);
    // A release build refuses the published development keys, so the fixture
    // supplies its own production identity. The fixture never seals with
    // these keys; each connected mapping carries its own key bytes.
    config.protocol_id = 0x6a09e667f3bcc909ULL;
    for (int i = 0; i < 32; ++i)
    {
        config.backend_public_key[i] = (uint8_t) (0x21 + i);
        config.server_public_key[i] = (uint8_t) (0x41 + i);
        config.server_secret_key[i] = (uint8_t) (0x61 + i);
    }
    config.max_clients = clients;
    config.max_payload_bytes = payload_limit;
    config.num_reassembly_slots = 1;
    strcpy(config.driver, "portable");
    auto * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config, 0.0);
    ROCKETNET_FAKE_DRIVER_CHECK(server);
    rocketnet_server_driver_close(&server->io);
    *operations = rocketnet_server_driver_operations_t();
    operations->destroy = fixture_destroy;
    operations->stop = fixture_stop;
    operations->send_batch = send_batch;
    operations->receive_batch = receive_batch;
    if (staged)
    {
        operations->flags = ROCKETNET_DRIVER_STAGED | ROCKETNET_DRIVER_FLUSH_ON_UPDATE;
        operations->start_packet = start_packet;
        operations->finish_packet = finish_packet;
        operations->abort_packet = abort_packet;
        operations->flush = flush;
        operations->receive = receive;
    }
    server->io.instance = f;
    server->io.operations = operations;
    for (int i = 0; i < clients; ++i)
        seed_connected_client(server, i);
    ROCKETNET_FAKE_DRIVER_CHECK(rocketnet_address_parse(&f->request_address, "127.0.0.1:20000"));
    const int pending = rocketnet_encryption_map_add(server->protocol.encryption_map, &f->request_address, 100.0);
    ROCKETNET_FAKE_DRIVER_CHECK(pending >= 0);
    auto * mapping = rocketnet_encryption_map_entry(server->protocol.encryption_map, pending);
    mapping->challenge_packet[0] = ROCKETNET_CONNECTION_CHALLENGE_PACKET;
    uint8_t * cursor = f->request;
    rocketnet_write_prefilter_header(&cursor, ROCKETNET_CONNECTION_REQUEST_PACKET, server->protocol.filter_value);
    return server;
}

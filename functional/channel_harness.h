/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The channel harness every message type's functional test uses: two ends
    -- a server and a client -- driven over the in-memory network simulator
    (sdk/rocketnet_network_simulator.h) with loss, duplication, latency and
    jitter injected by the simulator's deterministic seed. The harness knows
    no message type: a type card passes its code and reads what arrived.

    The SDK message objects run over an rn_message_link_t transport seam. The
    harness binds both real message objects to deterministic protocol-backed
    links, with the simulator as the wire between them. This is what the
    harness takes from yojimbo's and netcode's soak tests (two endpoints
    driven over a simulated link, deterministic seed for exact replay) and
    what it refuses: real sockets, wall-clock sleeps, and any non-mocked
    time (time is an argument the test advances, so a failing run replays
    exactly).

    The harness's message framing is minimal: a 4-byte little-endian channel
    code precedes the payload bytes, so a type card's code rides the wire and
    comes back on the delivery. Every measured number (completion time,
    resend count, wire bytes per direction) prints only on failure.

    All memory is carved at create: the protocol engines, the maps, and the
    delivery rings are created once; the exchange and the updates allocate
    nothing after create.
*/

#ifndef ROCKETNET_CHANNEL_HARNESS_H
#define ROCKETNET_CHANNEL_HARNESS_H

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_message_object.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_wire.h"
#include "rocketnet_fragment.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_ack.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES 32
#define ROCKETNET_CHANNEL_HARNESS_MAX_ENDPOINT_PAYLOADS 256
#define ROCKETNET_CHANNEL_HARNESS_MAX_PACKETS   32

static uint64_t rocketnet_channel_allocator_calls;

static inline void * rocketnet_channel_counting_malloc( void * context, size_t bytes )
{
    (void) context;
    rocketnet_channel_allocator_calls++;
    return malloc( bytes );
}

static inline void rocketnet_channel_counting_free( void * context, void * memory )
{
    (void) context;
    free( memory );
}

// A delivered message the harness collected: the channel, the bytes view
// (valid until the next collect) and a cheap FNV hash of its payload.
typedef struct rocketnet_channel_delivery_t
{
    int channel;
    const uint8_t * data;
    int bytes;
    uint64_t hash;
} rocketnet_channel_delivery_t;

// A bounded delivery ring, filled at receive and drained by collect. The
// buffers are the harness's, carved at create; nothing here allocates.
typedef struct rocketnet_channel_delivery_ring_t
{
    int head;
    int tail;
    int count;
    int free_head;
    int channel[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES];
    int len[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES];
    int next[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES];
    uint8_t * slot_data[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES];
    uint8_t * storage;
    int max_payload;
} rocketnet_channel_delivery_ring_t;

struct rocketnet_channel_harness_t;

typedef struct rocketnet_channel_endpoint_t
{
    struct rocketnet_channel_harness_t * harness;
    int is_server;
    int pending_frame;
    int outgoing_count;
    int payload_count;
    uint8_t frame[ROCKETNET_CHANNEL_HARNESS_MAX_PACKETS][ROCKETNET_FRAME_STRIDE];
    int frame_bytes[ROCKETNET_CHANNEL_HARNESS_MAX_PACKETS];
    uint8_t (* payload)[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];
    uint8_t * payload_ptr[ROCKETNET_CHANNEL_HARNESS_MAX_ENDPOINT_PAYLOADS];
    int payload_bytes[ROCKETNET_CHANNEL_HARNESS_MAX_ENDPOINT_PAYLOADS];
    int payload_client_index[ROCKETNET_CHANNEL_HARNESS_MAX_ENDPOINT_PAYLOADS];
} rocketnet_channel_endpoint_t;

typedef struct rocketnet_channel_harness_t
{
    struct rocketnet_network_simulator_t * sim;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_message_client_t * message_client;
    struct rocketnet_message_server_t * message_server;
    struct rocketnet_message_config_t message_config;
    rocketnet_channel_endpoint_t client_endpoint;
    rocketnet_channel_endpoint_t server_endpoint;
    struct rocketnet_address_t client_addr;
    struct rocketnet_address_t server_addr;

    rocketnet_channel_delivery_ring_t to_server;
    rocketnet_channel_delivery_ring_t to_client;

    double now;                     // the test clock, advanced by pump
    uint64_t unix_ts;              // captured at create, for token checks
    int client_slot;               // the connected server slot (0)
    int connected;
    int server_connected;
    uint64_t overflow_count;

    // link knobs, held here and applied to the simulator
    double latency;
    double jitter;
    float packet_loss_percent;
    float duplicate_percent;

    // measured counters
    uint64_t resends;
    int64_t wire_bytes_to_server;
    int64_t wire_bytes_to_client;
    uint64_t allocation_snapshot;
    uint64_t next_packet_id;
    int counting_allocator_installed;
    uint64_t seed; // the simulator seed this run used
} rocketnet_channel_harness_t;

static inline uint64_t rocketnet_channel_hash( const uint8_t * data, int bytes )
{
    uint64_t h = 1469598103934665603ULL;
    for ( int i = 0; i < bytes; i++ )
    {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline int rocketnet_channel_delivery_ring_init( rocketnet_channel_delivery_ring_t * ring, int max_payload )
{
    memset( ring, 0, sizeof(*ring) );
    ring->head = -1;
    ring->tail = -1;
    ring->free_head = 0;
    ring->max_payload = max_payload;
    for ( int i = 0; i < ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES; i++ )
    {
        ring->next[i] = i + 1;
        ring->slot_data[i] = NULL;
    }
    ring->next[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES - 1] = -1;
    const size_t storage_bytes = (size_t) ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES * (size_t) max_payload;
    ring->storage = (uint8_t *) rocketnet_channel_counting_malloc( NULL, storage_bytes );
    if ( !ring->storage )
        return 0;
    for ( int i = 0; i < ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES; i++ )
        ring->slot_data[i] = ring->storage + (size_t) i * (size_t) max_payload;
    return 1;
}

static inline void rocketnet_channel_delivery_ring_destroy( rocketnet_channel_delivery_ring_t * ring )
{
    if ( ring->storage )
        rocketnet_channel_counting_free( NULL, ring->storage );
    memset( ring, 0, sizeof(*ring) );
}

static inline int rocketnet_channel_delivery_ring_push( rocketnet_channel_harness_t * h, rocketnet_channel_delivery_ring_t * ring, int channel, const uint8_t * data, int bytes )
{
    if ( bytes < 0 || bytes > ring->max_payload || ring->free_head < 0 )
    {
        h->overflow_count++;
        return 0;
    }
    const int slot = ring->free_head;
    ring->free_head = ring->next[slot];
    ring->channel[slot] = channel;
    ring->len[slot] = bytes;
    if ( bytes > 0 )
        memcpy( ring->slot_data[slot], data, (size_t) bytes );
    ring->next[slot] = -1;
    if ( ring->tail >= 0 )
        ring->next[ring->tail] = slot;
    else
        ring->head = slot;
    ring->tail = slot;
    ring->count++;
    return 1;
}

static inline int rocketnet_channel_delivery_ring_pop_channel( rocketnet_channel_harness_t * h, rocketnet_channel_delivery_ring_t * ring, int channel, rocketnet_channel_delivery_t * out )
{
    int previous = -1;
    int slot = ring->head;
    while ( slot >= 0 && ring->channel[slot] != channel )
    {
        previous = slot;
        slot = ring->next[slot];
    }
    if ( slot < 0 )
        return 0;
    const int after = ring->next[slot];
    if ( previous >= 0 )
        ring->next[previous] = after;
    else
        ring->head = after;
    if ( ring->tail == slot )
        ring->tail = previous;
    out->channel = ring->channel[slot];
    out->data = ring->slot_data[slot];
    out->bytes = ring->len[slot];
    out->hash = rocketnet_channel_hash( ring->slot_data[slot], ring->len[slot] );
    ring->next[slot] = ring->free_head;
    ring->free_head = slot;
    ring->count--;
    if ( ring->count == 0 )
        ring->head = ring->tail = -1;
    (void) h;
    return 1;
}

static inline int rocketnet_channel_harness_connected( const rocketnet_channel_harness_t * h )
{
    return h->connected;
}

// Endpoint or delivery-capacity exhaustion is a harness failure, never a
// silent transport drop. Type tests should assert this remains zero.
static inline uint64_t rocketnet_channel_harness_overflows( const rocketnet_channel_harness_t * h )
{
    return h->overflow_count;
}

static inline void rocketnet_channel_endpoint_payload( rocketnet_channel_endpoint_t * endpoint, int client_index, const uint8_t * data, int bytes )
{
    if ( endpoint->payload_count >= ROCKETNET_CHANNEL_HARNESS_MAX_ENDPOINT_PAYLOADS || bytes <= 0 || bytes > ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES )
    {
        endpoint->harness->overflow_count++;
        return;
    }
    const int index = endpoint->payload_count++;
    memcpy( endpoint->payload[index], data, (size_t) bytes );
    endpoint->payload_ptr[index] = endpoint->payload[index];
    endpoint->payload_bytes[index] = bytes;
    endpoint->payload_client_index[index] = client_index;
}

static inline void rocketnet_channel_endpoint_send_control( rocketnet_channel_endpoint_t * endpoint, const uint8_t * data, int bytes )
{
    if ( bytes <= 0 )
        return;
    rocketnet_channel_harness_t * h = endpoint->harness;
    const struct rocketnet_address_t * from = endpoint->is_server ? &h->server_addr : &h->client_addr;
    const struct rocketnet_address_t * to = endpoint->is_server ? &h->client_addr : &h->server_addr;
    if ( endpoint->is_server )
        h->wire_bytes_to_client += bytes;
    else
        h->wire_bytes_to_server += bytes;
    rocketnet_network_simulator_send( h->sim, from, to, data, bytes, h->now );
}

static inline void rocketnet_channel_endpoint_update( void * transport, double time )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    rocketnet_channel_harness_t * h = endpoint->harness;
    endpoint->payload_count = 0;
    struct rocketnet_address_t from;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];

    if ( endpoint->is_server )
    {
        h->server.acks->num_acked = 0;
        int bytes;
        while ( ( bytes = rocketnet_network_simulator_receive( h->sim, &h->server_addr, time, &from, packet ) ) > 0 )
        {
            int response_bytes = 0;
            int payload_bytes = 0;
            int payload_client_index = -1;
            const int result = rocketnet_server_protocol_process_packet( &h->server, &from, packet, bytes,
                                                                          h->unix_ts, time,
                                                                          response, &response_bytes,
                                                                          payload, &payload_bytes, &payload_client_index );
            rocketnet_channel_endpoint_send_control( endpoint, response, response_bytes );
            if ( result == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE )
                h->server_connected = 1;
            if ( result == ROCKETNET_SERVER_PROCESS_ACCEPTED && payload_bytes > 0 )
                rocketnet_channel_endpoint_payload( endpoint, payload_client_index, payload, payload_bytes );
        }

        rocketnet_server_protocol_update( &h->server, time );
        for ( int i = 0; i < 4; i++ )
        {
            const int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &h->server, i, time, response );
            rocketnet_channel_endpoint_send_control( endpoint, response, keep_alive_bytes );
        }
        h->server_connected = rocketnet_client_slots_connected( h->server.client_slots, h->client_slot );
        return;
    }

    h->client.num_acked = 0;
    int bytes;
    while ( ( bytes = rocketnet_network_simulator_receive( h->sim, &h->client_addr, time, &from, packet ) ) > 0 )
    {
        int payload_bytes = 0;
        const int accepted = rocketnet_client_protocol_process_packet( &h->client, packet, bytes, time,
                                                                        payload, sizeof(payload), &payload_bytes );
        if ( accepted && payload_bytes > 0 )
            rocketnet_channel_endpoint_payload( endpoint, -1, payload, payload_bytes );
    }

    int packet_bytes = 0;
    rocketnet_client_protocol_update( &h->client, time, packet, &packet_bytes );
    rocketnet_channel_endpoint_send_control( endpoint, packet, packet_bytes );
}

static inline int rocketnet_channel_endpoint_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    *client_index = endpoint->is_server ? endpoint->payload_client_index : NULL;
    *payload_data = endpoint->payload_ptr;
    *payload_bytes = endpoint->payload_bytes;
    return endpoint->payload_count;
}

static inline int rocketnet_channel_endpoint_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    rocketnet_channel_harness_t * h = endpoint->harness;
    if ( endpoint->is_server )
    {
        *client_index = h->server.acks->acked_client_index;
        *packet_ids = h->server.acks->acked_packet_ids;
        return h->server.acks->num_acked;
    }
    *client_index = NULL;
    *packet_ids = h->client.acked_packet_ids;
    return h->client.num_acked;
}

static inline uint8_t * rocketnet_channel_endpoint_start_packet( void * transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    rocketnet_channel_harness_t * h = endpoint->harness;
    if ( endpoint->pending_frame >= 0 || endpoint->outgoing_count >= ROCKETNET_CHANNEL_HARNESS_MAX_PACKETS || request_bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return NULL;

    const int slot = endpoint->outgoing_count;
    uint8_t * wire_start = endpoint->frame[slot] + ROCKETNET_FRAME_WIRE_OFFSET;
    uint64_t sequence = 0;
    int started;
    if ( endpoint->is_server )
    {
        struct rocketnet_address_t to;
        started = rocketnet_server_protocol_start_payload( &h->server, client_index, &to, &sequence );
    }
    else
        started = rocketnet_client_protocol_start_payload( &h->client, &sequence );
    if ( !started )
        return NULL;

    memset( context, 0, sizeof(*context) );
    context->frame = wire_start;
    context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET;
    context->payload_capacity = ROCKETNET_MAX_PAYLOAD_BYTES;
    context->packet_id = ++h->next_packet_id;
    context->sequence = sequence;
    context->client_index = client_index;
    endpoint->pending_frame = slot;
    return wire_start;
}

static inline int rocketnet_channel_endpoint_finish_packet( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    rocketnet_channel_harness_t * h = endpoint->harness;
    if ( endpoint->pending_frame < 0 || context->frame != endpoint->frame[endpoint->pending_frame] + ROCKETNET_FRAME_WIRE_OFFSET )
        return 0;

    int wire_bytes;
    if ( endpoint->is_server )
        wire_bytes = rocketnet_server_protocol_seal_payload_frame( &h->server, context->client_index, context->sequence,
                                                                   context->packet_id, context->frame, payload_bytes, h->now );
    else
        wire_bytes = rocketnet_client_protocol_seal_payload_frame( &h->client, context->sequence, context->packet_id,
                                                                   context->frame, payload_bytes, h->now );
    if ( wire_bytes > 0 )
    {
        endpoint->frame_bytes[endpoint->pending_frame] = wire_bytes;
        endpoint->outgoing_count++;
    }
    endpoint->pending_frame = -1;
    return wire_bytes;
}

static inline void rocketnet_channel_endpoint_abort_packet( void * transport, struct rocketnet_packet_context_t * context )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    (void) context;
    endpoint->pending_frame = -1;
}

static inline void rocketnet_channel_endpoint_send_packets( void * transport )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    rocketnet_channel_harness_t * h = endpoint->harness;
    const struct rocketnet_address_t * from = endpoint->is_server ? &h->server_addr : &h->client_addr;
    const struct rocketnet_address_t * to = endpoint->is_server ? &h->client_addr : &h->server_addr;
    for ( int i = 0; i < endpoint->outgoing_count; i++ )
    {
        const int bytes = endpoint->frame_bytes[i];
        if ( endpoint->is_server )
            h->wire_bytes_to_client += bytes;
        else
            h->wire_bytes_to_server += bytes;
        rocketnet_network_simulator_send( h->sim, from, to,
                                          endpoint->frame[i] + ROCKETNET_FRAME_WIRE_OFFSET, bytes, h->now );
    }
    endpoint->outgoing_count = 0;
}

static inline uint64_t rocketnet_channel_endpoint_connection( void * transport, int client_index )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    rocketnet_channel_harness_t * h = endpoint->harness;
    if ( endpoint->is_server )
        return client_index == h->client_slot && h->server_connected ? 1 : 0;
    return h->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ? 1 : 0;
}

static inline int rocketnet_channel_endpoint_state( void * transport )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    return endpoint->is_server ? ( endpoint->harness->server_connected ? 1 : 0 ) : endpoint->harness->client.state;
}

static inline void rocketnet_channel_endpoint_disconnect( void * transport, int client_index )
{
    rocketnet_channel_endpoint_t * endpoint = (rocketnet_channel_endpoint_t *) transport;
    rocketnet_channel_harness_t * h = endpoint->harness;
    uint8_t packets[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_RECEIVE_CEILING];
    int packet_count = 0;
    int packet_bytes = 0;
    if ( endpoint->is_server )
    {
        rocketnet_server_protocol_disconnect_client( &h->server, client_index, packets, &packet_count, &packet_bytes );
        h->server_connected = 0;
    }
    else
    {
        rocketnet_client_protocol_disconnect( &h->client, packets, &packet_count, &packet_bytes );
        h->connected = 0;
    }
    const struct rocketnet_address_t * from = endpoint->is_server ? &h->server_addr : &h->client_addr;
    const struct rocketnet_address_t * to = endpoint->is_server ? &h->client_addr : &h->server_addr;
    for ( int i = 0; i < packet_count; i++ )
    {
        const uint8_t * packet = packets + (size_t) i * (size_t) packet_bytes;
        rocketnet_network_simulator_send( h->sim, from, to, packet, packet_bytes, h->now );
    }
}

static inline void rocketnet_channel_endpoint_link( struct rn_message_link_t * link, rocketnet_channel_endpoint_t * endpoint )
{
    memset( link, 0, sizeof(*link) );
    link->transport = endpoint;
    link->update = rocketnet_channel_endpoint_update;
    link->payloads = rocketnet_channel_endpoint_payloads;
    link->acked = rocketnet_channel_endpoint_acked;
    link->start_packet = rocketnet_channel_endpoint_start_packet;
    link->finish_packet = rocketnet_channel_endpoint_finish_packet;
    link->abort_packet = rocketnet_channel_endpoint_abort_packet;
    link->send_packets = rocketnet_channel_endpoint_send_packets;
    link->connection = rocketnet_channel_endpoint_connection;
    link->disconnect = rocketnet_channel_endpoint_disconnect;
    link->state = rocketnet_channel_endpoint_state;
    link->clock_from_first_update = 0;
}

static inline void rocketnet_channel_harness_destroy( rocketnet_channel_harness_t * h );

// The simulator seed. ROCKETNET_CHANNEL_SEED (decimal, or hex with 0x; strtoull
// base 0) picks it, so a nightly sweep walks many links; unset or empty is the
// pinned seed below, so a plain run is the same run every time. It is read once,
// at create. A case that exits through its check without reaching destroy gets
// one line from the exit hook, so every red run names the seed that replays it.
static const uint64_t rocketnet_channel_pinned_seed = 0x5EED5EED5EED5EEDULL;

struct rocketnet_channel_seed_state_t
{
    uint64_t seed;
    int live;
    int hook_installed;
};

inline rocketnet_channel_seed_state_t & rocketnet_channel_seed_state()
{
    static rocketnet_channel_seed_state_t state = { 0x5EED5EED5EED5EEDULL, 0, 0 };
    return state;
}

static inline uint64_t rocketnet_channel_seed_from_environment( void )
{
    const char * value = getenv( "ROCKETNET_CHANNEL_SEED" );
    if ( value && value[0] )
        return (uint64_t) strtoull( value, NULL, 0 );
    return rocketnet_channel_pinned_seed;
}

static inline void rocketnet_channel_seed_exit_hook( void )
{
    if ( rocketnet_channel_seed_state().live )
    {
        printf( "channel seed=0x%016llx (replay: ROCKETNET_CHANNEL_SEED=%llu)\n",
                (unsigned long long) rocketnet_channel_seed_state().seed,
                (unsigned long long) rocketnet_channel_seed_state().seed );
        fflush( stdout );
    }
}

// Create both ends over the simulator with a seed from ROCKETNET_CHANNEL_SEED.
// A non-NULL message_config replaces the default message config.
static inline int rocketnet_channel_harness_create_with_config( rocketnet_channel_harness_t * h, const struct rocketnet_message_config_t * message_config )
{
    struct rn_message_link_t client_link, server_link;
    struct rocketnet_server_protocol_t * server;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( h, 0, sizeof(*h) );
    h->now = 1000.0;
    h->unix_ts = 1700000000ULL;
    h->client_slot = 0;
    h->client_endpoint.harness = h;
    h->client_endpoint.pending_frame = -1;
    h->server_endpoint.harness = h;
    h->server_endpoint.is_server = 1;
    h->server_endpoint.pending_frame = -1;

    // Each functional case is a process. Install a counting allocator before
    // the SDK is initialized so object, protocol and simulator creates share
    // one count; the harness snapshots it after create and restores the SDK's
    // default allocator when it is destroyed.
    rocketnet_term();
    rocketnet_channel_allocator_calls = 0;
    if ( !rocketnet_allocator( rocketnet_channel_counting_malloc, rocketnet_channel_counting_free ) )
        return 0;
    if ( !rocketnet_init() )
    {
        rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function );
        rocketnet_init();
        return 0;
    }
    h->counting_allocator_installed = 1;

    // Keep the large fixture payload slots off the caller's stack. Both
    // endpoint buffers belong to this harness and are allocated before the
    // no-allocation snapshot used by the functional cases.
    h->client_endpoint.payload = (uint8_t (*)[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES])
        rocketnet_malloc( NULL, sizeof( *h->client_endpoint.payload ) * ROCKETNET_CHANNEL_HARNESS_MAX_ENDPOINT_PAYLOADS );
    h->server_endpoint.payload = (uint8_t (*)[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES])
        rocketnet_malloc( NULL, sizeof( *h->server_endpoint.payload ) * ROCKETNET_CHANNEL_HARNESS_MAX_ENDPOINT_PAYLOADS );
    if ( !h->client_endpoint.payload || !h->server_endpoint.payload )
        goto failed;

    // addresses: the client and the server on the simulated wire
    if ( !rocketnet_address_parse( &h->client_addr, "198.51.100.7:56789" ) )
        goto failed;
    if ( !rocketnet_address_parse( &h->server_addr, "203.0.113.10:40000" ) )
        goto failed;

    // the simulator: capacity 256, the seed from the environment (the pinned
    // seed when unset) so a failing run replays
    h->seed = rocketnet_channel_seed_from_environment();
    rocketnet_channel_seed_state().seed = h->seed;
    rocketnet_channel_seed_state().live = 1;
    if ( !rocketnet_channel_seed_state().hook_installed )
    {
        rocketnet_channel_seed_state().hook_installed = 1;
        atexit( rocketnet_channel_seed_exit_hook );
    }
    h->sim = rocketnet_network_simulator_create( NULL, 256, h->seed );
    if ( !h->sim )
        goto failed;

    // client protocol
    rocketnet_client_protocol_init( &h->client, &h->client_counters );

    if ( !rocketnet_write_dev_connect_token( "203.0.113.10:40000", 0xA11CE, h->unix_ts, token ) )
        goto failed;
    if ( !rocketnet_client_protocol_connect( &h->client, token, h->now, h->unix_ts ) )
        goto failed;

    // server protocol, configured with the development keys the token is
    // signed by (the test vectors are the dev keys)
    server = &h->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->filter_value, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server->advertised_ipv4 = h->server_addr;
    server->cookie_lifetime_seconds = 3;
    server->max_token_lifetime_seconds = 600;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->allow_negative_timeout = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->admitted_token_set = rocketnet_admitted_token_set_create( NULL, 16 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = &h->server_counters;
    if ( !server->encryption_map || !server->used_token_history || !server->admitted_token_set ||
         !server->client_slots || !server->acks || !server->reassembly )
        goto failed;
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, h->now );

    rocketnet_message_default_config( &h->message_config );
    if ( message_config )
        h->message_config = *message_config;
    if ( !rocketnet_channel_delivery_ring_init( &h->to_server, h->message_config.max_message_bytes ) ||
         !rocketnet_channel_delivery_ring_init( &h->to_client, h->message_config.max_message_bytes ) )
        goto failed;
    rocketnet_channel_endpoint_link( &client_link, &h->client_endpoint );
    rocketnet_channel_endpoint_link( &server_link, &h->server_endpoint );
    h->message_client = (struct rocketnet_message_client_t *) rn_message_object_create( NULL, &client_link, 0, 1,
                                                                                        &h->message_config, NULL, 0,
                                                                                        ROCKETNET_DEFAULT_CLIENT_SEND_KBPS, h->now );
    h->message_server = (struct rocketnet_message_server_t *) rn_message_object_create( NULL, &server_link, 1, 4,
                                                                                        &h->message_config, NULL, 0,
                                                                                        ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS, h->now );
    if ( !h->message_client || !h->message_server )
        goto failed;

    h->allocation_snapshot = rocketnet_channel_allocator_calls;

    return 1;

failed:
    rocketnet_channel_harness_destroy( h );
    return 0;
}

static inline int rocketnet_channel_harness_create( rocketnet_channel_harness_t * h )
{
    return rocketnet_channel_harness_create_with_config( h, NULL );
}

static inline int rocketnet_channel_harness_create_with_unreliable_deliveries( rocketnet_channel_harness_t * h, int deliveries )
{
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.unreliable_deliveries = deliveries;
    return rocketnet_channel_harness_create_with_config( h, &config );
}

// Set the link knobs the type cards use.
static inline void rocketnet_channel_harness_set_link( rocketnet_channel_harness_t * h, double latency, double jitter, float loss_percent, float duplicate_percent )
{
    h->latency = latency;
    h->jitter = jitter;
    h->packet_loss_percent = loss_percent;
    h->duplicate_percent = duplicate_percent;
    rocketnet_network_simulator_config( h->sim, latency, jitter, loss_percent, duplicate_percent );
}

static inline int rocketnet_channel_harness_set_budgets( rocketnet_channel_harness_t * h, uint32_t client_kbps, uint32_t server_kbps )
{
    return rocketnet_message_client_set_send_kbps( h->message_client, client_kbps ) &&
           rocketnet_message_server_set_send_kbps( h->message_server, h->client_slot, server_kbps );
}

// Advance both ends by dt seconds at the test clock and round-trip the wire.
// Bounded: returns 1 when connected, 0 after max_updates with no connection.
static inline int rocketnet_channel_harness_pump( rocketnet_channel_harness_t * h, double dt )
{
    h->now += dt;
    if ( rocketnet_message_client_update( h->message_client, h->now ) != ROCKETNET_MESSAGE_OK ||
         rocketnet_message_server_update( h->message_server, h->now ) != ROCKETNET_MESSAGE_OK )
        return 0;

    for ( int channel = 0; channel < h->message_config.num_channels; channel++ )
    {
        const uint8_t * data;
        int bytes;
        while ( rocketnet_message_server_receive( h->message_server, h->client_slot, channel, &data, &bytes ) )
        {
            rocketnet_channel_delivery_ring_push( h, &h->to_server, channel, data, bytes );
            rocketnet_message_server_release( h->message_server, h->client_slot, data );
        }
        while ( rocketnet_message_client_receive( h->message_client, channel, &data, &bytes ) )
        {
            rocketnet_channel_delivery_ring_push( h, &h->to_client, channel, data, bytes );
            rocketnet_message_client_release( h->message_client, data );
        }
    }

    rocketnet_message_client_send_packets( h->message_client );
    rocketnet_message_server_send_packets( h->message_server );
    h->connected = rocketnet_message_client_state( h->message_client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && h->server_connected;
    return h->connected;
}

static inline int rocketnet_channel_harness_connect( rocketnet_channel_harness_t * h, int max_updates )
{
    h->connected = 0;
    for ( int i = 0; i < max_updates && !h->connected; i++ )
        rocketnet_channel_harness_pump( h, 1.0 / 60.0 );
    return h->connected;
}

// Send bytes on a channel from the client (to_server) or the server (to_client).
static inline uint32_t rocketnet_channel_harness_client_send( rocketnet_channel_harness_t * h, int channel, int type_code, const uint8_t * data, int bytes )
{
    return rocketnet_message_client_send( h->message_client, channel, type_code, data, bytes );
}

static inline uint32_t rocketnet_channel_harness_server_send( rocketnet_channel_harness_t * h, int channel, int type_code, const uint8_t * data, int bytes )
{
    return rocketnet_message_server_send( h->message_server, h->client_slot, channel, type_code, data, bytes );
}

// Collect the next delivered message on a channel into out. Returns 1 when a
// message was delivered, 0 when none is pending.
static inline int rocketnet_channel_harness_server_collect( rocketnet_channel_harness_t * h, int channel, rocketnet_channel_delivery_t * out )
{
    return rocketnet_channel_delivery_ring_pop_channel( h, &h->to_server, channel, out );
}

static inline int rocketnet_channel_harness_client_collect( rocketnet_channel_harness_t * h, int channel, rocketnet_channel_delivery_t * out )
{
    return rocketnet_channel_delivery_ring_pop_channel( h, &h->to_client, channel, out );
}

// Refresh the measured counters from the protocol engines. The protocol
// layer has no message-level resend counter; the message objects' resend
// FIFO lives above it, so the smoke cases' unreliable traffic reports zero.
static inline void rocketnet_channel_harness_read_counters( rocketnet_channel_harness_t * h )
{
    h->resends = h->message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT] +
                 h->message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];
}

static inline void rocketnet_channel_harness_destroy( rocketnet_channel_harness_t * h )
{
    if ( h->message_client ) rocketnet_message_client_destroy( h->message_client );
    if ( h->message_server ) rocketnet_message_server_destroy( h->message_server );
    if ( h->server.encryption_map ) rocketnet_encryption_map_destroy( h->server.encryption_map );
    if ( h->server.used_token_history ) rocketnet_used_token_history_destroy( h->server.used_token_history );
    if ( h->server.admitted_token_set ) rocketnet_admitted_token_set_destroy( h->server.admitted_token_set );
    if ( h->server.client_slots ) rocketnet_client_slots_destroy( h->server.client_slots );
    if ( h->server.acks ) rocketnet_server_acks_destroy( h->server.acks );
    if ( h->server.reassembly ) rocketnet_reassembly_set_destroy( h->server.reassembly );
    if ( h->sim ) rocketnet_network_simulator_destroy( h->sim );
    if ( h->client_endpoint.payload ) rocketnet_free( NULL, h->client_endpoint.payload );
    if ( h->server_endpoint.payload ) rocketnet_free( NULL, h->server_endpoint.payload );
    rocketnet_channel_delivery_ring_destroy( &h->to_server );
    rocketnet_channel_delivery_ring_destroy( &h->to_client );
    if ( h->counting_allocator_installed )
    {
        rocketnet_term();
        rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function );
        rocketnet_init();
    }
    rocketnet_channel_seed_state().live = 0;
    memset( h, 0, sizeof(*h) );
}

#endif // ROCKETNET_CHANNEL_HARNESS_H

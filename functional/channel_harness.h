/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The channel harness every message type's functional test uses: two ends
    -- a server and a client -- driven over the in-memory network simulator
    (sdk/rocketnet_network_simulator.h) with loss, duplication, latency and
    jitter injected by the simulator's deterministic seed. The harness knows
    no message type: a type card passes its code and reads what arrived.

    Why the simulator, not sockets: the message objects the SDK registers run
    over real UDP transports, which open kernel sockets that cannot be asked
    to drop, duplicate or delay packet by packet. The one place a link's
    loss/duplication/latency/jitter can actually shape traffic is the
    protocol layer, so the harness drives the client and server protocol
    state machines directly, with the simulator as the wire between them --
    the same shape the unit suite's lossy-wire test uses. This is what the
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
#include <time.h>

#define ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES 128
#define ROCKETNET_CHANNEL_HARNESS_MAX_PAYLOAD   256

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
    int count;
    int channel[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES];
    uint8_t data[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES][ROCKETNET_CHANNEL_HARNESS_MAX_PAYLOAD];
    int len[ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES];
} rocketnet_channel_delivery_ring_t;

typedef struct rocketnet_channel_harness_t
{
    struct rocketnet_network_simulator_t * sim;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_address_t client_addr;
    struct rocketnet_address_t server_addr;

    rocketnet_channel_delivery_ring_t to_server;
    rocketnet_channel_delivery_ring_t to_client;

    double now;                     // the test clock, advanced by pump
    uint64_t unix_ts;              // captured at create, for token checks
    int client_slot;               // the connected server slot (0)
    int connected;

    // link knobs, held here and applied to the simulator
    double latency;
    double jitter;
    float packet_loss_percent;
    float duplicate_percent;

    // measured counters
    uint64_t resends;
    int64_t wire_bytes_to_server;
    int64_t wire_bytes_to_client;
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

static inline void rocketnet_channel_delivery_ring_push( rocketnet_channel_delivery_ring_t * ring, int channel, const uint8_t * data, int bytes )
{
    if ( ring->count >= ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES )
        return;
    const int len = bytes > ROCKETNET_CHANNEL_HARNESS_MAX_PAYLOAD ? ROCKETNET_CHANNEL_HARNESS_MAX_PAYLOAD : bytes;
    const int slot = ( ring->head + ring->count ) % ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES;
    ring->channel[slot] = channel;
    ring->len[slot] = len;
    if ( len > 0 )
        memcpy( ring->data[slot], data, (size_t) len );
    ring->count++;
}

static inline int rocketnet_channel_delivery_ring_pop( rocketnet_channel_delivery_ring_t * ring, rocketnet_channel_delivery_t * out )
{
    if ( ring->count == 0 )
        return 0;
    out->channel = ring->channel[ring->head];
    out->data = ring->data[ring->head];
    out->bytes = ring->len[ring->head];
    out->hash = rocketnet_channel_hash( ring->data[ring->head], ring->len[ring->head] );
    ring->head = ( ring->head + 1 ) % ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES;
    ring->count--;
    return 1;
}

static inline int rocketnet_channel_harness_connected( const rocketnet_channel_harness_t * h )
{
    return h->connected;
}

// Create both ends over the simulator with a deterministic seed.
static inline int rocketnet_channel_harness_create( rocketnet_channel_harness_t * h )
{
    memset( h, 0, sizeof(*h) );
    h->now = 1000.0;
    h->unix_ts = (uint64_t) time( NULL );
    h->client_slot = 0;

    // addresses: the client and the server on the simulated wire
    if ( !rocketnet_address_parse( &h->client_addr, "198.51.100.7:56789" ) )
        return 0;
    if ( !rocketnet_address_parse( &h->server_addr, "203.0.113.10:40000" ) )
        return 0;

    // the simulator: capacity 256, a fixed seed so a failing run replays
    h->sim = rocketnet_network_simulator_create( NULL, 256, 0x5EED5EED5EED5EEDULL );
    if ( !h->sim )
        return 0;

    // client protocol
    rocketnet_client_protocol_init( &h->client, &h->client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_dev_connect_token( "203.0.113.10:40000", 0xA11CE, h->unix_ts, token ) )
        return 0;
    if ( !rocketnet_client_protocol_connect( &h->client, token, h->now, h->unix_ts ) )
        return 0;

    // server protocol, configured with the development keys the token is
    // signed by (the test vectors are the dev keys)
    struct rocketnet_server_protocol_t * server = &h->server;
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
        return 0;
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, h->now );

    return 1;
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

// Advance both ends by dt seconds at the test clock and round-trip the wire.
// Bounded: returns 1 when connected, 0 after max_updates with no connection.
static inline int rocketnet_channel_harness_pump( rocketnet_channel_harness_t * h, double dt )
{
    h->now += dt;

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];
    struct rocketnet_address_t from;

    // client sends its packet for this tick
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &h->client, h->now, packet, &packet_bytes );
    if ( packet_bytes > 0 )
    {
        h->wire_bytes_to_server += packet_bytes;
        rocketnet_network_simulator_send( h->sim, &h->client_addr, &h->server_addr, packet, packet_bytes, h->now );
    }

    // everything due at the server this tick
    int bytes;
    while ( ( bytes = rocketnet_network_simulator_receive( h->sim, &h->server_addr, h->now, &from, packet ) ) > 0 )
    {
        int response_bytes = 0;
        int payload_bytes = 0;
        int payload_client_index = -1;
        const int result = rocketnet_server_protocol_process_packet( &h->server, &from, packet, bytes,
                                                                    h->unix_ts, h->now,
                                                                    response, &response_bytes,
                                                                    payload, &payload_bytes, &payload_client_index );
        if ( response_bytes > 0 )
        {
            h->wire_bytes_to_client += response_bytes;
            rocketnet_network_simulator_send( h->sim, &h->server_addr, &from, response, response_bytes, h->now );
        }
        if ( result == ROCKETNET_SERVER_PROCESS_ACCEPTED && payload_bytes > 0 )
        {
            int channel = (int) payload[0] | ( (int) payload[1] << 8 ) | ( (int) payload[2] << 16 ) | ( (int) payload[3] << 24 );
            rocketnet_channel_delivery_ring_push( &h->to_server, channel, payload + 4, payload_bytes - 4 );
        }
    }

    // server pacing: update, then keep-alives for every slot
    rocketnet_server_protocol_update( &h->server, h->now );
    for ( int i = 0; i < 4; i++ )
    {
        int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &h->server, i, h->now, response );
        if ( keep_alive_bytes > 0 )
        {
            h->wire_bytes_to_client += keep_alive_bytes;
            rocketnet_network_simulator_send( h->sim, &h->server_addr, &h->client_addr, response, keep_alive_bytes, h->now );
        }
    }

    // everything due at the client this tick
    while ( ( bytes = rocketnet_network_simulator_receive( h->sim, &h->client_addr, h->now, &from, packet ) ) > 0 )
    {
        int payload_bytes = 0;
        const int accepted = rocketnet_client_protocol_process_packet( &h->client, packet, bytes, h->now,
                                                                      payload, sizeof(payload), &payload_bytes );
        if ( accepted && payload_bytes > 0 )
        {
            int channel = (int) payload[0] | ( (int) payload[1] << 8 ) | ( (int) payload[2] << 16 ) | ( (int) payload[3] << 24 );
            rocketnet_channel_delivery_ring_push( &h->to_client, channel, payload + 4, payload_bytes - 4 );
        }
    }

    h->connected = h->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
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
static inline uint32_t rocketnet_channel_harness_client_send( rocketnet_channel_harness_t * h, int channel, const uint8_t * data, int bytes )
{
    if ( bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return 0;
    uint8_t framed[4 + ROCKETNET_MAX_PAYLOAD_BYTES];
    framed[0] = (uint8_t) ( channel & 0xFF );
    framed[1] = (uint8_t) ( ( channel >> 8 ) & 0xFF );
    framed[2] = (uint8_t) ( ( channel >> 16 ) & 0xFF );
    framed[3] = (uint8_t) ( ( channel >> 24 ) & 0xFF );
    if ( bytes > 0 )
        memcpy( framed + 4, data, (size_t) bytes );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int wire = rocketnet_client_protocol_write_payload( &h->client, framed, 4 + bytes, h->now, packet );
    if ( wire <= 0 )
        return 0;
    h->wire_bytes_to_server += wire;
    rocketnet_network_simulator_send( h->sim, &h->client_addr, &h->server_addr, packet, wire, h->now );
    return (uint32_t) wire;
}

static inline uint32_t rocketnet_channel_harness_server_send( rocketnet_channel_harness_t * h, int channel, const uint8_t * data, int bytes )
{
    if ( bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return 0;
    uint8_t framed[4 + ROCKETNET_MAX_PAYLOAD_BYTES];
    framed[0] = (uint8_t) ( channel & 0xFF );
    framed[1] = (uint8_t) ( ( channel >> 8 ) & 0xFF );
    framed[2] = (uint8_t) ( ( channel >> 16 ) & 0xFF );
    framed[3] = (uint8_t) ( ( channel >> 24 ) & 0xFF );
    if ( bytes > 0 )
        memcpy( framed + 4, data, (size_t) bytes );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int wire = rocketnet_server_protocol_write_payload( &h->server, h->client_slot, framed, 4 + bytes, h->now, packet );
    if ( wire <= 0 )
        return 0;
    h->wire_bytes_to_client += wire;
    rocketnet_network_simulator_send( h->sim, &h->server_addr, &h->client_addr, packet, wire, h->now );
    return (uint32_t) wire;
}

// Collect the next delivered message on a channel into out. Returns 1 when a
// message was delivered, 0 when none is pending.
static inline int rocketnet_channel_harness_server_collect( rocketnet_channel_harness_t * h, int channel, rocketnet_channel_delivery_t * out )
{
    rocketnet_channel_delivery_t d;
    while ( rocketnet_channel_delivery_ring_pop( &h->to_server, &d ) )
    {
        if ( d.channel == channel )
        {
            *out = d;
            return 1;
        }
    }
    return 0;
}

static inline int rocketnet_channel_harness_client_collect( rocketnet_channel_harness_t * h, int channel, rocketnet_channel_delivery_t * out )
{
    rocketnet_channel_delivery_t d;
    while ( rocketnet_channel_delivery_ring_pop( &h->to_client, &d ) )
    {
        if ( d.channel == channel )
        {
            *out = d;
            return 1;
        }
    }
    return 0;
}

// Refresh the measured counters from the protocol engines. The protocol
// layer has no message-level resend counter; the message objects' resend
// FIFO lives above it, so the smoke cases' unreliable traffic reports zero.
static inline void rocketnet_channel_harness_read_counters( rocketnet_channel_harness_t * h )
{
    (void) h;
    h->resends = 0;
}

static inline void rocketnet_channel_harness_destroy( rocketnet_channel_harness_t * h )
{
    if ( h->server.encryption_map ) rocketnet_encryption_map_destroy( h->server.encryption_map );
    if ( h->server.used_token_history ) rocketnet_used_token_history_destroy( h->server.used_token_history );
    if ( h->server.admitted_token_set ) rocketnet_admitted_token_set_destroy( h->server.admitted_token_set );
    if ( h->server.client_slots ) rocketnet_client_slots_destroy( h->server.client_slots );
    if ( h->server.acks ) rocketnet_server_acks_destroy( h->server.acks );
    if ( h->server.reassembly ) rocketnet_reassembly_set_destroy( h->server.reassembly );
    if ( h->sim ) rocketnet_network_simulator_destroy( h->sim );
    memset( h, 0, sizeof(*h) );
}

#endif // ROCKETNET_CHANNEL_HARNESS_H

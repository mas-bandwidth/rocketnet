/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The deterministic simulation harness core (docs/DST.md sections 1, 2 and
    6): the world lifted from soak_sim, the N-client rn_message_link_t seam,
    the named substreams, the payload scheme, the trace hash and the seeded
    single-process run. Time is an argument that starts at 1000.0 and is
    computed from the step; nothing here reads a clock, opens a socket or
    starts a thread.
*/

#ifndef ROCKETNET_DST_HARNESS_H
#define ROCKETNET_DST_HARNESS_H

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_message_object.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_ack.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_wire.h"
#include "rocketnet_fragment.h"
#if defined(ROCKETNET_USE_SYSTEM_SODIUM)
#include <sodium.h>
#else
#include "sodium/sodium.h"
#endif
#include "dst_checks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROCKETNET_DST_MAX_CLIENTS            16
#define ROCKETNET_DST_NUM_CHANNELS           9
#define ROCKETNET_DST_MAX_PACKETS            32
#define ROCKETNET_DST_MAX_PAYLOADS           512
#define ROCKETNET_DST_MAX_STREAMS            8

// The fault kinds. A kind that is off makes no draw.
#define ROCKETNET_DST_FAULT_LOSS             0x01
#define ROCKETNET_DST_FAULT_DUP              0x02
#define ROCKETNET_DST_FAULT_JITTER           0x04
#define ROCKETNET_DST_FAULT_REORDER          0x08
#define ROCKETNET_DST_FAULT_DELAY            0x10
#define ROCKETNET_DST_FAULT_SKEW             0x20
#define ROCKETNET_DST_FAULT_CRASH_CLIENT     0x40
#define ROCKETNET_DST_FAULT_CRASH_SERVER     0x80
#define ROCKETNET_DST_FAULT_ALL              0xFF

#define ROCKETNET_DST_SHAPE_FULL             0
#define ROCKETNET_DST_SHAPE_PUSH             1

// FNV-1a 64 over a byte range.
static inline uint64_t rocketnet_dst_fnv1a64( const uint8_t * data, size_t bytes )
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for ( size_t i = 0; i < bytes; i++ )
    {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// splitmix64's finaliser.
static inline uint64_t rocketnet_dst_splitmix64_finalise( uint64_t z )
{
    z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
    z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
    return z ^ ( z >> 31 );
}

// A substream: its own 64-bit state, drawn from the master seed and the
// substream's name.
struct rocketnet_dst_substream_t
{
    uint64_t state;
};

static inline void rocketnet_dst_substream_init( struct rocketnet_dst_substream_t * stream,
                                                 uint64_t master_seed, const char * name )
{
    const uint64_t name_hash = rocketnet_dst_fnv1a64( (const uint8_t*) name, strlen( name ) );
    stream->state = rocketnet_dst_splitmix64_finalise( master_seed ^ name_hash );
}

static inline uint64_t rocketnet_dst_substream_draw( struct rocketnet_dst_substream_t * stream )
{
    stream->state += 0x9E3779B97F4A7C15ULL;
    return rocketnet_dst_splitmix64_finalise( stream->state );
}

static inline uint64_t rocketnet_dst_substream_uniform( struct rocketnet_dst_substream_t * stream,
                                                        uint64_t lo, uint64_t hi )
{
    const uint64_t draw = rocketnet_dst_substream_draw( stream );
    return lo + draw % ( hi - lo + 1 );
}

static inline double rocketnet_dst_substream_real( struct rocketnet_dst_substream_t * stream )
{
    const uint64_t draw = rocketnet_dst_substream_draw( stream );
    return (double) ( draw >> 11 ) * ( 1.0 / 9007199254740992.0 );   // 2^-53, exact
}

// The trace accumulator: FNV-1a 64 over fixed-width records appended in step
// order.
struct rocketnet_dst_trace_t
{
    uint64_t hash;
};

static inline void rocketnet_dst_trace_init( struct rocketnet_dst_trace_t * trace )
{
    trace->hash = 0xcbf29ce484222325ULL;
}

static inline void rocketnet_dst_trace_u64( struct rocketnet_dst_trace_t * trace, uint64_t v )
{
    for ( int i = 0; i < 8; i++ )
    {
        trace->hash ^= (uint8_t) ( v >> ( 8 * i ) );
        trace->hash *= 0x100000001b3ULL;
    }
}

static inline void rocketnet_dst_trace_record( struct rocketnet_dst_trace_t * trace, uint64_t a, uint64_t b,
                                               uint64_t c, uint64_t d, uint64_t e )
{
    rocketnet_dst_trace_u64( trace, a );
    rocketnet_dst_trace_u64( trace, b );
    rocketnet_dst_trace_u64( trace, c );
    rocketnet_dst_trace_u64( trace, d );
    rocketnet_dst_trace_u64( trace, e );
}

struct rocketnet_dst_options_t
{
    uint64_t steps;             // total steps this seed runs
    uint64_t fault_steps;       // the quiet window starts at this step
    int shape;
    int clients;                // 0: draw from the shape's range
    uint32_t faults;
    int force_fail_step;        // a test's forced failure, -1 for none
};

struct rocketnet_dst_result_t
{
    int passed;
    uint64_t trace_hash;
    uint64_t seed;
    uint64_t step;
    char check[64];
    char detail[256];
};

struct rocketnet_dst_world_t;

typedef struct rocketnet_dst_endpoint_t
{
    struct rocketnet_dst_world_t * world;
    int is_server;
    int client_index;
    int pending_frame;
    uint64_t next_packet_id;
    int outgoing_count;
    uint8_t frame[ROCKETNET_DST_MAX_PACKETS][ROCKETNET_FRAME_STRIDE];
    int frame_bytes[ROCKETNET_DST_MAX_PACKETS];
    int frame_peer[ROCKETNET_DST_MAX_PACKETS];
    uint8_t (* payload)[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    uint8_t * payload_ptr[ROCKETNET_DST_MAX_PAYLOADS];
    int payload_bytes[ROCKETNET_DST_MAX_PAYLOADS];
    int payload_client_index[ROCKETNET_DST_MAX_PAYLOADS];
    int payload_count;
} rocketnet_dst_endpoint_t;

typedef struct rocketnet_dst_client_t
{
    struct rocketnet_client_protocol_t protocol;
    rocketnet_dst_endpoint_t endpoint;
    struct rocketnet_message_client_t * message;
    struct rocketnet_address_t address;
    struct rocketnet_dst_substream_t schedule;
    struct rocketnet_dst_substream_t traffic;
    uint64_t generation;
    int next_connect_step;
    int churn_step;
    int pause_until;
    int last_state;
    int last_server_slot;
    uint32_t sequence[ROCKETNET_DST_MAX_STREAMS][2];
    uint32_t epoch[2];
    unsigned mixed[2];
} rocketnet_dst_client_t;

typedef struct rocketnet_dst_world_t
{
    struct rocketnet_network_simulator_t * sim;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_server_protocol_t server;
    rocketnet_dst_endpoint_t server_endpoint;
    struct rocketnet_message_server_t * message_server;
    struct rocketnet_message_config_t message_config;
    struct rocketnet_address_t server_address;
    rocketnet_dst_client_t client[ROCKETNET_DST_MAX_CLIENTS];
    int num_clients;
    uint64_t step;
    double now;
    uint64_t unix_ts;
    struct rocketnet_dst_trace_t trace;
    uint64_t seed;
    struct rocketnet_dst_substream_t traffic_server;
    struct rocketnet_dst_substream_t link;
    struct rocketnet_dst_substream_t fault_loss;
    struct rocketnet_dst_substream_t fault_dup;
    struct rocketnet_dst_substream_t fault_jitter;
    uint32_t faults;
    double base_latency;
    double base_jitter;
    float loss_percent;
    float dup_percent;
    int quiet_applied;
    int server_incarnation;
    uint64_t fault_steps;
    struct rocketnet_dst_check_state_t check;
    int failed;
    uint64_t fail_step;
    char fail_check[64];
    char fail_detail[256];
} rocketnet_dst_world_t;

static inline void rocketnet_dst_fail( rocketnet_dst_world_t * w, const char * check, const char * detail )
{
    if ( w->failed )
        return;
    w->failed = 1;
    w->fail_step = w->step;
    snprintf( w->fail_check, sizeof( w->fail_check ), "%s", check );
    snprintf( w->fail_detail, sizeof( w->fail_detail ), "%s", detail );
}

static inline void rocketnet_dst_world_trace_send( rocketnet_dst_world_t * w, int sender, int receiver,
                                                   const uint8_t * data, int bytes )
{
    rocketnet_dst_trace_u64( &w->trace, w->step );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) sender );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) receiver );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) bytes );
    rocketnet_dst_trace_u64( &w->trace, bytes > 0 ? data[0] : 0 );
}

static inline void rocketnet_dst_endpoint_reset_payloads( rocketnet_dst_endpoint_t * endpoint )
{
    endpoint->payload_count = 0;
}

static inline void rocketnet_dst_endpoint_payload( rocketnet_dst_endpoint_t * endpoint, int client_index,
                                                   const uint8_t * data, int bytes )
{
    if ( endpoint->payload_count >= ROCKETNET_DST_MAX_PAYLOADS || bytes <= 0 ||
         bytes > ROCKETNET_RECEIVE_PAYLOAD_BYTES )
    {
        rocketnet_dst_fail( endpoint->world, "payload_ring", "the delivery ring is full" );
        return;
    }
    const int index = endpoint->payload_count++;
    memcpy( endpoint->payload[index], data, (size_t) bytes );
    endpoint->payload_ptr[index] = endpoint->payload[index];
    endpoint->payload_bytes[index] = bytes;
    endpoint->payload_client_index[index] = client_index;
}

static inline int rocketnet_dst_world_client_for_slot( rocketnet_dst_world_t * w, int slot )
{
    if ( !rocketnet_client_slots_connected( w->server.client_slots, slot ) )
        return -1;
    const uint64_t id = rocketnet_client_slots_client_id( w->server.client_slots, slot );
    return (int) ( id & 0xFF );
}

static inline int rocketnet_dst_world_client_for_address( rocketnet_dst_world_t * w,
                                                          const struct rocketnet_address_t * address )
{
    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( rocketnet_address_equal( &w->client[i].address, address ) )
            return i;
    }
    return -1;
}

static inline void rocketnet_dst_endpoint_send_control( rocketnet_dst_endpoint_t * endpoint, int peer,
                                                        const uint8_t * data, int bytes,
                                                        const struct rocketnet_address_t * to )
{
    if ( bytes <= 0 )
        return;
    rocketnet_dst_world_t * w = endpoint->world;
    const struct rocketnet_address_t * from = endpoint->is_server ? &w->server_address
                                                                  : &w->client[endpoint->client_index].address;
    if ( !to )
        to = &w->server_address;
    rocketnet_dst_world_trace_send( w, endpoint->is_server ? 0 : endpoint->client_index + 1, peer, data, bytes );
    rocketnet_network_simulator_send( w->sim, from, to, data, bytes, w->now );
}

static inline void rocketnet_dst_endpoint_update( void * transport, double time )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    rocketnet_dst_endpoint_reset_payloads( endpoint );
    struct rocketnet_address_t from;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];

    if ( endpoint->is_server )
    {
        w->server.acks->num_acked = 0;
        int bytes;
        while ( ( bytes = rocketnet_network_simulator_receive( w->sim, &w->server_address, time, &from, packet ) ) > 0 )
        {
            int response_bytes = 0;
            int payload_bytes = 0;
            int payload_client_index = -1;
            const int result = rocketnet_server_protocol_process_packet( &w->server, &from, packet, bytes,
                                                                          w->unix_ts, time,
                                                                          response, &response_bytes,
                                                                          payload, &payload_bytes,
                                                                          &payload_client_index );
            rocketnet_dst_endpoint_send_control( endpoint, rocketnet_dst_world_client_for_address( w, &from ) + 1,
                                                 response, response_bytes, &from );
            if ( result == ROCKETNET_SERVER_PROCESS_ACCEPTED && payload_bytes > 0 )
                rocketnet_dst_endpoint_payload( endpoint, payload_client_index, payload, payload_bytes );
        }

        rocketnet_server_protocol_update( &w->server, time );
        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            const int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &w->server, slot, time, response );
            if ( keep_alive_bytes > 0 )
            {
                const int mapping_index = rocketnet_client_slots_mapping_index( w->server.client_slots, slot );
                const struct rocketnet_address_t * to = rocketnet_encryption_map_address( w->server.encryption_map, mapping_index );
                if ( to )
                    rocketnet_dst_endpoint_send_control( endpoint, rocketnet_dst_world_client_for_slot( w, slot ) + 1,
                                                         response, keep_alive_bytes, to );
            }
        }
        return;
    }

    w->client[endpoint->client_index].protocol.num_acked = 0;
    int bytes;
    while ( ( bytes = rocketnet_network_simulator_receive( w->sim, &w->client[endpoint->client_index].address, time, &from, packet ) ) > 0 )
    {
        int payload_bytes = 0;
        const int accepted = rocketnet_client_protocol_process_packet( &w->client[endpoint->client_index].protocol,
                                                                       packet, bytes, time,
                                                                       payload, sizeof(payload), &payload_bytes );
        if ( accepted && payload_bytes > 0 )
            rocketnet_dst_endpoint_payload( endpoint, -1, payload, payload_bytes );
    }

    int packet_bytes = 0;
    rocketnet_client_protocol_update( &w->client[endpoint->client_index].protocol, time, packet, &packet_bytes );
    rocketnet_dst_endpoint_send_control( endpoint, 0, packet, packet_bytes, &w->server_address );
}

static inline int rocketnet_dst_endpoint_payloads( void * transport, const int ** client_index,
                                                   uint8_t * const ** payload_data, const int ** payload_bytes )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    *client_index = endpoint->is_server ? endpoint->payload_client_index : NULL;
    *payload_data = endpoint->payload_ptr;
    *payload_bytes = endpoint->payload_bytes;
    return endpoint->payload_count;
}

static inline int rocketnet_dst_endpoint_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( endpoint->is_server )
    {
        *client_index = w->server.acks->acked_client_index;
        *packet_ids = w->server.acks->acked_packet_ids;
        for ( int i = 0; i < w->server.acks->num_acked; i++ )
            rocketnet_dst_check_ack_id( &w->check, w->server.acks->acked_packet_ids[i] );
        return w->server.acks->num_acked;
    }
    *client_index = NULL;
    *packet_ids = w->client[endpoint->client_index].protocol.acked_packet_ids;
    for ( int i = 0; i < w->client[endpoint->client_index].protocol.num_acked; i++ )
        rocketnet_dst_check_ack_id( &w->check, w->client[endpoint->client_index].protocol.acked_packet_ids[i] );
    return w->client[endpoint->client_index].protocol.num_acked;
}

static inline uint8_t * rocketnet_dst_endpoint_start_packet( void * transport, int client_index, int request_bytes,
                                                             struct rocketnet_packet_context_t * context )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    (void) request_bytes;
    if ( endpoint->pending_frame >= 0 || endpoint->outgoing_count >= ROCKETNET_DST_MAX_PACKETS )
        return NULL;

    const int slot = endpoint->outgoing_count;
    uint8_t * wire_start = endpoint->frame[slot] + ROCKETNET_FRAME_WIRE_OFFSET;
    uint64_t sequence = 0;
    int peer = 0;
    if ( endpoint->is_server )
    {
        struct rocketnet_address_t to;
        if ( !rocketnet_server_protocol_start_payload( &w->server, client_index, &to, &sequence ) )
            return NULL;
        peer = rocketnet_dst_world_client_for_address( w, &to ) + 1;
    }
    else
    {
        if ( !rocketnet_client_protocol_start_payload( &w->client[endpoint->client_index].protocol, &sequence ) )
            return NULL;
    }

    memset( context, 0, sizeof(*context) );
    context->frame = wire_start;
    context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET;
    context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    context->packet_id = ++endpoint->next_packet_id;
    context->sequence = sequence;
    context->client_index = client_index;
    endpoint->pending_frame = slot;
    endpoint->frame_peer[slot] = peer;
    return wire_start;
}

static inline int rocketnet_dst_endpoint_finish_packet( void * transport, struct rocketnet_packet_context_t * context,
                                                        int payload_bytes )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( endpoint->pending_frame < 0 ||
         context->frame != endpoint->frame[endpoint->pending_frame] + ROCKETNET_FRAME_WIRE_OFFSET )
        return 0;

    int wire_bytes;
    if ( endpoint->is_server )
        wire_bytes = rocketnet_server_protocol_seal_payload_frame( &w->server, context->client_index,
                                                                   context->sequence, context->packet_id,
                                                                   context->frame, payload_bytes, w->now );
    else
        wire_bytes = rocketnet_client_protocol_seal_payload_frame( &w->client[endpoint->client_index].protocol,
                                                                   context->sequence, context->packet_id,
                                                                   context->frame, payload_bytes, w->now );
    if ( wire_bytes > 0 )
    {
        endpoint->frame_bytes[endpoint->pending_frame] = wire_bytes;
        endpoint->outgoing_count++;
        rocketnet_dst_check_issued( &w->check, context->packet_id );
    }
    endpoint->pending_frame = -1;
    return wire_bytes;
}

static inline void rocketnet_dst_endpoint_abort_packet( void * transport, struct rocketnet_packet_context_t * context )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    (void) context;
    endpoint->pending_frame = -1;
}

static inline void rocketnet_dst_endpoint_send_packets( void * transport )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    const struct rocketnet_address_t * from = endpoint->is_server ? &w->server_address
                                                                  : &w->client[endpoint->client_index].address;
    for ( int i = 0; i < endpoint->outgoing_count; i++ )
    {
        const int bytes = endpoint->frame_bytes[i];
        const int peer = endpoint->frame_peer[i];
        const struct rocketnet_address_t * to;
        if ( endpoint->is_server )
        {
            const int client_index = peer - 1;
            if ( client_index < 0 || client_index >= w->num_clients )
                continue;
            to = &w->client[client_index].address;
        }
        else
            to = &w->server_address;
        rocketnet_dst_world_trace_send( w, endpoint->is_server ? 0 : endpoint->client_index + 1, peer,
                                        endpoint->frame[i] + ROCKETNET_FRAME_WIRE_OFFSET, bytes );
        rocketnet_network_simulator_send( w->sim, from, to,
                                          endpoint->frame[i] + ROCKETNET_FRAME_WIRE_OFFSET, bytes, w->now );
    }
    endpoint->outgoing_count = 0;
}

static inline uint64_t rocketnet_dst_endpoint_connection( void * transport, int client_index )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( endpoint->is_server )
    {
        if ( !rocketnet_client_slots_connected( w->server.client_slots, client_index ) )
            return 0;
        return rocketnet_client_slots_client_id( w->server.client_slots, client_index );
    }
    if ( w->client[endpoint->client_index].protocol.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return 0;
    return w->client[endpoint->client_index].generation;
}

static inline int rocketnet_dst_endpoint_state( void * transport )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( endpoint->is_server )
        return 1;
    return w->client[endpoint->client_index].protocol.state;
}

static inline void rocketnet_dst_endpoint_disconnect( void * transport, int client_index )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    uint8_t packets[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int packet_count = 0;
    int packet_bytes = 0;
    if ( endpoint->is_server )
        rocketnet_server_protocol_disconnect_client( &w->server, client_index, packets, &packet_count, &packet_bytes );
    else
        rocketnet_client_protocol_disconnect( &w->client[endpoint->client_index].protocol, packets, &packet_count, &packet_bytes );
    const struct rocketnet_address_t * from = endpoint->is_server ? &w->server_address
                                                                  : &w->client[endpoint->client_index].address;
    for ( int i = 0; i < packet_count; i++ )
    {
        const uint8_t * packet = packets + (size_t) i * (size_t) ROCKETNET_SEND_BUDGET;
        const int peer = endpoint->is_server ? client_index + 1 : 0;
        const struct rocketnet_address_t * to = endpoint->is_server
            ? ( client_index >= 0 && client_index < w->num_clients ? &w->client[client_index].address : &w->server_address )
            : &w->server_address;
        rocketnet_dst_world_trace_send( w, endpoint->is_server ? 0 : endpoint->client_index + 1, peer, packet, packet_bytes );
        rocketnet_network_simulator_send( w->sim, from, to, packet, packet_bytes, w->now );
    }
}

static inline void rocketnet_dst_endpoint_link( struct rn_message_link_t * link, rocketnet_dst_endpoint_t * endpoint )
{
    memset( link, 0, sizeof(*link) );
    link->transport = endpoint;
    link->update = rocketnet_dst_endpoint_update;
    link->payloads = rocketnet_dst_endpoint_payloads;
    link->acked = rocketnet_dst_endpoint_acked;
    link->start_packet = rocketnet_dst_endpoint_start_packet;
    link->finish_packet = rocketnet_dst_endpoint_finish_packet;
    link->abort_packet = rocketnet_dst_endpoint_abort_packet;
    link->send_packets = rocketnet_dst_endpoint_send_packets;
    link->connection = rocketnet_dst_endpoint_connection;
    link->disconnect = rocketnet_dst_endpoint_disconnect;
    link->state = rocketnet_dst_endpoint_state;
    link->clock_from_first_update = 0;
}

static inline int rocketnet_dst_payload_build( uint8_t * buffer, int size, uint32_t sequence, int direction,
                                               int type, int channel, uint32_t generation, int incarnation,
                                               uint32_t epoch )
{
    if ( size < 14 )
        return 0;
    buffer[0] = (uint8_t) ( sequence );
    buffer[1] = (uint8_t) ( sequence >> 8 );
    buffer[2] = (uint8_t) ( sequence >> 16 );
    buffer[3] = (uint8_t) ( sequence >> 24 );
    buffer[4] = (uint8_t) ( 0xA0 | ( direction << 3 ) | ( type & 7 ) );
    buffer[5] = (uint8_t) channel;
    buffer[6] = (uint8_t) ( generation );
    buffer[7] = (uint8_t) ( generation >> 8 );
    buffer[8] = (uint8_t) ( generation >> 16 );
    buffer[9] = (uint8_t) incarnation;
    buffer[10] = (uint8_t) ( epoch );
    buffer[11] = (uint8_t) ( epoch >> 8 );
    buffer[12] = (uint8_t) ( epoch >> 16 );
    buffer[13] = (uint8_t) ( epoch >> 24 );
    for ( int i = 14; i < size; i++ )
        buffer[i] = (uint8_t) ( ( i + sequence ) % 256 );
    return 1;
}

static inline int rocketnet_dst_payload_verify( const uint8_t * buffer, int bytes, uint32_t * sequence,
                                                int * type, int * channel, uint32_t * generation )
{
    if ( bytes < 14 )
        return 0;
    if ( ( buffer[4] & 0xF0 ) != 0xA0 )
        return 0;
    *sequence = (uint32_t) buffer[0] | ( (uint32_t) buffer[1] << 8 ) |
                ( (uint32_t) buffer[2] << 16 ) | ( (uint32_t) buffer[3] << 24 );
    *type = buffer[4] & 7;
    *channel = buffer[5];
    *generation = (uint32_t) buffer[6] | ( (uint32_t) buffer[7] << 8 ) | ( (uint32_t) buffer[8] << 16 );
    if ( *channel >= ROCKETNET_DST_NUM_CHANNELS )
        return 0;
    for ( int i = 14; i < bytes; i++ )
    {
        if ( buffer[i] != (uint8_t) ( ( i + *sequence ) % 256 ) )
            return 0;
    }
    return 1;
}

static inline int rocketnet_dst_draw_size( struct rocketnet_dst_substream_t * traffic, int type )
{
    const double band = rocketnet_dst_substream_real( traffic );
    int lo, hi;
    if ( band < 0.70 )
    {
        lo = 14;
        hi = 256;
    }
    else if ( band < 0.95 )
    {
        lo = 257;
        hi = 4000;
    }
    else
    {
        lo = 4001;
        hi = 16384;
    }
    if ( type == 0 || type == 1 || type == 5 )
    {
        if ( hi > 1400 )
            hi = 1400;
        if ( lo > hi )
            lo = hi;
    }
    return (int) rocketnet_dst_substream_uniform( traffic, (uint64_t) lo, (uint64_t) hi );
}

static inline int rocketnet_dst_channel_type( rocketnet_dst_client_t * client, int channel, int direction )
{
    if ( channel < 8 )
        return channel;
    const unsigned pick = client->mixed[direction]++ % 3;
    return pick == 0 ? 3 : ( pick == 1 ? 4 : 1 );
}

static inline uint32_t rocketnet_dst_next_sequence( rocketnet_dst_client_t * client, int type, int direction )
{
    return client->sequence[type & 7][direction]++;
}

// A new session gets a fresh message object, as section 3's restart does:
// nothing of the old session survives into the new one.
static inline int rocketnet_dst_client_message_build( rocketnet_dst_world_t * w, int index )
{
    rocketnet_dst_client_t * client = &w->client[index];
    if ( client->message )
        rocketnet_message_client_destroy( client->message );
    client->endpoint.pending_frame = -1;
    client->endpoint.outgoing_count = 0;
    client->endpoint.next_packet_id = 0;
    struct rn_message_link_t client_link;
    rocketnet_dst_endpoint_link( &client_link, &client->endpoint );
    client->message = (struct rocketnet_message_client_t *) rn_message_object_create(
        NULL, &client_link, 0, 1, &w->message_config, NULL, 0,
        ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, w->now );
    return client->message != NULL;
}

static inline void rocketnet_dst_connect_client( rocketnet_dst_world_t * w, int index )
{
    rocketnet_dst_client_t * client = &w->client[index];
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    memset( &private_fields, 0, sizeof(private_fields) );

    client->generation++;
    rocketnet_dst_check_generation( &w->check, index, client->generation );
    if ( client->generation > 1 && !rocketnet_dst_client_message_build( w, index ) )
    {
        rocketnet_dst_fail( w, "message_object", "the restarted message object was refused" );
        return;
    }
    const uint64_t client_id = client->generation * 256ULL + (uint64_t) index;
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = w->unix_ts;
    signed_fields.expire_timestamp = w->unix_ts + 300;
    signed_fields.timeout_seconds = 5;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address_bytes[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address_bytes, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );
    private_fields.client_id = client_id;
    randombytes_buf( private_fields.client_to_server_key, 32 );
    randombytes_buf( private_fields.server_to_client_key, 32 );
    if ( !rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token ) )
    {
        rocketnet_dst_fail( w, "connect_token", "the token was refused" );
        return;
    }
    rocketnet_client_protocol_connect( &client->protocol, token, w->now, w->unix_ts );
    for ( int t = 0; t < ROCKETNET_DST_MAX_STREAMS; t++ )
    {
        client->sequence[t][0] = 0;
        client->sequence[t][1] = 0;
    }
    client->epoch[0] = 0;
    client->epoch[1] = 0;
    client->mixed[0] = 0;
    client->mixed[1] = 0;
}

static inline void rocketnet_dst_send_one( rocketnet_dst_world_t * w, int index, int direction, int slot )
{
    rocketnet_dst_client_t * client = &w->client[index];
    struct rocketnet_dst_substream_t * traffic = direction == 0 ? &client->traffic : &w->traffic_server;
    if ( rocketnet_dst_substream_real( traffic ) >= 0.5 )
        return;

    const int channel = (int) rocketnet_dst_substream_uniform( traffic, 0, ROCKETNET_DST_NUM_CHANNELS - 1 );
    const int type = rocketnet_dst_channel_type( client, channel, direction );
    const int size = rocketnet_dst_draw_size( traffic, type );
    int session = 16 + index;
    if ( direction == 0 )
    {
        session = -1;
        for ( int s = 0; s < ROCKETNET_DST_MAX_CLIENTS; s++ )
        {
            if ( rocketnet_client_slots_connected( w->server.client_slots, s ) &&
                 (int) ( rocketnet_client_slots_client_id( w->server.client_slots, s ) & 0xFF ) == index )
            {
                session = s;
                break;
            }
        }
        if ( session < 0 )
            return;
    }
    const int reliable = ( type == 2 || type == 3 || type == 4 || type == 6 || type == 7 );
    if ( reliable && rocketnet_dst_check_outstanding( &w->check, session ) > 65536 )
        return;
    uint32_t epoch = 0;
    if ( channel == 8 && type == 3 )
        epoch = client->epoch[direction]++;
    const uint32_t sequence = rocketnet_dst_next_sequence( client, type, direction );
    const uint32_t generation = (uint32_t) client->generation;
    uint8_t payload[16384];
    if ( size > (int) sizeof(payload) )
        return;
    if ( !rocketnet_dst_payload_build( payload, size, sequence, direction, type, channel,
                                       generation, w->server_incarnation, epoch ) )
        return;
    int sent = 0;
    if ( direction == 0 )
        sent = rocketnet_message_client_send( client->message, channel, type, payload, size );
    else
        sent = rocketnet_message_server_send( w->message_server, slot, channel, type, payload, size );
    if ( sent )
    {
        const uint64_t identity = direction == 0
            ? ( (uint64_t) generation * 256ULL + (uint64_t) index ) : (uint64_t) generation;
        rocketnet_dst_check_sent( &w->check, w->now, session, channel, type, sequence, generation,
                                  identity, epoch, size );
    }
    else
    {
        // A refused send is not sent: it leaves no gap in the stream.
        if ( client->sequence[type & 7][direction] > 0 )
            client->sequence[type & 7][direction]--;
        if ( channel == 8 && type == 3 && client->epoch[direction] > 0 )
            client->epoch[direction]--;
    }
}

static inline void rocketnet_dst_check_deliveries( rocketnet_dst_world_t * w, int session,
                                                   uint32_t expected_generation, int expected_incarnation,
                                                   uint64_t identity, const uint8_t * data, int bytes )
{
    uint32_t sequence;
    uint32_t generation;
    uint32_t epoch = 0;
    int incarnation = 0;
    int type;
    int channel;
    if ( !rocketnet_dst_payload_verify( data, bytes, &sequence, &type, &channel, &generation ) )
    {
        char detail[256];
        int mismatch = -1;
        if ( bytes >= 14 )
        {
            const uint32_t seq = (uint32_t) data[0] | ( (uint32_t) data[1] << 8 ) |
                                 ( (uint32_t) data[2] << 16 ) | ( (uint32_t) data[3] << 24 );
            for ( int i = 14; i < bytes; i++ )
            {
                if ( data[i] != (uint8_t) ( ( i + seq ) % 256 ) )
                {
                    mismatch = i;
                    break;
                }
            }
        }
        snprintf( detail, sizeof(detail), "session=%d bytes=%d b4=%02x b5=%02x at=%d",
                  session, bytes, bytes > 4 ? data[4] : 0, bytes > 5 ? data[5] : 0, mismatch );
        rocketnet_dst_fail( w, "payload_pattern", detail );
        return;
    }
    incarnation = data[9];
    epoch = (uint32_t) data[10] | ( (uint32_t) data[11] << 8 ) | ( (uint32_t) data[12] << 16 ) |
            ( (uint32_t) data[13] << 24 );
    rocketnet_dst_trace_u64( &w->trace, w->step );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) session );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) channel );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) type );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) sequence );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) bytes );

    rocketnet_dst_check_delivery( &w->check, w->now, session, channel, type, sequence, generation,
                                  incarnation, identity, epoch, bytes, expected_generation,
                                  expected_incarnation );
}

static inline void rocketnet_dst_world_draw( rocketnet_dst_world_t * w, uint64_t seed,
                                             const struct rocketnet_dst_options_t * options )
{
    rocketnet_dst_substream_init( &w->link, seed, "link" );
    rocketnet_dst_substream_init( &w->fault_loss, seed, "fault.loss" );
    rocketnet_dst_substream_init( &w->fault_dup, seed, "fault.dup" );
    rocketnet_dst_substream_init( &w->fault_jitter, seed, "fault.jitter" );
    rocketnet_dst_substream_init( &w->traffic_server, seed, "traffic.server" );
    struct rocketnet_dst_substream_t schedule;
    rocketnet_dst_substream_init( &schedule, seed, "schedule" );

    w->base_latency = 0.005 + rocketnet_dst_substream_real( &w->link ) * 0.145;
    if ( options->faults & ROCKETNET_DST_FAULT_JITTER )
        w->base_jitter = rocketnet_dst_substream_real( &w->fault_jitter ) * ( w->base_latency * 0.5 );
    else
        w->base_jitter = 0.0;

    if ( options->faults & ROCKETNET_DST_FAULT_LOSS )
    {
        static const int rates[] = { 0, 1, 2, 5, 10, 20 };
        static const int weights[] = { 10, 30, 30, 15, 10, 5 };
        const int roll = (int) rocketnet_dst_substream_uniform( &w->fault_loss, 0, 99 );
        int total = 0;
        int chosen = 0;
        for ( int i = 0; i < 6; i++ )
        {
            total += weights[i];
            if ( roll < total )
            {
                chosen = rates[i];
                break;
            }
        }
        w->loss_percent = (float) chosen;
    }
    else
        w->loss_percent = 0.0f;

    if ( options->faults & ROCKETNET_DST_FAULT_DUP )
    {
        static const int rates[] = { 0, 1, 2, 5 };
        static const int weights[] = { 40, 30, 20, 10 };
        const int roll = (int) rocketnet_dst_substream_uniform( &w->fault_dup, 0, 99 );
        int total = 0;
        int chosen = 0;
        for ( int i = 0; i < 4; i++ )
        {
            total += weights[i];
            if ( roll < total )
            {
                chosen = rates[i];
                break;
            }
        }
        w->dup_percent = (float) chosen;
    }
    else
        w->dup_percent = 0.0f;

    int lo = 2, hi = 8;
    if ( options->shape == ROCKETNET_DST_SHAPE_PUSH )
    {
        lo = 2;
        hi = 4;
    }
    int count = (int) rocketnet_dst_substream_uniform( &schedule, (uint64_t) lo, (uint64_t) hi );
    if ( options->clients > 0 )
        count = options->clients;
    w->num_clients = count;

    for ( int i = 0; i < w->num_clients; i++ )
    {
        char name[32];
        snprintf( name, sizeof(name), "schedule.c%d", i );
        rocketnet_dst_substream_init( &w->client[i].schedule, seed, name );
        snprintf( name, sizeof(name), "traffic.c%d", i );
        rocketnet_dst_substream_init( &w->client[i].traffic, seed, name );
        w->client[i].next_connect_step = (int) rocketnet_dst_substream_uniform( &w->client[i].schedule, 0, 99 );
        const int runs = rocketnet_dst_substream_real( &w->client[i].schedule ) < 0.2 ? 1 : 0;
        w->client[i].churn_step = runs ? (int) rocketnet_dst_substream_uniform( &w->client[i].schedule, 0,
                                                                                options->fault_steps > 1 ? options->fault_steps - 1 : 0 ) : -1;
        w->client[i].generation = 0;
        w->client[i].pause_until = -1;
        w->client[i].last_state = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
        w->client[i].last_server_slot = -1;
    }
}

static inline int rocketnet_dst_world_create( rocketnet_dst_world_t * w, uint64_t seed,
                                              const struct rocketnet_dst_options_t * options )
{
    memset( w, 0, sizeof(*w) );
    w->seed = seed;
    w->faults = options->faults;
    w->fault_steps = options->fault_steps;
    w->now = 1000.0;
    w->unix_ts = 1700000000ULL;
    w->server_incarnation = 1;
    rocketnet_dst_trace_init( &w->trace );
    rocketnet_dst_world_draw( w, seed, options );

    struct rocketnet_dst_substream_t wire;
    rocketnet_dst_substream_init( &wire, seed, "wire" );
    const uint64_t simulator_seed = rocketnet_dst_substream_draw( &wire );
    w->sim = rocketnet_network_simulator_create( NULL, 4096, simulator_seed );
    if ( !w->sim )
        return 0;
    rocketnet_network_simulator_config( w->sim, w->base_latency, w->base_jitter, w->loss_percent, w->dup_percent );

    if ( !rocketnet_address_parse( &w->server_address, "203.0.113.10:40000" ) )
        return 0;

    memset( &w->server_counters, 0, sizeof(w->server_counters) );
    memset( &w->client_counters, 0, sizeof(w->client_counters) );
    rocketnet_dst_check_init( &w->check, seed, 0 );
    w->server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    w->server.send_rate = 10.0;
    memcpy( w->server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( w->server.backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( w->server.server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( w->server.server_secret_key, rocketnet_test_server_secret_key, 32 );
    randombytes_buf( w->server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    w->server.cookie_lifetime_seconds = 3;
    w->server.cookie_rotation_seconds = 60;
    w->server.cookie_key_rotation_time = w->now + 60.0;
    w->server.handshake_deadline_seconds = 5.0;
    w->server.advertised_ipv4 = w->server_address;
    w->server.counters = &w->server_counters;
    w->server.encryption_map = rocketnet_encryption_map_create( NULL, 16 );
    w->server.used_token_history = rocketnet_used_token_history_create( NULL, 256 );
    w->server.client_slots = rocketnet_client_slots_create( NULL, ROCKETNET_DST_MAX_CLIENTS );
    w->server.acks = rocketnet_server_acks_create( NULL, ROCKETNET_DST_MAX_CLIENTS );
    w->server.reassembly = rocketnet_reassembly_set_create( NULL, ROCKETNET_DST_MAX_CLIENTS, 8, 16384,
                                                            ROCKETNET_DST_MAX_CLIENTS * 8 );
    if ( !w->server.encryption_map || !w->server.used_token_history || !w->server.client_slots ||
         !w->server.acks || !w->server.reassembly )
        return 0;
    rocketnet_reassembly_set_begin_update( w->server.reassembly );
    rocketnet_server_protocol_update( &w->server, w->now );

    w->server_endpoint.world = w;
    w->server_endpoint.is_server = 1;
    w->server_endpoint.pending_frame = -1;
    w->server_endpoint.payload = (uint8_t (*)[ROCKETNET_RECEIVE_PAYLOAD_BYTES])
        malloc( sizeof( uint8_t[ROCKETNET_RECEIVE_PAYLOAD_BYTES] ) * ROCKETNET_DST_MAX_PAYLOADS );
    if ( !w->server_endpoint.payload )
        return 0;
    w->check.allocation_balance++;

    rocketnet_message_default_config( &w->message_config );
    w->message_config.num_channels = ROCKETNET_DST_NUM_CHANNELS;

    struct rn_message_link_t server_link;
    rocketnet_dst_endpoint_link( &server_link, &w->server_endpoint );
    w->message_server = (struct rocketnet_message_server_t *) rn_message_object_create(
        NULL, &server_link, 1, ROCKETNET_DST_MAX_CLIENTS, &w->message_config, NULL, 0,
        ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, w->now );
    if ( !w->message_server )
        return 0;

    for ( int i = 0; i < w->num_clients; i++ )
    {
        rocketnet_dst_client_t * client = &w->client[i];
        char address_string[64];
        snprintf( address_string, sizeof(address_string), "198.51.100.%d:56789", 10 + i );
        if ( !rocketnet_address_parse( &client->address, address_string ) )
            return 0;
        rocketnet_client_protocol_init( &client->protocol, &w->client_counters );
        client->endpoint.world = w;
        client->endpoint.client_index = i;
        client->endpoint.pending_frame = -1;
        client->endpoint.payload = (uint8_t (*)[ROCKETNET_RECEIVE_PAYLOAD_BYTES])
            malloc( sizeof( uint8_t[ROCKETNET_RECEIVE_PAYLOAD_BYTES] ) * ROCKETNET_DST_MAX_PAYLOADS );
        if ( !client->endpoint.payload )
            return 0;
        w->check.allocation_balance++;
        struct rn_message_link_t client_link;
        rocketnet_dst_endpoint_link( &client_link, &client->endpoint );
        client->message = (struct rocketnet_message_client_t *) rn_message_object_create(
            NULL, &client_link, 0, 1, &w->message_config, NULL, 0,
            ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, w->now );
        if ( !client->message )
            return 0;
    }

    return 1;
}

static inline int rocketnet_dst_world_destroy( rocketnet_dst_world_t * w )
{
    if ( w->message_server )
        rocketnet_message_server_destroy( w->message_server );
    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( w->client[i].message )
            rocketnet_message_client_destroy( w->client[i].message );
    }
    if ( w->server.encryption_map )
        rocketnet_encryption_map_destroy( w->server.encryption_map );
    if ( w->server.used_token_history )
        rocketnet_used_token_history_destroy( w->server.used_token_history );
    if ( w->server.client_slots )
        rocketnet_client_slots_destroy( w->server.client_slots );
    if ( w->server.acks )
        rocketnet_server_acks_destroy( w->server.acks );
    if ( w->server.reassembly )
        rocketnet_reassembly_set_destroy( w->server.reassembly );
    if ( w->sim )
        rocketnet_network_simulator_destroy( w->sim );
    if ( w->server_endpoint.payload )
    {
        free( w->server_endpoint.payload );
        if ( w->check.allocation_balance > 0 )
            w->check.allocation_balance--;
    }
    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( w->client[i].endpoint.payload )
        {
            free( w->client[i].endpoint.payload );
            if ( w->check.allocation_balance > 0 )
                w->check.allocation_balance--;
        }
    }
    const int leaked = w->check.allocation_balance != 0;
    memset( w, 0, sizeof(*w) );
    return leaked;
}

static inline int rocketnet_dst_run_seed_with_options( uint64_t seed,
                                                       const struct rocketnet_dst_options_t * options,
                                                       struct rocketnet_dst_result_t * result )
{
    memset( result, 0, sizeof(*result) );
    result->seed = seed;

    rocketnet_dst_world_t * w = (rocketnet_dst_world_t *) calloc( 1, sizeof(rocketnet_dst_world_t) );
    if ( !w )
        return 0;
    if ( !rocketnet_dst_world_create( w, seed, options ) )
    {
        rocketnet_dst_world_destroy( w );
        free( w );
        return 0;
    }

    for ( uint64_t k = 0; k < options->steps; k++ )
    {
        w->step = k;
        w->check.step = k;
        w->now = 1000.0 + (double) k * 0.01;
        w->unix_ts = 1700000000ULL + (uint64_t) ( w->now - 1000.0 );

        if ( options->steps > options->fault_steps && k + 1 == options->fault_steps )
            rocketnet_dst_check_settle( &w->check );
        if ( !w->quiet_applied && options->fault_steps > 0 && k >= options->fault_steps )
        {
            rocketnet_network_simulator_config( w->sim, w->base_latency, 0.0, 0.0f, 0.0f );
            w->quiet_applied = 1;
        }

        if ( options->force_fail_step >= 0 && (int) k == options->force_fail_step )
            rocketnet_dst_fail( w, "forced", "the test forced this check" );

        for ( int i = 0; i < w->num_clients; i++ )
        {
            rocketnet_dst_client_t * client = &w->client[i];
            if ( client->protocol.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED &&
                 (int) k >= client->next_connect_step &&
                 ( client->pause_until < 0 || (int) k >= client->pause_until ) )
            {
                rocketnet_dst_connect_client( w, i );
                client->pause_until = -1;
            }
            if ( client->churn_step >= 0 && (int) k == client->churn_step &&
                 client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
                int burst_count = 0;
                int burst_bytes = 0;
                rocketnet_client_protocol_disconnect( &client->protocol, burst, &burst_count, &burst_bytes );
                for ( int b = 0; b < burst_count; b++ )
                {
                    uint8_t * packet = burst + (size_t) b * (size_t) ROCKETNET_SEND_BUDGET;
                    rocketnet_dst_world_trace_send( w, i + 1, 0, packet, burst_bytes );
                    rocketnet_network_simulator_send( w->sim, &client->address, &w->server_address,
                                                      packet, burst_bytes, w->now );
                }
                client->pause_until = (int) k + 50;
                client->next_connect_step = (int) k + 50;
                client->churn_step = -1;
            }
            rocketnet_message_client_update( client->message, w->now );
        }

        for ( int i = 0; i < w->num_clients; i++ )
        {
            rocketnet_dst_client_t * client = &w->client[i];
            if ( client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                 ( client->pause_until < 0 || (int) k >= client->pause_until ) )
                rocketnet_dst_send_one( w, i, 0, -1 );
            rocketnet_message_client_send_packets( client->message );
        }

        rocketnet_message_server_update( w->message_server, w->now );

        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            const int index = rocketnet_dst_world_client_for_slot( w, slot );
            if ( index >= 0 && index < w->num_clients )
                rocketnet_dst_send_one( w, index, 1, slot );
        }
        rocketnet_message_server_send_packets( w->message_server );

        {
            // the server's one receive queue across every slot: each message
            // names its sender's slot
            const uint8_t * data;
            int bytes;
            int slot = 0;
            while ( rocketnet_message_server_receive( w->message_server, &slot, NULL, &data, &bytes ) )
            {
                uint64_t client_id = rocketnet_client_slots_connected( w->server.client_slots, slot )
                    ? rocketnet_client_slots_client_id( w->server.client_slots, slot )
                    : w->check.slot_client_id[slot];
                uint32_t expected_generation = (uint32_t) ( client_id >> 8 );
                rocketnet_dst_check_deliveries( w, slot, expected_generation, w->server_incarnation,
                                                client_id, data, bytes );
                rocketnet_message_server_release( w->message_server, data );
            }
        }

        for ( int i = 0; i < w->num_clients; i++ )
        {
            rocketnet_dst_client_t * client = &w->client[i];
            {
                const uint8_t * data;
                int bytes;
                const uint32_t expected_generation = (uint32_t) client->generation;
                while ( rocketnet_message_client_receive( client->message, NULL, &data, &bytes ) )
                {
                    rocketnet_dst_check_deliveries( w, 16 + i, expected_generation, w->server_incarnation,
                                                    client->generation, data, bytes );
                    rocketnet_message_client_release( client->message, data );
                }
            }
            if ( client->protocol.state != client->last_state )
            {
                rocketnet_dst_trace_u64( &w->trace, w->step );
                rocketnet_dst_trace_u64( &w->trace, (uint64_t) ( i + 1 ) );
                rocketnet_dst_trace_u64( &w->trace, (uint64_t) client->protocol.state );
                client->last_state = client->protocol.state;
            }
        }

        w->check.step = k;
        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            const int connected = rocketnet_client_slots_connected( w->server.client_slots, slot );
            const uint64_t client_id = connected ? rocketnet_client_slots_client_id( w->server.client_slots, slot ) : 0;
            rocketnet_dst_check_slot( &w->check, slot, connected, client_id, w->server_incarnation );
            if ( connected )
            {
                const int mapping = rocketnet_client_slots_mapping_index( w->server.client_slots, slot );
                struct rocketnet_encryption_mapping_t * entry =
                    rocketnet_encryption_map_entry( w->server.encryption_map, mapping );
                if ( entry )
                    rocketnet_dst_check_replay( &w->check, slot, (uint64_t) entry->client_id,
                                                entry->replay_session.highest_opened );
            }
        }
        for ( int i = 0; i < w->num_clients && i < ROCKETNET_DST_MAX_CLIENTS; i++ )   // the array bound, which num_clients never passes
        {
            rocketnet_dst_client_t * client = &w->client[i];
            const int terminal_explained = (int) k < (int) w->fault_steps + 1000;
            rocketnet_dst_check_client_state( &w->check, i, client->protocol.state, terminal_explained );
            if ( client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                rocketnet_dst_check_replay( &w->check, 16 + i, client->generation,
                                            client->protocol.replay_session.highest_opened );
            rocketnet_dst_check_crashed_silent( &w->check, i + 1, 1, 0 );
            rocketnet_dst_check_clock( &w->check, i + 1, w->now, w->now, 0.45 );
        }
        rocketnet_dst_check_clock( &w->check, 0, w->now, w->now, 0.45 );

        if ( w->check.failed && !w->failed )
        {
            w->failed = 1;
            w->fail_step = w->check.step;
            snprintf( w->fail_check, sizeof(w->fail_check), "%s", w->check.check );
            snprintf( w->fail_detail, sizeof(w->fail_detail), "%s", w->check.detail );
        }
        if ( w->failed )
            break;
    }

    {
        int live[ROCKETNET_DST_CHECK_MAX_SESSIONS];
        uint64_t live_identity[ROCKETNET_DST_CHECK_MAX_SESSIONS];
        for ( int i = 0; i < ROCKETNET_DST_CHECK_MAX_SESSIONS; i++ )
        {
            live[i] = 0;
            live_identity[i] = 0;
        }
        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            live[slot] = rocketnet_client_slots_connected( w->server.client_slots, slot );
            if ( live[slot] )
                live_identity[slot] = rocketnet_client_slots_client_id( w->server.client_slots, slot );
        }
        for ( int i = 0; i < w->num_clients; i++ )
        {
            live[16 + i] = w->client[i].protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
            live_identity[16 + i] = w->client[i].generation;
        }
        if ( !w->check.failed )
        {
            w->check.step = options->steps;
            if ( options->steps > options->fault_steps )
            {
                for ( int i = 0; i < w->num_clients; i++ )
                    rocketnet_dst_check_liveness_connected( &w->check, i, 1,
                        w->client[i].protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
                rocketnet_dst_check_end( &w->check, w->now, live, live_identity );
            }
            rocketnet_dst_check_conservation( &w->check, &w->server_counters, "server" );
            rocketnet_dst_check_conservation( &w->check, &w->client_counters, "clients" );
            rocketnet_dst_check_unexplained_refusal( &w->check, &w->server_counters, "server",
                ( options->faults & ( ROCKETNET_DST_FAULT_DUP | ROCKETNET_DST_FAULT_REORDER | ROCKETNET_DST_FAULT_DELAY ) ) != 0 );
            rocketnet_dst_check_unexplained_refusal( &w->check, &w->client_counters, "clients",
                ( options->faults & ( ROCKETNET_DST_FAULT_DUP | ROCKETNET_DST_FAULT_REORDER | ROCKETNET_DST_FAULT_DELAY ) ) != 0 );
        }
        if ( w->check.failed && !w->failed )
        {
            w->failed = 1;
            w->fail_step = w->check.step;
            snprintf( w->fail_check, sizeof(w->fail_check), "%s", w->check.check );
            snprintf( w->fail_detail, sizeof(w->fail_detail), "%s", w->check.detail );
        }
    }

    result->passed = !w->failed;
    result->trace_hash = w->trace.hash;
    result->step = w->fail_step;
    if ( w->failed )
    {
        snprintf( result->check, sizeof(result->check), "%s", w->fail_check );
        snprintf( result->detail, sizeof(result->detail), "%s", w->fail_detail );
    }

    if ( rocketnet_dst_world_destroy( w ) )
    {
        result->passed = 0;
        result->step = options->steps;
        snprintf( result->check, sizeof(result->check), "leak" );
        snprintf( result->detail, sizeof(result->detail), "harness allocations not freed" );
    }
    free( w );
    return 1;
}

static inline int rocketnet_dst_run_seed( uint64_t seed, uint64_t steps, int pinned_clients,
                                          struct rocketnet_dst_result_t * result )
{
    struct rocketnet_dst_options_t options;
    memset( &options, 0, sizeof(options) );
    options.steps = steps;
    options.fault_steps = steps;
    options.shape = ROCKETNET_DST_SHAPE_FULL;
    options.clients = pinned_clients;
    options.faults = ROCKETNET_DST_FAULT_ALL;
    options.force_fail_step = -1;
    return rocketnet_dst_run_seed_with_options( seed, &options, result );
}

static inline void rocketnet_dst_faults_list( uint32_t faults, char * out, size_t out_bytes )
{
    if ( out_bytes == 0 )
        return;
    if ( faults == ROCKETNET_DST_FAULT_ALL )
    {
        snprintf( out, out_bytes, "all" );
        return;
    }
    if ( faults == 0 )
    {
        snprintf( out, out_bytes, "none" );
        return;
    }

    const struct
    {
        uint32_t bit;
        const char * name;
    } kinds[] =
    {
        { ROCKETNET_DST_FAULT_LOSS, "loss" },
        { ROCKETNET_DST_FAULT_DUP, "dup" },
        { ROCKETNET_DST_FAULT_JITTER, "jitter" },
        { ROCKETNET_DST_FAULT_REORDER, "reorder" },
        { ROCKETNET_DST_FAULT_DELAY, "delay" },
        { ROCKETNET_DST_FAULT_SKEW, "skew" },
        { ROCKETNET_DST_FAULT_CRASH_CLIENT, "crash-client" },
        { ROCKETNET_DST_FAULT_CRASH_SERVER, "crash-server" },
    };
    size_t used = 0;
    out[0] = '\0';
    for ( size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++ )
    {
        if ( faults & kinds[i].bit )
        {
            const int written = snprintf( out + used, out_bytes - used, "%s%s", used ? "," : "", kinds[i].name );
            if ( written < 0 || (size_t) written >= out_bytes - used )
            {
                out[out_bytes - 1] = '\0';
                return;
            }
            used += (size_t) written;
        }
    }
}

static inline int rocketnet_dst_seed_range_next( uint64_t seed, uint64_t last_seed, uint64_t * next_seed )
{
    if ( seed == last_seed )
        return 0;
    *next_seed = seed + 1;
    return 1;
}

// The replay line names the seed alone and every non-default flag of this run.
static inline void rocketnet_dst_replay_command( uint64_t seed, const struct rocketnet_dst_options_t * options,
                                                 char * out, size_t out_bytes )
{
    const char * shape = options->shape == ROCKETNET_DST_SHAPE_PUSH ? "push" : "full";
    char flags[256];
    char faults[128];
    int used = snprintf( flags, sizeof(flags), " --shape %s", shape );
    if ( options->clients > 0 )
        used += snprintf( flags + used, sizeof(flags) - (size_t) used, " --clients %d", options->clients );
    if ( options->steps != ( options->shape == ROCKETNET_DST_SHAPE_PUSH ? 2000ULL : 4000ULL ) )
        used += snprintf( flags + used, sizeof(flags) - (size_t) used, " --steps %llu",
                          (unsigned long long) options->fault_steps );
    if ( options->faults != ROCKETNET_DST_FAULT_ALL )
    {
        rocketnet_dst_faults_list( options->faults, faults, sizeof(faults) );
        used += snprintf( flags + used, sizeof(flags) - (size_t) used, " --faults %s", faults );
    }
    snprintf( out, out_bytes, "./bin/dst --seeds %llu-%llu%s",
              (unsigned long long) seed, (unsigned long long) seed, flags );
}

static inline void rocketnet_dst_format_failure( const struct rocketnet_dst_result_t * result,
                                                 const struct rocketnet_dst_options_t * options,
                                                 char * out, size_t out_bytes )
{
    // "./bin/dst --seeds " (18) + two u64 and a dash (41) + flags[256] (255) = 314
    char replay[320];
    rocketnet_dst_replay_command( result->seed, options, replay, sizeof(replay) );
    snprintf( out, out_bytes, "FAIL seed=%llu step=%llu check=%s what=%s\nreplay: %s",
              (unsigned long long) result->seed, (unsigned long long) result->step,
              result->check, result->detail, replay );
}

#endif // #ifndef ROCKETNET_DST_HARNESS_H

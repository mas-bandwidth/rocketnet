// Protocol defaults and table-sizing ratios the create path assigns.
// One declaration: rocketnet_server_transport_create uses these macros, and
// the conformance dump includes this file under
// ROCKETNET_CONFORMANCE_DEFAULTS_ONLY and expands the same ones.

#define ROCKETNET_DEFAULT_SEND_RATE                    10.0
#define ROCKETNET_DEFAULT_HANDSHAKE_DEADLINE_SECONDS   5.0
#define ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS      3
#define ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS      60
#define ROCKETNET_DEFAULT_MAX_TOKEN_LIFETIME_SECONDS   0
#define ROCKETNET_DEFAULT_ALLOW_NEGATIVE_TIMEOUT       0

static_assert( ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS > ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS,
               "cookie rotation period must exceed cookie lifetime so a cookie never outlives both keys" );

#define ROCKETNET_ENCRYPTION_MAP_RATIO                 2
#define ROCKETNET_USED_TOKEN_HISTORY_RATIO             8

#ifndef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

#include <new>
#include "rocketnet_packet_lease.h"
#include "rocketnet_stats_window.h"
/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_server_replay_marks.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_fragment.h"
#include "rocketnet_config.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_memory.h"

#include <sodium.h>
#include <atomic>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include <linux/if_xdp.h>
#endif
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <pthread.h>
#endif

// The transport driver is selected at RUNTIME (#106): one build carries
// every driver its platform has, and create walks the config's ordered
// candidate list -- config->driver, overridden by ROCKETNET_SERVER_TRANSPORT --
// left to right, first grant wins. On linux that is the runtime ladder:
// probe XDP (privilege and NIC support), fall to the non-root GSO driver
// (SO_REUSEPORT sharding + sendmmsg/recvmmsg + UDP GSO/GRO, per the #81
// measurements), floor at portable. "auto" walks the ladder; ANY explicit
// list is exactly that list, so a one-element list is a pin -- that driver
// or loud failure, never a silent fall that would mask a provisioning
// error. The granted tier is reported request-vs-granted style:
// rocketnet_server_transport_driver plus one log line naming the grant and
// why each earlier candidate refused. The GSO driver's own feature probing
// and degradation remains runtime inside the driver, below this ladder.

#if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )
#error driver selection is runtime now (#106): drop ROCKETNET_SERVER_XDP / ROCKETNET_SERVER_GSO and select with config driver or ROCKETNET_SERVER_TRANSPORT
#endif // #if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )

#define ROCKETNET_SERVER_MAX_RECEIVE_PACKETS 256
#define ROCKETNET_SERVER_MAX_SEND_PACKETS    256

struct rocketnet_server_send_buffer_t
{
    int num_packets;
    rocketnet_address_t to[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    int packet_bytes[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_SERVER_MAX_SEND_PACKETS];
};

// the receive batch on the non-xdp drivers: one pump call fills these, then
// processing walks them. the stride is the receive ceiling, so any packet
// the wire admits fits a slot; the pump moves the batch in as few syscalls
// as the platform earned (#100) and every platform serves the same call.
// the buffer is sized to a full per-update receive budget so one update can
// absorb everything the budget allows, draining a backlog in as few reads
// as the endpoint can answer (see ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE).

struct rocketnet_server_receive_buffer_t
{
    rocketnet_address_t from[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
    int packet_bytes[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
    uint8_t data[ROCKETNET_RECEIVE_CEILING*ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
};

// the portable payload ring: what the acquire/commit surface fills between
// update and the flush, from any number of game threads. slot acquisition is
// one atomic add; each slot's info sits alone on its cache line so
// concurrent finishes never share one; the frame stride keeps every root
// 16-aligned. the response ring above stays single-threaded under update
// and is deliberately not this.

#define ROCKETNET_SERVER_PAYLOAD_RING_SIZE      ROCKETNET_SERVER_MAX_SEND_PACKETS

struct alignas(64) rocketnet_server_payload_slot_info_t
{
    rocketnet_address_t to;
    int wire_bytes;
    int client_index;
};

struct rocketnet_server_payload_ring_t
{
    uint8_t padding_0[4096];

    volatile uint64_t num_packets;

    uint8_t padding_1[4096];

    struct rocketnet_server_payload_slot_info_t info[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];

    uint8_t padding_2[4096];

    alignas(16) uint8_t data[ROCKETNET_FRAME_STRIDE*ROCKETNET_SERVER_PAYLOAD_RING_SIZE];

    uint8_t padding_3[4096];
};

static_assert( ROCKETNET_FRAME_STRIDE % 16 == 0, "frame stride must keep every slot root 16-aligned" );
static_assert( ROCKETNET_FRAME_STRIDE >= ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING, "a slot must hold the slack and a full wire packet" );
static_assert( ROCKETNET_FRAME_PAYLOAD_OFFSET % 16 == 0, "the payload pointer handed to the caller must be 16-aligned" );

// Statistics accumulate during the existing send/receive walks. Buckets
// advance with elapsed time, independently of update cadence, and only the
// trailing second contributes. The frame field marks initialized buckets;
// it does not select their index or determine their retention.

#define ROCKETNET_NUM_SERVER_STATS_FRAMES 256

struct alignas(64) rocketnet_server_stats_frame_t
{
    double time;
    uint64_t frame;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

// cumulative wire totals, per server lifetime and per connection slot. a
// datagram that arrives or leaves before its slot is assigned is held here by
// address and folded into the slot at assignment, so the whole-server totals
// and the sum over the slots agree.

struct rocketnet_server_wire_totals_t
{
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

enum { ROCKETNET_SERVER_PENDING_WIRE_TOTALS = 256 };

struct rocketnet_server_pending_wire_totals_t
{
    rocketnet_address_t address;
    struct rocketnet_server_wire_totals_t totals;
    bool used;
};

// the payload batch from the last update -- capacity ROCKETNET_MAX_PAYLOADS_PER_UPDATE
// covers a full drain. the public surface iterates by index and never sees this layout.

struct rocketnet_server_payloads_t
{
    int num_payloads;
    int client_index[ROCKETNET_MAX_PAYLOADS_PER_UPDATE];
    uint64_t connection_id[ROCKETNET_MAX_PAYLOADS_PER_UPDATE];     // P9b: the connection each entry was received for
    uint8_t * payload_data[ROCKETNET_MAX_PAYLOADS_PER_UPDATE];
    int payload_bytes[ROCKETNET_MAX_PAYLOADS_PER_UPDATE];
};

struct rocketnet_server_transport_t
{
    rocketnet_packet_leases_t<131072> packet_leases;
    void * context;
    int state;
    rocketnet_address_t bind_address;
    rocketnet_address_t public_address;

    // the admission gate: consulted before process_packet for a source with
    // no encryption mapping. NULL admits every source (the default).
    rocketnet_server_admission_callback_t admission_callback;
    void * admission_context;

    // the granted driver (ROCKETNET_DRIVER_*), decided by the ladder at
    // create and constant for the transport's life -- the runtime fork
    // every seam function below branches on, one predictable branch

    int driver;

    rocketnet_server_driver_t io;

    // the kernel replay marks: allocated only when the granted driver reports
    // the replay capability, written from the update walk and committed to the
    // driver's map before the staged flush
    struct rocketnet_server_replay_marks_t * replay_marks;

    struct rocketnet_counters_t counters;
    struct rocketnet_server_protocol_t protocol;

    // the portable path's buffers ride the struct on every platform now
    // that the driver is runtime: about a megabyte a staged grant never
    // touches, priced once per server against carrying the runtime fork
    // without allocation branches

    struct rocketnet_server_send_buffer_t send_buffer;
    struct rocketnet_server_receive_buffer_t receive_buffer;
    struct rocketnet_server_payload_ring_t payload_ring;

    double time;                                        // the game's clock, last seen
    uint64_t test_timestamp;                            // unit-only Unix time; zero reads the wall clock
    uint64_t game_clock_epoch;                          // unix seconds at the game clock's zero for the token check; 0 reads the wall clock
    uint64_t frame;                                     // advanced once per update; identifies initialized stats buckets

    // the fragmentation shape from the config, and the send arena for
    // over-wire-capacity requests -- allocated once at create, to the
    // configured maximum. the reassembly set lives on the protocol.

    int max_payload_bytes;
    struct rocketnet_send_arena_t * send_arena;

    struct rocketnet_server_transport_stats_t server_stats;
    struct rocketnet_server_stats_frame_t stats_frames[ROCKETNET_NUM_SERVER_STATS_FRAMES];
    int current_stats_index;
    struct rocketnet_server_wire_totals_t * client_wire_totals;         // one per client slot
    int client_wire_totals_capacity;
    struct rocketnet_server_pending_wire_totals_t pending_wire_totals[ROCKETNET_SERVER_PENDING_WIRE_TOTALS];
    int maintenance_next_client;                       // first unserved slot after staged backpressure

    volatile uint64_t send_staged;                      // bumped by every producer; fills use the atomic add
    uint64_t send_flushed;                              // main thread only

    // payload sends on the staged drivers seal straight into the driver's
    // own staging from any game thread, so there is no transport-side ring
    // for the flush walk to read; each finish adds its packet here instead
    // -- the same relaxed atomic the path already pays for send_staged --
    // and send_packets folds the pair into the current bucket on the main
    // thread.
    volatile uint64_t stats_pending_packets_sent;
    volatile uint64_t stats_pending_bytes_sent;

    struct rocketnet_network_simulator_t * link_simulator;
    rocketnet_address_t link_simulator_from;
    rocketnet_platform_mutex_t link_simulator_mutex;

    struct rocketnet_server_payloads_t payloads;
    // +8: serialize's BitReader loads 8-byte windows at byte granularity, so
    // any buffer a ReadStream is bound over must extend at least 8 bytes past
    // the end of its data -- this is the last member, so the slack is real.
    uint8_t payload_storage[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES*ROCKETNET_MAX_PAYLOADS_PER_UPDATE + 8];
};

void rocketnet_server_transport_poison_slot_for_test( struct rocketnet_server_transport_t * server, int client_index )
{
    rocketnet_assert( server );
    rocketnet_assert( client_index >= 0 && client_index < server->client_wire_totals_capacity );
    memset( &server->client_wire_totals[client_index], 0xA5, sizeof(server->client_wire_totals[client_index]) );
}

bool rocketnet_server_transport_connection_state_clean_for_test( struct rocketnet_server_transport_t * server )
{
    return server && server->protocol.encryption_map && server->protocol.client_slots &&
        rocketnet_encryption_map_num_mappings( server->protocol.encryption_map ) == 0 &&
        rocketnet_client_slots_num_connected( server->protocol.client_slots ) == 0;
}

bool rocketnet_server_transport_slot_clean_for_test( struct rocketnet_server_transport_t * server, int client_index,
                                                     char * failure, size_t failure_bytes )
{
    rocketnet_assert( server && failure && failure_bytes > 0 );
    failure[0] = '\0';
    struct rocketnet_server_protocol_t * protocol = &server->protocol;
    rocketnet_assert( client_index >= 0 && client_index < rocketnet_client_slots_max_clients( protocol->client_slots ) );
    const char * member = NULL;
    if ( rocketnet_client_slots_connected( protocol->client_slots, client_index ) )
        member = "rocketnet_client_slots_t.connected";
    int iterator = -1;
    while ( !member && rocketnet_encryption_map_next( protocol->encryption_map, &iterator ) )
        if ( rocketnet_encryption_map_entry( protocol->encryption_map, iterator )->client_index == client_index )
            member = "rocketnet_encryption_mapping_t.client_index";
    if ( !member && protocol->connection_ids[client_index] != 0 )
        member = "rocketnet_server_protocol_t.connection_ids";
    if ( !member )
    {
        struct rocketnet_ack_endpoint_t * fresh = (struct rocketnet_ack_endpoint_t *) malloc( sizeof(struct rocketnet_ack_endpoint_t) );
        rocketnet_assert( fresh );
        rocketnet_ack_endpoint_reset( fresh );
        if ( memcmp( fresh, &protocol->acks->endpoints[client_index], sizeof(*fresh) ) != 0 )
            member = "rocketnet_server_acks_t.endpoints";
        free( fresh );
    }
    if ( !member && protocol->reassembly->newest_completed[client_index] != 0 )
        member = "rocketnet_reassembly_set_t.newest_completed";
    const uint8_t * slice = (const uint8_t *) ( protocol->reassembly->slots + client_index * protocol->reassembly->num_slots );
    for ( size_t i = 0; !member && i < (size_t) protocol->reassembly->num_slots * sizeof(struct rocketnet_reassembly_slot_t); i++ )
        if ( slice[i] != 0 )
            member = "rocketnet_reassembly_set_t.slots";
    if ( member )
        snprintf( failure, failure_bytes, "%s[%d]: not reset when the connection ended", member, client_index );
    return member == NULL;
}

static struct rocketnet_server_pending_wire_totals_t * rocketnet_server_pending_wire_find(
    struct rocketnet_server_transport_t * server_transport, const rocketnet_address_t * address, bool create )
{
    for ( int i = 0; i < ROCKETNET_SERVER_PENDING_WIRE_TOTALS; i++ )
    {
        struct rocketnet_server_pending_wire_totals_t * pending = &server_transport->pending_wire_totals[i];
        if ( pending->used && rocketnet_address_equal( &pending->address, address ) )
            return pending;
    }
    if ( !create )
        return NULL;
    for ( int i = 0; i < ROCKETNET_SERVER_PENDING_WIRE_TOTALS; i++ )
    {
        struct rocketnet_server_pending_wire_totals_t * pending = &server_transport->pending_wire_totals[i];
        if ( !pending->used )
        {
            memset( pending, 0, sizeof( *pending ) );
            pending->address = *address;
            pending->used = true;
            return pending;
        }
    }
    return NULL;
}

static void rocketnet_server_wire_totals_add_sent( struct rocketnet_server_transport_t * server_transport,
                                                   int client_index, const rocketnet_address_t * address,
                                                   int packet_bytes )
{
    const uint64_t wire_bytes = (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( address->type ) );
    server_transport->server_stats.packets_sent++;
    server_transport->server_stats.bytes_sent += wire_bytes;
    if ( client_index >= 0 && client_index < server_transport->client_wire_totals_capacity )
    {
        struct rocketnet_server_wire_totals_t * totals = &server_transport->client_wire_totals[client_index];
        totals->packets_sent++;
        totals->bytes_sent += wire_bytes;
        return;
    }
    struct rocketnet_server_pending_wire_totals_t * pending = rocketnet_server_pending_wire_find( server_transport, address, true );
    if ( pending )
    {
        pending->totals.packets_sent++;
        pending->totals.bytes_sent += wire_bytes;
    }
}

// The staged producers (any game thread) fold the whole-server sent counters
// and the per-client sent totals with relaxed atomics, as the fill-phase
// atomics in rocketnet_platform.h do for stats_pending_packets_sent and
// stats_pending_bytes_sent: each counter is meaningful alone and nothing is
// ordered behind it. The pending-by-address table is main-thread only, so a
// producer whose client_index is out of range drops the count rather than
// touch it.

static void rocketnet_server_wire_totals_add_sent_atomic( struct rocketnet_server_transport_t * server_transport,
                                                          int client_index, int address_type, int packet_bytes )
{
    const uint64_t wire_bytes = (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( address_type ) );
    rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &server_transport->server_stats.packets_sent, 1 );
    rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &server_transport->server_stats.bytes_sent, wire_bytes );
    if ( client_index >= 0 && client_index < server_transport->client_wire_totals_capacity )
    {
        struct rocketnet_server_wire_totals_t * totals = &server_transport->client_wire_totals[client_index];
        rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &totals->packets_sent, 1 );
        rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &totals->bytes_sent, wire_bytes );
    }
}

static void rocketnet_server_wire_totals_add_received( struct rocketnet_server_transport_t * server_transport,
                                                       int client_index, const rocketnet_address_t * address,
                                                       int packet_bytes )
{
    const uint64_t wire_bytes = (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( address->type ) );
    server_transport->server_stats.packets_received++;
    server_transport->server_stats.bytes_received += wire_bytes;
    if ( client_index >= 0 && client_index < server_transport->client_wire_totals_capacity )
    {
        struct rocketnet_server_wire_totals_t * totals = &server_transport->client_wire_totals[client_index];
        totals->packets_received++;
        totals->bytes_received += wire_bytes;
        return;
    }
    struct rocketnet_server_pending_wire_totals_t * pending = rocketnet_server_pending_wire_find( server_transport, address, true );
    if ( pending )
    {
        pending->totals.packets_received++;
        pending->totals.bytes_received += wire_bytes;
    }
}

static int rocketnet_server_wire_client_index( struct rocketnet_server_transport_t * server_transport,
                                               const rocketnet_address_t * address )
{
    const int mapping_index = rocketnet_encryption_map_find( server_transport->protocol.encryption_map, address );
    if ( mapping_index < 0 )
        return -1;
    return rocketnet_encryption_map_entry( server_transport->protocol.encryption_map, mapping_index )->client_index;
}

static void rocketnet_server_wire_totals_assign( struct rocketnet_server_transport_t * server_transport,
                                                 int client_index, const rocketnet_address_t * address )
{
    rocketnet_assert( client_index >= 0 && client_index < server_transport->client_wire_totals_capacity );
    struct rocketnet_server_wire_totals_t * totals = &server_transport->client_wire_totals[client_index];
    memset( totals, 0, sizeof( *totals ) );
    struct rocketnet_server_pending_wire_totals_t * pending = rocketnet_server_pending_wire_find( server_transport, address, false );
    if ( pending )
    {
        totals->packets_sent += pending->totals.packets_sent;
        totals->packets_received += pending->totals.packets_received;
        totals->bytes_sent += pending->totals.bytes_sent;
        totals->bytes_received += pending->totals.bytes_received;
        pending->used = false;
    }
}

// Dispatch is immutable for the transport lifetime. Drivers own their native
// resources; the transport selects behavior through the declared capabilities.
static bool rocketnet_server_transport_staged( const rocketnet_server_transport_t * transport )
{
    return rocketnet_server_driver_has( transport->io, ROCKETNET_DRIVER_STAGED );
}

// a NULL start with the id ROCKETNET_DRIVER_PACKET_UNREACHABLE is the
// driver's refusal for want of a link-layer destination (DRIVERS.md, xdp):
// counted here, never as a full ring, and never sent or counted as sent
static uint8_t * rocketnet_server_driver_start_packet( rocketnet_server_transport_t * transport, const rocketnet_address_t * to, uint64_t * id )
{
    *id = 0;
    uint8_t * frame = transport->io.operations->start_packet( transport->io.instance, to, id );
    if ( !frame && *id == ROCKETNET_DRIVER_PACKET_UNREACHABLE )
        rocketnet_platform_atomic_add_u64( &transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS], 1 );
    return frame;
}
static void rocketnet_server_driver_finish_packet( rocketnet_server_transport_t * transport, uint64_t id, uint8_t * frame, int bytes )
{
    transport->io.operations->finish_packet( transport->io.instance, id, frame, bytes );
}
static void rocketnet_server_driver_abort_packet( rocketnet_server_transport_t * transport, uint64_t id, uint8_t * frame )
{
    transport->io.operations->abort_packet( transport->io.instance, id, frame );
}
static void rocketnet_server_driver_send_packets( rocketnet_server_transport_t * transport )
{
    transport->io.operations->flush( transport->io.instance );
}

static void rocketnet_server_link_send( rocketnet_server_transport_t * transport,
                                        const rocketnet_address_t * to,
                                        const uint8_t * packet_data, int packet_bytes )
{
    rocketnet_platform_mutex_acquire( &transport->link_simulator_mutex );
    rocketnet_network_simulator_send( transport->link_simulator, &transport->link_simulator_from,
                                      to, packet_data, packet_bytes, transport->time );
    rocketnet_platform_mutex_release( &transport->link_simulator_mutex );
}

static void rocketnet_server_link_send_batch( rocketnet_server_transport_t * transport,
                                              const rocketnet_platform_send_t * sends, int num_sends )
{
    if ( !transport->link_simulator )
    {
        transport->io.operations->send_batch( transport->io.instance, sends, num_sends );
        return;
    }
    for ( int i = 0; i < num_sends; i++ )
        rocketnet_server_link_send( transport, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
}

static void rocketnet_server_link_drain( rocketnet_server_transport_t * transport )
{
    if ( !transport->link_simulator )
        return;
    for (;;)
    {
        rocketnet_address_t from, to;
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        rocketnet_platform_mutex_acquire( &transport->link_simulator_mutex );
        const int packet_bytes = rocketnet_network_simulator_receive_any( transport->link_simulator,
                                                                          transport->time, &from, &to, packet );
        rocketnet_platform_mutex_release( &transport->link_simulator_mutex );
        if ( packet_bytes <= 0 )
            break;
        if ( rocketnet_server_transport_staged( transport ) )
        {
            uint64_t id = 0;
            uint8_t * frame = rocketnet_server_driver_start_packet( transport, &to, &id );
            if ( frame )
            {
                memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, packet, (size_t) packet_bytes );
                rocketnet_server_driver_finish_packet( transport, id, frame, packet_bytes );
            }
        }
        else
        {
            rocketnet_platform_send_t send = { &to, packet, packet_bytes };
            transport->io.operations->send_batch( transport->io.instance, &send, 1 );
        }
    }
    if ( rocketnet_server_transport_staged( transport ) )
        rocketnet_server_driver_send_packets( transport );
}

// Control frames borrow this buffer only during synchronous send_batch.
// Drain full batches as update fills them, so receive responses cannot
// consume the entire maintenance budget and servers can service >256 slots.
static void rocketnet_server_flush_control_buffer( rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( !rocketnet_server_transport_staged( server_transport ) );
    rocketnet_assert( server_transport->io.operations && server_transport->io.operations->send_batch );
    struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    int num_sends = 0;
    for ( int i = 0; i < server_transport->send_buffer.num_packets; i++ )
    {
        if ( server_transport->send_buffer.packet_bytes[i] > 0 )
        {
            sends[num_sends].to = &server_transport->send_buffer.to[i];
            sends[num_sends].packet_data = server_transport->send_buffer.data + i * ROCKETNET_FRAME_SIZE + ROCKETNET_FRAME_WIRE_OFFSET;
            sends[num_sends].packet_bytes = server_transport->send_buffer.packet_bytes[i];
            num_sends++;
        }
    }
    if ( num_sends > 0 )
        rocketnet_server_link_send_batch( server_transport, sends, num_sends );
    server_transport->send_buffer.num_packets = 0;
}

// The send frame seam every driver serves: reserve hands back a frame to
// seal complete wire bytes into, commit stamps its length, abort returns it
// unused. The portable driver batches frames and flushes them through the
// socket in bounded batches during update; the staged drivers seal into their
// own send staging -- AF_XDP UMEM, a GSO batch, or an IOCP operation -- with
// zero copies between the seal and the flush.

struct rocketnet_server_frame_t
{
    uint8_t * data;
    uint64_t packet_id;             // the staged drivers' bookkeeping id; unused on the portable path
    rocketnet_address_t to;
    int client_index;               // slot this frame belongs to, or -1 before assignment
};

// tla/RNDriverSendStaging.tla Reserve
static uint8_t * rocketnet_server_reserve_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                                const rocketnet_address_t * to,
                                                                struct rocketnet_server_frame_t * frame )
{
    frame->to = *to;
    frame->client_index = -1;
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        frame->data = rocketnet_server_driver_start_packet( server_transport, to, &frame->packet_id );
        if ( !frame->data )
        {
            rocketnet_spam( "server transport send staging full. packet dropped" );
        }
        return frame->data;
    }

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;

    if ( send_buffer->num_packets >= ROCKETNET_SERVER_MAX_SEND_PACKETS )
    {
        rocketnet_server_flush_control_buffer( server_transport );
    }

    const int index = send_buffer->num_packets;
    send_buffer->to[index] = *to;
    send_buffer->packet_bytes[index] = 0;
    frame->data = send_buffer->data + index * ROCKETNET_FRAME_SIZE;
    return frame->data;
}

// tla/RNDriverSendStaging.tla Commit
static void rocketnet_server_commit_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                          struct rocketnet_server_frame_t * frame,
                                                          int packet_bytes )
{
    rocketnet_assert( frame->data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    // every commit through this seam -- responses, keep-alives, disconnect
    // bursts -- runs on the main thread, so it lands in the current stats
    // bucket and the cumulative totals directly

    const int stats_index = server_transport->current_stats_index;
    server_transport->stats_frames[stats_index].packets_sent++;
    server_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( frame->to.type ) );

    rocketnet_server_wire_totals_add_sent( server_transport, frame->client_index, &frame->to, packet_bytes );

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        if ( server_transport->link_simulator )
        {
            rocketnet_server_link_send( server_transport, &frame->to,
                                        frame->data + ROCKETNET_FRAME_WIRE_OFFSET, packet_bytes );
            rocketnet_server_driver_abort_packet( server_transport, frame->packet_id, frame->data );
        }
        else
            rocketnet_server_driver_finish_packet( server_transport, frame->packet_id, frame->data, packet_bytes );
        return;
    }

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;
    send_buffer->packet_bytes[send_buffer->num_packets] = packet_bytes;
    send_buffer->num_packets++;
}

static void rocketnet_server_abort_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                         struct rocketnet_server_frame_t * frame )
{
    if ( !frame->data )
        return;

    if ( rocketnet_server_transport_staged( server_transport ) )
        rocketnet_server_driver_abort_packet( server_transport, frame->packet_id, frame->data );

    // on the portable path the reserved slot is reused by the next
    // reserve; nothing to undo

    frame->data = NULL;
}

// rocketnet_server_transport_default_config lives in rocketnet_config.cpp,
// written from the config table so every default is declared exactly once

void rocketnet_server_transport_dev_config( struct rocketnet_server_transport_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    rocketnet_assert( config );
    rocketnet_server_transport_default_config( config );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( config->server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( config->server_secret_key, rocketnet_dev_server_secret_key, 32 );
}

static int rocketnet_server_config_has_keys( const struct rocketnet_server_transport_config_t * config )
{
    uint8_t zero[32];
    memset( zero, 0, sizeof(zero) );
    return memcmp( config->server_secret_key, zero, 32 ) != 0 &&
           memcmp( config->server_public_key, zero, 32 ) != 0 &&
           memcmp( config->backend_public_key, zero, 32 ) != 0;
}

// the one surviving pre-#106 env alias kept working under the ROCKETNET_PUMP
// pattern: ROCKETNET_SERVER_NUM_QUEUES is honored only where its successor
// ROCKETNET_SERVER_NUM_XDP_QUEUES is unset, strict-parsed by the same rules,
// and every use is logged loudly naming the successor; set beside the
// successor it yields, loudly.

static bool rocketnet_server_apply_env_alias( const char * alias_name, const char * successor_name, int * field )
{
    const char * value = rocketnet_platform_getenv( alias_name );
    if ( !value || value[0] == '\0' )
        return true;

    const char * successor_value = rocketnet_platform_getenv( successor_name );
    if ( successor_value && successor_value[0] != '\0' )
    {
        rocketnet_warn( "server config ignores %s: its successor %s is set", alias_name, successor_name );
        return true;
    }

    int parsed;
    if ( !rocketnet_config_parse_int_value( value, &parsed ) )
    {
        rocketnet_error( "server config could not parse %s=\"%s\" as an integer", alias_name, value );
        return false;
    }

    rocketnet_info( "server config override %s=%d (deprecated alias; %s is the successor) (displaces %d)", alias_name, parsed, successor_name, *field );
    *field = parsed;
    return true;
}

bool rocketnet_server_transport_memory_plan(const rocketnet_server_transport_config_t * config,
    const char * driver, rocketnet_server_memory_plan_t & plan)
{
    plan = {};
    if (!driver) return false;
    rocketnet_server_transport_config_t defaults;
    if (!config) { rocketnet_server_transport_default_config(&defaults); config = &defaults; }
    const int clients = config->max_clients;
    const int payload = config->max_payload_bytes ? config->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int slots = config->num_reassembly_slots ? config->num_reassembly_slots : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    if (clients < 1 || clients > ROCKETNET_MAX_CLIENTS || payload < ROCKETNET_MAX_PAYLOAD_BYTES ||
        payload > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES || slots < 1 || slots > ROCKETNET_MAX_FRAGMENTS ||
        config->num_xdp_queues < 0 || config->num_xdp_queues > 1024) return false;

    plan.transport_bytes = rocketnet_memory_allocations({sizeof(rocketnet_server_transport_t)});
    for (uint64_t bytes : {rocketnet_encryption_map_allocation_bytes(clients * ROCKETNET_ENCRYPTION_MAP_RATIO),
        rocketnet_token_table_allocation_bytes(clients * ROCKETNET_USED_TOKEN_HISTORY_RATIO),
        rocketnet_client_slots_allocation_bytes(clients), rocketnet_server_acks_allocation_bytes(clients),
        rocketnet_memory_allocations({(uint64_t)clients*sizeof(uint64_t), (uint64_t)clients*ROCKETNET_SENT_PACKET_BUFFER_SIZE*sizeof(uint64_t)}),
        // The plan counts what the transport allocates, once: the per-client wire totals are a separate allocation.
        rocketnet_memory_allocations({(uint64_t)clients*sizeof(rocketnet_server_wire_totals_t)})})
    {
        if (!bytes || !rocketnet_memory_add(plan.protocol_bytes, 1, bytes)) return false;
    }
    plan.reassembly_bytes = rocketnet_reassembly_memory_required_delivered(clients, slots, payload,
        rocketnet_server_reassembly_delivered_capacity(clients, slots),
        rocketnet_server_reassembly_delivered_bytes(clients, slots, payload));
    if (!plan.reassembly_bytes || !rocketnet_memory_add(plan.reassembly_bytes, 4, rocketnet_allocation_overhead())) return false;
    plan.send_arena_bytes = rocketnet_send_arena_allocation_bytes(ROCKETNET_SEND_ARENA_SLOTS, (payload + 15) & ~15);
    if (!rocketnet_builtin_server_memory_plan(config, driver, plan)) return false;
    if (!plan.transport_bytes || !plan.send_arena_bytes || !plan.driver_bytes) return false;
    for (uint64_t bytes : {plan.transport_bytes, plan.protocol_bytes, plan.reassembly_bytes,
        plan.send_arena_bytes, plan.driver_bytes, plan.umem_bytes})
        if (!rocketnet_memory_add(plan.total_bytes, 1, bytes)) return false;
    return plan.total_bytes <= SIZE_MAX;
}

uint64_t rocketnet_server_transport_required_memory(const rocketnet_server_transport_config_t * config, const char * driver)
{
    rocketnet_server_memory_plan_t plan;
    return rocketnet_server_transport_memory_plan(config, driver, plan) ? plan.total_bytes : 0;
}

// The stack a driver thread is created with. rocketnet_platform_thread_create
// passes no stack size (pthread_create with NULL attributes, CreateThread with
// zero), so the size is the platform default: on POSIX the default attribute's
// stack size, read without creating a thread; on Windows the executable's
// default reserve, 1 MiB unless the binary is linked with another /STACK.
static uint64_t rocketnet_server_thread_stack_bytes()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return 1024 * 1024;
#else
    pthread_attr_t attributes;
    if ( pthread_attr_init( &attributes ) != 0 )
        return 0;
    size_t bytes = 0;
    if ( pthread_attr_getstacksize( &attributes, &bytes ) != 0 )
        bytes = 0;
    pthread_attr_destroy( &attributes );
    return (uint64_t) bytes;
#endif
}

// Whole-host estimate (TRANSPORT_READINESS.md #155): the plan plus what each
// built-in driver asks of the kernel. Requests only, never grants. The sizes
// are the ones the drivers pass: the portable and msgx socket and every gso
// queue socket set SO_SNDBUF/SO_RCVBUF to ROCKETNET_SOCKET_SEND_BUFFER_SIZE and
// ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE (rocketnet_driver_builtin.cpp,
// rocketnet_server_gso_linux.cpp), as do every io_uring queue socket
// (rocketnet_server_uring_linux.cpp) and the iocp socket
// (rocketnet_server_iocp_windows.cpp); each xdp queue's AF_XDP socket asks for
// ROCKETNET_XDP_RECV_QUEUE_SIZE RX and ROCKETNET_XDP_SEND_QUEUE_SIZE TX
// descriptors and sets no socket buffer (rocketnet_server_socket_linux.cpp).
bool rocketnet_server_transport_host_memory_estimate( const struct rocketnet_server_transport_config_t * config,
                                                      const char * driver,
                                                      struct rocketnet_host_memory_estimate_t * estimate )
{
    if ( !rocketnet_api_null_guard( estimate ) )
        return false;
    memset( estimate, 0, sizeof(*estimate) );

    rocketnet_server_memory_plan_t plan;
    if ( !driver || !rocketnet_server_transport_memory_plan( config, driver, plan ) )
        return false;

    struct rocketnet_host_memory_estimate_t e;
    memset( &e, 0, sizeof(e) );
    e.allocator_bytes = plan.total_bytes - plan.umem_bytes;
    e.umem_bytes = plan.umem_bytes;
    e.sockets = plan.sockets;
    e.worker_threads = plan.worker_threads;

    const bool xdp = strcmp( driver, "xdp" ) == 0;
    if ( xdp )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
        const uint64_t ring_bytes = (uint64_t) ( ROCKETNET_XDP_RECV_QUEUE_SIZE + ROCKETNET_XDP_SEND_QUEUE_SIZE ) * sizeof(struct xdp_desc);
        if ( !rocketnet_memory_add( e.xdp_ring_bytes, (uint64_t) plan.sockets, ring_bytes ) )
            return false;
#else
        return false;
#endif
    }
    else
    {
        if ( !rocketnet_memory_add( e.socket_send_buffer_bytes, (uint64_t) plan.sockets, ROCKETNET_SOCKET_SEND_BUFFER_SIZE ) ||
             !rocketnet_memory_add( e.socket_receive_buffer_bytes, (uint64_t) plan.sockets, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE ) )
            return false;
    }

    if ( plan.worker_threads > 0 )
    {
        e.thread_stack_bytes_per_thread = rocketnet_server_thread_stack_bytes();
        if ( !e.thread_stack_bytes_per_thread ||
             !rocketnet_memory_add( e.thread_stack_bytes, (uint64_t) plan.worker_threads, e.thread_stack_bytes_per_thread ) )
            return false;
    }

    for ( uint64_t bytes : { e.allocator_bytes, e.umem_bytes, e.socket_send_buffer_bytes,
                             e.socket_receive_buffer_bytes, e.xdp_ring_bytes, e.thread_stack_bytes } )
    {
        if ( !rocketnet_memory_add( e.total_bytes, 1, bytes ) )
            return false;
    }

    // what the library cannot know, named rather than guessed
    const char * excluded[ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS];
    int count = 0;
    excluded[count++] = "kernel socket accounting beyond the buffer requests (linux doubles each SO_SNDBUF/SO_RCVBUF request) and grants smaller than the requests";
    if ( xdp )
    {
        excluded[count++] = "AF_XDP fill and completion rings (the umem config leaves their sizes to libxdp), ring headers and page rounding";
        excluded[count++] = "libbpf/libxdp allocations, the XDP program and its maps";
    }
    if ( strcmp( driver, "io_uring" ) == 0 )
        excluded[count++] = "io_uring submission and completion rings and the provided buffer rings (kernel and liburing mappings outside the SDK allocator)";
    if ( plan.worker_threads > 0 )
        excluded[count++] = "thread guard pages, thread-local storage and kernel thread structures; stacks count reserved address space, not resident pages";
    excluded[count++] = "kernel page tables and loader/shared-library memory";
    excluded[count++] = "libc allocator bookkeeping outside the SDK allocator";
    excluded[count++] = "application memory";
    rocketnet_assert( count <= ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS );
    for ( int i = 0; i < count; i++ )
        e.exclusions[i] = excluded[i];
    e.num_exclusions = count;

    *estimate = e;
    return true;
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context,
                                                                         const char * public_address_string,
                                                                         const struct rocketnet_server_transport_config_t * config)
{
    return rocketnet_server_transport_create_ex( context, public_address_string, config, 0.0, NULL );
}

static struct rocketnet_server_transport_t * rocketnet_server_transport_create_internal( void * context,
                                                                                       const char * public_address_string,
                                                                                       const struct rocketnet_server_transport_config_t * config,
                                                                                       double time, int * status );

struct rocketnet_server_transport_t * rocketnet_server_transport_create_ex( void * context, const char * public_address_string,
                                                                              const struct rocketnet_server_transport_config_t * config,
                                                                              double time, int * status )
{
    if ( !config )
        return rocketnet_server_transport_create_internal( context, public_address_string, NULL, time, status );
    struct rocketnet_server_transport_config_t current;
    rocketnet_server_transport_default_config_current( &current );
    memcpy( &current, config, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1 );
    return rocketnet_server_transport_create_internal( context, public_address_string, &current, time, status );
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create_current( void * context, const char * address,
                                                                                  const struct rocketnet_server_transport_config_t * config )
{
    return rocketnet_server_transport_create_internal( context, address, config, 0.0, NULL );
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create_current_ex( void * context, const char * address,
                                                                                     const struct rocketnet_server_transport_config_t * config, int * status )
{
    return rocketnet_server_transport_create_internal( context, address, config, 0.0, status );
}

static struct rocketnet_server_transport_t * rocketnet_server_transport_create_internal( void * context,
                                                                           const char * public_address_string,
                                                                           const struct rocketnet_server_transport_config_t * config,
                                                                           double time, int * status )
{
    // docs/ERRORS-AND-LOGGING.md 2.1 and the settled Q2: a NULL required
    // argument is a logged INVALID_ARGUMENT and a NULL return in debug and
    // release (S1).
    if ( !rocketnet_api_null_guard( public_address_string ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
        rocketnet_error( "server transport create: public_address_string is NULL" );
        return NULL;
    }

    // the lifecycle state is read once here, before any allocation or socket;
    // a create before rocketnet_init is refused, logged and counted
    if ( !rocketnet_library_initialized() )
    {
        if ( status )
            *status = ROCKETNET_STATUS_NOT_INITIALIZED;
        rocketnet_error( "rocketnet_server_transport_create: rocketnet_init must be called first" );
        rocketnet_api_misuse_record();
        return NULL;
    }

    rocketnet_assert( public_address_string );

    // the config layering (#106): DEFAULT < STRUCT < ENV. NULL config is
    // the default shape; the caller's struct is the middle layer; the
    // environment overlays both through the config table, every override
    // logged loudly with the value it displaced. validation refuses
    // loudly and never clamps.

    struct rocketnet_server_transport_config_t effective_config;
    if ( config )
        effective_config = *config;
    else
        rocketnet_server_transport_default_config_current( &effective_config );

    // the env layer's public_address is snapshotted against so its SCOPE
    // can be enforced below: the env override redirects the FIRST server
    // create in the process and is then consumed -- an operator's deployed
    // binary has one server transport and gets exactly the redirect they
    // asked for, while a process with many creates (the test suite, a
    // soak) is not wholesale redirected by one ambient variable. the
    // struct field keeps per-create scope: a caller who sets it in code
    // said what they meant, every time.

    char struct_public_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    memcpy( struct_public_address, effective_config.public_address, sizeof(struct_public_address) );

    int num_config_fields = 0;
    const struct rocketnet_config_field_t * config_fields = rocketnet_server_transport_config_fields( &num_config_fields );

    if ( !rocketnet_config_apply_env( "server", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return NULL;
    }

    // the one surviving alias: ROCKETNET_SERVER_NUM_QUEUES is the pre-#106
    // spelling of the num_xdp_queues field, whose canonical per-endpoint
    // name is now ROCKETNET_SERVER_NUM_XDP_QUEUES. ROCKETNET_SERVER_NUM_OS_CPUS
    // and ROCKETNET_SERVER_PUBLIC_ADDRESS are no longer aliases -- they are
    // the canonical names the table reads above -- and the old process-wide
    // ROCKETNET_NUM_OS_CPUS / ROCKETNET_PUBLIC_ADDRESS spellings are refused
    // aloud by rocketnet_config_apply_env with their replacements named.

    if ( !rocketnet_server_apply_env_alias( "ROCKETNET_SERVER_NUM_QUEUES", "ROCKETNET_SERVER_NUM_XDP_QUEUES", &effective_config.num_xdp_queues ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return NULL;
    }

    if ( !rocketnet_config_validate( "server", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }

    if ( !rocketnet_config_validate_public_link_simulator( "server", &effective_config, config_fields, num_config_fields, rocketnet_public_link_simulator_development() ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }

    config = &effective_config;

    // Refuse large cross-field allocations before sockets, drivers or arenas
    // are acquired. Independent field maxima do not define a usable budget.
    const int requested_payload = config->max_payload_bytes > 0 ? config->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int requested_slots = config->num_reassembly_slots > 0 ? config->num_reassembly_slots : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    const int requested_clients = config->max_clients > 0 ? config->max_clients : ROCKETNET_MAX_CLIENTS;
    if (!rocketnet_reassembly_memory_allowed_delivered(requested_clients, requested_slots, requested_payload,
            rocketnet_server_reassembly_delivered_capacity(requested_clients, requested_slots),
            rocketnet_server_reassembly_delivered_bytes(requested_clients, requested_slots, requested_payload)))
    {
        if ( status )
            *status = ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED;
        rocketnet_error( "server config refused: the reassembly cross-field allocation exceeds the operator memory budget" );
        return NULL;
    }


    // the config's public_address, when set, displaces the create call's
    // address parameter HERE, before anything reads it -- so the address
    // the ladder verifies, the address the granted driver binds, and the
    // address connect tokens validate against can never diverge. the env
    // layer's version of it is first-create-consumes, per the scope note
    // above.

    // the env override's consumption flag: atomic because two transports
    // in one process is a blessed case and nothing serializes creates, and
    // CONSUMED ONLY AFTER A FULLY SUCCESSFUL CREATE -- a first create that
    // refuses (bad keys, exhausted ladder) must not eat the deployed
    // binary's redirect on the retry. two concurrent first creates racing
    // the same override is unsupported and stated: creates are setup acts,
    // not fill-phase acts.

    static std::atomic<bool> env_public_address_consumed( false );

    bool consuming_env_public_address = false;

    if ( strncmp( effective_config.public_address, struct_public_address, sizeof(struct_public_address) ) != 0 )
    {
        // the env layer (or its deprecated alias) set it

        if ( env_public_address_consumed.load() )
        {
            rocketnet_info( "the public_address env override was consumed by an earlier server create in this process. using the create parameters" );
            memcpy( effective_config.public_address, struct_public_address, sizeof(struct_public_address) );
        }
        else
        {
            consuming_env_public_address = true;
        }
    }

    if ( config->public_address[0] != '\0' )
    {
        rocketnet_info( "server config public_address %s displaces the create parameter %s", config->public_address, public_address_string );
        public_address_string = config->public_address;
    }

    const int max_clients = config->max_clients;        // the table validated the range

    // a server with no keys is refused, never defaulted: default keys are how
    // every deployment of a protocol ends up sharing one identity. development
    // wants rocketnet_server_transport_dev_config, which fills the published keys loudly;
    // a deployed binary can also carry its keys on the env surface.

    if ( !rocketnet_server_config_has_keys( config ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_MISSING_KEYS;
        rocketnet_error( "server config has no keys: fill rocketnet_server_transport_dev_config() for development, or supply production keys" );
        return NULL;
    }

    // Refuse protocol_id 0 with production keys: only development keys may use protocol_id 0
    if ( config->protocol_id == 0 )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_PROTOCOL_ID;
        rocketnet_error( "protocol_id 0 is not permitted with production keys: use dev config or a non-zero protocol_id" );
        return NULL;
    }

    // the driver candidate list parses before anything allocates: a typo
    // fails here, loudly, listing the valid names

    int num_available_drivers = 0;
    const int * available_drivers = rocketnet_server_driver_availability( &num_available_drivers );

    struct rocketnet_driver_ladder_t ladder;
    if ( !rocketnet_driver_ladder_parse( config->driver, available_drivers, num_available_drivers, &ladder ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_TRANSPORT_LIST;
        return NULL;
    }

    rocketnet_info( "server transport public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_ADDRESS;
        rocketnet_error( "could not parse public address" );
        return NULL;
    }

    rocketnet_address_t bind_address = public_address;
    if ( bind_address.type == ROCKETNET_ADDRESS_IPV4 )
        memset( bind_address.data.ipv4, 0, sizeof(bind_address.data.ipv4) );
    else
        memset( bind_address.data.ipv6, 0, sizeof(bind_address.data.ipv6) );

    // the operator memory budget (#155): each candidate's pre-allocation plan
    // is compared to the budget before any allocation, and a candidate whose
    // plan exceeds it is refused exactly as a refusing driver is -- logged with
    // the plan and the budget, and skipped so the ladder falls to the next
    // candidate. a budget that no candidate fits refuses the create before the
    // first allocation.
    uint64_t memory_budget = 0;
    if ( !rocketnet_config_apply_env_uint64( "server", "memory_budget_bytes", &memory_budget ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return NULL;
    }
    bool budget_refused[ROCKETNET_NUM_DRIVERS];
    memset( budget_refused, 0, sizeof(budget_refused) );
    int budget_refused_total = 0;
    int budget_allowed = 0;

    for (int i = 0; i < ladder.num_candidates; ++i)
    {
        const int candidate = ladder.candidates[i];
        const char * name = rocketnet_driver_name(candidate);
        rocketnet_server_memory_plan_t plan;
        if (rocketnet_server_transport_memory_plan(config, name, plan))
        {
            rocketnet_info("server memory plan [%s]: %llu allocator bytes, %llu UMEM bytes, %d workers, %d sockets; excludes OS stacks, kernel queues, loader and application memory",
                name, (unsigned long long)(plan.total_bytes - plan.umem_bytes),
                (unsigned long long)plan.umem_bytes, plan.worker_threads, plan.sockets);
            if ( memory_budget > 0 && plan.total_bytes > memory_budget )
            {
                rocketnet_warn("server transport %s refused: memory plan %llu allocator bytes exceeds the %llu byte operator budget",
                    name, (unsigned long long)plan.total_bytes, (unsigned long long)memory_budget);
                budget_refused[candidate] = true;
                budget_refused_total++;
            }
            else
                budget_allowed++;
        }
        else
            rocketnet_info("server memory plan [%s]: unavailable; consult this transport's resource policy", name);
    }

    if ( memory_budget > 0 && budget_refused_total > 0 && budget_allowed == 0 )
    {
        if ( status )
            *status = ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED;
        rocketnet_error("server transport ladder exhausted: every candidate's memory plan exceeds the %llu byte operator budget",
            (unsigned long long)memory_budget);
        return NULL;
    }

    struct rocketnet_server_transport_t * server_transport = (struct rocketnet_server_transport_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_transport_t) );
    if ( !server_transport )
    {
        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        rocketnet_error( "server transport create: could not allocate server transport" );
        return NULL;
    }
    new ( server_transport ) rocketnet_server_transport_t();

    server_transport->context = context;
    server_transport->time = time;
    server_transport->game_clock_epoch = config->game_clock_epoch;
    rocketnet_server_link_drain( server_transport );

    // the fragmentation shape: zero means the defaults; the table already
    // refused anything out of range, loudly, never clamped

    server_transport->max_payload_bytes = config->max_payload_bytes > 0 ? config->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int num_reassembly_slots = config->num_reassembly_slots > 0 ? config->num_reassembly_slots : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;

    const int development_keys = memcmp( config->server_public_key, rocketnet_dev_server_public_key, 32 ) == 0 ||
                                 memcmp( config->backend_public_key, rocketnet_dev_backend_public_key, 32 ) == 0;
    if ( development_keys )
    {
        // The published development keys are public and not for production: an
        // implementation must warn on every start that uses them.
        rocketnet_warn( "development keys are public and not for production" );
#if ROCKETNET_DEVELOPMENT
#else // !ROCKETNET_DEVELOPMENT
        // A release build must refuse the published development keys outright.
        if ( status )
            *status = ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE;
        rocketnet_error( "development keys in release build; refusing to start" );
        rocketnet_free( context, server_transport );
        return NULL;
#endif // ROCKETNET_DEVELOPMENT
    }

    // THE LADDER (#106): candidates tried left to right, first grant wins.
    // an operator who wrote the list owns the list -- nothing is appended
    // -- and a one-element list is a pin that refuses rather than falls.
    // each refusal logs loudly; the exhausted list fails naming what was
    // skipped for platform and what refused for grant.

    int granted_driver = -1;

    char refused_names[128];
    refused_names[0] = '\0';

    bool refused_seen[ROCKETNET_NUM_DRIVERS];
    memset( refused_seen, 0, sizeof(refused_seen) );

    // #109: WHICH rungs the port-zero rule refused, so an exhausted ladder
    // can restate the rule at the level the failure itself is reported.
    // The names are collected rather than a bare flag because a mixed list
    // -- say "xdp,gso" where gso refused for its own queue geometry --
    // would make "no driver here can discover a port" a lie about gso. The
    // error names the rungs the rule actually refused, and nothing else.

    char port_zero_refused_names[128];
    port_zero_refused_names[0] = '\0';

    bool port_zero_refused_seen[ROCKETNET_NUM_DRIVERS];
    memset( port_zero_refused_seen, 0, sizeof(port_zero_refused_seen) );

    for ( int i = 0; i < ladder.num_candidates && granted_driver < 0; i++ )
    {
        const int candidate = ladder.candidates[i];

        // a candidate whose memory plan the operator budget refused was logged
        // and refused in the plan loop above; record it for the exhausted
        // summary and fall to the next candidate, never opening it
        if ( budget_refused[candidate] )
        {
            if ( candidate >= 0 && candidate < ROCKETNET_NUM_DRIVERS && !refused_seen[candidate] )
            {
                refused_seen[candidate] = true;
                const size_t used = strlen( refused_names );
                snprintf( refused_names + used, sizeof(refused_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( candidate ) );
            }
            continue;
        }

        // #109: a port-zero public address asks the KERNEL for an
        // ephemeral port, and the adoption below reads the granted port
        // back out of bind_address -- so the address is only meaningful on
        // a rung whose create discovers what it bound and hands it back.
        // The xdp rung does not: it takes an address STRING, binds the
        // port it is given, and leaves bind_address.port at zero, so the
        // adoption adopts zero and the transport silently advertises port
        // 0 -- tokens minted from rocketnet_server_transport_port() name
        // port 0, the cookie gate never matches, and nothing says why.
        //
        // This is asked of EVERY candidate rather than patched into the
        // one rung that fails it today, because #110 made create a loop
        // and the question is a property of each driver's bind
        // (rocketnet_driver_discovers_bound_port). It is a rung refusal
        // like any other: "auto" falls past xdp to a rung that discovers,
        // and only a PIN on a non-discovering rung refuses the create --
        // loudly, naming the rule, the way the pin law already works.

        if ( public_address.port == 0 && !rocketnet_driver_discovers_bound_port( candidate ) )
        {
            // named once however many times the operator listed it: the
            // ladder parse does not deduplicate, and "xdp, xdp" in an
            // error reads as a bug in the error rather than a repeat in
            // the list.
            //
            // keyed on the driver ID, not on a substring search of the
            // accumulated names. The hazard runs in the direction that is
            // easy to get backwards: strstr( "gso", "gso2" ) is NULL, so a
            // NEW longer name is not swallowed by a shorter accumulated
            // one -- but strstr( "gso2", "gso" ) FINDS, so an accumulated
            // longer name silently swallows a later legitimate shorter
            // one. No two names collide today; an id key cannot collide
            // whatever anyone adds.

            if ( candidate >= 0 && candidate < ROCKETNET_NUM_DRIVERS && !port_zero_refused_seen[candidate] )
            {
                port_zero_refused_seen[candidate] = true;

                const size_t used = strlen( port_zero_refused_names );
                snprintf( port_zero_refused_names + used, sizeof(port_zero_refused_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( candidate ) );
            }

            rocketnet_warn( "server transport %s refused: a port-zero public address asks the kernel for an ephemeral port, and the %s transport binds the port it is given and discovers nothing -- it would advertise port 0. give this server a real port, or leave a transport that discovers one in the list",
                            rocketnet_driver_name( candidate ), rocketnet_driver_name( candidate ) );
        }
        else
        {
            const rocketnet_driver_definition_t * definition = rocketnet_driver_definition( candidate );
            const rocketnet_server_driver_config_t driver_config = { rocketnet_driver_name(candidate), config, public_address_string };
            if ( rocketnet_server_driver_open( definition && definition->backend ? definition->backend->server : NULL, context,
                    &driver_config, &bind_address, &server_transport->io ) )
                granted_driver = candidate;
        }

        if ( granted_driver < 0 )
        {
            rocketnet_warn( "server transport %s refused%s", rocketnet_driver_name( candidate ),
                            i + 1 < ladder.num_candidates ? ". trying the next candidate" : "" );

            // deduped on the same rule as the port-zero list below it: a
            // repeated name in the operator's list is a repeat in the
            // list, not two refusals worth reading, and two adjacent
            // summary lines disagreeing about that reads as a bug in one
            // of them

            if ( candidate >= 0 && candidate < ROCKETNET_NUM_DRIVERS && !refused_seen[candidate] )
            {
                refused_seen[candidate] = true;

                const size_t used = strlen( refused_names );
                snprintf( refused_names + used, sizeof(refused_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( candidate ) );
            }
        }
    }

    if ( granted_driver < 0 )
    {
        char skipped_names[128];
        skipped_names[0] = '\0';
        for ( int i = 0; i < ladder.num_skipped; i++ )
        {
            const size_t used = strlen( skipped_names );
            snprintf( skipped_names + used, sizeof(skipped_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( ladder.skipped[i] ) );
        }

        rocketnet_error( "server transport ladder exhausted with no grant. refused: %s. skipped for platform: %s",
                         refused_names[0] != '\0' ? refused_names : "(none)",
                         skipped_names[0] != '\0' ? skipped_names : "(none)" );

        // #109 asked for an error naming the rule, and the per-rung warn
        // above is not that: a deployed binary and the test suite run at
        // the ERROR level, where a warn is invisible. So
        // the rule is restated HERE, at the level the failure itself is
        // reported -- on the fall it stays a warn, because a fall is not a
        // failure and an error line would be a lie about the outcome.

        if ( port_zero_refused_names[0] != '\0' )
        {
            // the remedy does NOT point at the per-rung warns: this error
            // exists precisely because the reader is at ERROR level, where
            // those warns are invisible. it has to stand alone.

            rocketnet_error( "the public address carries port zero, which asks the kernel for an ephemeral port: %s cannot discover the port it was granted and hand it back, so this server would advertise port 0. give this server a real port, or list a transport that discovers a granted port (%s). raise the log level to WARN to see why each candidate refused",
                             port_zero_refused_names, rocketnet_driver_discovering_names() );
        }

        if ( status )
            *status = ( port_zero_refused_names[0] != '\0' ) ? ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED
                                                             : ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED;
        rocketnet_server_transport_destroy( server_transport );
        return NULL;
    }

    server_transport->driver = granted_driver;

    if ( refused_names[0] != '\0' )
    {
        rocketnet_info( "server transport ladder granted %s after refusals: %s", rocketnet_driver_name( granted_driver ), refused_names );
    }

    // the started line names the granted tier -- performance claims attach
    // to what this says, never to what was requested

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const rocketnet_address_t * started_address = rocketnet_server_driver_has( server_transport->io, ROCKETNET_DRIVER_BINDS_PUBLIC_ADDRESS ) ? &public_address : &bind_address;
    rocketnet_info( "server transport started on %s [%s] (%s keys)", rocketnet_address_to_string( started_address, address_string ),
                    rocketnet_driver_name( granted_driver ), development_keys ? "development" : "production" );

    // a public address created with port zero adopts the granted bind
    // port, so the advertised address -- the one connect tokens are minted
    // for and validated against -- is the real one, and
    // rocketnet_server_transport_port reads it back

    if ( public_address.port == 0 )
        public_address.port = bind_address.port;

    server_transport->bind_address = bind_address;
    server_transport->public_address = public_address;
    server_transport->state = ROCKETNET_SERVER_TRANSPORT_RUNNING;
    server_transport->link_simulator_from = public_address;

    if ( config->simulated_loss_percent || config->simulated_latency_ms ||
         config->simulated_jitter_ms || config->simulated_duplicate_percent )
    {
        server_transport->link_simulator = rocketnet_network_simulator_create( context, 4096, UINT64_C(0x5345525645525349) );
        if ( !server_transport->link_simulator )
        {
            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
            rocketnet_server_transport_destroy( server_transport );
            return NULL;
        }
        if ( !rocketnet_platform_mutex_create( &server_transport->link_simulator_mutex ) )
        {
            if ( status )
                *status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;
            rocketnet_server_transport_destroy( server_transport );
            return NULL;
        }
        rocketnet_network_simulator_config( server_transport->link_simulator,
                                            config->simulated_latency_ms / 1000.0,
                                            config->simulated_jitter_ms / 1000.0,
                                            (float) config->simulated_loss_percent,
                                            (float) config->simulated_duplicate_percent );
    }

    // the protocol, on the configured identity, advertised at the public
    // address -- the address connect tokens are minted for

    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    protocol->protocol_id = config->protocol_id;
    protocol->send_rate = ROCKETNET_DEFAULT_SEND_RATE;
    memcpy( protocol->filter_value, config->filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( protocol->backend_public_key, config->backend_public_key, 32 );
    memcpy( protocol->server_public_key, config->server_public_key, 32 );
    memcpy( protocol->server_secret_key, config->server_secret_key, 32 );

    if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        protocol->advertised_ipv4 = public_address;
    }
    else
    {
        protocol->advertised_ipv6 = public_address;
        // a server created at the IPv6 wildcard is dual-stack: it serves
        // IPv4 as well, so it also advertises its IPv4 wildcard and IPv4
        // tokens for the loopback are accepted
        int wildcard = 1;
        for ( int i = 0; i < 8 && wildcard; i++ )
            if ( public_address.data.ipv6[i] != 0 )
                wildcard = 0;
        if ( wildcard )
        {
            memset( &protocol->advertised_ipv4, 0, sizeof(protocol->advertised_ipv4) );
            protocol->advertised_ipv4.type = ROCKETNET_ADDRESS_IPV4;
            protocol->advertised_ipv4.port = public_address.port;
        }
    }

    randombytes_buf( protocol->cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    sodium_memzero( protocol->previous_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    protocol->has_previous_cookie_key = 0;
    protocol->cookie_lifetime_seconds = ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS;
    protocol->cookie_rotation_seconds = ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS;
    protocol->cookie_key_rotation_time = server_transport->time + (double) protocol->cookie_rotation_seconds;
    protocol->previous_cookie_key_expire_time = 0.0;
    protocol->max_token_lifetime_seconds = ROCKETNET_DEFAULT_MAX_TOKEN_LIFETIME_SECONDS;
    protocol->handshake_deadline_seconds = config->handshake_deadline_seconds;
    protocol->max_timeout_seconds = config->max_timeout_seconds;
    protocol->allow_negative_timeout = ROCKETNET_DEFAULT_ALLOW_NEGATIVE_TIMEOUT;

    protocol->encryption_map = rocketnet_encryption_map_create( context, max_clients * ROCKETNET_ENCRYPTION_MAP_RATIO );
    protocol->used_token_history = rocketnet_used_token_history_create( context, max_clients * ROCKETNET_USED_TOKEN_HISTORY_RATIO );
    protocol->client_slots = rocketnet_client_slots_create( context, max_clients );
    protocol->acks = rocketnet_server_acks_create( context, max_clients );

    // the connection ids (docs/CHANNELS.md P9a, P9b): one per slot, 0 while
    // free, and one per acked batch entry, sized as the acked batch is
    protocol->connection_ids = (uint64_t*) rocketnet_malloc( context, (size_t) max_clients * sizeof(uint64_t) );
    protocol->acked_connection_ids = (uint64_t*) rocketnet_malloc( context, (size_t) max_clients * ROCKETNET_SENT_PACKET_BUFFER_SIZE * sizeof(uint64_t) );
    server_transport->client_wire_totals = (struct rocketnet_server_wire_totals_t*) rocketnet_malloc( context, (size_t) max_clients * sizeof(struct rocketnet_server_wire_totals_t) );
    server_transport->client_wire_totals_capacity = max_clients;
    if ( protocol->connection_ids )
        memset( protocol->connection_ids, 0, (size_t) max_clients * sizeof(uint64_t) );
    if ( server_transport->client_wire_totals )
        memset( server_transport->client_wire_totals, 0, (size_t) max_clients * sizeof(struct rocketnet_server_wire_totals_t) );
    // one reassembly set per connection slot, created here at create and
    // reached only from a connected slot (STANDARD.md _Fragmentation_).
    // the delivered mirror sizes to the server's completion bound: the
    // receive path drains one global array of up to
    // ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS packets per update with no
    // per-client share, and a slot clears at completion, so one client
    // completes more payloads in an update than it has slots. half the
    // drain plus the payloads begun in an earlier update (at most
    // connections x slots) bounds the completions, and at that capacity
    // BATCH_FULL stays zero under any load the drain delivers
    // (rocketnet_server_reassembly_delivered_capacity; finding D2 of
    // tla/README-FragmentsAndAcks.md). its bytes are what one update can
    // complete -- the drained fragments plus the payloads half-held in
    // slots -- not capacity x max payload
    // (rocketnet_server_reassembly_delivered_bytes).

    protocol->reassembly = rocketnet_reassembly_set_create_delivered( context, max_clients, num_reassembly_slots, server_transport->max_payload_bytes,
                                                                      rocketnet_server_reassembly_delivered_capacity( max_clients, num_reassembly_slots ),
                                                                      rocketnet_server_reassembly_delivered_bytes( max_clients, num_reassembly_slots, server_transport->max_payload_bytes ) );
    protocol->counters = &server_transport->counters;

    // the arena's slot stride stays 16-aligned so every root handed to a
    // caller is aligned for any word-based serializer

    server_transport->send_arena = rocketnet_send_arena_create( context, ROCKETNET_SEND_ARENA_SLOTS,
                                                               ( server_transport->max_payload_bytes + 15 ) & ~15 );

    if ( server_transport->io.operations->replay_mark )
        server_transport->replay_marks = rocketnet_server_replay_marks_create( context, max_clients );

    if ( !protocol->encryption_map || !protocol->used_token_history || !protocol->client_slots || !protocol->acks || !protocol->connection_ids || !protocol->acked_connection_ids || !server_transport->client_wire_totals || !protocol->reassembly || !server_transport->send_arena || ( server_transport->io.operations->replay_mark && !server_transport->replay_marks ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        rocketnet_error( "server transport create: could not allocate protocol storage" );
        rocketnet_server_transport_destroy( server_transport );
        return NULL;
    }

    // the env public_address is consumed only now, with the create whole

    if ( consuming_env_public_address )
    {
        bool expected = false;
        env_public_address_consumed.compare_exchange_strong( expected, true );
    }

    if ( status )
        *status = ROCKETNET_STATUS_OK;
    return server_transport;
}

void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );

    if ( server_transport->protocol.encryption_map )
        rocketnet_encryption_map_destroy( server_transport->protocol.encryption_map );
    if ( server_transport->protocol.used_token_history )
        rocketnet_used_token_history_destroy( server_transport->protocol.used_token_history );
    if ( server_transport->protocol.client_slots )
        rocketnet_client_slots_destroy( server_transport->protocol.client_slots );
    if ( server_transport->protocol.acks )
        rocketnet_server_acks_destroy( server_transport->protocol.acks );
    if ( server_transport->protocol.connection_ids )
        rocketnet_free( server_transport->context, server_transport->protocol.connection_ids );
    if ( server_transport->protocol.acked_connection_ids )
        rocketnet_free( server_transport->context, server_transport->protocol.acked_connection_ids );
    if ( server_transport->client_wire_totals )
        rocketnet_free( server_transport->context, server_transport->client_wire_totals );
    if ( server_transport->protocol.reassembly )
        rocketnet_reassembly_set_destroy( server_transport->protocol.reassembly );
    if ( server_transport->send_arena )
        rocketnet_send_arena_destroy( server_transport->send_arena );
    if ( server_transport->replay_marks )
        rocketnet_server_replay_marks_destroy( server_transport->replay_marks );
    if ( server_transport->link_simulator )
    {
        rocketnet_network_simulator_destroy( server_transport->link_simulator );
        rocketnet_platform_mutex_destroy( &server_transport->link_simulator_mutex );
    }

    // teardown branches on which socket is live, so a partial create --
    // any rung granted or none -- destroys cleanly

    rocketnet_server_driver_close( &server_transport->io );

    void * allocation_context = server_transport->context;
    server_transport->~rocketnet_server_transport_t();
    rocketnet_clear_and_free( allocation_context, server_transport, sizeof(rocketnet_server_transport_t) );
}

void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );
    server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPING;
}

int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return server_transport->state;
}

uint16_t rocketnet_server_transport_port( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return server_transport->bind_address.port;
}

// the granted-tier report: the driver that won the ladder at create, by
// name -- what performance claims and operator errands attach to

const char * rocketnet_server_transport_driver( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return NULL;
    rocketnet_assert( server_transport );
    return rocketnet_driver_name( server_transport->driver );
}

// process the previous stats frames into published per-second numbers, then
// advance the bucket when enough time has passed. The sum walks the fixed
// ring every update at bounded cost, and only buckets younger than one
// second contribute, so the published numbers are "per second" by window
// rather than by division.

static void rocketnet_server_update_stats( struct rocketnet_server_transport_t * server_transport, double time )
{
    // process previous stats frames
    {
        uint64_t total_packets_sent = 0;
        uint64_t total_packets_received = 0;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;

        for ( int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; i++ )
        {
            // IMPORTANT: only sum the last second worth of stats frames, we want "packets sent per-second, bandwidth sent per-second etc..."
            if ( server_transport->stats_frames[i].frame == 0 || server_transport->stats_frames[i].time + 1.0 < time )
                continue;

            total_packets_sent += server_transport->stats_frames[i].packets_sent;
            total_packets_received += server_transport->stats_frames[i].packets_received;
            total_bytes_sent += server_transport->stats_frames[i].bytes_sent;
            total_bytes_received += server_transport->stats_frames[i].bytes_received;
        }

        server_transport->server_stats.packets_sent_per_second = total_packets_sent;
        server_transport->server_stats.packets_received_per_second = total_packets_received;

        server_transport->server_stats.bandwidth_sent_kbps = (float) ( ( (double) total_bytes_sent * 8.0 ) / 1000.0 );
        server_transport->server_stats.bandwidth_received_kbps = (float) ( ( (double) total_bytes_received * 8.0 ) / 1000.0 );
    }

    // setup next stats frame
    {
        server_transport->frame++;
        server_transport->current_stats_index = rocketnet_stats_window_advance(
            server_transport->stats_frames, server_transport->current_stats_index,
            server_transport->frame, time);
    }
}

// The admission gate, consulted by the receive loops before a packet is
// handed to the protocol. A source with a mapping has already cleared the
// connect ladder and bypasses the gate; a callback that says DROP stops an
// unmapped packet before any protocol work -- and any crypto -- runs. The
// test drives this directly to pin the drop-before-verify contract.
int rocketnet_server_transport_admission( const struct rocketnet_server_protocol_t * protocol,
                                          rocketnet_server_admission_callback_t callback,
                                          void * context,
                                          const struct rocketnet_address_t * from,
                                          const uint8_t * packet_data,
                                          int packet_bytes )
{
    if ( !callback || rocketnet_encryption_map_find( protocol->encryption_map, from ) != -1 )
        return ROCKETNET_SERVER_ADMISSION_ADMIT;
    return callback( context, from, packet_data, packet_bytes );
}

void rocketnet_server_transport_set_admission_callback( struct rocketnet_server_transport_t * server_transport, rocketnet_server_admission_callback_t callback, void * context )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );
    server_transport->admission_callback = callback;
    server_transport->admission_context = context;
}

void rocketnet_server_transport_set_test_timestamp( struct rocketnet_server_transport_t * server_transport, uint64_t timestamp )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( timestamp != 0 );
    server_transport->test_timestamp = timestamp;
}

// tla/RNDriverSendStaging.tla Update
void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport, double time )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );

    // the caller's clock is clamped once, before anything reads it: a NaN,
    // an infinity or a step backwards becomes the last good time, counted and
    // loud in a debug build. the last good time starts at zero, the value a
    // value-initialized transport carries.

    bool time_clamped = false;
    time = rocketnet_clamp_update_time( server_transport->time, time, &time_clamped );

    server_transport->packet_leases.reset();

    server_transport->time = time;
    rocketnet_server_link_drain( server_transport );

    rocketnet_server_update_stats( server_transport, time );

    if ( server_transport->state == ROCKETNET_SERVER_TRANSPORT_STOPPING )
    {
        if ( !rocketnet_server_driver_stop( &server_transport->io ) )
            return;
        server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPED;
        return;
    }

    const double current_time = time;
    const uint64_t current_timestamp = server_transport->test_timestamp ? server_transport->test_timestamp : ( server_transport->game_clock_epoch != 0 ?
        server_transport->game_clock_epoch + (uint64_t) current_time : (uint64_t) ::time( NULL ) );

    server_transport->payloads.num_payloads = 0;

    // the acked-packet batch drains per update: what the blocks processed
    // below acknowledge lands here, valid until the next update

    server_transport->protocol.acks->num_acked = 0;

    // the reassembly's delivered window is per update: payloads completed
    // last update are invalid from here

    rocketnet_reassembly_set_begin_update( server_transport->protocol.reassembly );

    // receive and process, responses sealed straight into send frames

    if ( rocketnet_server_transport_staged( server_transport ) )
    {

    struct rocketnet_server_socket_process_packets_t * received = server_transport->io.operations->receive( server_transport->io.instance );

    const int stats_index = server_transport->current_stats_index;

    for ( int i = 0; i < received->num_packets; i++ )
    {
        server_transport->stats_frames[stats_index].packets_received++;
        server_transport->stats_frames[stats_index].bytes_received += (uint64_t) ( received->packet_bytes[i] + rocketnet_wire_header_bytes( received->from[i].type ) );

        const int wire_client_before = rocketnet_server_wire_client_index( server_transport, &received->from[i] );
        rocketnet_server_wire_totals_add_received( server_transport, wire_client_before, &received->from[i], received->packet_bytes[i] );

        // the admission gate: an unmapped source goes through the application's
        // callback before any send frame is reserved or protocol work runs, so
        // a drop costs no crypto and no staging
        if ( rocketnet_server_transport_admission( &server_transport->protocol, server_transport->admission_callback, server_transport->admission_context, &received->from[i], received->packet_data[i], received->packet_bytes[i] ) == ROCKETNET_SERVER_ADMISSION_DROP )
            continue;

        uint8_t * payload = server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE ?
            server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES : NULL;

        uint8_t payload_overflow[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        struct rocketnet_server_frame_t frame;
        uint8_t * response = rocketnet_server_reserve_send_frame( server_transport, &received->from[i], &frame );
        uint8_t response_overflow[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;

        if ( response )
            response += ROCKETNET_FRAME_WIRE_OFFSET;

        const int outcome = rocketnet_server_protocol_process_packet( &server_transport->protocol, &received->from[i],
                                         received->packet_data[i], received->packet_bytes[i],
                                         current_timestamp, current_time,
                                         response ? response : response_overflow, &response_bytes,
                                         payload ? payload : payload_overflow, &payload_bytes, &payload_client_index );

        // a packet that opened under a connection's keys teaches the driver
        // its link-layer source (the xdp reply-MAC rule, DRIVERS.md); a
        // dropped or unauthenticated packet (a cookie or a challenge answer)
        // teaches nothing
        if ( ( outcome == ROCKETNET_SERVER_PROCESS_ACCEPTED || outcome == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE ) && server_transport->io.operations->peer_accepted )
            server_transport->io.operations->peer_accepted( server_transport->io.instance, i );

        int wire_client_after = wire_client_before;
        if ( wire_client_before < 0 )
        {
            wire_client_after = rocketnet_server_wire_client_index( server_transport, &received->from[i] );
            if ( wire_client_after >= 0 )
                rocketnet_server_wire_totals_assign( server_transport, wire_client_after, &received->from[i] );
        }
        frame.client_index = wire_client_after;

        if ( response )
        {
            if ( response_bytes > 0 )
                rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
            else
                rocketnet_server_abort_send_frame( server_transport, &frame );
        }

        if ( payload && payload_bytes > 0 )
        {
            const int index = server_transport->payloads.num_payloads;
            server_transport->payloads.client_index[index] = payload_client_index;
            server_transport->payloads.connection_id[index] = server_transport->protocol.payload_connection_id;
            server_transport->payloads.payload_data[index] = payload;
            server_transport->payloads.payload_bytes[index] = payload_bytes;
            server_transport->payloads.num_payloads++;
        }

        // a fragment that completed its payload delivers here, in arrival
        // position: the reassembled bytes live in the reassembly's
        // delivered arena, valid until the next update, and join the same
        // batch attributed to their client slot

        struct rocketnet_reassembly_set_t * reassembly = server_transport->protocol.reassembly;
        if ( reassembly->completed_bytes > 0 )
        {
            if ( server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE )
            {
                const int index = server_transport->payloads.num_payloads;
                server_transport->payloads.client_index[index] = payload_client_index;
                server_transport->payloads.connection_id[index] = server_transport->protocol.payload_connection_id;
                server_transport->payloads.payload_data[index] = reassembly->completed_payload;
                server_transport->payloads.payload_bytes[index] = reassembly->completed_bytes;
                server_transport->payloads.num_payloads++;
            }
            else
            {
                rocketnet_spam( "server dropped reassembled payload. receive batch full" );
                server_transport->counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL]++;
            }
        }
    }

    }
    else
    {

    // Drain the receive queue up to this update's packet budget. Each read
    // takes whatever the endpoint has queued, up to the remaining budget, and
    // is processed before the next read; what is left past the budget stays
    // in the transport's queue for the next update, as before. The budget is
    // ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * ROCKETNET_MAX_CLIENTS (16 * 1000
    // = 16000), which equals the payload-batch capacity, so all packets read
    // in one update fit the payload batch that feeds the application.

    struct rocketnet_server_receive_buffer_t * receive_buffer = &server_transport->receive_buffer;

    const int stats_index = server_transport->current_stats_index;
    int packets_processed = 0;
    int batch_capacity = 0;
    int num_received = 0;

    do
    {
        const int remaining_budget = ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE - packets_processed;
        batch_capacity = remaining_budget;
        num_received = server_transport->io.operations->receive_batch( server_transport->io.instance,
                                                                        receive_buffer->from,
                                                                        receive_buffer->data,
                                                                        ROCKETNET_RECEIVE_CEILING,
                                                                        receive_buffer->packet_bytes,
                                                                        batch_capacity );

        for ( int i = 0; i < num_received; i++ )
        {
            struct rocketnet_address_t from = receive_buffer->from[i];
            uint8_t * receive_frame = receive_buffer->data + i * ROCKETNET_RECEIVE_CEILING;
            const int packet_bytes = receive_buffer->packet_bytes[i];

            server_transport->stats_frames[stats_index].packets_received++;
            server_transport->stats_frames[stats_index].bytes_received += (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( from.type ) );

            const int wire_client_before = rocketnet_server_wire_client_index( server_transport, &from );
            rocketnet_server_wire_totals_add_received( server_transport, wire_client_before, &from, packet_bytes );

            // the admission gate: an unmapped source goes through the application's
            // callback before any send frame is reserved or protocol work runs, so
            // a drop costs no crypto and no staging
            if ( rocketnet_server_transport_admission( &server_transport->protocol, server_transport->admission_callback, server_transport->admission_context, &from, receive_frame, packet_bytes ) == ROCKETNET_SERVER_ADMISSION_DROP )
                continue;

            uint8_t * payload = server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE ?
                server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES : NULL;

            uint8_t payload_overflow[ROCKETNET_RECEIVE_CEILING];
            int payload_bytes = 0;
            int payload_client_index = -1;

            struct rocketnet_server_frame_t frame;
            uint8_t * response = rocketnet_server_reserve_send_frame( server_transport, &from, &frame );
            uint8_t response_overflow[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0;

            if ( response )
                response += ROCKETNET_FRAME_WIRE_OFFSET;

            rocketnet_server_protocol_process_packet( &server_transport->protocol, &from, receive_frame, packet_bytes,
                                             current_timestamp, current_time,
                                             response ? response : response_overflow, &response_bytes,
                                             payload ? payload : payload_overflow, &payload_bytes, &payload_client_index );

            int wire_client_after = wire_client_before;
            if ( wire_client_before < 0 )
            {
                wire_client_after = rocketnet_server_wire_client_index( server_transport, &from );
                if ( wire_client_after >= 0 )
                    rocketnet_server_wire_totals_assign( server_transport, wire_client_after, &from );
            }
            frame.client_index = wire_client_after;

            if ( response )
            {
                if ( response_bytes > 0 )
                    rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
                else
                    rocketnet_server_abort_send_frame( server_transport, &frame );
            }

            if ( payload && payload_bytes > 0 )
            {
                const int index = server_transport->payloads.num_payloads;
                server_transport->payloads.client_index[index] = payload_client_index;
                server_transport->payloads.connection_id[index] = server_transport->protocol.payload_connection_id;
                server_transport->payloads.payload_data[index] = payload;
                server_transport->payloads.payload_bytes[index] = payload_bytes;
                server_transport->payloads.num_payloads++;
            }

            // a fragment that completed its payload delivers here, in arrival
            // position: the reassembled bytes live in the reassembly's
            // delivered arena, valid until the next update, and join the same
            // batch attributed to their client slot

            struct rocketnet_reassembly_set_t * reassembly = server_transport->protocol.reassembly;
            if ( reassembly->completed_bytes > 0 )
            {
                if ( server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE )
                {
                    const int index = server_transport->payloads.num_payloads;
                    server_transport->payloads.client_index[index] = payload_client_index;
                    server_transport->payloads.connection_id[index] = server_transport->protocol.payload_connection_id;
                    server_transport->payloads.payload_data[index] = reassembly->completed_payload;
                    server_transport->payloads.payload_bytes[index] = reassembly->completed_bytes;
                    server_transport->payloads.num_payloads++;
                }
                else
                {
                    rocketnet_spam( "server dropped reassembled payload. receive batch full" );
                    server_transport->counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL]++;
                }
            }
        }
        packets_processed += num_received;
    } while ( num_received == batch_capacity && packets_processed < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE );

    }

    // the sweep, then per client: the paced keep-alive or -- when the ack
    // debt has reached the threshold with no other block going out -- the
    // standalone ack, into send frames. a sixth of the slots also recompute
    // their published statistics, every slot covered over six updates.

    rocketnet_server_protocol_update( &server_transport->protocol, current_time );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    const int max_clients = rocketnet_client_slots_max_clients( server_transport->protocol.client_slots );
    // Statistics must cover every slot even when the driver's send pool is
    // exhausted. Their six-update schedule is independent of send progress.
    for ( int i = 0; i < max_clients; i++ )
        if ( ( (uint64_t) ( i % ROCKETNET_CLIENT_STATS_UPDATE_PHASES ) ) == ( acks->update_count % ROCKETNET_CLIENT_STATS_UPDATE_PHASES ) )
            rocketnet_ack_endpoint_update_stats( &acks->endpoints[i] );

    const int first_client = server_transport->maintenance_next_client;
    for ( int offset = 0; offset < max_clients; offset++ )
    {
        const int i = (first_client + offset) % max_clients;
        server_transport->maintenance_next_client = i;
        if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, i ) )
            continue;
        int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, i );
        const rocketnet_address_t * client_address = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, client_address, &frame );
        if ( !frame_data )
            break;
        frame.client_index = i;
        int send_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server_transport->protocol, i, current_time, frame_data + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( send_bytes == 0 )
            send_bytes = rocketnet_server_protocol_write_ack_if_due( &server_transport->protocol, i, current_time, frame_data + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( send_bytes > 0 )
            rocketnet_server_commit_send_frame( server_transport, &frame, send_bytes );
        else
            rocketnet_server_abort_send_frame( server_transport, &frame );
        server_transport->maintenance_next_client = (i + 1) % max_clients;
    }

    acks->update_count++;

    // The kernel replay walk and commit: after the maintenance sweep, the
    // transport tells the granted driver which client addresses to forget and
    // mark, then commits them. tla/RNXdpReplay.tla Walk and Commit.
    if ( server_transport->io.operations->replay_mark )
    {
        struct rocketnet_server_replay_sink_t replay_sink;
        replay_sink.context = server_transport->io.instance;
        replay_sink.forget = server_transport->io.operations->replay_forget;
        replay_sink.mark = server_transport->io.operations->replay_mark;
        rocketnet_server_replay_marks_walk( server_transport->replay_marks, server_transport->protocol.client_slots,
                                           server_transport->protocol.encryption_map, &replay_sink );
        server_transport->io.operations->replay_commit( server_transport->io.instance );
    }

    // Drivers with FLUSH_ON_UPDATE publish update-generated responses here.
    // Other staged drivers publish at send_packets after the joined fill phase.
    // XDP retains published generations until the worker drains them; the
    // staged/flushed counters avoid redundant driver calls between fills.

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        server_transport->send_staged++;

        if ( rocketnet_server_driver_has( server_transport->io, ROCKETNET_DRIVER_FLUSH_ON_UPDATE ) )
        {
            server_transport->send_flushed = server_transport->send_staged;
            rocketnet_server_driver_send_packets( server_transport );
        }

        return;
    }

    rocketnet_server_flush_control_buffer( server_transport );
}

int rocketnet_server_transport_num_payloads( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return server_transport->payloads.num_payloads;
}

int rocketnet_server_transport_payloads( struct rocketnet_server_transport_t * server_transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( client_index ) )
        return 0;
    if ( !rocketnet_api_null_guard( payload_data ) )
        return 0;
    if ( !rocketnet_api_null_guard( payload_bytes ) )
        return 0;
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( payload_data );
    rocketnet_assert( payload_bytes );

    *client_index = server_transport->payloads.client_index;
    *payload_data = server_transport->payloads.payload_data;
    *payload_bytes = server_transport->payloads.payload_bytes;

    return server_transport->payloads.num_payloads;
}

// docs/CHANNELS.md P9b: the connection id of each payload entry, parallel to
// the client index array above

int rocketnet_server_transport_payload_connection_ids( struct rocketnet_server_transport_t * server_transport, const uint64_t ** connection_id )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( connection_id ) )
        return 0;
    rocketnet_assert( server_transport );
    rocketnet_assert( connection_id );

    *connection_id = server_transport->payloads.connection_id;

    return server_transport->payloads.num_payloads;
}

uint8_t * rocketnet_server_transport_payload( struct rocketnet_server_transport_t * server_transport, int payload_index, int * client_index, int * payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return NULL;
    if ( !rocketnet_api_null_guard( client_index ) )
        return NULL;
    if ( !rocketnet_api_null_guard( payload_bytes ) )
        return NULL;
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( payload_bytes );

    struct rocketnet_server_payloads_t * payloads = &server_transport->payloads;

    if ( payload_index < 0 || payload_index >= payloads->num_payloads )
    {
        *client_index = -1;
        *payload_bytes = 0;
        return NULL;
    }

    *client_index = payloads->client_index[payload_index];
    *payload_bytes = payloads->payload_bytes[payload_index];
    return payloads->payload_data[payload_index];
}

// The acquire/commit send surface. Phase contract, which is what makes the
// whole family lock-free: rocketnet_server_transport_update and
// rocketnet_server_transport_send_packets run on the main thread; start, finish and
// abort may run on any number of game threads strictly between them. During
// that fill phase connections are stable -- only update creates or tears
// down mappings -- so acquisition is one atomic add for the slot and one
// for the sequence, sealing runs on the caller's thread in place in the
// frame, and the flush walks finished slots single-threaded.

static void rocketnet_server_transport_abort_packet_unchecked( rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, uint64_t backend_id );

uint8_t * rocketnet_server_transport_start_packet( struct rocketnet_server_transport_t * server_transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return NULL;
    if ( !rocketnet_api_null_guard( context ) )
        return NULL;
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    memset( context, 0, sizeof(struct rocketnet_packet_context_t) );

    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return NULL;

    if ( request_bytes < 1 || request_bytes > server_transport->max_payload_bytes )
    {
        rocketnet_spam( "server start packet refused. request out of range" );
        return NULL;
    }

    rocketnet_address_t to;
    uint64_t sequence;
    if ( !rocketnet_server_protocol_start_payload( &server_transport->protocol, client_index, &to, &sequence ) )
        return NULL;

    uint8_t * frame = NULL;

    if ( request_bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        // above the wire's capacity: the arena backs the write, and
        // finish_packet splits into fragments -- or collapses to one wire
        // packet when the write turns out small. The lease supplies its
        // public packet id independently of the driver.

        frame = rocketnet_send_arena_acquire( server_transport->send_arena );
        if ( !frame )
        {
            rocketnet_spam( "server send arena full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_ARENA_FULL], 1 );
            return NULL;
        }

        rocketnet_assert( ( (uintptr_t) frame % 16 ) == 0 );

        context->frame = frame;
        context->payload_index = 0;
        context->payload_capacity = server_transport->max_payload_bytes;
        context->packet_id = 0; // private lease supplies the public identifier
        context->sequence = sequence;
        context->client_index = client_index;

        if ( !server_transport->packet_leases.insert(*context) )
        {
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_LEASE_FULL], 1 );
            rocketnet_server_transport_abort_packet_unchecked( server_transport, context, context->packet_id );
            return NULL;
        }
        return frame;
    }

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        uint64_t packet_id;
        frame = rocketnet_server_driver_start_packet( server_transport, &to, &packet_id );
        if ( !frame )
        {
            if ( packet_id != ROCKETNET_DRIVER_PACKET_UNREACHABLE )
                rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return NULL;
        }
        rocketnet_platform_atomic_add_u64( &server_transport->send_staged, 1 );
        context->packet_id = packet_id;
    }
    else
    {
        struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

        uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
        if ( slot >= ROCKETNET_SERVER_PAYLOAD_RING_SIZE )
        {
            rocketnet_spam( "server payload ring full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return NULL;
        }

        ring->info[slot].to = to;
        ring->info[slot].client_index = client_index;
        ring->info[slot].wire_bytes = 0;

        frame = ring->data + slot * ROCKETNET_FRAME_STRIDE;

        // the packet id is the join key the acked batch surfaces: unique and
        // monotonic, never a ring slot. the slot is recovered from the frame
        // pointer wherever it is needed.

        context->packet_id = 0; // private lease supplies the public identifier
    }

    rocketnet_assert( ( (uintptr_t) frame % 16 ) == 0 );

    context->frame = frame;
    context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET;
    context->payload_capacity = ROCKETNET_MAX_PAYLOAD_BYTES;
    context->sequence = sequence;
    context->client_index = client_index;

    if ( !server_transport->packet_leases.insert(*context) )
    {
        rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_LEASE_FULL], 1 );
        rocketnet_server_transport_abort_packet_unchecked( server_transport, context, context->packet_id );
        return NULL;
    }
    return frame;
}

static void rocketnet_server_transport_abort_packet_unchecked( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, uint64_t backend_id )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return;

    if ( rocketnet_send_arena_owns( server_transport->send_arena, context->frame ) )
    {
        // an arena slot needs no undo: the flush reclaims the whole arena
        context->frame = NULL;
        return;
    }

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_driver_abort_packet( server_transport, backend_id, context->frame );
    }
    else
    {
        const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
        server_transport->payload_ring.info[slot].wire_bytes = 0;
    }

    context->frame = NULL;
}

uint8_t * rocketnet_server_transport_packet_payload( rocketnet_server_transport_t * transport, const rocketnet_packet_context_t * context, int * capacity )
{
    if ( !rocketnet_api_null_guard( transport ) )
        return NULL;
    if ( !rocketnet_api_null_guard( context ) )
        return NULL;
    if ( !rocketnet_api_null_guard( capacity ) )
        return NULL;
    if (capacity) *capacity = 0;
    if (!transport || !context || !capacity) return NULL;
    rocketnet_packet_context_t trusted;
    uint64_t backend_id = 0;
    if (!transport->packet_leases.resolve(*context, trusted, backend_id, false)) return NULL;
    *capacity = trusted.payload_capacity;
    return trusted.frame + trusted.payload_index;
}

void rocketnet_server_transport_abort_packet( rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    if ( !rocketnet_api_null_guard( context ) )
        return;
    if ( !server_transport || !context )
        return;
    rocketnet_packet_context_t trusted;
    uint64_t backend_id;
    if ( server_transport->packet_leases.take(*context, trusted, backend_id) )
        rocketnet_server_transport_abort_packet_unchecked( server_transport, &trusted, backend_id );
    context->frame = NULL;
}

// The fill-phase wire frame behind the arena finish: one sealed packet's
// destination on any driver, reserved before sealing so a split goes
// out whole or not at all. On the portable driver the frame is a payload
// ring slot; on the staged drivers it is a driver frame with its own packet
// id for the driver's bookkeeping -- the application's join key stays the
// context's.

struct rocketnet_server_fill_frame_t
{
    uint8_t * data;
    uint64_t socket_packet_id;      // the staged drivers' bookkeeping id
    uint64_t slot;                  // the portable payload ring slot
    int client_index;
    rocketnet_address_t to;
};

static int rocketnet_server_reserve_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                                const rocketnet_address_t * to, int client_index,
                                                struct rocketnet_server_fill_frame_t * frame )
{
    frame->client_index = client_index;
    frame->to = *to;

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        frame->data = rocketnet_server_driver_start_packet( server_transport, to, &frame->socket_packet_id );
        if ( !frame->data )
        {
            if ( frame->socket_packet_id != ROCKETNET_DRIVER_PACKET_UNREACHABLE )
                rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return 0;
        }
        rocketnet_platform_atomic_add_u64( &server_transport->send_staged, 1 );
        return 1;
    }

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    frame->slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( frame->slot >= ROCKETNET_SERVER_PAYLOAD_RING_SIZE )
    {
        rocketnet_spam( "server payload ring full. packet dropped" );
        rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        frame->data = NULL;
        return 0;
    }

    ring->info[frame->slot].to = *to;
    ring->info[frame->slot].client_index = client_index;
    ring->info[frame->slot].wire_bytes = 0;
    frame->data = ring->data + frame->slot * ROCKETNET_FRAME_STRIDE;
    return 1;
}

static void rocketnet_server_commit_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                                struct rocketnet_server_fill_frame_t * frame, int wire_bytes )
{
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        if ( server_transport->link_simulator )
        {
            rocketnet_server_link_send( server_transport, &frame->to,
                                        frame->data + ROCKETNET_FRAME_WIRE_OFFSET, wire_bytes );
            rocketnet_server_driver_abort_packet( server_transport, frame->socket_packet_id, frame->data );
        }
        else
            rocketnet_server_driver_finish_packet( server_transport, frame->socket_packet_id, frame->data, wire_bytes );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_packets_sent, 1 );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_bytes_sent, (uint64_t) ( wire_bytes + rocketnet_wire_header_bytes( frame->to.type ) ) );
        rocketnet_server_wire_totals_add_sent_atomic( server_transport, frame->client_index, frame->to.type, wire_bytes );
        return;
    }
    server_transport->payload_ring.info[frame->slot].wire_bytes = wire_bytes;
}

static void rocketnet_server_abort_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                               struct rocketnet_server_fill_frame_t * frame )
{
    if ( !frame->data )
        return;
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_driver_abort_packet( server_transport, frame->socket_packet_id, frame->data );
        frame->data = NULL;
        return;
    }
    server_transport->payload_ring.info[frame->slot].wire_bytes = 0;
    frame->data = NULL;
}

// The arena finish: the request was over the wire's capacity, so the
// payload sits contiguous in an arena slot. A write that turned out to fit
// one packet collapses to a single ordinary payload -- one copy into one
// wire frame, cheaper than fragmenting -- and a genuinely large one splits
// into type 9 fragments. Every wire frame is reserved before anything
// seals, so a reservation shortfall drops the payload whole; a seal
// refusal mid-split -- possible only through a fill-contract violation,
// since connections are stable during a fill -- withdraws every reserved
// frame on the portable and gso drivers, whose staging is untouched until
// the main-thread flush, while frames already committed to the xdp socket
// stay committed there (its worker drains staging asynchronously), a
// partial payload the semantics already price as fragments lost on the
// wire. Returns the total wire bytes staged.

static int rocketnet_server_transport_finish_arena_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    rocketnet_address_t to;
    if ( !rocketnet_server_protocol_client_address( &server_transport->protocol, context->client_index, &to ) )
    {
        context->frame = NULL;
        return 0;
    }

    if ( payload_bytes <= ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        struct rocketnet_server_fill_frame_t frame;
        if ( !rocketnet_server_reserve_fill_frame( server_transport, &to, context->client_index, &frame ) )
        {
            context->frame = NULL;
            return 0;
        }

        memcpy( frame.data + ROCKETNET_FRAME_PAYLOAD_OFFSET, context->frame, (size_t) payload_bytes );

        int wire_bytes = rocketnet_server_protocol_seal_payload_frame( &server_transport->protocol, context->client_index,
                                                                       context->sequence, context->packet_id,
                                                                       frame.data + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                       payload_bytes, server_transport->time );
        if ( wire_bytes == 0 )
        {
            rocketnet_server_abort_fill_frame( server_transport, &frame );
            context->frame = NULL;
            return 0;
        }

        rocketnet_server_commit_fill_frame( server_transport, &frame, wire_bytes );
        context->frame = NULL;
        return wire_bytes;
    }

    int fragment_count, fragment_stride;
    if ( !rocketnet_fragment_plan( payload_bytes, &fragment_count, &fragment_stride ) )
    {
        context->frame = NULL;
        return 0;
    }

    struct rocketnet_server_fill_frame_t frames[ROCKETNET_MAX_FRAGMENTS];
    for ( int i = 0; i < fragment_count; i++ )
    {
        if ( !rocketnet_server_reserve_fill_frame( server_transport, &to, context->client_index, &frames[i] ) )
        {
            // all or nothing: a partial payload is a dead payload, so the
            // frames already reserved go back
            rocketnet_spam( "server dropped fragmented packet. could not reserve every wire frame" );
            for ( int j = 0; j < i; j++ )
                rocketnet_server_abort_fill_frame( server_transport, &frames[j] );
            context->frame = NULL;
            return 0;
        }
    }

    // fragment 0 rides the sequence start_packet burned; the rest reserve
    // a block. the payload's one packet id lives in the aggregate, and
    // surfaces when every fragment's wire packet is acknowledged.

    uint64_t base_sequence = 0;
    if ( !rocketnet_server_protocol_start_fragments( &server_transport->protocol, context->client_index, fragment_count - 1, &base_sequence ) )
    {
        for ( int i = 0; i < fragment_count; i++ )
            rocketnet_server_abort_fill_frame( server_transport, &frames[i] );
        context->frame = NULL;
        return 0;
    }

    uint32_t payload_id = rocketnet_server_protocol_next_fragment_payload_id( &server_transport->protocol, context->client_index );
    uint64_t aggregate = rocketnet_ack_endpoint_open_aggregate( &server_transport->protocol.acks->endpoints[context->client_index],
                                                                context->packet_id, fragment_count );

    int total_wire_bytes = 0;
    for ( int i = 0; i < fragment_count; i++ )
    {
        const uint64_t sequence = i == 0 ? context->sequence : base_sequence + (uint64_t) ( i - 1 );
        const int fragment_bytes = i < fragment_count - 1 ? fragment_stride : payload_bytes - fragment_stride * ( fragment_count - 1 );
        int wire_bytes = rocketnet_server_protocol_seal_fragment( &server_transport->protocol, context->client_index,
                                                                  sequence, payload_id, i, fragment_count,
                                                                  context->frame + i * fragment_stride, fragment_bytes,
                                                                  aggregate, server_transport->time,
                                                                  frames[i].data + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( wire_bytes == 0 )
        {
            // the xdp socket's staging drains asynchronously, so frames
            // already committed to it stay committed (fragments lost on
            // the wire, priced by the semantics); the portable and gso
            // stagings are untouched until the main-thread flush, so the
            // whole reservation withdraws

            const int first_withdrawn = rocketnet_server_driver_has( server_transport->io, ROCKETNET_DRIVER_FINISH_FINAL ) ? i : 0;
            for ( int j = first_withdrawn; j < fragment_count; j++ )
                rocketnet_server_abort_fill_frame( server_transport, &frames[j] );
            context->frame = NULL;
            return 0;
        }
        rocketnet_server_commit_fill_frame( server_transport, &frames[i], wire_bytes );
        total_wire_bytes += wire_bytes;
    }

    context->frame = NULL;
    return total_wire_bytes;
}

static int rocketnet_server_transport_finish_packet_unchecked( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes, uint64_t backend_id )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return 0;

    if ( payload_bytes < 1 || payload_bytes > context->payload_capacity )
    {
        rocketnet_server_transport_abort_packet_unchecked( server_transport, context, backend_id );
        return 0;
    }

    if ( rocketnet_send_arena_owns( server_transport->send_arena, context->frame ) )
        return rocketnet_server_transport_finish_arena_packet( server_transport, context, payload_bytes );

    int wire_bytes = rocketnet_server_protocol_seal_payload_frame( &server_transport->protocol, context->client_index,
                                                                   context->sequence, context->packet_id,
                                                                   context->frame + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                   payload_bytes, server_transport->time );
    if ( wire_bytes == 0 )
    {
        rocketnet_server_transport_abort_packet_unchecked( server_transport, context, backend_id );
        return 0;
    }

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        const int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, context->client_index );
        const rocketnet_address_t * to = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );
        if ( server_transport->link_simulator )
        {
            rocketnet_server_link_send( server_transport, to,
                                        context->frame + ROCKETNET_FRAME_WIRE_OFFSET, wire_bytes );
            rocketnet_server_driver_abort_packet( server_transport, backend_id, context->frame );
        }
        else
            rocketnet_server_driver_finish_packet( server_transport, backend_id, context->frame, wire_bytes );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_packets_sent, 1 );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_bytes_sent, (uint64_t) ( wire_bytes + rocketnet_wire_header_bytes( to ? to->type : ROCKETNET_ADDRESS_NONE ) ) );
        rocketnet_server_wire_totals_add_sent_atomic( server_transport, context->client_index,
                                                      to ? to->type : ROCKETNET_ADDRESS_NONE, wire_bytes );
    }
    else
    {
        const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
        server_transport->payload_ring.info[slot].wire_bytes = wire_bytes;
    }

    context->frame = NULL;

    return wire_bytes;
}

int rocketnet_server_transport_finish_packet( rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( context ) )
        return 0;
    if ( !server_transport || !context )
        return 0;
    rocketnet_packet_context_t trusted;
    uint64_t backend_id;
    if ( !server_transport->packet_leases.take(*context, trusted, backend_id) )
    {
        context->frame = NULL;
        return 0;
    }
    const int result = rocketnet_server_transport_finish_packet_unchecked( server_transport, &trusted, payload_bytes, backend_id );
    context->frame = NULL;
    return result;
}

// tla/RNDriverSendStaging.tla SendPackets
void rocketnet_server_transport_send_packets( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );
    server_transport->packet_leases.reset();

    const int stats_index = server_transport->current_stats_index;

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        // fold the fill phase's payload sends into the current bucket. the fill
        // is closed before this call by the threading contract, so the pending
        // pair is quiescent and a plain read-and-clear is safe.

        server_transport->stats_frames[stats_index].packets_sent += server_transport->stats_pending_packets_sent;
        server_transport->stats_frames[stats_index].bytes_sent += server_transport->stats_pending_bytes_sent;
        server_transport->stats_pending_packets_sent = 0;
        server_transport->stats_pending_bytes_sent = 0;

        // Publish only work staged since the previous driver flush.

        if ( server_transport->send_staged != server_transport->send_flushed )
        {
            server_transport->send_flushed = server_transport->send_staged;
            rocketnet_server_driver_send_packets( server_transport );
        }

        rocketnet_send_arena_reset( server_transport->send_arena );
        return;
    }

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    uint64_t num_packets = ring->num_packets;
    if ( num_packets > ROCKETNET_SERVER_PAYLOAD_RING_SIZE )
        num_packets = ROCKETNET_SERVER_PAYLOAD_RING_SIZE;

    struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
    int num_sends = 0;

    for ( uint64_t i = 0; i < num_packets; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            // the payload accumulation rides the flush walk: the ring already
            // holds every committed packet's wire size, so this costs nothing

            server_transport->stats_frames[stats_index].packets_sent++;
            server_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + rocketnet_wire_header_bytes( ring->info[i].to.type ) );
            rocketnet_server_wire_totals_add_sent( server_transport, ring->info[i].client_index, &ring->info[i].to, ring->info[i].wire_bytes );

            sends[num_sends].to = &ring->info[i].to;
            sends[num_sends].packet_data = ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET;
            sends[num_sends].packet_bytes = ring->info[i].wire_bytes;
            num_sends++;
        }
    }

    if ( num_sends > 0 )
        rocketnet_server_link_send_batch( server_transport, sends, num_sends );

    ring->num_packets = 0;

    // The disconnect_client header promise: buffered control frames leave on
    // the next flush even when this fill phase sent no payload-ring packet.
    rocketnet_server_flush_control_buffer( server_transport );

    rocketnet_send_arena_reset( server_transport->send_arena );
}

// the convenience: the same path underneath, plus one memcpy the caller chose

void rocketnet_server_transport_send_packet( struct rocketnet_server_transport_t * server_transport,
                                             int client_index,
                                             const uint8_t * payload_data,
                                             int payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    if ( !rocketnet_api_null_guard( payload_data ) )
        return;
    rocketnet_assert( server_transport );
    rocketnet_assert( payload_data );

    if ( payload_bytes < 1 || payload_bytes > server_transport->max_payload_bytes )
        return;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server_transport, client_index, payload_bytes, &context );
    if ( !frame )
        return;

    memcpy( frame + context.payload_index, payload_data, payload_bytes );

    rocketnet_server_transport_finish_packet( server_transport, &context, payload_bytes );
}

// tla/RNDriverSendStaging.tla DisconnectClient
void rocketnet_server_transport_disconnect_client( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
        return;

    // the address must be taken before the protocol call: the disconnect
    // releases the mapping that holds it

    int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, client_index );
    rocketnet_address_t client_address = *rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );

    uint8_t burst[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_server_protocol_disconnect_client( &server_transport->protocol, client_index, burst, &burst_count, &burst_bytes );

    int committed = 0;
    for ( int i = 0; i < burst_count; i++ )
    {
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, &client_address, &frame );
        if ( !frame_data )
            break;
        frame.client_index = client_index;
        memcpy( frame_data + ROCKETNET_FRAME_WIRE_OFFSET, burst + i * ROCKETNET_SEND_BUDGET, (size_t) burst_bytes );
        rocketnet_server_commit_send_frame( server_transport, &frame, burst_bytes );
        committed++;
    }

    // The disconnect_client header promise: committed staged frames leave on
    // the next send_packets flush even when there is no following update.
    if ( committed && rocketnet_server_transport_staged( server_transport ) )
        server_transport->send_staged++;
}

int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_num_connected( server_transport->protocol.client_slots );
}

int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index );
}

uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_client_id( server_transport->protocol.client_slots, client_index );
}

// docs/CHANNELS.md P9a: the connection that holds the slot now

uint64_t rocketnet_server_transport_client_connection_id( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return rocketnet_server_protocol_connection_id( &server_transport->protocol, client_index );
}

const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return NULL;
    rocketnet_assert( server_transport );
    if ( server_transport->io.operations && server_transport->io.operations->prefilter_counters )
    {
        uint64_t prefilter[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
        server_transport->io.operations->prefilter_counters( server_transport->io.instance, prefilter );
        rocketnet_counters_apply_prefilter( &server_transport->counters, prefilter );
    }
    // SEND_FAILED is the driver's own cumulative count, assigned on read
    if ( server_transport->io.operations && server_transport->io.operations->send_failures )
        server_transport->counters.values[ROCKETNET_COUNTER_SEND_FAILED] = server_transport->io.operations->send_failures( server_transport->io.instance );
    return &server_transport->counters;
}

void rocketnet_server_transport_stats( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_transport_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    if ( !rocketnet_api_null_guard( stats ) )
        return;
    rocketnet_assert( server_transport );
    rocketnet_assert( stats );
    memset( stats, 0, sizeof(struct rocketnet_server_transport_stats_t) );
    stats->packets_sent_per_second = server_transport->server_stats.packets_sent_per_second;
    stats->packets_received_per_second = server_transport->server_stats.packets_received_per_second;
    stats->bandwidth_sent_kbps = server_transport->server_stats.bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = server_transport->server_stats.bandwidth_received_kbps;
    stats->packets_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &server_transport->server_stats.packets_sent );
    stats->packets_received = server_transport->server_stats.packets_received;
    stats->bytes_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &server_transport->server_stats.bytes_sent );
    stats->bytes_received = server_transport->server_stats.bytes_received;
}

int rocketnet_server_transport_max_payload_bytes( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return server_transport->max_payload_bytes;
}

int rocketnet_server_transport_acked_packets( struct rocketnet_server_transport_t * server_transport, const int ** client_index, const uint64_t ** packet_ids )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( client_index ) )
        return 0;
    if ( !rocketnet_api_null_guard( packet_ids ) )
        return 0;
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( packet_ids );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    *client_index = acks->acked_client_index;
    *packet_ids = acks->acked_packet_ids;

    return acks->num_acked;
}

// docs/CHANNELS.md P9b: the connection id of each acked packet id, parallel
// to the client index array above

int rocketnet_server_transport_acked_connection_ids( struct rocketnet_server_transport_t * server_transport, const uint64_t ** connection_id )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( connection_id ) )
        return 0;
    rocketnet_assert( server_transport );
    rocketnet_assert( connection_id );

    *connection_id = server_transport->protocol.acked_connection_ids;

    return server_transport->protocol.acks->num_acked;
}

void rocketnet_server_transport_client_stats( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_server_transport_client_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    if ( !rocketnet_api_null_guard( stats ) )
        return;
    rocketnet_assert( server_transport );
    rocketnet_assert( stats );

    memset( stats, 0, sizeof(struct rocketnet_server_transport_client_stats_t) );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    if ( client_index < 0 || client_index >= acks->max_clients )
        return;

    // An empty slot reads as zeros, exactly as an index out of range does.
    // rocketnet_server_connection_reset returns the slot's ack endpoint to
    // its initial state when the connection ends; the slot's wire totals are
    // not part of that reset and are cleared when the slot is next assigned
    // (rocketnet_server_wire_totals_assign), so until then this check is what
    // keeps a departed client's totals from being read.

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
        return;

    const struct rocketnet_ack_endpoint_t * endpoint = &acks->endpoints[client_index];

    stats->rtt = endpoint->rtt;
    stats->jitter = endpoint->jitter;
    stats->packet_loss = endpoint->packet_loss;
    stats->bandwidth_sent_kbps = endpoint->bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = endpoint->bandwidth_received_kbps;
    stats->bandwidth_acked_kbps = endpoint->bandwidth_acked_kbps;
    if ( client_index < server_transport->client_wire_totals_capacity )
    {
        const struct rocketnet_server_wire_totals_t * totals = &server_transport->client_wire_totals[client_index];
        stats->packets_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &totals->packets_sent );
        stats->packets_received = totals->packets_received;
        stats->bytes_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &totals->bytes_sent );
        stats->bytes_received = totals->bytes_received;
    }
}

#endif // ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

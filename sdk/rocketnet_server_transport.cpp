// Protocol defaults and table-sizing ratios the create path assigns.
// One declaration: rocketnet_server_transport_create uses these macros, and
// the conformance dump includes this file under
// ROCKETNET_CONFORMANCE_DEFAULTS_ONLY and expands the same ones.

#define ROCKETNET_DEFAULT_SEND_RATE                    10.0
#define ROCKETNET_DEFAULT_HANDSHAKE_DEADLINE_SECONDS   5.0
#define ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS      3
#define ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS      60

static_assert( ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS > ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS,
               "cookie rotation period must exceed cookie lifetime so a cookie never outlives both keys" );

#define ROCKETNET_ENCRYPTION_MAP_RATIO                 2
#define ROCKETNET_USED_TOKEN_HISTORY_RATIO             8

#ifndef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

#include "rocketnet_test_seams.h"
#include <limits.h>
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

// Defined beside the builtin operation tables in rocketnet_driver_builtin.cpp.
static void rocketnet_server_transport_abort_create( struct rocketnet_server_transport_t * transport );

bool rocketnet_server_transport_io_receive_drops( const struct rocketnet_server_transport_io_operations_t * operations,
                                                  void * transport, uint64_t * too_short, uint64_t * too_large );

// One payload slot of the receive batch's storage holds a whole opened
// packet: the protocol opens each packet into the next free slot and an
// accepted payload is delivered where it opened, its bytes behind the ack
// block, so the plaintext is written once (rocketnet_server_protocol_process_received).

#define ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES ROCKETNET_RECEIVE_CEILING

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES >= ROCKETNET_RECEIVE_PAYLOAD_BYTES,
                         "a payload slot holds the largest payload" );

#include <sodium.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if ROCKETNET_XDP
#include <linux/if_xdp.h>
#endif // #if ROCKETNET_XDP
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <pthread.h>
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// The transport is selected at RUNTIME (#106): one build carries
// every transport its platform has, and create walks the config's ordered
// candidate list -- config->transport, overridden by ROCKETNET_SERVER_TRANSPORT --
// left to right, first grant wins. On linux that is the runtime ladder:
// probe XDP (privilege and NIC support), fall to the non-root GSO transport
// (SO_REUSEPORT sharding + sendmmsg/recvmmsg + UDP GSO/GRO, per the #81
// measurements), floor at portable. "auto" walks the ladder; ANY explicit
// list is exactly that list, so a one-element list is a pin -- that transport
// or loud failure, never a silent fall that would mask a provisioning
// error. The granted tier is reported request-vs-granted style:
// rocketnet_server_transport_io_name plus one log line naming the grant and
// why each earlier candidate refused. The GSO transport's own feature probing
// and degradation remains runtime inside the transport, below this ladder.

#if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )
#error transport selection is runtime now (#106): drop ROCKETNET_SERVER_XDP / ROCKETNET_SERVER_GSO and select with config transport or ROCKETNET_SERVER_TRANSPORT
#endif // #if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE >= 2 * ROCKETNET_SERVER_MAX_SEND_PACKETS,
                         "receive budget twice send: per-update receive budget holds at least twice the portable send batch" );

struct rocketnet_server_send_buffer_t
{
    int num_packets;
    rocketnet_address_t to[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    int packet_bytes[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE * ROCKETNET_SERVER_MAX_SEND_PACKETS];
};

// the receive batch on the non-xdp transports: one pump call fills these, then
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
    uint8_t data[ROCKETNET_RECEIVE_CEILING * ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
};

// the portable payload ring: what the acquire/commit surface fills between
// update and the flush, from any number of game threads. slot acquisition is
// one atomic add; each slot's info sits alone on its cache line so
// concurrent finishes never share one; the frame stride keeps every root
// 16-aligned. the response ring above stays single-threaded under update
// and is deliberately not this.

#define ROCKETNET_SERVER_PAYLOAD_RING_SIZE      ROCKETNET_SERVER_MAX_SEND_PACKETS

struct alignas( 64 ) rocketnet_server_payload_slot_info_t
{
    rocketnet_address_t to;
    int wire_bytes;
    int client_index;
    uint8_t padding[64 - sizeof( rocketnet_address_t ) - 2 * sizeof( int )]; // the rest of the cache line, explicit
};

static_assert( sizeof( rocketnet_server_payload_slot_info_t ) == 64, "one server slot info per cache line" );

struct rocketnet_server_payload_ring_t
{
    uint8_t padding_0[4096];

    volatile uint64_t num_packets;

    // a page, plus the rest of num_packets' cache line, so info starts on a cache line
    uint8_t padding_1[4096 + 64 - sizeof( uint64_t )];

    struct rocketnet_server_payload_slot_info_t info[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];

    uint8_t padding_2[4096];

    alignas( 16 ) uint8_t data[ROCKETNET_FRAME_STRIDE * ROCKETNET_SERVER_PAYLOAD_RING_SIZE];

    uint8_t padding_3[4096];
};

static_assert( offsetof( rocketnet_server_payload_ring_t, info ) == 8192 + 64, "server ring info starts on a cache line with no implicit padding" );
static_assert( offsetof( rocketnet_server_payload_ring_t, data ) % 16 == 0 &&
                   offsetof( rocketnet_server_payload_ring_t, data ) == offsetof( rocketnet_server_payload_ring_t, padding_2 ) + 4096,
               "server ring data is 16-aligned with no implicit padding" );
static_assert( sizeof( rocketnet_server_payload_ring_t ) == offsetof( rocketnet_server_payload_ring_t, padding_3 ) + 4096,
               "server ring has no implicit tail padding" );
static_assert( ROCKETNET_FRAME_STRIDE % 16 == 0, "frame stride must keep every slot root 16-aligned" );
static_assert( ROCKETNET_FRAME_STRIDE >= ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING, "a slot must hold the slack and a full wire packet" );
static_assert( ROCKETNET_FRAME_PAYLOAD_OFFSET % 16 == 0, "the payload pointer handed to the caller must be 16-aligned" );

// Statistics accumulate during the existing send/receive walks. Buckets
// advance with elapsed time, independently of update cadence, and only the
// trailing second contributes. The frame field marks initialized buckets;
// it does not select their index or determine their retention.

#define ROCKETNET_NUM_SERVER_STATS_FRAMES 256

struct alignas( 64 ) rocketnet_server_stats_frame_t
{
    double time;
    uint64_t frame;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint8_t padding[64 - sizeof( double ) - 5 * sizeof( uint64_t )]; // the rest of the cache line, explicit
};

static_assert( sizeof( rocketnet_server_stats_frame_t ) == 64, "one server stats frame per cache line" );

// cumulative wire totals, per server lifetime and per connection slot. a
// datagram that arrives or leaves before its slot is assigned counts in the
// whole-server totals only: the server keeps no per-source state for traffic
// it has not authenticated, so a slot's totals start at its assignment.

struct rocketnet_server_wire_totals_t
{
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

// the payload batch from the last update -- capacity ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE
// covers a full drain. the public surface iterates by index and never sees this layout.

struct rocketnet_server_payloads_t
{
    int num_payloads;
    int client_index[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
    uint64_t connection_id[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
    uint64_t sequence[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];     // P9b: the connection each entry was received for
    uint8_t * payload_data[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
    int payload_bytes[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
};

// One server disconnect burst waiting to be sealed: everything the sealed
// packets need once the mapping that held it is gone. Its key lives beside
// it in disconnect_burst_keys, so the secret span is one array.
struct rocketnet_server_disconnect_burst_t
{
    rocketnet_address_t to;
    uint64_t first_sequence;
    int plaintext_bytes;
    int sealed;
    uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES];
};

struct rocketnet_server_transport_t
{
    // the cache-line-aligned members lead, each a whole number of cache
    // lines, so the compiler inserts no alignment padding before any of them
    rocketnet_packet_leases_t<ROCKETNET_SERVER_PACKET_LEASES> packet_leases;
    struct rocketnet_server_payload_ring_t payload_ring;
    struct rocketnet_server_stats_frame_t stats_frames[ROCKETNET_NUM_SERVER_STATS_FRAMES];

    void * context;
    int state;
    rocketnet_address_t bind_address;
    rocketnet_address_t public_address;

    // the granted transport (ROCKETNET_TRANSPORT_*), decided by the ladder at
    // create and constant for the transport's life -- the runtime fork
    // every seam function below branches on, one predictable branch

    int transport;

    rocketnet_server_transport_io_t io;
    uint64_t transport_faults;                     // a receive count or byte count this update refused

    // the kernel replay marks: allocated only when the granted transport reports
    // the replay capability, written from the update walk and committed to the
    // transport's map before the staged flush
    struct rocketnet_server_replay_marks_t * replay_marks;

    struct rocketnet_counters_t counters;

    // the transport's receive_drops totals as last read: each read adds only the
    // growth since to PREFILTER_TOO_SHORT and PREFILTER_TOO_LARGE, which the
    // protocol's own prefilter also counts into (sdk/rocketnet_server_protocol.cpp:1255)
    uint64_t transport_receive_too_short = 0;
    uint64_t transport_receive_too_large = 0;

    struct rocketnet_server_protocol_t protocol;

    // the portable path's buffers ride the struct on every platform now
    // that the transport is runtime: about a megabyte a staged grant never
    // touches, priced once per server against carrying the runtime fork
    // without allocation branches

    struct rocketnet_server_send_buffer_t send_buffer;
    struct rocketnet_server_receive_buffer_t receive_buffer;

    // the most payload-ring slots one update may fill. Create sets it from
    // the configured client count with the historical 256-slot floor; the
    // unit seam may set a smaller value so a cap below the client count
    // exercises fairness.
    int send_capacity;

    double time;                                        // the game's clock, last seen
    double raw_time;                                    // the caller's clock, last seen, beside the clamped one
    double time_offset;                                 // the backwards offset, exactly 0.0 until the first step
    uint64_t test_timestamp;                            // unit-only Unix time; zero reads the wall clock
    uint64_t token_epoch_unix_seconds;                  // unix seconds at the game clock's zero for the token check; 0 until the first update resolves it from the wall clock
    uint64_t frame;                                     // advanced once per update; identifies initialized stats buckets

    // the fragmentation shape from the config, and the send arena for
    // over-wire-capacity requests -- allocated once at create, to the
    // configured maximum. the reassembly set lives on the protocol.

    int max_payload_bytes;
    struct rocketnet_send_arena_t * send_arena;

    struct rocketnet_server_transport_stats_t server_stats;
    int current_stats_index;
    struct rocketnet_server_wire_totals_t * client_wire_totals;         // one per client slot
    int client_wire_totals_capacity;
    int maintenance_next_client;                       // first unserved slot after staged backpressure

    volatile uint64_t send_staged;                      // bumped by every producer; fills use the atomic add
    uint64_t send_flushed;                              // main thread only

    // payload sends on the staged transports seal straight into the transport's
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
#if ROCKETNET_DEV
    int link_busy_sends_for_test;   // the next link sends that take the lock-miss path
#endif // #if ROCKETNET_DEV

    // The disconnect bursts waiting to leave (proofs/tla/ServerSession.tla
    // DrainDisconnectBurst). A disconnect ends the slot at once; when this
    // update's allowance cannot seal its whole burst, the burst's plaintext
    // (the ack block of the receive state at the disconnect), its first
    // sequence, salt and session key wait here in a ring, and each update
    // seals at most disconnect_packets_per_update of them, one
    // packet per waiting burst per pass, so every client's first disconnect
    // packet leaves before any burst's redundant copies. The keys are wiped
    // as each burst completes and at destroy.
    struct rocketnet_server_disconnect_burst_t disconnect_bursts[ROCKETNET_MAX_CLIENTS];
    uint8_t disconnect_burst_keys[ROCKETNET_MAX_CLIENTS][ROCKETNET_KEY_BYTES];
    int disconnect_burst_head;
    int disconnect_burst_count;
    int disconnect_burst_cursor;                        // ring offset from the head of this pass's next burst
    int disconnect_packets_per_update;                  // configured whole-server allowance
    int disconnect_seals_left;                          // this update's allowance, reset by each update
    int disconnect_update_prepared;                     // message layer opened this public update's allowance
    uint64_t disconnect_seals;                          // disconnect packets sealed, for the unit work bound

    struct rocketnet_server_payloads_t payloads;
    // +8: serialize's BitReader loads 8-byte windows at byte granularity, so
    // any buffer a ReadStream is bound over must extend at least 8 bytes past
    // the end of its data -- this is the last member, so the slack is real.
    uint8_t payload_storage[ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES * ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE + 8];
};

static void rocketnet_server_transport_drain_disconnect_bursts( struct rocketnet_server_transport_t * server_transport, int max_seals, bool shutdown );

// The server transport's state zeroed in its raw storage, then default
// initialization, then the buffers' counts. The buffers (the payload ring's
// slots, the send and receive buffers, the payloads and their storage: 49 MB
// at the defaults) are never written here, so their pages are touched only
// when a packet uses them.
static void rocketnet_server_transport_construct( rocketnet_server_transport_t * server_transport )
{
    typedef rocketnet_server_transport_t T;
    typedef rocketnet_server_payload_ring_t R;
    typedef rocketnet_server_send_buffer_t B;
    typedef rocketnet_server_payloads_t P;

    const size_t ring = offsetof( T, payload_ring );
    const size_t send = offsetof( T, send_buffer );
    const size_t payloads = offsetof( T, payloads );
    const rocketnet_span_t skip[] = {
        { ring, offsetof( R, num_packets ) },
        { ring + offsetof( R, num_packets ) + sizeof( uint64_t ), sizeof( R ) - offsetof( R, num_packets ) - sizeof( uint64_t ) },
        { send + offsetof( B, to ), sizeof( B ) - offsetof( B, to ) },
        { offsetof( T, receive_buffer ), sizeof( rocketnet_server_receive_buffer_t ) },
        { offsetof( T, disconnect_bursts ), sizeof( ( (T *) 0 )->disconnect_bursts ) },
        { payloads + offsetof( P, client_index ), sizeof( P ) - offsetof( P, client_index ) },
        { offsetof( T, payload_storage ), sizeof( ( (T *) 0 )->payload_storage ) },
    };
    static_assert( offsetof( T, payload_ring ) < offsetof( T, send_buffer ) && offsetof( T, send_buffer ) < offsetof( T, receive_buffer ) &&
                       offsetof( T, receive_buffer ) < offsetof( T, disconnect_bursts ) && offsetof( T, disconnect_bursts ) < offsetof( T, payloads ) && offsetof( T, payloads ) < offsetof( T, payload_storage ),
                   "the skipped buffers are listed in their order in the struct" );
    rocketnet_zero_except( server_transport, sizeof( T ), skip, (int) ( sizeof( skip ) / sizeof( skip[0] ) ) );
    new ( server_transport ) rocketnet_server_transport_t;
    server_transport->payload_ring.num_packets = 0;
    server_transport->send_buffer.num_packets = 0;
    server_transport->payloads.num_payloads = 0;
}


#if ROCKETNET_DEV
void rocketnet_server_transport_poison_slot_for_test( struct rocketnet_server_transport_t * server, int client_index )
{
    rocketnet_assert( server );
    rocketnet_assert( client_index >= 0 && client_index < server->client_wire_totals_capacity );
    memset( &server->client_wire_totals[client_index], 0xA5, sizeof( server->client_wire_totals[client_index] ) );
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
        if ( rocketnet_encryption_map_control( protocol->encryption_map, iterator )->client_index == client_index )
            member = "rocketnet_encryption_mapping_control_t.client_index";
    if ( !member && protocol->connection_ids[client_index] != 0 )
        member = "rocketnet_server_protocol_t.connection_ids";
    bool allocation_failed = false;

    if ( !member )
    {
        struct rocketnet_ack_endpoint_t * fresh = (struct rocketnet_ack_endpoint_t *) rocketnet_malloc( server->context, sizeof( struct rocketnet_ack_endpoint_t ) );
        if ( !fresh )
        {
            // fail closed: without scratch memory the endpoint cannot be checked
            member = "rocketnet_server_acks_t.endpoints";
            allocation_failed = true;
        }
        else
        {
            rocketnet_ack_endpoint_reset( fresh );
            if ( !rocketnet_ack_endpoint_state_equal( fresh, &protocol->acks->endpoints[client_index] ) )
                member = "rocketnet_server_acks_t.endpoints";
            rocketnet_free( server->context, fresh );
        }
    }

    if ( !member && protocol->reassembly->newest_completed[client_index] != 0 )
        member = "rocketnet_reassembly_set_t.newest_completed";
    const uint8_t * slice = (const uint8_t *) ( protocol->reassembly->slots + client_index * protocol->reassembly->num_slots );
    for ( size_t i = 0; !member && i < (size_t) protocol->reassembly->num_slots * sizeof( struct rocketnet_reassembly_slot_t ); i++ )
        if ( slice[i] != 0 )
            member = "rocketnet_reassembly_set_t.slots";
    if ( member )
    {
        if ( allocation_failed )
            snprintf( failure, failure_bytes, "%s[%d]: check could not run (allocation failed)", member, client_index );
        else
            snprintf( failure, failure_bytes, "%s[%d]: not reset when the connection ended", member, client_index );
    }

    return member == NULL;
}
#endif // #if ROCKETNET_DEV

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
    }
}

// The staged producers (any game thread) fold the whole-server sent counters
// and the per-client sent totals with relaxed atomics, as the fill-phase
// atomics in rocketnet_platform.h do for stats_pending_packets_sent and
// stats_pending_bytes_sent: each counter is meaningful alone and nothing is
// ordered behind it. A producer whose client_index is out of range counts in
// the whole-server totals only, as the main-thread path does.

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
    }
}

// The one encryption-map lookup a received packet pays: the transport looks
// the source up once, attributes the wire totals from it, and hands the same
// answer to the protocol, which looks nothing up again. A packet that fails
// the shape check is not looked up.

static int rocketnet_server_receive_mapping_index( struct rocketnet_server_transport_t * server_transport,
                                                   bool shape_valid, const rocketnet_address_t * address )
{
    if ( !shape_valid )
        return ROCKETNET_SERVER_MAPPING_NOT_LOOKED_UP;
    return rocketnet_encryption_map_find( server_transport->protocol.encryption_map, address );
}

static int rocketnet_server_mapping_client_index( struct rocketnet_server_transport_t * server_transport, int mapping_index )
{
    if ( mapping_index < 0 )
        return -1;
    return rocketnet_encryption_map_control( server_transport->protocol.encryption_map, mapping_index )->client_index;
}

// A packet from an address with no slot before it can leave its mapping with
// one only by the response that connects it (ACCEPTED or SEND_KEEP_ALIVE on
// a mapping that existed before the packet); the mapping index is unchanged,
// so the slot is read from it rather than looked up again.

static void rocketnet_server_wire_totals_assign( struct rocketnet_server_transport_t * server_transport, int client_index );

static int rocketnet_server_wire_client_after( struct rocketnet_server_transport_t * server_transport,
                                               int wire_client_before, int mapping_index, int outcome )
{
    if ( wire_client_before >= 0 || mapping_index < 0 )
        return wire_client_before;
    if ( outcome != ROCKETNET_SERVER_PROCESS_ACCEPTED && outcome != ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE )
        return wire_client_before;
    const int wire_client_after = rocketnet_server_mapping_client_index( server_transport, mapping_index );
    if ( wire_client_after >= 0 )
        rocketnet_server_wire_totals_assign( server_transport, wire_client_after );
    return wire_client_after;
}

// The shape check, stateless and first for every datagram: the prefilter
// header (size, epoch, filter value), then the server-inbound type and size
// rule the kernel filter applies (ROCKETNET_WIRE_SERVER_PACKET_VALID). A
// datagram that fails it is refused by the protocol on its first rungs, with
// no response and no table read, so it gets no mapping lookup and no send
// frame; the protocol still runs on it, so every refusal counter is the same.

static bool rocketnet_server_packet_shape_valid( const struct rocketnet_server_protocol_t * protocol,
                                                 const uint8_t * packet_data, int packet_bytes )
{
    const int packet_type = rocketnet_prefilter_check( packet_data, packet_bytes, protocol->packet_filter );
    return packet_type >= 0 && ROCKETNET_WIRE_SERVER_PACKET_VALID( packet_type, packet_bytes );
}

static void rocketnet_server_wire_totals_assign( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_assert( client_index >= 0 && client_index < server_transport->client_wire_totals_capacity );
    memset( &server_transport->client_wire_totals[client_index], 0, sizeof( struct rocketnet_server_wire_totals_t ) );
}

// Dispatch is immutable for the transport lifetime. Transports own their native
// resources; the transport selects behavior through the declared capabilities.
static bool rocketnet_server_transport_staged( const rocketnet_server_transport_t * transport )
{
    return rocketnet_server_transport_io_has( transport->io, ROCKETNET_TRANSPORT_STAGED );
}

// The unit seam: the send capacity the next create reads. 0 keeps the
// default. It is consumed by that create so one case cannot shape another.
#if ROCKETNET_DEV
static int rocketnet_server_test_create_send_capacity = 0;

void rocketnet_server_transport_set_create_send_capacity_for_test( int packets )
{
    rocketnet_server_test_create_send_capacity =
        packets > 0 && packets <= ROCKETNET_SERVER_MAX_SEND_PACKETS ? packets : 0;
}
#else // #if ROCKETNET_DEV
static const int rocketnet_server_test_create_send_capacity = 0;
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
int rocketnet_server_transport_send_capacity( const rocketnet_server_transport_t * server_transport )
{
    return server_transport ? server_transport->send_capacity : 0;
}
#endif // #if ROCKETNET_DEV

// How many payload frames may still start this update. The staged transports
// keep their own staging, so the transport-side cap does not apply to them.
// proofs/tla/RNServerSendFair.tla Tick models this bounded cyclic send-pass cap.
int rocketnet_server_transport_send_capacity_available( const rocketnet_server_transport_t * server_transport )
{
    if ( !server_transport )
        return 0;
    if ( rocketnet_server_transport_staged( server_transport ) )
        return INT_MAX;
    const uint64_t used = server_transport->payload_ring.num_packets;
    return used >= (uint64_t) server_transport->send_capacity ? 0 : (int) ( (uint64_t) server_transport->send_capacity - used );
}

// a NULL start with the id ROCKETNET_TRANSPORT_PACKET_UNREACHABLE is the
// transport's refusal for want of a link-layer destination (TRANSPORTS.md, xdp):
// counted here, never as a full ring, and never sent or counted as sent
static uint8_t * rocketnet_server_transport_io_start_packet( rocketnet_server_transport_t * transport, const rocketnet_address_t * to, uint64_t * id )
{
    *id = 0;

    uint8_t * frame = transport->io.operations->start_packet( transport->io.instance, to, id );
    if ( !frame && *id == ROCKETNET_TRANSPORT_PACKET_UNREACHABLE )
        rocketnet_platform_atomic_add_u64( &transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS], 1 );
    return frame;
}
static void rocketnet_server_transport_io_finish_packet( rocketnet_server_transport_t * transport, uint64_t id, uint8_t * frame, int bytes )
{
    rocketnet_assert( transport && frame && id != ROCKETNET_TRANSPORT_PACKET_UNREACHABLE && bytes > 0 && bytes <= ROCKETNET_RECEIVE_CEILING );
    if ( !transport || !frame || id == ROCKETNET_TRANSPORT_PACKET_UNREACHABLE || bytes <= 0 || bytes > ROCKETNET_RECEIVE_CEILING )
        return;
    transport->io.operations->finish_packet( transport->io.instance, id, frame, bytes );
}
static void rocketnet_server_transport_io_abort_packet( rocketnet_server_transport_t * transport, uint64_t id, uint8_t * frame )
{
    transport->io.operations->abort_packet( transport->io.instance, id, frame );
}
static void rocketnet_server_transport_io_send_packets( rocketnet_server_transport_t * transport )
{
    transport->io.operations->flush( transport->io.instance );
}

// The link simulator is active when the setting in force changes the packets:
// any loss, latency, jitter or duplicate, or either pattern.  With all of
// them zero it is a pass-through that neither adds latency nor copies, and
// the datagram goes to the transport directly.
//
// Taking the setting also applies the one the caller handed over, so the
// setting in force at the send is the setting the caller last asked for: the
// change takes effect at once, on and off, in either direction.
//
// proofs/tla/Link.tla models the change: the setting in force at the send holds the
// packet, and the change itself removes nothing that was already in flight
// (LinkReconfigureHolds).
static bool rocketnet_server_link_active( rocketnet_server_transport_t * transport )
{
    if ( !transport->link_simulator )
        return false;
    return rocketnet_network_simulator_take_setting( transport->link_simulator, &transport->link_simulator_mutex ) ||
           rocketnet_network_simulator_patterns_active( transport->link_simulator );
}

// Returns true when the datagram went to the platform send or the simulator
// took it (a simulated loss inside the simulator is still a datagram sent and
// lost on the wire), and false when the simulator's lock was busy and the
// datagram was dropped here. A datagram is counted sent only once the link
// took it, so the callers charge the sent statistics on true alone.
static bool rocketnet_server_link_send( rocketnet_server_transport_t * transport,
                                        const rocketnet_address_t * to,
                                        const uint8_t * packet_data, int packet_bytes )
{
    // Datagram staging is best effort. In particular, a shutdown burst must
    // never wait for a producer that still owns the simulator lock.
    if ( !rocketnet_server_link_active( transport ) )
    {
        rocketnet_platform_send_t send;
        send.to = to;
        send.packet_data = packet_data;
        send.packet_bytes = packet_bytes;
        transport->io.operations->send_batch( transport->io.instance, &send, 1 );
        return true;
    }
#if ROCKETNET_DEV
    if ( transport->link_busy_sends_for_test > 0 )
    {
        transport->link_busy_sends_for_test--;
        rocketnet_platform_atomic_add_u64( &transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return false;
    }
#endif // #if ROCKETNET_DEV
    if ( !rocketnet_platform_mutex_try_acquire( &transport->link_simulator_mutex ) )
    {
        rocketnet_platform_atomic_add_u64( &transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return false;
    }

    rocketnet_network_simulator_send( transport->link_simulator, &transport->link_simulator_from,
                                      to, packet_data, packet_bytes, transport->time );
    rocketnet_platform_mutex_release( &transport->link_simulator_mutex );

    return true;
}

// The development network simulator call on a running transport: applies at
// once. A development server always owns a link simulator, so this stores the
// new settings as a config record under the tiny mutex and the thread that owns
// the simulator applies them before its next send. No simulator work happens
// under the lock.
#if ROCKETNET_DEV
bool rocketnet_server_transport_network_simulator( struct rocketnet_server_transport_t * server_transport, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent )
{
    rocketnet_assert( server_transport );

    if ( !server_transport->link_simulator )
        return false;
    // The new setting is handed over as a config record under the
    // simulator's own tiny mutex; the thread that owns the simulator takes
    // it before its next send, so the change takes effect at once and the
    // server's threads do not move.

    rocketnet_network_simulator_reconfigure( server_transport->link_simulator, &server_transport->link_simulator_mutex,
                                             latency_ms / 1000.0, jitter_ms / 1000.0,
                                             (float) packet_loss_percent, (float) duplicate_percent );
    return true;
}

// The unit seam for a lock miss without a second thread: the next `sends`
// link sends through an active simulator take the lock-miss path, counting
// SEND_RING_FULL as a real miss does, and never touch the mutex.
void rocketnet_server_transport_link_busy_for_test( struct rocketnet_server_transport_t * server_transport, int sends )
{
    rocketnet_assert( server_transport );
    server_transport->link_busy_sends_for_test = sends > 0 ? sends : 0;
}
#endif // #if ROCKETNET_DEV

// Returns the number of sends the link took and marks each one in accepted[],
// so the caller charges every send on its own result (the per-client totals
// stay per send). A send without the simulator goes to the platform whole.
static int rocketnet_server_link_send_batch( rocketnet_server_transport_t * transport,
                                             const rocketnet_platform_send_t * sends, int num_sends,
                                             bool * accepted, bool shutdown = false )
{
    if ( !rocketnet_server_link_active( transport ) )
    {
        if ( shutdown && transport->io.operations->send_batch_shutdown )
            transport->io.operations->send_batch_shutdown( transport->io.instance, sends, num_sends );
        else
            transport->io.operations->send_batch( transport->io.instance, sends, num_sends );
        for ( int i = 0; i < num_sends; i++ )
            accepted[i] = true;
        return num_sends;
    }

    int num_accepted = 0;

    for ( int i = 0; i < num_sends; i++ )
    {
        accepted[i] = rocketnet_server_link_send( transport, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
        if ( accepted[i] )
            num_accepted++;
    }

    return num_accepted;
}

// Link!ReverseWindow: the window the simulator holds is emitted reversed,
// whole, at the end of each send_packets (and of the update that sends). A
// link without patterns holds nothing and takes no lock here.
static void rocketnet_server_link_flush( rocketnet_server_transport_t * transport )
{
    if ( !rocketnet_server_link_active( transport ) || !rocketnet_network_simulator_patterns_active( transport->link_simulator ) )
        return;
    if ( !rocketnet_platform_mutex_try_acquire( &transport->link_simulator_mutex ) )
        return; // retain queued simulator packets; the next ordinary flush may retry
    rocketnet_network_simulator_flush_window( transport->link_simulator );
    rocketnet_platform_mutex_release( &transport->link_simulator_mutex );
}

// The drain runs whenever the server has a simulator, whatever the setting:
// a switch from latency or jitter to all-zero leaves the packets the
// simulator already holds, and they still leave when they come due (as the
// client drain does). Gating it on the setting would strand and lose them.
// A simulator holding nothing is read without the lock and drains nothing.
static void rocketnet_server_link_drain( rocketnet_server_transport_t * transport )
{
    if ( !transport->link_simulator || !rocketnet_network_simulator_holds_any( transport->link_simulator ) )
        return;
    for ( ;; )
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
            // proofs/tla/RNXdpReplay.tla Drain: the receive view a held reply
            // answered is gone by now, so the xdp transport addresses it by
            // the MAC its direct response took (the answered entry,
            // rocketnet_xdp_frame.h), and a duplicate the same way; an
            // address no frame came from is still refused
            uint64_t id = 0;
            uint8_t * frame = rocketnet_server_transport_io_start_packet( transport, &to, &id );
            if ( frame )
            {
                memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, packet, (size_t) packet_bytes );
                rocketnet_server_transport_io_finish_packet( transport, id, frame, packet_bytes );
            }
        }
        else
        {
            rocketnet_platform_send_t send = { &to, packet, packet_bytes };
            transport->io.operations->send_batch( transport->io.instance, &send, 1 );
        }
    }

    if ( rocketnet_server_transport_staged( transport ) )
        rocketnet_server_transport_io_send_packets( transport );
}

// The main-thread sent charge: every control frame and every portable payload
// lands in the current stats bucket and the cumulative totals directly.
static void rocketnet_server_charge_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                int client_index, const rocketnet_address_t * to, int packet_bytes )
{
    const int stats_index = server_transport->current_stats_index;
    server_transport->stats_frames[stats_index].packets_sent++;
    server_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( to->type ) );
    rocketnet_server_wire_totals_add_sent( server_transport, client_index, to, packet_bytes );
}

// Control frames borrow this buffer only during synchronous send_batch.
// Drain full batches as update fills them, so receive responses cannot
// consume the entire maintenance budget and servers can service >256 slots.
static void rocketnet_server_flush_control_buffer( rocketnet_server_transport_t * server_transport, bool shutdown = false )
{
    rocketnet_assert( !rocketnet_server_transport_staged( server_transport ) );
    rocketnet_assert( server_transport->io.operations && server_transport->io.operations->send_batch );

    struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    bool accepted[ROCKETNET_SERVER_MAX_SEND_PACKETS];
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

    // a portable control frame is charged at its commit (the disconnect
    // burst's count is read before the flush), so the results are not read

    if ( num_sends > 0 )
        rocketnet_server_link_send_batch( server_transport, sends, num_sends, accepted, shutdown );
    server_transport->send_buffer.num_packets = 0;
}

// The send frame seam every transport serves: reserve hands back a frame to
// seal complete wire bytes into, commit stamps its length, abort returns it
// unused. The portable transport batches frames and flushes them through the
// socket in bounded batches during update; the staged transports seal into their
// own send staging -- AF_XDP UMEM, a GSO batch, or an IOCP operation -- with
// zero copies between the seal and the flush.

struct rocketnet_server_frame_t
{
    uint8_t * data;
    uint64_t packet_id;             // the staged transports' bookkeeping id; unused on the portable path
    rocketnet_address_t to;
    int client_index;               // slot this frame belongs to, or -1 before assignment
};

// proofs/tla/RNDriverSendStaging.tla Reserve and FailedSend: reserves a frame in the
// transport's staging, or returns NULL and drops the frame when the staging is full
static uint8_t * rocketnet_server_reserve_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                      const rocketnet_address_t * to,
                                                      struct rocketnet_server_frame_t * frame, bool shutdown = false )
{
    frame->to = *to;
    frame->client_index = -1;

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        frame->data = rocketnet_server_transport_io_start_packet( server_transport, to, &frame->packet_id );
        if ( !frame->data )
        {
            rocketnet_spam( "server transport send staging full. packet dropped" );
        }

        return frame->data;
    }

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;

    if ( send_buffer->num_packets >= ROCKETNET_SERVER_MAX_SEND_PACKETS )
    {
        rocketnet_server_flush_control_buffer( server_transport, shutdown );
    }

    const int index = send_buffer->num_packets;
    send_buffer->to[index] = *to;
    send_buffer->packet_bytes[index] = 0;
    frame->data = send_buffer->data + index * ROCKETNET_FRAME_SIZE;
    return frame->data;
}

// proofs/tla/RNDriverSendStaging.tla Commit
static void rocketnet_server_commit_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                struct rocketnet_server_frame_t * frame,
                                                int packet_bytes )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( frame && frame->data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    if ( !server_transport || !frame || !frame->data || packet_bytes <= 0 || packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return;

    // every commit through this seam -- responses, keep-alives, disconnect
    // bursts -- runs on the main thread, so it lands in the current stats
    // bucket and the cumulative totals directly

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        bool sent = true;

        if ( rocketnet_server_link_active( server_transport ) )
        {
            sent = rocketnet_server_link_send( server_transport, &frame->to,
                                               frame->data + ROCKETNET_FRAME_WIRE_OFFSET, packet_bytes );
            rocketnet_server_transport_io_abort_packet( server_transport, frame->packet_id, frame->data );
        }
        else
            rocketnet_server_transport_io_finish_packet( server_transport, frame->packet_id, frame->data, packet_bytes );
        // a datagram is counted sent only once the link took it

        if ( sent )
            rocketnet_server_charge_send_frame( server_transport, frame->client_index, &frame->to, packet_bytes );
        return;
    }

    // the portable frame is charged here, at its commit, as before

    rocketnet_server_charge_send_frame( server_transport, frame->client_index, &frame->to, packet_bytes );

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
        rocketnet_server_transport_io_abort_packet( server_transport, frame->packet_id, frame->data );

    // on the portable path the reserved slot is reused by the next
    // reserve; nothing to undo

    frame->data = NULL;
}

// rocketnet_server_transport_default_config lives in rocketnet_config.cpp,
// written from the config table so every default is declared exactly once

#if ROCKETNET_DEV
void rocketnet_server_transport_dev_config( struct rocketnet_server_transport_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    rocketnet_assert( config );
    rocketnet_server_transport_default_config_current( config );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( config->server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( config->server_secret_key, rocketnet_dev_server_secret_key, 32 );
}
#endif // #if ROCKETNET_DEV

static int rocketnet_server_config_has_keys( const struct rocketnet_server_transport_config_t * config )
{
    uint8_t zero[32];
    memset( zero, 0, sizeof( zero ) );
    return memcmp( config->server_secret_key, zero, 32 ) != 0 &&
           memcmp( config->server_public_key, zero, 32 ) != 0 &&
           memcmp( config->backend_public_key, zero, 32 ) != 0;
}

// The stack a transport thread is created with. rocketnet_platform_thread_create
// passes no stack size (pthread_create with NULL attributes, CreateThread with
// zero), so the size is the platform default: on POSIX the default attribute's
// stack size, read without creating a thread; on Windows the executable's
// default reserve, 1 MiB unless the binary is linked with another /STACK.
uint64_t rocketnet_thread_stack_bytes()
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
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

// The link simulator's capacity: the packets a server holds while the setting
// delays them.  A development build creates the simulator for every server
// start, so the plan carries its bytes.
#define ROCKETNET_SERVER_LINK_SIMULATOR_SLOTS 4096

bool rocketnet_server_transport_memory_plan( const rocketnet_server_transport_config_t * config,
                                             const char * transport, rocketnet_server_memory_plan_t & plan )
{
    plan = {};
    if ( !transport )
        return false;
    rocketnet_server_transport_config_t defaults;

    if ( !config )
    {
        rocketnet_server_transport_default_config( &defaults );
        config = &defaults;
    }

    const int clients = config->max_clients;
    const int payload = ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES;
    const int slots = config->max_fragmented_messages ? config->max_fragmented_messages : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    if ( clients < 1 || clients > ROCKETNET_MAX_CLIENTS || payload < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ||
         payload > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES || slots < 1 || slots > ROCKETNET_MAX_FRAGMENTS ||
         config->num_xdp_queues < 0 || config->num_xdp_queues > 1024 )
        return false;

    plan.transport_bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_transport_t ) } );
    for ( uint64_t bytes : { rocketnet_encryption_map_allocation_bytes( clients * ROCKETNET_ENCRYPTION_MAP_RATIO ),
                             rocketnet_token_table_allocation_bytes( clients * ROCKETNET_USED_TOKEN_HISTORY_RATIO ),
                             rocketnet_client_slots_allocation_bytes( clients ), rocketnet_server_acks_allocation_bytes( clients ),
                             rocketnet_memory_allocations( { (uint64_t) clients * sizeof( uint64_t ), (uint64_t) clients * ROCKETNET_SENT_PACKET_BUFFER_SIZE * sizeof( uint64_t ),
                                                             (uint64_t) clients * sizeof( int ) } ),
        // The plan counts what the transport allocates, once: the per-client wire totals are a separate allocation.
                             rocketnet_memory_allocations( { (uint64_t) clients * sizeof( rocketnet_server_wire_totals_t ) } ) } )
    {
        if ( !bytes || !rocketnet_memory_add( plan.protocol_bytes, 1, bytes ) )
            return false;
    }

    plan.reassembly_bytes = rocketnet_reassembly_memory_required_delivered( clients, slots, payload,
                                                                            rocketnet_server_reassembly_delivered_capacity( clients, slots ),
                                                                            rocketnet_server_reassembly_delivered_bytes( clients, slots, payload ) );
    if ( !plan.reassembly_bytes || !rocketnet_memory_add( plan.reassembly_bytes, 4, rocketnet_allocation_overhead() ) )
        return false;
    plan.send_arena_bytes = rocketnet_send_arena_allocation_bytes( ROCKETNET_SEND_ARENA_SLOTS, ( payload + 15 ) & ~15 );
    if ( !rocketnet_builtin_server_memory_plan( config, transport, plan ) )
        return false;
    // loopback opens no socket and plans no I/O bytes: its endpoint is in the caller's hub
    if ( !plan.transport_bytes || !plan.send_arena_bytes || ( plan.sockets != 0 && !plan.transport_io_bytes ) )
        return false;
    for ( uint64_t bytes : { plan.transport_bytes, plan.protocol_bytes, plan.reassembly_bytes,
                             plan.send_arena_bytes, plan.transport_io_bytes, plan.registered_region_bytes } )
        if ( !rocketnet_memory_add( plan.allocator_bytes, 1, bytes ) )
            return false;
    if ( rocketnet_public_link_simulator_development() )
    {
        // the link simulator every development server owns, created with it
        const uint64_t simulator_bytes = rocketnet_network_simulator_memory_bytes( ROCKETNET_SERVER_LINK_SIMULATOR_SLOTS,
                                                                                   config->simulated_reverse_window );
        if ( !simulator_bytes || !rocketnet_memory_add( plan.allocator_bytes, 1, simulator_bytes ) )
            return false;
    }

    // the RIO registered region comes from the allocator; UMEM is page-aligned
    // memory the AF_XDP socket registers with the kernel, outside it

    plan.total_bytes = plan.allocator_bytes;
    if ( !rocketnet_memory_add( plan.total_bytes, 1, plan.umem_bytes ) )
        return false;
    if ( plan.worker_threads > 0 )
    {
        const uint64_t stack = rocketnet_thread_stack_bytes();
        if ( !stack || !rocketnet_memory_add( plan.thread_stack_bytes, (uint64_t) plan.worker_threads, stack ) )
            return false;
    }

    for ( uint64_t bytes : { plan.umem_bytes, plan.thread_stack_bytes, plan.mapped_ring_bytes } )
        if ( !rocketnet_memory_add( plan.os_bytes, 1, bytes ) )
            return false;
    return plan.total_bytes <= SIZE_MAX;
}

uint64_t rocketnet_server_transport_ladder_allocator_bytes( const rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_config_t defaults;

    if ( !config )
    {
        rocketnet_server_transport_default_config( &defaults );
        config = &defaults;
    }

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    struct rocketnet_transport_ladder_t ladder;
    if ( !rocketnet_transport_ladder_parse( config->transport, available, num_available, &ladder ) )
        return 0;
    uint64_t most = 0;

    for ( int i = 0; i < ladder.num_candidates; ++i )
    {
        rocketnet_server_memory_plan_t plan;
        if ( rocketnet_server_transport_memory_plan( config, rocketnet_transport_name( ladder.candidates[i] ), plan ) && plan.allocator_bytes > most )
            most = plan.allocator_bytes;
    }

    return most;
}

uint64_t rocketnet_server_transport_required_memory( const rocketnet_server_transport_config_t * config, const char * transport )
{
    rocketnet_server_memory_plan_t plan;
    return rocketnet_server_transport_memory_plan( config, transport, plan ) ? plan.total_bytes : 0;
}

// Whole-host estimate (TRANSPORT_READINESS.md #155): the plan plus what each
// built-in transport asks of the kernel. Requests only, never grants. The sizes
// are the ones the transports pass: the portable and msgx socket and every gso
// queue socket set SO_SNDBUF/SO_RCVBUF to ROCKETNET_SOCKET_SEND_BUFFER_SIZE and
// ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE (rocketnet_driver_builtin.cpp,
// rocketnet_server_gso_linux.cpp), as do every io_uring queue socket
// (rocketnet_server_uring_linux.cpp) and the iocp socket
// (rocketnet_server_iocp_windows.cpp); each xdp queue's AF_XDP socket asks for
// ROCKETNET_XDP_RECV_QUEUE_SIZE RX and ROCKETNET_XDP_SEND_QUEUE_SIZE TX
// descriptors and sets no socket buffer (rocketnet_server_socket_linux.cpp).
bool rocketnet_server_transport_host_memory_estimate( const struct rocketnet_server_transport_config_t * config,
                                                      const char * transport,
                                                      struct rocketnet_host_memory_estimate_t * estimate )
{
    if ( !rocketnet_api_null_guard( estimate ) )
        return false;
    memset( estimate, 0, sizeof( *estimate ) );

    rocketnet_server_memory_plan_t plan;
    if ( !transport || !rocketnet_server_transport_memory_plan( config, transport, plan ) )
        return false;

    struct rocketnet_host_memory_estimate_t e;
    memset( &e, 0, sizeof( e ) );
    e.allocator_bytes = plan.total_bytes - plan.umem_bytes - plan.registered_region_bytes;
    e.umem_bytes = plan.umem_bytes;
    e.registered_region_bytes = plan.registered_region_bytes;
    e.sockets = plan.sockets;
    e.worker_threads = plan.worker_threads;

    const bool xdp = strcmp( transport, "xdp" ) == 0;
    if ( xdp )
    {
#if ROCKETNET_XDP
        const uint64_t ring_bytes = (uint64_t) ( ROCKETNET_XDP_RECV_QUEUE_SIZE + ROCKETNET_XDP_SEND_QUEUE_SIZE ) * sizeof( struct xdp_desc );
        if ( !rocketnet_memory_add( e.xdp_ring_bytes, (uint64_t) plan.sockets, ring_bytes ) )
            return false;
#else
        return false;
#endif // #if ROCKETNET_XDP
    }
    else
    {
        if ( !rocketnet_memory_add( e.socket_send_buffer_bytes, (uint64_t) plan.sockets, ROCKETNET_SOCKET_SEND_BUFFER_SIZE ) ||
             !rocketnet_memory_add( e.socket_receive_buffer_bytes, (uint64_t) plan.sockets, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE ) )
            return false;
    }

    if ( plan.worker_threads > 0 )
    {
        e.thread_stack_bytes_per_thread = rocketnet_thread_stack_bytes();
        if ( !e.thread_stack_bytes_per_thread ||
             !rocketnet_memory_add( e.thread_stack_bytes, (uint64_t) plan.worker_threads, e.thread_stack_bytes_per_thread ) )
            return false;
    }

    for ( uint64_t bytes : { e.allocator_bytes, e.umem_bytes, e.registered_region_bytes, e.socket_send_buffer_bytes,
                             e.socket_receive_buffer_bytes, e.xdp_ring_bytes, e.thread_stack_bytes } )
    {
        if ( !rocketnet_memory_add( e.total_bytes, 1, bytes ) )
            return false;
    }

    // what the library cannot know, named rather than guessed

    const char * excluded[ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS];
    int count = 0;

    // refuse an overflow before the write: at the cap, drop and log once

    auto append = [&]( const char * line )
    {
        if ( count >= ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS )
        {
            rocketnet_error( "host-memory estimate exclusion list is full (%d); dropping: %s",
                             ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS, line );
            return;
        }

        excluded[count++] = line;
    };
    append( "kernel socket accounting beyond the buffer requests (linux doubles each SO_SNDBUF/SO_RCVBUF request) and grants smaller than the requests" );

    if ( xdp )
    {
        append( "AF_XDP fill and completion rings (the umem config leaves their sizes to libxdp), ring headers and page rounding" );
        append( "libbpf/libxdp allocations, the XDP program and its maps" );
    }

    if ( strcmp( transport, "io_uring" ) == 0 )
        append( "io_uring submission and completion rings and the provided buffer rings (kernel and liburing mappings outside the SDK allocator)" );
    if ( plan.worker_threads > 0 )
        append( "thread guard pages, thread-local storage and kernel thread structures; stacks count reserved address space, not resident pages" );
    append( "kernel page tables and loader/shared-library memory" );
    append( "libc allocator bookkeeping outside the SDK allocator" );
    append( "application memory" );
    rocketnet_assert( count <= ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS );
    for ( int i = 0; i < count; i++ )
        e.exclusions[i] = excluded[i];
    e.num_exclusions = count;

    *estimate = e;

    return true;
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context,
                                                                         const char * public_address_string,
                                                                         const struct rocketnet_server_transport_config_t * config )
{
    return rocketnet_server_transport_create_ex( context, public_address_string, config, 0.0, NULL );
}

static struct rocketnet_server_transport_t * rocketnet_server_transport_create_internal( void * context,
                                                                                         const char * public_address_string,
                                                                                         const struct rocketnet_server_transport_config_t * config,
                                                                                         double time, int * status,
                                                                                         const uint64_t * resolved_memory_budget = NULL );

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
                                                                                    const struct rocketnet_server_transport_config_t * config, double time, int * status )
{
    return rocketnet_server_transport_create_internal( context, address, config, time, status );
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create_resolved( void * context, const char * address,
                                                                                  const struct rocketnet_server_transport_config_t * config,
                                                                                  uint64_t memory_budget_bytes, int * status )
{
    rocketnet_assert( config );
    return rocketnet_server_transport_create_internal( context, address, config, 0.0, status, &memory_budget_bytes );
}

// The inputs a server create starts from, resolved before anything is
// allocated or opened: the layered config (DEFAULT < STRUCT < ENV) validated,
// the served addresses and the bind they share, the parsed transport ladder,
// and which candidates the operator memory budget refuses.

struct rocketnet_server_transport_resolved_t
{
    struct rocketnet_server_transport_config_t config;
    const char * public_address_string;
    rocketnet_address_t public_address;
    rocketnet_address_t second_address;
    rocketnet_address_t bind_address;
    int serves_both;
    int max_clients;
    struct rocketnet_transport_ladder_t ladder;
    bool budget_refused[ROCKETNET_NUM_TRANSPORTS];
};

static bool rocketnet_server_transport_resolve( const char * public_address_string,
                                                const struct rocketnet_server_transport_config_t * config,
                                                const uint64_t * resolved_memory_budget,
                                                struct rocketnet_server_transport_resolved_t * out,
                                                int * status )
{
    rocketnet_assert( public_address_string );
    rocketnet_assert( out );

    // the config layering (#106): DEFAULT < STRUCT < ENV. NULL config is
    // the default shape; the caller's struct is the middle layer; the
    // environment overlays both through the config table, every override
    // logged loudly with the value it displaced. validation refuses
    // loudly and never clamps. A config the public create already resolved
    // (resolved_memory_budget set, rocketnet_server_transport_create_resolved)
    // takes no second environment pass: start uses the config create
    // validated and sized, whatever the environment says now.

    struct rocketnet_server_transport_config_t effective_config;
    if ( config )
        effective_config = *config;
    else
        rocketnet_server_transport_default_config_current( &effective_config );

    int num_config_fields = 0;
    const struct rocketnet_config_field_t * config_fields = rocketnet_server_transport_config_fields( &num_config_fields );

    if ( !resolved_memory_budget && !rocketnet_config_apply_env( "server", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return false;
    }

    if ( !rocketnet_config_validate( "server", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return false;
    }

    if ( !rocketnet_config_validate_public_link_simulator( "server", &effective_config, config_fields, num_config_fields, rocketnet_public_link_simulator_development() ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return false;
    }

    if ( rocketnet_config_packet_filter_refused( "server", effective_config.packet_filter ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION;
        return false;
    }

    config = &effective_config;

    // Refuse large cross-field allocations before sockets, transports or arenas
    // are acquired. Independent field maxima do not define a usable budget.

    const int requested_payload = ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES;
    const int requested_slots = config->max_fragmented_messages > 0 ? config->max_fragmented_messages : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    const int requested_clients = config->max_clients > 0 ? config->max_clients : ROCKETNET_DEFAULT_MAX_CLIENTS;
    if ( !rocketnet_reassembly_memory_allowed_delivered( requested_clients, requested_slots, requested_payload,
                                                         rocketnet_server_reassembly_delivered_capacity( requested_clients, requested_slots ),
                                                         rocketnet_server_reassembly_delivered_bytes( requested_clients, requested_slots, requested_payload ) ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED;
        rocketnet_error( "server config refused: the reassembly memory for max_clients %d and max_fragmented_messages %d exceeds the fixed reassembly limit of %d MiB", requested_clients, requested_slots, ROCKETNET_REASSEMBLY_MEMORY_MIB );
        return false;
    }

    // the config's public_address, when set, displaces the create call's
    // address parameter here, before anything reads it. The address the
    // ladder verifies, the address the transport binds and the address
    // connect tokens validate against are then the same one. The field has
    // no environment override. The public create reads the served
    // addresses from ROCKETNET_SERVER_IPV4_ADDRESS and
    // ROCKETNET_SERVER_IPV6_ADDRESS.

    // The resolved string must outlive this frame: when the field displaces
    // the parameter, out names out->config's copy, never effective_config's.

    const bool public_address_from_config = config->public_address[0] != '\0';
    if ( public_address_from_config )
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
        return false;
    }

    // Refuse protocol_id 0 with production keys: only development keys may use protocol_id 0

    if ( config->protocol_id == 0 )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_PROTOCOL_ID;
        rocketnet_error( "protocol_id 0 is not permitted with production keys: use dev config or a non-zero protocol_id" );
        return false;
    }

    // the transport candidate list parses before anything allocates: a typo
    // fails here, loudly, listing the valid names

    int num_available_transports = 0;
    const int * available_transports = rocketnet_server_transport_io_availability( &num_available_transports );

    struct rocketnet_transport_ladder_t ladder;
    if ( !rocketnet_transport_ladder_parse( config->transport, available_transports, num_available_transports, &ladder ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_TRANSPORT_LIST;
        return false;
    }

    rocketnet_info( "server transport public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_ADDRESS;
        rocketnet_error( "could not parse public address" );
        return false;
    }

    // ServedFamilies is one address, or both when second_public_address names
    // the other family on the same port (both 0, or equal). A second address
    // of the first's family, on another port, or that does not parse is one
    // log line and INVALID_ADDRESS.

    rocketnet_address_t second_address;
    memset( &second_address, 0, sizeof( second_address ) );

    int serves_both = 0;

    if ( config->second_public_address[0] != '\0' )
    {
        if ( !rocketnet_address_parse( &second_address, config->second_public_address ) )
        {
            if ( status )
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            rocketnet_error( "server second_public_address does not parse" );
            return false;
        }

        if ( second_address.type == public_address.type )
        {
            if ( status )
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            rocketnet_error( "server second_public_address is the same family as the public address" );
            return false;
        }

        if ( second_address.port != public_address.port )
        {
            if ( status )
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            rocketnet_error( "server second_public_address is on a different port from the public address" );
            return false;
        }

        serves_both = 1;
    }

    rocketnet_address_t bind_address;

    if ( serves_both )
    {
        // Both families share the IPv6 wildcard on the one port, with
        // IPV6_V6ONLY off: the bind a "[::]" server takes.
        memset( &bind_address, 0, sizeof( bind_address ) );
        bind_address.type = ROCKETNET_ADDRESS_IPV6;
        bind_address.port = public_address.port;
    }
    else
    {
        bind_address = public_address;
        if ( bind_address.type == ROCKETNET_ADDRESS_IPV4 )
            memset( bind_address.data.ipv4, 0, sizeof( bind_address.data.ipv4 ) );
        else
            memset( bind_address.data.ipv6, 0, sizeof( bind_address.data.ipv6 ) );
    }

    // the operator memory budget (#155): each candidate's pre-allocation plan
    // is compared to the budget before any allocation, and a candidate whose
    // plan exceeds it is refused exactly as a refusing transport is -- logged with
    // the plan and the budget, and skipped so the ladder falls to the next
    // candidate. a budget that no candidate fits refuses the create before the
    // first allocation.

    uint64_t memory_budget = resolved_memory_budget ? *resolved_memory_budget : 0;
    if ( !resolved_memory_budget && !rocketnet_config_apply_env_uint64( "server", "memory_budget_bytes", &memory_budget ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return false;
    }

    bool budget_refused[ROCKETNET_NUM_TRANSPORTS];
    memset( budget_refused, 0, sizeof( budget_refused ) );

    int budget_refused_total = 0;
    int budget_allowed = 0;

    for ( int i = 0; i < ladder.num_candidates; ++i )
    {
        const int candidate = ladder.candidates[i];
        const char * name = rocketnet_transport_name( candidate );
        rocketnet_server_memory_plan_t plan;
        if ( rocketnet_server_transport_memory_plan( config, name, plan ) )
        {
            rocketnet_info( "server memory plan [%s]: %llu allocator bytes, %llu UMEM bytes, %d workers, %d sockets; excludes OS stacks, kernel queues, loader and application memory",
                            name, (unsigned long long) ( plan.total_bytes - plan.umem_bytes ),
                            (unsigned long long) plan.umem_bytes, plan.worker_threads, plan.sockets );
            if ( memory_budget > 0 && plan.total_bytes > memory_budget )
            {
                rocketnet_warn( "server transport %s refused: memory plan %llu allocator bytes exceeds the %llu byte operator budget",
                                name, (unsigned long long) plan.total_bytes, (unsigned long long) memory_budget );
                budget_refused[candidate] = true;
                budget_refused_total++;
            }
            else
                budget_allowed++;
        }
        else
            rocketnet_info( "server memory plan [%s]: unavailable; consult this transport's resource policy", name );
    }

    if ( memory_budget > 0 && budget_refused_total > 0 && budget_allowed == 0 )
    {
        if ( status )
            *status = ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED;
        rocketnet_error( "server transport ladder exhausted: every candidate's memory plan exceeds the %llu byte operator budget",
                         (unsigned long long) memory_budget );
        return false;
    }

    out->config = effective_config;
    out->public_address_string = public_address_from_config ? out->config.public_address : public_address_string;
    out->public_address = public_address;
    out->second_address = second_address;
    out->bind_address = bind_address;
    out->serves_both = serves_both;
    out->max_clients = max_clients;
    out->ladder = ladder;
    memcpy( out->budget_refused, budget_refused, sizeof( out->budget_refused ) );

    return true;
}

// THE LADDER (#106): opens the resolved candidates in order until one grants,
// into server_transport->io. Returns the granted transport, with bind_address
// holding what it bound; -1 when none granted, with *status saying why (an
// open that ran out of memory ends the ladder rather than falling).

static int rocketnet_server_transport_open_ladder( struct rocketnet_server_transport_t * server_transport, void * context,
                                                   const struct rocketnet_server_transport_resolved_t * resolved,
                                                   rocketnet_address_t * bind_address, int * status )
{
    const struct rocketnet_server_transport_config_t * config = &resolved->config;
    const char * public_address_string = resolved->public_address_string;
    const rocketnet_address_t * public_address = &resolved->public_address;
    const struct rocketnet_transport_ladder_t * ladder = &resolved->ladder;
    const bool * budget_refused = resolved->budget_refused;

    // candidates tried left to right, first grant wins.
    // an operator who wrote the list owns the list -- nothing is appended
    // -- and a one-element list is a pin that refuses rather than falls.
    // each refusal logs loudly; the exhausted list fails naming what was
    // skipped for platform and what refused for grant.

    int granted_transport = -1;

    char refused_names[128];
    refused_names[0] = '\0';

    bool refused_seen[ROCKETNET_NUM_TRANSPORTS];
    memset( refused_seen, 0, sizeof( refused_seen ) );

    // #109: WHICH rungs the port-zero rule refused, so an exhausted ladder
    // can restate the rule at the level the failure itself is reported.
    // The names are collected rather than a bare flag because a mixed list
    // -- say "xdp,gso" where gso refused for its own queue geometry --
    // would make "no transport here can discover a port" a lie about gso. The
    // error names the rungs the rule actually refused, and nothing else.

    char port_zero_refused_names[128];
    port_zero_refused_names[0] = '\0';

    bool port_zero_refused_seen[ROCKETNET_NUM_TRANSPORTS];
    memset( port_zero_refused_seen, 0, sizeof( port_zero_refused_seen ) );

    // Opens only, not port-zero skips. TRANSPORT_FAMILIES_UNSUPPORTED and
    // TRANSPORT_NOT_BUILT are the start status when every open refused for
    // that reason.

    int open_refusals = 0;
    int families_unsupported_refusals = 0;
    int not_built_refusals = 0;

    for ( int i = 0; i < ladder->num_candidates && granted_transport < 0; i++ )
    {
        const int candidate = ladder->candidates[i];

        // a candidate whose memory plan the operator budget refused was logged
        // and refused in the plan loop above; record it for the exhausted
        // summary and fall to the next candidate, never opening it

        if ( budget_refused[candidate] )
        {
            if ( candidate >= 0 && candidate < ROCKETNET_NUM_TRANSPORTS && !refused_seen[candidate] )
            {
                refused_seen[candidate] = true;

                const size_t used = strlen( refused_names );
                snprintf( refused_names + used, sizeof( refused_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( candidate ) );
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
        // and the question is a property of each transport's bind
        // (rocketnet_transport_discovers_bound_port). It is a rung refusal
        // like any other: "auto" falls past xdp to a rung that discovers,
        // and only a PIN on a non-discovering rung refuses the create --
        // loudly, naming the rule, the way the pin law already works.

        if ( public_address->port == 0 && !rocketnet_transport_discovers_bound_port( candidate ) )
        {
            // named once however many times the operator listed it: the
            // ladder parse does not deduplicate, and "xdp, xdp" in an
            // error reads as a bug in the error rather than a repeat in
            // the list.
            //
            // keyed on the transport ID, not on a substring search of the
            // accumulated names. The hazard runs in the direction that is
            // easy to get backwards: strstr( "gso", "gso2" ) is NULL, so a
            // NEW longer name is not swallowed by a shorter accumulated
            // one -- but strstr( "gso2", "gso" ) FINDS, so an accumulated
            // longer name silently swallows a later legitimate shorter
            // one. No two names collide today; an id key cannot collide
            // whatever anyone adds.

            if ( candidate >= 0 && candidate < ROCKETNET_NUM_TRANSPORTS && !port_zero_refused_seen[candidate] )
            {
                port_zero_refused_seen[candidate] = true;

                const size_t used = strlen( port_zero_refused_names );
                snprintf( port_zero_refused_names + used, sizeof( port_zero_refused_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( candidate ) );
            }

            rocketnet_warn( "server transport %s refused: a port-zero public address asks the kernel for an ephemeral port, and the %s transport binds the port it is given and discovers nothing -- it would advertise port 0. give this server a real port, or leave a transport that discovers one in the list",
                            rocketnet_transport_name( candidate ), rocketnet_transport_name( candidate ) );
        }
        else
        {
            const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( candidate );
            int transport_status = ROCKETNET_STATUS_OK;
            const rocketnet_server_transport_io_config_t transport_config = { rocketnet_transport_name( candidate ), config, public_address_string, &transport_status };
            if ( rocketnet_server_transport_io_open( definition && definition->backend ? definition->backend->server : NULL, context,
                                                     &transport_config, bind_address, &server_transport->io ) )
                granted_transport = candidate;

            // a rung that failed for want of memory is not a refusal: the
            // create ends with OUT_OF_MEMORY and never falls to the next
            // candidate. Every transport reports it through transport_status
            // (rocketnet_server_transport_io_config_t.status, sdk/rocketnet_driver.h);
            // any other failed open is a refusal and falls

            if ( granted_transport < 0 && transport_status == ROCKETNET_STATUS_OUT_OF_MEMORY )
            {
                rocketnet_error( "server transport create: the %s transport could not allocate its endpoint", rocketnet_transport_name( candidate ) );
                if ( status )
                    *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
                return -1;
            }

            if ( granted_transport < 0 )
            {
                open_refusals++;
                if ( transport_status == ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED )
                    families_unsupported_refusals++;
                if ( transport_status == ROCKETNET_STATUS_TRANSPORT_NOT_BUILT )
                    not_built_refusals++;
            }
        }

        if ( granted_transport < 0 )
        {
            rocketnet_warn( "server transport %s refused%s", rocketnet_transport_name( candidate ),
                            i + 1 < ladder->num_candidates ? ". trying the next candidate" : "" );

            // deduped on the same rule as the port-zero list below it: a
            // repeated name in the operator's list is a repeat in the
            // list, not two refusals worth reading, and two adjacent
            // summary lines disagreeing about that reads as a bug in one
            // of them

            if ( candidate >= 0 && candidate < ROCKETNET_NUM_TRANSPORTS && !refused_seen[candidate] )
            {
                refused_seen[candidate] = true;

                const size_t used = strlen( refused_names );
                snprintf( refused_names + used, sizeof( refused_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( candidate ) );
            }
        }
    }

    if ( granted_transport < 0 )
    {
        char skipped_names[128];
        skipped_names[0] = '\0';

        for ( int i = 0; i < ladder->num_skipped; i++ )
        {
            const size_t used = strlen( skipped_names );
            snprintf( skipped_names + used, sizeof( skipped_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( ladder->skipped[i] ) );
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
                             port_zero_refused_names, rocketnet_transport_discovering_names() );
        }

        if ( status )
        {
            if ( port_zero_refused_names[0] != '\0' )
                *status = ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED;
            else if ( open_refusals > 0 && families_unsupported_refusals == open_refusals )
                *status = ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED;
            else if ( open_refusals > 0 && not_built_refusals == open_refusals )
                *status = ROCKETNET_STATUS_TRANSPORT_NOT_BUILT;
            else
                *status = ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED;
        }

        return -1;
    }

    if ( refused_names[0] != '\0' )
    {
        rocketnet_info( "server transport ladder granted %s after refusals: %s", rocketnet_transport_name( granted_transport ), refused_names );
    }

    return granted_transport;
}

static struct rocketnet_server_transport_t * rocketnet_server_transport_create_internal( void * context,
                                                                                         const char * public_address_string,
                                                                                         const struct rocketnet_server_transport_config_t * config,
                                                                                         double time, int * status,
                                                                                         const uint64_t * resolved_memory_budget )
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

    struct rocketnet_server_transport_resolved_t resolved;
    if ( !rocketnet_server_transport_resolve( public_address_string, config, resolved_memory_budget, &resolved, status ) )
        return NULL;

    config = &resolved.config;
    rocketnet_address_t public_address = resolved.public_address;   // a port 0 takes the granted bind port below
    rocketnet_address_t second_address = resolved.second_address;
    rocketnet_address_t bind_address = resolved.bind_address;          // the granted open writes the bound port
    const int serves_both = resolved.serves_both;
    const int max_clients = resolved.max_clients;

    struct rocketnet_server_transport_t * server_transport = (struct rocketnet_server_transport_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_transport_t ) );
    if ( !server_transport )
    {
        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        rocketnet_error( "server transport create: could not allocate server transport" );
        return NULL;
    }

    rocketnet_server_transport_construct( server_transport );

    server_transport->context = context;
    server_transport->time = time;
    server_transport->raw_time = time;                  // the seed is the clock's origin, not a step
    server_transport->time_offset = 0.0;
    // the token clock's epoch: configured, or 0 until the first update reads
    // the wall clock once (rocketnet_server_transport_update)
    server_transport->token_epoch_unix_seconds = config->token_epoch_unix_seconds;
    rocketnet_server_link_drain( server_transport );

    // the fragmentation shape: zero means the defaults; the table already
    // refused anything out of range, loudly, never clamped

    server_transport->max_payload_bytes = ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES;

    const int default_send_capacity = config->max_clients > ROCKETNET_SERVER_MIN_SEND_PACKETS ? config->max_clients : ROCKETNET_SERVER_MIN_SEND_PACKETS;
    server_transport->send_capacity = rocketnet_server_test_create_send_capacity > 0 ? rocketnet_server_test_create_send_capacity : default_send_capacity;
    server_transport->disconnect_packets_per_update = config->disconnect_packets_per_update;
    server_transport->disconnect_seals_left = server_transport->disconnect_packets_per_update;
    server_transport->disconnect_update_prepared = 0;
#if ROCKETNET_DEV
    rocketnet_server_test_create_send_capacity = 0;
#endif // #if ROCKETNET_DEV
    const int max_fragmented_messages = config->max_fragmented_messages > 0 ? config->max_fragmented_messages : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;

    const int development_keys = memcmp( config->server_public_key, rocketnet_dev_server_public_key, 32 ) == 0 ||
                                 memcmp( config->backend_public_key, rocketnet_dev_backend_public_key, 32 ) == 0;
    if ( development_keys )
    {
        // The published development keys are public and not for production: an
        // implementation must warn on every start that uses them.
        rocketnet_warn( "development keys are public and not for production" );
#if ROCKETNET_DEV
// clang-format off
#else // !ROCKETNET_DEV
// clang-format on
        // A production build (ROCKETNET_DEV=0) must refuse the published development keys outright.
        if ( status )
            *status = ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION;
        rocketnet_error( "development keys in a production build (ROCKETNET_DEV=0); refusing to start" );
        rocketnet_server_transport_abort_create( server_transport );
        return NULL;
#endif // #if ROCKETNET_DEV
    }

    const int granted_transport = rocketnet_server_transport_open_ladder( server_transport, context, &resolved, &bind_address, status );
    if ( granted_transport < 0 )
    {
        rocketnet_server_transport_abort_create( server_transport );
        return NULL;
    }

    server_transport->transport = granted_transport;

    // the started line names the granted tier -- performance claims attach
    // to what this says, never to what was requested

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const rocketnet_address_t * started_address = rocketnet_server_transport_io_has( server_transport->io, ROCKETNET_TRANSPORT_BINDS_PUBLIC_ADDRESS ) ? &public_address : &bind_address;
    rocketnet_info( "server transport started on %s [%s] (%s keys)", rocketnet_address_to_string( started_address, address_string ),
                    rocketnet_transport_name( granted_transport ), development_keys ? "development" : "production" );

    // a public address created with port zero adopts the granted bind
    // port, so the advertised address -- the one connect tokens are minted
    // for and validated against -- is the real one, and
    // rocketnet_server_transport_port reads it back

    if ( public_address.port == 0 )
        public_address.port = bind_address.port;
    if ( serves_both && second_address.port == 0 )
        second_address.port = bind_address.port;

    server_transport->bind_address = bind_address;
    server_transport->public_address = public_address;
    server_transport->state = ROCKETNET_SERVER_TRANSPORT_RUNNING;
    server_transport->link_simulator_from = public_address;

    // A development build creates the link simulator for every server start,
    // memory up front; all-zero settings are a pass-through that neither adds
    // latency nor copies. A production library creates none and carries none
    // of its memory (rocketnet_network_simulator_create is a stub returning
    // NULL there, and the config walk already refuses nonzero simulated values).

    if ( rocketnet_public_link_simulator_development() )
    {
        server_transport->link_simulator = rocketnet_network_simulator_create( context, ROCKETNET_SERVER_LINK_SIMULATOR_SLOTS, UINT64_C( 0x5345525645525349 ) );
        if ( !server_transport->link_simulator )
        {
            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
            rocketnet_server_transport_abort_create( server_transport );
            return NULL;
        }

        if ( !rocketnet_platform_mutex_create( &server_transport->link_simulator_mutex ) )
        {
            if ( status )
                *status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;
            rocketnet_network_simulator_destroy( server_transport->link_simulator );
            server_transport->link_simulator = NULL;
            rocketnet_server_transport_abort_create( server_transport );
            return NULL;
        }

        rocketnet_network_simulator_config( server_transport->link_simulator,
                                            config->simulated_latency_ms / 1000.0,
                                            config->simulated_jitter_ms / 1000.0,
                                            (float) config->simulated_loss_percent,
                                            (float) config->simulated_duplicate_percent );
        if ( !rocketnet_network_simulator_patterns( server_transport->link_simulator,
                                                    config->simulated_drop_every_nth,
                                                    config->simulated_reverse_window ) )
        {
            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
            rocketnet_server_transport_abort_create( server_transport );
            return NULL;
        }
    }

    // the protocol, on the configured identity, advertised at the public
    // address -- the address connect tokens are minted for

    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    protocol->protocol_id = config->protocol_id;
    protocol->send_rate = ROCKETNET_DEFAULT_SEND_RATE;
    memcpy( protocol->packet_filter, config->packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( protocol->backend_public_key, config->backend_public_key, 32 );
    memcpy( protocol->server_public_key, config->server_public_key, 32 );
    memcpy( protocol->server_secret_key, config->server_secret_key, 32 );

    if ( serves_both )
    {
        // The named addresses, not the wildcard bind. ReachesServer is the
        // arrival family being one of these.
        if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
        {
            protocol->advertised_ipv4 = public_address;
            protocol->advertised_ipv6 = second_address;
        }
        else
        {
            protocol->advertised_ipv6 = public_address;
            protocol->advertised_ipv4 = second_address;
        }
    }
    else if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
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
            memset( &protocol->advertised_ipv4, 0, sizeof( protocol->advertised_ipv4 ) );
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
    protocol->handshake_deadline_seconds = config->handshake_deadline_seconds;
    protocol->handshakes_per_update = config->handshakes_per_update;
    protocol->max_timeout_seconds = config->max_timeout_seconds;

    protocol->encryption_map = rocketnet_encryption_map_create( context, max_clients * ROCKETNET_ENCRYPTION_MAP_RATIO );
    protocol->used_token_history = rocketnet_used_token_history_create( context, max_clients * ROCKETNET_USED_TOKEN_HISTORY_RATIO );
    protocol->client_slots = rocketnet_client_slots_create( context, max_clients );
    protocol->acks = rocketnet_server_acks_create( context, max_clients );

    // the connection ids (docs/CHANNELS.md P9a, P9b): one per slot, 0 while
    // free, and one per acked batch entry, sized as the acked batch is; and
    // one end reason per slot, NONE until a connection in it ends

    protocol->connection_ids = (uint64_t *) rocketnet_malloc( context, (size_t) max_clients * sizeof( uint64_t ) );
    protocol->acked_connection_ids = (uint64_t *) rocketnet_malloc( context, (size_t) max_clients * ROCKETNET_SENT_PACKET_BUFFER_SIZE * sizeof( uint64_t ) );
    protocol->end_reasons = (int *) rocketnet_malloc( context, (size_t) max_clients * sizeof( int ) );
    server_transport->client_wire_totals = (struct rocketnet_server_wire_totals_t *) rocketnet_malloc( context, (size_t) max_clients * sizeof( struct rocketnet_server_wire_totals_t ) );
    server_transport->client_wire_totals_capacity = max_clients;
    if ( protocol->connection_ids )
        memset( protocol->connection_ids, 0, (size_t) max_clients * sizeof( uint64_t ) );
    if ( protocol->end_reasons )
        memset( protocol->end_reasons, 0, (size_t) max_clients * sizeof( int ) );     // ROCKETNET_DISCONNECT_REASON_NONE
    if ( server_transport->client_wire_totals )
        memset( server_transport->client_wire_totals, 0, (size_t) max_clients * sizeof( struct rocketnet_server_wire_totals_t ) );
    // one reassembly set per connection slot, created here at create and
    // reached only from a connected slot (STANDARD.md _Fragmentation_).
    // the delivered mirror sizes to the server's completion bound: the
    // receive path drains one global array of up to
    // ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE packets per update with no
    // per-client share, and a slot clears at completion, so one client
    // completes more payloads in an update than it has slots. half the
    // drain plus the payloads begun in an earlier update (at most
    // connections x slots) bounds the completions, and at that capacity
    // BATCH_FULL stays zero under any load the drain delivers
    // (rocketnet_server_reassembly_delivered_capacity; finding D2 of
    // proofs/tla/README-FragmentsAndAcks.md). its bytes are what one update can
    // complete -- the drained fragments plus the payloads half-held in
    // slots -- not capacity x max payload
    // (rocketnet_server_reassembly_delivered_bytes).

    protocol->reassembly = rocketnet_reassembly_set_create_delivered( context, max_clients, max_fragmented_messages, server_transport->max_payload_bytes,
                                                                      rocketnet_server_reassembly_delivered_capacity( max_clients, max_fragmented_messages ),
                                                                      rocketnet_server_reassembly_delivered_bytes( max_clients, max_fragmented_messages, server_transport->max_payload_bytes ) );
    protocol->counters = &server_transport->counters;

    // the arena's slot stride stays 16-aligned so every root handed to a
    // caller is aligned for any word-based serializer

    server_transport->send_arena = rocketnet_send_arena_create( context, ROCKETNET_SEND_ARENA_SLOTS,
                                                                ( server_transport->max_payload_bytes + 15 ) & ~15 );

    if ( server_transport->io.operations->replay_mark )
        server_transport->replay_marks = rocketnet_server_replay_marks_create( context, max_clients );

    if ( !protocol->encryption_map || !protocol->used_token_history || !protocol->client_slots || !protocol->acks || !protocol->connection_ids || !protocol->acked_connection_ids || !protocol->end_reasons || !server_transport->client_wire_totals || !protocol->reassembly || !server_transport->send_arena || ( server_transport->io.operations->replay_mark && !server_transport->replay_marks ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        rocketnet_error( "server transport create: could not allocate protocol storage" );
        rocketnet_server_transport_abort_create( server_transport );
        return NULL;
    }

    if ( status )
        *status = ROCKETNET_STATUS_OK;
    return server_transport;
}

#if ROCKETNET_DEV
int rocketnet_server_transport_secret_spans_for_test( size_t * offsets, size_t * bytes, int max )
{
    const size_t protocol = offsetof( rocketnet_server_transport_t, protocol );
    const size_t spans[4][2] = {
        { protocol + offsetof( rocketnet_server_protocol_t, server_secret_key ), ROCKETNET_KEY_BYTES },
        { protocol + offsetof( rocketnet_server_protocol_t, cookie_key ), ROCKETNET_COOKIE_KEY_BYTES },
        { protocol + offsetof( rocketnet_server_protocol_t, previous_cookie_key ), ROCKETNET_COOKIE_KEY_BYTES },
        { offsetof( rocketnet_server_transport_t, disconnect_burst_keys ), sizeof( ( (rocketnet_server_transport_t *) 0 )->disconnect_burst_keys ) },
    };

    int count = 0;
    for ( ; count < 4 && count < max; count++ )
    {
        offsets[count] = spans[count][0];
        bytes[count] = spans[count][1];
    }

    return count;
}
#endif // #if ROCKETNET_DEV

void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );

    rocketnet_assert( rocketnet_server_transport_stopped( server_transport ) );
    if ( !rocketnet_server_transport_stopped( server_transport ) )
        return;

    // the server's keys: its secret key and both cookie keys. The session
    // keys are wiped with the encryption map below; nothing else here is a
    // secret, and the OS zero-fills every page before another process gets it

    sodium_memzero( server_transport->protocol.server_secret_key, sizeof( server_transport->protocol.server_secret_key ) );
    sodium_memzero( server_transport->protocol.cookie_key, sizeof( server_transport->protocol.cookie_key ) );
    sodium_memzero( server_transport->protocol.previous_cookie_key, sizeof( server_transport->protocol.previous_cookie_key ) );
    sodium_memzero( server_transport->disconnect_burst_keys, sizeof( server_transport->disconnect_burst_keys ) );

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
    if ( server_transport->protocol.end_reasons )
        rocketnet_free( server_transport->context, server_transport->protocol.end_reasons );
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

    rocketnet_server_transport_io_close( &server_transport->io );

    void * allocation_context = server_transport->context;
    server_transport->~rocketnet_server_transport_t();
    rocketnet_free( allocation_context, server_transport );
}

void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) || server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return;
    server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPING;

    // proofs/tla/RNDriverSendStaging.tla ResetLeases: stop bumps the lease epoch.
    // A finish of a packet started before this reset is caller misuse.

    server_transport->packet_leases.reset();
    rocketnet_server_transport_io_stop( &server_transport->io );
}

bool rocketnet_server_transport_stopped( const struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) || server_transport->state == ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return false;
    return rocketnet_server_transport_io_stopped( &server_transport->io );
}

// A failed create cannot return a live partially constructed object. This
// private construction-only unwind is allowed to wait; successful objects use
// the explicit request/completion/destroy contract above.
static void rocketnet_server_transport_abort_create( struct rocketnet_server_transport_t * transport )
{
    transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPING;
    rocketnet_server_transport_io_stop( &transport->io );
    while ( !rocketnet_server_transport_stopped( transport ) )
        rocketnet_platform_sleep( 0.001 );
    rocketnet_server_transport_destroy( transport );
}

int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    if ( server_transport->state == ROCKETNET_SERVER_TRANSPORT_STOPPING && rocketnet_server_transport_stopped( server_transport ) )
        return ROCKETNET_SERVER_TRANSPORT_STOPPED;
    return server_transport->state;
}

bool rocketnet_server_transport_advertised_address( struct rocketnet_server_transport_t * server_transport, int family, struct rocketnet_address_t * address )
{
    if ( !rocketnet_api_null_guard( server_transport ) || !rocketnet_api_null_guard( address ) )
        return false;
    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return false;
    const struct rocketnet_address_t * advertised = family == ROCKETNET_ADDRESS_IPV4 ? &server_transport->protocol.advertised_ipv4 : family == ROCKETNET_ADDRESS_IPV6 ? &server_transport->protocol.advertised_ipv6
                                                                                                                                                                      : NULL;
    if ( !advertised || advertised->type != family )
        return false;
    *address = *advertised;

    return true;
}

uint16_t rocketnet_server_transport_port( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return server_transport->bind_address.port;
}

// the granted-tier report: the transport that won the ladder at create, by
// name -- what performance claims and operator errands attach to

const char * rocketnet_server_transport_io_name( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return NULL;
    rocketnet_assert( server_transport );
    return rocketnet_transport_name( server_transport->transport );
}

#if ROCKETNET_DEV

// the development seam the ICMP-error functional cases read the live pump
// and the socket through (TRANSPORTS.md, ICMP errors): the portable and msgx
// transports' instance is the portable_server_t, which owns the socket; read
// as the socket itself, the pump was whatever bytes the owner held there

struct rocketnet_platform_socket_t * rocketnet_portable_server_test_socket( void * transport );

struct rocketnet_platform_socket_t * rocketnet_server_transport_platform_socket_for_testing( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    if ( server_transport->transport == ROCKETNET_TRANSPORT_PORTABLE || server_transport->transport == ROCKETNET_TRANSPORT_MSGX )
        return rocketnet_portable_server_test_socket( server_transport->io.instance );
    return NULL;
}

#endif // #if ROCKETNET_DEV

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
            server_transport->frame, time );
    }
}

#if ROCKETNET_DEV
void rocketnet_server_transport_set_test_timestamp( struct rocketnet_server_transport_t * server_transport, uint64_t timestamp )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( timestamp != 0 );
    server_transport->test_timestamp = timestamp;
}

void rocketnet_server_transport_test_receive_work( const struct rocketnet_server_transport_t * server_transport,
                                                   uint64_t * map_hashes, uint64_t * payload_writes )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( map_hashes );
    rocketnet_assert( payload_writes );
    *map_hashes = rocketnet_encryption_map_test_hashes( server_transport->protocol.encryption_map );
    *payload_writes = server_transport->protocol.test_payload_writes;
}
#endif // #if ROCKETNET_DEV

void rocketnet_server_transport_begin_message_update( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );
    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return;
    server_transport->disconnect_seals_left = server_transport->disconnect_packets_per_update;
    server_transport->disconnect_update_prepared = 1;
}

// proofs/tla/RNDriverSendStaging.tla Update
// One received datagram, once counted: its shape and mapping, the protocol,
// the response sealed straight into a send frame, and the payload (or the
// payload a fragment completed) into this update's batch in arrival order.
// view_index names the packet in a staged transport's receive view, -1 on
// the batch path.
static void rocketnet_server_receive_one( struct rocketnet_server_transport_t * server_transport, const rocketnet_address_t * from,
                                          uint8_t * packet_data, int packet_bytes, int view_index,
                                          uint64_t current_timestamp, double current_time )
{
    const bool shape_valid = rocketnet_server_packet_shape_valid( &server_transport->protocol, packet_data, packet_bytes );
    const int mapping_index = rocketnet_server_receive_mapping_index( server_transport, shape_valid, from );
    const int wire_client_before = rocketnet_server_mapping_client_index( server_transport, mapping_index );
    rocketnet_server_wire_totals_add_received( server_transport, wire_client_before, from, packet_bytes );

    // the packet opens into the next free payload slot; with the batch
    // full it opens on the stack and its payload is not delivered

    const bool payload_slot_free = server_transport->payloads.num_payloads < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE;
    uint8_t payload_overflow[ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES];
    uint8_t * opened = payload_slot_free ? server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES : payload_overflow;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    int payload_client_index = -1;

    struct rocketnet_server_frame_t frame;
    frame.data = NULL;

    uint8_t * response = shape_valid ? rocketnet_server_reserve_send_frame( server_transport, from, &frame ) : NULL;
    uint8_t response_overflow[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    if ( response )
        response += ROCKETNET_FRAME_WIRE_OFFSET;

    const int outcome = rocketnet_server_protocol_process_received( &server_transport->protocol, from, mapping_index,
                                                                    packet_data, packet_bytes,
                                                                    current_timestamp, current_time,
                                                                    response ? response : response_overflow, &response_bytes,
                                                                    opened, ROCKETNET_RECEIVE_PAYLOAD_BYTES,
                                                                    &payload, &payload_bytes, &payload_client_index );

    // a packet that opened under a connection's keys teaches a staged
    // transport its link-layer source (the xdp reply-MAC rule, TRANSPORTS.md);
    // a dropped or unauthenticated packet (a cookie or a challenge answer)
    // teaches nothing
    if ( view_index >= 0 && ( outcome == ROCKETNET_SERVER_PROCESS_ACCEPTED || outcome == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE ) &&
         server_transport->io.operations->client_accepted )
        server_transport->io.operations->client_accepted( server_transport->io.instance, view_index );

    rocketnet_assert( shape_valid || ( response_bytes == 0 && payload_bytes == 0 ) );

    frame.client_index = rocketnet_server_wire_client_after( server_transport, wire_client_before, mapping_index, outcome );

    if ( response )
    {
        if ( response_bytes > 0 )
            rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
        else
            rocketnet_server_abort_send_frame( server_transport, &frame );
    }

    if ( payload_slot_free && payload && payload_bytes > 0 )
    {
        // the payload's bytes inside the slot it opened into, which the
        // transport owns: delivered in place, never copied (opened is the
        // writable view of the same bytes)
        const int index = server_transport->payloads.num_payloads;
        server_transport->payloads.client_index[index] = payload_client_index;
        server_transport->payloads.connection_id[index] = server_transport->protocol.payload_connection_id;
        server_transport->payloads.payload_data[index] = opened + ( payload - opened );
        server_transport->payloads.payload_bytes[index] = payload_bytes;
        server_transport->payloads.sequence[index] = server_transport->protocol.payload_sequence;
        server_transport->payloads.num_payloads++;
    }

    // a fragment that completed its payload delivers here, in arrival
    // position: the reassembled bytes live in the reassembly's delivered
    // arena, valid until the next update, and join the same batch
    // attributed to their client slot

    struct rocketnet_reassembly_set_t * reassembly = server_transport->protocol.reassembly;
    if ( reassembly->completed_bytes > 0 )
    {
        if ( server_transport->payloads.num_payloads < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE )
        {
            const int index = server_transport->payloads.num_payloads;
            server_transport->payloads.client_index[index] = payload_client_index;
            server_transport->payloads.connection_id[index] = server_transport->protocol.payload_connection_id;
            server_transport->payloads.payload_data[index] = reassembly->completed_payload;
            server_transport->payloads.payload_bytes[index] = reassembly->completed_bytes;
            server_transport->payloads.sequence[index] = 0;
            server_transport->payloads.num_payloads++;
        }
        else
        {
            rocketnet_spam( "server dropped reassembled payload. receive batch full" );
            server_transport->counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL]++;
        }
    }
}

void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport, double time )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );

    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        if ( rocketnet_server_transport_stopped( server_transport ) )
            server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPED;
        return;
    }

    // the caller's clock is clamped once, before anything reads it: a NaN,
    // an infinity or a step backwards becomes the last good time, counted and
    // loud in a debug build. the last good time starts at zero, the value a
    // value-initialized transport carries.

    bool time_clamped = false;
    time = rocketnet_update_clock_advance( server_transport->time, &server_transport->raw_time, &server_transport->time_offset, time, &time_clamped );

    // proofs/tla/RNDriverSendStaging.tla ResetLeases: update bumps the lease epoch
    // before it reads. A fill still open here was not joined.

    server_transport->packet_leases.reset();

    server_transport->time = time;
    rocketnet_server_link_drain( server_transport );

    rocketnet_server_update_stats( server_transport, time );

    const double current_time = time;
    // the token clock is the epoch plus the caller's time, in whole seconds. An
    // epoch of zero is resolved here, once, from the wall clock less the
    // caller's time; nothing reads the wall clock again, so a paused caller
    // clock pauses token expiry

    if ( server_transport->token_epoch_unix_seconds == 0 )
        server_transport->token_epoch_unix_seconds = rocketnet_platform_token_epoch( 0, current_time );

#if ROCKETNET_DEV
    const uint64_t current_timestamp = server_transport->test_timestamp ? server_transport->test_timestamp : server_transport->token_epoch_unix_seconds + (uint64_t) current_time;
#else // #if ROCKETNET_DEV
    const uint64_t current_timestamp = server_transport->token_epoch_unix_seconds + (uint64_t) current_time;
#endif // #if ROCKETNET_DEV

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

            rocketnet_server_receive_one( server_transport, &received->from[i], received->packet_data[i], received->packet_bytes[i], i, current_timestamp, current_time );
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

        // a count past the slots asked for, or a byte count that is negative
        // or beyond the UDP envelope, is a transport fault and is not a received packet
            if ( num_received < 0 )
            {
                server_transport->transport_faults++;
                num_received = 0;
            }
            else if ( num_received > batch_capacity )
            {
                server_transport->transport_faults++;
                num_received = batch_capacity;
            }

            for ( int i = 0; i < num_received; i++ )
            {
                struct rocketnet_address_t from = receive_buffer->from[i];
                uint8_t * receive_frame = receive_buffer->data + i * ROCKETNET_RECEIVE_CEILING;
                const int packet_bytes = receive_buffer->packet_bytes[i];
                if ( packet_bytes < 0 || packet_bytes > ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES )
                {
                    server_transport->transport_faults++;
                    continue;
                }

                server_transport->stats_frames[stats_index].packets_received++;
                server_transport->stats_frames[stats_index].bytes_received += (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( from.type ) );

            // A truncated datagram retains its original length for accounting,
            // but only the shorter prefix is stored. Reject an oversize report
            // before the protocol reads that stored prefix as the whole packet.
                if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
                {
                    rocketnet_server_wire_totals_add_received( server_transport, -1, &from, packet_bytes );
                    server_transport->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED]++;
                    server_transport->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE]++;
                    continue;
                }

                rocketnet_server_receive_one( server_transport, &from, receive_frame, packet_bytes, -1, current_timestamp, current_time );
            }

            packets_processed += num_received;
        } while ( num_received == batch_capacity && packets_processed < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE );
    }

    // proofs/tla/RNChannel.tla FixBatch and proofs/tla/RNWindow.tla FixBatch: the received
    // batch is fixed here, before this update builds any reply or ack, so no
    // packet opened later can join it.

    // the sweep, then per client: the paced keep-alive or -- when the ack
    // debt has reached the threshold with no other block going out -- the
    // standalone ack, into send frames. The per-slot ACK statistics need no
    // pass here: the running aggregates keep them current as each send,
    // receive and ack lands (sdk/rocketnet_ack.cpp, proofs/tla/RNServerStats.tla).

    rocketnet_server_protocol_update( &server_transport->protocol, current_time );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    const int max_clients = rocketnet_client_slots_max_clients( server_transport->protocol.client_slots );
    const int first_client = server_transport->maintenance_next_client;

    for ( int offset = 0; offset < max_clients; offset++ )
    {
        const int i = ( first_client + offset ) % max_clients;
        server_transport->maintenance_next_client = i;
        if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, i ) )
            continue;
        if ( !rocketnet_server_protocol_maintenance_due( &server_transport->protocol, i, current_time ) )
        {
            server_transport->maintenance_next_client = ( i + 1 ) % max_clients;
            continue;
        }

        int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, i );
        if ( mapping_index < 0 )
        {
            // This internally permitted slot state has no address to stage.
            rocketnet_server_protocol_note_missing_mapping( &server_transport->protocol );
            server_transport->maintenance_next_client = ( i + 1 ) % max_clients;
            continue;
        }

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
        server_transport->maintenance_next_client = ( i + 1 ) % max_clients;
    }

    // proofs/tla/ServerSession.tla DrainDisconnectBurst: the public message-server
    // update opens the allowance before its pre-link kicks. A direct
    // transport update opens it here instead. Either way those disconnects
    // and this drain share exactly one allowance.

    if ( !server_transport->disconnect_update_prepared )
        server_transport->disconnect_seals_left = server_transport->disconnect_packets_per_update;
    server_transport->disconnect_update_prepared = 0;
    if ( server_transport->disconnect_burst_count > 0 && server_transport->disconnect_seals_left > 0 )
    {
        const uint64_t before = server_transport->disconnect_seals;
        rocketnet_server_transport_drain_disconnect_bursts( server_transport, server_transport->disconnect_seals_left, false );
        server_transport->disconnect_seals_left -= (int) ( server_transport->disconnect_seals - before );
    }

    acks->update_count++;

    // The kernel replay walk and commit: after the maintenance sweep, the
    // transport tells the granted transport which client addresses to forget and
    // mark, then commits them. proofs/tla/RNXdpReplay.tla Walk and Commit.

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

    // Transports with FLUSH_ON_UPDATE publish update-generated responses here.
    // Other staged transports publish at send_packets after the joined fill phase.
    // XDP retains published generations until the worker drains them; the
    // staged/flushed counters avoid redundant calls on synchronous transports;
    // send_packets retries XDP publication even when those counters agree.

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        server_transport->send_staged++;

        if ( rocketnet_server_transport_io_has( server_transport->io, ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE ) )
        {
            server_transport->send_flushed = server_transport->send_staged;
            rocketnet_server_transport_io_send_packets( server_transport );
        }

        rocketnet_server_link_flush( server_transport );
        return;
    }

    rocketnet_server_flush_control_buffer( server_transport );
    rocketnet_server_link_flush( server_transport );
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

    memset( context, 0, sizeof( struct rocketnet_packet_context_t ) );

    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return NULL;

    if ( request_bytes < 1 || request_bytes > server_transport->max_payload_bytes )
    {
        rocketnet_spam( "server start packet refused. request out of range" );
        return NULL;
    }

    // Reserve the frame and packet lease here, but leave the session sequence
    // untouched. finish_packet assigns it only after the caller's size is
    // accepted and every wire frame is reserved, keeping accepted server
    // sequence numbers contiguous. Client loss is measured from outbound ACKs.

    rocketnet_address_t to;
    if ( !rocketnet_server_protocol_client_address( &server_transport->protocol, client_index, &to ) )
        return NULL;

    uint8_t * frame = NULL;

    if ( request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
    {
        // above the wire's capacity: the arena backs the write, and
        // finish_packet splits into fragments -- or collapses to one wire
        // packet when the write turns out small. The lease supplies its
        // public packet id independently of the transport.

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
        context->sequence = 0;
        context->client_index = client_index;

        // proofs/tla/RNDriverSendStaging.tla StartPacket: the caller holds this lease
        // until finish, abort, or a reset.

        if ( !server_transport->packet_leases.insert( *context ) )
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
        frame = rocketnet_server_transport_io_start_packet( server_transport, &to, &packet_id );
        if ( !frame )
        {
            if ( packet_id != ROCKETNET_TRANSPORT_PACKET_UNREACHABLE )
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
        if ( slot >= (uint64_t) server_transport->send_capacity )
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
    context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    context->sequence = 0;
    context->client_index = client_index;

    // proofs/tla/RNDriverSendStaging.tla StartPacket: the caller holds this lease
    // until finish, abort, or a reset.

    if ( !server_transport->packet_leases.insert( *context ) )
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
        rocketnet_server_transport_io_abort_packet( server_transport, backend_id, context->frame );
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
    if ( capacity )
        *capacity = 0;
    if ( !transport || !context || !capacity )
        return NULL;
    rocketnet_packet_context_t trusted;
    uint64_t backend_id = 0;
    if ( !transport->packet_leases.resolve( *context, trusted, backend_id, false ) )
        return NULL;
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
    if ( server_transport->packet_leases.take( *context, trusted, backend_id ) )
        rocketnet_server_transport_abort_packet_unchecked( server_transport, &trusted, backend_id );
    context->frame = NULL;
}

// The fill-phase wire frame behind the arena finish: one sealed packet's
// destination on any transport, reserved before sealing so a split goes
// out whole or not at all. On the portable transport the frame is a payload
// ring slot; on the staged transports it is a transport frame with its own packet
// id for the transport's bookkeeping -- the application's join key stays the
// context's.

struct rocketnet_server_fill_frame_t
{
    uint8_t * data;
    uint64_t socket_packet_id;      // the staged transports' bookkeeping id
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
        frame->data = rocketnet_server_transport_io_start_packet( server_transport, to, &frame->socket_packet_id );
        if ( !frame->data )
        {
            if ( frame->socket_packet_id != ROCKETNET_TRANSPORT_PACKET_UNREACHABLE )
                rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return 0;
        }

        rocketnet_platform_atomic_add_u64( &server_transport->send_staged, 1 );
        return 1;
    }

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    frame->slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( frame->slot >= (uint64_t) server_transport->send_capacity )
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

// The staged sent charge, from any game thread: relaxed atomics folded into
// the current bucket by send_packets on the main thread.
static void rocketnet_server_charge_staged_frame( struct rocketnet_server_transport_t * server_transport,
                                                  int client_index, int address_type, int wire_bytes )
{
    rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_packets_sent, 1 );
    rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_bytes_sent, (uint64_t) ( wire_bytes + rocketnet_wire_header_bytes( address_type ) ) );
    rocketnet_server_wire_totals_add_sent_atomic( server_transport, client_index, address_type, wire_bytes );
}

// Returns true when a staged frame left (the staging finished it or the link
// took it) and is the caller's to charge with
// rocketnet_server_charge_staged_frame; a portable frame returns false and is
// charged by the flush, once the link took it. A frame the link was handed
// is back in the staging either way, so its data is cleared and a later
// withdrawal does not return it twice.
static bool rocketnet_server_commit_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                                struct rocketnet_server_fill_frame_t * frame, int wire_bytes )
{
    rocketnet_assert( server_transport && frame && frame->data && wire_bytes > 0 && wire_bytes <= ROCKETNET_RECEIVE_CEILING );

    if ( !server_transport || !frame || !frame->data || wire_bytes <= 0 || wire_bytes > ROCKETNET_RECEIVE_CEILING )
        return false;
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        if ( rocketnet_server_link_active( server_transport ) )
        {
            const bool sent = rocketnet_server_link_send( server_transport, &frame->to,
                                                          frame->data + ROCKETNET_FRAME_WIRE_OFFSET, wire_bytes );
            rocketnet_server_transport_io_abort_packet( server_transport, frame->socket_packet_id, frame->data );
            frame->data = NULL;
            return sent;
        }

        rocketnet_server_transport_io_finish_packet( server_transport, frame->socket_packet_id, frame->data, wire_bytes );
        return true;
    }

    server_transport->payload_ring.info[frame->slot].wire_bytes = wire_bytes;

    return false;
}

static void rocketnet_server_abort_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                               struct rocketnet_server_fill_frame_t * frame )
{
    if ( !frame->data )
        return;
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_transport_io_abort_packet( server_transport, frame->socket_packet_id, frame->data );
        frame->data = NULL;
        return;
    }

    server_transport->payload_ring.info[frame->slot].wire_bytes = 0;
    frame->data = NULL;
}

// proofs/tla/FragmentsAndAcks.tla BurnOne: a small payload whose seal fails after its sequence was taken leaves that sequence unsent.
// proofs/tla/FragmentsAndAcks.tla BurnFragmented: a seal refusal after the fragment reservation, payload id and aggregate withdraws the frames and sends nothing.
// The arena finish: the request was over the wire's capacity, so the
// payload sits contiguous in an arena slot. A write that turned out to fit
// one packet collapses to a single ordinary payload -- one copy into one
// wire frame, cheaper than fragmenting -- and a genuinely large one splits
// into type 9 fragments. Every wire frame is reserved before anything
// seals, so a reservation shortfall drops the payload whole; a seal
// refusal mid-split -- possible only through a fill-contract violation,
// since connections are stable during a fill -- withdraws every reserved
// frame on the portable and gso transports, whose staging is untouched until
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

    if ( payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
    {
        struct rocketnet_server_fill_frame_t frame;
        if ( !rocketnet_server_reserve_fill_frame( server_transport, &to, context->client_index, &frame ) )
        {
            context->frame = NULL;
            return 0;
        }

        memcpy( frame.data + ROCKETNET_FRAME_PAYLOAD_OFFSET, context->frame, (size_t) payload_bytes );

        if ( !rocketnet_server_protocol_start_payload( &server_transport->protocol, context->client_index, &to, &context->sequence ) )
        {
            rocketnet_server_abort_fill_frame( server_transport, &frame );
            context->frame = NULL;
            return 0;
        }

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

        // a datagram is counted sent only once the link took it

        if ( rocketnet_server_commit_fill_frame( server_transport, &frame, wire_bytes ) )
            rocketnet_server_charge_staged_frame( server_transport, frame.client_index, frame.to.type, wire_bytes );
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

    // Reserve the whole sequence block only after every wire frame is ready.
    // Fragment 0 uses the first sequence, preserving the wire order without
    // spending any sequence when start or finish cannot complete.

    uint64_t base_sequence = 0;
    if ( !rocketnet_server_protocol_start_fragments( &server_transport->protocol, context->client_index, fragment_count, &base_sequence ) )
    {
        for ( int i = 0; i < fragment_count; i++ )
            rocketnet_server_abort_fill_frame( server_transport, &frames[i] );
        context->frame = NULL;
        return 0;
    }

    context->sequence = base_sequence;

    uint32_t payload_id = rocketnet_server_protocol_next_fragment_payload_id( &server_transport->protocol, context->client_index );
    uint64_t aggregate = rocketnet_ack_endpoint_open_aggregate( &server_transport->protocol.acks->endpoints[context->client_index],
                                                                context->packet_id, fragment_count );

    int total_wire_bytes = 0;
    int fragment_wire_bytes[ROCKETNET_MAX_FRAGMENTS];
    bool fragment_sent[ROCKETNET_MAX_FRAGMENTS];

    for ( int i = 0; i < fragment_count; i++ )
    {
        const uint64_t sequence = base_sequence + (uint64_t) i;
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

            const int first_withdrawn = rocketnet_server_transport_io_has( server_transport->io, ROCKETNET_TRANSPORT_FINISH_FINAL ) ? i : 0;

            // a datagram is counted sent only once the link took it, and a
            // withdrawn frame never left: charge the frames that stay
            // committed and the ones the link already took (their data is
            // cleared, so the withdrawal below passes over them)

            for ( int j = 0; j < i; j++ )
            {
                if ( fragment_sent[j] && ( j < first_withdrawn || !frames[j].data ) )
                    rocketnet_server_charge_staged_frame( server_transport, frames[j].client_index, frames[j].to.type, fragment_wire_bytes[j] );
            }

            for ( int j = first_withdrawn; j < fragment_count; j++ )
                rocketnet_server_abort_fill_frame( server_transport, &frames[j] );
            context->frame = NULL;
            return 0;
        }

        fragment_sent[i] = rocketnet_server_commit_fill_frame( server_transport, &frames[i], wire_bytes );
        fragment_wire_bytes[i] = wire_bytes;
        total_wire_bytes += wire_bytes;
    }

    // a datagram is counted sent only once the link took it: the fragments
    // are charged after the whole split is committed, so a withdrawal never
    // leaves a charge behind

    for ( int i = 0; i < fragment_count; i++ )
    {
        if ( fragment_sent[i] )
            rocketnet_server_charge_staged_frame( server_transport, frames[i].client_index, frames[i].to.type, fragment_wire_bytes[i] );
    }

    context->frame = NULL;

    return total_wire_bytes;
}

// proofs/tla/FragmentsAndAcks.tla BurnOne: a seal refusal after the sequence was taken leaves that sequence unsent.
static int rocketnet_server_transport_finish_packet_unchecked( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes, uint64_t backend_id )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    if ( !server_transport || !context )
        return 0;

    if ( !context->frame )
    {
        rocketnet_assert( context->frame != NULL );
        return 0;
    }

    if ( payload_bytes < 1 || payload_bytes > context->payload_capacity )
    {
        rocketnet_server_transport_abort_packet_unchecked( server_transport, context, backend_id );
        return 0;
    }

    if ( rocketnet_send_arena_owns( server_transport->send_arena, context->frame ) )
        return rocketnet_server_transport_finish_arena_packet( server_transport, context, payload_bytes );

    rocketnet_address_t to;
    if ( !rocketnet_server_protocol_start_payload( &server_transport->protocol, context->client_index, &to, &context->sequence ) )
    {
        rocketnet_server_transport_abort_packet_unchecked( server_transport, context, backend_id );
        return 0;
    }

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

        // a connected slot may hold mapping index -1 (see rocketnet_server_connected_mapping)

        if ( mapping_index < 0 )
        {
            rocketnet_server_transport_abort_packet_unchecked( server_transport, context, backend_id );
            return 0;
        }

        const rocketnet_address_t * mapped_to = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );
        bool sent = true;

        if ( rocketnet_server_link_active( server_transport ) )
        {
            sent = rocketnet_server_link_send( server_transport, mapped_to,
                                               context->frame + ROCKETNET_FRAME_WIRE_OFFSET, wire_bytes );
            rocketnet_server_transport_io_abort_packet( server_transport, backend_id, context->frame );
        }
        else
            rocketnet_server_transport_io_finish_packet( server_transport, backend_id, context->frame, wire_bytes );
        // a datagram is counted sent only once the link took it

        if ( sent )
            rocketnet_server_charge_staged_frame( server_transport, context->client_index,
                                                  mapped_to ? mapped_to->type : ROCKETNET_ADDRESS_NONE, wire_bytes );
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
    if ( !server_transport->packet_leases.take( *context, trusted, backend_id ) )
    {
        // proofs/tla/RNDriverSendStaging.tla FinishPacket: a lease reset by update,
        // send_packets or stop is caller misuse. Refuse in every build.
        // Return 0, count api_misuses, and log. Do not assert: a debug-only
        // trap compiled out of release is not a refusal the caller can see.
        // The frame is not sealed. Any other miss (no start, a second finish,
        // a finish after abort, a modified or wrong-owner context) still asserts.
        if ( server_transport->packet_leases.retired( *context ) )
        {
            rocketnet_api_misuse_record();
            rocketnet_error( "server finish packet refused: the lease was reset by update, send_packets or stop" );
            context->frame = NULL;
            return 0;
        }

        rocketnet_assert( false );
        context->frame = NULL;
        return 0;
    }

    const int result = rocketnet_server_transport_finish_packet_unchecked( server_transport, &trusted, payload_bytes, backend_id );
    context->frame = NULL;
    context->sequence = trusted.sequence;

    return result;
}

// proofs/tla/FragmentsAndAcks.tla Flush: puts this update's staged packets on the wire in fill order.
static void rocketnet_server_transport_send_packets_impl( struct rocketnet_server_transport_t * server_transport, bool shutdown )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );
    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return;
    // proofs/tla/RNDriverSendStaging.tla ResetLeases: send_packets bumps the lease
    // epoch before it flushes. Same bump as update and stop.

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

        // RNDriverSendStaging.Publish: XDP may retain the producer
        // generation while its worker owns the other buffer, gso retains its
        // producer when the try-publish meets the TX worker on the handoff
        // lock or a full FIFO, and io_uring keeps its index while sends wait
        // for their CQEs. Re-flush even without a new commit, so the next
        // send boundary retries that work rather than the next update. Each
        // native flush is non-blocking: a try-publish or a bounded reap.

        // proofs/tla/RNDriverSendStaging.tla SendPackets

        if ( rocketnet_server_transport_io_has( server_transport->io, ROCKETNET_TRANSPORT_REFLUSHES ) ||
             server_transport->send_staged != server_transport->send_flushed )
        {
            server_transport->send_flushed = server_transport->send_staged;
            rocketnet_server_transport_io_send_packets( server_transport );
        }

        rocketnet_server_link_flush( server_transport );

        rocketnet_send_arena_reset( server_transport->send_arena );
        return;
    }

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    uint64_t num_packets = ring->num_packets;
    if ( num_packets > (uint64_t) server_transport->send_capacity )
        num_packets = (uint64_t) server_transport->send_capacity;

    struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
    int send_client_index[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
    bool accepted[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
    int num_sends = 0;

    for ( uint64_t i = 0; i < num_packets; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            sends[num_sends].to = &ring->info[i].to;
            sends[num_sends].packet_data = ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET;
            sends[num_sends].packet_bytes = ring->info[i].wire_bytes;
            send_client_index[num_sends] = ring->info[i].client_index;
            num_sends++;
        }
    }

    if ( num_sends > 0 )
    {
        rocketnet_server_link_send_batch( server_transport, sends, num_sends, accepted, shutdown );

        // a datagram is counted sent only once the link took it: each send
        // is charged on its own result, so the per-client totals stay per send

        for ( int i = 0; i < num_sends; i++ )
        {
            if ( accepted[i] )
                rocketnet_server_charge_send_frame( server_transport, send_client_index[i], sends[i].to, sends[i].packet_bytes );
        }
    }

    ring->num_packets = 0;

    // The disconnect_client header promise: buffered control frames leave on
    // the next flush even when this fill phase sent no payload-ring packet.

    rocketnet_server_flush_control_buffer( server_transport, shutdown );

    rocketnet_server_link_flush( server_transport );

    rocketnet_send_arena_reset( server_transport->send_arena );
}

void rocketnet_server_transport_send_packets( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_server_transport_send_packets_impl( server_transport, false );
}

// Internal public-server stop path: the last disconnect burst must not wait
// for a loopback receive worker that holds the loopback hub mutex.
void rocketnet_server_transport_send_packets_shutdown( struct rocketnet_server_transport_t * server_transport )
{
    // the stop path seals every waiting burst: nothing is left for an update
    // that will not come
    if ( server_transport && server_transport->disconnect_burst_count > 0 )
        rocketnet_server_transport_drain_disconnect_bursts( server_transport, ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_MAX_CLIENTS, true );
    rocketnet_server_transport_send_packets_impl( server_transport, true );
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

    memcpy( frame + context.payload_index, payload_data, (size_t) payload_bytes );

    rocketnet_server_transport_finish_packet( server_transport, &context, payload_bytes );
}

// proofs/tla/ServerSession.tla DisconnectAll: the slot ends in this step. What the
// burst needs from the mapping is copied into the ring first -- the ack block
// built once from the receive state, as every packet of the immediate burst
// opens with the same block -- then the slot is released exactly as
// rocketnet_server_protocol_disconnect_client releases it. O(1) per slot: no
// seal and no send here.
static void rocketnet_server_transport_close_client_locally( struct rocketnet_server_transport_t * server_transport,
                                                             int client_index, bool missing_mapping )
{
    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    if ( missing_mapping )
        rocketnet_server_protocol_note_missing_mapping( protocol );
    rocketnet_debug( "server disconnected client %d", client_index );
    protocol->counters->values[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT]++;
    protocol->counters->values[ROCKETNET_COUNTER_CLIENT_DISCONNECTED]++;
    rocketnet_server_connection_reset( protocol, client_index, ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
}

static void rocketnet_server_transport_defer_disconnect_burst( struct rocketnet_server_transport_t * server_transport, int client_index,
                                                               int mapping_index, const rocketnet_address_t * client_address )
{
    rocketnet_assert( server_transport->disconnect_burst_count < ROCKETNET_MAX_CLIENTS );

    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( protocol->encryption_map, mapping_index );
    const int index = ( server_transport->disconnect_burst_head + server_transport->disconnect_burst_count ) % ROCKETNET_MAX_CLIENTS;
    struct rocketnet_server_disconnect_burst_t * burst = &server_transport->disconnect_bursts[index];

    burst->plaintext_bytes = rocketnet_session_ack_block_build( burst->plaintext, 0, &mapping->accepted_session );
    burst->to = *client_address;
    // the whole burst's sequences are taken in one atomic add, so nothing
    // else sealed under this key can carry one of them
    burst->first_sequence = rocketnet_platform_atomic_add_u64( &mapping->send_sequence_session, (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    burst->sealed = 0;
    memcpy( burst->connection_salt, mapping->connection_salt, sizeof( burst->connection_salt ) );
    memcpy( server_transport->disconnect_burst_keys[index], mapping->keys.session_server_to_client, ROCKETNET_KEY_BYTES );
    server_transport->disconnect_burst_count++;

    rocketnet_server_transport_close_client_locally( server_transport, client_index, false );
}

// proofs/tla/ServerSession.tla DrainDisconnectBurst, each seal one SealStep: at most max_seals waiting
// disconnect packets are sealed into send frames, one per waiting burst per
// pass in ring order, so a burst's packets leave at consecutive sequences as
// the immediate burst's do. A burst whose last packet is sealed leaves the
// ring and its key is wiped. A full staging ends the drain; what is left
// waits for the next update.
static void rocketnet_server_transport_drain_disconnect_bursts( struct rocketnet_server_transport_t * server_transport, int max_seals, bool shutdown )
{
    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    int committed = 0;
    while ( server_transport->disconnect_burst_count > 0 && committed < max_seals )
    {
        if ( server_transport->disconnect_burst_cursor >= server_transport->disconnect_burst_count )
            server_transport->disconnect_burst_cursor = 0;
        const int index = ( server_transport->disconnect_burst_head + server_transport->disconnect_burst_cursor ) % ROCKETNET_MAX_CLIENTS;
        struct rocketnet_server_disconnect_burst_t * burst = &server_transport->disconnect_bursts[index];

        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, &burst->to, &frame, shutdown );
        if ( !frame_data )
            break;
        // The connection ended before this deferred frame was sealed. Its
        // slot may already name another connection, so the datagram belongs
        // only to the server-lifetime wire totals. The immediate path still
        // charges its slot because it commits before releasing that slot.

        frame.client_index = -1;

        const int bytes = rocketnet_write_sealed_packet( frame_data + ROCKETNET_FRAME_WIRE_OFFSET, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
                                                         protocol->packet_filter, burst->first_sequence + (uint64_t) burst->sealed,
                                                         NULL, burst->plaintext, burst->plaintext_bytes,
                                                         protocol->protocol_id, burst->connection_salt,
                                                         NULL, server_transport->disconnect_burst_keys[index] );
        rocketnet_assert( bytes >= ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );

        if ( bytes <= 0 )
        {
            rocketnet_server_abort_send_frame( server_transport, &frame );
            break;
        }

        rocketnet_server_commit_send_frame( server_transport, &frame, bytes );
        committed++;
        server_transport->disconnect_seals++;
        burst->sealed++;
        server_transport->disconnect_burst_cursor++;

        // sealed counts fall from the head to the tail, so the head's burst
        // is the first to complete

        while ( server_transport->disconnect_burst_count > 0 &&
                server_transport->disconnect_bursts[server_transport->disconnect_burst_head].sealed == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS )
        {
            sodium_memzero( server_transport->disconnect_burst_keys[server_transport->disconnect_burst_head], ROCKETNET_KEY_BYTES );
            server_transport->disconnect_burst_head = ( server_transport->disconnect_burst_head + 1 ) % ROCKETNET_MAX_CLIENTS;
            server_transport->disconnect_burst_count--;
            if ( server_transport->disconnect_burst_cursor > 0 )
                server_transport->disconnect_burst_cursor--;
        }
    }

    if ( committed && rocketnet_server_transport_staged( server_transport ) )
        server_transport->send_staged++;
}

#if ROCKETNET_DEV
uint64_t rocketnet_server_transport_disconnect_seals_for_test( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->disconnect_seals;
}

// The burst key lives at a ring index, the same index drain wipes
// (disconnect_burst_keys[disconnect_burst_head]), not at the client slot.
// Copies ROCKETNET_KEY_BYTES from that ring slot. head and count may be NULL.
int rocketnet_server_transport_disconnect_burst_key_for_test( struct rocketnet_server_transport_t * server_transport, int index, uint8_t * out, int out_bytes, int * head, int * count )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( out );

    if ( index < 0 || index >= ROCKETNET_MAX_CLIENTS || out_bytes < ROCKETNET_KEY_BYTES )
        return 0;
    memcpy( out, server_transport->disconnect_burst_keys[index], ROCKETNET_KEY_BYTES );
    if ( head )
        *head = server_transport->disconnect_burst_head;
    if ( count )
        *count = server_transport->disconnect_burst_count;
    return 1;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNDriverSendStaging.tla DisconnectClient
static void rocketnet_server_transport_disconnect_client_impl( struct rocketnet_server_transport_t * server_transport, int client_index, bool shutdown )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return;
    rocketnet_assert( server_transport );

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
        return;

    // the address must be taken before the protocol call: the disconnect
    // releases the mapping that holds it

    int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, client_index );

    // a connected slot may hold mapping index -1 (see rocketnet_server_connected_mapping):
    // the slot still ends, but no burst is staged, since there is no address to send it to

    if ( mapping_index < 0 )
    {
        rocketnet_server_transport_close_client_locally( server_transport, client_index, true );
        return;
    }

    rocketnet_address_t client_address = *rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );

    // proofs/tla/ServerSession.tla DisconnectAll and DrainDisconnectBurst: the slot
    // ends now; its burst is sealed now only while this update's allowance
    // holds the whole burst and no earlier burst waits, else it waits in the
    // ring for the following updates. The stop path seals at once.

    if ( !shutdown && ( server_transport->disconnect_burst_count > 0 ||
                        server_transport->disconnect_seals_left < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS ) )
    {
        if ( server_transport->disconnect_burst_count < ROCKETNET_MAX_CLIENTS )
            rocketnet_server_transport_defer_disconnect_burst( server_transport, client_index, mapping_index, &client_address );
        else
        {
            // Every retained entry owns an ended generation's key. Keep that
            // work and drop only this new burst's redundant notification
            // copies. Reset wipes the mapping key without ever copying or
            // counting a packet, and the local connection still ends now.
            rocketnet_server_transport_close_client_locally( server_transport, client_index, false );
        }

        return;
    }

    uint8_t burst[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_server_protocol_disconnect_client( &server_transport->protocol, client_index, burst, &burst_count, &burst_bytes );
    server_transport->disconnect_seals += (uint64_t) burst_count;
    server_transport->disconnect_seals_left -= burst_count < server_transport->disconnect_seals_left ? burst_count : server_transport->disconnect_seals_left;

    int committed = 0;

    for ( int i = 0; i < burst_count; i++ )
    {
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, &client_address, &frame, shutdown );
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

void rocketnet_server_transport_disconnect_client( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_server_transport_disconnect_client_impl( server_transport, client_index, false );
}

void rocketnet_server_transport_disconnect_client_shutdown( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_server_transport_disconnect_client_impl( server_transport, client_index, true );
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

// the slot holds its copy of the token's user data from assign until release,
// which wipes it (rocketnet_client_slots_release_exact; proofs/tla/RNDestroyWipe.tla
// Remove). This read changes no state; a free slot reads zeros here and never
// reaches the slot accessor, which asserts connected

bool rocketnet_server_transport_client_user_data( struct rocketnet_server_transport_t * server_transport, int client_index, uint8_t * user_data )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return false;
    rocketnet_assert( server_transport );
    rocketnet_assert( user_data );

    const struct rocketnet_client_slots_t * slots = server_transport->protocol.client_slots;
    if ( !rocketnet_client_slots_connected( slots, client_index ) )
    {
        memset( user_data, 0, ROCKETNET_USER_DATA_BYTES );
        return false;
    }

    memcpy( user_data, rocketnet_client_slots_user_data( slots, client_index ), ROCKETNET_USER_DATA_BYTES );

    return true;
}

// docs/CHANNELS.md P9a: the connection that holds the slot now

uint64_t rocketnet_server_transport_client_connection_id( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return 0;
    rocketnet_assert( server_transport );
    return rocketnet_server_protocol_connection_id( &server_transport->protocol, client_index );
}

int rocketnet_server_transport_client_end_reason( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
        return ROCKETNET_DISCONNECT_REASON_NONE;
    rocketnet_assert( server_transport );
    return rocketnet_server_protocol_end_reason( &server_transport->protocol, client_index );
}

#if ROCKETNET_DEV
// development library only: the transport instance behind the transport, for a
// functional case that drives a native transport's test seam
void * rocketnet_server_transport_io_name_for_testing( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->io.instance;
}
#endif // #if ROCKETNET_DEV

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

    // SEND_FAILED is the transport's own cumulative count, assigned on read

    if ( server_transport->io.operations && server_transport->io.operations->send_failures )
        server_transport->counters.values[ROCKETNET_COUNTER_SEND_FAILED] = server_transport->io.operations->send_failures( server_transport->io.instance );
    // a transport's own size pass drops before the prefilter sees a datagram:
    // its growth lands on the prefilter size counters, so one oversize
    // datagram reads PREFILTER_TOO_LARGE on every transport

    uint64_t too_short = 0;
    uint64_t too_large = 0;
    if ( rocketnet_server_transport_io_receive_drops( server_transport->io.operations, server_transport->io.instance, &too_short, &too_large ) )
    {
        server_transport->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] += too_short - server_transport->transport_receive_too_short;
        server_transport->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] += too_large - server_transport->transport_receive_too_large;
        server_transport->transport_receive_too_short = too_short;
        server_transport->transport_receive_too_large = too_large;
    }

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
    memset( stats, 0, sizeof( struct rocketnet_server_transport_stats_t ) );
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

    memset( stats, 0, sizeof( struct rocketnet_server_transport_client_stats_t ) );

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
        struct rocketnet_server_wire_totals_t * totals = &server_transport->client_wire_totals[client_index];
        stats->packets_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &totals->packets_sent );
        stats->packets_received = totals->packets_received;
        stats->bytes_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &totals->bytes_sent );
        stats->bytes_received = totals->bytes_received;
    }
}

#if ROCKETNET_DEV
// The transport's half of the delivery capture (rocketnet_layers.h): plain
// copies of the slot's mapping and ack endpoint, on the main thread outside
// update/fill. A slot with no connected client or no mapping leaves the
// capture untouched, so it reads as not connected.
void rocketnet_server_transport_delivery_capture( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( server_transport ) || !rocketnet_api_null_guard( capture ) )
        return;
    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    if ( !protocol->acks || client_index < 0 || client_index >= protocol->acks->max_clients )
        return;
    if ( !rocketnet_client_slots_connected( protocol->client_slots, client_index ) )
        return;
    const int mapping_index = rocketnet_client_slots_mapping_index( protocol->client_slots, client_index );
    if ( mapping_index < 0 )
        return;
    const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( protocol->encryption_map, mapping_index );
    const struct rocketnet_ack_endpoint_t * endpoint = &protocol->acks->endpoints[client_index];
    capture->connected = 1;
    capture->server_mapping_known = 1;
    capture->server_slot = client_index;
    rocketnet_address_to_string( rocketnet_encryption_map_address( protocol->encryption_map, mapping_index ), capture->remote_endpoint );
    capture->last_receive_age_seconds = mapping->control && mapping->control->last_receive_time > 0.0 ? server_transport->time - mapping->control->last_receive_time : -1.0;
    capture->highest_received_sequence = mapping->replay_session.highest_opened;
    capture->highest_sent_sequence = mapping->send_sequence_session > 0 ? mapping->send_sequence_session - 1 : 0;
    capture->largest_acked_sequence = endpoint->largest_acked_sequence;
    capture->keep_alive_acked = endpoint->keep_alive_acked ? 1 : 0;
}
#endif // #if ROCKETNET_DEV


void rocketnet_server_transport_defer_payload_acks( struct rocketnet_server_transport_t * transport )
{
    transport->protocol.defer_payload_acks = 1;
}

void rocketnet_server_transport_accept_payload( struct rocketnet_server_transport_t * transport, int k )
{
    const rocketnet_server_payloads_t * batch = &transport->payloads;
    if ( k >= 0 && k < batch->num_payloads )
        rocketnet_server_protocol_accept_payload( &transport->protocol, batch->client_index[k], batch->connection_id[k], batch->sequence[k] );
}

#endif // #ifndef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

#if ROCKETNET_DEV && !defined( ROCKETNET_CONFORMANCE_DEFAULTS_ONLY )
// Unit-only fixture for the public flush path; no transport create, socket,
// mutex, thread or clock is involved. The two calls have identical counters.
static void rocketnet_server_transport_test_count_flush( void * context )
{
    ++*(int *) context;
}

int rocketnet_server_transport_test_reflush_flags( unsigned int flags )
{
    void * storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_transport_t ) );
    if ( !storage )
        return -1;
    auto * transport = new ( storage ) rocketnet_server_transport_t();
    transport->send_arena = rocketnet_send_arena_create( NULL, 1, 1 );
    if ( !transport->send_arena )
    {
        transport->~rocketnet_server_transport_t();
        rocketnet_free( NULL, storage );
        return -1;
    }

    int flushes = 0;
    rocketnet_server_transport_io_operations_t operations = {};
    operations.flags = flags;
    operations.flush = rocketnet_server_transport_test_count_flush;
    transport->io.operations = &operations;
    transport->io.instance = &flushes;
    transport->send_staged = 1;
    rocketnet_server_transport_send_packets( transport );
    rocketnet_server_transport_send_packets( transport );
    rocketnet_send_arena_destroy( transport->send_arena );
    transport->~rocketnet_server_transport_t();
    rocketnet_free( NULL, storage );

    return flushes;
}

// the same two sends with a transport's declared flags: -2 when the
// transport is not built on this platform

int rocketnet_server_transport_test_reflush( int transport_kind )
{
    const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( transport_kind );
    if ( !definition || !definition->backend || !definition->backend->server )
        return -2;

    return rocketnet_server_transport_test_reflush_flags( definition->backend->server->flags );
}

// A staged send must close the simulator's partial reverse window at the
// end of the frame, just as the portable send path does.
int rocketnet_server_transport_test_staged_simulator_flush()
{
    void * storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_transport_t ) );
    if ( !storage )
        return -1;
    auto * transport = new ( storage ) rocketnet_server_transport_t();
    transport->send_arena = rocketnet_send_arena_create( NULL, 1, 1 );
    transport->link_simulator = rocketnet_network_simulator_create( NULL, 4, 1 );

    const bool mutex_created = transport->link_simulator &&
                               rocketnet_platform_mutex_create( &transport->link_simulator_mutex );
    int result = -1;

    if ( transport->send_arena && transport->link_simulator && mutex_created &&
         rocketnet_network_simulator_patterns( transport->link_simulator, 0, 2 ) )
    {
        int flushes = 0;
        rocketnet_server_transport_io_operations_t operations = {};
        operations.flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_REFLUSHES;   // XDP's declared flush capability
        operations.flush = rocketnet_server_transport_test_count_flush;
        transport->io.operations = &operations;
        transport->io.instance = &flushes;
        transport->transport = ROCKETNET_TRANSPORT_XDP;
        transport->time = 1000.0;

        rocketnet_address_t to;
        if ( rocketnet_address_parse( &to, "127.0.0.1:40000" ) )
        {
            const uint8_t packet = 0x5A;
            (void) rocketnet_server_link_send( transport, &to, &packet, 1 );
            rocketnet_address_t from, received_to;

            uint8_t received[ROCKETNET_RECEIVE_CEILING];
            const int before = rocketnet_network_simulator_receive_any( transport->link_simulator,
                                                                        1000.0, &from, &received_to, received );
            rocketnet_server_transport_send_packets( transport );

            const int after = rocketnet_network_simulator_receive_any( transport->link_simulator,
                                                                       1000.0, &from, &received_to, received );
            result = before == 0 && after == 1 && received[0] == packet && flushes == 1 ? 1 : 0;
        }
    }

    if ( mutex_created )
        rocketnet_platform_mutex_destroy( &transport->link_simulator_mutex );
    if ( transport->link_simulator )
        rocketnet_network_simulator_destroy( transport->link_simulator );
    if ( transport->send_arena )
        rocketnet_send_arena_destroy( transport->send_arena );
    transport->~rocketnet_server_transport_t();
    rocketnet_free( NULL, storage );

    return result;
}
#endif // #if ROCKETNET_DEV && !defined( ROCKETNET_CONFORMANCE_DEFAULTS_ONLY )

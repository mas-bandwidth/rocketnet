// Protocol defaults and table-sizing ratios the create path assigns.
// These values are chosen design values, not measurements from a recorded run
// in docs/runs/.
// One declaration: rocketnet_server_transport_create uses these macros, and
// the conformance dump includes this file under
// ROCKETNET_CONFORMANCE_DEFAULTS_ONLY and expands the same ones.

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
#include "rocketnet_fill_phase.h"
#include "rocketnet_receive_prefilter.h"
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
#include "rocketnet_config.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_memory.h"

static void rocketnet_server_transport_abort_create( struct rocketnet_server_transport_t * transport );

bool rocketnet_server_transport_io_receive_drops( const struct rocketnet_server_transport_io_operations_t * operations,
                                                  void * transport, uint64_t * too_short, uint64_t * too_large );
bool rocketnet_server_transport_io_receive_frames_dropped( const struct rocketnet_server_transport_io_operations_t * operations,
                                                           void * transport, uint64_t * dropped );

// One payload slot of the receive batch's storage holds a whole opened
// packet: the protocol opens each packet into the next free slot and an
// accepted payload is delivered where it opened, its bytes behind the ack
// block, so the plaintext is written once (rocketnet_server_protocol_process_received).

#define ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES ROCKETNET_RECEIVE_CEILING

ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES >= ROCKETNET_RECEIVE_PAYLOAD_BYTES,
                         "a payload slot holds the largest payload" );

#include <sodium.h>
#include <float.h>
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

// The transport is selected at RUNTIME: one build carries
// every transport its platform has, and create walks the config's ordered
// candidate list -- config->transport, overridden by ROCKETNET_SERVER_TRANSPORT --
// left to right, first grant wins. On linux that is the runtime ladder:
// probe XDP (privilege and NIC support), fall to the non-root GSO transport
// (SO_REUSEPORT sharding + sendmmsg/recvmmsg + UDP GSO/GRO, per the
// measurements), floor at portable. "auto" walks the ladder; ANY explicit
// list is exactly that list, so a one-element list is a pin -- that transport
// or loud failure, never a silent fall that would mask a provisioning
// error. The granted tier is reported request-vs-granted style:
// rocketnet_server_transport_io_name plus one log line naming the grant and
// why each earlier candidate refused. The GSO transport's own feature probing
// and degradation remains runtime inside the transport, below this ladder.

#if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )
#error transport selection is runtime now: drop ROCKETNET_SERVER_XDP / ROCKETNET_SERVER_GSO and select with config transport or ROCKETNET_SERVER_TRANSPORT
#endif // #if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )

// Every buffer below that scales with clients is carved, at create, out of
// the transport's one allocation, behind its fixed part, sized from the
// configured max_clients (rocketnet_server_transport_layout); the memory plan
// counts the same layout.

// the portable control batch: capacity is rocketnet_server_send_batch_packets, and
// sends and accepted are the flush's batch, built from the packets
struct rocketnet_server_send_buffer_t
{
    int num_packets;
    int capacity;
    rocketnet_address_t * to;
    int * packet_bytes;
    uint8_t * data;                                     // ROCKETNET_FRAME_SIZE per packet
    struct rocketnet_platform_send_t * sends;
    bool * accepted;
};

// the receive batch on the non-xdp transports: one pump call fills these, then
// processing walks them. the stride is the receive ceiling, so any packet
// the wire admits fits a slot; the pump moves the batch in as few syscalls
// as the platform earned and every platform serves the same call.
// the buffer is sized to a full per-update receive budget so one update can
// absorb everything the budget allows, draining a backlog in as few reads
// as the endpoint can answer (see rocketnet_server_receive_packets_per_update).

struct rocketnet_server_receive_buffer_t
{
    int capacity;                                       // rocketnet_server_receive_packets_per_update
    rocketnet_address_t * from;
    int * packet_bytes;
    uint8_t * data;                                     // ROCKETNET_RECEIVE_CEILING per packet
};

// the portable payload ring: what the acquire/commit surface fills between
// update and the flush, from any number of game threads. slot acquisition is
// one atomic add; each slot's info sits alone on its cache line so
// concurrent finishes never share one; the frame stride keeps every root
// 16-aligned. the response ring above stays single-threaded under update
// and is deliberately not this. The ring holds rocketnet_server_send_batch_packets
// slots; its info and data arrays are carved behind the transport's fixed
// part with a page between them and around them, so producers' writes share
// no line with num_packets or with each other's arrays.

struct alignas( 64 ) rocketnet_server_payload_slot_info_t
{
    uint64_t connection_id;                         // the connection that sealed this frame
    rocketnet_address_t to;
    int wire_bytes;
    int client_index;
    uint8_t padding[64 - sizeof( uint64_t ) - sizeof( rocketnet_address_t ) - 2 * sizeof( int )]; // the rest of the cache line, explicit
};

static_assert( sizeof( rocketnet_server_payload_slot_info_t ) == 64, "one server slot info per cache line" );

struct rocketnet_server_payload_ring_t
{
    uint8_t padding_0[4096];

    volatile uint64_t num_packets;

    // a page, plus the rest of num_packets' cache line, so the fields after it share no line with it
    uint8_t padding_1[4096 + 64 - sizeof( uint64_t )];

    struct rocketnet_server_payload_slot_info_t * info;
    uint8_t * data;                                             // ROCKETNET_FRAME_STRIDE per slot, 16-aligned
    struct rocketnet_platform_send_t * sends;                   // the flush's batch
    int * send_client_index;
    bool * accepted;
    int64_t size;                                               // slots: rocketnet_server_send_batch_packets

    uint8_t padding_2[64 - ( 5 * sizeof( void * ) + sizeof( int64_t ) ) % 64];    // the rest of the cache line, explicit
};

static_assert( offsetof( rocketnet_server_payload_ring_t, info ) == 8192 + 64, "the ring's fields after num_packets start on a cache line with no implicit padding" );
static_assert( sizeof( rocketnet_server_payload_ring_t ) % 64 == 0, "the ring is a whole number of cache lines" );
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
// a payload frame drained after that slot was freed and assigned again
// charges the server totals only, not the new connection.

// One slot's totals per cache line: a slot's one filler writes its sent
// pair while other fillers write their slots' (rocketnet_fill_phase.h, one
// filler per client), so no two slots share a line.
#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4324 )
#endif // #if defined( _MSC_VER )
struct alignas( 64 ) rocketnet_server_wire_totals_t
{
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};
#if defined( _MSC_VER )
#pragma warning( pop )
#endif // #if defined( _MSC_VER )

static_assert( sizeof( rocketnet_server_wire_totals_t ) == 64, "one slot's wire totals per cache line" );

// The fill's per-thread lines (rocketnet_fill_phase_line_of: the calling
// thread's id hashed to one of 64). What a filler would otherwise write per
// packet into state every filler shares goes on its own line, and main
// folds the lines between fill phases: nothing shared across clients is
// written per packet by a filler. Two threads whose ids hash to one line
// share it, so every write is a relaxed atomic; it stays on one core's line
// unless they collide.
//
//   - staged, packets_sent, bytes_sent: the staged transport's frames staged
//     (send_staged's share) and the sends the link took with their wire
//     bytes (the whole-server sent totals and the stats bucket's share).
//     Folded by main (rocketnet_server_fold_fill_lines) at update and at
//     send_packets, before either reads send_staged; read in place by
//     rocketnet_server_transport_stats.
//   - ring: on a buffered transport, the payload ring slots the line claimed
//     in one chunk and has not used, [end:32][next:32]. Returned by main at
//     the flush before the ring is read: each unused slot is marked empty.
//   - tails[queue]: on a staged transport, the send reserve's chunk claims,
//     [held:32][tail:32]: tail is the grants of the line's chunks not yet
//     taken; held is a chunk claim from its add until its grant or take
//     back. tail + held, summed over the lines, is the code's tail_held
//     (proofs/tla/RNSendReserve.tla TailHeld). Returned by main when it
//     opens the next fill phase, or by the filler when the transport refuses
//     a frame (RNSendReserve End).
//   - started[queue]: on a staged transport, the granted frames the line staged
//     this phase (RNSendReserve Count): the queue's grants staged are the sum
//     over the lines, read only by the budget recompute. Each line only
//     grows within a phase, so two sums that are equal saw no frame counted
//     on any line between their reads of it (Read1, Read3). Zeroed by main
//     when it opens the next fill phase.
#define ROCKETNET_SERVER_FILL_HALF_BITS 32
#define ROCKETNET_SERVER_FILL_HALF_MASK 0xFFFFFFFFULL

// The frames one chunk claim takes: a tuning number chosen for the cache-line
// cost (one add on the shared counter per this many frames), not derived from
// anything, until the hardening row measures it. A chunk is claimed only
// while the room left is at least this many frames for every line (the
// guard, ROCKETNET_FILL_PHASE_FILL_LINES lines), so the tails the lines hold
// stay a small part of the room.
#define ROCKETNET_SERVER_FILL_CHUNK 8

#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4324 )
#endif // #if defined( _MSC_VER )
struct alignas( 64 ) rocketnet_server_fill_line_t
{
    uint64_t staged;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t ring;
    uint64_t tails[ROCKETNET_SERVER_MAX_SEND_QUEUES];
    uint64_t started[ROCKETNET_SERVER_MAX_SEND_QUEUES];
};
#if defined( _MSC_VER )
#pragma warning( pop )
#endif // #if defined( _MSC_VER )

// the payload batch from the last update -- capacity rocketnet_server_receive_packets_per_update
// covers a full drain. the public surface iterates by index and never sees this layout.

struct rocketnet_server_payloads_t
{
    int num_payloads;
    int capacity;                                       // rocketnet_server_receive_packets_per_update
    int * client_index;
    uint64_t * connection_id;
    uint64_t * sequence;                                // the sequence each entry arrived at, for rocketnet_server_transport_accept_payload
    uint8_t ** payload_data;
    int * payload_bytes;
};

// The disconnect ring's entries (proofs/tla/ServerStopSpread.tla RingSize):
// a running server keeps at most max_clients bursts waiting (SteadyCap), and
// stop appends one for every connected client whatever is already waiting,
// so the ring holds twice the configured max_clients and an append never
// finds it full.
static inline int rocketnet_server_disconnect_bursts( int max_clients )
{
    return 2 * max_clients;
}

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
    // protocol's own prefilter (rocketnet_prefilter_check) also counts into
    uint64_t transport_receive_too_short = 0;
    uint64_t transport_receive_too_large = 0;

    // the transport's receive_drop_totals as last read: each read adds only
    // the growth since to the counters each reason names
    uint64_t transport_receive_drop_totals[ROCKETNET_RECEIVE_NUM_DROPS] = {};

    struct rocketnet_server_protocol_t protocol;

    // the portable path's buffers ride the struct on every platform: about a
    // megabyte a staged grant never touches, priced once per server against
    // carrying the runtime fork without allocation branches

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

    struct rocketnet_server_transport_stats_t server_stats;
    int current_stats_index;
    struct rocketnet_server_wire_totals_t * client_wire_totals;         // one per client slot
    int client_wire_totals_capacity;
    int maintenance_next_client;                       // first unserved slot after staged backpressure

    volatile uint64_t send_staged;                      // main only: its own frames, and the fill's folded from the lines
    uint64_t send_flushed;                              // main thread only

    // payload sends on the staged transports seal straight into the transport's
    // own staging from any game thread, so there is no transport-side ring
    // for the flush walk to read; each finish counts its packet on its
    // thread's fill line, main folds the lines here
    // (rocketnet_server_fold_fill_lines), and send_packets moves the pair
    // into the current bucket.
    volatile uint64_t stats_pending_packets_sent;
    volatile uint64_t stats_pending_bytes_sent;

    struct rocketnet_network_simulator_t * link_simulator;
    rocketnet_address_t link_simulator_from;
    rocketnet_platform_mutex_t link_simulator_mutex;
#if ROCKETNET_DEV
    int link_busy_sends_for_test;   // the next link sends that take the lock-miss path
    int link_setting_busy_for_test; // the next setting takes that meet the lock held
    uint64_t link_lock_misses;      // link sends dropped because the simulator's lock was busy
    int refuse_fill_chunks_for_test; // the next reserve chunk claims refused as if past the budget
    int steal_ring_chunk_for_test;   // the next payload ring chunk claims that find the line refilled, as by another thread on it
#endif // #if ROCKETNET_DEV

    // The disconnect bursts waiting to leave (proofs/tla/ServerSession.tla
    // SealWaiting and SealStep; proofs/tla/ServerStopSpread.tla for the ring's
    // layout and the stop). A disconnect ends the slot at once; when this
    // update's allowance or the transport's send room cannot take its whole
    // burst, the burst's plaintext (the ack block of the receive state at the
    // disconnect), its first sequence, salt and session key wait here in a
    // ring, and each update seals at most disconnect_packets_per_update of
    // them within the transport's room, one packet per waiting burst per
    // pass. Each burst waiting when a pass starts is sealed once before
    // that pass ends (proofs/tla/RNDisconnectBurstRing.tla PassFair). A new
    // burst joining during a pass can follow redundant copies already due.
    // Stop appends every connected client's burst
    // and each stop step seals what the room takes. The keys are wiped as
    // each burst completes and at destroy.
    struct rocketnet_server_disconnect_burst_t * disconnect_bursts;
    uint8_t ( *disconnect_burst_keys )[ROCKETNET_KEY_BYTES];
    int disconnect_burst_capacity;                      // rocketnet_server_disconnect_bursts( max_clients )
    int disconnect_burst_steady_cap;                    // max_clients: the most bursts a running server keeps waiting
    int disconnect_burst_head;
    int disconnect_burst_count;
    int disconnect_burst_cursor;                        // ring offset from the head of this pass's next burst
    int disconnect_packets_per_update;                  // configured whole-server allowance
    int disconnect_seals_left;                          // this update's allowance, reset by each update
    int disconnect_update_prepared;                     // message layer opened this public update's allowance
    int update_open;                                    // update_receive ran and update_finish has not (the acks are OPEN)
    uint64_t disconnect_seals;                         // disconnect packets sealed, for the unit work bound
    bool io_stop_requested;                             // the stop's last burst is flushed and the I/O told to stop

    // The protocol send reserve (proofs/tla/RNSendReserve.tla). On a staged
    // transport the game's packets and the protocol's frames share each queue's
    // staging. At the end of update and of send_packets main gives each queue a
    // budget: its room less the reserve the next update's protocol frames may
    // need there. A game packet start claims one unit of its queue's budget with one
    // atomic add and is refused (SEND_RESERVE_REFUSED) past it; protocol
    // frames never claim. Written by main only while no game packet is being started; the
    // claims are atomic adds.
    int send_queues;                                    // the transport's send_queues, or 1
    uint64_t fill_budget[ROCKETNET_SERVER_MAX_SEND_QUEUES];
    uint64_t fill_claims[ROCKETNET_SERVER_MAX_SEND_QUEUES];
    uint64_t fill_own_used[ROCKETNET_SERVER_MAX_SEND_QUEUES] = {};  // own frames taken, by the queue they staged in
    uint64_t send_reserve[ROCKETNET_SERVER_MAX_SEND_QUEUES]; // the last reserve computed, per queue (read by tests)
    // The largest room each queue has reported when a budget opened: its
    // staging, once it has been seen empty. The reserve is never more than
    // half of it (rocketnet_server_transport_open_fill_budget).
    uint64_t send_room_seen[ROCKETNET_SERVER_MAX_SEND_QUEUES] = {};
    // A connected client whose keep-alive is acknowledged owns its reserved
    // frame until the budget opens again: the game's first frame to it takes that frame (the
    // payload is what the keep-alive would have carried), and its later
    // frames claim the queue's budget. 1 while the frame is there. Set by
    // main at open; taken with one atomic add by a packet start, so two game
    // threads that start packets to one client at once take it once.
    uint64_t fill_own_frame[ROCKETNET_MAX_CLIENTS] = {};
    uint64_t fill_own_frames_left = 0;                      // own frames not yet taken since the budget opened (atomic)
#if ROCKETNET_DEV
    uint64_t retest_take_back_for_test = 0;                 // claims taken back, as by another thread, between a claim's refusal and its retest
#endif // #if ROCKETNET_DEV

    // the fill-phase contract's check, every build (rocketnet_fill_phase.h, proofs/tla/RNFillPhase.tla)
    struct rocketnet_fill_phase_t fill_phase;

    // the fill's per-thread lines (rocketnet_server_fill_line_t)
    struct rocketnet_server_fill_line_t fill_lines[ROCKETNET_FILL_PHASE_FILL_LINES] = {};

    struct rocketnet_server_payloads_t payloads;
    // payloads.capacity slots of ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES, plus 8:
    // a caller's bit reader may load 8-byte windows at byte granularity, so a
    // payload it reads in place must have at least 8 readable bytes past the
    // end of its data -- the storage is the last carve, so the slack is real.
    uint8_t * payload_storage;
};

// The carves behind the transport's fixed part, in order, and the whole
// allocation's bytes. Create and the memory plan both take this layout.
struct rocketnet_server_transport_layout_t
{
    int send_packets, receive_packets, ring_slots, disconnect_bursts, cookie_reply_source_words;
    uint64_t cookie_reply_sources;
    uint64_t send_to, send_packet_bytes, send_data, send_sends, send_accepted;
    uint64_t receive_from, receive_packet_bytes, receive_data;
    uint64_t ring_info, ring_data, ring_sends, ring_send_client_index, ring_accepted;
    uint64_t disconnect_bursts_offset, disconnect_burst_keys;
    uint64_t payload_client_index, payload_connection_id, payload_sequence, payload_data, payload_bytes, payload_storage;
    uint64_t bytes;
};

static bool rocketnet_server_transport_layout( int max_clients, rocketnet_server_transport_layout_t * layout )
{
    memset( layout, 0, sizeof( *layout ) );
    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        return false;
    }

    layout->send_packets = rocketnet_server_send_batch_packets( max_clients );
    layout->receive_packets = rocketnet_server_receive_packets_per_update( max_clients );
    layout->ring_slots = rocketnet_server_send_batch_packets( max_clients );
    layout->disconnect_bursts = rocketnet_server_disconnect_bursts( max_clients );
    layout->cookie_reply_source_words = rocketnet_server_cookie_reply_source_words( max_clients );

    const uint64_t send = (uint64_t) layout->send_packets;
    const uint64_t receive = (uint64_t) layout->receive_packets;
    const uint64_t ring = (uint64_t) layout->ring_slots;
    const uint64_t bursts = (uint64_t) layout->disconnect_bursts;

    rocketnet_carve_t carve = { sizeof( rocketnet_server_transport_t ), false };

    // the payload ring's slots, a page clear of the fixed part, of each other
    // and of what follows
    rocketnet_carve( carve, 1, 4096 );
    layout->ring_info = rocketnet_carve( carve, ring, sizeof( rocketnet_server_payload_slot_info_t ) );
    rocketnet_carve( carve, 1, 4096 );
    layout->ring_data = rocketnet_carve( carve, ring, (uint64_t) ROCKETNET_FRAME_STRIDE );
    rocketnet_carve( carve, 1, 4096 );
    layout->ring_sends = rocketnet_carve( carve, ring, sizeof( rocketnet_platform_send_t ) );
    layout->ring_send_client_index = rocketnet_carve( carve, ring, sizeof( int ) );
    layout->ring_accepted = rocketnet_carve( carve, ring, sizeof( bool ) );

    layout->send_to = rocketnet_carve( carve, send, sizeof( rocketnet_address_t ) );
    layout->send_packet_bytes = rocketnet_carve( carve, send, sizeof( int ) );
    layout->send_data = rocketnet_carve( carve, send, ROCKETNET_FRAME_SIZE );
    layout->send_sends = rocketnet_carve( carve, send, sizeof( rocketnet_platform_send_t ) );
    layout->send_accepted = rocketnet_carve( carve, send, sizeof( bool ) );

    layout->receive_from = rocketnet_carve( carve, receive, sizeof( rocketnet_address_t ) );
    layout->receive_packet_bytes = rocketnet_carve( carve, receive, sizeof( int ) );
    layout->receive_data = rocketnet_carve( carve, receive, ROCKETNET_RECEIVE_CEILING );

    layout->disconnect_bursts_offset = rocketnet_carve( carve, bursts, sizeof( rocketnet_server_disconnect_burst_t ) );
    layout->disconnect_burst_keys = rocketnet_carve( carve, bursts, ROCKETNET_KEY_BYTES );

    // the protocol's cookie reply source bitmap (rocketnet_server_cookie_reply_source_words)
    layout->cookie_reply_sources = rocketnet_carve( carve, (uint64_t) layout->cookie_reply_source_words, sizeof( uint64_t ) );

    layout->payload_client_index = rocketnet_carve( carve, receive, sizeof( int ) );
    layout->payload_connection_id = rocketnet_carve( carve, receive, sizeof( uint64_t ) );
    layout->payload_sequence = rocketnet_carve( carve, receive, sizeof( uint64_t ) );
    layout->payload_data = rocketnet_carve( carve, receive, sizeof( uint8_t * ) );
    layout->payload_bytes = rocketnet_carve( carve, receive, sizeof( int ) );
    layout->payload_storage = rocketnet_carve( carve, receive, ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES );
    rocketnet_carve( carve, 1, 8, 1 );

    layout->bytes = carve.bytes;
    return !carve.failed && carve.bytes <= SIZE_MAX;
}

static void rocketnet_server_transport_drain_disconnect_bursts( struct rocketnet_server_transport_t * server_transport, int max_seals, bool shutdown );

// The server transport for max_clients: one allocation of its layout, the
// fixed part zeroed and default initialized, every carve wired to its
// pointer, and the disconnect keys zeroed. The other carves (the payload
// ring's slots, the send and receive buffers, the payloads and their storage:
// 49 MB at 1000 clients) are never written here, so their pages are touched
// only when a packet uses them. NULL when the layout or the allocation is
// refused.
static rocketnet_server_transport_t * rocketnet_server_transport_allocate( void * context, int max_clients )
{
    rocketnet_server_transport_layout_t layout;
    if ( !rocketnet_server_transport_layout( max_clients, &layout ) )
    {
        return NULL;
    }

    uint8_t * base = (uint8_t *) rocketnet_malloc( context, (size_t) layout.bytes );
    if ( !base )
    {
        return NULL;
    }

    // value-initialized: zeroed, then its default member initializers
    rocketnet_server_transport_t * server_transport = new ( base ) rocketnet_server_transport_t();

    rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;
    ring->size = layout.ring_slots;
    ring->info = (rocketnet_server_payload_slot_info_t *) ( base + layout.ring_info );
    ring->data = base + layout.ring_data;
    ring->sends = (rocketnet_platform_send_t *) ( base + layout.ring_sends );
    ring->send_client_index = (int *) ( base + layout.ring_send_client_index );
    ring->accepted = (bool *) ( base + layout.ring_accepted );

    rocketnet_server_send_buffer_t * send = &server_transport->send_buffer;
    send->capacity = layout.send_packets;
    send->to = (rocketnet_address_t *) ( base + layout.send_to );
    send->packet_bytes = (int *) ( base + layout.send_packet_bytes );
    send->data = base + layout.send_data;
    send->sends = (rocketnet_platform_send_t *) ( base + layout.send_sends );
    send->accepted = (bool *) ( base + layout.send_accepted );

    rocketnet_server_receive_buffer_t * receive = &server_transport->receive_buffer;
    receive->capacity = layout.receive_packets;
    receive->from = (rocketnet_address_t *) ( base + layout.receive_from );
    receive->packet_bytes = (int *) ( base + layout.receive_packet_bytes );
    receive->data = base + layout.receive_data;

    server_transport->disconnect_bursts = (rocketnet_server_disconnect_burst_t *) ( base + layout.disconnect_bursts_offset );
    server_transport->disconnect_burst_keys = ( uint8_t( * )[ROCKETNET_KEY_BYTES] )( base + layout.disconnect_burst_keys );
    server_transport->disconnect_burst_capacity = layout.disconnect_bursts;
    server_transport->disconnect_burst_steady_cap = max_clients;
    memset( server_transport->disconnect_burst_keys, 0, (size_t) layout.disconnect_bursts * ROCKETNET_KEY_BYTES );

    // clear before the first receive: the first update's receives run before
    // its protocol update clears the bitmap
    server_transport->protocol.cookie_reply_sources = (uint64_t *) ( base + layout.cookie_reply_sources );
    server_transport->protocol.cookie_reply_source_words = layout.cookie_reply_source_words;
    memset( server_transport->protocol.cookie_reply_sources, 0, (size_t) layout.cookie_reply_source_words * sizeof( uint64_t ) );

    rocketnet_server_payloads_t * payloads = &server_transport->payloads;
    payloads->capacity = layout.receive_packets;
    payloads->client_index = (int *) ( base + layout.payload_client_index );
    payloads->connection_id = (uint64_t *) ( base + layout.payload_connection_id );
    payloads->sequence = (uint64_t *) ( base + layout.payload_sequence );
    payloads->payload_data = (uint8_t **) ( base + layout.payload_data );
    payloads->payload_bytes = (int *) ( base + layout.payload_bytes );
    server_transport->payload_storage = base + layout.payload_storage;
    ROCKETNET_RED_ZONE_CARVE( base, rocketnet_server_transport_layout( max_clients, &layout ) );     // ASan builds: poison the red zones between the carves (sdk/rocketnet_memory.h)
    return server_transport;
}

#if ROCKETNET_DEV
void rocketnet_server_transport_poison_slot_for_test( struct rocketnet_server_transport_t * server, int client_index )
{
    rocketnet_assert( server );
    rocketnet_assert( client_index >= 0 && client_index < server->client_wire_totals_capacity );
    memset( &server->client_wire_totals[client_index], 0xA5, sizeof( server->client_wire_totals[client_index] ) );
}

// The unit seam for the protocol's cookie reply source bitmap: its words,
// and whether every one of them is clear.
int rocketnet_server_transport_cookie_reply_source_words_for_test( const struct rocketnet_server_transport_t * server, bool * clear )
{
    rocketnet_assert( server && clear );
    const struct rocketnet_server_protocol_t * protocol = &server->protocol;
    *clear = protocol->cookie_reply_sources != NULL;
    for ( int i = 0; *clear && i < protocol->cookie_reply_source_words; ++i )
    {
        *clear = protocol->cookie_reply_sources[i] == 0;
    }
    return protocol->cookie_reply_source_words;
}

// The unit seam for a sweep over many clients without a handshake: seats
// slots 0..clients-1 connected at 127.0.0.1:(1000 + i) under test keys, each
// with its keep-alive due from the first update. Returns false when a slot
// cannot be seated.
bool rocketnet_server_transport_seat_clients_for_test( struct rocketnet_server_transport_t * server, int clients )
{
    rocketnet_assert( server );
    for ( int i = 0; i < clients; ++i )
    {
        rocketnet_address_t address;
        if ( !rocketnet_address_parse( &address, "127.0.0.1:0" ) )
        {
            return false;
        }
        address.port = (uint16_t) ( 1000 + i );
        const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 100.0 );
        if ( index < 0 )
        {
            return false;
        }
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, index );
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
        const uint64_t owner_generation = rocketnet_encryption_map_owner_generation( server->protocol.encryption_map, index );
        if ( rocketnet_client_slots_assign( server->protocol.client_slots, (uint64_t) i + 1, index, owner_generation, user_data ) != i )
        {
            return false;
        }
        mapping->control->client_index = i;
        mapping->client_id = (uint64_t) i + 1;
        mapping->control->established = 1;
        mapping->control->timeout_seconds = 1000;
        memset( &mapping->keys, i + 1, sizeof( mapping->keys ) );
        server->protocol.connection_ids[i] = ++server->protocol.next_connection_id;
    }
    return true;
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
    {
        member = "rocketnet_client_slots_t.connected";
    }
    int iterator = -1;
    while ( !member && rocketnet_encryption_map_next( protocol->encryption_map, &iterator ) )
    {
        if ( rocketnet_encryption_map_control( protocol->encryption_map, iterator )->client_index == client_index )
        {
            member = "rocketnet_encryption_mapping_control_t.client_index";
        }
    }
    if ( !member && protocol->connection_ids[client_index] != 0 )
    {
        member = "rocketnet_server_protocol_t.connection_ids";
    }
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
            {
                member = "rocketnet_server_acks_t.endpoints";
            }
            rocketnet_free( server->context, fresh );
        }
    }

    if ( member )
    {
        if ( allocation_failed )
        {
            snprintf( failure, failure_bytes, "%s[%d]: check could not run (allocation failed)", member, client_index );
        }
        else
        {
            snprintf( failure, failure_bytes, "%s[%d]: not reset when the connection ended", member, client_index );
        }
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

// The calling thread's fill line.
// Until the game names a client (the thread rule check not armed) it sends
// from one thread at a time and every call uses line 0: no thread id, no
// hash. Every word of a line is still written atomically, so the game's first
// calls from several threads at once, made before its first call that names
// a client arms the check, share line 0 safely.
static struct rocketnet_server_fill_line_t * rocketnet_server_fill_line( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_fill_phase_armed( &server_transport->fill_phase ) )
    {
        return &server_transport->fill_lines[0];
    }
    return &server_transport->fill_lines[rocketnet_fill_phase_line_of( rocketnet_platform_thread_id() )];
}

// A staged send the link took, from any game thread: the whole-server pair
// on the filler's line (folded by main), the slot's pair in place. A slot's
// totals have one writer in a fill phase, its filler, so each is a relaxed
// load and store, not a read-modify-write; main reads them between phases,
// and rocketnet_server_transport_client_stats loads them. A producer whose
// client_index is out of range counts in the whole-server totals only, as
// the main-thread path does.

static void rocketnet_server_fill_charge_sent( struct rocketnet_server_transport_t * server_transport,
                                               struct rocketnet_server_fill_line_t * line,
                                               int client_index, int address_type, int packet_bytes )
{
    const uint64_t wire_bytes = (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( address_type ) );
    rocketnet_platform_atomic_add_u64( &line->packets_sent, 1 );
    rocketnet_platform_atomic_add_u64( &line->bytes_sent, wire_bytes );

    if ( client_index >= 0 && client_index < server_transport->client_wire_totals_capacity )
    {
        struct rocketnet_server_wire_totals_t * totals = &server_transport->client_wire_totals[client_index];
        volatile uint64_t * packets = (volatile uint64_t *) &totals->packets_sent;
        volatile uint64_t * bytes = (volatile uint64_t *) &totals->bytes_sent;
        rocketnet_platform_atomic_store_u64( packets, rocketnet_platform_atomic_load_u64( packets ) + 1 );
        rocketnet_platform_atomic_store_u64( bytes, rocketnet_platform_atomic_load_u64( bytes ) + wire_bytes );
    }
}

// Main, with no fill running: the lines' staged frames into send_staged and
// their sends into the pending pair and the whole-server totals, then the
// lines' counts zeroed. Called by update and send_packets before either
// reads send_staged, so the flush comparison and the stats see every frame
// the fill staged.
static void rocketnet_server_fold_fill_lines( struct rocketnet_server_transport_t * server_transport )
{
    uint64_t staged = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        struct rocketnet_server_fill_line_t * line = &server_transport->fill_lines[i];
        staged += line->staged;
        packets += line->packets_sent;
        bytes += line->bytes_sent;
        line->staged = 0;
        line->packets_sent = 0;
        line->bytes_sent = 0;
    }
    server_transport->send_staged += staged;
    server_transport->stats_pending_packets_sent += packets;
    server_transport->stats_pending_bytes_sent += bytes;
    rocketnet_platform_atomic_store_u64( (volatile uint64_t *) &server_transport->server_stats.packets_sent,
                                         rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &server_transport->server_stats.packets_sent ) + packets );
    rocketnet_platform_atomic_store_u64( (volatile uint64_t *) &server_transport->server_stats.bytes_sent,
                                         rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &server_transport->server_stats.bytes_sent ) + bytes );
}

// The lines' sends not yet folded, for a stats read in between.
static void rocketnet_server_fill_lines_sent( const struct rocketnet_server_transport_t * server_transport, uint64_t * packets, uint64_t * bytes )
{
    *packets = 0;
    *bytes = 0;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        const struct rocketnet_server_fill_line_t * line = &server_transport->fill_lines[i];
        *packets += rocketnet_platform_atomic_load_u64( const_cast<volatile uint64_t *>( &line->packets_sent ) );
        *bytes += rocketnet_platform_atomic_load_u64( const_cast<volatile uint64_t *>( &line->bytes_sent ) );
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
    {
        return ROCKETNET_SERVER_MAPPING_NOT_LOOKED_UP;
    }
    return rocketnet_encryption_map_find( server_transport->protocol.encryption_map, address );
}

static int rocketnet_server_mapping_client_index( struct rocketnet_server_transport_t * server_transport, int mapping_index )
{
    if ( mapping_index < 0 )
    {
        return -1;
    }
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
    {
        return wire_client_before;
    }
    if ( outcome != ROCKETNET_SERVER_PROCESS_ACCEPTED && outcome != ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE )
    {
        return wire_client_before;
    }
    const int wire_client_after = rocketnet_server_mapping_client_index( server_transport, mapping_index );
    if ( wire_client_after >= 0 )
    {
        rocketnet_server_wire_totals_assign( server_transport, wire_client_after );
    }
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

bool rocketnet_server_transport_send_buffered( const struct rocketnet_server_transport_t * server_transport )
{
    return server_transport && !rocketnet_server_transport_staged( server_transport );
}

// The unit seam: the send capacity the next create reads. 0 keeps the
// default. It is consumed by that create so one case cannot shape another.
#if ROCKETNET_DEV
static int rocketnet_server_test_create_send_capacity = 0;

void rocketnet_server_transport_set_create_send_capacity_for_test( int packets )
{
    rocketnet_server_test_create_send_capacity =
        packets > 0 && packets <= ROCKETNET_MAX_CLIENTS ? packets : 0;
}
#else // #if ROCKETNET_DEV
static const int rocketnet_server_test_create_send_capacity = 0;
#endif // #if ROCKETNET_DEV

// The unit seam: the next create keeps the inline ack apply (each block
// applied where its packet is processed, as before the deferred apply), so a
// case can run the same trace both ways. Consumed by that create.
#if ROCKETNET_DEV
static bool rocketnet_server_test_create_inline_acks = false;

void rocketnet_server_transport_set_create_inline_acks_for_test( bool inline_acks )
{
    rocketnet_server_test_create_inline_acks = inline_acks;
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
int rocketnet_server_transport_send_capacity( const rocketnet_server_transport_t * server_transport )
{
    return server_transport ? server_transport->send_capacity : 0;
}
#endif // #if ROCKETNET_DEV

// The frames the transport accepts before its next flush without a refusal
// for room, as its send_room operation reports it; a transport without one
// is unbounded. gso and io_uring stage one server-wide bank; xdp a bank per
// queue, reporting the fewest free slots. proofs/tla/ServerStopSpread.tla Room.
static int rocketnet_server_transport_io_send_room( const rocketnet_server_transport_t * server_transport )
{
    if ( !server_transport->io.operations || !server_transport->io.operations->send_room )
    {
        return INT_MAX;
    }
    int room = server_transport->io.operations->send_room( server_transport->io.instance );

    // a buffered transport's control frames take their room at the flush:
    // each one committed and not yet flushed has already spent it

    if ( !rocketnet_server_transport_staged( server_transport ) )
    {
        room -= server_transport->send_buffer.num_packets;
    }
    return room > 0 ? room : 0;
}

// True once every frame the transport was handed has finished; a transport
// that does not report it is idle when its flush returns.
static bool rocketnet_server_transport_io_send_idle( const rocketnet_server_transport_t * server_transport )
{
    if ( !server_transport->io.operations || !server_transport->io.operations->send_idle )
    {
        return true;
    }
    return server_transport->io.operations->send_idle( server_transport->io.instance );
}

// The queue a frame to this address stages in: the transport's own
// destination hash, or 0 on a transport of one queue.
static int rocketnet_server_transport_send_queue_of( const rocketnet_server_transport_t * server_transport, const rocketnet_address_t * to )
{
    if ( server_transport->send_queues <= 1 )
    {
        return 0;
    }
    const int queue = server_transport->io.operations->send_queue_of( server_transport->io.instance, to );
    rocketnet_assert( queue >= 0 && queue < server_transport->send_queues );
    return queue >= 0 && queue < server_transport->send_queues ? queue : 0;
}

// The protocol reserve of each queue, in frames: the most protocol frames
// one update can start in that queue, from the constants that bound them.
//   - handshakes_per_update: each request taken to the signature verify is
//     answered by at most one challenge or denied, and no other request is.
//     All of them may hash to one queue, so every queue keeps all of it.
//   - disconnect_packets_per_update: the whole-server allowance of
//     disconnect seals one update makes (kicks and waiting bursts), again
//     all in one queue at worst.
//   - one frame per client slot that can be owed a control frame this
//     update: the reply to its connection response, or the sweep's
//     keep-alive, never both (the reply stamps the send time
//     rocketnet_server_keep_alive_due reads). A connected slot counts in
//     the queue its address hashes to; any other slot may be taken by a
//     client of any queue before the next update, so it counts in every
//     queue. With one queue that is every slot.
// Not reserved, by design: the cookie replies to requests that carry no
// valid cookie. They answer unauthenticated sources at whatever rate those
// send, so no constant bounds them; they compete with the game's packets and are
// refused like them when the staging is full.
static void rocketnet_server_transport_send_reserves( const rocketnet_server_transport_t * server_transport, uint64_t * reserve )
{
    const struct rocketnet_client_slots_t * slots = server_transport->protocol.client_slots;
    const uint64_t protocol = (uint64_t) server_transport->protocol.handshakes_per_update + (uint64_t) server_transport->disconnect_packets_per_update;

    // a transport not yet given its client slots (a bare one a unit test
    // builds) has no client to owe a control frame
    const int max_clients = slots ? rocketnet_client_slots_max_clients( slots ) : 0;

    if ( server_transport->send_queues <= 1 )
    {
        reserve[0] = protocol + (uint64_t) max_clients;
        return;
    }

    int connected = 0;
    for ( int queue = 0; queue < server_transport->send_queues; ++queue )
    {
        reserve[queue] = protocol;
    }
    for ( int client_index = 0; client_index < max_clients; ++client_index )
    {
        if ( !rocketnet_client_slots_connected( slots, client_index ) )
        {
            continue;
        }
        const int mapping_index = rocketnet_client_slots_mapping_index( slots, client_index );
        if ( mapping_index < 0 )
        {
            continue;
        }
        connected++;
        const rocketnet_address_t * address = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );
        reserve[rocketnet_server_transport_send_queue_of( server_transport, address )]++;
    }
    for ( int queue = 0; queue < server_transport->send_queues; ++queue )
    {
        reserve[queue] += (uint64_t) ( max_clients - connected );
    }
}

// The free frames of one send queue now: the transport's own count, or
// INT_MAX from a transport that does not report it.
static int rocketnet_server_transport_queue_room( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    if ( server_transport->send_queues > 1 )
    {
        return server_transport->io.operations->send_room_queue( server_transport->io.instance, queue );
    }
    if ( server_transport->io.operations && server_transport->io.operations->send_room )
    {
        return server_transport->io.operations->send_room( server_transport->io.instance );
    }
    return INT_MAX;
}

// The frames the drains may hand the transport this update: with several
// send queues, the sum of each queue's room (send_room is the fewest, so one
// full queue would stop bursts bound for queues with room); else the whole
// room. A burst whose own queue is full is refused for a full staging and
// waits (drain_disconnect_bursts).
static int rocketnet_server_transport_io_send_room_total( const struct rocketnet_server_transport_t * server_transport )
{
    if ( server_transport->send_queues <= 1 )
    {
        return rocketnet_server_transport_io_send_room( server_transport );
    }
    int64_t total = 0;
    for ( int queue = 0; queue < server_transport->send_queues; ++queue )
    {
        const int room = rocketnet_server_transport_queue_room( server_transport, queue );
        total += room > 0 ? room : 0;
    }
    return total >= INT_MAX ? INT_MAX : (int) total;
}

static uint64_t rocketnet_server_transport_load_u64( const uint64_t * value )
{
    return rocketnet_platform_atomic_load_u64( const_cast<volatile uint64_t *>( value ) );
}

// The most game frames the queue may grant until the budget opens again, as its state reads
// now: the grants already staged, plus the room left above the reserve the
// protocol still needs there. Every own frame staged has left the room and
// the reserve together, so it is taken out of the reserve. Both counts move
// only after their frame has left the room, so they are read before the
// room: a frame staged between the reads is then counted nowhere, never
// twice. Read again after the room and started over if either moved, so no
// frame staged in between is lost to the game's packets either
// (proofs/tla/RNSendReserve.tla Read1, Read2, Read3; witnesses RoomFirst and NoReread). Each start over follows a
// frame some other thread staged, so the loop ends.
// The grants the lines staged on the queue this phase, summed (rocketnet_server_fill_line_t started).
static uint64_t rocketnet_server_transport_started_sum( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    uint64_t sum = 0;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        sum += rocketnet_server_transport_load_u64( &server_transport->fill_lines[i].started[queue] );
    }
    return sum;
}

static uint64_t rocketnet_server_transport_queue_budget_now( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    for ( ;; )
    {
        const uint64_t started = rocketnet_server_transport_started_sum( server_transport, queue );
        const uint64_t own_used = rocketnet_server_transport_load_u64( &server_transport->fill_own_used[queue] );
        const int room = rocketnet_server_transport_queue_room( server_transport, queue );
        if ( rocketnet_server_transport_started_sum( server_transport, queue ) != started ||
             rocketnet_server_transport_load_u64( &server_transport->fill_own_used[queue] ) != own_used )
        {
            continue;
        }
        const uint64_t free_frames = room > 0 ? (uint64_t) room : 0;
        const uint64_t reserve = server_transport->send_reserve[queue] > own_used ? server_transport->send_reserve[queue] - own_used : 0;
        return started + ( free_frames > reserve ? free_frames - reserve : 0 );
    }
}
static void rocketnet_server_transport_return_fill_tails( struct rocketnet_server_transport_t * server_transport, int queues );



// proofs/tla/RNSendReserve.tla OpenFill: main, with no game packet being started, gives
// each queue a budget of its room less its reserve and zeroes the
// claims. A transport that is not staged keeps the game's packets in its own
// payload ring, apart from the protocol's frames, and needs no reserve.
static void rocketnet_server_transport_open_fill_budget( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_server_transport_staged( server_transport ) )
    {
        return;
    }

    // the queue count is read here, not cached at create, so an io swapped
    // in by a unit test is read whole; a transport without all three queue
    // operations is one queue
    const struct rocketnet_server_transport_io_operations_t * operations = server_transport->io.operations;
    int queues = 1;
    if ( operations && operations->send_queues && operations->send_queue_of && operations->send_room_queue )
    {
        queues = operations->send_queues( server_transport->io.instance );
        rocketnet_assert( queues >= 1 && queues <= ROCKETNET_SERVER_MAX_SEND_QUEUES );
        queues = queues < 1 ? 1 : ( queues > ROCKETNET_SERVER_MAX_SEND_QUEUES ? ROCKETNET_SERVER_MAX_SEND_QUEUES : queues );
    }
    const int queues_before = server_transport->send_queues;
    server_transport->send_queues = queues;

    rocketnet_server_transport_send_reserves( server_transport, server_transport->send_reserve );

    // The reserve is never more than half of a queue's staging, so a config
    // whose protocol allowances outgrow the staging (handshakes_per_update
    // and disconnect_packets_per_update are each legal up to 1024, against a
    // staging of 2048 frames) still leaves the game's packets half of it. The
    // staging is the largest room the queue has reported at an open: the
    // room now may be lower only because frames are still in flight, and a
    // reserve cut to half of that would hand the game frames the protocol
    // needs. A reserve cut here no longer covers every protocol frame of the
    // worst update: one the staging cannot take is refused as before the
    // reserve (SEND_RING_FULL) and is due again at the next update. Half is a
    // chosen share, not a measured one. A transport that does not report its
    // room reads INT_MAX and is never cut.
    uint64_t smallest_reserve = UINT64_MAX;
    for ( int queue = 0; queue < server_transport->send_queues; ++queue )
    {
        const int room = rocketnet_server_transport_queue_room( server_transport, queue );
        if ( room > 0 && (uint64_t) room > server_transport->send_room_seen[queue] )
        {
            server_transport->send_room_seen[queue] = (uint64_t) room;
        }
        const uint64_t most = server_transport->send_room_seen[queue] / 2;
        if ( server_transport->send_reserve[queue] > most )
        {
            server_transport->send_reserve[queue] = most;
        }
        if ( server_transport->send_reserve[queue] < smallest_reserve )
        {
            smallest_reserve = server_transport->send_reserve[queue];
        }
    }
    // the tails of the queues claimed since the last open: the count then, or now if it grew
    rocketnet_server_transport_return_fill_tails( server_transport, queues_before > queues ? queues_before : queues );

    for ( int queue = 0; queue < server_transport->send_queues; ++queue )
    {
        server_transport->fill_claims[queue] = 0;
        server_transport->fill_own_used[queue] = 0;
        server_transport->fill_budget[queue] = rocketnet_server_transport_queue_budget_now( server_transport, queue );
    }

    // The own frames. A client the game sends a payload to before the next update is
    // owed no keep-alive or ack at the next update once its keep-alive is
    // acknowledged: the payload stamps the send time
    // rocketnet_server_keep_alive_due reads and clears the ack debt. So its
    // reserved frame is the game's to use for that first payload. A guess,
    // stated: that holds while the game updates more often than send_rate;
    // when it does not, the keep-alive still due finds the room spent, and the
    // sweep, which stops at a full staging, serves that client first at the
    // next update. A client not yet acknowledged keeps its frame for the
    // protocol (its keep-alives are timed from the last keep-alive alone).
    // The own frames come out of the reserve, so no more are handed out than
    // the smallest queue's reserve holds (RNSendReserve MaxOwn <= Reserve):
    // a reserve cut to half a small staging hands out that many, in slot order.
    const struct rocketnet_client_slots_t * slots = server_transport->protocol.client_slots;
    const int max_clients = slots ? rocketnet_client_slots_max_clients( slots ) : 0;
    uint64_t own = 0;
    for ( int client_index = 0; client_index < max_clients && client_index < ROCKETNET_MAX_CLIENTS; ++client_index )
    {
        const bool owned = own < smallest_reserve && rocketnet_client_slots_connected( slots, client_index ) && server_transport->protocol.acks &&
                           server_transport->protocol.acks->endpoints[client_index].keep_alive_acked;
        server_transport->fill_own_frame[client_index] = owned ? 1 : 0;
        own += owned ? 1 : 0;
    }
    server_transport->fill_own_frames_left = own;
}

// Raises the queue's budget to what its state allows now, never lowers it:
// frames a transport retires after the budget opened (a completion port, a
// ring the kernel drains) come back to the game's packets, and the reserve stays.
static uint64_t rocketnet_server_transport_refresh_fill_budget( struct rocketnet_server_transport_t * server_transport, int queue )
{
    const uint64_t now = rocketnet_server_transport_queue_budget_now( server_transport, queue );
    rocketnet_platform_atomic_max_u64( &server_transport->fill_budget[queue], now );
    return rocketnet_server_transport_load_u64( &server_transport->fill_budget[queue] );
}

// proofs/tla/RNSendReserve.tla Claim, Check, Decide: size claims added on the queue's
// claims in one add (the reversed witness SplitClaim takes it as a load and a
// store, ClaimStore), granted when the whole claim fits under the budget
// (claim_index + size <= budget); else the budget is raised to what the
// queue's state allows now (Read1-3) and the claims outstanding, this one
// among them, are tested against it: not this claim's index, which a refused
// claim ahead of it has raised (witness IndexTest). Refused (Refuse): taken
// back whole (TakeBack) and false.
static bool rocketnet_server_transport_claim_frames( struct rocketnet_server_transport_t * server_transport, int queue, uint64_t size )
{
    const uint64_t claim = rocketnet_platform_atomic_add_u64( &server_transport->fill_claims[queue], size );
    if ( claim + size <= rocketnet_server_transport_load_u64( &server_transport->fill_budget[queue] ) )
    {
        return true;
    }
    const uint64_t budget = rocketnet_server_transport_refresh_fill_budget( server_transport, queue );
    if ( rocketnet_server_transport_load_u64( &server_transport->fill_claims[queue] ) <= budget )
    {
        return true;
    }
    rocketnet_platform_atomic_add_u64( &server_transport->fill_claims[queue], 0 - size );
    return false;
}

// The tails and chunk claims every line holds on the queue (TailHeld): read
// on the refusal path only.
static uint64_t rocketnet_server_transport_tails_held( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    uint64_t held = 0;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        const uint64_t word = rocketnet_server_transport_load_u64( &server_transport->fill_lines[i].tails[queue] );
        held += ( word & ROCKETNET_SERVER_FILL_HALF_MASK ) + ( word >> ROCKETNET_SERVER_FILL_HALF_BITS );
    }
    return held;
}

// proofs/tla/RNSendReserve.tla OwnClaim, Continue, Size, Claim, Check,
// Read1-3, Decide, Refuse, TakeBack, Retest: a client's first frame this phase takes its own
// reserved frame when it has one. Otherwise a frame of the calling thread's
// line's tail when it holds one (Continue: a compare-and-swap on the line,
// no write to the queue's claims). Otherwise the guard (Size, two loads):
// while the budget less the claims leaves ROCKETNET_SERVER_FILL_CHUNK frames
// for every line, a chunk claim, held on the line from its add; granted, its
// first frame is this one and the rest is the line's tail; refused, taken
// back whole and claimed again as one frame (witness NoFallback). Below the
// guard, one frame. A single claim refused is counted
// SEND_RESERVE_REFUSED_TAILS_HELD while any line holds a tail or a chunk
// claim (TailsNamed), else SEND_RESERVE_REFUSED. Returns the queue (with
// *own set for an own frame), or -1 refused.
static int rocketnet_server_transport_fill_claim( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_fill_line_t * line,
                                                  int client_index, const rocketnet_address_t * to, bool * own )
{
    const int queue = rocketnet_server_transport_send_queue_of( server_transport, to );

    // OwnClaim: the flag is taken with one atomic add, which gives back what
    // it held. Only the start that finds 1 there has the frame; a start that
    // finds anything else (another game thread's start to this client took
    // it first, or is taking it now) puts its one back and claims the budget.
    if ( client_index >= 0 && client_index < ROCKETNET_MAX_CLIENTS && rocketnet_server_transport_load_u64( &server_transport->fill_own_frame[client_index] ) == 1 )
    {
        if ( rocketnet_platform_atomic_add_u64( &server_transport->fill_own_frame[client_index], UINT64_MAX ) == 1 )
        {
            rocketnet_platform_atomic_add_u64( &server_transport->fill_own_frames_left, UINT64_MAX );
            *own = true;
            return queue;
        }
        rocketnet_platform_atomic_add_u64( &server_transport->fill_own_frame[client_index], 1 );
    }

    *own = false;

    volatile uint64_t * tails = &line->tails[queue];
    for ( ;; )
    {
        const uint64_t word = rocketnet_platform_atomic_load_u64( tails );
        if ( ( word & ROCKETNET_SERVER_FILL_HALF_MASK ) == 0 )
        {
            break;
        }
        if ( rocketnet_platform_atomic_cas_u64( tails, word, word - 1 ) )
        {
            return queue;
        }
    }

    const uint64_t chunk = ROCKETNET_SERVER_FILL_CHUNK;
    const uint64_t held = chunk << ROCKETNET_SERVER_FILL_HALF_BITS;
    const uint64_t budget = rocketnet_server_transport_load_u64( &server_transport->fill_budget[queue] );
    const uint64_t claims = rocketnet_server_transport_load_u64( &server_transport->fill_claims[queue] );
    if ( budget >= claims && budget - claims >= chunk * ROCKETNET_FILL_PHASE_FILL_LINES )
    {
        rocketnet_platform_atomic_add_u64( tails, held );
        bool refuse = false;
#if ROCKETNET_DEV
        if ( server_transport->refuse_fill_chunks_for_test > 0 )
        {
            server_transport->refuse_fill_chunks_for_test--;
            refuse = true;
        }
#endif // #if ROCKETNET_DEV
        if ( !refuse && rocketnet_server_transport_claim_frames( server_transport, queue, chunk ) )
        {
            rocketnet_platform_atomic_add_u64( tails, ( chunk - 1 ) - held );
            return queue;
        }
        rocketnet_platform_atomic_add_u64( tails, 0 - held );
    }

    // TakeBack, Retest: a single claim refused was taken back inside
    // claim_frames; the test that refused it may have counted another
    // filler's claim that has since been taken back too, so the claims and
    // the budget are read again and a frame now under the budget is claimed
    // again. Without the retest two fillers refused by each other's claims
    // are both refused with a frame left (witness NoRetry). A pass comes
    // back only when another filler took a claim back, or the budget rose,
    // between this claim's test and its retest.
    for ( ;; )
    {
        if ( rocketnet_server_transport_claim_frames( server_transport, queue, 1 ) )
        {
            return queue;
        }
#if ROCKETNET_DEV
        if ( server_transport->retest_take_back_for_test > 0 )
        {
            rocketnet_platform_atomic_add_u64( &server_transport->fill_claims[queue], 0 - server_transport->retest_take_back_for_test );
            server_transport->retest_take_back_for_test = 0;
        }
#endif // #if ROCKETNET_DEV
        const uint64_t claims_now = rocketnet_server_transport_load_u64( &server_transport->fill_claims[queue] );
        if ( claims_now + 1 > rocketnet_server_transport_load_u64( &server_transport->fill_budget[queue] ) )
        {
            break;
        }
    }

    rocketnet_spam( "server start packet refused. send room is down to the protocol reserve" );
    const int counter = rocketnet_server_transport_tails_held( server_transport, queue ) > 0 ? ROCKETNET_COUNTER_SEND_RESERVE_REFUSED_TAILS_HELD
                                                                                             : ROCKETNET_COUNTER_SEND_RESERVE_REFUSED;
    rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[counter], 1 );
    return -1;
}

// proofs/tla/RNSendReserve.tla Count and OwnCount: a granted frame the transport staged has
// left its queue's room, and only now is it counted. An own frame counted
// before it left the room would lower the reserve the budget keeps back
// while the room still held it (witness OwnCountFirst).
static void rocketnet_server_transport_fill_started( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_fill_line_t * line, int queue, bool own )
{
    rocketnet_platform_atomic_add_u64( own ? &server_transport->fill_own_used[queue] : &line->started[queue], 1 );
}

// proofs/tla/RNSendReserve.tla Stage, a queue with no room, and End: the transport refused
// the granted frame, so the run ends: the frame's claim and the line's tail
// go back in one add, and the grants go to the next frames. An own frame
// refused (OwnIo) stays taken: its client's keep-alive is still due and its frame
// stays in the reserve.
static void rocketnet_server_transport_fill_refused( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_fill_line_t * line, int queue, bool own )
{
#if defined( ROCKETNET_SEND_RESERVE_PLANT )
    // the trace check's plant (tests/tla_trace_send_reserve.cpp): the
    // refused frame's claim and the line's tail are kept
    (void) server_transport;
    (void) line;
    (void) queue;
    (void) own;
#else  // #if defined( ROCKETNET_SEND_RESERVE_PLANT )
    if ( own )
    {
        return;
    }
    volatile uint64_t * tails = &line->tails[queue];
    uint64_t rest = 0;
    for ( ;; )
    {
        const uint64_t word = rocketnet_platform_atomic_load_u64( tails );
        rest = word & ROCKETNET_SERVER_FILL_HALF_MASK;
        if ( rest == 0 || rocketnet_platform_atomic_cas_u64( tails, word, word - rest ) )
        {
            break;
        }
    }
    rocketnet_platform_atomic_add_u64( &server_transport->fill_claims[queue], 0 - ( rest + 1 ) );
#endif // #if defined( ROCKETNET_SEND_RESERVE_PLANT )
}

// Main, with no fill running (RNSendReserve End at the phase's end): every
// line's tail on every queue goes back to the queue's claims, one add each,
// and the line's words are zeroed.
static void rocketnet_server_transport_return_fill_tails( struct rocketnet_server_transport_t * server_transport, int queues )
{
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        struct rocketnet_server_fill_line_t * line = &server_transport->fill_lines[i];
        for ( int queue = 0; queue < queues; ++queue )
        {
            const uint64_t rest = line->tails[queue] & ROCKETNET_SERVER_FILL_HALF_MASK;
            if ( rest != 0 )
            {
                server_transport->fill_claims[queue] -= rest;
            }
            line->tails[queue] = 0;
            line->started[queue] = 0;
        }
    }
}

// A refused start with room left refused that one frame; without room, or
// from a transport that does not report it, it is a full staging. With
// several send queues the room that matters is the destination's queue: a
// start refused because its queue is full while another has room is a full
// staging, not a refusal of the frame. NULL to asks the whole room.
static bool rocketnet_server_transport_io_refused_full( const rocketnet_server_transport_t * server_transport, const rocketnet_address_t * to )
{
    if ( to && server_transport->send_queues > 1 )
    {
        return rocketnet_server_transport_queue_room( server_transport, rocketnet_server_transport_send_queue_of( server_transport, to ) ) <= 0;
    }
    return !server_transport->io.operations || !server_transport->io.operations->send_room ||
           server_transport->io.operations->send_room( server_transport->io.instance ) <= 0;
}

// The calling thread's fill line, read only.
static const struct rocketnet_server_fill_line_t * rocketnet_server_fill_line_of_caller( const struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_fill_phase_armed( &server_transport->fill_phase ) )
    {
        return &server_transport->fill_lines[0];
    }
    return &server_transport->fill_lines[rocketnet_fill_phase_line_of( rocketnet_platform_thread_id() )];
}

// The fill frames the queues still allow this phase to the calling thread,
// summed: each queue's budget as its room reads now less its claims, the
// grants of the caller's line's tail (claimed and its to take), and the own
// frames not yet taken (RNSendReserve). Another line's tail is not the
// caller's to take, so it counts as taken. INT_MAX before main first opens
// the budget.
static int rocketnet_server_transport_fill_budget_left( const rocketnet_server_transport_t * server_transport )
{
    const struct rocketnet_server_fill_line_t * line = rocketnet_server_fill_line_of_caller( server_transport );
    uint64_t left = rocketnet_server_transport_load_u64( &server_transport->fill_own_frames_left );
    for ( int queue = 0; queue < server_transport->send_queues; ++queue )
    {
        left += rocketnet_server_transport_load_u64( &line->tails[queue] ) & ROCKETNET_SERVER_FILL_HALF_MASK;
        const uint64_t opened = rocketnet_server_transport_load_u64( &server_transport->fill_budget[queue] );
        if ( opened == UINT64_MAX )
        {
            return INT_MAX;
        }
        const uint64_t now = rocketnet_server_transport_queue_budget_now( server_transport, queue );
        const uint64_t budget = now > opened ? now : opened;
        const uint64_t claims = rocketnet_server_transport_load_u64( &server_transport->fill_claims[queue] );
        left += budget > claims ? budget - claims : 0;
    }
    return left >= (uint64_t) INT_MAX ? INT_MAX : (int) left;
}

// How many payload frames may still start this update: the payload ring's
// free slots on a buffered transport; on a staged one the transport's own
// send room, and no more than the budget the protocol reserve leaves
// (RNSendReserve), so a full staging or a spent budget stops the fair send
// pass where the next unserved client waits. Both count what the calling
// thread may still take: its own fill line's unused chunk (ring slots or
// reserve grants) counts as free, another line's as taken
// (rocketnet_server_fill_line_t), so a thread filling alone reads the exact
// figure.
// proofs/tla/RNServerSendFair.tla FirstVisit and LaterVisit: the shared cap each visit tests, charged per frame start, kept on abort.
// proofs/tla/RNSendReserveCalls.tla Capacity: the room, and no more than the own frames left plus what the budget, as it would be raised now, leaves.
int rocketnet_server_transport_send_capacity_available( const rocketnet_server_transport_t * server_transport )
{
    if ( !server_transport )
    {
        return 0;
    }
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        const int room = rocketnet_server_transport_io_send_room( server_transport );
        const int left = rocketnet_server_transport_fill_budget_left( server_transport );
        return room < left ? room : left;
    }
    // num_packets is moved by fetch-add in start_packet on job threads during
    // a wide fill: read it atomically too.
    const uint64_t used = rocketnet_platform_atomic_load_u64( const_cast<volatile uint64_t *>( &server_transport->payload_ring.num_packets ) );
    const uint64_t chunk = rocketnet_server_transport_load_u64( &rocketnet_server_fill_line_of_caller( server_transport )->ring );
    const uint64_t next = chunk & ROCKETNET_SERVER_FILL_HALF_MASK;
    const uint64_t end = chunk >> ROCKETNET_SERVER_FILL_HALF_BITS;
    const uint64_t own = end > next ? end - next : 0;
    const uint64_t free_slots = ( used >= (uint64_t) server_transport->send_capacity ? 0 : (uint64_t) server_transport->send_capacity - used ) + own;
    return free_slots >= (uint64_t) INT_MAX ? INT_MAX : (int) free_slots;
}

// Every refused start is counted once, by what refused it: the id
// ROCKETNET_TRANSPORT_PACKET_UNREACHABLE is the transport's refusal for want
// of a link-layer destination (TRANSPORTS.md, xdp), SEND_NO_LINK_ADDRESS; a
// refusal with no send room left (or from a transport that does not report
// its room) is a full staging, SEND_RING_FULL; any
// other is the transport refusing that one frame (an address its socket
// cannot carry), which the transport counts in its own send_failures,
// SEND_FAILED. None is ever sent or counted as sent. An uncounted start (the
// receive path's response frame) is counted by its caller.
// The judged form counts a refusal whose room was read once by its caller,
// who acts on the same reading (the disconnect drain); returns full.
static bool rocketnet_server_transport_io_count_refused_judged( rocketnet_server_transport_t * transport, uint64_t id, bool full )
{
    if ( id == ROCKETNET_TRANSPORT_PACKET_UNREACHABLE )
    {
        rocketnet_platform_atomic_add_u64( &transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS], 1 );
        return false;
    }
    if ( full )
    {
        rocketnet_platform_atomic_add_u64( &transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
    }
    return full;
}
static bool rocketnet_server_transport_io_count_refused( rocketnet_server_transport_t * transport, const rocketnet_address_t * to, uint64_t id )
{
    return rocketnet_server_transport_io_count_refused_judged( transport, id, id != ROCKETNET_TRANSPORT_PACKET_UNREACHABLE && rocketnet_server_transport_io_refused_full( transport, to ) );
}
static uint8_t * rocketnet_server_transport_io_start_packet( rocketnet_server_transport_t * transport, const rocketnet_address_t * to, uint64_t * id, bool counted = true )
{
    *id = 0;

    uint8_t * frame = transport->io.operations->start_packet( transport->io.instance, to, id );
    if ( !frame && counted )
    {
        rocketnet_server_transport_io_count_refused( transport, to, *id );
    }
    return frame;
}
static void rocketnet_server_transport_io_finish_packet( rocketnet_server_transport_t * transport, uint64_t id, uint8_t * frame, int bytes )
{
    rocketnet_assert( transport && frame && id != ROCKETNET_TRANSPORT_PACKET_UNREACHABLE && bytes > 0 && bytes <= ROCKETNET_RECEIVE_CEILING );
    if ( !transport || !frame || id == ROCKETNET_TRANSPORT_PACKET_UNREACHABLE || bytes <= 0 || bytes > ROCKETNET_RECEIVE_CEILING )
    {
        return;
    }
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
// proofs/tla/Link.tla models the send: the setting in force at the send holds
// the packet (Send). The model has no action that clears the wire on a setting
// change, so a change removes nothing that was already in flight.
static bool rocketnet_server_link_active( rocketnet_server_transport_t * transport )
{
    if ( !transport->link_simulator )
    {
        return false;
    }
#if ROCKETNET_DEV
    if ( transport->link_setting_busy_for_test > 0 )
    {
        // the lock is held across this one take, as another thread holding
        // it would: a waiting setting stays waiting and the take answers the
        // setting already in force
        transport->link_setting_busy_for_test--;
        rocketnet_platform_mutex_acquire( &transport->link_simulator_mutex );
        const bool active = rocketnet_network_simulator_take_setting( transport->link_simulator, &transport->link_simulator_mutex );
        rocketnet_platform_mutex_release( &transport->link_simulator_mutex );
        return active || rocketnet_network_simulator_patterns_active( transport->link_simulator );
    }
#endif // #if ROCKETNET_DEV
    return rocketnet_network_simulator_take_setting( transport->link_simulator, &transport->link_simulator_mutex ) ||
           rocketnet_network_simulator_patterns_active( transport->link_simulator );
}

// Gives the datagram to an active link simulator. The caller read the setting
// active, once, for this operation, and the send acts on that read: a second
// read here could apply a setting handed over since and answer inactive, and
// a staged transport has no send_batch to take the datagram then.
//
// Returns true when the simulator took the datagram (a simulated loss inside
// the simulator is still a datagram sent and lost on the wire), and false when
// the simulator's lock was busy and the datagram was dropped here. A datagram
// is counted sent only once the link took it, so the callers charge the sent
// statistics on true alone.
static bool rocketnet_server_link_send( rocketnet_server_transport_t * transport,
                                        const rocketnet_address_t * to,
                                        const uint8_t * packet_data, int packet_bytes )
{
    // Datagram staging is best effort. In particular, a shutdown burst must
    // never wait for a producer that still owns the simulator lock.
    // Only a development build has a link simulator, so a lock miss happens
    // only there: the datagram is dropped and counted in the development count
    // link_lock_misses, not in SEND_RING_FULL, which names a full send ring.
#if ROCKETNET_DEV
    if ( transport->link_busy_sends_for_test > 0 )
    {
        transport->link_busy_sends_for_test--;
        rocketnet_platform_atomic_add_u64( &transport->link_lock_misses, 1 );
        return false;
    }
#endif // #if ROCKETNET_DEV
    if ( !rocketnet_platform_mutex_try_acquire( &transport->link_simulator_mutex ) )
    {
#if ROCKETNET_DEV
        rocketnet_platform_atomic_add_u64( &transport->link_lock_misses, 1 );
#endif // #if ROCKETNET_DEV
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
    {
        return false;
    }
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
// link_lock_misses as a real miss does, and never touch the mutex.
void rocketnet_server_transport_link_busy_for_test( struct rocketnet_server_transport_t * server_transport, int sends )
{
    rocketnet_assert( server_transport );
    server_transport->link_busy_sends_for_test = sends > 0 ? sends : 0;
}

// The unit seam for a setting take that meets the lock held without a second
// thread: the next `takes` reads of the link setting hold the simulator's lock
// across the take, so a setting handed over stays waiting and the read answers
// the setting already in force, as a real lock miss does.
void rocketnet_server_transport_link_setting_busy_for_test( struct rocketnet_server_transport_t * server_transport, int takes )
{
    rocketnet_assert( server_transport );
    server_transport->link_setting_busy_for_test = takes > 0 ? takes : 0;
}

uint64_t rocketnet_server_transport_link_lock_misses_for_test( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->link_lock_misses;
}

// The unit seam for a staged transport without a staged socket: exchanges the
// io the server sends through with the one given, so a test can fake the
// staging callbacks on a server that connected over loopback. Called again
// with the returned pair it puts the real io back; the caller does that
// before destroy.
void rocketnet_server_transport_swap_io_for_test( struct rocketnet_server_transport_t * server_transport,
                                                  const struct rocketnet_server_transport_io_operations_t ** operations,
                                                  void ** instance )
{
    rocketnet_assert( server_transport && operations && instance );
    const struct rocketnet_server_transport_io_operations_t * saved_operations = server_transport->io.operations;
    void * saved_instance = server_transport->io.instance;
    server_transport->io.operations = *operations;
    server_transport->io.instance = *instance;
    *operations = saved_operations;
    *instance = saved_instance;
}

uint64_t rocketnet_server_transport_fill_budget_for_test( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    rocketnet_assert( server_transport && queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    return server_transport->fill_budget[queue];
}

uint64_t rocketnet_server_transport_send_reserve_for_test( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    rocketnet_assert( server_transport && queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    return server_transport->send_reserve[queue];
}

void rocketnet_server_transport_set_keep_alive_acked_for_test( struct rocketnet_server_transport_t * server_transport, int client_index, int acked )
{
    rocketnet_assert( server_transport && server_transport->protocol.acks );
    rocketnet_assert( client_index >= 0 && client_index < rocketnet_client_slots_max_clients( server_transport->protocol.client_slots ) );
    server_transport->protocol.acks->endpoints[client_index].keep_alive_acked = (uint8_t) ( acked ? 1 : 0 );
}

uint64_t rocketnet_server_transport_fill_own_frames_for_test( const struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return rocketnet_server_transport_load_u64( &server_transport->fill_own_frames_left );
}

uint64_t rocketnet_server_transport_fill_claims_for_test( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    rocketnet_assert( server_transport && queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    return rocketnet_server_transport_load_u64( &server_transport->fill_claims[queue] );
}

int rocketnet_server_transport_fill_line_for_test( const struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return (int) ( rocketnet_server_fill_line_of_caller( server_transport ) - server_transport->fill_lines );
}

uint64_t rocketnet_server_transport_fill_tail_for_test( const struct rocketnet_server_transport_t * server_transport, int line, int queue )
{
    rocketnet_assert( server_transport && queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    if ( line < 0 )
    {
        line = (int) ( rocketnet_server_fill_line_of_caller( server_transport ) - server_transport->fill_lines );
    }
    rocketnet_assert( line < ROCKETNET_FILL_PHASE_FILL_LINES );
    return rocketnet_server_transport_load_u64( &server_transport->fill_lines[line].tails[queue] );
}

void rocketnet_server_transport_set_fill_tail_for_test( struct rocketnet_server_transport_t * server_transport, int line, int queue, uint64_t word )
{
    rocketnet_assert( server_transport && line >= 0 && line < ROCKETNET_FILL_PHASE_FILL_LINES );
    rocketnet_assert( queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    server_transport->fill_lines[line].tails[queue] = word;
}

void rocketnet_server_transport_refuse_fill_chunks_for_test( struct rocketnet_server_transport_t * server_transport, int chunks )
{
    rocketnet_assert( server_transport );
    server_transport->refuse_fill_chunks_for_test = chunks;
}

uint64_t rocketnet_server_transport_fill_started_for_test( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    rocketnet_assert( server_transport && queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    return rocketnet_server_transport_started_sum( server_transport, queue );
}

uint64_t rocketnet_server_transport_fill_own_used_for_test( const struct rocketnet_server_transport_t * server_transport, int queue )
{
    rocketnet_assert( server_transport && queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    return rocketnet_server_transport_load_u64( &server_transport->fill_own_used[queue] );
}

bool rocketnet_server_transport_fill_own_frame_for_test( const struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_assert( server_transport && client_index >= 0 && client_index < ROCKETNET_MAX_CLIENTS );
    return rocketnet_server_transport_load_u64( &server_transport->fill_own_frame[client_index] ) == 1;
}

void rocketnet_server_transport_add_fill_claims_for_test( struct rocketnet_server_transport_t * server_transport, int queue, uint64_t claims )
{
    rocketnet_assert( server_transport && queue >= 0 && queue < ROCKETNET_SERVER_MAX_SEND_QUEUES );
    server_transport->fill_claims[queue] += claims;
}

void rocketnet_server_transport_retest_take_back_for_test( struct rocketnet_server_transport_t * server_transport, uint64_t claims )
{
    rocketnet_assert( server_transport );
    server_transport->retest_take_back_for_test = claims;
}

void rocketnet_server_transport_steal_ring_chunk_for_test( struct rocketnet_server_transport_t * server_transport, int chunks )
{
    rocketnet_assert( server_transport );
    server_transport->steal_ring_chunk_for_test = chunks;
}

uint64_t rocketnet_server_transport_payload_ring_count_for_test( const struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return rocketnet_platform_atomic_load_u64( const_cast<volatile uint64_t *>( &server_transport->payload_ring.num_packets ) );
}

#if ROCKETNET_DEV
uint64_t rocketnet_server_transport_payload_ring_sealed_for_test( const struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    const struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;
    uint64_t taken = rocketnet_platform_atomic_load_u64( const_cast<volatile uint64_t *>( &ring->num_packets ) );
    if ( taken > (uint64_t) server_transport->send_capacity )
    {
        taken = (uint64_t) server_transport->send_capacity;
    }
    uint64_t sealed = 0;
    for ( uint64_t i = 0; i < taken; ++i )
    {
        sealed += ring->info[i].wire_bytes > 0 ? 1 : 0;
    }
    return sealed;
}
#endif // #if ROCKETNET_DEV

uint64_t rocketnet_server_transport_fill_ring_chunk_for_test( const struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return rocketnet_server_transport_load_u64( &rocketnet_server_fill_line_of_caller( server_transport )->ring );
}

// See rocketnet_dev_fail_disconnect_sealed_writes in rocketnet_test_seams.h.
// The count stays put until the drain abandons the burst, so a later stop
// step hits the same head entry and fails it again.
static int rocketnet_dev_disconnect_sealed_write_fails = 0;

void rocketnet_dev_fail_disconnect_sealed_writes( int count )
{
    rocketnet_dev_disconnect_sealed_write_fails = count > 0 ? count : 0;
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
        {
            transport->io.operations->send_batch_shutdown( transport->io.instance, sends, num_sends );
        }
        else
        {
            transport->io.operations->send_batch( transport->io.instance, sends, num_sends );
        }
        for ( int i = 0; i < num_sends; i++ )
        {
            accepted[i] = true;
        }
        return num_sends;
    }

    int num_accepted = 0;

    for ( int i = 0; i < num_sends; i++ )
    {
        accepted[i] = rocketnet_server_link_send( transport, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
        if ( accepted[i] )
        {
            num_accepted++;
        }
    }

    return num_accepted;
}

// Link!ReverseWindow: the window the simulator holds is emitted reversed,
// whole, at the end of each send_packets (and of the update that sends). A
// link without patterns holds nothing and takes no lock here.
static void rocketnet_server_link_flush( rocketnet_server_transport_t * transport )
{
    if ( !rocketnet_server_link_active( transport ) || !rocketnet_network_simulator_patterns_active( transport->link_simulator ) )
    {
        return;
    }
    if ( !rocketnet_platform_mutex_try_acquire( &transport->link_simulator_mutex ) )
    {
        return; // retain queued simulator packets; the next ordinary flush may retry
    }
    rocketnet_network_simulator_flush_window( transport->link_simulator );
    rocketnet_platform_mutex_release( &transport->link_simulator_mutex );
}

// The drain runs whenever the server has a simulator, whatever the setting:
// a switch from latency or jitter to all-zero leaves the packets the
// simulator already holds, and they still leave when they come due (as the
// client drain does). Gating it on the setting would strand and lose them.
// A simulator holding nothing is read without the lock and drains nothing.
// A due packet leaves only while the transport reports send room; the rest
// stay held for the next update's drain, so none is refused and lost
// (proofs/tla/ServerStopSpread.tla Room).  A busy simulator lock leaves the
// held packets for the next drain.  This path does not wait, except for the
// stop's one window flush, which must not leave packets outside the slots.
static void rocketnet_server_link_drain( rocketnet_server_transport_t * transport, bool shutdown = false )
{
    if ( !transport->link_simulator )
    {
        return;
    }
    if ( shutdown && rocketnet_network_simulator_patterns_active( transport->link_simulator ) )
    {
        rocketnet_platform_mutex_acquire( &transport->link_simulator_mutex );
        rocketnet_network_simulator_flush_window( transport->link_simulator );
        rocketnet_platform_mutex_release( &transport->link_simulator_mutex );
    }
    if ( !rocketnet_network_simulator_holds_any( transport->link_simulator ) )
    {
        return;
    }
    const double cutoff_time = shutdown ? DBL_MAX : transport->time;
    for ( ;; )
    {
        if ( rocketnet_server_transport_io_send_room( transport ) <= 0 )
        {
            break;
        }

        rocketnet_address_t from, to;

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        if ( !rocketnet_platform_mutex_try_acquire( &transport->link_simulator_mutex ) )
        {
            break;
        }

        const int packet_bytes = rocketnet_network_simulator_receive_any( transport->link_simulator,
                                                                          cutoff_time, &from, &to, packet );
        rocketnet_platform_mutex_release( &transport->link_simulator_mutex );
        if ( packet_bytes <= 0 )
        {
            break;
        }
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
            if ( shutdown && transport->io.operations->send_batch_shutdown )
            {
                transport->io.operations->send_batch_shutdown( transport->io.instance, &send, 1 );
            }
            else
            {
                transport->io.operations->send_batch( transport->io.instance, &send, 1 );
            }
        }
    }

    if ( rocketnet_server_transport_staged( transport ) )
    {
        rocketnet_server_transport_io_send_packets( transport );
    }
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

    struct rocketnet_platform_send_t * sends = server_transport->send_buffer.sends;
    bool * accepted = server_transport->send_buffer.accepted;
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
    {
        rocketnet_server_link_send_batch( server_transport, sends, num_sends, accepted, shutdown );
    }
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
// transport's staging, or returns NULL and drops the frame when the staging is full;
// an uncounted reserve leaves counting a refusal to the caller
static uint8_t * rocketnet_server_reserve_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                      const rocketnet_address_t * to,
                                                      struct rocketnet_server_frame_t * frame, bool shutdown = false,
                                                      bool counted = true )
{
    frame->to = *to;
    frame->client_index = -1;
    frame->packet_id = 0;

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        frame->data = rocketnet_server_transport_io_start_packet( server_transport, to, &frame->packet_id, counted );
        if ( !frame->data )
        {
            rocketnet_spam( "server transport send staging full. packet dropped" );
        }

        return frame->data;
    }

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;

    if ( send_buffer->num_packets >= send_buffer->capacity )
    {
        rocketnet_server_flush_control_buffer( server_transport, shutdown );
    }

    const int index = send_buffer->num_packets;
    send_buffer->to[index] = *to;
    send_buffer->packet_bytes[index] = 0;
    frame->data = send_buffer->data + index * ROCKETNET_FRAME_SIZE;
    return frame->data;
}

// The commit choice of a staged frame, written once: when the link simulator
// is active it takes the datagram and the staging slot goes back (the frame's
// data is cleared, so a later withdrawal does not return it twice); else the
// staging finishes the frame. Returns true when the datagram left, and a
// datagram is counted sent only then.
static bool rocketnet_server_commit_staged( struct rocketnet_server_transport_t * server_transport,
                                            const rocketnet_address_t * to, uint64_t packet_id,
                                            uint8_t ** data, int wire_bytes )
{
    if ( rocketnet_server_link_active( server_transport ) )
    {
        const bool sent = rocketnet_server_link_send( server_transport, to, *data + ROCKETNET_FRAME_WIRE_OFFSET, wire_bytes );
        rocketnet_server_transport_io_abort_packet( server_transport, packet_id, *data );
        *data = NULL;
        return sent;
    }
    rocketnet_server_transport_io_finish_packet( server_transport, packet_id, *data, wire_bytes );
    return true;
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
    {
        return;
    }

    // every commit through this seam -- responses, keep-alives, disconnect
    // bursts -- runs on the main thread, so it lands in the current stats
    // bucket and the cumulative totals directly

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        // a datagram is counted sent only once the link took it

        if ( rocketnet_server_commit_staged( server_transport, &frame->to, frame->packet_id, &frame->data, packet_bytes ) )
        {
            rocketnet_server_charge_send_frame( server_transport, frame->client_index, &frame->to, packet_bytes );
        }
        return;
    }

    // the portable frame is charged here, at its commit

    rocketnet_server_charge_send_frame( server_transport, frame->client_index, &frame->to, packet_bytes );

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;
    send_buffer->packet_bytes[send_buffer->num_packets] = packet_bytes;
    send_buffer->num_packets++;
}

static void rocketnet_server_abort_send_frame( struct rocketnet_server_transport_t * server_transport,
                                               struct rocketnet_server_frame_t * frame )
{
    if ( !frame->data )
    {
        return;
    }

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_transport_io_abort_packet( server_transport, frame->packet_id, frame->data );
    }

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
    {
        return;
    }
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
    {
        return 0;
    }
    size_t bytes = 0;
    if ( pthread_attr_getstacksize( &attributes, &bytes ) != 0 )
    {
        bytes = 0;
    }
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
    {
        return false;
    }
    rocketnet_server_transport_config_t defaults;

    if ( !config )
    {
        rocketnet_server_transport_default_config( &defaults );
        config = &defaults;
    }

    const int clients = config->max_clients;
    if ( clients < 1 || clients > ROCKETNET_MAX_CLIENTS || config->num_xdp_queues < 0 || config->num_xdp_queues > 1024 )
    {
        return false;
    }

    rocketnet_server_transport_layout_t layout;
    if ( !rocketnet_server_transport_layout( clients, &layout ) )
    {
        return false;
    }
    plan.transport_bytes = rocketnet_memory_allocations( { layout.bytes } );
    for ( uint64_t bytes : { rocketnet_encryption_map_allocation_bytes( clients * ROCKETNET_ENCRYPTION_MAP_RATIO ),
                             rocketnet_token_table_allocation_bytes( clients * ROCKETNET_USED_TOKEN_HISTORY_RATIO ),
                             rocketnet_client_slots_allocation_bytes( clients ), rocketnet_server_acks_allocation_bytes( clients ),
                             rocketnet_server_acks_defer_allocation_bytes( clients ),
                             rocketnet_memory_allocations( { (uint64_t) clients * sizeof( uint64_t ), (uint64_t) clients * ROCKETNET_SENT_PACKET_BUFFER_SIZE * sizeof( uint64_t ),
                                                             (uint64_t) clients * sizeof( int ) } ),
        // The plan counts what the transport allocates, once: the per-client wire totals are a separate allocation.
                             rocketnet_memory_allocations( { (uint64_t) clients * sizeof( rocketnet_server_wire_totals_t ) } ) } )
    {
        if ( !bytes || !rocketnet_memory_add( plan.protocol_bytes, 1, bytes ) )
        {
            return false;
        }
    }

    if ( !rocketnet_builtin_server_memory_plan( config, transport, plan ) )
    {
        return false;
    }

    // a transport that takes replay marks (xdp) gets the per-slot replay
    // records at create (rocketnet_server_replay_marks_create)

    for ( int i = 0; i < ROCKETNET_NUM_TRANSPORTS; i++ )
    {
        const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( i );
        if ( !definition || strcmp( definition->name, transport ) != 0 )
        {
            continue;
        }
        if ( definition->backend && definition->backend->server && definition->backend->server->replay_mark &&
             !rocketnet_memory_add( plan.protocol_bytes, 1, rocketnet_server_replay_marks_allocation_bytes( clients ) ) )
        {
            return false;
        }
        break;
    }

    // loopback opens no socket and plans no I/O bytes: its endpoint is in the caller's hub
    if ( !plan.transport_bytes || ( plan.sockets != 0 && !plan.transport_io_bytes ) )
    {
        return false;
    }
    for ( uint64_t bytes : { plan.transport_bytes, plan.protocol_bytes, plan.transport_io_bytes, plan.registered_region_bytes } )
    {
        if ( !rocketnet_memory_add( plan.allocator_bytes, 1, bytes ) )
        {
            return false;
        }
    }
    if ( rocketnet_public_link_simulator_development() )
    {
        // the link simulator every development server owns, created with it
        const uint64_t simulator_bytes = rocketnet_network_simulator_memory_bytes( ROCKETNET_SERVER_LINK_SIMULATOR_SLOTS,
                                                                                   config->simulated_reverse_window );
        if ( !simulator_bytes || !rocketnet_memory_add( plan.allocator_bytes, 1, simulator_bytes ) )
        {
            return false;
        }
    }

    // the RIO registered region comes from the allocator; UMEM is page-aligned
    // memory the AF_XDP socket registers with the kernel, outside it

    plan.total_bytes = plan.allocator_bytes;
    if ( !rocketnet_memory_add( plan.total_bytes, 1, plan.umem_bytes ) )
    {
        return false;
    }
    if ( plan.worker_threads > 0 )
    {
        const uint64_t stack = rocketnet_thread_stack_bytes();
        if ( !stack || !rocketnet_memory_add( plan.thread_stack_bytes, (uint64_t) plan.worker_threads, stack ) )
        {
            return false;
        }
    }

    for ( uint64_t bytes : { plan.umem_bytes, plan.thread_stack_bytes, plan.mapped_ring_bytes } )
    {
        if ( !rocketnet_memory_add( plan.os_bytes, 1, bytes ) )
        {
            return false;
        }
    }
    return plan.total_bytes <= SIZE_MAX;
}

uint64_t rocketnet_server_transport_order_allocator_bytes( const rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_config_t defaults;

    if ( !config )
    {
        rocketnet_server_transport_default_config( &defaults );
        config = &defaults;
    }

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    struct rocketnet_transport_order_t ladder;
    if ( !rocketnet_transport_order_parse( config->transport, available, num_available, &ladder ) )
    {
        return 0;
    }
    uint64_t most = 0;

    for ( int i = 0; i < ladder.num_candidates; ++i )
    {
        rocketnet_server_memory_plan_t plan;
        if ( rocketnet_server_transport_memory_plan( config, rocketnet_transport_name( ladder.candidates[i] ), plan ) && plan.allocator_bytes > most )
        {
            most = plan.allocator_bytes;
        }
    }

    return most;
}

uint64_t rocketnet_server_transport_required_memory( const rocketnet_server_transport_config_t * config, const char * transport )
{
    rocketnet_server_memory_plan_t plan;
    return rocketnet_server_transport_memory_plan( config, transport, plan ) ? plan.total_bytes : 0;
}

// Whole-host estimate (TRANSPORT_READINESS.md): the plan plus what each
// built-in transport asks of the kernel. Requests only, never grants. The sizes
// are the ones the transports pass: every UDP server socket sets SO_SNDBUF to
// ROCKETNET_SOCKET_SEND_BUFFER_SIZE and SO_RCVBUF to
// rocketnet_server_transport_receive_buffer_request, the function each create
// calls (rocketnet_driver_builtin.cpp, rocketnet_server_gso_linux.cpp,
// rocketnet_server_uring_linux.cpp, rocketnet_server_iocp_windows.cpp,
// rocketnet_server_rio_windows.cpp); each xdp queue's AF_XDP socket asks for
// ROCKETNET_XDP_RECV_QUEUE_SIZE RX and ROCKETNET_XDP_SEND_QUEUE_SIZE TX
// descriptors and sets no socket buffer (rocketnet_server_socket_linux.cpp).
bool rocketnet_server_transport_host_memory_estimate( const struct rocketnet_server_transport_config_t * config,
                                                      const char * transport,
                                                      struct rocketnet_host_memory_estimate_t * estimate )
{
    if ( !rocketnet_api_null_guard( estimate ) )
    {
        return false;
    }
    memset( estimate, 0, sizeof( *estimate ) );

    rocketnet_server_memory_plan_t plan;
    if ( !transport || !rocketnet_server_transport_memory_plan( config, transport, plan ) )
    {
        return false;
    }

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
        {
            return false;
        }
#else
        return false;
#endif // #if ROCKETNET_XDP
    }
    else
    {
        const int max_clients = config && config->max_clients > 0 ? config->max_clients : ROCKETNET_DEFAULT_MAX_CLIENTS;
        if ( !rocketnet_memory_add( e.socket_send_buffer_bytes, (uint64_t) plan.sockets, ROCKETNET_SOCKET_SEND_BUFFER_SIZE ) ||
             !rocketnet_memory_add( e.socket_receive_buffer_bytes, (uint64_t) plan.sockets,
                                    (uint64_t) rocketnet_server_transport_receive_buffer_request( transport, max_clients ) ) )
        {
            return false;
        }
    }

    if ( plan.worker_threads > 0 )
    {
        e.thread_stack_bytes_per_thread = rocketnet_thread_stack_bytes();
        if ( !e.thread_stack_bytes_per_thread ||
             !rocketnet_memory_add( e.thread_stack_bytes, (uint64_t) plan.worker_threads, e.thread_stack_bytes_per_thread ) )
        {
            return false;
        }
    }

    for ( uint64_t bytes : { e.allocator_bytes, e.umem_bytes, e.registered_region_bytes, e.socket_send_buffer_bytes,
                             e.socket_receive_buffer_bytes, e.xdp_ring_bytes, e.thread_stack_bytes } )
    {
        if ( !rocketnet_memory_add( e.total_bytes, 1, bytes ) )
        {
            return false;
        }
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
    {
        append( "io_uring submission and completion rings and the provided buffer rings (plan.mapped_ring_bytes counts the provided buffer rings; kernel and liburing mappings outside the SDK allocator)" );
    }
    if ( plan.worker_threads > 0 )
    {
        append( "thread guard pages, thread-local storage and kernel thread structures; stacks count reserved address space, not resident pages" );
    }
    append( "kernel page tables and loader/shared-library memory" );
    append( "libc allocator bookkeeping outside the SDK allocator" );
    append( "application memory" );
    rocketnet_assert( count <= ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS );
    for ( int i = 0; i < count; i++ )
    {
        e.exclusions[i] = excluded[i];
    }
    e.num_exclusions = count;

    *estimate = e;

    return true;
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context,
                                                                         const char * public_address_string,
                                                                         const struct rocketnet_server_transport_config_t * config )
{
    return rocketnet_server_transport_create_internal( context, public_address_string, config, 0.0, NULL );
}

static struct rocketnet_server_transport_t * rocketnet_server_transport_create_impl( void * context,
                                                                                     const char * public_address_string,
                                                                                     const struct rocketnet_server_transport_config_t * config,
                                                                                     double time, int * status,
                                                                                     const uint64_t * resolved_memory_budget = NULL );

// The config staging seam, compiled into the development library only: each
// stack copy of a server config, which carries server_secret_key, is wiped on
// every exit of the function that holds it, and is then handed to the hook so
// a unit case reads what the copy still holds (test_unit_secrets_wiped.cpp).

#if ROCKETNET_DEV
void ( *rocketnet_server_transport_config_staging_hook )( const uint8_t * secret_key, int stage ) = NULL;
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_SEEN( secret_key, stage ) do { if ( rocketnet_server_transport_config_staging_hook ) { rocketnet_server_transport_config_staging_hook( secret_key, stage ); } } while ( 0 )
#else
#define ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_SEEN( secret_key, stage ) do {} while ( 0 )
#endif // #if ROCKETNET_DEV

struct rocketnet_server_transport_t * rocketnet_server_transport_create_internal( void * context, const char * public_address_string,
                                                                                  const struct rocketnet_server_transport_config_t * config,
                                                                                  double time, int * status )
{
    return rocketnet_server_transport_create_impl( context, public_address_string, config, time, status );
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create_current( void * context, const char * address,
                                                                                 const struct rocketnet_server_transport_config_t * config )
{
    return rocketnet_server_transport_create_impl( context, address, config, 0.0, NULL );
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create_current_internal( void * context, const char * address,
                                                                                          const struct rocketnet_server_transport_config_t * config, double time, int * status )
{
    return rocketnet_server_transport_create_impl( context, address, config, time, status );
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create_resolved( void * context, const char * address,
                                                                                  const struct rocketnet_server_transport_config_t * config,
                                                                                  uint64_t memory_budget_bytes, int * status )
{
    rocketnet_assert( config );
    return rocketnet_server_transport_create_impl( context, address, config, 0.0, status, &memory_budget_bytes );
}

// The inputs a server create starts from, resolved before anything is
// allocated or opened: the layered config (DEFAULT < STRUCT < ENV) validated,
// the served addresses and the bind they share, the parsed transport list,
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
    struct rocketnet_transport_order_t ladder;
    bool budget_refused[ROCKETNET_NUM_TRANSPORTS];
};

// The local address the server binds. Unset, the served family's wildcard on
// the served port (both families share the IPv6 wildcard). Set
// (bind_ipv4_address or bind_ipv6_address), that address on the served port:
// the address clients connect to and the address the server binds differ
// behind NAT. Refused with INVALID_ADDRESS, one log line each: an address
// that does not parse, of the other family, with a port (":0" included),
// multicast, the IPv4 broadcast, IPv4-mapped IPv6 or IPv6 link-local (no
// scope to carry); a bind address for a family the server does not serve; and
// any bind address on a server that serves both families, whose one
// dual-stack socket cannot bind two specific addresses.
// The environment is ambient: set once for a host or a test run, it cannot
// know each server's families, so a bind address it sets for a family the
// server cannot bind specifically (a family it does not serve, or either
// family on a server that serves both on one dual-stack socket) is dropped
// with one log line, never refused: the environment never makes a server
// fail to start for its families. A config field is a statement about this
// server, so the resolve below still refuses those.
void rocketnet_server_transport_ignore_ambient_bind( struct rocketnet_server_transport_config_t * config, const char * ipv4_before, const char * ipv6_before, int binds_ipv4,
                                                     int binds_ipv6 )
{
    rocketnet_assert( config && ipv4_before && ipv6_before );
    if ( !binds_ipv4 && strcmp( config->bind_ipv4_address, ipv4_before ) != 0 )
    {
        rocketnet_info( "server config ignores ROCKETNET_SERVER_BIND_IPV4_ADDRESS=%s: the server binds no specific IPv4 address (it serves IPv6 only, or both families)",
                        config->bind_ipv4_address );
        rocketnet_copy_string( config->bind_ipv4_address, ipv4_before, sizeof( config->bind_ipv4_address ) );
    }
    if ( !binds_ipv6 && strcmp( config->bind_ipv6_address, ipv6_before ) != 0 )
    {
        rocketnet_info( "server config ignores ROCKETNET_SERVER_BIND_IPV6_ADDRESS=%s: the server binds no specific IPv6 address (it serves IPv4 only, or both families)",
                        config->bind_ipv6_address );
        rocketnet_copy_string( config->bind_ipv6_address, ipv6_before, sizeof( config->bind_ipv6_address ) );
    }
}

bool rocketnet_server_transport_resolve_bind_address( const struct rocketnet_server_transport_config_t * config, const rocketnet_address_t * public_address,
                                                      int serves_both, rocketnet_address_t * bind_address, int * status )
{
    rocketnet_assert( config );
    rocketnet_assert( public_address );
    rocketnet_assert( bind_address );

    const bool ipv4_set = config->bind_ipv4_address[0] != '\0';
    const bool ipv6_set = config->bind_ipv6_address[0] != '\0';

    memset( bind_address, 0, sizeof( *bind_address ) );

    if ( serves_both )
    {
        if ( ipv4_set || ipv6_set )
        {
            if ( status )
            {
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            }
            rocketnet_error( "server bind address refused: a server that serves both families binds the IPv6 wildcard; serve one family to bind a specific address" );
            return false;
        }

        // Both families share the IPv6 wildcard on the one port, with
        // IPV6_V6ONLY off: the bind a "[::]" server takes.
        bind_address->type = ROCKETNET_ADDRESS_IPV6;
        bind_address->port = public_address->port;
        rocketnet_info( "server transport binds [::]:%d, both families", (int) public_address->port );
        return true;
    }

    const bool ipv4 = public_address->type == ROCKETNET_ADDRESS_IPV4;
    const char * field = ipv4 ? "bind_ipv4_address" : "bind_ipv6_address";
    const char * text = ipv4 ? config->bind_ipv4_address : config->bind_ipv6_address;

    if ( ( ipv4 && ipv6_set ) || ( !ipv4 && ipv4_set ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_INVALID_ADDRESS;
        }
        rocketnet_error( "server bind address refused: %s is set, but the server serves only %s", ipv4 ? "bind_ipv6_address" : "bind_ipv4_address", ipv4 ? "IPv4" : "IPv6" );
        return false;
    }

    if ( text[0] == '\0' )
    {
        bind_address->type = public_address->type;
        bind_address->port = public_address->port;
        rocketnet_info( "server transport binds the %s wildcard, port %d", ipv4 ? "IPv4" : "IPv6", (int) public_address->port );
        return true;
    }

    rocketnet_address_t parsed;
    const char * refusal = NULL;
    if ( !rocketnet_address_parse( &parsed, text ) )
    {
        refusal = "does not parse";
    }
    else if ( parsed.type != public_address->type )
    {
        refusal = ipv4 ? "is not an IPv4 address" : "is not an IPv6 address";
    }
    else if ( parsed.port != 0 || ( ipv4 ? strchr( text, ':' ) != NULL : strstr( text, "]:" ) != NULL ) )
    {
        refusal = "carries a port; the port is the served address's";
    }
    else if ( ipv4 ? ( parsed.data.ipv4[0] >= 224 && parsed.data.ipv4[0] <= 239 ) : ( parsed.data.ipv6[0] & 0xff00 ) == 0xff00 )
    {
        refusal = "is a multicast address";
    }
    else if ( ipv4 && parsed.data.ipv4[0] == 255 && parsed.data.ipv4[1] == 255 && parsed.data.ipv4[2] == 255 && parsed.data.ipv4[3] == 255 )
    {
        refusal = "is the broadcast address";
    }
    else if ( !ipv4 && parsed.data.ipv6[0] == 0 && parsed.data.ipv6[1] == 0 && parsed.data.ipv6[2] == 0 && parsed.data.ipv6[3] == 0 && parsed.data.ipv6[4] == 0 &&
              parsed.data.ipv6[5] == 0xffff )
    {
        refusal = "is an IPv4-mapped address; set bind_ipv4_address and serve IPv4";
    }
    else if ( !ipv4 && ( parsed.data.ipv6[0] & 0xffc0 ) == 0xfe80 )
    {
        refusal = "is link-local; its bind needs an interface scope, which the field cannot carry";
    }

    if ( refusal )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_INVALID_ADDRESS;
        }
        rocketnet_error( "server bind address refused: %s \"%s\" %s", field, text, refusal );
        return false;
    }

    *bind_address = parsed;
    bind_address->port = public_address->port;

    char bound[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server transport binds %s (%s)", rocketnet_address_to_string( bind_address, bound ), field );
    return true;
}

static bool rocketnet_server_transport_resolve_staged( const char * public_address_string,
                                                       const struct rocketnet_server_transport_config_t * config,
                                                       const uint64_t * resolved_memory_budget,
                                                       struct rocketnet_server_transport_resolved_t * out,
                                                       int * status,
                                                       struct rocketnet_server_transport_config_t & effective_config )
{
    rocketnet_assert( public_address_string );
    rocketnet_assert( out );

    // the config layering: DEFAULT < STRUCT < ENV. NULL config is
    // the default shape; the caller's struct is the middle layer; the
    // environment overlays both through the config table, every override
    // logged loudly with the value it displaced. validation refuses
    // loudly and never clamps. A config the public create already resolved
    // (resolved_memory_budget set, rocketnet_server_transport_create_resolved)
    // takes no second environment pass: start uses the config create
    // validated and sized, whatever the environment says now.

    if ( config )
    {
        effective_config = *config;
    }
    else
    {
        rocketnet_server_transport_default_config_current( &effective_config );
    }

    int num_config_fields = 0;
    const struct rocketnet_config_field_t * config_fields = rocketnet_server_transport_config_fields( &num_config_fields );

    // the bind addresses before the environment pass: one the environment
    // sets for a family this server does not serve is dropped below, once
    // the served families are known (rocketnet_server_transport_ignore_ambient_bind)
    const bool env_pass = !resolved_memory_budget;
    char bind_before[2][ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_copy_string( bind_before[0], effective_config.bind_ipv4_address, sizeof( bind_before[0] ) );
    rocketnet_copy_string( bind_before[1], effective_config.bind_ipv6_address, sizeof( bind_before[1] ) );

    if ( env_pass && !rocketnet_config_apply_env( "server", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        }
        return false;
    }

    if ( !rocketnet_config_validate( "server", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        }
        return false;
    }

    if ( !rocketnet_config_validate_public_link_simulator( "server", &effective_config, config_fields, num_config_fields, rocketnet_public_link_simulator_development() ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        }
        return false;
    }

    if ( rocketnet_config_packet_filter_refused( "server", effective_config.packet_filter ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION;
        }
        return false;
    }

    config = &effective_config;

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
        {
            *status = ROCKETNET_STATUS_MISSING_KEYS;
        }
        rocketnet_error( "server config has no keys: fill rocketnet_server_transport_dev_config() for development, or supply production keys" );
        return false;
    }

    // Refuse protocol_id 0 in every build and with any keys: a title has its own non-zero protocol_id

    if ( config->protocol_id == 0 )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_INVALID_PROTOCOL_ID;
        }
        rocketnet_error( "server config protocol_id 0 is refused: set the title's non-zero protocol_id" );
        return false;
    }

    // the transport candidate list parses before anything allocates: a typo
    // fails here, loudly, listing the valid names

    int num_available_transports = 0;
    const int * available_transports = rocketnet_server_transport_io_availability( &num_available_transports );

    struct rocketnet_transport_order_t ladder;
    if ( !rocketnet_transport_order_parse( config->transport, available_transports, num_available_transports, &ladder ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_INVALID_TRANSPORT_LIST;
        }
        return false;
    }

    rocketnet_info( "server transport public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_INVALID_ADDRESS;
        }
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
            {
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            }
            rocketnet_error( "server second_public_address does not parse" );
            return false;
        }

        if ( second_address.type == public_address.type )
        {
            if ( status )
            {
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            }
            rocketnet_error( "server second_public_address is the same family as the public address" );
            return false;
        }

        if ( second_address.port != public_address.port )
        {
            if ( status )
            {
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            }
            rocketnet_error( "server second_public_address is on a different port from the public address" );
            return false;
        }

        serves_both = 1;
    }

    if ( env_pass )
    {
        const bool ipv4 = public_address.type == ROCKETNET_ADDRESS_IPV4;
        rocketnet_server_transport_ignore_ambient_bind( &effective_config, bind_before[0], bind_before[1], !serves_both && ipv4, !serves_both && !ipv4 );
    }

    rocketnet_address_t bind_address;
    if ( !rocketnet_server_transport_resolve_bind_address( &effective_config, &public_address, serves_both, &bind_address, status ) )
    {
        return false;
    }

    // the operator memory budget: each candidate's pre-allocation plan
    // is compared to the budget before any allocation, and a candidate whose
    // plan exceeds it is refused exactly as a refusing transport is -- logged with
    // the plan and the budget, and skipped so the ladder falls to the next
    // candidate. a budget that no candidate fits refuses the create before the
    // first allocation.

    uint64_t memory_budget = resolved_memory_budget ? *resolved_memory_budget : 0;
    if ( !resolved_memory_budget && !rocketnet_config_apply_env_uint64( "server", "memory_budget_bytes", &memory_budget ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        }
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
            {
                budget_allowed++;
            }
        }
        else
        {
            rocketnet_info( "server memory plan [%s]: unavailable; consult this transport's resource policy", name );
        }
    }

    if ( memory_budget > 0 && budget_refused_total > 0 && budget_allowed == 0 )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_NO_TRANSPORT_FOUND;
        }
        rocketnet_error( "server transport: no transport found: every candidate's memory plan exceeds the %llu byte operator budget",
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

// effective_config carries the caller's server_secret_key: it is wiped on
// every exit of the resolve, refused or resolved

static bool rocketnet_server_transport_resolve( const char * public_address_string,
                                                const struct rocketnet_server_transport_config_t * config,
                                                const uint64_t * resolved_memory_budget,
                                                struct rocketnet_server_transport_resolved_t * out,
                                                int * status )
{
    struct rocketnet_server_transport_config_t effective_config;
    const bool resolved = rocketnet_server_transport_resolve_staged( public_address_string, config, resolved_memory_budget, out, status, effective_config );
    sodium_memzero( &effective_config, sizeof( effective_config ) );
    ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_SEEN( effective_config.server_secret_key, ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_RESOLVE );
    return resolved;
}

// THE LADDER: opens the resolved candidates in order until one grants,
// into server_transport->io. Returns the granted transport, with bind_address
// holding what it bound; -1 when none granted, with *status saying why (an
// open that ran out of memory ends the ladder rather than falling).

static int rocketnet_server_transport_open_in_order( struct rocketnet_server_transport_t * server_transport, void * context,
                                                     const struct rocketnet_server_transport_resolved_t * resolved,
                                                     rocketnet_address_t * bind_address, int * status )
{
    const struct rocketnet_server_transport_config_t * config = &resolved->config;
    const char * public_address_string = resolved->public_address_string;
    const rocketnet_address_t * public_address = &resolved->public_address;
    const struct rocketnet_transport_order_t * ladder = &resolved->ladder;
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

    // WHICH rungs the port-zero rule refused, so an exhausted ladder
    // can restate the rule at the level the failure itself is reported.
    // The names are collected rather than a bare flag because a mixed list
    // -- say "xdp,gso" where gso refused for its own queue geometry --
    // would make "no transport here can discover a port" a lie about gso. The
    // error names the rungs the rule actually refused, and nothing else.

    char port_zero_refused_names[128];
    port_zero_refused_names[0] = '\0';

    bool port_zero_refused_seen[ROCKETNET_NUM_TRANSPORTS];
    memset( port_zero_refused_seen, 0, sizeof( port_zero_refused_seen ) );

    // Opens only, not port-zero skips. TRANSPORT_FAMILIES_UNSUPPORTED,
    // TRANSPORT_NOT_BUILT, TRANSPORT_BIND_REFUSED and INVALID_ADDRESS are the
    // start status when every open refused for that reason. The xdp transport
    // binds no datagram socket from the bind address (it serves the interface
    // that holds the public address), so its refusal says nothing about the
    // address: BIND_REFUSED is the status when at least one transport was
    // refused its bind and every other refusal is the xdp transport's (a
    // candidate skipped for the memory budget is not an open and not counted).

    int open_refusals = 0;
    int families_unsupported_refusals = 0;
    int not_built_refusals = 0;
    int bind_refusals = 0;
    int xdp_refusals = 0;
    int invalid_address_refusals = 0;

    for ( int i = 0; i < ladder->num_candidates && granted_transport < 0; i++ )
    {
        const int candidate = ladder->candidates[i];

        // a candidate whose memory plan the operator budget refused was logged
        // and refused in the plan loop above; record it for the exhausted
        // summary and fall to the next candidate, never opening it

        if ( budget_refused[candidate] )
        {
            if ( !refused_seen[candidate] )
            {
                refused_seen[candidate] = true;

                const size_t used = strlen( refused_names );
                snprintf( refused_names + used, sizeof( refused_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( candidate ) );
            }

            continue;
        }

        // A port-zero public address asks the KERNEL for an
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
        // one rung that fails it today, because create is a loop
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

            if ( !port_zero_refused_seen[candidate] )
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
            {
                granted_transport = candidate;
            }

            // a rung that failed for want of memory is not a refusal: the
            // create ends with OUT_OF_MEMORY and never falls to the next
            // candidate. Every transport reports it through transport_status
            // (rocketnet_server_transport_io_config_t.status, sdk/rocketnet_driver.h);
            // any other failed open is a refusal and falls

            if ( granted_transport < 0 && transport_status == ROCKETNET_STATUS_OUT_OF_MEMORY )
            {
                rocketnet_error( "server transport create: the %s transport could not allocate its endpoint", rocketnet_transport_name( candidate ) );
                if ( status )
                {
                    *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
                }
                return -1;
            }

            if ( granted_transport < 0 )
            {
                open_refusals++;
                if ( transport_status == ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED )
                {
                    families_unsupported_refusals++;
                }
                if ( transport_status == ROCKETNET_STATUS_TRANSPORT_NOT_BUILT )
                {
                    not_built_refusals++;
                }
                if ( candidate == ROCKETNET_TRANSPORT_XDP && transport_status != ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED )
                {
                    xdp_refusals++;
                }
                if ( transport_status == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED )
                {
                    bind_refusals++;
                }
                if ( transport_status == ROCKETNET_STATUS_INVALID_ADDRESS )
                {
                    invalid_address_refusals++;
                }
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

            if ( !refused_seen[candidate] )
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

        rocketnet_error( "server transport: no transport found. refused: %s. skipped for platform: %s",
                         refused_names[0] != '\0' ? refused_names : "(none)",
                         skipped_names[0] != '\0' ? skipped_names : "(none)" );

        // An error naming the rule is wanted, and the per-rung warn
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
            {
                *status = ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED;
            }
            else if ( open_refusals > 0 && families_unsupported_refusals == open_refusals )
            {
                *status = ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED;
            }
            else if ( open_refusals > 0 && not_built_refusals == open_refusals )
            {
                *status = ROCKETNET_STATUS_TRANSPORT_NOT_BUILT;
            }
            else if ( bind_refusals > 0 && bind_refusals + xdp_refusals == open_refusals )
            {
                *status = ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED;
            }
            else if ( open_refusals > 0 && invalid_address_refusals == open_refusals )
            {
                *status = ROCKETNET_STATUS_INVALID_ADDRESS;
            }
            else
            {
                *status = ROCKETNET_STATUS_NO_TRANSPORT_FOUND;
            }
        }

        return -1;
    }

    if ( refused_names[0] != '\0' )
    {
        rocketnet_info( "server transport %s granted after refusals: %s", rocketnet_transport_name( granted_transport ), refused_names );
    }

    return granted_transport;
}

static struct rocketnet_server_transport_t * rocketnet_server_transport_create_staged( void * context,
                                                                                       const char * public_address_string,
                                                                                       const struct rocketnet_server_transport_config_t * config,
                                                                                       double time, int * status,
                                                                                       const uint64_t * resolved_memory_budget,
                                                                                       struct rocketnet_server_transport_resolved_t & resolved )
{
    // docs/ERRORS-AND-LOGGING.md 2.1: a NULL required argument is a logged
    // INVALID_ARGUMENT and a NULL return in debug and release
    if ( !rocketnet_api_null_guard( public_address_string ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
        }
        rocketnet_error( "server transport create: public_address_string is NULL" );
        return NULL;
    }

    // the lifecycle state is read once here, before any allocation or socket;
    // a create before rocketnet_init is refused, logged and counted

    if ( !rocketnet_library_initialized() )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_NOT_INITIALIZED;
        }
        rocketnet_error( "rocketnet_server_transport_create: rocketnet_init must be called first" );
        rocketnet_api_misuse_record();
        return NULL;
    }

    if ( !rocketnet_server_transport_resolve( public_address_string, config, resolved_memory_budget, &resolved, status ) )
    {
        return NULL;
    }

    config = &resolved.config;
    rocketnet_address_t public_address = resolved.public_address;   // a port 0 takes the granted bind port below
    rocketnet_address_t second_address = resolved.second_address;
    rocketnet_address_t bind_address = resolved.bind_address;          // the granted open writes the bound port
    const int serves_both = resolved.serves_both;
    const int max_clients = resolved.max_clients;

    struct rocketnet_server_transport_t * server_transport = rocketnet_server_transport_allocate( context, max_clients );
    if ( !server_transport )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        }
        rocketnet_error( "server transport create: could not allocate server transport" );
        return NULL;
    }

    server_transport->context = context;
    server_transport->time = time;
    server_transport->raw_time = time;                  // the seed is the clock's origin, not a step
    server_transport->time_offset = 0.0;
    // the token clock's epoch: configured, or 0 until the first update reads
    // the wall clock once (rocketnet_server_transport_update)
    server_transport->token_epoch_unix_seconds = config->token_epoch_unix_seconds;

    const int default_send_capacity = config->max_clients > ROCKETNET_SERVER_MIN_SEND_PACKETS ? config->max_clients : ROCKETNET_SERVER_MIN_SEND_PACKETS;
    const int test_send_capacity = rocketnet_server_test_create_send_capacity <= server_transport->payload_ring.size ? rocketnet_server_test_create_send_capacity : 0;
    server_transport->send_capacity = test_send_capacity > 0 ? test_send_capacity : default_send_capacity;
    server_transport->disconnect_packets_per_update = config->disconnect_packets_per_update;
    server_transport->disconnect_seals_left = server_transport->disconnect_packets_per_update;

    // no budget is open until the first update or send_packets opens one;
    // until then the game's packets are unbudgeted, as before the reserve
    server_transport->send_queues = 1;
    for ( int queue = 0; queue < ROCKETNET_SERVER_MAX_SEND_QUEUES; ++queue )
    {
        server_transport->fill_budget[queue] = UINT64_MAX;
        server_transport->fill_claims[queue] = 0;
        server_transport->send_reserve[queue] = 0;
    }
    server_transport->disconnect_update_prepared = 0;
#if ROCKETNET_DEV
    rocketnet_server_test_create_send_capacity = 0;
#endif // #if ROCKETNET_DEV

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
        {
            *status = ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION;
        }
        rocketnet_error( "development keys in a production build (ROCKETNET_DEV=0); refusing to start" );
        rocketnet_server_transport_abort_create( server_transport );
        return NULL;
#endif // #if ROCKETNET_DEV
    }

    const int granted_transport = rocketnet_server_transport_open_in_order( server_transport, context, &resolved, &bind_address, status );
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
    {
        public_address.port = bind_address.port;
    }
    if ( serves_both && second_address.port == 0 )
    {
        second_address.port = bind_address.port;
    }

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
            {
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
            }
            rocketnet_server_transport_abort_create( server_transport );
            return NULL;
        }

        if ( !rocketnet_platform_mutex_create( &server_transport->link_simulator_mutex ) )
        {
            if ( status )
            {
                *status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;
            }
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
            {
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
            }
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
        // IPv4 as well, so it also advertises its IPv4 wildcard; a token for
        // the IPv4 loopback is accepted only when 127.0.0.1 is advertised too

        int wildcard = 1;
        for ( int i = 0; i < 8 && wildcard; i++ )
        {
            if ( public_address.data.ipv6[i] != 0 )
            {
                wildcard = 0;
            }
        }
        if ( wildcard )
        {
            memset( &protocol->advertised_ipv4, 0, sizeof( protocol->advertised_ipv4 ) );
            protocol->advertised_ipv4.type = ROCKETNET_ADDRESS_IPV4;
            protocol->advertised_ipv4.port = public_address.port;
        }
    }

    randombytes_buf( protocol->cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    randombytes_buf( protocol->cookie_reply_source_key, ROCKETNET_SERVER_COOKIE_REPLY_KEY_BYTES );
#if ROCKETNET_DEV
    rocketnet_dev_seed_hash_key( protocol->cookie_reply_source_key, ROCKETNET_SERVER_COOKIE_REPLY_KEY_BYTES, ROCKETNET_DEV_HASH_KEY_COOKIE_REPLY_SOURCES );
#endif // #if ROCKETNET_DEV
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

    // the deferred ack apply (rocketnet_server_acks_t): its queue, the
    // per-client id segments and claims
    const bool acks_defer_failed = protocol->acks && !rocketnet_server_acks_defer( protocol->acks );
#if ROCKETNET_DEV
    // the seam keeps the storage (the memory plan stays the transport's) and
    // turns only the deferral off
    if ( rocketnet_server_test_create_inline_acks )
    {
        rocketnet_server_test_create_inline_acks = false;
        if ( protocol->acks )
        {
            protocol->acks->deferred = 0;
        }
    }
#endif // #if ROCKETNET_DEV

    // the connection ids (standard/MESSAGES.md m1.4 rule 5): one per slot, 0 while
    // free, and one per acked batch entry, sized as the acked batch is; and
    // one end reason per slot, NONE until a connection in it ends

    protocol->connection_ids = (uint64_t *) rocketnet_malloc( context, (size_t) max_clients * sizeof( uint64_t ) );
    protocol->acked_connection_ids = (uint64_t *) rocketnet_malloc( context, (size_t) max_clients * ROCKETNET_SENT_PACKET_BUFFER_SIZE * sizeof( uint64_t ) );
    protocol->end_reasons = (int *) rocketnet_malloc( context, (size_t) max_clients * sizeof( int ) );
    server_transport->client_wire_totals = (struct rocketnet_server_wire_totals_t *) rocketnet_malloc( context, (size_t) max_clients * sizeof( struct rocketnet_server_wire_totals_t ) );
    server_transport->client_wire_totals_capacity = max_clients;
    if ( protocol->connection_ids )
    {
        memset( protocol->connection_ids, 0, (size_t) max_clients * sizeof( uint64_t ) );
    }
    if ( protocol->end_reasons )
    {
        memset( protocol->end_reasons, 0, (size_t) max_clients * sizeof( int ) );     // ROCKETNET_DISCONNECT_REASON_NONE
    }
    if ( server_transport->client_wire_totals )
    {
        memset( server_transport->client_wire_totals, 0, (size_t) max_clients * sizeof( struct rocketnet_server_wire_totals_t ) );
    }

    protocol->counters = &server_transport->counters;
    rocketnet_fill_phase_init( &server_transport->fill_phase, max_clients,
                               &server_transport->counters.values[ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP],
                               &server_transport->counters.values[ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS] );

    // one thread a client is the rule of a game that sends from game threads:
    // it is armed by that game's first call that names a client
    // (sdk/rocketnet_api.cpp), and a game that makes today's calls only is
    // never refused for it
    rocketnet_fill_phase_arm( &server_transport->fill_phase, false );

    if ( server_transport->io.operations->replay_mark )
    {
        server_transport->replay_marks = rocketnet_server_replay_marks_create( context, max_clients );
    }

    if ( !protocol->encryption_map || !protocol->used_token_history || !protocol->client_slots || !protocol->acks || acks_defer_failed || !protocol->connection_ids || !protocol->acked_connection_ids || !protocol->end_reasons || !server_transport->client_wire_totals || ( server_transport->io.operations->replay_mark && !server_transport->replay_marks ) )
    {
        if ( status )
        {
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        }
        rocketnet_error( "server transport create: could not allocate protocol storage" );
        rocketnet_server_transport_abort_create( server_transport );
        return NULL;
    }

    if ( status )
    {
        *status = ROCKETNET_STATUS_OK;
    }
    return server_transport;
}

// resolved.config carries the server_secret_key the protocol copies: it is
// wiped on every exit of the create, failed or created

static struct rocketnet_server_transport_t * rocketnet_server_transport_create_impl( void * context,
                                                                                     const char * public_address_string,
                                                                                     const struct rocketnet_server_transport_config_t * config,
                                                                                     double time, int * status,
                                                                                     const uint64_t * resolved_memory_budget )
{
    struct rocketnet_server_transport_resolved_t resolved;
    struct rocketnet_server_transport_t * server_transport = rocketnet_server_transport_create_staged( context, public_address_string, config, time, status, resolved_memory_budget, resolved );
    sodium_memzero( &resolved, sizeof( resolved ) );
    ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_SEEN( resolved.config.server_secret_key, ROCKETNET_SERVER_TRANSPORT_CONFIG_STAGING_CREATE );
    return server_transport;
}

#if ROCKETNET_DEV
int rocketnet_server_transport_secret_spans_for_test( const void * object, size_t * offsets, size_t * bytes, int max )
{
    const rocketnet_server_transport_t * transport = (const rocketnet_server_transport_t *) object;
    if ( !transport )
    {
        return 0;
    }
    const size_t protocol = offsetof( rocketnet_server_transport_t, protocol );
    const size_t spans[4][2] = {
        { protocol + offsetof( rocketnet_server_protocol_t, server_secret_key ), ROCKETNET_KEY_BYTES },
        { protocol + offsetof( rocketnet_server_protocol_t, cookie_key ), ROCKETNET_COOKIE_KEY_BYTES },
        { protocol + offsetof( rocketnet_server_protocol_t, previous_cookie_key ), ROCKETNET_COOKIE_KEY_BYTES },
        { (size_t) ( (const uint8_t *) transport->disconnect_burst_keys - (const uint8_t *) transport ), (size_t) transport->disconnect_burst_capacity * ROCKETNET_KEY_BYTES },
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
    {
        return;
    }
    rocketnet_assert( server_transport );

    rocketnet_assert( rocketnet_server_transport_stopped( server_transport ) );
    if ( !rocketnet_server_transport_stopped( server_transport ) )
    {
        rocketnet_error( "rocketnet_server_transport_destroy: the transport is not stopped; call rocketnet_server_transport_stop and poll rocketnet_server_transport_stopped before destroy" );
        return;
    }

    // the server's keys: its secret key and both cookie keys. The session
    // keys are wiped with the encryption map below; nothing else here is a
    // secret, and the OS zero-fills every page before another process gets it

    sodium_memzero( server_transport->protocol.server_secret_key, sizeof( server_transport->protocol.server_secret_key ) );
    sodium_memzero( server_transport->protocol.cookie_key, sizeof( server_transport->protocol.cookie_key ) );
    sodium_memzero( server_transport->protocol.previous_cookie_key, sizeof( server_transport->protocol.previous_cookie_key ) );
    if ( server_transport->disconnect_burst_keys )
    {
        sodium_memzero( server_transport->disconnect_burst_keys, (size_t) server_transport->disconnect_burst_capacity * ROCKETNET_KEY_BYTES );
    }

    if ( server_transport->protocol.encryption_map )
    {
        rocketnet_encryption_map_destroy( server_transport->protocol.encryption_map );
    }
    if ( server_transport->protocol.used_token_history )
    {
        rocketnet_used_token_history_destroy( server_transport->protocol.used_token_history );
    }
    if ( server_transport->protocol.client_slots )
    {
        rocketnet_client_slots_destroy( server_transport->protocol.client_slots );
    }
    if ( server_transport->protocol.acks )
    {
        rocketnet_server_acks_destroy( server_transport->protocol.acks );
    }
    if ( server_transport->protocol.connection_ids )
    {
        rocketnet_free( server_transport->context, server_transport->protocol.connection_ids );
    }
    if ( server_transport->protocol.acked_connection_ids )
    {
        rocketnet_free( server_transport->context, server_transport->protocol.acked_connection_ids );
    }
    if ( server_transport->protocol.end_reasons )
    {
        rocketnet_free( server_transport->context, server_transport->protocol.end_reasons );
    }
    if ( server_transport->client_wire_totals )
    {
        rocketnet_free( server_transport->context, server_transport->client_wire_totals );
    }
    if ( server_transport->replay_marks )
    {
        rocketnet_server_replay_marks_destroy( server_transport->replay_marks );
    }
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

static void rocketnet_server_transport_flush( struct rocketnet_server_transport_t * server_transport, bool shutdown );

// proofs/tla/ServerStopSpread.tla SealOne, Flush and RequestIoStop: one stop
// step. The waiting bursts are sealed within the transport's send room,
// what was sealed is flushed, and once nothing waits and the transport
// reports every send it was handed finished, the I/O is told to stop, so no
// flushed send is cut off. Bounded work and no wait: a burst the room
// cannot take, or a send still in flight, waits for the next step.
static void rocketnet_server_transport_stop_step( struct rocketnet_server_transport_t * server_transport )
{
    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_STOPPING || server_transport->io_stop_requested )
    {
        return;
    }

    if ( server_transport->disconnect_burst_count > 0 )
    {
        rocketnet_server_transport_drain_disconnect_bursts( server_transport, rocketnet_server_transport_io_send_room_total( server_transport ), true );
    }

    rocketnet_server_transport_flush( server_transport, true );
    rocketnet_server_link_drain( server_transport, true );

    // proofs/tla/ServerStopSpread.tla AbandonNoRoom and
    // proofs/tla/RNDisconnectBurstRing.tla DropForRoom: no send room and
    // nothing staged or in flight that could free any, so each waiting burst
    // is abandoned as an unreachable one is (popped, head on by count) and
    // counted as a start refused for a full staging (a complete one leaves
    // uncounted); the cursor is reset. The room is read again after
    // send_idle, which may publish what was staged and so free room. With
    // several send queues the room is every queue's (io_send_room_total):
    // no burst is abandoned while any queue could take a frame.
    if ( server_transport->disconnect_burst_count > 0 && rocketnet_server_transport_io_send_room_total( server_transport ) == 0 &&
         rocketnet_server_transport_io_send_idle( server_transport ) && rocketnet_server_transport_io_send_room_total( server_transport ) == 0 )
    {
        while ( server_transport->disconnect_burst_count > 0 )
        {
            if ( server_transport->disconnect_bursts[server_transport->disconnect_burst_head].sealed < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS )
            {
                rocketnet_server_transport_io_count_refused_judged( server_transport, 0, true );
            }
            sodium_memzero( server_transport->disconnect_burst_keys[server_transport->disconnect_burst_head], ROCKETNET_KEY_BYTES );
            server_transport->disconnect_burst_head = ( server_transport->disconnect_burst_head + 1 ) % server_transport->disconnect_burst_capacity;
            server_transport->disconnect_burst_count--;
        }
        server_transport->disconnect_burst_cursor = 0;
    }

    const bool link_empty = !server_transport->link_simulator ||
                            !rocketnet_network_simulator_holds_any( server_transport->link_simulator );
    if ( server_transport->disconnect_burst_count == 0 && link_empty && rocketnet_server_transport_io_send_idle( server_transport ) )
    {
        server_transport->io_stop_requested = true;
        rocketnet_server_transport_io_stop( &server_transport->io );
    }
}

// proofs/tla/ServerStopSpread.tla Stop: the transport is STOPPING on return
// and this call is the first stop step.
void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) || server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        return;
    }
    rocketnet_fill_phase_main_scope_t phase( &server_transport->fill_phase );
    if ( !phase.entered )
    {
        return;
    }
    server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPING;

    // Retained buffered sends must learn of shutdown before send_idle can
    // become true. Admission of the remaining disconnect batches stays open.
    if ( server_transport->io.operations->begin_shutdown )
    {
        server_transport->io.operations->begin_shutdown( server_transport->io.instance );
    }

    // proofs/tla/RNDriverSendStaging.tla ResetLeases: stop bumps the lease epoch.
    // A finish of a packet started before this reset is caller misuse.

    server_transport->packet_leases.reset();
    rocketnet_server_transport_stop_step( server_transport );
}

// proofs/tla/ServerStopSpread.tla IoStopped: stopped only once the last burst
// was handed over and the workers have exited. While bursts wait, the poll
// is the next stop step, as the I/O's own stopped may service ready
// completions; the transport is never a const object, only this view of it.
bool rocketnet_server_transport_stopped( const struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) || server_transport->state == ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        return false;
    }
    rocketnet_server_transport_stop_step( const_cast<struct rocketnet_server_transport_t *>( server_transport ) );
    return server_transport->io_stop_requested && rocketnet_server_transport_io_stopped( &server_transport->io );
}

// A failed create cannot return a live partially constructed object. This
// private construction-only unwind is allowed to wait; successful objects use
// the explicit request/completion/destroy contract above.
static void rocketnet_server_transport_abort_create( struct rocketnet_server_transport_t * transport )
{
    transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPING;
    transport->io_stop_requested = true;
    rocketnet_server_transport_io_stop( &transport->io );
    while ( !rocketnet_server_transport_stopped( transport ) )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_server_transport_destroy( transport );
}

int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    if ( server_transport->state == ROCKETNET_SERVER_TRANSPORT_STOPPING && rocketnet_server_transport_stopped( server_transport ) )
    {
        return ROCKETNET_SERVER_TRANSPORT_STOPPED;
    }
    return server_transport->state;
}

bool rocketnet_server_transport_advertised_address( struct rocketnet_server_transport_t * server_transport, int family, struct rocketnet_address_t * address )
{
    if ( !rocketnet_api_null_guard( server_transport ) || !rocketnet_api_null_guard( address ) )
    {
        return false;
    }
    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        return false;
    }
    const struct rocketnet_address_t * advertised = family == ROCKETNET_ADDRESS_IPV4 ? &server_transport->protocol.advertised_ipv4 : family == ROCKETNET_ADDRESS_IPV6 ? &server_transport->protocol.advertised_ipv6
                                                                                                                                                                      : NULL;
    if ( !advertised || advertised->type != family )
    {
        return false;
    }
    *address = *advertised;

    return true;
}

uint16_t rocketnet_server_transport_port( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return server_transport->bind_address.port;
}

// the granted-tier report: the transport that won the ladder at create, by
// name -- what performance claims and operator errands attach to

const char * rocketnet_server_transport_io_name( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return NULL;
    }
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
    {
        return rocketnet_portable_server_test_socket( server_transport->io.instance );
    }
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
            {
                continue;
            }

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
    {
        return;
    }
    rocketnet_assert( server_transport );
    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        return;
    }
    server_transport->disconnect_seals_left = server_transport->disconnect_packets_per_update;
    server_transport->disconnect_update_prepared = 1;
}

// proofs/tla/RNDriverSendStaging.tla Update
// One received datagram, once counted: its shape and mapping, the protocol,
// the response copied into a send frame, and the payload into this update's
// batch in arrival order.
// view_index names the packet in a staged transport's receive view, -1 on
// the batch path.
// The unit seam: the payload batch's capacity for this update's receive,
// below the batch the server's max_clients sizes, so a case can fill it with
// a few packets. 0 keeps the full batch.
#if ROCKETNET_DEV
static int rocketnet_server_test_payload_batch_capacity = 0;

void rocketnet_server_transport_set_payload_batch_capacity_for_test( int payloads )
{
    rocketnet_server_test_payload_batch_capacity =
        payloads > 0 && payloads <= rocketnet_server_receive_packets_per_update( ROCKETNET_MAX_CLIENTS ) ? payloads : 0;
}
#else // #if ROCKETNET_DEV
static const int rocketnet_server_test_payload_batch_capacity = 0;
#endif // #if ROCKETNET_DEV

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

    const int batch_capacity = rocketnet_server_test_payload_batch_capacity > 0 &&
                                       rocketnet_server_test_payload_batch_capacity < server_transport->payloads.capacity
                                   ? rocketnet_server_test_payload_batch_capacity
                                   : server_transport->payloads.capacity;
    const bool payload_slot_free = server_transport->payloads.num_payloads < batch_capacity;
    uint8_t payload_overflow[ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES];
    uint8_t * opened = payload_slot_free ? server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_SERVER_PAYLOAD_SLOT_BYTES : payload_overflow;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    int payload_client_index = -1;

    struct rocketnet_server_frame_t frame
    {
    };

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    // proofs/tla/SessionAcks.tla RecvData: the replay check, then the payload opens

    const int outcome = rocketnet_server_protocol_process_received( &server_transport->protocol, from, mapping_index,
                                                                    packet_data, packet_bytes,
                                                                    current_timestamp, current_time,
                                                                    response, &response_bytes,
                                                                    opened, ROCKETNET_RECEIVE_PAYLOAD_BYTES,
                                                                    &payload, &payload_bytes, &payload_client_index );

    // a send frame is reserved only for a response, so a packet with none
    // takes no send staging; from is the receive view's own entry, which the
    // xdp transport reads as the frame the response answers
    if ( response_bytes > 0 )
    {
        rocketnet_server_reserve_send_frame( server_transport, from, &frame, false, false );
    }

    // a packet that opened under a connection's keys teaches a staged
    // transport its link-layer source (the xdp reply-MAC rule, TRANSPORTS.md);
    // a dropped or unauthenticated packet (a cookie or a challenge answer)
    // teaches nothing
    if ( view_index >= 0 && ( outcome == ROCKETNET_SERVER_PROCESS_ACCEPTED || outcome == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE ) &&
         server_transport->io.operations->client_accepted )
    {
        server_transport->io.operations->client_accepted( server_transport->io.instance, view_index );
    }

    rocketnet_assert( shape_valid || ( response_bytes == 0 && payload_bytes == 0 ) );

    frame.client_index = rocketnet_server_wire_client_after( server_transport, wire_client_before, mapping_index, outcome );

    if ( frame.data )
    {
        memcpy( frame.data + ROCKETNET_FRAME_WIRE_OFFSET, response, (size_t) response_bytes );
        rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
    }
    else if ( response_bytes > 0 )
    {
        rocketnet_server_transport_io_count_refused( server_transport, from, frame.packet_id );
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
    else if ( payload && payload_bytes > 0 )
    {
        // the batch holds one entry per packet the update's drain takes, at
        // most the payload batch's capacity, so this
        // stays zero while that bound holds
        rocketnet_spam( "server dropped payload. receive batch full" );
        server_transport->counters.values[ROCKETNET_COUNTER_PAYLOAD_BATCH_FULL]++;
    }
}

// proofs/tla/RNAckApplyPhase.tla: update is three calls. update_receive
// (main) reads and processes the received packets, queuing each ack block
// on its client; update_client_acks (any thread, between the two) applies
// one client's queued blocks; update_finish (main) applies every client
// nobody took, publishes the acked batch and does the rest of update.
// update is the three in a row with no client taken: the same result.

void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport, double time )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    rocketnet_fill_phase_main_scope_t phase( &server_transport->fill_phase );
    if ( !phase.entered )
    {
        return;
    }
    rocketnet_server_transport_update_receive( server_transport, time );
    rocketnet_server_transport_update_finish( server_transport );
}

int rocketnet_server_transport_update_client_acks( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }

    // outside update_receive .. update_finish there is nothing to apply: a
    // call there is misuse, counted, and does nothing (the read of
    // update_open races nothing: main writes it only outside the open phase)

    if ( !server_transport->update_open )
    {
        // a transport that is stopping or stopped opens no update
        // (rocketnet_server_transport_update_receive returns before it
        // receives), so a game that makes its three calls every frame finds
        // nothing to apply here: not misuse

        if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        {
            return 0;
        }

        rocketnet_api_misuse_record();
        rocketnet_assert( server_transport->update_open && "update_client_acks runs between update_receive and update_finish" );
        return 0;
    }

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;
    if ( client_index < 0 || client_index >= acks->max_clients )
    {
        rocketnet_api_misuse_record();
        rocketnet_assert( client_index >= 0 && client_index < acks->max_clients );
        return 0;
    }

    return rocketnet_server_acks_apply_client( acks, client_index );
}

void rocketnet_server_transport_update_receive( struct rocketnet_server_transport_t * server_transport, double time )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    rocketnet_assert( server_transport );

    // a receive with the last one still open: that update is finished first,
    // and the skipped finish is the caller's misuse

    if ( server_transport->update_open )
    {
        rocketnet_api_misuse_record();
        rocketnet_assert( !server_transport->update_open && "update_finish before the next update_receive" );
        rocketnet_server_transport_update_finish( server_transport );
    }

    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        if ( rocketnet_server_transport_stopped( server_transport ) )
        {
            server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPED;
        }
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
    {
        server_transport->token_epoch_unix_seconds = rocketnet_platform_token_epoch( 0, current_time );
    }

#if ROCKETNET_DEV
    const uint64_t current_timestamp = server_transport->test_timestamp ? server_transport->test_timestamp : rocketnet_platform_token_clock( server_transport->token_epoch_unix_seconds, current_time );
#else // #if ROCKETNET_DEV
    const uint64_t current_timestamp = rocketnet_platform_token_clock( server_transport->token_epoch_unix_seconds, current_time );
#endif // #if ROCKETNET_DEV

    server_transport->payloads.num_payloads = 0;

    // proofs/tla/SessionAcks.tla Tick: the acked-packet batch drains per
    // update: what the blocks processed below acknowledge lands here, valid
    // until the next update (rocketnet_server_acks_begin_receive empties it)

    rocketnet_server_acks_begin_receive( server_transport->protocol.acks, current_time );

    // receive and process, responses copied into send frames

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        struct rocketnet_server_socket_process_packets_t * received = server_transport->io.operations->receive( server_transport->io.instance );

        const int stats_index = server_transport->current_stats_index;

        // a missing view, a negative count or one past the view's capacity,
        // or a byte count that is negative or past the receive ceiling (a
        // staged transport drops larger datagrams itself) is a transport
        // fault, not a received packet

        int num_received = 0;
        if ( !received || received->num_packets < 0 )
        {
            server_transport->transport_faults++;
        }
        else if ( received->num_packets > received->capacity )
        {
            server_transport->transport_faults++;
            num_received = received->capacity;
        }
        else
        {
            num_received = received->num_packets;
        }

        for ( int i = 0; i < num_received; i++ )
        {
            if ( received->packet_bytes[i] < 0 || received->packet_bytes[i] > ROCKETNET_RECEIVE_CEILING )
            {
                server_transport->transport_faults++;
                if ( received->packet_bytes[i] > ROCKETNET_RECEIVE_CEILING )
                {
                    server_transport->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE]++;
                }
                continue;
            }

            server_transport->stats_frames[stats_index].packets_received++;
            server_transport->stats_frames[stats_index].bytes_received += (uint64_t) ( received->packet_bytes[i] + rocketnet_wire_header_bytes( received->from[i].type ) );

            rocketnet_server_receive_one( server_transport, &received->from[i], received->packet_data[i], received->packet_bytes[i], i, current_timestamp, current_time );
        }
    }
    else
    {
    // One read per update: a single receive_batch call takes whatever the
    // endpoint has queued, up to this update's packet budget, and each packet
    // is processed in turn; what is left past the budget stays in the
    // transport's queue for the next update. The budget is
    // rocketnet_server_receive_packets_per_update (16 * 1000 = 16000 at 1000
    // clients), which equals the payload-batch capacity, so all packets read
    // in one update fit the payload batch that feeds the application.

        struct rocketnet_server_receive_buffer_t * receive_buffer = &server_transport->receive_buffer;

        const int stats_index = server_transport->current_stats_index;
        const int batch_capacity = receive_buffer->capacity;

        int num_received = server_transport->io.operations->receive_batch( server_transport->io.instance,
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
                if ( packet_bytes > ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES )
                {
                    server_transport->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE]++;
                }
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
    }

    // proofs/tla/RNAckApplyPhase.tla Open: the queued clients go to whoever
    // takes them until update_finish

    rocketnet_server_acks_open( server_transport->protocol.acks );
    server_transport->update_open = 1;
}

static void rocketnet_server_transport_update_finish_body( struct rocketnet_server_transport_t * server_transport );

// sending from the game threads opens the send budget after every update,
// so the split update's finish opens it too
void rocketnet_server_transport_update_finish( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    rocketnet_server_transport_update_finish_body( server_transport );
    if ( server_transport->state == ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        rocketnet_server_transport_open_fill_budget( server_transport );
    }
}

static void rocketnet_server_transport_update_finish_body( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );

    // a finish with no receive open (a stopped transport's update, or a
    // second finish) has nothing to do

    if ( !server_transport->update_open )
    {
        return;
    }
    server_transport->update_open = 0;

    // proofs/tla/RNAckApplyPhase.tla Close: after the game's join, every
    // client nobody took applies here, and the batch is published in arrival
    // order

    rocketnet_server_acks_close( server_transport->protocol.acks, server_transport->protocol.acked_connection_ids,
                                 &server_transport->counters.values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE],
                                 &server_transport->counters.values[ROCKETNET_COUNTER_ACKED_BATCH_FULL] );

    const double current_time = server_transport->time;

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
    // proofs/tla/RNServerMaintenanceSweep.tla BeginSweep: the sweep starts at
    // the cursor; SweepDone: it ends when every slot has had its visit

    const int first_client = server_transport->maintenance_next_client;

    for ( int offset = 0; offset < max_clients; offset++ )
    {
        const int i = ( first_client + offset ) % max_clients;
        server_transport->maintenance_next_client = i;
        if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, i ) )
        {
            continue;
        }

        // a sent-stats scan that found no survivor (rocketnet_ack.cpp) is an
        // internal fault: the slot ends as a kick ends it, and its reason is
        // PROTOCOL_VIOLATION, as the message layer's sent index names one

        if ( acks->endpoints[i].internal_error )
        {
            rocketnet_server_transport_disconnect_client( server_transport, i );
            server_transport->protocol.end_reasons[i] = ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION;
            server_transport->maintenance_next_client = ( i + 1 ) % max_clients;
            continue;
        }

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

        // a full staging ends this update's pass at this client, which is
        // served first next update: no start is made that would be refused
        // (proofs/tla/ServerStopSpread.tla Room)

        if ( rocketnet_server_transport_io_send_room( server_transport ) <= 0 )
        {
            break;
        }

        const rocketnet_address_t * client_address = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, client_address, &frame );
        if ( !frame_data )
        {
            // proofs/tla/RNServerMaintenanceSweep.tla SweepStep: a full staging
            // ends the sweep with the cursor on this slot. A refusal with room
            // left (SEND_NO_LINK_ADDRESS, or SEND_FAILED for an address the
            // transport cannot carry) is this address's alone: its keep-alive
            // stays due, and the sweep serves the rest, as the drain does.

            if ( frame.packet_id != ROCKETNET_TRANSPORT_PACKET_UNREACHABLE && rocketnet_server_transport_io_refused_full( server_transport, client_address ) )
            {
                break;
            }
            server_transport->maintenance_next_client = ( i + 1 ) % max_clients;
            continue;
        }
        frame.client_index = i;

        int send_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server_transport->protocol, i, current_time, frame_data + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( send_bytes == 0 )
        {
            send_bytes = rocketnet_server_protocol_write_ack_if_due( &server_transport->protocol, i, current_time, frame_data + ROCKETNET_FRAME_WIRE_OFFSET );
        }
        if ( send_bytes > 0 )
        {
            rocketnet_server_commit_send_frame( server_transport, &frame, send_bytes );
        }
        else
        {
            rocketnet_server_abort_send_frame( server_transport, &frame );
        }
        server_transport->maintenance_next_client = ( i + 1 ) % max_clients;
    }

    // proofs/tla/ServerSession.tla SealWaiting: the public message-server
    // update opens the allowance before its pre-link kicks. A direct
    // transport update opens it here instead. Either way those disconnects
    // and this drain share exactly one allowance, and the drain seals no
    // more than the transport's send room (proofs/tla/ServerStopSpread.tla
    // SealOne).

    if ( !server_transport->disconnect_update_prepared )
    {
        server_transport->disconnect_seals_left = server_transport->disconnect_packets_per_update;
    }
    server_transport->disconnect_update_prepared = 0;
    if ( server_transport->disconnect_burst_count > 0 && server_transport->disconnect_seals_left > 0 )
    {
        const uint64_t before = server_transport->disconnect_seals;
        const int room = rocketnet_server_transport_io_send_room_total( server_transport );
        const int max_seals = room < server_transport->disconnect_seals_left ? room : server_transport->disconnect_seals_left;
        rocketnet_server_transport_drain_disconnect_bursts( server_transport, max_seals, false );
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
        rocketnet_server_fold_fill_lines( server_transport );
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
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return server_transport->payloads.num_payloads;
}

int rocketnet_server_transport_payloads( struct rocketnet_server_transport_t * server_transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( client_index ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( payload_data ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( payload_bytes ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( payload_data );
    rocketnet_assert( payload_bytes );

    *client_index = server_transport->payloads.client_index;
    *payload_data = server_transport->payloads.payload_data;
    *payload_bytes = server_transport->payloads.payload_bytes;

    return server_transport->payloads.num_payloads;
}

// standard/MESSAGES.md m1.4 rule 5: the connection id of each payload entry, parallel to
// the client index array above

int rocketnet_server_transport_payload_connection_ids( struct rocketnet_server_transport_t * server_transport, const uint64_t ** connection_id )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( connection_id ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( connection_id );

    *connection_id = server_transport->payloads.connection_id;

    return server_transport->payloads.num_payloads;
}

uint8_t * rocketnet_server_transport_payload( struct rocketnet_server_transport_t * server_transport, int payload_index, int * client_index, int * payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return NULL;
    }
    if ( !rocketnet_api_null_guard( client_index ) )
    {
        return NULL;
    }
    if ( !rocketnet_api_null_guard( payload_bytes ) )
    {
        return NULL;
    }
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

// proofs/tla/RNServerPayloadRingLayout.tla Acquire, on the calling thread's
// line: a slot of the line's chunk when it holds one (a compare-and-swap on
// the line). Otherwise, while the ring has ROCKETNET_SERVER_FILL_CHUNK free
// slots for every line (the guard), one fetch-add of a chunk: its first slot
// is this one and the rest, clipped at send_capacity, the line's chunk. A
// line another thread on it refilled first keeps that one: this whole chunk
// is marked empty here and the slot is taken from the line's chunk instead,
// so the slots a thread takes only rise, and the flush, which walks the ring
// in slot order, sends one client's packets in the order they were started
// (a slot of this chunk used beside the other thread's lower chunk would
// leave before the packets this thread starts next). Below the guard one
// fetch-add of one slot. A slot at or past send_capacity is refused, though
// the counter has moved past it. Main marks each line's unused slots empty
// before the flush reads the ring
// (rocketnet_server_payload_ring_return_chunks), so every slot below the
// counter is a frame or empty. false: the ring is full.
static bool rocketnet_server_payload_ring_take( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_fill_line_t * line, uint64_t * slot )
{
    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;
    const uint64_t capacity = (uint64_t) server_transport->send_capacity;
    volatile uint64_t * chunk = &line->ring;

    for ( ;; )
    {
        uint64_t word = 0;
        for ( ;; )
        {
            word = rocketnet_platform_atomic_load_u64( chunk );
            const uint64_t next = word & ROCKETNET_SERVER_FILL_HALF_MASK;
            const uint64_t end = word >> ROCKETNET_SERVER_FILL_HALF_BITS;
            if ( next >= end )
            {
                break;
            }
            if ( rocketnet_platform_atomic_cas_u64( chunk, word, word + 1 ) )
            {
                *slot = next;
                return true;
            }
        }

        const uint64_t size = ROCKETNET_SERVER_FILL_CHUNK;
        const uint64_t used = rocketnet_platform_atomic_load_u64( &ring->num_packets );
        if ( used > capacity || capacity - used < size * ROCKETNET_FILL_PHASE_FILL_LINES )
        {
            break;
        }

#if ROCKETNET_DEV
        if ( server_transport->steal_ring_chunk_for_test > 0 )
        {
            // another thread on this line claims a chunk, takes its first slot
            // and installs the rest, between this thread's look at the line
            // and its own install (the slot the other thread took is left empty)
            server_transport->steal_ring_chunk_for_test--;
            const uint64_t other = rocketnet_platform_atomic_add_u64( &ring->num_packets, size );
            ring->info[other].wire_bytes = 0;
            rocketnet_platform_atomic_store_u64( chunk, ( ( other + size ) << ROCKETNET_SERVER_FILL_HALF_BITS ) | ( other + 1 ) );
        }
#endif // #if ROCKETNET_DEV

        const uint64_t base = rocketnet_platform_atomic_add_u64( &ring->num_packets, size );
        if ( base >= capacity )
        {
            return false;
        }
        const uint64_t end = base + size < capacity ? base + size : capacity;
        if ( end == base + 1 || rocketnet_platform_atomic_cas_u64( chunk, word, ( end << ROCKETNET_SERVER_FILL_HALF_BITS ) | ( base + 1 ) ) )
        {
            *slot = base;
            return true;
        }
        for ( uint64_t i = base; i < end; ++i )
        {
            ring->info[i].wire_bytes = 0;
        }
    }

    const uint64_t one = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( one >= capacity )
    {
        return false;
    }
    *slot = one;
    return true;
}

// Main, with no fill running: every line's unused chunk slots marked empty,
// so the flush sends none of them, and the lines cleared.
static void rocketnet_server_payload_ring_return_chunks( struct rocketnet_server_transport_t * server_transport )
{
    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_FILL_LINES; ++i )
    {
        struct rocketnet_server_fill_line_t * line = &server_transport->fill_lines[i];
        const uint64_t next = line->ring & ROCKETNET_SERVER_FILL_HALF_MASK;
        const uint64_t end = line->ring >> ROCKETNET_SERVER_FILL_HALF_BITS;
        for ( uint64_t slot = next; slot < end; ++slot )
        {
            ring->info[slot].wire_bytes = 0;
        }
        line->ring = 0;
    }
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
    {
        return NULL;
    }
    if ( !rocketnet_api_null_guard( context ) )
    {
        return NULL;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    memset( context, 0, sizeof( struct rocketnet_packet_context_t ) );
    context->client_index = -1;                     // a refused start names no client: its finish or abort claims none

    rocketnet_fill_phase_fill_scope_t phase( &server_transport->fill_phase, client_index );
    if ( !phase.entered )
    {
        return NULL;
    }

    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        return NULL;
    }

    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
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
    {
        return NULL;
    }

    uint8_t * frame = NULL;

    struct rocketnet_server_fill_line_t * line = rocketnet_server_fill_line( server_transport );

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        // proofs/tla/RNSendReserveCalls.tla StartStaged, StartIoRefused and StartRefused: the whole call, as one thread sees it
        bool own_frame = false;
        const int fill_queue = rocketnet_server_transport_fill_claim( server_transport, line, client_index, &to, &own_frame );
        if ( fill_queue < 0 )
        {
            return NULL;
        }

        uint64_t packet_id;
        frame = rocketnet_server_transport_io_start_packet( server_transport, &to, &packet_id );
        if ( !frame )
        {
            rocketnet_server_transport_fill_refused( server_transport, line, fill_queue, own_frame );
            return NULL;
        }
        rocketnet_server_transport_fill_started( server_transport, line, fill_queue, own_frame );

        rocketnet_platform_atomic_add_u64( &line->staged, 1 );
        context->packet_id = packet_id;
    }
    else
    {
        struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

        uint64_t slot = 0;
        if ( !rocketnet_server_payload_ring_take( server_transport, line, &slot ) )
        {
            rocketnet_spam( "server payload ring full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return NULL;
        }

        ring->info[slot].to = to;
        ring->info[slot].client_index = client_index;
        ring->info[slot].connection_id = rocketnet_server_protocol_connection_id( &server_transport->protocol, client_index );
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

    // the caller's view is the payload, not the frame: the wire header in
    // front of it is the transport's own (layers.h, the send surface)

    return frame + context->payload_index;
}

static void rocketnet_server_transport_abort_packet_unchecked( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, uint64_t backend_id )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    if ( !context->frame )
    {
        return;
    }

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_transport_io_abort_packet( server_transport, backend_id, context->frame );
    }
    else
    {
        // proofs/tla/RNServerPayloadRingLayout.tla Abort: the slot from the frame pointer and the stride
        const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
        server_transport->payload_ring.info[slot].wire_bytes = 0;
    }

    context->frame = NULL;
}

uint8_t * rocketnet_server_transport_packet_payload( rocketnet_server_transport_t * transport, const rocketnet_packet_context_t * context, int * capacity )
{
    if ( !rocketnet_api_null_guard( transport ) )
    {
        return NULL;
    }
    if ( !rocketnet_api_null_guard( context ) )
    {
        return NULL;
    }
    if ( !rocketnet_api_null_guard( capacity ) )
    {
        return NULL;
    }
    *capacity = 0;
    rocketnet_packet_context_t trusted;
    uint64_t backend_id = 0;
    if ( !transport->packet_leases.resolve( *context, trusted, backend_id, false ) )
    {
        return NULL;
    }
    *capacity = trusted.payload_capacity;

    return trusted.frame + trusted.payload_index;
}

// proofs/tla/RNSendReserveCalls.tla Abort: the frame left the room when it was staged, so its claim is not given back.
void rocketnet_server_transport_abort_packet( rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    if ( !rocketnet_api_null_guard( context ) )
    {
        return;
    }
    rocketnet_fill_phase_fill_scope_t phase( &server_transport->fill_phase, context->client_index );
    if ( !phase.entered )
    {
        return;                                     // the lease stays for the next reset
    }
    rocketnet_packet_context_t trusted;
    uint64_t backend_id;
    if ( server_transport->packet_leases.take( *context, trusted, backend_id ) )
    {
        rocketnet_server_transport_abort_packet_unchecked( server_transport, &trusted, backend_id );
    }
    else if ( server_transport->packet_leases.retired( *context ) )
    {
        // a lease reset by update, send_packets or stop: refused, counted and
        // logged in every build, never asserted (rocketnet_server.h)
        rocketnet_api_misuse_record();
        rocketnet_error( "server abort packet refused: the lease was reset by update, send_packets or stop" );
    }
    else
    {
        // no start, a second abort, an abort after finish, or a modified or
        // wrong-owner context
        rocketnet_assert( false );
        rocketnet_api_misuse_record();
    }
    context->frame = NULL;
}


// proofs/tla/SessionAcks.tla BurnOne: a seal refusal after the sequence was taken leaves that sequence unsent.
static int rocketnet_server_transport_finish_packet_unchecked( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes, uint64_t backend_id )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    if ( !server_transport || !context )
    {
        return 0;
    }

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
        // a datagram is counted sent only once the link took it

        if ( rocketnet_server_commit_staged( server_transport, mapped_to, backend_id, &context->frame, wire_bytes ) )
        {
            rocketnet_server_fill_charge_sent( server_transport, rocketnet_server_fill_line( server_transport ), context->client_index, mapped_to->type, wire_bytes );
        }
    }
    else
    {
        // proofs/tla/RNServerPayloadRingLayout.tla Finish: the slot from the frame pointer and the stride
        const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
        server_transport->payload_ring.info[slot].wire_bytes = wire_bytes;
    }

    context->frame = NULL;

    return wire_bytes;
}

// proofs/tla/RNSendReserveCalls.tla Finish: the reserve's words stay as the start left them.
int rocketnet_server_transport_finish_packet( rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( context ) )
    {
        return 0;
    }
    rocketnet_fill_phase_fill_scope_t phase( &server_transport->fill_phase, context->client_index );
    if ( !phase.entered )
    {
        return 0;                                   // not sealed; the lease stays for the next reset
    }
    rocketnet_packet_context_t trusted;
    uint64_t backend_id;
    if ( !server_transport->packet_leases.take( *context, trusted, backend_id ) )
    {
        // proofs/tla/RNDriverSendStaging.tla FinishPacket: a lease reset by update,
        // send_packets or stop is caller misuse. Refuse in every build.
        // Return 0, count api_misuses, and log. Do not assert: a debug-only
        // trap compiled out of release is not a refusal the caller can see.
        // The frame is not sealed. Any other miss (no start, a second finish,
        // a finish after abort, a modified or wrong-owner context) asserts in
        // a debug build and is counted in every build.
        if ( server_transport->packet_leases.retired( *context ) )
        {
            rocketnet_api_misuse_record();
            rocketnet_error( "server finish packet refused: the lease was reset by update, send_packets or stop" );
            context->frame = NULL;
            return 0;
        }

        rocketnet_assert( false );
        rocketnet_api_misuse_record();
        context->frame = NULL;
        return 0;
    }

    const int result = rocketnet_server_transport_finish_packet_unchecked( server_transport, &trusted, payload_bytes, backend_id );
    context->frame = NULL;
    context->sequence = trusted.sequence;

    return result;
}

struct rocketnet_fill_phase_t * rocketnet_server_transport_fill_phase( struct rocketnet_server_transport_t * server_transport )
{
    return server_transport ? &server_transport->fill_phase : NULL;
}

// A drained payload frame charges its slot only while the connection that
// sealed it still holds that slot. update can free the slot and assign it
// again before send_packets; the server totals still take the frame.
static bool rocketnet_server_send_slot_holds( const struct rocketnet_server_transport_t * server_transport,
                                              int client_index, uint64_t connection_id )
{
    if ( connection_id == 0 || client_index < 0 )
    {
        return false;
    }
    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
    {
        return false;
    }
    return rocketnet_server_protocol_connection_id( &server_transport->protocol, client_index ) == connection_id;
}

// proofs/tla/SessionAcks.tla Flush: puts this update's staged packets on the wire in fill order.
// The flush of send_packets, and of each stop step (shutdown: the
// nonwaiting backend send, and a staged transport is flushed whatever was
// staged, so work it kept from an earlier flush is retried every step).
static void rocketnet_server_transport_flush( struct rocketnet_server_transport_t * server_transport, bool shutdown )
{
    const int stats_index = server_transport->current_stats_index;

    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        // fold the fill phase's payload sends into the current bucket. the fill
        // is closed before this call by the threading contract, so the lines
        // and the pending pair are quiescent and a plain read-and-clear is safe.

        rocketnet_server_fold_fill_lines( server_transport );
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

        if ( shutdown || rocketnet_server_transport_io_has( server_transport->io, ROCKETNET_TRANSPORT_REFLUSHES ) ||
             server_transport->send_staged != server_transport->send_flushed )
        {
            server_transport->send_flushed = server_transport->send_staged;
            rocketnet_server_transport_io_send_packets( server_transport );
        }

        rocketnet_server_link_flush( server_transport );
        return;
    }

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    // proofs/tla/RNServerPayloadRingLayout.tla Flush: the lines' unused chunk
    // slots marked empty, the counter clamped to send_capacity, every
    // finished slot below it sent, the counter reset
    rocketnet_server_payload_ring_return_chunks( server_transport );
    uint64_t num_packets = ring->num_packets;
    if ( num_packets > (uint64_t) server_transport->send_capacity )
    {
        num_packets = (uint64_t) server_transport->send_capacity;
    }

    struct rocketnet_platform_send_t * sends = ring->sends;
    int * send_client_index = ring->send_client_index;
    bool * accepted = ring->accepted;
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
        // is charged on its own result. the server totals always move. the
        // slot totals move only when that slot is still the connection that
        // sealed the frame (send ring info), not merely the same index.

        int send_i = 0;
        for ( uint64_t i = 0; i < num_packets && send_i < num_sends; i++ )
        {
            if ( ring->info[i].wire_bytes <= 0 )
            {
                continue;
            }
            if ( accepted[send_i] )
            {
                const bool same_connection = rocketnet_server_send_slot_holds( server_transport, send_client_index[send_i],
                                                                               ring->info[i].connection_id );
                const int charge_slot = same_connection ? send_client_index[send_i] : -1;
                rocketnet_server_charge_send_frame( server_transport, charge_slot, sends[send_i].to, sends[send_i].packet_bytes );
            }
            send_i++;
        }
    }

    ring->num_packets = 0;

    // The disconnect_client header promise: buffered control frames leave on
    // the next flush even when this fill phase sent no payload-ring packet.

    rocketnet_server_flush_control_buffer( server_transport, shutdown );

    rocketnet_server_link_flush( server_transport );
}

void rocketnet_server_transport_send_packets( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    rocketnet_fill_phase_main_scope_t phase( &server_transport->fill_phase );
    if ( !phase.entered )
    {
        return;
    }
    rocketnet_assert( server_transport );
    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
    {
        return;
    }

    // proofs/tla/RNDriverSendStaging.tla ResetLeases: send_packets bumps the lease
    // epoch before it flushes. Same bump as update and stop.

    server_transport->packet_leases.reset();

    rocketnet_server_transport_flush( server_transport, false );

    // proofs/tla/RNSendReserveCalls.tla Open: the budget opens again with the room the flush left

    rocketnet_server_transport_open_fill_budget( server_transport );
}

// the convenience: the same path underneath, plus one memcpy the caller chose

void rocketnet_server_transport_send_packet( struct rocketnet_server_transport_t * server_transport,
                                             int client_index,
                                             const uint8_t * payload_data,
                                             int payload_bytes )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    if ( !rocketnet_api_null_guard( payload_data ) )
    {
        return;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( payload_data );

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
    {
        return;
    }

    struct rocketnet_packet_context_t context;
    uint8_t * payload = rocketnet_server_transport_start_packet( server_transport, client_index, payload_bytes, &context );
    if ( !payload )
    {
        return;
    }

    memcpy( payload, payload_data, (size_t) payload_bytes );

    rocketnet_server_transport_finish_packet( server_transport, &context, payload_bytes );
}

// The slot ends here with no packet sent: it is released exactly as
// rocketnet_server_protocol_disconnect_client releases it.
static void rocketnet_server_transport_close_client_locally( struct rocketnet_server_transport_t * server_transport,
                                                             int client_index, bool missing_mapping )
{
    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    if ( missing_mapping )
    {
        rocketnet_server_protocol_note_missing_mapping( protocol );
    }
    protocol->counters->values[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT]++;
    protocol->counters->values[ROCKETNET_COUNTER_CLIENT_DISCONNECTED]++;
    rocketnet_server_connection_reset( protocol, client_index, ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
    rocketnet_debug( "server disconnected client %d", client_index );
}

// proofs/tla/RNDisconnectBurstRing.tla Defer: append within the ring capacity (NoOverwrite).
// proofs/tla/ServerStopSpread.tla Kick and Stop: the burst is appended at
// ( head + count ) % disconnect_burst_capacity.
static void rocketnet_server_transport_defer_disconnect_burst( struct rocketnet_server_transport_t * server_transport, int client_index,
                                                               int mapping_index, const rocketnet_address_t * client_address )
{
    rocketnet_assert( server_transport->disconnect_burst_count < server_transport->disconnect_burst_capacity );

    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( protocol->encryption_map, mapping_index );
    const int index = ( server_transport->disconnect_burst_head + server_transport->disconnect_burst_count ) % server_transport->disconnect_burst_capacity;
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

// proofs/tla/ServerSession.tla SealWaiting, each seal one SealStep, and
// proofs/tla/ServerStopSpread.tla SealOne: at most max_seals waiting
// disconnect packets are sealed into send frames, one per waiting burst per
// pass in ring order, so a burst's packets leave at consecutive sequences as
// the immediate burst's do. The caller's max_seals is within the transport's
// send room, summed over its queues; with several queues a burst whose own
// queue is full is refused for room, and the drain ends there. A burst whose last packet is
// sealed leaves the ring and its key is wiped. A start refused for its
// destination (no link-layer address, or one the socket cannot carry) with
// room left abandons that burst: it is marked complete, and the
// pass passes over a complete entry until it reaches the head, so no packet
// past a burst's sequences is ever sealed. A sealed write that fails
// abandons that burst the same way and the drain continues. Any other
// refusal ends the drain; what is left waits for the next update or stop step.
// proofs/tla/RNDisconnectBurstRing.tla SealStep: the cursor restarts at count
// (SealOnlyLive) and falls with each pop at the head (SealedFallFromHead).
// proofs/tla/RNDisconnectBurstRing.tla Abandon marks the burst complete when
// this destination is refused with room left, and PassOver steps the cursor
// past a complete entry without sealing it.
static void rocketnet_server_transport_drain_disconnect_bursts( struct rocketnet_server_transport_t * server_transport, int max_seals, bool shutdown )
{
    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    int committed = 0;
    while ( server_transport->disconnect_burst_count > 0 && committed < max_seals )
    {
        if ( server_transport->disconnect_burst_cursor >= server_transport->disconnect_burst_count )
        {
            server_transport->disconnect_burst_cursor = 0;
        }
        const int index = ( server_transport->disconnect_burst_head + server_transport->disconnect_burst_cursor ) % server_transport->disconnect_burst_capacity;
        struct rocketnet_server_disconnect_burst_t * burst = &server_transport->disconnect_bursts[index];

        if ( burst->sealed == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS )
        {
            server_transport->disconnect_burst_cursor++;
            continue;
        }

        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, &burst->to, &frame, shutdown, false );
        if ( !frame_data )
        {
            // a full staging (the destination's queue, with several): the
            // burst waits for the next update or stop step. The room is read
            // once, and the count and the choice both follow that reading.

            const bool full = frame.packet_id != ROCKETNET_TRANSPORT_PACKET_UNREACHABLE && rocketnet_server_transport_io_refused_full( server_transport, &burst->to );
            if ( rocketnet_server_transport_io_count_refused_judged( server_transport, frame.packet_id, full ) )
            {
                break;
            }

            // the transport refused this destination with room left (no
            // link-layer address, or an address its socket cannot carry): no
            // packet of this burst can leave

            burst->sealed = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
        }
        else
        {
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

#if ROCKETNET_DEV
            const int sealed_bytes = ( bytes > 0 && rocketnet_dev_disconnect_sealed_write_fails > 0 ) ? 0 : bytes;
#else
            const int sealed_bytes = bytes;
#endif
            if ( sealed_bytes <= 0 )
            {
                // the same entry would fail again on the next stop step:
                // abandon it, as an unreachable destination does, and keep
                // draining so the stop can finish
                rocketnet_server_abort_send_frame( server_transport, &frame );
                burst->sealed = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
#if ROCKETNET_DEV
                if ( rocketnet_dev_disconnect_sealed_write_fails > 0 )
                {
                    rocketnet_dev_disconnect_sealed_write_fails--;
                }
#endif
            }
            else
            {
                rocketnet_server_commit_send_frame( server_transport, &frame, sealed_bytes );
                committed++;
                server_transport->disconnect_seals++;
                burst->sealed++;
            }
        }

        server_transport->disconnect_burst_cursor++;

        // a complete burst leaves from the head, the cursor moving back with
        // it (one abandoned away from the head leaves once it reaches it)

        while ( server_transport->disconnect_burst_count > 0 &&
                server_transport->disconnect_bursts[server_transport->disconnect_burst_head].sealed == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS )
        {
            sodium_memzero( server_transport->disconnect_burst_keys[server_transport->disconnect_burst_head], ROCKETNET_KEY_BYTES );
            server_transport->disconnect_burst_head = ( server_transport->disconnect_burst_head + 1 ) % server_transport->disconnect_burst_capacity;
            server_transport->disconnect_burst_count--;
            if ( server_transport->disconnect_burst_cursor > 0 )
            {
                server_transport->disconnect_burst_cursor--;
            }
        }
    }

    if ( committed && rocketnet_server_transport_staged( server_transport ) )
    {
        server_transport->send_staged++;
    }
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

    if ( index < 0 || index >= server_transport->disconnect_burst_capacity || out_bytes < ROCKETNET_KEY_BYTES )
    {
        return 0;
    }
    memcpy( out, server_transport->disconnect_burst_keys[index], ROCKETNET_KEY_BYTES );
    if ( head )
    {
        *head = server_transport->disconnect_burst_head;
    }
    if ( count )
    {
        *count = server_transport->disconnect_burst_count;
    }
    return 1;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNDriverSendStaging.tla DisconnectClient
static void rocketnet_server_transport_disconnect_client_impl( struct rocketnet_server_transport_t * server_transport, int client_index, bool shutdown )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    rocketnet_assert( server_transport );

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
    {
        return;
    }

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

    // proofs/tla/ServerSession.tla DisconnectAll and SealWaiting, and
    // proofs/tla/ServerStopSpread.tla Kick and Stop: the slot ends now; its
    // burst is sealed now only while this update's allowance and the
    // transport's send room (with several send queues, the room of the
    // queue its address stages in too) hold the whole burst and no earlier
    // burst waits,
    // else it waits in the ring for the following updates. A running server
    // keeps at most max_clients waiting; the stop's bursts always
    // wait, in the half of the ring kept for them, for the stop steps.

    if ( shutdown || server_transport->disconnect_burst_count > 0 ||
         server_transport->disconnect_seals_left < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS ||
         rocketnet_server_transport_io_send_room( server_transport ) < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS ||
         ( server_transport->send_queues > 1 &&
           rocketnet_server_transport_queue_room( server_transport, rocketnet_server_transport_send_queue_of( server_transport, &client_address ) ) <
               ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS ) )
    {
        if ( shutdown || server_transport->disconnect_burst_count < server_transport->disconnect_burst_steady_cap )
        {
            rocketnet_server_transport_defer_disconnect_burst( server_transport, client_index, mapping_index, &client_address );
        }
        else
        {
            // Every retained entry owns an ended generation's key. Keep that
            // work and drop only this new burst's redundant notification
            // copies. Reset wipes the mapping key without ever copying or
            // counting a packet, and the local connection still ends now.
            server_transport->protocol.counters->values[ROCKETNET_COUNTER_DISCONNECT_BURST_DROPPED]++;
            rocketnet_server_transport_close_client_locally( server_transport, client_index, false );
            rocketnet_debug( "server dropped disconnect burst for client %d: %d bursts already waiting", client_index,
                             server_transport->disconnect_burst_count );
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
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, &client_address, &frame );
        if ( !frame_data )
        {
            break;
        }
        frame.client_index = client_index;
        memcpy( frame_data + ROCKETNET_FRAME_WIRE_OFFSET, burst + i * ROCKETNET_SEND_BUDGET, (size_t) burst_bytes );
        rocketnet_server_commit_send_frame( server_transport, &frame, burst_bytes );
        committed++;
    }

    // The disconnect_client header promise: committed staged frames leave on
    // the next send_packets flush even when there is no following update.

    if ( committed && rocketnet_server_transport_staged( server_transport ) )
    {
        server_transport->send_staged++;
    }
}

void rocketnet_server_transport_disconnect_client( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    rocketnet_fill_phase_main_scope_t phase( &server_transport->fill_phase );
    if ( !phase.entered )
    {
        return;
    }
    rocketnet_server_transport_disconnect_client_impl( server_transport, client_index, false );
}

void rocketnet_server_transport_disconnect_client_shutdown( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_server_transport_disconnect_client_impl( server_transport, client_index, true );
}

int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_num_connected( server_transport->protocol.client_slots );
}

int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index );
}

uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_client_id( server_transport->protocol.client_slots, client_index );
}

// the slot holds its copy of the token's user data from assign until release,
// which wipes it (rocketnet_client_slots_release_exact; proofs/tla/RNDestroyWipe.tla
// Remove). This read changes no state; a free slot reads zeros here and never
// reaches the slot accessor, which asserts connected

bool rocketnet_server_transport_client_user_data( struct rocketnet_server_transport_t * server_transport, int client_index, uint8_t * user_data )
{
    if ( !rocketnet_api_null_guard( server_transport ) || !rocketnet_api_null_guard( user_data ) )
    {
        return false;
    }
    rocketnet_assert( server_transport );

    const struct rocketnet_client_slots_t * slots = server_transport->protocol.client_slots;
    if ( !rocketnet_client_slots_connected( slots, client_index ) )
    {
        memset( user_data, 0, ROCKETNET_USER_DATA_BYTES );
        return false;
    }

    memcpy( user_data, rocketnet_client_slots_user_data( slots, client_index ), ROCKETNET_USER_DATA_BYTES );

    return true;
}

// standard/MESSAGES.md m1.4 rule 5: the connection that holds the slot now

uint64_t rocketnet_server_transport_client_connection_id( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return rocketnet_server_protocol_connection_id( &server_transport->protocol, client_index );
}

uint64_t rocketnet_server_transport_connections_assigned( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return server_transport->protocol.next_connection_id;
}

int rocketnet_server_transport_client_end_reason( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return ROCKETNET_DISCONNECT_REASON_NONE;
    }
    rocketnet_assert( server_transport );
    return rocketnet_server_protocol_end_reason( &server_transport->protocol, client_index );
}

double rocketnet_server_transport_client_rtt( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0.0;
    }
    rocketnet_assert( server_transport );
    return rocketnet_server_protocol_client_rtt( &server_transport->protocol, client_index );
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

static void rocketnet_server_transport_fold_receive_drops( struct rocketnet_server_transport_t * server_transport )
{
    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS] = {};
    server_transport->io.operations->receive_drop_totals( server_transport->io.instance, totals );

    for ( int drop = ROCKETNET_RECEIVE_ACCEPT + 1; drop < ROCKETNET_RECEIVE_NUM_DROPS; ++drop )
    {
        // cumulative from one transport: a total never falls
        rocketnet_assert( totals[drop] >= server_transport->transport_receive_drop_totals[drop] );
        const uint64_t growth = totals[drop] - server_transport->transport_receive_drop_totals[drop];
        server_transport->transport_receive_drop_totals[drop] = totals[drop];
        if ( !growth )
        {
            continue;
        }
        int counters[ROCKETNET_RECEIVE_DROP_MAX_COUNTERS];
        const int num_counters = rocketnet_receive_drop_counters( (enum rocketnet_receive_drop_t) drop, counters );
        for ( int i = 0; i < num_counters; ++i )
        {
            server_transport->counters.values[counters[i]] += growth;
        }
    }
}

// the xdp replay writer's counts (sdk/rocketnet_driver_builtin.cpp), declared
// here beside their one reader
bool rocketnet_server_transport_io_replay_counts( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                  uint64_t * commit_failed, uint64_t * greetings_held );

// the builtin transports' own counter rows (sdk/rocketnet_driver_builtin.cpp),
// declared here beside their one reader
bool rocketnet_server_transport_io_transport_counters( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                       uint64_t * values );

const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return NULL;
    }
    rocketnet_assert( server_transport );

    if ( server_transport->io.operations && server_transport->io.operations->prefilter_counters )
    {
        uint64_t prefilter[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
        server_transport->io.operations->prefilter_counters( server_transport->io.instance, prefilter );
        rocketnet_counters_apply_prefilter( &server_transport->counters, prefilter );
    }

    // SEND_FAILED is the transport's own cumulative count, assigned on read

    if ( server_transport->io.operations && server_transport->io.operations->send_failures )
    {
        server_transport->counters.values[ROCKETNET_COUNTER_SEND_FAILED] = server_transport->io.operations->send_failures( server_transport->io.instance );
    }

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

    // the drops the transport's receive workers made before banking
    // (sdk/rocketnet_receive_prefilter.h): each reason's growth lands on the
    // counters main's ladder adds for the same datagram

    if ( server_transport->io.operations && server_transport->io.operations->receive_drop_totals )
    {
        rocketnet_server_transport_fold_receive_drops( server_transport );
    }

    // XDP_RECEIVE_FRAME_DROPPED is the transport's own cumulative count,
    // assigned on read

    uint64_t frames_dropped = 0;
    if ( rocketnet_server_transport_io_receive_frames_dropped( server_transport->io.operations, server_transport->io.instance, &frames_dropped ) )
    {
        server_transport->counters.values[ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED] = frames_dropped;
    }

    // the xdp replay writer's counts are its own cumulative counts too,
    // assigned on read; both are written on this thread

    uint64_t replay_commit_failed = 0;
    uint64_t replay_greetings_held = 0;
    if ( rocketnet_server_transport_io_replay_counts( server_transport->io.operations, server_transport->io.instance, &replay_commit_failed,
                                                      &replay_greetings_held ) )
    {
        server_transport->counters.values[ROCKETNET_COUNTER_XDP_REPLAY_COMMIT_FAILED] = replay_commit_failed;
        server_transport->counters.values[ROCKETNET_COUNTER_XDP_REPLAY_GREETING_HELD] = replay_greetings_held;
    }

    // the gso, io_uring, iocp and rio transports' own counts are the public
    // rows GSO_PACKETS_SENT .. RIO_COMMITS, and the rio, iocp, gso and xdp
    // transports' refused pins *_PIN_REFUSALS, cumulative, assigned on read;
    // a transport writes its own rows and the rest reads 0, so every platform
    // has every row

    uint64_t transport_rows[ROCKETNET_NUM_COUNTERS] = {};
    if ( rocketnet_server_transport_io_transport_counters( server_transport->io.operations, server_transport->io.instance, transport_rows ) )
    {
        for ( int i = ROCKETNET_COUNTER_GSO_PACKETS_SENT; i <= ROCKETNET_COUNTER_RIO_COMMITS; ++i )
        {
            server_transport->counters.values[i] = transport_rows[i];
        }
        // each transport's pin refusals, the rows after the shared ones
        server_transport->counters.values[ROCKETNET_COUNTER_RIO_PIN_REFUSALS] = transport_rows[ROCKETNET_COUNTER_RIO_PIN_REFUSALS];
        server_transport->counters.values[ROCKETNET_COUNTER_IOCP_PIN_REFUSALS] = transport_rows[ROCKETNET_COUNTER_IOCP_PIN_REFUSALS];
        server_transport->counters.values[ROCKETNET_COUNTER_GSO_PIN_REFUSALS] = transport_rows[ROCKETNET_COUNTER_GSO_PIN_REFUSALS];
        server_transport->counters.values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] = transport_rows[ROCKETNET_COUNTER_XDP_PIN_REFUSALS];
    }

    return &server_transport->counters;
}

void rocketnet_server_transport_stats( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_transport_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    if ( !rocketnet_api_null_guard( stats ) )
    {
        return;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( stats );
    memset( stats, 0, sizeof( struct rocketnet_server_transport_stats_t ) );
    stats->packets_sent_per_second = server_transport->server_stats.packets_sent_per_second;
    stats->packets_received_per_second = server_transport->server_stats.packets_received_per_second;
    stats->bandwidth_sent_kbps = server_transport->server_stats.bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = server_transport->server_stats.bandwidth_received_kbps;
    uint64_t lines_packets = 0;
    uint64_t lines_bytes = 0;
    rocketnet_server_fill_lines_sent( server_transport, &lines_packets, &lines_bytes );
    stats->packets_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &server_transport->server_stats.packets_sent ) + lines_packets;
    stats->packets_received = server_transport->server_stats.packets_received;
    stats->bytes_sent = rocketnet_platform_atomic_load_u64( (volatile uint64_t *) &server_transport->server_stats.bytes_sent ) + lines_bytes;
    stats->bytes_received = server_transport->server_stats.bytes_received;
}

int rocketnet_server_transport_max_payload_bytes( struct rocketnet_server_transport_t * server_transport )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

int rocketnet_server_transport_acked_packets( struct rocketnet_server_transport_t * server_transport, const int ** client_index, const uint64_t ** packet_ids )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( client_index ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( packet_ids ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( packet_ids );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    *client_index = acks->acked_client_index;
    *packet_ids = acks->acked_packet_ids;

    return acks->num_acked;
}

// standard/MESSAGES.md m1.4 rule 5: the connection id of each acked packet id, parallel
// to the client index array above

int rocketnet_server_transport_acked_connection_ids( struct rocketnet_server_transport_t * server_transport, const uint64_t ** connection_id )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return 0;
    }
    if ( !rocketnet_api_null_guard( connection_id ) )
    {
        return 0;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( connection_id );

    *connection_id = server_transport->protocol.acked_connection_ids;

    return server_transport->protocol.acks->num_acked;
}

void rocketnet_server_transport_client_stats( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_server_transport_client_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( server_transport ) )
    {
        return;
    }
    if ( !rocketnet_api_null_guard( stats ) )
    {
        return;
    }
    rocketnet_assert( server_transport );
    rocketnet_assert( stats );

    memset( stats, 0, sizeof( struct rocketnet_server_transport_client_stats_t ) );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    if ( client_index < 0 || client_index >= acks->max_clients )
    {
        return;
    }

    // An empty slot reads as zeros, exactly as an index out of range does.
    // rocketnet_server_connection_reset returns the slot's ack endpoint to
    // its initial state when the connection ends; the slot's wire totals are
    // not part of that reset and are cleared when the slot is next assigned
    // (rocketnet_server_wire_totals_assign), so until then this check is what
    // keeps a departed client's totals from being read.

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
    {
        return;
    }

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

// rocketnet_conditions_t's seconds_since_heard for one slot: since the last
// packet its mapping accepted; -1 for a slot with no connected client, no
// mapping, or nothing heard yet.

double rocketnet_server_transport_heard_age( const struct rocketnet_server_transport_t * server_transport, int client_index )
{
    if ( !server_transport )
    {
        return -1.0;
    }
    const struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    if ( !protocol->acks || client_index < 0 || client_index >= protocol->acks->max_clients || !rocketnet_client_slots_connected( protocol->client_slots, client_index ) )
    {
        return -1.0;
    }
    const int mapping_index = rocketnet_client_slots_mapping_index( protocol->client_slots, client_index );
    if ( mapping_index < 0 )
    {
        return -1.0;
    }
    const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( protocol->encryption_map, mapping_index );
    return mapping->control && mapping->control->last_receive_time > 0.0 ? server_transport->time - mapping->control->last_receive_time : -1.0;
}

#if ROCKETNET_DEV
// The transport's half of the delivery capture (rocketnet_layers.h): plain
// copies of the slot's mapping and ack endpoint, on the main thread outside
// update/fill. A slot with no connected client or no mapping leaves the
// capture untouched, so it reads as not connected.
void rocketnet_server_transport_delivery_capture( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( server_transport ) || !rocketnet_api_null_guard( capture ) )
    {
        return;
    }
    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    if ( !protocol->acks || client_index < 0 || client_index >= protocol->acks->max_clients )
    {
        return;
    }
    if ( !rocketnet_client_slots_connected( protocol->client_slots, client_index ) )
    {
        return;
    }
    const int mapping_index = rocketnet_client_slots_mapping_index( protocol->client_slots, client_index );
    if ( mapping_index < 0 )
    {
        return;
    }
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
    {
        rocketnet_server_protocol_accept_payload( &transport->protocol, batch->client_index[k], batch->connection_id[k], batch->sequence[k] );
    }
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
    auto * transport = rocketnet_server_transport_allocate( NULL, ROCKETNET_DEFAULT_MAX_CLIENTS );
    if ( !transport )
    {
        return -1;
    }
    void * storage = transport;

    int flushes = 0;
    rocketnet_server_transport_io_operations_t operations = {};
    operations.flags = flags;
    operations.flush = rocketnet_server_transport_test_count_flush;
    transport->io.operations = &operations;
    transport->io.instance = &flushes;
    transport->send_staged = 1;
    rocketnet_server_transport_send_packets( transport );
    rocketnet_server_transport_send_packets( transport );
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
    {
        return -2;
    }

    return rocketnet_server_transport_test_reflush_flags( definition->backend->server->flags );
}

// A staged send must close the simulator's partial reverse window at the
// end of the frame, just as the portable send path does.
int rocketnet_server_transport_test_staged_simulator_flush()
{
    auto * transport = rocketnet_server_transport_allocate( NULL, ROCKETNET_DEFAULT_MAX_CLIENTS );
    if ( !transport )
    {
        return -1;
    }
    void * storage = transport;
    transport->link_simulator = rocketnet_network_simulator_create( NULL, 4, 1 );

    const bool mutex_created = transport->link_simulator &&
                               rocketnet_platform_mutex_create( &transport->link_simulator_mutex );
    int result = -1;

    if ( transport->link_simulator && mutex_created &&
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
    {
        rocketnet_platform_mutex_destroy( &transport->link_simulator_mutex );
    }
    if ( transport->link_simulator )
    {
        rocketnet_network_simulator_destroy( transport->link_simulator );
    }
    transport->~rocketnet_server_transport_t();
    rocketnet_free( NULL, storage );

    return result;
}
#endif // #if ROCKETNET_DEV && !defined( ROCKETNET_CONFORMANCE_DEFAULTS_ONLY )

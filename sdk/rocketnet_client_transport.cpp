#include <new>
#include "rocketnet_packet_lease.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_stats_window.h"
/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"
#include "rocketnet_message_object.h"
#include "rocketnet_client_io.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_protocol.h"
#include "rocketnet_memory.h"
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include "rocketnet_client_gso.h"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include "rocketnet_client_iocp.h"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include "rocketnet_config.h"
#include "rocketnet_network_simulator.h"

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include <atomic>
#include <sodium.h>

// sdk/rocketnet_client_protocol.cpp: the server address a token selects on a
// host with or without IPv6
void rocketnet_client_protocol_pick_server_address( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                    bool ipv6_available,
                                                    struct rocketnet_address_t * address );

#if ROCKETNET_DEV
// counts the client's calls of rocketnet_ack_endpoint_update_stats, for the
// functional test
std::atomic<unsigned long long> g_client_ack_stats_count{ 0 };
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
// The one clamp both transports share. A NaN or infinite time makes every
// timeout comparison false and freezes a transport; a backwards step rewinds
// its deadlines. Both become the last good time instead, counted, logged in
// every build and an assert in a debug build. Called once per update and once
// at client create, never per packet.

double rocketnet_clamp_update_time( double last_good, double time, bool * clamped )
{
    if ( clamped )
        *clamped = false;

    const bool usable = isfinite( time ) && time >= last_good;

    if ( !usable )
    {
        if ( clamped )
            *clamped = true;

        rocketnet_api_misuse_record();

        rocketnet_error( "update refused its time %f: it is not finite or goes back from %f; the last good time is kept", time, last_good );

        rocketnet_assert( usable );
        return last_good;
    }

    return time;
}
#endif // #if ROCKETNET_DEV

// The update clock the three owners share: the caller's raw time beside the
// clamped time they hand out. proofs/tla/RNUpdateClock.tla models it: ForwardStep, a
// forward raw step, BackwardStep, a backwards raw step, and NonFiniteStep, a
// non-finite sample. A forward step of d advances the clamped time by exactly
// d while the caller's clock is still behind, so a clock whose caller
// stepped backwards resumes with the caller instead of standing still until
// the caller's clock passes the last good time again (BASE's rule, which
// proofs/tla/MCRNUpdateClockBrokenStall.cfg reverses). Once the caller's clock has
// caught up, the clock is the caller's clock again. While no backwards step
// has happened the offset is exactly 0.0 and the returned time is the
// caller's time, bit for bit: the common path never adds to the last good
// time, so every caller that reads an exact time still reads it.

double rocketnet_update_clock_advance( double last_good, double * raw_time, double * time_offset, double time, bool * clamped )
{
    if ( clamped )
        *clamped = false;

    if ( !isfinite( time ) )
    {
        // a non-finite sample changes neither the raw time nor the offset

        if ( clamped )
            *clamped = true;

        rocketnet_api_misuse_record();

        rocketnet_error( "update refused its time %f: it is not finite; the last good time %f is kept", time, last_good );

        rocketnet_assert( isfinite( time ) );
        return last_good;
    }

    const double raw = *raw_time;

    if ( time < raw )
    {
        // a backwards step: the clamped time stands, the raw time and the offset record how far the caller's clock fell behind

        *time_offset += raw - time;
        *raw_time = time;

        if ( clamped )
            *clamped = true;

        rocketnet_api_misuse_record();

        rocketnet_error( "update refused its time %f: it goes back from %f; the last good time %f is kept", time, raw, last_good );

        rocketnet_assert( time >= raw );
        return last_good;
    }

    if ( time >= last_good )
    {
        // the caller's clock has caught up with the clamped one: the clock is
        // the caller's clock again, bit for bit, and the offset keeps the
        // distance the clock fell behind

        *raw_time = time;
        return time;
    }

    // the caller's clock is still behind: the elapsed time is preserved, so
    // the clamped time advances by exactly the caller's step and the clock
    // does not stand still for the whole step

    const double step = time - raw;

    *raw_time = time;

    return last_good + step;
}

// payloads are decrypted out of the receive frames into this storage, and
// handed to the caller as pointers into it, valid until the next update

// the client payload ring: the acquire/commit staging, filled from any
// number of game threads between updates and flushed by send_packets on
// the main thread. same discipline as the server's: one atomic add per
// slot, per-slot info alone on its cache line, roots 16-aligned.

#define ROCKETNET_CLIENT_PAYLOAD_RING_SIZE     256

struct alignas( 64 ) rocketnet_client_payload_slot_info_t
{
    int wire_bytes;
    uint8_t padding[64 - sizeof( int )]; // the rest of the cache line, explicit
};

static_assert( sizeof( rocketnet_client_payload_slot_info_t ) == 64, "one client slot info per cache line" );

struct rocketnet_client_payload_ring_t
{
    uint8_t padding_0[4096];

    volatile uint64_t num_packets;

    // a page, plus the rest of num_packets' cache line, so info starts on a cache line
    uint8_t padding_1[4096 + 64 - sizeof( uint64_t )];

    struct rocketnet_client_payload_slot_info_t info[ROCKETNET_CLIENT_PAYLOAD_RING_SIZE];

    uint8_t padding_2[4096];

    alignas( 16 ) uint8_t data[ROCKETNET_FRAME_STRIDE * ROCKETNET_CLIENT_PAYLOAD_RING_SIZE];

    uint8_t padding_3[4096];
};

static_assert( offsetof( rocketnet_client_payload_ring_t, info ) == 8192 + 64, "client ring info starts on a cache line with no implicit padding" );
static_assert( offsetof( rocketnet_client_payload_ring_t, data ) % 16 == 0 &&
                   offsetof( rocketnet_client_payload_ring_t, data ) == offsetof( rocketnet_client_payload_ring_t, padding_2 ) + 4096,
               "client ring data is 16-aligned with no implicit padding" );
static_assert( sizeof( rocketnet_client_payload_ring_t ) == offsetof( rocketnet_client_payload_ring_t, padding_3 ) + 4096,
               "client ring has no implicit tail padding" );

struct rocketnet_client_received_packets_t
{
    int num_packets;
    int receive_index;
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    int packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint64_t sequence[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE * ROCKETNET_NUM_CLIENT_PACKETS];
};

// The statistics ring advances by elapsed time, independently of update cadence,
// accumulation riding the walks the transport already makes, published
// per-second numbers summing only buckets younger than one second. Each bucket
// leads with its frame and time, beside its counters, so the per-update scan
// reads one bucket to find whether it is inside the last second and to sum it.
// The ring advances through the shared rocketnet_stats_window_advance
// (sdk/rocketnet_stats_window.h).

#define ROCKETNET_NUM_CLIENT_STATS_FRAMES 256

struct rocketnet_client_stats_frame_t
{
    uint64_t frame;
    double time;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

// The transport keeps only counters and rates. Quality fields belong to the
// getter result and are read directly from the protocol ACK endpoint.
struct rocketnet_client_transport_counters_t
{
    int packets_sent_per_second;
    int packets_received_per_second;
    int bandwidth_sent_kbps;
    int bandwidth_received_kbps;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

// BASE uses the same N-buffer ownership exchange as the I/O core
// (proofs/tla/RNBufferHandoffBase.tla). The
// worker always owns fill; update drains every READY batch within its packet
// budget and returns it in the same call. Contended returns stay caller-owned.
struct rocketnet_client_legacy_receive_state_t
{
    rocketnet_buffer_handoff_t<ROCKETNET_CLIENT_IO_NUM_BUFFERS> handoff;
    int fill;
    int returned[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    int returned_count = 0;
#if ROCKETNET_DEV
    unsigned test_busy_mask = 0;
#endif // #if ROCKETNET_DEV
};

/*
    The declaration manifest: the struct is declared from this list, so a
    member exists only as an entry here. The RESET/KEEP inventory in
    rocketnet_connection_reset.h is compared with it by name, and a member with
    no decision is named in test_connection_reset_inventory_covers_every_field.
*/
// clang-format off
#define ROCKETNET_CLIENT_TRANSPORT_LAYOUT_FIELDS( X )                       \
    /* the cache-line-aligned members lead, each a whole number of cache */ \
    /* lines, so the compiler inserts no alignment padding before any of them */                                     \
    X( packet_leases, rocketnet_packet_leases_t<1024> packet_leases ) /* three rings of one layout (proofs/tla/RNBufferHandoff.tla): game threads */ /* fill payload_ring[client_io.send_fill]. The BASE path never hands */ /* off, so it fills ring 0 only. */                                                                              \
    X( payload_ring, struct rocketnet_client_payload_ring_t payload_ring[ROCKETNET_CLIENT_IO_NUM_RINGS] )            \
    X( context, void * context )                                                                                     \
    X( bound_port, uint16_t bound_port )                                                                             \
    X( transport, int transport ) /* the granted transport (ROCKETNET_TRANSPORT_*), decided by the ladder at create */           \
    X( io, rocketnet_client_transport_io_t io )                                                                            \
    X( thread, rocketnet_platform_thread_t * thread )                                                                \
    X( mutex, rocketnet_platform_mutex_t mutex )                                                                     \
    X( stop_requested, bool stop_requested{false} ) \
    X( stopped, std::atomic<bool> stopped{false} ) \
    X( stop_burst, uint8_t stop_burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET] ) \
    X( stop_burst_count, int stop_burst_count{0} ) \
    X( stop_burst_pending, std::atomic<bool> stop_burst_pending{false} ) \
    X( stop_burst_bytes, int stop_burst_bytes{0} ) \
    X( stop_address, rocketnet_address_t stop_address ) \
    X( quit, std::atomic<bool> quit{0} )                                                                             \
    X( receive_buffer_index, rocketnet_client_legacy_receive_state_t receive_buffer_index )                          \
    X( received_packets, rocketnet_client_received_packets_t received_packets )                                      \
    X( receive_buffer, rocketnet_client_receive_buffer_t receive_buffer[ROCKETNET_CLIENT_IO_NUM_BUFFERS] )           \
    X( time, double time ) /* the game's clock, last seen */                                                         \
    X( raw_time, double raw_time ) /* the caller's clock, last seen, beside the clamped one */                     \
    X( time_offset, double time_offset ) /* the backwards offset, exactly 0.0 until the first step */                \
    X( frame, uint64_t frame ) /* advanced once per update; identifies initialized stats buckets */                  \
    X( last_state, int last_state )                                                                                  \
    X( config, struct rocketnet_client_transport_config_t config ) /* the packet id counter behind the acquire/commit surface: unique and */ /* monotonic, the join key the acked-packet batch surfaces. the ring */ /* slot is recovered from the frame pointer wherever it is needed. */ /* the I/O thread design (docs/CLIENT-IO.md): true when the granted */ /* transport has wait and wake; step_driven when no I/O thread runs and the */ /* caller calls rocketnet_client_io_step itself (unit tests) */ \
    X( io_thread, bool io_thread )                                                                                   \
    X( step_driven, bool step_driven ) /* an internal unit-test create: the caller runs the step */                  \
    X( step_in_update, bool step_in_update ) /* the public config field: update and send_packets run the step */     \
    X( client_io, rocketnet_client_io_t client_io )                                                                  \
    X( stats_counted, uint64_t stats_counted ) /* Filling ring slots already counted into the stats */               \
    X( time_callback, double (*time_callback)( void * context ) ) /* the receive stamp clock, read once at create */ \
    X( link_simulator, struct rocketnet_network_simulator_t * link_simulator )                                       \
    X( link_simulator_from, rocketnet_address_t link_simulator_from )                                                \
    X( counters, struct rocketnet_counters_t counters )                                                              \
    X( protocol, struct rocketnet_client_protocol_t protocol )                                                       \
    X( client_stats, struct rocketnet_client_transport_counters_t client_stats )                                      \
    X( stats_frames, struct rocketnet_client_stats_frame_t stats_frames[ROCKETNET_NUM_CLIENT_STATS_FRAMES] )         \
    X( current_stats_index, int current_stats_index )
// clang-format on

struct rocketnet_client_transport_t
{
#define ROCKETNET_CLIENT_TRANSPORT_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    ROCKETNET_CLIENT_TRANSPORT_LAYOUT_FIELDS( ROCKETNET_CLIENT_TRANSPORT_LAYOUT_FIELDS_DECLARE )
#undef ROCKETNET_CLIENT_TRANSPORT_LAYOUT_FIELDS_DECLARE
};

// The members construction does not zero. Three are buffers: their bytes
// are written before they are read, at the count or ring index that names
// them, so only their counts are state, and construction and the connection
// reset set the counts and never touch the bytes (6.5 MB of receive buffers,
// 2.1 MB of received packets and 1.6 MB of payload rings at the defaults).
// Two initialize themselves: the lease table by its constructor, and the
// protocol by rocketnet_client_protocol_init.

static bool rocketnet_client_transport_buffer_member( const char * name )
{
    return strcmp( name, "payload_ring" ) == 0 || strcmp( name, "received_packets" ) == 0 || strcmp( name, "receive_buffer" ) == 0 ||
           strcmp( name, "packet_leases" ) == 0 || strcmp( name, "protocol" ) == 0;
}

static void rocketnet_client_transport_reset_buffer_counts( rocketnet_client_transport_t * client, int ring )
{
    client->payload_ring[ring].num_packets = 0;
}

// Every member that is state zeroed in the raw storage, then default
// initialization (members with initializers get them, nothing else is
// written), then the buffers' counts: the buffers' bytes are never written
// here, so their pages are touched only when a packet uses them. Zero only
// what is state (owner, 2026-10-22): pointers, handles, state that must start
// valid, and keys; never whole structures.

static void rocketnet_client_transport_construct( rocketnet_client_transport_t * client )
{
    uint8_t * storage = (uint8_t *) client;
#define ROCKETNET_CLIENT_TRANSPORT_ZERO_STATE( name, declaration ) \
    if ( !rocketnet_client_transport_buffer_member( #name ) ) \
        memset( storage + offsetof( rocketnet_client_transport_t, name ), 0, sizeof( ( (rocketnet_client_transport_t *) 0 )->name ) );
    ROCKETNET_CLIENT_TRANSPORT_LAYOUT_FIELDS( ROCKETNET_CLIENT_TRANSPORT_ZERO_STATE )
#undef ROCKETNET_CLIENT_TRANSPORT_ZERO_STATE
    new ( client ) rocketnet_client_transport_t;

    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
        rocketnet_client_transport_reset_buffer_counts( client, ring );

    client->received_packets.num_packets = 0;
    client->received_packets.receive_index = 0;

    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
        client->receive_buffer[i].num_packets = 0;
}

#if ROCKETNET_DEV
bool rocketnet_client_transport_inventory_matches_for_test( char * failure, size_t failure_bytes )
{
#define TRANSPORT_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rocketnet_client_transport_t, field, -1 ),
    static const rocketnet_reset_member_t members[] = { ROCKETNET_CLIENT_TRANSPORT_LAYOUT_FIELDS( TRANSPORT_MEMBER ) };
#undef TRANSPORT_MEMBER
#define TRANSPORT_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rocketnet_client_transport_t, field, #decision[0] == 'R' ),
    static const rocketnet_reset_member_t decisions[] = { ROCKETNET_CLIENT_TRANSPORT_CONNECTION_FIELDS( TRANSPORT_DECISION ) };
#undef TRANSPORT_DECISION
    return rocketnet_reset_inventory_matches( "rocketnet_client_transport_t", members, sizeof( members ) / sizeof( members[0] ),
                                              decisions, sizeof( decisions ) / sizeof( decisions[0] ), 0,
                                              sizeof( rocketnet_client_transport_t ), alignof( rocketnet_client_transport_t ),
                                              failure, failure_bytes );
}

bool rocketnet_client_transport_reset_witness_for_test( struct rocketnet_client_transport_t * client )
{
    if ( !client || client->io_thread )
        return false;

    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        client->payload_ring[ring].num_packets = 7;
        memset( client->payload_ring[ring].info, 0xA5, sizeof( client->payload_ring[ring].info ) );
        memset( client->payload_ring[ring].data, 0xA5, sizeof( client->payload_ring[ring].data ) );
    }

    memset( &client->received_packets, 0xA5, sizeof( client->received_packets ) );
    client->stats_counted = UINT64_MAX;
    rocketnet_client_connection_reset( client );

    // the buffers are reset by their counts: their bytes keep the poison and are unreachable, because nothing reads a slot at or past its count

    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        if ( client->payload_ring[ring].num_packets != 0 )
            return false;
    }

    return client->received_packets.num_packets == 0 && client->received_packets.receive_index == 0 &&
           client->stats_counted == 0;
}

void rocketnet_client_transport_mark_connection_state_for_test( struct rocketnet_client_transport_t * client )
{
    rocketnet_assert( client );

    // mark a live per-connection counter so the reset witness checks it

    client->stats_counted = 1;
}

bool rocketnet_client_transport_connection_state_clean_for_test( struct rocketnet_client_transport_t * client,
                                                                 char * failure, size_t failure_bytes )
{
    rocketnet_assert( client && failure && failure_bytes > 0 );

    failure[0] = '\0';

    const char * member = NULL;
    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS && !member; ring++ )
    {
        if ( client->io_thread && ring != client->client_io.send_fill )
            continue;
        if ( client->payload_ring[ring].num_packets != 0 )
            member = "payload_ring";
    }

    if ( !member && ( client->received_packets.num_packets != 0 || client->received_packets.receive_index != 0 ) )
        member = "received_packets";
    if ( !member && client->stats_counted != 0 )
        member = "stats_counted";
    if ( member )
        snprintf( failure, failure_bytes, "rocketnet_client_transport_t.%s: not reset when the connection ended", member );

    return member == NULL;
}
#endif // #if ROCKETNET_DEV

// Every refusal reaches here before a worker is successfully created. No
// thread owns transport storage, so failed construction is already quiescent.

static void rocketnet_client_transport_abort_create( rocketnet_client_transport_t * transport )
{
    rocketnet_assert( !transport->thread );
    transport->stop_requested = true;
    transport->stopped.store( true, std::memory_order_release );

    transport->client_io.stopped.store( true, std::memory_order_release );

    rocketnet_client_transport_destroy( transport );
}

static void rocketnet_client_thread_function( rocketnet_platform_thread_t * thread, void * data );
static void rocketnet_client_receive_packets( rocketnet_client_transport_t * client_transport, const double * synthetic_time = NULL );

// The transport-owned part of ClientSession End. The protocol overload has
// already retired keys and cleared protocol state. Only the main-thread fill
// ring is touched when an I/O thread is active; a handed-off ring is owned by
// that thread and is already sealed under the ended session.

void rocketnet_client_connection_reset( struct rocketnet_client_transport_t * client )
{
    rocketnet_assert( client );

    client->packet_leases.reset();

    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        if ( client->io_thread && ring != client->client_io.send_fill )
            continue;

        // only counts are reset; the ended connection cannot reach payload storage, and handed-off rings remain exclusively worker-owned

        rocketnet_client_transport_reset_buffer_counts( client, ring );
    }

    client->received_packets.num_packets = 0;
    client->received_packets.receive_index = 0;
    client->stats_counted = 0;
}

// The deterministic drop and reverse patterns (proofs/tla/Link.tla "everynth" and
// ReverseWindow) are on when the config names either and the link simulator
// exists. With both fields zero nothing below differs from the link
// simulator alone: every branch keyed on this predicate is the BASE branch.

static inline bool rocketnet_client_link_patterns( const rocketnet_client_transport_t * transport )
{
    return transport->link_simulator &&
           ( transport->config.simulated_drop_every_nth != 0 || transport->config.simulated_reverse_window != 0 );
}

// The link simulator is active when the setting in force changes the packets:
// any loss, latency, jitter or duplicate, or either pattern.  With all of
// them zero it is a pass-through that neither adds latency nor copies, and
// the I/O ring core (hand-off and caller step) carries the sends and receives
// exactly as a simulator-less transport.
//
// Taking the setting also applies the one the caller handed over, so the
// setting in force at the send is the setting the caller last asked for: the
// change takes effect at once, on and off, in either direction.

static inline bool rocketnet_client_link_active( rocketnet_client_transport_t * transport )
{
    if ( !transport->link_simulator )
        return false;

    return rocketnet_network_simulator_take_setting( transport->link_simulator, &transport->mutex ) ||
           rocketnet_client_link_patterns( transport );
}

// The path a connection runs on is fixed at create and never moves: the
// transport decides it, and the link simulator never changes it.  A live
// setting change only changes what the simulator does with the packets.
//
// A connection created with a simulator setting keeps the per-packet path it
// has always run (the simulator holds the packets, and the caller sends them
// one by one); a connection created without one keeps the ring path.  Which
// of the two it is was decided when the simulator was configured, and the
// live setting never changes it.

static inline bool rocketnet_client_io_core( const rocketnet_client_transport_t * transport )
{
#if ROCKETNET_DEV
    const bool runs_io_core = transport->io_thread || transport->step_in_update;
#else
    const bool runs_io_core = transport->io_thread;
#endif // #if ROCKETNET_DEV
    return runs_io_core &&
           !rocketnet_network_simulator_created_active( transport->link_simulator );
}

// Link!ReverseWindow: the window held by the simulator is emitted reversed, whole,
// at the end of each send_packets (and of the update that sends).

static inline void rocketnet_client_link_flush( rocketnet_client_transport_t * transport )
{
    if ( rocketnet_client_link_patterns( transport ) )
        rocketnet_network_simulator_flush_window( transport->link_simulator );
}

// The family race (TRANSPORT.md t11 rule 3): a token naming both an IPv4 and
// an IPv6 server address, on a host that can use both, dials both. The
// transport's io is then this pair, behind the ordinary operations table, so
// every path below it (the caller's sends, the I/O thread and its rings, the
// link simulator, the legacy receive worker, the caller's step) is unchanged:
// family[0] is the address the token selects (the IPv6 server, its socket
// connected to it as at BASE), family[1] the IPv4 server on a second socket
// of its own, opened and connected with the first at create, never during
// update. While both are in flight every packet sent to the primary server
// goes to the other server too, and both sockets are read. The first valid
// connection cookie commits the attempt to the family it arrived on
// (rocketnet_client_race_commit, proofs/tla/ClientConnection.tla ClientRecvCookie,
// proofs/tla/ServerConnection.tla CommitChoices): from then on only that family is
// sent to and read, so a connection request goes to one family only
// (OneRequestFamily, OneFamilyPerAttempt), and the other family's socket is
// closed by the thread that reads the sockets, at its next wait or receive.
// A late answer from the other family that was already read fails the
// source check and is counted as a foreign source.
//
// Threads (proofs/tla/RNClientRace.tla). The caller's thread commits (in update)
// and is the only thread that sends to a server address it reads from the
// protocol; the reading thread (the I/O thread, the legacy worker, or the
// caller's own step) closes the losing family's operating-system handles
// only after it observes the commit, and the caller touches only the kept
// family once it committed. No lock: one release store, read with acquire.
// The reading thread frees nothing: the losing family's memory, like the
// kept family's, is freed by the caller's thread when the transport is
// destroyed, after the reading thread is joined (FreedOnlyByCaller,
// FreedOnlyAfterJoin).

#define ROCKETNET_CLIENT_RACE_WAIT_SECONDS 0.001     // the reading thread's poll of the second socket during the race

struct rocketnet_client_race_t
{
    rocketnet_client_transport_io_operations_t operations;  // the table the transport holds: the inner capabilities, routed here
    rocketnet_client_transport_io_t family[2];              // [0] the token's selected family, [1] the other
    rocketnet_address_t server_address[2];                  // the server of each family, from the token
    std::atomic<int> committed;                             // -1 while both are in flight, else the family kept
    bool closed;                                            // the losing family's socket is closed (the reading thread)
    std::atomic<uint64_t> closed_send_failures;             // the losing socket's send failures, kept at its close
    void * context;
};

static void rocketnet_client_race_destroy( void * instance );

static rocketnet_client_race_t * rocketnet_client_race_of( const rocketnet_client_transport_t * transport )
{
    if ( !transport->io.operations || transport->io.operations->destroy != rocketnet_client_race_destroy )
        return NULL;

    return (rocketnet_client_race_t *) transport->io.instance;
}

static uint64_t rocketnet_client_race_family_send_failures( const rocketnet_client_transport_io_t * family )
{
    return family->instance && family->operations->send_failures ? family->operations->send_failures( family->instance ) : 0;
}

// the reading thread closes the losing family's handles once it sees the
// commit (proofs/tla/RNClientRace.tla ReaderStep); its memory stays until destroy

static int rocketnet_client_race_kept( rocketnet_client_race_t * race )
{
    const int committed = race->committed.load( std::memory_order_acquire );
    if ( committed >= 0 && !race->closed )
    {
        rocketnet_client_transport_io_t * lost = &race->family[1 - committed];
        race->closed_send_failures.store( rocketnet_client_race_family_send_failures( lost ), std::memory_order_release );

        if ( lost->operations->close )
            lost->operations->close( lost->instance );

        race->closed = true;
    }

    return committed;
}

// the caller's thread, after the reading thread is joined (proofs/tla/RNClientRace.tla
// CallerDestroy): both families' memory, the losing one's handles already
// closed

static void rocketnet_client_race_destroy( void * instance )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;

    for ( int i = 0; i < 2; i++ )
        rocketnet_client_transport_io_close( &race->family[i] );

    void * context = race->context;

    race->~rocketnet_client_race_t();

    rocketnet_free( context, race );
}

// the family whose server has the address family of to: only the two server
// addresses are ever sent to

static int rocketnet_client_race_family_of( const rocketnet_client_race_t * race, const rocketnet_address_t * to )
{
    return to->type == race->server_address[1].type ? 1 : 0;
}

// proofs/tla/RNClientRace.tla CallerSend: the caller's thread sends to both
// families while they are in flight, then to the kept family only

static void rocketnet_client_race_send( void * instance, const rocketnet_address_t * to, const uint8_t * packet_data, int packet_bytes )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;
    const int committed = race->committed.load( std::memory_order_acquire );
    const int family = rocketnet_client_race_family_of( race, to );

    if ( committed >= 0 )
    {
        // a packet queued for the losing family before the commit is dropped
        if ( family == committed )
            race->family[family].operations->send( race->family[family].instance, to, packet_data, packet_bytes );

        return;
    }

    race->family[family].operations->send( race->family[family].instance, to, packet_data, packet_bytes );

    if ( family == 0 && rocketnet_address_equal( to, &race->server_address[0] ) )
        race->family[1].operations->send( race->family[1].instance, &race->server_address[1], packet_data, packet_bytes );
}

static void rocketnet_client_race_send_batch( void * instance, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;
    const int committed = race->committed.load( std::memory_order_acquire );
    if ( committed >= 0 )
    {
        // a packet handed off for the losing family before the commit is dropped
        bool all_kept = true;

        for ( int i = 0; i < num_packets; i++ )
            all_kept = all_kept && rocketnet_client_race_family_of( race, sends[i].to ) == committed;

        rocketnet_client_transport_io_t * kept = &race->family[committed];

        if ( all_kept )
        {
            kept->operations->send_batch( kept->instance, sends, num_packets );
            return;
        }

        for ( int i = 0; i < num_packets; i++ )
            if ( rocketnet_client_race_family_of( race, sends[i].to ) == committed )
                kept->operations->send( kept->instance, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );

        return;
    }

    race->family[0].operations->send_batch( race->family[0].instance, sends, num_packets );

    for ( int i = 0; i < num_packets; i++ )
        if ( rocketnet_address_equal( sends[i].to, &race->server_address[0] ) )
            race->family[1].operations->send( race->family[1].instance, &race->server_address[1],
                                              sends[i].packet_data, sends[i].packet_bytes );
}

static int rocketnet_client_race_receive( void * instance, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;
    const int committed = rocketnet_client_race_kept( race );
    if ( committed >= 0 )
        return race->family[committed].operations->receive( race->family[committed].instance, from, packet_data, packet_bytes );

    const int bytes = race->family[0].operations->receive( race->family[0].instance, from, packet_data, packet_bytes );
    if ( bytes != 0 )
        return bytes;

    return race->family[1].operations->receive( race->family[1].instance, from, packet_data, packet_bytes );
}

static int rocketnet_client_race_receive_batch( void * instance, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;
    const int committed = rocketnet_client_race_kept( race );
    if ( committed >= 0 )
        return race->family[committed].operations->receive_batch( race->family[committed].instance, from, packet_data, packet_stride, packet_bytes, max_packets );

    int received = race->family[0].operations->receive_batch( race->family[0].instance, from, packet_data, packet_stride, packet_bytes, max_packets );
    if ( received < 0 || received >= max_packets )
        return received;

    const int second = race->family[1].operations->receive_batch( race->family[1].instance, from + received,
                                                                  packet_data + received * packet_stride, packet_stride,
                                                                  packet_bytes + received, max_packets - received );
    return second > 0 ? received + second : received;
}

// while both are in flight the wait blocks on the primary socket and its
// wake, at most the race poll, and reports readable so the second socket is
// read too; the handshake's packets are few, and the race ends at the first
// cookie

static bool rocketnet_client_race_wait( void * instance, bool readable, double timeout_seconds )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;
    const int committed = rocketnet_client_race_kept( race );
    if ( committed >= 0 )
        return race->family[committed].operations->wait( race->family[committed].instance, readable, timeout_seconds );
    if ( !readable )
        return race->family[0].operations->wait( race->family[0].instance, false, timeout_seconds );

    const double timeout = timeout_seconds < ROCKETNET_CLIENT_RACE_WAIT_SECONDS ? timeout_seconds : ROCKETNET_CLIENT_RACE_WAIT_SECONDS;
    race->family[0].operations->wait( race->family[0].instance, true, timeout );

    return true;
}

// the wake is level-triggered: both are set while both are in flight, so the
// kept family's wake is set whichever wins

static void rocketnet_client_race_wake( void * instance )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;
    const int committed = race->committed.load( std::memory_order_acquire );

    for ( int i = 0; i < 2; i++ )
        if ( committed < 0 || committed == i )
            race->family[i].operations->wake( race->family[i].instance );
}

static uint64_t rocketnet_client_race_send_failures( void * instance )
{
    rocketnet_client_race_t * race = (rocketnet_client_race_t *) instance;
    const int committed = race->committed.load( std::memory_order_acquire );
    if ( committed >= 0 )
        return rocketnet_client_race_family_send_failures( &race->family[committed] ) +
               race->closed_send_failures.load( std::memory_order_acquire );

    return rocketnet_client_race_family_send_failures( &race->family[0] ) +
           rocketnet_client_race_family_send_failures( &race->family[1] );
}

// the commit, on the caller's thread (proofs/tla/RNClientRace.tla CallerCommit): the
// attempt keeps the family that answered first, and its server is the
// protocol's server from now on

static void rocketnet_client_race_commit( rocketnet_client_transport_t * transport, rocketnet_client_race_t * race, int family )
{
    transport->protocol.server_address = race->server_address[family];

    race->committed.store( family, std::memory_order_release );

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client keeps %s: the first cookie answered there",
                    rocketnet_address_to_string( &race->server_address[family], address_string ) );
}

static void rocketnet_client_link_send( rocketnet_client_transport_t * transport,
                                        const rocketnet_address_t * to,
                                        const uint8_t * packet_data, int packet_bytes )
{
    if ( !transport->link_simulator )
    {
        transport->io.operations->send( transport->io.instance, to, packet_data, packet_bytes );
        return;
    }

    if ( !rocketnet_client_link_active( transport ) )
    {
        transport->io.operations->send( transport->io.instance, to, packet_data, packet_bytes );
        return;
    }

    rocketnet_network_simulator_send( transport->link_simulator, &transport->link_simulator_from,
                                      to, packet_data, packet_bytes, transport->time );
}

// proofs/tla/RNClientLinkStop.tla Drain: a running update sends every copy the link
// simulator holds that has come due by now.  A held packet leaves on the
// thread that owns the simulator: the caller's thread on both paths, since
// the per-packet path sends there and the ring path's hand-off is there too.
// A simulator holding nothing drains nothing, so it is skipped without the
// slot walk, as the server drain does: every development connection owns a
// simulator, and an all-zero setting holds no packet.

static void rocketnet_client_link_drain( rocketnet_client_transport_t * transport, double now )
{
    if ( !transport->link_simulator || !rocketnet_network_simulator_holds_any( transport->link_simulator ) )
        return;

    rocketnet_address_t from, to;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes;
    while ( ( packet_bytes = rocketnet_network_simulator_receive_any( transport->link_simulator,
                                                                      now, &from, &to, packet ) ) > 0 )
        transport->io.operations->send( transport->io.instance, &to, packet, packet_bytes );
}

// Shutdown completion sends everything the link simulator still holds, the
// disconnect burst included. Stop publishes quit with release, transferring
// the simulator from the caller to the worker; no running update can touch it
// afterward. Step-driven transports do this in their explicit shutdown step.
// Shutdown holds the clock fixed, so a held packet would never come due. The
// loss and duplicate policy already ran at send; what remains leaves now,
// ahead of its latency (proofs/tla/RNClientLinkStop.tla WorkerComplete).

static void rocketnet_client_link_drain_at_stop( rocketnet_client_transport_t * transport )
{
    if ( !transport->link_simulator )
        return;

    rocketnet_network_simulator_flush_window( transport->link_simulator );

    rocketnet_address_t from, to;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes;
    while ( ( packet_bytes = rocketnet_network_simulator_receive_any( transport->link_simulator,
                                                                      DBL_MAX, &from, &to, packet ) ) > 0 )
        transport->io.operations->send( transport->io.instance, &to, packet, packet_bytes );
}

// the ring game threads fill: the one send_fill names (always ring 0 on the
// BASE path, which never hands off)

static inline struct rocketnet_client_payload_ring_t * rocketnet_client_fill_ring( rocketnet_client_transport_t * client_transport )
{
    return &client_transport->payload_ring[client_transport->client_io.send_fill];
}

// the ring a frame of any ring lives in

static inline struct rocketnet_client_payload_ring_t * rocketnet_client_frame_ring( rocketnet_client_transport_t * client_transport, const uint8_t * frame )
{
    for ( int i = 1; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        struct rocketnet_client_payload_ring_t * ring = &client_transport->payload_ring[i];
        if ( frame >= ring->data && frame < ring->data + sizeof( ring->data ) )
            return ring;
    }

    return &client_transport->payload_ring[0];
}

// the ring commit of a packet the main thread built, defined with the ring
static void rocketnet_client_transport_io_commit_packet( rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes );

// The I/O ring path's link simulator: the hand-off is where the sealed
// packets leave the ring for the wire, so with the simulator active that is
// where they go instead.  The setting in force decides there whether a
// packet is let out, lost or held, and a held packet leaves with the drain
// when it comes due.  Nothing here changes the threading of the connection:
// the ring, the hand-off and the drain are the caller's thread, as they
// were before the simulator was here.

static void rocketnet_client_link_hold( rocketnet_client_transport_t * transport, const rocketnet_address_t * to )
{
    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_fill_ring( transport );
    uint64_t committed = ring->num_packets;
    if ( committed > ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
        committed = ROCKETNET_CLIENT_PAYLOAD_RING_SIZE;

    for ( uint64_t i = 0; i < committed; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
            rocketnet_network_simulator_send( transport->link_simulator, &transport->link_simulator_from, to,
                                              ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET,
                                              (int) ring->info[i].wire_bytes, transport->time );
    }

    ring->num_packets = 0;
    transport->stats_counted = 0;

    // the window the reverse pattern holds is emitted whole, at the end of the hand-off, as the per-packet path does at the end of its send

    rocketnet_client_link_flush( transport );
}

// NULL time_callback: rocketnet_platform_time as double (*)( void * )

static double rocketnet_client_platform_time( void * context )
{
    (void) context;
    return rocketnet_platform_time();
}

void rocketnet_client_io_wipe( void * data, size_t bytes )
{
    sodium_memzero( data, bytes );
}

// proofs/tla/RNClientIoDecrypt.tla PublishKeys: the protocol's hook at the challenge

static void rocketnet_client_transport_publish_keys( void * context, const struct rocketnet_client_protocol_t * client )
{
    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t *) context;
    rocketnet_client_io_publish_keys( &client_transport->client_io, client->keys.session_server_to_client,
                                      client->signed_fields.protocol_id, client->connection_salt, client->packet_filter,
                                      &client->server_address );
}

// proofs/tla/RNClientIoDecrypt.tla RetireKeys: the protocol's hook at a terminal state.
// When no background I/O thread owns the open block, take the retired epoch
// immediately so sodium_memzero erases session_server_to_client and salt at the
// terminal transition instead of leaving the retired copy in memory until the
// next update step or stop.

static void rocketnet_client_transport_retire_keys( void * context )
{
    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t *) context;
    rocketnet_client_io_retire_keys( &client_transport->client_io );

    if ( !client_transport->thread )
        rocketnet_client_io_take_keys( &client_transport->client_io );
}

bool rocketnet_client_transport_uses_io_thread( const rocketnet_client_transport_io_operations_t * operations )
{
    return operations && operations->wait && operations->wake;
}

// rocketnet_client_transport_default_config lives in rocketnet_config.cpp,
// written from the config table so every default is declared exactly once

// Pre-allocation client memory plan. The client's create allocations are the
// transport block and the granted transport's socket; each has a byte helper used here, never a re-derived size. The
// components embedded in the transport block (receive buffers, payload ring,
// protocol state) ride its sizeof, and the SDK allocator's alignment and
// ownership overhead is added per allocation. The transport name must be a
// concrete built-in for this platform; unknown names, "auto" and transport lists
// are not concrete and return false.
//
// The plan takes no token, so it is the bound for the worst token: one naming
// both families on a host that can use both, whose create opens the family
// race's second socket of the granted transport (no second thread) and its
// rocketnet_client_race_t (TRANSPORT.md t11 rule 3, proofs/tla/ServerConnection.tla
// RaceFamilies). A one-family create allocates less than the plan.

// total_bytes is all through the caller's allocator; thread_stack_bytes is
// the OS-owned default stack of the network thread a threaded client starts.
struct rocketnet_client_memory_plan_t
{
    uint64_t transport_bytes;
    uint64_t transport_io_bytes, race_bytes, link_simulator_bytes, total_bytes, thread_stack_bytes;
    int sockets;
};

// The link simulator's capacity: the packets one connection holds while the
// setting delays them.  A development build creates the simulator for every
// connection, so the plan carries its bytes.
#define ROCKETNET_CLIENT_LINK_SIMULATOR_SLOTS 512

#if defined( ROCKETNET_PLATFORM_HAS_WAKE )
struct rocketnet_portable_io_client_layout_t
{
    void * context;
    void * socket;
    void * wake;
};
#if ( ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( __linux__ ) )
struct rocketnet_platform_wake_layout_t
{
    void * context;
    int socket_fd;
    int event_fd;
};
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
struct rocketnet_platform_wake_layout_t
{
    void * context;
    int kq;
    int socket_fd;
    bool read_enabled;
};
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
struct rocketnet_platform_wake_layout_t
{
    void * context;
    void * socket_handle;
    void * read_event;
    void * wake_event;
};
#else
struct rocketnet_platform_wake_layout_t
{
    void * context;
};
#endif // #if ( ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( __linux__ ) )
#endif // #if defined( ROCKETNET_PLATFORM_HAS_WAKE )

// Mirrors of the native client I/O records, member for member, so the plan
// counts the bytes their creates allocate: the gso client's one record
// (sdk/rocketnet_client_gso_linux.cpp rocketnet_client_gso_io_t), beside its
// platform socket and wake, and the iocp client's one record
// (sdk/rocketnet_client_iocp_windows.cpp rocketnet_client_iocp_io_t), beside
// its platform socket. The lifecycle cases measure them against the plan.
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
struct rocketnet_client_gso_io_layout_t
{
    void * context;
    rocketnet_platform_socket_t * socket;
    rocketnet_platform_wake_t * wake;
    rocketnet_client_gso_t gso;
};
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
struct rocketnet_client_iocp_receive_layout_t
{
    OVERLAPPED overlapped;
    WSABUF buffer;
    DWORD flags;
    INT from_bytes;
    sockaddr_storage from;
    int index;
};
struct rocketnet_client_iocp_io_layout_t
{
    void * context;
    rocketnet_platform_socket_t * socket;
    bool ipv6;
    HANDLE port;
    rocketnet_client_iocp_t core;
    rocketnet_client_iocp_receive_layout_t receives[ROCKETNET_CLIENT_IOCP_RECEIVES];
    OVERLAPPED_ENTRY entries[ROCKETNET_CLIENT_IOCP_RECEIVES + 1];
    uint8_t buffers[ROCKETNET_CLIENT_IOCP_RECEIVES * ROCKETNET_CLIENT_IOCP_RECEIVE_BYTES];
};
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_DEV
// The memory plan seam allocates this mirror of the platform thread_record_t
// (the thread and its shim, one allocation, sdk/rocketnet_platform.cpp) that
// rocketnet_platform_thread_create allocates, so the seam allocates the bytes
// rocketnet_platform_thread_allocation_bytes counts.
struct rocketnet_thread_shim_layout_t
{
    void * context;
    void * real_thread_data;
    void * real_thread_function;
};
struct rocketnet_thread_record_layout_t
{
    rocketnet_platform_thread_t thread;
    rocketnet_thread_shim_layout_t shim;
};

// Test seam for socket-free, thread-free client memory plan unit testing.
static bool rocketnet_client_memory_plan_seam_enabled = false;

void rocketnet_client_transport_set_memory_plan_seam_for_test( bool enable )
{
    rocketnet_client_memory_plan_seam_enabled = enable;
}
#endif // #if ROCKETNET_DEV

// The client network thread's create and release. The memory plan seam stands in
// a bare allocation of the thread record for the thread, so it opens no thread.

static rocketnet_platform_thread_t * rocketnet_client_thread_create( void * context, rocketnet_platform_thread_func_t function, void * argument, int * status )
{
#if ROCKETNET_DEV
    if ( rocketnet_client_memory_plan_seam_enabled )
    {
        void * record = rocketnet_malloc( context, sizeof( rocketnet_thread_record_layout_t ) );
        if ( !record )
            rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return (rocketnet_platform_thread_t *) record;
    }
#endif // #if ROCKETNET_DEV

    return rocketnet_platform_thread_create( context, function, argument, status );
}

static void rocketnet_client_thread_release( void * context, rocketnet_platform_thread_t * thread )
{
#if ROCKETNET_DEV
    if ( rocketnet_client_memory_plan_seam_enabled )
    {
        rocketnet_free( context, thread );
        return;
    }
#else
    (void) context;
#endif // #if ROCKETNET_DEV

    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
}

static bool rocketnet_client_transport_memory_plan( const rocketnet_client_transport_config_t * config,
                                                    const char * transport, rocketnet_client_memory_plan_t & plan )
{
    plan = {};

    if ( !transport )
        return false;

    rocketnet_client_transport_config_t defaults;
    if ( !config )
    {
        rocketnet_client_transport_default_config( &defaults );
        config = &defaults;
    }

    plan.transport_bytes = rocketnet_memory_allocations( { sizeof( rocketnet_client_transport_t ) } );

    if ( rocketnet_public_link_simulator_development() )
    {
        // the simulator every development connection owns, created with it
        plan.link_simulator_bytes = rocketnet_network_simulator_memory_bytes( ROCKETNET_CLIENT_LINK_SIMULATOR_SLOTS,
                                                                              config->simulated_reverse_window );
        if ( !plan.link_simulator_bytes )
            return false;
    }

#if ROCKETNET_DEV
    const bool threaded = !config->step_driven;
#else
    const bool threaded = true;
#endif // #if ROCKETNET_DEV

    if ( strcmp( transport, "loopback" ) == 0 )
    {
        // the endpoint's record and queue are in the caller's hub, allocated
        // at hub create, and loopback opens no socket. A threaded client
        // still starts its network thread.
        plan.transport_io_bytes = 0;
        if ( threaded && !rocketnet_memory_add( plan.transport_io_bytes, 1, rocketnet_platform_thread_allocation_bytes() ) )
            return false;

        plan.sockets = 0;
    }
    else if ( strcmp( transport, "portable" ) == 0
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC // msgx exists on macOS only, as in the registry and the server plan
              || strcmp( transport, "msgx" ) == 0
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX // the native clients the default ladder ranks first
              || strcmp( transport, "gso" ) == 0
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
              || strcmp( transport, "iocp" ) == 0
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
        if ( strcmp( transport, "gso" ) == 0 )
            plan.transport_io_bytes = rocketnet_memory_allocations( { sizeof( rocketnet_platform_socket_t ),
                                                                      sizeof( rocketnet_client_gso_io_layout_t ),
                                                                      sizeof( rocketnet_platform_wake_layout_t ) } );
        else
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
            if ( strcmp( transport, "iocp" ) == 0 )
            plan.transport_io_bytes = rocketnet_memory_allocations( { sizeof( rocketnet_platform_socket_t ),
                                                                      sizeof( rocketnet_client_iocp_io_layout_t ) } );
        else
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#if defined( ROCKETNET_PLATFORM_HAS_WAKE )
            plan.transport_io_bytes = rocketnet_memory_allocations( { sizeof( rocketnet_platform_socket_t ),
                                                                      sizeof( rocketnet_portable_io_client_layout_t ),
                                                                      sizeof( rocketnet_platform_wake_layout_t ) } );
#else
        plan.transport_io_bytes = rocketnet_platform_socket_allocation_bytes();
#endif // #if defined( ROCKETNET_PLATFORM_HAS_WAKE )

        // the family race's second socket, the same allocations as the first,
        // and the race block that routes the two. The copy is taken before the
        // network thread is added, so the race does not count a second thread.
        plan.race_bytes = rocketnet_memory_allocations( { sizeof( rocketnet_client_race_t ) } );
        if ( !plan.race_bytes || !rocketnet_memory_add( plan.race_bytes, 1, plan.transport_io_bytes ) )
            return false;

        if ( threaded )
        {
            if ( !rocketnet_memory_add( plan.transport_io_bytes, 1, rocketnet_platform_thread_allocation_bytes() ) )
                return false;
        }

        plan.sockets = 2;
    }
    else
        return false;

    // loopback opens no socket and runs no family race; its endpoint is in
    // the caller's hub, so a step-driven loopback client plans no I/O bytes

    if ( !plan.transport_bytes || ( plan.sockets != 0 && !plan.transport_io_bytes ) )
        return false;
    if ( plan.sockets != 0 && !plan.race_bytes )
        return false;

    for ( uint64_t bytes : { plan.transport_bytes, plan.transport_io_bytes, plan.race_bytes,
                             plan.link_simulator_bytes } )
        if ( !rocketnet_memory_add( plan.total_bytes, 1, bytes ) )
            return false;

    if ( threaded )
    {
        plan.thread_stack_bytes = rocketnet_thread_stack_bytes();
        if ( !plan.thread_stack_bytes )
            return false;
    }

    return plan.total_bytes <= SIZE_MAX;
}

uint64_t rocketnet_client_transport_ladder_allocator_bytes( const rocketnet_client_transport_config_t * config )
{
    rocketnet_client_transport_config_t defaults;

    if ( !config )
    {
        rocketnet_client_transport_default_config( &defaults );

        config = &defaults;
    }

    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );
    struct rocketnet_transport_ladder_t ladder;
    if ( !rocketnet_transport_ladder_parse( config->transport, available, num_available, &ladder, false ) )
        return 0;

    uint64_t most = 0;

    for ( int i = 0; i < ladder.num_candidates; ++i )
    {
        rocketnet_client_memory_plan_t plan;
        if ( rocketnet_client_transport_memory_plan( config, rocketnet_transport_name( ladder.candidates[i] ), plan ) && plan.total_bytes > most )
            most = plan.total_bytes;
    }

    return most;
}

uint64_t rocketnet_client_transport_required_memory( const rocketnet_client_transport_config_t * config, const char * transport )
{
    rocketnet_client_memory_plan_t plan;
    return rocketnet_client_transport_memory_plan( config, transport, plan ) ? plan.total_bytes : 0;
}

rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time )
{
    return rocketnet_client_transport_create_ex( context, connect_token, config, time, NULL );
}

// One socket of the ladder: opens the transport bound as bind asks and
// connects it to server (TRANSPORT.md t11: the kernel then drops datagrams
// from any other source). A connect the kernel refuses closes the socket and
// is a refusal. bind is written with the bound address only when the socket
// is opened and connected, so a refused attempt never constrains the next
// one's port. A refusal for want of memory is reported in config->status.

static bool rocketnet_client_transport_open_connected( const rocketnet_client_transport_io_operations_t * operations, void * operations_context,
                                                       const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind,
                                                       const rocketnet_address_t * server, rocketnet_client_transport_io_t * io )
{
    rocketnet_address_t attempt = *bind;
    if ( !rocketnet_client_transport_io_open( operations, operations_context, config, &attempt, io ) )
        return false;

    if ( io->operations->connect && !io->operations->connect( io->instance, server ) )
    {
        rocketnet_client_transport_io_close( io );
        return false;
    }

    *bind = attempt;
    return true;
}

static rocketnet_client_transport_t * rocketnet_client_transport_create_internal( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status,
                                                                                  const rocketnet_client_transport_io_operations_t * step_driven_operations, void * step_driven_transport_context, size_t config_bytes, uint64_t test_timestamp,
                                                                                  const uint64_t * resolved_memory_budget = NULL );

rocketnet_client_transport_t * rocketnet_client_transport_create_ex( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status )
{
    return rocketnet_client_transport_create_internal( context, connect_token, config, time, status, NULL, NULL, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, 0 );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_current( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time )
{
    return rocketnet_client_transport_create_internal( context, connect_token, config, time, NULL, NULL, NULL, sizeof( struct rocketnet_client_transport_config_t ), 0 );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_current_ex( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status )
{
    return rocketnet_client_transport_create_internal( context, connect_token, config, time, status, NULL, NULL, sizeof( struct rocketnet_client_transport_config_t ), 0 );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_resolved( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time,
                                                                           uint64_t memory_budget_bytes, int * status )
{
    rocketnet_assert( config );
    return rocketnet_client_transport_create_internal( context, connect_token, config, time, status, NULL, NULL, sizeof( struct rocketnet_client_transport_config_t ), 0, &memory_budget_bytes );
}

#if ROCKETNET_DEV
rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time,
                                                                              const rocketnet_client_transport_io_operations_t * operations, void * transport_context )
{
    if ( !operations )
        return NULL;

    return rocketnet_client_transport_create_internal( context, connect_token, config, time, NULL, operations, transport_context, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, 0 );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_at_timestamp( void * context, const uint8_t * connect_token,
                                                                                           const struct rocketnet_client_transport_config_t * config, double time, uint64_t current_timestamp,
                                                                                           const rocketnet_client_transport_io_operations_t * operations, void * transport_context )
{
    return rocketnet_client_transport_create_step_driven_at_timestamp_ex( context, connect_token, config, time, current_timestamp, operations, transport_context, NULL );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_at_timestamp_ex( void * context, const uint8_t * connect_token,
                                                                                              const struct rocketnet_client_transport_config_t * config, double time, uint64_t current_timestamp,
                                                                                              const rocketnet_client_transport_io_operations_t * operations, void * transport_context, int * status )
{
    if ( !operations || current_timestamp == 0 )
        return NULL;

    return rocketnet_client_transport_create_internal( context, connect_token, config, time, status, operations, transport_context,
                                                       sizeof( struct rocketnet_client_transport_config_t ), current_timestamp );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_current( void * context, const uint8_t * connect_token,
                                                                                      const struct rocketnet_client_transport_config_t * config, double time,
                                                                                      const rocketnet_client_transport_io_operations_t * operations, void * transport_context )
{
    if ( !operations )
        return NULL;

    return rocketnet_client_transport_create_internal( context, connect_token, config, time, NULL, operations, transport_context,
                                                       sizeof( struct rocketnet_client_transport_config_t ), 0 );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_ex( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time,
                                                                                 const rocketnet_client_transport_io_operations_t * operations, void * transport_context, int * status )
{
    if ( !operations )
        return NULL;

    return rocketnet_client_transport_create_internal( context, connect_token, config, time, status, operations, transport_context, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, 0 );
}
#endif // #if ROCKETNET_DEV

rocketnet_client_io_t * rocketnet_client_transport_client_io( rocketnet_client_transport_t * client_transport )
{
#if ROCKETNET_DEV
    return client_transport && ( client_transport->io_thread || client_transport->step_in_update ) ? &client_transport->client_io : NULL;
#else
    return client_transport && client_transport->io_thread ? &client_transport->client_io : NULL;
#endif // #if ROCKETNET_DEV
}

#if ROCKETNET_DEV
// Private state seam for the client refusal unit. It makes a deterministic
// connected session without a socket or clock, exposes only the fields whose
// immobility is the refusal contract, and gives that unit one live mutation
// for each forbidden outcome. No declaration enters a public header.

void rocketnet_client_transport_seed_refusal_state_for_test(
    rocketnet_client_transport_t * client_transport, uint64_t protocol_id,
    const uint8_t * packet_filter, const uint8_t * connection_salt,
    const uint8_t * session_server_to_client )
{
    rocketnet_assert( client_transport && packet_filter && connection_salt && session_server_to_client );

    rocketnet_client_protocol_t * protocol = &client_transport->protocol;
    protocol->state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    protocol->signed_fields.protocol_id = protocol_id;
    memcpy( protocol->packet_filter, packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( protocol->connection_salt, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( protocol->keys.session_server_to_client, session_server_to_client, ROCKETNET_KEY_BYTES );
    protocol->client_index = 3;
    protocol->max_clients = 8;
    rocketnet_replay_advance( &protocol->replay_session, 23 );
    rocketnet_replay_advance( &protocol->accepted_session, 19 );

    protocol->ack.largest_acked_sequence = 13;
    protocol->ack.highest_received_sequence = 17;
    protocol->last_receive_time = 999.5;
}

void rocketnet_client_transport_refusal_state_for_test(
    const rocketnet_client_transport_t * client_transport,
    int * state, int * client_index, int * max_clients,
    uint64_t * replay_highest, uint64_t * accepted_highest,
    uint64_t * largest_acked, uint64_t * highest_received,
    double * last_receive_time, int * payloads, int * acked_packets )
{
    rocketnet_assert( client_transport );

    const rocketnet_client_protocol_t * protocol = &client_transport->protocol;

    if ( state )
        *state = protocol->state;
    if ( client_index )
        *client_index = protocol->client_index;
    if ( max_clients )
        *max_clients = protocol->max_clients;
    if ( replay_highest )
        *replay_highest = protocol->replay_session.highest_opened;
    if ( accepted_highest )
        *accepted_highest = protocol->accepted_session.highest_opened;
    if ( largest_acked )
        *largest_acked = protocol->ack.largest_acked_sequence;
    if ( highest_received )
        *highest_received = protocol->ack.highest_received_sequence;
    if ( last_receive_time )
        *last_receive_time = protocol->last_receive_time;
    if ( payloads )
        *payloads = client_transport->received_packets.num_packets;
    if ( acked_packets )
        *acked_packets = protocol->num_acked;
}

void rocketnet_client_transport_mutate_refusal_state_for_test(
    rocketnet_client_transport_t * client_transport, int outcome, int delta )
{
    rocketnet_assert( client_transport && ( delta == -1 || delta == 1 ) );

    if ( outcome == 0 )
        client_transport->received_packets.num_packets += delta;
    else if ( outcome == 1 )
    {
        if ( delta > 0 )
            client_transport->protocol.ack.largest_acked_sequence++;
        else
            client_transport->protocol.ack.largest_acked_sequence--;
    }
    else if ( outcome == 2 )
    {
        if ( delta > 0 )
            client_transport->protocol.replay_session.highest_opened++;
        else
            client_transport->protocol.replay_session.highest_opened--;
    }
    else
        rocketnet_assert( 0 );
}
#endif // #if ROCKETNET_DEV

// The inputs a client create starts from, resolved before anything is
// allocated or opened: the layered config (DEFAULT < STRUCT < ENV) validated,
// the parsed transport ladder, and which candidates the operator memory
// budget refuses.

struct rocketnet_client_transport_resolved_t
{
    struct rocketnet_client_transport_config_t config;
    struct rocketnet_transport_ladder_t ladder;
    bool budget_refused[ROCKETNET_NUM_TRANSPORTS];
};

static bool rocketnet_client_transport_resolve( const struct rocketnet_client_transport_config_t * config, size_t config_bytes,
                                                const uint64_t * resolved_memory_budget,
                                                struct rocketnet_client_transport_resolved_t * out, int * status )
{
    rocketnet_assert( out );

    // the config layering: DEFAULT < STRUCT < ENV. NULL config is
    // the development posture (the default shape); the caller's struct is
    // the middle layer; the environment overlays both through the config
    // table, every override logged loudly with the value it displaced.
    // validation refuses loudly and never clamps. A config the public create
    // already resolved (resolved_memory_budget set,
    // rocketnet_client_transport_create_resolved) takes no second environment
    // pass: connect uses the config create validated and sized, whatever the
    // environment says now.

    struct rocketnet_client_transport_config_t effective_config;
    rocketnet_client_transport_default_config_current( &effective_config );

    if ( config )
        memcpy( &effective_config, config, config_bytes );

    int num_config_fields = 0;
    const struct rocketnet_config_field_t * config_fields = rocketnet_client_transport_config_fields( &num_config_fields );

    if ( !resolved_memory_budget && !rocketnet_config_apply_env( "client", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;

        return false;
    }

    if ( !rocketnet_config_validate( "client", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;

        return false;
    }

    if ( !rocketnet_config_validate_public_link_simulator( "client", &effective_config, config_fields, num_config_fields, rocketnet_public_link_simulator_development() ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;

        return false;
    }

    if ( rocketnet_config_packet_filter_refused( "client", effective_config.packet_filter ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION;

        return false;
    }

    config = &effective_config;

#if ROCKETNET_DEV
    // step_driven is a development-only boolean stored at full width for the
    // private transport config's revision stability. Refuse any spelling other
    // than zero or one before selecting a transport.

    if ( config->step_driven > 1 )
    {
        rocketnet_error( "client config step_driven %llu is out of range (0 to 1)",
                         (unsigned long long) config->step_driven );

        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;

        return false;
    }
#endif // #if ROCKETNET_DEV

    // the transport candidate list parses before anything allocates: a typo fails here, loudly, listing the valid names

    int num_available_transports = 0;
    const int * available_transports = rocketnet_client_transport_io_availability( &num_available_transports );

    struct rocketnet_transport_ladder_t ladder;
    if ( !rocketnet_transport_ladder_parse( config->transport, available_transports, num_available_transports, &ladder, false ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_TRANSPORT_LIST;

        return false;
    }

    // the operator memory budget: each candidate's pre-allocation plan
    // is compared to the budget before any allocation, and a candidate whose
    // plan exceeds it is refused exactly as a refusing transport is -- logged with
    // the plan and the budget, and skipped so the ladder falls to the next
    // candidate. a budget that no candidate fits refuses the create before the
    // first allocation.

    uint64_t memory_budget = resolved_memory_budget ? *resolved_memory_budget : 0;
    if ( !resolved_memory_budget && !rocketnet_config_apply_env_uint64( "client", "memory_budget_bytes", &memory_budget ) )
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
        rocketnet_client_memory_plan_t plan;
        if ( rocketnet_client_transport_memory_plan( config, name, plan ) )
        {
            rocketnet_info( "client memory plan [%s]: %llu allocator bytes, %d sockets; excludes OS stacks, kernel queues, loader and application memory",
                            name, (unsigned long long) plan.total_bytes, plan.sockets );

            if ( memory_budget > 0 && plan.total_bytes > memory_budget )
            {
                rocketnet_warn( "client transport %s refused: memory plan %llu allocator bytes exceeds the %llu byte operator budget",
                                name, (unsigned long long) plan.total_bytes, (unsigned long long) memory_budget );

                budget_refused[candidate] = true;
                budget_refused_total++;
            }
            else
                budget_allowed++;
        }
        else
            rocketnet_info( "client memory plan [%s]: unavailable; consult this transport's resource policy", name );
    }

    if ( memory_budget > 0 && budget_refused_total > 0 && budget_allowed == 0 )
    {
        rocketnet_error( "client transport ladder exhausted: every candidate's memory plan exceeds the %llu byte operator budget",
                         (unsigned long long) memory_budget );

        if ( status )
            *status = ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED;

        return false;
    }

    out->config = effective_config;
    out->ladder = ladder;
    memcpy( out->budget_refused, budget_refused, sizeof( out->budget_refused ) );

    return true;
}

static rocketnet_client_transport_t * rocketnet_client_transport_create_internal( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status,
                                                                                  const rocketnet_client_transport_io_operations_t * step_driven_operations, void * step_driven_transport_context, size_t config_bytes, uint64_t test_timestamp,
                                                                                  const uint64_t * resolved_memory_budget )
{
#if !ROCKETNET_DEV
    (void) step_driven_operations;

    (void) step_driven_transport_context;
#endif // #if !ROCKETNET_DEV
    if ( status )
        *status = ROCKETNET_STATUS_OK;

    if ( !rocketnet_api_null_guard( connect_token ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_ARGUMENT;

        return NULL;
    }

    // the lifecycle state is read once here, before any allocation or socket;
    // a create before rocketnet_init is refused, logged and counted

    if ( !rocketnet_library_initialized() )
    {
        rocketnet_error( "rocketnet_client_transport_create: rocketnet_init must be called first" );

        rocketnet_api_misuse_record();

        if ( status )
            *status = ROCKETNET_STATUS_NOT_INITIALIZED;

        return NULL;
    }

    rocketnet_assert( connect_token );

    struct rocketnet_client_transport_resolved_t resolved;
    if ( !rocketnet_client_transport_resolve( config, config_bytes, resolved_memory_budget, &resolved, status ) )
        return NULL;

    config = &resolved.config;
    const struct rocketnet_transport_ladder_t ladder = resolved.ladder;
    const bool * budget_refused = resolved.budget_refused;

    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t *) rocketnet_malloc( context, sizeof( rocketnet_client_transport_t ) );
    if ( !client_transport )
    {
        rocketnet_error( "could not allocate client" );

        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

        return NULL;
    }

    rocketnet_client_transport_construct( client_transport );

    client_transport->context = context;

    // connect with the caller's token: the token names the server and is
    // the credential. the game owns the clock; with a configured epoch the
    // token check reads epoch plus the caller's time, and with the epoch zero
    // it reads unix time once, as before.

    rocketnet_client_protocol_init( &client_transport->protocol, &client_transport->counters );

    memcpy( client_transport->protocol.packet_filter, config->packet_filter, ROCKETNET_FILTER_VALUE_BYTES );

    // the create-time clock is checked the same way an update's is: a
    // non-finite or negative seed would poison every later comparison, so it
    // is clamped to the initial last good time before anything reads it.

    bool time_clamped = false;
    time = rocketnet_update_clock_advance( 0.0, &client_transport->raw_time, &client_transport->time_offset, time, &time_clamped );

    // the seed is the clock's origin and not a step: the raw time starts there, so the first update compares against the seed and not against zero

    client_transport->raw_time = time;
    client_transport->time_offset = 0.0;

    client_transport->config = *config;
    client_transport->time = time;
    client_transport->last_state = client_transport->protocol.state;

    const uint64_t token_timestamp = test_timestamp ? test_timestamp : rocketnet_platform_token_epoch( config->token_epoch_unix_seconds, time ) + (uint64_t) time;

    if ( !rocketnet_client_protocol_connect( &client_transport->protocol, connect_token, time, token_timestamp ) )
    {
        const int connect_failure = client_transport->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED ? ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED : ROCKETNET_STATUS_INVALID_CONNECT_TOKEN;

        rocketnet_client_transport_abort_create( client_transport );

        if ( status )
            *status = connect_failure;

        return NULL;
    }

    // The published development keys are public and not for production: an
    // implementation must warn on every start that uses them, at each end that
    // can tell. The client can tell only from a token sealed to the fixed test
    // server key; a public server's key is its own, so the server warns.
    // A production build (ROCKETNET_DEV=0) refuses such a token outright.

    if ( memcmp( client_transport->protocol.signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES ) == 0 )
    {
        rocketnet_warn( "development keys are public and not for production" );

        if ( rocketnet_development_values_refused() )
        {
            rocketnet_error( "connect token sealed to the development server key in a production build (ROCKETNET_DEV=0); refusing to connect" );

            rocketnet_client_transport_abort_create( client_transport );

            if ( status )
                *status = ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION;

            return NULL;
        }
    }

    // A token naming port 0 can never reach a server: macOS refuses the
    // connect, and Linux and Windows accept it and the packets reach no
    // server. Every flagged address is checked, so the refusal does not
    // depend on the host's IPv6.

    if ( rocketnet_signed_connect_data_names_port_zero( &client_transport->protocol.signed_fields ) )
    {
        rocketnet_error( "connect token names a server address with port 0; refusing to connect" );

        rocketnet_client_transport_abort_create( client_transport );

        if ( status )
            *status = ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO;

        return NULL;
    }

    // the socket binds after the token parse so the bind family can follow
    // the server and the host's IPv6 capability. A token naming both
    // families selects its IPv4 address on a host with no IPv6.

    const bool dual_stack = rocketnet_platform_client_dual_stack();
    const bool ipv6_available = rocketnet_platform_client_ipv6_available();

    rocketnet_client_protocol_pick_server_address( &client_transport->protocol.signed_fields, ipv6_available,
                                                   &client_transport->protocol.server_address );

    char string_buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    char address_buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client connecting to %s", rocketnet_address_to_string( &client_transport->protocol.server_address, string_buffer ) );

    // on macOS and iOS an IPv4 server's client binds IPv4. Darwin picks the
    // ephemeral port of a dual stack IPv6 socket without regard to the IPv4
    // sockets already bound, and delivers an IPv4 datagram to the IPv4 socket
    // on that port, so a dual stack client sharing a port never hears its
    // server. An IPv4 socket's ephemeral port is never one another IPv4 socket
    // holds.
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    const bool ipv4_server_dual_stack = false;
#else
    const bool ipv4_server_dual_stack = dual_stack;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS

    const int bind_family = rocketnet_platform_client_bind_family(
        client_transport->protocol.server_address.type, ipv6_available, ipv4_server_dual_stack );

    if ( bind_family == ROCKETNET_ADDRESS_NONE )
    {
        rocketnet_error( "client cannot connect: ipv6 is unavailable on this host" );

        rocketnet_client_transport_abort_create( client_transport );

        if ( status )
            *status = ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED;

        return NULL;
    }

    rocketnet_address_t bind_address;
    memset( &bind_address, 0, sizeof( bind_address ) );
    bind_address.type = (uint8_t) bind_family;

    // IMPORTANT: for many platforms it's best practice to bind to ipv6 and go dual stack on the client
    if ( bind_family == ROCKETNET_ADDRESS_IPV6 )
    {
        if ( dual_stack )
            rocketnet_info( "client socket is dual stack ipv4 and ipv6" );
        else
            rocketnet_info( "client socket is ipv6 only" );
    }
    else if ( client_transport->protocol.server_address.type == ROCKETNET_ADDRESS_IPV4 && dual_stack && !ipv6_available )
    {
        rocketnet_info( "client socket is ipv4 because ipv6 is unavailable on this host" );
    }

    // IMPORTANT: some platforms (GDK) have a preferred port that we must use to access packet tagging
    // If the bind address has set port of 0, substitute the preferred client port here
    if ( bind_address.port == 0 )
    {
        int preferred_client_port = rocketnet_platform_preferred_client_port();
        if ( preferred_client_port != 0 )
        {
            rocketnet_info( "client socket using preferred port %d", preferred_client_port );

            bind_address.port = (uint16_t) preferred_client_port;
        }
    }

    // A token naming both families on a host that can use both selected its
    // IPv6 server above. Its IPv4 server is the family race's second server
    // (below) and the family fallback's: a candidate whose IPv6 socket is
    // refused, at open or at connect, is opened again for the IPv4 server
    // before the ladder falls to the next candidate. The fallback binds IPv4,
    // as a host without IPv6 binds for an IPv4 server, so a host that refuses
    // IPv6 sockets does not refuse it too.

    const uint8_t token_flags = client_transport->protocol.signed_fields.address_flags;
    const bool both_families = client_transport->protocol.server_address.type == ROCKETNET_ADDRESS_IPV6 &&
                               ( token_flags & ROCKETNET_ADDRESS_FLAG_IPV4 ) != 0 && ( token_flags & ROCKETNET_ADDRESS_FLAG_IPV6 ) != 0;

    rocketnet_address_t ipv4_server;
    memset( &ipv4_server, 0, sizeof( ipv4_server ) );
    if ( both_families )
    {
        struct rocketnet_signed_connect_data_t ipv4_only = client_transport->protocol.signed_fields;
        ipv4_only.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
        rocketnet_client_protocol_pick_server_address( &ipv4_only, false, &ipv4_server );
    }

    rocketnet_address_t ipv4_bind = bind_address;
    ipv4_bind.type = (uint8_t) rocketnet_platform_client_bind_family( ROCKETNET_ADDRESS_IPV4, false, false );

    // THE LADDER, the client's short form: candidates tried left to
    // right, first grant wins, a one-element list is a pin that refuses
    // rather than falls, and the exhausted list fails loudly naming what
    // was skipped for platform and what refused for grant
    // (proofs/tla/RNClientLadder.tla).

    int granted_transport = -1;

    char refused_names[128];
    refused_names[0] = '\0';

    for ( int i = 0; i < ladder.num_candidates && granted_transport < 0; i++ )
    {
        const int candidate = ladder.candidates[i];

        // a candidate whose memory plan the operator budget refused was logged
        // and refused in the plan loop above; record it for the exhausted
        // summary and fall to the next candidate, never opening it

        if ( budget_refused[candidate] )
        {
            const size_t used = strlen( refused_names );
            snprintf( refused_names + used, sizeof( refused_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( candidate ) );
            continue;
        }

#if ROCKETNET_DEV
        if ( rocketnet_client_memory_plan_seam_enabled )
        {
            struct rocketnet_portable_seam_client_t
            {
                void * context;
                void * socket;
                void * wake;
            };
            rocketnet_portable_seam_client_t * seam_client = (rocketnet_portable_seam_client_t *)
                rocketnet_malloc( client_transport->context, sizeof( rocketnet_portable_io_client_layout_t ) );
            if ( !seam_client )
            {
                if ( status )
                    *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

                rocketnet_client_transport_abort_create( client_transport );
                return NULL;
            }

            seam_client->context = client_transport->context;
            seam_client->socket = rocketnet_malloc( client_transport->context, sizeof( rocketnet_platform_socket_t ) );
            seam_client->wake = rocketnet_malloc( client_transport->context, sizeof( rocketnet_platform_wake_layout_t ) );
            if ( !seam_client->socket || !seam_client->wake )
            {
                if ( seam_client->socket )
                    rocketnet_free( client_transport->context, seam_client->socket );
                if ( seam_client->wake )
                    rocketnet_free( client_transport->context, seam_client->wake );

                rocketnet_free( client_transport->context, seam_client );

                if ( status )
                    *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

                rocketnet_client_transport_abort_create( client_transport );
                return NULL;
            }

            static const rocketnet_client_transport_io_operations_t seam_transport_ops = {
                NULL,
                []( void * transport )
                {
                    rocketnet_portable_seam_client_t * client = (rocketnet_portable_seam_client_t *) transport;
                    if ( client )
                    {
                        if ( client->wake )
                            rocketnet_free( client->context, client->wake );
                        if ( client->socket )
                            rocketnet_free( client->context, client->socket );
                        rocketnet_free( client->context, client );
                    }
                },
                []( void *, const rocketnet_address_t *, const uint8_t *, int ) {},
                []( void *, rocketnet_address_t *, uint8_t *, int ) -> int
                { return 0; },
                []( void *, const rocketnet_platform_send_t *, int ) {},
                []( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) -> int
                { return 0; },
                []( void *, bool, double ) -> bool
                { return false; },
                []( void * ) {},
                []( void * ) -> uint64_t
                { return 0; },
                NULL,
                NULL
            };
            client_transport->io.operations = &seam_transport_ops;
            client_transport->io.instance = seam_client;
            granted_transport = candidate;
            continue;
        }
#endif // #if ROCKETNET_DEV

        {
            const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( candidate );
            int transport_status = ROCKETNET_STATUS_OK;
            const rocketnet_client_transport_io_config_t transport_config = { rocketnet_transport_name( candidate ), config, &transport_status };
#if ROCKETNET_DEV
            // the step-driven create (unit tests) opens the caller's operations in place of the candidate's backend

            const rocketnet_client_transport_io_operations_t * operations = step_driven_operations ? step_driven_operations : ( definition && definition->backend ? definition->backend->client : NULL );
            void * operations_context = step_driven_operations ? step_driven_transport_context : client_transport->context;
#else
            const rocketnet_client_transport_io_operations_t * operations = definition && definition->backend ? definition->backend->client : NULL;
            void * operations_context = client_transport->context;
#endif // #if ROCKETNET_DEV

            // the client trusts only the server (TRANSPORT.md t11): a
            // transport with a socket connects it to the server the token
            // selected, before anything is sent or received, so the kernel
            // drops datagrams from any other source. the source check in
            // rocketnet_client_process_packet stays behind it. a connect the
            // kernel refuses is a refusal (proofs/tla/RNClientLadder.tla
            // TryPrimary).

            if ( rocketnet_client_transport_open_connected( operations, operations_context, &transport_config, &bind_address,
                                                            &client_transport->protocol.server_address, &client_transport->io ) )
            {
                granted_transport = candidate;
            }
            else if ( both_families && transport_status != ROCKETNET_STATUS_OUT_OF_MEMORY )
            {
                // the family fallback (proofs/tla/RNClientLadder.tla
                // TryFallback): the same candidate, for the token's IPv4
                // server. A refusal for want of memory never falls to it.

                rocketnet_warn( "client transport %s refused the IPv6 server %s. trying the IPv4 server %s",
                                rocketnet_transport_name( candidate ),
                                rocketnet_address_to_string( &client_transport->protocol.server_address, string_buffer ),
                                rocketnet_address_to_string( &ipv4_server, address_buffer ) );

                if ( rocketnet_client_transport_open_connected( operations, operations_context, &transport_config, &ipv4_bind,
                                                                &ipv4_server, &client_transport->io ) )
                {
                    client_transport->protocol.server_address = ipv4_server;
                    bind_address = ipv4_bind;
                    granted_transport = candidate;
                }
            }

            // a rung that failed for want of memory is not a refusal: the create
            // ends with OUT_OF_MEMORY and never falls to the next candidate
            // or family (rocketnet_client_transport_io_config_t.status,
            // sdk/rocketnet_driver.h, proofs/tla/RNClientLadder.tla NoTryAfterOom);
            // any other failed open is a refusal and falls

            if ( granted_transport < 0 && transport_status == ROCKETNET_STATUS_OUT_OF_MEMORY )
            {
                rocketnet_error( "client transport create: the %s transport could not allocate its endpoint", rocketnet_transport_name( candidate ) );

                rocketnet_client_transport_abort_create( client_transport );

                if ( status )
                    *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

                return NULL;
            }

            if ( granted_transport < 0 )
            {
                rocketnet_warn( "client transport %s refused%s", rocketnet_transport_name( candidate ),
                                i + 1 < ladder.num_candidates ? ". trying the next candidate" : "" );

                const size_t used = strlen( refused_names );
                snprintf( refused_names + used, sizeof( refused_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( candidate ) );
            }
        }
    }

    if ( granted_transport < 0 )
    {
        char skipped_names[128];
        skipped_names[0] = '\0';

        for ( int i = 0; i < ladder.num_skipped; i++ )
        {
            const size_t used = strlen( skipped_names );
            snprintf( skipped_names + used, sizeof( skipped_names ) - used, "%s%s", used > 0 ? ", " : "", rocketnet_transport_name( ladder.skipped[i] ) );
        }

        rocketnet_error( "client transport ladder exhausted with no grant. refused: %s. skipped for platform: %s",
                         refused_names[0] != '\0' ? refused_names : "(none)",
                         skipped_names[0] != '\0' ? skipped_names : "(none)" );

        rocketnet_client_transport_abort_create( client_transport );

        if ( status )
            *status = ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED;

        return NULL;
    }

    client_transport->transport = granted_transport;

    // the family race (TRANSPORT.md t11 rule 3, proofs/tla/ServerConnection.tla
    // RaceFamilies): a token naming both families on a host that can use
    // both selected its IPv6 server above, and when the ladder granted that
    // server the IPv4 server is dialed too, on a second socket of the
    // granted transport, bound with the family an IPv4 server binds today
    // and connected to that server. A ladder that fell back to the IPv4
    // server races nothing. Both sockets are opened here, with the connect;
    // nothing is opened during update. A host whose second socket is
    // refused races nothing and connects exactly as a one-family client; a
    // refusal for want of memory ends the create, as for the first socket.
    // The memory plan seam opens no socket and races nothing.

    bool race_ipv4 = both_families && client_transport->protocol.server_address.type == ROCKETNET_ADDRESS_IPV6;
#if ROCKETNET_DEV
    if ( rocketnet_client_memory_plan_seam_enabled )
        race_ipv4 = false;
#endif // #if ROCKETNET_DEV
    if ( race_ipv4 )
    {
        const rocketnet_address_t race_server = ipv4_server;

        rocketnet_address_t race_bind;
        memset( &race_bind, 0, sizeof( race_bind ) );
        race_bind.type = (uint8_t) rocketnet_platform_client_bind_family( ROCKETNET_ADDRESS_IPV4, ipv6_available, ipv4_server_dual_stack );

        const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( granted_transport );
#if ROCKETNET_DEV
        const rocketnet_client_transport_io_operations_t * operations = step_driven_operations ? step_driven_operations : ( definition && definition->backend ? definition->backend->client : NULL );
        void * operations_context = step_driven_operations ? step_driven_transport_context : client_transport->context;
#else
        const rocketnet_client_transport_io_operations_t * operations = definition && definition->backend ? definition->backend->client : NULL;
        void * operations_context = client_transport->context;
#endif // #if ROCKETNET_DEV
        int race_status = ROCKETNET_STATUS_OK;
        const rocketnet_client_transport_io_config_t race_config = { rocketnet_transport_name( granted_transport ), config, &race_status };
        rocketnet_client_transport_io_t race_io = {};
        rocketnet_client_transport_open_connected( operations, operations_context, &race_config, &race_bind, &race_server, &race_io );

        rocketnet_client_race_t * race = NULL;
        bool race_out_of_memory = race_status == ROCKETNET_STATUS_OUT_OF_MEMORY;

        if ( race_io.instance )
        {
            race = (rocketnet_client_race_t *) rocketnet_malloc( context, sizeof( rocketnet_client_race_t ) );
            if ( !race )
            {
                rocketnet_client_transport_io_close( &race_io );

                race_out_of_memory = true;
            }
        }

        if ( race_out_of_memory )
        {
            rocketnet_error( "client transport create: the %s transport could not allocate its second family's endpoint", rocketnet_transport_name( granted_transport ) );

            rocketnet_client_transport_abort_create( client_transport );

            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

            return NULL;
        }

        if ( race )
        {
            new ( race ) rocketnet_client_race_t();

            const rocketnet_client_transport_io_operations_t * inner = client_transport->io.operations;
            race->operations = *inner;
            race->operations.destroy = rocketnet_client_race_destroy;
            race->operations.send = rocketnet_client_race_send;
            race->operations.receive = rocketnet_client_race_receive;
            race->operations.send_batch = inner->send_batch ? rocketnet_client_race_send_batch : NULL;
            race->operations.receive_batch = inner->receive_batch ? rocketnet_client_race_receive_batch : NULL;
            race->operations.wait = inner->wait ? rocketnet_client_race_wait : NULL;
            race->operations.wake = inner->wake ? rocketnet_client_race_wake : NULL;
            race->operations.send_failures = inner->send_failures ? rocketnet_client_race_send_failures : NULL;
            race->operations.connect = NULL;    // both sockets are connected above
            race->operations.close = NULL;      // the race closes its losing family itself
            race->family[0] = client_transport->io;
            race->family[1] = race_io;
            race->server_address[0] = client_transport->protocol.server_address;
            race->server_address[1] = race_server;
            race->committed.store( -1, std::memory_order_relaxed );

            race->closed = false;
            race->closed_send_failures.store( 0, std::memory_order_relaxed );

            race->context = context;
            client_transport->io.operations = &race->operations;
            client_transport->io.instance = race;

            rocketnet_info( "client races both families: %s too", rocketnet_address_to_string( &race_server, string_buffer ) );
        }
        else
        {
            rocketnet_info( "client races no family: the second family's socket was refused" );
        }
    }

    if ( refused_names[0] != '\0' )
    {
        rocketnet_info( "client transport ladder granted %s after refusals: %s", rocketnet_transport_name( granted_transport ), refused_names );
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client bound to %s [%s]", rocketnet_address_to_string( &bind_address, address_string ), rocketnet_transport_name( granted_transport ) );

    client_transport->bound_port = bind_address.port;
    client_transport->link_simulator_from = bind_address;

    // A development build creates the link simulator for every connection,
    // memory up front; all-zero settings are a pass-through that neither adds
    // latency nor copies. A production library creates none and carries none
    // of its memory (rocketnet_network_simulator_create is a stub returning
    // NULL there, and the config walk already refuses nonzero simulated values).

    if ( rocketnet_public_link_simulator_development() )
    {
        client_transport->link_simulator = rocketnet_network_simulator_create( context, ROCKETNET_CLIENT_LINK_SIMULATOR_SLOTS, UINT64_C( 0x434c49454e545349 ) );
        if ( !client_transport->link_simulator )
        {
            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

            rocketnet_client_transport_abort_create( client_transport );
            return NULL;
        }

        rocketnet_network_simulator_config( client_transport->link_simulator,
                                            config->simulated_latency_ms / 1000.0,
                                            config->simulated_jitter_ms / 1000.0,
                                            (float) config->simulated_loss_percent,
                                            (float) config->simulated_duplicate_percent );

        if ( !rocketnet_network_simulator_patterns( client_transport->link_simulator,
                                                    config->simulated_drop_every_nth,
                                                    config->simulated_reverse_window ) )
        {
            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

            rocketnet_client_transport_abort_create( client_transport );
            return NULL;
        }
    }

    rocketnet_buffer_handoff_init( &client_transport->receive_buffer_index.handoff, 0 );

    client_transport->receive_buffer_index.fill = 0;

    if ( !rocketnet_platform_mutex_create( &client_transport->mutex ) )
    {
        rocketnet_error( "client could not create mutex" );

        rocketnet_client_transport_abort_create( client_transport );

        if ( status )
            *status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;

        return NULL;
    }

    // the clock (docs/CLIENT-IO.md _The clock_): read once here; NULL means rocketnet_platform_time

    client_transport->time_callback = config->time_callback ? config->time_callback : rocketnet_client_platform_time;

    // a transport with wait and wake runs the I/O thread design of
    // docs/CLIENT-IO.md through rocketnet_client_io_t, modelled by
    // proofs/tla/RNClientIo.tla (every RNClientIo action named here). Built-in
    // loopback uses the same core with bounded worker polling. Custom drivers
    // without wait/wake keep the BASE path: per-packet sends on the main
    // thread and a polling receive worker that exchanges buffer ownership
    // under a short mutex.
    // Threading is decided at create and never moves: the transport alone
    // decides it, and the link simulator never changes the threading of a
    // running connection.  The simulator is in the send path either way: in
    // the I/O ring path the hand-off hands the sealed packets to it, and in
    // the per-packet path the send does.
    // A connection created with a simulator setting runs no network thread:
    // the simulator holds its packets and the caller sends them, one by one,
    // on its own thread.  That was decided when the simulator was configured
    // and it never moves: a live setting change does not move a running
    // connection between the two paths.

    client_transport->io_thread = !rocketnet_network_simulator_created_active( client_transport->link_simulator ) &&
#if ROCKETNET_DEV
                                  ( rocketnet_client_transport_uses_io_thread( client_transport->io.operations ) ||
                                    ( !step_driven_operations && granted_transport == ROCKETNET_TRANSPORT_LOOPBACK ) );
#else
                                  ( rocketnet_client_transport_uses_io_thread( client_transport->io.operations ) ||
                                    granted_transport == ROCKETNET_TRANSPORT_LOOPBACK );
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
    // the step-driven create (unit tests) runs no thread on either path

    client_transport->step_driven = step_driven_operations != NULL;
    client_transport->step_in_update = config->step_driven != 0;

    if ( client_transport->step_in_update )
    {
        // the public step-driven mode needs a transport whose receive returns at
        // once when drained: the I/O-thread design's wait and wake, or the
        // loopback transport's poll, or a batched receive. A transport without one
        // cannot be stepped on the caller's thread, so the create refuses it.
        const rocketnet_client_transport_io_operations_t * operations = client_transport->io.operations;
        const bool nonblocking_step = ( operations->wait && operations->wake ) || operations->receive_batch ||
                                      granted_transport == ROCKETNET_TRANSPORT_LOOPBACK;
        if ( !nonblocking_step )
        {
            rocketnet_error( "client config step_driven requires a transport with a non-blocking I/O step: %s has none",
                             rocketnet_transport_name( granted_transport ) );

            client_transport->step_in_update = false;
            rocketnet_client_transport_abort_create( client_transport );

            if ( status )
                *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;

            return NULL;
        }
    }
#else
    client_transport->step_driven = false;
    client_transport->step_in_update = false;
#endif // #if ROCKETNET_DEV

    // proofs/tla/RNClientIoShutdown.tla Start: create starts the worker generation
    // below, the I/O thread or the legacy receive thread.
    // proofs/tla/RNClientIo.tla OnThread: outside the step-driven mode the I/O thread
    // created here runs every core action.
    // proofs/tla/RNClientIo.tla IoSend, IoFinishRing, IoReceive, IoPublish, IoOwe,
    // IoWait, IoWake, IoClearWake and IoExit: the I/O thread's step sends each
    // handed-off ring, drains datagrams into its buffer and publishes it, waits
    // and wakes, and exits once quit is stored.
    // proofs/tla/RNClientIoShutdown.tla TakeRing and FinishRing: the worker takes the
    // handed-off ring and marks it sent when every slot left.

    if ( client_transport->io_thread || client_transport->step_in_update )
    {
        rocketnet_client_io_ring_view_t views[ROCKETNET_CLIENT_IO_NUM_RINGS];

        for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
        {
            views[i].data = client_transport->payload_ring[i].data;
            views[i].frame_stride = ROCKETNET_FRAME_STRIDE;
            views[i].wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
            views[i].wire_bytes = (const uint8_t *) &client_transport->payload_ring[i].info[0].wire_bytes;
            views[i].wire_bytes_stride = sizeof( struct rocketnet_client_payload_slot_info_t );
        }

        rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS];

        for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
            buffers[i] = &client_transport->receive_buffer[i];

        rocketnet_client_io_init( &client_transport->client_io, &client_transport->io, &client_transport->mutex, views, buffers,
                                  client_transport->time_callback, client_transport->context );

        client_transport->client_io.prefilter_too_large =
            &client_transport->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];

        // proofs/tla/RNClientIoDecrypt.tla PublishKeys and RetireKeys: the protocol
        // publishes the open copy at the challenge and retires it at every
        // terminal state; destroy retires it too

        client_transport->protocol.publish_keys = rocketnet_client_transport_publish_keys;
        client_transport->protocol.retire_keys = rocketnet_client_transport_retire_keys;
        client_transport->protocol.keys_context = client_transport;

#if ROCKETNET_DEV
        if ( client_transport->step_driven || client_transport->step_in_update )
            return client_transport;
#endif // #if ROCKETNET_DEV

        client_transport->thread = rocketnet_client_thread_create( context, rocketnet_client_io_thread_function, &client_transport->client_io, status );
    }
#if ROCKETNET_DEV
    else if ( client_transport->step_driven )
        return client_transport;
#endif // #if ROCKETNET_DEV
    else
    {
        client_transport->thread = rocketnet_client_thread_create( context, rocketnet_client_thread_function, client_transport, status );
    }

    if ( !client_transport->thread )
    {
        rocketnet_error( "client could not create thread" );

        rocketnet_client_transport_abort_create( client_transport );

        rocketnet_create_status( status, ROCKETNET_STATUS_THREAD_CREATE_FAILED );

        return NULL;
    }

    // the client network thread (the I/O thread or the legacy thread) takes the client thread priority from this thread, right after its create

#if ROCKETNET_DEV
    if ( rocketnet_client_memory_plan_seam_enabled )
        return client_transport;
#endif // #if ROCKETNET_DEV

    rocketnet_platform_client_thread_priority( client_transport->thread );

    return client_transport;
}

// The destination of the Filling ring's committed slots: the protocol's
// server address while it holds one; after disconnect resets it, the address
// recorded on the ring by the deferred hand-off. Retrying during update,
// send_packets or destroy therefore sends the burst to the server for which
// it was sealed.

static const rocketnet_address_t * rocketnet_client_transport_fill_destination( rocketnet_client_transport_t * client_transport )
{
    if ( client_transport->protocol.server_address.type != ROCKETNET_ADDRESS_NONE )
        return &client_transport->protocol.server_address;

    return &client_transport->client_io.to[client_transport->client_io.send_fill];
}

// proofs/tla/RNClientIoShutdown.tla abstracts this request/completion ownership;
// native backend and thread behavior still need their platform gates.
// Requests never join, call a backend send, or wait for a ring. The final
// filling ring is published without reopening the other ring: no producer may
// touch either ring after stop, and the worker can drain both independently.
// proofs/tla/RNClientIoShutdown.tla RequestStop, RepeatStop and RequestReturn: the
// first call publishes the final filling ring with quit, a repeat returns at
// once, and neither waits on the worker.
// proofs/tla/RNClientIo.tla Stop: stores quit and wakes the I/O thread
// (rocketnet_client_io_stop).
// proofs/tla/RNBufferHandoff.tla MainStop: hands the Filling ring to the worker with
// the quit store.
// proofs/tla/RNClientLinkStop.tla Stop: on the legacy path publishes quit without
// draining the link simulator or calling the backend (the worker's
// completion drains it); on the I/O-core path, where the caller keeps the
// simulator, sends every copy it holds and then publishes quit.

// proofs/tla/RNClientRace.tla CallerStop: the reading thread is asked to exit; it
// still owns the losing family's handles until it has.

void rocketnet_client_transport_stop( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) || client_transport->stop_requested )
        return;

    client_transport->stop_requested = true;
    client_transport->packet_leases.reset();

    if ( rocketnet_client_io_core( client_transport ) )
    {
        // proofs/tla/RNClientLinkStop.tla Stop, path iocore: the I/O worker never
        // owns the link simulator, so the packets it holds leave here, with
        // the stop, ahead of their latency
        rocketnet_client_link_drain( client_transport, DBL_MAX );

        rocketnet_client_io_t * io = &client_transport->client_io;
        uint64_t committed = rocketnet_client_fill_ring( client_transport )->num_packets;
        if ( committed > ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
            committed = ROCKETNET_CLIENT_PAYLOAD_RING_SIZE;

        rocketnet_client_io_retire_keys( io );

        rocketnet_client_io_stop( io, committed, rocketnet_client_transport_fill_destination( client_transport ) );

#if ROCKETNET_DEV
        if ( rocketnet_client_memory_plan_seam_enabled )
            io->stopped.store( true, std::memory_order_release );
#endif // #if ROCKETNET_DEV
    }
    else
    {
        client_transport->quit.store( true, std::memory_order_release );

        bool stopped_now = !client_transport->thread && !client_transport->link_simulator && !client_transport->stop_burst_pending.load( std::memory_order_acquire );
#if ROCKETNET_DEV
        if ( rocketnet_client_memory_plan_seam_enabled )
            stopped_now = true;
#endif // #if ROCKETNET_DEV
        if ( stopped_now )
            client_transport->stopped.store( true, std::memory_order_release );
    }
}

// proofs/tla/RNClientIoShutdown.tla BeginPoll and PollReturn: reads the worker's
// completion flag and returns without waiting.

// proofs/tla/RNClientRace.tla ReaderExit: that flag is the reading thread's exit.

bool rocketnet_client_transport_stopped( const rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return false;
    if ( !client_transport->stop_requested )
        return false;

    return rocketnet_client_io_core( client_transport ) ? client_transport->client_io.stopped.load( std::memory_order_acquire ) : client_transport->stopped.load( std::memory_order_acquire );
}

static void rocketnet_client_transport_flush_disconnect( rocketnet_client_transport_t * client_transport )
{
    if ( !client_transport->stop_burst_pending.load( std::memory_order_acquire ) )
        return;
    for ( int i = 0; i < client_transport->stop_burst_count; ++i )
        client_transport->io.operations->send( client_transport->io.instance, &client_transport->stop_address,
                                               client_transport->stop_burst + i * ROCKETNET_SEND_BUDGET, client_transport->stop_burst_bytes );

    client_transport->stop_burst_pending.store( false, std::memory_order_release );
}

// proofs/tla/RNClientIoShutdown.tla WorkerQuiesce: the legacy worker sends what it
// still holds, then stores stopped.

static void rocketnet_client_transport_finish_stop( rocketnet_client_transport_t * client_transport )
{
    rocketnet_client_transport_flush_disconnect( client_transport );

    rocketnet_client_link_drain_at_stop( client_transport );

    client_transport->stopped.store( true, std::memory_order_release );
}

#if ROCKETNET_DEV
int rocketnet_client_transport_secret_spans_for_test( size_t * offsets, size_t * bytes, int max )
{
    const size_t protocol = offsetof( rocketnet_client_transport_t, protocol );
    const size_t open = offsetof( rocketnet_client_transport_t, client_io ) + offsetof( rocketnet_client_io_t, open );
    const size_t spans[5][2] = {
        { protocol + offsetof( rocketnet_client_protocol_t, keys ), sizeof( rocketnet_connection_keys_t ) },
        { protocol + offsetof( rocketnet_client_protocol_t, token ), sizeof( rocketnet_connect_token_t ) },
        { protocol + offsetof( rocketnet_client_protocol_t, client_ephemeral_secret_key ), ROCKETNET_KEY_BYTES },
        { protocol + offsetof( rocketnet_client_protocol_t, session_nonce ), ROCKETNET_SESSION_NONCE_BYTES },
        { open + offsetof( rocketnet_client_io_open_t, session_server_to_client ), ROCKETNET_KEY_BYTES },
    };
    int count = 0;
    for ( ; count < 5 && count < max; count++ )
    {
        offsets[count] = spans[count][0];
        bytes[count] = spans[count][1];
    }

    return count;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNClientIo.tla Join and Free: the I/O thread is joined before the
// transport is freed.

// proofs/tla/RNClientIoShutdown.tla DestroyBegin, Join and Free: destroy refuses a
// transport not yet stopped, joins the worker thread, then frees.
// proofs/tla/RNClientIo.tla DestroyBegin, DestroyWait, DestroyWaitDone, Timeout and
// Done: destroy makes its last hand-off, waits at the deadline for the other
// ring to reach Sent, drops the committed slots it cannot hand off, joins,
// frees and ends.
// proofs/tla/RNClientIoShutdown.tla BeginRestart: a restart joins the prior worker
// generation before the next one runs, as the synchronous destroy and the
// next connect do here.
// proofs/tla/RNClientIoShutdown.tla Done: the freed endpoint's terminal state.

// proofs/tla/RNClientRace.tla CallerJoin and CallerDestroy: the reading thread is
// joined, then the caller's thread frees both race families, the losing
// one's handles already closed by the reading thread.

void rocketnet_client_transport_destroy( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;

    rocketnet_assert( rocketnet_client_transport_stopped( client_transport ) );

    if ( !rocketnet_client_transport_stopped( client_transport ) )
        return;

    if ( client_transport->thread )
        rocketnet_client_thread_release( client_transport->context, client_transport->thread );

    rocketnet_client_transport_io_close( &client_transport->io );

    if ( client_transport->link_simulator )
    {
        rocketnet_network_simulator_destroy( client_transport->link_simulator );
    }

    rocketnet_platform_mutex_destroy( &client_transport->mutex );

    // the keys, and only the keys: the protocol's and the I/O thread's open
    // copy (joined above). Every other byte goes back as it is; the OS
    // zero-fills a page before another process gets it.

    rocketnet_client_protocol_wipe_secrets( &client_transport->protocol );

    sodium_memzero( client_transport->client_io.open.session_server_to_client, sizeof( client_transport->client_io.open.session_server_to_client ) );

    void * allocation_context = client_transport->context;

    client_transport->~rocketnet_client_transport_t();

    rocketnet_free( allocation_context, client_transport );
}

static void rocketnet_client_process_packet( rocketnet_client_transport_t * client_transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes, double current_time,
                                             uint8_t verdict, uint64_t sequence )
{
    // only the dialed server is listened to; everything else is junk. While
    // the attempt holds a server address every such drop is counted. Once a
    // terminal state ends the attempt, the reset clears the address
    // (sdk/rocketnet_client_protocol.cpp:56-57,
    // sdk/rocketnet_connection_reset.cpp:42), and the server's own
    // redundant disconnect packets still in flight are dropped uncounted.

    // While the family race is in flight the other family's server is
    // dialed too (proofs/tla/ServerConnection.tla InFlight); once the race commits
    // it is foreign like any other source, so a late answer from it is
    // dropped and counted here.

    rocketnet_client_race_t * race = rocketnet_client_race_of( client_transport );
    const bool racing = race && race->committed.load( std::memory_order_relaxed ) < 0 &&
                        client_transport->protocol.server_address.type != ROCKETNET_ADDRESS_NONE;
    const bool from_second = racing && rocketnet_address_equal( from, &race->server_address[1] );

    if ( !from_second && !rocketnet_address_equal( from, &client_transport->protocol.server_address ) )
    {
        if ( client_transport->protocol.server_address.type != ROCKETNET_ADDRESS_NONE )
            client_transport->counters.values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE]++;

        return;
    }

    const int state_before = client_transport->protocol.state;

    struct rocketnet_client_received_packets_t * received = &client_transport->received_packets;

    uint8_t * payload = received->num_packets < ROCKETNET_NUM_CLIENT_PACKETS ? received->data + received->num_packets * ROCKETNET_FRAME_SIZE : NULL;

    uint8_t overflow[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    // proofs/tla/RNClientIoDecrypt.tla UpdateConsume: a record the I/O thread opened
    // or refused is consumed with its verdict; a raw one is processed as at
    // BASE
    // proofs/tla/SessionAcks.tla RecvData: the replay check, then the payload opens

    rocketnet_client_protocol_process_record( &client_transport->protocol, packet_data, packet_bytes, verdict, sequence,
                                              current_time,
                                              payload ? payload : overflow,
                                              payload ? ROCKETNET_FRAME_SIZE : ROCKETNET_RECEIVE_CEILING,
                                              &payload_bytes );

    // the first valid cookie moves the client out of requesting cookie: the
    // race commits to the family it arrived on (proofs/tla/ClientConnection.tla
    // ClientRecvCookie). Nothing else leaves requesting cookie forward.

    if ( racing && state_before == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE &&
         client_transport->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
        rocketnet_client_race_commit( client_transport, race, from_second ? 1 : 0 );

    const uint64_t session_sequence = client_transport->protocol.received_session_sequence;

    if ( payload_bytes > 0 && payload )
    {
        const int index = received->num_packets;
        received->packet_data[index] = payload;
        received->packet_bytes[index] = payload_bytes;
        received->sequence[index] = session_sequence;
        received->num_packets++;
    }
}

// RNClientIo HandOff and Defer; main thread, I/O thread path only, with the
// fills joined. The stats count a slot at the hand-off attempt that first sees
// it committed (stats_counted keeps a deferred slot from counting twice), not
// when the I/O thread sends it (docs/CLIENT-IO.md _Reuse only after sent; a hand-off never
// waits_). The committed slots of the Filling ring are counted into the stats
// once, then handed to the I/O thread. A ROCKETNET_CLIENT_RINGS_LATE result is
// the deferred hand-off: it counts CLIENT_SEND_DEFERRED and returns at once,
// the committed slots staying in the Filling ring, open to the game threads,
// for the next hand-off from send_packet, send_packets or update. No spin, no
// yield, no retry; a ring the I/O thread holds is never reused
// (HandOffNeverWaits, NoReuseBeforeSent).

// proofs/tla/RNClientIo.tla HandOffTo, DeferTo and Reopen: hands the Filling ring to
// the I/O thread, or counts the deferral when the other ring is busy; after a
// hand-off it empties the reopened ring send_fill now names.
// proofs/tla/RNClientIo.tla WaitDone: the reversed witness's blocked main resumes once
// the other ring is Sent; the fixed main never waits for the I/O thread.

static void rocketnet_client_transport_io_hand_off( rocketnet_client_transport_t * client_transport, const rocketnet_address_t * destination = NULL )
{
    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_fill_ring( client_transport );

    uint64_t committed = ring->num_packets;
    if ( committed > ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
        committed = ROCKETNET_CLIENT_PAYLOAD_RING_SIZE;

    if ( !destination )
        destination = rocketnet_client_transport_fill_destination( client_transport );

    const int stats_index = client_transport->current_stats_index;
    const rocketnet_address_t * to = destination ? destination : rocketnet_client_transport_fill_destination( client_transport );
    const int address_type = to->type;

    for ( uint64_t i = client_transport->stats_counted; i < committed; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            client_transport->stats_frames[stats_index].packets_sent++;
            client_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + rocketnet_wire_header_bytes( address_type ) );
            client_transport->client_stats.packets_sent++;
            client_transport->client_stats.bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + rocketnet_wire_header_bytes( address_type ) );
        }
    }

    client_transport->stats_counted = committed;

    if ( committed == 0 )
        return;

    // The simulator takes the sealed packets instead of the I/O thread when
    // its setting is active: the hand-off is the last place the caller
    // thread sees them.  The setting the caller handed over is applied
    // here, outside the lock, before the answer is read.

    if ( rocketnet_client_link_active( client_transport ) )
    {
        rocketnet_client_link_hold( client_transport, to );
        return;
    }

    const int result = rocketnet_client_io_hand_off( &client_transport->client_io, committed, to );

    if ( result == ROCKETNET_CLIENT_RINGS_HANDED_OFF )
    {
        // RNClientIo Reopen ran inside the hand-off: the ring send_fill now names is Filling and empty
        rocketnet_client_fill_ring( client_transport )->num_packets = 0;

        client_transport->stats_counted = 0;
    }
    else if ( result == ROCKETNET_CLIENT_RINGS_LATE )
    {
        client_transport->counters.values[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED]++;
    }
}

// RNClientIo Write and Commit for a packet the main thread built: the
// protocol packet of update and the disconnect burst are sealed into a slot of
// the Filling ring like a payload (docs/CLIENT-IO.md _The protocol packet and
// disconnect_), and leave with the next hand-off. No transport send on the main
// thread in the I/O thread path. A full Filling ring drops the packet and
// counts SEND_RING_FULL, as start_packet refuses a payload.

static void rocketnet_client_transport_io_commit_packet( rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( client_transport && packet_data && packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    if ( !client_transport || !packet_data || packet_bytes <= 0 || packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return;

    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_fill_ring( client_transport );

    uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( slot >= ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
    {
        rocketnet_spam( "client payload ring full. protocol packet dropped" );

        rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return;
    }

    memcpy( ring->data + slot * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET, packet_data, (size_t) packet_bytes );
    ring->info[slot].wire_bytes = packet_bytes;
}

// Try once at each caller handoff. Unit tests can refuse either phase without
// starting a competing thread or depending on scheduler timing.
// proofs/tla/RNBufferHandoff.tla MainTryLock: main takes a free mutex or skips this
// exchange, never waiting.
// proofs/tla/RNBufferHandoff.tla MainLockWait: the reversed witness's blocked main
// waits for a held mutex; the fixed main only try-acquires and never waits.
// proofs/tla/RNBufferHandoffBase.tla MainTry: the try-lock of update's expose and
// return phases.

static bool rocketnet_client_legacy_try_acquire( rocketnet_client_transport_t * client_transport, unsigned phase )
{
#if ROCKETNET_DEV
    if ( client_transport->receive_buffer_index.test_busy_mask & phase )
        return false;
#else
    (void) phase;
#endif // #if ROCKETNET_DEV
    return rocketnet_platform_mutex_try_acquire( &client_transport->mutex );
}

// Called with the mutex held. The counts were cleared while the caller still
// owned the buffers; publishing their indices is the only ownership transfer.

static void rocketnet_client_legacy_publish_returns( rocketnet_client_legacy_receive_state_t * state )
{
    for ( int i = 0; i < state->returned_count; ++i )
        rocketnet_buffer_handoff_release( &state->handoff, state->returned[i] );

    state->returned_count = 0;
}

// Take the oldest READY buffers that fit the packet budget. If the worker
// owns the mutex, leave everything in place and continue this update without
// receiving. Returning previously processed buffers precedes taking new ones.
// proofs/tla/RNBufferHandoffBase.tla Expose: returns the deferred cleared buffers and
// takes the oldest READY prefix within the packet budget.
// proofs/tla/RNBufferHandoff.tla ConsumerExchange and Unlock: main's release-and-take
// index exchange under the mutex, then the release.

static int rocketnet_client_update_expose_legacy_receive( rocketnet_client_transport_t * client_transport, int * exposed )
{
    if ( !rocketnet_client_legacy_try_acquire( client_transport, 1 ) )
        return 0;

    rocketnet_client_legacy_receive_state_t * state = &client_transport->receive_buffer_index;
    rocketnet_client_legacy_publish_returns( state );

    int num_exposed = 0;
    int num_packets = 0;

    while ( state->handoff.ready_count > 0 )
    {
        const int packets = client_transport->receive_buffer[state->handoff.ready_index[state->handoff.ready_head]].num_packets;
        if ( num_exposed > 0 && num_packets + packets > ROCKETNET_NUM_CLIENT_PACKETS )
            break;

        exposed[num_exposed++] = rocketnet_buffer_handoff_take( &state->handoff );
        num_packets += packets;
    }

    rocketnet_platform_mutex_release( &client_transport->mutex );

    return num_exposed;
}

// Keep processed buffers caller-owned if the worker has the mutex. They are
// neither READY nor FREE, so neither side can process or overwrite them again.
// proofs/tla/RNBufferHandoffBase.tla ReleaseOnly: clears the processed buffers
// outside the mutex and returns them without taking any.

static void rocketnet_client_update_return_legacy_receive( rocketnet_client_transport_t * client_transport, const int * exposed, int num_exposed )
{
    if ( num_exposed == 0 )
        return;

    rocketnet_client_legacy_receive_state_t * state = &client_transport->receive_buffer_index;
    rocketnet_assert( state->returned_count == 0 );

    for ( int i = 0; i < num_exposed; i++ )
    {
        client_transport->receive_buffer[exposed[i]].num_packets = 0;
        state->returned[state->returned_count++] = exposed[i];
    }

    if ( !rocketnet_client_legacy_try_acquire( client_transport, 2 ) )
        return;

    rocketnet_client_legacy_publish_returns( state );

    rocketnet_platform_mutex_release( &client_transport->mutex );
}

// Process previous stats frames into published per-second numbers, then
// advance the bucket when due. RTT, jitter and loss live in the protocol ACK
// endpoint and are read by the getter; the windowed rates and bandwidth are
// computed here once per update.

static void rocketnet_client_update_stats( rocketnet_client_transport_t * client_transport, double time )
{
    // process previous stats frames

    {
        uint64_t total_packets_sent = 0;
        uint64_t total_packets_received = 0;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;

        // Buckets are appended in monotonic game-clock order. Walk backward
        // from the newest and stop at the first unused or expired bucket;
        // every older bucket is also outside the one-second window.

        int i = client_transport->current_stats_index;
        for ( int remaining = ROCKETNET_NUM_CLIENT_STATS_FRAMES; remaining > 0; --remaining,
                  i = ( i + ROCKETNET_NUM_CLIENT_STATS_FRAMES - 1 ) % ROCKETNET_NUM_CLIENT_STATS_FRAMES )
        {
            if ( client_transport->stats_frames[i].frame == 0 || client_transport->stats_frames[i].time + 1.0 < time )
                break;

            total_packets_sent += client_transport->stats_frames[i].packets_sent;
            total_packets_received += client_transport->stats_frames[i].packets_received;
            total_bytes_sent += client_transport->stats_frames[i].bytes_sent;
            total_bytes_received += client_transport->stats_frames[i].bytes_received;
        }

        client_transport->client_stats.packets_sent_per_second = (int) total_packets_sent;
        client_transport->client_stats.packets_received_per_second = (int) total_packets_received;

        client_transport->client_stats.bandwidth_sent_kbps = (int) ( ( (double) total_bytes_sent * 8.0 ) / 1000.0 );
        client_transport->client_stats.bandwidth_received_kbps = (int) ( ( (double) total_bytes_received * 8.0 ) / 1000.0 );
    }

    // setup next stats frame
    {
        client_transport->frame++;
        client_transport->current_stats_index = rocketnet_stats_window_advance(
            client_transport->stats_frames, client_transport->current_stats_index,
            client_transport->frame, time );
    }
}

// proofs/tla/RNClientIo.tla Expose, Return and OnMain: update exposes the Ready
// buffer at its start and returns it at its end, on the main thread.
// proofs/tla/RNClientIo.tla StepBegin, StepEnd and OnCaller: with step_in_update the
// I/O step runs on the caller's thread inside update, never blocking.
// proofs/tla/RNBufferHandoff.tla Consume: update reads and clears the buffer it took.
// proofs/tla/RNBufferHandoffBase.tla Process and EndUpdate: update reads every exposed
// buffer outside the mutex, and its end begins the next expose.

void rocketnet_client_transport_update( rocketnet_client_transport_t * client_transport, double time )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;

    rocketnet_assert( client_transport );

    if ( client_transport->stop_requested )
    {
        if ( !client_transport->thread && !rocketnet_client_transport_stopped( client_transport ) )
        {
            if ( rocketnet_client_io_core( client_transport ) )
                rocketnet_client_io_step( &client_transport->client_io, false );
            else
                rocketnet_client_transport_finish_stop( client_transport );
        }

        return;
    }

    if ( !client_transport->thread )
        rocketnet_client_transport_flush_disconnect( client_transport );

    // the caller's clock is clamped once, before anything reads it: a NaN,
    // an infinity or a step backwards becomes the last good time, counted and
    // loud in a debug build.

    bool time_clamped = false;
    time = rocketnet_update_clock_advance( client_transport->time, &client_transport->raw_time, &client_transport->time_offset, time, &time_clamped );

    client_transport->packet_leases.reset();

    const double current_time = time;

    client_transport->time = time;
    rocketnet_client_link_drain( client_transport, current_time );

    rocketnet_client_update_stats( client_transport, time );

    const int state_before = client_transport->protocol.state;

    // proofs/tla/SessionAcks.tla Tick: the acked-packet batch drains per update: what the blocks processed below acknowledge lands there, valid until the next update

    client_transport->protocol.num_acked = 0;

    // RNClientIo Expose (docs/CLIENT-IO.md _The receive path_, update step 1):
    // a Ready buffer becomes Exposed and is processed exactly as at BASE. The
    // BASE path exposes the oldest Ready buffers that fit the packet budget
    // under one short mutex hold; additional buffers stay Ready for later.

    rocketnet_client_receive_buffer_t * receive_buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    int num_receive_buffers = 0;
    int legacy_exposed[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    int num_legacy_exposed = 0;

    const bool io_core = rocketnet_client_io_core( client_transport );

    if ( io_core )
    {
#if ROCKETNET_DEV
        if ( client_transport->step_in_update )
            rocketnet_client_io_step( &client_transport->client_io, false ); // RNClientIo CallerStep
#endif // #if ROCKETNET_DEV
        receive_buffers[0] = rocketnet_client_io_expose( &client_transport->client_io );
        num_receive_buffers = receive_buffers[0] ? 1 : 0;
    }
    else
    {
#if ROCKETNET_DEV
        // link simulator on in the caller-stepped mode: no thread receives, so the update receives from the transport, stamped with the update time

        if ( client_transport->step_in_update )
            rocketnet_client_receive_packets( client_transport );
#endif // #if ROCKETNET_DEV
        num_legacy_exposed = rocketnet_client_update_expose_legacy_receive( client_transport, legacy_exposed );
        for ( int i = 0; i < num_legacy_exposed; i++ )
            receive_buffers[i] = &client_transport->receive_buffer[legacy_exposed[i]];

        num_receive_buffers = num_legacy_exposed;
    }

    client_transport->received_packets.num_packets = 0;
    client_transport->received_packets.receive_index = 0;

    const int stats_index = client_transport->current_stats_index;

    for ( int b = 0; b < num_receive_buffers; b++ )
    {
        rocketnet_client_receive_buffer_t * receive_buffer = receive_buffers[b];
        for ( int i = 0; i < receive_buffer->num_packets; i++ )
        {
            client_transport->stats_frames[stats_index].packets_received++;
            client_transport->stats_frames[stats_index].bytes_received += (uint64_t) receive_buffer->packet_bytes[i] + (uint64_t) rocketnet_wire_header_bytes( receive_buffer->from[i].type );
            client_transport->client_stats.packets_received++;
            client_transport->client_stats.bytes_received += (uint64_t) receive_buffer->packet_bytes[i] + (uint64_t) rocketnet_wire_header_bytes( receive_buffer->from[i].type );

            // only the I/O thread writes verdicts; the BASE receive thread's records are raw

            const uint8_t verdict = io_core ? receive_buffer->verdict[i] : (uint8_t) ROCKETNET_CLIENT_IO_VERDICT_RAW;
            const uint64_t sequence = io_core ? receive_buffer->sequence[i] : 0;
            rocketnet_client_process_packet( client_transport, &receive_buffer->from[i], receive_buffer->packet_data[i], (int) receive_buffer->packet_bytes[i], current_time,
                                             verdict, sequence );
        }

        if ( io_core )
            receive_buffer->num_packets = 0;
    }

    rocketnet_client_update_return_legacy_receive( client_transport, legacy_exposed, num_legacy_exposed );

    // the protocol's scheduled send and its deadlines

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client_transport->protocol, current_time, packet, &packet_bytes );

    if ( state_before > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED &&
         client_transport->protocol.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        rocketnet_client_connection_reset( client_transport );

    if ( io_core )
    {
        // the protocol packet is sealed into a slot of the Filling ring
        // (counted into the stats by the hand-off); then RNClientIo Return
        // (update step 2: whether or not anything was exposed this frame, the
        // held buffer goes back Free, and the I/O thread is woken when it owes
        // a publish), then a hand-off attempt every frame, which retries any
        // deferred slots and hands off this packet
        if ( packet_bytes > 0 )
            rocketnet_client_transport_io_commit_packet( client_transport, packet, packet_bytes );

        rocketnet_client_io_return( &client_transport->client_io );

        rocketnet_client_transport_io_hand_off( client_transport );
#if ROCKETNET_DEV
        if ( client_transport->step_in_update )
            rocketnet_client_io_step( &client_transport->client_io, false ); // RNClientIo CallerStep
#endif // #if ROCKETNET_DEV
    }
    else if ( packet_bytes > 0 )
    {
        client_transport->stats_frames[stats_index].packets_sent++;
        client_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( client_transport->protocol.server_address.type ) );
        client_transport->client_stats.packets_sent++;
        client_transport->client_stats.bytes_sent += (uint64_t) ( packet_bytes + rocketnet_wire_header_bytes( client_transport->protocol.server_address.type ) );

        rocketnet_client_link_send( client_transport, &client_transport->protocol.server_address, packet, packet_bytes );
        rocketnet_client_link_flush( client_transport );
    }

    // one callback per update crossing: enough to observe every terminal,
    // and the poll of rocketnet_client_transport_state stays the source of truth

    if ( client_transport->protocol.state != state_before && client_transport->config.state_change_callback )
        client_transport->config.state_change_callback( client_transport->context, (enum rocketnet_client_state_t) state_before, (enum rocketnet_client_state_t) client_transport->protocol.state );
}

void rocketnet_client_transport_send_packet( rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    if ( !rocketnet_api_null_guard( packet_data ) )
        return;

    rocketnet_assert( client_transport );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    if ( packet_bytes < 1 || packet_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( client_transport, packet_bytes, &context );
    if ( !frame )
        return;

    memcpy( frame + context.payload_index, packet_data, (size_t) packet_bytes );

    rocketnet_client_transport_finish_packet( client_transport, &context, packet_bytes );

    rocketnet_client_transport_send_packets( client_transport );
}

// proofs/tla/SessionAcks.tla BurnOne: a sequence taken for this packet is spent unsent when the ring or the lease refuses the frame.
// The client's acquire family: the same shape, layout and phase
// contract as the server's, minus the client index. The convenience send
// above rides this path and flushes immediately, preserving its
// send-it-now meaning for simple callers.

static void rocketnet_client_transport_abort_packet_unchecked( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context );

// proofs/tla/RNClientIo.tla Write and OnGame: a game thread takes a free slot of the
// Filling ring.
// proofs/tla/RNBufferHandoff.tla Produce: on send the producer writes its ring outside
// the mutex.

uint8_t * rocketnet_client_transport_start_packet( rocketnet_client_transport_t * client_transport, int request_bytes, struct rocketnet_packet_context_t * context )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return NULL;
    if ( !rocketnet_api_null_guard( context ) )
        return NULL;

    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    memset( context, 0, sizeof( struct rocketnet_packet_context_t ) );

    if ( client_transport->stop_requested )
        return NULL;

    if ( request_bytes < 1 || request_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
    {
        rocketnet_spam( "client start packet refused. request out of range" );
        return NULL;
    }

    uint64_t sequence;
    if ( !rocketnet_client_protocol_start_payload( &client_transport->protocol, &sequence ) )
        return NULL;

    // a real wire frame, written in place, sealed in place, zero copies, in
    // the ring send_fill names (RNClientIo Write)

    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_fill_ring( client_transport );

    uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( slot >= ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
    {
        rocketnet_spam( "client payload ring full. packet dropped" );

        rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return NULL;
    }

    ring->info[slot].wire_bytes = 0;

    uint8_t * frame = ring->data + slot * ROCKETNET_FRAME_STRIDE;

    context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET;
    context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rocketnet_assert( ( (uintptr_t) frame % 16 ) == 0 );

    context->frame = frame;
    context->packet_id = 0; // private lease supplies the public identifier
    context->sequence = sequence;
    context->client_index = -1;

    if ( !client_transport->packet_leases.insert( *context ) )
    {
        rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_LEASE_FULL], 1 );

        rocketnet_client_transport_abort_packet_unchecked( client_transport, context );
        return NULL;
    }

    return frame;
}

static void rocketnet_client_transport_abort_packet_unchecked( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return;

    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_frame_ring( client_transport, context->frame );
    const uint64_t slot = (uint64_t) ( context->frame - ring->data ) / ROCKETNET_FRAME_STRIDE;
    ring->info[slot].wire_bytes = 0;

    context->frame = NULL;
}

uint8_t * rocketnet_client_transport_packet_payload( rocketnet_client_transport_t * transport, const rocketnet_packet_context_t * context, int * capacity )
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

void rocketnet_client_transport_abort_packet( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    if ( !rocketnet_api_null_guard( context ) )
        return;
    if ( !client_transport || !context )
        return;

    rocketnet_packet_context_t trusted;
    uint64_t backend_id;
    if ( client_transport->packet_leases.take( *context, trusted, backend_id ) )
        rocketnet_client_transport_abort_packet_unchecked( client_transport, &trusted );

    context->frame = NULL;
}

// proofs/tla/RNClientIo.tla Commit: seals the slot in place and commits it.

static int rocketnet_client_transport_finish_packet_unchecked( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    if ( !client_transport || !context )
        return 0;

    if ( !context->frame )
    {
        rocketnet_assert( context->frame != NULL );
        return 0;
    }

    if ( payload_bytes < 1 || payload_bytes > context->payload_capacity )
    {
        rocketnet_client_transport_abort_packet_unchecked( client_transport, context );
        return 0;
    }

    int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &client_transport->protocol, context->sequence,
                                                                   context->packet_id,
                                                                   context->frame + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                   payload_bytes, client_transport->time );
    if ( wire_bytes == 0 )
    {
        rocketnet_client_transport_abort_packet_unchecked( client_transport, context );
        return 0;
    }

    // RNClientIo Commit: sealed in place in the ring the frame lives in

    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_frame_ring( client_transport, context->frame );
    const uint64_t slot = (uint64_t) ( context->frame - ring->data ) / ROCKETNET_FRAME_STRIDE;
    ring->info[slot].wire_bytes = wire_bytes;
    context->frame = NULL;

    return wire_bytes;
}

int rocketnet_client_transport_finish_packet( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( context ) )
        return 0;
    if ( !client_transport || !context )
        return 0;

    rocketnet_packet_context_t trusted;
    uint64_t backend_id;
    if ( !client_transport->packet_leases.take( *context, trusted, backend_id ) )
    {
        rocketnet_assert( false );

        context->frame = NULL;
        return 0;
    }

    const int result = rocketnet_client_transport_finish_packet_unchecked( client_transport, &trusted, payload_bytes );

    context->frame = NULL;

    return result;
}

// proofs/tla/SessionAcks.tla Flush: puts the outbox on the wire in fill order.

void rocketnet_client_transport_send_packets( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    if ( client_transport->stop_requested )
        return;

    rocketnet_assert( client_transport );

    client_transport->packet_leases.reset();

    if ( rocketnet_client_io_core( client_transport ) )
    {
        // RNClientIo HandOff or Defer: no transport call on the main thread.
        rocketnet_client_transport_io_hand_off( client_transport );

#if ROCKETNET_DEV
        if ( client_transport->step_in_update )
            rocketnet_client_io_step( &client_transport->client_io, false ); // RNClientIo CallerStep
#endif // #if ROCKETNET_DEV
        return;
    }

    struct rocketnet_client_payload_ring_t * ring = &client_transport->payload_ring[0];

    uint64_t num_packets = ring->num_packets;
    if ( num_packets > ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
        num_packets = ROCKETNET_CLIENT_PAYLOAD_RING_SIZE;

    const int stats_index = client_transport->current_stats_index;

    for ( uint64_t i = 0; i < num_packets; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            // the payload accumulation rides the flush walk: the ring already holds every committed packet's wire size, so this costs nothing

            client_transport->stats_frames[stats_index].packets_sent++;
            client_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + rocketnet_wire_header_bytes( client_transport->protocol.server_address.type ) );
            client_transport->client_stats.packets_sent++;
            client_transport->client_stats.bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + rocketnet_wire_header_bytes( client_transport->protocol.server_address.type ) );

            rocketnet_client_link_send( client_transport, &client_transport->protocol.server_address,
                                        ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET,
                                        ring->info[i].wire_bytes );
        }
    }

    ring->num_packets = 0;

    rocketnet_client_link_flush( client_transport );
}

int rocketnet_client_transport_receive_packet( rocketnet_client_transport_t * client_transport, uint8_t * packet_data, int packet_capacity )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( packet_data ) )
        return 0;

    rocketnet_assert( client_transport );
    rocketnet_assert( packet_data );

    if ( packet_capacity < 1 )
    {
        // a capacity below one consumes nothing: no payload fits, so the
        // cursor does not move and nothing is counted -- the payloads stay
        // queued for the next call with a buffer that can hold them

        return 0;
    }

    struct rocketnet_client_received_packets_t * received = &client_transport->received_packets;

    // a payload larger than the caller's buffer is refused whole and
    // counted, never truncated, and the cursor moves on to the next
    // payload that fits -- a caller sizing by the configured maximum
    // payload never refuses anything

    while ( received->receive_index < received->num_packets )
    {
        const int index = received->receive_index++;

        if ( received->packet_bytes[index] > packet_capacity )
        {
            rocketnet_spam( "client receive cursor refused payload. %d bytes into a %d-byte buffer", received->packet_bytes[index], packet_capacity );

            client_transport->counters.values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL]++;
            continue;
        }

        memcpy( packet_data, received->packet_data[index], (size_t) received->packet_bytes[index] );

        return received->packet_bytes[index];
    }

    return 0;
}

int rocketnet_client_transport_max_payload_bytes( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;

    rocketnet_assert( client_transport );
    return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

// the batch the copying cursor above walks, exposed whole: by count and
// index, and as parallel arrays in one call for bindings and tight loops.
// valid until the next update.

int rocketnet_client_transport_num_payloads( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;

    rocketnet_assert( client_transport );
    return client_transport->received_packets.num_packets;
}

uint8_t * rocketnet_client_transport_payload( rocketnet_client_transport_t * client_transport, int payload_index, int * payload_bytes )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return NULL;
    if ( !rocketnet_api_null_guard( payload_bytes ) )
        return NULL;

    rocketnet_assert( client_transport );
    rocketnet_assert( payload_bytes );

    struct rocketnet_client_received_packets_t * received = &client_transport->received_packets;

    if ( payload_index < 0 || payload_index >= received->num_packets )
    {
        *payload_bytes = 0;
        return NULL;
    }

    *payload_bytes = received->packet_bytes[payload_index];

    return received->packet_data[payload_index];
}

int rocketnet_client_transport_payloads( rocketnet_client_transport_t * client_transport, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( payload_data ) )
        return 0;
    if ( !rocketnet_api_null_guard( payload_bytes ) )
        return 0;

    rocketnet_assert( client_transport );
    rocketnet_assert( payload_data );
    rocketnet_assert( payload_bytes );

    *payload_data = client_transport->received_packets.packet_data;
    *payload_bytes = client_transport->received_packets.packet_bytes;

    return client_transport->received_packets.num_packets;
}

int rocketnet_client_transport_state( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;

    rocketnet_assert( client_transport );
    return client_transport->protocol.state;
}

// the granted-tier report: the transport that won the ladder at create, by
// name -- what performance claims and operator errands attach to

const char * rocketnet_client_transport_io_name( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return NULL;

    rocketnet_assert( client_transport );
    return rocketnet_transport_name( client_transport->transport );
}

int rocketnet_client_transport_client_index( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;

    rocketnet_assert( client_transport );
    return client_transport->protocol.client_index;
}

// proofs/tla/RNClientIo.tla Disconnect: seals the burst into the Filling ring and
// hands it off to the address saved before the protocol reset.
// proofs/tla/RNClientLinkStop.tla Disconnect: with the link simulator the burst takes
// the loss policy and its surviving copies are staged in the simulator queue.

void rocketnet_client_transport_disconnect( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;

    rocketnet_assert( client_transport );

    if ( client_transport->stop_requested )
        return;

    const int state_before = client_transport->protocol.state;

    // this transport owns one connection attempt. A duplicate request must not overwrite ciphertext that its legacy worker may still be sending.

    if ( state_before == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        return;

    // The protocol reset erases the connection address. Like the server side,
    // retain the destination locally so the already-sealed disconnect burst is
    // the only connection data that outlives reset.

    const rocketnet_address_t server_address = client_transport->protocol.server_address;

    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_client_protocol_disconnect( &client_transport->protocol, burst, &burst_count, &burst_bytes );

    rocketnet_client_connection_reset( client_transport );

    // every state change reaches the callback, from update or from disconnect:
    // this transition never crosses an update, so it is reported here

    if ( client_transport->protocol.state != state_before && client_transport->config.state_change_callback )
        client_transport->config.state_change_callback( client_transport->context, (enum rocketnet_client_state_t) state_before, (enum rocketnet_client_state_t) client_transport->protocol.state );

    if ( rocketnet_client_io_core( client_transport ) )
    {
        // the burst is sealed into slots of the Filling ring and handed off
        // like any packet: deferred when the other ring is busy, retried by
        // the next update, or published as the final ring by stop
        // (docs/CLIENT-IO.md _The protocol packet and disconnect_)
        for ( int i = 0; i < burst_count; i++ )
            rocketnet_client_transport_io_commit_packet( client_transport, burst + i * ROCKETNET_SEND_BUDGET, burst_bytes );

        rocketnet_client_transport_io_hand_off( client_transport, &server_address );
        return;
    }

    const uint64_t wire_bytes = (uint64_t) burst_count * (uint64_t) ( burst_bytes + rocketnet_wire_header_bytes( server_address.type ) );

    client_transport->stats_frames[client_transport->current_stats_index].packets_sent += (uint64_t) burst_count;
    client_transport->stats_frames[client_transport->current_stats_index].bytes_sent += wire_bytes;
    client_transport->client_stats.packets_sent += (uint64_t) burst_count;
    client_transport->client_stats.bytes_sent += wire_bytes;
    if ( client_transport->link_simulator )
    {
        // simulator queues are owned by the caller. Preserve configured loss, latency and reorder; enqueue here, never bypass it on the worker.
        for ( int i = 0; i < burst_count; ++i )
            rocketnet_client_link_send( client_transport, &server_address,
                                        burst + i * ROCKETNET_SEND_BUDGET, burst_bytes );

        rocketnet_client_link_flush( client_transport );
        return;
    }

    client_transport->stop_address = server_address;
    client_transport->stop_burst_count = burst_count;
    client_transport->stop_burst_bytes = burst_bytes;

    for ( int i = 0; i < burst_count; ++i )
        memcpy( client_transport->stop_burst + i * ROCKETNET_SEND_BUDGET,
                burst + i * ROCKETNET_SEND_BUDGET, (size_t) burst_bytes );
    if ( burst_count )
        client_transport->stop_burst_pending.store( true, std::memory_order_release );
}

// test seam: the port the client socket is bound to, so a functional test
// sends datagrams at it
#if ROCKETNET_DEV
uint16_t rocketnet_client_transport_test_bound_port( rocketnet_client_transport_t * client_transport )
{
    return client_transport ? client_transport->bound_port : 0;
}
#endif // #if ROCKETNET_DEV

const struct rocketnet_counters_t * rocketnet_client_transport_counters( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return NULL;

    rocketnet_assert( client_transport );

    // SEND_FAILED is the transport's own cumulative count, assigned on read; the transport's count is atomic, so the I/O thread may be sending meanwhile

    if ( client_transport->io.operations && client_transport->io.operations->send_failures )
        client_transport->counters.values[ROCKETNET_COUNTER_SEND_FAILED] = client_transport->io.operations->send_failures( client_transport->io.instance );

    return &client_transport->counters;
}

#if ROCKETNET_DEV
// The transport's half of the delivery capture (rocketnet_layers.h): plain
// copies of the protocol's receive window, send sequence and ack endpoint, on
// the main thread outside update. Not connected leaves the capture untouched.

void rocketnet_client_transport_delivery_capture( rocketnet_client_transport_t * client_transport, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( client_transport ) || !rocketnet_api_null_guard( capture ) )
        return;

    const struct rocketnet_client_protocol_t * protocol = &client_transport->protocol;
    if ( protocol->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return;

    capture->connected = 1;
    capture->last_receive_age_seconds = protocol->last_receive_time > 0.0 ? client_transport->time - protocol->last_receive_time : -1.0;
    capture->highest_received_sequence = protocol->replay_session.highest_opened;
    capture->highest_sent_sequence = protocol->send_sequence_session > 0 ? protocol->send_sequence_session - 1 : 0;
    capture->largest_acked_sequence = protocol->ack.largest_acked_sequence;
    capture->keep_alive_acked = protocol->ack.keep_alive_acked ? 1 : 0;
}
#endif // #if ROCKETNET_DEV

int rocketnet_client_transport_acked_packets( rocketnet_client_transport_t * client_transport, const uint64_t ** packet_ids )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;
    if ( !rocketnet_api_null_guard( packet_ids ) )
        return 0;

    rocketnet_assert( client_transport );
    rocketnet_assert( packet_ids );

    *packet_ids = client_transport->protocol.acked_packet_ids;

    return client_transport->protocol.num_acked;
}

void rocketnet_client_transport_stats( rocketnet_client_transport_t * client_transport, struct rocketnet_client_transport_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    if ( !rocketnet_api_null_guard( stats ) )
        return;

    rocketnet_assert( client_transport );
    rocketnet_assert( stats );

    memset( stats, 0, sizeof( struct rocketnet_client_transport_stats_t ) );

#if ROCKETNET_DEV
    g_client_ack_stats_count.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV

    // the client's only reader of the ack statistics is this getter, so they are computed here (docs/CLIENT-IO-DECRYPT.md, The ack statistics)

    rocketnet_ack_endpoint_update_stats( &client_transport->protocol.ack );

    // rtt seeds from the handshake's round trips and the acks refresh it continuously from there: once positive first-ack samples exist they are the estimate

    stats->rtt = client_transport->protocol.ack.rtt > 0.0f ? client_transport->protocol.ack.rtt : (float) ( client_transport->protocol.rtt_estimate * 1000.0 );
    stats->jitter = client_transport->protocol.ack.jitter;
    stats->packet_loss = client_transport->protocol.ack.packet_loss;

    stats->packets_sent_per_second = client_transport->client_stats.packets_sent_per_second;
    stats->packets_received_per_second = client_transport->client_stats.packets_received_per_second;
    stats->bandwidth_sent_kbps = client_transport->client_stats.bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = client_transport->client_stats.bandwidth_received_kbps;
    stats->packets_sent = client_transport->client_stats.packets_sent;
    stats->packets_received = client_transport->client_stats.packets_received;
    stats->bytes_sent = client_transport->client_stats.bytes_sent;
    stats->bytes_received = client_transport->client_stats.bytes_received;
}

#if ROCKETNET_DEV
// One in-memory transport. No socket, thread, or wall clock. The caller frees
// it with rocketnet_client_transport_ack_stats_close_for_test.

static rocketnet_client_transport_t * rocketnet_client_transport_ack_stats_open_for_test( const rocketnet_ack_endpoint_t * endpoint )
{
    auto * client = (rocketnet_client_transport_t *) rocketnet_malloc( NULL, sizeof( rocketnet_client_transport_t ) );
    rocketnet_assert( client );
    rocketnet_client_transport_construct( client );

    rocketnet_client_protocol_init( &client->protocol, &client->counters );

    client->protocol.ack = *endpoint;
    return client;
}

static void rocketnet_client_transport_ack_stats_close_for_test( rocketnet_client_transport_t * client )
{
    if ( !client )
        return;

    client->~rocketnet_client_transport_t();

    rocketnet_free( NULL, client );
}

// Exercise the real getter with causal ACK history and deliberately different
// receive estimates. No transport lifecycle, I/O, thread or wall clock is used.

void rocketnet_client_transport_ack_stats_for_test( const rocketnet_ack_endpoint_t * endpoint,
                                                    rocketnet_client_transport_stats_t * stats )
{
    auto * client = rocketnet_client_transport_ack_stats_open_for_test( endpoint );
    rocketnet_client_transport_stats( client, stats );

    rocketnet_client_transport_ack_stats_close_for_test( client );
}

// The public rocketnet_client_stats path, over the same in-memory ACK history.
// The message client is only the object the public getter already walks. It
// is not a connection and it opens nothing. The test calls rocketnet_client_stats.

struct rocketnet_client_t * rocketnet_client_ack_stats_public_client_for_test( const rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_client_transport_t * transport = rocketnet_client_transport_ack_stats_open_for_test( endpoint );
    rn_message_link_t link;
    rn_message_client_link( &link, transport );

    rocketnet_message_client_t * message = (rocketnet_message_client_t *) rocketnet_malloc( NULL, sizeof( rocketnet_message_client_t ) );
    rocketnet_assert( message );

    memset( message, 0, sizeof( *message ) );
    message->object.link = link;

    struct rocketnet_client_t * client = rocketnet_client_adopt( NULL, message );
    rocketnet_assert( client );

    return client;
}

void rocketnet_client_ack_stats_public_client_for_test_free( struct rocketnet_client_t * client )
{
    if ( !client )
        return;

    struct rocketnet_message_client_t * message = rocketnet_client_message_client( client );
    rocketnet_client_transport_t * transport = message ? rocketnet_message_client_transport( message ) : NULL;

    rocketnet_free( NULL, client );
    rocketnet_free( NULL, message );

    rocketnet_client_transport_ack_stats_close_for_test( transport );
}
#endif // #if ROCKETNET_DEV

// RNBufferHandoff ProducerExchange on the BASE path: the receive worker
// always holds one buffer (fill), reads datagrams into it outside the mutex,
// and publishes it when the socket is drained or the buffer is full and a
// free buffer exists (MaxReady N-1).
// Otherwise it keeps the batch and adds to it while it has room; a full
// buffer that cannot publish leaves datagrams in the transport until a later
// update frees one. No buffer another side owns is ever written.

// proofs/tla/RNBufferHandoff.tla WorkerLock, ProducerExchange and Unlock: the receive
// worker locks, publishes its buffer index and takes a free one, then unlocks.
// proofs/tla/RNBufferHandoffBase.tla WorkerLock and Publish: the same lock and publish
// with MaxReady N-1.

static void rocketnet_client_legacy_publish( rocketnet_client_transport_t * client_transport )
{
    rocketnet_client_legacy_receive_state_t * state = &client_transport->receive_buffer_index;

    rocketnet_platform_mutex_acquire( &client_transport->mutex );

    const int next = rocketnet_buffer_handoff_publish( &state->handoff, state->fill, ROCKETNET_CLIENT_IO_NUM_BUFFERS - 1 );

    rocketnet_platform_mutex_release( &client_transport->mutex );

    if ( next >= 0 )
    {
        state->fill = next;
        client_transport->receive_buffer[next].num_packets = 0;
    }
}

// proofs/tla/RNBufferHandoff.tla Produce: the receive worker writes datagrams into
// its buffer outside the mutex.
// proofs/tla/RNBufferHandoff.tla WriteReady: the reversed witness's receive worker
// writes into a buffer it already published, which update now holds.
// proofs/tla/RNBufferHandoffBase.tla Produce: the same write into the producer buffer.

static void rocketnet_client_receive_packets( rocketnet_client_transport_t * client_transport, const double * synthetic_time )
{
    rocketnet_assert( client_transport );

    rocketnet_client_legacy_receive_state_t * state = &client_transport->receive_buffer_index;
    uint8_t scratch[ROCKETNET_RECEIVE_PACKET_BYTES];

    while ( 1 )
    {
        rocketnet_client_receive_buffer_t * receive_buffer = &client_transport->receive_buffer[state->fill];
        if ( receive_buffer->num_packets >= ROCKETNET_NUM_CLIENT_PACKETS )
        {
            const int full = state->fill;

            rocketnet_client_legacy_publish( client_transport );

            if ( state->fill == full )
                break;
            continue;
        }

        struct rocketnet_address_t from;
        const int packet_bytes = client_transport->io.operations->receive( client_transport->io.instance, &from, scratch, ROCKETNET_RECEIVE_PACKET_BYTES );
        if ( packet_bytes < 0 || packet_bytes > ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES )
            client_transport->client_io.transport_faults++;

        if ( packet_bytes <= 0 || packet_bytes > ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES )
        {
            // publish a partial batch on drain or fault; an empty buffer stays the worker's
            if ( receive_buffer->num_packets )
                rocketnet_client_legacy_publish( client_transport );
            break;
        }

        // every datagram is kept with its true size, a runt or an oversize
        // one too, as the I/O core keeps it (sdk/rocketnet_client_io.h): the
        // update thread's prefilter refuses it and counts PREFILTER_TOO_SHORT
        // or PREFILTER_TOO_LARGE (sdk/rocketnet_protocol.cpp). An oversize
        // datagram's bytes past scratch were never written, so only scratch's
        // prefix is copied.

        const int copy_bytes = packet_bytes < ROCKETNET_RECEIVE_PACKET_BYTES ? packet_bytes : ROCKETNET_RECEIVE_PACKET_BYTES;

        const double receive_time = synthetic_time ? *synthetic_time
#if ROCKETNET_DEV
                                                   : ( client_transport->step_in_update ? client_transport->time : rocketnet_platform_time() );
#else
                                                   : rocketnet_platform_time();
#endif // #if ROCKETNET_DEV

        const int index = receive_buffer->num_packets;
        memcpy( receive_buffer->data + index * ROCKETNET_FRAME_SIZE, scratch, (size_t) copy_bytes );
        receive_buffer->from[index] = from;
        receive_buffer->receive_time[index] = receive_time;
        receive_buffer->packet_data[index] = receive_buffer->data + index * ROCKETNET_FRAME_SIZE;
        receive_buffer->packet_bytes[index] = (size_t) packet_bytes;
        receive_buffer->num_packets++;
    }
}

#if ROCKETNET_DEV
void rocketnet_client_transport_step_legacy_for_test( rocketnet_client_transport_t * client_transport, double time )
{
    rocketnet_assert( client_transport && client_transport->step_driven &&
                      !client_transport->thread && !rocketnet_client_io_core( client_transport ) );

    if ( !client_transport || !client_transport->step_driven ||
         client_transport->thread || rocketnet_client_io_core( client_transport ) )
        return;

    if ( client_transport->stop_requested )
    {
        if ( !rocketnet_client_transport_stopped( client_transport ) )
            rocketnet_client_transport_finish_stop( client_transport );

        return;
    }

    rocketnet_client_transport_flush_disconnect( client_transport );

    rocketnet_client_receive_packets( client_transport, &time );
}

void rocketnet_client_transport_legacy_busy_for_test( rocketnet_client_transport_t * client_transport, unsigned mask )
{
    rocketnet_assert( client_transport && client_transport->step_driven && !client_transport->thread && mask <= 3 );
    client_transport->receive_buffer_index.test_busy_mask = mask;
}

// The step-driven fixture has no concurrent worker. Expose ownership so its
// regression can check backpressure and buffer reuse without depending on
// scheduler timing or private layout offsets.

void rocketnet_client_transport_legacy_receive_state_for_test(
    const rocketnet_client_transport_t * client_transport, int * fill, int * ready_count, int * free_count )
{
    rocketnet_assert( client_transport && client_transport->step_driven && !client_transport->thread &&
                      !rocketnet_client_io_core( client_transport ) );

    const rocketnet_client_legacy_receive_state_t * state = &client_transport->receive_buffer_index;

    if ( fill )
        *fill = state->fill;
    if ( ready_count )
        *ready_count = state->handoff.ready_count;
    if ( free_count )
        *free_count = state->handoff.free_count;
}
#endif // #if ROCKETNET_DEV

// proofs/tla/RNBufferHandoff.tla WorkerExit: on receive the worker exits once quit is
// seen.
// proofs/tla/RNBufferHandoff.tla WorkerWait, WorkerWakes and Terminated: the worker
// sleeps when nothing was done and wakes on the wake or readable, and the
// behaviour ends with both threads done.
// proofs/tla/RNClientIoShutdown.tla WorkerExit: the legacy worker returns after its
// stop is complete.

static void rocketnet_client_thread_function( rocketnet_platform_thread_t * thread, void * data )
{
    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t *) data;

    // quit is read with acquire on every iteration; the wait between polls is bounded at 1 ms. Stop does not acquire the platform wake mutex.

    while ( !client_transport->quit.load( std::memory_order_acquire ) )
    {
        rocketnet_client_transport_flush_disconnect( client_transport );

        rocketnet_client_receive_packets( client_transport );

        rocketnet_platform_thread_wait( thread, 0.001 );
    }

    rocketnet_client_transport_finish_stop( client_transport );
}

// The development network simulator call on a live transport: applies at once.
// A development connection always owns a link simulator, so this stores the new
// settings as a config record under the tiny mutex and the thread that owns the
// simulator applies them before its next send. No simulator work happens under
// the lock. Stop transfers the simulator to the worker; a stopped transport
// cannot be reconfigured.
#if ROCKETNET_DEV
bool rocketnet_client_transport_network_simulator( struct rocketnet_client_transport_t * client_transport, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent )
{
    rocketnet_assert( client_transport );

    if ( !client_transport->link_simulator || client_transport->stop_requested )
        return false;

    // The new setting is handed over as a config record under the
    // simulator's own tiny mutex; the thread that owns the simulator takes
    // it before its next send, so the change takes effect at once and the
    // connection's threads do not move.

    rocketnet_network_simulator_reconfigure( client_transport->link_simulator, &client_transport->mutex,
                                             latency_ms / 1000.0, jitter_ms / 1000.0,
                                             (float) packet_loss_percent, (float) duplicate_percent );

    return true;
}
#endif // #if ROCKETNET_DEV

void rocketnet_client_transport_defer_payload_acks( struct rocketnet_client_transport_t * transport )
{
    transport->protocol.defer_payload_acks = 1;
}

void rocketnet_client_transport_accept_payload( struct rocketnet_client_transport_t * transport, int k )
{
    const rocketnet_client_received_packets_t * batch = &transport->received_packets;
    if ( k >= 0 && k < batch->num_packets )
        rocketnet_client_protocol_accept_payload( &transport->protocol, batch->sequence[k] );
}

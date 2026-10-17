#include <new>
#include "rocketnet_packet_lease.h"
#include "rocketnet_stats_window.h"
/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_client_io.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_protocol.h"
#include "rocketnet_fragment.h"
#include "rocketnet_memory.h"
#include "rocketnet_memory_checks.h"
#include "rocketnet_packet_loss_tracker.h"
#include "rocketnet_jitter_tracker.h"
#include "rocketnet_config.h"

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <atomic>

// counts the client's calls of rocketnet_ack_endpoint_update_stats, for the
// functional test
unsigned long long g_client_ack_stats_count = 0;

// The one clamp both transports share. A NaN or infinite time makes every
// timeout comparison false and freezes a transport; a backwards step rewinds
// its deadlines. Both become the last good time instead, counted and loud in a
// debug build. Called once per update and once at client create, never per
// packet.

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
        rocketnet_assert( usable );
        return last_good;
    }

    return time;
}

// payloads are decrypted out of the receive frames into this storage, and
// handed to the caller as pointers into it, valid until the next update

// the client payload ring: the acquire/commit staging, filled from any
// number of game threads between updates and flushed by send_packets on
// the main thread. same discipline as the server's: one atomic add per
// slot, per-slot info alone on its cache line, roots 16-aligned.

#define ROCKETNET_CLIENT_PAYLOAD_RING_SIZE     256

struct alignas(64) rocketnet_client_payload_slot_info_t
{
    int wire_bytes;
};

struct rocketnet_client_payload_ring_t
{
    uint8_t padding_0[4096];

    volatile uint64_t num_packets;

    uint8_t padding_1[4096];

    struct rocketnet_client_payload_slot_info_t info[ROCKETNET_CLIENT_PAYLOAD_RING_SIZE];

    uint8_t padding_2[4096];

    alignas(16) uint8_t data[ROCKETNET_FRAME_STRIDE*ROCKETNET_CLIENT_PAYLOAD_RING_SIZE];

    uint8_t padding_3[4096];
};

struct rocketnet_client_received_packets_t
{
    int num_packets;
    int receive_index;
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    int packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_NUM_CLIENT_PACKETS];
};

// The statistics ring advances by elapsed time, independently of update cadence,
// accumulation riding the walks the transport already makes, published
// per-second numbers summing only buckets younger than one second. the two
// fields past the reference set carry receive-side packet loss: session
// packets accepted, and losses the tracker detected, in the same window.

#define ROCKETNET_NUM_CLIENT_STATS_FRAMES 256

struct alignas(64) rocketnet_client_stats_frame_t
{
    double time;
    uint64_t frame;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t session_packets_received;
    uint64_t packets_lost;
};

struct rocketnet_client_transport_t
{
    rocketnet_packet_leases_t<1024> packet_leases;
    void * context;

    uint16_t bound_port;

    int driver;                     // the granted driver (ROCKETNET_DRIVER_*), decided by the ladder at create

    rocketnet_client_driver_t io;

    // docs/CLIENT-IO.md _The interface_: a granted driver with wait and wake
    // runs the I/O thread design through client_io; any other runs the BASE
    // path (per-packet sends on the main thread, the polling receive thread).
    // io_step_mode is the step-driven unit mode (no I/O thread: the test
    // calls the step, and destroy steps in place of the sleep and the join).
    bool use_io_thread;
    bool io_step_mode;
    bool io_running;                // the core is initialized and its thread, or step mode, runs
    rocketnet_client_io_t client_io;

    rocketnet_platform_thread_t * thread;
    rocketnet_platform_mutex_t mutex;
    std::atomic<bool> quit{0};

    int receive_buffer_index;
    rocketnet_client_received_packets_t received_packets;
    rocketnet_client_receive_buffer_t receive_buffer[2];

    double time;                                        // the game's clock, last seen
    uint64_t frame;                                     // advanced once per update; identifies initialized stats buckets
    int last_state;
    struct rocketnet_client_transport_config_t config;

    // the packet id counter behind the acquire/commit surface: unique and
    // monotonic, the join key the acked-packet batch surfaces. the ring
    // slot is recovered from the frame pointer wherever it is needed.


    // the fragmentation shape from the config, and the storage it sizes:
    // the send arena for over-wire-capacity requests, and the reassembly
    // set the protocol delivers completed payloads through -- both
    // allocated once at create, to the configured maximum

    int max_payload_bytes;
    struct rocketnet_send_arena_t * send_arena;
    struct rocketnet_reassembly_set_t * reassembly;

    // two rings of the same layout (docs/CLIENT-IO.md _The hand-off and the
    // two rings_): the BASE path fills ring 0 only; the I/O path fills the
    // ring client_io.rings.fill names and hands it off to the I/O thread
    struct rocketnet_client_payload_ring_t payload_ring[2];

    struct rocketnet_counters_t counters;
    struct rocketnet_client_protocol_t protocol;

    struct rocketnet_client_transport_stats_t client_stats;
    struct rocketnet_client_stats_frame_t stats_frames[ROCKETNET_NUM_CLIENT_STATS_FRAMES];
    int current_stats_index;
    struct rocketnet_packet_loss_tracker_t packet_loss_tracker;
    struct rocketnet_jitter_tracker_t jitter_tracker;
};

static void rocketnet_client_thread_function( void * data );

// docs/CLIENT-IO.md _The interface_ (the design tla/RNClientIo.tla models,
// whose actions the comments below cite): the I/O thread design needs both wait and
// wake (rocketnet_client_driver_open refuses one without the other).
bool rocketnet_client_transport_uses_io_thread( const rocketnet_client_driver_operations_t * operations )
{
    return operations && operations->wait && operations->wake;
}

// The unit-test seam of sdk/rocketnet_client_io.h: while set, create opens
// these operations in place of the ladder's grant, and an I/O-path transport
// runs in the step-driven unit mode.
static const rocketnet_client_transport_test_driver_t * rocketnet_client_transport_test_driver_seam = NULL;

void rocketnet_client_transport_set_test_driver( const rocketnet_client_transport_test_driver_t * driver )
{
    rocketnet_client_transport_test_driver_seam = driver;
}

// docs/CLIENT-IO.md _The clock_: NULL time_callback means the platform clock.
static double rocketnet_client_transport_platform_time( void * context )
{
    (void) context;
    return rocketnet_platform_time();
}

// The ring game threads fill: ring 0 on the BASE path, the ring rings.fill
// names on the I/O path (RNClientIo Write, Commit). rings.fill changes only in
// a hand-off, which runs with the fills joined.
static struct rocketnet_client_payload_ring_t * rocketnet_client_fill_ring( rocketnet_client_transport_t * client_transport )
{
    return &client_transport->payload_ring[ client_transport->use_io_thread ? client_transport->client_io.rings.fill : 0 ];
}

// The ring a ring frame lives in.
static struct rocketnet_client_payload_ring_t * rocketnet_client_ring_of_frame( rocketnet_client_transport_t * client_transport, const uint8_t * frame )
{
    for ( int i = 0; i < 2; i++ )
    {
        const uint8_t * data = client_transport->payload_ring[i].data;
        if ( frame >= data && frame < data + sizeof( client_transport->payload_ring[i].data ) )
            return &client_transport->payload_ring[i];
    }
    rocketnet_assert( !"frame outside both payload rings" );
    return &client_transport->payload_ring[0];
}

static uint64_t rocketnet_client_ring_committed( const struct rocketnet_client_payload_ring_t * ring )
{
    const uint64_t num_packets = ring->num_packets;
    return num_packets > ROCKETNET_CLIENT_PAYLOAD_RING_SIZE ? ROCKETNET_CLIENT_PAYLOAD_RING_SIZE : num_packets;
}

// RNClientIo HandOff, Defer and Reopen (docs/CLIENT-IO.md _Reuse only after
// sent; a hand-off never waits_); main thread, fills joined. Hands the
// Filling ring's committed slots to the I/O thread when the other ring loads
// Sent (or was never handed off). Otherwise the hand-off is deferred: the
// slots stay in the Filling ring, open to the game threads, for the next
// hand-off from send_packet, send_packets or update; CLIENT_SEND_DEFERRED
// counts the call and it returns at once. No spin, no yield, no retry here
// (HandOffNeverWaits), and a ring the I/O thread holds is never reused
// (NoReuseBeforeSent).
static void rocketnet_client_transport_io_hand_off( rocketnet_client_transport_t * client_transport )
{
    rocketnet_client_io_t * io = &client_transport->client_io;
    const int fill = io->rings.fill;
    struct rocketnet_client_payload_ring_t * ring = &client_transport->payload_ring[fill];

    const uint64_t committed = rocketnet_client_ring_committed( ring );
    if ( committed == 0 )
        return;

    const int result = rocketnet_client_io_hand_off( io, committed, &client_transport->protocol.server_address );
    if ( result == ROCKETNET_CLIENT_RINGS_LATE )
    {
        client_transport->counters.values[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED]++;
        return;
    }
    if ( result != ROCKETNET_CLIENT_RINGS_HANDED_OFF )
        return;

    // the send accumulation rides the hand-off: the handed-off ring's slots
    // are only read here, as the I/O thread reads them

    const int stats_index = client_transport->current_stats_index;
    for ( uint64_t i = 0; i < committed; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            client_transport->stats_frames[stats_index].packets_sent++;
            client_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );
        }
    }

    // RNClientIo Reopen: the ring now Filling was reopened from Sent by the
    // hand-off, or was never handed off; its slots start again at zero

    client_transport->payload_ring[io->rings.fill].num_packets = 0;
}

// docs/CLIENT-IO.md _The protocol packet and disconnect_: a packet the main
// thread builds is sealed into a slot of the Filling ring like a payload
// (RNClientIo Write, Commit) and leaves with the next hand-off. No driver send
// on the main thread in the I/O path.
static void rocketnet_client_transport_io_commit_packet( rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes )
{
    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_fill_ring( client_transport );

    const uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( slot >= ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
    {
        rocketnet_spam( "client payload ring full. packet dropped" );
        rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return;
    }

    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_FRAME_STRIDE - ROCKETNET_FRAME_WIRE_OFFSET );
    memcpy( ring->data + slot * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET, packet_data, (size_t) packet_bytes );
    ring->info[slot].wire_bytes = packet_bytes;
}

int rocketnet_client_transport_io_step( rocketnet_client_transport_t * client_transport, bool block )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( client_transport->io_step_mode );
    return rocketnet_client_io_step( &client_transport->client_io, block );
}

// rocketnet_client_transport_default_config lives in rocketnet_config.cpp,
// written from the config table so every default is declared exactly once

// Pre-allocation client memory plan. The client's create allocations are the
// transport block, the send arena, the reassembly set and the granted driver's
// socket; each has a byte helper used here, never a re-derived size. The
// components embedded in the transport block (receive buffers, payload ring,
// protocol state) ride its sizeof, and the SDK allocator's alignment and
// ownership overhead is added per allocation. The driver name must be a
// concrete built-in for this platform; unknown names, "auto" and driver lists
// are not concrete and return false.

struct rocketnet_client_memory_plan_t
{
    uint64_t transport_bytes, reassembly_bytes, send_arena_bytes;
    uint64_t driver_bytes, total_bytes;
    int sockets;
};

static bool rocketnet_client_transport_memory_plan( const rocketnet_client_transport_config_t * config,
    const char * driver, rocketnet_client_memory_plan_t & plan )
{
    plan = {};
    if ( !driver ) return false;
    rocketnet_client_transport_config_t defaults;
    if ( !config ) { rocketnet_client_transport_default_config( &defaults ); config = &defaults; }
    const int payload = config->max_payload_bytes ? config->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int slots = config->num_reassembly_slots ? config->num_reassembly_slots : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    if ( payload < ROCKETNET_MAX_PAYLOAD_BYTES || payload > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES ||
        slots < 1 || slots > ROCKETNET_MAX_FRAGMENTS ) return false;

    plan.transport_bytes = rocketnet_memory_allocations( { sizeof(rocketnet_client_transport_t) } );
    plan.reassembly_bytes = rocketnet_reassembly_memory_required( 1, slots, payload, rocketnet_client_reassembly_delivered_capacity( slots ) );
    if ( !plan.reassembly_bytes || !rocketnet_memory_add( plan.reassembly_bytes, 4, rocketnet_allocation_overhead() ) ) return false;
    plan.send_arena_bytes = rocketnet_send_arena_allocation_bytes( ROCKETNET_SEND_ARENA_SLOTS, ( payload + 15 ) & ~15 );

    if ( strcmp( driver, "portable" ) == 0 )
    {
        plan.driver_bytes = rocketnet_platform_socket_allocation_bytes();
        plan.sockets = 1;
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    else if ( strcmp( driver, "msgx" ) == 0 )
    {
        plan.driver_bytes = rocketnet_platform_socket_allocation_bytes();
        plan.sockets = 1;
    }
#endif
    else return false;

    if ( !plan.transport_bytes || !plan.send_arena_bytes || !plan.driver_bytes ) return false;
    for ( uint64_t bytes : { plan.transport_bytes, plan.reassembly_bytes, plan.send_arena_bytes, plan.driver_bytes } )
        if ( !rocketnet_memory_add( plan.total_bytes, 1, bytes ) ) return false;
    return plan.total_bytes <= SIZE_MAX;
}

uint64_t rocketnet_client_transport_required_memory( const rocketnet_client_transport_config_t * config, const char * driver )
{
    rocketnet_client_memory_plan_t plan;
    return rocketnet_client_transport_memory_plan( config, driver, plan ) ? plan.total_bytes : 0;
}

rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time )
{
    return rocketnet_client_transport_create_ex( context, connect_token, config, time, NULL );
}

rocketnet_client_transport_t * rocketnet_client_transport_create_ex( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status )
{
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

    // the config layering (#106): DEFAULT < STRUCT < ENV. NULL config is
    // the development posture (the default shape); the caller's struct is
    // the middle layer; the environment overlays both through the config
    // table, every override logged loudly with the value it displaced.
    // validation refuses loudly and never clamps.

    struct rocketnet_client_transport_config_t effective_config;
    if ( config )
        effective_config = *config;
    else
        rocketnet_client_transport_default_config( &effective_config );

    int num_config_fields = 0;
    const struct rocketnet_config_field_t * config_fields = rocketnet_client_transport_config_fields( &num_config_fields );

    if ( !rocketnet_config_apply_env( "client", &effective_config, config_fields, num_config_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return NULL;
    }

    if ( !rocketnet_config_validate( "client", &effective_config, config_fields, num_config_fields ) )
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
    if (!rocketnet_reassembly_memory_allowed(1, requested_slots, requested_payload, rocketnet_client_reassembly_delivered_capacity( requested_slots )))
    {
        if ( status )
            *status = ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED;
        return NULL;
    }


    // the driver candidate list parses before anything allocates: a typo
    // fails here, loudly, listing the valid names

    int num_available_drivers = 0;
    const int * available_drivers = rocketnet_client_driver_availability( &num_available_drivers );

    struct rocketnet_driver_ladder_t ladder;
    if ( !rocketnet_driver_ladder_parse( config->driver, available_drivers, num_available_drivers, &ladder ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_DRIVER_LIST;
        return NULL;
    }

    // the operator memory budget (#155): each candidate's pre-allocation plan
    // is compared to the budget before any allocation, and a candidate whose
    // plan exceeds it is refused exactly as a refusing driver is -- logged with
    // the plan and the budget, and skipped so the ladder falls to the next
    // candidate. a budget that no candidate fits refuses the create before the
    // first allocation.
    uint64_t memory_budget = 0;
    if ( !rocketnet_config_apply_env_uint64( "client", "memory_budget_bytes", &memory_budget ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return NULL;
    }
    bool budget_refused[ROCKETNET_NUM_DRIVERS];
    memset( budget_refused, 0, sizeof(budget_refused) );
    int budget_refused_total = 0;
    int budget_allowed = 0;

    for ( int i = 0; i < ladder.num_candidates; ++i )
    {
        const int candidate = ladder.candidates[i];
        const char * name = rocketnet_driver_name( candidate );
        rocketnet_client_memory_plan_t plan;
        if ( rocketnet_client_transport_memory_plan( config, name, plan ) )
        {
            rocketnet_info( "client memory plan [%s]: %llu allocator bytes, %d sockets; excludes OS stacks, kernel queues, loader and application memory",
                name, (unsigned long long) plan.total_bytes, plan.sockets );
            if ( memory_budget > 0 && plan.total_bytes > memory_budget )
            {
                rocketnet_warn( "client driver %s refused: memory plan %llu allocator bytes exceeds the %llu byte operator budget",
                    name, (unsigned long long) plan.total_bytes, (unsigned long long) memory_budget );
                budget_refused[candidate] = true;
                budget_refused_total++;
            }
            else
                budget_allowed++;
        }
        else
            rocketnet_info( "client memory plan [%s]: unavailable; consult this driver's resource policy", name );
    }

    if ( memory_budget > 0 && budget_refused_total > 0 && budget_allowed == 0 )
    {
        rocketnet_error( "client driver ladder exhausted: every candidate's memory plan exceeds the %llu byte operator budget",
            (unsigned long long) memory_budget );
        if ( status )
            *status = ROCKETNET_STATUS_DRIVER_LADDER_EXHAUSTED;
        return NULL;
    }

    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t*) rocketnet_malloc( context, sizeof(rocketnet_client_transport_t) );
    if ( !client_transport )
    {
        rocketnet_error( "could not allocate client" );
        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        return NULL;
    }

    new ( client_transport ) rocketnet_client_transport_t();

    client_transport->context = context;

    // the fragmentation shape: zero means the defaults; the table already
    // refused anything out of range, loudly, never clamped

    client_transport->max_payload_bytes = config->max_payload_bytes > 0 ? config->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int num_reassembly_slots = config->num_reassembly_slots > 0 ? config->num_reassembly_slots : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;

    // the arena's slot stride stays 16-aligned so every root handed to a
    // caller is aligned for any word-based serializer

    client_transport->send_arena = rocketnet_send_arena_create( context, ROCKETNET_SEND_ARENA_SLOTS,
                                                                ( client_transport->max_payload_bytes + 15 ) & ~15 );
    // the delivered mirror sizes to the completion bound: one update drains
    // at most ROCKETNET_NUM_CLIENT_PACKETS packets, and its completions are
    // half the drain plus the payloads begun in an earlier update (at most
    // the slot count), so BATCH_FULL stays zero under any load the drain
    // delivers -- about 8.4MB at the default maximum

    client_transport->reassembly = rocketnet_reassembly_set_create( context, 1, num_reassembly_slots, client_transport->max_payload_bytes,
                                                                    rocketnet_client_reassembly_delivered_capacity( num_reassembly_slots ) );
    if ( !client_transport->send_arena || !client_transport->reassembly )
    {
        rocketnet_error( "client could not allocate fragmentation storage" );
        rocketnet_client_transport_destroy( client_transport );
        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        return NULL;
    }

    rocketnet_packet_loss_tracker_reset( &client_transport->packet_loss_tracker );
    rocketnet_jitter_tracker_reset( &client_transport->jitter_tracker );

    // connect with the caller's token: the token names the server and is
    // the credential. the game owns the clock; unix time is read here once
    // for the token expiry check, which is wall-clock by definition.

    rocketnet_client_protocol_init( &client_transport->protocol, &client_transport->counters );

    client_transport->protocol.reassembly = client_transport->reassembly;

    memcpy( client_transport->protocol.filter_value, config->filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    // the create-time clock is checked the same way an update's is: a
    // non-finite or negative seed would poison every later comparison, so it
    // is clamped to the initial last good time before anything reads it.

    bool time_clamped = false;
    time = rocketnet_clamp_update_time( 0.0, time, &time_clamped );

    client_transport->config = *config;
    client_transport->time = time;
    client_transport->last_state = client_transport->protocol.state;

    if ( !rocketnet_client_protocol_connect( &client_transport->protocol, connect_token, time, (uint64_t) ::time( NULL ) ) )
    {
        const int connect_failure = client_transport->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED ?
            ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED : ROCKETNET_STATUS_INVALID_CONNECT_TOKEN;
        rocketnet_client_transport_destroy( client_transport );
        if ( status )
            *status = connect_failure;
        return NULL;
    }

    char string_buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client connecting to %s", rocketnet_address_to_string( &client_transport->protocol.server_address, string_buffer ) );

    // the socket binds after the token parse so the bind family can follow
    // the server when the platform has no dual stack

    rocketnet_address_t bind_address;
    memset( &bind_address, 0, sizeof(bind_address) );
    bind_address.type = ROCKETNET_ADDRESS_IPV4;

    // IMPORTANT: for many platforms it's best practice to bind to ipv6 and go dual stack on the client
    if ( rocketnet_platform_client_dual_stack() )
    {
        rocketnet_info( "client socket is dual stack ipv4 and ipv6" );
        bind_address.type = ROCKETNET_ADDRESS_IPV6;
    }
    else if ( client_transport->protocol.server_address.type == ROCKETNET_ADDRESS_IPV6 )
    {
        // no dual stack on this platform (openbsd rejects v4-mapped
        // addresses by design): one socket speaks one family, so bind
        // the server's own
        rocketnet_info( "client socket is ipv6 only" );
        bind_address.type = ROCKETNET_ADDRESS_IPV6;
    }

    // IMPORTANT: some platforms (GDK) have a preferred port that we must use to access packet tagging
    // If the bind address has set port of 0, substitute the preferred client port here
    if ( bind_address.port == 0 )
    {
        int preferred_client_port = rocketnet_platform_preferred_client_port();
        if ( preferred_client_port != 0 )
        {
            rocketnet_info( "client socket using preferred port %d", preferred_client_port );
            bind_address.port = preferred_client_port;
        }
    }

    // THE LADDER (#106), the client's short form: candidates tried left to
    // right, first grant wins, a one-element list is a pin that refuses
    // rather than falls, and the exhausted list fails loudly naming what
    // was skipped for platform and what refused for grant.

    int granted_driver = -1;

    char refused_names[128];
    refused_names[0] = '\0';

    for ( int i = 0; i < ladder.num_candidates && granted_driver < 0; i++ )
    {
        const int candidate = ladder.candidates[i];

        // a candidate whose memory plan the operator budget refused was logged
        // and refused in the plan loop above; record it for the exhausted
        // summary and fall to the next candidate, never opening it
        if ( budget_refused[candidate] )
        {
            const size_t used = strlen( refused_names );
            snprintf( refused_names + used, sizeof(refused_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( candidate ) );
            continue;
        }

        const rocketnet_driver_definition_t * definition = rocketnet_driver_definition( candidate );
        const rocketnet_client_driver_config_t driver_config = { rocketnet_driver_name(candidate), config };
        const rocketnet_client_transport_test_driver_t * seam = rocketnet_client_transport_test_driver_seam;
        if ( seam ? rocketnet_client_driver_open( seam->operations, seam->instance_context, &driver_config, &bind_address, &client_transport->io ) :
             rocketnet_client_driver_open( definition && definition->backend ? definition->backend->client : NULL,
                client_transport->context, &driver_config, &bind_address, &client_transport->io ) )
            granted_driver = candidate;

        if ( granted_driver < 0 )
        {
            rocketnet_warn( "client driver %s refused%s", rocketnet_driver_name( candidate ),
                            i + 1 < ladder.num_candidates ? ". trying the next candidate" : "" );
            const size_t used = strlen( refused_names );
            snprintf( refused_names + used, sizeof(refused_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( candidate ) );
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

        rocketnet_error( "client driver ladder exhausted with no grant. refused: %s. skipped for platform: %s",
                         refused_names[0] != '\0' ? refused_names : "(none)",
                         skipped_names[0] != '\0' ? skipped_names : "(none)" );
        rocketnet_client_transport_destroy( client_transport );
        if ( status )
            *status = ROCKETNET_STATUS_DRIVER_LADDER_EXHAUSTED;
        return NULL;
    }

    client_transport->driver = granted_driver;

    if ( refused_names[0] != '\0' )
    {
        rocketnet_info( "client driver ladder granted %s after refusals: %s", rocketnet_driver_name( granted_driver ), refused_names );
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "client bound to %s [%s]", rocketnet_address_to_string( &bind_address, address_string ), rocketnet_driver_name( granted_driver ) );

    client_transport->bound_port = bind_address.port;

    if ( !rocketnet_platform_mutex_create( &client_transport->mutex ) )
    {
        rocketnet_error( "client could not create mutex" );
        rocketnet_client_transport_destroy( client_transport );
        if ( status )
            *status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;
        return NULL;
    }

    client_transport->use_io_thread = rocketnet_client_transport_uses_io_thread( client_transport->io.operations );

    if ( client_transport->use_io_thread )
    {
        // docs/CLIENT-IO.md _The design_ and _The clock_ (RNClientIo initial):
        // the core takes both rings and both receive buffers, and the clock
        // read once here; the I/O thread replaces the receive thread

        double (*time_callback)( void * ) = config->time_callback ? config->time_callback : rocketnet_client_transport_platform_time;

        rocketnet_client_io_ring_view_t views[2];
        for ( int i = 0; i < 2; i++ )
        {
            views[i].data = client_transport->payload_ring[i].data;
            views[i].frame_stride = ROCKETNET_FRAME_STRIDE;
            views[i].wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
            views[i].wire_bytes = (const uint8_t *) &client_transport->payload_ring[i].info[0].wire_bytes;
            views[i].wire_bytes_stride = sizeof( struct rocketnet_client_payload_slot_info_t );
        }
        rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = { &client_transport->receive_buffer[0], &client_transport->receive_buffer[1] };
        rocketnet_client_io_init( &client_transport->client_io, &client_transport->io, &views[0], &views[1], buffers,
                                  time_callback, client_transport->context );

        if ( rocketnet_client_transport_test_driver_seam )
        {
            client_transport->io_step_mode = true;
            client_transport->io_running = true;
            return client_transport;
        }

        client_transport->thread = rocketnet_platform_thread_create( context, rocketnet_client_io_thread_function, &client_transport->client_io );
        if ( client_transport->thread )
            client_transport->io_running = true;
    }
    else
        client_transport->thread = rocketnet_platform_thread_create( context, rocketnet_client_thread_function, client_transport );
    if ( !client_transport->thread )
    {
        rocketnet_error( "client could not create thread" );
        rocketnet_client_transport_destroy( client_transport );
        if ( status )
            *status = ROCKETNET_STATUS_THREAD_CREATE_FAILED;
        return NULL;
    }

    return client_transport;
}

void rocketnet_client_transport_destroy( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    rocketnet_assert( client_transport );

    if ( client_transport->io_running )
    {
        // docs/CLIENT-IO.md _Stop and destroy order_: the last hand-off, then
        // Stop, Join, close the driver, free (RNClientIo
        // CommittedEventuallyHandedOff, Stop, Join, FreedOnlyAfterJoin). Only
        // destroy waits, and it is off the frame path.

        rocketnet_client_io_t * io = &client_transport->client_io;

        if ( rocketnet_client_ring_committed( &client_transport->payload_ring[io->rings.fill] ) > 0 )
        {
            const int other = io->rings.fill ^ 1;
            while ( 1 )
            {
                const int state = io->rings.state[other].load( std::memory_order_acquire );
                if ( state == ROCKETNET_CLIENT_RING_SENT || state == ROCKETNET_CLIENT_RING_FILLING )
                    break;
                if ( client_transport->io_step_mode )
                    rocketnet_client_io_step( io, false );
                else
                    rocketnet_platform_sleep( 0.0001 );
            }
            rocketnet_client_transport_io_hand_off( client_transport );
        }

        rocketnet_client_io_stop( io );

        if ( client_transport->io_step_mode )
        {
            while ( rocketnet_client_io_step( io, false ) != ROCKETNET_CLIENT_IO_EXITED ) {}
        }
        else
        {
            rocketnet_platform_thread_join( client_transport->thread );
            rocketnet_platform_thread_destroy( client_transport->thread );
        }
        client_transport->thread = NULL;
    }
    else if ( client_transport->thread )
    {
        client_transport->quit = true;
        rocketnet_platform_thread_join( client_transport->thread );
        rocketnet_platform_thread_destroy( client_transport->thread );
    }

    rocketnet_client_driver_close( &client_transport->io );

    rocketnet_platform_mutex_destroy( &client_transport->mutex );

    if ( client_transport->send_arena )
        rocketnet_send_arena_destroy( client_transport->send_arena );
    if ( client_transport->reassembly )
        rocketnet_reassembly_set_destroy( client_transport->reassembly );

    void * allocation_context = client_transport->context;
    client_transport->~rocketnet_client_transport_t();
    rocketnet_clear_and_free( allocation_context, client_transport, sizeof(rocketnet_client_transport_t) );
}

static void rocketnet_client_process_packet( rocketnet_client_transport_t * client_transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes, double current_time, double receive_time )
{
    // only the dialed server is listened to; everything else is junk

    if ( !rocketnet_address_equal( from, &client_transport->protocol.server_address ) )
        return;

    struct rocketnet_client_received_packets_t * received = &client_transport->received_packets;

    uint8_t * payload = received->num_packets < ROCKETNET_NUM_CLIENT_PACKETS ?
        received->data + received->num_packets * ROCKETNET_FRAME_SIZE : NULL;

    uint8_t overflow[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;

    rocketnet_client_protocol_process_packet( &client_transport->protocol, packet_data, packet_bytes, current_time,
                                              payload ? payload : overflow,
                                              payload ? ROCKETNET_FRAME_SIZE : ROCKETNET_RECEIVE_CEILING,
                                              &payload_bytes );

    // an accepted session packet feeds the quality trackers: loss from gaps
    // in the session sequence, jitter from the receive thread's per-packet
    // stamp -- the stamp is already taken on arrival, and only its deltas
    // matter, so it never mixes with the game's clock

    const uint64_t session_sequence = client_transport->protocol.received_session_sequence;

    if ( session_sequence != 0 )
    {
        // wire session sequences are 1-based (zero is rejected by the protocol); the
        // trackers number from a 0-based feed, so the first wire sequence maps to 0 --
        // feeding 1-based values leaves one internal slot permanently unreceived and
        // publishes phantom loss on a clean connection.
        rocketnet_packet_loss_tracker_packet_received( &client_transport->packet_loss_tracker, session_sequence - 1 );
        rocketnet_jitter_tracker_packet_received( &client_transport->jitter_tracker, session_sequence - 1, receive_time );

        const int stats_index = client_transport->current_stats_index;
        client_transport->stats_frames[stats_index].session_packets_received++;
    }

    if ( payload_bytes > 0 && payload )
    {
        const int index = received->num_packets;
        received->packet_data[index] = payload;
        received->packet_bytes[index] = payload_bytes;
        received->num_packets++;
    }

    // a fragment that completed its payload delivers here, in arrival
    // position: the reassembled bytes live in the reassembly's delivered
    // arena, valid until the next update, and join the same batch

    struct rocketnet_reassembly_set_t * reassembly = client_transport->protocol.reassembly;
    if ( reassembly && reassembly->completed_bytes > 0 )
    {
        if ( received->num_packets < ROCKETNET_NUM_CLIENT_PACKETS )
        {
            const int index = received->num_packets;
            received->packet_data[index] = reassembly->completed_payload;
            received->packet_bytes[index] = reassembly->completed_bytes;
            received->num_packets++;
        }
        else
        {
            rocketnet_spam( "client dropped reassembled payload. receive batch full" );
            client_transport->counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL]++;
        }
    }
}

static void rocketnet_client_update_swap_receive_buffers( rocketnet_client_transport_t * client_transport )
{
    rocketnet_platform_mutex_acquire( &client_transport->mutex );

    client_transport->receive_buffer_index++;
    client_transport->receive_buffer_index &= 1;

    const int off_index = client_transport->receive_buffer_index ? 0 : 1;

    client_transport->receive_buffer[off_index].num_packets = 0;

    rocketnet_platform_mutex_release( &client_transport->mutex );
}

// process the previous stats frames into published per-second numbers, then
// advance the bucket when due, then run the loss tracker's bounded sweep.
// rtt and jitter live in the protocol and the jitter tracker and are read by
// the getter; the windowed numbers -- rates, bandwidth, loss -- are computed
// here, once per update, at flat cost.

static void rocketnet_client_update_stats( rocketnet_client_transport_t * client_transport, double time )
{
    // process previous stats frames
    {
        uint64_t total_packets_sent = 0;
        uint64_t total_packets_received = 0;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;
        uint64_t total_session_packets_received = 0;
        uint64_t total_packets_lost = 0;

        for ( int i = 0; i < ROCKETNET_NUM_CLIENT_STATS_FRAMES; i++ )
        {
            // IMPORTANT: only sum the last second worth of stats frames, we want "packets sent per-second, bandwidth sent per-second etc..."
            if ( client_transport->stats_frames[i].frame == 0 || client_transport->stats_frames[i].time + 1.0 < time )
                continue;

            total_packets_sent += client_transport->stats_frames[i].packets_sent;
            total_packets_received += client_transport->stats_frames[i].packets_received;
            total_bytes_sent += client_transport->stats_frames[i].bytes_sent;
            total_bytes_received += client_transport->stats_frames[i].bytes_received;
            total_session_packets_received += client_transport->stats_frames[i].session_packets_received;
            total_packets_lost += client_transport->stats_frames[i].packets_lost;
        }

        client_transport->client_stats.packets_sent_per_second = (int) total_packets_sent;
        client_transport->client_stats.packets_received_per_second = (int) total_packets_received;

        client_transport->client_stats.bandwidth_sent_kbps = (int) ( ( (double) total_bytes_sent * 8.0 ) / 1000.0 );
        client_transport->client_stats.bandwidth_received_kbps = (int) ( ( (double) total_bytes_received * 8.0 ) / 1000.0 );

        const uint64_t loss_denominator = total_packets_lost + total_session_packets_received;

        client_transport->client_stats.packet_loss = loss_denominator > 0 ?
            (float) ( ( (double) total_packets_lost * 100.0 ) / (double) loss_denominator ) : 0.0f;
    }

    // setup next stats frame
    {
        client_transport->frame++;
        client_transport->current_stats_index = rocketnet_stats_window_advance(
            client_transport->stats_frames, client_transport->current_stats_index,
            client_transport->frame, time);
        const int stats_index = client_transport->current_stats_index;

        // the loss tracker's sweep is bounded by its safety margin and runs
        // once per update; losses it finds land in the fresh bucket

        const int lost_packets = rocketnet_packet_loss_tracker_update( &client_transport->packet_loss_tracker );

        client_transport->stats_frames[stats_index].packets_lost += (uint64_t) lost_packets;
    }

}

void rocketnet_client_transport_update( rocketnet_client_transport_t * client_transport, double time )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    rocketnet_assert( client_transport );

    // the caller's clock is clamped once, before anything reads it: a NaN,
    // an infinity or a step backwards becomes the last good time, counted and
    // loud in a debug build.

    bool time_clamped = false;
    time = rocketnet_clamp_update_time( client_transport->time, time, &time_clamped );

    client_transport->packet_leases.reset();

    const double current_time = time;
    client_transport->time = time;

    rocketnet_client_update_stats( client_transport, time );

    const int state_before = client_transport->protocol.state;

    // the acked-packet batch drains per update: what the blocks processed
    // below acknowledge lands there, valid until the next update

    client_transport->protocol.num_acked = 0;

    // the receive buffer this update processes: on the I/O path, RNClientIo
    // Expose (docs/CLIENT-IO.md _The receive path_, update step 1): a Ready
    // buffer becomes Exposed and is processed exactly as at BASE, or NULL
    // when none is Ready; on the BASE path, the swap under the mutex

    struct rocketnet_client_receive_buffer_t * receive_buffer = NULL;

    if ( client_transport->use_io_thread )
        receive_buffer = rocketnet_client_io_expose( &client_transport->client_io );
    else
    {
        rocketnet_client_update_swap_receive_buffers( client_transport );
        receive_buffer = &client_transport->receive_buffer[client_transport->receive_buffer_index];
    }

    client_transport->received_packets.num_packets = 0;
    client_transport->received_packets.receive_index = 0;

    // the reassembly's delivered window is per update: payloads completed
    // last update are invalid from here

    rocketnet_reassembly_set_begin_update( client_transport->reassembly );

    const int stats_index = client_transport->current_stats_index;

    if ( receive_buffer )
    {
        for ( int i = 0; i < receive_buffer->num_packets; i++ )
        {
            client_transport->stats_frames[stats_index].packets_received++;
            client_transport->stats_frames[stats_index].bytes_received += (uint64_t) ( receive_buffer->packet_bytes[i] + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

            rocketnet_client_process_packet( client_transport, &receive_buffer->from[i], receive_buffer->packet_data[i], (int) receive_buffer->packet_bytes[i], current_time, receive_buffer->receive_time[i] );
        }

        receive_buffer->num_packets = 0;
    }

    // the protocol's scheduled send and its deadlines

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client_transport->protocol, current_time, packet, &packet_bytes );
    if ( packet_bytes > 0 )
    {
        if ( client_transport->use_io_thread )
        {
            // counted in the send accumulation when it is handed off
            rocketnet_client_transport_io_commit_packet( client_transport, packet, packet_bytes );
        }
        else
        {
            client_transport->stats_frames[stats_index].packets_sent++;
            client_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( packet_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

            client_transport->io.operations->send( client_transport->io.instance, &client_transport->protocol.server_address, packet, packet_bytes );
        }
    }

    if ( client_transport->use_io_thread )
    {
        // RNClientIo Return (docs/CLIENT-IO.md _The receive path_, update step
        // 2), every update whether or not one was exposed: the held buffer
        // Exposed to Free with seq_cst, then the seq_cst load of the owed flag
        // and the wake when it is true. Then a hand-off attempt every frame
        // (RNClientIo HandOff or Defer): it retries deferred slots and hands
        // off this update's own packet when it committed one.

        rocketnet_client_io_return( &client_transport->client_io );
        rocketnet_client_transport_io_hand_off( client_transport );
    }

    // one callback per update crossing: enough to observe every terminal,
    // and the poll of rocketnet_client_transport_state stays the source of truth

    if ( client_transport->protocol.state != state_before && client_transport->config.state_change_callback )
        client_transport->config.state_change_callback( client_transport->context, state_before, client_transport->protocol.state );
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

    if ( packet_bytes < 1 || packet_bytes > client_transport->max_payload_bytes )
        return;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( client_transport, packet_bytes, &context );
    if ( !frame )
        return;

    memcpy( frame + context.payload_index, packet_data, packet_bytes );

    rocketnet_client_transport_finish_packet( client_transport, &context, packet_bytes );
    rocketnet_client_transport_send_packets( client_transport );
}

// The client's acquire/commit family: the same shape, layout and phase
// contract as the server's, minus the client index. The convenience send
// above rides this path and flushes immediately, preserving its
// send-it-now meaning for simple callers.

static void rocketnet_client_transport_abort_packet_unchecked( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context );

uint8_t * rocketnet_client_transport_start_packet( rocketnet_client_transport_t * client_transport, int request_bytes, struct rocketnet_packet_context_t * context )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return NULL;
    if ( !rocketnet_api_null_guard( context ) )
        return NULL;
    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    memset( context, 0, sizeof(struct rocketnet_packet_context_t) );

    if ( request_bytes < 1 || request_bytes > client_transport->max_payload_bytes )
    {
        rocketnet_spam( "client start packet refused. request out of range" );
        return NULL;
    }

    uint64_t sequence;
    if ( !rocketnet_client_protocol_start_payload( &client_transport->protocol, &sequence ) )
        return NULL;

    uint8_t * frame = NULL;

    if ( request_bytes <= ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        // at or under the wire's capacity: a real wire frame, written in
        // place, sealed in place, zero copies

        struct rocketnet_client_payload_ring_t * ring = rocketnet_client_fill_ring( client_transport );

        uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
        if ( slot >= ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
        {
            rocketnet_spam( "client payload ring full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return NULL;
        }

        ring->info[slot].wire_bytes = 0;

        frame = ring->data + slot * ROCKETNET_FRAME_STRIDE;

        context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET;
        context->payload_capacity = ROCKETNET_MAX_PAYLOAD_BYTES;
    }
    else
    {
        // above the wire's capacity: the arena backs the write, and
        // finish_packet splits into fragments -- or collapses to one wire
        // packet when the write turns out small

        frame = rocketnet_send_arena_acquire( client_transport->send_arena );
        if ( !frame )
        {
            rocketnet_spam( "client send arena full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_ARENA_FULL], 1 );
            return NULL;
        }

        context->payload_index = 0;
        context->payload_capacity = client_transport->max_payload_bytes;
    }

    rocketnet_assert( ( (uintptr_t) frame % 16 ) == 0 );

    context->frame = frame;
    context->packet_id = 0; // private lease supplies the public identifier
    context->sequence = sequence;
    context->client_index = -1;

    if ( !client_transport->packet_leases.insert(*context) )
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

    if ( !rocketnet_send_arena_owns( client_transport->send_arena, context->frame ) )
    {
        struct rocketnet_client_payload_ring_t * ring = rocketnet_client_ring_of_frame( client_transport, context->frame );
        const uint64_t slot = (uint64_t) ( context->frame - ring->data ) / ROCKETNET_FRAME_STRIDE;
        ring->info[slot].wire_bytes = 0;
    }
    // an arena slot needs no undo: the flush reclaims the whole arena

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
    if (capacity) *capacity = 0;
    if (!transport || !context || !capacity) return NULL;
    rocketnet_packet_context_t trusted;
    uint64_t backend_id = 0;
    if (!transport->packet_leases.resolve(*context, trusted, backend_id, false)) return NULL;
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
    if ( client_transport->packet_leases.take(*context, trusted, backend_id) )
        rocketnet_client_transport_abort_packet_unchecked( client_transport, &trusted );
    context->frame = NULL;
}

// The arena finish: the request was over the wire's capacity, so the
// payload sits contiguous in an arena slot. A write that turned out to fit
// one packet collapses to a single ordinary payload -- one copy into one
// wire frame, cheaper than fragmenting -- and a genuinely large one splits
// into type 9 fragments. Every wire frame is reserved before anything
// seals, so a reservation shortfall drops the payload whole; a seal
// refusal mid-split -- possible only through a fill-contract violation,
// since connections are stable during a fill -- withdraws every reserved
// frame, the committed prefix included. Returns the total wire bytes
// staged.

static int rocketnet_client_transport_finish_arena_packet( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_fill_ring( client_transport );

    if ( payload_bytes <= ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
        if ( slot >= ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
        {
            rocketnet_spam( "client payload ring full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            context->frame = NULL;
            return 0;
        }

        ring->info[slot].wire_bytes = 0;
        uint8_t * frame = ring->data + slot * ROCKETNET_FRAME_STRIDE;
        memcpy( frame + ROCKETNET_FRAME_PAYLOAD_OFFSET, context->frame, (size_t) payload_bytes );

        int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &client_transport->protocol, context->sequence,
                                                                       context->packet_id,
                                                                       frame + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                       payload_bytes, client_transport->time );
        if ( wire_bytes == 0 )
        {
            context->frame = NULL;
            return 0;
        }

        ring->info[slot].wire_bytes = wire_bytes;
        context->frame = NULL;
        return wire_bytes;
    }

    int fragment_count, fragment_stride;
    if ( !rocketnet_fragment_plan( payload_bytes, &fragment_count, &fragment_stride ) )
    {
        context->frame = NULL;
        return 0;
    }

    uint64_t slots[ROCKETNET_MAX_FRAGMENTS];
    for ( int i = 0; i < fragment_count; i++ )
    {
        slots[i] = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
        if ( slots[i] >= ROCKETNET_CLIENT_PAYLOAD_RING_SIZE )
        {
            // all or nothing: the frames already reserved stay empty and
            // the flush skips them -- a partial payload is a dead payload
            rocketnet_spam( "client payload ring full. fragmented packet dropped" );
            rocketnet_platform_atomic_add_u64( &client_transport->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            context->frame = NULL;
            return 0;
        }
        ring->info[slots[i]].wire_bytes = 0;
    }

    // fragment 0 rides the sequence start_packet burned; the rest reserve
    // a block. the payload's one packet id lives in the aggregate, and
    // surfaces when every fragment's wire packet is acknowledged.

    uint64_t base_sequence = 0;
    if ( !rocketnet_client_protocol_start_fragments( &client_transport->protocol, fragment_count - 1, &base_sequence ) )
    {
        context->frame = NULL;
        return 0;
    }

    uint32_t payload_id = rocketnet_client_protocol_next_fragment_payload_id( &client_transport->protocol );
    uint64_t aggregate = rocketnet_ack_endpoint_open_aggregate( &client_transport->protocol.ack, context->packet_id, fragment_count );

    int total_wire_bytes = 0;
    for ( int i = 0; i < fragment_count; i++ )
    {
        const uint64_t sequence = i == 0 ? context->sequence : base_sequence + (uint64_t) ( i - 1 );
        const int fragment_bytes = i < fragment_count - 1 ? fragment_stride : payload_bytes - fragment_stride * ( fragment_count - 1 );
        int wire_bytes = rocketnet_client_protocol_seal_fragment( &client_transport->protocol, sequence, payload_id,
                                                                  i, fragment_count,
                                                                  context->frame + i * fragment_stride, fragment_bytes,
                                                                  aggregate, client_transport->time,
                                                                  ring->data + slots[i] * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( wire_bytes == 0 )
        {
            for ( int j = 0; j < fragment_count; j++ )
                ring->info[slots[j]].wire_bytes = 0;
            context->frame = NULL;
            return 0;
        }
        ring->info[slots[i]].wire_bytes = wire_bytes;
        total_wire_bytes += wire_bytes;
    }

    context->frame = NULL;
    return total_wire_bytes;
}

static int rocketnet_client_transport_finish_packet_unchecked( rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    rocketnet_assert( client_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return 0;

    if ( payload_bytes < 1 || payload_bytes > context->payload_capacity )
    {
        rocketnet_client_transport_abort_packet_unchecked( client_transport, context );
        return 0;
    }

    if ( rocketnet_send_arena_owns( client_transport->send_arena, context->frame ) )
        return rocketnet_client_transport_finish_arena_packet( client_transport, context, payload_bytes );

    int wire_bytes = rocketnet_client_protocol_seal_payload_frame( &client_transport->protocol, context->sequence,
                                                                   context->packet_id,
                                                                   context->frame + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                   payload_bytes, client_transport->time );
    if ( wire_bytes == 0 )
    {
        rocketnet_client_transport_abort_packet_unchecked( client_transport, context );
        return 0;
    }

    struct rocketnet_client_payload_ring_t * ring = rocketnet_client_ring_of_frame( client_transport, context->frame );
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
    if ( !client_transport->packet_leases.take(*context, trusted, backend_id) )
    {
        context->frame = NULL;
        return 0;
    }
    const int result = rocketnet_client_transport_finish_packet_unchecked( client_transport, &trusted, payload_bytes );
    context->frame = NULL;
    return result;
}

void rocketnet_client_transport_send_packets( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    rocketnet_assert( client_transport );
    client_transport->packet_leases.reset();

    if ( client_transport->use_io_thread )
    {
        // RNClientIo HandOff or Defer (docs/CLIENT-IO.md _The hand-off and
        // the two rings_): no driver call on the main thread. The arena's
        // payloads were copied into ring frames at finish, so it is reclaimed
        // whether the hand-off went or was deferred.

        rocketnet_client_transport_io_hand_off( client_transport );
        rocketnet_send_arena_reset( client_transport->send_arena );
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
            // the payload accumulation rides the flush walk: the ring already
            // holds every committed packet's wire size, so this costs nothing

            client_transport->stats_frames[stats_index].packets_sent++;
            client_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

            client_transport->io.operations->send( client_transport->io.instance, &client_transport->protocol.server_address,
                                                   ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET,
                                                   ring->info[i].wire_bytes );
        }
    }

    ring->num_packets = 0;

    rocketnet_send_arena_reset( client_transport->send_arena );
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
    return client_transport->max_payload_bytes;
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

// the granted-tier report: the driver that won the ladder at create, by
// name -- what performance claims and operator errands attach to

const char * rocketnet_client_transport_driver( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return NULL;
    rocketnet_assert( client_transport );
    return rocketnet_driver_name( client_transport->driver );
}

int rocketnet_client_transport_client_index( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return 0;
    rocketnet_assert( client_transport );
    return client_transport->protocol.client_index;
}

void rocketnet_client_transport_disconnect( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return;
    rocketnet_assert( client_transport );

    const int state_before = client_transport->protocol.state;

    uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_client_protocol_disconnect( &client_transport->protocol, burst, &burst_count, &burst_bytes );

    // every state change reaches the callback, from update or from disconnect:
    // this transition never crosses an update, so it is reported here

    if ( client_transport->protocol.state != state_before && client_transport->config.state_change_callback )
        client_transport->config.state_change_callback( client_transport->context, state_before, client_transport->protocol.state );

    const int stats_index = client_transport->current_stats_index;

    if ( client_transport->use_io_thread )
    {
        // docs/CLIENT-IO.md _The protocol packet and disconnect_: the burst is
        // sealed into the Filling ring and ends with a hand-off attempt
        // (RNClientIo Commit, HandOff or Defer); a deferred burst leaves with
        // the next update, or with destroy's last hand-off

        for ( int i = 0; i < burst_count; i++ )
            rocketnet_client_transport_io_commit_packet( client_transport, burst + i * ROCKETNET_SEND_BUDGET, burst_bytes );

        rocketnet_client_transport_io_hand_off( client_transport );
        return;
    }

    for ( int i = 0; i < burst_count; i++ )
    {
        client_transport->stats_frames[stats_index].packets_sent++;
        client_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( burst_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

        client_transport->io.operations->send( client_transport->io.instance, &client_transport->protocol.server_address,
                                               burst + i * ROCKETNET_SEND_BUDGET,
                                               burst_bytes );
    }
}

const struct rocketnet_counters_t * rocketnet_client_transport_counters( rocketnet_client_transport_t * client_transport )
{
    if ( !rocketnet_api_null_guard( client_transport ) )
        return NULL;
    rocketnet_assert( client_transport );
    return &client_transport->counters;
}

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

    memset( stats, 0, sizeof(struct rocketnet_client_transport_stats_t) );

    g_client_ack_stats_count++;
    // the client's only reader of the ack statistics is this getter, so they are computed here (docs/CLIENT-IO-DECRYPT.md, The ack statistics)
    rocketnet_ack_endpoint_update_stats( &client_transport->protocol.ack );

    // rtt seeds from the handshake's round trips and the acks refresh it
    // continuously from there: once positive first-ack samples exist they
    // are the estimate

    stats->rtt = client_transport->protocol.ack.rtt > 0.0f ?
        client_transport->protocol.ack.rtt :
        (float) ( client_transport->protocol.rtt_estimate * 1000.0 );
    stats->jitter = (float) ( client_transport->jitter_tracker.jitter * 1000.0 );
    stats->packet_loss = client_transport->client_stats.packet_loss;

    stats->packets_sent_per_second = client_transport->client_stats.packets_sent_per_second;
    stats->packets_received_per_second = client_transport->client_stats.packets_received_per_second;
    stats->bandwidth_sent_kbps = client_transport->client_stats.bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = client_transport->client_stats.bandwidth_received_kbps;
}

static void rocketnet_client_receive_packets( rocketnet_client_transport_t * client_transport )
{
    rocketnet_assert( client_transport );

    // the mutex covers the append only, never a socket call (docs/CLIENT-IO.md, Measured)

    uint8_t scratch[ROCKETNET_MAX_PACKET_BYTES];

    while ( 1 )
    {
        // a short hold: is there room in the buffer update is not reading?

        rocketnet_platform_mutex_acquire( &client_transport->mutex );

        int off_index = client_transport->receive_buffer_index ? 0 : 1;

        const bool full = client_transport->receive_buffer[off_index].num_packets >= ROCKETNET_NUM_CLIENT_PACKETS;

        rocketnet_platform_mutex_release( &client_transport->mutex );

        if ( full )
            break;

        struct rocketnet_address_t from;
        const int packet_bytes = client_transport->io.operations->receive( client_transport->io.instance, &from, scratch, ROCKETNET_MAX_PACKET_BYTES );
        if ( packet_bytes == 0 )
            break;

        // a cheap size sanity on the receive thread; the full checks and
        // their counters run on the update thread

        if ( packet_bytes < ROCKETNET_PREFILTER_BYTES || packet_bytes > ROCKETNET_RECEIVE_CEILING )
            continue;

        const double receive_time = rocketnet_platform_time();

        rocketnet_platform_mutex_acquire( &client_transport->mutex );

        // update may have swapped while the socket call ran

        off_index = client_transport->receive_buffer_index ? 0 : 1;

        rocketnet_client_receive_buffer_t * receive_buffer = &client_transport->receive_buffer[off_index];

        if ( receive_buffer->num_packets < ROCKETNET_NUM_CLIENT_PACKETS )
        {
            const int index = receive_buffer->num_packets;

            memcpy( receive_buffer->data + index * ROCKETNET_FRAME_SIZE, scratch, packet_bytes );

            receive_buffer->from[index] = from;
            receive_buffer->receive_time[index] = receive_time;
            receive_buffer->packet_data[index] = receive_buffer->data + index * ROCKETNET_FRAME_SIZE;
            receive_buffer->packet_bytes[index] = packet_bytes;

            receive_buffer->num_packets++;
        }

        rocketnet_platform_mutex_release( &client_transport->mutex );
    }
}

static void rocketnet_client_thread_function( void * data )
{
    rocketnet_client_transport_t * client_transport = (rocketnet_client_transport_t*) data;

    while ( !client_transport->quit )
    {
        rocketnet_client_receive_packets( client_transport );

        rocketnet_platform_sleep( 0.001 );
    }
}

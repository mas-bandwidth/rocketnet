/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The protocol send reserve (proofs/tla/RNSendReserve.tla). A server
// connects one client over the loopback hub; then its io is swapped for a
// fake staged transport with a fixed number of free frames per queue. Each
// send_packets opens the budget. The game's packets (start_packet) then get
// the room less the reserve and are refused past it, counted as
// SEND_RESERVE_REFUSED, while the room the reserve keeps is still free for
// the protocol's frames. The reserve is read against its formula
// (handshakes_per_update + disconnect_packets_per_update + free slots + the
// slots in use in that queue), for one queue and for several.
//
// No sockets, no real time: loopback, a step-driven client and a fake clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_message_object.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_layers.h"
#include "rocketnet_driver.h"
#include "rocketnet_test_vectors.h"
#include "unit_shutdown.h"
#include "loopback_hub.h"
#include "rocketnet_fill_phase.h"
#include "rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

#define SR_MAX_QUEUES 3
#define SR_POOL       512

static struct
{
    int queues;                   // 0: the fake declares no queue operations
    int queue_for_all;            // the queue every address hashes to
    int room[SR_MAX_QUEUES];      // free frames per queue, as start takes them
    int starts;
    int refuse_next;              // starts the fake refuses with room left (a transport refusal)
} sr;

// the frames start hands out, outside sr so the struct carries no alignment padding (MSVC C4324)
alignas( 16 ) static uint8_t sr_pool[SR_POOL][ROCKETNET_FRAME_STRIDE];

static uint8_t * sr_start( void *, const rocketnet_address_t *, uint64_t * id )
{
    const int queue = sr.queues > 0 ? sr.queue_for_all : 0;
    if ( sr.refuse_next > 0 )
    {
        sr.refuse_next--;
        *id = 0;
        return NULL;
    }
    if ( sr.room[queue] <= 0 || sr.starts >= SR_POOL )
    {
        *id = 0;
        return NULL;
    }
    sr.room[queue]--;
    *id = (uint64_t) ++sr.starts;
    return sr_pool[sr.starts - 1];
}

static void sr_finish( void *, uint64_t, uint8_t *, int ) {}
static void sr_abort( void *, uint64_t, uint8_t * ) {}
static void sr_flush( void * ) {}
static int sr_send_room( void * ) { return sr.room[0]; }
static bool sr_send_idle( void * ) { return true; }
static int sr_send_queues( void * ) { return sr.queues; }
static int sr_send_queue_of( void *, const rocketnet_address_t * ) { return sr.queue_for_all; }
static int sr_send_room_queue( void *, int queue ) { return sr.room[queue]; }

// an update's receive: nothing arrived (a test that updates the server)
static struct rocketnet_server_socket_process_packets_t sr_received;
static struct rocketnet_server_socket_process_packets_t * sr_receive( void * )
{
    sr_received.num_packets = 0;
    return &sr_received;
}

static double sr_clock = 1000.0;
static double sr_time( void * ) { return sr_clock; }

struct sr_rig_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    struct rocketnet_server_transport_config_t config;
    struct rocketnet_server_transport_io_operations_t fake;
    const struct rocketnet_server_transport_io_operations_t * real_operations;
    void * real_instance;
};

static void sr_connect( struct sr_rig_t * rig, int max_clients )
{
    rocketnet_quiet( 1 );
    sr_clock = 1000.0;

    const uint64_t protocol_id = 0x7265736572766531ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    rocketnet_server_transport_default_config_current( &rig->config );
    rig->config.protocol_id = protocol_id;
    rig->config.max_clients = max_clients;
    memcpy( rig->config.backend_public_key, backend_public_key, sizeof( rig->config.backend_public_key ) );
    memcpy( rig->config.server_public_key, server_public_key, sizeof( rig->config.server_public_key ) );
    memcpy( rig->config.server_secret_key, server_secret_key, sizeof( rig->config.server_secret_key ) );
    snprintf( rig->config.transport, sizeof( rig->config.transport ), "%s", "loopback" );
    rig->config.loopback_hub = rocketnet_test_loopback_hub();

    rig->server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &rig->config, sizeof( rig->config ) );
    check( rig->server != NULL );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( rig->server ) );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1, 0x5eed0000feedULL, 0,
                                                     1ULL << 40, 60, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = sr_time;
    rig->client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), sr_clock );
    check( rig->client != NULL );

    bool connected = false;
    for ( int i = 0; i < 400 && !connected; i++ )
    {
        sr_clock += 0.001;
        rocketnet_server_transport_update( rig->server, sr_clock );
        rocketnet_client_transport_update( rig->client, sr_clock );
        rocketnet_server_transport_send_packets( rig->server );
        rocketnet_client_transport_send_packets( rig->client );
        connected = rocketnet_client_transport_state( rig->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                    rocketnet_server_transport_is_client_connected( rig->server, 0 );
    }
    check( connected );
}

static bool sr_prime = true;

static void sr_swap_in_fake( struct sr_rig_t * rig, int queues )
{
    memset( &rig->fake, 0, sizeof( rig->fake ) );
    rig->fake.flags = ROCKETNET_TRANSPORT_STAGED;
    rig->fake.start_packet = sr_start;
    rig->fake.finish_packet = sr_finish;
    rig->fake.abort_packet = sr_abort;
    rig->fake.flush = sr_flush;
    rig->fake.send_room = sr_send_room;
    rig->fake.send_idle = sr_send_idle;
    rig->fake.receive = sr_receive;
    sr.queues = queues;
    if ( queues > 0 )
    {
        rig->fake.send_queues = sr_send_queues;
        rig->fake.send_queue_of = sr_send_queue_of;
        rig->fake.send_room_queue = sr_send_room_queue;
    }
    const struct rocketnet_server_transport_io_operations_t * operations = &rig->fake;
    void * instance = NULL;
    rocketnet_server_transport_swap_io_for_test( rig->server, &operations, &instance );
    rig->real_operations = operations;
    rig->real_instance = instance;

    // The reserve is never more than half the largest room a queue has
    // reported (the staging). The cases set rooms a little above the reserve,
    // so they first show the server a large staging; the case of the half
    // itself does not.
    if ( sr_prime )
    {
        for ( int queue = 0; queue < SR_MAX_QUEUES; ++queue )
        {
            sr.room[queue] = 1 << 20;
        }
        rocketnet_server_transport_send_packets( rig->server );
        memset( sr.room, 0, sizeof( sr.room ) );
    }
}

static void sr_teardown( struct sr_rig_t * rig )
{
    const struct rocketnet_server_transport_io_operations_t * operations = rig->real_operations;
    void * instance = rig->real_instance;
    rocketnet_server_transport_swap_io_for_test( rig->server, &operations, &instance );
    check( operations == &rig->fake );
    rn_test_client_transport_stop_and_destroy( rig->client );
    rn_test_server_transport_stop_and_destroy( rig->server );
}

// start packets to client 0 until refused; returns the starts accepted, each aborted
static int sr_fill( struct sr_rig_t * rig, int most )
{
    int accepted = 0;
    for ( int i = 0; i < most; ++i )
    {
        struct rocketnet_packet_context_t context;
        if ( !rocketnet_server_transport_start_packet( rig->server, 0, 8, &context ) )
        {
            break;
        }
        rocketnet_server_transport_abort_packet( rig->server, &context );
        accepted++;
    }
    return accepted;
}

static void test_server_send_reserve_one_queue()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_swap_in_fake( &rig, 0 );

    // the reserve: the handshake budget, the disconnect allowance, the 3
    // free slots, and the 1 slot in use (the connected client)
    const uint64_t reserve =
        (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3 + 1;

    sr.room[0] = (int) reserve + 40;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_send_reserve_for_test( rig.server, 0 ) == reserve );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 40 );

    const uint64_t refused_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
    check( sr_fill( &rig, 1000 ) == 40 );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused_before + 1 );

    // the game's packets left the whole reserve free for the protocol's frames
    check( sr.room[0] == (int) reserve );

    // a room at or below the reserve gives the game's packets nothing
    sr.room[0] = (int) reserve;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 0 );
    check( sr_fill( &rig, 10 ) == 0 );
    sr.room[0] = 0;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 0 );

    // the next send_packets opens a new budget
    sr.room[0] = (int) reserve + 5;
    rocketnet_server_transport_send_packets( rig.server );
    check( sr_fill( &rig, 1000 ) == 5 );

    sr_teardown( &rig );
}

static void test_server_send_reserve_per_queue()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 1;
    sr_swap_in_fake( &rig, SR_MAX_QUEUES );

    // every queue keeps the fixed part (handshakes, disconnects, the 3 free
    // slots); only queue 1, where the connected client hashes, adds its slot
    const uint64_t fixed = (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3;

    sr.room[0] = 500;
    sr.room[1] = (int) fixed + 1 + 7;
    sr.room[2] = 10;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_send_reserve_for_test( rig.server, 0 ) == fixed );
    check( rocketnet_server_transport_send_reserve_for_test( rig.server, 1 ) == fixed + 1 );
    check( rocketnet_server_transport_send_reserve_for_test( rig.server, 2 ) == fixed );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 500 - fixed );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 1 ) == 7 );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 2 ) == 0 );

    // the client's queue is 1: its budget, not queue 0's larger one, bounds the game's packets
    check( sr_fill( &rig, 1000 ) == 7 );
    check( sr.room[1] == (int) fixed + 1 );
    check( sr.room[0] == 500 );

    sr_teardown( &rig );
}

// A connected client whose keep-alive is acknowledged owns its reserved
// frame: the game's first frame to it after each opening takes that frame, not the
// queue's budget, and its later frames claim the budget. Not acknowledged,
// the frame stays the protocol's.
static void test_server_send_reserve_own_frame()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_swap_in_fake( &rig, 0 );

    const uint64_t reserve =
        (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3 + 1;

    // not acknowledged: the budget alone
    rocketnet_server_transport_set_keep_alive_acked_for_test( rig.server, 0, 0 );
    sr.room[0] = (int) reserve + 6;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_send_capacity_available( rig.server ) == 6 );
    check( sr_fill( &rig, 1000 ) == 6 );

    // acknowledged: the budget and the client's own frame
    rocketnet_server_transport_set_keep_alive_acked_for_test( rig.server, 0, 1 );
    sr.room[0] = (int) reserve + 6;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 6 );
    check( rocketnet_server_transport_send_capacity_available( rig.server ) == 7 );
    const uint64_t refused_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
    check( sr_fill( &rig, 1000 ) == 7 );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused_before + 1 );
    check( sr.room[0] == (int) reserve - 1 );

    // the own frame is one per opening: the next opening has it again
    sr.room[0] = (int) reserve;
    rocketnet_server_transport_send_packets( rig.server );
    check( sr_fill( &rig, 1000 ) == 1 );

    sr_teardown( &rig );
}

// RNSendReserve NoRefresh: frames the transport retires after the budget
// opened come back to the game's packets. The budget opens at nothing (the room is the
// reserve); room that frees up after it opened is granted at the next claim,
// and the reserve is still kept.
static void test_server_send_reserve_refill()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_swap_in_fake( &rig, 0 );
    rocketnet_server_transport_set_keep_alive_acked_for_test( rig.server, 0, 0 );

    const uint64_t reserve =
        (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3 + 1;

    sr.room[0] = (int) reserve;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 0 );
    check( sr_fill( &rig, 10 ) == 0 );

    // three frames retire before the next opening
    sr.room[0] += 3;
    check( rocketnet_server_transport_send_capacity_available( rig.server ) == 3 );
    check( sr_fill( &rig, 10 ) == 3 );
    check( sr.room[0] == (int) reserve );

    sr_teardown( &rig );
}

// RNSendReserve Stage, a queue that refuses: a granted frame the transport
// refuses gives its claim back, so the next frame gets the grant. Kept, the
// claim would hold a grant no frame used until the budget opened again.
static void test_server_send_reserve_refused_start_returns_claim()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_swap_in_fake( &rig, 0 );
    rocketnet_server_transport_set_keep_alive_acked_for_test( rig.server, 0, 0 );

    const uint64_t reserve =
        (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3 + 1;

    sr.room[0] = (int) reserve + 2;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 2 );

    sr.refuse_next = 1;
    const uint64_t refused_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
    check( sr_fill( &rig, 1 ) == 0 );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused_before );

    check( sr_fill( &rig, 10 ) == 2 );
    check( sr.room[0] == (int) reserve );

    sr_teardown( &rig );
}

// RNSendReserve OwnIo and witness OwnCountFirst: an own frame lowers the
// reserve only once it has left the room. Refused by the transport, it
// stays in the reserve (its client's keep-alive is still due), so the game
// gets nothing more from a room that is only the reserve.
static void test_server_send_reserve_refused_own_frame_stays_reserved()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_swap_in_fake( &rig, 0 );

    const uint64_t reserve =
        (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3 + 1;

    rocketnet_server_transport_set_keep_alive_acked_for_test( rig.server, 0, 1 );
    sr.room[0] = (int) reserve;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_fill_own_frames_for_test( rig.server ) == 1 );

    sr.refuse_next = 1;
    check( sr_fill( &rig, 1 ) == 0 );
    check( rocketnet_server_transport_fill_own_frames_for_test( rig.server ) == 0 );
    check( sr_fill( &rig, 10 ) == 0 );
    check( sr.room[0] == (int) reserve );

    sr_teardown( &rig );
}

// With several send queues, a start refused because the destination's queue
// is full is a full staging even while another queue has room: it counts as
// SEND_RING_FULL, and a disconnect burst to that destination waits for its
// queue's room instead of being abandoned as a refused destination.
static void test_server_send_reserve_full_queue_is_full_staging()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 1;
    sr_swap_in_fake( &rig, SR_MAX_QUEUES );

    // the client's queue (1) is full; queue 0 has room
    sr.room[0] = 500;
    sr.room[1] = 0;
    sr.room[2] = 0;

    // the budget opens: the server reads the fake's queues
    rocketnet_server_transport_send_packets( rig.server );

    const uint64_t full_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    rocketnet_server_transport_disconnect_client( rig.server, 0 );
    sr_clock += 0.01;
    rocketnet_server_transport_update( rig.server, sr_clock );
    rocketnet_server_transport_send_packets( rig.server );
    check( sr.starts == 0 );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] > full_before );

    // the queue's room returns: the burst was kept, and its packets leave
    sr.room[1] = 100;
    for ( int i = 0; i < 8 && sr.starts < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS; i++ )
    {
        sr_clock += 0.01;
        rocketnet_server_transport_update( rig.server, sr_clock );
        rocketnet_server_transport_send_packets( rig.server );
    }
    check( sr.starts == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

    sr_teardown( &rig );
}

// ---------------------------------------------------------------------------
// the fill lines (sdk/rocketnet_server_transport.cpp rocketnet_server_fill_line_t)
// ---------------------------------------------------------------------------

// the phase's reserve with no own frame: the connected client's keep-alive
// is not acknowledged
static uint64_t sr_open_phase( struct sr_rig_t * rig, int budget )
{
    const uint64_t reserve =
        (uint64_t) rig->config.handshakes_per_update + (uint64_t) rig->config.disconnect_packets_per_update + 3 + 1;
    sr.room[0] = (int) reserve + budget;
    rocketnet_server_transport_send_packets( rig->server );
    check( rocketnet_server_transport_fill_budget_for_test( rig->server, 0 ) == (uint64_t) budget );
    return reserve;
}

static bool sr_start_abort( struct sr_rig_t * rig )
{
    struct rocketnet_packet_context_t context;
    if ( !rocketnet_server_transport_start_packet( rig->server, 0, 8, &context ) )
    {
        return false;
    }
    rocketnet_server_transport_abort_packet( rig->server, &context );
    return true;
}

static void sr_setup_lines( struct sr_rig_t * rig )
{
    sr_connect( rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_swap_in_fake( rig, 0 );
    rocketnet_server_transport_set_keep_alive_acked_for_test( rig->server, 0, 0 );
}

#define SR_TAIL( word ) ( (word) & 0xFFFFFFFFULL )
#define SR_HELD( word ) ( (word) >> 32 )

// RNSendReserve Size (the guard): below a chunk for every line a claim is
// one frame; at or above it, one add claims a chunk, its first frame is the
// start and the rest is the line's tail, which the next starts take without
// a claim, and the start after the tail claims the next chunk.
static void test_server_fill_chunk_guard()
{
    struct sr_rig_t rig;
    sr_setup_lines( &rig );

    sr_open_phase( &rig, 40 );
    check( sr_start_abort( &rig ) );
    check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 1 );
    check( rocketnet_server_transport_fill_tail_for_test( rig.server, -1, 0 ) == 0 );

    sr_open_phase( &rig, 1000 );
    check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 0 );
    check( sr_start_abort( &rig ) );
    const uint64_t chunk = rocketnet_server_transport_fill_claims_for_test( rig.server, 0 );
    check( chunk > 1 && chunk * ROCKETNET_FILL_PHASE_FILL_LINES <= 1000 );
    const uint64_t word = rocketnet_server_transport_fill_tail_for_test( rig.server, -1, 0 );
    check( SR_TAIL( word ) == chunk - 1 && SR_HELD( word ) == 0 );
    for ( uint64_t i = 1; i < chunk; ++i )
    {
        check( sr_start_abort( &rig ) );
        check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == chunk );
    }
    check( rocketnet_server_transport_fill_tail_for_test( rig.server, -1, 0 ) == 0 );
    check( sr_start_abort( &rig ) );
    check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 2 * chunk );

    sr_teardown( &rig );
}

// RNSendReserve End, witness NoTailReturn: a frame the transport refuses
// ends the run, and that frame's claim and the rest of the line's tail go
// back in one add; the next start claims a fresh chunk.
static void test_server_fill_chunk_tail_returned()
{
    struct sr_rig_t rig;
    sr_setup_lines( &rig );

    sr_open_phase( &rig, 1000 );
    check( sr_start_abort( &rig ) );
    const uint64_t chunk = rocketnet_server_transport_fill_claims_for_test( rig.server, 0 );
    check( chunk > 2 );

    const uint64_t refused_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
    sr.refuse_next = 1;
    check( !sr_start_abort( &rig ) );
    check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 1 );
    check( rocketnet_server_transport_fill_tail_for_test( rig.server, -1, 0 ) == 0 );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused_before );

    check( sr_start_abort( &rig ) );
    check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 1 + chunk );

    sr_teardown( &rig );
}

// RNSendReserve Refuse, witness NoFallback: a refused chunk claim is taken
// back whole and claimed again as one frame, which the budget grants.
static void test_server_fill_chunk_refused_falls_back()
{
    struct sr_rig_t rig;
    sr_setup_lines( &rig );

    sr_open_phase( &rig, 1000 );
    const uint64_t refused_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
    rocketnet_server_transport_refuse_fill_chunks_for_test( rig.server, 1 );
    check( sr_start_abort( &rig ) );
    check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 1 );
    check( rocketnet_server_transport_fill_tail_for_test( rig.server, -1, 0 ) == 0 );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused_before );

    sr_teardown( &rig );
}

// RNSendReserve TakeBack and Retest, witness NoRetry: a single claim refused
// because other fillers' claims filled the budget is retested after its take
// back; when one of those claims was taken back in between, the frame now
// under the budget is claimed again and granted, and nothing is counted
// refused. With nothing taken back the retest refuses, counted once.
static void test_server_fill_refused_claim_retested()
{
    struct sr_rig_t rig;
    sr_setup_lines( &rig );
    const rocketnet_counters_t * counters = rocketnet_server_transport_counters( rig.server );

    for ( int taken_back = 0; taken_back < 2; ++taken_back )
    {
        sr_open_phase( &rig, 3 );
        rocketnet_server_transport_add_fill_claims_for_test( rig.server, 0, 3 );   // the other fillers hold the budget
        rocketnet_server_transport_retest_take_back_for_test( rig.server, (uint64_t) taken_back );
        const uint64_t refused = counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
        const uint64_t tails = counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED_TAILS_HELD];
        if ( taken_back )
        {
            check( sr_start_abort( &rig ) );
            check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 3 );
            check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused );
        }
        else
        {
            check( !sr_start_abort( &rig ) );
            check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 3 );
            check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused + 1 );
        }
        check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED_TAILS_HELD] == tails );
        rocketnet_server_transport_add_fill_claims_for_test( rig.server, 0, 0 - rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) );
    }

    sr_teardown( &rig );
}

// RNSendReserve TailsNamed: a single claim refused while another line holds
// a tail or a chunk claim is SEND_RESERVE_REFUSED_TAILS_HELD; with none held,
// SEND_RESERVE_REFUSED.
static void test_server_fill_refusal_names_tails_held()
{
    struct sr_rig_t rig;
    sr_setup_lines( &rig );

    const int own = rocketnet_server_transport_fill_line_for_test( rig.server );
    const int other = ( own + 1 ) % ROCKETNET_FILL_PHASE_FILL_LINES;
    const rocketnet_counters_t * counters = rocketnet_server_transport_counters( rig.server );

    for ( int held = 0; held < 2; ++held )
    {
        sr_open_phase( &rig, 3 );
        const uint64_t word = held ? ( 1ULL << 32 ) : 5ULL;           // a chunk claim deciding, or a tail of 5
        rocketnet_server_transport_set_fill_tail_for_test( rig.server, other, 0, word );
        const uint64_t refused = counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
        const uint64_t tails = counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED_TAILS_HELD];
        check( sr_fill( &rig, 10 ) == 3 );
        check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused );
        check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED_TAILS_HELD] == tails + 1 );

        rocketnet_server_transport_set_fill_tail_for_test( rig.server, other, 0, 0 );
        check( !sr_start_abort( &rig ) );
        check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused + 1 );
        check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED_TAILS_HELD] == tails + 1 );
    }

    sr_teardown( &rig );
}

// The payload ring (a buffered transport, the loopback): a chunk of slots
// claimed with one add while the ring has a chunk free for every line; the
// caller's unused chunk counts as free to it; and at the flush the slots of
// the chunk no start used are empty, though an earlier phase's frames sat
// there, so only the frames this phase finished are sent.
static void test_server_fill_ring_chunk()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 640 );
    rocketnet_server_transport_send_packets( rig.server );

    const int capacity = rocketnet_server_transport_send_capacity_available( rig.server );
    check( capacity >= 8 * ROCKETNET_FILL_PHASE_FILL_LINES );

    // a phase that finishes a whole chunk, so its slots hold frames
    struct rocketnet_packet_context_t context;
    check( rocketnet_server_transport_start_packet( rig.server, 0, 8, &context ) != NULL );
    const uint64_t chunk = rocketnet_server_transport_payload_ring_count_for_test( rig.server );
    check( chunk > 1 );
    const uint64_t word = rocketnet_server_transport_fill_ring_chunk_for_test( rig.server );
    check( ( word >> 32 ) - ( word & 0xFFFFFFFFULL ) == chunk - 1 );
    check( rocketnet_server_transport_send_capacity_available( rig.server ) == capacity - 1 );
    check( rocketnet_server_transport_finish_packet( rig.server, &context, 8 ) > 0 );
    for ( uint64_t i = 1; i < chunk; ++i )
    {
        check( rocketnet_server_transport_start_packet( rig.server, 0, 8, &context ) != NULL );
        check( rocketnet_server_transport_finish_packet( rig.server, &context, 8 ) > 0 );
    }
    check( rocketnet_server_transport_payload_ring_count_for_test( rig.server ) == chunk );
    check( rocketnet_server_transport_send_capacity_available( rig.server ) == capacity - (int) chunk );

    struct rocketnet_server_transport_stats_t stats;
    rocketnet_server_transport_stats( rig.server, &stats );
    const uint64_t sent = stats.packets_sent;
    rocketnet_server_transport_send_packets( rig.server );
    rocketnet_server_transport_stats( rig.server, &stats );
    check( stats.packets_sent == sent + chunk );
    check( rocketnet_server_transport_payload_ring_count_for_test( rig.server ) == 0 );
    check( rocketnet_server_transport_fill_ring_chunk_for_test( rig.server ) == 0 );

    // the next phase finishes one frame of a new chunk over the same slots
    check( rocketnet_server_transport_start_packet( rig.server, 0, 8, &context ) != NULL );
    check( rocketnet_server_transport_finish_packet( rig.server, &context, 8 ) > 0 );
    check( rocketnet_server_transport_payload_ring_count_for_test( rig.server ) == chunk );
    rocketnet_server_transport_send_packets( rig.server );
    rocketnet_server_transport_stats( rig.server, &stats );
    check( stats.packets_sent == sent + chunk + 1 );

    rn_test_client_transport_stop_and_destroy( rig.client );
    rn_test_server_transport_stop_and_destroy( rig.server );
}

// A thread whose chunk install finds the line refilled by another thread on
// it gives its own chunk up whole and takes its slot from the line's chunk:
// the slots it takes only rise, so the flush, which walks the ring in slot
// order, sends one client's packets in the order they were started. Its
// claimed chunk's first slot used beside the other thread's lower chunk
// would leave after the packets this thread starts next.
static void test_server_fill_ring_chunk_lost_install_keeps_start_order()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 640 );
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_send_capacity_available( rig.server ) >= 8 * ROCKETNET_FILL_PHASE_FILL_LINES );

    struct rocketnet_server_transport_stats_t stats;
    rocketnet_server_transport_stats( rig.server, &stats );
    const uint64_t sent = stats.packets_sent;

    rocketnet_server_transport_steal_ring_chunk_for_test( rig.server, 1 );

    struct rocketnet_packet_context_t first;
    struct rocketnet_packet_context_t second;
    uint8_t * a = rocketnet_server_transport_start_packet( rig.server, 0, 8, &first );
    check( a != NULL );
    const uint64_t claimed = rocketnet_server_transport_payload_ring_count_for_test( rig.server );
    check( claimed > 2 );                                                           // the other thread's chunk and this one's
    uint8_t * b = rocketnet_server_transport_start_packet( rig.server, 0, 8, &second );
    check( b != NULL );
    check( rocketnet_server_transport_payload_ring_count_for_test( rig.server ) == claimed );   // the second came from the line's chunk
    check( b > a );                                                                 // the ring's frames lie in slot order
    check( rocketnet_server_transport_finish_packet( rig.server, &first, 8 ) > 0 );
    check( rocketnet_server_transport_finish_packet( rig.server, &second, 8 ) > 0 );

    // the slot the other thread took, the chunk given up and the line's
    // unused slots are all empty: two packets leave
    rocketnet_server_transport_send_packets( rig.server );
    rocketnet_server_transport_stats( rig.server, &stats );
    check( stats.packets_sent == sent + 2 );
    check( rocketnet_server_transport_payload_ring_count_for_test( rig.server ) == 0 );

    rn_test_client_transport_stop_and_destroy( rig.client );
    rn_test_server_transport_stop_and_destroy( rig.server );
}

// The reserve is never more than half of the largest room a queue has
// reported at an open: a staging smaller than twice the formula still gives
// the game half of it, and once a larger staging has been seen the reserve
// is the formula again.
static void test_server_send_reserve_never_more_than_half_the_staging()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_prime = false;
    sr_swap_in_fake( &rig, 0 );
    sr_prime = true;
    rocketnet_server_transport_set_keep_alive_acked_for_test( rig.server, 0, 0 );

    const uint64_t reserve =
        (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3 + 1;

    // a staging of the formula and 10 more: half of it is kept, half is the game's
    const int staging = (int) reserve + 10;
    sr.room[0] = staging;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_send_reserve_for_test( rig.server, 0 ) == (uint64_t) ( staging / 2 ) );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == (uint64_t) ( staging - staging / 2 ) );
    check( sr_fill( &rig, 1000 ) == staging - staging / 2 );
    check( sr.room[0] == staging / 2 );

    // frames in flight lower the room, not the staging: the reserve stays
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_send_reserve_for_test( rig.server, 0 ) == (uint64_t) ( staging / 2 ) );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 0 );

    // a staging of twice the formula or more: the formula
    sr.room[0] = 2 * (int) reserve + 7;
    rocketnet_server_transport_send_packets( rig.server );
    check( rocketnet_server_transport_send_reserve_for_test( rig.server, 0 ) == reserve );
    check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == reserve + 7 );

    sr_teardown( &rig );
}

static void test_server_send_reserve_other_queue_full_drains()
{
    // a burst waits for a queue with room while another queue is full: the
    // fewest-room send_room is 0, but the drain's allowance is every queue's
    // room, so the burst leaves (rocketnet_server_transport_io_send_room_total)
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 1;
    sr_swap_in_fake( &rig, SR_MAX_QUEUES );
    rocketnet_server_transport_send_packets( rig.server );

    // queue 1 too short for the immediate burst, so it is deferred; then
    // queue 0 is full and queue 1 has room
    sr.room[0] = 500;
    sr.room[1] = 3;
    sr.room[2] = 500;
    rocketnet_server_transport_disconnect_client( rig.server, 0 );
    check( sr.starts == 0 );
    sr.room[0] = 0;
    sr.room[1] = 100;
    const uint64_t full_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    for ( int i = 0; i < 8 && sr.starts < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS; i++ )
    {
        sr_clock += 0.01;
        rocketnet_server_transport_update( rig.server, sr_clock );
        rocketnet_server_transport_send_packets( rig.server );
    }
    check( sr.starts == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == full_before );

    sr.room[0] = 500;
    sr_teardown( &rig );
}

static void test_server_send_reserve_stop_drains_while_a_queue_has_room()
{
    // stop with a burst waiting for a queue with room while another queue is
    // full: the stop step seals it rather than abandoning it for no room
    // (ServerStopSpread AbandonNoRoom reads every queue's room)
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 1;
    sr_swap_in_fake( &rig, SR_MAX_QUEUES );
    rocketnet_server_transport_send_packets( rig.server );

    // queue 1 too short for the burst, so disconnect defers it; then at stop
    // queues 0 and 2 are full and queue 1 has room
    sr.room[0] = 500;
    sr.room[1] = 3;
    sr.room[2] = 500;
    rocketnet_server_transport_disconnect_client( rig.server, 0 );
    check( sr.starts == 0 );
    sr.room[0] = 0;
    sr.room[1] = 100;
    sr.room[2] = 0;
    const uint64_t full_before = rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    rocketnet_server_transport_stop( rig.server );
    for ( int i = 0; i < 8 && sr.starts < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS; i++ )
    {
        (void) rocketnet_server_transport_stopped( rig.server );
    }
    check( sr.starts == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( rocketnet_server_transport_counters( rig.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == full_before );

    sr.room[0] = 500;
    sr.room[2] = 500;
    sr_teardown( &rig );
}

REGISTER_UNIT_TEST( test_server_send_reserve_one_queue );
REGISTER_UNIT_TEST( test_server_send_reserve_never_more_than_half_the_staging );
REGISTER_UNIT_TEST( test_server_send_reserve_refill );
REGISTER_UNIT_TEST( test_server_send_reserve_refused_own_frame_stays_reserved );
REGISTER_UNIT_TEST( test_server_send_reserve_refused_start_returns_claim );
REGISTER_UNIT_TEST( test_server_send_reserve_own_frame );
REGISTER_UNIT_TEST( test_server_send_reserve_per_queue );
REGISTER_UNIT_TEST( test_server_send_reserve_full_queue_is_full_staging );
REGISTER_UNIT_TEST( test_server_fill_chunk_guard );
REGISTER_UNIT_TEST( test_server_fill_chunk_tail_returned );
REGISTER_UNIT_TEST( test_server_fill_chunk_refused_falls_back );
REGISTER_UNIT_TEST( test_server_fill_refused_claim_retested );
REGISTER_UNIT_TEST( test_server_fill_refusal_names_tails_held );
REGISTER_UNIT_TEST( test_server_fill_ring_chunk );
REGISTER_UNIT_TEST( test_server_fill_ring_chunk_lost_install_keeps_start_order );
REGISTER_UNIT_TEST( test_server_send_reserve_other_queue_full_drains );
REGISTER_UNIT_TEST( test_server_send_reserve_stop_drains_while_a_queue_has_room );

#endif // #if ROCKETNET_DEV

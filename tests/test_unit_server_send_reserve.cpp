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

// RNSendReserve TakeBack and Retest, witness NoRetry: a claim refused because
// other claims filled the budget is retested after its take back; when one
// of those claims was taken back in between, the frame now under the budget
// is claimed again and granted, and nothing is counted refused. With nothing
// taken back the retest refuses, counted once.
static void test_server_send_reserve_refused_claim_retested()
{
    struct sr_rig_t rig;
    sr_connect( &rig, 4 );
    memset( sr.room, 0, sizeof( sr.room ) );
    sr.starts = 0;
    sr.refuse_next = 0;
    sr.queue_for_all = 0;
    sr_swap_in_fake( &rig, 0 );
    rocketnet_server_transport_set_keep_alive_acked_for_test( rig.server, 0, 0 );
    const rocketnet_counters_t * counters = rocketnet_server_transport_counters( rig.server );

    const uint64_t reserve =
        (uint64_t) rig.config.handshakes_per_update + (uint64_t) rig.config.disconnect_packets_per_update + 3 + 1;

    for ( int taken_back = 0; taken_back < 2; ++taken_back )
    {
        sr.room[0] = (int) reserve + 3;
        rocketnet_server_transport_send_packets( rig.server );
        check( rocketnet_server_transport_fill_budget_for_test( rig.server, 0 ) == 3 );
        rocketnet_server_transport_add_fill_claims_for_test( rig.server, 0, 3 );   // other claims hold the budget
        rocketnet_server_transport_retest_take_back_for_test( rig.server, (uint64_t) taken_back );
        const uint64_t refused = counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED];
        if ( taken_back )
        {
            check( sr_fill( &rig, 1 ) == 1 );
            check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 3 );
            check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused );
        }
        else
        {
            check( sr_fill( &rig, 1 ) == 0 );
            check( rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) == 3 );
            check( counters->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] == refused + 1 );
        }
        rocketnet_server_transport_add_fill_claims_for_test( rig.server, 0, 0 - rocketnet_server_transport_fill_claims_for_test( rig.server, 0 ) );
    }

    sr_teardown( &rig );
}

REGISTER_UNIT_TEST( test_server_send_reserve_one_queue );
REGISTER_UNIT_TEST( test_server_send_reserve_refused_claim_retested );
REGISTER_UNIT_TEST( test_server_send_reserve_refill );
REGISTER_UNIT_TEST( test_server_send_reserve_refused_own_frame_stays_reserved );
REGISTER_UNIT_TEST( test_server_send_reserve_refused_start_returns_claim );
REGISTER_UNIT_TEST( test_server_send_reserve_own_frame );
REGISTER_UNIT_TEST( test_server_send_reserve_per_queue );
REGISTER_UNIT_TEST( test_server_send_reserve_full_queue_is_full_staging );

#endif // #if ROCKETNET_DEV

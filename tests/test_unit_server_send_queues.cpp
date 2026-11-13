/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// One full send queue and the others. A message server seats four clients
// (rocketnet_server_transport_seat_clients_for_test, ports 1000..1003), then
// its transport's io is swapped for a fake staged transport of three send
// queues: a client's queue is its port modulo 3, so client 2 stages in queue
// 0, clients 0 and 3 in queue 1 and client 1 in queue 2. Each queue has its
// own room; a start takes one frame of its queue's room and the flush makes
// the room whole again. Queue 0 is held full while the others have room:
//   - the message layer's send pass still writes the queued messages of the
//     clients on the other queues (proofs/tla/RNServerSendFair.tla, queues),
//     and when queue 1 takes one message a pass its two clients take turns;
//   - the maintenance sweep still sends their keep-alives
//     (proofs/tla/RNServerMaintenanceSweep.tla, queues), so a client on a
//     queue with room never goes without the server's packets;
//   - each queue's protocol reserve is recomputed at every opening, so a
//     stall leaves it where the formula puts it once the queue's staging has
//     been seen (proofs/tla/RNSendReserve.tla OpenFill).
//
// No sockets, no real time: the loopback hub, a fake io and a fake clock.

#include "test_unit_registry.h"
#include "unit_shutdown.h"
#include "loopback_hub.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_server_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

bool rocketnet_server_transport_seat_clients_for_test( struct rocketnet_server_transport_t * server, int clients );

#define SQ_CLIENTS 4
#define SQ_QUEUES  3
#define SQ_FRAMES  256
#define SQ_ROOM    4096                     // a queue's staging: well above any reserve here
#define SQ_TIME    100.0                    // the time the seam seats the clients at

static struct
{
    int room[SQ_QUEUES];                    // free frames per queue, as start takes them
    int max_room[SQ_QUEUES];                // the room the flush leaves
    bool used[SQ_FRAMES];
    uint16_t to[SQ_FRAMES];                 // the port each frame was started to
    uint64_t finished[SQ_CLIENTS];          // frames handed back finished, per client
    uint64_t refused[SQ_QUEUES];            // starts refused for a full queue
} sq;

alignas( 16 ) static uint8_t sq_frames[SQ_FRAMES][ROCKETNET_FRAME_STRIDE];

static int sq_queue_of_port( uint16_t port )
{
    return port % SQ_QUEUES;
}

static uint8_t * sq_start( void *, const rocketnet_address_t * to, uint64_t * id )
{
    *id = 0;
    const int queue = sq_queue_of_port( to->port );
    if ( sq.room[queue] <= 0 )
    {
        sq.refused[queue]++;
        return NULL;
    }
    for ( int i = 0; i < SQ_FRAMES; i++ )
    {
        if ( !sq.used[i] )
        {
            sq.used[i] = true;
            sq.to[i] = to->port;
            sq.room[queue]--;
            *id = (uint64_t) i + 1;
            return sq_frames[i];
        }
    }
    return NULL;
}

static void sq_finish( void *, uint64_t id, uint8_t *, int )
{
    if ( id >= 1 && id <= SQ_FRAMES )
    {
        const int client = sq.to[id - 1] - 1000;
        if ( client >= 0 && client < SQ_CLIENTS )
        {
            sq.finished[client]++;
        }
        sq.used[id - 1] = false;
    }
}

static void sq_abort( void *, uint64_t id, uint8_t * )
{
    if ( id >= 1 && id <= SQ_FRAMES )
    {
        sq.used[id - 1] = false;
    }
}

static void sq_flush( void * )
{
    for ( int queue = 0; queue < SQ_QUEUES; queue++ )
    {
        sq.room[queue] = sq.max_room[queue];
    }
}

// the transports' own send_room: the fewest free frames of any queue
static int sq_send_room( void * )
{
    int room = sq.room[0];
    for ( int queue = 1; queue < SQ_QUEUES; queue++ )
    {
        room = sq.room[queue] < room ? sq.room[queue] : room;
    }
    return room;
}

static bool sq_send_idle( void * ) { return true; }
static int sq_send_queues( void * ) { return SQ_QUEUES; }
static int sq_send_queue_of( void *, const rocketnet_address_t * to ) { return sq_queue_of_port( to->port ); }
static int sq_send_room_queue( void *, int queue ) { return sq.room[queue]; }

static struct rocketnet_server_socket_process_packets_t sq_received;
static struct rocketnet_server_socket_process_packets_t * sq_receive( void * )
{
    sq_received.num_packets = 0;
    return &sq_received;
}

struct sq_rig_t
{
    struct rocketnet_message_server_t * server;
    struct rocketnet_server_transport_t * transport;
    struct rocketnet_server_transport_io_operations_t fake;
    const struct rocketnet_server_transport_io_operations_t * real_operations;
    void * real_instance;
    double time;
};

static void sq_set_rooms( int room0, int room1, int room2 )
{
    sq.max_room[0] = room0;
    sq.max_room[1] = room1;
    sq.max_room[2] = room2;
    memcpy( sq.room, sq.max_room, sizeof( sq.room ) );
}

// one update and its send, at the next tick of a 60 Hz clock
static void sq_tick( struct sq_rig_t * rig )
{
    rig->time += 1.0 / 60.0;
    rocketnet_message_server_update( rig->server, rig->time );
    rocketnet_message_server_send_packets( rig->server );
}

// prime: whether each queue first shows the server a large staging (the
// reserve is never more than half the largest room seen at an opening)
static void sq_setup( struct sq_rig_t * rig, bool prime )
{
    rocketnet_quiet( 1 );
    memset( &sq, 0, sizeof( sq ) );
    memset( rig, 0, sizeof( *rig ) );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = SQ_CLIENTS;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();

    // no send budget: the transport's capacity alone decides a start
    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    message_config.server_send_kbps = 0;

    rig->server = rocketnet_message_server_create( NULL, "127.0.0.1:0", &config, &message_config, NULL, 0, 0 );
    check( rig->server != NULL );
    rig->transport = rocketnet_message_server_transport( rig->server );
    check( rig->transport != NULL );
    check( rocketnet_server_transport_seat_clients_for_test( rig->transport, SQ_CLIENTS ) );

    rig->fake.flags = ROCKETNET_TRANSPORT_STAGED;
    rig->fake.start_packet = sq_start;
    rig->fake.finish_packet = sq_finish;
    rig->fake.abort_packet = sq_abort;
    rig->fake.flush = sq_flush;
    rig->fake.send_room = sq_send_room;
    rig->fake.send_idle = sq_send_idle;
    rig->fake.receive = sq_receive;
    rig->fake.send_queues = sq_send_queues;
    rig->fake.send_queue_of = sq_send_queue_of;
    rig->fake.send_room_queue = sq_send_room_queue;
    const struct rocketnet_server_transport_io_operations_t * operations = &rig->fake;
    void * instance = NULL;
    rocketnet_server_transport_swap_io_for_test( rig->transport, &operations, &instance );
    rig->real_operations = operations;
    rig->real_instance = instance;

    rig->time = SQ_TIME;
    if ( prime )
    {
        // the message layer observes its clients; both queues have room
        sq_set_rooms( SQ_ROOM, SQ_ROOM, SQ_ROOM );
        for ( int i = 0; i < 4; i++ )
        {
            sq_tick( rig );
        }
        for ( int c = 0; c < SQ_CLIENTS; c++ )
        {
            check( rocketnet_message_server_client_connected( rig->server, c ) );
        }
    }
}

static void sq_teardown( struct sq_rig_t * rig )
{
    const struct rocketnet_server_transport_io_operations_t * operations = rig->real_operations;
    void * instance = rig->real_instance;
    rocketnet_server_transport_swap_io_for_test( rig->transport, &operations, &instance );
    check( operations == &rig->fake );
    rn_test_message_server_stop_and_destroy( rig->server );
}

static void sq_send_each( struct sq_rig_t * rig, const bool * to )
{
    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for ( int c = 0; c < SQ_CLIENTS; c++ )
    {
        if ( to[c] )
        {
            check( rocketnet_message_server_send( rig->server, c, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, data, (int) sizeof( data ) ) != 0 );
        }
    }
}

// The message layer's send pass with queue 0 full and the others open: the
// messages queued for the clients of queues 1 and 2 are written this pass,
// and nothing is started on the full queue (no refusal is counted for it).
static void test_server_send_pass_continues_past_a_full_queue()
{
    struct sq_rig_t rig;
    sq_setup( &rig, true );

    sq_set_rooms( 0, SQ_ROOM, SQ_ROOM );
    rig.time += 1.0 / 60.0;
    rocketnet_message_server_update( rig.server, rig.time );

    const bool all[SQ_CLIENTS] = { true, true, true, true };
    sq_send_each( &rig, all );

    uint64_t before[SQ_CLIENTS];
    memcpy( before, sq.finished, sizeof( before ) );
    const uint64_t ring_full = rocketnet_server_transport_counters( rig.transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    rocketnet_message_server_send_packets( rig.server );

    check( sq.finished[0] > before[0] );
    check( sq.finished[1] > before[1] );
    check( sq.finished[3] > before[3] );
    check( sq.finished[2] == before[2] );
    check( sq.refused[0] == 0 );
    check( rocketnet_server_transport_counters( rig.transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full );

    // the queue's room returns: the client it held is served
    sq_set_rooms( SQ_ROOM, SQ_ROOM, SQ_ROOM );
    memcpy( before, sq.finished, sizeof( before ) );
    sq_tick( &rig );
    check( sq.finished[2] > before[2] );

    sq_teardown( &rig );
}

// Queue 0 full for many updates, queue 2 open, and queue 1 taking one game
// frame a pass (its room is its reserve and one frame more): its two clients
// (0 and 3), each sent a message every update, take turns. A cursor held on
// the client of the full queue would leave the pass's order the same every
// update, and the client of queue 1 that comes second would never be served.
static void test_server_send_pass_rotates_past_a_full_queue()
{
    struct sq_rig_t rig;
    sq_setup( &rig, true );

    const int reserve1 = (int) rocketnet_server_transport_send_reserve_for_test( rig.transport, 1 );
    check( reserve1 > 0 );
    sq_set_rooms( 0, reserve1 + 1, SQ_ROOM );

    const bool queue1[SQ_CLIENTS] = { true, false, false, true };
    const int updates = 120;
    uint64_t messages[SQ_CLIENTS] = {};
    for ( int u = 0; u < updates; u++ )
    {
        rig.time += 1.0 / 60.0;
        rocketnet_message_server_update( rig.server, rig.time );
        sq_send_each( &rig, queue1 );
        uint64_t before[SQ_CLIENTS];
        memcpy( before, sq.finished, sizeof( before ) );
        rocketnet_message_server_send_packets( rig.server );
        for ( int c = 0; c < SQ_CLIENTS; c++ )
        {
            messages[c] += sq.finished[c] - before[c];
        }
    }
    check( messages[0] >= updates / 4 );
    check( messages[3] >= updates / 4 );
    check( messages[2] == 0 );

    sq_teardown( &rig );
}

// The maintenance sweep with queue 0 full for 20 seconds of updates: every
// client of the other queues is sent its keep-alive at the keep-alive rate,
// so the longest run of updates without a packet to it stays a few updates,
// far below a client's timeout.
static void test_server_sweep_serves_clients_past_a_full_queue()
{
    struct sq_rig_t rig;
    sq_setup( &rig, true );

    sq_set_rooms( 0, SQ_ROOM, SQ_ROOM );
    const int updates = 20 * 60;
    int gap[SQ_CLIENTS] = {};
    int longest[SQ_CLIENTS] = {};
    for ( int u = 0; u < updates; u++ )
    {
        uint64_t before[SQ_CLIENTS];
        memcpy( before, sq.finished, sizeof( before ) );
        sq_tick( &rig );
        for ( int c = 0; c < SQ_CLIENTS; c++ )
        {
            gap[c] = sq.finished[c] > before[c] ? 0 : gap[c] + 1;
            longest[c] = gap[c] > longest[c] ? gap[c] : longest[c];
        }
    }
    check( longest[0] <= 60 );
    check( longest[1] <= 60 );
    check( longest[3] <= 60 );
    check( longest[2] == updates );
    check( sq.refused[0] == 0 );

    sq_teardown( &rig );
}

// The reserve over a stall: each queue's reserve is computed again at every
// opening, so a queue held full keeps it, and a queue whose first openings
// saw almost no staging (the reserve cut to half of it, 0) has it back at the
// first opening that sees the staging.
static void test_server_send_reserve_restored_after_a_stall()
{
    struct sq_rig_t rig;
    sq_setup( &rig, false );

    // the first openings see one free frame on queue 0: half of it is 0
    sq_set_rooms( 1, SQ_ROOM, SQ_ROOM );
    for ( int i = 0; i < 4; i++ )
    {
        sq_tick( &rig );
    }
    check( rocketnet_server_transport_send_reserve_for_test( rig.transport, 0 ) == 0 );

    // the stall clears: the reserve is the formula again, as on queue 2
    // (one client in each of the two)
    const uint64_t formula = rocketnet_server_transport_send_reserve_for_test( rig.transport, 2 );
    check( formula > 0 );
    sq_set_rooms( SQ_ROOM, SQ_ROOM, SQ_ROOM );
    sq_tick( &rig );
    check( rocketnet_server_transport_send_reserve_for_test( rig.transport, 0 ) == formula );

    // held full for many openings now: the reserve stays
    sq_set_rooms( 0, SQ_ROOM, SQ_ROOM );
    for ( int i = 0; i < 30; i++ )
    {
        sq_tick( &rig );
        check( rocketnet_server_transport_send_reserve_for_test( rig.transport, 0 ) == formula );
    }
    sq_set_rooms( SQ_ROOM, SQ_ROOM, SQ_ROOM );
    sq_tick( &rig );
    check( rocketnet_server_transport_send_reserve_for_test( rig.transport, 0 ) == formula );

    sq_teardown( &rig );
}

// Stop with queue 0 jammed (no room, and the transport idle: nothing in
// flight will free any) while the others have room: the bursts of the
// clients on the queues with room are sealed whole, though the jammed
// queue's burst lies between them in the ring; the jammed queue's burst is
// abandoned and counted as a full staging (SEND_RING_FULL); and the stop
// finishes in a bounded number of steps.
static void test_server_stop_finishes_past_a_jammed_queue()
{
    struct sq_rig_t rig;
    sq_setup( &rig, true );

    sq_set_rooms( 0, SQ_ROOM, SQ_ROOM );
    uint64_t before[SQ_CLIENTS];
    memcpy( before, sq.finished, sizeof( before ) );
    const uint64_t ring_full = rocketnet_server_transport_counters( rig.transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];

    // the stop as rocketnet_server_stop drives it: every slot ends with its
    // burst waiting in the ring (clients 0, 1, 2, 3 in that order), then the
    // transport stops
    for ( int c = 0; c < SQ_CLIENTS; c++ )
    {
        rocketnet_server_transport_disconnect_client_shutdown( rig.transport, c );
    }
    rocketnet_server_transport_stop( rig.transport );
    bool stopped = false;
    for ( int step = 0; step < 4 * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS && !stopped; step++ )
    {
        stopped = rocketnet_server_transport_stopped( rig.transport );
    }
    check( stopped );
    check( sq.finished[0] - before[0] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( sq.finished[1] - before[1] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( sq.finished[3] - before[3] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( sq.finished[2] == before[2] );
    check( rocketnet_server_transport_counters( rig.transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL] > ring_full );

    // every queue has its room back before the teardown
    sq_set_rooms( SQ_ROOM, SQ_ROOM, SQ_ROOM );
    sq_teardown( &rig );
}

// The same stop before any update: the io's queue count is read when the io
// is swapped in (and when a real one opens at create), not first at the
// update's opening of the budget, so a stop before the first update judges
// room per queue too: the bursts of the queues with room are sealed whole
// and the jammed queue's burst is abandoned.
static void test_server_stop_before_any_update_finishes_past_a_jammed_queue()
{
    struct sq_rig_t rig;
    sq_setup( &rig, false );

    sq_set_rooms( 0, SQ_ROOM, SQ_ROOM );
    const uint64_t ring_full = rocketnet_server_transport_counters( rig.transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    for ( int c = 0; c < SQ_CLIENTS; c++ )
    {
        rocketnet_server_transport_disconnect_client_shutdown( rig.transport, c );
    }
    rocketnet_server_transport_stop( rig.transport );
    bool stopped = false;
    for ( int step = 0; step < 4 * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS && !stopped; step++ )
    {
        stopped = rocketnet_server_transport_stopped( rig.transport );
    }
    check( stopped );
    check( sq.finished[0] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( sq.finished[1] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( sq.finished[3] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( sq.finished[2] == 0 );
    check( rocketnet_server_transport_counters( rig.transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL] > ring_full );

    sq_set_rooms( SQ_ROOM, SQ_ROOM, SQ_ROOM );
    sq_teardown( &rig );
}

// The first update's maintenance sweep, before any opening of the budget,
// with queue 0 full: the clients of the other queues are sent their
// keep-alives in that update, and nothing is started on the full queue.
static void test_server_first_update_sweep_serves_past_a_full_queue()
{
    struct sq_rig_t rig;
    sq_setup( &rig, false );

    sq_set_rooms( 0, SQ_ROOM, SQ_ROOM );
    rig.time += 1.0;
    rocketnet_message_server_update( rig.server, rig.time );
    check( sq.finished[0] > 0 );
    check( sq.finished[1] > 0 );
    check( sq.finished[3] > 0 );
    check( sq.finished[2] == 0 );
    check( sq.refused[0] == 0 );

    sq_set_rooms( SQ_ROOM, SQ_ROOM, SQ_ROOM );
    sq_teardown( &rig );
}

REGISTER_UNIT_TEST( test_server_stop_finishes_past_a_jammed_queue );
REGISTER_UNIT_TEST( test_server_stop_before_any_update_finishes_past_a_jammed_queue );
REGISTER_UNIT_TEST( test_server_first_update_sweep_serves_past_a_full_queue );
REGISTER_UNIT_TEST( test_server_send_pass_continues_past_a_full_queue );
REGISTER_UNIT_TEST( test_server_send_pass_rotates_past_a_full_queue );
REGISTER_UNIT_TEST( test_server_sweep_serves_clients_past_a_full_queue );
REGISTER_UNIT_TEST( test_server_send_reserve_restored_after_a_stall );

#endif // #if ROCKETNET_DEV

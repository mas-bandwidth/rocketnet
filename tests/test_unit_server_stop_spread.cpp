/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The server's disconnect bursts spread across updates (proofs/tla/ServerStopSpread.tla):
// a fake staged transport with a fixed number of frames per flush stands in
// for a staged bank; the stop, a kick and the capacity report run against
// it. No socket, no thread, no wall clock: the clock argument starts at
// 1000.0. This file is compiled into the loopback translation unit, which
// includes the server transport.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_internal.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_memory.h"
#include "suite_runner.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const int STOP_SPREAD_MAX_CLIENTS = 8;
static const int STOP_SPREAD_MAX_PACKETS = 4 * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * STOP_SPREAD_MAX_CLIENTS;

// start_packet hands back a FRAME_STRIDE root the payload path asserts is
// 16-byte aligned; every start shares this one frame, read at its finish
alignas( 16 ) static uint8_t stop_spread_frame[ROCKETNET_FRAME_STRIDE];

struct stop_spread_fixture_t
{
    rocketnet_address_t reserved_to;
    int capacity;                                       // frames the staging takes per flush
    int used;                                           // frames started since the last flush
    int refused_for_room;                               // starts called with the staging full
    int unreachable_port;                               // a destination with no link-layer address, 0: none
    int refused_port;                                   // a destination the socket cannot carry, 0: none
    int stops;                                          // the I/O told to stop
    bool hold_flushed;                                  // a flush hands frames on without finishing them
    int in_flight;                                      // frames flushed and not yet finished
    int staged[STOP_SPREAD_MAX_CLIENTS];                // disconnect packets staged, by client
    int flushed[STOP_SPREAD_MAX_CLIENTS];               // disconnect packets flushed, by client
    int control[STOP_SPREAD_MAX_CLIENTS];               // other frames finished (keep-alives, acks), by client
    int order[STOP_SPREAD_MAX_PACKETS];                 // the client of each disconnect packet, in seal order
    int num_order;
    rocketnet_server_socket_process_packets_t received;
};

static void stop_spread_destroy( void * ) {}
static void stop_spread_stop( void * p ) { ( (stop_spread_fixture_t *) p )->stops++; }
static bool stop_spread_stopped( void * p ) { return ( (stop_spread_fixture_t *) p )->stops > 0; }
static void stop_spread_send_batch( void *, const rocketnet_platform_send_t *, int ) {}
static int stop_spread_receive_batch( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) { return 0; }
static rocketnet_server_socket_process_packets_t * stop_spread_receive( void * p )
{
    stop_spread_fixture_t * f = (stop_spread_fixture_t *) p;
    f->received.num_packets = 0;
    return &f->received;
}

static uint8_t * stop_spread_start_packet( void * p, const rocketnet_address_t * to, uint64_t * id )
{
    stop_spread_fixture_t * f = (stop_spread_fixture_t *) p;
    if ( f->unreachable_port != 0 && to->port == f->unreachable_port )
    {
        *id = ROCKETNET_TRANSPORT_PACKET_UNREACHABLE;
        return NULL;
    }
    if ( f->refused_port != 0 && to->port == f->refused_port )
    {
        return NULL;
    }
    if ( f->used >= f->capacity )
    {
        f->refused_for_room++;
        return NULL;
    }
    f->reserved_to = *to;
    *id = (uint64_t) f->used++;

    return stop_spread_frame;
}

// a started frame stays claimed until the flush, finished or aborted, as on
// every real staged transport
static void stop_spread_finish_packet( void * p, uint64_t, uint8_t * frame, int )
{
    stop_spread_fixture_t * f = (stop_spread_fixture_t *) p;
    const int client = f->reserved_to.port - 1000;
    rocketnet_unit_check( client >= 0 && client < STOP_SPREAD_MAX_CLIENTS );
    if ( frame[ROCKETNET_FRAME_WIRE_OFFSET] != ROCKETNET_CONNECTION_DISCONNECT_PACKET )
    {
        f->control[client]++;
        return;
    }
    f->staged[client]++;
    rocketnet_unit_check( f->num_order < STOP_SPREAD_MAX_PACKETS );
    f->order[f->num_order++] = client;
}

static void stop_spread_abort_packet( void *, uint64_t, uint8_t * ) {}

// a flush hands the transport every staged frame, and the transport finishes
// them before the next step: the staging is whole again
static void stop_spread_flush( void * p )
{
    stop_spread_fixture_t * f = (stop_spread_fixture_t *) p;
    for ( int i = 0; i < STOP_SPREAD_MAX_CLIENTS; i++ )
    {
        f->flushed[i] += f->staged[i];
        f->staged[i] = 0;
    }
    if ( f->hold_flushed )
    {
        f->in_flight += f->used;
    }
    f->used = 0;
}

static bool stop_spread_send_idle( void * p )
{
    stop_spread_fixture_t * f = (stop_spread_fixture_t *) p;
    return f->used == 0 && f->in_flight == 0;
}

static int stop_spread_send_room( void * p )
{
    stop_spread_fixture_t * f = (stop_spread_fixture_t *) p;
    return f->capacity - f->used;
}

static int stop_spread_flushed( const stop_spread_fixture_t * f )
{
    int total = 0;
    for ( int i = 0; i < STOP_SPREAD_MAX_CLIENTS; i++ )
    {
        total += f->flushed[i];
    }
    return total;
}

// Slot i connected from port 1000 + i, as the handshake would leave it.
static void stop_spread_connect( rocketnet_server_transport_t * server, int i )
{
    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    address.port = (uint16_t) ( 1000 + i );

    const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 1000.0 );
    rocketnet_unit_check( index >= 0 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, index );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    const uint64_t owner_generation = rocketnet_encryption_map_owner_generation( server->protocol.encryption_map, index );
    rocketnet_unit_check( rocketnet_client_slots_assign( server->protocol.client_slots, (uint64_t) ( i + 1 ), index,
                                                         owner_generation, user_data ) == i );
    mapping->control->client_index = i;
    mapping->client_id = (uint64_t) ( i + 1 );
    mapping->control->established = 1;
    mapping->control->timeout_seconds = 1000;
    memset( &mapping->keys, i + 1, sizeof( mapping->keys ) );
}

// A server transport of `clients` connected slots (ports 1000 + i) over the
// fake staged transport; report_room offers the send_room operation.
static rocketnet_server_transport_t * stop_spread_server( stop_spread_fixture_t * f, rocketnet_server_transport_io_operations_t * operations,
                                                          int clients, bool report_room )
{
    rocketnet_unit_check( clients <= STOP_SPREAD_MAX_CLIENTS );

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config_current( &config );
    config.max_clients = clients;
    config.protocol_id = 1;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();

    // production create refuses dev keys and zero keys; any non-zero, non-dev
    // bytes satisfy the key check for this fixture, which runs no handshake

    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );

    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    rocketnet_unit_check( server );
    rn_test_server_io_stop_and_close( &server->io );

    memset( operations, 0, sizeof( *operations ) );
    operations->flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
    operations->destroy = stop_spread_destroy;
    operations->stop = stop_spread_stop;
    operations->stopped = stop_spread_stopped;
    operations->send_batch = stop_spread_send_batch;
    operations->receive_batch = stop_spread_receive_batch;
    operations->start_packet = stop_spread_start_packet;
    operations->finish_packet = stop_spread_finish_packet;
    operations->abort_packet = stop_spread_abort_packet;
    operations->flush = stop_spread_flush;
    operations->receive = stop_spread_receive;
    operations->send_room = report_room ? stop_spread_send_room : NULL;
    operations->send_idle = report_room ? stop_spread_send_idle : NULL;
    server->io.instance = f;
    server->io.operations = operations;

    for ( int i = 0; i < clients; i++ )
    {
        stop_spread_connect( server, i );
    }

    return server;
}

// The stop as rocketnet_server_stop drives it: every connected slot ends
// with its burst waiting, then the transport stops.
static void stop_spread_stop_server( rocketnet_server_transport_t * server, int clients )
{
    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_server_transport_disconnect_client_shutdown( server, i );
    }
    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_server_transport_stop( server );
}

// Eight clients owe 80 disconnect packets and the staging takes 24 a flush:
// the stop call stages 24, and each update after it stages what the room
// takes until every client has its whole burst, then the I/O stops and the
// transport reaches STOPPED. No start is ever refused for room, and every
// client's first packet leaves before any client's second.
static void test_server_stop_spreads_disconnect_bursts_across_updates()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 24;
    const int clients = 8;
    const int owed = clients * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
    rocketnet_unit_check( owed > f->capacity );

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );

    stop_spread_stop_server( server, clients );

    // the stop call is the first step: as much as the staging takes, then a flush

    rocketnet_unit_check( stop_spread_flushed( f ) == f->capacity );
    rocketnet_unit_check( f->stops == 0 );
    rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPING );

    double time = 1000.0;
    int updates = 0;
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED && updates < 16 )
    {
        time += 1.0 / 60.0;
        rocketnet_server_transport_update( server, time );
        updates++;
    }

    rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_unit_check( f->stops == 1 );
    rocketnet_unit_check( f->refused_for_room == 0 );
    rocketnet_unit_check( stop_spread_flushed( f ) == owed );
    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_unit_check( f->flushed[i] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    }

    // ceil( 80 / 24 ) = 4 steps: the stop call and three more (the state read
    // in the loop condition is a step as well, so the updates are at most that)

    rocketnet_unit_check( updates >= 1 && updates <= 3 );

    // one packet per client in turn: the first eight packets are eight clients

    bool seen[STOP_SPREAD_MAX_CLIENTS] = {};
    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_unit_check( !seen[f->order[i]] );
        seen[f->order[i]] = true;
    }

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A stop is complete only once its bursts are: stopped is false, and each
// poll is the next step, until the last burst is flushed and the I/O stops.
static void test_server_stop_polls_are_the_stop_steps()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
    const int clients = 3;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );

    stop_spread_stop_server( server, clients );
    rocketnet_unit_check( stop_spread_flushed( f ) == 10 );

    rocketnet_unit_check( !rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( stop_spread_flushed( f ) == 20 && f->stops == 0 );

    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( stop_spread_flushed( f ) == 30 && f->stops == 1 );
    rocketnet_unit_check( f->refused_for_room == 0 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A destination the transport has no link-layer address for can never be
// sent to: its burst is abandoned at the refused start, the other clients
// get theirs whole, and the stop finishes.
static void test_server_stop_abandons_an_unreachable_burst()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 8;
    f->unreachable_port = 1001;
    const int clients = 3;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );

    stop_spread_stop_server( server, clients );

    for ( int poll = 0; poll < 8 && !rocketnet_server_transport_stopped( server ); poll++ )
    {
    }

    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->flushed[0] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( f->flushed[1] == 0 );
    rocketnet_unit_check( f->flushed[2] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( f->refused_for_room == 0 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A staging that reports no send room with nothing staged or in flight (an
// xdp queue in hard send error whose producer is full, which its send_idle
// gives up on): no waiting burst can ever leave. The stop abandons each one,
// as an unreachable destination's, counted as a start refused for a full
// staging, wipes its key and finishes. It used to stay not-stopped forever.
static void test_server_stop_abandons_bursts_without_send_room()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 0;
    const int clients = 2;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t ring_full = counters[ROCKETNET_COUNTER_SEND_RING_FULL];

    stop_spread_stop_server( server, clients );

    for ( int poll = 0; poll < 8 && !rocketnet_server_transport_stopped( server ); poll++ )
    {
    }

    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->stops == 1 );
    rocketnet_unit_check( stop_spread_flushed( f ) == 0 );
    rocketnet_unit_check( server->disconnect_burst_count == 0 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + (uint64_t) clients );
    const uint8_t zero_key[ROCKETNET_KEY_BYTES] = {};
    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_unit_check( memcmp( server->disconnect_burst_keys[i], zero_key, ROCKETNET_KEY_BYTES ) == 0 );
    }

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// No send room while a send is still in flight: its finish frees room, so
// the stop waits and abandons nothing; once there is room the bursts leave
// whole and the stop finishes.
static void test_server_stop_without_room_waits_for_sends_in_flight()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 0;
    f->in_flight = 1;
    const int clients = 2;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t ring_full = counters[ROCKETNET_COUNTER_SEND_RING_FULL];

    stop_spread_stop_server( server, clients );

    for ( int poll = 0; poll < 4; poll++ )
    {
        rocketnet_unit_check( !rocketnet_server_transport_stopped( server ) );
    }
    rocketnet_unit_check( f->stops == 0 );
    rocketnet_unit_check( server->disconnect_burst_count == clients );

    f->in_flight = 0;
    f->capacity = 64;
    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->stops == 1 );
    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_unit_check( f->flushed[i] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    }
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A kick on a running server whose staging has less room than a burst: the
// slot ends at once, the burst waits, and each update seals what the room
// takes until the whole burst has left. Nothing is refused or dropped.
static void test_server_kick_spreads_its_burst_across_updates()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 4;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );

    double time = 1000.0;
    rocketnet_server_transport_disconnect_client( server, 0 );
    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_unit_check( f->num_order == 0 );

    int updates = 0;
    while ( stop_spread_flushed( f ) < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS && updates < 8 )
    {
        const int before = stop_spread_flushed( f );
        time += 1.0 / 60.0;
        rocketnet_server_transport_update( server, time );
        rocketnet_unit_check( stop_spread_flushed( f ) - before <= f->capacity );
        updates++;
    }

    rocketnet_unit_check( f->flushed[0] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( updates == ( ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 3 ) / 4 );
    rocketnet_unit_check( f->refused_for_room == 0 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// send_capacity_available on a staged transport is the transport's own
// room: the staging's free frames, 0 when full, whole again after a flush,
// and unbounded for a transport that does not report it.
static void test_server_send_capacity_reports_staged_room()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 24;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );

    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == 24 );

    for ( int i = 0; i < 5; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
        rocketnet_unit_check( frame );
        memset( frame, i, 16 );
        rocketnet_unit_check( rocketnet_server_transport_finish_packet( server, &context, 16 ) > 0 );
    }

    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == 19 );

    f->used = f->capacity;
    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == 0 );

    // the flush opens the budget: the room less the protocol reserve
    // (RNSendReserve: handshakes and disconnect seals per update, plus the
    // one connected slot, never more than half the staging). A staging of 24
    // is less than twice that, so half of it is kept

    const int formula = ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE + ROCKETNET_DEFAULT_SERVER_DISCONNECT_PACKETS_PER_UPDATE + 1;
    rocketnet_server_transport_send_packets( server );
    rocketnet_unit_check( (int) server->send_reserve[0] == 12 );
    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == 12 );

    // a staging of twice the reserve and 24 more: the whole reserve is kept

    f->capacity = 2 * formula + 24;
    rocketnet_server_transport_send_packets( server );
    const int reserve = (int) server->send_reserve[0];
    rocketnet_unit_check( reserve == formula );
    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == formula + 24 );

    // a transport that does not report its room is unbounded, less the reserve

    operations.send_room = NULL;
    rocketnet_server_transport_send_packets( server );
    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == INT_MAX - reserve );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A refused start is counted by what refused it: a full staging is
// SEND_RING_FULL, a destination with no link-layer address
// SEND_NO_LINK_ADDRESS, and a destination the socket cannot carry, with room
// left, neither (the transport counts it in its own send_failures). The stop
// abandons the burst of such a destination and finishes.
static void test_server_refused_starts_counted_by_cause()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 1;
    const int clients = 3;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t ring_full = counters[ROCKETNET_COUNTER_SEND_RING_FULL];
    const uint64_t no_link = counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS];

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
    rocketnet_unit_check( frame );
    memset( frame, 1, 16 );
    rocketnet_unit_check( rocketnet_server_transport_finish_packet( server, &context, 16 ) > 0 );

    // the staging is full: a full ring

    rocketnet_unit_check( !rocketnet_server_transport_start_packet( server, 0, 16, &context ) );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + 1 );

    // the checks below are of the transport's own refusals: room well past
    // the protocol reserve, so the budget the flush opens
    // (RNSendReserve) is not what refuses them

    f->capacity = 1024;
    rocketnet_server_transport_send_packets( server );

    // room left, but the transport refuses the destination: not a full ring

    f->refused_port = 1001;
    rocketnet_unit_check( !rocketnet_server_transport_start_packet( server, 1, 16, &context ) );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + 1 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == no_link );

    // no link-layer address: its own counter

    f->unreachable_port = 1002;
    rocketnet_unit_check( !rocketnet_server_transport_start_packet( server, 2, 16, &context ) );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + 1 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == no_link + 1 );

    // the stop abandons both refused destinations' bursts and finishes

    f->capacity = 4;
    stop_spread_stop_server( server, clients );
    for ( int poll = 0; poll < 8 && !rocketnet_server_transport_stopped( server ); poll++ )
    {
    }

    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->flushed[0] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( f->flushed[1] == 0 && f->flushed[2] == 0 );
    rocketnet_unit_check( f->refused_for_room == 1 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The stop tells the I/O to stop only once the transport reports every send
// it was handed finished: with the last burst flushed and still in flight,
// each poll is not-stopped and leaves the I/O running, and the poll after the
// transport finishes them stops it.
static void test_server_stop_waits_for_flushed_sends()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;
    f->hold_flushed = true;
    const int clients = 2;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );

    stop_spread_stop_server( server, clients );
    rocketnet_unit_check( stop_spread_flushed( f ) == clients * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( f->in_flight == clients * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( f->stops == 0 );

    for ( int poll = 0; poll < 4; poll++ )
    {
        rocketnet_unit_check( !rocketnet_server_transport_stopped( server ) );
    }
    rocketnet_unit_check( f->stops == 0 );

    f->in_flight = 0;
    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->stops == 1 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

static int g_stop_spread_dropped_lines;

static void stop_spread_log_capture( void *, enum rocketnet_log_level_t level, const char * message, size_t )
{
    if ( level == ROCKETNET_LOG_LEVEL_DEBUG && strstr( message, "dropped disconnect burst" ) != NULL )
    {
        g_stop_spread_dropped_lines++;
    }
}

// A running server keeps at most max_clients bursts waiting. With the ring at
// that cap, one more kick ends the slot and drops its burst: the drop is
// counted as DISCONNECT_BURST_DROPPED and logged at debug, and the bursts
// already waiting keep their places.
static void test_server_kick_at_steady_cap_counts_dropped_burst()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 4;
    const int clients = 2;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t dropped = counters[ROCKETNET_COUNTER_DISCONNECT_BURST_DROPPED];

    // the staging has less room than a burst, so each kick's burst waits

    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_server_transport_disconnect_client( server, i );
    }
    rocketnet_unit_check( server->disconnect_burst_count == clients );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_DISCONNECT_BURST_DROPPED] == dropped );

    // the last slot freed is the first reused (the free list is LIFO, its freed
    // slots published as an update's end publishes them): it connects again
    // and is kicked with the ring at its steady cap

    const int again = clients - 1;
    rocketnet_client_slots_publish( server->protocol.client_slots );
    stop_spread_connect( server, again );

    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_DEBUG );
    g_stop_spread_dropped_lines = 0;
    rocketnet_log_function( stop_spread_log_capture, NULL );

    rocketnet_server_transport_disconnect_client( server, again );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_unit_check( server->disconnect_burst_count == clients );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_DISCONNECT_BURST_DROPPED] == dropped + 1 );
    rocketnet_unit_check( g_stop_spread_dropped_lines == 1 );

    // with room for every waiting packet, the stop sends the two retained
    // bursts whole and nothing for the dropped one

    f->capacity = 64;
    rocketnet_server_transport_stop( server );
    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_unit_check( f->flushed[i] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    }

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_kick_at_steady_cap_counts_dropped_burst );
#if ROCKETNET_DEV
#include "rocketnet_base64.h"

static double stop_spread_link_now = 1000.0;
static double stop_spread_link_clock( void * ) { return stop_spread_link_now; }

// Stopping a server with the link simulator active drains the disconnect
// burst from the simulator into the transport within its send room, ahead of
// any latency, and requests I/O stop only once the simulator queue is empty,
// so connected clients receive SERVER_DISCONNECTED instead of timing out.
static void test_server_stop_sends_burst_staged_in_link_simulator()
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x6C ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );

    const uint64_t protocol_id = UINT64_C( 0x53544F5053494D01 );
    const int latency_ms[] = { 0, 50 };

    for ( int i = 0; i < 2; i++ )
    {
        stop_spread_link_now = 1000.0;

        rocketnet_server_config_t server_config;
        rocketnet_server_default_config( &server_config );
        server_config.protocol_id = protocol_id;
        server_config.max_clients = 2;
        memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
        server_config.transport = "loopback";
        server_config.loopback_hub = rocketnet_test_loopback_hub();

        rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
        rocketnet_unit_check( server != NULL );
        rocketnet_server_public_key( server, server_public );
        rocketnet_unit_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

        rocketnet_client_config_t client_config;
        rocketnet_client_default_config( &client_config );
        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = true;
        client_config.time_callback = stop_spread_link_clock;

        rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
        rocketnet_unit_check( client != NULL );

        uint8_t raw_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        char address[64];
        snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
        const char * addresses[] = { address };
        rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret, server_public, protocol_id,
                                                                        addresses, 1, UINT64_C( 0xE510 ) + (uint64_t) i,
                                                                        1000, UINT64_C( 1 ) << 40, 10, NULL,
                                                                        raw_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        rocketnet_unit_check( rocketnet_base64_encode_data( raw_token, sizeof( raw_token ), token, sizeof( token ) ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

        rocketnet_unit_check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );
        for ( int step = 0; step < 400 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED ||
                                            rocketnet_server_num_connected_clients( server ) == 0 );
              step++ )
        {
            stop_spread_link_now += 0.01;
            rocketnet_client_update( client, stop_spread_link_now );
            rocketnet_client_send_packets( client );
            rocketnet_server_update( server, stop_spread_link_now );
            rocketnet_server_send_packets( server );
        }
        rocketnet_unit_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        const int slot = rocketnet_client_index( client );
        rocketnet_unit_check( slot >= 0 && rocketnet_server_client_connected( server, slot ) );

        rocketnet_server_network_simulator( server, latency_ms[i], 0, 0, latency_ms[i] == 0 ? 100 : 0 );

        rocketnet_server_stop( server );
        rn_test_server_wait_stopped( server );

        rocketnet_client_update( client, stop_spread_link_now );
        rocketnet_unit_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

        rn_test_client_stop_and_destroy( client );
        rn_test_server_stop_and_destroy( server );
    }

    for ( int i = 0; i < 2; i++ )
    {
        stop_spread_fixture_t * f = new stop_spread_fixture_t();
        f->capacity = 24;
        const int clients = 8;
        const int duplicate = latency_ms[i] == 0 ? 100 : 0;
        const int owed = clients * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ( duplicate == 100 ? 2 : 1 );

        rocketnet_server_transport_io_operations_t operations;
        rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
        rocketnet_unit_check( rocketnet_server_transport_network_simulator( server, latency_ms[i], 0, 0, duplicate ) );

        stop_spread_stop_server( server, clients );
        rocketnet_unit_check( stop_spread_flushed( f ) == f->capacity );
        rocketnet_unit_check( f->stops == 0 );
        rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPING );

        double time = 1000.0;
        int updates = 0;
        while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED && updates < 16 )
        {
            time += 1.0 / 60.0;
            rocketnet_server_transport_update( server, time );
            updates++;
        }

        rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
        rocketnet_unit_check( f->stops == 1 );
        rocketnet_unit_check( f->refused_for_room == 0 );
        rocketnet_unit_check( stop_spread_flushed( f ) == owed );
        rocketnet_unit_check( !rocketnet_network_simulator_holds_any( server->link_simulator ) );

        rn_test_server_transport_stop_and_destroy( server );
        delete f;
    }
}
#endif // #if ROCKETNET_DEV

// One armed sealed-write failure during the stop. The burst is abandoned,
// as an unreachable destination's is, and the drain continues: the other
// clients' bursts leave and the transport stops. Left at the head, that
// one failure used to make rocketnet_server_transport_stopped stay false.
static void test_server_stop_abandons_a_burst_when_the_sealed_write_fails()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;
    const int clients = 3;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );

    rocketnet_dev_fail_disconnect_sealed_writes( 1 );
    stop_spread_stop_server( server, clients );

    for ( int poll = 0; poll < 8 && !rocketnet_server_transport_stopped( server ); poll++ )
    {
    }

    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->flushed[0] == 0 );
    rocketnet_unit_check( f->flushed[1] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( f->flushed[2] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( f->refused_for_room == 0 );
    rocketnet_unit_check( f->stops == 1 );

    rocketnet_dev_fail_disconnect_sealed_writes( 0 );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

REGISTER_UNIT_TEST( test_server_stop_spreads_disconnect_bursts_across_updates );
REGISTER_UNIT_TEST( test_server_stop_abandons_a_burst_when_the_sealed_write_fails );
REGISTER_UNIT_TEST( test_server_stop_waits_for_flushed_sends );
REGISTER_UNIT_TEST( test_server_refused_starts_counted_by_cause );

// proofs/tla/RNServerMaintenanceSweep.tla, the refused-with-room class. The
// transport refuses every start to client 1 with room left (an address its
// socket cannot carry, rocketnet_driver.h). That refusal is the one frame's,
// not a full staging: the sweep passes the slot and serves clients 0 and 2
// every update. It used to break there with the cursor on client 1, so every
// later sweep began and ended at client 1 and no slot was served again.
static void test_server_sweep_passes_a_destination_refused_with_room()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 16;
    f->refused_port = 1001;
    const int clients = 3;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
    const uint64_t ring_full = server->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL];

    // every client keeps sending, so no slot times out and the refused one
    // never ends on its own

    const int updates = 4;
    for ( int step = 1; step <= updates; step++ )
    {
        const double time = 1000.0 + step;
        for ( int i = 0; i < clients; i++ )
        {
            const int index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, i );
            rocketnet_encryption_map_entry( server->protocol.encryption_map, index )->control->last_receive_time = time;
        }
        rocketnet_server_transport_update( server, time );
    }

    rocketnet_unit_check( f->control[0] == updates );
    rocketnet_unit_check( f->control[1] == 0 );
    rocketnet_unit_check( f->control[2] == updates );
    rocketnet_unit_check( f->refused_for_room == 0 );
    rocketnet_unit_check( server->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full );
    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == clients );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_sweep_passes_a_destination_refused_with_room );
REGISTER_UNIT_TEST( test_server_stop_polls_are_the_stop_steps );
REGISTER_UNIT_TEST( test_server_stop_abandons_an_unreachable_burst );
REGISTER_UNIT_TEST( test_server_stop_abandons_bursts_without_send_room );
REGISTER_UNIT_TEST( test_server_stop_without_room_waits_for_sends_in_flight );
REGISTER_UNIT_TEST( test_server_kick_spreads_its_burst_across_updates );
REGISTER_UNIT_TEST( test_server_send_capacity_reports_staged_room );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_server_stop_sends_burst_staged_in_link_simulator );
#endif // #if ROCKETNET_DEV

// The sent-stats scan of a slot's ack endpoint that finds no survivor sets
// internal_error (sdk/rocketnet_ack.cpp). The next update ends that slot as a
// kick does, with its disconnect burst, and names PROTOCOL_VIOLATION as the
// reason; the other slot stays connected.
static void test_server_update_ends_slot_on_ack_internal_error()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;
    const int clients = 2;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );

    for ( int i = 0; i < clients; i++ )
    {
        const int index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, i );
        rocketnet_encryption_map_entry( server->protocol.encryption_map, index )->control->last_receive_time = 1000.0;
    }

    server->protocol.acks->endpoints[1].internal_error = 1;
    rocketnet_server_transport_update( server, 1000.0 + 1.0 / 60.0 );

    rocketnet_unit_check( rocketnet_client_slots_connected( server->protocol.client_slots, 0 ) );
    rocketnet_unit_check( !rocketnet_client_slots_connected( server->protocol.client_slots, 1 ) );
    rocketnet_unit_check( rocketnet_server_transport_client_end_reason( server, 1 ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rocketnet_unit_check( f->staged[1] + f->flushed[1] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( server->protocol.acks->endpoints[1].internal_error == 0 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_update_ends_slot_on_ack_internal_error );

// Every counter equals its value before the step, except SERVER_DISCONNECTED_CLIENT
// and CLIENT_DISCONNECTED (each up by ended), DISCONNECT_BURST_DROPPED (up by
// dropped) and SLOT_WITHOUT_MAPPING (up by missing).
static void stop_spread_check_ended( const uint64_t * before, const uint64_t * after, uint64_t ended, uint64_t dropped,
                                     uint64_t missing )
{
    rocketnet_unit_check( after[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT] - before[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT] == ended );
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        uint64_t expected = before[i];
        if ( i == ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT || i == ROCKETNET_COUNTER_CLIENT_DISCONNECTED )
        {
            expected += ended;
        }
        else if ( i == ROCKETNET_COUNTER_DISCONNECT_BURST_DROPPED )
        {
            expected += dropped;
        }
        else if ( i == ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING )
        {
            expected += missing;
        }
        rocketnet_unit_check( after[i] == expected );
    }
}

// SERVER_DISCONNECTED_CLIENT counts each slot the server ends, once, on each
// of the four ways a kick ends it: the burst sealed at once
// (rocketnet_server_protocol_disconnect_client), the burst left waiting for
// room, the burst dropped with the ring at its steady cap, and a connected
// slot with no mapping (the last three in
// rocketnet_server_transport_close_client_locally). A kick of a slot that is
// not connected counts nothing.
static void test_server_disconnected_client_counted_on_every_path()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;
    const int clients = 3;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
    const uint64_t * counters = server->protocol.counters->values;
    uint64_t before[ROCKETNET_NUM_COUNTERS];

    // sealed at once: room, allowance, and no burst waiting

    memcpy( before, counters, sizeof( before ) );
    const uint64_t seals = server->disconnect_seals;
    rocketnet_server_transport_disconnect_client( server, 0 );
    rocketnet_unit_check( server->disconnect_seals == seals + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( server->disconnect_burst_count == 0 );
    rocketnet_unit_check( !rocketnet_client_slots_connected( server->protocol.client_slots, 0 ) );
    stop_spread_check_ended( before, counters, 1, 0, 0 );

    // the same slot again: not connected, nothing counted

    memcpy( before, counters, sizeof( before ) );
    rocketnet_server_transport_disconnect_client( server, 0 );
    stop_spread_check_ended( before, counters, 0, 0, 0 );

    // no send room: each burst waits, up to the steady cap (max_clients, 3)

    f->capacity = f->used;
    memcpy( before, counters, sizeof( before ) );
    rocketnet_server_transport_disconnect_client( server, 1 );
    rocketnet_unit_check( server->disconnect_burst_count == 1 );
    stop_spread_check_ended( before, counters, 1, 0, 0 );

    memcpy( before, counters, sizeof( before ) );
    rocketnet_server_transport_disconnect_client( server, 2 );
    rocketnet_unit_check( server->disconnect_burst_count == 2 );
    stop_spread_check_ended( before, counters, 1, 0, 0 );

    rocketnet_client_slots_publish( server->protocol.client_slots );    // as an update's end does
    stop_spread_connect( server, 2 );
    memcpy( before, counters, sizeof( before ) );
    rocketnet_server_transport_disconnect_client( server, 2 );
    rocketnet_unit_check( server->disconnect_burst_count == 3 );
    stop_spread_check_ended( before, counters, 1, 0, 0 );

    // the ring at its steady cap: the burst is dropped, the slot still ends

    rocketnet_client_slots_publish( server->protocol.client_slots );
    stop_spread_connect( server, 2 );
    memcpy( before, counters, sizeof( before ) );
    rocketnet_server_transport_disconnect_client( server, 2 );
    rocketnet_unit_check( server->disconnect_burst_count == 3 );
    rocketnet_unit_check( !rocketnet_client_slots_connected( server->protocol.client_slots, 2 ) );
    stop_spread_check_ended( before, counters, 1, 1, 0 );

    // a connected slot with no mapping (the slot API permits it): it ends,
    // counted once, with SLOT_WITHOUT_MAPPING

    rocketnet_client_slots_publish( server->protocol.client_slots );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    const int orphan = rocketnet_client_slots_assign( server->protocol.client_slots, 99, -1, 99, user_data );
    rocketnet_unit_check( orphan >= 0 && orphan < clients );
    rocketnet_unit_check( rocketnet_client_slots_connected( server->protocol.client_slots, orphan ) );
    memcpy( before, counters, sizeof( before ) );
    rocketnet_server_transport_disconnect_client( server, orphan );
    rocketnet_unit_check( !rocketnet_client_slots_connected( server->protocol.client_slots, orphan ) );
    rocketnet_unit_check( server->disconnect_burst_count == 3 );
    stop_spread_check_ended( before, counters, 1, 0, 1 );

    f->capacity = 64;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_disconnected_client_counted_on_every_path );

static rocketnet_server_transport_t * g_stop_spread_reentry_server;
static int g_stop_spread_reentry_slot;
static int g_stop_spread_reentry_calls;

static void stop_spread_reentry_callback( void *, enum rocketnet_log_level_t level, const char * message, size_t )
{
    if ( level != ROCKETNET_LOG_LEVEL_DEBUG || strstr( message, "dropped disconnect burst" ) == NULL )
    {
        return;
    }

    g_stop_spread_dropped_lines++;
    if ( g_stop_spread_reentry_calls != 0 )
    {
        return;
    }

    g_stop_spread_reentry_calls++;
    rocketnet_server_transport_disconnect_client( g_stop_spread_reentry_server, g_stop_spread_reentry_slot );
}

// A log callback that disconnects the slot the burst-drop line names (as in
// test_log_callback_disconnect_reentry_does_not_repeat): the slot has already
// closed when the line is written, so the call finds nothing to do. The drop
// is logged and counted once and the slot is closed once.
static void test_server_kick_at_steady_cap_log_reentry_does_not_repeat()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 4;
    const int clients = 2;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, clients, true );
    const uint64_t * counters = server->protocol.counters->values;

    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_server_transport_disconnect_client( server, i );
    }
    rocketnet_unit_check( server->disconnect_burst_count == clients );

    const int again = clients - 1;
    rocketnet_client_slots_publish( server->protocol.client_slots );    // as an update's end does
    stop_spread_connect( server, again );

    const uint64_t dropped = counters[ROCKETNET_COUNTER_DISCONNECT_BURST_DROPPED];
    const uint64_t disconnected = counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED];

    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_DEBUG );
    g_stop_spread_dropped_lines = 0;
    g_stop_spread_reentry_server = server;
    g_stop_spread_reentry_slot = again;
    g_stop_spread_reentry_calls = 0;
    rocketnet_log_function( stop_spread_reentry_callback, NULL );

    rocketnet_server_transport_disconnect_client( server, again );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );
    g_stop_spread_reentry_server = NULL;

    rocketnet_unit_check( g_stop_spread_reentry_calls == 1 );
    rocketnet_unit_check( g_stop_spread_dropped_lines == 1 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_DISCONNECT_BURST_DROPPED] == dropped + 1 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED] == disconnected + 1 );
    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_unit_check( server->disconnect_burst_count == clients );

    f->capacity = 64;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_kick_at_steady_cap_log_reentry_does_not_repeat );

static int g_stop_spread_not_stopped_errors;
static int g_stop_spread_asserts;

static void stop_spread_error_capture( void *, enum rocketnet_log_level_t level, const char * message, size_t )
{
    if ( level == ROCKETNET_LOG_LEVEL_ERROR && strstr( message, "not stopped" ) != NULL )
    {
        g_stop_spread_not_stopped_errors++;
    }
}

static void stop_spread_assert_capture( const char *, const char *, const char *, int )
{
    g_stop_spread_asserts++;
}

// Two misuses as a release build meets them, with the debug assert captured:
// destroy of a transport that is not stopped says so in an error and keeps
// the transport, and a NULL user data buffer is refused and counted, never
// written through.
static void test_server_transport_misuse_refused_without_asserts()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );

    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    g_stop_spread_not_stopped_errors = 0;
    g_stop_spread_asserts = 0;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_log_function( stop_spread_error_capture, NULL );
    rocketnet_assert_function( stop_spread_assert_capture );

    rocketnet_server_transport_destroy( server );
    const int destroy_errors = g_stop_spread_not_stopped_errors;

    const uint64_t misuse = rocketnet_api_misuse_count();
    const bool user_data_read = rocketnet_server_transport_client_user_data( server, 0, NULL );

    rocketnet_assert_function( saved_assert );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    rocketnet_unit_check( destroy_errors == 1 );
    rocketnet_unit_check( !user_data_read );
    rocketnet_unit_check( rocketnet_api_misuse_count() == misuse + 1 );
    rocketnet_unit_check( g_stop_spread_asserts == ( ROCKETNET_ASSERTS ? 2 : 0 ) );
    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_transport_misuse_refused_without_asserts );

// the xdp replay writer's counts are assigned only when the transport's io
// table answers them: on every other table, on every platform, a read leaves
// both rows as they were

// sdk/rocketnet_driver_builtin.cpp, declared by its one reader
// (sdk/rocketnet_server_transport.cpp) and here
bool rocketnet_server_transport_io_replay_counts( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                  uint64_t * commit_failed, uint64_t * greetings_held );

static void test_server_transport_replay_counts_left_on_other_tables()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );

    server->counters.values[ROCKETNET_COUNTER_XDP_REPLAY_COMMIT_FAILED] = 5;
    server->counters.values[ROCKETNET_COUNTER_XDP_REPLAY_GREETING_HELD] = 9;
    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_XDP_REPLAY_COMMIT_FAILED] == 5 );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_XDP_REPLAY_GREETING_HELD] == 9 );

    uint64_t commit_failed = 1;
    uint64_t greetings_held = 1;
    rocketnet_unit_check( !rocketnet_server_transport_io_replay_counts( server->io.operations, server->io.instance, &commit_failed, &greetings_held ) );
    rocketnet_unit_check( commit_failed == 1 && greetings_held == 1 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_transport_replay_counts_left_on_other_tables );

// XDP_PIN_REFUSALS is assigned only when the transport's io table answers the
// transport rows (the xdp table, on Linux with XDP built): on every other
// table, on every platform, a read leaves the row as it was

// sdk/rocketnet_driver_builtin.cpp, declared by its one reader
// (sdk/rocketnet_server_transport.cpp) and here
bool rocketnet_server_transport_io_transport_counters( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                       uint64_t * values );

static void test_server_transport_xdp_pin_refusals_left_on_other_tables()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );

    server->counters.values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] = 6;
    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] == 6 );

    uint64_t values[ROCKETNET_NUM_COUNTERS] = {};
    values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] = 1;
    rocketnet_unit_check( !rocketnet_server_transport_io_transport_counters( server->io.operations, server->io.instance, values ) );
    rocketnet_unit_check( values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] == 1 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_transport_xdp_pin_refusals_left_on_other_tables );

#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

// The transport's own read of XDP_RECEIVE_FRAME_DROPPED
// (rocketnet_server_transport_counters): the development case
// rocketnet_server_socket_test_receive_dropped drives two synthetic xdp queues
// through their drops by reason and the kernel's rx_dropped and rx_ring_full,
// and asks its dispatch for the total after each step (0, 0, 31, 127, 127,
// 127, 128, 128, 63). Here the dispatch is a server transport whose I/O is
// the xdp table over those queues: the answer is what the transport's counter
// read leaves in XDP_RECEIVE_FRAME_DROPPED, so the case passes only when that
// counter holds each total exactly. A second read leaves it unchanged (the
// count is assigned, never added), and a transport whose I/O is not xdp
// leaves the counter as it was.

typedef bool rocketnet_server_socket_test_frames_dropped_t( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                            uint64_t * dropped );
typedef bool rocketnet_server_socket_test_replay_counts_t( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                           uint64_t * commit_failed, uint64_t * greetings_held );
bool rocketnet_server_socket_test_receive_dropped( rocketnet_server_socket_test_frames_dropped_t * dispatch,
                                                   rocketnet_server_socket_test_replay_counts_t * replay_dispatch,
                                                   const struct rocketnet_server_transport_io_operations_t * xdp );

static rocketnet_server_transport_t * g_frames_dropped_server;
static int g_frames_dropped_reads;

static bool stop_spread_frames_dropped_read( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                             uint64_t * dropped )
{
    rocketnet_server_transport_t * server = g_frames_dropped_server;
    const rocketnet_server_transport_io_operations_t * saved_operations = server->io.operations;
    void * saved_instance = server->io.instance;
    server->io.operations = operations;
    server->io.instance = transport;

    const uint64_t first = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED];
    const uint64_t again = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED];
    rocketnet_unit_check( again == first );

    server->io.operations = saved_operations;
    server->io.instance = saved_instance;
    g_frames_dropped_reads++;
    *dropped = first;
    return true;
}

// the replay writer's counts read the same way: through the transport's own
// counter read with the xdp table in place, so the seam's expected values
// are checked against XDP_REPLAY_COMMIT_FAILED and XDP_REPLAY_GREETING_HELD
// themselves, not the dispatch's answer
static int g_replay_counts_reads;

static bool stop_spread_replay_counts_read( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                            uint64_t * commit_failed, uint64_t * greetings_held )
{
    rocketnet_server_transport_t * server = g_frames_dropped_server;
    const rocketnet_server_transport_io_operations_t * saved_operations = server->io.operations;
    void * saved_instance = server->io.instance;
    server->io.operations = operations;
    server->io.instance = transport;

    const uint64_t failed = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_REPLAY_COMMIT_FAILED];
    const uint64_t held = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_REPLAY_GREETING_HELD];
    rocketnet_unit_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_REPLAY_COMMIT_FAILED] == failed );

    server->io.operations = saved_operations;
    server->io.instance = saved_instance;
    g_replay_counts_reads++;
    *commit_failed = failed;
    *greetings_held = held;
    return true;
}

uint64_t rocketnet_server_socket_memory_required( int num_queues, int max_clients );

// The two synthetic queues rocketnet_server_socket_test_receive_dropped
// builds are whole queue objects, zeroed at construction, each holding its
// receive staging and send buffers: past the suite's default bound of 698
// MiB (measured 1189 MiB in a Debug build on CI). The bound is the XDP plan
// of a two-queue server less the UMEM the case never maps, plus 64 MiB for
// the process and the case's own 64-client server.
static long stop_spread_xdp_two_queue_rss_bound_mb()
{
    const uint64_t plan = rocketnet_server_socket_memory_required( 2, ROCKETNET_DEFAULT_MAX_CLIENTS );
    const uint64_t umem = rocketnet_xdp_umem_bytes( 2 );
    if ( !plan || !umem || plan <= umem )
    {
        return 0;
    }
    return suite_rss_bound_mb( plan - umem, 64 );
}

SUITE_CASE_RSS_BOUND( test_server_transport_counts_xdp_frames_dropped, stop_spread_xdp_two_queue_rss_bound_mb );

static void test_server_transport_counts_xdp_frames_dropped()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );    // the synthetic queues have no stats map: its read logs once

    // not the xdp table: the counter is left as it was

    server->counters.values[ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED] = 7;
    rocketnet_unit_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED] == 7 );
    server->counters.values[ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED] = 0;

    const rocketnet_transport_definition_t * xdp = rocketnet_transport_definition( ROCKETNET_TRANSPORT_XDP );
    rocketnet_unit_check( xdp && xdp->backend && xdp->backend->server );
    g_frames_dropped_server = server;
    g_frames_dropped_reads = 0;
    g_replay_counts_reads = 0;
    rocketnet_unit_check( rocketnet_server_socket_test_receive_dropped( stop_spread_frames_dropped_read, stop_spread_replay_counts_read,
                                                                        xdp->backend->server ) );
    rocketnet_unit_check( g_frames_dropped_reads == 9 );
    rocketnet_unit_check( g_replay_counts_reads == 2 );
    g_frames_dropped_server = NULL;

    // the last totals stay until the next xdp read: a read through the fake
    // table assigns nothing

    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED] == 63 );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_XDP_REPLAY_COMMIT_FAILED] == 5 );
    rocketnet_unit_check( counters->values[ROCKETNET_COUNTER_XDP_REPLAY_GREETING_HELD] == 3 );

    rocketnet_log_level( saved_level );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_transport_counts_xdp_frames_dropped );

// The transport's own read of XDP_PIN_REFUSALS: the development case
// rocketnet_server_socket_test_pin_refusals pins two synthetic xdp queues
// through fake affinity calls (granted, the affinity call refused, the cpu not
// online, the cpu outside the set, the pin and its fallback off core 0 both
// refused, core 0 the only cpu) and asks its dispatch for the public row
// after each step (0, 1, 2, 3, 4, 5). Here the dispatch is a server transport whose
// I/O is the xdp table over those queues: the answer is what
// rocketnet_server_transport_counters leaves in XDP_PIN_REFUSALS, by value, so
// the case passes only when the row holds each count exactly; a second read
// leaves it unchanged (assigned, never added).

typedef bool rocketnet_server_socket_test_transport_counters_t( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                                uint64_t * values );
bool rocketnet_server_socket_test_pin_refusals( rocketnet_server_socket_test_transport_counters_t * dispatch,
                                                const struct rocketnet_server_transport_io_operations_t * xdp );

static int g_pin_refusals_reads;

static bool stop_spread_pin_refusals_read( const struct rocketnet_server_transport_io_operations_t * operations, void * transport, uint64_t * values )
{
    rocketnet_server_transport_t * server = g_frames_dropped_server;
    const rocketnet_server_transport_io_operations_t * saved_operations = server->io.operations;
    void * saved_instance = server->io.instance;
    server->io.operations = operations;
    server->io.instance = transport;

    const uint64_t refusals = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS];
    rocketnet_unit_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] == refusals );

    server->io.operations = saved_operations;
    server->io.instance = saved_instance;
    g_pin_refusals_reads++;
    values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] = refusals;
    return true;
}

SUITE_CASE_RSS_BOUND( test_server_transport_counts_xdp_pin_refusals, stop_spread_xdp_two_queue_rss_bound_mb );

static void test_server_transport_counts_xdp_pin_refusals()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );    // each refused pin logs, and the synthetic queues have no stats map

    const rocketnet_transport_definition_t * xdp = rocketnet_transport_definition( ROCKETNET_TRANSPORT_XDP );
    rocketnet_unit_check( xdp && xdp->backend && xdp->backend->server );
    g_frames_dropped_server = server;
    g_pin_refusals_reads = 0;
    rocketnet_unit_check( rocketnet_server_socket_test_pin_refusals( stop_spread_pin_refusals_read, xdp->backend->server ) );
    rocketnet_unit_check( g_pin_refusals_reads == 7 );
    g_frames_dropped_server = NULL;

    // the last count stays until the next xdp read: a read through the fake
    // table assigns nothing
    rocketnet_unit_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_XDP_PIN_REFUSALS] == 5 );

    rocketnet_log_level( saved_level );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_transport_counts_xdp_pin_refusals );

#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include "rocketnet_server_gso.h"
#include "rocketnet_server_uring.h"

// a row as the server reads it: rocketnet_server_transport_counters with the
// transport's io table and instance in place, read twice (assigned, never
// added), the fixture's own restored after
static rocketnet_server_transport_t * g_pin_fold_server;

static uint64_t stop_spread_pin_row( const struct rocketnet_server_transport_io_operations_t * operations, void * transport, int row )
{
    rocketnet_server_transport_t * server = g_pin_fold_server;
    const rocketnet_server_transport_io_operations_t * saved_operations = server->io.operations;
    void * saved_instance = server->io.instance;
    server->io.operations = operations;
    server->io.instance = transport;

    const uint64_t value = rocketnet_server_transport_counters( server )->values[row];
    rocketnet_unit_check( rocketnet_server_transport_counters( server )->values[row] == value );

    server->io.operations = saved_operations;
    server->io.instance = saved_instance;
    return value;
}

// The gso transport's placement of a worker (sdk/rocketnet_server_gso_linux.cpp,
// rocketnet_server_gso_place_worker) with only the refusals injected
// (rocketnet_server_gso_test_place_worker): the transport's own code counts,
// and GSO_PIN_REFUSALS is read by value through the server's fold. Its row
// counts refused pins and the failed confinements after them. A worker left
// on cpu 0 is flagged. Only a granted pin asks for SCHED_RR, once: a worker
// whose pin was refused asks for none, wherever it is left (confined off cpu
// 0, or on a mask that never held it). With cpu 0 not reserved
// (gso_reserve_cpu_0 ROCKETNET_CPU_0_NOT_RESERVED) worker q pins to cpu q,
// num_os_cpus is not applied, and a refused pin is counted but neither
// confined nor flagged.

static void test_server_transport_counts_gso_pin_refusals()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );    // each refusal logs

    const rocketnet_transport_definition_t * gso_definition = rocketnet_transport_definition( ROCKETNET_TRANSPORT_GSO );
    rocketnet_unit_check( gso_definition && gso_definition->backend && gso_definition->backend->server );
    const rocketnet_server_transport_io_operations_t * gso_operations = gso_definition->backend->server;
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    rocketnet_unit_check( gso != NULL );
    g_pin_fold_server = server;

    bool left = true;
    int round_robin = -1;
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 0 );

    // granted: cpu 1 of 4, not counted, SCHED_RR asked for
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 1, 4, 4, false, false, false, &left, &round_robin ) );
    rocketnet_unit_check( !left && round_robin == 1 && rocketnet_server_gso_worker_cpu( gso, 0 ) == 1 );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 0 );

    // the pin refused, the confinement (cpus 1 .. 3) taken: counted once, no
    // SCHED_RR on the cpus it now shares
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 1, 4, 4, false, true, false, &left, &round_robin ) );
    rocketnet_unit_check( !left && round_robin == 0 && rocketnet_server_gso_worker_cpu( gso, 0 ) == ROCKETNET_GSO_PIN_NONE );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 1 );

    // the pin and the confinement refused: both counted, left on cpu 0, no SCHED_RR
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 1, 4, 4, false, true, true, &left, &round_robin ) );
    rocketnet_unit_check( left && round_robin == 0 && rocketnet_server_gso_worker_cpu( gso, 0 ) == ROCKETNET_GSO_PIN_NONE );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 3 );

    // one cpu online: cpu 1 not online (counted), no cpu from num_os_cpus up
    // to confine to (counted), left on cpu 0, no SCHED_RR; the call returns:
    // the host runs
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 1, 1, 1, false, false, false, &left, &round_robin ) );
    rocketnet_unit_check( left && round_robin == 0 );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 5 );

    // a later granted pin clears the flag
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 1, 4, 4, false, false, false, &left, &round_robin ) );
    rocketnet_unit_check( !left && round_robin == 1 && rocketnet_server_gso_worker_cpu( gso, 0 ) == 1 );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 5 );

    // the pin and the confinement refused on a mask without cpu 0 (cpus 1 ..
    // 3): both counted, but the worker is not on cpu 0, so it is not flagged;
    // its pin was refused, so no SCHED_RR (a cpuset of game cores)
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 1, 4, 4, true, true, true, &left, &round_robin ) );
    rocketnet_unit_check( !left && round_robin == 0 && rocketnet_server_gso_worker_cpu( gso, 0 ) == ROCKETNET_GSO_PIN_NONE );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 7 );

    // cpu 0 not reserved: queue 0 pins to cpu 0 (num_os_cpus 3 is not
    // applied), not counted, SCHED_RR asked for
    rocketnet_server_gso_test_set_reserve_cpu_0( gso, false );
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 3, 4, 4, false, false, false, &left, &round_robin ) );
    rocketnet_unit_check( !left && round_robin == 1 && rocketnet_server_gso_worker_cpu( gso, 0 ) == 0 );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 7 );

    // not reserved, every affinity set refused: the pin counted once, no
    // confinement tried (so no second count), not flagged, no SCHED_RR
    rocketnet_unit_check( rocketnet_server_gso_test_place_worker( gso, 0, 1, 4, 4, false, true, true, &left, &round_robin ) );
    rocketnet_unit_check( !left && round_robin == 0 && rocketnet_server_gso_worker_cpu( gso, 0 ) == ROCKETNET_GSO_PIN_NONE );
    rocketnet_unit_check( stop_spread_pin_row( gso_operations, gso, ROCKETNET_COUNTER_GSO_PIN_REFUSALS ) == 8 );
    rocketnet_server_gso_test_set_reserve_cpu_0( gso, true );

    rocketnet_unit_check( !rocketnet_server_gso_test_place_worker( gso, 1, 1, 4, 4, false, false, false, &left, &round_robin ) );

    g_pin_fold_server = NULL;
    rocketnet_server_gso_test_close( gso );
    rocketnet_log_level( saved_level );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_transport_counts_gso_pin_refusals );

#if defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING

static bool stop_spread_uring_pin_read( const struct rocketnet_server_transport_io_operations_t * operations, void * transport, uint64_t * values )
{
    values[ROCKETNET_COUNTER_URING_PIN_REFUSALS] = stop_spread_pin_row( operations, transport, ROCKETNET_COUNTER_URING_PIN_REFUSALS );
    return true;
}

// The io_uring transport's placement of a worker
// (sdk/rocketnet_server_uring_linux.cpp, rocketnet_server_uring_pin_worker)
// with only the refusals injected (rocketnet_server_uring_test_place_worker,
// a fresh synthetic transport each call): the transport's own code counts, and
// URING_PIN_REFUSALS is read by value through the server's fold. Its row counts
// refused pins only, so a failed confinement after one is flagged and logged,
// not counted again. Only a granted pin asks for SCHED_RR: a worker whose pin
// was refused asks for none, wherever it is left. With cpu 0 not reserved
// (io_uring_reserve_cpu_0 ROCKETNET_CPU_0_NOT_RESERVED) worker 0 pins to cpu
// 0, num_os_cpus is not applied, and a refused pin is neither confined nor
// flagged.

static void test_server_transport_counts_uring_pin_refusals()
{
    stop_spread_fixture_t * f = new stop_spread_fixture_t();
    f->capacity = 64;

    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = stop_spread_server( f, &operations, 1, true );
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );    // each refusal logs

    const rocketnet_transport_definition_t * uring_definition = rocketnet_transport_definition( ROCKETNET_TRANSPORT_IO_URING );
    rocketnet_unit_check( uring_definition && uring_definition->backend && uring_definition->backend->server );
    const rocketnet_server_transport_io_operations_t * uring_operations = uring_definition->backend->server;
    g_pin_fold_server = server;

    uint64_t row = UINT64_MAX;
    bool left = true;
    int round_robin = -1;
    int cpu = -100;

    // granted: cpu 1, not counted, SCHED_RR asked for
    rocketnet_unit_check( rocketnet_server_uring_test_place_worker( stop_spread_uring_pin_read, uring_operations, 1, true, 4, 4, false, false, false, &row, &left, &round_robin, &cpu ) );
    rocketnet_unit_check( row == 0 && !left && round_robin == 1 && cpu == 1 );

    // the pin refused, the confine (cpus 1 .. 3) taken: counted once, no
    // SCHED_RR on the cpus it now shares
    rocketnet_unit_check( rocketnet_server_uring_test_place_worker( stop_spread_uring_pin_read, uring_operations, 1, true, 4, 4, false, true, false, &row, &left, &round_robin, &cpu ) );
    rocketnet_unit_check( row == 1 && !left && round_robin == 0 && cpu == ROCKETNET_URING_PIN_NONE );

    // the pin and the confine refused: counted once, left on cpu 0, no SCHED_RR
    rocketnet_unit_check( rocketnet_server_uring_test_place_worker( stop_spread_uring_pin_read, uring_operations, 1, true, 4, 4, false, true, true, &row, &left, &round_robin, &cpu ) );
    rocketnet_unit_check( row == 1 && left && round_robin == 0 );

    // one cpu online: the pin refused (counted once), nothing but cpu 0 to
    // confine to, left on cpu 0, no SCHED_RR; the call returns: the host runs
    rocketnet_unit_check( rocketnet_server_uring_test_place_worker( stop_spread_uring_pin_read, uring_operations, 1, true, 1, 1, false, false, false, &row, &left, &round_robin, &cpu ) );
    rocketnet_unit_check( row == 1 && left && round_robin == 0 );

    // the pin and the confine refused on a mask without cpu 0 (cpus 1 .. 3):
    // counted once, not on cpu 0, so not flagged; its pin was refused, so no
    // SCHED_RR (a cpuset of game cores)
    rocketnet_unit_check( rocketnet_server_uring_test_place_worker( stop_spread_uring_pin_read, uring_operations, 1, true, 4, 4, true, true, true, &row, &left, &round_robin, &cpu ) );
    rocketnet_unit_check( row == 1 && !left && round_robin == 0 );

    // cpu 0 not reserved: worker 0 pins to cpu 0 (num_os_cpus 3 not applied),
    // not counted, SCHED_RR asked for
    rocketnet_unit_check( rocketnet_server_uring_test_place_worker( stop_spread_uring_pin_read, uring_operations, 3, false, 4, 4, false, false, false, &row, &left, &round_robin, &cpu ) );
    rocketnet_unit_check( row == 0 && !left && round_robin == 1 && cpu == 0 );

    // not reserved, every set refused: counted once, not confined, not flagged
    // although its mask holds cpu 0, no SCHED_RR
    rocketnet_unit_check( rocketnet_server_uring_test_place_worker( stop_spread_uring_pin_read, uring_operations, 1, false, 4, 4, false, true, true, &row, &left, &round_robin, &cpu ) );
    rocketnet_unit_check( row == 1 && !left && round_robin == 0 && cpu == ROCKETNET_URING_PIN_NONE );

    g_pin_fold_server = NULL;
    rocketnet_log_level( saved_level );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}
REGISTER_UNIT_TEST( test_server_transport_counts_uring_pin_refusals );

#endif // #if defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING

// sdk/rocketnet_driver_builtin.cpp (development library, Linux)
typedef bool rocketnet_portable_server_test_transport_counters_t( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                                  uint64_t * values );
bool rocketnet_portable_server_test_place_worker( rocketnet_portable_server_test_transport_counters_t * dispatch, int worker, int num_os_cpus,
                                                  bool reserve_cpu_0, int online, int cpus, bool without_cpu_zero, bool refuse_pin, bool refuse_all,
                                                  uint64_t * row, bool * left_on_core_zero, int * pinned_cpu, int * set_calls, int * granted_first );

// the row as the server's fold takes it: the transport rows dispatch (the
// synthetic server has no socket, so the whole fold, which also reads the
// socket's send failures, is not run on it; the functional case
// test_portable_transport_worker_placement reads the row through the public
// counters of a real server)
static bool stop_spread_portable_pin_read( const struct rocketnet_server_transport_io_operations_t * operations, void * transport, uint64_t * values )
{
    uint64_t rows[ROCKETNET_NUM_COUNTERS] = {};
    const bool answered = rocketnet_server_transport_io_transport_counters( operations, transport, rows );
    values[ROCKETNET_COUNTER_PORTABLE_PIN_REFUSALS] = rows[ROCKETNET_COUNTER_PORTABLE_PIN_REFUSALS];
    return answered;
}

// The portable transport's placement of its two workers on Linux
// (sdk/rocketnet_driver_builtin.cpp, portable_place_worker) with only the
// refusals injected (rocketnet_portable_server_test_place_worker, a fresh
// synthetic server each call): the send worker (0) pins to cpu num_os_cpus,
// the receive worker (1) to num_os_cpus + 1, as the gso workers do, and
// PORTABLE_PIN_REFUSALS is read by value as the server's fold takes it. Its row
// counts refused pins and the failed confinements after them; a refused
// worker is confined to the cpus from num_os_cpus up, and flagged when it is
// left on a mask that holds cpu 0. The portable workers never ask for
// real-time scheduling. With cpu 0 not reserved (portable_reserve_cpu_0
// ROCKETNET_CPU_0_NOT_RESERVED) worker w pins to cpu w, num_os_cpus is not
// applied, and a refused pin is counted but neither confined nor flagged.

static void test_server_transport_counts_portable_pin_refusals()
{
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );    // each refusal logs

    uint64_t row = UINT64_MAX;
    bool left = true;
    int cpu = -100;
    int sets = -1;
    int granted = -100;

    // granted: send on cpu 1, receive on cpu 2, one set each, not counted
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 1, true, 4, 4, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 0 && !left && cpu == 1 && sets == 1 && granted == 1 );
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 1, 1, true, 4, 4, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 0 && !left && cpu == 2 && sets == 1 && granted == 2 );

    // two cpus left to the OS: receive on cpu 3
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 1, 2, true, 4, 4, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 0 && !left && cpu == 3 );

    // num_os_cpus 0 reads as 1: the OS keeps cpu 0
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 0, true, 4, 4, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 0 && cpu == 1 );

    // the receive worker's cpu (num_os_cpus 3, cpu 4) is not online: refused,
    // counted once, no pin tried, confined to the cpus from num_os_cpus up:
    // cpu 3, the last online
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 1, 3, true, 4, 4, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 1 && !left && cpu == ROCKETNET_GSO_PIN_NONE && sets == 1 && granted == 3 );

    // the pin refused, the confine (cpus 1 .. 3) taken: counted once
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 1, true, 4, 4, false, true, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 1 && !left && cpu == ROCKETNET_GSO_PIN_NONE && sets == 2 && granted == 1 );

    // the pin and the confine refused: counted twice, left on cpu 0
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 1, true, 4, 4, false, true, true, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 2 && left && cpu == ROCKETNET_GSO_PIN_NONE && sets == 2 && granted == -1 );

    // the pin and the confine refused on a mask without cpu 0 (a cpuset of
    // game cores): counted twice, not on cpu 0, so not flagged
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 1, true, 4, 4, true, true, true, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 2 && !left );

    // one cpu online: the pin refused (counted), nothing but cpu 0 to confine
    // to (counted, no set tried), left on cpu 0; the call returns: the host runs
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 1, true, 1, 1, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 2 && left && cpu == ROCKETNET_GSO_PIN_NONE && sets == 0 );

    // cpu 0 not reserved: send on cpu 0, receive on cpu 1, num_os_cpus 3 not applied
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 3, false, 4, 4, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 0 && !left && cpu == 0 && sets == 1 && granted == 0 );
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 1, 3, false, 4, 4, false, false, false, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 0 && !left && cpu == 1 && sets == 1 );

    // not reserved, the pin refused: counted once, not confined (one set, the
    // pin), not flagged although its mask holds cpu 0
    rocketnet_unit_check( rocketnet_portable_server_test_place_worker( stop_spread_portable_pin_read, 0, 1, false, 4, 4, false, true, true, &row, &left, &cpu, &sets, &granted ) );
    rocketnet_unit_check( row == 1 && !left && cpu == ROCKETNET_GSO_PIN_NONE && sets == 1 && granted == -1 );

    rocketnet_log_level( saved_level );
}
REGISTER_UNIT_TEST( test_server_transport_counts_portable_pin_refusals );

#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

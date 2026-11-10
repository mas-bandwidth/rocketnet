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
    int used;                                           // frames staged since the last flush
    int refused_for_room;                               // starts called with the staging full
    int unreachable_port;                               // a destination with no link-layer address, 0: none
    int refused_port;                                   // a destination the socket cannot carry, 0: none
    int stops;                                          // the I/O told to stop
    bool hold_flushed;                                  // a flush hands frames on without finishing them
    int in_flight;                                      // frames flushed and not yet finished
    int staged[STOP_SPREAD_MAX_CLIENTS];                // disconnect packets staged, by client
    int flushed[STOP_SPREAD_MAX_CLIENTS];               // disconnect packets flushed, by client
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
        return NULL;
    if ( f->used >= f->capacity )
    {
        f->refused_for_room++;
        return NULL;
    }
    f->reserved_to = *to;
    *id = (uint64_t) f->used;

    return stop_spread_frame;
}

static void stop_spread_finish_packet( void * p, uint64_t, uint8_t * frame, int )
{
    stop_spread_fixture_t * f = (stop_spread_fixture_t *) p;
    f->used++;
    if ( frame[ROCKETNET_FRAME_WIRE_OFFSET] != ROCKETNET_CONNECTION_DISCONNECT_PACKET )
        return;
    const int client = f->reserved_to.port - 1000;
    rocketnet_unit_check( client >= 0 && client < STOP_SPREAD_MAX_CLIENTS );
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
        f->in_flight += f->used;
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
        total += f->flushed[i];
    return total;
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
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
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

    return server;
}

// The stop as rocketnet_server_stop drives it: every connected slot ends
// with its burst waiting, then the transport stops.
static void stop_spread_stop_server( rocketnet_server_transport_t * server, int clients )
{
    for ( int i = 0; i < clients; i++ )
        rocketnet_server_transport_disconnect_client_shutdown( server, i );
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
        rocketnet_unit_check( f->flushed[i] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

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
        memset( frame + context.payload_index, i, 16 );
        rocketnet_unit_check( rocketnet_server_transport_finish_packet( server, &context, 16 ) > 0 );
    }

    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == 19 );

    f->used = f->capacity;
    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == 0 );

    rocketnet_server_transport_send_packets( server );
    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == 24 );

    operations.send_room = NULL;
    rocketnet_unit_check( rocketnet_server_transport_send_capacity_available( server ) == INT_MAX );

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
    memset( frame + context.payload_index, 1, 16 );
    rocketnet_unit_check( rocketnet_server_transport_finish_packet( server, &context, 16 ) > 0 );

    // the staging is full: a full ring

    rocketnet_unit_check( !rocketnet_server_transport_start_packet( server, 0, 16, &context ) );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + 1 );
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
        rocketnet_unit_check( !rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->stops == 0 );

    f->in_flight = 0;
    rocketnet_unit_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_unit_check( f->stops == 1 );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

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
        seed[i] = (uint8_t) ( 0x6C ^ i );

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

REGISTER_UNIT_TEST( test_server_stop_spreads_disconnect_bursts_across_updates );
REGISTER_UNIT_TEST( test_server_stop_waits_for_flushed_sends );
REGISTER_UNIT_TEST( test_server_refused_starts_counted_by_cause );
REGISTER_UNIT_TEST( test_server_stop_polls_are_the_stop_steps );
REGISTER_UNIT_TEST( test_server_stop_abandons_an_unreachable_burst );
REGISTER_UNIT_TEST( test_server_kick_spreads_its_burst_across_updates );
REGISTER_UNIT_TEST( test_server_send_capacity_reports_staged_room );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_server_stop_sends_burst_staged_in_link_simulator );
#endif // #if ROCKETNET_DEV

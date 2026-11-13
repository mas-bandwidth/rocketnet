/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public link simulator's production refusal: a nonzero simulated loss,
// latency, jitter or duplicate value is refused by the same config walk a
// production create uses, and admitted when the build is development. This
// exercises the walk over the real transport config tables, so the four
// fields' presence, their ranges and their refusal are pinned together.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_io.h"
#include "loopback_hub.h"

#if ROCKETNET_DEV
static double live_link_clock = 1000.0;

static double live_link_time_callback( void * )
{
    return live_link_clock;
}

struct live_link_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    double time;
};

static bool live_link_pair_setup( struct live_link_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    live_link_clock = pair->time;

    const uint64_t protocol_id = 0x6c6f6f706261636bULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    pair->server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    check( pair->server != NULL );
    if ( !pair->server )
    {
        return false;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( pair->server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x9e3779b97f4a7c15ULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = live_link_time_callback;
    pair->client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), pair->time );
    check( pair->client != NULL );
    return pair->client != NULL;
}

static void live_link_pair_step( struct live_link_pair_t * pair, double dt )
{
    pair->time += dt;
    live_link_clock = pair->time;
    rocketnet_server_transport_update( pair->server, pair->time );
    rocketnet_client_transport_update( pair->client, pair->time );
    rocketnet_server_transport_send_packets( pair->server );
    rocketnet_client_transport_send_packets( pair->client );
}

static bool live_link_pair_connect( struct live_link_pair_t * pair )
{
    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_is_client_connected( pair->server, 0 ) )
        {
            return true;
        }
        live_link_pair_step( pair, 0.01 );
    }

    return false;
}

static void live_link_pair_teardown( struct live_link_pair_t * pair )
{
    rn_test_client_transport_stop_and_destroy( pair->client );
    rn_test_server_transport_stop_and_destroy( pair->server );
}

// every payload the server hands over must be the one the case sent, whole:
// a cut or changed payload fails the case rather than counting

static int live_link_server_payload_count( struct live_link_pair_t * pair, const uint8_t * expected, int expected_bytes )
{
    const int * indices = NULL;
    uint8_t * const * data = NULL;

    const int * bytes = NULL;
    const int count = rocketnet_server_transport_payloads( pair->server, &indices, &data, &bytes );
    for ( int i = 0; i < count; i++ )
    {
        check( indices[i] == 0 );
        check( bytes[i] == expected_bytes );
        check( bytes[i] != expected_bytes || memcmp( data[i], expected, (size_t) expected_bytes ) == 0 );
    }
    return count;
}

static int live_link_client_receive( struct live_link_pair_t * pair, const uint8_t * expected, int expected_bytes )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int bytes = rocketnet_client_transport_receive_packet( pair->client, packet, (int) sizeof( packet ) );
    if ( bytes > 0 )
    {
        check( bytes == expected_bytes );
        check( bytes != expected_bytes || memcmp( packet, expected, (size_t) expected_bytes ) == 0 );
    }
    return bytes;
}

// The server's payload list and the client's delivery cursor both hold one
// update's worth: a payload delivered in an earlier update is gone by the
// next one. So the count is taken after every step and summed, and the
// client's cursor is drained at every step, which is also what the API asks
// of an application (take every message before the next update).

static int live_link_server_payloads_seen( struct live_link_pair_t * pair, int steps, const uint8_t * expected, int expected_bytes )
{
    int seen = 0;
    for ( int i = 0; i < steps; i++ )
    {
        live_link_pair_step( pair, 0.01 );
        seen += live_link_server_payload_count( pair, expected, expected_bytes );
    }

    return seen;
}

static int live_link_client_bytes_seen( struct live_link_pair_t * pair, int steps, const uint8_t * expected, int expected_bytes )
{
    int seen = 0;
    for ( int i = 0; i < steps; i++ )
    {
        live_link_pair_step( pair, 0.01 );
        int bytes = live_link_client_receive( pair, expected, expected_bytes );
        while ( bytes > 0 )
        {
            seen += bytes;
            bytes = live_link_client_receive( pair, expected, expected_bytes );
        }
    }

    return seen;
}

static void test_public_link_simulator_applies_to_live_connection()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct rocketnet_client_transport_stats_t before;
    struct rocketnet_client_transport_stats_t after;
    rocketnet_client_transport_stats( pair.client, &before );

    // the setting applies to the live connection at once: the packet the
    // client sends after the call leaves the client and never reaches the
    // server

    check( rocketnet_client_transport_network_simulator( pair.client, 0, 0, 100, 0 ) );

    uint8_t payload[4] = { 1, 2, 3, 4 };
    rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
    check( live_link_server_payloads_seen( &pair, 20, payload, (int) sizeof( payload ) ) == 0 );

    rocketnet_client_transport_stats( pair.client, &after );
    check( after.packets_sent > before.packets_sent );

    // the connection is still up: the loss applies to the packets, not to the
    // connection

    check( rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_server_transport_is_client_connected( pair.server, 0 ) );

    // and the setting comes back off: the next packet is delivered

    check( rocketnet_client_transport_network_simulator( pair.client, 0, 0, 0, 0 ) );

    rocketnet_client_transport_send_packet( pair.client, payload, sizeof( payload ) );
    check( live_link_server_payloads_seen( &pair, 20, payload, (int) sizeof( payload ) ) > 0 );

    live_link_pair_teardown( &pair );
}

static void test_public_link_simulator_server_applies_to_live_start()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct rocketnet_server_transport_stats_t before;
    struct rocketnet_server_transport_stats_t after;
    rocketnet_server_transport_stats( pair.server, &before );

    // the setting applies to the running server at once: the packet the
    // server sends after the call leaves the server and never reaches the
    // client

    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 100, 0 ) );

    uint8_t payload[4] = { 5, 6, 7, 8 };
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( live_link_client_bytes_seen( &pair, 20, payload, (int) sizeof( payload ) ) == 0 );

    rocketnet_server_transport_stats( pair.server, &after );
    check( after.packets_sent > before.packets_sent );

    // and the setting comes back off: the next packet is delivered

    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 0, 0 ) );

    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( live_link_client_bytes_seen( &pair, 20, payload, (int) sizeof( payload ) ) > 0 );

    live_link_pair_teardown( &pair );
}

// A server switched from latency to all-zero while the simulator holds
// packets still delivers every one of them: the setting changes what the
// simulator does with the packets sent after it, never the ones it holds.
static void test_public_link_simulator_server_switch_off_delivers_held()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    check( rocketnet_server_transport_network_simulator( pair.server, 200, 0, 0, 0 ) );

    const int num_payloads = 8;
    uint8_t payload[4] = { 9, 10, 11, 12 };

    for ( int i = 0; i < num_payloads; i++ )
    {
        rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    }

    check( live_link_client_bytes_seen( &pair, 1, payload, (int) sizeof( payload ) ) == 0 );

    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 0, 0 ) );
    check( live_link_client_bytes_seen( &pair, 40, payload, (int) sizeof( payload ) ) == num_payloads * (int) sizeof( payload ) );
    check( rocketnet_server_transport_is_client_connected( pair.server, 0 ) );

    live_link_pair_teardown( &pair );
}

// A datagram is counted sent only once the link took it: a send that meets the
// simulator's lock busy is dropped and never reaches packets_sent or
// bytes_sent. The link simulator exists only in a development build, so the
// drop is counted by the development count link_lock_misses, never by
// SEND_RING_FULL, which names a full send ring. The flush is called alone, without an
// update, so the one busy send is the payload's and nothing else's.
static void test_public_link_simulator_server_lock_miss_not_counted_sent()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct rocketnet_server_transport_stats_t before;
    struct rocketnet_server_transport_stats_t after;
    rocketnet_server_transport_stats( pair.server, &before );
    const uint64_t ring_full_before = rocketnet_server_transport_counters( pair.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    const uint64_t misses_before = rocketnet_server_transport_link_lock_misses_for_test( pair.server );

    // a nonzero latency keeps the simulator active, so the send goes through
    // the link and its lock

    check( rocketnet_server_transport_network_simulator( pair.server, 1, 0, 0, 0 ) );

    rocketnet_server_transport_link_busy_for_test( pair.server, 1 );

    uint8_t payload[4] = { 13, 14, 15, 16 };
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    rocketnet_server_transport_send_packets( pair.server );
    rocketnet_server_transport_stats( pair.server, &after );
    check( after.packets_sent == before.packets_sent );
    check( after.bytes_sent == before.bytes_sent );
    check( rocketnet_server_transport_counters( pair.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full_before );
    check( rocketnet_server_transport_link_lock_misses_for_test( pair.server ) == misses_before + 1 );

    // the next send meets a free lock: it is counted sent once and arrives

    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    rocketnet_server_transport_send_packets( pair.server );
    rocketnet_server_transport_stats( pair.server, &after );
    check( after.packets_sent == before.packets_sent + 1 );
    check( after.bytes_sent > before.bytes_sent );
    check( rocketnet_server_transport_counters( pair.server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full_before );
    check( rocketnet_server_transport_link_lock_misses_for_test( pair.server ) == misses_before + 1 );
    check( live_link_client_bytes_seen( &pair, 20, payload, (int) sizeof( payload ) ) == (int) sizeof( payload ) );

    live_link_pair_teardown( &pair );
}

// The three staged commit sites (a public start/finish, a payload that
// collapses out of the arena, a disconnect burst) each choose between the link
// simulator and the transport's finish. The fake here is a staged transport
// counting what the server hands it; the server connected over loopback and
// then sends through the fake. The simulator holding the packet (any active
// setting, a simulated loss included, is a datagram sent) gives the slot back
// by abort and is charged; a busy simulator lock gives it back too and is not
// charged; the pass-through finishes it. No sockets of the fake's own, and the
// clock is an argument.
struct staged_fake_t
{
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE];
    uint64_t next_id;
    int starts;
    int finishes;
    int aborts;
    int batches;
    int capacity;                                       // frames a flush takes, 0: unbounded
    int used;                                           // frames finished since the last flush
    int refused;                                        // starts refused for room
    int payload_finishes;                               // payload packets the staging took
    struct rocketnet_server_socket_process_packets_t view;
};

static uint8_t * staged_fake_start( void * instance, const rocketnet_address_t *, uint64_t * id )
{
    struct staged_fake_t * fake = (struct staged_fake_t *) instance;
    fake->starts++;
    if ( fake->capacity > 0 && fake->used >= fake->capacity )
    {
        fake->refused++;
        return NULL;
    }
    *id = ++fake->next_id;
    return fake->frame;
}
static void staged_fake_finish( void * instance, uint64_t, uint8_t * frame, int )
{
    struct staged_fake_t * fake = (struct staged_fake_t *) instance;
    fake->finishes++;
    fake->used++;
    if ( frame[ROCKETNET_FRAME_WIRE_OFFSET] == ROCKETNET_CONNECTION_PAYLOAD_PACKET )
    {
        fake->payload_finishes++;
    }
}
static void staged_fake_abort( void * instance, uint64_t, uint8_t * ) { ( (struct staged_fake_t *) instance )->aborts++; }
static void staged_fake_flush( void * instance ) { ( (struct staged_fake_t *) instance )->used = 0; }
static int staged_fake_send_room( void * instance )
{
    struct staged_fake_t * fake = (struct staged_fake_t *) instance;
    return fake->capacity - fake->used;
}
static struct rocketnet_server_socket_process_packets_t * staged_fake_receive( void * instance )
{
    return &( (struct staged_fake_t *) instance )->view;
}
static void staged_fake_send_batch( void * instance, const rocketnet_platform_send_t *, int ) { ( (struct staged_fake_t *) instance )->batches++; }

// loss and latency are the simulator setting; busy is the number of link sends
// that meet the lock held. Returns through the out parameters what each site
// did: the fake's aborts and finishes and the server's packets_sent.
static void staged_sites_run( int latency_ms, int loss_percent, int busy, int * aborts, int * finishes, int * charged )
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct staged_fake_t * fake = (struct staged_fake_t *) calloc( 1, sizeof( *fake ) );
    check( fake != NULL );
    struct rocketnet_server_transport_io_operations_t operations = {};
    operations.flags = ROCKETNET_TRANSPORT_STAGED;
    operations.start_packet = staged_fake_start;
    operations.finish_packet = staged_fake_finish;
    operations.abort_packet = staged_fake_abort;
    operations.flush = staged_fake_flush;
    operations.receive = staged_fake_receive;
    operations.send_batch = staged_fake_send_batch;

    const struct rocketnet_server_transport_io_operations_t * swapped_operations = &operations;
    void * swapped_instance = fake;
    rocketnet_server_transport_swap_io_for_test( pair.server, &swapped_operations, &swapped_instance );

    check( rocketnet_server_transport_network_simulator( pair.server, latency_ms, 0, loss_percent, 0 ) );
    rocketnet_server_transport_link_busy_for_test( pair.server, busy );

    struct rocketnet_server_transport_stats_t before;
    struct rocketnet_server_transport_stats_t after;
    rocketnet_server_transport_stats( pair.server, &before );

    // site: a public start and finish of an ordinary payload
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( fake->starts == 1 );

    // site: a full-capacity acquire that finishes small
    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( pair.server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
    check( frame != NULL );
    memcpy( frame, payload, sizeof( payload ) );
    check( rocketnet_server_transport_finish_packet( pair.server, &context, (int) sizeof( payload ) ) > 0 );
    check( fake->starts == 2 );

    // site: the disconnect burst
    rocketnet_server_transport_disconnect_client( pair.server, 0 );
    check( fake->starts > 2 );
    rocketnet_server_transport_send_packets( pair.server );
    rocketnet_server_transport_stats( pair.server, &after );

    check( fake->batches == 0 );
    check( fake->aborts + fake->finishes == fake->starts );
    *aborts = fake->aborts;
    *finishes = fake->finishes;
    *charged = (int) ( after.packets_sent - before.packets_sent );
    check( *charged >= 0 && *charged <= fake->starts );

    rocketnet_server_transport_swap_io_for_test( pair.server, &swapped_operations, &swapped_instance );
    live_link_pair_teardown( &pair );
    free( fake );
}

static void test_public_link_simulator_staged_commit_arms()
{
    int aborts, finishes, charged;

    // a simulator holding every packet at 100% loss still took them: the slot
    // goes back by abort, nothing finishes, every datagram is charged sent

    staged_sites_run( 0, 100, 0, &aborts, &finishes, &charged );
    check( aborts >= 3 && finishes == 0 && charged == aborts );

    // the lock busy: the slot goes back by abort and nothing is charged

    staged_sites_run( 1, 0, 1000, &aborts, &finishes, &charged );
    check( aborts >= 3 && finishes == 0 && charged == 0 );

    // an all-zero setting is a pass-through: the staging finishes every frame

    staged_sites_run( 0, 0, 0, &aborts, &finishes, &charged );
    check( finishes >= 3 && aborts == 0 && charged == finishes );
}

// The held packets come due into a staged transport whose staging takes three
// frames a flush: ten payloads held together come due together, and each
// update's drain moves only what the room takes and leaves the rest held, so
// all ten reach the transport and none is refused and lost. The clock is an
// argument.
static void test_public_link_simulator_drain_spreads_within_room()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct staged_fake_t * fake = (struct staged_fake_t *) calloc( 1, sizeof( *fake ) );
    check( fake != NULL );
    fake->capacity = 3;
    struct rocketnet_server_transport_io_operations_t operations = {};
    operations.flags = ROCKETNET_TRANSPORT_STAGED;
    operations.start_packet = staged_fake_start;
    operations.finish_packet = staged_fake_finish;
    operations.abort_packet = staged_fake_abort;
    operations.flush = staged_fake_flush;
    operations.receive = staged_fake_receive;
    operations.send_batch = staged_fake_send_batch;
    operations.send_room = staged_fake_send_room;

    const struct rocketnet_server_transport_io_operations_t * swapped_operations = &operations;
    void * swapped_instance = fake;
    rocketnet_server_transport_swap_io_for_test( pair.server, &swapped_operations, &swapped_instance );

    check( rocketnet_server_transport_network_simulator( pair.server, 50, 0, 0, 0 ) );

    // each payload is taken by the simulator, its staging slot given back,
    // and all ten are held for the same 50 ms

    const int payloads = 10;
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for ( int i = 0; i < payloads; i++ )
    {
        rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    }
    check( fake->finishes == 0 && fake->aborts == payloads );

    double time = pair.time;
    for ( int update = 0; update < 8; update++ )
    {
        const int before = fake->finishes;
        time += 0.1;
        rocketnet_server_transport_update( pair.server, time );
        check( fake->finishes - before <= fake->capacity );
    }

    check( fake->payload_finishes == payloads );
    check( fake->batches == 0 );

    rocketnet_server_transport_swap_io_for_test( pair.server, &swapped_operations, &swapped_instance );
    live_link_pair_teardown( &pair );
    free( fake );
}

// A staged commit reads the link setting once. The staged transport here has
// no send_batch, as gso has none: a commit that answered active and then sent
// as if inactive would call through it. The simulator is on, a switch to
// all-zero is handed over, and the commit's setting take meets the lock held,
// so it answers the setting in force (active): the simulator takes the
// datagram, the slot goes back by abort and it is charged. The next commit
// meets a free lock, applies the zero setting and the staging finishes it.
// The clock is an argument.
static void test_public_link_simulator_staged_commit_reads_setting_once()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    struct staged_fake_t * fake = (struct staged_fake_t *) calloc( 1, sizeof( *fake ) );
    check( fake != NULL );
    struct rocketnet_server_transport_io_operations_t operations = {};
    operations.flags = ROCKETNET_TRANSPORT_STAGED;
    operations.start_packet = staged_fake_start;
    operations.finish_packet = staged_fake_finish;
    operations.abort_packet = staged_fake_abort;
    operations.flush = staged_fake_flush;
    operations.receive = staged_fake_receive;

    const struct rocketnet_server_transport_io_operations_t * swapped_operations = &operations;
    void * swapped_instance = fake;
    rocketnet_server_transport_swap_io_for_test( pair.server, &swapped_operations, &swapped_instance );

    // the simulator on and in force: the send takes the setting

    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    check( rocketnet_server_transport_network_simulator( pair.server, 1, 0, 0, 0 ) );
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( fake->aborts == 1 && fake->finishes == 0 );

    struct rocketnet_server_transport_stats_t before;
    struct rocketnet_server_transport_stats_t after;
    rocketnet_server_transport_send_packets( pair.server );
    rocketnet_server_transport_stats( pair.server, &before );

    // the switch off waits behind a held lock at the commit's one read

    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 0, 0 ) );
    rocketnet_server_transport_link_setting_busy_for_test( pair.server, 1 );
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( fake->aborts == 2 && fake->finishes == 0 );

    // the lock free: the zero setting applies and the staging finishes it

    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( fake->aborts == 2 && fake->finishes == 1 );

    rocketnet_server_transport_send_packets( pair.server );
    rocketnet_server_transport_stats( pair.server, &after );
    check( after.packets_sent == before.packets_sent + 2 );

    rocketnet_server_transport_swap_io_for_test( pair.server, &swapped_operations, &swapped_instance );
    live_link_pair_teardown( &pair );
    free( fake );
}
#endif // #if ROCKETNET_DEV

static void public_link_simulator_check_fields( const struct rocketnet_config_field_t * fields, int num_fields )
{
    int found_loss = 0;
    int found_latency = 0;
    int found_jitter = 0;
    int found_duplicate = 0;
    for ( int i = 0; i < num_fields; i++ )
    {
        if ( strcmp( fields[i].name, "simulated_loss_percent" ) == 0 )
        {
            found_loss = 1;
        }

        if ( strcmp( fields[i].name, "simulated_latency_ms" ) == 0 )
        {
            found_latency = 1;
        }

        if ( strcmp( fields[i].name, "simulated_jitter_ms" ) == 0 )
        {
            found_jitter = 1;
        }

        if ( strcmp( fields[i].name, "simulated_duplicate_percent" ) == 0 )
        {
            found_duplicate = 1;
        }
    }

    check( found_loss );
    check( found_latency );
    check( found_jitter );
    check( found_duplicate );
}

static void test_public_link_simulator_production_refusal()
{
    // the client transport, from its real table

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );

    int client_fields = 0;
    const struct rocketnet_config_field_t * client_table = rocketnet_client_transport_config_fields( &client_fields );
    public_link_simulator_check_fields( client_table, client_fields );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_latency_ms = 25;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_latency_ms = 0;
    client_config.simulated_duplicate_percent = 50;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    client_config.simulated_duplicate_percent = 0;
    client_config.simulated_loss_percent = 100;
    check( !rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "client", &client_config, client_table, client_fields, true ) );

    // the server transport, from its real table

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );

    int server_fields = 0;
    const struct rocketnet_config_field_t * server_table = rocketnet_server_transport_config_fields( &server_fields );
    public_link_simulator_check_fields( server_table, server_fields );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );

    server_config.simulated_jitter_ms = 10;
    check( !rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, false ) );
    check( rocketnet_config_validate_public_link_simulator( "server", &server_config, server_table, server_fields, true ) );

    // ranges: percentages cap at 100, milliseconds at 60000

    client_config.simulated_loss_percent = 101;
    check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );

    client_config.simulated_loss_percent = 0;
    client_config.simulated_latency_ms = 60001;
    check( !rocketnet_config_validate( "client", &client_config, client_table, client_fields ) );

    client_config.simulated_latency_ms = 0;
}

REGISTER_UNIT_TEST( test_public_link_simulator_production_refusal );

#include "rocketnet_network_simulator.h"
#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_connect_token.h"
#include "fake_client_driver.h"

struct public_link_wire_t
{
    struct rocketnet_network_simulator_t * simulator;
    struct rocketnet_address_t from;
    struct rocketnet_address_t to;
    double time;
    uint8_t received[ROCKETNET_RECEIVE_CEILING];
};

static void public_link_wire_begin( struct public_link_wire_t * wire,
                                    const struct rocketnet_client_transport_config_t * config,
                                    uint64_t seed )
{
    memset( wire, 0, sizeof( *wire ) );
    wire->time = 1000.0;
    wire->simulator = rocketnet_network_simulator_create( NULL, 256, seed );
    check( wire->simulator != NULL );
    check( rocketnet_address_parse( &wire->from, "198.51.100.1:10000" ) );
    check( rocketnet_address_parse( &wire->to, "198.51.100.2:10000" ) );

    rocketnet_network_simulator_config( wire->simulator,
                                        config->simulated_latency_ms / 1000.0,
                                        config->simulated_jitter_ms / 1000.0,
                                        (float) config->simulated_loss_percent,
                                        (float) config->simulated_duplicate_percent );
}

static void public_link_wire_end( struct public_link_wire_t * wire )
{
    rocketnet_network_simulator_destroy( wire->simulator );
}

static void public_link_wire_send( struct public_link_wire_t * wire, uint8_t value )
{
    rocketnet_network_simulator_send( wire->simulator, &wire->from, &wire->to, &value, 1, wire->time );
}

static int public_link_wire_receive( struct public_link_wire_t * wire )
{
    struct rocketnet_address_t from;
    return rocketnet_network_simulator_receive( wire->simulator, &wire->to, wire->time, &from, wire->received );
}

#if ROCKETNET_DEV
static void public_link_simulator_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = UINT64_C( 1 ) << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static rocketnet_client_transport_t * public_link_simulator_client(
    rocketnet_fake_client_transport_io_t * transport, const rocketnet_client_transport_config_t * config )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_link_simulator_token( token );
    return rocketnet_client_transport_create_step_driven_current( transport, token, config, 1000.0,
                                                                  &rocketnet_fake_client_operations_base, transport );
}

static void test_public_link_simulator_drops_at_loss()
{

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_loss_percent = 100;

    rocketnet_fake_client_transport_io_t * dropped = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = public_link_simulator_client( dropped, &config );
    check( transport != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1001.0 );

    check( dropped->num_sent == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete dropped;

    config.simulated_loss_percent = 0;

    rocketnet_fake_client_transport_io_t * direct = new rocketnet_fake_client_transport_io_t();
    transport = public_link_simulator_client( direct, &config );
    check( transport != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );

    check( direct->num_sent == 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete direct;
}

static void test_public_link_simulator_latency()
{

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_latency_ms = 25;

    rocketnet_fake_client_transport_io_t * transport_io = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = public_link_simulator_client( transport_io, &config );
    check( transport != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );

    check( transport_io->num_sent == 0 );

    rocketnet_client_transport_update( transport, 1000.024 );

    check( transport_io->num_sent == 0 );

    rocketnet_client_transport_update( transport, 1000.025 );

    check( transport_io->num_sent == 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete transport_io;
}
#endif // #if ROCKETNET_DEV

static void test_public_link_simulator_jitter()
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_latency_ms = 10;
    config.simulated_jitter_ms = 20;

    struct public_link_wire_t wire;
    public_link_wire_begin( &wire, &config, 13 );

    for ( int i = 0; i < 64; i++ )
    {
        public_link_wire_send( &wire, (uint8_t) i );
    }

    // each packet's arrival step (whole milliseconds after the send), or -1
    // before it arrives; each arrives once, within latency .. latency +
    // jitter (10 .. 30 ms), and the delays spread over that range

    int arrived_ms[64];
    for ( int i = 0; i < 64; i++ )
    {
        arrived_ms[i] = -1;
    }

    int delivered = 0;
    int reordered = 0;
    int last_value = -1;
    for ( int step = 1; step <= 31; step++ )
    {
        wire.time = 1000.0 + 0.001 * step;
        while ( public_link_wire_receive( &wire ) > 0 )
        {
            const int value = wire.received[0];
            check( value >= 0 && value < 64 );
            if ( value < 0 || value >= 64 )
            {
                continue;
            }
            check( arrived_ms[value] == -1 );
            arrived_ms[value] = step;
            reordered += value < last_value ? 1 : 0;
            last_value = value;
            delivered++;
        }
    }

    check( delivered == 64 );

    int earliest = 1000;
    int latest = -1;
    bool seen[32] = {};
    int distinct = 0;
    for ( int i = 0; i < 64; i++ )
    {
        check( arrived_ms[i] >= 10 && arrived_ms[i] <= 31 );
        if ( arrived_ms[i] < 0 || arrived_ms[i] > 31 )
        {
            continue;
        }
        earliest = arrived_ms[i] < earliest ? arrived_ms[i] : earliest;
        latest = arrived_ms[i] > latest ? arrived_ms[i] : latest;
        distinct += seen[arrived_ms[i]] ? 0 : 1;
        seen[arrived_ms[i]] = true;
    }

    // 64 uniform delays over 20 ms: a simulator that ignored jitter would put
    // them all at 10 ms, in send order

    check( earliest <= 15 && latest >= 25 );
    check( distinct >= 10 );
    check( reordered > 0 );

    public_link_wire_end( &wire );
}

#if ROCKETNET_DEV
static void test_public_link_simulator_duplicate()
{

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_duplicate_percent = 100;

    rocketnet_fake_client_transport_io_t * transport_io = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = public_link_simulator_client( transport_io, &config );
    check( transport != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );

    check( transport_io->num_sent == 0 );

    rocketnet_client_transport_update( transport, 1000.001 );

    check( transport_io->num_sent == 2 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete transport_io;
}
#endif // #if ROCKETNET_DEV

static void test_public_link_simulator_same_seed_same_schedule()
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.simulated_loss_percent = 15;
    config.simulated_latency_ms = 5;
    config.simulated_jitter_ms = 30;
    config.simulated_duplicate_percent = 25;

    struct public_link_wire_t a;
    struct public_link_wire_t b;
    public_link_wire_begin( &a, &config, 23 );
    public_link_wire_begin( &b, &config, 23 );

    for ( int i = 0; i < 64; i++ )
    {
        public_link_wire_send( &a, (uint8_t) i );
        public_link_wire_send( &b, (uint8_t) i );
    }

    // the two schedules match packet for packet, and the schedule is not the
    // trivial one: some packets are lost, some duplicated, some reordered,
    // so the equality is of a schedule the seed drew

    int received = 0;
    int reordered = 0;
    int last_value = -1;
    int count[64] = {};
    for ( int step = 0; step < 40; step++ )
    {
        a.time += 0.001;
        b.time += 0.001;

        for ( ;; )
        {
            const int a_bytes = public_link_wire_receive( &a );
            const int b_bytes = public_link_wire_receive( &b );
            check( a_bytes == b_bytes );
            if ( a_bytes == 0 )
            {
                break;
            }
            check( memcmp( a.received, b.received, (size_t) a_bytes ) == 0 );
            const int value = a.received[0];
            check( value >= 0 && value < 64 );
            if ( value >= 0 && value < 64 )
            {
                count[value]++;
            }
            reordered += value < last_value ? 1 : 0;
            last_value = value;
            received++;
        }
    }

    int lost = 0;
    int duplicated = 0;
    for ( int i = 0; i < 64; i++ )
    {
        lost += count[i] == 0 ? 1 : 0;
        duplicated += count[i] > 1 ? 1 : 0;
    }
    check( received > 0 );
    check( lost > 0 && duplicated > 0 && reordered > 0 );

    public_link_wire_end( &a );
    public_link_wire_end( &b );
}

#include "rocketnet_platform.h"

#if !defined( _WIN32 )
#include <sys/mman.h>
#include <unistd.h>
#endif // #if !defined( _WIN32 )

extern bool rocketnet_network_simulator_take_setting( struct rocketnet_network_simulator_t * simulator, struct rocketnet_platform_mutex_t * mutex );
extern void rocketnet_network_simulator_reconfigure( struct rocketnet_network_simulator_t * simulator, struct rocketnet_platform_mutex_t * mutex, double latency, double jitter, float loss_percent, float duplicate_percent );
extern bool rocketnet_network_simulator_holds_any( struct rocketnet_network_simulator_t * simulator );

// An inactive simulator (every setting zero, nothing handed over) costs the
// send path no lock. A take with nothing handed over is first asked with a
// mutex that was never created: the answer comes from the setting word alone,
// so it never reaches the mutex, where a try-lock would assert that the mutex
// is not a created one. Then with the real mutex held, as a send would meet
// it. A handed-over setting is still taken under the lock, on and off. The
// never-created mutex shows a lock only where asserts are compiled in (a
// zeroed mutex is a valid unlocked one on the platforms), so that check is an
// assert-leg check.

static int public_link_simulator_lock_asserts = 0;

static void public_link_simulator_lock_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    public_link_simulator_lock_asserts++;
}

static void test_public_link_simulator_inactive_takes_no_lock()
{
    {
        struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 64, 1 );
        check( simulator != NULL );

        struct rocketnet_platform_mutex_t never_created;
        memset( &never_created, 0, sizeof( never_created ) );

        rocketnet_assert_function_t saved_assert = rn_assert_function;
        public_link_simulator_lock_asserts = 0;
        rocketnet_assert_function( public_link_simulator_lock_assert_hook );

        // inactive, nothing handed over: false, from the word

        rocketnet_network_simulator_config( simulator, 0.0, 0.0, 0.0f, 0.0f );
        check( !rocketnet_network_simulator_take_setting( simulator, &never_created ) );

        // active, nothing handed over: true, from the word, still no lock

        rocketnet_network_simulator_config( simulator, 0.1, 0.0, 0.0f, 0.0f );
        check( rocketnet_network_simulator_take_setting( simulator, &never_created ) );

        rocketnet_assert_function( saved_assert );
#if ROCKETNET_ASSERTS
        check( public_link_simulator_lock_asserts == 0 );
#endif // #if ROCKETNET_ASSERTS

        rocketnet_network_simulator_destroy( simulator );
    }

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 64, 1 );
    check( simulator != NULL );

    rocketnet_network_simulator_config( simulator, 0.0, 0.0, 0.0f, 0.0f );

    struct rocketnet_platform_mutex_t mutex;
    check( rocketnet_platform_mutex_create( &mutex ) );

    rocketnet_platform_mutex_acquire( &mutex );
    check( !rocketnet_network_simulator_take_setting( simulator, &mutex ) );
    check( !rocketnet_network_simulator_holds_any( simulator ) );

    rocketnet_platform_mutex_release( &mutex );

    rocketnet_network_simulator_reconfigure( simulator, &mutex, 0.1, 0.0, 0.0f, 0.0f );
    check( rocketnet_network_simulator_take_setting( simulator, &mutex ) );
    check( rocketnet_network_simulator_take_setting( simulator, &mutex ) );

    rocketnet_network_simulator_reconfigure( simulator, &mutex, 0.0, 0.0, 0.0f, 0.0f );
    check( !rocketnet_network_simulator_take_setting( simulator, &mutex ) );

    rocketnet_platform_mutex_acquire( &mutex );
    check( !rocketnet_network_simulator_take_setting( simulator, &mutex ) );

    rocketnet_platform_mutex_release( &mutex );

    rocketnet_platform_mutex_destroy( &mutex );
    rocketnet_network_simulator_destroy( simulator );
}

// A server whose simulator is inactive passes its packets straight through:
// the payload sent in one update reaches the client at the next one, with
// nothing held by the simulator.
#if ROCKETNET_DEV
static void test_public_link_simulator_server_inactive_passes_straight_through()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    check( rocketnet_server_transport_network_simulator( pair.server, 0, 0, 0, 0 ) );

    uint8_t payload[4] = { 13, 14, 15, 16 };
    rocketnet_server_transport_send_packet( pair.server, 0, payload, sizeof( payload ) );
    check( live_link_client_bytes_seen( &pair, 2, payload, (int) sizeof( payload ) ) == (int) sizeof( payload ) );

    live_link_pair_teardown( &pair );
}

// The client's hand-off with its simulator active gives the simulator each
// frame committed to the filling ring once, from the first slot to the last
// committed one, and no other: not an aborted slot (its frame was never
// sealed), not a slot a larger earlier fill left behind, and not a count of
// starts the full ring refused. First, fills with the simulator off, which
// move through every ring. Then three fills with it on, each drained before
// the next: five payloads and an aborted slot, then one (the earlier fill's
// later slots still hold sealed frames), then more starts than the ring
// holds. Every payload reaches the server exactly once, whole, by its own
// identity in the ledger below, and the server sees no replay or unopenable
// packet. Time is an argument.

// The ledger: every payload the fill writes carries its own identity, a 32-bit
// number in its first four bytes and the number's complement in the other
// four, so a payload that arrives cut, changed, twice, from an aborted slot or
// from a stale slot is named. An entry is OWED from its finish and RECEIVED
// once the server hands it over.

#define LIVE_LINK_LEDGER_MAX 4096

enum live_link_ledger_state_t
{
    LIVE_LINK_UNSENT = 0,
    LIVE_LINK_OWED = 1,
    LIVE_LINK_RECEIVED = 2
};

struct live_link_ledger_t
{
    uint32_t next;
    uint8_t state[LIVE_LINK_LEDGER_MAX];
};

static void live_link_ledger_write( uint8_t * frame, uint32_t id )
{
    const uint32_t inverse = ~id;
    memcpy( frame, &id, sizeof( id ) );
    memcpy( frame + 4, &inverse, sizeof( inverse ) );
}

static int live_link_client_fill( struct live_link_pair_t * pair, struct live_link_ledger_t * ledger, int payloads, int abort_at )
{
    int finished = 0;
    for ( int i = 0; i < payloads; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( pair->client, 8, &context );
        if ( !frame )
        {
            continue;
        }
        const uint32_t id = ++ledger->next;
        check( id < LIVE_LINK_LEDGER_MAX );
        live_link_ledger_write( frame, id );
        if ( i == abort_at )
        {
            rocketnet_client_transport_abort_packet( pair->client, &context );
            continue;
        }
        check( rocketnet_client_transport_finish_packet( pair->client, &context, 8 ) > 8 );
        if ( id < LIVE_LINK_LEDGER_MAX )
        {
            ledger->state[id] = LIVE_LINK_OWED;
        }
        finished++;
    }
    return finished;
}

// steps the pair, checks each payload the server hands over against the
// ledger (8 bytes, a whole identity, owed and not yet received), and returns
// how many arrived; every entry owed must have arrived by the last step

static int live_link_server_ledger( struct live_link_pair_t * pair, struct live_link_ledger_t * ledger, int steps )
{
    int arrived = 0;
    for ( int s = 0; s < steps; s++ )
    {
        live_link_pair_step( pair, 0.01 );

        const int * indices = NULL;
        uint8_t * const * data = NULL;
        const int * bytes = NULL;
        const int count = rocketnet_server_transport_payloads( pair->server, &indices, &data, &bytes );
        for ( int i = 0; i < count; i++ )
        {
            check( indices[i] == 0 );
            check( bytes[i] == 8 );
            if ( bytes[i] != 8 )
            {
                continue;
            }
            uint32_t id = 0;
            uint32_t inverse = 0;
            memcpy( &id, data[i], sizeof( id ) );
            memcpy( &inverse, data[i] + 4, sizeof( inverse ) );
            check( inverse == ~id );
            check( id > 0 && id < LIVE_LINK_LEDGER_MAX );
            if ( id == 0 || id >= LIVE_LINK_LEDGER_MAX )
            {
                continue;
            }
            check( ledger->state[id] == LIVE_LINK_OWED );
            ledger->state[id] = LIVE_LINK_RECEIVED;
            arrived++;
        }
    }

    for ( uint32_t id = 1; id <= ledger->next && id < LIVE_LINK_LEDGER_MAX; id++ )
    {
        check( ledger->state[id] != LIVE_LINK_OWED );
    }

    return arrived;
}

static void test_public_link_simulator_client_hold_sends_each_committed_frame_once()
{
    struct live_link_pair_t pair;
    check( live_link_pair_setup( &pair ) );
    check( live_link_pair_connect( &pair ) );

    const uint64_t * server_counters = rocketnet_server_transport_counters( pair.server )->values;
    const uint64_t * client_counters = rocketnet_client_transport_counters( pair.client )->values;
    const uint64_t replays = server_counters[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED];
    const uint64_t unopened = server_counters[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN];
    const uint64_t ring_full = client_counters[ROCKETNET_COUNTER_SEND_RING_FULL];

    // with the simulator off each hand-off moves the filling ring on, so
    // these fills land in every ring in turn: each finish and abort finds the
    // ring its frame lives in

    struct live_link_ledger_t * ledger = (struct live_link_ledger_t *) calloc( 1, sizeof( struct live_link_ledger_t ) );
    check( ledger != NULL );
    if ( !ledger )
    {
        live_link_pair_teardown( &pair );
        return;
    }

    for ( int round = 0; round < 6; round++ )
    {
        const int sent = live_link_client_fill( &pair, ledger, 3, 1 );
        check( sent == 2 );
        check( live_link_server_ledger( &pair, ledger, 2 ) == sent );
    }

    check( rocketnet_client_transport_network_simulator( pair.client, 10, 0, 0, 0 ) );

    const int first = live_link_client_fill( &pair, ledger, 6, 4 );
    check( first == 5 );
    check( live_link_server_ledger( &pair, ledger, 10 ) == first );

    const int second = live_link_client_fill( &pair, ledger, 1, -1 );
    check( second == 1 );
    check( live_link_server_ledger( &pair, ledger, 10 ) == second );

    // the ring takes its size less what the update commits in it, and every
    // start past that is refused and counted; the refused starts leave the
    // ring's count past its size, and the hand-off reads no slot past it

    const int ring_size = 256; // ROCKETNET_CLIENT_PAYLOAD_RING_SIZE, private to sdk/rocketnet_client_transport.cpp
    const int starts = ring_size + 1024;
    const int third = live_link_client_fill( &pair, ledger, starts, -1 );
    check( third > 0 && third <= ring_size );
    check( client_counters[ROCKETNET_COUNTER_SEND_RING_FULL] - ring_full >= (uint64_t) ( starts - ring_size ) );
    check( live_link_server_ledger( &pair, ledger, 10 ) == third );

    check( server_counters[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED] == replays );
    check( server_counters[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN] == unopened );
    check( rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    free( ledger );
    live_link_pair_teardown( &pair );
}
#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_public_link_simulator_inactive_takes_no_lock );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_public_link_simulator_server_inactive_passes_straight_through );
REGISTER_UNIT_TEST( test_public_link_simulator_drops_at_loss );
REGISTER_UNIT_TEST( test_public_link_simulator_latency );
REGISTER_UNIT_TEST( test_public_link_simulator_duplicate );
REGISTER_UNIT_TEST( test_public_link_simulator_applies_to_live_connection );
REGISTER_UNIT_TEST( test_public_link_simulator_server_applies_to_live_start );
REGISTER_UNIT_TEST( test_public_link_simulator_server_switch_off_delivers_held );
REGISTER_UNIT_TEST( test_public_link_simulator_server_lock_miss_not_counted_sent );
REGISTER_UNIT_TEST( test_public_link_simulator_staged_commit_arms );
REGISTER_UNIT_TEST( test_public_link_simulator_drain_spreads_within_room );
REGISTER_UNIT_TEST( test_public_link_simulator_client_hold_sends_each_committed_frame_once );
REGISTER_UNIT_TEST( test_public_link_simulator_staged_commit_reads_setting_once );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_public_link_simulator_jitter );
REGISTER_UNIT_TEST( test_public_link_simulator_same_seed_same_schedule );

#if !defined( _WIN32 )

// Two pages, the second neither readable nor writable, and a region of the
// asked size that ends exactly where the first page ends: a touch of one byte
// past the region faults.
struct revision_tail_t
{
    uint8_t * map;
    size_t map_bytes;
    uint8_t * bytes;
};

static bool revision_tail_open( struct revision_tail_t * tail, size_t bytes, size_t align )
{
    const long page = sysconf( _SC_PAGESIZE );
    if ( page <= 0 || bytes > (size_t) page )
    {
        return false;
    }
    tail->map_bytes = 2 * (size_t) page;
    void * map = mmap( NULL, tail->map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    if ( map == MAP_FAILED )
    {
        return false;
    }
    tail->map = (uint8_t *) map;
    if ( mprotect( tail->map + page, (size_t) page, PROT_NONE ) != 0 )
    {
        munmap( map, tail->map_bytes );
        return false;
    }
    tail->bytes = tail->map + page - bytes;
    return ( (uintptr_t) tail->bytes % align ) == 0;
}

static void revision_tail_close( struct revision_tail_t * tail )
{
    munmap( tail->map, tail->map_bytes );
}

#endif // #if !defined( _WIN32 )

// A revision-1 config is read only to its own end: the sized defaults and the
// sized creates leave the guard bytes past it untouched. Revision 1 ends
// before loopback_hub, so a revision-1 caller cannot name a hub and its
// loopback create is refused; the hub in the current struct is never read.
static void test_public_link_simulator_revision_1_prefixes()
{
    struct alignas( alignof( rocketnet_client_transport_config_t ) ) client_revision_1_t
    {
        uint8_t bytes[ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR];
        uint8_t guard[16];
    } client;
    memset( &client, 0xa5, sizeof( client ) );
    check( rocketnet_client_transport_default_config_sized(
               (rocketnet_client_transport_config_t *) client.bytes, sizeof( client.bytes ) ) == 1 );
    for ( size_t i = 0; i < sizeof( client.guard ); i++ )
    {
        check( client.guard[i] == 0xa5 );
    }

    rocketnet_client_transport_config_t client_current;
    rocketnet_client_transport_default_config_current( &client_current );
    snprintf( client_current.transport, sizeof( client_current.transport ), "%s", "loopback" );
    client_current.loopback_hub = rocketnet_test_loopback_hub();
    memcpy( client.bytes, &client_current, sizeof( client.bytes ) );
#if ROCKETNET_DEV
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_link_simulator_token( token );
    rocketnet_client_transport_t * client_transport = rocketnet_client_transport_create_sized(
        NULL, token, (const rocketnet_client_transport_config_t *) client.bytes,
        sizeof( client.bytes ), 1000.0 );
    check( client_transport == NULL );

    for ( size_t i = 0; i < sizeof( client.guard ); i++ )
    {
        check( client.guard[i] == 0xa5 );
    }
#endif // #if ROCKETNET_DEV

    struct alignas( alignof( rocketnet_server_transport_config_t ) ) server_revision_1_t
    {
        uint8_t bytes[ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1];
        uint8_t guard[16];
    } server;
    memset( &server, 0x5a, sizeof( server ) );
    check( rocketnet_server_transport_default_config_sized(
               (rocketnet_server_transport_config_t *) server.bytes, sizeof( server.bytes ) ) == 1 );
    for ( size_t i = 0; i < sizeof( server.guard ); i++ )
    {
        check( server.guard[i] == 0x5a );
    }

    rocketnet_server_transport_config_t server_current;
    rocketnet_server_transport_default_config_current( &server_current );
    server_current.max_clients = 1;
    server_current.protocol_id = 1;
    memset( server_current.backend_public_key, 0xAB, 32 );
    memset( server_current.server_public_key, 0xCD, 32 );
    memset( server_current.server_secret_key, 0xEF, 32 );
    snprintf( server_current.transport, sizeof( server_current.transport ), "%s", "loopback" );
    server_current.loopback_hub = rocketnet_test_loopback_hub();
    memcpy( server.bytes, &server_current, sizeof( server.bytes ) );
    rocketnet_server_transport_t * server_transport = rocketnet_server_transport_create_sized(
        NULL, "127.0.0.1:0", (const rocketnet_server_transport_config_t *) server.bytes,
        sizeof( server.bytes ) );
    check( server_transport == NULL );

    for ( size_t i = 0; i < sizeof( server.guard ); i++ )
    {
        check( server.guard[i] == 0x5a );
    }

#if !defined( _WIN32 )
    // The guard bytes above prove a later field's value is not used and not
    // written, not that it is never read. Here each prefix ends exactly at a
    // page that can be neither read nor written, so a read one byte past the
    // revision faults at once, in every build but Windows, which keeps only
    // the guard bytes.

    struct revision_tail_t client_tail;
    check( revision_tail_open( &client_tail, sizeof( client.bytes ), alignof( rocketnet_client_transport_config_t ) ) );
    check( rocketnet_client_transport_default_config_sized( (rocketnet_client_transport_config_t *) client_tail.bytes, sizeof( client.bytes ) ) == 1 );
    memcpy( client_tail.bytes, &client_current, sizeof( client.bytes ) );
#if ROCKETNET_DEV
    check( rocketnet_client_transport_create_sized( NULL, token, (const rocketnet_client_transport_config_t *) client_tail.bytes,
                                                    sizeof( client.bytes ), 1000.0 ) == NULL );
#endif // #if ROCKETNET_DEV
    revision_tail_close( &client_tail );

    struct revision_tail_t server_tail;
    check( revision_tail_open( &server_tail, sizeof( server.bytes ), alignof( rocketnet_server_transport_config_t ) ) );
    check( rocketnet_server_transport_default_config_sized( (rocketnet_server_transport_config_t *) server_tail.bytes, sizeof( server.bytes ) ) == 1 );
    memcpy( server_tail.bytes, &server_current, sizeof( server.bytes ) );
    check( rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", (const rocketnet_server_transport_config_t *) server_tail.bytes,
                                                    sizeof( server.bytes ) ) == NULL );
    revision_tail_close( &server_tail );
#endif // #if !defined( _WIN32 )
}

REGISTER_UNIT_TEST( test_public_link_simulator_revision_1_prefixes );

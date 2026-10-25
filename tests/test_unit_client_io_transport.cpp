/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport's wiring of the I/O core (sdk/rocketnet_client_io.h)
// against the fake client transport (tests/fake_client_driver.h): a transport with
// wait and wake runs the I/O thread design of docs/CLIENT-IO.md, any other
// keeps the BASE path. The transport is created step-driven: no I/O thread,
// no socket, no real time; the test calls rocketnet_client_io_step in place
// of the thread, and the clock is a test clock from 1000.0 (RNClientIo
// HandOff, Defer, Reopen, Expose, Return, Stop, IoSend, IoExit).

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_config.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

struct client_io_transport_clock_t
{
    double now;
    int calls;
};

static double client_io_transport_time( void * context )
{
    client_io_transport_clock_t * clock = (client_io_transport_clock_t *) context;
    clock->calls++;
    return clock->now;
}

// a connect token naming 127.0.0.1:40000; the client reads the signed fields
// at connect, so the signed data is enough to create and send requests
static void client_io_transport_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static rocketnet_client_transport_t * client_io_transport_create( rocketnet_fake_client_transport_io_t * fake,
                                                                  const rocketnet_client_transport_io_operations_t * operations, client_io_transport_clock_t * clock )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_io_transport_token( token );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    check( config.time_callback == NULL );
    config.time_callback = client_io_transport_time;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( clock, token, &config, clock->now, operations, fake );
    check( transport != NULL );
    return transport;
}

static uint64_t client_io_transport_counter( rocketnet_client_transport_t * transport, int counter )
{
    return rocketnet_client_transport_counters( transport )->values[counter];
}

extern void rocketnet_client_transport_legacy_receive_state_for_test(
    const rocketnet_client_transport_t * transport, int * fill, int * ready_count, int * free_count );

static uint64_t client_io_transport_packets_received( rocketnet_client_transport_t * transport )
{
    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( transport, &stats );
    return stats.packets_received;
}

// BASE publishes up to N-1 batches and retains its producer buffer. Update
// drains all READY batches that fit, and releases them in the same call.
extern void rocketnet_client_transport_legacy_busy_for_test( rocketnet_client_transport_t *, unsigned );
// Supply an already-completed batch through the bounded fake queue. The
// handoff buffer holds 1024 packets, but the generic fake queue holds only
// 256: replenishing one packet per receive keeps this fixture within both
// capacities even when assertions are enabled.
struct client_io_completed_batch_t : rocketnet_fake_client_transport_io_t
{
    int remaining;
    rocketnet_address_t source;
};

static int client_io_completed_batch_receive( void * context, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    auto * fake = static_cast<client_io_completed_batch_t *>( static_cast<rocketnet_fake_client_transport_io_t *>( context ) );
    if ( fake->inbound_count == 0 && fake->remaining > 0 )
    {
        const uint8_t datagram[64] = {};
        rocketnet_fake_client_push( fake, &fake->source, datagram, sizeof( datagram ) );
        --fake->remaining;
    }
    return rocketnet_fake_client_receive( fake, from, data, capacity );
}

static void test_client_legacy_receive_handoff_and_backpressure()
{
    auto * fake = new client_io_completed_batch_t();
    auto operations = rocketnet_fake_client_operations_base;
    operations.receive = client_io_completed_batch_receive;
    client_io_transport_clock_t clock = { 1000.0, 0 };
    auto * transport = client_io_transport_create( fake, &operations, &clock );
    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );
    uint8_t datagram[64] = {};
    int fill, ready, free_count;
    for ( int i = 0; i < 2; ++i )
    {
        rocketnet_fake_client_push( fake, &server, datagram, sizeof( datagram ) );
        rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    }
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( fill >= 0 && ready == 2 && free_count == 0 );
    rocketnet_client_transport_legacy_busy_for_test( transport, 1 );
    rocketnet_client_transport_update( transport, 1000.0 );
    check( client_io_transport_packets_received( transport ) == 0 );
    rocketnet_client_transport_legacy_busy_for_test( transport, 2 );
    rocketnet_client_transport_update( transport, 1000.1 );
    check( client_io_transport_packets_received( transport ) == 2 );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( ready == 0 && free_count == 0 ); // returned buffers remain caller-owned
    rocketnet_client_transport_legacy_busy_for_test( transport, 1 );
    rocketnet_client_transport_update( transport, 1000.2 );
    check( client_io_transport_packets_received( transport ) == 2 );
    rocketnet_client_transport_legacy_busy_for_test( transport, 0 );
    rocketnet_client_transport_update( transport, 1000.3 );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( ready == 0 && free_count == 2 );
    // Fill all three buffers; the producer must retain its full buffer and
    // leave further datagrams unread. Each update has only 1024 packet slots.
    for ( int batch = 0; batch < 3; ++batch )
    {
        fake->source = server;
        fake->remaining = ROCKETNET_NUM_CLIENT_PACKETS;
        rocketnet_client_transport_step_legacy_for_test( transport, 1000.4 );
        check( fake->remaining == 0 && fake->inbound_count == 0 );
    }
    rocketnet_fake_client_push( fake, &server, datagram, sizeof( datagram ) );
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.4 );
    check( fake->inbound_count == 1 );
    rocketnet_client_transport_update( transport, 1000.4 );
    check( client_io_transport_packets_received( transport ) == 2 + ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( ready == 1 && free_count == 1 );
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.5 );
    check( fake->inbound_count == 0 );
    rocketnet_client_transport_update( transport, 1000.5 );
    rocketnet_client_transport_update( transport, 1000.6 );
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.7 );
    rocketnet_client_transport_update( transport, 1000.7 );
    check( client_io_transport_packets_received( transport ) == 3 + 3 * ROCKETNET_NUM_CLIENT_PACKETS );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( ready == 0 && free_count == 2 );
    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->destroy_calls == 1 );
    delete fake;
}
REGISTER_UNIT_TEST( test_client_legacy_receive_handoff_and_backpressure );

// The helper: false for the base table, true for the io and batch tables. A
// step-driven transport on the base table has no I/O core and sends the
// protocol packet from update on the main thread, as at BASE.
static void test_client_io_backend_without_wait_uses_base_path()
{
    check( !rocketnet_client_transport_uses_io_thread( &rocketnet_fake_client_operations_base ) );
    check( !rocketnet_client_transport_uses_io_thread( NULL ) );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_base, &clock );

    check( rocketnet_client_transport_client_io( transport ) == NULL );

    rocketnet_client_transport_update( transport, 1000.0 );
    check( fake->send_calls == 1 );
    check( fake->num_sent == 1 );
    check( fake->wake_calls == 0 );
    check( fake->wait_calls == 0 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->destroy_calls == 1 );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_backend_without_wait_uses_base_path );

// The helper: true for the io table (wait and wake, per-packet send). A
// step-driven transport on it makes no transport send on the main thread: update
// seals its protocol packet into the Filling ring and hands it off (one wake),
// and the I/O step sends it to the server address. Destroy stops and exits the
// core, then closes the transport once.
static void test_client_io_backend_with_wait_uses_io_thread()
{
    check( rocketnet_client_transport_uses_io_thread( &rocketnet_fake_client_operations_io ) );
    check( rocketnet_client_transport_uses_io_thread( &rocketnet_fake_client_operations_batch ) );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_io, &clock );

    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    check( io != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );
    check( fake->send_calls == 0 );
    check( fake->num_sent == 0 );
    check( fake->wake_calls == 1 );
    check( io->send.ready_count == 1 && io->send_fill == 1 );

    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->send_calls == 1 );
    check( fake->num_sent == 1 );
    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );
    check( rocketnet_address_equal( &fake->sent[0].address, &server ) );
    check( io->send.ready_count == 0 && io->send_ring == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->destroy_calls == 1 );
    check( fake->num_sent == 1 );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_backend_with_wait_uses_io_thread );

// docs/CLIENT-IO.md test_client_io_handoff_never_waits, on the protocol packet
// update builds (send_packet needs a connected session, which needs a server):
// three updates with no I/O step between them all return; the first two hand
// off rings 0 and 1, and the third finds both still queued (MaxReady 2) and is
// deferred (CLIENT_SEND_DEFERRED 1, nothing sent). Two steps send rings 0 and
// 1; the next update hands off the deferred packet with its own in ring 2, and
// the step after that sends both. RNBufferHandoff MainNeverWaits. The stats
// count each packet once, the deferred one included: four sent, not five.
static void test_client_io_handoff_never_waits()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_batch, &clock );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    check( io != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1000.2 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 0 );
    check( io->send_fill == 2 && io->send.ready_count == 2 );

    rocketnet_client_transport_update( transport, 1000.4 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( io->send_fill == 2 && io->send.ready_count == 2 );
    check( fake->num_sent == 0 );

    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->num_sent == 1 );
    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->num_sent == 2 );
    check( io->send_ring == 1 && io->send.ready_count == 0 );

    rocketnet_client_transport_update( transport, 1000.6 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( io->send_fill == 0 && io->send.ready_count == 1 );
    check( io->committed[2] == 2 );

    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->num_sent == 4 );
    check( fake->send_calls == 0 );
    check( fake->send_batch_calls == 3 );

    // inside the protocol's send interval: no packet, nothing to hand off
    rocketnet_client_transport_update( transport, 1000.65 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( transport, &stats );
    check( stats.packets_sent_per_second == 4 );

    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->num_sent == 4 );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_handoff_never_waits );

// Stop with a deferred hand-off: the third update's packet waits in the
// Filling ring behind rings 0 and 1, both queued. Stop hands the Filling ring
// to the core with the quit store; the core sends rings 0 and 1, then it,
// then exits; nothing committed is dropped (RNBufferHandoff StopSendsAll,
// ExitSentAll; RNClientIo FreedOnlyAfterJoin).
static void test_client_io_destroy_hands_off_deferred_slots()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_io, &clock );

    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1001.0 );
    rocketnet_client_transport_update( transport, 1002.0 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( fake->num_sent == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->num_sent == 3 );
    check( fake->destroy_calls == 1 );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_destroy_hands_off_deferred_slots );

// The receive path through update: a datagram from the server, drained and
// published by the step (stamped by the config's time_callback), is exposed by
// the next update and reaches the protocol (PACKETS_RECEIVED moves by one);
// update returns the buffer at its end (Expose, Return). An update with
// nothing Ready processes nothing.
static void test_client_io_update_exposes_published_batch()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_io, &clock );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    check( io != NULL );

    const uint64_t received_before = client_io_transport_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED );

    rocketnet_client_transport_update( transport, 1000.0 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED ) == received_before );

    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );
    uint8_t datagram[64];
    memset( datagram, 0, sizeof( datagram ) );
    rocketnet_fake_client_push( fake, &server, datagram, (int) sizeof( datagram ) );

    const int clock_calls = clock.calls;
    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( clock.calls > clock_calls );
    check( io->receive.ready_count == 1 && io->receive_fill == 1 );
    check( io->buffer[0]->receive_time[0] == 1000.0 );

    clock.now = 1000.5;
    rocketnet_client_transport_update( transport, 1000.5 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED ) == received_before + 1 );
    check( io->receive_held == -1 && io->receive.ready_count == 0 && io->receive.free_count == 2 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_update_exposes_published_batch );

// Stop must not run backend sends, including packets waiting in the link
// simulator. The fake worker remains paused until explicitly stepped below.
static void test_client_simulator_stop_defers_backend_send()
{
    rocketnet_public_link_simulator_test_development( 1 );
    auto * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_io_transport_token( token );
    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_io_transport_time;
    config.simulated_latency_ms = 500;
    auto * transport = rocketnet_client_transport_create_step_driven_current( &clock, token, &config,
                                                                              clock.now, &rocketnet_fake_client_operations_base, fake );
    check( transport );
    rocketnet_client_transport_update( transport, clock.now );
    check( fake->send_calls == 0 ); // cookie request still held by latency
    rocketnet_client_transport_stop( transport );
    check( fake->send_calls == 0 );
    check( !rocketnet_client_transport_network_simulator( transport, 0, 0, 100, 0 ) );
    for ( int i = 0; i < 16; ++i )
    {
        rocketnet_client_transport_stop( transport );
        check( !rocketnet_client_transport_stopped( transport ) );
        check( fake->send_calls == 0 && fake->destroy_calls == 0 );
    }
    rocketnet_client_transport_step_legacy_for_test( transport, clock.now );
    check( rocketnet_client_transport_stopped( transport ) );
    check( fake->send_calls == 1 );
    rocketnet_client_transport_step_legacy_for_test( transport, clock.now );
    check( fake->send_calls == 1 );
    rocketnet_client_transport_destroy( transport );
    check( fake->destroy_calls == 1 );
    delete fake;
    rocketnet_public_link_simulator_test_development( 0 );
}
REGISTER_UNIT_TEST( test_client_simulator_stop_defers_backend_send );

// Busy expose/return, repeated contention and exact packet recovery are
// covered above by test_client_legacy_receive_handoff_and_backpressure with
// the unified handoff's permanently producer-owned buffer.

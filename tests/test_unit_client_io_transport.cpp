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
    memset( &signed_fields, 0, sizeof(signed_fields) );
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

// Step-driven BASE receive uses the same handoff as its worker, without a
// scheduler. Published batches that fit the update's packet budget are
// processed oldest first and returned FREE in the same update; when all
// three buffers are READY, another datagram stays unread in the transport
// until update returns a buffer.
static void test_client_legacy_receive_handoff_and_backpressure()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_base, &clock );
    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );
    uint8_t datagram[64] = {};
    int fill = -2, ready = -1, free_count = -1;

    datagram[0] = 0xA1;
    rocketnet_fake_client_push( fake, &server, datagram, sizeof(datagram) );
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( fill == -1 && ready == 1 && free_count == 2 );

    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( client_io_transport_packets_received( transport ) == 1 );
    check( fill == -1 && ready == 0 && free_count == 3 );

    datagram[0] = 0xB2;
    rocketnet_fake_client_push( fake, &server, datagram, sizeof(datagram) );
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.1 );
    datagram[0] = 0xC3;
    rocketnet_fake_client_push( fake, &server, datagram, sizeof(datagram) );
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.15 );
    datagram[0] = 0xD4;
    rocketnet_fake_client_push( fake, &server, datagram, sizeof(datagram) );
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.2 );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( fill == -1 && ready == 3 && free_count == 0 );

    datagram[0] = 0xE5;
    rocketnet_fake_client_push( fake, &server, datagram, sizeof(datagram) );
    const int calls_before = fake->receive_calls;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.25 );
    check( fake->receive_calls == calls_before && fake->inbound_count == 1 );

    rocketnet_client_transport_update( transport, 1000.3 );
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill, &ready, &free_count );
    check( client_io_transport_packets_received( transport ) == 4 );
    check( fill == -1 && ready == 0 && free_count == 3 );

    rocketnet_client_transport_step_legacy_for_test( transport, 1000.35 );
    check( fake->inbound_count == 0 );
    rocketnet_client_transport_update( transport, 1000.4 );
    check( client_io_transport_packets_received( transport ) == 5 );

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
    check( io->rings.state[0].load() == ROCKETNET_CLIENT_RING_HANDED_OFF );

    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->send_calls == 1 );
    check( fake->num_sent == 1 );
    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );
    check( rocketnet_address_equal( &fake->sent[0].address, &server ) );
    check( io->rings.state[0].load() == ROCKETNET_CLIENT_RING_SENT );

    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->destroy_calls == 1 );
    check( fake->num_sent == 1 );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_backend_with_wait_uses_io_thread );

// docs/CLIENT-IO.md test_client_io_handoff_never_waits, on the protocol packet
// update builds (send_packet needs a connected session, which needs a server):
// two updates with no I/O step between both return; the second hand-off finds
// ring 0 still HandedOff and is deferred (CLIENT_SEND_DEFERRED 1, nothing
// sent). After one step sends ring 0, the next update reopens it and hands off
// the deferred packet with its own, and the step after that sends both. The
// stall of MCRNClientIoBrokenHandOffWaits.cfg (HandOffNeverWaits). The stats
// count each packet once, the deferred one included: three sent, not four.
static void test_client_io_handoff_never_waits()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_batch, &clock );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    check( io != NULL );

    rocketnet_client_transport_update( transport, 1000.0 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 0 );
    check( io->rings.fill == 1 );

    rocketnet_client_transport_update( transport, 1000.2 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( io->rings.fill == 1 );
    check( fake->num_sent == 0 );
    check( io->rings.state[0].load() == ROCKETNET_CLIENT_RING_HANDED_OFF );

    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->num_sent == 1 );
    check( io->rings.state[0].load() == ROCKETNET_CLIENT_RING_SENT );

    rocketnet_client_transport_update( transport, 1000.4 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( io->rings.fill == 0 );
    check( io->rings.state[1].load() == ROCKETNET_CLIENT_RING_HANDED_OFF );
    check( io->rings.committed[1] == 2 );

    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->num_sent == 3 );
    check( fake->send_calls == 0 );
    check( fake->send_batch_calls == 2 );

    // inside the protocol's send interval: no packet, nothing to hand off
    rocketnet_client_transport_update( transport, 1000.45 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( transport, &stats );
    check( stats.packets_sent_per_second == 3 );

    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->num_sent == 3 );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_handoff_never_waits );

// Destroy with a deferred hand-off: the second update's packet waits in the
// Filling ring behind ring 0, still HandedOff. Destroy runs the step until
// ring 0 is Sent, hands the Filling ring off, stops, and the core sends it
// before it exits; nothing committed is dropped (CommittedEventuallyHandedOff,
// FreedOnlyAfterJoin).
static void test_client_io_destroy_hands_off_deferred_slots()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_io_transport_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_create( fake, &rocketnet_fake_client_operations_io, &clock );

    rocketnet_client_transport_update( transport, 1000.0 );
    rocketnet_client_transport_update( transport, 1001.0 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( fake->num_sent == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    check( fake->num_sent == 2 );
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
    memset( datagram, 0, sizeof(datagram) );
    rocketnet_fake_client_push( fake, &server, datagram, (int) sizeof(datagram) );

    const int clock_calls = clock.calls;
    check( rocketnet_client_io_step( io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( clock.calls > clock_calls );
    check( io->receive.state[0].load() == ROCKETNET_CLIENT_BUFFER_READY );
    check( io->buffer[0]->receive_time[0] == 1000.0 );

    clock.now = 1000.5;
    rocketnet_client_transport_update( transport, 1000.5 );
    check( client_io_transport_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED ) == received_before + 1 );
    check( io->receive.state[0].load() == ROCKETNET_CLIENT_BUFFER_FREE );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_io_update_exposes_published_batch );

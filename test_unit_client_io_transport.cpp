/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client I/O core wired into the client transport
// (sdk/rocketnet_client_transport.cpp, docs/CLIENT-IO.md, tla/RNClientIo.tla).
// The path choice by the driver's operations, and, through the step-driven
// unit mode of sdk/rocketnet_client_io.h, the transport's hand-off, deferral,
// receive handoff and destroy order against the fake client driver
// (test/fake_client_driver.h). No socket, no thread, no real time: the
// transport's clock is the test's, from 1000.0, and the I/O step is called
// on the test's thread in place of the I/O thread.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "rocketnet.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"

#include <string.h>

#define check rocketnet_unit_check

// The path choice (docs/CLIENT-IO.md _The interface_): a backend without
// wait and wake keeps the BASE path.
static void client_io_backend_without_wait_uses_base_path()
{
    check( !rocketnet_client_transport_uses_io_thread( &rocketnet_fake_client_operations_base ) );
    check( !rocketnet_client_transport_uses_io_thread( NULL ) );
}

REGISTER_UNIT_TEST( client_io_backend_without_wait_uses_base_path );

// A backend with wait and wake runs the I/O thread design, with or without
// the batch callbacks.
static void client_io_backend_with_wait_uses_io_thread()
{
    check( rocketnet_client_transport_uses_io_thread( &rocketnet_fake_client_operations_io ) );
    check( rocketnet_client_transport_uses_io_thread( &rocketnet_fake_client_operations_batch ) );
}

REGISTER_UNIT_TEST( client_io_backend_with_wait_uses_io_thread );

// ---------------------------------------------------------------------------
// the step-driven transport

struct client_io_transport_test_clock_t
{
    double now;
    int calls;
};

static double client_io_transport_test_time( void * context )
{
    client_io_transport_test_clock_t * clock = (client_io_transport_test_clock_t *) context;
    clock->calls++;
    return clock->now;
}

// A connect token the client accepts at connect: the client reads the signed
// fields directly (no signature check until the server), and the expiry is
// far past any wall clock.
static void client_io_transport_test_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_VERSION;
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

// Create a transport on the fake io backend in step mode; the clock is the
// transport's context and its time_callback.
static rocketnet_client_transport_t * client_io_transport_test_create( rocketnet_fake_client_driver_t * fake,
    client_io_transport_test_clock_t * clock )
{
    const rocketnet_client_transport_test_driver_t seam = { &rocketnet_fake_client_operations_io, fake };
    rocketnet_client_transport_set_test_driver( &seam );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_io_transport_test_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    check( config.time_callback == NULL );
    config.time_callback = client_io_transport_test_time;

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create( clock, token, &config, clock->now );
    rocketnet_client_transport_set_test_driver( NULL );
    check( transport != NULL );
    return transport;
}

static uint64_t client_io_transport_test_counter( rocketnet_client_transport_t * transport, int counter )
{
    return rocketnet_client_transport_counters( transport )->values[counter];
}

// docs/CLIENT-IO.md client_io_handoff_never_waits, through the transport
// (RNClientIo HandOff, Defer, Reopen; HandOffNeverWaits,
// CommittedEventuallyHandedOff). Two updates with no I/O step between both
// return: the first hands its protocol packet off, the second finds that ring
// still HandedOff and defers (CLIENT_SEND_DEFERRED is 1), and no driver send
// runs on the main thread. One I/O step sends the first ring; the next update
// reopens it and hands the deferred packet off, and the step after that sends
// it. The packet each update sends here is the connection request: before a
// session exists send_packet is refused by the protocol, so the deferred
// packet is update's own (the first live send_packet exercise is the Linux
// card's functional client_io_linux_handoff_round_trip).
static void client_io_handoff_never_waits()
{
    rocketnet_fake_client_driver_t * fake = new rocketnet_fake_client_driver_t();
    client_io_transport_test_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_test_create( fake, &clock );
    if ( !transport )
    {
        delete fake;
        return;
    }

    clock.now = 1000.5;
    rocketnet_client_transport_update( transport, clock.now );
    check( fake->wake_calls == 1 );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 0 );

    clock.now = 1001.0;
    rocketnet_client_transport_update( transport, clock.now );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( fake->wake_calls == 1 );
    check( fake->send_calls == 0 );
    check( fake->num_sent == 0 );

    // the I/O thread's step: the first ring leaves
    check( rocketnet_client_transport_io_step( transport, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->num_sent == 1 );

    // the next update reopens the sent ring and hands the deferred packet off
    // with its own
    clock.now = 1001.5;
    rocketnet_client_transport_update( transport, clock.now );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( fake->wake_calls == 2 );
    check( fake->num_sent == 1 );

    check( rocketnet_client_transport_io_step( transport, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( fake->num_sent == 3 );
    for ( int i = 0; i < fake->num_sent; i++ )
    {
        check( fake->sent[i].bytes > 0 );
        check( rocketnet_address_equal( &fake->sent[i].address, &fake->sent[0].address ) );
    }
    check( fake->send_calls == 3 );

    rocketnet_client_transport_destroy( transport );
    check( fake->destroy_calls == 1 );
    check( fake->num_sent == 3 );
    delete fake;
}

REGISTER_UNIT_TEST( client_io_handoff_never_waits );

// destroy makes the last hand-off itself (docs/CLIENT-IO.md _Stop and destroy
// order_; RNClientIo CommittedEventuallyHandedOff, Stop, Join,
// FreedOnlyAfterJoin): a packet deferred behind a ring still HandedOff is
// sent before the driver closes.
static void client_io_destroy_hands_off_deferred_slots()
{
    rocketnet_fake_client_driver_t * fake = new rocketnet_fake_client_driver_t();
    client_io_transport_test_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_test_create( fake, &clock );
    if ( !transport )
    {
        delete fake;
        return;
    }

    clock.now = 1000.5;
    rocketnet_client_transport_update( transport, clock.now );
    clock.now = 1001.0;
    rocketnet_client_transport_update( transport, clock.now );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == 1 );
    check( fake->num_sent == 0 );

    rocketnet_client_transport_destroy( transport );
    check( fake->num_sent == 2 );
    check( fake->destroy_calls == 1 );
    delete fake;
}

REGISTER_UNIT_TEST( client_io_destroy_hands_off_deferred_slots );

// The receive handoff through update (docs/CLIENT-IO.md _The receive path_;
// RNClientIo IoReceive, IoPublish, Expose, Return; NoReceiveStall): a batch
// the I/O step publishes is processed by the next update, update returns the
// buffer at its end every update, and a batch read after that is published
// into the returned buffer and reaches the update after. An update with no
// Ready buffer processes nothing. The datagrams come from the address the
// transport sends to; the receive stamps come from time_callback.
static void client_io_update_returns_buffer_every_update()
{
    rocketnet_fake_client_driver_t * fake = new rocketnet_fake_client_driver_t();
    client_io_transport_test_clock_t clock = { 1000.0, 0 };
    rocketnet_client_transport_t * transport = client_io_transport_test_create( fake, &clock );
    if ( !transport )
    {
        delete fake;
        return;
    }

    // one update and one step learn the server address from the request sent
    clock.now = 1000.5;
    rocketnet_client_transport_update( transport, clock.now );
    rocketnet_client_transport_io_step( transport, false );
    check( fake->num_sent == 1 );
    const rocketnet_address_t server = fake->sent[0].address;

    uint8_t junk[100];
    memset( junk, 0, sizeof(junk) );

    const uint64_t received_before = client_io_transport_test_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED );

    // an update with nothing Ready processes nothing
    clock.now = 1000.6;
    rocketnet_client_transport_update( transport, clock.now );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED ) == received_before );

    rocketnet_fake_client_push( fake, &server, junk, (int) sizeof(junk) );
    rocketnet_fake_client_push( fake, &server, junk, (int) sizeof(junk) );
    const int clock_calls = clock.calls;
    check( rocketnet_client_transport_io_step( transport, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( clock.calls > clock_calls );

    clock.now = 1000.7;
    rocketnet_client_transport_update( transport, clock.now );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED ) == received_before + 2 );

    // the second batch goes into the buffer update returned
    rocketnet_fake_client_push( fake, &server, junk, (int) sizeof(junk) );
    check( rocketnet_client_transport_io_step( transport, false ) == ROCKETNET_CLIENT_IO_WORKED );

    clock.now = 1000.8;
    rocketnet_client_transport_update( transport, clock.now );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED ) == received_before + 3 );

    // and a third into the first buffer again
    rocketnet_fake_client_push( fake, &server, junk, (int) sizeof(junk) );
    check( rocketnet_client_transport_io_step( transport, false ) == ROCKETNET_CLIENT_IO_WORKED );

    clock.now = 1000.9;
    rocketnet_client_transport_update( transport, clock.now );
    check( client_io_transport_test_counter( transport, ROCKETNET_COUNTER_PACKETS_RECEIVED ) == received_before + 4 );

    rocketnet_client_transport_destroy( transport );
    check( fake->destroy_calls == 1 );
    delete fake;
}

REGISTER_UNIT_TEST( client_io_update_returns_buffer_every_update );

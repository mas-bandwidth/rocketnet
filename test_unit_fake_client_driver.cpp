/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The in-memory fake client transport (test/fake_client_driver.h) pins the I/O
// step's transport contract (docs/CLIENT-IO.md "The I/O step"): each table opens
// through rocketnet_client_transport_io_open, wake is level-triggered and cleared by
// wait, and receive_batch drains in order with per-packet stride layout.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"
#include "test/portable_crt.h"

#include "rocketnet_address.h"

// Each table opens through rocketnet_client_transport_io_open with the fake as the
// context and closes with destroy counted once.
static void test_fake_client_transport_io_opens_with_all_eight()
{
    const rocketnet_client_transport_io_operations_t * tables[] =
    {
        &rocketnet_fake_client_operations_base,
        &rocketnet_fake_client_operations_io,
        &rocketnet_fake_client_operations_batch
    };
    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_fake_client_transport_io_t fake = {};
        rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
        rocketnet_address_t bind = {};
        rocketnet_client_transport_io_t transport = {};
        const bool opened = rocketnet_client_transport_io_open(
            tables[i],
            &fake, &config, &bind, &transport );
        rocketnet_unit_check( opened );
        rocketnet_unit_check( transport.instance == &fake );
        rocketnet_client_transport_io_close( &transport );
        rocketnet_unit_check( fake.destroy_calls == 1 );
    }
}

// wait is false with nothing; wake makes the next wait true; the following
// wait is false again; a pushed datagram makes wait true without consuming it.
static void test_fake_client_wake_is_level_triggered()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_unit_check( !rocketnet_fake_client_operations_io.wait( &fake, true, 1.0 ) );
    rocketnet_unit_check( fake.wait_calls == 1 );
    rocketnet_unit_check( fake.last_wait_timeout == 1.0 );

    rocketnet_fake_client_operations_io.wake( &fake );
    rocketnet_unit_check( fake.wake_calls == 1 );
    rocketnet_unit_check( rocketnet_fake_client_operations_io.wait( &fake, true, 0.5 ) );
    rocketnet_unit_check( !rocketnet_fake_client_operations_io.wait( &fake, true, 0.25 ) );

    uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_address_t from = {};
    from.port = 99;
    rocketnet_fake_client_push( &fake, &from, data, 4 );
    rocketnet_unit_check( rocketnet_fake_client_operations_io.wait( &fake, true, 0.0 ) );
    rocketnet_unit_check( fake.inbound_count == 1 );
}

// Three pushed datagrams of 10, 20 and 30 bytes from distinct ports drain by
// receive_batch in order with stride 2048: 2, then 1, then 0, addresses and
// bytes matching.
static void test_fake_client_receive_batch_layout()
{
    rocketnet_fake_client_transport_io_t fake = {};
    uint8_t d1[10] = {}, d2[20] = {}, d3[30] = {};
    rocketnet_address_t a1 = {}, a2 = {}, a3 = {};
    rocketnet_address_load_ipv4( &a1, 0x7f000001, 101 );
    rocketnet_address_load_ipv4( &a2, 0x7f000001, 202 );
    rocketnet_address_load_ipv4( &a3, 0x7f000001, 303 );
    rocketnet_fake_client_push( &fake, &a1, d1, 10 );
    rocketnet_fake_client_push( &fake, &a2, d2, 20 );
    rocketnet_fake_client_push( &fake, &a3, d3, 30 );

    uint8_t storage[2][2048];
    rocketnet_address_t from[2];
    int bytes[2];

    int got = rocketnet_fake_client_operations_batch.receive_batch( &fake, from, (uint8_t*) storage, 2048, bytes, 2 );
    rocketnet_unit_check( got == 2 );
    rocketnet_unit_check( bytes[0] == 10 && bytes[1] == 20 );
    rocketnet_unit_check( rocketnet_address_equal( &from[0], &a1 ) );
    rocketnet_unit_check( rocketnet_address_equal( &from[1], &a2 ) );

    got = rocketnet_fake_client_operations_batch.receive_batch( &fake, from, (uint8_t*) storage, 2048, bytes, 2 );
    rocketnet_unit_check( got == 1 );
    rocketnet_unit_check( bytes[0] == 30 );
    rocketnet_unit_check( rocketnet_address_equal( &from[0], &a3 ) );

    got = rocketnet_fake_client_operations_batch.receive_batch( &fake, from, (uint8_t*) storage, 2048, bytes, 2 );
    rocketnet_unit_check( got == 0 );
}

// wait honors readable: a datagram counts only when readable is true, and a
// wake returns regardless of readable and is consumed by the next wait.
static void test_fake_client_wait_honors_readable()
{
    rocketnet_fake_client_transport_io_t fake = {};
    uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_address_t from = {};
    from.port = 99;
    rocketnet_fake_client_push( &fake, &from, data, 4 );

    rocketnet_unit_check( !rocketnet_fake_client_operations_io.wait( &fake, false, 0.0 ) );
    rocketnet_unit_check( !fake.last_wait_readable );
    rocketnet_unit_check( fake.inbound_count == 1 );

    rocketnet_unit_check( rocketnet_fake_client_operations_io.wait( &fake, true, 0.0 ) );

    rocketnet_fake_client_operations_io.wake( &fake );
    rocketnet_unit_check( rocketnet_fake_client_operations_io.wait( &fake, false, 0.0 ) );
    rocketnet_unit_check( !rocketnet_fake_client_operations_io.wait( &fake, false, 0.0 ) );
}

// The io table opens with wait and wake set and batch null; the batch table
// opens with the batch callbacks set.
static void test_fake_client_io_table_opens_with_wait_and_wake()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open(
        &rocketnet_fake_client_operations_io,
        &fake, &config, &bind, &transport ) );
    rocketnet_unit_check( transport.operations->wait == rocketnet_fake_client_wait );
    rocketnet_unit_check( transport.operations->wake == rocketnet_fake_client_wake );
    rocketnet_unit_check( transport.operations->send_batch == NULL );
    rocketnet_unit_check( transport.operations->receive_batch == NULL );

    transport.operations->wait( transport.instance, true, 0.5 );
    rocketnet_unit_check( fake.last_wait_timeout == 0.5 );
    rocketnet_unit_check( fake.last_wait_readable );
    rocketnet_client_transport_io_close( &transport );

    rocketnet_unit_check( rocketnet_client_transport_io_open(
        &rocketnet_fake_client_operations_batch,
        &fake, &config, &bind, &transport ) );
    rocketnet_unit_check( transport.operations->send_batch == rocketnet_fake_client_send_batch );
    rocketnet_unit_check( transport.operations->receive_batch == rocketnet_fake_client_receive_batch );
    rocketnet_client_transport_io_close( &transport );
}

REGISTER_UNIT_TEST( test_fake_client_transport_io_opens_with_all_eight );
REGISTER_UNIT_TEST( test_fake_client_wake_is_level_triggered );
REGISTER_UNIT_TEST( test_fake_client_receive_batch_layout );
REGISTER_UNIT_TEST( test_fake_client_wait_honors_readable );
REGISTER_UNIT_TEST( test_fake_client_io_table_opens_with_wait_and_wake );

// ---------------------------------------------------------------------------
// A deferred disconnect burst keeps the server address (docs/CLIENT-IO.md
// _The protocol packet and disconnect_, tla/RNClientIo.tla SentToCommitted).
//
// The client runs the I/O core on the loopback client operations with a no-op
// wait and wake added (so rocketnet_client_transport_uses_io_thread is true)
// and send wrapped to record each datagram's destination. The step-driven
// create runs no thread; the test never steps between the hand-offs, so the
// I/O side still holds a handed-off ring when disconnect commits its burst.
// No socket, no thread, no real time: the clock starts at 1000.0.

#include "sdk/rocketnet_client_io.h"
#include "sdk/rocketnet_client_protocol.h"
#include "sdk/rocketnet_connect_token.h"
#include "sdk/rocketnet_counters.h"
#include "sdk/rocketnet_driver.h"
#include "sdk/rocketnet_layers.h"
#include "sdk/rocketnet_protocol.h"

#include <stdio.h>
#include <stdlib.h>

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

static const uint64_t fake_disconnect_protocol_id = 0x4B45455041444452ULL;

struct fake_disconnect_record_t
{
    rocketnet_address_t to[ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    int count;
};

static fake_disconnect_record_t fake_disconnect_record;

static void fake_disconnect_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    rocketnet_unit_check( fake_disconnect_record.count < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    fake_disconnect_record.to[fake_disconnect_record.count++] = *to;
    rocketnet_loopback_transport.client->send( transport, to, data, bytes );
}

static bool fake_disconnect_wait( void * transport, bool readable, double timeout_seconds )
{
    (void) transport; (void) readable; (void) timeout_seconds;
    return false;
}

static void fake_disconnect_wake( void * transport )
{
    (void) transport;
}

static int fake_disconnect_setenv( const char * name, const char * value )
{
#ifdef _WIN32
    return _putenv_s( name, value );
#else
    return value[0] ? setenv( name, value, 1 ) : unsetenv( name );
#endif
}

// the environment overlays the config's transport name at create; each pin is
// cleared for the case and restored at its end
struct fake_disconnect_env_guard_t
{
    const char * names[2] = { "ROCKETNET_SERVER_TRANSPORT", "ROCKETNET_CLIENT_TRANSPORT" };
    char saved[2][256] = {};
    fake_disconnect_env_guard_t()
    {
        for ( int i = 0; i < 2; i++ )
        {
            rocketnet_test_getenv( names[i], saved[i], sizeof( saved[i] ) );
            rocketnet_unit_check( fake_disconnect_setenv( names[i], "" ) == 0 );
        }
    }
    ~fake_disconnect_env_guard_t() noexcept( false ) // check() throws under the expected-fail harness
    {
        for ( int i = 0; i < 2; i++ )
            rocketnet_unit_check( fake_disconnect_setenv( names[i], saved[i] ) == 0 );
    }
};

static void test_fake_client_disconnect_burst_keeps_server_address()
{
    fake_disconnect_env_guard_t env_guard;
    rocketnet_quiet( 1 );
    fake_disconnect_record.count = 0;

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES], backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES], server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5A ^ i );
    rocketnet_unit_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );

    double time = 1000.0;

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config( &server_config );
    server_config.protocol_id = fake_disconnect_protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    int status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, time, &status );
    rocketnet_unit_check( server != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_server_transport_set_test_timestamp( server, 1000 );

    char server_string[64];
    snprintf( server_string, sizeof( server_string ), "127.0.0.1:%u", (unsigned) rocketnet_server_transport_port( server ) );
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, server_string ) );

    const char * addresses[] = { server_string };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret, server_public,
        fake_disconnect_protocol_id, addresses, 1, 0xF00DULL, 1000, 1030, 10, NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    rocketnet_client_transport_io_operations_t operations = *rocketnet_loopback_transport.client;
    operations.send = fake_disconnect_send;
    operations.wait = fake_disconnect_wait;
    operations.wake = fake_disconnect_wake;
    rocketnet_unit_check( rocketnet_client_transport_uses_io_thread( &operations ) );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    rocketnet_client_transport_t * client = rocketnet_client_transport_create_step_driven_at_timestamp(
        NULL, token, &client_config, time, 1000, &operations, NULL );
    rocketnet_unit_check( client != NULL );
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( client );
    rocketnet_unit_check( io != NULL );

    for ( int i = 0; i < 200 && rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; i++ )
    {
        time += 0.01;
        rocketnet_client_transport_update( client, time );
        rocketnet_client_io_step( io, false );
        rocketnet_server_transport_update( server, time );
        rocketnet_server_transport_send_packets( server );
        rocketnet_client_io_step( io, false );
    }
    rocketnet_unit_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    while ( rocketnet_client_send_rings_in_flight( &io->rings ) )
        rocketnet_client_io_step( io, false );

    // send_packets hands ring A off; the I/O side does not step, so the next
    // send_packets is deferred (ring B holds a committed packet) and the
    // disconnect burst joins ring B, deferred again
    const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    rocketnet_client_transport_send_packet( client, payload, (int) sizeof( payload ) );
    rocketnet_client_transport_send_packets( client );
    rocketnet_unit_check( rocketnet_client_send_rings_in_flight( &io->rings ) );
    rocketnet_client_transport_send_packet( client, payload, (int) sizeof( payload ) );
    rocketnet_client_transport_send_packets( client );
    const uint64_t deferred_before = rocketnet_client_transport_counters( client )->values[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED];
    rocketnet_unit_check( deferred_before >= 1 );

    const int sent_before_disconnect = fake_disconnect_record.count;
    rocketnet_client_transport_disconnect( client );
    rocketnet_unit_check( rocketnet_client_transport_counters( client )->values[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED] == deferred_before + 1 );
    rocketnet_unit_check( fake_disconnect_record.count == sent_before_disconnect );

    // destroy without stepping: it waits ring A out, hands ring B off and
    // the core sends both before it exits
    rocketnet_client_transport_destroy( client );
    const int sent = fake_disconnect_record.count - sent_before_disconnect;
    rocketnet_unit_check( sent == 1 + ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    for ( int i = sent_before_disconnect; i < fake_disconnect_record.count; i++ )
        rocketnet_unit_check( rocketnet_address_equal( &fake_disconnect_record.to[i], &server_address ) );

    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 1000 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        time += 0.01;
        rocketnet_server_transport_update( server, time );
    }
    rocketnet_server_transport_destroy( server );
}

REGISTER_UNIT_TEST( test_fake_client_disconnect_burst_keeps_server_address );

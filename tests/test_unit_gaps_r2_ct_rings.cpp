/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport's payload ring counts past the ring, the protocol
// packet the I/O path drops on a full ring, the BASE receive's packet budget
// and the production BASE path without a link simulator
// (sdk/rocketnet_client_transport.cpp). Each client is step-driven over the
// fake client transport (tests/fake_client_driver.h): no socket, no thread;
// time is an argument starting at 1000.0. Each failing check prints the
// fault first.
//
// The ring cases allocate the transport from an allocator that fills every
// block with the 32-bit word 7, so the bytes just past a ring's info[] read
// as slots holding a 7-byte packet: a walk past the ring's 256 slots then
// sends datagrams that were never sealed, and the count shows it.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "loopback_hub.h"

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_client_io.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_config.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

extern void rocketnet_client_transport_seed_refusal_state_for_test( rocketnet_client_transport_t *, uint64_t, const uint8_t *, const uint8_t *, const uint8_t * );
extern void rocketnet_client_transport_step_legacy_for_test( rocketnet_client_transport_t * client_transport, double time );
extern void rocketnet_client_transport_legacy_receive_state_for_test( const rocketnet_client_transport_t * client_transport, int * fill, int * ready_count, int * free_count );
extern bool rocketnet_client_transport_drop_link_simulator_for_test( rocketnet_client_transport_t * client_transport );

#define R2_RING_SIZE 256
#define R2_RING_STARTS 300
#define R2_PAYLOAD_BYTES 40

// ---------------------------------------------------------------------------
// a step-driven client (the ct_rings shape of tests/test_unit_gaps_ct_memory_rings.cpp)

static double r2_rings_now = 1000.0;
static double r2_rings_time( void * ) { return r2_rings_now; }

static void r2_rings_token( uint8_t * token )
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
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static void r2_rings_server_address( rocketnet_address_t * address )
{
    memset( address, 0, sizeof( *address ) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = 127;
    address->data.ipv4[3] = 1;
    address->port = 40000;
}

static rocketnet_client_transport_t * r2_rings_client( void * context, const rocketnet_client_transport_io_operations_t * operations, void * instance )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_rings_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = r2_rings_time;

    r2_rings_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( context, token, &config, 1000.0, operations, instance );
    check( transport != NULL );
    return transport;
}

static void r2_rings_seed_connected( rocketnet_client_transport_t * transport )
{
    rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value,
                                                            rocketnet_test_salt, rocketnet_test_session_s2c_key );
    check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
}

// 256 payloads started and finished one by one, then 44 more starts the full
// ring refuses: the fill ring's count is 300, its 256 slots sealed
static void r2_rings_overfill( rocketnet_client_transport_t * transport )
{
    for ( int i = 0; i < R2_RING_SIZE; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( transport, R2_PAYLOAD_BYTES, &context );
        check( frame != NULL );
        if ( !frame )
        {
            return;
        }
        memset( frame + context.payload_index, (int) ( i & 0xFF ), R2_PAYLOAD_BYTES );
        check( rocketnet_client_transport_finish_packet( transport, &context, R2_PAYLOAD_BYTES ) > 0 );
    }

    const uint64_t full_before = rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    for ( int i = R2_RING_SIZE; i < R2_RING_STARTS; i++ )
    {
        struct rocketnet_packet_context_t context;
        check( rocketnet_client_transport_start_packet( transport, R2_PAYLOAD_BYTES, &context ) == NULL );
    }
    check( rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == full_before + ( R2_RING_STARTS - R2_RING_SIZE ) );
}

// the datagrams of the payload size: every one a sealed slot
static int r2_rings_payload_datagrams( const rocketnet_fake_client_transport_io_t * fake, int from, int * other )
{
    int payloads = 0;
    *other = 0;
    for ( int i = from; i < fake->num_sent; i++ )
    {
        if ( fake->sent[i].bytes > R2_PAYLOAD_BYTES )
        {
            payloads++;
        }
        else
        {
            ( *other )++;
        }
    }
    return payloads;
}

// ---------------------------------------------------------------------------
// the allocator that fills every block with the word 7

struct r2_word_allocator_t
{
    int live;
};

static r2_word_allocator_t r2_word_state;

static void * r2_word_alloc( void * context, size_t bytes )
{
    (void) context;
    void * base = malloc( bytes + 128 );
    if ( !base )
    {
        return NULL;
    }
    r2_word_state.live++;

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 64;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );

    const uint32_t word = 7;
    uint8_t * block = (uint8_t *) address;
    for ( size_t i = 0; i + sizeof( word ) <= bytes; i += sizeof( word ) )
    {
        memcpy( block + i, &word, sizeof( word ) );
    }

    return (void *) address;
}

static void r2_word_free( void * context, void * pointer )
{
    (void) context;
    r2_word_state.live--;
    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    free( base );
}

static void r2_word_allocator_begin()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    r2_word_state.live = 0;
    int status = -1;
    check( rocketnet_init_internal( &r2_word_state, r2_word_alloc, r2_word_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );
}

static void r2_word_allocator_end()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( r2_word_state.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7734 (the BASE flush walks a ring count past the ring)
//
// 300 starts on the BASE path leave ring 0's count at 300 with 256 slots
// sealed: the flush sends the 256 and nothing from past the ring.

static void test_gaps_r2_ct_base_flush_sends_no_slot_past_the_ring()
{
    r2_word_allocator_begin();

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rings_client( &r2_word_state, &rocketnet_fake_client_operations_base, fake );
    r2_rings_seed_connected( transport );
    check( rocketnet_client_transport_client_io( transport ) == NULL );

    r2_rings_overfill( transport );

    const int sent_before = fake->num_sent;
    rocketnet_client_transport_send_packets( transport );

    int other = 0;
    const int payloads = r2_rings_payload_datagrams( fake, sent_before, &other );
    if ( payloads != R2_RING_SIZE || other != 0 )
    {
        printf( "mutation gap hunt 2/7734: the BASE flush of a ring counted %d (256 sealed) sent %d payload datagram(s) and %d datagram(s) of a slot past the ring's 256\n",
                R2_RING_STARTS, payloads, other );
    }
    check( payloads == R2_RING_SIZE );
    check( other == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    r2_word_allocator_end();
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_base_flush_sends_no_slot_past_the_ring );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7300 (stop hands the I/O core a fill count past the ring)
//
// The same overfilled ring on the I/O core path: stop publishes it as the
// final ring, and the worker sends its 256 sealed slots and nothing past it.

static void test_gaps_r2_ct_io_stop_sends_no_slot_past_the_ring()
{
    r2_word_allocator_begin();

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rings_client( &r2_word_state, &rocketnet_fake_client_operations_io, fake );
    r2_rings_seed_connected( transport );
    check( rocketnet_client_transport_client_io( transport ) != NULL );

    r2_rings_overfill( transport );

    const int sent_before = fake->num_sent;
    rocketnet_client_transport_stop( transport );
    rn_test_client_transport_wait_stopped( transport );

    int other = 0;
    const int payloads = r2_rings_payload_datagrams( fake, sent_before, &other );
    if ( payloads != R2_RING_SIZE || other != 0 )
    {
        printf( "mutation gap hunt 2/7300: stop's final ring, counted %d with 256 sealed, sent %d payload datagram(s) and %d datagram(s) of a slot past the ring's 256\n",
                R2_RING_STARTS, payloads, other );
    }
    check( payloads == R2_RING_SIZE );
    check( other == 0 );

    rocketnet_client_transport_destroy( transport );
    delete fake;

    r2_word_allocator_end();
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_io_stop_sends_no_slot_past_the_ring );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6716 (the link simulator's hold walks a ring count past the ring)
//
// An I/O core client whose live link setting is active hands its sealed
// packets to the simulator at the hand-off. With the ring counted 300 and
// 256 sealed, the simulator takes the 256: a latency of 1 ms holds them, and
// the drain after it sends exactly those.

static void test_gaps_r2_ct_link_hold_takes_no_slot_past_the_ring()
{
    r2_word_allocator_begin();

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rings_client( &r2_word_state, &rocketnet_fake_client_operations_io, fake );
    r2_rings_seed_connected( transport );
    check( rocketnet_client_transport_client_io( transport ) != NULL );
    check( rocketnet_client_transport_network_simulator( transport, 1, 0, 0, 0 ) );

    r2_rings_overfill( transport );

    const int sent_before = fake->num_sent;
    rocketnet_client_transport_send_packets( transport );
    check( fake->num_sent == sent_before );

    rocketnet_client_transport_stop( transport );
    rn_test_client_transport_wait_stopped( transport );

    int other = 0;
    const int payloads = r2_rings_payload_datagrams( fake, sent_before, &other );
    if ( payloads != R2_RING_SIZE || other != 0 )
    {
        printf( "mutation gap hunt 2/6716: the link simulator held a ring counted %d (256 sealed) and sent %d payload datagram(s) and %d datagram(s) of a slot past the ring's 256\n",
                R2_RING_STARTS, payloads, other );
    }
    check( payloads == R2_RING_SIZE );
    check( other == 0 );

    rocketnet_client_transport_destroy( transport );
    delete fake;

    r2_word_allocator_end();
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_link_hold_takes_no_slot_past_the_ring );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7426, 2/7427, 2/7428, 2/7429 (a protocol packet dropped on a full I/O ring)
//
// On the I/O core path the protocol packet an update sends is sealed into
// the fill ring like a payload. When the ring is full the packet is dropped,
// SEND_RING_FULL counts it once, and the drop is logged at SPAM.

static int r2_ring_full_lines = 0;

static void r2_ring_full_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( level == ROCKETNET_LOG_LEVEL_SPAM && strstr( message, "client payload ring full. protocol packet dropped" ) != NULL )
    {
        r2_ring_full_lines++;
    }
}

static void test_gaps_r2_ct_full_ring_drops_the_protocol_packet_once()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_rings_client( NULL, &rocketnet_fake_client_operations_io, fake );
    r2_rings_seed_connected( transport );

    r2_rings_overfill( transport );

    const uint64_t * values = rocketnet_client_transport_counters( transport )->values;
    const uint64_t full_before = values[ROCKETNET_COUNTER_SEND_RING_FULL];

    r2_ring_full_lines = 0;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( r2_ring_full_log, NULL );

    // a second on: the protocol's keep-alive is due, and the full ring has no slot for it

    r2_rings_now = 1001.0;
    rocketnet_client_transport_update( transport, r2_rings_now );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    const uint64_t full_after = rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    if ( full_after != full_before + 1 )
    {
        printf( "mutation gap hunt 2/7427,7428,7429: the update's protocol packet, dropped on a full ring, moved SEND_RING_FULL by %lld, not 1\n",
                (long long) ( full_after - full_before ) );
    }
    check( full_after == full_before + 1 );

    if ( r2_ring_full_lines != 1 )
    {
        printf( "mutation gap hunt 2/7426: the protocol packet's drop on a full ring logged %d \"protocol packet dropped\" line(s), not 1\n", r2_ring_full_lines );
    }
    check( r2_ring_full_lines == 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_full_ring_drops_the_protocol_packet_once );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7447, 2/7453 (the BASE receive budget splits buffers that fit it exactly)
//
// The BASE receive worker publishes a buffer of 1000 datagrams, then one of
// 24: together they are exactly the update's packet budget (1024), so the
// next update takes both, and no buffer is left ready after it.

struct r2_runts_t
{
    rocketnet_fake_client_transport_io_t fake;
    int pending;
};

static int r2_runts_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    r2_runts_t * runts = (r2_runts_t *) transport;
    if ( runts->pending == 0 || capacity < 1 )
    {
        return 0;
    }
    runts->pending--;
    r2_rings_server_address( from );
    data[0] = 0x55;
    return 1;
}

static void test_gaps_r2_ct_base_receive_takes_buffers_that_fit_the_budget_exactly()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = r2_runts_receive;

    r2_runts_t * runts = new r2_runts_t();
    rocketnet_client_transport_t * transport = r2_rings_client( NULL, &operations, runts );

    runts->pending = 1000;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    runts->pending = 24;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );

    int ready = -1;
    rocketnet_client_transport_legacy_receive_state_for_test( transport, NULL, &ready, NULL );
    check( ready == 2 );

    rocketnet_client_transport_update( transport, 1000.0 );

    rocketnet_client_transport_legacy_receive_state_for_test( transport, NULL, &ready, NULL );
    if ( ready != 0 )
    {
        printf( "mutation gap hunt 2/7447,7453: buffers of 1000 and 24 datagrams, exactly the 1024-packet budget, were split: %d buffer(s) still ready after the update\n", ready );
    }
    check( ready == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete runts;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_base_receive_takes_buffers_that_fit_the_budget_exactly );

// ---------------------------------------------------------------------------
// The production BASE path without a link simulator.
//
// A production library creates no link simulator (rocketnet_network_simulator.h
// stubs it to NULL), and on a platform with no client wake (no
// ROCKETNET_PLATFORM_HAS_WAKE: BSD) every built-in client transport runs the
// BASE path. Linux, macOS and Windows have the wake and every production
// client there runs the I/O core, so no CI leg reaches these lines in a
// production build. A development connection always owns a simulator, so
// rocketnet_client_transport_drop_link_simulator_for_test drops a step-driven
// BASE client's idle simulator before its first send: the client is then in
// the production BASE state.

static rocketnet_client_transport_t * r2_base_production_client( rocketnet_fake_client_transport_io_t * fake )
{
    rocketnet_client_transport_t * transport = r2_rings_client( NULL, &rocketnet_fake_client_operations_base, fake );
    check( rocketnet_client_transport_drop_link_simulator_for_test( transport ) );
    r2_rings_seed_connected( transport );
    check( rocketnet_client_transport_client_io( transport ) == NULL );
    return transport;
}

// mutation gap: hunt 2/6688 (the per-packet send with no link simulator sends nothing)

static void test_gaps_r2_ct_base_without_simulator_sends_each_payload()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_base_production_client( fake );

    const int sent_before = fake->num_sent;
    const uint8_t payload[R2_PAYLOAD_BYTES] = { 0x42 };
    rocketnet_client_transport_send_packet( transport, payload, (int) sizeof( payload ) );

    rocketnet_address_t server;
    r2_rings_server_address( &server );
    const bool sent = fake->num_sent == sent_before + 1 && fake->sent[sent_before].bytes > R2_PAYLOAD_BYTES &&
                      rocketnet_address_equal( &fake->sent[sent_before].address, &server );
    if ( !sent )
    {
        printf( "mutation gap hunt 2/6688: a production BASE client (no link simulator) sent %d datagram(s) for one payload, not one to its server\n",
                fake->num_sent - sent_before );
    }
    check( sent );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_base_without_simulator_sends_each_payload );

// mutation gap: hunt 2/7832, 2/7833, 2/7834, 2/7835, 2/7836, 2/7837, 2/7838 (disconnect stores the burst for the worker)
// mutation gap: hunt 2/7320, 2/7321, 2/7322 (the worker sends the stored burst once)
//
// A production BASE client's disconnect seals its burst and leaves it for
// the receive worker: the worker's next pass sends every packet of it, each
// to the server, each a distinct sealed packet of the burst's size, and the
// pass after that sends nothing more.

static void test_gaps_r2_ct_base_without_simulator_worker_sends_the_disconnect_burst_once()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = r2_base_production_client( fake );

    rocketnet_address_t server;
    r2_rings_server_address( &server );

    const int sent_before = fake->num_sent;
    rocketnet_client_transport_disconnect( transport );
    check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    if ( fake->num_sent != sent_before )
    {
        printf( "mutation gap hunt 2/7832: the disconnect itself sent %d datagram(s): the burst belongs to the worker\n", fake->num_sent - sent_before );
    }
    check( fake->num_sent == sent_before );

    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );

    const int burst = fake->num_sent - sent_before;
    if ( burst != ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS )
    {
        printf( "mutation gap hunt 2/7320,7321,7833,7835,7837,7838: the worker's pass after a disconnect sent %d datagram(s), not the burst of %d\n",
                burst, ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    }
    check( burst == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );

    for ( int i = 0; i < burst && i < ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS; i++ )
    {
        const rocketnet_fake_client_datagram_t * dg = &fake->sent[sent_before + i];
        if ( !rocketnet_address_equal( &dg->address, &server ) )
        {
            char string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            printf( "mutation gap hunt 2/7832: disconnect packet %d went to %s, not the server\n", i, rocketnet_address_to_string( &dg->address, string ) );
        }
        check( rocketnet_address_equal( &dg->address, &server ) );

        const rocketnet_fake_client_datagram_t * first = &fake->sent[sent_before];
        if ( dg->bytes <= 0 || dg->bytes != first->bytes )
        {
            printf( "mutation gap hunt 2/7834: disconnect packet %d is %d byte(s), packet 0 is %d\n", i, dg->bytes, first->bytes );
        }
        check( dg->bytes > 0 && dg->bytes == first->bytes );

        // each packet of the burst is sealed under its own sequence: two equal ones mean one was never copied

        for ( int j = 0; j < i; j++ )
        {
            const rocketnet_fake_client_datagram_t * other = &fake->sent[sent_before + j];
            const bool same = dg->bytes == other->bytes && dg->bytes > 0 && memcmp( dg->data, other->data, (size_t) dg->bytes ) == 0;
            if ( same )
            {
                printf( "mutation gap hunt 2/7836: disconnect packets %d and %d are the same bytes: one is not the sealed packet\n", j, i );
            }
            check( !same );
        }

        bool one_value = dg->bytes > 0;
        for ( int b = 1; b < dg->bytes; b++ )
        {
            one_value = one_value && dg->data[b] == dg->data[0];
        }
        if ( one_value )
        {
            printf( "mutation gap hunt 2/7836: disconnect packet %d is %d bytes of 0x%02x: stale bytes, not a sealed packet\n", i, dg->bytes, dg->data[0] );
        }
        check( !one_value );
    }

    // the next pass and stop send nothing more

    const int after_burst = fake->num_sent;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    if ( fake->num_sent != after_burst )
    {
        printf( "mutation gap hunt 2/7322: the worker's next pass sent the disconnect burst again (%d more datagram(s))\n", fake->num_sent - after_burst );
    }
    check( fake->num_sent == after_burst );

    rocketnet_client_transport_stop( transport );
    rn_test_client_transport_wait_stopped( transport );
    check( fake->num_sent == after_burst );

    rocketnet_client_transport_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_base_without_simulator_worker_sends_the_disconnect_burst_once );

#endif // #if ROCKETNET_DEV

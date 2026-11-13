/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Memory safety and stale data in the client transport
// (sdk/rocketnet_client_transport.cpp): the bytes just past the payload
// rings, a started slot that is not yet sealed, the family race's batch send
// with an empty batch and its commit at a connection's end, the memory plan
// seam's receive calls, the legacy receive's fault count at the size limit,
// the legacy hand-off seam, the race commit seam's refusals and the mutex at
// destroy. Each datagram is seen through the fake client transport
// (tests/fake_client_driver.h): no socket, no thread; time is an argument
// starting at 1000.0. Each failing check prints the fault first; a call that
// would crash on the fault is announced in a line printed before it, which
// the runner shows only when the case fails.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "loopback_hub.h"

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_config.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <map>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

extern void rocketnet_client_transport_counts_for_test( const rocketnet_client_transport_t * client_transport, uint64_t * ring_counts,
                                                        int * received_num_packets, int * received_receive_index, int * buffer_counts,
                                                        uint64_t * transport_faults );
extern void rocketnet_client_transport_layout_for_test( size_t * past_rings_count_offset, size_t * stop_burst_offset, size_t * stop_burst_bytes );
extern void rocketnet_client_transport_seed_refusal_state_for_test( rocketnet_client_transport_t *, uint64_t, const uint8_t *, const uint8_t *, const uint8_t * );
extern bool rocketnet_client_transport_commit_race_for_test( rocketnet_client_transport_t *, int );
extern void rocketnet_client_transport_set_memory_plan_seam_for_test( bool enable );
extern void rocketnet_client_transport_legacy_receive_state_for_test( const rocketnet_client_transport_t * client_transport, int * fill, int * ready_count, int * free_count );

// ---------------------------------------------------------------------------
// a step-driven client on the BASE path (the fake's four required calls)

static double ct_rings_now = 1000.0;
static double ct_rings_time( void * ) { return ct_rings_now; }

static void ct_rings_token( uint8_t * token )
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

static void ct_rings_server_address( rocketnet_address_t * address )
{
    memset( address, 0, sizeof( *address ) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = 127;
    address->data.ipv4[3] = 1;
    address->port = 40000;
}

static rocketnet_client_transport_t * ct_rings_client( const rocketnet_client_transport_io_operations_t * operations, rocketnet_fake_client_transport_io_t * fake )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    ct_rings_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = ct_rings_time;

    ct_rings_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, operations, fake );
    check( transport != NULL );
    return transport;
}

static void ct_rings_seed_connected( rocketnet_client_transport_t * transport )
{
    rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value,
                                                            rocketnet_test_salt, rocketnet_test_session_s2c_key );
    check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
}

static uint64_t ct_rings_packets_sent( rocketnet_client_transport_t * transport )
{
    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( transport, &stats );
    return stats.packets_sent;
}

static uint64_t ct_rings_packets_received( rocketnet_client_transport_t * transport )
{
    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( transport, &stats );
    return stats.packets_received;
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6423 (the connection reset's ring loop runs to <= NUM_RINGS)
//
// The connection reset writes the count of each payload ring and nothing
// past the last: the 8 bytes where a fourth ring's count would be lie inside
// the transport, in stop_burst, so ASan cannot see a write there. A guard
// planted in those bytes (stop_burst is idle while connected) is intact after
// the disconnect that runs the reset. A UBSan bounds leg also sees the
// out-of-range index directly.

static void test_gaps_ct_memory_connection_reset_writes_nothing_past_the_rings()
{
    size_t past_rings = 0, stop_burst = 0, stop_burst_bytes = 0;
    rocketnet_client_transport_layout_for_test( &past_rings, &stop_burst, &stop_burst_bytes );

    // the guard is in a member the reset never writes: if a layout change moves stop_burst, the guard must move with it

    const bool in_stop_burst = past_rings >= stop_burst && past_rings + sizeof( uint64_t ) <= stop_burst + stop_burst_bytes;
    if ( !in_stop_burst )
    {
        printf( "mutation gap hunt 2/6423: the bytes past payload_ring[] (offset %zu) are no longer inside stop_burst [%zu, %zu): move the guard\n",
                past_rings, stop_burst, stop_burst + stop_burst_bytes );
    }
    check( in_stop_burst );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_rings_client( &rocketnet_fake_client_operations_base, fake );
    ct_rings_seed_connected( transport );

    uint8_t * guard = (uint8_t *) transport + past_rings;
    uint8_t planted[sizeof( uint64_t )];
    memset( planted, 0x5A, sizeof( planted ) );
    if ( in_stop_burst )
    {
        memcpy( guard, planted, sizeof( planted ) );
    }

    rocketnet_client_transport_disconnect( transport );
    check( rocketnet_client_transport_state( transport ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    if ( in_stop_burst )
    {
        const bool intact = memcmp( guard, planted, sizeof( planted ) ) == 0;
        if ( !intact )
        {
            printf( "mutation gap hunt 2/6423: the connection reset wrote the 8 bytes one ring past payload_ring[%d] (inside stop_burst): an index past the rings\n",
                    ROCKETNET_CLIENT_IO_NUM_RINGS );
        }
        check( intact );
        memset( guard, 0, sizeof( planted ) );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_connection_reset_writes_nothing_past_the_rings );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7639 (start_packet marks the slot 1 byte instead of 0)
//
// A started slot that is not finished is not a packet: a flush while it is
// open sends nothing and counts nothing. The BASE path's flush walks ring 0
// and sends every slot with a wire size.

static void test_gaps_ct_memory_open_slot_is_never_sent()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_rings_client( &rocketnet_fake_client_operations_base, fake );
    ct_rings_seed_connected( transport );

    const int sent_before = fake->num_sent;
    const uint64_t counted_before = ct_rings_packets_sent( transport );

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( transport, 40, &context );
    check( frame != NULL );

    rocketnet_client_transport_send_packets( transport );

    if ( fake->num_sent != sent_before )
    {
        printf( "mutation gap hunt 2/7639: a flush with one slot started and not finished sent %d datagram(s) of %d byte(s): an unsealed frame left\n",
                fake->num_sent - sent_before, fake->num_sent > sent_before ? fake->sent[sent_before].bytes : 0 );
    }
    check( fake->num_sent == sent_before );
    check( ct_rings_packets_sent( transport ) == counted_before );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_open_slot_is_never_sent );

// ---------------------------------------------------------------------------
// the family race: one fake socket per family, as tests/test_unit_client_bind_family.cpp
// sets it up (fake[0] the token's IPv6 server, fake[1] its IPv4 server)

struct ct_race_pair_t
{
    rocketnet_fake_client_transport_io_t fake[2];
    int creates;
    uint64_t send_failures[2];
    int progress_refusals[2];
};

static ct_race_pair_t * ct_race_pair;

static void * ct_race_pair_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    ct_race_pair_t * pair = (ct_race_pair_t *) context;
    check( pair->creates < 2 );
    return pair->creates < 2 ? &pair->fake[pair->creates++] : NULL;
}

static uint64_t ct_race_pair_send_failures( void * instance )
{
    return ct_race_pair->send_failures[instance == &ct_race_pair->fake[1] ? 1 : 0];
}

static int ct_race_pair_send_progress( void * instance, const rocketnet_platform_send_t * sends, int count )
{
    const int family = instance == &ct_race_pair->fake[1] ? 1 : 0;
    if ( ct_race_pair->progress_refusals[family] > 0 )
    {
        ct_race_pair->progress_refusals[family]--;
        return 0;
    }
    rocketnet_fake_client_send_batch( instance, sends, count );
    return count;
}

struct ct_race_t
{
    rocketnet_address_t ipv4_server, ipv6_server;
    rocketnet_client_transport_io_operations_t operations;
    ct_race_pair_t pair;
    double now;
    rocketnet_client_transport_t * transport;
    rocketnet_client_io_t * io;
};

static double ct_race_time( void * context )
{
    return ( (ct_race_t *) context )->now;
}

static void ct_race_mint_both( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = 5;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;

    const uint8_t ipv4_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, ipv4_address, 6 );

    const uint8_t ipv6_address[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };
    memcpy( signed_fields.ipv6_address, ipv6_address, 18 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void ct_race_setup( ct_race_t * r, bool progress )
{
    check( rocketnet_address_parse( &r->ipv4_server, "203.0.113.10:40000" ) );
    check( rocketnet_address_parse( &r->ipv6_server, "[2001:db8::1]:40000" ) );

    r->operations = rocketnet_fake_client_operations_io;
    r->operations.create = ct_race_pair_create;
    r->operations.connect = rocketnet_fake_client_connect;
    r->operations.close = rocketnet_fake_client_close;
    r->operations.send_failures = ct_race_pair_send_failures;
    if ( progress )
    {
        r->operations.send_batch = rocketnet_fake_client_send_batch;
        r->operations.send_batch_progress = ct_race_pair_send_progress;
    }
    ct_race_pair = &r->pair;

    r->now = 1000.0;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    ct_race_mint_both( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.time_callback = ct_race_time;
    memcpy( config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    r->transport = rocketnet_client_transport_create_step_driven_at_timestamp( r, token, &config, r->now, ROCKETNET_TEST_CREATE_TS,
                                                                               &r->operations, &r->pair );
    check( r->transport != NULL );
    check( r->pair.creates == 2 );
    r->io = rocketnet_client_transport_client_io( r->transport );
    check( r->io != NULL );
}

static void ct_race_teardown( ct_race_t * r )
{
    rn_test_client_transport_stop_and_destroy( r->transport );
    ct_race_pair = NULL;
}

static uint64_t ct_race_send_failed( ct_race_t * r )
{
    return rocketnet_client_transport_counters( r->transport )->values[ROCKETNET_COUNTER_SEND_FAILED];
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6579 (the race's batch send settles a pending head on an empty batch)
//
// A fanout head is pending: its IPv6 copy left, its IPv4 copy was refused.
// The race's send_batch with no packets touches nothing: it neither settles
// the head (which would read sends[0] past an empty batch) nor sends. The
// head then settles on the next batch that carries it, once.

static void test_gaps_ct_memory_race_empty_batch_leaves_the_pending_head()
{
    ct_race_t * r = new ct_race_t();
    memset( (void *) r, 0, sizeof( *r ) );
    ct_race_setup( r, true );

    const rocketnet_client_transport_io_t * io = r->io->transport;
    const uint8_t data = 61;
    rocketnet_platform_send_t send = { &r->ipv6_server, &data, 1 };

    r->pair.progress_refusals[1] = 1000;
    check( io->operations->send_batch_progress( io->instance, &send, 1 ) == 0 );
    check( r->pair.fake[0].num_sent == 1 && r->pair.fake[1].num_sent == 0 );

    // the batch pointer names the pending head itself, so a wrong settle is seen as a send, not a crash

    io->operations->send_batch( io->instance, &send, 0 );
    if ( r->pair.fake[0].num_sent != 1 || r->pair.fake[1].num_sent != 0 )
    {
        printf( "mutation gap hunt 2/6579: the race's send_batch of 0 packets sent %d IPv6 and %d IPv4 datagram(s): it settled the pending head past an empty batch\n",
                r->pair.fake[0].num_sent - 1, r->pair.fake[1].num_sent );
    }
    check( r->pair.fake[0].num_sent == 1 && r->pair.fake[1].num_sent == 0 );

    // the head is still pending: the batch that carries it sends only its missing copy

    io->operations->send_batch( io->instance, &send, 1 );
    check( r->pair.fake[0].num_sent == 1 && r->pair.fake[1].num_sent == 1 );

    ct_race_teardown( r );
    delete r;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_race_empty_batch_leaves_the_pending_head );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7564 (a committed race is committed again when the connection ends)
//
// The race commits to the IPv6 family with the IPv4 family's three send
// failures kept; the reading step closes IPv4. The closed socket's count
// moves after that (the fake lets it), and the connection then times out:
// the end of a connection whose race already committed changes nothing, so
// SEND_FAILED is the same number before and after the end.

static void test_gaps_ct_memory_race_end_keeps_a_committed_race()
{
    ct_race_t * r = new ct_race_t();
    memset( (void *) r, 0, sizeof( *r ) );
    ct_race_setup( r, false );

    rocketnet_client_io_step( r->io, false );
    rocketnet_client_transport_update( r->transport, r->now );
    rocketnet_client_io_step( r->io, false );

    r->pair.send_failures[0] = 2;
    r->pair.send_failures[1] = 3;
    check( rocketnet_client_transport_commit_race_for_test( r->transport, 0 ) );
    rocketnet_client_io_step( r->io, false );
    check( r->pair.fake[1].close_calls == 1 );

    r->pair.send_failures[1] = 7;
    const uint64_t before = ct_race_send_failed( r );
    check( before == 5 );

    const int state_before = rocketnet_client_transport_state( r->transport );
    check( state_before > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    r->now = 1010.0;
    rocketnet_client_transport_update( r->transport, r->now );
    check( rocketnet_client_transport_state( r->transport ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    const uint64_t after = ct_race_send_failed( r );
    if ( after != before )
    {
        printf( "mutation gap hunt 2/7564: SEND_FAILED %llu before the connection's end and %llu after it: the end committed the already committed race again\n",
                (unsigned long long) before, (unsigned long long) after );
    }
    check( after == before );

    ct_race_teardown( r );
    delete r;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_race_end_keeps_a_committed_race );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6870 (the race commit seam calls the commit without a race or with a family outside 0..1)
//
// The seam refuses a client with no race and a family outside 0 and 1, and
// commits nothing: the race stays in flight, both sockets open.

static void test_gaps_ct_memory_commit_race_seam_refuses()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_rings_client( &rocketnet_fake_client_operations_base, fake );

    printf( "mutation gap hunt 2/6870: committing a race on a client that has none (a crash here: the seam called the commit with no race)\n" );
    fflush( stdout );
    const bool no_race = rocketnet_client_transport_commit_race_for_test( transport, 0 );
    if ( no_race )
    {
        printf( "mutation gap hunt 2/6870: the race commit seam accepted a client with no race\n" );
    }
    check( !no_race );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    ct_race_t * r = new ct_race_t();
    memset( (void *) r, 0, sizeof( *r ) );
    ct_race_setup( r, false );

    const int families[2] = { 2, -1 };
    for ( int i = 0; i < 2; i++ )
    {
        printf( "mutation gap hunt 2/6870: committing the race to family %d (a crash here: the seam indexed the race's families with it)\n", families[i] );
        fflush( stdout );
        const bool committed = rocketnet_client_transport_commit_race_for_test( r->transport, families[i] );
        if ( committed )
        {
            printf( "mutation gap hunt 2/6870: the race commit seam accepted family %d\n", families[i] );
        }
        check( !committed );
    }

    // nothing was committed: the reading step closes neither socket

    rocketnet_client_io_step( r->io, false );
    check( r->pair.fake[0].close_calls == 0 && r->pair.fake[1].close_calls == 0 );

    ct_race_teardown( r );
    delete r;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_commit_race_seam_refuses );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7118 (the memory plan seam's receive_batch reports a datagram)
//
// The memory plan seam's client has no socket: its batch receive reports
// nothing, so a step and an update of a seam client receive no datagram and
// count no fault.

static void ct_rings_seam_token( uint8_t * token )
{
    ct_rings_token( token );
}

static void test_gaps_ct_memory_plan_seam_batch_receive_is_empty()
{
    rocketnet_client_transport_set_memory_plan_seam_for_test( true );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    ct_rings_seam_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.step_driven = 1;
    config.time_callback = ct_rings_time;
    ct_rings_now = 1000.0;

    int status = -1;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, &status );
    check( transport != NULL );
    check( status == ROCKETNET_STATUS_OK );

    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    check( io != NULL );
    if ( transport && io )
    {
        rocketnet_client_io_step( io, false );
        rocketnet_client_transport_update( transport, 1000.0 );

        const uint64_t faults = io->transport_faults.load( std::memory_order_relaxed );
        const uint64_t received = ct_rings_packets_received( transport );
        if ( faults != 0 || received != 0 )
        {
            printf( "mutation gap hunt 2/7118: a memory plan seam client with no socket received %llu datagram(s) and %llu fault(s) in one step\n",
                    (unsigned long long) received, (unsigned long long) faults );
        }
        check( faults == 0 );
        check( received == 0 );
    }

    rn_test_client_transport_stop_and_destroy( transport );

    rocketnet_client_transport_set_memory_plan_seam_for_test( false );
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_plan_seam_batch_receive_is_empty );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7117 (the memory plan seam's single receive reports a byte)
//
// A seam client created with a link simulator setting runs the BASE path,
// which reads one datagram at a time: the seam's receive reports nothing, so
// an update (which receives, in the caller-stepped mode) takes nothing and
// counts nothing.

static void test_gaps_ct_memory_plan_seam_receive_is_empty()
{
    rocketnet_client_transport_set_memory_plan_seam_for_test( true );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    ct_rings_seam_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.step_driven = 1;
    config.simulated_latency_ms = 50;
    config.time_callback = ct_rings_time;
    ct_rings_now = 1000.0;

    int status = -1;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, &status );
    check( transport != NULL );
    check( status == ROCKETNET_STATUS_OK );

    if ( transport )
    {
        const uint64_t * values = rocketnet_client_transport_counters( transport )->values;
        const uint64_t too_short = values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT];
        const uint64_t foreign = values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE];

        // the caller-stepped mode (config step_driven): the update itself receives on the BASE path, stamped with the update time

        rocketnet_client_transport_update( transport, 1000.0 );

        const uint64_t received = ct_rings_packets_received( transport );
        if ( received != 0 || values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] != too_short ||
             values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] != foreign )
        {
            printf( "mutation gap hunt 2/7117: a memory plan seam client with no socket received %llu datagram(s) in one update (PREFILTER_TOO_SHORT +%llu, FOREIGN_SOURCE +%llu)\n",
                    (unsigned long long) received, (unsigned long long) ( values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] - too_short ),
                    (unsigned long long) ( values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] - foreign ) );
        }
        check( received == 0 );
        check( values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] == too_short );
        check( values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] == foreign );
    }

    rn_test_client_transport_stop_and_destroy( transport );

    rocketnet_client_transport_set_memory_plan_seam_for_test( false );
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_plan_seam_receive_is_empty );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7943 (a byte count of exactly the size limit counted as a fault)
//
// The BASE receive keeps a datagram of exactly ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES
// (the update's prefilter refuses it as too large): it is not a transport
// fault. A count one past the limit is one fault.

static int ct_rings_fault_bytes = 0;
static int ct_rings_fault_calls = 0;

static int ct_rings_fault_receive( void *, rocketnet_address_t * from, uint8_t *, int )
{
    if ( ct_rings_fault_calls++ > 0 )
    {
        return 0;
    }
    ct_rings_server_address( from );
    return ct_rings_fault_bytes;
}

static uint64_t ct_rings_transport_faults( const rocketnet_client_transport_t * transport )
{
    uint64_t rings[ROCKETNET_CLIENT_IO_NUM_RINGS];
    int received_num_packets = 0, received_receive_index = 0;
    int buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    uint64_t faults = 0;
    rocketnet_client_transport_counts_for_test( transport, rings, &received_num_packets, &received_receive_index, buffers, &faults );
    return faults;
}

static void test_gaps_ct_memory_legacy_size_limit_is_not_a_fault()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = ct_rings_fault_receive;
    rocketnet_client_transport_t * transport = ct_rings_client( &operations, fake );

    const uint64_t faults = ct_rings_transport_faults( transport );

    ct_rings_fault_bytes = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES;
    ct_rings_fault_calls = 0;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    check( ct_rings_fault_calls == 2 );
    if ( ct_rings_transport_faults( transport ) != faults )
    {
        printf( "mutation gap hunt 2/7943: a datagram of exactly %d bytes (the UDP size limit) was counted as a transport fault\n",
                ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES );
    }
    check( ct_rings_transport_faults( transport ) == faults );
    rocketnet_client_transport_update( transport, 1000.0 );

    // one past the limit is the fault

    ct_rings_fault_bytes = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES + 1;
    ct_rings_fault_calls = 0;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.1 );
    check( ct_rings_transport_faults( transport ) == faults + 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_legacy_size_limit_is_not_a_fault );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7975 (the legacy receive state seam does not report fill)
//
// The seam reports the worker's fill buffer: one datagram received and
// published moves fill to another buffer, and the seam says so. A NULL fill
// asks for nothing and writes nothing.

static int ct_rings_one_left = 0;

static int ct_rings_one_receive( void *, rocketnet_address_t * from, uint8_t * data, int )
{
    if ( ct_rings_one_left <= 0 )
    {
        return 0;
    }
    ct_rings_one_left--;
    ct_rings_server_address( from );
    data[0] = 0;
    return 1;
}

static void test_gaps_ct_memory_legacy_state_seam_reports_fill()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = ct_rings_one_receive;
    rocketnet_client_transport_t * transport = ct_rings_client( &operations, fake );

    int fill_before = -7, ready = -7, free_count = -7;
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill_before, &ready, &free_count );

    ct_rings_one_left = 1;
    rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
    check( ct_rings_one_left == 0 );

    int fill_after = -7;
    ready = -7;
    rocketnet_client_transport_legacy_receive_state_for_test( transport, &fill_after, &ready, &free_count );
    check( ready == 1 );

    const bool reported = fill_before >= 0 && fill_before < ROCKETNET_CLIENT_IO_NUM_BUFFERS && fill_after >= 0 &&
                          fill_after < ROCKETNET_CLIENT_IO_NUM_BUFFERS && fill_after != fill_before;
    if ( !reported )
    {
        printf( "mutation gap hunt 2/7975: the legacy receive state seam reported fill %d before a publish and %d after it: it does not report the fill buffer\n",
                fill_before, fill_after );
    }
    check( reported );

    printf( "mutation gap hunt 2/7975: asking the legacy receive state seam with fill NULL (a crash here: the seam wrote through NULL)\n" );
    fflush( stdout );
    ready = -7;
    rocketnet_client_transport_legacy_receive_state_for_test( transport, NULL, &ready, NULL );
    check( ready == 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_legacy_state_seam_reports_fill );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7351 (destroy does not destroy the transport's mutex)
//
// Destroy destroys the mutex create made: by the time the transport block
// goes back to the allocator, the platform mutex in it reads as destroyed
// (rocketnet_platform_mutex_destroy clears it). The mutex create is
// recorded through the platform's call table, so the case finds the mutex
// without knowing the transport's layout.

static rocketnet_platform_mutex_t * ct_rings_mutexes[64];
static int ct_rings_num_mutexes = 0;
static std::map<void *, size_t> * ct_rings_blocks = NULL;
static int ct_rings_live_mutexes_freed = 0;
static int ct_rings_mutexes_freed = 0;

static int ct_rings_mutex_init( rocketnet_platform_mutex_t * mutex )
{
    if ( ct_rings_num_mutexes < (int) ( sizeof( ct_rings_mutexes ) / sizeof( ct_rings_mutexes[0] ) ) )
    {
        ct_rings_mutexes[ct_rings_num_mutexes++] = mutex;
    }
    return rocketnet_platform_priority_calls_real()->mutex_init( mutex );
}

static void * ct_rings_malloc( void * context, size_t bytes )
{
    (void) context;
    void * p = malloc( bytes );
    if ( p && ct_rings_blocks )
    {
        ( *ct_rings_blocks )[p] = bytes;
    }
    return p;
}

static void ct_rings_free( void * context, void * p )
{
    (void) context;
    if ( p && ct_rings_blocks )
    {
        std::map<void *, size_t>::iterator it = ct_rings_blocks->find( p );
        if ( it != ct_rings_blocks->end() )
        {
            const uint8_t * begin = (const uint8_t *) p;
            const uint8_t * end = begin + it->second;
            for ( int i = 0; i < ct_rings_num_mutexes; i++ )
            {
                const uint8_t * m = (const uint8_t *) ct_rings_mutexes[i];
                if ( m >= begin && m + sizeof( rocketnet_platform_mutex_t ) <= end )
                {
                    ct_rings_mutexes_freed++;
                    if ( ct_rings_mutexes[i]->ok )
                    {
                        ct_rings_live_mutexes_freed++;
                    }
                }
            }
            ct_rings_blocks->erase( it );
        }
    }
    free( p );
}

static void test_gaps_ct_memory_destroy_destroys_the_mutex()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( NULL, ct_rings_malloc, ct_rings_free, &status ) );

    std::map<void *, size_t> blocks;
    ct_rings_blocks = &blocks;
    ct_rings_num_mutexes = 0;
    ct_rings_mutexes_freed = 0;
    ct_rings_live_mutexes_freed = 0;

    struct rocketnet_platform_priority_calls_t calls = *rocketnet_platform_priority_calls_current();
    calls.mutex_init = ct_rings_mutex_init;
    rocketnet_platform_set_priority_calls_for_testing( &calls );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_rings_client( &rocketnet_fake_client_operations_base, fake );

    rocketnet_platform_set_priority_calls_for_testing( NULL );
    check( ct_rings_num_mutexes >= 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    if ( ct_rings_live_mutexes_freed != 0 )
    {
        printf( "mutation gap hunt 2/7351: %d platform mutex(es) freed with the client transport were never destroyed\n", ct_rings_live_mutexes_freed );
    }
    check( ct_rings_mutexes_freed >= 1 );
    check( ct_rings_live_mutexes_freed == 0 );

    ct_rings_blocks = NULL;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_destroy_destroys_the_mutex );

#endif // #if ROCKETNET_DEV

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport's packet lease refusals, its sent-bytes statistics at
// hand-off and at stop, its memory plan seam and its secret-span seam
// (sdk/rocketnet_client_transport.cpp), on the in-memory loopback transport
// with a development-only step-driven client: no socket, no thread, no real
// time; every update time is given, from 1000.0.

#include "test_unit_registry.h"

#define check rocketnet_unit_check
#include "public_step_pair.h"

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_layers.h"
#include "rocketnet_memory.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

// a failing check prints the fault it found, then fails as every unit check does

#define delta_tr_check( condition, fault )                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if ( !( condition ) )                                                                                          \
        {                                                                                                              \
            printf( "mutation gap: %s\n", fault );                                                                     \
        }                                                                                                              \
        rocketnet_unit_check( condition );                                                                             \
    } while ( 0 )

static rocketnet_client_transport_t * delta_tr_transport( public_step_pair_t * pair )
{
    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( pair->client ) );
    rocketnet_unit_check( transport != NULL );
    return transport;
}

static void delta_tr_connect( public_step_pair_t * pair, public_step_identity_t * identity )
{
    public_step_identity( identity );
    public_step_pair_create( pair, identity );
    public_step_connect( pair );
}

// ---------------------------------------------------------------------------
// the secret-span seam: at most max spans, never more than the four there are

// mutation gap: delta sdk/rocketnet_client_transport.cpp:2977 boolean || (count < 4 || count < max)
// mutation gap: delta sdk/rocketnet_client_transport.cpp:2977 relational <= (count < 4 && count <= max)

static void test_delta_client_secret_spans_respect_max_and_the_four()
{
    size_t offsets[8];
    size_t bytes[8];
    for ( int i = 0; i < 8; i++ )
    {
        offsets[i] = 0xA5A5;
        bytes[i] = 0xA5A5;
    }

    const int all = rocketnet_client_transport_secret_spans_for_test( NULL, offsets, bytes, 8 );
    delta_tr_check( all == 4, "the client secret-span seam did not return exactly its four spans when given room for eight" );
    delta_tr_check( offsets[4] == 0xA5A5 && bytes[4] == 0xA5A5, "the client secret-span seam wrote a fifth span" );
    for ( int i = 0; i < 4; i++ )
    {
        delta_tr_check( bytes[i] > 0, "a client secret span has no bytes" );
    }

    size_t few_offsets[4] = { 0xA5A5, 0xA5A5, 0xA5A5, 0xA5A5 };
    size_t few_bytes[4] = { 0xA5A5, 0xA5A5, 0xA5A5, 0xA5A5 };
    const int two = rocketnet_client_transport_secret_spans_for_test( NULL, few_offsets, few_bytes, 2 );
    delta_tr_check( two == 2, "the client secret-span seam did not stop at max (2)" );
    delta_tr_check( few_offsets[2] == 0xA5A5 && few_bytes[2] == 0xA5A5, "the client secret-span seam wrote past max" );
    delta_tr_check( few_offsets[0] == offsets[0] && few_bytes[1] == bytes[1], "the client secret-span seam's first spans differ with max" );
}
REGISTER_UNIT_TEST( test_delta_client_secret_spans_respect_max_and_the_four );

// ---------------------------------------------------------------------------
// a lease reset by send_packets: packet_payload refuses it and leaves capacity
// 0; finish and abort refuse it, count it and log it, in every build

// mutation gap: delta sdk/rocketnet_client_transport.cpp:3779 off_by_one 1 (*capacity = 1)
// mutation gap: delta sdk/rocketnet_client_transport.cpp:3814 negate_condition (abort: !retired)
// mutation gap: delta sdk/rocketnet_client_transport.cpp:3895 negate_condition (finish: !retired)

static int delta_tr_abort_logs = 0;
static int delta_tr_finish_logs = 0;

static void delta_tr_log_capture( void * context, rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
    {
        return;
    }
    if ( strstr( message, "client abort packet refused: the lease was reset" ) )
    {
        delta_tr_abort_logs++;
    }
    if ( strstr( message, "client finish packet refused: the lease was reset" ) )
    {
        delta_tr_finish_logs++;
    }
}

static int delta_tr_asserts = 0;

static void delta_tr_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    delta_tr_asserts++;
}

static void test_delta_client_retired_lease_is_refused_with_capacity_zero_and_logged()
{
    public_step_identity_t identity;
    public_step_pair_t pair;
    delta_tr_connect( &pair, &identity );
    rocketnet_client_transport_t * transport = delta_tr_transport( &pair );

    rocketnet_packet_context_t context;
    rocketnet_unit_check( rocketnet_client_transport_start_packet( transport, 16, &context ) != NULL );
    rocketnet_packet_context_t finish_copy = context;
    rocketnet_packet_context_t abort_copy = context;

    rocketnet_client_transport_send_packets( transport );

    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    delta_tr_asserts = 0;
    rocketnet_assert_function( delta_tr_assert_hook );

    int capacity = 77;
    uint8_t * payload = rocketnet_client_transport_packet_payload( transport, &context, &capacity );
    delta_tr_check( payload == NULL, "packet_payload handed out a payload for a lease send_packets reset" );
    delta_tr_check( capacity == 0, "packet_payload refused a reset lease but left a nonzero capacity" );

    const rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_quiet( false );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    delta_tr_abort_logs = 0;
    delta_tr_finish_logs = 0;
    rocketnet_log_function( delta_tr_log_capture, NULL );

    const uint64_t misuse_before = rocketnet_api_misuse_count();
    const int finished = rocketnet_client_transport_finish_packet( transport, &finish_copy, 16 );
    rocketnet_client_transport_abort_packet( transport, &abort_copy );
    const uint64_t misuse_after = rocketnet_api_misuse_count();

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );
    rocketnet_quiet( true );
    rocketnet_assert_function( saved_assert );

    delta_tr_check( finished == 0, "finish of a lease send_packets reset was not refused" );
    delta_tr_check( misuse_after == misuse_before + 2, "the refused finish and abort of a reset lease were not each counted as one misuse" );
    delta_tr_check( delta_tr_finish_logs == 1, "the refused finish of a reset lease was not logged as a reset lease" );
    delta_tr_check( delta_tr_abort_logs == 1, "the refused abort of a reset lease was not logged as a reset lease" );
    delta_tr_check( delta_tr_asserts == 0, "a refused finish or abort of a reset lease asserted (it is a runtime condition)" );

    public_step_pair_destroy( &pair );
}
REGISTER_UNIT_TEST( test_delta_client_retired_lease_is_refused_with_capacity_zero_and_logged );

// ---------------------------------------------------------------------------
// stop counts the final ring: one committed 40-byte payload, sealed to
// wire_bytes, is one datagram of wire_bytes plus the IPv4 header in the
// cumulative totals, and an empty final ring counts nothing

// mutation gap: delta sdk/rocketnet_client_transport.cpp:4724 off_by_one 1 (uint64_t bytes = 1)
// mutation gap: delta sdk/rocketnet_client_transport.cpp:4730 arithmetic -= (bytes -= ...)
// mutation gap: delta sdk/rocketnet_client_transport.cpp:4730 arithmetic - (wire_bytes - rocketnet_wire_header_bytes)

static void delta_tr_stop_counts( bool commit, uint64_t * packets, uint64_t * bytes, int * wire_bytes )
{
    public_step_identity_t identity;
    public_step_pair_t pair;
    delta_tr_connect( &pair, &identity );
    rocketnet_client_transport_t * transport = delta_tr_transport( &pair );

    // a send_packets first: nothing committed is left in the Filling ring

    rocketnet_client_transport_send_packets( transport );

    *wire_bytes = 0;
    if ( commit )
    {
        rocketnet_packet_context_t context;
        rocketnet_unit_check( rocketnet_client_transport_start_packet( transport, 100, &context ) != NULL );
        int capacity = 0;
        uint8_t * payload = rocketnet_client_transport_packet_payload( transport, &context, &capacity );
        rocketnet_unit_check( payload != NULL && capacity >= 40 );
        if ( payload )
        {
            memset( payload, 0x6B, 40 );
        }
        *wire_bytes = rocketnet_client_transport_finish_packet( transport, &context, 40 );
        rocketnet_unit_check( *wire_bytes > 40 );
    }

    struct rocketnet_client_transport_stats_t before;
    rocketnet_client_transport_stats( transport, &before );

    rocketnet_client_transport_stop( transport );

    struct rocketnet_client_transport_stats_t after;
    rocketnet_client_transport_stats( transport, &after );

    *packets = after.packets_sent - before.packets_sent;
    *bytes = after.bytes_sent - before.bytes_sent;

    rn_test_client_transport_wait_stopped( transport );
    public_step_pair_destroy( &pair );
}

static void test_delta_client_stop_counts_the_final_ring_exactly()
{
    uint64_t packets = 0, bytes = 0;
    int wire_bytes = 0;

    delta_tr_stop_counts( false, &packets, &bytes, &wire_bytes );
    printf( "delta stop empty: packets %llu bytes %llu\n", (unsigned long long) packets, (unsigned long long) bytes );
    delta_tr_check( packets == 0 && bytes == 0, "stop with an empty final ring counted sent packets or bytes" );

    delta_tr_stop_counts( true, &packets, &bytes, &wire_bytes );
    printf( "delta stop one: packets %llu bytes %llu wire %d\n", (unsigned long long) packets, (unsigned long long) bytes, wire_bytes );
    delta_tr_check( packets == 1, "stop did not count the one committed datagram of the final ring" );
    delta_tr_check( bytes == (uint64_t) wire_bytes + (uint64_t) rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 ),
                    "stop did not count the final ring's datagram as its wire bytes plus the IPv4 header" );
}
REGISTER_UNIT_TEST( test_delta_client_stop_counts_the_final_ring_exactly );

// ---------------------------------------------------------------------------
// the hand-off's bytes reach the per-second window: bandwidth_sent_kbps over
// a window holding only one stats frame is that frame's wire bytes, the
// hand-off's among them

// mutation gap: delta sdk/rocketnet_client_transport.cpp:3131 arithmetic -= (stats_frames[stats_index].bytes_sent -= bytes)

static void test_delta_client_hand_off_bytes_reach_the_bandwidth_window()
{
    public_step_identity_t identity;
    public_step_pair_t pair;
    delta_tr_connect( &pair, &identity );
    rocketnet_client_transport_t * transport = delta_tr_transport( &pair );

    // over a second of quiet: every earlier stats frame leaves the window, and
    // the update at t starts a frame of its own

    const double t = pair.time + 1.05;
    rocketnet_client_transport_update( transport, t );
    rocketnet_client_transport_send_packets( transport );
    printf( "delta bandwidth: state %d\n", rocketnet_client_transport_state( transport ) );

    struct rocketnet_client_transport_stats_t start;
    rocketnet_client_transport_stats( transport, &start );

    // 4 payloads of 1000 bytes: about 33 kbps on their own

    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_packet_context_t context;
        rocketnet_unit_check( rocketnet_client_transport_start_packet( transport, 1000, &context ) != NULL );
        int capacity = 0;
        uint8_t * payload = rocketnet_client_transport_packet_payload( transport, &context, &capacity );
        rocketnet_unit_check( payload != NULL && capacity >= 1000 );
        if ( payload )
        {
            memset( payload, 0x70 + i, 1000 );
        }
        rocketnet_unit_check( rocketnet_client_transport_finish_packet( transport, &context, 1000 ) > 1000 );
    }
    rocketnet_client_transport_send_packets( transport );

    struct rocketnet_client_transport_stats_t sent;
    rocketnet_client_transport_stats( transport, &sent );

    rocketnet_client_transport_update( transport, t );

    struct rocketnet_client_transport_stats_t after;
    rocketnet_client_transport_stats( transport, &after );

    const uint64_t window_bytes = after.bytes_sent - start.bytes_sent;
    const int expected_kbps = (int) ( ( (double) window_bytes * 8.0 ) / 1000.0 );
    printf( "delta bandwidth: start %llu sent %llu after %llu kbps %d expected %d\n", (unsigned long long) start.bytes_sent,
            (unsigned long long) sent.bytes_sent, (unsigned long long) after.bytes_sent, after.bandwidth_sent_kbps, expected_kbps );
    delta_tr_check( sent.bytes_sent - start.bytes_sent > 4000, "the hand-off of four 1000-byte payloads did not count their wire bytes" );
    delta_tr_check( after.bandwidth_sent_kbps == expected_kbps,
                    "bandwidth_sent_kbps over a one-frame window is not that frame's wire bytes: the hand-off's bytes did not reach the stats frame" );

    public_step_pair_destroy( &pair );
}
REGISTER_UNIT_TEST( test_delta_client_hand_off_bytes_reach_the_bandwidth_window );

// ---------------------------------------------------------------------------
// the memory plan seam allocates what the plan counts: the wake stand-in asks
// for the platform wake's bytes, no more

// mutation gap: delta sdk/rocketnet_client_transport.cpp:2310 arithmetic + (wake_allocation_bytes() + rocketnet_allocation_overhead())

struct delta_plan_allocator_t
{
    uint64_t total_bytes;
    int live;
};

static void * delta_plan_alloc( void * context, size_t bytes )
{
    delta_plan_allocator_t * state = (delta_plan_allocator_t *) context;
    if ( state )
    {
        state->total_bytes += bytes;
        state->live++;
    }

    void * base = malloc( bytes + 128 );
    if ( !base )
    {
        return NULL;
    }

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    return (void *) address;
}

static void delta_plan_free( void * context, void * pointer )
{
    delta_plan_allocator_t * state = (delta_plan_allocator_t *) context;
    if ( state )
    {
        state->live--;
    }

    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    free( base );
}

extern void rocketnet_client_transport_set_memory_plan_seam_for_test( bool enable );

static void test_delta_client_memory_plan_seam_allocates_the_plan_exactly()
{
    rocketnet_client_transport_set_memory_plan_seam_for_test( true );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
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

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    config.step_driven = 0;

    const uint64_t plan_bytes = rocketnet_client_transport_required_memory_one_family( &config, "portable" );
    rocketnet_unit_check( plan_bytes > 0 );

    delta_plan_allocator_t state = { 0, 0 };
    rocketnet_unit_check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    rocketnet_unit_check( rocketnet_init_internal( &state, delta_plan_alloc, delta_plan_free, &status ) );

    state.total_bytes = 0;
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_internal( &state, token, &config, 1000.0, &status );
    rocketnet_unit_check( client != NULL );
    const uint64_t allocated = state.total_bytes;
    const uint64_t full_plan = rocketnet_client_transport_required_memory( &config, "portable" );
    printf( "delta plan: one family %llu full %llu allocated %llu overhead %llu\n", (unsigned long long) plan_bytes, (unsigned long long) full_plan,
            (unsigned long long) allocated, (unsigned long long) rocketnet_allocation_overhead() );

    rn_test_client_transport_stop_and_destroy( client );
    rocketnet_unit_check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( state.live == 0 );
    rocketnet_unit_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_client_transport_set_memory_plan_seam_for_test( false );

    delta_tr_check( allocated == plan_bytes, "the memory plan seam's create did not allocate exactly the one-family plan" );
}
REGISTER_UNIT_TEST( test_delta_client_memory_plan_seam_allocates_the_plan_exactly );

#endif // #if ROCKETNET_DEV
